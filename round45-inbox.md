# Round 45 inbox — RPCS3 RTX Remix backend, Haze (BLUS30094)

Written at the close of round 44b, 2026-08-27. Branch `remix-backend`, HEAD **`398f3f1ed`**.

**THE TREE MOVED DURING THIS ROUND — not by me.** It started at `f091ff5a4` and someone committed
`398f3f1ed` *"Remix: parallax height map bound as albedo; the world gauge is a per-frame global"* at
**2026-08-27 12:02:05**, mid-round, while round 44b was being built. That commit snapshots the **round 44**
state: the round-44 source, the round-44 KNOBS section, `round43/44/45-inbox.md`, and the launcher with
`RPCS3_REMIX_GAUGEDONORMAXT=0` (the coordinator's revert) at its line 202. **Round 44b is NOT in it.**

Everything this round produced is uncommitted working-tree state on top of `398f3f1ed`: seven modified
files (RemixGSRender.cpp/.h, RemixTransforms.cpp/.h, the KNOBS doc, the launcher, this file).
I committed nothing.

| artefact | SHA256 (first 16) | note |
| --- | --- | --- |
| `bin\rpcs3.exe` | `260E9986847110F8` | built this round, MSBuild `Release\|x64` exit 0, **0 errors, 1 warning** |
| `bin\rpcs3-next.exe` | `260E9986847110F8` | **identical — the copy was made** |
| `bin\remix\d3d9.dll` | `16A0B512F33EBB66` | **UNCHANGED.** Nothing deployed into `bin\remix\`. |
| `bin\rtx.conf` | `215975B8697DE21B` | **UNCHANGED.** Read only, never written. |

Round 44 deployed `14DD8282D86D298D` and it was play-tested and **reverted**. The one warning is the
pre-existing C4723, now at `RemixGSRender.cpp:11636`. **Zero new warnings.** Full derivations in
`docs\remix\KNOBS.md` — read **"Round 44b"** first, then "Round 44".

**Launcher line numbers shifted again.** `WORLDIDMAXT` `:156`, `GAUGEDONORBEST` `:203`, `TRACEALBEDO`
`:64`, `DEFERPREANCHOR` `:1027`, `GUESTLIGHTAUTO` `:1305`. Re-grep, never trust a quoted line number.

---

## READ THIS FIRST — it invalidates a whole class of measurement, including several of mine

**The user pauses the game before every Ctrl+Click.** Their words: *"I did pause the game to ctrl click the
light fixture (and others that teleport really quickly) so that way i don't mis click another texture that
is actually stable."*

That is sound method on their side. It also means **every pick in every log this project has ever taken was
captured with a frozen camera.** So round 43's `E40BF80AF519848A` follow reading `d_origin = 0` for 181
consecutive frames was not bad luck — it is **structural**, and it would have read 0 no matter how bad the
defect was.

**Any diagnostic that samples at pick time, is armed by a pick, or keys off `pick_record` can never observe
a motion defect.** `Remix pick-follow:` is disqualified for motion work whatever else is fixed about it.
Instruments for motion must run **continuously** and key on the albedo or the program.
`Remix albedo-trace:` (`RPCS3_REMIX_TRACEALBEDO` + `TRACEALBEDOVP`) is the one that qualifies today. Re-read
any earlier conclusion that rests on pick-adjacent sampling of a moving object.

---

## PLAY-TEST CARD — one knob changes pixels

### `RPCS3_REMIX_GAUGEDONORBEST=32` (launcher `:203`)

Round 44's `GAUGEDONORMAXT` **refused** an off-origin gauge donor. That handed it to the `ANCHORSTICKY`
park, and `promote_parked_anchors()` runs at **flip** — so the refused donor was installed anyway, **one
frame late**. Per flip, round 43 → round 44: `anchor_promoted` 0.00032 → 0.1360 (**+42,512%**),
`anchor_recap` 0.0766 → 0.0181, park outcomes 99.6%/0.4% recaptured/promoted → **11.8%/88.2%**, and the
share of placements on last frame's anchor 19.22% → **37.09%**. A one-frame-old gauge under camera rotation
displaces the whole scene by (distance from eye) x (turn per frame) — which is exactly *"the whole scene
warps aggressively when turning camera"*. **It is removed from the build**, not defaulted off.

`GAUGEDONORBEST` does the opposite. The frame's **first donor installs immediately and unconditionally**, so
the gauge is never late and never absent. A **later** donor of the same frame may **replace** it, but only
when at least **twice as close** to the world origin and only when the installed one is beyond the
threshold. First-draw-wins becomes best-draw-wins. Nothing is parked, promoted or refused.

| reading on `Remix live:` | meaning |
| --- | --- |
| `gauge_donor_upgrade_avail` > 0, `gauge_donor_upgraded` > 0 | the route fired. This is the run to judge. |
| `avail` > 0, `upgraded` = 0 | the knob is **off**, not the route dead |
| `avail` = 0 | a better donor never existed in the level you played. Nothing can change; play the plant. |
| `anchor_promoted` | **MUST stay near round 43's 0.00032/flip.** This knob cannot touch it. If it moves, something else did. |
| `gauge_prev` **as a share** of `gauge_used + gauge_prev + gauge_absent` | **MUST NOT rise** above round 43's 19.22%. Round 44 (bad) hit 37.09%. |

**Divide by `flips` before comparing anything.** Round 43 ran 56,381 flips, round 44 ran 17,583. Raw totals
across sessions are meaningless — see the post-mortem below.

**PRE-REGISTERED REFUTATION:** the gauge now changes mid-frame, so draws submitted before an upgrade used
the old one. **If the scene tears within a frame — part of the world offset from the rest, props separating
from the floor they stand on — that is this knob.**

**REVERT:** `set "RPCS3_REMIX_GAUGEDONORBEST=0"` — one line, round 43 bit-exactly.

### My guard failed twice, and both errors are reusable

1. **I put a threshold on an absolute cumulative counter and compared across sessions of different length.**
   The guard was "`gauge_absent` must not rise much above 295,391". It read 57,424 and I would have called
   that a pass. Per flip: 3.27 vs 5.24. The round-43 notes already say *"a cumulative counter divided by
   frames is not a rate"*; this is the mirror-image error.
2. **Even normalised it was the wrong counter.** The failure route was park → promote, so the gauge was
   never *absent*, only *late*. `anchor_promoted` moved 425-fold, and my own shipped comment named it as a
   counter that "may rise with it" **without putting a threshold on it**. Put the guard on the counter the
   failure mode moves.

**THE RULE:** *a gauge remedy must never make the gauge later or absent.* Wrong-but-current beats
correct-but-late.

---

## The defect is not in doubt — and it finally has a continuous, camera-free measurement

`TRACEALBEDO` was re-pointed at `E40BF80AF519848A` (`TRACEALBEDOVP=C2003391127734F6`) and produced **2,331
per-frame samples with the camera free**:

```
distinct raw vertex boxes over 2,331 frames ..........  2   (two sub-parts; neither ever moves)
frames placing it more than   1 unit off ............. 46.63%
                              8 units ................ 25.61%
                             32 units ................ 16.60%
                            128 units .................. 0.77%
max |t| = 194.6 units      max basis deviation = 0.1141 (~6.5 deg of spurious rotation)
bad frames form 53 runs, median 6 frames, longest 225
```

The raw box is bit-identical across every sample. **The guest never moves it; we do, on 47% of frames, by
up to 194 units.** Episodes of 6 to 225 frames is "it teleports and comes back". This is independent of any
pick and independent of the `worldid-draw` evidence, and it agrees with both.

The cause remains `capture_gauge_anchor()` (`RemixGSRender.cpp:1810`) installing a `WORLDIDENTITYVP` draw as
the frame's world gauge on the vp-hash list alone (`:1815`), while the submit site's `keep_resolved`
independently judges the same draw against `WORLDIDMAXT` and refuses to call it world-space.

**Keep the trace pointed here until it is closed.** If `GAUGEDONORBEST` works, these percentages fall. That
is the measurement, not a screenshot.

---

## PRIORITY 1 — the underfloor. It is a TWO-LAYER problem, not alternating dropout

New from the user: ***"The wooden plank floor is supposed to be under the checkerboard tiles."*** The tiles
render **correctly**. The black is the **missing underfloor**, seen through the gaps between them. The
"alternating dropout" framing that the checkerboard appearance suggested is wrong and should be dropped.

Counters from the round-44 build: `static_submit_dedup = 115,567`, `static_submit_meshdiff = 1,309` —
meshdiff is **1.13%** of dedups. And `xform_measured 1,777,810 − submitted 1,662,243 = 115,567` **exactly**,
which confirms round 44's subtraction attribution of that exit to the unit.

**THE DEDUP IS REFUTED AS THE CAUSE — settled from source this round, do not spend a round on it.**
`static_key` (`RemixGSRender.cpp:21027`-`21061`) folds in `block->real_offset_address`, `base_offset`,
`memory_location`, `attribute_stride`, the per-attribute layout, **`m_current_vp_hash`, `albedo_hash` AND
the material pointer**. The submit-signature dedup at `:23337` tests `static_entry->submitted_signatures`,
i.e. it is scoped to **one** `static_entry`. Two floor layers with different textures therefore land on
**different entries and can never collide.** `static_submit_meshdiff` at 1.13% of dedups is consistent
with that: the collapse is doing its job. **Do not fold the mesh hash into the signature to fix the
underfloor** — it would not touch it. (Folding it in may still be worth doing for its own sake later; that
is now an unrelated, low-priority item.)

So the planks are lost somewhere else, and the search is open.

Then find where the planks actually go. Candidate silent exits, all of which drop a draw without a refusal
census line: `static_index_deferred_dropped` (`:21299` — *"no live handle at all, so the draw is never
submitted, invisible WITHOUT a trace"*), the `source_changed` union wipe (`:21078`, uncounted), and the
static-index residency pathology (`resident=0` of `entries=16601` on the round-43 run, `evicted=16601`).

Refuted, do not re-run: **mesh-key collision** (the key hashes every raw vertex byte at `:20972-20979`),
**parity/ping-pong** (none exists on the world submit path), and **`world_refused` by material** (its census
keys on `(program, reason)` with a 128-line cap per 120 flips against 452,669 refusals — at most ~0.03% of
the population, one albedo per pair; round 43's "only two distinct keys" was both wrong and unanswerable).

---

## PRIORITY 2 — point lights on props. NEW, never investigated, and `GUESTLIGHTAUTO` is 0

User: *"some of the props like dumpsters and sheet metal have point lights on them, which have been there
for a while."* The automatic glow-card light path is **off**, so **something else creates these**. Round-44
counters: `guest_lights = 114`, `guest_light_match = 6,141`, `mat_emissive = 260`.

Enumerate every path that can create a Remix light (`CreateLight`, `light_info`,
`maybe_inject_guest_light`, the sun path, `suncard`, `lightpass`) and the knob gating each, then check which
are live with `GUESTLIGHTAUTO=0`. The dumpsters are `vp=830d7d1b9681c475` with fps `c61b0b9586dd67fb` /
`bd80201c29b6b01e`; dumpster/locker albedos include `CA4E206BDCF6143E`, `7EF0E9C703B50054`,
`B0624AD7BA785B13`, `955BCB4DA1EB7DD9`. `Remix light-candidate:` and `Remix lightpass:` censuses exist.
This is a long-standing visible artefact with a cheap likely fix (one knob to 0) — worth doing early.

## PRIORITY 3 — carried

* **Still teleporting:** the big metal pipes near the windows that used to carry the green lights. Same
  family as the fixture; judge them on the same run.
* **Lights are still entirely absent** and that is still deliberate. `GUESTLIGHTAUTO=0`. Round 44b did not
  fix either known fault — a world-geometry gate so character suit panels cannot qualify, and light
  position from the emitter rather than the billboard centroid. Do not re-enable without both.
  Note this is *separate* from Priority 2: that one is unwanted lights from another path.
* `gauge_prev_camfresh` — **611,546 draws divided by last frame's anchor while the elected camera is
  current**, 34.78/flip. If `GAUGEDONORBEST` does not close the wobble, this is the next population.
  `RPCS3_REMIX_DEFERPREANCHOR=1` (launcher `:1027`) is the shipped-but-off mechanism for exactly it.
  A whole round on its own — **do not bundle it with anything**, and remember the rule: it must not make
  the gauge later.
* Helmet clips: `1BF8325ADEF3C986`. Weapon/arms: `0721D150DF278E7D`, `CC6008D0E9E98972`.
* `524D584E4F544558` is ASCII **"RMXNOTEX"**, the backend's own untextured sentinel — that draw resolves no
  albedo at all.
* `6575ACE3A42A78E6` nectar UI renders as a solid yellow quad (unsampled texture). Check whether the UI
  compositor resolves an albedo `entry` at `RemixGSRender.cpp:13807` (`have_uv ? entry : nullptr`).
* `90DAF653752E0DD2` low-resolution. Smoke and the nectar gas grenade do not render though fire does. UI has
  residual noise and does not update smoothly with the camera.
* ADS collapse root-caused in round 43, not fixed. `UIWIDTH` is a trade, still 1280, user has not chosen.
* Chapter list untouched. Nothing under `bin\patches\` was changed.

---

## Tooling left behind

In `...\scratchpad\r44\`:

* **`run44b.log`** — the 17 MB slice of the round-44 play-test (build `Aug 27 2026 11:01:23`,
  `flips=17583`). Contains the 2,331 `Remix albedo-trace:` samples on `E40BF80AF519848A`.
* **`lastrun.log`** — the 49 MB round-43 slice (`flips=56381`). The two together are the per-flip A/B.
* **`fmtaudit44.py`** — a format-string auditor for `RemixGSRender.cpp` that **actually works**.
  `python fmtaudit44.py <file> [substring]`. 108 `fmt::format` calls, matching
  `grep -c "fmt::format("` exactly, 0 mismatches on the shipped file.
* `selftest.cpp` — a copy with a deliberate 1-extra-specifier mismatch injected at the `Remix pick-follow:`
  site. `fmtaudit44.py` catches it; **`scratchpad\r43\audit.py` does not** — it reports "298 formatted
  calls, 0 mismatches" on that same broken file. **Do not trust `r43\audit.py`.**

## Instrument notes worth keeping

* **Every pick was taken with the game paused.** Motion instruments must run continuously and key on the
  albedo or program, never on a pick. See the top of this file.
* **A gauge remedy must never make the gauge later or absent.** Wrong-but-current beats correct-but-late.
* **Normalise by `flips` before comparing two sessions,** and put the guard on the counter the failure mode
  actually moves — not the one that sounds related.
* **A census that dedups by key cannot enumerate a population.** Read the admission gate and compute the
  ceiling before quoting one as a fact about the world.
* **`d_origin` between two draws of one (program, texture) pair is not motion** where a title instances one
  mesh many times. The tell is that it is quantised: numerical error is not.
* **Self-test the format auditor on a deliberately broken copy every time.** One of the two in the
  scratchpad gives a false pass on exactly the site round 44 edited.
* **`bin\remix_dump.log` is ~1.17 GB and append-mode.** Find run boundaries with `grep -abo "Remix
  run-start:"` over a `dd`-skipped tail, slice once with `tail -c +N` into the scratchpad, then grep that.
* **`Remix stats:` carries `poisoned=`**, and it is what makes the `:23337` subtraction exact.
* **Do not build a launcher dry-run by `head -N` past the `cd /d "%~dp0bin"` line** — the next line starts
  the game.
