**RPCS3 RTX Remix — preview 3**
<https://github.com/BRAGme/rpcs3-rtx/releases/tag/remix-preview-3>

306 commits on from preview 2. **Ratchet & Clank Collection renders**, and the build has caught up with upstream RPCS3.

🎮 Ratchet & Clank was a black screen. Three faults, stacked:
• Picking a game from the collection menu **crashed** — Remix takes over the game window and never hands it back, and RPCS3 re-used that window for the next game
• With the crash gone it **froze** instead — starting Remix a second time fails silently, so the renderer quietly did nothing and the last frame stayed up
• Then **every world draw was refused** — the backend looks for the game's camera maths in its shader code and expects four rows in one place; Ratchet writes one of them somewhere else and overwrites another, so it found two and gave up
• Now: 18,137,400 draws go through, 9,911 refused. That's 0.05%

💿 Caught up with upstream RPCS3 — 299 commits, including **disc support**:
• Real Blu-ray drives and mounted `.iso` files, on every platform
• Insert Disc / Eject Disc menu entries, and a Disc Games category
• Encrypted ISOs read keys from a `data\redump` folder you make yourself

⚙️ New in **Settings → RTX Remix** (all still work as env vars / `<TITLEID>.conf` too):
• **Recover split matrix rows** *(on)* — the Ratchet fix above; finds a camera in games that split their projection maths across several places
• **Camera identity tracking** *(off)* — for a view that flickers between two nearby positions
• **Emissive sky textures** + intensity + keeps blending *(new Sky group)* — makes a sky glow instead of sitting there dark. The right tool for a dome; the Sky *category* isn't, it deletes it
• **Composite render target draws** *(off)* — turn on if a game's HUD is missing entirely. Ratchet needs it
• **Paint clear colour on 2D frames** *(on)* — see below

✅ Also:
• **Splash screens and menus have a background again** — games clear the screen to a colour before drawing 2D, and this backend had nowhere to put it, so you saw the empty ray-traced scene through every gap
• **A `Remix memory:` line in the log** — live mesh/texture/material counts and what they weigh, so a leak is something you read instead of guess at. Borrowed from Dolphin RTX v0.0.7

⚠️ Known issues:
• Ratchet's **sky renders unlit** — it's there, just waiting for a light instead of acting as a backdrop
• Ratchet's **HUD needs "Composite render target draws" turned on** — the game draws it off-screen first, and that path costs a full screen of work per frame so it's off by default
• **Framerate is low** — the mesh cache holds five seconds of geometry that's mostly used once. `BCUS98282.conf` ships with a shorter setting; not yet measured
• Still a research build, nothing is playable start to finish
• Needs a Remix Plus / extended-API runtime (stock Remix won't connect) plus your own firmware

🤝 Issues are open:
• A settings file for a game nobody's covered yet is the most useful thing you can send
• No git needed — paste it into an issue and you're credited as co-author
