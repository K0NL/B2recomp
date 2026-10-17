/* Hitch log (plan\phases\P3_performance.md, section 11): SSBU_HITCH=<ms> names, for every presented frame that took
 * longer than <ms>, which thread was late and inside what.
 *
 * How: a watchdog thread (time-critical priority) wakes about once per millisecond and looks at the main guest thread:
 *   - sleeping in the frame limiter                      -> "pace" (not a cost; never counted)
 *   - waiting for the render thread (gl_run)             -> "render: <what the render thread is doing>" (scope markers in nvn_gl.c)
 *   - inside an import or a run-time host function       -> "main: import <name>" (+ a note, e.g. the file being read);
 *                                                           "(blocked)" when the thread used no CPU during the sample, and then
 *                                                           the guest thread that did run is recorded as well
 *   - otherwise                                          -> "main: lifted code" (running / not running = preempted or in a hard page fault)
 * Each sample adds the time since the previous sample to its bucket; at every present the buckets of the frame are closed.
 * A frame over the threshold is printed with its buckets, the system-wide CPU use during the frame (so that load from
 * other programs can be told from our own cost) and the page faults of the process; at exit a table cause -> frames
 * where it was the largest part -> total ms is printed.
 * Costs nothing when the variable is not set (the markers are two stores; the watchdog is not started). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <psapi.h>
#include "runtime.h"
#include "hostrt.h"

int g_hitch_ms;                                   /* threshold; 0 = off */
volatile const char *g_hitch_rt = "idle";         /* render thread: current scope (nvn_gl.c) */
volatile uint64_t g_hitch_rt_arg;

typedef struct { DWORD tid; HANDLE h; Ctx *volatile ctx; volatile LONG wait_render, pacing; char note[160]; } HRec;
static HRec g_rec[256];
static volatile LONG g_nrec;
static _Thread_local HRec *t_rec;
static HRec *volatile g_main_rec, *volatile g_render_rec;

static HRec *rec_get(void) {
    if (t_rec) return t_rec;
    LONG i = InterlockedIncrement(&g_nrec) - 1;
    if (i >= 256) return NULL;
    g_rec[i].tid = GetCurrentThreadId();
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &g_rec[i].h, 0, FALSE, DUPLICATE_SAME_ACCESS);
    return t_rec = &g_rec[i];
}
/* The register context now running on this host thread (thread start, fiber switches). */
void hitch_cur_ctx(Ctx *c) { if (g_hitch_ms) { HRec *r = rec_get(); if (r) r->ctx = c; } }
void hitch_set_main(Ctx *c) { if (g_hitch_ms) { HRec *r = rec_get(); if (r) { r->ctx = c; g_main_rec = r; } } }
void hitch_set_render(void) { if (g_hitch_ms) g_render_rec = rec_get(); }
/* A detail for what this thread is doing inside an import (file name, module name); NULL clears it. */
void hitch_note(const char *s) {
    if (!g_hitch_ms) return;
    HRec *r = rec_get();
    if (!r) return;
    if (!s) { r->note[0] = 0; return; }
    size_t n = strlen(s);
    if (n > sizeof r->note - 1) { s += n - (sizeof r->note - 1); n = sizeof r->note - 1; }
    memcpy(r->note, s, n); r->note[n] = 0;
}
void hitch_wait_render(int on) { if (g_hitch_ms) { HRec *r = rec_get(); if (r) r->wait_render = on; } }
void hitch_pacing(int on) { if (g_hitch_ms) { HRec *r = rec_get(); if (r) r->pacing = on; } }

/* ---- buckets */
#define NB 96
typedef struct { char key[200]; double ms; } Bucket;
static Bucket g_cur[NB];
static int g_ncur;
static SRWLOCK g_lock;
typedef struct { char key[200]; double ms, worst; int frames, top; } Sum;
static Sum g_sum[2048], g_cat[128];
static int g_nsum, g_ncat;
static double g_hitch_total_ms, g_frames_seen, g_sample_gap_max;
static LONG64 g_hitch_frames;
static LARGE_INTEGER g_freq;

static void bucket_add(const char *key, double ms) {
    int i;
    for (i = 0; i < g_ncur && strcmp(g_cur[i].key, key); i++) {}
    if (i == g_ncur) { if (g_ncur == NB) i = NB - 1; else { g_ncur++; snprintf(g_cur[i].key, sizeof g_cur[i].key, "%s", key); g_cur[i].ms = 0; } }
    g_cur[i].ms += ms;
}
static Sum *sum_get(Sum *tab, int *n, int cap, const char *key) {
    int i;
    for (i = 0; i < *n && strcmp(tab[i].key, key); i++) {}
    if (i == *n) { if (*n == cap) return &tab[cap - 1]; (*n)++; memset(&tab[i], 0, sizeof tab[i]); snprintf(tab[i].key, sizeof tab[i].key, "%s", key); }
    return &tab[i];
}

/* Import name -> cause class of the brief (file read, module load, lock wait, allocation, exception, ...). */
static const char *import_class(const char *n) {
    if (strstr(n, "nvn")) return "graphics call (NVN entry point)";
    if (strstr(n, "2fs") || strstr(n, "fopen") || strstr(n, "fread")) return "file system";
    if (strstr(n, "2ro")) return "module load";
    if (strstr(n, "Unwind") || strstr(n, "__cxa_") || strstr(n, "__gxx")) return "exception";
    if (strstr(n, "mutex") || strstr(n, "Mutex") || strstr(n, "Lock") || strstr(n, "lock")) return "lock";
    if (strstr(n, "condition_variable") || strstr(n, "Wait") || strstr(n, "Event") || strstr(n, "Semaphore") || strstr(n, "MessageQueue") || strstr(n, "Sleep") || strstr(n, "Yield")) return "wait";
    if (strstr(n, "alloc") || strstr(n, "free") || !strncmp(n, "_Zn", 3) || !strncmp(n, "_Zd", 3) || strstr(n, "lmem") || strstr(n, "3mem") || strstr(n, "Heap") || strstr(n, "Memory")) return "memory allocation";
    if (strstr(n, "audio") || strstr(n, "Audio") || strstr(n, "opus") || strstr(n, "Opus")) return "audio";
    if (strstr(n, "Fiber")) return "fiber switch";
    if (strstr(n, "movie")) return "movie";
    return "other import";
}

int guest_thread_info(int i, Ctx **c, const char **name, HANDLE *h);          /* os_threads.c */
DWORD os_mutex_owner(uint64_t key);
static ULONG64 g_thr_cyc[256];

static DWORD WINAPI watchdog(LPVOID arg) {
    (void)arg;
    HANDLE timer = CreateWaitableTimerExW(NULL, NULL, 0x00000002 /* high resolution */, TIMER_ALL_ACCESS);
    LARGE_INTEGER prev, now;
    ULONG64 main_cyc = 0, rt_cyc = 0;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    QueryPerformanceCounter(&prev);
    for (;;) {
        LARGE_INTEGER due;
        due.QuadPart = -10000;                    /* 1 ms */
        if (timer && SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE)) WaitForSingleObject(timer, 100); else Sleep(1);
        QueryPerformanceCounter(&now);
        double dt = (double)(now.QuadPart - prev.QuadPart) * 1000.0 / (double)g_freq.QuadPart;
        prev = now;
        HRec *m = g_main_rec, *r = g_render_rec;
        if (!m || !m->ctx) continue;
        if (dt > g_sample_gap_max) g_sample_gap_max = dt;
        char key[200];
        ULONG64 cyc = 0, rc = 0;
        QueryThreadCycleTime(m->h, &cyc);
        int running = cyc - main_cyc > 20000;     /* more than a few microseconds of CPU since the last sample */
        main_cyc = cyc;
        if (r) QueryThreadCycleTime(r->h, &rc);
        int rt_running = rc - rt_cyc > 20000;
        rt_cyc = rc;
        Ctx *c = m->ctx;
        const char *imp = c->in_import;
        int blocked_scan = 0;
        extern volatile LONG g_display_holding;      /* display.c, session 14: the finished frame is held for its tick on the render thread */
        if (m->pacing || (m->wait_render && g_display_holding)) snprintf(key, sizeof key, "pace");
        else if (m->wait_render) {
            const char *s = (const char *)g_hitch_rt;
            uint64_t a = g_hitch_rt_arg;
            if (a) snprintf(key, sizeof key, "render: %s%s|%llx", s, rt_running ? "" : " (render thread not on a CPU: waiting for the driver or preempted)", (unsigned long long)a);
            else snprintf(key, sizeof key, "render: %s%s", s, rt_running ? "" : " (render thread not on a CPU: waiting for the driver or preempted)");
        } else if (imp) {
            if (running) snprintf(key, sizeof key, "main: %s: %s|%s", import_class(imp), imp, m->note);
            else snprintf(key, sizeof key, "main: %s: %s (blocked)|%s called from %llx", import_class(imp), imp, m->note, (unsigned long long)c->import_from);
            blocked_scan = !running;
        } else snprintf(key, sizeof key, "main: lifted code%s|f_%llx", running ? "" : " (not on a CPU: preempted or page fault)", (unsigned long long)c->fn);
        AcquireSRWLockExclusive(&g_lock);
        bucket_add(key, dt);
        if (blocked_scan && (strstr(imp, "mutex") || strstr(imp, "Mutex"))) {      /* round 3: who owns the mutex, and what is the owner inside? */
            DWORD owner = os_mutex_owner(c->x[0]);
            int found = 0;
            for (int i = 0; owner && i < 256; i++) {
                Ctx *tc; const char *name; HANDLE h;
                if (!guest_thread_info(i, &tc, &name, &h)) break;
                if (!h || GetThreadId(h) != owner) continue;
                const char *ti = tc->in_import;
                if (ti) snprintf(key, sizeof key, "  (owner of mutex %llx) %s: %s called from %llx", (unsigned long long)c->x[0], name ? name : "?", ti, (unsigned long long)tc->import_from);
                else snprintf(key, sizeof key, "  (owner of mutex %llx) %s: lifted code f_%llx", (unsigned long long)c->x[0], name ? name : "?", (unsigned long long)tc->fn);
                bucket_add(key, dt); found = 1;
                break;
            }
            if (!found) { snprintf(key, sizeof key, "  (owner of mutex %llx) host thread %lu%s", (unsigned long long)c->x[0], (unsigned long)owner, owner ? " (not a guest thread)" : " (no owner at the sample)"); bucket_add(key, dt); }
        }
        if (blocked_scan) {                       /* who ran while the main thread stood still? */
            for (int i = 0; i < 256; i++) {
                Ctx *tc; const char *name; HANDLE h;
                if (!guest_thread_info(i, &tc, &name, &h)) break;
                ULONG64 q = 0;
                if (!h || !QueryThreadCycleTime(h, &q)) continue;
                int ran = q - g_thr_cyc[i] > 200000;
                g_thr_cyc[i] = q;
                if (!ran) continue;
                const char *ti = tc->in_import;
                snprintf(key, sizeof key, "  (meanwhile) %s: %s", name ? name : "?", ti ? ti : "lifted code");
                bucket_add(key, dt);
            }
        }
        ReleaseSRWLockExclusive(&g_lock);
    }
}

static int cmp_bucket(const void *a, const void *b) { double x = ((const Bucket *)a)->ms, y = ((const Bucket *)b)->ms; return x > y ? -1 : x < y; }
static int cmp_sum(const void *a, const void *b) { double x = ((const Sum *)a)->ms, y = ((const Sum *)b)->ms; return x > y ? -1 : x < y; }

/* Render thread, at every present: `ms` = time since the previous present; `battle` = the frame had compute dispatches. */
void hitch_frame(long long frame, double ms, int battle) {
    static FILETIME idle0, kern0, user0, pk0, pu0;
    static DWORD faults0;
    static int said;
    Bucket b[NB];
    int n;
    if (!g_hitch_ms) return;
    AcquireSRWLockExclusive(&g_lock);
    n = g_ncur; memcpy(b, g_cur, sizeof(Bucket) * (size_t)n); g_ncur = 0;
    ReleaseSRWLockExclusive(&g_lock);
    g_frames_seen++;
    FILETIME idle, kern, user, c0, e0, pk, pu;
    PROCESS_MEMORY_COUNTERS pmc;
    GetSystemTimes(&idle, &kern, &user);
    GetProcessTimes(GetCurrentProcess(), &c0, &e0, &pk, &pu);
    memset(&pmc, 0, sizeof pmc); pmc.cb = sizeof pmc;
    GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof pmc);
#define FT(x) (((ULONGLONG)(x).dwHighDateTime << 32) | (x).dwLowDateTime)
    double d_idle = (double)(FT(idle) - FT(idle0)), d_all = (double)(FT(kern) - FT(kern0)) + (double)(FT(user) - FT(user0)), d_us = (double)(FT(pk) - FT(pk0)) + (double)(FT(pu) - FT(pu0));
    DWORD d_faults = pmc.PageFaultCount - faults0;
    idle0 = idle; kern0 = kern; user0 = user; pk0 = pk; pu0 = pu; faults0 = pmc.PageFaultCount;
    if (ms <= (double)g_hitch_ms || frame < 3) return;
    qsort(b, (size_t)n, sizeof b[0], cmp_bucket);
    double counted = 0;
    int top = -1;
    for (int i = 0; i < n; i++) if (strcmp(b[i].key, "pace") && b[i].key[0] != ' ') { counted += b[i].ms; if (top < 0) top = i; }
    g_hitch_frames++; g_hitch_total_ms += ms;
    if (said < 4000) {
        said++;
        printf("hitch: frame %lld%s %.1f ms | system CPU %.0f%% busy, this process %.0f%% of it | %lu page faults |", frame, battle ? " (battle)" : "", ms,
               d_all > 0 ? 100.0 * (d_all - d_idle) / d_all : 0.0, d_all - d_idle > 0 ? 100.0 * d_us / (d_all - d_idle) : 0.0, (unsigned long)d_faults);
        for (int i = 0, shown = 0; i < n && shown < 7; i++) { if (b[i].ms < 1.5 && shown) continue; printf(" [%.1f ms %s]", b[i].ms, b[i].key); shown++; }
        printf("\n");
    }
    for (int i = 0; i < n; i++) {
        if (!strcmp(b[i].key, "pace")) continue;
        Sum *s = sum_get(g_sum, &g_nsum, 2048, b[i].key);
        s->ms += b[i].ms; s->frames++; if (b[i].ms > s->worst) s->worst = b[i].ms; if (i == top) s->top++;
        char cat[200];
        snprintf(cat, sizeof cat, "%s%s", battle ? "battle | " : "other  | ", b[i].key);
        char *bar = strchr(cat + 9, '|');
        if (bar) *bar = 0;
        if (!strncmp(cat + 9, "main: ", 6) && strstr(cat + 15, ": ")) { char *q = strstr(cat + 15, ": "); const char *blk = strstr(q, " (blocked)"); if (blk) memmove(q, blk, strlen(blk) + 1); else *q = 0; }   /* class only */
        s = sum_get(g_cat, &g_ncat, 128, cat);
        s->ms += b[i].ms; s->frames++; if (b[i].ms > s->worst) s->worst = b[i].ms; if (i == top) s->top++;
    }
    (void)counted;
}

void cxxrt_readable_report(FILE *out);             /* nncxxrt.c */
void hitch_report(FILE *out) {
    if (!g_hitch_ms) return;
    cxxrt_readable_report(out);
    fprintf(out, "\nhitch log (SSBU_HITCH=%d): %lld of %.0f frames took more than %d ms, %.0f ms in all; longest gap between two samples of the watchdog %.1f ms\n", g_hitch_ms,
            (long long)g_hitch_frames, g_frames_seen, g_hitch_ms, g_hitch_total_ms, g_sample_gap_max);
    fprintf(out, "hitch table by cause (scene | cause -> frames where it was the largest part / frames where it appears -> total ms, worst single frame):\n");
    qsort(g_cat, (size_t)g_ncat, sizeof g_cat[0], cmp_sum);
    for (int i = 0; i < g_ncat; i++) fprintf(out, "hitch-cause: %-100s %4d / %4d frames %9.1f ms  worst %7.1f\n", g_cat[i].key, g_cat[i].top, g_cat[i].frames, g_cat[i].ms, g_cat[i].worst);
    fprintf(out, "hitch details (largest 60):\n");
    qsort(g_sum, (size_t)g_nsum, sizeof g_sum[0], cmp_sum);
    for (int i = 0; i < g_nsum && i < 60; i++) fprintf(out, "hitch-detail: %4d / %4d frames %9.1f ms  worst %7.1f  %s\n", g_sum[i].top, g_sum[i].frames, g_sum[i].ms, g_sum[i].worst, g_sum[i].key);
}

void hitch_init(void) {
    const char *e = getenv("SSBU_HITCH");
    QueryPerformanceFrequency(&g_freq);
    if (!e || atoi(e) <= 0) return;
    g_hitch_ms = atoi(e);
    InitializeSRWLock(&g_lock);
    CloseHandle(CreateThread(NULL, 0, watchdog, NULL, 0, NULL));
    printf("hitch: frames over %d ms are attributed (SSBU_HITCH)\n", g_hitch_ms);
}
