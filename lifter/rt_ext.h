/* [Recomps#1] Runtime helpers for the instructions our lifter copy adds on top of ssbu-recomp's recomp.py.
 * Include after runtime.h and neon.h. Element-by-element, correctness first (same style as neon.h).
 *
 * Requirements on the runtime:
 *   - Ctx needs one more field after ex_val:   uint64_t ex_val2;   (high half seen by the last LDXP of two X regs)
 *   - stx128 uses a 16-byte compare-and-swap: compile with clang -mcx16 (x86-64 CMPXCHG16B).
 * Saturating instructions do not set FPSR.QC (nothing in the lifted code reads it so far). */
#ifndef RECOMP_RT_EXT_H
#define RECOMP_RT_EXT_H

/* STXP / STLXP of two X registers: store only if the 16 bytes still hold what the matching LDXP read. 0 = success. */
static inline uint32_t stx128(Ctx *c, uint64_t a, uint64_t lo, uint64_t hi) {
    if (c->ex_addr != a) { stx_diag(c, a, 0, 0, lo, -128); return 1; }
    c->ex_addr = ~0ull;
    R1M(a, 16);
    unsigned __int128 expected = ((unsigned __int128)c->ex_val2 << 64) | c->ex_val;
    unsigned __int128 desired = ((unsigned __int128)hi << 64) | lo;
    if (__atomic_compare_exchange_n((unsigned __int128 *)(uintptr_t)a, &expected, desired, 0,
                                    __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) { c->ex_fail = 0; return 0; }
    stx_diag(c, a, c->ex_val, (uint64_t)expected, lo, 128);
    return 1;
}

static inline uint64_t sat_s(int64_t v, int es) {          /* clamp to a signed element of size 8 << es bits */
    int64_t hi = (int64_t)((1ull << ((8 << es) - 1)) - 1), lo = -hi - 1;
    return (uint64_t)(v > hi ? hi : v < lo ? lo : v) & (es == 3 ? ~0ull : (1ull << (8 << es)) - 1);
}
static inline uint64_t sat_u(uint64_t v, int es) {
    uint64_t hi = es == 3 ? ~0ull : (1ull << (8 << es)) - 1;
    return v > hi ? hi : v;
}

/* SMULL/UMULL (op 0), SMLAL/UMLAL (1), SMLSL/UMLSL (2), "2" forms (q = 1) read the upper halves.
 * es = source element size (0..2); the destination elements are twice as wide and fill all 128 bits. */
static inline void v_long3(V128 *d, const V128 *n, const V128 *m, int op, int es, int u, int q) {
    V128 r = *d;
    int cnt = 8 >> es;
    for (int i = 0; i < cnt; i++) {
        uint64_t a = vget(n, es, i + q * cnt), b = vget(m, es, i + q * cnt);
        uint64_t p = u ? a * b : (uint64_t)(vsext(a, es) * vsext(b, es));
        uint64_t acc = op ? vget(d, es + 1, i) : 0;
        vset(&r, es + 1, i, op == 0 ? p : op == 1 ? acc + p : acc - p);
    }
    *d = r;
}

/* SQXTN/UQXTN{2}: narrow each 2x-wide element of n with saturation; q = 1 writes the upper half and keeps the lower. */
static inline void v_qxtn(V128 *d, const V128 *n, int es, int q, int u) {
    V128 r = *d;
    int cnt = 8 >> es;
    if (!q) r.d[1] = 0;
    for (int i = 0; i < cnt; i++) {
        uint64_t x = vget(n, es + 1, i);
        vset(&r, es, i + q * cnt, u ? sat_u(x, es) : sat_s(vsext(x, es + 1), es));
    }
    *d = r;
}

/* SRSHR/URSHR: rounding shift right by sh (1..element bits); count elements (the rest of the register is zeroed). */
static inline void v_rshr(V128 *d, const V128 *n, int sh, int es, int u, int count) {
    V128 r; vzero(&r);
    for (int i = 0; i < count; i++) {
        uint64_t x = vget(n, es, i), y;
        if (u) y = (uint64_t)(((unsigned __int128)x + ((unsigned __int128)1 << (sh - 1))) >> sh);
        else y = (uint64_t)(int64_t)(((__int128)vsext(x, es) + ((__int128)1 << (sh - 1))) >> sh);
        vset(&r, es, i, y);
    }
    *d = r;
}

/* SQSHRN (round 0) / SQRSHRN (round 1){2}: signed 2x-wide source, shift right by sh, saturate to es; q as v_qxtn. */
static inline void v_sqshrn(V128 *d, const V128 *n, int sh, int es, int q, int round) {
    V128 r = *d;
    int cnt = 8 >> es;
    if (!q) r.d[1] = 0;
    for (int i = 0; i < cnt; i++) {
        __int128 v = vsext(vget(n, es + 1, i), es + 1);       /* 128-bit: rounding a 64-bit source cannot overflow */
        if (round) v += (__int128)1 << (sh - 1);
        v >>= sh;                                            /* now fits in es + 1 bits, so in int64 */
        vset(&r, es, i + q * cnt, sat_s((int64_t)v, es));
    }
    *d = r;
}

/* FCVTZS/FCVTZU (vector, fixed-point): x * 2^fbits, round toward zero, saturate; NaN gives 0. */
static inline void v_fcvtz_fix(V128 *d, const V128 *n, int fbits, int dbl, int u, int count) {
    V128 r; vzero(&r);
    double scale = ldexp(1.0, fbits);
    for (int i = 0; i < count; i++) {
        double x = (dbl ? n->df[i] : (double)n->f[i]) * scale;
        uint64_t y;
        if (x != x) y = 0;
        else if (dbl) {
            if (u) y = x <= 0.0 ? 0 : x >= 18446744073709551616.0 ? ~0ull : (uint64_t)x;
            else y = (uint64_t)(x >= 9223372036854775808.0 ? INT64_MAX : x < -9223372036854775808.0 ? INT64_MIN : (int64_t)x);
        } else {
            if (u) y = x <= 0.0 ? 0 : x >= 4294967296.0 ? 0xffffffffull : (uint64_t)x;
            else y = (uint32_t)(x >= 2147483648.0 ? INT32_MAX : x <= -2147483649.0 ? INT32_MIN : (int32_t)x);
        }
        vset(&r, dbl ? 3 : 2, i, y);
    }
    *d = r;
}

#endif
