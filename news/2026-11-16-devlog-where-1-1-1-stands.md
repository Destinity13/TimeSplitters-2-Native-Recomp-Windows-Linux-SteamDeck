---
title: "Devlog: where 1.1.1 stands"
date: 2026-11-16
tag: Devlog
order: 1
summary: Update 1.1.1 is taking longer than planned. What is already fixed, what comes after it and what we are working on right now.
---

# Devlog: where 1.1.1 stands

16 November 2026

Update 1.1.1 is taking longer than planned. Here is why, and what is already done.

## Already fixed

* Sniper scope: picture no longer too bright, no more strips at the edge of the scope
* AK zoom no longer gets stuck after switching weapons
* Linux: crash when starting with vkBasalt is fixed

## Coming after 1.1.1 (1.1.2)

* Field of view slider in the launcher
* Number keys select weapons in the same order as the game

## What we are working on right now

Several of you reported a blurry double image, mostly in the distance and at higher resolutions (issue #9). We found where it comes from. Every frame, TimeSplitters 2 copies the finished picture back onto itself as a light overlay. On a PS2 that copy lines up exactly. At higher internal resolutions it ends up a little off, so you see a second, shifted image on top. PCSX2 users with upscaling run into the same thing, so it is not something only our port does. We are fixing it at the source instead of just blurring over it.

We are also looking at hipfire with modern controls, where shots do not always land exactly on the crosshair.

## How we test

Every build runs automatic tests on Linux, Windows and the Steam Deck. We also compared all 10 story missions frame by frame against the original running in PCSX2.

We would rather ship 1.1.1 a bit later than ship it with a picture that looks worse than it should. Thanks for the reports and the patience. Please keep them coming, logs and screenshots help a lot.
