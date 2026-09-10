306 commits since preview 2. Two headlines: **Ratchet & Clank Collection renders**, and the build is **caught up with upstream RPCS3** — which brings disc support with it.

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

## New settings

Six settings that used to be environment variables are now in the **Settings → RTX Remix** tab. Everything on that tab is also still an `RPCS3_REMIX_*` variable and still settable per game in a `<TITLEID>.conf` file, and a variable always wins over the tab.

**Geometry**
- **Recover split matrix rows** *(on)* — the Ratchet & Clank fix above. Finds a camera in games that write their projection maths across several places instead of one. Turning it off restores exactly how every game was read before.

**Camera**
- **Camera identity tracking** *(off)* — for a view that flickers or snaps between two nearby positions. Follows the camera by identity over time instead of picking one afresh every frame: the longest-serving candidate keeps it, survives a few frames where it isn't drawn, and hands over only when it really disappears.

**Sky** *(new group)*
- **Emissive sky textures** — a list of texture hashes that should glow. Use it when a sky renders as ordinary dark geometry waiting for a light to reach it. This is the right tool for a sky dome; the Sky *category* below it is not, because a runtime that hides what it tags removes the dome from the scene entirely.
- **Emissive intensity** *(2.0)* — how brightly those textures glow.
- **Emissive keeps blending** *(on)* — keeps the sky's own transparency while it glows. Turn it off if a layered sky comes out doubled or washed out.

**Interface**
- **Composite render target draws** *(off)* — for a game whose HUD is missing entirely. Some games draw the HUD into an off-screen buffer and copy it over; those draws are skipped by default because each one costs a full screen of work. Ratchet & Clank needs this on.
- **Paint clear colour on 2D frames** *(on)* — see below.

Also in the build but **off by default**, environment variable only: `RPCS3_REMIX_UIFASTRASTER`, a faster path for the CPU-drawn 2D layer. Unmeasured, so it ships dormant — nothing changes unless you set it.

---

## Also new

**Splash screens and menus have a background again.** Games clear the screen to a colour before drawing a menu. This backend has no screen to clear, so that colour existed nowhere and you saw the empty ray-traced scene through every gap. On a frame with no 3D in it, the game's own clear colour is now painted underneath.

**A memory line in the log.** `Remix memory:` reports how many meshes, textures and materials are alive and what they weigh. If a number keeps climbing after a scene has settled, something is being held that shouldn't be. Borrowed from Dolphin RTX v0.0.7, which added the same thing for the same reason.

---

## Caught up with upstream RPCS3

299 upstream commits, six weeks of them, merged in. The one worth naming:

**Disc support.** Real Blu-ray drives and mounted `.iso` files both work now, on every platform. There are Insert Disc and Eject Disc menu entries and a Disc Games category in the game list. Encrypted ISOs read their keys from a `data\redump` folder you create yourself.

Everything else upstream did in those six weeks comes with it.

---

## What does not work

- **Ratchet & Clank's sky is unlit.** It renders, but as ordinary geometry waiting for a light to reach it rather than as a backdrop. Fixing it needs the sky texture identified first, then added to Emissive sky textures above.
- **Ratchet & Clank's HUD is missing** unless you turn on Composite render target draws.
- **The framerate is low.** The mesh cache holds five seconds of geometry, most of which is used once. `BCUS98282.conf` ships with a shorter setting; whether it is enough is not yet measured.
- Only a handful of titles have had real work done on them. Everything else is unexplored, not broken.
- Needs a Remix Plus / extended-API runtime — stock RTX Remix will not connect — plus your own firmware.

---

## Getting started

Extract the zip anywhere and run `rpcs3.exe`. `SETUP.txt` inside covers the rest.

The `<TITLEID>.conf` files beside the exe are the per-game settings, with comments explaining each one. `BCUS98282.conf` is the new one — read it if you want to see what tuning a title actually looks like. `docs/remix/KNOBS.md` in the repo documents all 271 settings.

Issues are open. A settings file for a game that isn't covered yet is the most useful thing you can send; paste it into an issue and you're credited.
