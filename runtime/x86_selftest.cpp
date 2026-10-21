// fmtownskit -- the self-test: the interpreter's vectors replayed on the recompiled code.
//
//     selftest FILE...       each FILE written by `python -m fmtownskit.recomp.selftest`
//
// A vector names a module and a function, the registers and flags going in, and what the
// interpreter saw coming out: the registers, EIP (where the return went), the flags it
// defines, and every byte written. Memory nobody wrote is x86_fill_page's, on both sides.
// The machine's services (x86_int, ports, segment loads, far transfers) are not in a
// self-test: a vector that reaches one stops here, as in the interpreter.
#include "x86.h"
#include <cstdio>
#include <cstdlib>
#include <map>
#include <setjmp.h>
#include <string>
#include <vector>

static std::map<uint32_t, std::vector<uint8_t>> g_pages;
static std::map<uint32_t, uint8_t> g_written;
static jmp_buf g_stop;
static std::string g_why;

static uint8_t* page(uint32_t p) {
    auto it = g_pages.find(p);
    if (it == g_pages.end()) {
        std::vector<uint8_t> v(4096);
        x86_fill_page(p, v.data());
        it = g_pages.emplace(p, std::move(v)).first;
    }
    return it->second.data();
}

// The self-test leaves the page table empty, so every access comes here: one byte at a
// time, every write remembered.
uint32_t x86_mem_read(uint32_t a, int n) {
    uint32_t v = 0;
    for (int k = 0; k < n; ++k) {
        uint32_t b = a + k;
        v |= (uint32_t)page(b >> 12)[b & 0xFFF] << (8 * k);
    }
    return v;
}
void x86_mem_write(uint32_t a, int n, uint32_t v) {
    for (int k = 0; k < n; ++k) {
        uint32_t b = a + k;
        page(b >> 12)[b & 0xFFF] = (uint8_t)(v >> (8 * k));
        g_written[b] = (uint8_t)(v >> (8 * k));
    }
}

[[noreturn]] static void stop(const char* why) { g_why = why; longjmp(g_stop, 1); }
static const X86Module* g_cur;
// The program's own selectors (CS, CS+8) at base 0, where its image is; any other at sel << 16,
// so that a write through it cannot land on the code (as fmtownskit.recomp.selftest.Machine).
void x86_load_seg(X86Cpu& c, int n, uint32_t sel) {
    sel &= 0xFFFF;
    c.sreg[n] = (uint16_t)sel;
    uint32_t cs = g_cur ? g_cur->cs & ~3u : 0;
    c.sbase[n] = ((sel & ~3u) == cs || (sel & ~3u) == ((cs + 8) & ~3u)) ? 0 : sel << 16;
}
void x86_int(X86Cpu&, int, uint32_t) { stop("int"); }
uint32_t x86_in(X86Cpu&, uint32_t, int) { stop("in"); }
void x86_out(X86Cpu&, uint32_t, int, uint32_t) { stop("out"); }
void x86_far(X86Cpu&, uint32_t, uint32_t, bool, int, uint32_t) { stop("far transfer"); }
void x86_retf(X86Cpu&, int, uint32_t) { stop("retf"); }
void x86_iret(X86Cpu&, int) { stop("iret"); }
void x86_system(X86Cpu&, uint32_t, const char* what) { stop(what); }
bool x86_lsl(X86Cpu&, uint32_t, uint32_t&) { stop("lsl"); }
void x86_poll(X86Cpu& c) { c.budget = 1 << 30; }
[[noreturn]] void x86_fault(X86Cpu&, uint32_t, const char* why) { stop(why); }

#ifdef X86_TRACE_ON
void x86_trace(X86Cpu& c, uint32_t at) {
    std::printf("T %x", at);
    for (int k = 0; k < 8; ++k) std::printf(" %x", c.r[k]);
    std::printf(" %x\n", c.fl & 0xFFF);
}
#endif

void x86_call(X86Cpu& c, uint32_t target) {
    X86Func f = x86_lookup(g_cur, target);
    if (!f) stop("call to a non-entry");
    f(c);
}
void x86_jump(X86Cpu& c, uint32_t target) {
    X86Func f = x86_lookup(g_cur, target);
    if (!f) stop("jump to a non-entry");
    f(c);
}

int main(int argc, char** argv) {
    long total = 0, failed = 0, stopped = 0;
    for (int ai = 1; ai < argc; ++ai) {
        FILE* fp = std::fopen(argv[ai], "r");
        if (!fp) { std::fprintf(stderr, "%s: cannot open\n", argv[ai]); return 2; }
        char mod[64];
        unsigned addr, in[10], out[10], defined, nw;
        // V module addr  eax..edi eflags  ->  eax..edi eip eflags definedmask  nwritten  (a v)*
        while (std::fscanf(fp, " V %63s %x", mod, &addr) == 2) {
            for (int k = 0; k < 9; ++k) std::fscanf(fp, " %x", &in[k]);
            for (int k = 0; k < 10; ++k) std::fscanf(fp, " %x", &out[k]);
            std::fscanf(fp, " %x %u", &defined, &nw);
            std::vector<std::pair<uint32_t, uint8_t>> want(nw);
            for (unsigned k = 0; k < nw; ++k) {
                unsigned a, v;
                std::fscanf(fp, " %x:%x", &a, &v);
                want[k] = {a, (uint8_t)v};
            }
            ++total;
            g_cur = x86_module(mod);
            X86Func f = g_cur ? x86_lookup(g_cur, addr) : nullptr;
            if (!f) { std::printf("%s %08x: no such function\n", mod, addr); ++failed; continue; }
            g_pages.clear();
            g_written.clear();
            for (uint32_t k = 0; k < g_cur->size;) {            // the image, at its offset (bases are 0 here)
                uint32_t b = g_cur->base + k, n = 4096 - (b & 0xFFF);
                if (n > g_cur->size - k) n = g_cur->size - k;
                std::memcpy(page(b >> 12) + (b & 0xFFF), g_cur->image + k, n);
                k += n;
            }
            X86Cpu c{};
            for (int k = 0; k < 8; ++k) c.r[k] = in[k];
            c.fl = in[8];
            c.budget = 1 << 30;
            uint32_t ret = 0xFFFFFFF0u;       // the vector's own return address, pushed
            push32(c, ret);
            g_written.clear();
            if (setjmp(g_stop)) {
                ++stopped;
                continue;
            }
            f(c);
            bool ok = c.eip == out[8];
            for (int k = 0; k < 8; ++k) ok = ok && c.r[k] == out[k];
            ok = ok && ((c.fl ^ out[9]) & defined & 0xFFF) == 0;
            std::map<uint32_t, uint8_t> w;
            for (auto& p : want) w[p.first] = p.second;
            // a byte written with the value it already had counts as either side's
            auto orig = [&](uint32_t a) -> uint8_t {
                if (a - g_cur->base < g_cur->size) return g_cur->image[a - g_cur->base];
                uint8_t pg[4096]; x86_fill_page(a >> 12, pg); return pg[a & 0xFFF];
            };
            for (auto& p : g_written) if (!w.count(p.first) && orig(p.first) != p.second) ok = false;
            for (auto& p : w) {
                auto it = g_written.find(p.first);
                if ((it != g_written.end() ? it->second : orig(p.first)) != p.second) ok = false;
            }
            if (!ok) {
                if (++failed <= 30) {
                    std::printf("%s %08x: differs:", mod, addr);
                    for (int k = 0; k < 8; ++k) if (c.r[k] != out[k]) std::printf(" r%d %08x/%08x", k, c.r[k], out[k]);
                    if (c.eip != out[8]) std::printf(" eip %08x/%08x", c.eip, out[8]);
                    if ((c.fl ^ out[9]) & defined & 0xFFF) std::printf(" flags %03x/%03x", c.fl & 0xFFF, out[9] & 0xFFF);
                    std::printf("\n");
                }
            }
        }
        std::fclose(fp);
    }
    std::printf("%ld vectors, %ld failures, %ld stopped at the machine\n", total, failed, stopped);
    return failed ? 1 : 0;
}
