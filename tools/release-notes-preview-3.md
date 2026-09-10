304 commits since preview 2. Two headlines: **Ratchet & Clank Collection renders**, and the build is **caught up with upstream RPCS3** — which brings disc support with it.

This is still a **research build**. Nothing is playable start to finish. Read "What does not work" before downloading.

---

## Ratchet & Clank Collection now renders

It used to be a black screen. That was three separate faults stacked on top of each other, and each one hid the next.

**1. Picking a game from the collection menu crashed the emulator.**

The Remix runtime takes over the game window when it starts and never gives it back when it shuts down. RPCS3 re-used that same window when the collection launched Ratchet & Clank 1, so the first message Windows sent to it landed in code belonging to a runtime that no longer existed. RPCS3 now hands the window back before shutting the runtime down.

**2. With the crash gone, the picture froze instead.**

Shutting the Remix runtime down does not release its claim on the window, so starting it a second time failed — and the failure is silent. The renderer quietly did nothing and the last frame stayed on screen, which looks exactly like a hang. Each game now gets a fresh window.

**3. Then every piece of the world was thrown away.**

The backend works out where the camera is by finding the game's projection maths in its shader code. It expects the four rows of that matrix to be written in one place. Ratchet & Clank writes one row somewhere else and copies it across, and overwrites another one afterwards — so the backend found two rows out of four, concluded there was no camera at all, and refused every single world draw. All 5,967,184 of them.

It now follows those two extra steps. After the fix, 18,137,400 draws go through and 9,911 are refused — 0.05%.

---

## Also new

**Splash screens and menus have a background again.** Games clear the screen to a colour before drawing a menu. This backend has no screen to clear, so that colour existed nowhere and you saw the empty ray-traced scene through every gap. On a frame with no 3D in it, the game's own clear colour is now painted underneath.

**Six settings moved into the Settings window.** Sky brightness for games whose sky renders unlit, camera tracking for a view that flickers, HUD compositing for games that draw their HUD off-screen first, and three more. They were environment variables before, which meant editing a launcher script.

**A memory line in the log.** `Remix memory:` reports how many meshes, textures and materials are alive and what they weigh. If a number keeps climbing after a scene has settled, something is being held that shouldn't be. Borrowed from Dolphin RTX v0.0.7, which added the same thing for the same reason.

---

## Caught up with upstream RPCS3

299 upstream commits, six weeks of them, merged in. The one worth naming:

**Disc support.** Real Blu-ray drives and mounted `.iso` files both work now, on every platform. There are Insert Disc and Eject Disc menu entries and a Disc Games category in the game list. Encrypted ISOs read their keys from a `data\redump` folder you create yourself.

Everything else upstream did in those six weeks comes with it.

---

## What does not work

- **Ratchet & Clank's sky is unlit.** It renders, but as ordinary geometry waiting for a light to reach it rather than as a backdrop. The fix needs the sky texture identified first.
- **Ratchet & Clank's HUD is missing** unless you turn on "Composite render target draws". The game draws its HUD into an off-screen buffer and copies it over, and that path is off by default because it costs a full screen of work per frame.
- **The framerate is low.** The mesh cache holds five seconds of geometry, most of which is used once. `BCUS98282.conf` ships with a shorter setting; whether it is enough is not yet measured.
- Only a handful of titles have had real work done on them. Everything else is unexplored, not broken.
- Needs a Remix Plus / extended-API runtime — stock RTX Remix will not connect — plus your own firmware.

---

## Getting started

Extract the zip anywhere and run `rpcs3.exe`. `SETUP.txt` inside covers the rest.

The `<TITLEID>.conf` files beside the exe are the per-game settings, with comments explaining each one. `BCUS98282.conf` is the new one — read it if you want to see what tuning a title actually looks like.

Issues are open. A settings file for a game that isn't covered yet is the most useful thing you can send; paste it into an issue and you're credited.
