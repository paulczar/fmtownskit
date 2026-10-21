// fmtownskit runtime -- the picture: the sprite controller drawing into VRAM, and a frame
// composed from the CRTC's two layers.
//
// Both as MAME's fmtowns_v.cpp does them (draw_sprites, screen_update and its scan-line
// drawers for 16 colours, 256 colours and 15-bit colour), read and written anew: the
// oracle a port is held against is MAME, so a frame here is what MAME's would be from the
// same registers and memory. The real CRTC is not modelled beyond that.
#include "towns.h"
#include <algorithm>
#include <cstring>

namespace towns {

// ---- sprites ----------------------------------------------------------------------------------
namespace {
struct Spr {
    Machine& m;
    uint8_t* tv;        // sprite RAM
    uint8_t* gv;        // VRAM
    int linesize;
    uint32_t vbase;
    void put(uint32_t voffset, uint16_t col, uint16_t xpos, uint16_t ypos) {
        if (voffset < 0x20000 && xpos < 256 && ypos < 256 && voffset > (uint32_t)linesize) {
            gv[0x40000 + voffset + vbase + 1] = (uint8_t)(col >> 8);
            gv[0x40000 + voffset + vbase] = (uint8_t)col;
        }
    }
    void four(uint32_t poffset, uint32_t coffset, uint16_t x, uint16_t y, bool xflip, bool yflip,
              bool xhalf, bool yhalf, bool rot) {
        if (rot) { std::swap(x, y); std::swap(xflip, yflip); }
        uint16_t xstart, xend, ystart, yend; int xdir, ydir;
        if (xflip) {
            if (xhalf) { xstart = x + 6; xdir = -1; } else { xstart = x + 14; xdir = -2; }
            xend = x - 2;
        } else {
            xstart = x + 1;
            if (xhalf) { xend = x + 9; xdir = 1; } else { xend = x + 17; xdir = 2; }
        }
        if (yflip) { ystart = yhalf ? y + 7 : y + 15; yend = y - 1; ydir = -1; }
        else { ystart = y; yend = yhalf ? y + 8 : y + 16; ydir = 1; }
        xstart &= 0x1FF; xend &= 0x1FF; ystart &= 0x1FF; yend &= 0x1FF;
        poffset &= 0x1FFFF;
        for (uint16_t ypos = ystart; ypos != yend; ypos = (uint16_t)((ypos + ydir) & 0x1FF)) {
            for (uint16_t xpos = xstart; xpos != xend; xpos = (uint16_t)((xpos + xdir) & 0x1FF)) {
                uint32_t voffset = 0;
                int pixel = (tv[poffset] & 0xF0) >> 4;
                uint16_t col = (uint16_t)((tv[(coffset + pixel * 2) & 0x1FFFF] | (tv[(coffset + pixel * 2 + 1) & 0x1FFFF] << 8)) & 0x7FFF);
                if (rot) { voffset += linesize * (xpos & 0x1FF); voffset += (ypos & 0x1FF) * 2; }
                else { voffset += linesize * (ypos & 0x1FF); voffset += (xpos & 0x1FF) * 2; }
                if (pixel) put(voffset, col, xpos, ypos);
                if (!xhalf) {
                    if (xflip) voffset += rot ? linesize : 2;
                    else voffset -= rot ? linesize : 2;
                    pixel = tv[poffset] & 0x0F;
                    col = (uint16_t)((tv[(coffset + pixel * 2) & 0x1FFFF] | (tv[(coffset + pixel * 2 + 1) & 0x1FFFF] << 8)) & 0x7FFF);
                    // the byte's second pixel, clipped at its own x: MAME clips it at the first's,
                    // so a mirrored sprite's pixel at x 256 lands at the next line's x 0 (a
                    // column of the sprite at the left edge; session 4, the CPU pushed out right)
                    if (pixel) put(voffset, col, (uint16_t)(xflip ? xpos + 1 : xpos - 1), ypos);
                }
                poffset = (poffset + 1) & 0x1FFFF;
            }
            if (yhalf) poffset += 8;
        }
    }
    void sixteen(uint32_t poffset, uint16_t x, uint16_t y, bool xflip, bool yflip, bool xhalf, bool yhalf, bool rot) {
        if (rot) { std::swap(x, y); std::swap(xflip, yflip); }
        uint16_t xstart, xend, ystart, yend; int xdir, ydir;
        if (xflip) { xstart = xhalf ? x + 7 : x + 15; xend = x - 1; xdir = -1; }
        else { xstart = x; xend = xhalf ? x + 8 : x + 16; xdir = 1; }
        if (yflip) { ystart = yhalf ? y + 7 : y + 15; yend = y - 1; ydir = -1; }
        else { ystart = y; yend = yhalf ? y + 8 : y + 16; ydir = 1; }
        xstart &= 0x1FF; xend &= 0x1FF; ystart &= 0x1FF; yend &= 0x1FF;
        poffset &= 0x1FFFF;
        for (uint16_t ypos = ystart; ypos != yend; ypos = (uint16_t)((ypos + ydir) & 0x1FF)) {
            for (uint16_t xpos = xstart; xpos != xend; xpos = (uint16_t)((xpos + xdir) & 0x1FF)) {
                uint32_t voffset = 0;
                uint16_t col = (uint16_t)(tv[poffset] | (tv[(poffset + 1) & 0x1FFFF] << 8));
                if (rot) { voffset += linesize * (xpos & 0x1FF); voffset += (ypos & 0x1FF) * 2; }
                else { voffset += linesize * (ypos & 0x1FF); voffset += (xpos & 0x1FF) * 2; }
                if (col < 0x8000) put(voffset, col, xpos, ypos);
                poffset = (poffset + (xhalf ? 4 : 2)) & 0x1FFFF;
            }
            if (yhalf) poffset += 16;
        }
    }
};
}  // namespace

void Machine::draw_sprites() {
    int limit = (sprite.reg[0] | (sprite.reg[1] << 8)) & 0x3FF;
    uint16_t xoff = (uint16_t)((sprite.reg[2] | (sprite.reg[3] << 8)) & 0x1FF);
    uint16_t yoff = (uint16_t)((sprite.reg[4] | (sprite.reg[5] << 8)) & 0x1FF);
    int linesize = video.crtc[24] * 4;
    Spr s{*this, sprram.data(), vram.data(), linesize, (uint32_t)(sprite.page ? 0x20000 : 0)};
    // the page cleared to its first line (MAME: what aburner and rbisland ask of it)
    uint8_t* page = vram.data() + (sprite.page ? 0x60000 : 0x40000);
    if (linesize > 0)
        for (int i = linesize; i + linesize <= 0x20000; i += linesize) std::memcpy(page + i, page, linesize);
    for (int n = limit; n < 1024; ++n) {
        const uint8_t* r = &sprram[8 * n];
        uint16_t x = (uint16_t)(r[0] | (r[1] << 8)), y = (uint16_t)(r[2] | (r[3] << 8));
        uint16_t attr = (uint16_t)(r[4] | (r[5] << 8)), colour = (uint16_t)(r[6] | (r[7] << 8));
        bool xflip = attr & 0x2000, yflip = attr & 0x1000, rot = attr & 0x4000;
        bool xhalf = attr & 0x400, yhalf = attr & 0x800;
        if (attr & 0x8000) { x += xoff; y += yoff; }
        x &= 0x1FF; y &= 0x1FF;
        uint32_t poffset = (uint32_t)(attr & 0x3FF) << 7;
        if (colour & 0x2000) continue;
        if (colour & 0x8000) s.four(poffset, (uint32_t)(colour & 0xFFF) << 5, x, y, xflip, yflip, xhalf, yhalf, rot);
        else s.sixteen(poffset, x, y, xflip, yflip, xhalf, yhalf, rot);
    }
}

// ---- the frame --------------------------------------------------------------------------------
namespace {
struct Rect { int min_x, max_x, min_y, max_y; };

struct Frame {
    Machine& m;
    const Video& v;
    std::vector<uint32_t>& px;
    int w, h;
    void set(int y, int x, uint32_t c) { if (x >= 0 && x < w && y >= 0 && y < h) px[(size_t)y * w + x] = c; }
    static uint32_t rgb(const uint8_t* p) { return (uint32_t)p[0] << 16 | (uint32_t)p[1] << 8 | p[2]; }

    void hicolour(const Rect& r, int layer, int line, int scanline) {
        const uint8_t* g = m.vram.data();
        uint32_t off = 0; int hzoom, linesize, page = 0;
        bool sph = !(v.crtc[28] & (layer ? 0x20 : 0x10));
        if (v.vreg[0] & 0x10) linesize = (layer == 0 ? v.crtc[20] : v.crtc[24]) * 4;
        else linesize = v.crtc[20] * 8;
        if (v.display_page) { off = 0x20000; page = 1; }
        if (layer) {
            if (!(v.vreg[0] & 0x10)) return;
            if (!sph) off += v.crtc[21] << 2;
            else off += ((v.crtc[21] & 0xFC00) << 2) | ((v.crtc[21] & 0x3FF) << 2);
            hzoom = ((v.crtc[27] & 0x0F00) >> 8) + 1;
            off += (uint32_t)(int)((float)(v.crtc[11] - v.crtc[22]) * (2.0f / (float)hzoom));
        } else {
            if (!sph) off += v.crtc[17] << 2;
            else off += ((v.crtc[17] & 0xFC00) << 2) | ((v.crtc[17] & 0x3FF) << 2);
            hzoom = (v.crtc[27] & 0x000F) + 1;
            off += (uint32_t)(int)((float)(v.crtc[9] - v.crtc[18]) * (2.0f / (float)hzoom));
        }
        off += line * linesize;
        off &= ~1u;
        for (int x = r.min_x; x < r.max_x; x += hzoom) {
            int offpage; uint32_t cur;
            if (v.vreg[0] & 0x10) { cur = off & 0x3FFFF; offpage = layer; }
            else { offpage = (off & 4) >> 2; cur = ((off & 0x7FFF8) >> 1) | (off & 3); }
            uint32_t a = cur + offpage * 0x40000;
            uint16_t c = (uint16_t)((g[(a + 1) & 0x7FFFF] << 8) | g[a & 0x7FFFF]);
            if (!(v.vreg[0] & 0x10) || (v.vreg[1] & 1) != layer || c < 0x8000) {
                uint32_t out = ((c & 0x1F) << 3) | ((c & 0x7C00) << 1) | ((uint32_t)(c & 0x3E0) << 14);
                for (int p = 0; p < hzoom; ++p) set(scanline, x + p, out);
            }
            off += 2;
            if (linesize && (off - page * 0x20000) % linesize == 0 && sph) off -= linesize;
        }
    }

    void c256(const Rect& r, int line, int scanline) {
        const uint8_t* g = m.vram.data();
        int off = 0, page = 0;
        bool sph = !(v.crtc[28] & 0x10);
        if (v.display_page) { off = 0x20000; page = 1; }
        int linesize = v.crtc[20] * 4;
        if (!sph) off += v.crtc[17] << 2;
        else off += ((v.crtc[17] & 0xFC00) << 2) | ((v.crtc[17] & 0x3FF) << 2);
        int hzoom = (v.crtc[27] & 0x000F) + 1;
        int subpix = (v.crtc[9] - v.crtc[18]) / hzoom;
        off += line * linesize;
        off += (subpix >> 1) & ~3;
        subpix &= 7;
        for (int x = r.min_x; x < r.max_x; x += hzoom) {
            off &= 0x3FFFF;
            uint8_t c = g[off + (subpix >= 4 ? (subpix & 3) + 0x40000 : subpix)];
            for (int p = 0; p < hzoom; ++p) set(scanline, x + p, rgb(v.pal256[c]));
            if (++subpix == 8) { off += 4; subpix = 0; }
            if (linesize && (off - page * 0x20000) % linesize == 0 && subpix == 0 && sph) off -= linesize;
        }
    }

    void c16(const Rect& r, int layer, int line, int scanline) {
        const uint8_t* g = m.vram.data();
        int off = 0, page = 0, hzoom;
        bool sph = !(v.crtc[28] & (layer ? 0x20 : 0x10));
        if (v.display_page) { off = 0x20000; page = 1; }
        bool bottom = (v.vreg[1] & 1) != layer;
        int linesize = (layer == 0 ? v.crtc[20] : v.crtc[24]) * 4;
        if (layer) {
            if (!(v.vreg[0] & 0x10)) return;
            if (!sph) off += v.crtc[21] << 2;
            else off += ((v.crtc[21] & 0xFC00) << 2) | ((v.crtc[21] & 0x3FF) << 2);
            hzoom = ((v.crtc[27] & 0x0F00) >> 8) + 1;
            off += (v.crtc[11] - v.crtc[22]) / (hzoom * 2);
        } else {
            if (!sph) off += v.crtc[17] << 2;
            else off += ((v.crtc[17] & 0xFC00) << 2) | ((v.crtc[17] & 0x3FF) << 2);
            hzoom = (v.crtc[27] & 0x000F) + 1;
            off += (v.crtc[9] - v.crtc[18]) / (hzoom * 2);
        }
        off += line * linesize;
        for (int x = r.min_x; x < r.max_x; x += hzoom * 2) {
            off &= (v.vreg[0] & 0x10) ? 0x3FFFF : 0x7FFFF;
            uint8_t byte = g[(off + layer * 0x40000) & 0x7FFFF];
            uint8_t c = byte >> 4;
            if (c || bottom) for (int p = 0; p < hzoom; ++p) set(scanline, x + hzoom + p, rgb(v.pal16[layer][c]));
            c = byte & 0x0F;
            if (c || bottom) for (int p = 0; p < hzoom; ++p) set(scanline, x + p, rgb(v.pal16[layer][c]));
            ++off;
            if (linesize && (off - page * 0x20000) % linesize == 0 && sph) off -= linesize;
        }
    }

    void layer(const Rect& r, const Rect& scr, int n) {
        int scanline = r.min_y;
        int top = scanline - scr.min_y;
        int bottom = (r.max_y - r.min_y) + top;
        int zoom = n == 0 ? ((v.crtc[27] & 0x00F0) >> 4) + 1 : ((v.crtc[27] & 0xF000) >> 12) + 1;
        int count = top % zoom;
        bottom /= zoom; top /= zoom;
        int mode = n == 0 ? (v.vreg[0] & 0x03) : (v.vreg[0] & 0x0C) >> 2;
        if (n == 1 && mode != 1 && mode != 3) return;
        for (int line = top; line <= bottom; ++line) {
            do {
                if (mode == 1) c16(r, n, line, scanline);
                else if (mode == 2) c256(r, line, scanline);
                else if (mode == 3) hicolour(r, n, line, scanline);
                ++scanline; ++count;
            } while (count < zoom);
            count = 0;
        }
    }
};
}  // namespace

void Machine::render(std::vector<uint32_t>& rgb, int& width, int& height) {
    const Video& v = video;
    width = v.crtc[4] - v.crtc[0] + 1;
    height = v.crtc[8] / 2 + 1;
    if (width <= 1 || height <= 1 || width > 1024 || height > 1024) { width = 640; height = 480; }
    rgb.assign((size_t)width * height, 0);
    Rect scr0{v.crtc[9] - v.crtc[0], v.crtc[10] - v.crtc[0], (v.crtc[13] - v.crtc[6]) / 2, (v.crtc[14] - v.crtc[6]) / 2 - 1};
    Rect scr1{v.crtc[11] - v.crtc[0], v.crtc[12] - v.crtc[0], (v.crtc[15] - v.crtc[6]) / 2, (v.crtc[16] - v.crtc[6]) / 2 - 1};
    auto clip = [&](Rect r) {
        r.min_x = std::max(r.min_x, 0); r.min_y = std::max(r.min_y, 0);
        r.max_x = std::min(r.max_x, width - 1); r.max_y = std::min(r.max_y, height - 1);
        return r;
    };
    Rect c0 = clip(scr0), c1 = clip(scr1);
    Frame f{*this, v, rgb, width, height};
    auto draw = [&](int n) {
        const Rect& c = n ? c1 : c0;
        if (c.max_x < c.min_x || c.max_y < c.min_y) return;
        f.layer(c, n ? scr1 : scr0, n);
    };
    if (!(v.vreg[1] & 1)) {
        if (v.layer_ctrl & 0x03) draw(1);
        if (v.layer_ctrl & 0x0C) draw(0);
    } else {
        if (v.layer_ctrl & 0x0C) draw(0);
        if (v.layer_ctrl & 0x03) draw(1);
    }
}

void Machine::display_area(int& x, int& y, int& w, int& h) const {
    const Video& v = video;
    int width = v.crtc[4] - v.crtc[0] + 1, height = v.crtc[8] / 2 + 1;
    if (width <= 1 || height <= 1 || width > 1024 || height > 1024) { width = 640; height = 480; }
    // the two layers' windows (as render() places them), clipped to the frame, joined
    int x0 = width, y0 = height, x1 = -1, y1 = -1;
    for (int n = 0; n < 2; ++n) {
        // (the end register is the first pixel past the window: render() leaves that column empty)
        int lx0 = std::max(v.crtc[9 + 2 * n] - v.crtc[0], 0), lx1 = std::min(v.crtc[10 + 2 * n] - v.crtc[0] - 1, width - 1);
        int ly0 = std::max((v.crtc[13 + 2 * n] - v.crtc[6]) / 2, 0), ly1 = std::min((v.crtc[14 + 2 * n] - v.crtc[6]) / 2 - 1, height - 1);
        if (lx1 < lx0 || ly1 < ly0) continue;
        x0 = std::min(x0, lx0); y0 = std::min(y0, ly0); x1 = std::max(x1, lx1); y1 = std::max(y1, ly1);
    }
    if (x1 < x0) { x = 0; y = 0; w = width; h = height; return; }
    x = x0; y = y0; w = x1 - x0 + 1; h = y1 - y0 + 1;
}

// ---- PNG: stored deflate blocks --------------------------------------------------------------
static void be32(std::vector<uint8_t>& o, uint32_t v) {
    o.push_back((uint8_t)(v >> 24)); o.push_back((uint8_t)(v >> 16)); o.push_back((uint8_t)(v >> 8)); o.push_back((uint8_t)v);
}

static void chunk(FILE* f, const char* type, const std::vector<uint8_t>& data) {
    std::vector<uint8_t> o;
    be32(o, (uint32_t)data.size());
    std::vector<uint8_t> body(type, type + 4);
    body.insert(body.end(), data.begin(), data.end());
    o.insert(o.end(), body.begin(), body.end());
    be32(o, x86_crc32(body.data(), body.size()));
    std::fwrite(o.data(), 1, o.size(), f);
}

bool write_png(const std::string& path, const std::vector<uint32_t>& rgb, int w, int h) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 13, 10, 26, 10};
    std::fwrite(sig, 1, 8, f);
    std::vector<uint8_t> ihdr;
    be32(ihdr, (uint32_t)w); be32(ihdr, (uint32_t)h);
    ihdr.insert(ihdr.end(), {8, 2, 0, 0, 0});
    chunk(f, "IHDR", ihdr);
    std::vector<uint8_t> raw;
    raw.reserve((size_t)h * (w * 3 + 1));
    for (int y = 0; y < h; ++y) {
        raw.push_back(0);
        for (int x = 0; x < w; ++x) {
            uint32_t c = rgb[(size_t)y * w + x];
            raw.push_back((uint8_t)(c >> 16)); raw.push_back((uint8_t)(c >> 8)); raw.push_back((uint8_t)c);
        }
    }
    std::vector<uint8_t> z = {0x78, 0x01};
    uint32_t a = 1, b = 0;
    for (uint8_t ch : raw) { a = (a + ch) % 65521; b = (b + a) % 65521; }
    for (size_t p = 0; p < raw.size() || p == 0;) {
        size_t n = std::min<size_t>(65535, raw.size() - p);
        bool last = p + n >= raw.size();
        z.push_back(last ? 1 : 0);
        z.push_back((uint8_t)n); z.push_back((uint8_t)(n >> 8));
        z.push_back((uint8_t)~n); z.push_back((uint8_t)(~n >> 8));
        z.insert(z.end(), raw.begin() + p, raw.begin() + p + n);
        p += n;
        if (last) break;
    }
    be32(z, (b << 16) | a);
    chunk(f, "IDAT", z);
    chunk(f, "IEND", {});
    std::fclose(f);
    return true;
}

}  // namespace towns
