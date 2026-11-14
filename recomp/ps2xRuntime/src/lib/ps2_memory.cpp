#include <cmath>
#include <climits>
#include <mutex>
#include <chrono>
#include <map>
#include <tuple>
#include <memory>
#include <functional>
#include "runtime/ps2_memory.h"
#include "runtime/ps2_address.h"
#include "runtime/gs/gs_frontend.h"
#include "ps2_log.h"
#include <atomic>
extern std::atomic<uint32_t> g_ps2xHblankHz;
#include <cstring>
#include <limits>
#include <stdexcept>
#include <algorithm>
#include <string>
#include <vector>

namespace
{
    // Cache for the GIF_STAT entry in m_ioRegisters (see advanceEeTimers); dropped on clear().
    thread_local const PS2Memory *s_gifOwner = nullptr;
    thread_local uint32_t *s_gifStat = nullptr;
    thread_local uint32_t s_gifRetry = 0;
}


namespace
{
    inline void inRange(uint32_t offset, size_t bytes, size_t regionSize, const char *op, uint32_t address)
    {
        if (static_cast<uint64_t>(offset) + static_cast<uint64_t>(bytes) > static_cast<uint64_t>(regionSize))
        {
            throw std::runtime_error(std::string(op) + " out-of-bounds at address: 0x" + std::to_string(address));
        }
    }

    template <typename T>
    inline T loadScalar(const uint8_t *base, uint32_t offset, size_t regionSize, const char *op, uint32_t address)
    {
        inRange(offset, sizeof(T), regionSize, op, address);
        T value{};
        std::memcpy(&value, base + offset, sizeof(T));
        return value;
    }

    template <typename T>
    inline void storeScalar(uint8_t *base, uint32_t offset, size_t regionSize, T value, const char *op, uint32_t address)
    {
        inRange(offset, sizeof(T), regionSize, op, address);
        std::memcpy(base + offset, &value, sizeof(T));
    }

    inline bool isGsPrivReg(uint32_t addr)
    {
        return Ps2AddressInRange(addr, PS2_GS_PRIV_REG_BASE, PS2_GS_PRIV_REG_SIZE);
    }

    inline bool isIoRegister(uint32_t addr)
    {
        return Ps2AddressInRange(addr, PS2_IO_BASE, PS2_IO_SIZE);
    }

    inline uint64_t *gsRegPtr(GSRegisters &gs, uint32_t addr)
    {
        // Support both 64-bit base offsets and +4 dword aliases.
        uint32_t off = (addr - PS2_GS_PRIV_REG_BASE) & ~0x7u;
        switch (off)
        {
        case 0x0000:
            return &gs.pmode;
        case 0x0010:
            return &gs.smode1;
        case 0x0020:
            return &gs.smode2;
        case 0x0030:
            return &gs.srfsh;
        case 0x0040:
            return &gs.synch1;
        case 0x0050:
            return &gs.synch2;
        case 0x0060:
            return &gs.syncv;
        case 0x0070:
            return &gs.dispfb1;
        case 0x0080:
            return &gs.display1;
        case 0x0090:
            return &gs.dispfb2;
        case 0x00A0:
            return &gs.display2;
        case 0x00B0:
            return &gs.extbuf;
        case 0x00C0:
            return &gs.extdata;
        case 0x00D0:
            return &gs.extwrite;
        case 0x00E0:
            return &gs.bgcolor;
        // CSR (offset 0x1000) is intentionally not handled here: it is
        // std::atomic<uint64_t> and no longer converts to uint64_t*. Callers must
        // check for offset 0x1000 themselves and go through writeCsrHalf/
        // writeCsrFull/gs.csr.load() instead of gsRegPtr().
        case 0x1010:
            return &gs.imr;
        case 0x1040:
            return &gs.busdir;
        case 0x1080:
            return &gs.siglblid;
        default:
            return nullptr;
        }
    }

    constexpr uint32_t kGsCsrRegOffset = 0x1000u;

    // Atomically apply a 32-bit write to one half (off=0 low dword, off=4 high
    // dword) of the GS CSR register. Bits 0..1 of the low dword (SIGNAL/FINISH) are
    // write-one-to-clear; everything else is a plain merge. Uses compare_exchange
    // so the whole read-modify-write is a single atomic step -- this register is
    // also touched by the vsync worker (FIELD bit) and the GIF (SIGNAL/FINISH) on
    // other threads, so a load-then-store here would race with them.
    inline void writeCsrHalf(std::atomic<uint64_t> &csr, uint32_t off, uint32_t value)
    {
        constexpr uint32_t kW1cMask = 0x3u;
        uint64_t expected = csr.load();
        uint64_t desired;
        do
        {
            if (off == 0u)
            {
                uint32_t oldLow = static_cast<uint32_t>(expected & 0xFFFFFFFFull);
                uint32_t mergedLow = (oldLow & kW1cMask) | (value & ~kW1cMask);
                desired = (expected & 0xFFFFFFFF00000000ull) | static_cast<uint64_t>(mergedLow);
                desired &= ~static_cast<uint64_t>(value & kW1cMask);
            }
            else
            {
                uint64_t mask = 0xFFFFFFFFull << (off * 8u);
                desired = (expected & ~mask) | (static_cast<uint64_t>(value) << (off * 8u));
            }
        } while (!csr.compare_exchange_weak(expected, desired));
    }

    // Same as writeCsrHalf but for a full 64-bit CSR write (bits 0..1 are still
    // write-one-to-clear against the current value).
    inline void writeCsrFull(std::atomic<uint64_t> &csr, uint64_t value)
    {
        constexpr uint64_t kW1cMask = 0x3ull;
        uint64_t expected = csr.load();
        uint64_t desired;
        do
        {
            desired = (expected & kW1cMask) | (value & ~kW1cMask);
            desired &= ~(value & kW1cMask);
        } while (!csr.compare_exchange_weak(expected, desired));
    }

    constexpr std::array<uint32_t, 4> kEeTimerBases = {
        0x10000000u,
        0x10000800u,
        0x10001000u,
        0x10001800u,
    };
    constexpr uint32_t kEeTimerCountOffset = 0x00u;
    constexpr uint32_t kEeTimerModeOffset = 0x10u;
    constexpr uint32_t kEeTimerCompareOffset = 0x20u;
    constexpr uint32_t kEeTimerHoldOffset = 0x30u;
    constexpr uint32_t kEeTimerModeClksMask = 0x3u;
    constexpr uint32_t kEeTimerModeConfigMask = 0x3FFu;
    constexpr uint32_t kEeTimerModeStatusMask = 0xC00u;
    constexpr uint32_t kEeTimerModeZret = 1u << 6;
    constexpr uint32_t kEeTimerModeCue = 1u << 7;
    constexpr uint32_t kEeTimerModeCmpe = 1u << 8;
    constexpr uint32_t kEeTimerModeOvfe = 1u << 9;
    constexpr uint32_t kEeTimerModeEquf = 1u << 10;
    constexpr uint32_t kEeTimerModeOvff = 1u << 11;
    constexpr uint64_t kEeClockHz = 294912000ull;
    // The H-blank clock (CLKS=3) follows the video mode: NTSC 15734 Hz, PAL 15625 Hz (SetGsCrt, PS2X_VBLANK_AUTO). Previously fixed NTSC.
    inline uint64_t eeTimerClockHz(uint32_t clks)
    {
        static constexpr uint64_t fixed[3] = {147456000ull, 9216000ull, 576000ull};
        return clks < 3u ? fixed[clks] : static_cast<uint64_t>(g_ps2xHblankHz.load(std::memory_order_relaxed));
    }

    // H-blank clocked EE timers (CLKS=3) are tied to the VSync instead of the estimated cycle count:
    // exactly 262.5 (NTSC) or 312.5 (PAL) lines per VSync. Games measure their frame time with it (TS2: T1);
    // with the cycle estimate the measured frame time varied by +-3 %, in PCSX2 by +-0.1 %.
    // PS2X_TIMER_VSYNC=1 turns the anchor on (diagnosis).
    struct HbAnker { uint64_t vs = ~0ull, cyc = 0u, letzter = 0u; bool init = false; };
    HbAnker g_hb[4];
    // Off by default (measured 10 Nov: game step and weather run as in PCSX2, the anchor changed nothing visible).
    const bool g_hbVsync = [] { const char *e = std::getenv("PS2X_TIMER_VSYNC"); return e && (*e == '1' || *e == '2'); }();
    // PS2X_TIMER_VSYNC=2: additionally count the (estimated) progress within the frame. Default: whole frames only,
    // because the estimate varies by several lines depending on load, and exactly that adds noise to the game's frame time.
    const bool g_hbImBild = [] { const char *e = std::getenv("PS2X_TIMER_VSYNC"); return e && *e == '2'; }();
    inline uint64_t hbZiel(const HbAnker &h, uint64_t clockHz)
    {
        const uint64_t halbe = clockHz >= 15700u ? 525u : 625u;   // Zeilen je zwei Halbbilder
        const uint64_t imBild = g_hbImBild ? std::min<uint64_t>(h.cyc * clockHz / kEeClockHz, halbe / 2u - 1u) : 0u;
        return h.vs * halbe / 2u + imBild;
    }

    inline bool decodeEeTimerRegister(uint32_t address, size_t &timerIndex, uint32_t &offset)
    {
        for (size_t index = 0; index < kEeTimerBases.size(); ++index)
        {
            const uint32_t candidateOffset = address - kEeTimerBases[index];
            if (candidateOffset == kEeTimerCountOffset ||
                candidateOffset == kEeTimerModeOffset ||
                candidateOffset == kEeTimerCompareOffset ||
                (index < 2u && candidateOffset == kEeTimerHoldOffset))
            {
                timerIndex = index;
                offset = candidateOffset;
                return true;
            }
        }
        return false;
    }

    constexpr uint64_t ticksUntilMatch(uint32_t count, uint32_t target)
    {
        const uint32_t distance = (target - count) & 0xFFFFu;
        return distance == 0u ? 0x10000ull : static_cast<uint64_t>(distance);
    }

    struct DmaTagView
    {
        uint16_t qwc = 0;
        uint8_t id = 0;
        bool irq = false;
        uint32_t addr = 0;
        uint32_t upper = 0;
    };

    inline DmaTagView decodeDmaTag(uint64_t tag)
    {
        DmaTagView out{};
        out.qwc = static_cast<uint16_t>(tag & 0xFFFFu);
        out.id = static_cast<uint8_t>((tag >> 28u) & 0x7u);
        out.irq = ((tag >> 31u) & 0x1ull) != 0ull;
        {   // ps2x (TS2): SPR bit 31 = scratchpad (as ps2xDmaAddr / PCSX2 dmaGetAddr)
            const uint32_t raw = static_cast<uint32_t>(tag >> 32u);
            out.addr = ((raw & 0x80000000u) != 0u && !ps2IsScratchpadAddress(raw)) ? (PS2_SCRATCHPAD_BASE + (raw & 0x3FF0u)) : raw;
        }
        out.upper = static_cast<uint32_t>((tag >> 16u) & 0xFFFFu);
        return out;
    }

    inline uint32_t gifTagNloop(uint64_t tagLo)
    {
        return static_cast<uint32_t>(tagLo & 0x7FFFu);
    }

    inline uint8_t gifTagFlg(uint64_t tagLo)
    {
        return static_cast<uint8_t>((tagLo >> 58u) & 0x3u);
    }

    inline uint32_t gifTagNreg(uint64_t tagLo)
    {
        uint32_t nreg = static_cast<uint32_t>((tagLo >> 60u) & 0xFu);
        return nreg == 0u ? 16u : nreg;
    }

}

// Helpers for GS VRAM addressing (PSMCT32 path).
static inline uint32_t gs_vram_offset(uint32_t basePage, uint32_t x, uint32_t y, uint32_t fbw)
{
    // basePage is in 2048-byte units; fbw is in blocks of 64 pixels.
    uint32_t strideBytes = fbw * 64 * 4;
    return basePage * 2048 + y * strideBytes + x * 4;
}

PS2Memory::PS2Memory()
    : m_rdram(nullptr), m_scratchpad(nullptr), iop_ram(nullptr), m_seenGifCopy(false), m_gsVRAM(nullptr)
{
    ps2SetScratchpadHostPtr(nullptr);
}

PS2Memory::~PS2Memory()
{
    if (m_rdram)
    {
        delete[] m_rdram;
        m_rdram = nullptr;
    }

    if (m_scratchpad)
    {
        ps2SetScratchpadHostPtr(nullptr);
        delete[] m_scratchpad;
        m_scratchpad = nullptr;
    }

    if (m_gsVRAM)
    {
        delete[] m_gsVRAM;
        m_gsVRAM = nullptr;
    }

    if (m_vu1Code)
    {
        delete[] m_vu1Code;
        m_vu1Code = nullptr;
    }
    if (m_vu1Data)
    {
        delete[] m_vu1Data;
        m_vu1Data = nullptr;
    }
    if (m_vu0Code)
    {
        delete[] m_vu0Code;
        m_vu0Code = nullptr;
    }
    if (m_vu0Data)
    {
        delete[] m_vu0Data;
        m_vu0Data = nullptr;
    }

    if (iop_ram)
    {
        delete[] iop_ram;
        iop_ram = nullptr;
    }
}

bool PS2Memory::initialize(size_t ramSize)
{
    auto cleanup = [this]()
    {
        delete[] m_rdram;
        delete[] m_scratchpad;
        delete[] iop_ram;
        delete[] m_gsVRAM;
        delete[] m_vu0Code;
        delete[] m_vu0Data;
        delete[] m_vu1Code;
        delete[] m_vu1Data;
        m_rdram = nullptr;
        m_scratchpad = nullptr;
        ps2SetScratchpadHostPtr(nullptr);
        iop_ram = nullptr;
        m_gsVRAM = nullptr;
        m_vu0Code = nullptr;
        m_vu0Data = nullptr;
        m_vu1Code = nullptr;
        m_vu1Data = nullptr;
    };

    cleanup();
    m_seenGifCopy = false;
    m_dmaStartCount.store(0, std::memory_order_relaxed);
    m_gifCopyCount.store(0, std::memory_order_relaxed);
    m_gsWriteCount.store(0, std::memory_order_relaxed);
    m_vifWriteCount.store(0, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(m_completedDmacMutex);
        m_completedDmacCauses.clear();
    }
    m_codeRegions.clear();
    m_path3Masked = false;
    m_path3MaskedFifo.clear();
    m_vif1PendingPath2ImageQwc = 0u;
    m_vif1PendingPath2DirectHl = false;
    resetEeTimers();

    try
    {
        // Allocate main RAM
        m_rdram = new uint8_t[ramSize];
        std::memset(m_rdram, 0, ramSize);

        // Allocate scratchpad
        m_scratchpad = new uint8_t[PS2_SCRATCHPAD_SIZE];
        std::memset(m_scratchpad, 0, PS2_SCRATCHPAD_SIZE);
        ps2SetScratchpadHostPtr(m_scratchpad);

        // Initialize EE TLB entries (R5900 has 48 entries).
        m_tlbEntries.assign(48, TLBEntry{0, 0, 0, false});

        // Allocate IOP RAM
        iop_ram = new uint8_t[2 * 1024 * 1024]; // 2MB

        // Initialize IOP RAM with zeros
        std::memset(iop_ram, 0, 2 * 1024 * 1024);

        // Initialize I/O registers
        m_ioRegisters.clear();
        s_gifOwner = nullptr; // zwischengespeicherten GIF_STAT-Zeiger verwerfen
        s_gifStat = nullptr;

        // Initialize GS registers
        memset(&gs_regs, 0, sizeof(gs_regs));
        // memset zero-fills std::atomic<uint64_t>::csr's bytes, which is not itself
        // a guaranteed-valid atomic store; make the zero-initialization explicit.
        gs_regs.csr.store(0);
        gs_regs.dispfb1 = (0ULL << 0) | (10ULL << 9) | (0ULL << 15) | (0ULL << 32) | (0ULL << 43);
        gs_regs.display1 = (0ULL << 0) | (0ULL << 12) | (0ULL << 23) | (0ULL << 27) | (639ULL << 32) | (447ULL << 44);
        gs_regs.dispfb2 = gs_regs.dispfb1;
        gs_regs.display2 = gs_regs.display1;

        // Allocate GS VRAM (4MB)
        m_gsVRAM = new uint8_t[PS2_GS_VRAM_SIZE];
        std::memset(m_gsVRAM, 0, PS2_GS_VRAM_SIZE);

        m_vu0Code = new uint8_t[PS2_VU0_CODE_SIZE];
        m_vu0Data = new uint8_t[PS2_VU0_DATA_SIZE];
        std::memset(m_vu0Code, 0, PS2_VU0_CODE_SIZE);
        std::memset(m_vu0Data, 0, PS2_VU0_DATA_SIZE);

        m_vu1Code = new uint8_t[PS2_VU1_CODE_SIZE];
        m_vu1Data = new uint8_t[PS2_VU1_DATA_SIZE];
        std::memset(m_vu1Code, 0, PS2_VU1_CODE_SIZE);
        std::memset(m_vu1Data, 0, PS2_VU1_DATA_SIZE);
        markVU0CodeModified();
        markVU1CodeModified();

        // Initialize VIF registers
        memset(&vif0_regs, 0, sizeof(vif0_regs));
        memset(&vif1_regs, 0, sizeof(vif1_regs));

        // Initialize DMA registers
        memset(dma_regs, 0, sizeof(dma_regs));

        return true;
    }
    catch (const std::exception &e)
    {
        std::cerr << "Error initializing PS2 memory: " << e.what() << std::endl;
        cleanup();
        return false;
    }
}

void PS2Memory::resetEeTimers() noexcept
{
    m_eeTimers = {};
}

uint32_t PS2Memory::advanceEeTimers(uint64_t eeCycles) noexcept
{
    if (eeCycles == 0u)
    {
        return 0u;
    }

    constexpr uint32_t kGifStat = 0x10003020u;
    constexpr uint32_t kGifFqcMask = 0x1F000000u;
    // Remember the GIF_STAT entry instead of looking it up in the hash table on every tick.
    // Pointers to unordered_map values stay valid as long as the entry is not erased.
    if (s_gifOwner != this)
    {
        s_gifOwner = this;
        s_gifStat = nullptr;
        s_gifRetry = 0;
    }
    if (!s_gifStat && s_gifRetry-- == 0u)
    {
        auto gifStatIt = m_ioRegisters.find(kGifStat);
        if (gifStatIt != m_ioRegisters.end())
            s_gifStat = &gifStatIt->second;
        s_gifRetry = 255u; // without an entry only look again occasionally
    }
    if (s_gifStat)
        *s_gifStat &= ~kGifFqcMask;

    {   // Testknopf: EE-Zeitgeber schneller/langsamer laufen lassen (Prozent)
        static const uint64_t skala = [] { const char *e = std::getenv("PS2X_TIMER_PROZENT"); return e && *e ? std::strtoull(e, nullptr, 10) : 100ull; }();
        if (skala != 100ull) eeCycles = eeCycles * skala / 100ull;
    }
    uint32_t interruptMask = 0u;
    for (size_t index = 0; index < m_eeTimers.size(); ++index)
    {
        EeTimer &timer = m_eeTimers[index];
        if ((timer.mode & kEeTimerModeCue) == 0u)
        {
            continue;
        }

        const uint64_t clockHz = eeTimerClockHz(timer.mode & kEeTimerModeClksMask);
        uint64_t ticks = 0u;
        if (g_hbVsync && (timer.mode & kEeTimerModeClksMask) == 3u)
        {
            // H-Blank-Zeitgeber am VSync verankern (siehe hbZiel)
            HbAnker &h = g_hb[index];
            const uint64_t vs = gs().vsyncTick.load(std::memory_order_relaxed);
            if (vs != h.vs) { h.vs = vs; h.cyc = 0u; } else h.cyc += eeCycles;
            const uint64_t ziel = hbZiel(h, clockHz);
            if (h.init && ziel > h.letzter) ticks = ziel - h.letzter;
            if (!h.init || ziel > h.letzter) h.letzter = ziel;
            h.init = true;
            timer.clockRemainder = 0u;
        }
        else
        {
            const uint64_t wholeSeconds = eeCycles / kEeClockHz;
            const uint64_t remainingCycles = eeCycles % kEeClockHz;
            const uint64_t scaled = remainingCycles * clockHz + timer.clockRemainder;
            ticks = wholeSeconds * clockHz + scaled / kEeClockHz;
            timer.clockRemainder = scaled % kEeClockHz;
        }
        if (ticks == 0u)
        {
            continue;
        }

        const uint32_t oldCount = timer.count & 0xFFFFu;
        const uint32_t compare = timer.compare & 0xFFFFu;
        const uint64_t compareDistance = ticksUntilMatch(oldCount, compare);
        const uint64_t overflowDistance = 0x10000ull - oldCount;
        const bool zeroReturn = (timer.mode & kEeTimerModeZret) != 0u;
        const bool compareReached = ticks >= compareDistance;
        bool overflowReached = false;

        if (zeroReturn)
        {
            overflowReached = ticks >= overflowDistance && overflowDistance <= compareDistance;
            if (compareReached)
            {
                const uint64_t remaining = ticks - compareDistance;
                timer.count = compare == 0u
                                  ? static_cast<uint32_t>(remaining & 0xFFFFu)
                                  : static_cast<uint32_t>(remaining % compare);
            }
            else
            {
                timer.count = static_cast<uint32_t>((oldCount + ticks) & 0xFFFFu);
            }
        }
        else
        {
            overflowReached = ticks >= overflowDistance;
            timer.count = static_cast<uint32_t>((oldCount + ticks) & 0xFFFFu);
        }

        if (compareReached && (timer.mode & kEeTimerModeCmpe) != 0u && (timer.mode & kEeTimerModeEquf) == 0u)
        {
            timer.mode |= kEeTimerModeEquf;
            interruptMask |= 1u << index;
        }
        if (overflowReached && (timer.mode & kEeTimerModeOvfe) != 0u && (timer.mode & kEeTimerModeOvff) == 0u)
        {
            timer.mode |= kEeTimerModeOvff;
            interruptMask |= 1u << index;
        }
    }
    return interruptMask;
}

uint64_t PS2Memory::cyclesUntilNextEeTimerInterrupt() const noexcept
{
    uint64_t nearest = std::numeric_limits<uint64_t>::max();
    for (const EeTimer &timer : m_eeTimers)
    {
        if ((timer.mode & kEeTimerModeCue) == 0u)
        {
            continue;
        }

        const uint32_t count = timer.count & 0xFFFFu;
        const uint32_t compare = timer.compare & 0xFFFFu;
        const uint64_t compareDistance = ticksUntilMatch(count, compare);
        const uint64_t overflowDistance = 0x10000ull - count;
        uint64_t eventTicks = std::numeric_limits<uint64_t>::max();

        if ((timer.mode & kEeTimerModeCmpe) != 0u &&
            (timer.mode & kEeTimerModeEquf) == 0u)
        {
            eventTicks = compareDistance;
        }
        const bool overflowCanOccur = (timer.mode & kEeTimerModeZret) == 0u ||
                                      overflowDistance <= compareDistance;
        if (overflowCanOccur &&
            (timer.mode & kEeTimerModeOvfe) != 0u &&
            (timer.mode & kEeTimerModeOvff) == 0u)
        {
            eventTicks = std::min(eventTicks, overflowDistance);
        }
        if (eventTicks == std::numeric_limits<uint64_t>::max())
        {
            continue;
        }

        const uint64_t clockHz = eeTimerClockHz(timer.mode & kEeTimerModeClksMask);
        const uint64_t numerator = eventTicks * kEeClockHz - timer.clockRemainder;
        const uint64_t cycles = (numerator + clockHz - 1u) / clockHz;
        nearest = std::min(nearest, std::max<uint64_t>(1u, cycles));
    }
    return nearest;
}


// ps2x (TS2): DMA addresses (MADR/TADR/tag ADDR) carry the SPR flag in bit 31: set = scratchpad, offset addr & 0x3FF0
// (PCSX2 dmaGetAddr). For CPU accesses 0x8000xxxx is KSEG0 RAM instead, so only convert here in the DMA chain.
// Previously bit 31 was masked off: scratchpad references (TS2 display lists) read garbage from low RAM.
static inline uint32_t ps2xDmaAddr(uint32_t raw)
{
    if ((raw & 0x80000000u) != 0u && !ps2IsScratchpadAddress(raw))
        return PS2_SCRATCHPAD_BASE + (raw & 0x3FF0u);
    return raw;
}
bool PS2Memory::isScratchpad(uint32_t address) const
{
    return ps2IsScratchpadAddress(address);
}

uint8_t *PS2Memory::mapVuMemory(uint32_t physAddr, uint32_t size, uint32_t &offset, uint32_t &limit)
{
    return const_cast<uint8_t *>(static_cast<const PS2Memory *>(this)->mapVuMemory(physAddr, size, offset, limit));
}

const uint8_t *PS2Memory::mapVuMemory(uint32_t physAddr, uint32_t size, uint32_t &offset, uint32_t &limit) const
{
    auto mapRange = [&](uint32_t base, uint32_t rangeSize, const uint8_t *ptr) -> const uint8_t *
    {
        if (!ptr || physAddr < base)
        {
            return nullptr;
        }
        const uint32_t local = physAddr - base;
        if (local >= rangeSize || size > (rangeSize - local))
        {
            return nullptr;
        }
        offset = local;
        limit = rangeSize;
        return ptr;
    };

    if (const uint8_t *ptr = mapRange(PS2_VU0_CODE_BASE, PS2_VU0_CODE_SIZE, m_vu0Code))
    {
        return ptr;
    }
    if (const uint8_t *ptr = mapRange(PS2_VU0_DATA_BASE, PS2_VU0_DATA_SIZE, m_vu0Data))
    {
        return ptr;
    }
    if (const uint8_t *ptr = mapRange(PS2_VU1_CODE_BASE, PS2_VU1_CODE_SIZE, m_vu1Code))
    {
        return ptr;
    }
    return mapRange(PS2_VU1_DATA_BASE, PS2_VU1_DATA_SIZE, m_vu1Data);
}

uint32_t PS2Memory::translateAddress(uint32_t virtualAddress)
{
    if (isScratchpad(virtualAddress))
    {
        return ps2ScratchpadOffset(virtualAddress);
    }

    // EE uncached aliases of main RAM (per PS2 memory map):
    //   0x20000000-0x3FFFFFFF -> 32MB mirror of RDRAM
    // This includes the accelerated window rooted at 0x30100000.
    if (Ps2IsUncachedRamMirrorAddress(virtualAddress))
    {
        return virtualAddress & PS2_RAM_MASK;
    }

    // KSEG0/KSEG1 direct-mapped window.
    if (Ps2IsKseg01Address(virtualAddress))
    {
        return Ps2DirectMappedPhysicalAddress(virtualAddress);
    }

    // In this runtime, low segments are treated as physical-style addresses already.
    if (virtualAddress < 0x80000000)
    {
        return virtualAddress;
    }

    // KSEG2/KSEG3 are TLB mapped.
    if (Ps2IsKseg23Address(virtualAddress))
    {
        for (const auto &entry : m_tlbEntries)
        {
            if (entry.valid)
            {
                // PageMask uses bits [24:13]. Build an address-level mask (plus 4KB base page bits).
                const uint32_t mask = entry.mask & 0x01FFE000u;
                const uint32_t compareMask = ~(mask | 0xFFFu);
                if ((virtualAddress & compareMask) == (entry.vpn & compareMask))
                {
                    // TLB hit
                    const uint32_t pageOffsetMask = mask | 0xFFFu;
                    const uint32_t physBase = entry.pfn << 12;
                    return physBase | (virtualAddress & pageOffsetMask);
                }
            }
        }
        throw std::runtime_error("TLB miss for address: 0x" + std::to_string(virtualAddress));
    }

    return virtualAddress;
}

bool PS2Memory::tlbRead(uint32_t index, uint32_t &vpn, uint32_t &pfn, uint32_t &mask, bool &valid) const
{
    if (index >= m_tlbEntries.size())
    {
        return false;
    }

    const TLBEntry &entry = m_tlbEntries[index];
    vpn = entry.vpn;
    pfn = entry.pfn;
    mask = entry.mask;
    valid = entry.valid;
    return true;
}

bool PS2Memory::tlbWrite(uint32_t index, uint32_t vpn, uint32_t pfn, uint32_t mask, bool valid)
{
    if (index >= m_tlbEntries.size())
    {
        return false;
    }

    TLBEntry &entry = m_tlbEntries[index];
    entry.vpn = vpn & 0xFFFFF000u;
    entry.pfn = pfn & 0x000FFFFFu;
    entry.mask = mask & 0x01FFE000u;
    entry.valid = valid;
    return true;
}

int32_t PS2Memory::tlbProbe(uint32_t vpn) const
{
    const uint32_t normalizedVpn = vpn & 0xFFFFF000u;
    for (uint32_t i = 0; i < static_cast<uint32_t>(m_tlbEntries.size()); ++i)
    {
        const TLBEntry &entry = m_tlbEntries[i];
        if (!entry.valid)
        {
            continue;
        }

        const uint32_t mask = entry.mask & 0x01FFE000u;
        const uint32_t compareMask = ~(mask | 0xFFFu);
        if ((normalizedVpn & compareMask) == (entry.vpn & compareMask))
        {
            return static_cast<int32_t>(i);
        }
    }

    return -1;
}


// ---- MTVU (PS2X_MTVU=1): VU1 programs run on their own thread ---------------------------------
// While a VU1 program is running, the EE thread must wait before anything that depends on it or could disturb it:
// GIF-Pakete (Reihenfolge PATH1 vor PATH2/3), VIF1/GIF-Register, VU1-Speicher, GS-Register. Ein einziger
// atomic read access while nothing is running.

// Buffer pool for the DMA data handed to the VU1 worker: the worker returns the buffers instead of
// freeing them (freeing foreign malloc areas collided with the EE: __lll_lock_wait_private in the profile).
namespace ps2x_bufpool
{
    static std::mutex g_m;
    static std::vector<std::vector<uint8_t>> g_pool;
    static std::vector<uint8_t> get()
    {
        std::lock_guard<std::mutex> lk(g_m);
        if (g_pool.empty())
            return {};
        std::vector<uint8_t> v = std::move(g_pool.back());
        g_pool.pop_back();
        v.clear();
        return v;
    }
    static void put(std::vector<uint8_t> &&v)
    {
        if (v.capacity() < 4096u)
            return;
        std::lock_guard<std::mutex> lk(g_m);
        if (g_pool.size() < 32u)
            g_pool.push_back(std::move(v));
    }
}
// Fast append (memcpy) instead of vector::insert: the insert variant copied byte by byte here
// (profile: 8.5 % of the EE thread), plus many reallocations while growing.
static inline void ps2xAppendBytes(std::vector<uint8_t> &v, const uint8_t *p, size_t n)
{
    const size_t o = v.size();
    if (o + n > v.capacity())
        v.reserve(std::max<size_t>(o + n, v.capacity() * 2u));
    v.resize(o + n);
    std::memcpy(v.data() + o, p, n);
}
std::atomic<bool> g_ps2xVu1Pending{false};
thread_local bool g_ps2xOnVu1Worker = false;
void (*g_ps2xVu1Sync)() = nullptr;
void ps2xVu1FenceAddr(uint32_t address)
{
    if (!g_ps2xVu1Pending.load(std::memory_order_acquire) || g_ps2xOnVu1Worker || !g_ps2xVu1Sync)
        return;
    const uint32_t phys = address & 0x1FFFFFFFu;
    if ((phys >= 0x10003000u && phys < 0x10006000u) || (phys >= 0x11000000u && phys < 0x11010000u) ||
        (phys >= 0x12000000u && phys < 0x12002000u))
    {
        static std::atomic<uint64_t> s_n{0};
        static std::atomic<uint32_t> s_hist[16];
        s_hist[(phys >> 12) & 15u].fetch_add(1, std::memory_order_relaxed);
        static std::atomic<uint32_t> s_addr[8];
        static const bool s_dbg = std::getenv("PS2X_MTVU_FENCELOG") != nullptr;
        if (s_dbg && g_ps2xVu1Pending.load(std::memory_order_relaxed))
        {
            const uint64_t k = s_n.load(std::memory_order_relaxed);
            if (k < 8) s_addr[k] = phys;
            if (k < 64) std::fprintf(stderr, "[MTVU-Zaun] %08x\n", phys);
        }
        if ((s_n.fetch_add(1, std::memory_order_relaxed) & (s_dbg ? 0x3Fu : 0x3FFFu)) == (s_dbg ? 0x3Fu : 0x3FFFu))
        {
            std::fprintf(stderr, "[MTVU] address wait points per 4 KB page:");
            for (int i = 0; i < 16; ++i) if (s_hist[i].load()) std::fprintf(stderr, " %x:%u", i, s_hist[i].load());
            std::fprintf(stderr, " (Adresse zuletzt %08x)\n", phys);
        }
        g_ps2xVu1Sync();
    }
}
bool ps2xMtvuEnabled();                      // ps2_runtime.cpp
void ps2xMtvuSubmit(std::function<void()> fn); // ps2_runtime.cpp
void ps2xVu1Fence()
{
    if (g_ps2xVu1Pending.load(std::memory_order_acquire) && !g_ps2xOnVu1Worker && g_ps2xVu1Sync)
    {
        static const bool s_dbg = std::getenv("PS2X_MTVU_FENCELOG") != nullptr;
        static std::atomic<uint32_t> s_k{0};
        if (s_dbg && s_k.fetch_add(1) < 64)
            std::fprintf(stderr, "[MTVU-Zaun] Aufrufer rel %lld\n", (long long)((const char *)__builtin_return_address(0) - (const char *)(void *)&ps2xVu1Fence));
        g_ps2xVu1Sync();
    }
}

uint8_t PS2Memory::read8(uint32_t address)
{
    ps2xVu1FenceAddr(address);
    const bool scratch = isScratchpad(address);
    uint32_t physAddr = translateAddress(address);

    if (scratch)
    {
        return m_scratchpad[physAddr];
    }
    if (physAddr < PS2_RAM_SIZE)
    {
        return m_rdram[physAddr];
    }
    uint32_t vuOffset = 0;
    uint32_t vuLimit = 0;
    if (const uint8_t *vuMem = mapVuMemory(physAddr, sizeof(uint8_t), vuOffset, vuLimit))
    {
        (void)vuLimit;
        return vuMem[vuOffset];
    }
    else if (isIoRegister(physAddr))
    {
        uint32_t regAddr = physAddr & ~0x3;
        uint32_t value = readIORegister(regAddr);
        uint32_t shift = (physAddr & 3) * 8;
        return static_cast<uint8_t>((value >> shift) & 0xFF);
    }

    return 0;
}

uint16_t PS2Memory::read16(uint32_t address)
{
    ps2xVu1FenceAddr(address);
    if (address & 1)
    {
        throw std::runtime_error("Unaligned 16-bit read at address: 0x" + std::to_string(address));
    }

    const bool scratch = isScratchpad(address);
    uint32_t physAddr = translateAddress(address);

    if (scratch)
    {
        return loadScalar<uint16_t>(m_scratchpad, physAddr, PS2_SCRATCHPAD_SIZE, "read16 scratchpad", address);
    }
    if (physAddr < PS2_RAM_SIZE)
    {
        return loadScalar<uint16_t>(m_rdram, physAddr, PS2_RAM_SIZE, "read16 rdram", address);
    }
    uint32_t vuOffset = 0;
    uint32_t vuLimit = 0;
    if (const uint8_t *vuMem = mapVuMemory(physAddr, sizeof(uint16_t), vuOffset, vuLimit))
    {
        return loadScalar<uint16_t>(vuMem, vuOffset, vuLimit, "read16 vu", address);
    }
    else if (isIoRegister(physAddr))
    {
        uint32_t regAddr = physAddr & ~0x3;
        uint32_t value = readIORegister(regAddr);
        uint32_t shift = (physAddr & 2) * 8;
        return static_cast<uint16_t>((value >> shift) & 0xFFFF);
    }

    return 0;
}

uint32_t PS2Memory::read32(uint32_t address)
{
    // MTVU pipeline: do not block VIF1_STAT/GIF_STAT queries ("is the graphics done?"). The values
    // do not come from the worker, and the DMA data is already copied at handover; everything that
    // wirklich VU1-Ergebnisse liest (VU-Speicher, GS-CSR, Register-Schreibungen), wartet weiter.
    static const bool s_pipe = [] { const char *e = std::getenv("PS2X_MTVU_PIPE"); return e && *e == '1'; }();
    const uint32_t physStat = address & 0x1FFFFFFFu;
    if (!(s_pipe && (physStat == 0x10003C00u || physStat == 0x10003020u)))
        ps2xVu1FenceAddr(address);
    if (address & 3)
    {
        throw std::runtime_error("Unaligned 32-bit read at address: 0x" + std::to_string(address));
    }

    if (isGsPrivReg(address))
    {
        uint32_t off = address & 7;
        const uint32_t regOff = (address - PS2_GS_PRIV_REG_BASE) & ~0x7u;
        if (regOff == kGsCsrRegOffset)
        {
            uint64_t val = gs_regs.csr.load();
            return (uint32_t)(val >> (off * 8));
        }
        uint64_t *reg = gsRegPtr(gs_regs, address);
        if (!reg)
            return 0;
        uint64_t val = *reg;
        return (uint32_t)(val >> (off * 8));
    }

    const bool scratch = isScratchpad(address);
    uint32_t physAddr = translateAddress(address);

    if (scratch)
    {
        return loadScalar<uint32_t>(m_scratchpad, physAddr, PS2_SCRATCHPAD_SIZE, "read32 scratchpad", address);
    }
    if (physAddr < PS2_RAM_SIZE)
    {
        return loadScalar<uint32_t>(m_rdram, physAddr, PS2_RAM_SIZE, "read32 rdram", address);
    }
    uint32_t vuOffset = 0;
    uint32_t vuLimit = 0;
    if (const uint8_t *vuMem = mapVuMemory(physAddr, sizeof(uint32_t), vuOffset, vuLimit))
    {
        return loadScalar<uint32_t>(vuMem, vuOffset, vuLimit, "read32 vu", address);
    }
    else if (isIoRegister(physAddr))
    {
        return readIORegister(physAddr);
    }

    return 0;
}

uint64_t PS2Memory::read64(uint32_t address)
{
    ps2xVu1FenceAddr(address);
    if (address & 7)
    {
        throw std::runtime_error("Unaligned 64-bit read at address: 0x" + std::to_string(address));
    }

    if (isGsPrivReg(address))
    {
        const uint32_t regOff = (address - PS2_GS_PRIV_REG_BASE) & ~0x7u;
        if (regOff == kGsCsrRegOffset)
        {
            return gs_regs.csr.load();
        }
        uint64_t *reg = gsRegPtr(gs_regs, address);
        return reg ? *reg : 0;
    }

    const bool scratch = isScratchpad(address);
    uint32_t physAddr = translateAddress(address);

    if (scratch)
    {
        return loadScalar<uint64_t>(m_scratchpad, physAddr, PS2_SCRATCHPAD_SIZE, "read64 scratchpad", address);
    }
    if (physAddr < PS2_RAM_SIZE)
    {
        return loadScalar<uint64_t>(m_rdram, physAddr, PS2_RAM_SIZE, "read64 rdram", address);
    }
    uint32_t vuOffset = 0;
    uint32_t vuLimit = 0;
    if (const uint8_t *vuMem = mapVuMemory(physAddr, sizeof(uint64_t), vuOffset, vuLimit))
    {
        return loadScalar<uint64_t>(vuMem, vuOffset, vuLimit, "read64 vu", address);
    }

    // 64-bit IO read: compose from the two adjacent 32-bit IO register slots
    // to avoid any side-effects from read32 handlers.
    if (isIoRegister(address))
    {
        uint32_t lo = m_ioRegisters.count(address) ? m_ioRegisters[address] : 0u;
        uint32_t hi = m_ioRegisters.count(address + 4) ? m_ioRegisters[address + 4] : 0u;
        return static_cast<uint64_t>(lo) | (static_cast<uint64_t>(hi) << 32);
    }
    return (uint64_t)read32(address) | ((uint64_t)read32(address + 4) << 32);
}

__m128i PS2Memory::read128(uint32_t address)
{
    ps2xVu1FenceAddr(address);
    if (address & 15)
    {
        throw std::runtime_error("Unaligned 128-bit read at address: 0x" + std::to_string(address));
    }

    const bool scratch = isScratchpad(address);
    uint32_t physAddr = translateAddress(address);

    if (scratch)
    {
        inRange(physAddr, sizeof(__m128i), PS2_SCRATCHPAD_SIZE, "read128 scratchpad", address);
        return _mm_loadu_si128(reinterpret_cast<__m128i *>(&m_scratchpad[physAddr]));
    }
    if (physAddr < PS2_RAM_SIZE)
    {
        inRange(physAddr, sizeof(__m128i), PS2_RAM_SIZE, "read128 rdram", address);
        return _mm_loadu_si128(reinterpret_cast<__m128i *>(&m_rdram[physAddr]));
    }
    uint32_t vuOffset = 0;
    uint32_t vuLimit = 0;
    if (const uint8_t *vuMem = mapVuMemory(physAddr, sizeof(__m128i), vuOffset, vuLimit))
    {
        inRange(vuOffset, sizeof(__m128i), vuLimit, "read128 vu", address);
        return _mm_loadu_si128(reinterpret_cast<const __m128i *>(vuMem + vuOffset));
    }

    // 128-bit reads are primarily for quad-word loads in the EE, which are only valid for RAM areas
    // Return zeroes for unsupported areas
    return _mm_setzero_si128();
}

void PS2Memory::write8(uint32_t address, uint8_t value)
{
    ps2xVu1FenceAddr(address);
    const bool scratch = isScratchpad(address);
    uint32_t physAddr = translateAddress(address);

    if (scratch)
    {
        m_scratchpad[physAddr] = value;
    }
    else if (physAddr < PS2_RAM_SIZE)
    {
        m_rdram[physAddr] = value;
    }
    else
    {
        uint32_t vuOffset = 0;
        uint32_t vuLimit = 0;
        if (uint8_t *vuMem = mapVuMemory(physAddr, sizeof(uint8_t), vuOffset, vuLimit))
        {
            (void)vuLimit;
            vuMem[vuOffset] = value;
            if (vuMem == m_vu0Code)
                markVU0CodeModified();
            else if (vuMem == m_vu1Code)
                markVU1CodeModified();
            return;
        }
    }
    if (isIoRegister(physAddr))
    {
        // IO registers - handle byte writes by modifying the appropriate byte in the word
        uint32_t regAddr = physAddr & ~0x3;
        uint32_t shift = (physAddr & 3) * 8;
        uint32_t mask = ~(0xFF << shift);
        uint32_t newValue = (m_ioRegisters[regAddr] & mask) | ((uint32_t)value << shift);
        writeIORegister(regAddr, newValue);
    }
}

void PS2Memory::write16(uint32_t address, uint16_t value)
{
    ps2xVu1FenceAddr(address);
    if (address & 1)
    {
        throw std::runtime_error("Unaligned 16-bit write at address: 0x" + std::to_string(address));
    }

    const bool scratch = isScratchpad(address);
    uint32_t physAddr = translateAddress(address);

    if (scratch)
    {
        storeScalar<uint16_t>(m_scratchpad, physAddr, PS2_SCRATCHPAD_SIZE, value, "write16 scratchpad", address);
    }
    else if (physAddr < PS2_RAM_SIZE)
    {
        storeScalar<uint16_t>(m_rdram, physAddr, PS2_RAM_SIZE, value, "write16 rdram", address);
    }
    else
    {
        uint32_t vuOffset = 0;
        uint32_t vuLimit = 0;
        if (uint8_t *vuMem = mapVuMemory(physAddr, sizeof(uint16_t), vuOffset, vuLimit))
        {
            storeScalar<uint16_t>(vuMem, vuOffset, vuLimit, value, "write16 vu", address);
            if (vuMem == m_vu0Code)
                markVU0CodeModified();
            else if (vuMem == m_vu1Code)
                markVU1CodeModified();
            return;
        }
    }
    if (isIoRegister(physAddr))
    {
        uint32_t regAddr = physAddr & ~0x3;
        uint32_t shift = (physAddr & 2) * 8;
        uint32_t mask = ~(0xFFFF << shift);
        uint32_t newValue = (m_ioRegisters[regAddr] & mask) | ((uint32_t)value << shift);
        writeIORegister(regAddr, newValue);
    }
}

void PS2Memory::write32(uint32_t address, uint32_t value)
{
    ps2xVu1FenceAddr(address);
    if (address & 3)
    {
        throw std::runtime_error("Unaligned 32-bit write at address: 0x" + std::to_string(address));
    }

    if (isGsPrivReg(address))
    {
        uint32_t off = address & 7;
        const uint32_t regOff = (address - PS2_GS_PRIV_REG_BASE) & ~0x7u;
        if (regOff == kGsCsrRegOffset)
        {
            // CSR: bits 0..1 of the low dword are write-one-to-clear status bits.
            // Done as a single atomic RMW -- see writeCsrHalf's comment.
            writeCsrHalf(gs_regs.csr, off, value);
        }
        else if (uint64_t *reg = gsRegPtr(gs_regs, address))
        {
            uint64_t mask = 0xFFFFFFFFULL << (off * 8);
            uint64_t newVal = (*reg & ~mask) | ((uint64_t)value << (off * 8));
            *reg = newVal;
        }
        return;
    }

    const bool scratch = isScratchpad(address);
    uint32_t physAddr = translateAddress(address);

    if (scratch)
    {
        storeScalar<uint32_t>(m_scratchpad, physAddr, PS2_SCRATCHPAD_SIZE, value, "write32 scratchpad", address);
    }
    else if (physAddr < PS2_RAM_SIZE)
    {
        // Check if this might be code modification
        markModified(address, 4);

        storeScalar<uint32_t>(m_rdram, physAddr, PS2_RAM_SIZE, value, "write32 rdram", address);
    }
    else
    {
        uint32_t vuOffset = 0;
        uint32_t vuLimit = 0;
        if (uint8_t *vuMem = mapVuMemory(physAddr, sizeof(uint32_t), vuOffset, vuLimit))
        {
            storeScalar<uint32_t>(vuMem, vuOffset, vuLimit, value, "write32 vu", address);
            if (vuMem == m_vu0Code)
                markVU0CodeModified();
            else if (vuMem == m_vu1Code)
                markVU1CodeModified();
            return;
        }
    }
    if (isIoRegister(physAddr))
    {
        writeIORegister(physAddr, value);
    }
}

void PS2Memory::write64(uint32_t address, uint64_t value)
{
    ps2xVu1FenceAddr(address);
    if (address & 7)
    {
        throw std::runtime_error("Unaligned 64-bit write at address: 0x" + std::to_string(address));
    }

    if (isGsPrivReg(address))
    {
        const uint32_t regOff = (address - PS2_GS_PRIV_REG_BASE) & ~0x7u;
        if (regOff == kGsCsrRegOffset)
        {
            // CSR: bits 0..1 are write-one-to-clear status bits. Done as a single
            // atomic RMW -- see writeCsrFull's comment.
            writeCsrFull(gs_regs.csr, value);
        }
        else if (uint64_t *reg = gsRegPtr(gs_regs, address))
        {
            *reg = value;
        }
        return;
    }

    const bool scratch = isScratchpad(address);
    uint32_t physAddr = translateAddress(address);

    if (scratch)
    {
        storeScalar<uint64_t>(m_scratchpad, physAddr, PS2_SCRATCHPAD_SIZE, value, "write64 scratchpad", address);
    }
    else if (physAddr < PS2_RAM_SIZE)
    {
        markModified(address, 8);
        storeScalar<uint64_t>(m_rdram, physAddr, PS2_RAM_SIZE, value, "write64 rdram", address);
    }
    else
    {
        uint32_t vuOffset = 0;
        uint32_t vuLimit = 0;
        if (uint8_t *vuMem = mapVuMemory(physAddr, sizeof(uint64_t), vuOffset, vuLimit))
        {
            storeScalar<uint64_t>(vuMem, vuOffset, vuLimit, value, "write64 vu", address);
            if (vuMem == m_vu0Code)
                markVU0CodeModified();
            else if (vuMem == m_vu1Code)
                markVU1CodeModified();
            return;
        }
    }
    if (isIoRegister(physAddr))
    {
        write32(address, (uint32_t)value);
        write32(address + 4, (uint32_t)(value >> 32));
    }
}

void PS2Memory::write128(uint32_t address, __m128i value)
{
    ps2xVu1FenceAddr(address);
    if (address & 15)
    {
        throw std::runtime_error("Unaligned 128-bit write at address: 0x" + std::to_string(address));
    }

    const bool scratch = isScratchpad(address);
    uint32_t physAddr = translateAddress(address);

    if (!scratch && physAddr == 0x10004000u) // VIF0_FIFO
    {
        alignas(16) uint8_t fifoData[16];
        _mm_storeu_si128(reinterpret_cast<__m128i *>(fifoData), value);
        processVIF0Data(fifoData, sizeof(fifoData));
        return;
    }
    if (!scratch && physAddr == 0x10005000u) // VIF1_FIFO
    {
        alignas(16) uint8_t fifoData[16];
        _mm_storeu_si128(reinterpret_cast<__m128i *>(fifoData), value);
        processVIF1Data(fifoData, sizeof(fifoData));
        return;
    }

    if (scratch)
    {
        inRange(physAddr, sizeof(__m128i), PS2_SCRATCHPAD_SIZE, "write128 scratchpad", address);
        _mm_storeu_si128(reinterpret_cast<__m128i *>(&m_scratchpad[physAddr]), value);
    }
    else if (physAddr < PS2_RAM_SIZE)
    {
        markModified(address, 16);
        inRange(physAddr, sizeof(__m128i), PS2_RAM_SIZE, "write128 rdram", address);
        _mm_storeu_si128(reinterpret_cast<__m128i *>(&m_rdram[physAddr]), value);
    }
    else
    {
        uint32_t vuOffset = 0;
        uint32_t vuLimit = 0;
        if (uint8_t *vuMem = mapVuMemory(physAddr, sizeof(__m128i), vuOffset, vuLimit))
        {
            inRange(vuOffset, sizeof(__m128i), vuLimit, "write128 vu", address);
            _mm_storeu_si128(reinterpret_cast<__m128i *>(vuMem + vuOffset), value);
            if (vuMem == m_vu0Code)
                markVU0CodeModified();
            else if (vuMem == m_vu1Code)
                markVU1CodeModified();
            return;
        }
    }
    if (isIoRegister(physAddr))
    {
        // Non-RAM 128-bit stores are modeled as two 64-bit stores.
        uint64_t lo = _mm_extract_epi64(value, 0);
        uint64_t hi = _mm_extract_epi64(value, 1);

        write64(address, lo);
        write64(address + 8, hi);
    }
}

namespace
{
    // Lazy VIF1 chain DMA: the PS2 DMAC walks a chain while the EE keeps appending to it
    // (SotC kicks VIF1 and only then writes the rest of the packet). Walking the whole chain at
    // the moment of the CHCR write sees half-built tags, so the walk is deferred until the game
    // looks at the channel again, kicks another DMA, or the frame ends.
    bool g_lazyVif1Enabled = std::getenv("PS2X_LAZY_VIF1_CHAIN") != nullptr;
    bool g_lazyVif1Pending = false;
    bool g_lazyVif1Replay = false;
    uint32_t g_lazyVif1Chcr = 0u;
}

void ps2xFlushLazyVif1Chain(PS2Memory &memory)
{
    if (!g_lazyVif1Pending || g_lazyVif1Replay)
        return;
    g_lazyVif1Pending = false;
    g_lazyVif1Replay = true;
    memory.writeIORegister(0x10009000u, g_lazyVif1Chcr);
    g_lazyVif1Replay = false;
}

bool PS2Memory::writeIORegister(uint32_t address, uint32_t value)
{
    if (g_lazyVif1Enabled && !g_lazyVif1Replay && address >= 0x10009000u && address < 0x10009100u)
    {
        const bool vif1ChainKick = address == 0x10009000u && (value & 0x100u) != 0u && ((value >> 2) & 3u) == 1u;
        if (g_lazyVif1Pending)
            ps2xFlushLazyVif1Chain(*this);
        if (vif1ChainKick)
        {
            g_lazyVif1Pending = true;
            g_lazyVif1Chcr = value;
            m_ioRegisters[0x10009000u] = value; // STR stays set until the deferred walk
            return true;
        }
    }
    size_t timerIndex = 0u;
    uint32_t timerOffset = 0u;
    if (decodeEeTimerRegister(address, timerIndex, timerOffset))
    {
        EeTimer &timer = m_eeTimers[timerIndex];
        switch (timerOffset)
        {
        case kEeTimerCountOffset:
            timer.count = value & 0xFFFFu;
            timer.clockRemainder = 0u;
            break;
        case kEeTimerModeOffset:
        {
            const uint32_t previousMode = timer.mode;
            if (static const bool lg = std::getenv("PS2X_TIMERLOG") != nullptr; lg)
                std::fprintf(stderr, "[TIMER] T%zu MODE=%08X (old %08X count %u)\n", timerIndex, value, previousMode, timer.count);
            const uint32_t status = (previousMode & kEeTimerModeStatusMask) & ~(value & kEeTimerModeStatusMask);
            timer.mode = (value & kEeTimerModeConfigMask) | status;
            if (((previousMode ^ timer.mode) & (kEeTimerModeClksMask | kEeTimerModeCue)) != 0u)
            {
                timer.clockRemainder = 0u;
            }
            break;
        }
        case kEeTimerCompareOffset:
            timer.compare = value & 0xFFFFu;
            break;
        case kEeTimerHoldOffset:
            timer.hold = value & 0xFFFFu;
            break;
        default:
            return false;
        }
        return true;
    }

    if (isGsPrivReg(address))
    {
        // NB: unreachable from write8/16/32/64 today since those all funnel IO
        // register writes through addresses in PS2_IO_BASE's range, which is
        // disjoint from PS2_GS_PRIV_REG_BASE; kept correct for direct callers.
        m_ioRegisters[address] = value;
        const uint32_t off = address & 7u;
        const uint32_t regOff = (address - PS2_GS_PRIV_REG_BASE) & ~0x7u;
        if (regOff == kGsCsrRegOffset)
        {
            writeCsrHalf(gs_regs.csr, off, value);
        }
        else if (uint64_t *reg = gsRegPtr(gs_regs, address))
        {
            const uint64_t mask = 0xFFFFFFFFull << (off * 8u);
            *reg = (*reg & ~mask) | (static_cast<uint64_t>(value) << (off * 8u));
        }
        m_gsWriteCount.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    if (address >= 0x10002000 && address <= 0x10002030)
    {
        if (address == 0x10002010)
        {
            m_ioRegisters[address] = value & ~(1u << 31);
            if (value & (1u << 30))
            {
                m_ioRegisters[0x10002000] = 0;
                m_ioRegisters[0x10002020] = 0;
                m_ioRegisters[0x10002030] = 0;
            }
        }
        else
        {
            m_ioRegisters[address] = value;
        }
        return true;
    }

    if (address == 0x1000E010u)
    {
        const uint32_t current = m_ioRegisters.count(address) ? m_ioRegisters[address] : 0u;
        uint32_t status = current & 0x3FFu;
        uint32_t mask = (current >> 16) & 0x3FFu;

        // D_STAT low bits are W1C status, high bits [16..25] toggle masks on write-one.
        status &= ~(value & 0x3FFu);
        { extern std::atomic<uint32_t> g_ps2xDmacPending; g_ps2xDmacPending.fetch_and(~(value & 0x3FFu)); }
        mask ^= ((value >> 16) & 0x3FFu);

        uint32_t next = (current & ~((0x3FFu) | (0x3FFu << 16) | (1u << 31)));
        next |= status | (mask << 16);
        if ((status & mask) != 0u)
            next |= (1u << 31);
        m_ioRegisters[address] = next;
        return true;
    }

    m_ioRegisters[address] = value;

    if (address >= 0x10003C00u && address < 0x10003E00u)
    {
        m_vifWriteCount.fetch_add(1, std::memory_order_relaxed);

        switch (address)
        {
        case 0x10003C10u:     // VIF1_FBRST
            if (value & 0x1u) // RST
            {
                const bool wasPath3Masked = m_path3Masked;
                std::memset(&vif1_regs, 0, sizeof(vif1_regs));
                m_vif1PendingPath2ImageQwc = 0u;
                m_vif1PendingPath2DirectHl = false;
                m_path3Masked = false;
                if (wasPath3Masked)
                    flushMaskedPath3Packets();
            }
            if (value & 0x8u) // STC
            {
                vif1_regs.stat &= ~((1u << 8) | (1u << 9) | (1u << 10) | (1u << 11) | (1u << 12) | (1u << 13));
            }
            break;
        case 0x10003C30u:
            vif1_regs.mark = value & 0xFFFFu;
            vif1_regs.stat &= ~(1u << 6); // clear MRK flag on CPU write
            break;
        case 0x10003C40u:
            vif1_regs.cycle = value & 0xFFFFu;
            break;
        case 0x10003C50u:
            vif1_regs.mode = value & 0x3u;
            break;
        case 0x10003C60u:
            vif1_regs.num = value & 0xFFu;
            break;
        case 0x10003C70u:
            vif1_regs.mask = value;
            break;
        case 0x10003C80u:
            vif1_regs.code = value;
            break;
        case 0x10003C90u:
            vif1_regs.itops = value & 0x3FFu;
            break;
        case 0x10003CA0u:
            vif1_regs.base = value & 0x3FFu;
            break;
        case 0x10003CB0u:
            vif1_regs.ofst = value & 0x3FFu;
            break;
        case 0x10003CC0u:
            vif1_regs.tops = value & 0x3FFu;
            break;
        case 0x10003CD0u:
            vif1_regs.itop = value & 0x3FFu;
            break;
        case 0x10003CE0u:
            vif1_regs.top = value & 0x3FFu;
            break;
        default:
            break;
        }

        return true;
    }

    if (address >= 0x10003800u && address < 0x10003A00u)
    {
        m_vifWriteCount.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    if (address >= 0x10008000 && address < 0x1000F000)
    {
        if ((address & 0xFF) == 0x00 && (value & 0x100))
        {
            const auto dctrlIt = m_ioRegisters.find(0x1000E000u);
            const bool dmacEnabled = (dctrlIt == m_ioRegisters.end()) || ((dctrlIt->second & 0x1u) != 0u);
            if (!dmacEnabled)
            {
                return true;
            }

            const uint32_t channelBase = address & 0xFFFFFF00;
            const uint32_t madr = ps2xDmaAddr(m_ioRegisters[channelBase + 0x10]);   // SPR-Bit 31
            const uint32_t qwc = m_ioRegisters[channelBase + 0x20];
            m_dmaStartCount.fetch_add(1, std::memory_order_relaxed);

            if (tryProcessScratchpadDma(channelBase, value))
            {
                return true;
            }

            {
                static const bool dmaLog = std::getenv("PS2X_DMALOG") != nullptr;
                static int n = 0;
                if (dmaLog && channelBase == 0x10009000u && n++ < 400)
                    std::fprintf(stderr, "[dmalog] VIF1 CHCR=0x%X madr=0x%08X qwc=%u tadr=0x%08X stat=0x%X\n", value, madr, qwc,
                                 m_ioRegisters[channelBase + 0x30], m_ioRegisters[0x10003C00u]);
            }
            // ps2x (TS2): VIF1 with DIR=0 = memory <- VIF1 FIFO, i.e. a GS download (TRXDIR=1, BUSDIR=1).
            // As PCSX2 (MTGS::InitAndReadFIFO): first hand all open VIF1/GIF packets to the GS, then read,
            // finish the channel with an interrupt. Previously this was treated as an upload and the interrupt never came.
            if (channelBase == 0x10009000u && (value & 0x1u) == 0u && qwc != 0u)
            {
                extern void ps2xMtvuWaitIdle();
                extern bool ps2xGsBridgeReadFifo(uint8_t *dst, uint32_t qwc);
                if (ps2xMtvuEnabled()) ps2xMtvuWaitIdle();
                const uint32_t bytes = qwc * 16u;
                const uint32_t dstAddr = madr & 0x01FFFFF0u;
                // PS2X_DL_JEDES=n (experiment): TS2 reads the whole 512x448 frame back every frame; only do the real
                // read every n-th time and leave the previous picture in memory otherwise (saves a GPU->CPU sync).
                static const uint32_t dlJedes = [] { const char *e = std::getenv("PS2X_DL_JEDES"); const long v = e ? std::atol(e) : 1L; return static_cast<uint32_t>(v < 1 ? 1 : v); }();
                static uint32_t dlZaehler = 0;
                const bool dlEcht = dlJedes <= 1 || (dlZaehler++ % dlJedes) == 0;
                if (dlEcht && dstAddr + bytes <= 0x02000000u)
                {
                    uint8_t *dst = m_rdram + dstAddr;
                    // Asynchronous (default): the game thread waits for the DMA interrupt anyway, which only comes at the
                    // next VBlank (the EeScheduler waits for the ticket there). Saves the EE standstill per frame.
                    extern void *ps2xGsBridgeReadFifoAsync(uint8_t *dst, uint32_t qwc);
                    extern std::atomic<void *> g_ps2xVif1DownloadTicket;
                    if (void *t = ps2xGsBridgeReadFifoAsync(dst, qwc))
                        g_ps2xVif1DownloadTicket.store(t);
                    else if (!ps2xGsBridgeReadFifo(dst, qwc))
                        std::memset(dst, 0, bytes);
                }
                static int s_log = 0;
                static const bool s_dl = std::getenv("PS2X_DLLOG") != nullptr;   // log every 30th download
                if (s_log++ < 8 || (s_dl && s_log % 30 == 0))
                    std::fprintf(stderr, "[ps2x] VIF1-Download #%d (GS->EE) madr=0x%08X qwc=%u D_STAT=0x%08X\n", s_log, madr, qwc, m_ioRegisters[0x1000E010u]);
                m_ioRegisters[channelBase + 0x10] = madr + bytes;
                m_ioRegisters[channelBase + 0x20] = 0u;
                // Like real hardware: the transfer takes time (917 KB ~ a few ms). STR stays set, the channel reports done
                // only at the next VBlank event (EeScheduler). TS2 enables the interrupt only after the start
                // and expects TWO notifications (pending upload + download) -> an immediate done merged the two.
                m_ioRegisters[channelBase] = value | 0x100u;
                { extern std::atomic<bool> g_ps2xVif1DownloadPending; g_ps2xVif1DownloadPending.store(true); }
                return true;
            }

            if ((channelBase == 0x1000A000u || channelBase == 0x10009000u || channelBase == 0x10008000u) && (m_gsVRAM || channelBase == 0x10008000u))
            {
                auto enqueueTransfer = [&](uint32_t srcAddr, uint32_t qwCount)
                {
                    if (qwCount == 0)
                        return;
                    const bool scratch = isScratchpad(srcAddr);
                    PendingTransfer pt;
                    pt.fromScratchpad = scratch;
                    pt.srcAddr = srcAddr;
                    pt.qwc = qwCount;
                    if (channelBase == 0x1000A000u)
                        m_pendingGifTransfers.push_back(pt);
                    else if (channelBase == 0x10009000u)
                        m_pendingVif1Transfers.push_back(pt);
                    else if (channelBase == 0x10008000u)
                        m_pendingVif0Transfers.push_back(pt);
                };

                uint32_t chcr = value;
                uint32_t mode = (chcr >> 2) & 0x3;

                if (mode == 0 && qwc > 0)
                {
                    enqueueTransfer(madr, qwc);
                }
                else if (mode == 1)
                {
                    uint32_t tagAddr = ps2xDmaAddr(m_ioRegisters[channelBase + 0x30]);
                    uint32_t asr0 = ps2xDmaAddr(m_ioRegisters[channelBase + 0x40]);
                    uint32_t asr1 = ps2xDmaAddr(m_ioRegisters[channelBase + 0x50]);
                    uint32_t asp = (chcr >> 4) & 0x3u;
                    const bool tieEnabled = (chcr & (1u << 7)) != 0u;
                    // Real DMAC has no tag limit; SotC frames use ~11.5k tags (4096 dropped every late draw incl. the player).
                    const int kMaxChainTags = 1 << 20;
                    std::vector<uint8_t> chainBuf = ps2xMtvuEnabled() ? ps2x_bufpool::get() : std::vector<uint8_t>{};
                    static thread_local size_t s_chainReserve = 256u * 1024u;
                    chainBuf.reserve(s_chainReserve);

                    auto appendData = [&](uint32_t srcAddr, uint32_t qwCount)
                    {
                        const uint64_t bytes64 = static_cast<uint64_t>(qwCount) * 16ull;
                        uint32_t bytes = (bytes64 > 0xFFFFFFFFull) ? 0xFFFFFFFFu : static_cast<uint32_t>(bytes64);
                        const bool scratch = isScratchpad(srcAddr);
                        uint32_t src = 0;
                        src = translateAddress(srcAddr);
                        const uint8_t *base2;
                        uint32_t maxSz2;
                        if (scratch)
                        {
                            base2 = m_scratchpad;
                            maxSz2 = PS2_SCRATCHPAD_SIZE;
                        }
                        else
                        {
                            base2 = m_rdram;
                            maxSz2 = PS2_RAM_SIZE;
                        }

                        while (bytes > 0)
                        {
                            if (src >= maxSz2)
                                src = 0;
                            uint32_t chunk = bytes;
                            if (src + chunk > maxSz2)
                                chunk = maxSz2 - src;
                            if (chunk == 0)
                                break;
                            ps2xAppendBytes(chainBuf, base2 + src, chunk);
                            bytes -= chunk;
                            src += chunk;
                        }
                    };

                    auto appendVifTagData = [&](uint32_t localTagAddr)
                    {
                        uint32_t tagPhys = 0u;
                        const bool tagScratch = isScratchpad(localTagAddr);
                        tagPhys = translateAddress(localTagAddr);

                        const uint8_t *localBase = tagScratch ? m_scratchpad : m_rdram;
                        const uint32_t localMax = tagScratch ? PS2_SCRATCHPAD_SIZE : PS2_RAM_SIZE;
                        if (tagPhys + 16u > localMax)
                            return;

                        // CHCR.TTE sends the DMAtag's upper 64 bits to the channel before
                        // the tag payload. VIF chains use those bytes for two VIFcodes.
                        ps2xAppendBytes(chainBuf, localBase + tagPhys + 8u, 8u);
                    };

                    const bool isVifChannel =
                        channelBase == 0x10009000u || channelBase == 0x10008000u;
                    const bool transferTagData = isVifChannel && (chcr & 0x40u) != 0u;

                    int tagsProcessed = 0;
                    uint32_t lastTagUpper = (chcr >> 16) & 0xFFFFu;
                    struct SotcTagLog { uint32_t tadr; uint64_t tag; uint32_t v0, v1; };
                    std::vector<SotcTagLog> sotcTags;
                    static const bool sotcTraceVifChainEnv = std::getenv("SOTC_TRACE_VIFCHAIN") != nullptr;
                    const bool sotcTagTrace = channelBase == 0x10009000u && sotcTraceVifChainEnv;
                    bool sotcSuspicious = false;

                    while (tagsProcessed < kMaxChainTags)
                    {
                        const uint32_t currentTagAddr = tagAddr;
                        const bool tagInSPR = isScratchpad(tagAddr);
                        uint32_t physTag = 0;
                        try
                        {
                            physTag = translateAddress(tagAddr);
                        }
                        catch (...)
                        {
                            break;
                        }
                        const uint8_t *tagBase;
                        uint32_t tagMax;
                        if (tagInSPR)
                        {
                            tagBase = m_scratchpad;
                            tagMax = PS2_SCRATCHPAD_SIZE;
                        }
                        else
                        {
                            tagBase = m_rdram;
                            tagMax = PS2_RAM_SIZE;
                        }
                        if (physTag + 16 > tagMax)
                            break;

                        const uint8_t *tp = tagBase + physTag;
                        uint64_t tag = loadScalar<uint64_t>(tp, 0, 16, "dma chain tag", tagAddr);
                        uint16_t tagQwc = static_cast<uint16_t>(tag & 0xFFFF);
                        uint32_t id = static_cast<uint32_t>((tag >> 28) & 0x7);
                        const bool irq = ((tag >> 31) & 0x1ull) != 0ull;
                        uint32_t addr = ps2xDmaAddr(static_cast<uint32_t>(tag >> 32));   // SPR-Bit 31 auswerten (siehe ps2xDmaAddr)
                        lastTagUpper = static_cast<uint32_t>((tag >> 16) & 0xFFFFu);
                        ++tagsProcessed;

                        uint32_t dataAddr = 0;
                        bool hasPayload = (tagQwc > 0);
                        bool endChain = false;

                        switch (id)
                        {
                        case 0:
                            dataAddr = addr;
                            tagAddr = tagAddr + 16;
                            endChain = true;
                            break;
                        case 1:
                            dataAddr = tagAddr + 16;
                            tagAddr = dataAddr + static_cast<uint32_t>(tagQwc) * 16u;
                            break;
                        case 2:
                            dataAddr = tagAddr + 16;
                            tagAddr = addr;
                            break;
                        case 3:
                        case 4:
                            dataAddr = addr;
                            tagAddr = tagAddr + 16;
                            break;
                        case 5:
                            dataAddr = tagAddr + 16;
                            {
                                const uint32_t retAddr = dataAddr + static_cast<uint32_t>(tagQwc) * 16u;
                                if (asp == 0u)
                                {
                                    asr0 = retAddr;
                                    asp = 1u;
                                }
                                else if (asp == 1u)
                                {
                                    asr1 = retAddr;
                                    asp = 2u;
                                }
                            }
                            tagAddr = addr;
                            break;
                        case 6:
                            dataAddr = tagAddr + 16;
                            if (asp == 2u)
                            {
                                tagAddr = asr1;
                                asp = 1u;
                            }
                            else if (asp == 1u)
                            {
                                tagAddr = asr0;
                                asp = 0u;
                            }
                            else
                            {
                                endChain = true;
                            }
                            break;
                        case 7:
                            dataAddr = tagAddr + 16;
                            endChain = true;
                            break;
                        default:
                            hasPayload = false;
                            endChain = true;
                            break;
                        }

                        if (transferTagData)
                            appendVifTagData(currentTagAddr);
                        if (sotcTagTrace)
                        {
                            uint32_t v0 = 0, v1 = 0;
                            std::memcpy(&v0, tp + 8, 4);
                            std::memcpy(&v1, tp + 12, 4);
                            sotcTags.push_back({currentTagAddr, tag, v0, v1});
                            if ((id == 0u || id == 3u || id == 4u) && tagQwc != 0u && (addr < 0x1000u || addr >= 0x02000000u) && (addr & 0x80000000u) == 0u)
                                sotcSuspicious = true;
                        }
                        static const bool sotcTraceVifTagEnv = std::getenv("SOTC_TRACE_VIFTAG") != nullptr;
                        if (channelBase == 0x10009000u && sotcTraceVifTagEnv)
                        {
                            static std::atomic<int> logged{0};
                            uint32_t up0 = 0, up1 = 0;
                            std::memcpy(&up0, tp + 8, 4);
                            std::memcpy(&up1, tp + 12, 4);
                            if (((up0 >> 24) & 0x7Fu) == 0u && (up0 & 0xFFFFu) != 0u && logged.fetch_add(1) < 12)
                                std::fprintf(stderr, "[VIF1 tag] tadr=0x%X id=%u qwc=%u addr=0x%X tte=%d chcr=0x%X vif=%08X %08X tag=%016llX\n",
                                             currentTagAddr, id, tagQwc, addr, (int)transferTagData, chcr, up0, up1, (unsigned long long)tag);
                        }

                        if (hasPayload)
                            appendData(dataAddr, tagQwc);
                        if (irq && tieEnabled)
                            endChain = true;
                        if (endChain)
                            break;
                    }

                    if (sotcTagTrace && chainBuf.size() > (1u << 20))
                        sotcSuspicious = true;   // ps2x (TS2): chain over 1 MB -> print the tags
                    if (sotcSuspicious)
                    {
                        static std::atomic<int> chains{0};
                        if (chains.fetch_add(1) < 3)
                        {
                            std::fprintf(stderr, "[VIF1 chain] start tadr=0x%X madr=0x%X qwc=%u chcr=0x%X tags=%zu\n",
                                         m_ioRegisters[channelBase + 0x30], madr, qwc, value, sotcTags.size());
                            {
                                const uint32_t badTadr = sotcTags.back().tadr;
                                std::fprintf(stderr, "   memory around 0x%X:\n", badTadr);
                                for (uint32_t q = 0; q < 12; ++q)
                                {
                                    const uint32_t a = (badTadr - 0x40u + q * 16u) & (PS2_RAM_SIZE - 1u);
                                    uint32_t w[4];
                                    std::memcpy(w, m_rdram + a, 16);
                                    std::fprintf(stderr, "     %08X: %08X %08X %08X %08X\n", a, w[0], w[1], w[2], w[3]);
                                }
                            }
                            {   // ps2x (TS2): stream offsets per tag (TTE: +8 bytes upper tag half), large tags and the range around 1 MB
                                uint64_t off = 0;
                                for (size_t k = 0; k < sotcTags.size(); ++k)
                                {
                                    const uint32_t q = (unsigned)(sotcTags[k].tag & 0xFFFF);
                                    const uint64_t len = (transferTagData ? 8u : 0u) + (uint64_t)q * 16u;
                                    if (q >= 256u || (off + len > 1000000u && off < 1120000u))
                                        std::fprintf(stderr, "   [off %8llu..%8llu] #%zu tadr=0x%X id=%u qwc=%u addr=0x%X vif=%08X %08X\n",
                                                     (unsigned long long)off, (unsigned long long)(off + len), k, sotcTags[k].tadr,
                                                     (unsigned)((sotcTags[k].tag >> 28) & 7), q, (unsigned)((sotcTags[k].tag >> 32) & 0xFFFFFFFFu),
                                                     sotcTags[k].v0, sotcTags[k].v1);
                                    off += len;
                                }
                                std::fprintf(stderr, "   total stream=%llu bytes, chainBuf=%zu\n", (unsigned long long)off, chainBuf.size());
                            }
                            for (size_t k = 0; k < sotcTags.size(); ++k)
                                if (k < 24 || k + 8 >= sotcTags.size())
                                    std::fprintf(stderr, "   #%zu tadr=0x%X id=%u qwc=%u addr=0x%X vif=%08X %08X\n", k, sotcTags[k].tadr,
                                                 (unsigned)((sotcTags[k].tag >> 28) & 7), (unsigned)(sotcTags[k].tag & 0xFFFF),
                                                 (unsigned)((sotcTags[k].tag >> 32) & 0xFFFFFFFFu), sotcTags[k].v0, sotcTags[k].v1);
                        }
                    }
                    m_ioRegisters[channelBase + 0x30] = tagAddr;
                    m_ioRegisters[channelBase + 0x40] = asr0;
                    m_ioRegisters[channelBase + 0x50] = asr1;
                    chcr = (chcr & ~(0x3u << 4)) | ((asp & 0x3u) << 4);
                    chcr = (chcr & 0x0000FFFFu) | (lastTagUpper << 16);
                    m_ioRegisters[channelBase + 0x00] = chcr;

                    if (chainBuf.size() + chainBuf.size() / 4u > s_chainReserve)
                        s_chainReserve = chainBuf.size() + chainBuf.size() / 4u;
                    if (!chainBuf.empty())
                    {
                        PendingTransfer pt;
                        pt.fromScratchpad = false;
                        pt.srcAddr = 0;
                        pt.qwc = 0;
                        pt.chainData = std::move(chainBuf);
                        if (channelBase == 0x1000A000)
                        {
                            m_pendingGifTransfers.push_back(std::move(pt));
                        }
                        else if (channelBase == 0x10009000u)
                        {
                            m_pendingVif1Transfers.push_back(std::move(pt));
                        }
                        else if (channelBase == 0x10008000u)
                        {
                            m_pendingVif0Transfers.push_back(std::move(pt));
                        }
                    }
                    // else if (channelBase == 0x10009000u)
                    // {

                    // }
                }
                else if (qwc > 0)
                {
                    enqueueTransfer(madr, qwc);
                }

                const bool autoProcessTransfers =
                    (channelBase == 0x1000A000u) ? (m_gifPacketCallback || m_gifArbiter != nullptr) : true;
                if (autoProcessTransfers)
                {
                    processPendingTransfers();
                }
            }
        }
        return true;
    }

    if (address >= 0x10000000 && address < 0x10010000)
    {
        if (address >= 0x10000200 && address < 0x10000300)
        {
            return true;
        }
        if (address >= 0x10000000 && address < 0x10000100)
        {
            return true;
        }
    }

    return false;
}

bool PS2Memory::tryProcessScratchpadDma(uint32_t channelBase, uint32_t chcr)
{
    static constexpr uint32_t kSprFromChannel = 0x1000D000u;
    static constexpr uint32_t kSprToChannel = 0x1000D400u;
    if (channelBase != kSprFromChannel && channelBase != kSprToChannel)
        return false;

    const uint32_t mode = (chcr >> 2u) & 0x3u;
    static const bool sotcTraceSprEnv = std::getenv("SOTC_TRACE_SPR") != nullptr;
    if (sotcTraceSprEnv)
    {
        static std::atomic<int> n{0};
        static std::atomic<uint32_t> modeSeen[2][4];
        const int dir = channelBase == kSprFromChannel ? 0 : 1;
        if (modeSeen[dir][mode].fetch_add(1) < 3 || n.fetch_add(1) < 10)
            std::fprintf(stderr, "[SPR %s] mode=%u chcr=0x%X madr=0x%X sadr=0x%X qwc=%u tadr=0x%X sqwc=0x%X\n",
                         dir == 0 ? "from" : "to", mode, chcr, m_ioRegisters[channelBase + 0x10u], m_ioRegisters[channelBase + 0x80u],
                         m_ioRegisters[channelBase + 0x20u], m_ioRegisters[channelBase + 0x30u], m_ioRegisters[0x1000E030u]);
    }
    if (mode != 0u)
        return false;

    const uint32_t qwc = m_ioRegisters[channelBase + 0x20u] & 0xFFFFu;
    const uint32_t byteCount = qwc * 16u;
    const uint32_t originalMadr = m_ioRegisters[channelBase + 0x10u] & 0x7FFFFFF0u;
    const uint32_t originalSadr = m_ioRegisters[channelBase + 0x80u] & 0x3FF0u;

    uint32_t mainOffset = 0u;
    try
    {
        mainOffset = translateAddress(originalMadr);
    }
    catch (const std::exception &)
    {
        return false;
    }

    if (mainOffset > PS2_RAM_SIZE || byteCount > PS2_RAM_SIZE - mainOffset)
        return false;

    const bool fromScratchpad = channelBase == kSprFromChannel;
    uint32_t scratchOffset = originalSadr;
    uint32_t bytesLeft = byteCount;
    uint32_t copied = 0u;
    while (bytesLeft != 0u)
    {
        const uint32_t scratchChunk = PS2_SCRATCHPAD_SIZE - scratchOffset;
        const uint32_t chunk = std::min(bytesLeft, scratchChunk);
        if (fromScratchpad)
        {
            std::memcpy(m_rdram + mainOffset + copied, m_scratchpad + scratchOffset, chunk);
            markModified(mainOffset + copied, chunk);
        }
        else
        {
            std::memcpy(m_scratchpad + scratchOffset, m_rdram + mainOffset + copied, chunk);
        }

        copied += chunk;
        bytesLeft -= chunk;
        scratchOffset = (scratchOffset + chunk) & (PS2_SCRATCHPAD_SIZE - 1u);
    }

    m_ioRegisters[channelBase + 0x10u] = (originalMadr + byteCount) & 0x7FFFFFF0u;
    m_ioRegisters[channelBase + 0x20u] = 0u;
    m_ioRegisters[channelBase + 0x80u] = (originalSadr + byteCount) & 0x3FF0u;
    completeDmacChannel(channelBase, fromScratchpad ? 8u : 9u);
    return true;
}

void PS2Memory::completeDmacChannel(uint32_t channelBase, uint32_t cause)
{
    static constexpr uint32_t kDStat = 0x1000E010u;
    m_ioRegisters[channelBase] &= ~0x100u;

    uint32_t dstat = m_ioRegisters.count(kDStat) ? m_ioRegisters[kDStat] : 0u;
    dstat |= 1u << cause;
    const uint32_t status = dstat & 0x3FFu;
    const uint32_t mask = (dstat >> 16u) & 0x3FFu;
    if ((status & mask) != 0u)
        dstat |= 1u << 31u;
    else
        dstat &= ~(1u << 31u);
    m_ioRegisters[kDStat] = dstat;
    queueCompletedDmacCause(cause);
}

void PS2Memory::processPendingTransfers()
{
    const bool hadGif = !m_pendingGifTransfers.empty();
    uint32_t observedGifQwc = 0u;
    for (const auto &transfer : m_pendingGifTransfers)
    {
        const uint64_t transferQwc = !transfer.chainData.empty()
                                         ? (transfer.chainData.size() / 16u)
                                         : transfer.qwc;
        observedGifQwc = static_cast<uint32_t>(std::min<uint64_t>(16u, static_cast<uint64_t>(observedGifQwc) + transferQwc));
    }
    if (observedGifQwc != 0u)
    {
        constexpr uint32_t kGifStat = 0x10003020u;
        constexpr uint32_t kGifFqcMask = 0x1F000000u;
        uint32_t &gifStat = m_ioRegisters[kGifStat];
        gifStat = (gifStat & ~kGifFqcMask) | (observedGifQwc << 24u);
    }

    for (size_t idx = 0; idx < m_pendingGifTransfers.size(); ++idx)
    {
        auto &p = m_pendingGifTransfers[idx];
        if (!p.chainData.empty())
        {
            m_seenGifCopy = true;
            m_gifCopyCount.fetch_add(1, std::memory_order_relaxed);
            submitGifPacket(GifPathId::Path3, p.chainData.data(), static_cast<uint32_t>(p.chainData.size()), false);
        }
        else if (p.qwc > 0)
        {
            const uint64_t bytes64 = static_cast<uint64_t>(p.qwc) * 16ull;
            uint32_t sizeBytes = (bytes64 > 0xFFFFFFFFull) ? 0xFFFFFFFFu : static_cast<uint32_t>(bytes64);
            uint32_t srcPhys = 0;
            try
            {
                srcPhys = translateAddress(p.srcAddr);
            }
            catch (const std::exception &)
            {
                continue;
            }
            if (p.fromScratchpad)
            {
                uint32_t bytesLeft = sizeBytes;
                while (bytesLeft >= 16)
                {
                    if (srcPhys >= PS2_SCRATCHPAD_SIZE)
                        srcPhys = 0;
                    uint32_t chunk = bytesLeft;
                    if (srcPhys + chunk > PS2_SCRATCHPAD_SIZE)
                        chunk = PS2_SCRATCHPAD_SIZE - srcPhys;
                    if (chunk == 0)
                        break;
                    m_seenGifCopy = true;
                    m_gifCopyCount.fetch_add(1, std::memory_order_relaxed);
                    submitGifPacket(GifPathId::Path3, m_scratchpad + srcPhys, chunk, false);
                    bytesLeft -= chunk;
                    srcPhys += chunk;
                }
            }
            else
            {
                uint32_t bytesLeft = sizeBytes;
                while (bytesLeft >= 16)
                {
                    if (srcPhys >= PS2_RAM_SIZE)
                        srcPhys = 0;
                    uint32_t chunk = bytesLeft;
                    if (srcPhys + chunk > PS2_RAM_SIZE)
                        chunk = PS2_RAM_SIZE - srcPhys;
                    if (chunk == 0)
                        break;
                    m_seenGifCopy = true;
                    m_gifCopyCount.fetch_add(1, std::memory_order_relaxed);
                    submitGifPacket(GifPathId::Path3, m_rdram + srcPhys, chunk, false);
                    bytesLeft -= chunk;
                    srcPhys += chunk;
                }
            }
        }
    }
    m_pendingGifTransfers.clear();

    const bool hadVif0 = !m_pendingVif0Transfers.empty();
    for (auto &p : m_pendingVif0Transfers)
    {
        if (!p.chainData.empty())
        {
            processVIF0Data(p.chainData.data(), static_cast<uint32_t>(p.chainData.size()));
        }
        else if (p.qwc > 0)
        {
            uint32_t srcPhys = 0;
            const uint64_t bytes64 = static_cast<uint64_t>(p.qwc) * 16ull;
            uint32_t sizeBytes = (bytes64 > 0xFFFFFFFFull) ? 0xFFFFFFFFu : static_cast<uint32_t>(bytes64);
            try
            {
                srcPhys = translateAddress(p.srcAddr);
            }
            catch (const std::exception &)
            {
                continue;
            }
            if (p.fromScratchpad)
            {
                uint32_t bytesLeft = sizeBytes;
                while (bytesLeft > 0)
                {
                    if (srcPhys >= PS2_SCRATCHPAD_SIZE)
                        srcPhys = 0;
                    uint32_t chunk = bytesLeft;
                    if (srcPhys + chunk > PS2_SCRATCHPAD_SIZE)
                        chunk = PS2_SCRATCHPAD_SIZE - srcPhys;
                    if (chunk == 0)
                        break;
                    processVIF0Data(m_scratchpad + srcPhys, chunk);
                    bytesLeft -= chunk;
                    srcPhys += chunk;
                }
            }
            else
            {
                uint32_t bytesLeft = sizeBytes;
                while (bytesLeft > 0)
                {
                    if (srcPhys >= PS2_RAM_SIZE)
                        srcPhys = 0;
                    uint32_t chunk = bytesLeft;
                    if (srcPhys + chunk > PS2_RAM_SIZE)
                        chunk = PS2_RAM_SIZE - srcPhys;
                    if (chunk == 0)
                        break;
                    processVIF0Data(srcPhys, chunk);
                    bytesLeft -= chunk;
                    srcPhys += chunk;
                }
            }
        }
    }
    m_pendingVif0Transfers.clear();

    const bool hadVif1 = !m_pendingVif1Transfers.empty();
    // MTVU: copy the VIF1 data and hand it to the worker thread (unpack + VU1 + GIF there, in order)
    bool vif1HandedOff = false;
    if (hadVif1 && ps2xMtvuEnabled() && !g_ps2xOnVu1Worker)
    {
        auto chunks = std::make_shared<std::vector<std::vector<uint8_t>>>();
        for (auto &p : m_pendingVif1Transfers)
        {
            if (!p.chainData.empty())
            {
                chunks->push_back(std::move(p.chainData));
                continue;
            }
            if (p.qwc == 0)
                continue;
            uint32_t srcPhys = 0;
            try { srcPhys = translateAddress(p.srcAddr); } catch (const std::exception &) { continue; }
            const uint64_t bytes64 = static_cast<uint64_t>(p.qwc) * 16ull;
            uint32_t bytesLeft = (bytes64 > 0xFFFFFFFFull) ? 0xFFFFFFFFu : static_cast<uint32_t>(bytes64);
            const uint8_t *base = p.fromScratchpad ? m_scratchpad : m_rdram;
            const uint32_t limit = p.fromScratchpad ? PS2_SCRATCHPAD_SIZE : PS2_RAM_SIZE;
            std::vector<uint8_t> buf = ps2x_bufpool::get();
            buf.reserve(bytesLeft);
            while (bytesLeft > 0)
            {
                if (srcPhys >= limit) srcPhys = 0;
                uint32_t chunk = std::min(bytesLeft, limit - srcPhys);
                if (chunk == 0) break;
                ps2xAppendBytes(buf, base + srcPhys, chunk);
                bytesLeft -= chunk;
                srcPhys += chunk;
            }
            chunks->push_back(std::move(buf));
        }
        m_pendingVif1Transfers.clear();
        ps2xMtvuSubmit([this, chunks]()
                       {
                           for (auto &c : *chunks)
                           {
                               if (!c.empty()) processVIF1Data(c.data(), static_cast<uint32_t>(c.size()));
                               ps2x_bufpool::put(std::move(c));
                           }
                           if (m_gifArbiter)
                               m_gifArbiter->drain(); });
        vif1HandedOff = true;
    }
    for (auto &p : m_pendingVif1Transfers)
    {
        if (!p.chainData.empty())
        {
            processVIF1Data(p.chainData.data(), static_cast<uint32_t>(p.chainData.size()));
        }
        else if (p.qwc > 0)
        {
            uint32_t srcPhys = 0;
            const uint64_t bytes64 = static_cast<uint64_t>(p.qwc) * 16ull;
            uint32_t sizeBytes = (bytes64 > 0xFFFFFFFFull) ? 0xFFFFFFFFu : static_cast<uint32_t>(bytes64);
            try
            {
                srcPhys = translateAddress(p.srcAddr);
            }
            catch (const std::exception &)
            {
                continue;
            }
            if (p.fromScratchpad)
            {
                uint32_t bytesLeft = sizeBytes;
                while (bytesLeft > 0)
                {
                    if (srcPhys >= PS2_SCRATCHPAD_SIZE)
                        srcPhys = 0;
                    uint32_t chunk = bytesLeft;
                    if (srcPhys + chunk > PS2_SCRATCHPAD_SIZE)
                        chunk = PS2_SCRATCHPAD_SIZE - srcPhys;
                    if (chunk == 0)
                        break;
                    processVIF1Data(m_scratchpad + srcPhys, chunk);
                    bytesLeft -= chunk;
                    srcPhys += chunk;
                }
            }
            else
            {
                uint32_t bytesLeft = sizeBytes;
                while (bytesLeft > 0)
                {
                    if (srcPhys >= PS2_RAM_SIZE)
                        srcPhys = 0;
                    uint32_t chunk = bytesLeft;
                    if (srcPhys + chunk > PS2_RAM_SIZE)
                        chunk = PS2_RAM_SIZE - srcPhys;
                    if (chunk == 0)
                        break;
                    processVIF1Data(srcPhys, chunk);
                    bytesLeft -= chunk;
                    srcPhys += chunk;
                }
            }
        }
    }
    m_pendingVif1Transfers.clear();

    extern bool ps2xMtvuDefer(std::function<void()> fn);
    if (!vif1HandedOff && !(m_gifArbiter && ps2xMtvuDefer([this]() { m_gifArbiter->drain(); }))) // otherwise the worker drains the GIF queue
    {
        ps2xVu1Fence();
        if (m_gifArbiter)
            m_gifArbiter->drain();
    }

    static constexpr uint32_t GIF_CHANNEL = 0x1000A000;
    static constexpr uint32_t VIF0_CHANNEL = 0x10008000;
    static constexpr uint32_t VIF1_CHANNEL = 0x10009000;
    static constexpr uint32_t D_STAT = 0x1000E010u;

    auto raiseDStatChannel = [&](uint32_t channelBit)
    {
        uint32_t dstat = m_ioRegisters.count(D_STAT) ? m_ioRegisters[D_STAT] : 0u;
        dstat |= (1u << channelBit);

        const uint32_t status = dstat & 0x3FFu;
        const uint32_t mask = (dstat >> 16) & 0x3FFu;
        if ((status & mask) != 0u)
            dstat |= (1u << 31);
        else
            dstat &= ~(1u << 31);

        m_ioRegisters[D_STAT] = dstat;
    };

    if (hadGif)
    {
        raiseDStatChannel(2u); // GIF channel
        queueCompletedDmacCause(2u);
        m_ioRegisters[GIF_CHANNEL + 0x00] &= ~0x100u;
        m_ioRegisters[GIF_CHANNEL + 0x20] = 0;
    }
    if (hadVif0)
    {
        raiseDStatChannel(0u); // VIF0 channel
        queueCompletedDmacCause(0u);
        m_ioRegisters[VIF0_CHANNEL + 0x00] &= ~0x100u;
        m_ioRegisters[VIF0_CHANNEL + 0x20] = 0;
    }
    if (hadVif1)
    {
        raiseDStatChannel(1u); // VIF1 channel
        queueCompletedDmacCause(1u);
        m_ioRegisters[VIF1_CHANNEL + 0x00] &= ~0x100u;
        m_ioRegisters[VIF1_CHANNEL + 0x20] = 0;
    }
}

void PS2Memory::queueCompletedDmacCause(uint32_t cause)
{
    std::lock_guard<std::mutex> lock(m_completedDmacMutex);
    m_completedDmacCauses.push_back(cause);
}

std::vector<uint32_t> PS2Memory::consumeCompletedDmacCauses()
{
    std::lock_guard<std::mutex> lock(m_completedDmacMutex);
    std::vector<uint32_t> causes;
    causes.swap(m_completedDmacCauses);
    return causes;
}

void PS2Memory::flushMaskedPath3Packets(bool drainImmediately)
{
    ps2xVu1Fence();
    if (m_path3Masked || m_path3MaskedFifo.empty())
        return;

    auto emit = [&](const uint8_t *packetData, uint32_t packetSize)
    {
        if (m_gifArbiter)
            m_gifArbiter->submit(GifPathId::Path3, packetData, packetSize, false);
        else if (m_gifPacketCallback)
            m_gifPacketCallback(packetData, packetSize);
    };

    for (const auto &packet : m_path3MaskedFifo)
    {
        if (packet.size() >= 16u)
            emit(packet.data(), static_cast<uint32_t>(packet.size()));
    }
    m_path3MaskedFifo.clear();

    if (m_gifArbiter && drainImmediately)
        m_gifArbiter->drain();
}

std::atomic<uint64_t> g_ps2xPath1Packets{0}; // Bildraten-Messung
// PS2X_GIFSTAT=1 (TS2 diagnosis): count per GIF path which primitives (PRIM type, FST/TME/ABE) are drawn
static void ps2xGifStat(int path, const uint8_t *data, uint32_t size)
{
    static const bool on = [] { const char *e = std::getenv("PS2X_GIFSTAT"); return e && *e == '1'; }();
    if (!on) return;
    static std::mutex m; static std::map<std::pair<int, uint32_t>, uint64_t> cnt; static auto last = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> l(m);
    uint32_t o = 0;
    while (o + 16 <= size)
    {
        uint64_t lo, hi; std::memcpy(&lo, data + o, 8); std::memcpy(&hi, data + o + 8, 8); o += 16;
        const uint32_t nloop = lo & 0x7FFF, pre = (lo >> 46) & 1, prim = (lo >> 47) & 0x7FF, flg = (lo >> 58) & 3;
        uint32_t nreg = (lo >> 60) & 0xF; if (!nreg) nreg = 16;
        if (pre && flg == 0) ++cnt[{path, prim}];
        if (flg == 0)
        {
            for (uint32_t i = 0; i < nloop; ++i)
                for (uint32_t r = 0; r < nreg; ++r, o += 16)
                {
                    if (o + 16 > size) break;
                    const uint32_t reg = (hi >> (4 * r)) & 0xF;
                    if (reg == 0xE) { uint64_t dlo, dhi; std::memcpy(&dlo, data + o, 8); std::memcpy(&dhi, data + o + 8, 8); if ((dhi & 0xFF) == 0) ++cnt[{path, static_cast<uint32_t>(dlo & 0x7FF)}]; }
                }
        }
        else if (flg == 1) o += ((nloop * nreg + 1) / 2) * 16;
        else o += nloop * 16;
        if (lo & 0x8000) break;
    }
    const auto now = std::chrono::steady_clock::now();
    if (now - last > std::chrono::seconds(5))
    {
        last = now;
        std::fprintf(stderr, "[GIFSTAT]");
        for (const auto &[k, v] : cnt) std::fprintf(stderr, " P%d:typ%u%s%s%s=%llu", k.first, k.second & 7, (k.second & 0x10) ? "+TME" : "", (k.second & 0x100) ? "+FST" : "", (k.second & 0x40) ? "+ABE" : "", (unsigned long long)v);
        std::fprintf(stderr, "\n");
        cnt.clear();
    }
}

// TS2 16:9: keep HUD and menus in 4:3 proportion. Both come (like the world) through PATH1, but as 2D primitives with
// fixed texture coordinates (PRIM.FST=1); the 3D world uses STQ. FST packets are squeezed to 0.75 around the screen centre
// (like a 4:3 picture centred in the 16:9 picture), so circles stay round and composite graphics (logo) stay intact.
// Excluded: full-screen 2D (>= 560 px wide: menu backgrounds) and tall strips (>= 360 px high: frame copies for
// effects such as blur/fog that copy the already drawn frame back strip by strip).
// TS2_HUD43=0 schaltet es ab.
// Remember the render targets (FRAME_1/2.FBP from A+D, all GIF paths): a texture inside one of them is a
// frame copy (blur, split-screen strips) and not HUD. TBP0 (64 words) vs. FBP (2048 words) -> FBP*32.
static uint32_t g_ps2xFbSeen[8] = {~0u, ~0u, ~0u, ~0u, ~0u, ~0u, ~0u, ~0u}; // FBP | FBW << 16
static void ps2xFbTrack(const uint8_t *data, uint32_t size)
{
    uint32_t o = 0;
    while (o + 16 <= size)
    {
        uint64_t lo, hi; std::memcpy(&lo, data + o, 8); std::memcpy(&hi, data + o + 8, 8); o += 16;
        const uint32_t nloop = lo & 0x7FFF, flg = (lo >> 58) & 3; uint32_t nreg = (lo >> 60) & 0xF; if (!nreg) nreg = 16;
        if (flg == 0)
        {
            for (uint32_t i = 0; i < nloop; ++i)
                for (uint32_t r = 0; r < nreg; ++r, o += 16)
                {
                    if (o + 16 > size) return;
                    if (((hi >> (4 * r)) & 0xF) != 0xE) continue;
                    const uint32_t ad = data[o + 8];
                    if (ad != 0x4C && ad != 0x4D) continue;
                    uint64_t dlo; std::memcpy(&dlo, data + o, 8); const uint32_t fbp = (dlo & 0x1FF) | (((dlo >> 16) & 0x3F) << 16);
                    bool da = false; for (uint32_t k : g_ps2xFbSeen) da |= k == fbp;
                    if (!da) { static uint32_t n = 0; g_ps2xFbSeen[n++ & 7] = fbp; }
                }
        }
        else if (flg == 1) o += ((nloop * nreg + 1) / 2) * 16;
        else o += nloop * 16;
        if (lo & 0x8000) break;
    }
}
static bool ps2xTexIstBild(uint32_t tbp)
{
    for (uint32_t k : g_ps2xFbSeen)
        if (k != ~0u && tbp >= (k & 0x1FF) * 32u && tbp < (k & 0x1FF) * 32u + (k >> 16) * 448u) return true;   // FBW*64 x 448 px, 32 Bit
    return false;
}

// TS2_GROSSLOG=<tick,tick,...>: diagnosis for issue #3 (grey smoke wall). For the listed VBlank ticks, sums up per
// texture/blend state the large alpha-blended textured primitives of PATH1 (count, biggest box, one sample).
static void ps2xGrossLog(const uint8_t *data, uint32_t size, uint64_t tick)
{
    static const std::vector<uint64_t> ticks = [] { std::vector<uint64_t> v; const char *e = std::getenv("TS2_GROSSLOG");
        while (e && *e) { char *end; const unsigned long long t = std::strtoull(e, &end, 10); if (end == e) break; v.push_back(t); e = *end ? end + 1 : end; } return v; }();
    if (ticks.empty()) return;
    struct Sum { uint32_t n = 0; int32_t w = 0, h = 0; std::string probe; };
    static std::map<std::tuple<uint32_t, uint64_t, uint64_t, uint64_t, uint64_t>, Sum> sum; static uint64_t akt = ~0ull;
    if (tick != akt)
    {
        if (!sum.empty())
        {
            for (const auto &[k, v] : sum)
            {
                const uint64_t tex0 = std::get<1>(k);
                std::fprintf(stderr, "[GROSS] t=%llu prim=%x TBP=%x PSM=%x TW=%u TH=%u TCC=%u TFX=%u CBP=%x CLAMP=%llx ALPHA=%llx TEST=%llx n=%u max %dx%d%s\n",
                             (unsigned long long)akt, std::get<0>(k), (unsigned)(tex0 & 0x3FFF), (unsigned)((tex0 >> 20) & 0x3F), (unsigned)((tex0 >> 26) & 0xF),
                             (unsigned)((tex0 >> 30) & 0xF), (unsigned)((tex0 >> 34) & 1), (unsigned)((tex0 >> 35) & 3), (unsigned)((tex0 >> 37) & 0x3FFF),
                             (unsigned long long)std::get<4>(k), (unsigned long long)std::get<2>(k), (unsigned long long)std::get<3>(k), v.n, v.w, v.h, v.probe.c_str());
            }
            sum.clear();
        }
        akt = tick;
    }
    if (std::find(ticks.begin(), ticks.end(), tick) == ticks.end()) return;
    static uint32_t prim = 0; static uint64_t tex0 = 0, clamp = 0, alpha = 0, test = 0;
    struct V { int32_t x, y; float s, t, q; uint32_t rgba; };
    static V vv[3]; static int nv = 0; static float cs = 0, ct = 0, cq = 1; static uint32_t crgba = 0;
    auto kick = [&](int32_t x, int32_t y, bool adc) {
        vv[0] = vv[1]; vv[1] = vv[2]; vv[2] = V{x, y, cs, ct, cq, crgba}; ++nv;
        const uint32_t typ = prim & 7; const int need = typ == 6 ? 2 : 3;
        if (adc || typ < 3 || nv < need || !(prim & 0x40) || !(prim & 0x10)) return;
        if (typ == 3 && nv % 3) return; if (typ == 6 && nv % 2) return;
        int32_t x0 = vv[2].x, x1 = x0, y0 = vv[2].y, y1 = y0;
        for (int i = 3 - need; i < 3; ++i) { x0 = std::min(x0, vv[i].x); x1 = std::max(x1, vv[i].x); y0 = std::min(y0, vv[i].y); y1 = std::max(y1, vv[i].y); }
        const int32_t w = (x1 - x0) / 16, h = (y1 - y0) / 16;
        if (w < 100 || h < 80) return;
        Sum &e = sum[{prim, tex0, alpha, test, clamp}]; ++e.n;
        if (w * h > e.w * e.h)
        {
            e.w = w; e.h = h; char buf[400]; int o2 = 0;
            for (int i = 3 - need; i < 3; ++i)
                o2 += std::snprintf(buf + o2, sizeof(buf) - o2, " | xy %d,%d st %.3f,%.3f q %.4f rgba %08x", vv[i].x / 16, vv[i].y / 16, vv[i].s, vv[i].t, vv[i].q, vv[i].rgba);
            e.probe = buf;
        }
    };
    uint32_t o = 0;
    while (o + 16 <= size)
    {
        uint64_t lo, hi; std::memcpy(&lo, data + o, 8); std::memcpy(&hi, data + o + 8, 8); o += 16;
        const uint32_t nloop = lo & 0x7FFF, flg = (lo >> 58) & 3; uint32_t nreg = (lo >> 60) & 0xF; if (!nreg) nreg = 16;
        if ((lo >> 46) & 1) { prim = (lo >> 47) & 0x7FF; nv = 0; }
        if (flg == 0)
        {
            for (uint32_t i = 0; i < nloop; ++i)
                for (uint32_t r = 0; r < nreg; ++r, o += 16)
                {
                    if (o + 16 > size) return;
                    const uint32_t reg = (hi >> (4 * r)) & 0xF;
                    uint64_t v; std::memcpy(&v, data + o, 8);
                    if (reg == 0x0) { prim = v & 0x7FF; nv = 0; }
                    else if (reg == 0x1) { crgba = data[o] | (data[o + 4] << 8) | (data[o + 8] << 16) | (static_cast<uint32_t>(data[o + 12]) << 24); }
                    else if (reg == 0x2) { std::memcpy(&cs, data + o, 4); std::memcpy(&ct, data + o + 4, 4); std::memcpy(&cq, data + o + 8, 4); }
                    else if (reg == 0x6) tex0 = v;
                    else if (reg == 0x8) clamp = v;
                    else if (reg == 0x4 || reg == 0x5) { uint16_t x, y; std::memcpy(&x, data + o, 2); std::memcpy(&y, data + o + 4, 2); kick(x, y, (data[o + 13] & 0x80) != 0); }
                    else if (reg == 0xE)
                    {
                        const uint32_t ad = data[o + 8];
                        if (ad == 0x00) { prim = v & 0x7FF; nv = 0; }
                        else if (ad == 0x01) { crgba = static_cast<uint32_t>(v); std::memcpy(&cq, data + o + 4, 4); }
                        else if (ad == 0x02) { std::memcpy(&cs, data + o, 4); std::memcpy(&ct, data + o + 4, 4); }
                        else if (ad == 0x06) tex0 = v;
                        else if (ad == 0x08) clamp = v;
                        else if (ad == 0x42) alpha = v;
                        else if (ad == 0x47) test = v;
                        else if (ad == 0x04 || ad == 0x05) kick(static_cast<int32_t>(v & 0xFFFF), static_cast<int32_t>((v >> 16) & 0xFFFF), false);
                        else if (ad == 0x0C || ad == 0x0D) kick(static_cast<int32_t>(v & 0xFFFF), static_cast<int32_t>((v >> 16) & 0xFFFF), true);
                    }
                }
        }
        else if (flg == 1) o += ((nloop * nreg + 1) / 2) * 16;
        else o += nloop * 16;
        if (lo & 0x8000) break;
    }
}

// TS2_BLENDLOG=1: diagnosis for trails/ghost images. Logs sprites with texture + alpha blending whose
// texture lies in a render target (frame copy): path, FRAME.FBP, TEX0.TBP0, ALPHA, width/height in pixels.
static void ps2xBlendLog(int path, const uint8_t *data, uint32_t size)
{
    static const bool on = std::getenv("TS2_BLENDLOG") != nullptr;
    if (!on) return;
    // TS2_BLENDLOG_AB=s: start logging only s seconds after the first packet (e.g. while zooming in a level)
    {
        static const int64_t ab = [] { const char *e = std::getenv("TS2_BLENDLOG_AB"); return e ? std::atoll(e) * 1000LL : 0LL; }();
        static const auto t0 = std::chrono::steady_clock::now();
        if (ab > 0 && std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count() < ab) return;
    }
    static uint32_t fbp = 0, tbp = 0, prim = 0, nlog = 0; static uint64_t alpha = 0, tex0 = 0;
    static int32_t x0 = 0, y0 = 0; static int par = 0; static uint32_t cu = 0, cv = 0, u0 = 0, v0 = 0;
    // In addition: count per frame all textured primitives (any type) whose texture lies in a render target
    static std::map<std::tuple<uint32_t, uint32_t, uint32_t, uint32_t, uint64_t>, uint32_t> zaehl; static uint32_t pakete = 0;
    if (++pakete % 3000 == 0 && !zaehl.empty())
    {
        for (const auto &[k, n] : zaehl)
            std::fprintf(stderr, "[BLENDSUM] P%u prim=%x FBP=%x TBP=%x ALPHA=%llx Ecken=%u\n", std::get<0>(k), std::get<1>(k), std::get<2>(k), std::get<3>(k),
                         (unsigned long long)std::get<4>(k), n);
        zaehl.clear();
    }
    auto vert = [&](int32_t x, int32_t y) {
        if ((prim & 0x10) && ps2xTexIstBild(tbp)) ++zaehl[{static_cast<uint32_t>(path), prim & 0x7FF, fbp & 0x1FF, tbp, (prim & 0x40) ? alpha : 0}];
        if ((prim & 7) != 6 || !(prim & 0x10)) { par = 0; return; }
        if (par == 0) { x0 = x; y0 = y; u0 = cu; v0 = cv; par = 1; return; }
        par = 0;
        if (!ps2xTexIstBild(tbp)) return;
        const int32_t w = std::abs(x - x0) / 16, h = std::abs(y - y0) / 16;
        if (w < 32 || h < 32) return;
        if (++nlog > 300) return;
        std::fprintf(stderr, "[BLEND] P%d FBP=%x TBP=%x (FB*32=%x) TEX0=%llx ALPHA=%llx prim=%x %dx%d @%d,%d-%d,%d UV %u,%u-%u,%u\n", path, fbp & 0x1FF, tbp,
                     (fbp & 0x1FF) * 32u, (unsigned long long)tex0, (unsigned long long)alpha, prim, w, h, x0 / 16, y0 / 16, x / 16, y / 16,
                     u0 / 16, v0 / 16, cu / 16, cv / 16);
    };
    uint32_t o = 0;
    while (o + 16 <= size)
    {
        uint64_t lo, hi; std::memcpy(&lo, data + o, 8); std::memcpy(&hi, data + o + 8, 8); o += 16;
        const uint32_t nloop = lo & 0x7FFF, flg = (lo >> 58) & 3; uint32_t nreg = (lo >> 60) & 0xF; if (!nreg) nreg = 16;
        if ((lo >> 46) & 1) { prim = (lo >> 47) & 0x7FF; par = 0; }
        if (flg == 0)
        {
            for (uint32_t i = 0; i < nloop; ++i)
                for (uint32_t r = 0; r < nreg; ++r, o += 16)
                {
                    if (o + 16 > size) return;
                    const uint32_t reg = (hi >> (4 * r)) & 0xF;
                    uint64_t v; std::memcpy(&v, data + o, 8);
                    if (reg == 0x6) { tex0 = v; tbp = v & 0x3FFF; }
                    else if (reg == 0x3) { cu = v & 0x3FFF; cv = (v >> 32) & 0x3FFF; }
                    else if (reg == 0x4 || reg == 0x5) { uint16_t x, y; std::memcpy(&x, data + o, 2); std::memcpy(&y, data + o + 4, 2); vert(x, y); }
                    else if (reg == 0xE)
                    {
                        const uint32_t ad = data[o + 8];
                        if (ad == 0x00) { prim = v & 0x7FF; par = 0; }
                        else if (ad == 0x06) { tex0 = v; tbp = v & 0x3FFF; }
                        else if (ad == 0x4C) fbp = v & 0x1FF;
                        else if (ad == 0x42) alpha = v;
                        else if (ad == 0x03) { cu = v & 0x3FFF; cv = (v >> 16) & 0x3FFF; }
                        else if (ad == 0x04 || ad == 0x05) vert(static_cast<int32_t>(v & 0xFFFF), static_cast<int32_t>((v >> 16) & 0xFFFF));
                    }
                }
        }
        else if (flg == 1) o += ((nloop * nreg + 1) / 2) * 16;
        else o += nloop * 16;
        if (lo & 0x8000) break;
    }
}

// TS2 motion trail: every frame the game blends triangle strips (PRIM 0x15C: TME+ABE+FST, ALPHA 0x44 = (Cs-Cd)*As+Cd)
// with the OTHER frame buffer (= previous frame) as texture over the new one -> ghost images when turning (the same in PCSX2,
// so original behaviour). With the menu option "motion trail" off (default) these primitives get zero area.
// TS2_NACHZIEH=1 forces the effect without the menu (test runs).
extern int ps2xGsBridgeOption(int id, int fallback);
// TS2_DUNST=<0..100> (percent, default 100 = original): reduce the opacity of full-screen overlays that stretch a
// small part of the picture over the whole screen (Siberia: snow flurry/wind streaks from a downscaled
// frame copy). At a high internal resolution this copy is much sharper than on the PS2 and the streaks look restless.
// Scales RGBAQ.A of the vertices and ALPHA.FIX in the packet.
static bool ps2xDunstFix(const uint8_t *data, uint32_t size, std::vector<uint8_t> &out)
{
    static const int proz = [] { const char *e = std::getenv("TS2_DUNST"); return e && *e ? std::clamp(std::atoi(e), 0, 100) : 100; }();
    if (proz >= 100) return false;
    static int32_t ofx = 1728 * 16, ofy = 1824 * 16;
    std::vector<uint32_t> aOffs, fixOffs; std::vector<bool> aPacked;
    int32_t mn = INT32_MAX, mx = INT32_MIN, ymn = INT32_MAX, ymx = INT32_MIN; uint32_t umn = 0xFFFF, umx = 0, nV = 0;
    uint32_t prim = 0, curU = 0; bool sprite = false, tme = false, alleSprite = true;
    uint32_t o = 0;
    while (o + 16 <= size)
    {
        uint64_t lo, hi; std::memcpy(&lo, data + o, 8); std::memcpy(&hi, data + o + 8, 8); o += 16;
        const uint32_t nloop = lo & 0x7FFF, flg = (lo >> 58) & 3; uint32_t nreg = (lo >> 60) & 0xF; if (!nreg) nreg = 16;
        if ((lo >> 46) & 1) prim = (lo >> 47) & 0x7FF;
        if (flg != 0) { o += flg == 1 ? ((nloop * nreg + 1) / 2) * 16 : nloop * 16; if (lo & 0x8000) break; continue; }
        for (uint32_t i = 0; i < nloop; ++i)
            for (uint32_t r = 0; r < nreg; ++r, o += 16)
            {
                if (o + 16 > size) return false;
                uint32_t reg = (hi >> (4 * r)) & 0xF; uint64_t v; std::memcpy(&v, data + o, 8);
                bool ad = false;
                if (reg == 0xE) { reg = data[o + 8]; ad = true; if (reg == 0x00) prim = v & 0x7FF; else if (reg == 0x18) { ofx = v & 0xFFFF; ofy = (v >> 32) & 0xFFFF; } }
                if (reg == 0x01) { aOffs.push_back(o); aPacked.push_back(!ad); }
                else if (ad && reg == 0x42) fixOffs.push_back(o);
                else if (reg == 0x03) curU = ad ? (v & 0x3FFF) : (v & 0x3FFF);
                else if (reg == 0x04 || reg == 0x05)
                {
                    uint16_t x, y; std::memcpy(&x, data + o, 2); std::memcpy(&y, data + o + (ad ? 2 : 4), 2);
                    mn = std::min<int32_t>(mn, x); mx = std::max<int32_t>(mx, x); ymn = std::min<int32_t>(ymn, y); ymx = std::max<int32_t>(ymx, y);
                    umn = std::min(umn, curU); umx = std::max(umx, curU); ++nV;
                    sprite = (prim & 7) == 6; tme = (prim & 0x10) != 0; if (!sprite || !tme || !((prim >> 8) & 1)) alleSprite = false;
                }
            }
        if (lo & 0x8000) break;
    }
    if (!nV || !alleSprite || nV > 8) return false;
    const float w = (mx - mn) / 16.0f, h = (ymx - ymn) / 16.0f, uw = (umx - umn) / 16.0f;
    if (w < 500.0f || h < 440.0f || uw >= 0.6f * w || uw < 8.0f) return false;   // Vollbild, stark vergroessert
    out.assign(data, data + size);
    for (size_t i = 0; i < aOffs.size(); ++i)
    {
        const uint32_t b = aOffs[i] + (aPacked[i] ? 12 : 3);
        out[b] = static_cast<uint8_t>(out[b] * proz / 100);
    }
    for (uint32_t f : fixOffs) out[f + 4] = static_cast<uint8_t>(out[f + 4] * proz / 100);
    static long logRest = [] { const char *e = std::getenv("PS2X_DUNSTLOG"); return e ? std::atol(e) : 0L; }();
    if (logRest > 0) { --logRest; std::fprintf(stderr, "[HAZE] uv width %.0f, %zu A values (first %u), %zu FIX\n", uw, aOffs.size(),
        aOffs.empty() ? 0u : static_cast<unsigned>(data[aOffs[0] + (aPacked[0] ? 12 : 3)]), fixOffs.size()); }
    return true;
}

static bool ps2xNachziehFix(const uint8_t *data, uint32_t size, std::vector<uint8_t> &out)
{
    // A frame copy is a direct-colour texture (CT32/CT24/CT16). Palette textures at an address that once was a render
    // target (impact smoke: 8-bit, TBP 0x33e0) are ordinary textures and must stay untouched (GitHub issue #3).
    static uint32_t psm = 0;
    static const int envFallback = [] { const char *e = std::getenv("TS2_NACHZIEH"); return (e && *e == '1') ? 1 : 0; }();
    static uint32_t prim = 0, tbp = 0, fbp = 0; static uint64_t alpha = 0;
    static bool anker = false; static uint16_t ax = 0, ay = 0;
    static uint32_t pakete = 0, naechste = 0; static int an = 0;
    if (++pakete >= naechste) { an = ps2xGsBridgeOption(1, envFallback); naechste = pakete + 256; }
    if (an) return false;
    bool geaendert = false;
    // Trail primitive: triangle strip/sprite with texture + alpha (FST), ALPHA 0x44, texture = start of the OTHER frame buffer
    auto treffer = [&]() {
        const uint32_t typ = prim & 7;
        return (typ == 4 || typ == 6) && (prim & 0x150) == 0x150 && (alpha & 0xFF) == 0x44 && (tbp & 31) == 0 &&
               (tbp >> 5) != (fbp & 0x1FF) && (psm <= 2 && ps2xTexIstBild(tbp));
    };
    auto vert = [&](uint32_t o, bool ad, uint16_t x, uint16_t y) {
        if (!treffer()) { anker = false; return; }
        if (!anker) { ax = x; ay = y; anker = true; return; }
        // put all further vertices onto the first one -> zero area, nothing is drawn
        if (!geaendert) { out.assign(data, data + size); geaendert = true; }
        std::memcpy(out.data() + o, &ax, 2);
        std::memcpy(out.data() + o + (ad ? 2 : 4), &ay, 2);
    };
    uint32_t o = 0;
    while (o + 16 <= size)
    {
        uint64_t lo, hi; std::memcpy(&lo, data + o, 8); std::memcpy(&hi, data + o + 8, 8); o += 16;
        const uint32_t nloop = lo & 0x7FFF, flg = (lo >> 58) & 3; uint32_t nreg = (lo >> 60) & 0xF; if (!nreg) nreg = 16;
        if ((lo >> 46) & 1) { prim = (lo >> 47) & 0x7FF; anker = false; }
        if (flg == 0)
        {
            for (uint32_t i = 0; i < nloop; ++i)
                for (uint32_t r = 0; r < nreg; ++r, o += 16)
                {
                    if (o + 16 > size) return geaendert;
                    const uint32_t reg = (hi >> (4 * r)) & 0xF;
                    uint64_t v; std::memcpy(&v, data + o, 8);
                    if (reg == 0x6) { tbp = v & 0x3FFF; psm = (v >> 20) & 0x3F; }
                    else if (reg == 0x4 || reg == 0x5 || reg == 0xC || reg == 0xD)
                    { uint16_t x, y; std::memcpy(&x, data + o, 2); std::memcpy(&y, data + o + 4, 2); vert(o, false, x, y); }
                    else if (reg == 0xE)
                    {
                        const uint32_t ad = data[o + 8];
                        if (ad == 0x00) { prim = v & 0x7FF; anker = false; }
                        else if (ad == 0x06) { tbp = v & 0x3FFF; psm = (v >> 20) & 0x3F; }
                        else if (ad == 0x4C) fbp = v & 0x1FF;
                        else if (ad == 0x42) alpha = v;
                        else if (ad == 0x04 || ad == 0x05 || ad == 0x0C || ad == 0x0D)
                            vert(o, true, static_cast<uint16_t>(v & 0xFFFF), static_cast<uint16_t>((v >> 16) & 0xFFFF));
                    }
                }
        }
        else if (flg == 1) o += ((nloop * nreg + 1) / 2) * 16;
        else o += nloop * 16;
        if (lo & 0x8000) break;
    }
    return geaendert;
}

// TS2 sniper/zoom blur: while zoomed the game blends the frame onto itself several times with triangle strips
// (PRIM 0x15C, ALPHA 0x48 = Cs*As+Cd, additive) and a full-screen sprite (PRIM 0x156, ALPHA 0x54) -> radial smear.
// The same happens on the PS2/in PCSX2, but at high internal resolution it just looks blurry.
// Scope option (bridge option 12, env TS2_SCOPE_KLAR as fallback): 0 = original, 1 = clear (default): the zoom feedback
// sprite blends with a fixed 8 % instead of the destination alpha (the original green tint stays), 2 = extra clear:
// 4 % and without the additive tint layers.
// The sprite stays because it also fills the picture edges that the zoomed scene does not cover.
// time (ms, steady clock) the scope's zoom feedback sprite was last seen: the mouse aiming in ts2_override
// uses it to notice "sniper scope is up although no zoom key is held"
std::atomic<long long> g_ps2xScopeMs{0};
static bool ps2xScopeKlarFix(const uint8_t *data, uint32_t size, std::vector<uint8_t> &out)
{
    // A frame copy is a direct-colour texture (CT32/CT24/CT16). Palette textures at an address that once was a render
    // target (impact smoke: 8-bit, TBP 0x33e0) are ordinary textures and must stay untouched (GitHub issue #3).
    static uint32_t psm = 0;
    static const int envFallback = [] { const char *e = std::getenv("TS2_SCOPE_KLAR"); return e && *e ? std::clamp(std::atoi(e), 0, 3) : 1; }();
    static uint32_t prim = 0, tbp = 0; static uint64_t alpha = 0;
    static bool anker = false; static uint16_t ax = 0, ay = 0;
    static uint32_t pakete = 0, naechste = 0; static int stufe = 1;
    if (++pakete >= naechste) { stufe = ps2xGsBridgeOption(12, envFallback); naechste = pakete + 256; }
    if (stufe <= 0) return false;
    bool geaendert = false;
    uint32_t alphaOff = ~0u;   // offset of the ALPHA_1 write (A+D) in this packet
    // Feedback weight of the zoom sprite: Klar 8 %, Extra klar 4 % (original: destination alpha)
    const uint8_t fix = stufe >= 2 ? 0x05 : 0x0A;
    bool alphaGesetzt = false, brauchtVorsatz = false; uint64_t alphaOrig = 0;
    uint32_t eopTagOff = ~0u, eopEnde = size;
    // vertices of the feedback sprite (for the edge strips): offsets of UV and XYZ, A+D or packed
    struct Ecke { uint32_t uvOff, xyOff; bool uvAd, xyAd; }; Ecke ecken[2]; int nEcken = 0;
    uint32_t uvOff = ~0u; bool uvAd = false;
    auto kopie = [&]() { if (!geaendert) { out.assign(data, data + size); geaendert = true; } };
    auto vert = [&](uint32_t o, bool ad, uint16_t x, uint16_t y) {
        const uint32_t typ = prim & 7, a = static_cast<uint32_t>(alpha & 0xFF);
        const bool bild = (prim & 0x150) == 0x150 && (psm <= 2 && ps2xTexIstBild(tbp));
        if (bild && typ == 6 && a == 0x54 && stufe != 3)
        {
            g_ps2xScopeMs.store(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(), std::memory_order_relaxed);
            if (nEcken < 2 && uvOff != ~0u) ecken[nEcken++] = {uvOff, o, uvAd, ad};
            // zoom feedback sprite: keep it (it also fills the picture edges), but with a small fixed weight
            if (alphaOff != ~0u) { if (!alphaGesetzt) { kopie(); uint64_t v; std::memcpy(&v, out.data() + alphaOff, 8);
                v = (v & ~(0x30ull | (0xFFull << 32))) | 0x20ull | (static_cast<uint64_t>(fix) << 32); std::memcpy(out.data() + alphaOff, &v, 8); alphaGesetzt = true; }
                anker = false; return; }
            // ALPHA was set in an earlier packet: wrap this packet in its own ALPHA write (and restore it afterwards)
            if (!brauchtVorsatz) { brauchtVorsatz = true; alphaOrig = alpha; }
            anker = false; return;
        }
        // the additive layers are the green scope tint, not the blur: only "extra clear" removes them
        const bool weg = bild && ((stufe == 2 && typ == 4 && a == 0x48) || (typ == 6 && a == 0x54 && stufe >= 2));
        if (!weg) { anker = false; return; }
        if (!anker) { ax = x; ay = y; anker = true; return; }
        kopie();
        std::memcpy(out.data() + o, &ax, 2);
        std::memcpy(out.data() + o + (ad ? 2 : 4), &ay, 2);
    };
    uint32_t o = 0;
    while (o + 16 <= size)
    {
        const uint32_t tagOff = o;
        uint64_t lo, hi; std::memcpy(&lo, data + o, 8); std::memcpy(&hi, data + o + 8, 8); o += 16;
        const uint32_t nloop = lo & 0x7FFF, flg = (lo >> 58) & 3; uint32_t nreg = (lo >> 60) & 0xF; if (!nreg) nreg = 16;
        if ((lo >> 46) & 1) { prim = (lo >> 47) & 0x7FF; anker = false; }
        if (flg == 0)
        {
            for (uint32_t i = 0; i < nloop; ++i)
                for (uint32_t r = 0; r < nreg; ++r, o += 16)
                {
                    if (o + 16 > size) return geaendert;
                    const uint32_t reg = (hi >> (4 * r)) & 0xF;
                    uint64_t v; std::memcpy(&v, data + o, 8);
                    if (reg == 0x6) { tbp = v & 0x3FFF; psm = (v >> 20) & 0x3F; }
                    else if (reg == 0x3) { uvOff = o; uvAd = false; }
                    else if (reg == 0x4 || reg == 0x5 || reg == 0xC || reg == 0xD)
                    { uint16_t x, y; std::memcpy(&x, data + o, 2); std::memcpy(&y, data + o + 4, 2); vert(o, false, x, y); }
                    else if (reg == 0xE)
                    {
                        const uint32_t ad = data[o + 8];
                        if (ad == 0x00) { prim = v & 0x7FF; anker = false; }
                        else if (ad == 0x06) { tbp = v & 0x3FFF; psm = (v >> 20) & 0x3F; }
                        else if (ad == 0x03) { uvOff = o; uvAd = true; }
                        else if (ad == 0x42) { alpha = v; alphaOff = o; }
                        else if (ad == 0x04 || ad == 0x05 || ad == 0x0C || ad == 0x0D)
                            vert(o, true, static_cast<uint16_t>(v & 0xFFFF), static_cast<uint16_t>((v >> 16) & 0xFFFF));
                    }
                }
        }
        else if (flg == 1) o += ((nloop * nreg + 1) / 2) * 16;
        else o += nloop * 16;
        if (lo & 0x8000) { eopTagOff = tagOff; eopEnde = std::min(o, size); break; }
    }
    if (brauchtVorsatz && eopTagOff != ~0u)
    {
        const uint64_t neu = (alphaOrig & ~(0x30ull | (0xFFull << 32))) | 0x20ull | (static_cast<uint64_t>(fix) << 32);
        auto adTag = [](std::vector<uint8_t> &v, uint64_t wert, bool eop) {
            const uint64_t tag = 1ull | (eop ? 0x8000ull : 0ull) | (1ull << 60), regs = 0xE;   // NLOOP 1, NREG 1, PACKED, A+D
            uint8_t q[32] = {}; std::memcpy(q, &tag, 8); std::memcpy(q + 8, &regs, 8); std::memcpy(q + 16, &wert, 8); q[24] = 0x42;
            v.insert(v.end(), q, q + 32); };
        std::vector<uint8_t> body = geaendert ? out : std::vector<uint8_t>(data, data + size);
        uint64_t tlo; std::memcpy(&tlo, body.data() + eopTagOff, 8); tlo &= ~0x8000ull; std::memcpy(body.data() + eopTagOff, &tlo, 8);
        std::vector<uint8_t> neuPaket; neuPaket.reserve(body.size() + 64);
        adTag(neuPaket, neu, false);
        neuPaket.insert(neuPaket.end(), body.begin(), body.begin() + eopEnde);
        adTag(neuPaket, alphaOrig, true);
        neuPaket.insert(neuPaket.end(), body.begin() + eopEnde, body.end());
        out.swap(neuPaket); geaendert = true;
    }
    // The zoomed scene leaves a thin border unrendered that the original feedback (zoomed copy at full weight) fills.
    // Add the original sprite again, but only over 48 px edge strips (full weight there, so no stale border).
    if (alphaGesetzt || brauchtVorsatz)
    {
        if (nEcken == 2 && eopTagOff != ~0u)
        {
            auto rd16 = [&](uint32_t off, int k) { uint16_t v; std::memcpy(&v, data + off + k, 2); return v; };
            auto xyK = [&](const Ecke &c, bool y) { return static_cast<float>(rd16(c.xyOff, y ? (c.xyAd ? 2 : 4) : 0)); };
            auto uvK = [&](const Ecke &c, bool vv) { uint32_t w; std::memcpy(&w, data + c.uvOff + (vv ? (c.uvAd ? 2 : 4) : 0), 4); return static_cast<float>(w & 0x3FFF); };
            const float X0 = xyK(ecken[0], false), X1 = xyK(ecken[1], false), Y0 = xyK(ecken[0], true), Y1 = xyK(ecken[1], true);
            const float U0 = uvK(ecken[0], false), U1 = uvK(ecken[1], false), V0 = uvK(ecken[0], true), V1 = uvK(ecken[1], true);
            const float S = 48.0f * 16.0f;
            if (X1 - X0 > 4 * S && Y1 - Y0 > 4 * S)
            {
                auto streifen = [&](float x0, float x1, float y0, float y1) {
                    std::vector<uint8_t> k(data, data + size);
                    uint64_t t; std::memcpy(&t, k.data() + eopTagOff, 8); t &= ~0x8000ull; std::memcpy(k.data() + eopTagOff, &t, 8);
                    const float xs[2] = {x0, x1}, ys[2] = {y0, y1};
                    for (int i = 0; i < 2; ++i)
                    {
                        const Ecke &c = ecken[i];
                        const uint16_t nx = static_cast<uint16_t>(xs[i] + 0.5f), ny = static_cast<uint16_t>(ys[i] + 0.5f);
                        std::memcpy(k.data() + c.xyOff, &nx, 2); std::memcpy(k.data() + c.xyOff + (c.xyAd ? 2 : 4), &ny, 2);
                        const float u = U0 + (xs[i] - X0) * (U1 - U0) / (X1 - X0), vv = V0 + (ys[i] - Y0) * (V1 - V0) / (Y1 - Y0);
                        uint32_t uw, vw; std::memcpy(&uw, k.data() + c.uvOff, 4); uw = (uw & ~0x3FFFu) | (static_cast<uint32_t>(u + 0.5f) & 0x3FFF); std::memcpy(k.data() + c.uvOff, &uw, 4);
                        const uint32_t vo = c.uvOff + (c.uvAd ? 2 : 4);
                        std::memcpy(&vw, k.data() + vo, 4); vw = (vw & ~0x3FFFu) | (static_cast<uint32_t>(vv + 0.5f) & 0x3FFF); std::memcpy(k.data() + vo, &vw, 4);
                    }
                    k.resize(eopEnde);
                    // the copy runs after the restore tag (original ALPHA) - clear the EOP of what came before
                    if (!out.empty()) { /* out ends with an EOP tag: find it via the restore tag we appended or the original */ }
                    return k; };
                // out currently ends with EOP (either the wrapped packet or the modified original): clear that EOP
                {
                    uint32_t q = 0, letzter = ~0u;
                    while (q + 16 <= out.size())
                    {
                        uint64_t lo, hi; std::memcpy(&lo, out.data() + q, 8); std::memcpy(&hi, out.data() + q + 8, 8);
                        const uint32_t nl = lo & 0x7FFF, fl = (lo >> 58) & 3; uint32_t nr = (lo >> 60) & 0xF; if (!nr) nr = 16;
                        if (lo & 0x8000) { letzter = q; }
                        q += 16 + (fl == 0 ? nl * nr * 16 : fl == 1 ? ((nl * nr + 1) / 2) * 16 : nl * 16);
                        if (lo & 0x8000) break;
                    }
                    if (letzter != ~0u) { uint64_t lo; std::memcpy(&lo, out.data() + letzter, 8); lo &= ~0x8000ull; std::memcpy(out.data() + letzter, &lo, 8); out.resize(std::min<size_t>(q, out.size())); }
                }
                const std::vector<uint8_t> teile[4] = {streifen(X1 - S, X1, Y0, Y1), streifen(X0, X1, Y0, Y0 + S), streifen(X0, X0 + S, Y0, Y1), streifen(X0, X1, Y1 - S, Y1)};
                for (const auto &t : teile) out.insert(out.end(), t.begin(), t.end());
                // EOP on the last tag of the last strip
                uint64_t lo; std::memcpy(&lo, out.data() + (out.size() - eopEnde) + eopTagOff, 8); lo |= 0x8000ull; std::memcpy(out.data() + (out.size() - eopEnde) + eopTagOff, &lo, 8);
            }
        }
    }
    return geaendert;
}

// HUD groups for TS2_HUD_RAND: combine the 2D packets of a frame into blocks (same rows, gap <= 20 px).
// In the next frame a packet only moves to the edge if its whole block was in the outer quarter - this keeps
// texts made of many single packets (mission messages) together while ammo/timers go to the edge.
struct Ps2xHudBox { float x0, x1, y0, y1; };
static void ps2xHudBloecke(std::vector<Ps2xHudBox> &b)
{
    auto beruehrt = [](const Ps2xHudBox &p, const Ps2xHudBox &q) {
        return p.y0 <= q.y1 + 2.0f && q.y0 <= p.y1 + 2.0f && p.x0 <= q.x1 + 20.0f && q.x0 <= p.x1 + 20.0f; };
    for (int durchgang = 0; durchgang < 2; ++durchgang)
        for (size_t i = 0; i < b.size(); ++i)
            for (size_t j = i + 1; j < b.size();)
                if (beruehrt(b[i], b[j]))
                {
                    b[i] = {std::min(b[i].x0, b[j].x0), std::max(b[i].x1, b[j].x1), std::min(b[i].y0, b[j].y0), std::max(b[i].y1, b[j].y1)};
                    b[j] = b.back(); b.pop_back(); j = i + 1;
                }
                else ++j;
}

// Menu layout for TS2_MENU_RAND: the in-game grouping glues menus into one block (header tiles, boxes and texts
// all touch). Here two kinds are grouped separately: text lines (h < 32 px, merged only along the same line) and
// areas (box parts, pictures, header tiles: merged when touching). Each group gets an anchor: narrow (< 200 px) and
// centred in the left/right third -> that 16:9 edge, otherwise the centre. Text inside a box (area < 480 px wide)
// follows the box, so a box never loses its contents.
struct Ps2xMenuBlock { Ps2xHudBox b; float anker; };   // anker in 640 coordinates (64, 320, 576)
static float ps2xMenuAnker(const Ps2xHudBox &g)
{
    const float w = g.x1 - g.x0, c = (g.x0 + g.x1) * 0.5f;
    if (w < 200.0f && c < 235.0f) return 64.0f;
    if (w < 200.0f && c > 405.0f) return 576.0f;
    return 320.0f;
}
static bool ps2xMenuIstZeile(const Ps2xHudBox &k) { return (k.y1 - k.y0) < 32.0f; }
static void ps2xMenuBloecke(const std::vector<Ps2xHudBox> &roh, std::vector<Ps2xMenuBlock> &zeilen, std::vector<Ps2xMenuBlock> &flaechen)
{
    std::vector<Ps2xHudBox> z, f;
    for (const auto &k : roh) (ps2xMenuIstZeile(k) ? z : f).push_back(k);
    ps2xHudBloecke(f);   // areas: merge when touching
    // lines: merge only with vertical overlap >= half the smaller height and a gap <= 20 px
    auto gleicheZeile = [](const Ps2xHudBox &p, const Ps2xHudBox &q) {
        const float ov = std::min(p.y1, q.y1) - std::max(p.y0, q.y0), mh = std::min(p.y1 - p.y0, q.y1 - q.y0);
        return ov >= 0.5f * std::max(mh, 1.0f) && p.x0 <= q.x1 + 20.0f && q.x0 <= p.x1 + 20.0f; };
    for (int durchgang = 0; durchgang < 2; ++durchgang)
        for (size_t i = 0; i < z.size(); ++i)
            for (size_t j = i + 1; j < z.size();)
                if (gleicheZeile(z[i], z[j]))
                { z[i] = {std::min(z[i].x0, z[j].x0), std::max(z[i].x1, z[j].x1), std::min(z[i].y0, z[j].y0), std::max(z[i].y1, z[j].y1)}; z[j] = z.back(); z.pop_back(); j = i + 1; }
                else ++j;
    flaechen.clear(); zeilen.clear();
    // Areas only move as a whole picture/box (>= 60x60); thin frame parts (box borders, glow brackets) stay centred
    // (edge anchors for pictures tore level pictures away from their list rows - everything keeps the original 4:3
    //  composition in the centre, which is how the PS2 widescreen mode lays out its menus as well)
    for (const auto &g : f) flaechen.push_back({g, 320.0f});
    for (const auto &g : z)
    {
        // Text never gets an edge anchor of its own: glyph runs of one word can arrive as separate pieces and were torn
        // apart ("Ven  us Starr", "Selec  t"). Text stays centred and only follows a picture/box it sits in.
        float a = 320.0f;
        const float cx = (g.x0 + g.x1) * 0.5f, cy = (g.y0 + g.y1) * 0.5f;
        for (const auto &fl : flaechen)   // text inside a box follows the box
            if (fl.anker != 320.0f && fl.b.x1 - fl.b.x0 < 480.0f && g.x0 >= fl.b.x0 - 4.0f && g.x1 <= fl.b.x1 + 4.0f && cy >= fl.b.y0 && cy <= fl.b.y1) { a = fl.anker; break; }
        zeilen.push_back({g, a});
    }
    // Mirror pairs (left/right parts of a centred composition, e.g. a box while it opens) belong to the centre
    // (for text lines only short pieces such as frame corners - real texts may legitimately sit left and right)
    auto spiegel = [](std::vector<Ps2xMenuBlock> &v, const std::vector<Ps2xMenuBlock> &alle, float maxBreite) {
        for (auto &x : v)
        {
            if (x.anker == 320.0f || x.b.x1 - x.b.x0 > maxBreite) continue;
            for (const auto &y : alle)
            {
                if (&x == &y) continue;
                const float cx = (x.b.x0 + x.b.x1) * 0.5f, cy = (y.b.x0 + y.b.x1) * 0.5f, wx = x.b.x1 - x.b.x0, wy = y.b.x1 - y.b.x0;
                if (std::fabs(cx + cy - 640.0f) < 24.0f && std::fabs(x.b.y0 - y.b.y0) < 16.0f && std::fabs(x.b.y1 - y.b.y1) < 16.0f &&
                    std::fabs(wx - wy) <= 0.3f * std::max(wx, wy) + 4.0f) { x.anker = 320.0f; break; }
            }
        }
    };
    std::vector<Ps2xMenuBlock> alle(flaechen); alle.insert(alle.end(), zeilen.begin(), zeilen.end());
    spiegel(flaechen, alle, 1e9f); spiegel(zeilen, alle, 40.0f);
}

static uint64_t s_ps2xTest1 = 0;   // TEST_1 as last set by the game (depth test state across packets)
static uint64_t s_ps2xScissor1 = 0x01DF0000027Full;   // SCISSOR_1 as last set by the game (GS state across packets)
static bool ps2xHudFix(const uint8_t *data, uint32_t size, std::vector<uint8_t> &out, uint64_t bild)
{
    const uint64_t scStart = s_ps2xScissor1;   // state at the start of this packet
    static const bool on = [] { const char *w = std::getenv("TS2_WIDESCREEN"); const char *h = std::getenv("TS2_HUD43");
                                return w && *w == '1' && !(h && *h == '0'); }();
    if (!on) return false;
    static int32_t ofx = 1728 * 16;   // XYOFFSET_1.OFX (12.4), tracked from A+D writes
    static int32_t ofy = 1824 * 16;   // XYOFFSET_1.OFY
    uint32_t curU = 0, curV = 0;      // last UV value (10.4) before the vertex
    // Walk: callback per XYZ field (offset of the 16-bit X) with the current FST state
    uint32_t curPrim = 0, primWrites = 0;
    static uint32_t curTbp = ~0u;   // TEX0_1.TBP0 (stays valid across packets)
    // PRMODECONT.AC = 0: TME/FST come from PRMODE instead of PRIM (stays valid across packets)
    static uint32_t prmodeAc = 1, prmode = 0;
    auto tme = [&]() { return ((prmodeAc ? curPrim : prmode) & 0x10) != 0; };
    auto untexSprite = [&]() { return (curPrim & 7) == 6 && !tme(); };
    std::vector<uint32_t> scissorOffs; uint32_t eopTagOff = ~0u, eopEnde = size;   // SCISSOR_1/2 (A+D) in the packet, last GIFtag
    auto walk = [&](auto &&onXyz) {
        uint32_t o = 0; bool fst = false;
        scissorOffs.clear();
        while (o + 16 <= size)
        {
            const uint32_t tagOff = o;
            uint64_t lo, hi; std::memcpy(&lo, data + o, 8); std::memcpy(&hi, data + o + 8, 8); o += 16;
            const uint32_t nloop = lo & 0x7FFF, flg = (lo >> 58) & 3; uint32_t nreg = (lo >> 60) & 0xF; if (!nreg) nreg = 16;
            if ((lo >> 46) & 1) { curPrim = (lo >> 47) & 0x7FF; fst = (curPrim >> 8) & 1; ++primWrites; }
            if (flg == 0)
            {
                for (uint32_t i = 0; i < nloop; ++i)
                    for (uint32_t r = 0; r < nreg; ++r, o += 16)
                    {
                        if (o + 16 > size) return;
                        const uint32_t reg = (hi >> (4 * r)) & 0xF;
                        if (reg == 0x6) { uint64_t t; std::memcpy(&t, data + o, 8); curTbp = t & 0x3FFF; }
                        else if (reg == 0x3) { uint64_t u; std::memcpy(&u, data + o, 8); curU = u & 0x3FFF; curV = (u >> 32) & 0x3FFF; }
                        else if (reg == 0x4 || reg == 0x5 || reg == 0xC || reg == 0xD) onXyz(o, fst || untexSprite(), false);
                        else if (reg == 0xE)
                        {
                            uint64_t dlo; std::memcpy(&dlo, data + o, 8); const uint32_t ad = data[o + 8];
                            if (ad == 0x00) { curPrim = dlo & 0x7FF; fst = (dlo >> 8) & 1; ++primWrites; }
                            else if (ad == 0x18) { ofx = static_cast<int32_t>(dlo & 0xFFFF); ofy = static_cast<int32_t>((dlo >> 32) & 0xFFFF); }
                            else if (ad == 0x03) { curU = dlo & 0x3FFF; curV = (dlo >> 16) & 0x3FFF; }
                            else if (ad == 0x06) curTbp = dlo & 0x3FFF;
                            else if (ad == 0x47) s_ps2xTest1 = dlo;
                            else if (ad == 0x1A) prmodeAc = dlo & 1;
                            else if (ad == 0x1B) prmode = dlo & 0x7FF;
                            else if (ad == 0x40 || ad == 0x41) { scissorOffs.push_back(o); if (ad == 0x40) std::memcpy(&s_ps2xScissor1, data + o, 8); }
                            else if (ad == 0x04 || ad == 0x05 || ad == 0x0C || ad == 0x0D) onXyz(o, fst || untexSprite(), true);
                        }
                    }
            }
            else if (flg == 1) o += ((nloop * nreg + 1) / 2) * 16;
            else o += nloop * 16;
            if (lo & 0x8000) { eopTagOff = tagOff; eopEnde = std::min(o, size); break; }
        }
    };
    int32_t mn = INT32_MAX, mx = INT32_MIN, ymn = INT32_MAX, ymx = INT32_MIN;
    // Strip copy: SPRITE whose texture is mapped 1:1 (or 1:2) onto the screen and is >= 64 px high
    // (screen copy in side strips, e.g. blur in split screen; the strip U starts at 0 for each strip).
    uint32_t sprPrimW = ~0u, sprPar = 0, nSprKopie = 0, nSpr = 0, nScope = 0; int32_t px0 = 0, py0 = 0; uint32_t pu0 = 0, pv0 = 0;
    uint32_t nVert = 0, nKopie = 0, uMin = 0xFFFF, uMax = 0, vMin = 0xFFFF, vMax = 0, primSeen = 0;
    // 2D vertices: FST primitives and untextured SPRITEs (crosshair, health bars, scope darkening)
    uint32_t nUntexSpr = 0;   // Eckpunkte untexturierter SPRITEs
    uint32_t n3d = 0;   // 3D vertices (STQ, not SPRITE): the game scene is running
    // World effects drawn as 2D sprites (impact smoke, explosions, fire, water spray, sparks): the game projects them
    // itself and tests them against the depth buffer (ZTST >= GEQUAL). HUD and menus are drawn with ZTST ALWAYS.
    // Such packets must keep their place - squeezed like the HUD they slid towards the screen centre, the further
    // from the centre the more (smoke and fountain water "moved" while walking, issue #3 follow-up).
    uint32_t nTief = 0;
    walk([&](uint32_t o, bool fst, bool ad) {
        if (!fst) { ++n3d; return; } uint16_t x, y; std::memcpy(&x, data + o, 2);
        std::memcpy(&y, data + o + (ad ? 2 : 4), 2);   // Y: A+D-Registerwert Bits 16..31, gepackt Bits 32..47
        mn = std::min<int32_t>(mn, x); mx = std::max<int32_t>(mx, x); ymn = std::min<int32_t>(ymn, y); ymx = std::max<int32_t>(ymx, y);
        // Frame copy? Texture coordinate = screen coordinate (1:1 or half resolution for effect buffers)
        const int32_t sx = static_cast<int32_t>(x) - ofx, sy = static_cast<int32_t>(y) - ofy;
        const bool kopie = tme() && ((std::abs(static_cast<int32_t>(curU) - sx) <= 48 && std::abs(static_cast<int32_t>(curV) - sy) <= 48) ||
                           (std::abs(static_cast<int32_t>(curU) * 2 - sx) <= 64 && std::abs(static_cast<int32_t>(curV) * 2 - sy) <= 64));
        if ((curPrim & 7) == 6)
        {
            if (sprPrimW != primWrites) { sprPrimW = primWrites; sprPar = 0; }
            if (sprPar == 0) { px0 = x; py0 = y; pu0 = curU; pv0 = curV; sprPar = 1; }
            else
            {
                sprPar = 0; ++nSpr;
                const int32_t dx = std::abs(static_cast<int32_t>(x) - px0), dy = std::abs(static_cast<int32_t>(y) - py0);
                const int32_t du = std::abs(static_cast<int32_t>(curU) - static_cast<int32_t>(pu0)), dv = std::abs(static_cast<int32_t>(curV) - static_cast<int32_t>(pv0));
                const bool eins = std::abs(du - dx) <= 32 && std::abs(dv - dy) <= 32, halb = std::abs(du * 2 - dx) <= 48 && std::abs(dv * 2 - dy) <= 48;
                if (dy >= 64 * 16 && (eins || halb)) ++nSprKopie;
                // Scope darkening: untextured rectangle from the left screen edge inwards (>= 48 wide, >= 100 high,
                // not up to the right edge like a full-screen flash) -> the HUD belongs to the scope, not to the edge
                const int32_t xa = (std::min<int32_t>(x, px0) - ofx) / 16, xb = (std::max<int32_t>(x, px0) - ofx) / 16;
                if (untexSprite() && xa <= 65 && xb < 560 && dx >= 48 * 16 && dy >= 100 * 16) ++nScope;
            }
        }
        if (untexSprite()) ++nUntexSpr;
        if (((s_ps2xTest1 >> 16) & 1) && ((s_ps2xTest1 >> 17) & 3) >= 2) ++nTief;
        ++nVert; if (kopie || (tme() && ps2xTexIstBild(curTbp))) ++nKopie;   // TBP only counts with TME (untextured scope frames)
        primSeen |= 1u << (curPrim & 7) | ((curPrim & 0x40) ? 0x100u : 0u) | (tme() ? 0x200u : 0u);
        uMin = std::min(uMin, curU); uMax = std::max(uMax, curU); vMin = std::min(vMin, curV); vMax = std::max(vMax, curV); });
    static std::atomic<int64_t> last3dMs{0};
    if (n3d >= 24) last3dMs.store(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(), std::memory_order_relaxed);
    if (mn > mx)
    {
        // PS2X_HUDLOG4=n (ab PS2X_HUDLOG4_AB s): Pakete ohne FST-Eckpunkte (Linien, STQ-Sprites) protokollieren
        static long rest4 = [] { const char *e = std::getenv("PS2X_HUDLOG4"); return e ? std::atol(e) : 0L; }();
        if (rest4 > 0)
        {
            static const int64_t ab4 = [] { const char *e = std::getenv("PS2X_HUDLOG4_AB"); return e ? std::atoll(e) * 1000LL : 0LL; }();
            static const int64_t t04 = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
            const int64_t j4 = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
            if (j4 - t04 >= ab4)
            {
                int32_t a = INT32_MAX, b = INT32_MIN, c = INT32_MAX, d = INT32_MIN; uint32_t n = 0, pr = 0;
                walk([&](uint32_t o, bool, bool ad) { uint16_t x, y; std::memcpy(&x, data + o, 2); std::memcpy(&y, data + o + (ad ? 2 : 4), 2);
                    a = std::min<int32_t>(a, x); b = std::max<int32_t>(b, x); c = std::min<int32_t>(c, y); d = std::max<int32_t>(d, y); ++n; pr |= 1u << (curPrim & 7) | ((curPrim & 0x10) ? 0x200u : 0u); });
                if (n && n < 64) { --rest4; std::fprintf(stderr, "[HUD4] t %lld x %.0f-%.0f y %.0f-%.0f n %u prims %03x\n", (long long)(j4 - t04),
                    (a - ofx) / 16.0, (b - ofx) / 16.0, (c - ofy) / 16.0, (d - ofy) / 16.0, n, pr); }
            }
        }
        return false;
    }
    {
        static const long logStart = [] { const char *e = std::getenv("PS2X_HUDLOG"); return e ? std::atol(e) : -1L; }();
        static long calls = 0; ++calls;
        if (logStart >= 0 && calls >= logStart && calls < logStart + 300)
            std::fprintf(stderr, "[HUD] #%ld x %.0f-%.0f y %.0f-%.0f uv %u-%u/%u-%u ofx %d ofy %d n %u kopie %u spr %u/%u prims %03x\n", calls,
                         (mn - ofx) / 16.0, (mx - ofx) / 16.0, (ymn - ofy) / 16.0, (ymx - ofy) / 16.0, uMin / 16, uMax / 16, vMin / 16, vMax / 16,
                         ofx / 16, ofy / 16, nVert, nKopie, nSprKopie, nSpr, primSeen);
    }
    {
        // PS2X_HUDLOG3=n (from PS2X_HUDLOG3_AB seconds): log all 2D packets with copy statistics
        static long rest3 = [] { const char *e = std::getenv("PS2X_HUDLOG3"); return e ? std::atol(e) : 0L; }();
        static const int64_t ab3 = [] { const char *e = std::getenv("PS2X_HUDLOG3_AB"); return e ? std::atoll(e) * 1000LL : 0LL; }();
        static const int64_t t03 = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
        const int64_t jetzt3 = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
        if (rest3 > 0 && jetzt3 - t03 >= ab3) { --rest3;
            std::fprintf(stderr, "[HUD3] t %lld x %.0f-%.0f y %.0f-%.0f uv %u-%u/%u-%u n %u kopie %u spr %u/%u prims %03x tbp %u\n", (long long)(jetzt3 - t03),
                (mn - ofx) / 16.0, (mx - ofx) / 16.0, (ymn - ofy) / 16.0, (ymx - ofy) / 16.0, uMin / 16, uMax / 16, vMin / 16, vMax / 16,
                nVert, nKopie, nSprKopie, nSpr, primSeen, curTbp); }
    }
    if (nKopie * 4 >= nVert * 3) return false;
    {
        // TS2_PARTIKEL_FREI=0: old behaviour; =w (tests): alternate every 64 VBlanks, so one run shows both
        static const int modus = [] { const char *e = std::getenv("TS2_PARTIKEL_FREI"); return (e && *e == '0') ? 0 : (e && *e == 'w') ? 2 : 1; }();
        const bool partikelFrei = modus == 1 || (modus == 2 && ((bild >> 6) & 1) == 0);
        if (nVert && nTief * 2 >= nVert)
        {
            // TS2_PARTIKEL_LOG=1: count the packets this rule applies to (tests)
            static const bool plog = [] { const char *e = std::getenv("TS2_PARTIKEL_LOG"); return e && *e == '1'; }();
            static uint64_t nFrei = 0;
            if (plog && (++nFrei % 500) == 1)
                std::fprintf(stderr, "[TS2] effect packets: %llu (vblank %llu, x %.0f-%.0f y %.0f-%.0f, %u vertices, prims %03x, kept %d)\n", (unsigned long long)nFrei,
                             (unsigned long long)bild, (mn - ofx) / 16.0, (mx - ofx) / 16.0, (ymn - ofy) / 16.0, (ymx - ofy) / 16.0, nVert, primSeen, partikelFrei ? 1 : 0);
            if (partikelFrei) return false;
        }
    }
    if (nSprKopie && nSprKopie * 2 >= nSpr) return false;   // ueberwiegend Streifenkopien -> Effekt
    const float L = static_cast<float>(ofx), Wd = 640.0f * 16.0f;
    const float wpx = (mx - mn) / 16.0f, cpx = ((mn + mx) * 0.5f - L) / 16.0f;
    // Menu detection for TS2_HUD_RAND: menus draw wide 2D areas (background, header); there are none in the
    // game. As long as such an area came within the last 150 ms everything stays centred (menus stay assembled).
    static std::atomic<int64_t> lastWideMs{0};
    const int64_t nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    // TS2 draws into x 64..575 (512 wide, centre 320): "wide" = almost the whole picture width
    // count only textured areas (menu backgrounds); untextured full-screen SPRITEs are flashes/darkening in the game
    // ... and only areas that lie inside the picture: menu backgrounds span x 62..577. Effects drawn over the game
    // (damage arcs, explosion rings) are wide too but reach far beyond the screen edge - they are not a menu and
    // must not pull the HUD to the centre. TS2_MENUE_EFFEKT=1 restores the old rule (for comparisons).
    {
        static const bool alt = [] { const char *e = std::getenv("TS2_MENUE_EFFEKT"); return e && *e == '1'; }();
        static long logRest = [] { const char *e = std::getenv("TS2_MENUELOG"); return e ? std::atol(e) : 0L; }();
        const float x0 = (mn - L) / 16.0f, x1 = (mx - L) / 16.0f, y0 = (ymn - ofy) / 16.0f, y1 = (ymx - ofy) / 16.0f;
        const bool imBild = x0 >= 40.0f && x1 <= 600.0f && y0 >= -24.0f && y1 <= 472.0f;
        const bool breit = wpx >= 480.0f && (primSeen & 0x200u);
        if (breit && logRest > 0) { --logRest; std::fprintf(stderr, "[MENUE] wide x %.0f-%.0f y %.0f-%.0f n %u prims %03x inside %d\n", x0, x1, y0, y1, nVert, primSeen, imBild ? 1 : 0); }
        if ((breit && (imBild || alt)) || nScope) lastWideMs.store(nowMs, std::memory_order_relaxed);
    }
    if (wpx >= 560.0f || (ymx - ymn) / 16.0f >= 360.0f) return false;
    // Untextured SPRITEs across the full width: clearing colour/depth buffer in strips (split screen) - never squeeze
    if (nUntexSpr && wpx >= 480.0f) return false;
    // ... and without a 3D scene (loading screen, pure 2D menus) do not move anything to the edge either
    const bool imMenue = nowMs - lastWideMs.load(std::memory_order_relaxed) < 150 || nowMs - last3dMs.load(std::memory_order_relaxed) > 250;
    // TS2_HUD_RAND=1: move HUD elements in the left/right third to the edge of the 16:9 picture (like modern
    // remasters), the centre stays centred. Default: whole HUD as a 4:3 area in the centre.
    static const bool hudRand = [] { const char *e = std::getenv("TS2_HUD_RAND"); return !(e && *e == '0'); }();   // Standard an
    {
        // PS2X_HUDLOG2=n: log n packets from the lower half of the picture (HUD candidates) with the basis for the decision
        static long rest = [] { const char *e = std::getenv("PS2X_HUDLOG2"); return e ? std::atol(e) : 0L; }();
        const float cy = ((ymn + ymx) * 0.5f - ofy) / 16.0f;
        static const int64_t ab = [] { const char *e = std::getenv("PS2X_HUDLOG2_AB"); return e ? std::atoll(e) * 1000LL : 0LL; }();
        static const int64_t t0 = nowMs;
        if (rest > 0 && cy > 300.0f && nowMs - t0 >= ab) { --rest; std::fprintf(stderr, "[HUD2] x %.0f-%.0f (c %.0f w %.0f) y %.0f-%.0f n %u menue %d prims %03x\n",
            (mn - L) / 16.0f, (mx - L) / 16.0f, cpx, wpx, (ymn - ofy) / 16.0f, (ymx - ofy) / 16.0f, nVert, imMenue ? 1 : 0, primSeen); }
    }
    float anchor = L + Wd * 0.5f;
    if (hudRand)
    {
        static std::vector<Ps2xHudBox> jetzt, vorher;
        static uint64_t jetztBild = ~0ull;
        if (bild != jetztBild) { ps2xHudBloecke(jetzt); vorher.swap(jetzt); jetzt.clear(); jetztBild = bild; }
        const Ps2xHudBox k{(mn - L) / 16.0f, (mx - L) / 16.0f, (ymn - ofy) / 16.0f, (ymx - ofy) / 16.0f};
        // Wide strips (menu backgrounds drawn in 6 px bands, >= 400 px) are not part of any block - otherwise they
        // glue every menu element into one screen-wide block and nothing can move to the edge.
        if (jetzt.size() < 512 && wpx < 400.0f) jetzt.push_back(k);
        // TS2_MENU_RAND (default on, 0 = off): also in menus move narrow groups in the outer thirds to the 16:9 edge (titles, button
        // prompts, level pictures), so they line up with the full-width menu backgrounds instead of floating inside.
        static const bool menuRand = [] { const char *e = std::getenv("TS2_MENU_RAND"); return !(e && *e == '0'); }();   // default on, 0 = off
        static std::vector<Ps2xHudBox> menuRoh, menuRohVorher; static uint64_t menuBild = ~0ull;
        static std::vector<Ps2xMenuBlock> mZeilen, mFlaechen;
        if (menuRand)
        {
            if (bild != menuBild) { menuRohVorher.swap(menuRoh); menuRoh.clear(); menuBild = bild; ps2xMenuBloecke(menuRohVorher, mZeilen, mFlaechen); }
            if (menuRoh.size() < 1024 && wpx < 560.0f) menuRoh.push_back(k);
        }
        if (menuRand && imMenue)
        {
            const auto &liste = ps2xMenuIstZeile(k) ? mZeilen : mFlaechen;
            const Ps2xMenuBlock *gef = nullptr;
            for (const auto &g : liste)
                if (k.x0 <= g.b.x1 && g.b.x0 <= k.x1 && k.y0 <= g.b.y1 && g.b.y0 <= k.y1) { anchor = L + g.anker * 16.0f; gef = &g; break; }
            static long rest = [] { const char *e = std::getenv("PS2X_MENUBLOCKLOG"); return e ? std::atol(e) : 0L; }();
            static const int64_t ab = [] { const char *e = std::getenv("PS2X_MENUBLOCKLOG_AB"); return e ? std::atoll(e) * 1000LL : 0LL; }();
            static const int64_t t0m = nowMs;
            if (rest > 0 && nowMs - t0m >= ab) { --rest; std::fprintf(stderr, "[MENUBLK] %s p %.0f-%.0f/%.0f-%.0f blk %.0f-%.0f/%.0f-%.0f anker %.0f\n", ps2xMenuIstZeile(k) ? "Z" : "F",
                k.x0, k.x1, k.y0, k.y1, gef ? gef->b.x0 : -1.f, gef ? gef->b.x1 : -1.f, gef ? gef->b.y0 : -1.f, gef ? gef->b.y1 : -1.f, gef ? gef->anker : -1.f); }
        }
        else if (!imMenue)
        {
            // Block from the last frame that contains this packet; without a block (newly appeared) it stays centred
            for (const Ps2xHudBox &g : vorher)
                if (k.x0 <= g.x1 && g.x0 <= k.x1 && k.y0 <= g.y1 && g.y0 <= k.y1)
                {
                    // Block narrower than a third, centre in the left/right third of the picture (x 64..576)
                    const float gc = (g.x0 + g.x1) * 0.5f;
                    if (g.x1 - g.x0 < 200.0f && gc < 235.0f) anchor = L + 64.0f * 16.0f;        // linke Bildkante bleibt
                    else if (g.x1 - g.x0 < 200.0f && gc > 405.0f) anchor = L + 576.0f * 16.0f;  // rechte Bildkante bleibt
                    {   // PS2X_HUDBLOCKLOG=n (from PS2X_HUDBLOCKLOG_AB s): packet, its block and the chosen anchor
                        static long rest = [] { const char *e = std::getenv("PS2X_HUDBLOCKLOG"); return e ? std::atol(e) : 0L; }();
                        static const int64_t ab = [] { const char *e = std::getenv("PS2X_HUDBLOCKLOG_AB"); return e ? std::atoll(e) * 1000LL : 0LL; }();
                        static const int64_t t0 = nowMs;
                        if (rest > 0 && nowMs - t0 >= ab) { --rest; std::fprintf(stderr, "[HUDBLK] p %.0f-%.0f/%.0f-%.0f blk %.0f-%.0f/%.0f-%.0f (%zu) menu %d anchor %.0f\n",
                            k.x0, k.x1, k.y0, k.y1, g.x0, g.x1, g.y0, g.y1, vorher.size(), imMenue ? 1 : 0, (anchor - L) / 16.0f); }
                    }
                    break;
                }
        }
    }
    out.assign(data, data + size);
    walk([&](uint32_t o, bool fst, bool) {
        if (!fst) return; uint16_t x; std::memcpy(&x, data + o, 2);
        // Vertices on the picture edge stay on the edge (scope darkening, bars flush with the edge)
        const float sx = (static_cast<float>(x) - L) / 16.0f;
        if (sx <= 72.0f || sx >= 568.0f) return;   // do not widen narrow edge strips either (62..66, split-screen frame)
        const float nx = anchor + (static_cast<float>(x) - anchor) * 0.75f;
        const uint16_t v = static_cast<uint16_t>(std::clamp(nx, 0.0f, 65535.0f) + 0.5f); std::memcpy(out.data() + o, &v, 2); });
    // Squeeze the SCISSOR in the squeezed 2D packet too (otherwise it clips wrongly: MapMaker tile list, cut-off texts).
    // Then restore the original value with an appended A+D so that later (3D) packets see the game's state.
    // Squeeze the clipping rectangle (SCISSOR_1) together with the 2D content. In menus it is usually set in an EARLIER
    // packet (scrolling list columns such as the character portraits): squeezing only the sprites moved them out of the
    // unchanged rectangle and they were cut in half. So the state valid for this packet is squeezed (prepended A+D),
    // the packet's own writes are squeezed, and the game's value is restored at the end for later (3D) packets.
    static const bool scissorAn = [] { const char *e = std::getenv("TS2_HUD_SCISSOR"); return !(e && *e == '0'); }();   // default on (0 = off)
    if (scissorAn && imMenue && eopTagOff != ~0u)
    {
        const float aS = (anchor - L) / 16.0f;
        auto quetsche = [&](uint64_t v) {
            const float x0 = static_cast<float>(v & 0x7FF), x1 = static_cast<float>((v >> 16) & 0x7FF);
            // union of the game's rectangle and the squeezed one: squeezed sprites stay inside, unsqueezed texts
            // (menu text keeps its position) are not cut at the panel edge either
            const float n0 = (x0 <= 72.0f) ? x0 : std::min(x0, std::floor(aS + (x0 - aS) * 0.75f));
            const float n1 = (x1 >= 568.0f) ? x1 : std::max(x1, std::ceil(aS + (x1 - aS) * 0.75f));
            return (v & ~0x07FF07FFull) | (static_cast<uint64_t>(std::clamp(n0, 0.0f, 2047.0f)) & 0x7FF) |
                   ((static_cast<uint64_t>(std::clamp(n1, 0.0f, 2047.0f)) & 0x7FF) << 16); };
        uint64_t letzterOrig = scStart; bool aenderung = false;
        for (uint32_t o : scissorOffs)
        {
            if (data[o + 8] != 0x40) continue;
            uint64_t v; std::memcpy(&v, data + o, 8); letzterOrig = v;
            const uint64_t q = quetsche(v); if (q != v) { std::memcpy(out.data() + o, &q, 8); aenderung = true; }
        }
        uint32_t versatz = 0;
        const uint64_t qStart = quetsche(scStart);
        if (qStart != scStart && (scissorOffs.empty() || scissorOffs.front() > 16u))
        {
            std::vector<uint8_t> vor(32, 0);
            const uint64_t tag = 1ull | (1ull << 60), regs = 0xE;   // NLOOP=1, no EOP, NREG=1, PACKED, REG A+D
            std::memcpy(vor.data(), &tag, 8); std::memcpy(vor.data() + 8, &regs, 8);
            std::memcpy(vor.data() + 16, &qStart, 8); vor[24] = 0x40;
            out.insert(out.begin(), vor.begin(), vor.end());
            versatz = 32; aenderung = true;
        }
        if (aenderung)
        {
            uint64_t tlo; std::memcpy(&tlo, out.data() + eopTagOff + versatz, 8); tlo &= ~0x8000ull; std::memcpy(out.data() + eopTagOff + versatz, &tlo, 8);
            std::vector<uint8_t> ein(32, 0);
            const uint64_t tag = 1ull | 0x8000ull | (1ull << 60), regs = 0xE;   // EOP
            std::memcpy(ein.data(), &tag, 8); std::memcpy(ein.data() + 8, &regs, 8);
            std::memcpy(ein.data() + 16, &letzterOrig, 8); ein[24] = 0x40;
            out.insert(out.begin() + eopEnde + versatz, ein.begin(), ein.end());
        }
    }
    return true;
}

void PS2Memory::submitGifPacket(GifPathId pathId, const uint8_t *data, uint32_t sizeBytes, bool drainImmediately, bool path2DirectHl, bool bridgeStripTag)
{
    if (pathId == GifPathId::Path1)
        g_ps2xPath1Packets.fetch_add(1, std::memory_order_relaxed);
    if (!data || sizeBytes < 16)
        return;
    if (!g_ps2xOnVu1Worker)
    {
        // MTVU: if VU1 is still running, queue the packet behind it instead of waiting (order is preserved)
        extern bool ps2xMtvuDefer(std::function<void()> fn);
        auto copy = std::make_shared<std::vector<uint8_t>>(data, data + sizeBytes);
        if (ps2xMtvuDefer([this, copy, pathId, drainImmediately, path2DirectHl, bridgeStripTag]()
                          { submitGifPacket(pathId, copy->data(), static_cast<uint32_t>(copy->size()), drainImmediately, path2DirectHl, bridgeStripTag); }))
            return;
    }
    ps2xVu1Fence(); // GIF-Reihenfolge: erst muss ein laufendes VU1-Programm (PATH1) fertig sein
    ps2xGifStat(pathId == GifPathId::Path1 ? 1 : pathId == GifPathId::Path2 ? 2 : 3, data, sizeBytes);
    ps2xFbTrack(data, sizeBytes);
    ps2xBlendLog(pathId == GifPathId::Path1 ? 1 : pathId == GifPathId::Path2 ? 2 : 3, data, sizeBytes);
    if (pathId == GifPathId::Path1) ps2xGrossLog(data, sizeBytes, gs().vsyncTick.load(std::memory_order_relaxed));
    thread_local std::vector<uint8_t> hudBuf;
    if (pathId == GifPathId::Path1 && ps2xHudFix(data, sizeBytes, hudBuf, gs().vsyncTick.load(std::memory_order_relaxed)))
    {
        data = hudBuf.data();
        sizeBytes = static_cast<uint32_t>(hudBuf.size());
    }
    thread_local std::vector<uint8_t> dunstBuf;
    if (pathId == GifPathId::Path1 && ps2xDunstFix(data, sizeBytes, dunstBuf))
    {
        data = dunstBuf.data();
        sizeBytes = static_cast<uint32_t>(dunstBuf.size());
    }
    thread_local std::vector<uint8_t> nachziehBuf;
    if (pathId == GifPathId::Path1 && ps2xNachziehFix(data, sizeBytes, nachziehBuf))
    {
        data = nachziehBuf.data();
        sizeBytes = static_cast<uint32_t>(nachziehBuf.size());
    }
    thread_local std::vector<uint8_t> scopeBuf;
    if (pathId == GifPathId::Path1 && ps2xScopeKlarFix(data, sizeBytes, scopeBuf))
    {
        data = scopeBuf.data();
        sizeBytes = static_cast<uint32_t>(scopeBuf.size());
    }

    if (pathId == GifPathId::Path3)
    {
        if (m_path3Masked)
        {
            m_path3MaskedFifo.emplace_back(data, data + sizeBytes);
            return;
        }
        flushMaskedPath3Packets(false);
    }

    if (m_gifArbiter && drainImmediately && m_gifArbiter->empty())
    {
        m_gifArbiter->processNow(pathId, data, sizeBytes, bridgeStripTag); // same order, no copy
        return;
    }
    if (m_gifArbiter)
        m_gifArbiter->submit(pathId, data, sizeBytes, path2DirectHl, bridgeStripTag);
    else if (m_gifPacketCallback)
        m_gifPacketCallback(data, sizeBytes);

    if (m_gifArbiter && drainImmediately)
        m_gifArbiter->drain();
}

void PS2Memory::processGIFPacket(uint32_t srcPhysAddr, uint32_t qwCount)
{
    if (!m_rdram || qwCount == 0)
        return;
    const uint64_t bytes64 = static_cast<uint64_t>(qwCount) * 16ull;
    uint32_t sizeBytes = (bytes64 > 0xFFFFFFFFull) ? 0xFFFFFFFFu : static_cast<uint32_t>(bytes64);
    uint32_t bytesLeft = sizeBytes;
    while (bytesLeft >= 16)
    {
        if (srcPhysAddr >= PS2_RAM_SIZE)
            srcPhysAddr = 0;
        uint32_t chunk = bytesLeft;
        if (srcPhysAddr + chunk > PS2_RAM_SIZE)
            chunk = PS2_RAM_SIZE - srcPhysAddr;
        if (chunk == 0)
            break;

        m_seenGifCopy = true;
        m_gifCopyCount.fetch_add(1, std::memory_order_relaxed);
        submitGifPacket(GifPathId::Path3, m_rdram + srcPhysAddr, chunk);

        bytesLeft -= chunk;
        srcPhysAddr += chunk;
    }
}

void PS2Memory::processGIFPacket(const uint8_t *data, uint32_t sizeBytes)
{
    if (m_gifArbiter)
        submitGifPacket(GifPathId::Path3, data, sizeBytes);
    else if (m_gifPacketCallback && data && sizeBytes >= 16)
        m_gifPacketCallback(data, sizeBytes);
}

bool PS2Memory::tryProcessNativeGifImageUploadChain(GS &gs, uint32_t tadr, uint32_t chcr)
{
    { extern bool ps2xGsBridgeActive(); if (ps2xGsBridgeActive()) return false; }
    static constexpr uint32_t GIF_CHANNEL = 0x1000A000u;
    static constexpr uint32_t D_STAT = 0x1000E010u;
    static constexpr uint32_t D_CTRL = 0x1000E000u;

    if (!m_rdram || !m_gsVRAM || m_path3Masked)
        return false;
    if (m_gifArbiter && !m_gifArbiter->empty())
        return false;
    if ((chcr & 0x100u) == 0u || ((chcr >> 2u) & 0x3u) != 1u)
        return false;
    if ((chcr & (1u << 7u)) != 0u || ((chcr >> 4u) & 0x3u) != 0u)
        return false;

    const auto dctrlIt = m_ioRegisters.find(D_CTRL);
    if (dctrlIt != m_ioRegisters.end() && ((dctrlIt->second & 0x1u) == 0u))
        return false;

    auto resolveContiguous = [&](uint32_t guestAddr, uint32_t bytes, const uint8_t *&out) -> bool
    {
        try
        {
            const bool scratch = isScratchpad(guestAddr);
            const uint32_t phys = translateAddress(guestAddr);
            const uint8_t *base = scratch ? m_scratchpad : m_rdram;
            const uint32_t limit = scratch ? PS2_SCRATCHPAD_SIZE : PS2_RAM_SIZE;
            if (!base || phys > limit || bytes > limit - phys)
                return false;
            out = base + phys;
            return true;
        }
        catch (const std::exception &)
        {
            return false;
        }
    };

    auto loadDmaTagAt = [&](uint32_t guestAddr, DmaTagView &out) -> bool
    {
        const uint8_t *ptr = nullptr;
        if (!resolveContiguous(guestAddr, 16u, ptr))
            return false;
        out = decodeDmaTag(loadScalar<uint64_t>(ptr, 0u, 16u, "native gif dma tag", guestAddr));
        return true;
    };

    auto decodeSetupPayload = [&](const uint8_t *payload, uint64_t (&regs)[4]) -> bool
    {
        const uint64_t tagLo = loadScalar<uint64_t>(payload, 0u, 80u, "native gif setup tag", 0u);
        const uint64_t tagHi = loadScalar<uint64_t>(payload, 8u, 80u, "native gif setup regs", 0u);
        if (gifTagNloop(tagLo) != 4u ||
            gifTagFlg(tagLo) != GIF_FMT_PACKED ||
            gifTagNreg(tagLo) != 1u ||
            (tagHi & 0xFull) != 0x0Eull)
        {
            return false;
        }

        static constexpr uint8_t kExpectedRegs[4] = {
            GS_REG_BITBLTBUF,
            GS_REG_TRXPOS,
            GS_REG_TRXREG,
            GS_REG_TRXDIR,
        };

        uint32_t offset = 16u;
        for (uint32_t i = 0; i < 4u; ++i)
        {
            regs[i] = loadScalar<uint64_t>(payload, offset, 80u, "native gif setup value", 0u);
            const uint64_t reg = loadScalar<uint64_t>(payload, offset + 8u, 80u, "native gif setup register", 0u);
            if ((reg & 0xFFu) != kExpectedRegs[i])
                return false;
            offset += 16u;
        }

        const uint32_t trxdirMode = static_cast<uint32_t>(regs[3] & 0x3ull);
        const uint32_t rrw = static_cast<uint32_t>(regs[2] & 0xFFFull);
        const uint32_t rrh = static_cast<uint32_t>((regs[2] >> 32u) & 0xFFFull);
        return trxdirMode == 0u && rrw != 0u && rrh != 0u;
    };

    DmaTagView setupTag{};
    if (!loadDmaTagAt(tadr, setupTag) ||
        setupTag.id != 1u ||
        setupTag.qwc != 5u ||
        setupTag.irq)
    {
        return false;
    }

    const uint8_t *setupPayload = nullptr;
    const uint32_t setupPayloadAddr = tadr + 16u;
    if (!resolveContiguous(setupPayloadAddr, 5u * 16u, setupPayload))
        return false;

    uint64_t setupRegs[4] = {};
    if (!decodeSetupPayload(setupPayload, setupRegs))
        return false;

    uint32_t imageTagDmaAddr = setupPayloadAddr + 5u * 16u;
    DmaTagView imageTagDma{};
    if (!loadDmaTagAt(imageTagDmaAddr, imageTagDma) ||
        imageTagDma.id != 1u ||
        imageTagDma.qwc != 1u ||
        imageTagDma.irq)
    {
        return false;
    }

    const uint8_t *imageGifTag = nullptr;
    if (!resolveContiguous(imageTagDmaAddr + 16u, 16u, imageGifTag))
        return false;

    const uint64_t imageTagLo = loadScalar<uint64_t>(imageGifTag, 0u, 16u, "native gif image tag", imageTagDmaAddr + 16u);
    if (gifTagFlg(imageTagLo) != GIF_FMT_IMAGE)
        return false;

    const uint32_t imageQwc = gifTagNloop(imageTagLo);
    if (imageQwc == 0u)
        return false;

    const uint64_t imageBytes64 = static_cast<uint64_t>(imageQwc) * 16ull;
    if (imageBytes64 > 0xFFFFFFFFull)
        return false;
    const uint32_t imageBytes = static_cast<uint32_t>(imageBytes64);

    const uint32_t payloadTagAddr = imageTagDmaAddr + 32u;
    DmaTagView payloadTag{};
    if (!loadDmaTagAt(payloadTagAddr, payloadTag) ||
        payloadTag.qwc != imageQwc ||
        payloadTag.irq)
    {
        return false;
    }

    uint32_t imageDataAddr = 0u;
    uint32_t finalTadr = payloadTagAddr;
    uint32_t lastTagUpper = payloadTag.upper;
    if (payloadTag.id == 3u || payloadTag.id == 4u)
    {
        imageDataAddr = payloadTag.addr;
        const uint32_t terminalTagAddr = payloadTagAddr + 16u;
        DmaTagView terminalTag{};
        if (!loadDmaTagAt(terminalTagAddr, terminalTag) ||
            terminalTag.qwc != 0u ||
            terminalTag.irq ||
            (terminalTag.id != 0u && terminalTag.id != 7u))
        {
            return false;
        }
        finalTadr = (terminalTag.id == 0u) ? (terminalTagAddr + 16u) : terminalTagAddr;
        lastTagUpper = terminalTag.upper;
    }
    else if (payloadTag.id == 7u)
    {
        imageDataAddr = payloadTagAddr + 16u;
        finalTadr = payloadTagAddr;
    }
    else
    {
        return false;
    }

    const uint8_t *imageData = nullptr;
    if (!resolveContiguous(imageDataAddr, imageBytes, imageData))
        return false;

    m_dmaStartCount.fetch_add(1, std::memory_order_relaxed);
    m_seenGifCopy = true;
    m_gifCopyCount.fetch_add(1, std::memory_order_relaxed);
    gs.uploadImageNative(setupRegs[0], setupRegs[1], setupRegs[2], setupRegs[3], imageData, imageBytes);

    m_ioRegisters[GIF_CHANNEL + 0x30u] = finalTadr;
    m_ioRegisters[GIF_CHANNEL + 0x40u] = 0u;
    m_ioRegisters[GIF_CHANNEL + 0x50u] = 0u;
    m_ioRegisters[GIF_CHANNEL + 0x00u] = ((chcr & 0x0000FFFFu) | (lastTagUpper << 16u)) & ~0x100u;
    m_ioRegisters[GIF_CHANNEL + 0x20u] = 0u;

    uint32_t dstat = m_ioRegisters.count(D_STAT) ? m_ioRegisters[D_STAT] : 0u;
    dstat |= (1u << 2u);
    const uint32_t status = dstat & 0x3FFu;
    const uint32_t mask = (dstat >> 16u) & 0x3FFu;
    if ((status & mask) != 0u)
        dstat |= (1u << 31u);
    else
        dstat &= ~(1u << 31u);
    m_ioRegisters[D_STAT] = dstat;
    queueCompletedDmacCause(2u);
    return true;
}

bool PS2Memory::tryProcessNativeGifPackedChain(GS &gs, uint32_t tadr, uint32_t chcr)
{
    { extern bool ps2xGsBridgeActive(); if (ps2xGsBridgeActive()) return false; }
    static constexpr uint32_t GIF_CHANNEL = 0x1000A000u;
    static constexpr uint32_t D_STAT = 0x1000E010u;
    static constexpr uint32_t D_CTRL = 0x1000E000u;

    if (!m_rdram || !m_gsVRAM || m_path3Masked)
        return false;
    if (m_gifArbiter && !m_gifArbiter->empty())
        return false;
    if ((chcr & 0x100u) == 0u || ((chcr >> 2u) & 0x3u) != 1u)
        return false;
    if ((chcr & (1u << 7u)) != 0u || ((chcr >> 4u) & 0x3u) != 0u)
        return false;

    const auto dctrlIt = m_ioRegisters.find(D_CTRL);
    if (dctrlIt != m_ioRegisters.end() && ((dctrlIt->second & 0x1u) == 0u))
        return false;

    auto resolveContiguous = [&](uint32_t guestAddr, uint32_t bytes, const uint8_t *&out) -> bool
    {
        try
        {
            const bool scratch = isScratchpad(guestAddr);
            const uint32_t phys = translateAddress(guestAddr);
            const uint8_t *base = scratch ? m_scratchpad : m_rdram;
            const uint32_t limit = scratch ? PS2_SCRATCHPAD_SIZE : PS2_RAM_SIZE;
            if (!base || phys > limit || bytes > limit - phys)
                return false;
            out = base + phys;
            return true;
        }
        catch (const std::exception &)
        {
            return false;
        }
    };

    const uint8_t *tagPtr = nullptr;
    if (!resolveContiguous(tadr, 16u, tagPtr))
        return false;

    const DmaTagView tag = decodeDmaTag(loadScalar<uint64_t>(tagPtr, 0u, 16u, "native packed gif dma tag", tadr));
    if (tag.id != 7u || tag.qwc == 0u || tag.irq)
        return false;

    const uint64_t payloadBytes64 = static_cast<uint64_t>(tag.qwc) * 16ull;
    if (payloadBytes64 > 0xFFFFFFFFull)
        return false;
    const uint32_t payloadBytes = static_cast<uint32_t>(payloadBytes64);

    const uint8_t *payload = nullptr;
    if (!resolveContiguous(tadr + 16u, payloadBytes, payload))
        return false;
    if (!gs.processNativePackedGIFPacket(payload, payloadBytes))
        return false;

    m_dmaStartCount.fetch_add(1, std::memory_order_relaxed);
    m_seenGifCopy = true;
    m_gifCopyCount.fetch_add(1, std::memory_order_relaxed);

    m_ioRegisters[GIF_CHANNEL + 0x30u] = tadr;
    m_ioRegisters[GIF_CHANNEL + 0x40u] = 0u;
    m_ioRegisters[GIF_CHANNEL + 0x50u] = 0u;
    m_ioRegisters[GIF_CHANNEL + 0x00u] = ((chcr & 0x0000FFFFu) | (tag.upper << 16u)) & ~0x100u;
    m_ioRegisters[GIF_CHANNEL + 0x20u] = 0u;

    uint32_t dstat = m_ioRegisters.count(D_STAT) ? m_ioRegisters[D_STAT] : 0u;
    dstat |= (1u << 2u);
    const uint32_t status = dstat & 0x3FFu;
    const uint32_t mask = (dstat >> 16u) & 0x3FFu;
    if ((status & mask) != 0u)
        dstat |= (1u << 31u);
    else
        dstat &= ~(1u << 31u);
    m_ioRegisters[D_STAT] = dstat;
    queueCompletedDmacCause(2u);
    return true;
}

int PS2Memory::pollDmaRegisters()
{
    return 0;
}

uint32_t PS2Memory::readIORegister(uint32_t address)
{
    if (g_lazyVif1Pending && address >= 0x10009000u && address < 0x10009100u)
        ps2xFlushLazyVif1Chain(*this);
    // TS2: Sony's sceGsSyncPath (original code) waits in loops for D1_CHCR.STR, D2_CHCR.STR, VIF1_STAT and GIF_STAT.
    // On real hardware the transfers continue in parallel; here deferred transfers are processed when these status
    // registers are read (as the HLE version did before querying). Without this the original ran until its
    // time limit (16 million queries) and halved the frame rate.
    if (address == 0x10009000u || address == 0x1000A000u || address == 0x10003C00u || address == 0x10003020u)
    {
        static const bool aus = std::getenv("PS2X_POLL_NO_FLUSH") != nullptr;
        if (!aus) processPendingTransfers();
    }
    size_t timerIndex = 0u;
    uint32_t timerOffset = 0u;
    if (decodeEeTimerRegister(address, timerIndex, timerOffset))
    {
        const EeTimer &timer = m_eeTimers[timerIndex];
        switch (timerOffset)
        {
        case kEeTimerCountOffset:
            if (g_hbVsync && (timer.mode & kEeTimerModeClksMask) == 3u && (timer.mode & kEeTimerModeCue) != 0u &&
                (timer.mode & (kEeTimerModeCmpe | kEeTimerModeOvfe | kEeTimerModeZret)) == 0u)
            {
                // catch up directly on read: the VSync may have come since the last batch step
                HbAnker &h = g_hb[timerIndex];
                const uint64_t vs = gs().vsyncTick.load(std::memory_order_relaxed);
                if (vs != h.vs) { h.vs = vs; h.cyc = 0u; }
                const uint64_t ziel = hbZiel(h, eeTimerClockHz(3u));
                if (h.init && ziel > h.letzter)
                {
                    m_eeTimers[timerIndex].count = static_cast<uint32_t>((timer.count + (ziel - h.letzter)) & 0xFFFFu);
                    h.letzter = ziel;
                }
            }
            if (static const bool lg = std::getenv("PS2X_TIMERLOG") != nullptr; lg)
            {
                static uint64_t n[4]; static uint32_t letzter[4];
                if ((++n[timerIndex] % 2000u) == 1u)
                    std::fprintf(stderr, "[TIMER] T%zu COUNT read #%llu value=%u (last %u)\n", timerIndex, (unsigned long long)n[timerIndex], timer.count, letzter[timerIndex]);
                letzter[timerIndex] = timer.count;
            }
            return timer.count & 0xFFFFu;
        case kEeTimerModeOffset:
            return timer.mode & (kEeTimerModeConfigMask | kEeTimerModeStatusMask);
        case kEeTimerCompareOffset:
            return timer.compare & 0xFFFFu;
        case kEeTimerHoldOffset:
            return timer.hold & 0xFFFFu;
        default:
            return 0u;
        }
    }

    if (isGsPrivReg(address))
    {
        // NB: unreachable from read8/16/32/64 today, same reasoning as the write
        // path above; kept correct for direct callers.
        const uint32_t off = address & 7u;
        const uint32_t regOff = (address - PS2_GS_PRIV_REG_BASE) & ~0x7u;
        if (regOff == kGsCsrRegOffset)
        {
            return static_cast<uint32_t>((gs_regs.csr.load() >> (off * 8u)) & 0xFFFFFFFFull);
        }
        if (uint64_t *reg = gsRegPtr(gs_regs, address))
        {
            return static_cast<uint32_t>((*reg >> (off * 8u)) & 0xFFFFFFFFull);
        }
        return 0u;
    }

    if (address >= 0x10002000 && address <= 0x10002030)
    {
        uint32_t val = 0;
        switch (address)
        {
        case 0x10002000:
            val = m_ioRegisters[address];
            break;
        case 0x10002010:
            val = m_ioRegisters[address] & ~(1u << 31);
            break;
        case 0x10002020:
        case 0x10002030:
            val = m_ioRegisters[address];
            break;
        default:
            val = 0;
            break;
        }
        return val;
    }

    if (address == 0x10003020u) // GIF_STAT
    {
        uint32_t stat = m_ioRegisters.count(address) ? m_ioRegisters[address] : 0u;
        const uint32_t mode = m_ioRegisters.count(0x10003010u) ? m_ioRegisters[0x10003010u] : 0u;
        const uint32_t ctrl = m_ioRegisters.count(0x10003000u) ? m_ioRegisters[0x10003000u] : 0u;

        // M3R and IMT mirror GIF_MODE, PSE mirrors GIF_CTRL, and M3P is the
        // effective PATH3 mask controlled by the VIF1 MSKPATH3 command.
        stat = (stat & ~0xFu) |
               (mode & 0x1u) |
               (m_path3Masked ? 0x2u : 0u) |
               (mode & 0x4u) |
               (ctrl & 0x8u);
        return stat;
    }

    if (address >= 0x10000000 && address < 0x10010000)
    {
        if (address >= 0x10008000 && address < 0x1000F000)
        {
            if ((address & 0xFF) == 0x00)
            {
                uint32_t channelStatus = m_ioRegisters[address] & ~0x100u;
                m_ioRegisters[address] = channelStatus;
                return channelStatus;
            }
        }

        if (address >= 0x10000200 && address < 0x10000300)
        {
            return 0;
        }

        if (address >= 0x1000F200 && address <= 0x1000F260)
        {
            if (address == 0x1000F230)
            {
                return 0x60000;
            }
            if (address == 0x1000F240)
            {
                return 0xF0000002;
            }
            return 0;
        }
    }

    auto it = m_ioRegisters.find(address);
    if (it != m_ioRegisters.end())
    {
        return it->second;
    }

    return 0;
}

void PS2Memory::registerCodeRegion(uint32_t start, uint32_t end)
{
    if (end <= start)
    {
        std::cerr << "Ignoring invalid code region: start=0x" << std::hex << start
                  << " end=0x" << end << std::dec << std::endl;
        return;
    }

    if ((end - start) > PS2_RAM_SIZE)
    {
        std::cerr << "Ignoring oversized code region: start=0x" << std::hex << start
                  << " end=0x" << end << std::dec << std::endl;
        return;
    }

    for (const auto &existing : m_codeRegions)
    {
        if (existing.start == start && existing.end == end)
        {
            return;
        }
    }

    CodeRegion region;
    region.start = start;
    region.end = end;

    size_t sizeInWords = (end - start + 3u) / 4u;
    region.modified.resize(sizeInWords, false);

    m_codeRegions.push_back(region);
    RUNTIME_LOG("Registered code region: " << std::hex << start << " - " << end << std::dec);
}

bool PS2Memory::isAddressInRegion(uint32_t address, const CodeRegion &region)
{
    return (address >= region.start && address < region.end);
}

bool PS2Memory::isCodeAddress(uint32_t address) const
{
    for (const auto &region : m_codeRegions)
    {
        if (address >= region.start && address < region.end)
        {
            return true;
        }
    }
    return false;
}

void PS2Memory::markModified(uint32_t address, uint32_t size)
{
    if (size == 0)
    {
        return;
    }

    const uint64_t writeEnd = static_cast<uint64_t>(address) + static_cast<uint64_t>(size);
    for (auto &region : m_codeRegions)
    {
        const uint64_t regionStart = region.start;
        const uint64_t regionEnd = region.end;
        if (writeEnd <= regionStart || static_cast<uint64_t>(address) >= regionEnd)
        {
            continue;
        }

        uint32_t overlapStart = static_cast<uint32_t>(std::max<uint64_t>(address, regionStart));
        uint32_t overlapEnd = static_cast<uint32_t>(std::min<uint64_t>(writeEnd, regionEnd));

        for (uint32_t addr = overlapStart; addr < overlapEnd; addr += 4)
        {
            size_t bitIndex = (addr - region.start) / 4;
            if (bitIndex < region.modified.size())
            {
                region.modified[bitIndex] = true;
                RUNTIME_LOG("Marked code at " << std::hex << addr << std::dec << " as modified");
            }
        }
    }
}

bool PS2Memory::isCodeModified(uint32_t address, uint32_t size)
{
    if (size == 0)
    {
        return false;
    }

    const uint64_t writeEnd = static_cast<uint64_t>(address) + static_cast<uint64_t>(size);
    for (const auto &region : m_codeRegions)
    {
        const uint64_t regionStart = region.start;
        const uint64_t regionEnd = region.end;
        if (writeEnd <= regionStart || static_cast<uint64_t>(address) >= regionEnd)
        {
            continue;
        }

        uint32_t overlapStart = static_cast<uint32_t>(std::max<uint64_t>(address, regionStart));
        uint32_t overlapEnd = static_cast<uint32_t>(std::min<uint64_t>(writeEnd, regionEnd));

        for (uint32_t addr = overlapStart; addr < overlapEnd; addr += 4)
        {
            size_t bitIndex = (addr - region.start) / 4;
            if (bitIndex < region.modified.size() && region.modified[bitIndex])
            {
                return true; // Found modified code
            }
        }
    }

    return false; // No modifications found
}

void PS2Memory::clearModifiedFlag(uint32_t address, uint32_t size)
{
    if (size == 0)
    {
        return;
    }

    const uint64_t writeEnd = static_cast<uint64_t>(address) + static_cast<uint64_t>(size);
    for (auto &region : m_codeRegions)
    {
        const uint64_t regionStart = region.start;
        const uint64_t regionEnd = region.end;
        if (writeEnd <= regionStart || static_cast<uint64_t>(address) >= regionEnd)
        {
            continue;
        }

        uint32_t overlapStart = static_cast<uint32_t>(std::max<uint64_t>(address, regionStart));
        uint32_t overlapEnd = static_cast<uint32_t>(std::min<uint64_t>(writeEnd, regionEnd));

        for (uint32_t addr = overlapStart; addr < overlapEnd; addr += 4)
        {
            size_t bitIndex = (addr - region.start) / 4;
            if (bitIndex < region.modified.size())
            {
                region.modified[bitIndex] = false;
            }
        }
    }
}
