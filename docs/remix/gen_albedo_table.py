r"""Build a fragment-program-hash -> diffuse texture unit table for the Remix backend.

    python docs/remix/gen_albedo_table.py <shaderdir> -o bin/BLUS30201.albedo
    python docs/remix/gen_albedo_table.py <shaderdir> --verify bin/remix_ucode

<shaderdir> is the 318 files unpacked from
    PS3_GAME/USRDIR/packfiles/ps3/shaders.vpp_ps3      (unpack with vppdis.py)

WHY THIS EXISTS
    The Remix backend has to decide which of the bound textures is albedo. Its current
    rule is "the lowest bound texture unit". That is a guess. A fragment CGB blob names
    its samplers, and cgbdis.py resolves a fragment parameter's `resource` (<16) to its
    texture unit -- so the shipped shader pack states the answer outright. All that is
    missing is a key the runtime can look up, and RPCS3 already computes one:
    get_fragment_program_ucode_hash().

THE HASH  (rpcs3/Emu/RSX/Program/ProgramStateCache.cpp:701, reimplemented below)
    Two u64 accumulators over 16-byte instructions, each rotated right by a
    position-dependent amount, and -- critically -- an instruction whose source
    operands include a constant causes the NEXT instruction (the constant payload) to
    be SKIPPED. Volition patches fragment constants into the instruction stream at load
    time, so the skip is exactly what makes a shipped, unpatched CGB blob hash the same
    as a live, patched capture.

    VERIFIED  recomputing the hash from the bytes of all 928 .fp files in
    bin/remix_ucode/ reproduces the filename (which is the hash RPCS3 computed) on
    928/928. Run with --verify to reproduce.

DIFFUSE SELECTION -- stated because it is a policy, not a fact in the file
    1. a sampler parameter named exactly `Diffuse_MapSampler`            -> its unit
    2. failing that, one of DIFFUSE_FALLBACK below, in order             -> its unit
    3. failing that, NOTHING IS EMITTED. A blob with only e.g.
       Env_MapSampler + V_Light_MapSampler has no albedo to name and is
       counted, not guessed.
    A blob with two parameters that both resolve to rule 1 and disagree is dropped.

CONFLICTS
    Different blobs can compile to identical microcode (the _c/_mc/_ms/_s quality
    variants routinely do). When two blobs hash the same but name different diffuse
    units, the entry is DROPPED and counted -- never resolved by picking one.
"""
import argparse
import os
import struct
import sys
from collections import Counter, defaultdict

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import cgbdis

DIFFUSE_PRIMARY = "Diffuse_MapSampler"

# Consulted, in order, only when Diffuse_MapSampler is absent. Every name here is a
# sampler that actually occurs in the SR2 pack and is unambiguously the base colour:
#   Diffuse_Map_ASampler   the A half of an A/B diffuse blend pair (140 blobs, unit 0)
#   Diffuse_Map_1Sampler   the 1 half of a 1/2 diffuse blend pair  ( 34 blobs, unit 0)
#   base_sampler           the lowercase post-process naming       ( 36 blobs, unit 0)
DIFFUSE_FALLBACK = ["Diffuse_Map_ASampler", "Diffuse_Map_1Sampler", "base_sampler"]

# DELIBERATELY NOT A FALLBACK: Decal_Map_1Sampler. It is tempting -- 264 blobs have it
# and no Diffuse_MapSampler -- but it is a LAYER, not the base colour. Proof: of the
# 1296 blobs that do name Diffuse_MapSampler, 96 also carry Decal_Map_1Sampler, and in
# every one of those 96 the decal is on unit 0 while the diffuse is on 1, 2 or 4. A
# sampler that sits BELOW the diffuse when both are present is not the diffuse. Those
# 264 blobs are therefore reported as "no-diffuse-name" and left out of the table.

M64 = (1 << 64) - 1


def _rotr64(v, s):
    s &= 63
    return v & M64 if s == 0 else ((v >> s) | (v << (64 - s))) & M64


def _is_any_src_constant(inst):
    """v128 s: (s._u32[1] & 0x300) == 0x200 || masked lo/hi == 0x200, where
    masked = s._u64[1] & 0x30000000300.  x86 v128::loadu reads little-endian."""
    u32_1 = struct.unpack_from('<I', inst, 4)[0]
    u64_1 = struct.unpack_from('<Q', inst, 8)[0]
    masked = u64_1 & 0x30000000300
    return ((u32_1 & 0x300) == 0x200
            or (masked & 0xffffffff) == 0x200
            or ((masked >> 32) & 0xffffffff) == 0x200)


def fp_ucode_hash(data):
    """get_fragment_program_ucode_hash() -- exact reimplementation."""
    acc0 = acc1 = 0
    n = len(data) // 16
    i = 0
    while i < n:
        inst = data[i * 16:i * 16 + 16]
        a, b = struct.unpack_from('<QQ', inst, 0)
        acc0 = (acc0 + _rotr64(a, i * 2)) & M64
        acc1 = (acc1 + _rotr64(b, i * 2 + 1)) & M64
        if _is_any_src_constant(inst):
            i += 1
        i += 1
    return (acc0 + acc1) & M64


def load_blobs(shaderdir):
    """[(filename, blob_index, Blob)] for every FRAGMENT blob under shaderdir."""
    out = []
    nfiles = ntotal = 0
    for f in sorted(os.listdir(shaderdir)):
        p = os.path.join(shaderdir, f)
        if not os.path.isfile(p):
            continue
        nfiles += 1
        data = open(p, 'rb').read()
        for i, b in enumerate(cgbdis.find_blobs(data)):
            ntotal += 1
            if b.type == cgbdis.FRAGMENT:
                out.append((f, i, b))
    return out, nfiles, ntotal


def samplers(blob):
    """[(name, unit)] for the blob's sampler parameters."""
    return [(p.name, p.unit) for p in blob.params if p.kind == "sampler"]


def pick_diffuse(sm):
    """(unit, rule) or (None, reason).  sm is [(name, unit)]."""
    hits = [u for n, u in sm if n == DIFFUSE_PRIMARY]
    if len(set(hits)) > 1:
        return None, "ambiguous-primary"
    if hits:
        return hits[0], "primary"
    for want in DIFFUSE_FALLBACK:
        hits = [u for n, u in sm if n == want]
        if len(set(hits)) == 1:
            return hits[0], "fallback:" + want
        if hits:
            return None, "ambiguous-fallback"
    if not sm:
        return None, "no-samplers"
    return None, "no-diffuse-name"


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument('shaderdir')
    ap.add_argument('-o', '--out', help='write the table here')
    ap.add_argument('--verify', metavar='UCODEDIR',
                    help='recompute the hash of every .fp there and check the filename')
    ap.add_argument('--names', action='store_true',
                    help='append the source shader names as a comment per entry')
    ap.add_argument('--title', default='BLUS30201')
    args = ap.parse_args()

    caps = {}
    if args.verify:
        files = sorted(f for f in os.listdir(args.verify) if f.endswith('.fp'))
        ok, bad = 0, []
        for f in files:
            d = open(os.path.join(args.verify, f), 'rb').read()
            h = "%016X" % fp_ucode_hash(d)
            caps[h] = f
            if h == f[:-3].upper():
                ok += 1
            else:
                bad.append((f, h))
        print("# hash verify: %d/%d .fp captures reproduce their filename"
              % (ok, len(files)), file=sys.stderr)
        for f, h in bad[:20]:
            print("#   FAIL %s -> %s" % (f, h), file=sys.stderr)

    blobs, nfiles, ntotal = load_blobs(args.shaderdir)
    print("# %d files, %d CGB blobs, %d fragment"
          % (nfiles, ntotal, len(blobs)), file=sys.stderr)

    per_hash = defaultdict(set)       # hash -> {unit}
    per_hash_names = defaultdict(set)
    all_hashes = set()
    reasons = Counter()
    rules = Counter()
    hist = Counter()
    sampler_names = Counter()
    nsamp = Counter()
    lower_light = 0
    both_light = 0
    for fname, idx, b in blobs:
        h = fp_ucode_hash(b.raw_ucode())
        all_hashes.add(h)
        sm = samplers(b)
        nsamp[len(sm)] += 1
        for n, _u in sm:
            sampler_names[n] += 1
        d = dict((n, u) for n, u in sm)
        if DIFFUSE_PRIMARY in d and "V_Light_MapSampler" in d:
            both_light += 1
            if d["V_Light_MapSampler"] < d[DIFFUSE_PRIMARY]:
                lower_light += 1
        unit, rule = pick_diffuse(sm)
        if unit is None:
            reasons[rule] += 1
            continue
        rules[rule] += 1
        per_hash[h].add(unit)
        per_hash_names[h].add("%s[%d]" % (fname, idx))

    conflicts = {h: u for h, u in per_hash.items() if len(u) > 1}
    table = {h: next(iter(u)) for h, u in per_hash.items() if len(u) == 1}
    for h, u in table.items():
        hist[u] += 1

    print("# distinct fragment hashes: %d" % len(all_hashes), file=sys.stderr)
    print("# distinct hashes with a diffuse: %d   conflicts dropped: %d   table: %d"
          % (len(per_hash), len(conflicts), len(table)), file=sys.stderr)
    print("# selection rules (per blob): %s" % dict(rules), file=sys.stderr)
    print("# no diffuse emitted (per blob): %s" % dict(reasons), file=sys.stderr)
    print("# sampler-count histogram (per blob): %s"
          % sorted(nsamp.items()), file=sys.stderr)
    print("# sampler names: %s" % sampler_names.most_common(), file=sys.stderr)
    print("# unit histogram (table entries): %s"
          % sorted(hist.items()), file=sys.stderr)
    print("# blobs with both Diffuse_MapSampler and V_Light_MapSampler: %d;"
          " light on a LOWER unit than diffuse: %d" % (both_light, lower_light),
          file=sys.stderr)
    if caps:
        inboth = sum(1 for h in all_hashes if "%016X" % h in caps)
        covered = sum(1 for h in table if "%016X" % h in caps)
        print("# captures matched by some SR2 fragment blob: %d/%d;"
              " of those, %d are covered by the table"
              % (inboth, len(caps), covered), file=sys.stderr)
        for h in sorted(conflicts):
            if "%016X" % h in caps:
                print("#   LIVE CONFLICT %016X units=%s %s"
                      % (h, sorted(conflicts[h]),
                         ",".join(sorted(per_hash_names[h]))), file=sys.stderr)

    if args.out:
        with open(args.out, 'w') as fh:
            fh.write("# %s fragment-program hash -> diffuse texture unit\n" % args.title)
            fh.write("# generated by docs/remix/gen_albedo_table.py from the shipped\n")
            fh.write("# shaders.vpp_ps3 CGB parameter tables.  <fp hash> <unit>\n")
            fh.write("# entries=%d  conflicts_dropped=%d  units=%s\n"
                     % (len(table), len(conflicts), sorted(hist.items())))
            for h in sorted(table):
                if args.names:
                    fh.write("%016X %d  # %s\n"
                             % (h, table[h], ",".join(sorted(per_hash_names[h]))))
                else:
                    fh.write("%016X %d\n" % (h, table[h]))
        print("# wrote %s" % args.out, file=sys.stderr)
    return 0


if __name__ == '__main__':
    sys.exit(main())
