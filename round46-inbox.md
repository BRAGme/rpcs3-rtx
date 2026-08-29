# Round 46 inbox — RPCS3 RTX Remix backend, Haze (BLUS30094)

Written at the close of round 45, 2026-08-29. Branch `remix-backend`, HEAD **`398f3f1ed`** — unchanged all
round. Nothing committed, nothing stashed, nothing checked out.

| artefact | SHA256 (first 16) | note |
| --- | --- | --- |
| `bin\rpcs3.exe` | **`7BA23387091418C2`** | built this round, MSBuild `Release\|x64` exit 0, **0 errors** |
| `bin\rpcs3-next.exe` | **`7BA23387091418C2`** | **identical — the copy was made** |
| `bin\remix\d3d9.dll` | `16A0B512F33EBB66` | **UNCHANGED.** Nothing deployed into `bin\remix\`. |
| `bin\rtx.conf` | `B24C0FA70F098F98` | **CHANGED, NOT BY ME.** See the concurrency note below. |

The only warning from anything under `Emu\RSX\Remix\` is the pre-existing C4723 at `RemixGSRender.cpp:11636`
— **zero new warnings.** Full derivations in `docs\remix\KNOBS.md`, **"Round 45"** (line 1160).

**CONCURRENCY:** the tree did not move. HEAD `398f3f1ed` at start and at end, same seven modified files.
But **`bin\rtx.conf` changed from `215975B8697DE21B` to `B24C0FA70F098F98`, mtime 2026-08-29 02:10:18** —
four minutes before the play-test log's last write. That is inside the user's session, so it is the Remix
runtime saving its own settings, not an edit. I never opened it. **Re-hash it at the start of round 46 and
do not treat `215975B8697DE21B` as the expected value any more.**

**Launcher line numbers shifted again.** `DEFERPREANCHOR` `:1103`, `DEFERPREVONLY` `:1127`, `WATCHALBEDO`
`:1223`, `GAUGEDONORBEST` `:203`, `WORLDIDMAXT` `:156`, `TRACEALBEDO` `:64`, `GUESTLIGHTAUTO` `:1396`,
`FPALBEDONARROW` `:1829`. Re-grep, never trust a quoted line number.

---

## PLAY-TEST CARD — one knob changes pixels

### `RPCS3_REMIX_DEFERPREANCHOR=1` (launcher `:1103`)

**This is a knob that was already root-caused, already shipped, and already CONFIRMED BY THE USER to stop
the props sliding, in round 31.** It was switched off on 2026-08-16 for ~14 fps. It is back on.

It is **not** a gauge remedy. It refuses, parks, promotes, re-ranks and reorders **no donor at all**, so
round 44b's rule (*a gauge remedy must never make the gauge later or absent*) does not apply to it. It holds
the **draw** until this frame's anchor for its own render source lands, then re-divides in f64 against it.
Anything still held at flip goes out with the placement it already carried — worst case byte-identical to
round 44b, one flush later, and **no draw is ever lost**.

**Why this population:** 8 of 8 displaced placements of the teleporting light fixture — 5 measured this
round, 9/9 measured in round 4 — carry `ref=anchor_prev`, with **perfect separation** from the correct ones,
which all carry `ref=anchor`. A one-frame-stale *anchor* under a pure camera *translation* gives a pure
translation error with no rotation, which is exactly *"the camera bounces as I walk so props follow the
bounce"* and is a motion round 38's `distance x turn-per-frame` model cannot produce.

| reading | where | meaning |
| --- | --- | --- |
| `deferred_instance=` | `Remix timing:`, **`bin\log\RPCS3.log`** | **THE COST, in ms/frame, measured.** It is `0.00` today. Compare against `frame_ms=` on the same line. This is the number the 2026-08-16 revert never had. |
| `defer_flip / defer_buffered` | `Remix live:` | **must come in BELOW 72%** — round 32's own pre-registered threshold. If it is still ~half, the `gauge_absent` branch was not the waste, `DEFERPREVONLY` is refuted, and both go back. |
| `defer_fresh` >> `defer_flip` | `Remix live:` | the deferral is doing work |
| `anchor_parked` / `anchor_recap` / `anchor_promoted` | `Remix live:` | **MUST stay at 8 / 0 / 8 per 21,630 flips.** This knob cannot touch them. If they move, something else did. |
| `Remix albedo-trace: ... ref=` | `remix_dump.log` | **NEW. `anchor_prev` should collapse.** This is the judgement, over ~4,000 samples, with the camera free. |

**PRE-REGISTERED REFUTATION:** a deferred draw is submitted **later in the frame** than the geometry around
it. If anything now sorts wrongly — decals sinking into surfaces, blended props against opaque ones — that
is this knob's submit-order change and nothing else in this build can cause it.

**REVERT:** `set "RPCS3_REMIX_DEFERPREANCHOR=0"` — one line, round 44b bit-exactly.

---

## READ THIS FIRST — round 44b's remedy was INERT, and that retires a whole family

```
gauge_donor_upgrade_avail = 0      over 21,630 flips
gauge_donor_upgraded      = 0
gauge_contested           = 0
```

`gauge_donor_upgrade_avail` is incremented **whether or not the knob is armed** — round 44b built it that
way exactly so an inert run could be told from a dead route. It read **0**. `GAUGEDONORBEST=32` never fired.
The fixture still teleports because the knob was never in the loop; that is not a refutation of the
diagnosis, it is the absence of a treatment.

**Both guards passed and are worth keeping:** `anchor_promoted` 8/21,630 = **0.00037/flip** against round
43's 0.00032 (round 44's bad run was 0.1360) — **flat**. `gauge_prev` share **17.31%** against round 43's
19.22% (round 44 hit 37.09%) — **did not rise**. The pre-registered within-frame tear **cannot** have
occurred: `gauge_donor_upgraded=0` means the gauge never changed mid-frame.

**WHY it was inert, and this is the important part.** `gauge_contested=0` lives in the *same branch* as the
upgrade — the second-and-later gauge donor of a frame — and that branch is entered constantly:
`worldid-census` shows `ad7ce9d672a0bf6b` alone submitting **1,377,732 draws over 21,630 flips = 64 per
frame**, and every one past the first re-enters it. **So there are ~63 later donors per frame and every
single one AGREES with the first to within tolerance.**

Round 44 refused a donor. Round 44b elected between donors. **Both assumed a choice exists inside the frame.
It does not.** The frame's only donor is itself sometimes wrong. **Do not spend another round on donor
selection — refusal, election, ranking, hysteresis or consensus. The measurement says there is nothing to
select between.** `GAUGEDONORBEST` is left at 32 because it costs nothing and its counters size the route
for free; nothing depends on it.

---

## PRIORITY 1 — judge `DEFERPREANCHOR`, and use the new `ref=` field to do it

`Remix albedo-trace:` now carries **`ref=` and `refage=`**. That closes the gap the round-45 brief named:
the one instrument that runs continuously and is not gated on a pick could *measure* the defect but never
*attribute* it, because `ref=` lived only on `Remix picked:` — and **every pick in this project's history
was taken with the game paused.**

Round 45's baseline on `E40BF80AF519848A` / `C2003391127734F6`, 3,989 samples, camera free:

```
|t| < 0.01 (exact) ..... 24.39%      > 1 unit ..... 49.84%
mode at 0.5-1.0 ........ 20.31%      > 8 units .... 15.47%
mode at 2-4 ............ 16.57%      > 32 units .... 7.85%
max |t| = 501.688     max basis deviation = 0.2331
```

**Point the trace at the same albedo, play it with the camera moving, and cross-tab `|t|` against `ref=`.**
Two readings and their meanings, pre-registered:

* `anchor_prev` share collapses AND the 0.5-4 unit modes go with it → **the deferral worked**, and the
  remaining tail is a separate defect.
* `anchor_prev` collapses and `|t|` does **not** → the stale anchor is not the displacement after all,
  the 8/8 pick correlation was confounded, and the next suspect is `gauge_cur_dims` (**116,584** draws
  placed by a *dimensions-fallback* anchor — a different render source that merely shares the clip size).

**501.688 is not the fixture's private number.** `worldid-census` reads `tmax=501.688` for
`c1d482dcd1b03ed0`, a different program, and `tmax=3201.11` shared by `ad7ce9d672a0bf6b`,
`d0b6a471bb2d463b` and `0214281b9a7a412d`. Unrelated programs sharing one displacement magnitude is the
per-frame global gauge, confirmed a second way.

Scene-wide, from the same census: **the world is placed correctly 99.97% of the time**
(`ad7ce9d672a0bf6b`, 391 bad of 1,377,732) — **it is the props that move**: `c1d482dcd1b03ed0` misplaces
**10.6%** of its 66,447 draws, which is the same order as `gauge_prev`'s 17.31% share of placements.

---

## PRIORITY 2 — the black floor: NO SKIP GATE EATS IT. Settled, do not re-run

The round-45 brief asked which of the six shared-counter gates eats the floor. **None of them.** This did
not need the counter split — it needed reading `Remix skip-census:` correctly, and it is settled from the
round-44b play-test log with no rebuild.

`report_skip_census` dedups by `(gate, vp, fp, albedo)` and caps at 256 lines, and **`m_skip_census_seen` /
`m_skip_census_lines` are never reset — there is no window.** The run emitted **6 lines of 256**, so the cap
was never approached and **a gate with no census line fired zero times, run-wide.** Only these fired:

```
skipuntexturedvp  vp=7f3d3abcefc8b057  albedo=0  clip=2048x2048   shadow map, untextured
unboundblend      vp=33ae0895aef9ef72  albedo=0  clip=512x288     aux pass, untextured
characterdepth    vp=56cc5a962ead7ecd  albedo=0  clip=512x288     aux pass, untextured
shadowonly        vp=6004dce66b8b7a11  albedo=0  clip=512x288     aux pass, untextured
skippair          vp=57a12323f22f4988  albedo=2722B18EB6EEDDF6  clip=1024x576  (two fp)
```

`skippair` is the **only** gate touching the main pass with a real albedo, and its size is MEASURED by
subtraction over counters that already existed: `notex_refused_skip 1,661,448 − skip_shadowonly 14,422 =
1,647,026` (untexvp + unbound + chardepth), and `skip_albedo 1,660,629 − 1,647,026 = **13,603**` — 0.18% of
7,728,159 draws, on the giant-NPC-weapon albedo the launcher documents. **Not a floor.**

`skip_vp = **0**`, so **`SKIPVP` and `SKIPEXTENTVP` both ate nothing at all** despite
`SKIPEXTENTVP=57A12323F22F4988 SKIPEXTENTMIN=128` being armed on main-pass geometry.
`skip_aux_untextured=0`, `so_nomain=0`, `so_notsmaller=0`; `skipalbedo` and `untexturedfppair` never fired.

The static index is clear too: `entries=567 deferred=0 stale=0 dropped=0 nomesh=0 peak=100 budget=128`.
`evicted=378` looks alarming and is not — it means the reaper freed those entries' union meshes and the next
draw rebuilds them, and `dropped=0` says **no draw ever took the no-live-handle exit**. Round 43's
`resident=0 of 16601` pathology did not recur.

**The counters still shipped** (`skipg_pair` / `skipg_alb` / `skipg_untexvp` / `skipg_untexfp` /
`skipg_chardepth` / `skipg_unbound` / `skipg_vp` / `skipg_extent`, on `Remix live:`, which reaches
`remix_dump.log`) so this is *readable* next round rather than *derived*. Two invariants to check:
the first six sum to `skip_albedo` and the last two sum to `skip_vp`, both on `Remix stats:`.

### What the floor's unclickability does and does NOT prove

The brief read *"unclickable"* as *"no draw exists, therefore a submission failure"*. **The first half is
right; the second is one hypothesis of two.** An empty pick means **no draw is AT THOSE PIXELS**. A draw
submitted and then thrown 500 units across the room is also not at those pixels and also picks as nothing.
Unclickability kills the *shading* hypothesis outright. It does **not** separate *not submitted* from
*submitted somewhere else*.

With every skip gate cleared, the static index clean, and the world-refusal exits accounted
(`nocam=68100 lay_other=18155 tail=107734`, summing exactly to `world_fallback=193989`), **displacement is
now the leading candidate**, and it is supported:

* the user's own clip, frame 28: **a long beam/plank floating diagonally in mid-air** — and the missing
  object is the **wooden plank** floor;
* `c1d482dcd1b03ed0`, the program drawing the props and tiles in that room, misplaces **10.6%** of its
  draws, max **501.688** units;
* `ad7ce9d672a0bf6b` reaches **3201.11** units on 391 draws — far enough to empty a room.

**INFERRED, not measured** — the plank in the video is not tied to a hash. **`WATCHALBEDO` is retargeted for
exactly this**: it was carrying `EF4700267F08C7F3`, which matched **zero lines all run**, and now carries
`4094F22DB2A8278A` (the checkerboard tile — the *submitted, clickable* neighbour of the void, which picks
`ref=identity-bypass origin=[0 0 0]`) and `2722B18EB6EEDDF6`. **Walk over the black floor with it armed and
copy every `Remix watch:` line.** And: **if Priority 1's fix moves the floor, they were one bug.**

---

## PRIORITY 3 — carried, unchanged

* **`gauge_cur_dims = 116,584`** draws placed by a *dimensions-fallback* anchor — a different render source
  that merely shares the clip size. Untouched, unexamined, and the next suspect if `ref=` clears
  `anchor_prev`.
* Still teleporting: the big metal pipes near the windows. Same family as the fixture; judge on the same run.
* **Lights: `guest_lights=0 guest_light_match=0 mat_emissive=59`.** Blanking `GUESTLIGHTALBEDO2` did what it
  was meant to — round 44 had `guest_lights=114 guest_light_match=6141`. Confirm with the user that the
  dumpster/sheet-metal point lights are gone. `GUESTLIGHTAUTO` is still 0 and the two known faults (no
  world-geometry gate; light position from the emitter rather than the billboard centroid) are still unfixed
  — do not re-enable without both.
* Walls still disappear at angles in that area. Helmet clips `1BF8325ADEF3C986`. Smoke and the nectar gas
  grenade still do not render although fire does. `524D584E4F544558` is ASCII **"RMXNOTEX"**, the backend's
  own untextured sentinel, and grey is `notexmat=1` working as designed (`notex_mat_applied=1192`).
  `6575ACE3A42A78E6` renders as a solid yellow quad — check whether the UI compositor resolves an albedo
  `entry` at `RemixGSRender.cpp:13807` (`have_uv ? entry : nullptr`).
* ADS collapse root-caused in round 43, not fixed. `UIWIDTH` is a trade, still 1280, user has not chosen.
* Chapter list untouched. Nothing under `bin\patches\` was changed.

---

## RETRACTED — do not investigate

**"White triangular spikes on an NPC" is NOT geometry corruption.** They are the **damage-direction
indicators in the HUD**, rendering correctly. The round-45 brief called them character geometry corruption
and the user corrected it. There is nothing there.

**The general lesson, because it will recur:** on this title the HUD draws through the same compositor as
the world, so a screen-space UI element can look like world geometry in a still frame. Before calling
anything in a frame a geometry defect, check **position stability across frames while the camera moves** —
screen-space holds its screen position, world geometry does not.

Also retracted, from round 44b: **the dedup is refuted as the cause of the underfloor** (the `static_key`
folds in `m_current_vp_hash`, `albedo_hash` and the material pointer, so two floor layers land on different
entries and can never collide). And **mesh-key collision**, **parity/ping-pong** and **`world_refused` by
material** were refuted before that. Do not re-run any of them.

---

## Tooling left behind

In `...\scratchpad\r45\`:

* **`run45.log`** — the 20.9 MB slice of the round-44b play-test (`build=Aug 27 2026 12:12:05`,
  `flips=21630`), sliced from `remix_dump.log` at byte offset 1,181,271,419.
* **`trace.py` / `trace2.py`** — the `Remix albedo-trace:` analysers (distinct raw boxes, `|t|` histogram,
  per-box cross-tab, camera-step correlation, bad-run lengths).
* **`fmtaudit45.py`** — a format-string auditor for `RemixGSRender.cpp` that works and is **self-tested**.
  `python fmtaudit45.py <file> [substring]`. Reports **108 `fmt::format` calls**, matching
  `grep -c "fmt::format("` exactly, **0 mismatches** on the shipped file. Self-test: `selftestA.cpp`
  (one argument deleted from `Remix live:`) and `selftestB.cpp` (one extra specifier added to
  `Remix albedo-trace:`) — **it catches both, at exactly the two sites round 45 edited.**
  **Round 44's note stands: `scratchpad\r43\audit.py` gives false passes. Do not use it.**
* **`dryrun.cmd` / `dryrun_base.cmd` / `dry2100.cmd`** — launcher parse tests. See below.

## Instrument notes worth keeping

* **`Remix stats:` and `Remix timing:` are not unreadable — they are in the OTHER log.** Both land in
  `bin\log\RPCS3.log`, 267 lines each, from the same run as the dump. Three of round 45's load-bearing
  numbers (`skip_albedo`, `skip_vp`, `deferred_instance=`) came from there. **Slice both logs, not one.**
* **A dedup census with an unhit cap is a proof of ABSENCE, even though it cannot count a population.**
  The standing warning ("a census that dedups by key cannot enumerate a population") is true and was read
  one step too far: it cannot say *how much*, but it says *whether* with certainty.
* **A launcher edit can be syntax-checked without launching the game, and it should be.** `head -n` to a
  line **well before** `cd /d "%~dp0bin"` (currently `:5017`; the next line starts the game), append
  `echo ...%VAR%...` + `exit /b 0`, run under `cmd /c`, and read the values back. Round 45's edits were
  verified this way: `DEFER=[1] WATCH=[4094F22DB2A8278A,2722B18EB6EEDDF6] BEST=[32] PREVONLY=[1] FPAN=[1]
  GLA2=[]`. **Diff the output against the same dry-run built from `git show <HEAD>:launch-haze-remix.cmd`** —
  round 45's was byte-identical, which is what proves an edit added no new parse behaviour. Note the
  launcher already emits three `is not recognized` / `unexpected at this time` lines from **pre-existing**
  `rem` text past line 2100; they are in the committed file too and the game launches regardless.
* **Every pick is taken with the game paused.** Motion instruments must run continuously and key on the
  albedo or program, never on a pick.
* **A cost attributed before the timer for it existed is a guess.** `deferred_instance=` was added in round
  32 for exactly this re-test; the ~14 fps predates it.
* **`bin\remix_dump.log` is ~1.2 GB and append-mode.** Find run boundaries with
  `tail -c 400000000 remix_dump.log | grep -abo "Remix run-start:"`, add the tail offset, then
  `tail -c +N` once into the scratchpad and grep that.
* **When a counter designed to size an unarmed route reads 0, believe it and go read its sibling in the
  same branch.** `gauge_donor_upgrade_avail=0` plus `gauge_contested=0` is what retired the whole
  donor-election family in one reading, with no play-test.
