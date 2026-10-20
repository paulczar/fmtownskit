// fmtownskit runtime -- the 386's instructions that are more than a line: shifts and
// rotates, multiply and divide, bit tests and scans, BCD, strings, POPF, ENTER.
// Each mirrors fmtownskit/i386emu.py's method of the same name; the self-test holds them
// together.
#include "x86.h"

const uint8_t g_x86_parity[256] = {
#define P(v) (uint8_t)((__builtin_popcount(v) & 1) ? 0 : X86_PF)
#define P4(v) P(v), P(v + 1), P(v + 2), P(v + 3)
#define P16(v) P4(v), P4(v + 4), P4(v + 8), P4(v + 12)
#define P64(v) P16(v), P16(v + 16), P16(v + 32), P16(v + 48)
    P64(0), P64(64), P64(128), P64(192)
#undef P64
#undef P16
#undef P4
#undef P
};

// op: 0 rol, 1 ror, 2 rcl, 3 rcr, 4 shl, 5 shr, 6 sal, 7 sar (the ModRM /reg order)
uint32_t x86_shift(X86Cpu& c, int op, uint32_t v, uint32_t cnt, int n, bool& write) {
    int bits = 8 * n;
    cnt &= 0x1F;
    write = false;
    if (cnt == 0) return v;
    write = true;
    uint32_t m = x86_mask(n), f = c.fl, res, cf, of;
    switch (op) {
    case 4: case 6: {
        uint64_t w = (uint64_t)v << cnt;
        res = (uint32_t)w & m;
        cf = (int)cnt <= bits ? (uint32_t)(v >> (bits - cnt)) & 1 : 0;
        if ((int)cnt == bits) cf = v & 1;
        of = ((res >> (bits - 1)) & 1) ^ cf;
        f = (f & ~X86_ARITH) | x86_szp(res, n) | (cf ? X86_CF : 0) | (of ? X86_OF : 0);
        break;
    }
    case 5:
        res = (int)cnt < bits ? v >> cnt : 0;
        cf = (int)cnt <= bits ? (v >> (cnt - 1)) & 1 : 0;
        of = (v >> (bits - 1)) & 1;
        f = (f & ~X86_ARITH) | x86_szp(res, n) | (cf ? X86_CF : 0) | (of ? X86_OF : 0);
        break;
    case 7: {
        int64_t sv = (v & x86_sign(n)) ? (int64_t)v - ((int64_t)1 << bits) : (int64_t)v;
        int k = (int)cnt < bits ? (int)cnt : bits;
        res = (uint32_t)(sv >> k) & m;
        cf = (uint32_t)(sv >> (k - 1)) & 1;
        f = (f & ~X86_ARITH) | x86_szp(res, n) | (cf ? X86_CF : 0);
        break;
    }
    case 0: case 1: {
        int k = (int)(cnt % bits);
        if (op == 0) res = k ? ((v << k) | (v >> (bits - k))) & m : v;
        else res = k ? ((v >> k) | (v << (bits - k))) & m : v;
        if (op == 0) { cf = res & 1; of = ((res >> (bits - 1)) & 1) ^ cf; }
        else { cf = (res >> (bits - 1)) & 1; of = cf ^ ((res >> (bits - 2)) & 1); }
        f = (f & ~(X86_CF | X86_OF)) | (cf ? X86_CF : 0) | (of ? X86_OF : 0);
        break;
    }
    default: {   // rcl, rcr: through CF, modulo bits + 1
        int w = bits + 1, k = (int)(cnt % (uint32_t)w);
        uint64_t x = (uint64_t)v | ((uint64_t)(f & X86_CF) << bits);
        uint64_t wm = ((uint64_t)1 << w) - 1;
        if (op == 2) {
            x = k ? ((x << k) | (x >> (w - k))) & wm : x;
            res = (uint32_t)x & m; cf = (uint32_t)(x >> bits) & 1;
            of = ((res >> (bits - 1)) & 1) ^ cf;
        } else {
            of = ((v >> (bits - 1)) & 1) ^ (f & X86_CF);
            x = k ? ((x >> k) | (x << (w - k))) & wm : x;
            res = (uint32_t)x & m; cf = (uint32_t)(x >> bits) & 1;
        }
        f = (f & ~(X86_CF | X86_OF)) | (cf ? X86_CF : 0) | (of ? X86_OF : 0);
        break;
    }
    }
    c.fl = f;
    return res;
}

uint32_t x86_double(X86Cpu& c, bool left, uint32_t d, uint32_t s, uint32_t cnt, int n, bool& write) {
    int bits = 8 * n;
    cnt &= 0x1F;
    write = false;
    if (cnt == 0) return d;
    write = true;
    uint32_t m = x86_mask(n), res, cf;
    if (left) {
        uint64_t x = ((uint64_t)d << bits) | s;
        res = (uint32_t)((x << cnt) >> bits) & m;
        cf = (int)cnt <= bits ? (d >> (bits - cnt)) & 1 : (uint32_t)(s >> (2 * bits - cnt)) & 1;
        if ((int)cnt == bits) cf = d & 1;
    } else {
        uint64_t x = ((uint64_t)s << bits) | d;
        res = (uint32_t)(x >> cnt) & m;
        cf = (uint32_t)(x >> (cnt - 1)) & 1;
    }
    uint32_t of = ((res ^ d) >> (bits - 1)) & 1;
    c.fl = (c.fl & ~X86_ARITH) | x86_szp(res, n) | (cf ? X86_CF : 0) | (of ? X86_OF : 0);
    return res;
}

static void wide(X86Cpu& c, uint64_t p, int n) {
    if (n == 1) s16(c, 0, (uint32_t)p);
    else { sr(c, 0, n, (uint32_t)p); sr(c, 2, n, (uint32_t)(p >> (8 * n))); }
}

void x86_mul(X86Cpu& c, uint32_t v, int n) {
    uint64_t p = (uint64_t)gr(c, 0, n) * v;
    wide(c, p, n);
    uint32_t hi = (uint32_t)(p >> (8 * n));
    x86_arith(c, (hi ? (X86_CF | X86_OF) : 0) | x86_szp((uint32_t)p & x86_mask(n), n));
}

static int64_t sx(uint32_t v, int n) {
    return (v & x86_sign(n)) ? (int64_t)v - ((int64_t)1 << (8 * n)) : (int64_t)v;
}

void x86_imul1(X86Cpu& c, uint32_t v, int n) {
    int64_t p = sx(gr(c, 0, n), n) * sx(v, n);
    wide(c, (uint64_t)p & (n == 4 ? ~0ull : ((1ull << (16 * n)) - 1)), n);
    bool fits = p >= -(int64_t)x86_sign(n) && p < (int64_t)x86_sign(n);
    x86_arith(c, (fits ? 0 : X86_CF | X86_OF) | x86_szp((uint32_t)p & x86_mask(n), n));
}

uint32_t x86_imul2(X86Cpu& c, uint32_t a, uint32_t b, int n) {
    int64_t p = sx(a & x86_mask(n), n) * sx(b & x86_mask(n), n);
    bool fits = p >= -(int64_t)x86_sign(n) && p < (int64_t)x86_sign(n);
    x86_arith(c, (fits ? 0 : X86_CF | X86_OF) | x86_szp((uint32_t)p & x86_mask(n), n));
    return (uint32_t)p & x86_mask(n);
}

static void divres(X86Cpu& c, uint32_t q, uint32_t r, int n) {
    if (n == 1) { s8(c, 0, q); s8(c, 4, r); }
    else { sr(c, 0, n, q); sr(c, 2, n, r); }
}

void x86_div(X86Cpu& c, uint32_t d, int n, uint32_t site) {
    if (d == 0) x86_fault(c, site, "#DE: divide by zero");
    uint64_t a = n == 1 ? gr(c, 0, 2) : ((uint64_t)gr(c, 2, n) << (8 * n)) | gr(c, 0, n);
    uint64_t q = a / d, r = a % d;
    if (q > x86_mask(n)) x86_fault(c, site, "#DE: quotient too large");
    divres(c, (uint32_t)q, (uint32_t)r, n);
}

void x86_idiv(X86Cpu& c, uint32_t dv, int n, uint32_t site) {
    int64_t d = sx(dv, n);
    if (d == 0) x86_fault(c, site, "#DE: divide by zero");
    int64_t a;
    if (n == 1) a = sx(gr(c, 0, 2), 2);
    else if (n == 2) a = sx(((uint32_t)gr(c, 2, 2) << 16) | gr(c, 0, 2), 4);
    else a = (int64_t)(((uint64_t)c.r[2] << 32) | c.r[0]);
    if (n == 4 && a == INT64_MIN && d == -1) x86_fault(c, site, "#DE: quotient too large");
    int64_t q = a / d, r = a - q * d;      // C++ truncates toward zero, as the 386
    if (q < -(int64_t)x86_sign(n) || q >= (int64_t)x86_sign(n)) x86_fault(c, site, "#DE: quotient too large");
    divres(c, (uint32_t)q & x86_mask(n), (uint32_t)r & x86_mask(n), n);
}

uint32_t x86_neg(X86Cpu& c, uint32_t a, int n) { return x86_sub(c, 0, a, 0, n); }

// how: 0 bt, 1 bts, 2 btr, 3 btc. A memory operand with a register bit offset reaches
// beyond the operand (signed); otherwise the offset is taken modulo the size.
void x86_bitop(X86Cpu& c, int how, uint32_t lin, bool mem_reg_offset, uint32_t bit, int n,
               uint32_t* reg, int regn) {
    uint32_t v;
    uint32_t a = lin;
    if (reg == nullptr && mem_reg_offset) {
        int64_t s = sx(bit & x86_mask(n), n);
        int64_t byteoff = (s >> 3) & ~(int64_t)(n - 1);
        a = lin + (uint32_t)byteoff;
        bit = (uint32_t)(s & (8 * n - 1));
        v = rdn(a, n);
    } else {
        bit &= 8 * n - 1;
        v = reg ? gr(c, regn, n) : rdn(a, n);
    }
    uint32_t cf = (v >> bit) & 1;
    c.fl = (c.fl & ~X86_CF) | cf;
    if (how == 0) return;
    uint32_t nv = how == 1 ? v | (1u << bit) : how == 2 ? v & ~(1u << bit) : v ^ (1u << bit);
    if (reg) sr(c, regn, n, nv); else wrn(a, n, nv);
}

uint32_t x86_bsf(X86Cpu& c, uint32_t v, uint32_t old) {
    if (v == 0) { c.fl |= X86_ZF; return old; }
    c.fl &= ~X86_ZF;
    return (uint32_t)__builtin_ctz(v);
}

uint32_t x86_bsr(X86Cpu& c, uint32_t v, uint32_t old) {
    if (v == 0) { c.fl |= X86_ZF; return old; }
    c.fl &= ~X86_ZF;
    return 31u - (uint32_t)__builtin_clz(v);
}

// op: 0 daa, 1 das, 2 aaa, 3 aas, 4 aam, 5 aad
void x86_bcd(X86Cpu& c, int op, uint32_t imm, uint32_t site) {
    uint32_t al = c.r[0] & 0xFF, cf = c.fl & X86_CF, af = c.fl & X86_AF, f = 0;
    switch (op) {
    case 0: {
        uint32_t old = al;
        if ((al & 0xF) > 9 || af) { al += 6; f |= X86_AF; if (al > 0xFF) f |= X86_CF; al &= 0xFF; }
        if (old > 0x99 || cf) { al = (al + 0x60) & 0xFF; f |= X86_CF; }
        s8(c, 0, al); x86_arith(c, f | x86_szp(al, 1));
        break;
    }
    case 1: {
        uint32_t old = al;
        if ((al & 0xF) > 9 || af) { f |= X86_AF | (al < 6 ? X86_CF : 0); al = (al - 6) & 0xFF; }
        if (old > 0x99 || cf) { al = (al - 0x60) & 0xFF; f |= X86_CF; }
        s8(c, 0, al); x86_arith(c, f | x86_szp(al, 1));
        break;
    }
    case 2:
        if ((c.r[0] & 0xF) > 9 || af) { s16(c, 0, (c.r[0] + 0x106) & 0xFFFF); f = X86_AF | X86_CF; }
        s8(c, 0, c.r[0] & 0xF); x86_arith(c, f);
        break;
    case 3:
        if ((c.r[0] & 0xF) > 9 || af) { s16(c, 0, (c.r[0] - 6) & 0xFFFF); s8(c, 4, (g8(c, 4) - 1) & 0xFF); f = X86_AF | X86_CF; }
        s8(c, 0, c.r[0] & 0xF); x86_arith(c, f);
        break;
    case 4:
        if (imm == 0) x86_fault(c, site, "#DE: aam 0");
        s8(c, 4, al / imm); s8(c, 0, al % imm); x86_arith(c, x86_szp(c.r[0] & 0xFF, 1));
        break;
    default: {
        uint32_t v = (g8(c, 0) + g8(c, 4) * imm) & 0xFF;
        s16(c, 0, v); x86_arith(c, x86_szp(v, 1));
        break;
    }
    }
}

// op: 0 movs, 1 cmps, 2 stos, 3 lods, 4 scas, 5 ins, 6 outs; rep: 0, 0xF3, 0xF2
void x86_string(X86Cpu& c, int op, int n, int rep, int asize, int srcseg) {
    bool a16 = asize == 2;
    int32_t step = (c.fl & X86_DF) ? -n : n;
    auto cx = [&]() { return a16 ? c.r[1] & 0xFFFF : c.r[1]; };
    auto adv = [&](int reg) { if (a16) s16(c, reg, c.r[reg] + step); else c.r[reg] += step; };
    auto at = [&](int reg, int seg) { return c.sbase[seg] + (a16 ? c.r[reg] & 0xFFFF : c.r[reg]); };
    for (;;) {
        if (rep && cx() == 0) return;
        switch (op) {
        case 0: wrn(at(7, 0), n, rdn(at(6, srcseg), n)); adv(6); adv(7); break;
        case 2: wrn(at(7, 0), n, gr(c, 0, n)); adv(7); break;
        case 3: sr(c, 0, n, rdn(at(6, srcseg), n)); adv(6); break;
        case 1: case 4: {
            uint32_t a = op == 1 ? rdn(at(6, srcseg), n) : gr(c, 0, n);
            uint32_t b = rdn(at(7, 0), n);
            x86_sub(c, a, b, 0, n);
            if (op == 1) adv(6);
            adv(7);
            break;
        }
        case 5: wrn(at(7, 0), n, x86_in(c, c.r[2] & 0xFFFF, n)); adv(7); break;
        default: x86_out(c, c.r[2] & 0xFFFF, n, rdn(at(6, srcseg), n)); adv(6); break;
        }
        if (!rep) return;
        if (a16) s16(c, 1, c.r[1] - 1); else c.r[1] -= 1;
        if (op == 1 || op == 4) {
            bool z = c.fl & X86_ZF;
            if ((rep == 0xF3 && !z) || (rep == 0xF2 && z)) return;
        }
    }
}

void x86_popfd(X86Cpu& c, int n) {
    if (n == 4) {
        uint32_t v = pop32(c), keep = 0x30000 | 0x3000;
        c.fl = (c.fl & keep) | (v & ~keep & 0x47FD5u) | 0x2;
    } else {
        uint32_t v = pop16(c), keep = 0x3000;
        c.fl = (c.fl & (0xFFFF0000u | keep)) | (v & ~keep & 0x7FD5u) | 0x2;
    }
}

void x86_enter(X86Cpu& c, uint32_t size, uint32_t level, int n) {
    level &= 31;
    if (n == 4) push32(c, c.r[5]); else push16(c, c.r[5] & 0xFFFF);
    uint32_t frame = c.r[4];
    for (uint32_t k = 1; k < level; ++k) {
        c.r[5] -= n;
        uint32_t v = rdn(c.sbase[2] + c.r[5], n);
        if (n == 4) push32(c, v); else push16(c, v);
    }
    if (level) { if (n == 4) push32(c, frame); else push16(c, frame); }
    sr(c, 5, n, frame);
    c.r[4] -= size;
}
