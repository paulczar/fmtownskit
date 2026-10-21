// fmtownskit runtime -- the I/O ports, the interrupt controllers and the machine's clock.
//
// The ports' meanings are MAME's fmtowns driver's (read, not copied: fmtowns.cpp,
// fmtowns_v.cpp), the devices kept as registers where nothing yet hangs on them. Time is the
// CPU's clocks at 16 MHz: VSYNC every 1/60 s (IRQ11), the vertical blank 2.5 ms long (MAME's
// figure), the sprite controller's drawing after its VSYNC, the free-running microsecond
// counter at 0x26, the RF5C68's channels stepping at its 20,833 Hz (its IRQ13 when a channel
// reaches the last byte of a 4 KB block, as MAME's end callback).
#include "towns.h"
#include <algorithm>
#include <cstring>

namespace towns {

// ---- the 8259s ----------------------------------------------------------------------------
static void pic_write(Pic& p, int a, uint8_t v) {
    if (a == 0) {
        if (v & 0x10) {                         // ICW1
            p.icw_step = 1; p.icw4 = v & 1; p.imr = 0; p.isr = 0; p.irr = 0; p.read_isr = 0;
            return;
        }
        if ((v & 0x18) == 0x08) {               // OCW3
            if (v & 2) p.read_isr = v & 1;
            return;
        }
        // OCW2
        int cmd = v >> 5;
        if (cmd == 1) {                         // non-specific EOI: the highest in service
            for (int i = 0; i < 8; ++i) if (p.isr & (1 << i)) { p.isr &= ~(1 << i); break; }
        } else if (cmd == 3) {                  // specific EOI
            p.isr &= ~(1 << (v & 7));
        }
        return;
    }
    if (p.icw_step == 1) { p.base = v & 0xF8; p.icw_step = 2; return; }
    if (p.icw_step == 2) { p.icw_step = p.icw4 ? 3 : 0; return; }
    if (p.icw_step == 3) { p.icw_step = 0; return; }
    p.imr = v;
}

static uint8_t pic_read(Pic& p, int a) {
    if (a == 0) return p.read_isr ? p.isr : p.irr;
    return p.imr;
}

void Machine::raise_irq(int irq, bool level) {
    Pic& p = pic[irq >> 3];
    uint8_t bit = (uint8_t)(1 << (irq & 7));
    if (level) p.irr |= bit; else p.irr &= ~bit;
}

// The interrupt to take now, as the 8259s in level-triggered mode decide it (IRR follows the
// lines), the master in special fully nested mode (TownsOS sets it: a slave request goes through
// while IR7 is in service, the slave's own in-service register deciding): its vector, put in
// service. A request with no protected-mode handler of the program's would go to real-mode
// handlers this runtime does not have: it is passed over, as if masked. -1: nothing to take.
int Machine::pending_irq() {
    Pic& m = pic[0];
    Pic& s = pic[1];
    int sirq = -1;
    for (int i = 0; i < 8; ++i) {
        if (s.isr & (1 << i)) break;
        if ((s.irr & ~s.imr & (1 << i)) && pm_vector[s.base + i].set) { sirq = i; break; }
    }
    int mirq = -1;
    for (int i = 0; i < 8; ++i) {
        bool req = i == 7 ? sirq >= 0 : (m.irr & (1 << i)) && pm_vector[m.base + i].set;
        if (req && !(m.imr & (1 << i))) { mirq = i; break; }
        if ((m.isr & (1 << i)) && i != 7) break;
    }
    if (mirq < 0) return -1;
    m.isr |= (uint8_t)(1 << mirq);
    if (mirq == 7) { s.isr |= (uint8_t)(1 << sirq); return s.base + sirq; }
    return m.base + mirq;
}

// The beam, as MAME's screen device places it: the vertical blank starts at the VSYNC, at the
// line after the visible area (the whole screen here, so line 0), a frame of 1/60 s over
// height x width pixels (the CRTC's total: registers 4 - 0 + 1 by 8 / 2 + 1).
void Machine::beam(int& hpos, int& vpos) const {
    const Video& v = video;
    int width = std::max(1, v.crtc[4] - v.crtc[0] + 1), height = std::max(1, v.crtc[8] / 2 + 1);
    uint64_t into = now - (next_vsync - kFrameClocks);
    uint64_t pixels = into * (uint64_t)width * height / kFrameClocks;
    vpos = (int)(pixels / width) % height;
    hpos = (int)(pixels % width);
}

// ---- time ------------------------------------------------------------------------------------
static constexpr uint32_t kPcmClocks = 768;     // 16 MHz / (8 MHz / 384)

struct PcmChannel { uint8_t env = 0, pan = 0, start = 0; uint16_t step = 0, loopst = 0; uint32_t addr = 0; bool on = false; };
static PcmChannel g_pcm[8];
static uint8_t g_pcm_cbank;
static uint64_t g_pcm_next = kPcmClocks;
static std::vector<int16_t> g_pcm_out;

static void pcm_sample(Machine& m) {
    Sound& s = m.sound;
    int left = 0, right = 0;
    bool enable = s.pcm_ctrl & 0x80;
    for (int ch = 0; ch < 8 && enable; ++ch) {
        PcmChannel& c = g_pcm[ch];
        if (!c.on) continue;
        if (((c.addr >> 11) & 0xFFF) == 0xFFF) {
            int bank = (int)((c.addr >> 11) / 0x2000);
            if (s.pcm_mask & (1 << bank)) {
                s.pcm_flags |= (uint8_t)(1 << bank);
                m.raise_irq(13, true);
            }
        }
        int sample = s.wave[(c.addr >> 11) & 0xFFFF];
        if (sample == 0xFF) {
            c.addr = (uint32_t)c.loopst << 11;
            sample = s.wave[(c.addr >> 11) & 0xFFFF];
            if (sample == 0xFF) continue;
        }
        c.addr += c.step;
        int lv = (c.pan & 0x0F) * c.env, rv = ((c.pan >> 4) & 0x0F) * c.env;
        if (sample & 0x80) { left += ((sample & 0x7F) * lv) >> 5; right += ((sample & 0x7F) * rv) >> 5; }
        else { left -= (sample * lv) >> 5; right -= (sample * rv) >> 5; }
    }
    left = std::clamp(left, -32768, 32767) & ~63;
    right = std::clamp(right, -32768, 32767) & ~63;
    g_pcm_out.push_back((int16_t)left);
    g_pcm_out.push_back((int16_t)right);
}

void pcm_take(std::vector<int16_t>& out) { out.swap(g_pcm_out); g_pcm_out.clear(); }

uint64_t Machine::next_event() const {
    uint64_t t = next_vsync;
    if (vblank) t = std::min(t, vblank_end);
    if (sprite.busy) t = std::min(t, sprite.done_at);
    t = std::min(t, g_pcm_next);
    return t;
}

void Machine::advance(uint64_t to) {
    while (now < to) {
        uint64_t t = std::min(next_event(), to);
        now = t;
        if (sprite.busy && now >= sprite.done_at) {
            sprite.busy = false;
            draw_sprites();
        }
        if (vblank && now >= vblank_end) {
            vblank = false;
            raise_irq(11, false);
        }
        while (g_pcm_next <= now) {             // the RF5C68's samples, silent while it is off
            pcm_sample(*this);
            g_pcm_next += kPcmClocks;
        }
        if (now >= next_vsync) {
            ++frame;
            vblank = true;
            vblank_end = now + kClock / 400;    // 2,500 us
            raise_irq(11, true);
            if ((sprite.reg[1] & 0x80) && !sprite.busy) {
                // the controller flips its page and draws the other (MAME's towns_vsync_irq)
                if (sprite.page == 0) { sprite.page = 1; video.crtc[21] &= ~0x8000; }
                else { sprite.page = 0; video.crtc[21] |= 0x8000; }
                int limit = (sprite.reg[0] | (sprite.reg[1] << 8)) & 0x3FF;
                sprite.busy = true;
                sprite.done_at = now + (uint64_t)(32 + 75 * (1024 - limit)) * (kClock / 1000000);
            }
            next_vsync += kFrameClocks;
            if (on_vsync) on_vsync(*this);
        }
    }
}

// ---- the ports -------------------------------------------------------------------------------
uint32_t Machine::in(uint32_t port, int n) {
    uint32_t v = 0;
    for (int k = 0; k < n; ++k) v |= (uint32_t)in8(port + k) << (8 * k);
    if (trace_io) std::fprintf(stderr, "  in  %04x.%d = %x\n", port, n, v);
    return v;
}

void Machine::out(uint32_t port, int n, uint32_t v) {
    if (trace_io) std::fprintf(stderr, "  out %04x.%d = %x\n", port, n, v);
    for (int k = 0; k < n; ++k) out8(port + k, (uint8_t)(v >> (8 * k)));
}

uint8_t Machine::in8(uint32_t port) {
    Video& v = video;
    switch (port) {
    case 0x00: return pic_read(pic[0], 0);
    case 0x02: return pic_read(pic[0], 2);
    case 0x10: return pic_read(pic[1], 0);
    case 0x12: return pic_read(pic[1], 2);
    case 0x20: case 0x25: return 0;
    case 0x26: return (uint8_t)(now / (kClock / 1000000));
    case 0x27: return (uint8_t)((now / (kClock / 1000000)) >> 8);
    case 0x28: return 0;
    case 0x30: return 0x01;
    case 0x31: return 0x01;
    case 0x60: return 0;
    case 0x6C: return 0;
    case 0x404: return 0;
    case 0x440: return v.crtc_sel;
    case 0x442: return v.crtc_sel == 30 ? 0 : (uint8_t)v.crtc[v.crtc_sel & 31];
    case 0x443: {
        if (v.crtc_sel != 30) return (uint8_t)(v.crtc[v.crtc_sel & 31] >> 8);
        int x, y;
        beam(x, y);
        uint8_t r = 0;
        if (x < (v.crtc[0] & 0xFE)) r |= 0x02;
        if (y < (v.crtc[6] & 0x1F)) r |= 0x04;
        int x0 = v.crtc[9] - v.crtc[0], x0e = v.crtc[10] - v.crtc[0];
        int x1 = v.crtc[11] - v.crtc[0], x1e = v.crtc[12] - v.crtc[0];
        int y0 = (v.crtc[13] - v.crtc[6]) / 2, y0e = (v.crtc[14] - v.crtc[6]) / 2 - 1;
        int y1 = (v.crtc[15] - v.crtc[6]) / 2, y1e = (v.crtc[16] - v.crtc[6]) / 2 - 1;
        if (x < x0e && x > x0) r |= 0x10;
        if (x < x1e && x > x1) r |= 0x20;
        if (y < y0e && y > y0) r |= 0x40;
        if (y < y1e && y > y1) r |= 0x80;
        return r;
    }
    case 0x448: return v.vsel;
    case 0x44A: return v.vreg[v.vsel & 1];
    case 0x44C: {
        uint8_t r = 0;
        if (v.dpmd) { v.dpmd = false; r |= 0x80; }
        if (sprite.busy) r |= 0x02;
        r |= sprite.page & 1;
        return r;
    }
    case 0x450: return sprite.sel;
    case 0x452: return sprite.sel == 6 ? (sprite.page ? 0x10 : 0) : sprite.reg[sprite.sel & 7];
    case 0x458: return v.mask_sel;
    case 0x45A: case 0x45B: return v.mask[((v.mask_sel & 1) << 1) + (port - 0x45A)];
    case 0x480: return 0;
    case 0x4D0: case 0x4D2: {
        int p = (port >> 1) & 1;
        uint8_t m = (uint8_t)((((pad_mask >> (p * 2)) & 1) << 4) | (((pad_mask >> (p * 2 + 1)) & 1) << 5)
                              | (((pad_mask >> (p + 4)) & 1) << 6));
        return pad[p] & (0x0F | m);
    }
    case 0x4D8: case 0x4DA: case 0x4DC: case 0x4DE: return 0;    // the YM3438: never busy, no timer
    case 0x4E0: case 0x4E1: return 0;
    case 0x4E2: return sound.volume[sound.volume_sel & 3];
    case 0x4E3: return sound.volume_sel;
    case 0x4E8: return 1;
    case 0x4E9: return (sound.pcm_flags ? 0x08 : 0);
    case 0x4EA: return sound.pcm_mask;
    case 0x4EB: {
        uint8_t r = sound.pcm_flags;
        sound.pcm_flags = 0;
        raise_irq(13, false);
        return r;
    }
    case 0x5C8: return 0;
    case 0x5E8: return 2;           // MB of RAM (MAME's fmtowns: 2)
    case 0x5EC: return 0;
    case 0xFD90: return v.pal_sel;
    case 0xFD92: case 0xFD94: case 0xFD96: {
        const uint8_t* rgb = (v.vreg[1] & 0x10) ? v.pal256[v.pal_sel] : v.pal16[(v.vreg[1] & 0x20) >> 5][v.pal_sel & 15];
        return port == 0xFD92 ? rgb[2] : port == 0xFD94 ? rgb[0] : rgb[1];
    }
    case 0xFDA0: {
        int x, y;
        beam(x, y);
        return (uint8_t)((vblank ? 1 : 0) | (x < v.crtc[10] - v.crtc[0] && x > v.crtc[9] - v.crtc[0] ? 2 : 0));
    }
    case 0xFF81: return (uint8_t)(((v.rplane << 6) & 0xC0) | v.wplane);
    case 0xFF82: return v.display_plane | v.display_page;
    case 0xFF83: return v.vram_page ? 0x10 : 0;
    case 0xFF86: {
        int x, y;
        beam(x, y);
        return (uint8_t)(0x10 | (vblank ? 0x04 : 0) | (x < v.crtc[10] - v.crtc[0] && x > v.crtc[9] - v.crtc[0] ? 0x80 : 0));
    }
    default:
        if (port >= 0xFD98 && port <= 0xFD9F) return v.degipal[port - 0xFD98];
        if (port >= 0x3000 && port < 0x5000) { unknown("CMOS read %04x", port); return 0; }
        unknown("in %04x", port);
        return 0;
    }
}

void Machine::out8(uint32_t port, uint8_t d) {
    Video& v = video;
    switch (port) {
    case 0x00: pic_write(pic[0], 0, d); return;
    case 0x02: pic_write(pic[0], 2, d); return;
    case 0x10: pic_write(pic[1], 0, d); return;
    case 0x12: pic_write(pic[1], 2, d); return;
    case 0x20: case 0x22: case 0x28: case 0x32: return;
    case 0x60: return;
    case 0x6C: return;              // a 1 us wait
    case 0x404: if (d & 0x80) unknown("0x404: main memory over the low VRAM windows"); return;
    case 0x440: v.crtc_sel = d & 31; return;
    case 0x442: v.crtc[v.crtc_sel] = (uint16_t)((v.crtc[v.crtc_sel] & 0xFF00) | d); return;
    case 0x443:
        if (v.crtc_sel == 21 && (sprite.reg[1] & 0x80))
            v.crtc[21] = (uint16_t)((v.crtc[21] & 0x80FF) | ((d & 0x7F) << 8));
        else
            v.crtc[v.crtc_sel] = (uint16_t)((v.crtc[v.crtc_sel] & 0x00FF) | (d << 8));
        return;
    case 0x448: v.vsel = d & 1; return;
    case 0x44A: v.vreg[v.vsel] = d; return;
    case 0x450: sprite.sel = d & 7; return;
    case 0x452:
        if (sprite.sel == 6) sprite.page = (d & 0x80) ? 1 : 0;
        else sprite.reg[sprite.sel] = d;
        return;
    case 0x458: v.mask_sel = d & 1; return;
    case 0x45A: case 0x45B: v.mask[((v.mask_sel & 1) << 1) + (port - 0x45A)] = d; vram_pages(); return;
    case 0x480: return;
    case 0x4D6: pad_mask = d; return;
    case 0x4D8: sound.opn_addr[0] = d; return;
    case 0x4DA: sound.opn[0][sound.opn_addr[0]] = d; return;
    case 0x4DC: sound.opn_addr[1] = d; return;
    case 0x4DE: sound.opn[1][sound.opn_addr[1]] = d; return;
    case 0x4E0: case 0x4E1: return;
    case 0x4E2: sound.volume[sound.volume_sel & 3] = d; return;
    case 0x4E3: sound.volume_sel = d; return;
    case 0x4EA: sound.pcm_mask = d; return;
    case 0x4EC: return;             // LED, mute
    case 0x4F0: case 0x4F1: case 0x4F2: case 0x4F3: case 0x4F4: case 0x4F5: case 0x4F6: {
        PcmChannel& c = g_pcm[g_pcm_cbank];
        switch (port - 0x4F0) {
        case 0: c.env = d; break;
        case 1: c.pan = d; break;
        case 2: c.step = (uint16_t)((c.step & 0xFF00) | d); break;
        case 3: c.step = (uint16_t)((c.step & 0x00FF) | (d << 8)); break;
        case 4: c.loopst = (uint16_t)((c.loopst & 0xFF00) | d); break;
        case 5: c.loopst = (uint16_t)((c.loopst & 0x00FF) | (d << 8)); break;
        case 6: c.start = d; if (!c.on) c.addr = (uint32_t)c.start << (8 + 11); break;
        }
        sound.pcm_reg[port - 0x4F0] = d;
        return;
    }
    case 0x4F7:
        if (d & 0x40) g_pcm_cbank = d & 7;
        // the wave RAM window's bank: kept in pcm_ctrl's low bits only when MOD is 0
        sound.pcm_ctrl = (uint8_t)((d & 0x80) | (d & 0x40 ? (sound.pcm_ctrl & 0x0F) : (d & 0x0F)));
        return;
    case 0x4F8:
        sound.pcm_onoff = d;
        for (int i = 0; i < 8; ++i) {
            g_pcm[i].on = !((d >> i) & 1);
            if (!g_pcm[i].on) g_pcm[i].addr = (uint32_t)g_pcm[i].start << (8 + 11);
        }
        return;
    case 0x5CA: raise_irq(11, false); return;
    case 0x5E8: case 0x5EC: return;
    // no device in MAME either: the odd bytes of TBIOS's 16- and 32-bit writes to 0x440 and
    // 0xFD92 (a palette entry written as one dword), and 0x4D5 (the sound module's)
    case 0x441: case 0xFD93: case 0xFD95: case 0x4D5: return;
    case 0xFD90: v.pal_sel = d; return;
    case 0xFD92: case 0xFD94: case 0xFD96: {
        int k = port == 0xFD92 ? 2 : port == 0xFD94 ? 0 : 1;   // b r g
        switch (v.vreg[1] & 0x30) {
        case 0x00: case 0x20: v.pal16[(v.vreg[1] & 0x20) >> 5][v.pal_sel & 15][k] = d; break;
        default: v.pal256[v.pal_sel][k] = d; break;
        }
        return;
    }
    case 0xFDA0: v.layer_ctrl = d; return;
    case 0xFF81: v.wplane = d & 0x0F; v.rplane = (d & 0xC0) >> 6; return;
    case 0xFF82: v.display_plane = d & 0x27; v.display_page = d & 0x10; return;
    case 0xFF83: v.vram_page = d & 0x10; return;
    default:
        if (port >= 0xFD98 && port <= 0xFD9F) { v.degipal[port - 0xFD98] = d; v.dpmd = true; return; }
        if (port >= 0x3000 && port < 0x5000) { unknown("CMOS write %04x", port); return; }
        unknown("out %04x", port);
    }
}

}  // namespace towns
