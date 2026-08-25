# Round 40 inbox — RPCS3 RTX Remix backend, Haze (BLUS30094)

Written at the close of round 39, 2026-08-24. Branch `remix-backend`, HEAD **`4fdaecd67`**, working tree
carries round 39 uncommitted.

| artefact | SHA256 (first 16) | note |
| --- | --- | --- |
| `bin\rpcs3.exe` | `E4E2A9A4EDAECE2A` | built this round, MSBuild exit 0, **0 errors, 1 warning** |
| `bin\rpcs3-next.exe` | `E4E2A9A4EDAECE2A` | **identical — the copy was made** |
| `bin\remix\d3d9.dll` | `16A0B512F33EBB66` | **UNCHANGED.** Nothing deployed into `bin\remix\`. |

The one warning is the pre-existing C4723 "potential divide by 0", now at `RemixGSRender.cpp:11197` (it was
`:11067`; it moved because this round inserted lines above it). **Zero new warnings.**

## THE TREE MOVED. First time the concurrency trap has fired — read this before anything else

At the start of round 39 HEAD was `44276ae06` with rounds 37–38 uncommitted and both exes at
`447D3904D1E969B2`, exactly as round 39's brief described. By the first build HEAD read **`4fdaecd67`**
*"Remix: unify viewmodel/prop drift as a one-frame donor lag"* — **another session committed rounds 37–38
mid-round**, including `KNOBS.md`, `launch-haze-remix.cmd` and both inbox files.

**Nothing was lost.** A commit moves HEAD and the index, not the working tree, and round 39 ran no
`checkout`/`restore`/`stash`/`reset`. Verified after the fact: `git diff --stat` against the new HEAD is
exactly round 39's own additions, and the launcher still carries round 38's armed values (`GAUGECURDIMS=1`,
`STATICINDEXBUDGET=128`, three `UIFORCEPAIRVP2` slots).

**The rule that made this survivable, and it is the rule to keep:** hash `bin\rpcs3.exe`,
`bin\rpcs3-next.exe` and `bin\remix\d3d9.dll` and read `git log --oneline -1` at BOTH ends of the round, and
never run a git command that writes the working tree. Round 39 also discovered a second symptom worth
knowing — `guarded_set_config_variable` already existed at HEAD, added by some earlier round and never
called, so a "new" helper you are about to add may already be there. **Grep before adding a wrapper.**

---

## PRIORITY 1 — read the sky classifier's verdict, in this exact order

`RPCS3_REMIX_SKYCLASSIFY=2` is armed. It identifies sky domes from geometry and **promotes** their albedo
hash into the sky-emissive set at runtime, which is equivalent to the user having Ctrl+clicked the sky and
pasted the hash into `RPCS3_REMIX_SKYEMISSIVE`. Full derivation is in `docs\remix\KNOBS.md` "Round 39"; the
measured thresholds are on `sky_classify_mode()` in `RemixTransforms.h`.

**1. The ground-truth check, and if it fails nothing else on the line is worth reading.**
`Remix skyclassify:` in `bin\remix_dump.log` must carry `D1A6D1B27ADE6232` and `CDFE11B12552EA2D` as
**`ARMED:listed`**. Those two are hashes the user identified by clicking the sky in game. The *older*
`SKYHASH` rule rejects **both** — MEASURED `upv=86.68` and `73.10` against its floor of 100 — which is the
whole reason round 39 built a second rule. If they do not arm here either, `SKYCLASSIFYUPV=60` is still too
high; drop it toward 50 and say so. **Replaying every gate over every `Remix sky-census:` row in the log
before shipping, all six hand-listed hashes are admitted**, so this check passing is the expected result and
this check *failing* would mean something changed in the live path that the historical rows do not capture.

**2. Ravine must arm nothing.** `haze_domes.csv` records all six `jungle_ravine_stream/*` backgrounds as
`(no skyModel authored)`. **A BLACK SKY IN RAVINE IS CORRECT.** Any `Remix skyclassify:` line emitted while
in Ravine is a false positive and it is the cheapest place in the game to catch one.

**3. Count.** `skyclassify_armed` on `Remix live:` should land in **8..20** over a full single-player pass.
MEASURED from `haze_domes.csv`: **16** distinct dome resources exist game-wide but only **12** are reachable
outside multiplayer (`mp_caves_sky`, `mp_mcv_sky`, `mp_pow_sky`, `mp_shanty_sky` are MP-only), and a dome
resource may bind more than one texture. Above ~28 the rule is over-matching; `SKYCLASSIFYUPV` is the nominal lever, but
**note its measured gap is 3.1× (23.57 / 73.09) and it is nearly redundant with the vertex ceiling** — every
census row the ceiling excludes is also below 60 — so if the rule over-matches, look at the extent ceiling and
`SKYCLASSIFYMINVTX` first. Replaying every gate over the log admits 18 distinct hashes; expect the live number
to be lower, because the census cannot evaluate the 8-draw / no-disqualification / settle rule.

**4. Did the promotion reach the material?** FIRST check `bin\log\RPCS3.log` for `Remix skypromote:` —
it must read `mat AAAA -> BBBB ok`, never `IDENTICAL - THE FOLD DID NOT FIRE`. Then `skyclassify_entries` > 0. **`skyclassify_promoted > 0` with
`skyclassify_entries == 0` means the rebuild found no resident texture and the dome stays black** — that is
a different failure from the classifier not matching, and it points at `promote_sky_emissive`'s entry walk,
not at the thresholds.

**4b. Was a promotion refused?** `skyclassify_failed` > 0 means every resident entry refused the material
rebuild — the promotion was withdrawn and will retry. `skyclassify_overflow` > 0 means the 64-entry
promoted set is full, which would mean the title has more dome textures than the array holds; raise the
array, not a threshold. Both should be **0**.

**5. The distrust check.** `skyclassify_armed − skyclassify_promoted` is the count of hashes the rule agreed
with the user about. **If that difference is 0, the rule found nothing the user had already found by hand**
— treat that as a reason to distrust it, not as a pass.

**Blast radius:** a promoted hash becomes emissive *and* stops occluding (BlendType::kEmissive). A wall,
water plane or fog card that starts glowing and stops casting shadow is this knob. `RPCS3_REMIX_SKYCLASSIFYMINVTX=16`
is a second gate added late in the round after replaying the rule over the log named a concrete false
positive — `AC936E2F25F147B0` on `vp=3c9186d8e026cec5`, a **four-vertex** quad spanning 32,331 units with the
camera inside it. If a flat card still glows, raise that floor before touching anything else.
`RPCS3_REMIX_SKYCLASSIFY=1` reverts to census-only with no rebuild and keeps every counter.

---

## The review caught a silent-failure defect. Read this before trusting any counter

An independent review of round 39's diff cleared the structural questions and found six issues. All are fixed
in the binary above, but **one of them is the reason to distrust counters in this project** and is worth
carrying forward as a pattern, not just as a fix.

**The promotion rebuilt the material under the SAME `material_hash`.** That hash comes from `content_hash` +
wrap/alpha state, none of which a promotion changes. So `CreateMaterial` was called a second time with the
same hash and a *different, emissive* definition — and the file's own round-7 comment, twelve lines above the
derivation, already records as measured fact that aliasing CreateMaterial definitions makes the winner
**draw-order dependent**. If the first definition won, the dome stays black while `skyclassify_promoted`,
`skyclassify_entries` **and** `mat_skyemissive` all report success. Three counters agreeing, nothing on screen.

Fixed by folding the promoted bit into `material_hash`, applied **on top and only for promoted hashes** so the
modding surface (`mat_<HASH>` in `mod.usda`) is untouched for everything else. **Verified rather than
assumed** — `fnv_bytes`/`hash64` replicated exactly, all 6 listed hashes and all 12 classifier candidates
produce distinct material hashes, 0 collisions in 18, and the unpromoted non-default-wrap value is bit-identical
to round 38. There is also a runtime self-check: **`Remix skypromote:`** prints the before/after pair per
rebuilt entry and says `IDENTICAL - THE FOLD DID NOT FIRE` if they ever match. **Read that line before reading
any skyclassify counter.**

The generalisable lesson: **a counter that increments at the point of intent cannot see a failure downstream of
it.** All three counters here sat upstream of the aliasing.

The other five, briefly:

- **Arming latched on intent, not success.** Both `promoted` and the array insert latched before the rebuild
  was known to work, so a full array or a refused `CreateMaterial` was permanent and unretried — the second
  case leaving the hash in the set so every later mesh re-keyed under an identity whose material never changed.
  Now the work runs first; a total failure calls `sky_emissive_unpromote()`. Safe only there, because the
  mesh-key fold is read *earlier* in `submit_subdraw` than the promotion runs, so nothing has moved yet.
  New counters `skyclassify_overflow` and `skyclassify_failed` size both.
- **The census budget starved its own deliverable.** One 64-line ceiling shared between `reject:mixed` (fires
  on the first bad draw of any of up to 4096 hashes) and `ARMED` (needs 8 draws plus a 60-frame settle). Rejects
  would have eaten it before one ARMED line existed — and ARMED is the only line carrying the camera position,
  i.e. the only way to attribute a dome hash to a level. Now **48 ARMED / 32 reject**.
- **The old round-13 census made the ground-truth check unfalsifiable.** It printed `listed=` from
  `sky_emissive_albedo_matches()`, which now also returns 1 for promoted hashes. New `sky_emissive_listed()`
  reads the env list alone. The identical hazard was guarded in the *new* census and missed in the old one —
  when a predicate widens, grep every consumer, not just the ones you wrote this round.
- **`mat_skyemissive` is no longer "did the dome attach?".** A promotion rebuilds every entry aliasing the
  content hash, ~45 on this title, so one dome moves it by tens. Use `skyclassify_entries` and
  `Remix skypromote:`.
- **Two comments were factually wrong** (`RemixTextures.h` claimed no mesh key moves — the material handle is
  folded into `static_key` and `union_hash`; and "mode 1 is image-identical" needed qualifying because the
  rule widens the bounding-box walk condition). Both corrected.

---

## PRIORITY 2 — the one cheap measurement that unblocks per-level sun AND fog

**Nothing in the guest signal carries a level name.** Round 39 searched the entire 969 MB
`bin\remix_dump.log` for every Haze level name and got **zero hits**. So a dome's texture hash cannot be
attributed to a level from a log alone, and that single missing mapping is what blocks per-level sun and
per-level fog for **14 of the title's 16 domes**.

`Remix skyclassify:` prints `cam=[x y z]` at the moment each hash arms, for exactly this reason. **Ask the
user to note which level they were in for each new `albedo=` value, or read the camera position against the
level.** One pass over the ten levels produces the whole table. This is a five-minute job that has been the
blocker for three rounds' worth of per-level work.

---

## PRIORITY 3 — the sun. The pre-registered test was CIRCULAR; do not re-run it

Round 39's brief asked whether a derived `sunAngle`+`sunTimeOfDay` → direction mapping reproduces the
confirmed `SUNMAP=D1A6D1B27ADE6232:0.5155,-0.5736,-0.6366`. **It cannot answer anything.** MEASURED at
`launch-haze-remix.cmd:3389-3436`: that vector's azimuth was itself typed in from the authored `sunAngle`
on 2026-08-17 and only the Y term was swept by eye. The ground truth decomposes to azimuth **128.9995°**
against an authored `sunAngle` of **129.0** — because it was copied from it.

What round 39 established instead, all MEASURED:

- **crashed_plane authors `sunAngle 129.0 / sunTimeOfDay 16.5`, not inherited**, on all six of its
  sun-bearing zone records (`haze_scene_env.csv` rows 48–53).
- **`sunAngle` == `eastAngle` and `sunTimeOfDay` == `timeOfDay`** — the same two quantities in two record
  families, never co-occurring in any of the 310/384 rows.
- **The one non-circular azimuth test supports the direct reading.** An image-derived sun bearing for the
  crashed_plane dome exists at `bin\remix_dump.log:1610776` (`centroid_travel = [0.6193, ~0, -0.78515]`,
  bearing **128.265°**), computed before the level files were cracked. `az_sun = sunAngle` residual **0.74°**;
  `eastAngle + 180f` sweep residual **31.41°**. No axis-origin or handedness choice can absorb that gap.
- **The elevation half does not discriminate and must not be fitted.** 32.14° (linear fold / great circle)
  and 47.88° (half-sine) are both inside the play-test acceptance band of roughly (25°, 73.5°). The honest
  count is 48 discrete × ≥2 continuous degrees of freedom against 2 numbers. Also worth knowing: the
  constant-rate great circle has a **hard ceiling of 32.143°** at `t = 16.5`, and the confirmed 35.0013° is
  **above** it — that model is falsified taken literally.

**Three hard blockers on a per-zone table, each measured, each fatal to one of the brief's proposed
zone-selection keys:**

1. **`zone_pos` is populated on 3 of 310 rows and `zone_dimensions` on 0 of 310.** Zone geometry was never
   recovered. "Camera position against zone bounds" is **dead** — there are no bounds.
2. **The dome-hash key collides.** `haze_domes.csv`: **quarry uses `crashed_plane_dome`**, the same dome as
   crashed_plane, while authoring `sunTimeOfDay 16.75` against 16.5 and `sunIntensity 3.89` against 2.0. A
   `SUNMAP` keyed on the dome hash aims quarry with crashed_plane's sun. **That is a live defect in the knob
   that already ships**, not a hypothetical.
3. **`sphericalH` is not per-zone.** 3 of 310 rows carry it and all three are `<shared>` template rows; 0 of
   384 in `haze_level_scenes.csv`; crashed_plane and copperplant carry zero. Every single-player level
   inherits one template SH. **The brief's third sun input does not exist.**

The per-zone authored table was built anyway and is the deliverable:
`C:\Users\Tristan\AppData\Local\Temp\claude\C--Users-Tristan-Documents-GitHub\6796ba40-1bae-400e-a7f1-6702b2c74f22\scratchpad\hazelight\haze_authored_env_by_zone.csv`
— 384 rows, level / zone / priority / blendDistance / sunAngle / sunTimeOfDay / eastAngle / timeOfDay /
sunIntensity / sunRgb / sunCol / skyAmbientRgb / fogCol / fogNear / fogFar / fogIntensity / skyModel, with
both elevation predictions appended.

**What round 40 can honestly ship on the sun:** azimuth is authored and independently corroborated to 0.74°;
only elevation is unvalidated. So a table whose azimuth comes from `sunAngle` and whose elevation comes from
**one global knob-selected curve** is defensible, *once priority 2 supplies the dome-hash → level mapping*.
Do not ship a per-level elevation fitted to the single confirmed vector.

---

## PRIORITY 4 — fog is reachable, but only two of the four authored fields, and it has never been executed

Round 39 audited this against the runtime source (`dxvk-remix-numos3`, branch `numos3`, HEAD `6476faea` —
the tree `RemixRuntime.cpp:143` records as matching the deployed DLL). **The `src/d3d9/`-only trap does
apply to the obvious mechanism** and this is its fourth appearance in the project:

- **D3D9 fixed-function fog: UNREACHABLE.** `FogState`'s only producer is `setFogState()`
  (`src/d3d9/d3d9_rtx_utils.cpp:245`), called only from `src/d3d9/d3d9_rtx.cpp:686`.
  `rtx_remix_api.cpp:900` default-constructs `DrawCallState`, so `fogState.mode` is `D3DFOG_NONE` forever.
- **`rtx.enableFog` / `rtx.fogColorScale` / `rtx.maxFogDistance`: PRESENT AND INERT.**
  `rtx_composite.h:90-92` declares them and a real pass reads them, but `composite.slangh:33-36` returns
  `vec4(0.0)` when `fogMode == D3DFOG_NONE`. **They multiply a hard zero.** Do not set them and do not
  report a result from them.
- **`rtx.volumetrics.*`: REACHABLE.** `RtxGlobalVolumetrics::getVolumeArgs`
  (`rtx_global_volumetrics.cpp:467`, per frame from `rtx_context.cpp:1365`) reads the options **directly**.
  The D3D9 override is behind `fogState.mode != D3DFOG_NONE` (`:483-486`), which an external draw cannot
  open — and volumetrics stay **on** in that case (`:445`, `:585`).
- **Runtime mutation:** `remixapi_SetConfigVariable` (`rtx_remix_api.cpp:1642`, bound `:2582`), one-frame
  latency via `applyPendingValues` at `rtx_context.cpp:840`. Env vars are load-time only; no conf hot-reload.

| authored | Remix option | verdict |
| --- | --- | --- |
| `fogCol` | `rtx.volumetrics.singleScatteringAlbedo` (+ `transmittanceColor`) | reachable at runtime |
| `fogIntensity` | `rtx.volumetrics.transmittanceMeasurementDistanceMeters` (shorter = denser) | reachable at runtime |
| `fogFar` | `rtx.volumetrics.froxelMaxDistanceMeters` | approximate — bounds froxel allocation, not fog end |
| `fogNear` | *nothing* | **no equivalent.** `VolumeArgs` has no start distance |

**Round 39 shipped a probe, not an implementation**, because every word above is inferred from source and
`SetConfigVariable` had never been called on this title. `runtime::probe_set_config_variable()` calls it with
`rtx.rpcs3.probeKeyThatCannotExist`; `rtx_remix_api.cpp:1653` returns `GENERAL_FAILURE` for an unknown key,
so the probe exercises the whole dispatch and provably writes nothing.

**Read `bin\log\RPCS3.log` at startup for `Remix: SetConfigVariable probe returned ...`.**
`GENERAL_FAILURE` is the **PASS**. `SUCCESS` on a key that cannot exist means the slot is misrouted — treat
fog as unreachable and say so.

**Three hazards to carry into any implementation.** (a) `remixapi_SetConfigVariable` takes only the remixapi
mutex while the render thread walks `m_optionLayerValueQueue` under `RtxOptionImpl::getUpdateMutex()` — call
it **on zone change only, never per frame**. (b) `RtxOptionLayer::save()` (`rtx_option_layer.cpp:376`)
serialises the whole user layer, so Save Settings in the Remix UI while zone fog is live bakes those values
into `bin\user.conf`, where they can never be overridden from `rtx.conf`. (c) `bin\rtx.conf` has zero
fog/volumetric keys; `bin\user.conf` has exactly two, `rtx.volumetrics.froxelDepthSlices = 48` (line 28) and
`rtx.volumetrics.froxelGridResolutionScale = 8` (line 38), both grid-shape rather than appearance. Nothing
will fight a runtime set.

A cheaper alternative exists and is **already reachable today**: the fork's weather system reads
`__weather.target` out of the game-value store this backend already writes to
(`RemixGSRender.cpp:5495` writes `haze.nectar_disruption`), and drives
`transmittanceMeasurementDistanceMeters` and `singleScatteringAlbedo` plus ~30 more options with free
time-based blending. Cost: Haze's continuous `fogCol`/`fogIntensity` quantise onto twelve named presets
(`clear, partlyCloudy, overcast, hazy, foggy, drizzle, rainstorm, thunderstorm, snow, blizzard, sandstorm,
smoggy`, `rtx_fork_weather.cpp:1131-1132`).

---

## Carried from round 38, untouched by round 39

Round 39 changed **nothing** in the gauge/viewmodel/helmet/static-index areas. Round 38's play-test card
still applies in full and its readings are still owed:

- **`gauge_cur_avail`** on `Remix live:` — `0` means `GAUGECURDIMS` is structurally dead on this title and
  the lever becomes `DEFERPREANCHOR=1` (~14 fps, user-confirmed to work).
- **`gauge_cur_dims` / `gauge_anchor_prev`** — did the fix fire, and did the prev-branch population fall by
  exactly that much.
- **The 4000-frame `Remix vmbasis:` series** must be re-taken with `s = 0` pre-registered as the new minimum.
  `VMBASISEVERY=1 VMBASISVTX=3649 VMBASISMAX=4000` is still armed on purpose.
- **Helmet:** `uiforcepairs=3` in `bin\log\RPCS3.log` **after exit**, and `Remix uiwrap:` gaining `route=2d`
  for `vp=9f591b6a6b825612`. The user has asked seven times.
- **Plant walls:** `Remix static-index:` `peak=` must stop pinning at `budget=` now the ceiling is 512 and
  the launcher arms 128.
- **`VMROTLOCK` stays 0 on purpose.** Test `GAUGECURDIMS` first.
- **Still owed, fourth round running:** `classify_draw`'s "IGNORE is a no-op on the API draw path" wants
  confirming by disassembling `bin\remix\d3d9.dll`; `RemixTransforms.h`'s "What `DRAWAUDIT=0` costs" still
  names neither `m_streak_measured` consumer.
- **Frame time.** Not measured for three rounds now. Round 35's instruction still stands: blank
  `SKIPEXTENTVP` and arm `DRAWAUDIT=0` together and say so, or attack `decode` (3.93 ms).
- **`rtx.fallbackLightMode` is 0** in `bin\rtx.conf`; round 33 asked for 2. That file is the user's.

## Instrument notes worth keeping

- **A census field that re-queries state the same function just mutated is a tautology, not a measurement.**
  Round 39's `Remix skyclassify:` originally re-read `sky_emissive_albedo_matches()` for its `listed=` field
  — after promotion had already inserted the hash. Every armed hash would have printed `listed=1` and the
  ground-truth check would have silently passed for free. It is now sampled by the caller and passed in as a
  parameter so it cannot come back. **Before printing a "was this already true?" field, check whether your
  own code has made it true.**
- **`Remix stats:` goes only to `bin\log\RPCS3.log`, which is exclusively locked while the emulator runs.**
  Anything a play-test has to judge must also be on `Remix live:`. Round 39's counters are on both.
- **The `Remix sky-census:` line is deduplicated per (vertex program, outcome) with a monotone extent rule.**
  It is a *program* census, not a draw census, and its row counts must never be read as a population.
