/* [Recomps#1] Diagnosis: a hardware write watchpoint (x86 debug register DR0) on one guest address, on every host thread.
 *
 *   RECOMPS_WATCH=<offset>[,<length>]   offset from the start of the game heap (nn::os::SetMemoryHeapSize), hex;
 *                                       length 1, 2, 4 or 8 bytes (default 8). Armed when the heap is created.
 *
 * Every write to the watched bytes prints the value now there, the host instruction, the guest function it belongs to,
 * the guest function last entered on that thread and its return address (at most 400 reports). Threads that exist when
 * the watch is armed get the register from a helper thread; guest threads made later get it before they first run
 * (os_threads.c creates them suspended). Nothing happens without the variable.
 * [Recomps#1] finding 118, TEST: RECOMPS_WATCH_ABS=<address>[,len] watches an absolute address (static game data) instead;
 * RECOMPS_WATCH_AFTER=<seconds> reports only later accesses; the first 30 reports print the guest frame-pointer chain.
 * watch_find_and_arm (nvn_vk_draw.c SSBU_VK_SHWATCH_PAT) finds a float pattern in the heap / main's data and watches READS
 * and writes of up to four matches (DR0..DR3); watch_arm_at arms on an address found at run time (SSBU_VK_SHWATCH). */
#include <stdio.h>
#include <stdlib.h>
#include <windows.h>
#include <tlhelp32.h>
#include "runtime.h"
#include "hostrt.h"

Ctx *current_guest_ctx(void);                            /* hostrt.c */

static uint64_t g_addr;
static int g_len = 8;
static int g_rw = 1;                                     /* DR7 RW0: 1 = writes, 3 = reads and writes (watch_find_and_arm) */
static uint64_t g_heap_lo, g_heap_hi;
static uint64_t g_more[3];                               /* DR1..DR3 (watch_find_and_arm: up to four matches at once), same length and kind */
static volatile LONG g_reports;
static LONGLONG g_after_qpc;                             /* [Recomps#1] TEST: RECOMPS_WATCH_AFTER=<seconds>: writes before then are not reported */

static void apply(HANDLE th) {
    CONTEXT ctx;
    memset(&ctx, 0, sizeof ctx);
    ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
    if (!GetThreadContext(th, &ctx)) return;
    DWORD64 len = g_len == 1 ? 0 : g_len == 2 ? 1 : g_len == 4 ? 3 : 2;           /* DR7 LEN0 encoding */
    ctx.Dr0 = g_addr;
    ctx.Dr7 = (ctx.Dr7 & ~0xf0003ull) | 1ull | ((DWORD64)g_rw << 16) | (len << 18);       /* L0, RW0 = write (or read / write) */
    for (int i = 0; i < 3; i++) if (g_more[i]) {
        if (i == 0) ctx.Dr1 = g_more[0]; else if (i == 1) ctx.Dr2 = g_more[1]; else ctx.Dr3 = g_more[2];
        ctx.Dr7 = (ctx.Dr7 & ~((0xfull << (20 + 4 * i)) | (3ull << (2 * (i + 1))))) | (1ull << (2 * (i + 1))) | ((DWORD64)g_rw << (20 + 4 * i)) | (len << (22 + 4 * i));
    }
    SetThreadContext(th, &ctx);
}

void watch_new_thread(HANDLE suspended_thread) { if (g_addr) apply(suspended_thread); }

static DWORD WINAPI apply_to(LPVOID arg) {               /* runs on a helper thread: the target cannot set its own DR */
    HANDLE th = (HANDLE)arg;
    SuspendThread(th);
    apply(th);
    ResumeThread(th);
    return 0;
}

static int readable(uint64_t a, size_t n) {                  /* committed, readable, not a guard page */
    MEMORY_BASIC_INFORMATION mi;
    if (!VirtualQuery((void *)(uintptr_t)a, &mi, sizeof mi) || mi.State != MEM_COMMIT || (mi.Protect & (PAGE_GUARD | PAGE_NOACCESS))) return 0;
    return a + n <= (uint64_t)(uintptr_t)mi.BaseAddress + mi.RegionSize;
}
static LONG CALLBACK on_exception(EXCEPTION_POINTERS *e) {
    if (e->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP || !(e->ContextRecord->Dr6 & 0xf)) return EXCEPTION_CONTINUE_SEARCH;
    DWORD64 hit = e->ContextRecord->Dr6 & 0xf;
    uint64_t addr = (hit & 1) ? g_addr : (hit & 2) ? g_more[0] : (hit & 4) ? g_more[1] : g_more[2];
    e->ContextRecord->Dr6 = 0;
    if (g_after_qpc) { LARGE_INTEGER t; QueryPerformanceCounter(&t); if (t.QuadPart < g_after_qpc) return EXCEPTION_CONTINUE_EXECUTION; }
    if (InterlockedIncrement(&g_reports) <= 400) {
        Ctx *c = current_guest_ctx();
        uint64_t rip = e->ContextRecord->Rip, val = 0;
        if (g_rw == 1) memcpy(&val, (void *)(uintptr_t)addr, (size_t)g_len);      /* a read watch would fire again on this read */
        printf("watch: [%llx] = %llx  accessed at host %llx = in f_%llx; thread %lu, last guest function f_%llx, lr %llx\n",
               (unsigned long long)addr, (unsigned long long)val, (unsigned long long)rip, (unsigned long long)guest_function_of(rip),
               GetCurrentThreadId(), c ? (unsigned long long)c->fn : 0ull, c ? (unsigned long long)c->x[30] : 0ull);
        if (c && g_reports <= 30) {                      /* [Recomps#1] the guest's frame-pointer chain: [x29] = caller's x29, [x29 + 8] = return address */
            uint64_t fp = c->x[29];
            printf("watch:   guest frames:");
            for (int k = 0; k < 12 && fp && !(fp & 7) && readable(fp, 16); k++) {
                uint64_t lr = *(const uint64_t *)(uintptr_t)(fp + 8);
                if (lr < 0x7100000000ull || lr >= 0x7200000000ull) break;
                printf(" %llx", (unsigned long long)lr);
                fp = *(const uint64_t *)(uintptr_t)fp;
            }
            printf("\n");
        }
        if (g_reports <= 30) {                           /* [Recomps#1] the guest functions on the host stack, as hostrt.c crash_filter walks it */
            CONTEXT ctx = *e->ContextRecord;
            uint64_t last = 0;
            printf("watch:   stack:");
            for (int depth = 0, shown = 0; depth < 200 && shown < 12; depth++) {
                uint64_t g = guest_function_of(ctx.Rip);
                if (g && g != last) { printf(" f_%llx", (unsigned long long)g); shown++; last = g; }
                DWORD64 image_base;
                RUNTIME_FUNCTION *rf = RtlLookupFunctionEntry(ctx.Rip, &image_base, NULL);
                if (!rf && depth == 0) { ctx.Rip = *(DWORD64 *)(uintptr_t)ctx.Rsp; ctx.Rsp += 8; continue; }
                if (!rf) break;
                void *hd; DWORD64 est;
                RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, ctx.Rip, rf, &ctx, &hd, &est, NULL);
                if (!ctx.Rip) break;
            }
            printf("\n");
        }
    }
    return EXCEPTION_CONTINUE_EXECUTION;
}

static void arm_all(const char *what);
void watch_heap_bounds(uint64_t base, uint64_t size) { g_heap_lo = base; g_heap_hi = base + size; }
/* [Recomps#1] TEST (shadow flicker): find the first place in the game heap holding the n floats of pat (committed pages only),
 * list up to 8 matches, and arm the watch for READS and writes on the float at index `at` of the first one. Once. */
void watch_find_and_arm(const float *pat, int n, int at, int which) {
    static volatile LONG once;
    if (g_addr || !g_heap_lo || InterlockedExchange(&once, 1)) return;
    uint64_t first = 0; int found = 0;
    const uint64_t ranges[2][2] = {{g_heap_lo, g_heap_hi}, {0x7101760000ull, 0x7101b60000ull}};      /* the game heap, then main's data / bss */
    for (int ri = 0; ri < 2; ri++) {
    uint64_t lo = ranges[ri][0], hi = ranges[ri][1];
    for (uint64_t a = lo; a < hi;) {
        MEMORY_BASIC_INFORMATION mi;
        if (!VirtualQuery((void *)(uintptr_t)a, &mi, sizeof mi)) break;
        uint64_t end = (uint64_t)(uintptr_t)mi.BaseAddress + mi.RegionSize;
        if (mi.State == MEM_COMMIT && (mi.Protect & (PAGE_READWRITE | PAGE_READONLY | PAGE_EXECUTE_READWRITE)) && !(mi.Protect & PAGE_GUARD))
            for (uint64_t p = a; p + 4 * (uint64_t)n <= end && p + 4 * (uint64_t)n <= hi; p += 4)
                if (!memcmp((void *)(uintptr_t)p, pat, 4 * (size_t)n)) { if (found < 40) printf("watch: pattern found at %llx (heap + %llx)\n", (unsigned long long)p, (unsigned long long)(p - g_heap_lo)); if (found == which) first = p; else if (found > which && found < which + 4 && at >= 0) g_more[found - which - 1] = p + 4 * (uint64_t)at; found++; }
        a = end;
    }
    }
    printf("watch: pattern of %d floats: %d places\n", n, found);
    if (!first) return;
    g_addr = first + 4 * (uint64_t)at; g_len = 4; g_rw = 3;
    arm_all("reads and writes, pattern match");
}
/* [Recomps#1] TEST: arm the watch once on an address found at run time (shadow flicker: SSBU_VK_SHWATCH, nvn_vk_draw.c) */
void watch_arm_at(uint64_t addr, int len) {
    static volatile LONG once;
    if (g_addr || InterlockedExchange(&once, 1)) return;
    g_addr = addr; g_len = len;
    arm_all("set at run time");
}
void watch_heap_created(uint64_t heap_base) {
    const char *e = getenv("RECOMPS_WATCH"), *abs = getenv("RECOMPS_WATCH_ABS");      /* [Recomps#1] _ABS: an absolute address (static game data) */
    if (abs && abs[0]) e = abs;
    if (!e || !e[0] || g_addr) return;
    g_addr = (abs && abs[0] ? 0 : heap_base) + strtoull(e, NULL, 16);
    const char *comma = strchr(e, ',');
    if (comma) { int l = atoi(comma + 1); if (l == 1 || l == 2 || l == 4 || l == 8) g_len = l; }
    if (getenv("RECOMPS_WATCH_RW") && atoi(getenv("RECOMPS_WATCH_RW")) == 3) g_rw = 3;      /* [Recomps#1] TEST: reads too */
    { const char *a = getenv("RECOMPS_WATCH_AFTER"); if (a && atof(a) > 0) { LARGE_INTEGER t, f; QueryPerformanceCounter(&t); QueryPerformanceFrequency(&f); g_after_qpc = t.QuadPart + (LONGLONG)(atof(a) * (double)f.QuadPart); } }
    arm_all(e);
}
static void arm_all(const char *what) {
    AddVectoredExceptionHandler(1, on_exception);
    DWORD me = GetCurrentThreadId(), pid = GetCurrentProcessId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 te = {sizeof te};
    int n = 0;
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != pid) continue;
        HANDLE th = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE, te.th32ThreadID);
        if (!th) continue;
        if (te.th32ThreadID == me) {                     /* our own thread: through a helper while we wait */
            HANDLE h = CreateThread(NULL, 0, apply_to, th, 0, NULL);
            WaitForSingleObject(h, INFINITE);
            CloseHandle(h);
        } else {
            SuspendThread(th); apply(th); ResumeThread(th);
        }
        CloseHandle(th);
        n++;
    }
    CloseHandle(snap);
    printf("watch: armed on [%llx] (%s), %d bytes, %d threads\n", (unsigned long long)g_addr, what, g_len, n);
}
