#!/usr/bin/env python3
"""
Resistance: Fall of Man (BCUS98107) -- authored analytic light extractor.

Reads PS3_GAME/USRDIR/game.psarc directly (no unpacking step) and writes one
<levelNN>.lights file per level into bin/resistance_lights/, in the exact text
format RemixGSRender::parse_authored_lights_file() accepts (35 whitespace
separated tokens on every `L` line).

WHERE THE LIGHTS LIVE
---------------------
game.psarc is a stock PSARC 1.2 (zlib, 0x10000 blocks); entry 0 is the newline
separated path manifest and entry k>=1 is manifest line k-1.

Every /packed/levels/level<NN>/ps3levelmain.dat is an IGHW container
("Insomniac Games Hunk Wad", big endian):

    'IGHW' u16 major u16 minor u32 sectionCount u32 flags
    v1.1 only: u32 dataEnd, u32 ?, 0xDEADDEAD, 0xDEADDEAD   (table then at 0x20)
    section entry = u32 id, u32 offset, u32 count, u32 elemSize
        count 0x0000_0001 -> elemSize IS the byte length (single blob)
        count 0x1000_NNNN -> NNNN elements of elemSize bytes

Section 0x8800 is the analytic light list: 128 bytes per record.
Section 0x8A00 is the light-volume list (ambient + two directional colours per
region), 128 bytes per record with a 128-byte header in front of it.
Section 0x7000 is the level environment block (ambient colour, sun direction).

0x8800 RECORD (offsets in bytes, big-endian)
    0x00 u32   flags (byte0 = 0/1, byte1 = 0x00/0x01/0xFF)
    0x04 u32   (type << 16) | index
               type bit 0x0004 = spot (cone + aim present), otherwise omni
               index 0xFFFF = dead slot, the whole record is zero
    0x14 f32   cull / fade distance
    0x18 f32   1.0 on every record seen
    0x20 f32   range: for an omni this EQUALS the bounding-sphere radius at 0x7C
    0x24 f32   secondary falloff parameter (unproven)
    0x28 f32   cone HALF angle, radians      (spot only)
    0x2C f32   penumbra angle, radians       (spot only)
    0x30 f32   cos(0x28)                     (spot only)
    0x34 f32   sin(0x28)                     (spot only)
    0x38 f32   cos(0x2C)                     (spot only)
    0x40 f32x3 world position (+0x4C = 1.0)
    0x50 f32x3 unit aim direction            (spot only)
    0x60 f32x3 linear RGB colour * intensity (may exceed 1.0)
    0x70 f32x4 bounding sphere: centre xyz, radius

PARSE VERIFICATION (each of these could have failed and did not)
  1. PSARC: manifest line count 3985 + 1 == TOC entry count 3986.
  2. IGHW v1.1: walking offset+length over all sections ends exactly at the u32
     stored at file offset 0x10 (level20: 0x019A3150 over 62 sections).
  3. omni: field 0x20 == bounding-sphere radius, and the sphere centre equals
     the position, on 320 of 321 omni records (the one exception is level90
     record 12, range 20 vs sphere radius 10.756).
  4. spot: |sphereCentre - position| == sphere radius AND the stored unit
     direction == normalize(sphereCentre - position), on 52 of 52 spots.
  5. spot trig: cos(f0x28)==f0x30, sin(f0x28)==f0x34, cos(f0x2C)==f0x38 and
     f0x30^2+f0x34^2==1, on 52 of 52 spots (to 2e-5).
  6. 0x8A00 header u32 at +4 == (sectionBytes/128 - 1) on 41 of 41 levels.
  7. 371 of 373 live lights fall inside the level's static-instance bounding
     box (section 0x9300 positions, padded 30 units), i.e. the lights are in
     the same world frame as the geometry.

COORDINATE FRAME
  Y is up (the AI navmesh in section 0x14100 is a constant-Y plane), right
  handed, metres. The emitted file is RAW: no permutation, no negation.
  Use RPCS3_REMIX_AUTHOREDLIGHTAXIS to reconcile with the runtime frame.

FIELD MAPPING INTO THE .lights RECORD
  cls   627 OmniLight / 631 SpotLight (the two classes the loader's sphere path
        understands; see RemixGSRender.cpp submit_authored_lights()).
  ARGB  A is always 255. RGB is the authored colour normalised by its largest
        component so hue survives the 8-bit quantisation.
  f0    intensity = that largest component (the loader does rgb/255 * f0 * scale).
  f1    range, field 0x20.
  f2    cull distance, field 0x14.
  f3    secondary falloff parameter, field 0x24 (raw, unproven).
  f4    field 0x18 (1.0 everywhere).
  f5    penumbra angle in DEGREES (spot only).
  f6    SYNTHESISED emitter radius, not authored -- Resistance stores no source
        extent. Defaults to 0.2, matching RPCS3_REMIX_AUTHOREDLIGHTMINRADIUS.
  f7    0.
  f8    FULL cone angle in DEGREES (the loader halves it), 0 for an omni.
  guid  md5(level|index)[:16] -- synthetic, the container carries no guid.
  M     line's map id is likewise synthetic (md5 of the level name): Resistance
        has no Maps/<16HEX>.map, so RPCS3_REMIX_AUTHOREDLIGHTAUTO cannot elect
        a level here. Use RPCS3_REMIX_AUTHOREDLIGHTLEVEL=<levelNN>.
"""

import argparse
import hashlib
import math
import os
import re
import struct
import sys
import zlib

# --------------------------------------------------------------------------- PSARC


class PSARC:
    def __init__(self, path):
        self.f = open(path, 'rb')
        hdr = self.f.read(32)
        magic, vmaj, vmin, comp, toc_len, ent_size, ents, block_size, flags = \
            struct.unpack('>4sHH4sIIIII', hdr)
        if magic != b'PSAR':
            raise ValueError('not a PSARC: %r' % magic)
        if comp != b'zlib':
            raise ValueError('unsupported compression %r' % comp)
        self.n = ents
        self.block_size = block_size
        raw = self.f.read(ent_size * ents)
        self.entries = []
        for i in range(ents):
            e = raw[i * ent_size:(i + 1) * ent_size]
            self.entries.append({
                'blk': struct.unpack('>I', e[16:20])[0],
                'usize': int.from_bytes(e[20:25], 'big'),
                'off': int.from_bytes(e[25:30], 'big'),
            })
        bs_bytes = 2 if block_size == 0x10000 else (3 if block_size == 0x1000000 else 4)
        remain = toc_len - 32 - ent_size * ents
        tab = self.f.read(remain)
        self.blocks = [int.from_bytes(tab[i * bs_bytes:(i + 1) * bs_bytes], 'big')
                       for i in range(remain // bs_bytes)]
        self._names = None

    def read(self, i, want=None):
        """Decompress entry i, stopping once `want` bytes are available."""
        e = self.entries[i]
        limit = e['usize'] if want is None else min(want, e['usize'])
        out = bytearray()
        self.f.seek(e['off'])
        bi = e['blk']
        while len(out) < limit:
            bl = self.blocks[bi]
            bi += 1
            if bl == 0:
                out += self.f.read(self.block_size)
            else:
                chunk = self.f.read(bl)
                out += zlib.decompress(chunk) if chunk[:1] == b'\x78' else chunk
        return bytes(out)

    def manifest(self):
        if self._names is None:
            text = self.read(0).decode('utf-8', 'replace')
            self._names = [l for l in text.replace('\r\n', '\n').split('\n') if l]
        return self._names


# --------------------------------------------------------------------------- IGHW


class IGHW:
    def __init__(self, data):
        if data[:4] != b'IGHW':
            raise ValueError('not IGHW')
        self.vmaj, self.vmin = struct.unpack('>HH', data[4:8])
        self.nsec = struct.unpack('>I', data[8:12])[0]
        base = 0x20 if (self.vmaj, self.vmin) == (1, 1) else 0x10
        self.data_end = struct.unpack('>I', data[16:20])[0] if base == 0x20 else None
        self.sections = {}
        self.order = []
        for i in range(self.nsec):
            o = base + i * 16
            sid, off, cnt, elem = struct.unpack('>IIII', data[o:o + 16])
            s = {'id': sid, 'off': off, 'cnt': cnt, 'elem': elem}
            s['bytes'] = elem if cnt == 1 else (cnt & 0xFFFF) * elem
            self.sections[sid] = s
            self.order.append(s)

    def walk_end(self):
        """Offset one past the last section -- compared against data_end."""
        last = max(self.order, key=lambda s: s['off'] + s['bytes'])
        return last['off'] + last['bytes']


# --------------------------------------------------------------------------- lights

OMNI_CLS = 627
SPOT_CLS = 631
REC = 128
DEFAULT_EMITTER_RADIUS = 0.2


class Light:
    __slots__ = ('index', 'slot', 'spot', 'flags', 'pos', 'aim', 'rgb', 'rng', 'cull',
                 'p24', 'p18', 'half_rad', 'pen_rad', 'sphere', 'file_off')


def parse_lights(blob, sec_off):
    """Section 0x8800 -> [Light]. `blob` is the whole section."""
    out = []
    for r in range(len(blob) // REC):
        b = blob[r * REC:(r + 1) * REC]
        u = struct.unpack('>32I', b)
        f = struct.unpack('>32f', b)
        index = u[1] & 0xFFFF
        if index == 0xFFFF:
            continue                      # dead slot; whole record is zero
        L = Light()
        L.index = index
        L.slot = r
        L.spot = bool((u[1] >> 16) & 0x0004)
        L.flags = u[0]
        L.cull = f[5]
        L.p18 = f[6]
        L.rng = f[8]
        L.p24 = f[9]
        L.half_rad = f[10] if L.spot else 0.0
        L.pen_rad = f[11] if L.spot else 0.0
        L.pos = (f[16], f[17], f[18])
        L.aim = (f[20], f[21], f[22]) if L.spot else (0.0, 1.0, 0.0)
        L.rgb = (f[24], f[25], f[26])
        L.sphere = (f[28], f[29], f[30], f[31])
        L.file_off = sec_off + r * REC
        out.append(L)
    return out


def verify_lights(lights):
    """Returns (checks_run, failures[]) -- the structural acceptance tests."""
    run = 0
    fails = []
    for L in lights:
        cx, cy, cz, cr = L.sphere
        d = math.dist(L.pos, (cx, cy, cz))
        if L.spot:
            run += 5
            if abs(d - cr) > 1e-3:
                fails.append('spot %d |c-p| %.5f != r %.5f' % (L.index, d, cr))
            if d > 1e-6:
                n = [(L.sphere[k] - L.pos[k]) / d for k in range(3)]
                if any(abs(n[k] - L.aim[k]) > 2e-4 for k in range(3)):
                    fails.append('spot %d aim != normalize(c-p)' % L.index)
            ca, sa = math.cos(L.half_rad), math.sin(L.half_rad)
            if abs(ca * ca + sa * sa - 1.0) > 1e-5:
                fails.append('spot %d trig identity' % L.index)
            if abs(math.sqrt(sum(x * x for x in L.aim)) - 1.0) > 2e-4:
                fails.append('spot %d aim not unit' % L.index)
            if not (0.0 < L.half_rad < math.pi):
                fails.append('spot %d cone angle out of range' % L.index)
        else:
            run += 2
            if abs(L.rng - cr) > 1e-4:
                fails.append('omni %d range %.5f != sphere r %.5f' % (L.index, L.rng, cr))
            if d > 1e-4:
                fails.append('omni %d sphere centre != position' % L.index)
    # The index column at +0x06 must equal (record slot + 2) on EVERY live record,
    # dead 0xFFFF slots included in the slot count. This is the structural check that
    # ties the walked record count to a field the exporter wrote independently: a
    # wrong stride or a wrong record size breaks it immediately.
    for L in lights:
        run += 1
        if L.index != L.slot + 2:
            fails.append('record %d: index %d != slot+2' % (L.slot, L.index))
    return run, fails


def parse_volumes(blob):
    """Section 0x8A00 -> (declared_count, [volume dicts])."""
    if len(blob) < REC:
        return 0, []
    declared = struct.unpack('>I', blob[4:8])[0]
    vols = []
    for r in range(1, len(blob) // REC):
        b = blob[r * REC:(r + 1) * REC]
        u = struct.unpack('>32I', b)
        f = struct.unpack('>32f', b)
        vols.append({
            'index': u[0] >> 16,
            'ambient': u[1], 'key': u[2], 'fill': u[3],
            'scale': f[4],
            'dir0': (f[8], f[9], f[10]),
            'dir1': (f[12], f[13], f[14]),
            'extent': (f[19], f[23], f[27]),
            'pos': (f[28], f[29], f[30]),
        })
    return declared, vols


# --------------------------------------------------------------------------- emit


def basis_from_aim(aim):
    """Row-major 3x3 whose ROW 1 is the aim (the loader's convention)."""
    ay = list(aim)
    n = math.sqrt(sum(x * x for x in ay))
    if n < 1e-6:
        return (1, 0, 0, 0, 1, 0, 0, 0, 1)
    ay = [x / n for x in ay]
    up = (1.0, 0.0, 0.0) if abs(ay[0]) < 0.9 else (0.0, 0.0, 1.0)
    ax = [up[1] * ay[2] - up[2] * ay[1],
          up[2] * ay[0] - up[0] * ay[2],
          up[0] * ay[1] - up[1] * ay[0]]
    n = math.sqrt(sum(x * x for x in ax)) or 1.0
    ax = [x / n for x in ax]
    az = [ay[1] * ax[2] - ay[2] * ax[1],
          ay[2] * ax[0] - ay[0] * ax[2],
          ay[0] * ax[1] - ay[1] * ax[0]]
    return tuple(ax) + tuple(ay) + tuple(az)


def quantise(rgb):
    """(A,R,G,B bytes, intensity) -- hue in the bytes, magnitude in f0."""
    m = max(rgb[0], rgb[1], rgb[2], 0.0)
    if m <= 1e-6:
        return (255, 0, 0, 0), 0.0
    b = tuple(max(0, min(255, int(round(c / m * 255.0)))) for c in rgb)
    return (255, b[0], b[1], b[2]), m


def g(x):
    x = float(x)
    if not math.isfinite(x):
        return '0'
    return '%g' % x


def emit(level, lights, map_id, src_name, src_bytes, out_path):
    lines = []
    A = lines.append
    A('# Resistance: Fall of Man (BCUS98107) authored analytic lights')
    A('# generated by tools/resistance/extract_lights.py')
    A('# source: %s section 0x8800 (128-byte records)' % src_name)
    A('# coordinates: level world space, Y-up, right-handed, metres -- RAW, no axis fix applied')
    A('# basis is row-major; row 1 (m10 m11 m12) is the aim direction')
    A('# class ids: 627=OmniLight 631=SpotLight')
    A('# f0 = intensity, f1 = range (metres), f2 = cull distance, f3 = raw falloff param,')
    A('# f4 = raw, f5 = penumbra angle (degrees), f6 = SYNTHESISED emitter radius (0.2, not authored),')
    A('# f7 = 0, f8 = FULL cone angle (degrees)')
    A('# L cls tmpl hasXform hasParams srcIdx  px py pz  m00 m01 m02 m10 m11 m12 m20 m21 m22  '
      'A R G B  f0 f1 f2 f3 f4 f5 f6 f7 f8  srcOffHex flagsHex guidHex name')
    A('V 1')
    A('M %s %s' % (map_id, level))
    A('N %d' % len(lights))
    A('S 0 %s %s %s %d %d' % (map_id, level, src_name, src_bytes, len(lights)))
    for L in lights:
        cls = SPOT_CLS if L.spot else OMNI_CLS
        argb, inten = quantise(L.rgb)
        bas = basis_from_aim(L.aim) if L.spot else (1, 0, 0, 0, 1, 0, 0, 0, 1)
        cone_deg = 2.0 * L.half_rad * 180.0 / math.pi if L.spot else 0.0
        pen_deg = L.pen_rad * 180.0 / math.pi if L.spot else 0.0
        guid = hashlib.md5(('%s|%d' % (level, L.index)).encode()).hexdigest()[:16]
        name = '%s_%s_%d' % ('SpotLight' if L.spot else 'OmniLight', level, L.index)
        A('L %d 1 1 1 0 %s %s %s %s %d %d %d %d %s 0x%x 0x%08x %s %s' % (
            cls,
            ' '.join(g(x) for x in L.pos),
            ' '.join(g(x) for x in bas[0:3]),
            ' '.join(g(x) for x in bas[3:6]),
            ' '.join(g(x) for x in bas[6:9]),
            argb[0], argb[1], argb[2], argb[3],
            ' '.join(g(x) for x in (inten, L.rng, L.cull, L.p24, L.p18,
                                    pen_deg, DEFAULT_EMITTER_RADIUS, 0.0, cone_deg)),
            L.file_off, L.flags, guid, name))
    with open(out_path, 'w', newline='\n') as fh:
        fh.write('\n'.join(lines) + '\n')
    return len(lines)


# --------------------------------------------------------------------------- main


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--game-dir', required=True,
                    help='directory containing PS3_GAME/USRDIR/game.psarc')
    ap.add_argument('--out-dir', required=True,
                    help='destination for the .lights files (bin/resistance_lights)')
    args = ap.parse_args()

    psarc_path = os.path.join(args.game_dir, 'PS3_GAME', 'USRDIR', 'game.psarc')
    if not os.path.isfile(psarc_path):
        psarc_path = os.path.join(args.game_dir, 'game.psarc')
    ar = PSARC(psarc_path)
    names = ar.manifest()
    if len(names) + 1 != ar.n:
        raise SystemExit('manifest/TOC mismatch: %d + 1 != %d' % (len(names), ar.n))
    index = {names[i - 1]: i for i in range(1, ar.n)}

    levels = sorted({re.match(r'/packed/levels/(level\d+)/', n).group(1)
                     for n in names if n.startswith('/packed/levels/')},
                    key=lambda s: int(s[5:]))
    os.makedirs(args.out_dir, exist_ok=True)

    report = ['Resistance: Fall of Man (BCUS98107) light extraction - parse report',
              'archive: %s' % psarc_path,
              'PSARC check: %d manifest lines + 1 == %d TOC entries  OK' % (len(names), ar.n),
              '']
    idx_lines = ['# Resistance (BCUS98107) authored light index',
                 '# I levelName mapAssetId file numSourceFiles numLights numSpots numOmnis',
                 'V 1']
    vol_lines = ['# Resistance (BCUS98107) light volumes, section 0x8A00. NOT emitted as .lights:',
                 '# they are an ambient plus two directional colours over an oriented box, which',
                 '# the sphere/rect submitter in RemixGSRender has no representation for.',
                 '# V level idx ambientRGBA keyRGBA fillRGBA scale dir0 dir1 halfExtent centre']

    tot_l = tot_s = tot_o = tot_v = 0
    checks = 0
    failures = []
    for level in levels:
        ent = index['/packed/levels/%s/ps3levelmain.dat' % level]
        head = ar.read(ent, 0x20000)
        g0 = IGHW(head)
        checks += 1
        if g0.data_end is not None and g0.walk_end() != g0.data_end:
            failures.append('%s: section walk ends %08X, header says %08X'
                            % (level, g0.walk_end(), g0.data_end))

        need = 0
        for sid in (0x7000, 0x8800, 0x8A00):
            s = g0.sections.get(sid)
            if s:
                need = max(need, s['off'] + s['bytes'])
        data = ar.read(ent, need + 0x1000) if need else head

        def blob(sid):
            s = g0.sections.get(sid)
            if not s or s['bytes'] == 0:
                return b''
            return data[s['off']:s['off'] + s['bytes']]

        s88 = g0.sections.get(0x8800)
        lights = parse_lights(blob(0x8800), s88['off'] if s88 else 0)
        n, f = verify_lights(lights)
        checks += n
        failures += ['%s: %s' % (level, x) for x in f]

        declared, vols = parse_volumes(blob(0x8A00))
        checks += 1
        if (vols or declared) and declared != len(vols):
            failures.append('%s: 0x8A00 declares %d, walked %d' % (level, declared, len(vols)))
        for v in vols:
            vol_lines.append('V %s %d %08X %08X %08X %s %s %s %s %s' % (
                level, v['index'], v['ambient'], v['key'], v['fill'], g(v['scale']),
                ','.join(g(x) for x in v['dir0']), ','.join(g(x) for x in v['dir1']),
                ','.join(g(x) for x in v['extent']), ','.join(g(x) for x in v['pos'])))

        map_id = hashlib.md5(level.encode()).hexdigest()[:16].upper()
        spots = sum(1 for L in lights if L.spot)
        emit(level, lights, map_id, 'ps3levelmain.dat',
             s88['bytes'] if s88 else 0,
             os.path.join(args.out_dir, '%s.lights' % level))
        idx_lines.append('I %s %s %s.lights 1 %d %d %d'
                         % (level, map_id, level, len(lights), spots, len(lights) - spots))
        report.append('  %-9s %3d lights (%2d spot, %3d omni)  %3d light volumes'
                      % (level, len(lights), spots, len(lights) - spots, len(vols)))
        tot_l += len(lights)
        tot_s += spots
        tot_o += len(lights) - spots
        tot_v += len(vols)

    report += ['',
               'total analytic lights: %d  (%d spot, %d omni)' % (tot_l, tot_s, tot_o),
               'total light volumes:   %d' % tot_v,
               'structural checks run: %d' % checks,
               'structural failures:   %d' % len(failures)]
    report += ['  ' + x for x in failures]
    with open(os.path.join(args.out_dir, 'index.txt'), 'w', newline='\n') as fh:
        fh.write('\n'.join(idx_lines) + '\n')
    with open(os.path.join(args.out_dir, 'lightvols.txt'), 'w', newline='\n') as fh:
        fh.write('\n'.join(vol_lines) + '\n')
    with open(os.path.join(args.out_dir, 'parse_report.txt'), 'w', newline='\n') as fh:
        fh.write('\n'.join(report) + '\n')
    print('\n'.join(report))
    return 0 if not failures else 1


if __name__ == '__main__':
    sys.exit(main())
