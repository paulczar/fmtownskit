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
#include <cstdio>
#include <set>
#include <utility>

using towns::g_towns;

// The far-selector trace (x86.h). Zero is off. See x86_far for why this exists.
uint32_t g_trace_far_sel = 0;
uint32_t g_trace_far_all = 0;
uint32_t g_far_eax = 0;
uint32_t g_trace_fn_cs = 0, g_trace_fn_lo = 0, g_trace_fn_hi = 0;
uint32_t g_trace_fn_dump_off = 0, g_trace_fn_dump_len = 0;
const char* g_trace_fn_dump_path = nullptr;

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
    // PATCH, this repository: log every entry into one code-selector address range, deduped. This is how "does the
    // program reach this routine at all" becomes answerable in a port whose functions are C++ calls with no
    // instruction fetches to watch. Set from the port's U2_TRACE_FN=<cs>:<lo>:<hi>.
    if (g_trace_fn_lo < g_trace_fn_hi && (cs & 0xFFFC) == (g_trace_fn_cs & 0xFFFC) && off >= g_trace_fn_lo
        && off <= g_trace_fn_hi) {
        static long entries = 0;
        if (++entries <= (long)g_watch_max)
            std::fprintf(stderr, "fn        %04x:%08x from %08x f%llu eax %08x ebx %08x ecx %08x edx %08x esi %08x edi %08x ebp %08x es %08x ds %08x gs %08x\n",
                         cs, off, site, (unsigned long long)(g_towns ? g_towns->frame : 0), c.r[0], c.r[3], c.r[1], c.r[2],
                         c.r[6], c.r[7], c.r[5], c.sbase[0], c.sbase[3], c.sbase[5]);
        // Dump the routine's state at the same instant the machine's breakpoint fires, so a comparison of two samples
        // is a comparison of the same moment. Done once: a per-call dump is a file per iteration and no more truth.
        static bool dumped = false;
        if (!dumped && g_trace_fn_dump_path && g_trace_fn_dump_len && g_towns) {
            dumped = true;
            const uint32_t at = c.sbase[5] + c.r[5] + g_trace_fn_dump_off;
            std::vector<uint8_t> buf(g_trace_fn_dump_len);
            g_towns->read_block(at, buf.data(), buf.size());
            if (FILE* fp = std::fopen(g_trace_fn_dump_path, "wb")) {
                std::fwrite(buf.data(), 1, buf.size(), fp);
                std::fclose(fp);
                std::printf("fndump    %08x -> %s (%u bytes)\n", at, g_trace_fn_dump_path, g_trace_fn_dump_len);
            }
        }
    }
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
    // PATCH, this repository: name the callers of one far selector, once per (caller, target) pair. This is how the
    // program's use of a system layer becomes visible at all: the calls are not constant-selector `9a` instructions
    // and no far pointer to the layer is stored in the image, so the caller exists only at run time. Bounded because
    // a dedup set makes it one line per distinct call site rather than one per call.
    if (g_trace_far_sel && sel == (g_trace_far_sel & 0xFFFF)) {
        static std::set<std::pair<uint32_t, uint32_t>> seen;
        static long calls = 0;
        // `next` is the resume address in the caller, and c.sreg[1] is still the caller's segment here.
        std::pair<uint32_t, uint32_t> key{ ((uint32_t)c.sreg[1] << 16) | (next & 0xFFFF), off };
        // The registers of the call in flight, so a write inside the layer can be attributed to the request that
        // caused it. A dispatcher takes its function number as an argument, which means the number exists only
        // here: this is the only place it can be captured.
        g_far_eax = c.r[0];
        if (g_trace_far_all) {
            if (++calls <= (long)g_trace_far_all)
                std::fprintf(stderr, "call     f%llu %04x:%08x -> %04x:%08x eax %08x ebx %08x ecx %08x edx %08x\n",
                             (unsigned long long)(g_towns ? g_towns->frame : 0), c.sreg[1], next, sel, off, c.r[0], c.r[3],
                             c.r[1], c.r[2]);
            else if (calls == (long)g_trace_far_all + 1)
                std::fprintf(stderr, "call     capped at %u calls\n", g_trace_far_all);
        }
        if (seen.insert(key).second)
            std::fprintf(stderr, "far      %04x:%08x -> %04x:%08x%s eax %08x ebx %08x ecx %08x edx %08x\n", c.sreg[1], next,
                         sel, off, call ? "" : " (jump)", c.r[0], c.r[3], c.r[1], c.r[2]);
    }
    if (call) {
        if (n == 4) { push32(c, c.sreg[1]); push32(c, next); }
        else { push16(c, c.sreg[1]); push16(c, next); }
    }
    uint16_t old = c.sreg[1];
    x86_load_seg(c, 1, sel);
    X86Func f = find(c, (uint16_t)sel, off, next);
    f(c);
    // The return values are the other half of the contract: a program that asks the system layer for a cursor or a
    // metric and gets a different answer lays itself out differently, with no call missing and no fault raised.
    if (g_trace_far_sel && sel == (g_trace_far_sel & 0xFFFF) && g_trace_far_all) {
        static long rets = 0;
        if (++rets <= (long)g_trace_far_all)
            std::fprintf(stderr, "ret       %04x:%08x <- %04x eax %08x ebx %08x ecx %08x edx %08x f%llu\n", old, next, sel,
                         c.r[0], c.r[3], c.r[1], c.r[2], (unsigned long long)(g_towns ? g_towns->frame : 0));
    }
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
    // PATCH, this repository. This program patches its own `int` instruction, so the vector in the
    // recompiled module is not the vector the program executes.
    //
    // The routine named "int86" at guest 0x1023f6 is:
    //
    //   1023fc  8a 45 08        mov al, [ebp+8]        the interrupt number is the first argument
    //   1023ff  a2 1b241000     mov [0x10241b], al     written into the next instruction
    //   102418  eb 00           jmp $+2
    //   10241a  cd 00           int 0                  which the loader left as CD 00
    //
    // Measured live, and it settles what is happening: at program entry the bytes at DS:0x10241A are
    // `CD 00`, and at the moment the call is made they are `CD 93`, which Tsugaru decodes as INT 93H.
    // The other eleven `CD 00` in the image are never patched because they are never executed, and a
    // scan for a `mov [mem], 0x93` finds nothing at all, because the store is the one above and its
    // value comes from a register.
    //
    // A recompiled module compiles the `int` from the bytes on disk, so it traps vector 0 forever. The
    // vector is therefore taken from the code immediately before the instruction, which is where the
    // program put it. `next` is the address after the two-byte instruction, so the vector is at
    // next - 1.
    if (n == 0) {
        int live = (int)rd8(c.sbase[1] + (next - 1));
        if (m.trace_int)
            std::fprintf(stderr, "  int 0 at %08x: vector byte %02x, cs base %08x, ds base %08x, ds byte %02x\n",
                         next - 2, (unsigned)live, (unsigned)c.sbase[1], (unsigned)c.sbase[3],
                         (unsigned)rd8(c.sbase[3] + 0x10241bu));
        if (live != 0) n = live;
    }
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
