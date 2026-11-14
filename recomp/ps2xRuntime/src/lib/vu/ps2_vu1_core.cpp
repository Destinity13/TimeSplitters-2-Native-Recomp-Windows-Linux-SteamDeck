#include <set>
#include <immintrin.h>
#include <mutex>
#include <string>
#include <memory>
#include <map>
#include <vector>
#include <atomic>
#include "runtime/ps2_vu1.h"
#include "runtime/gs/ps2_gif_arbiter.h"
#include "runtime/gs/gs_frontend.h"
#include "runtime/ps2_memory.h"
#include "ps2_vu1_detail.h"

#include <algorithm>
#include <cfenv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <limits>
#include <ps2_log.h>

namespace
{
    // PS2X_VU1_FAST=1 (default in the scripts): like PCSX2 microVU without the long double recomputation and without product sticky flags
    inline bool vu1FastFmac()
    {
        static const bool fast = [] { const char *e = std::getenv("PS2X_VU1_FAST"); return e && *e && *e != '0'; }();
        return fast;
    }
    constexpr uint8_t laneForComponent(uint32_t component)
    {
        return static_cast<uint8_t>(1u << (3u - component));
    }
}

namespace
{
    // Earliest readyCycle of any pending pipeline entry; lets commitReadyPipelines() skip
    // its full scan on cycles where nothing can commit. Kept outside the class so the
    // public header (included by all generated code) stays unchanged.
    const void *g_vu1CommitOwner = nullptr;
    uint64_t g_vu1NextCommit = 0;

    inline void vu1NoteReady(const void *owner, uint64_t readyCycle)
    {
        if (g_vu1CommitOwner != owner)
        {
            g_vu1CommitOwner = owner;
            g_vu1NextCommit = 0;
        }
        else if (readyCycle < g_vu1NextCommit)
            g_vu1NextCommit = readyCycle;
    }

    // Occupancy masks of the pipeline arrays: commitReadyPipelines() only visits
    // slots that were filled since the last commit. On an owner change
    // all bits are set (conservative, a wrong bit only costs one test).
    struct Vu1Occupancy
    {
        const void *owner = nullptr;
        uint32_t flag = 0, store = 0, vf = 0, vi = 0, acc = 0;
    };
    Vu1Occupancy g_vu1Occ;

    inline Vu1Occupancy &vu1Occ(const void *owner)
    {
        if (g_vu1Occ.owner != owner)
        {
            g_vu1Occ.owner = owner;
            g_vu1Occ.flag = g_vu1Occ.store = g_vu1Occ.vf = g_vu1Occ.vi = g_vu1Occ.acc = 0xFFFFFFFFu;
        }
        return g_vu1Occ;
    }
}

void VU1Interpreter::addVfRead(InstructionUsage &usage, uint8_t reg, uint8_t lanes)
{
    if (lanes == 0u)
        return;
    for (uint32_t index = 0; index < usage.vfReadCount; ++index)
    {
        if (usage.vfRead[index].reg == reg)
        {
            usage.vfRead[index].lanes |= lanes;
            return;
        }
    }
    if (usage.vfReadCount < usage.vfRead.size())
        usage.vfRead[usage.vfReadCount++] = {reg, lanes};
}

void VU1Interpreter::addVfWrite(InstructionUsage &usage, uint8_t reg, uint8_t lanes)
{
    if (reg == 0u || lanes == 0u)
        return;
    if (usage.vfWrite.reg == 0u)
        usage.vfWrite = {reg, lanes};
    else if (usage.vfWrite.reg == reg)
        usage.vfWrite.lanes |= lanes;
}

uint8_t VU1Interpreter::vfReadLanes(const InstructionUsage &usage, uint8_t reg)
{
    for (uint32_t index = 0; index < usage.vfReadCount; ++index)
    {
        if (usage.vfRead[index].reg == reg)
            return usage.vfRead[index].lanes;
    }
    return 0u;
}

VU1Interpreter::VU1Interpreter(Unit unit)
    : m_unit(unit)
{
    reset();
}

void VU1Interpreter::resetScheduler()
{
    m_flagPipeline = {};
    m_recPend = 0;
    m_fdiv = {};
    m_efu = {};
    m_storePipeline = {};
    m_vfWritePipeline = {};
    m_viWritePipeline = {};
    m_accWritePipeline = {};
    // without zeroing the 64 KB packet buffer (ran on every MSCAL: ~35 MB memset per frame)
    m_xgkick.sourceAddress = 0; m_xgkick.totalBytes = 0; m_xgkick.copiedBytes = 0; m_xgkick.currentTagEnd = 0;
    m_xgkick.cycleCredit = 0; m_xgkick.issueCycle = 0; m_xgkick.active = false; m_xgkick.currentTagEop = false;
    m_vfReady = {};
    m_viReady = {};
    m_accReady = {};
    m_vfLatestWrite = {};
    m_viLatestWrite = {};
    m_accLatestWrite = {};
    m_nextWriteSequence = 0;
    m_efuResourceReady = 0;
    m_workingClip = m_state.clip;
    m_viBranchBackupValue = 0;
    m_viBranchBackupReg = 0;
    m_viBranchBackupValid = false;
    m_stopRequested = false;
    m_pendingHaltD = false;
    m_pendingHaltT = false;
}

void VU1Interpreter::reset()
{
    std::memset(&m_state, 0, sizeof(m_state));
    m_state.vf[0][3] = 1.0f;
    m_state.q = 1.0f;
    m_state.r = 0x3F800000u;
    m_cycle = 0;
    resetScheduler();
}

float VU1Interpreter::broadcast(const float *vf, uint8_t bc)
{
    return normalizeOperand(vf[bc & 3u]);
}

float VU1Interpreter::normalizeOperand(float value) const
{
    uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    const uint32_t exponent = (bits >> 23) & 0xFFu;
    if (exponent == 0u)
    {
        bits &= 0x80000000u;
    }
    else if (exponent == 0xFFu)
    {
        bits = (bits & 0x80000000u) | 0x7F7FFFFFu;
    }
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

float VU1Interpreter::normalizeResult(float value, uint32_t &laneFlags) const
{
    uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    const uint32_t sign = bits & 0x80000000u;
    const uint32_t magnitude = bits & 0x7FFFFFFFu;
    const uint32_t exponent = (bits >> 23) & 0xFFu;

    laneFlags = sign != 0u ? 0x2u : 0u;
    if (magnitude == 0u)
    {
        laneFlags |= 0x1u;
    }
    else if (exponent == 0u)
    {
        laneFlags |= 0x5u;
        bits = sign;
    }
    else if (exponent == 0xFFu)
    {
        laneFlags |= 0x8u;
        bits = sign | 0x7F7FFFFFu;
    }

    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

uint32_t VU1Interpreter::microAddressMask() const
{
    return m_unit == Unit::VU1 ? 0x3FFFu : 0x0FFFu;
}

int32_t VU1Interpreter::readBranchVi(uint8_t reg) const
{
    if (reg == 0u)
        return 0;
    if (m_viBranchBackupValid &&
        m_viBranchBackupReg == reg)
    {
        return m_viBranchBackupValue;
    }
    return m_state.vi[reg];
}

void VU1Interpreter::recordViWriteForBranch(uint8_t reg, int32_t oldValue)
{
    if (reg == 0u)
        return;
    m_viBranchBackupValue = oldValue;
    m_viBranchBackupReg = reg;
    m_viBranchBackupValid = true;
}

void VU1Interpreter::applyDest(float *dst, const float *result, uint8_t dest)
{
    if (dest & 0x8u)
        dst[0] = result[0];
    if (dest & 0x4u)
        dst[1] = result[1];
    if (dest & 0x2u)
        dst[2] = result[2];
    if (dest & 0x1u)
        dst[3] = result[3];
}

void VU1Interpreter::applyDestAcc(const float *result, uint8_t dest)
{
    applyDest(m_state.acc, result, dest);
}

void VU1Interpreter::normalizeFmacResult(float *result, uint8_t dest,
                                         uint8_t laneFlags[4])
{
    for (uint32_t component = 0; component < 4u; ++component)
    {
        laneFlags[component] = 0u;
        if ((dest & laneForComponent(component)) == 0u)
            continue;

        // PS2X_VU1_FAST=1: without the exact long double recomputation (over/underflow flags bit-exact), instead
        // SSE float + clamping like PCSX2 microVU. Much faster, result differs by 1 ulp in edge cases.
        static const bool fastFmac = [] { const char *e = std::getenv("PS2X_VU1_FAST"); return e && *e && *e != '0'; }();
        long double exactResult = 0.0L;
        if (!fastFmac && calculateFmacExactResult(component, exactResult))
        {
            laneFlags[component] = normalizeFmacExactResult(result[component], exactResult);
            continue;
        }

        uint32_t flags = 0u;
        result[component] = normalizeResult(result[component], flags);
        laneFlags[component] = static_cast<uint8_t>(flags);
    }
}

bool VU1Interpreter::calculateFmacExactResult(uint32_t component,
                                               long double &result) const
{
    const uint32_t upper = m_currentUpperInstruction;
    const uint8_t op = static_cast<uint8_t>(upper & 0x3Fu);
    const uint8_t special = op >= 0x3Cu
                                ? static_cast<uint8_t>((upper & 3u) | ((upper >> 4) & 0x7Cu))
                                : 0xFFu;
    const uint8_t fs = FS(upper);
    const uint8_t ft = FT(upper);

    const auto operand = [this](float value)
    {
        return static_cast<long double>(normalizeOperand(value));
    };
    const auto vs = [&](uint32_t lane)
    {
        return operand(m_state.vf[fs][lane]);
    };
    const auto vt = [&](uint32_t lane)
    {
        return operand(m_state.vf[ft][lane]);
    };
    const auto acc = [&](uint32_t lane)
    {
        return operand(m_state.acc[lane]);
    };

    const long double q = operand(m_state.q);
    const long double i = operand(m_state.i);

    if (op < 0x3Cu)
    {
        if (op <= 0x03u)
            result = vs(component) + vt(op & 3u);
        else if (op <= 0x07u)
            result = vs(component) - vt(op & 3u);
        else if (op <= 0x0Bu)
            result = acc(component) + vs(component) * vt(op & 3u);
        else if (op <= 0x0Fu)
            result = acc(component) - vs(component) * vt(op & 3u);
        else if (op >= 0x18u && op <= 0x1Bu)
            result = vs(component) * vt(op & 3u);
        else
        {
            switch (op)
            {
            case 0x1Cu:
                result = vs(component) * q;
                break;
            case 0x1Eu:
                result = vs(component) * i;
                break;
            case 0x20u:
                result = vs(component) + q;
                break;
            case 0x21u:
                result = acc(component) + vs(component) * q;
                break;
            case 0x22u:
                result = vs(component) + i;
                break;
            case 0x23u:
                result = acc(component) + vs(component) * i;
                break;
            case 0x24u:
                result = vs(component) - q;
                break;
            case 0x25u:
                result = acc(component) - vs(component) * q;
                break;
            case 0x26u:
                result = vs(component) - i;
                break;
            case 0x27u:
                result = acc(component) - vs(component) * i;
                break;
            case 0x28u:
                result = vs(component) + vt(component);
                break;
            case 0x29u:
                result = acc(component) + vs(component) * vt(component);
                break;
            case 0x2Au:
                result = vs(component) * vt(component);
                break;
            case 0x2Cu:
                result = vs(component) - vt(component);
                break;
            case 0x2Du:
                result = acc(component) - vs(component) * vt(component);
                break;
            case 0x2Eu:
            {
                static constexpr uint8_t left[4] = {1u, 2u, 0u, 3u};
                static constexpr uint8_t right[4] = {2u, 0u, 1u, 3u};
                result = component == 3u
                             ? 0.0L
                             : acc(component) - vs(left[component]) * vt(right[component]);
                break;
            }
            default:
                return false;
            }
        }
        return true;
    }

    if (special <= 0x03u)
        result = vs(component) + vt(special & 3u);
    else if (special <= 0x07u)
        result = vs(component) - vt(special & 3u);
    else if (special <= 0x0Bu)
        result = acc(component) + vs(component) * vt(special & 3u);
    else if (special <= 0x0Fu)
        result = acc(component) - vs(component) * vt(special & 3u);
    else if (special >= 0x18u && special <= 0x1Bu)
        result = vs(component) * vt(special & 3u);
    else
    {
        switch (special)
        {
        case 0x1Cu:
            result = vs(component) * q;
            break;
        case 0x1Eu:
            result = vs(component) * i;
            break;
        case 0x20u:
            result = vs(component) + q;
            break;
        case 0x21u:
            result = acc(component) + vs(component) * q;
            break;
        case 0x22u:
            result = vs(component) + i;
            break;
        case 0x23u:
            result = acc(component) + vs(component) * i;
            break;
        case 0x24u:
            result = vs(component) - q;
            break;
        case 0x25u:
            result = acc(component) - vs(component) * q;
            break;
        case 0x26u:
            result = vs(component) - i;
            break;
        case 0x27u:
            result = acc(component) - vs(component) * i;
            break;
        case 0x28u:
            result = vs(component) + vt(component);
            break;
        case 0x29u:
            result = acc(component) + vs(component) * vt(component);
            break;
        case 0x2Au:
            result = vs(component) * vt(component);
            break;
        case 0x2Cu:
            result = vs(component) - vt(component);
            break;
        case 0x2Du:
            result = acc(component) - vs(component) * vt(component);
            break;
        case 0x2Eu:
        {
            static constexpr uint8_t left[4] = {1u, 2u, 0u, 3u};
            static constexpr uint8_t right[4] = {2u, 0u, 1u, 3u};
            result = component == 3u
                         ? 0.0L
                         : vs(left[component]) * vt(right[component]);
            break;
        }
        default:
            return false;
        }
    }
    return true;
}

uint8_t VU1Interpreter::normalizeFmacExactResult(float &value,
                                                  long double exactResult) const
{
    const bool negative = std::signbit(exactResult);
    const long double magnitude = std::fabs(exactResult);
    const long double maximum = static_cast<long double>(std::numeric_limits<float>::max());
    const long double minimum = static_cast<long double>(std::numeric_limits<float>::min());
    uint8_t flags = negative ? 0x2u : 0u;

    uint32_t bits = negative ? 0x80000000u : 0u;
    if (magnitude == 0.0L)
    {
        flags |= 0x1u;
        std::memcpy(&value, &bits, sizeof(value));
    }
    else if (magnitude > maximum)
    {
        flags |= 0x8u;
        bits |= 0x7F7FFFFFu;
        std::memcpy(&value, &bits, sizeof(value));
    }
    else if (magnitude < minimum)
    {
        flags |= 0x5u;
        std::memcpy(&value, &bits, sizeof(value));
    }

    return flags;
}

uint32_t VU1Interpreter::calculateFmacProductSticky(uint8_t dest) const
{
    uint32_t extraSticky = 0u;
    const uint32_t upper = m_currentUpperInstruction;
    const uint8_t op = static_cast<uint8_t>(upper & 0x3Fu);
    const uint8_t special = op >= 0x3Cu ? static_cast<uint8_t>((upper & 3u) | ((upper >> 4) & 0x7Cu)) : 0xFFu;
    const bool productSum =
        (op >= 0x08u && op <= 0x0Fu) ||
        op == 0x21u || op == 0x23u || op == 0x25u || op == 0x27u ||
        op == 0x29u || op == 0x2Du || op == 0x2Eu ||
        (special >= 0x08u && special <= 0x0Fu) ||
        special == 0x21u || special == 0x23u || special == 0x25u ||
        special == 0x27u || special == 0x29u || special == 0x2Du;
    if (!productSum)
        return 0u;

    const uint8_t fs = FS(upper);
    const uint8_t ft = FT(upper);
    for (uint32_t component = 0; component < 4u; ++component)
    {
        if ((dest & laneForComponent(component)) == 0u)
            continue;
        static constexpr uint8_t crossLeft[4] = {1u, 2u, 0u, 3u};
        static constexpr uint8_t crossRight[4] = {2u, 0u, 1u, 3u};
        const uint8_t leftComponent = op == 0x2Eu ? crossLeft[component] : static_cast<uint8_t>(component);
        const float left = normalizeOperand(m_state.vf[fs][leftComponent]);
        float right = 0.0f;
        if ((op >= 0x08u && op <= 0x0Fu) || (special >= 0x08u && special <= 0x0Fu))
        {
            right = normalizeOperand(m_state.vf[ft][(op >= 0x08u && op <= 0x0Fu ? op : special) & 3u]);
        }
        else if (op == 0x21u || op == 0x25u || special == 0x21u || special == 0x25u)
        {
            right = normalizeOperand(m_state.q);
        }
        else if (op == 0x23u || op == 0x27u || special == 0x23u || special == 0x27u)
        {
            right = normalizeOperand(m_state.i);
        }
        else if (op == 0x2Eu)
        {
            right = normalizeOperand(m_state.vf[ft][crossRight[component]]);
        }
        else
        {
            right = normalizeOperand(m_state.vf[ft][component]);
        }

        float product = left * right;
        const long double exactProduct = static_cast<long double>(left) * static_cast<long double>(right);
        const uint8_t productFlags = normalizeFmacExactResult(product, exactProduct);
        // Product-sum instructions report Z/S/U/O from the add/subtract result
        // as current flags, while every product condition accumulates into the
        // corresponding sticky flag.
        extraSticky |= productFlags & 0xFu;
    }
    return extraSticky;
}

void VU1Interpreter::updateFmacFlags(const uint8_t laneFlags[4], uint8_t dest,
                                     uint32_t extraSticky)
{
    if (dest == 0u || !m_flagsLive)
        return;

    uint32_t mac = 0u;
    uint32_t status = 0u;
    for (uint32_t component = 0; component < 4u; ++component)
    {
        const uint8_t lane = laneForComponent(component);
        if ((dest & lane) == 0u)
            continue;

        const uint32_t flags = laneFlags[component];
        if ((flags & 0x1u) != 0u)
            mac |= lane;
        if ((flags & 0x2u) != 0u)
            mac |= static_cast<uint32_t>(lane) << 4;
        if ((flags & 0x4u) != 0u)
            mac |= static_cast<uint32_t>(lane) << 8;
        if ((flags & 0x8u) != 0u)
            mac |= static_cast<uint32_t>(lane) << 12;
        status |= flags;
    }

    FlagPipelineEntry *entry = nullptr;
    for (FlagPipelineEntry &candidate : m_flagPipeline)
    {
        if (!candidate.valid)
        {
            entry = &candidate;
            break;
        }
    }
    if (!entry)
    {
        reportReservedInstruction(true, 0xFFFFFFFFu);
        return;
    }

    *entry = {};
    entry->valid = true;
    m_occ.flag |= 1u << static_cast<uint32_t>(entry - m_flagPipeline.data());
    entry->issueCycle = m_cycle;
    entry->readyCycle = m_cycle + kFmacLatency;
    noteReady(entry->readyCycle);
    entry->mac = mac;
    entry->status = status;
    entry->extraSticky = extraSticky;
    entry->writesMac = true;
    entry->writesStatus = true;
}

void VU1Interpreter::applyFmacDest(float *dst, float *result, uint8_t dest)
{
    uint8_t laneFlags[4]{};
    normalizeFmacResult(result, dest, laneFlags);
    if (m_flagsLive) // otherwise updateFmacFlags discards everything anyway: skip the sticky recomputation
        updateFmacFlags(laneFlags, dest, vu1FastFmac() ? 0u : calculateFmacProductSticky(dest)); // FAST: like microVU without product sticky
    applyDest(dst, result, dest);
}

void VU1Interpreter::applyFmacDestAcc(float *result, uint8_t dest)
{
    uint8_t laneFlags[4]{};
    normalizeFmacResult(result, dest, laneFlags);
    if (m_flagsLive) // otherwise updateFmacFlags discards everything anyway: skip the sticky recomputation
        updateFmacFlags(laneFlags, dest, vu1FastFmac() ? 0u : calculateFmacProductSticky(dest)); // FAST: like microVU without product sticky
    applyDestAcc(result, dest);
}

void VU1Interpreter::queueFsset(uint16_t immediate)
{
    for (FlagPipelineEntry &entry : m_flagPipeline)
    {
        if (entry.valid && entry.issueCycle == m_cycle)
            entry.writesStatus = false;
    }

    for (FlagPipelineEntry &entry : m_flagPipeline)
    {
        if (!entry.valid)
        {
            entry = {};
            entry.valid = true;
            m_occ.flag |= 1u << static_cast<uint32_t>(&entry - m_flagPipeline.data());
            entry.issueCycle = m_cycle;
            entry.readyCycle = m_cycle + kFmacLatency;
            noteReady(entry.readyCycle);
            entry.status = static_cast<uint32_t>(immediate) & 0xFC0u;
            entry.writesSticky = true;
            return;
        }
    }
    reportReservedInstruction(false, 0xFFFFFFFEu);
}

void VU1Interpreter::queueClip(uint32_t clip)
{
    m_workingClip = ((m_workingClip << 6) | (clip & 0x3Fu)) & 0xFFFFFFu;
    for (FlagPipelineEntry &entry : m_flagPipeline)
    {
        if (!entry.valid)
        {
            entry = {};
            entry.valid = true;
            m_occ.flag |= 1u << static_cast<uint32_t>(&entry - m_flagPipeline.data());
            entry.issueCycle = m_cycle;
            entry.readyCycle = m_cycle + kFmacLatency;
            noteReady(entry.readyCycle);
            entry.clip = m_workingClip;
            entry.writesClip = true;
            return;
        }
    }
    reportReservedInstruction(true, 0xFFFFFFFDu);
}

void VU1Interpreter::queueFcset(uint32_t clip)
{
    m_workingClip = clip & 0xFFFFFFu;
    for (FlagPipelineEntry &entry : m_flagPipeline)
    {
        if (entry.valid && entry.issueCycle == m_cycle)
            entry.writesClip = false;
    }
    for (FlagPipelineEntry &entry : m_flagPipeline)
    {
        if (!entry.valid)
        {
            entry = {};
            entry.valid = true;
            m_occ.flag |= 1u << static_cast<uint32_t>(&entry - m_flagPipeline.data());
            entry.issueCycle = m_cycle;
            entry.readyCycle = m_cycle + kFmacLatency;
            noteReady(entry.readyCycle);
            entry.clip = m_workingClip;
            entry.writesClip = true;
            return;
        }
    }
    reportReservedInstruction(false, 0xFFFFFFFAu);
}

void VU1Interpreter::queueQ(float value, uint32_t latency, uint32_t statusDi)
{
    uint32_t ignoredFlags = 0u;
    value = normalizeResult(value, ignoredFlags);
    m_fdiv.valid = true;
    m_fdiv.readyCycle = m_cycle + latency;
    noteReady(m_fdiv.readyCycle);
    m_fdiv.value = value;
    m_fdiv.statusDi = statusDi & 0x30u;
}

void VU1Interpreter::queueP(float value, uint32_t latency)
{
    uint32_t ignoredFlags = 0u;
    value = normalizeResult(value, ignoredFlags);
    for (ScalarPipelineEntry &entry : m_efu)
    {
        if (!entry.valid)
        {
            entry.valid = true;
            entry.readyCycle = m_cycle + latency;
            noteReady(entry.readyCycle);
            entry.value = value;
            // EFU throughput is one cycle shorter than result visibility.
            m_efuResourceReady = m_cycle + (latency > 0u ? latency - 1u : 0u);
            return;
        }
    }
    reportReservedInstruction(false, 0xFFFFFFF9u);
}

void VU1Interpreter::queueStore(uint32_t address, const uint32_t words[4], uint8_t laneMask)
{
    for (PendingStore &store : m_storePipeline)
    {
        if (!store.valid)
        {
            store.valid = true;
            m_occ.store |= 1u << static_cast<uint32_t>(&store - m_storePipeline.data());
            store.readyCycle = m_cycle + 1u;
            noteReady(store.readyCycle);
            store.address = address;
            store.laneMask = laneMask;
            std::copy(words, words + 4, store.words.begin());
            return;
        }
    }
    reportReservedInstruction(false, 0xFFFFFFFCu);
}

void VU1Interpreter::queueVfWrite(uint8_t reg, uint8_t laneMask,
                                  const float value[4], uint32_t latency)
{
    if (reg == 0u || laneMask == 0u)
        return;
    for (PendingVfWrite &write : m_vfWritePipeline)
    {
        if (!write.valid)
        {
            write = {};
            write.valid = true;
            m_occ.vf |= 1u << static_cast<uint32_t>(&write - m_vfWritePipeline.data());
            write.readyCycle = m_cycle + latency;
            noteReady(write.readyCycle);
            write.sequence = ++m_nextWriteSequence;
            write.reg = reg;
            write.laneMask = laneMask;
            std::copy(value, value + 4, write.value.begin());
            for (uint32_t component = 0; component < 4u; ++component)
            {
                if ((laneMask & laneForComponent(component)) != 0u)
                    m_vfLatestWrite[reg][component] = write.sequence;
            }
            return;
        }
    }
    reportReservedInstruction(false, 0xFFFFFFF7u);
}

void VU1Interpreter::queueViWrite(uint8_t reg, int32_t value, uint32_t latency)
{
    if (reg == 0u)
        return;
    for (PendingViWrite &write : m_viWritePipeline)
    {
        if (!write.valid)
        {
            write = {};
            write.valid = true;
            m_occ.vi |= 1u << static_cast<uint32_t>(&write - m_viWritePipeline.data());
            write.readyCycle = m_cycle + latency;
            noteReady(write.readyCycle);
            write.sequence = ++m_nextWriteSequence;
            write.reg = reg;
            write.value = value;
            m_viLatestWrite[reg] = write.sequence;
            return;
        }
    }
    reportReservedInstruction(false, 0xFFFFFFF6u);
}

void VU1Interpreter::queueAccWrite(uint8_t laneMask, const float value[4], uint32_t latency)
{
    if (laneMask == 0u)
        return;
    for (PendingAccWrite &write : m_accWritePipeline)
    {
        if (!write.valid)
        {
            write = {};
            write.valid = true;
            m_occ.acc |= 1u << static_cast<uint32_t>(&write - m_accWritePipeline.data());
            write.readyCycle = m_cycle + latency;
            noteReady(write.readyCycle);
            write.sequence = ++m_nextWriteSequence;
            write.laneMask = laneMask;
            std::copy(value, value + 4, write.value.begin());
            for (uint32_t component = 0; component < 4u; ++component)
            {
                if ((laneMask & laneForComponent(component)) != 0u)
                    m_accLatestWrite[component] = write.sequence;
            }
            return;
        }
    }
    reportReservedInstruction(true, 0xFFFFFFF5u);
}

void VU1Interpreter::commitReadyPipelines()
{
    if (m_cycle < m_nextCommit)
        return;
    uint64_t pendingMin = UINT64_MAX;
    Occupancy &occ = m_occ;
    for (uint32_t bits = occ.flag & ((1u << m_flagPipeline.size()) - 1u); bits != 0u; bits &= bits - 1u)
    {
        const uint32_t slot = static_cast<uint32_t>(__builtin_ctz(bits));
        FlagPipelineEntry &entry = m_flagPipeline[slot];
        if (!entry.valid)
        {
            occ.flag &= ~(1u << slot);
            continue;
        }
        if (entry.readyCycle > m_cycle)
        {
            pendingMin = std::min(pendingMin, entry.readyCycle);
            continue;
        }

        if (entry.writesMac)
            m_state.mac = entry.mac;
        if (entry.writesStatus)
        {
            const uint32_t current = entry.status & 0xFu;
            m_state.status = (m_state.status & 0xFF0u) | current | ((current | entry.extraSticky) << 6);
        }
        if (entry.writesSticky)
        {
            m_state.status = (m_state.status & 0x03Fu) | (entry.status & 0xFC0u);
        }
        if (entry.writesClip)
            m_state.clip = entry.clip;
        entry = {};
        occ.flag &= ~(1u << slot);
    }

    if (m_fdiv.valid && m_fdiv.readyCycle > m_cycle)
        pendingMin = std::min(pendingMin, m_fdiv.readyCycle);
    if (m_fdiv.valid && m_fdiv.readyCycle <= m_cycle)
    {
        m_state.q = m_fdiv.value;
        const uint32_t currentDi = m_fdiv.statusDi & 0x30u;
        m_state.status = (m_state.status & 0xFCFu) | currentDi | (currentDi << 6);
        m_fdiv = {};
    }

    for (ScalarPipelineEntry &entry : m_efu)
    {
        if (entry.valid && entry.readyCycle <= m_cycle)
        {
            m_state.p = entry.value;
            entry = {};
        }
        else if (entry.valid)
            pendingMin = std::min(pendingMin, entry.readyCycle);
    }

    for (uint32_t bits = occ.store & ((1u << m_storePipeline.size()) - 1u); bits != 0u; bits &= bits - 1u)
    {
        const uint32_t slot = static_cast<uint32_t>(__builtin_ctz(bits));
        PendingStore &store = m_storePipeline[slot];
        if (!store.valid)
        {
            occ.store &= ~(1u << slot);
            continue;
        }
        if (store.readyCycle > m_cycle)
        {
            pendingMin = std::min(pendingMin, store.readyCycle);
            continue;
        }
        if (m_activeVuData && store.address + 16u <= m_activeVuDataSize)
        {
            uint32_t oldWords[4]{};
            std::memcpy(oldWords, m_activeVuData + store.address, sizeof(oldWords));
            for (uint32_t component = 0; component < 4u; ++component)
            {
                if ((store.laneMask & laneForComponent(component)) != 0u)
                    oldWords[component] = store.words[component];
            }
            std::memcpy(m_activeVuData + store.address, oldWords, sizeof(oldWords));
        }
        store = {};
        occ.store &= ~(1u << slot);
    }

    for (uint32_t bits = occ.vf & ((1u << m_vfWritePipeline.size()) - 1u); bits != 0u; bits &= bits - 1u)
    {
        const uint32_t slot = static_cast<uint32_t>(__builtin_ctz(bits));
        PendingVfWrite &write = m_vfWritePipeline[slot];
        if (!write.valid)
        {
            occ.vf &= ~(1u << slot);
            continue;
        }
        if (write.readyCycle > m_cycle)
        {
            pendingMin = std::min(pendingMin, write.readyCycle);
            continue;
        }
        for (uint32_t component = 0; component < 4u; ++component)
        {
            if ((write.laneMask & laneForComponent(component)) != 0u &&
                m_vfLatestWrite[write.reg][component] == write.sequence)
            {
                m_state.vf[write.reg][component] = write.value[component];
            }
        }
        write = {};
        occ.vf &= ~(1u << slot);
    }

    for (uint32_t bits = occ.vi & ((1u << m_viWritePipeline.size()) - 1u); bits != 0u; bits &= bits - 1u)
    {
        const uint32_t slot = static_cast<uint32_t>(__builtin_ctz(bits));
        PendingViWrite &write = m_viWritePipeline[slot];
        if (!write.valid)
        {
            occ.vi &= ~(1u << slot);
            continue;
        }
        if (write.readyCycle > m_cycle)
        {
            pendingMin = std::min(pendingMin, write.readyCycle);
            continue;
        }
        if (m_viLatestWrite[write.reg] == write.sequence)
            m_state.vi[write.reg] = static_cast<int16_t>(write.value);
        write = {};
        occ.vi &= ~(1u << slot);
    }

    for (uint32_t bits = occ.acc & ((1u << m_accWritePipeline.size()) - 1u); bits != 0u; bits &= bits - 1u)
    {
        const uint32_t slot = static_cast<uint32_t>(__builtin_ctz(bits));
        PendingAccWrite &write = m_accWritePipeline[slot];
        if (!write.valid)
        {
            occ.acc &= ~(1u << slot);
            continue;
        }
        if (write.readyCycle > m_cycle)
        {
            pendingMin = std::min(pendingMin, write.readyCycle);
            continue;
        }
        for (uint32_t component = 0; component < 4u; ++component)
        {
            if ((write.laneMask & laneForComponent(component)) != 0u &&
                m_accLatestWrite[component] == write.sequence)
            {
                m_state.acc[component] = write.value[component];
            }
        }
        write = {};
        occ.acc &= ~(1u << slot);
    }
    m_nextCommit = pendingMin;
}

// ps2x (TS2): state at the last XGKICK (for diagnosing oversized packets)
namespace { thread_local struct { uint32_t pc, qw, top, itop; uint16_t vi[16]; } t_lastKick{}; }
void VU1Interpreter::progressXgkick()
{
    if (!m_xgkick.active || !m_activeVuData || m_activeVuDataSize == 0u)
        return;

    ++m_xgkick.cycleCredit;
    while (m_xgkick.active && m_xgkick.cycleCredit >= 2u)
    {
        m_xgkick.cycleCredit -= 2u;
        if (m_xgkick.copiedBytes > XgkickPipeline::kBufferSize - 16u)
        {
            reportReservedInstruction(false, 0xFFFFFFFBu);
            m_xgkick.active = false;
            return;
        }

        const uint32_t qwordOffset = m_xgkick.copiedBytes;
        const uint32_t src0 = (m_xgkick.sourceAddress + m_xgkick.copiedBytes) % m_activeVuDataSize;
        if (src0 + 16u <= m_activeVuDataSize) // common case without wrap-around: one block instead of 16 single bytes
            std::memcpy(&m_xgkick.packet[m_xgkick.copiedBytes], m_activeVuData + src0, 16u);
        else
            for (uint32_t i = 0; i < 16u; ++i)
            {
                const uint32_t source = (m_xgkick.sourceAddress + m_xgkick.copiedBytes + i) % m_activeVuDataSize;
                m_xgkick.packet[m_xgkick.copiedBytes + i] = m_activeVuData[source];
            }
        m_xgkick.copiedBytes += 16u;

        if (m_xgkick.currentTagEnd == 0u)
        {
            uint64_t tagLo = 0;
            std::memcpy(&tagLo, m_xgkick.packet.data() + qwordOffset, sizeof(tagLo));
            const uint32_t nloop = static_cast<uint32_t>(tagLo & 0x7FFFu);
            const uint32_t format = static_cast<uint32_t>((tagLo >> 58) & 0x3u);
            uint32_t nreg = static_cast<uint32_t>((tagLo >> 60) & 0xFu);
            if (nreg == 0u)
                nreg = 16u;

            uint64_t tagBytes = 16u;
            if (format == 0u)
                tagBytes += static_cast<uint64_t>(nloop) * nreg * 16u;
            else if (format == 1u)
                tagBytes += ((static_cast<uint64_t>(nloop) * nreg + 1u) & ~1ull) * 8u;
            else if (format == 2u || format == 3u) // FLG=3 ("disabled" in the GS manual) behaves exactly like IMAGE: NLOOP qwords of image data.
                tagBytes += static_cast<uint64_t>(nloop) * 16u;
            else
            {
                reportReservedInstruction(false, 0xFFFFFFF8u);
                m_xgkick.active = false;
                return;
            }

            if (tagBytes > XgkickPipeline::kBufferSize - qwordOffset)
            {
                {   // ps2x (TS2): diagnosis of oversized XGKICK packets
                    static int n = 0;
                    if (n++ < 12)
                    {
                        uint64_t tagHi = 0; std::memcpy(&tagHi, m_xgkick.packet.data() + qwordOffset + 8, 8);
                        std::fprintf(stderr, "[XGKICK Ausloeser] pc=0x%X qw=0x%X top=0x%X itop=0x%X vi=", t_lastKick.pc, t_lastKick.qw, t_lastKick.top, t_lastKick.itop);
                        for (int i = 0; i < 16; ++i) std::fprintf(stderr, "%X ", t_lastKick.vi[i]);
                        std::fprintf(stderr, "\n");
                        std::fprintf(stderr, "[XGKICK too big] src=0x%X off=0x%X tag=%016llX:%016llX nloop=%u flg=%u nreg=%u bytes=%llu\n",
                                     m_xgkick.sourceAddress, qwordOffset, (unsigned long long)tagHi, (unsigned long long)tagLo, nloop, format, nreg,
                                     (unsigned long long)tagBytes);
                        // Tag-Kette ab Paketanfang erneut ablaufen (Offset, NLOOP, EOP, FLG, NREG, PRIM)
                        if (n <= 3)
                        {
                            uint32_t o = 0;
                            for (int t = 0; t < 48 && o < 0x10000u; ++t)
                            {
                                const uint32_t a = (m_xgkick.sourceAddress + o) % m_activeVuDataSize;
                                uint64_t lo = 0, hi = 0; std::memcpy(&lo, m_activeVuData + a, 8); std::memcpy(&hi, m_activeVuData + a + 8, 8);
                                const uint32_t nl = lo & 0x7FFFu, eop = (lo >> 15) & 1u, pre = (lo >> 46) & 1u, prim = (lo >> 47) & 0x7FFu;
                                const uint32_t fl = (lo >> 58) & 3u; uint32_t nr = (lo >> 60) & 0xFu; if (!nr) nr = 16;
                                uint64_t b = 16;
                                if (fl == 0) b += (uint64_t)nl * nr * 16; else if (fl == 1) b += (((uint64_t)nl * nr + 1) & ~1ull) * 8; else b += (uint64_t)nl * 16;
                                std::fprintf(stderr, "    tag%02d off=0x%04X vu=0x%04X nloop=%u eop=%u flg=%u nreg=%u pre=%u prim=0x%X regs=%016llX\n",
                                             t, o, a, nl, eop, fl, nr, pre, prim, (unsigned long long)hi);
                                if (eop) break;
                                o += static_cast<uint32_t>(b);
                            }
                        }
                        for (uint32_t q = 0; q < 4; ++q)
                        {
                            const uint32_t a = (m_xgkick.sourceAddress + q * 16u) % m_activeVuDataSize;
                            uint64_t lo = 0, hi = 0; std::memcpy(&lo, m_activeVuData + a, 8); std::memcpy(&hi, m_activeVuData + a + 8, 8);
                            std::fprintf(stderr, "    vu[0x%04X] %016llX %016llX\n", a, (unsigned long long)hi, (unsigned long long)lo);
                        }
                    }
                }
                reportReservedInstruction(false, 0xFFFFFFFBu);
                m_xgkick.active = false;
                return;
            }
            m_xgkick.currentTagEnd = qwordOffset + static_cast<uint32_t>(tagBytes);
            m_xgkick.currentTagEop = ((tagLo >> 15) & 1u) != 0u;
            if (m_xgkick.currentTagEop)
                m_xgkick.totalBytes = m_xgkick.currentTagEnd;
        }

        if (m_xgkick.copiedBytes >= m_xgkick.currentTagEnd)
        {
            if (m_xgkick.currentTagEop)
                finishXgkick();
            else
            {
                // The next transferred qword is another GIFtag.
                m_xgkick.currentTagEnd = 0u;
                m_xgkick.currentTagEop = false;
            }
        }
    }
}

void VU1Interpreter::progressXgkickBulk()
{
    while (m_xgkick.active && m_activeVuData && m_activeVuDataSize != 0u)
    {
        if (m_xgkick.currentTagEnd == 0u || m_xgkick.copiedBytes >= m_xgkick.currentTagEnd)
        {
            // exactly one quadword (GIFtag or terminator) with the previous logic
            m_xgkick.cycleCredit = 1u;
            progressXgkick();
            continue;
        }
        uint32_t remaining = m_xgkick.currentTagEnd - m_xgkick.copiedBytes; // multiple of 16, fits into the buffer
        while (remaining != 0u)
        {
            const uint32_t src0 = (m_xgkick.sourceAddress + m_xgkick.copiedBytes) % m_activeVuDataSize;
            uint32_t chunk = m_activeVuDataSize - src0; // up to the wrap-around in VU data memory
            if (chunk > remaining)
                chunk = remaining;
            std::memcpy(&m_xgkick.packet[m_xgkick.copiedBytes], m_activeVuData + src0, chunk);
            m_xgkick.copiedBytes += chunk;
            remaining -= chunk;
        }
        if (m_xgkick.currentTagEop)
            finishXgkick();
        else
        {
            m_xgkick.currentTagEnd = 0u;
            m_xgkick.currentTagEop = false;
        }
    }
}

void VU1Interpreter::finishXgkick()
{
    if (!m_xgkick.active)
        return;

    if (m_gifCapture)
        m_gifCapture->insert(m_gifCapture->end(), m_xgkick.packet.data(), m_xgkick.packet.data() + m_xgkick.totalBytes);
    if (!m_gifSubmit)
    {
        m_xgkick.active = false;
        return;
    }
    if (m_activeMemory)
        m_activeMemory->submitGifPacket(GifPathId::Path1, m_xgkick.packet.data(), m_xgkick.totalBytes);
    else if (m_activeGs)
        m_activeGs->processGIFPacket(m_xgkick.packet.data(), m_xgkick.totalBytes);
    m_xgkick.active = false;
}

extern std::atomic<bool> g_ps2xStateHist;
std::mutex g_ps2xVu1ProgMutex;
std::map<uint32_t, uint64_t> g_ps2xVu1ProgCount;
// Diagnostics: when armed (by the game override), dump VU1 code/data/registers at the next few XGKICKs.
std::atomic<int> g_ps2xVu1DumpArm{0};
static const uint8_t *g_vu1LastCode = nullptr;
static uint32_t g_vu1LastCodeSize = 0;

void VU1Interpreter::startXgkick(uint32_t qwordAddress)
{
    t_lastKick.pc = m_state.pc; t_lastKick.qw = qwordAddress; t_lastKick.top = m_state.top; t_lastKick.itop = m_state.itop;
    for (int i = 0; i < 16; ++i) t_lastKick.vi[i] = static_cast<uint16_t>(m_state.vi[i]);
    if (m_unit != Unit::VU1 || !m_activeVuData || m_activeVuDataSize < 16u)
        return;
    {
        // TS2_KICKDUMP=n (diagnosis): arm the VU1 dump for the first n kicks whose first GIF tag draws FST triangle
        // strips with a vertex far outside the screen (the stretched impact smoke quads)
        static int rest = [] { const char *e = std::getenv("TS2_KICKDUMP"); return e ? std::atoi(e) : 0; }();
        if (rest > 0)
        {
            uint32_t o = (qwordAddress * 16u) % m_activeVuDataSize; bool weit = false; uint32_t prim = 0;
            for (int tags = 0; tags < 12 && !weit && o + 16u <= m_activeVuDataSize; ++tags)
            {
                uint64_t lo, hi; std::memcpy(&lo, m_activeVuData + o, 8); std::memcpy(&hi, m_activeVuData + o + 8, 8); o += 16u;
                const uint32_t nloop = lo & 0x7FFFu, flg = (lo >> 58) & 3u; uint32_t nreg = (lo >> 60) & 0xFu; if (!nreg) nreg = 16;
                if ((lo >> 46) & 1u) prim = (lo >> 47) & 0x7FFu;
                if (flg == 0u)
                {
                    for (uint32_t i = 0; i < nloop && !weit; ++i)
                        for (uint32_t r = 0; r < nreg; ++r, o += 16u)
                        {
                            if (o + 16u > m_activeVuDataSize) { i = nloop; break; }
                            const uint32_t reg = (hi >> (4u * r)) & 0xFu;
                            if (reg == 0xEu && m_activeVuData[o + 8] == 0x00) { uint64_t d; std::memcpy(&d, m_activeVuData + o, 8); prim = d & 0x7FFu; }
                            if ((reg == 4u || reg == 5u || reg == 0xCu || reg == 0xDu) && (prim & 0x157u) == 0x154u)
                            { uint16_t y; std::memcpy(&y, m_activeVuData + o + 4, 2); if (y / 16u > 3000u || y / 16u < 900u) weit = true; }
                        }
                }
                else if (flg == 1u) o += ((nloop * nreg + 1u) / 2u) * 16u;
                else o += nloop * 16u;
                if (lo & 0x8000u) break;
            }
            if (weit) { --rest; g_ps2xVu1DumpArm.store(1); }
        }
    }
    if (g_ps2xVu1DumpArm.load(std::memory_order_relaxed) > 0)
    {
        static int dumpIndex = 0;
        const int left = g_ps2xVu1DumpArm.fetch_sub(1);
        if (left > 0)
        {
            char path[96];
            std::snprintf(path, sizeof(path), "/tmp/vu1dump_%02d.bin", dumpIndex++);
            if (FILE *f = std::fopen(path, "wb"))
            {
                // layout: data(16K) code(16K) vf(32*16) vi(16*4) kickAddr pc
                std::fwrite(m_activeVuData, 1, std::min<uint32_t>(m_activeVuDataSize, 16384u), f);
                std::vector<uint8_t> code(16384u, 0u);
                if (g_vu1LastCode)
                    std::memcpy(code.data(), g_vu1LastCode, std::min<uint32_t>(g_vu1LastCodeSize, 16384u));
                std::fwrite(code.data(), 1, code.size(), f);
                std::fwrite(m_state.vf, 1, sizeof(m_state.vf), f);
                std::fwrite(m_state.vi, 1, sizeof(m_state.vi), f);
                const uint32_t extra[2] = {qwordAddress, m_state.pc};
                std::fwrite(extra, 1, sizeof(extra), f);
                std::fclose(f);
                std::fprintf(stderr, "[VU1 dump] %s kick=0x%X pc=0x%X\n", path, qwordAddress, m_state.pc);
            }
        }
    }

    const uint32_t sourceAddress = (qwordAddress * 16u) % m_activeVuDataSize;
    // Only reset the bookkeeping fields: "m_xgkick = {}" zeroed the 64 KB buffer on every XGKICK
    // (the buffer is written before it is read anyway).
    m_xgkick.sourceAddress = 0; m_xgkick.totalBytes = 0; m_xgkick.copiedBytes = 0; m_xgkick.currentTagEnd = 0;
    m_xgkick.cycleCredit = 0; m_xgkick.issueCycle = 0; m_xgkick.active = false; m_xgkick.currentTagEop = false;
    m_xgkick.active = true;
    m_xgkick.sourceAddress = sourceAddress;
    m_xgkick.cycleCredit = 1u; // XGKICK's issue cycle counts toward PATH1.
    m_xgkick.issueCycle = m_cycle;
}

void VU1Interpreter::advanceOneCycle()
{
    ++m_cycle;
    m_state.cycles = m_cycle;
    // LSU commits become visible at the cycle boundary before PATH1 consumes
    // its next qword from VU memory.
    commitReadyPipelines();
    progressXgkick();
}

void VU1Interpreter::advanceTo(uint64_t targetCycle)
{
    while (m_cycle < targetCycle)
    {
        // Without an XGKICK transfer in progress nothing happens until the next due time (m_nextCommit):
        // commitReadyPipelines() and progressXgkick() return immediately. Jump right before it (bit-identical).
        if (!m_xgkick.active)
        {
            const uint64_t stop = m_nextCommit < targetCycle ? m_nextCommit : targetCycle;
            if (stop > m_cycle + 1u)
                m_cycle = stop - 1u;
        }
        advanceOneCycle();
    }
}

bool VU1Interpreter::pipelinesPending() const
{
    if (m_fdiv.valid || m_xgkick.active)
        return true;
    for (const ScalarPipelineEntry &entry : m_efu)
        if (entry.valid)
            return true;
    for (const FlagPipelineEntry &entry : m_flagPipeline)
        if (entry.valid)
            return true;
    for (const PendingStore &store : m_storePipeline)
        if (store.valid)
            return true;
    for (const PendingVfWrite &write : m_vfWritePipeline)
        if (write.valid)
            return true;
    for (const PendingViWrite &write : m_viWritePipeline)
        if (write.valid)
            return true;
    for (const PendingAccWrite &write : m_accWritePipeline)
        if (write.valid)
            return true;
    return false;
}

void VU1Interpreter::flushPipelines()
{
    while (pipelinesPending())
        advanceOneCycle();
}

uint64_t VU1Interpreter::calculatePairReadyCycle(const DecodedInstructionPair &decoded) const
{
    uint64_t ready = m_cycle;
    const InstructionUsage *usages[2] = {
        &decoded.upperUsage,
        &decoded.lowerUsage};
    for (const InstructionUsage *usage : usages)
    {
        if (!usage)
            continue;
        for (uint32_t index = 0; index < usage->vfReadCount; ++index)
        {
            const VfAccess &access = usage->vfRead[index];
            for (uint32_t component = 0; component < 4u; ++component)
            {
                if ((access.lanes & laneForComponent(component)) != 0u)
                    ready = std::max(ready, m_vfReady[access.reg][component]);
            }
        }
        for (uint32_t bits = static_cast<uint32_t>(usage->viRead) & 0xFFFEu; bits != 0u; bits &= bits - 1u)
        {
            const uint32_t reg = static_cast<uint32_t>(__builtin_ctz(bits));
            if (reg < m_viReady.size())
                ready = std::max(ready, m_viReady[reg]);
        }
        for (uint32_t component = 0; component < 4u; ++component)
        {
            if ((usage->accRead & laneForComponent(component)) != 0u)
                ready = std::max(ready, m_accReady[component]);
        }
    }

    if (decoded.lowerUsage.pipeline == PipelineFdiv && m_fdiv.valid)
        ready = std::max(ready, m_fdiv.readyCycle);
    if (decoded.lowerUsage.pipeline == PipelineEfu)
        ready = std::max(ready, m_efuResourceReady);
    if (decoded.lowerUsage.waitQ && m_fdiv.valid)
        ready = std::max(ready, m_fdiv.readyCycle);
    if (decoded.lowerUsage.waitP)
    {
        for (const ScalarPipelineEntry &entry : m_efu)
            if (entry.valid)
                ready = std::max(ready, entry.readyCycle);
    }
    if (decoded.lowerUsage.pipeline == PipelineXgkick && m_xgkick.active)
        ready = std::max(ready, m_cycle + 1u);
    return ready;
}

void VU1Interpreter::markPairWrites(const DecodedInstructionPair &decoded)
{
    const VfAccess lowerWrite = decoded.lowerUsage.vfWrite;
    if (lowerWrite.reg != 0u &&
        decoded.suppressedLowerVf != lowerWrite.reg)
    {
        const uint32_t latency = decoded.lowerUsage.vfLatency != 0u
                                     ? decoded.lowerUsage.vfLatency
                                     : decoded.lowerUsage.latency;
        for (uint32_t component = 0; component < 4u; ++component)
        {
            if ((lowerWrite.lanes & laneForComponent(component)) != 0u)
                m_vfReady[lowerWrite.reg][component] = m_cycle + latency;
        }
    }

    const VfAccess upperWrite = decoded.upperUsage.vfWrite;
    if (upperWrite.reg != 0u)
    {
        const uint32_t latency = decoded.upperUsage.vfLatency != 0u
                                     ? decoded.upperUsage.vfLatency
                                     : decoded.upperUsage.latency;
        for (uint32_t component = 0; component < 4u; ++component)
        {
            if ((upperWrite.lanes & laneForComponent(component)) != 0u)
                m_vfReady[upperWrite.reg][component] = m_cycle + latency;
        }
    }

    for (uint32_t reg = 1; reg < m_viReady.size(); ++reg)
    {
        if ((decoded.lowerUsage.viWrite & (1u << reg)) != 0u)
            m_viReady[reg] = m_cycle + (decoded.lowerUsage.viLatency != 0u ? decoded.lowerUsage.viLatency : decoded.lowerUsage.latency);
    }
    for (uint32_t component = 0; component < 4u; ++component)
    {
        if ((decoded.upperUsage.accWrite & laneForComponent(component)) != 0u)
            m_accReady[component] = m_cycle + kAccForwardLatency;
    }
}

VU1Interpreter::InstructionUsage VU1Interpreter::decodeUpperUsage(uint32_t upper) const
{
    InstructionUsage usage;
    usage.pipeline = PipelineFmac;
    usage.latency = kFmacLatency;

    const uint8_t op = static_cast<uint8_t>(upper & 0x3Fu);
    const uint8_t dest = DEST(upper);
    const uint8_t fs = FS(upper);
    const uint8_t ft = FT(upper);
    const uint8_t fd = FD(upper);

    if (op <= 0x2Fu)
    {
        addVfRead(usage, fs, dest);
        addVfWrite(usage, fd, dest);
        if (op <= 0x1Bu)
            addVfRead(usage, ft, laneForComponent(op & 3u));
        else if (op >= 0x28u)
            addVfRead(usage, ft, op == 0x2Eu ? 0xEu : dest);
        if (op == 0x08u || op == 0x09u || op == 0x0Au || op == 0x0Bu ||
            op == 0x0Cu || op == 0x0Du || op == 0x0Eu || op == 0x0Fu ||
            op == 0x21u || op == 0x23u || op == 0x25u || op == 0x27u ||
            op == 0x29u || op == 0x2Du || op == 0x2Eu)
        {
            usage.accRead = dest;
        }
        return usage;
    }

    if (op >= 0x3Cu)
    {
        const uint8_t special = static_cast<uint8_t>((upper & 3u) | ((upper >> 4) & 0x7Cu));
        const bool writesAcc =
            special <= 0x0Fu ||
            (special >= 0x18u && special <= 0x1Cu) ||
            special == 0x1Eu ||
            (special >= 0x20u && special <= 0x2Au) ||
            (special >= 0x2Cu && special <= 0x2Eu);
        if (writesAcc)
        {
            addVfRead(usage, fs, dest);
            if (special <= 0x1Bu)
                addVfRead(usage, ft, laneForComponent(special & 3u));
            else if ((special >= 0x28u && special <= 0x2Eu))
                addVfRead(usage, ft, special == 0x2Eu ? 0xEu : dest);
            usage.accWrite = dest;
            if ((special >= 0x08u && special <= 0x0Fu) ||
                special == 0x21u || special == 0x23u || special == 0x25u ||
                special == 0x27u || special == 0x29u || special == 0x2Du)
            {
                usage.accRead = dest;
            }
        }
        else if (special >= 0x10u && special <= 0x17u)
        {
            addVfRead(usage, fs, dest);
            addVfWrite(usage, ft, dest);
        }
        else if (special == 0x1Du)
        {
            addVfRead(usage, fs, dest);
            addVfWrite(usage, ft, dest);
        }
        else if (special == 0x1Fu)
        {
            addVfRead(usage, fs, 0xEu);
            addVfRead(usage, ft, 0x1u);
            usage.writesClip = true;
        }
        else if (special != 0x2Fu && special != 0x30u)
        {
            usage.reserved = true;
        }
        return usage;
    }

    usage.reserved = true;
    return usage;
}

VU1Interpreter::InstructionUsage VU1Interpreter::decodeLowerUsage(uint32_t lower) const
{
    InstructionUsage usage;
    if (lower == 0u || lower == 0x8000033Cu)
        return usage;

    const uint8_t opHi = static_cast<uint8_t>((lower >> 25) & 0x7Fu);
    const uint8_t vfT = FT(lower);
    const uint8_t vfS = FS(lower);
    const uint8_t viT = VIT(lower);
    const uint8_t viS = VIS(lower);
    const uint8_t viD = VID(lower);
    const uint8_t dest = DEST(lower);
    auto readVi = [&](uint8_t reg)
    {
        if (reg != 0u)
            usage.viRead |= static_cast<uint16_t>(1u << reg);
    };
    auto writeVi = [&](uint8_t reg)
    {
        if (reg != 0u)
            usage.viWrite |= static_cast<uint16_t>(1u << reg);
    };

    switch (opHi)
    {
    case 0x00:
        usage.pipeline = PipelineLsu;
        usage.latency = 4u;
        readVi(viS);
        addVfWrite(usage, vfT, dest);
        return usage;
    case 0x01:
        usage.pipeline = PipelineLsu;
        usage.latency = 1u;
        readVi(viT);
        addVfRead(usage, vfS, dest);
        return usage;
    case 0x04:
        usage.pipeline = PipelineLsu;
        usage.latency = 4u;
        readVi(viS);
        writeVi(viT);
        return usage;
    case 0x05:
        usage.pipeline = PipelineLsu;
        usage.latency = 1u;
        readVi(viS);
        readVi(viT);
        return usage;
    case 0x08:
    case 0x09:
        usage.pipeline = PipelineIalu;
        usage.latency = 1u;
        usage.delaysNextBranchRead = true;
        readVi(viS);
        writeVi(viT);
        return usage;
    case 0x10:
    case 0x12:
    case 0x13:
        usage.pipeline = PipelineIalu;
        usage.latency = 1u;
        usage.readsClip = true;
        writeVi(1u);
        return usage;
    case 0x11:
        usage.pipeline = PipelineFmac;
        usage.latency = kFmacLatency;
        usage.writesClip = true;
        return usage;
    case 0x14:
    case 0x16:
    case 0x17:
        usage.pipeline = PipelineIalu;
        usage.latency = 1u;
        writeVi(viT);
        return usage;
    case 0x15:
        usage.pipeline = PipelineFmac;
        usage.latency = kFmacLatency;
        return usage;
    case 0x18:
    case 0x1A:
    case 0x1B:
        usage.pipeline = PipelineIalu;
        usage.latency = 1u;
        readVi(viS);
        writeVi(viT);
        return usage;
    case 0x1C:
        usage.pipeline = PipelineIalu;
        usage.latency = 1u;
        usage.readsClip = true;
        writeVi(viT);
        return usage;
    case 0x20:
        usage.pipeline = PipelineBranch;
        return usage;
    case 0x21:
        usage.pipeline = PipelineBranch;
        usage.latency = 1u;
        writeVi(viT);
        return usage;
    case 0x24:
        usage.pipeline = PipelineBranch;
        readVi(viS);
        return usage;
    case 0x25:
        usage.pipeline = PipelineBranch;
        usage.latency = 1u;
        readVi(viS);
        writeVi(viT);
        return usage;
    case 0x28:
    case 0x29:
        usage.pipeline = PipelineBranch;
        readVi(viS);
        readVi(viT);
        return usage;
    case 0x2C:
    case 0x2D:
    case 0x2E:
    case 0x2F:
        usage.pipeline = PipelineBranch;
        readVi(viS);
        return usage;
    case 0x40:
        break;
    default:
        usage.reserved = true;
        return usage;
    }

    const uint8_t direct = static_cast<uint8_t>(lower & 0x3Fu);
    if (direct == 0x30u || direct == 0x31u || direct == 0x34u || direct == 0x35u)
    {
        usage.pipeline = PipelineIalu;
        usage.latency = 1u;
        usage.delaysNextBranchRead = true;
        readVi(viS);
        readVi(viT);
        writeVi(viD);
        return usage;
    }
    if (direct == 0x32u)
    {
        usage.pipeline = PipelineIalu;
        usage.latency = 1u;
        usage.delaysNextBranchRead = true;
        readVi(viS);
        writeVi(viT);
        return usage;
    }
    if (direct < 0x3Cu)
    {
        usage.reserved = true;
        return usage;
    }

    const uint8_t special = static_cast<uint8_t>((lower & 3u) | ((lower >> 4) & 0x7Cu));
    switch (special)
    {
    case 0x30:
    case 0x31:
        usage.pipeline = PipelineFmac;
        usage.latency = 4u;
        addVfRead(usage, vfS, special == 0x31u ? 0xFu : dest);
        addVfWrite(usage, vfT, dest);
        break;
    case 0x34:
    case 0x36:
        usage.pipeline = PipelineLsu;
        usage.latency = 4u;
        usage.viLatency = 1u;
        usage.delaysNextBranchRead = true;
        readVi(viS);
        writeVi(viS);
        addVfWrite(usage, vfT, dest);
        break;
    case 0x35:
    case 0x37:
        usage.pipeline = PipelineLsu;
        usage.latency = 1u;
        usage.delaysNextBranchRead = true;
        readVi(viT);
        writeVi(viT);
        addVfRead(usage, vfS, dest);
        break;
    case 0x38:
        usage.pipeline = PipelineFdiv;
        usage.latency = 7u;
        addVfRead(usage, vfS, laneForComponent((lower >> 21) & 3u));
        addVfRead(usage, vfT, laneForComponent((lower >> 23) & 3u));
        break;
    case 0x39:
        usage.pipeline = PipelineFdiv;
        usage.latency = 7u;
        addVfRead(usage, vfT, laneForComponent((lower >> 23) & 3u));
        break;
    case 0x3A:
        usage.pipeline = PipelineFdiv;
        usage.latency = 13u;
        addVfRead(usage, vfS, laneForComponent((lower >> 21) & 3u));
        addVfRead(usage, vfT, laneForComponent((lower >> 23) & 3u));
        break;
    case 0x3B:
        usage.pipeline = PipelineFdiv;
        usage.waitQ = true;
        break;
    case 0x3C:
        usage.pipeline = PipelineIalu;
        usage.latency = 1u;
        usage.delaysNextBranchRead = true;
        addVfRead(usage, vfS, laneForComponent((lower >> 21) & 3u));
        writeVi(viT);
        break;
    case 0x3D:
        usage.pipeline = PipelineFmac;
        usage.latency = 4u;
        readVi(viS);
        addVfWrite(usage, vfT, dest);
        break;
    case 0x3E:
        usage.pipeline = PipelineLsu;
        usage.latency = 4u;
        readVi(viS);
        writeVi(viT);
        break;
    case 0x3F:
        usage.pipeline = PipelineLsu;
        usage.latency = 1u;
        readVi(viS);
        readVi(viT);
        break;
    case 0x40:
    case 0x41:
        usage.pipeline = PipelineFmac;
        usage.latency = 4u;
        addVfWrite(usage, vfT, dest);
        break;
    case 0x42:
    case 0x43:
        usage.pipeline = PipelineIalu;
        usage.latency = 1u;
        addVfRead(usage, vfS, laneForComponent((lower >> 21) & 3u));
        break;
    case 0x64:
        if (m_unit == Unit::VU0)
        {
            usage.reserved = true;
            break;
        }
        usage.pipeline = PipelineFmac;
        usage.latency = 4u;
        addVfWrite(usage, vfT, dest);
        break;
    case 0x68:
    case 0x69:
        usage.pipeline = PipelineIalu;
        usage.latency = 1u;
        writeVi(viT);
        break;
    case 0x6C:
        if (m_unit == Unit::VU0)
        {
            usage.reserved = true;
            break;
        }
        usage.pipeline = PipelineXgkick;
        usage.latency = 2u;
        readVi(viS);
        break;
    case 0x70:
    case 0x71:
    case 0x72:
    case 0x73:
    case 0x74:
    case 0x75:
    case 0x76:
    case 0x78:
    case 0x79:
    case 0x7A:
    case 0x7C:
    case 0x7D:
    case 0x7E:   // EFU opcode map as in ps2_vu1_lower.cpp: 0x79 ERSQRT, 0x7C ESIN, 0x7D EATAN, 0x7E EEXP (0x77 does not exist)
        if (m_unit == Unit::VU0)
        {
            usage.reserved = true;
            break;
        }
        usage.pipeline = PipelineEfu;
        switch (special)
        {
        case 0x70:
            usage.latency = 11u;
            break;
        case 0x71:
        case 0x72:
        case 0x79:
            usage.latency = 18u;
            break;
        case 0x73:
            usage.latency = 24u;
            break;
        case 0x74:
        case 0x75:
        case 0x7D:
            usage.latency = 54u;
            break;
        case 0x76:
        case 0x78:
        case 0x7A:
            usage.latency = 12u;
            break;
        case 0x7C:
            usage.latency = 29u;
            break;
        case 0x7E:
            usage.latency = 44u;
            break;
        default:
            break;
        }
        if (special >= 0x70u && special <= 0x73u)
            addVfRead(usage, vfS, 0xEu);
        else if (special == 0x74u)
            addVfRead(usage, vfS, 0xCu);
        else if (special == 0x75u)
            addVfRead(usage, vfS, 0xAu);
        else if (special == 0x76u)
            addVfRead(usage, vfS, 0xFu);
        else
            addVfRead(usage, vfS, laneForComponent((lower >> 21) & 3u));
        break;
    case 0x7B:
        if (m_unit == Unit::VU0)
        {
            usage.reserved = true;
            break;
        }
        usage.pipeline = PipelineEfu;
        usage.waitP = true;
        break;
    default:
        usage.reserved = true;
        break;
    }
    return usage;
}

VU1Interpreter::DecodedInstructionPair VU1Interpreter::decodeInstructionPair(const uint8_t *vuCode, uint32_t pc) const
{
    DecodedInstructionPair decoded;
    std::memcpy(&decoded.lower, vuCode + pc, sizeof(decoded.lower));
    std::memcpy(&decoded.upper, vuCode + pc + sizeof(decoded.lower), sizeof(decoded.upper));
    decoded.iBit = (decoded.upper & 0x80000000u) != 0u;
    decoded.eBit = (decoded.upper & 0x40000000u) != 0u;
    decoded.mBit = (decoded.upper & 0x20000000u) != 0u;
    decoded.dBit = (decoded.upper & 0x10000000u) != 0u;
    decoded.tBit = (decoded.upper & 0x08000000u) != 0u;
    decoded.upperUsage = decodeUpperUsage(decoded.upper);
    if (!decoded.iBit)
        decoded.lowerUsage = decodeLowerUsage(decoded.lower);

    const uint8_t upperWriteReg = decoded.upperUsage.vfWrite.reg;
    if (upperWriteReg != 0u && (vfReadLanes(decoded.lowerUsage, upperWriteReg) != 0u || decoded.lowerUsage.vfWrite.reg == upperWriteReg))
    {
        decoded.upperVfShadowReg = upperWriteReg;
        if (decoded.lowerUsage.vfWrite.reg == upperWriteReg)
            decoded.suppressedLowerVf = upperWriteReg;
    }
    return decoded;
}

void VU1Interpreter::rebuildDecodedCodeCache(const uint8_t *vuCode, uint32_t codeSize,
                                             const PS2Memory *memory, uint64_t generation)
{
    const uint32_t pairCount = std::min<uint32_t>(codeSize / 8u, kMaxDecodedPairs);
    for (uint32_t i = 0; i < pairCount; ++i)
        m_decodedCodeCache[i] = decodeInstructionPair(vuCode, i * 8u);

    // PS2X_VU1_FLAGS_ALWAYS=1 turns the saving off. Otherwise: if no instruction in micro memory reads MAC/status
    // (FSEQ/FSAND/FSOR/FMEQ/FMAND/FMOR), the FMAC flag pipeline does not need to be filled.
    static const bool flagsAlways = [] { const char *e = std::getenv("PS2X_VU1_FLAGS_ALWAYS"); return e && *e && *e != '0'; }();
    bool live = flagsAlways;
    for (uint32_t i = 0; i < pairCount && !live; ++i)
    {
        uint32_t lower = 0, upper = 0;
        std::memcpy(&lower, vuCode + i * 8u, 4);
        std::memcpy(&upper, vuCode + i * 8u + 4u, 4);
        if ((upper >> 31) != 0u || (lower >> 31) != 0u)
            continue;
        const uint32_t op = lower >> 25;
        if (op == 0x14u || op == 0x16u || op == 0x17u || op == 0x18u || op == 0x1Au || op == 0x1Bu)
            live = true;
    }
    m_flagsLive = live;

    m_cachedVuCode = vuCode;
    m_cachedMemory = memory;
    m_cachedCodeSize = codeSize;
    m_cachedCodeGeneration = generation;
    m_decodedCodeCacheValid = true;
}

VU1Interpreter::DecodedInstructionPair VU1Interpreter::getDecodedInstructionPairForPc(
    const uint8_t *vuCode, uint32_t codeSize, PS2Memory *memory, uint32_t pc)
{
    if ((pc & 7u) != 0u)
        return decodeInstructionPair(vuCode, pc);

    const bool trackedVu1Code = memory != nullptr &&
                                ((m_unit == Unit::VU1 && vuCode == memory->getVU1Code()) ||
                                 (m_unit == Unit::VU0 && vuCode == memory->getVU0Code()));
    if (!trackedVu1Code)
        return decodeInstructionPair(vuCode, pc);

    const uint64_t generation = m_unit == Unit::VU1 ? memory->getVU1CodeGeneration() : memory->getVU0CodeGeneration();
    if (!m_decodedCodeCacheValid ||
        m_cachedVuCode != vuCode ||
        m_cachedMemory != memory ||
        m_cachedCodeSize != codeSize ||
        m_cachedCodeGeneration != generation)
    {
        rebuildDecodedCodeCache(vuCode, codeSize, memory, generation);
    }
    const uint32_t pairIndex = pc / 8u;
    if (pairIndex >= kMaxDecodedPairs)
        return decodeInstructionPair(vuCode, pc);
    return m_decodedCodeCache[pairIndex];
}

const VU1Interpreter::DecodedInstructionPair *VU1Interpreter::decodedPairPtr(
    const uint8_t *vuCode, uint32_t codeSize, PS2Memory *memory, uint32_t pc)
{
    const bool tracked = (pc & 7u) == 0u && memory != nullptr && pc / 8u < kMaxDecodedPairs &&
                         ((m_unit == Unit::VU1 && vuCode == memory->getVU1Code()) ||
                          (m_unit == Unit::VU0 && vuCode == memory->getVU0Code()));
    if (!tracked)
    {
        m_flagsLive = true;
        m_decodeScratch = decodeInstructionPair(vuCode, pc);
        return &m_decodeScratch;
    }
    const uint64_t generation = m_unit == Unit::VU1 ? memory->getVU1CodeGeneration() : memory->getVU0CodeGeneration();
    if (!m_decodedCodeCacheValid || m_cachedVuCode != vuCode || m_cachedMemory != memory ||
        m_cachedCodeSize != codeSize || m_cachedCodeGeneration != generation)
        rebuildDecodedCodeCache(vuCode, codeSize, memory, generation);
    return &m_decodedCodeCache[pc / 8u];
}

void VU1Interpreter::reportReservedInstruction(bool upper, uint32_t instruction)
{
    RUNTIME_ERROR(
        "[VU" << (m_unit == Unit::VU1 ? "1" : "0")
              << " reserved " << (upper ? "upper" : "lower")
              << "] cycle=" << m_cycle
              << " pc=0x" << std::hex << m_state.pc
              << " instruction=0x" << instruction
              << std::dec << '\n');
    m_stopRequested = true;
}


// PS2X_VU1_DUMP=<dir>: record VU1 micro programs (basis for the VU1 recompiler).
//   <dir>/vu1_<hash>.bin   : 16 KB micro memory at the first run of this code
//   <ordner>/vu1_index.txt    : hash startpc aufrufe zyklen (alle 2048 Aufrufe neu geschrieben)
namespace ps2x_vu1dump
{
    struct Key { uint64_t hash; uint32_t pc; bool operator<(const Key &o) const { return hash != o.hash ? hash < o.hash : pc < o.pc; } };
    struct Val { uint64_t calls = 0; uint64_t cycles = 0; };
    static std::mutex g_mutex;
    static std::map<Key, Val> g_map;
    static uint64_t g_lastGen = ~0ull, g_lastHash = 0, g_calls = 0;
    static bool g_lastNew = false;
    static std::map<uint64_t, std::vector<uint8_t>> g_cover; // hash -> ausgefuehrte Paare (pc/8)
    static const char *dir() { static const char *d = [] { const char *e = std::getenv("PS2X_VU1_DUMP"); if (e && *e) { std::error_code ec; std::filesystem::create_directories(e, ec); } return (e && *e) ? e : nullptr; }(); return d; }
    static uint64_t fnv(const uint8_t *p, size_t n) { uint64_t h = 1469598103934665603ull; for (size_t i = 0; i < n; ++i) { h ^= p[i]; h *= 1099511628211ull; } return h; }
    static uint64_t codeHash(const uint8_t *code, uint32_t size, uint64_t gen)
    {
        if (gen == g_lastGen) return g_lastHash;
        g_lastGen = gen; g_lastHash = fnv(code, size);
        char path[512]; std::snprintf(path, sizeof(path), "%s/vu1_%016llx.bin", dir(), (unsigned long long)g_lastHash);
        g_lastNew = false;
        if (FILE *f = std::fopen(path, "rb")) std::fclose(f);
        else if (FILE *w = std::fopen(path, "wb")) { std::fwrite(code, 1, size, w); std::fclose(w); g_lastNew = true; }
        g_cover[g_lastHash].resize(size / 8u, 0u);
        return g_lastHash;
    }
    static void record(uint64_t hash, uint32_t pc, uint64_t cycles)
    {
        Val &v = g_map[Key{hash, pc}]; v.calls++; v.cycles += cycles;
        // PS2X_VU1_DUMP_EVERY: calls between two writes (default 2048 ~ 1 frame; while playing
        // e.g. 262144 ~ 2 min, so that recording causes no hitches during play)
        static const uint64_t every = [] { const char *e = std::getenv("PS2X_VU1_DUMP_EVERY"); const unsigned long long n = (e && *e) ? std::strtoull(e, nullptr, 10) : 0ull; return n ? (uint64_t)n : 2048ull; }();
        if ((++g_calls % every) != 0u) return;
        char path[512]; std::snprintf(path, sizeof(path), "%s/vu1_index.txt", dir());
        if (FILE *w = std::fopen(path, "w"))
        {
            for (const auto &kv : g_map) std::fprintf(w, "%016llx %04x %llu %llu\n", (unsigned long long)kv.first.hash, kv.first.pc, (unsigned long long)kv.second.calls, (unsigned long long)kv.second.cycles);
            std::fclose(w);
        }
        for (const auto &kv : g_cover)
        {
            std::snprintf(path, sizeof(path), "%s/vu1_%016llx.cov", dir(), (unsigned long long)kv.first);
            if (FILE *w = std::fopen(path, "w"))
            {
                for (size_t i = 0; i < kv.second.size(); ++i)
                    if (kv.second[i]) std::fprintf(w, "%04zx\n", i * 8u);
                std::fclose(w);
            }
        }
    }
}


extern const VU1Interpreter::CompiledEntry g_vu1CompiledTable[];
extern const uint32_t g_vu1CompiledStatusIgnore; // status bits the translated programs do not track (unread)
extern const size_t g_vu1CompiledCount;
extern const uint8_t *vu1CompiledEntryMap(uint64_t hash);
extern VU1Interpreter::CompiledFn vu1CompiledMatch(const uint8_t *code, uint32_t size, uint64_t whole, const uint8_t **entry);
[[maybe_unused]] static VU1Interpreter::CompiledFn vu1LookupCompiled(uint64_t hash)
{
    for (size_t i = 0; i < g_vu1CompiledCount; ++i)
        if (g_vu1CompiledTable[i].hash == hash)
            return g_vu1CompiledTable[i].fn;
    return nullptr;
}

void VU1Interpreter::dumpDecoded(const uint8_t *vuCode, uint32_t codeSize, const char *path) const
{
    FILE *w = std::fopen(path, "w");
    if (!w) return;
    // Spalten je Paar: pc lower upper iBit eBit mBit dBit tBit shadow suppressed | upper-usage | lower-usage
    // usage: r0reg r0lanes r1reg r1lanes nread wreg wlanes viRead viWrite accRead accWrite lat vfLat viLat pipe waitQ waitP rClip wClip delayBr reserved
    const auto u = [w](const InstructionUsage &x) {
        std::fprintf(w, " %u %u %u %u %u %u %u %u %u %u %u %u %u %u %u %u %u %u %u %u %u",
                     x.vfRead[0].reg, x.vfRead[0].lanes, x.vfRead[1].reg, x.vfRead[1].lanes, x.vfReadCount,
                     x.vfWrite.reg, x.vfWrite.lanes, x.viRead, x.viWrite, x.accRead, x.accWrite,
                     x.latency, x.vfLatency, x.viLatency, static_cast<unsigned>(x.pipeline),
                     x.waitQ, x.waitP, x.readsClip, x.writesClip, x.delaysNextBranchRead, x.reserved);
    };
    for (uint32_t pc = 0; pc + 8u <= codeSize; pc += 8u)
    {
        const DecodedInstructionPair d = decodeInstructionPair(vuCode, pc);
        std::fprintf(w, "%04x %08x %08x %u %u %u %u %u %u %u", pc, d.lower, d.upper, d.iBit, d.eBit, d.mBit, d.dBit, d.tBit,
                     d.upperVfShadowReg, d.suppressedLowerVf);
        u(d.upperUsage);
        u(d.lowerUsage);
        std::fputc('\n', w);
    }
    std::fclose(w);
}

void VU1Interpreter::execute(uint8_t *vuCode, uint32_t codeSize,
                             uint8_t *vuData, uint32_t dataSize,
                             GS &gs, PS2Memory *memory,
                             uint32_t startPC, uint32_t top, uint32_t itop,
                             uint32_t maxCycles)
{
    if (m_unit == Unit::VU1 && g_ps2xStateHist.load(std::memory_order_relaxed))
    {
        std::lock_guard<std::mutex> lock(g_ps2xVu1ProgMutex);
        ++g_ps2xVu1ProgCount[startPC & microAddressMask()];
    }
    resetScheduler();
    m_state.pc = startPC & microAddressMask();
    m_state.ebit = false;
    m_state.haltAfterDelaySlot = false;
    m_state.stoppedByD = false;
    m_state.stoppedByT = false;
    m_state.top = top;
    m_state.itop = itop;
    m_state.branchPending = false;
    m_state.branchTarget = 0;
    m_state.branchDelay = 0;
    m_state.vf[0][0] = 0.0f;
    m_state.vf[0][1] = 0.0f;
    m_state.vf[0][2] = 0.0f;
    m_state.vf[0][3] = 1.0f;
    // PS2X_VU1_DIFF=1: additionally run every call through the pure interpreter as a reference (copy), compare the result bit-exactly
    static const bool diffOn = [] { const char *e = std::getenv("PS2X_VU1_DIFF"); return e && *e && *e != '0'; }();
    if (diffOn && m_unit == Unit::VU1 && memory && vuCode == memory->getVU1Code() && !m_compiledDisabled)
    {
        static uint64_t s_calls = 0, s_bad = 0;
        auto ref = std::make_unique<VU1Interpreter>(*this);
        std::vector<uint8_t> refData(vuData, vuData + dataSize), capRef, capReal;
        ref->m_compiledDisabled = true;
        ref->m_gifCapture = &capRef;
        ref->m_gifSubmit = false;
        const uint32_t pc0 = m_state.pc;
        ref->run(vuCode, codeSize, refData.data(), dataSize, gs, memory, maxCycles);
        m_gifCapture = &capReal;
        run(vuCode, codeSize, vuData, dataSize, gs, memory, maxCycles);
        m_gifCapture = nullptr;
        ++s_calls;
        const VU1State &a = m_state, &b = ref->m_state;
        const char *what = nullptr;
        char detail[160] = {0};
        if (std::memcmp(a.vf, b.vf, sizeof(a.vf)) != 0)
        {
            what = "VF";
            for (int r = 0; r < 32 && !detail[0]; ++r)
                for (int c = 0; c < 4; ++c)
                    if (std::memcmp(&a.vf[r][c], &b.vf[r][c], 4) != 0)
                    {
                        uint32_t x, y; std::memcpy(&x, &a.vf[r][c], 4); std::memcpy(&y, &b.vf[r][c], 4);
                        std::snprintf(detail, sizeof(detail), " vf%d.%c translated %08x (%g) interpreter %08x (%g)", r, "xyzw"[c], x, a.vf[r][c], y, b.vf[r][c]);
                        break;
                    }
        }
        else if (std::memcmp(a.vi, b.vi, sizeof(a.vi)) != 0) what = "VI";
        else if (std::memcmp(a.acc, b.acc, sizeof(a.acc)) != 0) what = "ACC";
        else if (std::memcmp(&a.q, &b.q, 4) || std::memcmp(&a.p, &b.p, 4) || std::memcmp(&a.i, &b.i, 4) || a.r != b.r) what = "Q/P/I/R";
        else if (capReal != capRef) what = "XGKICK data";   // TS2: output first (CLIP at the end may still be in the pipeline)
        else if ([&] { // TS2: final CLIP value incl. pipeline entries still in flight (level 5 ends with fewer cycles)
                     auto fin = [](const VU1Interpreter &x) { uint32_t c = x.m_state.clip; uint64_t best = 0; bool any = false;
                         for (const auto &e : x.m_flagPipeline) if (e.valid && e.writesClip && (!any || e.issueCycle >= best)) { best = e.issueCycle; c = e.clip; any = true; }
                         return c; };
                     const bool d = fin(*this) != fin(*ref);
                     if (d) std::snprintf(detail, sizeof(detail), " clip translated %06x/fin %06x/work %06x  interpreter %06x/fin %06x/work %06x",
                                          m_state.clip, fin(*this), m_workingClip, ref->m_state.clip, fin(*ref), ref->m_workingClip);
                     return d; }()) what = "CLIP";
        else if (m_flagsLive && (a.mac != b.mac || ((a.status ^ b.status) & ~g_vu1CompiledStatusIgnore) != 0u)) what = "MAC/STATUS";
        else if (a.pc != b.pc) what = "PC";
        else if (std::memcmp(vuData, refData.data(), dataSize) != 0)
        {
            what = "VU-Datenspeicher";
            for (uint32_t k = 0; k < dataSize; ++k)
                if (vuData[k] != refData[k]) { std::snprintf(detail, sizeof(detail), " first difference at 0x%04x", k); break; }
        }
        // PS2X_VU1_DIFF=2: do not compare the cycle count (level 3 transfers XGKICK at once instead of cycle by cycle)
        static const bool cyclesToo = [] { const char *e = std::getenv("PS2X_VU1_DIFF"); return !(e && e[0] == '2'); }();
        if (!what && cyclesToo && m_cycle != ref->m_cycle) what = "Zyklen";
        static std::map<std::pair<uint64_t, std::string>, uint64_t> s_kinds;
        uint64_t kindCount = 0;
        if (what)
        {
            ++s_bad;
            kindCount = ++s_kinds[{ps2x_vu1dump::fnv(vuCode, codeSize), what}];
        }
        if (what && kindCount <= 3)
            std::fprintf(stderr, "[VU1-DIFF] call %llu code %016llx start 0x%04x: %s differs%s (pc %04x/%04x, cycles %llu/%llu)\n",
                         (unsigned long long)s_calls, (unsigned long long)ps2x_vu1dump::fnv(vuCode, codeSize), pc0, what, detail,
                         a.pc, b.pc, (unsigned long long)m_cycle, (unsigned long long)ref->m_cycle);
        if ((s_calls % 20000u) == 0u)
        {
            std::fprintf(stderr, "[VU1-DIFF] %llu calls checked, %llu differences:", (unsigned long long)s_calls, (unsigned long long)s_bad);
            for (const auto &kv : s_kinds)
                std::fprintf(stderr, " [%016llx %s: %llu]", (unsigned long long)kv.first.first, kv.first.second.c_str(), (unsigned long long)kv.second);
            std::fprintf(stderr, "\n");
        }
        return;
    }
    // PS2X_VU1_SELFTEST=<dir with vu1_<hash>.bin>: compute every translated program at every entry with random registers and
    // data once translated and once in the interpreter and compare bit-exactly (also modules no
    // test route reaches). Runs once at the first VU1 call; micro memory is restored afterwards.
    if (m_unit == Unit::VU1 && memory && vuCode == memory->getVU1Code())
    {
        static std::atomic<bool> s_stStarted{false};
        static const char *const sdEnv = std::getenv("PS2X_VU1_SELFTEST"); // read once (execute runs >2000x per frame)
        if (sdEnv && *sdEnv && !s_stStarted.exchange(true)) [&] { // nested execute() calls of the test run normally
            const char *sd = sdEnv;
            const int seeds = [] { const char *e = std::getenv("PS2X_VU1_SELFTEST_N"); return (e && *e) ? std::max(1, std::atoi(e)) : 8; }();
            std::vector<uint8_t> saveCode(vuCode, vuCode + codeSize), saveData(vuData, vuData + dataSize);
            uint64_t rng = 0x9E3779B97F4A7C15ull;
            auto rnd = [&rng]() { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return rng; };
            unsigned progs = 0, cases = 0, ok = 0, skipped = 0, bad = 0;
            std::error_code ec;
            std::vector<std::filesystem::path> files;
            for (const auto &e : std::filesystem::directory_iterator(sd, ec))
                if (e.path().extension() == ".bin" && e.path().filename().string().rfind("vu1_", 0) == 0) files.push_back(e.path());
            std::sort(files.begin(), files.end());
            for (const auto &fp : files)
            {
                std::vector<uint8_t> img(codeSize, 0);
                if (FILE *f = std::fopen(fp.string().c_str(), "rb")) { (void)std::fread(img.data(), 1, img.size(), f); std::fclose(f); }
                const uint8_t *entry = nullptr;
                const uint64_t whole = ps2x_vu1dump::fnv(img.data(), codeSize);
                if (!vu1CompiledMatch(img.data(), codeSize, whole, &entry) || !entry) { std::fprintf(stderr, "[VU1 self-test] %s: not translated\n", fp.filename().string().c_str()); continue; }
                ++progs;
                std::memcpy(vuCode, img.data(), codeSize);
                unsigned pBad = 0;
                for (uint32_t pc = 0; pc < codeSize; pc += 8u)
                {
                    if (!entry[pc >> 3]) continue;
                    for (int sdn = 0; sdn < seeds; ++sdn)
                    {
                        ++cases;
                        VU1State st = m_state;
                        for (int r = 1; r < 32; ++r) for (int c = 0; c < 4; ++c) st.vf[r][c] = static_cast<float>(static_cast<int64_t>(rnd() % 20001u) - 10000) / 64.0f;
                        for (int r = 1; r < 16; ++r) st.vi[r] = static_cast<int32_t>(rnd() % 1024u);
                        for (int c = 0; c < 4; ++c) st.acc[c] = static_cast<float>(static_cast<int64_t>(rnd() % 2001u) - 1000) / 16.0f;
                        std::vector<uint8_t> dA(dataSize), dB;
                        for (uint32_t k = 0; k < dataSize; k += 4)
                        {
                            const uint64_t x = rnd();
                            float fv = static_cast<float>(static_cast<int64_t>(x % 20001u) - 10000) / 32.0f;
                            uint32_t w; std::memcpy(&w, &fv, 4);
                            if ((x >> 40) % 4u == 0u) w = static_cast<uint32_t>(x >> 20) & 0x3FFu; // auch kleine Ganzzahlen (Zaehler, Adressen)
                            // Every quadword starts with a valid GIF tag (EOP, NLOOP 0): an XGKICK at any
                            // address then transfers a clean tag as in the game instead of random garbage
                            if ((k & 15u) == 0u) w = 0x00008000u;
                            else if ((k & 15u) == 4u) w = 0u;
                            std::memcpy(dA.data() + k, &w, 4);
                        }
                        dB = dA;
                        auto A = std::make_unique<VU1Interpreter>(*this);
                        auto B = std::make_unique<VU1Interpreter>(*this);
                        std::vector<uint8_t> capA, capB;
                        A->m_compiledDisabled = true; A->m_gifCapture = &capA; A->m_gifSubmit = false; A->m_recGen = ~0ull; A->m_cachedCodeSize = 0u; // decoder cache: test image, not the old code
                        B->m_gifCapture = &capB; B->m_gifSubmit = false; B->m_recGen = ~0ull; B->m_cachedCodeSize = 0u;
                        A->m_state = st; B->m_state = st;
                        const uint32_t top = static_cast<uint32_t>(rnd() % 1024u), itop = static_cast<uint32_t>(rnd() % 1024u);
                        const uint32_t budget = 200000u;
                        const uint64_t c0 = A->m_cycle;
                        A->execute(vuCode, codeSize, dA.data(), dataSize, gs, memory, pc, top, itop, budget);
                        B->execute(vuCode, codeSize, dB.data(), dataSize, gs, memory, pc, top, itop, budget);
                        if (A->m_cycle - c0 >= budget || B->m_cycle - c0 >= budget) { ++skipped; continue; } // Endlosschleife durch Zufallsdaten
                        const VU1State &a = B->m_state, &b = A->m_state;
                        const char *what = nullptr;
                        if (std::memcmp(a.vf, b.vf, sizeof(a.vf)) != 0) what = "VF";
                        else if (std::memcmp(a.vi, b.vi, sizeof(a.vi)) != 0) what = "VI";
                        else if (std::memcmp(a.acc, b.acc, sizeof(a.acc)) != 0) what = "ACC";
                        else if (std::memcmp(&a.q, &b.q, 4) || std::memcmp(&a.p, &b.p, 4) || std::memcmp(&a.i, &b.i, 4) || a.r != b.r) what = "Q/P/I/R";
                        else if (a.clip != b.clip) what = "CLIP";
                        else if (B->m_flagsLive && (a.mac != b.mac || ((a.status ^ b.status) & ~g_vu1CompiledStatusIgnore) != 0u)) what = "MAC/STATUS";
                        else if (a.pc != b.pc) what = "PC";
                        else if (capA != capB) what = "XGKICK data";
                        else if (dA != dB) what = "VU-Datenspeicher";
                        if (what)
                        {
                            ++bad; ++pBad;
                            if (pBad <= 3)
                                std::fprintf(stderr, "[VU1-Selbsttest] %s Einstieg %04x Lauf %d: %s weicht ab (pc %04x/%04x, Zyklen %llu/%llu)\n",
                                             fp.filename().string().c_str(), pc, sdn, what, a.pc, b.pc, (unsigned long long)B->m_cycle, (unsigned long long)A->m_cycle);
                        }
                        else ++ok;
                    }
                }
                std::fprintf(stderr, "[VU1-Selbsttest] %s: %u Abweichungen\n", fp.filename().string().c_str(), pBad);
            }
            std::memcpy(vuCode, saveCode.data(), codeSize);
            std::memcpy(vuData, saveData.data(), dataSize);
            std::fprintf(stderr, "[VU1 self-test] %u programs, %u cases: %u equal, %u differences, %u skipped (budget)\n", progs, cases, ok, bad, skipped);
        }();
    }
    // PS2X_VU1_DECODE=<dir>: translate micro memory images (vu1_<hash>.bin, e.g. from tools/vu1_module.py) once into
    // .dec with this runtime's decoder (basis for tools/vu1rec.py, without playing the game)
    if (m_unit == Unit::VU1)
    {
        static std::once_flag s_dec;
        std::call_once(s_dec, [this] {
            const char *dd = std::getenv("PS2X_VU1_DECODE");
            if (!dd || !*dd) return;
            std::error_code ec; unsigned n = 0;
            for (const auto &e : std::filesystem::directory_iterator(dd, ec))
            {
                const std::string fn = e.path().filename().string();
                if (fn.size() != 24 || fn.rfind("vu1_", 0) != 0 || e.path().extension() != ".bin") continue;
                std::filesystem::path dec = e.path(); dec.replace_extension(".dec");
                if (std::filesystem::exists(dec)) continue;
                std::vector<uint8_t> img(16384, 0);
                if (FILE *f = std::fopen(e.path().string().c_str(), "rb")) { (void)std::fread(img.data(), 1, img.size(), f); std::fclose(f); }
                dumpDecoded(img.data(), static_cast<uint32_t>(img.size()), dec.string().c_str());
                ++n;
            }
            std::fprintf(stderr, "[VU1-Decode] %u Abbilder in %s dekodiert\n", n, dd);
            // Release installer: exit right after decoding (pass 1 of the local build)
            if (const char *x = std::getenv("PS2X_VU1_DECODE_EXIT"); x && *x == '1') { std::fflush(nullptr); std::_Exit(0); }
        });
    }
    if (m_unit == Unit::VU1 && ps2x_vu1dump::dir() && memory && vuCode == memory->getVU1Code())
    {
        std::lock_guard<std::mutex> lk(ps2x_vu1dump::g_mutex);
        const uint64_t h = ps2x_vu1dump::codeHash(vuCode, codeSize, memory->getVU1CodeGeneration());
        if (ps2x_vu1dump::g_lastNew)
        {
            char path[512];
            std::snprintf(path, sizeof(path), "%s/vu1_%016llx.dec", ps2x_vu1dump::dir(), (unsigned long long)h);
            dumpDecoded(vuCode, codeSize, path);
            ps2x_vu1dump::g_lastNew = false;
        }
        const uint32_t pc0 = m_state.pc;
        const uint64_t c0 = m_cycle;
        run(vuCode, codeSize, vuData, dataSize, gs, memory, maxCycles);
        ps2x_vu1dump::record(h, pc0, m_cycle - c0);
        return;
    }
    run(vuCode, codeSize, vuData, dataSize, gs, memory, maxCycles);
}

void VU1Interpreter::resume(uint8_t *vuCode, uint32_t codeSize,
                            uint8_t *vuData, uint32_t dataSize,
                            GS &gs, PS2Memory *memory,
                            uint32_t top, uint32_t itop, uint32_t maxCycles)
{
    m_state.top = top;
    m_state.itop = itop;
    m_state.stoppedByD = false;
    m_state.stoppedByT = false;
    run(vuCode, codeSize, vuData, dataSize, gs, memory, maxCycles);
}

extern thread_local bool g_ps2xOnVu1Worker;
void VU1Interpreter::run(uint8_t *vuCode, uint32_t codeSize,
                         uint8_t *vuData, uint32_t dataSize,
                         GS &gs, PS2Memory *memory, uint32_t maxCycles)
{
    m_activeVuData = vuData;
    m_activeVuDataSize = dataSize;
    m_activeGs = &gs;
    m_activeMemory = memory;
    g_vu1LastCode = vuCode;
    g_vu1LastCodeSize = codeSize;

    // Rounding towards zero like the VU. On the MTVU worker it is set permanently -> no libm calls per
    // program (fegetround/fesetround cost ~1.5 % of the VU1 thread). Otherwise as before.
    const unsigned int csrNow = _mm_getcsr();
    const bool alreadyChop = (csrNow & 0x6000u) == 0x6000u && g_ps2xOnVu1Worker;
    const int previousRoundingMode = alreadyChop ? -1 : std::fegetround();
    const bool useVuRounding = alreadyChop ? false : (std::fesetround(FE_TOWARDZERO) == 0);
    const uint64_t budgetEnd = m_cycle + maxCycles;
    bool programEnded = false;
    const bool dumpCover = m_unit == Unit::VU1 && ps2x_vu1dump::dir() && memory && vuCode == memory->getVU1Code();
    std::vector<uint8_t> *coverVec = nullptr; // look up once per run instead of per instruction pair
    // PS2X_VU1_REC=1: vorab uebersetzte Mikroprogramme benutzen (Abdeckungsluecken -> Interpreter)
    static const bool recOn = [] { const char *e = std::getenv("PS2X_VU1_REC"); return e && *e && *e != '0'; }();
    // Recording (dumpCover) and translator together: the interpreter then records exactly the coverage gaps
    if (recOn && !m_compiledDisabled && m_unit == Unit::VU1 && memory && vuCode == memory->getVU1Code())
    {
        const uint64_t gen = memory->getVU1CodeGeneration();
        if (gen != m_recGen)
        {
            m_recGen = gen;
            // Lookup key: two CRC32C chains over 8-byte words (hardware, ~10x faster than FNV per
            // byte; ran on every code upload and cost ~6 % of the VU1 time). The mapping itself goes through signatures (cached).
            uint64_t recHash;
            {
                uint64_t c0 = 0x9E3779B9u, c1 = 0x85EBCA6Bu;
                const uint8_t *q = vuCode; uint32_t k = 0;
                for (; k + 16u <= codeSize; k += 16u, q += 16)
                {
                    uint64_t w0, w1; std::memcpy(&w0, q, 8); std::memcpy(&w1, q + 8, 8);
                    c0 = _mm_crc32_u64(c0, w0); c1 = _mm_crc32_u64(c1, w1);
                }
                recHash = ((c0 << 32) | (c1 & 0xFFFFFFFFu)) ^ (0xC2B2AE3D27D4EB4Full * codeSize) ^ 0x5A5A000000000000ull;
            }
            // Recognition through the bytes of the translated pairs (signature), not the whole micro memory:
            // the same module code with different leftovers in memory is still recognised
            m_recFn = vu1CompiledMatch(vuCode, codeSize, recHash, &m_recEntry);
            m_recSkipPc = ~0u;
            if (!m_recFn)
            {
                // An unknown micro program runs entirely in the interpreter: report once per program
                static std::mutex s_m; static std::set<uint64_t> s_seen;
                std::lock_guard<std::mutex> lk(s_m);
                if (s_seen.insert(recHash).second)
                    std::fprintf(stderr, "[VU1-Rec] unknown program %016llx (interpreter)\n", (unsigned long long)ps2x_vu1dump::fnv(vuCode, codeSize));
            }
        }
        if (m_recFn && m_recFn(*this, vuCode, codeSize, vuData, dataSize, gs, memory, budgetEnd, programEnded) == 0)
            goto vu1_run_epilogue;
    }
    while (m_cycle < budgetEnd && !m_stopRequested)
    {
        commitReadyPipelines();
        if (m_state.pc + 8u > codeSize)
            break;
        // After a fallback (coverage gap) return to the translated code at the next entry instead of computing the
        // rest of the program in the interpreter. Same conditions as the entry in the translated code.
        if (m_recEntry && !m_compiledDisabled && m_state.pc != m_recSkipPc && m_recEntry[(m_state.pc & 0x3FFFu) >> 3] &&
            !m_state.branchPending && !m_state.ebit && !m_state.haltAfterDelaySlot && !hasPendingRegWrites() && !hasPendingOther() && allWritesRetired())
        {
            const uint32_t pcBefore = m_state.pc;
            if (m_recFn(*this, vuCode, codeSize, vuData, dataSize, gs, memory, budgetEnd, programEnded) == 0)
                goto vu1_run_epilogue;
            m_recSkipPc = (m_state.pc == pcBefore) ? pcBefore : ~0u;
            continue;
        }

        const DecodedInstructionPair &decoded = *decodedPairPtr(vuCode, codeSize, memory, m_state.pc);
        if (dumpCover)
        {
            if (!coverVec) coverVec = &ps2x_vu1dump::g_cover[ps2x_vu1dump::g_lastHash];
            if (m_state.pc / 8u < coverVec->size()) (*coverVec)[m_state.pc / 8u] = 1u;
        }
        if (decoded.upperUsage.reserved || decoded.lowerUsage.reserved)
        {
            reportReservedInstruction(decoded.upperUsage.reserved, decoded.upperUsage.reserved ? decoded.upper : decoded.lower);
            break;
        }

        uint64_t readyCycle = calculatePairReadyCycle(decoded);
        while (readyCycle > m_cycle)
        {
            if (readyCycle >= budgetEnd)
            {
                advanceTo(budgetEnd);
                break;
            }
            advanceTo(readyCycle);
            readyCycle = calculatePairReadyCycle(decoded);
        }
        if (m_cycle >= budgetEnd)
            break;

        uint8_t writtenVi = 0u;
        int32_t oldVi = 0;
        for (uint32_t reg = 1; reg < 16u; ++reg)
        {
            if ((decoded.lowerUsage.viWrite & (1u << reg)) != 0u)
            {
                writtenVi = static_cast<uint8_t>(reg);
                oldVi = m_state.vi[reg];
                break;
            }
        }

        const VfAccess upperWrite = decoded.upperUsage.vfWrite;
        const VfAccess lowerWrite = decoded.lowerUsage.vfWrite;
        const bool hasUpperWrite = upperWrite.reg != 0u;
        const bool hasLowerWrite = lowerWrite.reg != 0u && decoded.suppressedLowerVf != lowerWrite.reg;
        const bool hasDistinctLowerWrite = hasLowerWrite && (!hasUpperWrite || lowerWrite.reg != upperWrite.reg);
        float oldUpperVf[4]{};
        float newUpperVf[4]{};
        float oldLowerVf[4]{};
        float newLowerVf[4]{};
        float oldAcc[4]{};
        float newAcc[4]{};
        if (hasUpperWrite)
            std::memcpy(oldUpperVf, m_state.vf[upperWrite.reg], sizeof(oldUpperVf));
        if (hasDistinctLowerWrite)
            std::memcpy(oldLowerVf, m_state.vf[lowerWrite.reg], sizeof(oldLowerVf));
        if (decoded.upperUsage.accWrite != 0u)
            std::memcpy(oldAcc, m_state.acc, sizeof(oldAcc));

        if (decoded.iBit)
        {
            execUpper(decoded.upper);
            float immediate = 0.0f;
            std::memcpy(&immediate, &decoded.lower, sizeof(immediate));
            m_state.i = normalizeOperand(immediate);
        }
        else if (decoded.upperVfShadowReg != 0u)
        {
            float oldVf[4]{};
            float upperVf[4]{};
            std::memcpy(oldVf,
                        m_state.vf[decoded.upperVfShadowReg],
                        sizeof(oldVf));
            execUpper(decoded.upper);
            std::memcpy(upperVf,
                        m_state.vf[decoded.upperVfShadowReg],
                        sizeof(upperVf));
            std::memcpy(m_state.vf[decoded.upperVfShadowReg],
                        oldVf,
                        sizeof(oldVf));
            execLower(decoded.lower, vuData, dataSize, gs, memory, decoded.upper);
            std::memcpy(m_state.vf[decoded.upperVfShadowReg],
                        upperVf,
                        sizeof(upperVf));
        }
        else
        {
            execUpper(decoded.upper);
            execLower(decoded.lower, vuData, dataSize, gs, memory, decoded.upper);
        }

        m_viBranchBackupValid = false;

        if (hasUpperWrite)
        {
            std::memcpy(newUpperVf, m_state.vf[upperWrite.reg], sizeof(newUpperVf));
            std::memcpy(m_state.vf[upperWrite.reg], oldUpperVf, sizeof(oldUpperVf));
            const uint32_t latency =
                decoded.upperUsage.vfLatency != 0u
                    ? decoded.upperUsage.vfLatency
                    : decoded.upperUsage.latency;
            queueVfWrite(upperWrite.reg, upperWrite.lanes, newUpperVf, latency);
        }
        if (hasDistinctLowerWrite)
        {
            std::memcpy(newLowerVf, m_state.vf[lowerWrite.reg], sizeof(newLowerVf));
            std::memcpy(m_state.vf[lowerWrite.reg], oldLowerVf, sizeof(oldLowerVf));
            const uint32_t latency = decoded.lowerUsage.vfLatency != 0u
                                         ? decoded.lowerUsage.vfLatency
                                         : decoded.lowerUsage.latency;
            queueVfWrite(lowerWrite.reg, lowerWrite.lanes, newLowerVf, latency);
        }
        if (decoded.upperUsage.accWrite != 0u)
        {
            std::memcpy(newAcc, m_state.acc, sizeof(newAcc));
            std::memcpy(m_state.acc, oldAcc, sizeof(oldAcc));
            // ACC is forwarded to the next upper instruction. Its arithmetic
            // flags still use the normal four-cycle FMAC timeline.
            queueAccWrite(decoded.upperUsage.accWrite, newAcc,
                          kAccForwardLatency);
        }
        if (writtenVi != 0u)
        {
            const int32_t newVi = m_state.vi[writtenVi];
            m_state.vi[writtenVi] = oldVi;
            const uint32_t latency =
                decoded.lowerUsage.viLatency != 0u
                    ? decoded.lowerUsage.viLatency
                    : decoded.lowerUsage.latency;
            queueViWrite(writtenVi, newVi, latency);
        }

        markPairWrites(decoded);
        if (writtenVi != 0u && decoded.lowerUsage.delaysNextBranchRead)
            recordViWriteForBranch(writtenVi, oldVi);

        m_state.vf[0][0] = 0.0f;
        m_state.vf[0][1] = 0.0f;
        m_state.vf[0][2] = 0.0f;
        m_state.vf[0][3] = 1.0f;
        m_state.vi[0] = 0;

        uint32_t nextPc = m_state.pc + 8u;
        if (nextPc >= codeSize)
            nextPc = 0u;
        m_state.pc = nextPc;

        if (m_state.branchPending)
        {
            if (m_state.branchDelay == 0u)
            {
                m_state.pc = m_state.branchTarget & microAddressMask();
                m_state.branchPending = false;
            }
            else
            {
                --m_state.branchDelay;
            }
        }

        const bool dHalt = decoded.dBit && m_state.dBitEnabled;
        const bool tHalt = decoded.tBit && m_state.tBitEnabled;
        const bool haltBit = dHalt || tHalt;
        const bool haltBranch = haltBit && decoded.lowerUsage.pipeline == PipelineBranch;

        if (m_state.haltAfterDelaySlot)
        {
            m_state.stoppedByD = m_pendingHaltD;
            m_state.stoppedByT = m_pendingHaltT;
            programEnded = true;
        }
        else if (m_state.ebit)
            programEnded = true;
        else if (haltBit && !haltBranch)
        {
            m_state.stoppedByD = dHalt;
            m_state.stoppedByT = tHalt;
            programEnded = true;
        }
        else if (decoded.eBit)
            m_state.ebit = true;
        else if (haltBranch)
        {
            m_state.haltAfterDelaySlot = true;
            m_pendingHaltD = dHalt;
            m_pendingHaltT = tHalt;
        }

        advanceOneCycle();
        if (programEnded)
            break;
    }

vu1_run_epilogue:
    if (programEnded)
    {
        // like flushPipelines(), plus the immediately written results of the translated programs
        while (pipelinesPending() || m_cycle < m_recPend)
        {
            // like advanceTo: nothing happens until the next due time (no XGKICK active) -> jump
            if (!m_xgkick.active)
            {
                uint64_t stop = m_nextCommit;
                if (m_cycle < m_recPend && m_recPend < stop)
                    stop = m_recPend;
                if (stop != UINT64_MAX && stop > m_cycle + 1u)
                    m_cycle = stop - 1u;
            }
            advanceOneCycle();
        }
        m_recPend = 0;
        m_state.ebit = false;
        m_state.haltAfterDelaySlot = false;
        m_pendingHaltD = false;
        m_pendingHaltT = false;
    }
    m_state.cycles = m_cycle;
    if (useVuRounding && previousRoundingMode != -1)
        std::fesetround(previousRoundingMode);
}
