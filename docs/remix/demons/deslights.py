"""Extract Demon's Souls per-level authored lighting from the DrawParam banks.

Produces a compact table of, per level and per lighting preset row:
  * the three LIGHT_BANK directional lights (angles, colour, intensity%)
  * the hemisphere ambient (up / down)
  * the LIGHT_SCATTERING_BANK sun (the SKY's sun) and its Rayleigh/Mie terms
  * the FOG_BANK colour and range

Angles are stored as integer degrees. The direction convention is derived below
and is the ONE thing here that is inferred rather than read: everything else is
a field the paramdef names.
"""
import json
import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from desparam import read_paramdef, read_param  # noqa: E402

ROOT = r'E:\PS3 Games\Demons Souls BLUS30443\PS3_GAME\USRDIR'
DEFS = os.path.join(ROOT, 'paramdef')
DRAW = os.path.join(ROOT, 'param', 'drawparam')

WORLD = {
    'm01': 'Boletarian Palace (1-x)',
    'm02': 'Stonefang Tunnel (2-x)',
    'm03': 'Tower of Latria (3-x)',
    'm04': 'Shrine of Storms (4-x)',
    'm05': 'Valley of Defilement (5-x)',
    'm06': 'world 6',
    'm07': 'world 7',
    'm08': 'world 8',
    'm99': 'shared / tutorial',
}


def direction(deg_x, deg_y):
    """Angles -> a unit TRAVEL vector (light -> scene), y up, in the convention
    this project uses for m_sun_light_travel.

    Read as: start pointing down (0,-1,0), pitch up by degRotX about X, then yaw
    by degRotY about Y. degRotX=90 is straight down, 0 is horizontal. NOT verified
    in game - it is the one inference in this file and the A/B is a single run.
    """
    rx = math.radians(deg_x)
    ry = math.radians(deg_y)
    # local direction after pitch: horizontal component cos(rx), vertical -sin(rx)
    hx = math.cos(rx)
    vy = -math.sin(rx)
    x = hx * math.sin(ry)
    z = hx * math.cos(ry)
    n = math.sqrt(x * x + vy * vy + z * z) or 1.0
    return [round(x / n, 5), round(vy / n, 5), round(z / n, 5)]


def load(bank, level):
    path = os.path.join(DRAW, '%s_%s.param' % (level, bank))
    if not os.path.exists(path):
        return None
    _, _, _, fields = read_paramdef(os.path.join(DEFS, '%s.paramdef' % bank))
    _, _, _, rows = read_param(path, fields)
    return rows


def col(v, r, g, b, a):
    """RGB 0..255 with a percentage multiplier -> linear-ish rgb triple."""
    scale = v[a] / 100.0
    return [round(v[r] / 255.0 * scale, 4), round(v[g] / 255.0 * scale, 4), round(v[b] / 255.0 * scale, 4)]


def main():
    out = {}
    levels = sorted({f.split('_')[0] for f in os.listdir(DRAW) if f.endswith('.param')})
    for lv in levels:
        lb = load('lightbank', lv)
        ls = load('lightscatteringbank', lv)
        fb = load('fogbank', lv)
        if not lb:
            continue
        entry = {'world': WORLD.get(lv, lv), 'rows': []}
        for i, (rid, rname, v) in enumerate(lb):
            # A row that is all zeros is an unused preset slot.
            if not any(v.get(k, 0) for k in ('colA_0', 'colA_1', 'colA_2', 'colA_u', 'colA_d')):
                continue
            rec = {
                'id': rid,
                'name': rname,
                'dir': [
                    {'ang': [v['degRotX_0'], v['degRotY_0']], 'travel': direction(v['degRotX_0'], v['degRotY_0']),
                     'rgb': col(v, 'colR_0', 'colG_0', 'colB_0', 'colA_0'), 'pct': v['colA_0']},
                    {'ang': [v['degRotX_1'], v['degRotY_1']], 'travel': direction(v['degRotX_1'], v['degRotY_1']),
                     'rgb': col(v, 'colR_1', 'colG_1', 'colB_1', 'colA_1'), 'pct': v['colA_1']},
                    {'ang': [v['degRotX_2'], v['degRotY_2']], 'travel': direction(v['degRotX_2'], v['degRotY_2']),
                     'rgb': col(v, 'colR_2', 'colG_2', 'colB_2', 'colA_2'), 'pct': v['colA_2']},
                ],
                'hemi_up': col(v, 'colR_u', 'colG_u', 'colB_u', 'colA_u'),
                'hemi_dn': col(v, 'colR_d', 'colG_d', 'colB_d', 'colA_d'),
                'env_dif_pct': v['envDif_colA'],
            }
            if ls and i < len(ls):
                sv = ls[i][2]
                rec['sky'] = {
                    'sun_ang': [sv['sunRotX'], sv['sunRotY']],
                    'sun_travel': direction(sv['sunRotX'], sv['sunRotY']),
                    'sun_rgb': col(sv, 'sunR', 'sunG', 'sunB', 'sunA'),
                    'sun_pct': sv['sunA'],
                    'betaRay': round(sv['lsBetaRay'], 6),
                    'betaMie': round(sv['lsBetaMie'], 6),
                    'HGg': round(sv['lsHGg'], 4),
                    'distanceMul': sv['distanceMul'],
                    'inscatterMul': sv['inscatteringMul'],
                    'blend': sv['blendCoef'],
                }
            if fb and i < len(fb):
                fv = fb[i][2]
                rec['fog'] = {'begin': fv['fogBeginZ'], 'end': fv['fogEndZ'],
                              'rgb': col(fv, 'colR', 'colG', 'colB', 'colA'), 'pct': fv['colA']}
            entry['rows'].append(rec)
        out[lv] = entry
    return out


if __name__ == '__main__':
    data = main()
    dest = sys.argv[1] if len(sys.argv) > 1 else 'des_lights.json'
    with open(dest, 'w', encoding='utf-8') as fh:
        json.dump(data, fh, ensure_ascii=False, indent=1)
    for lv, e in data.items():
        used = len(e['rows'])
        sky = e['rows'][0].get('sky') if e['rows'] else None
        print('%-5s %-28s presets=%-3d %s' % (
            lv, e['world'], used,
            ('sun_ang=%s pct=%d' % (sky['sun_ang'], sky['sun_pct'])) if sky else ''))
