"""fmtownskit - game-agnostic building blocks for FM Towns and FM Towns Marty ports.

Each module handles one thing the machine, its boot or its system software
imposes on every program, independent of any particular title:

    disc     CD images (.cue with one or many files, raw .bin/.img, .iso):
             the tracks and their pregaps, ISO 9660, the IPL record,
             extraction, CD-DA to WAV
    exp      Phar Lap 386|DOS-Extender executables (.EXP, P3 flat)
    towns    the machines: both memory maps (Towns and Marty), I/O ports,
             IRQs, machine ids, RUN386's selectors, DOS/Phar Lap names
    x86map   a program's 32-bit code: extents, functions, and every site
             where it reaches the system (needs capstone)
    mamedump a running machine frozen by MAME (CPU state, physical RAM):
             paging, the GDT and LDT, bytes found

Run as `python -m fmtownskit.<module>` from a port's root, where the kit is
a submodule. Pure Python 3.8+; game-specific knowledge belongs in the
game's own tools/, not here.
"""
