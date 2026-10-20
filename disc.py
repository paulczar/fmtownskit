"""FM Towns CD images: the cue sheet, the tracks, ISO 9660, the IPL record.

An FM Towns CD (and the FM Towns Marty's: the console reads the same discs)
is a Mode 1 data track first, then, very often, CD-DA tracks that the game
plays as its music through the CD BIOS. The data track is plain ISO 9660;
what makes it bootable is sector 0, the system area ISO 9660 leaves alone:
it begins with "IPL4" and holds the IPL the boot ROM loads and runs.

Images this reads:

    .cue            a sheet of one or more FILE entries (one file per track,
                    as Redump splits them, or one file for every track), with
                    MODE1/2352, MODE1/2048 and AUDIO tracks, INDEX 00/01
    .bin / .img     raw 2,352-byte Mode 1 sectors, the data at +16
    .iso            2,048-byte sectors

A track's position on the disc is the sum of the sectors of the files before
it plus its INDEX 01 inside its own file; INDEX 00 to INDEX 01 is its pregap,
which a split dump carries at the head of the track's file.

    python -m fmtownskit.disc GAME.cue                      # tracks, volume, IPL
    python -m fmtownskit.disc GAME.cue --list               # every file
    python -m fmtownskit.disc GAME.cue --extract build/disc # every file, and sysarea.bin
    python -m fmtownskit.disc GAME.cue --cat DUEL.EXP > DUEL.EXP
    python -m fmtownskit.disc GAME.cue --wav build/cdda     # each audio track, INDEX 01 on
"""
import argparse
import os
import re
import struct
import sys
import wave

RAW = 2352
COOKED = 2048
SYNC = b'\x00' + b'\xff' * 10 + b'\x00'


def msf(text):
    m, s, f = (int(x) for x in text.split(':'))
    return (m * 60 + s) * 75 + f


class Track:
    def __init__(self, number, mode, path):
        self.number = number
        self.mode = mode            # 'MODE1/2352', 'MODE1/2048', 'AUDIO'
        self.path = path
        self.index = {}             # index number -> sector inside its file
        self.file_start = 0         # first sector of its file on the disc
        self.sectors = 0            # from INDEX 01 to the next track or the end
        self.sector_size = COOKED if mode == 'MODE1/2048' else RAW

    @property
    def audio(self):
        return self.mode == 'AUDIO'

    @property
    def lba(self):
        """The disc sector of INDEX 01, counted from the first track's INDEX 01."""
        return self.file_start + self.index.get(1, 0)

    @property
    def pregap(self):
        return self.index[1] - self.index[0] if 0 in self.index else 0


class Disc:
    """A cue sheet, or one bare data track (.bin/.img raw, .iso cooked)."""

    def __init__(self, path):
        self.path = path
        self.tracks = []
        if path.lower().endswith('.cue'):
            self._read_cue(path)
        else:
            size = os.path.getsize(path)
            raw = size % RAW == 0 and open(path, 'rb').read(12) == SYNC
            t = Track(1, 'MODE1/2352' if raw else 'MODE1/2048', path)
            t.index[1] = 0
            t.sectors = size // t.sector_size
            self.tracks.append(t)
        self._files = {}
        data = [t for t in self.tracks if not t.audio]
        if not data:
            raise ValueError('%s: no data track' % path)
        self.data = data[0]

    def _read_cue(self, path):
        base = os.path.dirname(path)
        cur_file = None
        for line in open(path, encoding='latin-1'):
            words = line.strip()
            if words.startswith('FILE'):
                m = re.match(r'FILE\s+"(.*)"\s+(\S+)', words) or re.match(r'FILE\s+(\S+)\s+(\S+)', words)
                cur_file = os.path.join(base, m.group(1))
            elif words.startswith('TRACK'):
                _, n, mode = words.split()
                if mode not in ('MODE1/2352', 'MODE1/2048', 'AUDIO'):
                    raise ValueError('%s: track %s is %s, not read here' % (path, n, mode))
                self.tracks.append(Track(int(n), mode, cur_file))
            elif words.startswith('INDEX'):
                _, n, t = words.split()
                self.tracks[-1].index[int(n)] = msf(t)
            elif words.startswith('PREGAP') or words.startswith('POSTGAP'):
                raise ValueError('%s: %s (a gap not in the files) is not read here' % (path, words.split()[0]))
        # place each file on the disc, and each track's length
        start = 0
        files = []
        for t in self.tracks:
            if not files or files[-1][0] != t.path:
                files.append([t.path, start, t.sector_size])
                start += os.path.getsize(t.path) // t.sector_size
            t.file_start = files[-1][1]
        ends = {p: s + os.path.getsize(p) // z for p, s, z in files}
        for i, t in enumerate(self.tracks):
            nxt = self.tracks[i + 1] if i + 1 < len(self.tracks) else None
            if nxt is not None and nxt.path == t.path:
                end = nxt.file_start + min(nxt.index.values())
            else:
                end = ends[t.path]
            t.sectors = end - t.lba

    # --- the data track ---------------------------------------------------
    def sector(self, n, count=1):
        """`count` 2,048-byte sectors of the data track from its sector n."""
        t = self.data
        f = self._files.get(t.path)
        if f is None:
            f = self._files[t.path] = open(t.path, 'rb')
        out = []
        for i in range(count):
            pos = (t.index.get(1, 0) + n + i) * t.sector_size
            f.seek(pos)
            if t.sector_size == RAW:
                raw = f.read(RAW)
                if raw[:12] != SYNC or raw[15] != 1:
                    raise ValueError('data sector %d is not Mode 1' % (n + i))
                out.append(raw[16:16 + COOKED])
            else:
                out.append(f.read(COOKED))
        return b''.join(out)

    def read(self, lba, size):
        return self.sector(lba, (size + COOKED - 1) // COOKED)[:size]

    def pvd(self):
        """The primary volume descriptor's fields (ISO 9660, sector 16 on)."""
        n = 16
        while True:
            d = self.sector(n)
            if d[1:6] != b'CD001':
                raise ValueError('sector %d: no ISO 9660 descriptor' % n)
            if d[0] == 1:
                break
            if d[0] == 255:
                raise ValueError('no primary volume descriptor')
            n += 1
        s = lambda a, b: d[a:b].decode('ascii', 'replace').rstrip()
        return {
            'system': s(8, 40), 'volume': s(40, 72),
            'blocks': struct.unpack_from('<I', d, 80)[0],
            'block_size': struct.unpack_from('<H', d, 128)[0],
            'volume_set': s(190, 318), 'publisher': s(318, 446),
            'preparer': s(446, 574), 'application': s(574, 702),
            'created': s(813, 829), 'modified': s(830, 846),
            'root': (struct.unpack_from('<I', d, 156 + 2)[0], struct.unpack_from('<I', d, 156 + 10)[0]),
        }

    def walk(self):
        """Every entry: (path, is_dir, lba, size, (y, m, d, h, mi, s))."""
        out = []

        def go(lba, size, prefix):
            data = self.read(lba, size)
            p = 0
            while p < len(data):
                n = data[p]
                if n == 0:              # records never cross a sector
                    p = (p // COOKED + 1) * COOKED
                    continue
                rec = data[p:p + n]
                p += n
                name = rec[33:33 + rec[32]]
                if name in (b'\x00', b'\x01'):
                    continue
                elba = struct.unpack_from('<I', rec, 2)[0]
                esz = struct.unpack_from('<I', rec, 10)[0]
                when = (1900 + rec[18],) + tuple(rec[19:24])
                isdir = bool(rec[25] & 2)
                nm = name.decode('ascii', 'replace').split(';')[0]
                out.append((prefix + nm, isdir, elba, esz, when))
                if isdir:
                    go(elba, esz, prefix + nm + '/')
        root = self.pvd()['root']
        go(root[0], root[1], '')
        return out

    def file(self, name):
        for path, isdir, lba, size, _ in self.walk():
            if not isdir and path.lower() == name.lower().replace('\\', '/'):
                return self.read(lba, size)
        raise KeyError(name)

    def ipl(self):
        """Sector 0 as the boot ROM sees it: the "IPL4" mark, or None."""
        d = self.sector(0)
        return d if d[:4] == b'IPL4' else None

    # --- audio --------------------------------------------------------------
    def audio_samples(self, track):
        """The track's 16-bit stereo samples (little-endian), INDEX 01 to its end."""
        with open(track.path, 'rb') as f:
            f.seek((track.lba - track.file_start) * RAW)
            return f.read(track.sectors * RAW)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('image')
    ap.add_argument('--list', action='store_true')
    ap.add_argument('--extract', metavar='DIR')
    ap.add_argument('--cat', metavar='NAME')
    ap.add_argument('--wav', metavar='DIR')
    a = ap.parse_args(argv)
    d = Disc(a.image)
    if a.cat:
        sys.stdout.buffer.write(d.file(a.cat))
        return
    if a.list:
        for path, isdir, lba, size, w in d.walk():
            print('%-40s %10s  lba %6d  %04d-%02d-%02d %02d:%02d:%02d' %
                  (path + ('/' if isdir else ''), '<dir>' if isdir else size, lba, *w))
        return
    if a.extract:
        n = 0
        for path, isdir, lba, size, _ in d.walk():
            out = os.path.join(a.extract, *path.split('/'))
            if isdir:
                os.makedirs(out, exist_ok=True)
                continue
            os.makedirs(os.path.dirname(out), exist_ok=True)
            with open(out, 'wb') as f:
                f.write(d.read(lba, size))
            n += 1
        with open(os.path.join(a.extract, 'sysarea.bin'), 'wb') as f:
            f.write(d.sector(0, 16))
        print('%d files, and sectors 0-15 as sysarea.bin, to %s' % (n, a.extract))
        return
    if a.wav:
        os.makedirs(a.wav, exist_ok=True)
        for t in d.tracks:
            if not t.audio:
                continue
            out = os.path.join(a.wav, 'track%02d.wav' % t.number)
            w = wave.open(out, 'wb')
            w.setnchannels(2)
            w.setsampwidth(2)
            w.setframerate(44100)
            w.writeframes(d.audio_samples(t))
            w.close()
            print(out)
        return
    print('tracks:')
    for t in d.tracks:
        secs = t.sectors / 75
        print('  %02d %-10s lba %6d  %6d sectors  %2d:%05.2f  pregap %d' %
              (t.number, t.mode, t.lba, t.sectors, secs // 60, secs % 60, t.pregap))
    p = d.pvd()
    for k in ('system', 'volume', 'volume_set', 'publisher', 'preparer', 'application', 'created', 'modified'):
        print('%-12s %r' % (k, p[k]))
    print('%-12s %d of %d bytes (the track has %d)' % ('blocks', p['blocks'], p['block_size'], d.data.sectors))
    ipl = d.ipl()
    if ipl is None:
        print('IPL         none: sector 0 has no "IPL4" (not bootable by the boot ROM)')
    else:
        print('IPL         "IPL4", %d of 2,048 bytes not zero; jumps to +%#x' %
              (sum(1 for b in ipl if b), 6 + ipl[5] if ipl[4] == 0xEB else -1))
    entries = d.walk()
    print('files       %d in %d directories' % (sum(1 for e in entries if not e[1]),
                                               1 + sum(1 for e in entries if e[1])))


if __name__ == '__main__':
    main()
