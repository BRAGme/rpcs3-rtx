# Round 48 inbox — RPCS3 RTX Remix backend, Haze (BLUS30094)

Written at the close of round 47, 2026-08-29. Branch `remix-backend`.
**HEAD did NOT move this round: `7810224be` at the start and at the end.** I committed nothing, stashed
nothing, checked out nothing. (Round 46 was caught out by a mid-round commit; I re-ran `git log --oneline -3`
at the close and pinned every diff to the explicit SHA rather than to `HEAD`.)

| artefact | SHA256 (first 16) | note |
| --- | --- | --- |
| `bin\rpcs3.exe` | **`AC7245342F3E9627`** | built this round, MSBuild `Release\|x64` exit 0, **0 errors** |
| `bin\rpcs3-next.exe` | **`AC7245342F3E9627`** | **identical — the copy was made.** This is the file the launcher starts. |
| `bin\remix\d3d9.dll` | `16A0B512F33EBB66` | **UNCHANGED.** Nothing deployed into `bin\remix\`. |
| `bin\rtx.conf` | `B24C0FA70F098F98` | **UNCHANGED**, start and end. Not tampering if it moves — the Remix runtime writes it during play. |

Only warning from anything under `Emu\RSX\Remix\` is the pre-existing C4723, moved by this round's
insertions from `RemixGSRender.cpp:11674` to **`:11799`** — **zero new warnings.**
Full derivations in `docs\remix\KNOBS.md`, **"Round 47"** (line 1160).

**Launcher line numbers (re-grepped this round, never trust a quote):** `STATICINDEXBUDGET` `:405`,
`DEFERPREANCHOR` `:1103`, `DEFERPREVONLY` `:1127`, `DEFERVIEWMODEL` `:1205`, `PROJSPLIT` `:2370`,
`PROJSPLITERR` `:2374`, `VMTAGONLY` `:3746`, `MESHIDLE` `:432`, `MESHCAP` `:434`,
`cd /d "%~dp0bin"` `:5095`, `start "" "rpcs3-next.exe"` `:5096`.

---

## READ THIS FIRST — round 46's headline knob was never running

`DEFERVIEWMODEL=1` and `DEFERPREANCHOR=1` were both armed for the whole round-46 play-test and
**`defer_viewmodel` read 0** against `vm_tagged = 148,913`. Round 46's change was correct as far as it went.
A **second** cancellation, 610 lines downstream, threw the entire population back out — and round 46's own
comment had listed it as one of "the two cancellation sites" without ever measuring it.

**MEASURED, from the round-46 play-test (`RPCS3.log` survived this time; dump slice at byte 1,260,857,444,
`build=Aug 29 2026 03:57:52`, 52,208 flips):**

| reading | value |
| --- | --- |
| `proj_split_applied == vm_tagged` | **814 / 814** `Remix live:` samples, **0 / 813** consecutive-delta mismatches |
| `Remix tail-rescue: outcome=projsplit` carrying `age=1` | **281 / 281** — not one rescue had a fresh anchor |
| `gauge_prev − defer_buffered − proj_split_applied` | **constant 1,725** → this one line is **98.85%** of the candidate→buffered shortfall |
| `defer_viewmodel` and `defer_spilled` | **0 and 0 together** |

> **That last row is the shape to remember. Two zeros in a block whose arms partition the outcomes means the
> population never ARRIVED — not that the block rejected it. It sends you upstream instead of re-auditing
> the counter.**

The cancellation sat two lines below its own refutation: the branch computes
`m_ref_pick_source = (cross_age == 0) ? anchor : anchor_PREV` and then cancelled the deferral unconditionally,
on a comment claiming the draw had been "placed by an anchor of its own pass shape, which is exactly what the
pre-anchor deferral holds draws waiting for".

**And the obvious fix would have been a regression.** `flush_deferred_for_anchor` rebuilds with the PLAIN
division `fused * anchor.inverse` — and the tail-rescue ladder runs *only at the exit where that division has
already been rejected* (MEASURED residue 2.96 .. 3.14 against `tol = 0.02`). Deleting the cancellation without
touching the flush would have handed every one of these draws back the residue the rescue removed, and
`mat4_is_finite` would not have caught it. **Read the consumer before routing a new population into it.**

---

## PLAY-TEST CARD

### A. `RPCS3_REMIX_DEFERVIEWMODEL=1` (launcher `:1205`) — the code fix, no new knob

The flush now **re-crosses** a proj_split draw against the anchor that just landed —
`(V_anchor × P_draw)^-1` — instead of re-dividing it. It reproduces **all three** parts of the ladder's
acceptance through the ladder's own accessors: the split gate (`proj_split_max_error()`), the **`vdelta`
refusal** (`proj_split_view_delta_max()`), and `is_affine`. The `vdelta` half was missing from the first
draft and was added after review: it is **inert at the shipped `PROJSPLITVDELTA=0`, which is exactly why no
run would ever have found the omission**, and the launcher stages arming it as the next test of proj_split's
premise — at which point the flush would otherwise have installed crosses the ladder itself refuses.

**The change cannot lose geometry** — because both views come out of `try_split_once` on an **orthonormal**
basis, so `fused * cross^-1` is rigid and cannot be non-affine; and if the split, the `vdelta` gate or the
invert refuses, the draw keeps the transform it already carries and is still submitted. There is no drop arm.

**Do NOT read the re-run `is_affine` gate as the safety net — an independent review of this round's diff
proved it inert.** Round 19's own note at `RemixGSRender.cpp:18500-18503` says so in as many words (*"rigid,
hence affine, hence accepted, ALWAYS … the gate is not selective, it is inert"*), which is why
`proj_split_refused` reads 0 in every run; and `is_affine` tests only column 3
(`RemixTransforms.cpp:9493-9501`), which in this convention reduces the flush's test to the identical one the
ladder already passed. **Consequence for reading the counters: `kept` can only ever count a split/invert/vdelta
failure, never an affine refusal, and `fresh` means "was rebuildable", NOT "improved"** — a stationary camera
re-crosses to a numerically identical placement and is still booked `fresh`.

| reading | where | meaning |
| --- | --- | --- |
| **`defer_projsplit=<held>/<fresh>/<kept>`** | `Remix live:`, `remix_dump.log` | **THE pass/fail.** `held` > 0 says the cancellation is no longer firing. `held == 0` with both knobs armed means a **THIRD** gate exists downstream — and check `defer_spilled` at the same time: 0/0 again means it still never arrives. |
| `fresh` vs `held` | same | how much of the held population the flush could actually re-place |
| `defer_viewmodel=`, `vm_op=c/d/s/r` | same | `declined` **must stay 0**; `replayed` should now be non-zero for the first time |
| **the eyes** | — | **does the gun keep up with camera movement.** That is the user's own words for the residual and it is what this fixes. Also: the arms should not TELEPORT or DOUBLE — that is this change's own failure mode and nothing else in the build can cause it. |

**`fresh + kept < held` is EXPECTED and is not a leak.** Two structural reasons: draws whose anchor never
lands leave through `flush_deferred_at_flip` and touch neither counter; and the ladder's anchor lookup ignores
`surface_offset` while the flush matches on it, so the flush is strictly the narrower match.

**DO NOT RAISE `VMBASIS` WHILE THIS IS ON.** Every proj_split draw is also viewmodel-tagged, so the buffering
site's `is_viewmodel && !vm_op_valid` arm would **spill** all 148,913 of them. The decline gate is
`VMROTAXIS < 4 && VMROTPIVOT == 0 && (VMBASIS == 0 || VMBASISPIVOT == 0)`. The launcher arms
**`VMBASISPIVOT=1`** — harmless only because `VMBASIS=0` satisfies the disjunct. The tell would be
`defer_spilled` climbing, not `defer_projsplit`.

**Four more things the diff review caught that change how you READ things, not what the build does:**

1. **`Remix tail-rescue: outcome=projsplit … world=<matrix>` now prints the STALE cross for any draw the
   flush later re-crossed** (`:18646` formats `format_matrix(retry)` at the ladder). Round 19 added that field
   precisely because *"world= is the placement"* — for the `defer_projsplit_fresh` population it no longer is,
   and **no census line prints the matrix that actually shipped.** `++proj_split_applied` likewise stops
   meaning "submitted with this cross". The `proj_split_applied == vm_tagged` identity still holds, so the
   root-cause measurement survives; the *placement* reading does not. **If you need to judge the re-cross by
   matrix, the census has to move into the flush — that is a real, small change and the only way to see it.**
2. **`kept` is not bit-exact round 46 for that draw.** The *transform* is identical, but round 46 submitted
   these immediately, in frame order; they now go out at the anchor flush or at flip. Submission **order**
   changes for the whole proj_split population, on both arms.
3. **`defer_viewmodel` is gated on `has_vm_op`, not on "is a viewmodel draw".** At `VIEWMODELCAM` 0 or 1 the
   operator block never runs, so a buffered viewmodel draw would leave `defer_viewmodel` at 0 while
   `defer_projsplit` counted alone. Inert at the shipped `vm_mode=2`; do not compare the two across a
   `VIEWMODELCAM` change.
4. `world = mat4_multiply(entry.object_space, world)` runs even when `world_built` is false, on a deliberate
   zero matrix that is never consumed. Harmless and commented, but a trap for any future edit that adds a
   consumer below the guard.

**REVERT: `set "RPCS3_REMIX_DEFERVIEWMODEL=0"`** — one line, round 46 bit-exactly.

### B. `RPCS3_REMIX_STATICINDEXBUDGET=512` (launcher `:405`) — carried in from the round-47 brief

**Judge it on `dropped`. `peak` CANNOT judge it and the round-47 brief's test was not runnable.**

`m_static_index_rebuilds` is only incremented inside `if (... rebuilds < budget)`, so `rebuilds <= budget`
always and `peak = max(peak, rebuilds)` is a run-cumulative max that never resets. **`peak <= budget` is a
structural invariant.** Over 52,208 frames a single spiking frame pins it. `peak == budget` is a tautology the
moment the cap is touched once — it cannot refute anything.

MEASURED baseline at `budget=128`, from 774 census lines ~67 flips apart:
`dropped = 5,521` over 52,208 flips (**0.106/frame**, 0.069% of the 153.3 draws/frame submitted);
**only 39 of 773 intervals had any drop**; largest burst **1,303 in one ~67-frame interval ≈ 19.4/frame
sustained**; `stale=62`, `nomesh=0`, `resident/entries = 3,809/13,997` (**72.8% evicted**).

**The run average of 0.106/frame is the wrong statistic and it nearly refuted the hypothesis wrongly.** The
drops are bursty: 95% of the run has none and the worst window loses ~19 draws per frame for two seconds.
That is easily enough to hide a door, and the 5%-of-the-run pattern matches the user's *"only in the copper
plant level"*. **The bulb/door hypothesis SURVIVES.**

* `dropped`/flip falls ≥80% from 0.106 → **the budget was the binding constraint.**
* `dropped`/flip stays within ~±30% → **the budget is NOT the constraint.** 512 is the clamp ceiling
  (`RemixTransforms.cpp:10824`, `clamp(env_u32(...,4), 1u, 512u)`) so there is no headroom left, and the
  launcher's own round-34 note at `:400` already names the successor: *"the answer is the reaper, not the
  budget: MESHIDLE=600 is what evicts them, and the untaken lever is a longer idle window scoped to
  static-index UNION meshes only, which is a code change and NOT this knob."* `MESHIDLE=600` is armed at
  `:432` (default 300); `evicted/entries = 72.8%` is the supporting reading.
* Cost: a saturated frame can mint 512 union meshes instead of 128. At `mesh_create = 0.25 ms` for
  `44.7 creates/frame` (≈5.6 µs each) that is **+2.15 ms in a saturated frame** — a hitch, not a sustained
  cost. Watch `mesh_live` (46,649 last run) for growth; `MESHCAP=0` means no ceiling.

**ATTRIBUTION.** Two things changed. They share no counter — `defer_projsplit` can only move with A,
`Remix static-index: dropped=` can only move with B — and they have separate pixel tests (gun tracking vs
the bulb/door). **The one confound is frame rate.** If fps regresses and you must attribute it, revert B
first: it is one line and its counter is independent.

---

## PRIORITY 1 for round 48 — the frame cost, and it is the diagnostics

`Remix cost:` and `Remix timing:` were both readable this round. From the last `Remix timing:` (49 frames):

```
frame_ms=41.56 | flip=7.24 | draw=33.30 (ui=2.45 mesh_create=0.25 tex_bind=0.58 uv=2.98
  draw_instance=0.45 decode=5.57 audit=10.14 hash=3.06 xform=0.31 rest=7.50)
  | deferred_instance=0.03 | other=34.33
```

**`deferred_instance = 0.03 ms` of 41.56 — 0.07%. The deferral is free**, exactly as round 46 predicted, and
`xform = 0.31` (which *encloses* `deferred_instance` — never add them) confirms it. Round 32's
"`xform` is the discriminator because it is invariant to vertex count" stays false at `DEFERPREANCHOR=1`.

**`audit = 10.14 ms` is 24.4% of the whole frame and 30.5% of `draw`. It is the largest named child of
`draw`, larger than `decode` and `rest`.** The user reports 48 → 30.7 fps in the plant, 27.2 in the nectar
room, 21.3 on the stairs. **Unlike every other lever in this file, cutting the diagnostics carries no visual
risk at all** — nothing in `audit` places a draw. That is the round-48 item: find what `audit` covers, and
gate the per-draw parts behind the censuses that actually consume them.

## PRIORITY 2 — `DEFERPREVONLY=0` (launcher `:1127`), still staged, still unjudged

Unchanged from the round-47 brief and still the right next knob after A is judged. `ref=camera` is 19.1% of
trace samples, displaced 100% of the time, 68.5% of it 32–128 units out. `defer_absent_declined = 282,813`
over 52,208 flips = **5.4 draws/frame** added to a population currently at 22.0/frame.

**Fresh support:** the light-bulb ray the user clicked, `88046E3F4F28137C`, reads
`areason=no ref=camera anchor_vp=0000000000000000` — no anchor of any kind. It is in exactly this population.

**Ship it only after A is judged. One knob per round; two shipped together were both reverted.**

---

## Priority-3 picks — resolved, with one correction that changes what to try

All eight new picks resolved in the run. **None belongs to the proj_split family** (`830d7d1b9681c475` /
`f39f504649b6f442`), so item A does not touch them.

* **`6575ACE3A42A78E6` (the solid yellow block) reads `areason=chain`, NOT `wdivide`** —
  `vp=6f76ab0ad8d926b1`, `extent=0.85`, `blend=1`, `depth_write=0`, `sampled=0x1`, `material=1`, `ref=anchor`.
  It is a **no-matrix-chain-into-HPOS** refusal, a different family from every other pick in the list, which
  all read `wdivide`. **The carried suggestion to grep the UI compositor's `have_uv ? entry : nullptr` is
  testing the wrong family** — a material *was* resolved. Test against `areason=chain`.
* `88046E3F4F28137C` — see Priority 2.
* The other six all read `areason=wdivide material=1` with `ref=anchor` or `identity-bypass`, i.e. **placed
  exactly** by the round-46 cross-tab (`ref=anchor` ⟹ residue 0.000 on 5,622 of 5,622). Whatever is wrong
  with them, it is not placement.

---

## Carried, unchanged

* *"At a very small angle, the scene goes haywire with swinging"* — still uninvestigated. Needs a
  world-geometry instrument binned by camera yaw, which does not exist.
* The shake is **level-specific** (copper plant only, per the user). Not explained.
* Low-resolution textures `01C09BE5851AD069`, `AFE89AA51E682B49`. Smoke and the nectar gas grenade still do
  not render although fire does.
* `guest_lights=0 guest_light_match=0 mat_emissive=543` — `GUESTLIGHTALBEDO2` still blank, `GUESTLIGHTAUTO`
  still 0 with both known faults unfixed. Do not re-enable.
* ADS collapse root-caused in round 43, not fixed. `UIWIDTH` still 1280; the user has not chosen.
* **"White triangular spikes on an NPC" are HUD damage indicators**, rendering correctly.
* **The `Remix albedo-trace:` and pick `ref=` fields still sit UPSTREAM of the deferral.** A draw the deferral
  fixes still prints `ref=anchor_prev`. Do not pre-register a trace reading to judge a deferral knob — round
  45 did and it was unrunnable by construction.
* **`gauge_contested = 0` is not a property of this title** (round 46 retraction). Never cite it.
* **`anchor_parked/recap/promoted` 8/0/8 is not an invariant** — compare rates, not absolute counts. This run:
  1,588/1,557/31 over 52,208 flips.

---

## Tooling left behind, in `...\scratchpad\r47\`

* **`run47.log`** — the 51.9 MB slice of the round-46 play-test, from `bin\remix_dump.log` byte offset
  **1,260,857,444** (`build=Aug 29 2026 03:57:52`, `flips=52208`, `deferpreanchor=1 deferviewmodel=1`).
* **`si.txt`** — the 774 `Remix static-index:` lines pulled from `bin\log\RPCS3.log`, which is what the
  burst analysis differences.
* **`corr.py`** — the counter-identity cross-check across every `Remix live:` line. This is the instrument
  that produced 814/814 and 0/813; **the consecutive-delta check is what makes it proof rather than
  coincidence** and it is worth reusing on any two counters that look equal.
* **`posaudit.py`** — format-string audit **by position**, not by count. Prints the args either side of a
  named field so an insertion can be checked against the anchors the format string uses.
* **`fmtdiff.py`** — diffs the `Remix live:` ARG LIST between two versions of the file. Point it at
  `git show <SHA>:...` output to see exactly which counters a round added and where.
* **`build47.log` / `build47b.log`** — the two MSBuild runs.
* **`KNOBS.bak47.md`** — the pre-insert copy. The doc edit was verified **+227 / −0, insert-only**.
* `..\r45\fmtaudit45.py` still works: **109 `fmt::format` calls, 0 mismatches** after this round's edits.
  **`scratchpad\r43\audit.py` gives false passes. Do not use it.**
* `..\r46\haywire.py` — the viewmodel `cpre`-vs-turn-rate instrument. **Run it first on the next play-test**:
  the round-46 baseline is a monotone 32x dose-response, `0.0035` at 0–0.05 deg/frame rising to `0.1134`
  above 4 deg/frame. If item A works the slope flattens.

## Instrument notes worth keeping

* **INSTRUMENT BLINDNESS #7: a counter clamped by the quantity it is compared against.** `peak <= budget` is
  an invariant, so `peak == budget` is a tautology. Seven instruments have now been found structurally blind.
* **`defer_X = 0` AND `defer_spilled = 0` together means the population never arrived.** When a block's arms
  partition the outcomes, all-zeros points upstream of the block.
* **Read the consumer before routing a new population into it.** The flush's divide was the one that already
  failed for this population.
* **A run average hides a bursty defect.** 0.106 drops/frame reads as negligible; the worst window is 19.4.
  Difference the cumulative census before calling a rate small.
* **Grep for a condition that is computed and then not used by its neighbour.** `cross_age` decided the
  provenance on one line and was ignored by the cancellation two lines below it.
* **A comment enumerating where a thing can go wrong is a checklist, not a reassurance.** Round 46 wrote down
  both cancellation sites and measured neither.
* **`bin\log\RPCS3.log` survived this time** (65 MB, not truncated) — but only because no second launch
  happened. `remix_dump.log` is still the only durable record; it is ~1.31 GB and append-mode. Find run
  boundaries with `tail -c 200000000 remix_dump.log | grep -abo "Remix run-start:"`, add the tail offset,
  then `tail -c +N` once into the scratchpad.

## What the diff review found — read this before writing round 48's code

The round-47 diff was reviewed against the source before deploying, the way round 46's was. **Two MAJOR
findings, and both were invisible to any run at the shipped config** — they came from reading two sites side
by side, not from a measurement:

1. **The flush was missing one of the ladder's three acceptance conjuncts** (`PROJSPLITVDELTA`). Fixed and
   rebuilt. It is disabled at the shipped value, so **no play-test could ever have caught it** — and the
   launcher stages arming that exact knob next.
2. **The re-run `is_affine` gate is provably inert**, so the round's stated safety argument was wrong even
   though its conclusion (no draw is lost) survives on the rigidity of the construction. Round 19 had
   written the proof down 30 lines from the ladder.

> **When you copy an accepting condition, copy ALL of its conjuncts — and the one you will drop is the one
> that is inert today.** Round 46 shipped the same shape of defect (`Op` not replayable at three
> reachable-but-unshipped knob values). Diff the conjunction, not the outcome.

> **A gate copied from an accepting path is not a check.** Grep the gate you are about to rely on for a note
> saying it never fires.
