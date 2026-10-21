"""The 386's integer instruction set, decoded exactly.

What Towns programs are made of: 32-bit protected-mode code (and the odd
16-bit routine), with the operand-size and address-size prefixes, segment
overrides, REP/REPNE/LOCK, ModRM and SIB. Every instruction the 80386 has
outside the x87 is decoded; what later CPUs added (CPUID, BSWAP, CMPXCHG,
XADD, INVD, WBINVD, INVLPG, the CMOVs, ...) is refused with a reason, as
data decoded as code is: the recompiler stops on them rather than guess.

The x87's escapes (D8-DF) decode as one instruction, `esc N, r/m`, for
the machines as MAME has them: the Towns' 386 and the Marty's 386SX with
no coprocessor, where an escape reads its ModRM and does nothing. A
program's start-up probe for an FPU (MetaWare's `fninit`, `fnstcw`, a
control word compared with 0x33F) then finds none, and its library takes
the software path. `check` holds an `esc` against capstone's x87 reading
by its length and memory operand.

An instruction is an `Insn`: its address and length, a mnemonic as
capstone spells it (so the two can be held against each other), the
operand and address sizes, the prefixes, and its operands:

    ('r', n, size)                  a general register: n 0-7 in the order
                                    eax ecx edx ebx esp ebp esi edi; size 1,
                                    2 or 4 (size 1: al cl dl bl ah ch dh bh)
    ('s', n)                        a segment register: es cs ss ds fs gs
    ('m', seg, base, index, scale, disp, size)
                                    memory: seg the override or None, base
                                    and index register numbers or None,
                                    disp an unsigned 32-bit (16-bit when the
                                    address size is 16), size in bytes (0:
                                    none, as LEA's; 6: a far pointer)
    ('i', value, size)              an immediate, as the instruction encodes
                                    it sign-extended to its operand's size
    ('j', target)                   a relative transfer's target
    ('f', selector, offset)         a far pointer immediate
    ('c', n) / ('d', n) / ('t', n)  control, debug, test registers

`check` decodes every instruction of an image's reached code (or every
offset of a range) with both this module and capstone and compares them
field by field.

    python -m fmtownskit.i386 GAME.EXP --dis a7f0 -n 20
    python -m fmtownskit.i386 GAME.EXP --check            # the reached code, against capstone
    python -m fmtownskit.i386 TBIOS.BIN --raw --far-table 20:100 --check
    python -m fmtownskit.i386 --sweep 200000              # random bytes, against capstone
"""
import argparse
import collections
import random
import struct
import sys

REG32 = ['eax', 'ecx', 'edx', 'ebx', 'esp', 'ebp', 'esi', 'edi']
REG16 = ['ax', 'cx', 'dx', 'bx', 'sp', 'bp', 'si', 'di']
REG8 = ['al', 'cl', 'dl', 'bl', 'ah', 'ch', 'dh', 'bh']
SREG = ['es', 'cs', 'ss', 'ds', 'fs', 'gs']
CC = ['o', 'no', 'b', 'ae', 'e', 'ne', 'be', 'a', 's', 'ns', 'p', 'np', 'l', 'ge', 'le', 'g']
ALU = ['add', 'or', 'adc', 'sbb', 'and', 'sub', 'xor', 'cmp']
SHIFT = ['rol', 'ror', 'rcl', 'rcr', 'shl', 'shr', 'sal', 'sar']
SEG_PREFIX = {0x26: 0, 0x2E: 1, 0x36: 2, 0x3E: 3, 0x64: 4, 0x65: 5}


class Refused(Exception):
    """Bytes that are not a 386 integer instruction (or are a later CPU's)."""


class Insn:
    __slots__ = ('addr', 'size', 'op', 'osize', 'asize', 'ops', 'rep', 'lock', 'seg', 'raw')

    def __init__(self):
        self.ops = []
        self.rep = None
        self.lock = False
        self.seg = None

    def __repr__(self):
        return '<%x %s %s>' % (self.addr, self.op, format_ops(self))


def reg_name(n, size):
    return (REG8, REG16, None, REG32)[size - 1][n] if size in (1, 2, 4) else '?'


class _Stream:
    def __init__(self, data, off):
        self.d, self.p = data, off

    def u8(self):
        if self.p >= len(self.d):
            raise Refused('runs past the end')
        v = self.d[self.p]
        self.p += 1
        return v

    def u16(self):
        return self.u8() | (self.u8() << 8)

    def u32(self):
        return self.u16() | (self.u16() << 16)

    def s8(self):
        v = self.u8()
        return v - 256 if v & 0x80 else v


def _sx(v, frm, to):
    """v (frm bytes) sign-extended to `to` bytes, as an unsigned value."""
    bits = frm * 8
    if v & (1 << (bits - 1)):
        v -= 1 << bits
    return v & ((1 << (to * 8)) - 1)


def _modrm(s, i, size):
    """Decode a ModRM (and SIB, displacement): (mod, reg, rm-operand)."""
    b = s.u8()
    mod, reg, rm = b >> 6, (b >> 3) & 7, b & 7
    if mod == 3:
        return mod, reg, ('r', rm, size)
    if i.asize == 2:
        bases = [(3, 6), (3, 7), (5, 6), (5, 7), (6, None), (7, None), (5, None), (3, None)]
        base, index = bases[rm]
        if mod == 0 and rm == 6:
            base, disp = None, s.u16()
        elif mod == 0:
            disp = 0
        elif mod == 1:
            disp = s.s8() & 0xFFFF
        else:
            disp = s.u16()
        return mod, reg, ('m', i.seg, base, index, 1 if index is not None else 0, disp, size)
    index, scale = None, 0
    if rm == 4:
        sib = s.u8()
        scale, idx, base = 1 << (sib >> 6), (sib >> 3) & 7, sib & 7
        if idx != 4:
            index = idx
        else:
            scale = 0
        if base == 5 and mod == 0:
            base = None
            disp = s.u32()
        else:
            disp = None
    else:
        base = rm
        disp = None
        if rm == 5 and mod == 0:
            base = None
            disp = s.u32()
    if disp is None:
        disp = 0 if mod == 0 else (s.s8() & 0xFFFFFFFF if mod == 1 else s.u32())
    return mod, reg, ('m', i.seg, base, index, scale, disp, size)


def _imm(s, size):
    return s.u8() if size == 1 else s.u16() if size == 2 else s.u32()


def decode(data, off, addr=None, bits=32):
    """The instruction at data[off:]: an Insn, or Refused."""
    if addr is None:
        addr = off
    s = _Stream(data, off)
    i = Insn()
    i.addr = addr
    osize = asize = 4 if bits == 32 else 2
    flip = lambda n: 6 - n          # 4 <-> 2
    seen = set()
    while True:
        b = s.u8()
        if b in SEG_PREFIX:
            i.seg = SEG_PREFIX[b]
        elif b == 0x66:
            if 0x66 not in seen:
                osize = flip(osize)
        elif b == 0x67:
            if 0x67 not in seen:
                asize = flip(asize)
        elif b == 0xF0:
            i.lock = True
        elif b in (0xF2, 0xF3):
            if i.rep is not None and i.rep != b:
                raise Refused('both REP prefixes')
            i.rep = b
        else:
            break
        seen.add(b)
        if s.p - off > 14:
            raise Refused('too many prefixes')
    i.osize, i.asize = osize, asize
    v = osize
    ops = i.ops

    def E(size):
        mod, reg, op = _modrm(s, i, size)
        return mod, reg, op

    def done(op):
        i.op = op
        i.size = s.p - off
        if i.size > 15:
            raise Refused('longer than 15 bytes')
        if i.lock and not (op in LOCKABLE and ops and ops[0][0] == 'm'):
            raise Refused('lock on %s: invalid opcode' % op)
        i.raw = bytes(data[off:s.p])
        return i

    def rel(n):
        d = s.s8() if n == 1 else _sx(_imm(s, n), n, 4)
        if n != 1:
            d = d - (1 << 32) if d & 0x80000000 else d
        t = (addr + (s.p - off) + d) & (0xFFFFFFFF if osize == 4 else 0xFFFF)
        ops.append(('j', t))

    # --- one-byte opcodes ------------------------------------------------
    if b < 0x40 and (b & 7) < 6:
        op = ALU[b >> 3]
        k = b & 7
        if k == 0:
            _, r, m = E(1); ops += [m, ('r', r, 1)]
        elif k == 1:
            _, r, m = E(v); ops += [m, ('r', r, v)]
        elif k == 2:
            _, r, m = E(1); ops += [('r', r, 1), m]
        elif k == 3:
            _, r, m = E(v); ops += [('r', r, v), m]
        elif k == 4:
            ops += [('r', 0, 1), ('i', s.u8(), 1)]
        else:
            ops += [('r', 0, v), ('i', _imm(s, v), v)]
        return done(op)
    if b in (0x06, 0x0E, 0x16, 0x1E):
        ops.append(('s', b >> 3)); return done('push')
    if b in (0x07, 0x17, 0x1F):
        ops.append(('s', b >> 3)); return done('pop')
    if b in (0x27, 0x2F, 0x37, 0x3F):
        return done({0x27: 'daa', 0x2F: 'das', 0x37: 'aaa', 0x3F: 'aas'}[b])
    if 0x40 <= b < 0x60:
        ops.append(('r', b & 7, v))
        return done(['inc', 'dec', 'push', 'pop'][(b - 0x40) >> 3])
    if b == 0x60:
        return done('pushal' if v == 4 else 'pushaw')
    if b == 0x61:
        return done('popal' if v == 4 else 'popaw')
    if b == 0x62:
        mod, r, m = E(v * 2)
        if mod == 3:
            raise Refused('bound with a register')
        ops += [('r', r, v), m]; return done('bound')
    if b == 0x63:
        _, r, m = E(2); ops += [m, ('r', r, 2)]; return done('arpl')
    if b == 0x68:
        ops.append(('i', _imm(s, v), v)); return done('push')
    if b == 0x6A:
        ops.append(('i', _sx(s.u8(), 1, v), v)); return done('push')
    if b in (0x69, 0x6B):
        _, r, m = E(v)
        imm = _imm(s, v) if b == 0x69 else _sx(s.u8(), 1, v)
        ops += [('r', r, v), m, ('i', imm, v)]; return done('imul')
    if 0x6C <= b <= 0x6F:
        sz = 1 if b & 1 == 0 else v
        sfx = {1: 'b', 2: 'w', 4: 'd'}[sz]
        if b < 0x6E:
            ops += [('m', 0, 7, None, 0, 0, sz), ('r', 2, 2)]
            return done('ins' + sfx)
        ops += [('r', 2, 2), ('m', i.seg, 6, None, 0, 0, sz)]
        return done('outs' + sfx)
    if 0x70 <= b <= 0x7F:
        rel(1); return done('j' + CC[b & 15])
    if 0x80 <= b <= 0x83:
        sz = 1 if b in (0x80, 0x82) else v
        _, r, m = E(sz)
        imm = _imm(s, sz) if b != 0x83 else _sx(s.u8(), 1, v)
        ops += [m, ('i', imm, sz)]; return done(ALU[r])
    if b in (0x84, 0x85, 0x86, 0x87, 0x88, 0x89):
        sz = 1 if b & 1 == 0 else v
        _, r, m = E(sz)
        ops += [m, ('r', r, sz)]
        return done({0x84: 'test', 0x85: 'test', 0x86: 'xchg', 0x87: 'xchg'}.get(b, 'mov'))
    if b in (0x8A, 0x8B):
        sz = 1 if b == 0x8A else v
        _, r, m = E(sz); ops += [('r', r, sz), m]; return done('mov')
    if b == 0x8C:
        mod, r, m = E(2 if True else v)
        if r > 5:
            raise Refused('no segment register %d' % r)
        if mod == 3:
            m = ('r', m[1], v)
        ops += [m, ('s', r)]; return done('mov')
    if b == 0x8D:
        mod, r, m = E(0)
        if mod == 3:
            raise Refused('lea of a register')
        ops += [('r', r, v), m]; return done('lea')
    if b == 0x8E:
        mod, r, m = E(2)
        if r > 5 or r == 1:
            raise Refused('mov to segment register %d' % r)
        if mod == 3:
            m = ('r', m[1], v)      # only the low 16 bits are read; named at the operand size
        ops += [('s', r), m]; return done('mov')
    if b == 0x8F:
        _, r, m = E(v)
        if r != 0:
            raise Refused('8f /%d' % r)
        ops.append(m); return done('pop')
    if b == 0x90 and i.rep != 0xF3:
        return done('nop')
    if b == 0x90:
        return done('pause')
    if 0x91 <= b <= 0x97:
        ops += [('r', b & 7, v), ('r', 0, v)]; return done('xchg')
    if b == 0x98:
        return done('cwde' if v == 4 else 'cbw')
    if b == 0x99:
        return done('cdq' if v == 4 else 'cwd')
    if b in (0x9A, 0xEA):
        o = _imm(s, v); sel = s.u16()
        ops.append(('f', sel, o)); return done('lcall' if b == 0x9A else 'ljmp')
    if b == 0x9B:
        return done('wait')
    if b == 0x9C:
        return done('pushfd' if v == 4 else 'pushf')
    if b == 0x9D:
        return done('popfd' if v == 4 else 'popf')
    if b == 0x9E:
        return done('sahf')
    if b == 0x9F:
        return done('lahf')
    if 0xA0 <= b <= 0xA3:
        sz = 1 if b & 1 == 0 else v
        mo = ('m', i.seg, None, None, 0, _imm(s, asize), sz)
        ops += [('r', 0, sz), mo] if b < 0xA2 else [mo, ('r', 0, sz)]
        return done('mov')
    if 0xA4 <= b <= 0xAF and b not in (0xA8, 0xA9):
        sz = 1 if b & 1 == 0 else v
        sfx = {1: 'b', 2: 'w', 4: 'd'}[sz]
        name = {0xA4: 'movs', 0xA6: 'cmps', 0xAA: 'stos', 0xAC: 'lods', 0xAE: 'scas'}[b & ~1]
        src = ('m', i.seg, 6, None, 0, 0, sz)
        dst = ('m', 0, 7, None, 0, 0, sz)
        if name == 'movs':
            ops += [dst, src]
        elif name == 'cmps':
            ops += [src, dst]
        elif name == 'stos':
            ops += [dst, ('r', 0, sz)]
        elif name == 'lods':
            ops += [('r', 0, sz), src]
        else:
            ops += [('r', 0, sz), dst]
        return done(name + sfx)
    if b in (0xA8, 0xA9):
        sz = 1 if b == 0xA8 else v
        ops += [('r', 0, sz), ('i', _imm(s, sz), sz)]; return done('test')
    if 0xB0 <= b <= 0xB7:
        ops += [('r', b & 7, 1), ('i', s.u8(), 1)]; return done('mov')
    if 0xB8 <= b <= 0xBF:
        ops += [('r', b & 7, v), ('i', _imm(s, v), v)]; return done('mov')
    if b in (0xC0, 0xC1, 0xD0, 0xD1, 0xD2, 0xD3):
        sz = 1 if b & 1 == 0 else v
        _, r, m = E(sz)
        ops.append(m)
        if b in (0xC0, 0xC1):
            ops.append(('i', s.u8(), 1))
        elif b in (0xD0, 0xD1):
            ops.append(('i', 1, 1))
        else:
            ops.append(('r', 1, 1))
        return done(SHIFT[r])
    if b == 0xC2:
        ops.append(('i', s.u16(), 2)); return done('ret')
    if b == 0xC3:
        return done('ret')
    if b in (0xC4, 0xC5):
        mod, r, m = E(v + 2)
        if mod == 3:
            raise Refused('les/lds of a register')
        ops += [('r', r, v), m]; return done('les' if b == 0xC4 else 'lds')
    if b in (0xC6, 0xC7):
        sz = 1 if b == 0xC6 else v
        _, r, m = E(sz)
        if r != 0:
            raise Refused('%02x /%d' % (b, r))
        ops += [m, ('i', _imm(s, sz), sz)]; return done('mov')
    if b == 0xC8:
        a = s.u16(); n = s.u8()
        ops += [('i', a, 2), ('i', n, 1)]; return done('enter')
    if b == 0xC9:
        return done('leave')
    if b == 0xCA:
        ops.append(('i', s.u16(), 2)); return done('retf')
    if b == 0xCB:
        return done('retf')
    if b == 0xCC:
        return done('int3')
    if b == 0xCD:
        ops.append(('i', s.u8(), 1)); return done('int')
    if b == 0xCE:
        return done('into')
    if b == 0xCF:
        return done('iretd' if v == 4 else 'iret')
    if b in (0xD4, 0xD5):
        ops.append(('i', s.u8(), 1)); return done('aam' if b == 0xD4 else 'aad')
    if b == 0xD6:
        return done('salc')
    if b == 0xD7:
        ops.append(('m', i.seg, 3, None, 0, 0, 1)); return done('xlatb')
    if 0xD8 <= b <= 0xDF:
        # an x87 escape: `esc N, r/m` (N its six opcode bits), as a 386 with no coprocessor
        # runs it -- the ModRM read, nothing done (MAME's i386_escape)
        mod, reg, op = E(1)
        ops.append(('i', ((b & 7) << 3) | reg, 1))
        ops.append(op)
        return done('esc')
    if 0xE0 <= b <= 0xE3:
        rel(1)
        if b == 0xE3:
            return done('jecxz' if asize == 4 else 'jcxz')
        return done(['loopne', 'loope', 'loop'][b - 0xE0])
    if b in (0xE4, 0xE5):
        ops += [('r', 0, 1 if b == 0xE4 else v), ('i', s.u8(), 1)]; return done('in')
    if b in (0xE6, 0xE7):
        ops += [('i', s.u8(), 1), ('r', 0, 1 if b == 0xE6 else v)]; return done('out')
    if b == 0xE8:
        rel(v); return done('call')
    if b == 0xE9:
        rel(v); return done('jmp')
    if b == 0xEB:
        rel(1); return done('jmp')
    if b in (0xEC, 0xED):
        ops += [('r', 0, 1 if b == 0xEC else v), ('r', 2, 2)]; return done('in')
    if b in (0xEE, 0xEF):
        ops += [('r', 2, 2), ('r', 0, 1 if b == 0xEE else v)]; return done('out')
    if b == 0xF1:
        return done('int1')
    if b == 0xF4:
        return done('hlt')
    if b == 0xF5:
        return done('cmc')
    if b in (0xF6, 0xF7):
        sz = 1 if b == 0xF6 else v
        _, r, m = E(sz)
        ops.append(m)
        if r in (0, 1):
            ops.append(('i', _imm(s, sz), sz))
            return done('test')
        return done(['test', 'test', 'not', 'neg', 'mul', 'imul', 'div', 'idiv'][r])
    if 0xF8 <= b <= 0xFD:
        return done(['clc', 'stc', 'cli', 'sti', 'cld', 'std'][b - 0xF8])
    if b == 0xFE:
        _, r, m = E(1)
        if r > 1:
            raise Refused('fe /%d' % r)
        ops.append(m); return done('inc' if r == 0 else 'dec')
    if b == 0xFF:
        mod, r, m = E(v)
        if r == 7:
            raise Refused('ff /7')
        if r in (3, 5):
            if mod == 3:
                raise Refused('far transfer through a register')
            ops.append(m[:6] + (v + 2,))
            return done('lcall' if r == 3 else 'ljmp')
        ops.append(m)
        return done(['inc', 'dec', 'call', None, 'jmp', None, 'push'][r])
    if b == 0x0F:
        return _two(s, i, done, rel, E, v)
    raise Refused('opcode %02x' % b)


def _two(s, i, done, rel, E, v):
    b = s.u8()
    ops = i.ops
    if b == 0x00:
        mod, r, m = E(2)
        if r > 5:
            raise Refused('0f 00 /%d' % r)
        if mod == 3 and r < 2:
            m = ('r', m[1], v)
        ops.append(m)
        return done(['sldt', 'str', 'lldt', 'ltr', 'verr', 'verw'][r])
    if b == 0x01:
        mod, r, m = E(6)
        if r in (5, 7) or (mod == 3 and r != 4 and r != 6):
            raise Refused('0f 01 /%d' % r)
        if r in (4, 6):
            m = m if mod == 3 else m[:6] + (2,)
            if mod == 3 and r == 4:
                m = ('r', m[1], v)
            elif mod == 3:
                m = ('r', m[1], 2)
        ops.append(m)
        return done(['sgdt', 'sidt', 'lgdt', 'lidt', 'smsw', None, 'lmsw'][r])
    if b in (0x02, 0x03):
        _, r, m = E(2)
        if m[0] == 'r':
            m = ('r', m[1], v)
        ops += [('r', r, v), m]; return done('lar' if b == 2 else 'lsl')
    if b == 0x06:
        return done('clts')
    if b in (0x20, 0x21, 0x22, 0x23, 0x24, 0x26):
        x = s.u8()
        r, rm = (x >> 3) & 7, x & 7
        kind = {0x20: 'c', 0x22: 'c', 0x21: 'd', 0x23: 'd', 0x24: 't', 0x26: 't'}[b]
        if kind == 'c' and r not in (0, 2, 3):
            raise Refused('cr%d' % r)
        if kind == 't' and r not in (6, 7):
            raise Refused('tr%d' % r)
        if b in (0x20, 0x21, 0x24):
            ops += [('r', rm, 4), (kind, r)]
        else:
            ops += [(kind, r), ('r', rm, 4)]
        return done('mov')
    if 0x80 <= b <= 0x8F:
        rel(v); return done('j' + CC[b & 15])
    if 0x90 <= b <= 0x9F:
        _, r, m = E(1)
        ops.append(m); return done('set' + CC[b & 15])
    if b in (0xA0, 0xA8):
        ops.append(('s', 4 if b == 0xA0 else 5)); return done('push')
    if b in (0xA1, 0xA9):
        ops.append(('s', 4 if b == 0xA1 else 5)); return done('pop')
    if b in (0xA3, 0xAB, 0xB3, 0xBB):
        _, r, m = E(v); ops += [m, ('r', r, v)]
        return done({0xA3: 'bt', 0xAB: 'bts', 0xB3: 'btr', 0xBB: 'btc'}[b])
    if b in (0xA4, 0xA5, 0xAC, 0xAD):
        _, r, m = E(v); ops += [m, ('r', r, v)]
        ops.append(('i', s.u8(), 1) if b in (0xA4, 0xAC) else ('r', 1, 1))
        return done('shld' if b < 0xAC else 'shrd')
    if b == 0xAF:
        _, r, m = E(v); ops += [('r', r, v), m]; return done('imul')
    if b in (0xB2, 0xB4, 0xB5):
        mod, r, m = E(v + 2)
        if mod == 3:
            raise Refused('lss/lfs/lgs of a register')
        ops += [('r', r, v), m]; return done({0xB2: 'lss', 0xB4: 'lfs', 0xB5: 'lgs'}[b])
    if b in (0xB6, 0xB7, 0xBE, 0xBF):
        _, r, m = E(1 if b in (0xB6, 0xBE) else 2)
        ops += [('r', r, v), m]; return done('movzx' if b < 0xBE else 'movsx')
    if b == 0xBA:
        _, r, m = E(v)
        if r < 4:
            raise Refused('0f ba /%d' % r)
        ops += [m, ('i', s.u8(), 1)]; return done(['bt', 'bts', 'btr', 'btc'][r - 4])
    if b in (0xBC, 0xBD):
        _, r, m = E(v); ops += [('r', r, v), m]; return done('bsf' if b == 0xBC else 'bsr')
    later = {0x05: 'syscall', 0x07: 'sysret', 0x08: 'invd', 0x09: 'wbinvd', 0x0B: 'ud2', 0x31: 'rdtsc',
             0xA2: 'cpuid', 0xB0: 'cmpxchg', 0xB1: 'cmpxchg', 0xC0: 'xadd', 0xC1: 'xadd', 0xC7: 'cmpxchg8b'}
    if b in later:
        raise Refused('%s: not a 386 instruction' % later[b])
    if 0xC8 <= b <= 0xCF:
        raise Refused('bswap: not a 386 instruction')
    if 0x40 <= b <= 0x4F:
        raise Refused('cmov: not a 386 instruction')
    raise Refused('opcode 0f %02x' % b)


STRING_OPS = {p + s for p in ('movs', 'cmps', 'stos', 'lods', 'scas', 'ins', 'outs') for s in 'bwd'}
FAR_POINTER_OPS = ('lds', 'les', 'lfs', 'lgs', 'lss', 'lcall', 'ljmp', 'bound')
# LOCK is allowed only on these, with a memory destination (anything else raises #UD)
LOCKABLE = ('add', 'or', 'adc', 'sbb', 'and', 'sub', 'xor', 'not', 'neg', 'inc', 'dec',
            'xchg', 'bts', 'btr', 'btc')


# --- text, as capstone writes it ---------------------------------------------
SIZE_PTR = {1: 'byte ptr ', 2: 'word ptr ', 4: 'dword ptr ', 6: 'fword ptr ', 8: 'qword ptr ', 0: ''}


def hexs(v):
    return '%d' % v if v < 10 else '0x%x' % v


def format_op(o, i):
    k = o[0]
    if k == 'r':
        return reg_name(o[1], o[2])
    if k == 's':
        return SREG[o[1]]
    if k in 'cdt':
        return {'c': 'cr', 'd': 'dr', 't': 'tr'}[k] + str(o[1])
    if k == 'i':
        return hexs(o[1])
    if k == 'j':
        return hexs(o[1])
    if k == 'f':
        return '%s:%s' % (hexs(o[1]), hexs(o[2]))
    _, seg, base, index, scale, disp, size = o
    names = REG32 if i.asize == 4 else REG16
    parts = []
    if base is not None:
        parts.append(names[base])
    if index is not None:
        parts.append('%s*%d' % (names[index], scale) if i.asize == 4 else names[index])
    text = ' + '.join(parts)
    mask = 0xFFFFFFFF if i.asize == 4 else 0xFFFF
    if not parts:
        text = hexs(disp)
    elif disp:
        neg = disp & (1 << (i.asize * 8 - 1))
        text += (' - ' + hexs((-disp) & mask)) if neg else (' + ' + hexs(disp))
    segtxt = (SREG[seg] + ':') if seg is not None else ''
    return '%s%s[%s]' % (SIZE_PTR.get(size, ''), segtxt, text)


def format_ops(i):
    return ', '.join(format_op(o, i) for o in i.ops)


# --- the check against capstone ------------------------------------------------
def _cs():
    import capstone
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    md.detail = True
    return md


def _cs_ops(c):
    """capstone's operands in this module's form."""
    from capstone import x86_const as X
    out = []
    names = {n: k for k, n in enumerate(REG32)}
    names.update({n: k for k, n in enumerate(REG16)})
    for o in c.operands:
        if o.type == X.X86_OP_REG:
            n = c.reg_name(o.reg)
            if n in SREG:
                out.append(('s', SREG.index(n)))
            elif n in REG8:
                out.append(('r', REG8.index(n), 1))
            elif n in REG16:
                out.append(('r', REG16.index(n), 2))
            elif n in REG32:
                out.append(('r', REG32.index(n), 4))
            elif n[:2] in ('cr', 'dr', 'tr'):
                out.append((n[0], int(n[2:])))
            else:
                out.append(('?', n))
        elif o.type == X.X86_OP_IMM:
            out.append(('i', o.imm & ((1 << (8 * o.size)) - 1) if o.size else o.imm, o.size))
        elif o.type == X.X86_OP_MEM:
            m = o.mem
            seg = SREG.index(c.reg_name(m.segment)) if m.segment else None
            base = names[c.reg_name(m.base)] if m.base else None
            index = names[c.reg_name(m.index)] if m.index else None
            out.append(('m', seg, base, index, m.scale if index is not None else 0,
                        m.disp & 0xFFFFFFFF, o.size))
    return out


def _ours_for_cs(i):
    """Our operands as capstone reports them (where its conventions differ)."""
    out = []
    if i.op == 'xlatb':
        return out                                  # capstone gives xlatb no operand
    if i.op == 'enter':                             # capstone sign-extends both
        return [('i', _sx(o[1], o[2], 4), 4) for o in i.ops]
    for o in i.ops:
        if o[0] == 'f':
            out += [('i', o[1], 2), ('i', o[2], i.osize)]
        elif o[0] == 'j':
            out.append(('i', o[1], i.osize))
        elif o[0] == 'm':
            disp = o[5]
            if i.asize == 2:
                # capstone sign-extends a 16-bit displacement, or not; a 16-bit address
                # wraps at 64 KB, so the two are compared modulo 64 KB
                disp &= 0xFFFF
            out.append(o[:5] + (disp, o[6]))
        else:
            out.append(o)
    return out


def compare(i, c):
    """'' when this decoding and capstone's agree, else what differs."""
    if c.size != i.size:
        return 'length %d, capstone %d' % (i.size, c.size)
    if i.op == 'esc':               # capstone reads the x87 instruction; the address must agree
        if not c.mnemonic.startswith('f'):
            return 'esc, capstone %s' % c.mnemonic
        cut = (lambda o: o[:5] + (o[5] & 0xFFFF,)) if i.asize == 2 else (lambda o: o[:6])
        mine = [cut(o) for o in _ours_for_cs(i) if o[0] == 'm']
        theirs = [cut(o) for o in _cs_ops(c) if o[0] == 'm']
        if mine != theirs:
            return 'esc operand %s, capstone %s' % (mine, theirs)
        return ''
    cm = c.mnemonic
    # later CPUs' readings of prefixes a 386 ignores: MPX, CET, TSX, and AMD's "rep ret"
    for p in ('bnd ', 'notrack ', 'xacquire ', 'xrelease '):
        if cm.startswith(p):
            cm = cm[len(p):]
    if cm == 'repz ret':
        cm = 'ret'
    om = i.op
    if i.rep is not None and om in STRING_OPS and om[:4] not in ('cmps', 'scas'):
        om = ('rep ' if i.rep == 0xF3 else 'repne ') + om    # both repeat on ECX; capstone names F2 repne
    elif i.rep is not None and om in STRING_OPS:
        om = ('repe ' if i.rep == 0xF3 else 'repne ') + om
    if i.lock:
        om = 'lock ' + om
    if cm != om and not (om in ('lcall', 'ljmp') and cm == om[1:] and i.ops[0][0] == 'm'):
        return 'mnemonic %s, capstone %s' % (om, cm)   # capstone calls FF /3, /5 call and jmp
    mine, theirs = _ours_for_cs(i), _cs_ops(c)
    if len(mine) != len(theirs):
        return 'operands %s, capstone %s' % (mine, theirs)
    for a, b in zip(mine, theirs):
        if a[0] == 'm' and b[0] == 'm' and i.asize == 2:
            b = b[:5] + (b[5] & 0xFFFF,) + b[6:]
        if a[0] == 'm' and b[0] == 'm' and a[:6] == b[:6] and (
                a[6] == 0 or (a[6] in (4, 6) and i.op in FAR_POINTER_OPS)):
            continue                # LEA's operand has no size; capstone says 4 (and 4 for m16:32)
        if a[0] == 'i' and b[0] == 'i':
            if a[1] & ((1 << (8 * b[2])) - 1 if b[2] else ~0) != b[1] & ((1 << (8 * b[2])) - 1 if b[2] else ~0):
                return 'immediate %s, capstone %s' % (a, b)
            continue
        if a[0] == 'f' and b[0] == 'i':
            continue
        if a != b:
            return 'operand %s, capstone %s' % (a, b)
    return ''


def check_range(data, base, addrs, md=None, quiet=False):
    """Decode at each address with both; returns (agree, differ list, refused-by-us dict, refused-by-capstone)."""
    md = md or _cs()
    agree, differ = 0, []
    refused, cs_refused = collections.Counter(), 0
    for a in addrs:
        off = a - base
        c = next(md.disasm(data[off:off + 16], a), None)
        try:
            i = decode(data, off, a)
        except Refused as e:
            if c is not None:
                refused['%s (capstone: %s)' % (str(e).split(':')[0], c.mnemonic)] += 1
            else:
                cs_refused += 1
            continue
        if c is None:
            if i.op == 'esc':
                refused['esc with no x87 reading (capstone refuses it; a 386 runs it)'] += 1
            else:
                differ.append((a, 'capstone refuses %s' % i))
            continue
        d = compare(i, c)
        if d:
            differ.append((a, '%s: %s %s | %s %s  [%s]' % (d, i.op, format_ops(i), c.mnemonic, c.op_str,
                                                           i.raw.hex(' '))))
        else:
            agree += 1
    return agree, differ, refused, cs_refused


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('file', nargs='?')
    ap.add_argument('--raw', action='store_true')
    ap.add_argument('--base', default='0')
    ap.add_argument('--far-table', metavar='FROM:TO')
    ap.add_argument('--seed', default='')
    ap.add_argument('--symbol-seeds', metavar='END', help='every symbol below END (hex) is an entry too')
    ap.add_argument('--dis', metavar='ADDR')
    ap.add_argument('-n', type=int, default=20)
    ap.add_argument('--check', action='store_true', help='every reached instruction, against capstone')
    ap.add_argument('--sweep', type=int, metavar='N', help='N random instructions, against capstone')
    a = ap.parse_args(argv)
    if a.sweep:
        rnd = random.Random(1)
        md = _cs()
        agree, differ, refused, csr = 0, [], collections.Counter(), 0
        for k in range(a.sweep):
            buf = bytes(rnd.getrandbits(8) for _ in range(16))
            g, d, r, c = check_range(buf, 0, [0], md)
            agree += g; differ += d; refused.update(r); csr += c
        esc = sum(v for k, v in refused.items() if k.startswith('esc'))
        print('%d agree, %d differ, %d refused by both, %d refused here and decoded by capstone, '
              '%d escapes capstone has no x87 reading for' % (agree, len(differ), csr,
                                                               sum(refused.values()) - esc, esc))
        kinds = collections.Counter()
        for kv in refused.items():
            if not kv[0].startswith('esc'):
                kinds[kv[0].split(' ')[0]] += kv[1]
        print('  refused here, by kind:', dict(kinds.most_common()))
        cats = collections.Counter(d.split(':')[0].split(' ')[0] for _, d in differ)
        print('  differ, by kind:', dict(cats))
        for a_, d in differ[:40]:
            print('  %s' % d)
        return
    from . import exp as expmod
    from . import x86map
    data = open(a.file, 'rb').read()
    seeds = [int(x, 16) for x in a.seed.split(',') if x]
    if a.raw:
        image, base = data, int(a.base, 16)
        if a.far_table:
            lo, hi = (int(x, 16) for x in a.far_table.split(':'))
            seeds += [int.from_bytes(image[o:o + 4], 'little') for o in range(lo, hi, 8)
                      if int.from_bytes(image[o:o + 4], 'little')]
        entry, seeds = seeds[0], seeds[1:]
    else:
        e = expmod.Exp(data)
        image, base, entry = e.image, e.base_offset, e.eip
        if a.symbol_seeds:
            seeds += [off for off, _, _ in e.symbols if off < int(a.symbol_seeds, 16) and off != entry]
    if a.dis:
        p = int(a.dis, 16)
        for _ in range(a.n):
            try:
                i = decode(image, p - base, p)
            except Refused as ex:
                print('  %06x  refused: %s' % (p, ex)); break
            print('  %06x  %-22s %s%s %s' % (p, i.raw.hex(), '' if i.rep is None else
                                             ('rep ' if i.rep == 0xF3 else 'repne '), i.op, format_ops(i)))
            p += i.size
        return
    if a.check:
        m = x86map.Map(image, base, entry, seeds, None if a.raw or not e.symbols else e.table_end)
        agree, differ, refused, csr = check_range(image, base, sorted(m.insn))
        print('%d instructions reached: %d agree with capstone, %d differ, %d refused here'
              % (len(m.insn), agree, len(differ), sum(refused.values())))
        for kv in refused.most_common():
            print('  refused: %s x%d' % kv)
        for a_, d in differ[:60]:
            print('  %06x %s' % (a_, d))


if __name__ == '__main__':
    main()
