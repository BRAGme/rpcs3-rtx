# Round 51 (2026-09-06) — Demon's Souls: character shading, and the instrument the missing walls need

**Deployed:** `bin\rpcs3.exe` = `bin\rpcs3-next.exe` = md5 **`5B494D30C42CF1BBE05A687579C20729`**,
build stamp `Sep  6 2026 11:55:29`. The round-50 binary is preserved as
`bin\rpcs3-next-pre-vtxnormal-20260906.exe` (md5 `42B2578F856FC797BCC30C75FDE3E6C4`).
MSBuild `Release|x64` exit **0**, **0 errors**, no new warnings.

**CAUTION, read first:** a *second* session was running Eat Lead out of this same `bin\` directory at
11:59 while this round was deploying, so `bin\remix_dump.log` and `bin\log\RPCS3.log` are shared and
`rpcs3-next.exe` was swapped underneath that run. The swap is behaviourally inert for Eat Lead —
measured, see "what the first run already proved" below — but the logs are interleaved.

---

## 1. Character normals: ROOT CAUSE FOUND AND FIXED

**Every vertex this backend has ever submitted carried the constant object-space normal `(0, 0, 1)`.**
Not a Demon's Souls bug and not a recent regression: the one site that fills `m_scratch_vertices`
(`RemixGSRender.cpp`, the per-vertex decode loop) wrote

    v.normal[0] = 0.f;  v.normal[1] = 0.f;  v.normal[2] = 1.f;

for every vertex of every draw of every title, and had done since M1. It is the only place in the
backend that ever wrote a normal — the sole other occurrence is the hardcoded debug triangle.

It matters because Remix shades from the submitted normal, and **skins it**: dxvk-remix's skinning
pass (`src/dxvk/shaders/rtx/pass/skinning.h`) reads `srcNormal`, accumulates
`mul(bone, normal) * blendWeight` and renormalises, exactly as it does the position. So a constant
normal is a flat-shaded lie on every curved surface in the scene — nearly invisible on a flat wall,
maximally visible on a character, which is where it was reported.

**The fix** decodes the guest's own normal attribute with `decode_position` — the same decoder the
position uses, so every RSX packing is handled including `cmp` (X11Y11Z10, the compressed-normal
format), SNORM16, half and float — and submits it in **object** space, which is the space Remix wants
on both the rigid and the skinned path. Knobs: `RPCS3_REMIX_VTXNORMAL` (default `1`, `0` restores the
constant bit-exactly), `RPCS3_REMIX_NORMALATTR` (default `2`), `RPCS3_REMIX_NORMALCENSUS`.

**The wrong attribute cannot smear the scene.** The decode is gated per draw on the *mean
pre-normalisation length* of what it read; outside `[0.7, 1.4]` the draw keeps the old constant and
counts `rejected`. The gate tests the length the guest **stored**, not the length after normalising,
because a texcoord normalised per vertex would still be unit-length and still be wrong.

Counters `normals=<programs>/<applied>/<rejected>/<absent>/<degenerate>` on `Remix live:` **and**
`Remix stats:`; `vtxnormal=` / `normalattr=` on the run-start banner and the live knob tail.

### What the first run already proved — and it is not the run this was aimed at

The first run on the new binary was **Eat Lead**, not Demon's Souls, and it validated the instrument
better than a Demon's Souls run would have. `Remix vtxattr:` for the world program
`1b20d02263aa8ee4` reads:

    a0=ub2564[len=282.9/298.7/311.1  blen=0.9932  n=64]
    a1=ub2564[len=147.9/225.4/311.1  blen=0.9979  n=64]
    a2=s32k3 [len=3216/4137/4733     blen=8275    n=64]

Read straight off: **ATTR0 is a unit vector packed as biased bytes** (`blen` — the re-biased reading —
is 0.9932, i.e. 1.00 to within a byte of quantisation), ATTR1 is a second unit vector (tangent), and
ATTR2 is the position. That is an independent confirmation of round 50's ucode finding ("ATTR0 carries
a byte-quantised normal") by a completely different route, and it is why `NORMALATTR` is a knob rather
than a constant: **Eat Lead's normal is ATTR0, not ATTR2.**

On that run `normals=0/0/644/4221/0` — **applied = 0**. The gate correctly refused ATTR2 on every Eat
Lead program, so the new binary is behaviourally identical to round 50 for that title. That is the
regression check for the concurrent session, and it passed.

## 2. Missing geometry / walls: NOT the world-transform refusal, and the instrument was missing

Measured over all **40** Demon's Souls run segments in `bin\remix_dump.log` (126,349 lines), taking
each run's final `Remix live:` line. `world_refused` collapses across the DEMONSWORLD rounds:

| run | seen | submitted | world_refused | nocam | lay_other |
| --- | --- | --- | --- | --- | --- |
| early (#5) | 2,546,937 | 0 | 1,426,759 | 1,426,759 | — |
| mid (#7) | 3,540,286 | 1,101,084 | 773,484 | 163 | — |
| recent (#32) | 5,705,640 | 1,961,664 | **137** | 112 | 25 |
| recent (#36) | 5,939,176 | 1,997,191 | **2,132** | 112 | 2,020 |

`world_refused` partitions **exactly** into `nocam + lay_other` (112 + 2020 = 2132 on run 36), and in
run 36 the whole `lay_other` bucket is **two programs**: `7f4d3587c70d02da` (the particle program) and
`6f35d2e939a07d8d` (a 4-vertex blended quad). **So the missing walls are not a refused world
transform.** That hypothesis is closed on this evidence.

What *is* unexplained is the other end: run 36 saw **5,939,176** draws and submitted **1,997,191**.
Nothing on the live line says where the remaining ~3.9 M went. The partition that answers it — the
`skip screen= immediate= inline= volatile= reg_attr0= prim= restart= instanced= layout= mem= decode=
poison= vp= albedo= rt= rt_kept= camsurf= notinput= posdecode_refused=` group — existed **only** on
`Remix stats:`, which lands in `bin\log\RPCS3.log`, is held under an exclusive lock while the emulator
runs and is overwritten every run. A play-test standing in front of a hole in a wall could read `seen=`
and `submitted=` and could not read the difference between them.

**It is now mirrored verbatim onto `Remix live:`** — same names, same order — and verified emitting:

    skip screen=3284 immediate=0 inline=0 volatile=0 reg_attr0=0 prim=0 restart=0 instanced=0
    layout=540 mem=0 decode=0 poison=0 vp=0 albedo=1795 rt=462 rt_kept=0 camsurf=0 notinput=0
    posdecode_refused=0

It is cumulative like everything else on that line, so the reading is a **difference of two
consecutive lines** — the rate at a place — compared between a spot where the wall is missing and one
where it is not.

## 3. One thing found on the way, filed not fixed

`bin\rtx.conf` carries `rtx.hideInstanceTextures = 0x89F6FAA4C093B20D, 0xE5C6A3BF712D50CC,
-0x15CCD724E4F171B3, -0xB7AEEBE0C2365BC2`, and **`E5C6A3BF712D50CC` appears 350 times in the Demon's
Souls segments** — the only one of the 13 hidden/ignored hashes that does. `hideInstanceTextures`
deletes the geometry, not just the texture. Checked before believing it: the dumped image
(`bin\remix_tex\unit0_E5C6A3BF712D50CC_256x256.bmp`) is a grey cloud/noise 256x256, and every draw
that binds it is `vp=7f4d3587c70d02da` (the Demon's Souls **particle** program) at `blend=1
depth_write=0`. **So this is deleting fog/mist particles, not walls** — worth knowing, not the reported
symptom. The other twelve tagged hashes appear **zero** times in any Demon's Souls run.

Also standing, from round 50 and unchanged: `RPCS3_REMIX_TEXBUDGET=0` in `bin\BLUS30443.conf` is a
**no-op**, not "unlimited" — only the *config* value 0 reaches the unlimited branch.

## What is owed — one play session, on Demon's Souls

1. Launch Demon's Souls on `bin\rpcs3-next.exe`. First check the run-start banner reads
   `title=BLUS30443 vtxnormal=1 normalattr=2 demonsmenu=1 demonsworld=1` — `demonsmenu`/`demonsworld`
   are echoed there for the first time this round; until now the only place they could be read was
   `bin\BLUS30443.conf`, so "the profile did not load" and "the profile loaded and did not work" were
   indistinguishable. Also confirm `pos_input=0/0` — that is round 50's outstanding regression check.
2. **Normals.** Look at a character. Then read `normals=` on `Remix live:`. `applied` climbing is the
   fix firing; `rejected` climbing with `applied` at 0 says ATTR2 is not the slot, and the
   `Remix vtxattr:` lines say which slot is (whichever column reads ~1.00) — set
   `RPCS3_REMIX_NORMALATTR` to it. `RPCS3_REMIX_VTXNORMAL=0` is the A/B.
3. **Walls.** Stand where geometry is missing, wait for two `Remix live:` lines, then stand somewhere
   complete and wait for two more. Difference each pair. The `skip` bucket whose per-frame rate is
   higher at the broken spot is the gate that is removing the wall. `screen=` would mean world
   geometry is being classified as UI; `albedo=` means one of the six texture gates; `rt=` means the
   render-target gate; `layout=`/`mem=`/`decode=` mean the vertex stream itself.

---

# Addendum, 2026-09-07 08:17 — measured on a live Demon's Souls run (pid 10512)

Run banner: `title=BLUS30443 vtxnormal=1 normalattr=2 demonsmenu=1 demonsworld=1 posinput=1`,
`Remix game-config: BLUS30443 applied=7`. Counters below are the delta between two consecutive
`Remix live:` lines, 61 flips apart, with the inventory screen open.

## 1. Normals: CONFIRMED CORRECT ON DEMON'S SOULS

    normals=17/255493/0/417591/0        (programs/applied/rejected/absent/degenerate)

**17 programs, 255,493 draws applied, ZERO rejected.** `Remix vtxattr:` says why, on every one of the
17 world programs:

    a0=f3     (position)            a1=s14  len=1/1/1 or 0.7071  (blend weights)
    a2=ub4    blen=0.995 .. 1.001   <-- THE NORMAL, UNORM8-packed, biased
    a3=ub4    len=1.732             (white vertex colour)
    a7=ub2564 len=0                 (bone indices)
    a8=s32k   (texcoords)

`blen` is the re-biased reading (`2x-1`), and it lands on 1.00 to within a byte of quantisation on
every program. ATTR2 is the normal, the default `NORMALATTR=2` is right for this title, and the
per-draw unit-length gate never had to fire. Nothing more to do here.

## 2. The missing floor is NOT geometry being dropped — three independent measurements

- **Nothing is refused.** `world_refused=112` and it has not moved since boot; every one of the 16
  `Remix world-refused:` lines in the run is `fail=nocam` at frame 120. `wext_refused=0`.
- **Nothing is skipped.** The drop partition now on the live line reads
  `layout=0 mem=0 decode=0 poison=0 vp=0 albedo=0 prim=0 restart=0 instanced=0 camsurf=0 notinput=0
  posdecode_refused=0`. The only non-zero buckets are `screen` and `rt`/`rt_kept`.
- **Nothing world-shaped is being misrouted to 2D.** `Remix ui-route:` names exactly **two** programs
  across the whole run — `f2577d351159c828` (273 lines) and `fc7a78150c4515d7` (49) — both UI, both
  `reason=demonsmenu`.

**What the scene actually has is one light and nothing else:**

    guest_lights=0  guest_lights_created=0  mat_suncard=0  skyclassify_dome=0
    Remix sun-submit: travel=[-0.60402 -0.34202 0.71985] radiance=3 angle=0.5 aimed=1 draw=SUCCESS

with `rtx.fallbackLightType = 0`, `rtx.fallbackLightMode = 0` and `rtx.pathMaxBounces = 2` in the
Remix confs. So the entire game is lit by a single synthesised directional sun, and anything that sun
cannot reach gets only two bounces of indirect. A straight-edged region of near-black floor beside a
lit region is what that produces — and the rubble that IS faintly visible inside the dark region is
the tell that light reaches the volume at all.

**The next milestone for this title is guest-light extraction, not geometry.** `GUESTLIGHTVP`/`FP`
were configured for Haze and were never set up for Demon's Souls; `bin\BLUS30443.conf` has 7 keys and
none of them is a light.

**Still owed — the A/B that separates "unlit" from "absent", because both were inferred here:**
`RPCS3_REMIX_VTXNORMAL=0` for one run. If the dark half lights up, the normal decode is aiming the
floor away from the sun and this is a round-51 regression. If it does not change, the floor is present
and unlit and the finding above stands.

## 3. The messed-up UI has a concrete root cause: the inventory program's UVs collapse to one texel

Two programs draw the menu, and only one of them is broken:

| vp | texcoord attribute | submitted UV span (`Remix uiwrap:`) | on screen |
| --- | --- | --- | --- |
| `f2577d351159c828` | `a8=s32k2` (SINT16 x2) | `u=[0.00003..0.00005] v=[0.00021..0.00023]` | flat grey boxes |
| `fc7a78150c4515d7` | `a8=f2` (float x2) | `u=[0.31348..0.36816] v=[-0.00098..0.04980]` | draws correctly |

A span of 3e-5 on a 1024x1024 atlas is **a single texel**, so every item icon is filled with one
flat colour — exactly the grey blocks in the screenshot. The program that works is the one whose
texcoords are floats; the broken one is the one whose texcoords are 16-bit integers and therefore
depend on a divisor the matcher has to recover. `uv_scale_refuse=66845/0/0/0/0` — 66,845 draws fell
back to the fixed 1/4096 divisor through the first exit — and where the scalar pass DID resolve a
slot on this title it read `c467 = [3, 0.498047, 2.00784, 0]`, i.e. real divisors near **0.5 and 2**,
not 4096. `f2577d351159c828` does not appear in the `Remix uvscale-fixed:` census at all, so its
divisor is not merely wrong, it is never looked for.

**Next step for the UI:** replay `f2577d351159c828`'s stored ucode from `bin\remix_ucode\` and find
what actually scales its texcoord output, the same way round 50 found the position input. The A/B in
the meantime is `RPCS3_REMIX_DEMONSMENU=0`.

---

# Addendum 2, 2026-09-07 — the ucode replay, and why the inventory icons are flat

A disassembler for the RSX vertex ISA is at
`%TEMP%\claude\...\scratchpad\vpdis.py` — bitfields taken verbatim from
`rpcs3\Emu\RSX\Program\RSXVertexProgram.h`, dword order and operand slots from
`VertexProgramDecompiler.cpp:563-566` and `:599-622`. **The trap it cost an iteration to find: `ADD`
reads `$0` and `$2`, not `$0` and `$1`.** Decode ADD with src1 and every transform chain reads as
nonsense (`HPOS <- c[467].y + v0` instead of `HPOS <- c[467].y + r0`).

## The two menu programs, replayed

    F2577D351159C828                                    FC7A78150C4515D7
    0: MOV COL0    <- v3                                0: MOV COL0    <- v3
    1: MUL TEX0.xy <- v8(tc0).xy, c[467].zzzz           1: MUL r0      <- v0.yyyy, c[1]
    2: MUL r0.xy   <- v0(pos).xy, c[467].xxxx           2: ADD TEX0.xy <- c[467].yyyy, v8(tc0).xy
    3: MUL r1      <- r0.yyyy, c[1]                     3: MAD r0      <- v0.xxxx, c[0], r0
    4: MAD r0      <- r0.xxxx, c[0], r1                 4: ADD r0      <- c[3], r0
    5: ADD r0      <- c[3], r0                          5: ADD HPOS    <- c[467].xxxx, r0   END
    6: ADD HPOS    <- c[467].yyyy, r0   END

This matches, line for line, what `composite_ui_draw`'s `is_demons_menu_draw()` block already reads
out of c467 (`RemixGSRender.cpp:15832-15856`): for the packed program `scale=constants[0]`,
`bias=constants[1]`, `demons_uv_scale=constants[2]`; for the other, `scale=1`, `bias=constants[0]`,
`demons_uv_bias=constants[1]`. That block was derived correctly and is not the bug.

**The bug is that round 52 added the GENERAL 2D ucode scale immediately below it**, and for
`f2577d351159c828` both blocks resolve the *same* `c467.z`, so the texcoord is multiplied by it
twice. Three things confirm it rather than one:

- `ui_uv_ucode=351949` on a run whose 2D route is **entirely** `reason=demonsmenu` — the general
  block is demonstrably firing on the same draws the title-specific block already handled.
- `Remix uiwrap:` reports a submitted span of `u=[0.00003..0.00005]` on a 1024x1024 atlas. A correct
  icon span times a small `c467.z` again lands exactly there. One texel, one flat colour per box.
- **The falsifiable half:** the sibling program's texcoord write is an `ADD`, so there is no MUL for
  the scale resolver to find, `ui_uv_ucode_applied` stays false, no second application happens — and
  it is the one that renders correctly. Only the MUL program is broken, which is what a
  double-multiply predicts and what a mis-scale of the whole 2D route would not.

**Fix:** `RPCS3_REMIX_UIUVONCE` (default `1`) — the general block stands down wherever the
title-specific one already applied a scale or bias. `0` restores the round-52 double application
bit-exactly. Counter `ui_uv_double_skipped` (counted per triangle corner, not per draw).

## The ucode also settles the normal question independently

Every Demon's Souls world program replayed so far — `1EFBEBAA`, `E1B62419`, `DF335487`, `B32AA200`,
`C9D9ACB7`, `B3C9DB2B`, 6 of 6 — opens with

    ADD rN.xyz <- -c[467].yyyy, v2(normal).xyzx          then      MUL r4.xyz <- rN.xyzx, c[467].zzzz

i.e. **the guest itself reads ATTR2 as the normal and un-packs it as `(v2 - c467.y) * c467.z`**, then
rotates it by the blended bone rows (`DP3 o[9](TEX2).xyz <- {r3,r0,r1}, r4`). Measured on this title,
`c467 = [3, 0.498047, 2.00784, 0]`, so the guest's own transform is `(v - 0.498047) * 2.00784`, which
is `2v - 1` to three decimals — **exactly the UNORM8 de-bias the round-51 decode applies.** The
`blen=0.995..1.001` statistic said the slot was right; the ucode says the *transform* is right too.

## Filed for the next round: the world texcoord scale has the same shape the position matcher already handles

`uv_scale_fixed=66845`, all through the `no_multiply` exit, and the census names
`unit=6 attr=8 affine=affine:mad-operands`. The replay says why:

    E1B62419E7D43FF2
      5: MOV r2.x        <- c[466].xxxx
      6: MAD o[13](TEX6).xy <- v8(tc0).xyxx, r2.xxxx, c[120].xyxx

**The scale is `c466.x` forwarded through a single `MOV` into a temp**, so the matcher sees a register
where it requires a constant broadcast and refuses. That is precisely the relaxation round 10 already
shipped for the POSITION matcher — `MADACCUM` arm (A), "accept a scale operand forwarded through a
MOV of a constant, resolved with `last_component_writer`, broadcast reads only". The texcoord matcher
never got it. Giving it the same one-hop walk should resolve the world UVs, and there is a working
template in the tree to copy rather than a new mechanism to design.

Also noted, not fixed: the scalar 2D fallback (`RemixGSRender.cpp`, the `else if (!form.resolved)`
branch) reads `value[0]` from the resolved slot regardless of which component the ucode names, while
the affine path carries `uv_affine_form::scale_component[2]`. A program scaling by `c467.z` would
silently get `c467.x` there.

## Deployed and partially verified, 08:40

`bin\rpcs3.exe` = `bin\rpcs3-next.exe` = md5 **`A8CC86BDCA7474D93217D7181D9AE739`**; the previous
binary is preserved as `bin\rpcs3-next-pre-uiuvonce-20260907.exe` (`E09867C254E515B90150D06844913AB9`).
MSBuild exit 0, only the pre-existing C4723 at `RemixGSRender.cpp:14483`.

**What is verified:** the binary runs Demon's Souls with `title=BLUS30443 vtxnormal=1 normalattr=2
demonsmenu=1 demonsworld=1`, the new `ui_uv_double_skipped` counter prints, and the boot-menu UVs now
read as real atlas sub-rects — e.g. `8EA10EE46626F5A4 512x512 u=[0.12695..0.25195]
v=[0.50195..0.68945]`, against `u=[0.00008..0.00009] v=[0.00000..0.00002]` for the same albedo before.

**What is NOT verified, and must not be reported as fixed:** at the boot menu `ui_uv_ucode=0/0` and
`ui_uv_double_skipped=0`, i.e. the general block never fired in that state, so **the guard has not yet
been exercised on the population it targets.** The double application was measured with the INVENTORY
open (`ui_draws=410804`, `ui_uv_ucode=351949`); the run above never reached that state. The UV change
seen at the menu is a change, not a proof.

**To close it:** load a save, open the inventory, and read `ui_uv_double_skipped` on `Remix live:`.
It climbing is the guard firing. `RPCS3_REMIX_UIUVONCE=0` is the A/B and reproduces the flat boxes.

**Cosmetic defect to fix with the next change, not worth a rebuild on its own:** the live line prints
`ui_uv_ucode=0/0ui_uv_double_skipped=0` — the separating space was lost when the counter was appended
to that format group.

## VERIFIED, 08:45 — correcting the note above

The run that produced the "not exercised" reading above was sampled at the boot menu, ten seconds in.
Its FINAL `Remix live:` line, taken after the run ended (pid 23376, `title=BLUS30443`):

    ui_draws=524246  ui_uv_ucode=382662  ui_uv_double_skipped=2295972

**The guard fired 2,295,972 times.** The three counters are internally coherent, which is what makes
this a measurement rather than a number: `ui_uv_double_skipped` is counted per triangle CORNER, and
382,662 x 2 triangles x 3 corners = **2,295,972 exactly**. So every one of the 382,662 draws on which
the general block resolved a scale is a single quad, and on every one of them the second application
was suppressed.

The submitted UV spans are correspondingly sane, against a uniform ~1e-5 before the fix:

    2352A43742D7440F  512x512    u=[-0.00098..0.99316]  v=[-0.00098..0.10059]   (full-width bar)
    514B01331AE2E059  512x512    u=[ 0.60840..0.65918]  v=[-0.00098..0.04980]   (a ~26-texel glyph)
    89DB21792978F85E  1280x720   u=[ 0.00000..1.00000]  v=[ 0.00000..1.00000]   (full-screen blit)

Three different span scales — full atlas, small sub-rect, exact 0..1 blit — where before every span
was one texel. That is the defect gone, measured on the population it was aimed at.

**What is still not verified is the picture.** No screenshot has been taken since the fix, and the run
never left the menus (`submitted=0` for its whole life — zero world draws), so the in-game inventory
grid has not been looked at. The UV evidence is direct and the counters are consistent; the pixels
are still owed.
