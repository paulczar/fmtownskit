// fmtownskit runtime -- the disc: a cue sheet with its track files, as fmtownskit.disc reads it.
//
// Sectors are counted from the first track's INDEX 01, as one run over every file (the audio
// in a track's pregap is on the disc: a program plays by position). The data track is ISO
// 9660 with 2,048-byte sectors, raw (2,352: the data at +16) or cooked.
#include "towns.h"
#include <algorithm>
#include <cctype>
#include <cstring>

namespace towns {

static uint32_t msf(const char* t) {
    int m = 0, s = 0, f = 0;
    std::sscanf(t, "%d:%d:%d", &m, &s, &f);
    return (uint32_t)((m * 60 + s) * 75 + f);
}

static long file_size(const std::string& p) {
    FILE* f = std::fopen(p.c_str(), "rb");
    if (!f) return -1;
    std::fseek(f, 0, SEEK_END);
    long n = std::ftell(f);
    std::fclose(f);
    return n;
}

FILE* Disc::handle(const std::string& path) {
    // PATCH, this repository. An empty path means a track with no file was reached, which happens when nothing
    // was opened. fopen would take the empty string happily, so this is not the crash itself, but it turns a
    // silent nonsense open into a refusal.
    if (path.empty()) return nullptr;
    for (auto& h : handles_) if (h.first == path) return h.second;
    FILE* f = std::fopen(path.c_str(), "rb");
    if (f) handles_.push_back({path, f});
    return f;
}

bool Disc::open(const std::string& cue, std::string& err) {
    FILE* f = std::fopen(cue.c_str(), "r");
    if (!f) { err = cue + ": cannot open"; return false; }
    std::string base = cue;
    size_t slash = base.find_last_of("/\\");
    base = slash == std::string::npos ? "" : base.substr(0, slash + 1);
    char line[1024];
    int line_no = 0;
    std::string cur;
    while (std::fgets(line, sizeof line, f)) {
        ++line_no;
        char* p = line;
        while (*p == ' ' || *p == '\t') ++p;
        if (!std::strncmp(p, "FILE", 4)) {
            char* q = std::strchr(p, '"');
            if (q) {
                char* e = std::strchr(q + 1, '"');
                cur = base + std::string(q + 1, e ? e : q + 1);
            } else {
                char name[512];
                std::sscanf(p + 4, "%511s", name);
                cur = base + name;
            }
        } else if (!std::strncmp(p, "TRACK", 5)) {
            Track t;
            char mode[32] = {};
            std::sscanf(p + 5, "%d %31s", &t.number, mode);
            if (!std::strcmp(mode, "AUDIO")) t.audio = true;
            else if (!std::strcmp(mode, "MODE1/2048")) t.sector_size = 2048;
            else if (std::strcmp(mode, "MODE1/2352")) { err = std::string("track mode ") + mode + " not read here"; std::fclose(f); return false; }
            t.path = cur;
            tracks_.push_back(t);
        } else if (!std::strncmp(p, "INDEX", 5) && !tracks_.empty()) {
            int n = 0; char t[32] = {};
            std::sscanf(p + 5, "%d %31s", &n, t);
            if (n == 0) tracks_.back().index0 = (int32_t)msf(t);
            if (n == 1) tracks_.back().index1 = (int32_t)msf(t);
        } else if (!std::strncmp(p, "PREGAP", 6) || !std::strncmp(p, "POSTGAP", 7)) {
            // PATCH, this repository. The old message was "a gap not in the files is not read here": correct and
            // unhelpful, because it read like a missing feature. The problem is that the cue does not say enough
            // to decide, and guessing has a specific cost, so the message now names the line, the track, the gap
            // and the ambiguity. Seen on this disc's own cue: one FILE, eight tracks, `PREGAP 00:02:00` on track
            // 2, and every INDEX 01 still relative to that one file. Whether the rip already contains those 150
            // sectors is a property of the rip, not of the cue, and getting it wrong shifts every later track by
            // 150 sectors without an error. A CloneCD .ccd states it, and a one-track cue for the data track does
            // not care, which are the two answers to give a caller.
            bool is_pre = p[1] == 'R';
            char gap[32] = {};
            std::sscanf(p + (is_pre ? 6 : 7), "%31s", gap);
            char why[512];
            std::snprintf(why, sizeof why,
                          "%s:%d: %s %s on track %d cannot be decided from the cue. The gap is not in the file that "
                          "the track's INDEX 01 counts from, but whether the rip already contains it is a property "
                          "of the rip and the cue does not say, so accepting it would shift every later track by "
                          "%s and read the wrong sectors silently. Use the .ccd, which states the layout, or a "
                          "one-track cue for the data track if only that is wanted.",
                          cue.c_str(), line_no, is_pre ? "PREGAP" : "POSTGAP", gap,
                          tracks_.empty() ? 0 : tracks_.back().number, gap);
            err = why; std::fclose(f); return false;
        }
    }
    std::fclose(f);
    if (tracks_.empty()) { err = cue + ": no tracks"; return false; }
    // place each file on the disc, and each track's length
    std::string last;
    uint32_t start = 0, fstart = 0;
    std::vector<std::pair<std::string, uint32_t>> ends;
    for (auto& t : tracks_) {
        if (t.path != last) {
            long n = file_size(t.path);
            if (n < 0) { err = t.path + ": cannot open"; return false; }
            fstart = start;
            start += (uint32_t)(n / t.sector_size);
            ends.push_back({t.path, start});
            last = t.path;
        }
        t.file_start = fstart;
    }
    for (size_t i = 0; i < tracks_.size(); ++i) {
        Track& t = tracks_[i];
        uint32_t end = 0;
        if (i + 1 < tracks_.size() && tracks_[i + 1].path == t.path) {
            const Track& n = tracks_[i + 1];
            end = n.file_start + (uint32_t)(n.index0 >= 0 ? n.index0 : n.index1);
        } else {
            for (auto& e : ends) if (e.first == t.path) end = e.second;
        }
        t.sectors = end - t.lba();
    }
    for (auto& t : tracks_) if (!t.audio) { data_ = &t; break; }
    if (!data_) { err = "no data track"; return false; }
    return true;
}

bool Disc::read_data(uint32_t n, uint8_t* out) {
    // PATCH, this repository. `data_` is null until `open()` finds a data track, and this dereferenced it
    // unconditionally, so a program that opened a file without a disc mounted took a null pointer instead of
    // getting a DOS error. That is what killed the port's first 600-frame run inside Disc::handle, with a
    // backtrace pointing at fopen because the null Track's path was garbage.
    if (!data_) return false;
    const Track& t = *data_;
    FILE* f = handle(t.path);
    if (!f || n >= t.sectors) return false;
    long pos = (long)(t.index1 + n) * t.sector_size + (t.sector_size == 2352 ? 16 : 0);
    if (std::fseek(f, pos, SEEK_SET)) return false;
    return std::fread(out, 1, 2048, f) == 2048;
}

bool Disc::read_bytes(uint32_t lba, uint32_t off, uint32_t n, uint8_t* out) {
    uint8_t sec[2048];
    while (n) {
        uint32_t s = lba + off / 2048, o = off % 2048, k = std::min<uint32_t>(n, 2048 - o);
        if (!read_data(s, sec)) return false;
        std::memcpy(out, sec + o, k);
        out += k; off += k; n -= k;
    }
    return true;
}

bool Disc::read_audio(uint32_t sector, uint8_t* out) {
    for (const auto& t : tracks_) {
        if (!t.audio) continue;
        // the track's file holds its pregap and its sectors: the file's own sectors
        uint32_t first = t.file_start, last = t.lba() + t.sectors;
        if (sector < first || sector >= last) continue;
        FILE* f = handle(t.path);
        if (!f || std::fseek(f, (long)(sector - first) * 2352, SEEK_SET)) return false;
        return std::fread(out, 1, 2352, f) == 2352;
    }
    return false;
}

void Disc::walk(uint32_t lba, uint32_t size, const std::string& prefix, int depth) {
    if (depth > 8) return;
    std::vector<uint8_t> d(size);
    if (!read_bytes(lba, 0, size, d.data())) return;
    uint32_t p = 0;
    while (p < size) {
        uint8_t n = d[p];
        if (n == 0) { p = (p / 2048 + 1) * 2048; continue; }
        if (p + n > size || n < 34) break;
        const uint8_t* r = &d[p];
        p += n;
        uint8_t nl = r[32];
        if (nl == 1 && (r[33] == 0 || r[33] == 1)) continue;
        std::string name((const char*)r + 33, nl);
        size_t semi = name.find(';');
        if (semi != std::string::npos) name.resize(semi);
        if (!name.empty() && name.back() == '.') name.pop_back();
        uint32_t elba, esz;
        std::memcpy(&elba, r + 2, 4);
        std::memcpy(&esz, r + 10, 4);
        bool dir = r[25] & 2;
        // the record's date and time (years since 1900), as DOS packs them
        uint16_t time = (uint16_t)(r[21] << 11 | r[22] << 5 | r[23] / 2);
        uint16_t date = (uint16_t)((r[18] >= 80 ? r[18] - 80 : 0) << 9 | r[19] << 5 | r[20]);
        files_.push_back({prefix + name, elba, esz, dir, time, date});
        if (dir) walk(elba, esz, prefix + name + "\\", depth + 1);
    }
}

static std::string upper(std::string s) {
    for (auto& ch : s) ch = (char)std::toupper((unsigned char)ch);
    return s;
}

void Disc::index() {
    if (!walked_) {
        walked_ = true;
        // PATCH, this repository: with no disc opened there is nothing to walk, and find() should answer
        // "no such file" so the DOS layer can return an error rather than crash. See read_data.
        if (!data_) return;
        uint8_t pvd[2048];
        for (uint32_t n = 16; n < 32 && read_data(n, pvd); ++n) {
            if (std::memcmp(pvd + 1, "CD001", 5)) break;
            if (pvd[0] == 1) {
                uint32_t lba, size;
                std::memcpy(&lba, pvd + 156 + 2, 4);
                std::memcpy(&size, pvd + 156 + 10, 4);
                walk(lba, size, "", 0);
                break;
            }
            if (pvd[0] == 255) break;
        }
    }
}

static std::string disc_path(const std::string& path) {
    std::string want = upper(path);
    for (auto& ch : want) if (ch == '/') ch = '\\';
    while (!want.empty() && want[0] == '\\') want.erase(0, 1);
    while (!want.empty() && want.back() == '\\') want.pop_back();
    // each name cut to 8.3, as DOS cuts what it is given (ABEL's OPNG asks for GSCAPTALUP.FMB,
    // and gets GSCAPTAL.FMB)
    std::string out, part;
    want += '\\';
    for (char ch : want) {
        if (ch != '\\') { part += ch; continue; }
        size_t dot = part.find('.');
        std::string base = part.substr(0, dot), ext = dot == std::string::npos ? "" : part.substr(dot + 1);
        if (part != "." && part != "..") {
            if (base.size() > 8) base.resize(8);
            if (ext.size() > 3) ext.resize(3);
            part = ext.empty() ? base : base + "." + ext;
        }
        out += (out.empty() ? "" : "\\") + part;
        part.clear();
    }
    return out;
}

bool Disc::find(const std::string& path, DiscFile& out) {
    index();
    std::string want = disc_path(path);
    for (const auto& f : files_)
        if (upper(f.name) == want) { out = f; return true; }
    return false;
}

bool Disc::list(const std::string& dir, std::vector<DiscFile>& out) {
    index();
    out.clear();
    std::string want = disc_path(dir), prefix = want.empty() ? "" : want + "\\";
    bool found = want.empty();
    for (const auto& f : files_) {
        std::string n = upper(f.name);
        if (n == want && f.dir) found = true;
        if (n.size() > prefix.size() && n.compare(0, prefix.size(), prefix) == 0
            && n.find('\\', prefix.size()) == std::string::npos) {
            out.push_back(f);
            out.back().name = f.name.substr(prefix.size());
        }
    }
    return found;
}

bool Disc::file(const std::string& path, std::vector<uint8_t>& out) {
    DiscFile f;
    if (!find(path, f) || f.dir) return false;
    out.resize(f.size);
    return read_bytes(f.lba, 0, f.size, out.data());
}

}  // namespace towns
