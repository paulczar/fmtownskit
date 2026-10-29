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
#include <chrono>
#include <cstring>
#include <ctime>
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
// PATCH, this repository. Drive A: is a real floppy image, served by the program that links this runtime.
//
// WHY. Ultima II probes A:\ROSTER1.UT2 at start-up with int 21h AX=4300 before it draws anything. With no A:
// drive the probe failed, the program took its "insert the save disk" path, printed message 0x1930 and waited
// for a key, and the port sat there for 24,000 frames looking healthy. The machine mounts the same image, so the
// honest fix is to read it rather than to answer the query with a plausible lie.
//
// The two hooks are defined in port/main.cpp over port/floppy.h, which is a FAT12 reader. Returning false means
// "this runtime has no floppy", and every other drive behaves exactly as before.
bool fm_floppy_attrs(const std::string& path, uint32_t& attrib, uint32_t& size);
bool fm_floppy_read(const std::string& path, std::vector<uint8_t>& out);

static bool on_floppy(const std::string& name) {
    return name.size() >= 2 && name[1] == ':' && std::toupper((unsigned char)name[0]) == 'A';
}

static bool on_disc(Machine& m, const std::string& name, std::string& path) {
    char drive = m.drive;
    path = name;
    if (name.size() >= 2 && name[1] == ':') { drive = (char)std::toupper((unsigned char)name[0]); path = name.substr(2); }
    if (drive == 'Q') {                 // a relative name from the CD's current directory
        if (!m.cd_dir.empty() && (path.empty() || (path[0] != '\\' && path[0] != '/'))) path = m.cd_dir + "\\" + path;
        return true;
    }
    while (!path.empty() && (path[0] == '\\' || path[0] == '/')) path.erase(0, 1);
    for (auto& ch : path) {
        ch = (char)std::tolower((unsigned char)ch);
        if (ch == '\\') ch = '/';
    }
    path = m.save_dir + "/" + std::string(1, (char)std::tolower(drive)) + "/" + path;
    return false;
}

// ---- find first, find next (4Eh, 4Fh) -------------------------------------------------------
// The DTA as the Towns' DOS fills it (read in the oracle's RAM after a find on the CD): [0] the
// drive (the CD's 'Q' | 80h, as a redirector marks it; a host drive its number from 1), [1..11]
// the name searched as 8.3 with '?', [0Ch] the attributes asked; then, here, [0Dh] the next
// entry and [0Fh] the directory (an index into search_dirs: DOS keeps its own state there);
// [15h] the attributes found (a file on the CD 01h, read-only), [16h] the time, [18h] the date
// (the ISO record's), [1Ah] the size, [1Eh] the name, 13 bytes. Not found: CF and AX=12h.
struct DirEntry { std::string name; uint8_t attr; uint16_t time, date; uint32_t size; };

static bool list_dir(Machine& m, const std::string& key, std::vector<DirEntry>& out) {
    out.clear();
    std::string path = key.substr(2);
    if (key[0] == 'Q') {
        std::vector<DiscFile> fs;
        if (!m.disc.list(path, fs)) return false;
        for (const auto& f : fs)
            out.push_back({f.name, (uint8_t)(f.dir ? 0x11 : 0x01), f.time, f.date, f.dir ? 0 : f.size});
        return true;
    }
    namespace fs = std::filesystem;
    std::error_code ec;
    if (!fs::is_directory(path, ec)) return false;
    for (const auto& e : fs::directory_iterator(path, ec)) {
        DirEntry d{e.path().filename().string(), (uint8_t)(e.is_directory() ? 0x10 : 0x20), 0, 0, 0};
        if (!e.is_directory()) d.size = (uint32_t)e.file_size(ec);
        auto t = std::chrono::system_clock::to_time_t(std::chrono::clock_cast<std::chrono::system_clock>(e.last_write_time(ec)));
        if (const std::tm* lt = std::localtime(&t)) {
            d.time = (uint16_t)(lt->tm_hour << 11 | lt->tm_min << 5 | lt->tm_sec / 2);
            d.date = (uint16_t)((lt->tm_year >= 80 ? lt->tm_year - 80 : 0) << 9 | (lt->tm_mon + 1) << 5 | lt->tm_mday);
        }
        for (auto& ch : d.name) ch = (char)std::toupper((unsigned char)ch);
        out.push_back(d);
    }
    std::sort(out.begin(), out.end(), [](const DirEntry& a, const DirEntry& b) { return a.name < b.name; });
    return true;
}

// "KEYBORD.P*" -> "KEYBORD P??"; false if the name has no 8.3 form
static bool name83(const std::string& n, char out[11], bool pattern) {
    std::memset(out, ' ', 11);
    size_t dot = n.find('.');
    std::string base = n.substr(0, dot), ext = dot == std::string::npos ? "" : n.substr(dot + 1);
    if (base.size() > 8 || ext.size() > 3 || ext.find('.') != std::string::npos) return false;
    auto fill = [&](const std::string& s, char* f, int len) {
        for (int k = 0; k < len && k < (int)s.size(); ++k) {
            if (pattern && s[k] == '*') { std::memset(f + k, '?', len - k); return; }
            f[k] = (char)std::toupper((unsigned char)s[k]);
        }
    };
    fill(base, out, 8);
    fill(ext, out + 8, 3);
    return true;
}

static void find_next(Machine& m, X86Cpu& c) {
    uint8_t d[0x2B];
    m.read_block(m.dta, d, sizeof d);
    uint32_t next = d[0x0D] | d[0x0E] << 8, dir = d[0x0F] | d[0x10] << 8;
    std::vector<DirEntry> es;
    if (dir < m.search_dirs.size()) list_dir(m, m.search_dirs[dir], es);
    for (uint32_t k = next; k < es.size(); ++k) {
        const DirEntry& e = es[k];
        char n[11];
        if ((e.attr & 0x16) & ~d[0x0C] || !name83(e.name, n, false)) continue;
        bool match = true;
        for (int i = 0; i < 11 && match; ++i) match = d[1 + i] == '?' || d[1 + i] == (uint8_t)n[i];
        if (!match) continue;
        d[0x0D] = (uint8_t)(k + 1); d[0x0E] = (uint8_t)((k + 1) >> 8);
        d[0x15] = e.attr;
        d[0x16] = (uint8_t)e.time; d[0x17] = (uint8_t)(e.time >> 8);
        d[0x18] = (uint8_t)e.date; d[0x19] = (uint8_t)(e.date >> 8);
        for (int i = 0; i < 4; ++i) d[0x1A + i] = (uint8_t)(e.size >> (8 * i));
        std::memset(d + 0x1E, 0, 13);
        std::memcpy(d + 0x1E, e.name.data(), std::min<size_t>(12, e.name.size()));
        m.write_block(m.dta, d, sizeof d);
        if (m.trace_int) std::fprintf(stderr, "  found %s\n", e.name.c_str());
        s16(c, 0, 0);
        set_cf(c, false);
        return;
    }
    d[0x0D] = (uint8_t)es.size(); d[0x0E] = (uint8_t)(es.size() >> 8);
    m.write_block(m.dta, d, 0x0F);
    s16(c, 0, 0x12);
    set_cf(c, true);
}

static void find_first(Machine& m, X86Cpu& c, const std::string& pattern) {
    size_t cut = pattern.find_last_of("\\/:");
    std::string dirpart = cut == std::string::npos ? "" : pattern.substr(0, cut + 1);
    std::string file = cut == std::string::npos ? pattern : pattern.substr(cut + 1);
    if (dirpart.size() == 2 && dirpart[1] == ':') dirpart += "\\";
    std::string path;
    bool disc = on_disc(m, dirpart.empty() ? "x" : dirpart + "x", path);  // a name in that directory
    path.pop_back();
    char drive = dirpart.size() >= 2 && dirpart[1] == ':' ? (char)std::toupper((unsigned char)dirpart[0]) : m.drive;
    std::string key = std::string(1, disc ? 'Q' : 'H') + "|" + path;
    uint32_t id = 0;
    while (id < m.search_dirs.size() && m.search_dirs[id] != key) ++id;
    if (id == m.search_dirs.size()) m.search_dirs.push_back(key);
    uint8_t d[0x11] = {};
    d[0] = disc ? (uint8_t)(drive | 0x80) : (uint8_t)(drive - 'A' + 1);
    if (!name83(file, (char*)d + 1, true)) { s16(c, 0, 2); set_cf(c, true); return; }
    d[0x0C] = (uint8_t)c.r[1];
    d[0x0F] = (uint8_t)id; d[0x10] = (uint8_t)(id >> 8);
    m.write_block(m.dta, d, sizeof d);
    std::vector<DirEntry> es;
    if (!list_dir(m, key, es)) { s16(c, 0, 3); set_cf(c, true); return; }
    find_next(m, c);
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
    case 0x1A:                                      // the DTA at DS:EDX (registers as they were)
        dta_sel = c.sreg[3]; dta_off = c.r[2]; dta = ds + c.r[2];
        return;
    case 0x2F:                                      // the DTA: ES:EBX
        x86_load_seg(c, 0, dta_sel);
        c.r[3] = dta_off;
        return;
    case 0x4E:                                      // find the first name like DS:EDX, attributes CX
        find_first(*this, c, cstr(*this, ds + c.r[2]));
        if (trace_int && (c.fl & X86_CF)) std::fprintf(stderr, "  none (%x)\n", c.r[0] & 0xFFFF);
        return;
    case 0x4F:                                      // the next one, from the DTA
        find_next(*this, c);
        return;
    case 0x36: {                                    // free space on drive DL (0 the current):
        // AX sectors a cluster, BX free clusters, CX bytes a sector, DX clusters. A host drive
        // as a 2HD floppy of the Towns, 1,232 sectors of 1,024 bytes, less what its directory
        // holds (*inferred*: no floppy on the oracle); the CD none free (*inferred*)
        char d = (c.r[2] & 0xFF) ? (char)('A' + (c.r[2] & 0xFF) - 1) : drive;
        if (d == 'Q') {
            uint32_t n = 0;
            for (const auto& t : disc.tracks()) if (!t.audio) n += t.sectors;
            s16(c, 0, 1); s16(c, 3, 0); s16(c, 1, 2048); s16(c, 2, n);
            return;
        }
        std::string path;
        on_disc(*this, std::string(1, d) + ":x", path);
        path.pop_back();
        uint32_t used = 0;
        std::error_code ec;
        if (std::filesystem::is_directory(path, ec))
            for (const auto& e : std::filesystem::directory_iterator(path, ec))
                if (!e.is_directory()) used += (uint32_t)((e.file_size(ec) + 1023) / 1024);
        s16(c, 0, 1); s16(c, 3, used < 1232 ? 1232 - used : 0); s16(c, 1, 1024); s16(c, 2, 1232);
        return;
    }
    case 0x2A: case 0x2C: {                         // the date (AL weekday, CX year, DH month, DL day),
        // the time (CH hour, CL minutes, DH seconds, DL hundredths): the host's, as MAME's RTC,
        // or clock_start's plus the machine's time
        auto host = std::chrono::system_clock::now();
        if (clock_start)
            host = std::chrono::system_clock::time_point(std::chrono::seconds(clock_start))
                   + std::chrono::milliseconds(now / (kClock / 1000));
        std::time_t t = std::chrono::system_clock::to_time_t(host);
        std::tm lt{};
        if (const std::tm* p = std::localtime(&t)) lt = *p;
        if (ah == 0x2A) {
            s8(c, 0, (uint32_t)lt.tm_wday);
            s16(c, 1, (uint32_t)(lt.tm_year + 1900));
            s16(c, 2, (uint32_t)((lt.tm_mon + 1) << 8 | lt.tm_mday));
        } else {
            auto cs = std::chrono::duration_cast<std::chrono::milliseconds>(host.time_since_epoch()).count() / 10 % 100;
            s16(c, 1, (uint32_t)(lt.tm_hour << 8 | lt.tm_min));
            s16(c, 2, (uint32_t)(lt.tm_sec << 8 | (int)cs));
        }
        return;
    }
    case 0x30:                                      // the version: DOS 3.10, and RUN386's "DX" above
        c.r[0] = 0x44580A03u;                       // (the oracle: EAX 44580A03h, EBX FF00h)
        c.r[3] = 0xFF00;
        s16(c, 1, 0);
        return;
    case 0x25: {                                    // Phar Lap
        switch (al) {
        case 0x02:                                  // get the protected-mode vector CL: ES:EBX
            x86_load_seg(c, 0, pm_vector[c.r[1] & 0xFF].sel);
            c.r[3] = pm_vector[c.r[1] & 0xFF].off;
            set_cf(c, false);
            return;
        case 0x03:                                  // get the real-mode vector CL: EBX (seg:off)
            c.r[3] = rm_vector[c.r[1] & 0xFF];
            set_cf(c, false);
            return;
        case 0x04: case 0x06: case 0x07:           // set the protected-mode vector CL to DS:EDX
            // (06h: always taken in protected mode; 07h: and the real-mode one to EBX). Every
            // interrupt here is taken in protected mode; a vector set back to what 02h gave
            // before any was set (selector 0) is none.
            pm_vector[c.r[1] & 0xFF] = {c.sreg[3], c.r[2], c.sreg[3] != 0};
            if (al == 0x07) rm_vector[c.r[1] & 0xFF] = c.r[3];
            set_cf(c, false);
            return;
        case 0x05:                                  // set the real-mode vector CL to EBX
            rm_vector[c.r[1] & 0xFF] = c.r[3];
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
            const uint32_t pb = profile->prog_base;
            uint32_t phys = lin >= pb && lin < pb + 0x100000 ? lin - pb + profile->prog_phys : lin;
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
            (void)res;
            int93(c, eax, ret, rds * 16 + (c.r[7] & 0xFFFF));
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
        if (on_floppy(name)) {                      // PATCH, this repository: drive A: is the floppy image
            int fh = new_handle(*this);
            if (fh < 0) { set_cf(c, true); s16(c, 0, 4); return; }
            Handle& hf = handles[fh];
            if (ah == 0x3C || !fm_floppy_read(name, hf.data)) {
                if (trace_int) std::fprintf(stderr, "  %s: not on the floppy\n", name.c_str());
                set_cf(c, true); s16(c, 0, ah == 0x3C ? 5 : 2); return;
            }
            hf.host = false; hf.used = true; hf.pos = 0;
            if (trace_int) std::fprintf(stderr, "  %s -> handle %d (floppy, %u bytes)\n", name.c_str(), fh,
                                        (unsigned)hf.data.size());
            s16(c, 0, (uint32_t)fh);
            set_cf(c, false);
            return;
        }
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
        if (ah == 0x40 && (h == 1 || h == 2)) {     // the console (ABEL writes to it as it starts):
            std::string s;                          // shown here, every byte taken
            for (uint32_t k = 0; k < c.r[1]; ++k) {
                uint8_t ch = (uint8_t)mem_read(ds + c.r[2] + k, 1);
                if (ch >= 0x20 || ch == '\n') s += (char)ch;
                else { char e[8]; std::snprintf(e, sizeof e, "<%02x>", ch); s += e; }
            }
            std::fprintf(stderr, "dos: %s\n", s.c_str());
            c.r[0] = c.r[1];
            set_cf(c, false);
            return;
        }
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
    case 0x44: {                                    // IOCTL 00h, a handle's device information:
        uint32_t h = c.r[3] & 0xFFFF;               // a file (bit 7 clear), not written (bit 6), on
        if (al == 0x00 && h <= 2) {                 // the CD's drive: 50h. The standard handles: the
            s16(c, 2, 0x80D3);                      // console, 80D3h (ABEL's stdout, on the oracle)
            s16(c, 0, 0x80D3);
            set_cf(c, false);
            return;
        }
        if (al != 0x00 || h >= handles.size() || !handles[h].used) break;
        uint32_t info = 0x40 | (uint32_t)((handles[h].host ? 'A' : drive) - 'A');
        s16(c, 2, info);
        s16(c, 0, info);
        set_cf(c, false);
        return;
    }
    case 0x43: {                                    // get file attributes into CX, from DS:EDX. The
        if (al != 0x00) return;                     // set form (AL=1) changes nothing on a CD, which
        DiscFile f;                                 // is read only anyway
        std::string want = cstr(*this, ds + c.r[2]);
        if (on_floppy(want)) {                      // PATCH, this repository: drive A: is the floppy image
            uint32_t fattrib = 0, fsize = 0;
            if (!fm_floppy_attrs(want, fattrib, fsize)) {
                if (trace_int) std::fprintf(stderr, "  attributes: %s -> not on the floppy\n", want.c_str());
                c.r[0] = (c.r[0] & 0xFFFF0000u) | 2u;
                set_cf(c, true);
                return;
            }
            if (trace_int) std::fprintf(stderr, "  attributes: %s -> floppy, attr %02x, %u bytes\n",
                                        want.c_str(), fattrib, fsize);
            c.r[1] = fattrib;
            c.r[0] &= 0xFFFF0000u;
            set_cf(c, false);
            return;
        }
        if (!disc.find(want, f)) {
            // This is the check that decides whether the game shows its "press a key" error path, so a miss
            // here has to be visible rather than silent.
            if (trace_int) std::fprintf(stderr, "  attributes: %s -> not found\n", want.c_str());
            c.r[0] = (c.r[0] & 0xFFFF0000u) | 2u;   // file not found, DOS error 2
            set_cf(c, true);
            return;
        }
        if (trace_int) std::fprintf(stderr, "  attributes: %s -> found%s\n", want.c_str(), f.dir ? " (dir)" : "");
        c.r[1] = f.dir ? 0x10u : 0x20u;             // a directory, or an ordinary archive file
        c.r[0] &= 0xFFFF0000u;
        set_cf(c, false);
        return;
    }
    case 0xFF:                                      // not DOS's (an extender's test, DX=78h):
        unknown("int 21h AX=%04X DX=%04X: none (as on the Towns)", c.r[0] & 0xFFFF, c.r[2] & 0xFFFF);
        return;                                     // RUN386 leaves every register as it was
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

void Machine::int93(X86Cpu& c, uint32_t eax, uint32_t& ret, uint32_t buf) {
    uint8_t ah = (eax >> 8) & 0xFF, al = eax & 0xFF;
    if (trace_int) std::fprintf(stderr, "int 93h AX=%04X CX=%04X BX=%04X buffer %05x\n", eax & 0xFFFF, c.r[1] & 0xFFFF,
                                c.r[3] & 0xFFFF, buf);
    if (al != 0xC0) {
        // PATCH, this repository. AL is not always the CD-DA interface. The program's CD probe calls
        // int86(0x93) at guest 0xe2445 with AX=0x0220, then reads the returned AH from its outregs at
        // DS:0x7a635 and branches: AH==2 returns -1, otherwise two bits at DS:0x7a640 are consulted and
        // a second call goes out with AH=0x07. That is read off the code at guest 0xe2440-0xe24d7.
        //
        // The returns are MEASURED, not chosen. Breakpoint at 000C:0E244D, the instruction after the
        // first call, with the outregs struct the program passes read at DS:0x7a634:
        //
        //   in AX=0x0220   out EAX=0x00000020  EBX=0  ECX=0  EDX=4
        //   in AX=0x0720   out EAX=0x00000000  EBX=1  ECX=1  EDX=0
        //
        // The second row was read from the same struct after start-up had finished, since the second
        // call is the last one to write it. Both are one sample from one machine.
        if (al == 0x20) {
            if (ah == 0x02) {
                ret = 0x20u;
                c.r[3] = 0;
                c.r[1] = 0;
                c.r[2] = 4;
            } else {
                ret = 0;
                c.r[3] = 1;
                c.r[1] = 1;
                c.r[2] = 0;
            }
            set_cf(c, false);
            return;
        }
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
        cd_paused = false;
        cd.repeat = c.r[1] & 0xFF;
        if (trace_int) std::fprintf(stderr, "  CD-DA %u to %u (frame %llu)\n", cd.from, cd.to, (unsigned long long)frame);
        return;
    }
    case 0x53: {                                    // where the play is: AL and the buffer
        // As FORRBIOS.NSD answered on the oracle (tools/oracle.py --trace-calls at the int and
        // after it): AL 1 while a track plays, 0 paused or stopped; the buffer [1] the track,
        // [3..5] the position from the play's first sector, [7..9] the disc's position (the
        // sector as MSF, binary, with no lead-in: as MAME's CD answers, "incorrect" by its own
        // note) while it plays or is paused, all zero once a track has ended or none played.
        uint8_t p[10] = {};
        uint32_t s = cd_sector();               // (a track that has ended stops here)
        if (cd.playing || cd_paused) {
            auto msf = [](uint32_t f, uint8_t* q) { q[0] = (uint8_t)(f / 4500); q[1] = (uint8_t)(f / 75 % 60); q[2] = (uint8_t)(f % 75); };
            for (const auto& t : disc.tracks())
                if (s >= t.lba()) p[1] = (uint8_t)t.number;
            msf(s - cd.from, p + 3);
            msf(s, p + 7);
        }
        write_block(buf, p, sizeof p);
        ret = (ret & 0xFFFFFF00u) | (cd.playing ? 1 : 0);
        return;
    }
    case 0x55:                                      // pause: AH=22h when paused already
        if (cd_paused) { ret = (ret & 0xFFFF00FFu) | 0x2200; return; }
        cd_paused = cd.playing;
        if (cd.playing) cd_stop();
        return;
    case 0x56:                                      // go on after a pause: AH=23h with none
        if (!cd_paused) { ret = (ret & 0xFFFF00FFu) | 0x2300; return; }
        cd_paused = false;
        cd_play(cd.lba, cd.to);
        return;
    case 0x52:                                      // stop
        cd_paused = false;
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

// The keyboard, as a port drives it. One queue, served in order, so a sequence of presses reads back in the order
// the program asked for them. Nothing else consumes it: this is not a keyboard driver, it is the minimum that lets
// a program waiting for a key proceed, and the measured contract is in int90's case 0x09.
void Machine::press_key(uint8_t scan, uint8_t ascii) { keys.push_back(Key{scan, ascii}); }

// The keyboard BIOS, int 90h, as FORRBIOS.NSD answers it in protected mode (the program
// pushes "DOSX" "BIOS" before each call): read on the oracle (tools/oracle.py --trace-calls at
// each int and the instruction after it) with no key ever pressed -- AH 0 for every function,
// KYB_read (AH=09h) DX FFFFh and BX FF00h, KYB_matrix (AH=0Ah) its 16 bytes at DS:EDI none down
// (*inferred*: zeros).
void Machine::int90(X86Cpu& c) {
    uint8_t ah = (c.r[0] >> 8) & 0xFF;
    if (trace_int) std::fprintf(stderr, "int 90h AX=%04X EBX=%08X ECX=%08X EDX=%08X\n", c.r[0] & 0xFFFF, c.r[3], c.r[1], c.r[2]);
    switch (ah) {
    // PATCH, this repository. KYB_lock (AH=04) was not in this kit's recordings, because they were taken
    // with no key ever pressed. Measured on the live machine with the emulator console at 000C:0052:
    // EAX goes in as 0x00000400 (AH the function, AL the lock flag from [EBP+8]) and comes back as
    // 0x00000000. The stub there is `CD 90`, `MOVZX EAX,AH`, `LEAVE`, `RET`, so the result is in AH, and
    // the `c.r[0] &= 0xFFFF00FF` at the end of this function already delivers it by zeroing AH. One call
    // measured, with AL=0. The handler is TBIOS's own, reached through CS=0008 at linear 0x5C12E.
    case 0x04:
    case 0x00: case 0x01: case 0x02: case 0x05: case 0x06:   // init, buffer, code, click, clear
        break;
    case 0x09:                                      // read a key
        // PATCH, this repository. The kit had no keyboard, so this answered "none" for ever and a program
        // polling for input never moved. Measured on the live machine at 000C:000000B6, the instruction after
        // the `int 90h` in the KYB_read wrapper, with the key 'a' pressed at a break:
        //     BX = 0x1E00   DX = 0x0061     (the scan code in BX's high byte, the ASCII in DX)
        // and with no key, BX = 0xFF00 and DX = 0xFFFF, which is what the recordings showed. ENCHANT tests
        // `(BX & 0xFF00) == 0xFF00` for "none", then indexes a 114-entry table with `(BX >> 8) - 1`, so it reads
        // the SCAN CODE and DX is a convenience.
        if (!keys.empty()) {
            Key k = keys.front();
            keys.erase(keys.begin());
            ++keys_served;
            s16(c, 3, (uint32_t)k.scan << 8);
            s16(c, 2, k.ascii);
        } else {
            s16(c, 2, 0xFFFF);
            s16(c, 3, 0xFF00);
        }
        break;
    case 0x0A: {                                    // the key matrix
        static const uint8_t none[16] = {};
        write_block(c.sbase[3] + c.r[7], none, sizeof none);
        break;
    }
    default: {
        char why[64];
        std::snprintf(why, sizeof why, "int 90h AX=%04Xh: no service", c.r[0] & 0xFFFF);
        x86_fault(c, c.eip, why);
    }
    }
    c.r[0] &= 0xFFFF00FFu;
}

// KimG, int 8Eh AX=CF00h (its client: set, get, pop), as FORRBIOS.NSD answers it: EAX 0, the
// rest untouched (the oracle).
void Machine::int8e(X86Cpu& c) {
    if ((c.r[0] & 0xFFFF) != 0xCF00) {
        char why[64];
        std::snprintf(why, sizeof why, "int 8Eh AX=%04Xh: no service", c.r[0] & 0xFFFF);
        x86_fault(c, c.eip, why);
    }
    c.r[0] = 0;
}

void Machine::intfd(X86Cpu& c) {
    unknown("int FDh AX=%04X CX=%04X (nothing done)", c.r[0] & 0xFFFF, c.r[1] & 0xFFFF);
}

// ---- RUN386 ------------------------------------------------------------------------------------
bool parse_p3(const std::vector<uint8_t>& file, P3& p, std::string& err) {
    // a P3 bound behind an MZ stub (Fujitsu's "exp2exe", ABEL's programs): it starts where the
    // MZ header's page count ends, and every offset counts from there (fmtownskit.exp)
    size_t stub = 0;
    if (file.size() >= 0x1C && file[0] == 'M' && file[1] == 'Z') {
        size_t last = file[2] | file[3] << 8, pages = file[4] | file[5] << 8;
        stub = pages * 512 - (last ? 512 - last : 0);
        if (stub + 2 > file.size() || file[stub] != 'P' || file[stub + 1] != '3') {
            err = "an MZ file with no P3 where its header ends";
            return false;
        }
    }
    const std::vector<uint8_t> f(file.begin() + (long)stub, file.end());
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

// ---- the timer BIOS, int 97h ---------------------------------------------------------------
// Read on the oracle (ABEL's OPNG, tools/oracle.py --trace-calls at its int 97h and after):
// AH=00h starts a timer from the packet at DS:EDI -- [0] the mode (bit 0: once; OPNG's are 2,
// repeating), [2] the callback's offset and [6] its selector, [8] and [10] the limit as two
// words, the high one first -- and answers its number in AL; AH=01h stops timer AL; AH=02h
// writes timer AL's count of 10 ms ticks to [8] and [10] of the buffer at DS:EDI, the same way.
// AH=0 for success. OPNG waits by polling 02h (its count reaches 5 every 50 ms on the oracle)
// and sets limits of 400 and 2000 ticks for its callbacks: the one at 400 ran 4.00 s later.
void Machine::int97(X86Cpu& c) {
    uint8_t ah = (c.r[0] >> 8) & 0xFF, al = c.r[0] & 0xFF;
    uint32_t p = c.sbase[3] + c.r[7];
    auto setah = [&](uint8_t v) { c.r[0] = (c.r[0] & ~0xFF00u) | (uint32_t)v << 8; };
    if (trace_int) std::fprintf(stderr, "int 97h AX=%04X EDI=%08X\n", c.r[0] & 0xFFFF, c.r[7]);
    switch (ah) {
    case 0x00: {
        int n = 0;
        while (n < 8 && t97[n].on) ++n;
        if (n == 8) { unknown("int 97h: no timer free"); setah(1); return; }
        Timer97& t = t97[n];
        t = Timer97{};
        t.on = true;
        t.mode = (uint8_t)mem_read(p, 1);
        t.off = mem_read(p + 2, 4);
        t.sel = (uint16_t)mem_read(p + 6, 2);
        t.limit = mem_read(p + 8, 2) << 16 | mem_read(p + 10, 2);
        if (!next_t97) next_t97 = now + kClock / 100;
        c.r[0] = (c.r[0] & ~0xFFFFu) | (uint32_t)n;
        return;
    }
    case 0x01:
        if (al < 8) t97[al].on = false;
        setah(0);
        return;
    case 0x02:
        if (al < 8) {
            mem_write(p + 8, 2, t97[al].count >> 16);
            mem_write(p + 10, 2, t97[al].count & 0xFFFF);
        }
        setah(0);
        return;
    default: {
        char why[64];
        std::snprintf(why, sizeof why, "int 97h AX=%04Xh: no service", c.r[0] & 0xFFFF);
        x86_fault(c, c.eip, why);
    }
    }
}

int Machine::t97_due() {
    for (int n = 0; n < 8; ++n) {
        Timer97& t = t97[n];
        if (!t.on || !t.due) continue;
        t.due = false;
        if (t.mode & 1) t.on = false;
        else t.count = 0;
        return n;
    }
    return -1;
}

// A TBIOS in modules (ABEL's disc: CONFIG.SYS loads TBIOSLD.SYS, which reads TBIOS.INI), laid
// out as TBIOSLD does (fmtownskit.tbiosld, where the rules are written down and checked against
// the oracle's memory): the first file at 0 as it is, each Phar Lap "MQ" image after it at the
// next 16-byte boundary, relocated there; each module's records linked into the entry table
// (an export sets the slot to its entry and keeps the old pointer, an import takes the slot's
// pointer); the modules' data pointers at 0x200.
bool compose_tbios(Disc& disc, const std::string& ini, std::vector<uint8_t>& img, std::string& err) {
    std::vector<uint8_t> text;
    if (!disc.file(ini, text)) { err = ini + " not on the disc"; return false; }
    std::vector<std::pair<std::string, std::string>> mods;   // key, file
    std::string line;
    text.push_back('\n');
    for (uint8_t ch : text) {
        if (ch != '\n' && ch != '\r') { line += (char)ch; continue; }
        size_t eq = line.find('=');
        if (eq != std::string::npos && eq > 0 &&
            std::all_of(line.begin(), line.begin() + (long)eq, [](char k) { return std::isalpha((uint8_t)k); }))
            mods.push_back({line.substr(0, eq), line.substr(eq + 1)});
        line.clear();
    }
    if (mods.empty() || mods[0].first != "BIN") { err = ini + " does not start with BIN="; return false; }
    if (!disc.file(mods[0].second, img)) { err = mods[0].second + " not on the disc"; return false; }
    auto u16 = [](const uint8_t* p) { return (uint32_t)(p[0] | p[1] << 8); };
    auto u32 = [](const uint8_t* p) { uint32_t v; std::memcpy(&v, p, 4); return v; };
    auto put32 = [](uint8_t* p, uint32_t v) { std::memcpy(p, &v, 4); };
    std::vector<std::pair<uint32_t, uint16_t>> datas;
    for (size_t k = 1; k < mods.size(); ++k) {
        const std::string& name = mods[k].second;
        std::vector<uint8_t> f;
        if (!disc.file(name, f)) { err = name + " not on the disc"; return false; }
        if (f.size() < 0x1C || f[0] != 'M' || f[1] != 'Q') { err = name + ": not an MQ image"; return false; }
        uint32_t last = u16(&f[2]), pages = u16(&f[4]), nrel = u16(&f[6]), hpar = u16(&f[8]), roff = u16(&f[0x18]);
        if (pages * 512 - (last ? 512 - last : 0) != f.size() || roff + 4 * nrel > f.size() || hpar * 16 > f.size()) {
            err = name + ": its MQ header does not match the file";
            return false;
        }
        uint32_t base = ((uint32_t)img.size() + 15) & ~15u;
        std::vector<uint8_t> m(f.begin() + hpar * 16, f.end());
        for (uint32_t r = 0; r < nrel; ++r) {
            uint32_t rel = u32(&f[roff + 4 * r]), o = rel & 0x7FFFFFFF;
            if (o + 4 > m.size()) { err = name + ": a relocation past the image"; return false; }
            if (rel >> 31) put32(&m[o], u32(&m[o]) + base);
            else { uint32_t v = u16(&m[o]) + base; m[o] = (uint8_t)v; m[o + 1] = (uint8_t)(v >> 8); }
        }
        if (m.size() < 0x30 || std::memcmp(&m[0x10], "Townes", 6)) { err = name + ": no module header"; return false; }
        uint32_t data = u32(&m[0x24]), csel = u16(&m[0x28]), dsel = u16(&m[0x2A]), n = u16(&m[0x2E]);
        if (0x30 + 8 * n > m.size()) { err = name + ": its records run past the image"; return false; }
        for (uint32_t r = 0; r < n; ++r) {
            uint8_t* p = &m[0x30 + 8 * r];
            uint32_t slot = u32(p), s = slot & 0x7FFFFFFF, off = u32(p + 4);
            if (s + 6 > img.size()) { err = name + ": a record names a slot past the table"; return false; }
            std::memcpy(p, &img[s], 6);
            p[6] = p[7] = 0;
            if (!(slot >> 31)) {
                put32(&img[s], off);
                img[s + 4] = (uint8_t)csel; img[s + 5] = (uint8_t)(csel >> 8);
            }
        }
        if (data) datas.push_back({data, (uint16_t)dsel});
        img.resize(base);
        img.insert(img.end(), m.begin(), m.end());
    }
    for (size_t k = 0; k < datas.size(); ++k) {
        uint8_t* p = &img[0x200 + 8 * k];
        put32(p, datas[k].first);
        p[4] = (uint8_t)datas[k].second; p[5] = (uint8_t)(datas[k].second >> 8); p[6] = p[7] = 0;
    }
    return true;
}

void boot_run386(Machine& m, X86Cpu& c, const P3& p, const std::vector<uint8_t>& tbios) {
    boot_towns(m, tbios);
    run386_load(m, c, p);
}

void boot_towns(Machine& m, const std::vector<uint8_t>& tbios) {
    m.map_ram(0, 0xC0000);                              // the first 768 KB (Phar Lap's buffer is there)
    m.map_ram(0x100000, (uint32_t)tbios.size());
    m.write_block(0x100000, tbios.data(), (uint32_t)tbios.size());
    m.tbios_size = (uint32_t)tbios.size();
}

// What a program's end undoes before the next one runs (DOS, RUN386): its files closed, the
// protected-mode vectors it set back to none, its memory blocks given back. TBIOS stays, with
// what the program left in its work areas (ABEL's start: OPNG's there, on the oracle), and so
// do the machine's devices.
void run386_end(Machine& m) {
    for (size_t h = 5; h < m.handles.size(); ++h) {
        if (m.handles[h].f) std::fclose(m.handles[h].f);
        m.handles[h] = Machine::Handle{};
    }
    for (auto& v : m.pm_vector) v = IntVector{};
    for (auto& t : m.t97)
        if (t.on) { unknown("a timer BIOS timer left on at a program's end"); t.on = false; }
    m.search_dirs.clear();
    m.exited = false;
}

void run386_load(Machine& m, X86Cpu& c, const P3& p) {
    // the program's segment: its image, and the stack up to the page its top lies in (*inferred*:
    // 0x14000 for an image of 0x11F17 and a stack top of 0x13F20); a program that asks for all the
    // memory there is (max extra 0xFFFFFFFF) gets what the machine's RUN386 gives (the profile)
    const Profile& pr = *m.profile;
    uint32_t base = pr.prog_base;
    uint32_t end = std::max(p.base_offset + (uint32_t)p.image.size(), p.esp);
    if (p.max_extra == 0xFFFFFFFFu && !pr.prog_limit_all) {
        std::fprintf(stderr, "towns: the program asks for all memory; profile %s does not say how much\n", pr.name);
        throw Stop{2};
    }
    uint32_t size = p.max_extra == 0xFFFFFFFFu ? pr.prog_limit_all + 1 : (end + 0xFFF) & ~0xFFFu;
    m.map_ram(base, size);
    for (uint32_t a = base; a < base + size; a += 0x1000)   // a program after another: its memory fresh
        if (uint8_t* h = m.host(a)) std::memset(h, 0, 0x1000);
    m.write_block(base + p.base_offset, p.image.data(), (uint32_t)p.image.size());
    // the descriptors as the Towns has them (towns.SELECTORS)
    const uint32_t tb = m.tbios_size - 1;
    m.set_desc(0x0C, base, size - 1);  m.set_desc(0x14, base, size - 1);
    m.set_desc(0x04, 0xB7870, 0xFF);   m.set_desc(0x24, 0xB7870, 0xFF);   m.set_desc(0x2C, 0xB7820, 0x3E);
    m.set_desc(0x1C, 0x80000000u, 0x7FFFF); m.set_desc(0x34, 0, 0xFFFFF);
    m.set_desc(0x104, 0x80000000u, 0x7FFFF); m.set_desc(0x10C, 0x80100000u, 0x7FFFF); m.set_desc(0x114, 0x81000000u, 0x1FFFF);
    m.set_desc(0x40, 0, pr.ram_limit); m.set_desc(0x48, 0x80000000u, 0x7FFFF); m.set_desc(0x60, 0, 0xFFFFF);
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
    // the CPU's count goes on from the last program's: the machine's time does
    c.clocks = m.cpu_time(m.now);
    c.budget = (int32_t)std::min<uint64_t>(m.cpu_time(m.next_event()) - c.clocks, 1u << 30);
}

}  // namespace towns
