# Round 57 — the texcoord tint route, on the world path

## The colour was never in COL0, and the backend already knew where it was

Demon's Souls' messages lose their red because `apply_vertex_colour` reads COL0 and this program
family does not put the colour there. Read with `docs/remix/fpdis.py` from
`bin/remix_ucode/C0BEEFD11A0A5C97.fp` (vp=`9fac8d0968bbceb8` fp=`5e89966865402082`):

     1: TEX R0     <- f[4](tc0)  [tex0]      sample the glyph
     3: MUL R1     <- R0, f[5](tc1)          MODULATE BY TC1   <- the tint
    11: MUL R0.xyz <- R0, f[6](tc2)
    12: ADD R0.xyz <- f[7](tc3), R0

The tint is a **texcoord interpolator**. `vcolroute` reporting `route=none` was therefore correct and
complete - the vertex program writes COL0 nowhere - and the vertex-side machinery was looking in a
place this program never puts the colour.

**Both hops already existed and were already being computed:**

    fp_fingerprint::out_tint_texcoord   the TEXn the fragment program modulates by  (round 52)
    vp_fingerprint::texcoord_input[n]   the attribute the vertex program writes into it

and `composite_ui_draw()` has consumed that exact pair since round 52. **Only the WORLD-path
consumer was missing.** A world draw of the same shape reached `apply_vertex_colour`, found
`vcol_route::none`, and returned - submitting white.

That is the third time this session a needed value already existed and was being discarded:
`audit_world_extent` computing the world AABB and keeping only the span, the fragment ucode already
on disk under its raw-hash filename, and now this. **Before adding a mechanism, check whether the
codebase already computes what it needs and simply does not read it on this path.**

## What shipped

A world-side consumer of the same two fields, placed ahead of round 55's alpha-only path because it
is strictly better where it applies - it recovers the whole RGBA the guest computes, not just the
alpha. Knob `RPCS3_REMIX_WORLDTINT` (default 1), deliberately separate from `UITINTTEXCOORD` so the
world path can be bisected without disturbing the 2D one. `UITINTTEXCOORD=0` still gates whether the
field is POPULATED at all, so it disables both and this knob then has nothing to read.

Counters `vcol_tint=<applied>/<unmapped>` on `Remix live:`. `unmapped` is the refuse-and-count twin,
covering two cases: the fragment program named a varying the vertex table has no attribute for
(`resolve_output_input` refused that write), and a decode that failed part way - in which case every
vertex is put back to white, because half a tinted mesh is worse than none of it.

A 3-component varying leaves alpha at the decoder's 1.0, which is the opaque default the draw
already had, so a texcoord carrying only RGB cannot make anything vanish.

## Owed

1. `vcol_tint=` climbing on a Demon's Souls run with a message on screen, and the message reading
   red. `RPCS3_REMIX_WORLDTINT=0` is the A/B.
2. `vcol_tint_unmapped` climbing instead would mean the fp names a texcoord the vp table has no
   attribute for - a different, nameable problem rather than a silent white.
3. This route may also catch other world draws that were arriving white. That is the point, but it
   is a wider blast radius than the messages alone, and the counter is what measures it.
