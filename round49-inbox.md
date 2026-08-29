# Round 49 inbox — RPCS3 RTX Remix backend, Haze (BLUS30094)

Written at the close of round 48, 2026-08-29. Branch `remix-backend`.
**HEAD did NOT move: `7810224be` at the start and at the end.** Nothing committed, stashed or checked out.

| artefact | SHA256 (first 16) | note |
| --- | --- | --- |
| `bin\rpcs3.exe` | **`5B6B6396A7A8FE50`** | built this round, MSBuild `Release\|x64` exit 0, **0 errors** |
| `bin\rpcs3-next.exe` | **`5B6B6396A7A8FE50`** | **identical — the copy was made.** This is what the launcher starts. |
| `bin\remix\d3d9.dll` | `16A0B512F33EBB66` | **UNCHANGED.** Not opened. |
| `bin\rtx.conf` | `615E71138D151A96` | was `B24C0FA70F098F98`; **the runtime moved it during play.** Read only, never written. |

One warning from anything under `Emu\RSX\Remix\`: the pre-existing C4723, moved `RemixGSRender.cpp:11799` →
**`:11922`** by exactly the 123 lines inserted above it. **Zero new warnings.**
Full derivations in `docs\remix\KNOBS.md`, **"Round 48"** (line **1161**; Round 47 moved down to 1414).

**I verified the deployed exe contains this round's code by string search, not just by hash** — the UTF-16
env names and the ASCII census fields `guest_light_vmref=` / `guest_light_unstable=` / `guest_light_cells=`
are all present in `bin\rpcs3-next.exe`. Round 47's "the file the launcher runs is not necessarily the file
you built" is now checked directly.

**Launcher line numbers, re-grepped at the close — never trust a quote:** `GUESTLIGHTIDLE` `:850`,
`DRAWAUDIT` `:1319`, `GUESTLIGHTAUTO` `:1568`, `GUESTLIGHTSTABLE` `:1584`, `SKIPEXTENTVP` `:70`,
`SKIPEXTENTMIN` `:71`, `SMOOTHNORMALS` `:266`, `STATICINDEXBUDGET` `:405`, `GUESTLIGHTMAX` `:859`,
`GUESTLIGHTRADIANCE` `:802`, `DEFERVIEWMODEL` `:1224`, `DEFERPREANCHOR` `:1122`.

---

## READ THIS FIRST — round 47's two changes both WORKED and neither fixed its symptom

Round 47 shipped two things. Both hit their pre-registered pass criteria exactly. Both symptoms survived.
**Do not re-attack either mechanism; they are now measured to be innocent.**

MEASURED, round-47 play-test, `build=Aug 29 2026 09:22:04`, **83,019 flips**.

**A. `DEFERVIEWMODEL=1`** — `defer_projsplit=189481/189466/15`. `held=189,481` equals `vm_tagged` exactly, so
**100%** of the viewmodel population is now deferred and **99.99%** was re-crossed. `vm_op=189481/0/0/189466`
— `declined=0`, `spilled=0`, `replayed` non-zero for the first time. Every reading passed.
**The user still reports the weapon and arms shaking in the smelting plant.** The proj_split re-divide is
therefore **not** the shake. Three rounds have aimed here; go somewhere else.

**B. `STATICINDEXBUDGET=512`** — `dropped=0`, and that is **the only value across all 856 census lines**
(baseline 5,521 over 52,208 flips). `peak=177`, `stale=0` (was 62). The pre-registered threshold was an 80%
fall; the fall is 100%. **The user still reports the floor missing and walls disappearing in the hallway**,
so the static-index drop was **not** that mechanism either. Keep the budget (it costs nothing at
`peak=177 < 512`), drop the hypothesis.

> **`peak` DID judge it, and round 47 said it could not.** `peak <= budget` is a structural invariant, so
> `peak == budget` is a tautology — that part was right. But **`peak < budget` strictly is a proof the clamp
> was never reached**, and `peak=177` is exactly that, while `177 > 128` proves the old cap was binding.
> **A clamped counter is blind at its ceiling and informative below it.** Round 47 retired the whole
> instrument for a defect affecting one of its readings.

---

## PLAY-TEST CARD — two independent changes, and they cannot be confused

### A. The glow-card lights — `GUESTLIGHTAUTO=2` + `GUESTLIGHTSTABLE=30` + `GUESTLIGHTIDLE=4`

This is the user's request, both halves, and they are **one mechanism**. Haze draws an additive glow card
over a fixture that is lit and omits it for one that is not, so keying the light on the card gives
"only the ones actually lit" and "goes out when the bulb flickers off" from the same rule.

Round 41 shipped this and had to disable it for two faults. **Both are fixed in code this round.**

| reading | where | meaning |
| --- | --- | --- |
| **`guest_lights=`** | `Remix live:` | **THE pass/fail.** Was **0** all of round 47. Non-zero means lights are being created at all. |
| `guest_light_match=` | same | how often the rule fired. **If this is 0, everything downstream is 0 for an upstream reason — do not read the new gates as "working".** |
| `guest_light_vmref=` | same | **NEW.** AUTO candidates refused for being the player's viewmodel (fault 1). |
| `guest_light_unstable=` | same | **NEW.** Candidates that had not held still for 30 frames (fault 2). |
| `guest_light_cells=` | same | **NEW.** Staging-table occupancy against a 4096 ceiling. **Pinned at 4096 = the prune runs every frame and the gate is degraded.** |
| `guest_light_reaped=` | same | **must now be large and climbing.** 0 means `GUESTLIGHTIDLE` is inert and **there will be no flicker.** |
| `guest_light_capped=` | same | climbing = the rule over-matches. That is the **pre-registered refutation** of the glow-card hypothesis. |
| **the eyes** | — | **does the flickering bulb's light flicker with it, and do unlit fixtures stay dark.** No log needed. |

**What the fixes are, so you can judge them rather than re-derive them:**

* **Fault 1 (arms/weapon lit, spheres a metre in front of the eye) is ONE fault, not two.** A viewmodel draw
  satisfies every term of the glow-card test, and the position derived from it is the AABB centre of a
  *viewmodel* transform. A world-geometry gate on the AUTO arm fixes both symptoms at once. It uses the
  submit site's own `is_viewmodel` local, **deliberately not** `m_scratch_defer_is_viewmodel` — that member
  is set inside the `vm_op` capture block and reads false at `VIEWMODELCAM` 0 and 1, which is exactly the
  "silently inert at a reachable knob value" defect rounds 46 and 47 both shipped.
* **Fault 2 (soldiers lit as they walked)** cannot be caught by that gate — an NPC is ordinary world
  geometry. **A lamp does not move.** `GUESTLIGHTSTABLE=30` requires the same 0.25-unit cell on 30 distinct
  frames. An NPC leaves its cell in 2–4 frames and never graduates.

**MEASURED support that fault 1 is real and in this exact population:** of the 387
`Remix light-candidate:` lines in the round-47 slice (all 387 are `state=glowcard`; 14 albedos; 17 vp/fp
pairs), **`vp=f39f504649b6f442` appears 20 times — that is the launcher's own `hidepairvp`, the player's
body/legs.** Character geometry is provably inside the population AUTO=2 triggers on.

**THE BRIEF'S "FLICKER FALLS OUT FOR FREE" WAS WRONG, and this is the trap to remember.**
`GUESTLIGHTIDLE` shipped at **0 = keep every light forever**. A card can stop being drawn indefinitely and
the light stays lit. Flicker is impossible at 0 no matter how good the card keying is. Armed at 4.

**The two knobs interact and would deadlock if handled naively.** `STABLE` decides what may ever light;
`IDLE` decides when a light dies. A bulb that flickers off loses its light to `IDLE`, and with a 30-frame
apprenticeship to re-serve it could never return inside a flicker period. **Confirmation is therefore sticky
for the level**, so re-lighting is immediate. If you change either knob, trace one full flicker cycle
through both before shipping.

**REVERT, in order of blast radius (all one line each):**
`set "RPCS3_REMIX_GUESTLIGHTAUTO=0"` kills the whole feature.
`set "RPCS3_REMIX_GUESTLIGHTSTABLE=0"` keeps the lights but restores round-41 first-sight behaviour.
`set "RPCS3_REMIX_GUESTLIGHTIDLE=0"` keeps the lights but removes the flicker.

**`5003EB1597BF9DB0` WILL NOT LIGHT, and that is expected.** The user named it as *"bulbs that should cast
light and do not"*. MEASURED: it reads **`blend=0 dw=1`** — opaque and depth-writing, `vtx=18`,
`ext=0.4302` — so it is `state_fixture`, **not** a glow card, and mode 2 is glow-cards-only by construction.
If it stays dark, the finding is "Haze draws no card for it", which is a question about whether it is lit in
the raster original at all. **Do not fall back to `GUESTLIGHTAUTO=1` to catch it** — mode 1 is the
whole-census population that put lights on doors and sheet-metal covers in August.

### B. `DRAWAUDIT=0` — the ~35% frame-time item, decoupled in code

`audit=24.76 ms` of a `frame_ms=69.78` frame — **35.5% of the frame, 36.9% of `draw`**, larger than `decode`
and larger than `rest`, and **more than double** the 11.35 ms round 34 measured. Pure diagnostics.

Round 34 refused to ship `DRAWAUDIT=0` because the `SKIPEXTENTVP` refusal reads `m_streak_measured`, written
only inside `audit_world_extent`. **The weld is now cut in code:** the audit also runs whenever the current
vertex program *is* the `SKIPEXTENTVP` program, whatever `DRAWAUDIT` says. That preserves the gate **by
construction** for the one hash it can ever fire on, and everything else stops paying. Nothing is traded.

Supporting measurements over 83,019 flips: `wext_refused=0` and `skipg_extent=0` — the latter despite that
program being drawn (three glow-card census lines name `vp=57a12323f22f4988`), so the gate is live and simply
never met its 128-unit threshold.

| reading | meaning |
| --- | --- |
| `audit=` on `Remix timing:` | must fall hard. It is the whole point. |
| **`rest=`** | **the pre-registered tell.** `extent_plausible` becomes unconditionally true with the audit off, so more draws pay for the bbox walk in `maybe_inject_guest_light` — which sits in **no named timer** and lands in `rest`. `audit` down and `rest` up by a similar amount = the saving was eaten by item A's population. |
| `skipg_extent=` | should stay 0, as before. Non-zero would mean the gate now fires — interesting, not a failure. |

**REVERT: `set "RPCS3_REMIX_DRAWAUDIT=1"`.**

### ATTRIBUTION between A and B

They share no counter. A moves `guest_lights=` and is a **pixel test**; B moves `audit=` and has **no visual
effect at all**. The one confound is frame rate, and it has the tell above: if fps does not improve and
`rest` climbed while `audit` fell, that is A's bbox walk eating B's saving — **revert A first, not B.**

---

## PRIORITY 2 was already done, and the answer is "it is not the lever"

The brief asked for a knob setting `REMIXAPI_INSTANCE_CATEGORY_BIT_SMOOTH_NORMALS`. **It already exists and
is already armed** — `RPCS3_REMIX_SMOOTHNORMALS=1` at launcher `:266`, set in `classify_draw()`.

**MEASURED: `cat_smoothnormals=10167672`, and the total submitted instance count on the same line
(`xform_mirrored=0/0/10167672`) is the identical number.** The bit is on **100%** of submitted instances,
weapons included, and the user still reports blocky weapons.

**Tell the user:** their `rtx.conf` `rtx.smoothNormalsTextures` list *is* inert on this backend (one
consumer, `src/d3d9/d3d9_rtx.cpp`, which external draws never reach) — but the replacement has been shipped
and running at full scope for several rounds. Smooth normals changes shading interpolation only; it **cannot**
add silhouette vertices, so it can never fix blockiness on a low-poly PS3 weapon. That needs mesh
replacement. **No further work on this is worth doing.**

> **Fourth confirmed instance of the D3D9-path trap** (after sky rasterization, `ignoreTextures`, terrain
> baking). New corollary: **also grep this backend for a knob that already replaces the inert one** — this
> one had been shipped for rounds and the brief did not know.

---

## Priority-3 picks resolved

* `1BF8325ADEF3C986` (helmet, clips through geometry) — 1,365 lines. `vtx=1446`, `ext=3.50793`,
  `eye_dist=1.65711`, `dw=1 blend=0`, `verdict=opaquealias`, `cat=0x1000000` (bit 24 = SMOOTH_NORMALS,
  confirming the category reaches it). Clipping is a **depth/near-plane** question, not a placement one.
* `88046E3F4F28137C` (light-bulb ray) — only 12 lines, and it is in the **sun-card / sky** family, not the
  fixture family: `Remix suncard: ... elev=3.56 deg pinned=0 elected=0` plus `sky-census: reject:anchor`
  with `rawext=1.74e+09`. Round 47 filed it under Priority 2 (`ref=camera`, no anchor); that is still true,
  but note it is a sun candidate, so `DEFERPREVONLY` is not obviously its owner.
* `6575ACE3A42A78E6` (solid yellow block) — 409 lines, **all `Remix uiwrap:`**, `vp=6f76ab0ad8d926b1
  fp=4a1bc009fc6161ef`, `tex=512x512`, `route=2d`, `shrink=50`. **It is a UI 2-D route draw.** Round 47
  measured `areason=chain` and correctly retired the `wdivide` theory; this narrows it to the UI compositor's
  2-D path specifically.

---

## Still staged, still unjudged

* **`DEFERPREVONLY=0`** (launcher **`:1146`**, re-grepped at the close of round 48; it was `:1127` in the round-47 brief). Unchanged and still the right next
  knob once item A is judged. `ref=camera` is 19.1% of trace samples, displaced 100% of the time;
  `defer_absent_declined=412,217` over 83,019 flips = **5.0 draws/frame**. One knob per round.

## Carried, unchanged

* **The shake is level-specific (copper/smelting plant only) and is NOT the proj_split re-divide** — that is
  now measured, see above. Needs a new hypothesis.
* *"At a very small angle, the scene goes haywire with swinging"* — still uninvestigated. Needs a
  world-geometry instrument binned by camera yaw, which does not exist.
* **Floor missing / walls disappear in the hallway — and it is NOT the static-index budget** (`dropped=0`).
* Explosions do not render (fire does). Smoke and the nectar gas grenade do not render.
* **Glass is clear, not refracting** — the user's own read is that it is *one light behind the glass* rather
  than a refraction effect. Worth checking before implementing refraction. **Item A may change this**, since
  it is the first round in which fixture lights actually exist.
* The gun's live ammo counter digits do not render on the weapon model.
* Nectar disruption: no blood on hands; the alpha card behind the UI text still shows.
* Low-resolution textures `01C09BE5851AD069`, `AFE89AA51E682B49`.
* ADS collapse root-caused in round 43, not fixed. `UIWIDTH` still 1280; the user has not chosen.
* **"White triangular spikes on an NPC" are HUD damage indicators**, rendering correctly.
* `gauge_contested=11437` this run — **not 0**. Round 46's retraction holds; never cite it as 0.
* **`Remix albedo-trace:` and pick `ref=` fields sit UPSTREAM of the deferral.** Do not pre-register a trace
  reading to judge a deferral knob.

---

## Tooling left behind, in `...\scratchpad\r48\`

* **`run48.log`** — 99 MB slice of the round-47 play-test (`build=Aug 29 2026 09:22:04`, `flips=83019`).
  `tail.log` is the raw 250 MB tail it came from.
* **`live48.txt`** — the 886 `Remix live:` lines. **`si48.txt` / `si48_rpcs3log.txt`** — the 856
  `Remix static-index:` lines; the RPCS3.log copy is the one with timestamps.
* **`lc.txt`** — the 387 `Remix light-candidate:` lines, i.e. the exact population item A acts on.
* **`build48.log`** — the MSBuild run. **`*.bak48.*`** — pre-edit copies of all five edited files, so this
  round's diff can be isolated from rounds 46-47's uncommitted work with a plain `diff`.
* **`KNOBS.bak48.md`** — pre-insert copy; the doc edit was verified **+253 / −0, insert-only**.
* **`binBASE.cmd` / `binNEW.cmd`** — byte-preserving dry-run copies of the pristine and edited launcher, with
  only the `start` line neutralised. **This is the control that proved the launcher's console errors are
  pre-existing.** Reuse the pattern for any launcher edit.
* `..\r47\posaudit.py` — the by-POSITION format audit. Point it at a new field name; this round it confirmed
  381 specs / 381 args with the three new counters at exactly slots 88-90.
* `..\r45\fmtaudit45.py` — **109 `fmt::format` calls, 0 mismatches** after this round's edits.
  **`scratchpad\r43\audit.py` gives false passes. Do not use it.**

## Instrument notes worth keeping

* **A clamped counter is blind at its ceiling and informative below it.** Corrects round 47's blindness #7.
* **"It falls out for free" is a claim about a lifecycle, and lifecycles have defaults.** The flicker was
  assumed free; `GUESTLIGHTIDLE=0` meant nothing ever destroyed the light. **Read the default of every knob
  the free behaviour depends on.**
* **Two gates that each fix half a fault can deadlock on each other.** Trace one full cycle through both.
* **Bound any table keyed on the thing you are trying to reject** — its worst case is its rejection
  population.
* **Check whether the feature you were asked to build already exists.** Priority 2 cost nothing this round
  only because the counter was read before the code was written.
* **Always run the control before blaming your own diff.** The launcher's console errors looked like mine
  until the pristine backup produced byte-identical ones.
* **`bin\log\RPCS3.log` is where `Remix timing:` and `Remix static-index:` live** — not `remix_dump.log`. It
  survives only until the next launch. `remix_dump.log` is append-mode and now **1.41 GB**; find run
  boundaries with `tail -c 250000000 remix_dump.log | grep -abo "Remix run-start:"`, add the tail offset,
  then `tail -c +N` once into the scratchpad.
