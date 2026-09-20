# Round 56 — it was never ground, and it was never missing. It is water.

**Deployed** `bin\rpcs3.exe` = `bin\rpcs3-next.exe` = md5 **`41D8A7FA7E6C590B162349B92BEEC4DF`**.
Build succeeded, 0 errors.

## A fragment disassembler, and what it said

`docs/remix/fpdis.py` is the sibling of `vpdis.py` and reads the `.fp` files `RPCS3_REMIX_UCODESTOREFP`
already writes. On `613195379C52B18B.fp`, the fragment program of the black plane:

    0: TEX   H2.xy <- f[11](tc7)  [tex2]
    1: MAD   H2.xy <- H2, c[2 -1 0 0].xxxx, c.zzzz     <- 2x-1, a normal-map unpack
    3: TEX   H4.xy <- f[12](tc8)  [tex2]               <- same texture, second texcoord
    5: MAD   H4.xy <- H4, c[2 -1 0 0]                  <- unpacked again
    7: ADD   R2.w  <- -H1.z, c[1 0 0 0].x              <- 1 - dot(n.xy, n.xy)
   13: DIVSQ H2.z  <- R2.w, R2.w                       <- sqrt, rebuilding z

**Two scrolling normal maps, z reconstructed, and no albedo at all: that is water.** The level's own
DrawParam preset names agree - m08 authors rows called 水面 (water surface) and 水路 (waterway).

### The disassembler's own trap, recorded because it produces a wrong answer silently

Fragment ucode is NOT stored the way vertex ucode is. The words read **little-endian** and each one
then needs its bytes swapped **within each 16-bit half** (`OPDEST::from_be32`) before any bitfield
means anything. The wrong combination yields real opcodes, real-looking registers and a completely
different program, with no error. The right one was found by scoring the four candidates on
"contains exactly ONE end-of-program bit" - the first ordering tried produced a plausible
single-instruction program and would have been believed.

## The gate that never fired

The backend already knew this pair was water:

    const bool demons_water_surface = demons_world_enabled()
        && vp == 0x0314c853c971ceae && fp == 0x613195379c52b18b     // exactly the pair
        && surface_color_target() == 1;                             // never matched

**Neither log contains the "captured water surface" notice nor its failure twin**, so
`get_demons_water_material()` was never called once. The draw reached Remix with `material=nullptr`,
no albedo and no colour route, and rendered as an opaque black sheet lying over the courtyard.

The program pair is the identity; the colour target was a guess that happened to be wrong. It is now
a knob (`RPCS3_REMIX_DEMONSWATERANYTARGET=0` restores it) rather than deleted, because the term was
presumably added to separate two uses of one program and this evidence cannot prove there is only
one - and the observed target is **logged once** (`Remix Demons water: target=..`), so the next run
states what it actually is instead of inviting a third guess.

## What this costs in credibility, stated plainly

Rounds 51, 54 and 55 hunted "missing ground" through the drop partition, the world-box census and the
vertex-alpha path. **The geometry was present the whole time and was not ground.** The instruments
were not wasted - each eliminated a real class of cause, and the world-box census is what narrowed it
to "present but wrong material" - but the framing came from the report's wording and was never
challenged. **A symptom's NAME is a hypothesis.** "Missing ground" asserted absence, location and
identity; all three were wrong, and one `.fp` read settled it in a minute once the tool existed.

## Owed

1. Does the water render? `Remix Demons water: target=` names the real target, and the
   "captured water surface uses refractive material" notice should now appear.
2. Whether a refractive material reads correctly in a path tracer is unmeasured.
3. Round 55's vertex-alpha path is EXCLUDED from this draw by the `route=none` guard added after it.
   It may still help the particle and lock-on cards; unverified.
4. The message's red: `5E89966865402082.fp` is still not in the ucode store, so the fragment
   disassembler cannot be pointed at it yet. That is now a capture problem, not a tooling one.

## The message's red: it travels through TEXCOORDS, not COL0

**No capture was needed — the program was already on disk under a different name.** The ucode store
writes `%016llX.fp` using the RAW fp hash, while every census prints `m_current_fp_hash`, which is
the raw hash XORed with `0x9e3779b97f4a7c15` when the program uses 32-bit exports. So the file the
censuses call `5E89966865402082` is stored as **`C0BEEFD11A0A5C97.fp`**. I had concluded it was
"not captured" purely from a filename lookup, and armed `RPCS3_REMIX_UCODESTOREFPHASH` for nothing;
that conf line has been removed again.

Read with `docs/remix/fpdis.py`:

     0: MOV R2.zw   <- f[4](tc0)
     1: TEX R0      <- f[4](tc0)  [tex0]        <- sample the glyph
     3: MUL R1      <- R0, f[5](tc1)            <- MODULATE BY TC1
     4: MOV R0.xyz  <- R1
    11: MUL R0.xyz  <- R0, f[6](tc2)            <- and by TC2
    12: ADD R0.xyz  <- f[7](tc3), R0            <- plus TC3
    13: MUL R0.xyz  <- R0, c[0 2 0 1].xxxx
    15: DIV R0.xyz  <- R0, c.yyyy  [sat]   END

**The tint is `f[5](tc1)`, a TEXCOORD register — the vertex program carries the colour through the
texcoord interpolators instead of COL0.** That is why `vcolroute` reads `route=none`: there is no
COL0 write to classify, and the vertex-side machinery is looking in a place the colour was never
put. The `route=none` reading was correct and complete; it simply is not the whole picture for this
program family.

`apply_vertex_colour` therefore has nothing to apply, the draw reaches Remix white, and the message
loses its red.

**The fix has a clear shape and is NOT implemented.** The fingerprint already resolves which input
attribute feeds each texcoord output (`resolve_output_input(prog, 7 + unit, ...)` filling
`texcoord_input[unit]`), so the missing piece is a fragment-side classification: "the sampled texture
is modulated by texcoord register k", from which the vertex colour is `texcoord_input[k]`'s
attribute. That is a new `fp_out_source` case plus a route in `apply_vertex_colour`, and it wants
its own knob, counter and A/B rather than being bolted on at the end of this round.

**Second filename lesson in two rounds:** the fragment ucode's byte order was a silent wrong answer,
and its FILENAME was a silent absence. Both times the artefact was right there and the lookup was
wrong. Before concluding something was not captured, check what the writer actually names it.
