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
    std::string cur;
    while (std::fgets(line, sizeof line, f)) {
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
            err = "a gap not in the files is not read here"; std::fclose(f); return false;
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
        files_.push_back({prefix + name, elba, esz, dir});
        if (dir) walk(elba, esz, prefix + name + "\\", depth + 1);
    }
}

static std::string upper(std::string s) {
    for (auto& ch : s) ch = (char)std::toupper((unsigned char)ch);
    return s;
}

bool Disc::find(const std::string& path, DiscFile& out) {
    if (!walked_) {
        walked_ = true;
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
    std::string want = upper(path);
    for (auto& ch : want) if (ch == '/') ch = '\\';
    while (!want.empty() && want[0] == '\\') want.erase(0, 1);
    for (const auto& f : files_)
        if (upper(f.name) == want) { out = f; return true; }
    return false;
}

bool Disc::file(const std::string& path, std::vector<uint8_t>& out) {
    DiscFile f;
    if (!find(path, f) || f.dir) return false;
    out.resize(f.size);
    return read_bytes(f.lba, 0, f.size, out.data());
}

}  // namespace towns
