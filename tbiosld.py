"""A TBIOS made of modules: what TBIOSLD.SYS puts in memory, from the disc's own files.

The first two games' discs carry TBIOS as one file (TBIOS.BIN, V31L31),
which TBIOS.SYS copies to the segment of selector 0x110 (base 0x100000).
ABEL's (V31L35, 1993) carries it in pieces: CONFIG.SYS loads
`\\TBIOS\\TBIOSLD.SYS` ("TownsBIOS Loader Version3.0", 1993.12.06), and that
reads `TBIOS.INI`, one module a line:

    BIN=\\TBIOS\\TBIOSTOP.BIN   the entry table and its default stubs (768 bytes, as it is)
    EGB=\\TBIOS\\EGB_MR.BIN     graphic     SND=...SND_STD.BIN  sound      INT=...INT_4D.BIN  int 04dh
    MOS=\\TBIOS\\MOS_STD.BIN    mouse       MIO=...MIO_STD.BIN  mouseI/O   SPR=...SPR_STD.BIN sprite
    FNT=\\TBIOS\\FNT_STD.BIN    font

Every module but the first is a Phar Lap relocatable image, "MQ": an MZ
header with MQ for its signature (bytes in the last page, pages, relocation
count, header paragraphs, ..., relocation table offset at 0x18) and u32
relocations, bit 31 set for a 32-bit fixup (all of them, on this disc).
TBIOSLD, as read against the oracle's memory (ABEL's docs/03):

* lays the modules one after the other in the INI's order, the first at 0 as
  it is, each next at the next 16-byte boundary, each relocated by where it lands;
* reads each module's own header: 'V31Lnn', its date, 'Townes', its name
  (graphic, sound...), +0x24 a pointer to its data, +0x28 its code and data
  selectors (0x110, 0x118), +0x2E a count of records at +0x30, 8 bytes each,
  `u32 slot, u32 offset`: an **export** (slot < 0x80000000) puts the module's
  entry (offset, 0x110) in the entry table's slot and leaves the slot's old
  pointer in the record (a module chains to what it replaced); an **import**
  (bit 31) leaves the slot's pointer in the record, for the module to call;
* writes at 0x200 of the table, 8 bytes each, the data pointer (+0x24, with
  the data selector) of each module that has one, in order.

The image composed so is the TBIOS a program meets, byte for byte, but for
the work areas its modules fill as they run and the slots a program sets
(ABEL sets 0x98). `--check` holds it against the TBIOS read from an oracle
dump (`mamedump --read 0x100000:LEN`).

    python -m fmtownskit.tbiosld build/disc/TBIOS/TBIOS.INI                # the layout
    python -m fmtownskit.tbiosld build/disc/TBIOS/TBIOS.INI --out tbios.bin
    python -m fmtownskit.tbiosld build/disc/TBIOS/TBIOS.INI --check tbios_mem.bin
"""
import argparse
import os
import struct

CODE_SEL, DATA_SEL = 0x110, 0x118


def rex(data):
    """A Phar Lap "MQ" image: (its bytes past the header, [relocation offsets, bit 31 = 32-bit])."""
    if data[:2] != b'MQ':
        raise ValueError('not an MQ image (starts %r)' % data[:2])
    last, pages, nrel, hpar = struct.unpack_from('<HHHH', data, 2)
    size = pages * 512 - ((512 - last) if last else 0)
    if size != len(data):
        raise ValueError('MQ header says %d bytes, the file has %d' % (size, len(data)))
    roff = struct.unpack_from('<H', data, 0x18)[0]
    rel = [struct.unpack_from('<I', data, roff + 4 * k)[0] for k in range(nrel)]
    return data[hpar * 16:], rel


def relocate(body, rel, base):
    img = bytearray(body)
    for r in rel:
        o = r & 0x7FFFFFFF
        if r >> 31:
            struct.pack_into('<I', img, o, (struct.unpack_from('<I', img, o)[0] + base) & 0xFFFFFFFF)
        else:
            struct.pack_into('<H', img, o, (struct.unpack_from('<H', img, o)[0] + base) & 0xFFFF)
    return img


def read_ini(path):
    """TBIOS.INI: [(key, file path on the disc as written)] in its order."""
    out = []
    for line in open(path, 'rb').read().decode('latin-1').splitlines():
        key, eq, val = line.strip().partition('=')
        if eq and key.isalpha():
            out.append((key, val))
    return out


def compose(ini_path):
    """(image, [(key, file, base, size, name, [(kind, slot, offset)])])."""
    here = os.path.dirname(ini_path)
    mods = read_ini(ini_path)
    if not mods or mods[0][0] != 'BIN':
        raise ValueError('TBIOS.INI does not start with BIN= (the entry table)')
    def local(v):
        return os.path.join(here, v.replace('\\', '/').split('/')[-1])
    img = bytearray(open(local(mods[0][1]), 'rb').read())
    layout = [(mods[0][0], mods[0][1], 0, len(img), 'tbios', [])]
    datas = []
    for key, val in mods[1:]:
        body, rel = rex(open(local(val), 'rb').read())
        base = (len(img) + 15) & ~15
        img += bytes(base - len(img))
        m = relocate(body, rel, base)
        if m[0x10:0x16] != b'Townes':
            raise ValueError('%s: no module header (Townes at +0x10)' % val)
        name = m[0x18:0x20].split(b'\0')[0].decode('latin-1')
        data_ptr, = struct.unpack_from('<I', m, 0x24)
        csel, dsel, _, n = struct.unpack_from('<HHHH', m, 0x28)
        recs = []
        for k in range(n):
            p = 0x30 + 8 * k
            slot, off = struct.unpack_from('<II', m, p)
            s = slot & 0x7FFFFFFF
            if s + 6 > len(img):
                raise ValueError('%s: record %d names slot %#x, past the table' % (val, k, s))
            m[p:p + 8] = bytes(img[s:s + 6]) + b'\0\0'
            if slot >> 31:
                recs.append(('import', s, None))
            else:
                struct.pack_into('<IH', img, s, off, csel)
                recs.append(('export', s, off))
        if data_ptr:
            datas.append((data_ptr, dsel))
        img += m
        layout.append((key, val, base, len(m), name, recs))
    for k, (ptr, sel) in enumerate(datas):
        struct.pack_into('<IHH', img, 0x200 + 8 * k, ptr, sel, 0)
    return bytes(img), layout


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('ini', help="the disc's TBIOS.INI (the modules beside it)")
    ap.add_argument('--out', metavar='FILE', help='write the composed image')
    ap.add_argument('--check', metavar='IMAGE', help='hold it against a TBIOS read from memory')
    a = ap.parse_args(argv)
    img, layout = compose(a.ini)
    for key, val, base, size, name, recs in layout:
        print('%-3s %-22s %#07x-%#07x %-9s %s' % (key, val, base, base + size, name,
              ', '.join('%s %#x%s' % (k, s, '' if o is None else '=%#x' % o) for k, s, o in recs)))
    print('%d bytes' % len(img))
    if a.out:
        open(a.out, 'wb').write(img)
    if a.check:
        mem = open(a.check, 'rb').read()
        diff = [k for k in range(len(img)) if k >= len(mem) or img[k] != mem[k]]
        runs = []
        for k in diff:
            if runs and k <= runs[-1][1] + 4:
                runs[-1][1] = k
            else:
                runs.append([k, k])
        def where(o):
            for key, val, base, size, name, _ in layout:
                if base <= o < base + size:
                    return '%s+%#x' % (name, o - base)
            return '-'
        print('%d bytes differ from %s, in %d runs (work areas the modules fill, slots a program sets):'
              % (len(diff), a.check, len(runs)))
        for lo, hi in runs:
            print('  %#07x %-18s %d bytes' % (lo, where(lo), hi - lo + 1))


if __name__ == '__main__':
    main()
