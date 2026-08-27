# Round 43 inbox — RPCS3 RTX Remix backend, Haze (BLUS30094)

Written at the close of round 42, 2026-08-25. Branch `remix-backend`, HEAD **`f091ff5a4`**, working
tree carries rounds 37-42 (39-41 are committed; round 42's four source edits and the launcher are not).

| artefact | SHA256 (first 16) | note |
| --- | --- | --- |
| `bin\rpcs3.exe` | `DCCF0459B83778AE` | built this round, MSBuild `Release\|x64` exit 0, **0 errors, 1 warning** |
| `bin\rpcs3-next.exe` | `DCCF0459B83778AE` | **identical — the copy was made** |
| `bin\remix\d3d9.dll` | `16A0B512F33EBB66` | **UNCHANGED.** Nothing deployed into `bin\remix\`. |

The one warning is the pre-existing C4723 "potential divide by 0", now at `RemixGSRender.cpp:11389`
(round 41 recorded 11339; this round's edits moved it). **Zero new warnings.** **The tree did not
move**: HEAD `f091ff5a4` at both ends, both exes read `49D31EEA1D767004` at the start exactly as round
41 recorded. The exe contains no `expected<enum rsx::primitive_type` marker, so it is not the poisoned
`primitive_mode()` build.

Full derivations in `docs\remix\KNOBS.md` "Round 42".

---

## PLAY-TEST CARD — one change, two knobs, both one-line reverts

### The white walls: the modulate was thirty instructions upstream

**What was wrong, MEASURED by disassembling the raw ucode in `bin\remix_ucode\`.** Haze's fragment
programs **do** modulate the albedo by the vertex colour. The classifier could not see it because it
only ever read the program's **last** instruction, and the modulate sits thirty to forty-five
instructions earlier, behind the whole per-pixel lighting composite. Two of the four programs read are
ones **your own Ctrl+Clicks landed on walls** (`aa0fe222771ff5c0`, `65a91390aaf6bef3`):

```
 14: MOV  H1,     ATTR1                    ; COL0
 15: MUL  R1.xyz, H1, {2,0,0,0}.xxxx       ; the 0..2 lighting expansion
 26: TEX  R0,     ATTR5, tex0              ; the albedo
 27: MUL  R1,     R0, R1                   ; *** albedo x (COL0 x 2) ***
 ... 30 more instructions ...
 57: MUL  R0.xyz, R0, {0.999001,...}.xxxx  END   <- all the classifier ever saw
```

**What is armed now:** `RPCS3_REMIX_FPVCOLDEEP=1` — search the whole program for the modulate, not
just its last line. Four terms, all required: an unconditional `MUL` writing rgb; one operand a temp
whose writer **sampled a texture**; the other a clean COL0 read or a temp reached from one through
nothing but **broadcast** constant scales; and the product **provably live into COL0.rgb**. COL1 is
excluded because these same programs carry the packed **normal** in ATTR2.

Plus `RPCS3_REMIX_FPVCOLDEEPSCALE=1` — fold the measured x2 into the replayed vertex colour's rgb
(never alpha; the ucode copies the alpha lane unscaled).

**Pre-registered offline BEFORE the build ran:** the rule was reimplemented in Python and run over
every stored fragment program. **22 of 338 match, 316 do not**, all at scale 2.0 or 1.0 — and of the
**8** programs on the last run's own `Remix fpother:` census that have ucode on disk, **7 match**; the
8th is a one-instruction program that samples nothing and correctly cannot.

| reading on `Remix live:` | meaning |
| --- | --- |
| `fpdeep=P/D`, P ~20-40 and D in the millions | **PASS** — P programs named, D draws reached |
| `fpdeep=0/0` with `fpvcoldeep=1` on the banner | the search ran and matched nothing — not the same as never running |
| `fpdeep=P/0`, P > 0 | classifies but never reaches a textured draw; check `vcol_route_blocked` |
| `fpclass=a/b/c`, b must jump from 1 | b is the modulate program count |
| `vcol_mod=` — was **10,280** of 5,392,256 submitted (0.19%) | the number that has to move by orders of magnitude |
| `mesh_created=` — was **402,758** | expect ONE bounded re-creation of every world mesh. Unbounded climb = an animated vertex colour minting a mesh per step |

**PRE-REGISTERED REFUTATION: darker is the fix, wrong hue is the failure.** If surfaces come out
visibly wrong-**coloured** rather than merely darker, this is round 8's `RETRYUNSUP` repeating.
Revert: `set "RPCS3_REMIX_FPVCOLDEEP=0"` — one line, restores round 41 bit-exactly.
**MIND THE DIRECTION on the second knob.** The submitted factor is `min(1, COL0 x 2)` at
`FPVCOLDEEPSCALE=1` and plain `COL0` at `=0`, and `COL0 <= 1` — so **1 is the BRIGHTER setting, 0 is
the DARKER one**, and both can only darken relative to today's no-modulate factor of 1.0. If surfaces
are **still white / washed out**, or show flat white patches where the x2 clips, that is what
`set "RPCS3_REMIX_FPVCOLDEEPSCALE=0"` is for. If they are uniformly **too dark**, there is no
brighter setting — the ceiling is the 8-bit unorm Modulate factor — so report it and revert
`FPVCOLDEEP`.

### Also shipped, under the same knob: `apply_vertex_alpha` no longer forces RGB to white

It wrote `0x00FFFFFF | (alpha << 24)` and ran *after* `apply_vertex_colour` as an independent `if`, so
it erased a freshly replayed vertex colour on any textured, constant-alpha-albedo blended draw. Now
`(color & 0x00FFFFFF) | (alpha << 24)` — **under `FPVCOLDEEP`**, so that `FPVCOLDEEP=0` stays a
bit-exact round-41 control. (It shipped ungated in a draft, on the claim that the mask was a strict
identity; review showed it is not, because `apply_vertex_colour` also writes for the two programs the
*terminal* classifier already named. A revert knob that does not revert is worse than the bug it
hides.) This is round 41's inbox priority 4, closed.

### Unchanged and deliberately so

`RPCS3_REMIX_GUESTLIGHTAUTO=0` left exactly as you set it. Nothing else in the launcher was touched.

---

## THE BRIEF FOR ROUND 42 WAS BUILT ON A NUMBER THAT DOES NOT MEAN WHAT IT SAYS

**Read this before writing another round on `tex_none`.** The brief stated *"37.6% of textured-draw
attempts resolve to NO ALBEDO ... over a third of the world is submitted with no albedo"*. The ratio
is right. The conclusion is wrong, and the backend's own counters say so exactly.

**MEASURED**, `bin\log\RPCS3.log`, last run, 41,802 flips, 5,392,256 submitted draws:

```
tex_none = 3,693,131
  notex_skip        3,605,397   97.62%   refused by a named skip gate  -> NOT SUBMITTED
  notex_world          85,060    2.30%   refused by the world gate     -> NOT SUBMITTED
  notex_mat_applied     2,674    0.07%   reached Remix, took the grey
                    ---------
                    3,693,131            EXACT
```

All thirteen `report_skip_census(...)` call sites are each immediately followed by a `return` —
verified line by line. **2,674 of 5,392,256 submitted draws carry the neutral material. 0.0496%, not
37.6%.**

And the walk is not failing either. A second exact partition:

```
tex_no_unit        1,527,047  41.35%   the fragment program references no 2D unit at all
tex_retry_refused  2,166,084  58.65%   every eligible unit is the 2048x2048 DEPTH16 shadow map
tex_none_shadowonly = 2,166,084  (identical to tex_retry_refused)
tex_none_colorunit  = 0
```

**`tex_none_colorunit = 0`** — not one draw in the run had a colour texture on a sampled unit the walk
failed to reach. `WALKSAMPLED` is armed and working (`tex_walk_sampled = 77,883`). There is no albedo
to find for that population; those draws are shadow projections and lighting composites, and they are
already not submitted.

Your `524D584E4F544558` ("RMXNOTEX") pick is consistent with this. **Eighteen of nineteen
`Remix picked:` lines read `material=1 albedo_unit=0`.** The one exception is
`vp=fc0fac8afccec49a fp=938cfb957fcb7db7 sampled=0x0` — a fragment program that samples **nothing**,
i.e. genuinely vertex-coloured geometry, and the grey is the right placeholder for it.

---

## PRIORITY 1 FOR ROUND 43 — THE ADS WHITE BLOOM. ROOT-CAUSED, NOT FIXED

This is now the largest measured defect in the project and it has a mechanism. **MEASURED** from 468
`Remix timing:` windows (that line goes to `rsx_log.notice` only, so it is in `bin\log\RPCS3.log`,
**not** in `remix_dump.log` — that is why nobody has read it):

| per frame | normal (n=353) | the episode (n=22) | ratio |
| --- | --- | --- | --- |
| frame_ms | 21.42 (46.7 fps) | **81.83 (12.2 fps)** | 3.8x |
| **ui** | **2.26** | **64.88** | **28.7x** |
| **present** | **6.39** | **0.65** | **0.10x** |
| ui_px | 138,477 | **4,480,204** | 32.4x |

**`present` FALLS by 10x. The path tracer is starved, not overloaded.** The whole cost is CPU-side in
the backend's own software UI compositor: `ui_ms = 14.57 ns x ui_px + 0.207 ms, R2 = 0.99825` over 380
windows. 4.48M px/frame is **2.16 full-screen CPU-rasterized layers every frame**, from a fixed,
deterministic **24-draw** overlay set (`ui_pixel` goes 1.0 -> 24.0 draws/frame; `ui_uv` is stable to
+/-30 across every slow window). Correlation with frame time: `ui r=+0.896`, everything else <= +0.52.

**And the visual half is the same defect.** `compositor::blend()` in `RemixCompositor.cpp` implements
**straight alpha only** — there is no additive path in the file at all, and `alpha == 0xFF` takes a
`std::memcpy` at `:633`, a hard overwrite. An additive lens-flare / glare sprite with opaque white
texels therefore composites as a **solid opaque white disc** instead of a glow. One defect, two
symptoms.

**Ruled out by measurement** (per-window delta 0.0 through the whole episode): `mat_emissive`,
`guest_lights`, `sunsprite` (`1579/0` — every one of 1579 observations rejected offscreen, **zero
accepted**), `suncard_elected`, `fpvcol_emissive`, `fpvcol_additive`. The surface-tracking cap did not
fail open. **It is not a lighting problem.**

**The one thing round 43 must do first:** name the 24 draws. The `Remix uiwrap:` census is **already
re-keyed per stats window** (`RemixGSRender.cpp:13818`) — its 64-line cap is per window, not per
run — so the rows for the slow windows are already on disk. Match the window index to the slow frames
(the episode is one continuous encounter with the camera frozen at `scene=[1770 -31.65 1134]`) and the
albedo hashes fall out. Only then decide between (a) an additive path in the compositor, and (b)
refusing to composite a full-screen additive pass at all.

**Do not start by writing a new instrument.** The instrument exists.

---

## PRIORITY 2 — the one-frame donor lag (props bounce, viewmodel does not track)

Unchanged from round 42's inbox, and still the honest version: round 38's *"needs no new machinery —
only a third branch here"* is **refuted**, because `divide_anchor` is the only carrier of the f64
inverse and `camera_state::reference_inverse` is f32 with no f64 twin. Give `camera_state` an f64
reference inverse computed at latch time, then add the branch. Seven assignment sites, RE-GREPPED against CURRENT bytes this round (round 42 edited
`RemixGSRender.cpp` above line 800, so round 41's numbers had ALL shifted, not just some):
`2701, 2772, 3448, 16118, 16363, 16410, 16658` — from `grep -n "\.reference_inverse ="`.
Re-run that grep rather than trusting these; any edit to this file moves them again.

## PRIORITY 3 — instrument defects, cheap, do them while you are in the file

- **`glauto=%d` on both banners prints a BOOLEAN, not the mode.** The argument is
  `guest_light_auto_enabled() ? 1 : 0` (`RemixGSRender.cpp:929` and `:25983`). Round 41 armed
  `GUESTLIGHTAUTO=2` and every log line since reads `glauto=1` — indistinguishable from mode 1, the
  mode that put lights on doors in August. Print `guest_light_auto_mode()`.
- **`RPCS3_REMIX_EMISSIVEINT` is not a real variable** — the parser reads
  `RPCS3_REMIX_EMISSIVEINTENSITY` (`RemixTransforms.cpp:11219`). Nothing is broken (the launcher uses
  the long name), but the short name appears in prose and would silently do nothing.

## THE CHAPTER LIST — carried forward untouched from round 42's inbox

Nothing was done on it this round and nothing was changed under `bin\patches\`. The state of play is
unchanged and round 42's inbox has it in full: the mask gate at `0x4dd830` is **provably not the
limiter**, the PPU patch has **never** been observed to unlock the list without the archive edit, and
the untraced lead is **`0x4f4b40`** — which needs the decrypted ELF regenerating, because
`...\scratchpad\haze\eboot\EBOOT.elf` no longer exists. Do not re-derive any of that; read
`round42-inbox.md` "THE CHAPTER LIST".

---

## Tooling left behind that round 43 should reuse

Both in this session's scratchpad, and both worth keeping — regenerate from
`docs\remix\KNOBS.md` "Round 42" if they are gone:

- **A working RSX fragment-program disassembler** (Python). The bit layout that matters and the trap
  that cost two attempts: the words are read **little-endian** and then 16-bit-half-swapped
  (`fp_decode_word`), and in `OPDEST` the opcode is at **bits 24-29**, not 22-27 — `exp_tex` at bit 21
  and `prec` at 22-23 sit between. Getting either wrong produces a plausible-looking disassembly with
  the wrong opcodes, which is exactly what happened.
- **An offline reimplementation of the deep-modulate rule**, so a widening can be scored against the
  whole `bin\remix_ucode\` corpus before a build exists. That is what turned "this should match the
  wall programs" into "22 of 338, 7 of 8, every match at scale 2.0".

`bin\remix_ucode\` holds **338 `.fp` files and many `.vp`**, named by the **raw** ucode hash — which is
`fp_hash ^ 0x9e3779b97f4a7c15` when the program exports 32-bit registers (`RemixGSRender.cpp:9031`).
Every `Remix fpother:` row prints `fp32=`, so the mapping is mechanical.

## Instrument notes worth keeping

- **A ratio is not a population.** `tex_none / (tex_none + tex_bound)` is a *walk* failure rate;
  what renders is `notex_mat_applied / submitted`. They differ by 757x on this title, and three
  consecutive briefs carried the first as if it were the second. Before building a round on a
  counter, find the counters that partition it and check the partition sums to it exactly. Both
  partitions of `tex_none` sum exactly, which is also how you know the instrument is sound.
- **A matcher that reads one instruction cannot see a program.** When a classifier recognises two
  shapes in a whole title, the question is not "which extra shape" — it is "is it looking in the
  right place at all".
- **Grep the repo's own transcriptions before disassembling.** Round 11 wrote
  `rgb = texRGB * (2 * COL0.rgb) * 0.944243` into a source comment and it sat there for thirty rounds.
- **`bin\remix_dump.log` is ~1 GB and append-mode.** The current run starts at byte **1,031,756,566**;
  find run boundaries with `grep -abo "Remix run-start:"` and slice with `tail -c +N` into a
  scratchpad file once, then grep that. Never grep the whole file.
- **`Remix timing:` and `Remix stats:` go to `bin\log\RPCS3.log` only**, not to `remix_dump.log`.
  `Remix live:` goes to both. A play-test question that needs the timing split has to read RPCS3.log
  after the emulator exits — the file is exclusively locked while it runs.
- **Do not build a launcher dry-run by `head -N` past the `cd /d "%~dp0bin"` line** (currently 4801) —
  the next line is `start "" "rpcs3-next.exe" ...` and it will launch the game.
