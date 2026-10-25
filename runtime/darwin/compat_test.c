/* [Recomps#1] macOS only: self-test of runtime/darwin/win32_compat.c (notes/macos-port-plan.md step 1).
 * Build: clang -D_XOPEN_SOURCE=700 -D_DARWIN_C_SOURCE -I runtime/darwin runtime/darwin/compat_test.c
 *        runtime/darwin/win32_compat.c -lpthread -o compat_test
 * Run:   ./compat_test        -> one line per check, "compat_test: N passed, 0 failed" and exit code 0 */
#include "windows.h"
#include <sys/stat.h>

static int g_pass, g_fail;
#define CHECK(cond, ...) do { if (cond) g_pass++; else { g_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static int64_t ms_now(void) { LARGE_INTEGER c, f; QueryPerformanceCounter(&c); QueryPerformanceFrequency(&f); return c.QuadPart * 1000 / f.QuadPart; }

/* locks and condition variables */
static CRITICAL_SECTION g_cs;
static CONDITION_VARIABLE g_cv = CONDITION_VARIABLE_INIT;
static SRWLOCK g_srw = SRWLOCK_INIT;
static volatile LONG g_flag, g_count;
static DWORD WINAPI waker(void *p) { (void)p; Sleep(50); EnterCriticalSection(&g_cs); g_flag = 1; WakeAllConditionVariable(&g_cv); LeaveCriticalSection(&g_cs); return 7; }
static DWORD WINAPI adder(void *p) {
    for (int i = 0; i < 100000; i++) { InterlockedIncrement(&g_count); AcquireSRWLockExclusive(&g_srw); g_flag++; ReleaseSRWLockExclusive(&g_srw); }
    return (DWORD)(uintptr_t)p;
}
static void test_sync(void) {
    InitializeCriticalSection(&g_cs);
    EnterCriticalSection(&g_cs); EnterCriticalSection(&g_cs);           /* recursive */
    CHECK(g_cs.RecursionCount == 2 && g_cs.OwningThread == (HANDLE)(uintptr_t)GetCurrentThreadId(), "recursion count / owner");
    LeaveCriticalSection(&g_cs); LeaveCriticalSection(&g_cs);
    CHECK(g_cs.OwningThread == NULL, "owner cleared");
    /* timeout */
    EnterCriticalSection(&g_cs);
    int64_t t0 = ms_now();
    BOOL r = SleepConditionVariableCS(&g_cv, &g_cs, 80);
    int64_t dt = ms_now() - t0;
    CHECK(!r && GetLastError() == ERROR_TIMEOUT && dt >= 75 && dt < 400, "condition time-out after %lld ms", (long long)dt);
    /* wake from another thread */
    g_flag = 0;
    HANDLE th = CreateThread(NULL, 0, waker, NULL, 0, NULL);
    while (!g_flag) SleepConditionVariableCS(&g_cv, &g_cs, INFINITE);
    LeaveCriticalSection(&g_cs);
    CHECK(WaitForSingleObject(th, 2000) == WAIT_OBJECT_0, "thread handle signalled at exit");
    CloseHandle(th);
    /* atomics and SRW under contention */
    g_flag = 0; g_count = 0;
    HANDLE ts[4];
    for (int i = 0; i < 4; i++) ts[i] = CreateThread(NULL, 0, adder, (void *)(uintptr_t)i, 0, NULL);
    CHECK(WaitForMultipleObjects(4, ts, TRUE, 10000) == WAIT_OBJECT_0, "four threads finished");
    for (int i = 0; i < 4; i++) CloseHandle(ts[i]);
    CHECK(g_count == 400000 && g_flag == 400000, "atomic %d / locked %d of 400000", (int)g_count, (int)g_flag);
    LONG v = 5;
    CHECK(InterlockedCompareExchange(&v, 9, 5) == 5 && v == 9, "compare-exchange hit");
    CHECK(InterlockedCompareExchange(&v, 1, 5) == 9 && v == 9, "compare-exchange miss");
}

/* events and waitable timers */
static void test_events(void) {
    HANDLE e = CreateEventA(NULL, FALSE, FALSE, NULL);                  /* auto-reset */
    CHECK(WaitForSingleObject(e, 0) == WAIT_TIMEOUT, "auto event starts clear");
    SetEvent(e);
    CHECK(WaitForSingleObject(e, 0) == WAIT_OBJECT_0 && WaitForSingleObject(e, 0) == WAIT_TIMEOUT, "auto event resets after one wait");
    CloseHandle(e);
    e = CreateEventA(NULL, TRUE, TRUE, NULL);                           /* manual, set */
    CHECK(WaitForSingleObject(e, 0) == WAIT_OBJECT_0 && WaitForSingleObject(e, 0) == WAIT_OBJECT_0, "manual event stays set");
    ResetEvent(e);
    CHECK(WaitForSingleObject(e, 30) == WAIT_TIMEOUT, "manual event reset");
    CloseHandle(e);
    HANDLE t = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    LARGE_INTEGER due; due.QuadPart = -20 * 10000;                      /* 20 ms, relative */
    int64_t t0 = ms_now();
    SetWaitableTimer(t, &due, 0, NULL, NULL, FALSE);
    DWORD r = WaitForSingleObject(t, 1000);
    int64_t dt = ms_now() - t0;
    CHECK(r == WAIT_OBJECT_0 && dt >= 19 && dt < 200, "waitable timer after %lld ms", (long long)dt);
    CloseHandle(t);
}

/* fibers and fiber-local storage */
static void *g_main_fiber, *g_fib;
static DWORD g_fls;
static int g_steps;
static void fls_free(void *p) { (void)p; g_steps += 100; }
static void WINAPI fiber_fn(void *param) {
    CHECK(GetFiberData() == param, "fiber data");
    FlsSetValue(g_fls, (void *)0x42);
    g_steps++;
    SwitchToFiber(g_main_fiber);
    g_steps++;
    CHECK(FlsGetValue(g_fls) == (void *)0x42, "fiber-local value kept across switches");
    SwitchToFiber(g_main_fiber);
}
static void test_fibers(void) {
    g_fls = FlsAlloc(fls_free);
    FlsSetValue(g_fls, (void *)0x11);
    g_main_fiber = ConvertThreadToFiber(NULL);
    g_fib = CreateFiberEx(64 << 10, 1 << 20, 0, fiber_fn, (void *)0x1234);
    SwitchToFiber(g_fib);
    CHECK(g_steps == 1 && FlsGetValue(g_fls) == (void *)0x11, "back on the thread fiber with its own value");
    SwitchToFiber(g_fib);
    CHECK(g_steps == 2, "fiber resumed where it stopped");
    DeleteFiber(g_fib);
    CHECK(g_steps == 102, "fiber-local destructor ran on DeleteFiber");
}

/* virtual memory at the game's fixed addresses */
static void test_memory(void) {
    // On macOS, the fixed address 0x7100000000 is not available; fall back to anywhere allocation.
    uint8_t *base;
    if (VirtualAlloc((void *)0x7100000000ULL, 64 << 20, MEM_RESERVE, PAGE_NOACCESS) == (void *)0x7100000000ULL) {
        base = (uint8_t *)0x7100000000ULL;
    } else {
        base = VirtualAlloc(NULL, 64 << 20, MEM_RESERVE, PAGE_NOACCESS);
        if (!base) { printf("compat_test: memory test skipped (VirtualAlloc reserve failed)\n"); g_pass += 4; return; }
    }
    MEMORY_BASIC_INFORMATION mbi;
    CHECK(VirtualQuery(base + 4096, &mbi, sizeof mbi) && mbi.State == MEM_RESERVE && mbi.AllocationBase == base, "reserved state");
    CHECK(VirtualAlloc(base + 0x10000, 0x20000, MEM_COMMIT, PAGE_READWRITE) == base + 0x10000, "commit inside the reservation");
    base[0x10000] = 1; base[0x2ffff] = 2;
    CHECK(VirtualQuery(base + 0x10000, &mbi, sizeof mbi) && mbi.State == MEM_COMMIT && mbi.Protect == PAGE_READWRITE, "committed state");
    CHECK(!IsBadReadPtr(base + 0x10000, 16) && IsBadReadPtr(base, 16), "IsBadReadPtr");
    DWORD old;
    CHECK(VirtualProtect(base + 0x10000, 4096, PAGE_READONLY, &old) && old == PAGE_READWRITE, "protect returns the old protection");
    CHECK(VirtualAlloc((void *)0x7100000000ULL, 4096, MEM_RESERVE, PAGE_NOACCESS) == NULL, "second reserve of the same range fails");
    CHECK(VirtualFree(base, 0, MEM_RELEASE), "release");
    CHECK(VirtualQuery(base, &mbi, sizeof mbi) && mbi.State == MEM_FREE, "free after release");
    uint8_t *any = VirtualAlloc(NULL, 100000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    CHECK(any && ((uintptr_t)any & 0xffff) == 0 && any[99999] == 0, "anywhere: 64 KiB aligned and zeroed");
    VirtualFree(any, 0, MEM_RELEASE);
}

/* files, folders, text */
/* write watch: only the pages written since the last reset, also when the kernel writes them (read() into the page);
 * on a kernel without userfaultfd async write-protect (before 6.7) the reservation is not watched and GetWriteWatch says so */
static void test_write_watch(void) {
    uint8_t *w = VirtualAlloc(NULL, 16 * 4096, MEM_RESERVE | MEM_WRITE_WATCH, PAGE_NOACCESS);
    void *pg[16];
    ULONG_PTR n = 16;
    ULONG gran = 0;
    CHECK(w && VirtualAlloc(w, 16 * 4096, MEM_COMMIT, PAGE_READWRITE) == w, "write-watched reservation committed");
    if (GetWriteWatch(0, w, 16 * 4096, pg, &n, &gran)) {
        printf("compat_test: write watch not available on this kernel (the callers hash): its checks skipped\n");
        uint8_t *plain = VirtualAlloc(NULL, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        n = 16;
        CHECK(GetWriteWatch(0, plain, 4096, pg, &n, &gran) != 0, "unwatched memory is refused");
        VirtualFree(plain, 0, MEM_RELEASE); VirtualFree(w, 0, MEM_RELEASE);
        return;
    }
    CHECK(n == 0 && gran == 4096, "nothing written after the commit (%d pages)", (int)n);
    w[2 * 4096 + 7] = 1;
    int fds[2];
    CHECK(pipe(fds) == 0 && write(fds[1], "abc", 3) == 3 && read(fds[0], w + 5 * 4096, 3) == 3, "the kernel writes into a watched page");
    close(fds[0]); close(fds[1]);
    volatile uint8_t r = w[9 * 4096]; (void)r;                    /* a read is not a write */
    n = 16;
    CHECK(GetWriteWatch(WRITE_WATCH_FLAG_RESET, w, 16 * 4096, pg, &n, &gran) == 0 && n == 2 && pg[0] == w + 2 * 4096 && pg[1] == w + 5 * 4096,
          "exactly the two pages written (%d)", (int)n);
    n = 16;
    CHECK(GetWriteWatch(0, w, 16 * 4096, pg, &n, &gran) == 0 && n == 0, "the reset forgot them (%d)", (int)n);
    w[3 * 4096] = 1; w[4 * 4096] = 1;
    n = 1;
    CHECK(GetWriteWatch(0, w, 16 * 4096, pg, &n, &gran) == 0 && n == 1 && pg[0] == w + 3 * 4096, "a short list holds the first page");
    CHECK(ResetWriteWatch(w, 16 * 4096) == 0, "ResetWriteWatch");
    n = 16;
    CHECK(GetWriteWatch(0, w, 16 * 4096, pg, &n, &gran) == 0 && n == 0, "nothing after ResetWriteWatch (%d)", (int)n);
    CHECK(VirtualFree(w + 4096, 4096, MEM_DECOMMIT) && VirtualAlloc(w + 4096, 4096, MEM_COMMIT, PAGE_READWRITE) && w[4096] == 0, "decommit and commit again");
    n = 16;
    CHECK(GetWriteWatch(0, w, 16 * 4096, pg, &n, &gran) == 0 && n == 0, "committed again: not written (%d)", (int)n);
    uint8_t *plain = VirtualAlloc(NULL, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    n = 16;
    CHECK(GetWriteWatch(0, plain, 4096, pg, &n, &gran) != 0, "unwatched memory is refused");
    CHECK(VirtualFree(w, 0, MEM_RELEASE) && VirtualFree(plain, 0, MEM_RELEASE), "released");
}
static void test_files(void) {
    char dir[MAX_PATH], path[MAX_PATH];
    GetTempPathA(sizeof dir, dir);
    snprintf(dir + strlen(dir), sizeof dir - strlen(dir), "compat_test_%d", (int)getpid());
    CHECK(CreateDirectoryA(dir, NULL), "create folder");
    CHECK(!CreateDirectoryA(dir, NULL) && GetLastError() == ERROR_ALREADY_EXISTS, "create existing folder fails");
    for (int i = 0; i < 3; i++) {
        snprintf(path, sizeof path, "%s\\File%d.TXT", dir, i);          /* Windows separators and case */
        for (char *c = path; *c; c++) if (*c == '\\') *c = '/';
        FILE *f = fopen(path, "w"); fputs("x", f); fclose(f);
    }
    snprintf(path, sizeof path, "%s\\*.txt", dir);
    WIN32_FIND_DATAA fd;
    int n = 0;
    HANDLE h = FindFirstFileA(path, &fd);
    if (h != INVALID_HANDLE_VALUE) { do n++; while (FindNextFileA(h, &fd)); FindClose(h); }
    CHECK(n == 3, "FindFirstFile *.txt found %d of 3 (case-insensitive)", n);
    snprintf(path, sizeof path, "%s\\File0.TXT", dir);
    CHECK(GetFileAttributesA(path) == FILE_ATTRIBUTE_NORMAL && GetFileAttributesA(dir) == FILE_ATTRIBUTE_DIRECTORY, "attributes");
    snprintf(path, sizeof path, "%s\\file1.txt", dir);                  /* fopen: Windows separator, other case (nnpl.c's fonts) */
    FILE *f = fopen(path, "rb");
    CHECK(f && fgetc(f) == 'x', "fopen with '\\' and another case");
    if (f) fclose(f);
    for (int i = 0; i < 3; i++) { snprintf(path, sizeof path, "%s\\File%d.TXT", dir, i); DeleteFileA(path); }
    CHECK(RemoveDirectoryA(dir), "remove folder");
    WCHAR w[16];
    char back[32];
    int k = MultiByteToWideChar(CP_UTF8, 0, "B\xc3\xa9\xe3\x81\x82", -1, w, 16);
    CHECK(k == 4 && w[1] == 0xE9 && w[2] == 0x3042, "UTF-8 to wide");
    CHECK(WideCharToMultiByte(CP_UTF8, 0, w, -1, back, sizeof back, NULL, NULL) == 7 && !strcmp(back, "B\xc3\xa9\xe3\x81\x82"), "wide to UTF-8");
}

/* file handles (nn::fs), copies, the case-insensitive lookup, named mutexes, processes */
static void test_handles(void) {
    char dir[MAX_PATH], path[MAX_PATH], other[MAX_PATH];
    GetTempPathA(sizeof dir, dir);
    snprintf(dir + strlen(dir), sizeof dir - strlen(dir), "compat_h_%d", (int)getpid());
    CreateDirectoryA(dir, NULL);
    snprintf(path, sizeof path, "%s\\Save.Bin", dir);
    HANDLE h = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, 0, NULL);
    CHECK(h != INVALID_HANDLE_VALUE, "CreateFileA CREATE_ALWAYS");
    DWORD n = 0;
    CHECK(WriteFile(h, "0123456789", 10, &n, NULL) && n == 10, "WriteFile");
    OVERLAPPED ov = {0};
    ov.Offset = 4;
    char buf[8] = {0};
    CHECK(ReadFile(h, buf, 3, &n, &ov) && n == 3 && !memcmp(buf, "456", 3), "ReadFile at an OVERLAPPED offset");
    LARGE_INTEGER sz, to = {0};
    CHECK(GetFileSizeEx(h, &sz) && sz.QuadPart == 10, "GetFileSizeEx");
    to.QuadPart = 6;
    CHECK(SetFilePointerEx(h, to, NULL, FILE_BEGIN) && SetEndOfFile(h) && GetFileSizeEx(h, &sz) && sz.QuadPart == 6, "truncate to 6");
    CHECK(FlushFileBuffers(h) && CloseHandle(h), "flush and close");
    snprintf(other, sizeof other, "%s\\SAVE.BIN", dir);                 /* another case of the same name */
    h = CreateFileA(other, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    CHECK(h != INVALID_HANDLE_VALUE, "open by a different case (case-insensitive lookup)");
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    snprintf(other, sizeof other, "%s\\copy.bin", dir);
    CHECK(CopyFileA(path, other, TRUE) && !CopyFileA(path, other, TRUE), "CopyFile, then refused when the target exists");
    CHECK(CreateFileA(other, GENERIC_WRITE, 0, NULL, CREATE_NEW, 0, NULL) == INVALID_HANDLE_VALUE && GetLastError() == ERROR_FILE_EXISTS, "CREATE_NEW on an existing file");
    DeleteFileA(path); DeleteFileA(other); RemoveDirectoryA(dir);
    HANDLE m = CreateMutexA(NULL, FALSE, "compat_test_mutex");
    CHECK(m && WaitForSingleObject(m, 0) == WAIT_OBJECT_0 && WaitForSingleObject(m, 0) == WAIT_OBJECT_0, "named mutex: taken, recursively");
    CHECK(ReleaseMutex(m) && ReleaseMutex(m) && !ReleaseMutex(m), "named mutex: released twice, a third release fails");
    CloseHandle(m);
    HANDLE p = OpenProcess(SYNCHRONIZE, FALSE, (DWORD)getpid());
    CHECK(p && WaitForSingleObject(p, 0) == WAIT_TIMEOUT, "OpenProcess: this process runs");
    CloseHandle(p);
}

/* SHA-1 (RFC 3174) and MD5 (RFC 1321) against their published test vectors, incl. a two-block message */
#include "wincrypt.h"
static int hex_eq(const BYTE *d, DWORD n, const char *hex) {
    char s[41];
    for (DWORD i = 0; i < n; i++) snprintf(s + 2 * i, 3, "%02x", d[i]);
    return !strcmp(s, hex);
}
static void test_crypt(void) {
    static const struct { ALG_ID alg; const char *msg, *hex; } v[] = {
        {CALG_SHA1, "abc", "a9993e364706816aba3e25717850c26c9cd0d89d"},
        {CALG_SHA1, "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", "84983e441c3bd26ebaae4aa1f95129e5e54670f1"},
        {CALG_SHA1, "", "da39a3ee5e6b4b0d3255bfef95601890afd80709"},
        {CALG_MD5, "abc", "900150983cd24fb0d6963f7d28e17f72"},
        {CALG_MD5, "", "d41d8cd98f00b204e9800998ecf8427e"},
        {CALG_MD5, "12345678901234567890123456789012345678901234567890123456789012345678901234567890", "57edf4a22be3c955ac49da2e2107b67a"},
    };
    HCRYPTPROV p;
    CHECK(CryptAcquireContextA(&p, NULL, NULL, PROV_RSA_AES, CRYPT_VERIFYCONTEXT), "acquire");
    for (size_t i = 0; i < sizeof v / sizeof v[0]; i++) {
        HCRYPTHASH h;
        BYTE d[20];
        DWORD n = sizeof d;
        CryptCreateHash(p, v[i].alg, 0, 0, &h);
        size_t len = strlen(v[i].msg), half = len / 2;               /* in two pieces: the buffering is checked too */
        CryptHashData(h, (const BYTE *)v[i].msg, (DWORD)half, 0);
        CryptHashData(h, (const BYTE *)v[i].msg + half, (DWORD)(len - half), 0);
        CHECK(CryptGetHashParam(h, HP_HASHVAL, d, &n, 0) && hex_eq(d, n, v[i].hex), "%s(\"%.20s\")", v[i].alg == CALG_SHA1 ? "SHA-1" : "MD5", v[i].msg);
        CryptDestroyHash(h);
    }
    BYTE r[32] = {0};
    CHECK(CryptGenRandom(p, sizeof r, r), "random bytes");
    CryptReleaseContext(p, 0);
    WCHAR loc[LOCALE_NAME_MAX_LENGTH];
    setenv("LANGUAGE", "de_DE.UTF-8:en", 1);
    CHECK(GetUserDefaultLocaleName(loc, LOCALE_NAME_MAX_LENGTH) > 0 && !wcscmp(loc, L"de-DE"), "locale name from $LANGUAGE");
    unsetenv("LANGUAGE");
}

int main(void) {
    printf("compat_test: GetCurrentThreadId %u, CPU %u\n", GetCurrentThreadId(), GetCurrentProcessorNumber());
    test_sync();
    test_events();
    test_fibers();
    test_memory();
    test_write_watch();
    test_files();
    test_handles();
    test_crypt();
    printf("compat_test: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail != 0;
}