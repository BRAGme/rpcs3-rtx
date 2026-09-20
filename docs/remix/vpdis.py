r"""RSX (NV40) vertex-program disassembler for the .vp files in bin\remix_ucode\.

    python docs/remix/vpdis.py bin/remix_ucode/F2577D351159C828.vp

Bitfields taken verbatim from rpcs3/Emu/RSX/Program/RSXVertexProgram.h (unions
D0/D1/D2/D3/SRC, MSVC LSB-first u32 bitfields); dword order from
VertexProgramDecompiler.cpp:563-566 (d0,d1,d2,d3 = data[i*4+0..3]); operand slots and
destination selection from the same file at :599-622 and GetDST().

TWO TRAPS, both of which produce a plausible-looking but wrong listing:

  * ADD reads $0 and $2, NOT $0 and $1. Decode it with src1 and every transform chain
    reads as nonsense - 'HPOS <- c[467].y + v0' where the program actually says
    'HPOS <- c[467].y + r0'. The tell is a temp that is computed and then never used.
  * A vector op writes to an OUTPUT only when d0.vec_result is set and d3.dst != 0x1f,
    and to a temp when d0.dst_tmp != 0x3f. Both can happen on one instruction. Guessing
    from dst_tmp alone mislabels every dual-write.

The ucode store that feeds this is RPCS3_REMIX_UCODESTORE; the .fp siblings in the same
directory are fragment programs and are NOT decodable by this script.
"""
import struct
import sys

SCA = ["NOP", "MOV", "RCP", "RCC", "RSQ", "EXP", "LOG", "LIT", "BRA", "BRI",
       "CAL", "CLI", "RET", "LG2", "EX2", "SIN", "COS", "BRB", "CLB", "PSH", "POP"]

VEC = ["NOP", "MOV", "MUL", "ADD", "MAD", "DP3", "DPH", "DP4", "DST", "MIN",
       "MAX", "SLT", "SGE", "ARL", "FRC", "FLR", "SEQ", "SFL", "SGT", "SLE",
       "SNE", "STR", "SSG", "NULL", "NULL", "TXL"]

IN_NAME = ["pos", "weight", "normal", "diff_color", "spec_color", "fog",
           "point_size", "in7", "tc0", "tc1", "tc2", "tc3", "tc4", "tc5", "tc6", "tc7"]

# Vertex-program output registers. HPOS is 0 and the texcoord bank starts at 7,
# which is the same base RemixTransforms uses when it asks for output '7 + unit'.
OUT_NAME = {0: "HPOS", 1: "COL0", 2: "COL1", 3: "BFC0", 4: "BFC1", 5: "FOGC",
            6: "PSIZ", 7: "TEX0", 8: "TEX1", 9: "TEX2", 10: "TEX3", 11: "TEX4",
            12: "TEX5", 13: "TEX6", 14: "TEX7", 15: "UNK15"}

SWZ = "xyzw"


def bits(v, lo, n):
    return (v >> lo) & ((1 << n) - 1)


class Src:
    def __init__(self, raw):
        self.reg_type = bits(raw, 0, 2)
        self.tmp = bits(raw, 2, 6)
        self.swz = [bits(raw, 14, 2), bits(raw, 12, 2), bits(raw, 10, 2), bits(raw, 8, 2)]
        self.neg = bits(raw, 16, 1)

    def text(self, d1, mask_len=4):
        if self.reg_type == 1:
            base = "r%d" % self.tmp
        elif self.reg_type == 2:
            base = "v%d(%s)" % (d1["input_src"], IN_NAME[d1["input_src"]])
        elif self.reg_type == 3:
            base = "c[%d]" % d1["const_src"]
            if d1["index_const"]:
                base = "c[A0.%s + %d]" % (SWZ[d1["addr_swz"]], d1["const_src"])
        else:
            base = "?%d" % self.reg_type
        sw = ''.join(SWZ[c] for c in self.swz[:mask_len])
        return ("-" if self.neg else "") + base + "." + sw


def writemask(d3, scalar=False):
    if scalar:
        m = [bits(d3, 20, 1), bits(d3, 19, 1), bits(d3, 18, 1), bits(d3, 17, 1)]
    else:
        m = [bits(d3, 16, 1), bits(d3, 15, 1), bits(d3, 14, 1), bits(d3, 13, 1)]
    return ''.join(SWZ[i] for i in range(4) if m[i])


def disasm(path):
    raw = open(path, 'rb').read()
    words = struct.unpack('<%dI' % (len(raw) // 4), raw[:len(raw) // 4 * 4])
    n = len(words) // 4
    print("=== %s   (%d bytes, %d instructions)" % (path, len(raw), n))
    for i in range(n):
        d0, d1w, d2, d3 = words[i * 4:i * 4 + 4]

        d1 = {
            "input_src": bits(d1w, 8, 4),
            "const_src": bits(d1w, 12, 10),
            "index_const": bits(d3, 1, 1),
            "addr_swz": bits(d0, 0, 2),
        }
        vec_op = bits(d1w, 22, 5)
        sca_op = bits(d1w, 27, 5)

        src0 = Src(bits(d2, 23, 9) | (bits(d1w, 0, 8) << 9))
        src1 = Src(bits(d2, 6, 17))
        src2 = Src(bits(d3, 21, 11) | (bits(d2, 0, 6) << 11))

        dst = bits(d3, 2, 5)
        dst_tmp = bits(d0, 15, 6)
        vec_result = bits(d0, 30, 1)
        sca_dst_tmp = bits(d3, 7, 6)
        end = bits(d3, 0, 1)

        out = []

        if vec_op != 0:
            name = VEC[vec_op] if vec_op < len(VEC) else "VEC?%d" % vec_op
            wm = writemask(d3)
            # Destination selection exactly as VertexProgramDecompiler::GetDST does it:
            # an output write needs d0.vec_result AND d3.dst != 0x1f; a temp write needs
            # d0.dst_tmp != 0x3f; both can happen on the same vector instruction.
            targets = []
            if vec_result and dst != 0x1f:
                targets.append("o[%d](%s)" % (dst, OUT_NAME.get(dst, "?")))
            if dst_tmp != 0x3f:
                targets.append("r%d" % dst_tmp)
            if not targets:
                targets.append("CC")
            # Operand SLOTS, from VertexProgramDecompiler.cpp:599-622. ADD is the trap:
            # it reads $0 and $2, NOT $0 and $1.
            slots = {"MOV": (0,), "ARL": (0,), "FRC": (0,), "FLR": (0,), "SSG": (0,),
                     "ADD": (0, 2), "MAD": (0, 1, 2)}.get(name, (0, 1))
            pool = [src0, src1, src2]
            out.append("VEC %-3s %s.%-4s <- %s" % (
                name, "=".join(targets), wm,
                ", ".join(pool[k].text(d1) for k in slots)))

        if sca_op != 0:
            name = SCA[sca_op] if sca_op < len(SCA) else "SCA?%d" % sca_op
            wm = writemask(d3, scalar=True)
            targets = []
            if (not vec_result) and dst != 0x1f:
                targets.append("o[%d](%s)" % (dst, OUT_NAME.get(dst, "?")))
            if sca_dst_tmp != 0x3f:
                targets.append("r%d" % sca_dst_tmp)
            if not targets:
                targets.append("CC")
            out.append("SCA %-3s %s.%-4s <- %s" % (
                name, "=".join(targets), wm, src2.text(d1)))

        if not out:
            out.append("(nop)")

        for k, line in enumerate(out):
            print("  %2d: %s%s" % (i, line, "   END" if (end and k == len(out) - 1) else ""))


for p in sys.argv[1:]:
    disasm(p)
    print()
