// fmtownskit runtime -- the 386 as recompiled code sees it.
//
// Recompiled functions have the signature `void f_XXXXXXXX(X86Cpu& c)`, one
// namespace per program (`python -m fmtownskit.recomp`), keyed by their offset in
// the program's code segment. They touch guest state only through this header:
// the registers, EFLAGS and segment registers in X86Cpu; linear memory through
// rd8..wr32 (a table of 4 KB pages: a page that is not host memory, and an access
// that crosses pages, go to the runtime's x86_mem_*); and the out-of-line services
// the runtime provides (segment loads, interrupts, ports, far transfers, calls
// through a register, safe points).
//
// Semantics are fmtownskit/i386emu.py's, instruction for instruction: the
// self-test holds the two together, and i386emu is held against unicorn.
#pragma once
#include <cstdint>
#include <cstring>

#define X86_UNLIKELY(x) __builtin_expect(!!(x), 0)

enum : uint32_t {
    X86_CF = 0x1, X86_PF = 0x4, X86_AF = 0x10, X86_ZF = 0x40, X86_SF = 0x80,
    X86_TF = 0x100, X86_IF = 0x200, X86_DF = 0x400, X86_OF = 0x800,
    X86_ARITH = X86_CF | X86_PF | X86_AF | X86_ZF | X86_SF | X86_OF,
};

struct X86Cpu {
    uint32_t r[8];          // eax ecx edx ebx esp ebp esi edi
    uint32_t eip;           // where the last return went (the caller checks it); the resume point at a poll
    uint32_t fl;            // EFLAGS
    uint16_t sreg[6];       // es cs ss ds fs gs
    uint32_t sbase[6];
    int32_t  budget;        // 386 clocks left before x86_poll
    uint64_t clocks;        // clocks run, all told
};

typedef void (*X86Func)(X86Cpu&);
struct X86FuncEntry { uint32_t addr; X86Func fn; };
struct X86Module {
    const char* name;
    uint16_t cs;                        // the code selector the program runs under (0: any)
    uint32_t size, crc;                 // the image as the file holds it
    const X86FuncEntry* funcs;
    uint32_t nfuncs;
    uint32_t base;                      // the offset in its segment the image is loaded at
    const uint8_t* image;               // the image itself (for the self-test: build/ only)
};

// ---- memory: linear addresses, 4 KB pages ------------------------------------------------
extern uint8_t* g_x86_page[1u << 20];   // host memory of each linear page, or null
uint32_t x86_mem_read(uint32_t a, int n);         // a page with no host memory, or a crossing
void     x86_mem_write(uint32_t a, int n, uint32_t v);

static inline uint32_t rd8(uint32_t a) {
    uint8_t* p = g_x86_page[a >> 12];
    return X86_UNLIKELY(!p) ? x86_mem_read(a, 1) : p[a & 0xFFF];
}
static inline uint32_t rd16(uint32_t a) {
    uint8_t* p = g_x86_page[a >> 12];
    if (X86_UNLIKELY(!p || (a & 0xFFF) > 0xFFE)) return x86_mem_read(a, 2);
    uint16_t v; std::memcpy(&v, p + (a & 0xFFF), 2); return v;
}
static inline uint32_t rd32(uint32_t a) {
    uint8_t* p = g_x86_page[a >> 12];
    if (X86_UNLIKELY(!p || (a & 0xFFF) > 0xFFC)) return x86_mem_read(a, 4);
    uint32_t v; std::memcpy(&v, p + (a & 0xFFF), 4); return v;
}
static inline void wr8(uint32_t a, uint32_t v) {
    uint8_t* p = g_x86_page[a >> 12];
    if (X86_UNLIKELY(!p)) x86_mem_write(a, 1, v); else p[a & 0xFFF] = (uint8_t)v;
}
static inline void wr16(uint32_t a, uint32_t v) {
    uint8_t* p = g_x86_page[a >> 12];
    if (X86_UNLIKELY(!p || (a & 0xFFF) > 0xFFE)) { x86_mem_write(a, 2, v); return; }
    uint16_t w = (uint16_t)v; std::memcpy(p + (a & 0xFFF), &w, 2);
}
static inline void wr32(uint32_t a, uint32_t v) {
    uint8_t* p = g_x86_page[a >> 12];
    if (X86_UNLIKELY(!p || (a & 0xFFF) > 0xFFC)) { x86_mem_write(a, 4, v); return; }
    std::memcpy(p + (a & 0xFFF), &v, 4);
}
static inline uint32_t rdn(uint32_t a, int n) { return n == 1 ? rd8(a) : n == 2 ? rd16(a) : rd32(a); }
static inline void wrn(uint32_t a, int n, uint32_t v) {
    if (n == 1) wr8(a, v); else if (n == 2) wr16(a, v); else wr32(a, v);
}

// ---- registers --------------------------------------------------------------------------
static inline uint32_t g8(const X86Cpu& c, int n) { return n >= 4 ? (c.r[n & 3] >> 8) & 0xFF : c.r[n] & 0xFF; }
static inline void s8(X86Cpu& c, int n, uint32_t v) {
    if (n >= 4) c.r[n & 3] = (c.r[n & 3] & 0xFFFF00FFu) | ((v & 0xFF) << 8);
    else c.r[n] = (c.r[n] & 0xFFFFFF00u) | (v & 0xFF);
}
static inline void s16(X86Cpu& c, int n, uint32_t v) { c.r[n] = (c.r[n] & 0xFFFF0000u) | (v & 0xFFFF); }
static inline uint32_t gr(const X86Cpu& c, int n, int size) {
    return size == 4 ? c.r[n] : size == 2 ? c.r[n] & 0xFFFF : g8(c, n);
}
static inline void sr(X86Cpu& c, int n, int size, uint32_t v) {
    if (size == 4) c.r[n] = v; else if (size == 2) s16(c, n, v); else s8(c, n, v);
}

// ---- the stack (32-bit SS) --------------------------------------------------------------
static inline void push32(X86Cpu& c, uint32_t v) { c.r[4] -= 4; wr32(c.sbase[2] + c.r[4], v); }
static inline void push16(X86Cpu& c, uint32_t v) { c.r[4] -= 2; wr16(c.sbase[2] + c.r[4], v); }
static inline uint32_t pop32(X86Cpu& c) { uint32_t v = rd32(c.sbase[2] + c.r[4]); c.r[4] += 4; return v; }
static inline uint32_t pop16(X86Cpu& c) { uint32_t v = rd16(c.sbase[2] + c.r[4]); c.r[4] += 2; return v; }

// ---- flags (x86_flags.cpp mirrors i386emu's CPU.alu, shift, ...) ---------------------------
extern const uint8_t g_x86_parity[256];
static inline uint32_t x86_mask(int n) { return n == 4 ? 0xFFFFFFFFu : n == 2 ? 0xFFFFu : 0xFFu; }
static inline uint32_t x86_sign(int n) { return n == 4 ? 0x80000000u : n == 2 ? 0x8000u : 0x80u; }
static inline uint32_t x86_szp(uint32_t res, int n) {
    uint32_t f = g_x86_parity[res & 0xFF];
    if ((res & x86_mask(n)) == 0) f |= X86_ZF;
    if (res & x86_sign(n)) f |= X86_SF;
    return f;
}
static inline void x86_arith(X86Cpu& c, uint32_t f) { c.fl = (c.fl & ~X86_ARITH) | f; }
static inline uint32_t x86_add(X86Cpu& c, uint32_t a, uint32_t b, uint32_t cin, int n) {
    uint64_t full = (uint64_t)a + b + cin;
    uint32_t m = x86_mask(n), res = (uint32_t)full & m;
    uint32_t f = x86_szp(res, n) | (full > m ? X86_CF : 0);
    if ((~(a ^ b) & (a ^ res)) & x86_sign(n)) f |= X86_OF;
    if ((a ^ b ^ res) & 0x10) f |= X86_AF;
    x86_arith(c, f);
    return res;
}
static inline uint32_t x86_sub(X86Cpu& c, uint32_t a, uint32_t b, uint32_t bin, int n) {
    int64_t full = (int64_t)a - (int64_t)b - (int64_t)bin;
    uint32_t res = (uint32_t)full & x86_mask(n);
    uint32_t f = x86_szp(res, n) | (full < 0 ? X86_CF : 0);
    if (((a ^ b) & (a ^ res)) & x86_sign(n)) f |= X86_OF;
    if ((a ^ b ^ res) & 0x10) f |= X86_AF;
    x86_arith(c, f);
    return res;
}
static inline uint32_t x86_logic(X86Cpu& c, uint32_t res, int n) {
    res &= x86_mask(n);
    x86_arith(c, x86_szp(res, n));
    return res;
}
// inc/dec keep CF
static inline uint32_t x86_inc(X86Cpu& c, uint32_t a, int n) {
    uint32_t cf = c.fl & X86_CF; uint32_t r = x86_add(c, a, 1, 0, n); c.fl = (c.fl & ~X86_CF) | cf; return r;
}
static inline uint32_t x86_dec(X86Cpu& c, uint32_t a, int n) {
    uint32_t cf = c.fl & X86_CF; uint32_t r = x86_sub(c, a, 1, 0, n); c.fl = (c.fl & ~X86_CF) | cf; return r;
}
static inline bool x86_cc(const X86Cpu& c, int cc) {
    uint32_t f = c.fl;
    bool s = f & X86_SF, o = f & X86_OF;
    switch (cc) {
    case 0: return f & X86_OF;  case 1: return !(f & X86_OF);
    case 2: return f & X86_CF;  case 3: return !(f & X86_CF);
    case 4: return f & X86_ZF;  case 5: return !(f & X86_ZF);
    case 6: return f & (X86_CF | X86_ZF); case 7: return !(f & (X86_CF | X86_ZF));
    case 8: return s;  case 9: return !s;
    case 10: return f & X86_PF; case 11: return !(f & X86_PF);
    case 12: return s != o; case 13: return s == o;
    case 14: return (f & X86_ZF) || s != o; default: return !(f & X86_ZF) && s == o;
    }
}
// the rest, out of line (x86_flags.cpp)
uint32_t x86_shift(X86Cpu& c, int op, uint32_t v, uint32_t cnt, int n, bool& write);   // op: rol ror rcl rcr shl shr sal sar
uint32_t x86_double(X86Cpu& c, bool left, uint32_t d, uint32_t s, uint32_t cnt, int n, bool& write);
void     x86_mul(X86Cpu& c, uint32_t v, int n);
void     x86_imul1(X86Cpu& c, uint32_t v, int n);
uint32_t x86_imul2(X86Cpu& c, uint32_t a, uint32_t b, int n);
void     x86_div(X86Cpu& c, uint32_t v, int n, uint32_t site);
void     x86_idiv(X86Cpu& c, uint32_t v, int n, uint32_t site);
uint32_t x86_neg(X86Cpu& c, uint32_t a, int n);
void     x86_bitop(X86Cpu& c, int how, uint32_t lin, bool mem_reg_offset, uint32_t bit, int n,
                   uint32_t* reg, int regn);
uint32_t x86_bsf(X86Cpu& c, uint32_t v, uint32_t old);
uint32_t x86_bsr(X86Cpu& c, uint32_t v, uint32_t old);
void     x86_bcd(X86Cpu& c, int op, uint32_t imm, uint32_t site);    // daa das aaa aas aam aad
void     x86_string(X86Cpu& c, int op, int n, int rep, int asize, int srcseg);  // movs cmps stos lods scas ins outs
void     x86_popfd(X86Cpu& c, int n);
void     x86_enter(X86Cpu& c, uint32_t size, uint32_t level, int n);

// ---- the machine: provided by the runtime (or the self-test's stub) ---------------------------
void     x86_load_seg(X86Cpu& c, int n, uint32_t sel);
void     x86_int(X86Cpu& c, int n, uint32_t next);      // int n; next: the address after it
uint32_t x86_in(X86Cpu& c, uint32_t port, int n);
void     x86_out(X86Cpu& c, uint32_t port, int n, uint32_t v);
void     x86_far(X86Cpu& c, uint32_t sel, uint32_t off, bool call, int n, uint32_t next);
void     x86_retf(X86Cpu& c, int n, uint32_t extra);
void     x86_iret(X86Cpu& c, int n);
void     x86_system(X86Cpu& c, uint32_t site, const char* what);      // lgdt, mov crN, hlt, ...
bool     x86_lsl(X86Cpu& c, uint32_t sel, uint32_t& limit);           // false: no such selector
void     x86_call(X86Cpu& c, uint32_t target);           // a call through a register or memory
void     x86_jump(X86Cpu& c, uint32_t target);           // a jump the function does not hold
void     x86_poll(X86Cpu& c);                            // the budget is spent: time, interrupts
[[noreturn]] void x86_fault(X86Cpu& c, uint32_t site, const char* why);

// Each instruction's address, for a build that traces (-DX86_TRACE_ON): the runtime's x86_trace
// sees the state before the instruction runs.
#ifdef X86_TRACE_ON
void x86_trace(X86Cpu& c, uint32_t at);
#define X86_AT(c, a) x86_trace(c, a)
#else
#define X86_AT(c, a) ((void)0)
#endif

// The clocks the 386 would take: each block pays for its instructions at its start.
#define X86_TICK(c, n) ((c).budget -= (int32_t)(n), (c).clocks += (n))
// A safe point (a backward jump, a call): the budget spent, the runtime's turn; `at`
// is where execution resumes, the address an interrupt returns to.
#define X86_POLL(c, at) do { if (X86_UNLIKELY((c).budget < 0)) { (c).eip = (at); x86_poll(c); } } while (0)
// After a call: the callee returned here, or further out (a return past its caller's frame
// unwinds the C++ calls until a frame expects it).
#define X86_RET(c, a) do { if (X86_UNLIKELY((c).eip != (a))) return; } while (0)

// ---- modules -------------------------------------------------------------------------------
extern const X86Module* const g_x86_modules[];
extern const int g_x86_nmodules;
const X86Module* x86_module(const char* name);
X86Func x86_lookup(const X86Module* m, uint32_t addr);
X86Func x86_lookup_cs(uint16_t cs, uint32_t addr);       // the function at cs:addr, or null
// Several programs under one code selector, run one after another (ABEL's OPNG, ABEL, ENDG under
// 0x0C, as RUN386 runs them): the ones not running are parked, and lookups by selector pass them.
void x86_park(const X86Module* m, bool parked);
uint32_t x86_crc32(const uint8_t* p, size_t n, uint32_t crc = 0);
// The self-test's fill of a page of memory never written: the same as i386emu's (selftest.fill).
void x86_fill_page(uint32_t page, uint8_t* out);
