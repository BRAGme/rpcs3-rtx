#!/usr/bin/env python3
"""
Resistance: Fall of Man (BCUS98107) -- level light-VOLUME extractor (section 0x8A00).

Companion to extract_lights.py (section 0x8800, the analytic lights).  Reads
PS3_GAME/USRDIR/game.psarc directly and writes one <levelNN>.lightvols file per
level: a per-region ambient + key + fill rig attached to an ORIENTED BOX.

Everything below was derived from the shipped data; every claim carries the
check that could have refuted it (counts are over all 41 levels / 166 volumes).

SECTION 0x8A00 LAYOUT
    128-byte records.  Record 0 is a HEADER, records 1..N are the volumes.
    Section byte length / 128 - 1 == the count in the header (41 of 41 levels).

HEADER RECORD (record 0) -- the level DEFAULT rig, i.e. what applies when the
camera is inside no volume.  Same field order as a volume, but the colours are
linear floats (they exceed 1.0: the default key reaches 2.0) and there is no
box.
    +0x00 u32     0x00000080 on 41 of 41 levels (kind / flags tag)
    +0x04 u32     volume count
    +0x08 u32     1 on 41 of 41 levels
    +0x0C u32     0
    +0x10 f32x3   default ambient RGB (linear)          +0x1C = 0
    +0x20 f32x3   default key     RGB (linear, HDR)     +0x2C = 0
    +0x30 f32x3   default fill    RGB (linear)          +0x3C = 0
    +0x40 f32x3   default key  direction (unit)         +0x4C = 0
    +0x50 f32x3   default fill direction (unit)         +0x5C = 0
    +0x60 f32     default scale                         +0x64..+0x7F = 0
    Every word listed as 0 is 0 on all 41 levels.

VOLUME RECORD (records 1..N) -- ALL 128 bytes are accounted for:
    +0x00 u16     record index, 0..N-1, sequential (checked on 166 of 166)
    +0x02 u16     0 on 166 of 166 volumes
    +0x04 u8x4    ambient colour R,G,B,A
    +0x08 u8x4    key     colour R,G,B,A
    +0x0C u8x4    fill    colour R,G,B,A
                  A == 255 on all 3 x 166 colour words, which is what fixes the
                  byte order as R,G,B,A rather than A,R,G,B.
    +0x10 f32     scale (overall intensity).  17 distinct authored values,
                  0.17 .. 1.0; never <= 0 and never > 1.
    +0x14,+0x18,+0x1C  0 on 166 of 166
    +0x20 f32x3   key  direction, unit (|d| = 1 +/- 1e-3, 166 of 166)  +0x2C = 0
    +0x30 f32x3   fill direction, unit (|d| = 1 +/- 1e-3, 166 of 166)  +0x3C = 0
    +0x40 .. +0x7F   ONE 4x4 ROW-VECTOR TRANSFORM with the half extents packed
                  into the otherwise unused w column:
                      +0x40 f32x3 box X axis (world)   +0x4C f32 halfExtent.x
                      +0x50 f32x3 box Y axis (world)   +0x5C f32 halfExtent.y
                      +0x60 f32x3 box Z axis (world)   +0x6C f32 halfExtent.z
                      +0x70 f32x3 box centre (world)   +0x7C f32 0 on 166 of 166

THE ORIENTATION (the field the earlier pass missed)
  The 3x3 at +0x40 IS the box orientation.  It is NOT the pair at +0x20/+0x30:
  those are the key and fill light directions and are not orthogonal (level20
  volume 1 has dot = -0.46, volume 4 dot = -0.77).

  Evidence, all of which could have failed and did not:
    1. Rows are orthonormal and det = +1 on 166 of 166 records (tol 1e-4).
    2. 99 records are exactly the identity; the other 67 are, without
       exception, a PURE YAW about world +Y (m01 = m10 = m12 = m21 = 0,
       m11 = 1).  A mis-parsed or unrelated payload could not do that.
    3. Every one of those 67 yaw angles is an exact multiple of 0.5 degrees
       (-105.0 .. +45.5) -- an authored angle snap, not float noise.
    4. All 498 half extents are exact multiples of 0.5 as well.
    5. All 166 centres fall inside the level's static-instance bounding box
       (section 0x9300 row-3 translations, padded 30 units).

  ROWS ARE THE AXES (world = local * M), not columns:
    a. Section 0x9300 static instances use the identical 4x4 struct with the
       translation in ROW 3 (w = 1.0) and w = 0 in rows 0..2 -- the D3D
       row-vector convention.  A column-vector matrix would carry translation
       in column 3, which is 0 in those records.  The 0x8A00 block is the same
       struct, with the extents written into the free w column.
    b. Empirical: for the 24 volumes whose yaw is far enough from 0 and 45
       degrees to discriminate, the static instances inside the box have their
       OWN yaw equal to +volumeYaw on 532 instances and to -volumeYaw on 19.
       Transposing the matrix would invert that 28:1 result.

WHICH VOLUME CONTAINS A POINT (the per-frame test)
    d     = p - centre
    local = ( dot(d, row0), dot(d, row1), dot(d, row2) )
    inside <=> |local.x| <= ex && |local.y| <= ey && |local.z| <= ez
  Because the rotation is always a pure yaw, a loader may equivalently rotate
  only x/z by -yaw.  Overlap measured over all 508 intra-level volume pairs:
  48 pairs overlap, 7 of those are full containment, and no sampled point lies
  in more than 2 volumes.  So: test every volume and keep the match with the
  SMALLEST box volume (8*ex*ey*ez); that resolves all 7 nested pairs to the
  inner room.  If nothing matches, use the D line (the level default rig).

WHAT IS STILL UNPROVEN
  The photometric meaning of `scale`, and whether the LDR 0..255 key/fill are
  absolute or modulate the header's HDR key/fill.  The bytes are emitted raw.

OUTPUT (one file per level, tokenised like the .lights files)
    V 1
    M <mapId> <level>
    D  ambR ambG ambB  keyR keyG keyB  fillR fillG fillB
       keyDir(3) fillDir(3) scale                                   (16 floats)
    N <volumeCount>
    B idx  cx cy cz  ex ey ez  m00 m01 m02 m10 m11 m12 m20 m21 m22
      ambA ambR ambG ambB  keyA keyR keyG keyB  fillA fillR fillG fillB
      keyDir(3) fillDir(3)  scale  yawDeg  srcOffHex       (37 tokens + tag)
"""

import argparse
import hashlib
import math
import os
import re
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from extract_lights import PSARC, IGHW, g          # noqa: E402  (shared reader)

REC = 128


# --------------------------------------------------------------------------- parse


def parse_section(blob, sec_off):
    """0x8A00 blob -> (header dict, [volume dicts])."""
    if len(blob) < REC:
        return None, []
    h = struct.unpack('>32I', blob[:REC])
    hf = struct.unpack('>32f', blob[:REC])
    hdr = {
        'tag': h[0], 'count': h[1], 'one': h[2],
        'ambient': hf[4:7], 'key': hf[8:11], 'fill': hf[12:15],
        'keydir': hf[16:19], 'filldir': hf[20:23], 'scale': hf[24],
        'zero': [h[3], h[7], h[11], h[15], h[19], h[23]] + list(h[25:32]),
    }
    vols = []
    for r in range(1, len(blob) // REC):
        b = blob[r * REC:(r + 1) * REC]
        u = struct.unpack('>32I', b)
        f = struct.unpack('>32f', b)
        vols.append({
            'slot': r - 1,
            'index': u[0] >> 16,
            'pad': u[0] & 0xFFFF,
            'ambient': u[1], 'key': u[2], 'fill': u[3],
            'scale': f[4],
            'keydir': (f[8], f[9], f[10]),
            'filldir': (f[12], f[13], f[14]),
            'rot': ((f[16], f[17], f[18]), (f[20], f[21], f[22]), (f[24], f[25], f[26])),
            'extent': (f[19], f[23], f[27]),
            'centre': (f[28], f[29], f[30]),
            'zero': [u[5], u[6], u[7], u[11], u[15], u[31]],
            'off': sec_off + r * REC,
        })
    return hdr, vols


def is_identity_yaw(R):
    """True when R is a rotation about world +Y only."""
    return (abs(R[0][1]) < 1e-5 and abs(R[1][0]) < 1e-5 and abs(R[1][2]) < 1e-5
            and abs(R[2][1]) < 1e-5 and abs(R[1][1] - 1.0) < 1e-5)


def yaw_of(R):
    return math.degrees(math.atan2(R[0][2], R[0][0]))


def static_positions(data, sec):
    """Section 0x9300 row-3 translations (w == 1) -- the geometry-frame check."""
    out = []
    if not sec or not sec['bytes']:
        return out
    e = sec['elem']
    for i in range(sec['bytes'] // e):
        o = sec['off'] + i * e + 0x30
        f = struct.unpack('>4f', data[o:o + 16])
        if f[3] == 1.0 and all(abs(x) < 1e6 for x in f[:3]):
            out.append(f[:3])
    return out


# --------------------------------------------------------------------------- verify


def verify(level, hdr, vols, inst_pos):
    """Structural acceptance tests -> (checks_run, failures[])."""
    run, fails = 0, []
    run += 4
    if hdr['tag'] != 0x80:
        fails.append('%s: header tag %08X != 00000080' % (level, hdr['tag']))
    if hdr['one'] != 1:
        fails.append('%s: header +0x08 %d != 1' % (level, hdr['one']))
    if hdr['count'] != len(vols):
        fails.append('%s: header declares %d, walked %d' % (level, hdr['count'], len(vols)))
    if any(hdr['zero']):
        fails.append('%s: header reserved words not zero' % level)

    lo = hi = None
    if inst_pos:
        lo = [min(p[k] for p in inst_pos) for k in range(3)]
        hi = [max(p[k] for p in inst_pos) for k in range(3)]

    for v in vols:
        tag = '%s v%d' % (level, v['slot'])
        run += 2
        if v['index'] != v['slot']:
            fails.append('%s: index field %d out of sequence' % (tag, v['index']))
        if v['pad'] or any(v['zero']):
            fails.append('%s: reserved words not zero' % tag)
        run += 3
        for k, word in enumerate((v['ambient'], v['key'], v['fill'])):
            if word & 0xFF != 0xFF:
                fails.append('%s: colour %d alpha byte %02X != FF' % (tag, k, word & 0xFF))
        run += 2
        for nm in ('keydir', 'filldir'):
            n = math.sqrt(sum(x * x for x in v[nm]))
            if abs(n - 1.0) > 1e-3:
                fails.append('%s: %s |d| = %.6f' % (tag, nm, n))
        R = v['rot']
        run += 7
        for a in range(3):
            if abs(math.sqrt(sum(x * x for x in R[a])) - 1.0) > 1e-4:
                fails.append('%s: rotation row %d is not unit length' % (tag, a))
        for a, c in ((0, 1), (0, 2), (1, 2)):
            if abs(sum(R[a][k] * R[c][k] for k in range(3))) > 1e-4:
                fails.append('%s: rotation rows %d,%d not orthogonal' % (tag, a, c))
        det = (R[0][0] * (R[1][1] * R[2][2] - R[1][2] * R[2][1])
               - R[0][1] * (R[1][0] * R[2][2] - R[1][2] * R[2][0])
               + R[0][2] * (R[1][0] * R[2][1] - R[1][1] * R[2][0]))
        if abs(det - 1.0) > 1e-4:
            fails.append('%s: rotation det = %.6f' % (tag, det))
        run += 2
        if not is_identity_yaw(R):
            fails.append('%s: rotation is not a pure yaw' % tag)
        y = yaw_of(R)
        if abs(y * 2.0 - round(y * 2.0)) > 2e-3:
            fails.append('%s: yaw %.5f is not a 0.5 degree multiple' % (tag, y))
        run += 4
        for e in v['extent']:
            if e <= 0.0:
                fails.append('%s: half extent %g <= 0' % (tag, e))
            elif abs(e * 2.0 - round(e * 2.0)) > 1e-4:
                fails.append('%s: half extent %g is not a 0.5 multiple' % (tag, e))
        if not (0.0 < v['scale'] <= 1.0):
            fails.append('%s: scale %g outside (0,1]' % (tag, v['scale']))
        if lo:
            run += 1
            if not all(lo[k] - 30.0 <= v['centre'][k] <= hi[k] + 30.0 for k in range(3)):
                fails.append('%s: centre outside padded static bbox' % tag)
    return run, fails


# --------------------------------------------------------------------------- emit


def argb(word):
    """RGBA8 word -> (A, R, G, B), matching the .lights house order."""
    return (word & 0xFF, (word >> 24) & 0xFF, (word >> 16) & 0xFF, (word >> 8) & 0xFF)


HEADER_COMMENT = [
    '# Resistance: Fall of Man (BCUS98107) per-region light volumes',
    '# generated by tools/resistance/extract_lightvols.py',
    '# coordinates: level world space, Y-up, right-handed, metres -- RAW, no axis fix',
    '#',
    '# D = level default rig (the section header record); use it when the camera is',
    '#     inside no volume.  16 floats, colours are LINEAR and the key may exceed 1:',
    '#   D ambR ambG ambB  keyR keyG keyB  fillR fillG fillB  keyDir(3) fillDir(3) scale',
    '#',
    '# B = one light volume, 37 tokens after the tag (38 per line):',
    '#   B idx  cx cy cz  ex ey ez  m00 m01 m02 m10 m11 m12 m20 m21 m22',
    '#     ambA ambR ambG ambB  keyA keyR keyG keyB  fillA fillR fillG fillB',
    '#     keyDirX keyDirY keyDirZ  fillDirX fillDirY fillDirZ  scale  yawDeg  srcOffHex',
    '#',
    '# m** is ROW-MAJOR and the ROWS are the box axes in world space (world = local*M).',
    '# e* are HALF extents.  Containment test for a world point p:',
    '#   d = p - c;  inside <=> |dot(d,row0)|<=ex && |dot(d,row1)|<=ey && |dot(d,row2)|<=ez',
    '# If several volumes match, take the one with the smallest 8*ex*ey*ez; if none',
    '# match, use the D line.  Every shipped rotation is a pure yaw about +Y (yawDeg).',
    '# Colour bytes are 0..255 and the alpha byte is 255 on every record.',
]


def emit(level, hdr, vols, map_id, src_bytes, path):
    A = list(HEADER_COMMENT)
    A.insert(2, '# source: packed/levels/%s/ps3levelmain.dat section 0x8A00 (%d bytes)'
             % (level, src_bytes))
    A.append('V 1')
    A.append('M %s %s' % (map_id, level))
    A.append('D %s %s %s %s %s %s' % (
        ' '.join(g(x) for x in hdr['ambient']),
        ' '.join(g(x) for x in hdr['key']),
        ' '.join(g(x) for x in hdr['fill']),
        ' '.join(g(x) for x in hdr['keydir']),
        ' '.join(g(x) for x in hdr['filldir']),
        g(hdr['scale'])))
    A.append('N %d' % len(vols))
    for v in vols:
        R = v['rot']
        A.append('B %d %s %s %s %s %s %s %s %s %s %s %s %s 0x%x' % (
            v['index'],
            ' '.join(g(x) for x in v['centre']),
            ' '.join(g(x) for x in v['extent']),
            ' '.join(g(x) for x in R[0]),
            ' '.join(g(x) for x in R[1]),
            ' '.join(g(x) for x in R[2]),
            ' '.join(str(x) for x in argb(v['ambient'])),
            ' '.join(str(x) for x in argb(v['key'])),
            ' '.join(str(x) for x in argb(v['fill'])),
            ' '.join(g(x) for x in v['keydir']),
            ' '.join(g(x) for x in v['filldir']),
            g(v['scale']),
            g(yaw_of(R)),
            v['off']))
    with open(path, 'w', newline='\n') as fh:
        fh.write('\n'.join(A) + '\n')
    return A


# --------------------------------------------------------------------------- main


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--game-dir', required=True,
                    help='directory containing PS3_GAME/USRDIR/game.psarc')
    ap.add_argument('--out-dir', required=True,
                    help='destination for the .lightvols files')
    args = ap.parse_args()

    p = os.path.join(args.game_dir, 'PS3_GAME', 'USRDIR', 'game.psarc')
    if not os.path.isfile(p):
        p = os.path.join(args.game_dir, 'game.psarc')
    ar = PSARC(p)
    names = ar.manifest()
    if len(names) + 1 != ar.n:
        raise SystemExit('manifest/TOC mismatch: %d + 1 != %d' % (len(names), ar.n))
    index = {names[i - 1]: i for i in range(1, ar.n)}
    levels = sorted({re.match(r'/packed/levels/(level\d+)/', n).group(1)
                     for n in names if n.startswith('/packed/levels/')},
                    key=lambda s: int(s[5:]))
    os.makedirs(args.out_dir, exist_ok=True)

    report = ['Resistance: Fall of Man (BCUS98107) light-volume extraction (section 0x8A00)',
              'archive: %s' % p,
              'PSARC: %d manifest lines + 1 == %d TOC entries  OK' % (len(names), ar.n), '']
    idx = ['# Resistance (BCUS98107) light-volume index',
           '# I level mapId file numVolumes numYawed', 'V 1']
    checks = 0
    fails = []
    tot = rot = 0
    for level in levels:
        ent = index['/packed/levels/%s/ps3levelmain.dat' % level]
        head = ar.read(ent, 0x20000)
        gw = IGHW(head)
        checks += 1
        if gw.data_end is not None and gw.walk_end() != gw.data_end:
            fails.append('%s: section walk ends %08X, header says %08X'
                         % (level, gw.walk_end(), gw.data_end))
        need = 0
        for sid in (0x8A00, 0x9300):
            s = gw.sections.get(sid)
            if s:
                need = max(need, s['off'] + s['bytes'])
        data = ar.read(ent, need + 0x1000) if need else head
        s = gw.sections.get(0x8A00)
        if not s or not s['bytes']:
            continue
        hdr, vols = parse_section(data[s['off']:s['off'] + s['bytes']], s['off'])
        n, f = verify(level, hdr, vols, static_positions(data, gw.sections.get(0x9300)))
        checks += n
        fails += f
        map_id = hashlib.md5(level.encode()).hexdigest()[:16].upper()
        emit(level, hdr, vols, map_id, s['bytes'],
             os.path.join(args.out_dir, '%s.lightvols' % level))
        nrot = sum(1 for v in vols if abs(yaw_of(v['rot'])) > 1e-6)
        idx.append('I %s %s %s.lightvols %d %d' % (level, map_id, level, len(vols), nrot))
        report.append('  %-9s %3d volumes (%2d with a non-zero yaw)'
                      % (level, len(vols), nrot))
        tot += len(vols)
        rot += nrot
    report += ['', 'total light volumes: %d (%d carry a non-identity yaw)' % (tot, rot),
               'structural checks run: %d' % checks,
               'structural failures:   %d' % len(fails)]
    report += ['  ' + x for x in fails]
    with open(os.path.join(args.out_dir, 'index.txt'), 'w', newline='\n') as fh:
        fh.write('\n'.join(idx) + '\n')
    with open(os.path.join(args.out_dir, 'parse_report.txt'), 'w', newline='\n') as fh:
        fh.write('\n'.join(report) + '\n')
    print('\n'.join(report))
    return 0 if not fails else 1


if __name__ == '__main__':
    sys.exit(main())
