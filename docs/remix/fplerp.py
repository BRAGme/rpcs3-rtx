r"""Static scan for the HAND-ROLLED LERP endpoint colour in the .fp files in bin\remix_ucode\.

    python docs/remix/fplerp.py bin/remix_ucode
    find bin/remix_ucode -name "*.fp" | xargs python docs/remix/fplerp.py

A directory argument expands to every *.fp inside it, so the self-test fixtures are always in
the same invocation; never scope this corpus by file mtime - a program already dumped keeps its
OLD timestamp and "files newer than today" silently omits what the session is actually using.

Mirrors scan_fragment_program()'s round-63 out_rgb_lerp detector (RemixTransforms.cpp) so the
count and the endpoints can be read off disk before the emulator is built, and so a run's own
'Remix fpconst:' route=lerp rows can be cross-checked against an independent implementation of
the same rule. Decoding is imported from fpdis.py so the two can never disagree about a program.

THE IDIOM. No fragment program on this title uses LRP; a fade toward a colour is hand-rolled as

    ADD Rn.<lanes> <- -Rx.<swz>, c[].<swz>      c = [C0 C1 C2 C3]
    MAD col0.xyz   <- t, Rn.<swz>, Rx.<swz>

which is Rx + t*(C - Rx) = lerp(Rx, C, t) per lane; C is the colour the surface fades TOWARD and
t is a texcoord. The two measured shapes route their lanes differently, and a matcher written for
the first (Rn.xyz / identity swizzles) is blind to the second:

    CE3BE6B2685F446C   9: ADD R1.xyz <- -R0.xyzw, c[].xyzw   c=[0.694118 0.807843 1 1]
                      12: MAD R0.xyz <- R1.wwww, R1.xyzw, R0.xyzw       -> (0.694, 0.808, 1.0)
    9F30538CBDE6C249   7: ADD R1.yzw <- -R0.xxyz, c[].xxyz   c=[1 1 0.858824 0.203922]
                      11: MAD R0.xyz <- R1.xxxx, R1.yzww, R0.xyzw       -> (1.0, 1.0, 0.859)

so the endpoint is read by COMPOSING the swizzles per output lane: lane L of the MAD reads lane
m = swz_n[L] of Rn; that lane of the ADD reads Rx component swz_x[m] and C component swz_c[m];
the MAD adds back Rx component swz_x2[L]. It is a lerp only when swz_x[m] == swz_x2[L] on all
three lanes (the component subtracted is the one added back), the ADD wrote every lane the MAD
reads, Rx is not rewritten on those components between the two, and both are unconditional.

THE RANGE FILTER is not optional. The same shape carries depth and fog arithmetic whose 'C' is
(-79714900, 79526900, 98439600) or (6.66, -3.67, 41.95); an endpoint is a colour only when all
three lanes are finite and in [0, 1].

TERMINAL. What the C++ applies is the lerp whose MAD is the LAST writer of COL0.rgb (the same
index the round-62 MOV detector reads, with RPCS3_REMIX_FPVCOLHOP at its default 0 - no hops).
A lerp buried earlier in the program feeds further arithmetic and its endpoint is not the surface
colour; those are counted here as 'anywhere' for the record and are not applied. MEASURED: 102 of
them, dominated by an endpoint of (0, 0, 1) - a normal-map fade, not a colour.

THE WEIGHT GATE, measured 2026-09-11 and the reason the range filter alone is NOT enough: 41 of
793 programs end in a strict lerp and every one of the 41 endpoints is in [0, 1] - the range
filter refuses nothing at the terminal. 36 of the 41 fade toward the same (1, 1, 0.858824) with
the weight scaled by 0.203922 (the w lane of the same literal: 'MUL R1.w <- R1, c[].xyzw'), a
level-wide fog term, and the family includes A59484C9C738C0CB, a main world program. A flat
endpoint tint there repaints the world pale yellow. The code-blood pair scale t by exactly 1.0.
So t must be <varying> * k, or a bare literal k, and k >= MIN_WEIGHT; anything else - t from a
TEX, from a MAD, a per-lane product - is an unknown weight and is refused.

SELF-TEST: runs unconditionally before anything is printed. CE3BE6B2685F446C must resolve to
(0.694118, 0.807843, 1.0) with k = 1 - two earlier scans of this corpus were wrong until that
assertion existed - and 9F30538CBDE6C249 to (1, 1, 0.858824) with k = 0.203922, the
swizzle-composed shape that the weight gate then refuses. A differing fixture aborts with the
diff; it does not warn and continue.

INVOCATION TRAP: on this machine `find | xargs` splits 793 paths into batches of 679 + 114, and
each batch prints its own totals - the second batch fails the self-test because the fixtures are
not in it. Pass the directory.
"""
import glob
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fpdis  # noqa: E402

TEMP, INPUT, CONSTANT = 0, 1, 2
FLOW_OPS = {"BRK", "CAL", "IFE", "LOOP", "REP", "RET"}
SWZ = "xyzw"

MIN_WEIGHT = 0.5

# name -> (endpoint, weight scale k). Weight None = the t chain is not <varying> * k.
FIXTURES = {
    "CE3BE6B2685F446C": ((0.694118, 0.807843, 1.0), 1.0),
    "436265406A3B303B": ((0.694118, 0.807843, 1.0), 1.0),
    "9F30538CBDE6C249": ((1.0, 1.0, 0.858824), 0.203922),
}


def temp_name(src):
    return ("H" if src["fp16"] else "R") + str(src["reg"])


def unconditional(in_):
    """SRC0 exec_if_lt / exec_if_eq / exec_if_gr, bits 18..20 (RSXFragmentProgram.h union
    SRC0), all set - the same test scan_fragment_program applies to every writer it trusts."""
    s0 = in_["_raw"][0]
    return all(fpdis.bits(s0, b, 1) for b in (18, 19, 20))


def last_writer(code, reg, mask, before):
    for i in range(before - 1, -1, -1):
        if code[i]["dest"] == reg and (code[i]["mask"] & mask):
            return i
    return -1


def src_text(in_, slot):
    return fpdis.src_text(in_["_raw"][slot], slot, in_["_dest"])


def instr_text(in_):
    mask = "".join(SWZ[k] for k in range(4) if in_["mask"] & (1 << k))
    srcs = ", ".join(src_text(in_, k) for k in range(len(in_["srcs"])))
    extra = ""
    if in_["const"] is not None:
        extra = "   c=[%.6g %.6g %.6g %.6g]" % tuple(in_["const"])
    return "%2d: %-4s %s.%-4s <- %s%s" % (in_["slot"], in_["name"], in_["dest"] or "-",
                                         mask, srcs, extra)


def colour_range(rgb):
    return all(v == v and 0.0 <= v <= 1.0 for v in rgb)


def lerp_at(code, mad_index):
    """The endpoint if code[mad_index] is 'MAD out.xyz <- t, Rn, Rx' closing an
    'ADD Rn <- -Rx, c[]'. Returns (rgb, info) on a match, (None, reason) otherwise."""
    mad = code[mad_index]

    if mad["name"] != "MAD" or mad["dest"] is None or (mad["mask"] & 7) != 7:
        return None, "not-mad-rgb"
    if not unconditional(mad):
        return None, "mad-predicated"

    s2 = mad["srcs"][2]
    if s2["reg_type"] != TEMP or s2["neg"] or s2["abs"]:
        return None, "mad-src2-not-plain-temp"
    rx = temp_name(s2)

    reason = "no-rn-slot"
    for n_slot in (1, 0):
        sn = mad["srcs"][n_slot]
        if sn["reg_type"] != TEMP or sn["neg"] or sn["abs"]:
            continue
        rn = temp_name(sn)

        need = 0
        for lane in range(3):
            need |= 1 << sn["swz"][lane]

        add_index = last_writer(code, rn, need, mad_index)
        if add_index < 0:
            reason = "rn-no-writer"
            continue
        add = code[add_index]
        if add["name"] != "ADD":
            reason = "rn-writer-not-add"
            continue
        if (add["mask"] & need) != need:
            reason = "add-partial-write"
            continue
        if not unconditional(add):
            reason = "add-predicated"
            continue
        if add["const"] is None:
            reason = "add-no-constant"
            continue

        tslot = cslot = None
        for s in range(2):
            src = add["srcs"][s]
            if src["reg_type"] == TEMP and src["neg"] and not src["abs"]:
                tslot = s
            elif src["reg_type"] == CONSTANT and not src["neg"] and not src["abs"]:
                cslot = s
        if tslot is None or cslot is None:
            reason = "add-not-negtemp-plus-const"
            continue
        if temp_name(add["srcs"][tslot]) != rx:
            reason = "add-temp-is-not-mad-rx"
            continue

        rgb = []
        rx_components = 0
        ok = True
        for lane in range(3):
            m = sn["swz"][lane]
            a = add["srcs"][tslot]["swz"][m]
            b = s2["swz"][lane]
            if a != b:
                ok = False
                break
            rx_components |= 1 << b
            rgb.append(add["const"][add["srcs"][cslot]["swz"][m]])
        if not ok:
            reason = "rx-lane-mismatch"
            continue

        for i in range(add_index + 1, mad_index):
            if code[i]["dest"] == rx and (code[i]["mask"] & rx_components):
                ok = False
                break
        if not ok:
            reason = "rx-rewritten-between"
            continue

        return rgb, {"add": add_index, "mad": mad_index, "rx": rx, "rn": rn,
                     "t_slot": 1 - n_slot}

    return None, reason


def weight_scale(code, mad_index, t_slot):
    """k when the MAD's t operand is a bare literal or a broadcast temp whose last writer is an
    unconditional 'MUL <lane> <- <varying>, c[]'; None for any other t chain (unknown weight)."""
    mad = code[mad_index]
    t = mad["srcs"][t_slot]
    if t["neg"] or t["abs"]:
        return None
    comp = t["swz"][0]
    if t["swz"][1] != comp or t["swz"][2] != comp:
        return None
    if t["reg_type"] == CONSTANT:
        return mad["const"][comp] if mad["const"] is not None else None
    if t["reg_type"] != TEMP:
        return None
    w = last_writer(code, temp_name(t), 1 << comp, mad_index)
    if w < 0:
        return None
    mul = code[w]
    if mul["name"] != "MUL" or not unconditional(mul) or mul["const"] is None:
        return None
    kslot = None
    for s in range(2):
        src = mul["srcs"][s]
        if src["reg_type"] == CONSTANT and not src["neg"] and not src["abs"]:
            kslot = s
    if kslot is None:
        return None
    other = mul["srcs"][1 - kslot]
    if other["reg_type"] == CONSTANT or other["neg"] or other["abs"]:
        return None
    return mul["const"][mul["srcs"][kslot]["swz"][comp]]


def loose_idiom(code):
    """The shape as first described: an ADD of a negated temp and a constant whose destination
    register is read by ANY later MAD. Counted for comparison only - it has no lane check, no
    Rx round-trip, and no range filter, and it is what a mtime-scoped scan reported as 274."""
    for i, in_ in enumerate(code):
        if in_["name"] != "ADD" or in_["dest"] is None:
            continue
        srcs = in_["srcs"]
        if not any(s["reg_type"] == TEMP and s["neg"] for s in srcs):
            continue
        if not any(s["reg_type"] == CONSTANT for s in srcs):
            continue
        rn = in_["dest"]
        for m in code[i + 1:]:
            if m["name"] == "MAD" and any(s["reg_type"] == TEMP and temp_name(s) == rn
                                          for s in m["srcs"]):
                return True
    return False


class Program:
    def __init__(self, path):
        self.path = path
        self.name = os.path.splitext(os.path.basename(path))[0].upper()
        self.code, _, _ = fpdis.decode(path)
        self.has_flow = any(i["name"] in FLOW_OPS for i in self.code)
        self.truncated = not any(i["end"] for i in self.code)

        # COL0 is R0 with 32-bit exports and H0 without; the shader-control bit is not in the
        # file, so it is inferred from which one the program writes (as fpelect.py does).
        writes_r0 = any(i["dest"] == "R0" and i["mask"] for i in self.code)
        writes_h0 = any(i["dest"] == "H0" and i["mask"] for i in self.code)
        self.col0 = "R0" if writes_r0 or not writes_h0 else "H0"

        self.loose = loose_idiom(self.code)

        self.any_rgb = None
        self.any_info = None
        for idx, in_ in enumerate(self.code):
            if in_["name"] == "MAD":
                rgb, info = lerp_at(self.code, idx)
                if rgb is not None:
                    self.any_rgb, self.any_info = rgb, info
                    break

        self.rgb = None
        self.info = None
        self.reason = None
        if self.has_flow:
            self.reason = "flow"
        elif self.truncated:
            self.reason = "truncated"
        else:
            idx = last_writer(self.code, self.col0, 7, len(self.code))
            if idx < 0:
                self.reason = "no-col0-rgb-writer"
            else:
                rgb, info = lerp_at(self.code, idx)
                if rgb is None:
                    self.reason = info
                else:
                    self.rgb, self.info = rgb, info

        self.k = None
        if self.rgb is not None:
            self.k = weight_scale(self.code, self.info["mad"], self.info["t_slot"])
        self.applied = (self.rgb is not None and colour_range(self.rgb)
                        and self.k is not None and self.k >= MIN_WEIGHT)

    def t_description(self):
        mad = self.code[self.info["mad"]]
        slot = self.info["t_slot"]
        text = src_text(mad, slot)
        t = mad["srcs"][slot]
        if t["reg_type"] == TEMP:
            w = last_writer(self.code, temp_name(t), 1 << t["swz"][0], self.info["mad"])
            if w >= 0:
                text += "  (%s)" % instr_text(self.code[w]).strip()
        return text


def self_test(programs):
    by_name = {p.name: p for p in programs}
    bad = []
    for name, (want, want_k) in FIXTURES.items():
        p = by_name.get(name)
        if p is None:
            bad.append("%s: not in the corpus given" % name)
            continue
        if p.rgb is None:
            bad.append("%s: refused (%s), wanted %s" % (name, p.reason, want))
            continue
        if any(abs(g - w) > 1e-5 for g, w in zip(p.rgb, want)):
            bad.append("%s: got (%.6g, %.6g, %.6g), wanted %s" % ((name,) + tuple(p.rgb) + (want,)))
        if (p.k is None) != (want_k is None) or (p.k is not None and abs(p.k - want_k) > 1e-5):
            bad.append("%s: weight k=%s, wanted %s" % (name, p.k, want_k))
    if bad:
        print("SELF-TEST FAILED - this scan's output must not be used:")
        for line in bad:
            print("  " + line)
        sys.exit(2)
    for name, (want, want_k) in FIXTURES.items():
        p = by_name[name]
        print("self-test: %s -> (%.6g, %.6g, %.6g) k=%s applied=%d  OK" % (
            (name,) + tuple(p.rgb) + (p.k, p.applied)))


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
    flow = sum(p.has_flow for p in programs)
    trunc = sum(p.truncated for p in programs)
    loose = sum(p.loose for p in programs)
    anyw = [p for p in programs if p.any_rgb is not None]
    anyw_colour = [p for p in anyw if colour_range(p.any_rgb)]
    term = [p for p in programs if p.rgb is not None]
    term_colour = [p for p in term if colour_range(p.rgb)]
    applied = [p for p in term_colour if p.applied]

    print()
    print("programs=%d  flow=%d  truncated=%d" % (n, flow, trunc))
    print("loose idiom (ADD -temp,const; dest read by a later MAD) : %d" % loose)
    print("strict lerp anywhere (Rx round-trips, lanes composed)   : %d   in [0,1]^3: %d"
          % (len(anyw), len(anyw_colour)))
    print("strict lerp at the COL0.rgb terminal                     : %d   in [0,1]^3: %d"
          % (len(term), len(term_colour)))
    print("  ... and t = <varying> * k with k >= %g (what C++ applies): %d"
          % (MIN_WEIGHT, len(applied)))

    print()
    print("terminal lerps with a colour endpoint (APPLIED = weight gate passed):")
    for p in sorted(term_colour, key=lambda p: (not p.applied, p.name)):
        print("  %s  rgb=(%.6g, %.6g, %.6g)  = (%d, %d, %d)  k=%s%s" % (
            (p.name,) + tuple(p.rgb) + tuple(int(round(v * 255)) for v in p.rgb)
            + (p.k, "  APPLIED" if p.applied else "")))
        print("      %s" % instr_text(p.code[p.info["add"]]).strip())
        print("      %s" % instr_text(p.code[p.info["mad"]]).strip())
        print("      t = %s" % p.t_description())

    print()
    print("terminal lerps REFUSED by the range filter (the depth/fog arithmetic):")
    for p in sorted(term, key=lambda p: p.name):
        if p not in term_colour:
            print("  %s  C=(%.6g, %.6g, %.6g)" % ((p.name,) + tuple(p.rgb)))

    print()
    print("anywhere-but-not-terminal lerps with a colour endpoint (NOT applied):")
    for p in sorted(anyw_colour, key=lambda p: p.name):
        if p.rgb is None:
            print("  %s  rgb=(%.6g, %.6g, %.6g)  terminal=%s" % (
                (p.name,) + tuple(p.any_rgb) + (p.reason,)))

    print()
    reasons = {}
    for p in programs:
        if p.rgb is None:
            reasons[p.reason] = reasons.get(p.reason, 0) + 1
    print("terminal refusals: " + "  ".join("%s=%d" % kv for kv in sorted(reasons.items(),
                                                                          key=lambda kv: -kv[1])))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
