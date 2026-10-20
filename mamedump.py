"""A running Towns, frozen: MAME's CPU state and physical RAM, read as the CPU sees them.

The oracle (a port's tools/oracle.py --dump) has MAME write, at a moment
of emulated time, `state.txt` (every register MAME's i386 core exposes,
NAME=hex) and `ram.bin` (physical memory from 0). This module reads that
pair: linear addresses through the page tables when CR0.PG is set, the
GDT and the LDT with every present descriptor, the page mappings, and a
search for bytes.

Why it matters: on the Marty, RUN386 turns paging on and maps the Towns'
high addresses (VRAM at 0x80000000, sprite RAM at 0x81000000, the ROMs at
0xC2000000 and 0xFFFC0000) onto the Marty's 24-bit ones, so that the
descriptors -- and a program's view of the machine -- are the Towns' on
both. A dump of each machine shows it.

    python -m fmtownskit.mamedump DUMPDIR                 # descriptors, paging
    python -m fmtownskit.mamedump DUMPDIR --pages         # every page mapping
    python -m fmtownskit.mamedump DUMPDIR --find FILE[:OFF:LEN]   # where those bytes are
    python -m fmtownskit.mamedump DUMPDIR --read 0x200000:0x14000 OUT   # linear bytes out
"""
import argparse
import os
import struct


class Dump:
    def __init__(self, d):
        self.state = {}
        for line in open(os.path.join(d, 'state.txt')):
            k, _, v = line.strip().partition('=')
            if v:
                self.state[k] = int(v, 16)
        self.ram = open(os.path.join(d, 'ram.bin'), 'rb').read()
        self.paging = bool(self.state.get('CR0', 0) & 0x80000000)
        self.cr3 = self.state.get('CR3', 0) & ~0xFFF

    def _u32(self, p):
        return struct.unpack_from('<I', self.ram, p)[0] if p + 4 <= len(self.ram) else 0

    def phys(self, lin):
        """The physical address of a linear one, or None when it is not mapped."""
        if not self.paging:
            return lin
        pde = self._u32(self.cr3 + ((lin >> 22) << 2))
        if not pde & 1:
            return None
        pte = self._u32((pde & ~0xFFF) + (((lin >> 12) & 0x3FF) << 2))
        if not pte & 1:
            return None
        return (pte & ~0xFFF) | (lin & 0xFFF)

    def read(self, lin, n):
        """n bytes at a linear address; bytes outside RAM or unmapped are None."""
        out = []
        for i in range(n):
            p = self.phys(lin + i)
            out.append(self.ram[p] if p is not None and p < len(self.ram) else None)
        return out

    def descriptors(self, which):
        base, limit, ti = ((self.state['GDTRBASE'], self.state['GDTRLIMIT'], 0) if which == 'GDT'
                           else (self.state['LDTRBASE'], self.state['LDTRLIMIT'], 4))
        raw = self.read(base, limit + 1)
        out = []
        for i in range(0, limit + 1 - 7, 8):
            b = raw[i:i + 8]
            if None in b:
                continue
            lo, hi = struct.unpack('<II', bytes(b))
            acc = (hi >> 8) & 0xFF
            if not acc & 0x80:
                continue
            dbase = (lo >> 16) | ((hi & 0xFF) << 16) | (hi & 0xFF000000)
            dlim = (lo & 0xFFFF) | (hi & 0xF0000)
            if hi & 0x800000:
                dlim = (dlim << 12) | 0xFFF
            if acc & 0x10:
                kind = 'code' if acc & 8 else 'data'
            else:
                kind = 'system %x' % (acc & 0xF)
            out.append((i | ti, dbase, dlim, kind, (acc >> 5) & 3, 32 if hi & 0x400000 else 16))
        return out

    def pages(self):
        runs = []
        if not self.paging:
            return runs
        for di in range(1024):
            pde = self._u32(self.cr3 + di * 4)
            if not pde & 1:
                continue
            for ti in range(1024):
                pte = self._u32((pde & ~0xFFF) + ti * 4)
                if not pte & 1:
                    continue
                lin, ph = (di << 22) | (ti << 12), pte & ~0xFFF
                if runs and runs[-1][0] + runs[-1][2] == lin and runs[-1][1] + runs[-1][2] == ph:
                    runs[-1][2] += 0x1000
                else:
                    runs.append([lin, ph, 0x1000])
        return runs


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('dump')
    ap.add_argument('--pages', action='store_true')
    ap.add_argument('--find', metavar='FILE[:OFF:LEN]')
    ap.add_argument('--read', nargs=2, metavar=('LIN:LEN', 'OUT'))
    a = ap.parse_args(argv)
    d = Dump(a.dump)
    if a.find:
        path, _, rest = a.find.partition(':')
        data = open(path, 'rb').read()
        if rest:
            off, ln = (int(x, 0) for x in rest.split(':'))
            data = data[off:off + ln]
        else:
            data = data[:64]
        p = d.ram.find(data)
        while p >= 0:
            print('physical %08x' % p)
            p = d.ram.find(data, p + 1)
        return
    if a.read:
        lin, ln = (int(x, 0) for x in a.read[0].split(':'))
        b = d.read(lin, ln)
        open(a.read[1], 'wb').write(bytes(x if x is not None else 0 for x in b))
        print('%d bytes, %d unmapped' % (ln, b.count(None)))
        return
    s = d.state
    print('CR0 %08x (paging %s), CR3 %08x; CS:EIP %04x:%08x; GDT %08x+%x, LDT %08x+%x' % (
        s.get('CR0', 0), 'on' if d.paging else 'off', s.get('CR3', 0), s.get('CS', 0), s.get('EIP', 0),
        s['GDTRBASE'], s['GDTRLIMIT'], s['LDTRBASE'], s['LDTRLIMIT']))
    for which in ('GDT', 'LDT'):
        print(which)
        for sel, b, l, kind, dpl, bits in d.descriptors(which):
            p = d.phys(b)
            print('  %04x  base %08x  limit %08x  %-9s dpl%d %d-bit%s' % (
                sel, b, l, kind, dpl, bits,
                '' if p == b else '  (physical %s)' % ('%08x' % p if p is not None else 'unmapped')))
    if a.pages:
        for lin, ph, n in d.pages():
            print('  %08x -> %08x  %x' % (lin, ph, n))


if __name__ == '__main__':
    main()
