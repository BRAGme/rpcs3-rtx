# Round 47 inbox — RPCS3 RTX Remix backend, Haze (BLUS30094)

Written at the close of round 46, 2026-08-29. Branch `remix-backend`. **HEAD MOVED DURING THE ROUND:
`398f3f1ed` at the start, `7810224be` at the end.** I committed nothing, stashed nothing, checked out
nothing — see the concurrency block below.

| artefact | SHA256 (first 16) | note |
| --- | --- | --- |
| `bin\rpcs3.exe` | **`24506F89A9259722`** | built this round, MSBuild `Release\|x64` exit 0, **0 errors** |
| `bin\rpcs3-next.exe` | **`24506F89A9259722`** | **identical — the copy was made** |
| `bin\remix\d3d9.dll` | `16A0B512F33EBB66` | **UNCHANGED.** Nothing deployed into `bin\remix\`. |
| `bin\rtx.conf` | `B24C0FA70F098F98` | **UNCHANGED**, start and end. Do not treat a change here as tampering — the Remix runtime writes it during play. |

Only warning from anything under `Emu\RSX\Remix\` is the pre-existing C4723, now at
`RemixGSRender.cpp:11674` — **zero new warnings.** Full derivations in `docs\remix\KNOBS.md`, **"Round 46"**
(line 1160).

**CONCURRENCY — THE TREE MOVED, and the correction is worth more than the event.** The user committed round
45's work as **`7810224be`** *"Remix: the teleporting props were a stale anchor, not the gauge donor"* at
**03:26:11**, between my opening `git log` and my first `git diff`. Nothing conflicted and nothing was lost —
round 45's content was already in my working tree, I only added to those files, and `git diff --numstat`
against the new HEAD shows exactly and only my changes: `RemixGSRender.cpp` +319/-4, `RemixGSRender.h` +63,
`RemixTransforms.cpp` +11, `RemixTransforms.h` +36, `KNOBS.md` +291 insert-only, `launch-haze-remix.cmd`
**+78/-0 in one hunk**.

**But it caught me out and the trap will recur.** Mid-round I concluded from `git show HEAD:` that round 45's
work was already in `398f3f1ed` and that the `M` flags were `core.autocrlf` noise. Both wrong: that reading
was taken *after* HEAD had advanced. Verified at the close —
`git show 398f3f1ed:...RemixGSRender.cpp | grep -c "refage=%d"` = **0**, `git show 7810224be:...` = **1**.
**`git show HEAD:` is not a fixed reference. Pin every comparison to the SHA you recorded at the start, and
re-run `git log --oneline -3` before writing the concurrency line.** A commit landing mid-round re-bases
every measurement after it without touching one byte of the working tree.

**Launcher line numbers shifted.** `DEFERPREANCHOR` `:1103`, `DEFERPREVONLY` `:1127`, **`DEFERVIEWMODEL`
`:1205`**, `WATCHALBEDO` `:1301`, `TRACEALBEDO` `:64`, `VMTAGONLY` `:3746`, `cd /d "%~dp0bin"` `:5095`.
Re-grep, never trust a quoted line number.

---

## PLAY-TEST CARD — one knob changes pixels

### `RPCS3_REMIX_DEFERVIEWMODEL=1` (launcher `:1205`)

`DEFERPREANCHOR=1` fixed the **world** last round and the user confirmed it (*"light fixture is now stable,
props are not moving as camera turns"*). The **viewmodel** did not move with it, and this round found why: the
pre-anchor deferral has an explicit exclusion for viewmodel draws, and it is gated on the **tag** rather than
the **placement**.

```cpp
if (viewmodel_draw)  { defer_candidate = false; }     // round 45 and earlier
```

Its comment argues *"a viewmodel draw is placed relative to the weapon camera, so no world anchor can make it
more correct."* **`VMTAGONLY=1` — this title's shipped value — deletes that premise.** Round 34 already
reached the same conclusion for the twin gate ~130 lines below (the tail-rescue ladder) and moved that one to
`viewmodel_place`. This is the sibling it left behind.

| reading | where | meaning |
| --- | --- | --- |
| `vm_op=<captured>/<declined>/<singular>/<replayed>` | `Remix live:`, `remix_dump.log` | `captured` sizes the viewmodel population on **any** run (~2.3/frame). **`declined` MUST be 0** at the shipped `VMROTAXIS=1 VMROTPIVOT=0 VMBASIS=0` — non-zero means one of those moved and the operator stopped being replayable. `singular` ~0. `replayed` is bounded above by `defer_viewmodel`. |
| `defer_viewmodel=` | `Remix live:` | **THE ROUTE.** Viewmodel draws **actually buffered**, counted at the buffering site. **Structurally 0 unless `DEFERVIEWMODEL=1` AND `DEFERPREANCHOR=1`.** 0 with both armed means the diagnosis is wrong, not the plumbing. |
| the eyes | — | does the gun still have parkinsons; does the helmet still stretch and detach in the nectar room |
| `Remix cost:` | **`remix_dump.log`** (NEW) | `deferred_instance=` against `frame_ms=`, plus `rest=`. See Priority 2. |

**PRE-REGISTERED REFUTATION, and it is a real number, not a vibe.** `Remix vmbasis:` carries `cpre=` — the
viewmodel centroid on the camera's own (right, up, fwd) axes, which for a rigidly-attached viewmodel is a
constant. Binned by camera turn rate over 3,293 samples of the round-45 run:

```
turn deg/frame   n     median error
  0   - 0.05    432      0.0035
  0.5 - 1       211      0.0671
  > 4           104      0.1134        <- 32x the slowest bin
```

**A clean monotone dose-response, which is exactly what a one-frame-stale placement predicts and what real
weapon animation does not.** If the knob works, the slope flattens and the `> 4` bin falls toward 0.0035. If
the slope is unchanged, the stale anchor is not the viewmodel wobble — revert. Analyser:
`...\scratchpad\r46\haywire.py` (point it at the new run's slice).

**Do NOT judge it on raw `cpre` variance.** A full one-frame-stale model removes only 21.9% of that variance,
because `cpre` also contains the weapon's genuine animation — the 15 worst samples of the run are frames
26,306–26,320, error growing 1.26 → 2.74 units while the camera is nearly stationary. That is the gun
animating. Binning by turn rate is what separates the two.

**The operators are preserved, not dropped.** The flush rebuilds the transform and never re-ran
`apply_viewmodel_basis` / `apply_viewmodel_rotation` — MEASURED, `dbasis=0` on all 3,293 census lines
(`VMBASIS=0`, inert) but **`drot = 1.46..1.99` units on every one**. The submit site now captures
`Op = post * pre^-1` and the flush replays it. `flush_deferred_at_flip` does not rebuild the transform, so the
operator can never be applied twice. **If the gun TELEPORTS or doubles, the replay is composing wrongly** —
that is this knob's own failure mode and nothing else in this build can cause it.

**DO NOT MOVE `VMROTAXIS`, `VMROTPIVOT` OR `VMBASISPIVOT` WHILE THIS IS ON.** The replay is exact only while
both operators are placement-independent world-space LEFT-multiplies. `VMROTAXIS >= 4` switches
`apply_viewmodel_rotation` to a MODEL-space RIGHT-multiply (`M' = M*R`, its own comment says so), and
`VMROTPIVOT`/`VMBASISPIVOT` != 0 derive the pivot from the placement itself. The backend **refuses the
capture** at those settings rather than replaying a wrong operator — those draws take the immediate path and
land in `vm_op` `declined` — so nothing breaks, but the knob quietly stops working for them.

**REVERT:** `set "RPCS3_REMIX_DEFERVIEWMODEL=0"` — one line, round 45 bit-exactly.

---

## READ THIS FIRST — a fresh anchor is not "better". It is EXACT

`Remix albedo-trace:` on `E40BF80AF519848A`, **9,544 continuous samples, camera free**, cross-tabbing the
translation residue against `ref=`:

| `ref=` | draws | share | residue < 0.01 | max residue |
| --- | --- | --- | --- | --- |
| `anchor` (refage=0) | 5,622 | 58.9% | **5,622 / 5,622 = 100%** | **0.000** |
| `anchor_prev` (refage=1) | 2,099 | 22.0% | **0 / 2,099** | 393.97 |
| `camera` (refage=-1) | 1,823 | 19.1% | **0 / 1,823** (68.5% land 32–128 units out) | 184.26 |

**Perfect separation both ways.** Two consequences:

* **Round 34's ±0.28% anisotropic basis error is not a world placement defect.** Over the same 9,544
  `matrix=` fields the median orthogonality residual and median anisotropy of the world path are both
  **exactly 0.000000**. The viewmodel's are `0.001423` / `0.001537` and are **never** zero. Stop looking for a
  second error source on the world path: there is not one.
* **The entire remaining world defect is one sentence: 41.1% of draws never get a fresh anchor.**

Also settled: every one of the 19 picks. Viewmodel `830d7d1b9681c475` **4/4 `ref=anchor_prev`**; world and
props **15/15 `anchor` or `identity-bypass`, 0 `anchor_prev`** — where round 45 read the fixture 5/5
`anchor_prev` and round 4 read it 9/9. That is the user's split, in the instrument.

---

## PRIORITY 1 — the next knob is already sized, and its argument is now inverted

`RPCS3_REMIX_DEFERPREVONLY=0` (launcher `:1127`). Round 32 excluded the `gauge_absent` population from the
deferral on a **cost** argument: *"holding an absent-source draw bets on an anchor appearing for a source that
has not produced one in two frames."* MEASURED this round, that population (`ref=camera`, 19.1% of trace
samples) is **displaced 100% of the time, and 68.5% of it lands 32–128 units out** — far enough to empty a
stairwell. `defer_absent_declined = 352,051` over 44,375 flips = **7.9 draws/frame** added to a deferral
population currently at 19.7/frame (+40%), which is exactly round 31's 27.6.

**Pre-registered:** `ref=camera` collapses on the trace and the exact share rises from 58.9% toward ~78%. If
`defer_flip / defer_buffered` jumps from **0.88%** toward 29%, the absent branch genuinely has no anchor
coming, deferring it buys nothing, and round 32's exclusion was right — set it back.

**Ship this only after `DEFERVIEWMODEL` is judged.** One knob per round; two shipped together were both
reverted.

---

## PRIORITY 2 — the frame cost, and the instrument that now exists to price it

**The round-45 play-test's `deferred_instance=` was DESTROYED and this is not a metaphor.** `Remix stats:` and
`Remix timing:` go only to `bin\log\RPCS3.log`, which rpcs3 **truncates on every launch** — and the user
launched rpcs3 again four minutes after the play-test to install a DLC PKG. That log is now 14,991 bytes of
PKG installer. `RPCS3.log.gz` is the same session. **0 `Remix timing:` lines in the 45.4 MB dump slice.**

**FIXED:** a new **`Remix cost:`** line is appended to `remix_dump.log` beside the timing notice —
`frames frame_ms draw draw_instance deferred_instance rest other | defer/frame buffered fresh flip vm`.
Twelve arguments, audited. **This is the first run in which the deferral's cost is actually measurable.**

What is already known, MEASURED from `Remix live:` of the round-45 run:

```
defer_buffered 874,746   defer_fresh 867,086 (99.1%)   defer_flip 7,660 (0.88%)   defer_spilled 0
```

`defer_flip / defer_buffered = 0.88%` against round 32's own **< 72%** threshold — **passed by 82x**. Buffering
19.7 POD structs a frame cannot plausibly cost the ~17 fps the user reports (48 → 30.7 in the plant, 27.2 in
the nectar room, 21.3 on the stairs). **Read `deferred_instance=` against `frame_ms=` FIRST**, and if it is
near zero, the cost is not the deferral but what it exposes: read `rest=`, which round 31 measured at
**40.96 ms/frame = 77% of the whole frame** with the nine named children accounting for the remainder.

**READ THE LINE CORRECTLY — `xform` ENCLOSES `deferred_instance`.** Round 34's review logged this and it has
been inert until now, because the flush early-returns on an empty buffer and `DEFERPREANCHOR` was 0.
`per_draw_transform` -> `capture_gauge_anchor` -> `write_gauge_anchor` -> `flush_deferred_for_anchor` ->
`submit_deferred` all run inside the `xform` timing scope. So **`deferred_instance` is a SUBSET of `xform`,
never a sibling — do not add it to `draw` to form a total.** True transform math is `xform - deferred_instance`.
`rest` and `frame_ms` are unaffected, so the headline `deferred_instance` vs `frame_ms` comparison is sound as
printed. Round 32's *"`xform` is the only child invariant to vertex count, which makes it the discriminator"*
is **false at `DEFERPREANCHOR=1`** — false in exactly the run it was staged for.

**Round 34's other open review finding is STALE — do not act on it.** It says `SUNSPRITEHOLD` is armed at
`999999` against a ceiling of `216000`. MEASURED this round: the launcher sets it to `216000` at `:5023` and
the run-start banner reads `sunspritehold=216000`. **Armed == reported. Already fixed; the note outlived the
defect.** (Checked precisely because a twelve-round-old "still open" item is a claim with an expiry date.)

---

## PRIORITY 3 — geometry loss: the floor is SUBMITTED; it is placed wrong

`WATCHALBEDO=4094F22DB2A8278A` (the checkerboard tile, the clickable neighbour of the void), 24,927 lines:

```
submitted 24,412 (97.9%)      world_refused fail=nocam 515 (2.1%)
```

Not a refusal. But its provenance is: **50.2% `anchor` / 40.0% `anchor_prev` / 7.7% `camera`**, and **three
whole programs are 100% stale for this albedo** — `f39f504649b6f442` (5,368 draws), `af06f6d32ec048ee`
(2,315) and `830d7d1b9681c475` (1,347), every single draw `anchor_prev`. By the table above those are
displaced 100% of the time.

`Remix worldid-census:` cumulative, buckets `t = <=1 / <=32 / <=128 / >128`: `ad7ce9d672a0bf6b` 2,229,547
draws `t=1,753,910/394,983/882/79,772` (**3.58% beyond 128 units**, tmax 2276.61); `c1d482dcd1b03ed0` 170,326
draws, **8.9% beyond 32 units** (round 45 read 10.6%), tmax 1533.32.

**So displacement is confirmed as the mechanism and Priority 1 is its remedy.** Re-measure the same watch
table after `DEFERVIEWMODEL` and again after `DEFERPREVONLY=0`.

---

## RETRACTED / CORRECTED — do not repeat these

* **`gauge_contested = 0` is NOT a property of this title.** Round 45 retired the whole donor-selection family
  on that reading over 21,630 flips. **This run reads `gauge_contested = 6,602`,
  `gauge_donor_upgrade_avail = 124`, `gauge_donor_upgraded = 124`** — `GAUGEDONORBEST=32` fired. The
  *conclusion* survives on magnitude (0.15 contested per flip; only 1.9% of contested frames had a better
  donor, so donor election is still not worth a round) but the *evidence* was scene-specific and is falsified.
  **Never cite `gauge_contested=0` again.**
* **"`anchor_parked`/`anchor_recap`/`anchor_promoted` MUST stay at 8/0/8" is not an invariant.** Those were
  absolute counts from one 21,630-flip scene. This run reads 4,766/4,744/22 = 0.107/0.107/0.0005 per flip,
  which is the round-43 band (0.0769/0.0766/0.00032) and 272x below round 44's bad run (0.1360). Compare
  **rates**, and only against a run of similar length and scene mix.
* **Both `ref=` instruments sit UPSTREAM of the deferral they were built to judge.**
  `Remix albedo-trace:` is emitted at `RemixGSRender.cpp:~21712`, the pick record at `~23590`, and the
  deferral buffers at `~23720`. A draw the deferral later fixes still prints `ref=anchor_prev`. So round 45's
  pre-registration (*"`anchor_prev` should collapse on the trace"*) was **unrunnable by construction**, and
  the trace's 58.9%-exact against round 45's 24.4% is **scene variation, not the knob**. If you want the trace
  to see the deferral, the emit site has to move below the buffering — that is a real, small change and it is
  the only way to judge a deferral knob from the trace.
* **`git show HEAD:` is not a fixed reference.** A commit that lands mid-round re-bases every diff taken
  after it. See the concurrency note at the top — a whole conclusion was drawn from post-move readings and
  was wrong.

---

## Carried, unchanged

* *"At a very small angle, the scene goes haywire with swinging"* — **not investigated.** The obvious lead
  (the viewmodel's worst samples) turned out to be the gun's own animation, not a defect; see the round-46
  KNOBS section 3b. A real probe needs a world-geometry instrument binned by camera yaw, which does not exist.
* `6575ACE3A42A78E6` still a solid yellow block (UI path, unsampled texture). The suggested check is whether
  the UI compositor resolves an albedo `entry` — grep `have_uv ? entry : nullptr`.
* Low-resolution textures `01C09BE5851AD069`, `AFE89AA51E682B49`. Smoke and the nectar gas grenade still do
  not render although fire does. Big metal pipes near the windows still teleport (same family as the fixture;
  judge them on the same run).
* `guest_lights=0 guest_light_match=0 mat_emissive=366` — `GUESTLIGHTALBEDO2` still blank, `GUESTLIGHTAUTO`
  still 0 with both known faults unfixed. Do not re-enable.
* ADS collapse root-caused in round 43, not fixed. `UIWIDTH` still 1280; the user has not chosen.
* **"White triangular spikes on an NPC" are HUD damage indicators**, rendering correctly. Nothing there.

---

## Tooling left behind, in `...\scratchpad\r46\`

* **`run46.log`** — the 45.4 MB slice of the round-45 play-test, from `bin\remix_dump.log` byte offset
  **1,202,181,926** (`build=Aug 29 2026 02:41:26`, `flips=44375`, `deferpreanchor=1`).
* **`tcross.py`** — the `ref=` x residue cross-tab that produced the headline table. `python tcross.py <slice>`.
* **`basis.py`** — orthonormality / anisotropy / determinant statistics for the viewmodel `pre=` matrices and
  the world `matrix=` fields, side by side.
* **`haywire.py`** — **the viewmodel judgement instrument.** `cpre` deviation binned by camera turn rate and
  by pitch, plus the worst-sample listing. This is what round 47 runs first.
* **`vmlag.py` / `vmlag2.py` / `vmlag3.py`** — the one-frame-lag regressions (translation-only and full
  stale model). Kept because they are the evidence that raw `cpre` variance is the *wrong* statistic.
* **`dry_new.cmd` / `dry_base.cmd` / `out_new.txt` / `out_base.txt`** — the launcher parse test. Truncate at
  **2090** (working tree) / **2031** (`git show HEAD:launch-haze-remix.cmd`), append one `echo`, run under
  `cmd /c`. Round 46's result was byte-identical to HEAD apart from `DVM=[]` -> `DVM=[1]`. **Do not truncate
  past ~2095** — pre-existing `rem` text there aborts the parse and the echo never runs.
* **`KNOBS.bak.md`** — the pre-insert copy, for diffing the doc edit.
* `..\r45\fmtaudit45.py` still works and is still self-tested: **109 `fmt::format` calls, 0 mismatches** after
  this round's edits, with `Remix live:` at 374/374, `Remix run-start:` at 135/135, `Remix cost:` at 12/12.
  **`scratchpad\r43\audit.py` gives false passes. Do not use it.**

## Instrument notes worth keeping

* **A log another launch can truncate is not a record.** Anything a play-test is judged on must reach
  `remix_dump.log`, which is append-mode. `RPCS3.log` is not durable for one minute past the session.
* **Check where an instrument SITS in the call, not only what it prints.** Five instruments have now been
  found structurally blind: pick-gated sampling, deduped censuses, the shared skip counter, a knob whose route
  had no candidates, and now two `ref=` fields emitted upstream of the mechanism they judge.
* **Perfect separation beats a large effect size.** `ref=anchor` implies residue 0.000 on 5,622 of 5,622 and
  its negation on 3,922 of 3,922 — one cross-tab that closed four rounds of competing hypotheses.
* **Bin a noisy metric by the variable the mechanism predicts.** Raw `cpre` variance said the stale-anchor
  model explains 22%; binned by camera turn rate the same data shows a clean monotone 32x dose-response.
  Weapon animation is uncorrelated with turn rate; a one-frame lag is proportional to it.
* **A comment that argues from a premise a later knob removed is a bug waiting to be found.** Grep for the
  premise, not the symptom.
* **`bin\remix_dump.log` is ~1.25 GB and append-mode.** Find run boundaries with
  `tail -c 200000000 remix_dump.log | grep -abo "Remix run-start:"`, add the tail offset, then
  `tail -c +N` once into the scratchpad and grep that.
