# Round 53 — the texcoord MOV walk (shipped), and the Demon's Souls fix list

**Deployed:** `bin\rpcs3.exe` build 09-08 10:51 — **contains the texcoord MOV walk**, verified by
`grep -a "uvscalemov=%d" bin\rpcs3.exe`. That binary was linked by a *concurrent session's* build
that picked my source changes up out of the shared tree; I did not build it myself, so its exact
warning count is not mine to report.

**Shared-tree warning:** at 10:46 another session had **27 MSBuild processes** running in this same
checkout and had added its own `ROUND 53` and `ROUND 54` markers to `RemixGSRender.cpp` /
`RemixTransforms.cpp`. My round label collides with theirs. Do not assume "round 53" in the source
means this file.

## 1. The texcoord MOV walk — done, needs one measurement

`resolve_texcoord_scale_slot` refused `no_multiply` whenever the scale reached the multiply through
a register instead of being a constant operand. Demon's Souls writes **every** world texcoord that
way:

    5: MOV r2.x            <- c[466].xxxx
    6: MAD o[13](TEX6).xy  <- v8(tc0).xyxx, r2.xxxx, c[120].xyxx

so `uv_scale_fixed=66845` — all of it through that one exit — and every world UV took the fixed
1/4096 divisor. The walk is round 10's `MADACCUM` arm (A) applied to texcoords, with the same
guards: `last_component_writer` for the definition live at that point, then a `MOV` of a
non-indexed constant, unnegated, no abs, no saturate, unconditional, read through a broadcast
swizzle. Ambiguity across the slice refuses rather than guesses. Knob `RPCS3_REMIX_UVSCALEMOV`
(default 1), echoed on the banner.

**A second defect found while wiring it:** every reader of the scalar slot took `value[0]`
unconditionally, which is correct only when the ucode says `c[N].x`. The resolved **component** now
travels on `vp_fingerprint::texcoord_scale_component[8]` and both the 3D and the 2D reader honour
it; the census prints `slot=cN.c`. Defaults to 0, so anything resolved before this is unchanged.

**Owed:** one Demon's Souls run. `uv_scale_fixed` should collapse from ~66k toward 0 with
`uv_scale_ucode` taking it, and `Remix uvscale-fixed:` should stop naming `unit=6 attr=8`.
`RPCS3_REMIX_UVSCALEMOV=0` is the A/B.

## 2. The authored lighting is extracted — `docs/remix/demons/`

The disc ships **unpacked**, so the DrawParam banks read directly. `des_lights.json` now holds every
level's 64 lighting presets: 3 directional lights, hemisphere ambient, the scattering sky (sun angle,
colour, Rayleigh/Mie), and fog. The angle→vector convention is **verified** — `m08` row 0's
`sunRotX=20, sunRotY=-40` reproduces the live `Remix sun-submit: travel=[-0.60402 -0.34202 0.71985]`
to five decimal places, which also identifies that run as an `m08` map.

## The fix list, worst-supported-by-evidence last

1. **No ambient at all.** `guest_lights=0`, one synthesised sun, and `rtx.conf` sets
   `rtx.fallbackLightMode = 0` = `Never`. The authored data wants a 5% hemisphere on that level.
   **This is the "missing floor".** Cheapest test needs no build: `fallbackLightMode = 2` (Always)
   with `fallbackLightRadiance` at the authored hemisphere colour.
2. **Sun colour and intensity are invented.** The backend submits `radiance=3` flat; the authored
   value for that level is a specific RGB at 200%. Direction is already right.
3. **The other two directional lights are never submitted.** Every preset authors three.
4. **`POINT_LIGHT_BANK` is never read.** Authored local lights with real radii, and
   `guest_lights_created=0` says none reach the scene.
5. **The sky is not the authored sky.** `LIGHT_SCATTERING_BANK` carries Rayleigh/Mie/HG and a sun
   colour — the aerial-perspective runtime can consume that directly instead of `SKYEMISSIVE`.
6. **Fog is unused.** `FOG_BANK` has begin/end/colour per preset.
7. **Preset selection is unsolved.** 64 rows per level and no runtime hook yet. Matching the
   recovered sun direction against the table is self-validating and needs no guest hooking — it
   already picks m08 row 0 exactly.
8. **`rtx.hideInstanceTextures` deletes Demon's Souls fog particles** (`E5C6A3BF712D50CC`, 350
   occurrences, the particle program at `blend=1 depth_write=0`). Filed round 51, still set.
9. **`RPCS3_REMIX_TEXBUDGET=0` in `bin\BLUS30443.conf` is a no-op**, not "unlimited". Round 50.
10. **The scalar 2D fallback still can't express a per-lane scale**, only the affine path can — a
    program scaling u and v differently through the scalar route silently gets one of them.
11. **`ui_uv_double_skipped` counts triangle corners, not draws** — divide by 6 for quads when
    reading it.

**None of items 1-6 needs new plumbing to *test*** — 1 is an `rtx.conf` edit. They need plumbing to
*ship*, which is where the next build should go.

## The exact level the live run was in, fully decoded — `m08` preset row 0 (`B0基本`)

    LIGHT_BANK dir0   travel=[-0.66341, -0.5,     0.55667]  rgb=[1,1,1]        100%   ->  (30, -50)
    LIGHT_BANK dir1/2 0%  (this preset authors one directional light, not three)
    hemisphere        up=[0.05,0.05,0.05]  down=[0.05,0.05,0.05]   envDif=100%
    SCATTERING sun    travel=[-0.60402, -0.34202, 0.71985]  rgb=[1.41,1.96,1.73] 200%  ->  (20, -40)
    FOG               0% (off on this preset)

**The backend is lighting the scene with the SKY's sun, not the shading key light.** Those are two
different directions in the authored data — `(20, -40)` for the scattering sun against `(30, -50)`
for `LIGHT_BANK dir0` — and `Remix sun-submit:` reports the scattering one to five decimals. So
every shadow in the raytraced scene is cast from a direction the game intends for the *sky*, about
10 degrees off in both pitch and yaw from the light the game actually shades with. That is a
concrete, named defect and it belongs at the top of item 2.

The hemisphere ambient is **5% of the key light**. With the sun submitted at `radiance=3`, the
equivalent fill is roughly `0.15`, which is the starting value for the no-build test:

    # bin\rtx.conf  --  item 1, the ambient test. SHARED ACROSS EVERY TITLE IN THIS bin\,
    # so do not leave it set while another session is measuring Eat Lead or Haze.
    rtx.fallbackLightMode = 2          # Always (was 0 = Never)
    rtx.fallbackLightType = 0          # Distant
    rtx.fallbackLightRadiance = 0.15, 0.15, 0.15
    rtx.fallbackLightDirection = 0, -1, 0

That is one distant light standing in for a hemisphere, which is not what the game authors — but it
answers the only question that matters first: **is the dark floor unlit, or absent?** If it lights
up, every remaining item is lighting work and no geometry work is needed at all.
