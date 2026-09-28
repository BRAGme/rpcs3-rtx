109 commits since preview 3. The headline: **The Last of Us renders**, and the build is caught up with upstream RPCS3 again. Seven more per-game profiles ship with it, where preview 3 shipped one.

This is still a **research build**. Nothing is playable start to finish. Read "What does not work" before downloading.

---

## The Last of Us now renders

Since 2026-08-15 the launch script for this title has carried a banner reading `STATUS: BLOCKED AT STAGE 0`. It froze 25-30 seconds after boot, just past the save-slot scan. That freeze reproduced identically on the stock Vulkan renderer, so it was never a Remix defect, and it is gone. The title now reaches the menu and Hometown gameplay on update 1.11.

What was left after it booted was one fault wearing three faces: **every cutout in the game was solid.**

The backend replays a fragment shader's conditional `KIL` — the instruction that throws a pixel away — as an alpha test. It had been assuming two things about every such shader, and The Last of Us breaks both.

**1. It assumed the test direction.** Every `KIL` was replayed as a GREATER test. But `SGT` followed by `KIL(ne)` is a LESS_OR_EQUAL test, so half the cutouts in the game were inverted — keeping the pixels they were meant to discard. The recovered comparison is now carried as a real comparison op, and defaults to ALWAYS, meaning "not recovered", rather than guessing a direction.

**2. It assumed where the coverage lives.** The replay tested the alpha channel of whichever colour texture the backend had elected. The Last of Us doesn't keep coverage there. Foliage and hair coverage live in **`tex0.g`**; the Hometown window masks live in **`tex2.b`**. The elected colour maps are fully opaque in alpha — so testing that alpha kept every pixel, and every leaf card, every pane and every window came out a solid panel.

The backward slice through the shader now names the texture unit and channel the discard *actually* tests, and that channel is baked into a derived material's alpha. Channel indices are recovered in RGBA and mapped to BGRA on the way in, which is the kind of detail that silently produces a blue tree if you get it wrong.

Two deliberate guards on this, because the path is new:

- It only fires when the coverage texture shares the albedo's UV varying and addressing. A mask sampled with different coordinates is never quietly composited against the wrong texels.
- When the derivation refuses, the old GREATER replay stands unchanged. Applying a recovered direction to the old opaque alpha would *erase* a LESS_OR_EQUAL cutout in profiles that are already settled, and the titles we ship configs for cannot absorb that.

**The Hometown vehicle glass** is handled beside this rather than through it. That shader blends on `SRC_ALPHA` and folds `tex2.g` into its alpha additively *after* the colour texture is sampled, and the remaining terms are computed per-pixel with no Remix material that can express them. For that one title/shader triple the guest's own blend state is kept and the recoverable opacity map is used in place of the provably-wrong opaque alpha.

**The menu background** was blowing out to white. That shader writes `ATTR2` straight to `COL0`; the backend read the conventional `ATTR3` instead and replaced the authored black of the fade quad with the compositor's opaque-white default.

---

## Seven more per-game profiles

Preview 3 shipped one tuned `<TITLEID>.conf`. This build ships eight:

| | |
|---|---|
| `BLUS30094` | Haze |
| `BCUS98282` | Ratchet & Clank Collection |
| `BCUS98174` | **The Last of Us** — new |
| `BCUS98107` | Resistance: Fall of Man |
| `BLUS30201` | Saints Row 2 |
| `BLUS30267` | Eat Lead — the largest at 57 settings |
| `BLUS30443` | Demon's Souls |
| `NPUB30502` | Ghost Recon Advanced Warfighter 2 |

These are the settled per-title decisions that previously existed only in launcher scripts on one machine. They are commented; reading one is the fastest way to understand what tuning a title actually involves. A profile is read once at backend init, so changing one needs a restart, and a variable already set in your environment still beats the file.

**Demon's Souls** additionally carries its authored light tables, extracted from the game and compiled into the build. **GRAW 2** now boots through its child-process hand-off: the runtime is shut down without being unloaded, because the Reflex PCL-stats ping thread outlives `Shutdown()` and unloading it crashes.

---

## SHARC, if you want it

The backend is developed against an untagged build of the Remix Plus fork — Kim2091's `revised-9-10`, commit `38082acf` — which adds **SHARC**, a spatially hashed world-space radiance cache, as `rtx.integrateIndirectMode = 3`.

The API is byte-identical between that build and the tagged `remix-plus-1.5.1`, so this is purely opt-in and `1.5.1` remains the runtime this build is tested against. `SETUP.txt` explains how to get the newer one if you want to try SHARC.

One rename to know about: the f90 / specular-level patch this fork used to carry locally is now upstream in Kim's branch, with its options spelled `rtx.legacyMaterial.*` instead of `rtx.opaqueMaterial.*`. An `rtx.conf` written against the old spelling silently stops applying. Writing both spellings is harmless and correct on either build.

---

## Eat Lead's constant colours

*Added 2026-09-28, after this build was published — the original notes said this had never
been observed working, and that was wrong.*

Eat Lead paints its "digital" effects from a colour held in a **fragment-program constant**
rather than in a vertex attribute. A census of the title's 62 cached fragment programs found
that no program reads `diff_color`, `spec_color`, `fogc` or `wpos`, and that there are no `LRP`
opcodes at all — so the vertex-colour route was structurally a dead end here. Draws such as
`fpraw=6FD147A3B666FC1E` carry `albedo=0000000000000000` with no texture bound, which is why
the surface rendered flat white: the constant was the draw's only colour and nothing read it.

`RPCS3_REMIX_FPCONSTALBEDO` states that literal through the instance's fixed-function stage.
It is a bitmask: bit 1 the unconditional `MOV` literal, bit 2 the lerp endpoint, bit 4 the
per-channel tint.

**What this fixes, precisely: the blood pixels and the text render correctly.** Confirmed by
play-test. It does **not** fix the title's other "digital" effects -- objects disappearing, the
destroyed-object warp -- which still do not come through and are not explained by this route.

The knob is `0` globally, but **`BLUS30267.conf` in this zip already sets it to `7`** — all
three routes — alongside `FPCONSTMRT=1`. So it is on for this title as shipped and there is
nothing to enable.

What the original notes got wrong: they repeated a caveat written on 2026-09-10, when the route
had genuinely never fired because unattended boots stopped on the game's "reconnect the SIXAXIS
controller" dialog before any draw reached it. The profile was tuned on 2026-09-20 and shipped
switched on; the caveat was never re-checked against it. The `SETUP.txt` inside the downloaded
zip carries the same stale sentence.

---

## Caught up with upstream RPCS3 again

98 upstream commits, the three weeks since preview 3, merged in. Nothing headline-sized this time — preview 3 already brought disc support — but it includes a deadlock fix for CPU threads terminating abnormally, several ISO device-path fixes, `cellVdec` decoder-shutdown fixes, and a batch of `vm::page_protect` and `sys_mmapper` corrections.

---

## What does not work

- ~~**Eat Lead's flat "digital" colours are still flat.**~~ **Corrected after release — this works.** See "Eat Lead's constant colours" above.
- **Ratchet & Clank's sky is unlit** and its **HUD is missing** unless Composite render target draws is on. Both carried over from preview 3.
- **The framerate is low**, and no profile in this build fixes it.
- **`docs/remix/KNOBS.md` is behind the source.** 106 environment settings exist in the backend with no entry in that table. The table is still correct about what it does document; it is just incomplete, and regenerating it is queued.
- A subset of draws still lands at runaway positions and reads as white shards.
- Only the nine titles named in `SETUP.txt` have had real work done. Everything else is unexplored, not broken.
- Needs a Remix Plus / extended-API runtime — stock RTX Remix will not connect — plus your own firmware.

---

## Getting started

Extract the zip anywhere and run `rpcs3.exe`. `SETUP.txt` inside covers the rest, including the two things that are deliberately *not* in the zip and without which it will not run.

Issues are open. A settings file for a title that isn't covered yet is the most useful thing you can send; paste it into an issue and you're credited.

---

## Release asset

`rpcs3-rtx-remix-c68babbd-win64.zip`
SHA256 `bb91437cd5b7f540f21826d99d16ad71631fbd166e832b2466daffe4f1f5c118`

Built from `c68babbd8ba24b7dbe3717bbd210b783161f701f`, tag `remix-preview-4`. The
exe reports `0.0.42-c68babbd Alpha | remix-backend | local_build`, and `SETUP.txt`
inside the zip names the same commit.
