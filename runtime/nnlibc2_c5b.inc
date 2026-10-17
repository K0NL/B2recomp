/* ================================================================ close5 (c5b): C stdio = musl's own code
 * (included at the end of nnlibc2.c)
 * The stream functions the game imports (printf fprintf fopen freopen fclose fread getc feof ferror fflush write) are
 * the SDK's musl code lifted from sdk.elf (sdklift_roots.txt, "close5 (c5b)"), running on musl's own FILE objects:
 *   stdin  FILE @sdk 0xc1ba28 (read  = __stdin_read 0x4cf3a8 -> 0: always end of file),
 *   stdout FILE @sdk 0xc1bb20 (write = __stdout_write 0x4cf658 -> __nnmusl_ConsoleWrite 0x5191e8, line buffered),
 *   stderr FILE @sdk 0xc1b930 (same write function, unbuffered),
 * the variables `stdin` / `stdout` / `stderr` (sdk 0xc1bb10 / 0xc1bc08 / 0xc1ba18) hold those addresses after relocation.
 * __nnmusl_ConsoleWrite builds a LogMetaData and calls nndetailDiagPutImplForC(meta, text, size) [0x182cf0 ->
 * nn::diag::detail::PutImpl 0x182bf0 -> CallAllLogObserver -> PrintDebugString 0x184120 -> svc 0x27 OutputDebugString],
 * and returns `size`: the console's debug log, which no one sees on a retail unit. Here that one call is the host
 * boundary: the text goes to the trace file. Files: fopen -> open 0x4cbed8 -> __nnmusl_FileOpen 0x517088 (descriptor
 * table @0xcf4318, 256 entries of 0x18 bytes, descriptor = 0x01010000 | index) -> nn::fs::OpenFile / CreateFile /
 * SetFileSize / ReadFile / WriteFile / GetFileSize / CloseFile = the port's nn::fs handlers (nnfs.c).
 * The three FILE locks are set up by __nnmusl_init_locks [sdk 0x51a13c..0x51a148] at SDK start: done here with the
 * same three lifted functions. The host GFile layer above stays only for a run without sdk.elf. */
int sdk_image_ready(void);
ImportFn sdk_lifted(const char *name);
uint64_t sdk_symbol(const char *name);
void sdk_host_override(const char *name, ImportFn fn);
int sdk_install(const char *name);
uint32_t fs_mount_plain(const char *name, const char *host_dir);

static char *g_diag_capture;             /* self-test: the text that reached the debug log */
static size_t g_diag_capture_len, g_diag_capture_cap;
static void diag_PutImplForC(Ctx *c) {   /* (const LogMetaData *, const char *text, size_t size) */
    const char *text = (const char *)(uintptr_t)A1;
    size_t n = (size_t)A2;
    if (g_diag_capture) {
        if (n > g_diag_capture_cap - 1 - g_diag_capture_len) n = g_diag_capture_cap - 1 - g_diag_capture_len;
        memcpy(g_diag_capture + g_diag_capture_len, text, n);
        g_diag_capture_len += n;
        g_diag_capture[g_diag_capture_len] = 0;
        return;
    }
    trace_printf("     guest console: %.*s%s", (int)n, text, n && text[n - 1] == '\n' ? "" : "\n");
}
/* nn::fs::SetFileSize(FileHandle, s64) [sdk 0x1f5c50 -> FileAccessor::SetSize 0x1f31d0: the file system's answer].
 * musl calls it for O_TRUNC only (__nnmusl_FileOpen 0x517318, size 0). A file handle of nnfs.c is the Win32 handle.
 * The failure value is not modelled on anything (unverified): musl maps every unknown Result to errno 1. */
static void fs_SetFileSize_c5b(Ctx *c) {
    LARGE_INTEGER size;
    size.QuadPart = (LONGLONG)A1;
    if (!SetFilePointerEx((HANDLE)A0, size, NULL, FILE_BEGIN) || !SetEndOfFile((HANDLE)A0)) { RET(0x2ee202); return; }
    RET(0);
}

static const char *const musl_stdio_names[] = {"printf", "fprintf", "fopen", "freopen", "fclose", "fread", "getc", "feof", "ferror", "fflush", "write"};
#define MUSL_STDIO_COUNT ((int)(sizeof musl_stdio_names / sizeof musl_stdio_names[0]))
static int g_musl_stdio;

void libc2_install_musl_stdio(void) {
    static const char *const var[3] = {"stdin", "stdout", "stderr"};
    static const char *const init[3] = {"__nnmusl_stdin_lock_init", "__nnmusl_stdout_lock_init", "__nnmusl_stderr_lock_init"};
    if (!sdk_image_ready()) return;
    for (int i = 0; i < MUSL_STDIO_COUNT; i++) if (!sdk_lifted(musl_stdio_names[i])) return;
    for (int i = 0; i < 3; i++) if (!sdk_symbol(var[i]) || !sdk_lifted(init[i])) return;
    sdk_host_override("nndetailDiagPutImplForC", diag_PutImplForC);
    sdk_host_override("_ZN2nn2fs11SetFileSizeENS0_10FileHandleEl", fs_SetFileSize_c5b);
    Ctx *c = _aligned_malloc(sizeof *c, 16);
    uint8_t *stack = VirtualAlloc(NULL, 1 << 16, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    memset(c, 0, sizeof *c);
    for (int i = 0; i < 3; i++) {
        uint64_t file = *(uint64_t *)(uintptr_t)sdk_symbol(var[i]);
        c->x[31] = (uint64_t)(uintptr_t)(stack + (1 << 16) - 0x100);
        c->x[0] = file;
        sdk_lifted(init[i])(c);                              /* what __nnmusl_init_locks does */
        g_std_ptr[i] = file;
    }
    VirtualFree(stack, 0, MEM_RELEASE);
    _aligned_free(c);
    for (int i = 0; i < MUSL_STDIO_COUNT; i++) sdk_install(musl_stdio_names[i]);
    g_musl_stdio = 1;
}

/* ---- self-test */
#define SG(p) ((uint64_t)(uintptr_t)(p))
#define SR32(a) (*(uint32_t *)(uintptr_t)(a))
#define SR64(a) (*(uint64_t *)(uintptr_t)(a))
static uint64_t st_call(Ctx *c, const char *name, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3) {
    ImportFn f = import_lookup(name);
    if (!f) f = sdk_lifted(name);
    if (!f) { printf("  FAIL stdio: no function %s\n", name); return 0xdeadbeef; }
    c->x[0] = a0; c->x[1] = a1; c->x[2] = a2; c->x[3] = a3;
    f(c);
    return c->x[0];
}
static int st_errno(Ctx *c) {
    ImportFn f = sdk_lifted("___errno_location");
    if (!f) return -1;
    f(c);
    return (int)SR32(c->x[0]);
}
static int st_host_text(const char *path, const char *text) {
    char buf[256] = "";
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = 0;
    if (strcmp(buf, text)) printf("    host file has \"%s\", expected \"%s\"\n", buf, text);
    return strcmp(buf, text) == 0;
}
static int st_wide_is(const uint32_t *w, const char *text) {
    size_t i = 0;
    for (; text[i]; i++) if (w[i] != (uint8_t)text[i]) return 0;
    return w[i] == 0;
}
int libc2_stdio_selftest(Ctx *c) {
    int bad = 0, cases = 0;
    #define STCHECK(cond, text) do { cases++; if (!(cond)) { printf("  FAIL stdio: %s\n", text); bad++; } } while (0)
    if (!g_musl_stdio) { printf("  FAIL stdio: musl's stdio was not installed (sdk.elf missing or a root not lifted)\n"); return 1; }
    for (int i = 0; i < MUSL_STDIO_COUNT; i++) STCHECK(import_lookup(musl_stdio_names[i]) == sdk_lifted(musl_stdio_names[i]), musl_stdio_names[i]);
    uint64_t f_in = g_std_ptr[0], f_out = g_std_ptr[1], f_err = g_std_ptr[2];
    STCHECK(f_out == sdk_symbol("stdout") - 0xe8 && SR64(f_out + 0x48) == sdk_symbol("__stdout_write") && SR32(f_out + 0x78) == 1, "stdout is not musl's FILE with __stdout_write");
    STCHECK(SR64(f_err + 0x48) == sdk_symbol("__stdout_write") && SR32(f_err + 0x78) == 2 && SR64(f_in + 0x40) == sdk_symbol("__stdin_read"), "stderr / stdin are not musl's FILE objects");
    STCHECK(*(uint8_t *)(uintptr_t)(f_out + 0x84) == 1 && *(uint8_t *)(uintptr_t)(f_err + 0x84) == 1, "the FILE locks were not initialised");

    char cap[512];
    g_diag_capture = cap; g_diag_capture_cap = sizeof cap; g_diag_capture_len = 0; cap[0] = 0;
    /* printf("%d|%s|%5.2f|%Lf|%x\n", 42, "str", 3.14159, 2.5L, 255): double in d0, long double (binary128) in q1 */
    c->v[0].d[0] = 0x400921f9f01b866eULL; c->v[0].d[1] = 0;
    c->v[1].d[0] = 0; c->v[1].d[1] = 0x4000400000000000ULL;
    uint64_t r = st_call(c, "printf", SG("%d|%s|%5.2f|%Lf|%x\n"), 42, SG("str"), 255);
    STCHECK((int)r == 25 && strcmp(cap, "42|str| 3.14|2.500000|ff\n") == 0, "printf %d %s %5.2f %Lf %x to stdout");
    if (bad) printf("    printf gave %d \"%s\"\n", (int)r, cap);
    g_diag_capture_len = 0; cap[0] = 0;
    r = st_call(c, "printf", SG("no newline"), 0, 0, 0);
    STCHECK((int)r == 10 && cap[0] == 0, "stdout is not line buffered");
    st_call(c, "fflush", f_out, 0, 0, 0);
    STCHECK(strcmp(cap, "no newline") == 0, "fflush(stdout) did not write the buffer");
    g_diag_capture_len = 0; cap[0] = 0;
    r = st_call(c, "fprintf", f_err, SG("e%03d:%-4s|"), 7, SG("ab"));
    STCHECK((int)r == 10 && strcmp(cap, "e007:ab  |") == 0, "fprintf(stderr) is not unbuffered musl output");
    g_diag_capture_len = 0; cap[0] = 0;
    r = st_call(c, "write", 1, SG("raw"), 3, 0);
    STCHECK((int64_t)r == 3 && strcmp(cap, "raw") == 0, "write(1) does not reach the console");
    r = st_call(c, "write", 0x5555, SG("raw"), 3, 0);
    STCHECK((int64_t)r == 3 && strcmp(cap, "rawraw") == 0, "write(descriptor without the file tag) is not console output [sdk 0x5177d4]");
    r = st_call(c, "write", 0, SG("raw"), 3, 0);
    STCHECK((int64_t)r == -1 && st_errno(c) == 9, "write(0) is not -1 / EBADF");
    r = st_call(c, "write", 0x01010007, SG("raw"), 3, 0);
    STCHECK((int64_t)r == -1 && st_errno(c) == 9, "write(closed file descriptor) is not -1 / EBADF");
    /* __stdin_read [sdk 0x4cf3a8] returns 0 without touching the flags and __uflow [0x508468] turns that into EOF: getc gives -1, feof stays 0 */
    STCHECK((int)st_call(c, "getc", f_in, 0, 0, 0) == -1 && (int)st_call(c, "feof", f_in, 0, 0, 0) == 0 && (int)st_call(c, "ferror", f_in, 0, 0, 0) == 0, "getc(stdin) is not EOF with feof 0");
    g_diag_capture = NULL;

    /* files on a scratch mount under %TEMP% */
    char base[MAX_PATH], p[MAX_PATH];
    GetTempPathA(MAX_PATH - 64, base);
    snprintf(base + strlen(base), 64, "ssbu_stdio_selftest_%lu", (unsigned long)GetCurrentProcessId());
    CreateDirectoryA(base, NULL);
    STCHECK(fs_mount_plain("c5bstdio", base) == 0, "scratch mount");
    snprintf(p, sizeof p, "%s.work.%lu\\w.txt", base, (unsigned long)GetCurrentProcessId());   /* nnfs.c: changes of a mount live in its working folder until a commit */
    uint64_t f = st_call(c, "fopen", SG("c5bstdio:/none.txt"), SG("r"), 0, 0);
    STCHECK(f == 0 && st_errno(c) == 2, "fopen(missing, \"r\") is not NULL / ENOENT");
    f = st_call(c, "fopen", SG("c5bstdio:/w.txt"), SG("w"), 0, 0);
    STCHECK(f != 0 && (SR32(f + 0x78) >> 16) == 0x101, "fopen(\"w\") does not create the file (descriptor 0x0101xxxx)");
    if (f) {
        c->v[0].d[0] = 0x3ff8000000000000ULL;
        r = st_call(c, "fprintf", f, SG("%d-%s-%.1f\n"), 42, SG("x"));
        STCHECK((int)r == 9, "fprintf to a file");
        STCHECK((int)st_call(c, "fputs", SG("second\n"), f, 0, 0) >= 0, "fputs");
        STCHECK(st_call(c, "fwrite", SG("abc"), 1, 3, f) == 3, "fwrite");
        int rc = (int)st_call(c, "fclose", f, 0, 0, 0);
        if (rc) printf("    fclose %d errno %d\n", rc, st_errno(c));
        STCHECK(rc == 0 && st_host_text(p, "42-x-1.5\nsecond\nabc"), "the written file has the wrong content");
    }
    f = st_call(c, "fopen", SG("c5bstdio:/w.txt"), SG("a"), 0, 0);
    STCHECK(f != 0, "fopen(\"a\")");
    if (f) {
        st_call(c, "fprintf", f, SG("+%s"), SG("app"), 0);
        STCHECK((int)st_call(c, "fclose", f, 0, 0, 0) == 0 && st_host_text(p, "42-x-1.5\nsecond\nabc+app"), "append mode");
    }
    f = st_call(c, "fopen", SG("c5bstdio:/w.txt"), SG("r"), 0, 0);
    STCHECK(f != 0, "fopen(\"r\")");
    if (f) {
        char buf[64] = "";
        STCHECK((int)st_call(c, "getc", f, 0, 0, 0) == '4', "getc");
        STCHECK(st_call(c, "fgets", SG(buf), sizeof buf, f, 0) == SG(buf) && strcmp(buf, "2-x-1.5\n") == 0, "fgets");
        STCHECK((int)st_call(c, "fseek", f, 7, 1, 0) == 0 && (int64_t)st_call(c, "ftell", f, 0, 0, 0) == 16, "fseek / ftell");
        memset(buf, 0, sizeof buf);
        STCHECK(st_call(c, "fread", SG(buf), 1, 32, f) == 7 && strcmp(buf, "abc+app") == 0, "fread to the end");
        STCHECK((int)st_call(c, "feof", f, 0, 0, 0) != 0 && (int)st_call(c, "ferror", f, 0, 0, 0) == 0, "feof / ferror after a short read");
        STCHECK((int)st_call(c, "fprintf", f, SG("x"), 0, 0) < 0 && (int)st_call(c, "ferror", f, 0, 0, 0) != 0, "writing to a read-only stream does not fail");
        uint64_t g = st_call(c, "freopen", SG("c5bstdio:/w.txt"), SG("r+"), f, 0);
        STCHECK(g == f, "freopen(\"r+\") does not return the same stream");
        if (g) {
            st_call(c, "fwrite", SG("99"), 1, 2, g);
            STCHECK((int)st_call(c, "fclose", g, 0, 0, 0) == 0 && st_host_text(p, "99-x-1.5\nsecond\nabc+app"), "update mode does not overwrite in place");
        }
    }
    f = st_call(c, "fopen", SG("c5bstdio:/w.txt"), SG("w"), 0, 0);
    STCHECK(f != 0, "fopen(\"w\") on an existing file");
    if (f) STCHECK((int)st_call(c, "fclose", f, 0, 0, 0) == 0 && st_host_text(p, ""), "\"w\" does not truncate (nn::fs::SetFileSize)");
    st_call(c, "_ZN2nn2fs7UnmountEPKc", SG("c5bstdio"), 0, 0, 0);
    DeleteFileA(p);
    RemoveDirectoryA(base);

    /* swprintf: musl's vfwprintf formats every number with an internal fprintf on its FILE */
    uint32_t w[40];
    memset(w, 0xee, sizeof w);
    r = st_call(c, "swprintf", SG(w), 32, SG(U"%d"), 123);
    STCHECK((int)r == 3 && st_wide_is(w, "123"), "swprintf %d");
    c->x[4] = 7;
    r = st_call(c, "swprintf", SG(w), 32, SG(U"%02d:%02d"), 5);
    STCHECK((int)r == 5 && st_wide_is(w, "05:07"), "swprintf %02d:%02d");
    c->v[0].d[0] = 0x400921f9f01b866eULL;
    r = st_call(c, "swprintf", SG(w), 32, SG(U"%5.2f"), 0);
    STCHECK((int)r == 5 && st_wide_is(w, " 3.14"), "swprintf %5.2f");
    c->x[4] = 9;
    r = st_call(c, "swprintf", SG(w), 32, SG(U"<%ls|%d>"), SG(U"wide"));
    STCHECK((int)r == 8 && st_wide_is(w, "<wide|9>"), "swprintf %ls %d");
    STCHECK(import_lookup("swprintf") == sdk_lifted("swprintf"), "swprintf is not the lifted musl code");
    printf("  stdio (musl lifted: printf fprintf fopen freopen fclose fread getc feof ferror fflush write; swprintf): %d cases, %d failed\n", cases, bad);
    #undef STCHECK
    return bad;
}

/* ================================================================ close5 (c5b): libc++ leftovers
 * basic_streambuf<char>::underflow [sdk 0x465c00], pbackfail [0x465c48], overflow [0x465d18] are each
 * `mov w0, #-1; ret` in the SDK: the handler sb_eof (x0 = 0xffffffff) is the same function.
 * std::bad_cast::~bad_cast() D2 [0x4c8138] is `b std::exception::~exception()` [0x4c7c08 = `ret`]: x0 unchanged,
 * which is exc_dtor (nncxxrt.c).
 * std::string exceptions: sdklift.py now binds libc++'s __throw_length_error / __throw_out_of_range to the host
 * handlers that construct and throw the exception (nncxxrt.c), and sdklift_rt.c registers the SDK's unwind tables. */
#include <setjmp.h>
#include "eh_runtime.h"
void register_unwind_imports(void);                      /* nnunwind.c: installs the real unwinder */
static jmp_buf g_c5b_raise_jmp;
static int g_c5b_raise_reason = -1;
static void c5b_raise(Ctx *c, int reason) { (void)c; g_c5b_raise_reason = reason; longjmp(g_c5b_raise_jmp, 1); }
int libc2_cxx_selftest(Ctx *c) {
    int bad = 0, cases = 0;
    #define CXCHECK(cond, text) do { cases++; if (!(cond)) { printf("  FAIL libc++: %s\n", text); bad++; } } while (0)
    static const char *const sb[3] = {"_ZNSt3__115basic_streambufIcNS_11char_traitsIcEEE9underflowEv", "_ZNSt3__115basic_streambufIcNS_11char_traitsIcEEE9pbackfailEi",
                                      "_ZNSt3__115basic_streambufIcNS_11char_traitsIcEEE8overflowEi"};
    for (int i = 0; i < 3; i++) {
        uint64_t a = sdk_symbol(sb[i]);
        ImportFn f = import_lookup(sb[i]);
        c->x[0] = 0x1234567812345678ULL; c->x[1] = 'x';
        if (f) f(c);
        CXCHECK(a && SR32(a) == 0x12800000u && SR32(a + 4) == 0xd65f03c0u && f && c->x[0] == 0xffffffffULL, sb[i]);
    }
    {
        uint64_t a = sdk_symbol("_ZNSt8bad_castD2Ev"), e = sdk_symbol("_ZNSt9exceptionD2Ev");
        ImportFn f = import_lookup("_ZNSt8bad_castD2Ev");
        c->x[0] = 0x1234567812345678ULL;
        if (f) f(c);
        CXCHECK(a && e && (SR32(a) & 0xfc000000u) == 0x14000000u && SR32(e) == 0xd65f03c0u && f && c->x[0] == 0x1234567812345678ULL, "~bad_cast (D2) is not `b ~exception` = ret with x0 unchanged");
    }
    /* std::string::reserve(n) [sdk 0x4a6c40]: n >= size_t(-16) -> __throw_length_error [call at 0x4a6da0] */
    static const char *const reserve = "_ZNSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEE7reserveEm";
    ImportFn f = sdk_lifted(reserve);
    CXCHECK(f != NULL, "std::string::reserve is not lifted");
    if (f) {
        uint64_t str[3] = {0, 0, 0}, sp = c->x[31];
        EhFunction fn;
        memset(&fn, 0, sizeof fn);
        CXCHECK(eh_find_function(sdk_symbol(reserve) + 0x160, &fn) && fn.start == sdk_symbol(reserve) && fn.fde != 0, "the SDK's unwind tables are not registered (no FDE for std::string::reserve)");
        c->x[0] = SG(str); c->x[1] = 100; f(c);
        CXCHECK((str[0] & 1) && (str[0] & ~1ULL) >= 100 && str[2] != 0, "control: reserve(100) did not allocate");
        eh_set_unwinder(c5b_raise);
        g_c5b_raise_reason = -1;
        volatile int thrown = 0;
        if (setjmp(g_c5b_raise_jmp) == 0) { c->x[0] = SG(str); c->x[1] = ~0ULL; f(c); } else thrown = 1;
        register_unwind_imports();
        uint64_t e = eh_in_flight(c);
        CXCHECK(thrown && g_c5b_raise_reason == EH_RAISE_THROW && e && strcmp(eh_type_name(e), "St12length_error") == 0, "reserve(-1) did not throw std::length_error into the exception runtime");
        CXCHECK(c->x[30] == sdk_symbol(reserve) + 0x164, "the throw does not come from the call site sdk 0x4a6da0");
        if (thrown && e) {                                   /* what a `catch (std::length_error &)` pad does with it */
            EhPad pad;
            memset(&pad, 0, sizeof pad);
            pad.kind = EH_HANDLER; pad.selector = 1; pad.exception = e; pad.adjusted = e;
            eh_enter_pad(c, &pad);
            uint64_t hdr = c->x[0];
            CXCHECK(hdr == e - 0x20 && c->x[1] == 1 && eh_in_flight(c) == 0, "pad entry state");
            uint64_t obj = st_call(c, "__cxa_begin_catch", hdr, 0, 0, 0);
            const char *what = NULL;
            if (obj == e) { c->x[0] = obj; call_guest(c, SR64(SR64(obj) + 0x10)); what = (const char *)(uintptr_t)c->x[0]; }
            CXCHECK(obj == e && what && strcmp(what, "basic_string") == 0, "the caught object is not a length_error with what() = \"basic_string\"");
            st_call(c, "__cxa_end_catch", 0, 0, 0, 0);
            CXCHECK(eh_in_flight(c) == 0, "an exception is still in flight after __cxa_end_catch");
        }
        c->x[31] = sp;
        c->x[0] = SG(str); sdk_lifted("_ZNSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEED1Ev")(c);
    }
    printf("  libc++ leftovers (streambuf defaults, ~bad_cast, length_error thrown from lifted std::string::reserve): %d cases, %d failed\n", cases, bad);
    #undef CXCHECK
    return bad;
}
