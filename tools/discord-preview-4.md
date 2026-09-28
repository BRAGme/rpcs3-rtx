**RPCS3 RTX Remix — preview 4**
<https://github.com/BRAGme/rpcs3-rtx/releases/tag/remix-preview-4>

109 commits on from preview 3. **The Last of Us renders**, and the build is caught up with upstream RPCS3 again.

🎮 The Last of Us has carried a `BLOCKED AT STAGE 0` banner since August — it froze 25-30 seconds after boot. That froze on stock Vulkan too, so it was never ours, and it's gone. It reaches the menu and Hometown gameplay on update 1.11.

🍃 What was left was one fault wearing three faces: **every cutout in the game was solid.** The backend replays a shader's `KIL` — the throw-this-pixel-away instruction — as an alpha test, and it was assuming two things TLOU breaks:
• **The test direction.** Everything was replayed as GREATER. But `SGT` + `KIL(ne)` is LESS_OR_EQUAL, so half the cutouts were *inverted* — keeping exactly the pixels meant to be discarded
• **Where the coverage lives.** It tested the elected colour texture's alpha. TLOU doesn't keep it there — foliage and hair are in **`tex0.g`**, the Hometown window masks in **`tex2.b`**, and those colour maps are fully opaque in alpha. So the test kept everything, and every leaf card, pane and window came out a solid panel
• Now the backward slice through the shader names the unit and channel the discard *actually* tests, and bakes that channel into a derived material's alpha

🚗 **Hometown vehicle glass** gets its own path — that shader folds `tex2.g` into alpha additively *after* the colour sample, so the guest's own blend state is kept instead
🖤 **The menu background** stopped blowing out to white — that shader writes `ATTR2` to `COL0` and the backend was reading `ATTR3`, replacing an authored black with opaque white

📦 **Seven more per-game profiles.** Preview 3 shipped one, this ships eight:
• Haze · Ratchet & Clank Collection · **The Last of Us** · Resistance: Fall of Man · Saints Row 2 · Eat Lead (57 settings, the biggest) · Demon's Souls · GRAW 2
• These are settled per-title decisions that used to live in launcher scripts on one machine. They're commented — reading one is the fastest way to see what tuning a title means
• Demon's Souls also carries its authored light tables, extracted and compiled in. GRAW 2 now boots through its child-process hand-off

✨ **SHARC, if you want it.** The backend is developed against an untagged Remix Plus build (`revised-9-10`, `38082acf`) that adds a spatially hashed world-space radiance cache as `rtx.integrateIndirectMode = 3`. The API is byte-identical to tagged `remix-plus-1.5.1`, so it's purely opt-in and 1.5.1 stays the tested runtime. `SETUP.txt` has the details.
• One rename to know: the f90 / specular-level options are now `rtx.legacyMaterial.*`, not `rtx.opaqueMaterial.*`. The old spelling silently stops applying — writing both is harmless

💿 **98 upstream commits** merged, the three weeks since preview 3. No headline this time (disc support already landed in preview 3), but it includes a CPU-thread teardown deadlock fix, ISO device-path fixes, `cellVdec` shutdown fixes and a batch of `vm`/`sys_mmapper` corrections.

⚠️ Known issues:
• **Eat Lead's flat "digital" colours are still flat.** The route for it exists (`RPCS3_REMIX_FPCONSTALBEDO`) but is off by default and has *never been observed firing* — every unattended boot stops on the game's "reconnect the SIXAXIS controller" dialog first. A zero count there means no draws happened, not that it doesn't work
• Ratchet's **sky is still unlit** and its **HUD still needs Composite render target draws** on
• **Framerate is low**, and nothing in this build fixes it
• **`docs/remix/KNOBS.md` is behind the source** — 106 settings exist with no entry in the table. What's documented is still correct, it's just incomplete
• Still a research build, nothing is playable start to finish
• Needs a Remix Plus / extended-API runtime (stock Remix won't connect) plus your own firmware

🤝 Issues are open:
• A settings file for a game nobody's covered yet is the most useful thing you can send
• No git needed — paste it into an issue and you're credited as co-author
