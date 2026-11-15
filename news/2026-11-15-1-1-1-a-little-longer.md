---
title: Update 1.1.1 takes a little longer
date: 2026-11-15
tag: Update
order: 3
summary: In the last round of testing we found a few new bugs. We want to fix them before 1.1.1 goes out, so you get an update that really works.
---

# Update 1.1.1 takes a little longer

15 November 2026

1.1.1 was almost ready. In the last round of testing we found a few new bugs, and we would rather fix them first than ship an update with known problems. So 1.1.1 takes a little longer.

## What we found

- **Zoom stays on after the sniper rifle.** After using the Sniper Rifle and switching to another weapon with the mouse wheel, the Soviet S47 and other weapons could zoom in when aiming. This one is already fixed and in testing.
- **Linux: crash at the start with picture effects.** With the effect presets "Clear" or "Cinematic" turned on, the game could crash right at the start on Linux if the game folder has a space in its name. This is fixed too.
- **Picture looks smeared, doubled and too bright.** Compared to videos of the real PS2, many missions look smeared and too bright, outdoors most of all. Parts of the picture lie over each other like a doubled, shifted copy, and in the sniper scope on 16:9 and wider screens it is even stronger. We are comparing our picture with a reference render right now to find the cause.

## Found something else? Please tell us

If you run into a bug, please open an issue on GitHub and attach what you can:

- `app/build/play.log` from your game folder (and `app/build/setup.log` if the setup is the problem)
- a screenshot (F12 in the game, saved to your Pictures folder under "TimeSplitters 2")
- your system: Windows, Linux or Steam Deck, graphics card, screen resolution
- the mission and what you did when it happened

Every report with logs helps us find the cause much faster.

## Windows setup and Steam Deck

The new Windows setup is being tested on our test laptop right now, every way of building the game one after the other. The first runs passed. On the Steam Deck the setup through the launcher now works as it should.

## When

As soon as these fixes are tested. We don't want to give a date and then miss it. Thanks for all your reports and your patience, they help a lot.
