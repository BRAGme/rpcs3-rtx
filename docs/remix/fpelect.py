r"""Static replay of the Remix backend's ALBEDO-UNIT ELECTION over .fp files.

    python docs/remix/fpelect.py bin/remix_ucode/*.fp

Mirrors, in Python, exactly what RemixTransforms.cpp's scan_fragment_program() and
RemixGSRender.cpp's albedo_unit_mask() decide for one fragment program - so the size and
the blast radius of an election change can be read off disk BEFORE the emulator is built,
and so a run's own 'Remix albedo-elect:' rows can be cross-checked against an independent
implementation of the same rule inside one run. Any disagreement between the two is a
finding, not a rounding difference.

Decoding is imported from fpdis.py (same directory) so the two tools can never disagree
about what a program says. fpdis.py's third-operand bug was the SIXTH analysis-tool bug of
this project; that is why this file has a self-test rather than a comment saying it works.

SELF-TEST: runs unconditionally, before anything is printed, over the frozen fixture set in
bin\remix_ucode\ (the thirteen programs the round-58 plan tabulates). A single differing
row aborts with a diff - it does not print a warning and continue.

WHAT IT MODELS
  sampled/colour/narrow      scan_fragment_program's three masks (backward reachability
                             from COL0 with NO kill, then the >=3-channel 'wide' rule)
  before=                    albedo_unit_mask() at today's defaults (FPALBEDO=1,
                             FPALBEDONARROW=1): lowest set unit of the elected mask
  lerp=a>b:w                 the two-texture blend the colour target is, when it is one
  lanes=                     which lane pair (xy / zw) each unit's TEX reads
  after=/rule=               the same election with FPLERPUNIT=1 and UVLANES=1

WHAT IT CANNOT MODEL, and therefore assumes
  * referenced_textures_mask - taken as == sampled_mask (the fingerprint documents the two
    as equal by construction).
  * whether a unit is bindable - every unit in the mask is assumed bindable. In the runtime
    an unbindable unit is walked past by albedo_texture_unit_in(); here it is not.
  * the VERTEX program's macro lane pair, which decides the lane rule. Supply it with
    --macro=xy|zw|none (default none = the lane rule cannot fire); the fixture table below
    carries the four programs whose VP (7565B4CBD93682D9.vp) was replayed by hand.
"""
import glob
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fpdis  # noqa: E402

# fp_colour_sources() in RemixTransforms.cpp, by mnemonic. Anything absent contributes no
# colour-preserving source and terminates the reachability walk on that path.
COLOUR_SOURCES = {
    "MOV": 1, "FRC": 1, "FLR": 1, "PK4": 1, "UP4": 1, "PK2": 1, "UP2": 1,
    "PKB": 1, "UPB": 1, "PK16": 1, "UP16": 1, "PKG": 1, "UPG": 1,
    "MUL": 2, "ADD": 2, "MIN": 2, "MAX": 2, "SLT": 2, "SGE": 2, "SLE": 2,
    "SGT": 2, "SNE": 2, "SEQ": 2, "DIV": 2,
    "MAD": 3, "LRP": 3,
}

# fp_op_is_sample()
SAMPLE_OPS = {"TEX", "TXP", "TXD", "TXL", "TXB", "TEXBEM", "TXPBEM"}

# fp_op_is_flow()
FLOW_OPS = {"BRK", "CAL", "IFE", "LOOP", "REP", "RET"}

TEMP, INPUT, CONSTANT = 0, 1, 2

LANE_XY, LANE_ZW, LANE_MIXED, LANE_NONE = 0, 1, 2, 3
LANE_NAME = {LANE_XY: "xy", LANE_ZW: "zw", LANE_MIXED: "mix", LANE_NONE: "-"}


def popcount(v):
    return bin(v).count("1")


def lowest_unit(mask):
    """albedo_texture_unit_in(mask, 0) with every unit assumed bindable."""
    for unit in range(16):
        if mask & (1 << unit):
            return unit
    return -1


def temp_name(src):
    return ("H" if src["fp16"] else "R") + str(src["reg"])


class Program:
    def __init__(self, path):
        self.path = path
        self.code, _, _ = fpdis.decode(path)
        self.has_flow = any(i["name"] in FLOW_OPS for i in self.code)
        self.truncated = not any(i["end"] for i in self.code)

        # COL0 is R0 with 32-bit exports and H0 without. The shader-control bit is not in the
        # .fp file, so it is inferred from which of the two the program actually writes;
        # printed as col0= so a wrong inference is visible rather than silent.
        writes_r0 = any(i["dest"] == "R0" and i["mask"] for i in self.code)
        writes_h0 = any(i["dest"] == "H0" and i["mask"] for i in self.code)
        self.col0 = "R0" if writes_r0 or not writes_h0 else "H0"

        self.sampled = 0
        self.wide = 0
        self.lanes = [LANE_NONE] * 16
        seen = 0

        for in_ in self.code:
            if in_["name"] not in SAMPLE_OPS:
                continue

            unit = in_["tex_num"]
            self.sampled |= 1 << unit

            if popcount(in_["mask"] & 0xF) >= 3:
                self.wide |= 1 << unit

            src0 = in_["srcs"][0] if in_["srcs"] else None
            lane = LANE_NONE

            if src0 and src0["reg_type"] == INPUT and 4 <= in_["attr"] <= 13:
                if src0["swz"][0] == 0 and src0["swz"][1] == 1:
                    lane = LANE_XY
                elif src0["swz"][0] == 2 and src0["swz"][1] == 3:
                    lane = LANE_ZW
                else:
                    lane = LANE_MIXED

            if not (seen & (1 << unit)):
                seen |= 1 << unit
                self.lanes[unit] = lane
            elif self.lanes[unit] != lane:
                self.lanes[unit] = LANE_MIXED

        self.narrow = self.sampled & ~self.wide

        self.colour = self.walk(0x7) & self.sampled
        self.alpha_only = False
        if self.colour == 0:
            self.colour = self.walk(0xF) & self.sampled
            self.alpha_only = self.colour != 0

        self.lerp_a = None
        self.lerp_b = None
        self.lerp_w = 0.0
        self.lerp_note = self.find_lerp()

    # --- scan_fragment_program's backward reachability, no kill ---------------------------
    def walk(self, seed):
        live = {self.col0: seed}
        colour = 0

        for _ in range(8):
            changed = False

            for in_ in reversed(self.code):
                if in_["dest"] is None or not in_["mask"]:
                    continue
                if not (live.get(in_["dest"], 0) & in_["mask"]):
                    continue
                if in_["name"] in SAMPLE_OPS:
                    colour |= 1 << in_["tex_num"]
                    continue

                for s in range(COLOUR_SOURCES.get(in_["name"], 0)):
                    if s >= len(in_["srcs"]):
                        break
                    src = in_["srcs"][s]
                    if src["reg_type"] != TEMP:
                        continue
                    name = temp_name(src)
                    if live.get(name, 0) != 0xF:
                        live[name] = 0xF
                        changed = True

            if not changed:
                break

        return colour

    # --- the round-58 lerp detector -------------------------------------------------------
    def writer_of(self, reg, bound):
        """Last instruction before 'bound' writing ALL of reg.xyz.

        Returns (index, None) or (None, reason). A writer that touches only part of .xyz
        means the value at the use site is a mix of two instructions, which is refused
        rather than followed - the same 'refuse and count' rule the backend applies.
        """
        for i in range(bound - 1, -1, -1):
            in_ = self.code[i]
            if in_["dest"] != reg or not (in_["mask"] & 0x7):
                continue
            if (in_["mask"] & 0x7) != 0x7:
                return None, "partial-write"
            return i, None
        return None, "no-writer"

    def mul_tint_hop(self, in_):
        """'temp * constant' with exactly one temp and one constant. Returns the temp or None."""
        if in_["name"] != "MUL" or in_["const"] is None or len(in_["srcs"]) < 2:
            return None
        temps = [s for s in in_["srcs"][:2] if s["reg_type"] == TEMP]
        consts = [s for s in in_["srcs"][:2] if s["reg_type"] == CONSTANT]
        if len(temps) != 1 or len(consts) != 1:
            return None
        if temps[0]["neg"] or temps[0]["abs"]:
            return None
        return temps[0]

    def resolve_tex(self, reg, bound, hops):
        """Walk back from reg.xyz to the TEX that produced it. Returns (unit, None)/(None, why)."""
        for _ in range(hops + 1):
            i, why = self.writer_of(reg, bound)
            if i is None:
                return None, why

            in_ = self.code[i]

            if in_["name"] in SAMPLE_OPS:
                if popcount(in_["mask"] & 0xF) < 3:
                    return None, "narrow-tex"
                return in_["tex_num"], None

            temp = self.mul_tint_hop(in_)
            if temp is None:
                return None, "not-tex"

            reg = temp_name(temp)
            bound = i

        return None, "hop-budget"

    def find_lerp(self):
        if self.has_flow:
            return "flow"
        if self.truncated:
            return "truncated"

        # The terminal, plus up to two colour-preserving unary hops. 89137D5E0D4DFD18.fp
        # ends '34: MUL R0.xyz <- R0, c[0.619608]' - a per-material tint sitting on top of
        # the lerp - so the last COL0 writer is not always the MAD itself.
        reg = self.col0
        bound = len(self.code)
        mad = None

        for hop in range(3):
            i, why = self.writer_of(reg, bound)
            if i is None:
                return "col0-" + why

            in_ = self.code[i]

            if in_["name"] == "MAD":
                mad = in_
                bound = i
                break

            if hop == 2:
                return "terminal-" + in_["name"].lower()

            if in_["name"] == "MOV" and in_["srcs"] and in_["srcs"][0]["reg_type"] == TEMP \
                    and not in_["srcs"][0]["neg"] and not in_["srcs"][0]["abs"]:
                reg = temp_name(in_["srcs"][0])
                bound = i
                continue

            temp = self.mul_tint_hop(in_)
            if temp is None:
                return "terminal-" + in_["name"].lower()

            reg = temp_name(temp)
            bound = i

        if mad is None:
            return "terminal-not-mad"

        s0, s1, s2 = mad["srcs"][0], mad["srcs"][1], mad["srcs"][2]

        if s0["reg_type"] != TEMP or s0["neg"] or s0["abs"]:
            return "mad-src0"
        if s1["reg_type"] != CONSTANT or mad["const"] is None:
            return "mad-src1"
        if s2["reg_type"] != TEMP or s2["neg"] or s2["abs"]:
            return "mad-src2"

        weight = mad["const"][s1["swz"][0] & 3]
        diff = temp_name(s0)
        base = temp_name(s2)

        j, why = self.writer_of(diff, bound)
        if j is None:
            return "diff-" + why

        add = self.code[j]
        if add["name"] != "ADD" or len(add["srcs"]) < 2:
            return "diff-not-add"

        a0, a1 = add["srcs"][0], add["srcs"][1]
        if a0["reg_type"] != TEMP or a0["neg"] or a0["abs"]:
            return "add-src0"
        if a1["reg_type"] != TEMP or not a1["neg"] or a1["abs"]:
            return "add-src1"

        major = temp_name(a0)     # the term the weight scales towards
        minor = temp_name(a1)     # the base, which the MAD adds back

        if base != minor:
            return "base-mismatch"

        # The base takes no hop: it is read twice (by the ADD and by the MAD) and any
        # arithmetic between them would make the two reads different values.
        unit_a, why = self.resolve_tex(minor, j, 0)
        if unit_a is None:
            return "a-" + why

        unit_b, why = self.resolve_tex(major, j, 1)
        if unit_b is None:
            return "b-" + why

        if unit_a == unit_b:
            return "same-unit"

        self.lerp_a = unit_a
        self.lerp_b = unit_b
        self.lerp_w = weight
        return "ok"

    # --- albedo_unit_mask() ---------------------------------------------------------------
    def elect_before(self):
        referenced = self.sampled
        colour = self.colour & referenced

        if colour != 0 and colour != referenced:
            return colour, "ucode"

        wide = referenced & ~self.narrow

        if colour == 0 or wide == 0 or wide == referenced:
            return referenced, "lowest"

        if lowest_unit(wide) == lowest_unit(referenced):
            return referenced, "lowest"

        return wide, "narrow"

    def elect_after(self, macro):
        mask, rule = self.elect_before()

        # Step 3: the lerp rule.
        if self.lerp_a is not None and self.lerp_w > 0.5:
            a, b = self.lerp_a, self.lerp_b
            if (mask & (1 << a)) and (mask & (1 << b)):
                candidate = mask & ~(1 << a)
                if candidate and lowest_unit(candidate) != lowest_unit(mask):
                    mask, rule = candidate, "lerp"

        # Step 4d: the lane rule. The lerp's MAJOR unit is never dropped by it - measured on
        # D57D44C02167F820.fp, whose 'lerp=0>1:0.7 lanes=1:xy macro=xy' would otherwise have the
        # lane rule demote the very unit the program states is 70% of its colour. The lerp is a
        # direct reading of the arithmetic; the macro pair is an inference from coordinate scale.
        protected = self.lerp_b if (self.lerp_a is not None and self.lerp_w > 0.5) else None

        if macro in (LANE_XY, LANE_ZW):
            macro_units = 0
            for unit in range(16):
                if unit == protected:
                    continue
                if (mask & (1 << unit)) and self.lanes[unit] == macro:
                    macro_units |= 1 << unit

            keep = mask & ~macro_units
            if macro_units and keep and (keep & self.wide):
                if lowest_unit(keep) != lowest_unit(mask):
                    mask, rule = keep, "lane"

        return mask, rule


# The macro lane pair of the VERTEX program each of these is drawn by, replayed by hand from
# bin\remix_ucode\7565B4CBD93682D9.vp:
#   1: MUL r0.xy <- v4.xyxx, c[464].xxxx      the 1/2048 dequant
#  10: MUL r0.zw <- r0.xxxy, c[463].xxxx      ...times c463 = 0.1 live, i.e. the macro pair
#  11: MOV o[7](TEX0).xyzw <- r0.xyzw
# (raw hash, macro lane pair, lerp, lanes of tex0, before, after, rule).
#
# Rows 1-13 are the round-58 plan's Verification table. Their macro column is the pair the plan
# replayed by hand out of bin\remix_ucode\7565B4CBD93682D9.vp, and LANE_NONE everywhere the plan
# did not replay a vertex program at all - which is most of them.
#
# Rows 14-16 carry the macro pair the RUNTIME computes for those same programs' vertex programs,
# read off the 'Remix albedo-elect:' census on 2026-09-09. They exist because the plan's table was
# built with macro data for ONE vertex program and therefore could not predict how widely the lane
# rule fires. Rows 15 and 16 are two programs the plan names as controls that must NOT move, with
# their real macro pair: the lane rule DOES move them, 0->2 and 1->4. That is a finding, and it is
# why RPCS3_REMIX_UVLANES must stay off until the rule is narrowed. Row 14 is the two-scales
# family whose fragment program that run captured, and is what the lerp-major guard is for.
FIXTURE = [
    ("B3EFF9C2B4AE9F66", LANE_ZW,   "0>1:0.85", "zw", 0, 1, "lerp"),
    ("4D454157A816F756", LANE_NONE, "0>1:0.95", "zw", 0, 1, "lerp"),
    ("89137D5E0D4DFD18", LANE_ZW,   "0>1:0.89", "zw", 0, 1, "lerp"),
    ("5410780A71852F83", LANE_ZW,   "0>1:1.15", "zw", 0, 1, "lerp"),
    ("C940394021B83EE7", LANE_ZW,   "none",     "zw", 0, 1, "lane"),
    ("0B7312EE9A170A62", LANE_NONE, "0>1:0.35", "xy", 0, 0, "lowest"),
    ("633644D3E78A46A4", LANE_NONE, "none",     "xy", 0, 0, "lowest"),
    ("660DC2FCD3E57A41", LANE_NONE, "none",     "xy", 0, 0, "lowest"),
    ("87CE51141C3C01AC", LANE_NONE, "none",     "xy", 0, 0, "lowest"),
    ("DD26A93C6D0D5CF8", LANE_NONE, "none",     "xy", 0, 0, "lowest"),
    ("DB289A9ED10C30F8", LANE_NONE, "none",     "xy", 0, 0, "lowest"),
    ("EEDE93C214E686AF", LANE_NONE, "none",     "xy", 0, 0, "lowest"),
    # The plan's table writes lanes(tex0) as an em dash here, meaning 'not applicable to the
    # verdict' - unit 0 IS sampled on this program, through .xy. Measured, not assumed.
    ("E2BB0F1275CA632E", LANE_NONE, "none",     "xy", 1, 1, "narrow"),
    # --- measured macro, 2026-09-09 -----------------------------------------------------------
    ("D57D44C02167F820", LANE_XY,   "0>1:0.7",  "xy", 0, 1, "lerp"),
    ("EEDE93C214E686AF", LANE_XY,   "none",     "xy", 0, 2, "lane"),
    ("E2BB0F1275CA632E", LANE_XY,   "none",     "xy", 1, 4, "lane"),
]

# What a file named on the command line gets when no --macro is given: the pair the plan replayed.
FIXTURE_MACRO = {row[0]: row[1] for row in FIXTURE[:13] if row[1] != LANE_NONE}


def lerp_text(p):
    if p.lerp_a is None:
        return "none:" + p.lerp_note
    return "%d>%d:%.3g" % (p.lerp_a, p.lerp_b, p.lerp_w)


def lanes_text(p):
    parts = []
    for unit in range(16):
        if p.sampled & (1 << unit):
            parts.append("%d:%s" % (unit, LANE_NAME[p.lanes[unit]]))
    return ",".join(parts) if parts else "-"


def describe(path, macro):
    p = Program(path)
    before_mask, before_rule = p.elect_before()
    after_mask, after_rule = p.elect_after(macro)

    return p, ("%s sampled=0x%x wide=0x%x colour=0x%x col0=%s lerp=%s lanes=%s macro=%s "
               "before=%d after=%d rule=%s" % (
                   os.path.splitext(os.path.basename(path))[0],
                   p.sampled, p.wide, p.colour, p.col0,
                   lerp_text(p), lanes_text(p), LANE_NAME[macro],
                   lowest_unit(before_mask), lowest_unit(after_mask),
                   after_rule if after_rule != before_rule else before_rule))


def selftest(ucode_dir):
    failures = []

    for name, macro, lerp, lane0, before, after, rule in FIXTURE:
        path = os.path.join(ucode_dir, name + ".fp")
        label = "%s(macro=%s)" % (name, LANE_NAME[macro])

        if not os.path.isfile(path):
            failures.append("%s: fixture file missing (%s)" % (label, path))
            continue

        p = Program(path)
        got_lerp = "none" if p.lerp_a is None else "%d>%d:%.3g" % (p.lerp_a, p.lerp_b, p.lerp_w)
        got_lane0 = LANE_NAME[p.lanes[0]] if (p.sampled & 1) else "-"
        got_before = lowest_unit(p.elect_before()[0])
        got_after_mask, got_rule = p.elect_after(macro)
        got_after = lowest_unit(got_after_mask)

        for what, want, got in (("lerp", lerp, got_lerp), ("lanes(tex0)", lane0, got_lane0),
                                ("before", before, got_before), ("after", after, got_after),
                                ("rule", rule, got_rule)):
            if want != got:
                failures.append("%s: %s expected %r, got %r" % (label, what, want, got))

    if failures:
        sys.stderr.write("fpelect SELF-TEST FAILED (%d):\n  %s\n"
                         % (len(failures), "\n  ".join(failures)))
        sys.exit(2)

    print("fixture ok (%d programs)" % len(FIXTURE))


def main(argv):
    here = os.path.dirname(os.path.abspath(__file__))
    repo = os.path.dirname(os.path.dirname(here))
    ucode_dir = os.path.join(repo, "bin", "remix_ucode")

    macro = LANE_NONE
    paths = []

    for arg in argv:
        if arg.startswith("--macro="):
            macro = {"xy": LANE_XY, "zw": LANE_ZW, "none": LANE_NONE}[arg.split("=", 1)[1]]
        elif arg == "--selftest":
            pass
        else:
            paths.extend(sorted(glob.glob(arg)) or [arg])

    selftest(ucode_dir)

    for path in paths:
        name = os.path.splitext(os.path.basename(path))[0].upper()
        try:
            _, line = describe(path, FIXTURE_MACRO.get(name, macro))
        except Exception as exc:                      # noqa: BLE001 - a bad file is data
            print("%s ERROR %s" % (name, exc))
            continue
        print(line)


if __name__ == "__main__":
    main(sys.argv[1:])
