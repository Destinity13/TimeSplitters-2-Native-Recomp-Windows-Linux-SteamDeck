// TimeSplitters 2 (NTSC-U, SLUS-203.14) – game override for PS2Recomp.
// Derived from sotc_override.cpp (Shadow of the Colossus): the general parts (ISO access, test harness with
// screenshots/exit tick, watchdog, per-minute profile, RAM dumps) were kept; all SotC addresses were removed.
// Environment variables are still called SOTC_* for now (same test farm/tools); TS2_ISO works as well.
#if defined(__linux__)
#include <unistd.h>
#include <dlfcn.h>
#include <linux/perf_event.h>
#include <linux/hw_breakpoint.h>
#include <sys/syscall.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/mman.h>
#include <execinfo.h>
#include <dirent.h>
#endif
#include <cstring>
#include <random>
#include <cerrno>
#include <ctime>
#include <vector>
#include <cmath>
// Shadow of the Colossus (PAL, SCES-53326) – game override for PS2Recomp.
//
// Lives in ps2xRuntime/src/runner/ next to the generated code. Applied by the runtime when
// the boot ELF is named SCES_533.26 with entry 0x100010 (the statically linked image built by
// tools/link_sotc.py: loader + KERNEL/MANAGER/GAMECORE at their real load addresses).
//
// All addresses below are in the (unrelocated) loader SCES_533.26 and therefore stable.
//
// Replacing an entry in the runtime's function table is not enough: generated code reaches
// many functions by direct C++ calls (e.g. tail jumps). So every overridden function is defined
// here under its generated name (e.g. sceGetstat_0x10c2a8) and tools/build_game.sh removes the
// generated .cpp of the same name. The table entries are replaced as well, for indirect calls.

#include "ps2_runtime.h"
#include "runtime/gs/ps2_gs_memory.h"
#include "ps2_runtime_macros.h"
#include "game_overrides.h"
#include "runtime/ee_scheduler.h"
#include "ps2_syscalls.h"
#include "ps2_stubs.h"
#include "ps2_recompiled_functions.h"
bool ps2xGsBridgeMenuOpen();   // gs_frontend.cpp
namespace ts2_dtoa_test { void versuch(uint8_t *rdram, R5900Context *ctx, PS2Runtime *rt); }   // TS2_DTOA_TEST
extern std::atomic<uint64_t> g_ps2xBildwechsel;   // gs_frontend.cpp: real frame flips
bool ps2xGsBridgePad(uint16_t &pressed, uint8_t axes[4]);   // gs_frontend.cpp
int ps2xGsBridgeMouse(int &dx, int &dy);                     // gs_frontend.cpp (mouse aiming)
int ps2xGsBridgeOption(int id, int fallback);               // gs_frontend.cpp
void ps2xGsBridgeText(int id, const char *text);          // gs_frontend.cpp
int ps2xGsBridgeTextGet(int id, char *buf, int len);      // gs_frontend.cpp
// Online+ in-game server browser: chosen server (overrides online_server from the settings at the next network init)
std::mutex g_opServerM; std::string g_opServerWahl;
void ps2xGsBridgeCaptureEnd();                              // gs_frontend.cpp: finish the video recording
// view offset the Online+ impact effects applied since the last mouse tick (the mouse keeps it instead of undoing it)
static float g_ts2PunchDP = 0.f, g_ts2PunchDY = 0.f;
extern std::atomic<unsigned> g_ps2xPadZusatz;
extern std::atomic<unsigned> g_ps2xPadMaske;
extern std::atomic<unsigned> g_ps2xPadRoh;
extern std::atomic<unsigned> g_ps2xPadRStickAus;
extern std::atomic<unsigned> g_ps2xPadRStickSetzen;
extern std::atomic<unsigned> g_ps2xPadAchsenRoh;
extern std::atomic<bool> g_ps2xUplink;                       // ps2_memory.cpp: Temporal Uplink held (widescreen HUD squeeze skips its map)
extern std::atomic<long long> g_ps2xScopeMs;                // ps2_memory.cpp: sniper scope feedback last seen (ms)               // gs_frontend.cpp: additional PS2 buttons for player 1

#include <chrono>
#include <thread>
#ifdef _WIN32
extern "C" __declspec(dllimport) unsigned int __stdcall timeBeginPeriod(unsigned int); // winmm, without windows.h (near/far macros ...)
#endif

#include <algorithm>
#include <array>
#include <utility>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <atomic>
#include <map>
#include <unordered_map>
#include <mutex>
#include <string>
#include <string>
#include <cstdlib>

void ps2xSetPadOverridePort2(uint16_t buttons, uint8_t lx, uint8_t ly, uint8_t rx, uint8_t ry);
void ps2xSetPadOverridePlayer(int player, uint16_t buttons, uint8_t lx, uint8_t ly, uint8_t rx, uint8_t ry);
bool ps2xMultitapActive();
// Online+ hits/kills not yet reported to the online server (ts2netz sends them every 5 s)
std::atomic<uint32_t> g_opTrefferOffen{0}, g_opKillsOffen{0};

std::atomic<uint32_t> g_ts2EigenKills{0};   // own kills seen by the hit detection (Online+ scoreboard when the game's counter stays)
std::atomic<uint64_t> g_ts2EigenKillTick[8];   // ticks of the last own kills, for the kill feed

std::atomic<uint64_t> g_ts2EigenTrefferTick[8]; std::atomic<uint32_t> g_ts2EigenTreffer{0};   // ticks of the player's last hits
namespace ts2melde {
    // Online+ texts shown through the game's OWN on-screen message system (same font, place and animation as
    // "First kill: X!" / "Killed by X"): 0x32A9B0(screen a0, delay f12, type a1 1..6, text a2) - per screen a queue
    // of 16 messages at 0x4D9D60 + screen * 0x1254. Called from the game thread (hook in applyTs2), never from VBlank.
    std::mutex m; std::deque<std::pair<std::string, int>> q;
    void push(const std::string &t, int typ) { std::lock_guard<std::mutex> lk(m); if (q.size() < 16) q.emplace_back(t, typ); }
    bool pop(std::string &t, int &typ) { std::lock_guard<std::mutex> lk(m); if (q.empty()) return false; t = q.front().first; typ = q.front().second; q.pop_front(); return true; }
}
static int g_ts2Serie = 0;   // own kills since the last death (Online+ streak messages)
namespace ts2impact {
    // Online+ impact effects (bridge options: 15 = on/off, 16 = damage kick 0..100; events 13 = hit counter, 14 = kill counter).
    // Characters (player and enemies) share one struct: health float at +0x19C, max at +0x1A0 (as the game's
    // "infinite health" code shows). Player: *(*(0x59DC0C) + 0x1BC). Enemies are found by a periodic scan for the struct
    // signature (zero words that props do not have, plausible health/max); a drop of their health is a hit, 0 a kill.
    // TS2_IMPACT=1 switches it on without the menu (test runs).
    constexpr uint32_t kSpielerZeiger = 0x0059DC0Cu;
    inline float rf(const uint8_t *r, uint32_t a) { float v; std::memcpy(&v, r + (a & 0x1FFFFFCu), 4); return v; }
    inline uint32_t ru(const uint8_t *r, uint32_t a) { uint32_t v; std::memcpy(&v, r + (a & 0x1FFFFFCu), 4); return v; }
    inline void wf(uint8_t *r, uint32_t a, float v) { std::memcpy(r + (a & 0x1FFFFFCu), &v, 4); }
    bool istFigur(const uint8_t *r, uint32_t b)
    {
        // zero in every character (player and enemies), not in props; 36 words keep random data out
        static const uint32_t nullen[] = {0x28, 0x34, 0x38, 0x44, 0x48, 0x4C, 0x50, 0x54, 0x88, 0x90, 0x94, 0xB0, 0xB8, 0xC0, 0xC4,
                                          0xDC, 0xE0, 0xE4, 0xEC, 0xFC, 0x104, 0x114, 0x120, 0x12C, 0x134, 0x13C, 0x140, 0x144, 0x148,
                                          0x150, 0x154, 0x158, 0x15C, 0x160, 0x164, 0x168};
        if (b < 0x100000u || b + 0x2A0u > 0x2000000u) return false;
        const float h = rf(r, b + 0x19C), m = rf(r, b + 0x1A0);
        if (!(m >= 2.0f && m <= 500.0f) || !(h >= -1.0f && h <= m + 0.01f)) return false;
        for (uint32_t o : nullen) if (ru(r, b + o) != 0u) return false;
        return true;
    }
    void tick(uint8_t *r, uint64_t t, bool feuer)
    {
        if (!r) return;
        static uint64_t letzterSchuss = 0; if (feuer) letzterSchuss = t;
        static const int envAn = [] { const char *e = std::getenv("TS2_IMPACT"); return (e && *e == '1') ? 1 : 0; }();
        static int an = 0, stoss = 50; static uint64_t naechsteOpt = 0;
        if (t >= naechsteOpt) { an = ::ps2xGsBridgeOption(15, envAn); stoss = ::ps2xGsBridgeOption(16, 50); naechsteOpt = t + 30; }
        struct Fig { uint32_t b; float h, m; uint64_t seit; };   // last health, max, tick of the last change
        static std::vector<Fig> figuren; static uint64_t naechsterScan = 0;
        static int treffer = 0, kills = 0; static float spielerLetzt = -1.0f; static uint32_t spielerLetztAdr = 0;
        static float kickPitch = 0.0f, kickYaw = 0.0f;
        {
            static const bool plog = std::getenv("TS2_IMPACT_LOG") != nullptr;
            const uint32_t so = ru(r, kSpielerZeiger) & 0x1FFFFFFu;
            if (plog && so && so < 0x1FFF000u && t % 60u == 0u) std::fprintf(stderr, "[impact] tick %llu pitch %.2f obj %08x\n", (unsigned long long)t, rf(r, so + 0x14C), so);
        }
        if (!an) { figuren.clear(); spielerLetzt = -1.0f; return; }
        const uint32_t spObj = ru(r, kSpielerZeiger) & 0x1FFFFFFu;
        const uint32_t sp = (spObj && spObj < 0x1FFF000u) ? (ru(r, spObj + 0x1BC) & 0x1FFFFFFu) : 0u;
        // figure scan: spread over 30 frames (0.2 ms per frame instead of a 3 ms spike twice a second);
        // TS2_IMPACT_SCAN=1 keeps the old one-shot scan. Known figures keep their last health.
        {
            static const bool einmal = [] { const char *e = std::getenv("TS2_IMPACT_SCAN"); return e && *e == '1'; }();
            constexpr uint32_t kVon = 0x500000u, kBis = 0x1C00000u, kTeile = 30u, kSchritt = ((kBis - kVon) / kTeile + 15u) & ~15u;
            static std::vector<Fig> neu; static uint32_t teil = 0;
            static double maxMs = 0.0;
            auto scanne = [&](uint32_t von, uint32_t bis) {
                const auto t0 = std::chrono::steady_clock::now();
                struct Mess { std::chrono::steady_clock::time_point t0; ~Mess() { const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count(); if (ms > maxMs) maxMs = ms; } } mess{t0};
                for (uint32_t b = von; b < bis && neu.size() < 256; b += 16u)
                    if (b != sp && istFigur(r, b))
                    {
                        Fig n{b, rf(r, b + 0x19C), rf(r, b + 0x1A0), t};
                        for (const auto &f : figuren) if (f.b == b) { n = f; break; }
                        neu.push_back(n);
                    }
            };
            if (einmal) { if (t >= naechsterScan) { naechsterScan = t + 30; neu.clear(); scanne(kVon, kBis); figuren.swap(neu); neu.clear(); } }
            else
            {
                const uint32_t von = kVon + teil * kSchritt, bis = std::min(kBis, von + kSchritt);
                if (teil == 0) neu.clear();
                scanne(von, bis);
                if (++teil >= kTeile)
                {
                    teil = 0;

                    for (auto &n : neu) for (const auto &f : figuren) if (f.b == n.b) { n = f; break; }
                    figuren.swap(neu);
                }
            }
            static const bool slog = std::getenv("TS2_IMPACT_LOG") != nullptr;
            if (slog && t % 600u == 0u)
            {
                size_t voll = 0; for (uint32_t b = kVon; b < kBis; b += 16u) if (b != sp && istFigur(r, b)) ++voll;
                std::fprintf(stderr, "[impact] tick %llu scan max %.2f ms per tick (last 10 s), list %zu, one-shot now %zu\n", (unsigned long long)t, maxMs, figuren.size(), voll); maxMs = 0.0;
            }
        }
        // only in a level (player character valid); damage is a step: the value was stable before, max stays the same,
        // the drop is at least 2 % of max
        // ... and only shortly after the player fired (other damage - enemies among themselves, scenery - gives no marker)
        const bool imLevel = sp && istFigur(r, sp) && letzterSchuss && t - letzterSchuss <= 60;
        bool hatTreffer = false, hatKill = false;
        for (auto &f : figuren)
        {
            if (!istFigur(r, f.b)) { f.h = -2.0f; f.seit = t; continue; }
            const float h = rf(r, f.b + 0x19C), m = rf(r, f.b + 0x1A0);
            if (m != f.m) { f.m = m; f.h = h; f.seit = t; continue; }
            if (h != f.h)
            {
                if (imLevel && f.h > 0.0f && h < f.h && f.h - h >= 0.02f * m && t - f.seit >= 3) { hatTreffer = true; if (h <= 0.0f) hatKill = true; }
                f.h = h; f.seit = t;
            }
        }
        if (hatTreffer) { ::ps2xGsBridgeOption(13, ++treffer); ++g_opTrefferOffen; g_ts2EigenTrefferTick[g_ts2EigenTreffer.fetch_add(1) & 7u].store(t); }
        if (hatKill)
        {
            ::ps2xGsBridgeOption(14, ++kills); ++g_opKillsOffen;
            g_ts2EigenKillTick[g_ts2EigenKills.fetch_add(1) & 7u].store(t);   // own kill for the kill feed / scoreboard

            // multi-kill (kills within 4 s) and streak (kills without dying) -> the game's own big on-screen messages
            static uint64_t letzterKill = 0; static int kette = 0;
            kette = (letzterKill && t - letzterKill <= 240) ? kette + 1 : 1; letzterKill = t; ++g_ts2Serie;
            static const char *kKette[] = {"", "", "DOUBLE KILL", "TRIPLE KILL", "MULTI KILL", "MEGA KILL", "MONSTER KILL"};
            static const char *kSerie[] = {"KILLING SPREE", "RAMPAGE", "UNSTOPPABLE", "GODLIKE"};
            if (::ps2xGsBridgeOption(23, 1))
            {
                if (g_ts2Serie >= 5 && g_ts2Serie % 5 == 0) ts2melde::push(kSerie[std::min(g_ts2Serie / 5 - 1, 3)], 3);
                else if (kette >= 2) ts2melde::push(kKette[std::min(kette, 6)], 4);
            }
        }
        static const bool log = std::getenv("TS2_IMPACT_LOG") != nullptr;
        if (log && (hatTreffer || hatKill)) std::fprintf(stderr, "[impact] tick %llu hit %d kill %d (figures %zu)\n", (unsigned long long)t, treffer, kills, figuren.size());
        if (log && t % 600u == 0u) std::fprintf(stderr, "[impact] tick %llu figures %zu player %08x\n", (unsigned long long)t, figuren.size(), sp);
        // Arcade punch (Online+): camera offsets that spring back, so the aim is not moved for good.
        // Damage taken -> kick up + sideways (option 16), own shots -> small recoil punch (option 21, not while zoomed).
        // Damage / death are reported to the bridge (19 = damage strength 1..100, 20 = death counter) for the red edge
        // flash and the kill streaks.
        static float punchP = 0.0f, punchY = 0.0f, angewP = 0.0f, angewY = 0.0f;
        static int rueckstoss = 40, tode = 0; static uint64_t naechsteOpt2 = 0, letzterPunch = 0; static bool feuerVor = false;
        static const int envRs = [] { const char *e = std::getenv("TS2_RUECKSTOSS"); return e ? std::atoi(e) : -1; }();
        if (t >= naechsteOpt2) { rueckstoss = envRs >= 0 ? envRs : ::ps2xGsBridgeOption(21, 40); naechsteOpt2 = t + 30; }

        const bool spielerOk = sp && istFigur(r, sp);
        if (spielerOk)
        {
            const float h = rf(r, sp + 0x19C), m = rf(r, sp + 0x1A0);
            if (sp == spielerLetztAdr && spielerLetzt > 0.0f && h < spielerLetzt - 0.001f)
            {
                const float anteil = std::min(1.0f, (spielerLetzt - h) / std::max(1.0f, m * 0.15f));
                if (stoss > 0)
                {
                    const float staerke = (0.8f + 2.6f * anteil) * (static_cast<float>(stoss) / 50.0f);   // degrees
                    punchP += staerke; punchY += ((t * 2654435761u) & 1u ? 1.0f : -1.0f) * staerke * 0.6f;
                }
                ::ps2xGsBridgeOption(19, 1 + static_cast<int>(anteil * 99.0f));
                if (h <= 0.0f) { ::ps2xGsBridgeOption(20, ++tode); g_ts2Serie = 0; }
            }
            spielerLetzt = h; spielerLetztAdr = sp;
            if (rueckstoss > 0 && feuer && (!feuerVor || t - letzterPunch >= 7) && ::ps2xGsBridgeOption(10, 0) == 0)
            {
                punchP += 0.55f * static_cast<float>(rueckstoss) / 40.0f; letzterPunch = t;
            }
        }
        feuerVor = feuer;
        // target offset = punch (decays fast); the difference to what is already applied goes into the view angles
        punchP *= 0.72f; punchY *= 0.72f;
        if (std::fabs(punchP) < 0.005f) punchP = 0.0f;
        if (std::fabs(punchY) < 0.005f) punchY = 0.0f;
        // the applied offset belongs to one player object: a new object (respawn, level) starts from zero. A short
        // signature miss during a hit must NOT reset it (that left the offset in the view for good -> view sank down)
        static uint32_t punchObj = 0;
        if (spObj != punchObj) { punchObj = spObj; angewP = angewY = punchP = punchY = 0.0f; }
        if (spObj && (punchP != angewP || punchY != angewY))
        {
            const float yaw = rf(r, spObj + 0x148), pitch = rf(r, spObj + 0x14C);
            if (std::isfinite(yaw) && std::isfinite(pitch))
            {
                wf(r, spObj + 0x14C, pitch + (punchP - angewP)); wf(r, spObj + 0x148, yaw + (punchY - angewY));   // positive pitch = looking up
                g_ts2PunchDP += punchP - angewP; g_ts2PunchDY += punchY - angewY;
                angewP = punchP; angewY = punchY;
            }
        }
        (void)kickPitch; (void)kickYaw;
    }
}
namespace {
    // TS2_PROBE="59D6B4,4D47A0,..;file": writes the 32-bit values at these addresses (hex) every VSync, like the PINE probe of the PCSX2 reference run
    void ts2Probe(const uint8_t *rdram, uint64_t tickNow)
    {
        // TS2_SPIELERDUMP=file (research for Online+): every 10 VBlanks the player object (pointer 0x59DC0C, 0x1000 bytes)
        // as binary records: u64 tick, u32 pointer, then the bytes (pointer 0 -> zeros)
        {
            static std::FILE *df = [] { const char *e = std::getenv("TS2_SPIELERDUMP"); return (e && *e) ? std::fopen(e, "wb") : nullptr; }();
            if (df && tickNow % 10u == 0u)
            {
                uint32_t ptr = 0; std::memcpy(&ptr, rdram + 0x59DC0Cu, 4); ptr &= 0x1FFFFFFu;
                static uint8_t buf[0x1000]; std::memset(buf, 0, sizeof(buf));
                if (ptr && ptr + sizeof(buf) <= 0x2000000u) std::memcpy(buf, rdram + ptr, sizeof(buf));
                const uint64_t t = tickNow; std::fwrite(&t, 8, 1, df); std::fwrite(&ptr, 4, 1, df); std::fwrite(buf, 1, sizeof(buf), df); std::fflush(df);
            }
        }
        static std::vector<uint32_t> adr; static std::FILE *pf = nullptr; static bool init = false;
        if (!init) { init = true; if (const char *e = std::getenv("TS2_PROBE")) { std::string q(e); const size_t sc = q.find(';');
            if (sc != std::string::npos) { pf = std::fopen(q.c_str() + sc + 1, "w"); size_t p = 0;
                while (p < sc) { adr.push_back(static_cast<uint32_t>(std::strtoul(q.c_str() + p, nullptr, 16)) & 0x1FFFFFCu); p = q.find(',', p); if (p == std::string::npos || p > sc) break; ++p; } } } }
        if (!pf) return;
        std::fprintf(pf, "%llu", (unsigned long long)tickNow);
        for (uint32_t a2 : adr) { uint32_t v = 0; std::memcpy(&v, rdram + a2, 4); std::fprintf(pf, " %08X", v); }
        std::fputc('\n', pf); std::fflush(pf);
    }
}
namespace { void sotcRamDumpCheck(const uint8_t *r, uint64_t vs); void sotcRamDump(const uint8_t *r, uint64_t vs, const char *tag); }

namespace {
inline const char *sotcDbgDir()
{
    static const char *d = [] { const char *e = std::getenv("SOTC_DEBUG_DIR"); return (e && *e) ? e : "/tmp"; }();
    return d;
}
inline std::string sotcDbgFile(const char *name) { return std::string(sotcDbgDir()) + "/" + name; }
}

extern std::atomic<bool> g_ps2xSkipDraws;
extern std::atomic<uint64_t> g_ps2xPrimCount[8];
extern std::atomic<uint64_t> g_ps2xPrimTex;
extern std::atomic<bool> g_ps2xStateHist;
extern std::mutex g_ps2xStateHistMutex;
extern std::map<std::string, uint64_t> g_ps2xStateHistMap;
extern std::vector<std::string> g_ps2xStateHistOrder;
extern std::atomic<uint64_t> g_ps2xFogHist[256];
extern std::atomic<uint32_t> g_ps2xFogColor;
extern std::atomic<uint64_t> g_ps2xZMin, g_ps2xZMax;
extern std::mutex g_ps2xVtxSampleMutex;
extern std::vector<std::string> g_ps2xVtxSamples;
extern std::atomic<int> g_ps2xVu1DumpArm;
extern std::mutex g_ps2xVu1ProgMutex;
extern std::map<uint32_t, uint64_t> g_ps2xVu1ProgCount;
extern std::mutex g_ps2xCallHistMutex;
extern std::unordered_map<uint32_t, uint64_t> g_ps2xCallHist;

// VU0 random register R: exactly one hardware register on the PS2, not saved per thread (PCSX2: VU0.VI[REG_R]).
// The generated VRNEXT/VRINIT/VRXOR sites (patch_026) read/write this global register.
uint32_t g_ps2xVu0R = 0x3F800000u;

// SOTC_PROFILE=<us>: built-in sampling profiler (SIGPROF on the CPU time of all threads). Stores the interrupted
// instruction address (RIP) as an offset from the load address + thread ID. At exit -> $SOTC_DEBUG_DIR/profil_roh.txt,
// resolved offline with nm (tools/profil_aufloesen.py). Replaces perf, which is not installed on the PC.
#if defined(__linux__)
#include <ucontext.h>
#include <sys/time.h>
#include <link.h>
#endif

// GS bridge hooks (lib/gs/gs_frontend.cpp)
void ps2xGsBridgeRumble(int smallOn, int large);
int ps2xGsBridgeWideMode();
int ps2xGsBridgeAspectMilli();
void ps2xGsBridgeGameWide(int wide);
#ifdef SOTC_PGO_GEN
extern "C" void __gcov_dump(void); // only in the PGO measuring build (libgcov)
#endif
#if defined(__linux__)
namespace sotc_prof
{
    constexpr size_t kMax = 1u << 22;
    static uint64_t *g_rip = nullptr; static uint32_t *g_tid = nullptr;
    static std::atomic<size_t> g_n{0}; static uintptr_t g_base = 0; static bool g_on = false;
    static void handler(int, siginfo_t *, void *uc)
    {
        const size_t i = g_n.fetch_add(1, std::memory_order_relaxed);
        if (i >= kMax) return;
        g_rip[i] = static_cast<uint64_t>(static_cast<ucontext_t *>(uc)->uc_mcontext.gregs[REG_RIP]);
        g_tid[i] = static_cast<uint32_t>(syscall(SYS_gettid));
    }
    static int phdrCb(struct dl_phdr_info *info, size_t, void *) { if (!g_base) g_base = info->dlpi_addr; return 1; }
    inline void dump(bool stop, const char *name);
    inline void start()
    {
        const char *e = std::getenv("SOTC_PROFILE"); if (!e || !*e) return;
        const long us = std::max(200L, std::strtol(e, nullptr, 10));
        g_rip = static_cast<uint64_t *>(std::calloc(kMax, 8)); g_tid = static_cast<uint32_t *>(std::calloc(kMax, 4));
        dl_iterate_phdr(phdrCb, nullptr);
        struct sigaction sa{}; sa.sa_sigaction = handler; sa.sa_flags = SA_SIGINFO | SA_RESTART; sigemptyset(&sa.sa_mask);
        sigaction(SIGPROF, &sa, nullptr);
        itimerval tv{}; tv.it_interval.tv_usec = us; tv.it_value.tv_usec = us; setitimer(ITIMER_PROF, &tv, nullptr);
        g_on = true; std::fprintf(stderr, "[profile] on, every %ld us CPU time, base 0x%lx\n", us, (unsigned long)g_base);
        // SOTC_PROFILE_MINUTEN=1 (while playing): a cumulative snapshot profil_mNNN.txt every minute; the difference
        // between two snapshots is that minute's profile (run tools/profil_aufloesen.py on the difference)
        if (const char *m = std::getenv("SOTC_PROFILE_MINUTEN"); m && *m == '1')
            std::thread([] { for (unsigned k = 1;; ++k) { std::this_thread::sleep_for(std::chrono::seconds(60)); char nm[32]; std::snprintf(nm, sizeof(nm), "profil_m%03u.txt", k); dump(false, nm); } }).detach();
    }
    // stop=false: snapshot (profiler keeps running), name = file name in the output directory
    inline void dump(bool stop = true, const char *name = "profil_roh.txt");
    inline void dump(bool stop, const char *name)
    {
        if (!g_on) return;
        if (stop) { itimerval tv{}; setitimer(ITIMER_PROF, &tv, nullptr); }
        const size_t n = std::min(g_n.load(), kMax);
        const char *dd = std::getenv("SOTC_PROFILE_DIR"); if (!dd || !*dd) dd = std::getenv("SOTC_DEBUG_DIR");
        std::string path = std::string(dd ? dd : ".") + "/" + name;
        if (FILE *f = std::fopen(path.c_str(), "w"))
        {
            std::fprintf(f, "# base 0x%lx samples %zu\n", (unsigned long)g_base, n);
            // Executable mappings (libraries), so that samples outside the binary can be attributed
            if (FILE *m = std::fopen("/proc/self/maps", "r"))
            {
                char line[1024];
                while (std::fgets(line, sizeof(line), m))
                {
                    unsigned long lo = 0, hi = 0, off = 0; char perm[8] = {0}; char pathbuf[700] = {0};
                    if (std::sscanf(line, "%lx-%lx %7s %lx %*s %*s %699[^\n]", &lo, &hi, perm, &off, pathbuf) >= 4 && perm[2] == 'x')
                        std::fprintf(f, "# map %lx %lx %lx %s\n", lo - (unsigned long)g_base, hi - (unsigned long)g_base, off, pathbuf[0] ? pathbuf : "[anon]");
                }
                std::fclose(m);
            }
            std::unordered_map<uint64_t, uint32_t> h; h.reserve(1 << 16);
            for (size_t i = 0; i < n; ++i) ++h[((g_rip[i] - g_base) << 20) ^ (g_tid[i] & 0xFFFFFu)];
            for (const auto &[k, c] : h) std::fprintf(f, "%lx %u %u\n", (unsigned long)(k >> 20), (unsigned)(k & 0xFFFFFu), c);
            std::fclose(f);
            std::fprintf(stderr, "[profile] %zu samples -> %s\n", n, path.c_str());
        }
    }
}
#else
namespace sotc_prof { inline void start() {} inline void dump(bool = true, const char * = nullptr) {} } // Windows: no SIGPROF
#endif
bool ps2xGsBridgeScreenshot(const char *path);   // gs_frontend.cpp (PCSX2 GS bridge)
namespace sotc_vif_trace { void dump(); }

namespace
{
    constexpr uint32_t kRamMask = 0x01FFFFFFu;

    inline uint32_t arg(R5900Context *ctx, int reg) { return GPR_U32(ctx, reg); }
    inline void ret(R5900Context *ctx, int32_t v) { SET_GPR_S32(ctx, 2, v); }

    // Return to the caller like a leaf function: the generated dispatcher expects pc = $ra.
    inline void leave(R5900Context *ctx) { ctx->pc = GPR_U32(ctx, 31); }

    // --- Debug console (DECI2 / TTY) -----------------------------------------------------
    // The SDK routes printf through sceTtyWrite -> DECI2, which waits forever for a dev-kit
    // host. On a retail-like system we just print to stdout.
    void sotc_sceTtyWrite(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
    {
        const uint32_t buf = arg(ctx, 5), len = arg(ctx, 6);
        std::string s;
        s.reserve(len);
        for (uint32_t i = 0; i < len && i < 4096; ++i)
            s.push_back(static_cast<char>(rdram[(buf + i) & kRamMask]));
        std::fwrite("[SotC] ", 1, 7, stdout);
        std::fwrite(s.data(), 1, s.size(), stdout);
        if (s.empty() || s.back() != '\n') std::fputc('\n', stdout);
        std::fflush(stdout);
        ret(ctx, static_cast<int32_t>(len));
        leave(ctx);
    }
    void sotc_ret0(uint8_t *, R5900Context *ctx, PS2Runtime *) { ret(ctx, 0); leave(ctx); }
    void sotc_ret1(uint8_t *, R5900Context *ctx, PS2Runtime *) { ret(ctx, 1); leave(ctx); }
    void sotc_retm1(uint8_t *, R5900Context *ctx, PS2Runtime *) { ret(ctx, -1); leave(ctx); }

    // --- CD file table ---------------------------------------------------------------------
    // The PAL data archive spans NICO.DAT + XAB..XAE, which sit back to back on the disc; the
    // game computes sector numbers across all five. The runtime's own sceCdSearchFile hands
    // out pseudo LSNs per file, so we answer with the real LSNs of the disc and let sceCdRead
    // read them from the ISO (PS2Runtime IoPaths::cdImage).
    struct CdEntry { const char *name; uint32_t lsn; uint32_t size; };
    constexpr CdEntry kCdFiles[] = {
        {"SYSTEM.CNF", 293, 56},           {"SCES_533.26", 294, 229888},
        {"IOPRP300.IMG", 407, 275345},     {"MODULES\\SIO2MAN.IRX", 542, 6641},
        {"MODULES\\DBCMAN.IRX", 546, 15653}, {"MODULES\\SIO2D.IRX", 554, 11289},
        {"MODULES\\DS1O_D.IRX", 560, 10469}, {"STARTUP.XFF", 566, 26},
        {"KERNEL.XFF", 567, 419668},       {"MODULES\\LIBSD.IRX", 772, 30085},
        {"SG2IOPM1.IRX", 787, 27717},      {"MODULES\\MC2_D.IRX", 801, 20269},
        {"MANAGER.XFF", 811, 1732264},     {"GAMECORE.XFF", 1657, 2157144},
        {"MODULES2\\MCMAN.IRX", 2711, 96181}, {"MODULES2\\MCSERV.IRX", 2758, 7385},
        {"NICO.DAT", 2762, 1073741824u},   {"XAB", 527050, 1073741824u},
        {"XAC", 1051338, 1073741824u},     {"XAD", 1575626, 1073741824u},
        {"XAE", 2099914, 314316264u},
    };

    std::string normCdName(std::string n)
    {
        for (auto &c : n) c = (c == '/') ? '\\' : static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        const auto colon = n.find(':');
        if (colon != std::string::npos) n = n.substr(colon + 1);
        while (!n.empty() && n.front() == '\\') n.erase(0, 1);
        const auto semi = n.find(';');
        if (semi != std::string::npos) n.resize(semi);
        while (!n.empty() && n.back() == '.') n.pop_back();
        return n;
    }

    // int sceCdSearchFile(sceCdlFILE *fp, const char *name);  sceCdlFILE = {u32 lsn, u32 size, char name[16], u8 date[8]}
    void sotc_sceCdSearchFile(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
    {
        const uint32_t fp = arg(ctx, 4), namePtr = arg(ctx, 5);
        std::string name;
        for (uint32_t i = 0; i < 255; ++i)
        {
            const char c = static_cast<char>(rdram[(namePtr + i) & kRamMask]);
            if (!c) break;
            name.push_back(c);
        }
        const std::string key = normCdName(name);
        for (const auto &e : kCdFiles)
        {
            if (key == e.name)
            {
                uint8_t *out = rdram + (fp & kRamMask);
                std::memset(out, 0, 32);
                std::memcpy(out + 0, &e.lsn, 4);
                std::memcpy(out + 4, &e.size, 4);
                const std::string leaf = key.substr(key.rfind('\\') == std::string::npos ? 0 : key.rfind('\\') + 1);
                std::strncpy(reinterpret_cast<char *>(out + 8), leaf.c_str(), 15);
                std::fprintf(stderr, "[SotC] sceCdSearchFile %s -> lsn %u size %u\n", name.c_str(), e.lsn, e.size);
                ret(ctx, 1);
                leave(ctx);
                return;
            }
        }
        std::fprintf(stderr, "[SotC] sceCdSearchFile %s -> not found\n", name.c_str());
        ret(ctx, 0);
        leave(ctx);
    }

    // Use the original disc image for sector reads: $SOTC_ISO, else the first *.iso next to the
    // boot ELF or one directory above it.
    // File access to the disc image (up to 4.7 GB): on Windows long is 32 bits -> 64-bit seek and Unicode path
    std::FILE *ts2Oeffnen(const std::filesystem::path &p)
    {
#ifdef _WIN32
        return ::_wfopen(p.c_str(), L"rb");
#else
        return std::fopen(p.c_str(), "rb");
#endif
    }
    bool ts2Seek(std::FILE *f, uint64_t off)
    {
#ifdef _WIN32
        return ::_fseeki64(f, static_cast<__int64>(off), SEEK_SET) == 0;
#else
        return fseeko(f, static_cast<off_t>(off), SEEK_SET) == 0;
#endif
    }
    void configureIso()
    {
        PS2Runtime::IoPaths paths = PS2Runtime::getIoPaths();
        std::filesystem::path iso;
        if (const char *env = std::getenv("TS2_ISO"); env && *env) iso = env;
        else if (const char *env2 = std::getenv("SOTC_ISO"); env2 && *env2) iso = env2;
        for (const auto &dir : {paths.elfDirectory, paths.elfDirectory.parent_path()})
        {
            if (!iso.empty() || dir.empty()) break;
            std::error_code ec;
            for (const auto &de : std::filesystem::directory_iterator(dir, ec))
            {
                auto ext = de.path().extension().string();
                for (auto &c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                if (ext == ".iso") { iso = de.path(); break; }
            }
        }
        if (iso.empty())
        {
            std::fprintf(stderr, "[SotC override] WARNING: no ISO found (set SOTC_ISO); sector reads will fail\n");
            return;
        }
        paths.cdImage = iso;
        PS2Runtime::setIoPaths(paths);
        std::fprintf(stderr, "[SotC override] CD image: %s\n", iso.string().c_str());
    }

    extern std::atomic<uint32_t> g_wakeups, g_iwakeups;
    std::atomic<uint32_t> g_vblankHits{0}, g_gsIrqHits{0};
    // --- lock-delta tracing (SOTC_LOCKDELTA=0xFUNC,0xFUNC,...): net change of the KERNEL
    //     object-system lock count (0x1DC9D0) across each call of the listed functions.
    struct DeltaSlot { uint32_t addr = 0; PS2Runtime::RecompiledFunction orig = nullptr; std::atomic<int64_t> delta{0}; std::atomic<uint32_t> calls{0}; };
    DeltaSlot g_deltaSlots[32];
    inline int32_t lockCount(uint8_t *r) { int32_t v; std::memcpy(&v, r + 0x1DC9D0u, 4); return v; }
    template <int I>
    void deltaWrapper(uint8_t *r, R5900Context *c, PS2Runtime *rt)
    {
        const int32_t before = lockCount(r);
        g_deltaSlots[I].orig(r, c, rt);
        g_deltaSlots[I].delta += lockCount(r) - before;
        ++g_deltaSlots[I].calls;
    }
    template <int... Is>
    constexpr std::array<PS2Runtime::RecompiledFunction, sizeof...(Is)> makeDeltaWrappers(std::integer_sequence<int, Is...>)
    { return {deltaWrapper<Is>...}; }

    // --- module placement report: after KERNEL's isysLaunchModule relocated a module, print
    //     where its sections ended up (compare with docs/placement.json). Always on.
    PS2Runtime::RecompiledFunction g_origLaunchModule = nullptr;
    void reportModule(uint8_t *r, uint32_t hdr)
    {
        auto rd = [&](uint32_t a) { uint32_t v; std::memcpy(&v, r + (a & kRamMask), 4); return v; };
        hdr &= kRamMask;
        const uint32_t nsec = rd(hdr + 64), sectab = rd(hdr + 48) & kRamMask, names = rd(hdr + 68) & kRamMask, shstr = rd(hdr + 72) & kRamMask;
        const uint32_t nsym = rd(hdr + 36), symtab = rd(hdr + 40) & kRamMask;
        std::fprintf(stderr, "[SotC placement] module header 0x%08X sections:", hdr);
        int bssIdx = -1;
        for (uint32_t i = 0; i < nsec && i < 64; ++i)
        {
            const char *nm = reinterpret_cast<const char *>(r + ((shstr + rd(names + 4 * i)) & kRamMask));
            const uint32_t ptr = rd(sectab + 0x20 * i + 4) & kRamMask, size = rd(sectab + 0x20 * i + 8);
            if (std::strcmp(nm, ".bss") == 0) bssIdx = static_cast<int>(i);
            if (size && (std::strcmp(nm, ".text") == 0 || std::strcmp(nm, ".data") == 0 || std::strcmp(nm, ".rodata") == 0 ||
                         std::strcmp(nm, ".vutext") == 0))
                std::fprintf(stderr, " %s=0x%08X(0x%X)", nm, ptr, size);
        }
        // .bss is not in the section table: report relocated values of the first bss symbols
        int shown = 0;
        for (uint32_t i = 0; i < nsym && shown < 2; ++i)
        {
            const uint32_t value = rd(symtab + 16 * i + 4);
            uint16_t shndx; std::memcpy(&shndx, r + ((symtab + 16 * i + 14) & kRamMask), 2);
            if (bssIdx >= 0 && shndx == static_cast<uint16_t>(bssIdx))
            { std::fprintf(stderr, " bsssym#%u=0x%08X", i, value & kRamMask); ++shown; }
        }
        std::fprintf(stderr, "\n");
    }

    // lock-balance tracing for the KERNEL object system (SOTC_TRACE_OBJLOCK=1)
    std::mutex g_objLockMutex;
    std::map<uint32_t, uint32_t> g_objWaitByRa, g_objSigByRa;
    // --- Debug watchdog (SOTC_WATCHDOG=<seconds>): periodically dump guest thread states ---
#if defined(__linux__)
    // ---- SOTC_HWWATCH: in-process hardware write watchpoint (perf_event breakpoint) on one guest word ----
    int g_hwWatchFd = -1;
    volatile uint32_t *g_hwWatchHost = nullptr;
    volatile uint32_t g_hwWatchGuest = 0;
    volatile int g_hwWatchHits = 0;
    volatile uint32_t *g_hwFixedHost[4] = {};
    uint32_t g_hwFixedGuest[4] = {};
    uint32_t g_hwFixedLast[4] = {};
    void hwWatchSignal(int sig, siginfo_t *info, void *uctx)
    {
        // fixed watches: report every change (with value) regardless of filter
        for (int k = 0; k < 4; ++k)
        {
            if (!g_hwFixedHost[k]) continue;
            const uint32_t v = *g_hwFixedHost[k];
            if (v == g_hwFixedLast[k]) continue;
            g_hwFixedLast[k] = v;
            if (g_hwWatchHits >= 4000) return;
            ++g_hwWatchHits;
            char head[128];
            const int n = std::snprintf(head, sizeof(head), "[SotC HWFIX] guest 0x%08X <- 0x%08X bt:", g_hwFixedGuest[k], v);
            (void)!write(2, head, static_cast<size_t>(n));
            void *frames[8];
            const int depth = backtrace(frames, 8);
            char line[256];
            int len = 0;
            for (int f = 2; f < depth; ++f)
            {
                Dl_info dinfo{};
                const uintptr_t base = (dladdr(frames[f], &dinfo) && dinfo.dli_fbase) ? reinterpret_cast<uintptr_t>(dinfo.dli_fbase) : 0u;
                len += std::snprintf(line + len, sizeof(line) - static_cast<size_t>(len), " +0x%lx", static_cast<unsigned long>(reinterpret_cast<uintptr_t>(frames[f]) - base));
            }
            line[len++] = '\n';
            (void)!write(2, line, static_cast<size_t>(len));
        }
        (void)sig; (void)info; (void)uctx;
        if (!g_hwWatchHost || g_hwWatchHits >= 2000)
            return;
        const uint32_t v = *g_hwWatchHost;
        // only interesting: values that are neither 0 nor a plausible code address
        if (v == 0u || (v >= 0x00100000u && v < 0x01600000u && (v & 3u) == 0u))
            return;
        ++g_hwWatchHits;
        char head[128];
        const int n = std::snprintf(head, sizeof(head), "[SotC HWWATCH] guest 0x%08X <- 0x%08X bt:", g_hwWatchGuest, v);
        (void)!write(2, head, static_cast<size_t>(n));
        void *frames[8];
        const int depth = backtrace(frames, 8);
        char line[256];
        int len = 0;
        for (int k = 2; k < depth; ++k)
        {
            Dl_info info{};
            const uintptr_t base = (dladdr(frames[k], &info) && info.dli_fbase) ? reinterpret_cast<uintptr_t>(info.dli_fbase) : 0u;
            len += std::snprintf(line + len, sizeof(line) - static_cast<size_t>(len), " +0x%lx", static_cast<unsigned long>(reinterpret_cast<uintptr_t>(frames[k]) - base));
        }
        line[len++] = '\n';
        (void)!write(2, line, static_cast<size_t>(len));
    }
    void hwWatchArmFixed(uint8_t *rdram, const char *list)
    {
        struct sigaction sa{};
        sa.sa_sigaction = hwWatchSignal;
        sa.sa_flags = SA_SIGINFO | SA_RESTART;
        sigemptyset(&sa.sa_mask);
        sigaction(SIGIO, &sa, nullptr);
        int k = 0;
        for (const char *p = list; *p && k < 4;)
        {
            char *end = nullptr;
            const uint32_t guest = static_cast<uint32_t>(std::strtoul(p, &end, 0));
            p = (*end == ',') ? end + 1 : end;
            struct perf_event_attr attr{};
            attr.type = PERF_TYPE_BREAKPOINT;
            attr.size = sizeof(attr);
            attr.bp_type = HW_BREAKPOINT_W;
            attr.bp_addr = reinterpret_cast<uint64_t>(rdram + (guest & kRamMask));
            attr.bp_len = HW_BREAKPOINT_LEN_4;
            attr.sample_period = 1;
            attr.wakeup_events = 1;
            attr.exclude_kernel = 1;
            attr.exclude_hv = 1;
            const int fd = static_cast<int>(syscall(__NR_perf_event_open, &attr, 0, -1, -1, 0));
            if (fd < 0) { std::fprintf(stderr, "[SotC HWFIX] perf_event_open failed: %s\n", std::strerror(errno)); return; }
            fcntl(fd, F_SETFL, O_ASYNC);
            struct f_owner_ex owner{F_OWNER_TID, static_cast<pid_t>(syscall(SYS_gettid))};
            fcntl(fd, F_SETOWN_EX, &owner);
            fcntl(fd, F_SETSIG, SIGIO);
            g_hwFixedHost[k] = reinterpret_cast<volatile uint32_t *>(rdram + (guest & kRamMask));
            g_hwFixedGuest[k] = guest;
            g_hwFixedLast[k] = *g_hwFixedHost[k];
            ioctl(fd, PERF_EVENT_IOC_RESET, 0);
            ioctl(fd, PERF_EVENT_IOC_ENABLE, 0);
            std::fprintf(stderr, "[SotC HWFIX] armed 0x%08X\n", guest);
            ++k;
            if (end == p && *p) break;
        }
    }

    void hwWatchArm(uint8_t *rdram, uint32_t guest)
    {
        static bool init = false;
        if (!init)
        {
            init = true;
            struct sigaction sa{};
            sa.sa_sigaction = hwWatchSignal;
            sa.sa_flags = SA_SIGINFO | SA_RESTART;
            sigemptyset(&sa.sa_mask);
            sigaction(SIGIO, &sa, nullptr);
        }
        if (g_hwWatchFd >= 0) { close(g_hwWatchFd); g_hwWatchFd = -1; }
        struct perf_event_attr attr{};
        attr.type = PERF_TYPE_BREAKPOINT;
        attr.size = sizeof(attr);
        attr.bp_type = HW_BREAKPOINT_W;
        attr.bp_addr = reinterpret_cast<uint64_t>(rdram + (guest & kRamMask));
        attr.bp_len = HW_BREAKPOINT_LEN_4;
        attr.sample_period = 1;
        attr.wakeup_events = 1;
        attr.exclude_kernel = 1;
        attr.exclude_hv = 1;
        const int fd = static_cast<int>(syscall(__NR_perf_event_open, &attr, 0, -1, -1, 0));
        if (fd < 0)
        {
            static bool warned = false;
            if (!warned) { warned = true; std::fprintf(stderr, "[SotC HWWATCH] perf_event_open failed: %s\n", std::strerror(errno)); }
            return;
        }
        fcntl(fd, F_SETFL, O_ASYNC);
        struct f_owner_ex owner{F_OWNER_TID, static_cast<pid_t>(syscall(SYS_gettid))};
        fcntl(fd, F_SETOWN_EX, &owner);
        fcntl(fd, F_SETSIG, SIGIO);
        {
            static uint32_t lastArm = 0; static int armLogs = 0;
            if (guest != lastArm && armLogs < 60) { ++armLogs; lastArm = guest; std::fprintf(stderr, "[SotC HWWATCH] arm 0x%08X\n", guest); }
        }
        g_hwWatchHost = reinterpret_cast<volatile uint32_t *>(rdram + (guest & kRamMask));
        g_hwWatchGuest = guest;
        g_hwWatchFd = fd;
        ioctl(fd, PERF_EVENT_IOC_RESET, 0);
        ioctl(fd, PERF_EVENT_IOC_ENABLE, 0);
    }

#endif // __linux__ (HWWATCH)

    // SOTC_MEMWATCH=0xADDR[,0xADDR]: poll guest words every ~200us and report changes with the running thread.
    void startMemWatch(PS2Runtime &runtime)
    {
        const char *mw = std::getenv("SOTC_MEMWATCH");
        if (!mw || !*mw)
            return;
        std::vector<uint32_t> addrs;
        for (const char *p = mw; *p;)
        {
            char *end = nullptr;
            addrs.push_back(static_cast<uint32_t>(std::strtoul(p, &end, 0)));
            p = (*end == ',') ? end + 1 : end;
            if (end == p && *p != 0) break;
        }
        PS2Runtime *rt = &runtime;
        std::thread([rt, addrs]() {
            const uint8_t *ram = nullptr;
            while (!(ram = rt->memory().getRDRAM()))
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            std::vector<uint32_t> last(addrs.size(), 0u);
            for (size_t i = 0; i < addrs.size(); ++i) std::memcpy(&last[i], ram + (addrs[i] & kRamMask), 4);
            int reports = 0;
            while (reports < 200)
            {
                for (size_t i = 0; i < addrs.size(); ++i)
                {
                    uint32_t v; std::memcpy(&v, ram + (addrs[i] & kRamMask), 4);
                    if (v != last[i])
                    {
                        const EeKernelSnapshot snap = rt->eeScheduler().snapshot();
                        int pc = 0;
                        for (const auto &t : snap.threads) if (t.id == snap.runningThreadId) pc = (int)t.pc;
                        std::fprintf(stderr, "[SotC memwatch] 0x%08X: %08X -> %08X (running thread %d, last pc 0x%08X, vsync %llu)\n",
                                     addrs[i], last[i], v, snap.runningThreadId, pc, (unsigned long long)rt->memory().gs().vsyncTick.load());
                        last[i] = v; ++reports;
                    }
                }
                std::this_thread::sleep_for(std::chrono::microseconds(200));
            }
        }).detach();
    }

#if defined(__linux__)
    // SOTC_WPROT=0xADDR: write-protect the host page holding this guest address and print a host
    // backtrace on the first write into [addr, addr+0x40). Used to find stray DMA/HLE writes.
    uintptr_t g_wprotPage = 0, g_wprotLo = 0, g_wprotHi = 0;
    std::atomic<int> g_wprotHits{0};
    void wprotHandler(int, siginfo_t *si, void *)
    {
        const uintptr_t a = reinterpret_cast<uintptr_t>(si->si_addr);
        const long ps = sysconf(_SC_PAGESIZE);
        if (a < g_wprotPage || a >= g_wprotPage + ps)
        {
            signal(SIGSEGV, SIG_DFL);
            return;
        }
        mprotect(reinterpret_cast<void *>(g_wprotPage), ps, PROT_READ | PROT_WRITE);
        if (a >= g_wprotLo && a < g_wprotHi && g_wprotHits.fetch_add(1) < 40)
        {
            static const char msg[] = "[SotC wprot] write into watched range, backtrace:\n";
            write(2, msg, sizeof(msg) - 1);
            void *bt[48];
            const int n = backtrace(bt, 48);
            backtrace_symbols_fd(bt, n, 2);
        }
    }
    void startWriteProtect(PS2Runtime &runtime)
    {
        const char *w = std::getenv("SOTC_WPROT");
        if (!w || !*w)
            return;
        const uint32_t guest = static_cast<uint32_t>(std::strtoul(w, nullptr, 0));
        PS2Runtime *rt = &runtime;
        std::thread([rt, guest]() {
            uint8_t *ram = nullptr;
            while (!(ram = rt->memory().getRDRAM()))
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            std::this_thread::sleep_for(std::chrono::seconds(std::getenv("SOTC_WPROT_DELAY") ? std::atoi(std::getenv("SOTC_WPROT_DELAY")) : 20));
            const long ps = sysconf(_SC_PAGESIZE);
            g_wprotLo = reinterpret_cast<uintptr_t>(ram + guest);
            g_wprotHi = g_wprotLo + 0x10;
            g_wprotPage = g_wprotLo & ~static_cast<uintptr_t>(ps - 1);
            struct sigaction sa {};
            sa.sa_sigaction = wprotHandler;
            sa.sa_flags = SA_SIGINFO;
            sigaction(SIGSEGV, &sa, nullptr);
            std::fprintf(stderr, "[SotC wprot] watching guest 0x%08X (host %p), exe maps:\n", guest, (void *)g_wprotLo);
            if (FILE *m = std::fopen("/proc/self/maps", "r"))
            {
                char line[512];
                while (std::fgets(line, sizeof(line), m))
                    if (std::strstr(line, "ps2EntryRunner") && std::strstr(line, "r-xp")) std::fputs(line, stderr);
                std::fclose(m);
            }
            while (g_wprotHits.load() < 40)
            {
                mprotect(reinterpret_cast<void *>(g_wprotPage), ps, PROT_READ);
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
        }).detach();
    }

#else
    void startWriteProtect(PS2Runtime &) {} // Windows: no mprotect/SIGSEGV
#endif // __linux__ (WPROT)

    // ---- Test harness ----
    // SOTC_SHOT_TICKS=330,480,...  dump the render target (fbp 232, 512x416) at these vsync ticks
    // SOTC_SHOT_DIR=/path          where to put shot_<tick>.ppm (default /tmp)
    // SOTC_EXIT_TICK=N             print a summary line and exit at this vsync tick
    std::atomic<uint32_t> g_abortCount{0}, g_assertCount{0};
    void dumpFramePpm(PS2Runtime *rt, const std::string &path, unsigned fbp, unsigned fbw, unsigned psm, unsigned w, unsigned h)
    {
        // PCSX2 GS bridge active: save the picture the way PCSX2 outputs it (PNG next to the .ppm name)
        if (ps2xGsBridgeScreenshot((path.substr(0, path.size() - 4) + ".png").c_str()))
            return;
        if (FILE *f = std::fopen(path.c_str(), "wb"))
        {
            std::fprintf(f, "P6\n%u %u\n255\n", w, h);
            std::vector<unsigned char> row(w * 3u);
            // Consistent image: copy the whole VRAM at once (waits for the rasterizer with MTGS), then read from the
            // copy. Reading VRAM pixel by pixel would catch a half-redrawn frame with MTGS.
            rt->gs().refreshDisplaySnapshot();
            uint32_t snapSize = 0;
            uint8_t *snap = const_cast<uint8_t *>(rt->gs().lockDisplaySnapshot(snapSize));
            const bool useSnap = snap && psm == 0u && snapSize >= 4u * 1024u * 1024u;
            for (unsigned y = 0; y < h; ++y)
            {
                for (unsigned x = 0; x < w; ++x)
                {
                    const uint32_t c = useSnap ? GSMem::ReadCT32(snap, fbp * 32u, fbw, x, y) : rt->gs().ReadVram(psm, fbp * 32u, fbw, x, y);
                    row[x * 3u] = (unsigned char)c;
                    row[x * 3u + 1u] = (unsigned char)(c >> 8);
                    row[x * 3u + 2u] = (unsigned char)(c >> 16);
                }
                std::fwrite(row.data(), 1, row.size(), f);
            }
            rt->gs().unlockDisplaySnapshot();
            std::fclose(f);
        }
    }
    std::vector<uint64_t> g_shotTicks;
    size_t g_shotNext = 0;
    std::string g_shotDir = "/tmp";
    std::atomic<uint64_t> g_clipCalls[2], g_clipOut[2], g_clipNan[2], g_clipNanOut[2];
    int g_clipSamples = 0;
    std::mutex g_clipHistMutex;
    uint64_t g_clipHist[2][4] = {};
    std::mutex g_texTraceMutex;
    uint64_t g_texTraceCount = 0;
    int g_texTraceSamples = 0;
    uint64_t g_texTraceBytes = 0;
    std::map<uint32_t, uint64_t> g_texTraceByRa;
    uint64_t g_skipDrawUntil = 0;
    void (*g_subThreadReport)(uint64_t) = nullptr; // SOTC_TRACE_SUBTHREAD: report every 500 frames
    void (*g_oomReportNow)(uint64_t) = nullptr;    // SOTC_TRACE_OOM: report on a crash
    // Own walk through the partitions, modelled on iosDebugPartitionDump (0x1AB358 -> 0x1AA720), read-only:
    // Partition: +0x08 first block (words). Block header 0x20 bytes: +0x00 size (words), +0x08 next block,
    // +0x0C bit 24 used / bit 25 sub-partition (header at block+0x24), +0x18 caller PC (words).
    void sotcDumpPartition(const uint8_t *r, uint32_t part, int depth, const char *why)
    {
        if (depth > 6 || part >= 0x2000000u) return;
        struct Agg { uint64_t bytes = 0; uint32_t n = 0; };
        std::unordered_map<uint32_t, Agg> perPc;
        uint64_t used = 0, freeB = 0, largestFree = 0;
        uint32_t nUsed = 0, nFree = 0, nSub = 0;
        std::vector<uint32_t> subs;
        uint32_t b = (Ps2FastRead32(r, part + 0x8u) & 0xFFFFFFu) << 2;
        for (int guard = 0; b != 0u && b < 0x2000000u && guard < 400000; ++guard)
        {
            const uint32_t w0 = Ps2FastRead32(r, b), wC = Ps2FastRead32(r, b + 0xCu);
            const uint32_t size = (w0 & 0xFFFFFFu) << 2;
            if ((wC >> 24) & 1u)
            {
                used += size; ++nUsed;
                const uint32_t pc = (Ps2FastRead32(r, b + 0x18u) & 0xFFFFFFu) << 2;
                auto &a = perPc[pc]; a.bytes += size; ++a.n;
                if ((wC >> 25) & 1u) { ++nSub; subs.push_back(b + 0x24u); }
            }
            else { freeB += size; ++nFree; largestFree = std::max<uint64_t>(largestFree, size); }
            const uint32_t next = (Ps2FastRead32(r, b + 0x8u) & 0xFFFFFFu) << 2;
            if (next <= b) break;
            b = next;
        }
        std::vector<std::pair<uint32_t, Agg>> v(perPc.begin(), perPc.end());
        std::sort(v.begin(), v.end(), [](auto &x, auto &y) { return x.second.bytes > y.second.bytes; });
        std::fprintf(stderr, "[SotC ptn] %s %*sPartition 0x%08X: belegt %.2f MB/%u, frei %.2f MB/%u (groesster %u B), Unterpartitionen %u; Halter:",
                     why, depth * 2, "", part, used / 1048576.0, nUsed, freeB / 1048576.0, nFree, (unsigned)largestFree, nSub);
        for (size_t i = 0; i < v.size() && i < 8; ++i)
            std::fprintf(stderr, " pc=0x%08X:%.2fMB/%u", v[i].first, v[i].second.bytes / 1048576.0, v[i].second.n);
        std::fprintf(stderr, "\n");
        for (uint32_t sp : subs)
        {
            // print sub-partitions individually only from 256 KB up
            const uint32_t blk = sp - 0x24u;
            if (((Ps2FastRead32(r, blk) & 0xFFFFFFu) << 2) >= 0x40000u) sotcDumpPartition(r, sp, depth + 1, why);
        }
    }
    // SOTC_COUNT_FUNCS=addr,addr,... : count calls of arbitrary guest functions (load vs. unload).
    constexpr int kCountSlots = 32;
    PS2Runtime::RecompiledFunction g_countOrig[kCountSlots] = {};
    uint32_t g_countAddr[kCountSlots] = {};
    std::atomic<uint64_t> g_countHits[kCountSlots] = {};
    int g_countN = 0;
    // SOTC_CALLERS=1 in addition: a histogram of return addresses ($ra) per counted function -> who calls how often?
    bool g_countCallers = false;
    std::mutex g_callerMutex;
    std::unordered_map<uint32_t, uint64_t> g_callerHist[kCountSlots];
    template <int N>
    void countTrampoline(uint8_t *r, R5900Context *c, PS2Runtime *rt)
    {
        g_countHits[N].fetch_add(1, std::memory_order_relaxed);
        if (g_countCallers)
        {
            std::lock_guard<std::mutex> lk(g_callerMutex);
            ++g_callerHist[N][GPR_U32(c, 31)];
        }
        g_countOrig[N](r, c, rt);
    }
    template <int... I>
    constexpr std::array<PS2Runtime::RecompiledFunction, sizeof...(I)> makeCountTramps(std::integer_sequence<int, I...>)
    { return {&countTrampoline<I>...}; }
    const auto g_countTramps = makeCountTramps(std::make_integer_sequence<int, kCountSlots>{});
    uint64_t g_drawWindow = 0;
    void harnessOnVBlank(PS2Runtime *rt)
    {
        const uint64_t tick = rt->memory().gs().vsyncTick.load();
        if (g_subThreadReport && tick % 500u == 0u)
            g_subThreadReport(tick);
        if (g_countN > 0 && tick % 500u == 0u)
        {
            std::fprintf(stderr, "[SotC zaehler] vsync=%llu", (unsigned long long)tick);
            for (int i = 0; i < g_countN; ++i)
                std::fprintf(stderr, " 0x%X=%llu", g_countAddr[i], (unsigned long long)g_countHits[i].load());
            std::fprintf(stderr, "\n");
            if (g_countCallers)
            {
                std::lock_guard<std::mutex> lk(g_callerMutex);
                for (int i = 0; i < g_countN; ++i)
                {
                    if (g_callerHist[i].empty()) continue;
                    std::vector<std::pair<uint32_t, uint64_t>> v(g_callerHist[i].begin(), g_callerHist[i].end());
                    std::sort(v.begin(), v.end(), [](auto &a, auto &b) { return a.second > b.second; });
                    std::fprintf(stderr, "[SotC aufrufer] 0x%X:", g_countAddr[i]);
                    for (size_t k = 0; k < v.size() && k < 8; ++k) std::fprintf(stderr, " ra=0x%08X:%llu", v[k].first, (unsigned long long)v[k].second);
                    std::fprintf(stderr, "\n");
                }
            }
        }
        if (g_skipDrawUntil != 0u)
        {
            // Also open a draw window during the lead-in before each screenshot (otherwise all lead-in frames are black).
            const uint64_t win = g_drawWindow ? g_drawWindow : 20u;
            const bool nearShot = g_shotNext < g_shotTicks.size() && tick + win >= g_shotTicks[g_shotNext];
            const bool skip = tick < g_skipDrawUntil && !nearShot;
            if (nearShot && tick < g_skipDrawUntil)
                g_ps2xSkipDraws.store(false, std::memory_order_relaxed);
            else if (g_ps2xSkipDraws.load(std::memory_order_relaxed) != skip)
            {
                g_ps2xSkipDraws.store(skip, std::memory_order_relaxed);
                std::fprintf(stderr, "[SotC test] skip draws %s at tick=%llu\n", skip ? "on" : "off", (unsigned long long)tick);
            }
            if (tick >= g_skipDrawUntil)
                g_skipDrawUntil = 0;
        }
        // SOTC_DRAW_WINDOW=N: after the lead-in, draw only in the N frames before each screenshot.
        else if (g_drawWindow != 0u)
        {
            const bool near = g_shotNext < g_shotTicks.size() && tick + g_drawWindow >= g_shotTicks[g_shotNext];
            const bool skip = !near;
            if (g_ps2xSkipDraws.load(std::memory_order_relaxed) != skip)
                g_ps2xSkipDraws.store(skip, std::memory_order_relaxed);
        }
        // SOTC_STATE_HIST=<tick>: histogram of GS draw states for exactly one frame (tick .. tick+1), in draw order.
        static const uint64_t histTick = [] { const char *e = std::getenv("SOTC_STATE_HIST"); return e ? std::strtoull(e, nullptr, 10) : 0ull; }();
        if (histTick)
        {
            if (tick == histTick)
            {
                g_ps2xStateHist.store(true);
                if (const char *vd = std::getenv("SOTC_VU1_DUMP"); vd && *vd)
                    g_ps2xVu1DumpArm.store(std::atoi(vd));
            }
            else if (tick == histTick + 1u && g_ps2xStateHist.load())
            {
                g_ps2xStateHist.store(false);
                std::lock_guard<std::mutex> hl(g_ps2xStateHistMutex);
                std::fprintf(stderr, "[SotC STATES] frame tick=%llu distinct=%zu\n", (unsigned long long)histTick, g_ps2xStateHistOrder.size());
                if (std::getenv("SOTC_DUMP_LAYOUT"))
                    if (FILE *f = std::fopen(sotcDbgFile("sotc_ram.bin").c_str(), "wb")) { std::fwrite(rt->memory().getRDRAM(), 1, 0x2000000u, f); std::fclose(f); }
                for (const auto &k : g_ps2xStateHistOrder)
                    std::fprintf(stderr, "[SotC STATES] %6llu x %s\n", (unsigned long long)g_ps2xStateHistMap[k], k.c_str());
                {
                    std::lock_guard<std::mutex> cl(g_ps2xCallHistMutex);
                    if (FILE *f = std::fopen(sotcDbgFile("sotc_callhist.txt").c_str(), "w"))
                    {
                        for (const auto &[pc, n] : g_ps2xCallHist) std::fprintf(f, "%08x %llu\n", pc, (unsigned long long)n);
                        std::fclose(f);
                    }
                    std::fprintf(stderr, "[SotC STATES] call histogram: %zu distinct targets -> /tmp/sotc_callhist.txt\n", g_ps2xCallHist.size());
                }
                {
                    std::lock_guard<std::mutex> pl(g_ps2xVu1ProgMutex);
                    std::fprintf(stderr, "[SotC STATES] VU1 programs (start pc: runs):");
                    for (const auto &[pc, n] : g_ps2xVu1ProgCount) std::fprintf(stderr, " 0x%X:%llu", pc, (unsigned long long)n);
                    std::fprintf(stderr, "\n");
                }
                std::fprintf(stderr, "[SotC STATES] fogged tri vertices: fogcol=%06X z=[%llu..%llu] fog histogram (value:count):",
                             g_ps2xFogColor.load(), (unsigned long long)g_ps2xZMin.load(), (unsigned long long)g_ps2xZMax.load());
                for (int f = 0; f < 256; ++f)
                    if (const uint64_t n = g_ps2xFogHist[f].load()) std::fprintf(stderr, " %d:%llu", f, (unsigned long long)n);
                std::fprintf(stderr, "\n");
                {
                    std::lock_guard<std::mutex> sl(g_ps2xVtxSampleMutex);
                    for (const auto &line : g_ps2xVtxSamples)
                        std::fprintf(stderr, "[SotC STATES] tri %s\n", line.c_str());
                }
            }
        }
        static const bool kEnv_SOTC_TRACE_MODELCLIP = std::getenv("SOTC_TRACE_MODELCLIP") != nullptr;
        if (kEnv_SOTC_TRACE_MODELCLIP && tick % 50u == 0u)
        {
            std::lock_guard<std::mutex> lock(g_clipHistMutex);
            std::fprintf(stderr, "[SotC MODELCLIP] tick=%llu skin: <=-2:%llu (-2,0):%llu 0:%llu >0:%llu | solid: <=-2:%llu (-2,0):%llu 0:%llu >0:%llu\n",
                         (unsigned long long)tick,
                         (unsigned long long)g_clipHist[0][0], (unsigned long long)g_clipHist[0][1], (unsigned long long)g_clipHist[0][2], (unsigned long long)g_clipHist[0][3],
                         (unsigned long long)g_clipHist[1][0], (unsigned long long)g_clipHist[1][1], (unsigned long long)g_clipHist[1][2], (unsigned long long)g_clipHist[1][3]);
            std::memset(g_clipHist, 0, sizeof(g_clipHist));
        }
        static const bool tracePrims = std::getenv("SOTC_TRACE_PRIMS") != nullptr;
        if (tracePrims && tick % 25u == 0u)
        {
            uint64_t c[8];
            for (int k = 0; k < 8; ++k) c[k] = g_ps2xPrimCount[k].exchange(0);
            std::fprintf(stderr, "[SotC PRIMS] tick=%llu (25 frames) point=%llu line=%llu strip=%llu tri=%llu tristrip=%llu trifan=%llu sprite=%llu textured=%llu\n",
                         (unsigned long long)tick, (unsigned long long)c[0], (unsigned long long)(c[1] + c[2]), (unsigned long long)c[2],
                         (unsigned long long)c[3], (unsigned long long)c[4], (unsigned long long)c[5], (unsigned long long)c[6],
                         (unsigned long long)g_ps2xPrimTex.exchange(0));
        }
        if (g_shotNext >= g_shotTicks.size())
            return;
        while (g_shotNext < g_shotTicks.size() && tick >= g_shotTicks[g_shotNext])
        {
            char name[64];
            std::snprintf(name, sizeof(name), "/shot_%05llu.ppm", (unsigned long long)g_shotTicks[g_shotNext]);
            dumpFramePpm(rt, g_shotDir + name, 232, 8, 0, 512, 416);
            std::fprintf(stderr, "[SotC test] shot tick=%llu\n", (unsigned long long)g_shotTicks[g_shotNext]);
            ++g_shotNext;
        }
    }
    void startTestHarness(PS2Runtime &runtime)
    {
        if (const char *sk = std::getenv("SOTC_SKIP_DRAW_UNTIL"); sk && *sk)
        {
            g_skipDrawUntil = std::strtoull(sk, nullptr, 10);
            g_ps2xSkipDraws.store(g_skipDrawUntil != 0u);
        }
        if (const char *dw = std::getenv("SOTC_DRAW_WINDOW"); dw && *dw)
            g_drawWindow = std::strtoull(dw, nullptr, 10);
        const char *shots = std::getenv("SOTC_SHOT_TICKS");
        const char *exitTick = std::getenv("SOTC_EXIT_TICK");
        if ((!shots || !*shots) && (!exitTick || !*exitTick))
            return;
        std::vector<uint64_t> ticks;
        if (shots)
            for (const char *p = shots; *p;)
            {
                char *end = nullptr;
                ticks.push_back(std::strtoull(p, &end, 10));
                p = (*end == ',') ? end + 1 : end;
                if (end == p && *p) break;
            }
        std::sort(ticks.begin(), ticks.end());
        const uint64_t stopAt = exitTick ? std::strtoull(exitTick, nullptr, 10) : 0u;
        const std::string dir = std::getenv("SOTC_SHOT_DIR") ? std::getenv("SOTC_SHOT_DIR") : "/tmp";
        g_shotTicks = ticks;
        g_shotDir = dir;
        PS2Runtime *rt = &runtime;
        std::thread([rt, stopAt]() {
            const auto t0 = std::chrono::steady_clock::now();
            for (;;)
            {
                const uint64_t tick = rt->memory().gs().vsyncTick.load();
                if (stopAt && tick >= stopAt)
                {
                    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
                    std::fprintf(stderr, "[SotC test] done tick=%llu seconds=%.1f fps=%.2f aborts=%u asserts=%u\n",
                                 (unsigned long long)tick, secs, secs > 0 ? tick / secs : 0.0, g_abortCount.load(), g_assertCount.load());
                    sotc_prof::dump();
                    ::ps2xGsBridgeCaptureEnd();
                    std::fflush(stderr);
                    std::fflush(stdout);
                    #ifdef SOTC_PGO_GEN
                    __gcov_dump(); // PGO run: write the profile
#endif
                    std::_Exit(0);
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
        }).detach();
    }

#if defined(__linux__)
    // Hang report (always on, without gdb, also on the Steam Deck): if the displayed frame buffer (DISPFB) does not change
    // for a while (8 s, loading pauses are shorter) although VBlanks keep coming and the port menu is closed, the EE threads (status/wait/PC),
    // semaphores and the host stacks of all threads (resolved through the program's symbol table) are written to the log
    // once. After that only again after the next frame flip. TS2_HAENGERBERICHT=0 turns it off.
    namespace haenger
    {
        constexpr int kMaxTiefe = 40;
        struct Abzug { std::atomic<int> tid{0}; int n = 0; void *adr[kMaxTiefe]; std::atomic<bool> fertig{false}; };
        Abzug g_abzug[96];
        std::atomic<int> g_slot{-1};
        void sigHandler(int)
        {
            const int i = g_slot.load();
            if (i < 0 || i >= 96) return;
            g_abzug[i].n = backtrace(g_abzug[i].adr, kMaxTiefe);
            g_abzug[i].fertig.store(true);
        }
        struct Sym { uintptr_t a; uintptr_t g; std::string n; };
        std::vector<Sym> g_syms; uintptr_t g_basis = 0, g_symBasis = 0;
        void ladeSymbole()
        {
            Dl_info di{};
            if (dladdr(reinterpret_cast<void *>(&ladeSymbole), &di)) g_basis = reinterpret_cast<uintptr_t>(di.dli_fbase);
            std::FILE *f = std::fopen("/proc/self/exe", "rb");
            if (!f) return;
            std::vector<uint8_t> d; uint8_t buf[1 << 16]; size_t k;
            while ((k = std::fread(buf, 1, sizeof(buf), f)) > 0) d.insert(d.end(), buf, buf + k);
            std::fclose(f);
            if (d.size() < 64 || std::memcmp(d.data(), "\x7f" "ELF", 4) != 0 || d[4] != 2) return;
            // ET_EXEC (Zig package): symbol values are absolute addresses; ET_DYN (PIE): relative to the load address
            g_symBasis = (d[0x10] == 2) ? 0u : g_basis;
            auto u16 = [&](size_t o) { uint16_t v; std::memcpy(&v, d.data() + o, 2); return v; };
            auto u32 = [&](size_t o) { uint32_t v; std::memcpy(&v, d.data() + o, 4); return v; };
            auto u64 = [&](size_t o) { uint64_t v; std::memcpy(&v, d.data() + o, 8); return v; };
            const uint64_t shoff = u64(0x28); const uint16_t shentsize = u16(0x3A), shnum = u16(0x3C);
            for (uint16_t i = 0; i < shnum; ++i)
            {
                const size_t sh = shoff + size_t(i) * shentsize;
                if (sh + 64 > d.size() || u32(sh + 4) != 2) continue;   // SHT_SYMTAB
                const uint64_t off = u64(sh + 0x18), size = u64(sh + 0x20), ent = u64(sh + 0x38); const uint32_t link = u32(sh + 0x28);
                const size_t strsh = shoff + size_t(link) * shentsize; const uint64_t stroff = u64(strsh + 0x18);
                for (uint64_t o = off; ent && o + ent <= off + size && o + 24 <= d.size(); o += ent)
                {
                    if ((d[o + 4] & 0xF) != 2) continue;   // STT_FUNC
                    const uint64_t val = u64(o + 8), sz = u64(o + 16);
                    if (!val) continue;
                    const char *name = reinterpret_cast<const char *>(d.data() + stroff + u32(o));
                    g_syms.push_back({uintptr_t(val), uintptr_t(sz ? sz : 1), name});
                }
            }
            std::sort(g_syms.begin(), g_syms.end(), [](const Sym &a, const Sym &b) { return a.a < b.a; });
        }
        std::string aufloesen(void *p)
        {
            const uintptr_t a = reinterpret_cast<uintptr_t>(p) - g_symBasis;
            auto it = std::upper_bound(g_syms.begin(), g_syms.end(), a, [](uintptr_t v, const Sym &s) { return v < s.a; });
            char t[64];
            if (it != g_syms.begin()) { --it; if (a - it->a < (it->g > 1 ? it->g : uintptr_t(0x1000))) { std::snprintf(t, sizeof t, "+0x%lx", (unsigned long)(a - it->a)); return it->n + t; } }
            Dl_info di{};
            if (dladdr(p, &di) && di.dli_sname) { std::snprintf(t, sizeof t, "+0x%lx", (unsigned long)(reinterpret_cast<uintptr_t>(p) - reinterpret_cast<uintptr_t>(di.dli_saddr))); return std::string(di.dli_sname) + t; }
            if (g_basis && reinterpret_cast<uintptr_t>(p) >= g_basis && reinterpret_cast<uintptr_t>(p) - g_basis < (uintptr_t(1) << 31))
                std::snprintf(t, sizeof t, "exe+0x%lx", (unsigned long)(reinterpret_cast<uintptr_t>(p) - g_basis));
            else std::snprintf(t, sizeof t, "%p", p);
            return t;
        }
        void bericht(PS2Runtime *rt, uint64_t seitTick)
        {
            std::fprintf(stderr, "===== TS2 HANG REPORT: no new frame since VBlank %llu (now %llu) =====\n",
                         (unsigned long long)seitTick, (unsigned long long)rt->memory().gs().vsyncTick.load());
            const EeKernelSnapshot snap = rt->eeScheduler().snapshot();
            std::fprintf(stderr, "EE: running=%d cycle=%llu\n", snap.runningThreadId, (unsigned long long)snap.eeCycle);
            for (const auto &t : snap.threads)
                std::fprintf(stderr, "  EE-Thread %d status=%d warten=%d/%d prio=%d pc=0x%08X ra=0x%08X entry=0x%08X\n", t.id, int(t.status),
                             int(t.waitReason), t.waitId, t.currentPriority, t.pc, t.ra, t.entry);
            for (const auto &se : snap.semaphores)
                if (se.waiters) std::fprintf(stderr, "  Sema %d count=%d/%d wartende=%u\n", se.id, se.count, se.maxCount, se.waiters);
            if (g_syms.empty()) ladeSymbole();
            std::fprintf(stderr, "symbols: %zu (program base %p)\n", g_syms.size(), reinterpret_cast<void *>(g_basis));
            std::vector<int> tids;
            if (DIR *dir = opendir("/proc/self/task")) { while (dirent *e = readdir(dir)) if (e->d_name[0] != '.') tids.push_back(std::atoi(e->d_name)); closedir(dir); }
            const int self = int(syscall(SYS_gettid));
            int slot = 0;
            for (int tid : tids)
            {
                if (tid == self || slot >= 96) continue;
                char name[64] = "?"; { char pfad[64]; std::snprintf(pfad, sizeof pfad, "/proc/self/task/%d/comm", tid);
                  if (std::FILE *cf = std::fopen(pfad, "r")) { if (std::fgets(name, sizeof name, cf)) name[std::strcspn(name, "\n")] = 0; std::fclose(cf); } }
                Abzug &z = g_abzug[slot]; z.fertig = false; z.n = 0; g_slot.store(slot);
                if (syscall(SYS_tgkill, getpid(), tid, SIGRTMIN + 3) != 0) continue;
                for (int w = 0; w < 60 && !z.fertig.load(); ++w) std::this_thread::sleep_for(std::chrono::milliseconds(1));
                g_slot.store(-1);
                std::fprintf(stderr, "-- Host-Thread %d (%s)%s\n", tid, name, z.fertig.load() ? "" : " (no answer)");
                for (int i = 2; i < z.n; ++i) std::fprintf(stderr, "     #%d %s\n", i - 2, aufloesen(z.adr[i]).c_str());
                ++slot;
            }
            std::fprintf(stderr, "===== end of hang report (game keeps running) =====\n");
            std::fflush(stderr);
        }
        void start(PS2Runtime &runtime)
        {
            if (const char *e = std::getenv("TS2_HAENGERBERICHT"); e && *e == '0') return;
            { void *tmp[4]; backtrace(tmp, 4); }   // preload libgcc (backtrace inside the signal handler)
            struct sigaction sa{}; sa.sa_handler = sigHandler; sa.sa_flags = SA_RESTART; sigemptyset(&sa.sa_mask);
            sigaction(SIGRTMIN + 3, &sa, nullptr);
            PS2Runtime *rt = &runtime;
            std::thread([rt]() {
                uint64_t letzterFb = ~0ull, seitTick = 0; auto seit = std::chrono::steady_clock::now(); bool gemeldet = false;
                // TS2_HAENGER_TEST=<VBlank>: force the report once (tests the report itself)
                uint64_t testTick = 0; if (const char *e = std::getenv("TS2_HAENGER_TEST")) testTick = std::strtoull(e, nullptr, 10);
                for (;;)
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                    GSRegisters &g = rt->memory().gs();
                    const uint64_t fb = ::g_ps2xBildwechsel.load(std::memory_order_relaxed), tick = g.vsyncTick.load();
                    const auto jetzt = std::chrono::steady_clock::now();
                    if (testTick && tick >= testTick) { testTick = 0; bericht(rt, tick); }
                    if (fb != letzterFb || ::ps2xGsBridgeMenuOpen() || tick < 1200) { letzterFb = fb; seit = jetzt; seitTick = tick; gemeldet = false; continue; }
                    if (!gemeldet && jetzt - seit > std::chrono::seconds(8) && tick > seitTick + 400) { gemeldet = true; bericht(rt, seitTick); }
                }
            }).detach();
        }
    }
#endif

    void startWatchdog(PS2Runtime &runtime)
    {
        const char *env = std::getenv("SOTC_WATCHDOG");
        if (!env || !*env) return;
        const int period = std::max(1, std::atoi(env));
        PS2Runtime *rt = &runtime;
        std::thread([rt, period]() {
            for (int tick = 1;; ++tick)
            {
                std::this_thread::sleep_for(std::chrono::seconds(period));
                const EeKernelSnapshot snap = rt->eeScheduler().snapshot();
                std::fprintf(stderr, "[SotC watchdog t=%ds] running=%d cycle=%llu threads=%zu wakeups=%u iwakeups=%u vblankIrq=%u gsIrq=%u vsyncTick=%llu\n", tick * period,
                             snap.runningThreadId, static_cast<unsigned long long>(snap.eeCycle), snap.threads.size(),
                             g_wakeups.load(), g_iwakeups.load(), g_vblankHits.load(), g_gsIrqHits.load(),
                             (unsigned long long)rt->memory().gs().vsyncTick.load());
                for (const auto &t : snap.threads)
                    std::fprintf(stderr, "  thread %d status=%d wait=%d/%d prio=%d pc=0x%08X ra=0x%08X entry=0x%08X sp=0x%08X stack=[0x%08X..0x%08X)\n",
                                 t.id, static_cast<int>(t.status), static_cast<int>(t.waitReason), t.waitId,
                                 t.currentPriority, t.pc, t.ra, t.entry, t.sp, t.stack, t.stack + t.stackSize);
                if (std::getenv("SOTC_TRACE_VIF"))
                    sotc_vif_trace::dump();
                // SOTC_DUMP_CLUT=cbp : 16 CT32 entries in 8x2 layout
                if (const char *cl = std::getenv("SOTC_DUMP_CLUT"); cl && *cl)
                {
                    const unsigned cbp = (unsigned)std::strtoul(cl, nullptr, 0);
                    std::fprintf(stderr, "  clut %u:", cbp);
                    for (unsigned i = 0; i < 16; ++i)
                        std::fprintf(stderr, " %08x", rt->gs().ReadVram(0, cbp, 1, i & 7u, i >> 3));
                    if (FILE *tf = std::fopen(sotcDbgFile("sotc_tex4.pgm").c_str(), "wb"))
                    {
                        std::fprintf(tf, "P5\n128 128\n255\n");
                        for (unsigned ty = 0; ty < 128; ++ty)
                            for (unsigned tx = 0; tx < 128; ++tx)
                            {
                                unsigned char idx = (unsigned char)(rt->gs().ReadVram(20, 10752, 2, tx, ty) * 17u);
                                std::fwrite(&idx, 1, 1, tf);
                            }
                        std::fclose(tf);
                    }
                    if (FILE *tf = std::fopen(sotcDbgFile("sotc_tex4_ct32.ppm").c_str(), "wb"))
                    {
                        std::fprintf(tf, "P6\n64 32\n255\n");
                        for (unsigned ty = 0; ty < 32; ++ty)
                            for (unsigned tx = 0; tx < 64; ++tx)
                            {
                                uint32_t c = rt->gs().ReadVram(0, 10752, 1, tx, ty);
                                unsigned char rgb[3] = {(unsigned char)c, (unsigned char)(c >> 8), (unsigned char)(c >> 16)};
                                std::fwrite(rgb, 1, 3, tf);
                            }
                        std::fclose(tf);
                    }
                    std::fprintf(stderr, "\n  tex4 tbp=10752 row109:");
                    for (unsigned u = 96; u < 120; ++u)
                        std::fprintf(stderr, " %x", rt->gs().ReadVram(20, 10752, 2, u, 109));
                    std::fprintf(stderr, "\n");
                }
                // SOTC_DUMP_FB=fbp:fbw:psm:w:h,... -> /tmp/sotc_fb_<fbp>_<t>.ppm (fbp in 2048-word pages)
                if (const char *fbs = std::getenv("SOTC_DUMP_FB"); fbs && *fbs)
                {
                    std::string list(fbs);
                    size_t at = 0;
                    while (at < list.size())
                    {
                        size_t comma = list.find(',', at);
                        std::string item = list.substr(at, comma == std::string::npos ? std::string::npos : comma - at);
                        at = (comma == std::string::npos) ? list.size() : comma + 1;
                        unsigned fbp = 0, fbw = 8, psm = 0, w = 512, h = 448;
                        if (std::sscanf(item.c_str(), "%u:%u:%u:%u:%u", &fbp, &fbw, &psm, &w, &h) < 1)
                            continue;
                        char path[512];
                        std::snprintf(path, sizeof(path), "%s/sotc_fb_%u_%d.ppm", sotcDbgDir(), fbp, tick * period);
                        if (FILE *f = std::fopen(path, "wb"))
                        {
                            std::fprintf(f, "P6\n%u %u\n255\n", w, h);
                            for (unsigned y = 0; y < h; ++y)
                                for (unsigned x = 0; x < w; ++x)
                                {
                                    uint32_t c = rt->gs().ReadVram(psm, fbp * 32u, fbw, x, y);
                                    unsigned char rgb[3];
                                    if (psm == 2 || psm == 10)
                                    {
                                        rgb[0] = (unsigned char)((c & 31u) << 3);
                                        rgb[1] = (unsigned char)(((c >> 5) & 31u) << 3);
                                        rgb[2] = (unsigned char)(((c >> 10) & 31u) << 3);
                                    }
                                    else
                                    {
                                        rgb[0] = (unsigned char)c; rgb[1] = (unsigned char)(c >> 8); rgb[2] = (unsigned char)(c >> 16);
                                    }
                                    std::fwrite(rgb, 1, 3, f);
                                }
                            std::fclose(f);
                            std::fprintf(stderr, "  dumped %s\n", path);
                        }
                    }
                }
                if (std::getenv("SOTC_GSREGS"))
                {
                    auto &g = rt->memory().gs();
                    std::fprintf(stderr, "  gs: pmode=%llx smode2=%llx dispfb1=%llx display1=%llx dispfb2=%llx display2=%llx bgcolor=%llx csr=%llx imr=%llx\n",
                                 (unsigned long long)g.pmode, (unsigned long long)g.smode2, (unsigned long long)g.dispfb1,
                                 (unsigned long long)g.display1, (unsigned long long)g.dispfb2, (unsigned long long)g.display2,
                                 (unsigned long long)g.bgcolor, (unsigned long long)g.csr.load(), (unsigned long long)g.imr);
                }
                // SOTC_FIND_PAL: search guest RAM for the suspicious 4-bit palette bytes
                if (std::getenv("SOTC_FIND_PAL"))
                {
                    static const uint8_t needle[8] = {0x07, 0x0c, 0x05, 0x76, 0x0b, 0x2d, 0x05, 0x58};
                    const uint8_t *ram = rt->memory().getRDRAM();
                    int found = 0;
                    for (uint32_t a = 0; a + 64 < 0x2000000u && found < 4; ++a)
                        if (std::memcmp(ram + a, needle, 8) == 0)
                        {
                            ++found;
                            std::fprintf(stderr, "  pal@%08x:", a);
                            for (int k = -48; k < 80; ++k)
                                std::fprintf(stderr, "%s%02x", (k % 16 == 0) ? " | " : " ", ram[a + k]);
                            std::fprintf(stderr, "\n");
                        }
                    if (!found)
                        std::fprintf(stderr, "  pal not found in RAM\n");
                }
                // SOTC_FIND_WORD=0xVALUE : list RAM addresses holding this word (with neighbours)
                if (const char *fw = std::getenv("SOTC_FIND_WORD"); fw && *fw)
                {
                    const uint32_t needle = static_cast<uint32_t>(std::strtoul(fw, nullptr, 0));
                    const uint8_t *ram = rt->memory().getRDRAM();
                    int found = 0;
                    for (uint32_t a = 0; a < 0x2000000u && found < 24; a += 4)
                    {
                        uint32_t v; std::memcpy(&v, ram + a, 4);
                        if (v != needle) continue;
                        ++found;
                        std::fprintf(stderr, "  word@%08x:", a);
                        for (int k = -8; k < 8; ++k)
                        {
                            uint32_t w = 0;
                            const int64_t aa = static_cast<int64_t>(a) + k * 4;
                            if (aa >= 0 && aa < 0x2000000) std::memcpy(&w, ram + aa, 4);
                            std::fprintf(stderr, "%s%08x", k == 0 ? " >" : " ", w);
                        }
                        std::fprintf(stderr, "\n");
                    }
                    std::fprintf(stderr, "  find-word 0x%08X: %d hit(s)\n", needle, found);
                }
                // SOTC_PEEK=0xADDR,0xADDR,... : dump guest words
                if (const char *peek = std::getenv("SOTC_PEEK"); peek && *peek)
                {
                    const uint8_t *ram = rt->memory().getRDRAM();
                    std::string list(peek);
                    size_t pos = 0;
                    std::fprintf(stderr, "  peek:");
                    while (pos < list.size())
                    {
                        const size_t comma = list.find(',', pos);
                        const uint32_t addr = static_cast<uint32_t>(std::strtoul(list.substr(pos, comma - pos).c_str(), nullptr, 0));
                        uint32_t v = 0;
                        std::memcpy(&v, ram + (addr & kRamMask), 4);
                        std::fprintf(stderr, " [0x%08X]=0x%08X", addr, v);
                        if (comma == std::string::npos) break;
                        pos = comma + 1;
                    }
                    std::fprintf(stderr, "\n");
                }
                for (auto &slot : g_deltaSlots)
                    if (slot.addr && slot.calls)
                        std::fprintf(stderr, "  lockdelta 0x%08X calls=%u net=%lld\n", slot.addr, slot.calls.load(),
                                     static_cast<long long>(slot.delta.load()));
                if (std::getenv("SOTC_TRACE_OBJLOCK"))
                {
                    std::lock_guard<std::mutex> lock(g_objLockMutex);
                    std::fprintf(stderr, "  objlock wait:");
                    for (auto &[ra, n] : g_objWaitByRa) std::fprintf(stderr, " ra=0x%X x%u", ra, n);
                    std::fprintf(stderr, "\n  objlock signal:");
                    for (auto &[ra, n] : g_objSigByRa) std::fprintf(stderr, " ra=0x%X x%u", ra, n);
                    std::fprintf(stderr, "\n");
                }
                for (const auto &sm : snap.semaphores)
                    if (sm.waiters) std::fprintf(stderr, "  sema %d count=%d max=%d waiters=%u\n", sm.id, sm.count, sm.maxCount, sm.waiters);
            }
        }).detach();
    }

    void sotc_sceGetstat(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        ps2_syscalls::fioGetstat(rdram, ctx, runtime);
        leave(ctx);
    }
    // --- SIF tracing (SOTC_TRACE_SIF=1): log module upload to the IOP -----------------------
    bool traceSif() { static const bool on = std::getenv("SOTC_TRACE_SIF") != nullptr; return on; }

    void sotc_sceSifAllocSysMemory(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t type = arg(ctx, 4), size = arg(ctx, 5), addr = arg(ctx, 6);
        ps2_stubs::sceSifAllocSysMemory(rdram, ctx, runtime);
        if (traceSif()) std::fprintf(stderr, "[SotC SIF] AllocSysMemory(type=%u size=0x%x addr=0x%x) -> 0x%x\n", type, size, addr, GPR_U32(ctx, 2));
        leave(ctx);
    }
    // IOP destination -> EE source of recent SIF DMA uploads. The SDK's sceSifLoadModuleBuffer
    // takes an IOP address, while PS2Recomp's handler reads that address from EE RAM; we hand
    // it the EE source buffer the module was uploaded from (still intact at that point).
    std::mutex g_dmaMutex;
    std::map<uint32_t, std::pair<uint32_t, uint32_t>> g_dmaIopToEe;

    void sotc_sceSifSetDma(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t list = arg(ctx, 4), count = arg(ctx, 5);
        {
            std::lock_guard<std::mutex> lock(g_dmaMutex);
            for (uint32_t i = 0; i < count && i < 32; ++i)
            {
                uint32_t d[4];
                std::memcpy(d, rdram + ((list + 16 * i) & kRamMask), 16);
                if (d[2] > 0 && d[2] < 0x200000) g_dmaIopToEe[d[1] & 0x1FFFFF] = {d[0], d[2]};
            }
        }
        if (traceSif())
            for (uint32_t i = 0; i < count && i < 4; ++i)
            {
                uint32_t d[4];
                std::memcpy(d, rdram + ((list + 16 * i) & kRamMask), 16);
                std::fprintf(stderr, "[SotC SIF] SetDma[%u] src=0x%x dst=0x%x size=0x%x attr=0x%x\n", i, d[0], d[1], d[2], d[3]);
            }
        ps2_stubs::sceSifSetDma(rdram, ctx, runtime);
        if (traceSif()) std::fprintf(stderr, "[SotC SIF] SetDma -> %d\n", static_cast<int32_t>(GPR_U32(ctx, 2)));
        leave(ctx);
    }
    void sotc_sceSifLoadModuleBuffer(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t buf = arg(ctx, 4);
        uint8_t head[8] = {};
        const bool readable = runtime->readIopMemory(buf, head, sizeof(head));
        uint32_t eeSrc = 0;
        {
            std::lock_guard<std::mutex> lock(g_dmaMutex);
            auto it = g_dmaIopToEe.find(buf & 0x1FFFFF);
            if (it != g_dmaIopToEe.end()) eeSrc = it->second.first;
        }
        if (eeSrc && std::memcmp(rdram + (eeSrc & kRamMask), head, 4) == 0)
            SET_GPR_U32(ctx, 4, eeSrc);                    // module bytes as uploaded, in EE RAM
        ps2_stubs::sceSifLoadModuleBuffer(rdram, ctx, runtime);
        if (traceSif())
            std::fprintf(stderr, "[SotC SIF] LoadModuleBuffer(iop=0x%x ee=0x%x %s head=%02x %02x %02x %02x) -> %d\n", buf, eeSrc,
                         readable ? "ok" : "UNREADABLE", head[0], head[1], head[2], head[3], static_cast<int32_t>(GPR_U32(ctx, 2)));
        leave(ctx);
    }
    // --- Interrupt tracing (SOTC_TRACE_INTC=1) ------------------------------------------------
    bool traceIntc() { static const bool on = std::getenv("SOTC_TRACE_INTC") != nullptr; return on; }
    std::atomic<uint32_t> g_wakeups{0}, g_iwakeups{0};

    void sotc_AddIntcHandler(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t cause = arg(ctx, 4), handler = arg(ctx, 5), next = arg(ctx, 6), harg = arg(ctx, 7);
        ps2_syscalls::AddIntcHandler(rdram, ctx, runtime);
        if (traceIntc()) std::fprintf(stderr, "[SotC INTC] AddIntcHandler(cause=%u handler=0x%x next=%u arg=0x%x) -> %d\n",
                                      cause, handler, next, harg, static_cast<int32_t>(GPR_U32(ctx, 2)));
        leave(ctx);
    }
    void sotc_EnableIntc(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t cause = arg(ctx, 4);
        ps2_syscalls::EnableIntc(rdram, ctx, runtime);
        if (traceIntc()) std::fprintf(stderr, "[SotC INTC] _EnableIntc(cause=%u) -> %d\n", cause, static_cast<int32_t>(GPR_U32(ctx, 2)));
        leave(ctx);
    }
    void sotc_iWakeupThread(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        ++g_iwakeups;
        ps2_syscalls::iWakeupThread(rdram, ctx, runtime);
        leave(ctx);
    }
    void sotc_WakeupThread(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        ++g_wakeups;
        ps2_syscalls::WakeupThread(rdram, ctx, runtime);
        leave(ctx);
    }
    // ---- libpad2 (scePad2*) HLE ----
    // The game talks to the controller through libpad2/DBC RPCs to the IOP, which the runtime
    // does not emulate. padSysTickProc only needs: GetState()==1 (stable), a 4-byte button
    // profile, and Read() data = [btn0, btn1 (active low), rx, ry, lx, ly, pressures...].
    std::atomic<int> g_pad2Sockets{0};

    void sotc_scePad2Init(uint8_t *, R5900Context *ctx, PS2Runtime *)
    {
        g_pad2Sockets = 0;
        SET_GPR_S32(ctx, 2, 1);
        leave(ctx);
    }
    void sotc_scePad2End(uint8_t *, R5900Context *ctx, PS2Runtime *)
    {
        SET_GPR_S32(ctx, 2, 1);
        leave(ctx);
    }
    void sotc_scePad2CreateSocket(uint8_t *, R5900Context *ctx, PS2Runtime *)
    {
        SET_GPR_S32(ctx, 2, g_pad2Sockets.fetch_add(1));
        leave(ctx);
    }
    void sotc_scePad2GetState(uint8_t *, R5900Context *ctx, PS2Runtime *)
    {
        // Only the first socket (port 1) has a controller attached.
        SET_GPR_S32(ctx, 2, arg(ctx, 4) == 0u ? 1 : 0);
        leave(ctx);
    }
    void sotc_scePad2GetButtonProfile(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
    {
        const uint32_t buf = arg(ctx, 5);
        // DualShock 2: all 16 digital buttons, both sticks and pressure buttons.
        rdram[(buf + 0) & kRamMask] = 0xFF;
        rdram[(buf + 1) & kRamMask] = 0xFF;
        rdram[(buf + 2) & kRamMask] = 0xFF;
        rdram[(buf + 3) & kRamMask] = 0xFF;
        SET_GPR_S32(ctx, 2, 4);
        leave(ctx);
    }
    void sotc_scePad2Read(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t sock = arg(ctx, 4), buf = arg(ctx, 5);
        uint8_t raw[32] = {};
        if (sock != 0u || !runtime->padBackend().readState(0, 0, raw, sizeof(raw)))
        {
            SET_GPR_S32(ctx, 2, -1);
            leave(ctx);
            return;
        }
        // Input recording/playback per VSync (reproduce a bug from real play exactly in a test run)
        {
            const uint64_t tickNow = runtime->memory().gs().vsyncTick.load();
            if (std::getenv("SOTC_RAMDUMP")) sotcRamDumpCheck(rdram, tickNow); // Memory dumps at fixed points in the game
            // Game counter (iosGetISchedulerExecCnt, 0x1DC9EC): common time base with PCSX2 (readable through PINE)
            uint32_t gameCnt = 0; std::memcpy(&gameCnt, rdram + 0x1DC9ECu, 4);
            {
                static const char *mapPath = std::getenv("SOTC_TICKMAP"); // "vsyncTick gamecounter" per pad query
                static std::FILE *mf = (mapPath && *mapPath) ? std::fopen(mapPath, "w") : nullptr;
                static uint64_t lastT = ~0ull;
                if (mf && tickNow != lastT)
                {
                    lastT = tickNow;
                    // Player object like GAMEUTIL_get_player_obj / isysGroupForFirst(0x1477360) + position (diagnosis/comparison)
                    auto r32 = [&](uint32_t a) { uint32_t v = 0; std::memcpy(&v, rdram + (a & 0x1FFFFFCu), 4); return v; };
                    auto r16 = [&](uint32_t a) { uint16_t v = 0; std::memcpy(&v, rdram + (a & 0x1FFFFFEu), 2); return v; };
                    uint32_t player = 0;
                    uint32_t id = r16(0x1477360u);
                    for (int guard = 0; id && guard < 4096; ++guard)
                    {
                        const uint32_t node = 0x216B00u + id * 24u;
                        if (rdram[node + 0xC]) { id = r16(node + 0xE); continue; }
                        const uint32_t obj = r32(node);
                        if (r16(obj + 0xA) & 1u) { id = r16(node + 0xE); continue; }
                        player = obj;
                        break;
                    }
                    std::fprintf(mf, "%llu %u %08x\n", static_cast<unsigned long long>(tickNow), gameCnt, player);
                }
            }
            {
                // SOTC_RAMDUMP_GC="1000,2000": memory dump as soon as the game counter reaches the value (comparison with PCSX2 savestates)
                static std::vector<uint32_t> todo = [] { std::vector<uint32_t> v; if (const char *e = std::getenv("SOTC_RAMDUMP_GC"))
                    { std::string q(e); size_t p = 0; while (p < q.size()) { v.push_back(static_cast<uint32_t>(std::strtoul(q.c_str() + p, nullptr, 10)));
                      p = q.find(',', p); if (p == std::string::npos) break; ++p; } std::sort(v.rbegin(), v.rend()); } return v; }();
                if (!todo.empty() && gameCnt >= todo.back()) { const uint32_t w = todo.back(); todo.pop_back(); sotcRamDump(rdram, w, "gc"); }
            }
            static const char *recPath = std::getenv("SOTC_INPUT_RECORD");
            static const char *repPath = std::getenv("SOTC_INPUT_REPLAY");
            if (repPath && *repPath)
            {
                static std::vector<std::pair<uint64_t, std::array<uint8_t, 6>>> rep;
                static bool loaded = false;
                if (!loaded)
                {
                    loaded = true;
                    if (std::FILE *f = std::fopen(repPath, "r"))
                    {
                        unsigned long long t; unsigned v[6];
                        while (std::fscanf(f, "%llu %x %x %x %x %x %x", &t, &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) == 7)
                            rep.push_back({t, {uint8_t(v[0]), uint8_t(v[1]), uint8_t(v[2]), uint8_t(v[3]), uint8_t(v[4]), uint8_t(v[5])}});
                        std::fclose(f);
                    }
                    std::fprintf(stderr, "[input] playback: %zu entries from %s\n", rep.size(), repPath);
                }
                std::array<uint8_t, 6> cur = {0xFF, 0xFF, 0x80, 0x80, 0x80, 0x80};
                for (const auto &e : rep) { if (e.first > tickNow) break; cur = e.second; }
                for (int i = 0; i < 6; ++i) raw[2 + i] = cur[i];
            }
            else if (recPath && *recPath)
            {
                static std::FILE *rf = std::fopen(recPath, "w");
                static uint8_t last[6] = {0, 0, 0, 0, 0, 0};
                static bool first = true;
                if (rf && (first || std::memcmp(last, raw + 2, 6) != 0))
                {
                    first = false;
                    std::memcpy(last, raw + 2, 6);
                    std::fprintf(rf, "%llu %02x %02x %02x %02x %02x %02x\n", static_cast<unsigned long long>(tickNow),
                                 raw[2], raw[3], raw[4], raw[5], raw[6], raw[7]);
                    std::fflush(rf);
                }
            }
        }
        // SOTC_INPUT="tick:key,tick:key,..." scripted presses (held 8 vsyncs), keys: x o s t start select up down left right
        if (const char *script = std::getenv("SOTC_INPUT"); script && *script)
        {
            // Test run: only the scripted input, no real gamepad/keyboard in between
            raw[2] = raw[3] = 0xFF;
            raw[4] = raw[5] = raw[6] = raw[7] = 0x80;
            static std::vector<std::pair<uint64_t, std::string>> events;
            static bool parsed = false;
            if (!parsed)
            {
                parsed = true;
                std::string all(script);
                size_t at = 0;
                while (at < all.size())
                {
                    size_t comma = all.find(',', at);
                    std::string item = all.substr(at, comma == std::string::npos ? std::string::npos : comma - at);
                    at = comma == std::string::npos ? all.size() : comma + 1;
                    const size_t colon = item.find(':');
                    if (colon != std::string::npos)
                        events.emplace_back(std::strtoull(item.c_str(), nullptr, 10), item.substr(colon + 1));
                }
            }
            const uint64_t tick = runtime->memory().gs().vsyncTick.load();
            for (const auto &[t, keySpec] : events)
            {
                // "key" holds 8 vsyncs, "key*N" holds N vsyncs. Analog: lup/ldown/lleft/lright, rup/rdown/rleft/rright.
                std::string key = keySpec;
                uint64_t hold = 8u;
                if (const size_t star = key.find('*'); star != std::string::npos)
                {
                    hold = std::strtoull(key.c_str() + star + 1, nullptr, 10);
                    key = key.substr(0, star);
                }
                if (tick < t || tick >= t + hold)
                    continue;
                if (key == "lup") raw[7] = 0x00;
                else if (key == "ldown") raw[7] = 0xFF;
                else if (key == "lleft") raw[6] = 0x00;
                else if (key == "lright") raw[6] = 0xFF;
                else if (key == "rup") raw[5] = 0x00;
                else if (key == "rdown") raw[5] = 0xFF;
                else if (key == "rleft") raw[4] = 0x00;
                else if (key == "rright") raw[4] = 0xFF;
                static const struct { const char *name; int byte; int bit; } kKeys[] = {
                    {"select", 2, 0}, {"start", 2, 3}, {"up", 2, 4}, {"right", 2, 5}, {"down", 2, 6}, {"left", 2, 7},
                    {"l2", 3, 0}, {"r2", 3, 1}, {"l1", 3, 2}, {"r1", 3, 3}, {"t", 3, 4}, {"o", 3, 5}, {"x", 3, 6}, {"s", 3, 7}};
                for (const auto &k : kKeys)
                    if (key == k.name)
                        raw[k.byte] = static_cast<uint8_t>(raw[k.byte] & ~(1u << k.bit));
            }
        }
        // raw = classic libpad layout: [status, 0x73, btn0, btn1, rx, ry, lx, ly]
        uint8_t out[18];
        std::memset(out, 0, sizeof(out));
        out[0] = raw[2];
        out[1] = raw[3];
        out[2] = raw[4];
        out[3] = raw[5];
        out[4] = raw[6];
        out[5] = raw[7];
        // Pressure bytes: full pressure for pressed buttons (right, left, up, down, tri, circle, cross, square, L1, R1, L2, R2)
        static const struct { int byte; int bit; } kPressure[12] = {
            {0, 5}, {0, 7}, {0, 4}, {0, 6}, {1, 4}, {1, 5}, {1, 6}, {1, 7}, {1, 2}, {1, 3}, {1, 0}, {1, 1}};
        for (int i = 0; i < 12; ++i)
            out[6 + i] = (out[kPressure[i].byte] & (1u << kPressure[i].bit)) ? 0x00 : 0xFF;
        for (size_t i = 0; i < sizeof(out); ++i)
            rdram[(buf + i) & kRamMask] = out[i];
        static const bool kEnv_SOTC_TRACE_PAD = std::getenv("SOTC_TRACE_PAD") != nullptr;
        if (kEnv_SOTC_TRACE_PAD && (out[0] != 0xFF || out[1] != 0xFF))
            std::fprintf(stderr, "[SotC pad] buttons %02x %02x\n", out[0], out[1]);
        SET_GPR_S32(ctx, 2, static_cast<int32_t>(sizeof(out)));
        leave(ctx);
    }
    // ---- sceCdReadIOPm: read disc sectors into IOP RAM ----
    // The runtime forwards this to sceCdRead, which writes into EE RAM at the IOP address. Sound
    // streaming (ADPCM) uses it, and that overwrote the loader at 0x1064C0 (relocation jump table).
    void sotc_sceCdReadIOPm(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t lbn = arg(ctx, 4), sectors = arg(ctx, 5), iopBuf = arg(ctx, 6);
        const std::filesystem::path iso = PS2Runtime::getIoPaths().cdImage;
        uint8_t *iop = runtime->memory().getIOPRAM();
        constexpr uint32_t kIopMask = 0x1FFFFFu;
        bool ok = false;
        if (iop && !iso.empty())
        {
            if (std::FILE *f = ts2Oeffnen(iso))
            {
                std::vector<uint8_t> tmp(static_cast<size_t>(sectors) * 2048u);
                if (ts2Seek(f, static_cast<uint64_t>(lbn) * 2048u) &&
                    std::fread(tmp.data(), 1, tmp.size(), f) == tmp.size())
                {
                    // The IOP runs in the ps2xIOP emulator with its own RAM: write there (otherwise the
                    // music stream buffers stay empty = silence). The old runtime IOP RAM as well, for legacy paths.
                    static const bool oldPath = std::getenv("PS2X_CDREAD_OLD") != nullptr; // Diagnosis: music fix off
                    if (!oldPath && runtime->isIopMemoryRange(iopBuf & kIopMask, static_cast<uint32_t>(tmp.size())))
                        runtime->writeIopMemory(iopBuf & kIopMask, tmp.data(), tmp.size());
                    for (size_t i = 0; i < tmp.size(); ++i)
                        iop[(iopBuf + i) & kIopMask] = tmp[i];
                    ok = true;
                }
                std::fclose(f);
            }
        }
        static const bool kEnv_SOTC_TRACE_CDREAD = std::getenv("SOTC_TRACE_CDREAD") != nullptr;
        if (kEnv_SOTC_TRACE_CDREAD)
            std::fprintf(stderr, "[SotC cd] ReadIOPm lbn=%u sectors=%u iop=0x%X ok=%d\n", lbn, sectors, iopBuf, ok);
        (void)rdram;
        SET_GPR_S32(ctx, 2, ok ? 1 : 0);
        leave(ctx);
    }

    // ---- Memory card (libmc2 low level) ----
    // libmc2 keeps the whole PS2 card file system on the EE and only asks the IOP for raw pages
    // through _sceMcCore*. We answer those calls from a standard 8 MB card image in PCSX2 format
    // (16384 pages x (512 data + 16 spare/ECC)), so saves are interchangeable with PCSX2.
    constexpr uint32_t kMcPageSize = 512, kMcSpare = 16, kMcRaw = kMcPageSize + kMcSpare;
    constexpr uint32_t kMcPagesPerBlock = 16, kMcPages = 16384;
    constexpr uint32_t kMcSocketTable = 0x001609C8u; // 2 x 0x24 bytes: used, dbc socket, pagesize, blocksize, cardsize, ...

    std::mutex g_mcMutex;
    std::FILE *g_mcFile = nullptr;
    bool g_mcNewCard[2] = {true, true};
    std::string g_mcPath;

    uint8_t mcParity(uint8_t b) { return static_cast<uint8_t>(__builtin_popcount(b) & 1); }

    void mcEcc128(const uint8_t *d, uint8_t out[3])
    {
        static const uint8_t cpmasks[7] = {0x55, 0x33, 0x0F, 0x00, 0xAA, 0xCC, 0xF0};
        uint8_t col = 0x77, line0 = 0x7F, line1 = 0x7F;
        for (int i = 0; i < 128; ++i)
        {
            uint8_t mask = 0;
            for (int k = 0; k < 7; ++k)
                mask |= static_cast<uint8_t>(mcParity(d[i] & cpmasks[k]) << k);
            col ^= mask;
            if (mcParity(d[i]))
            {
                line0 ^= static_cast<uint8_t>(~i);
                line1 ^= static_cast<uint8_t>(i);
            }
        }
        out[0] = col;
        out[1] = line0 & 0x7F;
        out[2] = line1;
    }

    bool mcOpen()
    {
        if (g_mcFile)
            return true;
        if (g_mcPath.empty())
        {
            const char *env = std::getenv("SOTC_MEMCARD");
            g_mcPath = env && *env ? env : "Mcd001.ps2";
        }
        g_mcFile = std::fopen(g_mcPath.c_str(), "r+b");
        if (!g_mcFile)
        {
            g_mcFile = std::fopen(g_mcPath.c_str(), "w+b");
            if (!g_mcFile)
            {
                std::fprintf(stderr, "[SotC mc] cannot open %s\n", g_mcPath.c_str());
                return false;
            }
            std::vector<uint8_t> blank(kMcRaw * kMcPagesPerBlock, 0xFF);
            for (uint32_t b = 0; b < kMcPages / kMcPagesPerBlock; ++b)
                std::fwrite(blank.data(), 1, blank.size(), g_mcFile);
            std::fflush(g_mcFile);
            std::fprintf(stderr, "[SotC mc] created blank card %s\n", g_mcPath.c_str());
        }
        else
            std::fprintf(stderr, "[SotC mc] using card %s\n", g_mcPath.c_str());
        return true;
    }

    bool mcCardPresent(uint32_t sock) { return sock == 0u && mcOpen(); }

    void mcSet(R5900Context *ctx, int32_t v)
    {
        SET_GPR_S32(ctx, 2, v);
        leave(ctx);
    }

    void sotc_mcCreateSocket(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
    {
        uint32_t port;
        std::memcpy(&port, rdram + ((arg(ctx, 4) + 4u) & kRamMask), 4);
        const uint32_t idx = (port >= 2u) ? port - 2u : 0u;
        if (idx > 1u) { mcSet(ctx, -1); return; }
        const uint32_t e = kMcSocketTable + idx * 0x24u;
        const uint32_t vals[5] = {1u, 0x4D430000u + idx, kMcPageSize, kMcPagesPerBlock, kMcPages};
        for (int k = 0; k < 5; ++k) std::memcpy(rdram + ((e + 4u * k) & kRamMask), &vals[k], 4);
        const uint32_t zero = 0;
        for (uint32_t off : {0x14u, 0x18u, 0x1Cu, 0x20u}) std::memcpy(rdram + ((e + off) & kRamMask), &zero, 4);
        g_mcNewCard[idx] = true;
        mcSet(ctx, static_cast<int32_t>(idx));
    }
    void sotc_mcDeleteSocket(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
    {
        const uint32_t idx = arg(ctx, 4);
        if (idx > 1u) { mcSet(ctx, 0); return; }
        const uint32_t zero = 0;
        std::memcpy(rdram + ((kMcSocketTable + idx * 0x24u) & kRamMask), &zero, 4);
        mcSet(ctx, 1);
    }
    void sotc_mcGetConnection(uint8_t *, R5900Context *ctx, PS2Runtime *)
    {
        std::lock_guard<std::mutex> lock(g_mcMutex);
        mcSet(ctx, mcCardPresent(arg(ctx, 4)) ? 1 : 0);
    }
    void sotc_mcGetCardSpec(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
    {
        std::lock_guard<std::mutex> lock(g_mcMutex);
        const uint32_t sock = arg(ctx, 4), out = arg(ctx, 5);
        if (!mcCardPresent(sock)) { mcSet(ctx, 0); return; }
        const uint32_t e = kMcSocketTable + sock * 0x24u;
        const uint32_t vals[4] = {kMcPageSize, kMcPagesPerBlock, kMcPages, 1u};
        for (int k = 0; k < 4; ++k)
        {
            std::memcpy(rdram + ((e + 8u + 4u * k) & kRamMask), &vals[k], 4);
            if (out) std::memcpy(rdram + ((out + 4u * k) & kRamMask), &vals[k], 4);
        }
        mcSet(ctx, 1);
    }
    void sotc_mcCheckNewCard(uint8_t *, R5900Context *ctx, PS2Runtime *)
    {
        std::lock_guard<std::mutex> lock(g_mcMutex);
        const uint32_t sock = arg(ctx, 4);
        if (!mcCardPresent(sock)) { mcSet(ctx, 0); return; }
        mcSet(ctx, g_mcNewCard[sock & 1u] ? 2 : 1);
    }
    void sotc_mcClearNewCardFlag(uint8_t *, R5900Context *ctx, PS2Runtime *)
    {
        std::lock_guard<std::mutex> lock(g_mcMutex);
        const uint32_t sock = arg(ctx, 4);
        if (sock < 2u) g_mcNewCard[sock] = false;
        mcSet(ctx, mcCardPresent(sock) ? 1 : 0);
    }
    void sotc_mcPresentOk(uint8_t *, R5900Context *ctx, PS2Runtime *) // Authentication, WriteBack
    {
        std::lock_guard<std::mutex> lock(g_mcMutex);
        mcSet(ctx, mcCardPresent(arg(ctx, 4)) ? 1 : 0);
    }
    void sotc_mcReadClock(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
    {
        const uint32_t out = arg(ctx, 5);
        auto bcd = [](int v) { return static_cast<uint8_t>(((v / 10) << 4) | (v % 10)); };
        const std::time_t now = std::time(nullptr);
        std::tm tmv{};
#if defined(_WIN32)
        localtime_s(&tmv, &now);
#else
        localtime_r(&now, &tmv);
#endif
        const uint8_t clk[8] = {0, bcd(tmv.tm_sec), bcd(tmv.tm_min), bcd(tmv.tm_hour), 0,
                                bcd(tmv.tm_mday), bcd(tmv.tm_mon + 1), bcd(tmv.tm_year % 100)};
        for (int k = 0; k < 8; ++k) rdram[(out + k) & kRamMask] = clk[k];
        mcSet(ctx, 1);
    }
    void sotc_mcReadPage2(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
    {
        std::lock_guard<std::mutex> lock(g_mcMutex);
        const uint32_t sock = arg(ctx, 4), buf = arg(ctx, 5), page = arg(ctx, 6), count = arg(ctx, 7), ecc = arg(ctx, 8);
        if (!mcCardPresent(sock) || page + count > kMcPages) { mcSet(ctx, 0); return; }
        uint8_t raw[kMcRaw];
        for (uint32_t i = 0; i < count; ++i)
        {
            std::fseek(g_mcFile, static_cast<long>((page + i) * kMcRaw), SEEK_SET);
            if (std::fread(raw, 1, kMcRaw, g_mcFile) != kMcRaw) std::memset(raw, 0xFF, kMcRaw);
            for (uint32_t b = 0; b < kMcPageSize; ++b) rdram[(buf + i * kMcPageSize + b) & kRamMask] = raw[b];
            if (ecc)
                for (uint32_t b = 0; b < kMcSpare; ++b) rdram[(ecc + i * kMcSpare + b) & kRamMask] = raw[kMcPageSize + b];
        }
        mcSet(ctx, static_cast<int32_t>(count));
    }
    void sotc_mcReadPage(uint8_t *rdram, R5900Context *ctx, PS2Runtime *rt)
    {
        SET_GPR_U32(ctx, 8, 0u);
        sotc_mcReadPage2(rdram, ctx, rt);
    }
    void sotc_mcWritePage(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
    {
        std::lock_guard<std::mutex> lock(g_mcMutex);
        const uint32_t sock = arg(ctx, 4), buf = arg(ctx, 5), page = arg(ctx, 6), count = arg(ctx, 7);
        if (!mcCardPresent(sock) || page + count > kMcPages) { mcSet(ctx, 0); return; }
        uint8_t raw[kMcRaw];
        for (uint32_t i = 0; i < count; ++i)
        {
            for (uint32_t b = 0; b < kMcPageSize; ++b) raw[b] = rdram[(buf + i * kMcPageSize + b) & kRamMask];
            std::memset(raw + kMcPageSize, 0, kMcSpare);
            for (int c = 0; c < 4; ++c) mcEcc128(raw + c * 128, raw + kMcPageSize + c * 3);
            std::fseek(g_mcFile, static_cast<long>((page + i) * kMcRaw), SEEK_SET);
            std::fwrite(raw, 1, kMcRaw, g_mcFile);
        }
        std::fflush(g_mcFile);
        mcSet(ctx, static_cast<int32_t>(count));
    }
    void sotc_mcBlockErase(uint8_t *, R5900Context *ctx, PS2Runtime *)
    {
        std::lock_guard<std::mutex> lock(g_mcMutex);
        const uint32_t sock = arg(ctx, 4), block = arg(ctx, 5);
        if (!mcCardPresent(sock) || block >= kMcPages / kMcPagesPerBlock) { mcSet(ctx, 0); return; }
        std::vector<uint8_t> ff(kMcRaw * kMcPagesPerBlock, 0xFF);
        std::fseek(g_mcFile, static_cast<long>(block * kMcPagesPerBlock * kMcRaw), SEEK_SET);
        std::fwrite(ff.data(), 1, ff.size(), g_mcFile);
        std::fflush(g_mcFile);
        mcSet(ctx, 1);
    }
    void sotc_mcWriteSameData(uint8_t *, R5900Context *ctx, PS2Runtime *)
    {
        std::fprintf(stderr, "[SotC mc] WriteSameData(%u, 0x%X, %u) not implemented\n", arg(ctx, 4), arg(ctx, 5), arg(ctx, 6));
        mcSet(ctx, static_cast<int32_t>(arg(ctx, 6)));
    }
} // namespace

namespace
{
    // SOTC_RAMDUMP="5000,7000,8500": writes the 32 MB EE RAM when each vsync value is reached to
    // $SOTC_RAMDUMP_DIR/sotc_ram_<vsync>.bin (same evaluator as for PCSX2 savestates). Also on abort.
    void sotcRamDump(const uint8_t *r, uint64_t vs, const char *tag)
    {
        const char *dir = std::getenv("SOTC_RAMDUMP_DIR");
        if (!dir) dir = std::getenv("SOTC_DEBUG_DIR");   // Test farm: result directory of the run
        char path[512];
        std::snprintf(path, sizeof(path), "%s/sotc_ram_%s%05llu.bin", dir ? dir : ".", tag, (unsigned long long)vs);
        if (FILE *f = std::fopen(path, "wb")) { std::fwrite(r, 1, 32u * 1024u * 1024u, f); std::fclose(f);
            std::fprintf(stderr, "[SotC ramdump] %s\n", path); }
    }
    void sotcRamDumpCheck(const uint8_t *r, uint64_t vs)
    {
        static std::vector<uint64_t> todo = [] { std::vector<uint64_t> v; if (const char *e = std::getenv("SOTC_RAMDUMP"))
            { std::string s(e); size_t p = 0; while (p < s.size()) { v.push_back(std::strtoull(s.c_str() + p, nullptr, 10));
              p = s.find(',', p); if (p == std::string::npos) break; ++p; } std::sort(v.rbegin(), v.rend()); } return v; }();
        static std::mutex m; if (todo.empty()) return;
        std::lock_guard<std::mutex> lk(m);
        if (!todo.empty() && vs >= todo.back()) { todo.pop_back(); sotcRamDump(r, vs, ""); }
    }
}

// ---- ISO9660: real LSNs straight from the disc image (general, replaces SotC's fixed file table) ----
namespace ts2_iso
{
    std::mutex g_m;
    std::FILE *g_f = nullptr;
    bool open()
    {
        if (g_f) return true;
        const auto p = PS2Runtime::getIoPaths().cdImage;
        if (p.empty()) return false;
        g_f = ts2Oeffnen(p);
        if (!g_f)
        {
            static bool gemeldet = false;
            if (!gemeldet) std::fprintf(stderr, "[TS2] ERROR: disc image not readable (moved or renamed?): %s\n", p.string().c_str());
            gemeldet = true;
        }
        return g_f != nullptr;
    }
    bool sector(uint32_t lsn, uint8_t *buf)
    {
        return ts2Seek(g_f, static_cast<uint64_t>(lsn) * 2048u) && std::fread(buf, 1, 2048, g_f) == 2048;
    }
    // Path like "\PAK\CHR.PAK;1" or "cdrom0:\MUSIC\LEVEL1\3.VAG;1" -> lsn/size; case-insensitive
    bool lookup(const std::string &path, uint32_t &lsn, uint32_t &size)
    {
        std::lock_guard<std::mutex> lk(g_m);
        if (!open()) return false;
        uint8_t b[2048];
        if (!sector(16, b) || b[0] != 1 || std::memcmp(b + 1, "CD001", 5) != 0) return false;
        uint32_t dLsn, dSize; std::memcpy(&dLsn, b + 156 + 2, 4); std::memcpy(&dSize, b + 156 + 10, 4);
        std::string rest = normCdName(path);
        while (!rest.empty())
        {
            const auto cut = rest.find('\\');
            const std::string part = rest.substr(0, cut);
            rest = cut == std::string::npos ? std::string() : rest.substr(cut + 1);
            bool found = false;
            for (uint32_t off = 0; off < dSize && !found; off += 2048)
            {
                if (!sector(dLsn + off / 2048, b)) return false;
                for (uint32_t i = 0; i < 2048 && b[i];)
                {
                    const uint8_t len = b[i];
                    std::string n(reinterpret_cast<char *>(b + i + 33), b[i + 32]);
                    if (const auto sc = n.find(';'); sc != std::string::npos) n.resize(sc);
                    while (!n.empty() && n.back() == '.') n.pop_back();
                    for (auto &c : n) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
                    if (n == part)
                    {
                        std::memcpy(&dLsn, b + i + 2, 4); std::memcpy(&dSize, b + i + 10, 4);
                        found = true; break;
                    }
                    i += len;
                }
            }
            if (!found) return false;
        }
        lsn = dLsn; size = dSize;
        return true;
    }
}
namespace
{
    // int sceCdSearchFile(sceCdlFILE *fp, const char *name) / sceCdLayerSearchFile(fp, name, layer)
    // sceCdlFILE = {u32 lsn, u32 size, char name[16], u8 date[8]}
    void ts2_sceCdSearchFile(uint8_t *rdram, R5900Context *ctx, PS2Runtime *rtp)
    {
        ts2_dtoa_test::versuch(rdram, ctx, rtp);
        const uint32_t fp = arg(ctx, 4), namePtr = arg(ctx, 5);
        std::string name;
        for (uint32_t i = 0; i < 255; ++i) { const char c = static_cast<char>(rdram[(namePtr + i) & kRamMask]); if (!c) break; name.push_back(c); }
        uint32_t lsn = 0, size = 0;
        if (ts2_iso::lookup(name, lsn, size))
        {
            uint8_t *out = rdram + (fp & kRamMask);
            std::memset(out, 0, 32);
            std::memcpy(out + 0, &lsn, 4); std::memcpy(out + 4, &size, 4);
            const std::string key = normCdName(name);
            const std::string leaf = key.substr(key.rfind('\\') == std::string::npos ? 0 : key.rfind('\\') + 1);
            std::strncpy(reinterpret_cast<char *>(out + 8), leaf.c_str(), 15);
            static int n = 0;
            if (n++ < 200) std::fprintf(stderr, "[TS2] sceCdSearchFile %s -> lsn %u size %u\n", name.c_str(), lsn, size);
            ret(ctx, 1);
        }
        else
        {
            std::fprintf(stderr, "[TS2] sceCdSearchFile %s -> not found\n", name.c_str());
            ret(ctx, 0);
        }
        leave(ctx);
    }
}
// Definitions under the generated names (tools/ts2_bauen.sh empties the generated files of the same name)
#define TS2_DEFINE(GEN_NAME, IMPL) \
    void GEN_NAME(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) { IMPL(rdram, ctx, runtime); }
TS2_DEFINE(sceCdLayerSearchFile_0x488dc0, ts2_sceCdSearchFile)

// libmtap (multitap): the original asks MTAPMAN.IRX, which queries the hardware over SIO2 (not reproduced in the port).
// Pad input is HLE anyway (Pad.cpp), so here: multitap on port 1 when a third gamepad is plugged in.
namespace
{
    void ts2_sceMtapPortOpen(uint8_t *, R5900Context *ctx, PS2Runtime *) { ret(ctx, 1); leave(ctx); }
    void ts2_sceMtapPortClose(uint8_t *, R5900Context *ctx, PS2Runtime *) { ret(ctx, 1); leave(ctx); }
    void ts2_sceMtapGetConnection(uint8_t *, R5900Context *ctx, PS2Runtime *)
    {
        const uint32_t port = arg(ctx, 4);
        const int r = (port == 0u && ::ps2xMultitapActive()) ? 1 : 0;
        static int n = 0;
        if (n++ < 20) std::fprintf(stderr, "[MTAP] GetConnection port=%u -> %d\n", port, r);
        ret(ctx, r);
        leave(ctx);
    }
}
TS2_DEFINE(sceMtapPortOpen_0x44e248, ts2_sceMtapPortOpen)
TS2_DEFINE(sceMtapPortClose_0x44e2b8, ts2_sceMtapPortClose)
TS2_DEFINE(sceMtapGetConnection_0x44e328, ts2_sceMtapGetConnection)

// TS2_STUBCOUNT=1 (only with instrumented code, tools/ts2_stub_zaehlen.py): how often is each HLE replacement function
// called? Output at exit and every minute to stderr ("[STUBS] name=count ...").
#include <unordered_map>
extern uint32_t g_ps2xGameGParamAddr;   // Runtime GS.cpp: the game's sceGsGParam (sceGsGetGParam 0x4416E0 -> 0x56A250)
void ts2StubCount(const char *name)
{
    static std::mutex m; static std::unordered_map<const char *, uint64_t> cnt; static auto last = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> l(m);
    ++cnt[name];
    const auto now = std::chrono::steady_clock::now();
    if (now - last > std::chrono::seconds(20))
    {
        last = now;
        std::vector<std::pair<std::string, uint64_t>> v(cnt.begin(), cnt.end());
        std::sort(v.begin(), v.end());
        std::fprintf(stderr, "[STUBS]");
        for (const auto &[n, c] : v) std::fprintf(stderr, " %s=%llu", n.c_str(), (unsigned long long)c);
        std::fprintf(stderr, "\n");
    }
}

// TS2_DTOA_TEST=1 (test runs only): call the game's ORIGINAL functions malloc (0x48EE78) and sprintf (0x490180)
// once while the game is running, as at the gun turret (printf("%f") -> _dtoa_r -> _Balloc -> malloc).
// Runs in the EE context (on the first sceCdSearchFile); registers are restored afterwards.
namespace ts2_dtoa_test
{
    bool rufe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *rt, uint32_t fn, uint32_t magic)
    {
        ctx->r[31] = _mm_set_epi64x(0, static_cast<int64_t>(magic));
        ctx->pc = fn;
        for (int i = 0; i < 2000000 && ctx->pc != magic; ++i)
        {
            auto f = rt->lookupFunction(ctx->pc);
            if (!f) { std::fprintf(stderr, "[DTOA-TEST] no function for pc=0x%08X\n", ctx->pc); return false; }
            f(rdram, ctx, rt);
        }
        return ctx->pc == magic;
    }
    void versuch(uint8_t *rdram, R5900Context *ctx, PS2Runtime *rt)
    {
        static const bool aktiv = [] { const char *e = std::getenv("TS2_DTOA_TEST"); return e && *e == '1'; }();
        static bool fertig = false;
        if (aktiv && !fertig)
        {
            fertig = true;
            const R5900Context sicher = *ctx;
            const uint32_t sp = (GPR_U32(ctx, 29) - 0x800u) & ~0xFu, buf = sp + 0x100u, fmt = sp + 0x300u, magic = 0x00200000u;
            ctx->r[29] = _mm_set_epi64x(0, static_cast<int64_t>(sp));
            std::memset(rdram + buf, 0, 0x100);
            const char *f = "%f|%.3f|%d";
            std::memcpy(rdram + fmt, f, std::strlen(f) + 1);
            ctx->r[4] = _mm_set_epi64x(0, 64);
            const bool ok1 = rufe(rdram, ctx, rt, 0x0048EE78u, magic);
            const uint32_t m = GPR_U32(ctx, 2);
            std::fprintf(stderr, "[DTOA-TEST] malloc(64) -> 0x%08X (%s)\n", m, ok1 ? "zurueck" : "NICHT zurueck");
            // Soft-float routines (libgcc fp-bit) one by one against the host: dpadd/dpsub/dpmul/dpdiv (a0,a1 -> v0)
            {
                struct R { const char *n; uint32_t ad; int op; };
                const R rs[] = {{"dpadd", 0x0048C9F8u, 0}, {"dpsub", 0x0048CA50u, 1}, {"dpmul", 0x0048CAB8u, 2}, {"dpdiv", 0x0048CD68u, 3}};
                const double paare[][2] = {{1.5, 2.25}, {1234.5678, 10.0}, {0.5, 3.0}, {3.14159, 2.71828}, {1e10, 1e-5}, {-2.25, 0.1}, {7.0, 0.0001}};
                int fehler = 0;
                for (const R &r : rs)
                    for (const auto &pq : paare)
                    {
                        int64_t a, b; std::memcpy(&a, &pq[0], 8); std::memcpy(&b, &pq[1], 8);
                        ctx->r[29] = _mm_set_epi64x(0, static_cast<int64_t>(sp));
                        ctx->r[4] = _mm_set_epi64x(0, a); ctx->r[5] = _mm_set_epi64x(0, b);
                        rufe(rdram, ctx, rt, r.ad, magic);
                        const uint64_t v = static_cast<uint64_t>(_mm_cvtsi128_si64(ctx->r[2])); double g; std::memcpy(&g, &v, 8);
                        const double h = r.op == 0 ? pq[0] + pq[1] : r.op == 1 ? pq[0] - pq[1] : r.op == 2 ? pq[0] * pq[1] : pq[0] / pq[1];
                        uint64_t hb; std::memcpy(&hb, &h, 8);
                        if (hb != v) { ++fehler; std::fprintf(stderr, "[DTOA-TEST] %s(%.17g, %.17g) = %.17g, Host %.17g FALSCH (%016llx vs %016llx)\n", r.n, pq[0], pq[1], g, h,
                                                              (unsigned long long)v, (unsigned long long)hb); }
                    }
                std::fprintf(stderr, "[DTOA-TEST] Softfloat: %d Abweichungen\n", fehler);
            }
            // several values one by one: "%f" with exactly one double (in $a2), plus a "%d" check
            const double werte[] = {1.5, 0.5, 100.0, 1234.5678, 3.14159, -2.25, 0.001, 1e10, 123456789.0};
            const char *f1 = "%f";
            std::memcpy(rdram + fmt, f1, 3);
            for (double w : werte)
            {
                int64_t bits; std::memcpy(&bits, &w, 8);
                std::memset(rdram + buf, 0, 0x100);
                ctx->r[29] = _mm_set_epi64x(0, static_cast<int64_t>(sp));
                ctx->r[4] = _mm_set_epi64x(0, buf); ctx->r[5] = _mm_set_epi64x(0, fmt); ctx->r[6] = _mm_set_epi64x(0, bits);
                const bool ok2 = rufe(rdram, ctx, rt, 0x00490180u, magic);
                char host[64]; std::snprintf(host, sizeof host, "%f", w);
                const char *g = reinterpret_cast<const char *>(rdram + buf);
                std::fprintf(stderr, "[DTOA-TEST] %%f %-14s -> %-22.40s %s%s\n", host, g, std::strcmp(host, g) == 0 ? "OK" : "FALSCH", ok2 ? "" : " (NICHT zurueck)");
            }
            *ctx = sicher;
        }
    }
}

namespace ps2_stubs { void setPadOverrideState(uint16_t buttons, uint8_t lx, uint8_t ly, uint8_t rx, uint8_t ry); void clearPadOverrideState(); }
namespace
{
    // SOTC_INPUT="tick:key,tick:key*N,..." (same format as SotC): "key" holds 8 VSyncs, "key*N" N VSyncs.
    // Keys: x o s t start select up down left right l1 l2 r1 r2, analog lup/ldown/lleft/lright, rup/rdown/rleft/rright.
    // Through the runtime's setPadOverrideState (libpad HLE), port 0. Without SOTC_INPUT the real gamepad stays active.
    // TS2_WIDESCREEN=1: true 16:9 - aspect ratio constants as in the PCSX2 patch SLUS-20314_12532F1C.pnach
    // ("Widescreen 16:9", author No.47, patch=1 = every frame): 0x3f955555 (1.1667) -> 0x3fc71c71 (1.5556).
    // Data only, no code -> no retranslation needed. The renderer is set to 16:9 as well (PS2X_PCSX2_WIDESCREEN=1).
    // ---- Mouse aiming (PC): set the camera rotation directly in the player object ----
    // Method and object layout as in the Mouse Injector (GameCube version of TS2, GPL-2.0+, Carnivorous/garungorp):
    // yaw +0x148 and pitch +0x14C (degrees), crosshair sway +0x8B8/+0x8BC (-1..1). The PS2 addresses were found by
    // comparing RAM (right stick turns/tilts): player pointer 0x59DC0C, pitch limit 0x59DC1C (40 degrees).
    // Last pad state from SOTC_INPUT (test runs): the free camera reads it instead of the real pad
    static uint16_t g_skriptPad = 0; static uint8_t g_skriptAx[4] = {0x80, 0x80, 0x80, 0x80}; static bool g_skriptPadDa = false; static uint16_t g_skriptPadRoh = 0; static uint8_t g_skriptRStickRoh[2] = {0x80, 0x80};
    std::atomic<bool> g_ts2EigenesZiel{false};   // mouse aim mode active: the game's aim HUD is hidden, the port draws its crosshair
    std::atomic<bool> g_ts2MausZielt{false};     // mouse captured in first-person view: no game auto-aim
    static uint64_t g_ts2WechselTick = ~0ull >> 1;   // VBlank of the last weapon change (input or held model)
    std::atomic<bool> g_ts2Geschuetz{false};     // player on a turret / mounted gun (set in the weapon block below)
    namespace maus
    {
        constexpr uint32_t kSpielerZeiger = 0x0059DC0Cu;
        constexpr uint32_t kNickGrenze = 0x0059DC1Cu;
        inline float rf(const uint8_t *r, uint32_t a) { float v; std::memcpy(&v, r + (a & 0x1FFFFFCu), 4); return v; }
        inline void wf(uint8_t *r, uint32_t a, float v) { std::memcpy(r + (a & 0x1FFFFFCu), &v, 4); }
        // TS2_MAUS_TEST="tick:dx:dy*N,..." (test runs without a real mouse): dx/dy counts per VBlank for N VBlanks
        bool testDelta(uint64_t tick, int &dx, int &dy)
        {
            static const char *e = std::getenv("TS2_MAUS_TEST");
            if (!e || !*e) return false;
            struct T { uint64_t t, n; int dx, dy; };
            static std::vector<T> v = [] { std::vector<T> r; std::string s(e); size_t p = 0;
                while (p < s.size()) { T x{0, 1, 0, 0}; unsigned long long t = 0, n = 1; int a = 0, b = 0;
                    const size_t c = s.find(',', p); const std::string it = s.substr(p, c == std::string::npos ? std::string::npos : c - p);
                    if (std::sscanf(it.c_str(), "%llu:%d:%d*%llu", &t, &a, &b, &n) >= 3) { x.t = t; x.dx = a; x.dy = b; x.n = n; r.push_back(x); }
                    if (c == std::string::npos) break; p = c + 1; }
                return r; }();
            dx = dy = 0;
            for (const auto &x : v) if (tick >= x.t && tick < x.t + x.n) { dx += x.dx; dy += x.dy; }
            return true;
        }
        void tick(uint8_t *r, uint64_t tick)
        {
            int dx = 0, dy = 0;
            const bool test = testDelta(tick, dx, dy);
            bool gefangen = test || ::ps2xGsBridgeMouse(dx, dy) != 0;
            // Modern gamepad aiming (settings menu, Controls; TS2_PAD_MODERN=1 for test runs; off by default, the original
            // controls stay as they are): the right stick turns the view directly, like the mouse does - crosshair fixed in
            // the screen centre, shots land on it, no auto-aim pulling the view. L2 is the zoom/scope button.
            static const int envModern = [] { const char *e = std::getenv("TS2_PAD_MODERN"); return (e && *e == '1') ? 1 : 0; }();
            static int modern = 0; if ((tick & 31u) == 0u) modern = ::ps2xGsBridgeOption(32, envModern);
            const bool padZiel = modern != 0 && !gefangen;
            float padX = 0.f, padY = 0.f;
            if (padZiel)
            {
                uint8_t rx = 0x80, ry = 0x80;
                if (g_skriptPadDa) { rx = g_skriptRStickRoh[0]; ry = g_skriptRStickRoh[1]; }
                else { const unsigned a = g_ps2xPadAchsenRoh.load(std::memory_order_relaxed); rx = static_cast<uint8_t>(a >> 16); ry = static_cast<uint8_t>(a >> 24); }
                float x = (static_cast<float>(rx) - 128.f) / 127.f, y = (static_cast<float>(ry) - 128.f) / 127.f;
                const float m = std::sqrt(x * x + y * y);
                constexpr float tot = 0.16f;   // radial dead zone, then a soft curve: fine aiming near the centre, fast turns at the edge
                if (m > tot) { const float n = std::min((m - tot) / (1.f - tot), 1.f); const float f = (0.35f * n + 0.65f * n * n) / m; padX = x * f; padY = y * f; }
                gefangen = true;
            }
            // First-person view? (player object present, yaw/pitch in the valid range) - otherwise touch nothing
            uint32_t pb = 0; std::memcpy(&pb, r + kSpielerZeiger, 4);
            bool ego = pb >= 0x100000u && pb < 0x1FF0000u;
            // Cutscene (issue #8: mouse crosshair visible in cutscenes): in the game the camera *(0x59DB7C) +0x27C/+0x280/+0x284
            // sits exactly on the player position +0x1A0/+0x1A4/+0x1A8, in cutscenes somewhere else (measured in the Siberia
            // intro). Then it counts like no first-person view: no crosshair, no aim mode, no view writes. TS2_SEQUENZ=0 = off.
            if (ego)
            {
                static const bool seqAn = [] { const char *e = std::getenv("TS2_SEQUENZ"); return !(e && *e == '0'); }();
                uint32_t cam = 0; std::memcpy(&cam, r + 0x0059DB7Cu, 4); cam &= 0x1FFFFFFu;
                if (seqAn && cam >= 0x100000u && cam < 0x1FFF000u && (pb & 0x1FFFFFFu) + 0x1B0u < 0x2000000u)
                {
                    const uint32_t q = pb & 0x1FFFFFFu;
                    const bool seq = std::fabs(rf(r, cam + 0x27C) - rf(r, q + 0x1A0)) > 0.5f || std::fabs(rf(r, cam + 0x280) - rf(r, q + 0x1A4)) > 0.5f ||
                                     std::fabs(rf(r, cam + 0x284) - rf(r, q + 0x1A8)) > 0.5f;
                    static bool altSeq = false;
                    if (seq != altSeq) { altSeq = seq; std::fprintf(stderr, "[maus] %s\n", seq ? "cutscene camera: mouse layer paused" : "cutscene over"); }
                    if (seq && !g_ts2Geschuetz.load(std::memory_order_relaxed)) ego = false;
                }
            }
            float gier = 0.f, nick = 0.f, grenze = 0.f;
            if (ego)
            {
                gier = rf(r, pb + 0x148); nick = rf(r, pb + 0x14C); grenze = rf(r, kNickGrenze);
                // Own pitch limit for the mouse (original 40 degrees): TS2_NICK_MAX or bridge option 11, at most 89
                static const float envMax = [] { const char *e = std::getenv("TS2_NICK_MAX"); return e ? static_cast<float>(std::atof(e)) : 0.f; }();
                const float wunsch = envMax > 0.f ? envMax : static_cast<float>(::ps2xGsBridgeOption(11, 0));
                const float spielGrenze = grenze;
                // The game clamps the pitch to its own limit every frame (test ts2_522) -> raise the game's limit
                // while the mouse is captured; otherwise restore the original value.
                static float s_origGrenze = 0.f;
                if (s_origGrenze == 0.f && grenze > 1.f && grenze <= 90.f) s_origGrenze = grenze;
                if (s_origGrenze > 0.f)
                {
                    const float soll = (gefangen && wunsch > s_origGrenze) ? std::min(wunsch, 89.f) : s_origGrenze;
                    if (grenze != soll && grenze > 1.f && grenze <= 90.f) { wf(r, kNickGrenze, soll); grenze = soll; }
                }
                ego = gier >= 0.f && gier < 360.f && spielGrenze > 1.f && spielGrenze <= 90.f && nick >= -grenze - 1.f && nick <= grenze + 1.f;
                // Only inside a level: the player's health struct must be alive (in the menus at game start the pointer
                // already looks plausible - the crosshair showed up in the main menu)
                {
                    const uint32_t hs = ts2impact::ru(r, pb + 0x1BC) & 0x1FFFFFFu;
                    const bool lebt = hs >= 0x100000u && hs < 0x1FFF000u && rf(r, hs + 0x1A0) >= 0.2f && rf(r, hs + 0x1A0) <= 500.f &&
                                      rf(r, hs + 0x19C) > 0.f && rf(r, hs + 0x19C) <= 1000.f;
                    // (health above the maximum is allowed: in Neo Tokyo a character heals the player above it, and the mouse
                    // stayed off until the next hit - issue #8, second report)
                    if (!lebt) ego = false;
                }
            }
            // Aim mode (L2) slows walking by ~16 % (test ts2_300). So it is only held while zooming (the bridge holds L2 itself,
            // option 10) or when "always hold aim mode" is selected (option 5). Otherwise full walking speed and the
            // bridge's own crosshair (option 9 reports: first-person view with the mouse captured).
            // Mouse aiming: the game's aim mode (L2) is held all the time - only it shoots exactly where the crosshair is
            // (without it the game adds a random spread around its free-aim point). Its own wobbling aim HUD is hidden
            // (hook 0x2E18E8 below), the port's centre crosshair is the only one. TS2_MAUS_ZIELMODUS=0 = old behaviour.
            static const bool immerZiel = [] { const char *e = std::getenv("TS2_MAUS_ZIELMODUS"); return !(e && *e == '0'); }();
            bool dauer = gefangen && ego && (immerZiel || ::ps2xGsBridgeOption(5, 0) != 0);
            // TS2_ZIEL_PAUSE=a-b (tests): no permanent aim mode between these ticks (like a short menu / focus loss)
            { static const std::pair<uint64_t, uint64_t> pz = [] { const char *e = std::getenv("TS2_ZIEL_PAUSE"); uint64_t a = 0, b = 0; if (e) std::sscanf(e, "%llu-%llu", reinterpret_cast<unsigned long long *>(&a), reinterpret_cast<unsigned long long *>(&b)); return std::make_pair(a, b); }();
              if (pz.second && tick >= pz.first && tick < pz.second) dauer = false; }
            // Option 7 (zoom key held) counts too: older bridges do not know option 10 and do not hold L2 themselves.
            const bool padL2 = padZiel && ((g_skriptPadDa ? g_skriptPadRoh : static_cast<uint16_t>(g_ps2xPadRoh.load(std::memory_order_relaxed))) & 0x0100u) != 0;

            // test hook TS2_ZOOM_TEST="tick*frames,...": zoom key held for those frames (regression case scopetap)
            const bool zoomTest = [&] { static const std::vector<std::pair<uint64_t, uint64_t>> v = [] { std::vector<std::pair<uint64_t, uint64_t>> r2; if (const char *e = std::getenv("TS2_ZOOM_TEST")) { std::string q(e); size_t p2 = 0;
                    while (p2 < q.size()) { unsigned long long a = 0, n = 1; if (std::sscanf(q.c_str() + p2, "%llu*%llu", &a, &n) >= 1) r2.push_back({a, n}); p2 = q.find(',', p2); if (p2 == std::string::npos) break; ++p2; } } return r2; }();
                for (const auto &x : v) if (tick >= x.first && tick < x.first + x.second) return true; return false; }();

            // test hook TS2_WAFFE_TEST="tick:value,...": writes the weapon slot word (player +0xD48) at that tick
            { static std::vector<std::pair<uint64_t, uint32_t>> wt = [] { std::vector<std::pair<uint64_t, uint32_t>> r2; if (const char *e = std::getenv("TS2_WAFFE_TEST")) { std::string q(e); size_t p2 = 0;
                    while (p2 < q.size()) { unsigned long long a2 = 0; unsigned v2 = 0; if (std::sscanf(q.c_str() + p2, "%llu:%u", &a2, &v2) == 2) r2.push_back({a2, v2}); p2 = q.find(',', p2); if (p2 == std::string::npos) break; ++p2; } } return r2; }();
              for (const auto &x : wt) if (x.first == tick && pb) { const uint32_t ad = (pb & 0x1FFFFFFu) + 0xD48u; if (ad + 4 <= 0x2000000u) { std::memcpy(const_cast<uint8_t *>(r) + ad, &x.second, 4); std::fprintf(stderr, "[waffentest] t=%llu index %u written\n", (unsigned long long)tick, x.second); } } }
            const bool zoomTaste = ::ps2xGsBridgeOption(10, 0) != 0 || ::ps2xGsBridgeOption(7, 0) != 0 || zoomTest;
            bool zoomt = gefangen && ego && (zoomTaste || padL2);
            g_ps2xPadRStickAus.store((padZiel && ego && !g_ts2Geschuetz.load(std::memory_order_relaxed)) ? 1u : 0u, std::memory_order_relaxed);   // on a turret the right stick stays with the game
            // Sniper rifle: its aim mode IS the scope. If the scope comes up although no zoom key is held, the permanent
            // aim mode is switched off until the weapon changes (D-pad left/right = mouse wheel) or the player respawns.
            // The zoom key still brings the scope up and takes it down again.
            {
                static bool sperre = false; static uint32_t sperrePb = 0, sperreModell = 0;
                const long long jetztMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
                const bool scope = jetztMs - g_ps2xScopeMs.load(std::memory_order_relaxed) < 250;
                uint16_t pt = 0; uint8_t ax[4] = {0x80, 0x80, 0x80, 0x80};
                const bool dreieck = g_skriptPadDa ? (g_skriptPad & 0x00A0u) != 0 : (::ps2xGsBridgePad(pt, ax) && (pt & 0x00A0u) != 0);   // weapon change: D-pad right/left (mouse wheel)
                if (sperre && (pb != sperrePb || dreieck)) sperre = false;
                if (dreieck) g_ts2WechselTick = tick;   // weapon change pressed: the aim mode pauses (see below)

                // scope slot memory, off by default (TS2_SCOPE_SLOT=1 on): +0xD48 turned out to be the weapon class, which
                // several weapons share - the AK stayed zoomed after the sniper rifle (user report 15 Nov)
                static const bool slotAn = [] { const char *e = std::getenv("TS2_SCOPE_SLOT"); return e && *e == '1'; }();
                static uint64_t scopeSlots = 0; static uint32_t slotsPb = 0; static uint64_t zoomSeit = 0;
                if (pb != slotsPb) { scopeSlots = 0; slotsPb = pb; }
                const uint32_t slot = (ego && pb) ? ts2impact::ru(r, pb + 0xD48) : 64u;
                const bool slotOk = slotAn && slot < 64u;
                if (!sperre && dauer && !zoomt && scope)
                {
                    sperre = true; sperrePb = pb; sperreModell = 0;
                    if (slotOk) scopeSlots |= (1ull << slot);
                    std::fprintf(stderr, "[maus] scope weapon (slot %u): permanent aim mode off until weapon change\n", slot);
                }
                if (slotOk && (scopeSlots >> slot) & 1ull)
                {
                    static uint32_t altSlot = 64u;
                    if (altSlot != slot) std::fprintf(stderr, "[maus] slot %u is a scope weapon: no permanent aim mode\n", slot);
                    altSlot = slot;
                    if (zoomt && !scope) { if (!zoomSeit) zoomSeit = tick; else if (tick - zoomSeit > 40) { scopeSlots &= ~(1ull << slot); zoomSeit = 0; std::fprintf(stderr, "[maus] slot %u: no scope with the zoom key - forgotten\n", slot); } }
                    else zoomSeit = 0;
                    if ((scopeSlots >> slot) & 1ull) dauer = false;
                }
                else zoomSeit = 0;
                if (sperre) dauer = false;

                // weapon in hand: player +0x584 -> gun object, +8 = index into the model table 0x5410C0 (<name>_ph.raw, all 30
                // weapons by name). Scope weapons never get the permanent aim mode (TS2_WAFFENTAB=0 turns this rule off).
                {
                    static const bool tabAn = [] { const char *e = std::getenv("TS2_WAFFENTAB"); return !(e && *e == '0'); }();
                    struct W { uint16_t modell; const char *name; bool scope; };
                    static const W kWaffen[] = {
                        {978, "defaultgun", false}, {981, "assaultshotgun", false}, {984, "silencedpistol", false}, {987, "electrogun", false},
                        {990, "flamethrower", false}, {993, "sniperrifle", true}, {996, "sniperbolt", true}, {999, "ak47", false},
                        {1002, "fireext", false}, {1007, "laser", false}, {1010, "plasmamachinegun", false}, {1013, "rocketlauncher", false},
                        {1016, "homingrocketlauncher", false}, {1019, "grenadelauncher", false}, {1022, "remotemine", false}, {1025, "timedmine", false},
                        {1028, "proxmine", false}, {1034, "tnt", false}, {1037, "tommygun", false}, {1040, "doublebarrelshotgun", false},
                        {1043, "magnum", false}, {1046, "scifihandgun", false}, {1049, "uzi", false}, {1052, "silencedluger", false},
                        {1055, "luger", false}, {1058, "mapboy", false}, {1061, "crossbow", false}, {1064, "minigun", false},
                        {1067, "powderkeg", false}, {1070, "brick", false}};
                    uint32_t modell = 0;
                    bool sonder = false;
                    if (ego && pb) { const uint32_t g = ts2impact::ru(r, pb + 0x584) & 0x1FFFFFFu; if (g >= 0x100000u && g < 0x1FFFF00u) { modell = ts2impact::ru(r, g + 8);
                        const uint32_t ix = ts2impact::ru(r, g + 0x30); sonder = modell == 0xFFFFFFFFu || (ix < 38u && ts2impact::ru(r, 0x004C3818u + 0x240u * ix) == 0xFFFFFFFFu); } }
                    // the scope lock ends with any weapon change, also one the pad check above did not see (mouse wheel)
                    if (sperre && ego && modell != 0u) { if (!sperreModell) sperreModell = modell; else if (modell != sperreModell) { sperre = false; std::fprintf(stderr, "[maus] weapon changed: scope lock released\n"); } }
                    const W *w = nullptr;
                    for (const W &k : kWaffen) if (k.modell == modell) { w = &k; break; }
                    static uint32_t altModell = ~0u;
                    if (modell != altModell && ego) { altModell = modell; std::fprintf(stderr, "[maus] weapon model %u (%s)%s\n", modell, w ? w->name : "none/unknown", (w && w->scope) ? ": scope weapon, no permanent aim mode" : ""); }

                    // turret / mounted gun (issue #6): no permanent aim mode, so no held L2 and no press edge - with mouse
                    // aiming the character got off the turret right away. TS2_GESCHUETZ=0 = old behaviour.
                    { static const bool gAn = [] { const char *e = std::getenv("TS2_GESCHUETZ"); return !(e && *e == '0'); }();
                      static bool altSonder = false;
                      if (sonder != altSonder && ego) { altSonder = sonder; std::fprintf(stderr, "[maus] %s\n", sonder ? "mounted gun: no permanent aim mode" : "mounted gun left"); }
                      if (gAn && sonder) dauer = false;
                      g_ts2Geschuetz.store(gAn && sonder, std::memory_order_relaxed); }

                    // the game's weapon table 0x4C3818 (0x240 bytes per entry, 38 entries): +0x00 model, +0x08 flags (0x20 = telescopic
                    // sight), +0x54 field of view while the aim mode is held (60 = no zoom, S47 40, Sniper 17, Vintage Rifle 10).
                    // While only the port holds the aim mode (mouse aiming, no zoom key) it is kept at 60; the original value comes
                    // back with the zoom key or a weapon change (TS2_ZIEL_FOV=0 off). Fixes the S47 staying zoomed in.
                    {
                        constexpr uint32_t kTab = 0x004C3818u, kGross = 0x240u; constexpr int kAnz = 38;
                        static const bool fovAn = [] { const char *e = std::getenv("TS2_ZIEL_FOV"); return !(e && *e == '0'); }();
                        static float orig[kAnz]; static bool origDa = false;
                        if (!origDa) { for (int i = 0; i < kAnz; ++i) orig[i] = ts2impact::rf(r, kTab + kGross * i + 0x54); origDa = true; }
                        int e = -1;
                        if (modell >= 200 && modell < 2000) for (int i = 0; i < kAnz; ++i) if (ts2impact::ru(r, kTab + kGross * i) == modell) { e = i; break; }
                        bool tabScope = w && w->scope;
                        if (e >= 0) tabScope = (ts2impact::ru(r, kTab + kGross * e + 8) & 0x20u) != 0;
                        if (tabAn && tabScope) dauer = false;
                        // Weapon change (user report 16 Nov: from the pistol to the Sniper Rifle the scope came up at once):
                        // the permanent aim mode of the old weapon was still held while the game already switched, and with
                        // a scope weapon a held aim mode IS the scope. While a change is pressed or the held model changes,
                        // the aim mode pauses for 45 VBlanks; a scope weapon then never gets it, any other weapon gets it
                        // back with a fresh press edge. TS2_WECHSEL_PAUSE=0 = old behaviour.
                        {
                            static const bool wpAn = [] { const char *e = std::getenv("TS2_WECHSEL_PAUSE"); return !(e && *e == '0'); }();
                            static uint32_t wpModell = 0;
                            if (modell != wpModell) { if (wpModell != 0) g_ts2WechselTick = tick; wpModell = modell; }
                            // Player +0x574 = weapon switch state (measured): 1 = weapon held, 2 lowering, 3 lowered,
                            // 0 empty hands, 5/6 raising. It leaves 1 about 20 VBlanks before the held model changes, also
                            // when the game switches by itself after a pickup (AK and Sniper Rifle went into their aim view
                            // on the first pickup, user report 16 Nov). Firing and reloading leave it at 1.
                            if (pb && ts2impact::ru(r, pb + 0x574) != 1u) g_ts2WechselTick = tick;
                            if (wpAn && tick - g_ts2WechselTick < 45u) dauer = false;
                        }
                        static int altE = -1;
                        if (fovAn)
                        {
                            if (altE >= 0 && altE != e) ts2impact::wf(r, kTab + kGross * altE + 0x54, orig[altE]);
                            if (e >= 0 && orig[e] > 1.f && orig[e] < 60.f && !tabScope)
                            {
                                const float soll = (dauer && !zoomt) ? 60.f : orig[e];
                                if (ts2impact::rf(r, kTab + kGross * e + 0x54) != soll)
                                {
                                    ts2impact::wf(r, kTab + kGross * e + 0x54, soll);
                                    static int lg = 12; if (lg > 0) { --lg; std::fprintf(stderr, "[maus] aim field of view of weapon %u: %.1f (original %.1f)\n", modell, soll, orig[e]); }
                                }
                            }
                            altE = e;
                        }
                    }

                    // TS2_WAFFENLOG=1: weapon state every 10 frames ([wlog] lines)
                    { static const bool wlog = std::getenv("TS2_WAFFENLOG") != nullptr;
                      if (wlog && ego && pb && tick % 10u == 0u)
                      { const long long nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
                        std::fprintf(stderr, "[wlog] t=%llu m=%u %s fov=%.3f a0=%u a8=%u dauer=%d zoom=%d scope=%d\n", (unsigned long long)tick, modell, w ? w->name : "?",
                                     ts2impact::rf(r, pb + 0x914), ts2impact::ru(r, pb + 0x8A0), ts2impact::ru(r, pb + 0x8A8), dauer ? 1 : 0, zoomt ? 1 : 0,
                                     (nowMs - g_ps2xScopeMs.load(std::memory_order_relaxed) < 250) ? 1 : 0); } }

                    // tap to scope (TS2_SCOPE_TIPP=0 off): a tap on the zoom key opens the scope of a scope weapon, the next tap closes
                    // it; holding the key longer than 20 frames closes it on release like before
                    {
                        static const bool tippAn = [] { const char *e = std::getenv("TS2_SCOPE_TIPP"); return !(e && *e == '0'); }();
                        static bool latch = false, altTaste = false; static uint64_t gedruecktSeit = 0; static uint32_t latchPb = 0, latchModell = 0;
                        const bool taste = gefangen && ego && zoomTaste;
                        if (tippAn && tabAn && w && w->scope && gefangen && ego && pb == latchPb && modell == latchModell)
                        {
                            if (taste && !altTaste) { latch = !latch; gedruecktSeit = tick; }
                            if (!taste && altTaste && latch && tick - gedruecktSeit > 20) latch = false;
                            zoomt = latch || taste || padL2;
                        }
                        else latch = false;
                        latchPb = pb; latchModell = modell; altTaste = taste;
                    }
                }
            }
            const bool zielmodus = dauer || zoomt;
            // A held aim mode (L2) never gives the game a new press: with the sniper rifle the scope only opens on an
            // L2 press edge, so the zoom key / aim key did nothing (test sn713). When the player
            // presses the zoom key or the aim key himself while L2 is already held, L2 is released for 3 VBlanks.
            {
                static bool altZoom = false, altL2 = false, altGehalten = false; static int loslassen = 0;
                const bool roh = ((g_skriptPadDa ? g_skriptPadRoh : static_cast<uint16_t>(g_ps2xPadRoh.load(std::memory_order_relaxed))) & 0x0100u) != 0;
                if (altGehalten && ((zoomt && !altZoom) || (roh && !altL2))) loslassen = 3;
                // The permanent aim mode needs a press edge too: L2 held from the first tick of a life was not taken by the
                // game - the accurate aim only worked after the player had pressed the zoom key once (user report 14 Nov).
                // So when the permanent aim mode starts (level start, respawn = new player object, mouse captured again)
                // L2 is released for 3 VBlanks, and once more 1.5 s later in case the game was not ready for input yet.
                {
                    static bool altDauer = false; static uint32_t altDauerPb = 0; static uint64_t nochmal = 0;
                    if (dauer && (!altDauer || pb != altDauerPb)) { loslassen = 3; nochmal = tick + 90; }
                    else if (dauer && nochmal && tick >= nochmal) { if (!zoomt) loslassen = 3; nochmal = 0; }
                    if (!dauer) nochmal = 0;
                    altDauer = dauer; altDauerPb = pb;
                }
                altZoom = zoomt; altL2 = roh; altGehalten = zielmodus || roh;
                // D-pad up/down zoom the view in steps while the aim mode is held (the game's own control). With the
                // permanent aim mode they zoomed in whenever the D-pad was touched while switching weapons (Steam Deck
                // video 16 Nov: AK zoomed in after cycling with the D-pad). They only pass while the zoom key is held.
                // TS2_DPAD_ZOOM=1 = old behaviour.
                static const bool dpadAlt = [] { const char *e = std::getenv("TS2_DPAD_ZOOM"); return e && *e == '1'; }();
                const uint32_t dpadMaske = (!dpadAlt && dauer && !zoomt) ? 0x0050u : 0u;   // PS2 pad: 0x10 up, 0x40 down
                g_ps2xPadMaske.store((loslassen > 0 ? 0x0100u : 0u) | dpadMaske, std::memory_order_relaxed);
                if (loslassen > 0) --loslassen;
            }
            ::ps2xGsBridgeOption(9, (gefangen && ego && !padL2) ? 1 : 0);   // while zooming with the gamepad the game shows its own aim HUD / scope
            g_ps2xPadZusatz.store(zielmodus ? 0x0100u : 0u, std::memory_order_relaxed);
            g_ts2EigenesZiel.store(dauer && !zoomt, std::memory_order_relaxed);
            g_ts2MausZielt.store(gefangen && ego, std::memory_order_relaxed);
            // diagnosis in play.log (every ~10 s): mouse state for crosshair / aim mode problems
            if (tick % 600u == 0u)
                std::fprintf(stderr, "[maus] gefangen=%d ego=%d dauer=%d zoom=%d (opt10=%d opt7=%d) hudAus=%d\n", gefangen ? 1 : 0, ego ? 1 : 0, dauer ? 1 : 0, zoomt ? 1 : 0,
                             ::ps2xGsBridgeOption(10, 0), ::ps2xGsBridgeOption(7, 0), (dauer && !zoomt) ? 1 : 0);
            // TS2_POSLOG=1 (tests): player position every 10 ticks (walking speed in aim mode)
            static const bool poslog = std::getenv("TS2_POSLOG") != nullptr;
            if (poslog && ego && pb && tick % 10u == 0u)
                std::fprintf(stderr, "[pos] t=%llu x=%.3f z=%.3f l2=%d\n", (unsigned long long)tick, rf(r, pb + 0x1A0), rf(r, pb + 0x1A8), (dauer || zoomt) ? 1 : 0);
            // Permanent aim mode (mouse aiming) must not zoom: some weapons (Soviet S47 and others) zoom the view a little
            // as soon as the aim mode is held - with the mouse it is held all the time, so the view stayed zoomed in.
            // The view scale is the player float +0x914 (test fov856: writing 0.5 zooms the picture). Its normal value is
            // taken whenever the aim mode is not held (and at the first tick of a new player object); while the aim mode
            // is only held by the port (no zoom key) the scale is put back to that value. The zoom key zooms as before.
            // TS2_ZIEL_ZOOM=1 = old behaviour.
            // Note (1.1.1): +0x914 turned out to be only the zoom animation (0.91 -> 0 while the scope opens), not the view
            // scale; the S47 fix that works is the field of view hold on the weapon table above. This part is kept as it is.
            {
                static const bool alt = [] { const char *e = std::getenv("TS2_ZIEL_ZOOM"); return e && *e == '1'; }();
                static float normal = 0.f; static uint32_t normalPb = 0; static bool altEgo = false;
                if (ego && pb)
                {
                    const float v = rf(r, pb + 0x914);
                    const bool plausibel = v > 0.3f && v < 3.0f;
                    if (pb != normalPb || !altEgo || !zielmodus) { if (plausibel) { normal = v; normalPb = pb; } }
                    else if (!alt && dauer && !zoomt && normal > 0.f && plausibel && v < normal - 0.001f)
                    {
                        static long lg = [] { const char *e = std::getenv("TS2_ZIEL_ZOOM_LOG"); return e ? std::atol(e) : 0L; }();
                        if (lg > 0) { --lg; std::fprintf(stderr, "[aimzoom] t=%llu view scale %.3f -> %.3f\n", (unsigned long long)tick, v, normal); }
                        wf(r, pb + 0x914, normal);
                    }
                }
                altEgo = ego && pb;
            }
            // TS2_FOV_TEST=value (tests): write the player float +0x914 (candidate: view/zoom scale, 0.93 -> 0.2 while zooming)
            if (const char *fe = std::getenv("TS2_FOV_TEST"); fe && *fe && ego && pb)
                wf(r, pb + 0x914, static_cast<float>(std::atof(fe)));
            // TS2_FOVPROBE=1 (tests): float words of the player object (+0..0x1000) in 5..170 that change (search for the
            // view angle: the sniper zoom changes it)
            static const bool fovprobe = std::getenv("TS2_FOVPROBE") != nullptr;
            static const std::pair<float, float> fovBereich = [] { float a = 5.f, b = 170.f; if (const char *e = std::getenv("TS2_FOVPROBE")) std::sscanf(e, "%f:%f", &a, &b); return std::make_pair(a, b); }();
            if (fovprobe && ego && pb && tick % 20u == 0u)
            {
                static std::vector<float> altF; static uint32_t altFPb = 0;
                std::vector<float> neuF(0x1000 / 4);
                for (size_t i = 0; i < neuF.size(); ++i) neuF[i] = rf(r, pb + static_cast<uint32_t>(i * 4));
                if (altFPb == pb && altF.size() == neuF.size())
                {
                    std::string z;
                    for (size_t i = 0; i < neuF.size(); ++i)
                        if (neuF[i] != altF[i] && std::fabs(neuF[i]) >= fovBereich.first && std::fabs(neuF[i]) <= fovBereich.second && std::fabs(altF[i]) >= fovBereich.first && std::fabs(altF[i]) <= fovBereich.second)
                        { char b2[48]; std::snprintf(b2, sizeof b2, " +%03zx:%.2f>%.2f", i * 4, altF[i], neuF[i]); z += b2; }
                    if (!z.empty()) std::fprintf(stderr, "[fov] t=%llu%s\n", (unsigned long long)tick, z.c_str());
                }
                altF = std::move(neuF); altFPb = pb;
            }
            // TS2_WAFFENPROBE=1 (tests): words of the player object that change, small values only (weapon slot search)
            static const bool probe = std::getenv("TS2_WAFFENPROBE") != nullptr;
            if (probe && ego && pb && tick % 30u == 0u)
            {
                static std::vector<uint32_t> alt; static uint32_t altPb = 0;
                std::vector<uint32_t> neu(0x1000 / 4);
                for (size_t i = 0; i < neu.size(); ++i) neu[i] = ts2impact::ru(r, pb + static_cast<uint32_t>(i * 4));
                if (altPb == pb && alt.size() == neu.size())
                {
                    std::string z;
                    for (size_t i = 0; i < neu.size(); ++i)
                        if (neu[i] != alt[i] && neu[i] < 64u && alt[i] < 64u) { char b2[40]; std::snprintf(b2, sizeof b2, " +%03zx:%u>%u", i * 4, alt[i], neu[i]); z += b2; }
                    if (!z.empty()) std::fprintf(stderr, "[waffe] t=%llu%s\n", (unsigned long long)tick, z.c_str());
                }
                alt = std::move(neu); altPb = pb;
            }
            static bool s_aktiv = false; static float s_gier = 0.f, s_nick = 0.f;
            if (!gefangen || !ego) { s_aktiv = false; g_ts2PunchDP = g_ts2PunchDY = 0.f; g_ps2xPadRStickSetzen.store(0u, std::memory_order_relaxed); return; }
            // On a turret the game turns through the right stick, not through the player's view angles (issue #8): there the
            // mouse movement becomes right stick input (runtime g_ps2xPadRStickSetzen), view angles and aim point stay with the game.
            {
                static float stX = 0.f, stY = 0.f;
                if (g_ts2Geschuetz.load(std::memory_order_relaxed) && gefangen && !padZiel)
                {
                    const float sens = static_cast<float>(::ps2xGsBridgeOption(3, 30)) / 10.f;
                    const bool invert = ::ps2xGsBridgeOption(4, 0) != 0;
                    stX = std::clamp(stX * 0.55f + static_cast<float>(dx) * sens * 1.2f, -127.f, 127.f);
                    stY = std::clamp(stY * 0.55f + static_cast<float>(invert ? -dy : dy) * sens * 1.2f, -127.f, 127.f);
                    const unsigned rx = static_cast<unsigned>(128.f + stX) & 0xFFu, ry = static_cast<unsigned>(128.f + stY) & 0xFFu;
                    g_ps2xPadRStickSetzen.store(0x10000u | rx | (ry << 8), std::memory_order_relaxed);
                    s_aktiv = false;
                    return;
                }
                stX = stY = 0.f; g_ps2xPadRStickSetzen.store(0u, std::memory_order_relaxed);
            }
            wf(r, pb + 0x8B8, 0.f);   // aim point fixed at the screen centre (the game's free-aim offset moved it away from the crosshair)
            wf(r, pb + 0x8BC, 0.f);
            // The camera belongs to the mouse: small changes by the game (auto look-ahead pulls the view towards the
            // horizon while walking) are undone, large ones (recoil, script, death) are accepted.
            if (s_aktiv)
            {
                // Everything the game turns by itself is undone: auto look-ahead (pull towards the horizon) and above all the
                // auto-aim while firing, which dragged the view towards enemies ("magnetic" mouse). Only big jumps
                // (respawn, scripted camera, death) are accepted. The port's own impact kick is kept.
                s_nick += g_ts2PunchDP; s_gier += g_ts2PunchDY; g_ts2PunchDP = g_ts2PunchDY = 0.f;
                const float dn = nick - s_nick;
                float dg = gier - s_gier; while (dg > 180.f) dg -= 360.f; while (dg < -180.f) dg += 360.f;
                if (std::fabs(dn) < 20.f && std::fabs(dg) < 30.f) { nick = std::clamp(s_nick, -grenze, grenze); gier = s_gier; while (gier < 0.f) gier += 360.f; while (gier >= 360.f) gier -= 360.f; }
            }
            s_aktiv = true;
            if (dx != 0 || dy != 0)
            {
                // Sensitivity in tenths (menu: 0.1-10.0, default 3.0) -> degrees per mouse count as in PC shooters;
                // while zooming (right mouse button) additionally the zoom sensitivity in %
                const float sens = static_cast<float>(::ps2xGsBridgeOption(3, 30)) / 10.f;
                const bool invert = ::ps2xGsBridgeOption(4, 0) != 0;
                float zoom = ::ps2xGsBridgeOption(7, 0) ? static_cast<float>(::ps2xGsBridgeOption(6, 50)) / 100.f : 1.f;
                // Zoom scaled sensitivity (GitHub issue #13): while the view is zoomed in (aim mode, scope, sniper zoom
                // steps) the turn per mouse count follows the real field of view, tan(fov/2) / tan(base/2), so the
                // picture moves the same distance per count at every zoom level. Camera = *(0x59DB7C), +0x324 current
                // field of view in degrees. The zoom sensitivity from the menu (default 50 %) stays a factor on top.
                {
                    uint32_t cam = 0; std::memcpy(&cam, r + 0x0059DB7Cu, 4); cam &= 0x1FFFFFFu;
                    if (cam >= 0x100000u && cam < 0x1FFF000u)
                    {
                        float fov = 0.f; std::memcpy(&fov, r + cam + 0x324, 4);
                        const float basis = 60.f;
                        if (fov > 1.f && fov < basis - 0.5f)
                            zoom = std::tan(fov * 0.008726646f) / std::tan(basis * 0.008726646f) * (static_cast<float>(::ps2xGsBridgeOption(6, 50)) / 50.f);
                    }
                }
                const float k = 0.022f * sens * zoom;
                gier -= static_cast<float>(dx) * k;
                nick += static_cast<float>(invert ? dy : -dy) * k;
                while (gier < 0.f) gier += 360.f;
                while (gier >= 360.f) gier -= 360.f;
                nick = std::clamp(nick, -grenze, grenze);
            }
            if (padX != 0.f || padY != 0.f)
            {
                // full deflection: 200 degrees/s sideways, 130 up/down (TS2_PAD_SENS in percent); slower while zooming
                static const float envSens = [] { const char *e = std::getenv("TS2_PAD_SENS"); return e ? static_cast<float>(std::atof(e)) : 100.f; }();
                const float padSens = std::clamp(static_cast<float>(::ps2xGsBridgeOption(33, static_cast<int>(envSens))), 20.f, 400.f) / 100.f;
                const float zf = padL2 ? static_cast<float>(::ps2xGsBridgeOption(6, 50)) / 100.f : 1.f;
                const bool invert = ::ps2xGsBridgeOption(4, 0) != 0;
                gier -= padX * (200.f / 60.f) * padSens * zf;
                nick += (invert ? padY : -padY) * (130.f / 60.f) * padSens * zf;
                while (gier < 0.f) gier += 360.f;
                while (gier >= 360.f) gier -= 360.f;
                nick = std::clamp(nick, -grenze, grenze);
            }
            wf(r, pb + 0x148, gier);
            wf(r, pb + 0x14C, nick);
            s_gier = gier; s_nick = nick;
        }

        // ---- Free camera (dev mode, bridge option 8 / F7 / TS2_FREECAM=1) ----
        // Player position in the player object: +0x1A0/+0x1A4/+0x1A8 (x, eye height ~1.62, z), ground copy +0x194..+0x19C,
        // template +0xF0..+0xF8, walking speed +0x1B4/+0x1B8. Forward = (sin(yaw), 0, cos(yaw)) (RAM diff ts2_344/346).
        // The position is set anew every frame; game input is frozen (pad override) so that the character
        // neither walks nor shoots. The view direction still comes from the mouse (above).
        void flug(uint8_t *r, uint64_t tick)
        {
            static const bool envAn = [] { const char *e = std::getenv("TS2_FREECAM"); return e && *e == '1'; }();
            static const uint64_t ab = [] { const char *e = std::getenv("TS2_FREECAM_AB"); return e && *e ? std::strtoull(e, nullptr, 10) : 0ull; }();
            const bool an = (envAn && tick >= ab) || ::ps2xGsBridgeOption(8, 0) != 0;
            static bool war = false;
            static float px = 0.f, py = 0.f, pz = 0.f;
            uint32_t pb = 0; std::memcpy(&pb, r + kSpielerZeiger, 4);
            // Only inside a level (pitch limit set, view angles valid) - the player object also exists in the title/menu
            const float grenze = rf(r, kNickGrenze);
            const bool ego = pb >= 0x100000u && pb < 0x1FF0000u && rf(r, pb + 0x148) >= 0.f && rf(r, pb + 0x148) < 360.f && grenze > 1.f && grenze <= 90.f;
            if (!an || !ego)
            {
                if (war) { ps2_stubs::clearPadOverrideState(); war = false; std::fprintf(stderr, "[TS2] free camera off\n"); }
                return;
            }
            if (!war)
            {
                px = rf(r, pb + 0x1A0); py = rf(r, pb + 0x1A4); pz = rf(r, pb + 0x1A8);
                std::fprintf(stderr, "[TS2] free camera on at (%.1f %.1f %.1f)\n", px, py, pz);
                war = true;
            }
            // Input: left stick / WASD (forward/sideways), Cross (Space/E) up, L1 (Ctrl/C) down, L2 (Shift) fast
            uint16_t pt = 0; uint8_t ax[4] = {0x80, 0x80, 0x80, 0x80};
            if (g_skriptPadDa) { pt = g_skriptPad; std::memcpy(ax, g_skriptAx, 4); }
            else ::ps2xGsBridgePad(pt, ax);
            float vor = -(static_cast<float>(ax[1]) - 128.f) / 128.f, seit = (static_cast<float>(ax[0]) - 128.f) / 128.f;
            if (std::fabs(vor) < 0.15f) vor = 0.f;
            if (std::fabs(seit) < 0.15f) seit = 0.f;
            float hoch = ((pt & 0x4000u) ? 1.f : 0.f) - ((pt & 0x0400u) ? 1.f : 0.f);
            static const float tempoBasis = [] { const char *e = std::getenv("TS2_FREECAM_TEMPO"); return e && *e ? static_cast<float>(std::atof(e)) : 0.12f; }();
            const float tempo = tempoBasis * ((pt & 0x0100u) ? 4.f : 1.f);
            const float gier = rf(r, pb + 0x148) * 3.14159265f / 180.f, nick = rf(r, pb + 0x14C) * 3.14159265f / 180.f;
            // View direction (with pitch), sideways at a right angle in the plane
            const float fx = std::sin(gier) * std::cos(nick), fy = std::sin(nick), fz = std::cos(gier) * std::cos(nick);
            const float sx = std::cos(gier), sz = -std::sin(gier);
            px += (fx * vor + sx * seit) * tempo;
            py += (fy * vor + hoch) * tempo;
            pz += (fz * vor + sz * seit) * tempo;
            wf(r, pb + 0x1A0, px); wf(r, pb + 0x1A4, py); wf(r, pb + 0x1A8, pz);
            wf(r, pb + 0x194, px); wf(r, pb + 0x198, py - 1.62f); wf(r, pb + 0x19C, pz);
            wf(r, pb + 0x0F0, px); wf(r, pb + 0x0F4, py - 1.62f); wf(r, pb + 0x0F8, pz);
            wf(r, pb + 0x1B4, 0.f); wf(r, pb + 0x1B8, 0.f);
            // Camera copies at fixed addresses (RAM search ts2_350): eye point (y = ground + 1.62) and ground point; without them
            // the height stays glued to the ground because the game recomputes the eye point from ground contact every frame
            static const bool kopien = [] { const char *e = std::getenv("TS2_FREECAM_KOPIEN"); return !(e && *e == '0'); }();
            if (kopien)
            {
                for (uint32_t a : {0x784708u, 0x784750u, 0x7E82B0u, 0x7E837Cu})
                { wf(r, a, px); wf(r, a + 4, py); wf(r, a + 8, pz); }
                for (uint32_t a : {0x7849A8u, 0x7849F0u, 0x7B2368u, 0x7B23B0u})
                { wf(r, a, px); wf(r, a + 4, py - 1.62f); wf(r, a + 8, pz); }
            }
            // Freeze game input (no walking, no firing); the mouse view keeps running through tick()
            ps2_stubs::setPadOverrideState(0xFFFFu, 0x80, 0x80, 0x80, 0x80);
            (void)tick;
        }
    }

    void ts2WidescreenTick(uint8_t *rdram)
    {
        static const bool an = [] { const char *e = std::getenv("TS2_WIDESCREEN"); return e && *e == '1'; }();
        if (!rdram) return;
        // TS2_POKE="address=value[/16],..." (hex, for test runs only): set memory words every frame, e.g. the
        // unlocks from the CodeBreaker codes (decrypted): 5cc268/5cc26c/5cc270 story Easy/Normal/Hard,
        // 5cc280/16 challenges, 59d6fc "unlock everything".
        {
            struct Poke { uint32_t ad, val; bool half; };
            static const std::vector<Poke> pokes = [] {
                std::vector<Poke> v; const char *e = std::getenv("TS2_POKE");
                for (const char *p = e; p && *p;)
                {
                    char *end = nullptr; Poke k{static_cast<uint32_t>(std::strtoul(p, &end, 16)), 0u, false};
                    if (end && *end == '=') k.val = static_cast<uint32_t>(std::strtoul(end + 1, &end, 16));
                    if (end && *end == '/') { k.half = std::strtoul(end + 1, &end, 10) == 16; }
                    if (k.ad && k.ad + 4 <= 0x2000000u) v.push_back(k);
                    p = (end && *end == ',') ? end + 1 : nullptr;
                }
                return v; }();
            for (const auto &k : pokes) { if (k.half) { const uint16_t h = static_cast<uint16_t>(k.val); std::memcpy(rdram + k.ad, &h, 2); } else std::memcpy(rdram + k.ad, &k.val, 4); }
        }
        // "Unlock everything" (settings menu, Save data tab; TS2_UNLOCK_ALL=1 for test runs): the game's own switch at
        // 0x59d6fc opens all levels, characters and cheats while it is set. It is not part of the profile, so the
        // player's own progress stays as it is; switching the option off restores the normal state.
        {
            static const int envAn = [] { const char *e = std::getenv("TS2_UNLOCK_ALL"); return (e && *e == '1') ? 1 : 0; }();
            static bool gesetzt = false; static uint32_t zaehler = 0; static int frei = 0;
            if ((zaehler++ & 31u) == 0) frei = ::ps2xGsBridgeOption(31, envAn);
            if (frei) { if (!gesetzt) { uint32_t w; std::memcpy(&w, rdram + 0x0059d6fcu, 4); std::fprintf(stderr, "[TS2] unlock everything: on (switch was %08x)\n", w); } rdram[0x0059d6fcu] = 1; gesetzt = true; }
            else if (gesetzt) { rdram[0x0059d6fcu] = 0; gesetzt = false; }
        }
        // Ultrawide (bridge aspect option "any window shape"): the projection follows the window, A = width/height.
        // Divisor = 4/A (3.0 at 4:3, 2.25 at 16:9) and the aspect constants = A * 7/8 (1.1667 at 4:3, 1.5556 at 16:9).
        // Any window from 4:3 to 32:9 (16:10 included); outside that range 16:9 stays. Read every frame: resizing the window takes effect at once.
        float breit = 0.f;
        if (an) { const int m = ::ps2xGsBridgeAspectMilli(); if (m > 1250 && m < 6000) breit = static_cast<float>(m) / 1000.f; }
        // Divisor of the hard-coded 4:3 projections (instruction patch in tools/ts2_rec_elf.py): 3.0 = original, 2.25 = 16:9
        if (breit > 0.f) { const float d = 4.f / breit; std::memcpy(rdram + 0x0059e9f0u, &d, 4); }
        else { const uint32_t v = an ? 0x40100000u : 0x40400000u; std::memcpy(rdram + 0x0059e9f0u, &v, 4); }
        // Letterbox in cutscenes (instruction patch, values as in the original or the PCSX2 patch "No Cutscene Letterbox").
        // TS2_LETTERBOX=1 bars as in the original, 0 no bars; default: no bars at 16:9, original at 4:3.
        {
            static const bool balken = [] { const char *e = std::getenv("TS2_LETTERBOX"); const char *w = std::getenv("TS2_WIDESCREEN");
                                            if (e && *e) return *e == '1'; return !(w && *w == '1'); }();
            static const uint16_t orig[7] = {0x40, 0x180, 0x200, 0x40, 0x180, 0x200, 0x1C0};
            static const uint16_t ohne[7] = {0x00, 0x1C0, 0x000, 0x00, 0x000, 0x000, 0x000};
            std::memcpy(rdram + 0x0059e9d0u, balken ? orig : ohne, sizeof(orig));
        }
        // Temporal Uplink in hand (issue #12): player = *(0x59DC0C), weapon object +0x584, model +8 (1058 = uplink).
        // The runtime keeps the uplink's map out of the widescreen HUD squeeze while it is held.
        {
            uint32_t pb = 0, g = 0, m = 0; std::memcpy(&pb, rdram + 0x0059DC0Cu, 4); pb &= 0x1FFFFFFu;
            if (pb >= 0x100000u && pb < 0x1FF0000u) { std::memcpy(&g, rdram + pb + 0x584, 4); g &= 0x1FFFFFFu; if (g >= 0x100000u && g < 0x1FFFF00u) std::memcpy(&m, rdram + g + 8, 4); }
            g_ps2xUplink.store(m == 1058u, std::memory_order_relaxed);
        }
        if (!an) return;
        static const uint32_t adr[] = {0x005995dc, 0x00598a08, 0x00599664, 0x0059a304, 0x0059a308, 0x0059a39c, 0x0059a3ac, 0x0059ca10};
        { const float k = breit > 0.f ? breit * 0.875f : 0.f; uint32_t v = 0x3fc71c71u; if (k > 0.f) std::memcpy(&v, &k, 4);
          for (uint32_t a : adr) std::memcpy(rdram + a, &v, 4); }
        // TS2_WS_EXTRA="address[=value],..." (hex): set further data words every frame (search for HUD/weapon constants)
        static const std::vector<std::pair<uint32_t, uint32_t>> extra = [] {
            std::vector<std::pair<uint32_t, uint32_t>> v; const char *e = std::getenv("TS2_WS_EXTRA");
            for (const char *p = e; p && *p;)
            {
                char *end = nullptr; const uint32_t ad = static_cast<uint32_t>(std::strtoul(p, &end, 16)); uint32_t val = 0x3fc71c71u;
                if (end && *end == '=') val = static_cast<uint32_t>(std::strtoul(end + 1, &end, 16));
                if (ad && ad + 4 <= 0x2000000u) v.emplace_back(ad, val);
                p = (end && *end == ',') ? end + 1 : nullptr;
            }
            return v; }();
        for (const auto &[ad, val] : extra) std::memcpy(rdram + ad, &val, 4);
    }
    void ts2InputTickScript(uint64_t tick, const char *envName, int port);
    // TS2_EINGABE_AUFNAHME=file: record real pad input per VBlank (only on change: "tick buttons lx ly rx ry",
    // buttons 1 = pressed). TS2_EINGABE_WIEDERGABE=file plays it back through the pad override (reproduce bugs).
    void ts2EingabeAufnahme(uint64_t tick)
    {
        static const char *rep = std::getenv("TS2_EINGABE_WIEDERGABE");
        static const char *rec = std::getenv("TS2_EINGABE_AUFNAHME");
        if (rep && *rep)
        {
            struct E { uint64_t t; uint16_t p; uint8_t a[4]; };
            static std::vector<E> ev = [] {
                std::vector<E> v; if (std::FILE *f = std::fopen(rep, "r")) { unsigned long long t; unsigned p, a0, a1, a2, a3;
                    while (std::fscanf(f, "%llu %x %x %x %x %x", &t, &p, &a0, &a1, &a2, &a3) == 6)
                        v.push_back({t, uint16_t(p), {uint8_t(a0), uint8_t(a1), uint8_t(a2), uint8_t(a3)}}); std::fclose(f); }
                std::fprintf(stderr, "[TS2] input playback: %zu entries\n", v.size()); return v; }();
            static size_t i = 0;
            while (i + 1 < ev.size() && ev[i + 1].t <= tick) ++i;
            if (!ev.empty() && ev[0].t <= tick)
                ps2_stubs::setPadOverrideState(static_cast<uint16_t>(~ev[i].p), ev[i].a[0], ev[i].a[1], ev[i].a[2], ev[i].a[3]);
            return;
        }
        if (!rec || !*rec) return;
        static std::FILE *f = std::fopen(rec, "w");
        if (!f) return;
        uint16_t p = 0; uint8_t a[4] = {0x80, 0x80, 0x80, 0x80};
        if (!::ps2xGsBridgePad(p, a)) return;
        static uint16_t lp = 0xFFFF; static uint8_t la[4] = {0, 0, 0, 0};
        if (p == lp && std::memcmp(a, la, 4) == 0) return;
        lp = p; std::memcpy(la, a, 4);
        std::fprintf(f, "%llu %04x %02x %02x %02x %02x\n", static_cast<unsigned long long>(tick), p, a[0], a[1], a[2], a[3]);
        if ((tick & 63u) == 0u) std::fflush(f);
        static bool atexitReg = [] { std::atexit([] { if (f) std::fflush(f); }); return true; }(); (void)atexitReg;
    }
    void ts2InputTick(uint64_t tick)
    {
        ts2EingabeAufnahme(tick);
        ts2InputTickScript(tick, "SOTC_INPUT", 0);
        ts2InputTickScript(tick, "TS2_INPUT2", 1);   // Split screen: player 2 (port 2)
        ts2InputTickScript(tick, "TS2_INPUT3", 2);   // Multitap: player 3
        ts2InputTickScript(tick, "TS2_INPUT4", 3);   // Multitap: player 4
    }
    void ts2InputTickScript(uint64_t tick, const char *envName, int port)
    {
        const char *script = std::getenv(envName);
        if (!script || !*script) return;
        static std::vector<std::pair<uint64_t, std::string>> evPorts[4];
        static bool parsedPorts[4] = {false, false, false, false};
        if (!parsedPorts[port]) { parsedPorts[port] = true; evPorts[port] = [script] {
            std::vector<std::pair<uint64_t, std::string>> ev; std::string all(script); size_t at = 0;
            while (at < all.size())
            {
                const size_t comma = all.find(',', at);
                const std::string item = all.substr(at, comma == std::string::npos ? std::string::npos : comma - at);
                at = comma == std::string::npos ? all.size() : comma + 1;
                if (const size_t colon = item.find(':'); colon != std::string::npos)
                    ev.emplace_back(std::strtoull(item.c_str(), nullptr, 10), item.substr(colon + 1));
            }
            return ev; }(); }
        const auto &events = evPorts[port];
        uint8_t b2 = 0xFF, b3 = 0xFF, rx = 0x80, ry = 0x80, lx = 0x80, ly = 0x80;
        for (const auto &[t, spec] : events)
        {
            std::string key = spec; uint64_t hold = 8u;
            if (const size_t star = key.find('*'); star != std::string::npos) { hold = std::strtoull(key.c_str() + star + 1, nullptr, 10); key = key.substr(0, star); }
            if (tick < t || tick >= t + hold) continue;
            if (key == "lup") ly = 0x00; else if (key == "ldown") ly = 0xFF; else if (key == "lleft") lx = 0x00; else if (key == "lright") lx = 0xFF;
            else if (key == "rup") ry = 0x00; else if (key == "rdown") ry = 0xFF; else if (key == "rleft") rx = 0x00; else if (key == "rright") rx = 0xFF;
            static const struct { const char *name; int byte; int bit; } kKeys[] = {
                {"select", 2, 0}, {"start", 2, 3}, {"up", 2, 4}, {"right", 2, 5}, {"down", 2, 6}, {"left", 2, 7},
                {"l2", 3, 0}, {"r2", 3, 1}, {"l1", 3, 2}, {"r1", 3, 3}, {"t", 3, 4}, {"o", 3, 5}, {"x", 3, 6}, {"s", 3, 7}};
            for (const auto &k : kKeys)
                if (key == k.name) { if (k.byte == 2) b2 &= static_cast<uint8_t>(~(1u << k.bit)); else b3 &= static_cast<uint8_t>(~(1u << k.bit)); }
        }
        if (port == 0)
        {
            // buttons the override holds (mouse aiming: aim mode L2) also apply to scripted input, like in real play
            g_skriptPadRoh = static_cast<uint16_t>(~(b2 | (b3 << 8)));
            { const unsigned z = g_ps2xPadZusatz.load(std::memory_order_relaxed); b2 &= static_cast<uint8_t>(~(z & 0xFFu)); b3 &= static_cast<uint8_t>(~((z >> 8) & 0xFFu)); }
            { const unsigned m = g_ps2xPadMaske.load(std::memory_order_relaxed); b2 |= static_cast<uint8_t>(m & 0xFFu); b3 |= static_cast<uint8_t>((m >> 8) & 0xFFu); }
            g_skriptRStickRoh[0] = rx; g_skriptRStickRoh[1] = ry;
            if (g_ps2xPadRStickAus.load(std::memory_order_relaxed)) rx = ry = 0x80;   // modern gamepad aiming turns the view itself
            ps2_stubs::setPadOverrideState(static_cast<uint16_t>(b2 | (b3 << 8)), lx, ly, rx, ry);
            g_skriptPad = static_cast<uint16_t>(~(b2 | (b3 << 8))); g_skriptAx[0] = lx; g_skriptAx[1] = ly; g_skriptAx[2] = rx; g_skriptAx[3] = ry; g_skriptPadDa = true;
        }
        else
        {
            ::ps2xSetPadOverridePlayer(port, static_cast<uint16_t>(b2 | (b3 << 8)), lx, ly, rx, ry);
        }
    }
}

// ---- Online+: quick bot match ----
// The Online+ hub (bridge option 18) asks for a bot match: code = 1 + mode * 16 + level index. From the main menu
// (cursor on "Online+", where the hub was opened) we drive the game's own menus with the pad override:
// Arcade -> Arcade Custom -> pace Normal -> game mode -> level -> GO! -> character -> match against the bots of the
// recommended bot set. The real pad is blocked meanwhile and released afterwards.
namespace ts2feed {
    // Online+ killfeed + scoreboard. The game keeps one record per match participant (stride 0x2A0, heap):
    //   +0      u32 kills                      (found by RAM diffing in a bot match)
    //   -0xE8   pointer to the character object, its +0x18 = pointer into the character name table (0x5ECD8C -> strings)
    //   -0x20C  pointer to the participant's health struct (health +0x19C, max +0x1A0, same layout as the player's)
    // Kill counters going up name the killer, health dropping to 0 names the victim; both are paired within half a second.
    // Results go to the bridge as text: id 1 = feed line "killer\tvictim\tflags" (1 you killed, 2 you died, 4 suicide),
    // id 2 = scoreboard "name\tkills\tdeaths\tyou\n..." (twice a second).
    using ts2impact::ru; using ts2impact::rf;
    constexpr uint32_t kNamenTab = 0x005ECD8Cu, kNamenN = 154;
    bool istName(const uint8_t *r, uint32_t v) { return v >= 0x5EF900u && v < 0x5F0200u && r[v] >= 0x20 && r[v] < 0x7F; }
    bool istGesundheit(const uint8_t *r, uint32_t p)
    {
        p &= 0x1FFFFFFu; if (p < 0x100000u || p > 0x1FFF000u) return false;
        const float m = rf(r, p + 0x1A0), h = rf(r, p + 0x19C);
        return m >= 0.2f && m <= 500.0f && h >= -50.0f && h <= m + 0.01f;
    }
    struct Platz { uint32_t a; uint32_t kills; bool tot; int tode; uint64_t todSeit; std::string name; int su = 0, serie = 0, best = 0; };
    std::vector<Platz> plaetze; uint64_t naechsterScan = 0; uint32_t basis = 0;

    uint32_t eigenBasis = 0;   // own kills from the hit detection fill the scoreboard when the game's counter stays (TS2_FEED_EIGEN=0 off)
    const bool eigenAn = [] { const char *e = std::getenv("TS2_FEED_EIGEN"); return !(e && *e == '0'); }();
    uint32_t eigenKills(const Platz &p) { if (!eigenAn || !p.name.empty()) return p.kills; const uint32_t e = g_ts2EigenKills.load() - eigenBasis; return (e < 1000u && e > p.kills) ? e : p.kills; }
    std::string nameVon(const uint8_t *r, uint32_t a)
    {
        const uint32_t obj = ru(r, a - 0xE8u) & 0x1FFFFFFu;
        if (obj < 0x100000u || obj > 0x1FFF000u) return "";
        const uint32_t n = ru(r, obj + 0x18u);
        if (!istName(r, n)) return "";
        return std::string(reinterpret_cast<const char *>(r + n), strnlen(reinterpret_cast<const char *>(r + n), 40));
    }
    bool gueltig(const uint8_t *r, uint32_t a) { return ru(r, a) < 1000u && istGesundheit(r, ru(r, a - 0x20Cu)); }
    void suchen(const uint8_t *r)
    {
        // The records sit in one array (stride 0x2A0) that also holds unused/other entries. Participants are the named
        // records (bots: character name) plus the human player's record (no name, field -0x200 = 32 or 2048) lying
        // between the first and the last named one (RAM dumps ts2_601 / ts2_620).
        plaetze.clear(); basis = 0;
        for (uint32_t a = 0x900000u; a < 0x1800000u; a += 4u)
        {
            if (ru(r, a) >= 1000u) continue;
            if (nameVon(r, a).empty() || !gueltig(r, a)) continue;
            uint32_t erster = a, letzter = a;
            for (int k = 1; k <= 16; ++k) { const uint32_t c = a - k * 0x2A0u; if (c < 0x900000u || !gueltig(r, c)) break; if (!nameVon(r, c).empty()) erster = c; }
            for (int k = 1; k <= 16; ++k) { const uint32_t c = a + k * 0x2A0u; if (c >= 0x1800000u || !gueltig(r, c)) break; if (!nameVon(r, c).empty()) letzter = c; }
            bool spieler = false;
            for (uint32_t c = erster; c <= letzter; c += 0x2A0u)
            {
                const std::string n = nameVon(r, c);
                const uint32_t art = ru(r, c - 0x200u);
                if (n.empty() && (spieler || (art != 32u && art != 2048u))) continue;
                if (n.empty()) spieler = true;
                Platz p{c, ru(r, c), false, 0, 0, n};
                p.tot = rf(r, (ru(r, c - 0x20Cu) & 0x1FFFFFFu) + 0x19C) <= 0.0f;
                plaetze.push_back(p);
                if (plaetze.size() >= 16) break;
            }
            if (plaetze.size() >= 2) { basis = erster; return; }
            plaetze.clear();
            a = std::max(a, letzter);
        }
    }
    void tick(const uint8_t *r, uint64_t t)
    {
        if (!r) return;
        static const int envAn = [] { const char *e = std::getenv("TS2_IMPACT"); return (e && *e == '1') ? 1 : 0; }();
        static int an = 0; static uint64_t naechsteOpt = 0;
        if (t >= naechsteOpt) { an = ::ps2xGsBridgeOption(22, envAn); naechsteOpt = t + 60; }
        static const bool log = std::getenv("TS2_IMPACT_LOG") != nullptr;
        if (!an) { plaetze.clear(); return; }
        // the player must be in a level; otherwise forget the records (next match allocates new ones)
        const uint32_t spObj = ru(r, ts2impact::kSpielerZeiger) & 0x1FFFFFFu;
        if (!spObj) { if (!plaetze.empty()) { plaetze.clear(); ::ps2xGsBridgeText(2, ""); } return; }
        // only during a match: the player's own character must be valid
        // (a dead or respawning character briefly has no valid health pointer: that must not drop the records)
        bool kaputt = plaetze.empty();
        for (const auto &p : plaetze) if (ru(r, p.a) >= 1000u || (t % 60u == 0u && nameVon(r, p.a) != p.name)) { kaputt = true; break; }
        if (kaputt) { const uint32_t hs = ru(r, spObj + 0x1BC) & 0x1FFFFFFu; if (!hs || !istGesundheit(r, hs)) { if (!plaetze.empty()) { plaetze.clear(); ::ps2xGsBridgeText(2, ""); } return; } }
        if (kaputt)
        {
            if (t < naechsterScan) return;
            naechsterScan = t + 60;
            suchen(r);
            for (const auto &p : plaetze) if (p.name.empty()) eigenBasis = g_ts2EigenKills.load() - p.kills;
            if (log && !plaetze.empty()) { std::fprintf(stderr, "[feed] %zu participants at %08x:", plaetze.size(), basis); for (auto &p : plaetze) std::fprintf(stderr, " %s=%u", p.name.empty() ? "YOU" : p.name.c_str(), p.kills); std::fprintf(stderr, "\n"); }
            return;
        }
        // participants can still be spawning when the block is first found (names missing -> list cut short):
        // look again every 5 s and take a longer list, keeping the counters of known records
        static uint64_t naechsteNachsuche = 0; bool neueListe = false;
        if (t >= naechsteNachsuche)
        {
            naechsteNachsuche = t + 300;
            std::vector<Platz> alt; alt.swap(plaetze);
            suchen(r);
            if (plaetze.size() > alt.size())
            {
                for (auto &p : plaetze) for (const auto &q : alt) if (q.a == p.a) { p = q; p.name = nameVon(r, p.a); break; }
                neueListe = true;
                if (log) { std::fprintf(stderr, "[feed] now %zu participants:", plaetze.size()); for (auto &p : plaetze) std::fprintf(stderr, " %s", p.name.empty() ? "YOU" : p.name.c_str()); std::fprintf(stderr, "\n"); }
            }
            else plaetze.swap(alt);
        }
        // events of this frame
        struct Ev { int wer; uint64_t t; };
        static std::vector<Ev> killer, opfer;
        if (neueListe) { killer.clear(); opfer.clear(); }   // indices refer to the old list
        for (int i = 0; i < static_cast<int>(plaetze.size()); ++i)
        {
            Platz &p = plaetze[i];
            const uint32_t k = ru(r, p.a);
            if (k > p.kills && k - p.kills < 5) for (uint32_t n = p.kills; n < k; ++n) killer.push_back({i, t});
            p.kills = k;
            const uint32_t hp = ru(r, p.a - 0x20Cu);
            if (!istGesundheit(r, hp)) continue;   // between death and respawn: keep the last state
            const bool tot = rf(r, (hp & 0x1FFFFFFu) + 0x19C) <= 0.0f;
            if (tot && !p.tot) { opfer.push_back({i, t}); ++p.tode; }
            p.tot = tot;
        }
        auto nm = [](const Platz &p) { return p.name.empty() ? std::string("YOU") : p.name; };
        // pair: a victim with a killer seen within 30 ticks; a victim alone after 30 ticks = suicide / world
        while (!opfer.empty())
        {
            const Ev o = opfer.front();
            int kIdx = -1;
            for (size_t j = 0; j < killer.size(); ++j) if (killer[j].wer != o.wer && (killer[j].t + 90 >= o.t && o.t + 90 >= killer[j].t)) { kIdx = static_cast<int>(j); break; }
            if (kIdx < 0 && t < o.t + 90) break;   // wait for the counter (it can lag the death by about a second)

            // no killer from the counter: a kill the hit detection saw within 1.5 s counts as the player's kill
            if (kIdx < 0 && eigenAn && !plaetze[o.wer].name.empty())
                for (auto &kt : g_ts2EigenKillTick)
                {
                    const uint64_t e = kt.load();
                    if (!e || e + 90 < o.t || o.t + 90 < e) continue;
                    for (int i = 0; i < static_cast<int>(plaetze.size()); ++i) if (plaetze[i].name.empty()) { killer.push_back({i, e}); kIdx = static_cast<int>(killer.size()) - 1; break; }
                    if (kIdx >= 0) { kt.store(0); if (log) std::fprintf(stderr, "[feed] tick %llu own kill from the hit detection\n", (unsigned long long)t); }
                    break;
                }

            // or a hit by the player at most half a second before the bot died
            if (kIdx < 0 && eigenAn && !plaetze[o.wer].name.empty())
                for (auto &ht : g_ts2EigenTrefferTick)
                {
                    const uint64_t e = ht.load();
                    if (!e || e > o.t + 10 || o.t > e + 30) continue;
                    for (int i = 0; i < static_cast<int>(plaetze.size()); ++i) if (plaetze[i].name.empty()) { killer.push_back({i, e}); kIdx = static_cast<int>(killer.size()) - 1; break; }
                    if (kIdx >= 0) { ht.store(0); g_ts2EigenKillTick[g_ts2EigenKills.fetch_add(1) & 7u].store(0); if (log) std::fprintf(stderr, "[feed] tick %llu own kill from a hit just before the death\n", (unsigned long long)t); }
                    break;
                }
            const Platz &v = plaetze[o.wer];
            std::string zeile; int fl = 0;
            if (kIdx >= 0) { Platz &k = plaetze[killer[kIdx].wer]; zeile = nm(k) + "\t" + nm(v); if (k.name.empty()) fl |= 1; ++k.serie; k.best = std::max(k.best, k.serie); killer.erase(killer.begin() + kIdx); }
            else { zeile = std::string("\t") + nm(v); fl |= 4; ++plaetze[o.wer].su; }
            plaetze[o.wer].serie = 0;
            if (v.name.empty()) fl |= 2;
            zeile += "\t" + std::to_string(fl);
            ::ps2xGsBridgeText(1, zeile.c_str());
            if (kIdx >= 0 || true) ts2melde::push(kIdx >= 0 || !(fl & 4) ? (zeile.substr(0, zeile.find('\t')) + "  >>  " + nm(v)) : (nm(v) + "  (suicide)"), 2);
            if (log) std::fprintf(stderr, "[feed] tick %llu %s\n", (unsigned long long)t, zeile.c_str());
            opfer.erase(opfer.begin());
        }
        // stale killer entries (victim not detected): still show the kill
        while (!killer.empty() && t > killer.front().t + 120)
        {
            const Platz &k = plaetze[killer.front().wer];
            const std::string zeile = nm(k) + "\t?\t" + std::to_string(k.name.empty() ? 1 : 0);
            ::ps2xGsBridgeText(1, zeile.c_str());
            if (log) std::fprintf(stderr, "[feed] tick %llu %s\n", (unsigned long long)t, zeile.c_str());
            killer.erase(killer.begin());
        }
        if (t % 30u == 0u)
        {
            std::string sb;
            for (const auto &p : plaetze) sb += nm(p) + "\t" + std::to_string(eigenKills(p)) + "\t" + std::to_string(p.tode) + "\t" + (p.name.empty() ? "1" : "0") + "\t" +
                                                std::to_string(p.su) + "\t" + std::to_string(p.serie) + "\t" + std::to_string(p.best) + "\n";
            ::ps2xGsBridgeText(2, sb.c_str());
        }
    }
}
std::atomic<bool> g_ts2CheatAmmo{false};   // read by the reload wrapper below
std::atomic<bool> g_ts2OnlineSitzung{false};   // set while the Online+ connection loop runs: no cheats there
namespace ts2cheat {

    // Optional single player cheats (bridge options 35/36/37 = launcher / in-game menu, or TS2_CHEAT="health,armor,ammo").
    // Health struct *(player+0x1BC): health +0x19C, max +0x1A0; armor struct *(health+0x124): value +0x10, max +0x14.
    // Infinite ammo: see the wrapper around the reload function 0x2DBBD0. TS2_CHEATLOG=1 logs.
    using ts2impact::ru; using ts2impact::rf; using ts2impact::wf;
    inline void wu(uint8_t *r, uint32_t a, uint32_t v) { std::memcpy(r + (a & 0x1FFFFFCu), &v, 4); }
    void tick(uint8_t *r, uint64_t t)
    {
        if (!r) return;
        static const std::string env = [] { const char *e = std::getenv("TS2_CHEAT"); return std::string(e ? e : ""); }();
        static const bool log = std::getenv("TS2_CHEATLOG") != nullptr;
        static int h = 0, a = 0, m = 0;
        if (t % 30u == 0u)
        {
            h = ::ps2xGsBridgeOption(35, env.find("health") != std::string::npos ? 1 : 0);
            a = ::ps2xGsBridgeOption(36, env.find("armor") != std::string::npos ? 1 : 0);
            m = ::ps2xGsBridgeOption(37, env.find("ammo") != std::string::npos ? 1 : 0);
        }
        if (g_ts2OnlineSitzung.load(std::memory_order_relaxed)) { h = a = m = 0; }
        if (!m) g_ts2CheatAmmo.store(false, std::memory_order_relaxed);
        if (!h && !a && !m && !log) return;
        const uint32_t pb = ru(r, ts2impact::kSpielerZeiger) & 0x1FFFFFFu;
        if (pb < 0x100000u || pb > 0x1FFE000u) return;
        const uint32_t hs = ru(r, pb + 0x1BC) & 0x1FFFFFFu;
        if (hs < 0x100000u || hs > 0x1FFF000u) return;
        const float mx = rf(r, hs + 0x1A0), hp = rf(r, hs + 0x19C);
        const bool lebt = mx >= 0.2f && mx <= 500.f && hp > 0.f && hp <= mx + 0.01f;
        if (!lebt) { g_ts2CheatAmmo.store(false, std::memory_order_relaxed); return; }
        if (h && hp < mx) wf(r, hs + 0x19C, mx);
        const uint32_t as = ru(r, hs + 0x124) & 0x1FFFFFFu;
        const bool asOk = as >= 0x100000u && as < 0x1FFF000u;
        if (a && asOk) { const float am = rf(r, as + 0x14), av = rf(r, as + 0x10); if (am > 0.f && am <= 500.f && av >= 0.f && av < am) wf(r, as + 0x10, am); }
        g_ts2CheatAmmo.store(m != 0, std::memory_order_relaxed);
        if (log && t % 120u == 0u)
            std::fprintf(stderr, "[cheat] t=%llu hp %.2f/%.2f armor %s %.2f/%.2f\n", (unsigned long long)t, hp, mx, asOk ? "ok" : "-", asOk ? rf(r, as + 0x10) : 0.f, asOk ? rf(r, as + 0x14) : 0.f);
    }
}
namespace ts2botmatch
{
    // A step either presses a button, or ("select") presses down until the menu item with that text is highlighted.
    // Menu items: text pointer at +0x20, flags at +0x28 (0x100 = item, 0x10 = highlighted).
    struct Schritt { const char *taste; uint32_t warten; std::string text; };
    std::vector<Schritt> plan; size_t pos = 0; uint64_t naechster = 0, loslassen = 0, schrittStart = 0; bool aktiv = false;
    uint64_t letzteAbfrage = 0; int druecke = 0; const char *halten = "";
    // all copies of the text in the game's string area (menus may use different copies)
    const std::vector<uint32_t> &textAdressen(const uint8_t *r, const std::string &t)
    {
        static std::map<std::string, std::vector<uint32_t>> cache;
        if (auto it = cache.find(t); it != cache.end() && !it->second.empty()) return it->second;
        std::vector<uint32_t> v;
        const std::string such = std::string(1, '\0') + t + std::string(1, '\0');
        const char *b = reinterpret_cast<const char *>(r + 0x400000u); const char *e = b + 0x400000u;
        for (const char *f = b; (f = std::search(f, e, such.begin(), such.end())) != e; ++f) v.push_back(static_cast<uint32_t>(0x400000u + (f - b) + 1));
        return cache[t] = v;
    }
    // -1 = no such item on screen (yet), 0 = there but not highlighted, 1 = highlighted
    int zustand(const uint8_t *r, const std::vector<uint32_t> &texte, bool debug)
    {
        if (texte.empty()) return -1;
        int z = -1;
        for (uint32_t a = 0x00600000u; a < 0x01FF0000u; a += 4)
        {
            uint32_t v; std::memcpy(&v, r + a, 4);
            if (std::find(texte.begin(), texte.end(), v) == texte.end()) continue;
            uint32_t fl; std::memcpy(&fl, r + a + 8, 4);   // base + 0x28 = (a - 0x20) + 0x28
            if (debug) std::fprintf(stderr, "[Online+] candidate %08x -> text %08x flags(+8) %08x\n", a, v, fl);
            if (!(fl & 0x100u) || (fl & ~0xFFFu)) continue;
            if (fl & 0x10u) return 1;
            z = 0;
        }
        return z;
    }
    void starten(uint64_t t, int code)
    {
        static const char *kModi[] = {"Deathmatch", "Team Deathmatch", "Capture the Bag"};
        static const char *kKarten[] = {"Mexican Mission", "Ice Station", "Hospital", "Training Ground", "Aztec", "Scrapyard", "Nightclub", "Hangar",
                                        "Robot Factory", "Ufopia", "Chinese", "Chasm", "Streets", "Compound", "Site", "Circus"};
        static const char *kKartenCtb[] = {"Mexican Mission", "Ice Station", "Training Ground", "Hangar", "Streets"};
        static const char *kSets[] = {"Recommended", "Misfits", "Freaks", "The Living Dead", "Lovely Ladies", "Military", "Classic", "Leads", "Sidekicks"};
        // code = 1 + (set * 3 + mode) * 16 + level
        const int modus = ((code - 1) >> 4) % 3, level = (code - 1) & 15, set = std::min(((code - 1) >> 4) / 3, 8);
        plan.clear();
        plan.push_back({"", 45, ""});                                       // hub closes, game resumes
        for (int i = 0; i < 4; ++i) plan.push_back({"up", 18, ""});         // main menu: Online+ -> Arcade
        plan.push_back({"sel", 0, "Arcade"});                              // check it (coming back from the hub an extra press can slip in)
        plan.push_back({"x", 115, ""});                                    // -> Arcade Type (wait for the screen)
        plan.push_back({"sel", 0, "Arcade Custom"}); plan.push_back({"x", 115, ""});
        plan.push_back({"sel", 0, "Normal"}); plan.push_back({"x", 125, ""});
        // mode cards and level pictures are not plain menu items: those lists open at the top, so count instead
        for (int i = 0; i < modus; ++i) plan.push_back({"down", 25, ""});
        plan.push_back({"x", 125, ""});
        for (int i = 0; i < level; ++i) plan.push_back({"down", 24, ""});
        plan.push_back({"x", 125, ""});                                    // -> options (cursor on GO!)
        if (set > 0)
        {
            // Bot Set (one up from GO!) -> Setup Bots, the preset selector at the top: right = next preset, triangle = back
            plan.push_back({"up", 25, ""}); plan.push_back({"x", 110, ""});
            for (int i = 0; i < set; ++i) plan.push_back({"right", 25, ""});
            plan.push_back({"tri", 110, ""}); plan.push_back({"down", 25, ""});
        }
        plan.push_back({"x", 125, ""});                                    // GO! -> character
        for (int i = 0; i < 3; ++i) plan.push_back({"x", 90, ""});         // character -> match (a press during a transition is lost)
        pos = 0; naechster = t; aktiv = true; druecke = 0; halten = "";
        std::fprintf(stderr, "[Online+] bot match: %s on %s, bots %s (%zu steps)\n", kModi[modus], modus == 2 ? kKartenCtb[level % 5] : kKarten[level], kSets[set], plan.size());
    }
    void tick(const uint8_t *r, uint64_t t)
    {
        // TS2_BOTMATCH="tick:code" (test runs): start the macro at that VBlank without the hub
        static const std::pair<uint64_t, int> testStart = [] { const char *e = std::getenv("TS2_BOTMATCH"); unsigned long long tt = 0; int c = 0;
            if (e && std::sscanf(e, "%llu:%d", &tt, &c) == 2) return std::make_pair(static_cast<uint64_t>(tt), c); return std::make_pair(uint64_t(0), 0); }();
        if (!aktiv && testStart.second > 0 && t == testStart.first) starten(t, testStart.second);
        if (!aktiv && t - letzteAbfrage >= 10) { letzteAbfrage = t; const int code = ::ps2xGsBridgeOption(18, 0); if (code > 0) starten(t, code); }
        if (!aktiv || !r) return;
        if (t >= naechster)
        {
            halten = "";
            if (pos >= plan.size()) { aktiv = false; ps2_stubs::clearPadOverrideState(); std::fprintf(stderr, "[Online+] bot match: menus done\n"); return; }
            Schritt &s = plan[pos];
            if (std::string(s.taste) == "sel")
            {
                if (druecke == 0 && schrittStart == 0) schrittStart = t;
                const int z = zustand(r, textAdressen(r, s.text), false);
                if (z == 1) { std::fprintf(stderr, "[Online+] bot match: '%s' selected (%d presses)\n", s.text.c_str(), druecke); ++pos; druecke = 0; schrittStart = 0; naechster = t + 12; }
                else if (z == 0 && druecke < 24) { halten = "down"; loslassen = t + 6; naechster = t + 26; ++druecke; }
                else if (t - schrittStart > 420 || druecke >= 24)
                { std::fprintf(stderr, "[Online+] bot match: '%s' not found - stopped (%zu copies)\n", s.text.c_str(), textAdressen(r, s.text).size()); (void)zustand(r, textAdressen(r, s.text), true); aktiv = false; ps2_stubs::clearPadOverrideState(); return; }
                else naechster = t + 10;   // list not on screen yet (transition)
            }
            else { halten = s.taste; loslassen = t + 6; naechster = t + 6 + s.warten; ++pos; }
        }
        uint8_t b2 = 0xFF, b3 = 0xFF;
        if (t < loslassen && halten[0])
        {
            const std::string k = halten;
            if (k == "up") b2 &= ~(1u << 4); else if (k == "down") b2 &= ~(1u << 6); else if (k == "right") b2 &= ~(1u << 5);
            else if (k == "x") b3 &= ~(1u << 6); else if (k == "tri") b3 &= ~(1u << 4);
        }
        ps2_stubs::setPadOverrideState(static_cast<uint16_t>(b2 | (b3 << 8)), 0x80, 0x80, 0x80, 0x80);
    }
}

namespace
{
    void applyTs2(PS2Runtime &runtime)
    {
        // Complete log: if stdout is a pipe (the launcher writes the play log) the C runtime otherwise buffers
        // in blocks, and the end is missing after a hang/abort. Line buffering is enough (stderr is unbuffered).
#ifdef _WIN32
        std::setvbuf(stdout, nullptr, _IONBF, 0);   // Windows has no line buffering (_IOLBF = block buffering)
        // Windows wakes waiting threads only at the system timer tick (default 15.6 ms). The VBlank pace (16.7 ms) and
        // the frame synchronisation wait through condition_variable/Sleep -> half the frame rate without 1 ms resolution.
        ::timeBeginPeriod(1);
#else
        std::setvbuf(stdout, nullptr, _IOLBF, 1 << 14);
#endif
        configureIso();
        // TS2_MCROOT=/path: separate memory card directory (reproducible test runs, do not share disc/inhalt/mc0)
        if (const char *mc = std::getenv("TS2_MCROOT"); mc && *mc)
        {
            PS2Runtime::IoPaths p = PS2Runtime::getIoPaths();
            p.mcRoot = mc;
            PS2Runtime::setIoPaths(p);
            std::fprintf(stderr, "[TS2] Memory Card: %s\n", mc);
        }
        runtime.registerFunction(0x00488DC0u, sceCdLayerSearchFile_0x488dc0);
        // Infinite ammo cheat: the reload function 0x2DBBD0 takes rounds from the reserve array at a0+0xA3C (one word per
        // ammo type); while the cheat is on, a reserve that went down is put back.
        {
            static PS2Runtime::RecompiledFunction s_reload = runtime.lookupFunction(0x002DBBD0u);
            if (s_reload)
                runtime.registerFunction(0x002DBBD0u, [](uint8_t *r, R5900Context *c, PS2Runtime *rt) {
                    static const bool lg = std::getenv("TS2_CHEATLOG") != nullptr;
                    const bool an = g_ts2CheatAmmo.load(std::memory_order_relaxed);
                    const uint32_t b = (GPR_U32(c, 4) & 0x1FFFFFFu) + 0xA3Cu;
                    uint32_t vor[22]; const bool ok = b >= 0x100000u && b + sizeof(vor) <= 0x2000000u;
                    if (ok) std::memcpy(vor, r + b, sizeof(vor));
                    s_reload(r, c, rt);
                    if (!ok) return;
                    uint32_t nach[22]; std::memcpy(nach, r + b, sizeof(nach));
                    for (int i = 0; i < 22; ++i)
                        if (nach[i] < vor[i])
                        {
                            if (lg) std::fprintf(stderr, "[cheat] reload: reserve[%d] %u -> %u%s (base %08x)\n", i, vor[i], nach[i], an ? " put back" : "", b);
                            if (an && vor[i] <= 9999u) std::memcpy(r + b + 4u * i, &vor[i], 4);
                        }
                });
        }

        // Auto-aim off while the mouse aims: 0x2BBBC8 picks the enemy the aim point is pulled to (+0x8B0/+0x8B4 target,
        // smoothed into +0x8B8/+0x8BC = the real shot direction). With the mouse that felt "magnetic" and the shots did not
        // go where the centre crosshair was. No target -> the aim point stays at the centre. TS2_AUTOAIM=1 keeps it;
        // since 1.1.1 also the launcher switch "Auto-aim (mouse)" (bridge option 34).
        {
            static PS2Runtime::RecompiledFunction s_autoAim = runtime.lookupFunction(0x002BBBC8u);
            static const bool autoAimEnv = [] { const char *e = std::getenv("TS2_AUTOAIM"); return e && *e == '1'; }();
            if (s_autoAim && !autoAimEnv)
                runtime.registerFunction(0x002BBBC8u, [](uint8_t *r, R5900Context *c, PS2Runtime *rt) {

                    if (g_ts2MausZielt.load(std::memory_order_relaxed) && ::ps2xGsBridgeOption(34, 0) == 0)
                    {
                        uint32_t pl; std::memcpy(&pl, r + 0x0059DC0Cu, 4); pl &= 0x1FFFFFFu;   // *(gp-0x25E0) = player
                        if (pl >= 0x100000u && pl < 0x1FFF000u) { const uint32_t z = 0; std::memcpy(r + pl + 0x8C8u, &z, 4); }
                        SET_GPR_U32(c, 2, 0u); c->pc = GPR_U32(c, 31); return;
                    }
                    s_autoAim(r, c, rt);
                });
        }
        // 0x24FFF8 (menu background animation, runs every frame in the menus): wraps animation phases with a float loop
        // "while (1.0 <= phase) phase -= 1.0". A phase of +inf / >= 2^24 never gets smaller -> the game thread spins
        // forever. Seen in the Online+ lobby about every second run (regression suite, haenger735-737). Before each call
        // the object's float words that are not finite or absurdly large are logged once and reset to 0.
        {
            static PS2Runtime::RecompiledFunction s_menuAnim = runtime.lookupFunction(0x0024FFF8u);
            static const bool aus = [] { const char *e = std::getenv("TS2_ANIM_SCHUTZ"); return e && *e == '0'; }();
            if (s_menuAnim && !aus)
                runtime.registerFunction(0x0024FFF8u, [](uint8_t *r, R5900Context *c, PS2Runtime *rt) {
                    const uint32_t o = GPR_U32(c, 4) & 0x1FFFFFFu;
                    if (o >= 0x100000u && o + 0x200u < 0x2000000u)
                        for (uint32_t k : {0xE8u, 0x140u, 0x170u, 0x178u})   // phases of the two elements (+0xC0, stride 0x58, +0x28) and the timers
                        {
                            float v; std::memcpy(&v, r + o + k, 4);
                            if (!std::isfinite(v) || std::fabs(v) > 1048576.f)
                            {
                                static std::atomic<int> s_n{0};
                                float t170, t174, t178; std::memcpy(&t170, r + o + 0x170, 4); std::memcpy(&t174, r + o + 0x174, 4); std::memcpy(&t178, r + o + 0x178, 4);
                                if (s_n.fetch_add(1) < 20)
                                    std::fprintf(stderr, "[anim-schutz] object 0x%06X phase +0x%03X = %g -> 0 (+170 %g +174 %g +178 %g) ra=0x%08X\n", o, k, v, t170, t174, t178, GPR_U32(c, 31));
                                const float z = 0.f; std::memcpy(r + o + k, &z, 4);
                            }
                        }
                    // The loop itself yields to the scheduler (checkpoint) and resumes here with pc = 0x2501E0 and the
                    // phase in f0: a value that subtracting 1.0 cannot shrink (inf, NaN-free huge) is wrapped directly.
                    if (c->pc == 0x002501E0u && (!std::isfinite(c->f[0]) || c->f[0] >= 8388608.f))
                    {
                        static std::atomic<int> s_m{0};
                        if (s_m.fetch_add(1) < 20)
                            std::fprintf(stderr, "[anim-schutz] phase loop f0=%g f1=%g object 0x%06X -> 0\n", c->f[0], c->f[1], GPR_U32(c, 7) & 0x1FFFFFFu);
                        c->f[0] = 0.f;
                    }
                    s_menuAnim(r, c, rt);
                });
        }
        // Resume entry of the phase loop (the scheduler continues a function at its checkpoint pc through this table entry)
        {
            static PS2Runtime::RecompiledFunction s_resume = runtime.lookupFunction(0x002501E0u);
            std::fprintf(stderr, "[anim-schutz] resume entry 0x2501E0 %s\n", s_resume ? "found" : "not in the table");
            if (s_resume)
                runtime.registerFunction(0x002501E0u, [](uint8_t *r, R5900Context *c, PS2Runtime *rt) {
                    static std::atomic<int> s_k{0};
                    if (!std::isfinite(c->f[0]) || c->f[0] >= 8388608.f || s_k.load() < 3)
                    {
                        if (s_k.fetch_add(1) < 20)
                            std::fprintf(stderr, "[anim-schutz] resume f0=%g f1=%g object 0x%06X\n", c->f[0], c->f[1], GPR_U32(c, 7) & 0x1FFFFFFu);
                        if (!std::isfinite(c->f[0]) || c->f[0] >= 8388608.f) c->f[0] = 0.f;
                    }
                    s_resume(r, c, rt);
                });
        }
        // The aim-mode crosshair itself (round bracket crosshair at +0x8B8/+0x8BC, found by skipping candidates, ts2_674):
        // hidden while the mouse aims with the permanent aim mode, the port's centre crosshair is the only one.
        // Zooming keeps it (sniper scope).
        {
            static PS2Runtime::RecompiledFunction s_kreuz = runtime.lookupFunction(0x00335B90u);
            if (s_kreuz)
                runtime.registerFunction(0x00335B90u, [](uint8_t *r, R5900Context *c, PS2Runtime *rt) {
                    if (g_ts2EigenesZiel.load(std::memory_order_relaxed)) { c->pc = GPR_U32(c, 31); return; }
                    s_kreuz(r, c, rt);
                });
        }
        // TS2_SKIP_FN=addr,addr (diagnosis): these game functions return at once
        if (const char *e = std::getenv("TS2_SKIP_FN"))
        {
            std::string all(e); size_t p0 = 0;
            while (p0 < all.size())
            {
                const size_t q = all.find(',', p0); const uint32_t a = static_cast<uint32_t>(std::strtoul(all.substr(p0, q == std::string::npos ? std::string::npos : q - p0).c_str(), nullptr, 16));
                if (a) { runtime.registerFunction(a, [](uint8_t *, R5900Context *c, PS2Runtime *) { c->pc = GPR_U32(c, 31); }); std::fprintf(stderr, "[skip] %08x\n", a); }
                if (q == std::string::npos) break; p0 = q + 1;
            }
        }
        // Aim-mode HUD (wobbling crosshair at the free-aim point +0x8B8/+0x8BC, player object): skipped while the mouse aims
        {
            static PS2Runtime::RecompiledFunction s_zielHud = runtime.lookupFunction(0x002E18E8u);
            if (s_zielHud)
                runtime.registerFunction(0x002E18E8u, [](uint8_t *r, R5900Context *c, PS2Runtime *rt) {
                    static unsigned n = 0, weg = 0; const bool aus = g_ts2EigenesZiel.load(std::memory_order_relaxed);
                    if (aus) ++weg;
                    if (++n == 1 || n % 600 == 0) std::fprintf(stderr, "[zielhud] %u calls, %u hidden\n", n, weg);
                    if (aus) { c->pc = GPR_U32(c, 31); return; }
                    s_zielHud(r, c, rt);
                });
        }
        runtime.registerFunction(0x0044E248u, sceMtapPortOpen_0x44e248);
        runtime.registerFunction(0x0044E2B8u, sceMtapPortClose_0x44e2b8);
        runtime.registerFunction(0x0044E328u, sceMtapGetConnection_0x44e328);
        g_ps2xGameGParamAddr = 0x0056A250u;   // sceGsResetGraph runs as original code and writes the video mode there   // libcdvd without a server -> ISO9660
        startWatchdog(runtime);
#if defined(__linux__)
        haenger::start(runtime);   // Hang report into the log (without gdb)
#endif
        startMemWatch(runtime);      // SOTC_MEMWATCH=0xADDR,...
        startWriteProtect(runtime);  // SOTC_WPROT=0xADDR
        startTestHarness(runtime);
        sotc_prof::start();
        // Test harness: until the game's VBlank handler is known, poll the VSync counters from a thread of our own.
        PS2Runtime *rt = &runtime;
        std::thread([rt]() {
            uint64_t last = 0;
            for (;;)
            {
                const uint64_t t = rt->memory().gs().vsyncTick.load();
                if (t != last) { last = t; ++g_vblankHits; ts2WidescreenTick(rt->memory().getRDRAM()); ts2InputTick(t); maus::tick(rt->memory().getRDRAM(), t); maus::flug(rt->memory().getRDRAM(), t); harnessOnVBlank(rt); { static const bool vt = std::getenv("SOTC_TRACE_VIF") != nullptr; if (vt && t % 100u == 0u) { std::fprintf(stderr, "[VIF-Statistik] tick=%llu\n", (unsigned long long)t); sotc_vif_trace::dump(); } } sotcRamDumpCheck(rt->memory().getRDRAM(), t); ts2Probe(rt->memory().getRDRAM(), t); { uint16_t pt = 0; uint8_t ax[4]; bool f = false; if (g_skriptPadDa) f = (g_skriptPad & 0x0A00u) != 0; else f = ::ps2xGsBridgePad(pt, ax) && (pt & 0x0A00u) != 0; ts2impact::tick(rt->memory().getRDRAM(), t, f); } ts2botmatch::tick(rt->memory().getRDRAM(), t); ts2feed::tick(rt->memory().getRDRAM(), t); ts2cheat::tick(rt->memory().getRDRAM(), t); }
                std::this_thread::sleep_for(std::chrono::microseconds(500));
            }
        }).detach();
        // ---- Online+ edition: own entry "Online+" in the game's main menu ----
        // The main menu is built by 0x3F35F0: five items via 0x24C8E0 (create item in menu, 0x23, 0x100), text pointer at
        // item+0x20, style 0x15/3 at +0x1C/+0x24, width from 0x2533F8 at +0x34, 5 at +0x38, handler at +0x30; then
        // 0x253498 lays the menu out. We add a sixth item right before that layout call, with the same functions, so it
        // looks and navigates like the original ones. Its handler (a free slot in the code table) asks the bridge to
        // open the Online+ page (server browser, stats, settings). Text lives at 0xF0000 (always unused RAM).
        if (const char *ed = std::getenv("PS2XGS_EDITION"); ed && std::string(ed) == "onlineplus")
        {
            static PS2Runtime::RecompiledFunction s_origMenu = runtime.lookupFunction(0x003F35F0u);
            static PS2Runtime::RecompiledFunction s_origLayout = runtime.lookupFunction(0x00253498u);
            static PS2Runtime::RecompiledFunction s_neuItem = runtime.lookupFunction(0x0024C8E0u);
            static PS2Runtime::RecompiledFunction s_breite = runtime.lookupFunction(0x002533F8u);
            static std::atomic<bool> s_baue{false};
            // Online+ lobby (see below): descriptor, builder and item handler addresses, texts, build state
            // (handlers must live inside the generated function table 0x200000..0x499F20: unused padding words after
            //  "jr ra" in the menu code serve as addresses - never executed as code)
            constexpr uint32_t kLobbyDesc = 0x000F1000u, kLobbyBau = 0x003E039Cu, kLobbyText = 0x000F0200u;
            static const uint32_t kLobbyItemAdr[6] = {0x003E04BCu, 0x003E07FCu, 0x003E0A94u, 0x003E0EDCu, 0x003E10D4u, 0x003E15B4u};
            static const char *kLobbyEintraege[] = {"Quick Match", "Create Lobby", "Server Browser", "Bot Match", "Profile & Ranks", "Back"};
            constexpr int kLobbyN = 6;
            static bool s_lobbyBau = false; static std::vector<uint32_t> s_lobbyItems; static uint32_t s_lobbyMenu = 0;
            // in-game server browser (same technique): 6 server rows + Refresh + Back; rows are refreshed in place
            static auto opName = []() {
                std::string name = "Player";

                        {
                            const char *xdg = std::getenv("XDG_CONFIG_HOME"); const char *home = std::getenv("HOME"); const char *app = std::getenv("APPDATA");
                            const char *cn = std::getenv("PS2XGS_CFGNAME");
                            std::string basis = xdg && *xdg ? std::string(xdg) : app && *app ? std::string(app) : std::string(home ? home : ".") + "/.config";
                            for (const char *datei : {"/settings.ini", "/einstellungen.ini"})
                            {
                                std::ifstream in(basis + "/" + (cn && *cn ? cn : "ts2-recomp") + datei);
                                for (std::string z; std::getline(in, z);) if (z.rfind("online_name=", 0) == 0 && z.size() > 12) { name = z.substr(12, 20); while (!name.empty() && (name.back() == '\r' || name.back() == ' ')) name.pop_back(); }
                                if (name != "Player") break;
                            }
                        }
                return name; };
            static int s_lobbyModus = 1;   // 1 = lobby, 2 = server browser (which menu the layout hook builds)
            constexpr uint32_t kBrowserDesc = 0x000F1020u, kBrowserBau = 0x003E162Cu, kBrowserUpd = 0x003E20DCu, kBrowserText = 0x000F0A00u;
            static const uint32_t kBrowserItemAdr[8] = {0x003E224Cu, 0x003E227Cu, 0x003E29FCu, 0x003E323Cu, 0x003E385Cu, 0x003E3D5Cu, 0x003E406Cu, 0x003E4134u};
            constexpr int kBrowserN = 8, kBrowserServer = 6;
            static uint32_t s_browserItem[8] = {}; static std::vector<std::string> s_browserAdr;
            static int s_seite = 2;   // which 8-row page is open: 2 = server browser, 3 = profile & ranks
            constexpr uint32_t kProfilDesc = 0x000F1040u, kProfilBau = 0x003E41D4u;
            static std::vector<std::string> s_profilZeilen;
            // fills the browser rows from the bridge's list; returns the row texts
            static auto browserZeilen = []() {
                std::vector<std::string> z; s_browserAdr.clear();
                static char buf[8192]; ::ps2xGsBridgeTextGet(1, buf, sizeof buf);
                std::string all(buf); size_t p0 = 0;
                while (p0 < all.size() && z.size() < static_cast<size_t>(kBrowserServer))
                {
                    const size_t nl = all.find('\n', p0); const std::string l = all.substr(p0, nl == std::string::npos ? std::string::npos : nl - p0);
                    p0 = nl == std::string::npos ? all.size() : nl + 1;
                    std::vector<std::string> f; size_t q = 0;
                    while (true) { const size_t t2 = l.find('\t', q); f.push_back(l.substr(q, t2 == std::string::npos ? std::string::npos : t2 - q)); if (t2 == std::string::npos) break; q = t2 + 1; }
                    if (f.size() < 6) continue;
                    s_browserAdr.push_back(f[0]);
                    z.push_back(f[1].substr(0, 16) + "  " + f[2] + "/" + f[3] + "  " + f[4] + "ms" + (f[5] == "1" ? "  PW" : ""));
                }
                const bool sucht = ::ps2xGsBridgeOption(28, 0) != 0;
                if (z.empty()) z.push_back(sucht ? "Searching ..." : "No servers found");
                while (z.size() < static_cast<size_t>(kBrowserServer)) z.push_back(" ");
                z.push_back("Refresh"); z.push_back("Back");
                return z; };
            constexpr uint32_t kText = 0x000F0000u, kHandler = 0x003F392Cu;
            if (s_origMenu && s_origLayout && s_neuItem && s_breite)
            {
                runtime.registerFunction(0x003F35F0u, [](uint8_t *r, R5900Context *c, PS2Runtime *rt) { s_baue = true; s_origMenu(r, c, rt); s_baue = false; });
                runtime.registerFunction(0x00253498u, [](uint8_t *r, R5900Context *c, PS2Runtime *rt) {
                    if (s_lobbyBau && s_lobbyItems.size() >= 3)
                    {
                        const uint32_t menu = GPR_U32(c, 4) & 0x1FFFFFFu;
                        if (s_lobbyModus == 1) s_lobbyMenu = menu;
                        const bool browser = s_lobbyModus >= 2;
                        const std::vector<std::string> bz = s_lobbyModus == 2 ? browserZeilen() : s_lobbyModus == 3 ? s_profilZeilen : std::vector<std::string>{};
                        const R5900Context save = *c;
                        auto rufe = [&](PS2Runtime::RecompiledFunction f, uint32_t adr, uint32_t a0, uint32_t a1, uint32_t a2) -> uint32_t {
                            SET_GPR_U32(c, 4, a0); SET_GPR_U32(c, 5, a1); SET_GPR_U32(c, 6, a2); SET_GPR_U32(c, 31, 0x003E9E28u);
                            c->pc = adr; f(r, c, rt); return GPR_U32(c, 2); };
                        auto w32 = [&](uint32_t a, uint32_t v) { std::memcpy(r + (a & 0x1FFFFFCu), &v, 4); };
                        auto r32 = [&](uint32_t a) { uint32_t v; std::memcpy(&v, r + (a & 0x1FFFFFCu), 4); return v; };
                        uint32_t tp = kLobbyText + 0x20u;
                        auto text = [&](const char *t) { const uint32_t a = tp; const size_t n = std::strlen(t) + 1; std::memcpy(r + a, t, n); tp += static_cast<uint32_t>((n + 15) & ~15u); return a; };
                        const int anzahl = browser ? kBrowserN : kLobbyN;
                        for (int i = 0; i < anzahl; ++i)
                        {
                            uint32_t it = i < 3 ? s_lobbyItems[static_cast<size_t>(i)] : rufe(s_neuItem, 0x0024C8E0u, menu, 0x23u, 0x100u) & 0x1FFFFFFu;
                            if (!it || it >= 0x1FFF000u) continue;
                            uint32_t ta;
                            if (browser) { ta = kBrowserText + static_cast<uint32_t>(i) * 0x40u; const std::string &t = bz[static_cast<size_t>(i)]; std::memcpy(r + ta, t.c_str(), std::min<size_t>(t.size() + 1, 0x3F)); r[ta + 0x3F] = 0; s_browserItem[i] = it; }
                            else ta = text(kLobbyEintraege[i]);
                            w32(it + 0x20, ta); w32(it + 0x1C, 0x15u); w32(it + 0x24, 3u);
                            w32(it + 0x28, (r32(it + 0x28) & ~1u) | 0x100u);   // never greyed out
                            w32(it + 0x34, rufe(s_breite, 0x002533F8u, it, 0u, 0u)); w32(it + 0x38, 5u); w32(it + 0x30, browser ? kBrowserItemAdr[i] : kLobbyItemAdr[i]);
                        }
                        const uint32_t hoehe = r32(menu + 0x14);
                        w32(menu + 0x20, hoehe); w32(menu + 0x28, hoehe);
                        *c = save;
                    }
                    if (s_baue.exchange(false))
                    {
                        const uint32_t menu = GPR_U32(c, 4);
                        static const char txt[] = "Online+";
                        std::memcpy(r + kText, txt, sizeof txt);
                        const R5900Context save = *c;
                        auto rufe = [&](PS2Runtime::RecompiledFunction f, uint32_t adr, uint32_t a0, uint32_t a1, uint32_t a2) -> uint32_t {
                            SET_GPR_U32(c, 4, a0); SET_GPR_U32(c, 5, a1); SET_GPR_U32(c, 6, a2); SET_GPR_U32(c, 31, 0x003F3854u);
                            c->pc = adr; f(r, c, rt); return GPR_U32(c, 2); };
                        auto w32 = [&](uint32_t a, uint32_t v) { std::memcpy(r + (a & 0x1FFFFFCu), &v, 4); };
                        const uint32_t item = rufe(s_neuItem, 0x0024C8E0u, menu, 0x23u, 0x100u) & 0x1FFFFFFu;
                        if (item && item < 0x1FFF000u)
                        {
                            w32(item + 0x20, kText); w32(item + 0x1C, 0x15u); w32(item + 0x24, 3u);
                            const uint32_t w = rufe(s_breite, 0x002533F8u, item, 0u, 0u);
                            w32(item + 0x34, w); w32(item + 0x38, 5u); w32(item + 0x30, kHandler);
                            // the builder copied the menu height (+0x14, grows with every item) to +0x20/+0x28 before
                            // this call - copy it again so the box shows all six rows instead of scrolling
                            uint32_t hoehe; std::memcpy(&hoehe, r + ((menu + 0x14) & 0x1FFFFFCu), 4);
                            w32(menu + 0x20, hoehe); w32(menu + 0x28, hoehe);
                            std::fprintf(stderr, "[Online+] main menu item added (item %08x, width %u)\n", item, w);
                        }
                        *c = save;
                    }
                    s_origLayout(r, c, rt);
                });
                // Online+ goes straight into the game's own network menu (host a game with bots / join one) - the same
                // screen as Arcade -> Network (its item handler 0x3E9C70 = 0x3D5A98(screen 0x5554F8)). The connection to
                // the Online+ server runs in the background, so online games show up in the game's own game list.
                static PS2Runtime::RecompiledFunction s_netzMenue = runtime.lookupFunction(0x003E9C70u);
                // ---- Online+ lobby: an own screen built with the game's menu engine (looks/behaves like every TS2 menu) ----
                // A screen is a 6-word descriptor {update, draw, 0, 0, builder, 0}; 0x3D5A98(descriptor) switches to it.
                // update 0x3E9F40 / draw 0x3E9F08 are the generic ones (menu input, triangle = back). The builder is ours
                // (registered at a free address) and creates the menu through the game's own functions, exactly like the
                // Arcade Type builder 0x3E9C90 does.
                // handlers must live inside the generated function table (0x200000..0x499F20): unused padding words after
                // "jr ra" in the menu code serve as addresses (never executed as code)

                static PS2Runtime::RecompiledFunction s_screen = runtime.lookupFunction(0x003D5A98u);
                static PS2Runtime::RecompiledFunction s_zurueck = runtime.lookupFunction(0x003D5A10u);
                static PS2Runtime::RecompiledFunction s_menuNeu = runtime.lookupFunction(0x0024C718u);
                static PS2Runtime::RecompiledFunction s_lay2 = runtime.lookupFunction(0x002534B0u);
                static PS2Runtime::RecompiledFunction s_strcpy = runtime.lookupFunction(0x00490180u);
                static PS2Runtime::RecompiledFunction s_f24db58 = runtime.lookupFunction(0x0024DB58u);
                static PS2Runtime::RecompiledFunction s_f2530e0 = runtime.lookupFunction(0x002530E0u);
                static PS2Runtime::RecompiledFunction s_f24d848 = runtime.lookupFunction(0x0024D848u);
                static PS2Runtime::RecompiledFunction s_f250620 = runtime.lookupFunction(0x00250620u);
                static PS2Runtime::RecompiledFunction s_f2507a8 = runtime.lookupFunction(0x002507A8u);
                static PS2Runtime::RecompiledFunction s_f3d9528 = runtime.lookupFunction(0x003D9528u);
                

                {
                    uint8_t *r0 = runtime.memory().getRDRAM();
                    const uint32_t desc[6] = {0x003E9F40u, 0x003E9F08u, 0u, 0u, kLobbyBau, 0u};
                    std::memcpy(r0 + kLobbyDesc, desc, sizeof desc);
                }
                // The lobby screen reuses the game's own Arcade Type builder 0x3E9C90 (so frame, glow, footer hints and
                // animation are exactly the game's) and rewrites its menu right before the layout call: the three items get
                // our texts/handlers, three more are added, the box is widened, the title replaced.
                static PS2Runtime::RecompiledFunction s_arcadeBau = runtime.lookupFunction(0x003E9C90u);
                // the box width has to be set before the items exist (they take their position from it)
                static PS2Runtime::RecompiledFunction s_menuNeuOrig = runtime.lookupFunction(0x0024C718u);
                runtime.registerFunction(0x0024C718u, [](uint8_t *r, R5900Context *c, PS2Runtime *rt) {
                    if (s_lobbyBau) SET_GPR_U32(c, 5, s_lobbyModus >= 2 ? 0x170u : 0xF0u);
                    s_menuNeuOrig(r, c, rt);
                });
                runtime.registerFunction(0x0024C8E0u, [](uint8_t *r, R5900Context *c, PS2Runtime *rt) {
                    s_neuItem(r, c, rt);
                    if (s_lobbyBau) s_lobbyItems.push_back(GPR_U32(c, 2) & 0x1FFFFFFu);
                });
                runtime.registerFunction(kLobbyBau, [](uint8_t *r, R5900Context *c, PS2Runtime *rt) {
                    const uint32_t screen = GPR_U32(c, 4) & 0x1FFFFFFu;
                    s_lobbyItems.clear(); s_lobbyBau = true; s_lobbyModus = 1;
                    ::ps2xGsBridgeOption(28, 1);   // fetch the server list in the background (Quick Match / browser)
                    c->pc = 0x003E9C90u; s_arcadeBau(r, c, rt);
                    s_lobbyBau = false;
                    // ---- right panel: profile (name, level/rank, XP, kills) as a second, non-selectable menu ----
                    {
                        const R5900Context save = *c;
                        auto rufe = [&](PS2Runtime::RecompiledFunction f, uint32_t adr, uint32_t a0, uint32_t a1, uint32_t a2) -> uint32_t {
                            SET_GPR_U32(c, 4, a0); SET_GPR_U32(c, 5, a1); SET_GPR_U32(c, 6, a2); SET_GPR_U32(c, 31, 0x003E9E28u);
                            c->pc = adr; f(r, c, rt); return GPR_U32(c, 2); };
                        auto w32 = [&](uint32_t a, uint32_t v) { std::memcpy(r + (a & 0x1FFFFFCu), &v, 4); };
                        auto r32 = [&](uint32_t a) { uint32_t v; std::memcpy(&v, r + (a & 0x1FFFFFCu), 4); return v; };
                        auto wf = [&](uint32_t a, float v) { std::memcpy(r + (a & 0x1FFFFFCu), &v, 4); };
                        auto rf2 = [&](uint32_t a) { float v; std::memcpy(&v, r + (a & 0x1FFFFFCu), 4); return v; };
                        uint32_t tp = kLobbyText + 0x400u;
                        auto text = [&](const std::string &t) { const uint32_t a = tp; std::memcpy(r + a, t.c_str(), t.size() + 1); tp += static_cast<uint32_t>((t.size() + 16) & ~15u); return a; };
                        // nickname from the settings (online_name), otherwise "Player"
                        const std::string name = opName();
                        static const char *kRang[] = {"Recruit", "Private", "Corporal", "Sergeant", "Lieutenant", "Captain", "Major", "Colonel", "Commander", "General", "Legend"};
                        const int lv = ::ps2xGsBridgeOption(24, 1), xp = ::ps2xGsBridgeOption(25, 0), kills = ::ps2xGsBridgeOption(26, 0), naechst = ::ps2xGsBridgeOption(27, 500);
                        const std::string zeilen[] = {name, kRang[std::clamp(lv / 5, 0, 10)], "Level " + std::to_string(lv),
                                                      "XP " + std::to_string(xp), "Kills " + std::to_string(kills)};
                        (void)naechst;
                        const uint32_t m1 = s_lobbyMenu;
                        const uint32_t m2 = rufe(s_menuNeuOrig, 0x0024C718u, screen, 0xD0u, 0u) & 0x1FFFFFFu;
                        if (m2 && m2 < 0x1FFF000u)
                        {
                            for (const auto &z : zeilen)
                            {
                                const uint32_t it = rufe(s_neuItem, 0x0024C8E0u, m2, 0x23u, 0x100u) & 0x1FFFFFFu;
                                if (!it || it >= 0x1FFF000u) continue;
                                w32(it + 0x20, text(z)); w32(it + 0x1C, 0x15u); w32(it + 0x24, 3u);
                                w32(it + 0x34, rufe(s_breite, 0x002533F8u, it, 0u, 0u)); w32(it + 0x38, 5u); w32(it + 0x30, 0u);
                                w32(it + 0x28, r32(it + 0x28) | 1u);   // not selectable
                            }
                            w32(m2 + 0x2C, 0x1Eu); w32(m2 + 0x30, 0x1Au); w32(m2 + 0x24, 0u);
                            w32(m2 + 0x28, r32(m2 + 0x14)); w32(m2 + 0x20, r32(m2 + 0x14));
                            wf(m2 + 0x18, static_cast<float>(static_cast<int32_t>(rufe(s_origLayout, 0x00253498u, m2, 0u, 0u))));
                            wf(m2 + 0x1C, static_cast<float>(static_cast<int32_t>(rufe(s_lay2, 0x002534B0u, m2, 0u, 0u))));
                            // side by side: lobby menu left, profile right (same top)
                            if (m1)
                            {
                                const float b1 = static_cast<float>(r32(m1 + 0x10)), b2 = static_cast<float>(r32(m2 + 0x10));
                                const float gesamt = b1 + 16.0f + b2, x0 = 320.0f - gesamt * 0.5f - 24.0f;   // a little left: the right edge of the picture clips
                                std::fprintf(stderr, "[Online+] lobby panels: menu1 x %.0f y %.0f, menu2 x %.0f y %.0f\n", rf2(m1 + 0x18), rf2(m1 + 0x1C), rf2(m2 + 0x18), rf2(m2 + 0x1C));
                                wf(m1 + 0x18, x0); wf(m2 + 0x18, x0 + b1 + 16.0f); wf(m2 + 0x1C, rf2(m1 + 0x1C));
                            }
                            w32(m2 + 0x188, 2u); w32(m2 + 0x184, r32(screen + 0x830) + 0xA);
                            rufe(s_f24db58, 0x0024DB58u, m2, 0u, 0u);
                            rufe(s_f2530e0, 0x002530E0u, m2, 0u, 0u);
                            rufe(s_f2507a8, 0x002507A8u, m2, 0u, 0u);
                        }
                        *c = save;
                    }
                    const char titel[] = "Online+ Lobby";
                    std::memcpy(r + kLobbyText, titel, sizeof titel);
                    uint32_t tp = kLobbyText; std::memcpy(r + ((screen + 0x828) & 0x1FFFFFCu), &tp, 4);
                    std::fprintf(stderr, "[Online+] lobby screen built (%zu items)\n", s_lobbyItems.size());
                });
                // item handlers: 0 quick match / 1 create lobby -> the game's network menu for now; 5 back
                // (function pointers without captures: one small lambda per item)
                static void (*s_lobbyItem)(int, uint8_t *, R5900Context *, PS2Runtime *) = [](int i, uint8_t *r, R5900Context *c, PS2Runtime *rt) {
                    std::fprintf(stderr, "[Online+] lobby item %d\n", i);
                    if (i == 0)
                    {
                        // Quick Match: no server chosen yet -> the best answering one from the list (most players, lowest ping)
                        std::lock_guard<std::mutex> lk(g_opServerM);
                        char buf[512]; ::ps2xGsBridgeTextGet(2, buf, sizeof buf);
                        if (g_opServerWahl.empty() && !buf[0])
                        {
                            static char liste[8192]; ::ps2xGsBridgeTextGet(1, liste, sizeof liste);
                            const std::string l(liste); const size_t t = l.find('\t');
                            if (t != std::string::npos) { g_opServerWahl = l.substr(0, t); ::ps2xGsBridgeText(4, g_opServerWahl.c_str()); std::fprintf(stderr, "[Online+] quick match: server %s\n", g_opServerWahl.c_str()); }
                        }
                    }
                    // Bot Match: the game's own Arcade "Custom" setup (bots, map, rules) - offline against bots
                    if (i == 3) { static PS2Runtime::RecompiledFunction s_custom = rt->lookupFunction(0x003E9C40u); if (s_custom) { c->pc = 0x003E9C40u; s_custom(r, c, rt); return; } }
                    if (i == 4 && s_screen) { SET_GPR_U32(c, 4, kProfilDesc); c->pc = 0x003D5A98u; s_screen(r, c, rt); return; }
                    if (i == 2 && s_screen) { SET_GPR_U32(c, 4, kBrowserDesc); c->pc = 0x003D5A98u; s_screen(r, c, rt); return; }
                    if ((i == 0 || i == 1) && s_netzMenue) { c->pc = 0x003E9C70u; s_netzMenue(r, c, rt); return; }
                    if (i == 5 && s_zurueck) { c->pc = 0x003D5A10u; s_zurueck(r, c, rt); return; }
                    SET_GPR_U32(c, 2, 0u); c->pc = GPR_U32(c, 31);
                };
                runtime.registerFunction(kLobbyItemAdr[0], [](uint8_t *r, R5900Context *c, PS2Runtime *rt) { s_lobbyItem(0, r, c, rt); });
                runtime.registerFunction(kLobbyItemAdr[1], [](uint8_t *r, R5900Context *c, PS2Runtime *rt) { s_lobbyItem(1, r, c, rt); });
                runtime.registerFunction(kLobbyItemAdr[2], [](uint8_t *r, R5900Context *c, PS2Runtime *rt) { s_lobbyItem(2, r, c, rt); });
                runtime.registerFunction(kLobbyItemAdr[3], [](uint8_t *r, R5900Context *c, PS2Runtime *rt) { s_lobbyItem(3, r, c, rt); });
                runtime.registerFunction(kLobbyItemAdr[4], [](uint8_t *r, R5900Context *c, PS2Runtime *rt) { s_lobbyItem(4, r, c, rt); });
                runtime.registerFunction(kLobbyItemAdr[5], [](uint8_t *r, R5900Context *c, PS2Runtime *rt) { s_lobbyItem(5, r, c, rt); });
                // ---- in-game server browser screen ----
                {
                    uint8_t *r0 = runtime.memory().getRDRAM();
                    const uint32_t desc[6] = {kBrowserUpd, 0x003E9F08u, 0u, 0u, kBrowserBau, 0u};
                    std::memcpy(r0 + kBrowserDesc, desc, sizeof desc);
                }
                static PS2Runtime::RecompiledFunction s_updGen = runtime.lookupFunction(0x003E9F40u);
                runtime.registerFunction(kBrowserBau, [](uint8_t *r, R5900Context *c, PS2Runtime *rt) {
                    const uint32_t screen = GPR_U32(c, 4) & 0x1FFFFFFu;
                    ::ps2xGsBridgeOption(28, 1);
                    s_lobbyItems.clear(); s_lobbyBau = true; s_lobbyModus = 2; s_seite = 2;
                    c->pc = 0x003E9C90u; s_arcadeBau(r, c, rt);
                    s_lobbyBau = false; s_lobbyModus = 1;
                    const char titel[] = "Server Browser";
                    std::memcpy(r + kLobbyText + 0x10, titel, sizeof titel);
                    uint32_t tp = kLobbyText + 0x10; std::memcpy(r + ((screen + 0x828) & 0x1FFFFFCu), &tp, 4);
                    std::fprintf(stderr, "[Online+] server browser built (%zu servers)\n", s_browserAdr.size());
                });
                {
                    uint8_t *r0 = runtime.memory().getRDRAM();
                    const uint32_t desc[6] = {kBrowserUpd, 0x003E9F08u, 0u, 0u, kProfilBau, 0u};
                    std::memcpy(r0 + kProfilDesc, desc, sizeof desc);
                }
                // ---- Profile & Ranks screen: name, rank, level, XP to the next level, kills, next rank ----
                runtime.registerFunction(kProfilBau, [](uint8_t *r, R5900Context *c, PS2Runtime *rt) {
                    const uint32_t screen = GPR_U32(c, 4) & 0x1FFFFFFu;
                    static const char *kRang[] = {"Recruit", "Private", "Corporal", "Sergeant", "Lieutenant", "Captain", "Major", "Colonel", "Commander", "General", "Legend"};
                    const int lv = ::ps2xGsBridgeOption(24, 1), xp = ::ps2xGsBridgeOption(25, 0), kills = ::ps2xGsBridgeOption(26, 0), naechst = ::ps2xGsBridgeOption(27, 500);
                    const int rg = std::clamp(lv / 5, 0, 10);
                    s_profilZeilen = {opName(), "Rank: " + std::string(kRang[rg]), "Level " + std::to_string(lv),
                                      "XP " + std::to_string(xp) + " / " + std::to_string(naechst), "Kills " + std::to_string(kills),
                                      rg < 10 ? "Next rank: " + std::string(kRang[rg + 1]) + " (Lv " + std::to_string((rg + 1) * 5) + ")" : std::string("Top rank reached"),
                                      " ", "Back"};
                    s_lobbyItems.clear(); s_lobbyBau = true; s_lobbyModus = 3; s_seite = 3;
                    c->pc = 0x003E9C90u; s_arcadeBau(r, c, rt);
                    s_lobbyBau = false; s_lobbyModus = 1;
                    // info rows are not selectable: only "Back" (last row)
                    for (int i = 0; i < kBrowserN - 1; ++i) { const uint32_t it = s_browserItem[i]; if (it && it < 0x1FFF000u) { uint32_t f; std::memcpy(&f, r + it + 0x28, 4); f |= 1u; std::memcpy(r + it + 0x28, &f, 4); } }
                    const char titel[] = "Profile & Ranks";
                    std::memcpy(r + kBrowserText + 0x300u, titel, sizeof titel);
                    uint32_t tp = kBrowserText + 0x300u; std::memcpy(r + ((screen + 0x828) & 0x1FFFFFCu), &tp, 4);
                    std::fprintf(stderr, "[Online+] profile screen built (level %d)\n", lv);
                });
                // update: the game's generic menu update, plus refreshing the rows twice a second while the list changes
                runtime.registerFunction(kBrowserUpd, [](uint8_t *r, R5900Context *c, PS2Runtime *rt) {
                    const R5900Context save = *c;
                    static int n = 0;
                    if (s_seite == 2 && ++n % 30 == 0 && s_breite)
                    {
                        const std::vector<std::string> bz = browserZeilen();
                        for (int i = 0; i < kBrowserServer; ++i)
                        {
                            const uint32_t it = s_browserItem[i]; if (!it || it >= 0x1FFF000u) continue;
                            const uint32_t ta = kBrowserText + static_cast<uint32_t>(i) * 0x40u; const std::string &t = bz[static_cast<size_t>(i)];
                            if (std::strncmp(reinterpret_cast<const char *>(r + ta), t.c_str(), 0x3F) == 0) continue;
                            std::memcpy(r + ta, t.c_str(), std::min<size_t>(t.size() + 1, 0x3F)); r[ta + 0x3F] = 0;
                            SET_GPR_U32(c, 4, it); SET_GPR_U32(c, 31, kBrowserUpd); c->pc = 0x002533F8u; s_breite(r, c, rt);
                            const uint32_t w = GPR_U32(c, 2); std::memcpy(r + ((it + 0x34) & 0x1FFFFFCu), &w, 4);
                            *c = save;
                        }
                    }
                    c->pc = 0x003E9F40u; s_updGen(r, c, rt);
                });
                static void (*s_browserItemFn)(int, uint8_t *, R5900Context *, PS2Runtime *) = [](int i, uint8_t *r, R5900Context *c, PS2Runtime *rt) {
                    if (s_seite == 3) { if (i == kBrowserN - 1 && s_zurueck) { c->pc = 0x003D5A10u; s_zurueck(r, c, rt); return; } }
                    else if (i < kBrowserServer && i < static_cast<int>(s_browserAdr.size()))
                    {
                        { std::lock_guard<std::mutex> lk(g_opServerM); g_opServerWahl = s_browserAdr[static_cast<size_t>(i)]; }
                        ::ps2xGsBridgeText(4, s_browserAdr[static_cast<size_t>(i)].c_str());
                        std::fprintf(stderr, "[Online+] server chosen: %s\n", s_browserAdr[static_cast<size_t>(i)].c_str());
                        if (s_netzMenue) { c->pc = 0x003E9C70u; s_netzMenue(r, c, rt); return; }
                    }
                    else if (i == kBrowserN - 2) ::ps2xGsBridgeOption(28, 1);   // Refresh
                    else if (i == kBrowserN - 1 && s_zurueck) { c->pc = 0x003D5A10u; s_zurueck(r, c, rt); return; }
                    SET_GPR_U32(c, 2, 0u); c->pc = GPR_U32(c, 31);
                };
                runtime.registerFunction(kBrowserItemAdr[0], [](uint8_t *r, R5900Context *c, PS2Runtime *rt) { s_browserItemFn(0, r, c, rt); });
                runtime.registerFunction(kBrowserItemAdr[1], [](uint8_t *r, R5900Context *c, PS2Runtime *rt) { s_browserItemFn(1, r, c, rt); });
                runtime.registerFunction(kBrowserItemAdr[2], [](uint8_t *r, R5900Context *c, PS2Runtime *rt) { s_browserItemFn(2, r, c, rt); });
                runtime.registerFunction(kBrowserItemAdr[3], [](uint8_t *r, R5900Context *c, PS2Runtime *rt) { s_browserItemFn(3, r, c, rt); });
                runtime.registerFunction(kBrowserItemAdr[4], [](uint8_t *r, R5900Context *c, PS2Runtime *rt) { s_browserItemFn(4, r, c, rt); });
                runtime.registerFunction(kBrowserItemAdr[5], [](uint8_t *r, R5900Context *c, PS2Runtime *rt) { s_browserItemFn(5, r, c, rt); });
                runtime.registerFunction(kBrowserItemAdr[6], [](uint8_t *r, R5900Context *c, PS2Runtime *rt) { s_browserItemFn(6, r, c, rt); });
                runtime.registerFunction(kBrowserItemAdr[7], [](uint8_t *r, R5900Context *c, PS2Runtime *rt) { s_browserItemFn(7, r, c, rt); });
                runtime.registerFunction(kHandler, [](uint8_t *r, R5900Context *c, PS2Runtime *rt) {
                    std::fprintf(stderr, "[Online+] main menu item selected -> lobby screen\n");
                    ::ps2xGsBridgeOption(17, 1);
                    if (s_screen) { SET_GPR_U32(c, 4, kLobbyDesc); c->pc = 0x003D5A98u; s_screen(r, c, rt); return; }
                    SET_GPR_U32(c, 2, 0u);
                    c->pc = GPR_U32(c, 31);
                });
                // Flush the Online+ message queue from the game thread: 0x367B60 (number of players) is asked many times per
                // frame while a match runs. After the original call the queued texts go to screen 0 through 0x32A9B0.
                static PS2Runtime::RecompiledFunction s_anzahl = runtime.lookupFunction(0x00367B60u);
                static PS2Runtime::RecompiledFunction s_melden = runtime.lookupFunction(0x0032A9B0u);
                if (s_anzahl && s_melden)
                    runtime.registerFunction(0x00367B60u, [](uint8_t *r, R5900Context *c, PS2Runtime *rt) {
                        s_anzahl(r, c, rt);
                        static bool drin = false;
                        if (drin) return;
                        // TS2_MELDETEST="tick:type:text;..." (tests): push texts at VBlank ticks
                        static std::vector<std::tuple<uint64_t, int, std::string>> test = [] {
                            std::vector<std::tuple<uint64_t, int, std::string>> v; const char *e = std::getenv("TS2_MELDETEST"); if (!e) return v;
                            std::string all(e); size_t p = 0;
                            while (p < all.size()) { size_t q2 = all.find(';', p); std::string it = all.substr(p, q2 == std::string::npos ? std::string::npos : q2 - p);
                                unsigned long long tk = 0; int ty = 0; const size_t c1 = it.find(':'), c2 = it.find(':', c1 + 1);
                                if (c1 != std::string::npos && c2 != std::string::npos) { tk = std::strtoull(it.c_str(), nullptr, 10); ty = std::atoi(it.c_str() + c1 + 1); v.emplace_back(tk, ty, it.substr(c2 + 1)); }
                                if (q2 == std::string::npos) break; p = q2 + 1; }
                            return v; }();
                        if (!test.empty())
                        {
                            const uint64_t tk = rt->memory().gs().vsyncTick.load();
                            for (auto it = test.begin(); it != test.end();) { if (std::get<0>(*it) <= tk) { ts2melde::push(std::get<2>(*it), std::get<1>(*it)); it = test.erase(it); } else ++it; }
                        }
                        std::string t; int typ = 3;
                        if (!ts2melde::pop(t, typ)) return;
                        drin = true;
                        constexpr uint32_t kPuffer = 0x000F0100u;   // free RAM (like the menu text at 0xF0000)
                        if (t.size() > 100) t.resize(100);
                        std::memcpy(r + kPuffer, t.c_str(), t.size() + 1);
                        const R5900Context save = *c;
                        SET_GPR_U32(c, 4, 0u); SET_GPR_U32(c, 5, static_cast<uint32_t>(std::clamp(typ, 1, 6))); SET_GPR_U32(c, 6, kPuffer);
                        float null = 0.0f; std::memcpy(&c->f[12], &null, 4);
                        SET_GPR_U32(c, 31, 0x00367B60u); c->pc = 0x0032A9B0u;
                        s_melden(r, c, rt);
                        *c = save;
                        drin = false;
                        static const bool lg = std::getenv("TS2_IMPACT_LOG") != nullptr;
                        if (lg) std::fprintf(stderr, "[melde] type %d: %s\n", typ, t.c_str());
                    });
            }
        }
        // Diagnosis: how often do the GS handler (frame done, INTC 0) and the render timeout alarm fire?
        static PS2Runtime::RecompiledFunction s_origGs = runtime.lookupFunction(0x0020267Cu);
        static PS2Runtime::RecompiledFunction s_origAlarm = runtime.lookupFunction(0x00202150u);
        static std::atomic<uint64_t> s_gsHits{0}, s_alarmHits{0};
        if (s_origGs)
            runtime.registerFunction(0x0020267Cu, [](uint8_t *r, R5900Context *c, PS2Runtime *rt) { ++s_gsHits; ++g_gsIrqHits; s_origGs(r, c, rt); });
        if (s_origAlarm)
            runtime.registerFunction(0x00202150u, [](uint8_t *r, R5900Context *c, PS2Runtime *rt) {
                const uint64_t n = ++s_alarmHits;
                if (n <= 5 || n % 100 == 0)
                    std::fprintf(stderr, "[TS2] render timeout alarm #%llu at vsync %llu (GS handler so far %llu)\n", (unsigned long long)n,
                                 (unsigned long long)rt->memory().gs().vsyncTick.load(), (unsigned long long)s_gsHits.load());
                s_origAlarm(r, c, rt); });
        // TS2_FPUTEST=1: call the game's translated libm functions with edge values (PS2 FPU: no Inf/NaN, x/0 = +-Fmax)
        if (std::getenv("TS2_FPUTEST"))
        {
            struct T { const char *name; uint32_t addr; } fns[] = {{"acosf", 0x00484870u}, {"sinf", 0x00483DA8u}, {"cosf", 0x00483CA8u}};
            const uint32_t ins[] = {0xC1233333u /*-10.2*/, 0x3F800001u /*1.0000001*/, 0x3F800000u, 0x7F7FFFFFu /*+Fmax*/, 0xFF7FFFFFu,
                                    0x7F800000u /*Inf pattern*/, 0x7FC00000u /*NaN pattern*/, 0x4B800000u /*2^24*/, 0x40490FDBu /*pi*/};
            for (const auto &fn : fns)
            {
                auto f = runtime.lookupFunction(fn.addr);
                if (!f) { std::fprintf(stderr, "[FPUTEST] %s missing\n", fn.name); continue; }
                for (uint32_t in : ins)
                {
                    R5900Context c{};
                    c.r[29] = _mm_set_epi64x(0, 0x01F80000);
                    std::memcpy(&c.f[12], &in, 4);
                    f(runtime.memory().getRDRAM(), &c, &runtime);
                    uint32_t out; std::memcpy(&out, &c.f[0], 4);
                    float fi, fo; std::memcpy(&fi, &in, 4); std::memcpy(&fo, &out, 4);
                    std::fprintf(stderr, "[FPUTEST] %s(0x%08X = %g) = 0x%08X = %g\n", fn.name, in, (double)fi, out, (double)fo);
                }
            }
        }
        std::fprintf(stderr, "[TS2 override] applied (SLUS-203.14)\n");
    }
}

PS2_REGISTER_GAME_OVERRIDE("TimeSplitters 2 (NTSC-U)", "SLUS_203.14", 0x00200008u, 0u, applyTs2)

// =====================================================================================================================
// Online multiplayer (virtual LAN through a TS2 server, see src/server/ts2_server.cpp)
//
// TS2 has a LAN mode for the PS2 network adapter. The chain in the original:
//   Boot: DEV9.IRX (result 0 = adapter present) -> network menu -> load INET/NETCNF/EZNETCNF -> network configuration
//   from the memory card (ezNetCnf) -> INETCTL/SMAP/MSIFRPC/LIBNETB -> connection (ezNetCtl) -> the game's own socket
//   layer (0x479178 Open, 0x479378 SendTo, ...) on top of libnet (Sony INET through SIF RPC + ring buffers on the IOP).
// Replaced:
//   * module loading (0x44B000/0x44B020): DEV9 reports "adapter present", the network modules count as loaded
//   * ezNetCnf/ezNetCtl: one configuration "TS2 Online", connection "connected" right away
//   * the backend of the socket layer (0x479F90.., 16 entry points): a tunnel to the TS2 server instead of libnet.
//     Every player gets a virtual IP 10.13.x.y there; broadcasts (game search) go to everyone on the server.
// Socket types of the game layer: 0 TCP connect, 1 UDP, 2 TCP wait (the peer connects), 3 ICMP (ping).
// Backend error codes: 0 ok, 1 error, 2 parameter, 5 not initialised, 0xD connection closed.
//
// Enable: TS2_ONLINE_SERVER=host[:port] (default port 27713), TS2_ONLINE_NAME, TS2_ONLINE_PASSWORT
// or ~/.config/ts2-native/online.ini (server=, name=, passwort=). TS2_NETLOG=1: log every call.
// =====================================================================================================================
#include <condition_variable>
#include <deque>
#include <fstream>
#if defined(__linux__)
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#define TS2_NETZ_SOCKETS 1
namespace ts2netz
{
    inline int netzSchliessen(int s) { return ::close(s); }
    inline void nichtBlockierend(int s) { fcntl(s, F_SETFL, O_NONBLOCK); }
    inline int netzPoll(pollfd *p, int n, int ms) { return ::poll(p, nfds_t(n), ms); }
    constexpr int kSendeFlags = MSG_NOSIGNAL;
    inline bool netzStart() { return true; }
}
#elif defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>

// Laptops with two graphics chips: Nvidia and AMD drivers read these two symbols from the program file and then run the
// game on the fast card instead of the power-saving one (without them Windows picked the Intel chip - laptop test,
// GTX 1050: "Intel(R) HD Graphics 630"). They have to be exported from the .exe itself, a DLL does not count.
#ifdef _WIN32
extern "C" {
__declspec(dllexport) unsigned long NvOptimusEnablement = 0x00000001;
__declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
}
#endif
#define TS2_NETZ_SOCKETS 1
namespace ts2netz
{
    inline int netzSchliessen(int s) { return closesocket(SOCKET(s)); }
    inline void nichtBlockierend(int s) { u_long an = 1; ioctlsocket(SOCKET(s), FIONBIO, &an); }
    inline int netzPoll(pollfd *p, int n, int ms) { return WSAPoll(p, ULONG(n), ms); }
    constexpr int kSendeFlags = 0;
    inline bool netzStart() { static const bool ok = [] { WSADATA d; return WSAStartup(MAKEWORD(2, 2), &d) == 0; }(); return ok; }
}
#else
#define TS2_NETZ_SOCKETS 0
#endif
void sub_0044ADD8_0x44add8(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime); // Stub sceSifLoadModule (generated)
void ps2xGsBridgeOnlineBrowser();                   // gs_frontend.cpp: open the server browser in the game menu
void ps2xGsBridgeOnlineStatus(const char *text);    // gs_frontend.cpp: connection status in the server browser

namespace ts2netz
{
    constexpr uint16_t kProtokoll = 1;
    constexpr uint16_t kStandardPort = 27713;
    constexpr uint32_t kBroadcast = 0x0A0DFFFFu;
    enum : uint8_t
    {
        M_HELLO = 1, M_WELCOME = 2, M_REJECT = 3, M_DGRAM = 10, M_SCONNECT = 20, M_SINCOMING = 21, M_SACCEPT = 22,
        M_SRESULT = 23, M_SDATA = 24, M_SCLOSE = 25, M_PING = 30, M_KEEPALIVE = 40, M_INFO = 41, M_OPSTAT = 50, M_OPPROFIL = 51,
    };

    struct Konfig
    {
        bool aktiv = false;
        std::string host, name, passwort;
        uint16_t port = kStandardPort;
        bool log = false;
    };

    inline std::string trim(std::string s)
    {
        while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
        size_t i = 0;
        while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
        return s.substr(i);
    }

    // Settings file of the game menu (ESC/F1 -> Online): <XDG_CONFIG_HOME|~/.config>/<PS2XGS_CFGNAME>/einstellungen.ini
    inline std::string iniPfad()
    {
        if (const char *c = std::getenv("PS2XGS_CFG"); c && *c) return c;
        const char *xdg = std::getenv("XDG_CONFIG_HOME");
        const char *home = std::getenv("HOME");
        const char *name = std::getenv("PS2XGS_CFGNAME");
        const std::string basis = xdg && *xdg ? std::string(xdg) : std::string(home ? home : ".") + "/.config";
        return basis + "/" + (name && *name ? name : "ts2-recomp") + "/einstellungen.ini";
    }

    // Online+ profile: a random id + secret key, created once and kept in onlineplus_profil.ini next to the settings
    // (the launcher's update backups include it). The server keeps level/XP per id, so the rank stays with the player
    // on every server and computer; nobody else can write to it without the key. No personal data is involved.
    std::string profilDatei() { std::string p = iniPfad(); const size_t e = p.find_last_of("/\\"); return (e == std::string::npos ? std::string(".") : p.substr(0, e)) + "/onlineplus_profil.ini"; }
    bool profil(std::string &id, std::string &schluessel)
    {
        std::ifstream in(profilDatei());
        for (std::string z; std::getline(in, z);)
        {
            if (z.rfind("id=", 0) == 0) id = trim(z.substr(3));
            else if (z.rfind("schluessel=", 0) == 0) schluessel = trim(z.substr(11));
        }
        auto ok = [](const std::string &h) { return h.size() == 32 && h.find_first_not_of("0123456789abcdef") == std::string::npos; };
        if (ok(id) && ok(schluessel)) return true;
        std::random_device rd;
        auto hex = [&] { std::string h; for (int i = 0; i < 4; ++i) { char b[9]; std::snprintf(b, sizeof b, "%08x", rd()); h += b; } return h; };
        id = hex(); schluessel = hex();
        std::ofstream out(profilDatei());
        out << "# Online+ profile (keep this file: it holds your rank on the Online+ servers)\nid=" << id << "\nschluessel=" << schluessel << "\n";
        return static_cast<bool>(out);
    }

    Konfig lesen()
    {
        Konfig c;
        std::string server, name, pw;
#if TS2_NETZ_SOCKETS
        std::ifstream in(iniPfad());
        for (std::string z; std::getline(in, z);)
        {
            const auto p = z.find('=');
            if (z.empty() || z[0] == '#' || p == std::string::npos) continue;
            const std::string key = trim(z.substr(0, p)), val = trim(z.substr(p + 1));
            if (key == "online_server") server = val;
            else if (key == "online_name") name = val;
            else if (key == "online_passwort") pw = val;
        }
#endif
        { std::lock_guard<std::mutex> lk(g_opServerM); if (!g_opServerWahl.empty()) server = g_opServerWahl; }   // Online+ in-game browser
        if (const char *e = std::getenv("TS2_ONLINE_SERVER")) server = e;
        if (const char *e = std::getenv("TS2_ONLINE_NAME")) name = e;
        if (const char *e = std::getenv("TS2_ONLINE_PASSWORT")) pw = e;
        // never the system user name (it would be sent to a public server): a neutral default instead
        if (name.empty()) { std::random_device rd; name = "Player" + std::to_string(1000 + rd() % 9000); }
        server = trim(server);
        if (!server.empty())
        {
            std::string h = server; // host, host:port, [v6]:port
            if (h[0] == '[' && h.find(']') != std::string::npos)
            {
                const size_t e = h.find(']');
                if (e + 1 < h.size() && h[e + 1] == ':') c.port = uint16_t(std::atoi(h.c_str() + e + 2));
                h = h.substr(1, e - 1);
            }
            else if (std::count(h.begin(), h.end(), ':') == 1)
            {
                c.port = uint16_t(std::atoi(h.c_str() + h.find(':') + 1));
                h = h.substr(0, h.find(':'));
            }
            c.host = h;
            c.aktiv = TS2_NETZ_SOCKETS && !h.empty() && c.port != 0;
        }
        c.name = name;
        c.passwort = pw;
        c.log = std::getenv("TS2_NETLOG") != nullptr;
        return c;
    }

    Konfig &konfig()
    {
        static Konfig k = lesen();
        return k;
    }

    // Report the network adapter: always (online mode selectable), except with TS2_ONLINE=0 or without socket support
    inline bool adapterAn()
    {
        static const bool an = [] { const char *e = std::getenv("TS2_ONLINE"); return TS2_NETZ_SOCKETS && !(e && *e == '0'); }();
        return an;
    }

    inline std::string ipText(uint32_t ip)
    {
        char b[20];
        std::snprintf(b, sizeof b, "%u.%u.%u.%u", ip >> 24, (ip >> 16) & 255, (ip >> 8) & 255, ip & 255);
        return b;
    }
    inline bool ipLesen(const std::string &s, uint32_t &ip)
    {
        unsigned a, b, c, d;
        char rest;
        if (std::sscanf(s.c_str(), "%u.%u.%u.%u%c", &a, &b, &c, &d, &rest) != 4 || a > 255 || b > 255 || c > 255 || d > 255) return false;
        ip = (a << 24) | (b << 16) | (c << 8) | d;
        return true;
    }
    inline void put16(std::string &s, uint16_t v) { s.push_back(char(v)); s.push_back(char(v >> 8)); }
    inline void put32(std::string &s, uint32_t v) { for (int i = 0; i < 4; ++i) s.push_back(char(v >> (8 * i))); }
    inline uint16_t get16(const uint8_t *p) { return uint16_t(p[0] | (p[1] << 8)); }
    inline uint32_t get32(const uint8_t *p) { return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24); }

    struct Datagramm { uint32_t vip; uint16_t port; std::string daten; };
    struct PingAntwort { uint32_t vip; uint16_t id; };

    struct Sock
    {
        int typ = 1;
        uint16_t lport = 0;
        std::deque<Datagramm> rx;        // UDP
        uint32_t sid = 0, cid = 0;       // TCP stream
        int zustand = 0;                 // 0 waiting, 1 connecting, 2 open, 3 closed/refused
        std::string srx;                 // TCP receive buffer
        uint32_t peerVip = 0;
        uint16_t peerPort = 0;
        std::deque<PingAntwort> prx;     // ICMP
    };

#if TS2_NETZ_SOCKETS
    class Tunnel
    {
    public:
        std::mutex m;
        std::condition_variable cv;
        int zustand = 0; // 0 off, 1 connecting, 2 online, 3 error
        std::string fehler, servername, motd;
        uint32_t vip = 0, token = 0;
        int spieler = 0;
        std::map<int, Sock> socks;
        int naechsterHandle = 1;
        uint16_t naechsterPort = 50000;
        uint32_t naechsteCid = 1;

        // Start (idempotent); waits until online or error, at most ms
        bool online(int ms)
        {
            std::unique_lock<std::mutex> l(m);
            if (zustand == 0 || zustand == 3)
            {
                zustand = 1;
                fehler.clear();
                if (thread.joinable()) { l.unlock(); thread.join(); l.lock(); }
                thread = std::thread([this] { lauf(); });
            }
            cv.wait_for(l, std::chrono::milliseconds(ms), [this] { return zustand == 2 || zustand == 3; });
            return zustand == 2;
        }

        // Disconnect from the server (e.g. another server was chosen); game sockets are dropped
        void beenden()
        {
            {
                std::lock_guard<std::mutex> l(m);
                if (zustand != 1 && zustand != 2) return;
                trennen = true;
            }
            wecken();
            if (thread.joinable()) thread.join();
            std::lock_guard<std::mutex> l(m);
            zustand = 0;
            trennen = false;
            socks.clear();
            vip = 0;
            std::fprintf(stderr, "[ONLINE] disconnected from the server\n");
        }

        void senden(uint8_t typ, const std::string &nutz)
        {
            std::string f;
            put32(f, uint32_t(nutz.size() + 1));
            f.push_back(char(typ));
            f += nutz;
            std::lock_guard<std::mutex> l(ausM);
            if (aus.size() < (16u << 20)) aus += f;
            wecken();
        }

        void datagramm(uint16_t srcport, uint32_t dstvip, uint16_t dstport, const uint8_t *d, size_t n)
        {
            if (dstvip == vip) // to itself
            {
                std::lock_guard<std::mutex> l(m);
                zustellen(vip, srcport, dstport, d, n);
                return;
            }
            if (udpOk.load() && n <= 1400)
            {
                std::string p;
                put32(p, token); put32(p, vip); put16(p, srcport); put32(p, dstvip); put16(p, dstport);
                p.append(reinterpret_cast<const char *>(d), n);
                if (int(::sendto(udp, p.data(), int(p.size()), 0, reinterpret_cast<sockaddr *>(&srv), srvLen)) == int(p.size())) return;
            }
            std::string p;
            put16(p, srcport); put32(p, dstvip); put16(p, dstport);
            p.append(reinterpret_cast<const char *>(d), n);
            senden(M_DGRAM, p);
        }

        // m must be held
        void zustellen(uint32_t srcvip, uint16_t srcport, uint16_t dstport, const uint8_t *d, size_t n)
        {
            for (auto &kv : socks)
            {
                Sock &s = kv.second;
                if (s.typ == 1 && s.lport == dstport)
                {
                    if (s.rx.size() >= 512) s.rx.pop_front();
                    s.rx.push_back({srcvip, srcport, std::string(reinterpret_cast<const char *>(d), n)});
                    return;
                }
            }
        }

    private:
        std::thread thread;
        int tcp = -1, udp = -1, weck = -1; // weck: UDP socket on 127.0.0.1 that wakes the network thread (instead of a pipe, also on Windows)
        sockaddr_in weckAdr{};
        sockaddr_storage srv{};
        socklen_t srvLen = 0;
        std::mutex ausM;
        std::string aus, ein;
        std::atomic<bool> udpOk{false};
        bool trennen = false; // m

        void wecken() { if (weck >= 0) { const char c = 1; (void)::sendto(weck, &c, 1, 0, reinterpret_cast<const sockaddr *>(&weckAdr), sizeof weckAdr); } }

        void scheitern(const std::string &grund)
        {
            std::lock_guard<std::mutex> l(m);
            if (zustand != 3) { std::fprintf(stderr, "[ONLINE] connection: %s\n", grund.c_str()); ::ps2xGsBridgeOnlineStatus(("Not connected: " + grund).c_str()); }
            fehler = grund;
            zustand = 3;
            for (auto &kv : socks) if (kv.second.typ == 0 || kv.second.typ == 2) if (kv.second.zustand < 3 && kv.second.zustand > 0) kv.second.zustand = 3;
            cv.notify_all();
        }

        bool verbinden()
        {
            if (!netzStart()) { scheitern("Network (Winsock) not available"); return false; }
            const Konfig &k = konfig();
            addrinfo hint{}, *res = nullptr;
            hint.ai_family = AF_UNSPEC;
            hint.ai_socktype = SOCK_STREAM;
            const std::string port = std::to_string(k.port);
            if (getaddrinfo(k.host.c_str(), port.c_str(), &hint, &res) != 0 || !res) { scheitern("Server \"" + k.host + "\" not found"); return false; }
            for (addrinfo *a = res; a; a = a->ai_next)
            {
                const int fd = int(::socket(a->ai_family, SOCK_STREAM, 0));
                if (fd < 0) continue;
                nichtBlockierend(fd);
                ::connect(fd, a->ai_addr, a->ai_addrlen);
                pollfd p{decltype(pollfd::fd)(fd), POLLOUT, 0};
                int err = 0;
                socklen_t el = sizeof err;
                if (netzPoll(&p, 1, 4000) == 1 && getsockopt(fd, SOL_SOCKET, SO_ERROR, reinterpret_cast<char *>(&err), &el) == 0 && err == 0)
                {
                    tcp = fd;
                    std::memcpy(&srv, a->ai_addr, a->ai_addrlen);
                    srvLen = a->ai_addrlen;
                    break;
                }
                netzSchliessen(fd);
            }
            freeaddrinfo(res);
            if (tcp < 0) { scheitern("Server " + k.host + ":" + port + " does not answer"); return false; }
            int ja = 1;
            setsockopt(tcp, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char *>(&ja), sizeof ja);
            udp = int(::socket(srv.ss_family, SOCK_DGRAM, 0));
            if (udp >= 0) nichtBlockierend(udp);
            return true;
        }

        void nachricht(uint8_t typ, const uint8_t *p, size_t n)
        {
            std::unique_lock<std::mutex> l(m);
            switch (typ)
            {
            case M_WELCOME:
                if (n < 8) break;
                vip = get32(p);
                token = get32(p + 4);
                {
                    const std::string rest(reinterpret_cast<const char *>(p + 8), n - 8);
                    servername = rest.substr(0, rest.find('\0'));
                    motd = rest.find('\0') != std::string::npos ? rest.substr(rest.find('\0') + 1) : "";
                }
                zustand = 2;
                {
                    std::string id, sl;
                    if (profil(id, sl))
                    {
                        std::string pr;
                        auto bytes = [&](const std::string &h) { for (size_t i = 0; i + 1 < h.size(); i += 2) pr.push_back(char(std::stoi(h.substr(i, 2), nullptr, 16))); };
                        bytes(id); bytes(sl);
                        l.unlock(); senden(M_OPPROFIL, pr); l.lock();
                    }
                }
                std::fprintf(stderr, "[ONLINE] connected to \"%s\" as %s%s%s\n", servername.c_str(), ipText(vip).c_str(),
                             motd.empty() ? "" : " - ", motd.c_str());
                ::ps2xGsBridgeOnlineStatus(("Connected to \"" + servername + "\"" + (motd.empty() ? "" : " - " + motd)).c_str());
                cv.notify_all();
                break;
            case M_REJECT:
                l.unlock();
                scheitern("abgelehnt: " + std::string(reinterpret_cast<const char *>(p), n));
                break;
            case M_INFO:
                if (n >= 4) spieler = int(get32(p));
                break;
            case M_OPPROFIL:
                // server copy of the profile: xp (2x u32), kills, hits -> the game shows the higher of local and server values
                if (n >= 16)
                {
                    const uint64_t xp = uint64_t(get32(p)) | (uint64_t(get32(p + 4)) << 32);
                    const std::string t = std::to_string(xp) + "\t" + std::to_string(get32(p + 8)) + "\t" + std::to_string(get32(p + 12));
                    ::ps2xGsBridgeText(3, t.c_str());
                }
                break;
            case M_DGRAM:
                if (n >= 8) zustellen(get32(p), get16(p + 4), get16(p + 6), p + 8, n - 8);
                break;
            case M_SINCOMING:
            {
                if (n < 12) break;
                const uint32_t sid = get32(p), src = get32(p + 4);
                const uint16_t sport = get16(p + 8), dport = get16(p + 10);
                bool ok = false;
                for (auto &kv : socks)
                {
                    Sock &s = kv.second;
                    if (s.typ == 2 && s.lport == dport && s.zustand == 0)
                    {
                        s.sid = sid; s.zustand = 2; s.peerVip = src; s.peerPort = sport;
                        ok = true;
                        break;
                    }
                }
                l.unlock();
                std::string r;
                put32(r, sid);
                r.push_back(ok ? 1 : 0);
                senden(M_SACCEPT, r);
                if (konfig().log) std::fprintf(stderr, "[ONLINE] incoming from %s:%u to port %u -> %s\n", ipText(src).c_str(), sport, dport, ok ? "angenommen" : "abgelehnt");
                break;
            }
            case M_SRESULT:
            {
                if (n < 9) break;
                const uint32_t cid = get32(p), sid = get32(p + 4);
                for (auto &kv : socks)
                    if (kv.second.cid == cid && kv.second.zustand == 1)
                    {
                        kv.second.sid = sid;
                        kv.second.zustand = p[8] ? 2 : 3;
                    }
                cv.notify_all();
                break;
            }
            case M_SDATA:
            {
                if (n < 4) break;
                const uint32_t sid = get32(p);
                for (auto &kv : socks)
                    if (kv.second.sid == sid && kv.second.zustand == 2 && kv.second.srx.size() < (4u << 20))
                        kv.second.srx.append(reinterpret_cast<const char *>(p + 4), n - 4);
                break;
            }
            case M_SCLOSE:
            {
                if (n < 4) break;
                const uint32_t sid = get32(p);
                for (auto &kv : socks)
                    if (kv.second.sid == sid) kv.second.zustand = 3;
                break;
            }
            case M_PING:
            {
                if (n < 11) break;
                const uint32_t von = get32(p);
                const uint16_t id = get16(p + 4);
                if (!p[10]) // query -> answer right away
                {
                    l.unlock();
                    std::string r;
                    put32(r, von);
                    r.append(reinterpret_cast<const char *>(p + 4), 6);
                    r.push_back(1);
                    senden(M_PING, r);
                    break;
                }
                for (auto &kv : socks)
                    if (kv.second.typ == 3 && kv.second.prx.size() < 64) kv.second.prx.push_back({von, id});
                break;
            }
            default:
                break;
            }
        }

        void udpLesen()
        {
            uint8_t b[2048];
            for (;;)
            {
                const int n = int(::recv(udp, reinterpret_cast<char *>(b), sizeof b, 0));
                if (n < 0) return;
                if (n == 4 && std::memcmp(b, "T2OK", 4) == 0)
                {
                    if (!udpOk.exchange(true)) std::fprintf(stderr, "[ONLINE] UDP channel active\n");
                    continue;
                }
                if (n < 8) continue;
                std::lock_guard<std::mutex> l(m);
                zustellen(get32(b), get16(b + 4), get16(b + 6), b + 8, size_t(n) - 8);
            }
        }

        void lauf()
        {
            if (!verbinden()) return;
            weck = int(::socket(AF_INET, SOCK_DGRAM, 0));
            if (weck >= 0)
            {
                weckAdr.sin_family = AF_INET;
                weckAdr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
                socklen_t wl = sizeof weckAdr;
                if (::bind(weck, reinterpret_cast<sockaddr *>(&weckAdr), sizeof weckAdr) == 0 && getsockname(weck, reinterpret_cast<sockaddr *>(&weckAdr), &wl) == 0)
                    nichtBlockierend(weck);
                else { netzSchliessen(weck); weck = -1; }
            }
            {
                const Konfig &k = konfig();
                std::string h;
                put16(h, kProtokoll);
                h += k.name;
                h.push_back('\0');
                h += k.passwort;
                senden(M_HELLO, h);
            }
            auto letzteReg = std::chrono::steady_clock::now() - std::chrono::seconds(10);
            auto letzterKeep = std::chrono::steady_clock::now();
            g_opTrefferOffen = 0; g_opKillsOffen = 0;   // only what happens while connected counts on the server
            struct OnlineMarke { OnlineMarke() { g_ts2OnlineSitzung.store(true); } ~OnlineMarke() { g_ts2OnlineSitzung.store(false); } } onlineMarke;   // cheats stay off while connected
            for (;;)
            {
                const auto jetzt = std::chrono::steady_clock::now();
                if (udp >= 0 && vip && jetzt - letzteReg > std::chrono::milliseconds(udpOk ? 2000 : 500))
                {
                    letzteReg = jetzt;
                    std::string r;
                    put32(r, token); put32(r, vip); put16(r, 0); put32(r, 0); put16(r, 0);
                    (void)::sendto(udp, r.data(), 12, 0, reinterpret_cast<sockaddr *>(&srv), srvLen);
                }
                if (jetzt - letzterKeep > std::chrono::seconds(5))
                {
                    letzterKeep = jetzt; senden(M_KEEPALIVE, "");
                    // Online+: report hits/kills to the server (servers without Online+ ignore the message)
                    const uint32_t h = g_opTrefferOffen.exchange(0), k = g_opKillsOffen.exchange(0);
                    if (h || k) { std::string st; put32(st, h); put32(st, k); senden(M_OPSTAT, st); }
                }
                bool schreiben;
                { std::lock_guard<std::mutex> l(ausM); schreiben = !aus.empty(); }
                using fd_t = decltype(pollfd::fd);
                pollfd p[3] = {{fd_t(tcp), short(POLLIN | (schreiben ? POLLOUT : 0)), 0}, {fd_t(udp), POLLIN, 0}, {fd_t(weck), POLLIN, 0}};
                if (netzPoll(p, weck >= 0 ? 3 : 2, weck >= 0 ? 250 : 5) < 0) continue;
                { std::lock_guard<std::mutex> l(m); if (trennen) break; }
                if (weck >= 0 && (p[2].revents & POLLIN)) { char d[64]; while (::recv(weck, d, sizeof d, 0) > 0) {} }
                if (p[1].revents & POLLIN) udpLesen();
                if (p[0].revents & (POLLERR | POLLHUP)) { scheitern("Lost connection to the server"); break; }
                if (p[0].revents & POLLIN)
                {
                    char b[16384];
                    const int n = int(::recv(tcp, b, sizeof b, 0));
                    if (n <= 0) { scheitern(zustand == 2 ? "Lost connection to the server" : "Server closed the connection"); break; }
                    ein.append(b, size_t(n));
                    size_t pos = 0;
                    while (ein.size() - pos >= 5)
                    {
                        const uint32_t len = get32(reinterpret_cast<const uint8_t *>(ein.data() + pos));
                        if (len == 0 || len > (1u << 20)) { scheitern("invalid data from the server"); pos = ein.size(); break; }
                        if (ein.size() - pos < 4 + len) break;
                        const uint8_t *msg = reinterpret_cast<const uint8_t *>(ein.data() + pos + 4);
                        nachricht(msg[0], msg + 1, len - 1);
                        pos += 4 + len;
                    }
                    ein.erase(0, pos);
                    std::lock_guard<std::mutex> l(m);
                    if (zustand == 3) break;
                }
                {
                    std::lock_guard<std::mutex> l(ausM);
                    while (!aus.empty())
                    {
                        const int n = int(::send(tcp, aus.data(), int(aus.size()), kSendeFlags));
                        if (n <= 0) break;
                        aus.erase(0, size_t(n));
                    }
                }
            }
            netzSchliessen(tcp); tcp = -1;
            if (udp >= 0) { netzSchliessen(udp); udp = -1; }
            if (weck >= 0) { netzSchliessen(weck); weck = -1; }
            udpOk = false;
            std::lock_guard<std::mutex> l(ausM);
            aus.clear();
            ein.clear();
        }
    };

    Tunnel &tunnel()
    {
        static Tunnel *t = new Tunnel(); // never destroyed on purpose (the thread runs until the process ends)
        return *t;
    }
#endif

    // ---- Helpers for guest memory ----
    inline std::string gStr(uint8_t *rdram, uint32_t a, size_t max = 255)
    {
        std::string s;
        if (!a) return s;
        for (size_t i = 0; i < max; ++i) { const char c = char(rdram[(a + i) & kRamMask]); if (!c) break; s.push_back(c); }
        return s;
    }
    inline void gStrSchreiben(uint8_t *rdram, uint32_t a, const std::string &s)
    {
        if (!a) return;
        for (size_t i = 0; i < s.size(); ++i) rdram[(a + i) & kRamMask] = uint8_t(s[i]);
        rdram[(a + s.size()) & kRamMask] = 0;
    }
    inline void g8(uint8_t *rdram, uint32_t a, uint8_t v) { if (a) rdram[a & kRamMask] = v; }
    inline void g16(uint8_t *rdram, uint32_t a, uint16_t v) { if (a) std::memcpy(rdram + (a & kRamMask), &v, 2); }
    inline void g32(uint8_t *rdram, uint32_t a, uint32_t v) { if (a) std::memcpy(rdram + (a & kRamMask), &v, 4); }
    inline uint32_t r32(uint8_t *rdram, uint32_t a) { uint32_t v = 0; std::memcpy(&v, rdram + (a & kRamMask), 4); return v; }

    inline bool istNetzModul(const std::string &pfad)
    {
        static const char *namen[] = {"\\INET.IRX", "\\NETCNF.IRX", "\\EZNETCNF.IRX", "\\INETCTL.IRX", "\\SMAP.IRX",
                                      "\\MSIFRPC.IRX", "\\LIBNETB.IRX", "\\EZNETCTL.IRX"};
        std::string p = pfad;
        for (auto &c : p) c = char(std::toupper(static_cast<unsigned char>(c)));
        for (const char *n : namen)
            if (p.find(n) != std::string::npos) return true;
        return false;
    }

    // sceSifLoadModule(path, argsize, args) = 0x44B000, sceSifLoadStartModule(path, argsize, args, int *result) = 0x44B020
    void ladeModul(uint8_t *rdram, R5900Context *ctx, PS2Runtime *rt, bool mitErgebnis)
    {
        const std::string pfad = gStr(rdram, arg(ctx, 4));
        const uint32_t ergebnis = mitErgebnis ? arg(ctx, 7) : 0u;
        std::string grossPfad = pfad;
        for (auto &c : grossPfad) c = char(std::toupper(static_cast<unsigned char>(c)));
        const bool dev9 = grossPfad.find("\\DEV9.IRX") != std::string::npos;
        if (adapterAn() && (dev9 || istNetzModul(pfad)))
        {
            static int naechsteId = 0x60;
            if (ergebnis) g32(rdram, ergebnis, 0); // module "stays resident"
            std::fprintf(stderr, "[ONLINE] Modul %s -> nachgebildet\n", pfad.c_str());
            ret(ctx, naechsteId++);
            leave(ctx);
            return;
        }
        SET_GPR_U32(ctx, 8, 0); // t0 = 0 as in the original
        if (!mitErgebnis) SET_GPR_U32(ctx, 7, 0);
        sub_0044ADD8_0x44add8(rdram, ctx, rt); // sets pc = ra, v0 = module ID
        // Start result: the runtime does not write it. DEV9 without online = no adapter (as before in practice).
        if (ergebnis) g32(rdram, ergebnis, dev9 ? 1u : 0u);
    }
    void ts2_LoadModule(uint8_t *rdram, R5900Context *ctx, PS2Runtime *rt) { ladeModul(rdram, ctx, rt, false); }
    void ts2_LoadStartModule(uint8_t *rdram, R5900Context *ctx, PS2Runtime *rt) { ladeModul(rdram, ctx, rt, true); }

    // ---- ezNetCnf / ezNetCtl (libezcnf.c / libezctl.c) ----
    void ts2_ezNetCnf_A(uint8_t *, R5900Context *ctx, PS2Runtime *) // 0x482150 Init, 0x482218 Exit, 0x482298 GetCount
    {
        const uint32_t pc = ctx->pc;
        const bool an = adapterAn();
        if (pc == 0x482298u) ret(ctx, an ? 1 : 0);
        else if (pc == 0x482218u) ret(ctx, 0);
        else ret(ctx, an ? 1 : 0);
        if (konfig().log) std::fprintf(stderr, "[ONLINE] ezNetCnf %06X\n", pc);
        leave(ctx);
    }
    void ts2_ezNetCnfGetCombinationList(uint8_t *rdram, R5900Context *ctx, PS2Runtime *) // (path, list)
    {
        const uint32_t list = arg(ctx, 5);
        if (!adapterAn() || !list) { ret(ctx, 0); leave(ctx); return; }
        for (uint32_t i = 0; i < 0x30 + 0x68; i += 4) g32(rdram, list + i, 0);
        g32(rdram, list + 0, 1); // count
        g32(rdram, list + 4, 1); // default (1-based)
        const uint32_t e = list + 0x30;
        g32(rdram, e + 0, 1);
        gStrSchreiben(rdram, e + 0x04, "TS2 Online");
        gStrSchreiben(rdram, e + 0x28, "TS2 Online");
        gStrSchreiben(rdram, e + 0x48, "Server");
        std::fprintf(stderr, "[ONLINE] Netzwerk-Konfiguration \"TS2 Online\" bereitgestellt\n");
        if (!std::getenv("TS2_ONLINE_SERVER")) ::ps2xGsBridgeOnlineBrowser(); // show the server browser (the game waits in the menu)
        ret(ctx, 1);
        leave(ctx);
    }
    void ts2_ret0(uint8_t *, R5900Context *ctx, PS2Runtime *) { ret(ctx, 0); leave(ctx); }
    void ts2_ezNetCtl_Init(uint8_t *, R5900Context *ctx, PS2Runtime *) // 0x4826B8 Init, 0x482780 Exit
    {
        ret(ctx, ctx->pc == 0x482780u ? 0 : (adapterAn() ? 1 : 0));
        leave(ctx);
    }
    void ts2_ezNetCtl_Status(uint8_t *rdram, R5900Context *ctx, PS2Runtime *) // 0x4829E8 GetStatus(st), 0x482A88
    {
        if (ctx->pc == 0x4829E8u)
        {
            const uint32_t st = arg(ctx, 4);
            g32(rdram, st + 0, 0);
            g32(rdram, st + 4, 0);
            g32(rdram, st + 8, adapterAn() ? 1u : 0u); // 1 = connected
        }
        ret(ctx, 0);
        leave(ctx);
    }

    // ---- Backend of the socket layer ----
    void backend(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
    {
        const uint32_t pc = ctx->pc;
        const uint32_t a0 = arg(ctx, 4), a1 = arg(ctx, 5), a2 = arg(ctx, 6), a3 = arg(ctx, 7);
        const uint32_t t0 = arg(ctx, 8), t1 = arg(ctx, 9);
        int r = 1;
#if TS2_NETZ_SOCKETS
        Tunnel &T = tunnel();
        const bool log = konfig().log;
        auto handle = [&](uint32_t h) -> Sock * { auto it = T.socks.find(int(h)); return it == T.socks.end() ? nullptr : &it->second; };
        switch (pc)
        {
        case 0x479FC0u: // Init
        {
            {
                const Konfig neu = lesen();
                bool anders, online;
                {
                    std::lock_guard<std::mutex> l(T.m);
                    anders = neu.host != konfig().host || neu.port != konfig().port || neu.name != konfig().name || neu.passwort != konfig().passwort;
                    online = T.zustand == 1 || T.zustand == 2;
                }
                if (anders && online) T.beenden(); // another server chosen: reconnect
                if (anders || !online) konfig() = neu;
            }
            if (!konfig().aktiv)
            {
                std::fprintf(stderr, "[ONLINE] no server set (game menu ESC/F1 -> Online)\n");
                ::ps2xGsBridgeOnlineStatus("No server selected - connect to one in the Online tab");
                r = 1;
                break;
            }
            std::fprintf(stderr, "[ONLINE] Server %s Port %u, Name \"%s\"\n", konfig().host.c_str(), konfig().port, konfig().name.c_str());
            if (std::getenv("TS2_NETHIST") && !g_ps2xStateHist.exchange(true)) // Diagnosis: count calls of the game's network layer
                std::thread([] {
                    std::map<uint32_t, uint64_t> vorher;
                    for (;;)
                    {
                        std::this_thread::sleep_for(std::chrono::seconds(5));
                        g_ps2xStateHist.store(true);
                        std::string z;
                        std::lock_guard<std::mutex> cl(g_ps2xCallHistMutex);
                        for (const auto &[pc, n] : g_ps2xCallHist)
                            if (pc >= 0x458000u && pc < 0x47C000u && n != vorher[pc])
                            {
                                char b[40]; std::snprintf(b, sizeof b, " %06X:%llu", pc, (unsigned long long)(n - vorher[pc]));
                                z += b; vorher[pc] = n;
                            }
                        std::fprintf(stderr, "[NETHIST]%s\n", z.c_str());
                    }
                }).detach();
            r = T.online(6000) ? 0 : 1;
            std::fprintf(stderr, "[ONLINE] Netzwerk-Start -> %s\n", r == 0 ? "ok" : T.fehler.c_str());
            break;
        }
        case 0x47A088u: // Term
        {
            std::lock_guard<std::mutex> l(T.m);
            for (auto &kv : T.socks)
                if (kv.second.sid && kv.second.zustand == 2) { std::string p; put32(p, kv.second.sid); T.senden(M_SCLOSE, p); }
            T.socks.clear();
            r = 0;
            break;
        }
        case 0x47A108u: // Open(int *out, const char *host, u16 port, cfg*)
        {
            const int typ = int(r32(rdram, a3));
            const std::string host = gStr(rdram, a1);
            const uint16_t port = uint16_t(a2);
            std::unique_lock<std::mutex> l(T.m);
            if (T.zustand != 2) { r = 1; break; }
            Sock s;
            s.typ = typ;
            s.lport = port;
            if (typ == 0)
            {
                uint32_t ziel = 0;
                if (!ipLesen(host, ziel)) { r = 1; break; }
                s.lport = T.naechsterPort++;
                if (T.naechsterPort < 50000) T.naechsterPort = 50000;
                s.cid = T.naechsteCid++;
                s.zustand = 1;
                s.peerVip = ziel;
                s.peerPort = port;
                const int h = T.naechsterHandle++;
                T.socks[h] = s;
                std::string p;
                put32(p, s.cid); put16(p, s.lport); put32(p, ziel); put16(p, port);
                l.unlock();
                T.senden(M_SCONNECT, p);
                l.lock();
                T.cv.wait_for(l, std::chrono::milliseconds(5000), [&] { auto it = T.socks.find(h); return it == T.socks.end() || it->second.zustand != 1; });
                auto it = T.socks.find(h);
                if (it != T.socks.end() && it->second.zustand == 2) { g32(rdram, a0, uint32_t(h)); r = 0; }
                else { if (it != T.socks.end()) T.socks.erase(it); r = 1; }
                if (log) std::fprintf(stderr, "[ONLINE] TCP verbinden %s:%u -> %s\n", host.c_str(), port, r ? "fehlgeschlagen" : "ok");
                break;
            }
            if (typ == 1 && port == 0) { s.lport = T.naechsterPort++; }
            if (typ < 1 || typ > 3) { r = 3; break; }
            const int h = T.naechsterHandle++;
            T.socks[h] = s;
            g32(rdram, a0, uint32_t(h));
            r = 0;
            if (log) std::fprintf(stderr, "[ONLINE] Open Typ %d Port %u -> Handle %d\n", typ, s.lport, h);
            break;
        }
        case 0x47A3C0u: // Close(h)
        {
            std::lock_guard<std::mutex> l(T.m);
            auto it = T.socks.find(int(a0));
            if (it == T.socks.end()) { r = 2; break; }
            if (it->second.sid && it->second.zustand == 2) { std::string p; put32(p, it->second.sid); T.senden(M_SCLOSE, p); }
            T.socks.erase(it);
            r = 0;
            break;
        }
        case 0x47A410u: // Send(h, buf, u16 len, u16 *sent)
        {
            std::unique_lock<std::mutex> l(T.m);
            Sock *s = handle(a0);
            if (!s || !a1 || !a3) { r = 2; break; }
            g16(rdram, a3, 0);
            if (s->zustand == 3) { r = 0xD; break; }
            if (s->zustand != 2) { r = 0; break; } // not connected yet: nothing sent
            const uint32_t sid = s->sid;
            l.unlock();
            std::string p;
            put32(p, sid);
            for (uint32_t i = 0; i < (a2 & 0xFFFF); ++i) p.push_back(char(rdram[(a1 + i) & kRamMask]));
            T.senden(M_SDATA, p);
            g16(rdram, a3, uint16_t(a2));
            r = 0;
            break;
        }
        case 0x47A4A8u: // SendTo(h, const char *host, u16 port, buf, u16 len, u16 *sent)
        {
            uint16_t lport = 0;
            {
                std::lock_guard<std::mutex> l(T.m);
                Sock *s = handle(a0);
                if (!s || !a1 || !a3 || !t1) { r = 2; break; }
                lport = s->lport;
            }
            g16(rdram, t1, 0);
            uint32_t ziel = 0;
            const std::string host = gStr(rdram, a1);
            if (!ipLesen(host, ziel)) { r = 1; break; }
            const uint16_t n = uint16_t(t0);
            std::vector<uint8_t> d(n);
            for (uint32_t i = 0; i < n; ++i) d[i] = rdram[(a3 + i) & kRamMask];
            T.datagramm(lport, ziel, uint16_t(a2), d.data(), d.size());
            g16(rdram, t1, n);
            r = 0;
            static int z = 0;
            if (log && z++ < 200) std::fprintf(stderr, "[ONLINE] SendTo %u -> %s:%u (%u Bytes)\n", lport, host.c_str(), uint16_t(a2), n);
            break;
        }
        case 0x47A598u: // PingSend(h, u8 *ok, const char *host, u16 id)
        {
            g8(rdram, a1, 0);
            uint32_t ziel = 0;
            if (!ipLesen(gStr(rdram, a2), ziel)) { r = 1; break; }
            {
                std::lock_guard<std::mutex> l(T.m);
                if (!handle(a0)) { r = 2; break; }
                if (ziel == T.vip) { handle(a0)->prx.push_back({ziel, uint16_t(a3)}); g8(rdram, a1, 1); r = 0; break; }
            }
            std::string p;
            put32(p, ziel); put16(p, uint16_t(a3)); put32(p, 0); p.push_back(0);
            T.senden(M_PING, p);
            g8(rdram, a1, 1);
            r = 0;
            break;
        }
        case 0x47A658u: // Recv(h, buf, u16 len, u16 *got)
        {
            std::lock_guard<std::mutex> l(T.m);
            Sock *s = handle(a0);
            if (!s || !a1 || !a3) { r = 2; break; }
            g16(rdram, a3, 0);
            const uint32_t n = std::min<uint32_t>(a2 & 0xFFFF, uint32_t(s->srx.size()));
            if (n == 0) { r = s->zustand == 3 ? 0xD : 0; break; }
            for (uint32_t i = 0; i < n; ++i) rdram[(a1 + i) & kRamMask] = uint8_t(s->srx[i]);
            s->srx.erase(0, n);
            g16(rdram, a3, uint16_t(n));
            r = 0;
            break;
        }
        case 0x47A708u: // RecvFrom(h, char *host, u16 *port, buf, u16 len, u16 *got)
        {
            std::lock_guard<std::mutex> l(T.m);
            Sock *s = handle(a0);
            if (!s || !a1 || !a2 || !a3 || !t1) { r = 2; break; }
            g16(rdram, t1, 0);
            r = 0;
            if (s->rx.empty()) break;
            Datagramm d = std::move(s->rx.front());
            s->rx.pop_front();
            const uint32_t n = std::min<uint32_t>(t0 & 0xFFFF, uint32_t(d.daten.size()));
            for (uint32_t i = 0; i < n; ++i) rdram[(a3 + i) & kRamMask] = uint8_t(d.daten[i]);
            g16(rdram, a2, d.port);
            gStrSchreiben(rdram, a1, ipText(d.vip));
            g16(rdram, t1, uint16_t(n));
            static int z = 0;
            if (log && z++ < 200) std::fprintf(stderr, "[ONLINE] RecvFrom %s:%u (%u Bytes)\n", ipText(d.vip).c_str(), d.port, n);
            break;
        }
        case 0x47A810u: // PingReceive(h, u8 *got, char *host, u16 *id)
        {
            std::lock_guard<std::mutex> l(T.m);
            Sock *s = handle(a0);
            if (!s || !a1 || !a2) { r = 2; break; }
            g8(rdram, a1, 0);
            r = 0;
            if (s->prx.empty()) break;
            const PingAntwort p = s->prx.front();
            s->prx.pop_front();
            g8(rdram, a1, 1);
            gStrSchreiben(rdram, a2, ipText(p.vip));
            g16(rdram, a3, p.id);
            break;
        }
        case 0x47A8E0u: // Available(h, u16 *out)
        {
            std::lock_guard<std::mutex> l(T.m);
            Sock *s = handle(a0);
            if (!s || !a1) { r = 2; break; }
            uint32_t n = s->typ == 1 ? (s->rx.empty() ? 0u : uint32_t(s->rx.front().daten.size())) : uint32_t(s->srx.size());
            g16(rdram, a1, uint16_t(std::min<uint32_t>(n, 0xFFFF)));
            r = (n == 0 && s->zustand == 3) ? 0xD : 0;
            break;
        }
        case 0x47A978u: // OwnIP(char *out)
            if (!a0) { r = 2; break; }
            gStrSchreiben(rdram, a0, ipText(T.vip));
            r = 0;
            break;
        case 0x47A9C0u: // Broadcast(char *out)
            if (!a0) { r = 2; break; }
            gStrSchreiben(rdram, a0, ipText(kBroadcast));
            r = 0;
            break;
        case 0x47AA08u: // Resolve(const char *name, char *out)
        {
            uint32_t ip = 0;
            if (!a0 || !a1) { r = 2; break; }
            if (!ipLesen(gStr(rdram, a0), ip)) { r = 1; break; }
            gStrSchreiben(rdram, a1, ipText(ip));
            r = 0;
            break;
        }
        case 0x479F90u: // Thunk -> 0x47AAB8
        case 0x47AAB8u: // Peer(h, char *out)
        {
            std::lock_guard<std::mutex> l(T.m);
            Sock *s = handle(a0);
            if (!s || !a1) { r = 2; break; }
            gStrSchreiben(rdram, a1, ipText(s->peerVip ? s->peerVip : T.vip));
            r = 0;
            break;
        }
        case 0x479FA8u: // Thunk -> 0x47AB68
        case 0x47AB68u: // Query
            r = 0;
            break;
        default:
            std::fprintf(stderr, "[ONLINE] unbekannter Backend-Einstieg 0x%06X\n", pc);
            r = 1;
            break;
        }
        if (log && pc != 0x47A708u && pc != 0x47A658u && pc != 0x47A810u && pc != 0x47A8E0u && pc != 0x47A4A8u && pc != 0x47AB68u && pc != 0x479FA8u)
            std::fprintf(stderr, "[ONLINE] Backend %06X (%08X %08X %08X %08X %08X %08X) -> %d\n", pc, a0, a1, a2, a3, t0, t1, r);
#else
        (void)rdram; (void)a0; (void)a1; (void)a2; (void)a3; (void)t0; (void)t1;
        r = 5;
#endif
        ret(ctx, r);
        leave(ctx);
    }
}
TS2_DEFINE(sub_0044B000_0x44b000, ts2netz::ts2_LoadModule)
TS2_DEFINE(sub_0044B020_0x44b020, ts2netz::ts2_LoadStartModule)
TS2_DEFINE(sub_00482150_0x482150, ts2netz::ts2_ezNetCnf_A)
TS2_DEFINE(sub_00482358_0x482358, ts2netz::ts2_ezNetCnfGetCombinationList)
TS2_DEFINE(sub_00482518_0x482518, ts2netz::ts2_ret0)
TS2_DEFINE(sub_004825E0_0x4825e0, ts2netz::ts2_ret0)
TS2_DEFINE(sub_004826B8_0x4826b8, ts2netz::ts2_ezNetCtl_Init)
TS2_DEFINE(sub_00482800_0x482800, ts2netz::ts2_ret0)
TS2_DEFINE(sub_004829E8_0x4829e8, ts2netz::ts2_ezNetCtl_Status)
TS2_DEFINE(sub_00482B00_0x482b00, ts2netz::ts2_ret0)
TS2_DEFINE(sub_00482B78_0x482b78, ts2netz::ts2_ret0)
TS2_DEFINE(sub_00479F90_0x479f90, ts2netz::backend)
