# Round 35 inbox — RPCS3 RTX Remix backend, Haze (BLUS30094)

Written at the close of round 34, 2026-08-17. Branch `remix-backend`, working tree dirty (rounds 32-34
are **not committed**; HEAD is still `1ebff4c57`).

Deployed for play-test:

| artefact | hash (SHA256, first 16) | note |
| --- | --- | --- |
| `bin\rpcs3.exe` | `2C2E69DA628CE554` | built this round, MSBuild exit 0, 0 errors |
| `bin\rpcs3-next.exe` | `2C2E69DA628CE554` | **identical to rpcs3.exe — the copy was made** |
| `bin\remix\d3d9.dll` | `16A0B512F33EBB66` | **UNCHANGED.** Nothing was deployed into `bin\remix\`. |

Armed knobs this round: `VMTAGONLY=1`, `VMBASISPIVOT=1`, `VMBASIS=0` (disarmed from 6),
`VMDEPTHOFFSET=2` (re-armed), `STATICINDEXBUDGET=32` (from 8), `DRAWAUDIT=1` (**armed 0 then reverted —
see priority 6**). Every armed value verified inside its clamp.

The diff was reviewed by a second agent before this was written. It found five real items: three
comment/doc defects of mine (fixed), one latent code defect of mine (**fixed** — `defer_candidate =
false` deliberately stays on `viewmodel_draw`, because `flush_deferred_for_anchor` recomputes the
transform and does not re-run `apply_viewmodel_basis`, so a deferred viewmodel draw would lose its basis
correction), and the `DRAWAUDIT` safety refutation that caused the revert. It also flagged two open
items from the uncommitted round-32 work — see the end of this file.

Binary audits, all PASS (script kept in the scratchpad, `audit_binary.ps1`):
NEW census text `tagonly=%u pivotsrc=%u pivot=[%.6g %.6g %.6g] cmeasured=%d`,
`centre_pre=[...] centre_post=[...]`, `cdist_pre=%.6g cdist_post=%.6g dcentre=%.6g dtrans=%.6g dbasis=%.6g`
and `vmbasispivot=%u vmtagonly=%u` all **present**; OLD tail
`eye_post=[%.6g %.6g %.6g] frame=%llu line=%u/%u` and OLD adjacency
`projsplitvdelta=%.4g vmpairvps=%u` both **absent**; new knobs present as **UTF-16**
(`RPCS3_REMIX_VMBASISPIVOT`, `RPCS3_REMIX_VMTAGONLY`) with `RPCS3_REMIX_GAUGEANCHOR` as the known-good
control and an ASCII scan confirmed to report them missing (which is why an ASCII scan lies).
New `vendored_runtime_fnv1a` quad `0x09653f484ec94dc0` present at file offset 19237648; old quad
`0x63656dfe3da8f069` **absent**.

---

## READ THIS FIRST: read `bin\log\RPCS3.log` from the last play-test BEFORE forming a hypothesis.

That is not general advice. Round 33 armed `VMDEPTHOFFSET=2`, the user played it, and the resulting 7 MB
log contained **four of round 34's five findings** — `vm_tagged=7960`, six `Remix vmbasis:` lines with
full pre/post matrices, `vmcam_applied=7960/7974`, `vmcam_twin=3185/7096`. Round 34's brief was written
believing `vm_tagged=0` and that the census had never emitted. Both were stale by one play-test.

Also: `Remix live:` does **not** appear in `bin\log\RPCS3.log` (0 hits, vs 79 for `Remix stats:`).
Grepping the emulator log for a live-only field silently finds nothing. And when tailing
`bin\remix_dump.log`, **shrink the window until the match count is under your cap, then take the last
line** — a `-Max N` filter stops at the FIRST N matches in the window, and I read three counters off an
earlier session that way before catching it.

`docs\remix\KNOBS.md` "Round 34" carries every derivation below with its file anchors. Read it before
re-opening any of these.

---

## Priority 1 — judge the viewmodel. Step 1 of two is armed; step 2 is one launcher line.

**Four faults were found, all MEASURED, and none of them was the tag.** Full evidence in KNOBS Round 34.
Summary: (1) the viewmodel divide is self-referential and parks every viewmodel draw at the world origin
2129 units from the eye; (2) `apply_viewmodel_basis` pivots about the eye, which then throws it a further
2537-2564 units; (3) `!viewmodel_draw` disarms the tail-rescue ladder that PROJSPLIT lives behind;
(4) `!m_active_viewmodel.valid` suppresses the **only** VIEW_MODEL camera submission in the backend, on
44.9% → 0% of exactly the frames the arms are on screen, which is the dev menu's row of dashes.

**Shipped as step 1: `VMTAGONLY=1` + `VMBASISPIVOT=1` + `VMBASIS=0` + `VMDEPTHOFFSET=2`.** That restores
the placement the ordinary world path measurably gets right (0.40 and 0.44 units from the eye, unit
basis) and adds exactly one new variable — the tag.

**Pre-registered readings. Record the actual value even where it disagrees.**

- `vm_tagged` non-zero **and** `vmcam_considered` = **0** on `Remix stats:`. That pair is the one-line
  proof `VMTAGONLY` did what it claims: the whole placement block is skipped.
- `vmcam_twin` rises from **44.9% of flips** (3185/7096) toward `cam_resolved`, and the Remix dev menu's
  `VIEWMODEL` row shows a real Position/Direction/FOV instead of `-`. **If `vmcam_twin` still tracks
  ~45%, fault 4's suppressor is not that condition and I was wrong.**
- **THE REFUTATION THAT MATTERS:** the census's new `cdist_pre=` reads **~0.4**, not ~2129. If it stays
  at ~2129 then the relocation is NOT the viewmodel divide, `VMTAGONLY` is the wrong lever, and the round
  should stop and say so rather than tune the pivot.
- `pivotsrc=0` with `flip=0` means "the operator did not run", not "it chose the eye" —
  `apply_viewmodel_basis` returns before reading the pivot when `VMBASIS=0`. Do not read a `pivot=[0 0 0]`
  as a measurement.

**Step 2, only after step 1 is judged, and only if the arms are visibly misoriented:** set
`RPCS3_REMIX_VMBASIS=6` in `launch-haze-remix.cmd`. With `VMBASISPIVOT=1` that is an in-place 180-degree
rotation about the camera's right axis. Pre-registered: **`dbasis` ~2 (non-zero, or the operator did not
run) and `dcentre` < 1e-3 (or it moved the mesh)**, with `cdist_post` within a few hundredths of
`cdist_pre`. **If the arms look right after step 1, do not do step 2** — round 19's `6` was a guess taken
against a mesh 2129 units out of place and it measures nothing about handedness.

**Still open, and it is the user's own diagnostic hint: nobody has identified the program that draws the
LEGS.** The legs render correctly in the same frame with the same camera, so a side-by-side of their
recovered basis against the arms' is the best differential available, and it needs one measurement this
round could not make: **ask the user to Ctrl+Click their own legs** (looking straight down). `Remix
picked:` writes to `remix_dump.log`, which is readable while the game runs. The legs are at
`scale_z=0.49875 offset_z=0.50125` (the world depth range), so they are NOT in the near slice and the
depth route will never tag them — which is exactly what makes them a clean reference.

**Over-match to keep an eye on:** at `VMDEPTHOFFSET=2` the near-depth slice tags three programs, and the
census names a fourth that round 19's list of three did not — `af06f6d32ec048ee` (vtx=4, three albedos
`8F03C4383D517AE5` / `D2919CF63382D2C4` / `D7CC636E3795EA3B`) alongside `830d7d1b9681c475` and
`57a12323f22f4988`. All are small (4-8 vertices for the extras). The user did not report the sky
vanishing in the round-33 test, and gate 3 (the only masking gate) is off, so this appears tolerable —
but `vm_tagged=7960` is the population, and if something small disappears, this is where to look.

---

## Priority 2 — the static-index budget at 32. Round 31's BLAS objection is retired.

`STATICINDEXBUDGET` 8 → **32**, with the objection settled from the deployed runtime's own source rather
than argued: a ~426-triangle union **never gets its own BLAS** (strict `<` against `max(1000,100)`), a new
mesh **hash** costs no BLAS at all (BLASes are born on the draw path; 86.9 submitted draws/frame against
`meshes_live=16133`), `numFramesToKeepBLAS` resolves to **1** here so a BLAS is freed ~70 ms after its
last draw, and the merged pool recycles **by buffer size, never by mesh hash**. `MESHIDLE=600` governs
host mesh handles, not BLAS memory. Derivations and file anchors in KNOBS Round 34.

**Pre-registered reading, DIFFERENCED WITHIN THE NEW RUN** (first to last census line — never against
another run; the rate varies 1.03..4.34 across runs):

- SUCCESS 1: **`peak < 32`**. If it comes back `peak=32 budget=32`, go straight to 64 — the whole 8→64
  range is bounded at +0.25 ms of frame time.
- SUCCESS 2: `dropped`/`rebuild` below **3.661**. (The widely-quoted **1.17 was an undifferenced
  2020/1728**; that run's correct differenced value is 1.145. Do not compare against 1.17.)
- SUCCESS 3: `resident`/`entries` on the **FINAL** line above **63.8%** (257/403). **These three are
  point-in-time gauges, not cumulative counters — read the last line, never difference them.**
- REGRESSION: `frame_ms` rises >1.0 ms while `mesh_create` stays under 1.0 ms (the merged-bucket rebuild
  term), or per-create cost rises above ~6 us from the measured 4.54 us.
- **Round 32's `evicted`-rising regression clause is WITHDRAWN.** `evicted` counts RPCS3-side handles
  reaped by MESHIDLE and a handle carries no BLAS unless drawn that frame; rising `evicted` with falling
  `dropped` is the expected shape. The regression signal is `frame_ms`.

**The bigger lever, flagged and NOT taken.** One earlier run ended `resident=12 evicted=1168` — **1.0%**
of unions still holding a live handle, a steady-state decay rather than a load transient. Every evicted
union needs a budget slot to come back, so 1168 at 8/frame is ≥146 frames while `MESHIDLE=600` re-evicts
anything out of view for 10 s. **A longer idle window scoped to static-index UNION meshes only** would
relieve the budget far more cheaply than raising it. That is a code change, not a knob, and it is the
single highest-value item left on the white-walls problem.

---

## Priority 3 — `Use RSX Backface Culling`: the user edited the file the game does not read

MEASURED: `bin\config\config.yml` = **`true`**, `bin\config\custom_configs\config_BLUS30094.yml` =
**`false`**, and `bin\log\RPCS3.log` line 166 reads `config_mode='custom config'`. **The custom config
wins; the effective value is OFF and their change never took effect.** No env escape hatch either —
`cull_from_rsx()` is `env || g_cfg...`, so `RPCS3_REMIX_CULL` can only force ON and it is not set.

**Tell the user plainly: leave it off, and it was never on for Haze.** Round 33 recorded that turning it
on made the plant far worse; that report is now hard to attribute, because the setting they turned on was
in the file the game ignores. Every instance currently ships `doubleSided=1`. The white walls are the
static-index budget, not culling.

---

## Priority 4 — the NEW defect worth a round: the world divide is BIMODAL on one mesh

For (`vp=d0b6a471bb2d463b`, albedo `27159433F0E63031`), all rows on the **same** `surf=014D0000` with the
**same** raw bbox: **355 rows (79.2%) discard a translation < 1e-6, and 76 rows (17.0%) discard > 1000.**
Identical geometry, two different answers. And the bad mode's magnitude is the **camera's**, not the
object's: `|origin − cam|` clusters at 2094.94..2143.41 against `|cam| = 2106.8` — within 2%.

INFERRED: `fused × ref⁻¹` where `ref` did not match the draw's `fused`, i.e. a wrong-pass or wrong-frame
reference leaving the camera's own translation in the result. Supporting: `camclip=512x288` against the
draws' `1024x576` in most rows, and `camage` up to **702 frames**. This is the population the eleven
identity-pin knobs are currently masking, and the tree already names the shape — *"Haze submits absolute
world vertices through several passes which share a vertex program. A VP-wide transform rule cannot
distinguish the gameplay draw from a camera-relative copy."*

**Pre-registered:** if the >1000 bucket does not fall from **76/448 = 17.0%**, the reference-mismatch
inference is wrong.

**Do NOT change the three identity picks** (`a7505f7ad3a86838`, `1f9342de47afb400`, `d0b6a471bb2d463b`).
They are **correct**: absolute-world-space static geometry, for which an identity transform is right and
the vertices already sit at ~1800. Nothing rides the camera. MEASURED: 7 distinct raw bboxes across 321
draws spanning frames 3596..55560, the dominant one **bit-identical across 135 draws** while the camera
moved 3.23 in X. `a7505f7ad3a86838` is hand-pinned via `WORLDIDENTITYOPAQUEFPPAIR4` (not
`WORLDIDENTITYVP`), and that pin discards `translation=3.72529e-09` — 78.2% of its draws discard < 1e-6,
with `WORLDIDMAXT=32` already exempting the largest remaining buckets. **`identity-bypass` has ELEVEN
producers, not one** — check all of `world_identity_pair` .. `pair5`, `fp_pair`, `opaque_fp_pair` ..
`opaque_fp_pair4` before concluding a draw was not pinned.

---

## Priority 5 — the warping light fixture, with a discriminator that works and a baseline to beat

`vp=830d7d1b9681c475` draws **612 distinct albedos across 35,575 census rows**, so a VP-keyed fix
over-matches by ~600×. **(vp, albedo) does isolate it**, and the discriminator is the `eye_dist` floor:
the two rigid arm albedos (`1CDD5249E6504F13`, `CC6008D0E9E98972`) **never exceed 1.10**, and the seven
fixture albedos (`3928B58DC87F4702`, `5BC48BBB303398E3`, `60CCC35DE6ED42B7`, `08865794B7B59AEC`,
`57720018CA20525D`, `5A55210D7739C716`, `38C858E6DC48E488`) have a floor that **never drops below 1.81**.
No overlap. **Exclude `0721D150DF278E7D` and `86885A0E60751491` from any albedo rule** — they appear at
both 0.09 and 2158 units.

The warp is **not** in the vertex data: `|corr(ext, eye_dist)| ≤ 0.22`, and `5BC48BBB303398E3` holds
`ext` to 0.331..0.333 (0.6%) over 20 draws at a fixed 180 vertices. It is in the recovered basis, which
deviates anisotropically up to **±0.28%** frame to frame with `ref=anchor_prev` on 51/65 picks.

**Pre-registered test with its baseline:** measure `basis` spread on picks of (`830d7d1b9681c475`,
`5BC48BBB303398E3`) — raw `ext` constant to 0.6%, so any deviation is pure gauge error. **Baseline
±0.28%.** If a freshness fix does not bring it below **±0.05%** and does not lower `gauge_prev`, the
anisotropy is in the anchor program's own matrix rather than its age — stop and attack
`ad7ce9d672a0bf6b`.

---

## Priority 6 — frame time: `audit` IS the winner, but `DRAWAUDIT=0` is NOT safe. Take it next round.

Round 33's pre-registration resolved: MEASURED `frame_ms=35.22 | draw=33.20 (ui=2.28 mesh_create=0.26
tex_bind=0.53 uv=3.48 draw_instance=0.43 decode=6.35 audit=11.35 hash=3.76 xform=0.19 rest=4.58)`.
`audit` **is** the largest new child at **32.2% of the whole frame**, `rest` has fallen to 13%, and
`rest` is not `0.00` beside a large `draw`, so the double-counting tripwire did not trip.

**But `DRAWAUDIT=0` was armed and then reverted, and the reason generalises.** The standing safety
argument — *"`wext_refused=0`, so the gate never fired and skipping the audit cannot change what is
submitted"* — is **wrong**, because `wext` is not the only thing the audit feeds. `m_streak_measured` is
written **only** inside `audit_world_extent`, so with the audit off it stays `false` all session, and:

- the `SKIPEXTENTVP` refusal gate reads `&& m_streak_measured` and **stops firing** — and the launcher
  arms `SKIPEXTENTVP=57A12323F22F4988`, which is **one of the three near-depth viewmodel programs**. That
  changes what is submitted for the exact population priority 1 measures. Arming both in one run would
  confound it.
- `extent_plausible = !m_streak_measured || ...` becomes unconditionally **true**, so `GUESTLIGHTEXTENT`
  stops rejecting anything.

**`RemixTransforms.h`'s "What 0 costs" list names neither. Correct that doc before anyone arms 0 again.**
**Round 35: take the ~11 ms, either by arming `DRAWAUDIT=0` with `SKIPEXTENTVP` blanked so the
interaction is explicit, or by attacking `decode` (6.35 ms) — round 33 already named that fix: cache
decoded positions per (source pointer, count).** `hash` is third at 3.76 ms, `uv` fourth at 3.48.
`xform` is 0.19 ms, so **`per_draw_transform`'s `mat4_invert` is not the frame-time story — retire that
hypothesis.**

**General lesson: "counter X is 0, therefore removing the code that computes X is free" only holds if X
is that code's ONLY consumer.** Grep every reader of every intermediate the block writes, not just the
counter named in the knob's doc.

## Two open items from the uncommitted round-32 work, found in review, NOT round 34's

1. **The `xform` timing span encloses `deferred_instance`.** `per_draw_transform` calls
   `capture_gauge_anchor` → `write_gauge_anchor` → `flush_deferred_for_anchor` → `submit_deferred`, which
   adds to `m_timing.deferred_instance` from inside the `xform` scope. `rest` stays a valid residual
   (`deferred_instance` is not in the subtraction), but round 32's claim that `xform` is *"the only child
   whose cost is INVARIANT to vertex count, which makes it the discriminator"* is **false the moment
   `DEFERPREANCHOR=1`** — precisely the run it was staged for. Inert today: the flush early-returns on an
   empty buffer.
2. **`SUNSPRITEHOLD` is still armed at `999999` against the new ceiling of `216000`.** The knobs line
   will report `sunspritehold=216000` and reproduce the exact reported-vs-armed mismatch round 32's fix
   was written to end. **One launcher edit**, and it is the third instance of this trap.

---

## Priority 7 — two conf-file items for the user, not for us

1. **`rtx.fallbackLightMode = 0`** (currently `1`, NoLightsPresent, in `bin\rtx.conf`). Round 33 asked
   for this and it was not done. It eliminates the runtime's own fallback distant light as the candidate
   for "the sun reverts". One line, no rebuild. `bin\rtx.conf` is the user's live tagging state and is
   read-only to these rounds.
2. **Which `rtx.conf` does the runtime read?** `rtx.viewModel.enable = True` is set only in
   `bin\rtx.conf`; `bin\remix\rtx.conf` (beside the DLL) contains just `rtx.useNewGuiInputMethod = False`
   and `rtx.showUI = 0`. The evidence says `bin\rtx.conf` is live (the dev menu writes it, and the user's
   texture tagging in it demonstrably works), but nobody has confirmed it directly and gate 1 of
   `createViewModelInstances` depends on it. **One grep of a fresh run's Remix log for the conf path it
   loads would settle it**, and it is a prerequisite for judging priority 1.

## Runtime swap — the user's call, stated with the risk

`bin\remix\d3d9.dll` (`16A0B512F33EBB66`) and `dxvk-remix-numos3\_output\d3d9.dll`
(`F75A70D76B850829`) are the **same code** except one function: `_output` has the
`if (bd == NULL) return 0;` guard in `ImGui_ImplWin32_WndProcHandler`, which is the fix for the Remix
ImGui WndProc crash affecting every Remix game. The deployed DLL does **not** have it. Both are API
0.1000.1; no ABI or interface-slot change; `toRtDrawState` is byte-identical. Costs: `_output` came from
a **dirty** tree so the commit alone does not reproduce it (identify by SHA-256), `vendored_runtime_fnv1a`
would need to become `0x5c5478cd184f4b0a`, and `_output\d3d9.pdb` should be copied alongside it —
**`bin\remix\d3d9.pdb` is currently `.bak-0729`'s PDB (GUID `04C3AFFD…` Age 24) and will symbolize the
deployed runtime with WRONG function names.** Nothing was deployed this round.

---

## Play-test card

Launch as usual (`launch-haze-remix.cmd` → `bin\rpcs3-next.exe`, `6F1C4AA0FDC571DD`).

| what to look at | where | one-line revert |
| --- | --- | --- |
| Arms/weapon: present? oriented right? | on screen; then `Remix vmbasis:` in `bin\log\RPCS3.log` — read `cdist_pre` (want ~0.4) and `tagonly=1` | `set "RPCS3_REMIX_VMTAGONLY=0"` |
| Dev menu → camera panel `VIEWMODEL` row shows real values, not `-` | Remix dev menu | same as above |
| White/invisible walls in the plant | `Remix static-index:` — `peak` (want < 32), `dropped`/`rebuilds`, final-line `resident`/`entries` | `set "RPCS3_REMIX_STATICINDEXBUDGET=8"` |
| Frame rate (expect ~28 → ~42 fps) | `Remix timing:` — `audit` should be ~0.00 | `set "RPCS3_REMIX_DRAWAUDIT=1"` |
| If the arms are visible but misoriented, THEN do step 2 | `dbasis` non-zero, `dcentre` < 1e-3 | `set "RPCS3_REMIX_VMBASIS=0"` |
| **One measurement to ask for:** Ctrl+Click your own LEGS looking straight down | `Remix picked:` in `bin\remix_dump.log`, readable while the game runs | n/a |
