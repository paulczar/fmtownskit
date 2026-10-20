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
    56 min extra 4K pages                   5a max extra pages
    5e base offset    62 initial ESP        66 SS      68 initial EIP
    6c CS             6e LDT   70 TSS       72 flags   74 memory required
    78 checksum32     7c stack size

The run-time parameters ("DX" first) carry the extender's switches as the
program was linked. Only level 1 with no relocations and no segment table is
taken: what has more is refused, not guessed (no disc so far has one).

    python -m fmtownskit.exp DUEL.EXP                  # header, parameters
    python -m fmtownskit.exp DUEL.EXP --image out.bin  # the image as loaded at 0
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
    ('min_extra_pages', 0x56, 'I'), ('max_extra_pages', 0x5A, 'I'), ('base_offset', 0x5E, 'I'),
    ('esp', 0x62, 'I'), ('ss', 0x66, 'H'), ('eip', 0x68, 'I'), ('cs', 0x6C, 'H'),
    ('ldt', 0x6E, 'H'), ('tss', 0x70, 'H'), ('flags', 0x72, 'H'), ('mem_required', 0x74, 'I'),
    ('checksum32', 0x78, 'I'), ('stack_size', 0x7C, 'I'),
]


class Exp:
    def __init__(self, data):
        if data[:2] != b'P3':
            raise ValueError('not a P3 .EXP (starts %r)' % data[:2])
        self.data = data
        for name, off, t in FIELDS:
            setattr(self, name, struct.unpack_from('<' + t, data, off)[0])
        if self.file_size != len(data):
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


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('exp')
    ap.add_argument('--image', metavar='OUT')
    a = ap.parse_args(argv)
    e = Exp(open(a.exp, 'rb').read())
    if a.image:
        open(a.image, 'wb').write(e.image)
        return
    for n, v in e.fields():
        if v:
            print('%-16s %#x' % (n, v))
    print('%-16s %s' % ('run-time params', e.rtp[:16].hex(' ')))
    print('image: %d bytes at file offset %#x, loaded at %#x; entry %#x, stack top %#x'
          % (e.image_size, e.image_offset, e.base_offset, e.eip, e.esp))


if __name__ == '__main__':
    main()
