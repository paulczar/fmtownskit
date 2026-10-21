// fmtownskit runtime -- what every build of recompiled 386 code links: the page table of
// linear memory, the modules and their lookup.
#include "x86.h"

uint8_t* g_x86_page[1u << 20];

uint32_t x86_crc32(const uint8_t* p, size_t n, uint32_t crc) {
    static uint32_t table[256];
    if (!table[1]) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
    }
    crc = ~crc;
    for (size_t i = 0; i < n; ++i) crc = table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

const X86Module* x86_module(const char* name) {
    for (int i = 0; i < g_x86_nmodules; ++i)
        if (!std::strcmp(g_x86_modules[i]->name, name)) return g_x86_modules[i];
    return nullptr;
}

X86Func x86_lookup(const X86Module* m, uint32_t addr) {
    uint32_t lo = 0, hi = m->nfuncs;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        if (m->funcs[mid].addr < addr) lo = mid + 1;
        else hi = mid;
    }
    return lo < m->nfuncs && m->funcs[lo].addr == addr ? m->funcs[lo].fn : nullptr;
}

static const X86Module* g_parked[16];
static int g_nparked = 0;

void x86_park(const X86Module* m, bool parked) {
    int k = 0;
    while (k < g_nparked && g_parked[k] != m) ++k;
    if (parked && k == g_nparked && g_nparked < 16) g_parked[g_nparked++] = m;
    if (!parked && k < g_nparked) g_parked[k] = g_parked[--g_nparked];
}

X86Func x86_lookup_cs(uint16_t cs, uint32_t addr) {
    for (int i = 0; i < g_x86_nmodules; ++i) {
        const X86Module* m = g_x86_modules[i];
        bool parked = false;
        for (int k = 0; k < g_nparked; ++k) parked |= g_parked[k] == m;
        if (parked) continue;
        if (m->cs == cs || m->cs == 0)
            if (X86Func f = x86_lookup(m, addr)) return f;
    }
    return nullptr;
}

// xorshift32 from a seed of the page number: the bytes of a page nobody wrote, the same
// as fmtownskit.recomp.selftest.fill.
void x86_fill_page(uint32_t page, uint8_t* out) {
    uint32_t x = page * 0x9E3779B1u + 0x7F4A7C15u;
    if (!x) x = 1;
    for (int i = 0; i < 1024; ++i) {
        x ^= x << 13; x ^= x >> 17; x ^= x << 5;
        std::memcpy(out + 4 * i, &x, 4);
    }
}
