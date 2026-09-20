r"""Static scan for the PER-CHANNEL CONSTANT TINT of a sampled texel in the .fp files in bin\remix_ucode\.

    python docs/remix/fptint.py bin/remix_ucode

Pass the DIRECTORY: on this machine `find | xargs` splits the 793-file corpus into two
invocations (679 + 114) and each batch prints its own totals and runs its own self-test, so a
count read off an xargs run is a partial count. Never scope this corpus by file mtime either - a
program already dumped keeps its OLD timestamp, so "today's files" omits programs in active use.

Mirrors scan_fragment_program()'s round-67 out_rgb_tint detector (RemixTransforms.cpp) so the
count and the tints can be read off disk before the emulator is built, and so a run's own
'Remix fpconst:' route=tint rows can be cross-checked against an independent implementation of
the same rule. Decoding is imported from fpdis.py and the round-63 lerp reading from fplerp.py,
so the three tools can never disagree about a program.

THE SHAPE. The texel is multiplied by a per-channel literal straight out of the sampler:

    9F30538CBDE6C249   0: TEX R0.xyzw <- f[5](tc1).yzzw  [tex0]
                       1: MUL R0.xyz  <- R0.xyzw, c[].xyzw   c=[0.5 0.8 1.5 0]     <- the tint

and the product goes on to the export - here through 'MUL R0.xyz <- R0, R1' and the level-wide
fog lerp at slot 11 that round 63 correctly refuses at k = 0.204. The literal is read THROUGH
the operand's swizzle lane by lane, exactly as the round-62 and round-63 readings are.

THE DANGER, and the reason the counts are staged: 'texture times a constant' is ordinary shading.
The same instruction shape carries a specular colour on a gloss map, a detail-map scale, a
brightness scale and a normal-map decode, and a rule that only asks for the shape repaints the
level. Each stage below is one tightening; the listing says what each removes; the C++ applies
only the last one.

  S0 shape       unconditional 'MUL Rd.xyz <- Rs, c[]': Rs a plain temp read through the identity
                 swizzle on xyz, whose last writer of .xyz is a texture sample that wrote all of
                 .xyz; no neg/abs on either operand; the literal finite and >= 0 on all three lanes.
  S1 reaches     all three lanes of the product are live into COL0.rgb by a lane-precise backward
                 liveness walk WITH kills. The scanner's colour_mask walk has no kill because
                 over-approximating is the safe direction for electing a unit; for a tint it is the
                 wrong direction - a tint that is later overwritten must not be applied - and
                 straight-line code (flow control refuses the whole program) makes kills exact.
  S2 non-neutral the lanes are not all (nearly) equal: a broadcast literal is a brightness, not a
                 hue, and a spread under NEUTRAL_SPREAD is a no-op on an 8-bit factor anyway.
  S3 albedo unit the tinted sample is the LOWEST unit whose sample reaches COL0.rgb - the
                 backend's rule=lowest election. A specular colour on a gloss map reaches the
                 output through an ADD and passes S0..S2; it is not the albedo's tint. (The C++
                 compares against the unit actually elected at the draw, which is exact.)
  S4 one tint    exactly one distinct (unit, literal) among the reaching non-neutral matches. Two
                 tints on one texel is two colours and no rule can choose.
  S5 precedence  the round-62 literal and the round-63 lerp are the exact readings and are tested
                 first at the apply site; a program they claim is not re-tinted here.
  S6 COL0 only   the program writes no MRT output register - R2/R3/R4 with fp32 exports, H4/H6/H8
                 with fp16 (rpcs3's s_fp32_output_set / s_fp16_output_set) - so COL0 is its only
                 colour output: a FORWARD-PASS effect program, not a deferred G-buffer material.
                 MEASURED 2026-09-11: S5 passes 45 of 793 and 43 of the 45 write all of R2, R3
                 and R4 - the G-buffer pass (target=31 on their draws) in which 'R0 = tex0 x c[]'
                 is the MATERIAL'S DIFFUSE COLOUR of a lit surface. They draw the level: concrete
                 walls 2C62891E53AC7C89 (x228 in one run's dump), FDF6489A939E6563 (x219),
                 E6AE2AA616434545 (x355), marble E1F20A0B844B0E63, a character's skin
                 FFBA203FD04E78D5. Applying them repaints the level - the blast radius this round
                 refuses; if that material colour is ever wanted it is its own route over all 43,
                 not a side effect of this one. The 2 that pass are the two constant-variants of
                 ONE effect program (fp 01072a35c2acbe5c / 01072a35c28cbe20; both write only R0
                 and the scratch R1; every draw target=1 blend=1): the code blood - a black sheet
                 of white specks 921904970ADA8A23, a splat C3F743DC7B3CB9F1, water droplets
                 02F4F31E9A9729A9 - and a dust puff 67E81BA9F74C7906. This is the stage the C++
                 applies.
                 REJECTED discriminator, for the record: "the tinted unit is the program's only
                 sampled unit" leaves 5, but one of them (A50DE5B1F5682CE2) is a G-buffer program
                 drawing the concrete wall E6AE2AA616434545 x355 whose near-twin 1B98CE5D069789E2
                 (same tint, same tc1 sample, plus a tex1 luminance into R4) it refuses - it would
                 tint one wall of a family blue-grey and leave the other raw.

SELF-TEST: runs unconditionally before anything is printed. 9F30538CBDE6C249 must resolve to the
RAW tint (0.5, 0.8, 1.5) - before the apply site's >1.0 handling - on unit 0 and survive every
stage through S6. A differing fixture aborts with the diff; it does not warn and continue.
"""
import collections
import glob
import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fpdis   # noqa: E402
import fplerp  # noqa: E402

TEMP, INPUT, CONSTANT = 0, 1, 2
FLOW_OPS = {"BRK", "CAL", "IFE", "LOOP", "REP", "RET"}

# fp_op_is_sample, RemixTransforms.cpp.
SAMPLE_OPS = {"TEX", "TXP", "TXD", "TXL", "TXB", "TEXBEM", "TXPBEM"}

# fp_colour_sources, RemixTransforms.cpp: how many source slots a COLOUR-PRESERVING opcode reads;
# everything else (DP3, NRM, RCP, ...) turns a colour into a scalar and is a barrier.
COLOUR_NSRC = {"MOV": 1, "FRC": 1, "FLR": 1, "PK4": 1, "UP4": 1, "PK2": 1, "UP2": 1, "PKB": 1,
               "UPB": 1, "PK16": 1, "UP16": 1, "PKG": 1, "UPG": 1,
               "MUL": 2, "ADD": 2, "MIN": 2, "MAX": 2, "SLT": 2, "SGE": 2, "SLE": 2, "SGT": 2,
               "SNE": 2, "SEQ": 2, "DIV": 2, "MAD": 3, "LRP": 3}

# Of those, the ones whose output lane L reads source lane swz[L] - lane-precise liveness. The
# pack/unpack opcodes mix lanes and are walked conservatively (all four lanes).
LANEWISE = {"MOV", "MUL", "ADD", "MAD", "MIN", "MAX", "LRP", "FRC", "FLR", "SLT", "SGE", "SLE",
            "SGT", "SNE", "SEQ", "DIV"}

NEUTRAL_SPREAD = 0.02

# The fpraw name on disk is fp_hash ^ this when the program exports fp32 (every world program on
# this title does); printed so a passing program can be looked up in remix_dump.log by fp=.
FP32_MARK = 0x9e3779b97f4a7c15

# name -> (raw tint, unit)
FIXTURES = {
    "9F30538CBDE6C249": ((0.5, 0.8, 1.5), 0),
}


def finite(v):
    return not (math.isnan(v) or math.isinf(v))


def live_into_col0_after(code, index, col0):
    """{register: lane mask} of everything live into COL0.rgb immediately AFTER code[index] -
    i.e. the backward walk over code[index+1:], seeded with col0.xyz. Kills are exact because the
    caller has already refused flow control."""
    live = {col0: 7}

    for i in range(len(code) - 1, index, -1):
        in_ = code[i]
        dest = in_["dest"]

        if dest is None:
            continue

        hit = live.get(dest, 0) & in_["mask"]

        if not hit:
            continue

        if fplerp.unconditional(in_):
            live[dest] = live.get(dest, 0) & ~in_["mask"]

        if in_["name"] in SAMPLE_OPS:
            continue   # a sample's source is a coordinate, not a colour

        nsrc = COLOUR_NSRC.get(in_["name"], 0)
        lanewise = in_["name"] in LANEWISE

        for s in range(min(nsrc, len(in_["srcs"]))):
            src = in_["srcs"][s]

            if src["reg_type"] != TEMP:
                continue

            reg = fplerp.temp_name(src)
            add = 0

            if lanewise:
                for lane in range(4):
                    if hit & (1 << lane):
                        add |= 1 << src["swz"][lane]
            else:
                add = 0xf

            live[reg] = live.get(reg, 0) | add

    return live


def terminal_literal(code, col0):
    """Round 62: the last writer of COL0.rgb is an unconditional 'MOV col0.xyz <- c[]'."""
    idx = fplerp.last_writer(code, col0, 7, len(code))
    if idx < 0:
        return False
    in_ = code[idx]
    if in_["name"] != "MOV" or (in_["mask"] & 7) != 7 or not fplerp.unconditional(in_):
        return False
    src = in_["srcs"][0]
    return src["reg_type"] == CONSTANT and not src["neg"] and not src["abs"] \
        and in_["const"] is not None


def tint_matches(code, col0):
    """Every S0 match in program order, each annotated with its S1 verdict."""
    out = []

    for i, in_ in enumerate(code):
        if in_["name"] != "MUL" or in_["dest"] is None or (in_["mask"] & 7) != 7:
            continue
        if not fplerp.unconditional(in_) or in_["const"] is None:
            continue

        tslot = cslot = None

        for s in range(2):
            src = in_["srcs"][s]
            if src["reg_type"] == TEMP and not src["neg"] and not src["abs"] \
                    and src["swz"][:3] == [0, 1, 2]:
                tslot = s
            elif src["reg_type"] == CONSTANT and not src["neg"] and not src["abs"]:
                cslot = s

        if tslot is None or cslot is None:
            continue

        rs = fplerp.temp_name(in_["srcs"][tslot])
        w = fplerp.last_writer(code, rs, 7, i)

        if w < 0:
            continue

        tex = code[w]

        if tex["name"] not in SAMPLE_OPS or (tex["mask"] & 7) != 7:
            continue

        rgb = [in_["const"][in_["srcs"][cslot]["swz"][lane]] for lane in range(3)]

        if not all(finite(v) and v >= 0.0 for v in rgb):
            continue

        live = live_into_col0_after(code, i, col0)
        reaches = (live.get(in_["dest"], 0) & 7) == 7

        out.append({
            "mul": i, "tex": w, "unit": tex["tex_num"], "rgb": rgb, "mask": in_["mask"],
            "reaches": reaches, "spread": max(rgb) - min(rgb),
        })

    return out


class Program:
    def __init__(self, path):
        self.path = path
        self.name = os.path.splitext(os.path.basename(path))[0].upper()
        self.code, _, _ = fpdis.decode(path)
        self.has_flow = any(i["name"] in FLOW_OPS for i in self.code)
        self.truncated = not any(i["end"] for i in self.code)

        writes_r0 = any(i["dest"] == "R0" and i["mask"] for i in self.code)
        writes_h0 = any(i["dest"] == "H0" and i["mask"] for i in self.code)
        self.col0 = "R0" if writes_r0 or not writes_h0 else "H0"

        self.sampled = sorted({i["tex_num"] for i in self.code if i["name"] in SAMPLE_OPS})
        self.matches = []
        self.colour_units = set()
        self.lowest_unit = None
        self.literal = False
        self.lerp_applied = False
        self.refusal = None

        if self.has_flow:
            self.refusal = "flow"
            return
        if self.truncated:
            self.refusal = "truncated"
            return

        self.matches = tint_matches(self.code, self.col0)

        for j, in_ in enumerate(self.code):
            if in_["name"] in SAMPLE_OPS and in_["dest"] is not None:
                live = live_into_col0_after(self.code, j, self.col0)
                if live.get(in_["dest"], 0) & in_["mask"] & 7:
                    self.colour_units.add(in_["tex_num"])

        self.lowest_unit = min(self.colour_units) if self.colour_units else None
        self.literal = terminal_literal(self.code, self.col0)
        self.lerp_applied = fplerp.Program(path).applied

    # --- the stages, each a strict subset of the previous -------------------------------------
    def s0(self):
        return self.matches

    def s1(self):
        return [m for m in self.s0() if m["reaches"]]

    def s2(self):
        return [m for m in self.s1() if m["spread"] >= NEUTRAL_SPREAD]

    def s3(self):
        return [m for m in self.s2() if m["unit"] == self.lowest_unit]

    def s4(self):
        """The single tint, or None when there are none or several. Distinctness is over EVERY
        reaching non-neutral match, unit included - the same conflict rule the C++ applies, which
        defers the unit test to the draw (selected_albedo_unit) rather than electing offline."""
        m = self.s3()
        if not m:
            return None
        distinct = {(x["unit"],) + tuple(round(v, 6) for v in x["rgb"]) for x in self.s2()}
        return m[0] if len(distinct) == 1 else None

    def s5(self):
        m = self.s4()
        if m is None or self.literal or self.lerp_applied:
            return None
        return m

    def mrt_written(self):
        """The MRT output registers this program writes, by rpcs3's output set for its precision."""
        regs = {"R2", "R3", "R4"} if self.col0 == "R0" else {"H4", "H6", "H8"}
        return sorted(regs & {i["dest"] for i in self.code if i["dest"] is not None and i["mask"]})

    def s6(self):
        m = self.s5()
        if m is None or self.mrt_written():
            return None
        return m

    def s5_refusal(self):
        if self.refusal:
            return self.refusal
        if not self.s0():
            return "no-shape"
        if not self.s1():
            return "not-reaching"
        if not self.s2():
            return "neutral"
        if not self.s3():
            return "not-albedo-unit"
        if self.s4() is None:
            return "several-tints"
        if self.literal:
            return "round-62-literal"
        if self.lerp_applied:
            return "round-63-lerp"
        if self.s6() is None:
            return "writes-mrt"
        return None

    def fp32_hash(self):
        return int(self.name, 16) ^ FP32_MARK

    def terminal_text(self):
        idx = fplerp.last_writer(self.code, self.col0, 7, len(self.code))
        return fplerp.instr_text(self.code[idx]).strip() if idx >= 0 else "(no COL0.rgb writer)"


def packed(rgb):
    """What the apply site sends: each lane CLAMPED to [0, 1], then 8-bit. See the decision in
    RemixGSRender.cpp's round-67 block; normalise-by-max is the alternative printed beside it."""
    return tuple(int(round(min(1.0, max(0.0, v)) * 255)) for v in rgb)


def normalised(rgb):
    m = max(rgb)
    return tuple(v / m for v in rgb) if m > 1.0 else tuple(rgb)


def self_test(programs):
    by_name = {p.name: p for p in programs}
    bad = []

    for name, (want, want_unit) in FIXTURES.items():
        p = by_name.get(name)
        if p is None:
            bad.append("%s: not in the corpus given" % name)
            continue
        m = p.s6()
        if m is None:
            bad.append("%s: refused at %s, wanted %s" % (name, p.s5_refusal(), want))
            continue
        if any(abs(g - w) > 1e-5 for g, w in zip(m["rgb"], want)):
            bad.append("%s: got (%.6g, %.6g, %.6g), wanted %s" % ((name,) + tuple(m["rgb"]) + (want,)))
        if m["unit"] != want_unit:
            bad.append("%s: unit %d, wanted %d" % (name, m["unit"], want_unit))

    if bad:
        print("SELF-TEST FAILED - this scan's output must not be used:")
        for line in bad:
            print("  " + line)
        sys.exit(2)

    for name in FIXTURES:
        m = by_name[name].s6()
        print("self-test: %s -> raw tint (%.6g, %.6g, %.6g) unit=%d  clamp=%s  normalise=(%.3f, %.3f, %.3f)  OK"
              % ((name,) + tuple(m["rgb"]) + (m["unit"], packed(m["rgb"])) + normalised(m["rgb"])))


def main(argv):
    paths = []
    for arg in argv:
        if os.path.isdir(arg):
            paths.extend(sorted(glob.glob(os.path.join(arg, "*.fp"))))
        else:
            paths.extend(glob.glob(arg) or [arg])
    if not paths:
        print(__doc__)
        return 1

    programs = [Program(p) for p in paths]
    self_test(programs)

    n = len(programs)
    stages = [
        ("S0 shape: MUL rgb <- <TEX temp>, c[] (unconditional, plain, literal >= 0)", lambda p: bool(p.s0())),
        ("S1 + all three product lanes reach COL0.rgb (kill-aware walk)", lambda p: bool(p.s1())),
        ("S2 + non-neutral (lane spread >= %g)" % NEUTRAL_SPREAD, lambda p: bool(p.s2())),
        ("S3 + tinted unit is the lowest COL0.rgb-reaching unit (albedo)", lambda p: bool(p.s3())),
        ("S4 + exactly one distinct tint on that unit", lambda p: p.s4() is not None),
        ("S5 + not already claimed by round 62 (literal) or 63 (lerp)", lambda p: p.s5() is not None),
        ("S6 + COL0 is the only colour output (no MRT register written)", lambda p: p.s6() is not None),
    ]

    print()
    print("programs=%d  flow=%d  truncated=%d" % (
        n, sum(p.has_flow for p in programs), sum(p.truncated for p in programs)))
    for label, pred in stages:
        print("  %-72s : %d" % (label, sum(1 for p in programs if pred(p))))

    print()
    print("S2 tints by literal (what the shape alone would have painted, before the unit gate):")
    fam = collections.Counter()
    for p in programs:
        for m in p.s2():
            fam[tuple(round(v, 4) for v in m["rgb"])] += 1
    for rgb, count in fam.most_common():
        print("  %4d  (%g, %g, %g)" % ((count,) + rgb))

    print()
    print("S6 - what the C++ applies (route=tint):")
    for p in sorted(programs, key=lambda p: p.name):
        m = p.s6()
        if m is None:
            continue
        print("  %s  fp32-hash=%016x  unit=%d  sampled=%s  instrs=%d  raw=(%.6g, %.6g, %.6g)  clamp=%s  normalise=(%.3f, %.3f, %.3f)" % (
            (p.name, p.fp32_hash(), m["unit"], ",".join(str(u) for u in p.sampled), len(p.code))
            + tuple(m["rgb"]) + (packed(m["rgb"]),) + normalised(m["rgb"])))
        print("      %s" % fplerp.instr_text(p.code[m["tex"]]).strip())
        print("      %s" % fplerp.instr_text(p.code[m["mul"]]).strip())
        print("      terminal: %s" % p.terminal_text())

    print()
    print("S2 programs REFUSED by a later stage (the danger, by name):")
    for p in sorted(programs, key=lambda p: p.name):
        if not p.s2() or p.s6() is not None:
            continue
        m = p.s2()[0]
        print("  %s  refused=%s  unit=%d lowest=%s  mrt=%s  tint=(%.4g, %.4g, %.4g)  sampled=%s  terminal: %s" % (
            (p.name, p.s5_refusal(), m["unit"], p.lowest_unit, ",".join(p.mrt_written()) or "-") + tuple(m["rgb"])
            + (",".join(str(u) for u in p.sampled), p.terminal_text())))

    print()
    reasons = collections.Counter(p.s5_refusal() for p in programs if p.s6() is None)
    print("refusals: " + "  ".join("%s=%d" % kv for kv in reasons.most_common()))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
