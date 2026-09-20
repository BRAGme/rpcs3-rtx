# Round 55 — the alpha was being thrown away with the colour

**Deployed** `bin\rpcs3.exe` = `bin\rpcs3-next.exe` = md5 **`D84CBD3CD2CD33901B05218B93B3A50E`**.
Build succeeded, 0 errors. Rollback: `bin\rpcs3-next-pre-demonslights-20260908.exe` (round 53).

## The census found the "missing ground", and it was never missing

`Remix worldbox:` (added this round, publishing the AABB `audit_world_extent` has always computed
and discarded) returned 23 programs on a run through the area. **Every box was sane** — nothing
misplaced, distances 3 to 160 units. One line was the answer:

    vp=0314c853c971ceae albedo=00000000 vtx=52
    lo=[-77.67, -0.052, -2.67]   hi=[6.13, -0.052, 47.12]

Y is CONSTANT: a perfectly flat horizontal plane, 84 x 50 units. Its other censuses complete it:

    sky-census: raw=[-76.2 -0.4 -92.3]..[-26.4 -0.4 -8.5]  depth_write=0  albedo=0000000000000000
    vcolroute:  route=none  alpha_from_attr=0  slots=[-;-;-;-]
    kil:        blend=1  depth_write=0  alpha_range=255..0  alpha_test=1

A blended, un-textured, un-depth-writing flat plane carrying a vertex alpha gradient of **255..0** —
a raster ground fade. It reached the path tracer opaque and became a black slab lying over the
courtyard floor. **The ground was never absent; it was covered.** The drop partition was right to
report zero: nothing was being removed.

## Root cause: one early return

`apply_vertex_colour` opens with

    if (!vcol_route_replayable()) { ++m_stats.vcol_route_blocked; return; }

`vcol_route_replayable()` demands the FRAGMENT program prove where ATTR3's **RGB** reaches COL0.
That test is strict for a good reason — replaying a colour the guest never computes repaints
geometry. But on failure the function returns **before reaching the alpha lane it already treats as
a separate question** twenty lines lower, so the vertex keeps `0xFFFFFFFF` and every blended draw
whose colour route is unproven arrives **fully opaque**. `vcol_route_blocked` was reading 93,117 on
an earlier run: that is the size of the population this was flattening.

It explains three separate reports with one mechanism:

| symptom | draw | alpha |
| --- | --- | --- |
| black slab over the floor | `0314c853c971ceae` | 255..0 |
| particle cards | `7f4d3587c70d02da` | 255..255 |
| lock-on card | `9fac8d0968bbceb8` | 0..251 |

## The fix, and why it is safe

A blended draw's alpha is consumed by the **blend equation**, not by the colour output, so the
fragment program does not have to prove anything about `COL0.rgb` for the alpha to be correct.
**The RGB gate is not relaxed.** Only the alpha is submitted, only where blending consumes it, under
four guards that make it structurally impossible for this path to delete geometry:

* blending must be enabled, or the alpha is decoration;
* ATTR3 must carry four components, so an alpha lane exists;
* the alpha must **vary** across the draw — a constant is either already opaque or is not being used
  as coverage, and replaying it could only remove geometry for no gain;
* the maximum must be non-zero, so no draw can be erased.

RGB is left white and only bits 24-31 are written, in the packing the main path already uses.
Counter `vcol_alpha_only` (a strict subset of `vcol_route_blocked`) on both stat lines; knob
`RPCS3_REMIX_VCOLALPHAONLY=0` restores the all-or-nothing behaviour bit-exactly.

## Also this round

`Remix worldbox:` — the instrument that found it. Note `cam=[0 0 0]` on every line, so either the
submitted world is camera-relative or `m_active_camera.position` is never filled; the boxes are
self-consistent either way, but the `dist=` column is only meaningful once that is settled.

`RPCS3_REMIX_DEMONSHEMI` (default 1) splits the hemisphere fill from the authored directional
lights, so "did the fill reveal the cards" is one variable. **That question is now largely moot** —
the cards were opaque because of the alpha bug, not because the fill revealed them — but the knob is
worth keeping for the frame-rate question, which is still unmeasured.

## Owed

1. Does the floor appear? `vcol_alpha_only` climbing is the fix firing.
2. A raster alpha-fade plane is a screen-space trick. Blended correctly it may read as a translucent
   sheet rather than vanishing into the lighting. **If it does, the honest answer is that the plane
   should not be submitted at all** — but that is a judgement to make after seeing it blended
   correctly, not before, and it is not a reason to hide the draw now.
3. The message's red: `albedo=80512CAEF0C276F2` is in `bin\rtx.conf` **twice**, in both
   `rtx.worldSpaceUiTextures` and `rtx.uiTextures`. Those are contradictory tags and the world-space
   UI path is flat/unlit. A conf edit, not a code change - untested.
4. Frames when looking at the sun: still unmeasured, three candidates, `DEMONSHEMI=0` and
   `DEMONSLIGHTS=0` are the bisect.
