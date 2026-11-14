---
title: Devlog - the road to 1.1
date: 2026-11-14
tag: Devlog
image: img/compare-11-snow.jpg
order: 2
summary: What happened between 1.0.1 and 1.1. A wrong sine in the VU1, a new launcher, a Windows test lab, mouse aiming, Online+, and the things we got wrong on the way.
---

# Devlog - the road to 1.1

1.0.1 went up on 11 November. Three days and a lot of reports later, this is what changed, how we found it, and what is still open. Longer than the release notes on purpose.

## The reports

Most of 1.1 comes from what you wrote on GitHub and Reddit:

- the launcher was German on Windows no matter what you picked
- black bars at 16:9 on 16:10 screens
- a big grey, flickering wall of smoke when bullets hit something
- snow and fog in Siberia that looked wrong and too busy
- slowdowns on older graphics cards
- requests: a field-of-view option, better mouse aiming, VR (not planned for now)

Thank you for every one of them. The smoke report alone led to the most important fix of this update.

## The smoke, the snow and one wrong sine

![The smoke when bullets hit a surface, before and after](img/smoke-before-after.jpg)

This one took the longest, and we were wrong about it twice.

**First guess: it is the scaling.** At 3× and above the smoke looked like a hard grey patch and the white haze over Siberia pulsed. We blamed the high internal resolution and even wrote into the known issues that the haze "only looks busier" when it is rendered sharper. That was wrong.

**Second guess: one function.** A fix in the arctangent of the vector unit made the smoke in the tunnel look right. We called it fixed. It was fixed by accident, in one spot.

**What it really was.** The PS2's second vector unit (VU1) has a small group of math instructions: square root, reciprocal, sine, arctangent, exponential. In our table four of them sat on the wrong index. When the game's particle code asked for a sine, it got something else.

TimeSplitters 2 rotates every particle sprite with `sin(angle)` and `sin(angle + 90°)`, and the angle grows with the age of the particle. With a wrong sine the sprite corners went anywhere: smoke became a huge flat patch, debris was far too big, and the snow haze flashed over the whole picture.

How we found it:

1. a fixed route through the first story level, so every run shoots the same wall at the same tick
2. the same scene in PCSX2 as the reference
3. a dump of every draw call from our renderer, and a dump of the VU1 memory at the moment the bad sprite is sent
4. reading the particle program instruction by instruction until one result made no sense
5. comparing our whole opcode table with PCSX2's: 130 entries, four wrong

After the fix the smoke is a soft puff, debris has its size, and the haze is calm. Measured over six frames at 6×, the brightness swing of the haze went from 9–16 down to about 1. The story picture is closer to PCSX2 than before.

The lesson for us: when something looks wrong, compare values with the working reference first, and guess later.

## Particles and the HUD in 16:9

Our widescreen HUD rule moves 2D elements to the screen edges. It also caught snow, sparks, debris and smoke, because the game sends them in a similar way, and pulled them towards the middle. Worse, smoke on screen made the HUD jump inward.

Particles with a depth test are left alone now. One case is still open: the HUD can jump for a moment while the red damage arcs are drawn.

## Picture

- fits 16:9, 16:10, 21:9 and 32:9 without bars or stretching
- **"Auto" internal resolution** is the default now: it picks a resolution for your screen and steps down when the frame rate stays low
- menus and HUD in 16:9
- sniper scope: Original, Clear or Extra clear
- texture packs in PCSX2 format (`texture_packs` folder, none included)

## Controls

- **Mouse aiming rebuilt:** crosshair fixed in the centre, shots land on it from the first shot, no auto-aim pulling the view, raw input on Linux
- weapons like the Soviet S47 no longer stay zoomed in while aiming with the mouse
- the sniper scope opens with the mouse, and the zoom key no longer skips a level
- **Modern gamepad aiming** (optional, off by default)
- **Unlock everything** (optional, your own progress stays untouched)

## The new launcher

One small app for setup, settings, news and updates, in English and German.

- setup from your own disc image with a progress display
- **automatic updates:** on start it looks at the latest release, downloads only the files that changed (checked by SHA-256), backs up your saves and settings first and relinks the game with the disc image it remembers. Only updates that need a full new setup ask first
- saves live in `savedata/` at the top of the folder now, old saves are moved once

The update path is new. 1.1.0 is the first release that carries update files, so the first real update will be the one after it.

## Sorry for the German

The project started in German, and it showed: launcher texts, file names, folders, log messages. On Windows the launcher even ignored the language you picked. Then our own packaging script broke the Windows start script by putting a header in front of the byte-order mark, so `-Setup` and `-Play` were ignored and dialogs came up instead.

All of that is English by default now. German is a language you can choose.

## A Windows test lab

The main development machine runs Linux. Until now Windows was tested on CI runners and by a friend, build after build. That was too slow.

Now there is a second machine, an older Windows 11 laptop with a GTX 1050. The Linux PC builds the package and puts a job in a queue. The laptop fetches it over the local network, sets the game up from the disc image, plays a scripted route and sends logs and screenshots back. Nobody has to touch it.

The first runs found two bugs within an hour (the byte-order mark, and the `.bat` not opening the new launcher). 1.1.0 was set up and played on that laptop before release: launcher start, setup without a dialog, story run through Direct3D 11, smoke and debris correct.

## The laptop that played on the wrong graphics chip

The laptop has two graphics chips, an Intel one and a GTX 1050. Our first frame rates on it were bad: 14 to 30 FPS at 4× in the first minute, 36 on average. We built a guard that lowers the "Auto" resolution when a PC cannot keep up, and found a bug in it on the way (it only stepped down after it had seen 60 FPS once, so it never helped the PCs that needed it).

Then the question came up that should have been asked first: which chip is the game running on? The log said `Intel(R) HD Graphics 630`. Windows gives a program the power-saving chip unless the program asks for the fast one, and ours did not ask.

Now the game marks itself for the fast card and the renderer picks the high-performance adapter by name, with a fallback to the Windows default if that fails. Same laptop, same run, with a new graphics driver installed in between: 58 FPS on average at 4×, and the log says `NVIDIA GeForce GTX 1050`. If you played 1.0.x on a laptop and it felt slow, this was probably why.

## Online+

An optional second edition with its own lobby in the game's menus: server browser, quick match, bot match, profile and ranks, hit markers, killfeed and scoreboard.

It has no goal. It happened on the side to see how far the game can be changed using only what is already in it: its own menus, its own on-screen messages, its own HUD. Some of it nobody needs and some of it will not stay. It is a separate edition you start on purpose, and the classic game and its multiplayer stay as they are.

## Under the hood

- a test suite replays story, sniper, deathmatch and Online+ scenes before every package
- every package is scanned for personal data before it leaves the machine
- crash reports contain useful information, log files are smaller
- per-draw dumps and VU1 dumps as diagnosis tools, the ones that found the sine

## What we got wrong

- "the haze only looks busier at high resolutions" - no, it was a bug
- "smoke fixed" after the first patch - it was one spot, by luck
- two more wrong guesses about texture coordinates and the camera blur switch
- the launcher wrote `true`/`false` where the game expects `1`/`0`, so no switch in it could be turned on or back off
- the first Windows package that went to a tester was missing a DLL the launcher needs. The lab had tested setup and game, but never started the launcher itself
- German in a release for an English-speaking community

## Still open

- HUD jump while damage arcs are drawn (16:9)
- mouse aiming: switching back to the sniper rifle can bring the scope up once for a moment
- Online+: the scoreboard does not always count your own kills, bot matches can stutter
- a faint straight edge on impact smoke at very close range
- explosions and fountain water are not yet compared one by one with PCSX2
- Windows is tested on one laptop and the CI runners; AMD and Intel graphics are untested by us
- 3–4 players, later story levels by hand

## Next

- an auto-aim switch for the original controls
- a field-of-view option
- the new options inside the game's own menus
- more scenes compared against the original, one by one

Everything new stays optional. The game as it was on the PS2 stays the default.
