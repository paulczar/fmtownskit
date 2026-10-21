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
the disc's TBIOS runs the Towns path, and needs no ROM. A later disc may
carry that TBIOS in modules, which a loader lays out at boot (`tbiosld`):
the same entry table at selector 0x110, the same place in memory.

## Ports built on it

| Port | Game | What it asked of fmtownskit |
|---|---|---|
| pc-thequeenofduellist | The Queen of Duellist (1993, Agumix; FM Towns CD, Rev A, runs on the Marty) | where the kit was born: the cue sheet with one file per track and audio in the pregaps, ISO 9660 and the IPL record (`disc`), the Phar Lap P3 executable (`exp`), both machines' memory maps, ports, IRQs and machine ids (`towns`), and the map of a program's system surface -- DOS and Phar Lap calls, the CD BIOS, ports, selectors, far calls into TBIOS (`x86map`); RUN386's selectors and paging read on both machines running (`mamedump`); the 386 decoder, the interpreter, discovery with its jump tables, the emitter, the runtime's CPU and the self-test (`i386`, `i386emu`, `recomp`, `runtime/x86*`), on the game and on TBIOS; the machine under them (`runtime/towns*`): memory and selectors as RUN386 leaves them, far calls into TBIOS, the program's interrupt handlers, VSYNC, the CRTC, palettes, VRAM and the sprite controller, the RF5C68's channels, DOS, Phar Lap and the CD BIOS over the disc image -- the logo, the opening and the title drawn as MAME draws them, pixel for pixel; the sound (`runtime/towns_sound.cpp`: the RF5C68, the YM3438 on ymfm, the CD's audio, mixed as MAME's speakers at its stream rates and resampling) held against MAME's WAV by replaying the oracle's sound log; the window and the Towns pad on SDL3 (`runtime/towns_sdl.*`, optional) |
| pc-4ddriving | 4D Driving (1993, Electronic Arts Victor: *Stunts*; FM Towns Marty rerelease, Marty only) | a compiled program ten times the first's (MetaWare High C, 1,273 functions): the P3 **symbol table** Phar Lap's linker keeps (`exp`), its symbols as discovery's seeds, its sites' names and the bound of a jump table read with none (`x86map`, `recomp.discover`) -- a table of handlers filled at run time had been read on into the next variables; the P3 header's "extra" fields, bytes; the **x87's escapes** decoded and run as a 386 with no coprocessor runs them (`i386`, `i386emu`, `recomp.emit`), for MetaWare's FPU probe; the ports it reads (`towns`); a machine profile as data (`runtime/towns.h`, `towns.PROFILES`), since it asks RUN386 for all memory and MAME's 2 MB Towns leaves it too little; jump tables **placed after their jump** in hand-written drawing code, read by discovery; the symbols as the C++'s names and `--exp ...@CS/END` seeds; the self-test's vectors in slices and processes; `lsl` and `arpl` for MetaWare's start; DOS 30h/44h, Phar Lap's vectors (2502h-2507h), FORRBIOS.NSD's keyboard (int 90h), KimG (int 8Eh) and the CD BIOS raised in protected mode (`runtime/towns_dos`); **the mouse in the second port**, as MAME's fmtowns has it (`runtime/towns_io`) -- its start, logo, title and credits drawn as MAME draws them, pixel for pixel; DOS's **find first and next** (1Ah, 2Fh, 4Eh, 4Fh: the Towns' DTA, on the CD and on host drives) and **free space** (36h), the CD BIOS's 53h and 55h **while a track plays** (`runtime/towns_dos`, `towns_disc`), the host's mouse in the window (`runtime/towns_sdl`) -- its menus, a race driven as on MAME, its CD-DA and engine held against MAME's WAV, the user disk on the host |
| pc-abelsmt | ABEL - Shin Mokushiroku Taisen (1995, Family Soft; FM Towns CD, 4 MB) | three programs, each a P3 **bound behind a DOS stub** (Fujitsu's "exp2exe", an MZ that starts RUN386 on its own file: `exp`); a **TBIOS in modules** (V31L35: TBIOSLD.SYS lays out TBIOS.INI's eight Phar Lap "MQ" relocatable images, links their exports and imports into the entry table: `tbiosld`, composed from the disc's files and held against the oracle's memory); `x86map`'s far calls told from MetaWare's switches (`jmp cs:[reg*4 + table]`, its table after it, had been listed as far calls), a compare's bound dropped when the index is rewritten after it, and signed bounds (`jg`) read by `x86map` and `recomp.discover` |

## Using it

fmtownskit is its own repository. A port takes it as a git submodule at
`fmtownskit/`, so that `python -m fmtownskit.…` works from the port's root.

```sh
python -m fmtownskit.disc GAME.cue                        # tracks, volume, IPL
python -m fmtownskit.disc GAME.cue --list                 # every file
python -m fmtownskit.disc GAME.cue --extract build/disc   # every file, and sectors 0-15
python -m fmtownskit.disc GAME.cue --wav build/cdda       # each CD-DA track from INDEX 01
python -m fmtownskit.exp build/disc/GAME.EXP              # the P3 header
python -m fmtownskit.exp build/disc/GAME.EXP --symbols    # its symbol table, if it kept one
python -m fmtownskit.recomp.discover GAME.EXP --symbol-seeds END --report   # every symbol below END a seed
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
python -m fmtownskit.recomp --out build/recomp --exp game=GAME.EXP@0c/END+SEEDS \
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
| 1. Recognise | What is on this disc? | `disc` (cue sheets, raw and cooked tracks, ISO 9660, the IPL record, the tracks' positions and pregaps), `exp` (Phar Lap P3, its symbol table, a P3 bound behind an MZ stub), `tbiosld` (a TBIOS of Phar Lap "MQ" modules, laid out and linked as TBIOSLD.SYS does) | MZ executables and RUN386's own loader; floppy images (.hdm) for the discs that come with a user disk |
| 2. Extract | Turn standard formats into standard files | `disc --extract`, `disc --wav` | the TBIOS-level formats games share (sprite patterns, the sound BIOS's data), when a second game confirms them |
| 3. Map code | What does the code do, where? | `x86map` (descent, extents, every `int` with AH/AL, every port, every selector loaded, far calls through TBIOS's table, indirect transfers), `mamedump` (a running machine: GDT, LDT, paging, bytes found) | the TBIOS's own functions named by module and AH |
| 4. Translate | Turn 386 code into C++ | `i386` (the 386's integer instruction set exactly: 16- and 32-bit operand and address sizes, segment overrides, REP/LOCK, ModRM/SIB, the 0F map; later CPUs' instructions refused with a reason; the x87's escapes as a 386 with no coprocessor runs them), `i386emu` (an interpreter: the reference, its undefined flags listed), `recomp.discover` (functions and their bodies, jump tables bounded by the guard that really guards them -- or by `setcc`, `and` --, tables with no bound read until an entry's code conflicts or is refused, or to the next symbol, the descent rerun until clean; a program's symbols as seeds), `recomp.emit` and `python -m fmtownskit.recomp` (C++ per function, a module per program keyed by its code selector, calls as C++ calls with the return checked and unwound, jump tables as switches on the entry read, the 386's clocks a block at a time), `recomp.selftest` (the interpreter records, the C++ replays; the optest: every instruction form, one-instruction functions) | flags only where read; the clocks held against the oracle |
| 5. Runtime | What an engine links | for recompiled code (C++20): `x86.h` (the CPU, linear memory as a table of 4 KB pages, the flags), `x86_flags` (shifts, multiply and divide, bits, BCD, strings, POPF, ENTER as `i386emu` does them), `x86_core` (modules, lookup by code selector), `x86_selftest`; the machine (`towns.h`, `towns.cmake`): a `Profile` of the machine as data (its RAM, where RUN386 puts the program and how large, selector 0x40), `towns_mem` (linear memory as RUN386 leaves it, the descriptors, VRAM's mask and windows, the PCM wave RAM's bank, the font ROM; the state TownsOS hands over), `towns_cpu` (far calls and returns between a program and TBIOS, calls by code selector, `int`, interrupts taken at safe points), `towns_io` (the ports, level-triggered PICs, VSYNC and the beam, the microsecond counter, the sprite controller, the RF5C68's channels and its interrupt, the mouse in the second port), `towns_video` (sprites drawn and the frame composed as MAME's `screen_update`, PNG), `towns_dos` (DOS and Phar Lap over the disc image, the CD BIOS's TOC, play, pause and stop -- through Phar Lap's 2511h or raised in protected mode --, FORRBIOS.NSD's keyboard and KimG, RUN386's start of a P3 program), `towns_disc` (cue sheets, ISO 9660, audio sectors) | the YM3438 and the RF5C68 heard, the CD-DA played, a window and the pad; the Marty's speed and 2 MB for its oracle |

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
| `exp` | the same disc's `DUEL.EXP`: a level-1 P3 image of 73,495 bytes at 0x200, entry 0xA7F0, stack top 0x13F20, no relocations. 4D Driving's `4DDFMT.EXP`: 290,132 bytes, entry 0x340A4 (`_mwINIT`), and its SYM1 table read to the last byte of both sizes: 2 segments, 2,047 symbols. On both, image + the minimum "extra" = the initial ESP. ABEL's three programs, each behind the same 0x850-byte MZ stub, the P3 where the MZ header's page count ends: `ABEL.EXE` 648,423 bytes of image (entry 0x40D20), `OPNG.EXE` 299,936 (0x27D14), `ENDG.EXE` 170,187 (0x11444), image + minimum = ESP on all three; the P3 read from behind the stub the same, byte for byte, as cut out by hand |
| `tbiosld` | ABEL's TBIOS: the eight modules of its TBIOS.INI composed from the disc's files (90,464 bytes: TBIOSTOP at 0, the graphic module at 0x300 ... the font module to 0x16160) against the TBIOS in the oracle's memory 66 s into the game (`mamedump --read 0x100000:0x17000`): the entry table the same but for slot 0x98 (the game's), and the 449 other bytes that differ all lie between a module's header and its first code -- its work areas, filled as it runs |
| `towns` | the memory maps, ports, IRQs and machine ids as MAME's fmtowns driver has them; confirmed by a game's code: the pad ports 0x4D0/0x4D2/0x4D6, the CRTC's 0x440/0x442/0x443, the sprite controller's 0x450/0x452, the sound status at 0x4E9 and IRQ13 as int 4Dh (slave PIC at 0x48) |
| `towns.PROFILES` | 4D Driving's dumps on MAME's `fmtowns` with 2 and 4 MB and `fmtmarty`: the program's segment at 0x200000 with 2 MB but **0x408000 with 4 MB** (physical 0x49000 both), its limit when it asks for all memory, selector 0x40 short of the RAM by the 32 KB a driver keeps; nothing else in the tables differs between 2 and 4 MB |
| `towns.SELECTORS`, `PAGES_MARTY` | the descriptor tables of MAME's `fmtowns` and `fmtmarty` running The Queen of Duellist, 20 seconds in: GDT and LDT the same base for base on both (the TBIOS's limit aside, 0x16FFF and 0x18FFF), paging on in both, the Towns' high pages 1:1, the Marty's at the seven mappings listed; the game's image at linear 0x200000 on both (physical 0x49000 and 0x47000), its code byte for byte the file's |
| `mamedump` | the same two dumps: descriptors, the page walk, the image and the TBIOS found in RAM (the Towns' TBIOS the disc's `TBIOS.BIN` but for 265 bytes of data; the Marty's found in its ROM `MAR_EX1` at 0x19C00) |
| `i386` | every instruction discovery reaches in `DUEL.EXP` (7,903), `TBIOS.BIN` (16,120) and `4DDFMT.EXP` (68,033 with its symbols as seeds, x87 escapes among them) against capstone, field by field: 0 differ. 300,000 random byte sequences: 290,438 agree, 11 differ and each is capstone's error (a 67h `jmp rel32` truncated to 16 bits, a REP or LOCK prefix dropped, the test registers TR6/TR7 unknown), 1,159 are escapes capstone has no x87 reading for |
| `i386emu` | 10 known answers; against unicorn from random states, memory mapped on demand: 30,020 vectors over `DUEL.EXP`'s instructions, 46,545 over TBIOS's, 169,696 over random instructions -- 0 differ. Set aside: the machine's instructions, the flags Intel leaves undefined, and unicorn's own two errors (`ret imm16` sign-extended; F6/F7 /1, TEST's second encoding, refused) |
| `recomp.discover` | `DUEL.EXP`: 157 functions, 0 conflicts. `4DDFMT.EXP` with its 1,218 code symbols as seeds: 1,273 functions, 66,927 instructions, 25 jump tables (21 bounded), 0 refusals, 0 conflicts; with the tables placed after their jumps read (19, each as long as the distance to its lowest target) and eight seeds more, 1,289 functions, 68,823 instructions, 45 tables, 0 conflicts. ABEL's `ABEL.EXE` from its entry: 1,522 functions, 81,184 instructions, 199 jump tables (192 bounded), 0 refusals, and 4 conflicts that are the code's own (a jump into the middle of `66 A5`, where `A5` alone is the `movsd` of a shorter copy); `OPNG.EXE` 605 functions, `ENDG.EXE` 451, 0 refusals, 0 conflicts; the signed bound (`jle`/`jg`) of two tables of 21 handlers each, read before to 45 and 24 entries, into the next table and the data. The first two games' tables the same, entry for entry, with the signed bounds. `TBIOS.BIN` from its far-pointer table: 355 functions, 14,385 instructions, 33 jump tables (11 bounded), 0 refusals, 0 conflicts -- where a first descent had 260 conflicts and 14 refusals from a table bounded by a comparison of a register rewritten before the jump |
| `recomp.emit`, `recomp.selftest` | the optest: 7,464 one-instruction functions (every distinct instruction of both programs and of a random sweep), 22,340 vectors from `i386emu`, replayed on the C++: 0 failures. The programs' own functions, 3 random states each: `DUEL.EXP` 135 vectors, TBIOS 682, 0 failures. Every generated file compiles with clang. With the x87's escapes: 7,652 functions, 22,909 vectors, 0 failures; the first game's and TBIOS's C++ unchanged byte for byte. 4D Driving, its TBIOS and an optest of 18,259 functions: 56,974 vectors, 0 failures (each function seeded by its address since; the first game: 23,525, 0 failures) |
| `runtime/towns*` | The Queen of Duellist run from its first instruction, the state at that instruction read on MAME's `fmtowns` (registers, RAM, the PICs' save items, the CRTC, video registers and palettes through the ports): memory blocks, selectors and bases as RUN386 gave them; TBIOS set up by the game's own calls; the logo, its fades, the opening and the title screen with its menu (the font ROM's text) the same as MAME's snapshots pixel for pixel, at every moment tried; the attract fight run, its sprites, HUD and backgrounds drawn. 4D Driving on `fmtowns -ramsize 4M`, its start read the same way: its user-disk prompt, Electronic Arts' logo, its title and its credits, 26 snapshots the same pixel for pixel (MAME's mouse in the second port is what made the last pixels agree) |
| `x86map` | `ABEL.EXE`: 1,560 functions from the entry; its 194 MetaWare switches and calls through tables addressed by CS (`jmp cs:[reg*4 + table]`, `call cs:[reg*4 + table]`) no longer listed as far calls (228 "far calls" before, 34 now, all through TBIOS's table); the bound of `cmp al, 0xF0; jae` no longer taken for an index rewritten after it (`sub ebx, 0x80; shr ebx, 4`: 240 entries read into code, the addresses "undecodable" that came of them gone). The first two games' maps the same but for two MetaWare switches of 4D Driving's library, listed as far calls before. `4DDFMT.EXP`: 724 functions from the entry, 1,276 with its symbols, each site named by its function. `DUEL.EXP`: 154 functions from the entry (157 with three seeds), 27,124 bytes of code in 0xA7F0-0x11B98, every site named (33 `int`, 18 `out`, 6 `in`, 9 far calls into TBIOS, 48 selector loads -- of 0x14, 0x104, 0x110 and 0x130 only, a 32-bit register's low half included), 12 indirect transfers listed |

## Known gaps

* **Three discs.** Everything above is what three games show; the third
  (ABEL) only read, not yet run. The runtime's machine is the
  Towns with the disc's TBIOS, its RAM and RUN386's placing a profile; the
  Marty's own TBIOS (its ROM's extension: a TV zoom, the pad as a mouse) is
  not run.
* **Profiles are read, not derived**: how RUN386 places a program by RAM
  size and drivers is a table of what the dumps showed, not RUN386's rule.
* **RUN386's selectors** are read from two machines running, not from
  RUN386's own code; the blocks `int 21h AH=48h` hands out (LDT 0x44 up)
  are placed by it as the dumps show, not yet by a rule.
* **Phar Lap's function names** (`towns.PHARLAP`) are the extender's
  documented ones and have not yet been checked against RUN386's dispatch.
* **The sound** was held against the oracle on both games (the second plays
  no FM: the YM3438's FM voices were held only by `fmtest.lua`'s notes on
  the first). The ports are what two games and their TBIOS touch; anything
  else is logged once.
* **FORRBIOS.NSD's services are answered as one game saw them**: the
  keyboard with no key ever pressed (the matrix *inferred* empty), the CD
  BIOS's 53h-56h as one game's calls met them; its 91h, 96h, 97h not at
  all.
* **A host drive is a 2HD floppy** to DOS's 36h (*inferred*: 1,232 sectors
  of 1,024 bytes); the Towns' DOS's DTA for a find on a host drive is the
  one read for the CD, with the host's attributes.
* **The CMOS RAM** reads 0 (MAME's comes from its NVRAM; the two addresses
  the second game reads hold 0 there too).
* **Loads take no time**: a file is read from the image at once, where the
  Towns reads its CD at the drive's pace -- the same pictures come sooner
  than on the oracle, and what hangs on timing (a computer's play) drifts.
* **Interrupts are taken at safe points** (backward jumps; the budget set
  to the next event), not between any two instructions.
* **The emitter computes every flag it sets** (no liveness pass yet), and
  its clocks are the datasheet's, one figure a form, not yet held against
  the oracle. MAME's 386 counts the datasheet's clocks too, with no wait
  states: a game paced by its own CPU, not by a timer, runs on MAME faster
  than on a Towns (MAME's list says so of ABEL: "Runs too fast"), and would
  in a port the same. Calibrating against the real machine is open.
* **A TBIOS in modules is read from one disc** (ABEL's, V31L35): the
  loader's rules (`tbiosld`) are what that disc's eight modules and its
  memory show, not TBIOSLD.SYS's code read.
* **Self-modifying code** is not met (the first game's code is the file's,
  byte for byte, in RAM): the recompiled code would not see it.
* **The P3 header's run-time parameters** ("DX" first) are read but not
  yet understood. Its "extra" fields are bytes past the image (both discs:
  image + minimum = ESP), and a maximum of 0xFFFFFFFF gets all the memory
  there is (0x169FFF of segment on MAME's Towns, 0x184FFF on the Marty).
* **The x87** is not emulated: the machines MAME has have no coprocessor,
  and an escape does nothing, as there. A program that needs a 387 would
  need more.
* **`i386emu --check` on a large program** holds unicorn's memory: 55,000
  instructions with two states each reached 8 GB. Check in slices.

## Licence

MIT (`LICENSE`), but for `runtime/ymfm/`: ymfm by Aaron Giles, BSD 3-Clause
(its `LICENSE` beside it), the files a YM3438 needs, unchanged from MAME's
`3rdparty/ymfm`. The window links SDL3 (zlib licence) when a port builds it.
