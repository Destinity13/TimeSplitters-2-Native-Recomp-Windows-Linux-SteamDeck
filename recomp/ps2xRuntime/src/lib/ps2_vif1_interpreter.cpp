// Based on Blackline Interactive implementation
#include "runtime/ps2_memory.h"
#include <cstring>
#include <vector>
#include <cstdlib>
#include <cstdio>
#include <atomic>
bool ps2xGsBridgeActive(); // gs_frontend.cpp (globaler Namensraum)
void ps2xVu1Fence();      // ps2_memory.cpp (MTVU)

namespace sotc_vif_trace
{
    std::atomic<uint64_t> g_op[128];
    std::atomic<uint64_t> g_calls{0}, g_bytes{0};
    // UNPACK statistics per format (vn<<2|vl): vectors in the fast / general path, reasons for the general one
    std::atomic<uint64_t> g_unpFastVec[16], g_unpSlowVec[16], g_unpSlowMask{0}, g_unpSlowMode{0}, g_unpSlowCycle{0}, g_unpSlowV45{0};
    bool enabled()
    {
        static const bool on = std::getenv("SOTC_TRACE_VIF") != nullptr;
        return on;
    }
    void dump()
    {
        std::fprintf(stderr, "[VIF1-UNPACK] slow because of mask=%llu mode=%llu CL!=WL=%llu V4-5=%llu; vectors fast/slow per format:",
                     (unsigned long long)g_unpSlowMask.load(), (unsigned long long)g_unpSlowMode.load(), (unsigned long long)g_unpSlowCycle.load(), (unsigned long long)g_unpSlowV45.load());
        static const char *kFmt[16] = {"S-32","S-16","S-8","?","V2-32","V2-16","V2-8","?","V3-32","V3-16","V3-8","?","V4-32","V4-16","V4-8","V4-5"};
        for (int i = 0; i < 16; ++i)
            if (g_unpFastVec[i].load() || g_unpSlowVec[i].load())
                std::fprintf(stderr, " %s:%llu/%llu", kFmt[i], (unsigned long long)g_unpFastVec[i].load(), (unsigned long long)g_unpSlowVec[i].load());
        std::fprintf(stderr, "\n");
        std::fprintf(stderr, "[VIF1] calls=%llu bytes=%llu ops:", (unsigned long long)g_calls.load(), (unsigned long long)g_bytes.load());
        for (int i = 0; i < 128; ++i)
            if (g_op[i].load())
                std::fprintf(stderr, " %02x:%llu", i, (unsigned long long)g_op[i].load());
        std::fprintf(stderr, "\n");
    }
}

enum VIFCmd : uint8_t
{
    VIF_NOP = 0x00,
    VIF_STCYCL = 0x01,
    VIF_OFFSET = 0x02,
    VIF_BASE = 0x03,
    VIF_ITOP = 0x04,
    VIF_STMOD = 0x05,
    VIF_MSKPATH3 = 0x06,
    VIF_MARK = 0x07,
    VIF_FLUSHE = 0x10,
    VIF_FLUSH = 0x11,
    VIF_FLUSHA = 0x13,
    VIF_MSCAL = 0x14,
    VIF_MSCALF = 0x15,
    VIF_MSCNT = 0x17,
    VIF_STMASK = 0x20,
    VIF_STROW = 0x30,
    VIF_STCOL = 0x31,
    VIF_MPG = 0x4A,
    VIF_DIRECT = 0x50,
    VIF_DIRECTHL = 0x51,
};

namespace
{
    constexpr uint8_t kGifFmtImage = 2u;

    uint32_t pendingGifImageQwc(const uint8_t *data, uint32_t sizeBytes)
    {
        if (!data || sizeBytes < 16u)
            return 0u;

        uint32_t offset = 0u;
        while (offset + 16u <= sizeBytes)
        {
            uint64_t tagLo = 0u;
            std::memcpy(&tagLo, data + offset, sizeof(tagLo));
            offset += 16u;

            const uint32_t nloop = static_cast<uint32_t>(tagLo & 0x7FFFu);
            const uint8_t flg = static_cast<uint8_t>((tagLo >> 58) & 0x3u);
            uint32_t nreg = static_cast<uint32_t>((tagLo >> 60) & 0xFu);
            if (nreg == 0u)
                nreg = 16u;

            uint64_t payloadBytes = 0u;
            if (flg == 0u) // PACKED
            {
                payloadBytes = static_cast<uint64_t>(nloop) * nreg * 16ull;
            }
            else if (flg == 1u) // REGLIST, padded to a quadword
            {
                payloadBytes = static_cast<uint64_t>(nloop) * nreg * 8ull;
                payloadBytes = (payloadBytes + 15ull) & ~15ull;
            }
            else if (flg == kGifFmtImage || flg == 3u) // FLG=3 ("disabled" in the GS manual) behaves exactly like IMAGE: NLOOP qwords of image data.
            {
                payloadBytes = static_cast<uint64_t>(nloop) * 16ull;
                const uint64_t availableBytes = sizeBytes - offset;
                if (payloadBytes > availableBytes)
                {
                    return static_cast<uint32_t>((payloadBytes - availableBytes) / 16ull);
                }
            }
            else
            {
                return 0u;
            }

            if (payloadBytes > static_cast<uint64_t>(sizeBytes - offset))
                return 0u;
            offset += static_cast<uint32_t>(payloadBytes);
        }

        return 0u;
    }
}

void PS2Memory::processVIF0Data(uint32_t srcPhys, uint32_t sizeBytes)
{
    if (sizeBytes == 0u || srcPhys >= PS2_RAM_SIZE)
        return;

    const uint64_t requestedEnd = static_cast<uint64_t>(srcPhys) + static_cast<uint64_t>(sizeBytes);
    if (requestedEnd > static_cast<uint64_t>(PS2_RAM_SIZE))
        sizeBytes = PS2_RAM_SIZE - srcPhys;

    processVIF0Data(m_rdram + srcPhys, sizeBytes);
}

void PS2Memory::processVIF0Data(const uint8_t *data, uint32_t sizeBytes)
{
    if (sizeBytes == 0u)
        return;

    uint32_t pos = 0;
    while (pos + 4 <= sizeBytes)
    {
        uint32_t cmd = 0u;
        std::memcpy(&cmd, data + pos, sizeof(cmd));
        pos += 4u;

        const uint8_t opcode = static_cast<uint8_t>((cmd >> 24) & 0x7Fu);
        const uint16_t imm = static_cast<uint16_t>(cmd & 0xFFFFu);
        const uint8_t num = static_cast<uint8_t>((cmd >> 16) & 0xFFu);
        const bool irq = (cmd & 0x80000000u) != 0u;

        vif0_regs.code = cmd;
        vif0_regs.num = num;
        if (irq)
            vif0_regs.stat |= (1u << 11);

        if (opcode == VIF_NOP)
        {
            continue;
        }
        else if (opcode == VIF_STCYCL)
        {
            vif0_regs.cycle = imm;
            continue;
        }
        else if (opcode == VIF_ITOP)
        {
            vif0_regs.itops = imm & 0x3FFu;
            continue;
        }
        else if (opcode == VIF_STMOD)
        {
            vif0_regs.mode = imm & 3u;
            continue;
        }
        else if (opcode == VIF_MARK)
        {
            vif0_regs.mark = imm;
            vif0_regs.stat |= (1u << 6);
            continue;
        }
        else if (opcode == VIF_FLUSHE || opcode == VIF_FLUSH || opcode == VIF_FLUSHA)
        {
            continue;
        }
        else if (opcode == VIF_STMASK)
        {
            if (pos + 4u > sizeBytes)
                break;
            std::memcpy(&vif0_regs.mask, data + pos, sizeof(vif0_regs.mask));
            pos += 4u;
            continue;
        }
        else if (opcode == VIF_STROW)
        {
            if (pos + 16u > sizeBytes)
                break;
            std::memcpy(vif0_regs.row, data + pos, 16u);
            pos += 16u;
            continue;
        }
        else if (opcode == VIF_STCOL)
        {
            if (pos + 16u > sizeBytes)
                break;
            std::memcpy(vif0_regs.col, data + pos, 16u);
            pos += 16u;
            continue;
        }
        else if (opcode == VIF_MPG)
        {
            const uint32_t destAddr = static_cast<uint32_t>(imm & 0x1FFu) * 8u;
            const uint32_t instructionCount = (num == 0u) ? 256u : static_cast<uint32_t>(num);
            const uint32_t mpgBytes = instructionCount * 8u;
            uint32_t copyBytes = 0u;
            if (m_vu0Code && destAddr < PS2_VU0_CODE_SIZE && mpgBytes > 0u)
            {
                copyBytes = mpgBytes;
                if (destAddr + copyBytes > PS2_VU0_CODE_SIZE)
                    copyBytes = PS2_VU0_CODE_SIZE - destAddr;
                if (pos + copyBytes <= sizeBytes)
                {
                    std::memcpy(m_vu0Code + destAddr, data + pos, copyBytes);
                    markVU0CodeModified();
                }
            }

            pos += mpgBytes;
            if (pos > sizeBytes)
                break;
            continue;
        }
        else if ((opcode & 0x60u) == 0x60u)
        {
            const uint8_t vn = static_cast<uint8_t>((opcode >> 2) & 0x3u);
            const uint8_t vl = static_cast<uint8_t>(opcode & 0x3u);
            const int components = static_cast<int>(vn) + 1;
            int bitsPerComponent = 32;
            switch (vl)
            {
            case 0:
                bitsPerComponent = 32;
                break;
            case 1:
                bitsPerComponent = 16;
                break;
            case 2:
                bitsPerComponent = 8;
                break;
            case 3:
                bitsPerComponent = (vn == 3u) ? 4 : 16;
                break;
            default:
                break;
            }
            const int bitsPerVector = (vl == 3u && vn == 3u) ? 16 : (components * bitsPerComponent);
            uint32_t bytesPerVector = static_cast<uint32_t>((bitsPerVector + 7) / 8);
            const uint32_t writeVectorCount = (num == 0u) ? 256u : static_cast<uint32_t>(num);
            uint32_t cl = vif0_regs.cycle & 0xFFu;
            uint32_t wl = (vif0_regs.cycle >> 8) & 0xFFu;
            if (cl == 0u)
                cl = 1u;
            if (wl == 0u)
                wl = 1u;
            uint32_t sourceVectorCount = writeVectorCount;
            if (cl < wl)
            {
                const uint32_t fullBlocks = writeVectorCount / wl;
                uint32_t remainder = writeVectorCount % wl;
                if (remainder > cl)
                    remainder = cl;
                sourceVectorCount = fullBlocks * cl + remainder;
            }
            uint32_t totalBytes = sourceVectorCount * bytesPerVector;
            totalBytes = (totalBytes + 3u) & ~3u;

            if (m_vu0Data && pos + totalBytes <= sizeBytes && vl == 0u)
            {
                uint32_t vuAddr = static_cast<uint32_t>(imm & 0x3FFu);
                if ((imm & 0x8000u) != 0u)
                    vuAddr = (vuAddr + (vif0_regs.tops & 0x3FFu)) & 0x3FFu;
                const uint8_t *srcBase = data + pos;
                uint32_t srcIndex = 0u;
                for (uint32_t writeIndex = 0; writeIndex < writeVectorCount; ++writeIndex)
                {
                    const uint32_t cyclePos = writeIndex % wl;
                    const bool sourceAvailable = (cl >= wl) || (cyclePos < cl);
                    uint32_t destVec = (cl >= wl) ? ((vuAddr + (writeIndex / wl) * cl + cyclePos) & 0x3FFu)
                                                  : ((vuAddr + writeIndex) & 0x3FFu);
                    const uint32_t destOff = destVec * 16u;
                    if (destOff + 16u > PS2_VU0_DATA_SIZE)
                    {
                        if (sourceAvailable && srcIndex < sourceVectorCount)
                            ++srcIndex;
                        continue;
                    }
                    if (!sourceAvailable || srcIndex >= sourceVectorCount)
                        continue;
                    const uint8_t *srcVec = srcBase + srcIndex * bytesPerVector;
                    ++srcIndex;
                    uint32_t lanes[4] = {0u, 0u, 0u, 0u};
                    std::memcpy(lanes, m_vu0Data + destOff, sizeof(lanes));
                    const uint32_t limit = (components > 4) ? 4u : static_cast<uint32_t>(components);
                    for (uint32_t c = 0; c < limit; ++c)
                    {
                        uint32_t scalar = 0u;
                        std::memcpy(&scalar, srcVec + c * 4u, sizeof(scalar));
                        lanes[c] = scalar;
                    }
                    _mm_storeu_si128(reinterpret_cast<__m128i *>(m_vu0Data + destOff), _mm_loadu_si128(reinterpret_cast<const __m128i *>(lanes)));
                }
            }
            pos += totalBytes;
            if (pos > sizeBytes)
                break;
            continue;
        }
        else
        {
            break;
        }
    }
}

void PS2Memory::processVIF1Data(uint32_t srcPhys, uint32_t sizeBytes)
{
    if (sizeBytes == 0u || srcPhys >= PS2_RAM_SIZE)
        return;

    const uint64_t requestedEnd = static_cast<uint64_t>(srcPhys) + static_cast<uint64_t>(sizeBytes);
    if (requestedEnd > static_cast<uint64_t>(PS2_RAM_SIZE))
        sizeBytes = PS2_RAM_SIZE - srcPhys;

    processVIF1Data(m_rdram + srcPhys, sizeBytes);
}

namespace { std::vector<uint8_t> g_vif1Carry; }

// UNPACK without a mask (format V2/V3/V4/S, 8/16/32 bits): skipping/filling write mode (CL != WL) and MODE 1/2
// (offset/difference with the row register) specialised per format. Bit-identical to the general loop in
// processVIF1Data (fields beyond the components keep the old memory value and get, with MODE 1/2,
// ebenfalls Row addiert; Fuellzyklen ohne Quelle schreiben Row).
namespace
{
    template <int VL>
    inline uint32_t vif1UnpackLoad(const uint8_t *sv, uint32_t c, bool zeroExtend)
    {
        if constexpr (VL == 0) { uint32_t w; std::memcpy(&w, sv + c * 4u, 4); return w; }
        else if constexpr (VL == 1)
        {
            uint16_t raw; std::memcpy(&raw, sv + c * 2u, 2);
            return zeroExtend ? static_cast<uint32_t>(raw) : static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(raw)));
        }
        else
        {
            const uint8_t raw = sv[c];
            return zeroExtend ? static_cast<uint32_t>(raw) : static_cast<uint32_t>(static_cast<int32_t>(static_cast<int8_t>(raw)));
        }
    }

    template <int VL, int N>
    void vif1UnpackNoMask(uint8_t *vu1Data, const uint8_t *srcBase, uint32_t bytesPerVector, uint32_t writeVectorCount,
                          uint32_t sourceVectorCount, uint32_t vuAddr, uint32_t cl, uint32_t wl, bool zeroExtend,
                          uint32_t mode, uint32_t *row)
    {
        const bool skipping = cl >= wl;
        const bool addRow = (mode == 1u || mode == 2u);
        uint32_t srcIndex = 0u, cp = 0u, blk = 0u;
        for (uint32_t wi = 0; wi < writeVectorCount; ++wi)
        {
            const uint32_t destVec = (skipping ? (vuAddr + blk * cl + cp) : (vuAddr + wi)) & 0x3FFu;
            uint8_t *dst = vu1Data + destVec * 16u;
            if ((skipping || cp < cl) && srcIndex < sourceVectorCount)
            {
                const uint8_t *sv = srcBase + srcIndex * bytesPerVector;
                ++srcIndex;
                uint32_t d[4];
                std::memcpy(d, dst, 16);
                if constexpr (N == 1)
                {
                    const uint32_t v0 = vif1UnpackLoad<VL>(sv, 0u, zeroExtend);
                    d[0] = d[1] = d[2] = d[3] = v0;
                }
                else
                {
                    for (uint32_t c = 0; c < static_cast<uint32_t>(N); ++c)
                        d[c] = vif1UnpackLoad<VL>(sv, c, zeroExtend);
                }
                if (addRow)
                {
                    for (uint32_t f = 0; f < 4u; ++f)
                    {
                        d[f] += row[f];
                        if (mode == 2u)
                            row[f] = d[f];
                    }
                }
                std::memcpy(dst, d, 16);
            }
            else
                std::memcpy(dst, row, 16);
            if (++cp == wl)
            {
                cp = 0u;
                ++blk;
            }
        }
    }

    using Vif1UnpackFn = void (*)(uint8_t *, const uint8_t *, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, bool, uint32_t, uint32_t *);
    // index (vn << 2) | vl; vl == 3 (V4-5) not here
    const Vif1UnpackFn kVif1UnpackNoMask[16] = {
        vif1UnpackNoMask<0, 1>, vif1UnpackNoMask<1, 1>, vif1UnpackNoMask<2, 1>, nullptr,
        vif1UnpackNoMask<0, 2>, vif1UnpackNoMask<1, 2>, vif1UnpackNoMask<2, 2>, nullptr,
        vif1UnpackNoMask<0, 3>, vif1UnpackNoMask<1, 3>, vif1UnpackNoMask<2, 3>, nullptr,
        vif1UnpackNoMask<0, 4>, vif1UnpackNoMask<1, 4>, vif1UnpackNoMask<2, 4>, nullptr,
    };
}

void PS2Memory::processVIF1Data(const uint8_t *data, uint32_t sizeBytes)
{
    std::vector<uint8_t> merged;
    if (!g_vif1Carry.empty())
    {
        merged.swap(g_vif1Carry);
        if (data && sizeBytes)
            merged.insert(merged.end(), data, data + sizeBytes);
        data = merged.data();
        sizeBytes = static_cast<uint32_t>(merged.size());
    }
    auto impl = [&](const uint8_t *data, uint32_t sizeBytes) -> void
    {
    if (sizeBytes == 0u)
        return;
    if (sotc_vif_trace::enabled())
    {
        sotc_vif_trace::g_calls.fetch_add(1, std::memory_order_relaxed);
        sotc_vif_trace::g_bytes.fetch_add(sizeBytes, std::memory_order_relaxed);
    }

    uint32_t pos = 0;

    while (pos + 4 <= sizeBytes)
    {
        uint32_t cmd;
        memcpy(&cmd, data + pos, 4);
        pos += 4;
        const uint32_t cmdStart = pos - 4u;
        // A VIFcode whose payload continues in the next DMA transfer: keep it and finish it then.
        auto carryRest = [&]() { g_vif1Carry.assign(data + cmdStart, data + sizeBytes); };

        uint8_t opcode = (cmd >> 24) & 0x7F;
        {   // ps2x (TS2): PS2X_VIFPOS=from,to,minSize -> print every VIF command in the window (large blocks)
            static const struct { uint32_t a, b, m; bool on; } vp = [] {
                const char *e = std::getenv("PS2X_VIFPOS"); unsigned a = 0, b = 0, m = 0;
                const bool on = e && std::sscanf(e, "%u,%u,%u", &a, &b, &m) == 3; return decltype(vp){a, b, m, on}; }();
            if (vp.on && sizeBytes >= vp.m && cmdStart >= vp.a && cmdStart <= vp.b)
                std::fprintf(stderr, "[vifpos] pos=%u cmd=%08X op=%02X imm=%u num=%u carry=%d\n", cmdStart, cmd, opcode, cmd & 0xFFFFu, (cmd >> 16) & 0xFFu, (int)!g_vif1Carry.empty());
        }
        if (sotc_vif_trace::enabled())
        {
            sotc_vif_trace::g_op[opcode].fetch_add(1, std::memory_order_relaxed);
            static thread_local uint32_t ringCmd[8], ringPos[8];
            static thread_local unsigned ringN = 0;
            static std::atomic<int> dumps{0};
            const bool valid = opcode <= 0x07 || opcode == 0x10 || opcode == 0x11 || opcode == 0x13 || opcode == 0x14 ||
                               opcode == 0x15 || opcode == 0x17 || opcode == 0x20 || opcode == 0x30 || opcode == 0x31 ||
                               opcode == 0x4A || opcode == 0x50 || opcode == 0x51 || opcode >= 0x60;
            if (!valid && dumps.fetch_add(1) < 6)
            {
                std::fprintf(stderr, "[VIF1 bad] op=%02x cmd=%08x pos=%u size=%u cycle=%08x prev:", opcode, cmd, pos - 4, sizeBytes, vif1_regs.cycle);
                for (unsigned k = 0; k < 8 && k < ringN; ++k)
                {
                    unsigned i = (ringN - 1 - k) & 7;
                    std::fprintf(stderr, " [%u]%08x", ringPos[i], ringCmd[i]);
                }
                std::fprintf(stderr, "\n  bytes:");
                const uint32_t from = (pos >= 68u) ? pos - 68u : 0u;
                for (uint32_t b = from; b < pos + 28u && b + 4u <= sizeBytes; b += 4)
                {
                    uint32_t w; std::memcpy(&w, data + b, 4);
                    std::fprintf(stderr, "%s%08x", (b == pos - 4) ? " >" : " ", w);
                }
                std::fprintf(stderr, "\n");
            }
            ringCmd[ringN & 7] = cmd; ringPos[ringN & 7] = pos - 4; ++ringN;
        }
        uint16_t imm = cmd & 0xFFFF;
        uint8_t num = (cmd >> 16) & 0xFF;
        const bool irq = (cmd & 0x80000000u) != 0u;

        // Track most-recent command for VIFn_CODE emulation.
        vif1_regs.code = cmd;
        vif1_regs.num = num;
        if (irq)
            vif1_regs.stat |= (1u << 11); // INT

        if (opcode == VIF_NOP)
        {
            continue;
        }
        else if (opcode == VIF_STCYCL)
        {
            vif1_regs.cycle = imm;
            continue;
        }
        else if (opcode == VIF_OFFSET)
        {
            // VIF double-buffer setup. OFFSET clears DBF and resets TOPS to BASE.
            // Do not rewrite BASE from the previous TOPS value.
            vif1_regs.ofst = imm & 0x3FFu;
            vif1_regs.tops = vif1_regs.base & 0x3FFu;
            vif1_regs.stat &= ~(1u << 7); // clear DBF
            continue;
        }
        else if (opcode == VIF_BASE)
        {
            // BASE only updates the base register. TOPS changes on OFFSET/MSCAL.
            vif1_regs.base = imm & 0x3FFu;
            continue;
        }
        else if (opcode == VIF_ITOP)
        {
            // ITOP VIFcode writes pending ITOPS; VU XITOP observes it after MSCAL/MSCNT.
            vif1_regs.itops = imm & 0x3FFu;
            continue;
        }
        else if (opcode == VIF_STMOD)
        {
            vif1_regs.mode = imm & 3u;
            continue;
        }
        else if (opcode == VIF_MSKPATH3)
        {
            // VIF command docs: MSKPATH3 uses IMMEDIATE bit 15.
            const bool wasMasked = m_path3Masked;
            m_path3Masked = (imm & 0x8000u) != 0u;
            if (wasMasked && !m_path3Masked)
                flushMaskedPath3Packets();
            continue;
        }
        else if (opcode == VIF_MARK)
        {
            vif1_regs.mark = imm;
            vif1_regs.stat |= (1u << 6); // MRK
            continue;
        }
        else if (opcode == VIF_FLUSHE || opcode == VIF_FLUSH || opcode == VIF_FLUSHA)
        {
            ps2xVu1Fence(); // waits for the end of the VU1 program like the real VIF
            continue;
        }
        else if (opcode == VIF_MSCAL || opcode == VIF_MSCALF)
        {
            uint32_t startPC = (uint32_t)imm * 8u;

            const uint32_t runTop = vif1_regs.tops & 0x3FFu;
            const uint32_t runItop = vif1_regs.itops & 0x3FFu;
            vif1_regs.top = runTop;
            vif1_regs.itop = runItop;

            const bool dbf = (vif1_regs.stat & (1u << 7)) != 0u;
            if (dbf)
                vif1_regs.tops = vif1_regs.base & 0x3FFu;
            else
                vif1_regs.tops = (vif1_regs.base + vif1_regs.ofst) & 0x3FFu;
            vif1_regs.stat ^= (1u << 7); // toggle DBF

            if (m_vu1MscalCallback)
                m_vu1MscalCallback(startPC, runTop, runItop);
            continue;
        }
        else if (opcode == VIF_MSCNT)
        {
            const uint32_t runTop = vif1_regs.tops & 0x3FFu;
            const uint32_t runItop = vif1_regs.itops & 0x3FFu;
            vif1_regs.top = runTop;
            vif1_regs.itop = runItop;

            const bool dbf = (vif1_regs.stat & (1u << 7)) != 0u;
            if (dbf)
                vif1_regs.tops = vif1_regs.base & 0x3FFu;
            else
                vif1_regs.tops = (vif1_regs.base + vif1_regs.ofst) & 0x3FFu;
            vif1_regs.stat ^= (1u << 7); // toggle DBF

            if (m_vu1MscntCallback)
                m_vu1MscntCallback(runTop, runItop);
            continue;
        }
        else if (opcode == VIF_STMASK)
        {
            if (pos + 4 > sizeBytes)
            {
                carryRest();
                return;
            }
            uint32_t maskValue = 0;
            std::memcpy(&maskValue, data + pos, sizeof(maskValue));
            vif1_regs.mask = maskValue;
            pos += 4;
            continue;
        }
        else if (opcode == VIF_STROW)
        {
            if (pos + 16 > sizeBytes)
            {
                carryRest();
                return;
            }
            std::memcpy(vif1_regs.row, data + pos, 16);
            pos += 16;
            continue;
        }
        else if (opcode == VIF_STCOL)
        {
            if (pos + 16 > sizeBytes)
            {
                carryRest();
                return;
            }
            std::memcpy(vif1_regs.col, data + pos, 16);
            pos += 16;
            continue;
        }
        else if (opcode == VIF_MPG)
        {
            ps2xVu1Fence(); // only replace the micro program once the running one is done
            uint32_t destAddr = (uint32_t)imm * 8u;
            const uint32_t instructionCount = (num == 0u) ? 256u : static_cast<uint32_t>(num);
            const uint32_t mpgBytes = instructionCount * 8u;
            if (pos + mpgBytes > sizeBytes)
            {
                carryRest();
                return;
            }
            if (sotc_vif_trace::enabled())
            {
                static std::atomic<int> mpgLogs{0};
                if (mpgLogs.fetch_add(1) < 40)
                    std::fprintf(stderr, "[VIF1 MPG] dest=0x%X instr=%u pos=%u size=%u fits=%d\n", destAddr, instructionCount, pos, sizeBytes, (int)(pos + mpgBytes <= sizeBytes));
            }
            if (m_vu1Code && destAddr < PS2_VU1_CODE_SIZE && mpgBytes > 0)
            {
                uint32_t copyBytes = mpgBytes;
                if (destAddr + copyBytes > PS2_VU1_CODE_SIZE)
                    copyBytes = PS2_VU1_CODE_SIZE - destAddr;
                if (pos + copyBytes <= sizeBytes)
                {
                    std::memcpy(m_vu1Code + destAddr, data + pos, copyBytes);
                    markVU1CodeModified();
                }
            }
            pos += mpgBytes;
            if (pos > sizeBytes)
                break;
            continue;
        }
        else if (opcode == VIF_DIRECT || opcode == VIF_DIRECTHL)
        {
            uint32_t qwCount = imm;
            if (qwCount == 0)
                qwCount = 65536;
            const uint32_t availableQw = (sizeBytes - pos) / 16u;
            const bool truncated = qwCount > availableQw;
            if (truncated && qwCount < 65536u)
            {
                carryRest();
                return;
            }
            if (qwCount > availableQw)
                qwCount = availableQw;

            const uint8_t *directData = data + pos;
            uint32_t directQw = qwCount;
            // A GIF IMAGE transfer can span several VIF DIRECT commands: the GIFtag arrives in one
            // DIRECT, the pixel data in the following ones. Real VIF only forwards data via DIRECT,
            // so continue the pending IMAGE here instead of eating raw words after the tag.
            // With the PCSX2 GS its GIF parser keeps the PATH2 state itself: pass follow-up data on raw.
            // Artificial IMAGE tag for our GS; the PCSX2 bridge gets the data without it (bridgeStripTag)
            // because PCSX2's GIF parser keeps the PATH2 state itself.
            if (directQw > 0 && m_vif1PendingPath2ImageQwc != 0u)
            {
                const uint32_t chunkQw = std::min<uint32_t>(m_vif1PendingPath2ImageQwc, directQw);
                std::vector<uint8_t> imagePacket(16u + static_cast<size_t>(chunkQw) * 16u, 0u);
                const uint64_t imageTag =
                    static_cast<uint64_t>(chunkQw & 0x7FFFu) |
                    ((m_vif1PendingPath2ImageQwc == chunkQw) ? (1ull << 15) : 0ull) |
                    (static_cast<uint64_t>(kGifFmtImage) << 58);
                std::memcpy(imagePacket.data(), &imageTag, sizeof(imageTag));
                std::memcpy(imagePacket.data() + 16u, directData, static_cast<size_t>(chunkQw) * 16u);
                submitGifPacket(GifPathId::Path2, imagePacket.data(), static_cast<uint32_t>(imagePacket.size()), true, m_vif1PendingPath2DirectHl, true);
                m_vif1PendingPath2ImageQwc -= chunkQw;
                if (m_vif1PendingPath2ImageQwc == 0u)
                    m_vif1PendingPath2DirectHl = false;
                directData += chunkQw * 16u;
                directQw -= chunkQw;
            }

            if (directQw > 0)
            {
                const bool directHl = (opcode == VIF_DIRECTHL);
                submitGifPacket(GifPathId::Path2, directData, directQw * 16, true, directHl);

                const uint32_t pendingImageQw = pendingGifImageQwc(directData, directQw * 16u);
                if (pendingImageQw != 0u)
                {
                    static std::atomic<int> idumps{0};
                    if (sotc_vif_trace::enabled() && idumps.fetch_add(1) < 4)
                    {
                        std::fprintf(stderr, "[VIF1 img] direct qw=%u pendingImgQw=%u at pos=%u; next words:", qwCount, pendingImageQw, pos);
                        for (uint32_t b = pos + qwCount * 16u; b < pos + qwCount * 16u + 40u && b + 4u <= sizeBytes; b += 4)
                        {
                            uint32_t w; std::memcpy(&w, data + b, 4);
                            std::fprintf(stderr, " %08x", w);
                        }
                        std::fprintf(stderr, "\n");
                    }
                    m_vif1PendingPath2ImageQwc = pendingImageQw;
                    m_vif1PendingPath2DirectHl = directHl;
                }
            }

            pos += qwCount * 16;
            if (truncated)
            {
                pos = sizeBytes;
                break;
            }
            continue;
        }
        else if ((opcode & 0x60) == 0x60)
        {
            uint8_t vn = (opcode >> 2) & 0x3;
            uint8_t vl = opcode & 0x3;
            const bool maskEnable = (opcode & 0x10u) != 0u;
            int components = vn + 1;
            int bitsPerComponent = 32;
            switch (vl)
            {
            case 0:
                bitsPerComponent = 32;
                break;
            case 1:
                bitsPerComponent = 16;
                break;
            case 2:
                bitsPerComponent = 8;
                break;
            case 3:
                bitsPerComponent = (vn == 3) ? 4 : 16;
                break;
            default:
                break;
            }
            int bitsPerVector = (vl == 3 && vn == 3) ? 16 : (components * bitsPerComponent);
            uint32_t bytesPerVector = (bitsPerVector + 7) / 8;
            // UNPACK semantics: NUM is 8-bit and NUM==0 means 256 vectors (writes).
            const uint32_t writeVectorCount = (num == 0u) ? 256u : static_cast<uint32_t>(num);

            // STCYCL controls write cycles for UNPACK.
            uint32_t cl = vif1_regs.cycle & 0xFFu;
            uint32_t wl = (vif1_regs.cycle >> 8) & 0xFFu;
            if (cl == 0u)
                cl = 1u;
            if (wl == 0u)
                wl = 1u;

            uint32_t sourceVectorCount = writeVectorCount;
            if (cl < wl)
            {
                const uint32_t fullBlocks = writeVectorCount / wl;
                uint32_t remainder = writeVectorCount % wl;
                if (remainder > cl)
                    remainder = cl;
                sourceVectorCount = fullBlocks * cl + remainder;
            }

            uint32_t totalBytes = sourceVectorCount * bytesPerVector;
            totalBytes = (totalBytes + 3) & ~3u;

            uint32_t vuAddr = (uint32_t)imm & 0x3FFu;
            if ((imm & 0x8000u) != 0u)
                vuAddr = (vuAddr + (vif1_regs.tops & 0x3FFu)) & 0x3FFu;

            const bool zeroExtend = (imm & 0x4000u) != 0u;

            if (totalBytes > 0 && pos + totalBytes > sizeBytes)
            {
                carryRest();
                return;
            }
            // Fast path (like PCSX2's pre-generated unpack routines) for the normal case: no mask, MODE 0,
            // CL == WL (write consecutively). Same result as the general loop below.
            if (sotc_vif_trace::enabled())
            {
                const bool fast = !maskEnable && (vif1_regs.mode & 3u) == 0u && cl == wl && vl != 3u;
                (fast ? sotc_vif_trace::g_unpFastVec : sotc_vif_trace::g_unpSlowVec)[(vn << 2) | vl].fetch_add(writeVectorCount, std::memory_order_relaxed);
                if (maskEnable) sotc_vif_trace::g_unpSlowMask.fetch_add(writeVectorCount, std::memory_order_relaxed);
                if ((vif1_regs.mode & 3u) != 0u) sotc_vif_trace::g_unpSlowMode.fetch_add(writeVectorCount, std::memory_order_relaxed);
                if (cl != wl) sotc_vif_trace::g_unpSlowCycle.fetch_add(writeVectorCount, std::memory_order_relaxed);
                if (vl == 3u) sotc_vif_trace::g_unpSlowV45.fetch_add(writeVectorCount, std::memory_order_relaxed);
            }
            if (m_vu1Data && totalBytes > 0 && pos + totalBytes <= sizeBytes && !maskEnable &&
                (vif1_regs.mode & 3u) == 0u && cl == wl && vl != 3u)
            {
                const uint8_t *src = data + pos;
                const uint32_t comps = static_cast<uint32_t>(components);
                for (uint32_t i = 0; i < writeVectorCount; ++i, src += bytesPerVector)
                {
                    uint8_t *dst = m_vu1Data + (((vuAddr + i) & 0x3FFu) * 16u);
                    if (vl == 0u)
                    {
                        if (comps == 1u)
                        {
                            uint32_t v; std::memcpy(&v, src, 4);
                            const uint32_t q[4] = {v, v, v, v};
                            std::memcpy(dst, q, 16);
                        }
                        else
                            std::memcpy(dst, src, comps * 4u);
                    }
                    else
                    {
                        uint32_t q[4];
                        std::memcpy(q, dst, 16);
                        for (uint32_t c = 0; c < comps; ++c)
                        {
                            if (vl == 1u)
                            {
                                uint16_t raw; std::memcpy(&raw, src + c * 2u, 2);
                                q[c] = zeroExtend ? static_cast<uint32_t>(raw) : static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(raw)));
                            }
                            else
                            {
                                const uint8_t raw = src[c];
                                q[c] = zeroExtend ? static_cast<uint32_t>(raw) : static_cast<uint32_t>(static_cast<int32_t>(static_cast<int8_t>(raw)));
                            }
                        }
                        if (comps == 1u)
                            q[1] = q[2] = q[3] = q[0];
                        std::memcpy(dst, q, 16);
                    }
                }
                pos += totalBytes;
                if (pos > sizeBytes)
                    break;
                continue;
            }
            // Specialised routines for UNPACK without a mask (CL != WL and/or MODE 1/2), see vif1UnpackNoMask
            // PS2X_VIF_CHECK=1: compute the special routine on a copy, then the general loop, then compare
            static const bool vifCheck = std::getenv("PS2X_VIF_CHECK") != nullptr;
            static thread_local uint8_t chkMem[PS2_VU1_DATA_SIZE];
            static thread_local uint32_t chkRow[4];
            bool chkPending = false;
            if (m_vu1Data && totalBytes > 0 && pos + totalBytes <= sizeBytes && !maskEnable && vl != 3u &&
                kVif1UnpackNoMask[(vn << 2) | vl] != nullptr)
            {
                if (!vifCheck)
                {
                    kVif1UnpackNoMask[(vn << 2) | vl](m_vu1Data, data + pos, bytesPerVector, writeVectorCount, sourceVectorCount,
                                                      vuAddr, cl, wl, zeroExtend, vif1_regs.mode & 3u, vif1_regs.row);
                    pos += totalBytes;
                    if (pos > sizeBytes)
                        break;
                    continue;
                }
                std::memcpy(chkMem, m_vu1Data, PS2_VU1_DATA_SIZE);
                std::memcpy(chkRow, vif1_regs.row, sizeof(chkRow));
                kVif1UnpackNoMask[(vn << 2) | vl](chkMem, data + pos, bytesPerVector, writeVectorCount, sourceVectorCount,
                                                  vuAddr, cl, wl, zeroExtend, vif1_regs.mode & 3u, chkRow);
                chkPending = true;
            }
            if (m_vu1Data && totalBytes > 0 && pos + totalBytes <= sizeBytes)
            {
                const uint8_t *srcBase = data + pos;
                uint32_t srcIndex = 0u;
                for (uint32_t writeIndex = 0; writeIndex < writeVectorCount; ++writeIndex)
                {
                    const uint32_t cyclePos = writeIndex % wl;
                    const bool sourceAvailable = (cl >= wl) || (cyclePos < cl);

                    uint32_t destVec = 0;
                    if (cl >= wl)
                    {
                        destVec = (vuAddr + (writeIndex / wl) * cl + cyclePos) & 0x3FFu;
                    }
                    else
                    {
                        destVec = (vuAddr + writeIndex) & 0x3FFu;
                    }

                    uint32_t destOff = destVec * 16u;
                    if (destOff + 16u > PS2_VU1_DATA_SIZE)
                    {
                        if (sourceAvailable && srcIndex < sourceVectorCount)
                            ++srcIndex;
                        continue;
                    }

                    uint32_t lanes[4] = {0u, 0u, 0u, 0u};
                    std::memcpy(lanes, m_vu1Data + destOff, sizeof(lanes));
                    uint32_t decompressed[4] = {lanes[0], lanes[1], lanes[2], lanes[3]};
                    bool decoded = false;

                    const uint8_t *srcVec = nullptr;
                    if (sourceAvailable && srcIndex < sourceVectorCount)
                    {
                        srcVec = srcBase + srcIndex * bytesPerVector;
                        ++srcIndex;
                        decoded = true;
                    }

                    auto extend16 = [&](uint16_t raw) -> uint32_t
                    {
                        if (zeroExtend)
                            return static_cast<uint32_t>(raw);
                        return static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(raw)));
                    };

                    auto extend8 = [&](uint8_t raw) -> uint32_t
                    {
                        if (zeroExtend)
                            return static_cast<uint32_t>(raw);
                        return static_cast<uint32_t>(static_cast<int32_t>(static_cast<int8_t>(raw)));
                    };

                    bool handledFormat = true;
                    if (!decoded)
                    {
                        handledFormat = false;
                    }
                    else if (vl == 0u)
                    {
                        if (components == 1)
                        {
                            uint32_t scalar = 0;
                            std::memcpy(&scalar, srcVec, sizeof(scalar));
                            decompressed[0] = scalar;
                            decompressed[1] = scalar;
                            decompressed[2] = scalar;
                            decompressed[3] = scalar;
                        }
                        else
                        {
                            const uint32_t limit = (components > 4) ? 4u : static_cast<uint32_t>(components);
                            for (uint32_t c = 0; c < limit; ++c)
                            {
                                uint32_t scalar = 0;
                                std::memcpy(&scalar, srcVec + c * 4u, sizeof(scalar));
                                decompressed[c] = scalar;
                            }
                        }
                    }
                    else if (vl == 1u)
                    {
                        if (components == 1)
                        {
                            uint16_t raw = 0;
                            std::memcpy(&raw, srcVec, sizeof(raw));
                            const uint32_t scalar = extend16(raw);
                            decompressed[0] = scalar;
                            decompressed[1] = scalar;
                            decompressed[2] = scalar;
                            decompressed[3] = scalar;
                        }
                        else
                        {
                            const uint32_t limit = (components > 4) ? 4u : static_cast<uint32_t>(components);
                            for (uint32_t c = 0; c < limit; ++c)
                            {
                                uint16_t raw = 0;
                                std::memcpy(&raw, srcVec + c * 2u, sizeof(raw));
                                decompressed[c] = extend16(raw);
                            }
                        }
                    }
                    else if (vl == 2u)
                    {
                        if (components == 1)
                        {
                            const uint32_t scalar = extend8(srcVec[0]);
                            decompressed[0] = scalar;
                            decompressed[1] = scalar;
                            decompressed[2] = scalar;
                            decompressed[3] = scalar;
                        }
                        else
                        {
                            const uint32_t limit = (components > 4) ? 4u : static_cast<uint32_t>(components);
                            for (uint32_t c = 0; c < limit; ++c)
                            {
                                decompressed[c] = extend8(srcVec[c]);
                            }
                        }
                    }
                    else if (vl == 3u && vn == 3u)
                    {
                        // V4-5: packed color-like format in a single 16-bit value.
                        uint16_t packed = 0;
                        std::memcpy(&packed, srcVec, sizeof(packed));
                        decompressed[0] = packed & 0x1Fu;
                        decompressed[1] = (packed >> 5) & 0x1Fu;
                        decompressed[2] = (packed >> 10) & 0x1Fu;
                        decompressed[3] = (packed >> 15) & 0x01u;
                    }
                    else
                    {
                        handledFormat = false;
                    }

                    // Unknown compressed format fallback: preserve legacy raw-copy behavior.
                    if (!handledFormat && decoded && !maskEnable && (vif1_regs.mode == 0u || vif1_regs.mode == 3u))
                    {
                        uint32_t copyBytes = (bytesPerVector < 16u) ? bytesPerVector : 16u;
                        std::memcpy(m_vu1Data + destOff, srcVec, copyBytes);
                        continue;
                    }

                    const bool canAdd = (vl != 3u || vn != 3u);
                    const uint32_t mode = vif1_regs.mode & 3u;
                    const uint32_t colIdx = (cyclePos > 3u) ? 3u : cyclePos;
                    const uint32_t maskCycle = (cyclePos > 3u) ? 3u : cyclePos;

                    for (uint32_t field = 0u; field < 4u; ++field)
                    {
                        uint32_t maskSpec = 0u;
                        if (maskEnable)
                        {
                            const uint32_t shift = ((maskCycle * 4u) + field) * 2u;
                            maskSpec = (vif1_regs.mask >> shift) & 0x3u;
                        }

                        // In fill-write cycles with suspended source reads, treat raw-data selections as row-fill.
                        if (!decoded && maskSpec == 0u)
                            maskSpec = 1u;

                        uint32_t writeVal = lanes[field];
                        if (maskSpec == 0u)
                        {
                            if (handledFormat)
                            {
                                writeVal = decompressed[field];
                                if (canAdd && (mode == 1u || mode == 2u))
                                {
                                    writeVal = writeVal + vif1_regs.row[field];
                                    if (mode == 2u)
                                        vif1_regs.row[field] = writeVal;
                                }
                            }
                        }
                        else if (maskSpec == 1u)
                        {
                            writeVal = vif1_regs.row[field];
                        }
                        else if (maskSpec == 2u)
                        {
                            writeVal = vif1_regs.col[colIdx];
                        }
                        else
                        {
                            continue; // write-protect
                        }

                        lanes[field] = writeVal;
                    }

                    std::memcpy(m_vu1Data + destOff, lanes, sizeof(lanes));
                }
            }
            if (chkPending)
            {
                static std::atomic<uint64_t> s_n{0}, s_bad{0};
                const bool bad = std::memcmp(chkMem, m_vu1Data, PS2_VU1_DATA_SIZE) != 0 || std::memcmp(chkRow, vif1_regs.row, sizeof(chkRow)) != 0;
                const uint64_t n = s_n.fetch_add(1) + 1, nb = bad ? s_bad.fetch_add(1) + 1 : s_bad.load();
                if (bad && nb <= 5)
                    std::fprintf(stderr, "[VIF-CHECK] difference: format vn=%u vl=%u cl=%u wl=%u mode=%u num=%u\n", vn, vl, cl, wl, vif1_regs.mode & 3u, writeVectorCount);
                if ((n & 0xFFFFFu) == 0u)
                    std::fprintf(stderr, "[VIF-CHECK] %llu UNPACKs geprueft, %llu Abweichungen\n", (unsigned long long)n, (unsigned long long)nb);
            }
            pos += totalBytes;

            if (pos > sizeBytes)
                break;
            continue;
        }
        else
        {
            continue;
        }
    }
    };
    impl(data, sizeBytes);
}
