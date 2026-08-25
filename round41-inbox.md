# Round 41 inbox — RPCS3 RTX Remix backend, Haze (BLUS30094)

Written at the close of round 40, 2026-08-25. Branch `remix-backend`, HEAD **`4fdaecd67`**, working tree
carries rounds 37-40 uncommitted.

| artefact | SHA256 (first 16) | note |
| --- | --- | --- |
| `bin\rpcs3.exe` | `46D3DF83376C0391` | built this round, MSBuild `Release\|x64` exit 0, **0 errors, 1 warning** |
| `bin\rpcs3-next.exe` | `46D3DF83376C0391` | **identical — the copy was made** |
| `bin\remix\d3d9.dll` | `16A0B512F33EBB66` | **UNCHANGED.** Nothing deployed into `bin\remix\`. |

The one warning is the pre-existing C4723 "potential divide by 0", now at `RemixGSRender.cpp:11269` (it was
`:11197`; it moved because this round inserted lines above it). **Zero new warnings.**

**The tree did NOT move this round.** HEAD was `4fdaecd67` at both ends and both exes read
`E4E2A9A4EDAECE2A` at the start, exactly as round 39 recorded. Hash both exes and `bin\remix\d3d9.dll` and
read `git log --oneline -1` at BOTH ends, and never run a git command that writes the working tree.

**Round 39's build got a real 30-minute play-test.** `bin\log\RPCS3.log` (44 MB, unlocked) and
`bin\remix_dump.log` (996 MB) hold a complete 64,139-flip / 15,526,016-draw session ending 09:38 today, with
14 Ctrl+Click picks. Everything round 40 claims is measured against that run. **Read the previous round's
logs before forming any hypothesis** — this is the third round where they already held the answer.

---

## PLAY-TEST CARD — read this in order

Five things changed. Four are launcher-only and each has a one-line revert. Full derivations are in
`docs\remix\KNOBS.md` "Round 40".

### 1. Lights. THE headline, and the one number that decides the round

MEASURED last run: **`guest_lights=0`** across 64,139 flips. Haze has had **no analytical lights at all** —
two independent mechanisms, both now fixed or exposed:

- `GUESTLIGHTALBEDO2` was **unreachable dead code**. The arming test read only the *primary*
  `GUESTLIGHTALBEDO` list, which was blanked on 2026-08-17 to remove the door lights. That one edit killed
  every guest light in the game, silently, and nobody connected the two. One-line source fix.
- `CAMLIGHT` has been **inert since round 5** and the launcher's comment saying otherwise was false. Proven
  against the deployed runtime's own tree: `DestroyLight` only *queues*, and the queue drains after the
  create, so the per-frame destroy-then-create erased the light it had just made. Now fixed (same-hash
  create, `isDynamic = 1`) and **armed at 0** so the newly working light stays off until asked for.

**READ FIRST: `guest_lights=` on `Remix live:` in `bin\remix_dump.log` (readable while the game runs).**

| reading | meaning |
| --- | --- |
| `guest_lights > 0` **and** `Remix guest-light:` lines appear | **PASS.** The bulbs now light the room. |
| `guest_light_match=0` and `guest_lights=0` | the (vp, fp, albedo) triple still misses — the fix landed but the key is wrong |
| `guest_light_match>0` and `guest_lights=0` | `CreateLight` refused; look at `guest_light_capped` |

Revert to round 39 exactly: `set "RPCS3_REMIX_GUESTLIGHTALBEDO="`.
To try the now-working camera fill: `RPCS3_REMIX_CAMLIGHT=2` or `3`. **Not 12** — at 12 it will now really
do what the source warns: *"blew out everything near the player and crushed everything far from them"*.

### 2. `SKIPVP=` blanked — STAGE 2, finally run. Watch for the wooden plank floor

`4D5A87BFFBCE0717` has been dropped **wholesale since before the forge record began**, and it is one of the
four programs the `WDIVWALK` decode fix repaired, so the giant-geometry reason for dropping it is long gone.
MEASURED that it eats **main-pass** geometry (`clip=1024x576`, `skip vp=24569` draws/run). This is the
strongest single candidate for *"just a black void floor"*.

**Watch:** anything that appears. Also `wext_refused=` and `wext_max=` on `Remix live:` — they read `0` and
`24906.5` before this change. **If ANYTHING renders at absurd scale, revert this line first.**
Revert: `set "RPCS3_REMIX_SKIPVP=4D5A87BFFBCE0717"`.

### 3. `SKYCLASSIFY=1` — round 39's classifier failed its own pre-registered check

It armed exactly **one** hash all run (`23A3978F1405B16E`), `listed=0`, `armed − promoted = 0` — which round
39 itself pre-registered as *"a reason to distrust it, not a pass"* — and it armed at
`cam=[1746.6 -71.676 1049]`, **inside the copper plant**. A promoted hash becomes emissive **and stops
occluding**, which is what "white" plus "vanishes at angles" looks like.

Honest caveat: the census argues it is a genuine backdrop (2048×512, raw box ±12370, 61/61 dome-shaped). At
mode 1 nothing is rebuilt and no pixel changes from the classifier.
**If the plant walls stop being white / stop vanishing, this was it. If unchanged, put it back to 2 and the
classifier is cleared.** Revert: `set "RPCS3_REMIX_SKYCLASSIFY=2"`.

### 4. `CAMTRACE=1` — the FOV has never once been measured

Five rounds of *"fov still isn't fixed"* and **zero `fov=` tokens exist anywhere in the last run's log**,
because the only line that prints it is behind this knob and it has been 0. One session, then put it back —
it writes one line per frame.
**Read:** `Remix camera trace: ... fov=` in `bin\remix_dump.log`. Compare against the 72.000 horizontal /
44.634 vertical the viewmodel code quotes. Revert: `set "RPCS3_REMIX_CAMTRACE=0"`.

### 5. THE ONE THING ONLY THE USER CAN DO: **Ctrl+Click a white wall, and a floor that vanishes**

All 14 existing picks landed on objects that resolved correctly (`material=1 albedo_unit=0`, sane UVs).
There is **no counter downstream of `CreateMaterial`**, so nothing in the instrumentation can separate "the
material is right and Remix renders it white anyway" from "the wrong texture is attached" from "the wall is
skipped and something else shows through". One pick on the actual white surface answers it directly and
unblocks the largest remaining defect. The pick line goes to `bin\remix_dump.log`, readable while playing.

### Also worth one straight-line walk (no rebuild, no knob)

Walk forward **without turning**, past a near prop and a far prop, and say whether they bounce.
- Round 38's rotation-only model predicts **no bounce at all**, and any bounce scaling with distance.
- Round 40's corrected model predicts **bounce on both**, amplitude ≈ your per-frame displacement,
  roughly **independent of distance**.

---

## What round 40 REFUTED. Do not re-chase these

- **The static-index budget is not the vanishing walls, and this contradicts rounds 34 and 38.** At
  `STATICINDEXBUDGET=128`: `peak=128 budget=128` — so round 38's pre-registered check failed and the budget
  *is* still reached — but `dropped=121` in **15,526,016 draws (0.00078%)** and `stale=1`. `peak` is a
  monotone high-water mark and can read `budget` after one frame in 64,139; the *cost* has its own
  cumulative counter and it is three orders of magnitude too small. `evicted=7248/7419 = 97.7%` reproduces
  round 38's number and does not matter: an evicted entry that is drawn again is simply rebuilt
  (`rebuilds=9657`), and only becomes the invisible `dropped` exit when the budget is *also* spent.
- **The texture path is not producing white.** Exactly **one** texture descriptor was ever refused in the
  whole run — `Remix texrefuse: format fmt=92 2048x2048`, the DEPTH16 shadow map — and
  `tex_unsupported 2,155,582 − tex_tombstone 2,155,581 = 1`. Every wall format (DXT1/23/45, A8R8G8B8,
  D8R8G8B8) is supported. A material-less draw gets **grey 0.5** (`NOTEXMAT=1`, `notex_mat_applied=2676` of
  2676 eligible), not white; 97.5% of `tex_none` is skipped outright.
- **`mat_untested` does not mean the peak-UV walk was skipped.** It counts materials created while the RSX
  alpha test was off. `peak_uv` has no counter at all.
- **`vmcam_considered=0` is expected, not a defect**, and **`VMCAMFOVX/Y` are live, not inert.** That
  counter lives behind `if (viewmodel_place)` and `VMTAGONLY=1` makes it unconditionally false — exactly as
  `RemixTransforms.h` pre-registers. The real counters are `vmcam_twin=29343 vmcam_real=29343` against
  `cam_resolved=29343`: a VIEW_MODEL camera reached the runtime on **every frame the world camera resolved**,
  100% carrying the armed `60.001 / 36.132`. They are simply not echoed on `Remix run-start:`.
- **`fpvcol_skygate=83772` is not the particles.** The gate needs `!material` and `sampled_mask == 0`; both
  picked particles have `material=1`. It is the 82-vertex sky dome, 1.31 per frame.
- **The additive blend translation is fine.** `6/1/0` (SRC_ALPHA/ONE) = 113,511 draws, verdict `kept`, and
  the four `blend_pairs` entries sum to `blend_translucent` exactly.
- **`isDynamic=0` on the guest fixture lights is harmless.** They are created once and never updated, so
  `addExternalLight` takes the emplace arm and `updateLightStaticSleep` is never called for them. Adding the
  flag there would have been a no-op dressed as a fix. It IS required on any light re-created per frame.

---

## PRIORITY 1 for round 41 — widen `scan_fragment_program`. It unlocks the walls AND the particles

This is the biggest single item and it is now fully diagnosed.

**MEASURED: every one of the 192 `Remix alphastate:` rows in the run reads `tcolor=1/0/3`** —
`Arg1=Texture, Arg2=None, Modulate`, i.e. *"colour = albedo texture, vertex colour discarded"* — and
`vcol_applied=93,778` of 6,390,568 gauge-placed draws is **1.47%**.

Meanwhile Haze's geometry **has** a replayable vertex colour. Of 47 programs in `Remix vcolroute:`,
**19 are `route=scaled` and 3 `route=passthrough`**, including the world/wall program itself:

```
Remix vcolroute: vp=ad7ce9d672a0bf6b route=scaled alpha_from_attr=1 slots=[c[18].x;c[18].y;c[18].z;c[18].w]
```

and the bulb program, and `f39f5046`, `bd1c10df`, `d0b6a471`, `af06f6d3`, `a7505f7a`, `c2003391`.

**The binding gate is the FRAGMENT classifier.** `fp_wants_vcol` requires
`m_current_fp_fingerprint->vcol_replayable()`, true only for `MOV out, COL0` (1 instruction) and
`TEX; MUL out, tex, COL0` (2 instructions). MEASURED `fpclass=1/1/0` — **two programs in the entire
title**, and `fpvcol_applied=698` of 5,783,237 submitted draws (0.012%). The additive particle's own
fragment ucode is on disk at `bin\remix_ucode\E07AC460430042B6.fp`, **256 bytes = 16 instructions**; the
classifier can only ever match 1- and 2-instruction shapes.

If Haze authors a neutral base texture and carries the copper tint per-vertex — which is exactly what
`route=scaled alpha_from_attr=1` with a per-draw `c[18]` factor looks like — then *"copper plant walls white
texture not albedo"* **is** this, and it is the same root cause as the uncoloured lava smoke.

**No zero-code lever exists.** `VCOLMOD=1` alone cannot help (`fp_wants_vcol` ANDs in the fragment gate);
`FPVCOL=0` moves the wrong way; `FPVCOLROUTE` is the *vertex* route gate, a different predicate.

### Second half of the same job: `scan_vcol_route`'s whole-register mask precondition

All six particle vertex programs decode to

```
MUL rN.xyzw = ATTR3, c[467].xxxx
MUL o1.xyz  = rN.xyz, c[66].xxxx      ; COL0.rgb = ATTR3.rgb x c[467].x x c[66].x
MOV o1.w    = <computed fade ramp>    ; COL0.w is NOT ATTR3.w
```

and `scan_vcol_route` bails at `if (vec_writemask(in) != 0xf || in.d3.index_const)` where `in` is the
**last** instruction writing `o1` — the `MOV o1.w`, mask `0x8`. The register is rejected wholesale because
two instructions write different lanes. **The code's own comment names the blast radius: *"29 of the 82
cached programs land here (masks 0x7 and 0x8)."*** And `vcol_lane_source` — a per-lane backward walker over
MOV/MUL chains through temps, ≤3 hops, constant co-operands, i.e. exactly this shape — sits **22 lines
below** the bail-out and is never reached. It would classify the rgb half as `scaled_chain` with
`slots=[c[66].x|c[467].x]`.

**So the hue really is per-vertex, in ATTR3.** Both constants are scalar broadcasts; they can only scale
brightness, not tint.

**LATENT BUG — fix it in the same change, not after.** `apply_vertex_alpha` writes
`color = 0x00FFFFFFu | (alpha << 24)`, forcing RGB to **white**, and it runs *after* the
`material && fp_wants_vcol` call to `apply_vertex_colour` as an independent `if`, not an `else`. The moment
these gates open, any textured draw with a constant-alpha albedo will have its freshly replayed RGB
overwritten white by the alpha rescue. It is harmless today only because the RGB is already white.

**Two zero-code separating tests, and do NOT run them in the same session as this round's A/B:**
- `RPCS3_REMIX_FPVCOL=0` → **predicted pixel-identical** (698 of 5.78 M draws, none a particle). Any visible
  change refutes the fragment-gate diagnosis.
- `RPCS3_REMIX_FPVCOLROUTE=0` → makes `vcol_route_replayable()` unconditionally true. Then Ctrl+Click a
  smoke draw and read `tcolor=`. Still `1/0/3` ⇒ the route gate is not binding and the fragment gate alone
  is.

---

## PRIORITY 2 — the one-frame donor lag, and why round 38's lever is NOT free

**The bounce is real and measured.** 180 of 180 `Remix pick-follow:` samples of the first-person arms
(`vp=830d7d1b9681c475 albedo=86885A0E60751491`, vtx=3649) read `ref=anchor_prev`,
`frame − anchor_frame = 1`, `frame − cam_frame = 0`, `zfold=1`. Run-wide `gauge_prev=1,029,650` with
`gauge_prev_camfresh=1,013,828` = **98.46%** having a current-frame camera. The four gauge branches partition
exactly: `4,566,060 + 396,132 + 1,029,650 + 398,726 = 6,390,568 = world_ref_fresh + world_ref_stale`, so the
stale branch is **16.11%** of gauge-placed draws (*not* 22.6% of `gauge_used`, which is only the first
branch).

**Round 38's model is missing a term.** It says the residual is *"rotated about the eye by one frame of
camera turn, so displacement = (distance from eye) × (turn per frame)"* — which predicts **no bounce when
walking straight**, the very case the user reports. The composite error is
`V(t)P(t)P(t−1)⁻¹V(t−1)⁻¹` = with constant projection `V(t)V(t−1)⁻¹`, **a rigid transform with a rotation
part AND a translation part**. The translation part is one frame of camera translation. Measured jitter on
the arms over 4000 frames: `p50=0.00284 p95=0.07428 p99=0.10012` — 10 cm every frame at p99.

**Round 38's "needs no new machinery — only a third branch here" is REFUTED.** `divide_anchor` is the
**only** carrier of the f64 inverse (`if (divide_anchor && divide_anchor->inverse_f64_valid && gauge_f64_enabled())`),
and `camera_state::reference_inverse` is `remix_rsx::mat4` — **f32, with no f64 twin**. Taking the camera
reference drops ~1.01 M draws/run out of the f64 path (`world_div_f64=5,991,842` today) into the f32 one,
whose own in-tree measurement is *"~1.0 of translation error"* on an ill-conditioned anchor. Trading a
measured **0.10-unit** lag for a possible **1.0-unit** precision error is a regression.

**The honest version: give `camera_state` an f64 reference inverse computed at latch time, then add the
third branch.** Seven sites assign `reference_inverse` — `RemixGSRender.cpp:2693, 2978, 3440, 15946, 16191,
16238, 16486`. This is the one change that fixes "props bounce" and "viewmodel does not track" together.

Independent second viewmodel defect, untouched: `zfold=1` on **180/180** arm draws — the anchor folded
`0.49875/0.50125` while the arms render at `0.00125/0.00125`. Run-wide `gauge_zfold_mismatch=492,957`
= 7.69/frame. Refolding with the anchor's own registers is staged in a comment already.

---

## PRIORITY 3 — parallel lists for `GUESTLIGHTVP`/`GUESTLIGHTFP`

The vp/fp narrowing is a single **global** pair ANDed against **both** albedo rules, so only one fixture
family can be lit per run. Round 40 pointed it at the bulbs; the floor-recessed fittings the user asked
about cannot be lit at the same time. The right shape is parallel comma lists (the `UIFORCEPAIRVP2`/`FP2`
idiom, entry *i* of VP paired with entry *i* of FP).

**Do NOT just blank them.** `71D189E9B559A7F9` is bound by 8 distinct (vp, fp) pairs in the log, one of
which is the smoke program `ad7ce9d6/aa0fe222` — blanking would put a sphere light inside every smoke puff,
which is the same failure the launcher's 2026-08-15 door note records.

---

## Smaller items, carried

- **`GUESTLIGHTRADIUSSCALE=0` is silently `0.35`.** `env_float` rejects 0, and the accessor's own comment
  ("0 falls back here and the caller then takes `max(fixed, 0)` = the fixed radius") is **wrong** — the
  fallback is 0.35, so the radius is `max(0.6, extent × 0.35)`. The live line echoes `glradiusscale=0.35`.
  For the bulb (extent 1.731) that is 0.606 vs 0.600 and does not matter; for a 6-unit fixture it would be
  2.1. Left alone this round so it cannot confound the first run in which guest lights exist at all.
- **`CAMLIGHT=0` is only "off" because the config agrees.** `env_float` rejects 0 → falls through to
  `g_cfg.video.remix.camera_light`, which reads `Camera Fill Light: 0` in **both** `bin\config\config.yml`
  and `custom_configs\config_BLUS30094.yml` (verified). If either config ever gains a non-zero value, `0` in
  the launcher will silently turn the light **on**.
- **Three knobs are armed to their own shipped default and therefore do nothing:** `NOTEXMAT=1` (default 1),
  `NOTEXCENSUS=1` (default 1), `TEXVERIFY=120` (default 120).
- **Four knobs are read but never echoed on `Remix run-start:`** — `VMCAMFOVX`, `VMCAMFOVY`,
  `VIEWMODELANCHOR`, `VMPAIRMAXDIST`. That gap is why five rounds could not tell whether the viewmodel FOV
  was live. A four-field addition to that format string closes it. `VIEWMODELANCHOR` is the worst of the
  four: it is `env_float` with a default of **4.f** and is armed at **4**, so arming it is indistinguishable
  from not arming it, and `VIEWMODELANCHOR=0` would silently read back 4.
- **`rtx.fallbackLightMode` is still 0** in `bin\rtx.conf`; round 33 asked for 2. That file is the user's.
  It matters more now that the scene is known to have almost no lights.
- **Fog**: round 39's `SetConfigVariable` probe **PASSED** — `Remix: SetConfigVariable probe returned
  GENERAL_FAILURE (1)` in `bin\log\RPCS3.log`, which is the expected result and means `rtx.volumetrics.*`
  really is settable at runtime from this client. Nothing was built on it yet.
- **Helmet**: `uiforcepairs=3` confirmed in `bin\log\RPCS3.log`. The user has asked seven times; it is
  landing.
- **`GAUGECURDIMS` is alive**: `gauge_cur_dims=396,132` of `gauge_cur_avail=396,132` — it fired on **100% of
  what was available**. Round 38's "structurally dead on this title" worry is closed.
- **Frame time**: still not measured, four rounds running.

## Instrument notes worth keeping

- **`wdiv=` and `wdiv_shadowed=` are printed inside the `skip …` group of `Remix stats:` and are NOT
  skips.** They are `m_stats.wdiv_draws` / `wdiv_shadowed_draws`, positive decode counters.
  `wdiv=12,934,988` read as a refusal is a 12.9 M-draw phantom.
- **A monotone high-water mark cannot price a per-frame budget.** `peak=budget` and `dropped=121` are both
  true; only the second is a cost. When a counter is a `std::max` over the run, find the cumulative twin.
- **A cheap "is the feature armed" guard that reads only one of the feature's two inputs is the shape of
  this round's headline bug.** When a rule grows a second input, grep the *arming* test, not just the
  matching test.
- **`isDynamic` matters for UPDATED lights and not for CREATE-ONCE ones; `DestroyLight` matters for
  same-frame re-create and not otherwise.** Both were assumed uniform across the three light producers in
  one file and neither is.
- **`Remix live:` goes to `bin\remix_dump.log` and is readable while the game runs; `Remix stats:` goes only
  to `bin\log\RPCS3.log`, which is exclusively locked until the emulator exits.** `vmcam_twin`/`vmcam_real`
  exist only on the live line, which is why the stats line could never answer "did the viewmodel camera
  submit?".
- **`bin\remix_dump.log` is ~1 GB and append-mode.** Never grep it whole: `tail -c 200000000 <file> | grep
  -o "<pattern>" | tail -N`, shrinking the window until output fits, then take the LAST lines.
