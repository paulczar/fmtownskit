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
    def __init__(self, image, base, entry, seeds=()):
        if capstone is None:
            raise SystemExit('x86map needs capstone')
        self.img = image
        self.base = base
        self.md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
        self.md.detail = True
        self.insn = {}                     # addr -> capstone insn
        self.functions = {entry} | set(seeds)
        self.targets = set()               # branch targets: where constants are forgotten
        self.indirect = []
        self.bad = []
        self._descend([entry] + list(seeds))
        self.sites = self._sites()

    def _at(self, a):
        off = a - self.base
        if not 0 <= off < len(self.img):
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
                if m == 'call':
                    if imm is None:
                        self.indirect.append(pc)
                    elif imm not in self.functions:
                        self.functions.add(imm)
                        work.append(imm)
                elif m.startswith('j') or m.startswith('loop') or m in ('jecxz', 'jcxz'):
                    if imm is None:
                        self.indirect.append(pc)
                    else:
                        self.targets.add(imm)
                        work.append(imm)
                    if m == 'jmp':
                        break
                elif m in ('ret', 'retf', 'iret', 'iretd', 'hlt', 'ljmp'):
                    break
                pc += ins.size

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
                    and ins.reg_name(ops[0].mem.segment) in SEG and ops[0].mem.base == 0:
                sr = ins.reg_name(ops[0].mem.segment)
                out.append((a, 'far', ops[0].mem.disp, sr, c.get(sr), c.get('ah')))
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
    if kind == 'far':
        return 'call far %s:[%#x], %s=%s AH=%s  %s' % (x, n, x, h(y, 4), h(z),
                                                     towns.SELECTORS.get(y, '') if y is not None else '')
    if kind == 'sel':
        return '%s <- %04Xh  %s' % (x, n, towns.SELECTORS.get(n, ''))
    return str(s)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('file', help='a P3 .EXP')
    ap.add_argument('--seed', default='', help='more entries, hex, comma-separated')
    ap.add_argument('--dis', metavar='ADDR')
    ap.add_argument('-n', type=int, default=30)
    ap.add_argument('--functions', action='store_true')
    a = ap.parse_args(argv)
    e = expmod.Exp(open(a.file, 'rb').read())
    seeds = [int(x, 16) for x in a.seed.split(',') if x]
    if a.dis:
        md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
        addr = int(a.dis, 16)
        for i, ins in enumerate(md.disasm(e.image[addr - e.base_offset:], addr)):
            if i >= a.n:
                break
            print('  %06x  %-22s %s %s' % (ins.address, ins.bytes.hex(), ins.mnemonic, ins.op_str))
        return
    m = Map(e.image, e.base_offset, e.eip, seeds)
    ext = m.extents()
    code = sum(i.size for i in m.insn.values())
    print('%d functions, %d instructions, %d bytes of code in %d extents (%#x-%#x), image %d bytes'
          % (len(m.functions), len(m.insn), code, len(ext), ext[0][0], ext[-1][1], len(e.image)))
    if m.bad:
        print('undecodable at', ' '.join('%#x' % b for b in m.bad))
    if a.functions:
        print(' '.join('%x' % f for f in sorted(m.functions)))
    kinds = collections.Counter()
    for s in m.sites:
        kinds[s[1]] += 1
        print('  %06x  %s' % (s[0], name_site(s)))
    print('indirect transfers (not followed): %d' % len(m.indirect))
    for p in m.indirect:
        ins = m.insn[p]
        print('  %06x  %s %s' % (p, ins.mnemonic, ins.op_str))
    print(', '.join('%s %d' % kv for kv in sorted(kinds.items())))


if __name__ == '__main__':
    main()
