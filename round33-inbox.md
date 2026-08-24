# Round 33 inbox — RPCS3 RTX Remix backend, Haze (BLUS30094)

Written at the close of round 32, 2026-08-17. Branch `remix-backend`, working tree dirty (round 32 is
**not committed**; HEAD is still `1ebff4c57`).

Deployed for play-test:

| artefact | hash (SHA256, first 16) | note |
| --- | --- | --- |
| `bin\rpcs3.exe` | `8BA42ED85B410240` | built this round, MSBuild exit 0, 0 `error C` |
| `bin\rpcs3-next.exe` | `8BA42ED85B410240` | **identical to rpcs3.exe — the copy was made** |
| `bin\remix\d3d9.dll` | `16A0B512F33EBB66` | **UNCHANGED.** Never deployed from the fork. |

Format-string edits verified in the binary by scanning for the NEW field text present **and** the OLD
field text absent: `decode=%.2f` / `audit=%.2f` / `hash=%.2f` / `xform=%.2f` /
`scene=[%.4g %.4g %.4g] meshes=%llu` / `resident=%llu evicted=%llu nomesh=%llu` /
`defer_absent_declined=%llu` all present; `draw_instance=%.2f rest=%.2f`, `peak=%u budget=%u"` and
`defer_skinned=%llu nectar_published=%llu` all **gone**. New knobs confirmed as UTF-16 strings
(`DRAWAUDIT`, `DEFERPREVONLY`) with `GAUGEANCHOR` as the known-good control.

---

## Read `docs\remix\KNOBS.md` "Round 32" section first. It closes four leads. Do not re-open them.

1. **`anchor_frame == frame - 1` on a `ref=anchor_prev` pick is a tautology**, not an off-by-one:
   `find_gauge_anchor(false, ...)` accepts *only* `entry.frame + 1 == m_frame_counter`.
2. **The wobble fix exists and is switched off on purpose.** `DEFERPREANCHOR=0`, launcher note dated
   2026-08-16: props stop sliding, costs ~14 fps, `defer_flip` was 72% of `defer_buffered`.
3. **`camanchor == cam` bit-for-bit on a pick line is AMBIGUOUS** — the field is seeded from `cam` and
   only overwritten if `anchor_frame_eye()` succeeds. Only a difference (even 1e-4) proves a read.
4. **`Remix worldid-census: tmax`** cannot vindicate an anchor donor: it divides by that donor's own
   inverse, so identity is guaranteed by construction.

---

## Priority 1 — the `rest` split lands this round. Read it before anything else.

`rest` was **40.96 ms/frame = 80.5% of `draw` and 77% of the whole frame** in round 31's worst
geometry window. Four new children now split it. **One filtered grep of `bin\log\RPCS3.log` for
`Remix timing:` answers the whole of round 32's item 4.**

**Pre-registered readings — record the actual value even where it disagrees:**

- If `audit` is the largest new child: the fix is `RPCS3_REMIX_DRAWAUDIT=0`, not an optimisation. It
  is pure diagnostic and `wext_refused=0` all session, so it is free to lose *on this title*.
- If `hash` is the largest: the union re-hash is confirmed as the cost (the source already nominated
  it) and the next step is a cheaper union key — it currently hashes 3 floats + colour per vertex
  **plus one hash per index**, per draw.
- If `decode` is the largest: vertex/index decode dominates and the answer is caching decoded
  positions per (source pointer, count), not a micro-optimisation.
- If `xform` is the largest: cost is **per-draw, not per-vertex** — that separates it from the other
  three in one run and points at `per_draw_transform`'s `mat4_invert`.
- **If `rest` is STILL the largest after all four:** the remaining untimed candidates, in the order
  the recon ranked them, are the vertex post-process family (`apply_vertex_colour` /
  `apply_vertex_alpha` / `apply_haze_fade` / `apply_particle_billboards`, each a full vertex sweep
  with a `decode_position` per vertex), the instance-signature hash block, and the per-draw census
  string formatting.
- **REFUTATION CONDITION:** `rest=0.00` beside a large `draw=` is **not** success — it is the
  saturation tell and means one of the nine children is double-counting. Say so if you see it.

**Two arithmetic traps this round proved, do not re-derive:**

- **`other = frame_ms − flip`, so `other` CONTAINS `draw`.** Never add them.
  `frame_ms = flip + other`; `draw ⊂ other`;
  `draw = ui + mesh_create + tex_bind + uv + draw_instance + decode + audit + hash + xform + rest`.
- **`ui_px/frame` is not `ui=`'s pixel count.** One counter is shared by `composite_ui_draw` (timed
  as `ui`, in `draw`) and `composite_native_overlay` (timed as `overlay`, in `flip`). A real log line
  reads `draw=0.00 (ui=0.00 …)` with `ui_px/frame=2065645`. **Never divide `ui_px` by `ui`.**
  If you want that ratio, split `m_pixels` along the `overlay`/`ui` boundary first.

**And the quarry question round 32 could not answer.** The brief asked whether the quarry's ~20 fps is
the `rest` kind or the `ui_px` kind. It could not be settled from round 31's log because **the three
largest and three smallest `frame_ms` samples are all boot/menu windows** (`draw=0.00`,
`overlay`≈26 ms, `ui_px`≈2M) and nothing on the line said so. `scene=[x y z]` and `meshes=` now do.
**Filter the new log on `scene=` to separate the quarry from the carrier from the plant, then answer
it.** The three worst *geometry* windows in round 31 all read `ui_px/frame` ≈ 141–147k (normal), so
the working hypothesis is `rest`, not UI — but state it from the new field, not from this note.

---

## Priority 2 — the static-index budget sweep, now that both axes exist

`Remix static-index:` gains `resident=` / `evicted=` / `nomesh=`, which sum to `entries=`.

Round-31 baseline (MEASURED, final line of the newest session):
`entries=470 triangles=171236 rebuilds=1728 deferred=2020 stale=191 dropped=1829 peak=8 budget=8`.
`dropped` is **90.5%** of deferrals — the never-submitted exit, i.e. the plant's missing ceiling.
`peak == budget` on every line = saturated.

**Pre-registered reading for a `STATICINDEXBUDGET=16` run:**

- `dropped` per rebuild must fall. `deferred/rebuilds` was **1.17**; at 16 it should approach 0.
- `evicted + nomesh` should predict `dropped`. If `dropped` falls while `evicted` **rises**, the
  budget is now churning BLASes faster than `MESHIDLE=600` reaps them and 16 is a regression — that is
  the exact cost term round 31 was right to refuse to guess at.
- `resident` is what BLAS memory pays for. Watch it against `mesh_live` (round-31 baseline
  `mesh_live=16335 mesh_created=129646 mesh_destroyed=113311`, `MESHCAP=0` so there is no ceiling).
- **Counters are cumulative and never reset per window** — difference two lines from *one* run. Raw
  values across runs of different length are meaningless.
- Judge it against `frame_ms` and the new `hash=` child, not against how aligned one room looks.

**Do not build a test on "looks aligned while paused."** Round 31 established the convergence
direction is counter-intuitive: the budget resets only when `m_frame_counter` changes, so if flips
genuinely stop the deferral **freezes**. What converges it is the *geometry* stopping while flips
continue.

---

## Priority 3 — the X-only anchor drift (`bd1c10df5703e559`). Root-caused, NOT fixed.

MEASURED this round: `anchor_cam_offset=4255` flips, `anchor_cam_offmax=430.126` units, and ~99.7% of
flips in one 1809-flip stretch carried a >1-unit disagreement. All 121 over-one-unit
`Remix anchor-gauge:` lines in a 125 MB window carry the same pair
(`anchor_vp=bd1c10df5703e559`, `elected_vp=7f3d3abcefc8b057`). `anchorcam` X ramps monotonically
−217 → −384, **wraps to +411**, then decays toward the elected value; `electedcam` X moves 25.87 →
34.66 over the same 1080 frames (a player walking). Y and Z agree to six digits.

The *following* session read `anchor_cam_offset=0 anchor_cam_offmax=0`, so this is **level-scoped**.

**Why nothing catches it:** `ANCHORSTICKY` compares *consecutive* frames
(`matrix_relative_delta(fused, slot->fused) <= s_camera_discontinuity_tolerance`), and a slow
monotonic drift stays under the tolerance every single frame, forever. Confirmed:
`gauge_contested=0`, `anchor_parked=3`.

**The cheap shippable next step, deliberately left for round 33 because it changes placement:** both
eyes are already in hand at one place — `apply_gauge_anchor_camera` computes the anchor eye and the
census already subtracts `m_frame_candidate.position` from it. Add
`RPCS3_REMIX_ANCHOREYEMAX=<units>` (default 0 = off, measure-only) that **invalidates the anchor slot**
when the offset exceeds the limit, counting `anchor_eye_rejected`. Draws then fall to
`gauge_absent` → the elected camera's `reference_inverse`, which is the frame the submitted camera is
in, i.e. self-consistent. **Pre-register the refutation:** if invalidating the slot makes the scene
*worse*, the anchor's frame is the correct one and the ELECTED camera is what should be rejected —
in which case the polarity question this project has deferred since round 29 finally has to be
answered, and `gauge_cam` firing on only **7447 of 18451 flips (40.4%)** is the number to attack.

**Also unresolved and worth one grep:** `apply_gauge_anchor_camera` was **silent for the whole
back half of the session that produced the picks** — the last `Remix anchor-gauge:` line is at
frame 18417 while picks continue to frame 30902. Find out whether that is the per-window budget
(`anchorgaugecensus=32`, key-deduplicated) or an early return, because if it is an early return then
the camera is not being put in the anchor frame at all on the levels where the wobble is reported.

---

## Priority 4 — the sun, second half

The clamp is fixed (`SUNSPRITEHOLD` ceiling 3600 → 216000), so the 72-second expiry is gone. Two
things remain, both MEASURED:

1. **The sun is 42.48 degrees wrong from boot until the player looks up**, because the sprite is the
   only live rung on the carrier (`SUNTRACK=0`; `sun_sky_examined=0` on all 172 stats lines; zero
   `Remix sunmap:` lines) and the electing draw does not exist until the sun is fully inside NDC
   (`k_edge = 1.f - 1e-4f`; measured `hi[1]` 1.1697 → 0.9463 across the offscreen→solved flip).
   Zero-code mitigation: `RPCS3_REMIX_SUNDIR=0.1223,-0.9444,-0.3051` (the sprite-derived travel).
   Proper fix: give this level a `SUNMAP` entry, which is file-derived ground truth and outranks the
   sprite. The PBCK `k_scene_sun` block is the source.
2. **INFERRED, needs one run to confirm:** RPCS3's light `0x4` *cannot* re-elect to `SUNDIR`
   mid-mission (`update_sun_light()` returns early when no rung is live and the light holds its last
   aim), so a sun that visibly reverts is more likely the **runtime's own** fallback distant light.
   `bin\rtx.conf` reads `rtx.fallbackLightMode = 1` (NoLightsPresent), re-evaluated every frame in
   the fork's `prepareSceneData`, and the distant fallback is not recreated per frame. **Ask the user
   to set `rtx.fallbackLightMode = 0`** — it is a one-line conf change, no rebuild, and it eliminates
   the candidate. Not done here: `bin\rtx.conf` is the user's live tagging state and is read-only to
   this round.

**Project-rule violation to fix cheaply:** `sun_sky_examined` / `sun_sky_solved` / `sun_sky_refused` /
`sun_sky_slots` are the **only** sun counters that live exclusively on `Remix stats:`, i.e. in
`bin\log\RPCS3.log`, which is exclusively locked while the game runs. They are exactly the counters
that prove two rungs of the precedence chain are dead. Mirror them onto `Remix live:`.

---

## Priority 5 — the NPC head, and the ceiling. Both still open.

**The head was NOT settled this round.** What is known: the six `STATICINDEXVP` programs are
identical to the six `WORLDIDENTITYVP` programs
(`0214281B9A7A412D,24D1BA819F701E47,BD1C10DF5703E559,7F02E76D7369D09E,AD7CE9D672A0BF6B,33AE0895AE9FEF72`),
and **neither VP from the seven wobble picks is in that list** (`f39f504649b6f442`,
`15ad612980aca110`) — so the wobbling props are ordinary gauge-divided draws, not static-index draws.
The head has not been picked, so its VP is unknown.

**Get the user to Ctrl+Click the smeared face directly.** That is the one measurement that answers it,
and `Remix picked:` writes to `remix_dump.log`, which is readable while the game runs. Then check
whether that VP is in `STATICINDEXVP` and whether `nomesh` accounts for its draws — round 31's own
mechanism predicts a second material mints a fresh entry with `mesh_hash = 0`, which is now countable
as `nomesh` rather than merely inferable.

**Do NOT propose `Use RSX Backface Culling` again.** The user tried it; it made the plant far worse
(walls/floor went black). Verified back to `false` in both `bin\config\config.yml` and
`custom_configs\config_BLUS30094.yml`. Single-sided submission drops the inward-facing faces this
title needs.

---

## Priority 6 — one-line fix left on the table

`SKYANCHOR` is the only one of the five distance gates still comparing the **transform's translation
column** (`transform.matrix[i][3]`) against the eye, while a correct geometric `centre` is computed
**twelve lines above in the same loop** and used only for the `inside` box test. Against a threshold
of 4, an absolute-world draw with a correct identity transform reads ~2100 units, so the gate is dead
for that whole class. Changing the gate to read `centre` puts it on the same footing as the other
four. **Ship it behind a knob defaulting to OFF with a counter** — SKY hides the instance from the
world pass, so a change here can make geometry vanish, and `sky_refused_anchor` /
`sky_refused_anchor_held` currently print on `Remix stats:` only (not readable live). Move them too.

**Separate live bug in the same audit:** `env_float` ends
`return (std::isfinite(parsed) && parsed > 0.f) ? parsed : fallback;`, so **`SKYANCHOR=0` and
`VIEWMODELANCHOR=0` do not disable those gates — they silently yield 4.** The source comment and the
older `SKYANCHOR` row in KNOBS.md both claim otherwise and are wrong against current bytes. Same
filter affects `SUNSPRITEMAXSPAN`, `SUNCARDINT`, `SUNRADIANCE`, `SUNANGLE`.

---

## Method notes earned this round

- **Grep a new knob's clamp against BOTH its default and the value the launcher arms.** Round 31 hit
  ceiling == default (`STATICINDEXBUDGET`); round 32 hit armed-value 277× above ceiling
  (`SUNSPRITEHOLD`). The instrument reported the clamped value on `Remix live:` in both cases and
  nobody read it. **Diff the launcher's `set` values against the `knobs:` block on `Remix live:` — it
  is one pass and it would have caught both.**
- **A field seeded from another field cannot prove agreement.** `camanchor` is seeded from `cam`. Any
  diagnostic written as "seed, then overwrite if available" needs a validity flag printed beside it.
- **A self-referential test proves nothing.** `worldid-census tmax` divides by the anchor's own
  inverse.
- **`remix_dump.log` is 767 MB and append-mode across sessions**, and **session boundaries are not
  marked**. Frame numbers restart, so a tail window can hold two runs; correlate a pick with the
  *nearest following* `Remix live:` line rather than the last one in the file. Stream it with a
  `FileStream` opened `FileShare.ReadWrite` and seek to `Length - N` — never read it whole.
- Per-window census budgets (`anchorgaugecensus=32` and friends) mean **a census going quiet is not
  evidence the condition stopped**. Check the budget before reading silence as a result.
