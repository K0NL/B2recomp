/* Track P round "four players" (2026-10-13): a sampling profiler of EVERY busy thread of the process. TEST ONLY.
 *   SSBU_PROF15=<seconds after the fight began>,<seconds sampled>[,<output file>]
 * Nothing runs without the variable. The fight is recognised by the game's own ready-go flag (FighterManager + 0xd2,
 * the flag debug_mode.c uses); `SSBU_PROF15=w<seconds from process start>,<seconds>` = wall time instead (menus).
 * One second before the window the CPU time of every thread is read; the 14 busiest are sampled round robin (suspend,
 * context, unwind, resume). Per thread and per FUNCTION (key = the guest address for lifted code, else the start of the
 * host function from the unwind tables, as module + offset) the number of samples in which the function was the
 * innermost frame ("self") and in which it was anywhere on the stack ("incl") is counted; for innermost frames that are
 * not lifted code the direct caller and the nearest lifted function above are kept too. Names of host functions are
 * put in afterwards from the linker's map (tests\prof15_report.py). CPU ms per thread over the window is printed so
 * that sample shares can be turned into ms: a sample of a thread that is waiting has its innermost frame in ntdll /
 * win32u and is told apart there. No game content in this file. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <tlhelp32.h>
#include "runtime.h"
#include "hostrt.h"

uint64_t guest_function_of(uint64_t rip);            /* hostrt.c */
const char *guest_thread_name_of(DWORD tid);         /* os_threads.c */
uint64_t hook_address(const char *name);             /* fighter_sdk\hook_host.c (included by nnro.c) */

extern volatile LONG64 g_prof15_frame;               /* defined and written by display.c at every presented frame */
#define P15_THREADS 14
#define P15_TAB (1 << 17)
#define P15_PAIRS (1 << 16)
typedef struct { uint64_t key; unsigned char thr, used; unsigned self, incl; } P15Fn;
typedef struct { uint64_t leaf, parent, guest; unsigned char thr, used; unsigned n; } P15Pair;
static P15Fn *g_p15_fn;
static P15Pair *g_p15_pair;
static struct { DWORD tid; HANDLE h; ULONGLONG cpu0, cpu1; unsigned samples, waits; } g_p15_thr[P15_THREADS];
static int g_p15_nthr;
static struct { uint64_t base, size; char name[64]; } g_p15_mod[128];
static int g_p15_nmod;

static int p15_read(uint64_t addr, void *buf, size_t n) {
    SIZE_T got = 0;
    return ReadProcessMemory(GetCurrentProcess(), (const void *)(uintptr_t)addr, buf, n, &got) && got == n;
}
static int p15_fight_on(void) {
    static uint64_t inst;
    uint64_t mgr = 0, impl = 0;
    unsigned char go = 0;
    if (!inst) { inst = hook_address("_ZN3lib9SingletonIN3app14FighterManagerEE9instance_E"); if (!inst) return 0; }
    if (!p15_read(inst, &mgr, 8) || !mgr || !p15_read(mgr, &impl, 8) || !impl || !p15_read(impl + 0xd2, &go, 1)) return 0;
    return go != 0;
}
static ULONGLONG p15_cpu(HANDLE h) {
    FILETIME c, e, k, u;
    if (!GetThreadTimes(h, &c, &e, &k, &u)) return 0;
    return (((ULONGLONG)k.dwHighDateTime << 32) | k.dwLowDateTime) + (((ULONGLONG)u.dwHighDateTime << 32) | u.dwLowDateTime);
}
static void p15_modules(void) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, 0);
    MODULEENTRY32 me;
    if (snap == INVALID_HANDLE_VALUE) return;
    me.dwSize = sizeof me;
    for (BOOL ok = Module32First(snap, &me); ok && g_p15_nmod < 128; ok = Module32Next(snap, &me)) {
        g_p15_mod[g_p15_nmod].base = (uint64_t)(uintptr_t)me.modBaseAddr;
        g_p15_mod[g_p15_nmod].size = me.modBaseSize;
        snprintf(g_p15_mod[g_p15_nmod].name, sizeof g_p15_mod[0].name, "%s", me.szModule);
        g_p15_nmod++;
    }
    CloseHandle(snap);
}
static const char *p15_mod_of(uint64_t a, uint64_t *off) {
    for (int i = 0; i < g_p15_nmod; i++)
        if (a >= g_p15_mod[i].base && a - g_p15_mod[i].base < g_p15_mod[i].size) { *off = a - g_p15_mod[i].base; return g_p15_mod[i].name; }
    *off = a;
    return "?";
}
static void p15_count(int thr, uint64_t key, int self) {
    unsigned h = (unsigned)(((key >> 2) * 0x9E3779B97F4A7C15ull) >> 40) + (unsigned)thr * 7919u;
    for (int probe = 0; probe < 4096; probe++) {
        P15Fn *e = &g_p15_fn[(h + (unsigned)probe) & (P15_TAB - 1)];
        if (!e->used) { e->used = 1; e->key = key; e->thr = (unsigned char)thr; }
        if (e->key == key && e->thr == thr) { if (self) e->self++; else e->incl++; return; }
    }
}
static void p15_pair(int thr, uint64_t leaf, uint64_t parent, uint64_t guest) {
    unsigned h = (unsigned)((((leaf >> 2) ^ (parent << 7) ^ (guest << 17)) * 0x9E3779B97F4A7C15ull) >> 40) + (unsigned)thr * 7919u;
    for (int probe = 0; probe < 4096; probe++) {
        P15Pair *e = &g_p15_pair[(h + (unsigned)probe) & (P15_PAIRS - 1)];
        if (!e->used) { e->used = 1; e->leaf = leaf; e->parent = parent; e->guest = guest; e->thr = (unsigned char)thr; }
        if (e->leaf == leaf && e->parent == parent && e->guest == guest && e->thr == thr) { e->n++; return; }
    }
}
/* Guest keys are the guest addresses themselves (0x71.. main, 0x72.. modules, 0x75.. are host functions of the lifted
 * SDK and stay host keys); host keys are real addresses (always below 0x7100000000 is NOT guaranteed, so guest keys
 * get bit 63 set). */
#define P15_GUEST (1ull << 63)
static void p15_sample(int ti) {
    CONTEXT ctx;
    uint64_t keys[160];
    int k = 0;
    memset(&ctx, 0, sizeof ctx);
    ctx.ContextFlags = CONTEXT_FULL;
    if (SuspendThread(g_p15_thr[ti].h) == (DWORD)-1) return;
    if (GetThreadContext(g_p15_thr[ti].h, &ctx)) {
        for (int depth = 0; depth < 600 && k < 160; depth++) {
            uint64_t g = guest_function_of(ctx.Rip), key;
            DWORD64 base = 0;
            RUNTIME_FUNCTION *rf = RtlLookupFunctionEntry(ctx.Rip, &base, NULL);
            if (g) key = g | P15_GUEST;
            else key = rf ? base + rf->BeginAddress : ctx.Rip;
            if (k == 0 || keys[k - 1] != key) keys[k++] = key;
            if (!rf) {                                  /* a leaf function without unwind data: the return address is at the stack pointer */
                uint64_t ra = 0;
                if (depth > 0 || !p15_read(ctx.Rsp, &ra, 8) || !ra) break;
                ctx.Rip = ra; ctx.Rsp += 8;
                continue;
            }
            void *hd; DWORD64 est;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, ctx.Rip, rf, &ctx, &hd, &est, NULL);
            if (!ctx.Rip) break;
        }
    }
    ResumeThread(g_p15_thr[ti].h);
    if (!k) return;
    g_p15_thr[ti].samples++;
    p15_count(ti, keys[0], 1);
    for (int i = 0; i < k; i++) {
        int seen = 0;
        for (int j = 0; j < i && !seen; j++) seen = keys[j] == keys[i];
        if (!seen) p15_count(ti, keys[i], 0);
    }
    if (!(keys[0] & P15_GUEST)) {
        uint64_t guest = 0;
        for (int i = 1; i < k && !guest; i++) if (keys[i] & P15_GUEST) guest = keys[i];
        p15_pair(ti, keys[0], k > 1 ? keys[1] : 0, guest);
    }
}
static void p15_pick_threads(void) {
    static struct { DWORD tid; HANDLE h; ULONGLONG cpu; } all[768];
    int n = 0;
    DWORD pid = GetCurrentProcessId(), me = GetCurrentThreadId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 te;
    if (snap == INVALID_HANDLE_VALUE) return;
    te.dwSize = sizeof te;
    for (BOOL ok = Thread32First(snap, &te); ok && n < 768; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != pid || te.th32ThreadID == me) continue;
        HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
        if (!h) continue;
        all[n].tid = te.th32ThreadID; all[n].h = h; all[n].cpu = p15_cpu(h); n++;
    }
    CloseHandle(snap);
    Sleep(1000);
    for (int i = 0; i < n; i++) all[i].cpu = p15_cpu(all[i].h) - all[i].cpu;
    for (int t = 0; t < P15_THREADS; t++) {
        int best = -1;
        for (int i = 0; i < n; i++) if (all[i].h && all[i].cpu > 20000 && (best < 0 || all[i].cpu > all[best].cpu)) best = i;   /* over 2 ms in the second */
        if (best < 0) break;
        g_p15_thr[g_p15_nthr].tid = all[best].tid; g_p15_thr[g_p15_nthr].h = all[best].h; g_p15_nthr++;
        all[best].h = NULL;
    }
    for (int i = 0; i < n; i++) if (all[i].h) CloseHandle(all[i].h);
}
static DWORD WINAPI p15_thread(LPVOID arg) {
    const char *spec = arg;
    int wall = spec[0] == 'w', start = 0, dur = 0;
    char out[512] = "";
    if (sscanf(spec + wall, "%d,%d,%511s", &start, &dur, out) < 2 || dur <= 0) return 0;
    if (wall) Sleep((DWORD)start * 1000);
    else {
        int on = 0;
        while (on < start * 20) { Sleep(50); on = p15_fight_on() ? on + 1 : 0; }
    }
    g_p15_fn = VirtualAlloc(NULL, sizeof(P15Fn) * P15_TAB, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    g_p15_pair = VirtualAlloc(NULL, sizeof(P15Pair) * P15_PAIRS, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!g_p15_fn || !g_p15_pair) return 0;
    memset(g_p15_fn, 0, sizeof(P15Fn) * P15_TAB); memset(g_p15_pair, 0, sizeof(P15Pair) * P15_PAIRS);   /* touch every page now, not while a thread is suspended */
    p15_modules();
    p15_pick_threads();
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    LARGE_INTEGER f, t0, t;
    QueryPerformanceFrequency(&f); QueryPerformanceCounter(&t0);
    for (int i = 0; i < g_p15_nthr; i++) g_p15_thr[i].cpu0 = p15_cpu(g_p15_thr[i].h);
    unsigned rounds = 0;
    LONG64 frame0 = g_prof15_frame;
    for (;;) {
        for (int i = 0; i < g_p15_nthr; i++) p15_sample(i);
        rounds++;
        QueryPerformanceCounter(&t);
        if ((double)(t.QuadPart - t0.QuadPart) / (double)f.QuadPart >= (double)dur) break;
        Sleep(1);
    }
    for (int i = 0; i < g_p15_nthr; i++) g_p15_thr[i].cpu1 = p15_cpu(g_p15_thr[i].h);
    double secs = (double)(t.QuadPart - t0.QuadPart) / (double)f.QuadPart;
    FILE *fo = out[0] ? fopen(out, "w") : stdout;
    if (!fo) fo = stdout;
    fprintf(fo, "[p15] window %.2f s, %u rounds, %d threads, fight flag at the end: %d, frames %lld\n", secs, rounds, g_p15_nthr, p15_fight_on(), (long long)(g_prof15_frame - frame0));
    for (int i = 0; i < g_p15_nthr; i++) {
        const char *name = guest_thread_name_of(g_p15_thr[i].tid);
        fprintf(fo, "[p15] T %d tid %lu cpu_ms %.1f samples %u name %s\n", i, (unsigned long)g_p15_thr[i].tid,
                (double)(g_p15_thr[i].cpu1 - g_p15_thr[i].cpu0) / 10000.0, g_p15_thr[i].samples, name ? name : "host");
    }
    for (int j = 0; j < P15_TAB; j++) {
        P15Fn *e = &g_p15_fn[j];
        uint64_t off;
        if (!e->used) continue;
        if (e->key & P15_GUEST) fprintf(fo, "[p15] F %d G guest %llx %u %u\n", e->thr, (unsigned long long)(e->key & ~P15_GUEST), e->self, e->incl);
        else { const char *m = p15_mod_of(e->key, &off); fprintf(fo, "[p15] F %d H %s %llx %u %u\n", e->thr, m, (unsigned long long)off, e->self, e->incl); }
    }
    for (int j = 0; j < P15_PAIRS; j++) {
        P15Pair *e = &g_p15_pair[j];
        uint64_t o1, o2;
        if (!e->used) continue;
        const char *m1 = p15_mod_of(e->leaf, &o1);
        if (e->parent & P15_GUEST) fprintf(fo, "[p15] P %d %s %llx <- guest %llx | guest %llx : %u\n", e->thr, m1, (unsigned long long)o1,
                                           (unsigned long long)(e->parent & ~P15_GUEST), (unsigned long long)(e->guest & ~P15_GUEST), e->n);
        else { const char *m2 = p15_mod_of(e->parent, &o2); fprintf(fo, "[p15] P %d %s %llx <- %s %llx | guest %llx : %u\n", e->thr, m1, (unsigned long long)o1, m2,
                                                                     (unsigned long long)o2, (unsigned long long)(e->guest & ~P15_GUEST), e->n); }
    }
    if (fo != stdout) fclose(fo);
    printf("prof15: %u rounds over %.2f s written%s%s\n", rounds, secs, out[0] ? " to " : "", out);
    fflush(stdout);
    return 0;
}
void prof15_start(void) {
    const char *spec = getenv("SSBU_PROF15");
    if (spec && *spec) CloseHandle(CreateThread(NULL, 0, p15_thread, (LPVOID)spec, 0, NULL));
}
