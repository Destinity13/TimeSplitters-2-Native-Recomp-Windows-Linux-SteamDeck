#include <functional>
#include <memory>
#include "runtime/gs/gs_frontend.h"
#include "ps2x/iop/spu_bridge.h"
#include "runtime/gs/gs_cpu_backend.h"
#include "runtime/gs/gs_threaded_backend.h"
#if defined(_WIN32)
// Windows port: libps2xgs.dll through LoadLibrary instead of dlopen
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#undef EXCEPTION_BREAKPOINT // clashes with PS2Exception in ps2_runtime.h
#include <cstdio>
#define RTLD_NOW 0
#define RTLD_LOCAL 0
#define RTLD_GLOBAL 0
#define RTLD_NOLOAD 0
static void *dlopen(const char *f, int) { return reinterpret_cast<void *>(LoadLibraryA(f)); }
static void *dlsym(void *h, const char *n) { return reinterpret_cast<void *>(GetProcAddress(static_cast<HMODULE>(h), n)); }
static const char *dlerror() { static char b[64]; std::snprintf(b, sizeof(b), "LoadLibrary error %lu", static_cast<unsigned long>(GetLastError())); return b; }
#else
#include <dlfcn.h>
#endif
#include <filesystem>
#include <system_error>
#include "ps2_log.h"
#include "runtime/ps2_memory.h"
#include "ps2_runtime.h"
#include "runtime/ee_scheduler.h"
#include <atomic>
#include <thread>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <sstream>

// GS event (SIGNAL/FINISH set): wake the EE scheduler right away so that it raises the GS interrupt (INTC 0) at once
// like real hardware - otherwise it was only checked at the next scheduler event (VBlank), and a game
// waiting for the interrupt (TS2: SIGNAL at the end of the frame) lost one VBlank per frame (30 instead of 60 fps).
// ExternalWake with type 0xFFFFFFFF matches no waiting thread but triggers processPendingEvents/pollGsInterrupt.
extern PS2Runtime *g_ps2xMtvuRuntime;
static void ps2xWakeForGsEvent()
{
    static const bool aus = std::getenv("PS2X_GS_EVENT_NO_WAKE") != nullptr;
    if (!aus && g_ps2xMtvuRuntime)
        g_ps2xMtvuRuntime->eeScheduler().postEvent(EeEvent{EeEventType::ExternalWake, 0xFFFFFFFFu, 0u});
}

namespace
{
    static constexpr uint32_t kHostFrameWidth = 640u;

    GSPrimReg decodePrimRegister(uint64_t value)
    {
        GSPrimReg prim{};
        prim.type = static_cast<GSPrimType>(value & 0x7u);
        prim.iip = ((value >> 3) & 1u) != 0u;
        prim.tme = ((value >> 4) & 1u) != 0u;
        prim.fge = ((value >> 5) & 1u) != 0u;
        prim.abe = ((value >> 6) & 1u) != 0u;
        prim.aa1 = ((value >> 7) & 1u) != 0u;
        prim.fst = ((value >> 8) & 1u) != 0u;
        prim.ctxt = ((value >> 9) & 1u) != 0u;
        prim.fix = ((value >> 10) & 1u) != 0u;
        return prim;
    }

    static inline uint64_t loadLE64(const uint8_t *p)
    {
        uint64_t v;
        std::memcpy(&v, p, 8);
        return v;
    }

    struct PackedGifPacketTag
    {
        uint64_t lo = 0u;
        uint64_t hi = 0u;
        uint32_t payloadOffset = 0u;
        uint32_t nloop = 0u;
        uint32_t nreg = 0u;
        uint8_t regs[16]{};
    };

    template <typename Visitor>
    bool visitPackedGifPacket(const uint8_t *data, uint32_t sizeBytes, Visitor &&visitor)
    {
        uint32_t offset = 0u;
        while (offset + 16u <= sizeBytes)
        {
            PackedGifPacketTag tag{};
            tag.lo = loadLE64(data + offset);
            tag.hi = loadLE64(data + offset + 8u);

            const uint8_t flg = static_cast<uint8_t>((tag.lo >> 58u) & 0x3u);
            if (flg != GIF_FMT_PACKED)
                return false;

            tag.nloop = static_cast<uint32_t>(tag.lo & 0x7FFFu);
            tag.nreg = static_cast<uint32_t>((tag.lo >> 60u) & 0xFu);
            if (tag.nreg == 0u)
                tag.nreg = 16u;

            const uint64_t payloadBytes64 =
                static_cast<uint64_t>(tag.nloop) * static_cast<uint64_t>(tag.nreg) * 16ull;
            if (payloadBytes64 > 0xFFFFFFFFull)
                return false;

            offset += 16u;
            const uint32_t payloadBytes = static_cast<uint32_t>(payloadBytes64);
            if (payloadBytes > sizeBytes - offset)
                return false;

            tag.payloadOffset = offset;
            for (uint32_t i = 0u; i < tag.nreg; ++i)
                tag.regs[i] = static_cast<uint8_t>((tag.hi >> (i * 4u)) & 0xFu);

            if (!visitor(tag))
                return false;

            offset += payloadBytes;
        }

        return offset == sizeBytes;
    }

    bool validatePackedGifPacket(const uint8_t *data, uint32_t sizeBytes)
    {
        return visitPackedGifPacket(data, sizeBytes, [](const PackedGifPacketTag &)
                                    { return true; });
    }

    std::atomic<uint32_t> s_debugGifPacketCount{0};
    std::atomic<uint32_t> s_debugGsRegisterCount{0};
    std::atomic<uint32_t> s_debugGsPackedVertexCount{0};
    std::atomic<uint32_t> s_debugGsVertexKickCount{0};
    std::atomic<uint32_t> s_debugCopyRegCount{0};
    std::atomic<uint32_t> s_debugTexaWriteCount{0};
    std::atomic<uint32_t> s_debugCvFontUploadCount{0};
    std::atomic<uint32_t> s_debugLocalCopyCount{0};
}


namespace
{
    // PS2X_MTGS=1: rasterizer in its own thread (GSThreadedBackend), otherwise synchronous as before.
    std::unique_ptr<GSRasterBackend> makeGsBackend()
    {
        const char *e = std::getenv("PS2X_MTGS");
        if (e && *e && *e != '0')
            return std::make_unique<GSThreadedBackend>(std::make_unique<GSCpuBackend>());
        return std::make_unique<GSCpuBackend>();
    }
}


// ---------------------------------------------------------------------------------------------------------
// PCSX2-GS-Bruecke (PS2X_PCSX2_GS=/pfad/libps2xgs.so): Der nativ uebersetzte Spielcode bleibt unveraendert,
// only the GS command packets additionally go to PCSX2's graphics chip (Vulkan/OpenGL, upscaling).
// Our own rasterizer then no longer draws (saves CPU); transfers/VRAM for readback remain.
// ---------------------------------------------------------------------------------------------------------
extern thread_local int g_ps2xCurrentGifPath;
extern thread_local bool g_ps2xBridgeStripTag;
extern std::atomic<bool> g_ps2xSkipDraws;
extern std::atomic<bool> g_ps2xOwnRasterOff;
namespace ps2xbridge
{
    using StartFn = int (*)(const char *, int, float, int);
    using TransferFn = void (*)(int, const unsigned char *, unsigned int);
    using VsyncFn = void (*)(const unsigned char *, int);
    using QueueFn = unsigned int (*)();
    using ShotFn = void (*)(const char *);
    ShotFn s_shot = nullptr;
    using PadFn = int (*)(unsigned short *, unsigned char *);
    PadFn s_pad = nullptr;
    using RumbleFn = void (*)(int, int);
    RumbleFn s_rumble = nullptr;
    using MenuFn = int (*)();
    MenuFn s_menu = nullptr;
    MenuFn s_wideMode = nullptr;
    MenuFn s_aspectMilli = nullptr;
    MenuFn s_turbo = nullptr;
    using WideFn = void (*)(int);
    WideFn s_gameWide = nullptr;
    using OptFn = int (*)(int, int);
    OptFn s_option = nullptr;
    using OnlineFn = void (*)();
    OnlineFn s_onlineBrowser = nullptr;
    using StatusFn = void (*)(const char *);
    StatusFn s_onlineStatus = nullptr;
    using TextFn = void (*)(int, const char *);
    TextFn s_text = nullptr;   // ps2xgs_text: generic text events for the overlay (Online+ killfeed, scoreboard)
    using TextGetFn = int (*)(int, char *, int);
    TextGetFn s_textGet = nullptr;   // ps2xgs_text_get: text from the bridge (Online+ server list)
    using FlushFn = void (*)();
    using ReadFifoFn = void (*)(unsigned char *, unsigned int);
    ReadFifoFn s_readFifo = nullptr;
    using PadNFn = int (*)(int, unsigned short *, unsigned char *);
    PadNFn s_padN = nullptr;
    using MouseFn = int (*)(int *, int *);
    MouseFn s_mouse = nullptr;
    using CaptureEndFn = void (*)();
    CaptureEndFn s_captureEnd = nullptr;
    using ReadFifoAsyncFn = void *(*)(unsigned char *, unsigned int);
    using ReadFifoWaitFn = void (*)(void *);
    ReadFifoAsyncFn s_readFifoAsync = nullptr;
    ReadFifoWaitFn s_readFifoWait = nullptr;
    FlushFn s_flush = nullptr;
    StartFn s_start = nullptr; TransferFn s_transfer = nullptr; VsyncFn s_vsync = nullptr; QueueFn s_queue = nullptr;
    bool s_active = false;

    bool init()
    {
        static bool tried = false;
        if (tried) return s_active;
        tried = true;
        const char *lib = std::getenv("PS2X_PCSX2_GS");
        if (!lib || !*lib) return false;
        void *h = dlopen(lib, RTLD_NOW | RTLD_LOCAL);
        if (!h) { std::fprintf(stderr, "[ps2xgs] dlopen failed: %s\n", dlerror()); return false; }
        s_start = reinterpret_cast<StartFn>(dlsym(h, "ps2xgs_start"));
        s_transfer = reinterpret_cast<TransferFn>(dlsym(h, "ps2xgs_transfer"));
        s_vsync = reinterpret_cast<VsyncFn>(dlsym(h, "ps2xgs_vsync"));
        s_queue = reinterpret_cast<QueueFn>(dlsym(h, "ps2xgs_queue_size"));
        s_shot = reinterpret_cast<ShotFn>(dlsym(h, "ps2xgs_screenshot"));
        s_pad = reinterpret_cast<PadFn>(dlsym(h, "ps2xgs_pad"));
        s_rumble = reinterpret_cast<RumbleFn>(dlsym(h, "ps2xgs_rumble"));
        s_menu = reinterpret_cast<MenuFn>(dlsym(h, "ps2xgs_menu_open"));
        s_wideMode = reinterpret_cast<MenuFn>(dlsym(h, "ps2xgs_wide_mode"));
        s_aspectMilli = reinterpret_cast<MenuFn>(dlsym(h, "ps2xgs_aspect_milli"));   // newer bridges only (ultrawide)
        s_turbo = reinterpret_cast<MenuFn>(dlsym(h, "ps2xgs_turbo"));
        s_gameWide = reinterpret_cast<WideFn>(dlsym(h, "ps2xgs_game_wide"));
        s_option = reinterpret_cast<OptFn>(dlsym(h, "ps2xgs_option"));
        s_onlineBrowser = reinterpret_cast<OnlineFn>(dlsym(h, "ps2xgs_online_browser"));
        s_onlineStatus = reinterpret_cast<StatusFn>(dlsym(h, "ps2xgs_online_status"));
        s_text = reinterpret_cast<TextFn>(dlsym(h, "ps2xgs_text"));
        s_textGet = reinterpret_cast<TextGetFn>(dlsym(h, "ps2xgs_text_get"));
        s_flush = reinterpret_cast<FlushFn>(dlsym(h, "ps2xgs_flush"));
        s_readFifo = reinterpret_cast<ReadFifoFn>(dlsym(h, "ps2xgs_read_fifo"));
        s_padN = reinterpret_cast<PadNFn>(dlsym(h, "ps2xgs_pad_n"));
        s_mouse = reinterpret_cast<MouseFn>(dlsym(h, "ps2xgs_mouse"));
        s_captureEnd = reinterpret_cast<CaptureEndFn>(dlsym(h, "ps2xgs_capture_end"));
        s_readFifoAsync = reinterpret_cast<ReadFifoAsyncFn>(dlsym(h, "ps2xgs_read_fifo_async"));
        s_readFifoWait = reinterpret_cast<ReadFifoWaitFn>(dlsym(h, "ps2xgs_read_fifo_wait"));
        if (!s_start || !s_transfer || !s_vsync) { std::fprintf(stderr, "[ps2xgs] symbols missing\n"); return false; }
        const char *e;
        const int renderer = (e = std::getenv("PS2X_PCSX2_RENDERER")) ? std::atoi(e) : 14;   // 14 = Vulkan
        const float upscale = (e = std::getenv("PS2X_PCSX2_UPSCALE")) ? static_cast<float>(std::atof(e)) : 3.0f;
        const int wide = (e = std::getenv("PS2X_PCSX2_WIDESCREEN")) ? std::atoi(e) : 0;
        const char *live = std::getenv("PS2X_PCSX2_LIVEFILE");
        // Default: the system temp directory (Windows has no /tmp -> "C:\\tmp" is missing, the file could not be
        // created and PCSX2 aborted the VM start: "VM nach 10 s nicht bereit")
        static std::string livePath = live ? live : [] {
            std::error_code ec;
            const std::filesystem::path t = std::filesystem::temp_directory_path(ec);
            return (ec ? std::filesystem::path(".") : t) / "ps2x_live.gs";
        }().string();
        std::fprintf(stderr, "[ps2xgs] live file: %s\n", livePath.c_str());
        if (FILE *f = std::fopen(livePath.c_str(), "ab")) std::fclose(f);   // Datei muss existieren (VMManager)
        if (!s_start(livePath.c_str(), renderer, upscale, wide))
        {
            std::fprintf(stderr, "[ps2xgs] start failed (renderer %d)\n", renderer);
            // PS2XGS_VM_PFLICHT=1: the launcher then tries another renderer (exit code 3) instead of the built-in GS
            if (const char *pf = std::getenv("PS2XGS_VM_PFLICHT"); pf && *pf == '1')
            {
                std::fflush(nullptr);
#ifdef _WIN32
                // not through ExitProcess: the atexit hook of the bridge DLL would otherwise exit with code 0
                TerminateProcess(GetCurrentProcess(), 3);
#endif
                std::_Exit(3);
            }
            return false;
        }
        s_active = true;
        // Sound: attach the real SPU2 chip (PCSX2) to the IOP; PS2X_SPU2=0 switches back
        if (const char *sp = std::getenv("PS2X_SPU2"); !(sp && *sp == '0'))
        {
            auto &b = ps2x::iop::g_spuBridge;
            b.write = reinterpret_cast<void (*)(unsigned int, unsigned short, unsigned int)>(dlsym(h, "ps2xspu_write"));
            b.read = reinterpret_cast<unsigned short (*)(unsigned int, unsigned int)>(dlsym(h, "ps2xspu_read"));
            b.dma = reinterpret_cast<void (*)(int, unsigned short *, unsigned int, int, unsigned int)>(dlsym(h, "ps2xspu_dma"));
            b.async = reinterpret_cast<void (*)(unsigned int)>(dlsym(h, "ps2xspu_async"));
            b.takeIrqs = reinterpret_cast<unsigned int (*)()>(dlsym(h, "ps2xspu_take_irqs"));
            if (!(b.write && b.read && b.dma && b.async && b.takeIrqs))
                b = ps2x::iop::SpuBridge{};
            else
                std::fprintf(stderr, "[ps2xgs] SPU2 (sound) active\n");
        }
        if (!std::getenv("PS2X_PCSX2_ALSO_SW"))
            g_ps2xOwnRasterOff.store(true, std::memory_order_relaxed);
        std::fprintf(stderr, "[ps2xgs] PCSX2 GS active (renderer %d, scale %.1fx)\n", renderer, upscale);
        return true;
    }
}

bool ps2xGsBridgeActive() { return ps2xbridge::init(); }

// X11 must be made thread-safe before the first Xlib call (raylib/GLFW window) because PCSX2's window
// runs in its own thread. So as a constructor at program start, only if the bridge is wanted.
__attribute__((constructor)) static void ps2xEarlyXInitThreads()
{
    const char *lib = std::getenv("PS2X_PCSX2_GS");
    if (!lib || !*lib) return;
    if (void *x = dlopen("libX11.so.6", RTLD_NOW | RTLD_GLOBAL))
        if (auto fn = reinterpret_cast<int (*)()>(dlsym(x, "XInitThreads")))
            fn();
}
extern std::atomic<unsigned> g_ps2xPadZusatz;
extern std::atomic<unsigned> g_ps2xPadMaske;
extern std::atomic<unsigned> g_ps2xPadRoh;
extern std::atomic<unsigned> g_ps2xPadRStickAus;
extern std::atomic<unsigned> g_ps2xPadAchsenRoh;
extern std::atomic<unsigned> g_ps2xPadRStickSetzen;
// Input from the PCSX2 game window (keyboard) + SDL3 gamepad. false = bridge without input -> old path (raylib).
bool ps2xGsBridgePad(uint16_t &pressed, uint8_t axes[4])
{
    // Do not call ps2xbridge::init(): the pad query must not start the bridge prematurely.
    if (!ps2xbridge::s_active || !ps2xbridge::s_pad) return false;
    unsigned short p = 0;
    if (!ps2xbridge::s_pad(&p, axes)) return false;
    // Mouse aiming (TS2): hold the aim mode button while playing -> original crosshair (ts2_override sets the bit)
    g_ps2xPadRoh.store(p, std::memory_order_relaxed);
    // axes as the player moved them (lx, ly, rx, ry); modern gamepad aiming (TS2) turns the view itself and takes the
    // right stick away from the game
    g_ps2xPadAchsenRoh.store(static_cast<unsigned>(axes[0]) | (static_cast<unsigned>(axes[1]) << 8) | (static_cast<unsigned>(axes[2]) << 16) | (static_cast<unsigned>(axes[3]) << 24), std::memory_order_relaxed);
    if (g_ps2xPadRStickAus.load(std::memory_order_relaxed)) axes[2] = axes[3] = 0x80;
    // right stick set by the game override (TS2: the mouse steers a turret through the right stick)
    if (const unsigned rs = g_ps2xPadRStickSetzen.load(std::memory_order_relaxed); rs & 0x10000u) { axes[2] = static_cast<uint8_t>(rs); axes[3] = static_cast<uint8_t>(rs >> 8); }
    p |= static_cast<unsigned short>(g_ps2xPadZusatz.load(std::memory_order_relaxed));
    // released for a few VBlanks so the game sees a fresh press (sniper scope opens only on an L2 edge)
    p &= static_cast<unsigned short>(~g_ps2xPadMaske.load(std::memory_order_relaxed));
    pressed = p;
    return true;
}
std::atomic<unsigned> g_ps2xPadZusatz{0};   // PS2 buttons additionally pressed by the game override (player 1)
std::atomic<unsigned> g_ps2xPadMaske{0};    // PS2 buttons the override forces released (player 1), after Zusatz
std::atomic<unsigned> g_ps2xPadRStickAus{0};        // 1 = the game gets a centred right stick (player 1)
std::atomic<unsigned> g_ps2xPadRStickSetzen{0};     // 0x10000 | rx | ry << 8 = right stick set by the override (player 1)
std::atomic<unsigned> g_ps2xPadAchsenRoh{0x80808080u}; // player 1 axes as moved (lx | ly<<8 | rx<<16 | ry<<24)
std::atomic<unsigned> g_ps2xPadRoh{0};      // player 1 buttons as the player pressed them (without Zusatz/Maske)
// Mouse aiming: relative mouse movement collected since the last call (only while the game window captures the mouse).
// Returns: 0 = mouse not captured (menu open, window without focus, mouse aiming off), otherwise 1.
// Finish a running video recording (F8 / PS2XGS_CAPTURE) before exiting
void ps2xGsBridgeCaptureEnd()
{
    if (ps2xbridge::s_active && ps2xbridge::s_captureEnd) ps2xbridge::s_captureEnd();
}

int ps2xGsBridgeMouse(int &dx, int &dy)
{
    dx = dy = 0;
    if (!ps2xbridge::s_active || !ps2xbridge::s_mouse) return 0;
    return ps2xbridge::s_mouse(&dx, &dy);
}
void ps2xGsBridgeRumble(int smallOn, int large)
{
    if (ps2xbridge::s_active && ps2xbridge::s_rumble)
        ps2xbridge::s_rumble(smallOn, large);
}
int ps2xGsBridgeWideMode()
{
    return (ps2xbridge::init() && ps2xbridge::s_wideMode) ? ps2xbridge::s_wideMode() : 0;
}
// Ultrawide: window width/height x 1000 when the player chose "any window shape", otherwise 0 (also with older bridges)
int ps2xGsBridgeAspectMilli()
{
    return (ps2xbridge::s_active && ps2xbridge::s_aspectMilli) ? ps2xbridge::s_aspectMilli() : 0;
}
void ps2xGsBridgeGameWide(int wide)
{
    if (ps2xbridge::s_active && ps2xbridge::s_gameWide)
        ps2xbridge::s_gameWide(wide);
}
// Hand this thread's batched graphics packets to PCSX2 (ordering between EE and VU1 thread)
void ps2xGsBridgeFlush()
{
    if (ps2xbridge::s_active && ps2xbridge::s_flush)
        ps2xbridge::s_flush();
}
// GS download (VIF1 -> memory, BUSDIR=1): read qwc quadwords from the PCSX2 GS (MTGS::InitAndReadFIFO)
bool ps2xGsBridgeReadFifo(uint8_t *dst, uint32_t qwc)
{
    if (!ps2xbridge::s_active || !ps2xbridge::s_readFifo) return false;
    if (ps2xbridge::s_flush) ps2xbridge::s_flush();
    ps2xbridge::s_readFifo(dst, qwc);
    return true;
}
// Input for players 2..4 (further gamepads, split screen); false = no device
bool ps2xGsBridgePadN(int player, uint16_t &pressed, uint8_t axes[4])
{
    if (!ps2xbridge::init() || !ps2xbridge::s_padN) return false;
    unsigned short p = 0;
    if (!ps2xbridge::s_padN(player, &p, axes)) return false;
    pressed = p;
    return true;
}
// Asynchronous GS download: send the job, the EE carries on; wait for the result only at completion (VBlank).
// PS2X_DL_SYNC=1 forces the old synchronous path. Returns nullptr = not available.
void *ps2xGsBridgeReadFifoAsync(uint8_t *dst, uint32_t qwc)
{
    static const bool sync = std::getenv("PS2X_DL_SYNC") != nullptr;
    if (sync || !ps2xbridge::s_active || !ps2xbridge::s_readFifoAsync || !ps2xbridge::s_readFifoWait) return nullptr;
    if (ps2xbridge::s_flush) ps2xbridge::s_flush();
    return ps2xbridge::s_readFifoAsync(dst, qwc);
}
void ps2xGsBridgeReadFifoWait(void *ticket)
{
    if (ticket && ps2xbridge::s_readFifoWait) ps2xbridge::s_readFifoWait(ticket);
}
// Game options from the menu (e.g. 0 = camera blur); fallback without the bridge
// Online (TS2): open the server browser in the game menu / show the connection status
void ps2xGsBridgeOnlineBrowser()
{
    if (ps2xbridge::s_active && ps2xbridge::s_onlineBrowser) ps2xbridge::s_onlineBrowser();
}
void ps2xGsBridgeOnlineStatus(const char *text)
{
    if (ps2xbridge::s_active && ps2xbridge::s_onlineStatus) ps2xbridge::s_onlineStatus(text);
}
int ps2xGsBridgeTextGet(int id, char *buf, int len)
{
    if (buf && len > 0) buf[0] = 0;
    return (ps2xbridge::s_active && ps2xbridge::s_textGet) ? ps2xbridge::s_textGet(id, buf, len) : 0;
}
void ps2xGsBridgeText(int id, const char *text)
{
    if (ps2xbridge::s_active && ps2xbridge::s_text) ps2xbridge::s_text(id, text);
}
int ps2xGsBridgeOption(int id, int fallback)
{
    return (ps2xbridge::s_active && ps2xbridge::s_option) ? ps2xbridge::s_option(id, fallback) : fallback;
}
// F10 in the game window: turbo (frame limiter off)
bool ps2xGsBridgeTurbo()
{
    return ps2xbridge::s_active && ps2xbridge::s_turbo && ps2xbridge::s_turbo() != 0;
}
// Spielmenue offen -> EE pausiert am VBlank
bool ps2xGsBridgeMenuOpen()
{
    return ps2xbridge::s_active && ps2xbridge::s_menu && ps2xbridge::s_menu() != 0;
}
// Screenshot through the PCSX2 GS (PNG, the way PCSX2 outputs the picture); false if the bridge is not active
bool ps2xGsBridgeScreenshot(const char *path)
{
    if (!ps2xbridge::init() || !ps2xbridge::s_shot) return false;
#ifdef _WIN32
    // PCSX2 opens absolute paths as \\\\?\\ paths: '/' is not a separator there -> normalise
    std::string p(path);
    for (char &ch : p) if (ch == '/') ch = '\\';
    ps2xbridge::s_shot(p.c_str());
#else
    ps2xbridge::s_shot(path);
#endif
    return true;
}

// Number of real frame flips (DISPFB changes per VBlank) - for the hang report in ts2_override.cpp
std::atomic<uint64_t> g_ps2xBildwechsel{0};
// From the EE scheduler at every VBlank: privileged registers in the PS2 layout (0x12000000..) + VSync
void ps2xGsBridgeVsync(GSRegisters &r)
{
    if (!ps2xbridge::init()) return;
    // MTVU pipeline: capture the registers now, but send the VSync only after this frame's GIF packets on the
    // VU1 worker. Meanwhile the EE already computes the next frame (at most one frame ahead).
    extern bool ps2xMtvuPipeVsync(std::function<void()> fn);
    auto buf = std::make_shared<std::vector<uint8_t>>(0x2000u, 0u);
    uint8_t *regs = buf->data();
    auto put = [&](uint32_t off, uint64_t v) { std::memcpy(regs + off, &v, 8); };
    put(0x0000, r.pmode); put(0x0010, r.smode1); put(0x0020, r.smode2); put(0x0030, r.srfsh);
    put(0x0040, r.synch1); put(0x0050, r.synch2); put(0x0060, r.syncv); put(0x0070, r.dispfb1);
    put(0x0080, r.display1); put(0x0090, r.dispfb2); put(0x00A0, r.display2); put(0x00B0, r.extbuf);
    put(0x00C0, r.extdata); put(0x00D0, r.extwrite); put(0x00E0, r.bgcolor);
    {
        // Measure the real game frame rate: how often the displayed frame buffer (DISPFB) changes per 250 VBlanks
        static uint64_t s_lastFb = ~0ull, s_vs = 0, s_flips = 0, s_minFb = 0;
        const uint64_t fb = r.dispfb1 ^ (r.dispfb2 << 1);
        if (fb != s_lastFb) { ++s_flips; s_lastFb = fb; g_ps2xBildwechsel.fetch_add(1, std::memory_order_relaxed); }
        if (++s_vs == 250)
        {
            static const bool log = std::getenv("PS2X_FPSLOG") != nullptr;
            static auto s_t0 = std::chrono::steady_clock::now();
            const auto t1 = std::chrono::steady_clock::now();
            const double sek = std::chrono::duration<double>(t1 - s_t0).count(); s_t0 = t1;
            // Real time: VBlanks/s (target 59.94) and from that the frames per second actually visible
            if (log) std::fprintf(stderr, "[framerate] %llu flips / 250 VBlanks | %.1f VBlanks/s | %.1f fps visible\n",
                                  (unsigned long long)s_flips, 250.0 / sek, (double)s_flips / sek);
            s_vs = 0; s_flips = 0; (void)s_minFb;
        }
    }
    {
        // PS2X_DISPLOG=1: log the display registers on every change (comparison with PCSX2)
        static const bool dlog = std::getenv("PS2X_DISPLOG") != nullptr;
        static uint64_t last[6] = {~0ull, ~0ull, ~0ull, ~0ull, ~0ull, ~0ull};
        const uint64_t cur[6] = {r.pmode, r.smode2, r.dispfb1, r.display1, r.dispfb2, r.display2};
        if (dlog && std::memcmp(cur, last, sizeof(cur)) != 0)
        {
            std::memcpy(last, cur, sizeof(cur));
            auto d = [](uint64_t v, char *o) { std::sprintf(o, "DX=%u DY=%u MAGH=%u MAGV=%u DW=%u DH=%u", (unsigned)(v & 0xFFF),
                (unsigned)((v >> 12) & 0x7FF), (unsigned)((v >> 23) & 0xF), (unsigned)((v >> 27) & 3), (unsigned)((v >> 32) & 0xFFF), (unsigned)((v >> 44) & 0x7FF)); };
            char a[96], b[96]; d(r.display1, a); d(r.display2, b);
            std::fprintf(stderr, "[DISP] PMODE=%llx SMODE2=%llx(INT=%u FFMD=%u) FB1=%llx {%s} FB2=%llx {%s}\n",
                (unsigned long long)r.pmode, (unsigned long long)r.smode2, (unsigned)(r.smode2 & 1), (unsigned)((r.smode2 >> 1) & 1),
                (unsigned long long)r.dispfb1, a, (unsigned long long)r.dispfb2, b);
        }
    }
    const uint64_t csr = r.csr.load(std::memory_order_acquire);
    put(0x1000, csr); put(0x1010, r.imr); put(0x1040, r.busdir); put(0x1080, r.siglblid);
    const int field = (csr & 0x2000ull) ? 0 : 1;
    auto push = [buf, field]()
    {
        ps2xbridge::s_vsync(buf->data(), field);
        if (ps2xbridge::s_queue)
            for (int i = 0; i < 200 && ps2xbridge::s_queue() > 200000u; ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
    };
    if (ps2xMtvuPipeVsync(push))
        return;
    ps2xbridge::s_vsync(regs, field);
    // Limit the backlog: if the GS thread falls behind, wait briefly instead of filling memory without bound
    if (ps2xbridge::s_queue)
        for (int i = 0; i < 200 && ps2xbridge::s_queue() > 200000u; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
}

GS::GS()
    : m_backend(makeGsBackend())
{
    reset();
}

void GS::init(uint8_t *vram, uint32_t vramSize, GSRegisters *privRegs)
{
    m_localMemoryStorage = vram;
    m_localMemorySize = vramSize;
    m_privRegs = privRegs;
    if (!m_backend)
        m_backend = makeGsBackend();
    m_backend->Initialize(vram, vramSize);
    reset();
}

void GS::reset()
{
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
    std::memset(m_ctx, 0, sizeof(m_ctx));
    m_prim = {};
    m_primRegister = {};
    m_prmodeRegister = {};
    m_curR = 0x80;
    m_curG = 0x80;
    m_curB = 0x80;
    m_curA = 0x80;
    m_curQ = 1.0f;
    m_curS = 0.0f;
    m_curT = 0.0f;
    m_curU = 0;
    m_curV = 0;
    m_curFog = 0;
    m_fogR = 0;
    m_fogG = 0;
    m_fogB = 0;
    m_prmodecont = true;
    m_pabe = false;
    m_scanmsk = 0u;
    m_dimx = 0u;
    m_dthe = 0u;
    m_colclamp = 0u;
    m_texa = {0u, false, 0u};
    m_texclut = {0u, 0u, 0u};
    m_bitbltbuf = {};
    m_trxpos = {};
    m_trxreg = {};
    m_trxdir = 3;
    m_vtxCount = 0;
    m_vtxIndex = 0;
    m_preferredDisplaySourceFrame = {};
    m_preferredDisplayDestFbp = 0;
    m_hasPreferredDisplaySource = false;
    if (m_backend)
    {
        m_backend->Flush();
        m_backend->Sync(GSSyncReason::Reset);
        m_backend->Reset();
    }
    {
        std::lock_guard<std::mutex> presentationLock(m_presentationMutex);
        m_hostPresentationFrame.clear();
        m_hostPresentationWidth = 0u;
        m_hostPresentationHeight = 0u;
        m_hostPresentationDisplayFbp = 0u;
        m_hostPresentationSourceFbp = 0u;
        m_hostPresentationUsedPreferred = false;
        m_hasHostPresentationFrame = false;
    }

    m_debugHistoryWrite = 0;
    m_debugHistoryCount = 0;
    m_debugNextSeq = 1;
    m_debugFrameIndex = 0;
    m_debugLastVsyncTick = UINT64_MAX;

    for (int i = 0; i < 2; ++i)
    {
        m_ctx[i].frame.fbw = 10;
        m_ctx[i].scissor = {0, 639, 0, 447};
        m_ctx[i].xyoffset = {0, 0};
    }
}

GSContext &GS::activeContext()
{
    return m_ctx[m_prim.ctxt ? 1 : 0];
}

void GS::snapshotVRAM()
{
    // Presentation/debug snapshots run outside m_stateMutex so the EE can keep
    // feeding the GS while a backend performs host-side conversion. Keep the
    // selected backend alive and unswappable for the duration of the call.
    std::lock_guard<std::mutex> backendLock(m_backendLifetimeMutex);
    if (!m_backend)
        return;
    std::vector<uint8_t> snapshot;
    m_backend->Sync(GSSyncReason::DebugReadback);
    m_backend->SnapshotVram(snapshot);
    std::lock_guard<std::mutex> lock(m_snapshotMutex);
    m_displaySnapshot.swap(snapshot);
}

const uint8_t *GS::lockDisplaySnapshot(uint32_t &outSize)
{
    m_snapshotMutex.lock();
    if (m_displaySnapshot.empty())
    {
        outSize = 0;
        return nullptr;
    }

    outSize = static_cast<uint32_t>(m_displaySnapshot.size());
    return m_displaySnapshot.data();
}

GSDebugSnapshot GS::getDebugSnapshot() const
{
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);

    GSDebugSnapshot snapshot{};
    snapshot.ctx[0] = m_ctx[0];
    snapshot.ctx[1] = m_ctx[1];
    snapshot.prim = m_prim;
    snapshot.texa = m_texa;
    snapshot.texclut = m_texclut;
    snapshot.scanmsk = m_scanmsk;
    snapshot.dimx = m_dimx;
    snapshot.dthe = m_dthe;
    snapshot.colclamp = m_colclamp;
    snapshot.bitbltbuf = m_bitbltbuf;
    snapshot.trxpos = m_trxpos;
    snapshot.trxreg = m_trxreg;
    const GSTransferSnapshot transfer = m_backend ? m_backend->GetTransferSnapshot() : GSTransferSnapshot{};
    snapshot.trxdir = transfer.direction;
    snapshot.transferX = transfer.x;
    snapshot.transferY = transfer.y;
    snapshot.transferTotalPixels = transfer.totalPixels;
    snapshot.transferCopiedPixels = transfer.copiedPixels;
    snapshot.lastDisplayBaseBytes = m_lastDisplayBaseBytes;
    snapshot.preferredDisplaySourceFrame = m_preferredDisplaySourceFrame;
    snapshot.preferredDisplayDestFbp = m_preferredDisplayDestFbp;
    snapshot.hasPreferredDisplaySource = m_hasPreferredDisplaySource;
    {
        std::lock_guard<std::mutex> presentationLock(m_presentationMutex);
        snapshot.hostPresentationWidth = m_hostPresentationWidth;
        snapshot.hostPresentationHeight = m_hostPresentationHeight;
        snapshot.hostPresentationDisplayFbp = m_hostPresentationDisplayFbp;
        snapshot.hostPresentationSourceFbp = m_hostPresentationSourceFbp;
        snapshot.hostPresentationUsedPreferred = m_hostPresentationUsedPreferred;
        snapshot.hasHostPresentationFrame = m_hasHostPresentationFrame;
    }
    snapshot.localToHostPendingBytes = transfer.localToHostPendingBytes;
    return snapshot;
}

std::vector<GSDebugHistoryEntry> GS::getDebugHistory() const
{
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);

    std::vector<GSDebugHistoryEntry> out;
    out.reserve(m_debugHistoryCount);
    const size_t first = (m_debugHistoryWrite + kDebugHistoryCapacity - m_debugHistoryCount) % kDebugHistoryCapacity;
    for (size_t i = 0; i < m_debugHistoryCount; ++i)
    {
        out.push_back(m_debugHistory[(first + i) % kDebugHistoryCapacity]);
    }
    return out;
}

void GS::clearDebugHistory()
{
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
    m_debugHistoryWrite = 0;
    m_debugHistoryCount = 0;
    m_debugNextSeq = 1;
    m_debugFrameIndex = 0;
    m_debugLastVsyncTick = UINT64_MAX;
}

bool GS::isDebugHistoryPaused() const
{
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
    return m_debugHistoryPaused;
}

void GS::setDebugHistoryPaused(bool paused)
{
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
    m_debugHistoryPaused = paused;
}

GSDebugHistoryEntry GS::makeDebugEventUnlocked(GSDebugEventKind kind) const
{
    GSDebugHistoryEntry entry{};
    entry.kind = kind;
    entry.prim = m_prim;
    const uint32_t ci = m_prim.ctxt ? 1u : 0u;
    entry.frame = m_ctx[ci].frame;
    entry.zbuf = m_ctx[ci].zbuf;
    entry.tex0 = m_ctx[ci].tex0;
    entry.scissor = m_ctx[ci].scissor;
    entry.test = m_ctx[ci].test;
    entry.alpha = m_ctx[ci].alpha;
    entry.bitbltbuf = m_bitbltbuf;
    entry.trxpos = m_trxpos;
    entry.trxreg = m_trxreg;
    const GSTransferSnapshot transfer = m_backend ? m_backend->GetTransferSnapshot() : GSTransferSnapshot{};
    entry.trxdir = transfer.direction;
    entry.transferPixels = transfer.totalPixels;
    return entry;
}

void GS::recordDebugEventUnlocked(GSDebugHistoryEntry entry)
{
    if (m_debugHistoryPaused)
    {
        return;
    }

    const uint64_t tick = m_privRegs ? m_privRegs->vsyncTick.load(std::memory_order_acquire) : 0u;
    if (m_debugLastVsyncTick == UINT64_MAX)
    {
        m_debugLastVsyncTick = tick;
    }
    else if (tick != m_debugLastVsyncTick)
    {
        ++m_debugFrameIndex;
        m_debugLastVsyncTick = tick;
    }

    entry.seq = m_debugNextSeq++;
    entry.vsyncTick = tick;
    entry.frameIndex = m_debugFrameIndex;

    m_debugHistory[m_debugHistoryWrite] = entry;
    m_debugHistoryWrite = (m_debugHistoryWrite + 1u) % kDebugHistoryCapacity;
    if (m_debugHistoryCount < kDebugHistoryCapacity)
    {
        ++m_debugHistoryCount;
    }
}

void GS::recordGifTagDebugEventUnlocked(uint32_t sizeBytes, uint32_t nloop, uint8_t flg, uint32_t nreg)
{
    if (m_debugHistoryPaused)
    {
        return;
    }

    GSDebugHistoryEntry entry = makeDebugEventUnlocked(GSDebugEventKind::GifTag);
    entry.gifSizeBytes = sizeBytes;
    entry.gifNloop = nloop;
    entry.gifFlg = flg;
    entry.gifNreg = static_cast<uint8_t>(std::min<uint32_t>(nreg, 16u));
    recordDebugEventUnlocked(entry);
}

void GS::recordRegisterDebugEventUnlocked(uint8_t regAddr, uint64_t value)
{
    if (m_debugHistoryPaused)
    {
        return;
    }

    switch (regAddr)
    {
    case GS_REG_PRIM:
    case GS_REG_TEX0_1:
    case GS_REG_TEX0_2:
    case GS_REG_TEX2_1:
    case GS_REG_TEX2_2:
    case GS_REG_TEXA:
    case GS_REG_TEXCLUT:
    case GS_REG_FRAME_1:
    case GS_REG_FRAME_2:
    case GS_REG_ZBUF_1:
    case GS_REG_ZBUF_2:
    case GS_REG_ALPHA_1:
    case GS_REG_ALPHA_2:
    case GS_REG_TEST_1:
    case GS_REG_TEST_2:
    case GS_REG_SCISSOR_1:
    case GS_REG_SCISSOR_2:
    case GS_REG_XYOFFSET_1:
    case GS_REG_XYOFFSET_2:
    case GS_REG_BITBLTBUF:
    case GS_REG_TRXPOS:
    case GS_REG_TRXREG:
    case GS_REG_TRXDIR:
        break;
    default:
        return;
    }

    GSDebugHistoryEntry entry = makeDebugEventUnlocked(GSDebugEventKind::Register);
    entry.reg = regAddr;
    entry.regValue = value;
    recordDebugEventUnlocked(entry);
}

void GS::recordDrawDebugEventUnlocked(int vertexCount)
{
    {
        static const bool traceEnv = std::getenv("SOTC_TRACE_GS") != nullptr;
        static const double traceDelay = std::getenv("SOTC_TRACE_GS_DELAY") ? std::atof(std::getenv("SOTC_TRACE_GS_DELAY")) : 0.0;
        static const auto traceStart = std::chrono::steady_clock::now();
        const bool trace = traceEnv && std::chrono::duration<double>(std::chrono::steady_clock::now() - traceStart).count() >= traceDelay;
        static std::atomic<int> printed{0};
        static uint64_t seen[512];
        static int seenN = 0;
        if (trace && vertexCount > 0)
        {
            const uint32_t ci = m_prim.ctxt ? 1u : 0u;
            const auto &c = m_ctx[ci];
            const GSVertex &v0 = m_vtxQueue[0];
            const GSVertex &v1 = m_vtxQueue[std::min(vertexCount, kMaxVerts) - 1];
            uint64_t sig = (uint64_t)m_prim.type | ((uint64_t)m_prim.tme << 3) | ((uint64_t)m_prim.abe << 4) |
                           ((uint64_t)c.tex0.psm << 5) | ((uint64_t)c.tex0.tbp0 << 11) | ((uint64_t)c.frame.fbp << 25) |
                           ((uint64_t)c.tex0.cpsm << 34) | ((uint64_t)c.tex0.cbp << 40) | ((uint64_t)v0.r << 54) ^ ((uint64_t)(int)v0.x << 20) ^ ((uint64_t)v0.a << 46);
            {
                static int triN = 0;
                if (vertexCount >= 3 && c.tex0.psm == 20 && m_prim.tme && (v0.x < 1780.0f || v1.x < 1780.0f) && triN < 60)
                {
                    ++triN;
                    std::fprintf(stderr, "    tri rgba: %u,%u,%u,%u / %u,%u,%u,%u / %u,%u,%u,%u iip=%d xy2=%.1f,%.1f uv2=%u,%u\n",
                                 m_vtxQueue[0].r, m_vtxQueue[0].g, m_vtxQueue[0].b, m_vtxQueue[0].a,
                                 m_vtxQueue[1].r, m_vtxQueue[1].g, m_vtxQueue[1].b, m_vtxQueue[1].a,
                                 m_vtxQueue[2].r, m_vtxQueue[2].g, m_vtxQueue[2].b, m_vtxQueue[2].a, (int)m_prim.iip,
                                 m_vtxQueue[2].x, m_vtxQueue[2].y, m_vtxQueue[2].u, m_vtxQueue[2].v);
                }
            }
            bool dup = false;
            for (int i = 0; i < seenN; ++i)
                if (seen[i] == sig) { dup = true; break; }
            if (!dup && seenN < 512 && printed.load() < 400)
            {
                seen[seenN++] = sig;
                printed.fetch_add(1);
                std::fprintf(stderr, "[GS draw] prim=%d tme=%d abe=%d fst=%d ctx=%u n=%d frame fbp=%u fbw=%u psm=%u msk=%08x | tex tbp=%u tbw=%u psm=%u tw=%u th=%u tcc=%u tfx=%u cbp=%u cpsm=%u csm=%u csa=%u | v0 xy=%.1f,%.1f rgba=%u,%u,%u,%u uv=%u,%u st=%.3f,%.3f | v1 xy=%.1f,%.1f | z=%.0f,%.0f fge=%d\n",
                             (int)m_prim.type, m_prim.tme, m_prim.abe, m_prim.fst, ci, vertexCount,
                             c.frame.fbp, c.frame.fbw, c.frame.psm, c.frame.fbmsk,
                             c.tex0.tbp0, c.tex0.tbw, c.tex0.psm, c.tex0.tw, c.tex0.th, c.tex0.tcc, c.tex0.tfx,
                             c.tex0.cbp, c.tex0.cpsm, c.tex0.csm, c.tex0.csa,
                             v0.x, v0.y, v0.r, v0.g, v0.b, v0.a, v0.u, v0.v, v0.s, v0.t, v1.x, v1.y, v0.z, v1.z, (int)m_prim.fge);
                if (m_prim.type == GS_PRIM_SPRITE)
                    std::fprintf(stderr, "    ctx: test=%llx alpha=%llx fba=%llx scissor=%u-%u,%u-%u ofs=%u,%u zbuf zbp=%u psm=%u msk=%d\n",
                                 (unsigned long long)c.test, (unsigned long long)c.alpha, (unsigned long long)c.fba,
                                 c.scissor.x0, c.scissor.x1, c.scissor.y0, c.scissor.y1, c.xyoffset.ofx >> 4, c.xyoffset.ofy >> 4,
                                 c.zbuf.zbp, c.zbuf.psm, (int)c.zbuf.zmask);
            }
        }
    }
    if (m_debugHistoryPaused)
    {
        return;
    }

    if (vertexCount <= 0)
    {
        return;
    }

    GSDebugHistoryEntry entry = makeDebugEventUnlocked(GSDebugEventKind::Draw);
    entry.vertexCount = static_cast<uint32_t>(vertexCount);

    const int count = std::min(vertexCount, kMaxVerts);
    entry.xMin = entry.xMax = m_vtxQueue[0].x;
    entry.yMin = entry.yMax = m_vtxQueue[0].y;
    entry.zMin = entry.zMax = m_vtxQueue[0].z;
    entry.aMin = entry.aMax = m_vtxQueue[0].a;

    for (int i = 1; i < count; ++i)
    {
        const GSVertex &v = m_vtxQueue[i];
        entry.xMin = std::min(entry.xMin, v.x);
        entry.xMax = std::max(entry.xMax, v.x);
        entry.yMin = std::min(entry.yMin, v.y);
        entry.yMax = std::max(entry.yMax, v.y);
        entry.zMin = std::min(entry.zMin, v.z);
        entry.zMax = std::max(entry.zMax, v.z);
        entry.aMin = std::min(entry.aMin, v.a);
        entry.aMax = std::max(entry.aMax, v.a);
    }

    recordDebugEventUnlocked(entry);
}

void GS::recordTransferDebugEventUnlocked()
{
    {
        static const bool trace = std::getenv("SOTC_TRACE_GS") != nullptr;
        static int printed = 0;
        static const bool sotcTraceClutEnv = std::getenv("SOTC_TRACE_CLUT") != nullptr;
        if ((trace || sotcTraceClutEnv) && printed < 300)
        {
            ++printed;
            std::fprintf(stderr, "[GS xfer] sbp=%u sbw=%u spsm=%u dbp=%u dbw=%u dpsm=%u spos=%u,%u dpos=%u,%u dir=%u size=%ux%u\n",
                         m_bitbltbuf.sbp, m_bitbltbuf.sbw, m_bitbltbuf.spsm, m_bitbltbuf.dbp, m_bitbltbuf.dbw, m_bitbltbuf.dpsm,
                         m_trxpos.ssax, m_trxpos.ssay, m_trxpos.dsax, m_trxpos.dsay, m_trxpos.dir, m_trxreg.rrw, m_trxreg.rrh);
        }
    }
    if (m_debugHistoryPaused)
    {
        return;
    }

    GSDebugHistoryEntry entry = makeDebugEventUnlocked(GSDebugEventKind::Transfer);
    entry.transferPixels = m_backend ? m_backend->GetTransferSnapshot().totalPixels : 0u;
    recordDebugEventUnlocked(entry);
}

void GS::recordPresentDebugEventUnlocked(uint32_t displayFbp, uint32_t sourceFbp, uint32_t width, uint32_t height, bool usedPreferred)
{
    if (m_debugHistoryPaused)
    {
        return;
    }

    GSDebugHistoryEntry entry = makeDebugEventUnlocked(GSDebugEventKind::Present);
    entry.displayFbp = displayFbp;
    entry.sourceFbp = sourceFbp;
    entry.width = width;
    entry.height = height;
    entry.usedPreferred = usedPreferred;
    recordDebugEventUnlocked(entry);
}

bool GS::getPreferredDisplaySource(GSFrameReg &outSource, uint32_t &outDestFbp) const
{
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
    if (!m_hasPreferredDisplaySource)
    {
        outSource = {};
        outDestFbp = 0u;
        return false;
    }

    outSource = m_preferredDisplaySourceFrame;
    outDestFbp = m_preferredDisplayDestFbp;
    return true;
}

void GS::unlockDisplaySnapshot()
{
    m_snapshotMutex.unlock();
}

uint32_t GS::getLastDisplayBaseBytes() const
{
    return m_lastDisplayBaseBytes;
}

void GS::refreshDisplaySnapshot()
{
    snapshotVRAM();
}

GSPresentationRequest GS::buildPresentationRequestUnlocked() const
{
    GSPresentationRequest request{};
    if (!m_privRegs)
        return request;
    request.pmode = m_privRegs->pmode;
    request.smode2 = m_privRegs->smode2;
    request.dispfb1 = m_privRegs->dispfb1;
    request.display1 = m_privRegs->display1;
    request.dispfb2 = m_privRegs->dispfb2;
    request.display2 = m_privRegs->display2;
    request.bgcolor = m_privRegs->bgcolor;
    request.vsyncTick = m_privRegs->vsyncTick.load(std::memory_order_acquire);
    request.contextFrames[0] = m_ctx[0].frame;
    request.contextFrames[1] = m_ctx[1].frame;
    request.preferredSource = m_preferredDisplaySourceFrame;
    request.preferredDestFbp = m_preferredDisplayDestFbp;
    request.hasPreferredSource = m_hasPreferredDisplaySource;
    return request;
}

void GS::latchHostPresentationFrame()
{
    GSPresentationRequest request{};
    {
        std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
        if (!m_backend || !m_privRegs)
        {
            std::lock_guard<std::mutex> presentationLock(m_presentationMutex);
            m_hostPresentationFrame.clear();
            m_hasHostPresentationFrame = false;
            m_hostPresentationWidth = m_hostPresentationHeight = 0u;
            return;
        }
        request = buildPresentationRequestUnlocked();
    }

    PresentationFrame frame{};
    {
        std::lock_guard<std::mutex> backendLock(m_backendLifetimeMutex);
        if (m_backend)
        {
            m_backend->Flush();
            m_backend->Sync(GSSyncReason::Presentation);
            frame = m_backend->Present(request);
        }
    }

    {
        static const bool trace = std::getenv("SOTC_TRACE_PRESENT") != nullptr;
        static uint32_t n = 0;
        if (trace && (n++ % 60u) == 0u)
        {
            uint32_t nonBlack = 0u;
            for (size_t i = 0; i + 3 < frame.pixels.size(); i += 4)
                if (frame.pixels[i] | frame.pixels[i + 1] | frame.pixels[i + 2])
                    ++nonBlack;
            std::fprintf(stderr, "[present] pmode=%llx smode2=%llx dispfb1=%llx display1=%llx dispfb2=%llx display2=%llx -> ok=%d %ux%u disp=%u src=%u pref=%d nonblack=%u\n",
                         (unsigned long long)request.pmode, (unsigned long long)request.smode2, (unsigned long long)request.dispfb1,
                         (unsigned long long)request.display1, (unsigned long long)request.dispfb2, (unsigned long long)request.display2,
                         (int)static_cast<bool>(frame), frame.width, frame.height, frame.displayFbp, frame.sourceFbp, (int)frame.usedPreferred, nonBlack);
        }
    }
    const bool hasFrame = static_cast<bool>(frame);
    const uint32_t displayFbp = frame.displayFbp;
    const uint32_t sourceFbp = frame.sourceFbp;
    const uint32_t width = frame.width;
    const uint32_t height = frame.height;
    const bool usedPreferred = frame.usedPreferred;
    {
        std::lock_guard<std::mutex> presentationLock(m_presentationMutex);
        m_hostPresentationFrame = std::move(frame.pixels);
        m_hostPresentationWidth = width;
        m_hostPresentationHeight = height;
        m_hostPresentationDisplayFbp = displayFbp;
        m_hostPresentationSourceFbp = sourceFbp;
        m_hostPresentationUsedPreferred = usedPreferred;
        m_hasHostPresentationFrame = hasFrame;
    }

    if (hasFrame)
    {
        std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
        recordPresentDebugEventUnlocked(displayFbp, sourceFbp, width, height, usedPreferred);
    }
}

bool GS::copyLatchedHostPresentationFrame(std::vector<uint8_t> &outPixels,
                                          uint32_t &outWidth,
                                          uint32_t &outHeight,
                                          uint32_t *outDisplayFbp,
                                          uint32_t *outSourceFbp,
                                          bool *outUsedPreferred) const
{
    std::lock_guard<std::mutex> lock(m_presentationMutex);
    if (!m_hasHostPresentationFrame || m_hostPresentationFrame.empty())
    {
        outPixels.clear();
        outWidth = 0u;
        outHeight = 0u;
        if (outDisplayFbp)
            *outDisplayFbp = 0u;
        if (outSourceFbp)
            *outSourceFbp = 0u;
        if (outUsedPreferred)
            *outUsedPreferred = false;
        return false;
    }

    outWidth = m_hostPresentationWidth;
    outHeight = m_hostPresentationHeight;
    if (outDisplayFbp)
        *outDisplayFbp = m_hostPresentationDisplayFbp;
    if (outSourceFbp)
        *outSourceFbp = m_hostPresentationSourceFbp;
    if (outUsedPreferred)
        *outUsedPreferred = m_hostPresentationUsedPreferred;

    const size_t packedRowBytes = static_cast<size_t>(outWidth) * 4u;
    outPixels.resize(packedRowBytes * static_cast<size_t>(outHeight));
    if (outWidth != 0u && outHeight != 0u)
    {
        const size_t sourceRowBytes = static_cast<size_t>(kHostFrameWidth) * 4u;
        for (uint32_t y = 0; y < outHeight; ++y)
        {
            const size_t srcOffset = static_cast<size_t>(y) * sourceRowBytes;
            const size_t dstOffset = static_cast<size_t>(y) * packedRowBytes;
            if (srcOffset + packedRowBytes > m_hostPresentationFrame.size() ||
                dstOffset + packedRowBytes > outPixels.size())
            {
                outPixels.clear();
                outWidth = 0u;
                outHeight = 0u;
                if (outDisplayFbp)
                    *outDisplayFbp = 0u;
                if (outSourceFbp)
                    *outSourceFbp = 0u;
                if (outUsedPreferred)
                    *outUsedPreferred = false;
                return false;
            }

            std::memcpy(outPixels.data() + dstOffset,
                        m_hostPresentationFrame.data() + srcOffset,
                        packedRowBytes);
        }
    }
    return true;
}

void GS::processGIFPacket(const uint8_t *data, uint32_t sizeBytes)
{
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
    if (!data || sizeBytes < 16 || !m_backend)
        return;
    if (ps2xbridge::init())
    {
        if (g_ps2xBridgeStripTag)
        {
            if (sizeBytes > 16u)
                ps2xbridge::s_transfer(g_ps2xCurrentGifPath, data + 16, (sizeBytes - 16u) & ~15u);
        }
        else
            ps2xbridge::s_transfer(g_ps2xCurrentGifPath, data, sizeBytes & ~15u);
        // With the PCSX2 GS, PCSX2 draws and stores everything itself; our own GS only needs from the packet
        // the event registers the EE sees through CSR/SIGLBLID (SIGNAL/FINISH/LABEL by A+D).
        // Saves re-parsing every packet completely (vertices, textures into our own VRAM).
        static const bool s_fullParse = [] { const char *e = std::getenv("PS2X_GS_FULLPARSE"); return e && *e == '1'; }();
        if (!s_fullParse)
        {
            if (g_ps2xBridgeStripTag)
                return; // pure image data (VIF1 DIRECT-IMAGE), no registers
            uint32_t off = 0u;
            while (off + 16u <= sizeBytes)
            {
                const uint64_t tagLo = loadLE64(data + off);
                const uint64_t tagHi = loadLE64(data + off + 8);
                off += 16u;
                const uint32_t nloop = static_cast<uint32_t>(tagLo & 0x7FFFu);
                const uint32_t flg = static_cast<uint32_t>((tagLo >> 58) & 0x3u);
                uint32_t nreg = static_cast<uint32_t>((tagLo >> 60) & 0xFu);
                if (nreg == 0u)
                    nreg = 16u;
                if (flg == GIF_FMT_PACKED)
                {
                    bool hasAd = false;
                    for (uint32_t r = 0; r < nreg; ++r)
                        if (((tagHi >> (r * 4u)) & 0xFu) == 0xEu)
                            hasAd = true;
                    if (hasAd)
                    {
                        uint32_t o = off;
                        for (uint32_t loop = 0; loop < nloop; ++loop)
                            for (uint32_t r = 0; r < nreg; ++r, o += 16u)
                            {
                                if (o + 16u > sizeBytes)
                                    return;
                                if (((tagHi >> (r * 4u)) & 0xFu) != 0xEu)
                                    continue;
                                const uint8_t addr = static_cast<uint8_t>(loadLE64(data + o + 8) & 0xFFu);
                                if (addr == GS_REG_SIGNAL || addr == GS_REG_FINISH || addr == GS_REG_LABEL)
                                    writeRegisterUnlocked(addr, loadLE64(data + o));
                            }
                    }
                    off += nloop * nreg * 16u;
                }
                else if (flg == GIF_FMT_REGLIST)
                    off += ((nloop * nreg + 1u) & ~1u) * 8u;
                else
                    off += nloop * 16u;
            }
            return;
        }
    }

    if (tryProcessNativeImageUploadPacket(data, sizeBytes))
        return;

    PS2_IF_AGRESSIVE_LOGS({
        const uint32_t packetIndex = s_debugGifPacketCount.fetch_add(1, std::memory_order_relaxed);
        if (packetIndex < 48u)
        {
            const uint64_t tagLo = loadLE64(data);
            const uint32_t nloop = static_cast<uint32_t>(tagLo & 0x7FFFu);
            const uint8_t flg = static_cast<uint8_t>((tagLo >> 58) & 0x3u);
            uint32_t nreg = static_cast<uint32_t>((tagLo >> 60) & 0xFu);
            if (nreg == 0u)
                nreg = 16u;
            RUNTIME_LOG("[gs:gif] idx=" << packetIndex
                                        << " size=" << sizeBytes
                                        << " nloop=" << nloop
                                        << " flg=" << static_cast<uint32_t>(flg)
                                        << " nreg=" << nreg
                                        << " ctx0fbp=" << m_ctx[0].frame.fbp
                                        << " ctx1fbp=" << m_ctx[1].frame.fbp
                                        << std::endl);
        }
    });

    uint32_t offset = 0;
    while (offset + 16 <= sizeBytes)
    {
        uint64_t tagLo = loadLE64(data + offset);
        uint64_t tagHi = loadLE64(data + offset + 8);
        offset += 16;

        m_curQ = 1.0f;

        uint32_t nloop = static_cast<uint32_t>(tagLo & 0x7FFF);
        uint8_t flg = static_cast<uint8_t>((tagLo >> 58) & 0x3);
        uint32_t nreg = static_cast<uint32_t>((tagLo >> 60) & 0xF);
        if (nreg == 0)
            nreg = 16;

        recordGifTagDebugEventUnlocked(sizeBytes, nloop, flg, nreg);

        bool pre = ((tagLo >> 46) & 1) != 0;
        if (pre)
        {
            writeRegisterUnlocked(GS_REG_PRIM, (tagLo >> 47) & 0x7FF);
        }

        uint8_t regs[16];
        for (uint32_t i = 0; i < nreg; ++i)
            regs[i] = static_cast<uint8_t>((tagHi >> (i * 4)) & 0xF);

        if (flg == GIF_FMT_PACKED)
        {
            for (uint32_t loop = 0; loop < nloop; ++loop)
            {
                for (uint32_t r = 0; r < nreg; ++r)
                {
                    if (offset + 16 > sizeBytes)
                        return;
                    uint64_t lo = loadLE64(data + offset);
                    uint64_t hi = loadLE64(data + offset + 8);
                    offset += 16;
                    writeRegisterPacked(regs[r], lo, hi);
                }
            }
        }
        else if (flg == GIF_FMT_REGLIST)
        {
            for (uint32_t loop = 0; loop < nloop; ++loop)
            {
                for (uint32_t r = 0; r < nreg; ++r)
                {
                    if (offset + 8 > sizeBytes)
                        return;
                    writeRegisterUnlocked(regs[r], loadLE64(data + offset));
                    offset += 8;
                }
            }
            if ((nloop * nreg) & 1)
                offset += 8;
        }
        else if (flg == GIF_FMT_IMAGE || flg == GIF_FMT_DISABLED)
        {
            // FLG=3 ("disabled" in the GS manual) behaves exactly like IMAGE: NLOOP qwords of image data.
            uint32_t imageBytes = nloop * 16;
            if (offset + imageBytes > sizeBytes)
                imageBytes = sizeBytes - offset;
            processImageData(data + offset, imageBytes);
            offset += imageBytes;
        }
    }
}

bool GS::processNativePackedGIFPacket(const uint8_t *data, uint32_t sizeBytes)
{
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
    if (!data || sizeBytes < 16u || !m_backend)
        return false;

    if (!validatePackedGifPacket(data, sizeBytes))
        return false;

    const bool processed = visitPackedGifPacket(data, sizeBytes, [&](const PackedGifPacketTag &tag)
                                                {
        m_curQ = 1.0f;

        recordGifTagDebugEventUnlocked(sizeBytes, tag.nloop, GIF_FMT_PACKED, tag.nreg);

        const bool pre = ((tag.lo >> 46u) & 1u) != 0u;
        if (pre)
            writeRegisterUnlocked(GS_REG_PRIM, (tag.lo >> 47u) & 0x7FFu);

        uint32_t offset = tag.payloadOffset;
        for (uint32_t loop = 0u; loop < tag.nloop; ++loop)
        {
            for (uint32_t r = 0u; r < tag.nreg; ++r)
            {
                const uint64_t lo = loadLE64(data + offset);
                const uint64_t hi = loadLE64(data + offset + 8u);
                offset += 16u;
                writeRegisterPacked(tag.regs[r], lo, hi);
            }
        }

        return true; });

    if (!processed)
        return false;

    ++m_nativePackedGIFPacketCount;
    return true;
}

void GS::uploadImageNative(uint64_t bitbltbuf,
                           uint64_t trxpos,
                           uint64_t trxreg,
                           uint64_t trxdir,
                           const uint8_t *data,
                           uint32_t sizeBytes)
{
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
    uploadImageNativeUnlocked(bitbltbuf, trxpos, trxreg, trxdir, data, sizeBytes);
}

void GS::uploadImageNativeUnlocked(uint64_t bitbltbuf,
                                   uint64_t trxpos,
                                   uint64_t trxreg,
                                   uint64_t trxdir,
                                   const uint8_t *data,
                                   uint32_t sizeBytes)
{
    if (!data || sizeBytes == 0 || !m_backend)
        return;

    writeRegisterUnlocked(GS_REG_BITBLTBUF, bitbltbuf);
    writeRegisterUnlocked(GS_REG_TRXPOS, trxpos);
    writeRegisterUnlocked(GS_REG_TRXREG, trxreg);
    writeRegisterUnlocked(GS_REG_TRXDIR, trxdir);
    processImageData(data, sizeBytes);
    ++m_nativeImageUploadCount;
}

bool GS::tryProcessNativeImageUploadPacket(const uint8_t *data, uint32_t sizeBytes)
{
    constexpr uint32_t kSetupRegisters = 4u;
    constexpr uint32_t kPackedAdPayloadBytes = kSetupRegisters * 16u;
    constexpr uint64_t kPackedAdDescriptor = 0x0Eull;

    if (!data || sizeBytes < 16u + kPackedAdPayloadBytes + 16u)
        return false;

    const uint64_t setupTagLo = loadLE64(data);
    const uint64_t setupTagHi = loadLE64(data + 8u);
    const uint32_t setupNloop = static_cast<uint32_t>(setupTagLo & 0x7FFFu);
    const uint8_t setupFlg = static_cast<uint8_t>((setupTagLo >> 58u) & 0x3u);
    uint32_t setupNreg = static_cast<uint32_t>((setupTagLo >> 60u) & 0xFu);
    if (setupNreg == 0u)
        setupNreg = 16u;

    if (setupNloop != kSetupRegisters ||
        setupFlg != GIF_FMT_PACKED ||
        setupNreg != 1u ||
        (setupTagHi & 0xFull) != kPackedAdDescriptor)
    {
        return false;
    }

    uint64_t regs[kSetupRegisters] = {};
    uint32_t offset = 16u;
    constexpr uint8_t expectedRegs[kSetupRegisters] = {
        GS_REG_BITBLTBUF,
        GS_REG_TRXPOS,
        GS_REG_TRXREG,
        GS_REG_TRXDIR,
    };

    for (uint32_t i = 0; i < kSetupRegisters; ++i)
    {
        regs[i] = loadLE64(data + offset);
        const uint64_t reg = loadLE64(data + offset + 8u);
        if ((reg & 0xFFu) != expectedRegs[i])
            return false;
        offset += 16u;
    }

    const uint32_t trxdirMode = static_cast<uint32_t>(regs[3] & 0x3ull);
    const uint32_t rrw = static_cast<uint32_t>(regs[2] & 0xFFFull);
    const uint32_t rrh = static_cast<uint32_t>((regs[2] >> 32u) & 0xFFFull);
    if (trxdirMode != 0u || rrw == 0u || rrh == 0u)
        return false;

    if (offset + 16u > sizeBytes)
        return false;

    const uint64_t imageTagLo = loadLE64(data + offset);
    const uint8_t imageFlg = static_cast<uint8_t>((imageTagLo >> 58u) & 0x3u);
    const uint32_t imageNloop = static_cast<uint32_t>(imageTagLo & 0x7FFFu);
    if (imageFlg != GIF_FMT_IMAGE || imageNloop == 0u)
        return false;

    offset += 16u;
    const uint64_t imageBytes64 = static_cast<uint64_t>(imageNloop) * 16ull;
    if (imageBytes64 > 0xFFFFFFFFull)
        return false;
    const uint32_t imageBytes = static_cast<uint32_t>(imageBytes64);
    if (offset + imageBytes != sizeBytes)
        return false;

    uploadImageNativeUnlocked(regs[0], regs[1], regs[2], regs[3], data + offset, imageBytes);
    return true;
}

void GS::writeRegisterPacked(uint8_t regDesc, uint64_t lo, uint64_t hi)
{
    switch (regDesc)
    {
    case 0x00:
        writeRegisterUnlocked(GS_REG_PRIM, lo & 0x7FF);
        break;
    case 0x01:
        m_curR = static_cast<uint8_t>(lo & 0xFF);
        m_curG = static_cast<uint8_t>((lo >> 32) & 0xFF);
        m_curB = static_cast<uint8_t>(hi & 0xFF);
        m_curA = static_cast<uint8_t>((hi >> 32) & 0xFF);
        break;
    case 0x02:
    {
        uint32_t sBits = static_cast<uint32_t>(lo & 0xFFFFFFFF);
        uint32_t tBits = static_cast<uint32_t>((lo >> 32) & 0xFFFFFFFF);
        uint32_t qBits = static_cast<uint32_t>(hi & 0xFFFFFFFF);
        std::memcpy(&m_curS, &sBits, 4);
        std::memcpy(&m_curT, &tBits, 4);
        std::memcpy(&m_curQ, &qBits, 4);
        if (m_curQ == 0.0f)
            m_curQ = 1.0f;
        break;
    }
    case 0x03:
        m_curU = static_cast<uint16_t>(lo & 0x3FFFu);
        m_curV = static_cast<uint16_t>((lo >> 32) & 0x3FFFu);
        break;
    case 0x04:
    {
        uint16_t x = static_cast<uint16_t>(lo & 0xFFFF);
        uint16_t y = static_cast<uint16_t>((lo >> 32) & 0xFFFF);
        uint32_t z = static_cast<uint32_t>((hi >> 4) & 0xFFFFFF);
        uint8_t f = static_cast<uint8_t>((hi >> 36) & 0xFF);
        bool adk = ((hi >> 47) & 1) != 0;
        PS2_IF_AGRESSIVE_LOGS({
            const uint32_t debugIndex = s_debugGsPackedVertexCount.fetch_add(1, std::memory_order_relaxed);
            if (debugIndex < 64u)
            {
                RUNTIME_LOG("[gs:packed-xyzf] idx=" << debugIndex
                                                    << " x=" << x
                                                    << " y=" << y
                                                    << " z=0x" << std::hex << z
                                                    << std::dec
                                                    << " fog=" << static_cast<uint32_t>(f)
                                                    << " kick=" << static_cast<uint32_t>(!adk ? 1u : 0u)
                                                    << " prim=" << static_cast<uint32_t>(m_prim.type)
                                                    << std::endl);
            }
        });
        GSVertex &vtx = m_vtxQueue[m_vtxCount % kMaxVerts];
        vtx.x = static_cast<float>(x) / 16.0f;
        vtx.y = static_cast<float>(y) / 16.0f;
        vtx.z = static_cast<float>(z);
        vtx.r = m_curR;
        vtx.g = m_curG;
        vtx.b = m_curB;
        vtx.a = m_curA;
        vtx.q = m_curQ;
        vtx.s = m_curS;
        vtx.t = m_curT;
        vtx.u = m_curU;
        vtx.v = m_curV;
        vtx.fog = f;
        vertexKick(!adk);
        break;
    }
    case 0x05:
    {
        uint16_t x = static_cast<uint16_t>(lo & 0xFFFF);
        uint16_t y = static_cast<uint16_t>((lo >> 32) & 0xFFFF);
        uint32_t z = static_cast<uint32_t>(hi & 0xFFFFFFFF);
        bool adk = ((hi >> 47) & 1) != 0;
        PS2_IF_AGRESSIVE_LOGS({
            const uint32_t debugIndex = s_debugGsPackedVertexCount.fetch_add(1, std::memory_order_relaxed);
            if (debugIndex < 64u)
            {
                RUNTIME_LOG("[gs:packed-xyz] idx=" << debugIndex
                                                   << " x=" << x
                                                   << " y=" << y
                                                   << " z=0x" << std::hex << z
                                                   << std::dec
                                                   << " kick=" << static_cast<uint32_t>(!adk ? 1u : 0u)
                                                   << " prim=" << static_cast<uint32_t>(m_prim.type)
                                                   << std::endl);
            }
        });
        GSVertex &vtx = m_vtxQueue[m_vtxCount % kMaxVerts];
        vtx.x = static_cast<float>(x) / 16.0f;
        vtx.y = static_cast<float>(y) / 16.0f;
        vtx.z = static_cast<float>(z);
        vtx.r = m_curR;
        vtx.g = m_curG;
        vtx.b = m_curB;
        vtx.a = m_curA;
        vtx.q = m_curQ;
        vtx.s = m_curS;
        vtx.t = m_curT;
        vtx.u = m_curU;
        vtx.v = m_curV;
        vtx.fog = m_curFog;
        vertexKick(!adk);
        break;
    }
    case 0x0A:
        m_curFog = static_cast<uint8_t>((hi >> 36) & 0xFF);
        break;
    case 0x0C:
    {
        PS2_IF_AGRESSIVE_LOGS({
            const uint32_t debugIndex = s_debugGsPackedVertexCount.fetch_add(1, std::memory_order_relaxed);
            if (debugIndex < 64u)
            {
                RUNTIME_LOG("[gs:packed-xyzf3] idx=" << debugIndex
                                                     << " x=" << static_cast<uint32_t>(lo & 0xFFFFu)
                                                     << " y=" << static_cast<uint32_t>((lo >> 32) & 0xFFFFu)
                                                     << " kick=0"
                                                     << " prim=" << static_cast<uint32_t>(m_prim.type)
                                                     << std::endl);
            }
        });
        GSVertex &vtx = m_vtxQueue[m_vtxCount % kMaxVerts];
        vtx.x = static_cast<float>(lo & 0xFFFF) / 16.0f;
        vtx.y = static_cast<float>((lo >> 32) & 0xFFFF) / 16.0f;
        vtx.z = static_cast<float>((hi >> 4) & 0xFFFFFF);
        vtx.r = m_curR;
        vtx.g = m_curG;
        vtx.b = m_curB;
        vtx.a = m_curA;
        vtx.q = m_curQ;
        vtx.s = m_curS;
        vtx.t = m_curT;
        vtx.u = m_curU;
        vtx.v = m_curV;
        vtx.fog = static_cast<uint8_t>((hi >> 36) & 0xFF);
        vertexKick(false);
        break;
    }
    case 0x0D:
    {
        PS2_IF_AGRESSIVE_LOGS({
            const uint32_t debugIndex = s_debugGsPackedVertexCount.fetch_add(1, std::memory_order_relaxed);
            if (debugIndex < 64u)
            {
                RUNTIME_LOG("[gs:packed-xyz3] idx=" << debugIndex
                                                    << " x=" << static_cast<uint32_t>(lo & 0xFFFFu)
                                                    << " y=" << static_cast<uint32_t>((lo >> 32) & 0xFFFFu)
                                                    << " kick=0"
                                                    << " prim=" << static_cast<uint32_t>(m_prim.type)
                                                    << std::endl);
            }
        });
        GSVertex &vtx = m_vtxQueue[m_vtxCount % kMaxVerts];
        vtx.x = static_cast<float>(lo & 0xFFFF) / 16.0f;
        vtx.y = static_cast<float>((lo >> 32) & 0xFFFF) / 16.0f;
        vtx.z = static_cast<float>(hi & 0xFFFFFFFF);
        vtx.r = m_curR;
        vtx.g = m_curG;
        vtx.b = m_curB;
        vtx.a = m_curA;
        vtx.q = m_curQ;
        vtx.s = m_curS;
        vtx.t = m_curT;
        vtx.u = m_curU;
        vtx.v = m_curV;
        vtx.fog = m_curFog;
        vertexKick(false);
        break;
    }
    case 0x0E:
    {
        uint8_t addr = static_cast<uint8_t>(hi & 0xFF);
        writeRegisterUnlocked(addr, lo);
        break;
    }
    case 0x0F:
        break;
    default:
        writeRegisterUnlocked(regDesc, lo);
        break;
    }
}

void ps2xVu1Fence(); // ps2_memory.cpp (MTVU)
extern thread_local bool g_ps2xOnVu1Worker;
void GS::writeRegister(uint8_t regAddr, uint64_t value)
{
    if (!g_ps2xOnVu1Worker)
    {
        // MTVU: if VU1 is still running, queue the register write behind its work instead of waiting
        extern bool ps2xMtvuDefer(std::function<void()> fn);
        if (ps2xMtvuDefer([this, regAddr, value]() { writeRegister(regAddr, value); }))
            return;
    }
    ps2xVu1Fence(); // direct register writes (HLE) only after the running VU1 program
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
    if (ps2xbridge::init())
    {
        // Pass direct register writes (HLE of sceGsPutDrawEnv & co.) on as an A+D packet on PATH3,
        // otherwise the PCSX2 GS does not know FRAME/ZBUF/SCISSOR/...
        alignas(16) uint64_t pkt[4];
        pkt[0] = 1ull | (1ull << 15) | (1ull << 60); // NLOOP=1, EOP, FLG=PACKED, NREG=1
        pkt[1] = 0xEull;                               // REGS[0] = A+D
        pkt[2] = value;
        pkt[3] = regAddr;
        // Not on PATH3: a split IMAGE transfer may be in progress there, the A+D packet
        // wuerde als Bilddaten verschluckt. PATH1-Pakete sind immer vollstaendig (EOP) -> sicher.
        static const int regPath = [] { const char *e = std::getenv("PS2X_PCSX2_REGPATH"); return e ? std::atoi(e) : 0; }();
        if (regPath >= 0)
            ps2xbridge::s_transfer(regPath, reinterpret_cast<const uint8_t *>(pkt), sizeof(pkt));
    }
    writeRegisterUnlocked(regAddr, value);
}

void GS::writeRegisterUnlocked(uint8_t regAddr, uint64_t value)
{
    const bool interestingReg =
        regAddr == GS_REG_PRIM ||
        regAddr == GS_REG_RGBAQ ||
        regAddr == GS_REG_ST ||
        regAddr == GS_REG_UV ||
        regAddr == GS_REG_XYZ2 ||
        regAddr == GS_REG_XYZ3 ||
        regAddr == GS_REG_XYZF2 ||
        regAddr == GS_REG_XYZF3 ||
        regAddr == GS_REG_TEX0_1 ||
        regAddr == GS_REG_TEX0_2 ||
        regAddr == GS_REG_TEX2_1 ||
        regAddr == GS_REG_TEX2_2 ||
        regAddr == GS_REG_TEXCLUT ||
        regAddr == GS_REG_TEXA ||
        regAddr == GS_REG_XYOFFSET_1 ||
        regAddr == GS_REG_XYOFFSET_2 ||
        regAddr == GS_REG_SCISSOR_1 ||
        regAddr == GS_REG_SCISSOR_2 ||
        regAddr == GS_REG_FRAME_1 ||
        regAddr == GS_REG_FRAME_2 ||
        regAddr == GS_REG_ALPHA_1 ||
        regAddr == GS_REG_ALPHA_2 ||
        regAddr == GS_REG_TEST_1 ||
        regAddr == GS_REG_TEST_2 ||
        regAddr == GS_REG_BITBLTBUF ||
        regAddr == GS_REG_TRXPOS ||
        regAddr == GS_REG_TRXREG ||
        regAddr == GS_REG_TRXDIR;

    PS2_IF_AGRESSIVE_LOGS({
        if (interestingReg)
        {
            const uint32_t debugIndex = s_debugGsRegisterCount.fetch_add(1, std::memory_order_relaxed);
            if (debugIndex < 128u)
            {
                RUNTIME_LOG("[gs:reg] idx=" << debugIndex
                                            << " reg=0x" << std::hex << static_cast<uint32_t>(regAddr)
                                            << " value=0x" << value
                                            << std::dec
                                            << std::endl);
            }
        }
    });

    const bool isCopyRelevantReg =
        regAddr == GS_REG_PRIM ||
        regAddr == GS_REG_TEX0_2 ||
        regAddr == GS_REG_TEX1_2 ||
        regAddr == GS_REG_ALPHA_2 ||
        regAddr == GS_REG_TEST_2 ||
        regAddr == GS_REG_PABE ||
        regAddr == GS_REG_FRAME_2 ||
        regAddr == GS_REG_XYOFFSET_2 ||
        regAddr == GS_REG_SCISSOR_2;
    PS2_IF_AGRESSIVE_LOGS({
        if (isCopyRelevantReg &&
            s_debugCopyRegCount.fetch_add(1u, std::memory_order_relaxed) < 64u)
        {
            RUNTIME_LOG("[gs:copy-reg] reg=0x"
                        << std::hex << static_cast<uint32_t>(regAddr)
                        << " value=0x" << value
                        << std::dec
                        << " primCtxt=" << static_cast<uint32_t>(m_prim.ctxt)
                        << " ctx0fbp=" << m_ctx[0].frame.fbp
                        << " ctx1fbp=" << m_ctx[1].frame.fbp
                        << std::endl);
        }
    });

    switch (regAddr)
    {
    case GS_REG_PRIM:
    {
        m_primRegister = decodePrimRegister(value);
        if (m_prmodecont)
        {
            m_prim = m_primRegister;
        }
        else
        {
            // PRIM always selects the primitive topology. With AC=0, all
            // rendering attributes remain sourced from PRMODE.
            m_prim.type = m_primRegister.type;
        }
        m_vtxCount = 0;
        m_vtxIndex = 0;
        break;
    }
    case GS_REG_RGBAQ:
    {
        m_curR = static_cast<uint8_t>(value & 0xFF);
        m_curG = static_cast<uint8_t>((value >> 8) & 0xFF);
        m_curB = static_cast<uint8_t>((value >> 16) & 0xFF);
        m_curA = static_cast<uint8_t>((value >> 24) & 0xFF);
        uint32_t qBits = static_cast<uint32_t>((value >> 32) & 0xFFFFFFFF);
        std::memcpy(&m_curQ, &qBits, 4);
        if (m_curQ == 0.0f)
            m_curQ = 1.0f;
        break;
    }
    case GS_REG_ST:
    {
        uint32_t sBits = static_cast<uint32_t>(value & 0xFFFFFFFF);
        uint32_t tBits = static_cast<uint32_t>((value >> 32) & 0xFFFFFFFF);
        std::memcpy(&m_curS, &sBits, 4);
        std::memcpy(&m_curT, &tBits, 4);
        break;
    }
    case GS_REG_UV:
    {
        m_curU = static_cast<uint16_t>(value & 0x3FFFu);
        m_curV = static_cast<uint16_t>((value >> 16) & 0x3FFFu);
        break;
    }
    case GS_REG_XYZF2:
    case GS_REG_XYZF3:
    {
        GSVertex &vtx = m_vtxQueue[m_vtxCount % kMaxVerts];
        vtx.x = static_cast<float>(value & 0xFFFF) / 16.0f;
        vtx.y = static_cast<float>((value >> 16) & 0xFFFF) / 16.0f;
        vtx.z = static_cast<double>((value >> 32) & 0xFFFFFF);
        vtx.fog = static_cast<uint8_t>((value >> 56) & 0xFF);
        vtx.r = m_curR;
        vtx.g = m_curG;
        vtx.b = m_curB;
        vtx.a = m_curA;
        vtx.q = m_curQ;
        vtx.s = m_curS;
        vtx.t = m_curT;
        vtx.u = m_curU;
        vtx.v = m_curV;
        vertexKick(regAddr == GS_REG_XYZF2);
        break;
    }
    case GS_REG_XYZ2:
    case GS_REG_XYZ3:
    {
        GSVertex &vtx = m_vtxQueue[m_vtxCount % kMaxVerts];
        vtx.x = static_cast<float>(value & 0xFFFF) / 16.0f;
        vtx.y = static_cast<float>((value >> 16) & 0xFFFF) / 16.0f;
        vtx.z = static_cast<double>((value >> 32) & 0xFFFFFFFF);
        vtx.r = m_curR;
        vtx.g = m_curG;
        vtx.b = m_curB;
        vtx.a = m_curA;
        vtx.q = m_curQ;
        vtx.s = m_curS;
        vtx.t = m_curT;
        vtx.u = m_curU;
        vtx.v = m_curV;
        vtx.fog = m_curFog;
        vertexKick(regAddr == GS_REG_XYZ2);
        break;
    }
    case GS_REG_TEX0_1:
    case GS_REG_TEX0_2:
    {
        int ci = (regAddr == GS_REG_TEX0_2) ? 1 : 0;
        auto &t = m_ctx[ci].tex0;
        t.tbp0 = static_cast<uint32_t>(value & 0x3FFF);
        t.tbw = static_cast<uint8_t>((value >> 14) & 0x3F);
        t.psm = static_cast<uint8_t>((value >> 20) & 0x3F);
        t.tw = static_cast<uint8_t>((value >> 26) & 0xF);
        t.th = static_cast<uint8_t>((value >> 30) & 0xF);
        t.tcc = static_cast<uint8_t>((value >> 34) & 0x1);
        t.tfx = static_cast<uint8_t>((value >> 35) & 0x3);
        t.cbp = static_cast<uint32_t>((value >> 37) & 0x3FFF);
        t.cpsm = static_cast<uint8_t>((value >> 51) & 0xF);
        t.csm = static_cast<uint8_t>((value >> 55) & 0x1);
        t.csa = static_cast<uint8_t>((value >> 56) & 0x1F);
        t.cld = static_cast<uint8_t>((value >> 61) & 0x7);
        m_backend->LoadClut(t, m_texclut);
        break;
    }
    case GS_REG_CLAMP_1:
    case GS_REG_CLAMP_2:
    {
        int ci = (regAddr == GS_REG_CLAMP_2) ? 1 : 0;
        m_ctx[ci].clamp = value;
        break;
    }
    case GS_REG_FOG:
        m_curFog = static_cast<uint8_t>((value >> 56) & 0xFF);
        break;
    case GS_REG_TEX1_1:
    case GS_REG_TEX1_2:
    {
        int ci = (regAddr == GS_REG_TEX1_2) ? 1 : 0;
        m_ctx[ci].tex1 = value;
        break;
    }
    case GS_REG_TEX2_1:
    case GS_REG_TEX2_2:
    {
        int ci = (regAddr == GS_REG_TEX2_2) ? 1 : 0;
        auto &t = m_ctx[ci].tex0;
        t.psm = static_cast<uint8_t>((value >> 20) & 0x3F);
        t.cbp = static_cast<uint32_t>((value >> 37) & 0x3FFF);
        t.cpsm = static_cast<uint8_t>((value >> 51) & 0xF);
        t.csm = static_cast<uint8_t>((value >> 55) & 0x1);
        t.csa = static_cast<uint8_t>((value >> 56) & 0x1F);
        t.cld = static_cast<uint8_t>((value >> 61) & 0x7);
        m_backend->LoadClut(t, m_texclut);
        break;
    }
    case GS_REG_XYOFFSET_1:
    case GS_REG_XYOFFSET_2:
    {
        int ci = (regAddr == GS_REG_XYOFFSET_2) ? 1 : 0;
        m_ctx[ci].xyoffset.ofx = static_cast<uint16_t>(value & 0xFFFF);
        m_ctx[ci].xyoffset.ofy = static_cast<uint16_t>((value >> 32) & 0xFFFF);
        break;
    }
    case GS_REG_PRMODECONT:
    {
        m_prmodecont = (value & 1) != 0;
        const GSPrimType type = m_primRegister.type;
        m_prim = m_prmodecont ? m_primRegister : m_prmodeRegister;
        m_prim.type = type;
        break;
    }
    case GS_REG_PRMODE:
    {
        m_prmodeRegister = decodePrimRegister(value);
        if (!m_prmodecont)
        {
            const GSPrimType type = m_primRegister.type;
            m_prim = m_prmodeRegister;
            m_prim.type = type;
        }
        break;
    }
    case GS_REG_TEXCLUT:
        m_texclut.cbw = static_cast<uint8_t>(value & 0x3Fu);
        m_texclut.cou = static_cast<uint8_t>((value >> 6) & 0x3Fu);
        m_texclut.cov = static_cast<uint16_t>((value >> 12) & 0x3FFu);
        break;
    case GS_REG_SCISSOR_1:
    case GS_REG_SCISSOR_2:
    {
        int ci = (regAddr == GS_REG_SCISSOR_2) ? 1 : 0;
        m_ctx[ci].scissor.x0 = static_cast<uint16_t>(value & 0x7FF);
        m_ctx[ci].scissor.x1 = static_cast<uint16_t>((value >> 16) & 0x7FF);
        m_ctx[ci].scissor.y0 = static_cast<uint16_t>((value >> 32) & 0x7FF);
        m_ctx[ci].scissor.y1 = static_cast<uint16_t>((value >> 48) & 0x7FF);
        break;
    }
    case GS_REG_ALPHA_1:
    case GS_REG_ALPHA_2:
    {
        int ci = (regAddr == GS_REG_ALPHA_2) ? 1 : 0;
        m_ctx[ci].alpha = value;
        break;
    }
    case GS_REG_TEST_1:
    case GS_REG_TEST_2:
    {
        int ci = (regAddr == GS_REG_TEST_2) ? 1 : 0;
        m_ctx[ci].test = value;
        break;
    }
    case GS_REG_FRAME_1:
    case GS_REG_FRAME_2:
    {
        int ci = (regAddr == GS_REG_FRAME_2) ? 1 : 0;
        m_ctx[ci].frame.fbp = static_cast<uint32_t>(value & 0x1FF);
        m_ctx[ci].frame.fbw = static_cast<uint32_t>((value >> 16) & 0x3F);
        m_ctx[ci].frame.psm = static_cast<uint8_t>((value >> 24) & 0x3F);
        m_ctx[ci].frame.fbmsk = static_cast<uint32_t>((value >> 32) & 0xFFFFFFFF);
        break;
    }
    case GS_REG_ZBUF_1:
    case GS_REG_ZBUF_2:
    {
        int ci = (regAddr == GS_REG_ZBUF_2) ? 1 : 0;
        m_ctx[ci].zbuf.zbp = value & 0x1FF;
        m_ctx[ci].zbuf.psm = ((value >> 24) & 0xF) | 0x30;
        m_ctx[ci].zbuf.zmask = (value >> 32) & 1;
        break;
    }
    case GS_REG_FBA_1:
    case GS_REG_FBA_2:
    {
        int ci = (regAddr == GS_REG_FBA_2) ? 1 : 0;
        m_ctx[ci].fba = value;
        break;
    }
    case GS_REG_BITBLTBUF:
    {
        m_bitbltbuf.sbp = static_cast<uint32_t>(value & 0x3FFF);
        m_bitbltbuf.sbw = static_cast<uint8_t>((value >> 16) & 0x3F);
        m_bitbltbuf.spsm = static_cast<uint8_t>((value >> 24) & 0x3F);
        m_bitbltbuf.dbp = static_cast<uint32_t>((value >> 32) & 0x3FFF);
        m_bitbltbuf.dbw = static_cast<uint8_t>((value >> 48) & 0x3F);
        m_bitbltbuf.dpsm = static_cast<uint8_t>((value >> 56) & 0x3F);
        break;
    }
    case GS_REG_TRXPOS:
    {
        m_trxpos.ssax = static_cast<uint16_t>(value & 0x7FF);
        m_trxpos.ssay = static_cast<uint16_t>((value >> 16) & 0x7FF);
        m_trxpos.dsax = static_cast<uint16_t>((value >> 32) & 0x7FF);
        m_trxpos.dsay = static_cast<uint16_t>((value >> 48) & 0x7FF);
        m_trxpos.dir = static_cast<uint8_t>((value >> 59) & 0x3);
        break;
    }
    case GS_REG_TRXREG:
    {
        m_trxreg.rrw = static_cast<uint16_t>(value & 0xFFF);
        m_trxreg.rrh = static_cast<uint16_t>((value >> 32) & 0xFFF);
        break;
    }
    case GS_REG_TRXDIR:
    {
        m_trxdir = static_cast<uint32_t>(value & 0x3);

        if (m_backend)
        {
            GSTransferCommand command{};
            command.bitbltbuf = m_bitbltbuf;
            command.trxpos = m_trxpos;
            command.trxreg = m_trxreg;
            command.direction = m_trxdir;
            m_backend->BeginTransfer(command);
        }
        recordTransferDebugEventUnlocked();
        break;
    }
    case GS_REG_HWREG:
    {
        uint8_t buf[8];
        std::memcpy(buf, &value, 8);
        processImageData(buf, 8);
        break;
    }
    case GS_REG_PABE:
        m_pabe = (value & 1u) != 0u;
        break;
    case GS_REG_FOGCOL:
        m_fogR = static_cast<uint8_t>(value & 0xFFu);
        m_fogG = static_cast<uint8_t>((value >> 8) & 0xFFu);
        m_fogB = static_cast<uint8_t>((value >> 16) & 0xFFu);
        break;
    case GS_REG_TEXFLUSH:
        if (m_backend)
            m_backend->TextureFlush();
        break;
    case GS_REG_SCANMSK:
        m_scanmsk = value;
        break;
    case GS_REG_DIMX:
        m_dimx = value;
        break;
    case GS_REG_DTHE:
        m_dthe = value;
        break;
    case GS_REG_COLCLAMP:
        m_colclamp = value;
        break;
    case GS_REG_MIPTBP1_1:
    case GS_REG_MIPTBP1_2:
    {
        const int ci = (regAddr == GS_REG_MIPTBP1_2) ? 1 : 0;
        m_ctx[ci].miptbp1 = value;
        break;
    }
    case GS_REG_MIPTBP2_1:
    case GS_REG_MIPTBP2_2:
    {
        const int ci = (regAddr == GS_REG_MIPTBP2_2) ? 1 : 0;
        m_ctx[ci].miptbp2 = value;
        break;
    }
    case GS_REG_TEXA:
    {
        m_texa.ta0 = static_cast<uint8_t>(value & 0xFFu);
        m_texa.aem = ((value >> 15) & 0x1u) != 0u;
        m_texa.ta1 = static_cast<uint8_t>((value >> 32) & 0xFFu);
        PS2_IF_AGRESSIVE_LOGS({
            const uint32_t texaIndex = s_debugTexaWriteCount.fetch_add(1u, std::memory_order_relaxed);
            if (texaIndex < 24u)
            {
                RUNTIME_LOG("[gs:texa] idx=" << texaIndex
                                             << " value=0x" << std::hex << value
                                             << " ta0=0x" << ((value >> 0) & 0xFFu)
                                             << " aem=" << ((value >> 15) & 0x1u)
                                             << " ta1=0x" << ((value >> 32) & 0xFFu)
                                             << std::dec
                                             << std::endl);
            }
        });
        break;
    }
    case GS_REG_SIGNAL:
    {
        if (m_privRegs)
        {
            uint32_t id = static_cast<uint32_t>(value & 0xFFFFFFFF);
            uint32_t mask = static_cast<uint32_t>(value >> 32);
            uint32_t lo = static_cast<uint32_t>(m_privRegs->siglblid & 0xFFFFFFFF);
            lo = (lo & ~mask) | (id & mask);
            m_privRegs->siglblid = (m_privRegs->siglblid & 0xFFFFFFFF00000000ULL) | lo;
            m_privRegs->csr.fetch_or(0x1);
            ps2xWakeForGsEvent();
            { static const bool sl = std::getenv("PS2X_SIGLOG") != nullptr; static int n = 0;
              if (sl && n < 400) { ++n; std::fprintf(stderr, "[SIG] SIGNAL id=%08x tick=%llu\n", id, (unsigned long long)m_privRegs->vsyncTick.load()); } }
        }
        break;
    }
    case GS_REG_FINISH:
    {
        if (m_backend)
        {
            m_backend->Flush();
            m_backend->Sync(GSSyncReason::Finish);
        }
        if (m_privRegs)
        {
            m_privRegs->csr.fetch_or(0x2);
            ps2xWakeForGsEvent();
        }
        break;
    }
    case GS_REG_LABEL:
    {
        if (m_privRegs)
        {
            uint32_t id = static_cast<uint32_t>(value & 0xFFFFFFFF);
            uint32_t mask = static_cast<uint32_t>(value >> 32);
            uint32_t hi = static_cast<uint32_t>(m_privRegs->siglblid >> 32);
            hi = (hi & ~mask) | (id & mask);
            m_privRegs->siglblid = (static_cast<uint64_t>(hi) << 32) | (m_privRegs->siglblid & 0xFFFFFFFF);
        }
        break;
    }
    case 0x59:
        if (m_privRegs)
            m_privRegs->dispfb1 = value;
        break;
    case 0x5a:
        if (m_privRegs)
            m_privRegs->display1 = value;
        break;
    case 0x5b:
        if (m_privRegs)
            m_privRegs->dispfb2 = value;
        break;
    case 0x5c:
        if (m_privRegs)
            m_privRegs->display2 = value;
        break;
    case 0x5f:
        if (m_privRegs)
            m_privRegs->bgcolor = value;
        break;
    default:
        break;
    }

    recordRegisterDebugEventUnlocked(regAddr, value);
}

void GS::vertexKick(bool drawing)
{
    ++m_vtxCount;
    ++m_vtxIndex;

    PS2_IF_AGRESSIVE_LOGS({
        const uint32_t debugIndex = s_debugGsVertexKickCount.fetch_add(1, std::memory_order_relaxed);
        if (debugIndex < 96u)
        {
            RUNTIME_LOG("[gs:kick] idx=" << debugIndex
                                         << " drawing=" << static_cast<uint32_t>(drawing ? 1u : 0u)
                                         << " prim=" << static_cast<uint32_t>(m_prim.type)
                                         << " vtxCount=" << m_vtxCount
                                         << std::endl);
        }
    });

    int needed = 0;
    switch (m_prim.type)
    {
    case GS_PRIM_POINT:
        needed = 1;
        break;
    case GS_PRIM_LINE:
        needed = 2;
        break;
    case GS_PRIM_LINESTRIP:
        needed = 2;
        break;
    case GS_PRIM_TRIANGLE:
        needed = 3;
        break;
    case GS_PRIM_TRISTRIP:
        needed = 3;
        break;
    case GS_PRIM_TRIFAN:
        needed = 3;
        break;
    case GS_PRIM_SPRITE:
        needed = 2;
        break;
    default:
        return;
    }

    if (m_vtxCount < needed)
        return;

    // With the PCSX2 GS our rasterizer does not draw: do not even build and copy the draw job
    if (drawing && m_backend && !g_ps2xOwnRasterOff.load(std::memory_order_relaxed))
    {
        GSPrimitiveBatch batch = buildDrawBatch(needed);
        updatePreferredDisplaySourceForDraw(batch);
        m_backend->Submit(batch);
        recordDrawDebugEventUnlocked(needed);
    }

    switch (m_prim.type)
    {
    case GS_PRIM_LINE:
    case GS_PRIM_TRIANGLE:
    case GS_PRIM_SPRITE:
    case GS_PRIM_POINT:
        m_vtxCount = 0;
        break;
    case GS_PRIM_LINESTRIP:
        m_vtxQueue[0] = m_vtxQueue[1];
        m_vtxCount = 1;
        break;
    case GS_PRIM_TRISTRIP:
        m_vtxQueue[0] = m_vtxQueue[1];
        m_vtxQueue[1] = m_vtxQueue[2];
        m_vtxCount = 2;
        break;
    case GS_PRIM_TRIFAN:
        m_vtxQueue[1] = m_vtxQueue[2];
        m_vtxCount = 2;
        break;
    default:
        m_vtxCount = 0;
        break;
    }
}

void GS::processImageData(const uint8_t *data, uint32_t sizeBytes)
{
    if (m_backend)
        m_backend->UploadImage(data, sizeBytes);
}


bool GS::clearFramebufferContext(uint32_t contextIndex, uint32_t rgba)
{
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
    return m_backend && m_backend->ClearFramebuffer(m_ctx[(contextIndex != 0u) ? 1 : 0], rgba);
}

bool GS::clearActiveFramebuffer(uint32_t rgba)
{
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
    return m_backend && m_backend->ClearFramebuffer(activeContext(), rgba);
}

uint32_t GS::consumeLocalToHostBytes(uint8_t *dst, uint32_t maxBytes)
{
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
    return m_backend ? m_backend->ConsumeLocalToHostBytes(dst, maxBytes) : 0u;
}

void GS::setRasterBackend(std::unique_ptr<GSRasterBackend> backend)
{
    if (!backend)
        backend = std::make_unique<GSCpuBackend>();

    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
    std::lock_guard<std::mutex> backendLock(m_backendLifetimeMutex);
    if (m_backend)
    {
        m_backend->Flush();
        m_backend->Sync(GSSyncReason::Reset);

        // The external 4 MiB GS allocation is the backend hand-off format.
        // This keeps hot backend replacement deterministic even when a future
        // GPU backend keeps a private/mirrored local-memory representation.
        if (m_localMemoryStorage && m_localMemorySize != 0u)
        {
            std::vector<uint8_t> localMemory;
            m_backend->SnapshotVram(localMemory);
            const size_t bytes = std::min<size_t>(localMemory.size(), m_localMemorySize);
            if (bytes != 0u)
                std::memcpy(m_localMemoryStorage, localMemory.data(), bytes);
        }
    }

    m_backend = std::move(backend);
    m_backend->Initialize(m_localMemoryStorage, m_localMemorySize);
}

uint32_t GS::ReadVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y) const
{
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
    return m_backend ? m_backend->ReadVram(psm, base, bw, x, y) : 0u;
}

void GS::WriteVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y, uint32_t value)
{
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
    if (m_backend)
        m_backend->WriteVram(psm, base, bw, x, y, value);
}

GSPrimitiveBatch GS::buildDrawBatch(int vertexCount) const
{
    GSPrimitiveBatch batch{};
    batch.vertexCount = static_cast<uint8_t>(std::min(vertexCount, 3));
    for (int i = 0; i < batch.vertexCount; ++i)
        batch.vertices[static_cast<size_t>(i)] = m_vtxQueue[i];
    batch.state.context = m_ctx[m_prim.ctxt ? 1 : 0];
    batch.state.prim = m_prim;
    batch.state.texa = m_texa;
    batch.state.texclut = m_texclut;
    batch.state.pabe = m_pabe;
    batch.state.scanmsk = m_scanmsk;
    batch.state.dimx = m_dimx;
    batch.state.dthe = m_dthe;
    batch.state.colclamp = m_colclamp;
    batch.state.fogR = m_fogR;
    batch.state.fogG = m_fogG;
    batch.state.fogB = m_fogB;
    batch.state.textureWidth = static_cast<uint16_t>(1u << std::min<uint32_t>(batch.state.context.tex0.tw, 10u));
    batch.state.textureHeight = static_cast<uint16_t>(1u << std::min<uint32_t>(batch.state.context.tex0.th, 10u));
    const uint64_t tex1 = batch.state.context.tex1;
    const uint8_t mmag = static_cast<uint8_t>((tex1 >> 5u) & 0x1u);
    const uint8_t mmin = static_cast<uint8_t>((tex1 >> 6u) & 0x7u);
    batch.state.linearFilter = mmag != 0u || mmin == 1u || (mmin & 0x4u) != 0u;
    return batch;
}

void GS::updatePreferredDisplaySourceForDraw(const GSPrimitiveBatch &batch)
{
    const GSDrawState &state = batch.state;
    const GSContext &ctx = state.context;
    if (m_hasPreferredDisplaySource && ctx.frame.fbp == m_preferredDisplayDestFbp)
        m_hasPreferredDisplaySource = false;
    if (state.prim.type != GS_PRIM_SPRITE || batch.vertexCount < 2u)
        return;

    const GSVertex &v0 = batch.vertices[0];
    const GSVertex &v1 = batch.vertices[1];
    int x0 = static_cast<int>(v0.x) - (ctx.xyoffset.ofx >> 4);
    int y0 = static_cast<int>(v0.y) - (ctx.xyoffset.ofy >> 4);
    int x1 = static_cast<int>(v1.x) - (ctx.xyoffset.ofx >> 4);
    int y1 = static_cast<int>(v1.y) - (ctx.xyoffset.ofy >> 4);
    if (x0 > x1) std::swap(x0, x1);
    if (y0 > y1) std::swap(y0, y1);
    const int xEnd = x0 + std::max(1, x1 - x0) - 1;
    const int yEnd = y0 + std::max(1, y1 - y0) - 1;
    const uint8_t alphaMode = static_cast<uint8_t>(ctx.alpha & 0xFFu);
    const uint8_t alphaFix = static_cast<uint8_t>((ctx.alpha >> 32u) & 0xFFu);
    const bool displayCopy = state.prim.tme && state.prim.abe && state.prim.fst && state.prim.ctxt &&
                             ctx.frame.fbp != ctx.tex0.tbp0 && alphaMode == 0x64u &&
                             (alphaFix == 0x60u || alphaFix == 0x80u) &&
                             x0 <= 0 && y0 <= 0 && xEnd >= 639 && yEnd >= 447;
    if (displayCopy)
    {
        m_preferredDisplaySourceFrame = {ctx.tex0.tbp0, ctx.tex0.tbw, ctx.tex0.psm, 0u};
        m_preferredDisplayDestFbp = ctx.frame.fbp;
        m_hasPreferredDisplaySource = true;
    }
}
