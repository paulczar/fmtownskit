# fmtownskit

A game-agnostic toolkit for Fujitsu FM Towns and FM Towns Marty reverse
engineering and native PC ports: CD images and the IPL that boots them,
the Phar Lap executables Towns programs are, the machine those programs
touch -- its memory map on both models, its I/O ports, its interrupts, the
TBIOS behind selector 0x110 -- and, as the ports need them, a static
recompiler from 386 code to C++ and a runtime that stands in for the
machine under it.

The same idea as [wiikit](https://github.com/vs-sr-dev/wiikit),
[saturnkit](https://github.com/vs-sr-dev/saturnkit),
[3dokit](https://github.com/vs-sr-dev/3dokit) and ps2kit, for the FM Towns.
Each Towns game has its own engine and formats, but much of every port is
the *same* work: the same CD layout and boot, the same RUN386 extender
loading the same kind of .EXP, the same TBIOS for graphics, sprites, sound
and the pad, the same CD BIOS playing the same CD-DA. fmtownskit collects
that shared part. It grows inside the ports: each piece is written because
a game needed it, then kept free of that game's knowledge. Game formats and
game fixes live in the ports.

**One kit for both machines.** The Marty is an FM Towns in a console's
case -- a 386SX with 2 MB, no hard disk, a pad for keyboard and mouse --
and it boots the same discs. Where the two differ (the SX's 24-bit bus puts
VRAM, sprite RAM and the ROMs at other addresses; the machine id at port
0x31 says 0x4A; the Marty has ROMs of its own), the kit names the machine
as a parameter (`towns.MEMORY['towns']`, `['marty']`), never a fork. A
"Marty title" and a Towns title that runs on the Marty are the same kind of
program to the kit.

## Ports built on it

| Port | Game | What it asked of fmtownskit |
|---|---|---|
| pc-thequeenofduellist | The Queen of Duellist (1993, Agumix; FM Towns CD, Rev A, runs on the Marty) | where the kit was born: the cue sheet with one file per track and audio in the pregaps, ISO 9660 and the IPL record (`disc`), the Phar Lap P3 executable (`exp`), both machines' memory maps, ports, IRQs and machine ids (`towns`), and the map of a program's system surface -- DOS and Phar Lap calls, the CD BIOS, ports, selectors, far calls into TBIOS (`x86map`) |

## Using it

fmtownskit is its own repository. A port takes it as a git submodule at
`fmtownskit/`, so that `python -m fmtownskit.…` works from the port's root.

```sh
python -m fmtownskit.disc GAME.cue                        # tracks, volume, IPL
python -m fmtownskit.disc GAME.cue --list                 # every file
python -m fmtownskit.disc GAME.cue --extract build/disc   # every file, and sectors 0-15
python -m fmtownskit.disc GAME.cue --wav build/cdda       # each CD-DA track from INDEX 01
python -m fmtownskit.exp build/disc/GAME.EXP              # the P3 header
python -m fmtownskit.x86map build/disc/GAME.EXP           # code extents, system sites
python -m fmtownskit.x86map build/disc/GAME.EXP --seed 103e2 --dis 103e2 -n 40
python -m fmtownskit.towns                                # both machines' maps, the ports
```

## What a Towns CD is

Worth writing down in one place, because every port starts here. Checked on
one disc so far (see *Checks*):

* **A Mode 1 data track, then CD-DA tracks.** The data track is plain ISO
  9660. The audio tracks are the music, played by the game through the CD
  BIOS (`int 93h`, AL=0xC0) by position, so the audio in each track's pregap
  (INDEX 00 to 01) is part of the disc a game can play: a runtime has to
  see the disc as one run of sectors, not as files.
* **Sector 0 is the IPL**: "IPL4", a short jump, and real-mode code that
  compares the five bytes at 0040:0000 with "FBIOS" -- which is how `IO.SYS`
  begins -- before it goes on. `IO.SYS` is Towns-DOS ("BIOS Version 3.58" on
  the first disc), whose `CONFIG.SYS` loads TBIOS (`TBIOS.SYS` loading
  `TBIOS.BIN`) and whose `AUTOEXEC.BAT` starts the game, very often as
  `RUN386 GAME.EXP`. The shell, `C:\COMMAND.COM`, is on no disc so far
  (*inferred*: the ROM's drive).
* **RUN386** is the Phar Lap 386|DOS-Extender as Fujitsu built it for the
  Towns ("1.2aJ L50" on the first disc). It loads a P3 .EXP flat at 0,
  gives it selectors for the machine (0x104 VRAM, 0x110 TBIOS, 0x130 sprite
  RAM, as programs use them) and passes `int 21h` to DOS.
* **TBIOS** (`TBIOS.BIN`, 94 KB on the first disc, "V31L31 92/11/11") is
  Fujitsu's protected-mode BIOS: modules each marked with a version, a date
  and a name (graphic, mouse, sprite, the sound module, System, warikomi --
  the interrupt manager). Its first bytes are the table a program calls
  through: `call far fs:[n]` with FS=0x110 lands at the far pointer stored
  at offset n (0x20 graphic, 0x40 mouse, 0x60 sprite, 0x80 sound, 0xA0 a
  module Fujitsu calls "ま～くん").

## Layers

| Layer | Question it answers | Now | Next |
|---|---|---|---|
| 1. Recognise | What is on this disc? | `disc` (cue sheets, raw and cooked tracks, ISO 9660, the IPL record, the tracks' positions and pregaps), `exp` (Phar Lap P3) | MZ executables and RUN386's own loader; TBIOS.BIN's modules as a format; floppy images (.hdm) for the discs that come with a user disk |
| 2. Extract | Turn standard formats into standard files | `disc --extract`, `disc --wav` | the TBIOS-level formats games share (sprite patterns, the sound BIOS's data), when a second game confirms them |
| 3. Map code | What does the code do, where? | `x86map` (descent, extents, every `int` with AH/AL, every port, every selector loaded, far calls through TBIOS's table, indirect transfers) | the TBIOS's own functions named by module and AH; RUN386's selector setup on each machine |
| 4. Translate | Turn 386 code into C++ | -- | an IA-32 decoder (the 386's instruction set, 16- and 32-bit operand and address sizes, segment overrides), a reference interpreter checked against unicorn, discovery, the emitter, a self-test |
| 5. Runtime | What an engine links | -- | the machine: memory and selectors for both models, VRAM layers and the CRTC, the sprite controller, the YM3438 and RF5C68, the CD with its CD-DA, the PICs and timers, the pad; DOS and Phar Lap at their interrupt boundary; TBIOS run from the disc's own code |

## Principles

* Pure Python 3.8+, no dependencies, except `x86map` (capstone).
* Every claim is checked on a real disc or in the running game before it
  goes in. What no disc shows is refused with an error rather than guessed.
  The hardware tables in `towns` are read from MAME's driver (not copied)
  and marked where a game's code confirms them.
* Game knowledge stays out. Where games differ it is a parameter, never a
  constant.
* Both machines, one kit: a difference between the Towns and the Marty is
  data keyed by the machine.

## Checks behind each module

| Module | Checked by |
|---|---|
| `disc` | The Queen of Duellist (Rev A; Redump's split dump, 10 tracks): every track's file matches the CRC32 MAME's software list gives, and the CHD made from the sheet has its SHA-1. The data track: ISO 9660, 2,932 blocks of 6,750 sectors (the rest zero), 167 files in 4 directories, extracted byte for byte as a separate reader does; nine CD-DA tracks whose pregaps (345 to 815 sectors) hold audio in seven of them |
| `exp` | the same disc's `DUEL.EXP`: a level-1 P3 image of 73,495 bytes at 0x200, entry 0xA7F0, stack top 0x13F20, no relocations |
| `towns` | the memory maps, ports, IRQs and machine ids as MAME's fmtowns driver has them; confirmed by a game's code: the pad ports 0x4D0/0x4D2/0x4D6, the CRTC's 0x440/0x442/0x443, the sprite controller's 0x450/0x452, the sound status at 0x4E9 and IRQ13 as int 4Dh (slave PIC at 0x48) |
| `x86map` | `DUEL.EXP`: 154 functions from the entry (157 with three seeds), 27,124 bytes of code in 0xA7F0-0x11B98, every site named (33 `int`, 18 `out`, 6 `in`, 9 far calls into TBIOS, 48 selector loads -- of 0x14, 0x104, 0x110 and 0x130 only, a 32-bit register's low half included), 12 indirect transfers listed |

## Known gaps

* **One disc.** Everything above is what one game shows; the next will add
  to it, and some of what is written here as the Towns' way may turn out to
  be that game's.
* **RUN386's selectors** are named from their use; the extender builds its
  descriptors at run time, which is where the Towns and the Marty differ,
  and that code is still to read.
* **Phar Lap's function names** (`towns.PHARLAP`) are the extender's
  documented ones and have not yet been checked against RUN386's dispatch.
* **The P3 header** past the image fields (the extra pages, the run-time
  parameters) is read but not yet understood: the first disc's says
  0x2009 extra pages minimum and maximum.
