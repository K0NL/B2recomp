/* libc++ iostreams: the base classes the game imports from the SDK's C++ library.
 *
 * The game has its own stream classes (std::ostringstream / istringstream with their stringbuf are
 * compiled into the game) but takes the base-class pieces from the SDK: ios_base::init, the
 * basic_streambuf constructor and default virtuals, ostream::sentry, operator<< for integers,
 * istream::get and operator>>(float&). Layouts are libc++'s on AArch64 (the stringbuf fields the game
 * reads in f_7102d099c0 -- pbase +0x28, pptr +0x30, high mark +0x58, mode +0x60 -- agree with them):
 *
 *   basic_streambuf: +0 vtable, +8 locale, +0x10 eback, +0x18 gptr, +0x20 egptr, +0x28 pbase, +0x30 pptr, +0x38 epptr
 *     vtable: +0x10 imbue, +0x18 setbuf, +0x20 seekoff, +0x28 seekpos, +0x30 sync, +0x38 showmanyc, +0x40 xsgetn,
 *             +0x48 underflow, +0x50 uflow, +0x58 pbackfail, +0x60 xsputn, +0x68 overflow
 *   ios_base: +0 vtable, +8 fmtflags (u32), +0x10 precision, +0x18 width, +0x20 rdstate (u32), +0x24 exceptions (u32),
 *             +0x28 rdbuf, +0x30 locale, +0x38.. callback and iword/pword arrays (to +0x88); basic_ios: +0x88 tie, +0x90 fill
 *   a stream object reaches its basic_ios part through the offset stored at vtable[-3]
 *   ostream::sentry: +0 bool ok, +8 ostream *
 *
 * Only the classic locale exists (see libcxx.c). Exceptions are never thrown: errors only set state bits. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include "runtime.h"
#include "hostrt.h"

#define A0 (c->x[0])
#define A1 (c->x[1])
#define A2 (c->x[2])
#define RET(v) (c->x[0] = (uint64_t)(v))

enum { SB_EBACK = 0x10, SB_GPTR = 0x18, SB_EGPTR = 0x20, SB_PBASE = 0x28, SB_PPTR = 0x30, SB_EPPTR = 0x38 };
enum { VT_XSGETN = 0x40, VT_UNDERFLOW = 0x48, VT_UFLOW = 0x50, VT_XSPUTN = 0x60, VT_OVERFLOW = 0x68 };
enum { IOS_FLAGS = 8, IOS_PRECISION = 0x10, IOS_WIDTH = 0x18, IOS_STATE = 0x20, IOS_EXCEPTIONS = 0x24, IOS_RDBUF = 0x28, IOS_LOCALE = 0x30, IOS_FILL = 0x90 };
enum { F_DEC = 2, F_HEX = 8, F_LEFT = 0x20, F_OCT = 0x40, F_SHOWBASE = 0x200, F_SHOWPOS = 0x800, F_SKIPWS = 0x1000, F_UPPERCASE = 0x4000 };
enum { BADBIT = 1, EOFBIT = 2, FAILBIT = 4 };
#define EOF_INT 0xffffffffu

static uint8_t g_locale_impl[64];                      /* stands for the classic locale, like libcxx.c's */

/* Calls a virtual function of a guest object with up to two more arguments; returns x0. */
static uint64_t vcall(Ctx *c, uint64_t obj, int slot, uint64_t a1, uint64_t a2) {
    c->x[0] = obj;
    c->x[1] = a1;
    c->x[2] = a2;
    call_guest(c, rd64(rd64(obj) + (uint64_t)slot));
    return c->x[0];
}

static uint64_t ios_of(uint64_t stream) { return stream + rd64(rd64(stream) - 0x18); }
void cxxrt_throw_ios_failure(Ctx *c, const char *msg);          /* nncxxrt.c: throws std::ios_base::failure */
/* setstate() as it behaves inside libc++'s stream operations: they run under `try { ... setstate(bits); } catch (...)
 * { __set_badbit_and_consider_rethrow(); }`, so a bit enabled in exceptions() turns into badbit, and the exception
 * leaves the operation only when badbit itself is enabled in exceptions(). */
static void set_state_c(Ctx *c, uint64_t ios, uint32_t bits) {
    uint32_t state = rd32(ios + IOS_STATE) | bits, exceptions = rd32(ios + IOS_EXCEPTIONS);
    if (state & exceptions) state |= BADBIT;
    wr32(ios + IOS_STATE, state);
    if ((state & exceptions) && (exceptions & BADBIT)) cxxrt_throw_ios_failure(c, "ios_base::clear");
}
#define set_state(ios, bits) set_state_c(c, ios, bits)

/* ---- basic_streambuf */
static void sb_ctor(Ctx *c) {
    uint64_t sb = A0;
    wr64(sb + 8, (uint64_t)(uintptr_t)g_locale_impl);
    for (int off = SB_EBACK; off <= SB_EPPTR; off += 8) wr64(sb + (uint64_t)off, 0);
    RET(sb);
}
static void sb_dtor(Ctx *c) { RET(A0); }
static void sb_sync(Ctx *c) { RET(0); }
static void sb_imbue(Ctx *c) { (void)c; }
static void sb_setbuf(Ctx *c) { RET(A0); }
static void sb_showmanyc(Ctx *c) { RET(0); }
static void sb_eof(Ctx *c) { RET(EOF_INT); }                         /* underflow, pbackfail, overflow */
static void sb_uflow(Ctx *c) {
    uint64_t sb = A0;
    if ((uint32_t)vcall(c, sb, VT_UNDERFLOW, 0, 0) == EOF_INT) { RET(EOF_INT); return; }
    uint64_t g = rd64(sb + SB_GPTR);
    wr64(sb + SB_GPTR, g + 1);
    RET(*(uint8_t *)(uintptr_t)g);
}
static void sb_xsgetn(Ctx *c) {                                       /* (this, char *s, streamsize n) */
    uint64_t sb = A0, s = A1;
    int64_t n = (int64_t)A2, done = 0;
    while (done < n) {
        uint64_t g = rd64(sb + SB_GPTR), e = rd64(sb + SB_EGPTR);
        if (g < e) {
            int64_t k = (int64_t)(e - g) < n - done ? (int64_t)(e - g) : n - done;
            memcpy((void *)(uintptr_t)(s + (uint64_t)done), (void *)(uintptr_t)g, (size_t)k);
            wr64(sb + SB_GPTR, g + (uint64_t)k);
            done += k;
        } else {
            uint32_t ch = (uint32_t)vcall(c, sb, VT_UFLOW, 0, 0);
            if (ch == EOF_INT) break;
            *(uint8_t *)(uintptr_t)(s + (uint64_t)done++) = (uint8_t)ch;
        }
    }
    RET(done);
}
static int64_t put_chars(Ctx *c, uint64_t sb, const char *s, int64_t n) {
    int64_t done = 0;
    while (done < n) {
        uint64_t p = rd64(sb + SB_PPTR), e = rd64(sb + SB_EPPTR);
        if (p < e) {
            int64_t k = (int64_t)(e - p) < n - done ? (int64_t)(e - p) : n - done;
            memcpy((void *)(uintptr_t)p, s + done, (size_t)k);
            wr64(sb + SB_PPTR, p + (uint64_t)k);
            done += k;
        } else {
            if ((uint32_t)vcall(c, sb, VT_OVERFLOW, (uint8_t)s[done], 0) == EOF_INT) break;
            done++;
        }
    }
    return done;
}
static void sb_xsputn(Ctx *c) {                                       /* (this, const char *s, streamsize n) */
    int64_t n = (int64_t)A2;
    char *copy = malloc(n > 0 ? (size_t)n : 1);                       /* the guest call below may clobber what s points into */
    if (n > 0) memcpy(copy, (void *)(uintptr_t)A1, (size_t)n);
    int64_t done = put_chars(c, A0, copy, n);
    free(copy);
    RET(done);
}

/* ---- ios_base / basic_ios */
static void ios_init(Ctx *c) {                                        /* ios_base::init(void *rdbuf) */
    uint64_t ios = A0, sb = A1;
    memset((void *)(uintptr_t)(ios + 8), 0, 0x88 - 8);
    wr32(ios + IOS_FLAGS, F_SKIPWS | F_DEC);
    wr64(ios + IOS_PRECISION, 6);
    wr32(ios + IOS_STATE, sb ? 0 : BADBIT);
    wr64(ios + IOS_RDBUF, sb);
    wr64(ios + IOS_LOCALE, (uint64_t)(uintptr_t)g_locale_impl);
}
static void ios_clear(Ctx *c) {                                       /* ios_base::clear(iostate): throws when an enabled bit is set */
    uint64_t ios = A0;
    uint32_t state = rd64(ios + IOS_RDBUF) ? (uint32_t)A1 : (uint32_t)A1 | BADBIT;
    wr32(ios + IOS_STATE, state);
    if (state & rd32(ios + IOS_EXCEPTIONS)) cxxrt_throw_ios_failure(c, "ios_base::clear");
}
static void ios_getloc(Ctx *c) { wr64(c->x[8], (uint64_t)(uintptr_t)g_locale_impl); }   /* returns a locale by value */
static void nothing(Ctx *c) { (void)c; }
static void ret_this(Ctx *c) { RET(A0); }

/* ---- basic_ostream */
static void os_sentry_ctor(Ctx *c) {                                  /* (sentry *, ostream &) */
    uint64_t s = A0, os = A1;
    wr64(s, 0);
    wr64(s + 8, os);
    *(uint8_t *)(uintptr_t)s = rd32(ios_of(os) + IOS_STATE) == 0;
    RET(s);
}

/* Writes one formatted field honouring width, fill and adjustment, then resets the width. */
static void put_field(Ctx *c, uint64_t os, const char *text) {
    uint64_t ios = ios_of(os), sb = rd64(ios + IOS_RDBUF);
    if (rd32(ios + IOS_STATE) != 0 || !sb) { set_state(ios, FAILBIT); return; }
    char buf[160];
    int len = (int)strlen(text), width = (int)rd64(ios + IOS_WIDTH), fill = (int)rd32(ios + IOS_FILL);
    if (fill <= 0 || fill > 0xff) fill = ' ';                         /* libc++ fills lazily: an unset fill means a space */
    if (width > (int)sizeof buf - 1) width = (int)sizeof buf - 1;
    int pad = width > len ? width - len : 0, left = (rd32(ios + IOS_FLAGS) & F_LEFT) != 0;
    memset(buf, fill, sizeof buf);
    /* ios_base::internal (0x10): the padding goes after the sign or the 0x prefix (libc++ __pad_and_output) */
    int prefix = 0;
    if ((rd32(ios + IOS_FLAGS) & 0xb0) == 0x10 && pad) {
        if (text[0] == '-' || text[0] == '+') prefix = 1;
        else if (len >= 2 && text[0] == '0' && (text[1] | 0x20) == 'x') prefix = 2;
    }
    if (prefix) { memcpy(buf, text, (size_t)prefix); memcpy(buf + prefix + pad, text + prefix, (size_t)(len - prefix)); }
    else memcpy(buf + (left ? 0 : pad), text, (size_t)len);
    wr64(ios + IOS_WIDTH, 0);
    if (put_chars(c, sb, buf, len + pad) != len + pad) set_state(ios_of(os), BADBIT);
}
static void put_integer(Ctx *c, uint64_t os, uint64_t value, int is_signed) {
    uint32_t flags = rd32(ios_of(os) + IOS_FLAGS);
    char text[80];
    if (flags & F_HEX) snprintf(text, sizeof text, flags & F_UPPERCASE ? "%s%llX" : "%s%llx", flags & F_SHOWBASE ? (flags & F_UPPERCASE ? "0X" : "0x") : "", (unsigned long long)value);
    else if (flags & F_OCT) snprintf(text, sizeof text, "%s%llo", flags & F_SHOWBASE && value ? "0" : "", (unsigned long long)value);
    else if (is_signed) snprintf(text, sizeof text, flags & F_SHOWPOS ? "%+lld" : "%lld", (long long)value);
    else snprintf(text, sizeof text, "%llu", (unsigned long long)value);
    put_field(c, os, text);
}
static void os_put_int(Ctx *c) {                                      /* operator<<(int): hex and oct print the 32-bit pattern */
    uint64_t os = A0;
    uint32_t flags = rd32(ios_of(os) + IOS_FLAGS);
    int32_t v = (int32_t)A1;
    put_integer(c, os, flags & (F_HEX | F_OCT) ? (uint64_t)(uint32_t)v : (uint64_t)(int64_t)v, 1);
    RET(os);
}
static void os_put_ulong(Ctx *c) { uint64_t os = A0; put_integer(c, os, A1, 0); RET(os); }

/* ---- basic_istream */
static uint32_t peek_char(Ctx *c, uint64_t sb) {
    uint64_t g = rd64(sb + SB_GPTR);
    if (g < rd64(sb + SB_EGPTR)) return *(uint8_t *)(uintptr_t)g;
    return (uint32_t)vcall(c, sb, VT_UNDERFLOW, 0, 0);
}
static uint32_t take_char(Ctx *c, uint64_t sb) {
    uint64_t g = rd64(sb + SB_GPTR);
    if (g < rd64(sb + SB_EGPTR)) { wr64(sb + SB_GPTR, g + 1); return *(uint8_t *)(uintptr_t)g; }
    return (uint32_t)vcall(c, sb, VT_UFLOW, 0, 0);
}
static void is_get(Ctx *c) {                                          /* int get(): also records gcount at +8 */
    uint64_t is = A0, ios = ios_of(is), sb = rd64(ios + IOS_RDBUF);
    uint32_t ch = EOF_INT;
    wr64(is + 8, 0);
    if (rd32(ios + IOS_STATE) == 0 && sb) ch = take_char(c, sb);
    if (ch == EOF_INT) set_state(ios_of(is), EOFBIT | FAILBIT); else wr64(is + 8, 1);
    RET(ch);
}
static void is_read_float(Ctx *c) {                                   /* operator>>(float &) */
    uint64_t is = A0, out = A1, ios = ios_of(is), sb = rd64(ios + IOS_RDBUF);
    char text[80];
    int n = 0;
    if (rd32(ios + IOS_STATE) != 0 || !sb) { set_state(ios, FAILBIT); RET(is); return; }
    uint32_t ch = peek_char(c, sb);
    if (rd32(ios_of(is) + IOS_FLAGS) & F_SKIPWS)
        while (ch == ' ' || (ch >= 9 && ch <= 13)) { take_char(c, sb); ch = peek_char(c, sb); }
    while (ch != EOF_INT && n < (int)sizeof text - 1 && (strchr("+-.eExXpP", (int)ch) || (ch >= '0' && ch <= '9') || (ch | 0x20) == 'n' || (ch | 0x20) == 'a' ||
           (ch | 0x20) == 'i' || (ch | 0x20) == 'f' || ((ch | 0x20) >= 'a' && (ch | 0x20) <= 'f' && n > 1 && (text[1] | 0x20) == 'x'))) {
        text[n++] = (char)ch;
        take_char(c, sb);
        ch = peek_char(c, sb);
    }
    text[n] = 0;
    char *end = text;
    float v = strtof(text, &end);
    ios = ios_of(is);
    if (end == text) { v = 0; set_state(ios, FAILBIT); }
    if (ch == EOF_INT) set_state(ios, EOFBIT);
    *(float *)(uintptr_t)out = v;
    RET(is);
}

#define SBUF "_ZNSt3__115basic_streambufIcNS_11char_traitsIcEEE"
#define OSTR "_ZNSt3__113basic_ostreamIcNS_11char_traitsIcEEE"
#define ISTR "_ZNSt3__113basic_istreamIcNS_11char_traitsIcEEE"
static const ImportDef iostream_imports[] = {
    {SBUF "C2Ev", sb_ctor}, {SBUF "D2Ev", sb_dtor}, {SBUF "4syncEv", sb_sync}, {SBUF "5imbueERKNS_6localeE", sb_imbue},
    {SBUF "6setbufEPcl", sb_setbuf}, {SBUF "9showmanycEv", sb_showmanyc}, {SBUF "6xsgetnEPcl", sb_xsgetn}, {SBUF "9underflowEv", sb_eof},
    {SBUF "5uflowEv", sb_uflow}, {SBUF "9pbackfailEi", sb_eof}, {SBUF "6xsputnEPKcl", sb_xsputn}, {SBUF "8overflowEi", sb_eof},
    {"_ZNSt3__18ios_base4initEPv", ios_init}, {"_ZNSt3__18ios_base5clearEj", ios_clear}, {"_ZNKSt3__18ios_base6getlocEv", ios_getloc},
    {"_ZNSt3__19basic_iosIcNS_11char_traitsIcEEED2Ev", ret_this},
    {OSTR "6sentryC1ERS3_", os_sentry_ctor}, {OSTR "6sentryD1Ev", nothing}, {OSTR "lsEi", os_put_int}, {OSTR "lsEm", os_put_ulong},
    {OSTR "D0Ev", ret_this}, {OSTR "D1Ev", ret_this}, {OSTR "D2Ev", ret_this},
    {ISTR "3getEv", is_get}, {ISTR "rsERf", is_read_float}, {ISTR "D1Ev", ret_this},
};

void register_iostream_imports(void) {
    register_imports(iostream_imports, (int)(sizeof iostream_imports / sizeof iostream_imports[0]));
}
