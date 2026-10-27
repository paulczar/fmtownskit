"""32-bit x86 code of a Towns program: where it is, and what of the machine it touches.

A recursive descent from the entry (and any seeds) over a flat image, as a
.EXP's is loaded at its base: function starts are call targets, code ends
at unconditional transfers. Indirect transfers are listed, not followed.
Inside each straight run of code a small constant propagation follows the
registers a program sets before it reaches the machine -- AH/AL before an
`int`, DX before `in`/`out`, a selector pushed and popped into a segment
register -- so that every site comes out named with the tables of `towns`:

    int 21h AH=3Dh (open file)        int 93h AX=52C0h (disk BIOS, CD-ROM)
    out 0x0440 (CRTC register select) call far fs:[0x60], fs=0x0110
    es <- 0x0130                      (the selectors a program uses)

This is a map, not an analysis: a value set in another block is "?".
Needs capstone.

    python -m fmtownskit.x86map DUEL.EXP                    # sites, extents, functions
    python -m fmtownskit.x86map DUEL.EXP --seed 10489,104e8 # more entries
    python -m fmtownskit.x86map DUEL.EXP --dis a7f0 -n 40   # disassembly
    python -m fmtownskit.x86map 4DDFMT.EXP --symbol-seeds 3a650  # its symbols below 0x3A650 too

A program with symbols (`exp`) has each site named by the symbol it lies in,
and its tables read with no bound stop at the next symbol.
"""
import argparse
import collections
import sys

try:
    import capstone
    from capstone import x86_const as X
except ImportError:      # pragma: no cover
    capstone = None

from . import exp as expmod
from . import towns

SEG = {'es', 'cs', 'ss', 'ds', 'fs', 'gs'}
SUB = {'al': ('eax', 0, 0xFF), 'ah': ('eax', 8, 0xFF), 'ax': ('eax', 0, 0xFFFF), 'eax': ('eax', 0, 0xFFFFFFFF),
       'bl': ('ebx', 0, 0xFF), 'bh': ('ebx', 8, 0xFF), 'bx': ('ebx', 0, 0xFFFF), 'ebx': ('ebx', 0, 0xFFFFFFFF),
       'cl': ('ecx', 0, 0xFF), 'ch': ('ecx', 8, 0xFF), 'cx': ('ecx', 0, 0xFFFF), 'ecx': ('ecx', 0, 0xFFFFFFFF),
       'dl': ('edx', 0, 0xFF), 'dh': ('edx', 8, 0xFF), 'dx': ('edx', 0, 0xFFFF), 'edx': ('edx', 0, 0xFFFFFFFF),
       'di': ('edi', 0, 0xFFFF), 'edi': ('edi', 0, 0xFFFFFFFF), 'si': ('esi', 0, 0xFFFF), 'esi': ('esi', 0, 0xFFFFFFFF)}


class Consts:
    """Known register values in a straight run: full registers as (value, known mask)."""

    def __init__(self):
        self.r = {}
        self.stack = []

    def get(self, name):
        if name in SEG:
            return self.r.get(name)
        full, sh, m = SUB[name]
        v = self.r.get(full)
        if v is None or (v[1] >> sh) & m != m:
            return None
        return (v[0] >> sh) & m

    def set(self, name, val):
        if name in SEG:
            self.r[name] = val
            return
        full, sh, m = SUB[name]
        if val is None:
            v = self.r.get(full)
            if v is not None:
                self.r[full] = (v[0], v[1] & ~(m << sh))
            return
        v = self.r.get(full, (0, 0))
        self.r[full] = ((v[0] & ~(m << sh)) | ((val & m) << sh), v[1] | (m << sh))

    def clear(self):
        self.r.clear()
        self.stack.clear()


class Map:
    def __init__(self, image, base, entry, seeds=(), table_end=None, no_code=()):
        """table_end: a function giving where a table read with no bound must stop (a
        program's symbols: `exp.Exp.table_end`), or None.

        no_code: half-open [lo, hi) ranges where code cannot be. A target there is listed
        as a bad transfer rather than decoded; see `recomp.discover.Program` for why zeros
        make an unguarded descent succeed silently."""
        if capstone is None:
            raise SystemExit('x86map needs capstone')
        self.img = image
        self.base = base
        self.no_code = sorted((int(lo), int(hi)) for lo, hi in no_code)
        self.md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
        self.md.detail = True
        self.insn = {}                     # addr -> capstone insn
        self.functions = {entry} | set(seeds)
        self.targets = set()               # branch targets: where constants are forgotten
        self.indirect = []
        self.tables = []                   # (site, table address, entries, bounded)
        self.bad = []
        self.table_end = table_end
        self._descend([entry] + list(seeds))
        self.sites = self._sites()

    def _at(self, a):
        off = a - self.base
        if not 0 <= off < len(self.img):
            return None
        for lo, hi in self.no_code:
            if lo <= a < hi:
                return None
        return next(self.md.disasm(self.img[off:off + 16], a), None)

    def _descend(self, work):
        work = list(work)
        while work:
            pc = work.pop()
            while pc not in self.insn:
                ins = self._at(pc)
                if ins is None:
                    self.bad.append(pc)
                    break
                self.insn[pc] = ins
                m = ins.mnemonic
                ops = ins.operands
                imm = ops[0].imm if ops and ops[0].type == X.X86_OP_IMM else None
                table = self._table(pc, ins) if imm is None and m in ('call', 'jmp') else None
                if table is not None:
                    for t in table:
                        if t == 0:
                            continue
                        if m == 'call' and t not in self.functions:
                            self.functions.add(t)
                        elif m == 'jmp':
                            self.targets.add(t)
                        work.append(t)
                if m == 'call':
                    if imm is None:
                        if table is None:
                            self.indirect.append(pc)
                    elif imm not in self.functions:
                        self.functions.add(imm)
                        work.append(imm)
                elif m.startswith('j') or m.startswith('loop') or m in ('jecxz', 'jcxz'):
                    if imm is None:
                        if table is None:
                            self.indirect.append(pc)
                    else:
                        self.targets.add(imm)
                        work.append(imm)
                    if m == 'jmp':
                        break
                elif m in ('ret', 'retf', 'iret', 'iretd', 'hlt', 'ljmp'):
                    break
                pc += ins.size

    def _table(self, pc, ins):
        """A transfer through `[index*4 + table]`: the table's entries, or None.

        The bound is the `cmp index8/index, N` and `ja` just before (N+1
        entries). With none, entries are read while each is 0 (a hole) or an
        address inside the image that decodes, up to 256 -- and, when the
        program has symbols, not past the next symbol (`table_end`)."""
        if not ins.operands or ins.operands[0].type != X.X86_OP_MEM:
            return None
        mem = ins.operands[0].mem
        if mem.base != 0 or mem.index == 0 or mem.scale != 4:
            return None
        tbl = mem.disp & 0xFFFFFFFF
        if not 0 <= tbl - self.base < len(self.img):
            return None
        idx = ins.reg_name(mem.index)
        low = {'eax': ('al', 'ah', 'ax'), 'ebx': ('bl', 'bh', 'bx'), 'ecx': ('cl', 'ch', 'cx'),
               'edx': ('dl', 'dh', 'dx'), 'esi': ('si',), 'edi': ('di',), 'ebp': ('bp',)}
        # the index's source: `movzx idx, r8` just before names the register compared
        names = {idx} | set(low.get(idx, ()))
        bound = None
        back = [self.insn[a] for a in sorted(a for a in self.insn if pc - 48 <= a < pc)]
        for p in back:
            if p.mnemonic == 'movzx' and p.op_str.split(',')[0] == idx:
                names.add(p.op_str.split(', ')[1])
        def rewrites(p):
            # the index changed after its compare (`sub ebx, 0x80; shr ebx, 4`: ABEL.EXE's 0x474CC
            # compares AL with 0xF0, then indexes by (AL - 0x80) >> 4): the compare bounds no more
            if p.mnemonic == 'movzx' and p.op_str.split(',')[0] == idx or p.mnemonic == 'cwde':
                return False       # a widening keeps the value compared (`cmp ax, 0x4A; ja; cwde`)
            try:
                _, written = p.regs_access()
            except capstone.CsError:
                return True
            return any(p.reg_name(r) in names for r in written)
        for k, (p, q) in enumerate(zip(back, back[1:])):
            if p.mnemonic == 'cmp' and q.mnemonic in ('ja', 'jae', 'jg', 'jge') and p.op_str.split(', ')[0] in names:
                try:
                    n = int(p.op_str.split(', ')[1], 0)
                except ValueError:
                    continue
                bound = None if any(rewrites(r) for r in back[k + 2:]) else \
                    (n + 1 if q.mnemonic in ('ja', 'jg') else n)
        out = []
        end = self.table_end(tbl) if self.table_end and bound is None else None
        for k in range(bound if bound is not None else 256):
            off = tbl - self.base + 4 * k
            if off + 4 > len(self.img) or (end is not None and tbl + 4 * k >= end):
                break
            v = int.from_bytes(self.img[off:off + 4], 'little')
            if bound is None and v != 0 and (not 0 <= v - self.base < len(self.img) or self._at(v) is None):
                break
            out.append(v)
        self.tables.append((pc, tbl, len(out), bound is not None))
        return out

    def _sites(self):
        out = []
        c = Consts()
        prev_end = None
        for a in sorted(self.insn):
            ins = self.insn[a]
            if a != prev_end or a in self.targets or a in self.functions:
                c.clear()
            prev_end = a + ins.size
            m, ops = ins.mnemonic, ins.operands
            reg = lambda o: ins.reg_name(o.reg)
            if m == 'int':
                n = ops[0].imm
                out.append((a, 'int', n, c.get('ah'), c.get('al'), None))
                c.clear()
            elif m in ('in', 'out'):
                po = ops[1] if m == 'in' else ops[0]
                port = po.imm if po.type == X.X86_OP_IMM else c.get('dx')
                out.append((a, m, port, None, None, None))
                if m == 'in':
                    c.set(reg(ops[0]), None)
            elif m in ('call', 'lcall', 'jmp', 'ljmp') and ops and ops[0].type == X.X86_OP_MEM \
                    and ins.opcode[0] == 0xFF and (ins.modrm >> 3) & 7 in (3, 5) \
                    and ins.reg_name(ops[0].mem.segment) in SEG and ops[0].mem.base == 0:
                # a far transfer through a pointer (FF /3, FF /5: capstone's operand size is not
                # reliable for them); a near `jmp cs:[reg*4 + table]` (FF /4, MetaWare's switch
                # with its table placed after it) is not one
                sr = ins.reg_name(ops[0].mem.segment)
                out.append((a, 'far' if m in ('call', 'lcall') else 'farjmp', ops[0].mem.disp, sr,
                            c.get(sr), c.get('ah')))
                c.clear()
            elif m == 'mov' and len(ops) == 2 and ops[0].type == X.X86_OP_REG:
                dst = reg(ops[0])
                if ops[1].type == X.X86_OP_IMM and (dst in SUB or dst in SEG):
                    c.set(dst, ops[1].imm)
                elif ops[1].type == X.X86_OP_REG and dst in SEG:
                    src = reg(ops[1])
                    if src in SUB and src.startswith('e'):
                        src = src[1:]          # a selector is the low 16 bits
                    v = c.get(src) if src in SUB or src in SEG else None
                    c.set(dst, v if v is None else v & 0xFFFF)
                    if v is not None:
                        out.append((a, 'sel', v & 0xFFFF, dst, None, None))
                elif dst in SUB:
                    c.set(dst, None)
            elif m == 'push' and ops and ops[0].type == X.X86_OP_IMM:
                c.stack.append(ops[0].imm)
            elif m == 'push':
                src = reg(ops[0]) if ops and ops[0].type == X.X86_OP_REG else None
                c.stack.append(c.get(src) if src in SEG or src in SUB else None)
            elif m == 'pop' and ops and ops[0].type == X.X86_OP_REG:
                dst = reg(ops[0])
                v = c.stack.pop() if c.stack else None
                if dst in SEG or dst in SUB:
                    c.set(dst, v)
                if dst in SEG and v is not None:
                    out.append((a, 'sel', v & 0xFFFF, dst, None, None))
            elif m in ('add', 'sub', 'inc', 'dec') and ops and ops[0].type == X.X86_OP_REG \
                    and reg(ops[0]) in SUB:
                dst = reg(ops[0])
                v = c.get(dst)
                if m in ('inc', 'dec'):
                    k = 1
                elif len(ops) == 2 and ops[1].type == X.X86_OP_IMM:
                    k = ops[1].imm
                else:
                    v = None
                if v is not None:
                    v = v + k if m in ('add', 'inc') else v - k
                c.set(dst, v)
            elif m in ('call', 'pushal', 'popal', 'pushaw', 'popaw'):
                c.clear()
            else:
                # anything else that writes a register forgets it
                try:
                    _, written = ins.regs_access()
                except capstone.CsError:
                    written = []
                for r in written:
                    n = ins.reg_name(r)
                    if n in SUB or n in SEG:
                        c.set(n, None)
        return out

    def refs_at(self, site):
        """The absolute-address references made by the single instruction at `site`.

        The same classification as `refs` for one instruction, which is what a caller wants
        when walking the whole map once rather than searching for one address. `refs` asks
        "who names this address"; this asks "what does this instruction name", and a sweep
        needs the second, because calling `refs` per extent means passing a window and a
        window wider than the gap to the next object reports that object too.

        THAT IS NOT HYPOTHETICAL. Sweeping one extent of 3,984 bytes with the width as its
        window produced 1,048 references, and on inspection they were the player record at
        0xceca0 and 0xcdf6b to 0xcdf6f, which lie ABOVE the extent. The window had reached
        past it. Nothing was wrong with the data; the question was asked wrongly.
        """
        ins = self.insn.get(site)
        if ins is None or not ins.operands:
            return []
        m, ops = ins.mnemonic, ins.operands
        out = []
        for i, op in enumerate(ops):
            if op.type == X.X86_OP_MEM:
                d = op.mem.disp & 0xFFFFFFFF
                # an absolute address has no base; a record offset such as [edx + 0xcdf6b]
                # is still reported, with the base named, because that is how this program
                # reaches its globals
                indexed = bool(op.mem.index)
                based = ins.reg_name(op.mem.base) if op.mem.base else None
                dst = i == 0 and m in ('mov', 'movzx', 'movsx', 'add', 'sub', 'or', 'and',
                                        'xor', 'inc', 'dec', 'shl', 'shr', 'sar')
                if indexed:
                    form = 'indexed-%s' % ('write' if dst else 'read')
                elif based and m.startswith('lea'):
                    form = 'address+base'
                elif based:
                    form = 'scalar-%s' % ('write' if dst else 'read')
                elif m.startswith('lea'):
                    form = 'address'
                else:
                    form = 'scalar-%s' % ('write' if dst else 'read')
                out.append({'site': site, 'op': m, 'text': ins.op_str, 'form': form, 'disp': d,
                            'base': based,
                            'index': ins.reg_name(op.mem.index) if op.mem.index else None,
                            'scale': op.mem.scale if op.mem.index else None})
            elif op.type == X.X86_OP_IMM and m in ('push', 'mov', 'cmp', 'add', 'sub', 'or',
                                                    'and', 'xor', 'test'):
                v = op.imm & 0xFFFFFFFF
                form = 'argument' if m == 'push' else 'immediate-%s' % m
                out.append({'site': site, 'op': m, 'text': ins.op_str, 'form': form, 'disp': v,
                            'base': None, 'index': None, 'scale': None})
        return out

    def refs(self, addr, window=0):
        """Every reached instruction naming `addr`, with the form of the reference.

        A search over the whole map, built from `refs_at` so the two cannot classify a
        reference differently. Use `refs_at` for a sweep.

        WHY FORM AND NOT JUST PRESENCE. Whether an address is touched at all is weak; how it
        is touched is most of what a caller wanted to know. The same displacement means
        different things in each of these, and the operand structure tells them apart without
        knowing anything about the program:

            lea edx, [eax*4 + 0xd03e7]   an array of dwords, indexed, stride from the scale
            lea eax, [0xd03e7]           the address of a whole object, usually passed on
            push 0xd03e7                 the same, as an argument, order less certain
            mov [0xd03e7], al            a byte is written there: a scalar field, not a table
            mov eax, [0xd03e7]           a word or dword is read: a scalar or a first element
            cmp dword [0xd03e7], 0       read as one value, and compared

        A displacement that lands in the MIDDLE of an object is indistinguishable here from
        the object's start, so `window` widens the test to `addr - window <= disp < addr`. With
        window=0 only an exact match is reported.

        BASES ARE REPORTED, NOT DROPPED, and that distinction cost a blind spot. The first
        version skipped every memory operand with a base register on the reasoning that
        `[ebp-4]` is a frame slot rather than an absolute address. True, and irrelevant here:
        this program reaches its globals as `[edx + 0xcdf6b]`, where edx holds a record base
        the code computed. Dropping those reported 4 references to PLAYER_X where a
        displacement scan of the same image finds 68 sites and 12 writes. So a base is kept
        and named, and the caller can tell the two cases apart because a frame slot has a
        small displacement and a global has a large one.
        """
        out = []
        for a in sorted(self.insn):
            for r in self.refs_at(a):
                if addr - window <= r['disp'] < addr + max(1, window):
                    out.append(r)
        return out

    def extents(self, slack=8):
        ext = []
        for a in sorted(self.insn):
            e = a + self.insn[a].size
            if ext and a <= ext[-1][1] + slack:
                ext[-1][1] = max(ext[-1][1], e)
            else:
                ext.append([a, e])
        return ext


def name_site(s):
    a, kind, n, x, y, z = s
    h = lambda v, w=2: '?' if v is None else '%0*Xh' % (w, v)
    if kind == 'int':
        what = towns.INTS.get(n, '')
        if n == 0x21 and x is not None:
            what = towns.DOS.get(x, 'DOS %02Xh' % x)
            if x == 0x25 and y is not None:
                what = 'Phar Lap: ' + towns.PHARLAP.get(y, '?')
        return 'int %02Xh AH=%s AL=%s  %s' % (n, h(x), h(y), what)
    if kind in ('in', 'out'):
        return '%-3s %s  %s' % (kind, '?' if n is None else '0x%04x' % n,
                                '' if n is None else towns.port_name(n))
    if kind in ('far', 'farjmp'):
        return '%s far %s:[%#x], %s=%s AH=%s  %s' % ('call' if kind == 'far' else 'jmp', x, n, x, h(y, 4), h(z),
                                                   towns.SELECTORS.get(y, '') if y is not None else '')
    if kind == 'sel':
        return '%s <- %04Xh  %s' % (x, n, towns.SELECTORS.get(n, ''))
    return str(s)


def symbol_at(e):
    """A function naming the symbol an address lies in, as '[name+off] '."""
    import bisect
    names = e.names()
    starts = sorted(names)

    def where(a):
        k = bisect.bisect_right(starts, a) - 1
        if k < 0:
            return ''
        d = a - starts[k]
        return '[%s%s] ' % (names[starts[k]].split(',')[0], '+%x' % d if d else '')
    return where


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('file', help='a P3 .EXP, or with --raw any flat image')
    ap.add_argument('--raw', action='store_true', help='the file is a flat image (--base, entries from --seed)')
    ap.add_argument('--base', default='0', help='with --raw: the address of its first byte, hex')
    ap.add_argument('--far-table', metavar='FROM:TO',
                    help='with --raw: seed every far pointer (offset, selector) stored in FROM-TO, hex')
    ap.add_argument('--seed', default='', help='more entries, hex, comma-separated')
    ap.add_argument('--symbol-seeds', metavar='END',
                    help="a program with symbols: every symbol below END (hex: where its code ends) "
                         "is an entry too")
    ap.add_argument('--dis', metavar='ADDR')
    ap.add_argument('-n', type=int, default=30)
    ap.add_argument('--no-code', action='append', default=[], metavar='LO:HI',
                    help='a half-open range where code cannot be, hex, repeatable; a target '
                         'there is listed rather than decoded. Zeros decode as instructions '
                         'without error, so without this a descent into zero fill succeeds '
                         'silently and endlessly')
    ap.add_argument('--functions', action='store_true')
    ap.add_argument('--refs', metavar='ADDR',
                    help='every reached instruction naming this address, with the form of the '
                         'reference: indexed (an array), address, scalar read or write, argument')
    ap.add_argument('--refs-window', type=lambda s: int(s, 0), default=0, metavar='N',
                    help='with --refs: also count displacements in ADDR-N..ADDR-1, for a '
                         'displacement that lands inside an object')
    ap.add_argument('--mnemonics', action='store_true', help='how often each instruction occurs')
    a = ap.parse_args(argv)
    data = open(a.file, 'rb').read()
    seeds = [int(x, 16) for x in a.seed.split(',') if x]
    e = None
    if a.raw:
        image, base = data, int(a.base, 16)
        if a.far_table:
            lo, hi = (int(x, 16) for x in a.far_table.split(':'))
            for o in range(lo, hi, 8):
                off = int.from_bytes(image[o:o + 4], 'little')
                if off:
                    seeds.append(off)
        if not seeds:
            sys.exit('--raw needs entries (--seed or --far-table)')
        entry, seeds = seeds[0], seeds[1:]
    else:
        e = expmod.Exp(data)
        image, base, entry = e.image, e.base_offset, e.eip
        # An MP has no entry point (its 0x18 field is the header size), so the first
        # seed is the entry, as in i386.py.
        if entry is None:
            if not seeds:
                sys.exit('%s: this is an MP, which has no entry point; '
                         'give one with --seed' % a.file)
            entry, seeds = seeds[0], seeds[1:]
        if a.symbol_seeds:
            end = int(a.symbol_seeds, 16)
            seeds += [off for off, _, _ in e.symbols if off < end and off != entry]
    if a.dis:
        md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
        addr = int(a.dis, 16)
        for i, ins in enumerate(md.disasm(image[addr - base:], addr)):
            if i >= a.n:
                break
            print('  %06x  %-22s %s %s' % (ins.address, ins.bytes.hex(), ins.mnemonic, ins.op_str))
        return
    m = Map(image, base, entry, seeds, e.table_end if e is not None and e.symbols else None,
            no_code=expmod.parse_no_code(a.no_code))
    if a.refs is not None:
        target = int(a.refs, 16)
        rows = m.refs(target, a.refs_window)
        if not rows:
            print('no reached instruction names %#x%s' % (target,
                  '' if not a.refs_window else ' within %#x below it' % a.refs_window))
            return
        print('%#x: %d reference%s from reached code' % (target, len(rows), '' if len(rows) == 1 else 's'))
        for r in rows:
            idx = '' if r['index'] is None else '  idx=%s*%d' % (r['index'], r['scale'])
            base = '' if r.get('base') is None else '  base=%s' % r['base']
            print('  %-22s %#08x  %-8s %s%s%s'
                  % (r['form'], r['site'], r['op'], r['text'], idx, base))
        return
    where = (lambda a: '') if e is None or not e.symbols else symbol_at(e)
    ext = m.extents()
    code = sum(i.size for i in m.insn.values())
    print('%d functions, %d instructions, %d bytes of code in %d extents (%#x-%#x), image %d bytes'
          % (len(m.functions), len(m.insn), code, len(ext), ext[0][0], ext[-1][1], len(image)))
    for site, tbl, n, bounded in m.tables:
        print('table at %#x for %#x: %d entries (%s)' % (tbl, site, n, 'bounded by a cmp' if bounded else 'read'))
    if a.mnemonics:
        hist = collections.Counter(i.mnemonic for i in m.insn.values())
        print(' '.join('%s:%d' % kv for kv in sorted(hist.items(), key=lambda kv: -kv[1])))
        return
    if m.bad:
        print('undecodable at', ' '.join('%#x' % b for b in m.bad))
    if a.functions:
        print(' '.join('%x' % f for f in sorted(m.functions)))
    kinds = collections.Counter()
    for s in m.sites:
        kinds[s[1]] += 1
        print('  %06x  %s%s' % (s[0], where(s[0]), name_site(s)))
    print('indirect transfers (not followed): %d' % len(m.indirect))
    for p in m.indirect:
        ins = m.insn[p]
        print('  %06x  %s%s %s' % (p, where(p), ins.mnemonic, ins.op_str))
    print(', '.join('%s %d' % kv for kv in sorted(kinds.items())))


if __name__ == '__main__':
    main()
