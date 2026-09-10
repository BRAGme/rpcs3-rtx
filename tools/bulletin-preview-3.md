**RPCS3 RTX Remix — preview 3 — full change list**
<https://github.com/BRAGme/rpcs3-rtx/releases/tag/remix-preview-3>

306 commits since preview 2. Every change, one line each.

**🐛 Fixed**
• **Crash when picking a game from a collection menu** — Remix takes over the game window and never hands it back; RPCS3 re-used that window for the next game, so the first message Windows sent landed in a runtime that no longer existed. The window is handed back before Remix shuts down now
• **Frozen picture after switching games** — starting Remix a second time on the same window fails, and fails *silently*, so the renderer quietly did nothing and the last frame stayed up. Every game gets a fresh window now
• **Ratchet & Clank: the entire world was invisible** — the backend finds the camera by reading the game's projection maths out of its shader code, and expects four rows in one place. Ratchet writes one row somewhere else and overwrites another, so it found two, decided there was no camera, and threw away all 5,967,184 world draws. Now 18,137,400 go through and 9,911 are refused — 0.05%
• **Splash screens and menus showed the empty ray-traced scene** — games clear the screen to a colour before drawing 2D, and this backend has no screen to clear, so that colour existed nowhere. It's painted underneath now on any frame with no 3D in it

**⚙️ Seven settings moved into Settings → RTX Remix**
(all still work as `RPCS3_REMIX_*` variables and in `<TITLEID>.conf`; a variable always beats the tab)
• **Recover split matrix rows** *(on)* — the Ratchet fix above, for any game that splits its projection maths across several places
• **Camera identity tracking** *(off)* — for a view that flickers or snaps between two nearby positions
• **Emissive sky textures** — texture hashes that should glow, for a sky that renders as dark geometry
• **Emissive intensity** *(2.0)* — how brightly they glow
• **Emissive keeps blending** *(on)* — keeps the sky's transparency while it glows; off if a layered sky doubles up
• **Composite render target draws** *(off)* — turn on if a game's HUD is missing entirely. Ratchet needs it
• **Paint clear colour on 2D frames** *(on)* — the splash-screen fix above

**🔍 New diagnostics**
• **`Remix memory:` log line** — how many meshes, textures and materials are alive and what they weigh. If a number keeps climbing after a scene settles, something's being held that shouldn't be. Borrowed from Dolphin RTX v0.0.7
• **Sky classification and UI routing counters** now say *why* something was refused, not just that it was

**💤 In the build but switched off**
• **`RPCS3_REMIX_UIFASTRASTER`** — a faster path for the CPU-drawn 2D layer, with its own proof counters. Unmeasured, so it ships dormant; nothing changes unless you set it

**💿 From upstream RPCS3 (299 commits, six weeks)**
• **Disc support** — real Blu-ray drives and mounted `.iso` files, on every platform
• **Insert Disc / Eject Disc** menu entries, and a **Disc Games** category in the game list
• **Encrypted ISOs** read their keys from a `data\redump` folder you create yourself
• **ISO loader hardening** — 18 commits of bounds and error handling
• New GPU settings: **Disable hardware blending**, **Disable blit engine scaling**
• **Start Big Picture mode on boot** setting
• Everything else upstream did in those six weeks

**🔧 Internal**
• Followed `rsx_utils.h` to its new home after an upstream refactor — the only source break in 299 commits
• Six stale submodules updated (fusion, FAudio, curl, discord-rpc, SDL, opencv)

**⚠️ Known issues**
• **Ratchet's sky is unlit** — it renders, but as ordinary geometry waiting for a light. Needs the sky texture identified, then added to Emissive sky textures
• **Ratchet's HUD needs Composite render target draws turned on** — off by default because each such draw costs a full screen of work
• **Framerate is low** — the mesh cache holds five seconds of geometry that's mostly used once. `BCUS98282.conf` ships with a shorter setting; not yet measured
• Only a handful of titles have had real work done on them. Everything else is unexplored, not broken
• Still a research build — nothing is playable start to finish
• Needs a Remix Plus / extended-API runtime (stock Remix won't connect) plus your own firmware

**🤝 Contributing**
• Issues are open. A settings file for a game nobody's covered yet is the most useful thing you can send
• No git needed — paste it into an issue and you're credited as co-author
