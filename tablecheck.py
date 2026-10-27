"""Does x86map's jump-table detection find the tables that are really there?

WHY THIS EXISTS. On `ENCHANT.EXP` the tool reports table sites that Capstone
decodes as ordinary code, and a first one-off check found a phantom function at
0x68FFFF67 reached from a `table at 0xF0CED` which is `push 6; call 0xE6A58;
add esp,0x44; ...` with no indirect transfer in it at all. One false positive on
a handful of spot checks is not a rate. This measures the rate.

THE HARDER QUESTION IS WHAT "REALLY THERE" MEANS, and it has to be answered
without using the thing under test. It is a byte pattern: an indirect jump
through a scaled index is one of a small set of encodings, and `jmp [reg*4+disp32]`
is the only form `_table` claims. So the population is the set of addresses where
that encoding actually occurs, found by scanning bytes rather than by decoding
control flow. A table detector that reports a site where that encoding does not
occur is wrong, whatever the table contents later look like.

WHAT THIS DOES NOT SETTLE. It compares SITES, not table BASES or COUNTS. A site
can be found at the right address with the wrong base or one entry short, and this
passes it. That is a separate measurement and a smaller one, printed for the sites
that do match so a reader can see how far apart the two are.

    python -m fmtownskit.tablecheck GAME.EXP --seed 0xe6e68,0xe1c9c

Exit status 1 if any reported site has no such encoding, so a caller can gate on
it.
"""
import argparse
import sys

try:
    import capstone
    from capstone import x86_const as X
except ImportError:      # pragma: no cover
    capstone = None

from . import exp as expmod
from . import x86map

# Every form of `jmp` OR `call` through an index*4 with no base register.
# _table claims both: `self._table(pc, ins) if ... m in ('call', 'jmp')`.
#
# FF /4 = jmp, FF /2 = call, so the second opcode byte is 0x24 or 0x14. Both are
# mod=00, rm=100 (SIB) with SIB.base=101 (disp32, no base) and scale=10 (times 4):
# sib & 0xC7 == 0x85, i.e. 0x85, 0x8D, 0x95, 0x9D, 0xA5, 0xAD, 0xB5, 0xBD.
#
# LEAVING OUT FF 14 REPORTED A FALSE POSITIVE THAT WAS NOT ONE. 0xFE4D5 is
# `2E FF 14 9D E6 E2 0F 00` = `call [ebx*4 + 0xFE2E6]`, a real table transfer, and
# a scan for jmp only called it a false positive. The population has to be
# everything the detector is allowed to claim, not everything that is convenient
# to find.
SIB_BYTES = range(0x85, 0xC0, 0x08)
TRANSFER_OPCODES = (0x24, 0x14)      # /4 jmp, /2 call

# PREFIXES ARE PART OF THE INSTRUCTION AND THE SCAN MISSED THEM.
#
# ENCHANT.EXP's jump table at 0xE2D29 is reached by
#
#   0xE2F21  2E FF 24 95 29 2D 0E 00     jmp [edx*4 + 0xE2D29]
#
# The 2E is a CS segment override. A scan for `FF 24 95` therefore finds it at
# 0xE2F22, one byte late, and every site it reports is then compared against a
# claimed site that is correct, and fails. That produced a 7-of-7 false-positive
# rate for a detector whose bases were right all along. The scan has to begin at
# the first prefix byte.
LEGACY_PREFIXES = {0x2E, 0x36, 0x3E, 0x26, 0x64, 0x65, 0x66, 0x67, 0xF0, 0xF2, 0xF3}


def encoded_sites(image):
    """Every offset of an optional prefix run followed by `FF /2|/4 <sib> <disp32>`."""
    out = []
    n = len(image)
    i = 0
    while i < n:
        j = i
        while j < n and image[j] in LEGACY_PREFIXES:
            j += 1
        if (j + 6 <= n and image[j] == 0xFF and image[j + 1] in TRANSFER_OPCODES
                and image[j + 2] in SIB_BYTES):
            out.append(i)
            i = j + 6
        else:
            i += 1
    return out


def confirmed_by_capstone(image, base, offset):
    """The bytes at `offset` decode as exactly one jmp through [index*4+disp].

    The byte scan alone would count an `FF 24 85` that happens to sit inside a
    longer instruction or inside data. Requiring Capstone to decode it, at that
    exact boundary, from exactly that offset, removes the accidental matches
    that a two-byte pattern finds in any large binary.
    """
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    md.detail = True      # operands are only populated with detail on
    ins = next(md.disasm(image[offset:offset + 16], base + offset), None)
    if ins is None or ins.mnemonic not in ('jmp', 'call') or len(ins.operands) < 1:
        return False
    op = ins.operands[0]
    if op.type != X.X86_OP_MEM:
        return False
    m = op.mem
    return m.base == 0 and m.index != 0 and m.scale == 4


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('file')
    ap.add_argument('--seed', default='', help='entries, hex, comma-separated')
    ap.add_argument('--raw', action='store_true', help='the file is a flat image')
    ap.add_argument('--base', default='0', help='with --raw, hex')
    ap.add_argument('--verbose', action='store_true')
    a = ap.parse_args(argv)

    if capstone is None:
        sys.exit('this needs capstone')

    data = open(a.file, 'rb').read()
    seeds = [int(x, 16) for x in a.seed.split(',') if x]
    if a.raw:
        image, base, entry = data, int(a.base, 16), (seeds[0] if seeds else None)
        if entry is None:
            sys.exit('--raw needs --seed')
        seeds = seeds[1:]
    else:
        e = expmod.Exp(data)
        image, base, entry = e.image, e.base_offset, e.eip
        if entry is None:
            if not seeds:
                sys.exit('this is an MP, which has no entry point; give one with --seed')
            entry, seeds = seeds[0], seeds[1:]

    # 1. the population, found by bytes and confirmed by decoding
    raw_offsets = encoded_sites(image)
    real = {base + o for o in raw_offsets if confirmed_by_capstone(image, base, o)}

    # 2. what the tool under test reports
    m = x86map.Map(image, base, entry, seeds, None)
    claimed = {s for s, tbl, n, bounded in m.tables}

    hit = sorted(claimed & real)
    miss = sorted(claimed - real)
    unfound = sorted(real - claimed)

    print('%s: %d bytes of encoding FF 24 <sib> <disp32> scale=4, %d of them decode as that jmp'
          % (a.file, len(raw_offsets), len(real)))
    print('x86map reported %d table sites' % len(claimed))
    print('  site is a real jmp-through-table : %d' % len(hit))
    print('  site is NOT (false positive)     : %d' % len(miss))
    print('  real jmp sites it did not report : %d  (not necessarily tables)' % len(unfound))

    if hit:
        print('\nmatched, with the base and count the tool chose:')
        for s, tbl, n, bounded in sorted(m.tables):
            if s in real:
                print('  %#x -> table %#x, %d entries, %s'
                      % (s, tbl, n, 'bounded by a cmp' if bounded else 'read to a stop'))

    if miss and a.verbose:
        print('\nfalse positives:')
        md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
        for s in miss:
            ins = next(md.disasm(image[s - base:s - base + 8], s), None)
            print('  %#x decodes as %s' % (s, (ins.mnemonic + ' ' + ins.op_str) if ins else 'nothing'))

    # A detector that has never been seen to fail is a comment, so the gate is
    # "no false positives", not "some hits". Reporting nothing at all and calling
    # that clean would hide the failure this exists to find.
    if miss:
        print('\n%d of %d reported sites are not jmp-through-table (%.1f%%)'
              % (len(miss), len(claimed), 100.0 * len(miss) / max(1, len(claimed))))
        return 1
    if not claimed:
        print('\nthe tool reported no tables at all: nothing was tested')
        return 2
    print('\nno false positives')
    return 0


if __name__ == '__main__':
    sys.exit(main())
