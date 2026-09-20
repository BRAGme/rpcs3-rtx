r"""RSX (NV40) FRAGMENT-program disassembler for the .fp files in bin\remix_ucode\.

    python docs/remix/fpdis.py bin/remix_ucode/613195379C52B18B.fp

Sibling of vpdis.py, which handles the .vp vertex programs and cannot read these.

Layouts taken verbatim from rpcs3/Emu/RSX/Program/RSXFragmentProgram.h (unions
OPDEST/SRC0/SRC1/SRC2, MSVC LSB-first u32 bitfields), opcode numbering from
rpcs3/Emu/RSX/Program/Assembler/FPOpcodes.h, and the fragment input register names
from FragmentProgramDecompiler.cpp's reg_table.

THE TRAP, and it is the whole reason this file exists rather than a few lines of struct
unpacking: fragment ucode is NOT stored the way vertex ucode is. Each 32-bit word must be
byte-swapped WITHIN ITS TWO HALVES before any bitfield means anything --

    hex = ((w & 0x00FF00FF) << 8) | ((w & 0xFF00FF00) >> 8)

which is OPDEST::from_be32 in that header, applied to all four words of the instruction.
Skip it and every field reads as plausible garbage: real opcodes, real-looking registers,
and a completely wrong program. There is no error, only a wrong answer.

An instruction is 4 words (16 bytes). The opcode is split: 6 bits in OPDEST.opcode plus a
high bit in SRC1.opcode_hi, giving the 0x00..0x45 range the opcode table covers.

TRAP 2, fixed 2026-09-09: the THIRD source operand has the SAME bit layout as the first
two. `union SRC2` in RSXFragmentProgram.h is fp16:1 at bit 8, swizzle_x..w at 9/11/13/15,
neg at 17, abs at 18 -- byte for byte SRC0/SRC1. This file used to decode SRC2 from bit 8,
which printed every MAD/LRP/DP2A third operand with a phantom negation and a rotated
swizzle (`-R0.wxzx` where the program says `R0.xyzw`). That is enough to read a lerp
backwards, and it did: listings quoted from this tool before 2026-09-09 must be re-derived.
Only .fp listings are affected; vpdis.py was never wrong this way.

TRAP 3, fixed with it: the swizzle used to print high-lane-first, so the identity swizzle
appeared as `.wzyx`. Lanes now print x,y,z,w and the identity reads `.xyzw`.
"""
import struct
import sys

OPS = {
    0x00: "NOP", 0x01: "MOV", 0x02: "MUL", 0x03: "ADD", 0x04: "MAD", 0x05: "DP3",
    0x06: "DP4", 0x07: "DST", 0x08: "MIN", 0x09: "MAX", 0x0A: "SLT", 0x0B: "SGE",
    0x0C: "SLE", 0x0D: "SGT", 0x0E: "SNE", 0x0F: "SEQ", 0x10: "FRC", 0x11: "FLR",
    0x12: "KIL", 0x13: "PK4", 0x14: "UP4", 0x15: "DDX", 0x16: "DDY", 0x17: "TEX",
    0x18: "TXP", 0x19: "TXD", 0x1A: "RCP", 0x1B: "RSQ", 0x1C: "EX2", 0x1D: "LG2",
    0x1E: "LIT", 0x1F: "LRP", 0x20: "STR", 0x21: "SFL", 0x22: "COS", 0x23: "SIN",
    0x24: "PK2", 0x25: "UP2", 0x26: "POW", 0x27: "PKB", 0x28: "UPB", 0x29: "PK16",
    0x2A: "UP16", 0x2B: "BEM", 0x2C: "PKG", 0x2D: "UPG", 0x2E: "DP2A", 0x2F: "TXL",
    0x31: "TXB", 0x33: "TEXBEM", 0x34: "TXPBEM", 0x35: "BEMLUM", 0x36: "REFL",
    0x37: "TIMESWTEX", 0x38: "DP2", 0x39: "NRM", 0x3A: "DIV", 0x3B: "DIVSQ",
    0x3C: "LIF", 0x3D: "FENCT", 0x3E: "FENCB", 0x40: "BRK", 0x41: "CAL", 0x42: "IFE",
    0x43: "LOOP", 0x44: "REP", 0x45: "RET",
}

IN_NAME = ["wpos", "diff_color", "spec_color", "fogc", "tc0", "tc1", "tc2", "tc3",
           "tc4", "tc5", "tc6", "tc7", "tc8", "tc9", "ssa"]

SWZ = "xyzw"
PREC = {0: "", 1: "H", 2: "X", 3: "F9", 4: "SAT", 5: "?"}

# How many source operands each opcode reads. Anything not listed reads two.
NSRC = {"NOP": 0, "MOV": 1, "RCP": 1, "RSQ": 1, "EX2": 1, "LG2": 1, "FRC": 1,
        "FLR": 1, "LIT": 1, "COS": 1, "SIN": 1, "NRM": 1, "KIL": 0, "TEX": 1,
        "TXB": 1, "TXL": 1, "DDX": 1, "DDY": 1, "PK2": 1, "PK4": 1, "PK16": 1,
        "PKB": 1, "PKG": 1, "UP2": 1, "UP4": 1, "UP16": 1, "UPB": 1, "UPG": 1,
        "MAD": 3, "LRP": 3, "DP2A": 3, "TXD": 3, "RET": 0}


def bits(v, lo, n):
    return (v >> lo) & ((1 << n) - 1)


def unswap(w):
    """OPDEST::from_be32 - swap the bytes inside each 16-bit half."""
    return ((w & 0x00FF00FF) << 8) | ((w & 0xFF00FF00) >> 8)


def src_text(raw, which, dest):
    reg_type = bits(raw, 0, 2)
    idx = bits(raw, 2, 6)
    # All THREE source words share one layout (SRC0/SRC1/SRC2 in the header):
    # fp16 at bit 8, swizzle_x..w at 9/11/13/15, neg at 17, abs at 18. See the
    # docstring's trap list -- this used to read SRC2 one bit low.
    fp16 = bits(raw, 8, 1)
    swz = [bits(raw, 9, 2), bits(raw, 11, 2), bits(raw, 13, 2), bits(raw, 15, 2)]
    neg = bits(raw, 17, 1)

    if reg_type == 0:
        base = ("H" if fp16 else "R") + str(idx)
    elif reg_type == 1:
        n = dest["src_attr_reg_num"]
        base = "f[%d](%s)" % (n, IN_NAME[n] if n < len(IN_NAME) else "?")
    elif reg_type == 2:
        base = "c[]"                     # an inline constant follows the instruction
    else:
        base = "?%d" % reg_type
    return ("-" if neg else "") + base + "." + ''.join(SWZ[c] for c in swz)


def src_fields(raw, which):
    """The decoded source operand, as fields.

    reg_type/reg/fp16/swizzle/neg are identical in all three source words. 'abs' is NOT:
    SRC0 carries the condition-code fields between neg and abs, so its abs sits at bit 29,
    while SRC1 and SRC2 have it at 18. Reading bit 18 for SRC0 lands on exec_if_lt, which is
    set on ordinary unpredicated instructions - every MAD src0 then looks like |R0|.
    """
    return {
        "reg_type": bits(raw, 0, 2),   # 0 TEMP, 1 INPUT, 2 CONSTANT
        "reg": bits(raw, 2, 6),
        "fp16": bits(raw, 8, 1),
        "swz": [bits(raw, 9, 2), bits(raw, 11, 2), bits(raw, 13, 2), bits(raw, 15, 2)],
        "neg": bits(raw, 17, 1),
        "abs": bits(raw, 29, 1) if which == 0 else bits(raw, 18, 1),
    }


def decode(path):
    """Decode a .fp into instruction records in program order.

    One dict per instruction: slot, name, opcode, dest ('R0'/'H3'/None for no_dest),
    mask (4-bit int, x = bit 0), tex_num, attr (OPDEST::src_attr_reg_num), srcs (list of
    src_fields dicts, only the slots this opcode reads), const (list of four floats or
    None), end. Shared with fpelect.py so the two tools can never disagree about a
    program - the whole reason this is a function rather than a copy.
    """
    raw = open(path, 'rb').read()
    words = struct.unpack('<%dI' % (len(raw) // 4), raw[:len(raw) // 4 * 4])

    out = []
    i = 0
    n = len(words) // 4

    while i < n:
        d, s0, s1, s2 = [unswap(x) for x in words[i * 4:i * 4 + 4]]

        dest = {
            "end": bits(d, 0, 1),
            "dest_reg": bits(d, 1, 6),
            "fp16": bits(d, 7, 1),
            "mask": [bits(d, 9, 1), bits(d, 10, 1), bits(d, 11, 1), bits(d, 12, 1)],
            "src_attr_reg_num": bits(d, 13, 4),
            "tex_num": bits(d, 17, 4),
            "prec": bits(d, 22, 2),
            "no_dest": bits(d, 30, 1),
            "saturate": bits(d, 31, 1),
        }
        opcode = bits(d, 24, 6) | (bits(s1, 31, 1) << 6)
        name = OPS.get(opcode, "OP?%02x" % opcode)
        nsrc = NSRC.get(name, 2)

        # A constant operand is an extra 16 bytes inlined after the instruction.
        inline = any(bits(x, 0, 2) == 2 for x in (s0, s1, s2)[:max(nsrc, 1)])
        const = None
        if inline and (i + 1) < n:
            c = [unswap(x) for x in words[(i + 1) * 4:(i + 1) * 4 + 4]]
            const = [struct.unpack('>f', struct.pack('>I', x))[0] for x in c]

        out.append({
            "slot": i,
            "name": name,
            "opcode": opcode,
            "dest": None if dest["no_dest"] else
                    (("H" if dest["fp16"] else "R") + str(dest["dest_reg"])),
            "mask": sum(b << k for k, b in enumerate(dest["mask"])),
            "tex_num": dest["tex_num"],
            "attr": dest["src_attr_reg_num"],
            "srcs": [src_fields(x, k) for k, x in enumerate((s0, s1, s2)[:nsrc])],
            "const": const,
            "end": bool(dest["end"]),
            "_dest": dest,
            "_raw": (s0, s1, s2),
        })

        if dest["end"]:
            break
        i += 2 if inline else 1

    return out, len(raw), n


def disasm(path):
    code, nbytes, slots = decode(path)
    print("=== %s   (%d bytes, %d instruction slots)" % (path, nbytes, slots))

    for in_ in code:
        dest = in_["_dest"]
        name = in_["name"]

        mask = ''.join(SWZ[k] for k in range(4) if dest["mask"][k])
        target = "-" if dest["no_dest"] else \
            ("H" if dest["fp16"] else "R") + str(dest["dest_reg"])

        nsrc = NSRC.get(name, 2)
        srcs = [src_text(raw_src, k, dest)
                for k, raw_src in enumerate(in_["_raw"][:nsrc])]

        extra = ""
        if in_["const"] is not None:
            extra = "   c=[%.6g %.6g %.6g %.6g]" % tuple(in_["const"])

        flags = []
        if dest["saturate"]:
            flags.append("sat")
        if PREC.get(dest["prec"]):
            flags.append(PREC[dest["prec"]])
        if name in ("TEX", "TXP", "TXB", "TXL", "TXD"):
            flags.append("tex%d" % dest["tex_num"])

        print("  %2d: %-7s %s.%-4s <- %s%s%s%s" % (
            in_["slot"], name, target, mask, ", ".join(srcs) if srcs else "",
            ("  [" + ",".join(flags) + "]") if flags else "",
            extra, "   END" if dest["end"] else ""))


if __name__ == "__main__":
    for p in sys.argv[1:]:
        disasm(p)
        print()
