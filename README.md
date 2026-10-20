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

And for a program run by RUN386 the two are **one machine**: RUN386 runs
with paging on, on both, and on the Marty its pages put the Towns' high
addresses back where a Towns has them (`towns.PAGES_MARTY`); the
descriptors are the same on both, base for base (read from both machines
running, `mamedump`). What does differ is the TBIOS: on the Towns it is the
disc's `TBIOS.BIN`, on the Marty a build of the same version from the
Marty's own ROM, its modules from the sprite one on 16 bytes lower, with 8
KB more. A port that runs
the disc's TBIOS runs the Towns path, and needs no ROM.

## Ports built on it

| Port | Game | What it asked of fmtownskit |
|---|---|---|
| pc-thequeenofduellist | The Queen of Duellist (1993, Agumix; FM Towns CD, Rev A, runs on the Marty) | where the kit was born: the cue sheet with one file per track and audio in the pregaps, ISO 9660 and the IPL record (`disc`), the Phar Lap P3 executable (`exp`), both machines' memory maps, ports, IRQs and machine ids (`towns`), and the map of a program's system surface -- DOS and Phar Lap calls, the CD BIOS, ports, selectors, far calls into TBIOS (`x86map`); RUN386's selectors and paging read on both machines running (`mamedump`); the 386 decoder, the interpreter, discovery with its jump tables, the emitter, the runtime's CPU and the self-test (`i386`, `i386emu`, `recomp`, `runtime/x86*`), on the game and on TBIOS |

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
python -m fmtownskit.mamedump DUMPDIR [--pages]           # a running machine's GDT, LDT, paging
python -m fmtownskit.mamedump DUMPDIR --find FILE:OFF:LEN # where those bytes sit in RAM
python -m fmtownskit.x86map TBIOS.BIN --raw --far-table 20:100   # a raw image from its far-pointer table
python -m fmtownskit.i386 GAME.EXP --dis a7f0 -n 20       # the 386 decoder
python -m fmtownskit.i386 GAME.EXP --check                # every reached instruction against capstone
python -m fmtownskit.i386 --sweep 300000                  # random bytes against capstone
python -m fmtownskit.i386emu --test                       # the interpreter's known answers
python -m fmtownskit.i386emu --check GAME.EXP --states 4  # every reached instruction against unicorn
python -m fmtownskit.i386emu --sweep 100000               # random instructions against unicorn
python -m fmtownskit.recomp.discover GAME.EXP --seed 103e2 --report     # functions, tables, conflicts
python -m fmtownskit.recomp --out build/recomp --exp game=GAME.EXP@0c+SEEDS \
    --raw tbios=TBIOS.BIN@110:20-100 --optest --selftest 3                # C++, a module per program
cmake -S build/recomp -B build/recomp/b -G Ninja -DCMAKE_CXX_COMPILER=clang++
ninja -C build/recomp/b && build/recomp/b/selftest build/recomp/selftest/*.txt
```

A dump is MAME's CPU state and physical RAM (`state.txt`, `ram.bin`) at a
moment the CPU is in protected mode with paging on; a port's oracle writes
it with a Lua script.

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
| 3. Map code | What does the code do, where? | `x86map` (descent, extents, every `int` with AH/AL, every port, every selector loaded, far calls through TBIOS's table, indirect transfers), `mamedump` (a running machine: GDT, LDT, paging, bytes found) | the TBIOS's own functions named by module and AH |
| 4. Translate | Turn 386 code into C++ | `i386` (the 386's integer instruction set exactly: 16- and 32-bit operand and address sizes, segment overrides, REP/LOCK, ModRM/SIB, the 0F map; later CPUs' instructions and the x87 refused with a reason), `i386emu` (an interpreter: the reference, its undefined flags listed), `recomp.discover` (functions and their bodies, jump tables bounded by the guard that really guards them -- or by `setcc`, `and` --, tables with no bound read until an entry's code conflicts or is refused, the descent rerun until clean), `recomp.emit` and `python -m fmtownskit.recomp` (C++ per function, a module per program keyed by its code selector, calls as C++ calls with the return checked and unwound, jump tables as switches on the entry read, the 386's clocks a block at a time), `recomp.selftest` (the interpreter records, the C++ replays; the optest: every instruction form, one-instruction functions) | flags only where read; the clocks held against the oracle |
| 5. Runtime | What an engine links | for recompiled code (C++20): `x86.h` (the CPU, linear memory as a table of 4 KB pages, the flags), `x86_flags` (shifts, multiply and divide, bits, BCD, strings, POPF, ENTER as `i386emu` does them), `x86_core` (modules, lookup by code selector), `x86_selftest` | the machine: memory and selectors for both models, VRAM layers and the CRTC, the sprite controller, the YM3438 and RF5C68, the CD with its CD-DA, the PICs and timers, the pad; DOS and Phar Lap at their interrupt boundary; TBIOS run from the disc's own code |

## Principles

* Pure Python 3.8+, no dependencies, except `x86map` (capstone); `i386 --check`
  is held against capstone and `i386emu --check` against unicorn. The
  recompiled code's runtime is C++20, built with CMake, Ninja and clang.
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
| `towns.SELECTORS`, `PAGES_MARTY` | the descriptor tables of MAME's `fmtowns` and `fmtmarty` running The Queen of Duellist, 20 seconds in: GDT and LDT the same base for base on both (the TBIOS's limit aside, 0x16FFF and 0x18FFF), paging on in both, the Towns' high pages 1:1, the Marty's at the seven mappings listed; the game's image at linear 0x200000 on both (physical 0x49000 and 0x47000), its code byte for byte the file's |
| `mamedump` | the same two dumps: descriptors, the page walk, the image and the TBIOS found in RAM (the Towns' TBIOS the disc's `TBIOS.BIN` but for 265 bytes of data; the Marty's found in its ROM `MAR_EX1` at 0x19C00) |
| `i386` | every instruction discovery reaches in `DUEL.EXP` (7,903) and `TBIOS.BIN` (16,120) against capstone, field by field: 0 differ. 300,000 random byte sequences: 281,740 agree, 11 differ and each is capstone's error (a 67h `jmp rel32` truncated to 16 bits, a REP or LOCK prefix dropped, the test registers TR6/TR7 unknown) |
| `i386emu` | 10 known answers; against unicorn from random states, memory mapped on demand: 30,020 vectors over `DUEL.EXP`'s instructions, 46,545 over TBIOS's, 169,696 over random instructions -- 0 differ. Set aside: the machine's instructions, the flags Intel leaves undefined, and unicorn's own two errors (`ret imm16` sign-extended; F6/F7 /1, TEST's second encoding, refused) |
| `recomp.discover` | `DUEL.EXP`: 157 functions, 0 conflicts. `TBIOS.BIN` from its far-pointer table: 355 functions, 14,385 instructions, 33 jump tables (11 bounded), 0 refusals, 0 conflicts -- where a first descent had 260 conflicts and 14 refusals from a table bounded by a comparison of a register rewritten before the jump |
| `recomp.emit`, `recomp.selftest` | the optest: 7,464 one-instruction functions (every distinct instruction of both programs and of a random sweep), 22,340 vectors from `i386emu`, replayed on the C++: 0 failures. The programs' own functions, 3 random states each: `DUEL.EXP` 135 vectors, TBIOS 682, 0 failures. Every generated file compiles with clang |
| `x86map` | `DUEL.EXP`: 154 functions from the entry (157 with three seeds), 27,124 bytes of code in 0xA7F0-0x11B98, every site named (33 `int`, 18 `out`, 6 `in`, 9 far calls into TBIOS, 48 selector loads -- of 0x14, 0x104, 0x110 and 0x130 only, a 32-bit register's low half included), 12 indirect transfers listed |

## Known gaps

* **One disc.** Everything above is what one game shows; the next will add
  to it, and some of what is written here as the Towns' way may turn out to
  be that game's.
* **RUN386's selectors** are read from two machines running, not from
  RUN386's own code; the blocks `int 21h AH=48h` hands out (LDT 0x44 up)
  are placed by it as the dumps show, not yet by a rule.
* **Phar Lap's function names** (`towns.PHARLAP`) are the extender's
  documented ones and have not yet been checked against RUN386's dispatch.
* **The runtime has the CPU, not yet the machine**: no VRAM, sprites,
  sound, CD, timers or DOS behind `x86_int`, `x86_in`/`x86_out` and
  `x86_load_seg` -- the self-test's stub stops there.
* **The emitter computes every flag it sets** (no liveness pass yet), and
  its clocks are the datasheet's, one figure a form, not yet held against
  the oracle.
* **Self-modifying code** is not met (the first game's code is the file's,
  byte for byte, in RAM): the recompiled code would not see it.
* **The P3 header** past the image fields (the extra pages, the run-time
  parameters) is read but not yet understood: the first disc's says
  0x2009 extra pages minimum and maximum.
