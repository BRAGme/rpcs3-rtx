# Round 42 inbox — RPCS3 RTX Remix backend, Haze (BLUS30094)

Written at the close of round 41, 2026-08-25. Branch `remix-backend`, HEAD **`4fdaecd67`**, working tree
carries rounds 37-41 uncommitted.

| artefact | SHA256 (first 16) | note |
| --- | --- | --- |
| `bin\rpcs3.exe` | `49D31EEA1D767004` | built this round, MSBuild `Release\|x64` exit 0, **0 errors, 1 warning** |
| `bin\rpcs3-next.exe` | `49D31EEA1D767004` | **identical — the copy was made** |
| `bin\remix\d3d9.dll` | `16A0B512F33EBB66` | **UNCHANGED.** Nothing deployed into `bin\remix\`. |

The one warning is the pre-existing C4723 "potential divide by 0", now at `RemixGSRender.cpp:11339`.
**Zero new warnings.** **The tree did not move**: HEAD `4fdaecd67` at both ends, both exes read
`46D3DF83376C0391` at the start exactly as round 40 recorded.

Round 40's build was play-tested. **The guest-light fix landed** — `guest_lights=31`, `guest_light_match=826`,
non-zero for the first time in the project — **and the user's verdict was "nothing got fixed", which is
fair.** The lights that appeared were the wrong lights. Round 41 fixes that and ships the vertex-colour
change round 40 only diagnosed.

---

## PLAY-TEST CARD

Full derivations in `docs\remix\KNOBS.md` "Round 41". Two things changed that can move pixels; everything
else is a one-line revert.

### 1. The lights are now keyed on GLOW CARDS, not on a bulb texture

**What was wrong, MEASURED from all 31 `Remix guest-light:` lines:** every light was the same albedo
`71D189E9B559A7F9`, and their world extents ran **0.5385 .. 83.18 — a 154× spread on one texture**. Nine rows
at 0.54 .. 1.73 are the bulbs; twenty-two at 7.89 .. 83.18 are a different mesh sharing the texture. The
83.18 row produced `radius=29.1` at the centroid of an 83-unit mesh. **That is the oversized disc in your
screenshot.** That hash also never appears in the backend's own `Remix light-candidate:` census — we were
lighting a texture nothing had nominated.

**What is armed now:** `GUESTLIGHTAUTO=2` — *"put a light where a glow card is drawn"* — with the albedo list
blank. The census separates two clean populations: small additive **glow cards** carrying the game's own lamp
tints (warm white `[0.842 0.831 0.578]`, yellow `[1 1 0.741]`, pure white) and larger opaque **fixture**
housings at a flat metal `lum ≈ 0.72`. Mode 2 takes only the cards.

**The hypothesis this run tests (INFERRED):** Haze draws a glow card over a fixture that is **lit** and omits
it for one that is not. If it holds, this reproduces the raster reference's *"not every bulb is lit"* with no
per-bulb list, and each light gets the card's own colour and its own extent-derived radius for free.

Also fixed: the radius was a flat 0.6 (a 60 cm emitter — **larger than a 0.54-unit bulb**), now
`max(0.1, extent × 0.5)` = the mesh's own half-extent; and the extent ceiling now applies to a listed albedo
too (`GUESTLIGHTLISTEXT`, new counter `guest_light_toobig`).

| reading on `Remix live:` | meaning |
| --- | --- |
| `guest_lights` roughly matches the number of lit fixtures you can see | **PASS** |
| far more lights than lit fixtures, or `guest_light_capped` climbing | the glow card is **not** the "is lit" signal — pre-registered refutation |
| `guest_lights=0` | mode 2 matched nothing; check `guest_light_match` |
| `guest_light_toobig` > 0 | the size gate is doing its job (expect ≈ 22 if the old list were still armed) |

Brightness: `GUESTLIGHTRADIANCE=1200` is the **only** knob. Radiance is per unit area and the emitter shrank
~4× in radius, so this is a deliberate half-step and may read slightly dimmer than round 40. Too dark → 2400
then 4800. Too bright → 600 then 300.
Revert the whole thing: `set "RPCS3_REMIX_GUESTLIGHTAUTO=0"`. **Do not use mode 1 as a fallback** — mode 1 is
the whole census population including housings, which is what put lights on doors in August.

### 2. Vertex colour now reaches textured surfaces — the white walls and the grey lava smoke

**The diagnosis (round 40, unchanged):** every one of 192 `Remix alphastate:` rows reads `tcolor=1/0/3` —
*colour = albedo texture, vertex colour discarded* — while 22 of 47 vertex programs, **including the wall
program `ad7ce9d672a0bf6b`**, carry a fully replayable `route=scaled slots=[c[18].x..w]`. The vertex side is
ready; the **fragment classifier** is the gate, and it recognises two shapes in the entire title
(`fpclass=1/1/0`, `fpvcol_applied=698` of 5,783,237 draws = 0.012%).

Three things shipped together, because any one alone is a no-op:

- **`FPVCOLHOP=4`** — the classifier now steps backwards through **identity temp copies** (unconditional
  `MOV` of a temp, identity swizzle, no negate/abs) before deciding. That is the identity function, so it
  **cannot admit a shape the classifier did not already recognise** — it only reaches programs that build the
  recognised shape in a temp and export it. Default 0 in code; armed at 4 here.
- **`VCOLMOD=1`** — this had been **0**, and `fp_wants_vcol` ANDs it in, so without this the hop could not
  change one pixel.
- **`VCOLCONSTBLACK=1`** (new, default on) — the guard that makes `VCOLMOD=1` safe again.

**READ THIS IF THE HUD GAUGES GO BLACK.** `VCOLMOD` was disabled on 2026-08-15 with the note *"the HUD gauges
render BLACK with this on … most likely those meshes carry no COL0 attribute"*. That guess is wrong — the
code already returns when `map_attribute(3, …)` fails. The real cause is measured: `Remix vcolroute:` names
exactly two constant-route programs and one of them resolves to `vp=b01bfce3fc580e3b route=constant
cval=[0 0 0 1]` — **a constant vertex colour of pure black**. On an untextured draw that constant is the
colour and replaying it is right; on a **textured** draw it is a Modulate factor, and a Modulate by zero
deletes the surface. The new guard refuses a constant-route colour under `1/255` on textured draws only.

| reading | meaning |
| --- | --- |
| `fpclass=` moves off `1/1/0`, `hops=` > 0 on `Remix fpvcol:` | the hop reached — **this is the pass** |
| `fpclass=` still `1/1/0` | the hop is **not** the gate. Read the new `Remix fpother:` census (below) |
| gauges black **and** `vcol_const_black` > 0 | the guard fired but something else is also black |
| gauges black **and** `vcol_const_black` = 0 | **the guard is aimed at the wrong mechanism** — pre-registered refutation |
| `mesh_created` far above 582,659 | an animated vertex colour is minting a mesh per step |

Revert: `set "RPCS3_REMIX_VCOLMOD=0"` — one line, and it restores round 40 exactly.

### 3. The instrument that did not exist: `Remix fpother:` in `bin\remix_dump.log`

`Remix fpvcol:` has only ever printed programs that **did** classify, so a title whose answer is "two of
them" produced two lines and no way to see the other hundred and forty. The new line prints, once per unique
fragment program, the terminal instruction the walk ended on:

```
Remix fpother: fp=<hash> op=<opcode> srcs=N pred=0/1 hops=N t0/t1/t2=<src reg types>
               k0/k1/k2=<bitfield> sampled=0x.. instrs=N fp32=0/1 line=n/64
```

`k` bits: `1` clean COL0/COL1 read, `2` temp whose writer sampled a texture, `4` temp, `8` non-identity
swizzle, `0x10` negated, `0x20` abs. **A row reading `op=MUL k0=2 k1=1` should already have classified; a row
reading `op=MAD`, or `k1=9` (COL0 but swizzled), names the exact widening round 42 has to write.** If item 2
does not visibly change anything, this census is the deliverable and it is what round 42 should be built on.

### 4. Still carried from round 40, unread

`SKIPVP=` blanked (stage 2 — watch for the wooden plank floor, and revert first if anything renders at absurd
scale), `SKYCLASSIFY=1`, `CAMTRACE=1` (**the FOV has still never been measured** — read `fov=` in
`bin\remix_dump.log`, then set it back to 0). And **Ctrl+Click a white wall and a vanishing floor** — all 14
existing picks landed on surfaces that resolved correctly, and there is no counter downstream of
`CreateMaterial`.

---

## What round 41 established, and what it refuted

- **`GUESTLIGHTRADIUSSCALE=0` never meant "use the fixed radius".** `env_float` rejects 0 and returns the
  accessor's fallback of **0.35**; the banner has echoed `glradiusscale=0.35` all along, and the accessor's
  own comment claiming otherwise is wrong. Now written explicitly as `0.5`.
- **The 2026-08-15 black-HUD diagnosis is refuted.** "No COL0 attribute" cannot be it —
  `map_attribute(3, …) != ok` already returns without writing. The cause is a constant route resolving to
  `[0 0 0 1]`, which the `Remix vcolroute:` census had been printing all along.
- **`small_enough` never gated a listed albedo**, and no comment ever argued for the exemption. That is what
  let an 83-unit mesh become a 29-unit light.
- **The `(vp, fp)` key is dead for the glow-card population.** `F613BD83DAF2B4E2` alone is drawn by **five**
  vertex programs and **twelve** fragment programs.

## Clamp audit for the knobs this round touched

| knob | armed | clamp | note |
| --- | --- | --- | --- |
| `GUESTLIGHTAUTO` | **2** | `min(env, 2)` | **at its ceiling** — deliberate: 2 is the highest *defined* mode, not a truncation. If a mode 3 is ever added, move the clamp with it |
| `GUESTLIGHTMAX` | 128 | `clamp(env, 1, 4096)` | 32× |
| `FPVCOLHOP` | 4 | `min(env, 8)` | 2× |
| `GUESTLIGHTRADIUS` / `SCALE` / `RADIANCE` | 0.1 / 0.5 / 1200 | `env_float` | all > 0, so none hits the reject-zero trap |
| `GUESTLIGHTLISTEXT`, `VCOLCONSTBLACK` | unset → code default 1 | `env_u32(…, 1) != 0` | boolean |
| `GUESTLIGHTMAXEXT`, `GUESTLIGHTLUM`, `VCOLMOD` | 6 / 0.7 / 1 | — | **each armed at its own shipped default, so the launcher line is a no-op.** Kept because each is a documented one-line revert |

---

## THE CHAPTER LIST — the premise was wrong, and that is this round's most useful result

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

## Priorities for round 42

0. **The chapter list — read the section above first.** The mask gate is provably not the limiter, and the
   patch has never been shown to work without the archive edit. Next step is `0x4f4b40`, and it needs the
   decrypted ELF regenerating.
1. **Whatever `Remix fpother:` says.** If `fpclass` did not move, that census names the terminal instruction
   of every unclassified fragment program and the widening writes itself. This is still the largest visual
   defect and the one with the diagnosis complete.
2. **The one-frame donor lag** (props bounce on walking, viewmodel does not track). Round 38's model is
   missing its translation term; round 38's "needs no new machinery — only a third branch here" is **refuted**
   because `divide_anchor` is the only carrier of the f64 inverse and `camera_state::reference_inverse` is
   f32 with no f64 twin. The honest version is to give `camera_state` an f64 reference inverse computed at
   latch time, then add the branch. Seven assignment sites: `RemixGSRender.cpp:2693, 2978, 3440, 15946,
   16191, 16238, 16486`.
3. **Parallel comma lists for `GUESTLIGHTVP`/`GUESTLIGHTFP`** (the `UIFORCEPAIRVP2`/`FP2` idiom) if the
   primary list is ever needed again. Round 41 severed the narrowing for the glow rule, which removes the
   urgency but not the defect.
4. **`apply_vertex_alpha` latent bug, fix it with any further vertex-colour work.** It writes
   `color = 0x00FFFFFFu | (alpha << 24)` — forcing RGB to **white** — and runs *after* the
   `material && fp_wants_vcol` call to `apply_vertex_colour` as an independent `if`, not an `else`. With
   `VCOLMOD=1` now armed, any textured draw with a constant-alpha albedo will have freshly replayed RGB
   overwritten white by the alpha rescue. **This may already be visible in this round's play-test.**

## Instrument notes worth keeping

- **A knob that ANDs into the gate you are widening will silently swallow the widening.** `VCOLMOD=0` would
  have made every line of round 41's task 2 unobservable. Grep the full boolean chain a matcher's result
  feeds and check every term is armed before shipping the matcher change.
- **A census that prints only its successes cannot tell you why it is failing.**
- **"Most likely X" in a knob's disable note is a hypothesis, and it will be inherited as fact.**
- **When one texture is shared between a fixture and a prop, extent separates them and the hash cannot.**
- **`bin\remix_dump.log` is ~1 GB and append-mode.** Never grep it whole: `tail -c 200000000 <file> | grep -o
  "<pattern>" | tail -N`, shrinking the window until output fits.
- **Do not build a launcher dry-run by `head -N` past the `cd /d "%~dp0bin"` line** — the next line is
  `start "" "rpcs3-next.exe" …` and it will launch the game. Cut at the `cd /d` line, found by grep, never by
  a hard-coded count.
