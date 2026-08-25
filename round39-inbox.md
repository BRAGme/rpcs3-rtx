# Round 39 inbox — RPCS3 RTX Remix backend, Haze (BLUS30094)

Written at the close of round 38, 2026-08-24. Branch `remix-backend`, HEAD `44276ae06`, working tree carries
rounds 37 **and** 38 uncommitted (rounds 32-36 are committed and pushed).

| artefact | SHA256 (first 16) | note |
| --- | --- | --- |
| `bin\rpcs3.exe` | `447D3904D1E969B2` | built this round, MSBuild exit 0, **0 errors, 1 warning** |
| `bin\rpcs3-next.exe` | `447D3904D1E969B2` | **identical — the copy was made** |
| `bin\remix\d3d9.dll` | `16A0B512F33EBB66` | **UNCHANGED.** Nothing deployed into `bin\remix\`. |

**The tree did NOT move under this round.** At the start `bin\rpcs3.exe` and `bin\rpcs3-next.exe` were
byte-identical (MD5 `0A5EECD31AB0641EAAA086FCAA21E34B`) and `rpcs3-next.exe` hashed `56A0A57DA51FF02E`,
exactly round 37's binary; HEAD was `44276ae06` at both ends; `git status` showed only the same untracked
files. Second round running where the concurrency trap did not fire. **Hash both again anyway.**

The one warning is the pre-existing C4723 "potential divide by 0", now at `RemixGSRender.cpp:11067` (it was
`:11065`; it moved by 2 because this round inserted lines above it). **Zero new warnings from this round.**

---

## READ THIS FIRST: four user reports, one defect, and the fix may be structurally unable to fire

The jiggling dumpsters and lockers, the teleporting light fixture, the weapon that lags when you turn, and
"geo following the camera" are **the same bug**.

A draw whose render source has not yet produced its identity-donor draw *this* frame is divided by
`fused_donor(t-1)` and rendered with `camera(t)`. The residual is `D = V(t)·V(t-1)⁻¹`, which for a point `p`
is `p -> eye + (p - eye)·dR` — a rotation **about the eye** by one frame of camera turn. So

> **displacement = (distance from the eye) × (camera turn per frame)**

Same defect, three magnitudes: millimetres on the weapon at 0.73 m, a hand-width on a dumpster, metres on a
fixture across the room. That is why three rounds chased it as three bugs.

**MEASURED three independent ways** (full tables in `docs\remix\KNOBS.md` "Round 38"):

1. 4000 **consecutive** `Remix vmbasis:` frames (12311..16395, step 1 on all 3999 gaps). Express the
   recovered viewmodel 3x3 in the camera basis of frame `t+s`; `s = -1` is a **strict minimum** in median
   (0.2287 deg), mean (0.3750) and p95 (1.2348) over `s` in {-3..+3}, against `s = 0` at
   0.2984/0.4655/1.5531. `s = -2` and `s = +1` are both worse than `s = -1`, so it is one frame and not
   "any decorrelation helps". Lagged beats current on 69.6% of frames (~25 sigma).
2. All **48** `Remix picked:`/`pick-deep:` lines for `vp=830d7d1b9681c475 fp=c61b0b9586dd67fb` — the family
   the round-38 brief named — read `ref=anchor_prev`, `anchor_frame == frame-1`, `cam_age=0`. 48 of 48.
3. Magnitude: `0.734 m x 8.856 deg = 0.113 m` against a measured max `|d cpost|` of `0.1248 m`.

**This is not round 32's tautology.** Round 32 was right that "`ref=anchor_prev`, therefore it lags a frame"
restates the name. Measurement 1 uses a different program, different fields, and never reads `ref=`.

### The premise that justified the stale branch has been falsified

The comment above the branch says *"Last frame's anchor is used for draws that arrive before the frame's
first identity draw. That is never worse than the old path: the elected camera's reference was always a
frame old."* MEASURED on the last run: `world_ref_fresh=3598650` vs `world_ref_stale=97515` — **97.4% of
draws now carry a current-frame elected camera.** `CAMRELATCH` removed the baseline that branch compares
itself to. Population: `gauge_used=2825794 gauge_prev=641662` = **18.5%** of world draws (an earlier window
in the same log reads **33.8%**).

### Shipped: `RPCS3_REMIX_GAUGECURDIMS=1` — and PRIORITY 1 IS TO CHECK WHETHER IT FIRED AT ALL

When the current-frame exact-key anchor lookup misses, try a **this-frame** anchor on `(target, clip)` alone
before falling back to last frame's. Same relaxation `GAUGEPREVDIMS` already ships ON for frame `t-1`, one
frame fresher; `find_gauge_anchor_shape(…, 0, 0, …)` already existed for the tail rescue.

**The honest risk, stated before the run: this route may be structurally dead.** A draw that arrives before
its own source's donor may arrive before *every* donor, in which case no this-frame anchor exists and the
lookup always misses. So `gauge_cur_avail` is incremented **whether or not the knob is armed**.

**Read these on `Remix live:` in `bin\remix_dump.log`, in this order:**

| counter | reading | what it means |
| --- | --- | --- |
| `gauge_cur_avail` | **0** | **the route is dead on this title.** Stop tuning it. Go to `DEFERPREANCHOR=1`. |
| `gauge_cur_avail` | > 0 | the route can fire; `gauge_cur_dims` says how often it did |
| `gauge_cur_dims` | > 0 | draws rescued. `gauge_anchor_prev` must fall by exactly this. |
| `gauge_prev_camfresh` | -- | prev-branch draws whose **elected** camera was current: the size of the third, untried route |

Visible check with no log: Ctrl+click a jiggling dumpster. `Remix picked:` read `ref=anchor_prev
anchor_frame=<frame-1>` in round 37 and must now read `ref=anchor anchor_frame=<this frame>`.

**Blast radius:** this hands a different render source's this-frame anchor to a draw whose own source has
not anchored yet. Static scenery in the wrong place, or the world shearing when you turn, is this knob.
`GAUGECURDIMS=0` reproduces round 37 byte for byte, no rebuild.

### The already-proven fallback, and its price is known

`RemixGSRender.cpp` records beside this branch that `DEFERPREANCHOR` *"was root-caused, shipped, and
CONFIRMED by the user to stop the props sliding — then switched back off on 2026-08-16 because it cost
~14 fps"*. **The user has already seen this exact defect fixed once.** Round 32 then narrowed its population
by 35.8% via `DEFERPREVONLY` (default 1, inert while `DEFERPREANCHOR=0`), so a re-test today buffers ~64% of
what cost 14 fps. If `gauge_cur_avail` is 0, that is round 39's lever and its cost is known in advance —
do not re-derive it.

Third route, if both fail: divide the prev-branch draws by the **elected camera** when
`m_active_camera.latch_frame == m_frame_counter`. `gauge_prev_camfresh` sizes it. It needs no new machinery,
only a fourth arm in the same `if` chain. Known objection: the elected camera is built at `512x288` while
these draws are `1024x576` — see the next section, which measures that that does **not** displace the eye.

---

## Refutations. Each was pre-registered; each read otherwise; that is the finding

1. **NOT a stale camera *basis* applied to a correct gun.** That model predicts displacement
   `(Q - I)·cpost` with `Q = M(t)M(t-1)ᵀ`. Direction agreement with the measured displacement has median
   `cos = -0.348`; only **4.1%** of frames exceed `cos 0.9`.
2. **NOT a held or reused transform.** The `pre=` matrix is bit-identical between consecutive frames on
   **0 of 3999** pairs; the update-gap histogram is `{1: 3998}`.
3. **The anisotropic basis error is a SEPARATE defect — do not fold it in.** `D` is projective, so a stale
   divisor should leave anisotropy that grows with camera motion. It does not: recovered column lengths
   span 0.9948..1.0054, anisotropy `max/min - 1` median **1.176e-3**, and it is the same with the camera
   fully stationary (n=19, median 1.361e-3) as while turning >1 deg (n=328, median 1.470e-3);
   `corr(turn, anisotropy) = 0.142`. **The small n=19 is the weak point of that reading — retake it.**
   Also `world_div_f64 = 3467456 = gauge_used + gauge_prev` **exactly**, so `GAUGEF64` is fully engaged and
   this residual is not f32 conditioning either. Round 34's ±0.28% anisotropy is still unexplained.
4. **The `1024x576` vs `512x288` split does not displace the camera.** `Remix anchor-gauge:`, n=**41,718**:
   **41,036 (98.4%)** carry `anchor_clip=1024x576` against `elected_clip=512x288`, and on those the anchor
   camera and the elected camera agree to **median 1.07e-4 units, p95 4.88e-4**. This upgrades round 36's
   rate-mismatch argument to a direct measurement. **Named limit:** 0.75% of lines (311) read `worst > 1.0`,
   max **1787.66** — a real tail nobody has explained, but not the mechanism.

### An instrument trap this round nearly walked into, recorded so it is not walked into again

An earlier pass of the vmbasis analysis reported "median frame-to-frame gun rotation exactly 0.0000 deg" and
was about to become a finding about held transforms. It was **the instrument**: `acos((tr-1)/2)` on
6-significant-figure output has a quantisation floor near **0.14 deg**, and below that it returns 0 exactly.
Every rotation magnitude in round 38 uses `‖B-A‖_F = 2√2·sin(θ/2)` instead. **Any angle computed by `acos`
of a trace, from a printed matrix, is suspect below ~0.15 deg.**

---

## Priority 2 — the helmet. A fourth pair, and two corrections to how round 37 was read

Round 37 asked "either a fourth pair draws it, or the UI route does not prevent clipping". **It is a fourth
pair, and it is the second-busiest of the four.** MEASURED over the last 200 MB, every line carrying
`C61753D31FB96507`:

| vp | fp | `fpcandidate` | `uiwrap route=2d` | was armed? |
| --- | --- | --- | --- | --- |
| `830d7d1b9681c475` | `479890ff55f1d96e` | 424 | **331** | yes |
| **`9f591b6a6b825612`** | **`cbede4eb45f0fd25`** | **416** | **0** | **NO** |
| `f39f504649b6f442` | `4afa02b3dbbe9b7e` | 246 | **65** | yes (`VP2` slot 0) |
| `830d7d1b9681c475` | `609a4216b89e296a` | 6 | 0 | yes (`VP2` slot 1) |

All four draw `vtx=59` — same mesh. Added as `VP2`/`FP2` slot 2 (backing array is `std::array<u64, 8>`).
The extra-pair route itself is proven working: slot 0 produced 65 `route=2d` lines.

**Correction 1 — you cannot read `uiforcepairs=` or `ui_forced_pair=` in `remix_dump.log`.** They are on
`Remix stats:`, which goes **only** to `bin\log\RPCS3.log` — **0 of 13,495** `Remix live:` lines in the last
200 MB carry `ui_forced_pair`. Round 37 read them correctly *in RPCS3.log* but wrote the pre-registration as
"the knobs line", which is the `Remix live:` line. RPCS3.log is exclusively locked while the emulator runs,
so this reading only exists **after it exits**. Round 38's three new counters are on **both** lines for
exactly this reason. **Pre-register nothing on `Remix stats:` that a play-test has to judge.**

**Correction 2 — round 37's `~2/frame` prediction failed, and its stated alternative was wrong.** MEASURED
from RPCS3.log over eight consecutive intervals spanning 623 frames, `ui_forced_pair` rose at **exactly
1.000/frame with zero remainder** (93/93, 83/83, 65/65, 70/70, 86/86, 82/82, 72/72, 72/72) while
`uiforcepairs=2`. Round 37 said "if it stays at 1/frame the extra list did not parse, and `uiforcepairs=`
says so" — the list **did** parse. The extra pair simply does not draw every frame (331 vs 65 `uiwrap`
lines, ~5:1). **A rate prediction needs each pair's per-frame incidence, not just its existence.**
Pre-registration for round 39: `uiforcepairs=3`, and `Remix uiwrap:` gaining `route=2d` lines for
`vp=9f591b6a6b825612`.

`ui_skipped` is **live again** — 0 -> 7163 within one run in this log, so round 37's "frozen since frame
9678" no longer holds and it *can* be read as evidence if the helmet vanishes. (It then froze at 7246 for
that run's last eight intervals.) Round 37's other helmet conclusions still stand and should not be
re-derived: there is no shadow flag and no overlooked category bit, and if the UI route ever fails to
deliver "visible, no shadow, no clipping", only a shadow-only proxy submission can.

The user has now asked for the helmet **seven times**. If the fourth pair does not close it, the next honest
answer is the proxy submission or "cannot be delivered", stated with the measurement.

---

## Priority 3 — the plant walls. `peak=64 budget=64` was the CLAMP, and the real limiter is eviction

`STATICINDEXBUDGET` was clamped to `[1, 64]` and the launcher armed **64**. Round 35's clamp audit recorded
armed-equals-ceiling as *intended* — and as a value it was — but **a saturated window and a correctly-tuned
window print the same thing**, so `peak=64 budget=64` could never be A/B'd. Ceiling raised to **512**;
launcher takes one step to **128**. That is the round-31/round-33 clamp trap for a third time.

MEASURED, last run:

```
Remix static-index: entries=11498 triangles=2252883 rebuilds=12228 deferred=841
                    stale=10 dropped=831 peak=64 budget=64 resident=115 evicted=11376 nomesh=7
```

Round 37's "relieved" reading (`peak=56 budget=64 dropped=0`) is real and is in this log — on a **388-entry**
window. Every **11,498-entry** window reads `peak=64 budget=64` with `dropped` in the hundreds to thousands.
Different scenes; the small one is not the one the plant walls are in.

**The new limiter is the eviction population.** `resident=115` against `entries=11498` = **98.9%** of
static-index entries hold a `mesh_hash` whose mesh the reaper already freed, and the backend's own
accounting says *"evicted = … the reaper took it -> next draw of this entry takes the `dropped` exit and
renders nothing"*. Verified against current bytes: both `dropped` exits `return` before the refusal
accounting (hence never in `Remix world-refused:`) and **both are gated by
`m_static_index_rebuilds >= budget`**. So the budget is the throttle that turns an eviction into an
invisible frame, and `MESHIDLE=600` (~20 s at 30 fps) is what makes the evictions. **If 128 does not help,
the lever is the reaper — a static-index-scoped idle window, a code change, not this knob.** Ladder if it
partly helps: 256, then 512, watching `mesh_live=` on `Remix live:` beside the frame rate.

---

## Carried, untouched this round

- **`VMROTLOCK` stays at 0 on purpose.** Round 37's residual-pose A/B and round 38's one-frame lag are
  different defects; arming both would confound the play-test. Test `GAUGECURDIMS` first, then `VMROTLOCK`.
- **`VMBASISEVERY=1 VMBASISVTX=3649 VMBASISMAX=4000` stays armed on purpose** — the 4000-frame series must
  be re-taken so the `s`-shift table can be recomputed. **Pre-registered: `s = 0` becomes the minimum.**
- Round 37's algebraic identity for the viewmodel residual (`cos(relpre) + cos(relpost) = dotpre[0] - 1`)
  is untouched and still stands; round 38 measured the *time* axis, not the pose axis.
- **Frame time.** Not measured again, two rounds running. Round 35's instruction still holds: either blank
  `SKIPEXTENTVP` and arm `DRAWAUDIT=0` together and say so, or attack `decode` (3.93 ms).
- **`rtx.fallbackLightMode` is 0** in `bin\rtx.conf`; round 33 asked for 2. That file is the user's.
- **STILL OWED, third round running:** `classify_draw`'s "IGNORE is a no-op on the API draw path" wants
  confirming by disassembling `bin\remix\d3d9.dll`; `RemixTransforms.h`'s "What `DRAWAUDIT=0` costs" still
  names neither `m_streak_measured` consumer.
- **The weapon-screen numerals** (round 37 priority 4) were not re-picked. `TEXREHASH` stays unarmed —
  round 37's refutation (1,767 lines carrying `86885A0E60751491`, zero `texstale`) is not disturbed by
  anything here.

## Play-test card — round 38

Launch as usual (`launch-haze-remix.cmd` -> `bin\rpcs3-next.exe`, **`447D3904D1E969B2`** — hash it).

| what to look at | where to read it | one-line revert |
| --- | --- | --- |
| **Do the dumpsters/lockers stop jiggling? Does the fixture stop teleporting?** | Ctrl+click one: `Remix picked:` must read `ref=anchor anchor_frame=<this frame>` | `set "RPCS3_REMIX_GAUGECURDIMS=0"` |
| **Did the fix fire at all?** | `Remix live:` — `gauge_cur_dims` > 0, `gauge_anchor_prev` down by that much | -- |
| **`gauge_cur_avail=0`** | route is structurally dead; stop tuning it | round 39: `set "RPCS3_REMIX_DEFERPREANCHOR=1"` (~14 fps, user-confirmed to work) |
| **Weapon still lagging when turning?** | **TURN LEFT AND RIGHT.** Re-take 4000 `Remix vmbasis:` lines; `s=0` must become the minimum | `set "RPCS3_REMIX_VMBASISEVERY=0"` |
| **Scenery in the wrong place / world shears when turning** | this knob and only this knob | `set "RPCS3_REMIX_GAUGECURDIMS=0"` |
| **Helmet: visible, no shadow, no clipping?** | `Remix uiwrap:` gaining `route=2d` for `vp=9f591b6a6b825612`; `uiforcepairs=3` in `bin\log\RPCS3.log` **after exit** | drop slot 2 from `UIFORCEPAIRVP2`/`FP2` |
| helmet VANISHED | `ui_skipped` (live again) | same revert |
| **Plant walls still disappearing?** | `Remix static-index:` — `peak=` must stop pinning at `budget=`, `dropped=` must fall | `set "RPCS3_REMIX_STATICINDEXBUDGET=64"` |
| frame rate dropped after the budget step | `mesh_live=` climbing across the session | same revert |
| Sky / everything else | unchanged from round 37 | see round 37's card |
