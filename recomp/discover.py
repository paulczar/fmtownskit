"""Discovery: a program's functions and the code of each, for the recompiler.

A recursive descent with the kit's own decoder (`i386`) over a flat image,
from the entries a port names: the program's entry, its interrupt handlers,
function pointers it stores, a far-pointer table it exports. Function
starts are call targets; jump tables (`jmp/call [index*4 + table]`) are
read with the bound their `cmp`/`ja` gives, or while their entries hold
(each 0 -- a hole -- or an address in the image that decodes; with a
program's symbols, not past the next one); everything else indirect is
listed for the runtime to meet.

A function's body is what its entry reaches by falling through and jumping,
never by calling; bodies may share code (a jump into another function's
middle copies that code into both). Two instructions that overlap -- one
starting inside another -- are a conflict: the descent has gone into data
somewhere, and the report names both.

    python -m fmtownskit.recomp.discover GAME.EXP --seed 103e2 --report
    python -m fmtownskit.recomp.discover TBIOS.BIN --raw --far-table 20:100 --report
    python -m fmtownskit.recomp.discover 4DDFMT.EXP --symbol-seeds 3a650 --report
"""
import argparse
import collections
import sys

from .. import i386
from ..i386 import Refused, decode

ENDS = ('ret', 'retf', 'iret', 'iretd', 'hlt', 'ljmp')


class Program:
    def __init__(self, image, base=0, entries=(), name='prog', far_table=None, table_end=None, names=None):
        self.img, self.base, self.name = image, base, name
        self.names = names or {}       # offset -> a program's symbols there (for the C++'s comments)
        self.table_end = table_end     # a program's symbols: where a read table must stop
        self.insn = {}                 # addr -> Insn
        self.functions = set()
        self.targets = set()           # jump targets
        self.tables = {}               # site -> (table address, [targets], bounded)
        self.indirect = []             # sites whose targets are unknown
        self.refused = {}              # addr -> reason (reached, not an instruction)
        seeds = list(entries)
        if far_table:
            lo, hi = far_table
            for o in range(lo, hi, 8):
                v = int.from_bytes(image[o:o + 4], 'little')
                if v:
                    seeds.append(v)
        self.entries = list(dict.fromkeys(seeds))
        self.cut = {}                  # table site -> entries kept (a read table cut short)
        self.pushed = {}               # jmp site -> the return address a `push` before it gave
        for _ in range(64):
            self._reset()
            self._descend([(e, None) for e in self.entries])
            if not self._prune() and not self._pushcalls():
                break

    def _pushcalls(self):
        """Calls made by hand, `push RET; jmp F` (ABEL's TBIOS, the graphic module: `push 0x41B4;
        jmp 0x5FA4`, F's `ret` coming back to 0x41B4): F is a function, RET the code that goes on.
        RET must be code already -- an instruction discovery has, or the start of code right after
        a ret, retf or jmp (nops between) -- and not below 0x100: a compiler's `push N; jmp` to
        where its call happens pushes an argument (in a program at base 0, a number like any
        address). True if new ones were found."""
        new = False
        for a, i in list(self.insn.items()):
            if i.op != 'push' or not i.ops or i.ops[0][0] != 'i' or i.osize != 4:
                continue
            j = self.insn.get(a + i.size)
            if j is None or j.op != 'jmp' or j.ops[0][0] != 'j' or (a + i.size) in self.pushed:
                continue
            v = i.ops[0][1] & 0xFFFFFFFF
            if v < 0x100 or not 0 <= v - self.base < len(self.img):
                continue
            k = v
            while k > self.base and self.img[k - 1 - self.base] == 0x90:
                k -= 1
            # (not the pair's own jmp: `push 4; push 0x7708; jmp 0x7604` passes a table placed after it)
            after_end = any(b in self.insn and b + self.insn[b].size == k and b != a + i.size and
                            self.insn[b].op in ('ret', 'retf', 'jmp', 'iretd') for b in range(k - 5, k))
            if v in self.insn or after_end:
                self.pushed[a + i.size] = v
                new = True
        return new

    def _reset(self):
        self.insn, self.origin = {}, {}
        self.functions = set(self.entries)
        self.targets, self.tables, self.indirect, self.refused = set(), {}, [], {}

    def _prune(self):
        """Cut each read table short at the first entry whose code is not clean (a conflict,
        a refusal): such an entry was data. True if anything was cut."""
        bad = {}
        def blame(a):
            o = self.origin.get(a)
            if o is not None and not self.tables[o[0]][2]:
                site, k = o
                bad[site] = min(bad.get(site, 1 << 30), k)
        for a, b in self.conflicts():
            oa, ob = self.origin.get(a), self.origin.get(b)
            # the later-read entry is the suspect; code from a function's own descent is not
            if ob is not None and (oa is None or ob[1] >= oa[1]):
                blame(b)
            elif oa is not None:
                blame(a)
        for a in self.refused:
            blame(a)
        changed = False
        for site, k in bad.items():
            if self.cut.get(site, 1 << 30) > k:
                self.cut[site] = k
                changed = True
        return changed

    def at(self, a):
        off = a - self.base
        if not 0 <= off < len(self.img):
            raise Refused('outside the image')
        return decode(self.img, off, a)

    def _descend(self, work):
        """work: (address, origin); origin is (table site, entry index) for code first reached
        through a table entry, inherited by everything that code reaches."""
        work = list(work)
        while work:
            pc, org = work.pop()
            while pc not in self.insn and pc not in self.refused:
                try:
                    i = self.at(pc)
                except Refused as e:
                    self.refused[pc] = str(e)
                    self.origin[pc] = org
                    break
                self.insn[pc] = i
                self.origin[pc] = org
                op, o0 = i.op, (i.ops[0] if i.ops else None)
                nxt = pc + i.size
                if op in ('call', 'jmp') and o0[0] == 'm':
                    t = self._table(pc, i)
                    if t is None:
                        self.indirect.append(pc)
                    else:
                        for k, x in enumerate(t):
                            if not x:
                                continue
                            (self.functions if op == 'call' else self.targets).add(x)
                            read = not self.tables[pc][2]
                            work.append((x, (pc, k) if read else org))
                elif op in ('call', 'jmp') and o0[0] == 'r':
                    self.indirect.append(pc)
                elif op == 'jmp' and pc in self.pushed:
                    # a call made by hand: the jump's target a function, the pushed address goes on
                    if o0[1] not in self.functions:
                        self.functions.add(o0[1])
                        work.append((o0[1], org))
                    self.targets.add(self.pushed[pc])
                    work.append((self.pushed[pc], org))
                elif op == 'call':
                    if o0[1] not in self.functions:
                        self.functions.add(o0[1])
                        work.append((o0[1], org))
                elif o0 is not None and o0[0] == 'j':
                    self.targets.add(o0[1])
                    work.append((o0[1], org))
                if op == 'jmp' or op in ENDS or (op == 'lcall' and False):
                    break
                pc = nxt

    def _table(self, pc, i):
        o = i.ops[0]
        _, seg, base, index, scale, disp, _ = o
        if base is not None and index is None and i.op == 'jmp' and i.asize == 4:
            return self._placed(pc, i, disp, seg)
        if base is not None or index is None or scale != 4 or i.asize != 4:
            return None
        if not 0 <= disp - self.base < len(self.img):
            return None
        bound = self._bound(pc, index)
        limit = self.cut.get(pc, 256) if bound is None else bound
        end = self.table_end(disp) if self.table_end and bound is None else None
        out = []
        for k in range(limit):
            off = disp - self.base + 4 * k
            if off + 4 > len(self.img) or (end is not None and disp + 4 * k >= end):
                break
            v = int.from_bytes(self.img[off:off + 4], 'little')
            if bound is None and v:
                try:
                    self.at(v)
                except Refused:
                    break
            out.append(v)
        self.tables[pc] = (disp, out, bound is not None)
        return out

    def _placed(self, pc, i, disp, seg):
        """A table placed in the code after its jump, `jmp cs:[reg + table]`, the register an
        index already scaled (hand-written drawing code). It lies past the jump and inside the
        same symbol's code (with no symbols: addressed through CS), and ends where the first
        code it jumps to begins; read while its entries hold, as a table with no bound."""
        if not pc < disp or not 0 <= disp - self.base < len(self.img):
            return None
        end = self.table_end(pc) if self.table_end else None
        if end is None and seg != 1 or end is not None and disp >= end:
            return None
        out, low = [], end if end is not None else self.base + len(self.img)
        for k in range(self.cut.get(pc, 256)):
            a = disp + 4 * k
            if a + 4 > low:
                break
            v = int.from_bytes(self.img[a - self.base:a - self.base + 4], 'little')
            if v:
                try:
                    self.at(v)
                except Refused:
                    break
                if v > disp:
                    low = min(low, v)
            out.append(v)
        if not out:
            return None
        self.tables[pc] = (disp, out, False)
        return out

    def _bound(self, pc, index):
        """How many entries a table indexed by register `index` at `pc` can be asked for, from
        the straight run of code before it, or None. Walking back: the `cmp idx, N` and `ja`
        (or a signed `jg`) that guard it (with no write to idx in between), `and idx, N`, a `setcc` into its low
        byte; a `movzx idx, r8` moves the question to that register."""
        run, a = [], pc
        while True:            # the contiguous instructions ending just before pc
            prev = next((b for b in range(a - 1, max(a - 16, 0) - 1, -1)
                         if b in self.insn and b + self.insn[b].size == a), None)
            if prev is None or len(run) >= 24:
                break
            run.append(self.insn[prev])
            a = prev
        fam = index
        for j, q in enumerate(run):
            o0 = q.ops[0] if q.ops else None
            writes = o0 is not None and o0[0] == 'r' and (o0[1] if o0[2] != 1 else o0[1] & 3) == fam \
                and q.op not in ('cmp', 'test', 'push', 'bt') and not q.op.startswith('j')
            if q.op in ('ja', 'jae', 'jg', 'jge') and j + 1 < len(run):
                c = run[j + 1]
                if c.op == 'cmp' and c.ops[0][0] == 'r' and c.ops[1][0] == 'i' and \
                        (c.ops[0][1] if c.ops[0][2] != 1 else c.ops[0][1] & 3) == fam:
                    # a signed `jg` bounds the entries from 0 up as `ja` does (ABEL.EXE's 0xCB2D:
                    # `jle` past index 0 and below, `cmp esi, 0x14; jg`: 21 entries)
                    n = c.ops[1][1]
                    if q.op in ('ja', 'jg') and j > 0 and run[j - 1].op == 'je':
                        # N itself goes elsewhere on the same flags (ABEL's TBIOS, the sprite
                        # module's 0x15B20: `cmp al, 2; ja; je`, a table of 2 entries)
                        return n
                    return n + 1 if q.op in ('ja', 'jg') else n
            if not writes:
                continue
            if q.op.startswith('set'):
                return 2
            if q.op == 'and' and q.ops[1][0] == 'i':
                return (q.ops[1][1] & 0xFF) + 1 if q.ops[1][1] < 0x100 else None
            if q.op == 'movzx' and q.ops[1][0] == 'r':
                fam = q.ops[1][1] & 3 if q.ops[1][2] == 1 else q.ops[1][1]
                continue
            return None
        return None

    def body(self, f):
        """The addresses of f's code: from its entry by falling through and jumping."""
        seen, work = set(), [f]
        while work:
            pc = work.pop()
            while pc in self.insn and pc not in seen:
                seen.add(pc)
                i = self.insn[pc]
                o0 = i.ops[0] if i.ops else None
                if pc in self.tables and i.op == 'jmp':
                    work += [x for x in self.tables[pc][1] if x]
                    break
                if pc in self.pushed:               # a call made by hand: on at the pushed address
                    work.append(self.pushed[pc])
                    break
                if o0 is not None and o0[0] == 'j' and i.op != 'call':
                    work.append(o0[1])
                if i.op == 'jmp' or i.op in ENDS or i.op == 'iretd':
                    break
                pc += i.size
        return seen

    def conflicts(self):
        """Instructions that start inside another: (outer, inner)."""
        out = []
        starts = sorted(self.insn)
        for a in starts:
            end = a + self.insn[a].size
            for b in range(a + 1, end):
                if b in self.insn:
                    out.append((a, b))
        return out

    def report(self):
        code = sum(i.size for i in self.insn.values())
        lines = ['%s: %d functions, %d instructions, %d bytes of code; %d jump tables (%d bounded), '
                 '%d indirect transfers unresolved, %d refused, %d conflicts'
                 % (self.name, len(self.functions), len(self.insn), code, len(self.tables),
                    sum(1 for t in self.tables.values() if t[2]), len(self.indirect),
                    len(self.refused), len(self.conflicts()))]
        for a, why in sorted(self.refused.items()):
            lines.append('  refused at %#x: %s' % (a, why))
        for a, b in self.conflicts()[:20]:
            lines.append('  conflict: %#x (%s) overlaps %#x (%s)' % (a, self.insn[a].op, b, self.insn[b].op))
        for p in self.indirect:
            lines.append('  indirect at %#x: %s %s' % (p, self.insn[p].op, i386.format_ops(self.insn[p])))
        return '\n'.join(lines)


def load(path, raw=False, base=0, seeds=(), far_table=None, name=None, symbol_seeds=None):
    """symbol_seeds: with a program's symbols, every symbol below this address (where its
    code ends) is an entry too."""
    data = open(path, 'rb').read()
    if raw:
        return Program(data, base, seeds, name or path, far_table)
    from .. import exp
    e = exp.Exp(data)
    seeds = list(seeds)
    if symbol_seeds is not None:
        seeds += [off for off, _, _ in e.symbols if off < symbol_seeds and off != e.eip]
    return Program(e.image, e.base_offset, [e.eip] + seeds, name or path,
                   table_end=e.table_end if e.symbols else None, names=e.names())


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('file')
    ap.add_argument('--raw', action='store_true')
    ap.add_argument('--base', default='0')
    ap.add_argument('--far-table', metavar='FROM:TO')
    ap.add_argument('--seed', default='')
    ap.add_argument('--symbol-seeds', metavar='END', help='every symbol below END (hex) is an entry too')
    ap.add_argument('--report', action='store_true')
    ap.add_argument('--functions', action='store_true')
    a = ap.parse_args(argv)
    ft = tuple(int(x, 16) for x in a.far_table.split(':')) if a.far_table else None
    p = load(a.file, a.raw, int(a.base, 16), [int(x, 16) for x in a.seed.split(',') if x], ft,
             symbol_seeds=int(a.symbol_seeds, 16) if a.symbol_seeds else None)
    print(p.report())
    if a.functions:
        print(' '.join('%x' % f for f in sorted(p.functions)))


if __name__ == '__main__':
    main()
