![Crystal Cut](../docs/screenshots/banner_crystalcut.jpg)

# Devlog: the full Crystal Cut polish list

We went through every report again. Every open GitHub issue, every closed one, Reddit, YouTube and the RecompLauncher page. Everything that is still broken or that we can't prove is fixed went onto one list, and that list is what we're working on now. All of it, not just the easy parts.

We don't want a port that mostly works. We want TimeSplitters 2 to look and play the way it did on the PS2, just sharper and on your PC. PCSX2 is our reference for every fix, and every bug gets its own test that stays in our automatic test run, so it can't quietly come back later.

A big thank you to everyone who reported something. Every screenshot, clip and log on this list made it much easier to track things down.

## Graphics and widescreen

These come first.

- **Broken or missing textures on Windows** (#23). Only on Windows so far and it goes away with some Crystal Cut options turned off. Our Windows test laptop is back online for exactly this one.
- **Bouncing and jitter when turning**, even on fast PCs. Most likely frame pacing, not raw performance.
- **Odd effects and HUD behaviour** in most levels (RecompLauncher report).
- **Stutter with VSync off** that goes away after a restart.
- **Screen shake in Siberia** on an Intel Arc handheld (#31).
- **Fire effect beside the burning player** in Virus and Flame Tag (#28). We can reproduce the match on our side now and the flame core is off by exactly the amount our widescreen HUD handling squeezes things. Same family as the light glow bug from 1.3.2.
- **Team icons above heads** drifting sideways in Team Deathmatch (#28).
- **Stretched text and images at the screen edge**, like the NeoTokyo terminal view and pause menu pages sliding in (#16).
- **MapMaker tile placement** slightly off in widescreen.

## Aiming and controls

- **Hip fire doesn't land on the crosshair**, shots go down and to the right (#25, #21). The 1.3.2 fix only covered aim mode, and you rightly told us so.
- **Sniper scope doesn't pan** at the screen edge (#21).
- **Scope aiming on controller**, the right stick doesn't turn the view and max zoom shrinks the crosshair range (#30). Scope sensitivity on controller is still too high as well (#13).
- **Turrets started from an odd angle** only work after lining up the view (#24).
- **Mouse and controller at the same time**, for example gyro through Steam Input (#20, #25).
- **Auto aim with modern controls** (#27). This one is a wish, not a bug, and comes after the fixes.

## Gameplay

- **Challenge timer** in the glass smashing challenges counts a few seconds too much for the medal.
- **Bots all spawning on the same spot** in Arcade.
- **Regenerate hanging while loading** in Arcade with heavy bots and custom settings.
- **Bots walking through walls** with a high FOV setting.

## Setup and launcher

- **Setup stops at "VU1 decoding produced nothing"** (#26). We found two different causes. On CPUs without AVX2 (older AMD FX and Intel chips) the build used instructions the CPU can't run. The setup now checks the CPU and builds without AVX2 where needed. On Linux a graphics library was missing from the package, so the Vulkan renderer couldn't start during setup. That gets fixed too.
- **Windows launcher closing right away** for some players.

## Performance

- **Steam Deck at a steady 60 FPS** without drops.
- **Drops on weaker PCs** even on the lowest settings (#19, GTX 960M report).

## Old fixes we test again

These were fixed in earlier versions. We play through each of them again on the current build before we call them done.

- Turrets with mouse aim and turrets used a second time (#6, #8, #11, #18)
- Weapons stuck in zoom and auto zoom after picking up a weapon (#5, #7, #8, #10)
- Crosshair in cutscenes and the mouse leaving the window (#7, #8)
- Cut off settings menu and shifted images (#7)
- Temporal Uplink position (#12)
- Difficulty select bars (#15)
- HD textures and HD text (#17)
- Double image and blur at high resolutions (#8, #9)
- Smoke wall when bullets hit (#3)
- MapMaker walls (#14)
- Setup hanging at 68 percent on Windows (#4)

## When is the next update

When this list is done and the exact release package has been tested on Linux, Windows and Steam Deck. Not before. We'd rather take a few more days than ship another update that only half fixes things.

Found something that isn't on this list? Open an issue or leave a comment on Reddit or YouTube. A screenshot or short clip plus `app/build/play.log` helps a lot.
