# Round 38 inbox — RPCS3 RTX Remix backend, Haze (BLUS30094)

Written at the close of round 37, 2026-08-24. Branch `remix-backend`, HEAD `44276ae06`, working tree carries
round 37's edits **uncommitted** (rounds 32-36 are committed and pushed).

| artefact | SHA256 (first 16) | note |
| --- | --- | --- |
| `bin\rpcs3.exe` | `56A0A57DA51FF02E` | built this round, MSBuild exit 0, **0 errors** |
| `bin\rpcs3-next.exe` | `56A0A57DA51FF02E` | **identical — the copy was made** |
| `bin\remix\d3d9.dll` | `16A0B512F33EBB66` | **UNCHANGED.** Nothing deployed into `bin\remix\`. |

**The tree did NOT move under this round.** `bin\rpcs3.exe` and `bin\rpcs3-next.exe` were byte-identical at
the start (MD5 `0dcd06947452837bdbd789131221dbf0`, 11:04 and 11:07), HEAD was `44276ae06` at both ends, and
`git status` showed only the two untracked files that were already there. That is the first round since 33
where the concurrency trap did not fire. **Hash both again anyway.**

Warnings: the full-solution build reported 7. A second pass that force-recompiled *only* the two files this
round touched reported **1** — the pre-existing C4723 "potential divide by 0", now at
`RemixGSRender.cpp:11065` (it was `:10363`; it moved because this round inserted lines above it). Zero new
warnings from this round's code. The other 6 belong to projects this round did not touch and nobody has read
them; if that matters, build with `/v:m` and no `/clp:ErrorsOnly`.

---

## READ THIS FIRST: the census could not see time, and now it can

The `Remix vmbasis:` census deduplicated on `(vp ^ albedo * golden)` into a set that lives for the whole
session. **Each (vp, albedo) pair emitted exactly ONE line per session, ever.** Round 36's 19,200-frame
play-test produced **nine lines across six frames**.

The user's complaint — "doesn't follow the camera turning smoothly" — is a statement about time. That
instrument **structurally cannot** observe it. Not "did not this time": cannot, at any cap, for any length of
play-test. Three rounds asked this log about jitter and the question was unanswerable before it was asked.

`RPCS3_REMIX_VMBASISEVERY=1` + `VMBASISVTX=3649` + `VMBASISMAX=4000` are armed. That is **~4000 consecutive
frames** of the gun body, about two minutes at 30 fps. If the play-test happened, you have the first time
series this project has ever had of the viewmodel transform, and **priority 1 is to read it before doing
anything else.**

What to compute from it, in order:

1. `cpost=[right up fwd]` as a function of `frame`. Rigid attachment means these are near-constant. Any
   frame-to-frame excursion IS the jitter, and its size in metres is the answer.
2. `camvp=` per frame. **MEASURED this round over 161,354 `Remix gauge:` rows: the elected camera is one of
   TWO programs** — `7f3d3abcefc8b057` at 512x288 (89.9%) and `ad7ce9d672a0bf6b` at 1024x576 (9.98%) — and
   **10,293 of 46,636 frames carry both**. A 180-degree rotation **doubles** any error in the axis it turns
   about, so if `cpost` jumps on the frames where `camvp` changes, that is the bug and it is nothing to do
   with the viewmodel code. This is the strongest untested lead in the round.
3. `camage=` per frame against the `cpost` excursions. It was 0 on all six round-36 census frames, so
   staleness is refuted **for those samples only** — a per-frame series can test it properly.
4. `relaxis=[right up fwd]` per frame. If the residual axis is stable per mesh, the residual is the mesh's
   pose. If it swings frame to frame, it is a defect.

If the log has **no** `Remix vmbasis:` lines, vtx 3649 was not drawn: `VMBASISVTX=0` + `VMBASISEVERY=4`.

---

## Priority 1 — the viewmodel residual. Your predecessor's pre-registration failed for a reason you can now state exactly

**Do not re-open "the pivot is stale". It is refuted three ways** and re-testing it wastes the round:
`apply_viewmodel_rotation` takes pivot 0 from `m_active_camera.position` (the anchor-frame eye is pivot 3);
`rotpivot=` equals `cam=` bit for bit on all nine round-36 lines; `camage=0` on every census frame including
the 79-degree outlier.

**The residual is an identity of the operator.** For a rotation about the camera's RIGHT axis the
post-operator diagonal is `(d0, -d1, -d2)`, hence

```
cos(relpre) + cos(relpost) = dotpre[0] - 1
```

verified to better than 4e-6 on 8 of 9 lines (the 9th had `relpre` **clamped**). So `relpost` is
`acos(dotpre[0])` to within 1.5 degrees on every line — the object-X vs camera-right misalignment, which a
rotation about `right` cannot touch at any angle. `180 - relpre` is the special case `dotpre[0] = 1`, and it
**held on six of the nine lines**; the three the round-37 brief quoted were the exceptions, and all three
read `dotpre[0] < 0.92` while all six that held read `> 0.989`.

Also correct the record: **`relpre = 180.000` is a clamp**, not a measurement. `relative_angle` clamps
`(trace-1)/2` to `[-1,1]` and only NORMALISES the object columns, never orthogonalises them, so a
slightly non-orthogonal basis pushes the trace below -1. Two of the nine lines read 180.000 for that reason.

**Whether the residual is a defect at all is the open question, and `RPCS3_REMIX_VMROTLOCK` is the test.**
Shipped OFF. Mode 1 replaces the object 3x3 with the camera basis; mode 2 aligns only the object X column.
`relpost` reading ~0 afterwards **proves only that it composed** — do not report that as evidence. The
evidence is the user's eyes: arms look right and stop swimming = it was a defect; arms freeze rigid, lose
their aim pitch, hands detach = it was the genuine pose and the knob goes back to 0.

The residual splits into two families and the axis separates them, which is why `relaxis=` now prints: lines
1-5 and 9 are a **pitch about camera right** (closable with a smaller `VMROTDEG`), lines 6-8 are **roll/yaw
about up and forward** (not closable on that axis at any angle). Do not treat them as one bug.

---

## Priority 2 — the weapon distance. The lever is new and the prediction is falsifiable

`RPCS3_REMIX_VMROTPIVOTFWD=0.15` is armed. With the pivot at camera-space `(p_r, p_u, p_f)` a 180-degree
turn about `right` sends `(r, u, f)` to `(r, 2p_u - u, 2p_f - f)`, so **the model moves by TWICE the offset**.

**PRE-REGISTERED, and every one of these can read otherwise:**

| field | must read | a different reading means |
| --- | --- | --- |
| `cpost` forward | **+0.30 larger** than round 36's on the same mesh | a 1x change = the offset is a translation, not a pivot; the composition is wrong |
| `rotpivot=` | **NO LONGER equal to `cam=`** | the offset never reached the pivot |
| `cdist_post` | **no longer equal to `cdist_pre`** | same |
| `dcentre` | still 0.3 .. 1.6-ish, shifted | ~0 = a centroid pivot leaked back in |
| knobs line | `vmrotpivotf=0.15` | a comma decimal separator parses to 0 through `wcstod` |

Why 0.15: `cpost` forward read 0.09 .. 0.55 on the round-36 lines, and the world scale is **~1 unit = 1
metre**, MEASURED from the camera rather than assumed — the eye moved 0.11 units in the single frame
10013->10014 and 0.26 units over frames 9656..9660, i.e. 2.0 .. 3.3 units/second at 30 fps, human walk-to-jog
speed. So the gun origin was sitting 9 to 55 **centimetres** from the eye. 0.15 pushes it +0.30 m.

Ladder: still too close `0.25` then `0.35`; too far `0.08`; too low `VMROTPIVOTUP=0.15`. `UP` and `RIGHT`
ship at 0 on purpose — arming two at once confounds them.

---

## Priority 3 — the helmet. Root-caused, fixed, and round 36's counter reading needs two corrections

**The UI route WORKS.** MEASURED: `ui_forced_pair=14664` rising at **exactly one per frame** over four
consecutive `Remix stats:` intervals (101/101, 99/99, 105/105, 104/104), **138** `Remix uiwrap: … route=2d`
lines carrying the helmet albedo, `DrawScreenOverlay failed` = 0. Round 35's structural claim is CONFIRMED —
`screen_space = true` reaches a `return` inside `submit_subdraw` that precedes both `per_draw_transform` and
the world `DrawInstance`, every compositor refusal is a bare `++counter; return;`, and no draw can be
submitted twice.

**It clipped because the helmet albedo `C61753D31FB96507` is submitted by THREE (vp, fp) pairs and the key
had one slot:** `830d7d1b9681c475/479890ff55f1d96e` (138 lines, UI), `f39f504649b6f442/4afa02b3dbbe9b7e` (33
lines, WORLD, **all 33 that albedo and no other**), `830d7d1b9681c475/609a4216b89e296a` (2 lines, WORLD).
Both extra pairs read `dw=0`, so the existing depth-write guard admits them. `UIFORCEPAIRVP2`/`FP2` are armed
with both, matched **positionally**.

**PRE-REGISTERED:** `uiforcepairs=` on the knobs line must read **2**. `ui_forced_pair` should roughly
double to ~2/frame — check it against the frame delta on two consecutive stats lines, that arithmetic is
exact. If the helmet **vanishes**, read `ui_skipped`.

**Two corrections to round 36's pre-registration:**

- `ui_skipped` and `ui_space_none` are **the same event** — separate members incremented on two adjacent
  lines of one `else` branch in `composite_ui_draw`. `ui_skipped` has six other increment sites and their
  equality at 5077 proves none of them ever fired. It carries no independent information, and it is
  **frozen** — it stopped moving at frame 9678 and did not move for the following 13,800 frames.
- **There is no shadow flag and no overlooked category bit.** `remixapi_InstanceInfo` has six fields and no
  shadow control; all 27 category bits were enumerated; `HIDDEN` removes primary rays too, `IGNORE` is
  documented in-tree as a no-op on this path, `HAIR_CARDS` is unimplemented. **If the UI route ever fails to
  deliver "visible, no shadow, no clipping", no flag can — the answer would have to be a second submission
  (a shadow-only proxy instance), not a category.** Say that plainly if it comes up a sixth time.

**Stated before it is seen:** the existing forced pair's `uiwrap` UVs read `u=[2676..30089] v=[3417..28455]`
on a 512x512 texture, three orders of magnitude outside `[0,1]`. Expect the new copies to look like the
composited one, not the world one. Separate defect, named not fixed.

---

## Priority 4 — the weapon screen is NOT a stale texture. Refuted. Do not arm `TEXREHASH`

MEASURED over the last 200 MB of `bin\remix_dump.log` (508,091 lines, >=72 runs): **1,767 lines carry
`86885A0E60751491` and ZERO are `Remix texstale:`** — in a window holding **703 `texstale` lines naming 382
distinct hashes**, 42 at the same `1024x1024 fmt=86` shape. The detector was live and would have named it.
The submitted UV span is **bit-identical** (`u=[0.1116..0.9944] v=[0.01239..0.992]`) across all four
`Remix picked:` lines at frames 3364, 4826, 10633, 13920 and across two vertex programs, so it is not UV
animation either. **The pick landed on the weapon BODY atlas. The live numerals are a different draw.**

Corrected mechanism, for the record: the cache **key** is descriptor-only (`RemixTextures.cpp:308-317`) but
the **albedo hash is genuine texel content** (`RemixTextures.cpp:831-837`). `decode()` is the only writer of
`content_hash` and runs only on a descriptor-key miss, so identity is content-sampled exactly once and then
cached behind a descriptor key forever.

Cost of arming it anyway, so nobody re-derives it: the albedo hash folds into the **mesh** key at three sites
in `RemixGSRender.cpp` (static-index entry, static-index UNION, ordinary mesh) as well as the material key.
In-tree precedent 5,881 -> **188,427** meshes created.

**A selective refresh is not possible with the current header.** `remix_c.h` has Create/Destroy for materials
and textures and **no `UpdateMaterial`, no `UpdateTexture`**; the only `Update*` is `UpdateLightDefinition`.
`remixapi_InstanceInfo` has no material field, so it cannot be overridden per instance. The nearest existing
code is the hash-pinned in-place re-decode at `RemixTextures.cpp:532-560`, called only by the UI compositor.
Making it work for 3D means `DestroyTexture` + `CreateTexture` with `info.hash` pinned — which hinges on
whether the fork's `remixapi_CreateTexture` **replaces** a texture already registered under an existing hash.
`textureHashPathLookup` is absent from every local dxvk-remix clone. Unverifiable from source; measure it
against the deployed DLL or read the fork.

**The next measurement is to re-pick on the NUMERALS, not the weapon body**, and cross-check that draw
against `Remix skip-census:` / `Remix world-refused:`. `RPCS3_REMIX_WATCH` is already armed (19,366
`Remix watch:` lines in the window), so the watch route is the cheapest instrument.

---

## Priority 5 — light fixtures. Two of round 34's premises do not survive re-measurement

No fix shipped, on evidence, and that is deliberate — a `(vp, fp)` gate would have over-matched ~60x.

- **They are not viewmodel-tagged.** `viewmodel=0` on all 8 `Remix picked:` lines for
  `fp=c61b0b9586dd67fb`, `vmlisted=0` / `listedvp=0` on all **11,775** `fpcandidate` lines, `depth_write=1`
  throughout. Worth checking first because a 180-degree flip about the eye is the one operator that makes an
  object move with the camera — and it is not the cause.
- **The `(vp, fp)` pair is not narrow.** It carries **438 distinct albedos over 11,775 rows** in the last
  300 MB, top albedo 278 rows. Round 34's "seven fixture albedos, no overlap" describes a much smaller
  window than the current log.
- **The lead is an existing classifier.** `Remix light-candidate:` already prints `state=fixture` per albedo
  on exactly this family (`albedo=BEE05E8D1184F67E … extent=0.4627 state=fixture depth_write=1 vtx=1020`).
  **Key any gate on that predicate, not on `(vp, fp)` and not on `vp`.**
- Still unmeasured: whether the fixture *mesh* moves or only its *light*. There is no per-frame position
  census for it. The `VMBASISEVERY` pattern shipped this round is the template for adding one.

---

## Priority 6 — geometry following the camera by the door. `c1d482dcd1b03ed0` is not the viewmodel path

**REFUTED**, MEASURED over the last 300 MB: `viewmodel=0` on all 7 `picked` lines, `vmlisted=0` /
`listedvp=0` on all **6,406** `fpcandidate` lines, and `scale_z=0.49875 offset_z=0.50125` on every line that
prints them — the ordinary world depth range, not the viewmodel's `0.00125`. It never reaches the tag.

Carried from round 36 and still true: do NOT re-report `ref=anchor_prev` as a one-frame lag (round 32 proved
that is a tautology); `d0b6a471bb2d463b` is refuted; the `camclip=512x288` vs `clip=1024x576` mismatch runs
at ~100% while the effect is at 0% and is not causal.

**The one unexplained thing the census does show**, worth the next round: on `Remix worldid-draw:` this
program's recovered transform carries a **non-zero translation on geometry whose raw vertex AABB is already
absolute world**, e.g. `raw=[1762.63 -26.6278 1076.73]..[1786.28 -13.1973 1157.18]` against
`pre=[… 15.0431; … 42.5709; …]`, `translation=42.5709`, `camage=0`, `basis_delta=0.0184`. Applying that to
already-absolute vertices displaces the mesh ~42.6 units. Establish first whether `raw=` is the pre- or
post-transform AABB — the whole reading turns on it, and guessing would be a round wasted.

---

## Carried, untouched this round

- **Frame time.** Not measured again. Round 35's standing instruction still holds: either blank
  `SKIPEXTENTVP` and arm `DRAWAUDIT=0` together and say so, or attack `decode` (3.93 ms) with round 33's
  named fix. Note `SKIPEXTENTVP` is armed on `57A12323F22F4988`, which is one of the viewmodel programs
  priority 1 is measuring, so do the `decode` half while the viewmodel work is live.
- **The static-index budget** is still relieved. The untaken lever is a longer idle window scoped to
  static-index UNION meshes only — a code change.
- **`rtx.fallbackLightMode` is 0** in `bin\rtx.conf`; round 33 asked for 2. That file is the user's and is
  read-only to these rounds.
- **`Use RSX Backface Culling` has been OFF the whole time** — the custom config wins.
- **STILL OWED:** `classify_draw`'s "IGNORE is a no-op on the API draw path" wants confirming by
  disassembling `bin\remix\d3d9.dll` (this round confirmed the *bit exists*, not that it does nothing);
  `RemixTransforms.h`'s "What `DRAWAUDIT=0` costs" still names neither `m_streak_measured` consumer.

---

## Play-test card

Launch as usual (`launch-haze-remix.cmd` -> `bin\rpcs3-next.exe`, **`56A0A57DA51FF02E`** — hash it).

| what to look at | where to read it | one-line revert |
| --- | --- | --- |
| **Weapon at a sane distance?** | `Remix vmbasis:` -> `cpost=[right up fwd]`, forward ~0.30 larger; `rotpivot=` must no longer equal `cam=`; knobs line `vmrotpivotf=0.15` | `set "RPCS3_REMIX_VMROTPIVOTFWD=0"` |
| still too close / too far | same field; each change moves it 2x | `0.25` then `0.35` / `0.08` |
| weapon too LOW | `cpost` up | `set "RPCS3_REMIX_VMROTPIVOTUP=0.15"` |
| **Does it follow the camera smoothly?** | **TURN LEFT AND RIGHT WHILE PLAYING.** ~4000 consecutive `Remix vmbasis:` lines are the point of this run | `set "RPCS3_REMIX_VMBASISEVERY=0"` |
| no `Remix vmbasis:` lines at all | vtx 3649 not drawn | `set "RPCS3_REMIX_VMBASISVTX=0"` + `set "RPCS3_REMIX_VMBASISEVERY=4"` |
| still not smooth **after** the distance fix | one variable at a time, never both | `set "RPCS3_REMIX_VMROTLOCK=1"`, then `2` |
| **Helmet: visible, no shadow, no clipping?** | knobs line `uiforcepairs=` must read **2**; `ui_forced_pair` ~2/frame | `set "RPCS3_REMIX_UIFORCEPAIRVP2="` |
| helmet VANISHED | `ui_skipped` (frozen at 5077 since frame 9678 — it would have to MOVE) | same revert |
| helmet visible but wrongly textured | expected; the existing pair's UVs are already 3 orders out of range | not fixed this round |
| Sky / static-index / everything else | unchanged from round 36 | see round 36's card |
