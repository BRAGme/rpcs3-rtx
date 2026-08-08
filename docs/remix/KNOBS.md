# RTX Remix backend tuning knobs

The Remix RSX backend reads a set of `RPCS3_REMIX_*` environment variables. They are the
bisect and tuning surface for the backend: most of them exist so a single run can attribute a
visual change to one specific decision in the transform/material pipeline rather than to a
whole milestone.

## How to set one

Set the environment variable in the shell (or launcher/shortcut) that starts rpcs3, before
launch. Example:

    set RPCS3_REMIX_SKYHASH=2
    rpcs3.exe

A subset is also exposed on the **RTX Remix** page in *Configuration* (see the `in GUI`
column). Where both exist, the environment variable wins when it is set, so existing test
scripts keep working unchanged; the config value is the fallback. A handful of the config
entries are re-read per frame and so take effect without a restart -- the GUI labels those
that do not with "(restart)".

Most environment knobs are latched once per process (`static const`), so changing one requires
restarting rpcs3.

Value conventions:

- Boolean knobs read through `env_flag` are true for any value whose first character is not
  `0`, and false when unset. These cannot express "explicitly off", which is why several of the
  on-by-default knobs are read as integers instead.
- Integer knobs read through `env_u32` fall back only when unset, so `0` is a usable value.
- Float knobs read through `env_float` accept finite positive values only; anything else falls
  back, which is why several of them use a negative sentinel internally to mean "unset".

## Provenance

This list is distilled from the in-code documentation block in
`rpcs3/Emu/RSX/Remix/RemixTransforms.h` (lines 587--1217) plus a repo-wide grep for
`RPCS3_REMIX_*` read sites across `rpcs3/Emu/RSX/Remix/`, `rpcs3/Emu/system_config.h` and
`rpcs3/rpcs3qt/`. It can lag the code. **The code is authoritative** -- when this file and the
source disagree, the source is right.

Defaults shown are the effective default of the whole read path: where a knob falls through to
an `Emu/system_config.h` entry, the config default is what is listed.

---

## Runtime

| name | default | effect | in GUI |
| --- | --- | --- | --- |
| `RPCS3_REMIX_DLL` | empty (`<exe dir>\remix\d3d9.dll`) | Path to the Remix runtime DLL. The runtime is deliberately not placed next to `rpcs3.exe`, because a file named `d3d9.dll` there would be picked up by anything else in the process resolving that name. | |
| `RPCS3_REMIX_FARPLANE` | `1000` | Far plane used by the hardcoded camera, overridable at runtime so a unit-scale sweep needs no rebuild. Values must be greater than 1. | yes |

## Camera

| name | default | effect | in GUI |
| --- | --- | --- | --- |
| `RPCS3_REMIX_CAMHOLD` | `300` | How many consecutive flips the last resolved camera is kept when a frame produces no candidate of its own. `0` restores the unconditional per-frame latch, where a single candidate-less frame dropped the camera and the backend submitted its origin fallback instead. 300 is roughly 5 s at 60 fps. | yes |
| `RPCS3_REMIX_NOCAM` | off | Force the milestone-1 hardcoded camera plus the debug triangle. | |

## Geometry and world placement

| name | default | effect | in GUI |
| --- | --- | --- | --- |
| `RPCS3_REMIX_DRAWNOWORLD` | off | Submit a draw whose world transform could not be resolved at the identity anyway. Off by default because identity means raw model-space vertices at the world origin, i.e. every unresolved draw piled on top of every other one. | yes |
| `RPCS3_REMIX_NOWDIV` | off | Submit the stored `ATTR0.xyz` even for programs whose ucode divides the position by `ATTR0.w`. The bisect knob for the per-vertex divide. | yes |
| `RPCS3_REMIX_STRICTINPUT` | off | Refuse any draw whose position chain never reached the vertex attribute (the "innermost operand is not an attribute" population). Diagnostic only. | yes |
| `RPCS3_REMIX_RTVERTS` | `32` | Vertex ceiling for the 3D render-target-feedback gate: a draw that samples a bound colour/depth surface is treated as a post-process pass only when it is also small enough to be a full-screen quad. `0` disables the shape test and refuses on the address alone. | yes |
| `RPCS3_REMIX_POSAFFINE` | `1` (on) | Honour `vp_fingerprint::has_const_affine` and refuse draws whose recognised `pos = attr * c[S] (+ c[B])` decode could not be rebuilt, instead of silently submitting the raw quantised attribute. `0` restores the older behaviour. | |
| `RPCS3_REMIX_HPOSINDIRECT` | `1` (on) | Reach an HPOS writer through the register it was parked in, instead of requiring each of the four components to be written by a DP4/DPH landing on HPOS directly. `0` discards any 4x4 whose rows did not all land on HPOS. | |
| `RPCS3_REMIX_FULLCHAIN` | `1` (on) | Under a fused active camera, fold the whole matrix chain of a layered program rather than only its outermost group. `0` folds only the outermost group, which drops the inner groups carrying the model and view rows. | |
| `RPCS3_REMIX_PASSSKIP` | `1` (on) | Skip the extra passes of a multi-pass forward renderer. `0` submits every pass, which on a path tracer stacks the lighting pass instead of blending it. | |
| `RPCS3_REMIX_ORTHO2D` | `1` (on) | Honour `vp_fingerprint::has_ortho2d` and apply the 2-row transform of a 2D program in the compositor. `0` drops that transform and composites the raw attribute values as if already projected. | |
| `RPCS3_REMIX_WBUFFERZ` | `1` (on) | Recover the matrix row for a program that writes `HPOS.z` as its clip z premultiplied by its clip w. `0` refuses such programs instead. | |
| `RPCS3_REMIX_LOOSESLICE` | off | Let the backward slice from HPOS collect writers that sit after the instruction whose operand is being traced. RSX vertex programs are straight-line code, so this is the bisect knob for the ordering constraint. | |
| `RPCS3_REMIX_BASISAFFINE` | `1` (on) | Apply the object placement `match_basis_affine` recognises. `0` restores the behaviour where indexed-palette characters reached the outer group with no placement and landed at the camera group's origin. | |
| `RPCS3_REMIX_INDEXEDWORLD` | `1` (on) | Offer the register indirection to programs that read a constant palette through the address register, let `match_indexed_affine` express a 3-row indexed group with a post-matrix translation, and let `match_mad_chain` accept a partial writemask or non-unit stride for an indexed group. `0` restores all three refusals at once. | |
| `RPCS3_REMIX_INDEXEDBIASREG` | off | Let `match_indexed_affine` express an indexed group whose rows are written into one register and moved into another by the post-matrix translation, instead of requiring the translation to be in place. | |
| `RPCS3_REMIX_INDEXEDUNIFORM` | `0` (off) | Submit a draw whose indexed constant reads are provably the same constant for every vertex. `0` off; `1` release only programs whose matrix chain also reaches the vertex attribute; `2` release every program with a uniform address register. | |
| `RPCS3_REMIX_DRAWINDEXED` | off | Submit draws whose position chain really does read a constant through an address register instead of refusing them. Diagnostic -- the refusal exists because such a draw renders in its bind pose or torn across the map. | |
| `RPCS3_REMIX_CULL` | off | Submit `doubleSided=0` for draws whose RSX cull state is enabled, instead of forcing every instance double-sided. Off by default because a wrong winding in the strip/fan/quad expansion turns a single-sided surface invisible. | yes |
| `RPCS3_REMIX_MESHCAP` | `0` (uncapped) | Cap the live mesh-handle cache at N entries with LRU eviction. `0` leaves it unbounded, which is what the idle-frame rule alone gives. | yes |
| `RPCS3_REMIX_MESHIDLE` | `300` | How many frames a mesh handle survives after the last draw that referenced it before `reap_idle_meshes` destroys it. Raising it trades residency against repeated BLAS builds. `0` reaps a mesh the first frame it goes unreferenced. | yes |
| `RPCS3_REMIX_VTXSPREAD` | `64` | How many times the median a draw's furthest decoded vertex may sit from the draw's own median position before `audit_vertex_extent` reports it. `0` disables the pass. Diagnostic only. | |
| `RPCS3_REMIX_VTXREFUSE` | off | Drop a draw that `audit_vertex_extent` called geometrically incoherent instead of submitting it. An A/B, not a fix. | |
| `RPCS3_REMIX_STREAKGATE` | `128` | How many times the frame's own median world extent a draw's post-transform bounding box may span before `audit_world_extent` refuses it. Measured against the frame's median, so it needs to know nothing about the title's units. `0` restores the behaviour where nothing measured the geometry Remix actually receives. | |

## Skinning

| name | default | effect | in GUI |
| --- | --- | --- | --- |
| `RPCS3_REMIX_NOSKIN` | off | Skip every skinned draw instead of submitting bone transforms. | |
| `RPCS3_REMIX_SKINID` | off | Submit identity bone transforms with mesh, indices and weights unchanged. Geometry rigid and correctly placed means the matrices are at fault. | |
| `RPCS3_REMIX_SKINBONE` | unset | Clamp every dense bone index to n. Mesh rigid means the palette read is fine and the index decode is at fault. Unset is a distinct sentinel so `0` stays a usable value. | |
| `RPCS3_REMIX_SKINRAW` | off | Feed the raw (un-scaled) attribute value to `evaluate_bone_offset` instead of the scaled one. | |
| `RPCS3_REMIX_BONEBLEND` | `1` (on) | Let `match_blend_palette` express summed accumulators, let `resolve_bone_index` follow the x3 index scaling blend programs build by repeated self-addition, and let the indexing audit accept the four matched address components. `0` refuses every program that blends more than one palette entry per vertex, restoring all three at once. | |
| `RPCS3_REMIX_BONESCALE` | `1` (on) | Refuse a blended draw when one of its bones is orders of magnitude away from the median of the others in the same draw. `0` submits it anyway; the gate exists because a 3-row palette otherwise reaches Remix with no magnitude check at all. | |
| `RPCS3_REMIX_BONEUNIFORM` | `1` (submit) | Submit a blended draw whose palette entries are all the same matrix -- such a palette blends to exactly that matrix and the draw is a correct rigid draw. `0` refuses that population, as a one-run A/B. | |
| `RPCS3_REMIX_SKINREACH` | `32` | How many times the median a skinned draw's furthest vertex-to-its-own-bone distance may be before `audit_skin_extent` reports it. `0` disables the pass. Diagnostic only -- nothing is ever refused on it. | |

## Materials and textures

| name | default | effect | in GUI |
| --- | --- | --- | --- |
| `RPCS3_REMIX_NOTEX` | off | Disable the whole texture workstream. | yes |
| `RPCS3_REMIX_NOUV` | off | Submit world geometry with texcoord `(0,0)` on every vertex, so a bound albedo material produces one flat colour per draw. The bisect knob for the UV fetch. | |
| `RPCS3_REMIX_UVATTR` | `0` (automatic) | Force the vertex attribute the albedo texcoords are read from, instead of the 8+unit convention and its fallback scan. Accepts 1..15 when forced; `0` stays refused because that is the position attribute. | |
| `RPCS3_REMIX_UVUCODE` | `1` (on) | Read the texcoord attribute out of the vertex program (`vp_fingerprint::texcoord_input`). `0` ignores it and picks with the size/type heuristic alone. | |
| `RPCS3_REMIX_UVWIDE` | `1` (on) | Widen the automatic texcoord scan past attributes 8..15 to referenced attributes 1, 2, 4, 5, 6, 7 (never 0/position, never 3/colour, never the skinning attribute), taking only 2-component streams that decode finite. `0` restricts the scan to 8..15. | |
| `RPCS3_REMIX_UVFLIPV` | off | Submit `1-v` instead of `v` for every texcoord, on both the 3D and the 2D path. A per-title knob for a per-title defect: `v = 0` is row 0 of the decoded texture on RSX, so a global flip would invert every title whose coordinates are already right. | yes |
| `RPCS3_REMIX_UVINTSCALE` | `4096` | Divisor for S32K (raw 16-bit integer) texcoords, whose real divisor is a vertex-program constant this backend does not read. `0` submits them raw. | |
| `RPCS3_REMIX_FPALBEDO` | `1` (on) | Pick the albedo unit from `fp_fingerprint::colour_mask`. `0` picks the lowest referenced, enabled 2D unit with a retry loop free to walk to any other referenced unit -- which is what puts a normal map in the albedo slot. | |
| `RPCS3_REMIX_NOALPHA` | off | Create materials with a fixed always-pass alpha state (`alphaTestType 7`, `useDrawCallAlphaState 1`) instead of the title's own alpha test. The bisect knob for cutout foliage. | yes |
| `RPCS3_REMIX_NOVCOL` | off | Leave every submitted vertex colour at `0xFFFFFFFF` instead of reading ATTR3 for draws that resolved no material. The bisect knob for vertex-coloured geometry. | yes |
| `RPCS3_REMIX_BLENDSTATE` | `1` (on) | Set `useDrawCallAlphaState = 1` on the material and carry the per-draw blend and alpha-test state on the instance (`remixapi_InstanceInfoBlendEXT`). `0` restores every material declaring its own alpha state with no blend chained onto the instance, so every draw reached the runtime fully opaque. It is one switch, not two, because the runtime reads `useLegacyAlphaState` once for both halves. | |
| `RPCS3_REMIX_TEXBUDGET` | `8` | Cap on `CreateTexture` calls per frame. Anything over budget submits with a null material for that frame and renders untextured until a later frame lets it through. `0` is unlimited. Read live. | yes |
| `RPCS3_REMIX_TEXIDLE` | `300` | How long a decoded texture and its Remix handles survive after the last draw that bound them, in frames. Raising it trades VRAM against re-decoding and re-creating. The config floor is 30, but the environment variable can express `0`. Read live. | yes |
| `RPCS3_REMIX_TEXLINEAR` | off | Upload textures as UNORM instead of SRGB. | |
| `RPCS3_REMIX_TEXREHASH` | `0` | Texture staleness policy: `0` descriptor only, `1` sampled fingerprint, `2` full hash every draw. Above 0 costs heavy mesh churn, because the albedo hash is folded into the mesh key. Animated textures go stale at 0; 1 or 2 buys them back. | yes |

## Sky and viewmodel classification

| name | default | effect | in GUI |
| --- | --- | --- | --- |
| `RPCS3_REMIX_SKYEXTENT` | `2000` | Minimum world-unit span in the widest axis for a depth-write-off, camera-anchored draw to be tagged SKY. `0` disables the detection. | yes |
| `RPCS3_REMIX_SKYANCHOR` | `4` | How far the draw's own origin may sit from the camera and still be a sky candidate. `0` disables the anchor requirement and restores the extent-only rule. The distance is measured against the resolved camera, so a draw with no world transform is refused, not guessed. | |
| `RPCS3_REMIX_SKYBACKDROP` | `1` (measure only) | The separate backdrop rule -- world extent past `SKYEXTENT`, camera inside the draw's transformed bounding box, and a minimum extent-per-vertex. `0` off, `1` measure only (counts `sky_backdrop_hit` / `sky_backdrop_dw`, changes nothing on screen), `2` measure and tag SKY. | |
| `RPCS3_REMIX_SKYHASH` | `1` (measure only) | Identify the sky by albedo content hash rather than by geometry, using geometry only to learn which hashes belong to a dome. A hash is armed after enough consistently dome-shaped draws and is disqualified permanently by a single draw that is not. `0` off, `1` measure and census only, `2` measure and tag SKY. | |
| `RPCS3_REMIX_SKYTEXTURED` | `1` (on) | Allow a textured draw to be a sky candidate. `0` requires a sky candidate to be untextured, which was written against a vertex-coloured dome and does not generalise. | |
| `RPCS3_REMIX_VIEWMODEL` | `2` (tag) | First-person arms/weapon detection by viewport depth range. `0` off, `1` measure and census only, `2` measure and tag `VIEW_MODEL`. | |
| `RPCS3_REMIX_VIEWMODELCAM` | `2` (transform) | Which reference `per_draw_transform` divides a viewmodel draw by. `0` uses the world camera's reference for every draw; `1` counts viewmodel draws and drops them; `2` uses a reference latched from the viewmodel population itself, refusing until one has been latched. Separate from `VIEWMODEL`, which only names the population. | |

## Instance categories

Comma-separated 16-hex-digit albedo *content* hashes -- the same values the `Remix tex=` dump
line and the Remix dev menu display. Setting `categoryFlags` at submit time is the only
mechanism that reaches a draw on the `submitExternalDraw` path; Remix's own `rtx.*Textures`
conf lists are matched on the D3D9 path only, which is why tagging a texture in the dev menu
does nothing for this backend.

| name | default | effect | in GUI |
| --- | --- | --- | --- |
| `RPCS3_REMIX_CAT_SKY` | empty | Tag the listed hashes SKY -- selects the sky camera and hides the instance. | yes |
| `RPCS3_REMIX_CAT_HIDE` | empty | Tag the listed hashes HIDDEN. (IGNORE is a no-op for API draws and is not used.) | yes |
| `RPCS3_REMIX_CAT_PARTICLE` | empty | Tag the listed hashes PARTICLE. | yes |
| `RPCS3_REMIX_CAT_DECAL` | empty | Tag the listed hashes DECAL_STATIC. | yes |

## Lighting

The camera-parked debug sphere blows out everything near it and crushes everything far, so the
readable default is a distant sun; the sphere stays as an optional fill, off by default. The
sun direction is in the recovered world space and is normalised in code.

| name | default | effect | in GUI |
| --- | --- | --- | --- |
| `RPCS3_REMIX_NOSUN` | off | No default sun at all. The environment can force this on but not off; the config is the way to re-enable the sun once a script has disabled it. | yes |
| `RPCS3_REMIX_SUNDIR` | `-0.3509,-0.9023,-0.2506` | Sun direction as `"x,y,z"` in recovered world space, normalised in code. | yes |
| `RPCS3_REMIX_SUNRADIANCE` | `3` | Distant-light radiance. The shader divides a distant light's radiance by `sin^2(halfAngle)` and samples the cone uniformly, so the delivered irradiance is roughly `pi * radiance` regardless of angular diameter -- single digits are the useful range, not the sphere light's 100. | yes |
| `RPCS3_REMIX_SUNANGLE` | `0.5` | Sun angular diameter in degrees. | yes |
| `RPCS3_REMIX_CAMLIGHT` | `0` (off) | Radiance of the camera fill light. The measured useful range is 8..40. `env_float` rejects non-positive values, so an explicit `0` from the environment cannot be told from unset -- use the config entry to turn it off. | yes |
| `RPCS3_REMIX_LIGHTRADIUS` | `0.1` | Radius of the debug sphere light, so a derived camera can be judged visually. | yes |
| `RPCS3_REMIX_LIGHTRADIANCE` | `100` | Radiance of the debug sphere light. | yes |

## 2D UI compositor

| name | default | effect | in GUI |
| --- | --- | --- | --- |
| `RPCS3_REMIX_NOUI` | off | Disable the compositor entirely. | yes |
| `RPCS3_REMIX_KEEP_UI` | off | Disable the screen-space skip (bisection). | |
| `RPCS3_REMIX_KEEPRT` | off | Composite the title's own render-target blits (its post-process chain) through the CPU rasterizer. Off by default: those draws are not UI, they cost a full-screen fill each, and they are what held one test title at 1.9 FPS. | |
| `RPCS3_REMIX_UIWIDTH` | `1920` | Ceiling on the compositor buffer's width in pixels; the height follows the window's aspect. `0` removes the cap. Everything the rasterizer draws is authored at the guest's own surface resolution or at a virtual 1280x720, so more pixels buy no detail. | yes |
| `RPCS3_REMIX_UIPROBE` | off | Draw a fixed known pattern through `DrawScreenOverlay` instead of judging the call for the first time with real UI data flowing through it. | |
| `RPCS3_REMIX_UISPACE` | `1` (on) | Derive the guest window-space y convention from the viewport registers the guest programmed (RSX window space is y-down exactly when `viewport_scale_y` is negative). `0` restores the clip-pixel row conversion that hard-coded "guest pixel y=0 is the top row". Not a flip: substituting a y-down title's own registers reproduces the old expression exactly. | |
| `RPCS3_REMIX_UIDUMP` | `0` (off) | Log the geometry of the first N textured UI draws: screen bbox and the raw `(x,y,u,v)` of the first triangle. A glyph batch whose per-quad UVs span the whole 0..1 atlas instead of one glyph cell is the overlapping-text signature. | |
| `RPCS3_REMIX_UIDUMPVP` | `0` (unset) | 16-hex vertex-program hash. Narrows the UI dump to one program, logs every quad rather than the first triangle, and writes that draw's albedo texture out as a BMP once. | |

## Diagnostics

| name | default | effect | in GUI |
| --- | --- | --- | --- |
| `RPCS3_REMIX_DUMP` | off | One log line per unique vertex program, plus the rest of the draw census. A debug mode with a heavy per-frame cost. | yes |
| `RPCS3_REMIX_TEXBMP` | `1` (on) | Alongside the `Remix tex=` census line, write each unique decoded texture out as a BMP under `remix_tex\`. Only reached when `DUMP` is already on; `0` turns it off without turning the rest of the dump off. | |
| `RPCS3_REMIX_SKIPVP` | `0` (unset) | 16-hex vertex-program hash. Drop every draw of that one program -- the one-run bisector for "which program draws that". | |
| `RPCS3_REMIX_WORLDVP` | `0` (unset) | 16-hex vertex-program hash. Print the world transform that one program is being given, once per stats window, to `RPCS3.log` and `remix_dump.log`: which branch of `per_draw_transform` built it, and the L1 residue of the perspective row `to_remix_transform` is about to truncate away. The companion to `SKIPVP`. | |
