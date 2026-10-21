// fmtownskit runtime -- what a Towns program asks by interrupt, answered at the interrupt:
// DOS and Phar Lap's 386|DOS-Extender (int 21h), the CD BIOS (int 93h), int FDh.
//
// Files: the disc's own (drive Q:, the CD, and names with no drive while the current drive
// is Q:), read from the image; any other drive is a directory on the host (save_dir), for
// what a game writes. Memory: int 21h AH=48h hands out a new LDT selector per block, each
// block page-aligned after the program's segment and the one before (as RUN386 does on the
// Towns: the dumps of tools/oracle.py). The values Phar Lap's 250Dh returns are the ones
// RUN386 returned on the Towns (the oracle's RAM).
#include "towns.h"
#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>

namespace towns {

static void set_cf(X86Cpu& c, bool cf) { c.fl = cf ? (c.fl | X86_CF) : (c.fl & ~X86_CF); }

static std::string cstr(Machine& m, uint32_t lin, uint32_t max = 260) {
    std::string s;
    for (uint32_t k = 0; k < max; ++k) {
        char ch = (char)m.mem_read(lin + k, 1);
        if (!ch) break;
        s += ch;
    }
    return s;
}

static int new_handle(Machine& m) {
    for (int h = 5; h < (int)m.handles.size(); ++h) if (!m.handles[h].used) return h;
    return -1;
}

// "Q:\AGUMIX.PX", "A:dueltwn.sav", "DUEL.EXP": the disc's, or a host file in save_dir.
static bool on_disc(Machine& m, const std::string& name, std::string& path) {
    char drive = m.drive;
    path = name;
    if (name.size() >= 2 && name[1] == ':') { drive = (char)std::toupper((unsigned char)name[0]); path = name.substr(2); }
    if (drive == 'Q') return true;
    while (!path.empty() && (path[0] == '\\' || path[0] == '/')) path.erase(0, 1);
    for (auto& ch : path) {
        ch = (char)std::tolower((unsigned char)ch);
        if (ch == '\\') ch = '/';
    }
    path = m.save_dir + "/" + std::string(1, (char)std::tolower(drive)) + "/" + path;
    return false;
}

void Machine::int21(X86Cpu& c) {
    uint8_t ah = (c.r[0] >> 8) & 0xFF, al = c.r[0] & 0xFF;
    uint32_t ds = c.sbase[3];
    if (trace_int) std::fprintf(stderr, "int 21h AX=%04X EBX=%08X ECX=%08X EDX=%08X\n", c.r[0] & 0xFFFF, c.r[3], c.r[1], c.r[2]);
    switch (ah) {
    case 0x09: {                                    // print a string ending in '$'
        std::string s;
        for (uint32_t k = 0; k < 1024; ++k) {
            char ch = (char)mem_read(ds + c.r[2] + k, 1);
            if (ch == '$') break;
            s += ch;
        }
        std::fprintf(stderr, "dos: %s\n", s.c_str());
        return;
    }
    case 0x19: s8(c, 0, (uint32_t)(drive - 'A')); return;
    case 0x25: {                                    // Phar Lap
        switch (al) {
        case 0x06:                                  // a vector always taken in protected mode
            pm_vector[c.r[1] & 0xFF] = {c.sreg[3], c.r[2], true};
            set_cf(c, false);
            return;
        case 0x0D:                                  // real-mode link information (RUN386's, on the Towns)
            c.r[0] = 0x2B9504B1u;                   // the real-mode call-back
            c.r[3] = (rm_buffer >> 4) << 16;        // the buffer, real mode (seg:off)
            c.r[1] = 0x400;                         // its size (*inferred*: Phar Lap's default 1 KB)
            x86_load_seg(c, 0, 0x60);
            c.r[2] = rm_buffer;                     // and in protected mode, ES:EDX
            set_cf(c, false);
            return;
        case 0x0E:                                  // call a real-mode procedure: nothing to run here
            set_cf(c, false);
            return;
        case 0x0F: {                                // a protected-mode address made real (the program's
            uint32_t lin = c.sbase[0] + c.r[3];     // image is below 1 MB on the Towns: physical 0x49000)
            uint32_t phys = lin >= 0x200000 && lin < 0x300000 ? lin - 0x200000 + 0x49000 : lin;
            c.r[1] = ((phys >> 4) << 16) | (phys & 0xF);
            set_cf(c, false);
            return;
        }
        case 0x11: {                                // issue a real-mode interrupt: the block at DS:EDX
            uint32_t blk = ds + c.r[2];
            uint32_t num = mem_read(blk, 2);
            uint32_t rds = mem_read(blk + 2, 2), res = mem_read(blk + 4, 2);
            uint32_t eax = mem_read(blk + 0xA, 4), edx = mem_read(blk + 0xE, 4);
            if (num != 0x93) {
                char why[64];
                std::snprintf(why, sizeof why, "real-mode int %02Xh through 2511h: no service", num);
                x86_fault(c, c.eip, why);
            }
            uint32_t save_edx = c.r[2];
            c.r[2] = edx;
            uint32_t ret = eax;
            int93(c, eax, ret, rds, res);
            mem_write(blk + 0xA, 4, ret);
            c.r[0] = ret;
            c.r[2] = save_edx;
            return;
        }
        default: break;
        }
        break;
    }
    case 0x3C: case 0x3D: {                         // create, open
        std::string name = cstr(*this, ds + c.r[2]), path;
        int h = new_handle(*this);
        if (h < 0) { set_cf(c, true); s16(c, 0, 4); return; }
        Handle& hd = handles[h];
        if (on_disc(*this, name, path)) {
            if (ah == 0x3C || !disc.file(path, hd.data)) {
                if (trace_int) std::fprintf(stderr, "  %s: not on the disc\n", name.c_str());
                set_cf(c, true); s16(c, 0, ah == 0x3C ? 5 : 2); return;
            }
            hd.host = false;
        } else {
            if (ah == 0x3C) std::filesystem::create_directories(std::filesystem::path(path).parent_path());
            hd.f = std::fopen(path.c_str(), ah == 0x3C ? "w+b" : ((al & 3) ? "r+b" : "rb"));
            if (!hd.f) { set_cf(c, true); s16(c, 0, 2); return; }
            hd.host = true;
        }
        hd.used = true; hd.pos = 0;
        if (trace_int) std::fprintf(stderr, "  %s -> handle %d\n", name.c_str(), h);
        s16(c, 0, (uint32_t)h);
        set_cf(c, false);
        return;
    }
    case 0x3E: {
        uint32_t h = c.r[3] & 0xFFFF;
        if (h >= handles.size() || !handles[h].used) { set_cf(c, true); s16(c, 0, 6); return; }
        Handle& hd = handles[h];
        if (hd.f) std::fclose(hd.f);
        hd = Handle{};
        set_cf(c, false);
        return;
    }
    case 0x3F: case 0x40: {                         // read, write: ECX bytes at DS:EDX
        uint32_t h = c.r[3] & 0xFFFF;
        if (h >= handles.size() || !handles[h].used) { set_cf(c, true); s16(c, 0, 6); return; }
        Handle& hd = handles[h];
        uint32_t n = c.r[1], buf = ds + c.r[2];
        std::vector<uint8_t> tmp(n);
        uint32_t done = 0;
        if (ah == 0x3F) {
            if (hd.host) done = (uint32_t)std::fread(tmp.data(), 1, n, hd.f);
            else {
                done = hd.pos < hd.data.size() ? std::min<uint32_t>(n, (uint32_t)hd.data.size() - hd.pos) : 0;
                std::memcpy(tmp.data(), hd.data.data() + hd.pos, done);
                hd.pos += done;
            }
            write_block(buf, tmp.data(), done);
        } else {
            read_block(buf, tmp.data(), n);
            if (!hd.host) { set_cf(c, true); s16(c, 0, 5); return; }
            done = (uint32_t)std::fwrite(tmp.data(), 1, n, hd.f);
        }
        c.r[0] = done;
        set_cf(c, false);
        return;
    }
    case 0x42: {                                    // seek: CX:DX from AL's origin
        uint32_t h = c.r[3] & 0xFFFF;
        if (h >= handles.size() || !handles[h].used) { set_cf(c, true); s16(c, 0, 6); return; }
        Handle& hd = handles[h];
        int32_t off = (int32_t)(((c.r[1] & 0xFFFF) << 16) | (c.r[2] & 0xFFFF));
        uint32_t pos;
        if (hd.host) {
            std::fseek(hd.f, off, al == 0 ? SEEK_SET : al == 1 ? SEEK_CUR : SEEK_END);
            pos = (uint32_t)std::ftell(hd.f);
        } else {
            int64_t p = al == 0 ? off : al == 1 ? (int64_t)hd.pos + off : (int64_t)hd.data.size() + off;
            hd.pos = (uint32_t)std::max<int64_t>(0, p);
            pos = hd.pos;
        }
        s16(c, 0, pos & 0xFFFF);
        s16(c, 2, pos >> 16);
        set_cf(c, false);
        return;
    }
    case 0x48: {                                    // memory: EBX pages of 4 KB, a selector in AX
        uint32_t size = c.r[3] * 4096;
        if (!size || next_block + size > 0x01000000u) { set_cf(c, true); s16(c, 0, 8); return; }
        map_ram(next_block, size);
        uint16_t sel = alloc_ldt(next_block, size - 1);
        if (trace_int) std::fprintf(stderr, "  %u KB at %08x: selector %04x\n", size / 1024, next_block, sel);
        next_block += size;
        s16(c, 0, sel);
        set_cf(c, false);
        return;
    }
    case 0x4C:
        exited = true;
        exit_code = al;
        throw Stop{al};
    default: break;
    }
    char why[64];
    std::snprintf(why, sizeof why, "int 21h AX=%04Xh: no service", c.r[0] & 0xFFFF);
    x86_fault(c, c.eip, why);
}

// The CD BIOS, device AL=0xC0. Buffers are real-mode (DS:DI, as Phar Lap's 2511h passes them).
// MSF positions are binary and count the 2-second lead-in, data tracks with bit 7 of the minute
// (read in the oracle's real-mode buffer after AH=54h).
static void to_msf(uint32_t lba, uint8_t* p) {
    lba += 150;
    p[0] = (uint8_t)(lba / (75 * 60)); p[1] = (uint8_t)(lba / 75 % 60); p[2] = (uint8_t)(lba % 75);
}

static uint32_t from_msf(const uint8_t* p) {
    uint32_t f = ((uint32_t)(p[0] & 0x7F) * 60 + p[1]) * 75 + p[2];
    return f >= 150 ? f - 150 : 0;
}

void Machine::int93(X86Cpu& c, uint32_t eax, uint32_t& ret, uint32_t rm_ds, uint32_t) {
    uint8_t ah = (eax >> 8) & 0xFF, al = eax & 0xFF;
    uint32_t buf = rm_ds * 16 + (c.r[7] & 0xFFFF);
    if (trace_int) std::fprintf(stderr, "int 93h AX=%04X CX=%04X BX=%04X buffer %05x\n", eax & 0xFFFF, c.r[1] & 0xFFFF,
                                c.r[3] & 0xFFFF, buf);
    if (al != 0xC0) {
        char why[64];
        std::snprintf(why, sizeof why, "int 93h AX=%04Xh: not the CD", eax & 0xFFFF);
        x86_fault(c, c.eip, why);
    }
    ret = eax & 0xFFFF00FFu;                         // AH=0: done
    set_cf(c, false);
    switch (ah) {
    case 0x50: {                                    // play from [0..2] to [3..5]
        uint8_t p[6];
        read_block(buf, p, 6);
        cd_play(from_msf(p), from_msf(p + 3));
        cd.repeat = c.r[1] & 0xFF;
        if (trace_int) std::fprintf(stderr, "  CD-DA %u to %u (frame %llu)\n", cd.from, cd.to, (unsigned long long)frame);
        return;
    }
    case 0x52:                                      // stop
        cd_stop();
        if (trace_int) std::fprintf(stderr, "  CD-DA stop (frame %llu)\n", (unsigned long long)frame);
        return;
    case 0x54: {                                    // the table of contents
        const auto& t = disc.tracks();
        std::vector<uint8_t> o(6 + 3 * t.size());
        o[0] = 3;                                   // as RUN386's buffer held it (*not understood*)
        o[1] = (uint8_t)t.front().number;
        o[2] = (uint8_t)t.back().number;
        to_msf(t.back().lba() + t.back().sectors, &o[3]);
        for (size_t k = 0; k < t.size(); ++k) {
            to_msf(t[k].lba(), &o[6 + 3 * k]);
            if (!t[k].audio) o[6 + 3 * k] |= 0x80;
        }
        write_block(buf, o.data(), (uint32_t)o.size());
        return;
    }
    default: break;
    }
    char why[64];
    std::snprintf(why, sizeof why, "int 93h AX=%04Xh: no service", eax & 0xFFFF);
    x86_fault(c, c.eip, why);
}

void Machine::intfd(X86Cpu& c) {
    unknown("int FDh AX=%04X CX=%04X (nothing done)", c.r[0] & 0xFFFF, c.r[1] & 0xFFFF);
}

// ---- RUN386 ------------------------------------------------------------------------------------
bool parse_p3(const std::vector<uint8_t>& f, P3& p, std::string& err) {
    auto u32 = [&](size_t o) { uint32_t v; std::memcpy(&v, &f[o], 4); return v; };
    auto u16 = [&](size_t o) { uint16_t v; std::memcpy(&v, &f[o], 2); return v; };
    if (f.size() < 0x80 || f[0] != 'P' || f[1] != '3') { err = "not a P3 .EXP"; return false; }
    if (u16(2) != 1 || u32(0x18) || u32(0x20)) { err = "not a flat P3 with no relocations"; return false; }
    p.image_offset = u32(0x26); p.image_size = u32(0x2A);
    p.min_extra = u32(0x56); p.max_extra = u32(0x5A); p.base_offset = u32(0x5E);
    p.esp = u32(0x62); p.eip = u32(0x68);
    if (p.image_offset + p.image_size > f.size()) { err = "the image runs past the file"; return false; }
    p.image.assign(f.begin() + p.image_offset, f.begin() + p.image_offset + p.image_size);
    return true;
}

void boot_run386(Machine& m, X86Cpu& c, const P3& p, const std::vector<uint8_t>& tbios) {
    m.map_ram(0, 0xC0000);                              // the first 768 KB (Phar Lap's buffer is there)
    m.map_ram(0x100000, (uint32_t)tbios.size());
    m.write_block(0x100000, tbios.data(), (uint32_t)tbios.size());
    // the program's segment: its image, and the stack up to the page its top lies in (*inferred*:
    // 0x14000 for an image of 0x11F17 and a stack top of 0x13F20)
    uint32_t base = 0x200000;
    uint32_t end = std::max(p.base_offset + (uint32_t)p.image.size(), p.esp);
    uint32_t size = (end + 0xFFF) & ~0xFFFu;
    m.map_ram(base, size);
    m.write_block(base + p.base_offset, p.image.data(), (uint32_t)p.image.size());
    // the descriptors as the Towns has them (towns.SELECTORS)
    const uint32_t tb = (uint32_t)tbios.size() - 1;
    m.set_desc(0x0C, base, size - 1);  m.set_desc(0x14, base, size - 1);
    m.set_desc(0x04, 0xB7870, 0xFF);   m.set_desc(0x24, 0xB7870, 0xFF);   m.set_desc(0x2C, 0xB7820, 0x3E);
    m.set_desc(0x1C, 0x80000000u, 0x7FFFF); m.set_desc(0x34, 0, 0xFFFFF);
    m.set_desc(0x104, 0x80000000u, 0x7FFFF); m.set_desc(0x10C, 0x80100000u, 0x7FFFF); m.set_desc(0x114, 0x81000000u, 0x1FFFF);
    m.set_desc(0x40, 0, 0x1FFFFF); m.set_desc(0x48, 0x80000000u, 0x7FFFF); m.set_desc(0x60, 0, 0xFFFFF);
    m.set_desc(0x100, 0xFFFC0000u, 0x3FFFF); m.set_desc(0x108, 0xFFFC0000u, 0x3FFFF);
    m.set_desc(0x110, 0x100000, tb); m.set_desc(0x118, 0x100000, tb);
    m.set_desc(0x120, 0x80000000u, 0x7FFFF); m.set_desc(0x128, 0x80100000u, 0x7FFFF);
    m.set_desc(0x130, 0x81000000u, 0x1FFFF); m.set_desc(0x138, 0xC2100000u, 0x41FFF);
    m.set_desc(0x140, 0xC2200000u, 0xFFF); m.set_desc(0x148, 0xC2000000u, 0x7FFFF);
    m.next_block = base + size;
    m.next_ldt = 0x44;
    // the registers RUN386 hands over (the Towns, at the first instruction)
    c = X86Cpu{};
    c.fl = 0x202;
    c.r[4] = p.esp;
    x86_load_seg(c, 1, 0x0C);
    for (int n : {0, 2, 3, 4, 5}) x86_load_seg(c, n, 0x14);
    c.eip = p.eip;
    c.budget = (int32_t)m.next_event();
}

}  // namespace towns
