# Round 45 inbox — RPCS3 RTX Remix backend, Haze (BLUS30094)

Written at the close of round 44, 2026-08-27. Branch `remix-backend`, HEAD **`f091ff5a4`**, working tree
carries rounds 37–44 (39–41 committed; 42's, 43's and 44's source edits and the launcher are not).

| artefact | SHA256 (first 16) | note |
| --- | --- | --- |
| `bin\rpcs3.exe` | `14DD8282D86D298D` | built this round, MSBuild `Release\|x64` exit 0, **0 errors, 1 warning** |
| `bin\rpcs3-next.exe` | `14DD8282D86D298D` | **identical — the copy was made** |
| `bin\remix\d3d9.dll` | `16A0B512F33EBB66` | **UNCHANGED.** Nothing deployed into `bin\remix\`. |
| `bin\rtx.conf` | `215975B8697DE21B` | **UNCHANGED.** Read only, never written. |
| `bin\user.conf` | `77A65DFE391BA7D5` | **UNCHANGED.** Read only. |

Round 43 deployed `ADA8AD6CDCDB53B1`; both exes read exactly that at the start of this round, which is
what its inbox recorded. The one warning is the pre-existing C4723 "potential divide by 0", now at
`RemixGSRender.cpp:11562` (round 43 recorded 11456; this round's edits above that line moved it).
**Zero new warnings.** HEAD `f091ff5a4` at both ends and the same modified-file set — the tree did not move.

Full derivations in `docs\remix\KNOBS.md` — read **"Round 44"** first.

**Launcher line numbers shifted.** `launch-haze-remix.cmd` gained 177 lines around `:158`–`:203`, so every
line reference in round 42/43 notes past that point is off by ~+50. `WORLDIDMAXT` is now `:156`,
`GAUGEDONORMAXT` `:202`, `TRACEALBEDO` `:64`.

---

## PLAY-TEST CARD — one knob changes pixels

### `RPCS3_REMIX_GAUGEDONORMAXT=32` (launcher `:202`)

**What it does.** A `WORLDIDENTITYVP` draw sitting more than 32 world units from the origin may keep its
own transform (that is `WORLDIDMAXT`, unchanged) but may no longer become **the whole frame's world
gauge**. One declaration drove two gates and only one of them ever learned the threshold.

**What to look at.** Props, light fixtures and beams that teleport or bounce — `E40BF80AF519848A` above
all, and the eight deeper in the plant. The prediction is that the *jump* class goes away or shrinks
sharply. It is **not** aimed at continuous fine wobble.

| reading on `Remix live:` | meaning |
| --- | --- |
| `gauge_donor_offside` > 0, `gauge_donor_refused` > 0 | the gate fired. This is the run to judge. |
| `gauge_donor_offside` > 0, `gauge_donor_refused` = 0 | the knob is **off**, not the mechanism absent |
| `gauge_donor_offside` = 0 | the mechanism is **not present in the level you played**. Nothing can change; play the plant. |
| `gauge_absent` climbing well past **295,391** | **FAIL** — the gate starved the gauge slot. Revert. |
| `anchor_parked` up from **4,338** | expected. Offside donors are parked, not dropped, so a scene cut still recovers. |
| `gauge_prev` up from **1,634,288** | acceptable cost. Last frame's *correct* gauge beats this frame's wrong one. |

**PRE-REGISTERED REFUTATION:** props teleport by the same amounts while `gauge_donor_refused` reads in the
hundreds of thousands. Then the donor election is not what moves them, and the next suspect is the
`ref=anchor_prev` population (`gauge_prev_camfresh = 1,582,140`, 18.6% of all placed draws, divided by
last frame's anchor while the elected camera is current).

**REVERT:** `set "RPCS3_REMIX_GAUGEDONORMAXT=0"` — one line, round 43 bit-exactly.

### Also changed, both DIAGNOSTIC ONLY — no pixels

* `RPCS3_REMIX_TRACEALBEDO=E40BF80AF519848A` + `RPCS3_REMIX_TRACEALBEDOVP=C2003391127734F6`
  (launcher `:64`–`:65`, was `2C6485F7F04591F1` / `C1D482DCD1B03ED0`).
  **PLAY THIS ONE WITH THE CAMERA MOVING.** `Remix albedo-trace:` prints one line per frame carrying
  `raw=[..]..[..]` **and** `matrix=[..]` **and** `cam=[..]` together — the only instrument that can say in
  one line whether the guest moved the object or we did. `C2003391127734F6` is on no launcher list, so
  `worldid-draw` has never covered that copy.
* `Remix pick-follow:` now also matches the clicked record's **vertex count**, and prints `vtx= ext= det=`.
* `Remix live:` gains `gauge_donor_offside`, `gauge_donor_refused`, `static_submit_dedup`,
  `static_submit_meshdiff`. Both banners gain `gaugedonormaxt=`.

---

## PRIORITY 1 — the checkerboard floor. An uncounted `return` discards 7.29% of all draws

`RemixGSRender.cpp:23263`:

```cpp
if (static_entry->submitted_signatures.contains(static_submit_signature))
{
    return;
}
```

The signature (`:23214-23261`) is built from **only** the 3x4 instance transform, `categoryFlags`,
`doubleSided` and the blend state. **Nothing about which geometry the draw covers.** Haze bakes world
geometry in world space — every plant-floor `worldid-draw` line reads `pre=[1 0 0 0; 0 1 0 0; 0 0 1 0]` —
so **every static-index floor tile in a frame produces an identical signature and only the first
submits.** That is correct if and only if the union mesh the first one carried already held every tile,
and the union is built incrementally, is wiped mid-frame by the `source_changed` reset at `:21004` (also
uncounted), and sits at **`resident=0` of `entries=16601`**.

**Its size, MEASURED by subtraction on the round-43 build.** `:23263` is the only return between
`++m_stats.xform_measured` (`:22771`) and `++m_stats.draws_submitted`, other than the poisoned-mesh
return, and `Remix stats:` reads `poisoned=0`:

```
xform_measured   8,569,414
draws_submitted  7,944,541
poisoned                 0
--------------------------
this exit          624,873      = 7.29% of every draw that resolved a transform and a mesh
```

**Round 45 starts by reading two numbers this build now prints.** `static_submit_dedup` must reconcile to
that subtraction. `static_submit_meshdiff` is the half that matters — suppressions where the mesh **this**
draw had selected is not the mesh already submitted under that signature, i.e. where a strictly larger
union was built and thrown away.

* `meshdiff` near 0 → the collapse is benign; the half-present floor is somewhere else and this lead is
  closed with a number instead of an argument.
* `meshdiff` large → **the fix is to fold the selected mesh `hash` into `static_submit_signature`.** That
  is provably a superset of today's submissions (identical union ⇒ identical hash ⇒ identical signature ⇒
  suppressed exactly as now). **The risk to think about before shipping it is double-draw:** the partial
  union already submitted is a strict subset of the completed one, so both would be drawn coincident. One
  mechanism, one knob, and the counters above are the A/B.

**Refuted this round, do not re-run:**

* **Mesh-key collision.** The ordinary mesh key hashes **every raw byte of every vertex**
  (`RemixGSRender.cpp:20898-20905`) plus all indices, the vp hash and the albedo; `union_hash`
  (`:21130-21161`) does the same. Two distinct tiles cannot collide. `tex_key_dup=33333` is a
  texture-descriptor counter, unrelated.
* **Parity / ping-pong.** No frame-parity gate, draw-index parity, ping-pong buffer or double-buffered
  slot array exists on the world submission or mesh path. Every `% 2` / `& 1` in `RemixGSRender.cpp` and
  `RemixTransforms.cpp` is a texture-unit bitmask, a rate-limited log, or triangle-strip winding in the
  **UI** compositor (`:14217`).
* **`world_refused` by material is UNANSWERABLE from the census.** `world_refused_census_slot()`
  (`:10157-10173`) dedups on **(program, reason)** only — albedo, vtx, clip and surf are printed but not
  keyed — window 120 flips, cap 128 lines, 14 reasons. Ceiling 14x14 = 196 lines per window against
  452,669 refusals: **at most ~0.03% of the population, one albedo per (vp, reason).**
  **Round 43's "only two distinct keys in the whole run" is wrong twice over** — the 1,087 lines name 18
  distinct albedos and 14 vps — and the instrument could not have answered it either way. By draw:
  `tail` 402,198 (88.85%), `nocam` 46,317 (10.23%), `lay_other` 42,781 (9.45%); the 38,627 remainder is
  exactly the `particle=38627/416887` arm, which is submitted and correctly not counted as refused.

**Still true and still unexplained:** `entries=16601 rebuilds=21243 dropped=68 peak=128 budget=128
resident=0 evicted=16601`. `static_key` (`:20986`) and `union_hash` (`:21159`) both fold in
`reinterpret_cast<usz>(material)` — a runtime heap address — plus the guest vertex-buffer address
(`:20957`), and **no vertex data**. The reaper erases from `m_meshes` but never clears
`static_entry->mesh_hash`, which is what `evicted` counts. Correlation over 671 windows does **not** pin
the 46 → 16,601 growth on material recreation (`corr(d_entries, d_tex_destroyed) = +0.370`,
`d_tex_created = -0.003`).

**The floor, named** — tile pitch measured at **~8 world units** (7.75, 8.01, 7.90, 8.42, 8.11), which is
the checkerboard grain: `E40BF80AF519848A`, `CB1677B87EDD72F5`, `71D189E9B559A7F9`, `6DEBE6C7CC0FEEDB`,
`CDC167D57B21D8E2`, `39D5CDABDAAE3ABB`, `E0D568A78FDD03F6`, `514AD452138A2803`. **11 floor albedos are
drawn through BOTH `AD7CE9D672A0BF6B` (on `STATICINDEXVP`) and `C1D482DCD1B03ED0` (not).**

---

## PRIORITY 2 — the wobble. Judge `GAUGEDONORMAXT`, then attack `anchor_prev`

The root cause of the **jump** class is established: the divide gauge, moving as a per-frame global.
559 of 759 multi-row `worldid-draw` frames have every row on one bit-identical pre-translation;
`Remix gauge: translation=` mean 41.99, max 1718.67, above 128 on 7.36% of frames; and
`E40BF80AF519848A`'s raw box never moves while its transform swings 0 → 21.75 → 966.5.

If `GAUGEDONORMAXT` does not close it, the next population is already sized:

```
gauge_used=6571673  gauge_prev=1634288  gauge_absent=295391      -> anchor_prev = 19.22% of placements
gauge_prev_camfresh=1582140                                       -> 96.8% of those had a CURRENT camera
gauge_cur_dims=431701 = gauge_cur_avail=431701                    -> round 38's rescue always fires when it can
```

**1,582,140 draws are divided by last frame's anchor while being rendered with this frame's camera.**
`Remix pick-follow:` breaks down as: `anchor_prev` 44.01% of frames moved (max jump 40.94),
`anchor` 27.19% (max 8.13), `identity-bypass` 17.92% (max 8.12), `camera` 0.00%. **The four largest
single-frame transform jumps in the whole run are all `anchor_prev`.** The election condition
(`RemixGSRender.cpp:17483-17537`): no anchor exists for the draw's own pass shape yet this frame, and the
GAUGECURDIMS fallback also missed — the source comment says why, *"Haze's camera program draws late in
the frame"*. `RPCS3_REMIX_DEFERPREANCHOR=1` is the shipped-but-off mechanism for exactly that population
(launcher `:1025`; round 6 measured 47% of buffered draws never seeing their frame's anchor and left it
off). That is a whole round on its own — **do not bundle it with anything.**

### Do not reuse round 43's origin-jitter numbers. They measured tile pitch.

`trace_pick_follow()` matched on `(vp, albedo)` only and printed the **first** matching draw per frame.
On this title that is not an object: `E40BF80AF519848A` alone carries **42 distinct raw vertex boxes
across 41 distinct vertex counts** on one program. `d_origin` was the L1 distance between two **different
tiles**. Round 43's *"jumps up to 8.16 units, quantised at multiples of ~2.04"* is the instance pitch —
measured on `F312BA4706AA7162`, the positions land on exactly three points evenly spaced on a line, step
**2.899 Euclidean / 4.047 L1**, with 8.10 for a double step. Round 43's companion claim *"exactly 0.0000
on every identity-bypass one"* also fails here: those read maxima of 8.12, 8.11 and 6.36.

And the one round ever aimed at `E40BF80AF519848A` fired its 181-frame follow while the camera was frozen
for **all 191 frames of the window** (`cam=[1786.56 -31.2314 1226.39]`, 191/191 identical; `Remix gauge:`
a constant `translation=6.02944`). `d_origin=0` was read as a stable object. It is what a parked camera
looks like. **That is why this hash stayed open for a week.** The follow now carries the vertex count in
its match and prints `vtx= ext= det=`.

---

## PRIORITY 3 — lights are still entirely absent, and it is still deliberate

`GUESTLIGHTAUTO=0`, unchanged. Round 44 did **not** fix either of the two known faults, and says so:
a world-geometry gate so character suit panels cannot qualify, and light position from the emitter rather
than the billboard centroid. Radius already derives from mesh extent. Do not re-enable it without both.
Current run: `guest_lights=28 guest_light_match=29614 guest_light_toobig=4029 mat_emissive=347`.

## PRIORITY 4 — carried, untouched this round

* Helmet still clips: `1BF8325ADEF3C986`. Weapon/arms picks: `0721D150DF278E7D`, `CC6008D0E9E98972`.
* `524D584E4F544558` is ASCII **"RMXNOTEX"** — the backend's own untextured sentinel. That draw resolves
  no albedo at all; the grey ripple is that, not a material bug.
* `6575ACE3A42A78E6` nectar UI renders as a solid yellow quad — an **unsampled** texture. Round 43's lead
  stands: check whether the UI compositor resolves an albedo `entry` at `RemixGSRender.cpp:13733`
  (`have_uv ? entry : nullptr`); a null `entry` rasterises the flat tint, which is a solid coloured quad.
* `90DAF653752E0DD2` low-resolution. Smoke and the nectar gas grenade do not render though fire does.
  UI has residual noise and does not update smoothly with the camera.
* ADS collapse: root-caused in round 43, not fixed. `UIWIDTH` is a trade, not a defect fix, and is
  **still 1280** because the user has not chosen a value. Do not change it unilaterally.
* The chapter list is untouched again. Nothing under `bin\patches\` was changed.

---

## Tooling left behind

In `...\scratchpad\r44\`:

* **`lastrun.log`** — the 49 MB slice of the newest session (from byte 1,104,125,772 of
  `bin\remix_dump.log`, build `Aug 26 2026 10:50:47`, `flips=56381`). Every number in this inbox comes
  from it.
* **`fmtaudit44.py`** — a format-string auditor for `RemixGSRender.cpp` that **actually works**.
  Usage: `python fmtaudit44.py <file> [substring]`. It finds all 108 `fmt::format(` calls (matching
  `grep -c "fmt::format("` exactly), balances specifiers against top-level arguments, and reports 0
  mismatches on the shipped file.
* `selftest.cpp` — a copy of `RemixGSRender.cpp` with a deliberate 1-extra-specifier mismatch injected at
  the `Remix pick-follow:` site. `fmtaudit44.py` catches it. **`scratchpad\r43\audit.py` does not** — it
  reports "298 formatted calls, 0 mismatches" on that same broken file. **Do not trust `r43\audit.py`.**
* `hash_all.txt`, `wid_all.txt`, `pf_all.txt` and the parsers `parse_wid.py`, `rawmove.py`, `bigpre.py`,
  `movers.py`, `perframe.py` — the `E40BF80AF519848A` and per-frame-global analysis.

`scratchpad\r43\` still holds `chanrule.py`, `killwalk.py` and `fpdis.py` (the albedo-election work).
Those are unaffected.

## Instrument notes worth keeping

* **A census that dedups by key cannot enumerate a population.** Read the admission gate and compute the
  ceiling before quoting a census as a fact about the world. Round 43 quoted `Remix world-refused:` for a
  question its key structurally cannot answer, and miscounted it as well.
* **`d_origin` between two draws of one (program, texture) pair is not motion** where a title instances
  one mesh many times. The tell is that it is **quantised**: numerical error is not.
* **Zero from an instrument that cannot move is not evidence.** Before believing a null result, check that
  the driving variable actually varied during the window.
* **Self-test the format auditor on a broken copy every single time.** One of the two in the scratchpad
  gives a false pass on exactly the site this round edited.
* **A gate that judges against a stale reference must not be a bare `return`.** `GAUGEDONORMAXT`'s first
  draft would have locked the gauge slot out permanently on the first hard scene cut, because on the frame
  after a cut every candidate is legitimately far from the stale gauge. Found by asking what the probe
  measures then, not by testing.
* **`bin\remix_dump.log` is ~1.15 GB and append-mode.** Find run boundaries with `grep -abo "Remix
  run-start:"` over a `dd`-skipped tail, slice once with `tail -c +N` into the scratchpad, then grep that.
* **`Remix timing:` and `Remix stats:` go to `bin\log\RPCS3.log` only.** `Remix live:` goes to both.
  `poisoned=` is on `Remix stats:`, and it is what makes the `:23263` subtraction exact.
* **Do not build a launcher dry-run by `head -N` past the `cd /d "%~dp0bin"` line** — the next line starts
  the game.
