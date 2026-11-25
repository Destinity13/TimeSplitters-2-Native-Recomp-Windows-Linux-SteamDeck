![Crystal Cut](../docs/screenshots/banner_crystalcut.jpg)

# 1.3.3 is almost here

A quick update on what we have been working on. 1.3.3 is basically done. Over the next few hours we are testing everything on Linux, Windows and a pretty weak laptop, and if we are happy with the results it goes out right after.

## Aiming and controls

- Hip fire now hits where the crosshair is (#25, #21, #9)
- Sniper scopes pan properly at the edge of the screen (#21)
- Scopes on controller are no longer way too sensitive (#13, #30)
- Auto aim now works with modern controls on gamepad (#27)
- Mouse and gamepad at the same time, with the new option "Mouse and gamepad together"
- You can now use the mouse in the game menus, with its own TS2 style cursor
- Turrets work from awkward angles again (#24)

## Visuals

- Fixed the screen shake on Siberia: the game thought some stairs were a turret (#31)
- Fire effects and team icons line up in widescreen again (#28)
- HUD fixes for 21:9, 16:10 and 32:9
- Less bouncing when turning and smoother frame pacing with V-Sync off
- Pause menu text no longer stretches (#16)

## Setup and performance

- The "VU1 decoding produced nothing" error is fixed (#26). The setup no longer has to start the game for that step
- It only needs SSE4.1 now instead of AVX2, so much older CPUs work too. If a CPU really is too old, you get a clear message instead of a cryptic error
- The setup picks how many build jobs to run based on your free RAM, so machines with little memory don't choke anymore
- Better performance on weaker PCs

## Arcade

- Bots no longer spawn on top of each other on the extra arcade maps
- Glass challenge timer fixed

## Coming next

Real 120 fps. Our first test run hit 120 fps today, but it needs a lot more testing before it goes in. Same goes for story co op online.

## Thank you

Thanks to everyone who reported bugs and sent screenshots and videos, that made most of these fixes possible. And thanks to Lukas, who tested the Windows builds as always and once again proved that every bug can be found if you just press all buttons at the same time.
