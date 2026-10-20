/* Bit-exact AArch64 behaviour for the few floating-point operations where
 * x86 differs: reciprocal estimates and the sign of generated NaNs.
 * The estimate algorithms follow the pseudocode in the Arm architecture manual. */
#ifndef RECOMP_FPEXACT_H
#define RECOMP_FPEXACT_H

/* x86 produces a negative quiet NaN for invalid operations, AArch64 a positive one. */
static inline float fnan32(float r) {
    uint32_t b;
    memcpy(&b, &r, 4);
    if (b == 0xffc00000u) { b = 0x7fc00000u; memcpy(&r, &b, 4); }
    return r;
}
static inline double fnan64(double r) {
    uint64_t b;
    memcpy(&b, &r, 8);
    if (b == 0xfff8000000000000ull) { b = 0x7ff8000000000000ull; memcpy(&r, &b, 8); }
    return r;
}

/* [Recomps#1] RecipEstimate and RecipSqrtEstimate of the Arm manual as tables (finding 134): entry
 * a - 256 (RecipEstimate) or a - 128 (RecipSqrtEstimate) holds the low 8 bits of the result, which is always 256..511.
 * Written as the manual's pseudocode, RecipSqrtEstimate searched b upwards from 512 one step at a time while
 * a' * (b + 1)^2 < 2^28 (a' = 2a + 1 below 256, else (a & ~1) * 2 + 2), up to 510 steps per lane, in every FRSQRTE the
 * game runs (B2's lift: 40 sites). RecipEstimate was
 * ((2^19 / (2a + 1)) + 1) / 2. The tables were made by those two functions and equal them for every input a. */
static const uint8_t fp_recpe_tab[256] = {
    0xff, 0xfd, 0xfb, 0xf9, 0xf7, 0xf5, 0xf3, 0xf1, 0xf0, 0xee, 0xec, 0xea, 0xe8, 0xe6, 0xe5, 0xe3, 0xe1, 0xdf, 0xdd, 0xdc, 0xda, 0xd8, 0xd7, 0xd5,
    0xd3, 0xd2, 0xd0, 0xce, 0xcd, 0xcb, 0xc9, 0xc8, 0xc6, 0xc5, 0xc3, 0xc2, 0xc0, 0xbf, 0xbd, 0xbc, 0xba, 0xb9, 0xb7, 0xb6, 0xb4, 0xb3, 0xb1, 0xb0,
    0xae, 0xad, 0xac, 0xaa, 0xa9, 0xa7, 0xa6, 0xa5, 0xa3, 0xa2, 0xa1, 0x9f, 0x9e, 0x9d, 0x9c, 0x9a, 0x99, 0x98, 0x96, 0x95, 0x94, 0x93, 0x91, 0x90,
    0x8f, 0x8e, 0x8d, 0x8b, 0x8a, 0x89, 0x88, 0x87, 0x86, 0x84, 0x83, 0x82, 0x81, 0x80, 0x7f, 0x7e, 0x7c, 0x7b, 0x7a, 0x79, 0x78, 0x77, 0x76, 0x75,
    0x74, 0x73, 0x72, 0x71, 0x70, 0x6f, 0x6e, 0x6d, 0x6c, 0x6b, 0x6a, 0x69, 0x68, 0x67, 0x66, 0x65, 0x64, 0x63, 0x62, 0x61, 0x60, 0x5f, 0x5e, 0x5d,
    0x5c, 0x5b, 0x5a, 0x59, 0x58, 0x58, 0x57, 0x56, 0x55, 0x54, 0x53, 0x52, 0x51, 0x51, 0x50, 0x4f, 0x4e, 0x4d, 0x4c, 0x4b, 0x4b, 0x4a, 0x49, 0x48,
    0x47, 0x46, 0x46, 0x45, 0x44, 0x43, 0x42, 0x42, 0x41, 0x40, 0x3f, 0x3f, 0x3e, 0x3d, 0x3c, 0x3b, 0x3b, 0x3a, 0x39, 0x38, 0x38, 0x37, 0x36, 0x35,
    0x35, 0x34, 0x33, 0x33, 0x32, 0x31, 0x30, 0x30, 0x2f, 0x2e, 0x2e, 0x2d, 0x2c, 0x2c, 0x2b, 0x2a, 0x2a, 0x29, 0x28, 0x28, 0x27, 0x26, 0x26, 0x25,
    0x24, 0x24, 0x23, 0x22, 0x22, 0x21, 0x20, 0x20, 0x1f, 0x1e, 0x1e, 0x1d, 0x1d, 0x1c, 0x1b, 0x1b, 0x1a, 0x1a, 0x19, 0x18, 0x18, 0x17, 0x17, 0x16,
    0x15, 0x15, 0x14, 0x14, 0x13, 0x12, 0x12, 0x11, 0x11, 0x10, 0x10, 0x0f, 0x0f, 0x0e, 0x0d, 0x0d, 0x0c, 0x0c, 0x0b, 0x0b, 0x0a, 0x0a, 0x09, 0x09,
    0x08, 0x07, 0x07, 0x06, 0x06, 0x05, 0x05, 0x04, 0x04, 0x03, 0x03, 0x02, 0x02, 0x01, 0x01, 0x00,
};
static const uint8_t fp_rsqrte_tab[384] = {
    0xff, 0xfd, 0xfb, 0xf9, 0xf7, 0xf5, 0xf3, 0xf2, 0xf0, 0xee, 0xec, 0xea, 0xe9, 0xe7, 0xe5, 0xe4, 0xe2, 0xe0, 0xdf, 0xdd, 0xdb, 0xda, 0xd8, 0xd7,
    0xd5, 0xd4, 0xd2, 0xd1, 0xcf, 0xce, 0xcc, 0xcb, 0xc9, 0xc8, 0xc6, 0xc5, 0xc4, 0xc2, 0xc1, 0xc0, 0xbe, 0xbd, 0xbc, 0xba, 0xb9, 0xb8, 0xb7, 0xb5,
    0xb4, 0xb3, 0xb2, 0xb0, 0xaf, 0xae, 0xad, 0xac, 0xaa, 0xa9, 0xa8, 0xa7, 0xa6, 0xa5, 0xa4, 0xa3, 0xa2, 0xa0, 0x9f, 0x9e, 0x9d, 0x9c, 0x9b, 0x9a,
    0x99, 0x98, 0x97, 0x96, 0x95, 0x94, 0x93, 0x92, 0x91, 0x90, 0x8f, 0x8e, 0x8d, 0x8c, 0x8c, 0x8b, 0x8a, 0x89, 0x88, 0x87, 0x86, 0x85, 0x84, 0x83,
    0x83, 0x82, 0x81, 0x80, 0x7f, 0x7e, 0x7e, 0x7d, 0x7c, 0x7b, 0x7a, 0x79, 0x79, 0x78, 0x77, 0x76, 0x76, 0x75, 0x74, 0x73, 0x72, 0x72, 0x71, 0x70,
    0x6f, 0x6f, 0x6e, 0x6d, 0x6d, 0x6c, 0x6b, 0x6a, 0x69, 0x69, 0x68, 0x68, 0x67, 0x67, 0x65, 0x65, 0x64, 0x64, 0x63, 0x63, 0x61, 0x61, 0x60, 0x60,
    0x5f, 0x5f, 0x5d, 0x5d, 0x5c, 0x5c, 0x5b, 0x5b, 0x5a, 0x5a, 0x58, 0x58, 0x57, 0x57, 0x56, 0x56, 0x55, 0x55, 0x54, 0x54, 0x52, 0x52, 0x51, 0x51,
    0x50, 0x50, 0x4f, 0x4f, 0x4e, 0x4e, 0x4d, 0x4d, 0x4c, 0x4c, 0x4b, 0x4b, 0x4a, 0x4a, 0x48, 0x48, 0x47, 0x47, 0x46, 0x46, 0x45, 0x45, 0x44, 0x44,
    0x43, 0x43, 0x42, 0x42, 0x41, 0x41, 0x40, 0x40, 0x3f, 0x3f, 0x3e, 0x3e, 0x3d, 0x3d, 0x3c, 0x3c, 0x3c, 0x3c, 0x3b, 0x3b, 0x3a, 0x3a, 0x39, 0x39,
    0x38, 0x38, 0x37, 0x37, 0x36, 0x36, 0x35, 0x35, 0x34, 0x34, 0x33, 0x33, 0x33, 0x33, 0x32, 0x32, 0x31, 0x31, 0x30, 0x30, 0x2f, 0x2f, 0x2e, 0x2e,
    0x2e, 0x2e, 0x2d, 0x2d, 0x2c, 0x2c, 0x2b, 0x2b, 0x2a, 0x2a, 0x2a, 0x2a, 0x29, 0x29, 0x28, 0x28, 0x27, 0x27, 0x26, 0x26, 0x26, 0x26, 0x25, 0x25,
    0x24, 0x24, 0x23, 0x23, 0x23, 0x23, 0x22, 0x22, 0x21, 0x21, 0x21, 0x21, 0x20, 0x20, 0x1f, 0x1f, 0x1e, 0x1e, 0x1e, 0x1e, 0x1d, 0x1d, 0x1c, 0x1c,
    0x1c, 0x1c, 0x1b, 0x1b, 0x1a, 0x1a, 0x1a, 0x1a, 0x19, 0x19, 0x18, 0x18, 0x18, 0x18, 0x17, 0x17, 0x16, 0x16, 0x16, 0x16, 0x15, 0x15, 0x14, 0x14,
    0x14, 0x14, 0x13, 0x13, 0x13, 0x13, 0x12, 0x12, 0x11, 0x11, 0x11, 0x11, 0x10, 0x10, 0x10, 0x10, 0x0f, 0x0f, 0x0e, 0x0e, 0x0e, 0x0e, 0x0d, 0x0d,
    0x0d, 0x0d, 0x0c, 0x0c, 0x0b, 0x0b, 0x0b, 0x0b, 0x0a, 0x0a, 0x0a, 0x0a, 0x09, 0x09, 0x09, 0x09, 0x08, 0x08, 0x08, 0x08, 0x07, 0x07, 0x06, 0x06,
    0x06, 0x06, 0x05, 0x05, 0x05, 0x05, 0x04, 0x04, 0x04, 0x04, 0x03, 0x03, 0x03, 0x03, 0x02, 0x02, 0x02, 0x02, 0x01, 0x01, 0x01, 0x01, 0x00, 0x00,
};
static inline uint32_t recip_estimate(uint32_t a) { return 0x100u | fp_recpe_tab[a - 256]; }          /* a in 256..511 */
static inline uint32_t recip_sqrt_estimate(uint32_t a) { return 0x100u | fp_rsqrte_tab[a - 128]; }    /* a in 128..511 */

/* Generic implementations on (sign, biased exponent, fraction left-aligned to 52 bits). */
static inline uint64_t fp_recpe_bits(uint64_t bits, int dbl) {
    int ebits = dbl ? 11 : 8, fbits = dbl ? 52 : 23;
    uint64_t sign = bits >> (ebits + fbits);
    int64_t exp = (int64_t)((bits >> fbits) & ((1u << ebits) - 1));
    uint64_t frac = bits & ((1ull << fbits) - 1);
    int64_t emax = (1 << ebits) - 1;
    uint64_t signbit = sign << (ebits + fbits);
    if (exp == emax) {
        if (frac) return bits | (1ull << (fbits - 1));          /* NaN: quieten */
        return signbit;                                         /* infinity -> zero */
    }
    if (exp == 0 && frac == 0) return signbit | ((uint64_t)emax << fbits);       /* zero -> infinity */
    if (exp == 0 && frac < (1ull << (fbits - 2))) return signbit | ((uint64_t)emax << fbits);  /* overflow */
    uint64_t f52 = frac << (52 - fbits);
    if (exp == 0) {
        if ((f52 >> 51) == 0) { exp = -1; f52 = (f52 << 2) & ((1ull << 52) - 1); }
        else f52 = (f52 << 1) & ((1ull << 52) - 1);
    }
    uint32_t scaled = 0x100 | (uint32_t)(f52 >> 44);
    int64_t rexp = (dbl ? 2045 : 253) - exp;
    uint64_t rf = (uint64_t)(recip_estimate(scaled) & 0xff) << 44;
    if (rexp == 0) rf = (1ull << 51) | (rf >> 1);
    else if (rexp == -1) { rf = (1ull << 50) | (rf >> 2); rexp = 0; }
    return signbit | ((uint64_t)rexp << fbits) | (rf >> (52 - fbits));
}
static inline uint64_t fp_rsqrte_bits(uint64_t bits, int dbl) {
    int ebits = dbl ? 11 : 8, fbits = dbl ? 52 : 23;
    uint64_t sign = bits >> (ebits + fbits);
    int64_t exp = (int64_t)((bits >> fbits) & ((1u << ebits) - 1));
    uint64_t frac = bits & ((1ull << fbits) - 1);
    int64_t emax = (1 << ebits) - 1;
    uint64_t default_nan = ((uint64_t)emax << fbits) | (1ull << (fbits - 1));
    if (exp == emax && frac) return bits | (1ull << (fbits - 1));                 /* NaN: quieten */
    if (exp == 0 && frac == 0) return (sign << (ebits + fbits)) | ((uint64_t)emax << fbits);  /* zero -> infinity */
    if (sign) return default_nan;                                                 /* negative -> NaN */
    if (exp == emax) return 0;                                                    /* +infinity -> +0 */
    uint64_t f52 = frac << (52 - fbits);
    if (exp == 0) {
        while ((f52 >> 51) == 0) { f52 = (f52 << 1) & ((1ull << 52) - 1); exp--; }
        f52 = (f52 << 1) & ((1ull << 52) - 1);
    }
    uint32_t scaled = (exp & 1) == 0 ? 0x100 | (uint32_t)(f52 >> 44) : 0x80 | (uint32_t)(f52 >> 45);
    int64_t rexp = ((dbl ? 3068 : 380) - exp) / 2;
    uint64_t est = recip_sqrt_estimate(scaled) & 0xff;
    return ((uint64_t)rexp << fbits) | (est << (fbits - 8));
}

static inline float recpe_f(float x) { uint32_t b; memcpy(&b, &x, 4); b = (uint32_t)fp_recpe_bits(b, 0); memcpy(&x, &b, 4); return x; }
static inline double recpe_d(double x) { uint64_t b; memcpy(&b, &x, 8); b = fp_recpe_bits(b, 1); memcpy(&x, &b, 8); return x; }
static inline float rsqrte_f(float x) { uint32_t b; memcpy(&b, &x, 4); b = (uint32_t)fp_rsqrte_bits(b, 0); memcpy(&x, &b, 4); return x; }
static inline double rsqrte_d(double x) { uint64_t b; memcpy(&b, &x, 8); b = fp_rsqrte_bits(b, 1); memcpy(&x, &b, 8); return x; }

/* FCVT between half precision (IEEE binary16, FPCR.AHP = 0) and single / double: FPConvert of the Arm manual with
 * round-to-nearest-even and FPCR.DN = 0. A NaN keeps its sign and the top bits of its payload and becomes quiet
 * (FPConvertNaN). Done on the bit patterns, so there is one rounding and no dependence on the host's conversions. */
static inline uint16_t fp_to_half_bits(uint64_t bits, int dbl) {
    int ebits = dbl ? 11 : 8, fbits = dbl ? 52 : 23;
    uint16_t sign = (uint16_t)(((bits >> (ebits + fbits)) & 1) << 15);
    int64_t exp = (int64_t)((bits >> fbits) & ((1u << ebits) - 1));
    uint64_t frac = bits & ((1ull << fbits) - 1);
    int64_t emax = (1 << ebits) - 1, bias = emax >> 1;
    if (exp == emax) {
        if (frac) return (uint16_t)(sign | 0x7e00 | ((frac >> (fbits - 10)) & 0x1ff));   /* NaN: quiet, payload bits below the quiet bit */
        return (uint16_t)(sign | 0x7c00);                                                 /* infinity */
    }
    if (exp == 0 && frac == 0) return sign;
    uint64_t m;                                    /* value = m * 2^(e - fbits), bit fbits of m set */
    int64_t e;
    if (exp) { m = frac | (1ull << fbits); e = exp - bias; }
    else { m = frac; e = 1 - bias; while (!(m >> fbits)) { m <<= 1; e--; } }
    if (e > 15) return (uint16_t)(sign | 0x7c00);                                         /* overflow: infinity (round to nearest) */
    int64_t sh = fbits - 10 + (e < -14 ? -14 - e : 0);                                    /* below 2^-14 the result is subnormal */
    if (sh > fbits + 1) return sign;                                                      /* less than half the smallest subnormal */
    uint64_t q = m >> sh, rem = m & ((1ull << sh) - 1), half = 1ull << (sh - 1);
    if (rem > half || (rem == half && (q & 1))) q++;
    uint32_t r = e < -14 ? (uint32_t)q : (uint32_t)(((uint64_t)(e + 15) << 10) + (q - 0x400));   /* a carry out of the fraction moves into the exponent */
    if (r >= 0x7c00) r = 0x7c00;
    return (uint16_t)(sign | r);
}
static inline uint64_t fp_from_half_bits(uint16_t h, int dbl) {
    int ebits = dbl ? 11 : 8, fbits = dbl ? 52 : 23;
    uint64_t sign = (uint64_t)(h >> 15) << (ebits + fbits), frac = h & 0x3ff, emax = (1u << ebits) - 1;
    int64_t exp = (h >> 10) & 31, bias = (int64_t)(emax >> 1), e;
    if (exp == 31) {
        if (frac) return sign | (emax << fbits) | (1ull << (fbits - 1)) | ((frac & 0x1ff) << (fbits - 10));
        return sign | (emax << fbits);
    }
    if (exp == 0) {
        if (!frac) return sign;
        e = -14;
        while (!(frac & 0x400)) { frac <<= 1; e--; }
        frac &= 0x3ff;
    } else e = exp - 15;
    return sign | ((uint64_t)(e + bias) << fbits) | (frac << (fbits - 10));
}
static inline uint16_t f32_to_half(float x) { uint32_t b; memcpy(&b, &x, 4); return fp_to_half_bits(b, 0); }
static inline uint16_t f64_to_half(double x) { uint64_t b; memcpy(&b, &x, 8); return fp_to_half_bits(b, 1); }
static inline float half_to_f32(uint16_t h) { uint32_t b = (uint32_t)fp_from_half_bits(h, 0); float x; memcpy(&x, &b, 4); return x; }
static inline double half_to_f64(uint16_t h) { uint64_t b = fp_from_half_bits(h, 1); double x; memcpy(&x, &b, 8); return x; }

/* FMAX / FMIN / FMAXNM / FMINNM (FPMax, FPMin, FPMaxNum, FPMinNum of the Arm manual, FPCR.DN = 0), on bit patterns.
 * A signalling NaN operand wins (first operand first) and is returned quiet; then a quiet NaN (first operand first),
 * except that the NM forms return the other operand when exactly one operand is a quiet NaN and none is signalling.
 * +0 is greater than -0. The C library's fmax / fmin do neither. */
static inline uint32_t fp_maxmin32_bits(uint32_t a, uint32_t b, int is_min, int num) {
    int na = (a & 0x7fffffffu) > 0x7f800000u, nb = (b & 0x7fffffffu) > 0x7f800000u;
    if (na | nb) {
        int sa = na && !(a & 0x00400000u), sb = nb && !(b & 0x00400000u);
        if (num && !sa && !sb && na != nb) return na ? b : a;
        return (sa ? a : sb ? b : na ? a : b) | 0x00400000u;
    }
    if (((a | b) & 0x7fffffffu) == 0) return is_min ? (a | b) : (a & b);
    float fa, fb;
    memcpy(&fa, &a, 4); memcpy(&fb, &b, 4);
    return (is_min ? fa < fb : fa > fb) ? a : b;
}
static inline uint64_t fp_maxmin64_bits(uint64_t a, uint64_t b, int is_min, int num) {
    int na = (a & 0x7fffffffffffffffull) > 0x7ff0000000000000ull, nb = (b & 0x7fffffffffffffffull) > 0x7ff0000000000000ull;
    if (na | nb) {
        int sa = na && !(a & 0x0008000000000000ull), sb = nb && !(b & 0x0008000000000000ull);
        if (num && !sa && !sb && na != nb) return na ? b : a;
        return (sa ? a : sb ? b : na ? a : b) | 0x0008000000000000ull;
    }
    if (((a | b) & 0x7fffffffffffffffull) == 0) return is_min ? (a | b) : (a & b);
    double fa, fb;
    memcpy(&fa, &a, 8); memcpy(&fb, &b, 8);
    return (is_min ? fa < fb : fa > fb) ? a : b;
}
static inline float fp_maxmin32(float x, float y, int is_min, int num) {
    uint32_t a, b;
    memcpy(&a, &x, 4); memcpy(&b, &y, 4);
    a = fp_maxmin32_bits(a, b, is_min, num);
    memcpy(&x, &a, 4);
    return x;
}
static inline double fp_maxmin64(double x, double y, int is_min, int num) {
    uint64_t a, b;
    memcpy(&a, &x, 8); memcpy(&b, &y, 8);
    a = fp_maxmin64_bits(a, b, is_min, num);
    memcpy(&x, &a, 8);
    return x;
}

#endif
