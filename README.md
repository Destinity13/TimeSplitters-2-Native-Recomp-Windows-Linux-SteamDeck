<div align="center">

<img src="docs/screenshots/banner_crystalcut.jpg" width="100%" alt="Crystal Cut: the restored native PC port for TimeSplitters 2">

# Crystal Cut: the restored native PC port for TimeSplitters 2

**The PS2 classic, recompiled into a real PC game. No emulator. Windows, Linux and Steam Deck.<br>
Now with the content that was cut before release, found again on your own disc. USA and PAL disc, one game.**

[![Download](https://img.shields.io/github/v/release/Destinity13/TimeSplitters-2-Native-Recomp-Windows-Linux-SteamDeck?style=for-the-badge&label=download&color=2ea44f)](https://github.com/Destinity13/TimeSplitters-2-Native-Recomp-Windows-Linux-SteamDeck/releases/latest)
[![Windows](https://img.shields.io/badge/Windows-10%20%2F%2011-0078d4?style=for-the-badge&logo=windows&logoColor=white)](#windows)
[![Linux](https://img.shields.io/badge/Linux-any%20distro-fcc624?style=for-the-badge&logo=linux&logoColor=black)](#linux)
[![Steam Deck](https://img.shields.io/badge/Steam%20Deck-plays-1a9fff?style=for-the-badge&logo=steamdeck&logoColor=white)](#steam-deck)
[![Discs](https://img.shields.io/badge/disc-USA%20%2B%20PAL-c0392b?style=for-the-badge)](#usa-and-pal-one-game)

[![Listed on PlayPorted](https://playported.com/badge.svg)](https://playported.com/ports/1340-timesplitters-2/)

[![No install](https://img.shields.io/badge/install-nothing-blue)](#play-it-in-3-minutes)
[![Renderer](https://img.shields.io/badge/renderer-PCSX2%20GS%20·%20Vulkan%20·%20D3D11-informational)](gsbridge/)
[![Verified](https://img.shields.io/badge/verified-against%20PCSX2-orange)](docs/VERIFICATION.md)
[![AI-assisted](https://img.shields.io/badge/AI--assisted-Claude-d97757)](#ai-disclosure)
[![License](https://img.shields.io/badge/license-GPL--3.0-green)](LICENSE)

[News](news/README.md) · [Play it](#play-it-in-3-minutes) · [Firsts](#firsts) · [USA and PAL](#usa-and-pal-one-game) · [Play it your way](#play-it-your-way) · [Found on the disc](#found-on-the-disc) · [Online](#online-multiplayer) · [Gallery](#gallery) · [FAQ](#faq) · [Status](#status) · [Docs](#documentation)

> [!NOTE]
> **Seeing a bug in widescreen?** In 4:3 the game runs as it was made. Real native widescreen means every part of the game that was built for a 4:3 TV has to be adjusted by hand, and some of those spots only show up over time. [Why widescreen is the hard part, what is fixed and what comes next](news/2026-11-24-why-widescreen-is-hard.md). Thank you for every report!

<img src="docs/screenshots/cc/koop1.jpg" width="100%" alt="MapMaker Story co-op in split screen on the PS2 version, Splitter Base">
<sub>MapMaker Story co-op in split screen. As far as we know, this has never run on the PS2 version before.</sub>

</div>

<!-- news:start -->
## 📣 News

<table>
<tr>
<td width="50%" valign="top"><a href="news/2026-11-25-devlog-1-3-3-almost-here.md"><img src="docs/screenshots/banner_crystalcut.jpg" width="100%" alt=""></a><br><sub>2026-11-25 · Devlog</sub><br><b><a href="news/2026-11-25-devlog-1-3-3-almost-here.md">1.3.3 is almost here</a></b><br><sub>Hip fire on the crosshair, auto aim with modern controls, mouse and gamepad together, the VU1 setup error fixed and setup for older CPUs. In testing now, release in the next hours.</sub></td>
<td width="50%" valign="top"><a href="news/2026-11-24-why-widescreen-is-hard.md"><img src="news/img/2026-11-24-mapmaker-after.jpg" width="100%" alt=""></a><br><sub>2026-11-24 · Devlog</sub><br><b><a href="news/2026-11-24-why-widescreen-is-hard.md">Why widescreen is the hard part, and where we are</a></b><br><sub>Where most of the current bugs come from, what is already fixed and what comes next.</sub></td>
</tr>
</table>

<sub>All posts, patch notes and what's coming next: <a href="news/README.md">news</a></sub>
<!-- news:end -->

> [!IMPORTANT]
> **The current version is Crystal Cut 1.3.2.** [Patch notes](news/2026-11-21-patch-notes-1-3-2.md) · From 1.3.1 on, the launcher updates itself: just press Update.
>
> Coming from 1.2 or older: extract the new version into a **new** folder and set it up once with your disc (USA or PAL). To keep your saves, copy the
> `savedata` folder from your old install into the new one. On Windows, extract the zip with 7-Zip: the built-in
> Windows extractor is very slow with the 8000 small compiler files in the package.

> [!NOTE]
> **This is still a young project.** Bugs and odd cases can happen, especially in the newest features. If something
> looks wrong, please tell us: [GitHub issues](https://github.com/Destinity13/TimeSplitters-2-Native-Recomp-Windows-Linux-SteamDeck/issues),
> Reddit or YouTube comments. A screenshot or short clip plus `app/build/play.log` helps a lot. Thank you to everyone
> who already did.

## What Crystal Cut is

In TimeSplitters 2 everything revolves around the time crystals. This edition is about the cut: content the
developers cut before release that is still sitting on the original disc. We went through the whole disc, file by file
and table by table, and brought back what could be brought back. **Everything comes from your own disc.** Nothing from
the game is in the download, nothing was taken from other games or leaked builds.

Every restored piece is on by default and has its own switch. You can still play the game exactly like on the PS2.

<table>
<tr>
<td align="center" width="16%"><b><big>9</big></b><br><sub>new Arcade<br>maps</sub></td>
<td align="center" width="16%"><b><big>25 + 1</big></b><br><sub>extra characters<br>plus the Consultant</sub></td>
<td align="center" width="16%"><b><big>4</big></b><br><sub>cut characters back<br>in the story</sub></td>
<td align="center" width="16%"><b><big>2</big></b><br><sub>discs, USA and PAL,<br>one game</sub></td>
<td align="center" width="16%"><b><big>128 MB</big></b><br><sub>game memory<br>instead of 32 MB</sub></td>
<td align="center" width="16%"><b><big>60 FPS</big></b><br><sub>16:9, up to 8×,<br>online play</sub></td>
</tr>
<tr>
<td align="center" colspan="6"><sub>8849 files in 88 archives checked one by one · every build tested on both discs · checked against PCSX2</sub></td>
</tr>
</table>

## Firsts

As far as we know, none of this has been done before on TimeSplitters 2. We checked The Cutting Room Floor, the
TimeSplitters wiki, Nexus and ModDB before calling anything new. If someone did it first, tell us and we will happily
give credit.

<table>
<tr>
<td width="50%"><img src="docs/screenshots/cc/koop2.jpg" width="100%" alt="MapMaker Story co-op, both players share the objectives"></td>
<td width="50%"><img src="docs/screenshots/cc/arcade_liste.jpg" width="100%" alt="Notre Dame, Chicago and Atom Smasher in the Arcade map list"></td>
</tr>
<tr>
<td><sub><b>MapMaker Story co-op on the PS2 version.</b> The GameCube version can do it with mods. On PS2 the lock was never found. Now a second player joins in split screen with shared objectives, on all three story sample maps, USA and PAL.</sub></td>
<td><sub><b>Chicago, Atom Smasher and NeoTokyo as Arcade maps.</b> Their arcade setups were on the disc, the menu never showed them. Chicago gets 19 spawn points from its own story setup.</sub></td>
</tr>
<tr>
<td><img src="docs/screenshots/cc/planetx.jpg" width="100%" alt="Deathmatch on Planet X"></td>
<td><img src="docs/screenshots/cc/oneonone_menu.jpg" width="100%" alt="One on One in the mode menu"></td>
</tr>
<tr>
<td><sub><b>Planet X, Spacestation and Robot Factory as Arcade maps.</b> Spacestation and Robot Factory crash on a real PS2 because the memory is too small. The port gives the game 128 MB.</sub></td>
<td><sub><b>One on One is playable.</b> A complete mode entry with name, description and icon that the menu always skipped.</sub></td>
</tr>
</table>

- **Cut characters as story enemies.** Lean Molly and Ample Sally are loaded in Wild West but never placed. Marco the Snitch and the Reaper Splitter are loaded in Notre Dame but never placed. Now they fight you.
- **25 characters added on top of the roster.** Mods replace existing characters, we extend the list so nobody is lost.
- **Louie Bignose speaks with his own voice.** Nine recordings made for him sit on the disc unused.
- **128 MB game memory** in a PS2 recompilation of this game, at the same frame rate.
- **USA and PAL are one game** (next section).

## USA and PAL: one game

<img src="docs/screenshots/cc/koop_pal.jpg" width="100%" alt="The same MapMaker co-op map running from the PAL disc">

- **One download for both discs.** The setup checks which disc you have and builds the right game.
- **The European disc runs at 60 Hz** like the USA one: 723 timing values were set to the USA timing, so speed, timers and animations match.
- **Both play together online.** A USA player and a PAL player meet in the same match.
- **Shared saves:** a profile from one disc loads with the other.
- Both start in English. PAL owners can pick the game language in the launcher.
- Every feature and every restored piece works on both, and every build is tested on both.

## Play it your way

- **The original:** the PS2 game as it was, at 60 FPS and in 16:9, with nothing added. One preset.
- **Crystal Cut:** everything restored, on by default.
- **Your own mix:** a switch for every restored piece, every picture and control option, saved as **profiles** you can load and share.
- **Presets** such as Steam Deck 60 fps.
- The same options in the launcher and in the game menu (**F1**, Guide or Select+Start).

## Found on the disc

Not everything can be played, but all of it is real and still on the disc.

- A voice actor's demo recording, shipped by accident and never used
- Leftovers from TimeSplitters 1 in the sound bank (Bank, Mall, Cyberden) and four complete voice sets of TS1 characters
- 49 muted sounds, an unused ambient track and two silent placeholder tracks
- 121 files the game asks for that never made it onto the disc, among them 12 skies
- A cut Arcade Story mode with its own setup for Planet X. The level data it needs is not on the disc, very likely the cut map "Martian"
- The British text set on the USA disc, and a placeholder level

All of it in the [patch notes](news/2026-11-20-patch-notes-1-3-0.md#what-we-found-on-the-disc).

## Play it in 3 minutes

Download the package for your system from [**Releases**](https://github.com/Destinity13/TimeSplitters-2-Native-Recomp-Windows-Linux-SteamDeck/releases/latest),
extract it, start the launcher and pick **your** disc image, USA or PAL. The launcher checks the disc, extracts it and
builds the game once with a progress bar (about 3 minutes on a desktop PC, longer on a laptop). Then press **Start Game**.

Nothing to install: each package brings its own compiler and prebuilt libraries, and only the code generated from
your disc is compiled on your PC.

<table>
<tr><th width="33%" id="windows">🪟 Windows 10 / 11</th><th width="33%" id="linux">🐧 Linux</th><th width="33%" id="steam-deck">🎮 Steam Deck</th></tr>
<tr><td valign="top">

1. Download `…-windows-x86_64.zip`, extract it with **7-Zip** into a **new** folder
2. Run **`TS2-Launcher.exe`**
3. Pick your ISO, wait, **Play**

<sub>Nothing to install; uses Direct3D 11 (Vulkan as fallback). Windows may warn about an unsigned app:
"More info" → "Run anyway". [Details](docs/WINDOWS_PORT.md)</sub>

</td><td valign="top">

1. Download `…-linux-x86_64.tar.xz`, extract it
2. Run **`./TS2-Launcher`**
3. Pick your ISO, wait, **Play**

<sub>Needs only `python3`; runs on any current distribution.</sub>

</td><td valign="top">

1. Set it up in Desktop Mode with the Linux package
2. Add **`TS2-SteamDeck.sh`** to Steam
3. Play in **Game Mode**

<sub>Deck detected automatically, preset for a steady 60 FPS. [Step by step](docs/STEAM_DECK.md)</sub>

</td></tr>
</table>

**In the game:** F1 / Guide / Select+Start = settings menu (an extra key can be picked) · F7 = free camera · F9 = FPS · F10 = turbo · F11 or Alt+Enter = fullscreen · F12 = screenshot

**Mouse and keyboard:** click into the game to capture the mouse. Mouse look with the game's own aim-mode crosshair,
left = fire, right = zoom, middle/Q = secondary fire, E/Space = use, R = reload, C = crouch, wheel = weapon, WASD =
move. Every key and button can be rebound in the menu. [Details](docs/MOUSE.md)

## Online multiplayer

> [!NOTE]
> New in 1.0. Tested on a LAN so far; the first matches over the internet are what the community list is for – add
> your server with a pull request to [server/servers.txt](server/servers.txt).

The PS2 game had LAN play through the Network Adapter; online play was cut before release. **This port brings it
online.** Everyone on the same **`ts2-server`** meets in *Arcade → Network* as if on one LAN, with all modes, all maps
and up to 16 players.

<img src="docs/screenshots/online_match.jpg" width="100%" alt="First online match: the joining game finds the server, both players in the lobby, Deathmatch on Mexican Mission from both sides">

- **In-game server browser** in the style of the classic PC shooters: server list, players, ping, password, favourites, direct address. It opens by itself at *Arcade → Network*.
- **Host your own server** with one command: `./ts2-server --name "My server"`. It is included in the package and is a single C++ file without dependencies (it also builds on Windows with MSVC). [How to host](server/README.md)
- **One server for every platform:** Linux and Steam Deck players meet on the same `ts2-server`; Windows players join the same servers.
- Nothing in the game is changed: you use the original menus, *Start Game* to host and *Join Game* to join.
- **Your IP stays private:** players only ever talk to the server, never to each other. The server is a dumb relay
  with rate limits and connection caps, and ships with a hardened systemd unit. [Security model](server/README.md#security-model)

Roadmap and technical details: [ONLINE](docs/ONLINE.md)

> [!NOTE]
> **Online+** is a separate, optional edition you pick in the launcher: its own lobby in the game's menus, bot match,
> ranks, hit markers, killfeed and scoreboard. It is a side experiment to see how far the game can be changed with
> its own menus, messages and HUD. It has no goal, it still has bugs, and the classic game and its multiplayer stay
> untouched and stay the default.

## What makes it different

|  |  |
|---|---|
| **Native, not emulated** | The game's MIPS code is recompiled into C++ and compiled for your CPU. No CPU emulation at runtime; only graphics and sound come from PCSX2's GS and SPU2. |
| **Online the PS2 never had** | The original LAN mode, carried over the internet by a small relay server, with a real server browser. |
| **One click** | Same game on Windows, Linux and Steam Deck. The launcher does everything; you only bring the disc. |
| **Better than the original, faithful where it counts** | 60 FPS, real 16:9 instead of stretching, 1×–8× resolution, optional fixes (frame blending off, no letterbox). Everything else behaves like the PS2. |
| **Checked against PCSX2** | Every fix is measured against the emulator (savestates, screenshots, register logs) before it lands. [VERIFICATION](docs/VERIFICATION.md) |
| **Open and legal** | GPL-3.0, no game data in the repository, every bug and fix documented. [FINDINGS](docs/FINDINGS.md) |

## Features

<table>
<tr><td valign="top" width="50%">

**Picture**
- Real 16:9 (Hor+), also 16:10, 21:9 and 32:9; menus and HUD use the wide picture
- Internal resolution 1×–8× or "Auto" (steps down on slower cards), anisotropic filtering, FXAA, AMD CAS
- Sniper scope: Original, Clear or Extra clear
- Original frame blending ("ghosting" when turning) switchable, **off by default**
- Cutscenes without letterbox bars (switchable)
- PCSX2 texture packs from `texture_packs/` (none included)

</td><td valign="top" width="50%">

**Play**
- Locked 60 FPS in game, menus and split-screen
- Split-screen for 2 players; a third gamepad turns on the multitap
- Online play with server browser, up to 16 players
- Modern gamepads (SDL3), rumble, pressure buttons
- Mouse aiming with a fixed crosshair, free key/mouse binding in the menu ([details](docs/MOUSE.md))
- Optional: modern gamepad aiming, unlock everything (your own progress stays untouched)
- Launcher with setup, settings, news and automatic updates
- In-game settings menu, memory card saves, built-in hang report

</td></tr>
</table>

## 1.1 vs 1.2

<table>
<tr>
<td width="50%"><img src="docs/screenshots/patch12_haze.jpg" width="100%" alt="Haze overlay on in 1.1.0 (left), off in 1.2 (right)"></td>
<td width="50%"><img src="docs/screenshots/patch12_uplink.jpg" width="100%" alt="Temporal Uplink map: off center in 1.1.0 (left), fixed in 1.2 (right)"></td>
</tr>
<tr>
<td><sub>Clear picture: the double image in the distance is gone. The original overlay can be turned back on in the menu.</sub></td>
<td><sub>Temporal Uplink map: no longer shifted and doubled, it looks like in PCSX2 now.</sub></td>
</tr>
</table>
<sub>Also in 1.2: scope weapons no longer zoom in by themselves, mouse speed follows the zoom, turrets with the mouse,
launcher on the Steam Deck, safer Windows setup. All of it in the <a href="news/2026-11-16-patch-notes-1-2.md">patch notes</a>.</sub>

### 1.0.1 vs 1.1

<table>
<tr>
<td width="50%"><img src="docs/screenshots/compare_11_snow.jpg" width="100%" alt="Snow in the Siberia intro: 1.0.1 left, 1.1 right"></td>
<td width="50%"><img src="docs/screenshots/compare_11_smoke.gif" width="100%" alt="Impact smoke on a wall at half speed: 1.0.1 left, 1.1 right, with a moving divider"></td>
</tr>
<tr>
<td><sub>Snow and particles sit where they belong again.</sub></td>
<td><sub>Impact smoke at half speed. The grey patch is gone, it is a soft puff like on the PS2.</sub></td>
</tr>
</table>
<sub>Left is release 1.0.1, right is 1.1, same input replay. The cause was a wrong sine in our vector unit code, found by
comparing with PCSX2. Details in the <a href="news/2026-11-14-devlog-road-to-1-1.md">devlog</a>.</sub>

<table>
<tr>
<td width="50%"><img src="news/img/launcher-home.jpg" width="100%" alt="The 1.1 launcher: start page with news, edition switch and Start Game"></td>
<td width="50%"><img src="news/img/launcher-options.jpg" width="100%" alt="The 1.1 launcher: controls page with modern gamepad aiming and Unlock everything"></td>
</tr>
</table>
<sub>The new launcher: news and updates on the start page, every setting in one place.</sub>

## Gallery

<table>
<tr>
<td width="50%"><img src="docs/screenshots/gallery12_camp.jpg" width="100%" alt="1.2, Siberia: sniper rifle at the camp, clear picture"></td>
<td width="50%"><img src="docs/screenshots/gallery12_uplink_tunnel.jpg" width="100%" alt="1.2, Siberia: Temporal Uplink in the tunnel"></td>
</tr>
<tr>
<td width="50%"><img src="docs/screenshots/cutscene_11_intro.gif" width="100%" alt="Story intro cutscene in 1.1, rendered live by the game in 16:9"></td>
<td width="50%"><img src="docs/screenshots/clip_aztec.gif" width="100%" alt="Aztec Ruins level intro"></td>
</tr>
<tr>
<td><img src="docs/screenshots/gallery_online_mexico.jpg" alt="Online Deathmatch on Mexican Mission"></td>
<td><img src="docs/screenshots/gallery_coop_tunnel.jpg" alt="Story co-op split-screen: both players in the tunnel"></td>
</tr>
<tr>
<td><img src="docs/screenshots/shot_chicago.jpg" alt="Chicago level intro"></td>
<td><img src="docs/screenshots/shot_crystal.jpg" alt="Aztec Ruins: the time crystal"></td>
</tr>
<tr>
<td><img src="docs/screenshots/gallery12_dam_gun.jpg" alt="1.2, Siberia: the twin gun on the dam against the helicopter"></td>
<td><img src="docs/screenshots/gallery_arcade_fight.jpg" alt="Arcade: close combat in the tomb"></td>
</tr>
<tr>
<td><img src="docs/screenshots/gallery_dam.jpg" alt="Level intro: the wall of Oblask Dam"></td>
<td><img src="docs/screenshots/gallery_character.jpg" alt="Character select at 16:9"></td>
</tr>
</table>
<table>
<tr>
<td width="50%"><img src="docs/screenshots/cc/chicago.jpg" alt="Deathmatch in Chicago"></td>
<td width="50%"><img src="docs/screenshots/cc/neotokyo.jpg" alt="Deathmatch in NeoTokyo"></td>
</tr>
<tr>
<td><img src="docs/screenshots/cc/gothic.jpg" alt="Gothic tile set in the MapMaker"></td>
<td><img src="docs/screenshots/cc/figuren.jpg" alt="Extra characters in the character select"></td>
</tr>
</table>

<sub>All pictures straight from the port on the author's own disc: no AI upscaling, no texture pack. The clips are
captured from the running game.</sub>

## Before / after

<img src="docs/screenshots/compare_resolution.gif" width="100%" alt="The same frame at the PS2's output resolution and at 4x internal resolution, with a moving divider">

**[▶ Open the interactive slider](https://destinity13.github.io/TimeSplitters-2-Native-Recomp-Windows-Linux-SteamDeck/compare/)** – drag between
the PS2 picture and the port, for resolution and for 4:3 vs 16:9.

<img src="docs/screenshots/compare_widescreen.jpg" width="100%" alt="Same frame of the same input replay: original 4:3 (left) vs real 16:9 (right)">

<sub>Same frame of the same input replay: original 4:3 (left), real 16:9 with more of the scene (right). All shots from
the port's renderer on the author's own disc, no AI upscaling, no texture pack.</sub>

## FAQ

| | |
|---|---|
| **Does it run on Windows?** | Yes, since 1.0.1: Windows 10 and 11, 64-bit, with Direct3D 11 (Vulkan as fallback). The package brings everything it needs, including the compiler that builds your copy. Windows may warn about an unsigned app the first time – "More info" → "Run anyway". |
| **Windows blocks the launcher or the setup stops at "Recompiling"** | That is **Smart App Control** (Windows 11). It blocks every program that is not signed, and it has no per-app exception. Your build is compiled on your own PC, so it can never carry a signature. To play, turn it off: Windows Security → App & browser control → Smart App Control → Off, then start the setup again. Recent Windows updates let you turn it back on later without reinstalling. |
| **Do I need PCSX2 installed?** | No. The package brings the small GS/SPU2 bridge it needs. Nothing else is installed on your system. |
| **Which disc works?** | The **USA** release (`SLUS-203.14`) and the **European PAL** release (`SLES-508.77`), as an `.iso` dump of your own disc. Both play the same game and can play together online. The Japanese disc is not supported. |
| **Is it an emulator with extra steps?** | No. The game's CPU code is translated to C++ once and compiled for your PC. At runtime there is no MIPS interpreter or JIT, only the GS/SPU2 (graphics and sound) from PCSX2. That is why it holds 60 FPS on a Steam Deck. |
| **Why does it compile on my PC?** | Because the translated game code is derived from your disc and must not be distributed. Your PC turns your disc into your build, once, with the compiler that ships in the package. |
| **Controller, mouse or both?** | Both, at the same time. Gamepads through SDL3 (any modern pad, rumble, pressure buttons), mouse look with the game's own crosshair, all keys and buttons rebindable in the menu. Mouse play is the newest part and [not finished](docs/MOUSE.md). |
| **Can I play with friends?** | With 1.0: online through a `ts2-server` (host your own in one command, up to 16 players), or split-screen on one PC today. |
| **Does it change how the game plays?** | No. The PS2 game logic runs unchanged; the visible differences (60 FPS, 16:9, no ghosting, no letterbox) are switchable, and the Easy/Normal/Hard quirks of the original are all still there. |

## Status

<img src="docs/status.svg" width="100%" alt="Project status per area">

| | |
|---|---|
| 🆕 **New in 1.3.0** | **Crystal Cut:** MapMaker Story co-op on PS2 · cut characters in the story · 9 new Arcade maps · One on One · 25 extra characters + Consultant · Gothic tile set · Bignose voice · 128 MB · USA and PAL as one game · Crystal Cut menu with presets and profiles · low stimulus options · FOV slider · MapMaker walls, widescreen menus, turret, rumble and lights fixed |
| ✅ **Already there** | title, profiles, Story (all 10 levels load and play, Siberia played through), Story co-op, Arcade League, Challenge, MapMaker, options, bonus trailer, memory card, sound |
| 🆕 **New in 1.2** | **clear picture at high resolutions** (haze overlay off by default) · scope weapons no longer zoom in by themselves · mouse speed follows the zoom · Temporal Uplink map fixed · turrets with the mouse · optional cheats · launcher on the Steam Deck · safer Windows setup |
| 🆕 **New in 1.1** | **launcher with automatic updates and news** · particle effects fixed (fog, snow, smoke, debris) · any aspect ratio · 16:9 menus and HUD · mouse aiming rebuilt · modern gamepad aiming · unlock everything · texture packs · fast graphics card on laptops · English by default · Online+ as an optional experiment |
| 🆕 **New in 1.0.1** | **Windows release** · HUD at the 16:9 edges · sniper scope correct in 16:9 · co-op split screen fixed in 16:9 · no PCSX2 messages at start |
| 🆕 **New in 1.0** | online multiplayer with in-game server browser · `ts2-server` in the package · mouse and keyboard play · TS2-style settings menu (EN/DE) · F8 video recording · F7 free camera |
| 🟡 **Being tested** | mouse play beyond Siberia/Arcade ([open points](docs/MOUSE.md)), Story levels 2–10 by hand (automated runs are clean), Arcade Custom, 3–4 players via multitap, online over the internet |
| 🛠 **Being worked on** | HD texture packs · controllable turrets · performance on slower PCs · video recording · more from the disc (GameCube and Xbox data checks) |

Every build: [CHANGELOG](docs/CHANGELOG.md) · open issues: [KNOWN_ISSUES](docs/KNOWN_ISSUES.md) · found a bug?
[Open an issue](https://github.com/Destinity13/TimeSplitters-2-Native-Recomp-Windows-Linux-SteamDeck/issues) and attach `app/build/play.log` and `app/build/bridge.log`.

**Which TimeSplitters 2 port is this?** As far as we know the only active recompilation of the **PS2** version,
checked frame by frame against PCSX2. [split2-recomp](https://github.com/phobos665/split2-recomp) recompiles the
**Xbox** version instead; both are worth a look.

## How it was built

1. **PCSX2 is the reference, always.** Every scene is first run in PCSX2 with a scripted virtual gamepad; savestates
   at fixed frames give the "truth" for RAM, GS registers and pictures.
2. **The port replays the same inputs headless** and dumps the same things. A difference is a bug; the diff says where.
3. **Fixes go into the recompiler or the runtime, not into the game.** The game's own code stays as it came off the
   disc; every finding is written down ([FINDINGS](docs/FINDINGS.md)), from FPU rounding to a frozen COP0 counter
   that kept the network code silent.
4. **Every build is numbered and tested** (`pc001` … `pc228`): a test suite replays story, sniper, deathmatch and
   online scenes before every package; package setup as a fresh user on Linux; CI builds on GitHub; a Windows laptop
   that picks up every new package by itself, sets it up from the disc, plays a scripted run and sends logs and
   screenshots back; and play-tests on a real Windows PC by Lowkas.
5. **Tools were built along the way:** input record/replay, RAM-diff search (that is how the camera and player
   position for mouse look were found), write watches with native backtraces, a hang reporter, frame capture.

A human maintainer sets direction and play-tests on real hardware; an AI coding agent (Claude) does most of the
reverse engineering, implementation and measurement. See [AI disclosure](#ai-disclosure) and
[VERIFICATION](docs/VERIFICATION.md).

## How it works

```mermaid
flowchart LR
    ISO["your disc image"] --> E["SLUS_203.14"]
    E --> R["PS2Recomp fork<br/>R5900 → C++"]
    E --> V["VU1 microcode → C++/SSE"]
    R --> B["native executable"]
    V --> B
    RT["runtime: kernel · IOP · DMA/VIF/GIF · timing · network"] --> B
    B -->|GIF · SPU2| GS["PCSX2 GS (Vulkan / D3D11) + SPU2"]
    B -->|virtual LAN| S["ts2-server"]
```

No CPU emulation at runtime: the game's MIPS code becomes C++ and is compiled for your PC. Only graphics (GS) and
sound (SPU2) come from PCSX2, through a small bridge library. The PS2 network adapter is emulated and connected to a
`ts2-server`, so the game's own LAN code plays online.

## Documentation

| | |
|---|---|
| [ONLINE](docs/ONLINE.md) | online multiplayer: how it works, roadmap · [host a server](server/README.md) |
| [MOUSE](docs/MOUSE.md) | mouse and keyboard play: how it is done, what is still open |
| [STEAM_DECK](docs/STEAM_DECK.md) | setup and Game Mode on the Deck |
| [WINDOWS_PORT](docs/WINDOWS_PORT.md) | how the Windows build works and how it was tested |
| [Launcher](launcher/README.md) | the launcher: source, how updates work |
| [BUILDING](docs/BUILDING.md) | how the game is built on your PC, all settings |
| [CHANGELOG](docs/CHANGELOG.md) | every build and release |
| [FINDINGS](docs/FINDINGS.md) | every bug found against the reference and how it was fixed |
| [PERFORMANCE](docs/PERFORMANCE.md) | frame-rate measurements and optimisations |
| [VERIFICATION](docs/VERIFICATION.md) | how changes are checked |
| [KNOWN_ISSUES](docs/KNOWN_ISSUES.md) | open issues |

## Legal

- Tooling and runtime under GPL-3.0 (a fork of PS2Recomp), plus a patch and bridge for PCSX2 under PCSX2's license.
- **No code, data, textures, text, audio or BIOS from the game or from Sony.** Translations of the game's code are
  produced locally from your own disc and never committed.
- You must own the game and dump your own disc. Do not share dumps or generated code. This project provides no BIOS,
  keys or tools or instructions for getting around copy protection.
- Screenshots and clips in `docs/screenshots/` were taken from the port running the author's own disc.
- *TimeSplitters* is a trademark of its respective owners. This project is not affiliated with or endorsed by them.

## Credits

[PS2Recomp](https://github.com/ran-j/PS2Recomp) (recompiler and runtime base) · [PCSX2](https://pcsx2.net) (GS/SPU2
and the hardware reference) · [ps2autotests](https://github.com/unknownbrackets/ps2autotests) · ps2sdk and the PS2
homebrew community · methods from the author's
[Shadow of the Colossus port](https://github.com/Destinity13/shadow-of-the-colossus-recomp).

## AI disclosure

Built by a human maintainer together with Claude (Anthropic) working as a coding agent. The maintainer sets direction,
owns the hardware and play-testing and makes the decisions; the agent did most of the reverse engineering,
implementation, measurement and documentation. Every change is checked against the reference before it lands.
