"""Phar Lap 386|DOS-Extender executables: the .EXP file, "P3" flat form.

FM Towns programs that run in protected mode are, very often, .EXP files
run by RUN386.EXE -- the Phar Lap 386|DOS-Extender as Fujitsu shipped it for
the Towns, on the game's own disc. RUN386 loads the image into a flat
segment of its own, gives it selectors for the machine (VRAM, sprite RAM,
the TBIOS's entry table: see `towns`), and passes DOS calls (int 21h) down to
real mode, adding its own functions at AH=25h.

The "P3" header, as read here (little-endian; offsets in the file):

    00 'P3'           02 level (1 flat)     04 header size     06 file size
    0a checksum16     0c run-time parameters offset, 10 size
    14 relocations offset, 18 size          1c segment table offset, 20 size,
    24 its entry size 26 image offset, 2a image size
    2e symbols offset, 32 size              36-55 GDT, LDT, IDT, TSS (offset, size)
    56 min extra bytes                      5a max extra bytes
    5e base offset    62 initial ESP        66 SS      68 initial EIP
    6c CS             6e LDT   70 TSS       72 flags   74 memory required
    78 checksum32     7c stack size

The "extra" fields count bytes past the image, not pages: on both discs read
so far the image's size plus the minimum is the initial ESP (DUEL.EXP
0x11F17 + 0x2009 = 0x13F20; 4DDFMT.EXP 0x46D54 + 0x24FBC = 0x6BD10), and a
maximum of 0xFFFFFFFF asks for all the memory there is.

The run-time parameters ("DX" first) carry the extender's switches as the
program was linked. Only level 1 with no relocations and no segment table is
taken: what has more is refused, not guessed (no disc so far has one).

**The symbol table.** A program linked with symbols carries them at the
header's symbols offset, past the image ("SYM1", read on 4DDFMT.EXP):

    00 'SYM1'  04 u16 its header's size (14)
    06 u32 the segments' size   0a u32 the symbols' size
    then each segment:  [u8 len][name][u32 0][u32 0xFFFFFFFF][u32 selector]
    then each symbol:   [u8 len][name][u32 offset][u16 segment number]

The header's file size then stops at the end of the table's own 14-byte
header, short of the file's end by the segments and symbols: a file is taken
when the table's offset and size reach its end exactly, and both lists are
read to their sizes' last byte.

    python -m fmtownskit.exp GAME.EXP                  # header, parameters, symbols' count
    python -m fmtownskit.exp GAME.EXP --image out.bin  # the image as loaded at 0
    python -m fmtownskit.exp GAME.EXP --symbols        # every symbol, by offset
"""
import argparse
import struct
import sys

FIELDS = [
    ('level', 0x02, 'H'), ('header_size', 0x04, 'H'), ('file_size', 0x06, 'I'),
    ('checksum16', 0x0A, 'H'), ('rtp_offset', 0x0C, 'I'), ('rtp_size', 0x10, 'I'),
    ('reloc_offset', 0x14, 'I'), ('reloc_size', 0x18, 'I'),
    ('seg_offset', 0x1C, 'I'), ('seg_size', 0x20, 'I'), ('seg_entry_size', 0x24, 'H'),
    ('image_offset', 0x26, 'I'), ('image_size', 0x2A, 'I'),
    ('sym_offset', 0x2E, 'I'), ('sym_size', 0x32, 'I'),
    ('gdt_offset', 0x36, 'I'), ('gdt_size', 0x3A, 'I'), ('ldt_offset', 0x3E, 'I'), ('ldt_size', 0x42, 'I'),
    ('idt_offset', 0x46, 'I'), ('idt_size', 0x4A, 'I'), ('tss_offset', 0x4E, 'I'), ('tss_size', 0x52, 'I'),
    ('min_extra', 0x56, 'I'), ('max_extra', 0x5A, 'I'), ('base_offset', 0x5E, 'I'),
    ('esp', 0x62, 'I'), ('ss', 0x66, 'H'), ('eip', 0x68, 'I'), ('cs', 0x6C, 'H'),
    ('ldt', 0x6E, 'H'), ('tss', 0x70, 'H'), ('flags', 0x72, 'H'), ('mem_required', 0x74, 'I'),
    ('checksum32', 0x78, 'I'), ('stack_size', 0x7C, 'I'),
]


def read_symbols(t):
    """A SYM1 table: ([(name, a, b, selector)], [(offset, segment, name)]), or ValueError."""
    if t[:4] != b'SYM1' or len(t) < 14:
        raise ValueError('the symbol table does not start SYM1 (%r)' % t[:4])
    hsize, ssize, nsize = struct.unpack_from('<HII', t, 4)
    if hsize + ssize + nsize != len(t):
        raise ValueError('SYM1 sizes %d + %d + %d, the table has %d' % (hsize, ssize, nsize, len(t)))

    def name(p, end):
        n = t[p]
        if n == 0 or p + 1 + n > end:
            raise ValueError('SYM1: bad name at %#x' % p)
        return t[p + 1:p + 1 + n].decode('latin-1'), p + 1 + n

    segs, syms = [], []
    p, end = hsize, hsize + ssize
    while p < end:
        nm, p = name(p, end)
        a, b, sel = struct.unpack_from('<III', t, p)
        p += 12
        segs.append((nm, a, b, sel))
    end += nsize
    while p < end:
        nm, p = name(p, end)
        off, seg = struct.unpack_from('<IH', t, p)
        p += 6
        syms.append((off, seg, nm))
    if p != end:
        raise ValueError('SYM1: the last record runs %d bytes past its size' % (p - end))
    return segs, sorted(syms)


class Exp:
    def __init__(self, data):
        if data[:2] != b'P3':
            raise ValueError('not a P3 .EXP (starts %r)' % data[:2])
        self.data = data
        for name, off, t in FIELDS:
            setattr(self, name, struct.unpack_from('<' + t, data, off)[0])
        self.segments, self.symbols = [], []
        if self.sym_size:
            if self.sym_offset + self.sym_size != len(data):
                raise ValueError('symbols at %#x, %d bytes: the file has %d'
                                 % (self.sym_offset, self.sym_size, len(data)))
            self.segments, self.symbols = read_symbols(data[self.sym_offset:])
        elif self.file_size != len(data):
            raise ValueError('header says %d bytes, the file has %d' % (self.file_size, len(data)))
        if self.level != 1 or self.reloc_size or self.seg_size:
            raise ValueError('level %d, %d bytes of relocations, %d of segment table: not read here'
                             % (self.level, self.reloc_size, self.seg_size))
        if self.image_offset + self.image_size > len(data):
            raise ValueError('image runs past the file')
        self.image = data[self.image_offset:self.image_offset + self.image_size]
        self.rtp = data[self.rtp_offset:self.rtp_offset + self.rtp_size]

    def fields(self):
        return [(n, getattr(self, n)) for n, _, _ in FIELDS]

    def table_end(self, tbl):
        """Where a table of dwords at `tbl`, read with no bound, ends: the first symbol past its
        second slot (a table indexed from 1 is addressed a slot below its symbol), or None.
        4DDFMT.EXP's `call [ecx*4 + 0x41A48]` (ECX > 0) is `keysubs`, 0x41A4C-0x41B4B."""
        for off, _, _ in self.symbols:
            if off > tbl + 4:
                return off
        return None

    def names(self):
        """offset -> the symbols there, joined (several names can share an offset)."""
        out = {}
        for off, _, nm in self.symbols:
            out[off] = out[off] + ',' + nm if off in out else nm
        return out


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('exp')
    ap.add_argument('--image', metavar='OUT')
    ap.add_argument('--symbols', action='store_true', help='every symbol: offset, segment, name')
    a = ap.parse_args(argv)
    e = Exp(open(a.exp, 'rb').read())
    if a.image:
        open(a.image, 'wb').write(e.image)
        return
    if a.symbols:
        for off, seg, nm in e.symbols:
            print('%06x %d %s' % (off, seg, nm))
        return
    for n, v in e.fields():
        if v:
            print('%-16s %#x' % (n, v))
    print('%-16s %s' % ('run-time params', e.rtp[:16].hex(' ')))
    print('image: %d bytes at file offset %#x, loaded at %#x; entry %#x, stack top %#x'
          % (e.image_size, e.image_offset, e.base_offset, e.eip, e.esp))
    if e.symbols:
        inside = sum(1 for off, _, _ in e.symbols if off < e.base_offset + e.image_size)
        print('symbols: %d (%d in the image, %d past it), segments %s; at the entry: %s'
              % (len(e.symbols), inside, len(e.symbols) - inside,
                 ', '.join('%s %#x' % (s[0], s[3]) for s in e.segments), e.names().get(e.eip, '-')))


if __name__ == '__main__':
    main()
