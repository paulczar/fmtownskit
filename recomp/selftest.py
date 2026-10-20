"""The self-test's vectors: the interpreter runs, the recompiled code replays.

For each function asked for, from random registers and flags, `i386emu`
runs it -- its return address a sentinel pushed first -- until it returns
there; memory nobody wrote reads as `fill` (runtime/x86_core.cpp's
x86_fill_page, the same bytes). A vector is the state going in, the state
coming out, and every byte written. A run that reaches the machine (an
interrupt, a port, a far transfer) or runs past a step limit is not a
vector: the C++ would stop at the same place.

The optest (`optest_image`) is the emitter's own test, independent of any
game: every instruction form a sweep of random bytes decodes, and every
instruction of the programs given, each made into a one-instruction function
(the instruction, then `ret`), with vectors from random states.

    python -m fmtownskit.recomp.selftest --out FILE MODULE=EXP [--per 3] [--auto]
"""
import random
import struct

from .. import i386, i386emu
from ..i386 import Refused, decode

SENTINEL = 0xFFFFFFF0
M32 = 0xFFFFFFFF


def fill(page):
    x = (page * 0x9E3779B1 + 0x7F4A7C15) & M32
    if not x:
        x = 1
    out = bytearray(4096)
    for k in range(1024):
        x ^= (x << 13) & M32
        x ^= x >> 17
        x ^= (x << 5) & M32
        struct.pack_into('<I', out, 4 * k, x)
    return bytes(out)


class Machine(i386emu.CPU):
    """The self-test's machine: nothing behind it. A REP of more than 100,000 (a count read
    from memory nobody wrote) is not a vector: one step would not end."""

    cs = 0

    def load_segment(self, n, sel):
        """The program's own selectors (its code, CS, and data, CS+8) at base 0, where its image
        is; any other at a base of its own, sel << 16, so that a write through it (VRAM, sprite
        RAM...) cannot land on the code. runtime/x86_selftest.cpp does the same."""
        sel &= 0xFFFF
        self.sreg[n] = sel
        self.sbase[n] = 0 if (sel & ~3) in (self.cs & ~3, (self.cs + 8) & ~3) else (sel << 16) & M32

    def string(self, i, op, n):
        if i.rep is not None and (self.r[1] & (0xFFFF if i.asize == 2 else M32)) > 100000:
            raise i386emu.Stop('a REP of %d' % self.r[1])
        i386emu.CPU.string(self, i, op, n)


def random_state(rnd):
    r = [rnd.getrandbits(32) for _ in range(8)]
    if rnd.random() < 0.4:      # small values make loops end and tables index in range
        r = [x & 0xFF for x in r]
    r[1] = rnd.randrange(0, 64)          # a REP or LOOP count: one step must end
    r[4] = 0x00F00000 + (rnd.getrandbits(16) & ~3)      # a stack apart from the code
    flags = 0x2 | (rnd.getrandbits(12) & (i386emu.ARITH | i386emu.DF))
    return r, flags


def run(image_load, entry, regs, flags, limit=200000, cs=0):
    """(regs, eip, flags, written) or None when the machine is reached or the limit passed."""
    mem = i386emu.FlatMemory(fill)
    for base, data in image_load:
        mem.load(base, data)
    cpu = Machine(mem)
    cpu.cs = cs
    cpu.r = list(regs)
    cpu.flags = flags
    cpu.push(SENTINEL, 4)
    mem.written.clear()
    cpu.eip = entry
    cache = {}
    # The recompiled code's calls are C++ calls: a `ret` returns from the function it is in,
    # and a caller whose callee came back elsewhere returns too (X86_RET). `frames` holds the
    # return addresses the C++ frames expect; the function asked for has returned when a
    # `ret` leaves no frame expecting where it went.
    frames = []
    for _ in range(limit):
        i = cache.get(cpu.eip)
        if i is None:
            try:
                raw = bytes(mem.read(cpu.eip + k, 1) for k in range(16))
                i = decode(raw, 0, cpu.eip)
            except Refused:
                return None
            cache[cpu.eip] = i
        try:
            cpu.step(i)
        except (i386emu.Stop, i386emu.Fault):
            return None
        if i.op == 'call':
            frames.append((i.addr + i.size) & M32)
        elif i.op == 'ret':
            while frames and frames[-1] != cpu.eip:
                frames.pop()
            if not frames:
                break
            frames.pop()
    else:
        return None
    written = {a: mem.read(a, 1) for a in mem.written}
    return cpu.r, cpu.eip, cpu.flags, written


def vector_line(mod, addr, regs_in, fl_in, res):
    r, eip, fl, written = res
    parts = ['V', mod, '%x' % addr] + ['%x' % v for v in regs_in] + ['%x' % fl_in]
    parts += ['%x' % v for v in r] + ['%x' % eip, '%x' % fl, 'fff', '%d' % len(written)]
    parts += ['%x:%x' % kv for kv in sorted(written.items())]
    return ' '.join(parts)


def vectors(mod, image_load, entries, per, seed=1, limit=20000, cs=0):
    """Lines for the C++ self-test, and counts."""
    rnd = random.Random(seed)
    lines, done, skipped = [], 0, 0
    for f in entries:
        for _ in range(per):
            regs, fl = random_state(rnd)
            res = run(image_load, f, regs, fl, limit, cs)
            if res is None:
                skipped += 1
                continue
            lines.append(vector_line(mod, f, regs, fl, res))
            done += 1
    return lines, done, skipped


# --- the optest: one-instruction functions ---------------------------------------
OPTEST_BASE = 0x00100000
EXCLUDE = {'int', 'int1', 'int3', 'into', 'in', 'out', 'insb', 'insw', 'insd', 'outsb', 'outsw', 'outsd',
           'iret', 'iretd', 'retf', 'ret', 'lcall', 'ljmp', 'call', 'jmp', 'hlt', 'loop', 'loope', 'loopne',
           'jecxz', 'jcxz', 'enter', 'leave', 'popf', 'popfd', 'pushfd', 'pushf'} | set(
    'j' + c for c in i386.CC) | {'arpl', 'sldt', 'str', 'lldt', 'ltr', 'verr', 'verw', 'sgdt', 'sidt',
                                 'lgdt', 'lidt', 'smsw', 'lmsw', 'lar', 'lsl', 'clts', 'bound', 'cli', 'sti'}


def optest_image(programs=(), sweep=6000, seed=3):
    """(image bytes, [entry addresses]): each test instruction followed by `ret`."""
    rnd = random.Random(seed)
    seen, raws = set(), []

    def take(i):
        if i.op in EXCLUDE or (i.op == 'mov' and any(o[0] in 'cdt' for o in i.ops)):
            return
        if i.raw in seen:
            return
        seen.add(i.raw)
        raws.append(i.raw)
    for prog in programs:
        for a in sorted(prog.insn):
            take(prog.insn[a])
    n = 0
    while n < sweep:
        buf = bytes(rnd.getrandbits(8) for _ in range(16))
        try:
            i = decode(buf, 0, 0)
        except Refused:
            continue
        n += 1
        take(i)
    image, entries = bytearray(), []
    for raw in raws:
        entries.append(OPTEST_BASE + len(image))
        image += raw + b'\xC3'
    return bytes(image), entries
