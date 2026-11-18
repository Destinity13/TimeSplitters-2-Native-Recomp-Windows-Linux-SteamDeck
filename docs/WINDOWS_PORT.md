<sub>[← back to README](../README.md)</sub>

# Windows port: status

Status: **released with 1.0.1.** The first story level was played through on a real Windows PC (RTX 3060 Ti,
Direct3D 11) at 59.9 FPS, about 30 minutes without a single error. The whole chain also runs under Wine 11 at 60 FPS.

## How the 1.0.0 start problem was found

The 1.0.0 Windows package exited right at start on real PCs. It took three separate fixes, found on a clean GitHub
Windows runner, in a Windows virtual machine without a graphics driver and with logs from a tester:

| Problem | Cause | Fix |
|---|---|---|
| exit `0xC0000409` at start/exit | background threads of the graphics DLL were still joinable `std::thread`s when the DLL unloaded, so the C++ runtime called `abort()` | threads detached, the process ends without running those destructors |
| setup stopped at "VU1 decoding produced nothing" without OpenGL (VMs, Remote Desktop, broken drivers) | raylib 5.5 continues after a failed window and calls OpenGL through null pointers | the runtime probes OpenGL first and runs without its (invisible) helper window when there is none |
| "VM not ready after 10 s", black window | the graphics hand-over file defaulted to `/tmp/ps2x_live.gs`, which on Windows means `C:\tmp` – that folder does not exist on a normal Windows | the file now lives in the system temp folder |

Also new for Windows: the launcher starts with **Direct3D 11** and falls back to Vulkan once if D3D11 cannot start
(the choice is kept in `build\renderer.txt`). PCSX2's data lives in `%APPDATA%\ts2-recomp\pcsx2`, so an installed
PCSX2 cannot interfere, and `build\bridge.log` records every start step and, if it ever crashes, the exact location.

## How it works

The Windows package follows the same zero-install idea as the Linux package. You need nothing installed.

| Part | Windows build |
|---|---|
| Launcher | `TS2-Recomp.bat` opens a PowerShell/WinForms launcher. It asks for the ISO, shows the setup progress, then gives you Play / Settings / desktop shortcut. |
| Setup | Bundled embedded Python 3.12 runs the same steps as on Linux: check the ISO (SHA-1 of `SLUS_203.14` or `SLES_508.77`), extract it, recompile it, build in two passes (VU1 microcode is decoded by the runtime in between). |
| Compiler | Bundled Zig 0.17.1 for Windows, trimmed to the MinGW parts (`x86_64-windows-gnu`). |
| Recompiler tools | `ps2_recomp.exe` / `ps2_analyzer.exe`, cross-built with Zig. They include the COP0 `Count` fix. |
| Runtime, IOP, raylib, FFmpeg (MPEG-1/2 only) | Prebuilt static libraries. Only the code generated from your own disc is compiled on your PC. |
| GS/SPU2 bridge | `libps2xgs.dll`: PCSX2's GS and SPU2 with the ps2x bridge, built by GitHub Actions (`.github/workflows/windows-bridge.yml`, MSVC/clang-cl, Vulkan + OpenGL). It includes the VC++ runtime DLLs. |
| Online | Winsock in the game's network layer and in `ts2-server.exe`. The server browser is in the bridge menu, on the Online tab. |

As always, the package contains no ISO, ELF, BIOS, generated game code or textures.

## Verification (Wine 11, RX 7900 XT, Vulkan)

| Test | Result |
|---|---|
| Game exe from the dev tree | 1600 ticks, 59.93 FPS, PCSX2-GS renderer 14 (Vulkan), profile screen in HD widescreen |
| Package: ISO check, extraction, `ps2_recomp.exe` under Wine | ok |
| Package: build with the package's prebuilt libraries (Linux-hosted Zig 0.17.1, same target), VU1 decode under Wine, pass 2 | ok (1 VU1 program, 1961 instruction pairs) |
| Package-built game under Wine | 2500 ticks, 59.94 FPS, menu input works, screenshots written |
| Online: Windows network layer under Wine ↔ Linux `ts2-server` | connects and exchanges game traffic |

The bundled `zig.exe` itself cannot run under Wine: Wine 11 does not fully implement `NtLockFile`, so Zig's
cache fails with `manifest_create Unexpected`. This only affects Wine, not Windows. On Linux, use the Linux package.

## Found on the first real Windows PC (rc1–rc3), fixed in rc4

- **Setup hung at the VU1 decode step.** The decode ran with the GS bridge loaded, and the bridge's surfaceless
  start stalled on an NVIDIA driver. The decode needs no renderer, so it now runs without the bridge (same output,
  about 3 s).
- **Black screen, every renderer logged `L_103.PAK nicht gefunden`.** `long` is 32-bit on Windows, so `fseek`/`ftell`
  could not address anything past 2 GB of the 4.3 GB disc image. All disc reads now use 64-bit seeks (`_fseeki64`)
  and Unicode paths (`_wfopen`). The launcher also re-asks for the disc image if it was moved.
- **Frame pacing ran at the 15.6 ms scheduler tick.** `timeBeginPeriod(1)` at start.
- **Truncated log after a crash.** stdout is unbuffered on Windows.

## Fixed along the way

- PCSX2 opens absolute paths as `\\?\` paths, and those do not accept `/` as a separator. Screenshot paths are now
  written with backslashes only.
- Hook pointers in the bridge's `extern "C"` exports had C linkage, which broke the MSVC link. They are now declared at
  file scope.
- Texture sidecar lookup uses `_fullpath` instead of `realpath`.
- The COP0 `Count` register now runs at the real 294.912 MHz on every platform. The game's network flush timer depends on it.

## Still open

- Second play test on real Windows hardware with rc4 (gamepad, mouse capture via Raw Input, longer sessions).
- Code signing: none. Windows SmartScreen will warn on first start ("More info" → "Run anyway").
