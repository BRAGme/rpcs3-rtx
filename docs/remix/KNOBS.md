# RTX Remix backend tuning knobs

The Remix RSX backend reads a set of `RPCS3_REMIX_*` environment variables. They are the
bisect and tuning surface for the backend: most of them exist so a single run can attribute a
visual change to one specific decision in the transform/material pipeline rather than to a
whole milestone.

## How to set one

Set the environment variable in the shell (or launcher/shortcut) that starts rpcs3, before
launch. Example:

    set RPCS3_REMIX_SKYHASH=2
    rpcs3.exe

A subset is also exposed on the **RTX Remix** page in *Configuration* (see the `in GUI`
column). Where both exist, the environment variable wins when it is set, so existing test
scripts keep working unchanged; the config value is the fallback. A handful of the config
entries are re-read per frame and so take effect without a restart -- the GUI labels those
that do not with "(restart)".

Most environment knobs are latched once per process (`static const`), so changing one requires
restarting rpcs3.

Value conventions:

- Boolean knobs read through `env_flag` are true for any value whose first character is not
  `0`, and false when unset. These cannot express "explicitly off", which is why several of the
  on-by-default knobs are read as integers instead.
- Integer knobs read through `env_u32` fall back only when unset, so `0` is a usable value.
- Float knobs read through `env_float` accept finite positive values only; anything else falls
  back, which is why several of them use a negative sentinel internally to mean "unset".

## Provenance

This list is distilled from the in-code documentation block in
`rpcs3/Emu/RSX/Remix/RemixTransforms.h` (lines 587--1217) plus a repo-wide grep for
`RPCS3_REMIX_*` read sites across `rpcs3/Emu/RSX/Remix/`, `rpcs3/Emu/system_config.h` and
`rpcs3/rpcs3qt/`. It can lag the code. **The code is authoritative** -- when this file and the
source disagree, the source is right.

Defaults shown are the effective default of the whole read path: where a knob falls through to
an `Emu/system_config.h` entry, the config default is what is listed.

---

## Runtime

| name | default | effect | in GUI |
| --- | --- | --- | --- |
| `RPCS3_REMIX_DLL` | empty (`<exe dir>\remix\d3d9.dll`) | Path to the Remix runtime DLL. The runtime is deliberately not placed next to `rpcs3.exe`, because a file named `d3d9.dll` there would be picked up by anything else in the process resolving that name. | |
| `RPCS3_REMIX_FARPLANE` | `1000` | Far plane used by the hardcoded camera, overridable at runtime so a unit-scale sweep needs no rebuild. Values must be greater than 1. | yes |

## Camera

| name | default | effect | in GUI |
| --- | --- | --- | --- |
| `RPCS3_REMIX_CAMHOLD` | `300` | How many consecutive flips the last resolved camera is kept when a frame produces no candidate of its own. `0` restores the unconditional per-frame latch, where a single candidate-less frame dropped the camera and the backend submitted its origin fallback instead. 300 is roughly 5 s at 60 fps. | yes |
| `RPCS3_REMIX_NOCAM` | off | Force the milestone-1 hardcoded camera plus the debug triangle. | |
| `RPCS3_REMIX_CAMRELATCH` | `1` (on) | Refresh the active camera's matrices and reference inverse *during* the frame, as soon as a candidate from the same source and within the discontinuity tolerance is seen, instead of only at flip. The flip-only latch is image-exact for a rasteriser (`world * V_old * P_old = fused_now`) and wrong for a path tracer, which reads the world pose: every static instance then absorbs the camera's per-frame motion, which is the residual prop wobble. `0` restores the flip-only latch and is the A/B for the fix. It cannot switch or validate a camera -- only refresh one that is already active and already continuous. Counters: `cam_relatch`, `world_ref_fresh` / `world_ref_stale`. | |
| `RPCS3_REMIX_GAUGEANCHOR` | `1` (on) | Divide every world draw by the *main pass's own* view x projection -- captured from that frame's first `WORLDIDENTITYVP` draw on the same render source, whose world transform is the identity so its fused matrix is `V*P` exactly -- instead of by the elected camera's reference inverse, and rebuild the submitted camera from the same anchor so poses and camera share one gauge. Round 1 made the reference same-frame (88 % fresh) and the shake stayed, which leaves the gauge rather than its age: on Haze the elected camera is a 512x288 auxiliary pass while the world draws are 1024x576, the election alternates between two such passes, and the two disagree by 512/510 == 256/255 of basis scale. `0` restores the elected camera's reference exactly and is the A/B. Counters: `gauge_used` / `gauge_prev` / `gauge_absent` (they partition the reference branch), `gauge_contested` (a listed program also drawing another pass on the same key -- must stay 0), `gauge_cam` / `gauge_cam_failed`. Self-check: `Remix worldid-draw:` translation collapses to ~0 on `resolved=1` lines. | |
| `RPCS3_REMIX_GAUGETRACE` | `600` | Frames of `Remix gauge:` lines, measuring the disagreement between the elected camera's reference and the anchor as basis delta and translation, plus one `Remix camvp-viewport:` census line per elected camera program per stats window (the direct test of the one-pixel-viewport hypothesis). Re-armed by any successful pick. Unaffected by `GAUGEANCHOR`, so the A/B run measures the same quantity. `0` disables the trace only. | |
| `RPCS3_REMIX_GAUGESLOTS` | `16` | How many render sources may hold a gauge anchor at once (1..16; `4` restores round-2 sizing). A slot is only reusable once its anchor is two frames old, so on Haze the shadow pass (2048x2048), the auxiliary pass (512x288), the main pass (1024x576) and anything else drawing that frame competed for four entries and the loser's draws silently fell back to the elected camera -- one of the two previously untraced exits behind `gauge_absent` at 16 % of draws. Counter: `gauge_slot_exhausted` (and `gauge_invert_failed` for the other exit). Census: `Remix gauge-keys:`, one line per stats window listing every live slot, which is what answers "is 16 enough" from the run. | |
| `RPCS3_REMIX_GAUGEPREVDIMS` | `1` (on) | When the *previous* frame's anchor lookup misses on the exact (surface offset, target, clip) key, retry matching only (target, clip width, clip height). Surface offsets move between frames whenever the title double-buffers or reallocates; the pass shape does not, and every exact-key miss sent that draw to the cross-pass camera. The current-frame lookup keeps the exact key -- within one frame the offset is the pass's identity. Counters: `gauge_prev_exact` / `gauge_prev_dims`, which sum to `gauge_prev`. `0` restores exact-only lookup. | |
| `RPCS3_REMIX_GAUGECAMHOLD` | `30` | Frames for which a flip with no anchor at all keeps the last anchor-derived camera split instead of falling back to the elected (alternating, half-resolution) gauge. Round 2 rebuilt the camera on only 43 % of flips, so on the rest the scene divided by anchors while the camera came from the old election -- the two disagreeing at frame rate is a whole-scene shake. Only matrices are held; the camera age, the pending-switch confirmation and the per-frame vote are untouched. Counters: `gauge_cam` (current anchor), `gauge_cam_prev` (previous frame's), `gauge_cam_held` (this hold); the three partition the flips. `0` disables holding. Risk: a real scene cut with no identity draws holds a stale gauge for up to this many frames -- the tell is `gauge_cam_held` spiking at cuts. | |
| `RPCS3_REMIX_GAUGEF64` | `1` (on) | Invert the gauge anchor's fused matrix in **double** precision and carry out the `world = fused x reference` divide in **double**, narrowing only the finished world matrix back to f32. The wobble is floating-point noise, not reference selection: a fused view x projection is ill-conditioned by construction (z and w columns nearly parallel, translation row growing with camera distance from the world origin) and `mat4_invert` is a single-precision cofactor expansion -- the numerically worst algorithm for that input. Replaying the shipped algorithm bit-for-bit on the run's own `%a`-hex anchor matrices: cond2 2.8e4 at coords ~40 leaves 3.5e-5 basis / 8.6e-5 translation; cond2 8.9e7 at coords ~1780 leaves **3.2e-3 basis / 1.07 units** -- against 1.5e-9 in f64. That is the measured `ref=anchor` wobble (median `d_basis` 1.4e-3), why the large-coordinate cargo plane shakes 2-9 units while small-coordinate Selva looks almost still, and why `ref=identity` -- the one path that never multiplies by an inverse -- is bit-exact. **Both** halves must be f64: the same replay shows an f64 inverse cast back to f32 still leaves ~1.0 of translation error, because the cast inverse's large entries cancel in the f32 multiply just as thoroughly. Counter: `world_div_f64` (should equal `gauge_used + gauge_prev`). Self-check: `Remix gauge-selfcheck: err32= err64= tmag=` once per anchor per stats window -- `tmag` is the region tag and the line is unreadable without it, because the error is multiplicative. `0` restores the f32 path bit-exactly. | |
| `RPCS3_REMIX_CAMCLIPGATE` | `1` (on) | **The far-away "portal" frames, closed structurally (round 10).** With a camera lock configured, a candidate whose surface clip is neither the same as, nor exactly double, nor exactly half the session main-clip reference is refused before the vote. Haze's **lock program itself draws the 2048x2048 shadow pass**, so the shadow variant was a legitimate *primary* candidate: `Remix cam-elect: vp=7f3d3abcefc8b057 surf=0x0 clip=2048x2048 src=resolved candidates=17 cam=[-35.4 31.95 -32.05] frame=23022` -- it won at frame 23022 with the camera 30 units above a player whose gameplay frames read y=1.6-2.5 and held for ~1400 frames, and one cutscene run's tally of elected identities reads 10x `7f3d @512x288`, 8x `ad7c @1024x576`, **2x `7f3d @surf=0x0 2048x2048`**. The rule is the same same/double/half `compatible_surface` test the world-draw path already trusts at the `skip_camera_surface` site, lifted one level up so it decides which candidate may be *elected* rather than only which draws may follow one: against a 1024x576 reference, 2048x2048 fails (2048 = 2x1024 but 2048 != 2x576) while 512x288 and 1024x576 pass. The reference is the **session** main clip (textured draws only, so a material-less shadow map can never define it), falling back to the per-frame latched main clip and then to the active camera's own clip; with none of the three available every candidate is admitted, because a boot frame must not be camera-less. Placed at the head of `consider_camera_candidate`, so the split-failure recovery path inherits it rather than routing around it. Counter `cam_clipgate_refused`; census `Remix cam-clipgate:` names every (vp, clip) it refuses, bounded 16 per window. `0` restores today's admission bit-exactly. | |
| `RPCS3_REMIX_CAMFBRELATCH` | `1` (on) | **The dev-menu camera-type flicker (round 10).** When the active camera is the lock program's and the frame's winner is a *fallback*-program candidate that is pose-continuous with it, the elected **identity stays the lock** and only the **matrices** are taken from the candidate that actually drew. Haze's lock `7F3D3ABCEFC8B057` stops producing candidates for stretches (its matrices become unsplittable near the vertical view, and the split-failure recovery additionally requires the surface to match the *active* camera's, so once the election has flipped away the recovery is locked out); `AD7CE9D672A0BF6B` then wins, and because the two express the same pose on different surfaces (positions within 0.2 units), `camera_source_changed` treated every handover as a full identity change needing 12 confirm frames. Measured: 11 `cam-elect` lines in one run alternating exactly those two identities, flips >=40 frames apart, `candidates=1` throughout -- which the lock filter *guarantees* in either outcome and therefore does **not** mean only one pass drew. Implemented by rewriting `m_frame_candidate` rather than short-circuiting the machinery, so the ordinary latch installs it, the source-change test sees no change and the age resets. Guarded exactly as the mid-frame relatch is (`archetype`, `has_reference`, `group_count`, view within the discontinuity tolerance, projection within 1e-2), so a genuinely different lens falls through to today's confirm-and-switch. Counter `cam_fb_relatch`; the visible test is `cam-elect` going quiet. `0` restores the flicker. | |
| `RPCS3_REMIX_ANCHORSTICKY` | `1` (on) | **Selva's "geometry follows the camera" and the carrier/cargo split (round 10).** Elects the per-key gauge anchor by **continuity with its own previous frame** instead of by draw order. `capture_gauge_anchor` is first-draw-wins per (surface, target, clip); `gauge_contested` read 462,080 mid-session and 550,394 at close-out (vs ~0 in prior runs) and **all 61 `Remix gauge-contested:` lines in the Selva run are one pair on the main key**: holder `BD1C10DF5703E559` -- a genuine identity-world donor (absolute world-span strips, x from -1373 to +1113; a `worldvp` probe shows its divided world = identity to 1e-15) -- against contender `AD7CE9D672A0BF6B`, which is also on `WORLDIDENTITYVP` but submits **non-identity** draws here (deltas 1.7-7.4 drifting over time: a moving object). While BD1C draws first the tripwire correctly refuses AD7C; on frames where BD1C is culled, first-draw-wins hands AD7C the slot and `world = fused x wrong_ref^-1` leaves a camera-correlated residue on every draw -- geometry tracks the viewpoint while the sun (a Remix light, world-anchored by the runtime) stays put -- while draws issued before the frame's first identity draw divide by the *previous* frame's anchor, putting two references in one frame. With the knob on, a frame's first same-key donor that disagrees beyond the discontinuity tolerance is **parked**, a later continuous donor installs and displaces it, and any key that saw no continuous donor all frame **promotes** its parked candidate at flip -- so a real cut converges in exactly one frame and cannot be held. In the gap the divide's existing previous-frame anchor path and `GAUGECAMHOLD` cover lookups, which is today's behaviour for anchor-less frames. The contested tripwire is untouched. Counters `anchor_parked` / `anchor_recap` / `anchor_promoted`; census `Remix anchor-elect:` fires whenever a key changes holder, and `Remix gauge-contested:` now also carries `vpscale=`/`vpoffset=`/`cont=`. `0` restores first-draw-wins bit-exactly. | |
| `RPCS3_REMIX_GAUGEDONORMAXT` | `0` (off) | **Round 44 -- round 30's specified-but-never-shipped fix, and the one knob of that round that changes pixels.** Whole world units. `WORLDIDENTITYVP` drives **two** gates and only `WORLDIDMAXT` ever learned about a threshold: at submit, `keep_resolved` sees a draw 900 units from the origin and correctly keeps its real transform; at capture, `capture_gauge_anchor()` (`RemixGSRender.cpp:1810`) has already installed that same fused matrix as the **whole frame's world gauge**, qualified on the vp-hash list alone -- it never looks at the translation. When non-zero, a candidate whose own placement (probed as `fused * slot->inverse`, translation read from row 3 after dividing out `m[3][3]` -- character for character `report_gauge_trace()`'s own measurement, so this and `Remix gauge: translation=` are the same number) exceeds this may keep its transform at submit but may **not** donate the gauge. **MEASURED, round 44:** of 759 `Remix worldid-draw:` frames carrying two or more rows, **559 have every row reporting one identical pre-translation, bit for bit across unrelated meshes** (`f=53640`: `vtx=160`, `vtx=224`, `vtx=518` all read `t=[1.579 -132.9 -2.179]`) -- guest motion cannot do that. `Remix gauge: translation=` over 17,386 frames has mean **41.99**, max **1718.67**, and exceeds 128 units on **7.36%** of frames. `E40BF80AF519848A`'s raw vertex box never moves (30 of 36 mesh identities exactly `0.0000` over spans up to 14,500 frames) while its transform swings 0 -> 21.75 -> 966.5. **`ANCHORSTICKY` does not already cover this**: its test is `matrix_relative_delta <= 0.8`, a *relative* whole-matrix measure, and a donor 21.75 units off the origin scores nowhere near 0.8. Sticky asks *did the gauge change*; this asks *is the gauge wrong*, and a stably-wrong gauge is perfectly continuous. **Cannot starve the slot:** `AD7CE9D672A0BF6B` puts 2,486,813 of 3,316,291 draws (75.0%) at `|t| <= 1` and only 242,939 (7.3%) above 32. Programs on `WORLDIDMAXTEXEMPTVP` are exempt for the same reason they are there. An offside donor is **parked**, not dropped, so `promote_parked_anchors()` still converges a scene cut in one frame -- a bare `return` would lock the slot out permanently, and `anchor_parked` therefore rises. Counters `gauge_donor_offside` (counted **whether or not the knob is armed**, so an unarmed run still sizes the population) and `gauge_donor_refused`. Watch `gauge_absent` -- if it rises much above 295,391 the gate starved the slot. `0` restores round 43 byte for byte. |  |

## Geometry and world placement

| name | default | effect | in GUI |
| --- | --- | --- | --- |
| `RPCS3_REMIX_SKIPCCCONST` | `1` (on) | **The `lay_other` matcher widening (round 7).** When choosing the terminal instruction of the HPOS chain, walk past a full-mask writer that is executed **under a condition code** *and* whose every consumed source is a **constant**. Round 6 read seven cached `.vp` leaders behind `fail=lay_other` offline and found the same idiom in all of them: the last full-mask writer to `o0` is a `MOV` from a constant under `cond != always` -- the particle/geometry cull idiom, "if culled, park HPOS at a constant off-screen point". Starting the backward walk there finds a MOV whose only operand is a constant, the source test fails two steps later, and the real chain sitting directly behind it is discarded as `no matrix chain into HPOS`. Both terms are required: an *unconditional* constant write really is all such a program does with HPOS (refusing it is correct), and a predicated write reading a temp is a real value that must not be stepped over. **Honest expectation:** affected programs move OFF `lay_other` onto an inner-chain refusal reason -- getting further with a better-named error is the deliverable; a placed draw is not promised. `0` restores the old terminal choice exactly. | |
| `RPCS3_REMIX_DRAWNOWORLD` | off | Submit a draw whose world transform could not be resolved at the identity anyway. Off by default because identity means raw model-space vertices at the world origin, i.e. every unresolved draw piled on top of every other one. | yes |
| `RPCS3_REMIX_NOWDIV` | off | Submit the stored `ATTR0.xyz` even for programs whose ucode divides the position by `ATTR0.w`. The bisect knob for the per-vertex divide. | yes |
| `RPCS3_REMIX_WDIVWALK` | `1` (on) | **The giant-geometry fix (round 8).** Inside `match_wdivide`, choose the position temp's producing instruction by walking the writer list backwards and **skipping writes whose xyz mask is empty**, instead of trusting the single most recent VEC writer. The old `prog.last_temp_writer(temp, before)` was the whole bug: Haze's character/prop compiler emits `MUL rN.xyz = ATTR0.xyz * RCP(ATTR0.w)` (the divide) immediately followed by the **w-only** `MOV rN.w = c[467].z` that completes the homogeneous vector, then the matrix group. The last writer before the group is therefore the w-only MOV, the MUL/mask test failed, the matcher returned before its RCP back-scan ever ran, `has_wdivide` stayed false -- and the backend submitted the **raw signed-16-bit quantised attribute**. That is the ship at extent 9211, the "incorrect bones" character at 669.7 and the giant NPC weapons: a clean instance basis around a mesh that was never dequantised. The recorded `areason=sca-xyz` was `match_const_affine`'s own (correct) refusal for the RCP at slot 4 -- the *last* matcher's exit, not the reason. A w-only write cannot alter the submitted xyz nor hide a position term, which is the rule `match_const_affine`'s hop loop already states in as many words and `match_prescale` already implements; `match_wdivide` was the only position matcher that did not. **Sweep over every cached `.vp` on this title: 29 carry the wdivide MUL shape, 25 select the identical instruction under both policies, exactly 4 flip refuse->match (`57A12323F22F4988`, `4D5A87BFFBCE0717`, `96EDAAED0C27FD05`, `1D9A973AF5CD1514`), zero regress.** Nothing downstream of the selection moves -- the replay is the existing per-vertex divide, proven on ~6M divides per run, and the RCP back-scan still refuses any intervening VEC write to the scalar lane. Counter: `wdiv_shadowed=` on `Remix live:` (strict subset of `wdiv=`). Census: `Remix wdiv-shadowed: vp=... skipped=N`, once per program per run. `0` restores the single-last-writer selection bit-exactly -- the giants must come back, and that A/B is the whole attribution. | |
| `RPCS3_REMIX_MADCHAINMIX` | `1` (on) | **The Selva canopy arm (round 9).** In `match_mad_chain`, a third pass tried **after** the strict and relaxed ones, so no program that matches today changes matcher. It accepts a fused matrix group whose per-row scalars are broadcast reads of *different* temps/lanes, provided each of them chases back through **additive accumulator hops only** (MAD addend / ADD operands, depth <= 4) to the **same base temp** with lanes exactly `{x, y, z}` -- and the row-to-component mapping is taken from the **constant slot**, never from the broadcast lane, because a lane of a scratch temp says nothing about which matrix row it feeds. Named from a full 47-slot decode of `DF46F03B1B7AB8A4.vp`: the group is `MUL r0 = r4.yyyy*c[1]` @10, `MAD r0 = r1.wwww*c[0]+r0` @32, `MAD r0 = r1.wwww*c[2]+r0` @42, `ADD o0 = r0+c[3]` @44, so the single-source rule refuses it -- yet slot 9 is a clean per-vertex divide `r4.xyz = I0.xyz * RCP@2(I0.w)` and each odd scalar chases to a distinct lane of r4 (`r1.w@27 = r2.x*c467.z + r4.x`, `r4.y` direct, `r1.w@40 = r1.w*c467.z + r4.z`). The additive terms **are the wind sway**; the chase steps over them, so the replay is a **static canopy** -- the honest trade, and the contract round 6 anticipated for this family. Counters `madmix=<programs>/<draws>` on `Remix live:`; census `Remix madmix: vp=...`, once per program per run. `0` restores the single-source rule bit-exactly. | |
| `RPCS3_REMIX_MADLANEMAP` | `1` (on) | **The Selva tree tops (round 17).** `MADCHAINMIX` above additionally requires each matrix row to resolve to the lane matching its own constant-derived component -- it assumes the per-vertex divide wrote x, y, z into lanes x, y, z, which is what `DF46F03B1B7AB8A4` does. The two canopy programs captured by `UCODESTORE` do not: `f7f12d5d15bb9c37` writes `MUL r4.xyw = v0.xyxz * r4.wwww` (z parked in lane w) and `c1f88035801f88de` writes `MUL r0.xzw = v0.xxyz * r1.yyyy` (y in lane z, z in lane w), so both are refused with `areason=no-chain` and never reach the scene. This arm reads the permutation out of the divide instruction's own writemask and source swizzle -- it *proves* the lane-to-component mapping rather than relaxing the rule -- and lets the same permuted divide satisfy the `wdivide` step, without which the rescued group would be applied to the raw quantised attribute and render blown apart from the inside. Offline sweep over all 111 cached `.vp` (shader cache plus `remix_ucode`): **5 programs move, every one currently refused with `groups=0`**, so nothing that renders today can change -- `f7f12d5d15bb9c37`, `c1f88035801f88de`, `61ed1d272c0653aa`, `1d22398b18a0e97d`, `5888b152531b2d91`. `f7f12d5d15bb9c37` carries no sway and replays exactly; the other four carry SIN/COS wind the chase drops, so they replay static. Counters `madlanemap=<programs>/<draws>` on `Remix live:`; census `Remix madlanemap: vp=... lane_x= lane_y= lane_z= wdiv=...`, once per program per run, where `wdiv=1` is the field that says the divide was accepted. `0` restores round 9's identity-lane rule bit-exactly. | |
| `RPCS3_REMIX_UCODESTORE` | `1` (on) | **The permanent ucode capture (round 9).** Writes the raw vertex ucode of every program whose position decode this backend refuses (`archetype == unknown`, or the innermost operand of the position chain is not an attribute) to `bin\remix_ucode\%016llX.vp`, once per program per run, size-checked, from `fingerprint_for` -- the one site that is by construction once per unique program and has `current_vertex_program` in scope. **Why it exists:** `bin\cache\...\shaders_cache\raw` is populated *only* by `rsx_cache.h::store`, which runs when the GL/Vulkan backend compiles a pipeline. The Remix backend never compiles one, so that directory's newest files predate every Remix session -- which is why the four `mad-src0-not-input` foliage programs (`c97cd1531ac480d8`, `c6eae522652f742e`, `5a278d19120f3e30`, `392df4233aaefbd1`) and the Selva canopy pair (`f7f12d5d15bb9c37`, `c1f88035801f88de`) have never been decodable offline despite printing a refusal in every run. This is the same raw write, from the backend that actually runs, into a directory the real cache never touches. No behaviour change. Counter `ucode_stored=<ok>/<failed>`; census `Remix ucode-store:`. `0` disables. **ROUND 26 adds ONE exception to the refusal filter: the program `RPCS3_REMIX_UIDUMPVP` names is captured too.** Decoding the six particle programs out of this directory is what turned 22 rounds of guessing about the missing smoke into an exact answer (they are VP-expanded point sprites), and the HUD font's remaining open question -- which per-vertex term doubles the rectangle -- is the same shape of question. UI programs are `arch=fused` with `inner_is_input`, i.e. **healthy** by the filter above, so they had never been captured. One extra file per run, no pixels, and inert when `UIDUMPVP` is unset. | |
| `RPCS3_REMIX_STRICTINPUT` | off | Refuse any draw whose position chain never reached the vertex attribute (the "innermost operand is not an attribute" population). Diagnostic only. | yes |
| `RPCS3_REMIX_RTVERTS` | `32` | Vertex ceiling for the 3D render-target-feedback gate: a draw that samples a bound colour/depth surface is treated as a post-process pass only when it is also small enough to be a full-screen quad. `0` disables the shape test and refuses on the address alone. | yes |
| `RPCS3_REMIX_POSAFFINE` | `1` (on) | Honour `vp_fingerprint::has_const_affine` and refuse draws whose recognised `pos = attr * c[S] (+ c[B])` decode could not be rebuilt, instead of silently submitting the raw quantised attribute. `0` restores the older behaviour. | |
| `RPCS3_REMIX_HPOSINDIRECT` | `1` (on) | Reach an HPOS writer through the register it was parked in, instead of requiring each of the four components to be written by a DP4/DPH landing on HPOS directly. `0` discards any 4x4 whose rows did not all land on HPOS. | |
| `RPCS3_REMIX_FULLCHAIN` | `1` (on) | Under a fused active camera, fold the whole matrix chain of a layered program rather than only its outermost group. `0` folds only the outermost group, which drops the inner groups carrying the model and view rows. | |
| `RPCS3_REMIX_PASSSKIP` | `1` (on) | Skip the extra passes of a multi-pass forward renderer. `0` submits every pass, which on a path tracer stacks the lighting pass instead of blending it. | |
| `RPCS3_REMIX_ORTHO2D` | `1` (on) | Honour `vp_fingerprint::has_ortho2d` and apply the 2-row transform of a 2D program in the compositor. `0` drops that transform and composites the raw attribute values as if already projected. | |
| `RPCS3_REMIX_WBUFFERZ` | `1` (on) | Recover the matrix row for a program that writes `HPOS.z` as its clip z premultiplied by its clip w. `0` refuses such programs instead. | |
| `RPCS3_REMIX_LOOSESLICE` | off | Let the backward slice from HPOS collect writers that sit after the instruction whose operand is being traced. RSX vertex programs are straight-line code, so this is the bisect knob for the ordering constraint. | |
| `RPCS3_REMIX_BASISAFFINE` | `1` (on) | Apply the object placement `match_basis_affine` recognises. `0` restores the behaviour where indexed-palette characters reached the outer group with no placement and landed at the camera group's origin. | |
| `RPCS3_REMIX_INDEXEDWORLD` | `1` (on) | Offer the register indirection to programs that read a constant palette through the address register, let `match_indexed_affine` express a 3-row indexed group with a post-matrix translation, and let `match_mad_chain` accept a partial writemask or non-unit stride for an indexed group. `0` restores all three refusals at once. | |
| `RPCS3_REMIX_INDEXEDBIASREG` | off | Let `match_indexed_affine` express an indexed group whose rows are written into one register and moved into another by the post-matrix translation, instead of requiring the translation to be in place. | |
| `RPCS3_REMIX_INDEXEDUNIFORM` | `0` (off) | Submit a draw whose indexed constant reads are provably the same constant for every vertex. `0` off; `1` release only programs whose matrix chain also reaches the vertex attribute; `2` release every program with a uniform address register. | |
| `RPCS3_REMIX_DRAWINDEXED` | off | Submit draws whose position chain really does read a constant through an address register instead of refusing them. Diagnostic -- the refusal exists because such a draw renders in its bind pose or torn across the map. | |
| `RPCS3_REMIX_CULL` | off | Submit `doubleSided=0` for draws whose RSX cull state is enabled, instead of forcing every instance double-sided. Off by default because a wrong winding in the strip/fan/quad expansion turns a single-sided surface invisible. | yes |
| `RPCS3_REMIX_SMOOTHNORMALS` | off | Tag every submitted world instance `SMOOTH_NORMALS`, which has Remix recompute the normal buffer on the GPU as an area-weighted average over the mesh's own triangles (`RtxGeometryUtils::dispatchSmoothNormals`, on BLAS build and update only). This backend never recovers the game's normals -- it submits a constant `(0,0,1)` on every vertex -- so with this off the whole scene is lit off one direction. Cheap here because that placeholder buffer already exists: `forceNormals` stays false, the interleaved fast path survives, and the compute pass overwrites in place. Read live. Counter: `cat_smoothnormals`. | yes |
| `RPCS3_REMIX_MESHCAP` | `0` (uncapped) | Cap the live mesh-handle cache at N entries with LRU eviction. `0` leaves it unbounded, which is what the idle-frame rule alone gives. | yes |
| `RPCS3_REMIX_MESHIDLE` | `300` | How many frames a mesh handle survives after the last draw that referenced it before `reap_idle_meshes` destroys it. Raising it trades residency against repeated BLAS builds. `0` reaps a mesh the first frame it goes unreferenced. | yes |
| `RPCS3_REMIX_VTXSPREAD` | `64` | How many times the median a draw's furthest decoded vertex may sit from the draw's own median position before `audit_vertex_extent` reports it. `0` disables the pass. Diagnostic only. | |
| `RPCS3_REMIX_VTXREFUSE` | off | Drop a draw that `audit_vertex_extent` called geometrically incoherent instead of submitting it. An A/B, not a fix. | |
| `RPCS3_REMIX_STREAKGATE` | `128` | How many times the frame's own median world extent a draw's post-transform bounding box may span before `audit_world_extent` refuses it. Measured against the frame's median, so it needs to know nothing about the title's units. `0` restores the behaviour where nothing measured the geometry Remix actually receives. | |
| `RPCS3_REMIX_AFFINETOL` | `20` (= 0.02) | **Undocumented until round 6.** Integer *milli*-units: the tolerance `is_affine()` applies to the perspective residue of `world = fused x reference^-1` at the end of `per_draw_transform`. A draw over it is refused (`fail=tail`) rather than submitted, because `to_remix_transform` drops the perspective row outright and a draw admitted on a loose tolerance is silently *flattened* into the wrong place -- drifting with camera pitch rather than snapping. **Reading discipline:** judge it from the `Remix affine-residue:` histogram, not by sweeping. That histogram bucket-counts refusals at `<0.05 <0.2 <1 <10 >=10`; if the first two buckets are empty, no achievable tolerance admits anything honestly and the refusals are a *reference* problem, not a tolerance problem. On Haze they are empty (`<0.05=0 <0.2=0 <1=22031 <10=109636 >=10=1`), which is what `TAILRESCUE` exists for. | |
| `RPCS3_REMIX_TAILRESCUE` | `1` (on) | **The missing-geometry fix (round 6).** When the affinity gate above is about to refuse a draw that was *not* divided by its own render source's current-frame anchor, retry the division against a same-**pass-shape** anchor (colour target + clip rectangle, any surface offset) and re-run the same gate. Two retries: this frame's anchor, then the freshest one aged <= `TAILRESCUEAGE`. Each recomputes `world = f64(fused x anchor^-1)`, re-prepends the object-space composite through the live lambda, re-normalises w, and faces the **same** affinity test -- a rescued draw is never exempt, so a wrong gauge keeps its perspective residue and still refuses. Worst case is today's behaviour (missing geometry), never smeared geometry. Why it works: `fail=tail` is 57% of world refusals (168,782 of 294,676), three different main-clip world programs refuse with the *bit-identical* residue 3.40683 -- a residue shared across unrelated programs is a property of the reference, not the programs -- and `Remix gauge-keys:` shows a usable main-pass anchor in essentially every window that nothing ever retried them against. Synchronous: no buffering, no submit-order change, no interaction with `DEFERPREANCHOR`. Requires `GAUGEANCHOR=1`. Counters: `tail_rescued_cur`, `tail_rescued_aged`, `tail_rescue_failed`; census `Remix tail-rescue:` (residue before -> after, which retry, anchor age). `0` restores drop-at-refusal bit-exactly. | |
| `RPCS3_REMIX_TAILRESCUEAGE` | `30` | How stale the second `TAILRESCUE` retry's same-shape anchor may be, in frames. The `GAUGECAMHOLD` precedent: long enough to cover a cut or a burst frame in which both anchor slots went stale, short enough that the pose is still the same shot. The first retry is this-frame only, and the second starts at age 1, so it can never re-test the first's pick. | |
| `RPCS3_REMIX_MADACCUM` | `1` (on) | **The giant-flower fix (round 10)** -- three coupled relaxations of `match_const_affine`'s backward walk, shipped as one knob because the sweep proves each is individually inert. **(M)** skip an accumulate-in-place decoration `MAD dst.xyz = a*b + dst.xyz` (neither factor an attribute), tested **only where the arm has already refused**; the unguarded ordering would also swallow `ADD dst.xyz = dst.xyz + c[K]`, a real constant bias the ADD arm folds correctly -- 24 such sites exist in this title's programs and it fires on 12 of them. **(A)** accept a scale operand forwarded through a `MOV` of a constant, resolved with `last_component_writer` (the definition live at that point, not merely a writer of the register) plus a `from_sca` refusal, broadcast reads only. **(B)** where the walk would refuse `partial-xyz`, merge a per-lane assembly when all three lanes reach the **same** full-xyz writer each on its **own** lane -- a permutation is refused, never read as the identity -- repositioning the cursor only, so the ordinary arms still judge what it landed on. `C97CD1531AC480D8` needs all three: `MAD r0.xyz = r1.xyzx, r1.wwww, r0.xyzx` @52 (a distance-scaled billboard offset) blocks the walk, beneath it `r0` is assembled per lane from `r4`, and `r4.xyz` @7 is `MAD r4.xyz = I0.xyzx, r0.xxxx, c[61].xyzx` whose scale `r0.xxxx` is `c[467].y` via `MOV r0.xz = c[467].yyzy` @4. Rebuilt decode: **`pos = ATTR0.xyz * c[467].y + c[61].xyz`**, `bias_before_scale` false (the walk meets a MAD -- scale first). **Sweep over 64 cached + 17 captured programs:** M / A / B / M+A each flip **0**; **M+A+B flips exactly 1** (the flower) with **0 regressions** and 80 still refused; universal probe over 29,049 entry points gives 78 gains / **0 regressions**, all resolving to that one program. **Deliberately NOT shipped:** generalising M's skip to `MAD dst.xyz = a*b + X` terminating on `X == ATTR0.xyz` flips 5 more programs with 0 *matcher* regressions -- and would **delete their geometry**, because those decodes carry neither scale nor bias, `build_prescale` ends `return has_scale \|\| has_bias`, and `per_draw_transform` turns that false into `++pos_decode_refused; return false`. A sweep that stops at the matcher would have shipped it. Replay is **static** (billboard offset and sway dropped -- the same contract round 9 shipped for the DF46 canopy). Counters `madaccum_resolved` / `madaccum_draws`; census `Remix madaccum:` prints the rebuilt scale/bias slots, because a wrong scale and a refused decode look identical from outside. `0` restores all three refusals bit-exactly. | |

## Skinning

| name | default | effect | in GUI |
| --- | --- | --- | --- |
| `RPCS3_REMIX_NOSKIN` | off | Skip every skinned draw instead of submitting bone transforms. | |
| `RPCS3_REMIX_SKINID` | off | Submit identity bone transforms with mesh, indices and weights unchanged. Geometry rigid and correctly placed means the matrices are at fault. | |
| `RPCS3_REMIX_SKINBONE` | unset | Clamp every dense bone index to n. Mesh rigid means the palette read is fine and the index decode is at fault. Unset is a distinct sentinel so `0` stays a usable value. | |
| `RPCS3_REMIX_SKINRAW` | off | Feed the raw (un-scaled) attribute value to `evaluate_bone_offset` instead of the scaled one. | |
| `RPCS3_REMIX_BONEBLEND` | `1` (on) | Let `match_blend_palette` express summed accumulators, let `resolve_bone_index` follow the x3 index scaling blend programs build by repeated self-addition, and let the indexing audit accept the four matched address components. `0` refuses every program that blends more than one palette entry per vertex, restoring all three at once. | |
| `RPCS3_REMIX_BONESCALE` | `1` (on) | Refuse a blended draw when one of its bones is orders of magnitude away from the median of the others in the same draw. `0` submits it anyway; the gate exists because a 3-row palette otherwise reaches Remix with no magnitude check at all. | |
| `RPCS3_REMIX_BONEUNIFORM` | `1` (submit) | Submit a blended draw whose palette entries are all the same matrix -- such a palette blends to exactly that matrix and the draw is a correct rigid draw. `0` refuses that population, as a one-run A/B. | |
| `RPCS3_REMIX_SKINREACH` | `32` | How many times the median a skinned draw's furthest vertex-to-its-own-bone distance may be before `audit_skin_extent` reports it. `0` disables the pass. Diagnostic only -- nothing is ever refused on it. | |

## Materials and textures

| name | default | effect | in GUI |
| --- | --- | --- | --- |
| `RPCS3_REMIX_NOTEX` | off | Disable the whole texture workstream. | yes |
| `RPCS3_REMIX_NOTEXMAT` | `1` (on) | **The untextured-population mitigation (round 6).** Attach a shared, lazily-created mid-grey material -- one 2x2 CPU texture through the synthetic `0x<hash>` path, albedo **0.5 linear** (byte 128 under UNORM, 188 under sRGB) -- to every draw that reaches submission with no albedo material, instead of submitting material-less. A material-less surface renders **white**. **CORRECTED, round 42:** the "41% of Haze's submitted draws are in that state (`tex_none=2,008,830` vs `tex_bound=2,879,211`)" claim this row used to carry is WRONG and was inherited for six rounds — that ratio is the albedo *walk's* failure rate, and 97.6% of `tex_none` is refused by a skip gate before submission. MEASURED on a 41,802-flip run: `notex_mat_applied=2,674` of `submitted=5,392,256`, i.e. **0.0496%**, and `tex_none` partitions exactly into skip + world + this. The knob still does what it says; it is just not covering a third of the screen. See "Round 42" TASK 0. One defect, three looks, and the worst thing on screen. Grey is the honest constant for an unknown colour -- it invents no texture. Only `surface.material` is redirected: the local `material` stays null through every heuristic (A2C/KIL replays, blend extension, sky and skip rules), so this changes exactly one thing. **Caveat:** an untextured draw that resolved the title's own ATTR3 colour (`vcol_applied`; Haze's sky dome) has that colour modulated by the material, so it comes out about half as bright -- if a vertex-coloured surface looks too dark, this knob is the sever. Counter: `notex_mat_applied`. `0` restores the material-less submit bit-exactly. | |
| `RPCS3_REMIX_SKIPSHADOWONLY` | `1` (on) | **`SKIPAUXUNTEX`'s successor, with the term that actually discriminates.** Refuse as world geometry any draw that resolved no material, whose **every** eligible texture unit carries a `CELL_GCM_TEXTURE_DEPTH*` format, and whose render clip is *strictly smaller* than the main pass's. The measured shape: `Remix notex:` on disk reads `class=retry-refused reason=format unit=0 fmt=0xb2 dims=2048x2048 clip=512x288` on the half-res lighting passes (`33ae0895...`, `a41a18e1...`, `56cc5a96...`, `6004dce6...`) -- a 2048x2048 DEPTH16 shadow map as the only eligible unit. A draw whose entire sampler set is depth buffers has no albedo anywhere; it is a shadow projection or a lighting composite, and submitting it to a path tracer as a white surface *is* the flat-white wash. Its predecessor keyed on the absence of any eligible unit, which could never be true of that population. Main-clip shadow-only draws are **censused and left alone** this round. Counter: `skip_shadowonly`; census `Remix shadowonly:` names every (vp, fp) it touches with the sampled mask and both clips, so a widening cannot happen quietly. `0` restores submission. | |
| `RPCS3_REMIX_MAINCLIPMAX` | `1` (on) | **Why `SKIPSHADOWONLY` reached only 4.7% of what it classifies (round 7).** Every "strictly smaller than the main pass" rule compares against `m_main_clip_area`, which is the **previous frame's** largest *textured* clip. On Haze that value oscillates -- `1024x576` on frames whose main world pass drew something textured, `512x288` on the many frames it did not -- and the half-res shadow/lighting passes are themselves `512x288`, so the test was `147456 < 147456`, false, on most frames. Measured over a 13,400-frame session: `tex_shadowonly=437,810` against `skip_shadowonly=20,656`; the 4.7% that got through are exactly the frames where the previous one happened to contain a textured `1024x576` draw. With this on, the comparison runs against the **largest textured clip seen this session** -- a property of the title rather than of the previous frame. The reference can only ever grow, so this can only widen such a rule, never narrow it. Both values are echoed as `mainclip=` and `maxclip=` on `Remix live:`; the reject partition is `so_nomain` / `so_notsmaller`. `0` restores the per-frame value exactly. | |
| `RPCS3_REMIX_WALKSAMPLED` | `1` (on) | When the albedo walk is about to refuse to substitute a higher unit (the `tex_retry_refused` exit), step past the refused unit anyway -- but **only** to a unit named in the fragment program's own `sampled_mask` whose bound texture carries a **colour** format. That sampled-mask term is exactly what the old `RETRYUNSUP` widening lacked when it stepped onto units nothing sampled and painted character faces onto tree trunks; a unit the program provably samples, and which is not a depth buffer, is a colour source the program itself named. Counter: `tex_walk_sampled`; the population it draws from is `tex_colorunit`. `0` restores the refusal exactly. | |
| `RPCS3_REMIX_NOUV` | off | Submit world geometry with texcoord `(0,0)` on every vertex, so a bound albedo material produces one flat colour per draw. The bisect knob for the UV fetch. | |
| `RPCS3_REMIX_UVATTR` | `0` (automatic) | Force the vertex attribute the albedo texcoords are read from, instead of the 8+unit convention and its fallback scan. Accepts 1..15 when forced; `0` stays refused because that is the position attribute. | |
| `RPCS3_REMIX_UVUCODE` | `1` (on) | Read the texcoord attribute out of the vertex program (`vp_fingerprint::texcoord_input`). `0` ignores it and picks with the size/type heuristic alone. | |
| `RPCS3_REMIX_UVWIDE` | `1` (on) | Widen the automatic texcoord scan past attributes 8..15 to referenced attributes 1, 2, 4, 5, 6, 7 (never 0/position, never 3/colour, never the skinning attribute), taking only 2-component streams that decode finite. `0` restricts the scan to 8..15. | |
| `RPCS3_REMIX_CLAMPALBEDO` | empty | **The UI seam lever for the route no per-sample rule can reach (round 7).** Comma list (bound 16) of albedo **content hashes** whose wrap mode is forced to CLAMP at texture-entry creation, overriding the guest's own RSX wrap state for those textures only. Haze draws UI through **two** routes -- the CPU compositor *and* camera-anchored world geometry sampled on the GPU through a Remix material -- and the same program appears in both. `UICLAMPSUBRECT` cannot touch the second. Because both the compositor's `address_coordinate` and `material.wrapModeU/V` read the same `entry.wrap_u/wrap_v`, one list entry closes **both** routes for that texture. Non-default wrap already folds into `material_hash`, so the clamped variant gets its own material and cannot alias the repeat one. Fill it from a `Remix uiwrap:` line (2D), a `Remix uvrange: ... rsxwrap=1/1 u=[-...` line (3D) or a Ctrl+Click on the garbled element; relaunch only, no rebuild. `6575ACE3A42A78E6` is the pre-identified first candidate. Risk, by design: a texture shared with world geometry gets clamp there too (edge streaks) -- remove the hash to restore. Counter: `tex_wrap_forced`. Ships **empty**: zero behaviour change until a hash is listed. **ROUND 25 -- MEASURED DEAD ON HAZE'S FONT ATLAS, DO NOT RE-ARM IT THERE.** `Remix uvrange: vp=6f76ab0ad8d926b1 ... albedo=6575ACE3A42A78E6 rsxwrap=3/3 dims=512x512`: RSX wrap 3 is CLAMP_TO_EDGE, so the **guest already binds clamp on both axes** and the decoder already maps that to `wrap_u/wrap_v = 0` -- which is why all 146 `Remix uiwrap:` rows for that atlas read `wrap=0/0`. This knob can only *write* 0/0, so listing that hash wrote what was already there: same entry state, same material hash, same sampler on both routes, zero pixels changed. And `tex_wrap_forced=1` was never evidence to the contrary -- it counts **listed-and-created**, once per texture upload, not per draw. The atlas's real defect is a UV rectangle 2x too large; see `RPCS3_REMIX_UIRECTSHRINK`. | |
| `RPCS3_REMIX_UVFLIPV` | off | Submit `1-v` instead of `v` for every texcoord, on both the 3D and the 2D path. A per-title knob for a per-title defect: `v = 0` is row 0 of the decoded texture on RSX, so a global flip would invert every title whose coordinates are already right. | yes |
| `RPCS3_REMIX_UVINTSCALE` | `4096` | Divisor for S32K (raw 16-bit integer) texcoords, whose real divisor is a vertex-program constant this backend does not read. `0` submits them raw. | |
| `RPCS3_REMIX_UVAFFINEVP` | empty | Replay one named program's ATTR8-to-TEX3 affine UV slice with every constant slot hardcoded (Haze's visor alpha program). Per-hash and unit-0 only; kept as a forced override. | |
| `RPCS3_REMIX_UVAFFINEALL` | `1` (on) | Replay the same 2x2-plus-two-biases form for every program and unit where the walk resolves it out of that program's own ucode -- attribute, scale slot **and component**, row slots, bias slots. This is what carries the constant scale through instead of falling back to `UVINTSCALE`: 267,355 draws took the fixed 4096 divisor in the last Haze run while the ucode named 1/32768, which is the 8x tiling. `0` restores per-hash-only. Counter: `uv_affine_general`; every refused program is named with its reason by the `Remix uvscale-fixed:` census. | |
| `RPCS3_REMIX_UVSCALELANES` | `1` (on) | Let the resolved affine form carry one divisor **component per lane** (or per 2x2 row) instead of one scalar for both. Read out of Haze's ucode, not inferred: `c151` is a *vector* of per-attribute divisors -- `f7f12d5d15bb9c37` multiplies ATTR8 by `c151.x`, ATTR9 by `c151.y` and ATTR10 by `c151.z` in one TEX0 slice, and `24d1ba819f701e47`'s live `c151` reads `[0.00024426 3.05176e-05 0.00024426 3.05176e-05]`, two divisors 8x apart inside one uploaded vector. A program may therefore divide u and v differently in a single instruction, and `bab9af462da74331` does (`MUL o7.xy = ATTR8.xyxx * c68.xyxx`); the single-scalar form refused it (`affine:mul-two-components`) and the draw fell back to the fixed 1/4096 on **both** lanes. Provably inert on every program that already resolves -- those name the same component twice, so both readings compute the same number -- so it can only change draws that are refused today. Counter: `uv_affine_lanes` (0 means no program in the scene divides u and v differently and the knob changed nothing). `0` restores the refusal. | |
| `RPCS3_REMIX_UVRANGECENSUS` | `1` (on) | Name every (program, texcoord output, albedo) whose **submitted** UVs leave the unit square by more than a texel's slack: one `Remix uvrange:` line, capped at 256, carrying the u/v range, the repeat count, the resolved form, and the live value of the scale slot, both 2x2 rows and both biases. Pure logging. The pick line answers this for a clicked draw, and 116 clicks in the round-2 Haze run produced exactly one tiling draw -- not enough to separate "this mesh genuinely tiles 4x in v" from "the replay is out on a population the cursor never landed on". Printing the divisor beside the range settles it without a rebuild: a divisor the ucode names and the register file holds is the game's own. Counter: `uv_tiled`. `0` removes both. | |
| `RPCS3_REMIX_DEFERPREANCHOR` | `1` (on) | **Pre-anchor deferral.** A draw whose render source has no *current-frame* gauge anchor is otherwise placed by last frame's (`gauge_prev`, 25% of divides) or by the cross-pass camera (`gauge_absent`, 20%) -- one frame of camera motion stale, which is the residual wobble round 4 attributed: the ceiling fixture `vp=c2003391127734f6` picked `ref=anchor_prev` 9/9 with its origin drifting ~0.5 units between frames while its basis stayed within 3e-5 of 1 (the arithmetic was already clean; the *pose* was late). Such an instance is now buffered by value -- info, blend/picking extension structs, the fused matrix, the object-space prepend and the source key -- and submitted the moment `capture_gauge_anchor` lands that source's anchor, re-divided in f64 against it. The object-space matrix is captured by running the live prepend lambda on an identity, so there is no second copy of that composition to drift. Anything still buffered at flip is flushed with the placement it already carried, so no draw is ever lost. Skinned draws keep the immediate path (their bone extension points at per-draw scratch). Counters: `defer_buffered`, `defer_fresh`, `defer_flip`, `defer_spilled`, `defer_skinned` -- **`fresh` >> `flip` is the success reading**; `flip` dominant says the anchor arrives too late in the title's frame and full flip-deferral is the next step. `0` restores the immediate submit bit-exactly. | |
| `RPCS3_REMIX_DEFERPREANCHORMAX` | `4096` | Hard cap on the per-frame deferral buffer. A frame past it spills the remainder through the immediate path and counts `defer_spilled`. | |
| `RPCS3_REMIX_NECTARMODE` | `1` | `1` publishes the fork game value `haze.nectar_disruption="1"` on every frame the `(A41A18E14C782613, E5D8F51451B96165)` pass is submitted -- the previous behaviour. `0` never publishes, which kills the fork-side greyscale outright. That pair turns out to be one of the generic half-resolution blended shadow-sampling passes (`Remix notex:` has it at `sampled=0x3 blend=1 depth_write=0`), i.e. drawn whenever its volume is on screen -- so mode 1 fires whenever the player merely *looks toward* the room rather than when the effect is active. The pass is still detected in either mode; only the publish is gated. Mode `2` (publish on a discriminator) is reserved until `LIGHTPASSCENSUS` names one. Counter: `nectar_published`. | |
| `RPCS3_REMIX_LIGHTPASSCENSUS` | `1` (on) | One `Remix lightpass:` line per (vp, fp) per stats window, capped at 64, for draws on a *strictly smaller than main* clip with blend on and depth-write off -- the population `Remix notex:` names on Haze (`a41a18e14c782613/e5d8f51451b96165`, `504d2b3a.../9e16b7f2...`, `8ded5c7d.../8ab4259c...`, `33ae0895.../1e27bc07...`, all sampling the 2048x2048 DEPTH16 shadow map, i.e. the shape of a per-light shadow/lighting composite). Dumps vertex count, sampled mask, live blend function/equation/factors and blend colour, and the fragment program's first eight inline literal vec4s. Pure measurement, zero behaviour change: RSX has no fixed-function light registers, so the only place a title's light parameters exist in a form the RSX consumes is the constants of the passes that apply them -- and nothing has ever dumped Haze's fragment literals. If per-light positions or colours are in there, real light interception becomes designable on data; if not, the option is dead and the fixture-albedo channel is the whole answer. `0` removes the lines. | |
| `RPCS3_REMIX_NOTEXCENSUS` | `1` (on) | Name every (vertex program, fragment program) submitted with **no albedo material at all** -- the `tex_none` population, which on Haze is 7.8 M against 5.2 M textured. One `Remix notex:` line, capped at 128, carrying the surface address, clip size, colour target, sampled mask, depth-write and blend. These draws never reach `apply_texcoords`, so their pick line reads `uv_attr=-1 uv_scale=notex(0)` -- "never asked", not "the UV walk failed", which were indistinguishable before this. `6004dce66b8b7a11` is the case that motivated it: same vertex count, same origin and same frame as the textured soldier draw beside it, but `material=0`, `albedo_unit=-1` and `clip=512x288` rather than `1024x576`. Pure logging; nothing is refused on the strength of it. `0` removes the lines. | |
| `RPCS3_REMIX_SKIPAUXUNTEX` | **`0` (RETIRED, round 5)** | **Verdict:** round 4 fired this rule 99,695 times on exactly its named population (`Remix aux-untex:` names `6004dce...` and `504d2b3a...`) and the greyscale wash was still reported; a subsequent run with the gate off left the doors and walls just as invisible. No demonstrated benefit, no demonstrated harm -- and the wash now has a better-supported explanation (the nectar game-value greyscale, `NECTARMODE`). A rule that refuses ~100k draws per run for no measured gain is a standing risk of eating legitimate material-less geometry, so the default flipped `1` -> `0`. The knob and its census stay for archaeology; `1` restores the round-3/4 behaviour exactly. Original description follows. Refuse as world geometry any draw that resolved no material **and** named no albedo unit, whose render source is *strictly smaller* than the main pass's. Haze renders a second, untextured pass over the same meshes into a half-resolution auxiliary surface: six of the user's own picks name `6004dce66b8b7a11` at `material=0 albedo_unit=-1 sampled=0x1 clip=512x288 depth_write=1 blend=0`, at in-room world positions, while the textured soldier draw beside it has the same 342 vertices, the same origin and the same frame at `clip=1024x576`. Submitted as world geometry it is a cloud of flat grey meshes filling the room -- the greyscale wash. *Strictly smaller*, not merely different, because Haze's shadow map is a material-less 2048x2048 pass that must keep its current behaviour. The main clip is the largest clip a **textured** draw used in the previous frame, so a material-less depth pass can never define it, and the sky dome (material-less on the main clip) is unaffected. The four hand-listed exact skips stay ahead of this rule so their counters keep their lineage. Counter: `skip_auxuntex`; census: `Remix aux-untex:`, one line per (vp, fp) it touches with both clips. `0` restores submission. | |
| `RPCS3_REMIX_FPKIL` | `1` (on) | Replay the fragment program's **per-pixel discard** as a per-draw alpha test. A draw is a cutout when its ucode carries a *conditional* `KIL` (opcode 0x12, which rpcs3's own decompiler turns into `discard` under the instruction's execution mask, `FragmentProgramDecompiler.cpp:1486-1489`) or when its bound albedo unit has `NV4097_SET_TEXTURE_CONTROL0` bit 2 set (`fragment_texture::alpha_kill_enabled`, consumed by core at `RSXThread.cpp:2267-2271`). Such a draw is submitted `alphaTestType = 4` (GREATER) with either the threshold constant recovered from the ucode or `FPKILREF`. Why this exists: 3559 of 3572 materials on Haze are created with the RSX alpha test **disabled**, so no material and no blend state can be carrying a cutout, and a grep of `Remix/` found zero references to KIL, `ROP_discard` or `alpha_kill` -- the recurring bug shape, an instruction in the ucode the backend never replays. Applied per **instance**, not per material: materials are content-hash-keyed and shared, so the same albedo can be drawn by a KIL program and a non-KIL program in one frame. An *unconditional* KIL is deliberately not a cutout (it kills every pixel). Counters: `kil_ucode`, `kil_ctrl`, `kil_disagree` (detection, ucode vs the guest-set shader-control bit -- the ucode is ground truth), `texkill_seen`, `kil_applied`, `kil_applied_texctl`. Census: `Remix kil:`, one line per (fp, albedo) cross-tabulating the channel, the blend state and the texture's real alpha range -- so "the title does not cut out at all" is a reportable verdict and not a silent no-op. Requires `BLENDSTATE=1`. `0` restores today's always-pass exactly. | |
| `RPCS3_REMIX_FPA2C` | `1` (on) | Replay **alpha to coverage** as an instance alpha test (GREATER, `FPKILREF`) on material-bound draws with no alpha test of their own. **Round 6 changed the semantics; the round-5 shape is now a documented mistake.** `1` requires BOTH bits -- the shader-control bit AND `NV4097_SET_ANTI_ALIASING_CONTROL`'s alpha-to-coverage flag (`rsx_methods.h:1263-1270`). `2` is round 5's ctrl-only OR, kept only so the regression can be reproduced. `0` is off. Why: A2C is a ROP state gated on the *register*. `GLDraw.cpp:214-218` enables `GL_SAMPLE_ALPHA_TO_COVERAGE` from `msaa_alpha_to_coverage_enabled()`, and core's own *shader* emulation of it is gated the same way -- `RSXThread.cpp:2170` masks the guest's shader control word down to `32_BITS_EXPORTS / DEPTH_EXPORT / USES_KIL` and then **synthesises** `RSX_SHADER_CONTROL_ALPHA_TO_COVERAGE` itself at `:2191-2198` from the register. Core never reads an A2C bit out of the guest's word, and it cannot: that flag is `0x01000000`, which lies inside `RSX_SHADER_CONTROL_USED_TEMP_REGS_MASK` (`0xff000000`). Read straight off the guest's shader control word -- which is what round 5 did -- the bit is the low bit of the fragment program's **used-temp-register count**. That is the whole of Haze's `a2c_ctrl=866,637` (it means "uses an odd number of temporaries", about half of every draw) and of the 658,012 spurious alpha tests behind the see-through surfaces. Haze's `a2c_reg` is 0 for a whole run, so with the AND the replay is inert: **the honest verdict on the title is negative -- it does not use alpha to coverage.** There is no threshold to look for either; coverage is proportional to alpha, so a title that really does set the register needs the blend-to-cutout category, not a scalar test. Counters: `a2c_ctrl`, `a2c_reg` (detection, counted in every mode), `a2c_applied`. | |
| `RPCS3_REMIX_FPKILREF` | `128` | Alpha reference (0-255) used when the ucode walk could not recover the program's own threshold constant -- i.e. when the `Remix kil:` line reads `ref=-1`. Recovery only trusts the exact classic cutout shape (an `ADD`/`MAD` with `set_cond` comparing a TEX result against an inline literal in the addend slot); every other shape falls back here rather than replaying a constant that might be a scale. Sweep 64 / 192 if cutouts come out inverted or too aggressive. | |
| `RPCS3_REMIX_FPALBEDO` | `1` (on) | Pick the albedo unit from `fp_fingerprint::colour_mask`. `0` picks the lowest referenced, enabled 2D unit with a retry loop free to walk to any other referenced unit -- which is what puts a normal map in the albedo slot. | |
| `RPCS3_REMIX_RETRYUNSUP` | `0` (off) | Let the albedo unit walk step past a unit `bind()` refused for a permanent format reason instead of stopping on `FPALBEDO`'s guard. Haze puts a 2048x2048 DEPTH16 shadow map on the lowest referenced unit of every shadow-receiving draw and names no albedo unit in its ucode, so that guard fired unconditionally and 55,360 draws reached Remix untextured. Shipped on, then measured off: the walk raised `tex_unit_substituted` from 4 to 3445 and painted character face textures onto tree trunks, while `tex_none` barely moved because ~99% of the walks find no material on any higher unit either. A missing texture is better than a wrong one. `1` restores the walk. Counter: `tex_retry_unsupported`. | |
| `RPCS3_REMIX_NOALPHA` | off | Create materials with a fixed always-pass alpha state (`alphaTestType 7`, `useDrawCallAlphaState 1`) instead of the title's own alpha test. The bisect knob for cutout foliage. | yes |
| `RPCS3_REMIX_NOVCOL` | off | Leave every submitted vertex colour at `0xFFFFFFFF` instead of reading ATTR3 for draws that resolved no material. The bisect knob for vertex-coloured geometry. | yes |
| `RPCS3_REMIX_BLENDSTATE` | `1` (on) | Set `useDrawCallAlphaState = 1` on the material and carry the per-draw blend and alpha-test state on the instance (`remixapi_InstanceInfoBlendEXT`). `0` restores every material declaring its own alpha state with no blend chained onto the instance, so every draw reached the runtime fully opaque. It is one switch, not two, because the runtime reads `useLegacyAlphaState` once for both halves. | |
| `RPCS3_REMIX_TEXBUDGET` | `8` | Cap on `CreateTexture` calls per frame. Anything over budget submits with a null material for that frame and renders untextured until a later frame lets it through. `0` is unlimited. Read live. | yes |
| `RPCS3_REMIX_TEXIDLE` | `300` | How long a decoded texture and its Remix handles survive after the last draw that bound them, in frames. Raising it trades VRAM against re-decoding and re-creating. The config floor is 30, but the environment variable can express `0`. Read live. | yes |
| `RPCS3_REMIX_TEXLINEAR` | off | Upload textures as UNORM instead of SRGB. | |
| `RPCS3_REMIX_TEXREHASH` | `0` | Texture staleness policy: `0` descriptor only, `1` sampled fingerprint, `2` full hash every draw. Above 0 costs heavy mesh churn, because the albedo hash is folded into the mesh key. Animated textures go stale at 0; 1 or 2 buys them back. | yes |
| `RPCS3_REMIX_TEXVERIFY` | `120` frames | **The staleness census (round 8) -- measurement only, nothing is rebuilt.** How often a live cache entry re-hashes its guest bytes (full FNV, not the strided sample) to find out whether the image under a stable descriptor key has been replaced. This is the "stale pool slot" arm of the wrong-texture bug -- *a Mantel soldier rendered entirely in tree bark*: the cache key hashes offset/format/dims/wrap/alpha and **never content**, there is no write-tracking, so a streaming pool that recycles an address keeps the entry serving the previous image's pixels *and* the previous image's Remix material forever. Counter `tex_stale_detected` on `Remix live:`; census `Remix texstale: key=... albedo=... fmt=... dims=... age=...`. Non-zero with a wrong texture on screen confirms the stale arm; **zero** across a session where one was seen moves the thread to the content-alias arm, which `Remix texdup:` names. **Why it does not fix it:** the honest fix is an in-place rebuild with `content_hash` **pinned** (the shipped `refresh_pixels` idiom) so the mesh key never moves and the measured 32x mesh-churn trap is avoided -- but a pinned key is exactly what makes destroying and re-creating the Remix texture + material unsafe here, because the mesh cache **bakes the material handle into the mesh at `CreateMesh` time and a reused mesh keeps it**, so meshes whose key did not move would submit a destroyed material. A safe fix needs a way to re-point a live material; that is round 9's. Cost: one hash per live entry per 120 frames, amortised -- not per bind. `0` disables the check entirely (the bisect step if frame time regresses). | |
| `RPCS3_REMIX_TEXSTALEEVICT` | `0` (off) | **Round 17 -- the fix for the wrong-texture family ("my helmet turned into tree bark").** Acts on the detector above: when the cadence verify finds the guest bytes under a stable descriptor key have changed, **erase** the entry so the next bind decodes the image that is actually there. Root cause: `texture_descriptor::key()` identifies a texture by **where** it is (offset, location, format, pitch, dims, wrap, alpha state) while `content_hash` identifies it by **what** it is; Haze streams into a recycled address pool, so bark decodes at address X, the helmet is later written over X, the helmet's bind produces the same key, **hits**, and is handed the material whose `albedoTexture` path is `0x<bark hash>`. Nothing ages it out -- `tex_destroyed` and `reap_freed` are both `0` over 18,269 flips -- so every swap is permanent for the session. Round 8 measured it (`tex_stale_detected=134` over 63+ keys) and deliberately did not act, on the assumption that the fix had to be an in-place rebuild with `content_hash` **pinned** -- and the pin is exactly what makes destroying the Remix material unsafe. Erasing needs no pin: the next bind derives the correct hash and re-keys its own meshes. The old texture+material pair is **orphaned rather than destroyed**, because the mesh cache bakes the material handle at `CreateMesh` while keying meshes on the content hash and ~45 live entries share one content hash on this title. Not the `TEXREHASH` trap -- that hangs off a strided `sample_fingerprint` that false-positives (604 rehashes of 682 textures, 32x mesh churn); this hangs off the full-hash cadence verify. Expected blast radius: 134 entries rebuilt in 18,269 flips = 1.2 % of `tex_created`, 0.003 % of `tex_hits`. Counters `tex_stale_evicted` and `tex_stale_orphaned`, which should be equal; a gap means a material was destroyed with a mesh handle still baked in. Inert unless `TEXVERIFY != 0`. `0` is bit-exact. | |
| `RPCS3_REMIX_TEXREAPSAFE` | `1` (on) | **The material-lifetime fix (round 9).** `texture_cache::reap` may no longer destroy the material of an entry that a **live mesh still has baked in**. The hole is proven by code inspection independently of any symptom: reap consulted *nothing* about meshes, the mesh cache bakes the material handle at `CreateMesh`, and a mesh key is **content-derived and therefore stable** -- so a reaped material leaves a dangling handle inside a mesh that will be *reused, not rebuilt*, for as long as its geometry is unchanged. No bind can heal that; there is no code path anywhere that re-points a live mesh's material. The result is a permanently white surface, and on handle reuse a **wrongly textured** one -- which is the first mechanism that explains the idle degradation and the bark-soldier / portrait-floor family with one defect. `mesh_entry` gains the baked handle (one member, one assignment); the render side builds the live-material set from `m_meshes` and hands it to reap. The mesh walk is gated on a new `texture_cache::has_idle()` because reap runs every flip while it only has *work* once per TEXIDLE window per entry (a kept entry is re-aged, a freed one erased), and a mesh created since the last build cannot be missed -- creating it required a successful `bind()` this frame, which stamps `last_used_frame`, so its entry cannot be idle. Counters `reap_kept` / `reap_freed` on `Remix live:`. VRAM cost is bounded by the materials of live meshes -- i.e. what is on screen, which has to stay resident anyway -- and those age out normally once MESHIDLE reaps the meshes themselves; the tell for overreach is `tex_live` trending up without plateau. `0` restores the pre-round-9 reap bit-exactly and is the control for the 30-second idle repro. | |

## Sky and viewmodel classification

| name | default | effect | in GUI |
| --- | --- | --- | --- |
| `RPCS3_REMIX_SKYEXTENT` | `2000` | Minimum world-unit span in the widest axis for a depth-write-off, camera-anchored draw to be tagged SKY. `0` disables the detection. | yes |
| `RPCS3_REMIX_SKYANCHOR` | `4` | How far the draw's own origin may sit from the camera and still be a sky candidate. `0` disables the anchor requirement and restores the extent-only rule. The distance is measured against the resolved camera, so a draw with no world transform is refused, not guessed. | |
| `RPCS3_REMIX_SKYBACKDROP` | `1` (measure only) | The separate backdrop rule -- world extent past `SKYEXTENT`, camera inside the draw's transformed bounding box, and a minimum extent-per-vertex. `0` off, `1` measure only (counts `sky_backdrop_hit` / `sky_backdrop_dw`, changes nothing on screen), `2` measure and tag SKY. | |
| `RPCS3_REMIX_SKYHASH` | `1` (measure only) | Identify the sky by albedo content hash rather than by geometry, using geometry only to learn which hashes belong to a dome. A hash is armed after enough consistently dome-shaped draws and is disqualified permanently by a single draw that is not. `0` off, `1` measure and census only, `2` measure and tag SKY. | |
| `RPCS3_REMIX_SKYTEXTURED` | `1` (on) | Allow a textured draw to be a sky candidate. `0` requires a sky candidate to be untextured, which was written against a vertex-coloured dome and does not generalise. | |
| `RPCS3_REMIX_SKYLEARN` | `1` (on) | Admit the narrower latitude bands of a dome whose vertex program has already produced a draw tagged SKY on geometry alone, so the cap and the horizon rings of a banded dome are classified with the body instead of being submitted as world geometry on their own smaller extent. `0` restores extent-only classification. Counter: `sky_learned_ring` -- `0` means the rule never admitted anything and is not the reason any geometry is missing. | |
| `RPCS3_REMIX_SKYCAM` | `1` (on) | Register a `REMIXAPI_CAMERA_TYPE_SKY` camera each frame carrying the same view and projection as the main one. A draw tagged SKY selects the sky camera on this fork, so without a valid one registered the tag alone can leave the sky mis-transformed. `0` submits no sky camera. Echoed as `skycam=` on the `Remix live:` knob tail. | |
| `RPCS3_REMIX_VIEWMODEL` | `2` (tag) | First-person arms/weapon detection by viewport depth range. `0` off, `1` measure and census only, `2` measure and tag `VIEW_MODEL`. | |
| `RPCS3_REMIX_VIEWMODELCAM` | `2` (transform) | Which reference `per_draw_transform` divides a viewmodel draw by. `0` uses the world camera's reference for every draw; `1` counts viewmodel draws and drops them; `2` uses a reference latched from the viewmodel population itself, refusing until one has been latched. Separate from `VIEWMODEL`, which only names the population. | |
| `RPCS3_REMIX_VIEWMODELVP` | empty | Comma-separated 16-hex vertex-program hashes treated as viewmodel geometry whatever their viewport depth range says. Haze reports `scale_z=0.49875 offset_z=0.50125` for every draw in the scene including the sky dome, so the depth rule selects nothing and there is no threshold to retune. Feeds the existing viewmodel machinery end to end: the viewmodel reference latch, the own-reference division under `VIEWMODELCAM`, and the `VIEW_MODEL` category at submit. Every tagged draw must additionally pass `VIEWMODELANCHOR`. Bounded at 8. Counters: `vm_tagged_hash`, `vm_hash_anchor_refused`. | |
| `RPCS3_REMIX_VIEWMODELANCHOR` | `4` | How far a `VIEWMODELVP`-tagged draw's instance translation may sit from the eye and still be treated as viewmodel geometry. The guard exists because a program hash provably does not separate viewmodel from world geometry -- the same program draws both. Only the hash tagger consults it; the depth-based path and its counters are untouched. | |
| `RPCS3_REMIX_VIEWMODELALBEDO` | empty | **The visor discriminator (round 10).** Comma-separated 16-hex albedo *content* hashes tagged as viewmodel geometry, beside the existing program list. The visor is `vp=830d7d1b9681c475` in two pieces ~1.2 units in front of the eye (`1BF8325ADEF3C986` opaque shell `vtx=1446 ext=3.23`; `C61753D31FB96507` blended layer `vtx=59 ext=7.46`) and **cannot be tagged program-wide: `830d7d1b9681c475` is also `GUESTLIGHTVP`**, so tagging the program drags the ceiling-light fixtures into the viewmodel camera and undoes round 5's lighting. The goal is *not* to shrink the visor -- in the raster original it is deliberately ~10 feet in front of the player and does not clip world geometry, because it is a HUD element shaped like a visor -- it is to classify it as a viewmodel so Remix gives it its own near plane and excludes it from world clipping. Tag rule: albedo matches **and** (`VIEWMODELVP` is empty **or** the program matches it too), so an armed albedo list works on its own while a user who sets both keeps the intersection. Bounded at 16, nothing pre-filled in code. Counter `vm_tagged_albedo` -- its own counter; `vm_tagged_hash` is **not** overloaded. **Do not add the HUD gauge albedos** (`099FCDACD6FDA024 A47E2136CC3785B3 EF2A8E2D0547AD1A D4EC78903E48A162 4B1F4BA19202CFCB D6FB4AC0556FE7E4 4D3334785D8A7E44`): tagging `2f64c2f8ffd6add1` as viewmodel made the gauges vanish. | |
| `RPCS3_REMIX_VMANCHORGEO` | `1` (on) | **The reason the viewmodel machinery has never worked since round 1 (round 10).** Measures the anchor guard against the draw's **geometry** (its local AABB centre transformed by the draw's own matrix -- the exact computation `maybe_inject_guest_light` already performs) instead of against its instance **origin**. Proven by experiment: with `VIEWMODELANCHOR=4` the hash matched 54,389 draws and the guard refused **all** of them (`vm_hash_anchor_refused=54389`, `vm_tagged_hash=0`); raising the limit to 100000 flipped it to `vm_tagged_hash=67901`, `vm_hash_anchor_refused=0`. The picks report those same draws ~1.5 units from the eye -- because picks measure the geometry -- while the draws carry identity-ish transforms (`origin ~ [0,0,0]`) with **world-space vertices** and the camera sits at `[1761,-23,1145]`, so the guard was measuring ~2100 units to a point no geometry occupies. With geometry measured, a pinned `VIEWMODELANCHOR=4` becomes **correct** for the visor (~1.2 < 4) while still excluding world geometry, which is what the guard's name always claimed. A tagged draw that reaches the guard before its vertices are decoded falls back to the origin measurement and is counted `vm_anchor_unmeasured` rather than guessed at -- that counter must stay 0. `0` restores the origin measurement bit-exactly. | |
| `RPCS3_REMIX_VMALBEDOCAM` | `1` (on) | **The camera that makes the `VIEW_MODEL` tag mean anything (round 10).** The category bit has been settable since round 5 and has never done anything, because the runtime also requires a `REMIXAPI_CAMERA_TYPE_VIEW_MODEL` camera and this backend has never submitted one: `createViewModelInstances` returns early on `!cameraManager.isCameraValid(CameraType::ViewModel)` and then builds its correction matrix from **both** cameras (XY from the viewmodel projection, Z/W from the main one). The albedo route has no program to latch a dedicated one from -- `VIEWMODELVP` is empty by design here, precisely because the visor's program is also `GUESTLIGHTVP` -- so when the albedo list is armed, `VIEWMODEL >= 2`, and no viewmodel reference is latched, the VIEW_MODEL camera is submitted as the world camera's **twin**. That is the correct answer rather than a convenient one: identical cameras yield an identity correction, which is the intended no-op, and what the tagged geometry gains is exactly the stated goal -- the viewmodel pass's own near plane and its exclusion from world clipping. A real viewmodel reference, if one is ever latched, keeps priority. A `SetupCamera` rejection is censused once (`Remix vmcam-twin:`) and falls back silently. `rtx.viewModel.enable = True` is already in conf. Counter `vmcam_twin`. **ROUND 21 -- THIS GATE WAS BROKEN AND ROUND 20 SHIPPED ON TOP OF IT.** The condition at `RemixGSRender.cpp:4675-4678` tested `viewmodel_albedo_count() != 0`, i.e. the size of `RPCS3_REMIX_VIEWMODELALBEDO` -- and that list is deliberately **blank** on this title, because both visor albedos are shared with the sky dome and the effects program. Blanking it therefore disarmed the **camera** as a side effect. MEASURED, from the round-20 build's own run (`build=Aug 16 2026 07:33:57`, `remix_dump.log` line 1450436): `vm_tagged_pair=123` with `vmcam_twin=0 vmcam_real=0 vmcam_applied=0 vmalbedos=0 vmpairvps=1 vmpairalbedos=3`. The pair gate round 20 shipped was tagging draws into a pass that could not exist. Re-read from the deployed runtime this round: `createViewModelInstances` takes the early-out at `rtx_instance_manager.cpp:1504-1507` and calls `cleanupAllPersistentViewModelInstances()`; because that return happens **before** the candidate loop at `:1553`, nothing touches `m_vkInstance.mask` and the tagged draws stay in the world pass unchanged -- so the tag was inert rather than destructive, but the `VMBASIS` flip still ran, because that is applied at the **tag** site (`:17861`) and not at the camera site. The gate now arms on **any** route that can set the tag: `VIEWMODELALBEDO`, the `VMPAIRVP`+`VMPAIRALBEDO` pair, `VIEWMODELVP`, or a launcher-raised `VMDEPTHOFFSET`. The always-on built-in depth rule is deliberately excluded -- it is the default for every configuration, and arming on it would submit this camera on titles that never asked for a viewmodel pass. **Validity is per-frame and decays silently:** `isCameraValid` is `m_frameLastTouched == frameIdx` (`rtx_camera.h:283`) and `CameraManager::onFrameEnd` never resets it, so the client must submit this camera **every frame** or the pass drops out for that frame. **This is round 21's one behaviour change, and it is the revert knob for it:** once the camera is valid a VIEW_MODEL-tagged instance gets `mask = 0` (`rtx_instance_manager.cpp:1562`) and leaves the world pass, so `=0` is what puts the 123 tagged draws back where round 20 had them. | |
| `RPCS3_REMIX_SKYEMISSIVE` | empty | **The sky dome's own emissive material (round 13).** Comma-separated 16-hex albedo *content* hashes, bounded at 8, given a non-zero `emissiveIntensity` **and** an emissive texture pointed at their own albedo (the synthetic `0x<hash>` path -- the same thing the runtime's WorldUI arm does internally at `rtx_instance_manager.cpp:1107`), so the dome glows per-texel rather than as a flat mean colour. Separate list and separate intensity from `RPCS3_REMIX_EMISSIVE`, which is the ceiling-fixture list. **What this actually fixes is occlusion, not visibility** -- see `SKYEMISSIVEBLEND`. Empty list restores prior behaviour bit for bit. Counters `mat_skyemissive`, `mat_skyunordered`; census `Remix skyemissive:` (gated on `DIAGLINES`). | |
| `RPCS3_REMIX_SKYEMISSIVEINT` | `2.0` | `emissiveIntensity` for the `SKYEMISSIVE` list. **2.0 is not a guess**: `rtx_instance_manager.cpp:1105` sets exactly `2.0f` on every WorldUI instance, which is what a dome listed in `rtx.worldSpaceUiTextures` renders at today, so this default is a no-op in brightness. Inert while the hash remains in `rtx.worldSpaceUiTextures`, because WorldUI's override runs after material creation and re-forces 2.0. | |
| `RPCS3_REMIX_SKYEMISSIVEBLEND` | `1` (on) | **The half that gives the sun back (round 13).** Declares `BlendType::kEmissive` on the listed dome's material (`useDrawCallAlphaState=0`, `blendType_hasvalue=1`, `blendType_value=6`), so the runtime takes `calculateAlphaState`'s `!useLegacyAlphaState` arm and reads it directly -- no dependence on blend-factor pattern matching or on `rtx.enableEmissiveBlendModeTranslation`. `alphaState.emissiveBlend` then sets `m_isUnordered` (`rtx_instance_manager.cpp:1216`) and the instance takes `OBJECT_MASK_UNORDERED_ALL_EMISSIVE` (`:1270`), which is deliberately **absent** from `OBJECT_MASK_ALL_STANDARD` -- so the direct shadow ray misses the dome while primary rays still see it. Without this the dome is a closed opaque shell taking `OBJECT_MASK_OPAQUE` (`:1283`) and blocks 100% of the fallback distant sun, which is the entire "the sky lights up the environment instead of the sun" symptom. `0` keeps the glow and keeps the occlusion -- the A/B that attributes any lighting change to this mechanism. | |
| `RPCS3_REMIX_VMCAMFOVX` / `RPCS3_REMIX_VMCAMFOVY` | `0` / `0` (off) | **A real viewmodel projection instead of a twin (round 13).** Field of view in degrees for the `REMIXAPI_CAMERA_TYPE_VIEW_MODEL` camera. Writes `m[0][0]` and `m[1][1]` (`= 1/tan(fov/2)`, sign preserved) of the world projection and nothing else -- which is the complete payload, because the runtime overwrites the depth row from the main camera (`rtx_instance_manager.cpp:1526-1528`), making the measured near plane irrelevant. **Why this matters:** with an exact twin the runtime's correction matrix `mainViewToWorld * (mainProjectionToView * vmProjection * scale) * vmWorldToView` cancels to the identity (`rtx.viewModel.scale` defaults to 1.0), so the viewmodel pass ran every frame and moved nothing -- `vmcam_twin = 13869 = cam_resolved` while the visor still clipped. Both knobs must be set; one alone is ignored, and a degenerate FOV falls back to the twin rather than submitting a camera the runtime would build a bad correction from. Measured values for Haze: `60.001` / `36.132` against the world's `72.000` / `44.634`. Counter `vmcam_real`. | |

## Instance categories

Comma-separated 16-hex-digit albedo *content* hashes -- the same values the `Remix tex=` dump
line and the Remix dev menu display. Setting `categoryFlags` at submit time is the only
mechanism that reaches a draw on the `submitExternalDraw` path.

**Correction (round 5).** The sentence that used to stand here -- "Remix's own `rtx.*Textures`
conf lists are matched on the D3D9 path only, which is why tagging a texture in the dev menu
does nothing for this backend" -- is **false for this fork**. `rtx_fork_submit.cpp:65-100`
applies the `rtx.*Textures` conf lists to API draws, and it is called from
`rtx_scene_manager.cpp:2432` on the `submitExternalDraw` path. So a hash added to a conf list
*does* reach these draws, and the knobs below are a convenience (and the only mechanism for
categories the conf has no list for), not the sole one.

| name | default | effect | in GUI |
| --- | --- | --- | --- |
| `RPCS3_REMIX_CAT_SKY` | empty | Tag the listed hashes SKY -- selects the sky camera and hides the instance. | yes |
| `RPCS3_REMIX_CAT_HIDE` | empty | Tag the listed hashes HIDDEN. (IGNORE is a no-op for API draws and is not used.) | yes |
| `RPCS3_REMIX_CAT_PARTICLE` | empty | Tag the listed hashes PARTICLE. | yes |
| `RPCS3_REMIX_CAT_DECAL` | empty | Tag the listed hashes DECAL_STATIC. | yes |

One further category is set at submit time but is not a hash list:
`RPCS3_REMIX_SMOOTHNORMALS` (see *Geometry and world placement*) tags every world instance
`SMOOTH_NORMALS`. It is global because the backend fabricates the normal on every vertex of
every mesh, so a hash list would mean hunting hashes before any lighting improved at all.

## Lighting

The camera-parked debug sphere blows out everything near it and crushes everything far, so the
readable default is a distant sun; the sphere stays as an optional fill, off by default. The
sun direction is in the recovered world space and is normalised in code.

| name | default | effect | in GUI |
| --- | --- | --- | --- |
| `RPCS3_REMIX_NOSUN` | off | No default sun at all. The environment can force this on but not off; the config is the way to re-enable the sun once a script has disabled it. | yes |
| `RPCS3_REMIX_SUNDIR` | `-0.3509,-0.9023,-0.2506` | Sun direction as `"x,y,z"` in recovered world space, normalised in code. | yes |
| `RPCS3_REMIX_SUNSKY` | `0` (off) | **Round 23 -- a PER-LEVEL sun, derived from the sky dome's own texture.** `SUNDIR` above is one vector for the whole process, which is wrong by construction on a title whose sky is per-area. The key is the **sky dome's albedo hash**, because that is a per-area value the backend already watches change and it needs no level-change signal from the guest. Only albedos on `RPCS3_REMIX_SKYEMISSIVE` are measured or matched -- that list *is* the per-level key. **Why it works, MEASURED offline on the dumped Selva dome (`bin\remix_tex\unit0_D1A6D1B27ADE6232_2048x1024.bmp`): the sun is PAINTED INTO the dome texture.** Peak Rec.709 luma **249.2** (rgb 255,251,214, a warm white) at texel (1340,513); **112 texels sit within 2 % of that peak** and their centroid is uv **(0.6769, 0.5015)**; the top half of the image (v < 0.5) is solid black, i.e. the panorama occupies v in [0.5, 1]. Zero texels reach luma 250 and 3060 reach 220, so it is a **broad glow, not a hard disc** -- which is why the code uses the luminance-weighted centroid of the near-peak texels and not the single brightest one (they are 46 texels apart). Two halves: `texture_cache::upload` measures that centroid once per dome upload into `texture_entry::peak_uv` (two extra walks of the buffer for one or two textures per level; every other texture skips the block on a <=8-entry hash compare), and at the dome's first draw `derive_sky_sun` finds the dome **vertex** whose texcoord is nearest it and takes the world ray from the eye to that vertex, negated into the TRAVEL convention. The eye is the ray's origin rather than the dome's centroid on purpose -- a sky dome is by definition drawn to surround the viewer, and a hemisphere's centroid sits well above its centre, which would bias every derived elevation downwards; the centroid answer is computed anyway and printed beside it as `centroid_travel=` so a dome that is not eye-centred shows up in the log instead of being silently wrong. Solved **once per dome albedo** (the sun does not move within a level) and republished on every later draw of that dome, so walking into a new area re-aims the light on the first frame the new dome appears. Reader: `Remix sunmap:` -- `skyalbedo=`, `travel=`, `peakuv=`, `uverr=` (how close the best vertex's texcoord got; large means the dome's UVs do not cover the bright region and the answer is a guess), `vtx=`, `centroid_travel=`, `sundir=` (what `SUNDIR` would have given). Counters `sun_sky_examined/solved/refused`, `sun_sky_slots`. `0` = round 22 exactly. | |
| `RPCS3_REMIX_SUNMAP` | empty | **Round 23 -- the per-level sun config file, and it OVERRIDES the derivation above.** `<skyalbedo>:<x>,<y>,<z>;<skyalbedo>:<x>,<y>,<z>`, bounded at 8 entries. Vectors need not be unit length (normalised in code) and use the same convention as `SUNDIR`: the direction the light **travels**, sun -> scene. Workflow: read `travel=` off `Remix sunmap:` (or aim it by eye), paste it in, and it sticks for that level. Full precedence, highest first: **`SUNMAP` > `SUNSKY` > `SUNTRACK` (the card) > `SUNDIR`** -- `SUNDIR` still covers every level that matches nothing, so a level with no entry is unchanged. A `SUNMAP` entry needs no geometry and no texture measurement at all; it resolves the first time that dome albedo is bound. | |
| `RPCS3_REMIX_SUNSKYMINVTX` | `64` | **Round 23.** Vertex floor for the dome draw `SUNSKY`'s derivation is allowed to use, so a 4-vertex card that shares the dome's vertex program cannot win the UV match. Every sky-dome draw in the census carries 134..1446 vertices. | |
| `RPCS3_REMIX_SUNSKYPEAKFRAC` | `98` | **Round 24.** Percent of the dome texture's **peak** luma a texel must reach to join the luminance-weighted centroid that becomes `peak_uv`. Clamped to `50..100`; `98` reproduces the round-23 constant **bit-exactly**, so unset is byte-for-byte round 23. That required care and a review caught it: the fraction is computed as `percent / 100.f`, **not** `percent * 0.01f`. MEASURED in IEEE-754 single, `0.98f` is `0x3F7AE148` but `98.f * 0.01f` is `0x3F7AE147`, one ULP low, because `0.01f` is itself `0.00999999977648...` and the product rounds down past `0.98f`. Division is correctly rounded, so `p/100` lands on the nearest representable value by definition -- exact at 50, 85, 90, 98 and 100, where the multiply form is wrong at both 85 and 98. **WHY IT NEEDS A KNOB, MEASURED offline on `bin\remix_tex\unit0_D1A6D1B27ADE6232_2048x1024.bmp`:** at `98` only **112 texels qualify and every one of them is on a single row** (y=513, x 1304..1463) -- a 160x1 streak, not a disc -- because that row is a one-row spike (row 512 max 162.75, row **513 max 249.18**, row 514 max 230.25) sitting on the top edge of the panorama band. The centroid is therefore dragged to `v=0.50146`, the very first row of the used band. Sensitivity on that texture: `98` gives 112 texels across 1 row and `peak_uv=[0.67693 0.50146]`; `90` gives 972 texels across 28 rows and `[0.66404 0.50961]`; `85` gives 19418 texels across 229 rows and `[0.63368 0.56012]`. A knob rather than a changed constant **because the threshold is shared by every dome**, and the dome that currently solves correctly (`CDFE11B12552EA2D`, `uverr=0.035`) must not be moved in order to fix another. Read `sunskypeak=` on `Remix stats:` and `peakuv=`/`uverr=` on `Remix sunmap:`. | |
| `RPCS3_REMIX_SUNSKYDOWN` | `1` (on) | **Round 24 -- a latch guard, not a tuning knob.** Refuses a **derived** sky sun whose travel vector does not point downwards, i.e. `travel[1] >= 0`. Travel is "the direction the light travels", so a non-negative y is a sun at or below the horizon lighting the level from underneath, which no daylit sky dome ever wants. **Why it must exist:** a solution is taken **once per dome albedo and never revised**, so one bad derivation aims that level's sun wrongly for the entire session -- the exact failure mode four of round 23's six review defects shared. MEASURED on this build: the Selva dome `D1A6D1B27ADE6232` derived `travel=[0.58892 0.11312 -0.80024]`, a **positive** y, against the working dome's `[-0.19933 -0.38269 0.90212]`. With the guard the bad solve is refused and `SUNDIR` keeps that level -- what shipped for every round before the derivation existed, and strictly better than a permanent wrong aim. **The refusal is STICKY, and a review defect showed it has to be.** `travel` is rebuilt from the LIVE camera position on every dome draw and the caller is `if (!solution && derive_sky_sun(...))`, so a bare refusal would re-run the derivation every draw and the dome would latch on the first frame whose camera happened to put `travel[1]` a hair below zero -- a horizon-grazing sun, chosen nondeterministically, which is a *worse* version of the bug the guard exists to prevent. Instead the refused albedo **claims its slot with `source = 0`**, so `find_sky_sun` answers for it from then on, `derive_sky_sun` is never re-entered for it, and the publish site refuses to read a `source = 0` slot. Both `sun_sky_solved` and `sun_sky_refused` therefore stay bounded by the number of distinct dome albedos. **Deliberately not applied to `SUNMAP`:** a hand-written vector is an explicit instruction and is never second-guessed. `0` restores round 23 exactly. **Acceptance:** read `sunsky_up=` on `Remix stats:` -- a counter dedicated to *this* guard, because the three pre-existing refusal paths are structurally expected traffic (every 4-vertex card sharing a dome albedo trips the vertex floor) and a shared `sun_sky_refused` could never be evidence. The guard also emits one `Remix sunrefuse: skyalbedo=... reason=horizon travel=[...] peakuv=[...] uverr=... cam=[...]` line per refused dome, which is the 98-threshold baseline a re-tuned `SUNSKYPEAKFRAC` is compared against. | |
| `RPCS3_REMIX_SUNRADIANCE` | `3` | Distant-light radiance. The shader divides a distant light's radiance by `sin^2(halfAngle)` and samples the cone uniformly, so the delivered irradiance is roughly `pi * radiance` regardless of angular diameter -- single digits are the useful range, not the sphere light's 100. | yes |
| `RPCS3_REMIX_SUNANGLE` | `0.5` | Sun angular diameter in degrees. | yes |
| `RPCS3_REMIX_SUNTRACK` | `0` (off) | **Round 14.** Aim the distant sun at the game's own sun card instead of at `SUNDIR`'s static vector. `0` = today's behaviour bit-exactly. `1` = track a card whose albedo is pinned in `SUNCARDALBEDO`. `2` = also allow the census's highest-in-sky candidate to drive it when no pin is set (can be wrong on its own evidence, hence not the default). Counters `suncard_elected` / `sun_retargeted`. | |
| `RPCS3_REMIX_SUNCARDALBEDO` | empty | **Round 14.** Up to 4 comma-separated albedo content hashes naming the sun card. Nothing in the repository identifies it -- read `Remix suncard:` and paste. Counter `suncard_pinned` must climb once set. | |
| `RPCS3_REMIX_SUNCARDVP` | empty | **Round 16.** Up to 4 comma-separated vertex-program hashes. When non-empty a draw is a pinned sun candidate only if its **program AND its albedo** both match, the same rule `SKIPPAIRVP`+`SKIPPAIRALBEDO` and `VIEWMODELVP`+`VIEWMODELALBEDO` already use. Empty = albedo-only, i.e. round 14 bit-exactly. It exists because Haze's warmest census candidate `C61753D31FB96507` is also the visor's blended layer and is drawn by three different programs, so an albedo alone cannot name one of them. **Gates the render-side pin only** -- `SUNCARDEMISSIVE` runs in `CreateMaterial`, which is keyed on texture content and has no program in scope, so the material pin stays albedo-only and cannot be narrowed this way. Echoed as `suncardvps=` on the run-start and live lines; attributed per draw by `pin_alb=` / `pin_vp=` on `Remix suncard:`. | |
| `RPCS3_REMIX_SUNCARDCENSUS` | `1` | **Round 14.** `Remix suncard:` -- one line per `(vp, albedo)` per window (cap 32), `DIAGLINES`-gated. Names every draw shaped like a sun sprite with its world centre, the camera, the derived direction in **both** senses, elevation, azimuth, mean RGB and alpha range. Aims nothing, refuses nothing. Measured population on the round-13 Selva run: 27 of 110 `(vp, albedo)` pairs before the elevation gate. | |
| `RPCS3_REMIX_SUNCARDMAXVTX` | `64` | **Round 14.** Vertex-count bound of the sun-card shape test. | |
| `RPCS3_REMIX_SUNCARDMINELEV` | `2` | **Round 14.** Degrees above the horizon a candidate must sit. Negative disables the test. Without it every blended ground decal in front of the camera is a candidate. | |
| `RPCS3_REMIX_SUNCARDMINDIST` | `0` (off) | **Round 16.** Minimum eye distance for a census **line**. Not part of the shape gate: no counter moves, no pin is affected, and a pinned or elected row always prints. Measured on the 2026-08-16 run -- 3394 of 6796 `Remix suncard:` lines (49.9%) sit closer than 4 units, 2919 of them 4-vertex HUD quads from `vp=2f64c2f8ffd6add1`, and 210 of the 236 distinct `(albedo, vp)` pairs survive a floor of 4. Distance rather than an extent or vertex floor because both of those delete real candidates: 1360 lines (20%) are at `dist >= 5` with `ext < 1`, and the far cards carry 3 to 8 vertices. `0` restores the full census exactly. | |
| `RPCS3_REMIX_SUNTRACKDEG` | `1` | **Round 14, CORRECTED IN ROUND 26.** Degrees of movement before the light is re-aimed. The old text here said "the Remix C API has no update-light entry point, so a retarget costs a destroy+create" -- **both clauses were false, and the destroy was deleting the sun outright.** In the deployed numos3 runtime `remixapi_DestroyLight` only *queues* the handle (`rtx_remix_api.cpp:1605-1616`) while `remixapi_CreateLight` is *immediate* (`:1520-1552`), and `remixapi_Present` drains the queue applying **destroys first** (`:2134-2139`) and tombstones any create for the same handle in that frame (`:2142-2144`). So destroy-then-create-with-the-same-hash is a delete, not a replace, and `ensure_sun_light()` can never rebuild it because the handle *is* the hash and stays non-null. Measured: in the round-25 run the sun was created at 0:01:27, retargeted 89.24 deg at 0:01:27, **and there was no distant sun in the scene for the remaining 14 minutes.** `CreateLight` with the same hash was already an update in place (`rtx_light_manager.cpp:722-732`); the destroy is gone. A retarget is now cheap, so this is a hysteresis knob only. There is also a real `remixapi_UpdateLightDefinition` (`remix_c.h:824`, interface slot `:1067`) if a queued update is ever wanted. Watch `sun_retargeted` for cost and **`sun_destroys`, which must read 0 on every in-play `Remix stats:` line** -- non-zero means the sun is being deleted again and its direction will appear frozen. | |
| `RPCS3_REMIX_SUNCARDEMISSIVE` + `RPCS3_REMIX_SUNCARDINT` | `0` (off) + `2.0` | **Round 14.** The sun card renders as a hard opaque rectangle with visible edges. This gives a `SUNCARDALBEDO` hash the same treatment `SKYEMISSIVE` gives the dome -- emissive material, albedo as the per-texel emissive texture, and `BlendType::kEmissive` so the instance becomes unordered. For a glare card that is the whole fix: opacity goes to 0, dark texels stop being drawn, the rectangle stops occluding, and only the bright core emits. Counter `mat_suncard`. | |
| `RPCS3_REMIX_CAMLIGHT` | `0` (off) | Radiance of the camera fill light. The measured useful range is 8..40. `env_float` rejects non-positive values, so an explicit `0` from the environment cannot be told from unset -- use the config entry to turn it off. **Round 26, two things worth knowing before blaming it for anything.** (1) **It is currently inert**, for the same reason the sun was: `place_debug_light` destroys and re-creates hash `0x3` on every frame, and the deployed runtime applies the destroy at `Present` after the immediate create, so it is erased every frame. Deliberately not fixed in round 26 -- doing so would turn a light that has contributed nothing since round 5 back on and confound the sun A/B. (2) **Even live it cannot wash out the sun.** At the default `LIGHTRADIUS` of 0.1 a sphere of radiance `L` gives irradiance `L*pi*r^2/d^2 = 0.377/d^2` at `CAMLIGHT=12`, against a distant light's `pi*radiance = 9.42` which is independent of distance and of angular diameter: the sun is **25x** stronger at 1 unit and **225x** at 3. Crossover is at d ~= 0.2 units. And setting it to `0` **raises** the radiance rather than lowering it, because `place_debug_light` then falls back to `LIGHTRADIANCE` (default 100). | yes |
| `RPCS3_REMIX_LIGHTRADIUS` | `0.1` | Radius of the debug sphere light, so a derived camera can be judged visually. | yes |
| `RPCS3_REMIX_LIGHTRADIANCE` | `100` | Radiance of the debug sphere light. | yes |
| `RPCS3_REMIX_GUESTLIGHTVP` + `RPCS3_REMIX_GUESTLIGHTFP` | empty | **Optional narrowing as of round 5** (they used to be *required*). A program hash restricts injection to that program; `0`/unset means "any program". Requiring the pair is what limited Haze's dark cargo area to four bulbs: the configured vp was a shared world program that added no selectivity while excluding every fixture drawn by anything else. | |
| `RPCS3_REMIX_GUESTLIGHTALBEDO` | empty | Comma-separated 16-hex albedo *content* hashes of lamp fixtures -- **the one required key**. A sphere light is created at each distinct fixture position. A comma list so every fixture picked with Ctrl+Click becomes a light source by editing the launcher, with no rebuild. Bounded at 16 (was 8). Counters: `guest_lights`, `guest_light_match`, `guest_light_capped`. | |
| `RPCS3_REMIX_GUESTLIGHTALBEDO2` | empty | One further albedo hash, matched only on a blended, non-depth-writing draw -- the glow card in front of a fixture rather than the fixture itself. Kept separate from the list above precisely because that restriction is part of the rule. | |
| `RPCS3_REMIX_GUESTLIGHTRADIUS` | `0.2` | Minimum radius of each injected guest light. | |
| `RPCS3_REMIX_GUESTLIGHTRADIUSSCALE` | `0.35` | Radius becomes `max(GUESTLIGHTRADIUS, draw world extent * this)`. A fixed 0.2 sphere sits at the bbox *centre* of a 2.6-unit lamp housing -- inside its own shade, occluded by the mesh it was derived from. `0` restores the fixed radius. | |
| `RPCS3_REMIX_GUESTLIGHTRADIANCE` | `30` | Radiance of each injected guest light. The doc'd useful range for a 0.1 sphere in *open air* is 8..40; an occluded industrial interior needs roughly ten times that, and the Haze launcher ships 150. This is the brightness knob -- no rebuild. | |
| `RPCS3_REMIX_GUESTLIGHTCOLOR` | `1` | Tint the light by the mean RGB of the fixture's own decoded albedo, normalised so the largest component is 1 (hue only; brightness stays `GUESTLIGHTRADIANCE`). `0` restores the fixed warm `1 / 0.86 / 0.68` constant. | |
| `RPCS3_REMIX_GUESTLIGHTIDLE` | `0` (never) | Destroy a guest light no draw has re-matched for this many frames. `0` keeps every light forever, which is what a room lit by its own ceiling fixtures wants -- a lamp does not stop existing when the player turns around. Counter: `guest_light_reaped`. | |
| `RPCS3_REMIX_GUESTLIGHTMAX` | `64` | Live guest-light cap, previously hardcoded. Clamped to 1..4096. `guest_light_capped` counts matches refused at the cap. | |
| `RPCS3_REMIX_GUESTLIGHTLUM` | `0.7` | Luminance floor (Rec.709, over the albedo's own decoded mean RGB) for a submitted draw to appear on a `Remix light-candidate:` census line. Measurement input for the fixture list; nothing acts on it unless `GUESTLIGHTAUTO` is on. | |
| `RPCS3_REMIX_GUESTLIGHTMAXEXT` | `6` | World-extent ceiling for the same census. Together with the render-state test -- opaque fixture (`depth_write=1, blend=0`) or glow card (`blend=1, depth_write=0`) -- these are the three terms of the guest-light discriminator. The lines double as `EMISSIVE` candidates. | |
| `RPCS3_REMIX_GUESTLIGHTAUTO` | `0` (off) | Treat every census-qualifying draw above as a fixture trigger, through the existing dedup / cap / idle machinery, without its albedo being on `GUESTLIGHTALBEDO`. **Default off deliberately:** the last generalisation of fixture identity (albedo alone) put lights on doors and sheet-metal covers, because Haze shares fixture textures with ordinary props. The census de-risks the list in one ordinary session, and flipping this on is a launcher edit inside the same sitting. | |
| `RPCS3_REMIX_EMISSIVE` | empty | Comma-separated 16-hex albedo *content* hashes made emissive at material creation, so a fixture's own visible surface glows from exactly where the game says the light comes from. No embedding problem -- it emits from the drawn geometry. Bounded at 16. Counter: `mat_emissive`. Materials are cache-keyed by content + sampler/alpha state and the list is latched once, so cache coherence is unaffected. | |
| `RPCS3_REMIX_EMISSIVEINTENSITY` | `1` | `emissiveIntensity` for the listed materials. The colour is the texture's own normalised mean RGB (white if it never decoded). | |
| `RPCS3_REMIX_FPVCOL` | `1` (on) | **The vertex-coloured effects fix (round 9).** State the colour pipeline the *fragment program's own bytes* prove in the per-draw `remixapi_InstanceInfoBlendEXT`, instead of the parity default that says "colour = Texture" for every draw. Haze's yellow nectar danger pulse binds no texture, so round 6's neutral grey 2x2 material attaches -- and its fragment program, decoded from `DBB822C008101A2.fp`, is literally **one instruction: `MOV r0.xyzw, COL0.xyzw`** with the END bit. Its colour *is* the vertex colour; `apply_vertex_colour` already decodes ATTR3 into the submitted mesh, the runtime's API path already binds `color0Buffer` from `remixapi_HardcodedVertex::color`, and the blend extension was then telling the fixed-function stage to take colour from the Texture argument only. The vertex colour was not missing -- it was being dropped by explicit configuration. With the knob on, a program classified `vcol_pass` gets `textureColorArg1Source = VertexColor0` with `SelectArg1`; one classified `vcol_modulate` (`TEX r0, TEX0, tex0` then `MUL r0, r0, COL0` -- the giant flower, `D12A627700B7818A.fp`) gets `Arg1 = Texture`, `Arg2 = VertexColor0`, `Modulate`. Both also clear `isVertexColorBakedLighting`, because under that flag the runtime **normalises the vertex colour by its max channel and lerps it toward white by `rtx.vertexColorStrength` (0.6 in this fork)** -- the baked-lighting reading, which for an effect whose colour *is* the vertex colour destroys the ramp. Nothing else in the fill moves: `alphaTest*`, `tFactor`, the blend factors and the write mask are untouched. Anything deeper than the two decoded shapes -- extra instructions between the sample and the output, swizzled or negated reads, a conditional write, flow control -- classifies `other` and changes nothing. Counter `fpvcol_applied` (draws) and `fpclass=<pass>/<mod>/<col1>` (programs) on `Remix live:`; censuses `Remix fpvcol:` (once per program) and `Remix effect:` (once per vp/fp per window); `fpcol=` on every `Remix picked:` line. `0` restores today's parity fill for every draw bit-exactly -- that A/B is the whole attribution. | |
| `RPCS3_REMIX_VCOLMOD` | `1` (on) | **The mesh-hash half of the same fix (round 9), severable on its own.** Lets `apply_vertex_colour` run on a **textured** draw when its fragment program proves the modulate. The call was gated `!material` with the comment *"a textured draw's ATTR3 modulates its albedo in the title's own fragment program with per-title semantics this backend does not read, so tinting one would be a guess"* -- and `scan_fragment_program` now **reads those semantics**, so the objection is discharged for exactly the `vcol_modulate` population and no other. Kept apart from `FPVCOL` because this is the half that moves **mesh content hashes**: the vertex colour is hashed into the mesh key, so an *animated* vertex colour makes a new mesh per animation step. That is already true today for the material-less population (the pulse); this extends it to FP-proven textured effects. Watch `mesh_created` on `Remix live:`; counter `vcol_mod`. `0` restores the `!material` gate exactly. | |
| `RPCS3_REMIX_VCOLBGRA` | `1` (on) | **The blue-HUD fix -- a byte-order bug, round 10.** `apply_vertex_colour` packed `R \| G<<8 \| B<<16 \| A<<24` (bytes R,G,B,A), and the runtime binds that field as **`VK_FORMAT_B8G8R8A8_UNORM`** -- bytes B,G,R,A -- in *both* `color0Buffer` bindings in the fork's `rtx_remix_api.cpp` (`offsetof(remixapi_HardcodedVertex, color)`). Red and blue were therefore swapped in **every** replayed vertex colour, and Haze's amber HUD nectar/health gauges rendering "blue-ish" is that bug verbatim: amber is R >> B, which reads back as B >> R = cyan. Now packs D3DCOLOR order to match the format. Greys are swap-invariant, so this is *not* why the nectar pulse read flat grey (that is `FPVCOLEMISSIVE`), and `apply_vertex_alpha`'s `0x00FFFFFF` white constant is swap-invariant and untouched. **Expected one-time churn:** mesh *content* hashes cover these bytes, so the first run after this change re-creates every coloured mesh exactly once -- a bounded `mesh_created` bump that then settles, not a leak. `0` restores the swapped pack bit-exactly, which is the A/B that attributes the gauges turning amber. | |
| `RPCS3_REMIX_FPVCOLALPHAGATE` | `1` (on) | **The invisible-vegetation repair (round 10) -- a round-9 regression.** Applies the **alpha** halves of the `FPVCOL` replay only when the guest actually consumes fragment alpha (`blend_enabled() \|\| alpha_test_enabled()`). Round 9 replayed alpha unconditionally, which is unfaithful to the hardware on a draw with blend and alpha test both off -- the guest ROP reads no fragment alpha there -- and actively destructive. The Selva effect census caught it: `vp=c97cd1531ac480d8 fp=4f1d1bce7ffdfd9f class=vcol_modulate material=1 blend=0 dw=1 vtx=649..1960 vcol=[rgb-varies=1 alpha=00..00]`. With `textureAlphaArg2Source = VertexColor0` + `Modulate` the runtime opacity is `texA x 0 = 0`, and the fork's `calcOpaqueSurfaceMaterialOpacity` then takes the blending-**disabled** arm `opacity = newAlpha > 0 ? 1 : 0` (`opaque_surface_material_blending.slangh`) -- zero. The draw renders **fully invisible**. Opaque vegetation carrying 0-alpha vertex colours has been invisible since the round-9 build. The RGB halves -- the whole visible effects fix -- are untouched, as are the KIL and A2C arms, which set their own alpha test from their own evidence. Counter `fpvcol_alpha_skip`. `0` restores round 9's unconditional alpha replay; if something that *was* correctly transparent turns solid, that is the sever. | |
| `RPCS3_REMIX_FPVCOLEMISSIVE` | `1.0` (`0` = off) | **The self-illuminated effects material (round 10).** The nectar danger pulse is no longer mis-coloured -- round 9 fixed that, and the runtime honours `SelectArg1(VertexColor0)` for albedo so the pulse's albedo *is* its vertex colour -- it is **unlit**: albedo is reflectance, and in a dark room reflectance reads grey. The decisive reading is the fork's own shader: the **same fixed-function stage is applied a second time to `emissiveColor`** (`opaque_surface_material_interaction.slangh` drives `chooseTextureOperationColor(emissiveColor, ...)` from the *same* `surface.textureColorArg*` sources the albedo block just used), and `emissiveRadiance = emissiveBlendOverrideInfluence(1.0 default) x emissiveColor x emissiveIntensity`. With round 9's arg-source replay already shipped, **the only missing ingredient was a material whose `emissiveIntensity > 0`** -- the per-vertex gradient then reaches emissive radiance intact, with no fork edit and no per-draw materials. Attaches a self-lit twin of the neutral grey (its own constant hash, white 2x2, `emissiveColorConstant = {1,1,1}`) to draws matching the decoded shape and nothing wider: no guest material, fp classified `vcol_pass`, `sampled_mask == 0` (a program that samples nothing and writes `MOV out, COL0` is self-illuminated by definition), and the submitted vertex colours actually **vary**. That last term is a named exclusion: Haze's flat sky-dome variant (`fc0fac8afccec49a`, `vtx=32`, one flat colour) satisfies everything else and must stay on today's path, because a uniformly-coloured shell turned emissive is a light source the size of the level. Requires `FPVCOL` (the arg sources *are* the mechanism; without them the emissive stage would read the Texture argument and glow flat white). The HUD gauges cannot use this lever -- they are textured, with shared per-texture materials -- theirs is `RPCS3_REMIX_EMISSIVE`. Counter `fpvcol_emissive`; census flag `selflit=`. `0` restores the plain neutral-grey attach. | |
| `RPCS3_REMIX_FPVCOLADDITIVE` | `1` (on) | **The translucency half of the same fix (round 10), severable in both directions.** For exactly the `FPVCOLEMISSIVE` signature **plus** depth-write disabled and guest blend disabled -- the overlay signature the pulse actually arrives with (`blend=0 dw=0 alpha=ff..ff`, because on PS3 this pass is composited offscreen) -- states `ONE/ONE` with `ADD` on the instance blend ext instead of the opaque alias. The runtime classifies that pair `BlendType::kEmissive` (`calcOpaqueSurfaceMaterialOpacity`'s kEmissive arm: opacity -> 0, influence 1), so the surface **occludes nothing and contributes only its emissive radiance** -- and the black portions of the gradient vanish for free, because black added is nothing. Without it the pulse would be a solid glowing wall standing in the room. Deliberately **interpretive** (we approximate an offscreen composite in world space), hence its own knob; draws whose guest blend is *enabled* never reach here and keep their own translated blend exactly as today. Counter `fpvcol_additive`; census flag `additive=`. `0` restores the opaque-alias fill. | |
| `RPCS3_REMIX_HAZEFADE` | `1` (on) | **The “black backdrop plane”, solved as an un-replayed fade (round 11).** The plane standing in the Selva jungle is an atmospheric haze / god-ray card and it is **exactly where the game put it** -- `pick-deep` proves it three ways: its fused matrix equals the live camera's V x P to ~1e-5 (`basis=[1 0.999983 1]`, `ref=anchor`), its submitted vertices are world-space at `raw=[-33.55 0.896 34.73]..[12.07 18.30 36.31]` while the camera sits at z~-42, and `attr0=ok type=2 size=3 w=[1..1]` makes its w-divide a no-op. The “placed at the world origin” reading was an artefact of the pick's `origin=` field, which is the **instance transform's** translation (identity, so zero), not the geometry's location. What was missing is the fade, and the fade lives entirely in fragment-program maths this backend never replayed -- on top of an albedo whose alpha channel is fully opaque (`alpha_range=255..255` on the `Remix kil:` census), so the card arrived at alpha 255 under SRC_ALPHA/ONE_MINUS_SRC_ALPHA: an effectively opaque plane, path-lit in a dark jungle. Decoded from `EDC10321BF7CEB8F.vp` (30 slots) and `3C3D0320F8611D4F.fp` (23 instructions, logged `fp=a20a7a99872b615a`) and now replayed **per vertex**: `COL0 = ATTR3 * c[18]`; `rgb = COL0.rgb * 2 * 0.944243`; `alpha = COL0.a * dist_ramp * angle_ramp`, where the two ramps are the fp's own saturated ramps over `|viewpos|` and `|cos(theta)|` with its embedded literal `K = [0, 0, 0.9, 0.4]`, and `theta` is the angle between the view-space position and a per-vertex direction the vp expands from a **quaternion in ATTR9** (`o8 = A*c[8] + B*c[9] + C*c[10]`, the third rotation column). **Two facts the plan did not have, both from the bytes:** `K.x == K.y == 0` makes the *distance* ramp identically 1 for this program, so shipping “the distance ramp alone” would have shipped no fade at all -- the angle term *is* the fade; and the o8 chain is eight multiply-adds, not the intricate walk the plan feared, so nothing is deferred. Both ramps are implemented in the ucode's own degenerate-guard form, so a sibling carrying a real distance ramp is honoured without another decode. Matched by fp raw hash **plus** the vp's `scaled` route, not by a hash list of draws. The card is **not hidden** -- the user asked for it rendered where it is, fading as authored. Counter `hazefade`; census `Remix hazefade:` printing `a_range=` and both ramps. Acceptance is numeric, not aesthetic: the submitted alpha range must leave `255..255` and move with the camera. `0` restores the opaque black wall bit-exactly. | |
| `RPCS3_REMIX_FPVCOLROUTE` | `1` (on) | **The vertex-colour route gate (round 11) -- why `VCOLMOD=1` rendered the HUD gauges black and the foliage invisible.** Rounds 9/10 replayed the mesh's ATTR3 into the submitted vertex colour, and pointed the blend extension's arg sources at `VertexColor0`, whenever the **fragment** program named COL0. COL0 is not an attribute: it is the **vertex** program's output register `o1`, and the ucode is free to pass ATTR3 through, scale it, compute it, or never write it. The offline sweep over all **82** cached `.vp` (`sweep11.py`, run before this shipped) says the two are the same thing for only 16 of them: **passthrough 16, scaled 22 (every one of them `c[18]`), scaled_chain 1, computed 36, none 7.** The giant flower `C97CD1531AC480D8` is the proof case -- its rgb *is* a scaled chain of ATTR3 (`I3.xyz * c[62] -> * c[464].y -> * c[66].x`) while its **alpha** is a computed distance ramp off `c[59]`/`c[466]` that never reaches `I3.w` at all. Its mesh ATTR3 alpha is `00`, the backend replayed that `00`, the fp modulates alpha by COL0.a, and the draw died: no consumption gate can repair a wrong *source*, which is why this supersedes `FPVCOLALPHAGATE` as the load-bearing protection (that gate stays -- it is still correct for its own blend-off/atest-off corner). The rule at all four consumption sites is one sentence: **replay what the hardware would see, else leave white.** `passthrough` replays as today; `scaled`/`scaled_chain` replay with the constants folded (`VCOLFOLD`); `computed`/`none` do not replay, which leaves the decode loop's `0xFFFFFFFF` -- the pre-round-9 look, never a new wrong colour. Every alpha half additionally requires the route to prove `ATTR3.w` reaches `COL0.w`. Classification is strict by design: a partial write mask, an indexed constant, a negated operand, an SCA write or more than three hops all fall to `computed`. Counter `vcol_route_blocked` (the partition term against `fpvcol_applied`); census `Remix vcolroute:` once per program; `route=`/`alpha_from_attr=` on `Remix effect:` and on `Remix pick-deep:`. `0` restores round 10's route-blind replay bit-exactly. | |
| `RPCS3_REMIX_VCOLFOLD` | `1` (on) | **The constant-fold half of the route gate, severable on its own.** For the `scaled` and `scaled_chain` routes, multiplies the decoded ATTR3 by the transform constants the ucode multiplies it by, read live per draw from `rsx::method_registers.transform_constants` -- the same access the UV scale machinery performs, and the one `Remix pick-deep:`'s `slotval=[...]` proves reads the value the draw actually used. `0` makes those routes replay the **raw** attribute (round 10's value) while still being route-gated, so “is the route right?” and “is the fold right?” are separately answerable from one session each. Counter `vcol_fold`. | |
| `RPCS3_REMIX_FPVCOLSKYGATE` | `1` (on) | **The sky may never go emissive (round 11) -- closing a latent regression round 10 shipped without knowing.** Round 10's self-lit verdict excluded “the flat dome variant” on colour variance and calibrated that exclusion on the **wrong** variant. Haze's 32-vertex dome is the flat one (`Remix sky-census: ... vtx=32 ... reject:extent`); the **82-vertex** one carries the horizon gradient, and its own census rows print `rgb-varies=1 selflit=1 additive=1` -- it passes every term of the verdict. It escaped becoming an emissive `ONE/ONE` additive shell only because it draws during the menu stretch, where `cam_fallback=37` and the world-transform gate refuses it (`fail=nocam`) **before** the attach ever runs. With `rtx.skyMode=0` the rasterized sky *is* the visible sky, so the first open-sky visit with a camera present would have turned it into a light the size of the level. Now excluded by **measurement**: a self-lit candidate must additionally have a raw extent below `sky_min_extent()` (2000). The dome spans 10000; no room-scale effect, the nectar pulse included, approaches it. The extent is measured once per draw and shared with the census, so a row can never print a number the verdict did not use. Counter `fpvcol_skygate` (draws excluded) -- climbing in open sky is the gate **working**, not a refusal to investigate. `0` is the regression repro; do not leave it off. | |
| `RPCS3_REMIX_UCODESTOREFP` | `1` (on) | **The fragment-program twin of `UCODESTORE` (round 11).** `bin\cache\...\shaders_cache\raw` is written only by GL/Vulkan pipeline compilation, so a program first met under the Remix backend has never been decodable offline -- which is why round 10 spent a whole planning pass guessing at the flat-yellow sun card from six census rows: its ucode `0DBB822C00810202` is not in that cache. Writes the raw fp ucode of every program classified `out_rgb_source == other` on an **untextured** draw to `bin\remix_ucode\<rawhash>.fp`, bounded by the same seen-set and size guard as its sibling. `<rawhash>` is the **unmodified** ucode hash, matching both cache filename conventions -- the logged `fp=` carries an extra `0x9e3779b97f4a7c15` term when the program exports 32-bit registers, and that term is removed here rather than in the reader (worked example: logged `fp=a20a7a99872b615a` is file `3C3D0320F8611D4F.fp`). Counter `ucode_fp=<stored>/<failed>`; census `Remix ucode-store-fp:`. | |
| `RPCS3_REMIX_FXREFPROBE` | `1` (on) | **A read-only instrument, no behaviour change (round 11) -- the Selva smoke and explosions, re-attributed.** They are **not** the emissive family: the effect census never grows a new untextured `vcol_pass` pair. What the round-10 sessions show all session long is a refused **textured** effects family on the main surface -- `vp=f39f504649b6f442` (18 refusal windows, siblings `af06f6d32ec048ee` 19, `15ad612980aca110` 18, `57a12323f22f4988` 11), 952..3649-vertex batches re-creating their meshes every frame, refused `fail=tail areason=wdivide persp_residue=3.7144 tol=0.02` with `ref=camera` / `ref=anchor` / `ref=anchor_prev` all `rescue=failed`. Its ucode decodes to the same atmosphere family as the backdrop card (`o1 = I3 * c[18]`, w-divided positions through fused `c[0..3]`, scrolled atlas UVs, a camera-relative scatter chain). A residue of 3.71 against a 0.02 tolerance is not noise -- it says the reference that divided the draw is not the projection the draw went through (the rows show the **aux** camera active at 512x288 while the draw is on the 1024x576 main surface). On the `rescue=failed` exit for fused programs, bounded to 8 lines per stats window, this divides the draw's recovered fused matrix against **every live gauge slot** and prints the best three beside the refused reference and its residue. **Pre-registered verdicts:** a best residue within tolerance against some *other* live reference means reference **selection** is the defect, and round 12 ships a bounded retry-against-alternates divide; a best residue far outside against everything means these draws carry **their own projection**, and the banked second-projection-reference design's entry ticket is met. No fix ships on this thread this round -- guessing before the numbers exist is the rounds-1-3 failure mode. Separately named and no longer a mystery: `641d6432efd6add4` (`vtx=4`, 512x288 aux surface, `persp_residue=0.999988` against **both** cameras) is the aux pass's screen-space processing quads; residue ~1 is the ortho-vs-perspective signature and they are correctly un-worldable. Census `Remix fxref:`. `0` silences it. | |
| `RPCS3_REMIX_FXREFVP` | *(empty)* | **Round 18 -- the fxref census above was SATURATED and its top line was never an effect.** MEASURED over the three most recent `title=BLUS30094` runs: the census emits **exactly** its cap in every stats window it speaks in (744 lines / 93 windows = **8.00** in the round-17 run), so the "320 `Remix fxref:` lines" read as a sample of the effects family are the first eight refusals of *one frame* per window, repeated. Two of those eight are not effects at all. `641d6432efd6add4`, which round 4 through round 18 carried as "the missiles and smoke", is a **six-instruction two-tap blur quad**: its whole ucode is `o7(TEX0).xy = v8.xy - c67.xy` / `o7(TEX0).zw = v8.xy + c67.xy` plus `o0(HPOS) = v0.x*c0 + v0.y*c1 + v0.z*c2 + v0.w*c3` -- a symmetric sample offset on a 4-vertex quad covering the 512x288 half-res buffer, i.e. a separable blur/downsample. Its `refused_residue` is **bit-identical `0.999988` in every frame of every run** because it equals `1/Q` of the title's projection and depends on nothing in the scene. It is a post-process pass, it is correctly refused, and giving it a camera would put a screen-covering quad into the world. The other is `af06f6d32ec048ee` drawing a 4-vertex sky backdrop quad on surface `01120000`. Up to 8 comma-separated vp hashes; **empty is the round-17 census exactly** (every fused refusal). | |
| `RPCS3_REMIX_FXREFMAX` | `8` | **Round 18.** Lines per 120-frame stats window for the `Remix fxref:` census; `8` was the hardcoded round-11 constant and is what saturated. Clamped to 64 in code so a mis-set value cannot make this census the dominant writer in the dump log. The same round also puts the fields the census was missing on its line: `vpz=[scale offset]` (the **draw's** viewport depth range), `bestz=[scale offset]` (the range the best anchor folded into its gauge), `zfold=` (whether the reference that actually divided this draw disagreed with it -- `gauge_zfold_mismatch` has been climbing at a dead-constant **924 per window** with nothing attributing it to a program), and `world=[...]`, **the recovered matrix itself**. Six rounds of reading one scalar residue and never the sixteen numbers behind it is how a blur quad was carried as the smoke for fourteen rounds. | |
| `RPCS3_REMIX_PROJSPLIT` | `0` (off) | **Round 18 -- THE SELVA SMOKE/EXPLOSIONS FIX, armed in the launcher, NOT play-tested.** Why this is indicated rather than guessed: the refusal residue `|m03|+|m13|+|m23|+|m33-1|` of `world = fused * reference_inverse` is structurally **blind to any difference in the view** -- for `reference = V_a*P` and `fused = W*V_d*P` the product is `W*V_d*P*P^-1*V_a^-1 = W*V_d*V_a^-1`, affine for any two rigid views, residue 0. **A non-zero residue therefore means one thing only: the draw's PROJECTION is not the reference's.** MEASURED for the effects family `vp=f39f504649b6f442`: residue **3.21949 as the first sampled refusal of all three runs**, median **3.409** (p10 3.386, p90 3.763) over 226 samples spanning three sessions at unrelated camera positions. A camera-lag or wrong-anchor defect cannot hold a residue that still while the player walks and turns; a second projection can. Round 8's own `split=1` ticket has been printing on every one of these refusals since it was added. The fix runs at the **tail-rescue `failed` exit only** -- the exit where the draw is dropped today -- and rebuilds the reference as `cross = split(anchor_fused).view * split(draw_fused).projection = V * P_d`, giving `world = fused * cross^-1 = W*V*P_d * P_d^-1*V^-1 = W`, then re-runs the **same** `is_affine` gate every other path passes. A split that won on the **transpose** is refused outright (there `view*projection` reconstructs `fused^T`, and mixing the two conventions is how a matrix bug becomes a geometry bug). **Blast radius: it cannot move anything that renders today**, because it only runs where the draw is already being dropped. Acceptance counter `proj_split=<applied>/<refused>/<nosplit>` on `Remix live:`; census `Remix tail-rescue: ... outcome=projsplit`. `0` restores the round-17 drop bit-exactly. **ROUND 19 CORRECTION, and the sentence removed here was actively misleading: "a wrong premise surfaces as `proj_split_refused` climbing rather than as garbage in the scene" is FALSE.** The `is_affine` gate on this construction cannot refuse -- see `RPCS3_REMIX_PROJSPLITVDELTA` below for the derivation -- so `refused=0` is structural and says nothing about whether the premise held. The blast-radius half of the claim still stands; the safety half does not. **What round 19 did confirm, measured:** the draws really are placed. `Remix tail-rescue: vp=f39f504649b6f442 ... outcome=projsplit residue=3.21904 -> 3.47437e-08`, 346 such lines in the round-18 run, second residue median `1.07e-06` over n=346, always against `anchor_vp=ad7ce9d672a0bf6b`; and the `Remix fxref:` census -- which runs at the *refused* return -- fell from **744 lines to 0**, because the family it was pointed at is no longer being refused. Round 18's central claim that the draw carries its own projection is **supported**. The smoke still not rendering is therefore a separate defect, not a failure of this path. | |
| `RPCS3_REMIX_PROJSPLITERR` | `50` (= 0.05) | **Round 18.** Largest split reconstruction L1 error either split may carry before the cross above is refused, scaled by 1000 exactly as `AFFINETOL` is. `split_view_projection` forces the view it synthesises to be orthonormal and reports `l1_error` for how well `view * projection` reconstructs the matrix it was given; a large error means the "view" is not the draw's actual view and the cross would be meaningless. Lower is stricter. | |
| `RPCS3_REMIX_PROJSPLITVDELTA` | `0` (no gate) | **Round 19 -- the gate `PROJSPLIT` was believed to have, and does not.** The round-18 entry two rows up claims "a wrong premise surfaces as `proj_split_refused` climbing rather than as garbage in the scene". **REFUTED, from the source.** `try_split_once` builds `view_to_world` from an **orthonormal** basis -- `right`/`up`/`forward` via two cross products, `RemixTransforms.cpp:6502-6524` -- so `vp_split::view` is **rigid by construction**, for the draw and for the anchor alike. The recovered world is therefore `fused * cross^-1 = (V_d * P_d) * P_d^-1 * V_a^-1 = V_d * V_a^-1`, a product of two rigid matrices: rigid, hence affine, hence **accepted always**. `is_affine` is vacuous on this construction and `proj_split_refused` is structurally pinned at `0` -- which is exactly what both measured runs report (`74552/0/0` and `170115/0/0`), and why `applied` came out **bit-identical to `tail_split_ok`** in both. That equality was never a mislabelled counter; it is the gate being unable to say no. **What the construction actually assumes is `V_draw == V_anchor`, and nothing tested it.** This does: the L1 distance between the two rigid views, printed as `vdelta=` on every `Remix tail-rescue: ... outcome=projsplit` line **whether or not the gate is armed**, so the distribution is readable before anything is refused on it. Near zero means the draw lands where the guest drew it; large means `proj_split` is placing it cleanly *somewhere else in the world*, which is invisible on every other counter this backend emits and is a live candidate for "the smoke is placed and still does not render". Scaled by 1000 like every other tolerance here. Subset counter `vdrefused=` on `Remix live:`; it increments `proj_split_refused` too, so the documented three-way partition of `tail_split_ok` stays exact. `0` = round 18 behaviour bit for bit. **ROUND 20 HAS THE DISTRIBUTION, and it says DO NOT ARM THIS.** MEASURED over the 85 `outcome=projsplit` census rows of the round-19 run: `vdelta` min **53.95**, p10 55.88, median **58.14**, p90 79.17, max **91.46** -- **not one row is near zero**, and `vdrefused=0` only because the gate is off. Round 19's play-test guidance ("near 0 = good; if large, arm at roughly the p90") would therefore refuse work that is demonstrably **correct**: on the same lines, the recovered `world=` translation for the first-person arms is `[-5.4674 1.9421 -42.251]` against `cam=[-5.3396 1.6886 -41.932]`, i.e. **0.427** units from the eye -- matching, to three decimals, the independent `anchor=0.427494` that `Remix viewmodel-census` measures for that same program by a different route. A large `vdelta` and a correct placement coexist on the same draw. The conclusion is about the **metric**, not the fix: a raw L1 over the whole 4x4 including the translation row (`RemixGSRender.cpp:14192-14198`) is dominated by a component that cancels in `V_d * V_a^-1`, so as scaled it is **not** a usable premise test. Arming it anywhere below 53.95 re-drops the entire family round 18 rescued; arming it at the p90 refuses ~10% of correct placements at random. If the premise is to be tested, the metric has to be rebuilt as `||V_d * V_a^-1 - I||` (the quantity that actually multiplies the world matrix), not `||V_d - V_a||`. **Leave at `0`.** | |
| `RPCS3_REMIX_VMBASIS` | `0` (off) | **Round 19 -- THE VIEWMODEL ORIENTATION FIX, armed at `6` in the launcher, NOT play-tested.** The arms and weapon reached the screen for the first time in round 18 and arrived **mirrored, upside down and displaced** up-and-to-the-right. Three symptoms, one operator: a sign error on a pair of axes. **The runtime is ruled out, from its source.** `createViewModelInstances` builds `perspectiveCorrection = mainViewToWorld * (mainProjectionToView * vmProjection * scale) * vmWorldToView` (`rtx_instance_manager.cpp:1547`, deployed numos3 build, sha256 `36a5641a...` verified against `bin\remix\d3d9.dll`), after overwriting `vmProjection`'s `[2][2]/[2][3]/[3][2]` from the main camera. This backend submits a VIEW_MODEL camera that is the world camera with only `projection[0][0]` and `[1][1]` replaced, so `mainProjectionToView * vmProjection` collapses to `diag(a'/a, b'/b, 1, 1)` and the whole correction is `V^-1 * S * V` -- an **exactly uniform 1.2584x widening about the eye**. Both ratios come out identical (`a'/a = b'/b = 1.258383` from the measured FOVs 60.001/36.132 against 72.000/44.634) because both pairs carry the same 16:9 aspect, so `b'/b` reduces algebraically to `a'/a`. Built numerically with a real perspective matrix and the depth-row overwrite applied: `det(3x3) = +1.583528`, singular values `(1.258383, 1.258383, 1.0)` -- determinant **positive**, no axis reversed. Neither the runtime nor the camera we hand it can produce the flip; the error is in the instance transform, which is what this rewrites. Applied at the **submit site**, the only point at which a draw is known to be both VIEW_MODEL-tagged and actually submitted. Bitmask: bit0 (`1`) negates the camera's **right** axis (mirrors left/right), bit1 (`2`) the **up** axis (upside down), bit2 (`4`) the **forward** axis (puts it behind the eye). The axes are the columns of the row-vector `worldToView` and are orthonormal for any rigid view, so `C = sum_k d_k a_k a_k^T` is an exact reflection/rotation: **a wrong value cannot rescale, stretch or shear the viewmodel, only reorient it**, which is what makes shipping a guess acceptable at all. `6` (up+forward = 180 degrees about the camera's right axis) is the launcher's first guess because it is the only single operator producing all three reported symptoms at once -- upside down, facing away, displaced up, same side of the screen. Retrying is one launcher edit and no rebuild. `0` = the round-18 orientation exactly. **ROUND 20, MEASURED FROM THE LOG, and it refutes the round-20 brief's premise that "`flip=6` is being recorded but is not visibly changing the transform".** Re-deriving the three `Remix vmbasis:` lines of the round-19 run: `max\|pre-post\| = 3.3997`, `L1\|pre-post\| = 9.62` -- not a fifth-decimal difference, a 180-degree rotation. `post` reproduces `C * pre` to within `1e-4` with `C` rebuilt from the logged `right`/`up`/`fwd` and `d = (+1,-1,-1)`, `det(C) = +1.000001`, and `eye_post` negates `up` and `fwd` exactly (`[0.000170 -0.00131 1.3937]` -> `[0.000171 +0.00131 -1.3937]`). **The flip reaches the submitted draw and works as designed.** It also clears the transform of the reported mirror: `det(pre) = +4.8963`, `det(post) = +4.8963`, column norms `(1.6998, 1.6998, 1.6947)` -- a positive, near-uniform 1.6995x scale with **no axis reversed**, before and after. Round 20 adds `det_pre=`/`det_post=` to the census so this stops being a re-derivation. **Why the user saw no change anyway:** the draws it acted on were not the viewmodel -- see `VMPAIRVP`. | |
| `RPCS3_REMIX_VMBASISMAX` | `24` (clamped to 96) | **Round 19, census only.** Cap for the new `Remix vmbasis:` census, deduped one line per program and gated on `DIAGLINES`. **This is the measurement nineteen rounds have been missing.** Nothing has ever printed the recovered world matrix of a VIEW_MODEL-tagged draw: the `Remix worldvp:` census covers six programs in the round-18 run and **none of them is the viewmodel**, and round 18's own `world=` field went onto the `Remix fxref:` line -- which stopped emitting **entirely** (744 lines in the round-17 run, **0** in the round-18 run) the moment `PROJSPLIT` started accepting the very draws it censused, taking the field with it. The line carries the transform **before and after** the flip, the camera's three world-space axes, the viewport depth range, and -- the field to actually read -- `eye_pre=`/`eye_post=`, the object origin resolved onto right/up/forward **in metres**. The sign pattern that changes between those two triples names the wrong axes directly instead of by eye. Tags nothing, refuses nothing, moves no counter. **ROUND 20 fixes a real defect in it and adds three fields.** The dedup key was the **vp hash alone**, so its three lines reported only the FIRST tagged draw of each program -- and the round-20 brief read those three lines as "the viewmodel program `830d7d1b9681c475` draws the opaque body at `vtx=1446`". MEASURED against the same run's `Remix fpcandidate:` census, that program draws at least **eleven** distinct albedos, and the `vtx=1446` one sits **1.684** units from the eye with a **3.499** extent while the actual first-person geometry is `86885A0E60751491` (`vtx=3649`, `ext=0.862`, `eye_dist=0.465`) and `0721D150DF278E7D` (`vtx=2140`, `ext=0.980`, `eye_dist=0.437`) -- both reported `vmlisted=0`, i.e. never selected. A vp-only key cannot show that. The key is now `vp ^ albedo`. New fields: `bypair=` (which route tagged it) and `det_pre=`/`det_post=`, the determinant of the submitted 3x3 in f64 -- **a mirror is a determinant sign change and nothing else**, so this is the field that either names the bug or clears the transform outright. | |
| `RPCS3_REMIX_VMDEPTHOFFSET` | `0` (use the built-in `1e-3`) | **Round 19 -- refutes a comment that has stood since round 5.** `RemixGSRender.cpp:5313` states "Haze reports the same `scale_z`/`offset_z` for every draw in the scene including the sky dome, so the depth rule below selects nothing at all and there is no threshold to retune". **MEASURED, and false.** The round-18 run's own `Remix viewmodel-census` -- **38 lines, one per program, the 64-line cap never reached, so this is the complete population** -- splits into two depth bands: 35 programs at `scale_z=0.49875 offset_z=0.50125`, and **three** at `scale_z=0.00125 offset_z=0.00125`. Those three are `830d7d1b9681c475` (the first-person arms, `vtx=3649`), `57a12323f22f4988` and `9f591b6a6b825612` (the visor), with measured eye distances of **0.427 / 0.425 / 0.558** world units against a limit of 4 -- they are on the player's face and nothing else in the scene is. A separate near depth slice exists and it selects **exactly** the viewmodel. The built-in threshold is `1e-3` and the viewmodel's offset is `1.25e-3`: the rule misses by **25%**. Setting this to `2` (= 0.002) lets the depth rule select them. Why that matters: the albedo route provably **cannot** separate them -- all four programs it currently tags share albedo `C61753D31FB96507`, and two of them are not the viewmodel at all (**the sky dome `af06f6d32ec048ee` and the effects family `f39f504649b6f442`**, both named on `Remix viewmodel-camera:` lines). A VIEW_MODEL-tagged instance is **removed from the world pass** by the runtime (`candidateInstance->m_vkInstance.mask = 0`, `rtx_instance_manager.cpp:1562`) and redrawn glued to the camera, so anything mis-tagged disappears from the world. Round 19 also widens the twin-fallback arm in `per_draw_transform` to cover depth-selected draws, because without that a draw the depth rule selects but no albedo list names would fall to the `REFUSED:noref` arm and be **dropped** -- arming this knob would have deleted the arms rather than placed them. **Trap, caught in review before shipping and worth knowing before touching any of this:** `classify_viewmodel_depth` returns `tagged` **immediately** for anything on `VIEWMODELVP` (`RemixGSRender.cpp:5318` -- the hash list short-circuits **ahead of both depth tests**), so its raw verdict is `tagged` at *every* threshold including `0`. The widened arm therefore takes the **depth-only half**, `viewmodel_depth_tagged && !m_scratch_vm_by_hash`; using the raw verdict would have silently kept hash-listed draws that round 18 dropped, on any title that arms `VIEWMODELVP`. `viewmodel_draw` itself keeps the original predicate and is bit-identical to round 18 either way. **Inertness proven arithmetically rather than asserted:** at the shipped `0` the limit is `1e-3` and **both** depth bands reject (`0.00125` and `0.50125`), so the widening covers nothing; at `2` the limit is `0.002` and the viewmodel band is `TAGGED` while all 35 world programs still reject -- the lever selects exactly the three programs and nothing else. Round 19 ships this **inert** deliberately: arming it in the same round as `VMBASIS` would change two things at once and neither result could be attributed. `0` = the built-in constant = round-18 behaviour bit for bit. **ROUND 20 CONFIRMS the band split on a second, independent run** (round-19 build, `title=BLUS30094`, `build=Aug 16 2026 06:35:10`): 35 censused programs, **33** at `0.49875/0.50125` and **two** at `0.00125/0.00125` -- `830d7d1b9681c475` (`vtx=3649`, anchor **0.427**) and `57a12323f22f4988` (`vtx=8`, anchor **0.425**); every one of the 35 reads `reject:offset` at the shipped `maxoffset=0.001`. `9f591b6a6b825612` did not draw in this shorter run. **This is now the strongest lever available on the viewmodel and it needs no rebuild.** | |
| `RPCS3_REMIX_VMPAIRVP` + `RPCS3_REMIX_VMPAIRALBEDO` | both empty (route off) | **Round 20 -- THE VIEWMODEL PAIR GATE, armed in the launcher, NOT play-tested.** A draw is tagged VIEW_MODEL only when its vertex program is on the first list **and** its albedo is on the second -- the same intersection shape `SUNCARDVP`+`SUNCARDALBEDO` uses and `SKIPPAIRVP`+`SKIPPAIRALBEDO` used before it. Bounds 4 programs and 8 albedos; **an empty either half disarms the whole route**, which is what makes it inert-by-default rather than inert-by-assertion. Implemented as a **third, independent route** ORed alongside the depth verdict and the albedo route (`m_scratch_vm_by_pair`, counter `vm_tagged_pair`, banner `vmpairvps=`/`vmpairalbedos=`, census field `bypair=`). **Why a new route and not "AND `VIEWMODELVP` instead of OR", which is what round 20's brief asked for: that framing is half wrong and the wrong half matters.** `viewmodel_albedo_matches` is **already** ANDed with the program list -- both call sites (`RemixGSRender.cpp:13601` and `:17436`) read `by_albedo = viewmodel_albedo_matches(a) && (viewmodel_vp_count() == 0 \|\| by_hash)` -- so with `VIEWMODELVP` non-empty the albedo route is *already* a pair gate. What is ORed is the **depth verdict**, and `classify_viewmodel_depth` returns `tagged` **immediately** for anything on `VIEWMODELVP` (`:5330`, ahead of both depth tests). Listing a program there therefore tags **every** draw of it whatever its albedo, and since `830d7d1b9681c475` is also `GUESTLIGHTVP` that drags the ceiling light fixtures into the viewmodel camera and undoes round 5's lighting. This route never touches `classify_viewmodel_depth`, so it cannot. Also added to the **twin-fallback arm** in `per_draw_transform`, for the identical reason round 19 added `viewmodel_by_depth`: the dedicated viewmodel reference is never latched on this title, so a pair-tagged draw would otherwise fall to `REFUSED:noref` -> `note_world_fail(10)` and be **dropped** -- arming the pair would have deleted the viewmodel instead of placing it. The anchor guard (`VIEWMODELANCHOR`, limit 4) still applies on top. `vm_tagged_pair` is last in the attribution chain so `tagged_hash + tagged_albedo + tagged_pair + hash_anchor_refused` still partitions the listed population. **Round 23 adds a fifth term:** a pair-only draw refused by `RPCS3_REMIX_VMPAIRMAXDIST` is counted in `vm_pair_far` and in **none** of those four (the `\|\|` short-circuits ahead of the anchor counter), so the partition is now `tagged_hash + tagged_albedo + tagged_pair + hash_anchor_refused + pair_far`. Either list empty = round-19 behaviour bit for bit. | |
| `RPCS3_REMIX_VMPAIRMAXDIST` | `0` (off) | **Round 23 -- the eye-distance ceiling that makes a SHARED first-person albedo safe to list.** World units, applied to the `VMPAIRVP`+`VMPAIRALBEDO` route **only** -- not the hash route, not the albedo route, not the depth route -- and `0` reproduces round 22 exactly. It is folded into the existing viewmodel verdict at both tagging sites (the divide site in `per_draw_transform` and the submit site in `submit_subdraw`, exactly one of which runs per draw), and only when the pair is the **sole** reason the draw is listed, so it can never take a hash- or albedo-listed draw off the viewmodel path. Counter `vm_pair_far`; a non-zero value is proof the bound fired. It cannot live inside `viewmodel_pair_matches()` -- that is a pure free function with no camera or geometry access. **Why it is needed, MEASURED from `Remix fpcandidate:` `eye_dist` on the shared character program `830d7d1b9681c475` (`listedvp=1`, so its rows are NOT truncated by `FPCENSUSMAXDIST`): `86885A0E60751491` vtx=3649, the first-person body, 0.465..0.624; `0721D150DF278E7D` vtx=2140/952, the weapon, 0.429..1.090; `EC3C2D7AC0AB2938` vtx=554, 20.708..38.764; `19177730D341388A` vtx=92, 20.871..37.257.** A 20x gap, so the threshold is not delicate -- anything in 2..20 separates the two populations, and `2` is what the launcher documents. This is the missing discriminator the pair lists never had: it is what allows the weapon albedo (shared with character meshes, and whose earlier unbounded listing deleted NPC bodies at close range) to be added back. | |
| `RPCS3_REMIX_UIFORCEVP` | empty | **Round 23 -- pin a HUD vertex program to the 2D compositor. This is the nectar/health gauge fix.** `<vp>[,<vp>...]`, bounded at 8. **ROOT CAUSE, MEASURED.** `is_screen_space_draw()` decides UI-vs-world from exactly **two live per-draw inputs** -- the outer constant block read out of RSX constant memory, and `depth_write_enabled()`. It never consults the vertex program, the albedo, the clip size or the viewport. So a HUD program that is not statically fingerprinted `screen_space` **flips between the compositor and world geometry from draw to draw**, which is precisely the reported "correct in a vehicle, wrong on foot" symptom. In the round-22 run the gauge program `2f64c2f8ffd6add1` is on **both** paths in one session: 277 `Remix uiwrap: ... route=2d` lines (composited) **and** `Remix sky-census: vp=2f64c2f8ffd6add1 reject:extent ... raw=[-0.02 -0.765 0]..[0.02 -0.725 0] origin=[-5.2743 -0.001 -40.476] cam=[-5.2743 0 -41.982]` plus `Remix fpcandidate: ... eye_dist=1.679 dw=0 blend=1` -- i.e. on the 3D path the HUD quad is placed as **world geometry 1.5 units in front of the eye**, with raw vertices already inside the NDC cube at z = 0. All three reported symptoms follow from that one placement: it clips through geometry because it *is* geometry at 1.5 units; its resolution changes because a world quad is rasterized through the camera's projection; and it is **uncoloured** because the world path only replays vertex colour when `VCOLMOD` is on (it is `0` on this title) while the 2D path reads the colour straight out of ATTR3 -- `Remix vcolroute:` confirms `2f64c2f8ffd6add1 route=passthrough` (ATTR3) against its twin `2f650a38ffe6add1 route=constant cval=[1 1 1 1]`. The `depth_write` clause is the guard: it is the one input that says "this draw wants to occlude", and every gauge draw measured reads `depth_write=0`. Applied at the **call site**, not inside `is_screen_space_draw()`, so the classifier itself is byte-identical and the override gets its own counter, `ui_forced` (the forced **subset** of `skip_screen_space`). **This is not the viewmodel tag that made the gauges vanish in an earlier round** -- that deleted them from the world; this routes them to the overlay the compositor already draws hundreds of thousands of UI draws through, and which already accepts this exact program. Empty = round 22 exactly. | |
| `RPCS3_REMIX_DIAGLINES` | `1` (on) | **Round 12's headline fix, and the reason round 11 was unverifiable: all four of round 11's new census emitters were structurally silent for an entire session.** `Remix vcolroute:`, `Remix hazefade:`, `Remix selflit-miss:` and `Remix fxref:` each opened their guard with `dump_enabled()`, which is `RPCS3_REMIX_DUMP` (`0` in this title's launcher; `env_flag` returns false for a literal `"0"`) **OR** the `Log Draw Diagnostics` config flag (off). So the counters climbed while the lines that explain them never printed. **The partition is exact, and it replicates across all three round-11 runs in the log:** each run emitted `0/0/0/0` of the four while the ungated `Remix effect:` printed 11, 10 and 16 and `Remix live:` printed 180, 174 and 185 -- against `hazefade` counters of 22365, 11220 and 6635 respectively. All **seven** `dump_enabled()`-gated censuses (`Remix vcolroute:`, `hazefade:`, `selflit-miss:`, `fxref:`, `vptex`, `fpdump`, `screen-census`) emitted **0** lines in every run; all **eleven** ungated ones emitted in every run. `report_effect_draw` -- six source lines above `report_vcol_route` at the *same* call site, and the reason the adjacency looked paradoxical -- has **no** `dump_enabled()` guard at all. `dump_enabled()` remains the right gate for the *unbounded* dumps it also guards (`Remix vp= slice:`, the per-texture and per-fragment-program dumps): those cost real frame time and are opened one window at a time. It is the wrong gate for a census capped at **64 lines per run** costing one hash-set probe per draw, which is exactly what these four are and exactly what the unconditional `Remix effect:` beside them already costs. All four functions are pure reporters, so this changes no decision. `0` restores round 11's silence. Echoed as `diaglines=` on both the startup banner and the `Remix live:` line -- **read that field first** if a future run is missing these lines. | |
| `RPCS3_REMIX_VCOLCONST` | `1` (on) | **The `constant` vertex-colour route (round 12) -- the second, independent mechanism sending the HUD gauges to a wrong colour.** Round 11's offline sweep found programs whose COL0 is a pure constant, `MOV o1.xyzw, c[K]`, with the mesh's ATTR3 never read; round 11 lumped that shape in with `computed` and refused it, so those draws render **white**. Ground truth for the gauges is **amber** (the user's Vulkan-renderer screenshot), and the amber is in `c[K]`. Adds a sixth `vcol_route` matched in `scan_vcol_route` between the `passthrough` and `scaled` tests, reusing the existing `vcol_scale_slot`/`vcol_scale_comp` arrays for `K` and the per-lane swizzle, under the same full-write-mask and `!index_const` guards. The replay is deliberately narrower than every other route in two ways. **RGB only:** the submitted alpha stays at the decode loop's opaque `255`, because a constant alpha of `0` would delete the draw and rounds 9/10 already proved what replaying an unmeasured alpha costs -- `vcol_alpha_from_attr` is `false` for this route by construction, so `vcol_route_alpha_replayable()` and every alpha consumer refuse it with no special case. **No ATTR3 map:** the branch runs *before* `map_attribute(3, ...)`, because these meshes need not carry an ATTR3 at all -- going through the attribute path is how the colour would have been silently dropped even with the route correctly classified. Under `RPCS3_REMIX_VCOLMOD=0` this can only fire on **untextured** draws, since that is the only arm that calls `apply_vertex_colour`, so `vcol_const=0` with `VCOLMOD` off is a statement about which programs drew untextured, not a failed fix. **Blast-radius sweep before shipping** (all 82 cached `.vp`, 71 unique, decoder sanity-checked against all five hand-decoded round-11 programs): exactly **7** programs move, `computed 25 -> 18` and `constant 0 -> 7`; `none`, `passthrough`, `scaled` and `scaled_chain` are **unchanged**, and the count with `alpha_from_attr=true` is unchanged at 38 because all seven movers were already `false`. All five known actors -- gauges `2F64C2F8FFD6ADD1`, sky dome `FC0FAC8AFCCEC49A`, backdrop `EDC10321BF7CEB8F`, effects family `F39F504649B6F442`, giant flower `C97CD1531AC480D8` -- **do not move**, which is structural rather than lucky: the new branch sits after the `passthrough` MOV test and fires only on a MOV from a CONSTANT, while the three scaled actors are MULs. There are **zero near-misses** (no program is rejected by the new rule's `neg`/`index_const`/partial-mask guards) and after the change **zero full-writemask programs remain in `computed`** -- the route absorbs 100% of that population and nothing else. **The movers are not what the lead assumed:** only `2F650A38FFE6ADD1` (6 slots, `0:MOV o7.xy <- I8.xyxx`, `1:MOV o1.xyzw <- c[18]`, a plain `c[0..3]` MVP quad-MAD -- and 93 `Remix uiwrap:` lines in the live log) is HUD-shaped. The other six all read `c[94]` and are unmistakably **world geometry**: 15--48 slots with `o11..o14` DP4 blocks against `c[97..112]`, an `ADD o9.xyz <- -r3, c[96]` view-vector term and `MUL o8.xy <- r0.xy, c[95].zw`. Five of the six are **byte-identical** in the o1-writing quad; the sixth (`56CC5A962EAD7ECD`) differs only in its co-issued scalar half (`SCA RCP` vs NOP) and is field-for-field identical in the VEC half. Counter `vcol_const`. **The classifier runs unconditionally and only the replay is knobbed**, so `Remix vcolroute:` prints `route=constant cval=[r g b a]` -- the live `c[K]` -- even with this set to `0`; the colour is therefore readable from a session that has not adopted the change. `0` restores round 11's rendering bit-exactly: the route is not replayable, the draw takes the same refusal `computed` took, and it goes back to white. | |

## 2D UI compositor

| name | default | effect | in GUI |
| --- | --- | --- | --- |
| `RPCS3_REMIX_UICLAMPSUBRECT` | `1` (on) | **The guest-UI seam fix (round 7).** In the CPU compositor's sampler, under **REPEAT** addressing only, a coordinate outside `[0,1]` on an axis whose *authored UV span is at most one texture* clamps to the sheet edge instead of wrapping to the opposite edge of the atlas. Why it matters: `address_coordinate` implements repeat as `coordinate -= floor(coordinate)` and the fetch is **nearest**, so a UV a hair below 0 does not land on the glyph's own gutter -- it lands on the far side of the sheet and returns a *different glyph's ink at 100% weight*. Real RSX bilinear blends such a seam over about one texel; this sampler has no such bound. Measured on the project's own census: `vp=6f76ab0ad8d926b1` draws a 13-quad glyph batch with `u=[-0.018555..0.99902]` on a repeat-bound atlas -- ~9.5 texels on a 512-wide sheet, an order of magnitude past float rounding, i.e. authored padding. The discriminator is authored **span**, not magnitude: a sub-rect draw cannot be intending to tile, a span>1 draw genuinely tiles and keeps true repeat. Per-axis and per-triangle, so the two regressions of the old combined `force_clamp` flag (both axes clamped when either did; mirror collapsed to repeat) are impossible by construction. In-range sampling, mirror, clip, clamp and the `force_clamp` native-overlay path are **bit-identical** to round 6. Counters: `ui_uv=in/wrap/seam/mirror/clip/clamp` on `Remix live:`; census `Remix uiwrap:` names every (vp, atlas) with its real wrap modes and its excursion in texels. `0` restores round 6's sampling bit-exactly -- that A/B is the whole attribution. | |
| `RPCS3_REMIX_UIRECTSHRINK` | empty | **Round 26 -- the HUD font fix, now COMPLETE.** `<albedo>[,<albedo>...]`, bounded at 8, of texture **content hashes**. For a listed texture, the primitive's sampled UV rectangle **and its screen rectangle** are both pulled toward their own centres by `UIRECTSHRINKPCT`. **ROUND 25 SHIPPED THE UV HALF ALONE AND THAT WAS WRONG** -- the user's verdict was "more legible but bigger and squished", which is exactly what the UV half alone predicts: magnifying the middle quarter of the UV onto an unchanged screen quad draws the right glyph at `1/k` times its authored size, and Haze's pen advance is 1x, so neighbours then overlap by ~44%. **PROVEN by replaying the `UIDUMP` capture against the dumped atlas**, not argued: the per-vertex `Remix ui-quads[..]` lines give 4 vertices per glyph and 92 vertices for the string `11:17 Hours, 18th June 2048` (exactly 23 non-space glyphs); sampling the atlas through the **authored** rectangles reproduces the round-24 screenshot, through the **UV-only-shrunk** rectangles reproduces the round-25 screenshot, and through **both shrunk** renders clean, correctly spaced text. The px-per-texel ratio is *identical* between "authored" and "both shrunk" -- `k` cancels -- so the fix leaves every glyph exactly the size it already was on screen and removes only the padding. That invariance is why it is the correct correction. **AND IT CANNOT BE FIXED FURTHER UPSTREAM, which is a proof rather than a failure to look:** the expansion is about *each quad's own centre*, and the ratio (primitive half-extent)/(spacing between adjacent primitive centres) is invariant under any affine map. Every step between the vertex attribute and the compositor -- the recovered vertex-program matrix, `build_prescale`, the NDC/pixel conversion, the viewport terms, `uv_scale` -- is affine and is applied identically to every vertex of the draw, so no global term anywhere in this backend can change that ratio. Whatever doubles the rectangle is per-vertex, in the decoded attribute data itself. **ROOT CAUSE, MEASURED AGAINST THE USER'S OWN PIXELS.** Haze's HUD glyph quads carry a UV rectangle that is exactly **2x the glyph cell on both axes, expanded about the rectangle's own centre**: cropping the dumped atlas `unit0_6575ACE3A42A78E6_512x512.bmp` at exactly the rectangle a `Remix uiwrap:` row logs (`u=[444.0..476.0] v=[64.5..110.5]` texels) reproduces the photographed garbled ammo counter pixel for pixel -- the bottom of one glyph row, the wanted digits, the top of the row below. So the sampler, the wrap mode and the seam rule are all innocent and the fault is entirely in the authored/decoded rectangle. The atlas has **no gutter** (rows 29..176 are one continuous ink band; 6 blank columns in 512), which is why an oversized rectangle shows neighbours at full strength. **The 0.5 factor is measured, not fitted:** scaling four independent single-glyph rectangles about their own centre and scoring mean ink on the resulting one-texel border gives **exactly 0.0 at 0.50 on four of four** and non-zero at every factor from 0.35 to 1.00 and at every other anchor (low corner, high corner, texture origin). Applied **per triangle** -- a string draw's box is the union of many cells, but each glyph quad's triangles carry that quad's box alone -- and guarded to authored spans of at most 0.5 of the sheet on both axes, so a listed full-sheet blit can never collapse onto its middle quarter (the largest **authored** single-glyph span measured is 70/512 = 0.137 in u and 46/512 = 0.09 in v, so the guard sits **2.7x** above the population it admits). Both rasterizers read the same rule so the triangle and quad paths cannot drift. **2D compositor route only**; the world route is untouched by design. **The debt:** this corrects the rectangle at the sampler instead of fixing whatever doubles it. A centre-preserving doubling is the signature of a per-vertex offset-from-centre term scaled twice, which points at the MAD/affine UV recovery -- close it with `UIDUMPVP`, which prints per-vertex `(x,y,u,v)` and the `uvscale` actually used. Ships **empty**: zero behaviour change until a hash is listed. Observable: `uirectshrink=` on the run-start banner and `Remix live:`, and `shrink=` on every `Remix uiwrap:` row. **RISK (1) IS CLOSED IN ROUND 26, twice over.** It was: "each glyph quad's two triangles carry that quad's box" holds for quads / triangles / indexed lists but **not for a triangle strip**, whose connecting triangle straddles two cells at a span of ~0.18 -- *under* the guard -- and would be **moved** rather than resized. Closed (i) by measurement, the `UIDUMP` capture shows 4 vertices per glyph and 23 quads for a 23-glyph string, so these are not strips; and (ii) by construction, `is_axis_aligned_half` now refuses any primitive that is not one half of an axis-aligned quad, which is exactly the shape a strip's connecting triangle is not. **RISK (2) STANDS:** keyed on **albedo alone**, and three vertex programs share this atlas (`5bb8451bcd6f1feb` the HUD counters, `6f76ab0ad8d926b1` and `3f73fa83fa68911f` the multi-glyph strings), so if one authors its rectangle correctly its text now renders at half size. Measured against that: the smallest v extent any of the three ever authors is 46 texels against an atlas row pitch of ~23.5, so all three are doubled. Round 26 retargets `UIDUMPVP` to `6F76AB0AD8D926B1` to settle it per-quad. **Observables:** `uirectshrink=` on the run-start banner and `Remix live:`, `shrink=` on every `Remix uiwrap:` row, and **new in round 26** `ui_rect=<shrunk>/<declined>` on `Remix live:` -- `shrunk` must climb while HUD text is on screen (0 means the rule is not reaching the glyphs) and `declined` counts primitives that matched the hash and the span guard but were refused by the axis-aligned test rather than torn. Also note `ui_uv`'s **`seam` bucket will FALL** for a listed atlas, because the shrunk box lies strictly inside the authored one -- the `CLAMPALBEDO`-era reading "seam climbing means the fix is firing" is inverted here. | |
| `RPCS3_REMIX_UIRECTSHRINKPCT` | `50` | Percent for `UIRECTSHRINK`, a value that **parses** is clamped into `10..100`, so `200` becomes `100` and is a true no-op; only unparseable input falls back to `50`. **`100` is a true no-op even with a hash listed** -- it short-circuits *before any arithmetic*, which is load-bearing rather than cosmetic: `c + (x-c)*1.0f` differs from `x` for **35%** of random inputs in IEEE-754 single, so a k=1 round-trip would not have been bit-identical. It restores today's sampling bit-exactly and is the A/B that attributes any change. `50` is the measured value. If the glyphs come out slightly too large or too small this is the dial, but a best value other than 50 would mean the model is wrong, not the number. | |
| `RPCS3_REMIX_NOUI` | off | Disable the compositor entirely. | yes |
| `RPCS3_REMIX_KEEP_UI` | off | Disable the screen-space skip (bisection). | |
| `RPCS3_REMIX_KEEPRT` | off | Composite the title's own render-target blits (its post-process chain) through the CPU rasterizer. Off by default: those draws are not UI, they cost a full-screen fill each, and they are what held one test title at 1.9 FPS. | |
| `RPCS3_REMIX_UIWIDTH` | `1920` | Ceiling on the compositor buffer's width in pixels; the height follows the window's aspect. `0` removes the cap. Everything the rasterizer draws is authored at the guest's own surface resolution or at a virtual 1280x720, so more pixels buy no detail. | yes |
| `RPCS3_REMIX_UIPROBE` | off | Draw a fixed known pattern through `DrawScreenOverlay` instead of judging the call for the first time with real UI data flowing through it. | |
| `RPCS3_REMIX_UISPACE` | `1` (on) | Derive the guest window-space y convention from the viewport registers the guest programmed (RSX window space is y-down exactly when `viewport_scale_y` is negative). `0` restores the clip-pixel row conversion that hard-coded "guest pixel y=0 is the top row". Not a flip: substituting a y-down title's own registers reproduces the old expression exactly. | |
| `RPCS3_REMIX_UIDUMP` | `0` (off) | Log the geometry of the first N textured UI draws: screen bbox and the raw `(x,y,u,v)` of the first triangle. A glyph batch whose per-quad UVs span the whole 0..1 atlas instead of one glyph cell is the overlapping-text signature. | |
| `RPCS3_REMIX_UIDUMPVP` | `0` (unset) | 16-hex vertex-program hash. Narrows the UI dump to one program, logs every quad rather than the first triangle, and writes that draw's albedo texture out as a BMP once. **Round 26: this is the instrument that closed the HUD font.** Its `Remix ui-quads[n]: vp=... verts=N [i](x,y,u,v)...` lines carry the RAW authored attribute (`uvq * uv_scale`) and the computed screen position per vertex, so a capture can be replayed offline against `bin\remix_atlas.bmp` and scored against a screenshot. Note the lines land in `bin\log\RPCS3.log`, **not** `remix_dump.log`, and `UIDUMPVP` alone does nothing -- the limit comes from `UIDUMP`. | |

## Diagnostics

| name | default | effect | in GUI |
| --- | --- | --- | --- |
| `RPCS3_REMIX_DUMP` | off | One log line per unique vertex program, plus the rest of the draw census. A debug mode with a heavy per-frame cost. | yes |
| `RPCS3_REMIX_TEXBMP` | `1` (on) | Alongside the `Remix tex=` census line, write each unique decoded texture out as a BMP under `remix_tex\`. Only reached when `DUMP` is already on; `0` turns it off without turning the rest of the dump off. | |
| `RPCS3_REMIX_SKIPVP` | `0` (unset) | 16-hex vertex-program hash. Drop every draw of that one program -- the one-run bisector for "which program draws that". | |
| `RPCS3_REMIX_WORLDVP` | `0` (unset) | 16-hex vertex-program hash. Print the world transform that one program is being given, once per stats window, to `RPCS3.log` and `remix_dump.log`: which branch of `per_draw_transform` built it, and the L1 residue of the perspective row `to_remix_transform` is about to truncate away. The companion to `SKIPVP`. | |
| `RPCS3_REMIX_WATCHALBEDO` | empty | `<hex>[,<hex>...]`, up to 8 albedo hashes. Log every submit/skip decision for draws binding one of them -- `Remix watch: albedo=... verdict=submitted\|world_refused\|<gate> fail=... vp=... clip=... vtx=... extent=... dist=...` -- one line per verdict class (submitted / skipped) per frame per albedo. **Round 5 extends it to the two world-refusal sites**, with `fail=` naming which of `per_draw_transform`'s fourteen exits took the draw. That is the only instrument that can name geometry refused *before* submission, which never reaches the pick table at all -- Haze's invisible closed door being the case: it produces no pick line, while the same door kicked open picks cleanly and hands over its albedo. Zero behaviour change. The instrument for a symptom that only appears at a distance the cursor cannot reach: take `albedo=` from a close-up pick, arm it, walk backwards, and the log names the gate at the moment the object disappears -- or shows the albedo simply ceasing to be drawn, which is the title's own LOD swap and moves the question to whatever the far draw is refused for. Companion census: `Remix skip-census:`, one line per (gate, vp, fp, albedo) for every rule that drops a draw after submission began. | |
| `RPCS3_REMIX_FPCENSUSVP` | empty | **The player-weapon census (round 13), census only.** Comma-separated 16-hex vertex-program hashes, bounded at 4. Emits one `Remix fpcandidate:` line per `(vp, albedo)` the listed program draws -- albedo, vertex count, submitted extent, and the **geometry-to-eye distance** (`geometry_centre_in`, the same measurement `VMANCHORGEO` made load-bearing) -- capped at 48 lines and gated on `DIAGLINES`. Tags nothing and refuses nothing. Exists because `vp=830d7d1b9681c475` draws both the player's first-person weapon **and** whole NPC soldiers, so extent cannot separate them and adding a weapon-sized albedo to `VIEWMODELALBEDO` on extent alone would stick an NPC's rifle to the player's face. The player's weapon is the albedo whose `eye_dist` stays small across the whole run. | |
| `RPCS3_REMIX_FPCENSUSMAXDIST` | `0` (off) | **Round 17, census only.** Why the census above never named the weapon, measured: **all 242 `Remix fpcandidate:` lines in the log carry `vp=830d7d1b9681c475`**, the single hash on `FPCENSUSVP`, because the list test returns before anything is measured. The first-person rig is not one program -- the 1446-vertex head-locked mesh is drawn by `830d7d1b9681c475`, `af06f6d32ec048ee` **and** `f39f504649b6f442`, and `f39f504649b6f442` also draws a 952-vertex mesh whose instance origin sits 1.8194--1.8256 units **below the eye in three different runs at three different world positions**, which the census has never measured. Second defect: `m_fp_candidate_census_seen` is never cleared, so a `(vp, albedo)` pair is sampled **once per process** and "stays near the eye all run" was never answerable from it. Non-zero fixes both: an **unlisted** program may also emit provided its geometry centre is measured and within this many world units of the eye, and the census re-arms once per stats window so pairs report repeatedly. `FPCENSUSVP`-listed programs are exempt from the ceiling so the tagged visor rows that anchor every comparison always print; an unmeasurable draw is never admitted. New line fields `listedvp=` and `maxdist=`. Tags nothing, refuses nothing, moves no counter. `0` is round 13's census bit-exactly. | |

| `RPCS3_REMIX_ALPHACENSUS` | `0` (off, clamped to 256) | **Round 21 -- the per-albedo alpha-state census, and it exists because two whole rounds of theory about transparency had nothing to read.** Emits one `Remix alphastate:` line per **distinct albedo hash**, printing the `remixapi_InstanceInfoBlendEXT` this backend actually shipped -- blend enable, all six factors, the write mask, the alpha-test op and reference, both fixed-function texture stages, `tFactor`, the texture's own measured alpha range, the vertex-alpha rescue flag and the instance category flags -- next to the verdict `calculateAlphaState` will derive from exactly that input. The two derived fields are the point: `rt_fullyopaque=1` means the runtime forces `opacity = 1.0` (`opaque_surface_material_blending.slangh:39`), which is *the* mechanism that turns a texture with a soft alpha into a **hard-edged opaque quad**; `rt_blendingdisabled=1` means opacity is binarised to 0 or 1 (`:90-95`), which turns a soft falloff into a hard-edged *shape*. Both are recomputed here from `rtx_instance_manager.cpp:845-846` rather than guessed, and the blend arm reuses the existing `classify_blend` table so this line and the packed `blend_pairs={...}` list can never disagree. **Why it was needed: the two leading explanations were refuted and nothing replaced them.** MEASURED, from the deployed round-20 build's own `Remix stats:` line -- which `log_stats()` writes to `bin\log\RPCS3.log`, **not** to `remix_dump.log`, which is why no round had ever quoted it: `blend_chained=1183539 blend_translucent=405800 blend_unmapped=0 blend_rtopaque=0`, `blend_pairs={6/7/0=322419 1/7/0=71486 6/1/0=7951 1/1/0=3944}`. Zero unmapped pairs and zero runtime-dropped pairs over 1.18M chained draws: the blend translation loses nothing and the runtime overrules nothing, so "the blend state translation" is dead as an explanation for both the sun card and the missing effects. Also on that line, `blend_astranded=184268` -- blended draws whose texture alpha is a **constant** and whose vertex alpha could not be recovered, i.e. draws whose blending cannot do anything as submitted. Tags nothing, refuses nothing, moves no counter; costs one hash-set insert per new albedo and nothing thereafter. Echoed as `alphacensus=` on both banners. `0` = no output and no work. | |
| `RPCS3_REMIX_WORLDIDCENSUS` | `0` (off, clamped to 12) | **Round 22 -- how much geometry the `WORLDIDENTITYVP` family is deleting, per program, countable.** Emits `Remix worldid-census:` once per stats window with one slot per program that has reached the identity override, **run-cumulative**, so the last line of a run is the run total. Each slot prints `draws=`, `kept=`, the discarded translation bucketed `<=1 / <=32 / <=128 / >128`, the discarded 3x3 deviation bucketed `<=0.02 / <=0.1 / >0.1`, and `tmax=`/`bmax=`. **`Remix worldid-draw:` cannot answer this and never could:** its dedup key hashes the translation and basis *buckets* together with albedo, surface, clip and vertex count, so its lines count **tuples**, not draws, and cannot be summed. **Why it exists.** MEASURED from the round-21 build (pid 22856, frames 9390-30480, 51,232 flips), `Remix worldid-draw:` tuple rows by discarded translation: `BD1C10DF5703E559` 406 rows **100 % at `<=1`, tmax exactly 0**; `7F02E76D7369D09E` 382 rows, same; `AF06F6D32EC048EE` (via the `PAIRVP`+`PAIRALBEDO` rule) 9 rows, same. Against that: `C1D482DCD1B03ED0` 1362 rows with **1323 above 128 units (98.8 % displaced)**, `D0B6A471BB2D463B` 26 rows **100 % above 128**, `AD7CE9D672A0BF6B` 6682 rows with 3911 above 32 (58.5 %), tmax **2123**, and `0214281B9A7A412D` with a discarded `basis_delta` of **1.98** on 214 rows -- a real rotation, not a residue. The first three programs submit absolute world vertices, so forcing the identity is genuinely free; the rest do not. `AD7CE9D672A0BF6B`'s raw vertex boxes are a **fixed** 220x54x81 volume centred on its own origin (x `[-122.8, 101.0]`, y `[-19.1, 35.0]`, z `[-45.6, 35.5]`) for the whole run, while `BD1C10DF5703E559`'s span the map (x `[-1397.7, 1146.3]`) -- local coordinates versus world coordinates, which is exactly the distinction the override site's own comment says a VP-wide rule cannot make. Costs one linear scan of at most 12 fixed slots per identity-forced draw, no allocation, and counts every draw whether or not the escape hatch below is armed. `0` = no output. | |
| `RPCS3_REMIX_WORLDIDMAXT` | `0` (off) | **Round 22 -- the escape hatch for the above, deliberately left unarmed.** Whole world units. When non-zero, an identity-forced draw whose discarded translation exceeds this **keeps the transform `per_draw_transform` already resolved for it** instead of being pinned to the world origin; `kept=` on `Remix worldid-census:` counts them. `0` reproduces round 21 byte for byte. Three deliberate properties: it does **not** touch `world_identity_match`, so the refusal gate at the end of `per_draw_transform` still treats the draw as identity-covered and cannot drop it; it requires `world_resolved`, so a draw with nothing better to fall back on still gets the identity; and the census counts the population either way, so an unarmed run still reports the full damage. **The measured separation is wide, so the threshold is not delicate:** `AD7CE9D672A0BF6B` puts 2644 tuple rows at `<=1`, 127 in `(1, 32]`, and 3911 above 32. `32` is the natural first value. This is the round-23 experiment, not a shipped fix -- read the census first. | |
| `RPCS3_REMIX_WORLDIDMAXB` | `0` (off) | **Round 22 -- the same test on the basis instead of the translation.** Thousandths of 3x3 deviation, the same idiom as `RPCS3_REMIX_AFFINETOL`. Separate from `WORLDIDMAXT` because `0214281B9A7A412D` discards a `basis_delta` of **1.98** on 214 tuple rows while most of its translations are small, so a translation-only test would miss it entirely. Either test firing keeps the resolved transform. `0` = off. | |
| `RPCS3_REMIX_WORLDIDMAXTEXEMPTVP` | empty | **Round 24 -- the correction to `WORLDIDMAXT`, and the only knob this round that changes what you see.** `<vp>[,<vp>...]`, bounded at 8. A listed program is **never** granted the `WORLDIDMAXT`/`WORLDIDMAXB` escape hatch: it is always pinned to the identity, i.e. round-21 behaviour for that program alone, while the hatch stays armed for every other program. Empty = the hatch applies exactly as it did in round 23, byte for byte. **WHY, MEASURED.** The hatch reads a large discarded translation as "this is a local-space model whose placement we are about to delete", which is right for `AD7CE9D672A0BF6B` / `C1D482DCD1B03ED0` / `D0B6A471BB2D463B` and **inverted** for a program that submits absolute world vertices -- there the resolved translation is not a placement at all, and applying it moves geometry that was already correct. This title has exactly two such programs and the round-24 run measures the hatch displacing both: `BD1C10DF5703E559` `draws=24760 kept=5584 tmax=847.5`, and `7F02E76D7369D09E` `draws=131397 kept=38 tmax=293.3`. The clincher is a single `Remix worldid-draw:` row -- `vp=bd1c10df5703e559 translation=417.553 raw=[-1397.69 -40.03 -271.72]..[1146.32 -4.57 261.50]`: the raw vertices already span 2,544 world units, so those 417 units are added on top of a correct position. 92 of 140 traced `bd1c` rows sit above the `WORLDIDMAXT=32` threshold and are therefore kept and displaced. Both programs were measured at **0.0 % displaced** in round 22 ("absolute coords, identity is free"), which is precisely why they are the two that must be exempt rather than the two that benefit. Read `worldidexempt=` on `Remix stats:` for the parsed list size, and `kept=` per program on `Remix worldid-census:` -- an armed exemption drives the listed programs' `kept` to **0** while leaving the others untouched. | |

## Round 27 -- particle billboard replay, the sun sprite, and the viewmodel triple

| name | default | effect | in GUI |
| --- | --- | --- | --- |
| `RPCS3_REMIX_PARTICLEBILLBOARDVP` | empty | **Round 27 -- ITEM 1, the smoke, missile trails and explosions, open since round 4.** `<vp>[,<vp>...]`, bounded at 8. Family **A**, the camera-facing rotating sprite. **What was wrong:** six vertex programs on this title build their quad *inside the vertex program*, so the 4x4 into HPOS takes a computed temp rather than an attribute and the matcher refuses all six with `arch=unknown / note=no matrix chain into HPOS`. That refusal is `fail=lay_other`, and it was **209,904 dropped draws** in the round-26 run -- the entire particle system, never reaching Remix. **What ships:** the backend replays the expansion on the CPU, from algebra read out of the stored ucode and cross-checked against a numeric RSX vertex-program emulator (agreement to **3.6e-15**). Per listed program, per quad: `corner = v8.xy * c467.z - c467.w`; `A0 = (c8.x, c9.x, c10.x)`, `A1 = (c8.y, c9.y, c10.y)` (the view basis' rows, i.e. the camera's world-space axes); `A = A0*cos(ATTR0.w) + A1*sin(ATTR0.w)`, **deliberately left unnormalised** because the ucode does not normalise it and doing so would silently correct a scale the hardware keeps; `V = normalize(ATTR0.xyz - c26.xyz)` with `c26` the eye; `B = normalize(cross(A, V))`; a near-size fade `k = saturate((|ATTR0.xyz - c26| / (saturate(min(v8.z,v8.w)) + c467.y) - c85.z) * c85.w)`; and finally `world = ATTR0.xyz + A*(v8.z*k*corner.x) + B*(v8.w*k*corner.y)`. `v8.z`/`v8.w` are **half**-extents -- there is no `0.5` anywhere in the ucode and none is introduced. `4ECE0A28F80FFC72` alone adds a velocity stretch from `v12` (`A' = lerp(A, normalize(v12.xyz), v12.w)`, `stretch = lerp(1, saturate(abs(v12)), v12.w)`, applied to the **A axis only**), which degenerates exactly to the plain billboard at `v12.w == 0`, so reading it for the whole family costs nothing and misses nothing. **THE STRUCTURAL FACT THAT MADE THIS SAFE, and it corrects round 26's framing:** these are *not* one-vertex point sprites. The guest already submits **four real vertices per quad** and the shader only displaces them -- measured over **6,148** `Remix world-refused:` census lines covering all six programs, every `vtx=` divisible by 4, **zero exceptions** -- corroborated by `vtx=4 idx=6` / `vtx=8 idx=12`, and by no program writing `o6` (PSIZ), without which RSX hardware point-sprite expansion cannot occur. So the replay rewrites positions **in place** and never changes a vertex count or an index buffer. The 4-vertex grouping is re-verified per draw (all four corners must carry an identical ATTR0 within 1e-3 relative) and a draw that fails is **refused**, not rewritten. Submitted at `transform = identity`, because in both families the matrix following the expansion is the fused view*projection in `c0..c3` -- there is no object-to-world matrix in the chain to recover. **A DEDICATED ROUTE, NOT `WORLDIDENTITYVP`:** that list also selects the gauge anchor for the entire world, so listing these programs there would feed the particle pass's own view*projection into the reference that places every other draw in the scene. Round 26 caught that trap before shipping it. Counters `particle=<draws>/<quads>` and `particle_ref=<attr>/<consts>/<group>/<basis>` on `Remix live:`; census `Remix particle:`. Empty = byte-for-byte round 26. | |
| `RPCS3_REMIX_PARTICLERIBBONVP` | empty | **Round 27 -- ITEM 1, family B, the trail ribbon.** `<vp>[,<vp>...]`, bounded at 8. Genuinely different geometry from family A: two endpoints, no roll, no size fade, and it never touches `c8/c9/c10`. `D = v12.xyz - ATTR0.xyz` (tail minus head); `V = normalize(ATTR0.xyz - c26.xyz)`; `B = normalize(cross(normalize(D), V))`; `side = v8.y * c467.y - c467.z` -- **only Y is remapped**, `v8.x` is an along-trail lerp in `[0,1]`, not a corner; `world = ATTR0.xyz + D*v8.x + B*(side * v8.z)`. Note `v8.z` is a half-width but `v8.w` is a **UV tiling scalar, not a size** -- one of round 26's readings that this round refuted. `315E21388632FE3F`'s ATTR0 is declared `size=3`, so its `w` is the RSX default `1.0` and the `MAD v0.w, c3` is just the generic encoding of `+c3`; round 26's "ATTR0.w = homogeneous w" is refuted for it. A separate knob from family A on purpose: one can be right while the other is wrong, and each has its own revert. | |
| `RPCS3_REMIX_PARTICLEFLIPBOOKVP` | empty | **Round 27 -- the animation grid.** `<vp>[,<vp>...]`, bounded at 8. Two of the six programs (`946A6296D06C4AF8` in family A, `391B10C33305C812` in family B) index an animation grid *inside* the atlas sub-rect rather than using the rect directly: `gx = v9.x`, `gy = v9.y`, `phase = v9.z`, `f = (gx*gy - 1)*phase`, `f0 = floor(f)`, `row = floor(f0/gx)`, `uv = (base.u/gx + (f0/gx - row), base.v/gy + row/gy)`. **It needs its own list rather than detection** because the other four programs also feed ATTR9 -- they read only `.w`, as an opaque passthrough to the fragment program -- so "ATTR9 exists" is not the test and applying the grid to them would move every particle's UV onto the wrong cell. Only the near cell is replayed; the ucode also computes `f0+1` and a cross-frame blend factor into `o8`, which a single texcoord set cannot carry. If the grid cannot be read or is not a sane integer pair, the plain sub-rect is used -- a wrong cell is a far smaller defect than a NaN UV. Empty = every listed program uses the plain sub-rect, which is correct for four of the six. | |
| `RPCS3_REMIX_PARTICLEUV` | `1` (on) | **Round 27.** Replay the atlas sub-rect UV as well as the position: `o[TEX0].xy = v8.xy * (v10.zy - v10.xw) + v10.xw` for family A, and `((v10.z-v10.x)*(v8.w*(c467.z - v8.x)) + v10.x, (v10.y-v10.w)*v8.y + v10.w)` for family B. This is **not optional decoration**: the scale *and* the bias are per-vertex and come from a second attribute, a form `UVAFFINEALL`/`UVSCALELANES` can never resolve, so without it these draws fall back to the fixed `1/4096` divisor and every particle samples texel 0. Inert when both program lists are empty. `0` is a **diagnostic**, not a fix -- it isolates "the particles are in the wrong place" from "the particles are in the right place but textured wrong". Counter `particle_uv`. | |
| `RPCS3_REMIX_PARTICLEFLIPPHASE` | `1` (on) | **Round 28 -- the flipbook's animation PHASE is per PARTICLE, and round 27 read it per DRAW.** The animation grid `PARTICLEFLIPBOOKVP` indexes carries three values in ATTR9: `gx = v9.x`, `gy = v9.y`, `phase = v9.z`. The first two genuinely are per-effect constants. The third is that particle's own age, and round 27 read all three once, from **vertex 0 of the draw**, on the stated assumption that ATTR9 is "a per-effect constant fed identically to every vertex". These draws batch heavily -- censused vertex counts run to **1,620, i.e. 405 quads in a single draw** -- and a batch of particles is by construction a batch of different ages, so that pinned every particle of an explosion to particle 0's cell. If that cell is a spent frame of the sheet, the whole batch is invisible. That is the exact shape of the round-27 play-test result: gun smoke and impact effects rendered for the first time since round 4, explosions and missile-ship effects did not. `1` re-reads `v9.z` per quad, from that quad's corner 0 -- the same corner every other per-quad term is taken from. **It cannot regress a genuinely per-effect phase, but by VALUE and not by pointer** -- an earlier draft of this row claimed "a register-sourced ATTR9 has stride 0 so `at(base)` is byte-identical to `at(0)`", and that is wrong: `map_attribute()` refuses a non-persistent attribute *and* a zero stride separately, so a register-sourced ATTR9 leaves `have_tc1` false and turns the flipbook path off entirely. The real safety is that a stream genuinely carrying one phase per effect holds that same value at every vertex. On a per-quad decode failure it falls back to the draw-wide value, never to cell 0, so one unreadable quad cannot send a single particle to the first frame while its neighbours animate. `0` restores round 27's draw-wide read. Echoed on `Remix live:` as `particleflipphase=`. | |
| `RPCS3_REMIX_PARTICLEFLIPPHASE` companion counters | -- | **Round 28 -- `particle_declined` and `particle_zeroseg`, and a REFUTATION of round 27's acceptance test.** Round 27 shipped the rule "`particle=<draws>/<quads>` must climb and `lay_other` on `Remix world-fail:` must fall by that same draw count -- that pairing is the whole attribution". **It cannot.** `lay_other` is `note_world_fail(7)`, raised inside `per_draw_transform()` at the `!fp.has_outer()` exit, and these six programs take that exit on **every** draw whether the replay then succeeds or not -- the replay only forces the transform to identity *afterwards* and never revisits the classification. So `lay_other` counts the population, not the drop, and stays flat however well the replay works. MEASURED, round-27 run, 62,221 flips: `lay_other=130,728` against `particle=125,830/3,907,165`, and the `Remix world-refused:` census for that session names **only the two ribbon programs**, no billboard. `particle_declined` counts draws on a matched program where the replay wrote nothing, so `particle_replayed + particle_declined` is every matched draw **that reached the replay site** -- a qualifier the review insisted on and it is load-bearing: `submit_subdraw` returns early in ~40 places before the replay, and `skip_instanced` in particular discards `pass_count()-1` passes of a trivially instanced draw on the test `!fp->has_outer()`, which is exactly the property these six programs have. Equally, **do not read the 130,728 - 125,830 = 4,898 difference as this counter's expected value**: `lay_other` fires for *any* `!has_outer()` program, only inside the `has_reference` branch, and double-counts under `RPCS3_REMIX_DUMP`. What *is* measured is that the dedicated `Remix layother:` census named exactly those six programs and no seventh, out of a 64-line budget it never filled. `particle_zeroseg` names what `particle_ref[.../basis]` actually contained: MEASURED, 100% of the 9,039 was the `zeroseg` verdict and 100% of that was ribbons -- a trail whose two endpoints coincide, which the guest's own `RSQ(0)` discards too. Refusing it is faithful, not lost geometry, and the counter's name ("the derived camera basis was degenerate") was misleading. | |
| `RPCS3_REMIX_PARTICLECENSUS` | `0` (off, clamped to 64) | **Round 27 -- the acceptance instrument for item 1.** One `Remix particle:` line per (program, outcome) per stats window, deduped with a golden-ratio-mixed key rather than an XOR of a small integer (an XOR collides between programs differing only in their low bits -- the trap `world_refused_census_slot()` documents at its own key). Prints the decoded centre, the authored size, the derived quad count, and -- **the field that matters most** -- the eye `c26` the replay billboarded from, **beside the backend's own `m_active_camera.position`**. If those two disagree, the particle pass is drawing through a different camera than the gauge anchor and the replayed "world" is not our world. That is the single assumption in this route the ucode alone cannot settle, so read it before anything else. Gated only on its own budget, **not** on `DIAGLINES` or `DUMP` -- round 12 shipped a census gated on `dump_enabled()` that produced zero lines against 22,365 applications, and this deliberately avoids that. `0` = no output and no work. | |
| `RPCS3_REMIX_SUNSPRITE` | empty | **Round 27 -- ITEM 2, aim the sun from the sun's own sprite.** `<albedo>[,<albedo>...]`, bounded at 4, of texture **content hashes**. Haze draws its sun as a **64x64 screen-space sprite** (`albedo=A61A3CBECA257FE0`, `route=2d`, `tex=64x64`, `subrect=1/1`, drawn by `2f64c2f8ffd6add1` and `2f650a38ffe6add1`, 292 `Remix uiwrap:` rows and **zero** world-path rows in the round-26 run). That is why it is not clickable in the dev menu, why it never appeared as world geometry, and why round 16's "head-locked, distance pinned at ~2.0" measurement was *right about the geometry and wrong about the conclusion*. Because the game draws the sprite where its own sun **appears**, the sprite's screen position encodes the sun's **direction**: the quad's NDC centre unprojected through the live camera is the ray to the sun -- per level, per frame, with no sky-texture analysis and no hand-tuned table. Implemented by unprojecting `(ndc_x, ndc_y, 0)` and `(ndc_x, ndc_y, 1)` through `m_active_camera.view_proj_inverse` and subtracting, so the eye translation cancels and no separate camera position is needed; the result is **negated**, because this backend's convention throughout is `travel` = the direction the light *travels* (sun to scene), which is the negation of eye-to-sun and the single easiest sign here to get backwards. **PRECEDENCE SHIPPED:** `SUNMAP > SPRITE > SUNSKY > SUNTRACK card > SUNDIR`. An earlier draft of this round put the sprite *above* `SUNMAP`, on the grounds that `SUNMAP` was one hand-tuned vector per sky texture that could not serve two levels at once. **That premise is now false and the decision was reversed before shipping:** Haze's PBCK archives have since been unpacked, every level pak authors its own `k_scene_sun` block with an azimuth and an elevation, and for the jungle level the file-derived direction agrees with the sky-texture centroid to **0.8 degrees in azimuth**. `SUNMAP` is therefore file-derived ground truth wherever it has an entry, and ground truth outranks a derivation -- including this one. The sprite's job is what it should always have been: the automatic per-level source for every level `SUNMAP` does **not** cover, sitting above the sky-texture centroid. Gated on the **albedo**, never the program: both vertex programs that draw this sprite also draw other content (`2f64c2f8ffd6add1` is the nectar/health HUD gauge; `2f650a38ffe6add1` draws ordinary depth-writing world geometry), so a program-keyed observer would aim the sun at a HUD bar. Same one-frame freshness rule as the sky path, so a latch from a level the player has left cannot keep aiming the light; going stale holds the last aim rather than snapping to `SUNDIR`. **ROUND 28: that freshness rule is now `SUNSPRITEHOLD` frames on BOTH this rung and the SUNMAP rung** (see that knob for why holding only one of them inverts the precedence), and the counter group gained a fourth slot: `sunsprite_rej=<offscreen>/<span>/<nocam>/<backwards>`. Counters `sunsprite=<seen>/<solved>` and `sunsprite_rej=...` on `Remix live:`. Empty = byte-for-byte round 26. **Blanking it does NOT "put SUNMAP back on top" -- SUNMAP is already on top; blanking removes the automatic rung that covers every level SUNMAP has no entry for.** | |
| `RPCS3_REMIX_SUNSPRITEMAXSPAN` | `0.5` | **Round 27 -- the guard that makes the sprite trustworthy.** NDC units. The accepted quad must be **fully inside** the NDC cube (any bound at or past +-1 is rejected as `offscreen`), both spans must be at most this value, and the two spans must be within 2x of each other. A sprite the engine has clamped to a screen edge is where the *edge* is, not where the sun is, and using it would swing the light by tens of degrees as the player turns; a full-screen or stretched draw sharing the albedo is not the sun even when its texture is. A sun that has genuinely left the screen simply stops publishing and the light holds its last aim. `0.5` is a quarter of the screen per axis -- far larger than a sun sprite ever is, and small enough to reject a full-screen flare. | |
| `RPCS3_REMIX_SUNSPRITECENSUS` | `0` (off, clamped to 64) | **Round 27.** `Remix sunsprite:` lines carrying the NDC bounding box, the derived centre, the derived `travel`, the camera validity and inverse-availability flags, and the verdict (`solved` / `offscreen` / `span` / `nocam` / `unproject` / `degenerate` / `nonfinite`). This is how "the sun never moves" is answered without a guess: `sunsprite=N/0` means every sighting was **rejected** and the line names which guard did it, which is a different problem from the sprite never being drawn. `0` = no output. | |
| `RPCS3_REMIX_SUNSPRITEHOLD` | `0` (off, clamped to 3600) | **Round 28 -- stop the sun flipping direction when the player looks down.** Frames. The sprite is only solvable while it is **fully** on screen, so it necessarily stops solving the moment the player tilts far enough for the sun to leave the view -- and one frame later the precedence chain drops to the next rung. MEASURED in the round-27 run, two consecutive retargets: `travel=[0.3695 -0.8084 -0.4582] src=sprite frame=4337` then `travel=[-0.1993 -0.3827 0.9021] moved=100.2 deg src=sky frame=4339`. A hundred-degree swing keyed to head pitch is worse than a slightly wrong sun that stays put, which is the user's report verbatim. This extends how long the last **solved** direction stays authoritative; nothing is recomputed while held, and `Remix sun-retarget:` prints `src=spritehold` rather than `src=sprite` so the two states are never confused. The 3600 clamp exists because the one-frame rule it relaxes was there to stop a per-level source surviving a level change. `0` = round 27's one-frame rule exactly. Echoed as `sunspritehold=`. **IT APPLIES TO THE SUNMAP RUNG AS WELL, and that is not optional -- the round-28 review caught it before it shipped.** `m_sky_sun_frame` is republished only while that area's dome is *being drawn*, so holding only the sprite would **invert the documented precedence**: any two consecutive frames without a dome (indoors, looking down, a cutscene) would drop SUNMAP out of the chain and let the held sprite re-aim the light away from file-derived ground truth that was authoritative one frame earlier. Before round 28 both rungs went stale together, `want` stayed null, and the light simply *held* SUNMAP's aim -- the correct outcome. Both rungs now take this same value, so they can never trade places because of it, and `src=sunmaphold` names the SUNMAP half. Known, bounded residue: neither latch is cleared on a level change, so for up to this many frames after a transition the previous level's direction can still be held. | |
| Sun-sprite `nocam` -- **ROOT-CAUSED IN ROUND 28, and it was not the camera** | -- | **The archetype-B split camera path never filled `has_view_proj_inverse`.** MEASURED, round-27 run: of 1,258 `Remix sunsprite:` census lines, **all 610** `nocam` rejections read `camvalid=1 vpinv=0`, and **not one** read `camvalid=0`; only the 179 `solved` lines and 38 of the `offscreen` lines read `vpinv=1`. Cause: of the three `consider_camera_candidate()` call sites in `RemixGSRender.cpp`, the layered site and the `recovered` site both compute `has_view_proj_inverse = mat4_invert(mat4_multiply(view, projection), view_proj_inverse)`, and the third -- the archetype-B split path, the dominant producer on this title -- assigned `view`, `projection`, `reference_inverse`, `position` and the viewport but **never** `has_view_proj_inverse`. The field kept its `false` default member initialiser all the way onto `m_active_camera`, so everything that unprojects through the live camera saw "this camera cannot be inverted" while `m_active_camera.valid` was perfectly true. Fixed by computing it there with the identical expression the two siblings use. That accounts for the whole of `sunsprite_rej[.../nocam]=8487`. **`sunsprite_rej` now has a fourth slot:** `backwards` was folded into `nocam`, and it is the one verdict meaning the ray came out pointing away from the camera's forward axis -- the reverse-Z failure that would aim the sun exactly 180 degrees the wrong way with every counter still climbing. It measured **zero** on this title, so any non-zero reading is new information. | |
| `RPCS3_REMIX_VMTRIPLEVP` / `VMTRIPLEFP` / `VMTRIPLEALBEDO` | empty | **Round 27 -- ITEM 3, the (vp, fp, albedo) viewmodel triple. DECLARED, PARSED, AND DELIBERATELY NOT WIRED TO A DECISION SITE.** Any one list being empty disarms the route, the same rule `viewmodel_pair_matches` uses. **Why a triple rather than the (vp, fp) pair the round-27 brief proposed -- both refuted by measurement this round:** (i) the fragment program does **not** partition the rig, because the user's picks of *both* the weapon (`vtx=2140`) and the arms (`vtx=4456`) read the same `fp=0ccd70030837ee85` on `vp=830d7d1b9681c475`; and (ii) that same fp also draws **four world textures** (`1CDD5249E6504F13` alone carries 164 `Remix fpcandidate:` rows and 79 `Remix world-refused:` rows), so `(vp, fp)` alone would tag world geometry -- exactly the regression class that has repeatedly deleted NPC bodies. **And the deeper refutation, which is why nothing is armed:** `vm_tagged=1` against `vm_considered=13,631,618` in the round-26 run. The viewmodel tag fires **once** in a 72,000-frame session, so `VMBASIS` is acting on essentially nothing and the arms the user sees are ordinary world geometry. Their real obstacle is upstream of tagging entirely -- they are **refused** at the world gate (`fail=tail`, residue 3.2--6.2 against `tol=0.02`, tail rescue failed), not mis-tagged. The route exists so it can be armed from the launcher next round with no rebuild, once the arms are no longer being dropped. | |
| `RPCS3_REMIX_FPCENSUSVP` (amended) | empty | **Round 27 adds `fp=` to the `Remix fpcandidate:` line and to its dedup key.** This was the one census large enough to answer "does the fragment program separate the player's rig from the NPC bodies that share its albedo", and it printed `vp=` and `albedo=` but not `fp=`. Before this change the only log lines carrying `(vp, fp, albedo)` together were eleven user-driven `Remix picked:` rows and a deduped, capped `Remix kil:` census -- suggestive, but not a population measurement. **The key change is load-bearing, not cosmetic:** printing `fp=` while continuing to dedup on `(vp, albedo)` would emit exactly one fp per pair and silently hide the rest, which is the opposite of the cross-tab this census now exists to produce. The extra distinctness is bounded by the same 48-line budget. This is the only round-27 change that alters behaviour when its knob is already armed, and it alters **log content only** -- it tags nothing, refuses nothing, and moves no counter. | |

## Round 29 -- the camera position's frame of reference

| name | default | effect | in GUI |
| --- | --- | --- | --- |
| `RPCS3_REMIX_ANCHORGAUGECENSUS` | `32` (lines per 120-flip window, `0` = off, hard-capped at 64) | **Round 29 -- pure measurement, no behaviour, and it names the thing 28 rounds of wobble hunting kept tripping over.** `m_active_camera.position` has **two sources in different frames of reference** and nothing in the backend labels which one a consumer is holding: the camera election writes the guest's world-space eye (it matches the guest's own `c26` eye constant to 4-5 significant figures), and `apply_gauge_anchor_camera()` overwrites it every flip with the eye recovered by splitting the **gauge anchor's** fused matrix -- which is the eye in the *anchor donor's* frame. On Haze the main-clip (1024x576) anchor is donated by `AD7CE9D672A0BF6B` on 3,516 of 3,901 traced flips, and round 22 already measured that program discarding a real placement (mean \|t\| 197, max 4,377) with a **fixed carrier-local vertex box**: it is the land carrier, its fused matrix is `W*V*P` rather than `V*P`, and so the anchor's "world" rides with the vehicle. This knob emits bounded `Remix anchor-gauge:` lines from the one site that can see both sources at once -- inside `apply_gauge_anchor_camera`, after the override is written and before the flip's election latch replaces it -- printing both positions, their difference, the dominant axis and the anchor's identity; plus two live-line counters, `anchor_cam_offset` (flips disagreeing by over one unit) and `anchor_cam_offmax` (the largest L-infinity disagreement of the run). **MEASURED** on the round-27 run (`bin\remix_dump.log`, newest full session at line 1,906,093, 62,315 flips), reconstructed offline because no counter said any of it: 46 of 681 `Remix particle:` census lines disagree by over a unit; **45 of the 46 are X-dominant**, max `dx` 421.44 against max `dy` 2.78 and max `dz` 8.60 -- X-dominant because the carrier drives along X. It is **not** accumulating error: the slope is a dead-constant 0.54 units/frame while the guest's own eye sits at X = 15.2 for 3,000 frames, i.e. it is the carrier's speed. It is **not** a reset either: the four ~784-unit jumps are the guest re-basing the carrier's local origin, and 4 of 4 coincide with a main-clip `Remix anchor-elect:` line (frames 4609, 6139, 7737, 9178), while the frames that appear to "snap back" are simply the flips where the election latch won. The identity `cam = (guest eye) + (the translation the WORLDIDENTITYVP override is discarding on that frame's anchor donor)` holds to **±1.01 units on 39 of 41 disagreeing frames against a 421-unit signal**, the residual being one frame of the donor's own travel. **The root cause is that one declaration drives two gates and only one of them learned about `WORLDIDMAXT`**: `submit_subdraw` reads a 335-unit discarded placement and correctly keeps the draw's real transform (round 24's carrier fix, armed at 32), while `per_draw_transform` has already installed that same draw's fused matrix as the whole frame's world gauge on the vp-hash alone. Round 30's fix is to apply the same verdict at the capture site; 67% of `AD7CE9`'s draws sit at \|t\| <= 1, so the slot stays populated. **Read:** `anchor_cam_offset` / `anchor_cam_offmax` on `Remix live:`, and `Remix anchor-gauge:` in `bin\remix_dump.log`. `offmax` in the hundreds with `axis=x` and `anchor_vp=ad7ce9d672a0bf6b` reproduces the whole measurement in one grep; `offmax` under ~2 for a session means the level played had no moving anchor donor and this mechanism is **not** that session's wobble. **What this REFUTES:** round 28 inferred the particles were "being placed hundreds of units from where the guest drew them". They are not -- `apply_particle_billboards` contains no reference to `m_active_camera` at all, building its basis from the guest's own `c26` eye and `c8`/`c9`/`c10` view rows against raw pre-divide vertices. The `cam=` field of `Remix particle:` is a diagnostic printed beside `eye=`, and that pair is what the 421.44 figure was reconstructed *from*; it never placed a quad. **Not gated by `DIAGLINES`** -- it has its own budget, so set this to `0` to silence it. **Revert:** `0`, which removes the lines and the two counters and changes nothing else. | no |

## Round 30 -- the camera-frame fix, per-albedo emissive intensity, and the UV-wrapping family

| name | default | effect | in GUI |
| --- | --- | --- | --- |
| `RPCS3_REMIX_CAMANCHOREYE` | `1` (on) | **Round 30 -- this is the fix round 29 specified and deliberately did not ship, and it corrects round 29 on one load-bearing point.** Six consumers compute a distance between the camera and a **submitted** instance transform. A submitted transform is `fused * anchor->inverse`, so it lives in the **gauge anchor's** frame by construction -- whatever frame that anchor happens to be in. `m_active_camera.position`, meanwhile, has three write sites and two frames: the flip-tail election latch, the mid-frame relatch in `consider_camera_candidate` (which fires on 52,505 of 66,508 flips, so it is what consumers actually hold), and `apply_gauge_anchor_camera`. This knob publishes the anchor's own eye -- the value `apply_gauge_anchor_camera` **already computes** for free -- under its own name, and hands it to those six consumers plus four evidence-gating diagnostics. It changes nothing about what `.position` means, because `.position` and `.reference_inverse` are always written together and the viewmodel probe divides by `.reference_inverse`. **MEASURED, live, from the round-29 build's own census** (newest session in `bin\remix_dump.log`, starts at line 1,998,822, `flips=66508`): `anchor_cam_offset=2134`, `anchor_cam_offmax=429.96` units, `gauge_cam=59008` (88.7% of flips take the override). So the two writers disagree by over a unit on 3.2% of flips, worst 429.96 units on X -- and on those flips every one of the six was subtracting two points that are not in the same space. **WHERE ROUND 29 WAS WRONG.** It attributed the disagreement to `AD7CE9D672A0BF6B`, the land carrier's local-space program, on 90% of flips. Bucketing the 13,928 `Remix anchor-gauge:` lines of that session by magnitude (with a field-splitting parser, not a regex -- a first regex pass silently missed 685 lines to a greedy `.*`, though it agreed on the 30) gives bucket 0 = 13,898 and buckets 1..4 = 30 lines, and **all 30 of the over-one-unit lines carry `anchor_vp=BD1C10DF5703E559`, all on X** -- the program round 24 documented as submitting ABSOLUTE WORLD vertices (raw box x [-1397.69 .. +1146.32], i.e. the map). Per-donor worst disagreement over every line it anchored: `BD1C10DF5703E559` **427.48 units** (151 lines), `AD7CE9D672A0BF6B` **0.124146 units** (11,062 lines), `0214281B9A7A412D` **0.00146484 units** (2,715 lines). **The carrier program round 29 accused never disagrees by more than an eighth of a unit across eleven thousand samples.** Round 29's proposed fix (a), refuse a displaced draw as a gauge donor, would therefore have **refused the genuine world-space map** and kept the carrier-local gauge instead. It was not shipped, and the polarity question it turns on does not need answering for this fix to be correct. **The six:** `derive_sky_sun`'s `travel` (its verdict is latched per albedo for the whole run, so one bad frame mis-aims a level's sun permanently), `note_sun_card`'s `to_card`, `submit_subdraw`'s sky `inside` and `anchor` tests (`SKYANCHOR` is a few-unit limit, so 429.96 units refuses every dome outright), `submit_subdraw`'s viewmodel distance gate, and `apply_viewmodel_basis`'s reflection **pivot**. **Plus four diagnostics that were biasing this project's own evidence:** `SUNCARDMINDIST`'s floor and `FPCENSUSMAXDIST`'s ceiling both **gate line emission**, and `viewmodel_far` / `skip_census_dist` are read as evidence. **Deliberately NOT changed:** the viewmodel gate reached from `per_draw_transform`, whose position comes from `fused * m_active_camera.reference_inverse` and is therefore already self-consistent with `.position`; it now passes `m_active_camera.position` explicitly, and `viewmodel_anchor_rejects` / `viewmodel_pair_rejects` take the eye as a parameter so the two callers can never be confused again. **AND THE REFUSAL THAT MAKES IT SAFE -- the round-30 review caught this as a BLOCKER and it is now part of the knob's contract.** The eye is served only to a draw that was actually divided by that same anchor. `per_draw_transform` initialises `reference = &m_active_camera.reference_inverse` and swaps in an anchor only when `find_gauge_anchor` hits; on a miss it counts `gauge_anchor_absent` and divides by the ELECTED camera, so that draw's submitted transform is in the elected frame and its correct eye is `m_active_camera.position`. MEASURED: `gauge_used=13,782,344` + `gauge_prev=2,798,910` + `gauge_absent=1,287,878` = **7.21% of divides are in the elected frame**, and the gates being fed are armed at single digits in this launcher (`SKYANCHOR=4`, `VIEWMODELANCHOR=4`, `VMPAIRMAXDIST=2`, `SUNCARDMINDIST=4`, `FPCENSUSMAXDIST=4`) against a 429.96-unit frame gap -- so substituting there would have been a regression, not a rounding concern. The gate is `m_ref_pick_source` in {`anchor`, `anchor_prev`} **and** `m_ref_pick_vp == m_anchor_frame_eye_vp` (the second half matters because `find_gauge_anchor` keys on the exact (surface, target, clip) while `main_gauge_anchor`, which publishes the eye, returns the largest-area slot). Residual, stated: two slots held by the same program pass the vp test, and by measurement that program's own disagreement is at most 0.124146 units, so the residual is sub-unit. **Two further review fixes in the same family:** `derive_sky_sun` alone refuses an eye that came from the `GAUGECAMHOLD` branch (a held eye is re-stamped as fresh and can be 30 frames old; every other consumer re-evaluates each frame, but this one freezes its verdict per albedo for the run), and `skip_census_dist` uses the uncounted getter because it runs on every submitted draw (~14.6M/run) and nothing refuses on it, so counting it would have swamped `camframe_served`. **Cost:** zero new matrix work -- the eye is a copy of a value already computed once per flip. **Limit, stated as a number:** the published eye is one flip old for a mid-frame consumer, so it lags by the anchor donor's own travel, 0.54 units/frame on the carrier (round 29, least-squares over 480 frames) = 0.13% of the 429.96 it removes; anything older than one flip is refused outright, which is the scene-cut guard. **Read:** `camframe_served` / `camframe_corrected` / `camframe_absent` / `camframe_max` on `Remix live:`. `camframe_corrected` and `camframe_max` accumulate with the knob **off** as well, so the `CAMANCHOREYE=0` arm still reports the size of the correction it declined to make -- that is what makes the A/B one relaunch, and the first draft got it wrong by testing the knob first. `camframe_max` should be the same order as `anchor_cam_offmax`. `camframe_corrected=0` with `camframe_served` large is a **result**, not a failure: that level had no frame disagreement. Also new: `camanchor=[..]` on `Remix picked:`, beside an unchanged `cam=` -- `origin=` is anchor-frame, so `origin - camanchor` is a real distance and `origin - cam` is the quantity this project had been quoting as ground truth while it was off by up to 429.96 units on X. **Revert:** `0`. Every consumer falls back to `m_active_camera.position`, which is the round-29 value exactly. | no |
| `RPCS3_REMIX_EMISSIVE` (amended) | the launcher's four hashes | **Round 30 -- per-albedo intensity.** The syntax is now `<hex>[:<intensity>][,<hex>[:<intensity>]...]`, bound 16, following the `SUNMAP=<hash>:<x>,<y>,<z>` precedent already in this codebase. A hash with **no** colon keeps using the global `RPCS3_REMIX_EMISSIVEINTENSITY` and is parsed exactly as before, so a colon-free launcher is bit-identical to round 29 -- **VERIFIED** by compiling the shipped parser text verbatim into a standalone harness and running the round-29 value through it: `count=4 with_intensity=0`, all four intensities `= 1`. The round-30 value returns `count=4 with_intensity=1` with `71D189E9B559A7F9 -> 30` and the other three unchanged at `1`. **Why it exists:** `EMISSIVEINTENSITY` was a single global applied to every listed hash, so raising it for the plant's light bulbs would also have blasted the smelting-plant windows (`3ED07EDE1C03A651`) and two door fixtures. **Bounds and failure modes** (all exercised in the harness): an intensity must parse, be finite and be `> 0`, and is clamped to `1e6`; `:`, `:abc`, `:-5`, `:0` and `:1e999` all leave that hash on the global **and** still parse the rest of the list; whitespace either side of the colon is accepted (`AAAA : 30` works -- the space-before case was a silent no-op in the first draft and was found by that harness, not by review); the environment buffer is 512 wchars, raised from 400 because 16 entries with intensities can reach 415 characters and the over-long guard refuses the **whole** list rather than truncating it. The material cache is keyed on content hash plus sampler/alpha state and the intensity is a pure function of the content hash, so no cache-key change was needed. A review pass then found two more, both fixed and re-verified: `AAAA:30E` invented a **phantom albedo** (`wcstod` stops before a trailing hex letter and the outer loop parsed that letter as a hash, consuming one of the 16 slots), and `AAAA:30:40` / `AAAA:30x` dropped the rest of the list. One skip that stops at `,` `;` **or whitespace** fixes both without regressing `AAAA:30 BBBB`. **Read:** `emissiveper=` on the run-start banner and on `Remix live:` -- the count of entries carrying their own intensity. `emissiveper=0` with `emissive=4` means the running exe predates this round and the colon did nothing. **Revert:** drop the `:30`. | no |
| `RPCS3_REMIX_UIDUMP` (value changed, no code change) | `24` -> `4000` in the launcher | **Round 30 -- diagnostic budget only, no pixel depends on it.** Round 26's HUD-font fix (`UIRECTSHRINK`, a 50% shrink keyed on albedo `6575ACE3A42A78E6`) was re-verified and **two further overlays were tested per albedo rather than by resemblance**, because applying a 50% shrink to a correctly-authored quad renders it at half size. **Positive control `6575ACE3A42A78E6` (512x512): signature PRESENT** -- from the 24 `Remix ui-quads[..]` lines in `bin\log\RPCS3.log` (16 quads, all 24 payloads byte-identical), extent/adjacent-centre-spacing = **2.0417** mean over n=15, v extent 45.978-46.029 texels against a measured atlas row pitch of **23** (autocorrelation peak 0.3767 at lag 23) = **1.9990-2.0013**, and every u extent halves to an integer (16, 17, 18, 19, 20). **Second atlas found, `85CC1EB51A38B1E9` (128x192), drawn by the same vp+fp pair (`6F76AB0AD8D926B1` / `4A1BC009FC6161EF`): signature ABSENT on v, and it must NOT be listed on present evidence.** Its row pitch is exactly **16.000** texels (9 bands, tops 1,17,33..129, eight deltas all 16; autocorrelation 0.7650 at lag 16); its authored `v_lo` is **-0.500** texels, but round-26 doubling moves every cell top up half a pitch and so requires `v_lo` congruent to -8 mod 16 -- a 7.5-texel discrepancy with no row index fitting, while 1:1 authoring gives `16k - 0.5` with `k = 0` exactly, residual 0.000. The same test passes on the positive control, so it is calibrated. The u axis is NOT DETERMINED (that sheet is a proportional cache with no column grid). **Pulsate warning (grey): NOT DETERMINED** -- no `Remix picked:` line in any of the 82 sessions in `remix_dump.log` lands on a full-screen overlay (1,506 pick lines; the only glyph-atlas pick ever recorded is `6575ACE3A42A78E6`), 58 of the 68 UI albedos in the newest session author exactly `u=[0..1] v=[0..1] exc=0.00,0.00`, and `ui_rect_shrink_for()` returns 100 whenever a span exceeds 0.5 of the sheet -- so listing a full-screen albedo here is a **guaranteed no-op** whatever its screen rect does. **Why the budget moved:** the old 24 was exhausted at `t=34.787 s` and the first 128x192 draw on that program is at `t=89.542 s / frame 3747`, roughly 1,670 dumps later, so the extent/spacing test could not be run at all. 4000 covers it for about 6 MB of `RPCS3.log`. After the next run, `findstr /C:"ui-quads" bin\log\RPCS3.log` filtered for `tex=128x192` settles it: v extent about 32 texels means doubled (add the hash), about 16 means correctly authored (adding it would halve correct text -- which is what the `v_lo` arithmetic predicts). **Revert:** `24`, or `0` to switch the capture off. | no |

## Round 31 -- the static-index rebuild budget: one mechanism behind the lagging mesh AND the vanishing walls

Round 31 shipped no new knob. It raised a **source clamp** that had made an existing knob
untestable, split one counter in two, added three sub-timers, and changed three launcher values.
Every number below is measured from the round-30 play-test (`bin\remix_dump.log` lines
2,127,518-2,211,207, build `05:07:04`, pid 19648, **60,211 flips**) and from `bin\log\RPCS3.log`.

**Three named hypotheses were refuted before this one was found.**
(1) *A skinned draw's bone palette going a frame stale*: `skinned=1` occurs **0 times** in the whole
session and `Remix bone-fail:` is all-zero on all 840 lines -- there is no active skinned path at all,
every character goes through `arch=fused`.
(2) *Two draws of one skeleton splitting on gauge age*: gauge age is a per-**program** near-constant,
only 4 programs ever report `ref=anchor_prev`, and across 6,876 frames exactly **one**
`(frame, albedo)` group ever received two different references.
(3) *Back-face culling eating the walls*: `instance.doubleSided` comes from
`cull_from_rsx() && cull_face_enabled()` at `RemixGSRender.cpp:20447`. `cull_from_rsx()` returns
`env || g_cfg.video.remix.cull_from_rsx`, i.e. the env var and the config are **OR'd**, so either one
alone would arm the path and "inert" requires **both** off. Both are: `RPCS3_REMIX_CULL` is absent
from the launcher (verified absent from the child environment through real `cmd.exe`) **and**
`Use RSX Backface Culling: false` in both `bin/config/config.yml` (the `Remix:` block, line 246;
key at 280) and `bin/config/custom_configs/config_BLUS30094.yml`. That config key is
`system_config.h:235` `cull_from_rsx`, the only setting in that file whose name contains "Backface"
(there is no `g_cfg.video.backface_culling`), so it is a genuine gate rather than an unrelated GL/VK
setting. Every instance is therefore submitted double-sided.
Note also that the backend never reads `cull_face_mode()` or `front_face_mode()` at all, so if that
knob is ever armed the winding is unvalidated -- the risk its own comment already warns about.

A fourth, weaker reading was also refuted: the "2,045-unit displacement" visible on 9 of the 18 picks
is an **artefact**. `record.origin` is the transform's *translation row*
(`RemixGSRender.cpp:21003`) and `dist` is `|translation - eye|` (`:20258`), so a draw whose vertices
are already absolute world and whose transform is correctly the identity necessarily reads ~|camera|
away from the eye. 72.3% of `worldid-draw` vertex boxes sit at `|coord| >= 1000`, which is the
confirmation. Every single-digit distance gate in this launcher (`SKYANCHOR=4`, `VIEWMODELANCHOR=4`,
`VMPAIRMAXDIST=2`, `SUNCARDMINDIST=4`, `FPCENSUSMAXDIST=4`) is compared against that same quantity.

| name | default | effect | in GUI |
| --- | --- | --- | --- |
| `RPCS3_REMIX_STATICINDEXBUDGET` (source clamp raised; default unchanged) | `4`; ceiling was **also 4**, now `64` | **Round 31 -- this was never a performance lever; it decides whether geometry is drawn at all.** `STATICINDEXVP` programs accumulate a changing triangle subset into a "union" mesh, and this caps union-mesh creations per presented frame. What a deferral costs is at `RemixGSRender.cpp:19258-19295` and `:19398-19409`, and it is not frame time. There are **three** deferral exits with **two** outcomes. With a previously built handle still live the draw renders **that** subset -- a *previous frame's* TRIANGLE SET, geometry only (`stale`). **It is NOT a material effect**: `static_key` (`:19053-19055`) and the union hash (`:19225-19227`) both fold in vp, albedo *and* the material pointer, so a material change lands on a different entry whose `mesh_hash` is 0, which makes `active_valid` false and sends the draw to the drop exit instead. A stale union always shades identically to the draw falling back to it -- what lags is the triangle set, never the shading. An earlier draft of this section claimed otherwise; chasing that would have cost a round. **What makes a stale copy converge, also corrected:** it is the *geometry* stopping while flips CONTINUE — nothing sets `static_entry->dirty`, so the rebuild branch is never entered and the last-known-good union IS the current one. It is **not** "when frames stop": `++m_frame_counter` lives inside `flip()` and the budget resets only when that counter changes, so if flips genuinely stop the budget stays spent and the stale copy **freezes** rather than converging. An in-game pause menu is the first state, not the second, which is why it reads as aligned while paused — and a round-32 test built on "looks aligned while paused" must hold that distinction. With no live handle, or on the budgeted-fallback path when the subset mesh is not in the cache, the draw **`return`s and is never submitted** (`dropped`, two sites). That second exit precedes the refusal accounting, so a surface lost this way can **never** appear in `Remix world-refused:` -- which is exactly why searching the refusal families for the missing walls comes back empty (measured: **0 of 15** picked albedos appear in any refusal family). That evidence has a stated limit: the pick record is built *after* this return (`:21003`), so a dropped draw can never be picked, and the picks prove only that these surfaces are not refused at the angles where they do render. What carries it is that refusal here is not angle-scoped -- the dominant refusing program appears in 95.6% of all 120-frame census windows in 5 runs, and the next two have single absences of 12,201 and 14,374 frames, i.e. level-scoped. Refusal cannot be what comes and goes as the camera turns on the spot; a per-frame rebuild budget can. MEASURED: `Remix static-index:` read `peak=4 budget=4` on **all 811 lines** of the session with `deferred=915,419` = **15.2 deferrals/frame against a budget of 4**, i.e. saturated for the entire run. `std::clamp(env, 1u, 4u)` meant the knob could only ever be turned *down*, so the saturated case could not be A/B'd; the ceiling is now 64 and `RemixTransforms.cpp:10348` carries the reasoning. Cost of relieving it, measured rather than guessed: `Remix timing:` gives `mesh_create` = **0.11 ms/frame** over 20.02 creates/frame = **5.49 us per create** (110/20.02; a frame-weighted pass over all 840 windows gives 5.64 us, the gap being the rounded per-window ms field -- the two numerators do not mix), so ~20 extra creations/frame is ~113 us = **0.4%** of the measured 28.14 ms frame. **The launcher went 4 -> 8, not 4 -> 24, and the first draft's cost model was wrong.** It priced only CreateMesh CPU and omitted **BLAS residency**, which is the term this project has already proved is the FPS driver: every rebuild mints a new handle, the superseded one stays resident for `MESHIDLE` frames, and `MESHCAP=0` means there is no ceiling. 4 -> 24 would have taken creates/frame ~20 -> ~39, roughly doubling the resident set -- a probable regression inside a round whose item 3 is performance. And `peak=4 budget=4` on all 811 lines means the unions never converged, so the churn is steady-state, not transient. 8 clears about half the 15.2 deferrals/frame for ~+4 creates/frame (+20%). **Report `meshes_live/created/destroyed` from `Remix stats:` beside the frame rate before raising it further, and set a finite `MESHCAP` if `mesh_live` climbs across a session.** **Revert:** `set "RPCS3_REMIX_STATICINDEXBUDGET=4"`. | no |
| `Remix static-index:` gains `stale=` and `dropped=` | -- | **Round 31 -- measurement only; no decision changes.** `m_static_index_deferred` counted all three exits above as one number, so a run could not say whether a deferral cost a stale copy or a missing surface. `stale + dropped == deferred` exactly, so runs taken before the split stay readable against this one. `stale` is the "second mesh falling behind the bodies" population; `dropped` is the invisible-walls population. Read them beside `peak` vs `budget`: `peak == budget` on every line means the budget is still the binding constraint. | no |
| `Remix timing:` gains `tex_bind=`, `uv=`, `draw_instance=`, `rest=`, `deferred_instance=` | -- | **Round 31 -- the frame-time instrument existed, was live and ungated all along, and nobody had read it, because it writes through `rsx_log.notice` only and therefore lands in `bin\log\RPCS3.log` and never in `remix_dump.log`** (`RemixGSRender.cpp:22793`; contrast `dump_line()` at `:48-56`, which writes both sinks). 840 windows covering exactly 60,211 frames. MEASURED, frame-weighted: **frame 28.14 ms, `draw` 20.15 ms = 71.6%**; `flip` 7.04 (`present` 5.05), `ui` 3.20, `mesh_create` 0.11. Split by regime, the **quarry** is 50.86 ms / 19.7 fps with `draw`=41.22 and the **interiors** are 19.88 ms / 50.3 fps with `draw`=10.60 -- so `draw` is **98.8% of the entire 30.98 ms gap** while `mesh_create` is 0.3% of it. `draw` wraps only `submit_subdraw()` (`:3087-3742`), and its two existing children left **16.84 ms/frame (59.8% of the whole frame) attributed to nothing**; the three new sub-timers plus the saturating `rest=` residual split it. **This refutes the standing framing that mesh churn is the remaining cost:** at 0.11 ms/frame, eliminating *all* churn recovers ~0.1-0.2 ms of a 28 ms frame. It also refutes all four previously recorded churn rates -- `8eae853c96f5a07e` measured mean **4.5**/frame not 15-32, `f352d7dafa72d0e0` **4.0** not 3-17, `15ad612980aca110` **7.4** with max **33.8** (above the recorded 24 ceiling), and **no** program sits at exactly 5.0 in all its windows. The current top churners are `f39f504649b6f442` (17.2% of all creates) and `15ad612980aca110` (16.8%), neither previously named. **Two stated limits.** (a) All five children of `draw` were proved DISJOINT by mapping every call site to its owning function, not assumed: `texture_cache::bind` has a second caller inside `composite_ui_draw` which is deliberately left untimed because that function is already the `ui` child, so `tex_bind` and `ui` cannot double-count. (b) `submit_deferred`'s own `DrawInstance` (`:2024`) would otherwise have landed in `rest=` -- it is reachable from `write_gauge_anchor` -> `flush_deferred_for_anchor`, which is on the draw path -- silently attributing Remix runtime cost to the backend. Review caught it. It now has its **own** accumulator `deferred_instance=`, printed OUTSIDE the `draw=(..)` group and deliberately not folded into `draw_instance`: its other caller `flush_deferred_at_flip` runs inside `flip`, so a shared counter would be a child of two parents, could exceed `draw` and saturate `rest` to 0. It is 0 today (`defer_buffered=0` with `DEFERPREANCHOR=0`, so the function never runs) which is exactly why the field had to exist before anyone re-enables deferral. `submit_debug_scene:3873` runs once and stays untimed. | no |
| `RPCS3_REMIX_UIRECTSHRINK` (value changed) | gains `85CC1EB51A38B1E9` | **Round 31 -- the hash is KEPT, and the argument is round 30's OWN test finally run per-quad.** Round 30 raised `UIDUMP` specifically so this could be measured, then predicted the answer from the sheet bounding box and warned that adding the hash would halve correct text. **My first pass did not actually rebut it** -- the pre-registered rule at `launcher:3190-3194` said *v extent ~32 texels => doubled, ~16 => correctly authored*, I measured **6 / 18 / 24**, neither predicted value, and I substituted a different statistic (extent / adjacent-centre spacing = 2.058) without addressing the `v_lo` grid argument at all. Review was right to reject that. **So here is round 30's own test, on all 837 captured quads instead of the sheet bbox, against the 16.000-texel row pitch and the row tops at 1, 17, 33 ... 129 that round 30 itself measured:** *read as 1:1 authored* -- per-quad `v_lo` takes the 5 values -4.992, -0.499, 11.002, 92.506, 108.499, a mean residual of **4.901 texels** off that grid, and **701 of 837 quads (83.8%) are TALLER THAN THE 16-TEXEL PITCH**, which no glyph on a 16-texel row grid can be. *Read as doubled and un-doubled about each quad centre* -- `v_lo` becomes **0.998, 1.008, 17.002, 97.003, 113.002**, i.e. exactly round 30's measured row tops, at a mean residual of **0.002 texels (max 0.008)**, and extents become 3 / 9 / 12 with **zero** exceeding the pitch. Two independent tests, three orders of magnitude apart, both say doubled. Round 30's `v_lo` arithmetic was not wrong, it was applied to the sheet *bounding box* (the min over all quads, and -0.499 is indeed one of the five per-quad values) and tested against an assumed grid origin of `16k - 0.5` rather than the row tops it had measured at 1, 17, 33. **Round 30 is not refuted in general -- its measurements all stand and its caution was reasonable; only its conclusion on this one hash is overturned, by its own nominated test.** The 2.058 spacing ratio is withdrawn as an argument: review correctly noted that none of the 136 samples sits at 2.000, which for a proportional glyph cache is expected (advance spacing includes per-glyph bearing and need not equal extent) but which makes it useless as a discriminator. The shrink is not a no-op here: `ui_rect_shrink_for()` refuses spans past 0.5 of the sheet and these are 0.03-0.22; the list bound is 8 and matching is per-hash, so `6575ACE3A42A78E6` behaves byte-identically. Caveat still recorded: the hash-to-sheet binding (128x192 on vp `6F76AB0AD8D926B1` == `85CC1EB51A38B1E9`) is round 30's attribution inherited, **not** re-measured -- the `ui-quads` line carries no albedo field, and that is the thing to doubt first if ASCII text comes out half-size. **Revert:** `set "RPCS3_REMIX_UIRECTSHRINK=6575ACE3A42A78E6"`. | no |
| `RPCS3_REMIX_UIDUMP` (value changed) | `4000` -> `0` | **Round 31 -- the capture it was raised for is complete (row above), so the budget is now pure cost.** MEASURED: those 4,000 lines are **32,184,151 bytes at a mean 8,046 bytes/line = 40.1% of every byte in `bin\log\RPCS3.log`**, each built with a per-vertex `fmt::append` loop; and neither `UIDUMP` nor `UIDUMPVP` appears on the run-start banner, which is why it went unnoticed. The cap is tested *before* the format (`RemixGSRender.cpp:11847`), so frames past 4000 cost nothing -- but frames 1-4000 built ~8 KB of string each. **Revert:** `set "RPCS3_REMIX_UIDUMP=4000"`. | no |

**The union hash is the other half of the story** (`RemixGSRender.cpp:19198-19227`). It is built from
every vertex's position, normal, texcoord and colour, every index, then `m_current_vp_hash`, then
`albedo_hash`, then **`reinterpret_cast<usz>(material)`** -- the material *pointer*. Two consequences.
(1) **A material change mints a brand-new union and therefore consumes budget** -- and because the
material is in `static_key` too, it mints a whole new *entry* with `mesh_hash = 0`, so its first
frames take the **drop** exit and the layer is missing rather than late. That is the correct reading
of a second "bloody" reveal material; it is not a stale-material effect. (2) It is an **O(vertices + indices) serial hash over the entire
accumulated union, on every draw of a listed program**, and the accumulation only grows (`entries`
climbed 22 -> 10,161 and `triangles` 19,555 -> 6,955,311 across the session). That makes it the leading
candidate for the 16.84 ms/frame of unattributed `draw` time, and it predicts the observed regime split
directly -- the quarry's unions are large, the interiors' are small. It will land in the new `rest=`
field. If `rest=` dominates, the cheap fix is to hash incrementally as triangles are inserted rather
than re-hashing the whole set per draw, since `static_entry->dirty` already records when it changed.
A latent risk, recorded not acted on: hashing the material as a raw pointer means a recycled material
address can collide with a retired union.

**Two further measured facts recorded so round 32 does not re-derive them.**

*The f32 gauge inverse is latently wrong by whole world units at this title's coordinates.*
`Remix gauge-selfcheck:` `err32` stratified by `tmag`: `<10` median 6.1e-05 (n=3); `10-100` median
2.44e-04 (n=93); `100-1000` median 0.0156 (n=19); **`>=1000` median 2.0, max 16 (n=469 = 80.3% of all
samples)** -- while `err64` never exceeds 2.98e-08. `f64=1` on all 584 lines and
`world_div_f64 = gauge_used + gauge_prev` exactly, so the **divide** is on the f64 path and this error
is **latent, not live**. But `slot.inverse` (f32) is still what every non-divide consumer reads by
design (`RemixGSRender.h:1523-1527`), and that includes the diagnostics the last several rounds have
been reasoning from.

*Raising the affine tolerance cannot recover the tail refusals.* `Remix world-fail:` closes at
`tail=190,080` / `lay_other=141,114` / `nocam=33,468`, and `Remix affine-residue:` buckets that exact
190,080 population: `<0.05` = **0** and `<0.2` = **0** for the entire session; `<1` = 54,533;
`<10` = 109,461; `>=10` = 26,086; worst residue **35,970.5** against `tol=0.02`. 71.3% are at least
50x the tolerance, so the distribution is nowhere near it and widening the gate is not the fix.

---

## Round 32 (2026-08-17)

**The headline: two of round 31's four leads dissolved on inspection, and the two that survived were
already-solved problems that had been switched off or clamped out.** Read this section before
proposing a fix for wobble or for the sun.

### `ref=anchor_prev` with `anchor_frame == frame - 1` carries NO information. It is a tautology.

`find_gauge_anchor(false, ...)` accepts an entry **only** when `entry.frame + 1 == m_frame_counter`
(anchor: `if (current ? (entry.frame == m_frame_counter) : (entry.frame + 1 == m_frame_counter))`).
So a `ref=anchor_prev` pick can print no other age. "All 7 picks read `anchor_frame = frame - 1`" is
the lookup predicate restated -- not an off-by-one and not a discovery. **Pre-registered refutation:
if `anchor_prev` could ever report an age other than 1, the observation would carry information. It
cannot.**

What `ref=anchor_prev` *does* mean: the draw arrived **before this frame's first `WORLDIDENTITYVP`
draw** on its own render-target key. `capture_gauge_anchor` returns immediately unless
`world_identity_vp_matches(m_current_vp_hash)`, so the anchor cannot exist until one of those six
programs draws. That ordering is deliberate and documented at the call site.

### The wobble fix already exists, was user-confirmed, and is deliberately OFF for ~14 fps.

`launch-haze-remix.cmd` sets `RPCS3_REMIX_DEFERPREANCHOR=0`, with a 2026-08-16 comment recording the
whole trade in the launcher's own words: *"What you lose: the ship no longer slides as you turn --
that fix was real and you confirmed it. What you get back: roughly 14 fps. This is a genuine trade,
not a defect."* Its own pre-registered tripwire tripped: MEASURED `defer_buffered=543244
defer_fresh=149761 defer_flip=393483` = **72% of the buffering did no work**.

**So "props wobbling with the camera" in round 32 is the known, accepted cost of that revert. It is
not a new bug and it does not need a new root cause.** Confirmed from the round-31 run's own
counters: `defer_buffered=0 defer_fresh=0 defer_flip=0`.

### Lead B (the X-only camera divergence) is CONFIRMED, 28x larger than reported, and its mechanism is named.

The brief quoted 15.08 units from one pick. MEASURED from the same session's `Remix live:` lines:
`anchor_cam_offset=4255` flips with `anchor_cam_offmax=430.126` units, and `camframe_max=430.126`
(the same event). Between `flips=25441` and `flips=27250` the count went 884 -> 2687, i.e. **~99.7% of
flips in that stretch had a >1-unit disagreement.** In the *following* session the same counters read
`anchor_cam_offset=0 anchor_cam_offmax=0` -- so this is **level-scoped, not always-on**.

`Remix anchor-gauge:` names it exactly. All 121 over-one-unit lines in a 125 MB window carry the
**same pair** -- `anchor_vp=bd1c10df5703e559`, `elected_vp=7f3d3abcefc8b057` -- and the shape is a
**monotonic ramp that wraps**, not noise:

| frame | `anchorcam` X | `electedcam` X | offset X |
| --- | --- | --- | --- |
| 27720 | -217.335 | 25.8755 | -243.21 |
| 27960 | -327.359 | 25.8759 | -353.235 |
| 28080 | -383.777 | 25.8759 | -409.653 |
| 28200 | **+411.656** | 26.0178 | +385.639 |
| 28680 | 144.0 | 33.6889 | 110.311 |

`electedcam` X moves 25.87 -> 34.66 over 1080 frames (a player walking). `anchorcam` sweeps +/-400 and
wraps. Y and Z agree to six digits throughout. **Four consequences:**

1. **A one-frame staleness cannot produce 400 units at walking pace.** Lead A and lead B are
   unrelated defects and were being conflated.
2. **`ANCHORSTICKY` structurally cannot catch this.** Its test is
   `matrix_relative_delta(fused, slot->fused) <= s_camera_discontinuity_tolerance` -- a
   *consecutive-frame* comparison. A slow monotonic drift stays under the tolerance every single
   frame, forever. Confirmed by counters: `gauge_contested=0`, `anchor_parked=3`.
3. **`bd1c10df5703e559` is the offender; `0214281b9a7a412d` is not.** The five round-31 picks anchored
   on `0214...` read `camanchor - cam` of ~2e-4. Round 30 reached the same per-donor conclusion, so
   this **confirms** round 30 rather than contradicting it.
4. **`gauge_cam` fired on only 7447 of 18451 flips (40.4%)**, so on ~60% of flips the camera was left
   in the elected frame while draws divided by anchors.

**Trap in reading `camanchor=` on a pick line.** It is *seeded* with `m_active_camera.position` and
overwritten only if `anchor_frame_eye()` succeeds (anchor:
`record.camera_anchor_position[i] = m_active_camera.position[i];` followed by
`anchor_frame_eye(record.camera_anchor_position);`). So **`camanchor == cam` bit-for-bit is
ambiguous** -- it means either "the two frames agreed" or "no anchor eye was available". A difference
at 1e-4 proves a real read; exact equality does not. Two of the seven round-31 picks are ambiguous on
this basis, so "6 of 7 picks agree" overstates the evidence.

**Also a methodological catch:** `Remix worldid-census:` reporting `tmax=1.49e-08` for
`bd1c10df5703e559` does **not** vindicate it. That test measures `world = fused x ref^-1` where `ref`
is that same anchor's own inverse, so it is self-referential and identity by construction. It says
nothing about whether the anchor's frame matches the world frame.

| name | default | effect | in GUI |
| --- | --- | --- | --- |
| `RPCS3_REMIX_SUNSPRITEHOLD` (**source clamp raised**) | `0`; ceiling was **3600**, now `216000` | **Round 32 -- the sun revert was a CLAMP, not a failed hold. Highest-confidence finding of the round.** The launcher arms `999999`; the old clamp was `std::min(env, 3600)`, so the effective hold was 3600 flips -- the knob was armed **277x above its own ceiling**. The instrument was already reporting it and nobody read it: the round-31 run's `Remix live:` knobs block reads **`sunspritehold=3600`** on the same line the launcher's 999999 was supposed to produce. MEASURED flip rate from consecutive `Remix sun-submit:` lines (frame 10800 at `0:03:03.633284`, frame 18000 at `0:05:28.412816`) = **49.7 flips/s**, so 3600 flips = **~72 s**. The user's report is "changed back to old position a few **minutes** into playing the mission". **The hold did not fail; it expired on schedule.** The old comment's reasoning was sound and its arithmetic was not -- it argued 3600 is "far short of a level's lifetime", and a Haze mission is minutes long, so 3600 flips is well *inside* a level. Raised to 216000 (~1 h at 60 fps), longer than any single Haze level and still finite. **This is the round-31 clamp trap a second time** (`STATICINDEXBUDGET` had ceiling == default). **Grep any new knob's clamp against BOTH its default AND the value the launcher arms.** Also MEASURED and separate: `m_sun_sprite_have` is written in exactly **one** place and **never cleared** -- no flip reset, no level-change clear, no discontinuity clear -- so the clamp was the only release path there was. **Revert:** nothing to revert; the launcher value is unchanged and now takes effect. To restore round-28 behaviour set `RPCS3_REMIX_SUNSPRITEHOLD=3600`. | no |
| `RPCS3_REMIX_DRAWAUDIT` | `1` (on) | **Round 32 -- the only REMOVABLE occupant of `rest`, and `rest` is where the frame goes.** MEASURED in the round-31 log's worst window that actually submitted geometry (`frames=38`): `draw=50.88` ms/frame of a `frame_ms=53.03`, children `ui=2.53 mesh_create=0.14 tex_bind=1.11 uv=5.67 draw_instance=0.48` -> **`rest=40.96` ms/frame = 80.5% of `draw` and 77% of the whole frame**. `0` skips `audit_vertex_extent` (four separate passes over the decoded positions of every sub-draw) and `audit_world_extent` (walks the whole index list transforming each vertex into world space). Both are diagnostics. **Read `audit=` on `Remix timing:` FIRST** -- the timer and the knob ship together precisely so the size of the trade is stated before it is taken. What `0` costs: the vtx-spread census, the streak/wext census, and the **wext refusal gate**. Losing the gate is defensible on Haze *and only because it is measured*: `wext_refused=0` for the entire round-31 session, so it never fired and skipping it cannot change what is submitted. **On any other title, check `wext_refused` on `Remix live:` before setting 0.** `VTXREFUSE=1` with `DRAWAUDIT=0` is contradictory -- the audit never runs, so it can refuse nothing. **Revert:** `set "RPCS3_REMIX_DRAWAUDIT=1"`. | no |
| `RPCS3_REMIX_DEFERPREVONLY` | `1` (on) | **Round 32 -- narrows pre-anchor deferral to the branch that can actually pay off.** Two branches set `defer_candidate`: `anchor_prev` (last frame's anchor exists for this source, so this frame's is plausibly still to come) and `gauge_anchor_absent` (**no** anchor in this frame or the previous one). Holding an absent-source draw bets on an anchor appearing for a source that has not produced one in two consecutive frames. MEASURED size of that bet: `gauge_used=680753 gauge_prev=225700 gauge_absent=125966`, so absent is **125966 of the 351666 deferral population = 35.8%**; reproduced at 35.8% in the earlier session (`prev=328430 absent=182985`). This removes a third of the buffering cost before any judgement about whether the remainder pays. **INERT at the shipped config** -- `DEFERPREANCHOR=0` makes the whole path dead -- it is staged so that when deferral is re-tested the cheap third is already gone. Read `defer_absent_declined` against `defer_buffered` on `Remix live:`. `defer_absent_declined` being non-zero with `DEFERPREANCHOR=0` is **expected**: the count is taken at the election, which runs regardless. **Revert:** `set "RPCS3_REMIX_DEFERPREVONLY=0"` reproduces round 31's deferral population exactly. | no |
| `Remix timing:` gains `decode=`, `audit=`, `hash=`, `xform=`, `scene=[..]`, `meshes=` | -- | **Round 32 -- split `rest` (80.5% of `draw`) and name the level a sample came from.** The four new children are siblings of round 31's five, all inside `submit_subdraw`, so `rest` remains a saturating residual and shrinks by exactly what they capture. `decode` = index widen/rebase + `strip_to_list` + non-native primitive expansion + the per-vertex position decode with its `w_divide` + the index bounds check (five untimed O(vertex_count) loops that run on **every** sub-draw before any gate). `audit` = both geometry audits (see `DRAWAUDIT`). `hash` = static-index key build + per-triangle union accumulation + the union re-hash + skin-palette hashing -- **the source's own nominated suspect.** `xform` = `per_draw_transform` alone, ~1250 lines and fourteen exits; it is the only child whose cost is **invariant to vertex count**, which makes it the discriminator: if `xform` tracks draw count while `decode`/`hash`/`audit` track vertex count, the two are separable in one run. Implemented with a new `scope_us` RAII helper carrying an explicit `stop()`, because unlike round 31's five single-call children these are contiguous **spans** containing refusal `return`s (the two static-index dropped exits alone); an explicit `now_us()` pair would report a span that is hot *because* it is refusing draws as free. `stop()` keeps the diff to two inserted lines per span with no re-indentation. **`scene=` and `meshes=` fix a real analysis failure, not a nicety:** the round-31 log's three largest `frame_ms` samples were `62.21`, `60.79` and `54.23`, and **all three are boot/menu windows** (`draw=0.00`, `overlay`~26 ms, `ui_px`~2M) -- as are the three smallest. Nothing on the line said so, so worst-frame analysis was being done on the wrong frames. `scene=` is `m_active_camera.position`, freshly rewritten by `apply_gauge_anchor_camera` earlier in the same `flip()`. **Three facts to carry forward.** (a) **`other = frame_ms - flip`, so `other` CONTAINS `draw`.** They are NOT siblings and adding them double-counts. Correct decomposition: `frame_ms = flip + other`; `other` is a superset of `draw`; `draw = ui + mesh_create + tex_bind + uv + draw_instance + decode + audit + hash + xform + rest`. (b) **The ucode fingerprint/matcher lookup is NOT in `rest`** -- `fingerprint_for` and `fp_fingerprint_for` run in `end()` *before* the `draw` timer opens, so they land in `other`. Do not hunt for them in `rest`. (c) **`ui_px/frame` is not the `ui=` scope's pixel count.** One `m_pixels` counter is shared by `composite_ui_draw` (timed as `ui`, inside `draw`) and `composite_native_overlay` (timed as `overlay`, inside `flip`); a real log line reads `draw=0.00 (ui=0.00 ...)` with `ui_px/frame=2065645`. **Never divide `ui_px` by `ui`.** | no |
| `Remix static-index:` gains `resident=`, `evicted=`, `nomesh=` | -- | **Round 32 -- the second axis, without which a budget sweep cannot be judged.** Round 31 correctly refused to raise the budget past 8 because its cost model omitted **BLAS residency**: every rebuild mints a new union hash and orphans the old one, so doubling the budget doubles both the creation rate and the rate at which union BLASes go idle -- and `MESHCAP=0` means no ceiling. Nothing said how many `entries` currently hold a live mesh. The triple sums to `entries=` exactly (mirroring the `stale + dropped == deferred` invariant) using the submit path's own derivation: `resident` = `mesh_hash != 0` and present in `m_meshes`; `evicted` = `mesh_hash != 0` but reaped, so the next draw of that entry takes the **`dropped`** exit and renders nothing; `nomesh` = `mesh_hash == 0`, never committed a union (the second-skin / material-pointer case, also `dropped`). **So `dropped` is predicted by `evicted + nomesh`, and `resident` is what BLAS memory pays for.** One pass over a container MEASURED at 470 entries, once per stats window, not per frame. **Deliberately NOT global mesh counts:** `meshes_live`/`created`/`destroyed` are already on `Remix stats:` and `Remix live:`, and adding them a third time would be exactly the duplicate-counter defect this project already shipped once. **`STATICINDEXBUDGET` stays at 8 this round** -- the instrument ships first, per round 31's own blocking note. Pre-registered sweep reading is in `round33-inbox.md`. **Note the static-index counters are never reset per window**, so two lines from one run must be differenced; raw values across runs of different length are meaningless. | no |

### The five distance gates audited (round 31 asked for this)

`origin` is the transform's translation row and `dist` is `|translation - eye|`, so an absolute-world
draw with a correct identity transform necessarily reads ~`|camera|`. Verdicts, from source:

| knob | armed | LHS quantity | verdict |
| --- | --- | --- | --- |
| `SKYANCHOR` | 4 | **translation column** `transform.matrix[i][3]` | **DEAD for absolute-world geometry.** A correct identity transform reads ~2100 units on Haze against a threshold of 4. Still meaningful for camera-locked draws (Haze's dome reads 0.000). **A correct `centre` is computed 12 lines above in the same loop and the gate re-reads the raw translation instead** -- a one-line change would put it on the same footing as the other four. Counters `sky_anchor` / `sky_anchor_held` are on `Remix stats:` only, i.e. NOT readable while the game runs. |
| `VIEWMODELANCHOR` | 4 | genuine bbox centre (`VMANCHORGEO=1`) | **INERT.** All three arming lists (`VIEWMODELVP`, `VIEWMODELALBEDO`, `VMPAIRVP`) are empty, so `viewmodel_anchor_rejects` is never called. Would be sound if armed -- round 30 + `VMANCHORGEO` already fixed the artefact on this path. `vm_tagged=0` for the whole run confirms. |
| `VMPAIRMAXDIST` | 2 | genuine bbox centre | **INERT**, doubly so: requires a non-empty `VMPAIRVP`, which round 28 deliberately blanked. `vm_pair_far=0` means "the route is disarmed", **not** "the bound never fired" -- the launcher's own note reads it the second way. |
| `SUNCARDMINDIST` | 4 | genuine world bbox centre from decoded verts | **MEANINGFUL.** Immune to the artefact. Census-line floor only; pinned rows exempt. No counter for the gate itself, so a firing is observable only as a missing census row. |
| `FPCENSUSMAXDIST` | 4 | genuine bbox centre, `!measured` never admitted | **MEANINGFUL.** Census admission only; tags and refuses nothing. |

**And a live bug found in the audit: `SKYANCHOR=0` and `VIEWMODELANCHOR=0` do NOT disable those
gates.** `env_float` ends `return (std::isfinite(parsed) && parsed > 0.f) ? parsed : fallback;`, so `0`
fails the filter, returns the `-1` sentinel, and the accessor's ternary yields **4**. Both the source
comment ("0 is a legitimate value meaning do not require the anchor at all") and this document's own
`SKYANCHOR` row further up are **wrong against current bytes**. These two knobs are turnable upward
only, never off. The same filter affects `SUNSPRITEMAXSPAN`, `SUNCARDINT`, `SUNRADIANCE` and
`SUNANGLE`. `env_float_signed` does accept 0 and negatives.

### The sun's other half: it starts 42.5 degrees wrong on every boot

Separate from the clamp, and MEASURED from `bin\log\RPCS3.log`. On the land carrier the sprite is the
**only live rung** of the precedence chain `SUNMAP > SPRITE > SUNSKY > SUNTRACK card > SUNDIR`:
`SUNTRACK=0` disarms the card, and all 172 `Remix stats:` lines read
`sun_sky_examined=0 sun_sky_solved=0 sun_sky_refused=0` with zero `Remix sunmap:` lines and neither
`SKYEMISSIVE` albedo appearing anywhere in the log -- so the whole sky-sun subsystem never runs there.

The sprite can only solve while it is **fully inside NDC** (`constexpr f32 k_edge = 1.f - 1e-4f;`),
which is why looking up fixes the aim. MEASURED transition one flip apart at the same camera position:
`verdict=offscreen ndc=[...]..[0.8454 1.1697]` then `verdict=solved ndc=[...]..[0.87662 0.9463]` --
`hi[1]` crossed back inside +1. Pitching up is exactly what lowers `hi[1]`.

Consequence: `Remix sun: created travel=[-0.3659 -0.9022 0.2285]` at `0:02:56`, then thirteen
`Remix sun-submit:` lines through frame 18000 all reading `aimed=0 retargets=0` at that same travel,
then a single `src=sprite` retarget with **`moved=42.48 deg`**. So the sun is 42.5 degrees wrong from
boot until the player happens to look up, because the electing draw does not exist before then.
Zero-code mitigation: set `RPCS3_REMIX_SUNDIR` to the sprite-derived travel for this level,
`0.1223,-0.9444,-0.3051` (currently `-0.365,-0.9,0.228`). Trade: `SUNDIR` is global, so this is right
for the carrier and arbitrary elsewhere -- which is what `SUNMAP` exists to fix per level.

**Unresolved and INFERRED, for round 33:** because `update_sun_light()` returns early when no rung is
live, RPCS3's own light `0x4` *keeps* its last aim and **cannot** re-elect to `SUNDIR` mid-mission. So
a sun that visibly reverts is more likely the *runtime's own* fallback distant light:
`bin\rtx.conf` reads `rtx.fallbackLightMode = 1` (NoLightsPresent), which the fork re-evaluates every
frame in `prepareSceneData`, and the distant fallback is deliberately not recreated per frame. Setting
`rtx.fallbackLightMode = 0` is the zero-rebuild way to eliminate that candidate. **Not changed here --
`bin\rtx.conf` is the user's live tagging state.**


## Round 34 (2026-08-17)

**The headline: the viewmodel census had already emitted, in a log nobody read, and it names the bug.**
Six `Remix vmbasis:` lines are in `bin\log\RPCS3.log` from the run that ended 2026-08-17 09:37:23 --
round 33's own `RPCS3_REMIX_VMDEPTHOFFSET=2` play-test. Every doc block in the tree calling that census
"the measurement no census has ever emitted" was stale from that moment. **Grep the previous round's
log before writing a hypothesis.**

### The viewmodel: four faults, all MEASURED, and the tag was never one of them

| # | fault | evidence |
| --- | --- | --- |
| 1 | **The viewmodel divide is self-referential and parks every viewmodel draw at the world origin.** `m_frame_viewmodel_candidate.reference_inverse = inverse(folded)` is latched from the FIRST viewmodel-depth draw of the frame, and the comment beside the latch says the population is *expected* to share one view-projection. So `world = fused x reference_inverse` divides a matrix by (near enough) its own inverse. | `vmcam_applied=7960` of `vmcam_considered=7974` (99.8%). Census `pre` translation `[0.0555344 -0.244347 0.00686479]` on the vtx=3649 arms draw with `cam=[1774.14 -31.2479 1177.04]` -- **2129 units out**. Two of six lines have a bit-near-exact identity `pre`. |
| 2 | **`apply_viewmodel_basis` pivots about the EYE, on a premise that is false here.** Its own doc block says the operator "cannot rescale the geometry - only reorient it and its position relative to the eye". True; the second clause is the defect once the translation is not the eye. | `post` translation `[158.622 -59.112 2563.8]`. All six census lines move **2537..2564 units**. `eye_pre=[1698.05 -236.325 1262.76]` -> `eye_post=[1698.05 236.325 -1262.76]`. |
| 3 | **`!viewmodel_draw` disarms the tail-rescue ladder, and PROJSPLIT lives behind it.** Same gate round 28 caught deleting the arms through the `VMPAIRVP` route; still live, anchor `if (remix_rsx::tail_rescue_enabled() && remix_rsx::gauge_anchor_enabled() && !viewmodel_place`. | `const bool tail_split_ok = remix_rsx::split_view_projection(rescue_fused, tail_split);` sits inside `if (rescue_valid)`. |
| 4 | **`!m_active_viewmodel.valid` suppresses the ONLY VIEW_MODEL camera submission in the backend.** There is exactly one site setting `camera_info.type = REMIXAPI_CAMERA_TYPE_VIEW_MODEL`. `m_frame_viewmodel_candidate` is filled with `.valid/.archetype/.projection/.has_reference/.reference_inverse` and **no `.view`, no `.position`** -- it is a divisor, not a camera, and nothing submits it as one. Latching it does not replace the twin, it deletes it. | `vmcam_twin=3185 vmcam_real=3185` of `flips=7096` = **44.9%**, and the missing 55.1% are exactly the frames a viewmodel-depth draw latched the candidate, i.e. the frames the arms were on screen. **That is the dev menu's `VIEWMODEL Position: - Direction: - FOV: -`.** |

**And the world path is measurably CORRECT for these draws, which retires round 5's justification.**
Untagged `Remix picked:` lines for the same program in the same level read
`origin=[1780.78 -31.2362 1186.65]` against `cam=[1780.42 -31.3929 1186.57]` (albedo `0721D150DF278E7D`
vtx=2140, **0.40 units**) and `origin=[1780.75 -31.1536 1186.66]` against
`cam=[1780.42 -31.3948 1186.57]` (albedo `86885A0E60751491` vtx=4456, **0.44 units**), with
`basis=[0.998796 0.99919 1.00065]` and `[1.00293 1.00239 1.0015]`. Round 5's doc block says dividing the
viewmodel by the world camera "collapses the basis to (0.491, 0.002, 1.150)". Against current bytes that
same quantity reads **unity to three decimals** -- the f64 gauge, the anchor election and PROJSPLIT all
landed after round 5 measured it. **Do not re-derive the old number from the old comment; re-measure it.**

Independent corroboration already in the tree, unreconciled for ~11 rounds: the doc block on
`geometry_centre_in` says *"Haze's viewmodel programs use near-identity transforms (origin ~ [0,0,0])
with world-space vertices ... The picks meanwhile reported those same draws ~1.5 units from the eye -
because the picks measure the geometry."* That is fault 1 and fault 2 stated together, and
`apply_viewmodel_basis` was never reconciled with it.

| name | default | effect | in GUI |
| --- | --- | --- | --- |
| `RPCS3_REMIX_VMTAGONLY` | `0` (off = bit-for-bit round 33) | **Round 34 -- the actual viewmodel fix, and the bigger half.** Tagging a draw VIEW_MODEL does four things in `per_draw_transform` and only one is the tag. `1` introduces `viewmodel_place = viewmodel_draw && !viewmodel_tag_only()` and severs the three placement consumers: the divide by `m_active_viewmodel.reference_inverse` (fault 1), `defer_candidate = false`, the `REFUSED:noref`/`REFUSED:mode1` arm that **drops** the draw, and the `&& !viewmodel_draw` rescue gate (fault 3). `viewmodel_draw` itself is deliberately NOT cleared -- the submit site recomputes the verdict from the same inputs and must still tag, still run the basis operator and still emit the census. It also lifts the fault-4 camera suppression, because with the reference unused there is no reading left on which it has priority over anything. **Pre-registered refutation:** `vm_tagged` stays non-zero while `vmcam_considered` goes to **0** (the block is skipped entirely), `vmcam_twin` rises from 44.9% of flips toward `cam_resolved`, and the census's new `cdist_pre=` reads **~0.4** instead of ~2129. **If `cdist_pre` stays at ~2129, the relocation is NOT the viewmodel divide and this knob is the wrong lever -- say so and stop.** **Revert:** `set "RPCS3_REMIX_VMTAGONLY=0"`. | no |
| `RPCS3_REMIX_VMBASISPIVOT` | `0` (the eye = round 19..33 bit-for-bit); clamp `min(env, 2)`, launcher arms `1`, so every reachable value is inside the clamp | **Round 34 -- the pivot was the bug.** `0` = the anchor-frame eye. `1` = the draw's own geometry centroid under the **pre**-operator transform, the only pivot that makes `apply_viewmodel_basis`'s own claim literally true: the mesh rotates and does not move. Falls back to the eye if the centroid is unmeasurable, reported as `pivotsrc=3` -- never to the origin. `2` = the transform's translation column; **wrong on this title** (that column *is* the world origin) and provided only to separate "modelled about the origin" from "modelled about the centroid" in one run. Uses `geometry_centre_in`, both overloads of which are already `const`, so `apply_viewmodel_basis` stays `const`; it reports the pivot back through out-params rather than storing it. **Inert while `VMBASIS=0`** -- the operator returns before the pivot is read, so `pivotsrc=0` with `flip=0` means "did not run", not "chose the eye". **Revert:** `0`. | no |
| `RPCS3_REMIX_VMBASIS` (**launcher disarmed 6 -> 0**) | `0` | **Round 34 -- step 1 of two, honouring round 19's own discipline rather than restating it.** Round 19 armed `6` to fix "mirrored, upside down and displaced". All three symptoms had one cause and it was not handedness. An orientation guess taken against a mesh 2129 units from where it belongs measures nothing. **Step 1 (this build): `VMTAGONLY=1` + `VMBASISPIVOT=1` + `VMBASIS=0` -- one new variable, the tag.** The census still emits, with `pre == post`, and `cdist_pre=` is the number to read. **Step 2 (only after step 1 is judged): set 6 again.** With `VMBASISPIVOT=1` that is an in-place 180-degree rotation about the camera's right axis: `dbasis` goes non-zero and `dcentre` must stay ~0. **If the arms look right after step 1, do not do step 2.** **Revert:** `6` restores round 33. | no |
| `Remix vmbasis:` gains `tagonly=`, `pivotsrc=`, `pivot=[..]`, `cmeasured=`, `centre_pre=[..]`, `centre_post=[..]`, `cdist_pre=`, `cdist_post=`, `dcentre=`, `dtrans=`, `dbasis=` | -- | **Round 34 -- separating the two halves an orientation correction has to keep apart.** `det_pre`/`det_post` only say whether an axis was reversed, and `eye_pre`/`eye_post` are the object **origin**'s offset from the eye, which on this title is the world origin and therefore says nothing about where the mesh is. `dbasis` = `max abs(post3x3 - pre3x3)` and **must be non-zero or the operator did not run**. `dcentre` = `norm(centroid(post) - centroid(pre))` and **must be ~0 or the operator moved the mesh**. `dtrans` is kept beside it deliberately: at a centroid pivot `dtrans` is large while `dcentre` is ~0, and reading `dtrans` alone would look like a failure -- they disagree exactly when the mesh is not modelled about its own origin, which is the case that made this round necessary. `cdist_pre`/`cdist_post` are the centroid's distance from the eye, i.e. the one number that says whether the arms are in front of the player at all. **Pre-registered for pivot 1: `dcentre < 1e-3` with `dbasis ~2`, and `cdist_post` within a few hundredths of `cdist_pre`.** Appended as one contiguous block immediately before `frame=`/`line=` -- the only insertion point that cannot shift an existing specifier/argument pair. 17 specifiers, 17 arguments, paired position by position; verified in the binary with the NEW text present *and* the OLD tail `eye_post=[%.6g %.6g %.6g] frame=%llu line=%u/%u` absent. | no |
| `knobs:` on the boot line and `Remix live:` gain `vmbasispivot=` and `vmtagonly=` | -- | Round 34. Both print the **clamped** value, so a launcher armed outside its clamp shows up here. That is the diff round 31 (`STATICINDEXBUDGET` ceiling == default) and round 32 (`SUNSPRITEHOLD` armed 277x over ceiling) each lost a round to not making. | no |

### The deployed runtime DOES accept bit 26. The `viewmodel_mode()` comment saying otherwise is deleted.

MEASURED by disassembling `bin\remix\d3d9.dll` (sha256
`16a0b512f33ebb66a89ac703e75289d9e008558a13d2c9a6a5455b0be7c40858`, 240657408 bytes, fnv1a
`09653f484ec94dc0`). In `toRtDrawState` at RVA `0x001ED290`:

```
0x001ED309  mov   eax,[rdx+0x10]    ; remixapi_InstanceInfo::categoryFlags
0x001ED30C  bt    eax,0x1a          ; bit 26 = VIEW_MODEL
0x001ED312  mov   ebx,1             ; CameraType::ViewModel
0x001ED31C  and   ebx,4             ; else bit2 SKY -> Sky, else Main
0x001ED346  cmp   ebx,4 / cmove     ; deliberate Sky -> Main clamp
0x001ED34D  mov   [rbp+0x1f4],eax   ; DrawCallState::cameraType   <-- load-bearing
```

The `+0x10` operand is verified against `remix_c.h`'s layout (`sType@0 pNext@8 categoryFlags@0x10
mesh@0x18 transform@0x20`) by the transform rows loaded from `+0x20/0x30/0x40` immediately after. The
build also carries the API bump to **0.1000.1** (`mov eax,0x03E80001` at RVA `0x000EDCD1`), which
commit `6476faea` introduced. `toRtDrawState` is **byte-identical across all 15,328 bytes** to
`dxvk-remix-numos3\_output\d3d9.dll`; a whole-file diff is 2,282 bytes over 9 regions, eight of them
build stamps, the ninth `ImGui_ImplWin32_WndProcHandler`.

**The deleted claim and why it was wrong.** `viewmodel_mode()` argued from three true observations --
`remix_c.h` stops at `SMOOTH_NORMALS = 1 << 24`, `toRtCategories()` maps bits 0..24 by name, and the
`static_assert` on `InstanceCategories::Count == 25` -- to a false conclusion. **Bit 26 is deliberately
not an `InstanceCategories` member**; it is consumed by `categoryToCameraType()` and never reaches
`toRtCategories()`. `6476faea`'s own message says the `static_assert` is unchanged. Two obvious "tests"
for this are therefore non-tests: the assert count, and scanning for new identifier names (they are
preprocessor-only and reach neither DLL nor PDB).

An external draw can reach `CameraType::ViewModel` **only** through the category bit.
`remixapi_SetupCamera`'s `REMIXAPI_CAMERA_TYPE_VIEW_MODEL` registers matrices in a camera slot and
cannot make an instance reference it, and `submitExternalDraw` touches `ExternalDrawState::cameraType`
at exactly one site -- `getCamera(state.cameraType)` -- and never copies it into
`state.drawCall.cameraType`.

`createViewModelInstances` early-returns in order on (1) `!RtxOptions::ViewModel::enable()`, (2)
`!cameraManager.isCameraValid(CameraType::ViewModel)`, (3) `PlayerModel::enableInPrimarySpace()`.
**Only gate 3 masks the instance** (`m_vkInstance.mask = 0`), and it is `false` and absent from
`rtx.conf`. So **a tagged draw with no viewmodel camera renders as ordinary world geometry**, which is
exactly what round 20 measured on 123 tagged draws. **The launcher's claim that a mis-tagged draw is
removed from the world pass is retracted for this configuration.**

**Vendored constants reconciled.** `vendored_runtime_fnv1a` was `0x63656dfe3da8f069`, described as sha256
`36A5641AF4FA848E...`. **No file on this machine hashes to that** -- it was an earlier incremental link
of the same tree (same PDB GUID `228D2E7A-E41C-451F-8C80-D8B7ADD7E065`, lower Age), since overwritten.
Now `0x09653f484ec94dc0`; `vendored_runtime_size` was already correct at 240657408, which is why the
warning fired on the hash alone and read as noise. Verified in the binary: new quad present at file
offset 19237648, old quad **absent**. Also retracted: the provenance block's claim that the
`remix-plus-1.5.1` zip "predates the VIEW_MODEL category bit" -- the July CI build at
`bin\remix\d3d9.dll.bak-0729` already carries a bit-26 arm (`bt eax,0x1a` at RVA `0x001FAEB6`).

**PDB TRAP, worth more than the constant.** `bin\remix\d3d9.pdb` has GUID
`04C3AFFD-472B-4565-9AA0-08EBE452E439` Age 24 -- that is **`.bak-0729`'s** PDB, not the deployed DLL's,
stale by ~2.5 weeks and 24 link generations. Symbolizing a crash in the deployed runtime with it gives
**wrong function names**. The matching lineage is `dxvk-remix-numos3\_output\d3d9.pdb` (GUID
`228D2E7A...`, Age 49).

**Runtime swap, if the user wants it (their decision, not taken here).** `_output\d3d9.dll` is the same
code plus the `if (bd == NULL) return 0;` null-`bd` guard in `ImGui_ImplWin32_WndProcHandler`, which is
the fix for the Remix ImGui WndProc crash that affects every Remix game -- the currently deployed DLL
does **not** have it (`xor edi,edi` then falls through into the switch). No ABI, interface-slot or
version change; both are 0.1000.1. Costs: it comes from a **dirty** tree so the commit alone does not
reproduce it (identify by sha256 `F75A70D76B850829...`), `vendored_runtime_fnv1a` would need to become
`0x5c5478cd184f4b0a` instead, and `_output\d3d9.pdb` should be copied alongside it.

### Round 31's BLAS objection does NOT survive. `STATICINDEXBUDGET` 8 -> 32.

Round 31's words, `launch-haze-remix.cmd`: *"every rebuild mints a new BLAS handle and the superseded
one stays resident for MESHIDLE frames, and RPCS3_REMIX_MESHCAP=0 means there is no ceiling on the
resident set."* **Both halves are false**, read from
`dxvk-remix-numos3\src\dxvk\rtx_render\rtx_accel_manager.cpp` (clean since `ef3313e2`, 2026-06-04; all
eight BLAS anchor strings verified present in the deployed DLL; **no** conf file overrides any of the
five options in `bin\rtx.conf`, `bin\user.conf` or `bin\remix\rtx.conf`).

1. **A ~426-triangle union never gets its own BLAS.** `forceMergedBlas` includes
   `(!minimizeBlasMerging() && blasPrims < minPrimsInDynamicBLAS)` with the threshold
   `std::max(minPrimsInDynamicBLAS(), 100u)` = **1000** and a **strict `<`**. Measured triangles per
   union across three logged runs: **426.2 / 267.1 / 364.3**. `forceMergedBlas` overrides *every* clause
   of `requestDynamicBlas`, including `dynamicBlas != nullptr` ("keep the one you have"), and hands any
   existing dynamic BLAS back to `m_blasPool`.
2. **A new mesh HASH costs no BLAS at all.** `remixapi_CreateMesh` allocates
   `HOST_VISIBLE|HOST_CACHED` buffers ("Remix API mesh buffer") and does a map insert
   (`m_extMeshes.emplace`). A BLAS is only born on the **draw** path (`commitExternalGeometryToRT` ->
   `BlasEntry` -> `AccelManager`). MEASURED: **86.9 submitted draws per frame against
   `meshes_live=16133`**, so >=99.4% of live handles hold no BLAS on any frame.
3. **`numFramesToKeepBLAS` resolves to 1 here, so a BLAS is freed 2 frames (~70 ms) after its last
   draw.** `std::max(enablePreviousTLAS() ? 2u : 1u, numFramesToKeepBLAS())`, and `enablePreviousTLAS()`
   is **false** on this machine's `user.conf` (`upscalerType = 1` = DLSS + `enableRayReconstruction =
   True` -> ray reconstruction on; `integrateIndirectMode = 0` != ReSTIRGI). `PooledBlas::~PooledBlas`
   genuinely frees. **MESHIDLE governs host mesh handles, not BLAS memory.** Note `numFramesToKeepBLAS`
   is also aliased as `numFramesToKeepGeometryData()` **and** `numFramesToKeepMaterialTextures()` -- one
   option, three jobs, not obvious from the name.
4. **The merged pool is recycled by BUFFER SIZE, never by mesh hash.** Pool size is bounded by buckets
   alive in the last 2 frames. What a new hash *does* cost is a changed bucket content hash, so
   `canSkipBuild` fails and that bucket takes a full BUILD instead of an UPDATE -- **per-frame and
   transient, not cumulative.**
5. **No cap a higher rebuild rate can blow.** One `vkCmdBuildAccelerationStructuresKHR` for everything
   accumulated; scratch memory grows on demand and is released each frame; `maxPrimsInMergedBLAS` is a
   **per-mesh** test that routes a >50000-prim mesh to its own BLAS, and `BlasBucket::tryAddInstance`
   never rejects on triangle count. The only hard cap is RPCS3-side (`s_max_indices_per_mesh` = 1048576
   = 349,525 tris), and at 426 tris/union you are ~800x away.

**Real cost of a higher budget, in order:** GPU merged-bucket full rebuilds (transient; watch
`frame_ms`); CPU **4.54 us per mesh create** = `mesh_create=0.26` / `mesh_creates/frame=57.3` = **0.74%
of a 35.22 ms frame**; ~20 KB host RAM per resident union (~24 MB for 1200). **VRAM growth: none on this
evidence.**

**Why 32 and not 64:** 32 leaves `peak` readable, and the entire 8->64 range is bounded at +0.25 ms of
frame time. Demand in the plant burst was **~57 deferrals/frame against 8 slots** (census lines 43->44,
2.01 s apart: `deferred +3202` over `rebuilds +324` = 9.88/rebuild, ~56 frames at the measured 27.9 fps).
Eviction *is* recoverable -- `if (static_entry->dirty || !active_valid)` re-enters the rebuild path --
but recovery must win a budget slot, so restoring 1168 evicted unions at 8/frame is **>=146 frames ~
5.1 s** while `MESHIDLE=600` re-evicts anything out of view for 10 s. A treadmill the current budget
cannot win.

**Pre-registered reading, all DIFFERENCED WITHIN THE NEW RUN** (first to last census line; the rate
varies **1.03..4.34** across runs, so raw cross-run values are meaningless):
- SUCCESS 1: **`peak < 32`**.
- SUCCESS 2: `dropped`/`rebuild` falls below **3.661** (this run's differenced value). The widely-quoted
  **1.17 was an undifferenced `2020/1728`**; that run's correct differenced value is **1.145**.
- SUCCESS 3: `resident`/`entries` on the **FINAL** line rises above **63.8%** (257/403).
  **`resident`/`evicted`/`nomesh` are point-in-time GAUGES, not cumulative counters -- read the last
  line, never difference them.**
- REGRESSION 1: `frame_ms` rises >1.0 ms while `mesh_create` stays under 1.0 ms. That is the
  merged-bucket rebuild term, and it is the only real cost.
- REGRESSION 2: per-create cost rises above ~6 us.
- **RETIRED:** round 32 registered *"if `dropped` falls while `evicted` RISES, the budget is churning
  BLASes faster than they are reaped and 16 is a regression."* **Withdrawn.** `evicted` counts
  RPCS3-side handles reaped by MESHIDLE and a handle carries no BLAS unless drawn that frame. Rising
  `evicted` with falling `dropped` is the **expected** shape. The regression signal is `frame_ms`.

**Invariants checked:** `stale + dropped == deferred` **HOLDS** on absolutes and on every differenced
pair. `resident + evicted + nomesh == entries` **HOLDS** -- but it is true **by construction**, all four
coming from one pass over `m_static_indices` with a three-way if/else, so it is a self-consistency check
on that loop, not independent corroboration.

**Most alarming number found, and it is not in the current run:** a 309-line run ended
**`resident=12 evicted=1168`** -- 1.0% of static-index entries still held a live mesh handle. Another
ended `resident=3 evicted=695`. Since `evicted` means the next draw of that entry takes the `dropped`
exit and renders nothing, that is the invisible-walls symptom in numeric form, and it is a
**steady-state decay, not a load transient.** Lever flagged but NOT taken: a longer idle window scoped
to static-index **union** meshes only would relieve the budget more cheaply than raising it, but that is
a code change, not a knob.

### `Use RSX Backface Culling` -- the user changed the file the game does not read

MEASURED: `bin\config\config.yml` reads **`true`**; `bin\config\custom_configs\config_BLUS30094.yml`
reads **`false`**; and `bin\log\RPCS3.log` line 166 reads
`Emulator::BootGame: ... config_mode='custom config'`. **The custom config wins, so the effective value
is OFF and the user's change never took effect.** There is no env escape hatch either --
`cull_from_rsx()` is `env || g_cfg.video.remix.cull_from_rsx`, the env var can only force ON, and
`RPCS3_REMIX_CULL` is absent from the launcher. Consequence:
`instance.doubleSided = (cull_from_rsx() && cull_face_enabled()) ? 0u : 1u` submits **every instance
doubleSided=1**; nothing is ever single-sided. **Advice: leave it off** -- round 33 recorded that
turning it on made the plant far worse (walls/floor went black), and single-sided submission drops the
inward-facing faces this title needs. The white walls are the static-index budget, not culling.

Other values in the custom config, for the record: `Live Mesh Cap: 0`, `Mesh Idle Frames: 300` (the
launcher's `MESHIDLE=600` overrides it -- `mesh_idle_frames()` is `env != umax ? env : g_cfg...`),
`Texture Idle Frames: 21600`, `Texture Uploads Per Frame: 192`, `Camera Hold Frames: 300`,
`Resolution Scale: 150`, `Shader Precision: Low`.

### `audit` IS the largest child of `draw` — but `DRAWAUDIT=0` is NOT safe, and was reverted

`round33-inbox.md` registered: *"If `audit` is the largest new child: the fix is
`RPCS3_REMIX_DRAWAUDIT=0`, not an optimisation."* MEASURED, last two `Remix timing:` windows of the
2026-08-17 run:

```
frame_ms=35.22 | draw=33.20 (ui=2.28 mesh_create=0.26 tex_bind=0.53 uv=3.48 draw_instance=0.43
                             decode=6.35 audit=11.35 hash=3.76 xform=0.19 rest=4.58)
frame_ms=33.57 | draw=31.51 (... decode=6.10 audit=11.03 hash=3.64 xform=0.12 rest=3.78)
```

`audit` **is** the largest new child at **11.35 ms = 32.2% of the whole frame and 34.2% of `draw`**, and
`rest` has fallen to 4.58 ms (13%) -- round 31's 40.96 ms residual is fully accounted, and **`rest` is
not `0.00` beside a large `draw`**, so round 33's saturation/double-counting tripwire did not trip.
`decode` is second at 6.35 ms and is the next target if more is wanted.

**`DRAWAUDIT=0` was armed, then REVERTED to `1`, because the safety argument is wrong.** The argument
everyone has used — including round 32's own knob row and this round's first draft — is *"`wext_refused=0`
for the whole session, so the gate never fired and skipping the audit cannot change what is submitted."*
A review of the round-34 diff found **`wext` is not the only gate the audit feeds.** `m_streak_measured`
is written **only** inside `audit_world_extent`, so with the audit off it stays `false` for the entire
session, and two further consumers change behaviour:

- The `SKIPEXTENTVP` refusal gate reads `&& m_streak_measured` and therefore **stops firing**. The
  launcher arms `SKIPEXTENTVP=57A12323F22F4988` with `SKIPEXTENTMIN=128` — and `57A12323F22F4988` is
  **one of the three near-depth viewmodel programs**. Disabling that refusal changes what is submitted
  for the exact population priority 1 is measuring.
- `extent_plausible = !m_streak_measured || ...` becomes **unconditionally true**, so
  `GUESTLIGHTEXTENT` stops rejecting anything (injection is unaffected at `GUESTLIGHTAUTO=0`, but the
  lightcand census widens and pays bounding-box walks it previously skipped — a perf knob partly
  defeating itself).

`RemixTransforms.h`'s "What 0 costs" enumerates only the vtx-spread census, the streak/wext census and
the wext refusal gate. **Neither of the above is named, and the doc should be corrected before anyone
arms 0 again.** Arming `DRAWAUDIT=0` in the same run as the viewmodel change would confound the one
measurement that matters, so frame time waits a round. **Round 35: take the ~11 ms then, and either arm
`DRAWAUDIT=0` with `SKIPEXTENTVP` blanked so the interaction is explicit, or attack `decode` (6.35 ms) by
caching decoded positions per (source pointer, count) as round 33 pre-registered.**

**General lesson: "counter X is 0, therefore removing the code that computes X is free" only holds if X
is the code's ONLY consumer.** Grep every reader of every intermediate the block writes, not just the
counter named in the knob's doc.

### Task 4: the identity-transform props are CORRECT. The premise was the artefact.

All three picks are verdict **(C) absolute-world-space static geometry**, for which an identity instance
transform is right and the mesh's own vertex coordinates place it at ~1800. **Nothing is riding the
camera.**

- **`identity-bypass` has ELEVEN producers, not one.** `world_identity_match` ORs
  `world_identity_vp_matches` with `world_identity_pair` .. `pair5`, `fp_pair`, and `opaque_fp_pair` ..
  `opaque_fp_pair4`. `WORLDIDENTITYVP`/`STATICINDEXVP` carry the same six hashes and **none of the three
  picks is in either**, but the launcher arms ten more pair knobs and all three picked VPs appear among
  them. `a7505f7ad3a86838`/`fp=a91b57ce21bf0082` matches `WORLDIDENTITYOPAQUEFPPAIR4` exactly, with
  `depth_write=1 blend=0` satisfying `opaque_depth_draw`. **`origin` is exactly `[0 0 0]`, not an
  epsilon -- the signature of an assignment, not a division.**
- **(A) view-space vertices is REFUTED from the raw bounds:**
  `raw=[1788.75 -24.9408 1204.12]..[1808.94 -24.9388 1212.04]` -- absolute world, magnitude ~1800.
- **Not camera-locked, MEASURED:** only **7 distinct raw bboxes across 321 draws spanning frames
  3596..55560**, in two fixed world clusters. The dominant bbox is **bit-identical across 135 draws
  covering frames 9006..19320**, during which the camera moved 3.23 in X and 2.29 in Z. Control that the
  metric resolves motion: a sibling albedo shows **129 distinct bboxes over 1845 rows**.
- **The `a7505f7ad3a86838` pin discards essentially nothing:** `Remix worldid-draw:` reads
  `basis_delta=1.13687e-13 translation=3.72529e-09`, and **251 of 321 traced draws (78.2%)** discard a
  translation < 1e-6. `WORLDIDMAXT=32` already exempts the two largest remaining buckets, leaving
  **8.1%** residual. **Do not change these three.**
- For `1f9342de47afb400` and `d0b6a471bb2d463b` **no knob fires** (the armed albedo/fp keys do not match
  the picked ones), so the division genuinely ran and produced a residue of **2.18e-11** against entries
  of order 1e3 -- agreement to ~14 significant digits. INFERRED: there is no object->world matrix in the
  chain; `fused` **is** `V x P`, the same matrix the anchor holds. `ad7ce9d672a0bf6b` is itself on
  `WORLDIDENTITYVP`, i.e. another declared world-space program reading the same constants.

**NEW DEFECT found while doing this, and it is the real one: the world divide is BIMODAL on the same
mesh, and the wrong mode's residue is of CAMERA magnitude.** For (`d0b6a471bb2d463b`,
`27159433F0E63031`), all on the same `surf=014D0000` and the same raw bbox: 355 rows (79.2%) discard
<1e-6, and **76 rows (17.0%) discard >1000**. Pick-line `|origin - cam|` for that mode clusters at
2094.94 .. 2143.41 against `|cam| = 2106.8` -- agreement within 2%. `max_translation` is
`max abs(matrix[row][3])`, so this is a camera-coordinate residue, i.e. `fused x ref^-1` where `ref` did
not match the draw's `fused`. Supporting: `camclip=512x288` vs the draws' `1024x576` in most rows, and
`camage` up to **702 frames**. **Pre-registered:** if that bucket does not fall from **76/448 = 17.0%**,
the reference-mismatch inference is wrong. The tree already names the shape: *"Haze submits absolute
world vertices through several passes which share a vertex program. A VP-wide transform rule cannot
distinguish the gameplay draw from a camera-relative copy."*

### The warping light fixture: 612 albedos on one VP, and `eye_dist` separates it cleanly

`vp=830d7d1b9681c475` draws **612 distinct albedos across 35,575 `Remix fpcandidate:` rows**, so any
VP-keyed fix over-matches by ~600x. But **(vp, albedo) does isolate the fixture**, and the discriminator
is the `eye_dist` **floor**:

| class | albedos | n | `eye_dist` | vtx | ext |
| --- | --- | --- | --- | --- | --- |
| ARMS, rigid | `1CDD5249E6504F13`, `CC6008D0E9E98972` | 718, 65 | **0.25 .. 1.10** | 244-3800 | 0.061-0.830 |
| ARMS, contaminated -- **exclude from any albedo rule** | `0721D150DF278E7D`, `86885A0E60751491` | 2928, 808 | 0.09 .. **2158.85** | 40-4456 | 0.070-20.203 |
| FIXTURE | `3928B58DC87F4702`, `5BC48BBB303398E3`, `60CCC35DE6ED42B7`, `08865794B7B59AEC`, `57720018CA20525D`, `5A55210D7739C716`, `38C858E6DC48E488` | 22..335 each | **floor never below 1.81** | 168-590 | 0.272-2.770 |
| translucent, distinct (`depth_write=0 blend=1`, `sampled=0x7`, `fp=479890ff55f1d96e`) | `C61753D31FB96507` | 1613 | 1.58 .. 3.77 | 59 fixed | 5.067-8.473 |

No overlap between the rigid-arm ceiling (1.10) and the fixture floor (1.81). **The warp is NOT in the
vertex data:** `corr(ext, eye_dist)` is `abs(r) <= 0.22` for all four fixture albedos tested, and
`5BC48BBB303398E3` holds `ext` to 0.331..0.333 (0.6%) over 20 draws at a fixed 180 vertices. The
distortion is in the recovered basis -- picks show anisotropic deviation up to **+/-0.28%**
(`0.997445 0.999418` .. `1.00279 0.999904`), varying frame to frame, with `ref=anchor_prev` on 51/65
picks. **Pre-registered test with a baseline, for whoever attacks it:** measure `basis` spread on picks
of (`830d7d1b9681c475`, `5BC48BBB303398E3`), whose raw `ext` is constant to 0.6% so any deviation is
pure gauge error. **Baseline +/-0.28%.** If a freshness fix does not bring it below **+/-0.05%** and does
not lower `gauge_prev`, the anisotropy is in the anchor program's own matrix rather than its age -- stop
and attack `ad7ce9d672a0bf6b` instead.

### Method notes earned this round

- **The log from the last play-test is a primary source and it was not read.** Round 33 set
  `VMDEPTHOFFSET=2`, the user played it, and `bin\log\RPCS3.log` held `vm_tagged=7960`, six
  `Remix vmbasis:` lines with the full pre/post matrices, `vmcam_applied=7960/7974` and
  `vmcam_twin=3185/7096` -- four of this round's five findings, sitting in a 7 MB file.
- **A doc comment that says "no census has ever emitted this" is a claim with an expiry date.** Three
  separate blocks carried it after it became false. When shipping an instrument, the same commit should
  say how to tell it has fired.
- **`FileStream` tail + `-Max N` stops at the FIRST N matches in the window, not the last.** I read
  `vm_tagged=0` and `vmcam_twin=0` off a 40 MB tail and they were from an earlier session; a 4 MB tail
  gave `flips=7096 vm_tagged=7960 vmcam_twin=3185`. **When tailing an append-mode log, shrink the window
  until the match count is under the cap, then take the last line.**
- **Two counters with the same name on two different lines are not necessarily a duplicate defect.**
  `vm_tagged` is on both `Remix stats:` (RPCS3.log) and `Remix live:` (dump) and they agreed exactly at
  7960. `Remix live:` does **not** appear in RPCS3.log at all (0 hits vs 79 for `Remix stats:`), so
  grepping the emulator log for a live-only field silently finds nothing.
- **"Strictly more correct and keeps priority" needs a consumer.** Fault 4 is a suppression guarding a
  priority over a structure that no code path can use. Before writing a precedence rule, check that the
  higher-priority object is actually consumable by the same sink.
- **When splitting one flag into two, ask of EACH consumer whether it had a second reason.**
  `viewmodel_draw` drove four things and three moved to `viewmodel_place`. The fourth — `defer_candidate
  = false` — looks like placement and is documented as placement, but it has a second, independent
  reason: `flush_deferred_for_anchor` recomputes `entry.info.transform = to_remix_transform(world)` and
  does **not** re-run `apply_viewmodel_basis`, so a deferred viewmodel draw would silently lose its basis
  correction and be placed differently depending on whether it was buffered. **It deliberately stays on
  `viewmodel_draw`.** Caught in review of this round's own diff, not by testing — it is unreachable at
  the shipped `DEFERPREANCHOR=0`, so no run would have found it.
- **Two review findings that are NOT round 34's and are still open, both from the uncommitted round-32
  work.** (a) The `xform` timing span **encloses `deferred_instance`**: `per_draw_transform` calls
  `capture_gauge_anchor` -> `write_gauge_anchor` -> `flush_deferred_for_anchor` -> `submit_deferred`,
  which adds to `m_timing.deferred_instance` from inside the `xform` scope. `rest` stays a valid residual
  (`deferred_instance` is not in the subtraction), but round 32's claim that `xform` is "the only child
  whose cost is INVARIANT to vertex count, which makes it the discriminator" is false the moment
  `DEFERPREANCHOR=1` — precisely the run it was staged for. Inert today because the flush early-returns
  on an empty buffer. (b) **`SUNSPRITEHOLD` is still armed at `999999` against the new ceiling of
  `216000`**, so the knobs line will report `sunspritehold=216000` and reproduce the exact
  reported-vs-armed mismatch round 32's fix was written to end. One launcher edit.
- **A "0.4 units from the eye" anchor distance and a "2129 units from the eye" transform translation are
  the same draw.** The first is `geometry_centre_in` (VMANCHORGEO), the second is `matrix[i][3]`. They
  are different quantities and conflating them is what let the eye pivot survive fifteen rounds. The new
  `cdist_pre=`/`centre_pre=` fields print both halves on one line so it cannot happen again.

## Round 35 (2026-08-17)

Deployed: `bin\rpcs3.exe` = `bin\rpcs3-next.exe` (hashes in `round36-inbox.md`). Runtime
`bin\remix\d3d9.dll` **UNCHANGED at `16A0B512F33EBB66`** — nothing was deployed into `bin\remix\`.

### `apply_viewmodel_basis` is CORRECT. The "120-degree yaw" was a row-vs-column projection error.

The round-35 brief reported that `VMBASIS=6` "produced a 120-degree yaw about the up axis" where a clean
180-degree rotation about the camera's right axis was wanted, and asked for the reflection composition to
be rewritten. **Refuted.** The brief projected the instance transform's **ROWS** onto the camera axes.
`remixapi_Transform` is **column-vector** — `p_world = M * p_object`, and `matrix[i][3]` is the
translation, which is what the `VMBASISPIVOT=2` branch and the census's `dtrans=` both read — so the
object's world-space X/Y/Z axes are its **COLUMNS**. Row `i` of a non-symmetric rotation is a coefficient
vector, not an axis.

MEASURED, re-projected by script over all 10 `flip=6` lines of the 2026-08-17 12:36 run in
`bin\remix_dump.log` (`VMTAGONLY=1 VMBASISPIVOT=1 VMBASIS=6`):

| line | COLUMN dots post (X.right Y.up Z.fwd) | ROW dots post (as the brief read them) |
| --- | --- | --- |
| `830d7d1b9681c475` / `0721D150DF278E7D` vtx=2140 frame=3945 | **+0.99945 +0.99946 +0.99891** | -0.50091 +0.99984 -0.50101 |
| `830d7d1b9681c475` / `86885A0E60751491` vtx=3649 frame=3945 | **+0.99751 +0.99986 +0.99752** | -0.53236 +0.99976 -0.53211 |
| `57a12323f22f4988` / `804C702EC7A87C58` vtx=8 frame=3945 | **+0.99948 +0.99987 +0.99948** | -0.44552 +0.99976 -0.44529 |
| `9f591b6a6b825612` / `CC6008D0E9E98972` vtx=4 frame=6640 (the visor) | **+0.91169 +0.92747 +0.96838** | +0.87411 +0.94216 +0.91478 |

The row reading reproduces the brief's numbers to five decimals, which is what identifies the artefact.
The column reading **is the pass mark the brief itself asked for** — X.right, Y.up, Z.fwd all at +1 — and
it holds on *every* flip=6 line, so the premise that "one program composes correctly and the other yaws"
is also an artefact: the visor is not better behaved, it is only less perfectly camera-aligned to begin
with (its own `pre` reads +0.912 / -0.927 / -0.968).

Everything else the brief pre-registered for the placement already passed and is unchanged:
`cdist_pre=0.429486` vs `cdist_post=0.429487`, `dcentre=1.45e-06`, `dbasis=1.99947`, `pivotsrc=1`.

**Shipped: `dotpre=[...] dotpost=[...]` on the `Remix vmbasis:` line** (`RemixGSRender.cpp`,
`axis_dot` lambda in `report_viewmodel_basis_census`) — the COLUMN direction cosines, normalised on both
sides so a scaled basis still reads 1. Format audited position by position: **61 specifiers against 61
top-level arguments, zero type mismatches.**

### The target, not the operator, is what is undecided — and the eight flips are now a lookup table

MEASURED: `pre` is uniformly `(right, -up, -fwd)` on all 24 `tagonly=1` census rows, so every flip has a
known `dotpost` reading and a known determinant (even flips preserve it, odd flips mirror):

| flip | dotpost (X.right Y.up Z.fwd) | det | flip | dotpost | det |
| --- | --- | --- | --- | --- | --- |
| 0 | (+1 -1 -1) | +1 | 4 | (+1 -1 +1) | **-1 MIRROR** |
| 1 | (-1 -1 -1) | -1 | 5 | (-1 -1 +1) | +1 |
| 2 | (+1 +1 -1) | -1 | 6 | (+1 +1 +1) | +1 (round 34 step 2) |
| 3 | (-1 +1 -1) | +1 | 7 | (-1 +1 +1) | -1 |

The user's verdict on flip=6 was **"arms are facing the right way, just upside down and a little high"**.
Keeping the forward axis, negating the up axis and adding no mirror leaves exactly one flip: **5**, armed
this round. This is INFERRED from a verbal report, not measured, which is why `dotpost=` ships beside it.

**Why not 4, which the round-19 launcher note recommends for exactly this symptom:** flip 4 has det -1,
i.e. it mirrors the arms. `det_pre` reads +1.00001 on every census line, and the legs and the whole world
come through the same world divide and are not mirrored, so that divide preserves handedness and the
correction must be a proper rotation. Round 19's "try 4" predates that measurement. Superseded.

### "A little high" is REAL, is NOT caused by the rotation, and gets no offset knob this round

MEASURED, `centre_pre` minus `m_active_camera.position` over all 24 `tagonly=1` census rows — **every**
near-depth viewmodel draw's centroid sits ABOVE the eye:

| (vp, albedo) | dY(centre - eye) | horizontal dXZ |
| --- | --- | --- |
| `830d7d1b9681c475` / `86885A0E60751491` | +0.158 .. +0.164 | 0.43 |
| `830d7d1b9681c475` / `0721D150DF278E7D` | +0.374 .. +0.384 | 0.21 |
| `57a12323f22f4988` / `804C702EC7A87C58` | +0.098 .. +0.108 | 0.63 |
| `f39f504649b6f442` / `86885A0E60751491` | +0.373 .. +0.382 | 0.48 |
| `f39f504649b6f442` / `0721D150DF278E7D` | +0.599 .. +0.599 | 0.11 |
| `15ad612980aca110` / `804C702EC7A87C58` | +0.932 | 0.48 |
| `9f591b6a6b825612` / `CC6008D0E9E98972` | +0.321 .. +0.413 | 0.13 |

**It is not the operator.** The same albedo reads +0.3844 at flip=0 (frame 2512) and +0.3754 at flip=6
(frame 3945) — a 0.009 difference across a different camera pose — and `dcentre <= 6.5e-06` on every
line says directly that the basis operator does not move the centroid. So "high" is a property of the
recovered world transform, not of `VMBASIS`.

**It is not yet proven wrong, either.** INFERRED: an upside-down arm rig puts the hands at the top of the
frame, which reads as "high" independently of any translation. Re-judge it after the orientation is
settled; do NOT add an offset knob before then, or the knob will be tuned to compensate for a rotation.

### The player's body: the FRAGMENT program is the discriminator, and (vp, albedo) is NOT

The brief directed the legs work at the (vp, albedo) pair route. **That route over-matches here.**
MEASURED from the user's own Ctrl+Click picks in `bin\remix_dump.log`:

```
legs  vp=f39f504649b6f442 fp=b64dc06f79b8b42b albedo=0721D150DF278E7D vtx=952  viewmodel=0
arms  vp=f39f504649b6f442 fp=d6f00cddfb5c6e0a albedo=0721D150DF278E7D vtx=2140 viewmodel=1
```

Same vp, same albedo. A (vp, albedo) pin takes the arms with the legs — round 20's failure mode again.

MEASURED over every `Remix fpcandidate:` row in the 757 MB dump: `b64dc06f79b8b42b` appears **1381
times, all 1381 at vtx=952**, across 12 albedos (the skin variants). Not one row is anything else. So
`(f39f504649b6f442, b64dc06f79b8b42b)` is the player body and nothing else.

This does **not** contradict round 27's "the fragment program does not partition the rig" — that measured
that the weapon and the arms share `d6f00cddfb5c6e0a`, which is still true. It partitions the BODY from
the first-person rig, which is a different cut.

Shipped as `RPCS3_REMIX_HIDEPAIRVP` + `RPCS3_REMIX_HIDEPAIRFP` + `RPCS3_REMIX_HIDEPAIRMODE`, armed at
that pair with mode 1. Counter `cat_hidepair` on `Remix stats:` (kept separate from `cat_hidden`, which
already read 2617 from the albedo list). Format audited: **256 specifiers against 256 arguments.**
Empty either half disarms the route. Clamp `min(env, 2)`, default 1 — ceiling above default.

### What the VIEW_MODEL tag actually buys on this runtime, measured from the fork's source

Read out of `dxvk-remix-numos3` (the fork that built the deployed DLL; every cited file has an mtime
before the deploy timestamp, so this is INFERRED-to-be-the-running-code rather than disassembled):

- **There is no per-instance "casts shadows" flag anywhere in the runtime.** A search for
  `castsShadow|castShadow|noShadow|shadowCaster|disableShadow` over `rtx_render/` and `shaders/rtx/`
  returns zero hits. Shadow visibility is decided entirely by the 8-bit
  `VkAccelerationStructureInstanceKHR::mask` and by which TLAS the instance lands in.
- **`InstanceCategories::Hidden` is not a shadow-only lever** — it sets `mask = 0`, removing the instance
  from primary rays too, and the BLAS is then skipped.
- **A VIEW_MODEL-tagged instance stops casting onto world geometry only once the duplicate exists.**
  `primaryRayMaskToObjectMask` strips `OBJECT_MASK_ALL_VIEWMODEL` from a non-viewmodel hit's shadow mask,
  so world surfaces can never be shadowed by view-model geometry. But the duplicate is created only past
  four gates in `createViewModelInstances`, and the second is `!isCameraValid(CameraType::ViewModel)`,
  which requires a VIEW_MODEL camera submitted **that frame**.
- **VIEW_MODEL tagging does nothing at all about clipping.** The instance is ordinary world-space geometry
  in the same opaque TLAS and the primary ray mask includes `OBJECT_MASK_VIEWMODEL`, so view model and
  world resolve against each other. The only mitigation the runtime offers is shrinking it
  (`rtx.viewModel.scale`, default 1.0, "Minimize to prevent clipping").
- **The only per-instance "visible but casts no shadow" route that exists** is
  `THIRD_PERSON_PLAYER_MODEL` (`OBJECT_MASK_PLAYER_MODEL`, bit 6, not in `OBJECT_MASK_ALL`) plus
  `rtx.playerModel.enableInPrimarySpace = True` and `rtx.playerModel.enablePrimaryShadows = False`.
  Both lines or it is worse than nothing, and `enableInPrimarySpace = True` also masks every VIEW_MODEL
  candidate to zero. That is `HIDEPAIRMODE=2`.
- **`rtx.worldSpaceUiTextures` is LIVE on this path** (its consumer is the fork's `rtx_fork_submit.cpp`,
  not `src/d3d9/`) but it does **not** hide anything from shadow rays — it forces unlit emissive. It is
  not a solution to "the helmet must not cast big shadows".
- **A leading `-` in a `rtx.conf` hash list is a first-class veto, not a two's-complement number.**
  `util_hash_set_layer.h` strips it with `.substr(1)` and inserts into `m_negatives`. So
  `-0x0721D150DF278E7D` in `rtx.worldSpaceUiTextures` means "remove this hash from the list", and since
  nothing else contributes it, it currently subtracts nothing. (This only holds for `HashSet` options;
  `HashVector` options have no `-` branch and there a leading `-` really would wrap around.)
- **Correction to a standing backend comment:** `classify_draw` says "IGNORE is a no-op on the API draw
  path". The fork has since added `externalDrawShouldSkip` (`rtx_fork_submit.cpp`), called from
  `rtx_scene_manager.cpp`, which drops an `Ignore`-tagged submesh before instance creation. INFERRED to be
  in the deployed DLL from file mtimes (all before the deploy), NOT confirmed by disassembly.

### The helmet-as-UI request: the mechanism is a one-line launcher edit, the TARGET is unidentified

`UIFORCEVP` is the right mechanism and it is stronger than any category flag: a forced draw returns from
`RemixGSRender.cpp`'s screen-space block **before** `per_draw_transform` and `submit_subdraw`, so there is
no mesh, no instance, no material and no BLAS — no shadow, no lighting, no world clipping, and no depth of
any kind (the compositor has no depth; `grep -n depth RemixCompositor.*` returns zero hits). It is a
**comma list, bound 8**, currently holding one hash (`2F64C2F8FFD6ADD1`, `ui_forced=98681`), so seven
slots are free and adding the helmet needs **no rebuild**. One guard to know:
`!rsx::method_registers.depth_write_enabled()` — a listed program that writes depth is not forced.

**But `9f591b6a6b825612`, labelled "the visor" since round 19, is not a visor.** MEASURED over every
`Remix fpcandidate:` row: it has exactly two fragment programs and neither is a helmet overlay.

| fp | albedo | vtx | extent | eye_dist | rows |
| --- | --- | --- | --- | --- | --- |
| `cbede4eb45f0fd25` | `C61753D31FB96507` (+8 more) | 59 | 5.15 .. 11.02 | 1.56 .. 3.77 | 1309 |
| `7faad0c4437ae7c1` | `CC6008D0E9E98972` | 4 | **0.02 .. 0.03** | 0.20 .. 0.56 | 56 |

A 7-unit object two metres away is not a visor at the eye, and a 3-centimetre quad is not one either.
Round 19's label came from the near-depth census (`scale_z=0.00125`), never from a pick. **The helmet has
never been identified**, and pinning `UIFORCEVP` at a program with 34 (albedo, vtx) signatures on a guess
is exactly what deleted world geometry in round 20. One Ctrl+Click on the helmet/visor edge unblocks it.

### Frame time: `DRAWAUDIT=0` deliberately NOT taken, and why

`audit=7.60 ms` of `draw=23.34 ms` of `frame_ms=31.25` in the last run (24.3% of the frame, down from
round 34's 11.35/35.22 = 32.2% — the same knobs, a different scene, which is why the ratio is the
comparable number and not the milliseconds). Not armed: round 34 proved `DRAWAUDIT=0` also stops the
`SKIPEXTENTVP` refusal gate firing (it reads `&& m_streak_measured`, written only inside
`audit_world_extent`), and `SKIPEXTENTVP` is armed on `57A12323F22F4988` — **one of the near-depth
viewmodel programs this round is measuring**. Decoupling it means blanking `SKIPEXTENTVP` in the same
run, which changes what is submitted for exactly that population. Deferred rather than confounded.

### Clamp audit (the trap that has bitten three rounds running)

Every armed value verified inside its clamp against current bytes. Two are exactly AT their ceiling and
that is now the intended state, not a mismatch: `STATICINDEXBUDGET=64` against `clamp(env, 1, 64)`, and
`SUNSPRITEHOLD=216000` against `min(env, 216000)` — **round 34's open item 2 is closed**, the launcher
reads 216000 and the knobs line will now agree. Two `set` lines are no-ops because armed == default:
`VMBASISMAX=24` and `DRAWAUDIT=1`. `VMBASIS=5` passes `& 7u`. `VMPAIRVP` is blank so `VMPAIRALBEDO` and
`VMPAIRMAXDIST=2` are inert by construction — do not read `vm_tagged_pair=0` as a failure.

**A stale launcher claim found and corrected:** the note at `launch-haze-remix.cmd` saying that blanking
`VMPAIRVP` also disarms the VIEW_MODEL camera because "vm_tag_route_armed has four terms and this
launcher makes all four false" is **no longer true** — round 34 armed `VMDEPTHOFFSET=2`, and
`viewmodel_depth_offset_max() > 0.f` is the fourth term.

### Caught by diff review, not by me: the new knobs were reported on no line at all

The first cut of round 35 shipped `HIDEPAIRVP` / `HIDEPAIRFP` / `HIDEPAIRMODE` with **no field on either
`knobs=` block** — the same reported-vs-armed blindness that cost round 31 and round 32 a round each, and
that round 34 added `vmbasispivot=`/`vmtagonly=` specifically to end. Concretely: `HIDEPAIRMODE=3` would
have silently run as mode 2 (THIRD_PERSON_PLAYER_MODEL, a completely different behaviour from HIDDEN)
with nothing anywhere saying so, and a mistyped hash parses to 0 and disarms the route with no field
naming which half was wrong. Fixed: `hidepairvp=%016llx hidepairfp=%016llx hidepairmode=%u` added to
both blocks, arguments in matching positions, re-audited at **112/112** and **308/308** specifiers to
arguments with the new triple verified sitting between `vmtagonly` and `vmpairvps` in both.

**The lesson is not "add the field", it is that I applied the clamp-audit discipline to the knobs I
inherited and not to the knob I wrote.** A new knob needs the same instrumentation the old ones have, in
the same commit — otherwise the audit that catches the trap has nothing to read.

Three comment defects were found in the same review and are fixed: a second copy of the wrong-default
claim (`RemixTransforms.cpp`, "Anything else = 0 = off" — the default is 1 and out-of-range clamps *up*
to 2); a round-34 paragraph documenting `cdist_pre`/`cdist_post` orphaned from its `eye_distance` lambda
by the round-35 insertion, now moved back beside it; and "the -0.5/+0.866 '120 degrees'" written beside a
triple that contains no 0.866 (the ±0.866 are the OFF-diagonal row dots, X.fwd and Z.right).

The review independently re-derived the flip table from the operator itself
(`C = sum_k s_k a_k a_k^T`, left-multiplied, giving `dotpost = (s0, -s1, -s2)` from `pre = (+1,-1,-1)`)
and confirmed all eight rows and all eight determinant signs, and separately confirmed the
column-vs-row indexing, the `m_current_fp_hash` freshness and the disarmed no-op. So the round's central
refutation has two independent derivations, not one.
### Method notes earned this round

- **Before rewriting an operator because a projection looks wrong, check which way the matrix multiplies.**
  A row projection and a column projection of the same rotation are different numbers, and only one of
  them is a property of the object. The fix here was zero lines of operator change and six specifiers of
  census.
- **When a census prints two matrices and leaves the reader to project them, the reader will eventually
  project them wrong.** Print the derived quantity.
- **A discriminator that failed for one cut can still be the right one for another.** Round 27 correctly
  measured that the fragment program does not separate the weapon from the arms, and that finding was
  carried forward as "the fp does not partition the rig" — but it partitions the body from the rig
  perfectly. Re-test a rejected key against the new question.

## Round 44 (2026-08-27) — the wobble is the DIVIDE GAUGE, and it is a per-frame global

Deployed: `bin\rpcs3.exe` = `bin\rpcs3-next.exe` = **`14DD8282D86D298D`**. `bin\remix\d3d9.dll` **UNCHANGED at
`16A0B512F33EBB66`** — nothing deployed into `bin\remix\`. `bin\rtx.conf` READ ONLY: never written.
MSBuild `Release|x64` exit **0**, **0 errors, 1 warning** — the pre-existing C4723 "potential divide by
0", now at `RemixGSRender.cpp:11562` (round 43 recorded 11456; this round's edits above that line moved
it). **Zero new warnings.** HEAD `f091ff5a4` at both ends of the round; the tree did not move.

**One knob changes pixels this round: `RPCS3_REMIX_GAUGEDONORMAXT=32`.** Everything else added is a
counter or a print.

---

### THE HEADLINE: a wrong world gauge moves every instance in the frame TOGETHER

**MEASURED**, from the session that ended 2026-08-27 10:27 (build `Aug 26 2026 10:50:47`, `flips=56381`),
over all **12,779** `Remix worldid-draw:` lines:

```
frames carrying two or more rows ......................... 759
  every row reports ONE identical pre-translation ........ 559   (73.6%)
  rows disagree ...........................................200
frames whose max |pre.t| > 1.0 ........................... 293
```

At `f=53640` three unrelated meshes — `vtx=160`, `vtx=224`, `vtx=518` — all carry
`t=[1.579 -132.9 -2.179]`, identical to the last printed digit. At `f=5044`, `vtx=36` and `vtx=340` both
carry `t=[2646 -539.5 -782.5]`. **No per-object guest motion can produce bit-identical translations on
three different meshes in one frame.** In the 200 disagreeing frames the outlier is typically one
genuinely moving prop while everything else shares one value.

`Remix gauge:` corroborates it frame for frame — the same quantity, measured by
`report_gauge_trace()` at `RemixGSRender.cpp:2560` as `anchor->fused * m_active_camera.reference_inverse`:

| frame | `Remix gauge: translation=` | `worldid-draw` max `pre.t` component |
| --- | --- | --- |
| 48840 | 197.741 | 197.0 |
| 52560 | 21.3542 | 21.67 |
| 53640 | 132.275 | −132.9 |
| 50764 | 916.247 | −937.5 |

Run-wide over 17,386 `Remix gauge:` lines: **mean 41.99, max 1718.67, above 128 units on 7.36% of
frames.** For a program the title declares submits absolute world vertices, every one of those numbers
should be zero.

And the object is not moving. `E40BF80AF519848A` — the teleporting light fixture, open for over a week —
has a **raw vertex box that never moves**: of its 36 multi-sample mesh identities, **30 report exactly
0.0000 displacement** over spans up to 14,500 frames, while `pre.t` on a bit-identical raw box
(`vp=ad7ce9d672a0bf6b vtx=320 idx=480`, `rawcentre=[1702.385 -59.536 1037.805]` on all 19 samples) swings

```
f=50764  |t| = 966.5     basisdev = 0.5419
f=50880  |t| = 3.7e-09   basisdev = 4.5e-13
f=52560  |t| = 21.75     basisdev = 0.0181
```

**Raw box static + transform swinging 0 → 21.75 → 966.5 = ours, unambiguously.**

### The cause: ONE declaration drives TWO gates and only one of them ever learned the threshold

`WORLDIDENTITYVP` names the programs whose vertices are already in world space. It is read in two places:

* **at submit**, `keep_resolved` (`RemixGSRender.cpp:20647`) measures the draw's discarded translation
  against `RPCS3_REMIX_WORLDIDMAXT=32` and correctly says "900 units — this is not a world-identity
  draw, keep its real transform". That is round 24's carrier fix and it is armed.
* **at capture**, `capture_gauge_anchor()` (`RemixGSRender.cpp:1810`) has already run for that same draw,
  earlier in the same submit, and installed that same fused matrix as **the whole frame's world gauge**.
  Its only qualification, verified against current bytes at `:1811-1818`, is
  `world_identity_vp_matches(m_current_vp_hash)`. **It never looks at the translation at all.**

So the backend calls one draw "carrier-local" and "this is the world" in one frame for one reason.
The launcher has recorded this diagnosis since round 30 and the fix was specified there and never shipped.

**Why `ANCHORSTICKY` does not already cover it**, stated because it looks like it should: its test is
`matrix_relative_delta(fused, slot->fused) <= s_camera_discontinuity_tolerance`, and that tolerance is
**0.8** (`RemixGSRender.cpp:132`) on a *relative, whole-matrix* measure. A donor sitting 21.75 units off
the world origin scores nowhere near 0.8 and installs silently. Sticky asks *did the gauge change*; this
asks *is the gauge wrong*. **A stably-wrong gauge is perfectly continuous and passes sticky every frame.**
The tripwire that does fire confirms the shape of the problem rather than fixing it — every
`Remix gauge-contested:` line reads `holder_vp = contender_vp = ad7ce9d672a0bf6b`, **the same program
contesting itself**, `delta=0.988881 tol=0.8`.

### What shipped: `RPCS3_REMIX_GAUGEDONORMAXT` (whole world units, 0 = OFF = round 43 byte for byte)

Round 30's specification, applied at the capture site. A `WORLDIDENTITYVP` candidate whose own placement
— probed as `fused * slot->inverse`, translation read from row 3 after dividing out `m[3][3]`, character
for character the measurement `report_gauge_trace()` makes, so the gate and `Remix gauge: translation=`
are the same number — exceeds the threshold may keep its own transform at submit time but may **not**
donate the frame's gauge.

Four deliberate properties:

* **It is placed below the contested tripwire's own return**, so a donor arriving after the frame's gauge
  has landed still reaches that tripwire and `gauge_anchor_contested` is not silently deflated.
* **An offside donor is PARKED, not dropped.** This is a correctness requirement, not tidiness: the probe
  measures against the gauge the slot already holds, so on the frame after a scene cut *every* candidate
  is legitimately far from the stale gauge. A bare `return` would refuse them all, leave nothing parked,
  give `promote_parked_anchors()` nothing to install, and the slot would never recover — a permanent
  lockout on the first hard cut. Parking reuses round 10's existing recovery verbatim.
  **`gauge_anchor_parked` therefore rises; that is expected, and the round-43 baselines are 4,338 parked
  and 18 promoted over 56,381 flips.**
* **`WORLDIDMAXTEXEMPTVP` is honoured**, for exactly the reason it exists at the submit site.
  `BD1C10DF5703E559` submits genuine absolute world spans and its census reads
  `t=1412/4859/9/831` — 80% of its draws above 1 unit — so gating it would starve the slot.
* **`gauge_donor_offside` is counted before the knob is consulted.** An unarmed run still reports the
  population it would have refused. The last three rounds each shipped a counter that could only ever
  read one way; this is the guard against a fourth.

**It cannot starve the slot, and the census says so.** From the final `Remix worldid-census:` line
(buckets are `|t|<=1 / 1..32 / 32..128 / >128`):

```
vp=ad7ce9d672a0bf6b draws=3316291 kept=242939 t=2486813/586539/2986/239953  tmax=2646.13 bmax=2.00231
```

**2,486,813 of 3,316,291 draws (75.0%) sit at |t| <= 1** and only 242,939 (7.3%) exceed 32, so at a
threshold of 32 there are still 2.49M eligible donors on that program alone. (Round 25 measured 67% on a
different level; this run measures 75.0%.) `kept = 242,939` equals buckets 2+3 (`2,986 + 239,953`)
exactly, which is the arithmetic check that the buckets and the submit-site gate agree.

---

### THE INSTRUMENT THAT WAS LYING: `Remix pick-follow:` cannot see one object

`trace_pick_follow()` matched on `(vp_hash, albedo_hash)` and printed the **first** matching draw of each
frame (`RemixGSRender.cpp:1741`, `if (m_frame_counter == m_pick_follow_last_frame) return;`). On this
title that is not an object. `E40BF80AF519848A` alone carries **42 distinct raw vertex boxes across 41
distinct vertex counts** on one program — a tiled prefab drawn as many instances of one mesh under one
texture. `d_origin` was therefore the distance between two **different tiles**, and it is an **L1** norm
(`RemixGSRender.cpp:1756`, `+= std::abs(...)` per component), not Euclidean.

**This retracts round 43's evidence.** Round 43 wrote *"per-frame instance-origin jumps of up to 8.16
units, quantised at multiples of ~2.04"* and read the quantisation as jitter. Measured this round on
`F312BA4706AA7162`, the "jumps" land on exactly three positions —
`[3.1949 −1.1123 −4.0354]`, `[0.7553 −1.0691 −5.5999]`, `[5.6292 −1.1555 −2.4742]` — **evenly spaced on
one line, step 2.899 units Euclidean, 4.047 in L1, and 8.10 for a double step.** That is the **tile
pitch**, not a jitter. Round 43's other pick-follow claim, *"exactly 0.0000 on every identity-bypass
one"*, also fails on this run: the identity-bypass objects read `d_origin` maxima of 8.12, 8.11 and 6.36.

Worse, the one round ever pointed at `E40BF80AF519848A` fired its 181-frame follow during a window in
which the camera was frozen for **all 191 frames** (`cam=[1786.56 −31.2314 1226.39]`, 191/191 identical),
and `Remix gauge:` was a constant `translation=6.02944` throughout. `d_origin=0` for 181 consecutive
frames is **what a parked camera looks like**, and it was read as proof of stability. That is why this
object has stayed open for a week.

**Fixed, diagnostic only, no pixels:** the follow now also matches the clicked record's `vertex_count`
(published through `pick_state::follow_vertex_count`), and the line prints `vtx=`, `ext=` and `det=` so a
reader can confirm the trace stayed on one mesh shape and can tell a size change from real motion.

**Also re-pointed, launcher only:** `RPCS3_REMIX_TRACEALBEDO=E40BF80AF519848A` with
`RPCS3_REMIX_TRACEALBEDOVP=C2003391127734F6`. `Remix albedo-trace:` emits one line per frame carrying
`raw=[..]..[..]` **and** `matrix=[..]` **and** `cam=[..]` together — the only instrument that can answer
"did the guest move it or did we" in one line — and `C2003391127734F6` is on **no launcher list at all**,
so `worldid-draw` (which fires only for `WORLDIDENTITYVP` programs) has never covered that copy.
**Play it with the camera moving.**

---

### PRIORITY 1 FOR ROUND 45: an uncounted `return` discards 624,873 draws, 7.29% of everything

`RemixGSRender.cpp:23263`:

```cpp
if (static_entry->submitted_signatures.contains(static_submit_signature))
{
    return;                       // no counter, no census, until this round
}
```

The signature (`:23214-23261`) is built from **only** the 3x4 instance transform, `categoryFlags`,
`doubleSided` and the blend state. **It contains nothing about which geometry the draw covers.** On a
title that bakes world geometry in world space — and Haze does; every plant-floor `worldid-draw` line
carries `pre=[1 0 0 0; 0 1 0 0; 0 0 1 0]` — every static-index tile in a frame produces an identical
signature and only the first submits. That is correct if and only if the union mesh the first one carried
already held every tile, and the union is built **incrementally**, is wiped mid-frame by the
`source_changed` reset at `:21004` (also uncounted), and sits at **0 resident of 16,601 entries**.

**MEASURED exactly, by subtraction on the round-43 build.** `:23263` is the *only* return between
`++m_stats.xform_measured` (`:22771`) and `++m_stats.draws_submitted`, other than the poisoned-mesh
return — and `Remix stats:` reads `poisoned=0`. So:

```
xform_measured   8,569,414
draws_submitted  7,944,541
poisoned                 0
------------------------------
this exit          624,873      = 7.29% of every draw that resolved a transform and a mesh
```

**Shipped this round, diagnostic only:** `static_submit_dedup` (must reconcile to that subtraction) and
`static_submit_meshdiff` — the subset where the mesh this draw had selected is **not** the mesh already
submitted under that signature, i.e. where a strictly larger union was built and thrown away.
**`meshdiff` is the number round 45 needs**: near 0 means the collapse is benign and the half-present
floor is elsewhere; large means the floor is drawn from a partial union every frame.

Supporting measurements for the same area, all this round:

* **Mesh-key collision is REFUTED.** The ordinary mesh key hashes **every raw byte of every vertex**
  (`RemixGSRender.cpp:20898-20905`, position/normal/texcoord/colour) plus all indices, the vp hash and
  the albedo. Two distinct tiles cannot collide. The union path's `union_hash` (`:21130-21161`) does the
  same. `tex_key_dup=33333` is a texture-descriptor counter and is unrelated.
* **The static-index union key really is pointer-keyed.** `static_key` at `RemixGSRender.cpp:20986` and
  the union hash at `:21159` both fold in `reinterpret_cast<usz>(material)` — a runtime heap address —
  alongside the guest vertex-buffer address `block->real_offset_address` (`:20957`), and **no vertex data
  at all**. A recreated material entry silently forks a new entry with `mesh_hash = 0`. Correlation over
  671 windows cannot pin the 46 → 16,601 entry growth on material recreation, though
  (`corr(d_entries, d_tex_destroyed) = +0.370`, `d_tex_created = −0.003`).
* **The residency is the pathology.** Final line:
  `entries=16601 triangles=2982810 rebuilds=21243 deferred=68 stale=0 dropped=68 peak=128 budget=128
  resident=0 evicted=16601 nomesh=0`. `resident/entries` runs 1.000 → median 0.143 → **0.000**, with 408
  of 671 windows under 20%. The reaper erases from `m_meshes` but never clears
  `static_entry->mesh_hash`, which is exactly what `evicted` counts.
* **Parity/ping-pong is REFUTED.** No frame-parity gate, draw-index parity, ping-pong buffer or
  double-buffered slot array exists on the world submission or mesh path. Every `% 2` / `& 1` hit in
  `RemixGSRender.cpp` and `RemixTransforms.cpp` is a texture-unit bitmask, a rate-limited log, or
  triangle-strip winding in the **UI** compositor (`:14217`).
* **The floor, named.** Tile pitch measured at **~8 world units** (7.75, 8.01, 7.90, 8.42, 8.11 across
  independent tiles) — that is the checkerboard grain. Most-drawn large-extent opaque surfaces on the
  plant floor plane: `E40BF80AF519848A`, `CB1677B87EDD72F5`, `71D189E9B559A7F9`, `6DEBE6C7CC0FEEDB`,
  `CDC167D57B21D8E2`, `39D5CDABDAAE3ABB`, `E0D568A78FDD03F6`, `514AD452138A2803`. **11 of them are drawn
  through BOTH a `STATICINDEXVP` program (`AD7CE9D672A0BF6B`) and an ordinary one
  (`C1D482DCD1B03ED0`)**, 296 census shapes to 259 — one path hashing real vertex data and always
  submitting, the other unioning by metadata and a heap pointer. (Caveat: `worldid-draw` is deduped, so
  those are distinct observed *shapes*, not draw counts.)

---

### `world_refused` — round 43's "only two distinct keys" is REFUTED, and the census is blind anyway

`world_refused = 452,669`. The 1,087 `Remix world-refused:` lines name **18 distinct albedos** and
**14 distinct vps**, 20 distinct `(vp, albedo, vtx)` tuples and 17 distinct `(vp, fail)` pairs. Under
every reading the answer is not 2.

But the number is unusable either way. `world_refused_census_slot()` (`RemixGSRender.cpp:10157-10173`)
dedups on `m_current_vp_hash ^ (0x9e3779b97f4a7c15 * (m_world_fail_index + 1))` — **(program, reason)
only**; albedo, vtx, clip and surf are printed but not keyed. Window is
`s_stats_interval_flips = 120` (`:122`), cap is `s_max_world_refused_lines = 128`
(`RemixGSRender.h:3688`), reason space is 14 names. **Ceiling 14 x 14 = 196 possible lines per window,
capped at 128, against 452,669 refusals — at most ~0.03% of the population, and exactly one albedo per
(vp, reason).** Any statement of the form "material X is/is not being refused" is unanswerable from this
instrument.

Refusals **by draw**, from the final `Remix world-fail:` line, which is the right shape:

| reason | draws | % of world_refused |
| --- | --- | --- |
| `tail` | 402,198 | 88.85% |
| `nocam` | 46,317 | 10.23% |
| `lay_other` | 42,781 | 9.45% |

`46,317 + 42,781 + 402,198 = 491,296`, minus `world_refused` `452,669` = **38,627**, which is exactly the
`particle=38627/416887` field — GATE 2's particle arm, which calls `note_world_fail` but is deliberately
not counted as refused because those draws *are* submitted. The books balance to the unit.
`Remix layother:` is 256/256 `areason=no-chain`, all `blend=1 depth_write=0` — **transparent
non-depth-writing draws, not floor tiles.**

---

### Instrument notes worth keeping

* **A census that dedups by key cannot enumerate a population.** Round 43 read "two distinct keys in the
  whole run" off an instrument whose key is `(vp, reason)` and whose cap is 128 lines per 120 flips.
  Before quoting a census as a fact about the world, read its admission gate and compute its ceiling.
* **`d_origin` between two draws of one (program, texture) pair is not motion.** Where a title instances
  one mesh many times, the "jitter" is the instance pitch. The tell is that it is **quantised**: real
  numerical error is not.
* **Zero from an instrument that cannot move is not evidence.** 181 frames of `d_origin=0` were read as a
  stable object; the camera had not moved for any of them. Before believing a null result, check that the
  driving variable varied.
* **Check the format auditor before trusting it.** `scratchpad\r43\audit.py` reported "298 formatted
  calls, 0 mismatches" on a file into which a 1-extra-specifier mismatch had been deliberately injected
  at the `Remix pick-follow:` site. It gives a **false pass**. `scratchpad\r44\fmtaudit44.py` catches that
  same injection and reports 108 `fmt::format` calls (matching `grep -c "fmt::format("` exactly), 0
  mismatches on the shipped file. Self-test the tool on a broken copy every time.
* **A gate that judges against a stale reference must not be a bare `return`.** The scene-cut lockout in
  the first draft of `GAUGEDONORMAXT` was found by asking what the probe measures on the frame *after* a
  cut, not by testing.
* **Both banners, both arg lists.** `gaugedonormaxt=%u` had to be added to the run-start banner and the
  `Remix live:` knobs section, with the matching argument in both lists. The `Remix live:` format is one
  360-specifier call; a positional slip there misroutes every later field.

## Round 43b (2026-08-26) — the kill was too wide; the rule is CHANNEL WIDTH

**`RPCS3_REMIX_FPALBEDOKILL` was play-tested and reverted.** It produced a full-screen coloured-static
overlay at 32 fps. It is **removed from the build**, not merely defaulted off — the exe contains neither
the wide string `RPCS3_REMIX_FPALBEDOKILL` nor `tex_albedo_kill=%llu` nor `fpalbedokill=%d` (all three
verified absent at offset -1).

Deployed: `bin\rpcs3.exe` = `bin\rpcs3-next.exe` = **`ADA8AD6CDCDB53B1`**. `bin\remix\d3d9.dll`
unchanged at `16A0B512F33EBB66`; `bin\rtx.conf` unchanged at `215975B8697DE21B` (read only all round).
MSBuild `Release|x64` exit 0, **0 errors, 1 warning** — the same pre-existing C4723, now at
`RemixGSRender.cpp:11456`.

### What the kill got right, and what it got wrong

It fired exactly as designed: `tex_albedo_ucode` went **0 → 81,091**, all 81,091 from the kill. The
diagnosis of the white walls stands unchanged. The defect was **scope**, in two independent ways — and
only one of them was the one anticipated.

### The re-election was NOT what painted the screen

The theory was that one of the 36 re-elected programs draws full-screen. **That is not supported.**
Mapping all 36 back through `raw ^ 0x9e3779b97f4a7c15` and searching the 32,888-frame log:

```
of the 36 kill-changed programs:
  appear in 'Remix uiwrap:' / 'Remix ui-biggest:' (screen-space) ....  0
  appear anywhere in the run, as world geometry ....................  1   (65a91390aaf6bef3)
  never drew at all in that session ................................ 35
```

**Caveat, stated because it matters:** that log is the round-42 session, *before* the noise. "Never drew"
transfers only weakly — a different area draws different programs. The **zero screen-space hits** is the
stronger half: across a session covering menus, loading screens and gameplay, not one of the 36 ever
reached the UI census.

### The likelier mechanism is the one shipped alongside it

`albedo_unit_mask()` also set `from_ucode = true` on the kill path. That is not cosmetic: it unblocks the
retry guard at `RemixGSRender.cpp:19445`, whose own comment says it "can never not fire" on this title.
Unblocked, those draws take the **unrestricted** `albedo_texture_unit_in(unit_mask, unit + 1)` walk — the
one the source documents as having "painted character faces onto tree trunks" on Resistance 2 (NPEA00431).
81,091 draws with an unrestricted substitution walk fits world-wide static far better than 36 programs of
which one drew.

**Both mechanisms are closed by the new design**, not just the smaller one.

### The rule that shipped instead: channel width

A texture unit **every sample of which writes fewer than three destination channels** cannot be carrying
RGB. Structural, not heuristic — the channels are not there. From the confirmed-good program
`fp=65a91390aaf6bef3`:

```
  4: TEX R1     TEX2.zwzz, tex2    4 channels
  9: TEX R2.x   TEX2,      tex0    ONE channel   <- parallax height map, elected today
 18: ADD R4.xy  TEX2, R2                         <- used only to perturb a UV
 21: TEX R2.yw  R4,        tex3    2 channels
 24: TEX R2     R4,        tex1    4 channels    <- the real diffuse
```

Scored offline over all 338 `.fp` files (`scratchpad\r43\chanrule.py`) **before the build**:

| | KILL (reverted) | CHANNEL (shipped) |
| --- | --- | --- |
| saturated programs narrowed | 110 | 178 |
| **elected unit CHANGES** | **36** | **3** |
| of those, electing unit >= 8 | **16** | **0** |
| units re-elected to | 1, 8, 10, 11, 13, 14, 15 | **all three are 0 -> 1** |
| `fp=65a91390aaf6bef3` | `0x1f` -> `0x16`, unit 0 -> 1 | **identical** |

Same answer on the one case ever verified, one twelfth of the blast radius, and every high-unit
re-election gone.

### Two containments, both required

`albedo_unit_mask()` returns `referenced` untouched unless the narrowed mask (a) names a unit the backend
can **bind**, and (b) elects a **different** unit from the one that would have been taken anyway. (b) is
what makes it surgical: the rule narrows 178 programs but only 3 change election, so the other 175 are
bit-exact and their retry walk is never confined to a smaller mask.

And `from_ucode` is **deliberately left false**. `tex_albedo_narrow` is therefore a subset of
`tex_albedo_guess`, not of `tex_albedo_ucode`, and the retry policy is byte-for-byte round 42's. One
change at a time.

### The one program that will actually move

`65a91390aaf6bef3` is the only one of the three that drew in the logged session. It appears **0 times** in
`Remix uiwrap:` and `Remix ui-biggest:`, and 2,545 times in `Remix watch:` plus `Remix effect:`,
`Remix suncard:` and `Remix fpcandidate:` — all world-geometry censuses. **It is world geometry and never
screen-space.** The other two (`f26a8613a74b6178`, `1ab7ae13810b04f2`) never drew at all.

### The mirroring hypothesis is REFUTED — by the instrument shipped the same round

`xform_mirrored = 0`, and three fresh picks read `det = 1.00104`, `det = 1`, `det = 1`. **No reflected
instances exist.** The black voids are not a facing problem, and the reversed wall text in the capture has
some other cause — graffiti authored mirrored in the game, or read through a transparent surface. The
hypothesis was wrong; the `det=` field made it cost one run instead of a round.

Hypotheses (a) never submitted, (b) hidden, (c) degenerate transform and (d) reflected are now **all
refuted**. Round 44 starts from picks taken on the black surfaces themselves — they now carry `det=`, and
`xform_measured`/`xform_degenerate` say whether those draws reach the transform path at all.

### Instrument note

**Two changes shipped under one knob cannot be told apart by a play-test.** The kill changed *which unit
is elected* **and** *whether the retry walk may run*. The visible failure was attributed to the first; the
evidence fits the second better; and one knob could not separate them. One mechanism per knob.

## Round 43 (2026-08-26)

Deployed: `bin\rpcs3.exe` = `bin\rpcs3-next.exe` = **`DD82C9B9A8FDAB7F`**. Runtime `bin\remix\d3d9.dll`
**UNCHANGED at `16A0B512F33EBB66`** — nothing deployed into `bin\remix\`. `bin\rtx.conf` READ ONLY:
not written this round.

Tree at both ends: HEAD `f091ff5a4`, working tree carries rounds 37–42 plus this round's four source
edits and the launcher. **The tree did not move.** Both exes read `DCCF0459B83778AE` at the start,
exactly what round 42's inbox recorded. MSBuild `Release|x64` exit **0**, **0 errors, 1 warning** —
the pre-existing C4723 "potential divide by 0", now at `RemixGSRender.cpp:11440` (round 42 recorded
11389; this round's edits above that line moved it). **Zero new warnings.**

---

### THE WHITE WALLS ARE A PARALLAX HEIGHT MAP BOUND AS THE ALBEDO

Round 42's deep vertex-colour search **worked**, and **could never have fixed these walls**. Both halves
are measured on the round-42 build the user play-tested (`build=Aug 25 2026 18:50:12`, 32,888 flips):

| counter | round 41 | round 42 build | verdict |
| --- | --- | --- | --- |
| `vcol_mod` | 10,280 / 5,392,256 = **0.19%** | **3,242,137** / 5,013,889 = **64.7%** | the fix fired |
| `fpclass=a/b/c` | b = 1 | b = **122** | the fix fired |
| `fpdeep` | — | **121 / 3,233,387** | the fix fired |
| `created` (meshes) | 402,758 | 435,050, `live` 2,296 | bounded, no per-step minting |

And yet the walls are still white, because **the replayed factor is neutral**. Every draw of all five
hashes the user listed reports `class=vcol_modulate replay=1 route=scaled rgb-varies=0` with
`vcol0=FEFEFE` (76 draws for `A26189276C5819BC`; 207 / 22 / 12 / 18 for the others). `vcol0` is read
from the *submitted* vertices, after the fold and after `clamp(v,0,1)*255` at `RemixGSRender.cpp:11116`.
Working back through the `fold *= 2.0` at `:11093-11095`, `0xFE` post-fold means the guest's COL0 byte
is exactly **`0x7F`** — the unlit-neutral value in the 0..2 convention the code documents at
`:11062-11064`. **Multiplying by 1.0 cannot darken anything.**

**The fault is one unit further up, and the run says so exactly.** From the last `Remix stats:` line:

```
tex_albedo_ucode = 0            tex_albedo_guess = 7,383,877
```

The ucode albedo discriminator did not resolve **one** draw in the whole session.
`albedo_unit_mask()` (`RemixGSRender.cpp:6061`) declines whenever `colour == referenced`
(`:6087`) and the caller then takes the **lowest referenced unit** (`albedo_texture_unit_in`, `:6105`).

On `fp=65a91390aaf6bef3` — the program on the picked white wall `9AAA430414B3D49D` — the lowest unit
is **not** the diffuse:

```
  9: TEX      R2.x   TEX2, tex0      <- .x only: a single-channel PARALLAX HEIGHT MAP
 11: MUL      R2.x   R2, {0.334754}
 13: MAD      R2.x   -R1.wwww, {0.5}, R2
 17: MUL      R2.xy  R2.xxxx, R2.zwzz
 18: ADD      R4.xy  TEX2, R2        <- R4 is a TEXTURE COORDINATE
 24: TEX      R2     R4, tex1        <- the real diffuse
 31: MUL      R2     R2, R5          <- the modulate round 42 replays
 46: TEX      R4     ...,  tex4      <- writes R4 AGAIN
```

**Why `colour_mask` saturates.** The backward walk at `RemixTransforms.cpp:8170-8218` has **no kill** —
its own comment at `:8168-8171` says so and calls the over-approximation safe. So `R4`, live from
instruction 46's result, stays live all the way back past instruction 18; the walk follows `R2` into
the height-map chain and credits **tex0** as a colour. `colour_mask == sampled_mask == 0x1f`, the
discriminator declines, the lowest unit wins, and a **near-white greyscale height map is bound as the
albedo**. A near-white albedo is a white wall.

This is a known-but-unclosed defect in the file itself: `RemixGSRender.cpp:19435` already reads *"Haze
reports `tex_albedo_ucode=0`, so `unit_from_ucode` is always false here and the guard can never not
fire"*.

It also explains why tagging those five hashes in `rtx.conf` never did anything: **they are the content
hashes of height/mask maps, not of wall diffuse maps** (`albedo_hash = entry->content_hash` for the
*selected* unit). Two of them were in `rtx.decalTextures` and were untagged before this round; that
change is orthogonal and is not the reason anything does or does not improve.

#### What shipped: `RPCS3_REMIX_FPALBEDOKILL`

`fp_fingerprint::colour_mask_kill` (`RemixTransforms.h`, filled at `RemixTransforms.cpp` right after
`colour_mask`) — the same reachability, one reverse pass, with an ordinary backward kill: an
**unpredicated** write retires the destination bits it defines before its own sources are added.
Predicated writes retire nothing, which is the same guard the deep-modulate search uses and the reason
the existing walk refused a kill at all.

Single pass, not a fixpoint, deliberately: the kill makes `live` non-monotonic, so the existing loop's
termination argument ("bounded because live only grows") stops holding. A single pass is also what makes
the answer a guaranteed **subset**: single-pass-with-kill ⊆ single-pass-no-kill ⊆ fixpoint-no-kill.

`albedo_unit_mask()` consults it **only on the branch that already declined**, behind three guards, all
required: non-empty; a **strict** subset of `referenced`; and it must name a unit
`albedo_texture_unit_in()` can actually bind. Without the third, a program whose only killed unit is
disabled would go from *wrong texture* to *no texture*. It is also AND-ed with `colour`, so a future
divergence between the two walks can only ever remove units.

**PRE-REGISTERED OFFLINE, before the build existed**, by reimplementing both walks in Python over all
338 `.fp` files in `bin\remix_ucode\` (`scratchpad\r43\killwalk.py`, decode mirrored field-for-field
from `RemixTransforms.cpp:8000-8095`):

```
325 programs with a sample (313 multi-unit)
  saturated today, untouched by this change ........ 123
  saturated today, kill NARROWS it ................. 110
      of those, same elected unit .................. 74   (no visible change; from_ucode flips true)
      of those, elected unit CHANGES ............... 36   <- the whole blast radius
  kill yields empty / no narrowing ................. 92   (falls back, bit-exact)
  kill WIDENED a mask (must be 0) .................. 0
fp=65a91390aaf6bef3 : mask 0x1f -> 0x16, elected unit 0 -> 1
```

Saturation over the corpus drops **202/325 (62.2%) → 76/325 (23.4%)**; programs that discriminate go
**121 → 227**.

**Second-order effect, named rather than hidden.** `unit_from_ucode` turning true also unblocks the
retry guard at `RemixGSRender.cpp:19445`, which on this title has never once been able to not fire. The
`tex_retry_refused = 1,917,177` population (identical to `tex_none_shadowonly`) may now walk past its
2048×2048 DEPTH16 shadow map to a higher unit. That is the intended direction, and it is also the
largest thing that could go wrong.

---

### PRIORITY 1, THE BLACK VOID: three hypotheses refuted, a fourth found, and the instrument that could never see it

`Remix pick-follow:`, `Remix world-refused:` and the category counters separate the three hypotheses the
brief listed. **All three are refuted for walls and floors:**

- **(a) never submitted.** `world_refused = 248,109`, but the `Remix world-refused:` census names only
  **two distinct keys in the entire run** — `line=1/128` and `line=2/128` of a 128-line-per-window
  budget — and both are `vtx=4` full-screen quads (`albedo=EA4E4CF3B4BF0C7C`,
  `albedo=FCB2B58AF77F60A0`). No wall or floor is being refused for want of a world transform.
- **(b) submitted and hidden.** `cat_hidden = 134,023` comes **entirely** from
  `RPCS3_REMIX_CAT_HIDE=B6AE753B64E6F693,597C5F48E671924F`, two hashes deliberately set at launcher
  line 1163 (`hash_in_category`, `RemixGSRender.cpp:5705-5709`). `cat_decal = 0`, `cat_particle = 0`,
  `cat_sky = 0`. Nothing else is hidden, and `rtx.conf`'s own lists never reach this path — the backend
  reads `RPCS3_REMIX_CAT_*` (`RemixTransforms.cpp:6309-6320`), not the conf file.
- **(c) degenerate transform.** Real but **not proof of a visible defect**: `Remix pick-follow:` shows
  per-frame instance-origin jumps of up to **8.16 units**, quantised at multiples of ~2.04, on every
  surface whose reference is `anchor`/`anchor_prev`, and **exactly 0.0000** on every `identity-bypass`
  one. But the `arch=fused` path divides the vertices by that same reference, so the product is
  unchanged — origin jitter alone cannot move a surface on screen. It matters for motion vectors and
  mesh identity, not for a missing wall.

**The fourth hypothesis, from the user's own capture** (`rpcs3__2026-08-26__09-24-20_trim.mp4`):

- `f_030.jpg` / `f_031.jpg` — painted wall text reads **backwards** ("PAZ", "FUERA", "NUESTRO"), and it
  is on precisely the surface that is rendering **solid black**. The rpcs3 HUD in the same frame reads
  correctly, so the final image is not mirrored.
- `f_200.jpg` — in a different area of the *same run*, world text reads **correctly** ("SECTION D" on a
  sign, "R107" stamped on a dozen steel beams, "003-KVZ MANTEL GLOBAL INDUSTRIES"), and the scene is
  properly textured and lit.

So the reflection is **per instance**, not a global handedness error in the camera, and it is not
back-face culling either: `instance.doubleSided` is 1 on every instance unless `cull_from_rsx()`
(`RemixGSRender.cpp:22611`), and `RPCS3_REMIX_CULL` is absent.

**A negative 3×3 determinant is the one transform property that produces both symptoms at once**: the
geometry is reflected, so its texture reads mirrored, and the winding is reversed, so the surface is
shaded from behind and renders unlit — a black polygon that light leaks past.

**And it has never been observable.** `pick_record::basis[i]` is `sqrt(x²+y²+z²)` of row *i* — a norm,
sign-blind by construction, so `basis=[1 1 1]` reads identically for an identity and for a reflection.
The only `det3()` in the backend (`RemixGSRender.cpp:7633`) is a lambda local to the **viewmodel** basis
census and is never applied to a world instance. Four rounds of hypotheses about the black voids were
argued with no instrument that could see a reflection.

**Shipped this round, diagnostic only, no pixel depends on it:** `xform_measured` / `xform_mirrored` /
`xform_degenerate` over every instance that reaches `DrawInstance`, on both `Remix stats:` and
`Remix live:` (as `xform_mirrored=M/D/T`), and **`det=` on `Remix picked:`**, computed from the same
transform at the same point so the counter and the pick line can never disagree.

---

### PRIORITY 2, THE ADS COLLAPSE: root-caused, and it is NOT the compositor's blend

Round 42 named `compositor::blend()`'s missing additive path and its `alpha == 0xFF` `memcpy`
(`RemixCompositor.cpp:631-633`) as the mechanism. That code is exactly as described — straight alpha
only, no additive path anywhere in the file, `blend()` the sole compositing operator (3 call sites,
`:782`, `:891`, `:952`) — **but it is not what costs the frame.** Three of round 42's supporting numbers
do not survive re-measurement:

- **`ui_px = 4,480,204` is from a different run.** It came from a scratchpad `timing.txt` written
  2026-08-25 18:14, not from `bin\log\RPCS3.log`. This run's worst in-world window is **1,383,961**
  px/frame, and its global maximum over all 400 windows is 2,142,748 (a loading screen).
- **"a fixed, deterministic 24-draw overlay set" is backwards.** `ui_pixel` is cumulative
  (`m_stats` is never reset; only `m_timing` and `m_compositor.reset_pixels()` are, at
  `RemixGSRender.cpp:25294-25295`). Per-window deltas make `ui_pixel/frame` bimodal at 1 and 22–25, and
  **the 22–25 population is the cheap one**. In the expensive windows `ui_pixel/frame` is exactly 0.
- **`present` falling is not the tracer being starved of geometry.** `submitted/frame` goes 56 → 48 and
  `draws/frame` 227 → 234 across the boundary — essentially unchanged. The world is still being
  submitted; the CPU is simply spending 25 ms elsewhere in the same frame.

**What actually happens, MEASURED across the boundary** (windows 105–107 control vs 109–115 ADS, camera
parked at the same `scene=[1786 -31.31 1227]`):

| per frame | control | ADS | |
| --- | --- | --- | --- |
| `ui_ndc` | 20.0 | **94.0** | the branch that flipped |
| `ui_forced` | 22.0 | **0.0** | |
| `ui_pixel` | 22.2 | **0.0** | |
| `ui_px` | 24,811 | **1,383,930** | **55.8×** |
| `ui` ms | 0.29 | **25.2** | **87×** |
| `present` ms | 11.07 | **0.41** | |
| `frame_ms` | 21.5–22.3 (45.7 fps) | 33.0–35.0 (**29.7 fps**) | |

Vertex program **`2f64c2f8ffd6add1`** is not statically fingerprinted as screen-space, so
`is_screen_space_draw()` re-decides per draw. In the control regime the classifier **refuses** it and
`RPCS3_REMIX_UIFORCEVP` drags it in through the **clip-pixel** branch (`RemixGSRender.cpp:12870`), where
coordinates are already pixels — 22 cheap draws/frame. In ADS the same program **passes the classifier
on its own**, so `ui_forced` goes to 0 and it takes the **NDC** branch (`:12859`), which multiplies every
vertex up to full compositor scale at `:12864-12865`. Its 1280×720 sheet **`FF724C765485B42B`** is then
rasterised at exact full-screen NDC `[-1,-1]..[1,1]`; `Remix ui-biggest:` reports `area=861440px`, which
is the **entire** compositor buffer (`RPCS3_REMIX_UIWIDTH=1280`, launcher line 59 → 1280×673 = 861,440).
That one quad is **63.3%** of the 1,359,880 textured px/frame.

Pixel reconciliation, exact: `ui_uv` counts `address_coordinate` calls, two per sampled texel
(`RemixCompositor.cpp:368-369`), so textured samples = Σ`ui_uv`/2 = 1,359,880; plus a constant
~24,050 px/frame of untextured rpcs3-overlay work → **1,383,934 = `ui_px`, residual 0.0**.
Refitting on textured pixels only: `ui_ms = 16.84 ns × px + 0.295 ms`, **R² = 0.98151**.

`ui_px` is counted at `RemixCompositor.cpp:613`, the **first** statement of `blend()`, before both the
`alpha == 0` early-out and the clip reject — which is why the linear fit is so tight, and which means
the cost is paid in `sample_bgra` and the raster loop, **not** in the blend arithmetic.

**No existing knob can skip this draw.** `SKIPVP` takes a single hash and would delete the whole HUD;
`UIFORCEVP` is measured at 0.00/frame in ADS and cannot help; `UIRECTSHRINKPCT` reaches only the 298
font triangles/frame. The only lever is `RPCS3_REMIX_UIWIDTH`, and the cost is **quadratic** in it:

| `UIWIDTH` | full-screen layer | ADS `ui` | ADS frame | |
| --- | --- | --- | --- | --- |
| 1280 (now) | 861,440 px | 25.2 ms | ~33.5 ms (29.9 fps) | |
| 960 | 484,560 px | ~14.2 ms | ~22.5 ms (44 fps) | HUD upscaled 4× to a 3840 client |
| 640 | 215,360 px | ~6.3 ms | ~14.6 ms (68 fps) | HUD noticeably soft |

That is a **trade, not a fix**, and it is offered as one. The fix is either to stop that program taking
the NDC branch when its post-matrix extent already covers the screen, or to make the raster loop not
scalar. Neither is a one-liner and neither shipped this round.

---

### Also settled

- **`6575ACE3A42A78E6` is not a rect-sizing problem.** `f_200.jpg` shows two **solid filled** yellow
  quads and a solid white bar where nectar HUD text should be — the atlas is not being sampled at all.
  `RPCS3_REMIX_UIRECTSHRINK` addresses UV rect geometry and cannot fix an unsampled texture. It is on
  the shrink list at 50% and carries a veto in `rtx.uiTextures`; neither is the cause.
- **`RPCS3_REMIX_EMISSIVEINT` is still not a real variable** — the parser reads
  `RPCS3_REMIX_EMISSIVEINTENSITY` (`RemixTransforms.cpp`). Unchanged from round 42's note.
- **`glauto=%d` still prints a boolean, not the mode.** Unchanged from round 42's note; the launcher
  has `GUESTLIGHTAUTO=0` so it reads 0 either way this round.
- **The static-index cache is thrashing and nobody has explained it.** `entries=3296 rebuilds=11183
  dropped=69 peak=128 budget=128 resident=62 evicted=3234`. Round 42 refuted round 31's budget theory
  using `dropped`, which is the right counter for "draws that got no index" — but `evicted` is a
  different counter and 3,234 of 3,296 entries have been evicted from a 128-slot cache. Not claimed as
  a defect; claimed as an unexplained number.

### Instrument notes worth keeping

- **A norm cannot see a sign.** `basis=[1 1 1]` was read as "no transform anomaly" for four rounds. Ask
  what a diagnostic is mathematically incapable of representing before concluding from it.
- **Check which run a number came from.** Round 42's headline `ui_px` and its "24 draws" both came from
  a scratchpad file belonging to an earlier session, and neither reproduces in the log the same brief
  pointed at.
- **A cumulative counter divided by frames is not a rate.** `ui_pixel` reads 22–25/frame only if you
  take per-window deltas; taken raw it is nonsense, and the population it names is the cheap one.
- **The offline corpus is the cheapest possible A/B.** Reimplementing a classifier in Python over
  `bin\remix_ucode\` scored this round's change to a blast radius of 36 programs before a compiler ran.

## Round 42 (2026-08-25)

Deployed: `bin\rpcs3.exe` = `bin\rpcs3-next.exe` = **`DCCF0459B83778AE`**. Runtime `bin\remix\d3d9.dll`
**UNCHANGED at `16A0B512F33EBB66`** — nothing deployed into `bin\remix\`.

Tree at both ends: HEAD `f091ff5a4`, working tree carries this round's four source edits plus the
launcher. **The tree did not move.** Both exes read `49D31EEA1D767004` at the start, exactly what
round 41 recorded. MSBuild `Release|x64` exit 0, **0 errors, 1 warning** — the pre-existing C4723
"potential divide by 0", now at `RemixGSRender.cpp:11389` (it was 11339; this round's edits moved it).
**Zero new warnings.** The exe carries no `expected<enum rsx::primitive_type` marker, so the build is
not the poisoned `primitive_mode()` shape.

---

### TASK 0 — THE BRIEF'S PREMISE IS REFUTED, AND THAT IS THIS ROUND'S FIRST RESULT

The brief opened: *"37.6% of textured-draw attempts resolve to NO ALBEDO ... Over a third of the world
is submitted with no albedo and renders with the backend's neutral material. This dwarfs everything
else."* The ratio is arithmetically correct and the conclusion drawn from it is not.

**MEASURED**, the final `Remix stats:` and `Remix live:` lines of the round-41 play-test
(`bin\log\RPCS3.log`, 41,802 flips, 5,392,256 submitted draws):

```
tex_none = 3,693,131
  notex_skip        3,605,397   97.62%   refused by a named skip gate
  notex_world          85,060    2.30%   refused by the world/affinity gate
  notex_mat_applied     2,674    0.07%   reached Remix, took the neutral grey
                    ---------
                    3,693,131            EXACT
```

**The three sum to `tex_none` exactly.** And the refusals really are refusals: all thirteen
`report_skip_census(...)` call sites in `RemixGSRender.cpp` — `wext`, `viewmodelrefused`, `skipvp`,
`skippair`, `skipalbedo`, `skipuntexturedvp`, `untexturedfppair`, `characterdepth`, `unboundblend`,
`auxuntex`, `shadowonly`, `r2streak-late`, `skipextent` — are each immediately followed by a
`return`, verified line by line against current bytes.

**So the number of draws that reach Remix with no albedo is 2,674 out of 5,392,256 — 0.0496%, not
37.6%.** `tex_none / (tex_none + tex_bound)` is the *walk's* failure rate over every draw that ever
looked at a texture unit, including 2.2M binds of a depth buffer. It is not a submitted-population
share, and it never was.

A second exact partition says the same thing from the other side:

```
tex_none = 3,693,131
  tex_no_unit          1,527,047   41.35%   no eligible 2D unit at all
  tex_retry_refused    2,166,084   58.65%   the walk gave up
                       ---------
                       3,693,131            EXACT
tex_shadowonly (= tex_none_shadowonly) = 2,166,084   <- identical to tex_retry_refused
tex_colorunit  (= tex_none_colorunit)  = 0
```

**`tex_none_colorunit = 0`.** Not one draw in a 41,802-flip run had a colour-format texture on a
sampled unit the walk failed to reach. The `WALKSAMPLED` widening is armed (`walksampled=1` on the
banner) and did its work: `tex_walk_sampled = 77,883`. The albedo walk is not failing on this title —
**for 58.65% of the population every eligible unit is the 2048x2048 DEPTH16 shadow map and there is no
colour texture anywhere, and for the other 41.35% the fragment program references no 2D unit at all.**

The `Remix notex:` census on disk names them: **919 unique rows read
`class=retry-refused reason=format unit=0 fmt=0xb2 shadowonly=1 colorunit=0 sampled=0x1`** — the
program samples exactly one unit, that unit is the shadow map, and there is nothing else to elect.
541 rows read `class=no-unit sampled=0x0`.

**Therefore the brief's stated round-42 task — "why does the fragment program's sampler analysis fail
to name the albedo unit"** — has no failing population to work on. `tex_albedo_ucode=0` is real, but
it is not costing pixels: with `colour == referenced` the mask falls back to the lowest referenced
unit, which is the same answer, and `tex_colorunit=0` proves no better answer existed.

**The user's `524D584E4F544558` ("RMXNOTEX") Ctrl+Click is consistent with this, not against it.**
Of nineteen `Remix picked:` lines recovered from the run, **eighteen read `material=1
albedo_unit=0`** — a resolved texture. The single exception is

```
vp=fc0fac8afccec49a fp=938cfb957fcb7db7 albedo=0000000000000000 vtx=32 extent=99.71
material=0 albedo_unit=-1 sampled=0x0 fpcol=vcol_pass fpalpha=vcol_pass fpvcol=1
```

`sampled=0x0` — the fragment program samples **nothing**. That draw is vertex-coloured by design; the
grey placeholder is what an untextured draw is supposed to get, and the pick landed on one of the
2,674. One pick is not one third of the world.

---

### TASK 1 — THE WHITE WALLS, FOUND IN THE UCODE

The brief's *instinct* was right and its target was wrong. It predicted the answer would be *"a bit or
a shape in the ucode encoding the matcher fails to replay"*. It is exactly that — in the **fragment
vertex-colour classifier**, not the albedo-unit matcher.

#### 1a. What the classifier was looking at, and what the programs actually do

`scan_fragment_program` classifies the **last instruction** that writes `col0.rgb`, accepting only
`MOV out, COL0` or `MUL out, tex, COL0`. Round 41 added a hop through identity `MOV` copies and moved
`fpclass` from `1/1/0` to **`2/1/0`** — one more program in the whole title.

The round-41 `Remix fpother:` census is what settles it. **MEASURED, all 64 rows:**

| terminal shape | rows |
| --- | --- |
| `op=2(MUL) srcs=2 t0=0(TEMP) t1=2(CONST) k0=0x04 k1=0x08` | **39** |
| `op=2 srcs=2 t0=0 t1=0 k0=0x04 k1=0x0c` | 6 |
| `op=1(MOV) srcs=1 t0=2(CONST)` | 3 |
| `op=4(MAD)` variants | 4 |
| everything else | 12 |

**39 of 64 end on `MUL col0.rgb, <temp>, <inline constant>.<swizzle>`** — a multiply by a literal.
(`k1=0x08` says non-identity swizzle; the census cannot say *broadcast*, but all four programs
disassembled below read theirs as `.xxxx`.)

Four of these programs have their raw ucode on disk in `bin\remix_ucode\` from earlier runs, stored
by `store_refused_fp_ucode`. Disassembled this round (the file name is `fp_hash ^
0x9e3779b97f4a7c15` when `fp32_outputs`, `RemixGSRender.cpp:9031`). Two of them are programs the
**user's own Ctrl+Clicks landed on walls** — `aa0fe222771ff5c0` and `65a91390aaf6bef3`.

`34389B9B085589D5.fp` (= fp `aa0fe222771ff5c0`, 58 slots), slot numbers verbatim:

```
 14: MOV  H1,     ATTR1                    ; COL0
 15: MUL  R1.xyz, H1, {2,0,0,0}.xxxx       ; the 0..2 lighting expansion
 17: MOV  R1.w,   H1                       ; alpha copied UNSCALED
 26: TEX  R0,     ATTR5, tex0              ; the albedo
 27: MUL  R1,     R0, R1                   ; *** albedo x (COL0 x 2) ***
 ... 30 instructions of normal map, specular, lighting composite ...
 57: MUL  R0.xyz, R0, {0.999001,1.001,0,0}.xxxx  END
```

`FB9E6A29D5BCC2E6.fp` (= fp `65a91390aaf6bef3`, 64 slots) is the same program with the modulate at
slot 41 and the terminal at 63 (`{0.999002,...}`). `3FF09E976DF177B5.fp` and `7BD30EA79A75DB92.fp`
likewise, at slots 5 and 6.

**The modulate is real, unambiguous and thirty to forty-five instructions upstream of the export.**
A classifier that reads only the terminal instruction cannot reach it, and no number of identity hops
will: the instructions in between are `MAD`s doing real lighting arithmetic. That is round 41's own
pre-registered failure mode firing exactly as written — *"a program that does real arithmetic between
the modulate and the export ... is not reached and stays other."*

**Independently corroborated inside this repository.** Round 11 transcribed the haze card's fragment
program into a source comment and wrote its line 9-10 as
`rgb = texRGB * (2 * COL0.rgb) * 0.944243` (`RemixGSRender.cpp`, the `apply_haze_fade` doc block).
The same shape, decoded a different round by a different route, sat in a comment for thirty rounds
without being connected to the classifier.

#### 1b. The fix: `RPCS3_REMIX_FPVCOLDEEP` (default 0, armed 1)

Search the whole program for the modulate instead of only its last line. Four terms, **all** required:

1. an unconditional `MUL` writing rgb;
2. one operand a temp whose writer **sampled a texture** (`is_sampled_temp`'s own predicate,
   reached through the existing identity hop);
3. the other operand a clean `COL0` read, or a temp reached from one through nothing but
   **broadcast** constant scales — never a swizzle, negate or abs, and never a per-channel constant
   (the three rgb swizzle lanes must be equal, so a *tint* cannot be collapsed to one scalar);
4. the product **provably live into `col0.rgb`**, by the same no-kill backward walk `colour_mask`
   already uses, restricted to the instructions after the candidate.

**COL1 / ATTR2 is excluded by measurement, not by caution.** These same programs carry the packed
tangent-space **normal** in ATTR2 — `MAD H7.xyz, H7, {2,-1}` then `NRM` — so a rule that accepted
ATTR2 would replay a normal map as a colour. The existing terminal `is_vcol` accepts `attr_reg == 1
|| attr_reg == 2` and is saved only by `vcol_replayable()` requiring attr 1 downstream; the deep rule
requires attr 1 at the match site.

Latest match wins: a program that modulates twice has its final colour built by the later one.

#### 1c. PRE-REGISTERED OFFLINE, BEFORE THIS BUILD EVER RAN

The rule was reimplemented in Python from the same bit layout (`RSXFragmentProgram.h:29-140`,
`FPOpcodes.h:10-80`) and run over the stored ucode corpus. **MEASURED:**

| corpus | match | no match |
| --- | --- | --- |
| all 338 `.fp` files in `bin\remix_ucode\` | **22** | 316 |
| the 8 programs on this run's own `Remix fpother:` census that have ucode on disk | **7** | 1 |

Every one of the 22 reports a scale of **2.0** (19) or **1.0** (3) — no other value appears. The one
fpother program that does not match is `0DBB822C00810202` (fp `938cfb957fcb7e17`), a
**one-instruction program with `sampled=0x00`**: it samples nothing, so it correctly cannot have a
modulate. The corpus is heavily biased *against* the shape — `store_refused_fp_ucode` only stores
programs drawn with **no material**, and a material-less draw usually samples no albedo — so 22/338 is
a floor, not a prediction.

The four programs above were also hand-simulated instruction by instruction against the rule,
including every other `MUL` in each program, and in all four the match is **unique**: on
`34389B9B085589D5` twelve `MUL`s are examined and only slot 27 satisfies all four terms.

#### 1d. `RPCS3_REMIX_FPVCOLDEEPSCALE` (default 1) — the x2, and why it is folded

`apply_vertex_colour`'s `fold[]` already carries the constants the **vertex** ucode multiplies ATTR3
by (`c[18]` on every Haze world program — MEASURED, 44 `Remix vcolroute:` rows). It has never carried
the constant the **fragment** ucode multiplies COL0 by, because until this round no such program was
ever classified. That constant is **2.0 on every Haze program read**.

Remix's Modulate factor is an 8-bit unorm, so a factor above 1 cannot be represented and the fold
saturates above 0.5. Folding it is still the faithful reading: under the x2 convention **0.5 is the
unlit-neutral value**, so replaying COL0 raw would land every surface at half the guest's shading.

**MIND THE DIRECTION — this was written backwards once and caught before shipping.** The submitted
factor is `min(1, COL0 x 2)` at `FPVCOLDEEPSCALE=1` and plain `COL0` at `=0`, and `COL0 <= 1`, so
**1 is the BRIGHTER setting and 0 is the DARKER one** — and *both* can only darken relative to
today's no-modulate factor of 1.0. `=0` is therefore the lever for surfaces that come out **still
white / washed out**, or that show flat white patches where the x2 clips. There is no brighter
setting: at `=1` the ceiling is the 8-bit unorm factor itself.

**RGB only, and that is read from the bytes rather than assumed:** the same programs copy the alpha
lane across **unscaled** (`MOV R1.w, H1`), so folding it into `fold[3]` would invent an opacity the
guest never computes — the exact failure the alpha-route gate exists for.

#### 1e. The latent bug that would have eaten part of the fix

`apply_vertex_alpha` wrote `m_scratch_vertices[i].color = 0x00FFFFFFu | (alpha << 24)` — **forcing RGB
to white** — and it runs *after* `apply_vertex_colour` as an independent `if`, not an `else`
(the two statements at `RemixGSRender.cpp:19842` and `:19872`; the comment at the first of them says
so in as many words). Round 41's inbox flagged this as priority 4. On any textured draw that is both
fp-classified and blended with a constant-alpha albedo, it overwrote a freshly replayed vertex colour
with white.

Now `(color & 0x00FFFFFFu) | (alpha << 24)` — **under `FPVCOLDEEP`, and the gate matters.**

An earlier draft shipped it ungated on the argument that the mask is *"a strict identity on every
draw the old line could reach before this round"*. **That argument is false and review caught it.**
It is an identity only where `apply_vertex_colour` did not write — and `apply_vertex_colour` writes
on any draw the **terminal** classifier already named, i.e. the two `vcol_pass`/`vcol_modulate`
programs that predate this round. Ungated, `FPVCOLDEEP=0` would therefore *not* have been a bit-exact
round-41 control, and since the mesh content hash covers these bytes those meshes would re-create for
no reason. **A revert knob that does not revert is worse than the bug it hides**, so the knob owns
this line too: with `FPVCOLDEEP=0` it writes the literal `0x00FFFFFF` exactly as before.

Everywhere else the mask genuinely is an identity: every vertex leaves the decode loop at
`0xFFFFFFFF` (`RemixGSRender.cpp:3904` and `:18704`) and `apply_vertex_colour` is the only writer
between there and here.

#### 1f. THE FULL BOOLEAN CHAIN, GREPPED AND VERIFIED ARMED

Round 41's own instrument note: *"a knob that ANDs into the gate you are widening will silently
swallow the widening."* Every term checked against current bytes and against the last run's banner:

| term | site | state |
| --- | --- | --- |
| `vertex_colour_modulate_enabled()` | `fp_wants_vcol`, `RemixGSRender.cpp:19832` | `VCOLMOD=1`, launcher:1560 |
| `fp_vertex_colour_replay_enabled()` | same | `FPVCOL=1`, launcher:1503 |
| `vcol_replayable()` | same | **the gate this round widens** |
| `vertex_colour_disabled()` (`NOVCOL`) | `apply_vertex_colour:10927` | false — proven by `vcol_mod=10280 > 0` last run |
| `vcol_route_replayable()` | `:10937` and the blend ext | **`route=scaled` on every Haze world VP**, MEASURED |
| all three ANDed | blend ext, `:22605` | armed |

The `Remix vcolroute:` census, all 44 rows: `830d7d1b9681c475`, `f39f504649b6f442`,
`ad7ce9d672a0bf6b`, `c1d482dcd1b03ed0`, `d0b6a471bb2d463b`, `af06f6d32ec048ee`, `57a12323f22f4988`,
`c2003391127734f6`, `0214281b9a7a412d`, `bd1c10df5703e559`, `4d5a87bffbce0717` and eleven more all
read `route=scaled alpha_from_attr=1 slots=[c[18].x;c[18].y;c[18].z;c[18].w]`. **The vertex side is
ready for the entire world. The fragment classifier was the only thing in the way.**

`fpvcol_skygate` is NOT in this chain — it gates the self-lit/emissive verdict
(`RemixGSRender.cpp:20006`), not the colour replay.

#### 1g. THE READINGS, AND THE PRE-REGISTERED REFUTATION

| reading on `Remix live:` | meaning |
| --- | --- |
| `fpdeep=P/D` with P ~20-40 and D in the millions | **PASS** — the search named P programs and they account for D draws |
| `fpdeep=0/0` with `fpvcoldeep=1` on the banner | the search ran and matched nothing — a different failure from not running |
| `fpdeep=P/0`, P > 0 | the programs classify but never reach a **textured** draw; check `vcol_route_blocked` |
| `fpclass=a/b/c` — b must jump from 1 | b is the modulate program count |
| `vcol_mod=` — was **10,280** of 5,392,256 submitted (0.19%) | the number that has to move by orders of magnitude |
| `mesh_created=` — was **402,758** | expect ONE bounded re-creation of every world mesh (vertex colour is in the mesh key). Unbounded climb = an animated vertex colour minting a mesh per step |

**PRE-REGISTERED REFUTATION.** *Darker is the fix; wrong hue is the failure.* If surfaces come out
visibly wrong-**coloured** rather than merely darker, this is round 8's `RETRYUNSUP` repeating and
`RPCS3_REMIX_FPVCOLDEEP=0` reverts it in one line. If they come out **still white or washed out**,
`RPCS3_REMIX_FPVCOLDEEPSCALE=0` is the second, independent line — see the direction note in 1d: 0 is
the DARKER setting, not the brighter one.

#### 1h. Clamp audit

| knob | armed | clamp | note |
| --- | --- | --- | --- |
| `FPVCOLDEEP` | **1** | `env_u32(..., 0) != 0` | boolean, no ceiling to hit |
| `FPVCOLDEEPSCALE` | **1** | `env_u32(..., 1) != 0` | boolean; armed at its shipped default, kept as a documented one-line revert |
| `FPVCOLHOP` | 4 | `min(env, 8)` | unchanged; the deep search reuses the same hop |
| `VCOLMOD`, `FPVCOL`, `FPVCOLROUTE`, `VCOLFOLD`, `VCOLBGRA`, `VCOLCONSTBLACK` | 1 | boolean | all verified armed above |

Neither new knob is an `env_float`, so **neither can hit the `env_float` rejects-zero trap** that cost
round 41 the `GUESTLIGHTRADIUSSCALE=0` finding. The fragment fold is deliberately **not** inside the
`vcol_fold_enabled()` block, so `VCOLFOLD=0` does not silently disable it — and for the same reason
it does **not** set `folded`, so `vcol_fold_applied` keeps meaning "the *vertex* program's fold
fired". The consequence, stated rather than left to be derived: with `FPVCOLDEEP=1` the pack loop's
invariant *"fold[] is all-ones whenever `VCOLFOLD=0`"* no longer holds; `FPVCOLDEEPSCALE=0` is what
makes `fold[]` all-ones on that branch.

**Two further review findings, both now fixed in the shipped build.** (1) An earlier draft
snapshotted `hops_used` before the deep search and wrote it back afterwards, on the theory that it
was restoring the terminal walk's value — it was not, because the round-41 `srckind` census *also*
calls `hop_copies`, so the snapshot was already post-census. Both lines are deleted;
`result.out_rgb_hops` is assigned once from the terminal walk and never touched again. (2) **KNOWN
GAP, measured inert on this title:** the constant-vertex-colour-route branch returns before the fold,
so a program that is constant-route on the vertex side *and* deep-classified on the fragment side
does not get the x2. Both of Haze's constant routes are degenerate for it — `cval=[1 1 1 0]` is
already at the unorm ceiling and `cval=[0 0 0 1]` is refused outright on textured draws by
`VCOLCONSTBLACK`.

`RPCS3_REMIX_GUESTLIGHTAUTO=0` was left untouched at launcher:1244, as instructed.

---

### TASK 2 — THE ADS WHITE BLOOM AND THE 5x FRAMERATE COLLAPSE, MEASURED

Not fixed this round — the brief gates it behind task 1 — but **root-caused with numbers**, which is
what round 43 needs. All MEASURED from 468 `Remix timing:` windows in `bin\log\RPCS3.log`
(the timing line goes to `rsx_log.notice` only, `RemixGSRender.cpp:24622`, so it is **not** in
`remix_dump.log`; 380 of the windows are in-level).

| per frame | normal (n=353) | the episode (n=22) | ratio |
| --- | --- | --- | --- |
| frame_ms | 21.42 (46.7 fps) | **81.83 (12.2 fps)** | 3.8x |
| draw | 11.73 | 88.89 | 7.6x |
| **ui** | **2.26** | **64.88** | **28.7x** |
| **present** | **6.39** | **0.65** | **0.10x** |
| ui_px | 138,477 | **4,480,204** | 32.4x |

**`present` FALLS.** The path tracer was *starved*, not overloaded — the cost is 100% CPU-side in the
backend's own software UI compositor. Correlation with frame time: `ui r=+0.896`, `ui_px r=+0.894`,
everything else <= +0.52, `present r=-0.424`. Regression over 380 windows:
**`ui_ms = 14.57 ns x ui_px + 0.207 ms, R2 = 0.99825`** — 4.48M px/frame is **2.16 full-screen
CPU-rasterized layers per frame**. Of the +77.16 ms over a normal draw, **62.86 ms (81.5%) is `ui`**.

The worst window: `frame_ms=88.26 draw=88.89 (ui=65.12) present=0.63 scene=[1770 -31.65 1134]`.
All 22 slow windows are one continuous encounter with the camera frozen at that position for 21 s.
`Remix live:` deltas: `ui_pixel` (the pixel-space UI route) goes **1.0 -> 24.0 draws per frame** and `ui_uv` is stable to +/-30 across every slow window — a fixed, deterministic
24-draw overlay set.

**The visual half, MEASURED:** `compositor::blend()` in `RemixCompositor.cpp` implements **straight
alpha only** (`++m_pixels` at `:613` is what `ui_px` counts) — there is no additive path anywhere in the file, and `alpha == 0xFF` takes a
`std::memcpy` (`:633`), a hard overwrite. **INFERRED, and it is the obvious reading:** an additive
lens-flare / glare sprite with opaque white texels is composited as a solid opaque white disc rather
than as a glow. That makes the white bloom and the framerate one defect, not two.

**Ruled out by measurement — none of these moved during the episode (per-window delta 0.0):**
`mat_emissive` (485), `guest_lights` (128 = exactly `glmax`, pool saturated), `sunsprite`
(`1579/0` — **every one of 1579 observations rejected offscreen, zero accepted**), `suncard_elected`
(0), `fpvcol_emissive` (0), `fpvcol_additive` (0). The post-process surface cap did not fail open
either: `s_max_tracked_surfaces = 256` and the warning never fired.

---

### Instrument defects found this round

- **`glauto=%d` on both banners prints a BOOLEAN, not the mode** — the argument is
  `guest_light_auto_enabled() ? 1 : 0` (`RemixGSRender.cpp:929` and `:25983`). Round 41 armed
  `GUESTLIGHTAUTO=2` and every log line since has read `glauto=1`, which is indistinguishable from
  mode 1 — the mode that *"put lights on doors in August"*. Nothing was wrong with the run; the
  banner cannot state which mode it used. Worth one line in a later round.
- **`RPCS3_REMIX_EMISSIVEINT` is not a real variable.** The parser reads
  `RPCS3_REMIX_EMISSIVEINTENSITY` (`RemixTransforms.cpp:11219`); `emissiveint=` on the banner is a
  dump field only. The launcher sets `EMISSIVEINTENSITY`, so nothing is broken — but the short name
  appears in prose and would silently do nothing.
- **The `Remix uiwrap:` census is already re-keyed per stats window**
  (`RemixGSRender.cpp:13818`), so its 64-line cap is per window and not per run. Round 43 can
  name the ADS overlay draws from the existing census by matching the window to the slow frames; no
  new instrument is needed.
- **`mat_untested` on the stats line is correctly wired** to `tex.materials_untested`
  (`RemixTextures.h:176`) — it grepped as missing only because it lives in a different header.

### Lessons

- **A ratio is not a population.** `tex_none / (tex_none + tex_bound)` measured the walk's failure
  rate; the thing that renders is `notex_mat_applied / submitted`, and the two differ by a factor of
  757 on this title. Three rounds of briefs carried the first number as if it were the second. Before
  building a round on a counter, find the counter that partitions it and check the partition sums.
- **A matcher that reads one instruction cannot see a program.** The round-41 hop was safe, correct
  and aimed thirty instructions too late. When a classifier recognises two shapes in a whole title,
  the question is not "which extra shape" but "is it looking in the right place at all".
- **The answer was already in a comment.** Round 11 wrote `rgb = texRGB * (2 * COL0.rgb) * 0.944243`
  into `apply_haze_fade`'s doc block and moved on. Grep the repo's own transcriptions before
  disassembling.
- **Reimplement the rule offline and run it over the stored corpus before you build.** 22/338 and
  7/8 with one measured constant is a pre-registration; "it should match the wall programs" is not.

## Round 41 (2026-08-25)

Deployed: `bin\rpcs3.exe` = `bin\rpcs3-next.exe` = **`49D31EEA1D767004`**. Runtime `bin\remix\d3d9.dll`
**UNCHANGED at `16A0B512F33EBB66`** — nothing deployed into `bin\remix\`.

Tree at both ends: HEAD `4fdaecd67`, rounds 37-41 uncommitted. **The tree did not move.** Both exes read
`46D3DF83376C0391` at the start, exactly what round 40 recorded.

Round 40's build was play-tested. **The guest-light fix landed** — `guest_lights=31`, `guest_light_match=826`,
non-zero for the first time in the project. **And the user's verdict was "nothing got fixed", which is fair:**
the lights that appeared were the wrong lights, and everything else round 40 shipped was a knob A/B.

---

### TASK 1 — the light exists but was keyed on the wrong texture. Three separate defects, all measured

#### 1a. One albedo, a 154× spread of world extents, and no size gate on a listed hash

MEASURED, all 31 `Remix guest-light:` lines of the round-40 run. Every one is the same albedo
(`71D189E9B559A7F9`) on the same `(vp, fp)` pair, and their world extents are:

```
0.5385  0.5389  0.5396  0.5399  0.5579  0.5579  0.5607  0.5646  1.731
7.893  8.047  8.095  8.453  8.456  8.469  10.02  11.16  12.54  14.52  14.58
20.33  21.60  21.62  21.64  21.59  22.17  30.08  30.11  30.13  58.94  83.18
```

**Nine rows at 0.54 .. 1.73 and twenty-two at 7.89 .. 83.18**, with a **4.6× gap** between them. The small
group are the bulbs (1.731 is the user's own Ctrl+Click). The large group is a different mesh sharing the
texture. The 83.18 row produced `radius=29.1` placed at the AABB centre of an 83-unit mesh — **that is the
"too big and not aligned with the bulbs" disc in the user's screenshot**, and its position is not wrong so
much as meaningless: the centroid of an 83-unit object is not where any bulb is.

`small_enough` (`world_extent <= GUESTLIGHTMAXEXT`, default 6) already existed and already gated the census
and the AUTO trigger. It **did not gate an explicitly listed albedo**, and no comment in the file ever argued
for that exemption — it is an oversight, not a decision. Round 41 applies it, behind
`RPCS3_REMIX_GUESTLIGHTLISTEXT` (default 1; 0 restores round 40 bit-exactly) with a new counter
`guest_light_toobig` on `Remix live:`. The default ceiling of 6 lands inside the measured 4.6× gap, so the
separation is on data rather than taste.

`guest_light_match` deliberately stays upstream of the rejection: **match − toobig is the accepted
population**, and folding the rejection into `match` would make "wrong list" and "over-sized fixture" read as
the same number.

#### 1b. The radius was a constant, and on a small bulb the emitter was bigger than the bulb

`sphere.radius = max(GUESTLIGHTRADIUS, world_extent × GUESTLIGHTRADIUSSCALE)`. The launcher armed
`RADIUS=0.6` and `RADIUSSCALE=0`, and at world scale ≈ 1 unit = 1 m that is a **60 cm** emitter forced on
every fixture. On the measured 0.54-extent bulbs the floor won outright — the light sphere was **larger than
the bulb**.

**`RADIUSSCALE=0` never meant what the file says it meant.** `env_float` rejects 0 and returns the accessor's
fallback of **0.35**, and the live banner has been echoing `glradiusscale=0.35` all along. The accessor's own
comment claims "0 falls back here and the caller then takes `max(fixed, 0)` = the fixed radius", which is
wrong: the fallback is 0.35, not 0. Round 40 recorded this trap and left it; round 41 writes the value
explicitly so the file and the behaviour agree.

Now `RADIUSSCALE=0.5`, `RADIUS=0.1` — the emitter is exactly the mesh's own half-extent, a sphere that fills
the fixture and no more, with a floor low enough never to bind on a real one.

**Radiance follows, and this is INFERRED, not measured on screen.** A sphere light's radiance is per unit
area, so total power goes as r². Round 40 ran r = 0.6; round 41 runs ≈ 0.10 .. 0.17 on the measured glow
cards. `(0.6/0.15)² = 16`, so equal power would want ~2400. `GUESTLIGHTRADIANCE` is armed at **1200**, a
deliberate half-step — expect *slightly dimmer* than round 40 rather than blown out. It is the only
brightness knob; too dark → 2400 then 4800, too bright → 600 then 300.

#### 1c. The discriminator for "only SOME bulbs are lit" — and it needs no list at all

MEASURED, `Remix light-candidate:` grouped by (albedo, state):

| albedo | state | n | vtx | lum | mean_rgb |
| --- | --- | --- | --- | --- | --- |
| `F613BD83DAF2B4E2` | glowcard | 82 | 22, 23, 26 | 0.8155 | `[0.8424 0.8314 0.5781]` warm white |
| `0F86FCEDC4226D3B` | glowcard | 64 | 4 | 0.9375 | `[0.9375 0.9375 0.9375]` |
| `A0D0AB03F0BB1D3C` | glowcard | 59 | 200, 36, 64 | 1 | `[1 1 1]` |
| `577B02B123B0F15F` | glowcard | 53 | 80 | 1 | `[1 1 1]` |
| `099D2DA136CEA2C4` | glowcard | 53 | 40 | 0.9938 | |
| `9CA366166DF12A90` | glowcard | 27 | 116, 12, 16 | 0.9813 | `[1 1 0.7412]` yellow |
| `9E95F66ECE26BE29` | **fixture** | 23 | 216, 8, 846 | 0.7226 | |
| `16B46EA28EEDFC8E` | **fixture** | 22 | 8 | 0.7223 | |

Three facts settle it:

1. **`71D189E9B559A7F9` — the hash round 40 armed — is not in the census at all.** The backend's own fixture
   classifier never nominated it. We were creating lights keyed on a texture nothing recommended.
2. Every `blend=1 depth_write=0` row is a **glow card**: a small additive billboard (4 .. 200 vertices)
   carrying the game's own lamp tint. None of them is the bulb texture.
3. `state=fixture` is a separate, larger, opaque population (216 and 846 vertices) at a flat `lum ≈ 0.72` —
   the housing's metal, not a light.

**The hypothesis, INFERRED and this run is the test:** Haze draws an additive glow card over a fixture that
is **lit** and omits it for one that is not. If that holds, the rule is *"create a light where a glow card is
drawn"*, which reproduces the raster reference's "not every bulb is lit" **with no per-bulb list**, and gives
each light the card's own measured `mean_rgb` and its own extent-derived radius for free.

Shipped as `RPCS3_REMIX_GUESTLIGHTAUTO=2` — glow cards only. Mode 1 keeps the round-6 meaning (fixtures **and**
cards) exactly, so old runs stay comparable; mode 1 is what put lights on doors in August, so **0, not 1, is
the fallback**.

It also retires the `(vp, fp)` key for this population: `F613BD83DAF2B4E2` alone is drawn by **five vertex
programs and twelve fragment programs**, so no single pair can name it. Mode 2 does not consult
`vp_ok`/`fp_ok` at all — those gate `trigger_match` only.

**PRE-REGISTERED REFUTATION:** if the plant ends up with far more lights than it has visibly lit fixtures, or
`guest_light_capped` starts climbing, the glow card is not the "is lit" signal and the discriminator is
something else. `guest_lights=` and `guest_light_capped=` on `Remix live:` size it directly.
`GUESTLIGHTMAX` raised 64 → 128 because mode 2 lights a whole level rather than one texture and
`GUESTLIGHTIDLE=0` keeps them forever; the clamp is 4096, so 128 is nowhere near a ceiling.

#### 1d. And the vp/fp narrowing now applies to the primary list only

It was ANDed against **both** albedo rules, which made the two lists share one global program pair and meant
only one fixture family could ever be lit per run. Severed for the glow rule and kept for the primary one,
because the two carry different amounts of built-in selectivity: the primary rule is albedo alone, and the
launcher's 2026-08-15 note records by measurement that an albedo alone is **not** the fixture identity on
Haze (it put lights on doors); `glow_match` already carries its own state discriminator — blend enabled AND
depth write off — which is the very thing the vp/fp pair was standing in for. Round 16 makes the same
argument for `SUNCARDVP` in as many words.

---

### TASK 2 — the white walls and the uncoloured lava: the fix, and the no-op it nearly was

Round 40's diagnosis stands and is unchanged: every one of 192 `Remix alphastate:` rows reads
`tcolor=1/0/3` (*colour = albedo texture, vertex colour discarded*) while 22 of 47 vertex programs — the wall
program `ad7ce9d672a0bf6b` among them — carry a fully replayable `route=scaled slots=[c[18].x..w]`. The gate
is the **fragment** classifier, which recognises two shapes in the whole title (`fpclass=1/1/0`,
`fpvcol_applied=698` of 5,783,237 submitted draws = 0.012%).

#### 2a. `RPCS3_REMIX_FPVCOLHOP` — step through identity temp copies before classifying

`scan_fragment_program` classifies the **last** instruction writing `col0.rgb` and accepts only
`MOV out, COL0` or `MUL out, tex, COL0`. A program that builds the recognised shape in a temp and then
copies it out — `MUL rB, rA, COL0` … `MOV col0, rB` — is `other`.

The hop walks backwards through **identity copies**: an unconditional `MOV` of a `TEMP` with identity
swizzle and no negate and no abs. **That is the identity function**, so replacing a copy with its own
producer cannot change what the program computes, and hopping one **cannot admit a shape the classifier did
not already recognise**. It only reaches programs that write the recognised shape and then export it. That
safety is the whole design — `RPCS3_REMIX_RETRYUNSUP` is on record in this project as the cost of widening a
matcher until something matched, and this widening cannot mis-match. Applied to the rgb walk, the alpha walk
and to `is_sampled_temp`'s producer lookup. Clamped to 8, armed at 4, **default 0** (round-40 behaviour
bit-exactly).

**PRE-REGISTERED, and this is how it fails:** a program doing real arithmetic between the modulate and the
export — a fog lerp, a specular add, a `MAD` — is not reached and stays `other`. **The reading is `fpclass=`
on `Remix live:` moving off `1/1/0`, and `hops=` > 0 on `Remix fpvcol:` lines.** If `fpclass` stays `1/1/0`
the hop is not the gate.

#### 2b. The census that never existed: `Remix fpother:`

`Remix fpvcol:` has only ever printed programs that **did** classify, so a title whose answer is "two of
them" produced two lines and no way to see what the other hundred and forty look like. The new line prints,
once per unique fragment program (it sits inside the `m_fp_fingerprints` cache miss, so no dedup set is
needed), the terminal instruction the walk ended on: opcode, source count, predication, hops used, each
slot's register type, and a `kind` bitfield — `1` clean COL0/COL1 read, `2` temp whose writer sampled a
texture, `4` temp, `8` non-identity swizzle, `0x10` negated, `0x20` abs.

A row reading `op=MUL k0=2 k1=1` is the modulate shape and should already have classified; a row reading
`op=MAD`, or `k1=9` (COL0 but swizzled), **names the exact widening the next round has to write**. Bounded to
64 lines. This is the deliverable if 2a does not reach.

#### 2c. `VCOLMOD=0` would have made all of it a no-op, and the reason it was 0 is now root-caused

`fp_wants_vcol = vertex_colour_modulate_enabled() && fp_vertex_colour_replay_enabled() && vcol_replayable()`.
**`RPCS3_REMIX_VCOLMOD` was armed at 0**, so no textured draw reaches `apply_vertex_colour` whatever the
classifier decides. Arming the hop without arming this would have shipped a change that could not alter one
pixel — the exact defect class this project has shipped before.

It was turned off on 2026-08-15 with the note *"the HUD gauges render BLACK with this on … most likely those
meshes carry no COL0 attribute at all and the replay is feeding zeros"*. That guess is wrong in its
mechanism and right in its symptom, and the real mechanism is measured: `Remix vcolroute:` names exactly two
constant-route programs on this title and one of them resolves to

```
vp=b01bfce3fc580e3b route=constant cval=[0 0 0 1]
```

**a constant vertex colour of pure black.** ("No COL0 attribute" cannot be it — `map_attribute(3, …) != ok`
already returns without writing.) On an untextured draw that constant *is* the colour and round 12 is right
to replay it. On a **textured** draw it reaches Remix as a Modulate factor, and a Modulate by zero cannot
make a surface more correct — it can only delete it.

`RPCS3_REMIX_VCOLCONSTBLACK` (default 1) refuses a constant-route replay whose resolved RGB is under
`1/255` **on a textured draw only**; the untextured path is bit-exact. `1/255` is the threshold because the
pack quantises to 8 bits, so it refuses exactly the values that would have packed to 0. Refusing leaves the
decode loop's white, i.e. the albedo unmodified — the same identity this file already chooses for a missing
texture and for an alpha the route cannot prove. Counter `vcol_const_black` on `Remix live:`.
`apply_vertex_colour` grew a `textured` parameter for it; both call sites pass their own `material != 0`.

**If the gauges go black anyway: `VCOLMOD=0`, one line.** And report `vcol_const_black` — **if that counter
is 0, the black came from somewhere else and the guard is aimed at the wrong mechanism.** That is the
pre-registered refutation.

**Second watch: `mesh_created=`.** Vertex colour is hashed into the mesh key, so an animated vertex colour
mints a mesh per step. It was **582,659** with `VCOLMOD` off.

---

### TASK 3 — THE CHAPTER LIST — the premise was wrong, and that is this round's most useful result

**Do not go hunting for a "second consumer" until you have read this.** The brief for round 41 stated that
the `Unlock All Chapters` patch is provably applied and that the list is still truncated, therefore round 39
found one of at least two paths. The patch *is* applied — the log shows `Applied patch` for all five Haze
entries under the matching hash, with per-anchor entry counts `(<- 1)` and `(<- 2)` matching the `be32` line
counts exactly. **But the patch has never once been observed to unlock the list on its own.**

**MEASURED from file mtimes, SHA1s and the round-39 transcript timestamps (local TZ = UTC-5):**

| local time, 22 Aug | event |
| --- | --- |
| 00:40:29 | **15 chapter gates re-zeroed in all four pak members** (`15 gates -> 0`) |
| 00:41:17 | verified `gates all0=True n=15` |
| 00:50:43 | `patch.yml` backed up, chapter patch written |
| 00:51:53 | that round writes: *"revert the archives but keep the patch. If 15 chapters still list, the patch alone carries it"* |
| 00:57:31 | play-test: **all 15 chapters list, all stars lit** — **with the archives still edited** |
| 01:18:57 | all six paks restored to stock — **and no play-test after** |

The decisive "revert the archives, keep the patch" test **was proposed and never run**. Every Haze archive is
now byte-identical to its pristine backup (`71423f03.pak` `35fd02d8…`, `71423f03.pak.00` `e396850b…`,
`misc.pak` `3b6ed5f8…`, `patched.pak` `1579f0f0…`), so the stock gate values are live and the list is
truncated. The one effect cleanly attributable to the PPU patch is the **star pips**. This contradicts
`reference_haze_unlock_chapters.md`, which claims the two PPU patches work with data 100% stock — **that
claim was never verified and should be corrected.**

### And the mask gate is provably not the limiter

Recovered verbatim from the round-39 capstone output (MEASURED-FROM-RECORD; the ELF itself is gone, see
below). The function starts at **`0x4dd830`**, not `0x4dd840`:

```
0x004dd830  5484103a  slwi   r4, r4, 2
0x004dd834  38840110  addi   r4, r4, 0x110
0x004dd838  7c8407b4  extsw  r4, r4
0x004dd83c  7c632214  add    r3, r3, r4
0x004dd840  80630004  lwz    r3, 4(r3)      <- the patch target
0x004dd844  4e800020  blr
```

`u32 GetChapterMask(void* profile, int idx)` → `*(u32*)(profile + 0x110 + idx*4 + 4)`. A six-instruction
leaf; `li r3, 0x7fff` cannot fault. A full-image `bl` scan finds **exactly three callers** — `0xabb4c`,
`0x4f4a00`, `0x4f4a24` — all of `0x4dd830`, so the patch reaches every one and there is no unpatched sibling.

The list builder is `feflCallbackSelectChapterBuildChapterList` @ **`0xab1a0`**, and its gate is:

```
0x000abb4c  bl    0x4dd830      ; r3 = mask
0x000abb54  lwz   r0, 0(r24)    ; onlyDisplayOnceChapterUnlocked
0x000abb68  slw   r0, r25, r0
0x000abb74  and   r9, r0, r3
0x000abb80  cmpwi r9, 0
0x000abb94  beq   0xabce0       ; bit clear -> skip
```

The gate values are **MEASURED as exactly 0..14** (`levelselect_full.res`, member `6ba2da6d`, plaintext,
9484 B, decoded independently twice). With `r25 = 1` and the mask forced to `0x7FFF`,
`(1 << g) & 0x7FFF != 0` for every `g` in `[0, 14]`. **This test cannot truncate the list.** That is the
pre-registered refutation firing, and it is why the membership filter is most likely a *different* comparison
against the same `onlyDisplayOnceChapterUnlocked` field — which explains why zeroing the field worked and
widening the mask did not.

### The untraced lead: `0x4f4b40`, and it matches the user's own observation

`CheckPointLoad` @ `0x179b48`:

```
0x00179b90  mr   r4, r28        ; r28 = chapter index just loaded
0x00179b9c  bl   0x4f4b40       ; <- records the chapter as a plain integer?
0x00179bc0  slw  r5, r5, r0     ; 1 << (chapter-1)
0x00179bd0  bl   0x4f4a90       ; -> 0x4dd770
0x00179c08  bl   0x4f49b8       ; per-player fold, mask 0x7FFF
```

The completion path at `0x647280` calls the same pair with **`li r4, 0xf`** (chapter 15) and `li r5, 0x4000`.
So `0x4f4b40(profile, chapterIndex, x)` is written on every checkpoint load with the current chapter and
hard-coded to 15 on completion — exactly the shape of a "furthest chapter reached" integer, and exactly what
the user describes when they say the list tracks their furthest checkpoint. **`0x4f4b40` has never been
disassembled and its readers have never been enumerated. That is the first thing round 42 should do.**

Sibling accessors already counted: `0x4dd848` (OR into the halfword at `profile+0x110`) 2 callers,
`0x4dd858` (bit test on it) 1 caller `0x4f48f0`, `0x4dd878` (`lwz r3, 0x1cc(r3)`) 8 callers.

### BLOCKER: the decrypted ELF is gone, and no second offset is being shipped without it

It was at `…\scratchpad\haze\eboot\EBOOT.elf` (16,167,616 B, 22 Aug 00:43); that subtree no longer exists.
Searched C:, D:, E:, F: for `*.elf` > 2 MB plus name matches under `rpcs3\bin`, `AppData\Local\Temp`,
`.claude` and `E:\PS3 Games` — only RPCS3's own `bin\test\*.elf` and unrelated PS2 files. The Ghidra MCP
refused connection on `127.0.0.1:8080`, so there is no fallback. The SELF cannot be read directly: section
entries 0 and 1 of `dev_hdd0\game\BLUS30094\USRDIR\EBOOT.BIN` are `encrypted=1 compressed=0` at file
offsets `0xa80` / `0xde0a80`, and the word at the mapped location for vaddr `0x4dd840` reads `0x8c6cd243` —
high entropy, not an `lwz`.

**Proposing a patch offset that cannot be read from the file would violate this project's own reporting
standard, so none is proposed.** Two grounded options instead:

1. **Zero-risk and the only thing ever observed to work:** re-apply the archive gate-zeroing. All four
   members, v1.00 id set (`c4e3`) on disc and v1.36 (`c514`) on HDD. The exact tooling and chunk-table
   handling are in the round-39 transcript.
2. **To find the real gate**, regenerate the ELF headlessly (this is the command the earlier round ran; it
   does not boot the title, but it *does* run `rpcs3-next.exe`, so run it deliberately rather than as a side
   effect):
   ```
   SP=".../scratchpad/haze"; mkdir -p "$SP/eboot"
   cp ".../bin/dev_hdd0/game/BLUS30094/USRDIR/EBOOT.BIN" "$SP/eboot/EBOOT.BIN"
   cd ".../bin" && ./rpcs3-next.exe --decrypt "$SP/eboot/EBOOT.BIN"
   ```
   Then, in order: disassemble `0x4f4b40`; `bl`-scan for its callers and for any getter reading the same
   field; and dump `feflCallbackSelectChapterBuildChapterList` from `0xab2b0` through `0xabd00` — the
   recorded window stops at `0xabbc0`, so the loop bound and any second compare are still unseen.
   `vaddr = file_offset + 0x10000` is confirmed: PH0 reads `off=0x0 vaddr=0x10000 filesz=0xddc748`.

One cheap fully-grounded throwaway if a test is wanted first: change `0x004dd840` to `0x3860ffff`
(`li r3, -1`). Same instruction form, cannot fault, widens the mask past bit 14 — it covers the cases
`gate >= 15` or `r25 != 1`. **By the analysis above it should NOT help**, so a null result is further
confirmation that the gate is not the limiter. Nothing under `bin\patches\` was modified this round.

---

### Clamp audit — every knob this round touched, against its armed value

| knob | armed | reader / clamp | headroom |
| --- | --- | --- | --- |
| `GUESTLIGHTLISTEXT` | 1 (default) | `env_u32(…, 1) != 0` | boolean |
| `GUESTLIGHTAUTO` | **2** | `min(env_u32(…, 0), 2)` | **at the ceiling — and 2 is a real mode, not a reserved value.** Mode 3 does not exist; if a third is ever added the clamp must move with it |
| `GUESTLIGHTMAX` | 128 | `clamp(env, 1, 4096)` | 32× |
| `GUESTLIGHTRADIUS` | 0.1 | `env_float(…, 0.2)` | ok, > 0 |
| `GUESTLIGHTRADIUSSCALE` | **0.5** | `env_float(…, 0.35)` | ok — and it is now written explicitly, so the `0`-means-fixed trap can no longer fire |
| `GUESTLIGHTRADIANCE` | 1200 | `env_float(…, 30)` | ok |
| `GUESTLIGHTMAXEXT` | 6 | `env_float(…, 6)` | **armed at its own default — a no-op line, kept for documentation** |
| `GUESTLIGHTLUM` | 0.7 | `env_float(…, 0.7)` | **armed at its own default** |
| `FPVCOLHOP` | 4 | `min(env_u32(…, 0), 8)` | 2× |
| `VCOLCONSTBLACK` | 1 (default) | `env_u32(…, 1) != 0` | boolean |
| `VCOLMOD` | 1 | `env_u32(…, 1) != 0` | **armed at its own default — the launcher line is now a no-op, and that is deliberate: the line documents the black-HUD history and is the one-line revert** |

`GUESTLIGHTAUTO=2` sitting exactly at its clamp ceiling is called out on purpose — that is the
`STATICINDEXBUDGET` trap's shape. It is safe here because 2 is the highest **defined** mode, not a value the
clamp is silently truncating; a sweep upward is meaningless rather than blocked.

### Instrument notes worth keeping

- **A knob that ANDs into the gate you are widening will silently swallow the widening.** `VCOLMOD=0` would
  have made every line of task 2 unobservable. Before shipping a matcher change, grep the full boolean chain
  the matcher's result feeds and check every term is armed.
- **A census that prints only its successes cannot tell you why it is failing.** `Remix fpvcol:` printed two
  lines for two classified programs and was read for rounds as "the classifier works". `Remix fpother:` is
  the missing half.
- **"Most likely X" in a knob's disable note is a hypothesis, not a finding, and it will be inherited as
  fact.** The 2026-08-15 black-HUD note blamed a missing COL0 attribute; the code already returns on that
  case, and the real cause was a constant route resolving to `[0 0 0 1]` — which the `Remix vcolroute:`
  census had been printing all along.
- **When one texture is shared between a fixture and a prop, extent separates them and the hash cannot.**
  0.54 .. 1.73 against 7.89 .. 83.18 on one albedo, with a 4.6× gap.

## Round 40 (2026-08-25)

Deployed: `bin\rpcs3.exe` = `bin\rpcs3-next.exe` = **`46D3DF83376C0391`**. Runtime `bin\remix\d3d9.dll`
**UNCHANGED at `16A0B512F33EBB66`** — nothing deployed into `bin\remix\`.

Tree at both ends of the round: HEAD `4fdaecd67`, rounds 37-39 uncommitted. **The tree did not move this
round.** Both exes read `E4E2A9A4EDAECE2A` at the start, which is exactly what round 39 recorded.

**The round-39 build got a real play-test.** `bin\log\RPCS3.log` (44 MB, unlocked) and `bin\remix_dump.log`
(996 MB) hold a complete 64,139-flip / 15,526,016-draw session ending 2026-08-25 09:38, with 14 Ctrl+Click
picks. Everything below is measured against that run unless it says otherwise.

---

### TASK 1 — THE HEADLINE. Haze has had **no analytical lights at all**, and there are two independent reasons

This is the finding of the round and it retires a large part of the standing lighting defect list at once.
MEASURED, final `Remix live:` line of the round-39 run:

```
guest_lights=0  guest_light_match=0  guest_light_capped=0  guest_light_reaped=0   mat_emissive=419
glalbedos=0  glradiance=150  glradius=0.6  glmax=64
```

Zero guest lights across 64,139 flips. And the camera fill that was supposed to be covering for them is
inert. So the entire lighting budget of every level is **one distant sun** plus emissive materials.

#### 1a. `GUESTLIGHTALBEDO2` was unreachable dead code, and blanking one launcher line in August killed every light in the game

`maybe_inject_guest_light` armed on

```cpp
const bool trigger_match = remix_rsx::guest_light_albedo_any() && vp_ok && fp_ok
    && (primary_match || glow_match);
```

`guest_light_albedo_any()` is `return guest_light_albedos().count != 0;` and reads **only** the primary
`RPCS3_REMIX_GUESTLIGHTALBEDO` list. On 2026-08-17 that list was blanked at the user's request ("remove the
added lights that are on the doors") — its only entry was the door hash. From that moment the leading term
was false for **every draw in the process**, so `glow_match`, i.e. the whole of `GUESTLIGHTALBEDO2`, could
never be reached no matter what was armed. The launcher's own note said to watch `guest_lights=` drop to 0.
It dropped to 0 and stayed there, and nothing connected that to the second list also dying.

`guest_light_albedo_any()` has **exactly one consumer** — that expression — so the fix is local:

```cpp
const bool guest_light_armed = remix_rsx::guest_light_albedo_any() || configured_albedo2 != 0;
```

`glow_match` already re-checks `configured_albedo2 != 0` together with its blend / no-depth-write state, so
this restores reachability and widens neither rule by one draw.

#### 1b. `RPCS3_REMIX_CAMLIGHT` has done nothing since round 5, and the launcher comment saying otherwise is false

`place_debug_light` did `DestroyLight(0x3)` then `CreateLight(0x3)` every frame. MEASURED against the
deployed runtime's own tree (`dxvk-remix-numos3`, branch `numos3`, HEAD `6476faea`):

- `remixapi_DestroyLight` only **queues** — `rtx_remix_api.cpp:1613`, `s_pendingLightDestroys.push_back(handle);`
  is the entire body.
- It drains in `remixapi_Present` (`:2123`) → `unregisterPersistentExternalLight` + `removeExternalLight`
  (`:2136-2137`) → deferred **again** to `m_pendingExternalLightErases` (`rtx_fork_light.cpp:191`) → erased in
  the flush loop (`rtx_fork_light.cpp:46-60`), which runs inside `prepareSceneData` **before** linearization.

So the per-frame order was: queue destroy → create emplaces immediately (`rtx_remix_api.cpp:1536-1539`) →
Present queues the erase → the flush erases **the light this function just created**. The
`DrawLightInstance` activation went with it, because the pending-activate loop (`rtx_fork_light.cpp:79-86`)
requires the handle to still be in `m_externalLights`. The tombstone guard at `rtx_remix_api.cpp:2127-2128`
covers only `remixapi_CreateLightBatched`, which this is not.

`RemixGSRender.cpp` has carried a comment saying exactly this since round 23 and left it deliberately so the
sun A/B would not be confounded. **Round 40 fixed it**: destroy removed, same-hash create (what
`update_sun_light` already does, and `Remix sun-submit: retargets=1 destroys=0 draw=SUCCESS` proves works),
handle committed through a local so a failed re-create cannot null a working light, and `isDynamic = 1`.

**`isDynamic` is required here and is NOT required on the guest fixture lights**, and getting that backwards
would have been a silent no-op fix. MEASURED in the fork:

| light | lifecycle | `addExternalLight` arm | `updateLightStaticSleep` | verdict |
| --- | --- | --- | --- | --- |
| guest fixture | created once, never updated | emplace (`rtx_light_manager.cpp:728-730`) | **never called** | `isDynamic=0` is **harmless** |
| camera fill (0x3) | re-created every frame | existing (`:724-727`) | called every frame | `isDynamic=0` **freezes it after 50 updates** |
| sun (0x4) | re-created on retarget | existing | called | already sets `isDynamic=1`, correct |

`getNumFramesToPutLightsToSleep()` = `numFramesToKeepLights()/2` = **50** (`rtx_options.h:2492` over the
default 100 at `:647`); no override in `bin\rtx.conf` or `bin\user.conf`. And `isStaticCount` is **never
reset** for an external light — the only reset (`rtx_light_manager.cpp:556`) is on the game-converted
`m_lights` path. Static sleep is also **not a cull**: `rtx_fork_light.cpp:143-146` merely skips the
`*light = newLight` copy, so a slept light keeps emitting from stale data — silent, not dark.

**The one filter that does remove an external light from the frame** is
`light.getColorAndIntensity().w <= 0` (`rtx_light_manager.cpp:378`, `:424`), zero only when
`max(r,g,b) < FLT_MIN`. If a guest light ever appears to contribute nothing, check that, not `isDynamic`.

`garbageCollectionInternal` never iterates `m_externalLights` (it walks `m_lights` at `:112` and
`m_externallyTrackedLights` at `:135`), so API lights are structurally exempt from GC.

#### 1c. What is actually lighting the level today

MEASURED, `Remix sun-submit:` at frames 62400 / 63000 / 63600:

```
travel=[0.58884 -0.075284 0.80473] radiance=3 angle=0.5 aimed=1 retargets=1 destroys=0 draw=SUCCESS
```

One distant sun, **radiance 3**, aimed nearly **horizontally** (`y = -0.075`) after a single `SUNSKY`
retarget — not the armed `SUNDIR=0.1223,-0.9444,-0.3051`. Plus `mat_emissive=419`. That is the whole of it.
An interior lit by one grazing distant light, with auto-exposure lifting a nearly-unlit scene, is a
sufficient explanation for **"walls white"** and **"black void floor"** at the same time, and it is the
first hypothesis that accounts for both without needing a texture defect.

#### 1d. What is armed, and the pre-registered readings

| knob | was | now | why |
| --- | --- | --- | --- |
| `GUESTLIGHTALBEDO` | *(blank)* | `71D189E9B559A7F9` | the user's own light-bulb pick |
| `GUESTLIGHTVP` | `830D7D1B9681C475` | `C1D482DCD1B03ED0` | the bulb's own program |
| `GUESTLIGHTFP` | `BD80201C29B6B01E` | `796BC90574F89CA1` | the bulb's own program |
| `CAMLIGHT` | `12` (inert) | `0` | the mechanism now works; 12 would blow out everything near the player |

The bulb pick is `vp=c1d482dcd1b03ed0 fp=796bc90574f89ca1 albedo=71D189E9B559A7F9 vtx=124 ext=1.731`,
which is a **different program** from the pair that was armed — so the bulb could never have produced a
light even with the arming bug fixed. The launcher's own 2026-08-15 note set the precondition for moving
these ("blank them again only if a fixture drawn by a different program is confirmed by a pick"); this pick
is that confirmation.

**PRE-REGISTERED. `guest_lights=` on `Remix live:` must be > 0 and `Remix guest-light:` lines must appear in
`bin\remix_dump.log`. If `guest_lights` is still 0, this whole task is wrong and nothing else in it is worth
reading.** `guest_light_match=0` with `guest_lights=0` means the (vp, fp, albedo) triple still misses;
`guest_light_match>0` with `guest_lights=0` means `CreateLight` refused.

**KNOWN LIMIT, and it is the next round's job.** The vp/fp narrowing is a single **global** pair ANDed
against **both** albedo rules, so only one fixture family can be lit per run. The right shape is parallel
comma lists (the `UIFORCEPAIRVP2`/`FP2` idiom, entry *i* of VP paired with entry *i* of FP), which would let
the bulbs and the floor-recessed fittings be lit together. **Do not just blank `GUESTLIGHTVP`/`FP`**:
`71D189E9B559A7F9` is bound by 8 distinct (vp, fp) pairs in the log, one of which is the smoke program
`ad7ce9d6/aa0fe222` — blanking would put a sphere light inside every smoke puff, which is the same failure
the 2026-08-15 door note records.

---

### TASK 2 — three standing explanations for the white / vanishing plant walls are REFUTED

#### 2a. The static-index budget is no longer the mechanism. This contradicts rounds 34 and 38

Round 34 concluded *"the white walls are the static-index budget, not culling"*, and round 38 named eviction
as the real limiter with both `dropped` exits gated on the budget. MEASURED at `STATICINDEXBUDGET=128`, last
`Remix static-index:` line of the run:

```
entries=7419 triangles=1471247 rebuilds=9657 deferred=122 stale=1 dropped=121
peak=128 budget=128 resident=171 evicted=7248 nomesh=0
```

Every one of those is **cumulative** (only `m_static_index_rebuilds` resets, per frame). So:

- `peak == budget == 128` — round 38's pre-registered check ("`peak=` must stop pinning at `budget=`")
  **failed**: the per-frame budget is still reached.
- But `dropped=121` in **15,526,016 draws = 0.00078%**, and `stale=1`. Raising the budget 8 → 128 took the
  observable consequence of saturation to essentially zero even though saturation still occurs.
- `evicted=7248 / entries=7419 = 97.7%` reproduces round 38's 98.9% — **and it does not matter**. An evicted
  entry that is drawn again is simply rebuilt (`union_exists` false → budget available → rebuild); that is
  what `rebuilds=9657` is. It only becomes the invisible `dropped` exit when the budget is *also* spent, and
  that happened 121 times all session.

**A saturated high-water mark is not a saturated budget.** `m_static_index_rebuild_peak` is a monotone
max and can read `budget` after a single frame in 64,139. Round 38 read the peak and inferred the cost; the
cost has its own counter and it is three orders of magnitude too small.

#### 2b. The texture path is not producing white either

MEASURED, and every identity below is exact to the unit:

```
tex_unsupported 2,155,582 − tex_tombstone 2,155,581               =         1 fresh refusal
tex_retry_refused 1,923,563 + tex_unit_retry 232,019              = 2,155,582
tex_none 3,763,672 − tex_no_unit 1,840,109                        = 1,923,563
notex_skip 3,667,916 + notex_world 93,080 + notex_mat_applied 2,676 = 3,763,672
```

So the 2.16 M "unsupported" hits are **one** texture descriptor plus 2,155,581 tombstone re-hits of it. The
whole 44 MB `RPCS3.log` contains exactly **one** `Remix texrefuse:` line — `format fmt=92 2048x2048`, i.e.
`CELL_GCM_TEXTURE_DEPTH16`, the shadow map. Corroborated over 11,936 `Remix notex:` census rows: 3,145
`class=retry-refused reason=format fmt=0xb2 dims=2048x2048` and 8,791 `class=no-unit`; **100% of format
refusals are that one shadow map**, on four half-res programs at `clip=512x288`, never the main pass.

Every format a PS3 wall diffuse uses is supported — DXT1 `0x86`, DXT23 `0x87`, DXT45 `0x88`, A8R8G8B8
`0x85`, D8R8G8B8 `0x9E`. The refused set is depth buffers, HILO normal maps and float formats.

And a material-less draw does **not** render white today: `notex_mat_applied=2,676` of 2,676 eligible got the
**grey 0.5** neutral material (`NOTEXMAT=1`, `Remix notexmat: created ok (albedo 0.5 linear, SRGB)`), 97.5%
were skipped outright, 2.5% were world-refused. The "a material-less surface renders WHITE" comment
describes pre-`NOTEXMAT` behaviour.

All 14 picks read `material=1 albedo_unit=0` with sane UVs, and `uv_applied/tex_bound = 99.03%`.

#### 2c. Also refuted: `mat_untested` does not mean the peak-UV walk was skipped

`mat_untested=22832 / mat_created=23693 = 96.4%` is set at `RemixTextures.cpp` `if (!alpha_tested)` from
`rsx::method_registers.alpha_test_enabled()`. It means 96.4% of materials were created while the RSX alpha
test was **off**, which is a statement about where transparency comes from on this title (the blend path:
`blend_translucent=2,894,707`), not about UV measurement. `peak_uv` has no counter at all.

#### 2d. What is armed instead

Two armed knobs are now known to delete or de-occlude main-pass geometry, and both are A/B'd this round:

- **`SKIPVP=` (was `4D5A87BFFBCE0717`).** That program has been dropped **wholesale since before the forge
  record began**, and it is one of the four the `WDIVWALK` decode fix repaired (`57A12323F22F4988`,
  `4D5A87BFFBCE0717`, `96EDAAED0C27FD05`, `1D9A973AF5CD1514`) — so the giant-geometry reason for dropping it
  has been fixed for several rounds. The launcher's own "STAGE 2" block has been asking for this relaunch
  ever since. MEASURED that it eats **main-pass** geometry, not an aux pass:
  `gate=skipvp vp=4d5a87bffbce0717 fp=97c0b2e9be86fd48 clip=1024x576` and
  `... fp=aa5f822213201b4b clip=1024x576`, against `mainclip=1024x576`. `skip vp=24569` draws over the run.
  This is the strongest single candidate for the missing wooden plank floor.
- **`SKYCLASSIFY=1` (was 2).** See task 3.

#### 2e. A NEW live candidate for "white texture not albedo": the walls' vertex colour is thrown away

MEASURED, and it is not a particle-only problem. **Every one of the 192 `Remix alphastate:` rows in the run
reads `tcolor=1/0/3`** — `Arg1=Texture, Arg2=None, Modulate`, i.e. *"colour = albedo texture, vertex colour
discarded"* — and `vcol_applied=93,778` of the 6,390,568 gauge-placed draws is **1.47%**.

Meanwhile the `Remix vcolroute:` census says Haze's world geometry has a perfectly replayable vertex colour:
of 47 classified programs, **19 are `route=scaled` and 3 are `route=passthrough`**, and the wall/world
program itself reads

```
Remix vcolroute: vp=ad7ce9d672a0bf6b route=scaled alpha_from_attr=1 slots=[c[18].x;c[18].y;c[18].z;c[18].w]
```

so does the bulb program `c1d482dcd1b03ed0`, and so do `f39f5046`, `bd1c10df`, `d0b6a471`, `af06f6d3`,
`a7505f7a`, `c2003391` — the whole world family. **ATTR3 × c[18] is available and is being discarded.**

The gate is the FRAGMENT classifier, not the vertex one: `fp_wants_vcol` requires
`m_current_fp_fingerprint->vcol_replayable()`, which is true only for `MOV out, COL0` and
`TEX; MUL out, tex, COL0`. MEASURED `fpclass=1/1/0` — **two programs in the entire title**. The in-code
reasoning is sound and explicit (*"a textured draw's ATTR3 modulates its albedo in the title's own fragment
program with per-title semantics this backend does not read, so tinting one would be a guess"*); it is the
narrowness of the classifier that is the defect.

**If Haze authors a neutral base texture and carries the copper tint per-vertex — which is what
`route=scaled alpha_from_attr=1` with a per-draw `c[18]` factor looks like — then "white texture not albedo"
is literally this, and it is the same root cause as the uncoloured lava smoke in task 6.**

**No zero-code lever exists for it.** `VCOLMOD=1` alone cannot help (`fp_wants_vcol` ANDs in the fragment
gate); `FPVCOL=0` moves the wrong way; `FPVCOLROUTE` is the *vertex* route gate, a different predicate.
Widening `scan_fragment_program` past its two hard-coded 1- and 2-instruction shapes is the unlock, and it
unlocks the walls and the particles together. That is round 41's biggest single item.

**The decisive measurement this round could not take: nobody has ever picked a white wall.** All 14 picks
landed on textured objects that resolved correctly. No counter downstream of `CreateMaterial` exists, so
nothing in the instrumentation can separate "the material is right and Remix renders it white anyway" from
"the wrong texture is attached" from "the wall is skipped and something else shows through". One Ctrl+Click
on the white wall answers it directly.

---

### TASK 3 — the round-39 sky classifier failed its own pre-registered check

MEASURED, the single `Remix skyclassify:` line of the run:

```
albedo=23A3978F1405B16E ARMED dome=61 other=0 total=61 agree=1 |
vp=af06f6d32ec048ee vtx=152 wext=24728.6 upv=162.688 inside=1 depth_write=0 |
listed=0 promoted=1 armed=1 setsize=1 overflow=0 | cam=[1746.6 -71.676 1049] frame=59921
```

with `skyclassify_armed=1 promoted=1 entries=1 failed=0 overflow=0` and
`Remix skypromote: content=23A3978F1405B16E mat 23A3978F1405B16E -> 87005760088C3126 ok (2048x512)` — the
material-hash fold **did** fire, so round 39's aliasing guard works.

Against round 39's own four checks:

1. `D1A6D1B27ADE6232` / `CDFE11B12552EA2D` did **not** arm — but this session never left the plant, so this
   is untested rather than failed.
2. Ravine untested.
3. `skyclassify_armed=1`, against a pre-registered band of **8..20**. Below the band.
4. `armed − promoted = 0`, which round 39 pre-registered as **"a reason to distrust it, not a pass"**, and
   `listed=0` confirms it: the one hash it found is not one the user had found by hand.

It armed at `cam=[1746.6 -71.676 1049]`, inside the copper-plant region the other picks came from
(X 1746..1806, Z 1049..1228) — the exact level reported as having white walls that vanish at angles. A
promoted hash gets `blendType = 6` (`kEmissive`): *opacity → 0, emissive influence 1, occludes nothing*.
Emissive **and** non-occluding is what "white" plus "disappears at angles" looks like.

**Stated honestly: the census argues it is a real backdrop** — 2048×512, raw box `[-12370..12370]`, 61 of 61
draws dome-shaped, `upv=162.7`. So this may well be a correct promotion. That is why it is an A/B and not a
deletion. `SKYCLASSIFY=1` keeps every counter and changes no pixel.

---

### TASK 4 — props bounce on WALKING: the round-38 model is missing its translation term

Round 38 modelled the residual as *"rotated about the eye by one frame of camera turn, so its displacement
is (distance from eye) × (turn per frame)"*. That model predicts **no bounce at all when the player walks in
a straight line**, which is exactly the user's report it fails to explain.

The model is right about the mechanism and wrong about the terms. `world = fused(t) · A(t−1)⁻¹` where
`A = V·P`, and Remix then renders with `V(t)P(t)`, so the composite error is
`V(t)P(t)P(t−1)⁻¹V(t−1)⁻¹`, which with a constant projection is `V(t)V(t−1)⁻¹` — **a rigid transform with a
rotation part and a translation part**. The translation part is one frame of camera *translation*. So a prop
is displaced by roughly the distance the player moved that frame, in the opposite direction, whether or not
they turned. That is the bounce.

MEASURED that the population really is on the stale branch, from 180 `Remix pick-follow:` lines for the
first-person arms (`vp=830d7d1b9681c475 albedo=86885A0E60751491`, vtx=3649):

```
180/180  ref=anchor_prev   frame − anchor_frame = 1   frame − cam_frame = 0   zfold=1
```

and run-wide `gauge_prev=1,029,650` with `gauge_prev_camfresh=1,013,828` = **98.46%** of those having a
current-frame elected camera. The four gauge branches are mutually exclusive and partition the population
exactly: `gauge_used 4,566,060 + gauge_cur_dims 396,132 + gauge_prev 1,029,650 + gauge_absent 398,726 =
6,390,568 = world_ref_fresh 6,303,857 + world_ref_stale 86,711`. So the stale branch is **16.11%** of
gauge-placed draws — *not* 22.6% of `gauge_used`, which is only the first branch and is the mistake this
line is written out longhand to prevent.

Residual jitter on the arms, n=4000 `Remix vmbasis:` frames, `cdist_pre` frame-to-frame |Δ|:
`p50=0.00284  p95=0.07428  p99=0.10012  max=3.63`. At ~1 unit ≈ 1 m that is 7.4 cm at p95 and 10 cm at p99
every frame.

**PRE-REGISTERED TEST that separates the two models, and it needs no build.** Walk in a straight line
without turning, past a near prop and a far prop.
- Round 38's rotation-only model: **no bounce**, and any bounce that does occur scales with distance from
  the eye.
- The translation term: **bounce on both**, with an amplitude equal to the player's per-frame displacement,
  roughly **independent of distance**.

#### Round 38's "needs no new machinery — only a third branch here" is REFUTED

The staged lever was: in the `anchor_prev` branch, when the elected camera is fresh, divide by the camera
instead. `reference` already defaults to `&m_active_camera.reference_inverse`, so the branch itself is two
lines. **But it is not free, and this is why it was not shipped this round.**

`divide_anchor` is the **only** carrier of the f64 inverse. The high-precision divide is
`if (divide_anchor && divide_anchor->inverse_f64_valid && gauge_f64_enabled())`, and
`m_active_camera` has **no f64 twin** — `camera_state::reference_inverse` is `remix_rsx::mat4` (f32).
Taking the camera reference therefore drops ~1.01 M draws/run out of the f64 path
(`world_div_f64=5,991,842` today) into the f32 one, whose own in-tree measurement is *"~1.0 of translation
error"* on an ill-conditioned anchor. Trading a measured **0.10-unit** p99 lag jitter for a possible
**1.0-unit** precision error is a regression, not a fix.

**The honest version of the lever is: give `camera_state` an f64 reference inverse computed at latch time,
then add the third branch.** Seven assignment sites touch `reference_inverse`
(`RemixGSRender.cpp:2693, 2978, 3440, 15946, 16191, 16238, 16486`). That is next round's work and it is
worth doing — it is the one change that addresses "props bounce" and "viewmodel does not track" together.

Second, independent viewmodel defect, unchanged this round: `zfold=1` on **180/180** arm draws — the anchor
folded `0.49875/0.50125` while the arms render at `0.00125/0.00125`. Run-wide
`gauge_zfold_mismatch=492,957` = 7.69/frame.

---

### TASK 5 — the viewmodel camera is NOT missing, and `VMCAMFOVX/Y` are NOT inert

The brief carried `vmcam_considered=0` as evidence the VIEW_MODEL camera never runs. **Refuted, and the
counter never touched the camera.** `m_stats.viewmodel_cam_considered` is incremented inside
`per_draw_transform` behind `if (viewmodel_place)`, and
`viewmodel_place = viewmodel_draw && !viewmodel_tag_only()` — with `VMTAGONLY=1` that is unconditionally
false, so `considered / applied / fallback / refused / census` all stay 0 **by design**.
`RemixTransforms.h` pre-registers exactly that: *"with 1 set, vm_tagged stays non-zero while
vmcam_considered goes to ZERO"*. MEASURED `vm_tagged=106,243` with `vmcam_considered=0`. Prediction met.

The real submission counters are on `Remix live:` only: **`vmcam_twin=29343 vmcam_real=29343`** against
`cam_resolved=29343` — a VIEW_MODEL camera reached the runtime on **every frame the world camera resolved**,
and 100% of them took the `vm_real` branch that carries the armed `60.001 / 36.132`. The knobs are live.
They are simply **not echoed** on `Remix run-start:` (0 `VMCAMFOV` tokens in `RPCS3.log`), which is why five
rounds could not tell.

`cam_fallback=34,796` (54.25% of flips) is also not what it looks like: it **stops accumulating at frame
34,848**. The single `Remix cam-elect:` line reads `latch_frame=34796 fallback=34796`; from frame 34,849 to
64,139 the camera resolves at 100%. The 70° hardcoded FOV in `submit_debug_scene` is the menu/loading
camera, not the gameplay one.

**The gameplay FOV has never been measured.** `to_camera_matrix` is a plain copy (no transpose, unlike
`to_remix_transform`) and `guarded_setup_camera` validates nothing, so whatever the guest projection carries
is what Remix is told. The only line that prints it is behind `CAMTRACE`, which has been 0 — MEASURED zero
`fov=` tokens anywhere in the run. **`CAMTRACE=1` is armed this round** for one session.

One measurable oddity to carry forward: the elected camera is a **512×288** pass while world geometry
anchors on **1024×576** — `Remix gauge: camclip=512x288 anchor_clip=1024x576 basis_delta=0.00391388`, which
matches the predicted 512/510 ratio to four significant figures. Same aspect, so it is a scale
disagreement, not an aspect error.

---

### TASK 6 — the lava smoke and bubbles are uncoloured at THREE independent gates, each sufficient

Nothing was changed for this; it needs code the round did not have room for. The mechanism is now fully
named, which is what round 41 needs.

The one line that settles it, from `bin\remix_dump.log`:

```
Remix alphastate: albedo=099D2DA136CEA2C4 vp=2d5186a8011589b8 fp=50a10ad7974fdfd7 vtx=4 |
  blend_on=1 src=1 dst=7 op=0 ... verdict=kept |
  talpha=1/0/1 tcolor=1/0/3 vcbake=1 tfactor=0xFFFFFFFF
```

`tcolor=1/0/3` is `Arg1=Texture, Arg2=None, Modulate` — the **parity fill**, i.e. *"colour = albedo texture,
vertex colour discarded"*. All five measured particle programs print the same triple, and every particle
vertex ships `color = 0xFFFFFFFF`. The surface Remix receives is the raw albedo at full intensity, which is
exactly "smoke is bright, bubbles look uncoloured".

**Gate 1 — the fragment classifier names two programs in the entire title.** `fp_out_source` has three
values and only two real shapes: `MOV out, COL0` (1 instruction) and `TEX; MUL out, tex, COL0`
(2 instructions). Everything else is `other`. MEASURED `fpclass=1/1/0` and `fpvcol_applied=698` of
**5,783,237** submitted draws = 0.012%. All 15 `Remix picked:` lines read `fpcol=other fpalpha=other
fpvcol=0`. With `fpcol=other` the arg-source replay is skipped and the parity fill ships. The additive
particle's own fragment ucode is on disk at `bin\remix_ucode\E07AC460430042B6.fp`, **256 bytes = 16
instructions** — the classifier can only ever match 1- and 2-instruction shapes.

**Gate 2 — `scan_vcol_route` misclassifies all six particle vertex programs as `computed`, and the fix is
22 lines below the bail-out.** All six decode to the identical shape (indices given per program):

```
MUL rN.xyzw = ATTR3, c[467].xxxx
MUL o1.xyz  = rN.xyz, c[66].xxxx      ; COL0.rgb = ATTR3.rgb x c[467].x x c[66].x
MOV o1.w    = <computed fade ramp>    ; COL0.w is NOT ATTR3.w
```

`2D5186A8` 39/41/55 · `2FC99887` 37/38/57 · `4ECE0A28` 46/48/62 · `946A6296` 16/18/74 · `315E2138`
28/29/49 · `391B10C3` 12/14/69.

**So the hue IS per-vertex, in ATTR3** — both constants are scalar broadcasts and can only scale brightness.
`RemixTransforms.cpp` bails at `if (vec_writemask(in) != 0xf || in.d3.index_const) { vcol_route::computed; }`
where `in` is the **last** instruction writing `o1` — the `MOV o1.w`, mask `0x8`. The register is rejected
wholesale because two instructions write different lanes of it. The code's own comment admits the blast
radius: *"29 of the 82 cached programs land here (masks 0x7 and 0x8)."* And `vcol_lane_source` — a per-lane
backward walker over MOV/MUL chains through temps, ≤3 hops, constant co-operands, i.e. **exactly this
shape** — sits 22 lines further down and is never reached. It would have produced `scaled_chain` with
`slots=[c[66].x|c[467].x]` and `alpha_from_attr=false`.

Corroborated: `Remix vcolroute:` (47 lines) reads `route=computed` for **all six** particle programs, while
the two picked programs read `route=scaled slots=[c[18].x;c[18].y;c[18].z;c[18].w]` — their vertex side is
perfectly replayable.

**Gate 3 — `VCOLMOD=0` keeps `apply_vertex_colour` off every textured draw**, so particle vertices ship
`0xFFFFFFFF`. Ranked last because gate 1 also closes it: `fp_wants_vcol` requires `vcol_replayable()`, so
flipping `VCOLMOD=1` alone changes nothing on these draws.

**REFUTED as suspects, both named in the brief:**

- **`fpvcol_skygate=83772` is not the particles.** The gate can only fire when `self_lit_shape` is already
  true, which requires `!material` **and** `sampled_mask == 0`. Both picked particles have `material=1` and
  `sampled=0x1f` / `0x1`. It is structurally impossible. What the 83,772 actually is: the 82-vertex sky
  dome, 83772/64139 flips = **1.31 per frame**, one dome draw per frame.
- **The additive blend translation is fine.** `blend_pairs` decodes as src/dst/**op** = count:
  `6/7/0` SRC_ALPHA/ONE_MINUS_SRC_ALPHA = 1,839,343; `1/1/0` ONE/ONE = 694,748; `1/7/0` premultiplied =
  247,105; **`6/1/0` SRC_ALPHA/ONE — the picked additive particle (`bsrc=770 bdst=1`) = 113,511**. None
  carries the `*` verdict mark, `classify_blend` lists `(6<<8)|1` explicitly as `kept`, and the four sum to
  2,894,707 = `blend_translucent` exactly, so the census is complete. **ADDITIVE is represented and
  survives.**
- **`vcol_const=0` does not mean `VCOLCONST` is inert.** The banner echoes `vcolconst=1`; the counter only
  increments in `apply_vertex_colour`'s flat branch, which with `VCOLMOD=0` is reachable only for untextured
  draws, and of the 6 constant-route programs three are already on `SKIPRTVP`/`SKIPUNTEXTUREDFPPAIRVP`. The
  condition never occurred.

**`blend_astranded=684,265`** is a textured blend-enabled draw whose albedo alpha is constant and whose
ATTR3 alpha substitute also failed. Split, MEASURED from `Remix alphastate:`: `315e2138` (ribbon,
`albedo_alpha=255..255`) and the picked additive `af06f6d3` (`alpha=255..255`) **are** in it; `2d5186a8`
(0..197), `2fc99887`, `946a6296`, `391b10c3` are not.

**LATENT BUG to fix at the same time as the gates, not after.** `apply_vertex_alpha` writes
`color = 0x00FFFFFFu | (alpha << 24)` — it forces RGB to **white** — and it runs *after* the
`material && fp_wants_vcol` call to `apply_vertex_colour`, as an independent `if`, not an `else`. The moment
gates 1-3 open, any textured draw with a constant-alpha albedo will have its freshly replayed RGB
overwritten white by the alpha rescue. It is harmless today only because the RGB is already white.

**Two zero-code separating tests, for whoever runs this next** (do **not** run them in the same session as
this round's light/geometry A/B):

- `RPCS3_REMIX_FPVCOL=0` → **predicted pixel-identical**, because only 698 of 5.78 M draws take that path
  and none is a particle. Any visible change refutes gate 1.
- `RPCS3_REMIX_FPVCOLROUTE=0` → makes `vcol_route_replayable()` return unconditional true. Then Ctrl+Click a
  smoke draw and read `tcolor=`. Still `1/0/3` ⇒ the route gate is not binding and gate 1 alone is.

### Clamp audit — every knob this round touched, against its armed value

| knob | armed | reader / clamp | verdict |
| --- | --- | --- | --- |
| `GUESTLIGHTALBEDO` | 1 hash | `std::array<u64,16>` list | 16× headroom |
| `GUESTLIGHTVP` / `FP` | 16 hex | `read_hash_env`, `wchar_t[32]` | fits |
| `GUESTLIGHTMAX` | 64 | `clamp(env, 1, 4096)` | 64× headroom |
| `GUESTLIGHTRADIANCE` | 150 | `env_float`, default 30 | ok |
| `GUESTLIGHTRADIUS` | 0.6 | `env_float`, default 0.2 | ok |
| `CAMLIGHT` | **0** | `env_float(..., -1.f)` then `env >= 0 ? env : cfg` | **`env_float` rejects 0 → falls through to `g_cfg.video.remix.camera_light`, which is `0` in BOTH `bin\config\config.yml` and `custom_configs\config_BLUS30094.yml` (verified). So 0 really is off — but only because the config agrees.** |
| `SKYCLASSIFY` | 1 | `min(env, 3)` | ok |
| `CAMTRACE` | 1 | `wcstol != 0` | ok |
| `SKIPVP` | *(blank)* | `read_hash_env` → 0 → gate off | ok |

**A trap found and NOT changed, deliberately:** `GUESTLIGHTRADIUSSCALE=0` is read by `env_float` with a
fallback of **0.35**, so the launcher's stated intent ("0 = use `GUESTLIGHTRADIUS`") is defeated — the live
line echoes `glradiusscale=0.35` and the radius is `max(0.6, extent × 0.35)`. The accessor's own comment
claims 0 "falls back here and the caller then takes `max(fixed, 0)` = the fixed radius", which is wrong: the
fallback is 0.35, not 0. For the bulb (extent 1.731) this gives 0.606 instead of 0.600 and does not matter;
for a 6-unit fixture it would give 2.1. Left alone this round so it cannot confound the first run in which
guest lights exist at all.

**Three knobs armed to their own shipped default, i.e. no-ops:** `NOTEXMAT=1` (default 1),
`NOTEXCENSUS=1` (default 1), `TEXVERIFY=120` (default 120).

### Instrument notes worth keeping

- **`wdiv=` and `wdiv_shadowed=` are printed inside the `skip …` group of `Remix stats:` and are not
  skips.** They are `m_stats.wdiv_draws` / `wdiv_shadowed_draws`, positive decode counters. `wdiv=12,934,988`
  read as a refusal is a 12.9 M-draw phantom.
- **A monotone high-water mark cannot price a per-frame budget.** `peak=budget` and `dropped=121` are both
  true; only the second is a cost. Whenever a counter is a `std::max` over the run, find the cumulative twin
  before concluding anything.
- **`guest_light_albedo_any()` is the general shape of the bug in 1a: a cheap "is the feature armed" guard
  that reads only one of the feature's two inputs.** When a rule grows a second input, grep the arming test,
  not just the matching test.
- **The camera-fill defect is the same class as the sun's, one lifecycle over.** `isDynamic` matters for
  *updated* lights and not for *create-once* lights; `DestroyLight` matters for *same-frame re-create* and
  not otherwise. Both were assumed uniform and neither is.

## Round 39 (2026-08-24)

Deployed: `bin\rpcs3.exe` = `bin\rpcs3-next.exe` = **`E4E2A9A4EDAECE2A`**. Runtime `bin\remix\d3d9.dll`
**UNCHANGED at `16A0B512F33EBB66`** — nothing deployed into `bin\remix\`.

MSBuild `Release|x64` exit **0**, **0 errors, 1 warning** — the pre-existing C4723 "potential divide by 0",
now at `RemixGSRender.cpp:11197` (it was `:11067` in round 38; it moved because this round inserted lines
above it). **Zero new warnings.**

**THE TREE MOVED UNDER THIS ROUND, and this is the first time the concurrency trap has actually fired.**
At the start HEAD was `44276ae06` with rounds 37–38 uncommitted and both exes at `447D3904D1E969B2`. By the
first build HEAD read **`4fdaecd67`** *"Remix: unify viewmodel/prop drift as a one-frame donor lag"* — another
session committed rounds 37–38 (source, `KNOBS.md`, `launch-haze-remix.cmd`, both inbox files) mid-round.
**Nothing was lost**: a commit moves HEAD and the index, not the working tree, and no `checkout`/`reset`/
`stash` was run here. Verified after the fact — `git diff --stat` against the new HEAD is exactly this
round's additions, and the launcher still carries round 38's armed values (`GAUGECURDIMS=1`,
`STATICINDEXBUDGET=128`, `UIFORCEPAIRVP2` with three slots). **The rule that saved it: hash and check at
BOTH ends, and never run a git command that writes the working tree.**

---

### TASK 1 — the sky, by classification. Shipped as `RPCS3_REMIX_SKYCLASSIFY`

#### The root cause is the material, and it is measured rather than argued

A dome renders as a lit sky only if its texture **content hash** is on `RPCS3_REMIX_SKYEMISSIVE`, because
that list is what makes its material emissive (`RemixTextures.cpp`, the `sky_emissive` local in
`texture_cache::upload`). The launcher carries six hashes; `haze_domes.csv` names **sixteen** distinct dome
resources game-wide.

MEASURED over `bin\remix_dump.log`, `mat_skyemissive=`/`mat_skyunordered=` across every stats line:

| reading | lines |
| --- | --- |
| `mat_skyemissive=0 mat_skyunordered=0` | **14,726** |
| `mat_skyemissive=11` | 1,177 |
| `mat_skyemissive=5` | 1,108 |
| `mat_skyemissive=43` (largest seen) | 697 |

Entire sessions in which the sky-emissive material was **never created once**, against sessions on a listed
level that reach 43. That is the black sky, stated as a mechanism.

#### The brief's stated constraint on `CATEGORY_BIT_SKY` is REFUTED against current bytes

The round-39 brief carried forward that *"`CATEGORY_BIT_SKY` sets `m_isHidden` on this remixapi backend"*,
which would have made "tag the dome as sky" an own-goal and forced a separate visibility mechanism. **That
is not true of the deployed runtime.** `dxvk-remix-numos3` (branch `numos3`, HEAD `6476faea` — the tree
`RemixRuntime.cpp:143` records as matching the deployed `d3d9.dll`):

- `src/dxvk/rtx_render/rtx_remix_api.cpp:915` —
  `prototype.cameraType = (cameraType == CameraType::Sky) ? CameraType::Main : cameraType;`
  with the comment *"promoting them to CameraType::Sky here would hide the instance with nothing left to
  draw it"*.
- `src/dxvk/rtx_render/rtx_instance_manager.cpp:1005` — the hide is gated on
  `drawCall.cameraType == CameraType::Sky`, which by the line above an external draw can never be.

This backend's own header already said so and was not read: `RemixTransforms.h`'s category table reads
`RPCS3_REMIX_CAT_SKY -> SKY (selects the sky camera; does NOT hide - see above)`. Corroborated from the log:
`af06f6d32ec048ee` (Haze's dome program) carries `TAGGED` rows for `CDFE11B12552EA2D` — a hash the user
identified by clicking the sky — on levels where the sky demonstrably works.

**Named limit, because it is not free either.** `SceneManager::submitExternalDraw`
(`rtx_scene_manager.cpp:2388`) still takes `getCamera(state.cameraType)` for `worldToView` / `viewToProjection`,
and this backend never calls `SetupCamera` for `CameraType::Sky`. So a SKY-tagged external draw is placed by
`objectToWorld` (correct) but carries an uninitialised sky camera's view/projection. Not this round's defect
and not touched; recorded so the next round does not discover it as a surprise.

#### The existing `SKYHASH` rule was the obvious lever and IT REJECTS OUR GROUND TRUTH

`sky_hash_mode()` has shipped at mode 1 (measure, tag nothing) for several rounds and already learns "albedo
hashes that only ever appear on dome-shaped draws". Promoting it looked like a two-line round. MEASURED, every
`Remix sky-hash-census:` line in the log:

```
albedo=D1A6D1B27ADE6232  reject:mixed  dome=1 other=1  vp=af06f6d32ec048ee vtx=304 wext=26351.6 upv=86.68
albedo=CDFE11B12552EA2D  reject:mixed  dome=1 other=1  vp=af06f6d32ec048ee vtx=372 wext=27194.1 upv=73.10
```

**Both are on the user's own `SKYEMISSIVE` list.** The rule's units-per-vertex floor is
`s_sky_backdrop_min_units_per_vertex = 100` (`RemixGSRender.h:1737`) and those domes' own latitude bands
measure 86.68 and 73.10 — and one non-dome draw disqualifies a hash for the life of the process. A tessellated
dome is a stack of bands of **differing** density, so the strictest band disqualifies its own texture. That is
structural, not a tuning miss, and it is why round 39 ships a second rule rather than a mode on the first.

#### The predicate, and where every threshold's number comes from

Population: every `Remix sky-census:` line in the whole 969 MB `bin\remix_dump.log`, parsed to unique rows,
filtered to `depth_write=0 AND inside=1 AND wext >= 2000` — **187 unique rows, 23 vertex programs, 46 albedo
hashes**. Four families fall out:

| family | programs | `wext` | `vtx` | `upv` | what it is |
| --- | --- | --- | --- | --- | --- |
| A | `8eae853c96f5a07e`, `f352d7dafa72d0e0`, `595b8e2fa69b566b`, `5c9cfb394074a479` | 1.22e9 .. **1.12e18** | 8..420 | 8.5e6 .. 1.8e16 | broken transform — the mined `farPlane` is **14000**, so the world fits in ~1.4e4 units |
| B | `d736a5bdc6e2552a`, `c1781a2e32aba35d`, `a426abcdd77da419`, `41c59a3a2bfc71bf`, `d4dcf86a2f42d60b` | 7.0e5 .. **2.14e6** | 42..266 | 6.2e3 .. 4.0e4 | **domes** — carries `35C2353F6B3CE2A8`, `174F4F689CF2A3D8`, `3213E0CC136ED294`, all hand-listed |
| C | `af06f6d32ec048ee`, `fc0fac8afccec49a` | 2000 .. 2.09e5 | 33..747 | 3.4 .. 689 | **domes** — carries `D1A6D1B27ADE6232`, `CDFE11B12552EA2D` |
| D | `2c62e34057e58c21`, `487c71da8d277fb0`, `aae8e0d5ae292dd4`, `788113cb1bad5321`, `333a616a4093f353` | 2.0e3 .. 7.8e4 | 96..**12875** | 4.7 .. **49.28** | terrain |

So:

| gate | value | measured justification | gap |
| --- | --- | --- | --- |
| `depth_write == 0` | — | domes write no depth; already the `sky_candidate` gate | — |
| camera **inside** the transformed AABB | — | not `|translation − eye|`, which collapses to `|eye|` for an absolute-world draw (round 36: `anchor=2137.85` against `limit=4`) | — |
| `wext >= SKYEXTENT` | 2000 | existing knob, config default, ceiling 1000000 | — |
| `wext <= SKYCLASSIFYMAXEXT` | **4.0e6** | between family B's max **2.14e6** and family A's min **1.22e9** | **570×** |
| `vtx <= SKYCLASSIFYMAXVTX` | **1024** | between the domes' max **747** and terrain's min **2714** | **3.6×** |
| `vtx >= SKYCLASSIFYMINVTX` | **16** | the smallest hand-listed dome row is **33 vertices**; the floor drops one named false positive, a **4-vertex** quad spanning 32,331 units with the camera inside (`AC936E2F25F147B0` on `3c9186d8e026cec5`) | **2.1×** |
| `upv >= SKYCLASSIFYUPV` | **60** | between **23.57** (highest on any row the vertex ceiling excludes) and **73.09** (lowest on any hand-listed dome row that clears the extent and vertex gates) | **3.1×** |

**A CLAIM THIS ROUND MADE AND THEN WITHDREW.** An earlier draft of this section said the `upv` gate was the
narrow one at 1.34×, between "terrain at 49.28" and "listed domes at 66.12". **Both rows were mis-assigned** —
the 66.12 row carries `albedo=0000000000000000` (untextured, so not a listed-dome row at all) and the 49.28
row sits *inside* the vertex ceiling rather than outside it. Re-measured properly the gap is 3.1×, and the
more useful correction is this: **on this data `upv` is very nearly redundant with the vertex ceiling** —
every row the ceiling excludes is also below 60 — so the separating power is really coming from the extent
bounds, the vertex bounds and `inside`, not from `upv`. Caught by re-deriving the number for the report
rather than trusting the one already written into the source.

**Replayed before shipping**, which is the closest thing to a dry run this rule can have. Applying every gate
to every `Remix sky-census:` row in the 969 MB log admits **19** distinct albedo hashes with the vertex floor
off and **18** with it on. **All six hand-listed dome hashes are among them** — the ground-truth check passing
on historical data before the play-test sees it. The other 12 are candidates, not confirmations: the census is
deduplicated per (program, outcome) and carries no draw counts, so it cannot evaluate the 8-draw /
no-disqualification / settle-window rule that actually arms a hash. **Expect the live number to be lower than
18.**

**The extent CEILING must not be tightened to something "sensible".** Three of the six hand-listed domes live
in family B at 7e5..2.1e6, so a ceiling anywhere near the world's 1.4e4-unit size would delete them. That was
this round's first design idea and the data refuted it.

**`SKYANCHORMODE=1` is also wrong and the same data says so.** Haze's dome reads `canchor=4687..5360` — the
AABB centre of a dome sits ~5000 units above an eye that is at its origin — so a `centre_anchor` limit of 4
rejects it. Mode 2 (`inside`) is the right shape and is what this rule uses.

#### Per-hash, not per-draw, and the one irreversible failure it can produce

Emissiveness is a property of the **material**, which is per texture, so a hash admitted on one draw glows on
all of them. A hash arms when it has been dome-shaped `SKYCLASSIFYMIN` times, has never been seen on a
non-dome draw, **and has been known for `SKYCLASSIFYSETTLE` frames**. The settle window is the guard on the
only failure that cannot be undone — a material rebuilt emissive cannot be rebuilt back — and it is sized
from the older rule's own observation that a shared texture's disqualifying draw arrives inside the first
second. MEASURED support for the hazard being real: `94EC83A8E7989A15` armed at `wext=2.15e6 upv=33605` and
went `reject:mixed` **14 frames later** at `wext=21.86 upv=0.55`.

#### The two halves of making a promotion actually reach the screen

1. **The material.** `texture_cache::promote_sky_emissive()` (new) rebuilds the material of every live entry
   carrying the hash, and re-runs the `peak_uv` walk. The ordering it exists to fix: a dome's texture is
   uploaded — and its material created — on the *first* draw that binds it, which is strictly before
   `submit_subdraw()` has the world-space AABB it needs. Without this the promotion is a no-op for the run.
   The old material handle is **orphaned, never destroyed** — `mesh_entry::material` records that a Remix mesh
   bakes its material at `CreateMesh` with no API to re-point it, so destroying it is the dangling-handle
   failure the round-9 reap split exists to prevent. Round 17 already priced orphaning at one material per
   event; here the event count is bounded by the title's 16 domes.
   `upload()` was split into `measure_peak_uv()` + `build_material()` by a verbatim move, so both callers run
   the same code. The only behavioural edit is that the `CreateMaterial` failure arm no longer destroys the
   texture — `upload()` does, which is where it belonged.
2. **The mesh.** The mesh key folds `sky_emissive_promoted()`. Without it every mesh created before the
   promotion keeps submitting the old, non-emissive handle and the dome stays black regardless of what the
   texture cache did. Safe as a key because promotion is **monotone** — the bit flips false→true once and
   never back. Deliberately a re-key and not an erase: erasing mesh entries is what turns a static-index entry
   into the invisible `dropped` exit round 38 measured behind the vanishing plant walls.

`sky_emissive_albedo_matches()` now returns `list-match || promoted`, and that one edit is what gives a
classified dome the emissive material, the `peak_uv` walk **and** the per-level sun at once — all three
already asked exactly that predicate.

#### An instrument defect, caught before it shipped

The census's `listed=%d` field originally re-queried `sky_emissive_albedo_matches()`. At mode 2 the promotion
has already inserted the hash by the time the census runs, so **every armed hash would have printed
`listed=1`** — the ground-truth check would have silently become a tautology. `listed` is now sampled by the
caller before the promotion and passed in; the parameter exists so it cannot come back.

#### Pre-registered, in the order to check them

1. **`D1A6D1B27ADE6232` and `CDFE11B12552EA2D` must both appear on `Remix skyclassify:` as `ARMED:listed`.**
   They are ground truth and the OLD rule rejects both. If they do not arm, the `upv` floor is still wrong and
   nothing else this round claims is worth reading.
2. **Ravine must arm nothing.** `haze_domes.csv`: all six `jungle_ravine_stream/*` backgrounds read
   `(no skyModel authored)`. **A black sky in Ravine is CORRECT.** Any hash arming there is a false positive
   and it is the cheapest place in the game to see one.
3. `skyclassify_armed` in **8..20** over a full single-player pass. MEASURED from `haze_domes.csv`: **16**
   distinct dome resources exist game-wide but only **12** of them appear outside multiplayer
   (`mp_caves_sky`, `mp_mcv_sky`, `mp_pow_sky`, `mp_shanty_sky` are MP-only), and a dome resource may bind
   more than one texture. **Above ~28 is over-matching** → raise `SKYCLASSIFYUPV`.
4. `skyclassify_armed − skyclassify_promoted` **> 0**. That difference is the count of hashes the rule agreed
   with the user about. **If it is 0 the rule found nothing the user had already found by hand**, which is a
   reason to distrust it rather than to celebrate it.

#### Clamp audit — every ceiling against the value the launcher arms

| knob | clamp | launcher arms | headroom |
| --- | --- | --- | --- |
| `SKYCLASSIFY` | `min(env, 3)` | **2** | ceiling is 3, and mode 3 is a real behaviour (promote, ignore disqualification), not a reserved value |
| `SKYCLASSIFYMAXEXT` | `env_float`, negative sentinel, default 4.0e6 | **4000000** | no ceiling; `env_float` rejects 0, so 0 is not a usable value here |
| `SKYCLASSIFYMAXVTX` | `min(env, 65535)` | **1024** | 64× |
| `SKYCLASSIFYMINVTX` | `min(env, 4096)` | **16** | 256× |
| `SKYCLASSIFYUPV` | `min(env, 1000000)` | **60** | 16667× |
| `SKYCLASSIFYMIN` | `clamp(env, 1, 4096)` | **8** | 512× |
| `SKYCLASSIFYSETTLE` | `min(env, 1000000)` | **60** | 16667× |

No knob is armed at its ceiling. That is the `STATICINDEXBUDGET` trap, checked deliberately.

---

### TASK 2 — the pre-registered sun test could not be run, and that is the finding

#### The test is CIRCULAR by construction

The brief's test was: look up crashed_plane's zone in `haze_scene_env.csv`, apply a derived
`sunAngle`+`sunTimeOfDay` → direction mapping, and check it reproduces the play-test-confirmed
`RPCS3_REMIX_SUNMAP=D1A6D1B27ADE6232:0.5155,-0.5736,-0.6366`.

MEASURED, `launch-haze-remix.cmd:3389-3436`: **that vector's azimuth was itself typed in from the authored
`sunAngle` on 2026-08-17**, and only the Y term was ever swept by eye (16.5 "too low", 25 "a little low", 35
**armed and confirmed**, 73.5 "too high"). Decomposing the ground truth confirms it exactly:

```
(0.5155, -0.5736, -0.6366)   |v| = 1.0000084
  ->  azimuth = 128.9995 deg     elevation = 35.0013 deg     (round-trip error 0.0014 deg)
```

against crashed_plane's authored `sunAngle = 129.0`. **Testing `atan2(ground truth) ≈ sunAngle` re-measures a
number that was copied from `sunAngle`.** It cannot validate anything.

#### The elevation half runs, and fails to discriminate

crashed_plane authors `sunAngle 129.0 / sunTimeOfDay 16.5` — MEASURED, **authored not inherited**, on all six
of its sun-bearing zone records (`haze_scene_env.csv` rows 48–53). At `timeSunRise 5.0` / `timeSunSet 19.0`,
`f = 0.82143`:

| model | elevation | error vs the confirmed 35.0013° |
| --- | --- | --- |
| linear fold `90(1−|t−12|/7)` | 32.143° | 2.86° |
| great circle `asin(sin 180f)` | 32.143° | 2.86° |
| `90·cos(15(t−12))` | 34.442° | 0.56° |
| half-sine `90 sin(πf)` | 47.883° | 12.88° |

The play-test ladder establishes an accepted band of roughly **(25°, 73.5°)**, and **32.14° and 47.88° are
both inside it**. One accepted value does not choose between them. The 0.56° fit is post-hoc curve selection,
not evidence.

Honest degrees of freedom: 3 up-axis × 4 azimuth origin × 2 handedness × 2 sign = **48 discrete** combinations
plus ≥2 continuous (shape and amplitude of `f(t)`), against **one vector = two numbers**. Any (az, el) pair is
reachable. **A single ground truth structurally cannot validate this mapping**, and no table was shipped on it.

Note the constant-rate great circle has a *hard ceiling* of 32.143° at `t = 16.5` for any tilt whatsoever. The
confirmed vector sits **2.86° above that ceiling**, which falsifies that model taken literally.

#### One genuinely independent azimuth measurement exists, and it supports the direct reading

`bin\remix_dump.log:1610776` carries an **image-derived** bearing for the crashed_plane dome, computed before
the level files were cracked: `centroid_travel = [0.6193, -9.13e-9, -0.78515]` → bearing **128.265°**. An
axis-origin or handedness choice can only add a multiple of 90° or flip sign — it cannot absorb a
time-dependent sweep — so best-residual-over-all-conventions is a fair test:

| model | predicted bearing | best residual vs 128.265° |
| --- | --- | --- |
| **direct, `az_sun = sunAngle`** | 129.0 | **0.74°** |
| `eastAngle + 180f` sweep | 276.86 | 31.41° |

**MEASURED and non-circular: the direct reading wins by 31° on this level**, despite the engine's own name for
the field being `eastAngle`. A second dome (`CDFE11B12552EA2D`, bearing 280.166°) is inconclusive — it fits
`sky_containership_inside` to 0.11° and the land-carrier assignment the launcher assumes to 30.17°, and the
level cannot be resolved from the log because **no level-name string appears anywhere in the 969 MB dump**.

#### THREE HARD BLOCKERS ON A PER-ZONE TABLE, all measured this round

1. **Zone geometry was never recovered.** `haze_scene_env.csv`'s `zone_pos_0` is populated on **3 of 310** rows
   and `zone_dimensions_*` on **0 of 310**. The brief's "camera position against `zone_pos`/`zone_dimensions`"
   option is **dead** — there are no bounds to test against.
2. **The dome-hash key collides.** `haze_domes.csv`: **quarry uses `backgrounds/skydomes/crashed_plane_dome`**,
   the same dome as crashed_plane, while authoring a different `sunTimeOfDay` (16.75 against 16.5) and a very
   different `sunIntensity` (3.89 against the inherited 2.0). A `SUNMAP` keyed on the dome's texture hash aims
   quarry with crashed_plane's sun. That is a live limitation of the mechanism that already ships.
3. **`sphericalH` is not per-zone at all.** MEASURED: **3 of 310** rows in `haze_scene_env.csv` carry it, all
   three are `<shared>` template rows, **0 of 384** in `haze_level_scenes.csv`, and crashed_plane and
   copperplant carry **zero**. Every single-player level inherits one template SH. **There is no per-zone
   ambient SH to port** — the brief's third sun input does not exist.

The per-zone authored table was produced anyway and is the round's Task-2 deliverable:
`…\scratchpad\hazelight\haze_authored_env_by_zone.csv` (384 rows: level, zone, priority, blendDistance,
sunAngle, sunTimeOfDay, eastAngle, timeOfDay, sunIntensity, sunRgb, sunCol, skyAmbientRgb, fogCol, fogNear,
fogFar, fogIntensity, skyModel, both elevation predictions). Also MEASURED: `sunAngle`/`sunTimeOfDay` and
`eastAngle`/`timeOfDay` are the **same two quantities in two record families** — they never co-occur in any of
the 310/384 rows, and `07c4ac95.vms:986` authors `eastAngle 129.375 / timeOfDay 16.5` for the same level whose
zone records author `sunAngle 129.0 / sunTimeOfDay 16.5`.

#### Fog: reachable, but only one of the four authored fields

The `src/d3d9/`-only trap was checked first and it **does** apply to the obvious mechanism:

- **D3D9 fixed-function fog — UNREACHABLE.** `FogState` has exactly one producer in the whole runtime,
  `setFogState()` at `src/d3d9/d3d9_rtx_utils.cpp:245`, called only from `src/d3d9/d3d9_rtx.cpp:686`.
  `rtx_remix_api.cpp:900` default-constructs `DrawCallState`, so `fogState.mode` stays `D3DFOG_NONE` forever
  for an external draw.
- **`rtx.enableFog` / `rtx.fogColorScale` / `rtx.maxFogDistance` — PRESENT AND INERT.**
  `rtx_composite.h:90-92` declares them and a real renderer pass reads them, but
  `composite.slangh:33-36` returns `vec4(0.0)` when `fogMode == D3DFOG_NONE`. **They multiply a hard zero.**
  This is the fourth instance of the sky/`ignoreTextures`/terrain pattern in this project.
- **`rtx.volumetrics.*` — REACHABLE.** `RtxGlobalVolumetrics::getVolumeArgs`
  (`rtx_global_volumetrics.cpp:467`, called per frame from `rtx_context.cpp:1365`) reads
  `transmittanceColor()`, `transmittanceMeasurementDistanceMeters()`, `singleScatteringAlbedo()` and
  `anisotropy()` **directly** from `RtxOption` values. D3D9 fog only *overrides* them behind a gate
  (`fogState.mode != D3DFOG_NONE`, `:483-486`) an external draw cannot open — and volumetrics stay **on**, not
  off, because `shouldConvertToPhysicalFog` returns true immediately for `D3DFOG_NONE` (`:445`).
- **Runtime mutation exists**: `remixapi_SetConfigVariable` (`rtx_remix_api.cpp:1642`, bound at `:2582`) writes
  into the user layer and `RtxOptionManager::applyPendingValues` runs every frame at `rtx_context.cpp:840`.
  Latency one frame. Env vars are load-time only (`rtx_option_layer.cpp:698`); there is no conf hot-reload.

Mapping Haze's four authored fog fields:

| authored | Remix option | verdict |
| --- | --- | --- |
| `fogCol` | `rtx.volumetrics.singleScatteringAlbedo` (+ `transmittanceColor`) | reachable at runtime |
| `fogIntensity` | `rtx.volumetrics.transmittanceMeasurementDistanceMeters` | reachable at runtime |
| `fogFar` | `rtx.volumetrics.froxelMaxDistanceMeters` | approximate — it bounds froxel allocation, not fog end |
| `fogNear` | *nothing* | **no equivalent.** Physical volumetrics are uniform from the camera outward; `VolumeArgs` has no start distance |

**Shipped this round: a negative probe, not a fog implementation.** `runtime::probe_set_config_variable()`
calls `SetConfigVariable` with `rtx.rpcs3.probeKeyThatCannotExist` — `rtx_remix_api.cpp:1653` looks keys up
with `RtxOptionImpl::getOptionByFullName` and returns `GENERAL_FAILURE` for an unknown one, so the probe
exercises pointer, marshalling and return path while **provably writing nothing**. `GENERAL_FAILURE` is the
PASS; a `SUCCESS` would mean the slot is misrouted. Read it on `bin\log\RPCS3.log` at startup:
`Remix: SetConfigVariable probe returned ...`.

**Found while wiring it:** `guarded_set_config_variable` **already existed** in `RemixRuntime.h`/`.cpp` at
HEAD, declared and defined and **never called from anywhere**. My first patch added a duplicate; it was
removed. That is independent corroboration that this route has never been executed on this title.

**Three named hazards before anyone builds fog on it.** (a) `remixapi_SetConfigVariable` takes only the
remixapi mutex while the render thread walks `m_optionLayerValueQueue` under
`RtxOptionImpl::getUpdateMutex()` — call it **on zone change only, never per frame**. (b) `RtxOptionLayer::save()`
(`rtx_option_layer.cpp:376`) serialises the whole user layer, so clicking Save Settings in the Remix UI while
zone fog is live bakes those values into `bin\user.conf`, where the layering trap makes them
un-overridable from `rtx.conf`. (c) `bin\rtx.conf` has **zero** fog/volumetric keys and `bin\user.conf` has
exactly two — `rtx.volumetrics.froxelDepthSlices = 48` (line 28) and
`rtx.volumetrics.froxelGridResolutionScale = 8` (line 38), both written by `graphicsPreset = 4` and both
grid-shape rather than appearance. **Nothing will fight a runtime set.**

---

### Post-review corrections — one of them would have made this round fail silently

An independent review of the diff cleared the structural questions (the `upload()` refactor is
behaviour-preserving, `promote_sky_emissive`'s iterator lifetime is safe, exit accounting is exact, no thread
race, the `SetConfigVariable` probe cannot gate `m_fork_features`) and found **six** issues. All are fixed and
the binary named above is the corrected one.

**RISK-1 — the rebuilt material aliased the one it replaced, and every counter would have said it worked.**
`material_hash` is derived from `content_hash` + `wrap_u`/`wrap_v`/`alpha_func`/`alpha_ref`, and **a promotion
changes none of them** — they are properties of the texture, not of the classification. So
`promote_sky_emissive` called `CreateMaterial` a second time with the **same hash and a different, emissive
definition**. The comment twelve lines above the derivation already records what that costs, as measured fact:
aliasing CreateMaterial definitions made the winning state **draw-order dependent**. If the first definition
won, the dome would stay black while `skyclassify_promoted`, `skyclassify_entries` and `mat_skyemissive` all
reported success — this project's most expensive recurring failure shape, and **none of this round's new
counters could have seen it.**

Fixed by folding the promoted bit into `material_hash`, in the same shape the wrap/alpha variant already uses.
**Applied on top, and only for a promoted hash, deliberately**: a fifth byte added unconditionally to
`material_state` would move the identity of every non-default-wrap material in the title, and `material_hash`
is the **modding surface** — the `mat_<HASH>` a `mod.usda` replacement targets.

**VERIFIED, not assumed**, by replicating `fnv_bytes`/`hash64` exactly and running the real hashes:

| content hash | material before | material after |
| --- | --- | --- |
| `D1A6D1B27ADE6232` | `D1A6D1B27ADE6232` | `33B1BC70BA330E12` |
| `CDFE11B12552EA2D` | `CDFE11B12552EA2D` | `6CDD81A0F713D131` |
| `35C2353F6B3CE2A8` | `35C2353F6B3CE2A8` | `AFA7C3612AC248C0` |
| `174F4F689CF2A3D8` | `174F4F689CF2A3D8` | `776CB30908D8EAF0` |
| `3213E0CC136ED294` | `3213E0CC136ED294` | `072328D6474BC984` |
| `32AE81D64BEA29CD` | `32AE81D64BEA29CD` | `BBC39485414A01D1` |

All six listed hashes plus all twelve classifier candidates differ; **0 collisions in 18**. The
non-default-wrap variant of `D1A6D1B27ADE6232` is `D480D8EA0874FFCD` unpromoted — **the round-38 value,
unchanged** — and `AC64C1CB4F1537D1` promoted. A runtime self-check backs it up: `Remix skypromote:` prints the
before/after pair per rebuilt entry and says **`IDENTICAL - THE FOLD DID NOT FIRE`** if they ever match.

**Consequence, stated because it is real:** a promoted dome's material hash is no longer equal to its albedo
content hash. `Remix skypromote:` is where a modder reads the new one.

**RISK-3 — arming latched on intent, not on success.** `classify_entry.promoted` and the promoted-array insert
both latched before the rebuild was known to have worked, so two failures were permanent and unretried: the
array being full (nothing ever emissive) and every resident entry refusing `CreateMaterial` (the hash in the
set, so every later mesh re-keys under an identity whose material never changed — a black dome reported as a
success). Now the work runs first and `promoted` latches only on success; a total rebuild failure calls
`sky_emissive_unpromote()`. **Undoing is safe exactly here**: the mesh-key fold reads `sky_emissive_promoted()`
*earlier* in `submit_subdraw` than this block runs, so a hash promoted on this draw cannot have moved a mesh key
until the next frame. New counters `skyclassify_overflow` and `skyclassify_failed` size both cases.

**RISK-4 — the census budget starved the deliverable.** One shared 64-line ceiling, with `reject:mixed` firing
on the *first* disqualifying draw of any of up to 4096 tracked hashes while `ARMED` needs 8 dome draws plus a
60-frame settle. The rejects would normally exhaust the budget before a single `ARMED` line existed — and the
`ARMED` line **is** the deliverable, because it is the only place the camera position is printed and therefore
the only way to attribute a dome hash to a level. Split into two budgets: **48 for `ARMED`, 32 for rejects**.

**RISK-6 — the ground-truth check could not fail.** The round-13 census in `report_sky_emissive_census` prints
`listed=` from `sky_emissive_albedo_matches()`, which after this round also returns 1 for classifier-promoted
hashes — while its own doc block says `listed` is a statement about the env var. New `sky_emissive_listed()`
reads the env list alone and the census uses it; the *gate* keeps the wider predicate so a promoted dome still
gets a line. **The identical hazard was guarded in the new census and missed in the old one.**

**RISK-2, RISK-5, NIT-2, NIT-4 (comments, all corrected).** `RemixTextures.h` claimed "no mesh key moves
because of this call" — wrong: the material handle is folded into both `static_key` and `union_hash`, and the
ordinary mesh key is moved deliberately. The "mode 1 is image-identical" claim is now qualified — it mutates no
material, mesh key or category, but it widens the condition under which the vertex bounding box is walked, so
with `SKYHASH=0 SKYBACKDROP=0` it costs a walk round 38 did not pay (no pixel changes; the cost does). The
argument-count comments said seven/eleven/twelve and now say eight/thirteen/fourteen. And **`mat_skyemissive`
is no longer a "did the dome attach?" test**: `promote_sky_emissive` rebuilds every entry aliasing the content
hash, ~45 on this title, so one dome moves it by tens — use `skyclassify_entries` and `Remix skypromote:`.

### Knobs table additions

| knob | default | what it does |
| --- | --- | --- |
| `RPCS3_REMIX_SKYCLASSIFY` | `1` (census only) | Classify sky domes from geometry per albedo hash and, at `2`, **promote** them into the sky-emissive set — equivalent to having typed the hash into `RPCS3_REMIX_SKYEMISSIVE`. `3` promotes while ignoring the disqualification. `1` is image-identical to round 38 and still produces every counter and every `Remix skyclassify:` line. `0` disables even the census. |
| `RPCS3_REMIX_SKYCLASSIFYMAXEXT` | `4.0e6` | Upper world-extent bound. Rejects the 1.22e9..1.12e18 broken-transform family while keeping the 7e5..2.1e6 dome family that three hand-listed hashes live in. **Do not lower below ~3e6.** |
| `RPCS3_REMIX_SKYCLASSIFYMAXVTX` | `1024` | Vertex ceiling. Domes measure 33..747, terrain 2714..12875. |
| `RPCS3_REMIX_SKYCLASSIFYMINVTX` | `16` | Vertex floor. Drops one named false positive: a 4-vertex quad spanning 32,331 units with the camera inside it. |
| `RPCS3_REMIX_SKYCLASSIFYUPV` | `60` | Extent-per-vertex floor. 3.1× gap (23.57 / 73.09), but **nearly redundant with the vertex ceiling on this data** — raise it toward 73 if a non-sky surface glows. |
| `RPCS3_REMIX_SKYCLASSIFYMIN` | `8` | Dome-shaped draws needed to arm. |
| `RPCS3_REMIX_SKYCLASSIFYSETTLE` | `60` frames | Delay between first sight and arming. Guards the one irreversible failure — a material rebuilt emissive cannot be rebuilt back. |
| `Remix stats:` / `Remix live:` gain `skyclassify_*` | — | `seen`, `dome`, `tracked`, `armed`, `promoted`, `rejected`, `settling`, `entries`, `orphans`, `overflow`, `failed`, `set`, `mode` (+`census` on the stats line). On **both** lines deliberately — `Remix stats:` goes only to `RPCS3.log`, which is locked while the emulator runs. Audited position by position: `Remix run-start:` **130/130**, `Remix stats:` **277/277**, `Remix live:` **342/342** specifiers vs arguments, and every new specifier resolves to its intended argument. |
| `knobs:` gains `skyclassify=` … `skyclassifyset=` | — | Seven fields on the boot banner and the flip-time repeat, printing the **clamped** values. |
| `Remix skyclassify:` census | — | One line per hash per transition: verdict (`ARMED` / `ARMED:listed` / `reject:mixed`), `dome`/`other`/`agree`, `vp`, `vtx`, `wext`, `upv`, `inside`, `depth_write`, every threshold it was judged against, `listed=`, and **the camera position at the moment it armed** — which is the only way to attribute a dome hash to a level, since no level name exists anywhere in the guest signal. Ceiling 64 lines; a full budget is itself the finding. |
| `runtime::probe_set_config_variable()` | always | Negative probe of `remixapi_Interface::SetConfigVariable` at startup. Writes nothing. `GENERAL_FAILURE` is the pass. |

### Play-test card — round 39

Launch as usual (`launch-haze-remix.cmd` → `bin\rpcs3-next.exe`, **`E4E2A9A4EDAECE2A`** — hash it). Round 38's
knobs are all still armed and unchanged; its card still applies alongside this one.

| what to look at | where to read it | one-line revert |
| --- | --- | --- |
| **Do the black skies come back on the levels that had them?** | `Remix live:` — `skyclassify_promoted` > 0 **and** `skyclassify_entries` > 0. Entries 0 with promoted > 0 = the rebuild found no resident texture and the sky stays black | `set "RPCS3_REMIX_SKYCLASSIFY=1"` |
| **GROUND TRUTH, check this first** | `Remix skyclassify:` must carry `D1A6D1B27ADE6232` and `CDFE11B12552EA2D` as **`ARMED:listed`**. The old rule rejects both. If they are absent, `SKYCLASSIFYUPV` is still too high | `set "RPCS3_REMIX_SKYCLASSIFYUPV=50"` |
| **RAVINE MUST STAY BLACK** | `haze_domes.csv` authors no dome there. Any new `Remix skyclassify:` line while in Ravine is a false positive | `set "RPCS3_REMIX_SKYCLASSIFY=1"` |
| **Over-matching** | `skyclassify_armed` above ~28 (expected 8..20 — 12 of the title's 16 dome resources are reachable outside multiplayer) | `set "RPCS3_REMIX_SKYCLASSIFYUPV=66"` |
| **Something that is not sky is glowing / has stopped casting shadow** | `Remix skyclassify:` names the albedo; check it against what glows | `set "RPCS3_REMIX_SKYCLASSIFY=1"` |
| **A level is left too briefly for its dome to arm** | `skyclassify_settling` stuck high | `set "RPCS3_REMIX_SKYCLASSIFYSETTLE=15"` |
| **Did the material rebuild actually take?** | `bin\log\RPCS3.log`: `Remix skypromote: content=... mat AAAA -> BBBB ok`. If it ever reads **`IDENTICAL - THE FOLD DID NOT FIRE`**, stop — the promotion is aliasing the material it replaces and whether the dome lights is draw-order dependent | `set "RPCS3_REMIX_SKYCLASSIFY=1"` |
| **A promotion was refused** | `Remix live:` — `skyclassify_failed` > 0 (every resident entry refused the rebuild; the promotion was withdrawn and retries) or `skyclassify_overflow` > 0 (the 64-entry set is full) | — |
| **Is fog reachable at all?** | `bin\log\RPCS3.log` at startup: `Remix: SetConfigVariable probe returned ...`. `GENERAL_FAILURE` = PASS. `SUCCESS` = the slot is misrouted, do not build on it | — |
| **Please also record, per level, the `albedo=` from `Remix skyclassify:`** | this is what maps a dome hash to a level, and it is what unblocks per-level sun and fog | — |

## Round 38 (2026-08-24)

Deployed: `bin\rpcs3.exe` = `bin\rpcs3-next.exe` = **`447D3904D1E969B2`**. Runtime `bin\remix\d3d9.dll`
**UNCHANGED at `16A0B512F33EBB66`** — nothing deployed into `bin\remix\`.

MSBuild `Release|x64` exit **0**. Tree state: HEAD `44276ae06` at start and at end; at the start
`bin\rpcs3.exe` and `bin\rpcs3-next.exe` were byte-identical (MD5 `0A5EECD31AB0641EAAA086FCAA21E34B`) and
`bin\rpcs3-next.exe` hashed `56A0A57DA51FF02E`, i.e. **exactly round 37's binary. The tree did not move
under another session.**

---

### ONE DEFECT BEHIND FOUR REPORTS: the divisor is a frame old while the camera is current

The jiggling dumpsters and lockers, the teleporting light fixture, the weapon that lags when you turn, and
"geo following the camera" are **the same bug**, and its size is set by one product.

A draw whose render source has not yet produced its identity-donor draw *this* frame is divided by
**`fused_donor(t-1)`** and then rendered by Remix with **`camera(t)`**. Write `V` for the donor's fused
matrix. The recovered world is `W_true · V(t) · V(t-1)⁻¹`, so the submitted geometry carries the extra
factor `D = V(t)·V(t-1)⁻¹`, which for a point `p` is

```
p  ->  eye + (p - eye) · dR
```

a rotation **about the eye** by one frame of camera turn. Therefore

> **displacement = (distance from the eye) × (camera turn per frame)**

which is why the identical defect reads as a millimetre wobble on the weapon at 0.73 m, a hand-width jiggle
on a dumpster at a few metres, and metres on a light fixture across the room. Nobody was looking for one
cause because the three symptoms have three magnitudes.

The branch is `RemixGSRender.cpp`, the `else if (… find_gauge_anchor(false, …))` arm of the gauge dispatch,
and the comment directly above it states the behaviour and defends it: *"Last frame's anchor is used for
draws that arrive before the frame's first identity draw. That is never worse than the old path: the
elected camera's reference was always a frame old."* **That premise has since been falsified by
`CAMRELATCH`**: MEASURED on the last run's `Remix live:` line, `world_ref_fresh=3598650` against
`world_ref_stale=97515` — **97.4 % of draws now carry a current-frame elected camera.** The branch is
comparing itself to a baseline that no longer exists.

Population size, same line: `gauge_used=2825794 gauge_prev=641662 gauge_absent=228709`, so **18.5 % of all
world draws** take the stale divisor; an earlier window in the same log reads 445228 / 227300 = **33.8 %**.

#### Evidence 1 — the viewmodel, 4000 consecutive frames

Round 37's `VMBASISEVERY=1 VMBASISVTX=3649 VMBASISMAX=4000` fired and produced **4000 lines over frames
12311..16395, one per frame, step 1 on every one of the 3999 gaps**, all `vtx=3649`, all
`albedo=86885A0E60751491`, all `camvp=7f3d3abcefc8b057`, all `camage=0`. First per-frame time series this
project has had of the viewmodel transform.

Take the recovered viewmodel 3×3, express it in the camera basis of frame `t+s`, and measure the
frame-to-frame change of that camera-space orientation. If the gun is rigidly attached, the shift that
minimises the change is the frame it is actually attached to.

| s | median (deg) | mean | p95 |
| --- | --- | --- | --- |
| −3 | 0.2844 | 0.4819 | 1.7876 |
| −2 | 0.3031 | 0.4676 | 1.4879 |
| **−1** | **0.2287** | **0.3750** | **1.2348** |
| 0 | 0.2984 | 0.4655 | 1.5531 |
| +1 | 0.2855 | 0.4773 | 1.8047 |
| +2 | 0.2885 | 0.5045 | 1.9772 |
| +3 | 0.2926 | 0.5297 | 2.0924 |

**`s = −1` is a strict minimum in all three statistics**, and `s = −2` and `s = +1` are both worse than it,
so this is *one frame* and not "any decorrelation helps". Restricted to the 328 frames where the camera
turned more than 1 degree the same shape holds and steepens: −1 = 0.3484/0.4791, 0 = 0.3879/0.6472,
+1 = 0.4882/0.9985, +2 = 0.5654/1.2506. The lagged basis is smaller than the current one on **69.6 %** of
frames (binomial SE 0.79 %, so ~25σ from 50 %).

Magnitude check against the model: the gun sits at `cdist_post` median **0.734 m** from the eye and the
camera turns up to **8.856 deg** in one frame; `0.734 × 8.856 deg = 0.113 m` against a **measured** max
frame-to-frame `|Δcpost|` of **0.1248 m**. The model predicts the observed excursion to within 10 %.

#### Evidence 2 — the family the user actually named

All **48** `Remix picked:` / `Remix pick-deep:` lines in the last 300 MB carrying
`vp=830d7d1b9681c475 fp=c61b0b9586dd67fb` — the pair the brief identified as the dumpsters, the lockers and
the teleporting light fixture — read

```
ref=anchor_prev   anchor_frame = frame - 1   anchor_vp=ad7ce9d672a0bf6b   cam_age=0
```

48 of 48, on every frame sampled (11566/11565, 11645/11644, 11715/11714, 11771/11770). Divisor a frame old,
camera current, no mixture. Across all picked families in that window: `anchor_prev` 150, `identity` 84,
`anchor` 62, `camera` 28.

**This is not round 32's tautology.** Round 32 was right that "`ref=anchor_prev` therefore it lags a frame"
restates the name. The claim here is different and is measured separately: the *displacement that follows*
has a predicted size, and Evidence 1 measures it on a different program, from different fields, without
reading `ref=` at all.

#### Shipped: `RPCS3_REMIX_GAUGECURDIMS=1`, and the counter that says whether it can fire

When the current-frame exact-key lookup misses, try a **this-frame** anchor on `(target, clip)` alone before
falling back to last frame's. Identical relaxation to `GAUGEPREVDIMS`, which has shipped ON for several
rounds for frame `t−1`; this applies it one frame fresher, and `find_gauge_anchor_shape(…, 0, 0, …)` already
existed for the tail rescue.

**The honest risk is that this route cannot fire at all**, because a draw that arrives before its own
source's donor may arrive before *every* donor. So `gauge_cur_avail` is incremented **whether or not the
knob is armed** — a run with `GAUGECURDIMS=0` still answers the question. Read it first:

| counter | where | meaning |
| --- | --- | --- |
| `gauge_cur_avail` | `Remix live:` | a this-frame same-shape anchor **existed**. **0 = the route is structurally dead on this title** and no value of the knob changes anything — go to `DEFERPREANCHOR=1` instead. |
| `gauge_cur_dims` | `Remix live:` | draws the route actually rescued. `<= gauge_cur_avail` always. |
| `gauge_anchor_prev` | `Remix live:` | must **fall** by `gauge_cur_dims`. |
| `gauge_prev_camfresh` | `Remix live:` | prev-branch draws whose **elected** camera was current: the size of the untried third route (divide by the fresh camera rather than a stale anchor). Measurement only. |

Visible confirmation without reading a log: Ctrl+click a jiggling dumpster. `Remix picked:` read
`ref=anchor_prev anchor_frame=<frame−1>` in round 37; it must now read `ref=anchor anchor_frame=<this
frame>`.

**Blast radius**: this hands a *different* render source's this-frame anchor to a draw whose own source has
not anchored yet. If static scenery lands in the wrong place, this knob is the first suspect.
`GAUGECURDIMS=0` reproduces round 37 byte for byte, no rebuild.

**The already-proven alternative, for the record.** `RemixGSRender.cpp` states beside this branch that
`DEFERPREANCHOR` *"was root-caused, shipped, and CONFIRMED by the user to stop the props sliding — then
switched back off on 2026-08-16 because it cost ~14 fps"*. So the user has already seen this defect fixed
once. Round 32 then narrowed its population by 35.8 % (`DEFERPREVONLY`, default 1, inert while
`DEFERPREANCHOR=0`). If `gauge_cur_avail` comes back 0, that knob — not a fresher lookup — is round 39's
lever, and its cost is now known in advance.

### Three refutations, each pre-registered before it was read

1. **NOT a stale camera *basis* applied to a correct gun.** Predicted displacement under that model is
   `(Q − I)·cpost` with `Q = M(t)M(t−1)ᵀ`. Direction agreement with the measured displacement has median
   **cos = −0.348**, and only **4.1 %** of frames exceed cos 0.9. Refuted.
2. **NOT a held / reused transform.** The `pre=` matrix is bit-identical between consecutive frames on
   **0 of 3999** pairs; the update-gap histogram is `{1: 3998}`. *(An earlier pass of this same analysis
   reported "median frame-to-frame rotation exactly 0.0000°", which was **the instrument, not the signal**:
   `acos((tr−1)/2)` on 6-significant-figure output has a quantisation floor near 0.14°. Every rotation
   magnitude in this section uses `‖B−A‖_F = 2√2·sin(θ/2)` instead.)*
3. **The anisotropic basis error is NOT this defect.** `D` is projective, so a stale divisor *should* leave
   anisotropy that grows with camera motion. MEASURED: recovered column lengths span 0.9948..1.0054,
   anisotropy `max/min − 1` median **1.176e-3**, and it is **the same with the camera stationary**
   (turn < 0.02° and eye unmoved, n=19: median 1.361e-3) as while turning > 1° (n=328: median 1.470e-3);
   `corr(turn, anisotropy) = 0.142`. Round 34's ±0.28 % anisotropic basis is a **separate, smaller defect**
   and must not be folded into this one. Note `world_div_f64 = 3467456 = gauge_used + gauge_prev` exactly,
   so `GAUGEF64` is fully engaged and this residual is **not** f32 conditioning either.

### The 1024x576 / 512x288 split is measured, not merely uncorrelated

The brief named the clip-resolution split "the live question". `Remix anchor-gauge:` answers it directly:
over **41,718** lines, **41,036 (98.4 %)** have `anchor_clip=1024x576` against `elected_clip=512x288`, and
on those the anchor camera and the elected camera agree to **median 1.07e-4 units, p95 4.88e-4**. Round 36
argued from the ~100 %-vs-0 % rate mismatch that the split is not causal; this upgrades that to a direct
measurement of the quantity itself. **Named limit:** 0.75 % of lines (311) read `worst > 1.0`, max
**1787.66** — a real tail, not the mechanism.

### TASK 3. A fourth pair draws the helmet, and it is the second-busiest of the four

Round 37 asked: "either a fourth pair draws it, or the UI route does not prevent clipping. Measure which."
MEASURED, last 200 MB, every line carrying the helmet albedo `C61753D31FB96507`:

| vp | fp | `Remix fpcandidate` | `Remix uiwrap … route=2d` | armed in round 37? |
| --- | --- | --- | --- | --- |
| `830d7d1b9681c475` | `479890ff55f1d96e` | 424 | **331** | yes (`UIFORCEPAIRVP`) |
| **`9f591b6a6b825612`** | **`cbede4eb45f0fd25`** | **416** | **0** | **NO** |
| `f39f504649b6f442` | `4afa02b3dbbe9b7e` | 246 | **65** | yes (`VP2` slot 0) |
| `830d7d1b9681c475` | `609a4216b89e296a` | 6 | 0 | yes (`VP2` slot 1) |

All four draw `vtx=59` — the same mesh. The unarmed pair is the second-busiest and the only busy one with
zero `uiwrap` lines, so its copy goes out as world geometry and clips. **It is a fourth pair.** Added to
`UIFORCEPAIRVP2`/`FP2` as slot 2 (the backing array is `std::array<u64, 8>`, so 3 of 8 are used).

**Two corrections round 39 must carry.**

- **`uiforcepairs=` and `ui_forced_pair=` are on `Remix stats:`, which goes ONLY to `bin\log\RPCS3.log`.**
  They are **not** on `Remix live:` and therefore not in `remix_dump.log` — 0 of **13,495** `Remix live:`
  lines in the last 200 MB carry `ui_forced_pair`. Round 37 read them correctly (in RPCS3.log) but wrote the
  pre-registration as "the knobs line", which is the `Remix live:` line. RPCS3.log is exclusively locked
  while the emulator runs, so this reading is only available **after it exits**. Round 38's three new
  counters are deliberately on **both** lines for this reason.
- **Round 37's `~2/frame` prediction read otherwise, and `uiforcepairs=2` proves its stated alternative
  wrong.** MEASURED from RPCS3.log over eight consecutive `Remix stats:` intervals spanning 623 frames,
  `ui_forced_pair` rose at **exactly 1.000/frame** with zero remainder (93/93, 83/83, 65/65, 70/70, 86/86,
  82/82, 72/72, 72/72) while `uiforcepairs=2`. Round 37's note says "if it stays at 1/frame the extra list
  did not parse, and `uiforcepairs=` says so" — the list *did* parse, and the extra pair simply does not
  draw on every frame (331 vs 65 `uiwrap` lines is roughly 5:1). **A rate prediction needs the per-frame
  incidence of each pair, not just its existence.**
- `ui_skipped` is **live again**: 0 → 7163 within one run in this log, so round 37's "frozen since frame
  9678" no longer holds and it can be read as evidence if the helmet vanishes. (It then froze at 7246 for
  the last eight stats intervals of that run.)

### TASK 4. `peak=64 budget=64` was the CLAMP reporting itself — the clamp trap, a third time

`RemixTransforms.cpp` clamped `STATICINDEXBUDGET` to `[1, 64]` and the launcher armed **64**. Round 35's
clamp audit recorded that armed-equals-ceiling as *intended*, and as a value it was — but the consequence
was missed: **a saturated window and a correctly-tuned window print the same thing**, and the knob had no
headroom to A/B against. Ceiling raised to **512**; the launcher takes one conservative step to **128**
(the ladder has been 4 → 8 → 32 → 64).

MEASURED, last run in `bin\remix_dump.log`:

```
Remix static-index: entries=11498 triangles=2252883 rebuilds=12228 deferred=841
                    stale=10 dropped=831 peak=64 budget=64 resident=115 evicted=11376 nomesh=7
```

Round 37 reported the budget "relieved" from `peak=56 budget=64 dropped=0`. That line is real and it is in
this log — on a **388-entry** window. Every **11,498-entry** window in the same log reads `peak=64
budget=64` with `dropped` in the hundreds to thousands. Both readings are true; they are different scenes,
and the small one is not the one the plant walls are in.

**The new limiter is the eviction population, not the budget count.** `resident=115` against
`entries=11498` means **98.9 %** of static-index entries hold a `mesh_hash` whose mesh the reaper has
already freed, and this backend's own accounting says what that costs: *"evicted = mesh_hash != 0 and NOT in
m_meshes (the reaper took it -> next draw of this entry takes the `dropped` exit and renders nothing)"*.
Verified against the code: both `dropped` exits `return` before the refusal accounting, which is why the
missing walls have never appeared in `Remix world-refused:`, and **both are gated by
`m_static_index_rebuilds >= budget`**. So the budget is the throttle that converts an eviction into an
invisible frame, and `MESHIDLE=600` (~20 s at 30 fps) is what creates the evictions. If 128 does not help,
the lever is the reaper — a static-index-scoped idle window, which is a code change, not this knob.

### Knobs table additions

| knob | default | what it does |
| --- | --- | --- |
| `RPCS3_REMIX_GAUGECURDIMS` | `1` (on) | When the **current** frame's exact-key anchor lookup misses, retry on `(target, clip width, clip height)` alone against **this frame's** anchors, before falling back to the previous frame's. Same relaxation as `GAUGEPREVDIMS`, one frame fresher. Counters `gauge_cur_dims` / `gauge_cur_avail` / `gauge_prev_camfresh` on **both** `Remix stats:` and `Remix live:`. `0` restores round 37 exactly. |
| `RPCS3_REMIX_STATICINDEXBUDGET` (**source clamp raised**) | `4`; ceiling was **64**, now `512` | Armed value was exactly at the ceiling, so `peak=64 budget=64` could not be distinguished from a tuned result. Launcher moves 64 → 128. |
| `Remix stats:` / `Remix live:` gain `gauge_cur_dims=` `gauge_cur_avail=` `gauge_prev_camfresh=` | -- | Inserted adjacent to `gauge_prev_dims=`, three specifiers and three arguments in matching positions. Audited position-by-position: `Remix run-start:` 122/122, `Remix stats:` 263/263, `Remix live:` 321/321 specifiers vs arguments, and each new specifier resolves to its intended argument. |
| `knobs:` gains `gaugecurdims=` | -- | Echoed on both the boot banner and the flip-time repeat, printing the clamped value. |

### Play-test card — round 38

Launch as usual (`launch-haze-remix.cmd` -> `bin\rpcs3-next.exe`, **`447D3904D1E969B2`** — hash it).
`VMBASISEVERY=1 VMBASISVTX=3649 VMBASISMAX=4000` is **still armed on purpose**: the same 4000-frame series
must be re-taken so the `s`-shift table can be recomputed. `VMROTLOCK` stays at **0** — the residual pose
question and the one-frame lag are different defects and arming both would confound the run.

| what to look at | where to read it | one-line revert |
| --- | --- | --- |
| **Do the dumpsters/lockers stop jiggling? Does the light fixture stop teleporting?** | Ctrl+click one: `Remix picked:` must read `ref=anchor anchor_frame=<this frame>`, not `ref=anchor_prev anchor_frame=<frame−1>` | `set "RPCS3_REMIX_GAUGECURDIMS=0"` |
| **Did the fix fire at all?** | `Remix live:` — `gauge_cur_dims` > 0 and `gauge_anchor_prev` down by that amount | — |
| **`gauge_cur_avail=0`** | the route is structurally dead on this title; stop tuning it | round 39: `set "RPCS3_REMIX_DEFERPREANCHOR=1"` (costs ~14 fps, user-confirmed to work) |
| **Does the weapon still lag when turning?** | **TURN LEFT AND RIGHT.** Re-take the 4000 `Remix vmbasis:` lines; `s=0` must become the minimum of the shift table | `set "RPCS3_REMIX_VMBASISEVERY=0"` |
| **Static scenery in the wrong place / world shears when you turn** | this knob and only this knob | `set "RPCS3_REMIX_GAUGECURDIMS=0"` |
| **Helmet: visible, no shadow, no clipping?** | `Remix uiwrap:` gaining `route=2d` lines for `vp=9f591b6a6b825612`. `uiforcepairs=` must read **3** — in `bin\log\RPCS3.log` **after the emulator exits**, not in `remix_dump.log` | drop the third entry from `UIFORCEPAIRVP2`/`FP2` |
| helmet VANISHED | `ui_skipped` (live again — 0 → 7163 in the last run) | same revert |
| **Plant walls still disappearing?** | `Remix static-index:` — `peak=` must stop pinning at `budget=` and `dropped=` must fall; watch `mesh_live=` on `Remix live:` beside the frame rate | `set "RPCS3_REMIX_STATICINDEXBUDGET=64"` |
| frame rate dropped after the budget step | `mesh_live=` climbing across the session | same revert |

---

## Round 37 (2026-08-24)

Deployed: `bin\rpcs3.exe` = `bin\rpcs3-next.exe` = **`56A0A57DA51FF02E`**. Runtime `bin\remix\d3d9.dll`
**UNCHANGED at `16A0B512F33EBB66`** — nothing was deployed into `bin\remix\`.

MSBuild `Release|x64` exit 0, **0 errors**. The full-solution pass reported 7 warnings; a second pass that
force-recompiled *only* the two files this round touched reported **1**, and it is the pre-existing C4723
"potential divide by 0", now at `RemixGSRender.cpp:11065` — it was at `:10363` in round 36 and moved only
because this round inserted lines above it. **Zero new warnings from this round's code.**

Tree state at start and at end: HEAD `44276ae06` both times, `git status` clean apart from the two untracked
files that were already there (`metrics.txt`, `rpcs3.dxvk-cache`). `bin\rpcs3.exe` and `bin\rpcs3-next.exe`
were **identical** at the start (`0DCD0694…` by MD5, both 11:04/11:07) — unlike round 36, **the tree did not
move under this round.**

---

### TASK 1. The residual is an ALGEBRAIC IDENTITY of the operator, and both hypotheses in the brief are refuted

The round-37 brief pre-registered a stale-pivot explanation: "`pivotsrc=0` … pivot 0 is the **anchor-frame
eye** … stale by at least a frame". Both halves are wrong, and both are refuted from bytes that were already
on disk.

**REFUTATION 1 — pivot 0 is not the anchor-frame eye, at source.** `RemixGSRender.cpp`, in
`apply_viewmodel_rotation`, sets the default pivot from `m_active_camera.position` and the anchor-frame eye
is **pivot 3** (`case 3: anchor_frame_eye(pivot); pivot_source = 3;`). Round 36 changed this deliberately;
the brief read round 36's description of rounds *19..35* as a description of round 36.

**REFUTATION 2 — the pivot is bit-for-bit the eye that is printed on the same line.** On all **9** armed
census lines `rotpivot=` equals `cam=` exactly, e.g. line 1 `cam=[1775.82 -31.2545 1179.18]`
`rotpivot=[1775.82 -31.2545 1179.18]`. They come from the same `m_active_camera` snapshot in the same call.
There is no frame between them to be stale in.

**REFUTATION 3 — the camera was not stale on any census frame.** The census fired on frames 9656, 9660,
10013, 10014, 10134, 21672. Cross-referenced against `Remix gauge:` for those exact frames: `camage=0` on
the primary camera row of **every one**, including frame 10134, the 79-degree outlier the brief called
decisive.

**THE ACTUAL ANSWER, and it is one line of algebra.** For a rotation about the camera's RIGHT axis, the
post-operator diagonal is `(d0, -d1, -d2)` — a rotation about `right` cannot change any component along
`right`. Therefore

```
cos(relpre) + cos(relpost) = dotpre[0] - 1
```

MEASURED, re-deriving both sides from each line's own fields: the residual of that identity is
**6.1e-07, -3.4e-06 (x3), 8.3e-08, -1.8e-06, 2.7e-07, 7.5e-04, 4.2e-06** on the nine lines. The 7.5e-04 is
line 8, the only line whose `relpre` was **clamped** — `relative_angle` clamps `(trace-1)/2` to `[-1, 1]` and
that line's un-normalised trace is `-1.0015`, so `relpre=180.000` there is the clamp firing, not a
measurement. `relpre=180.000` on line 9 is the same artefact (trace `-1.000037`).

**So `relpost` is, to within 1.5 degrees on all nine lines, `acos(dotpre[0])` — the angle between the
object's own X axis and the camera's right axis. That is the entire residual, and no angle on the `right`
axis can reduce it.**

**The brief's table was three lines out of nine, and they were the three exceptions.** Full set:

| # | vp | vtx | relpre | 180-relpre | relpost | dotpre[0] | acos(d0) | verdict |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 1 | `830d7d1b9681c475` | 2140 | 178.318 | 1.68 | **1.704** | 0.99999 | 0.28 | **holds, 0.02 deg** |
| 2 | `af06f6d32ec048ee` | 4 | 118.488 | 61.51 | **62.178** | 0.98976 | 8.21 | **holds, 0.67 deg** |
| 3 | `af06f6d32ec048ee` | 4 | 118.488 | 61.51 | **62.178** | 0.98976 | 8.21 | **holds** |
| 4 | `57a12323f22f4988` | 8 | 118.488 | 61.51 | **62.178** | 0.98976 | 8.21 | **holds** |
| 5 | `830d7d1b9681c475` | 3649 | 156.446 | 23.55 | **24.651** | 0.99218 | 7.17 | **holds, 1.10 deg** |
| 6 | `830d7d1b9681c475` | 2421 | 169.509 | 10.49 | 28.357 | 0.89673 | 26.27 | misses by 17.9 |
| 7 | `9f591b6a6b825612` | 4 | 175.157 | 4.84 | 23.667 | 0.91947 | 23.15 | misses by 18.8 |
| 8 | `830d7d1b9681c475` | 1878 | 180.000\* | 0 | 79.325 | 0.18449 | 79.37 | misses by 79.3 |
| 9 | `9f591b6a6b825612` | 4 | 180.000\* | 0 | **1.786** | 0.99951 | 1.79 | **holds** |

\* clamped. **Six of nine satisfy the pre-registration within 1.1 degrees. Every line that misses reads
`dotpre[0] < 0.92`; every line that holds reads `dotpre[0] > 0.989`.** One rule, no exceptions.

**And on the two cleanest lines the correction is exactly right.** Re-projecting the POST basis onto the
camera axes: line 1 gives `objX=[1.000 0.001 0.005] objY=[-0.001 1.000 -0.029] objZ=[-0.005 0.029 1.000]`
and line 9 gives `[1.000 0.007 0.031] / [-0.007 1.000 -0.002] / [-0.030 0.002 1.000]`. That is a first-person
mesh whose local axes coincide with the camera's to under two degrees. The round-36 operator is not
approximately right on those draws — it is right.

**The residual splits into two families, and the axis is what separates them** (residual rotation axis
resolved on the camera's own axes, from the antisymmetric part of the post-operator relative rotation):

| # | vtx | residual | axis on [right up fwd] | reads as |
| --- | --- | --- | --- | --- |
| 1 | 2140 | 1.70 | `[-0.987 -0.161 0.021]` | nothing |
| 2-4 | 4, 8 | 62.19 | `[0.990 0.140 0.017]` | **pitch** about camera right |
| 5 | 3649 | 24.65 | `[0.957 0.288 -0.045]` | **pitch** |
| 6 | 2421 | 28.36 | `[0.375 -0.416 -0.828]` | roll + yaw |
| 7 | 4 | 23.67 | `[0.199 -0.543 -0.816]` | roll + yaw |
| 8 | 1878 | 79.32 | `[0.013 -0.995 -0.096]` | **pure yaw** |
| 9 | 4 | 1.79 | `[-0.062 -0.973 0.223]` | nothing |

A pitch about `right` is closable with a smaller `VMROTDEG`; a yaw about `up` is not closable on that axis at
any angle. Those are different bugs, and one number could not have told them apart — which is why the axis
is now printed (`relaxis=`).

**Whether the residual is a defect at all is still open, and this round ships the test rather than a guess.**
See `RPCS3_REMIX_VMROTLOCK` below.

### THE INSTRUMENT COULD NOT SEE TIME, AND THAT IS WHY THREE ROUNDS COULD NOT ANSWER "DOES IT JITTER"

The user's complaint is that the weapon "doesn't follow the camera turning smoothly". That is a statement
about time. The `Remix vmbasis:` census deduplicates on `(vp ^ albedo * 0x9E3779B97F4A7C15)` into
`m_viewmodel_basis_census_seen`, a set that lives for the whole session — so **each (vp, albedo) pair emits
exactly ONE line per session, ever.** Round 36's 19,200-frame play-test produced **nine lines across six
frames**.

An instrument that samples each object once per session **structurally cannot** observe jitter. Not "did not
this time" — cannot, at any cap, for any length of play-test. Every round that asked this log about
smoothness was asking a question the instrument could not answer, and that is the round's most important
finding even though it renders nothing.

Shipped: `RPCS3_REMIX_VMBASISEVERY` (0 = the old dedup, N>=1 = drop the dedup and emit on
`frame % N == 0`) and `RPCS3_REMIX_VMBASISVTX` (0 = any, else only that vertex count). `VMBASISMAX`'s
ceiling raised **96 -> 65536**; the launcher arms **4000**, which is inside it.

Armed `VMBASISEVERY=1 VMBASISVTX=3649 VMBASISMAX=4000` = ~4000 **consecutive** frames of the gun body,
about two minutes at 30 fps. Fallback if that mesh is not drawn: `VMBASISVTX=0 VMBASISEVERY=4`.

### TASK 2. The weapon is too close, and the round-36 operator has no lever that can reach it

`cdist_post == cdist_pre` was pre-registered by round 36 as a success criterion and it is one — a rotation
about the eye cannot change the distance from the eye. That is also precisely why the round-36 operator can
place the weapon in front of you at the wrong distance and be unable to correct it. Not a tuning gap: a
structural one.

**Shipped: `RPCS3_REMIX_VMROTPIVOTRIGHT` / `VMROTPIVOTUP` / `VMROTPIVOTFWD`**, signed world units, applied
to the pivot on the camera's own axes inside `apply_viewmodel_rotation`. With the pivot at camera-space
`(p_r, p_u, p_f)`, a 180-degree turn about `right` sends `(r, u, f)` to `(r, 2 p_u - u, 2 p_f - f)`.
**The corrected model therefore moves by TWICE the offset**, and that factor of two is a falsifiable
prediction, not a description: `cpost`'s forward component must change by exactly `2 x VMROTPIVOTFWD`. A 1x
change would mean the offset is being applied as a translation and the composition is wrong.

**Why 0.15 and not a number picked by eye.** MEASURED: `cpost` forward reads 0.09 .. 0.55 over the nine
lines, `cdist_post` 0.38 .. 1.04. The world scale is ~1 unit = 1 metre, and that is measured from the camera
rather than assumed — between census frames 10013 and 10014 the eye moved **0.11 units in one frame**, and
0.26 units over the four frames 9656..9660, i.e. **2.0 .. 3.3 units/second at 30 fps**, which is human
walk-to-jog speed and is not consistent with any other scale. At 1 unit = 1 m the gun origin is sitting
**9 to 55 centimetres** from the eye, which is what "seems a little too close" and "had to free cam back just
to see [the gun's screen]" both describe. `0.15` pushes it +0.30 m, to roughly 0.4 .. 0.85 m.

Ladder, one launcher edit each: still too close `0.25` then `0.35`; too far `0.08`; weapon too low
`VMROTPIVOTUP=0.15`. `UP` and `RIGHT` ship at 0 deliberately — arming two at once confounds them, which is
the doctrine round 36 used for `VMBASIS`.

`env_float_signed`, never `env_float`: 0 is the OFF value for all three and `env_float` rejects 0 (round 32's
`SKYANCHOR=0` defect). Negative is a real value — it pulls the model towards the eye — so a reader that
clamped at zero would delete half the range. No ceiling on any of the three.

### `RPCS3_REMIX_VMROTLOCK` — the test that separates "genuine pose" from "second defect", shipped OFF

Nothing in the log distinguishes those two readings of the residual, so this exists to be armed once and
judged by eye. Applied after the rotation, about the **same pivot**, so it can only turn the instance and
never move it — `dcentre` must not change when it is armed, and if it does, the pivot did not survive.

- `1` — replace the object's 3x3 with the camera's basis, `Q = C U^T` on Gram-Schmidt-orthonormalised object
  **columns** (columns, because the transform is column-vector 3x4 and column *j* is where model axis *j*
  points). Column lengths survive because `Q` is orthogonal, so scale is preserved.
- `2` — the minimal rotation taking the object's X column onto the camera's right axis, and nothing else.
  This is the half the identity above says is responsible for the whole residual.

**`relpost` reading ~0 afterwards proves only that the operator composed. It is not evidence about the
world**, and pre-registering it as if it were would be exactly the tautology this project has been caught by
six times. The evidence is what the user sees: arms look right and stop swimming -> the residual was a
defect; arms freeze rigid, lose their aim pitch, hands detach -> the residual was the genuine pose and the
knob goes back to 0. Either outcome settles it in one play-test.

Reachable with `VMROTAXIS=0` on purpose (`rot` stays the identity and the composition is a no-op), so the
two can be A/B'd one variable at a time.

### Census: nine new fields, and why `camvp`/`camage` belong on this line

`Remix vmbasis:` gains `camvp= camage= lock= pivotoff=[r u f] relaxis=[right up fwd]` immediately before
`frame=`. Format audited position by position: **85 specifiers against 85 arguments**, `camvp` at spec[73]
against `m_active_camera.vp_hash`, `relaxis` at spec[79..81] against `rel_axis(post, 0..2)`.

Settling the staleness question this round required cross-referencing `Remix gauge:` by frame number for six
separate frames. The provenance of the camera the operator actually used belongs on the operator's own line.
And it is not idle: **MEASURED over 161,354 gauge rows, the elected camera is one of TWO programs** —
`7f3d3abcefc8b057` at 512x288 (145,158 rows, 89.9%) and `ad7ce9d672a0bf6b` at 1024x576 (16,112 rows, 9.98%),
plus 84 rows at 2048x2048 — and **10,293 of 46,636 frames carry both**. A 180-degree rotation **doubles** any
error in the axis it turns about, so which of the two was elected is a first-order term in where the weapon
lands. `VMBASISEVERY=1` plus `camvp=` on every line is the first configuration that can see that.

`relaxis` is extracted from the antisymmetric part, which degenerates as the angle approaches 180. That is
not hidden: it returns zeros, and a zero axis beside a `relpost` near 180 means "unavailable", not "along
nothing".

### TASK 3. The UI route WORKS. It was under-keyed by two thirds

Round 36's pre-registration said "zero means the hashes never matched, not that the mechanism failed". The
counter is not zero and the mechanism did not fail. MEASURED on the round-36 log:

- `ui_forced_pair=14664`, and the cadence is **exactly one per frame** over four consecutive `Remix stats:`
  intervals: 101 frames/101 increments, 99/99, 105/105, 104/104. The counter is per sub-draw
  (`submit_subdraw` has exactly one call site), so that arithmetic is the right check and it passes.
- **138 `Remix uiwrap: vp=830d7d1b9681c475 fp=479890ff55f1d96e albedo=C61753D31FB96507 … route=2d` lines.**
  `report_ui_wrap` is called after rasterization, so a line there is proof the draw was composited.
  `DrawScreenOverlay failed` appears **0** times.
- Round 35's structural claim is **CONFIRMED**: `screen_space = true` falls through to a `return` inside
  `submit_subdraw` that precedes both `per_draw_transform` and the world `DrawInstance`, and every compositor
  refusal is a bare `++counter; return;` — a refusal deletes the draw, it never falls back to world. No draw
  can be submitted twice.

**ROOT CAUSE: the helmet albedo `C61753D31FB96507` is submitted by THREE (vp, fp) pairs and the round-36 key
had one slot.**

| (vp, fp) | lines | route |
| --- | --- | --- |
| `830d7d1b9681c475` / `479890ff55f1d96e` | 138 | **UI**, forced (round 36's pair) |
| `f39f504649b6f442` / `4afa02b3dbbe9b7e` | 33 | **WORLD**, submitted — **all 33 that albedo, no other** |
| `830d7d1b9681c475` / `609a4216b89e296a` | 2 | **WORLD**, submitted |

The second pair is dedicated to the helmet and reads `dw=0`, so the existing `!depth_write_enabled()` guard
admits it. It was never excluded on the merits; it was not in a key with one slot.

Shipped: `RPCS3_REMIX_UIFORCEPAIRVP2` / `UIFORCEPAIRFP2`, comma-separated lists of up to eight additional
pairs matched **positionally** — slot *i* against slot *i*, never crossed. Crossing two three-entry lists
would force nine combinations of which six are other geometry, which is round 20's failure mode exactly. The
call site now tests `ui_force_pair_any_matches`, which is the OR of the round-36 single pair and the list, so
blanking `UIFORCEPAIRVP2` restores round 36 bit for bit and blanking `UIFORCEPAIRVP` as well restores round
35. `uiforcepairs=` on the knobs line prints the number of **complete** pairs. `Remix stats:` audited at
**260 specifiers against 260 arguments**, `uiforcepairs` at spec[245].

**Two corrections owed to round 36's pre-registration.**

`ui_skipped` and `ui_space_none` are the **same event**. They are separate members but they are incremented
on two adjacent lines of one `else` branch in `composite_ui_draw` ("Neither space. Guessing would put the UI
somewhere arbitrary; counting is honest." then `++m_stats.ui_space_none; ++m_stats.ui_skipped; return;`).
`ui_skipped` has six other increment sites and their equality at 5077 proves **none of them ever fired**.
Round 36 pre-registered `ui_skipped` as a distinct failure signal; it carries no independent information
here. Worse, it is **stale**: it froze permanently at 5077 at frame 9678 and did not move for the following
13,800 frames while `ui_forced_pair` kept ticking.

`remix_c.h` confirms round 35 on shadows and there is **no overlooked bit**. `remixapi_InstanceInfo` has six
fields — `sType, pNext, categoryFlags, mesh, transform, doubleSided` — and no shadow control; the chainable
EXTs are BoneTransforms, Blend, ObjectPicking, ParticleSystem, GpuInstancing, none with a shadow field. All
27 category bits were enumerated and none is "hidden from secondary rays": `HIDDEN` removes primary too,
`IGNORE` is documented in-tree as a no-op on the API draw path, `HAIR_CARDS` is unimplemented. **If the UI
route ever fails to deliver "visible, no shadow, no clipping", the API as it stands has no other flag that
can, and the answer would have to be a second submission (a shadow-only proxy) rather than a category.**

**Stated before it is seen:** the existing forced pair's `uiwrap` lines read `u=[2676..30089]
v=[3417..28455]` on a 512x512 texture — three orders of magnitude outside `[0,1]`. Whatever the compositor
paints for the helmet today is not a sane atlas region. Expect the newly-routed copies to look like the
composited one, not like the world one. That is a separate defect: named, not fixed.

### TASK 6 (added mid-round). The weapon screen is NOT a stale texture — refuted, and no `TEXREHASH` change shipped

The hypothesis was that descriptor-only texture identity leaves a guest-rewritten texture frozen at its
first-bind snapshot. The mechanism is real but **it is not what is happening to this surface.**

First, the mechanism, corrected: the cache **key** is descriptor-only (`RemixTextures.cpp:308-317`, offset /
location / format / pitch / dims / mips / wrap / alpha), but the **albedo hash is genuine texel content** —
`fnv_bytes` over mip 0 mixed with format and dimensions (`RemixTextures.cpp:831-837`). `decode()` is the only
writer of `content_hash` and runs only on a descriptor-key **miss**. So identity is content-sampled exactly
once and then cached behind a descriptor key forever. "Descriptor-only, not content-sampled" is the wrong
phrasing for a real defect.

**MEASURED refutation for `86885A0E60751491`**, over the last 200 MB of `bin\remix_dump.log` (508,091 lines,
>=72 runs): **1,767 lines carry the hash and ZERO are `Remix texstale:`** — in a window containing **703
`texstale` lines naming 382 distinct content hashes**, 42 of them at the same `1024x1024 fmt=86` shape. The
detector was live and would have named it. And the submitted UV span is **bit-identical**
(`u=[0.1116..0.9944] v=[0.01239..0.992]`) across all four `Remix picked:` lines for that hash, at frames
3364, 4826, 10633 and 13920 and across two different vertex programs — so the digits are not arriving by UV
animation either. The pick landed on the weapon **body** atlas (DXT1 1024x1024); the live numerals are a
different draw. Limit stated honestly: `texstale` is capped at 64 lines per run and deduped by descriptor
key, so this is strong evidence, not proof.

**No `TEXREHASH` change shipped, and that is the finding.** Had it been armed, the cost is documented and
large: the albedo hash folds into the **mesh** key at three sites in `RemixGSRender.cpp` (the static-index
entry key, the static-index UNION key and the ordinary mesh key) as well as the material key, so a content
hash that moves re-keys every mesh that binds the texture. The in-tree precedent is 5,881 -> **188,427**
meshes created.

**A selective refresh is not possible with this header.** `remix_c.h` has `CreateMaterial`,
`DestroyMaterial`, `CreateTexture`, `DestroyTexture`, `AddTextureHash`, `RemoveTextureHash` — and **no
`UpdateMaterial` and no `UpdateTexture`**; the only `Update*` in the whole interface is
`UpdateLightDefinition`. And `remixapi_InstanceInfo` carries no material field, so the material cannot be
overridden per instance. The nearest existing code is the hash-pinned in-place re-decode at
`RemixTextures.cpp:532-560`, whose only caller is the UI compositor. Making it work for 3D would mean
`DestroyTexture` + `CreateTexture` with `info.hash` left at the pinned `content_hash` — which **hinges on
whether the fork's `remixapi_CreateTexture` replaces a texture already registered under an existing hash**,
and that symbol (`textureHashPathLookup`) is absent from every local dxvk-remix clone. Unverifiable from
current bytes; it would have to be measured against the deployed DLL.

### TASK 4. Light fixtures: two of round 34's premises do not survive re-measurement

No fix shipped, on evidence. Round 34 reported the fixture family as `vp=830d7d1b9681c475
fp=c61b0b9586dd67fb` and said a `(vp, fp)` gate isolates it because "seven fixture albedos" separate cleanly
from the rigid arms by eye distance.

- **REFUTED: they are not viewmodel-tagged, so the round-36 flip is not doing it.** `viewmodel=0` on all 8
  `Remix picked:` lines for that fp, `vmlisted=0` and `listedvp=0` on all 11,775 `Remix fpcandidate:` lines,
  and `depth_write=1` throughout. A 180-degree flip about the eye is the one operator that makes an object
  move with the camera, so this was worth checking before anything else — and it is not the cause.
- **REFUTED: the `(vp, fp)` pair is not narrow.** It carries **438 distinct albedos over 11,775 rows** in the
  last 300 MB, the top one appearing 278 times. A `(vp, fp)`-keyed fix over-matches roughly **60x**, not 1x.
  Round 34's "seven fixture albedos" describes a much smaller window than the current log.
- **The lead for round 38 is an existing classifier, not a new key.** `Remix light-candidate:` already prints
  `state=fixture` per albedo on exactly this family, e.g. `albedo=BEE05E8D1184F67E … extent=0.4627
  state=fixture depth_write=1 vtx=1020`. That predicate already separates the fixtures the `(vp, fp)` pair
  cannot. Key any future gate on it.

### TASK 5. `c1d482dcd1b03ed0` is not the viewmodel path either

Checked the one hypothesis that would have made it cheap: a world draw mis-tagged VIEW_MODEL gets the
180-degree flip about the eye and would therefore literally follow the camera. **REFUTED.** MEASURED over
the last 300 MB: `viewmodel=0` on all 7 `Remix picked:` lines, `vmlisted=0` and `listedvp=0` on all **6,406**
`Remix fpcandidate:` lines, and `scale_z=0.49875 offset_z=0.50125` on every line that prints them — the
ordinary world depth range, not the viewmodel's `0.00125`. It never reaches the viewmodel tag.

What the census does show, unexplained and worth the next round's attention: on `Remix worldid-draw:` this
program's recovered transform carries a **non-zero translation on geometry whose raw vertex AABB is already
absolute world**, e.g. `raw=[1762.63 -26.6278 1076.73]..[1786.28 -13.1973 1157.18]` with
`pre=[… 15.0431; … 42.5709; …]` and `translation=42.5709` at `camage=0`. Applying that transform to already-
absolute vertices displaces the mesh ~42.6 units.

### Corrections owed to standing documents

- **MADE:** `relpost = 180 - relpre` is not a general property of the round-36 operator. It is the special
  case `dotpre[0] = 1`. The general form is `cos(relpre) + cos(relpost) = dotpre[0] - 1`.
- **MADE:** `relpre = 180.000` on a census line is a **clamp**, not a measurement — `relative_angle` clamps
  `(trace-1)/2` to `[-1,1]` and the object columns are only normalised, never orthogonalised, so a slightly
  non-orthogonal basis pushes the trace below -1. Two of the nine lines read 180.000 for that reason.
- **MADE:** `ui_skipped` and `ui_space_none` are the same event; round 36's "distinct failure signal"
  pre-registration is void. `ui_skipped` has been frozen at 5077 since frame 9678.
- **MADE:** round 34's "the (vp, fp) pair isolates the fixture family" — the pair carries 438 albedos.
- **STILL OWED**, none done this round: `classify_draw`'s "IGNORE is a no-op on the API draw path" comment
  still wants confirming by disassembling `bin\remix\d3d9.dll` (the remix_c.h read this round confirms the
  bit exists, not that it does nothing); `RemixTransforms.h`'s "What `DRAWAUDIT=0` costs" list still names
  neither `m_streak_measured` consumer; the frame-time split was not measured again.

### Play-test card

Launch as usual (`launch-haze-remix.cmd` -> `bin\rpcs3-next.exe`, **`56A0A57DA51FF02E`** — hash it).

| what to look at | where to read it | one-line revert |
| --- | --- | --- |
| **Weapon at a sane distance?** | `Remix vmbasis:` -> `cpost=[right up fwd]`. Forward must be ~0.30 LARGER than round 36's, and `rotpivot=` must NO LONGER equal `cam=`. knobs line: `vmrotpivotf=0.15` | `set "RPCS3_REMIX_VMROTPIVOTFWD=0"` |
| still too close / too far | same field. Each change moves it by 2x | `0.25` then `0.35` / `0.08` |
| weapon too LOW | `cpost` up | `set "RPCS3_REMIX_VMROTPIVOTUP=0.15"` |
| **Does it follow the camera smoothly now?** | this is the run that finally MEASURES it — `Remix vmbasis:` should carry ~4000 consecutive frames. **Turn left and right for a few seconds while playing**, or the series contains no motion | `set "RPCS3_REMIX_VMBASISEVERY=0"` |
| no `Remix vmbasis:` lines at all | vtx 3649 was not drawn | `set "RPCS3_REMIX_VMBASISVTX=0"` + `set "RPCS3_REMIX_VMBASISEVERY=4"` |
| still not smooth after the distance fix | the residual A/B. **Only after**, never both at once | `set "RPCS3_REMIX_VMROTLOCK=1"`, then `2` |
| **Helmet: visible, no shadow, no clipping?** | knobs line `uiforcepairs=` must read **2**. `ui_forced_pair` should roughly double, to ~2/frame — check against the frame delta on two `Remix stats:` lines | `set "RPCS3_REMIX_UIFORCEPAIRVP2="` |
| helmet VANISHED | `ui_skipped` — frozen at 5077 since frame 9678, it would have to MOVE | same revert |
| helmet visible but wrongly textured | expected; the existing pair's UVs are already 3 orders out of range | not fixed this round |

## Round 36 (2026-08-24)

Deployed: `bin\rpcs3.exe` = `bin\rpcs3-next.exe` = **`F9206BA434AE3ADB`**. Runtime `bin\remix\d3d9.dll`
**UNCHANGED at `16A0B512F33EBB66`** — nothing was deployed into `bin\remix\`. Previous `rpcs3-next.exe`
saved as `bin\rpcs3-next-pre-round36-20260824.exe` (`C87A2930E676F5F8`).

### READ THIS FIRST: the title was NOT running round 35's binary

MEASURED at the start of this round. `bin\rpcs3.exe` was `E984DD7DAE13CAD5` dated 2026-08-17 13:21 — round
35's build, exactly as its inbox said. But **`bin\rpcs3-next.exe`, which is the file the launcher runs, was
`C87A2930E676F5F8` dated 2026-08-23 08:01 and 7,680 bytes larger**, copied from
`build\codex-link\rpcs3.exe`. A concurrent session (a Resistance 2 effort, Aug 22-23) edited
`rpcs3\Emu\RSX\Remix\RemixGSRender.cpp` on 2026-08-23 07:51, built into its own output directory, and
deployed that. `launch-haze-remix.cmd` also carries an Aug 22 23:11 edit.

Consequences, stated plainly: round 35's inbox reported the two hashes as identical and they were not by the
time the play-test happened; the play-test verdicts quoted in the round-36 brief were produced by
`C87A2930E676F5F8`, whose Remix source differs from round 35's in ways this round did not diff. The round-36
build is taken from the CURRENT tree, so it contains round 35 + that session's changes + this round's, and
supersedes both. **Check `bin\rpcs3-next.exe`'s hash, not `bin\rpcs3.exe`'s, before trusting any play-test.**

### THE VIEWMODEL IS NOT MIS-ORIENTED. IT IS SUBMITTED BEHIND THE CAMERA.

The brief was right that the sign-flip model is refuted, and right that the diagonal camera-axis dot products
cannot settle it. The reason is that they contain no position at all, and position is the whole defect.

MEASURED over 287 `Remix vmbasis:` lines in the last 400 MB of `bin\remix_dump.log` (the `VMBASIS=5`
session), by re-projecting each line's own logged `pre=`/`post=` matrices and `right=`/`up=`/`fwd=` camera
axes — no new build was needed, the data was already on the line:

1. **The camera basis is left-handed on all 287 lines**: `right x up . fwd = +1.00`, every line, no
   exceptions. So the sign of a forward coordinate is a fact about the scene and not a convention of the
   instrument. This is the control that has to come first, because everything below is a sign.

2. **Every near-eye VIEW_MODEL-tagged draw is submitted BEHIND THE EYE.** Centroid resolved onto
   (right, up, fwd) as an offset from `m_active_camera.position`, all groups within 5 units:

| vp | vtx | n | fwd | up | dist |
| --- | --- | --- | --- | --- | --- |
| `af06f6d32ec048ee` | 4 | 25 | **-0.383 .. -0.056** | +0.023 .. +0.601 | 0.19 .. 0.68 |
| `aae8e0d5ae292dd4` | 91 | 22 | **+0.083 .. +0.484** | -0.067 .. +0.299 | 0.478 .. 0.490 |
| `15ad612980aca110` | 8 | 8 | **-0.586 .. -0.450** | +0.101 .. +0.937 | 0.61 .. 1.05 |
| `f39f504649b6f442` | 3649 | 7 | **-0.647 .. -0.467** | +0.142 .. +0.381 | 0.59 .. 0.76 |
| `830d7d1b9681c475` | 3649 | 7 | **-0.542 .. -0.405** | +0.150 .. +0.402 | 0.46 .. 0.68 |
| `57a12323f22f4988` | 8 | 7 | **-0.627 .. -0.451** | +0.097 .. +0.927 | 0.63 .. 1.03 |
| `830d7d1b9681c475` | 2140 | 6 | **-0.206 .. -0.131** | +0.373 .. +0.628 | 0.43 .. 0.65 |
| *(20 further groups)* | | | all negative | mostly positive | < 1.1 |

   **`aae8e0d5ae292dd4` vtx=91 is the control and it is the important row**: it is the one near-eye group in
   the whole census that is NOT viewmodel-tagged, and it reads fwd **positive** — in front of the eye, 0.48
   units out, 8.6 degrees off camera-aligned. That is what a correctly placed first-person object looks like
   on this instrument. Every tagged group reads the opposite sign.

3. **The relative basis is a rotation about the camera's RIGHT axis, 118 to 179 degrees.** Full 3x3
   direction-cosine matrix, orthonormalised columns: `det = +1.00000`, column norms `1.0000`, axis component
   on `right` >= 0.996 on every near-eye line. Not a yaw, not a mirror, not a shear, and not a
   "24-degree misalignment on two axes" — that reading is what a single clean rotation looks like when only
   its diagonal is printed. The spread 118 / 155 / 156 / 174 / 177 / 178 / 179 is the rig's own aim pitch
   (180 minus the object's pitch), which differs per object in the same frame.

**One operator produces all three at once: 180 degrees about the camera's right axis, PIVOTED AT THE EYE.**
It sends `up -> -up`, `fwd -> -fwd`, `right -> right` and leaves the distance from the eye alone — which is
"upside down and backwards", the user's own words for `VMBASIS=0`, and it is the only single rigid motion
that puts an object simultaneously behind you, above you, and rotated 180 degrees about your right axis.

**Why `VMBASIS=6` did not fix it, and this is the part a sign mask structurally cannot reach.** At 180
degrees the Rodrigues form collapses to `-I + 2 a a^T`, which is bit-for-bit what `sum_k s_k a_k a_k^T` with
`s = (+1,-1,-1)` already builds — so flip 6's **3x3 was correct all along** and `dotpost = (+,+,+)` was
telling the truth about the orientation. What flip 6 could not do is move the mesh, because rounds 34-35
pivoted it at the **centroid** (`VMBASISPIVOT=1`). It turned the arms in place and left them above and
behind the eye. Correctly-oriented arms hanging behind your head read as "still upside down", which is
exactly what came back. Verified on the census's own fields: at flip 6 the post-operator relative angle is
**2.1 / 2.7 / 4.1 degrees** for the three `830d7d1b9681c475` / `57a12323f22f4988` groups — essentially
camera-aligned — while `dcentre <= 6.5e-06` says directly that the mesh never moved.

Pivot 0 was not the escape, because pivot 0 is the **anchor-frame** eye (`anchor_frame_eye()`, and
`RPCS3_REMIX_CAMANCHOREYE=1` is armed at `launch-haze-remix.cmd:482`), which round 34 measured throwing the
mesh 2537..2564 units.

**The raw eye is the right pivot, and that is a measurement, not a preference.** `cdist_pre` is
`|centroid - m_active_camera.position|` and reads **0.43 .. 1.04 on all 287 lines of the current build**. A
centroid half a unit from that point cannot be in a different frame from it.

### Shipped: `RPCS3_REMIX_VMROTAXIS` / `VMROTDEG` / `VMROTPIVOT` — a real rotation

`apply_viewmodel_rotation` (`RemixGSRender.cpp`, immediately above `report_viewmodel_basis_census`). Runs
after `apply_viewmodel_basis` and is independent of it.

- **axis** `0` off; `1/2/3` = the camera's right/up/forward axis, Rodrigues, LEFT-multiplied about the pivot;
  `4/5/6` = the MODEL's own X/Y/Z, RIGHT-multiplied (`M' = M * R`) about the object origin. 4..6 is the
  "authored Z-up in a Y-up renderer" hypothesis the brief named — it would be `VMROTAXIS=4 VMROTDEG=270` —
  and the pivot knob is inert there (census reports `rotpivsrc=5`).
- **deg** taken `% 360`. **`VMROTDEG=360` therefore parses to 0 and means OFF** — write 180.
- **pivot** `0` raw eye (armed), `1` centroid, `2` translation, `3` anchor-frame eye (= round 19..35).
- Clamps: `min(env,6)`, `% 360`, `min(env,3)`. Armed `1 / 180 / 0`, all inside. `env_u32` on all three, never
  `env_float` — `env_float` rejects 0 and 0 is the OFF value for two of them (round 32's `SKYANCHOR=0`
  defect).

**Launcher: `VMBASIS` 5 -> 0.** The flip sweep is closed; composing both is legal but confounds them.

### Census: the observable that is not a dot product

`Remix vmbasis:` gains one contiguous block before `frame=`:
`rotaxis= rotdeg= rotpivsrc= rotpivot=[..] drot= cpre=[right up fwd] cpost=[..] relpre= relpost=`.

- **`cpre`/`cpost`** are the geometry centroid resolved onto the camera axes as an offset from the eye, in
  metres. This is the field that names the bug and it is the one the brief asked for. `eye_pre`/`eye_post`
  already resolved the object ORIGIN onto those axes, but on this title that origin is the world origin and
  says nothing about the mesh — which is why round 34 added `centre_pre`/`centre_post` in the first place.
- **`relpre`/`relpost`** are the ANGLE of the full relative rotation, from `trace(D) = 1 + 2 cos(theta)` on
  orthonormalised columns. Three diagonal cosines are three of nine numbers; one angle cannot be misread the
  way `(+0.99, -0.47, -0.46)` was.
- **`dbasis` now measures pre -> MID, not pre -> post**, and `drot` measures mid -> post, where `mid` is the
  transform between the two operators. Otherwise adding a second operator would have silently re-pointed an
  existing field at the sum of both. With `VMBASIS=0` armed, `dbasis` reads 0 and that is correct.

Format audited position by position: **`Remix vmbasis:` 76 specifiers / 76 arguments**, with `rotaxis` at
spec[58] against `viewmodel_rotate_axis()`, `drot` at spec[64] against `drot`, `relpost` at spec[72] against
`relative_angle(post)`.

**PRE-REGISTERED for `VMROTAXIS=1 VMROTDEG=180 VMROTPIVOT=0`** — every one of these can read otherwise:

| field | must read | what a different reading means |
| --- | --- | --- |
| `cpost` fwd | **> 0** where `cpre` fwd was < 0 | still behind the eye: the pivot or the axis is wrong |
| `cpost` up | **< 0** where `cpre` up was > 0 | same |
| `cpost` right | unchanged to ~1e-3 | a rotation about `right` cannot move the right component |
| `relpost` | `180 - relpre` +- 1 deg, i.e. **1 .. 62** | the 3x3 did not compose |
| `cdist_post` | == `cdist_pre` to 1% | ~2550 means the anchor-frame eye leaked back in |
| `dcentre` | **0.3 .. 1.6** | ~0 means a CENTROID pivot leaked back in (`cdist` alone cannot see this — it is invariant under both pivots, `dcentre` is the discriminator) |
| `dbasis` / `drot` | 0 / ~2 | `dbasis` non-zero means `VMBASIS` is still armed |
| `knobs=` | `vmrotaxis=1 vmrotdeg=180 vmrotpivot=0` | anything else is a clamp or a typo |

If the arms come out right way up at the wrong PITCH, that residual is the 118..179 spread and the answer is
a smaller angle on the same axis (`DEG=156`, then `118`) — one launcher edit, no rebuild. If they come out
upside down the OTHER way, the model-space hypothesis is live: `VMROTAXIS=4 VMROTDEG=270`.

### The helmet: `RPCS3_REMIX_UIFORCEPAIRVP` + `UIFORCEPAIRFP`, and the vp-only key would have over-matched 26x

Ground truth, the user's Ctrl+Click this session:
`vp=830d7d1b9681c475 fp=479890ff55f1d96e albedo=C61753D31FB96507 vtx=59 extent=6.465 depth_test=0
depth_write=0 blend=1`.

MEASURED over the last 400 MB of `bin\remix_dump.log` (1,038,135 lines, counted twice by two independent
scripts that agree exactly):

- `vp=830d7d1b9681c475` alone: **47,399 lines across 31 distinct fragment programs**, its two largest fps
  carrying **230 and 193 distinct vertex counts**. It draws most of the scene. Adding it to `UIFORCEVP` is a
  **~26x over-match** and is round 20's failure mode exactly.
- the PAIR `(830d7d1b9681c475, 479890ff55f1d96e)`: **1,833 lines, 1,767 of them (96.4%) at albedo
  `C61753D31FB96507` / vtx=59** — the helmet. Residue: 35 lines of a vtx=152 family at extent 2.16..2.52,
  and 9 quads at vtx 6/14/44/66 with extent 0.09..0.13.
- The helmet's pick state `depth_test=0 depth_write=0 blend=1` is **unique among all 227 picks** for that vp.

**Why the UI route and not a category flag** — on round 35's reading of the deployed runtime, not on
preference. There is no per-instance castShadow flag anywhere in it; `Hidden` sets `mask = 0` and kills
primary rays too (the helmet would vanish, failing "visible"); `THIRD_PERSON_PLAYER_MODEL` needs
`rtx.playerModel.enableInPrimarySpace = True`, which masks every VIEW_MODEL candidate to zero and would take
the ARMS with it — a head-on collision with this round's viewmodel work. A UI-forced draw returns from the
screen-space block **before** `per_draw_transform` and `submit_subdraw`: no mesh, no instance, no material,
**no BLAS**. No shadow and no world clipping by construction rather than by flag.

The existing guard is doing real work and is kept: `!depth_write_enabled()`. The helmet reads
`depth_write=0`; any member of the pair that writes depth stays on the world path.
`m_current_fp_hash` is fresh at that site — written once per clause at `RemixGSRender.cpp:3766`, before
`draw_call.begin()`.

Counter `ui_forced_pair` on `Remix stats:`, kept separate from `ui_forced` for the reason round 35 had to
split `cat_hidepair` out of `cat_hidden`: a shared counter cannot say which key matched. Both hashes print
as `uiforcepairvp=` / `uiforcepairfp=` (the PARSED values, so a typo that parses to 0 and silently disarms
the route is visible). Format audited: **`Remix stats:` 259 / 259**, `ui_forced_pair` at spec[242].

**THE RISK, stated up front:** the compositor can still refuse a forced draw (`ui_skipped`, `ui_space_none`,
`ui_render_target`) and a refusal DELETES the draw rather than falling back to world geometry. If the helmet
DISAPPEARS instead of flattening, read those three counters — not `ui_forced_pair`.

### The black sky: the anchor test IS broken, and it is NOT why the sky is black

Two separate findings, and conflating them would have shipped a fix that made the problem worse.

**(a) The anchor test measures the wrong quantity — confirmed, fixed as a measurement.**
`sky_max_anchor()` is compared against `|transform.translation - eye|`. For an absolute-world draw (correct
identity-ish transform, world-space vertices) the translation IS the world origin, so the quantity it
evaluates is `|eye|`. The unlocked levels read `anchor=2137.85 limit=4` with the camera 2137 units out —
that identity to five figures. Round 31 measured the same thing on `VIEWMODELANCHOR`; round 32 already
listed `SKYANCHOR` as dead for absolute-world geometry. Shipped: `centre_anchor = |AABB centre - eye|`
computed in the loop that already has the box, printed as `canchor=` beside the existing `anchor=` on every
`Remix sky-census:` row at every mode, plus `anchormode=`. `RPCS3_REMIX_SKYANCHORMODE` selects the
comparison: `0` legacy (default, bit-for-bit), `1` centre-vs-limit, `2` eye inside the AABB, `3` = 2 or 0.
Format audited: **`Remix sky-census:` 35 / 35**, `canchor` at spec[30].

**(b) Admitting more domes would make them WORSE, and this launcher already worked out why.**
`launch-haze-remix.cmd:1775-1790` reads the runtime source directly: `rtx_instance_manager.cpp:1006` sets
`m_isHidden = true` for `CameraType::Sky`, and `rasterizeSky()` is unreachable from an API-submitted draw.
`is_sky` sets `REMIXAPI_INSTANCE_CATEGORY_BIT_SKY` at `RemixGSRender.cpp:21468`. **On this backend the SKY
tag can only ever make the dome INVISIBLE.** So `SKYANCHORMODE=2` would turn black domes into absent domes.
It is shipped at 0 and deliberately not armed.

Second, independent reason not to arm it, MEASURED on the 278 `reject:anchor` rows of the last 250 MB:
**146 of them are HIGH-vtx terrain (1211..12875 vertices) against 69 plausible dome bands**, and `inside=1`
fires on **exactly 146 of the 278**. Until somebody cross-tabs `inside=` against `vtx` on those rows, mode 2
may be admitting precisely the terrain and refusing precisely the domes. No anchor threshold separates them
either — terrain saturates first at every value tested (at `anchor<=10`: 51 terrain to 2 domes; at `<=100`:
142 to 12). What DOES separate them: the two populations share **zero vertex programs and zero albedos**, and
`backdrop=1` fires on 39 dome rows and **0** terrain rows. A future rule keys on the program, the albedo, or
the already-computed `backdrop` predicate — never on a looser distance.

**(c) The actual black-sky fix is `RPCS3_REMIX_SKYEMISSIVE`, and it needed no rebuild.**
`sky_emissive_albedo_matches()` is consumed in `RemixTextures.cpp:1049` and `:1235` against the texture
CONTENT HASH, with no reference to `is_sky` or to the anchor gate at all. It is what makes a dome visible on
this backend. The list held two hashes; the newly-reachable levels use different dome textures and were
therefore never lit. Added the four largest anchor-rejected dome candidates, each recurring across six
separate runs in the log:

| albedo | max wext | vtx |
| --- | --- | --- |
| `35C2353F6B3CE2A8` | 2.14e6 | 54..79 |
| `174F4F689CF2A3D8` | 1.83e6 | 42..66 |
| `3213E0CC136ED294` | 1.44e6 | 45..90 |
| `32AE81D64BEA29CD` | 1.29e6 | 147..266 |

Low vertex counts against million-unit extents: dome latitude bands. The terrain in the same
`reject:anchor` pile runs vtx 1211..12875 at wext under 79k and shares no albedo with these four.
List bound verified against the parser: `std::array<u64, 8>`, buffer 400 wchar; **six entries, 101 chars**.
`sky_emissive_albedo_count()` already prints on the knobs line and **must read 6**.

**The `haze_sun_table.csv` and `levelnames.json` named in the round-36 brief DO NOT EXIST.** Exhaustive
recursive search of `C:\Users\Tristan\AppData\Local\Temp\claude` (116 files total across 5 session
directories), `C:\Users\Tristan\.claude` and the repo found neither, nor any directory named `haze` holding
them. Per-level sun azimuth/elevation/colour must be re-mined before it can be used.

### The four other distance gates, audited against current bytes

Round 32 found two inert and two meaningful. Re-audited now that the underlying quantity is known to be
wrong, the split is different and it is **which quantity**, not which is armed:

| gate | quantity compared | file:line | verdict |
| --- | --- | --- | --- |
| `VIEWMODELANCHOR` | `transform.matrix[i][3] - eye` (TRANSLATION) | `RemixGSRender.cpp:21203-21208` | **wrong quantity**, but it never refuses — it only increments `viewmodel_far`. So the gate is harmless and the COUNTER is misleading evidence. |
| `VMPAIRMAXDIST` | `transform.matrix[i][3]` (TRANSLATION) | `RemixGSRender.cpp:21094` feeding `viewmodel_pair_rejects` at `:6431` | **wrong quantity, and this one does refuse.** Currently inert only because `VMPAIRVP` is blank. |
| `SUNCARDMINDIST` | the geometry CENTRE vs the anchor-frame eye | `RemixGSRender.cpp:4763` | **correct quantity** |
| `FPCENSUSMAXDIST` | `geometry_centre_in(transform, centre)` | `RemixGSRender.cpp:7447+` | **correct quantity** |

### Geometry following the camera: round 34's finding is REFUTED, and so is its mechanism

The brief asked me to confirm the bimodal world divide is still live and fix it. It is not, and the named
mechanism is refuted independently. No fix shipped, on evidence.

- **There is no `discard=` field and there never was.** Whole-file byte scan of all 923,425,680 bytes:
  `discard=` **0 hits**, `disc=` 0, `worlddiv` 0. Round 34's "discard" is a narration name for
  `translation=` on `Remix worldid-draw:` — the code calls it exactly that at `RemixGSRender.cpp:19511`,
  "the transform the override is about to discard".
- **On `d0b6a471bb2d463b` the >1000 bucket is 6 of 5,380 rows = 0.11%**, against round 34's claimed
  76 of 448 = 17.0%. **In the most recent run it is 0 of 72**, and that run's own cumulative census reads
  `tmax=2.23517e-08` over **35,202 draws**. Below the 1% refutation threshold on both readings.
- **The `camclip=512x288` vs `clip=1024x576` mismatch is still present — and cannot be the cause.** It
  disagrees on 96.43% of target rows and 98.79% of `Remix gauge:` rows, including **72 of 72 rows of the
  newest run, where the residue is 0%**. A condition present at 100% while the effect is at 0% is not the
  cause.
- **The bimodality is not unique to that mesh** and the named mesh is the LEAST affected of the top six:
  `c1d482dcd1b03ed0` 24.80% >1000, `ad7ce9d672a0bf6b` 2.14%, `0214281b9a7a412d` 2.13%,
  `bd1c10df5703e559` 0.68%, `d0b6a471bb2d463b` 0.11%. `basis_delta > 1000` is **0 rows on every vp**.
- **New MEASURED correlation, and it is honest in both directions.** Cross-tab of `camage` against
  `translation`, 265,453 rows:

| bucket | `Remix gauge:` rows | gauge t>=1000 | `Remix worldid-draw:` rows | worldid t>=1000 |
| --- | --- | --- | --- | --- |
| camage=0 | 62,927 | **0.01%** | 163,432 | **4.71%** |
| camage 2-8 | 4,032 | 0.52% | 15,154 | 0.09% |
| camage 9-64 | 1,859 | 9.04% | 11,700 | 0.27% |
| camage >64 | 1,468 | **28.61%** | 1,221 | **6.72%** |

  A stale camera anchor enriches the gauge outliers **2861-fold** and the per-draw outliers only **1.4-fold**.
  So the stale anchor explains the gauge REFERENCE going wrong and does **not** explain the per-draw residue.
  Corroborating: at frame 55760 `Remix gauge:` carries `translation=3346.54 basis_delta=1.63014 camage=711`
  and the identical numbers appear on `ad7ce9d672a0bf6b`, `c1d482dcd1b03ed0` and `d0b6a471bb2d463b` the same
  frame — the outlier is a property of the frame's reference, inherited by whatever draws in it.

The live lead is now **`c1d482dcd1b03ed0` at 24.80%**, not `d0b6a471bb2d463b`, and the question is what the
world-identity override is discarding on a quarter of that program's draws.

### Method notes earned this round

- **A dot product has no position in it.** Three rounds were spent sweeping an orientation knob against a
  defect that was three parts position and one part orientation, because the census printed only direction
  cosines. The fix was to print where the centroid actually is, in the frame the question is asked in.
- **Print the whole rotation, or at least its angle — never just the diagonal.** `(+0.99, -0.47, -0.46)`
  reads as "two axes about half wrong" and is one clean 118-degree rotation about the first axis.
- **When a symptom persists across every setting of a knob, suspect the knob's PIVOT, not its value.**
  `VMBASIS`'s 3x3 was correct from round 34 onward; only its pivot was wrong, and no amount of sweeping the
  3x3 could show that.
- **A control group is worth more than another measurement of the suspect.** One untagged near-eye draw
  reading `fwd > 0` while 26 tagged groups read `fwd < 0` is what turned "the sign convention might be mine"
  into "the placement is wrong".
- **Check what the tag DOES before widening the gate that grants it.** Admitting more sky domes would have
  hidden them, because SKY means hidden on this backend. The launcher had recorded that a year of rounds
  ago and the brief that asked for the widening had not read it.
- **The file the launcher runs is not necessarily the file you built.** `bin\rpcs3-next.exe` had been
  replaced by another session's build; hashing both is the only way to know.
