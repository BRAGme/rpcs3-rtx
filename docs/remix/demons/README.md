# Demon's Souls (BLUS30443) — authored lighting, extracted from the game's own DrawParam

`des_lights.json` is every per-level lighting preset From Software authored, read straight out of
the disc. Nothing here is inferred except the angle→vector convention, and that is **verified**
(below). Regenerate with:

    python docs/remix/demons/deslights.py docs/remix/demons/des_lights.json

## Where it comes from

The disc is shipped **unpacked**, so no archive extraction is needed:

    PS3_GAME/USRDIR/param/drawparam/<level>_<bank>.param
    PS3_GAME/USRDIR/paramdef/<bank>.paramdef

`desparam.py` reads From Software's PARAM/PARAMDEF pair (big-endian on PS3; layouts are documented
in that file's header). The paramdef gives field names, types and sizes, so the rows decode by name
rather than by offset guessing — `LIGHT_BANK` is 54 fields / 224 bytes, and 224 is exactly the row
stride the PARAM's own row table implies, which is the check that the layout is right.

Four banks matter:

| bank | what it holds |
| --- | --- |
| `LIGHT_BANK` | **3 directional lights** (`degRotX/Y_0..2`, RGB 0-255, `colA` = intensity **percent**), a specular light (`_s`), and the hemisphere ambient — `colA_u` up, `colA_d` down |
| `LIGHT_SCATTERING_BANK` | **the sky**: `sunRotX/Y`, sun RGB + percent, Rayleigh (`lsBetaRay`), Mie (`lsBetaMie`), `lsHGg`, in-scattering and distance multipliers |
| `FOG_BANK` | `fogBeginZ`, `fogEndZ`, colour + percent |
| `POINT_LIGHT_BANK` | `dwindleBegin/End` (radius), colour + percent |

Each level file carries **64 preset rows**, named in Japanese for the sub-area they light
(`神殿部分` shrine section, `B0ファランクスの床` Phalanx's floor, `天球` skydome, `屋内（奥）` indoor deep …).

## The angle convention — VERIFIED, not assumed

    travel.x = cos(degRotX) * sin(degRotY)
    travel.y = -sin(degRotX)
    travel.z = cos(degRotX) * cos(degRotY)

i.e. start pointing horizontally, pitch **down** by `degRotX`, yaw by `degRotY`; the result is a
TRAVEL vector (light → scene), matching `m_sun_light_travel`.

The check: a live Demon's Souls run reported

    Remix sun-submit: travel=[-0.60402 -0.34202 0.71985] radiance=3 angle=0.5 aimed=1 draw=SUCCESS

and `m08` preset row 0 (`B0基本`) has `sunRotX=20, sunRotY=-40`, which this formula turns into
**`[-0.60402, -0.34202, 0.71985]`** — identical to five decimal places. An integer-degree pair
reproducing a runtime float to 5dp is not a coincidence, so the convention is right **and** the
backend's own sun recovery already lands exactly on the authored value. It also identifies which
level that run was in: an `m08` map.

## Per-file summary (preset row 0)

| file | row-0 name | sun angle X,Y | sun % | sun travel | hemi up / down |
| --- | --- | --- | --- | --- | --- |
| m01 | 神殿部分 | 55, 0 | 200 | `[0, -0.819, 0.574]` | 0.05 / 0.07 |
| m02 | B0基本 | 30, 70 | 200 | `[0.814, -0.5, 0.296]` | 0.086 / 0.086 |
| m03 | B1基本 | 50, 110 | 100 | `[0.604, -0.766, -0.220]` | 0.15 / 0.15 |
| m04 | B0基本 | 35, -73 | 70 | `[-0.783, -0.574, 0.240]` | 0.03 / 0.03 |
| m05 | オブジェ　明 | 60, -90 | 100 | `[-0.5, -0.866, 0]` | 0.05 / 0.05 |
| m06 | B0外観 | 10, 47 | 83 | `[0.720, -0.174, 0.672]` | 0.05 / 0.05 |
| m07 | 基本 | 9, 0 | 120 | `[0, -0.156, 0.988]` | 0.044 / 0.161 |
| m08 | B0基本 | 20, -40 | 200 | `[-0.604, -0.342, 0.720]` | 0.05 / 0.05 |
| m99 | 基本 | 20, -50 | 500 | `[-0.720, -0.342, 0.604]` | 0.88 / 0.5 |

The `mNN` prefix maps 1:1 onto the map-id prefix the game loads (`map/mNN_XX_00_00`). Retail world
names are deliberately **not** asserted here — the row names identify the sub-area, and that is what
a lookup needs.

## Why this matters, in one number

A measured Demon's Souls run reports `guest_lights=0`, `guest_lights_created=0`, one synthesised
sun, and `bin\rtx.conf` sets `rtx.fallbackLightMode = 0`, which is `FallbackLightMode::Never`
(`dxvk-remix/src/dxvk/rtx_render/rtx_light_manager.h:88`). **So the entire scene is lit by exactly
one distant light and nothing else.** The authored data above says the game intends, for that same
level, a sun at 200% plus two more directional lights plus a 5% hemisphere ambient. The missing
ambient is the black-shadowed-floor report.
