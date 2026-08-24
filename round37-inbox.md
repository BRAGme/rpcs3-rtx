# Round 37 inbox — RPCS3 RTX Remix backend, Haze (BLUS30094)

Written at the close of round 36, 2026-08-24. Branch `remix-backend`, working tree dirty (rounds 32-36 are
**not committed**; HEAD is still `1ebff4c57`).

Deployed for play-test:

| artefact | SHA256 (first 16) | note |
| --- | --- | --- |
| `bin\rpcs3.exe` | `F9206BA434AE3ADB` | built this round, MSBuild **exit 0, 0 errors, 1 warning** (same count round 35 reported for the pre-existing C4723 divide-by-0; the text was suppressed by `/clp:ErrorsOnly`, so I did not re-read it) |
| `bin\rpcs3-next.exe` | `F9206BA434AE3ADB` | **identical — the copy was made** |
| `bin\rpcs3-next-pre-round36-20260824.exe` | `C87A2930E676F5F8` | the build that was actually deployed before this round. See below. |
| `bin\remix\d3d9.dll` | `16A0B512F33EBB66` | **UNCHANGED.** Nothing was deployed into `bin\remix\`. |

---

## READ THIS FIRST: the title was not running round 35's binary, and another session shares this tree

MEASURED at round 36's start. `bin\rpcs3.exe` was `E984DD7DAE13CAD5` dated 2026-08-17 13:21 — round 35's
build, exactly as its inbox said. But **`bin\rpcs3-next.exe`, the file the launcher actually runs, was
`C87A2930E676F5F8`, dated 2026-08-23 08:01, 7,680 bytes larger**, copied from `build\codex-link\rpcs3.exe`.
`rpcs3\Emu\RSX\Remix\RemixGSRender.cpp` carries a 2026-08-23 07:51 mtime and `launch-haze-remix.cmd` a
2026-08-22 23:11 one. A concurrent session (a Resistance 2 effort) edited this tree, built into its own
output directory and deployed that.

So the play-test verdicts round 36 was handed were produced by `C87A2930E676F5F8`, not by round 35's build.
Round 36's binary is taken from the CURRENT tree and therefore contains round 35 + that session's changes +
round 36's, and supersedes both.

**Hash `bin\rpcs3-next.exe`, not `bin\rpcs3.exe`, before trusting any play-test.** The project memory
records this concurrency trap twice already; this is the third instance and the first where the two `bin`
artefacts diverged silently.

---

## Priority 1 — judge the viewmodel. The model changed; the sweep is over.

**The finding, in one line: the viewmodel is not mis-oriented, it is submitted BEHIND THE CAMERA**, and the
180-degree rotation about the camera's right axis that puts it there also explains the orientation. The
brief's own refutation of the sign-flip model was correct; what it could not see is that the missing half is
position, and a dot product of two directions has no position in it.

MEASURED over 287 `Remix vmbasis:` lines (last 400 MB of `bin\remix_dump.log`, the `VMBASIS=5` session),
re-projected from each line's own `pre=`/`post=`/`right=`/`up=`/`fwd=` fields — no new build was needed:

- the camera basis is left-handed on **all 287** lines (`right x up . fwd = +1.00`), so the forward sign is
  readable;
- **26 near-eye (vp, vtx) groups, every one with the centroid at fwd < 0** (range -0.06 .. -0.81), up mostly
  > 0 (+0.05 .. +0.94), 0.3 .. 1.1 units from the eye;
- **the control**: `vp=aae8e0d5ae292dd4 vtx=91`, the one near-eye group that is NOT viewmodel-tagged, reads
  fwd **+0.083 .. +0.484** — in front, 8.6 degrees off camera-aligned. That is what correct looks like;
- the relative basis is a rotation about the camera RIGHT axis of 118..179 degrees, `det = +1.00000`, column
  norms `1.0000` — one clean rotation, not "two axes half wrong".

`VMBASIS=6`'s 3x3 was **correct** (at 180 degrees Rodrigues collapses to exactly what the mask builds); it
was pivoted at the CENTROID, so it turned the arms in place and left them behind your head. At flip 6 the
post-operator relative angle is 2.1 / 2.7 / 4.1 degrees on the three main groups with `dcentre <= 6.5e-06`.

**Armed:** `VMBASIS=0`, `VMROTAXIS=1` (camera right), `VMROTDEG=180`, `VMROTPIVOT=0` (the RAW eye, no
anchor conversion — justified because `cdist_pre` = `|centroid - m_active_camera.position|` reads
0.43..1.04 on all 287 lines, so they are in the same frame).

**PRE-REGISTERED. Read `Remix vmbasis:` in `bin\remix_dump.log` — it is readable while the game runs.
Record the actual value even where it disagrees.**

| field | must read | a different reading means |
| --- | --- | --- |
| `cpost` fwd | **> 0** where `cpre` fwd < 0 | still behind the eye — pivot or axis wrong |
| `cpost` up | **< 0** where `cpre` up > 0 | same |
| `cpost` right | unchanged to ~1e-3 | a rotation about `right` cannot move the right component |
| `relpost` | `180 - relpre` +- 1 deg, i.e. **1 .. 62** | the 3x3 did not compose |
| `cdist_post` | == `cdist_pre` to 1% | ~2550 = the anchor-frame eye leaked back in |
| `dcentre` | **0.3 .. 1.6** | **~0 = a CENTROID pivot leaked back in.** `cdist` alone cannot see this — it is invariant under both pivots. `dcentre` is the only discriminator. |
| `dbasis` / `drot` | **0** / **~2** | `dbasis` non-zero = `VMBASIS` still armed and the two are confounded |
| `knobs=` | `vmrotaxis=1 vmrotdeg=180 vmrotpivot=0` | a clamp or a typo |

**The failure modes and their one-line answers, no rebuild:**

- right way up but the wrong PITCH -> that residual is the 118..179 spread (the rig's own aim pitch). Try
  `set "RPCS3_REMIX_VMROTDEG=156"`, then `118`.
- upside down the OTHER way -> the model-space hypothesis the brief named is live:
  `set "RPCS3_REMIX_VMROTAXIS=4"` and `set "RPCS3_REMIX_VMROTDEG=270"` (-90 about the model's own X).
- **arms VANISH** -> they were only ever visible through some other path; that is itself the finding, and
  `cpre` fwd < 0 on the census says they were behind the camera all along.
- REVERT: `set "RPCS3_REMIX_VMROTAXIS=0"`.

**Do NOT re-open "a little high" yet** — the same warning round 35 gave. Height is a component of the same
displacement this operator is undoing; re-measure `cpost` up after the orientation settles.

---

## Priority 2 — the helmet, aimed for the first time in four asks

Armed: `RPCS3_REMIX_UIFORCEPAIRVP=830D7D1B9681C475` + `UIFORCEPAIRFP=479890FF55F1D96E`.

MEASURED (1,038,135 lines, counted twice by independent scripts that agree exactly):
the vp alone is **47,399 lines / 31 fragment programs / 230 distinct vtx on its largest fp** — a ~26x
over-match, round 20's failure mode. The PAIR is **1,833 lines, 96.4% of them the helmet**
(albedo `C61753D31FB96507`, vtx=59). Residue: 35 lines of a vtx=152 family (extent ~2.2) and 9 quads under
extent 0.13.

The route was chosen on round 35's reading of the deployed runtime, not on preference: no per-instance
castShadow flag exists; `Hidden` sets `mask = 0` and would make the helmet vanish;
`THIRD_PERSON_PLAYER_MODEL` needs `rtx.playerModel.enableInPrimarySpace = True`, which masks every
VIEW_MODEL candidate to zero and would take the ARMS with it. A UI-forced draw returns before
`per_draw_transform`/`submit_subdraw` — no mesh, no instance, no material, no BLAS.

**PRE-REGISTERED:**

- the helmet is still visible, casts no shadow, and no longer clips into geometry;
- `ui_forced_pair=` on `Remix stats:` in **hundreds or thousands**. **Zero means the hashes never matched,
  not that the mechanism failed** — and `uiforcepairvp=` / `uiforcepairfp=` beside it print the PARSED
  values, so a typo that parses to 0 shows there. `Remix stats:` goes to `bin\log\RPCS3.log`, which is
  exclusively locked while the game runs — read it after the session;
- **THE REFUTING READING:** if the helmet **disappears**, the compositor refused it. Read `ui_skipped`,
  `ui_space_none`, `ui_render_target` — a refusal DELETES the draw rather than falling back to world
  geometry. That is the stated risk of this route and it is the one thing that can go wrong.
- if the vtx=152 family or the tiny quads show up as stray screen overlays, the next refinement is a vertex
  count filter on the pair. It is a code change (vertex_count is not in scope at the force site), not a knob.
- REVERT: `set "RPCS3_REMIX_UIFORCEPAIRVP="`.

---

## Priority 3 — the black sky. The premise handed to round 36 was wrong about the mechanism.

**The fix that was actually shipped is a launcher line, and it is `RPCS3_REMIX_SKYEMISSIVE`.**

The brief reasoned: the domes fail the anchor gate, so they are never categorised SKY, so they render black.
The first two clauses are true. The third does not follow, and this launcher already contained the
refutation at `launch-haze-remix.cmd:1775-1790`, read out of the runtime source:
`rtx_instance_manager.cpp:1006` sets `m_isHidden = true` for `CameraType::Sky`, and `rasterizeSky()` is
unreachable from an API-submitted draw. `is_sky` sets `REMIXAPI_INSTANCE_CATEGORY_BIT_SKY`
(`RemixGSRender.cpp:21468`). **On this backend a SKY tag can only ever make the dome INVISIBLE.** Widening
the anchor gate would have turned black domes into absent ones.

What makes a dome visible here is the EMISSIVE material, and `sky_emissive_albedo_matches()` is consumed in
`RemixTextures.cpp:1049` / `:1235` against the texture CONTENT HASH, with no reference to `is_sky` or to the
anchor gate at all. The list held two hashes; the newly-reachable levels use different dome textures.

**Armed:** four added, six total —
`D1A6D1B27ADE6232,CDFE11B12552EA2D,35C2353F6B3CE2A8,174F4F689CF2A3D8,3213E0CC136ED294,32AE81D64BEA29CD`.
The four are the largest anchor-rejected dome candidates in the census (wext 1.29e6..2.14e6 at vtx 42..266),
each recurring across six separate runs. Bound verified against the parser: `std::array<u64, 8>`, buffer
400 wchar; six entries, 101 chars.

**PRE-REGISTERED, per level:**

- `sky_emissive_albedos=` on the knobs line **must read 6**. 2 means the list did not parse; 8 means
  something truncated it.
- the sky stops being black and starts glowing at `SKYEMISSIVEINT=2.0`.
- **black -> BLINDING** is a success with the wrong intensity: lower `RPCS3_REMIX_SKYEMISSIVEINT`, do not
  remove the hash.
- **still black in some level** -> that level's dome is a fifth texture. Ctrl+Click the sky and read
  `albedo=` off `Remix picked:`, or take the largest-`wext` row of `Remix sky-census:`. Two slots are free.
- **terrain or ground vanishes at certain angles** -> that is over-tagging, and the standing first test is
  `set "RPCS3_REMIX_SKYLEARN=0"`. It would NOT be caused by this change (nothing here tags anything), so if
  it happens, look at `SKYLEARN` and `CAT_SKY`.
- REVERT: `set "RPCS3_REMIX_SKYEMISSIVE=D1A6D1B27ADE6232,CDFE11B12552EA2D"`.

**Also shipped, deliberately NOT armed: `RPCS3_REMIX_SKYANCHORMODE` (default 0).** The anchor test really is
comparing the wrong quantity — `|transform.translation - eye|`, which for an absolute-world draw evaluates
`|eye|`; the unlocked levels read `anchor=2137.85 limit=4` with `|camera| = 2137`. `canchor=`
(`|AABB centre - eye|`) and `anchormode=` now print beside `anchor=` on every `Remix sky-census:` row at
every mode, so the switch is auditable from one run.

**Before anyone arms mode 2, run this cross-tab first.** MEASURED on the 278 `reject:anchor` rows of the
last 250 MB: **146 are HIGH-vtx terrain (1211..12875 vertices)**, 69 are plausible dome bands — and
`inside=1` fires on **exactly 146 of the 278**. The counts coincide, and nobody has checked whether
`inside=1` selects the terrain or the domes. If it selects the terrain, mode 2 is precisely backwards.
No anchor threshold separates them either: at `anchor<=10` it is 51 terrain to 2 domes; at `<=100`, 142 to
12. What DOES separate them, measured on the same rows: the two populations share **zero vertex programs and
zero albedos**, and `backdrop=1` fires on 39 dome rows and **0** terrain rows. Key a future rule on the
program hash, the albedo, or `backdrop` — never on a looser distance.

**`haze_sun_table.csv` and `levelnames.json` DO NOT EXIST.** Exhaustive recursive search of
`C:\Users\Tristan\AppData\Local\Temp\claude` (116 files across 5 session directories),
`C:\Users\Tristan\.claude` and the repo found neither, nor any `haze` directory containing them. The
203-row per-level sun table has to be re-mined from the game archives before it can be used.

---

## Priority 4 — "geo follows the camera": round 34's finding is REFUTED. No fix shipped, and that is the answer.

The brief asked me to confirm the bimodal world divide is still live and fix it if the evidence supports a
fix. It does not.

- **There is no `discard=` field and there never was one.** Whole-file byte scan of all 923,425,680 bytes:
  `discard=` 0 hits, `disc=` 0, `worlddiv` 0. Round 34's "discard" is a narration name for `translation=` on
  `Remix worldid-draw:` — the code calls it that at `RemixGSRender.cpp:19511`.
- **On `d0b6a471bb2d463b` the >1000 bucket is 6 of 5,380 = 0.11%**, against round 34's 76 of 448 = 17.0%,
  and **0 of 72 in the most recent run**, whose own cumulative census reads `tmax=2.23517e-08` over
  **35,202 draws**. Below the pre-registered 1% threshold on both readings.
- **The `camclip=512x288` vs `clip=1024x576` mismatch is still there and cannot be the cause.** It disagrees
  on 96.43% of target rows and 98.79% of `Remix gauge:` rows — including 72 of 72 rows of the newest run,
  where the residue is 0%. A condition at 100% while the effect is at 0% is not the cause.
- **Not unique to that mesh, and that mesh is the least affected of the top six:**
  `c1d482dcd1b03ed0` 24.80%, `ad7ce9d672a0bf6b` 2.14%, `0214281b9a7a412d` 2.13%, `bd1c10df5703e559` 0.68%,
  `d0b6a471bb2d463b` 0.11%. `basis_delta > 1000` is **0 rows on every vp**.
- **New correlation, honest in both directions.** Cross-tab of `camage` against `translation`, 265,453 rows:

| camage | `Remix gauge:` rows | gauge t>=1000 | `worldid-draw:` rows | worldid t>=1000 |
| --- | --- | --- | --- | --- |
| 0 | 62,927 | **0.01%** | 163,432 | **4.71%** |
| 2-8 | 4,032 | 0.52% | 15,154 | 0.09% |
| 9-64 | 1,859 | 9.04% | 11,700 | 0.27% |
| >64 | 1,468 | **28.61%** | 1,221 | **6.72%** |

  A stale camera anchor enriches the GAUGE outliers 2861-fold and the PER-DRAW outliers only 1.4-fold. So
  it explains the reference going wrong and does **not** explain the residue. Corroborating: at frame 55760
  `Remix gauge:` carries `translation=3346.54 basis_delta=1.63014 camage=711`, and the identical numbers
  appear on `ad7ce9d672a0bf6b`, `c1d482dcd1b03ed0` and `d0b6a471bb2d463b` the same frame — the outlier
  belongs to the frame's reference, inherited by whatever draws in it.

**The live lead for round 37 is `c1d482dcd1b03ed0` at 24.80%, not `d0b6a471bb2d463b`**, and the question is
what the world-identity override is discarding on a quarter of that program's draws. `WORLDIDENTITYVP` and
its pair knobs are armed on a six-entry list in the launcher; start there.

---

## Priority 5 — carried, still open

- **Frame time.** `DRAWAUDIT=0` was NOT taken again, and this round did not measure the split — the round
  was spent on four other tasks. Round 35's standing instruction holds: either blank `SKIPEXTENTVP` and arm
  `DRAWAUDIT=0` together and say so, or attack `decode` (3.93 ms) with round 33's named fix (cache decoded
  positions per source pointer + count). Note `SKIPEXTENTVP` is armed on `57A12323F22F4988`, which is one of
  the near-depth viewmodel programs priority 1 is measuring — so if priority 1 is still open, do the
  `decode` half instead.
- **The static-index budget** is still relieved (round 35: `peak=56 budget=64 dropped=0 stale=0`). The
  untaken lever is still a longer idle window scoped to static-index UNION meshes only — a code change.
- **The warping light fixture**, +-0.28% anisotropic basis error on
  (`830d7d1b9681c475`, `5BC48BBB303398E3`) at 180 vertices. Untouched for three rounds.
- **`rtx.fallbackLightMode` is 0** in `bin\rtx.conf`; round 33 asked for 2. `bin\rtx.conf` is the user's
  file and read-only to these rounds.
- **`Use RSX Backface Culling` has been OFF the whole time** — the custom config wins.

## Corrections owed to standing documents

- **MADE:** the four-gate audit round 32 left open is redone against current bytes, and the split is by
  QUANTITY, not by armed-ness. `VIEWMODELANCHOR` (`RemixGSRender.cpp:21203-21208`) and `VMPAIRMAXDIST`
  (`:21094` -> `viewmodel_pair_rejects` at `:6431`) both compare the transform's TRANSLATION and are
  therefore measuring `|eye|` for absolute-world draws; `SUNCARDMINDIST` (`:4763`) and `FPCENSUSMAXDIST`
  (`:7447`) both compare the geometry CENTRE and are correct. `VIEWMODELANCHOR` never refuses (it only
  increments `viewmodel_far`), so the gate is harmless but **the counter is misleading evidence**.
  `VMPAIRMAXDIST` DOES refuse and is inert only because `VMPAIRVP` is blank.
- **STILL OWED** from round 35, none of these were done: `classify_draw`'s "IGNORE is a no-op on the API
  draw path" comment is probably stale and needs confirming by disassembling `bin\remix\d3d9.dll`;
  `RemixTransforms.h`'s "What `DRAWAUDIT=0` costs" list still names neither `m_streak_measured` consumer.
- **NOTE:** a scanner that looks for `env_u32|env_float|GetEnvironmentVariableW` will report ~35 armed knobs
  as "not read by any source file". That is a **false positive** — they are read through
  `parse_bounded_hash_list<T>(L"NAME")` and similar templates. Verified by grepping `L"<NAME>"` directly.
  Do not report it as a finding.

---

## Play-test card

Launch as usual (`launch-haze-remix.cmd` -> `bin\rpcs3-next.exe`, **`F9206BA434AE3ADB`** — hash it, the
previous one was replaced by another session).

| what to look at | where to read it | one-line revert |
| --- | --- | --- |
| **Arms: right way up AND in front of you?** | `Remix vmbasis:` in `bin\remix_dump.log` (readable live). Want `cpost` fwd **> 0**, `cpost` up **< 0**, `relpost` **1..62**, `dcentre` **0.3..1.6**, `drot` ~2, `dbasis` 0 | `set "RPCS3_REMIX_VMROTAXIS=0"` |
| Arms right way up but at the wrong PITCH | same line, `relpost` | `set "RPCS3_REMIX_VMROTDEG=156"` then `118` |
| Arms upside down the OTHER way | the model-space hypothesis | `set "RPCS3_REMIX_VMROTAXIS=4"` + `set "RPCS3_REMIX_VMROTDEG=270"` |
| **Helmet: visible, no shadow, no clipping?** | `Remix stats:` -> `ui_forced_pair=` (hundreds+). **If the helmet VANISHED**, read `ui_skipped` / `ui_space_none` / `ui_render_target` instead | `set "RPCS3_REMIX_UIFORCEPAIRVP="` |
| **Sky in the newly-unlocked levels: still black?** | knobs line -> `sky_emissive_albedos=` must read **6**. Then just look at the sky, per level | `set "RPCS3_REMIX_SKYEMISSIVE=D1A6D1B27ADE6232,CDFE11B12552EA2D"` |
| Sky went from black to BLINDING | that is a success with the wrong intensity | `set "RPCS3_REMIX_SKYEMISSIVEINT=1.0"` |
| A level is STILL black | Ctrl+Click the sky; read `albedo=` on `Remix picked:`. Two list slots are free | n/a — this is the measurement that unblocks it |
| Terrain/ground vanishing at angles | not caused by anything armed this round | `set "RPCS3_REMIX_SKYLEARN=0"` |
| White/invisible walls in the plant | `Remix static-index:` — last read `peak=56 budget=64 dropped=0` | `set "RPCS3_REMIX_STATICINDEXBUDGET=8"` |
