// fmtownskit runtime -- linear memory and the descriptors, as RUN386 leaves them on a Towns.
//
// Plain memory (RAM, sprite RAM, VRAM while its write mask is all ones) is host pages in
// x86.h's table; the rest comes here a byte at a time: VRAM through the write mask, VRAM's
// single-layer window (the same bytes, the address bits swapped), the RF5C68's wave RAM
// through its 4 KB bank, the font ROM when one is given. What has no device is logged once.
#include "towns.h"
#include <cstdarg>
#include <cstring>
#include <set>
#include <string>

// The linear write watch (declared in x86.h). Global scope, because the inline store paths in that header
// reference it from outside any namespace. Off unless a port arms it, and bounded so a program that writes
// the watched byte in a loop cannot bury the useful first hit.
uint32_t g_watch_lo = 0, g_watch_hi = 0;
const X86Cpu* g_x86_cur = nullptr;
static long g_watch_hits = 0;
// The bound is a default, not a law: 64 hits is right for "what writes this one byte" and wrong for "what does
// this redraw write", where the fills come first and bury the interesting part. A port raises it with
// g_watch_max, and arms late (the port's U2_WATCH_AT) so the log holds the operation under study rather than the
// program's start-up. Measured before this knob existed: four different watches all reported exactly 64 lines,
// all of them the same start-up fill, which read as "nothing else writes this range".
uint32_t g_watch_max = 64;
void x86_watch_write(uint32_t a, uint32_t v, int n) {
    if ((uint32_t)++g_watch_hits > g_watch_max) {
        if ((uint32_t)g_watch_hits == g_watch_max + 1) std::fprintf(stderr, "watch     capped at %u hits\n", g_watch_max);
        return;
    }
    const X86Cpu* c = g_x86_cur;
    const uint32_t mask = n == 4 ? 0xFFFFFFFFu : n == 2 ? 0xFFFFu : 0xFFu;
    std::fprintf(stderr, "watch     %08x = %0*x (%dB) at %04x:%08x eax %08x\n", a, n * 2, v & mask, n,
                 c ? (unsigned)c->sreg[1] : 0, c ? c->eip : 0, g_far_eax);
}

namespace towns {

Machine* g_towns;

// MAME's fmtowns as it comes (2 MB), read under the first game (towns.PROFILES); how much a
// program asking for all memory gets was not read there (0: refused)
const Profile kTowns2M = {"towns2m", 2, 0x200000, 0x49000, 0, 0x1FFFFF};

static std::set<std::string> g_unknown;
void unknown(const char* fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (g_unknown.insert(buf).second && g_unknown.size() < 400) std::fprintf(stderr, "towns: %s\n", buf);
}

static std::vector<std::unique_ptr<uint8_t[]>> g_ram_pages;

Machine::Machine() {
    g_towns = this;
    std::memset(g_x86_page, 0, sizeof g_x86_page);
    vram_pages();
    for (uint32_t a = 0; a < 0x20000; a += 0x1000) g_x86_page[(0x81000000u + a) >> 12] = &sprram[a];
    sound_start();
    townsos();
}

// The machine as TownsOS leaves it to a program RUN386 starts: read on MAME's fmtowns at the
// program's first instruction (tools/oracle.py --dump-write, The Queen of Duellist): the PICs
// (level-triggered, the master at int 40h, the slave at 48h on its IR7, OCW3 reading the
// in-service register), the CRTC in the 640x480 mode of two 16-colour layers, the video
// output registers, the FM-R registers and both 16-colour palettes (the 256-colour one: the
// eight colours at full brightness, 32 times over).
void Machine::townsos() {
    pic[0] = Pic{}; pic[1] = Pic{};
    pic[0].base = 0x40; pic[0].imr = 0x3C; pic[0].read_isr = 1;
    pic[1].base = 0x48; pic[1].imr = 0xEE; pic[1].read_isr = 1;
    static const uint16_t crtc[32] = {
        0x0040, 0x0320, 0x0000, 0x0000, 0x035F, 0x0000, 0x0010, 0x0000, 0x036F, 0x009C, 0x031C, 0x009C,
        0x031C, 0x0040, 0x0360, 0x0040, 0x0360, 0x0000, 0x009C, 0x0000, 0x0050, 0x0000, 0x009C, 0x0000,
        0x0050, 0x004A, 0x0001, 0x0000, 0x803F, 0x0003, 0x0000, 0x0150};
    std::memcpy(video.crtc, crtc, sizeof crtc);
    video.vreg[0] = 0x15; video.vreg[1] = 0x28;
    video.wplane = 0x07; video.display_plane = 0x07; video.display_page = 0;
    static const uint8_t pal[16][3] = {
        {0x00, 0x00, 0x00}, {0x00, 0x00, 0x80}, {0x80, 0x00, 0x00}, {0x80, 0x00, 0x80},
        {0x00, 0x80, 0x00}, {0x00, 0x80, 0x80}, {0x80, 0x80, 0x00}, {0x80, 0x80, 0x80},
        {0x40, 0x40, 0x40}, {0x00, 0x00, 0xF0}, {0xF0, 0x00, 0x00}, {0xF0, 0x00, 0xF0},
        {0x00, 0xF0, 0x00}, {0x00, 0xF0, 0xF0}, {0xF0, 0xF0, 0x00}, {0xF0, 0xF0, 0xF0}};   // r g b
    std::memcpy(video.pal16[0], pal, sizeof pal);
    std::memcpy(video.pal16[1], pal, sizeof pal);
    for (int i = 0; i < 256; ++i) {                 // bit 0 red, 1 green, 2 blue
        video.pal256[i][0] = i & 1 ? 0xFF : 0; video.pal256[i][1] = i & 2 ? 0xFF : 0; video.pal256[i][2] = i & 4 ? 0xFF : 0;
    }
    // the colour registers as the boot left them (*inferred*: MAME's are not in its save state;
    // the last palettes written, the 16-colour ones for entries 0-15)
    std::memcpy(video.pal_reg, video.pal256, sizeof video.pal_reg);
    std::memcpy(video.pal_reg, pal, sizeof pal);
}

Machine::~Machine() {
    for (auto& h : handles) if (h.f) std::fclose(h.f);
    if (g_towns == this) g_towns = nullptr;
}

void Machine::map_ram(uint32_t linear, uint32_t size) {
    for (uint32_t a = linear & ~0xFFFu; a < linear + size; a += 0x1000) {
        if (g_x86_page[a >> 12]) continue;
        g_ram_pages.emplace_back(new uint8_t[4096]());
        g_x86_page[a >> 12] = g_ram_pages.back().get();
    }
}

void Machine::unmap_font_window() {
    // Drop the RAM pages covering the ANK font's physical window so read8's font cases answer instead. Without this the
    // page table hits first and returns the zero-filled RAM that boot_towns put there.
    for (uint32_t a = 0xCA000; a < 0xCC000; a += 0x1000) g_x86_page[a >> 12] = nullptr;
}

uint8_t* Machine::host(uint32_t a) {
    uint8_t* p = g_x86_page[a >> 12];
    return p ? p + (a & 0xFFF) : nullptr;
}

void Machine::write_block(uint32_t a, const uint8_t* p, uint32_t n) {
    for (uint32_t k = 0; k < n; ++k) mem_write(a + k, 1, p[k]);
}

void Machine::read_block(uint32_t a, uint8_t* p, uint32_t n) {
    for (uint32_t k = 0; k < n; ++k) p[k] = (uint8_t)mem_read(a + k, 1);
}

// VRAM's pages are host memory while every byte of the write mask is 0xFF.
void Machine::vram_pages() {
    bool plain = !count_access && video.mask[0] == 0xFF && video.mask[1] == 0xFF && video.mask[2] == 0xFF
                 && video.mask[3] == 0xFF;
    for (uint32_t a = 0; a < 0x100000; a += 0x1000)
        g_x86_page[(0x80000000u + a) >> 12] = plain ? &vram[a & 0x7FFFF] : nullptr;
}

void Machine::set_count_access(bool on) {
    count_access = on;
    vram_pages();
    for (uint32_t a = 0; a < 0x20000; a += 0x1000) g_x86_page[(0x81000000u + a) >> 12] = on ? nullptr : &sprram[a];
}

static inline uint32_t packed(uint32_t o) {
    // MAME's bitswap<19>(o, 2,18..3,1,0): bit 2 to the top, bits 3-18 down one
    return ((o & 4) << 16) | ((o >> 1) & 0x3FFFC) | (o & 3);
}

static uint8_t read8(Machine& m, uint32_t a) {
    if (uint8_t* p = g_x86_page[a >> 12]) return p[a & 0xFFF];
    if (a >= 0x80000000u && a < 0x80100000u) return m.vram[a & 0x7FFFF];
    if (a >= 0x80100000u && a < 0x80200000u) return m.vram[packed(a & 0x7FFFF)];
    if (a >= 0xC2200000u && a < 0xC2201000u) {
        uint32_t bank = (m.sound.pcm_ctrl & 0x0F) << 12;
        return m.sound.wave[bank | (a & 0xFFF)];
    }
    if (a >= 0xC2100000u && a < 0xC2140000u && !m.font.empty()) return m.font[(a - 0xC2100000u) % m.font.size()];
    // THE 20-DOT FONT, a second ROM with its own window (emulator's townsdef.h: base 0xC2180000, 512 KB, masked).
    // It is separate from the ANK font above and from the PCM window at 0xC2200000, and a program that renders a title
    // screen's large text can be using this one rather than the ANK block.
    if (a >= 0xC2180000u && a < 0xC2200000u && !m.font20.empty()) return m.font20[(a - 0xC2180000u) % m.font20.size()];
    // THE ANK FONT'S PHYSICAL WINDOW, which is a different view of the same ROM. The emulator's townsdef.h gives the
    // mapping outright: physical 0xCA000 is the 8x8 ANK block and reads font ROM offset 0x3D000, physical 0xCB000 is
    // the 16x16 block and reads 0x3D800. It is not linear across the two, which is why a window mapped by subtraction
    // alone gives the wrong glyphs. These addresses sit inside the low 768 KB that boot_towns maps as RAM, so the RAM
    // pages for them have to be dropped first (Machine::unmap_font_window), or the page table answers instead and the
    // game reads zeroes: that is what made the copyright notice's box empty while its bars, which are layer art, drew.
    if (a >= 0xCA000u && a < 0xCB000u && !m.font.empty()) return m.font[(0x3D000u + (a - 0xCA000u)) % m.font.size()];
    if (a >= 0xCB000u && a < 0xCC000u && !m.font.empty()) return m.font[(0x3D800u + (a - 0xCB000u)) % m.font.size()];
    if (a >= 0x81000000u && a < 0x81020000u) return m.sprram[a - 0x81000000u];     // count_access
    unknown("read of linear %08x (no device)", a & ~0xFFFu);
    return 0;
}

static void write8(Machine& m, uint32_t a, uint8_t v) {
    if (uint8_t* p = g_x86_page[a >> 12]) { p[a & 0xFFF] = v; return; }
    if (a >= 0x80000000u && a < 0x80200000u) {
        uint32_t o = a & 0x7FFFF;
        uint8_t mask = m.video.mask[o & 3];
        if (a >= 0x80100000u) o = packed(o);
        m.vram[o] = (uint8_t)((m.vram[o] & ~mask) | (v & mask));
        return;
    }
    if (a >= 0xC2200000u && a < 0xC2201000u) {
        uint32_t bank = (m.sound.pcm_ctrl & 0x0F) << 12;
        m.sound.wave[bank | (a & 0xFFF)] = v;
        return;
    }
    if (a >= 0x81000000u && a < 0x81020000u) { m.sprram[a - 0x81000000u] = v; return; }   // count_access
    unknown("write of linear %08x (no device)", a & ~0xFFFu);
}

uint32_t Machine::mem_read(uint32_t a, int n) {
    if (count_access) {
        if (a >= 0x80000000u && a < 0x80200000u) ++access.vram_r;
        else if (a >= 0x81000000u && a < 0x81020000u) ++access.spr_r;
    }
    uint32_t v = 0;
    for (int k = 0; k < n; ++k) v |= (uint32_t)read8(*this, a + k) << (8 * k);
    return v;
}

void Machine::mem_write(uint32_t a, int n, uint32_t v) {
    if (count_access) {
        if (a >= 0x80000000u && a < 0x80200000u) ++access.vram_w;
        else if (a >= 0x81000000u && a < 0x81020000u) ++access.spr_w;
    }
    for (int k = 0; k < n; ++k) write8(*this, a + k, (uint8_t)(v >> (8 * k)));
}

void Machine::set_desc(uint16_t sel, uint32_t base, uint32_t limit) {
    Descriptor& d = desc[(sel & 0x1FFC) >> 2];
    d.base = base; d.limit = limit; d.valid = true;
}

uint16_t Machine::alloc_ldt(uint32_t base, uint32_t limit) {
    uint16_t sel = next_ldt;
    next_ldt += 8;
    set_desc(sel, base, limit);
    return sel;
}

}  // namespace towns

using towns::g_towns;

uint32_t x86_mem_read(uint32_t a, int n) { return g_towns->mem_read(a, n); }
void x86_mem_write(uint32_t a, int n, uint32_t v) { g_towns->mem_write(a, n, v); }

void x86_load_seg(X86Cpu& c, int n, uint32_t sel) {
    sel &= 0xFFFF;
    c.sreg[n] = (uint16_t)sel;
    if ((sel & 0xFFFC) == 0) { c.sbase[n] = 0; return; }
    const towns::Descriptor& d = g_towns->desc[(sel & 0x1FFC) >> 2];
    if (!d.valid) towns::unknown("load of selector %04x (no descriptor)", sel);
    c.sbase[n] = d.base;
}
