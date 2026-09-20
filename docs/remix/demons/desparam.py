"""Demon's Souls (PS3) PARAM / PARAMDEF reader.

Both formats are big-endian on PS3. Layouts read straight off the files:

PARAMDEF (paramdef/*.paramdef)
    0x00 u32  fileSize
    0x04 u16  headerSize (0x30)
    0x06 u16  dataVersion
    0x08 u16  fieldCount
    0x0A u16  fieldSize (0xAC = 172)
    0x0C c32  paramName, space padded
    0x2C u8   bigEndian (0xFF)
    ...
    0x30      fieldCount x 172-byte field records:
        +0x00 c64 displayName (Shift-JIS)
        +0x40 c8  displayType
        +0x48 c8  displayFormat
        +0x50 f32 default
        +0x54 f32 min
        +0x58 f32 max
        +0x5C f32 increment
        +0x60 s32 editFlags
        +0x64 s32 byteCount
        +0x68 s32 descriptionOffset
        +0x6C c32 internalType
        +0x8C c32 internalName

PARAM (param/drawparam/*.param)
    0x00 u32  stringsOffset
    0x04 u16  dataOffset
    0x06 u16  unk06
    0x08 u16  paramdefDataVersion
    0x0A u16  rowCount
    0x0C c32  paramType, space padded
    0x2C u8   bigEndian (0xFF)
    0x30      rowCount x (u32 id, u32 dataOffset, u32 nameOffset)

Bit fields: a field whose internalType carries ':N' packs N bits; consecutive
bitfields of the same storage type share one storage unit.
"""
import struct
import sys
import os

BE = '>'


def cstr(b):
    b = b.split(b'\0')[0]
    return b.decode('shift_jis', errors='replace').rstrip()


class Field:
    __slots__ = ('name', 'itype', 'dtype', 'size', 'bits', 'default', 'lo', 'hi', 'display')

    def __repr__(self):
        return '%s %s[%d]' % (self.itype, self.name, self.size)


def read_paramdef(path):
    d = open(path, 'rb').read()
    file_size, header_size, data_version, field_count, field_size = struct.unpack_from(BE + 'IHHHH', d, 0)
    name = cstr(d[0x0C:0x2C])
    fields = []
    for i in range(field_count):
        o = header_size + i * field_size
        f = Field()
        f.display = cstr(d[o:o + 0x40])
        f.dtype = cstr(d[o + 0x40:o + 0x48])
        f.default, f.lo, f.hi = struct.unpack_from(BE + 'fff', d, o + 0x50)
        f.size = struct.unpack_from(BE + 'i', d, o + 0x64)[0]
        f.itype = cstr(d[o + 0x6C:o + 0x8C])
        raw_name = cstr(d[o + 0x8C:o + 0xAC])
        f.bits = 0
        if ':' in raw_name:
            raw_name, _, bits = raw_name.partition(':')
            f.bits = int(bits.strip() or 0)
        if '[' in raw_name:
            raw_name = raw_name.split('[')[0]
        f.name = raw_name.strip()
        fields.append(f)
    return name, data_version, field_count, fields


FMT = {'s8': 'b', 'u8': 'B', 's16': 'h', 'u16': 'H', 's32': 'i', 'u32': 'I', 'f32': 'f'}


def decode_row(fields, blob):
    """Return an ordered list of (name, value). Bit fields are unpacked in order."""
    out = []
    off = 0
    bit_carry = None  # (storage_type, storage_offset, bits_used, value)
    for f in fields:
        base = f.itype.split(':')[0].strip()
        if f.bits:
            if bit_carry is None or bit_carry[0] != base or bit_carry[2] + f.bits > f.size * 8:
                code = FMT.get(base)
                if code is None:
                    off += f.size
                    bit_carry = None
                    continue
                value = struct.unpack_from(BE + code, blob, off)[0]
                bit_carry = [base, off, 0, value]
                off += f.size
            shift = bit_carry[2]
            mask = (1 << f.bits) - 1
            out.append((f.name, (bit_carry[3] >> shift) & mask))
            bit_carry[2] += f.bits
            continue
        bit_carry = None
        if base == 'dummy8':
            off += f.size
            continue
        code = FMT.get(base)
        if code is None:
            off += f.size
            continue
        width = struct.calcsize(code)
        count = max(1, f.size // width)
        vals = struct.unpack_from(BE + code * count, blob, off)
        out.append((f.name, vals[0] if count == 1 else list(vals)))
        off += f.size
    return out, off


def read_param(path, fields):
    d = open(path, 'rb').read()
    strings_offset, data_offset, unk06, defver, row_count = struct.unpack_from(BE + 'IHHHH', d, 0)
    ptype = cstr(d[0x0C:0x2C])
    rows = []
    for i in range(row_count):
        rid, doff, noff = struct.unpack_from(BE + 'III', d, 0x30 + i * 12)
        rname = cstr(d[noff:noff + 64]) if noff else ''
        rows.append((rid, doff, rname))
    stride = (rows[1][1] - rows[0][1]) if len(rows) > 1 else 0
    decoded = []
    for rid, doff, rname in rows:
        blob = d[doff:doff + (stride if stride else 512)]
        vals, used = decode_row(fields, blob)
        decoded.append((rid, rname, dict(vals)))
    return ptype, defver, stride, decoded


if __name__ == '__main__':
    defpath, parampath = sys.argv[1], sys.argv[2]
    name, dv, fc, fields = read_paramdef(defpath)
    print('PARAMDEF %s dataVersion=%d fields=%d expected_row_bytes=%d'
          % (name, dv, fc, sum(f.size for f in fields)))
    if parampath == '-fields':
        for f in fields:
            print('   %-24s %-10s size=%-3d bits=%-2d  %s' % (f.name, f.itype, f.size, f.bits, f.display))
        sys.exit(0)
    ptype, defver, stride, rows = read_param(parampath, fields)
    print('PARAM %s defver=%d rows=%d stride=%d  (%s)'
          % (ptype, defver, len(rows), stride, os.path.basename(parampath)))
    for rid, rname, vals in rows:
        print('  row %-5d %-28s %s' % (rid, rname, vals))
