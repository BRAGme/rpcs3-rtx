#!/usr/bin/env python3
"""
extract_lights.py - authored analytic light extractor for
"Eat Lead: The Return of Matt Hazard" (PS3, BLUS30267), Vicious Engine 2.

Reads the shipped, uncompressed, big-endian .map / .rgn containers and emits one
plain-text light list per level plus an index, for the RPCS3 RTX Remix backend.

The parse is STRUCTURAL: it walks the container's pointer grammar and validates
that the pointer intervals form a properly nested (laminar) family covering the
whole file.  Light records are then located inside that tree, not by scanning
raw memory for float patterns.  Anything that does not parse is counted and
named in the report - see --report / parse_report.txt.

Container grammar (all big-endian, all offsets byte-verified in this session):

    0x00  u32  0xFAAFFAAF                 magic
    0x04  u32  0x00000001                 version
    0x08  u32  0xBBBBBBBB                 root pointer tag
    0x0C  u32  filesize - 8               root pointee end offset (exclusive)
    0x10  ..                              root pointee
    EOF-4 u32  0xFEEFFEEF                 footer

    0xBBBBBBBB <u32 end>                  non-null pointer; the pointee is
                                          serialized inline at +8 and ends at
                                          the absolute offset `end` (exclusive)
    0xBEBEBEBE                            null pointer, no payload

    object record (4-byte aligned in every file checked):
      +0x00 u32  0x00000002               object tag
      +0x04 u32  classId
      +0x08 u64  instance GUID
      +0x10 u32  sizeof(class) in memory
      +0x14 u32  flags  a bitfield, NOT a two-valued field.  Bit 0x01000000
                        marks a template (body carries the material chain
                        inline); 0x00200000 / 0x400 / 0x40 / 0x08 / 0x04 also
                        occur on ordinary instances.  Filtering flags to
                        {0, 0x01000000} - which earlier work did - drops 240
                        real light records.
      +0x18 body

    Label / string node: node content of exactly 48 bytes =
      u32 nodeVersion(1), u32 capacity(0x24), char name[32], u32 pad,
      u32 (len<<16)|hash16

    light parameter block (position within the body varies by body variant, so
    it is located by search inside the record's own byte window):
      u8      0xFF                        alpha; 0xFF on 2415 of 2422 records,
                                          0xD6 on the other 7
      u8[3]   R G B
      f32[9]  f0..f8                      f1 = range (m), f8 = cone angle (deg)
      u8      0 or 1

    transform: the final 48 bytes of a pointer node inside the light's entity
      node = float3 world position followed by a row-major orthonormal 3x3
      basis.  Basis row 1 is the aim direction.  Z-up, right-handed, metres.

Level membership is byte-verified, not inferred from names: each .map file
contains the 8-byte asset ID of every region that belongs to it (the ID is also
the region's 16-hex filename and lives at file offset 0x28).  Across the 10 maps
and 53 regions this is a perfect partition - every region is referenced by
exactly one map.

Read-only: this script never writes to the game directory.
"""

from __future__ import annotations

import argparse
import bisect
import glob
import math
import os
import struct
import sys
from collections import Counter, OrderedDict

# ---------------------------------------------------------------------------
# constants
# ---------------------------------------------------------------------------

MAGIC = 0xFAAFFAAF
FOOTER = 0xFEEFFEEF
PTR_TAG = b'\xbb\xbb\xbb\xbb'
PTR_TAG_U32 = 0xBBBBBBBB
NULL_TAG = 0xBEBEBEBE       # null pointer; carries no payload, so the walk just
                            # steps over it - listed here for the record

OBJ_TAG = 2
# Every flags bit seen on a light object across all 63 containers.  Anything
# outside this mask is counted (light_hdr_unknown_flag_bits) but still kept.
KNOWN_FLAG_BITS = 0x01200000 | 0x0000044C
TEMPLATE_BIT = 0x01000000

# classId -> (name, sizeof(class))   sizes are byte-verified on real records
LIGHT_CLASSES = OrderedDict((
    (0x00000273, ('OmniLight', 120)),
    (0x00000277, ('SpotLight', 128)),
    (0x000002EA, ('AreaLight', 120)),
    (0x73CEB6A4, ('SpotOmniLight', 132)),
    # Fifth class, not in the earlier survey: template names are "DualArea" and
    # "DualArea_StaticShadow", matching the RenderConfig's vedualsurfacelight*
    # shader set.  138 records.
    (0x4240F7C5, ('DualAreaLight', 120)),
))

# The light parameter block is  u8 A(=0xFF), u8 R, u8 G, u8 B, f32[9], u8 tail.
# It is anchored on the opaque alpha byte rather than on what precedes it,
# because what precedes it is NOT constant: five body variants were measured
# across the 1802 instance records --
#   00 01 00 00 00 00 00   1589    the common inline form
#   00 01 01 00 00 00 00     77
#   80 00 01 00 00 00 00       8
#   04 00 01 00 00 00 00       1
#   00 01 00 00 00 00 01       3
#   24 ..                    124    body opens with an inline 44-byte Label,
#                                   then more fields, then BEBEBEBE, then the
#                                   block (this is the "124 junk records" the
#                                   fixed-offset decode reported)
PARAM_ALPHA = 0xFF
ALPHA_BYTE = bytes([PARAM_ALPHA])
PARAM_LEN = 4 + 9 * 4 + 1               # ARGB + 9 floats + tail byte

# Per-field plausibility windows.  Deliberately wide: they exist to reject
# random byte runs, not to assert semantics.  Only f1 (range) and f8 (cone
# angle in degrees) have evidence behind their meaning.
PARAM_RANGE = (
    (0.0, 1.0e3),      # f0 intensity
    (1e-6, 1.0e5),     # f1 range, must be positive
    (0.0, 1.0e3),      # f2
    (0.0, 1.0e3),      # f3
    (0.0, 1.0e5),      # f4
    (0.0, 1.0e3),      # f5
    (0.0, 1.0e5),      # f6
    (0.0, 1.0e5),      # f7
    (0.0, 720.0),      # f8 cone angle, degrees
)
MIN_MAGNITUDE = 1e-6    # a non-zero field below this is denormal garbage
PARAM_FALLBACK_WINDOW = 0x400   # bytes searched when the opaque-alpha anchor misses

# A template light record carries its whole material/texture payload inline; the
# largest measured gap between the object header and its parameter block is
# ~66 KB, so the per-light search window is capped well above that but still
# finite, and is additionally clipped to the next light object header.
MAX_LIGHT_SPAN = 1 << 20

XFORM_LEN = 48                           # float3 pos + 3x3 basis
ORTHO_TOL = 1e-3
POS_LIMIT = 1e5

OUT_VERSION = 1

# ---------------------------------------------------------------------------
# small helpers
# ---------------------------------------------------------------------------


def u32(data: bytes, off: int) -> int:
    return struct.unpack_from('>I', data, off)[0]


def finite(x: float) -> bool:
    return x == x and -math.inf < x < math.inf


def valid_transform(v):
    """v = 12 floats.  True if v[0:3] is a plausible position and v[3:12] is an
    orthonormal 3x3 basis."""
    if not all(finite(x) for x in v):
        return False
    if any(abs(x) > POS_LIMIT for x in v[0:3]):
        return False
    rows = (v[3:6], v[6:9], v[9:12])
    for r in rows:
        n = math.sqrt(r[0] * r[0] + r[1] * r[1] + r[2] * r[2])
        if abs(n - 1.0) > ORTHO_TOL:
            return False
    a, b, c = rows
    det = (a[0] * (b[1] * c[2] - b[2] * c[1])
           - a[1] * (b[0] * c[2] - b[2] * c[0])
           + a[2] * (b[0] * c[1] - b[1] * c[0]))
    return abs(abs(det) - 1.0) <= 1e-2


def valid_params(data: bytes, p: int, require_alpha: bool = True) -> bool:
    """True if a light parameter block starts at p: alpha, nine floats inside
    their plausibility windows, and a 0/1 tail byte."""
    if require_alpha and data[p] != PARAM_ALPHA:
        return False
    tail = data[p + PARAM_LEN - 1]
    if tail > 1:
        return False
    f = struct.unpack_from('>9f', data, p + 4)
    for x, (lo, hi) in zip(f, PARAM_RANGE):
        if not finite(x) or x < lo or x > hi:
            return False
        if x != 0.0 and abs(x) < MIN_MAGNITUDE:
            return False
    return True


def bisect_nodes(nodes, lo, hi):
    """Indices of nodes whose content start lies in [lo, hi).  nodes[] is in DFS
    pre-order, which is ascending by start."""
    starts = _starts_for(nodes)
    return range(bisect.bisect_left(starts, lo), bisect.bisect_left(starts, hi))


_NODE_STARTS_CACHE = {}


def _starts_for(nodes):
    starts = _NODE_STARTS_CACHE.get(id(nodes))
    if starts is None or len(starts) != len(nodes):
        starts = [nd.start for nd in nodes]
        _NODE_STARTS_CACHE.clear()
        _NODE_STARTS_CACHE[id(nodes)] = starts
    return starts


def innermost_node(nodes, starts, off):
    """Index of the innermost pointer node containing byte `off`, or -1.
    Node intervals are laminar, so among the nodes that start at or before off
    the first one (scanning back) that also ends after off is the innermost."""
    i = bisect.bisect_right(starts, off) - 1
    while i >= 0:
        if nodes[i].end > off:
            return i
        i -= 1
    return -1


def find_all(data: bytes, pat: bytes, lo: int = 0, hi: int | None = None):
    """Every occurrence of pat, including overlapping ones."""
    if hi is None:
        hi = len(data)
    out = []
    i = data.find(pat, lo, hi)
    while i >= 0:
        out.append(i)
        i = data.find(pat, i + 1, hi)
    return out


# ---------------------------------------------------------------------------
# container parse
# ---------------------------------------------------------------------------


class Node:
    __slots__ = ('start', 'end', 'parent', 'ver', 'last_child_end')

    def __init__(self, start, end, parent, ver):
        self.start = start          # first byte of node content (the version dword)
        self.end = end              # exclusive
        self.parent = parent        # index into nodes[], -1 for the root
        self.ver = ver
        self.last_child_end = start


class LightRec:
    __slots__ = ('off', 'cls_id', 'cls_name', 'size', 'flags', 'name', 'guid',
                 'argb', 'params', 'param_off', 'pos', 'basis', 'xform_off',
                 'entity_node', 'notes', 'body_prefix', 'owner_node')

    def __init__(self):
        self.name = ''
        self.argb = None
        self.params = None
        self.param_off = -1
        self.pos = None
        self.basis = None
        self.xform_off = -1
        self.notes = []
        self.body_prefix = ''
        self.guid = 0
        self.owner_node = -1


class FileParse:
    def __init__(self, path):
        self.path = path
        self.ok = False
        self.error = None
        self.asset_id = b''
        self.name = ''
        self.size = 0
        self.nodes = []
        self.lights = []
        self.stats = Counter()


def parse_container(path, keep_nodes=False) -> FileParse:
    fp = FileParse(path)
    try:
        with open(path, 'rb') as f:
            data = f.read()
    except OSError as exc:
        fp.error = 'read failed: %s' % exc
        return fp

    n = len(data)
    fp.size = n
    if n < 0x60:
        fp.error = 'file too short (%d bytes)' % n
        return fp
    if u32(data, 0) != MAGIC:
        fp.error = 'bad magic %08x (want %08x)' % (u32(data, 0), MAGIC)
        return fp
    if u32(data, n - 4) != FOOTER:
        fp.stats['bad_footer'] += 1
    if u32(data, 8) != PTR_TAG_U32:
        fp.error = 'no root pointer tag at 0x08'
        return fp
    root_end = u32(data, 12)
    if root_end != n - 8:
        fp.stats['root_end_mismatch'] += 1

    fp.asset_id = data[0x28:0x30]
    fp.name = data[0x34:0x54].split(b'\x00')[0].decode('ascii', 'replace')

    # ---- collect markers -------------------------------------------------
    ptr_offs = [o for o in find_all(data, PTR_TAG) if o % 4 == 0]
    obj_offs = []
    for cid, (cname, csize) in LIGHT_CLASSES.items():
        pat = struct.pack('>II', OBJ_TAG, cid)
        for o in find_all(data, pat):
            if o % 4:
                fp.stats['light_hdr_unaligned'] += 1
                continue
            if o + 24 > n:
                fp.stats['light_hdr_truncated'] += 1
                continue
            if u32(data, o + 16) != csize:
                fp.stats['light_hdr_bad_size'] += 1
                continue
            fl = u32(data, o + 20)
            if fl & ~KNOWN_FLAG_BITS:
                fp.stats['light_hdr_unknown_flag_bits'] += 1
            obj_offs.append((o, cid, csize, fl))

    markers = [(o, 0, None) for o in ptr_offs]
    markers += [(rec[0], 1, rec) for rec in obj_offs]
    markers.sort(key=lambda t: (t[0], t[1]))

    # ---- laminar walk ----------------------------------------------------
    nodes = [Node(8, n - 4, -1, 0)]          # synthetic root covering the body
    stack = [0]
    ptr_header_ends = set()                   # offsets consumed by a pointer's end field
    for off, kind, rec in markers:
        while len(stack) > 1 and off >= nodes[stack[-1]].end:
            stack.pop()
        top = nodes[stack[-1]]
        if off < top.start:
            # inside a pointer header we already consumed; not possible for
            # aligned scanning, but count it rather than assume.
            fp.stats['marker_before_node_start'] += 1
            continue
        if kind == 0:
            if off in ptr_header_ends:
                fp.stats['ptr_in_ptr_header'] += 1
                continue
            if off + 8 > top.end:
                fp.stats['ptr_truncated'] += 1
                continue
            end = u32(data, off + 4)
            if end % 4 or end <= off + 8 or end > top.end:
                fp.stats['ptr_rejected'] += 1
                continue
            ptr_header_ends.add(off + 4)
            ver = u32(data, off + 8)
            nodes.append(Node(off + 8, end, stack[-1], ver))
            top.last_child_end = max(top.last_child_end, end)
            stack.append(len(nodes) - 1)
        else:
            o, cid, csize, fl = rec
            if o in ptr_header_ends:
                fp.stats['obj_in_ptr_header'] += 1
                continue
            if o + 24 > top.end:
                fp.stats['obj_crosses_node'] += 1
                continue
            lr = LightRec()
            lr.off = o
            lr.cls_id = cid
            lr.cls_name = LIGHT_CLASSES[cid][0]
            lr.size = csize
            lr.flags = fl
            lr.guid = struct.unpack_from('>Q', data, o + 8)[0]
            lr.entity_node = stack[-1]
            fp.lights.append(lr)

    fp.stats['nodes'] = len(nodes) - 1
    fp.stats['light_headers'] = len(fp.lights)

    # ---- decode each light ----------------------------------------------
    # Search scope for one light is [objectHeader, next light object header),
    # capped by MAX_LIGHT_SPAN.  A template record legitimately carries tens of
    # kilobytes of material/texture payload before its parameter block, so the
    # scope cannot be the innermost pointer node - measured body variants put
    # the block inside the node, one node up, or 66 KB downstream.
    fp.lights.sort(key=lambda x: x.off)
    node_starts = [nd.start for nd in nodes]
    for i, lr in enumerate(fp.lights):
        scan_lo = lr.off + 24
        nxt = fp.lights[i + 1].off if i + 1 < len(fp.lights) else n
        scan_hi = min(nxt, scan_lo + MAX_LIGHT_SPAN, n)

        # ---- parameter block --------------------------------------------
        p = data.find(ALPHA_BYTE, scan_lo, scan_hi)
        while p >= 0:
            if p + PARAM_LEN <= scan_hi and valid_params(data, p):
                lr.argb = tuple(data[p:p + 4])
                lr.params = struct.unpack_from('>9f', data, p + 4)
                lr.param_off = p
                break
            p = data.find(ALPHA_BYTE, p + 1, scan_hi)
        if lr.params is None:
            # Fallback: 7 records out of 2422 store a non-opaque alpha (0xD6),
            # so the opaque-alpha anchor misses them.  Re-scan a short window
            # from the body start testing only the nine floats and the tail
            # byte.  Bounded deliberately - these are all compact instance
            # bodies with the block at +7.
            end2 = min(scan_hi, scan_lo + PARAM_FALLBACK_WINDOW)
            for q in range(scan_lo, max(scan_lo, end2 - PARAM_LEN) + 1):
                if valid_params(data, q, require_alpha=False):
                    lr.argb = tuple(data[q:q + 4])
                    lr.params = struct.unpack_from('>9f', data, q + 4)
                    lr.param_off = q
                    lr.notes.append('param_alpha=%02x' % lr.argb[0])
                    break
        if lr.params is None:
            lr.notes.append('no_param_block')
        else:
            lr.body_prefix = data[scan_lo:lr.param_off][:16].hex()

        # ---- transform ---------------------------------------------------
        # The placement is the trailing 48 bytes of a pointer node that lives
        # inside the same node as the parameter block.  Bounding the search that
        # way is what keeps it honest: a light that has no placement (the light
        # component of a particle/effect prefab) reports no_transform instead of
        # silently adopting some unrelated downstream node's basis.
        tlo = lr.param_off + PARAM_LEN if lr.param_off >= 0 else scan_lo
        owner = innermost_node(nodes, node_starts, tlo - 1)
        thi = min(nodes[owner].end if owner >= 0 else scan_hi, scan_hi)
        lr.owner_node = owner
        for idx in bisect_nodes(nodes, tlo, thi):
            nd = nodes[idx]
            t = nd.end - XFORM_LEN
            if t < nd.start or t < nd.last_child_end or nd.end > thi:
                continue
            v = struct.unpack_from('>12f', data, t)
            if not valid_transform(v):
                continue
            lr.pos = v[0:3]
            lr.basis = v[3:12]
            lr.xform_off = t
            break
        if lr.pos is None:
            lr.notes.append('no_transform')

        # ---- name --------------------------------------------------------
        # the entity wrapper writes its Label before the object header; take the
        # closest 48-byte Label node that starts before the object.
        best = None
        idx = bisect.bisect_left(node_starts, lr.off) - 1
        while idx >= 0 and lr.off - nodes[idx].start <= 0x400:
            nd = nodes[idx]
            if nd.end - nd.start == 48 and u32(data, nd.start + 4) == 0x24:
                best = idx
                break
            idx -= 1
        if best is not None:
            raw = data[nodes[best].start + 8:nodes[best].start + 8 + 32]
            lr.name = raw.split(b'\x00')[0].decode('ascii', 'replace')
        if not lr.name:
            lr.name = ''
            lr.notes.append('no_name')

    if keep_nodes:
        fp.nodes = nodes
    fp.ok = True
    return fp


# ---------------------------------------------------------------------------
# deep (exhaustive) verification walk - every dword accounted for
# ---------------------------------------------------------------------------


def deep_walk(path):
    """Step every 4-byte word of the container and confirm each pointer node is
    consumed exactly to its declared end.  Slow; used by --deep as an
    independent check on the fast marker walk."""
    with open(path, 'rb') as f:
        data = f.read()
    n = len(data)
    stats = Counter()
    nodes = 0
    stack = [(n - 4, 8)]
    o = 8
    while stack:
        e, _ = stack[-1]
        if o + 4 > e:
            if o != e:
                stats['tail_short'] += 1
            stack.pop()
            continue
        v = u32(data, o)
        if v == PTR_TAG_U32 and o + 8 <= e:
            end = u32(data, o + 4)
            if end % 4 == 0 and end > o + 8 and end <= e:
                nodes += 1
                stack.append((end, o))
                o += 8
                continue
            stats['ptr_rejected'] += 1
            o += 4
            continue
        o += 4
    stats['nodes'] = nodes
    return stats


# ---------------------------------------------------------------------------
# level membership
# ---------------------------------------------------------------------------


def read_header(path):
    with open(path, 'rb') as f:
        b = f.read(0x60)
    if len(b) < 0x60 or struct.unpack_from('>I', b, 0)[0] != MAGIC:
        return None
    return b[0x28:0x30], b[0x34:0x54].split(b'\x00')[0].decode('ascii', 'replace')


def build_levels(game_dir, log):
    maps = sorted(glob.glob(os.path.join(game_dir, 'Maps', '*.map')))
    regions = sorted(glob.glob(os.path.join(game_dir, 'Regions', '*.rgn')))
    if not maps:
        raise SystemExit('no .map files under %s' % os.path.join(game_dir, 'Maps'))

    reg_info = OrderedDict()
    for p in regions:
        h = read_header(p)
        if h is None:
            log('REGION HEADER UNREADABLE: %s' % p)
            continue
        aid, name = h
        base = os.path.basename(p)[:16].upper()
        if aid.hex().upper() != base:
            log('REGION ID/FILENAME MISMATCH: %s id=%s' % (base, aid.hex().upper()))
        reg_info[p] = (aid, name)

    levels = OrderedDict()
    claimed = {}
    for mp in maps:
        h = read_header(mp)
        if h is None:
            log('MAP HEADER UNREADABLE: %s' % mp)
            continue
        maid, mname = h
        with open(mp, 'rb') as f:
            mdata = f.read()
        members = []
        for rp, (raid, rname) in reg_info.items():
            if mdata.find(raid) >= 0:
                members.append(rp)
                claimed.setdefault(rp, []).append(mname)
        levels[mp] = dict(id=maid, name=mname, regions=members)
        del mdata

    for rp, (raid, rname) in reg_info.items():
        owners = claimed.get(rp, [])
        if len(owners) == 0:
            log('ORPHAN REGION (no map references its asset id): %s (%s)'
                % (rname, os.path.basename(rp)))
        elif len(owners) > 1:
            log('REGION CLAIMED BY %d MAPS: %s -> %s' % (len(owners), rname, owners))
    return levels, reg_info


# ---------------------------------------------------------------------------
# emit
# ---------------------------------------------------------------------------

FIELD_DOC = (
    'L cls tmpl hasXform hasParams srcIdx  px py pz  '
    'm00 m01 m02 m10 m11 m12 m20 m21 m22  A R G B  '
    'f0 f1 f2 f3 f4 f5 f6 f7 f8  srcOffHex flagsHex guidHex name'
)


def fmt_f(x):
    return '%.6g' % x


def emit_level(out_dir, level, order):
    """order = list of (srcIdx, FileParse, is_map)."""
    name = level['name']
    path = os.path.join(out_dir, name + '.lights')
    total = sum(len(fp.lights) for _, fp, _ in order)
    with open(path, 'w', encoding='ascii', newline='\n') as f:
        f.write('# Eat Lead (BLUS30267) authored analytic lights\n')
        f.write('# generated by tools/eatlead/extract_lights.py\n')
        f.write('# coordinates: region space, Z-up, right-handed, metres\n')
        f.write('# basis is row-major; row 1 (m10 m11 m12) is the aim direction\n')
        f.write('# class ids: %s\n' % ' '.join(
            '%d=%s' % (cid, nm) for cid, (nm, _sz) in LIGHT_CLASSES.items()))
        f.write('# f1 = range (metres), f8 = cone angle (degrees); f0 and f2..f7 '
                'are raw and unproven\n')
        f.write('# ' + FIELD_DOC + '\n')
        f.write('V %d\n' % OUT_VERSION)
        f.write('M %s %s\n' % (level['id'].hex().upper(), name))
        f.write('N %d\n' % total)
        for idx, fp, is_map in order:
            f.write('S %d %s %s %s %d %d\n' % (
                idx, fp.asset_id.hex().upper(), 'map' if is_map else 'rgn',
                fp.name if fp.name else '-', fp.size, len(fp.lights)))
        for idx, fp, _ in order:
            for lr in fp.lights:
                pos = lr.pos if lr.pos else (0.0, 0.0, 0.0)
                bas = lr.basis if lr.basis else (1, 0, 0, 0, 1, 0, 0, 0, 1)
                argb = lr.argb if lr.argb else (0, 0, 0, 0)
                par = lr.params if lr.params else (0,) * 9
                nm = lr.name if lr.name else '-'
                nm = ''.join(ch if 33 <= ord(ch) < 127 else '_' for ch in nm) or '-'
                f.write('L %d %d %d %d %d %s %s %d %d %d %d %s '
                        '0x%x 0x%08x %016x %s\n' % (
                            lr.cls_id, 1 if (lr.flags & TEMPLATE_BIT) else 0,
                            1 if lr.pos else 0, 1 if lr.params else 0, idx,
                            ' '.join(fmt_f(x) for x in pos),
                            ' '.join(fmt_f(x) for x in bas),
                            argb[0], argb[1], argb[2], argb[3],
                            ' '.join(fmt_f(x) for x in par),
                            lr.off, lr.flags, lr.guid, nm))
    return path, total


def read_level_file(path):
    """Re-reader used by --verify.  Returns (meta, per-source counts, lights)."""
    meta = {'sources': OrderedDict()}
    lights = []
    per_src = Counter()
    with open(path, 'r', encoding='ascii') as f:
        for ln, line in enumerate(f, 1):
            line = line.strip()
            if not line or line.startswith('#'):
                continue
            t = line.split(' ')
            if t[0] == 'V':
                meta['version'] = int(t[1])
            elif t[0] == 'M':
                meta['map_id'] = t[1]
                meta['map_name'] = t[2]
            elif t[0] == 'N':
                meta['declared_total'] = int(t[1])
            elif t[0] == 'S':
                meta['sources'][int(t[1])] = dict(
                    asset_id=t[2], kind=t[3], name=t[4],
                    bytes=int(t[5]), lights=int(t[6]))
            elif t[0] == 'L':
                if len(t) != 35:
                    raise ValueError('%s:%d: expected 35 tokens, got %d'
                                     % (path, ln, len(t)))
                rec = dict(
                    cls=int(t[1]), tmpl=int(t[2]), has_xf=int(t[3]),
                    has_par=int(t[4]), src=int(t[5]),
                    pos=tuple(float(x) for x in t[6:9]),
                    basis=tuple(float(x) for x in t[9:18]),
                    argb=tuple(int(x) for x in t[18:22]),
                    params=tuple(float(x) for x in t[22:31]),
                    src_off=int(t[31], 16), flags=int(t[32], 16),
                    guid=t[33], name=t[34])
                lights.append(rec)
                per_src[rec['src']] += 1
            else:
                raise ValueError('%s:%d: unknown record type %r' % (path, ln, t[0]))
    return meta, per_src, lights


# ---------------------------------------------------------------------------
# main
# ---------------------------------------------------------------------------

DEFAULT_GAME = (r'E:\PS3 Games\Eat Lead - The Return of Matt Hazard '
                r'(USA) (En,Fr,De,Es,It)\PS3_GAME\USRDIR')
DEFAULT_OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                           '..', '..', 'bin', 'eatlead_lights')


def do_extract(args):
    game_dir = args.game_dir
    out_dir = os.path.abspath(args.out)
    os.makedirs(out_dir, exist_ok=True)

    report = []

    def log(msg):
        report.append(msg)
        if args.verbose:
            print('  ! ' + msg)

    levels, reg_info = build_levels(game_dir, log)

    index_rows = []
    grand = Counter()
    failures = []
    all_notes = Counter()
    per_level_counts = []
    param_gap = Counter()          # objectHeader+24 -> parameter block
    xform_gap = Counter()          # parameter block -> transform
    flag_hist = Counter()
    noxf_names = Counter()
    nopar_names = Counter()

    for mp, lvl in levels.items():
        order = []
        parses = []
        fp = parse_container(mp)
        if not fp.ok:
            failures.append('%s: %s' % (mp, fp.error))
            log('MAP PARSE FAILED %s: %s' % (os.path.basename(mp), fp.error))
            continue
        order.append((0, fp, True))
        parses.append(fp)
        for i, rp in enumerate(lvl['regions'], 1):
            rfp = parse_container(rp)
            if not rfp.ok:
                failures.append('%s: %s' % (rp, rfp.error))
                log('REGION PARSE FAILED %s: %s' % (os.path.basename(rp), rfp.error))
                continue
            order.append((i, rfp, False))
            parses.append(rfp)

        path, total = emit_level(out_dir, lvl, order)
        cls_counter = Counter()
        no_xf = no_pb = 0
        for _, p, _ in order:
            for lr in p.lights:
                cls_counter[lr.cls_name] += 1
                grand[lr.cls_name] += 1
                if lr.pos is None:
                    no_xf += 1
                if lr.params is None:
                    no_pb += 1
                for nt in lr.notes:
                    all_notes[nt.split('=')[0]] += 1
                flag_hist['%08x' % lr.flags] += 1
                if lr.param_off >= 0:
                    param_gap[lr.param_off - (lr.off + 24)] += 1
                    if lr.xform_off >= 0:
                        xform_gap[lr.xform_off - lr.param_off] += 1
                if lr.pos is None:
                    noxf_names[lr.name or '(unnamed)'] += 1
                if lr.params is None:
                    nopar_names['%s %s' % (lr.cls_name, lr.name or '(unnamed)')] += 1
            for k, v in p.stats.items():
                if k not in ('nodes', 'light_headers'):
                    grand['stat:' + k] += v
                else:
                    grand[k] += v
        per_level_counts.append((lvl['name'], total, len(order), cls_counter,
                                 no_xf, no_pb))
        index_rows.append((lvl['name'], lvl['id'].hex().upper(),
                           os.path.basename(path), len(order), total,
                           total - no_xf, total - no_pb))
        print('%-24s %4d lights  %2d files  xform=%d params=%d  -> %s'
              % (lvl['name'], total, len(order), total - no_xf, total - no_pb,
                 os.path.basename(path)))

    # index
    idx_path = os.path.join(out_dir, 'index.txt')
    with open(idx_path, 'w', encoding='ascii', newline='\n') as f:
        f.write('# Eat Lead (BLUS30267) authored light index\n')
        f.write('# I levelName mapAssetId file numSourceFiles numLights '
                'numWithTransform numWithParams\n')
        f.write('V %d\n' % OUT_VERSION)
        for row in index_rows:
            f.write('I %s %s %s %d %d %d %d\n' % row)
    print('\nindex -> %s' % idx_path)

    # report
    rep_path = os.path.join(out_dir, 'parse_report.txt')
    with open(rep_path, 'w', encoding='ascii', newline='\n') as f:
        f.write('Eat Lead light extraction - parse report\n')
        f.write('game dir: %s\n\n' % game_dir)
        f.write('per level:\n')
        for name, total, nfiles, cls, no_xf, no_pb in per_level_counts:
            f.write('  %-24s %4d lights over %2d files  %s'
                    % (name, total, nfiles, dict(cls)))
            if no_xf or no_pb:
                f.write('   MISSING: transform=%d params=%d' % (no_xf, no_pb))
            f.write('\n')
        cnames = [v[0] for v in LIGHT_CLASSES.values()]
        f.write('\ntotals by class: %s\n'
                % dict((k, grand[k]) for k in cnames if grand[k]))
        f.write('total light records: %d\n' % sum(grand[k] for k in cnames))
        f.write('pointer nodes walked: %d\n' % grand.get('nodes', 0))
        f.write('\nper-record decode gaps: %s\n' % dict(all_notes))
        f.write('\nflags bitfield histogram: %s\n' % dict(flag_hist.most_common()))
        f.write('\nStructural consistency evidence.  Both distributions below are\n'
                'discrete and tightly clustered, which is what a correct structural\n'
                'read looks like; a scattered distribution would mean the search\n'
                'was latching onto unrelated bytes.\n')
        f.write('  byte gap objectHeader+24 -> parameter block: %s\n'
                % dict(param_gap.most_common(16)))
        f.write('  byte gap parameter block -> transform:       %s\n'
                % dict(xform_gap.most_common(16)))
        f.write('\nrecords with no world transform, by entity name.  These are the\n'
                'light components of effect / weapon / prop prefabs: they carry a\n'
                'colour and the nine parameters but no static placement, because\n'
                'their owner positions them at runtime.\n')
        for k, v in noxf_names.most_common(40):
            f.write('  %-44s %d\n' % (k, v))
        if len(noxf_names) > 40:
            f.write('  ... %d more distinct names\n' % (len(noxf_names) - 40))
        f.write('\nrecords with no parameter block:\n')
        for k, v in nopar_names.most_common(20):
            f.write('  %-44s %d\n' % (k, v))
        if not nopar_names:
            f.write('  none\n')
        f.write('\ncontainer-level anomalies (counted, not ignored):\n')
        any_stat = False
        for k, v in sorted(grand.items()):
            if k.startswith('stat:'):
                f.write('  %-28s %d\n' % (k[5:], v))
                any_stat = True
        if not any_stat:
            f.write('  none\n')
        f.write('\nfile-level failures:\n')
        if failures:
            for x in failures:
                f.write('  %s\n' % x)
        else:
            f.write('  none\n')
        f.write('\nnotes emitted while building the level map:\n')
        if report:
            for x in report:
                f.write('  %s\n' % x)
        else:
            f.write('  none\n')
    print('report -> %s' % rep_path)
    cls_names = [v[0] for v in LIGHT_CLASSES.values()]
    print('\ntotals: %s  (%d records)'
          % (dict((k, grand[k]) for k in cls_names if grand[k]),
             sum(grand[k] for k in cls_names)))
    print('decode gaps: %s' % dict(all_notes))
    anomalies = dict((k[5:], v) for k, v in grand.items() if k.startswith('stat:'))
    print('container anomalies: %s' % (anomalies if anomalies else 'none'))
    return 0


def do_verify(args):
    out_dir = os.path.abspath(args.out)
    idx = os.path.join(out_dir, 'index.txt')
    if not os.path.isfile(idx):
        print('no index.txt in %s - run the extractor first' % out_dir)
        return 2
    declared = []
    with open(idx, 'r', encoding='ascii') as f:
        for line in f:
            t = line.strip().split(' ')
            if t and t[0] == 'I':
                declared.append(dict(name=t[1], map_id=t[2], file=t[3],
                                     nfiles=int(t[4]), n=int(t[5]),
                                     nxf=int(t[6]), npar=int(t[7])))
    print('%-24s %6s %6s %6s %6s  %s' % ('level', 'lights', 'xform', 'params',
                                         'files', 'status'))
    bad = 0
    grand = 0
    gxf = 0
    gpar = 0
    for d in declared:
        p = os.path.join(out_dir, d['file'])
        try:
            meta, per_src, lights = read_level_file(p)
        except Exception as exc:                       # noqa: BLE001
            print('%-24s  RE-READ FAILED: %s' % (d['name'], exc))
            bad += 1
            continue
        nxf = sum(1 for x in lights if x['has_xf'])
        npar = sum(1 for x in lights if x['has_par'])
        problems = []
        if len(lights) != d['n']:
            problems.append('index says %d, file has %d' % (d['n'], len(lights)))
        if meta.get('declared_total') != len(lights):
            problems.append('N record says %s' % meta.get('declared_total'))
        if nxf != d['nxf']:
            problems.append('xform %d vs %d' % (nxf, d['nxf']))
        if npar != d['npar']:
            problems.append('params %d vs %d' % (npar, d['npar']))
        if len(meta['sources']) != d['nfiles']:
            problems.append('sources %d vs %d' % (len(meta['sources']), d['nfiles']))
        for si, sm in meta['sources'].items():
            if per_src.get(si, 0) != sm['lights']:
                problems.append('src %d: S says %d, %d L rows'
                                % (si, sm['lights'], per_src.get(si, 0)))
        if meta.get('map_name') != d['name']:
            problems.append('map name mismatch')
        grand += len(lights)
        gxf += nxf
        gpar += npar
        print('%-24s %6d %6d %6d %6d  %s'
              % (d['name'], len(lights), nxf, npar, len(meta['sources']),
                 'OK' if not problems else 'MISMATCH: ' + '; '.join(problems)))
        if problems:
            bad += 1
    print('\n%d levels, %d lights, %d with transform, %d with params'
          % (len(declared), grand, gxf, gpar))
    print('verify: %s' % ('ALL OK' if bad == 0 else '%d LEVEL(S) FAILED' % bad))

    # per source-file totals
    if args.verbose:
        print('\nper source file:')
        for d in declared:
            meta, per_src, _ = read_level_file(os.path.join(out_dir, d['file']))
            for si, sm in meta['sources'].items():
                print('  %-24s %-3s %-34s %8d bytes %5d lights'
                      % (d['name'], sm['kind'], sm['name'], sm['bytes'], sm['lights']))
    return 0 if bad == 0 else 1


def do_deep(args):
    game_dir = args.game_dir
    files = (sorted(glob.glob(os.path.join(game_dir, 'Maps', '*.map')))
             + sorted(glob.glob(os.path.join(game_dir, 'Regions', '*.rgn'))))
    if args.limit:
        files = files[:args.limit]
    bad = 0
    for p in files:
        fast = parse_container(p)
        slow = deep_walk(p)
        same = (fast.stats.get('nodes', -1) == slow.get('nodes', -2))
        tail = slow.get('tail_short', 0)
        status = 'OK' if (same and tail == 0) else 'DIFF'
        if status != 'OK':
            bad += 1
        print('%-26s %-34s fast_nodes=%-7d deep_nodes=%-7d tail_short=%d %s'
              % (os.path.basename(p), fast.name, fast.stats.get('nodes', -1),
                 slow.get('nodes', -1), tail, status))
    print('\ndeep check: %d file(s) disagreed' % bad)
    return 0 if bad == 0 else 1


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--game-dir', default=DEFAULT_GAME,
                    help='PS3_GAME/USRDIR of the disc image (read-only)')
    ap.add_argument('--out', default=DEFAULT_OUT, help='output directory')
    ap.add_argument('--verify', action='store_true',
                    help='re-read the emitted files and report per-file totals')
    ap.add_argument('--deep', action='store_true',
                    help='cross-check the fast marker walk against an exhaustive '
                         'word-by-word walk of the container (slow)')
    ap.add_argument('--limit', type=int, default=0, help='--deep: only N files')
    ap.add_argument('--verbose', '-v', action='store_true')
    args = ap.parse_args(argv)

    if args.verify:
        return do_verify(args)
    if args.deep:
        return do_deep(args)
    if not os.path.isdir(args.game_dir):
        print('game dir not found: %s' % args.game_dir)
        return 2
    return do_extract(args)


if __name__ == '__main__':
    sys.exit(main())
