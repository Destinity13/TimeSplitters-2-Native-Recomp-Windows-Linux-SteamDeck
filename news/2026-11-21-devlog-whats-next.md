![Crystal Cut](../docs/screenshots/banner_crystalcut.jpg)

# Devlog: what we're working on and what could come

1.3.2 fixed the two biggest widescreen bugs, the crosshair and the light glows. While hunting those we learned a lot about how the game draws its 2D parts, and that helps with the rest of the list. Here's where things stand.

## Working on next

- **MapMaker tile placement in widescreen.** The grid in the background stretches across the full width while the tiles and the cursor stay in the 4:3 area, so they don't line up perfectly. Cause found, the fix comes next. Thanks to patzilla777!
- **Texture glitch in the level select (#23).** Some players see black or wrong textures in the menus. We couldn't reproduce it yet on Linux, neither with the latest build nor with the release package, so we're looking at the Windows side and specific settings. Thanks to SusDTSteve for the report!
- **Mouse and controller at the same time (#20).** Right now the controller locks the mouse out. You'll be able to use both, which also makes gyro aiming through Steam Input possible.
- **More widescreen spots.** The pause menu edges, the monitor in NeoTokyo and a few menus.
- **Co-op cutscenes in split screen.**
- **Windows launcher closing right away** on some systems.

## What could come

- **Cinema mode.** Hide the HUD and the weapon for screenshots and videos.
- **TimeSplitters 1 MapMaker music.** We found TS1 leftovers on the disc. If the MapMaker music is among them, it goes back in.
- **Better performance with lots of bots** on slower PCs.
- **Public online servers**, so you can play over the internet without hosting a server yourself.
- **Even more cut content** from the disc, and a look at the GameCube and Xbox data.

Nothing here is promised for a date. We'd rather get it right than rush it.

Found something? Open an issue or drop a comment on Reddit or YouTube. A screenshot or short clip plus `app/build/play.log` helps a ton. Every fix starts with a report.
