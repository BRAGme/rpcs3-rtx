# Round 36 inbox — RPCS3 RTX Remix backend, Haze (BLUS30094)

Written at the close of round 35, 2026-08-17. Branch `remix-backend`, working tree dirty (rounds 32-35 are
**not committed**; HEAD is still `1ebff4c57`).

Deployed for play-test:

| artefact | hash (SHA256, first 16) | note |
| --- | --- | --- |
| `bin\rpcs3.exe` | `E984DD7DAE13CAD5` | built this round, MSBuild exit 0, **0 errors**, 1 warning, the pre-existing C4723 "potential divide by 0" at `RemixGSRender.cpp:10363` (not mine; it moved from :10345 only because round 35 inserted lines above it) |
| `bin\rpcs3-next.exe` | `E984DD7DAE13CAD5` | **identical to rpcs3.exe — the copy was made** |
| `bin\remix\d3d9.dll` | `16A0B512F33EBB66` | **UNCHANGED.** Nothing was deployed into `bin\remix\`. |

Knobs changed this round: `VMBASIS` 6 → **5**, and three new ones armed — `HIDEPAIRVP=F39F504649B6F442`,
`HIDEPAIRFP=B64DC06F79B8B42B`, `HIDEPAIRMODE=1`. Everything else is round 34's. Every armed value verified
inside its clamp against current bytes.

Binary audit, all **PASS** (`audit_binary35.py` in the scratchpad): NEW text
`dbasis=%.6g dotpre=[...] dotpost=[...] frame=%llu line=%u/%u` and `cat_hidden=%llu cat_hidepair=%llu
cat_particle=%llu` **present**; OLD text `dbasis=%.6g frame=%llu line=%u/%u` and
`cat_hidden=%llu cat_particle=%llu` both **absent**; new knobs present as **UTF-16**
(`RPCS3_REMIX_HIDEPAIRVP/FP/MODE`) with `RPCS3_REMIX_VMBASISPIVOT` as the known-good control, and an ASCII
scan of all four confirmed to report them missing — which is why an ASCII scan lies.
Format audits, position by position: `Remix vmbasis:` **61 specifiers / 61 arguments, 0 type mismatches**;
`Remix stats:` **256 / 256**, with `cat_hidepair` verified adjacent to `cat_hidden` on both sides.

---

## READ THIS FIRST: the round-35 brief's headline finding was a measurement artefact, and the same
## class of error is the single most likely thing to happen to you.

Round 35's brief opened with a confident, numerically specific bug report: `apply_viewmodel_basis`
"produced a 120-degree yaw about the up axis", `-0.5 / ±0.866` being cos/sin of 120 degrees, with a
request to rewrite the reflection composition.

**The operator was correct. The brief projected the transform's ROWS.** `remixapi_Transform` is
column-vector — `p_world = M * p_object`, `matrix[i][3]` is the translation — so the object's world-space
axes are the **COLUMNS**. Row `i` of a non-symmetric rotation is a coefficient vector and its dot product
with a camera axis is not a property of the object at all.

On the brief's own line (`vp=830d7d1b9681c475 albedo=0721D150DF278E7D flip=6 frame=3945`):

```
ROW    dots post = (-0.50091  +0.99984  -0.50101)   <- reproduces the brief to 5 decimals
COLUMN dots post = (+0.99945  +0.99946  +0.99891)   <- the pass mark the brief itself asked for
```

Zero lines of operator change were needed. Round 34 lost a round to a stale comment; round 35's brief
nearly lost one to a transpose. **Print the derived quantity, do not print two matrices and hope.** That
is what `dotpre=`/`dotpost=` now do.

Also: `bin\remix_dump.log` is **757 MB** and append-mode, shared by every title, and `dump_line` writes
every census to BOTH it and `bin\log\RPCS3.log`. The dump is readable while the game runs; RPCS3.log is
exclusively locked. **Do not use a `-Max N` tail filter — it stops at the FIRST N matches in the window.**
A `StreamReader` over the whole file keeping the last N in a queue takes ~40 s and cannot lie; that is how
every number in this file was obtained.

`docs\remix\KNOBS.md` "Round 35" carries every derivation below with its anchors. Read it before
re-opening any of these.

---

## Priority 1 — judge `VMBASIS=5`, and it is now a one-grep question

The census emits `dotpre=[X.right Y.up Z.fwd] dotpost=[...]` — COLUMN direction cosines, normalised on
both sides. MEASURED: `pre` is uniformly `(right, -up, -fwd)` on all 24 `tagonly=1` rows, so all eight
flips are a lookup table and you never have to project anything by hand again:

| flip | dotpost | det | flip | dotpost | det |
| --- | --- | --- | --- | --- | --- |
| 0 | (+1 -1 -1) | +1 | 4 | (+1 -1 +1) | **-1 MIRROR** |
| 1 | (-1 -1 -1) | -1 | 5 | (-1 -1 +1) | +1 **← armed** |
| 2 | (+1 +1 -1) | -1 | 6 | (+1 +1 +1) | +1 (round 34 step 2) |
| 3 | (-1 +1 -1) | +1 | 7 | (-1 +1 +1) | -1 |

**Pre-registered readings. Record the actual value even where it disagrees.**

- `dotpost=[-1 -1 1]` (within 0.01), `dcentre < 1e-3`, `cdist_post` within 0.01 of `cdist_pre`. If
  `dotpost` reads anything else, the operator changed behaviour and THAT is the finding.
- **THE REFUTATION THAT MATTERS:** flip 5 is INFERRED from the user's words "facing the right way, just
  upside down", not measured. If the arms come out **upside down with left and right swapped**, the error
  is a genuine MIRROR and the answer is `VMBASIS=4` — and that would also refute my determinant argument
  (that the world divide preserves handedness because the legs and the world are not mirrored). Say so.
- If flip 5 lands them **right way up but facing backwards**, try 0. If **right way up, facing right, on
  the wrong side of the screen**, go back to 6 and report — that combination is not reachable by any flip
  and would mean the recovered basis itself is wrong, not the correction.
- All three are one launcher line and **no rebuild**.

**Do NOT chase "a little high" yet.** MEASURED: every near-depth viewmodel draw's centroid sits above the
eye, +0.098 to +0.932 depending on the (vp, albedo) — and it is IDENTICAL at flip 0 and flip 6 (same
albedo: +0.3844 vs +0.3754), with `dcentre <= 6.5e-06` proving directly that the basis operator does not
move the centroid. So it is a property of the recovered world transform. But an upside-down arm rig also
*reads* as "high" because the hands end up at the top of the frame. **Re-measure after the orientation is
settled, or you will tune an offset knob to cancel a rotation.** Full per-pair table in KNOBS Round 35.

---

## Priority 2 — the player's full-body shadow: armed, and the brief's chosen key was the wrong one

The round-35 brief directed this at the (vp, albedo) pair route. **That route over-matches.** MEASURED
from the user's own picks:

```
legs  vp=f39f504649b6f442 fp=b64dc06f79b8b42b albedo=0721D150DF278E7D vtx=952  viewmodel=0
arms  vp=f39f504649b6f442 fp=d6f00cddfb5c6e0a albedo=0721D150DF278E7D vtx=2140 viewmodel=1
```

Same vp, same albedo — a (vp, albedo) pin takes the arms with the legs, which is round 20's failure mode.
The **fragment program** separates them: `b64dc06f79b8b42b` appears **1381 times in the dump, all 1381 at
vtx=952**, across 12 albedos (skin variants), and not one row is anything else.

Shipped as `RPCS3_REMIX_HIDEPAIRVP` + `HIDEPAIRFP` + `HIDEPAIRMODE` (empty either half disarms;
`min(env, 2)`, default 1). Armed at that pair, mode 1.

**Pre-registered:**

- **First, on the `knobs=` block, check `hidepairvp=f39f504649b6f442 hidepairfp=b64dc06f79b8b42b
  hidepairmode=1`.** All three print the PARSED/CLAMPED value, so a typo that parses to 0, or a
  `HIDEPAIRMODE=3` silently running as mode 2 (THIRD_PERSON_PLAYER_MODEL, not HIDDEN), is visible there
  rather than silent. **This was missed in the first cut of round 35 and caught by diff review** — the
  round-31/32 reported-vs-armed trap for a fourth time. The fields exist now because of it.
- `cat_hidepair` on `Remix stats:` climbs into the thousands. **If it reads 0 the key never matched and
  the hashes are wrong, not the mechanism.** It is deliberately separate from `cat_hidden`, which already
  read 2617 from the albedo list — a shared counter could not have answered this.
  Note `Remix stats:` goes to `bin\log\RPCS3.log` (`rsx_log.notice`), which is exclusively locked while
  the game runs — read it after the session, not during. The visible result (no legs, no body shadow) is
  the mid-session signal.
- The full-body shadow is gone, and looking down shows no legs. **Mode 1 removes the body from primary
  rays too** — that is what `Hidden` does in this runtime (`mask = 0`).
- **If the user wants the legs still visible**, that is `HIDEPAIRMODE=2` plus TWO lines the user must add
  to `bin\rtx.conf`: `rtx.playerModel.enableInPrimarySpace = True` and
  `rtx.playerModel.enablePrimaryShadows = False`. Both or it is worse than nothing (with only the first,
  invisible AND still casting). Warning: `enableInPrimarySpace = True` also masks every VIEW_MODEL
  candidate to zero inside `createViewModelInstances`, which would take the ARMS with it the moment a
  view-model camera is valid. That interaction is untested.
- REVERT: `set "RPCS3_REMIX_HIDEPAIRVP="`.

---

## Priority 3 — the helmet as UI. The mechanism is ready; the TARGET has never been identified.

**The user has now asked three times and this round could not aim it. Do not let that stand again — the
unblocking measurement is one Ctrl+Click.**

What is settled, so nobody re-derives it:

- **`UIFORCEVP` is the right mechanism and it is stronger than any category flag.** A forced draw returns
  from the screen-space block *before* `per_draw_transform` and `submit_subdraw`: no mesh, no instance, no
  material, **no BLAS** — therefore no shadow, no lighting, no world clipping, and no depth at all (the
  compositor has none; `grep -n depth RemixCompositor.*` = 0 hits). Both of the user's stated requirements
  are met by construction.
- It is a **comma list, bound 8**, currently holding one hash (`2F64C2F8FFD6ADD1`, `ui_forced=98681`), so
  **seven slots are free and adding the helmet needs NO rebuild.** One guard:
  `!rsx::method_registers.depth_write_enabled()` — a listed program that writes depth is not forced. Five
  further gates can then still refuse it (`ui_skipped`, `ui_space_none`, `ui_render_target`), and a
  refusal DELETES the draw rather than falling back to world geometry. That is the risk.
- **`9f591b6a6b825612`, called "the visor" since round 19, is not a visor.** MEASURED over every
  `Remix fpcandidate:` row: it has exactly two fragment programs, `cbede4eb45f0fd25` (vtx=59,
  extent 5.15..11.02, eye_dist 1.56..3.77, 1309 rows) and `7faad0c4437ae7c1` (vtx=4, albedo
  `CC6008D0E9E98972`, **extent 0.02..0.03**, eye_dist 0.20..0.56, 56 rows). A 7-unit object two metres
  out is not a visor at the eye; a 3-centimetre quad is not one either. Round 19's label came from the
  near-depth census (`scale_z=0.00125`), never from a pick.
- **What VIEW_MODEL tagging currently buys the visor: nothing that helps.** Read from the fork source
  (`dxvk-remix-numos3`; every cited file's mtime precedes the deploy, so INFERRED-to-be-running, not
  disassembled): there is **no per-instance castShadow flag anywhere in the runtime**; a VIEW_MODEL
  instance stops shadowing world geometry only once the duplicate exists, which needs a VIEW_MODEL camera
  submitted *that frame*; and it does **nothing at all** about clipping — the instance is ordinary
  world-space geometry in the same opaque TLAS. `rtx.worldSpaceUiTextures` is live on this path but only
  forces unlit emissive and does not remove anything from shadow rays. Full detail in KNOBS Round 35.

**The measurement to ask for: Ctrl+Click the helmet / visor edge** (and, separately, the muzzle end of the
weapon). `Remix picked:` writes to `bin\remix_dump.log`, readable while the game runs. Then add the vp to
`RPCS3_REMIX_UIFORCEVP` as a second comma entry — one launcher line, no rebuild.

---

## Priority 4 — frame time: `DRAWAUDIT=0` deliberately NOT taken, and the reason is explicit

MEASURED this run: `frame_ms=31.25 | draw=23.34 (ui=2.40 mesh_create=0.12 tex_bind=1.16 uv=2.39
draw_instance=0.34 decode=3.93 audit=7.60 hash=1.58 xform=0.32 rest=3.50)`. `audit` is **24.3% of the
frame** — still the largest child of `draw`. (Round 34's 11.35/35.22 = 32.2% was a different scene; quote
the ratio, not the milliseconds, when comparing runs.)

Not armed, on purpose: round 34 proved `DRAWAUDIT=0` also stops the `SKIPEXTENTVP` refusal gate firing (it
reads `&& m_streak_measured`, written only inside `audit_world_extent`), and `SKIPEXTENTVP` is armed on
`57A12323F22F4988` — **one of the near-depth viewmodel programs priority 1 is measuring**. Decoupling
means blanking `SKIPEXTENTVP` in the same run, which changes what is submitted for exactly that
population. **Round 36: take it. Either blank `SKIPEXTENTVP` and arm `DRAWAUDIT=0` together and say so,
or attack `decode` (3.93 ms) with round 33's named fix — cache decoded positions per (source pointer,
count).**

---

## Priority 5 — carried unchanged from round 35, still open

- **The static-index budget passed and is no longer the binding constraint.** MEASURED final line:
  `entries=77 triangles=42203 rebuilds=1884 deferred=0 stale=0 dropped=0 peak=56 budget=64 resident=65
  evicted=12 nomesh=0`. `peak < budget`, and `dropped`/`stale` are **zero**. The white-walls mechanism is
  relieved. The untaken lever remains **a longer idle window scoped to static-index UNION meshes only** —
  that is a code change, not a knob, and it is still the highest-value item on that problem.
- **The world divide is BIMODAL on one mesh** (`d0b6a471bb2d463b` / `27159433F0E63031`): 79.2% of rows
  discard < 1e-6 and 17.0% discard > 1000, with the bad mode's magnitude equal to the camera's. Untouched
  this round. Pre-registered refutation still stands: if the >1000 bucket does not fall from 76/448, the
  reference-mismatch inference is wrong.
- **The warping light fixture**, baseline ±0.28% anisotropic basis error on
  (`830d7d1b9681c475`, `5BC48BBB303398E3`) at fixed 180 vertices with `ext` constant to 0.6%. Want
  < ±0.05%. Untouched this round.
- **`rtx.fallbackLightMode` is `0`** in `bin\rtx.conf` (verified this round, `:9`), and
  `rtx.fallbackLightType` is `0` (`:2`). Round 33 asked for `fallbackLightMode = 2` and it has not been
  done. `bin\rtx.conf` is the user's file and is read-only to these rounds.
- **`Use RSX Backface Culling` has been OFF the whole time** — the custom config wins. Unchanged.

## Corrections to standing documents, made or owed

- **MADE:** `docs\remix\KNOBS.md` round-34 open item 2 (`SUNSPRITEHOLD` armed at 999999) is **closed** —
  the launcher now reads `216000`, exactly the ceiling, so the knobs line and the launcher agree.
- **OWED:** the launcher note claiming that blanking `VMPAIRVP` disarms the VIEW_MODEL camera because
  "vm_tag_route_armed has four terms and this launcher makes all four false" is **stale** — round 34 armed
  `VMDEPTHOFFSET=2` and `viewmodel_depth_offset_max() > 0.f` is the fourth term, so that route is armed.
- **OWED:** `classify_draw`'s comment "IGNORE is a no-op on the API draw path" is probably stale. The fork
  added `externalDrawShouldSkip` (`rtx_fork_submit.cpp`, called from `rtx_scene_manager.cpp`) which drops
  an `Ignore`-tagged submesh before instance creation. INFERRED from file mtimes preceding the deploy —
  **confirm by disassembling `bin\remix\d3d9.dll` before relying on it**, the way round 34 confirmed bit 26.
- **OWED:** `RemixTransforms.h`'s "What `DRAWAUDIT=0` costs" list still names neither `m_streak_measured`
  consumer. Round 34 flagged it; it was not corrected this round either.
- Note two `set` lines are no-ops because armed == default: `VMBASISMAX=24` and `DRAWAUDIT=1`.

---

## Play-test card

Launch as usual (`launch-haze-remix.cmd` → `bin\rpcs3-next.exe`, `E984DD7DAE13CAD5`).

| what to look at | where | one-line revert |
| --- | --- | --- |
| **Arms: still upside down?** | `Remix vmbasis:` in `bin\remix_dump.log` (readable while the game runs) — read `dotpost=` , want `[-1 -1 1]` | `set "RPCS3_REMIX_VMBASIS=6"` |
| If arms are now right way up but LEFT/RIGHT swapped | that means a real mirror | `set "RPCS3_REMIX_VMBASIS=4"` |
| **Full-body shadow gone? Legs gone when you look down?** | `Remix stats:` — `cat_hidepair` should be in the thousands | `set "RPCS3_REMIX_HIDEPAIRVP="` |
| If you want the legs VISIBLE but casting no shadow | `set "RPCS3_REMIX_HIDEPAIRMODE=2"` **and** add both `rtx.playerModel.enableInPrimarySpace = True` and `rtx.playerModel.enablePrimaryShadows = False` to `bin\rtx.conf` | `HIDEPAIRMODE=1` |
| White/invisible walls in the plant | `Remix static-index:` — last round read `peak=56 budget=64 dropped=0`, i.e. relieved | `set "RPCS3_REMIX_STATICINDEXBUDGET=8"` |
| **One measurement to ask for: Ctrl+Click the HELMET / visor edge** | `Remix picked:` in `bin\remix_dump.log` | n/a — this is what unblocks the helmet-as-UI request |
