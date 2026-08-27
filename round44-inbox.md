# Round 44 inbox — RPCS3 RTX Remix backend, Haze (BLUS30094)

Written at the close of round 43, 2026-08-26. Branch `remix-backend`, HEAD **`f091ff5a4`**, working tree
carries rounds 37–43 (39–41 committed; 42's and 43's source edits and the launcher are not).

| artefact | SHA256 (first 16) | note |
| --- | --- | --- |
| `bin\rpcs3.exe` | `ADA8AD6CDCDB53B1` | built this round, MSBuild `Release\|x64` exit 0, **0 errors, 1 warning** |
| `bin\rpcs3-next.exe` | `ADA8AD6CDCDB53B1` | **identical — the copy was made** |
| `bin\remix\d3d9.dll` | `16A0B512F33EBB66` | **UNCHANGED.** Nothing deployed into `bin\remix\`. |
| `bin\rtx.conf` | `215975B8697DE21B` | **UNCHANGED.** Read only, never written. |

The one warning is the pre-existing C4723 "potential divide by 0", now at `RemixGSRender.cpp:11456`
(round 42 recorded 11389). **Zero new warnings.** HEAD `f091ff5a4` at both ends — the tree did not move.

This round shipped twice: an albedo fix that was play-tested and **reverted**, and a narrower replacement.
Full derivations in `docs\remix\KNOBS.md` — read **"Round 43b"** first, then "Round 43".

---

## PLAY-TEST CARD — one change, one knob, one-line revert

### The white walls: a parallax HEIGHT MAP is bound as the albedo

The diagnosis is unchanged and confirmed. `tex_albedo_ucode = 0` against `tex_albedo_guess = 7,383,877` —
the ucode albedo discriminator resolved **zero** draws in a 41,802-flip run. It declines whenever
`colour_mask == referenced`, and the backend then takes the **lowest referenced unit**. On
`fp=65a91390aaf6bef3`, the program on the picked white wall `9AAA430414B3D49D`, that unit is a
single-channel parallax height map used only to perturb a UV:

```
  9: TEX R2.x   TEX2, tex0     ONE channel    <- elected today
 18: ADD R4.xy  TEX2, R2                      <- used as a UV perturbation
 24: TEX R2     R4,   tex1     four channels  <- the real diffuse
```

A near-white greyscale height map bound as albedo **is** a white wall. Round 42's vertex-colour work was
correct and could never have fixed it: all five hashes replay `vcol0=FEFEFE`, a neutral factor.

### What is armed now: `RPCS3_REMIX_FPALBEDONARROW=1`

Drop units that **cannot** carry RGB — every sample of them writes fewer than three destination channels.
Structural, not heuristic: the channels are not there.

**`RPCS3_REMIX_FPALBEDOKILL` IS GONE FROM THE BUILD.** Not defaulted off — removed. The exe contains
neither the wide string `RPCS3_REMIX_FPALBEDOKILL` nor `tex_albedo_kill=%llu` nor `fpalbedokill=%d`, all
three verified absent. Do not look for it.

| offline over all 338 `.fp` files, **before the build** | KILL (reverted) | CHANNEL (armed) |
| --- | --- | --- |
| **elected unit CHANGES** | **36** | **3** |
| of those, electing unit >= 8 | **16** | **0** |
| units re-elected to | 1, 8, 10, 11, 13, 14, 15 | **all three are 0 -> 1** |
| `fp=65a91390aaf6bef3` | `0x1f` -> `0x16`, unit 0 -> 1 | **identical** |

Same answer on the one case ever verified, one twelfth of the blast radius.

**Two containments, both in `albedo_unit_mask()`.** The narrowed mask must name a unit the backend can
**bind**, and it must elect a **different** unit from the one taken anyway — otherwise `referenced` is
returned untouched. So the 175 programs that narrow without moving their election are bit-exact.
And `from_ucode` is **deliberately left false**, so the retry policy is byte-for-byte round 42's.

| reading on `Remix live:` / `Remix stats:` | meaning |
| --- | --- |
| `tex_albedo_narrow` > 0 | it moved that many draws. Subset of `tex_albedo_GUESS`, not of `ucode` |
| `tex_albedo_narrow = 0` | it moved nothing — check `fpalbedonarrow=1` on the banner before assuming it is broken |
| `Remix picked:` on a wall reads `albedo_unit=1` | the wall took its diffuse |
| `tex_none` / `notex_mat_applied` climbing hard | **FAIL** — surfaces losing their texture |

**PRE-REGISTERED REFUTATION:** full-screen noise like last time, or an obviously wrong image (a normal
map's blue, a lightmap), means the re-election is still wrong even at three programs. Report which
surface. **`set "RPCS3_REMIX_FPALBEDONARROW=0"`** — one line, round 42 bit-exactly.

### Why the last attempt painted the screen — and why the stated reason was probably wrong

`FPALBEDOKILL` changed **two** things under one knob: which unit is elected, and — via `from_ucode` —
whether the retry walk may run. The failure was attributed to the first. The evidence fits the second:

```
of the 36 kill-changed programs, over the 32,888-frame log:
  appear in any screen-space census ('Remix uiwrap:', 'Remix ui-biggest:') ....  0
  appear anywhere in the run, as world geometry ...............................  1
  never drew at all in that session .......................................... 35
```

**Caveat:** that log is the round-42 session, *before* the noise, so "never drew" transfers only weakly.
The **zero screen-space hits** is the stronger half. Meanwhile `from_ucode = true` unblocked the retry
guard at `RemixGSRender.cpp:19445` for all 81,091 resolved draws, letting them take the **unrestricted**
walk to the next referenced unit — the one the source says "painted character faces onto tree trunks" on
Resistance 2. That fits world-wide static much better.

**Both mechanisms are closed now.** Three programs instead of 36, and no `from_ucode` flip.

---

## PRIORITY 1 FOR ROUND 44 — THE BLACK VOID. ALL FOUR HYPOTHESES NOW REFUTED

- **(a) never submitted — REFUTED.** `world_refused = 248,109`, but the `Remix world-refused:` census
  names only **two distinct keys in the whole run**, both `vtx=4` full-screen quads, in a 128-line budget.
- **(b) submitted and hidden — REFUTED.** `cat_hidden = 134,023` is **entirely**
  `RPCS3_REMIX_CAT_HIDE`'s two deliberately-set hashes (launcher line 1163). `cat_decal = cat_particle =
  cat_sky = 0`. And `rtx.conf`'s category lists **never reach this path** — the backend reads
  `RPCS3_REMIX_CAT_*` (`RemixTransforms.cpp:6309-6320`), not the conf file. Untagging hashes from
  `rtx.decalTextures` therefore changed nothing and was never going to.
- **(c) degenerate transform — REFUTED as a cause.** Real jitter exists (per-frame origin jumps up to
  8.16 units on `anchor` references, exactly 0.0000 on `identity-bypass`) but `arch=fused` divides the
  vertices by the same reference, so the product is unchanged. It matters for motion vectors and mesh
  identity, not for a missing wall.
- **(d) submitted REFLECTED — REFUTED, this round, by the instrument shipped with it.**
  `xform_mirrored = 0`, and three fresh picks read `det = 1.00104`, `det = 1`, `det = 1`. No reflected
  instances exist. The reversed wall text in the capture has another cause — graffiti authored mirrored,
  or read through a transparent surface. **Do not re-run the culling toggle either**; `doubleSided` is 1
  on every instance unless `cull_from_rsx()` and `RPCS3_REMIX_CULL` is absent.

**Where round 44 starts.** Not from a new theory. The user has Ctrl+Clicked the black surfaces and those
picks now carry `det=`. Read, in this order:

1. `xform_measured` vs `submitted` — do the black-surface draws reach the transform path **at all**?
   `xform_degenerate` separates a collapsed frame from a healthy one.
2. The `Remix picked:` lines taken **on the black surfaces**: `albedo=`, `material=`, `albedo_unit=`,
   `alpha=`, `blend=`, `fp_out_rgb=`. A black polygon that IS submitted and IS textured is a shading
   question, not a geometry one — and the albedo work above is the first thing that could change it.
3. Only then consider lighting. `mat_emissive`, `guest_lights` and the sun path are all separately
   instrumented and none of them moved during the reported episodes.

---

## PRIORITY 2 — THE ADS COLLAPSE. ROOT-CAUSED, NOT FIXED. NOT THE COMPOSITOR'S BLEND

Round 42 named `compositor::blend()`'s missing additive path and its `alpha == 0xFF` `memcpy`. That code
is exactly as described. **It is not what costs the frame.** Three of round 42's supporting numbers do not
survive re-measurement — including that `ui_px = 4,480,204` came from a scratchpad file belonging to an
**earlier run**. See `docs\remix\KNOBS.md` "Round 43".

**MEASURED** (control windows 105–107 vs ADS 109–115, camera parked at the same `scene=[1786 -31.31 1227]`):

| per frame | control | ADS |
| --- | --- | --- |
| `ui_ndc` | 20.0 | **94.0** |
| `ui_forced` | 22.0 | **0.0** |
| `ui_px` | 24,811 | **1,383,930** (55.8x) |
| `ui` ms | 0.29 | **25.2** (87x) |
| `submitted`/frame | 56 | 48 — **the world is still submitted** |
| fps | 45.7 | **29.7** |

Vertex program **`2f64c2f8ffd6add1`** is not statically fingerprinted as screen-space, so
`is_screen_space_draw()` re-decides per draw. Normally the classifier **refuses** it and
`RPCS3_REMIX_UIFORCEVP` drags it in through the cheap **clip-pixel** branch (`RemixGSRender.cpp:12870`).
In ADS the same program **passes on its own**, `ui_forced` goes to 0, and it takes the **NDC** branch
(`:12859`), which scales every vertex to full compositor size at `:12864-12865`. Its 1280x720 sheet
`FF724C765485B42B` is then rasterised at exact full-screen NDC — `Remix ui-biggest:` reports
`area=861440px`, the **entire** compositor buffer, 63.3% of the cost.

**Two candidate fixes, neither a one-liner, neither shipped:** stop that program taking the NDC branch
when its post-matrix extent already covers the screen (the branch condition is an extent test under 1.5 at
`:12859`, and a full-screen quad is exactly what it should not admit); or make the raster loop not scalar
(`ui_px` is counted at `RemixCompositor.cpp:613`, the first statement of `blend()`, before both early-outs
— so the cost is in `sample_bgra` and the raster loop, not the blend arithmetic; row-parallel over
disjoint bands is safe, per-thread `uv_counters` and `m_pixels` are the only shared state).

**`RPCS3_REMIX_UIWIDTH` is a trade, not a defect fix, and the user has not chosen a value.** It is 1280
(launcher line 59) and the cost is quadratic: 960 → ADS `ui` ~14.2 ms (~44 fps), 640 → ~6.3 ms (~68 fps),
at the price of a softer HUD. Do not change it unilaterally.

---

## PRIORITY 3 — `6575ACE3A42A78E6` is an UNSAMPLED texture, not a rect-sizing bug

`f_200.jpg` shows **two solid filled yellow quads** and a solid white bar where the nectar HUD text should
be. The atlas is not being sampled at all. `RPCS3_REMIX_UIRECTSHRINK` addresses UV rect geometry and
cannot fix an unsampled texture; it is on that list at 50% and also carries a veto in `rtx.uiTextures`.
Look at whether the UI compositor resolves an albedo `entry` for it at `RemixGSRender.cpp:13610`
(`have_uv ? entry : nullptr`) — a null `entry` there rasterises the flat tint, which is exactly a solid
coloured quad.

## PRIORITY 4 — carried

- **Partial progress, reported by the user:** the teleporting beams are *"a bit more stable now but still
  move"*. Something in rounds 42–43 reduced it. Nothing was aimed at it deliberately, so **find out what**
  before assuming it generalises.
- **NPC geometry SMEARS, it does not merely wobble.** Between `f_030` and `f_031` the soldier is stretched
  into a streak with magenta and cyan patches. The colour corruption is a separate signal from the
  positional error. Haze skins on the SPU (`skin_submitted = 0` — no bone path runs at all), so vertices
  arrive already animated: a per-vertex path, not a bone palette.
- `E40BF80AF519848A` still teleporting; it carries a veto in `rtx.worldSpaceUiTextures`.
- Weapon wiggles when idle. Round 38's model predicts zero motion when still, so idle wiggle is a second
  mechanism; round 34's +/-0.28% anisotropic basis error is the standing candidate.
- **Emissive vs lit fixtures.** Untouched this round. `GUESTLIGHTAUTO=0` as the user set it; emissive is
  material-scoped so *lit-only* cannot be expressed through it. The swap needs round 41's glow-card light
  path fixed first (world-geometry gate, light position from the emitter not the billboard centroid).
- **The static-index cache is thrashing and nobody has explained it.** `entries=3296 rebuilds=11183
  dropped=69 peak=128 budget=128 resident=62 evicted=3234`. Round 42 refuted round 31's budget theory
  using `dropped`, the right counter for "draws that got no index" — but `evicted` is a different counter,
  and 3,234 of 3,296 entries have been evicted from a 128-slot cache. Not claimed as a defect; claimed as
  an unexplained number.
- `glauto=%d` still prints a boolean not the mode; `RPCS3_REMIX_EMISSIVEINT` is still not a real variable
  (the parser reads `RPCS3_REMIX_EMISSIVEINTENSITY`). Both carried from round 42, both harmless today.

## THE CHAPTER LIST

Untouched again. Nothing under `bin\patches\` was changed. Read `round42-inbox.md` "THE CHAPTER LIST"; the
untraced lead is still `0x4f4b40` and it needs the decrypted ELF regenerating.

---

## Tooling left behind

In `...\scratchpad\r43\`:

- **`chanrule.py`** — scores the channel-width rule against the liveness kill over `bin\remix_ucode\`.
  This is what turned "narrow the kill somehow" into "3 programs instead of 36, none above unit 1".
- **`killwalk.py`** — both colour_mask walks reimplemented, decode mirrored field-for-field from
  `RemixTransforms.cpp:8000-8095`. `chanrule.py` imports it. Run one of these before touching the albedo
  election again.
- **`fpdis.py`** — RSX fragment-program disassembler. Two traps: words are little-endian then
  16-bit-half-swapped, and in OPDEST **the opcode is at bits 24-29**, not 22-27.
- **`audit.py`** — bracket-balanced format-string auditor over `RemixGSRender.cpp`: 298 formatted calls,
  specifier count vs argument count. **Self-tested against a synthetic 3-vs-2 mismatch before being
  trusted.** Run it after any format edit in that file.

## Instrument notes worth keeping

- **One mechanism per knob.** `FPALBEDOKILL` changed the elected unit *and* unblocked the retry walk. The
  play-test could not tell them apart, and the post-mortem still cannot say for certain which one painted
  the screen. Two mechanisms behind one flag costs a whole round to disambiguate.
- **A norm cannot see a sign.** `basis=[1 1 1]` was read as "no transform anomaly" for four rounds while
  being mathematically incapable of representing a reflection. Adding `det=` refuted the hypothesis it was
  built for in a single run — a diagnostic that kills your own theory quickly is worth more than one that
  confirms it slowly.
- **Check which run a number came from.** Round 42's headline `ui_px` and its "24 draws" both came from a
  scratchpad file belonging to an earlier session.
- **A cumulative counter divided by frames is not a rate.** `m_stats` is never reset — only `m_timing` and
  `m_compositor.reset_pixels()` are.
- **Score a classifier change offline before building it.** `bin\remix_ucode\` holds the whole corpus; both
  of this round's rules were scored to an exact blast radius before a compiler ran, and that is the only
  reason the second attempt could be shown to be 12x narrower than the first without a play-test.
- **`bin\remix_dump.log` is ~1 GB and append-mode.** Find run boundaries with `grep -abo "Remix
  run-start:"` over a `dd`-skipped tail, slice once with `tail -c +N` into the scratchpad, then grep that.
- **`Remix timing:` and `Remix stats:` go to `bin\log\RPCS3.log` only.** `Remix live:` goes to both, which
  is why `tex_albedo_narrow` and `xform_mirrored` are on both lines.
- **Do not build a launcher dry-run by `head -N` past the `cd /d "%~dp0bin"` line** — the next line starts
  the game.
