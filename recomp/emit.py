"""386 to C++: a recompiled function per discovered function.

Each instruction becomes a few lines of C++ over `runtime/x86.h`, with the
semantics of `i386emu`'s method of the same name (the self-test holds the
two together): registers and flags in the X86Cpu, linear memory as
segment base + offset, the machine through the runtime's services.

Control flow:

* a function's body is `discover.Program.body` -- what its entry reaches by
  falling through and jumping; a jump is a `goto` to a label in the body, and
  a backward jump is a safe point (`X86_POLL`) first;
* a call pushes its return address and calls the callee's C++ function;
  after it, `X86_RET` checks the return came back here (a return further out
  unwinds the C++ calls until a frame expects it);
* a jump table is a `switch` on the entry read from the table, so a table
  rewritten at run time still lands right or reaches the runtime; other
  indirect transfers go to the runtime (`x86_call`, `x86_jump`);
* `ret` pops EIP into the X86Cpu and returns.

Time: each block pays its instructions' 386 clocks at its start
(`X86_TICK`), from `clocks()`: the 80386's datasheet figures, rounded to one
number per form (a taken and a not-taken branch alike, a multiply at its
middle) -- approximate, to be held against the oracle.
"""
from .. import i386

CC = {cc: k for k, cc in enumerate(i386.CC)}
SHIFTS = {'rol': 0, 'ror': 1, 'rcl': 2, 'rcr': 3, 'shl': 4, 'shr': 5, 'sal': 6, 'sar': 7}
STRING = {'movs': 0, 'cmps': 1, 'stos': 2, 'lods': 3, 'scas': 4, 'ins': 5, 'outs': 6}
BCD = {'daa': 0, 'das': 1, 'aaa': 2, 'aas': 3, 'aam': 4, 'aad': 5}
SYSTEM = {'hlt', 'arpl', 'sldt', 'str', 'lldt', 'ltr', 'verr', 'verw', 'sgdt', 'sidt', 'lgdt', 'lidt',
          'smsw', 'lmsw', 'lar', 'lsl', 'clts', 'bound'}
TRANSFERS_OUT = {'ret', 'retf', 'iret', 'iretd', 'jmp', 'ljmp', 'hlt'}


def hx(v):
    return '0x%Xu' % (v & 0xFFFFFFFF)


def clocks(i):
    """The 386's clocks for an instruction, roughly (its datasheet, one figure a form)."""
    op = i.op
    mem = any(o[0] == 'm' for o in i.ops)
    if op == 'mov':
        return 4 if mem and i.ops[1][0] == 'm' else 2
    if op in i386.ALU or op == 'test':
        return 7 if mem and i.ops[0][0] == 'm' else 6 if mem else 2
    if op in ('inc', 'dec', 'not', 'neg'):
        return 6 if mem else 2
    if op in ('push',):
        return 5 if mem else 2
    if op in ('pop',):
        return 5 if mem else 4
    if op in ('lea', 'cwde', 'cbw', 'cwd', 'cdq', 'lahf', 'sahf', 'clc', 'stc', 'cmc', 'cld', 'std',
              'cli', 'sti', 'nop', 'salc'):
        return 2
    if op in ('movzx', 'movsx'):
        return 6 if mem else 3
    if op == 'xchg':
        return 5 if mem else 3
    if op.startswith('j') or op.startswith('loop'):
        return 7 if op in ('jmp',) else 11 if op.startswith('loop') else 5
    if op in ('call',):
        return 7
    if op in ('ret',):
        return 10
    if op in ('shl', 'sal', 'shr', 'sar', 'rol', 'ror', 'rcl', 'rcr', 'shld', 'shrd'):
        return 7 if mem else 3
    if op in ('mul', 'imul'):
        return 22
    if op in ('div', 'idiv'):
        return 40
    if op.startswith('set'):
        return 5 if mem else 4
    if op in ('bt', 'bts', 'btr', 'btc'):
        return 8 if mem else 3
    if op in ('bsf', 'bsr'):
        return 10
    if op in i386.STRING_OPS:
        return 7
    if op in ('pushal', 'pushaw'):
        return 18
    if op in ('popal', 'popaw'):
        return 24
    if op in ('int', 'int3', 'into', 'int1'):
        return 37
    if op in ('in', 'out'):
        return 12
    return 6


class Emitter:
    def __init__(self, prog, ns):
        self.p, self.ns = prog, ns

    # --- operands --------------------------------------------------------
    def off(self, i, o):
        _, seg, base, index, scale, disp, _ = o
        parts = []
        if i.asize == 4:
            if base is not None:
                parts.append('c.r[%d]' % base)
            if index is not None:
                parts.append('c.r[%d]' % index + ('' if scale == 1 else ' * %d' % scale))
            if disp or not parts:
                parts.append(hx(disp))
            return '(uint32_t)(%s)' % ' + '.join(parts)
        if base is not None:
            parts.append('(c.r[%d] & 0xFFFF)' % base)
        if index is not None:
            parts.append('(c.r[%d] & 0xFFFF)' % index)
        if disp or not parts:
            parts.append(hx(disp))
        return '((%s) & 0xFFFFu)' % ' + '.join(parts)

    def seg(self, i, o):
        if o[1] is not None:
            return o[1]
        base = o[2]
        if i.asize == 4:
            return 2 if base in (4, 5) else 3
        return 2 if base == 5 else 3

    def lin(self, i, o):
        return '(c.sbase[%d] + %s)' % (self.seg(i, o), self.off(i, o))

    def rd(self, i, o, size=None):
        k = o[0]
        if k == 'r':
            n = size or o[2]
            return 'c.r[%d]' % o[1] if n == 4 else '(c.r[%d] & 0xFFFFu)' % o[1] if n == 2 else 'g8(c, %d)' % o[1]
        if k == 'i':
            return hx(o[1])
        if k == 's':
            return '(uint32_t)c.sreg[%d]' % o[1]
        n = size or o[6]
        return 'rd%d(%s)' % (8 * n, self.lin(i, o))

    def wr(self, i, o, v, size=None, lin=None):
        k = o[0]
        if k == 'r':
            n = size or o[2]
            if n == 4:
                return 'c.r[%d] = %s;' % (o[1], v)
            return '%s(c, %d, %s);' % ('s16' if n == 2 else 's8', o[1], v)
        n = size or o[6]
        return 'wr%d(%s, %s);' % (8 * n, lin or self.lin(i, o), v)

    @staticmethod
    def size(o, default):
        return o[2] if o[0] == 'r' else o[6] if o[0] == 'm' else default

    # --- one function -------------------------------------------------------
    def function(self, f):
        p = self.p
        body = sorted(p.body(f))
        inbody = self.inbody = set(body)
        labels = set()
        for a in body:
            i = p.insn[a]
            o0 = i.ops[0] if i.ops else None
            if o0 is not None and o0[0] == 'j' and i.op != 'call':
                labels.add(o0[1])
            if a in p.tables and i.op == 'jmp':
                labels.update(x for x in p.tables[a][1] if x in inbody)
        # where the emitted order breaks a fall-through, a goto: its target is a label too
        for k, a in enumerate(body):
            i = p.insn[a]
            nxt = a + i.size
            if not self.ends(i) and (k + 1 >= len(body) or body[k + 1] != nxt):
                labels.add(nxt)
        out = ['void f_%08x(X86Cpu& c) {' % f]
        if body and body[0] != f:
            out.append('    goto L_%x;' % f)
            labels.add(f)
        tick_pending = True
        for k, a in enumerate(body):
            i = p.insn[a]
            if a in labels:
                out.append('L_%x:' % a)
                tick_pending = True
            if tick_pending:
                n = 0
                for b in body[k:]:
                    if b != a and b in labels:
                        break
                    n += clocks(p.insn[b])
                    if self.transfers(p.insn[b]):
                        break
                out.append('    X86_TICK(c, %d);' % n)
                tick_pending = False
            out.append('    // %06x  %s %s' % (a, i.op, i386.format_ops(i)))
            out.append('    X86_AT(c, 0x%xu);' % a)
            out.extend('    ' + line for line in self.insn(i, inbody, labels))
            if self.transfers(i):
                tick_pending = True
            nxt = a + i.size
            if not self.ends(i) and (k + 1 >= len(body) or body[k + 1] != nxt):
                out.append('    ' + self.go(a, nxt))
        out.append('    x86_fault(c, 0x%xu, "ran off the end of a function");' % f)
        out.append('}')
        return '\n'.join(out) + '\n'

    def ends(self, i):
        """No fall-through after it."""
        return i.op in TRANSFERS_OUT or (i.op == 'jmp')

    def transfers(self, i):
        return self.ends(i) or (i.ops and i.ops[0][0] == 'j') or i.op in ('call', 'lcall', 'int')

    # --- one instruction ------------------------------------------------------
    def insn(self, i, inbody, labels):
        op, ops = i.op, i.ops
        a, nxt = i.addr, (i.addr + i.size) & 0xFFFFFFFF
        n = self.size(ops[0], i.osize) if ops else i.osize
        L = []

        def rmw(fn):
            """dst = fn(old) with the address computed once."""
            d = ops[0]
            if d[0] == 'm':
                L.append('{ uint32_t ea = %s; uint32_t v = rd%d(ea); %s }' % (
                    self.lin(i, d), 8 * n, fn('v', 'ea')))
            else:
                L.append('{ uint32_t v = %s; %s }' % (self.rd(i, d), fn('v', None)))

        if op in i386.ALU:
            b = self.rd(i, ops[1])
            if ops[1][0] == 'r' and ops[1][2] != n:
                b = '(%s & 0x%Xu)' % (b, (1 << (8 * n)) - 1)
            calc = {
                'add': 'x86_add(c, %%s, %s, 0, %d)' % (b, n),
                'adc': 'x86_add(c, %%s, %s, c.fl & 1, %d)' % (b, n),
                'sub': 'x86_sub(c, %%s, %s, 0, %d)' % (b, n),
                'cmp': 'x86_sub(c, %%s, %s, 0, %d)' % (b, n),
                'sbb': 'x86_sub(c, %%s, %s, c.fl & 1, %d)' % (b, n),
                'and': 'x86_logic(c, %%s & %s, %d)' % (b, n),
                'or': 'x86_logic(c, %%s | %s, %d)' % (b, n),
                'xor': 'x86_logic(c, %%s ^ %s, %d)' % (b, n),
            }[op]
            if op == 'cmp':
                L.append('(void)%s;' % (calc % self.rd(i, ops[0])))
            else:
                rmw(lambda v, ea: self.wr(i, ops[0], calc % v, lin=ea))
            return L
        if op == 'test':
            L.append('(void)x86_logic(c, %s & %s, %d);' % (self.rd(i, ops[0]), self.rd(i, ops[1]), n))
            return L
        if op.startswith('set') and op[3:] in CC:
            L.append(self.wr(i, ops[0], 'x86_cc(c, %d) ? 1u : 0u' % CC[op[3:]]))
            return L
        if op.startswith('j') and op[1:] in CC:
            t = ops[0][1]
            L.append('if (x86_cc(c, %d)) { %s }' % (CC[op[1:]], self.go(a, t)))
            return L
        if op == 'mov':
            d, s = ops
            if d[0] == 's':
                L.append('x86_load_seg(c, %d, %s & 0xFFFFu);' % (d[1], self.rd(i, s, 2)))
            elif s[0] == 's':
                L.append(self.wr(i, d, '(uint32_t)c.sreg[%d]' % s[1], 2 if d[0] == 'm' else d[2]))
            elif d[0] in 'cdt' or s[0] in 'cdt':
                L.append('x86_system(c, 0x%xu, "mov to or from a control, debug or test register");' % a)
            else:
                L.append(self.wr(i, d, self.rd(i, s)))
            return L
        if op == 'movzx':
            L.append(self.wr(i, ops[0], self.rd(i, ops[1])))
            return L
        if op == 'movsx':
            sz = self.size(ops[1], 1)
            L.append(self.wr(i, ops[0], '(uint32_t)(int32_t)(%s)%s' % (
                'int8_t' if sz == 1 else 'int16_t', self.rd(i, ops[1]))))
            return L
        if op == 'lea':
            L.append(self.wr(i, ops[0], self.off(i, ops[1])))
            return L
        if op == 'xchg':
            x, y = ops
            if x[0] == 'm':
                x, y = y, x
            if y[0] == 'm':
                L.append('{ uint32_t ea = %s; uint32_t t = rd%d(ea); wr%d(ea, %s); %s }' % (
                    self.lin(i, y), 8 * n, 8 * n, self.rd(i, x), self.wr(i, x, 't')))
            else:
                L.append('{ uint32_t t = %s; %s %s }' % (self.rd(i, x), self.wr(i, x, self.rd(i, y)),
                                                        self.wr(i, y, 't')))
            return L
        if op in ('nop', 'wait', 'pause'):
            return L
        if op == 'push':
            o = ops[0]
            push = 'push32' if i.osize == 4 else 'push16'
            if o[0] == 's':
                L.append('%s(c, c.sreg[%d]);' % (push, o[1]))
            else:
                L.append('{ uint32_t v = %s; %s(c, v); }' % (self.rd(i, o, i.osize if o[0] != 'i' else None), push))
            return L
        if op == 'pop':
            o = ops[0]
            pop = 'pop32' if i.osize == 4 else 'pop16'
            if o[0] == 's':
                L.append('x86_load_seg(c, %d, %s(c) & 0xFFFFu);' % (o[1], pop))
            else:
                L.append('{ uint32_t v = %s(c); %s }' % (pop, self.wr(i, o, 'v', i.osize)))
            return L
        if op in ('pushal', 'pushaw'):
            w = 4 if op == 'pushal' else 2
            push = 'push32' if w == 4 else 'push16'
            L.append('{ uint32_t sp = c.r[4]; for (int k = 0; k < 8; ++k) %s(c, k == 4 ? sp : c.r[k]); }' % push)
            return L
        if op in ('popal', 'popaw'):
            if op == 'popal':
                L.append('for (int k = 7; k >= 0; --k) { uint32_t v = pop32(c); if (k != 4) c.r[k] = v; }')
            else:
                L.append('for (int k = 7; k >= 0; --k) { uint32_t v = pop16(c); if (k != 4) s16(c, k, v); }')
            return L
        if op == 'pushfd':
            L.append('push32(c, c.fl & 0x00FCFFFFu);')
            return L
        if op == 'pushf':
            L.append('push16(c, c.fl & 0xFFFFu);')
            return L
        if op in ('popfd', 'popf'):
            L.append('x86_popfd(c, %d);' % (4 if op == 'popfd' else 2))
            return L
        if op == 'lahf':
            L.append('s8(c, 4, (c.fl & 0xD5u) | 2u);')
            return L
        if op == 'sahf':
            L.append('c.fl = (c.fl & ~0xD5u) | (g8(c, 4) & 0xD5u);')
            return L
        if op == 'cbw':
            L.append('s16(c, 0, (uint32_t)(int32_t)(int8_t)c.r[0]);')
            return L
        if op == 'cwde':
            L.append('c.r[0] = (uint32_t)(int32_t)(int16_t)c.r[0];')
            return L
        if op == 'cwd':
            L.append('s16(c, 2, (c.r[0] & 0x8000u) ? 0xFFFFu : 0u);')
            return L
        if op == 'cdq':
            L.append('c.r[2] = (c.r[0] & 0x80000000u) ? 0xFFFFFFFFu : 0u;')
            return L
        if op == 'xlatb':
            seg = ops[0][1] if ops[0][1] is not None else 3
            mask = '0xFFFFFFFFu' if i.asize == 4 else '0xFFFFu'
            L.append('s8(c, 0, rd8(c.sbase[%d] + ((%s + (c.r[0] & 0xFF)) & %s)));' % (
                seg, 'c.r[3]' if i.asize == 4 else '(c.r[3] & 0xFFFF)', mask))
            return L
        if op in ('les', 'lds', 'lss', 'lfs', 'lgs'):
            sr = {'les': 0, 'lds': 3, 'lss': 2, 'lfs': 4, 'lgs': 5}[op]
            w = i.osize
            L.append('{ uint32_t ea = %s; uint32_t o = rd%d(ea); uint32_t s = rd16(ea + %d); '
                     'x86_load_seg(c, %d, s); %s }' % (self.lin(i, ops[1]), 8 * w, w, sr, self.wr(i, ops[0], 'o')))
            return L
        if op in ('inc', 'dec'):
            rmw(lambda v, ea: self.wr(i, ops[0], 'x86_%s(c, %s, %d)' % (op, v, n), lin=ea))
            return L
        if op == 'neg':
            rmw(lambda v, ea: self.wr(i, ops[0], 'x86_neg(c, %s, %d)' % (v, n), lin=ea))
            return L
        if op == 'not':
            rmw(lambda v, ea: self.wr(i, ops[0], '~%s & 0x%Xu' % (v, (1 << (8 * n)) - 1), lin=ea))
            return L
        if op == 'mul':
            L.append('x86_mul(c, %s, %d);' % (self.rd(i, ops[0]), n))
            return L
        if op == 'imul':
            if len(ops) == 1:
                L.append('x86_imul1(c, %s, %d);' % (self.rd(i, ops[0]), n))
            elif len(ops) == 2:
                m = ops[0][2]
                L.append(self.wr(i, ops[0], 'x86_imul2(c, %s, %s, %d)' % (self.rd(i, ops[0]), self.rd(i, ops[1]), m)))
            else:
                m = ops[0][2]
                L.append(self.wr(i, ops[0], 'x86_imul2(c, %s, %s, %d)' % (self.rd(i, ops[1]), hx(ops[2][1]), m)))
            return L
        if op in ('div', 'idiv'):
            L.append('x86_%s(c, %s, %d, 0x%xu);' % (op, self.rd(i, ops[0]), n, a))
            return L
        flagops = {'cmc': 'c.fl ^= X86_CF;', 'clc': 'c.fl &= ~X86_CF;', 'stc': 'c.fl |= X86_CF;',
                   'cld': 'c.fl &= ~X86_DF;', 'std': 'c.fl |= X86_DF;', 'cli': 'c.fl &= ~X86_IF;',
                   'sti': 'c.fl |= X86_IF;', 'salc': 's8(c, 0, (c.fl & X86_CF) ? 0xFFu : 0u);'}
        if op in flagops:
            L.append(flagops[op])
            return L
        if op in BCD:
            L.append('x86_bcd(c, %d, %s, 0x%xu);' % (BCD[op], hx(ops[0][1]) if ops else '0u', a))
            return L
        if op in ('bt', 'bts', 'btr', 'btc'):
            how = {'bt': 0, 'bts': 1, 'btr': 2, 'btc': 3}[op]
            o, b = ops
            if o[0] == 'r':
                L.append('x86_bitop(c, %d, 0, false, %s, %d, c.r, %d);' % (how, self.rd(i, b), n, o[1]))
            else:
                L.append('x86_bitop(c, %d, %s, %s, %s, %d, nullptr, 0);' % (
                    how, self.lin(i, o), 'true' if b[0] == 'r' else 'false', self.rd(i, b), n))
            return L
        if op in ('bsf', 'bsr'):
            m = ops[0][2]
            L.append(self.wr(i, ops[0], 'x86_%s(c, %s, %s)' % (op, self.rd(i, ops[1], m), self.rd(i, ops[0]))))
            return L
        if op in SHIFTS:
            cnt = self.rd(i, ops[1])
            rmw(lambda v, ea: 'bool w; uint32_t r = x86_shift(c, %d, %s, %s, %d, w); if (w) %s' % (
                SHIFTS[op], v, cnt, n, self.wr(i, ops[0], 'r', lin=ea)))
            return L
        if op in ('shld', 'shrd'):
            src, cnt = self.rd(i, ops[1]), self.rd(i, ops[2])
            rmw(lambda v, ea: 'bool w; uint32_t r = x86_double(c, %s, %s, %s, %s, %d, w); if (w) %s' % (
                'true' if op == 'shld' else 'false', v, src, cnt, n, self.wr(i, ops[0], 'r', lin=ea)))
            return L
        if op in i386.STRING_OPS:
            name, sz = op[:-1], {'b': 1, 'w': 2, 'd': 4}[op[-1]]
            src = i.seg if i.seg is not None else 3
            L.append('x86_string(c, %d, %d, %d, %d, %d);' % (STRING[name], sz, i.rep or 0, i.asize, src))
            return L
        # --- transfers ------------------------------------------------------------
        if op == 'jmp':
            o = ops[0]
            if o[0] == 'j':
                L.append(self.go(a, o[1]))
            elif a in self.p.tables:
                L.append('{ uint32_t t = %s;' % self.rd(i, o))
                L.append('  switch (t) {')
                for x in sorted(set(x for x in self.p.tables[a][1] if x and x in inbody)):
                    L.append('  case 0x%xu: %s' % (x, self.go(a, x)))
                L.append('  default: x86_jump(c, t); return; } }')
            else:
                L.append('x86_jump(c, %s); return;' % self.rd(i, o))
            return L
        if op == 'call':
            o = ops[0]
            push = 'push32' if i.osize == 4 else 'push16'
            if o[0] == 'j':
                L.append('%s(c, 0x%xu); f_%08x(c); X86_RET(c, 0x%xu);' % (push, nxt, o[1], nxt))
            elif a in self.p.tables:
                L.append('{ uint32_t t = %s; %s(c, 0x%xu);' % (self.rd(i, o), push, nxt))
                L.append('  switch (t) {')
                for x in sorted(set(x for x in self.p.tables[a][1] if x)):
                    L.append('  case 0x%xu: f_%08x(c); break;' % (x, x))
                L.append('  default: x86_call(c, t); }')
                L.append('  X86_RET(c, 0x%xu); }' % nxt)
            else:
                L.append('{ uint32_t t = %s; %s(c, 0x%xu); x86_call(c, t); X86_RET(c, 0x%xu); }' % (
                    self.rd(i, o), push, nxt, nxt))
            return L
        if op == 'ret':
            pop = 'pop32' if i.osize == 4 else 'pop16'
            L.append('c.eip = %s(c);%s return;' % (pop, ' c.r[4] += %s;' % hx(ops[0][1]) if ops else ''))
            return L
        if op == 'retf':
            L.append('x86_retf(c, %d, %s); return;' % (i.osize, hx(ops[0][1]) if ops else '0u'))
            return L
        if op in ('iret', 'iretd'):
            L.append('x86_iret(c, %d); return;' % (4 if op == 'iretd' else 2))
            return L
        if op in ('lcall', 'ljmp'):
            o = ops[0]
            call = 'true' if op == 'lcall' else 'false'
            if o[0] == 'f':
                L.append('x86_far(c, 0x%xu, 0x%xu, %s, %d, 0x%xu);' % (o[1], o[2], call, i.osize, nxt))
            else:
                L.append('{ uint32_t ea = %s; x86_far(c, rd16(ea + %d), rd%d(ea), %s, %d, 0x%xu); }' % (
                    self.lin(i, o), i.osize, 8 * i.osize, call, i.osize, nxt))
            L.append('X86_RET(c, 0x%xu);' % nxt if op == 'lcall' else 'return;')
            return L
        if op in ('loop', 'loope', 'loopne', 'jecxz', 'jcxz'):
            t = ops[0][1]
            cx = 'c.r[1]' if i.asize == 4 else '(c.r[1] & 0xFFFFu)'
            if op in ('jecxz', 'jcxz'):
                L.append('if (%s == 0) { %s }' % (cx, self.go(a, t)))
                return L
            dec = 'c.r[1] -= 1;' if i.asize == 4 else 's16(c, 1, c.r[1] - 1);'
            extra = {'loop': '', 'loope': ' && (c.fl & X86_ZF)', 'loopne': ' && !(c.fl & X86_ZF)'}[op]
            L.append('%s if (%s != 0%s) { %s }' % (dec, cx, extra, self.go(a, t)))
            return L
        if op == 'enter':
            L.append('x86_enter(c, %s, %s, %d);' % (hx(ops[0][1]), hx(ops[1][1]), i.osize))
            return L
        if op == 'leave':
            pop = 'pop32' if i.osize == 4 else 'pop16'
            L.append('c.r[4] = c.r[5]; %s' % self.wr(i, ('r', 5, i.osize), '%s(c)' % pop))
            return L
        if op in ('int', 'int3', 'into', 'int1'):
            num = {'int3': 3, 'into': 4, 'int1': 1}.get(op, ops[0][1] if ops else 0)
            if op == 'into':
                L.append('if (c.fl & X86_OF) x86_int(c, 4, 0x%xu);' % nxt)
            else:
                L.append('x86_int(c, 0x%x, 0x%xu);' % (num, nxt))
            return L
        if op == 'in':
            port = hx(ops[1][1]) if ops[1][0] == 'i' else '(c.r[2] & 0xFFFFu)'
            L.append(self.wr(i, ops[0], 'x86_in(c, %s, %d)' % (port, ops[0][2])))
            return L
        if op == 'out':
            port = hx(ops[0][1]) if ops[0][0] == 'i' else '(c.r[2] & 0xFFFFu)'
            L.append('x86_out(c, %s, %d, %s);' % (port, ops[1][2], self.rd(i, ops[1])))
            return L
        if op in SYSTEM:
            L.append('x86_system(c, 0x%xu, "%s");' % (a, op))
            return L
        raise NotImplementedError('%x: %s' % (a, op))

    @staticmethod
    def poll(a, t):
        return 'X86_POLL(c, 0x%xu); ' % t if t <= a else ''

    def go(self, a, t):
        """A jump from a to t: a goto in the body, else the runtime's (refused bytes there)."""
        if t in self.inbody:
            return '%sgoto L_%x;' % (self.poll(a, t), t)
        return 'x86_jump(c, 0x%xu); return;' % t
