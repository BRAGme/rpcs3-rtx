r"""Volition "Cg Binary" (CGB) shader parser -- Saints Row 2, PS3.

    python docs/remix/cgbdis.py list    <file>
    python docs/remix/cgbdis.py params  <file> [-b N] [--raw]
    python docs/remix/cgbdis.py extract <file> <outdir>
    python docs/remix/cgbdis.py match   <shaderdir> <ucodedir> [--since YYYY-MM-DD]

Sibling of vpdis.py / fpdis.py: this reads the shipped shader pack, those read the
microcode once it is out. `extract` writes .vp/.fp slices that vpdis.py and fpdis.py
open directly -- no further byte munging (see BYTE ORDER below).

Input is the 318 files unpacked from
    PS3_GAME/USRDIR/packfiles/ps3/shaders.vpp_ps3   (unpack with vppdis.py)

WHAT THIS IS FOR
    A parameter record gives a NAME and a CONSTANT REGISTER INDEX. That is what turns
    `c[10]` in a vpdis.py listing into `V_eye_pos`, and it is the whole point of the file.

================================================================================
TWO CONTAINERS
================================================================================
12 of the 318 files are a bare CGB blob. The other 304 begin with magic 'KB\xa1\xee'
and hold several CGB blobs each (they are the "building block" shader permutation
sets -- the bb-* names). The KB index table is NOT decoded here and is not needed: a
CGB blob states its own total size exactly, so scanning for the magic and walking the
size chain recovers every blob.

  EVIDENCE  4722 blobs found across all 318 files by scanning for 'CGB\0'; for every
            one of the 4722, 0x20 + ucodeSize + constSize + paramSize lands inside the
            file and the parameter section's string table ends on the last byte of the
            blob with zero slack. A wrong record size or a wrong section header size
            would leave a remainder on essentially every blob; it leaves none on any.

================================================================================
CGB BLOB LAYOUT  (everything big-endian)
================================================================================
  +0x00  char[4]  'CGB\0'
  +0x04  u32      0                      -- 4722/4722 zero
  +0x08  u16      ucodeSize, in bytes
  +0x0a  u8       programType: 0 = vertex, 1 = fragment
  +0x0b  u8       3                      -- 4722/4722; format version, presumed
  +0x0c  ...      per-type header, below
  +0x20  u8[]     microcode, ucodeSize bytes
  +0x20+ucodeSize        literal-constant section  (see below)
         +constSize      parameter section         (see below)

  EVIDENCE for ucodeSize@0x08 and ucode@0x20: on the five smallest single-program
  files (dummy 0x10, tex_quincunx 0x10, 2d_quincunx 0x20, notex 0x40, transform_notex
  0x50) the u16 at +0x08 is exactly the byte distance from 0x20 to the next section
  header, and the bytes in between disassemble cleanly. It then holds for all 4722.

VERTEX header (programType 0)
  +0x0c  u16      attributeInputMask -- bit N set <=> the program reads v[N]
  +0x0e  u8       temp-register count
  +0x0f  u8       0                      -- 2360/2360 zero
  +0x10  u32      attributeOutputMask (32-bit, so its HIGH half is at 0x10)

  EVIDENCE attributeInputMask: decoding every VP's instruction stream and collecting
           the v<N> operands, the read set is a subset of this mask on 2360/2360
           blobs, and equals the set of parameter resources classed as attributes on
           2360/2360.
  EVIDENCE attributeOutputMask: rebuilding the mask from the o[N] registers each
           program actually writes, with the bit table
               o[1]COL0->0 o[2]COL1->1 o[3]BFC0->2 o[4]BFC1->3 o[5]FOGC->4
               o[6]PSIZ->5 o[7..14]TEX0..TEX7->14..21 o[15]->12
           reproduces u32@0x10 EXACTLY on 2360/2360 blobs. o[0] (HPOS) has no bit.
           Note this is why it must be read as a u32: a program writing TEX0..TEX4
           needs bits 14..18, and bits 16+ live in the half at 0x10.
  EVIDENCE temp count: (highest temp register touched + 1) == byte@0x0e on
           2256/2360 blobs (95.6%). Not universal -- treat as advisory, not proven.

FRAGMENT header (programType 1)
  +0x0c  u32      attributeInputMask -- same 32-bit bit space as the VP output mask
  +0x10  u16      texCoordsInputMask, presumed
  +0x12  u16      texCoords2D, presumed
  +0x1a  u16      RSX shader-control flags, presumed
  +0x1c  u8       fragment temp-register count, presumed
  (+0x14..0x19 and +0x1e are 0 on 2362/2362)

  EVIDENCE attributeInputMask: its value distribution is the same one the VP output
           mask takes, and 2012/2362 FP blobs carry a value that some VP blob also
           carries as an output mask. A crude scan of FP attribute reads is a subset
           of this mask on 2353/2362. Solid but not airtight.
  NOT PROVEN  0x10 matches a scan of TEXn reads on 2038/2362 (mismatches are all
           "mask declares one more coord than the scan found"); 0x12 equals
           ~u16@0x10 on 1989/2362, so it is a separate field that usually looks like
           the complement; 0x1a takes only {0x8400, 0x8440, 0x844e, 0x840e}, which
           has the shape of an RSX SET_SHADER_CONTROL word (0x40 = 32-bit exports,
           0x0e = depth export) but nothing here proves it; byte@0x1c ranges 2..0x13
           and is presumed the temp count -- my FP register scan is too crude to
           confirm it and disagrees more often than it agrees.

================================================================================
LITERAL-CONSTANT SECTION  (at 0x20 + ucodeSize)
================================================================================
  +0x00  u16      sectionSize
  +0x02  u16      count
  +0x04  u16[count]   constant register index, one per entry
  ... padding to sectionSize - 16*count ...
  then   count x 16 bytes: the float4 value, big-endian

  EVIDENCE  sectionSize == 16 * (1 + count) on 4722/4722 blobs -- a 16-byte header
            plus one 16-byte float4 per entry, so both the header size and the entry
            size are pinned. count is 0, 1 or 2 and is 0 on 2362/2362 fragment
            programs (fragment literals are embedded in the ucode instead, below).
            The only register indices that ever appear are 467 (2243x) and 466
            (694x) -- the top of the 468-entry RSX vertex constant file, which is
            where Cg allocates compiler literals, downward.
  CROSS-CHECK  the live-captured vertex program B21803879233E34B reads c[467] and at
            runtime that register holds (0.00787402, 1, 0, 0). The blob this ucode
            came from (bb_part_standard_additive_pb @0x140) stores exactly
            (0.00787402, 1.0, 0.0, 0.0) at register 467. Byte-for-byte agreement
            between the shipped file and an independently observed runtime value.

================================================================================
PARAMETER SECTION
================================================================================
  +0x00  u16      sectionSize
  +0x02  u16      paramCount
  +0x04  u16      constTableWords   (0 on every vertex program)
  +0x06  record[paramCount]         8 bytes each
         u32   nameOffset           (the high half is 0 on all 48353 records)
         u16   parentIndex          0xffff = top level, else index of the owning
                                    struct/array parameter in this same list
         u16   resource             see below; 0xffff = none (structs and arrays)
  then   u16[constTableWords]       fragment embedded-constant table (below)
  then   the string table: a leading NUL then NUL-terminated names, so offset 0 is
         the empty string and a real name never has offset 0.

  EVIDENCE  the string table ends on the final byte of the section with zero slack on
            4722/4722 blobs, and all 48353 nameOffsets resolve to a NUL-terminated
            string inside it. That simultaneously pins the 6-byte section header, the
            8-byte record, and the 2*constTableWords table between them -- get any one
            wrong and the string table stops landing flush.
  TRAP      the string table does NOT start right after the records. It starts after
            the embedded-constant table, i.e. at recordsEnd + 2*constTableWords. On a
            vertex program that table is empty so the mistake is invisible; on a
            fragment program every name comes out as garbage that still happens to be
            NUL-terminated, so it fails silently.

`resource` -- VERTEX
  A raw index. It is an ATTRIBUTE index (v[N]) for the varying inputs and a CONSTANT
  REGISTER index (c[N]) for the uniforms, and THE RECORD DOES NOT SAY WHICH.

  The split is positional: every varying parameter precedes every uniform. Take the
  unique prefix of the list whose resources are exactly the set bits of
  attributeInputMask; that prefix is the attributes, the rest are uniforms.

  EVIDENCE  such a prefix exists and is UNIQUE on 2360/2360 vertex blobs. The split
            it produces never contradicts itself: across all 2360 blobs the set of
            names it calls attributes and the set it calls uniforms do not overlap by
            even one name, and they come out as exactly the two vocabularies you would
            expect -- {pos, norm, tang, uv0..uv3, vweights, vindices, tweenwt,
            __morph_pos1, ...} against {V_MVP_matrix, V_eye_pos, V_bones,
            VS_packed_const_N, ...}.
  WHY IT MATTERS  the obvious shortcut -- "resource is an attribute if its bit is set
            in attributeInputMask" -- is WRONG. 1549 of the 2360 vertex blobs contain
            two parameters with the same resource number (e.g. attribute norm=2 and
            uniform V_light_pos=2), so the mask alone is ambiguous on 66% of blobs and
            silently mislabels V_eye_pos, V_fog_dist, V_light_pos and
            View_proj_matrix as attributes.

`resource` -- FRAGMENT
    res  < 16     texture unit for a sampler
    res >= 1024   1024 + word index into the embedded-constant table
    res == 0xffff struct or array parent, no resource

  EVIDENCE  no fragment parameter ever takes a value in 16..1023 (21377 records).
            Sampler units are per-program (Diffuse_MapSampler is unit 0 in 1156 blobs
            but 1, 2 or 4 in others), which is what a real texture-unit allocation
            looks like; the >=1024 values are not, see below.

EMBEDDED-CONSTANT TABLE (fragment only)
  A fragment program has no constant register file -- its constants sit inside the
  instruction stream and the engine patches them in place. Each entry is

         u16  constant slot index
         u16  patchCount
         u16  patchOffset[patchCount]    byte offset into the ucode

  and a parameter points at its entry by word index (resource - 1024).

  EVIDENCE  walking the entries reachable from the parameters consumes every word of
            the table exactly once -- no gap, no overlap -- on 2134/2134 blobs that
            have one. Every one of the resulting patchOffsets is 4-byte aligned and
            below ucodeSize, with 0 violations.
  EVIDENCE for "slot index": the first u16 is a function of the NAME, not of the
            program. V_tint is 4 in all 1827 occurrences, V_fog_color 6 in all 1319,
            V_fog_color2 14 in all 1319, V_ambient_render 5 in all 872, V_light_color
            2 in all 1267, and PS_packed_const_0..3 are 24, 25, 26, 27 -- consecutive,
            exactly as a packed constant array would be. 88 of 89 fragment names have
            a single value across all 2362 blobs.
  EVIDENCE that it is the SAME numbering as the vertex `resource`: four names appear
            on both sides and all four agree -- V_fog_dist 14/14, V_time_of_day 8/8,
            Target_dims 9/9, V_light_atten 0/0.

WHAT THE FORMAT DOES NOT RECORD -- stated because it cannot be recovered
  * There is NO TYPE FIELD. Nothing anywhere says float4 vs float4x4 vs sampler2D.
    The u32 nameOffset's high half is zero on all 48353 records, so it is not hiding
    there. Sampler-ness is inferable on fragment programs (resource < 16) and from
    the name elsewhere, but a matrix is indistinguishable from a float4 except by
    how many registers the next parameter leaves free. V_MVP_matrix at c[4] occupies
    c[4..7] only because V_eye_pos is at c[10]; the file never says "4 rows".
  * There is no array length either. An array shows up as a parent record with
    resource 0xffff plus one child record per element, the child NAMED by its decimal
    index ("Decal_parameters" with a child literally called "4"), and only the
    elements the program actually referenced are present.
  * The KB container's own index table is not decoded.

================================================================================
BYTE ORDER, and why `extract` is not a plain memcpy
================================================================================
VERTEX ucode is stored big-endian in the CGB and little-endian by RPCS3's capture, so
`extract` byte-swaps each u32. FRAGMENT ucode is stored in the CGB in exactly the
layout RPCS3 captures, half-swapped words and all, and is copied straight through.

  EVIDENCE  with that rule and nothing else, extracted slices are byte-identical to
            live captures: 58 distinct .vp captures match exactly. Fragment slices
            match once the embedded-constant patch sites are excluded from the
            comparison (the runtime overwrites them), which is itself a check on the
            patch offsets: 82 .fp captures match that way.

`match` reports which shaders a directory of captures came from. Be careful reading
the result: bin/remix_ucode/ is CUMULATIVE ACROSS TITLES, so most of what is in there
is not Saints Row 2 and will never match. Sliced by capture date, the Saints Row 2
session (2026-09-12) resolves 141 of 141 captures, while all 1070 captures from other
days resolve 1 -- use --since to avoid reporting a 12% hit rate on a 100% result.
Roughly half the hits name a group of shaders rather than one, because the _c/_mc/_ms/
_s quality variants of a shader frequently compile to identical microcode.
"""
import argparse
import datetime
import os
import struct
import sys

MAGIC = b'CGB\0'

# Vertex output register -> attributeOutputMask bit. Derived by regression against the
# o[N] registers 2360 programs actually write; reproduces the stored mask exactly.
VP_OUT_BIT = {1: 0, 2: 1, 3: 2, 4: 3, 5: 4, 6: 5,
              7: 14, 8: 15, 9: 16, 10: 17, 11: 18, 12: 19, 13: 20, 14: 21, 15: 12}

VP_OUT_NAME = {0: "HPOS", 1: "COL0", 2: "COL1", 3: "BFC0", 4: "BFC1", 5: "FOGC",
               6: "PSIZ", 7: "TEX0", 8: "TEX1", 9: "TEX2", 10: "TEX3", 11: "TEX4",
               12: "TEX5", 13: "TEX6", 14: "TEX7", 15: "OUT15"}

VP_IN_NAME = ["pos", "weight", "normal", "diff_color", "spec_color", "fog",
              "point_size", "in7", "tc0", "tc1", "tc2", "tc3", "tc4", "tc5",
              "tc6", "tc7"]

VERTEX, FRAGMENT = 0, 1


class Param(object):
    __slots__ = ("index", "name", "qualified", "parent", "resource", "kind",
                 "register", "unit", "slot", "patches")

    def __init__(self, index, name, parent, resource):
        self.index = index
        self.name = name
        self.qualified = name
        self.parent = parent
        self.resource = resource
        self.kind = "?"          # attribute | uniform | sampler | container
        self.register = None     # constant register / attribute index
        self.unit = None         # texture unit (fragment samplers)
        self.slot = None         # engine constant slot (fragment uniforms)
        self.patches = []        # ucode byte offsets to patch (fragment uniforms)


class Blob(object):
    """One CGB program. Offsets in the attributes are absolute within `data`."""

    def __init__(self, data, base):
        if data[base:base + 4] != MAGIC:
            raise ValueError("no CGB magic at 0x%x" % base)
        u16 = lambda o: struct.unpack_from('>H', data, o)[0]
        self.data = data
        self.base = base
        self.reserved04 = struct.unpack_from('>I', data, base + 4)[0]
        self.ucode_size = u16(base + 8)
        self.type = data[base + 0x0a]
        self.version = data[base + 0x0b]
        self.ucode_off = base + 0x20

        if self.type == VERTEX:
            self.attr_in_mask = u16(base + 0x0c)
            self.reg_count = data[base + 0x0e]
            self.attr_out_mask = struct.unpack_from('>I', data, base + 0x10)[0]
            self.texcoord_mask = None
            self.shader_control = None
        else:
            self.attr_in_mask = struct.unpack_from('>I', data, base + 0x0c)[0]
            self.texcoord_mask = u16(base + 0x10)
            self.texcoords_2d = u16(base + 0x12)
            self.shader_control = u16(base + 0x1a)
            self.reg_count = data[base + 0x1c]
            self.attr_out_mask = None

        # ---- literal-constant section ----
        c = self.ucode_off + self.ucode_size
        self.const_off = c
        self.const_size = u16(c)
        self.const_count = u16(c + 2)
        self.literals = []
        if self.const_count:
            vbase = c + self.const_size - 16 * self.const_count
            for k in range(self.const_count):
                self.literals.append((u16(c + 4 + 2 * k),
                                      struct.unpack_from('>4f', data, vbase + 16 * k)))

        # ---- parameter section ----
        p = c + self.const_size
        self.param_off = p
        self.param_size = u16(p)
        self.end = p + self.param_size
        count = u16(p + 2)
        self.const_words = u16(p + 4)
        recs = [struct.unpack_from('>IHH', data, p + 6 + 8 * i) for i in range(count)]
        tbl_off = p + 6 + 8 * count
        self.const_table = [u16(tbl_off + 2 * k) for k in range(self.const_words)]
        self.str_off = tbl_off + 2 * self.const_words

        self.params = []
        for i, (noff, parent, res) in enumerate(recs):
            self.params.append(Param(i, self._string(noff), parent, res))
        self._qualify()
        self._classify()

    # -- helpers ----------------------------------------------------------
    def _string(self, off):
        s = self.str_off + off
        e = self.data.find(b'\0', s, self.end)
        if e < 0:
            raise ValueError("unterminated name at +0x%x" % off)
        return self.data[s:e].decode('latin1')

    def _qualify(self):
        for p in self.params:
            if p.parent != 0xffff and p.parent < len(self.params):
                p.qualified = "%s.%s" % (self.params[p.parent].qualified, p.name)

    def _classify(self):
        if self.type == VERTEX:
            bits = sorted(i for i in range(16) if self.attr_in_mask >> i & 1)
            res = [p.resource for p in self.params]
            split = None
            for k in range(len(res) + 1):
                if sorted(x for x in res[:k] if x != 0xffff) == bits:
                    split = k
                    break
            if split is None:            # should not happen; be loud, not wrong
                split = 0
                self.split_ok = False
            else:
                self.split_ok = True
            self.attr_split = split
            for p in self.params:
                if p.resource == 0xffff:
                    p.kind = "container"
                elif p.index < split:
                    p.kind = "attribute"
                    p.register = p.resource
                else:
                    p.kind = "uniform"
                    p.register = p.resource
            return

        self.split_ok = True
        self.attr_split = None
        for p in self.params:
            if p.resource == 0xffff:
                p.kind = "container"
            elif p.resource < 16:
                p.kind = "sampler"
                p.unit = p.resource
            elif p.resource >= 1024:
                p.kind = "uniform"
                w = p.resource - 1024
                if w + 1 < len(self.const_table):
                    p.slot = self.const_table[w]
                    p.register = p.slot
                    n = self.const_table[w + 1]
                    p.patches = self.const_table[w + 2:w + 2 + n]
            else:
                p.kind = "?res%d" % p.resource

    # -- microcode --------------------------------------------------------
    def raw_ucode(self):
        return self.data[self.ucode_off:self.ucode_off + self.ucode_size]

    def ucode_for_tools(self):
        """Bytes in the layout vpdis.py / fpdis.py and bin/remix_ucode expect."""
        u = self.raw_ucode()
        if self.type == VERTEX:
            return b''.join(u[i:i + 4][::-1] for i in range(0, len(u), 4))
        return u

    def const_patch_mask(self):
        """True for each ucode byte the runtime overwrites with a constant."""
        m = bytearray(self.ucode_size)
        for p in self.params:
            for off in p.patches:
                for t in range(16):
                    if off + t < self.ucode_size:
                        m[off + t] = 1
        return bytes(m)

    @property
    def kind_name(self):
        return "vertex" if self.type == VERTEX else "fragment"

    @property
    def suffix(self):
        return ".vp" if self.type == VERTEX else ".fp"


def find_blobs(data):
    """Every CGB blob in a file, bare or inside a KB container."""
    out, i = [], 0
    while True:
        j = data.find(MAGIC, i)
        if j < 0:
            return out
        i = j + 4
        try:
            b = Blob(data, j)
        except Exception:
            continue
        if b.end <= len(data):
            out.append(b)


def _mask_names(mask, names, width=32):
    got = [names.get(i, str(i)) if isinstance(names, dict) else names[i]
           for i in range(width) if mask >> i & 1 and (i < len(names) if
           not isinstance(names, dict) else True)]
    return ",".join(got)


# ------------------------------------------------------------------ commands
def cmd_list(args):
    data = open(args.file, 'rb').read()
    blobs = find_blobs(data)
    print("%s  %d bytes  container=%s  %d CGB blob(s)"
          % (args.file, len(data), "KB" if data[:2] == b'KB' else "bare", len(blobs)))
    for n, b in enumerate(blobs):
        print("  [%2d] @0x%-6x %-8s ucode=0x%-5x literals=%d params=%-3d total=0x%x"
              % (n, b.base, b.kind_name, b.ucode_size, b.const_count,
                 len(b.params), b.end - b.base))


def cmd_params(args):
    data = open(args.file, 'rb').read()
    blobs = find_blobs(data)
    for n, b in enumerate(blobs):
        if args.blob is not None and n != args.blob:
            continue
        print("=== [%d] @0x%x  %s  ucode 0x%x @0x%x  v%d"
              % (n, b.base, b.kind_name, b.ucode_size, b.ucode_off, b.version))
        if b.type == VERTEX:
            ins = ",".join(VP_IN_NAME[i] for i in range(16) if b.attr_in_mask >> i & 1)
            outs = ",".join(VP_OUT_NAME[o] for o in sorted(VP_OUT_BIT)
                            if b.attr_out_mask >> VP_OUT_BIT[o] & 1)
            print("    in  mask 0x%04x  [%s]" % (b.attr_in_mask, ins))
            print("    out mask 0x%08x [HPOS,%s]" % (b.attr_out_mask, outs))
            print("    temp registers %d%s" % (b.reg_count,
                  "" if b.split_ok else "   ** attribute/uniform split FAILED **"))
        else:
            print("    in  mask 0x%08x  texcoord mask 0x%04x  shader_control 0x%04x"
                  " temps %d" % (b.attr_in_mask, b.texcoord_mask,
                                 b.shader_control, b.reg_count))
        for reg, val in b.literals:
            print("    literal c[%d] = (%.8g, %.8g, %.8g, %.8g)" % ((reg,) + val))
        if args.raw:
            print("    const table:", b.const_table)
        for p in b.params:
            if p.kind == "container":
                print("    %-9s %-34s -" % (p.kind, p.qualified))
            elif p.kind == "sampler":
                print("    %-9s %-34s texunit %d" % (p.kind, p.qualified, p.unit))
            elif p.kind == "uniform" and b.type == FRAGMENT:
                print("    %-9s %-34s c[%d]  patched at %s"
                      % (p.kind, p.qualified, p.register,
                         ",".join("0x%x" % o for o in p.patches) or "-"))
            else:
                tag = "v[%d]" % p.register if p.kind == "attribute" else "c[%d]" % p.register
                print("    %-9s %-34s %s" % (p.kind, p.qualified, tag))


def cmd_extract(args):
    if not os.path.isdir(args.outdir):
        os.makedirs(args.outdir)
    files = ([args.file] if os.path.isfile(args.file)
             else [os.path.join(args.file, f) for f in sorted(os.listdir(args.file))])
    n = 0
    for path in files:
        data = open(path, 'rb').read()
        stem = os.path.basename(path)
        for i, b in enumerate(find_blobs(data)):
            out = os.path.join(args.outdir, "%s.%02d%s" % (stem, i, b.suffix))
            open(out, 'wb').write(b.ucode_for_tools())
            n += 1
    print("wrote %d microcode file(s) to %s" % (n, args.outdir))


def cmd_match(args):
    shaders = {}
    for f in sorted(os.listdir(args.shaderdir)):
        p = os.path.join(args.shaderdir, f)
        if os.path.isfile(p):
            shaders[f] = open(p, 'rb').read()

    vp, fp = {}, []
    for name, data in shaders.items():
        for b in find_blobs(data):
            if b.type == VERTEX:
                vp.setdefault(b.ucode_for_tools(), set()).add(name)
            else:
                fp.append((name, b.raw_ucode(), b.const_patch_mask()))

    since = None
    if args.since:
        since = datetime.date.fromisoformat(args.since)

    caps, skipped = [], 0
    for f in sorted(os.listdir(args.ucodedir)):
        p = os.path.join(args.ucodedir, f)
        if not os.path.isfile(p) or not f.endswith(('.vp', '.fp')):
            continue
        if since and datetime.date.fromtimestamp(os.path.getmtime(p)) < since:
            skipped += 1
            continue
        caps.append((f, open(p, 'rb').read()))

    hit = 0
    for f, v in caps:
        if f.endswith('.vp'):
            names = sorted(vp.get(v, ()))
        else:
            names = sorted({nm for nm, uc, m in fp if len(uc) == len(v)
                            and all(a == c for a, c, mm in zip(uc, v, m) if not mm)})
        if names:
            hit += 1
        print("%s\t%s" % (f, ",".join(names) if names else "?"))
    print("# %d/%d captures resolved%s" % (hit, len(caps),
          ("  (%d older captures skipped by --since)" % skipped) if skipped else ""),
          file=sys.stderr)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    sub = ap.add_subparsers(dest='cmd')

    a = sub.add_parser('list', help='enumerate CGB blobs in a shader file')
    a.add_argument('file')
    a.set_defaults(func=cmd_list)

    a = sub.add_parser('params', help='print the parameter table')
    a.add_argument('file')
    a.add_argument('-b', '--blob', type=int, default=None)
    a.add_argument('--raw', action='store_true', help='also dump the const table')
    a.set_defaults(func=cmd_params)

    a = sub.add_parser('extract', help='slice microcode out to .vp/.fp files')
    a.add_argument('file', help='a shader file or a directory of them')
    a.add_argument('outdir')
    a.set_defaults(func=cmd_extract)

    a = sub.add_parser('match', help='map captured ucode hashes to shader names')
    a.add_argument('shaderdir')
    a.add_argument('ucodedir')
    a.add_argument('--since', help='ignore captures older than YYYY-MM-DD')
    a.set_defaults(func=cmd_match)

    args = ap.parse_args()
    if not getattr(args, 'func', None):
        ap.print_help()
        return 2
    args.func(args)
    return 0


if __name__ == '__main__':
    sys.exit(main())
