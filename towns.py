"""The machines: the FM Towns and the FM Towns Marty, as a program meets them.

The Marty (1993) is an FM Towns in a console's case: the same video, sprite,
sound and CD hardware and the same boot from CD, with a 386SX instead of a
386(DX). The SX has a 24-bit address bus, so everything the Towns maps high
(VRAM at 0x80000000, sprite RAM at 0x81000000, the ROMs at 0xC2000000 and up)
is mapped again below 16 MB on the Marty. A program that reaches the
hardware through the selectors RUN386 gives it (see SELECTORS) does not see
the difference; one that builds linear addresses itself does -- which is what
makes a disc "Marty-compatible" or not, together with the Marty's 2 MB of
RAM, no hard disk and the pad for keyboard and mouse.

Sources: MAME's fmtowns driver (src/mame/fujitsu/fmtowns.cpp, fmtowns_v.cpp),
read for the addresses and ports below, not copied; where a game's own code
confirms a port it is said. The tables are data for the other modules
(`x86map` names what a program touches with them).

    python -m fmtownskit.towns              # both memory maps, the ports
"""

# Linear addresses as the CPU sees them: (start, end inclusive, what).
MEMORY = {
    'towns': [   # Model 1/2 (386DX) and later 386DX/486 models
        (0x00000000, 0x000BFFFF, 'RAM (first 768 KB)'),
        (0x000C0000, 0x000C7FFF, 'FM-R compatible VRAM window (plane access)'),
        (0x000C8000, 0x000CAFFF, 'text/sprite RAM window (low)'),
        (0x000CFF80, 0x000CFFFF, 'FM-R video registers (memory-mapped)'),
        (0x000D8000, 0x000D9FFF, 'CMOS (battery-backed RAM, low window)'),
        (0x000F8000, 0x000FFFFF, 'boot ROM or RAM (banked)'),
        (0x00100000, 0x7FFFFFFF, 'RAM above 1 MB (as much as is fitted)'),
        (0x80000000, 0x8007FFFF, 'VRAM, layered access (mirrored to 0x800FFFFF)'),
        (0x80100000, 0x8017FFFF, 'VRAM, single-layer access (mirrored)'),
        (0x81000000, 0x8101FFFF, 'sprite RAM (attributes, patterns, palettes)'),
        (0xC0000000, 0xC1FFFFFF, 'IC memory card'),
        (0xC2000000, 0xC207FFFF, 'OS ROM (FMT_DOS)'),
        (0xC2080000, 0xC20FFFFF, 'dictionary ROM (FMT_DIC)'),
        (0xC2100000, 0xC213FFFF, 'font ROM (FMT_FNT)'),
        (0xC2140000, 0xC2141FFF, 'CMOS'),
        (0xC2180000, 0xC21FFFFF, 'F20 ROM (FMT_F20)'),
        (0xC2200000, 0xC2200FFF, 'PCM wave RAM window (RF5C68)'),
        (0xFFFC0000, 0xFFFFFFFF, 'system ROM (FMT_SYS)'),
    ],
    'marty': [   # 386SX: 24-bit bus; MAME's fmtmarty map
        (0x00000000, 0x000BFFFF, 'RAM (first 768 KB)'),
        (0x000C0000, 0x000FFFFF, 'the same low windows as the Towns'),
        (0x00100000, 0x001FFFFF, 'RAM to 2 MB'),
        (0x00600000, 0x0067FFFF, 'OS ROM'),
        (0x00680000, 0x0087FFFF, 'the Marty\'s extended ROM (MAR_EX0-3, 2 MB)'),
        (0x00A00000, 0x00A7FFFF, 'VRAM, layered access (mirrored)'),
        (0x00B00000, 0x00B7FFFF, 'VRAM, single-layer access (mirrored)'),
        (0x00C00000, 0x00C1FFFF, 'sprite RAM'),
        (0x00D00000, 0x00DFFFFF, 'IC memory card'),
        (0x00E80000, 0x00EFFFFF, 'dictionary ROM'),
        (0x00F00000, 0x00F7FFFF, 'font ROM'),
        (0x00F80000, 0x00F80FFF, 'PCM wave RAM window (RF5C68)'),
        (0x00FC0000, 0x00FFFFFF, 'system ROM'),
    ],
}

# I/O ports (byte addresses).
PORTS = {
    0x0000: 'PIC master, command/status', 0x0002: 'PIC master, mask',
    0x0010: 'PIC slave, command/status', 0x0012: 'PIC slave, mask',
    0x0020: 'system (0x20-0x33: reset, CPU and machine id)',
    0x0040: 'PIT 0', 0x0042: 'PIT 0', 0x0044: 'PIT 0', 0x0046: 'PIT 0 control',
    0x0050: 'PIT 1', 0x0052: 'PIT 1', 0x0054: 'PIT 1', 0x0056: 'PIT 1 control',
    0x0060: 'timer interrupt control', 0x0068: 'interval timer 2', 0x006C: 'system (0x6c)',
    0x0070: 'RTC data', 0x0080: 'RTC select',
    0x0200: 'FDC',
    0x0400: 'video: unknown (read)', 0x0404: 'video: VRAM access mode',
    0x0440: 'CRTC register select', 0x0442: 'CRTC data, low byte', 0x0443: 'CRTC data, high byte',
    0x0448: 'video output control select', 0x044A: 'video output control data',
    0x044C: 'video (0x44c)',
    0x0450: 'sprite controller register select', 0x0452: 'sprite controller data',
    0x0458: 'VRAM write mask select', 0x045A: 'VRAM write mask', 0x045B: 'VRAM write mask',
    0x0480: 'system: boot ROM / RAM banking',
    0x04C0: 'CD-ROM controller', 0x04C2: 'CD-ROM controller', 0x04C4: 'CD-ROM controller',
    0x04C6: 'CD-ROM controller', 0x04C8: 'CD-ROM controller', 0x04CA: 'CD-ROM controller',
    0x04CC: 'CD-ROM controller',
    0x04D0: 'pad port 1 (read)', 0x04D2: 'pad port 2 (read)', 0x04D6: 'pad output lines (write)',
    0x04D8: 'YM3438 (OPN2) address 0', 0x04DA: 'YM3438 data 0',
    0x04DC: 'YM3438 address 1', 0x04DE: 'YM3438 data 1',
    0x04E0: 'electronic volume', 0x04E2: 'electronic volume',
    0x04E8: 'sound control', 0x04E9: 'sound interrupt status (bit 0 FM, bit 3 PCM)',
    0x04EA: 'PCM channel interrupt mask', 0x04EB: 'PCM channel interrupt flags (read clears)',
    0x04EC: 'sound control (LED/mute)',
    0x04F0: 'RF5C68 envelope', 0x04F1: 'RF5C68 pan', 0x04F2: 'RF5C68 frequency low',
    0x04F3: 'RF5C68 frequency high', 0x04F4: 'RF5C68 loop low', 0x04F5: 'RF5C68 loop high',
    0x04F6: 'RF5C68 start', 0x04F7: 'RF5C68 control', 0x04F8: 'RF5C68 channel on/off',
    0x05C8: 'video (0x5c8: TVRAM)', 0x05CA: 'video (0x5ca: VSYNC interrupt clear)',
    0x05E8: 'system (0x5e8)',
    0x0600: 'keyboard', 0x0602: 'keyboard', 0x0604: 'keyboard',
    0x0A00: 'serial',
}

# IRQs per the driver. In protected mode under TownsOS the master PIC's
# vectors start at 0x40 and the slave's at 0x48 (IRQ n is int 0x40+n): a
# game hooking int 4Dh and unmasking bit 5 of port 0x12 takes IRQ13.
IRQS = {0: 'PIT timer', 1: 'keyboard', 2: 'serial', 6: 'FDC', 7: 'slave PIC',
        8: 'SCSI', 9: 'CD-ROM', 11: 'VSYNC', 12: 'printer',
        13: 'sound (YM3438 timers, RF5C68 channels), mouse', 15: '16-bit PCM (expansion)'}
IRQ_BASE = {'master': 0x40, 'slave': 0x48}

# Port 0x31, the machine id, as the driver lists it (a program can tell a
# Marty from a Towns here).
MACHINE_ID = {1: 'FM Towns 1/2', 2: 'FM Towns 1F/2F/1H/2H', 3: 'FM Towns 10F/20F/40H/80H',
              4: 'FM Towns II UX', 5: 'CX', 6: 'UG', 7: 'HR', 8: 'HG', 9: 'UR', 11: 'MA',
              12: 'MX', 13: 'ME', 15: 'MF/Fresh TV', 16: 'SN', 17: 'HA/HB/HC',
              0x4A: 'FM Towns Marty'}

# Selectors as RUN386 1.2aJ L50 sets them up, read from the descriptor
# tables of a running Towns and a running Marty (`mamedump`): the same
# linear bases on both. RUN386 runs with paging on, on both machines; on
# the Towns the hardware's pages map 1:1, on the Marty they map to its
# 24-bit addresses (PAGES_MARTY), so a program sees one machine.
# LDT (TI=1) are the program's; GDT ones are the system's, open to it.
SELECTORS = {
    0x000C: 'the program\'s code (LDT; base 0x200000 for the first program, limit its image + stack)',
    0x0014: 'the program\'s data, also SS (LDT; the same base as 0x0C)',
    0x001C: 'VRAM, layered (LDT; Phar Lap\'s "screen")',
    0x0024: 'the PSP (LDT; 256 bytes in the first MB: inferred by size, as Phar Lap documents it)',
    0x002C: 'the environment (LDT; inferred, as Phar Lap documents it)',
    0x0034: 'the first megabyte (LDT)',
    0x0040: 'all RAM (GDT; limit the RAM size - 1)',
    0x0048: 'VRAM, layered (GDT)',
    0x0060: 'the first megabyte (GDT)',
    0x0100: 'system ROM, code (GDT; 0xFFFC0000)',
    0x0104: 'VRAM, layered (LDT; 0x80000000, 512 KB)',
    0x0108: 'system ROM, data (GDT)',
    0x010C: 'VRAM, single-layer (LDT; 0x80100000, 512 KB)',
    0x0110: 'TBIOS code (GDT; base 0x100000, limit TBIOS\'s size - 1): its entry table at 0',
    0x0114: 'sprite RAM (LDT; 0x81000000, 128 KB)',
    0x0118: 'TBIOS data (GDT; the same base and limit as 0x110)',
    0x0120: 'VRAM, layered (GDT)',
    0x0128: 'VRAM, single-layer (GDT)',
    0x0130: 'sprite RAM (GDT; 0x81000000, 128 KB)',
    0x0138: 'font ROM (GDT; 0xC2100000)',
    0x0140: 'PCM wave RAM window (GDT; 0xC2200000, 4 KB)',
    0x0148: 'OS ROM (GDT; 0xC2000000)',
}
# int 21h AH=48h hands a program a new LDT selector per block (0x44, 0x4C,
# ... upward), its base above the program's in linear memory.

# The Marty's page mappings for the Towns' high addresses (linear -> physical, size).
PAGES_MARTY = [
    (0x80000000, 0x00A00000, 0x80000), (0x80100000, 0x00B00000, 0x80000),
    (0x81000000, 0x00C00000, 0x20000), (0xC2000000, 0x00600000, 0x80000),
    (0xC2100000, 0x00F00000, 0x42000), (0xC2200000, 0x00F80000, 0x1000),
    (0xFFFC0000, 0x00FC0000, 0x40000),
]

# Software interrupts a Towns program may raise. 21h is DOS (and the
# extender's own functions at AH=25h); 93h is the Towns disk BIOS, whose
# AL is the device (0xC0 the CD-ROM).
INTS = {0x21: 'DOS / Phar Lap', 0x93: 'disk BIOS (AL: device; 0xC0 CD-ROM)'}

DOS = {
    0x09: 'print string', 0x19: 'current drive', 0x25: 'Phar Lap (AL: function)',
    0x3C: 'create file', 0x3D: 'open file', 0x3E: 'close file', 0x3F: 'read file',
    0x40: 'write file', 0x42: 'seek', 0x48: 'allocate memory', 0x49: 'free memory',
    0x4A: 'resize memory', 0x4C: 'exit',
}
# Phar Lap's AH=25h functions, by AL, as the 386|DOS-Extender reference names
# them; checked against RUN386's own dispatch before the runtime relies on one.
PHARLAP = {
    0x02: 'get protected-mode interrupt vector', 0x03: 'get real-mode interrupt vector',
    0x04: 'set protected-mode interrupt vector', 0x05: 'set real-mode interrupt vector',
    0x06: 'set interrupt to always gain control in protected mode',
    0x0D: 'get real-mode link information', 0x0E: 'call real-mode procedure',
    0x0F: 'convert protected-mode address to MS-DOS address',
    0x11: 'issue real-mode interrupt',
}


def port_name(p):
    return PORTS.get(p) or PORTS.get(p & ~1, '?')


def main():
    for m, rows in MEMORY.items():
        print(m)
        for a, b, what in rows:
            print('  %08x-%08x  %s' % (a, b, what))
    print('ports')
    for p in sorted(PORTS):
        print('  %04x  %s' % (p, PORTS[p]))


if __name__ == '__main__':
    main()
