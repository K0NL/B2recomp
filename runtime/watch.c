/* [Recomps#1] Diagnosis: a hardware write watchpoint (x86 debug register DR0) on one guest address, on every host thread.
 *
 *   RECOMPS_WATCH=<offset>[,<length>]   offset from the start of the game heap (nn::os::SetMemoryHeapSize), hex;
 *                                       length 1, 2, 4 or 8 bytes (default 8). Armed when the heap is created.
 *
 * Every write to the watched bytes prints the value now there, the host instruction, the guest function it belongs to,
 * the guest function last entered on that thread and its return address (at most 400 reports). Threads that exist when
 * the watch is armed get the register from a helper thread; guest threads made later get it before they first run
 * (os_threads.c creates them suspended). Nothing happens without the variable. */
#include <stdio.h>
#include <stdlib.h>
#include <windows.h>
#include <tlhelp32.h>
#include "runtime.h"
#include "hostrt.h"

Ctx *current_guest_ctx(void);                            /* hostrt.c */

static uint64_t g_addr;
static int g_len = 8;
static volatile LONG g_reports;

static void apply(HANDLE th) {
    CONTEXT ctx;
    memset(&ctx, 0, sizeof ctx);
    ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
    if (!GetThreadContext(th, &ctx)) return;
    DWORD64 len = g_len == 1 ? 0 : g_len == 2 ? 1 : g_len == 4 ? 3 : 2;           /* DR7 LEN0 encoding */
    ctx.Dr0 = g_addr;
    ctx.Dr7 = (ctx.Dr7 & ~0xf0003ull) | 1ull | (1ull << 16) | (len << 18);       /* L0, RW0 = write */
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

static LONG CALLBACK on_exception(EXCEPTION_POINTERS *e) {
    if (e->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP || !(e->ContextRecord->Dr6 & 1)) return EXCEPTION_CONTINUE_SEARCH;
    e->ContextRecord->Dr6 = 0;
    if (InterlockedIncrement(&g_reports) <= 400) {
        Ctx *c = current_guest_ctx();
        uint64_t rip = e->ContextRecord->Rip, val = 0;
        memcpy(&val, (void *)(uintptr_t)g_addr, (size_t)g_len);
        printf("watch: [%llx] = %llx  written at host %llx = in f_%llx; thread %lu, last guest function f_%llx, lr %llx\n",
               (unsigned long long)g_addr, (unsigned long long)val, (unsigned long long)rip, (unsigned long long)guest_function_of(rip),
               GetCurrentThreadId(), c ? (unsigned long long)c->fn : 0ull, c ? (unsigned long long)c->x[30] : 0ull);
    }
    return EXCEPTION_CONTINUE_EXECUTION;
}

void watch_heap_created(uint64_t heap_base) {
    const char *e = getenv("RECOMPS_WATCH");
    if (!e || !e[0] || g_addr) return;
    g_addr = heap_base + strtoull(e, NULL, 16);
    const char *comma = strchr(e, ',');
    if (comma) { int l = atoi(comma + 1); if (l == 1 || l == 2 || l == 4 || l == 8) g_len = l; }
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
    printf("watch: armed on [%llx] (heap + %s), %d bytes, %d threads\n", (unsigned long long)g_addr, e, g_len, n);
}
