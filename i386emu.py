"""A 386 interpreter: the reference the recompiled code is held against.

It runs `i386.Insn`s on a state -- eight general registers, EIP, EFLAGS,
six segment registers with their bases -- over a memory and an I/O space
the caller gives it. What the machine must answer (a segment register
loaded, `int n`, a port, `hlt`) goes through methods a runtime overrides;
by default segment bases are 0 (a flat test machine) and the rest stops.

The flags are the 386's where Intel defines them. Where it leaves them
undefined (AF after AND/OR/XOR, OF after a shift of more than one, SF/ZF/
AF/PF after MUL, everything after DIV, ...) the interpreter computes what a
386 is not known to do, and `UNDEFINED` says which; the check against
unicorn sets them aside, and code that reads them is the recompiler's to
refuse.

    python -m fmtownskit.i386emu --test                       # known answers
    python -m fmtownskit.i386emu --check GAME.EXP --states 8  # its instructions, against unicorn
    python -m fmtownskit.i386emu --check TBIOS.BIN --raw --far-table 20:100
    python -m fmtownskit.i386emu --sweep 20000                # random instructions, against unicorn
"""
import argparse
import collections
import random
import struct
import sys

from . import i386
from .i386 import Insn, Refused, decode

CF, PF, AF, ZF, SF, TF, IF, DF, OF = 0x1, 0x4, 0x10, 0x40, 0x80, 0x100, 0x200, 0x400, 0x800
ARITH = CF | PF | AF | ZF | SF | OF
MASK = {1: 0xFF, 2: 0xFFFF, 4: 0xFFFFFFFF}
SIGN = {1: 0x80, 2: 0x8000, 4: 0x80000000}
PARITY = [0] * 256
for _v in range(256):
    PARITY[_v] = PF if bin(_v).count('1') % 2 == 0 else 0


class Fault(Exception):
    """A CPU exception: #DE, #UD, ... (vector, text)."""

    def __init__(self, vector, text):
        Exception.__init__(self, '#%s: %s' % ({0: 'DE', 6: 'UD', 13: 'GP'}.get(vector, vector), text))
        self.vector = vector


class Stop(Exception):
    """The machine has to answer: an instruction the interpreter does not run on its own."""


class FlatMemory:
    """Bytes on demand: each 4 KB page filled the first time it is touched, by `fill`."""

    def __init__(self, fill=None):
        self.pages = {}
        self.fill = fill or (lambda page: bytes(4096))
        self.written = set()

    def _page(self, a):
        p = a >> 12
        pg = self.pages.get(p)
        if pg is None:
            pg = self.pages[p] = bytearray(self.fill(p))
        return pg

    def read(self, a, n):
        v = 0
        for k in range(n):
            b = (a + k) & 0xFFFFFFFF
            v |= self._page(b)[b & 0xFFF] << (8 * k)
        return v

    def write(self, a, n, v):
        for k in range(n):
            b = (a + k) & 0xFFFFFFFF
            self._page(b)[b & 0xFFF] = (v >> (8 * k)) & 0xFF
            self.written.add(b)

    def load(self, a, data):
        k = 0
        while k < len(data):
            b = (a + k) & 0xFFFFFFFF
            n = min(4096 - (b & 0xFFF), len(data) - k)
            self._page(b)[b & 0xFFF:(b & 0xFFF) + n] = data[k:k + n]
            k += n


# Flags each instruction leaves undefined (Intel's manual for the 386 and later).
UNDEFINED = {
    'and': AF, 'or': AF, 'xor': AF, 'test': AF,
    'mul': SF | ZF | AF | PF, 'imul': SF | ZF | AF | PF,
    'div': ARITH, 'idiv': ARITH,
    'bsf': CF | OF | SF | AF | PF, 'bsr': CF | OF | SF | AF | PF,
    'bt': OF | SF | AF | PF, 'bts': OF | SF | AF | PF, 'btr': OF | SF | AF | PF, 'btc': OF | SF | AF | PF,
    'daa': OF, 'das': OF, 'aaa': OF | SF | ZF | PF, 'aas': OF | SF | ZF | PF,
    'aam': OF | AF | CF, 'aad': OF | AF | CF,
}


class CPU:
    def __init__(self, mem=None):
        self.r = [0] * 8
        self.eip = 0
        self.flags = 0x2
        self.sreg = [0] * 6
        self.sbase = [0] * 6
        self.mem = mem or FlatMemory()
        self.cycles = 0

    # --- the machine's side: a runtime overrides these ----------------------
    def load_segment(self, n, sel):
        """A segment register loaded: its base from the descriptor (flat here)."""
        self.sreg[n] = sel & 0xFFFF
        self.sbase[n] = 0

    def interrupt(self, n, insn):
        raise Stop('int %02xh' % n)

    def port_in(self, port, size):
        raise Stop('in %04x' % port)

    def port_out(self, port, size, value):
        raise Stop('out %04x' % port)

    def system(self, insn):
        raise Stop(insn.op)

    # --- registers ---------------------------------------------------------
    def get_reg(self, n, size):
        if size == 4:
            return self.r[n]
        if size == 2:
            return self.r[n] & 0xFFFF
        return (self.r[n & 3] >> 8) & 0xFF if n >= 4 else self.r[n] & 0xFF

    def set_reg(self, n, size, v):
        if size == 4:
            self.r[n] = v & 0xFFFFFFFF
        elif size == 2:
            self.r[n] = (self.r[n] & 0xFFFF0000) | (v & 0xFFFF)
        elif n >= 4:
            self.r[n & 3] = (self.r[n & 3] & 0xFFFF00FF) | ((v & 0xFF) << 8)
        else:
            self.r[n] = (self.r[n] & 0xFFFFFF00) | (v & 0xFF)

    # --- memory operands -----------------------------------------------------
    def ea(self, i, o):
        """(segment number, offset) of a memory operand."""
        _, seg, base, index, scale, disp, _ = o
        if i.asize == 4:
            off = disp
            if base is not None:
                off += self.r[base]
            if index is not None:
                off += self.r[index] * scale
            off &= 0xFFFFFFFF
            dflt = 2 if base in (4, 5) else 3
        else:
            off = disp
            if base is not None:
                off += self.r[base] & 0xFFFF
            if index is not None:
                off += self.r[index] & 0xFFFF
            off &= 0xFFFF
            dflt = 2 if base == 5 else 3
        return (seg if seg is not None else dflt), off

    def lin(self, seg, off):
        return (self.sbase[seg] + off) & 0xFFFFFFFF

    def read(self, i, o, size=None):
        k = o[0]
        if k == 'r':
            return self.get_reg(o[1], size or o[2])
        if k == 'i':
            return o[1]
        if k == 's':
            return self.sreg[o[1]]
        seg, off = self.ea(i, o)
        return self.mem.read(self.lin(seg, off), size or o[6])

    def write(self, i, o, v, size=None):
        k = o[0]
        if k == 'r':
            self.set_reg(o[1], size or o[2], v)
            return
        seg, off = self.ea(i, o)
        n = size or o[6]
        self.mem.write(self.lin(seg, off), n, v & MASK[n])

    # --- the stack ---------------------------------------------------------
    def push(self, v, n):
        self.r[4] = (self.r[4] - n) & 0xFFFFFFFF
        self.mem.write(self.lin(2, self.r[4]), n, v & MASK[n])

    def pop(self, n):
        v = self.mem.read(self.lin(2, self.r[4]), n)
        self.r[4] = (self.r[4] + n) & 0xFFFFFFFF
        return v

    # --- flags ---------------------------------------------------------------
    def szp(self, res, n):
        f = PARITY[res & 0xFF]
        if res & MASK[n] == 0:
            f |= ZF
        if res & SIGN[n]:
            f |= SF
        return f

    def set_arith(self, f):
        self.flags = (self.flags & ~ARITH) | f

    def cond(self, cc):
        f = self.flags
        c = {
            'o': f & OF, 'no': not f & OF, 'b': f & CF, 'ae': not f & CF,
            'e': f & ZF, 'ne': not f & ZF, 'be': f & (CF | ZF), 'a': not f & (CF | ZF),
            's': f & SF, 'ns': not f & SF, 'p': f & PF, 'np': not f & PF,
            'l': bool(f & SF) != bool(f & OF), 'ge': bool(f & SF) == bool(f & OF),
            'le': f & ZF or bool(f & SF) != bool(f & OF), 'g': not f & ZF and bool(f & SF) == bool(f & OF),
        }[cc]
        return bool(c)

    def alu(self, op, a, b, n):
        """The eight ALU operations: (result, flags)."""
        m, s = MASK[n], SIGN[n]
        c = self.flags & CF
        if op in ('add', 'adc'):
            cin = 1 if op == 'adc' and c else 0
            full = a + b + cin
            res = full & m
            f = self.szp(res, n) | (CF if full > m else 0)
            f |= OF if (~(a ^ b) & (a ^ res)) & s else 0
            f |= AF if (a ^ b ^ res) & 0x10 else 0
            return res, f
        if op in ('sub', 'sbb', 'cmp'):
            bin_ = 1 if op == 'sbb' and c else 0
            full = a - b - bin_
            res = full & m
            f = self.szp(res, n) | (CF if full < 0 else 0)
            f |= OF if ((a ^ b) & (a ^ res)) & s else 0
            f |= AF if (a ^ b ^ res) & 0x10 else 0
            return res, f
        res = (a & b if op in ('and', 'test') else a | b if op == 'or' else a ^ b) & m
        return res, self.szp(res, n)

    # --- one instruction -----------------------------------------------------
    def step(self, i):
        """Execute i (decoded at self.eip). EIP moves on unless the instruction transfers."""
        nxt = (self.eip + i.size) & 0xFFFFFFFF
        self.eip = nxt
        op = i.op
        ops = i.ops
        n = ops[0][2] if ops and ops[0][0] == 'r' else (ops[0][6] if ops and ops[0][0] == 'm' else i.osize)
        h = getattr(self, 'op_' + op, None)
        if h is not None:
            h(i, ops, n)
            return
        if op in i386.ALU:
            a, b = self.read(i, ops[0]), self.read(i, ops[1])
            if ops[1][0] == 'i' and ops[1][2] != n:
                b &= MASK[n]
            res, f = self.alu(op, a, b, n)
            self.set_arith(f)
            if op != 'cmp':
                self.write(i, ops[0], res)
            return
        if op.startswith('j') and op[1:] in i386.CC:
            if self.cond(op[1:]):
                self.eip = ops[0][1]
            return
        if op.startswith('set') and op[3:] in i386.CC:
            self.write(i, ops[0], 1 if self.cond(op[3:]) else 0)
            return
        if op in i386.STRING_OPS:
            self.string(i, op[:-1] if op[-1] in 'bwd' else op, {'b': 1, 'w': 2, 'd': 4}[op[-1]])
            return
        if op in ('rol', 'ror', 'rcl', 'rcr', 'shl', 'sal', 'shr', 'sar'):
            self.shift(i, op, ops, n)
            return
        self.eip = (self.eip - i.size) & 0xFFFFFFFF
        self.system(i)

    # data movement
    def op_mov(self, i, ops, n):
        d, s = ops
        if d[0] == 's':
            self.load_segment(d[1], self.read(i, s, 2))
        elif s[0] == 's':
            self.write(i, d, self.sreg[s[1]], 2 if d[0] == 'm' else d[2])
        elif d[0] in 'cdt' or s[0] in 'cdt':
            self.system(i)
        else:
            self.write(i, d, self.read(i, s))

    def op_movzx(self, i, ops, n):
        self.write(i, ops[0], self.read(i, ops[1]))

    def op_movsx(self, i, ops, n):
        sz = ops[1][2] if ops[1][0] == 'r' else ops[1][6]
        self.write(i, ops[0], i386._sx(self.read(i, ops[1]), sz, 4))

    def op_lea(self, i, ops, n):
        _, off = self.ea(i, ops[1])
        self.write(i, ops[0], off)

    def op_xchg(self, i, ops, n):
        a, b = self.read(i, ops[0]), self.read(i, ops[1])
        self.write(i, ops[0], b)
        self.write(i, ops[1], a)

    def op_nop(self, i, ops, n):
        pass

    def op_esc(self, i, ops, n):
        pass                        # an x87 escape on a 386 with no coprocessor (i386.py)

    def op_push(self, i, ops, n):
        o = ops[0]
        if o[0] == 's':
            self.push(self.sreg[o[1]], i.osize)
        elif o[0] == 'i':
            self.push(o[1], i.osize)
        else:
            self.push(self.read(i, o), i.osize)

    def op_pop(self, i, ops, n):
        o = ops[0]
        if o[0] == 's':
            v = self.pop(i.osize)
            self.load_segment(o[1], v & 0xFFFF)
            return
        v = self.pop(i.osize)
        self.write(i, o, v, i.osize)     # pop into [esp+...] uses ESP after the pop

    def op_pushal(self, i, ops, n):
        sp = self.r[4]
        for k in range(8):
            self.push(sp if k == 4 else self.r[k], 4)

    def op_pushaw(self, i, ops, n):
        sp = self.r[4] & 0xFFFF
        for k in range(8):
            self.push(sp if k == 4 else self.r[k] & 0xFFFF, 2)

    def op_popal(self, i, ops, n):
        for k in reversed(range(8)):
            v = self.pop(4)
            if k != 4:
                self.r[k] = v

    def op_popaw(self, i, ops, n):
        for k in reversed(range(8)):
            v = self.pop(2)
            if k != 4:
                self.set_reg(k, 2, v)

    def op_pushfd(self, i, ops, n):
        self.push(self.flags & 0x00FCFFFF, 4)      # VM and RF read as 0

    def op_pushf(self, i, ops, n):
        self.push(self.flags & 0xFFFF, 2)

    def op_popfd(self, i, ops, n):
        v = self.pop(4)
        keep = 0x30000 | 0x3000                    # VM, RF; IOPL kept in this flat model
        self.flags = (self.flags & keep) | (v & ~keep & 0x47FD5) | 0x2

    def op_popf(self, i, ops, n):
        v = self.pop(2)
        keep = 0x3000
        self.flags = (self.flags & (0xFFFF0000 | keep)) | (v & ~keep & 0x7FD5) | 0x2

    def op_lahf(self, i, ops, n):
        self.set_reg(4, 1, (self.flags & 0xD5) | 0x2)

    def op_sahf(self, i, ops, n):
        self.flags = (self.flags & ~0xD5) | (self.get_reg(4, 1) & 0xD5)

    def op_cbw(self, i, ops, n):
        self.set_reg(0, 2, i386._sx(self.r[0] & 0xFF, 1, 2))

    def op_cwde(self, i, ops, n):
        self.r[0] = i386._sx(self.r[0] & 0xFFFF, 2, 4)

    def op_cwd(self, i, ops, n):
        self.set_reg(2, 2, 0xFFFF if self.r[0] & 0x8000 else 0)

    def op_cdq(self, i, ops, n):
        self.r[2] = 0xFFFFFFFF if self.r[0] & 0x80000000 else 0

    def op_xlatb(self, i, ops, n):
        seg, off = self.ea(i, ('m', ops[0][1], 3, None, 0, 0, 1))
        off = (off + (self.r[0] & 0xFF)) & (0xFFFFFFFF if i.asize == 4 else 0xFFFF)
        self.set_reg(0, 1, self.mem.read(self.lin(seg, off), 1))

    def _lptr(self, i, ops, sreg):
        seg, off = self.ea(i, ops[1])
        a = self.lin(seg, off)
        o = self.mem.read(a, i.osize)
        sel = self.mem.read(a + i.osize, 2)
        self.load_segment(sreg, sel)
        self.write(i, ops[0], o)

    def op_les(self, i, ops, n):
        self._lptr(i, ops, 0)

    def op_lds(self, i, ops, n):
        self._lptr(i, ops, 3)

    def op_lss(self, i, ops, n):
        self._lptr(i, ops, 2)

    def op_lfs(self, i, ops, n):
        self._lptr(i, ops, 4)

    def op_lgs(self, i, ops, n):
        self._lptr(i, ops, 5)

    # arithmetic
    def op_inc(self, i, ops, n):
        a = self.read(i, ops[0])
        res, f = self.alu('add', a, 1, n)
        self.flags = (self.flags & ~(ARITH & ~CF)) | (f & ~CF)
        self.write(i, ops[0], res)

    def op_dec(self, i, ops, n):
        a = self.read(i, ops[0])
        res, f = self.alu('sub', a, 1, n)
        self.flags = (self.flags & ~(ARITH & ~CF)) | (f & ~CF)
        self.write(i, ops[0], res)

    def op_neg(self, i, ops, n):
        a = self.read(i, ops[0])
        res, f = self.alu('sub', 0, a, n)
        self.set_arith(f)
        self.write(i, ops[0], res)

    def op_not(self, i, ops, n):
        self.write(i, ops[0], ~self.read(i, ops[0]) & MASK[n])

    def op_test(self, i, ops, n):
        a, b = self.read(i, ops[0]), self.read(i, ops[1])
        _, f = self.alu('and', a, b & MASK[n], n)
        self.set_arith(f)

    def op_mul(self, i, ops, n):
        a = self.get_reg(0, n)
        p = a * self.read(i, ops[0])
        self._wide(p, n)
        hi = p >> (8 * n)
        f = (CF | OF) if hi else 0
        self.set_arith(f | self.szp(p & MASK[n], n))

    def _wide(self, p, n):
        if n == 1:
            self.set_reg(0, 2, p)
        else:
            self.set_reg(0, n, p)
            self.set_reg(2, n, p >> (8 * n))

    def op_imul(self, i, ops, n):
        sx = lambda v, k: v - (1 << (8 * k)) if v & SIGN[k] else v
        if len(ops) == 1:
            p = sx(self.get_reg(0, n), n) * sx(self.read(i, ops[0]), n)
            self._wide(p & ((1 << (16 * n)) - 1), n)
            fits = -SIGN[n] <= p < SIGN[n]
        else:
            n = ops[0][2]
            a = self.read(i, ops[1]) if len(ops) == 3 else self.read(i, ops[0])
            b = (ops[2][1] if len(ops) == 3 else self.read(i, ops[1])) & MASK[n]
            p = sx(a, n) * sx(b, n)
            self.write(i, ops[0], p & MASK[n])
            fits = -SIGN[n] <= p < SIGN[n]
        self.set_arith((0 if fits else CF | OF) | self.szp(p & MASK[n], n))

    def op_div(self, i, ops, n):
        d = self.read(i, ops[0])
        if d == 0:
            raise Fault(0, 'divide by zero')
        a = self.get_reg(0, 2) if n == 1 else (self.get_reg(2, n) << (8 * n)) | self.get_reg(0, n)
        q, r = divmod(a, d)
        if q > MASK[n]:
            raise Fault(0, 'quotient too large')
        self._divres(q, r, n)

    def op_idiv(self, i, ops, n):
        sx = lambda v, k: v - (1 << (8 * k)) if v & (1 << (8 * k - 1)) else v
        d = sx(self.read(i, ops[0]), n)
        if d == 0:
            raise Fault(0, 'divide by zero')
        a = sx(self.get_reg(0, 2), 2) if n == 1 else sx((self.get_reg(2, n) << (8 * n)) | self.get_reg(0, n), 2 * n)
        q = abs(a) // abs(d)
        if (a < 0) != (d < 0):
            q = -q
        r = a - q * d
        if not -SIGN[n] <= q < SIGN[n]:
            raise Fault(0, 'quotient too large')
        self._divres(q & MASK[n], r & MASK[n], n)

    def _divres(self, q, r, n):
        if n == 1:
            self.set_reg(0, 1, q)
            self.set_reg(4, 1, r)
        else:
            self.set_reg(0, n, q)
            self.set_reg(2, n, r)

    def op_cmc(self, i, ops, n):
        self.flags ^= CF

    def op_clc(self, i, ops, n):
        self.flags &= ~CF

    def op_stc(self, i, ops, n):
        self.flags |= CF

    def op_cld(self, i, ops, n):
        self.flags &= ~DF

    def op_std(self, i, ops, n):
        self.flags |= DF

    def op_cli(self, i, ops, n):
        self.flags &= ~IF

    def op_sti(self, i, ops, n):
        self.flags |= IF

    def op_salc(self, i, ops, n):
        self.set_reg(0, 1, 0xFF if self.flags & CF else 0)

    def op_daa(self, i, ops, n):
        al, cf, af = self.r[0] & 0xFF, self.flags & CF, self.flags & AF
        old_al, f = al, 0
        if (al & 0xF) > 9 or af:
            al += 6
            f |= AF
            if al > 0xFF:
                f |= CF
            al &= 0xFF
        if old_al > 0x99 or cf:
            al = (al + 0x60) & 0xFF
            f |= CF
        self.set_reg(0, 1, al)
        self.set_arith(f | self.szp(al, 1))

    def op_das(self, i, ops, n):
        al, cf, af = self.r[0] & 0xFF, self.flags & CF, self.flags & AF
        old_al, f = al, 0
        if (al & 0xF) > 9 or af:
            f |= AF | (CF if al < 6 else 0)
            al = (al - 6) & 0xFF
        if old_al > 0x99 or cf:
            al = (al - 0x60) & 0xFF
            f |= CF
        self.set_reg(0, 1, al)
        self.set_arith(f | self.szp(al, 1))

    def op_aaa(self, i, ops, n):
        if (self.r[0] & 0xF) > 9 or self.flags & AF:
            self.set_reg(0, 2, (self.r[0] + 0x106) & 0xFFFF)
            f = AF | CF
        else:
            f = 0
        self.set_reg(0, 1, self.r[0] & 0xF)
        self.set_arith(f)

    def op_aas(self, i, ops, n):
        if (self.r[0] & 0xF) > 9 or self.flags & AF:
            self.set_reg(0, 2, (self.r[0] - 6) & 0xFFFF)
            self.set_reg(4, 1, (self.get_reg(4, 1) - 1) & 0xFF)
            f = AF | CF
        else:
            f = 0
        self.set_reg(0, 1, self.r[0] & 0xF)
        self.set_arith(f)

    def op_aam(self, i, ops, n):
        base = ops[0][1]
        if base == 0:
            raise Fault(0, 'aam 0')
        al = self.r[0] & 0xFF
        self.set_reg(4, 1, al // base)
        self.set_reg(0, 1, al % base)
        self.set_arith(self.szp(self.r[0] & 0xFF, 1))

    def op_aad(self, i, ops, n):
        al = (self.get_reg(0, 1) + self.get_reg(4, 1) * ops[0][1]) & 0xFF
        self.set_reg(0, 2, al)
        self.set_arith(self.szp(al, 1))

    # bits
    def _bitop(self, i, ops, n, how):
        o, b = ops
        bit = self.read(i, b)
        if o[0] == 'm' and b[0] == 'r':
            # a register bit offset reaches beyond the operand (signed)
            sx = bit - (1 << (8 * n)) if bit & SIGN[n] else bit
            seg, off = self.ea(i, o)
            byteoff = (sx >> 3) & ~(n - 1)
            a = self.lin(seg, (off + byteoff) & 0xFFFFFFFF)
            bit = sx & (8 * n - 1)
            v = self.mem.read(a, n)
            put = lambda x: self.mem.write(a, n, x)
        else:
            bit &= 8 * n - 1
            v = self.read(i, o)
            put = lambda x: self.write(i, o, x)
        cf = (v >> bit) & 1
        self.flags = (self.flags & ~CF) | cf
        if how == 's':
            put(v | (1 << bit))
        elif how == 'r':
            put(v & ~(1 << bit))
        elif how == 'c':
            put(v ^ (1 << bit))

    def op_bt(self, i, ops, n):
        self._bitop(i, ops, n, '')

    def op_bts(self, i, ops, n):
        self._bitop(i, ops, n, 's')

    def op_btr(self, i, ops, n):
        self._bitop(i, ops, n, 'r')

    def op_btc(self, i, ops, n):
        self._bitop(i, ops, n, 'c')

    def op_bsf(self, i, ops, n):
        n = ops[0][2]
        v = self.read(i, ops[1], n)
        if v == 0:
            self.flags |= ZF
            return
        self.flags &= ~ZF
        self.write(i, ops[0], (v & -v).bit_length() - 1)

    def op_bsr(self, i, ops, n):
        n = ops[0][2]
        v = self.read(i, ops[1], n)
        if v == 0:
            self.flags |= ZF
            return
        self.flags &= ~ZF
        self.write(i, ops[0], v.bit_length() - 1)

    # shifts and rotates
    def shift(self, i, op, ops, n):
        bits = 8 * n
        cnt = self.read(i, ops[1]) & 0x1F
        if cnt == 0:
            return
        v = self.read(i, ops[0])
        m = MASK[n]
        f = self.flags
        if op in ('shl', 'sal'):
            res = (v << cnt) & m
            cf = (v >> (bits - cnt)) & 1 if cnt <= bits else 0
            of = ((res >> (bits - 1)) & 1) ^ cf
            f = (f & ~ARITH) | self.szp(res, n) | (CF if cf else 0) | (OF if of else 0)
        elif op == 'shr':
            res = v >> cnt if cnt < bits else 0
            cf = (v >> (cnt - 1)) & 1 if cnt <= bits else 0
            of = (v >> (bits - 1)) & 1
            f = (f & ~ARITH) | self.szp(res, n) | (CF if cf else 0) | (OF if of else 0)
        elif op == 'sar':
            sv = v - (1 << bits) if v & SIGN[n] else v
            res = (sv >> min(cnt, bits)) & m
            cf = (sv >> (min(cnt, bits) - 1)) & 1
            f = (f & ~ARITH) | self.szp(res, n) | (CF if cf else 0)
        elif op in ('rol', 'ror'):
            c = cnt % bits
            res = ((v << c) | (v >> (bits - c))) & m if op == 'rol' else ((v >> c) | (v << (bits - c))) & m
            if op == 'rol':
                cf = res & 1
                of = ((res >> (bits - 1)) & 1) ^ cf
            else:
                cf = (res >> (bits - 1)) & 1
                of = cf ^ ((res >> (bits - 2)) & 1)
            f = (f & ~(CF | OF)) | (CF if cf else 0) | (OF if of else 0)
        else:   # rcl, rcr: through CF, modulo bits + 1
            c = cnt % (bits + 1)
            x = v | ((f & CF) << bits)
            w = bits + 1
            if op == 'rcl':
                x = ((x << c) | (x >> (w - c))) & ((1 << w) - 1)
                res, cf = x & m, (x >> bits) & 1
                of = ((res >> (bits - 1)) & 1) ^ cf
            else:
                of = ((v >> (bits - 1)) & 1) ^ (f & CF)
                x = ((x >> c) | (x << (w - c))) & ((1 << w) - 1)
                res, cf = x & m, (x >> bits) & 1
            f = (f & ~(CF | OF)) | (CF if cf else 0) | (OF if of else 0)
        self.flags = f
        self.write(i, ops[0], res)

    def _double(self, i, ops, n, left):
        bits = 8 * n
        cnt = self.read(i, ops[2]) & 0x1F
        if cnt == 0:
            return
        d, s = self.read(i, ops[0]), self.read(i, ops[1])
        m = MASK[n]
        if left:
            x = (d << bits) | s
            res = (x << cnt >> bits) & m
            cf = (d >> (bits - cnt)) & 1 if cnt <= bits else (s >> (2 * bits - cnt)) & 1
        else:
            x = (s << bits) | d
            res = (x >> cnt) & m
            cf = (x >> (cnt - 1)) & 1
        of = ((res ^ d) >> (bits - 1)) & 1
        self.flags = (self.flags & ~ARITH) | self.szp(res, n) | (CF if cf else 0) | (OF if of else 0)
        self.write(i, ops[0], res)

    def op_shld(self, i, ops, n):
        self._double(i, ops, n, True)

    def op_shrd(self, i, ops, n):
        self._double(i, ops, n, False)

    # strings
    def string(self, i, op, n):
        a16 = i.asize == 2
        cx = (lambda: self.r[1] & 0xFFFF) if a16 else (lambda: self.r[1])
        step = -n if self.flags & DF else n

        def adv(reg):
            if a16:
                self.set_reg(reg, 2, self.r[reg] + step)
            else:
                self.r[reg] = (self.r[reg] + step) & 0xFFFFFFFF

        def at(reg, seg):
            off = self.r[reg] & (0xFFFF if a16 else 0xFFFFFFFF)
            return self.lin(seg, off)

        src_seg = i.seg if i.seg is not None else 3
        rep = i.rep
        while True:
            if rep is not None and cx() == 0:
                return
            if op == 'movs':
                self.mem.write(at(7, 0), n, self.mem.read(at(6, src_seg), n)); adv(6); adv(7)
            elif op == 'stos':
                self.mem.write(at(7, 0), n, self.get_reg(0, n)); adv(7)
            elif op == 'lods':
                self.set_reg(0, n, self.mem.read(at(6, src_seg), n)); adv(6)
            elif op in ('cmps', 'scas'):
                a = self.mem.read(at(6, src_seg), n) if op == 'cmps' else self.get_reg(0, n)
                b = self.mem.read(at(7, 0), n)
                _, f = self.alu('sub', a, b, n)
                self.set_arith(f)
                if op == 'cmps':
                    adv(6)
                adv(7)
            elif op == 'ins':
                self.mem.write(at(7, 0), n, self.port_in(self.r[2] & 0xFFFF, n)); adv(7)
            elif op == 'outs':
                self.port_out(self.r[2] & 0xFFFF, n, self.mem.read(at(6, src_seg), n)); adv(6)
            if rep is None:
                return
            if a16:
                self.set_reg(1, 2, self.r[1] - 1)
            else:
                self.r[1] = (self.r[1] - 1) & 0xFFFFFFFF
            if op in ('cmps', 'scas'):
                z = bool(self.flags & ZF)
                if (rep == 0xF3 and not z) or (rep == 0xF2 and z):
                    return

    # transfers
    def op_jmp(self, i, ops, n):
        o = ops[0]
        self.eip = o[1] if o[0] == 'j' else self.read(i, o) & (0xFFFFFFFF if i.osize == 4 else 0xFFFF)

    def op_call(self, i, ops, n):
        o = ops[0]
        t = o[1] if o[0] == 'j' else self.read(i, o)
        self.push(self.eip, i.osize)
        self.eip = t & (0xFFFFFFFF if i.osize == 4 else 0xFFFF)

    def op_ret(self, i, ops, n):
        self.eip = self.pop(i.osize)
        if ops:
            self.r[4] = (self.r[4] + ops[0][1]) & 0xFFFFFFFF

    def op_retf(self, i, ops, n):
        eip = self.pop(i.osize)
        cs = self.pop(i.osize) & 0xFFFF
        self.load_segment(1, cs)
        self.eip = eip
        if ops:
            self.r[4] = (self.r[4] + ops[0][1]) & 0xFFFFFFFF

    def _far(self, i, ops, call):
        o = ops[0]
        if o[0] == 'f':
            sel, off = o[1], o[2]
        else:
            seg, eo = self.ea(i, o)
            a = self.lin(seg, eo)
            off = self.mem.read(a, i.osize)
            sel = self.mem.read(a + i.osize, 2)
        if call:
            self.push(self.sreg[1], i.osize)
            self.push(self.eip, i.osize)
        self.load_segment(1, sel)
        self.eip = off

    def op_lcall(self, i, ops, n):
        self._far(i, ops, True)

    def op_ljmp(self, i, ops, n):
        self._far(i, ops, False)

    def op_iretd(self, i, ops, n):
        eip = self.pop(4)
        cs = self.pop(4) & 0xFFFF
        fl = self.pop(4)
        self.load_segment(1, cs)
        self.eip = eip
        self.flags = (fl & 0x257FD5) | 0x2

    def _loop(self, i, ops, extra):
        if i.asize == 2:
            self.set_reg(1, 2, self.r[1] - 1)
            c = self.r[1] & 0xFFFF
        else:
            self.r[1] = (self.r[1] - 1) & 0xFFFFFFFF
            c = self.r[1]
        if c and extra():
            self.eip = ops[0][1]

    def op_loop(self, i, ops, n):
        self._loop(i, ops, lambda: True)

    def op_loope(self, i, ops, n):
        self._loop(i, ops, lambda: self.flags & ZF)

    def op_loopne(self, i, ops, n):
        self._loop(i, ops, lambda: not self.flags & ZF)

    def op_jecxz(self, i, ops, n):
        if self.r[1] == 0:
            self.eip = ops[0][1]

    def op_jcxz(self, i, ops, n):
        if self.r[1] & 0xFFFF == 0:
            self.eip = ops[0][1]

    def op_enter(self, i, ops, n):
        size, level = ops[0][1], ops[1][1] & 31
        w = i.osize
        self.push(self.get_reg(5, w), w)
        frame = self.r[4]
        for k in range(1, level):
            self.r[5] = (self.r[5] - w) & 0xFFFFFFFF
            self.push(self.mem.read(self.lin(2, self.r[5]), w), w)
        if level:
            self.push(frame, w)
        self.set_reg(5, w, frame)
        self.r[4] = (self.r[4] - size) & 0xFFFFFFFF

    def op_leave(self, i, ops, n):
        self.r[4] = self.r[5]
        self.set_reg(5, i.osize, self.pop(i.osize))

    def op_int(self, i, ops, n):
        self.interrupt(ops[0][1], i)

    def op_in(self, i, ops, n):
        port = ops[1][1] if ops[1][0] == 'i' else self.r[2] & 0xFFFF
        self.set_reg(0, ops[0][2], self.port_in(port, ops[0][2]))

    def op_out(self, i, ops, n):
        port = ops[0][1] if ops[0][0] == 'i' else self.r[2] & 0xFFFF
        self.port_out(port, ops[1][2], self.get_reg(0, ops[1][2]))

    def op_bound(self, i, ops, n):
        n = ops[0][2]
        seg, off = self.ea(i, ops[1])
        a = self.lin(seg, off)
        sx = lambda v: v - (1 << (8 * n)) if v & SIGN[n] else v
        v, lo, hi = sx(self.get_reg(ops[0][1], n)), sx(self.mem.read(a, n)), sx(self.mem.read(a + n, n))
        if not lo <= v <= hi:
            raise Fault(5, 'bound')


# --- the check against unicorn --------------------------------------------------
SET_ASIDE = {'int', 'int1', 'int3', 'into', 'iret', 'iretd', 'in', 'out', 'insb', 'insw', 'insd',
             'outsb', 'outsw', 'outsd', 'hlt', 'lcall', 'ljmp', 'retf', 'les', 'lds', 'lss', 'lfs',
             'lgs', 'arpl', 'sldt', 'str', 'lldt', 'ltr', 'verr', 'verw', 'sgdt', 'sidt', 'lgdt',
             'lidt', 'smsw', 'lmsw', 'lar', 'lsl', 'clts', 'wait', 'cli', 'sti', 'popf', 'popfd',
             'bound', 'pause', 'esc'}


def set_aside(i):
    """Instructions the flat check cannot hold against unicorn (the machine's, or segments')."""
    if i.op in SET_ASIDE:
        return 'machine'
    if any(o[0] in 'scdt' for o in i.ops) and i.op in ('mov', 'pop'):
        return 'segment' if any(o[0] == 's' for o in i.ops) else 'machine'
    if i.op == 'ret' and i.ops and i.ops[0][1] >= 0x8000:
        return 'unicorn: ret imm16 sign-extended'      # the 386 zero-extends it (Intel, Bochs)
    if i.op == 'test' and i.raw[-len(i.raw):] and _group3_alias(i):
        return 'unicorn: F6/F7 /1 (TEST) refused'      # an alias of /0 on Intel CPUs
    return None


def _group3_alias(i):
    """F6 /1 or F7 /1: TEST by its undocumented second encoding."""
    k = 0
    while i.raw[k] in (0x26, 0x2E, 0x36, 0x3E, 0x64, 0x65, 0x66, 0x67, 0xF0, 0xF2, 0xF3):
        k += 1
    return i.raw[k] in (0xF6, 0xF7) and (i.raw[k + 1] >> 3) & 7 == 1


def _fill(page):
    return random.Random(page * 2654435761).randbytes(4096)


def _uc_run(code, base, state, rep=False):
    import unicorn
    from unicorn import x86_const as U
    uc = unicorn.Uc(unicorn.UC_ARCH_X86, unicorn.UC_MODE_32)
    mapped = set()

    def mp(page):
        if page not in mapped:
            uc.mem_map(page << 12, 4096)
            uc.mem_write(page << 12, _fill(page))
            mapped.add(page)

    def on_unmapped(uc_, access, address, size, value, data):
        for p in range(address >> 12, ((address + max(size, 1) - 1) >> 12) + 1):
            mp(p & 0xFFFFF)
        return True
    for p in range(base >> 12, ((base + len(code) - 1) >> 12) + 1):
        mp(p)
    uc.mem_write(base, code)
    writes = []
    uc.hook_add(unicorn.UC_HOOK_MEM_READ_UNMAPPED | unicorn.UC_HOOK_MEM_WRITE_UNMAPPED |
                unicorn.UC_HOOK_MEM_FETCH_UNMAPPED, on_unmapped)
    uc.hook_add(unicorn.UC_HOOK_MEM_WRITE, lambda u, a, ad, sz, v, d: writes.append((ad, sz)))
    regs = [U.UC_X86_REG_EAX, U.UC_X86_REG_ECX, U.UC_X86_REG_EDX, U.UC_X86_REG_EBX,
            U.UC_X86_REG_ESP, U.UC_X86_REG_EBP, U.UC_X86_REG_ESI, U.UC_X86_REG_EDI]
    for k, r in enumerate(regs):
        uc.reg_write(r, state['r'][k])
    uc.reg_write(U.UC_X86_REG_EFLAGS, state['flags'])
    err = None
    try:
        if rep:     # QEMU runs a REP one iteration at a time: on to the next instruction
            uc.emu_start(base, base + len(code), count=1000000)
        else:
            uc.emu_start(base, 0xFFFFFFFF, count=1)
    except unicorn.UcError as e:
        err = str(e)
    out = {'r': [uc.reg_read(r) for r in regs], 'eip': uc.reg_read(U.UC_X86_REG_EIP),
           'flags': uc.reg_read(U.UC_X86_REG_EFLAGS), 'err': err}
    out['mem'] = {}
    for ad, sz in writes:
        for k in range(sz):
            a = (ad + k) & 0xFFFFFFFF
            out['mem'][a] = uc.mem_read(a, 1)[0]
    return out


def _ours_run(code, base, state):
    mem = FlatMemory(_fill)
    mem.load(base, code)
    cpu = CPU(mem)
    cpu.r = list(state['r'])
    cpu.flags = state['flags']
    cpu.eip = base
    mem.written.clear()
    err = None
    try:
        i = decode(code, 0, base)
        cpu.step(i)
    except Fault as e:
        err = str(e)
    out = {'r': cpu.r, 'eip': cpu.eip, 'flags': cpu.flags, 'err': err,
           'mem': {a: mem.read(a, 1) for a in mem.written}}
    return out


def random_state(rnd, i):
    r = [rnd.getrandbits(32) for _ in range(8)]
    if rnd.random() < 0.3:
        r = [x & 0xFF for x in r] if rnd.random() < 0.5 else [x | 0xFFFFFF00 for x in r]
    if i.rep is not None or i.op.startswith('loop'):
        r[1] = rnd.randrange(0, 12)
    r[4] = (r[4] & ~3) | 0x1000            # keep the stack off the code page and aligned
    flags = 0x2 | (rnd.getrandbits(12) & (ARITH | DF))
    return {'r': r, 'flags': flags}


def compare_one(i, raw, base, state):
    a = _ours_run(raw, base, state)
    b = _uc_run(raw, base, state, i.rep is not None and i.op in i386.STRING_OPS)
    mask = 0xFFFFFFFF & ~UNDEFINED.get(i.op, 0)
    if i.op in ('shl', 'sal', 'shr', 'sar', 'rol', 'ror', 'rcl', 'rcr', 'shld', 'shrd'):
        cnt_op = i.ops[-1]
        mask &= ~(OF | AF)                     # OF undefined but for a count of 1; AF undefined
        if i.op in ('shld', 'shrd') and i.osize == 2:
            c = (cnt_op[1] if cnt_op[0] == 'i' else state['r'][1]) & 0x1F
            if c > 16:
                return ''                      # a 16-bit double shift by more than 16: undefined
    if (a['err'] is None) != (b['err'] is None):
        return 'fault: ours %s, unicorn %s' % (a['err'], b['err'])
    if a['err'] is not None:
        return ''
    diffs = []
    for k in range(8):
        if a['r'][k] != b['r'][k]:
            diffs.append('%s %08x/%08x' % (i386.REG32[k], a['r'][k], b['r'][k]))
    if a['eip'] != b['eip']:
        diffs.append('eip %08x/%08x' % (a['eip'], b['eip']))
    if (a['flags'] ^ b['flags']) & mask & 0xFFF:
        diffs.append('flags %03x/%03x' % (a['flags'] & 0xFFF, b['flags'] & 0xFFF))
    # the bytes either wrote, compared by value (QEMU writes back an unchanged operand: same bytes)
    def orig(x):
        if base <= x < base + len(raw):
            return raw[x - base]
        return _fill(x >> 12)[x & 0xFFF]
    bad = [x for x in set(a['mem']) | set(b['mem'])
           if a['mem'].get(x, orig(x)) != b['mem'].get(x, orig(x))]
    if bad:
        diffs.append('memory: %d bytes differ' % len(bad))
    return '; '.join(diffs)


def check(insns, states, seed=1, limit=60):
    rnd = random.Random(seed)
    stats = collections.Counter()
    bad = []
    for i in insns:
        why = set_aside(i)
        if why:
            stats['set aside: ' + why] += 1
            continue
        base = 0x00400000 + (rnd.getrandbits(12) & ~0xF) if i.ops and i.ops[0][0] != 'j' else 0x00400000
        raw = i.raw
        # a relative transfer was decoded at its own address: re-decode at base
        try:
            j = decode(raw, 0, base)
        except Refused:
            continue
        for k in range(states):
            st = random_state(rnd, j)
            d = compare_one(j, raw, base, st)
            stats['vectors'] += 1
            if d:
                stats['differ'] += 1
                if len(bad) < limit:
                    bad.append('%s %s [%s]: %s' % (j.op, i386.format_ops(j), raw.hex(' '), d))
    return stats, bad


def known_answers():
    """What a 386 does that a careless interpreter does not."""
    t = []

    def run(code, regs=None, flags=0x2, mem=None):
        m = FlatMemory()
        m.load(0x1000, code)
        if mem:
            for a, data in mem.items():
                m.load(a, data)
        c = CPU(m)
        c.eip = 0x1000
        c.r[4] = 0x8000
        for k, v in (regs or {}).items():
            c.r[i386.REG32.index(k)] = v
        c.flags = flags
        while c.eip < 0x1000 + len(code):
            c.step(decode(m.pages[1], c.eip - 0x1000, c.eip))
        return c
    # rcl of a byte by 9 is a full turn through CF
    c = run(bytes([0xC0, 0xD0, 9]), {'eax': 0x81}, 0x3)
    t.append(('rcl al, 9 leaves al and CF', c.r[0] & 0xFF == 0x81 and c.flags & CF))
    # shift counts are taken modulo 32 on the 386
    c = run(bytes([0xC1, 0xE0, 33]), {'eax': 1})
    t.append(('shl eax, 33 shifts by 1', c.r[0] == 2))
    # push esp pushes the value before the push
    c = run(bytes([0x54, 0x58]))
    t.append(('push esp; pop eax == old esp', c.r[0] == 0x8000))
    # pop [esp] computes the address after the pop
    c = run(bytes([0x6A, 0x05, 0x6A, 0x07, 0x8F, 0x04, 0x24, 0x58]))
    t.append(('pop [esp] writes above the popped word', c.r[0] == 7))
    # bt with a register offset reaches past the operand
    c = run(bytes([0x0F, 0xA3, 0x03]), {'ebx': 0x2000, 'eax': 35}, mem={0x2004: b'\x08'})
    t.append(('bt [ebx], eax with eax=35 tests bit 3 of [ebx+4]', c.flags & CF))
    # idiv rounds toward zero
    c = run(bytes([0xF7, 0xF9]), {'eax': (-7) & 0xFFFFFFFF, 'edx': 0xFFFFFFFF, 'ecx': 2})
    t.append(('idiv -7/2 = -3 rem -1', c.r[0] == 0xFFFFFFFD and c.r[2] == 0xFFFFFFFF))
    # rep with ecx = 0 does nothing
    c = run(bytes([0xF3, 0xAA]), {'ecx': 0, 'edi': 0x3000, 'eax': 0x55})
    t.append(('rep stosb with ecx 0 writes nothing', c.r[7] == 0x3000))
    # loop with ecx = 1 falls through after decrementing
    c = run(bytes([0xE2, 0xFE]), {'ecx': 1})
    t.append(('loop with ecx 1 falls through', c.r[1] == 0 and c.eip == 0x1002))
    # 16-bit operand: inc ax wraps without touching the high half
    c = run(bytes([0x66, 0x40]), {'eax': 0x1234FFFF})
    t.append(('inc ax wraps in 16 bits', c.r[0] == 0x12340000 and c.flags & ZF))
    # neg 0 clears CF
    c = run(bytes([0xF7, 0xD8]), {'eax': 0}, 0x3)
    t.append(('neg 0 clears CF', not c.flags & CF))
    return t


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--test', action='store_true')
    ap.add_argument('--check', metavar='FILE')
    ap.add_argument('--raw', action='store_true')
    ap.add_argument('--far-table', metavar='FROM:TO')
    ap.add_argument('--seed', default='')
    ap.add_argument('--states', type=int, default=4)
    ap.add_argument('--sweep', type=int, metavar='N')
    a = ap.parse_args(argv)
    if a.test:
        res = known_answers()
        for name, ok in res:
            print('  %-50s %s' % (name, 'ok' if ok else 'FAILS'))
        print('%d of %d' % (sum(1 for _, ok in res if ok), len(res)))
        return
    if a.sweep:
        rnd = random.Random(7)
        insns = []
        while len(insns) < a.sweep:
            buf = bytes(rnd.getrandbits(8) for _ in range(16))
            try:
                i = decode(buf, 0, 0)
            except Refused:
                continue
            if i.op in ('call', 'jmp') or i.op.startswith(('j', 'loop')):
                pass
            insns.append(i)
        stats, bad = check(insns, a.states)
    else:
        from . import exp as expmod
        from . import x86map
        data = open(a.check, 'rb').read()
        seeds = [int(x, 16) for x in a.seed.split(',') if x]
        if a.raw:
            image, base = data, 0
            if a.far_table:
                lo, hi = (int(x, 16) for x in a.far_table.split(':'))
                seeds += [int.from_bytes(image[o:o + 4], 'little') for o in range(lo, hi, 8)
                          if int.from_bytes(image[o:o + 4], 'little')]
            entry, seeds = seeds[0], seeds[1:]
        else:
            e = expmod.Exp(data)
            image, base, entry = e.image, e.base_offset, e.eip
        m = x86map.Map(image, base, entry, seeds)
        insns = []
        for addr in sorted(m.insn):
            try:
                insns.append(decode(image, addr - base, addr))
            except Refused:
                pass
        stats, bad = check(insns, a.states)
    print(', '.join('%s %d' % kv for kv in sorted(stats.items())))
    for b in bad:
        print('  ' + b)


if __name__ == '__main__':
    main()
