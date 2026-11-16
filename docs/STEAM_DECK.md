# Steam Deck / SteamOS

> Status: **tested on a Steam Deck by the maintainer** (v0.9.4 and later): full speed in Game Mode, controls and
> menu working. Please report your results in the issue tracker.

The package brings its own compiler (Zig/clang) and prebuilt libraries, so you don't need to install anything on SteamOS.
The read-only system stays untouched, and you don't need distrobox or developer mode.

## One-time setup (Desktop Mode)

1. Switch to Desktop Mode: hold the power button, then pick *Switch to Desktop*.
2. Download the release `.tar.xz` and extract it, for example to `~/Games/TS2-Recomp/`. You need about 5 GB free.
3. Copy your **TimeSplitters 2 (USA)** ISO onto the Deck.
4. Double-click `TS2-Recomp.sh` (or `TS2-SteamDeck.sh`). The launcher opens, the same one as on every other PC.
   Click *Set up* and select the ISO. It checks it, extracts it and builds the game.
   This takes roughly 10–15 minutes on the Deck and only happens once.

SteamOS has no WebKitGTK, so since 1.2 the package brings the launcher a second time as `TS2-Launcher.AppImage`
with its own copy. The start scripts pick it by themselves; nothing needs installing. Before 1.2 the launcher did not
open on the Deck (you got simple dialogs instead) and the game could not update itself, so download 1.2 once by hand.

## Add to Steam (Game Mode)

1. In Desktop Mode, open Steam → *Games* → *Add a Non-Steam Game to My Library* → *Browse*.
2. Select `TS2-SteamDeck.sh`. Set the file filter to *All files* so the script shows up.
3. Right-click the new entry → *Properties*:
   - Name: `TimeSplitters 2`
   - Icon: `tools/installer/ts2-recomp_256.png`
4. Go back to Game Mode and start it from your library. The launcher opens there too (use the right trackpad
   as the mouse), so you get news and updates without switching to Desktop Mode. Press *Play* to start the game.
5. To skip the launcher and start the game directly, set the launch option `TS2_DIREKT=1 %command%`.
   Updates are then only installed when you open the launcher.

## Controls and tips

- The Deck controls work as a gamepad automatically. While the gamepad is in use, keyboard and mouse events from
  Steam's desktop layout are ignored (no stray weapon switches), and the mouse cursor is hidden.
- The Deck is detected automatically: 2× internal resolution without FXAA for a steady 60 FPS. Settings you change
  in the in-game menu take priority.
- **In-game menu:** press **Select + Start**. The Steam button opens the Steam overlay instead.
- 2× (the Deck default) is plenty for the 1280×800 screen and saves battery.
- 4-player split-screen works with external controllers. Plugging in a third controller turns on the multitap.
- Saves are stored in the extracted folder under `savedata/`.

## Troubleshooting

If the build fails, attach `app/build/setup.log` from the extracted folder to an issue. If the game freezes, attach
`app/build/play.log`: it contains an automatic hang report. If the launcher does not open, `./TS2-Recomp.sh` falls
back to simple dialogs; please report that too.
