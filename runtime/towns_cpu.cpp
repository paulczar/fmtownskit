// fmtownskit runtime -- x86.h's services for a Towns program: far transfers between the
// program and TBIOS, calls through a register, interrupts, and the safe points where time
// passes and the program's interrupt handlers run.
//
// A far call is a C++ call: CS and the return EIP pushed as the 386 pushes them, the callee
// found by its code selector (each recompiled module is keyed by one), `retf` pops them and
// returns to the caller's C++ frame, which checks the EIP it came back to. An interrupt is
// taken at a safe point (x86_poll): EFLAGS, CS and the resume address pushed, IF cleared, the
// handler called; its `iretd` comes back to the poll.
#include "towns.h"
#include <algorithm>
#include <cstdlib>

using towns::g_towns;

[[noreturn]] void x86_fault(X86Cpu& c, uint32_t site, const char* why) {
    std::fprintf(stderr, "fault at %04x:%08x: %s (eip %08x, frame %llu)\n", c.sreg[1], site, why, c.eip,
                 (unsigned long long)(g_towns ? g_towns->frame : 0));
    std::fprintf(stderr, "  eax %08x ecx %08x edx %08x ebx %08x esp %08x ebp %08x esi %08x edi %08x fl %08x\n",
                 c.r[0], c.r[1], c.r[2], c.r[3], c.r[4], c.r[5], c.r[6], c.r[7], c.fl);
    std::fprintf(stderr, "  es %04x cs %04x ss %04x ds %04x fs %04x gs %04x\n",
                 c.sreg[0], c.sreg[1], c.sreg[2], c.sreg[3], c.sreg[4], c.sreg[5]);
    std::fflush(stderr);
    std::exit(3);
}

static X86Func find(X86Cpu& c, uint16_t cs, uint32_t off, uint32_t site) {
    X86Func f = x86_lookup_cs(cs & 0xFFFC, off);
    if (!f) f = x86_lookup_cs(cs, off);
    if (!f) {
        char why[96];
        std::snprintf(why, sizeof why, "no recompiled function at %04x:%08x", cs, off);
        x86_fault(c, site, why);
    }
    return f;
}

void x86_far(X86Cpu& c, uint32_t sel, uint32_t off, bool call, int n, uint32_t next) {
    sel &= 0xFFFF;
    if (n == 2) off &= 0xFFFF;
    if (call) {
        if (n == 4) { push32(c, c.sreg[1]); push32(c, next); }
        else { push16(c, c.sreg[1]); push16(c, next); }
    }
    uint16_t old = c.sreg[1];
    x86_load_seg(c, 1, sel);
    X86Func f = find(c, (uint16_t)sel, off, next);
    f(c);
    if (!call) return;
    // a far call returns through retf, which set CS and EIP; the caller checks EIP
    (void)old;
}

void x86_retf(X86Cpu& c, int n, uint32_t extra) {
    uint32_t eip, cs;
    if (n == 4) { eip = pop32(c); cs = pop32(c) & 0xFFFF; }
    else { eip = pop16(c); cs = pop16(c); }
    c.r[4] += extra;
    x86_load_seg(c, 1, cs);
    c.eip = eip;
}

void x86_iret(X86Cpu& c, int n) {
    uint32_t eip, cs, fl;
    if (n == 4) { eip = pop32(c); cs = pop32(c) & 0xFFFF; fl = pop32(c); }
    else { eip = pop16(c); cs = pop16(c); fl = (c.fl & 0xFFFF0000u) | pop16(c); }
    x86_load_seg(c, 1, cs);
    c.fl = (fl & 0x00257FD5u) | 2u;
    c.eip = eip;
}

void x86_call(X86Cpu& c, uint32_t target) {
    X86Func f = find(c, c.sreg[1], target, target);
    f(c);
}

void x86_jump(X86Cpu& c, uint32_t target) {
    X86Func f = find(c, c.sreg[1], target, target);
    f(c);
}

void x86_system(X86Cpu& c, uint32_t site, const char* what) {
    char why[96];
    std::snprintf(why, sizeof why, "a system instruction (%s)", what);
    x86_fault(c, site, why);
}

bool x86_lsl(X86Cpu&, uint32_t sel, uint32_t& limit) {
    sel &= 0xFFFF;
    if (sel > 0x1FFF || !(sel & ~3u)) return false;
    const towns::Descriptor& d = g_towns->desc[(sel & 0x1FFC) >> 2];
    if (!d.valid) return false;
    limit = d.limit;
    return true;
}

uint32_t x86_in(X86Cpu& c, uint32_t port, int n) {
    g_towns->advance(g_towns->machine_time(c.clocks));
    if (g_towns->count_access) { ++g_towns->access.io_r; if ((port & ~3u) == 0x4D0) ++g_towns->access.pad_r; }
    return g_towns->in(port, n);
}

void x86_out(X86Cpu& c, uint32_t port, int n, uint32_t v) {
    g_towns->advance(g_towns->machine_time(c.clocks));
    if (g_towns->count_access) { ++g_towns->access.io_w; if (port == 0x6C) ++g_towns->access.wait_w; }
    g_towns->out(port, n, v);
}

// Take an interrupt now: the handler runs as a call, its iretd returns here.
static void take(X86Cpu& c, int vec) {
    const towns::IntVector& iv = g_towns->pm_vector[vec];
    uint32_t resume = c.eip;
    push32(c, c.fl);
    push32(c, c.sreg[1]);
    push32(c, resume);
    c.fl &= ~(X86_IF | X86_TF);
    x86_load_seg(c, 1, iv.sel);
    X86Func f = find(c, iv.sel, iv.off, resume);
    f(c);
    if (c.eip != resume) x86_fault(c, resume, "an interrupt handler returned elsewhere");
}

void x86_poll(X86Cpu& c) {
    towns::Machine& m = *g_towns;
    m.advance(m.machine_time(c.clocks));
    while (c.fl & X86_IF) {
        int vec = m.pending_irq();
        if (vec < 0) break;
        take(c, vec);
        m.advance(m.machine_time(c.clocks));
    }
    while (c.fl & X86_IF) {             // the timer BIOS's callbacks (int 97h), as its interrupt calls them
        int n = m.t97_due();
        if (n < 0) break;
        const towns::Machine::Timer97& t = m.t97[n];
        uint32_t r[8], fl = c.fl, resume = c.eip;
        uint16_t s[6];
        std::copy_n(c.r, 8, r);
        std::copy_n(c.sreg, 6, s);
        push32(c, c.sreg[1]);
        push32(c, resume);
        c.fl &= ~(X86_IF | X86_TF);
        x86_load_seg(c, 1, t.sel);
        X86Func f = find(c, t.sel, t.off, resume);
        f(c);
        if (c.eip != resume) x86_fault(c, resume, "a timer BIOS callback returned elsewhere");
        std::copy_n(r, 8, c.r);
        for (int k = 0; k < 6; ++k) x86_load_seg(c, k, s[k]);
        c.fl = fl;
        m.advance(m.machine_time(c.clocks));
    }
    uint64_t next = m.next_event();
    int64_t left = (int64_t)(m.cpu_time(next) - c.clocks);
    c.budget = (int32_t)(left < 64 ? 64 : left > (1 << 30) ? (1 << 30) : left);
}

void x86_int(X86Cpu& c, int n, uint32_t next) {
    towns::Machine& m = *g_towns;
    m.advance(m.machine_time(c.clocks));
    c.eip = next;
    switch (n) {
    case 0x21: m.int21(c); break;
    case 0x93: {
        uint32_t eax = c.r[0];
        m.int93(c, c.r[0], eax, c.sbase[3] + c.r[7]);
        c.r[0] = eax;
        break;
    }
    case 0x90: m.int90(c); break;
    case 0x8E: m.int8e(c); break;
    case 0x97: m.int97(c); break;
    case 0xFD: m.intfd(c); break;
    default: {
        char why[64];
        std::snprintf(why, sizeof why, "int %02Xh, AX=%04X: no service", n, c.r[0] & 0xFFFF);
        x86_fault(c, next, why);
    }
    }
    c.eip = next;
}
