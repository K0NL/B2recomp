/* [Recomps#1] macOS only: implementations behind runtime/darwin/windows.h (notes/macos-port-plan.md step 1).
 * Kernel objects (threads, events, waitable timers) are one handle type with a mutex, a condition and a "signalled"
 * flag; fibers are ucontext; virtual memory keeps its reservations in a table so MEM_RELEASE and VirtualQuery answer as
 * Windows does; paths accept '\\'. Never built on Windows. */
#include "windows.h"
#include "psapi.h"
#include <dirent.h>
#include <dlfcn.h>
#include <fnmatch.h>
#include <malloc/malloc.h>
#include <fcntl.h>
#include <signal.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <mach-o/nlist.h>
#include <mach/thread_info.h>
#include <mach/task_info.h>
#include <libproc.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <pwd.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/proc_info.h>
#include <ucontext.h>
#include <execinfo.h>

static size_t g_page;           /* Darwin page size (4 KiB on Intel, 16 KiB on Apple Silicon); initialized once in compat_args */

static size_t page_size(void) { return g_page; }
/* ---- process arguments and environment */
int __argc;
char **__argv;
__attribute__((constructor)) static void compat_args(int argc, char **argv, char **envp) { (void)envp; __argc = argc; __argv = argv; g_page = (size_t)sysconf(_SC_PAGESIZE); }
int _putenv(const char *kv) { return putenv(strdup(kv)) ? -1 : 0; }      /* putenv keeps the string: give it a copy */
char *_fullpath(char *out, const char *path, size_t n) {
    char tmp[PATH_MAX], *r;
    char p[PATH_MAX];
    snprintf(p, sizeof p, "%s", path);
    for (char *c = p; *c; c++) if (*c == '\\') *c = '/';
    r = realpath(p, tmp);
    if (!r) {                                            /* a path that does not exist yet: absolute by joining with the cwd */
        if (p[0] == '/') snprintf(tmp, sizeof tmp, "%s", p);
        else { char cwd[PATH_MAX]; if (!getcwd(cwd, sizeof cwd)) return NULL; snprintf(tmp, sizeof tmp, "%s/%s", cwd, p); }
    }
    if (!out) return strdup(tmp);
    if (strlen(tmp) + 1 > n) return NULL;
    memcpy(out, tmp, strlen(tmp) + 1);
    return out;
}

/* ---- time */
static int64_t now_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (int64_t)t.tv_sec * 1000000000 + t.tv_nsec; }
BOOL QueryPerformanceCounter(LARGE_INTEGER *c) { c->QuadPart = now_ns(); return TRUE; }
BOOL QueryPerformanceFrequency(LARGE_INTEGER *f) { f->QuadPart = 1000000000; return TRUE; }
ULONGLONG GetTickCount64(void) { return (ULONGLONG)(now_ns() / 1000000); }
void Sleep(DWORD ms) {
    if (!ms) { sched_yield(); return; }
    struct timespec t = { ms / 1000, (long)(ms % 1000) * 1000000 };
    while (nanosleep(&t, &t) && errno == EINTR) {}
}
void GetSystemTimeAsFileTime(FILETIME *ft) {             /* 100 ns units since 1601-01-01 */
    struct timespec t; clock_gettime(CLOCK_REALTIME, &t);
    uint64_t v = (uint64_t)t.tv_sec * 10000000u + (uint64_t)t.tv_nsec / 100u + 116444736000000000ull;
    ft->dwLowDateTime = (DWORD)v; ft->dwHighDateTime = (DWORD)(v >> 32);
}
static void st_of(const struct tm *t, long ms, SYSTEMTIME *st) {
    st->wYear = (WORD)(t->tm_year + 1900); st->wMonth = (WORD)(t->tm_mon + 1); st->wDayOfWeek = (WORD)t->tm_wday;
    st->wDay = (WORD)t->tm_mday; st->wHour = (WORD)t->tm_hour; st->wMinute = (WORD)t->tm_min; st->wSecond = (WORD)t->tm_sec;
    st->wMilliseconds = (WORD)ms;
}
void GetLocalTime(SYSTEMTIME *st) { struct timespec t; struct tm m; clock_gettime(CLOCK_REALTIME, &t); localtime_r(&t.tv_sec, &m); st_of(&m, t.tv_nsec / 1000000, st); }
void GetSystemTime(SYSTEMTIME *st) { struct timespec t; struct tm m; clock_gettime(CLOCK_REALTIME, &t); gmtime_r(&t.tv_sec, &m); st_of(&m, t.tv_nsec / 1000000, st); }
DWORD GetCurrentProcessorNumber(void) { return 0; }        /* [Recomps#1] Darwin has no public current-CPU API; diagnostics only; see plan step 3 */
static struct timespec deadline_ms(DWORD ms) {           /* CLOCK_MONOTONIC deadline ms from now */
    int64_t d = now_ns() + (int64_t)ms * 1000000;
    return (struct timespec){ d / 1000000000, d % 1000000000 };
}
/* [Recomps#1] ts_of removed: Apple condition variables are CLOCK_REALTIME; the waits above pass a RELATIVE span now */

/* ---- last error */
static __thread DWORD t_last_error;
DWORD GetLastError(void) { return t_last_error; }
void SetLastError(DWORD e) { t_last_error = e; }

/* ---- critical sections, condition variables, init-once */
void InitializeCriticalSection(CRITICAL_SECTION *c) {
    c->LockCount = -1;
    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&c->m, &a);
    pthread_mutexattr_destroy(&a);
    c->ready = 1;
}
/* release(lock) after the counter is read under the internal mutex; reacquire(lock) after the wait */
/* [Recomps#1] TEST ONLY (SSBU_CVDIAG=1): how often the waits are entered and woken - the slowness diagnosis */
static volatile LONG64 g_cvs_n, g_cvs_inf, g_cvs_timed, g_wfso_n, g_setev_n, g_cvs_iter;
static volatile LONG g_cvs_on;
static void *cvs_tick(void *unused) {
    (void)unused;
    for (;;) {
        Sleep(5000);
        LONG64 n = InterlockedExchange64(&g_cvs_n, 0), inf = InterlockedExchange64(&g_cvs_inf, 0), t = InterlockedExchange64(&g_cvs_timed, 0),
              w = InterlockedExchange64(&g_wfso_n, 0), s = InterlockedExchange64(&g_setev_n, 0), it = InterlockedExchange64(&g_cvs_iter, 0);
        printf("[cvs] %lld cond waits/s (inf %lld, timed %lld), %lld iters/s, %lld WaitForSingleObject/s, %lld SetEvent/s\n", n / 5, inf / 5, t / 5, it / 5, w / 5, s / 5);
        fflush(stdout);
    }
}
static BOOL cv_sleep(CONDITION_VARIABLE *v, DWORD ms, void (*release)(void *), void (*acquire)(void *), void *lock) {
    { static LONG once; if (!InterlockedExchange(&once, 1)) if (getenv("SSBU_CVDIAG") && atoi(getenv("SSBU_CVDIAG"))) { InterlockedExchange(&g_cvs_on, 1); CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)(void *)cvs_tick, NULL, 0, NULL); } }
    if (g_cvs_on) { InterlockedIncrement64(&g_cvs_n); InterlockedIncrement64(ms == INFINITE ? &g_cvs_inf : &g_cvs_timed); }
    int64_t until = ms == INFINITE ? INT64_MAX : now_ns() + (int64_t)ms * 1000000;
    int timed_out = 0;
    pthread_mutex_lock(&v->m);
    uint64_t seq = v->seq;
    release(lock);
    while (v->seq == seq) {
        if (g_cvs_on) InterlockedIncrement64(&g_cvs_iter);
        if (ms == INFINITE) pthread_cond_wait(&v->c, &v->m);
        else {
            int64_t left = until - now_ns();
            if (left <= 0) { timed_out = v->seq == seq; break; }
            struct timespec rel = { left / 1000000000, left % 1000000000 };
            if (pthread_cond_timedwait_relative_np(&v->c, &v->m, &rel) == ETIMEDOUT) { timed_out = v->seq == seq; break; }
        }
    }
    pthread_mutex_unlock(&v->m);
    acquire(lock);
    if (timed_out) { SetLastError(ERROR_TIMEOUT); return FALSE; }
    return TRUE;
}
static void cs_release(void *l) { LeaveCriticalSection(l); }
static void cs_acquire(void *l) { EnterCriticalSection(l); }
static void srw_release_x(void *l) { ReleaseSRWLockExclusive(l); }   /* [Recomps#1] the kind is known: the flag below says which */
static void srw_acquire_x(void *l) { AcquireSRWLockExclusive(l); }
static void srw_release_s(void *l) { ReleaseSRWLockShared(l); }
static void srw_acquire_s(void *l) { AcquireSRWLockShared(l); }
BOOL SleepConditionVariableCS(CONDITION_VARIABLE *v, CRITICAL_SECTION *c, DWORD ms) { return cv_sleep(v, ms, cs_release, cs_acquire, c); }
BOOL SleepConditionVariableSRW(CONDITION_VARIABLE *v, SRWLOCK *s, DWORD ms, ULONG flags) {
    return (flags & CONDITION_VARIABLE_LOCKMODE_SHARED)
        ? cv_sleep(v, ms, srw_release_s, srw_acquire_s, s)
        : cv_sleep(v, ms, srw_release_x, srw_acquire_x, s);
}
static pthread_mutex_t g_once_m = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_once_c = PTHREAD_COND_INITIALIZER;
BOOL InitOnceExecuteOnce(INIT_ONCE *o, PINIT_ONCE_FN fn, void *param, void **ctx) {    /* state: 0 new, 1 running, 2 done */
    if (__atomic_load_n(&o->state, __ATOMIC_ACQUIRE) == 2) return TRUE;
    pthread_mutex_lock(&g_once_m);
    while (o->state == 1) pthread_cond_wait(&g_once_c, &g_once_m);
    if (o->state == 2) { pthread_mutex_unlock(&g_once_m); return TRUE; }
    o->state = 1;
    pthread_mutex_unlock(&g_once_m);
    BOOL ok = fn(o, param, ctx);
    pthread_mutex_lock(&g_once_m);
    __atomic_store_n(&o->state, ok ? 2 : 0, __ATOMIC_RELEASE);
    pthread_cond_broadcast(&g_once_c);
    pthread_mutex_unlock(&g_once_m);
    return ok;
}

/* ---- kernel objects: threads, events, waitable timers */
enum { H_THREAD = 1, H_EVENT, H_TIMER, H_MUTEX, H_PROC, H_MAP, H_SEM };
typedef struct Obj {
    int kind, manual;
    volatile int refs;
    pthread_mutex_t m;
    pthread_cond_t c;
    int signaled;
    /* thread */
    pthread_t th;
    LPTHREAD_START_ROUTINE fn;
    void *arg;
    volatile DWORD tid;
    int suspended;
    DWORD exit_code;
    /* timer */
    int armed;
    int64_t due, period;
    /* mutex (fd >= 0: named, a lock file), process, file mapping */
    int fd, owned;
    DWORD owner;
    pid_t pid;
    uint64_t size;
    int map_write;
} Obj;
#define IS_FH(h) ((h) != INVALID_HANDLE_VALUE && ((intptr_t)(h) & 3) == 3)   /* a file handle: fd << 2 | 3 */
#define FH_FD(h) ((int)((intptr_t)(h) >> 2))
#define FH_OF(fd) ((HANDLE)(((intptr_t)(fd) << 2) | 3))
#define PSEUDO_THREAD ((HANDLE)(intptr_t)-2)
static Obj *obj_new(int kind) {
    Obj *o = calloc(1, sizeof *o);
    o->kind = kind; o->refs = 1; o->fd = -1;
    pthread_mutex_init(&o->m, NULL);
    pthread_condattr_t a; pthread_condattr_init(&a); /* [Recomps#1] Darwin cond attr has no setclock; relative_np used */
    pthread_cond_init(&o->c, &a); pthread_condattr_destroy(&a);
    return o;
}
static void obj_unref(Obj *o) {
    if (__atomic_sub_fetch(&o->refs, 1, __ATOMIC_ACQ_REL)) return;
    if (o->fd >= 0) close(o->fd);
    pthread_mutex_destroy(&o->m); pthread_cond_destroy(&o->c); free(o);
}
DWORD GetCurrentThreadId(void) { uint64_t tid; pthread_threadid_np(NULL, &tid); return (DWORD)tid; }
/* [Recomps#1] TEST ONLY (SSBU_TCDIAG=<seconds>): the N busiest threads by CPU over the window, with a stack sample of
 * each - the slowness diagnosis on macOS, where task_for_pid is not available to the surrounding tools. */
static void *tcdiag_tick(void *unused) {
    (void)unused;
    int win = 5;
    for (;;) {
        static LxThread a[400], b[400];
        int na = lx_threads(a, 400);
        Sleep((DWORD)win * 1000);
        int nb = lx_threads(b, 400);
        static struct TcRow { DWORD tid; uint64_t d; char comm[20]; } top[400];
        int nt = 0;
        for (int i = 0; i < nb; i++) {
            uint64_t d = 0;
            for (int j = 0; j < na; j++) if (a[j].tid == b[i].tid) { d = b[i].cpu_ns > a[j].cpu_ns ? b[i].cpu_ns - a[j].cpu_ns : 0; break; }
            if (d >= 100000000ull) { top[nt].tid = b[i].tid; top[nt].d = d; snprintf(top[nt].comm, sizeof top[nt].comm, "%s", b[i].comm); nt++; }
        }
        for (int i = 1; i < nt; i++) for (int j = i; j > 0 && top[j - 1].d < top[j].d; j--) {
            struct TcRow t = top[j - 1]; top[j - 1] = top[j]; top[j] = t;
        }
        uint64_t total = 0;
        for (int i = 0; i < nt; i++) total += top[i].d;
        printf("[tc] %d threads over %d ms: %.2f cores\n", nt, win * 1000, (double)total / 1e9 / win);
        {   const struct mach_header *mh = _dyld_get_image_header(0);      /* the main image's load address, for atos */
            printf("[tc] base game_native %p  game.dylib %p\n", (void *)mh, dlsym(RTLD_DEFAULT, "guest_function_of") ? (void *)_dyld_get_image_header(1) : NULL); }
        uint64_t (*gfo)(uint64_t) = (uint64_t (*)(uint64_t))dlsym(RTLD_DEFAULT, "guest_function_of");
        for (int i = 0; i < nt; i++) {
            uint64_t pc[14];
            int nf = lx_thread_stack(top[i].tid, pc, 14);
            printf("[tc] %6.1f%% of a core  tid %lu  %s\n", (double)top[i].d / (win * 10000000.0), (unsigned long)top[i].tid, top[i].comm);
            for (int f = 0; f < nf; f++) {
                uint64_t g = gfo ? gfo(pc[f]) : 0;
                Dl_info di;
                const char *img = (dladdr((void *)(uintptr_t)pc[f], &di) && di.dli_fname) ? strrchr(di.dli_fname, '/') + 1 : "?";
                printf("[tc]     %llx%s%llx %s+0x%llx\n", (unsigned long long)pc[f], g ? " guest f_" : " ", g ? (unsigned long long)g : 0, img,
                       (unsigned long long)(di.dli_saddr ? pc[f] - (uint64_t)(uintptr_t)di.dli_saddr : 0));
            }
        }
        fflush(stdout);
    }
}
static void tcdiag_maybe_start(void) {
    static LONG started;
    const char *e;
    if (InterlockedExchange(&started, 1)) return;
    if ((e = getenv("SSBU_TCDIAG")) && atoi(e) > 0) CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)(void *)tcdiag_tick, NULL, 0, NULL);
}


static void *thread_main(void *p) {
    Obj *o = p;
    tcdiag_maybe_start();                                   /* [Recomps#1] TEST ONLY: SSBU_TCDIAG (defined below) */
    pthread_mutex_lock(&o->m);
    o->tid = GetCurrentThreadId();
    pthread_cond_broadcast(&o->c);
    while (o->suspended) pthread_cond_wait(&o->c, &o->m);
    pthread_mutex_unlock(&o->m);
    DWORD r = o->fn(o->arg);
    pthread_mutex_lock(&o->m);
    o->exit_code = r; o->signaled = 1;
    pthread_cond_broadcast(&o->c);
    pthread_mutex_unlock(&o->m);
    obj_unref(o);                                        /* the thread's own reference */
    return NULL;
}
HANDLE CreateThread(SECURITY_ATTRIBUTES *sa, SIZE_T stack, LPTHREAD_START_ROUTINE fn, void *arg, DWORD flags, DWORD *tid) {
    (void)sa;
    Obj *o = obj_new(H_THREAD);
    o->fn = fn; o->arg = arg; o->manual = 1;
    o->suspended = (flags & CREATE_SUSPENDED) != 0;
    o->refs = 2;                                         /* the caller's handle + the running thread */
    pthread_attr_t a;
    pthread_attr_init(&a);
    if (stack < (SIZE_T)(1 << 20)) stack = 1 << 20;       /* Windows reserves 1 MiB by default; never less */
    pthread_attr_setstacksize(&a, stack);
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    int e = pthread_create(&o->th, &a, thread_main, o);
    pthread_attr_destroy(&a);
    if (e) { free(o); SetLastError((DWORD)e); return NULL; }
    pthread_mutex_lock(&o->m);
    while (!o->tid) pthread_cond_wait(&o->c, &o->m);     /* the id is known when CreateThread returns, as on Windows */
    pthread_mutex_unlock(&o->m);
    if (tid) *tid = o->tid;
    return o;
}
DWORD ResumeThread(HANDLE h) {
    Obj *o = h;
    if (!h || h == PSEUDO_THREAD || h == INVALID_HANDLE_VALUE || o->kind != H_THREAD) return (DWORD)-1;
    pthread_mutex_lock(&o->m);
    DWORD was = (DWORD)o->suspended;
    o->suspended = 0;
    pthread_cond_broadcast(&o->c);
    pthread_mutex_unlock(&o->m);
    return was;
}
HANDLE GetCurrentThread(void) { return PSEUDO_THREAD; }
DWORD GetThreadId(HANDLE h) { return h == PSEUDO_THREAD ? GetCurrentThreadId() : ((Obj *)h)->tid; }
static pid_t tid_of(HANDLE h) { return h == PSEUDO_THREAD ? (pid_t)GetCurrentThreadId() : (pid_t)((Obj *)h)->tid; }
BOOL SetThreadPriority(HANDLE h, int prio) {
    /* nice values: lowering works for anyone; raising needs CAP_SYS_NICE (or an RLIMIT_NICE), so it is tried and
     * a refusal is not an error (the runtime only hints with priorities) */
    int nice = prio >= THREAD_PRIORITY_TIME_CRITICAL ? -10 : -prio * 2;
    setpriority(PRIO_PROCESS, (id_t)tid_of(h), nice);
    return TRUE;
}
DWORD_PTR SetThreadAffinityMask(HANDLE h, DWORD_PTR mask) {
    (void)h; (void)mask;                                     /* [Recomps#1] Darwin has no public per-thread CPU pinning; only cluster hints via thread_policy_set */
    return 0;                                                /* previous mask (not tracked); caller expects nonzero on success */
}
BOOL GetProcessAffinityMask(HANDLE p, DWORD_PTR *proc, DWORD_PTR *sys) {
    (void)p;
    int n = sysconf(_SC_NPROCESSORS_ONLN);
    DWORD_PTR m = n > 0 ? ((DWORD_PTR)1 << n) - 1 : 1;
    if (proc) *proc = m;
    if (sys) *sys = m;
    return TRUE;
}
HANDLE CreateEventA(SECURITY_ATTRIBUTES *sa, BOOL manual, BOOL initial, LPCSTR name) {
    (void)sa; (void)name;
    Obj *o = obj_new(H_EVENT);
    o->manual = manual; o->signaled = initial != 0;
    return o;
}
BOOL SetEvent(HANDLE h) {
    Obj *o = h;
    if (g_cvs_on) InterlockedIncrement64(&g_setev_n);
    pthread_mutex_lock(&o->m); o->signaled = 1; pthread_cond_broadcast(&o->c); pthread_mutex_unlock(&o->m);
    return TRUE;
}
BOOL ResetEvent(HANDLE h) { Obj *o = h; pthread_mutex_lock(&o->m); o->signaled = 0; pthread_mutex_unlock(&o->m); return TRUE; }
HANDLE CreateWaitableTimerExW(SECURITY_ATTRIBUTES *sa, LPCWSTR name, DWORD flags, DWORD access) {
    (void)sa; (void)name; (void)access;
    Obj *o = obj_new(H_TIMER);
    o->manual = flags & 1;                               /* CREATE_WAITABLE_TIMER_MANUAL_RESET */
    return o;
}
BOOL SetWaitableTimer(HANDLE h, const LARGE_INTEGER *due, LONG period, void *fn, void *arg, BOOL resume) {
    (void)fn; (void)arg; (void)resume;
    Obj *o = h;
    int64_t d = due->QuadPart, at;
    if (d < 0) at = now_ns() + (-d) * 100;               /* relative, 100 ns units */
    else {                                               /* absolute FILETIME: convert through the wall clock */
        FILETIME ft; GetSystemTimeAsFileTime(&ft);
        int64_t wall = (int64_t)((uint64_t)ft.dwHighDateTime << 32 | ft.dwLowDateTime);
        at = now_ns() + (d - wall) * 100;
    }
    pthread_mutex_lock(&o->m);
    o->due = at; o->period = (int64_t)period * 1000000; o->armed = 1; o->signaled = 0;
    pthread_cond_broadcast(&o->c);
    pthread_mutex_unlock(&o->m);
    return TRUE;
}
/* with o->m held: is it signalled now (a due timer becomes signalled); consume it for auto-reset kinds */
static int obj_take(Obj *o) {
    if (o->kind == H_TIMER && o->armed && !o->signaled && now_ns() >= o->due) {
        o->signaled = 1;
        if (o->period > 0) o->due += o->period; else o->armed = 0;
    }
    if (o->kind == H_SEM) {                              /* a semaphore: one unit per successful wait */
        if (o->owned <= 0) return 0;
        o->owned--;
        return 1;
    }
    if (!o->signaled) return 0;
    if (!o->manual && o->kind != H_THREAD) o->signaled = 0;
    return 1;
}
static DWORD wait_special(Obj *o, DWORD ms);
DWORD WaitForSingleObject(HANDLE h, DWORD ms) {
    if (g_cvs_on) InterlockedIncrement64(&g_wfso_n);
    if (h == PSEUDO_THREAD || h == INVALID_HANDLE_VALUE || !h || IS_FH(h)) return WAIT_FAILED;
    Obj *o = h;
    if (o->kind == H_MUTEX || o->kind == H_PROC) return wait_special(o, ms);
    int64_t until = ms == INFINITE ? INT64_MAX : now_ns() + (int64_t)ms * 1000000;
    DWORD r = WAIT_TIMEOUT;
    pthread_mutex_lock(&o->m);
    for (;;) {
        if (obj_take(o)) { r = WAIT_OBJECT_0; break; }
        int64_t wake = until;
        if (o->kind == H_TIMER && o->armed && o->due < wake) wake = o->due;
        if (wake != INT64_MAX && now_ns() >= until) break;
        if (wake == INT64_MAX) pthread_cond_wait(&o->c, &o->m);
        else {
            /* [Recomps#1] RELATIVE: Apple's condition variables are CLOCK_REALTIME (notes/macos-port-plan.md section 2);
             * an absolute pthread_cond_timedwait here took the MONOTONIC deadline as a wall-clock time, always found
             * it past, and returned at once - every timed wait and every timer then became a 100 % CPU spin loop
             * (measured: ~28 threads, ~48 % of a core each, in this loop; the game ran at 4-9 fps). */
            int64_t left = wake - now_ns();
            struct timespec rel = { left / 1000000000, left % 1000000000 };
            pthread_cond_timedwait_relative_np(&o->c, &o->m, &rel);
        }
    }
    pthread_mutex_unlock(&o->m);
    return r;
}
DWORD WaitForMultipleObjects(DWORD n, const HANDLE *hs, BOOL all, DWORD ms) {
    int64_t until = ms == INFINITE ? INT64_MAX : now_ns() + (int64_t)ms * 1000000;
    if (all) {                                           /* each in turn within the one time limit */
        for (DWORD i = 0; i < n; i++) {
            int64_t left = until == INT64_MAX ? -1 : (until - now_ns()) / 1000000;
            if (WaitForSingleObject(hs[i], left < 0 && until != INT64_MAX ? 0 : (until == INT64_MAX ? INFINITE : (DWORD)left)) != WAIT_OBJECT_0) return WAIT_TIMEOUT;
        }
        return WAIT_OBJECT_0;
    }
    for (;;) {                                           /* any: poll (one caller, rarely used) */
        for (DWORD i = 0; i < n; i++) if (WaitForSingleObject(hs[i], 0) == WAIT_OBJECT_0) return WAIT_OBJECT_0 + i;
        if (now_ns() >= until) return WAIT_TIMEOUT;
        Sleep(1);
    }
}
BOOL CloseHandle(HANDLE h) {
    if (!h || h == PSEUDO_THREAD || h == INVALID_HANDLE_VALUE) return TRUE;
    if (IS_FH(h)) return close(FH_FD(h)) == 0;
    obj_unref(h);
    return TRUE;
}
void ExitProcess(UINT code) { fflush(NULL); _exit((int)code); }

/* ---- fibers (ucontext) and fiber-local storage */
#define FLS_SLOTS 64
typedef struct Fiber {
    ucontext_t ctx;
    void *stack;
    size_t stack_size;
    LPFIBER_START_ROUTINE fn;
    void *param;
    int is_thread;
    void *fls[FLS_SLOTS];
} Fiber;
static __thread Fiber *t_fiber;                           /* the fiber running on this thread (NULL: not converted) */
static __thread void *t_fls[FLS_SLOTS];                   /* fiber-local values of a thread that never became a fiber */
static PFLS_CALLBACK_FUNCTION g_fls_cb[FLS_SLOTS];
static volatile int g_fls_used[FLS_SLOTS];
static pthread_key_t g_fls_key;
static pthread_once_t g_fls_once = PTHREAD_ONCE_INIT;
static void fls_run(void **slots) {
    for (int i = 0; i < FLS_SLOTS; i++)
        if (slots[i] && g_fls_used[i] && g_fls_cb[i]) { void *v = slots[i]; slots[i] = NULL; g_fls_cb[i](v); }
}
static void fls_thread_end(void *unused) { (void)unused; fls_run(t_fiber ? t_fiber->fls : t_fls); }
static void fls_init(void) { pthread_key_create(&g_fls_key, fls_thread_end); }
static void **fls_slots(void) {
    pthread_once(&g_fls_once, fls_init);
    pthread_setspecific(g_fls_key, (void *)1);            /* the destructor runs at this thread's end */
    return t_fiber ? t_fiber->fls : t_fls;
}
DWORD FlsAlloc(PFLS_CALLBACK_FUNCTION cb) {
    for (int i = 0; i < FLS_SLOTS; i++)
        if (!__atomic_exchange_n(&g_fls_used[i], 1, __ATOMIC_ACQ_REL)) { g_fls_cb[i] = cb; return (DWORD)i; }
    return FLS_OUT_OF_INDEXES;
}
void *FlsGetValue(DWORD i) { return i < FLS_SLOTS ? fls_slots()[i] : NULL; }
BOOL FlsSetValue(DWORD i, void *v) { if (i >= FLS_SLOTS) return FALSE; fls_slots()[i] = v; return TRUE; }
BOOL FlsFree(DWORD i) { if (i >= FLS_SLOTS) return FALSE; g_fls_cb[i] = NULL; __atomic_store_n(&g_fls_used[i], 0, __ATOMIC_RELEASE); return TRUE; }
void *ConvertThreadToFiber(void *param) {
    if (t_fiber) return t_fiber;
    Fiber *f = calloc(1, sizeof *f);
    f->is_thread = 1; f->param = param;
    memcpy(f->fls, t_fls, sizeof f->fls);                 /* the thread's values stay its own */
    getcontext(&f->ctx);
    t_fiber = f;
    return f;
}
static void fiber_entry(void) {
    Fiber *f = t_fiber;                                   /* SwitchToFiber set it before swapping in */
    f->fn(f->param);
    pthread_exit(NULL);                                   /* a fiber function that returns ends its thread, as on Windows */
}
void *CreateFiberEx(SIZE_T commit, SIZE_T reserve, DWORD flags, LPFIBER_START_ROUTINE fn, void *param) {
    (void)commit; (void)flags;
    size_t page = page_size(), size = (reserve ? reserve : 1 << 20) + page;
    uint8_t *mem = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (mem == MAP_FAILED) return NULL;
    mprotect(mem, page, PROT_NONE);                       /* guard page below the stack */
    Fiber *f = calloc(1, sizeof *f);
    f->stack = mem; f->stack_size = size; f->fn = fn; f->param = param;
    getcontext(&f->ctx);
    f->ctx.uc_stack.ss_sp = mem + page;
    f->ctx.uc_stack.ss_size = size - page;
    f->ctx.uc_link = NULL;
    makecontext(&f->ctx, fiber_entry, 0);
    return f;
}
void SwitchToFiber(void *fiber) {
    Fiber *from = t_fiber, *to = fiber;
    if (!from || from == to) { t_fiber = to; if (!from) setcontext(&to->ctx); return; }
    t_fiber = to;
    swapcontext(&from->ctx, &to->ctx);
}
void DeleteFiber(void *fiber) {
    Fiber *f = fiber;
    if (f == t_fiber) { fls_run(f->fls); pthread_exit(NULL); }   /* deleting the running fiber ends the thread */
    fls_run(f->fls);
    if (f->stack) munmap(f->stack, f->stack_size);
    free(f);
}
void *GetCurrentFiber(void) { return t_fiber; }
void *GetFiberData(void) { return t_fiber ? t_fiber->param : NULL; }

/* ---- write watch (MEM_WRITE_WATCH, GetWriteWatch): userfaultfd write-protection in async mode, where the kernel itself
 * lifts a page's protection at its first write (also a write by a system call or a driver), and PAGEMAP_SCAN, which lists
 * the pages written and protects them again in one call (Linux 6.7+; Wine emulates write watches the same way). Where the
 * kernel has neither (or userfaultfd is not allowed), a reservation is not watched and the callers hash, as before.
 * The kernel ABI is spelled out here: the toolchain's kernel headers may be older than these calls. */
/* ---- virtual memory */
/* [Recomps#1] Darwin has no userfaultfd/PAGEMAP_SCAN, so write watches are not tracked by the kernel: GetWriteWatch
 * returns "not watched" and the callers hash. The block below is intentionally empty (the Linux build uses
 * asynchronous userfaultfd write-protect; see plan step 6). */
typedef struct { uintptr_t base; size_t size; int ww; } Res;   /* ww: a write-watched reservation the kernel tracks */
static Res *g_res;
static int g_nres, g_capres;
static pthread_mutex_t g_res_m = PTHREAD_MUTEX_INITIALIZER;
#define GRAN ((size_t)64 << 10)
static int prot_of(DWORD p) {
    if (p & PAGE_GUARD) return PROT_NONE;
    switch (p & 0xff) {
    case PAGE_NOACCESS: return PROT_NONE;
    case PAGE_READONLY: return PROT_READ;
    case PAGE_READWRITE: return PROT_READ | PROT_WRITE;
    case PAGE_EXECUTE: return PROT_EXEC;
    case PAGE_EXECUTE_READ: return PROT_READ | PROT_EXEC;
    default: return PROT_READ | PROT_WRITE | PROT_EXEC;
    }
}
static Res *res_find(uintptr_t a) {                       /* with g_res_m held: the reservation holding a */
    for (int i = 0; i < g_nres; i++) if (a >= g_res[i].base && a < g_res[i].base + g_res[i].size) return &g_res[i];
    return NULL;
}
static void res_add(uintptr_t b, size_t n, int ww) {
    if (g_nres == g_capres) { g_capres = g_capres ? g_capres * 2 : 64; g_res = realloc(g_res, (size_t)g_capres * sizeof *g_res); }
    g_res[g_nres++] = (Res){ b, n, ww };
}
void *VirtualAlloc(void *addr, SIZE_T size, DWORD type, DWORD prot) {
    size_t page = page_size();
    uintptr_t a = (uintptr_t)addr;
    if (!size) { SetLastError(87); return NULL; }
    pthread_mutex_lock(&g_res_m);
    Res *r = a ? res_find(a) : NULL;
    if (r && (type & MEM_COMMIT)) {                       /* commit inside an existing reservation */
        uintptr_t s = a & ~(page - 1), e = (a + size + page - 1) & ~(page - 1);
        int ok = mprotect((void *)s, e - s, prot_of(prot)) == 0;
        if (ok) { }                                      /* Darwin: no kernel write-watch; r->ww always false */
        pthread_mutex_unlock(&g_res_m);
        return ok ? (void *)s : NULL;
    }
    if (r) { pthread_mutex_unlock(&g_res_m); SetLastError(487); return NULL; }   /* ERROR_INVALID_ADDRESS: already reserved */
    size_t n = (size + GRAN - 1) & ~(GRAN - 1);
    int p = (type & MEM_COMMIT) ? prot_of(prot) : PROT_NONE, fl = MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE;
    uint8_t *m;
    if (a) {
        a &= ~(GRAN - 1);
        fl |= MAP_FIXED;     /* macOS: enforce fixed address for GUEST_BASE / IMPORT_BASE */
        m = mmap((void *)a, n, p, fl, -1, 0);                 /* Darwin: a is a hint; the check below refuses a different address */
        if (m != MAP_FAILED && (uintptr_t)m != a) { munmap(m, n); m = MAP_FAILED; }   /* old kernels ignore the flag */
    } else {                                              /* 64 KiB aligned, as Windows' allocation granularity */
        uint8_t *big = mmap(NULL, n + GRAN, p, fl, -1, 0);
        if (big == MAP_FAILED) m = MAP_FAILED;
        else {
            uintptr_t al = ((uintptr_t)big + GRAN - 1) & ~(GRAN - 1);
            if (al > (uintptr_t)big) munmap(big, al - (uintptr_t)big);
            if ((uintptr_t)big + n + GRAN > al + n) munmap((void *)(al + n), (uintptr_t)big + n + GRAN - (al + n));
            m = (uint8_t *)al;
        }
    }
    if (m == MAP_FAILED) { pthread_mutex_unlock(&g_res_m); SetLastError(8); return NULL; }   /* ERROR_NOT_ENOUGH_MEMORY */
    res_add((uintptr_t)m, n, 0);                                                      /* [Recomps#1] Darwin: no kernel write-watch (plan step 6) */
    pthread_mutex_unlock(&g_res_m);
    return m;
}
BOOL VirtualFree(void *addr, SIZE_T size, DWORD type) {
    uintptr_t a = (uintptr_t)addr;
    pthread_mutex_lock(&g_res_m);
    Res *r = res_find(a);
    if (!r) { pthread_mutex_unlock(&g_res_m); SetLastError(487); return FALSE; }
    if (type & MEM_RELEASE) {
        if (a != r->base || size) { pthread_mutex_unlock(&g_res_m); SetLastError(87); return FALSE; }
        munmap((void *)r->base, r->size);
        *r = g_res[--g_nres];
    } else {                                              /* MEM_DECOMMIT: back to reserved, contents gone */
        size_t page = page_size();
        uintptr_t s = a & ~(page - 1), e = size ? (a + size + page - 1) & ~(page - 1) : r->base + r->size;
        madvise((void *)s, e - s, MADV_DONTNEED);
        mprotect((void *)s, e - s, PROT_NONE);
    }
    pthread_mutex_unlock(&g_res_m);
    return TRUE;
}
/* The pages of [base, base+n) written since the last reset, in address order, at most *count of them; with
 * WRITE_WATCH_FLAG_RESET the ones listed count as not written again. 1 = not a watched reservation (the callers hash). */
UINT GetWriteWatch(DWORD flags, void *base, SIZE_T n, void **addrs, ULONG_PTR *count, ULONG *gran) {
    (void)flags; (void)base; (void)n; (void)addrs; (void)count; (void)gran;
    return 1;  /* [Recomps#1] Darwin: no kernel write-watch (plan step 6) */
}
UINT ResetWriteWatch(void *base, SIZE_T n) {
    (void)base; (void)n;
    return 1;  /* [Recomps#1] Darwin: no kernel write-watch (plan step 6) */
}
/* /proc/self/maps: the mapping holding a (or the gap before the next one) */
static int maps_find(uintptr_t a, uintptr_t *lo, uintptr_t *hi, int *prot, uintptr_t *next) {
    mach_vm_address_t addr = a;
    mach_vm_size_t size = 0;
    struct vm_region_basic_info_64 info;
    mach_msg_type_number_t cnt = VM_REGION_BASIC_INFO_COUNT_64;
    mach_port_t object = MACH_PORT_NULL;
    *next = UINTPTR_MAX;
    if (mach_vm_region(mach_task_self(), &addr, &size, VM_REGION_BASIC_INFO_64, (vm_region_info_t)&info, &cnt, &object) != KERN_SUCCESS) return 0;
    if (addr > a) { *next = (uintptr_t)addr; return 0; }
    *lo = (uintptr_t)addr; *hi = (uintptr_t)(addr + size);
    *prot = (info.protection & VM_PROT_READ ? PROT_READ : 0) | (info.protection & VM_PROT_WRITE ? PROT_WRITE : 0);
    return 1;
}
static DWORD page_of(int p) {
    if (p == PROT_NONE) return PAGE_NOACCESS;
    if (p & PROT_EXEC) return (p & PROT_WRITE) ? PAGE_EXECUTE_READWRITE : (p & PROT_READ) ? PAGE_EXECUTE_READ : PAGE_EXECUTE;
    return (p & PROT_WRITE) ? PAGE_READWRITE : PAGE_READONLY;
}
SIZE_T VirtualQuery(const void *addr, MEMORY_BASIC_INFORMATION *mbi, SIZE_T len) {
    if (len < sizeof *mbi) return 0;
    uintptr_t a = (uintptr_t)addr & ~(uintptr_t)(page_size() - 1), lo, hi, next;
    int prot;
    memset(mbi, 0, sizeof *mbi);
    mbi->BaseAddress = (void *)a;
    if (!maps_find(a, &lo, &hi, &prot, &next)) {
        mbi->State = MEM_FREE; mbi->Protect = PAGE_NOACCESS;
        mbi->RegionSize = (next == UINTPTR_MAX ? a + GRAN : next) - a;
        return sizeof *mbi;
    }
    pthread_mutex_lock(&g_res_m);
    Res *r = res_find(a);
    mbi->AllocationBase = (void *)(r ? r->base : lo);
    pthread_mutex_unlock(&g_res_m);
    mbi->RegionSize = hi - a;
    mbi->State = (prot == PROT_NONE && r) ? MEM_RESERVE : MEM_COMMIT;
    mbi->Protect = mbi->State == MEM_RESERVE ? 0 : page_of(prot);
    mbi->AllocationProtect = PAGE_READWRITE;
    mbi->Type = MEM_PRIVATE;
    return sizeof *mbi;
}
BOOL VirtualProtect(void *addr, SIZE_T size, DWORD prot, DWORD *old) {
    MEMORY_BASIC_INFORMATION mbi;
    if (old) *old = VirtualQuery(addr, &mbi, sizeof mbi) ? mbi.Protect : PAGE_NOACCESS;
    uintptr_t s = (uintptr_t)addr & ~(uintptr_t)(page_size() - 1), e = ((uintptr_t)addr + size + (page_size() - 1)) & ~(uintptr_t)(page_size() - 1);
    return mprotect((void *)s, e - s, prot_of(prot)) == 0;
}
BOOL IsBadWritePtr(void *p, UINT_PTR n) {
    uintptr_t a = (uintptr_t)p, end = a + (n ? n : 1);
    while (a < end) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery((void *)a, &mbi, sizeof mbi) || mbi.State != MEM_COMMIT || !(mbi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE))) return TRUE;
        a = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
    }
    return FALSE;
}
BOOL IsBadReadPtr(const void *p, UINT_PTR n) {
    uintptr_t a = (uintptr_t)p, end = a + (n ? n : 1);
    while (a < end) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery((void *)a, &mbi, sizeof mbi) || mbi.State != MEM_COMMIT || !(mbi.Protect & 0xEE)) return TRUE;
        a = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
    }
    return FALSE;
}

/* ---- modules */
static const struct { const char *dll, *so; } k_libs[] = {
    {"vulkan-1.dll", "libvulkan.dylib"}, {"sdl3.dll", "libSDL3.dylib"}, {"opengl32.dll", "libGL.dylib"},
    {"game.dll", "game.dylib"}, {"ssbu_glslang.dll", "libssbu_glslang.so"}, {"libopus-0.dll", "libopus.dylib"}, {"opus.dll", "libopus.dylib"},
};
static void *open_named(const char *name, int flags) {
    char low[MAX_PATH];
    size_t i;
    for (i = 0; name[i] && i + 1 < sizeof low; i++) low[i] = (char)(name[i] >= 'A' && name[i] <= 'Z' ? name[i] + 32 : name[i]);
    low[i] = 0;
    const char *base = strrchr(low, '\\') ? strrchr(low, '\\') + 1 : strrchr(low, '/') ? strrchr(low, '/') + 1 : low;
    for (size_t k = 0; k < sizeof k_libs / sizeof k_libs[0]; k++)
        if (!strcmp(base, k_libs[k].dll)) {
            if (!strcmp(k_libs[k].so, "game.dylib") || !strcmp(k_libs[k].so, "libssbu_glslang.so")) {   /* ours: beside the program */
                char p[PATH_MAX];
                int n = proc_pidpath(getpid(), p, sizeof p - 1);      /* [Recomps#1] Darwin: no /proc/self/exe (plan section 2) */
                if (n > 0) {
                    char *s;
                    p[n] = 0;
                    if ((s = strrchr(p, '/'))) {
                        snprintf(s + 1, sizeof p - (size_t)(s + 1 - p), "%s", k_libs[k].so);
                        void *h = dlopen(p, flags);
                        if (h) return h;
                    }
                }
            }
            return dlopen(k_libs[k].so, flags);
        }
    return dlopen(name, flags);
}
HMODULE LoadLibraryA(LPCSTR name) { void *h = open_named(name, RTLD_NOW | RTLD_LOCAL); if (!h) SetLastError(126); return h; }
HMODULE GetModuleHandleA(LPCSTR name) { return name ? open_named(name, RTLD_NOW | RTLD_NOLOAD) : dlopen(NULL, RTLD_NOW); }
BOOL GetModuleHandleExA(DWORD flags, LPCSTR name, HMODULE *out) {
    if (flags & GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS) {
        Dl_info di;
        if (!dladdr(name, &di)) { *out = NULL; return FALSE; }
        *out = dlopen(di.dli_fname, RTLD_NOW | RTLD_NOLOAD);
        return *out != NULL;
    }
    *out = GetModuleHandleA(name);
    return *out != NULL;
}
void *GetProcAddress(HMODULE m, LPCSTR name) { return dlsym(m ? m : RTLD_DEFAULT, name); }
BOOL FreeLibrary(HMODULE m) { return dlclose(m) == 0; }
DWORD GetModuleFileNameA(HMODULE m, char *out, DWORD n) {
    char p[PATH_MAX];
    ssize_t k = -1;
    if (!m || m == RTLD_DEFAULT) {                             /* main executable */
        k = proc_pidpath(getpid(), p, sizeof p - 1);
    } else {                                                   /* loaded module: not yet ported (plan step 6) */
        k = 0;
    }
    if (k <= 0 || !n) return 0;
    p[k] = 0;
    snprintf(out, n, "%s", p);
    for (char *c = out; *c; c++) if (*c == '/') *c = '\\';   /* as Windows spells it: the runtime cuts the path at '\\' (its
                                                               <exe folder>\\..\\config); the file functions here take both */
    return (DWORD)strlen(out);
}

/* ---- files and folders */
/* A path that does not exist as written: each folder level is looked up case-insensitively (the first match wins); the
 * levels that do not exist at all are kept as given (a file or folder about to be made). */
static void ci_fix(char *p) {
    struct stat st;
    if (!*p || !lstat(p, &st)) return;
    char out[PATH_MAX];
    size_t o = 0;
    const char *s = p;
    if (*s == '/') { out[o++] = '/'; s++; }
    out[o] = 0;
    while (*s) {
        const char *e = strchr(s, '/');
        size_t len = e ? (size_t)(e - s) : strlen(s);
        char comp[256], trial[PATH_MAX];
        if (len >= sizeof comp) return;
        memcpy(comp, s, len); comp[len] = 0;
        snprintf(trial, sizeof trial, "%s%s", out, comp);
        if (len && lstat(trial, &st)) {
            DIR *d = opendir(o ? out : ".");
            struct dirent *de;
            int found = 0;
            if (d) {
                while ((de = readdir(d))) if (!strcasecmp(de->d_name, comp)) { snprintf(comp, sizeof comp, "%s", de->d_name); found = 1; break; }
                closedir(d);
            }
            if (!found) { snprintf(out + o, sizeof out - o, "%s", s); break; }
        }
        o += (size_t)snprintf(out + o, sizeof out - o, "%s%s", comp, e ? "/" : "");
        if (!e || o >= sizeof out) break;
        s = e + 1;
    }
    snprintf(p, PATH_MAX, "%s", out);
}
static const char *upath(const char *in, char *buf) {     /* '\\' -> '/', then the case of existing folders and files */
    snprintf(buf, PATH_MAX, "%s", in);
    for (char *c = buf; *c; c++) if (*c == '\\') *c = '/';
    ci_fix(buf);
    return buf;
}
#undef fopen
FILE *recomps_fopen(const char *path, const char *mode) { char b[PATH_MAX]; return fopen(path ? upath(path, b) : path, mode); }
#define fopen recomps_fopen
static DWORD err_of(int e) { return e == ENOENT ? ERROR_FILE_NOT_FOUND : e == EEXIST ? ERROR_ALREADY_EXISTS : e == ENOTDIR ? ERROR_PATH_NOT_FOUND : (DWORD)e; }
BOOL CreateDirectoryA(LPCSTR path, SECURITY_ATTRIBUTES *sa) { (void)sa; char b[PATH_MAX]; if (!mkdir(upath(path, b), 0755)) return TRUE; SetLastError(err_of(errno)); return FALSE; }
BOOL RemoveDirectoryA(LPCSTR path) { char b[PATH_MAX]; if (!rmdir(upath(path, b))) return TRUE; SetLastError(err_of(errno)); return FALSE; }
BOOL DeleteFileA(LPCSTR path) { char b[PATH_MAX]; if (!unlink(upath(path, b))) return TRUE; SetLastError(err_of(errno)); return FALSE; }
BOOL MoveFileExA(LPCSTR from, LPCSTR to, DWORD flags) {
    char a[PATH_MAX], b[PATH_MAX];
    struct stat st;
    upath(from, a); upath(to, b);
    if (!(flags & MOVEFILE_REPLACE_EXISTING) && !stat(b, &st)) { SetLastError(ERROR_ALREADY_EXISTS); return FALSE; }
    if (!rename(a, b)) return TRUE;
    SetLastError(err_of(errno));
    return FALSE;
}
static void ft_of(time_t t, long ns, FILETIME *ft) {
    uint64_t v = (uint64_t)t * 10000000u + (uint64_t)ns / 100u + 116444736000000000ull;
    ft->dwLowDateTime = (DWORD)v; ft->dwHighDateTime = (DWORD)(v >> 32);
}
DWORD GetFileAttributesA(LPCSTR path) {
    char b[PATH_MAX];
    struct stat st;
    if (stat(upath(path, b), &st)) { SetLastError(err_of(errno)); return INVALID_FILE_ATTRIBUTES; }
    DWORD a = S_ISDIR(st.st_mode) ? FILE_ATTRIBUTE_DIRECTORY : 0;
    if (!(st.st_mode & S_IWUSR)) a |= FILE_ATTRIBUTE_READONLY;
    return a ? a : FILE_ATTRIBUTE_NORMAL;
}
BOOL GetFileAttributesExA(LPCSTR path, GET_FILEEX_INFO_LEVELS lvl, void *out) {
    (void)lvl;
    char b[PATH_MAX];
    struct stat st;
    WIN32_FILE_ATTRIBUTE_DATA *d = out;
    if (stat(upath(path, b), &st)) { SetLastError(err_of(errno)); return FALSE; }
    memset(d, 0, sizeof *d);
    d->dwFileAttributes = GetFileAttributesA(path);
    ft_of(st.st_birthtimespec.tv_sec, st.st_birthtimespec.tv_nsec, &d->ftCreationTime);
    ft_of(st.st_atimespec.tv_sec, st.st_atimespec.tv_nsec, &d->ftLastAccessTime);
    ft_of(st.st_mtimespec.tv_sec, st.st_mtimespec.tv_nsec, &d->ftLastWriteTime);
    d->nFileSizeHigh = (DWORD)((uint64_t)st.st_size >> 32); d->nFileSizeLow = (DWORD)st.st_size;
    return TRUE;
}
DWORD GetTempPathA(DWORD n, char *out) {
    const char *t = getenv("TMPDIR");
    char b[PATH_MAX];
    snprintf(b, sizeof b, "%s/", t && *t ? t : "/tmp");
    if (strlen(b) + 1 > n) return (DWORD)strlen(b) + 1;
    memcpy(out, b, strlen(b) + 1);
    return (DWORD)strlen(b);
}

/* ---- text: UTF-8 <-> wchar_t (UTF-32 on Linux) */
int MultiByteToWideChar(UINT cp, DWORD flags, LPCSTR s, int n, WCHAR *out, int on) {
    (void)cp; (void)flags;
    const unsigned char *p = (const unsigned char *)s, *e = n < 0 ? p + strlen(s) + 1 : p + n;
    int k = 0;
    while (p < e) {
        uint32_t c = *p++;
        int extra = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
        if (extra) { c &= 0x3F >> extra; while (extra-- && p < e) c = c << 6 | (*p++ & 0x3F); }
        if (on) { if (k >= on) { SetLastError(122); return 0; } out[k] = (WCHAR)c; }
        k++;
    }
    return k;
}
int WideCharToMultiByte(UINT cp, DWORD flags, LPCWSTR s, int n, char *out, int on, LPCSTR def, BOOL *used) {
    (void)cp; (void)flags; (void)def;
    if (used) *used = FALSE;
    size_t len = n < 0 ? wcslen(s) + 1 : (size_t)n;
    int k = 0;
    for (size_t i = 0; i < len; i++) {
        uint32_t c = (uint32_t)s[i];
        char b[4]; int m;
        if (c < 0x80) { b[0] = (char)c; m = 1; }
        else if (c < 0x800) { b[0] = (char)(0xC0 | c >> 6); b[1] = (char)(0x80 | (c & 0x3F)); m = 2; }
        else if (c < 0x10000) { b[0] = (char)(0xE0 | c >> 12); b[1] = (char)(0x80 | (c >> 6 & 0x3F)); b[2] = (char)(0x80 | (c & 0x3F)); m = 3; }
        else { b[0] = (char)(0xF0 | c >> 18); b[1] = (char)(0x80 | (c >> 12 & 0x3F)); b[2] = (char)(0x80 | (c >> 6 & 0x3F)); b[3] = (char)(0x80 | (c & 0x3F)); m = 4; }
        if (on) { if (k + m > on) { SetLastError(122); return 0; } memcpy(out + k, b, (size_t)m); }
        k += m;
    }
    return k;
}

/* ---- misc */
BOOLEAN SystemFunction036(void *buf, ULONG len) {
    uint8_t *p = buf;
    while (len) { arc4random_buf(p, len); break; } /* [Recomps#1] Darwin: getrandom() not available; use arc4random_buf */
    return TRUE;
}
void RaiseException(DWORD code, DWORD flags, DWORD n, const ULONG_PTR *args) {
    (void)flags; (void)n; (void)args;
    fprintf(stderr, "RaiseException 0x%08x: stopping\n", code);
    fflush(NULL);
    abort();                                              /* the crash report (hostrt.c) moves to a signal handler in plan step 4 */
}

/* ---- directory listing: FindFirstFile / FindNextFile ("dir\pattern", '*' and '?' in the last part only) */
typedef struct { DIR *d; char dir[PATH_MAX], pat[MAX_PATH]; } Find;
static int find_next(Find *f, WIN32_FIND_DATAA *fd) {
    struct dirent *e;
    while ((e = readdir(f->d))) {
        if (fnmatch(f->pat, e->d_name, FNM_CASEFOLD)) continue;
        char full[PATH_MAX + MAX_PATH];
        struct stat st;
        snprintf(full, sizeof full, "%s/%s", f->dir, e->d_name);
        memset(fd, 0, sizeof *fd);
        if (!stat(full, &st)) {
            fd->dwFileAttributes = S_ISDIR(st.st_mode) ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
            fd->nFileSizeHigh = (DWORD)((uint64_t)st.st_size >> 32); fd->nFileSizeLow = (DWORD)st.st_size;
            ft_of(st.st_mtimespec.tv_sec, st.st_mtimespec.tv_nsec, &fd->ftLastWriteTime);
            ft_of(st.st_birthtimespec.tv_sec, st.st_birthtimespec.tv_nsec, &fd->ftCreationTime);
            ft_of(st.st_atimespec.tv_sec, st.st_atimespec.tv_nsec, &fd->ftLastAccessTime);
        }
        snprintf(fd->cFileName, sizeof fd->cFileName, "%s", e->d_name);
        return 1;
    }
    return 0;
}
HANDLE FindFirstFileA(LPCSTR pattern, WIN32_FIND_DATAA *fd) {
    char p[PATH_MAX];
    upath(pattern, p);
    Find *f = calloc(1, sizeof *f);
    char *slash = strrchr(p, '/');
    if (slash) { *slash = 0; snprintf(f->dir, sizeof f->dir, "%s", *p ? p : "/"); snprintf(f->pat, sizeof f->pat, "%s", slash + 1); }
    else { snprintf(f->dir, sizeof f->dir, "."); snprintf(f->pat, sizeof f->pat, "%s", p); }
    if (!strcmp(f->pat, "*.*")) snprintf(f->pat, sizeof f->pat, "*");   /* Windows: "*.*" also matches names without a dot */
    if (!(f->d = opendir(f->dir))) { free(f); SetLastError(ERROR_PATH_NOT_FOUND); return INVALID_HANDLE_VALUE; }
    if (!find_next(f, fd)) { closedir(f->d); free(f); SetLastError(ERROR_FILE_NOT_FOUND); return INVALID_HANDLE_VALUE; }
    return f;
}
BOOL FindNextFileA(HANDLE h, WIN32_FIND_DATAA *fd) { if (find_next(h, fd)) return TRUE; SetLastError(18); return FALSE; }   /* ERROR_NO_MORE_FILES */
static void fd_w(const WIN32_FIND_DATAA *a, WIN32_FIND_DATAW *w) {
    memcpy(w, a, offsetof(WIN32_FIND_DATAA, cFileName));
    MultiByteToWideChar(CP_UTF8, 0, a->cFileName, -1, w->cFileName, MAX_PATH);
    w->cAlternateFileName[0] = 0;
}
HANDLE FindFirstFileW(LPCWSTR pattern, WIN32_FIND_DATAW *fd) {
    char p[PATH_MAX];
    WIN32_FIND_DATAA a;
    if (!WideCharToMultiByte(CP_UTF8, 0, pattern, -1, p, sizeof p, NULL, NULL)) return INVALID_HANDLE_VALUE;
    HANDLE h = FindFirstFileA(p, &a);
    if (h != INVALID_HANDLE_VALUE) fd_w(&a, fd);
    return h;
}
BOOL FindNextFileW(HANDLE h, WIN32_FIND_DATAW *fd) { WIN32_FIND_DATAA a; if (!FindNextFileA(h, &a)) return FALSE; fd_w(&a, fd); return TRUE; }
BOOL FindClose(HANDLE h) { Find *f = h; if (!f || h == INVALID_HANDLE_VALUE) return FALSE; closedir(f->d); free(f); return TRUE; }

/* ---- diagnostics: registered, not called yet on Linux (plan step 4 adds the signal handler that calls them) */
static PVECTORED_EXCEPTION_HANDLER g_veh[8];
static LPTOP_LEVEL_EXCEPTION_FILTER g_top_filter;
void *AddVectoredExceptionHandler(ULONG first, PVECTORED_EXCEPTION_HANDLER h) {
    (void)first;
    for (int i = 0; i < 8; i++) if (!g_veh[i]) { g_veh[i] = h; return &g_veh[i]; }
    return NULL;
}
ULONG RemoveVectoredExceptionHandler(void *h) { PVECTORED_EXCEPTION_HANDLER *s = h; if (!s) return 0; *s = NULL; return 1; }
LPTOP_LEVEL_EXCEPTION_FILTER SetUnhandledExceptionFilter(LPTOP_LEVEL_EXCEPTION_FILTER f) { LPTOP_LEVEL_EXCEPTION_FILTER o = g_top_filter; g_top_filter = f; return o; }
void RtlCaptureContext(CONTEXT *c) {
    memset(c, 0, sizeof *c);
    c->Rip = (DWORD64)__builtin_return_address(0);
    c->Rsp = (DWORD64)__builtin_frame_address(0);
    c->Rbp = c->Rsp;
}

/* ---- memory counters (psapi.h) */
BOOL GetProcessMemoryInfo(HANDLE p, PROCESS_MEMORY_COUNTERS *pmc, DWORD cb) {
    (void)p;
    struct task_vm_info vm;
    mach_msg_type_number_t cnt = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&vm, &cnt) != KERN_SUCCESS) return FALSE;
    if (cb >= sizeof(PROCESS_MEMORY_COUNTERS_EX)) {
        PROCESS_MEMORY_COUNTERS_EX *pmce = (void *)pmc;
        pmce->cb = cb;
        pmce->PagefileUsage = (SIZE_T)vm.phys_footprint;
        pmce->PrivateUsage = (SIZE_T)vm.phys_footprint;
        pmce->PeakPagefileUsage = 0;   /* peak footprint not available */
        pmce->PeakWorkingSetSize = vm.resident_size;   /* Darwin: no separate peak resident tracking in this struct; use current */
    } else {
        pmc->cb = cb;
        pmc->PagefileUsage = (SIZE_T)vm.phys_footprint;
    }
    pmc->WorkingSetSize = vm.resident_size;
    pmc->PageFaultCount = 0;  /* not available on Darwin; Linux also 0 for task-only */
    return TRUE;
}

/* ---- file handles from _get_osfhandle (io.h): the descriptor itself */
BOOL SetFilePointerEx(HANDLE h, LARGE_INTEGER to, LARGE_INTEGER *now, DWORD how) {
    off_t r = lseek(FH_FD(h), (off_t)to.QuadPart, how == FILE_END ? SEEK_END : how == FILE_CURRENT ? SEEK_CUR : SEEK_SET);
    if (r < 0) { SetLastError(err_of(errno)); return FALSE; }
    if (now) now->QuadPart = r;
    return TRUE;
}
BOOL SetEndOfFile(HANDLE h) {
    int fd = FH_FD(h);
    off_t at = lseek(fd, 0, SEEK_CUR);
    if (at < 0 || ftruncate(fd, at)) { SetLastError(err_of(errno)); return FALSE; }
    return TRUE;
}
BOOL ReadProcessMemory(HANDLE p, LPCVOID from, void *to, SIZE_T n, SIZE_T *got) {
    (void)p;
    mach_vm_size_t out = 0;
    kern_return_t kr = mach_vm_read_overwrite(mach_task_self(), (mach_vm_address_t)(uintptr_t)from, n,
                                              (mach_vm_address_t)(uintptr_t)to, &out);   /* fails cleanly on unmapped memory */
    if (got) *got = out;
    return kr == KERN_SUCCESS && out == n;
}

/* ---- file handles, copies, attributes, mappings, mutexes, processes */
HANDLE CreateFileA(LPCSTR path, DWORD access, DWORD share, SECURITY_ATTRIBUTES *sa, DWORD disposition, DWORD flags, HANDLE tmpl) {
    (void)share; (void)sa; (void)flags; (void)tmpl;
    char b[PATH_MAX];
    struct stat st;
    upath(path, b);
    int existed = stat(b, &st) == 0;
    int fl = (access & GENERIC_WRITE) ? ((access & GENERIC_READ) ? O_RDWR : O_WRONLY) : O_RDONLY;
    if (existed && S_ISDIR(st.st_mode)) fl = O_RDONLY;   /* a folder (FILE_FLAG_BACKUP_SEMANTICS): open to flush it */
    switch (disposition) {
    case CREATE_NEW: fl |= O_CREAT | O_EXCL; break;
    case CREATE_ALWAYS: fl |= O_CREAT | O_TRUNC; break;
    case OPEN_ALWAYS: fl |= O_CREAT; break;
    case TRUNCATE_EXISTING: fl |= O_TRUNC; break;
    default: break;                                       /* OPEN_EXISTING */
    }
    int fd = open(b, fl | O_CLOEXEC, 0644);
    if (fd < 0) { SetLastError(errno == EEXIST ? ERROR_FILE_EXISTS : errno == EACCES ? ERROR_ACCESS_DENIED : err_of(errno)); return INVALID_HANDLE_VALUE; }
    SetLastError(existed && (disposition == CREATE_ALWAYS || disposition == OPEN_ALWAYS) ? ERROR_ALREADY_EXISTS : 0);
    return FH_OF(fd);
}
BOOL ReadFile(HANDLE h, void *buf, DWORD n, DWORD *got, void *overlapped) {
    (void)overlapped;
    size_t done = 0;
    while (done < n) {
        const OVERLAPPED *ov = overlapped;
        ssize_t r = ov ? pread(FH_FD(h), (char *)buf + done, n - done, (off_t)(((uint64_t)ov->OffsetHigh << 32 | ov->Offset) + done))
                       : read(FH_FD(h), (char *)buf + done, n - done);
        if (r < 0) { if (errno == EINTR) continue; if (got) *got = (DWORD)done; SetLastError(err_of(errno)); return FALSE; }
        if (!r) break;
        done += (size_t)r;
    }
    if (got) *got = (DWORD)done;
    return TRUE;
}
BOOL WriteFile(HANDLE h, const void *buf, DWORD n, DWORD *put, void *overlapped) {
    (void)overlapped;
    size_t done = 0;
    while (done < n) {
        const OVERLAPPED *ov = overlapped;
        ssize_t r = ov ? pwrite(FH_FD(h), (const char *)buf + done, n - done, (off_t)(((uint64_t)ov->OffsetHigh << 32 | ov->Offset) + done))
                       : write(FH_FD(h), (const char *)buf + done, n - done);
        if (r < 0) { if (errno == EINTR) continue; if (put) *put = (DWORD)done; SetLastError(err_of(errno)); return FALSE; }
        done += (size_t)r;
    }
    if (put) *put = (DWORD)done;
    return TRUE;
}
BOOL GetUserNameW(WCHAR *out, DWORD *n) {
    const char *u = getenv("USER");
    struct passwd *pw = (!u || !*u) ? getpwuid(getuid()) : NULL;
    if (pw) u = pw->pw_name;
    if (!u || !*u) return FALSE;
    int k = MultiByteToWideChar(CP_UTF8, 0, u, -1, out, (int)*n);
    if (!k) return FALSE;
    *n = (DWORD)k;
    return TRUE;
}
BOOL GetFileSizeEx(HANDLE h, LARGE_INTEGER *size) { struct stat st; if (fstat(FH_FD(h), &st)) return FALSE; size->QuadPart = st.st_size; return TRUE; }
DWORD GetFileSize(HANDLE h, DWORD *high) {
    LARGE_INTEGER s;
    if (!GetFileSizeEx(h, &s)) return 0xFFFFFFFFu;
    if (high) *high = (DWORD)((uint64_t)s.QuadPart >> 32);
    return (DWORD)s.QuadPart;
}
BOOL FlushFileBuffers(HANDLE h) { return fsync(FH_FD(h)) == 0; }
BOOL CopyFileA(LPCSTR from, LPCSTR to, BOOL fail_if_exists) {
    char a[PATH_MAX], b[PATH_MAX], buf[1 << 16];
    struct stat st;
    upath(from, a); upath(to, b);
    int in = open(a, O_RDONLY | O_CLOEXEC);
    if (in < 0) { SetLastError(err_of(errno)); return FALSE; }
    fstat(in, &st);
    int out = open(b, O_WRONLY | O_CREAT | O_CLOEXEC | (fail_if_exists ? O_EXCL : O_TRUNC), st.st_mode & 0777);
    if (out < 0) { SetLastError(errno == EEXIST ? ERROR_FILE_EXISTS : err_of(errno)); close(in); return FALSE; }
    ssize_t r;
    int ok = 1;
    while ((r = read(in, buf, sizeof buf)) > 0) if (write(out, buf, (size_t)r) != r) { ok = 0; break; }
    if (r < 0) ok = 0;
    close(in);
    if (close(out)) ok = 0;
    return ok;
}
BOOL SetFileAttributesA(LPCSTR path, DWORD attrs) {
    char b[PATH_MAX];
    struct stat st;
    if (stat(upath(path, b), &st)) { SetLastError(err_of(errno)); return FALSE; }
    mode_t m = (attrs & FILE_ATTRIBUTE_READONLY) ? (st.st_mode & ~(mode_t)0222) : (st.st_mode | S_IWUSR);
    return chmod(b, m & 07777) == 0;
}
BOOL GetDiskFreeSpaceExA(LPCSTR path, ULARGE_INTEGER *avail, ULARGE_INTEGER *total, ULARGE_INTEGER *free_) {
    char b[PATH_MAX];
    struct statvfs v;
    if (statvfs(upath(path ? path : ".", b), &v)) return FALSE;
    if (avail) avail->QuadPart = (ULONGLONG)v.f_bavail * v.f_frsize;
    if (total) total->QuadPart = (ULONGLONG)v.f_blocks * v.f_frsize;
    if (free_) free_->QuadPart = (ULONGLONG)v.f_bfree * v.f_frsize;
    return TRUE;
}
HANDLE CreateFileMappingA(HANDLE file, SECURITY_ATTRIBUTES *sa, DWORD prot, DWORD hi, DWORD lo, LPCSTR name) {
    (void)sa; (void)name;
    LARGE_INTEGER s;
    if (!IS_FH(file) || !GetFileSizeEx(file, &s)) return NULL;
    Obj *o = obj_new(H_MAP);
    o->fd = dup(FH_FD(file));
    o->size = (hi || lo) ? ((uint64_t)hi << 32 | lo) : (uint64_t)s.QuadPart;
    o->map_write = (prot & 0xff) == PAGE_READWRITE;
    return o;
}
static struct { const void *p; size_t n; } g_views[256];
static pthread_mutex_t g_views_m = PTHREAD_MUTEX_INITIALIZER;
void *MapViewOfFile(HANDLE map, DWORD access, DWORD off_hi, DWORD off_lo, SIZE_T n) {
    Obj *o = map;
    uint64_t off = (uint64_t)off_hi << 32 | off_lo;
    if (!o || o->kind != H_MAP || off > o->size) return NULL;
    if (!n) n = (SIZE_T)(o->size - off);
    int prot = PROT_READ | ((access & FILE_MAP_WRITE) && o->map_write ? PROT_WRITE : 0);
    void *p = mmap(NULL, n, prot, MAP_SHARED, o->fd, (off_t)off);
    if (p == MAP_FAILED) return NULL;
    pthread_mutex_lock(&g_views_m);
    for (int i = 0; i < 256; i++) if (!g_views[i].p) { g_views[i].p = p; g_views[i].n = n; break; }
    pthread_mutex_unlock(&g_views_m);
    return p;
}
BOOL UnmapViewOfFile(const void *p) {
    size_t n = 0;
    pthread_mutex_lock(&g_views_m);
    for (int i = 0; i < 256; i++) if (g_views[i].p == p) { n = g_views[i].n; g_views[i].p = NULL; break; }
    pthread_mutex_unlock(&g_views_m);
    return n && munmap((void *)p, n) == 0;
}
HANDLE CreateMutexA(SECURITY_ATTRIBUTES *sa, BOOL owned, LPCSTR name) {
    (void)sa;
    Obj *o = obj_new(H_MUTEX);
    if (name && *name) {
        const char *dir = getenv("XDG_RUNTIME_DIR");
        char path[PATH_MAX];
        snprintf(path, sizeof path, "%s/%s.lock", dir && *dir ? dir : "/tmp", name);
        for (char *c = path + strlen(dir && *dir ? dir : "/tmp") + 1; *c; c++) if (*c == '/' || *c == '\\') *c = '_';
        o->fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
        if (o->fd < 0) { obj_unref(o); return NULL; }
    }
    if (owned) wait_special(o, INFINITE);
    return o;
}
BOOL ReleaseMutex(HANDLE h) {
    Obj *o = h;
    if (!o || o->kind != H_MUTEX) return FALSE;
    pthread_mutex_lock(&o->m);
    if (!o->owned || o->owner != GetCurrentThreadId()) { pthread_mutex_unlock(&o->m); return FALSE; }
    if (--o->owned == 0) { if (o->fd >= 0) flock(o->fd, LOCK_UN); pthread_cond_broadcast(&o->c); }
    pthread_mutex_unlock(&o->m);
    return TRUE;
}
HANDLE OpenProcess(DWORD access, BOOL inherit, DWORD pid) {
    (void)access; (void)inherit;
    if (kill((pid_t)pid, 0)) { SetLastError(errno == EPERM ? ERROR_ACCESS_DENIED : 87); return NULL; }
    Obj *o = obj_new(H_PROC);
    o->pid = (pid_t)pid;
    return o;
}
/* mutex: owned by one thread (recursively); a named one also holds its lock file. process: signalled once it is gone. */
static DWORD wait_special(Obj *o, DWORD ms) {
    int64_t until = ms == INFINITE ? INT64_MAX : now_ns() + (int64_t)ms * 1000000;
    if (o->kind == H_PROC) {
        for (;;) {
            if (kill(o->pid, 0) && errno == ESRCH) return WAIT_OBJECT_0;
            if (now_ns() >= until) return WAIT_TIMEOUT;
            Sleep(10);
        }
    }
    DWORD me = GetCurrentThreadId();
    pthread_mutex_lock(&o->m);
    for (;;) {
        if (o->owned && o->owner == me) { o->owned++; pthread_mutex_unlock(&o->m); return WAIT_OBJECT_0; }
        if (!o->owned) {
            if (o->fd < 0 || !flock(o->fd, LOCK_EX | LOCK_NB)) { o->owned = 1; o->owner = me; pthread_mutex_unlock(&o->m); return WAIT_OBJECT_0; }
            pthread_mutex_unlock(&o->m);              /* another process holds the file: poll */
            if (now_ns() >= until) return WAIT_TIMEOUT;
            Sleep(2);
            pthread_mutex_lock(&o->m);
            continue;
        }
        if (now_ns() >= until) { pthread_mutex_unlock(&o->m); return WAIT_TIMEOUT; }
        if (until == INT64_MAX) pthread_cond_wait(&o->c, &o->m);
        else {                                              /* [Recomps#1] RELATIVE, as in WaitForSingleObject above */
            int64_t left = until - now_ns();
            struct timespec rel = { left / 1000000000, left % 1000000000 };
            pthread_cond_timedwait_relative_np(&o->c, &o->m, &rel);
        }
    }
}

/* ---- CryptoAPI subset (wincrypt.h): random bytes, SHA-1 (RFC 3174) and MD5 (RFC 1321) */
#include "wincrypt.h"
#include <math.h>
typedef struct { ALG_ID alg; uint64_t len; uint8_t buf[64]; size_t n; uint32_t h[5]; int done; uint8_t out[20]; } Hash;
static uint32_t rol32(uint32_t x, int s) { return (x << s) | (x >> (32 - s)); }
static void sha1_block(uint32_t *h, const uint8_t *p) {
    uint32_t w[80], a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
    for (int i = 0; i < 16; i++) w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
    for (int i = 16; i < 80; i++) w[i] = rol32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999; }
        else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1; }
        else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
        else { f = b ^ c ^ d; k = 0xCA62C1D6; }
        uint32_t t = rol32(a, 5) + f + e + k + w[i];
        e = d; d = c; c = rol32(b, 30); b = a; a = t;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
}
static void md5_block(uint32_t *h, const uint8_t *p) {
    static uint32_t K[64];
    static const int S[4][4] = {{7, 12, 17, 22}, {5, 9, 14, 20}, {4, 11, 16, 23}, {6, 10, 15, 21}};
    if (!K[0]) for (int i = 0; i < 64; i++) K[i] = (uint32_t)(fabs(sin((double)(i + 1))) * 4294967296.0);
    uint32_t m[16], a = h[0], b = h[1], c = h[2], d = h[3];
    for (int i = 0; i < 16; i++) m[i] = (uint32_t)p[4 * i] | (uint32_t)p[4 * i + 1] << 8 | (uint32_t)p[4 * i + 2] << 16 | (uint32_t)p[4 * i + 3] << 24;
    for (int i = 0; i < 64; i++) {
        uint32_t f;
        int g;
        if (i < 16) { f = (b & c) | (~b & d); g = i; }
        else if (i < 32) { f = (d & b) | (~d & c); g = (5 * i + 1) % 16; }
        else if (i < 48) { f = b ^ c ^ d; g = (3 * i + 5) % 16; }
        else { f = c ^ (b | ~d); g = (7 * i) % 16; }
        f += a + K[i] + m[g];
        a = d; d = c; c = b;
        b += rol32(f, S[i / 16][i % 4]);
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d;
}
static void hash_block(Hash *x, const uint8_t *p) { if (x->alg == CALG_SHA1) sha1_block(x->h, p); else md5_block(x->h, p); }
BOOL CryptAcquireContextA(HCRYPTPROV *prov, LPCSTR container, LPCSTR provider, DWORD type, DWORD flags) {
    (void)container; (void)provider; (void)type; (void)flags;
    *prov = 1;
    return TRUE;
}
BOOL CryptReleaseContext(HCRYPTPROV prov, DWORD flags) { (void)prov; (void)flags; return TRUE; }
BOOL CryptGenRandom(HCRYPTPROV prov, DWORD n, BYTE *out) { (void)prov; return SystemFunction036(out, n); }
BOOL CryptCreateHash(HCRYPTPROV prov, ALG_ID alg, HCRYPTKEY key, DWORD flags, HCRYPTHASH *out) {
    (void)prov; (void)key; (void)flags;
    if (alg != CALG_SHA1 && alg != CALG_MD5) { SetLastError(0x80090008u); return FALSE; }   /* NTE_BAD_ALGID */
    Hash *x = calloc(1, sizeof *x);
    x->alg = alg;
    x->h[0] = 0x67452301; x->h[1] = 0xEFCDAB89; x->h[2] = 0x98BADCFE; x->h[3] = 0x10325476; x->h[4] = 0xC3D2E1F0;
    *out = (HCRYPTHASH)x;
    return TRUE;
}
BOOL CryptHashData(HCRYPTHASH hh, const BYTE *data, DWORD n, DWORD flags) {
    (void)flags;
    Hash *x = (Hash *)hh;
    if (!x || x->done) return FALSE;
    x->len += n;
    while (n) {
        size_t k = 64 - x->n < n ? 64 - x->n : n;
        memcpy(x->buf + x->n, data, k);
        x->n += k; data += k; n -= (DWORD)k;
        if (x->n == 64) { hash_block(x, x->buf); x->n = 0; }
    }
    return TRUE;
}
BOOL CryptGetHashParam(HCRYPTHASH hh, DWORD param, BYTE *out, DWORD *n, DWORD flags) {
    (void)flags;
    Hash *x = (Hash *)hh;
    DWORD size = x->alg == CALG_SHA1 ? 20 : 16;
    if (param == HP_HASHSIZE) { if (*n < 4) return FALSE; memcpy(out, &size, 4); *n = 4; return TRUE; }
    if (param != HP_HASHVAL) return FALSE;
    if (!out) { *n = size; return TRUE; }
    if (*n < size) { *n = size; SetLastError(234); return FALSE; }   /* ERROR_MORE_DATA */
    if (!x->done) {                                       /* padding, then the length in bits */
        uint64_t bits = x->len * 8;
        x->buf[x->n++] = 0x80;
        if (x->n > 56) { memset(x->buf + x->n, 0, 64 - x->n); hash_block(x, x->buf); x->n = 0; }
        memset(x->buf + x->n, 0, 56 - x->n);
        for (int i = 0; i < 8; i++) x->buf[56 + i] = (uint8_t)(x->alg == CALG_SHA1 ? bits >> (56 - 8 * i) : bits >> (8 * i));
        hash_block(x, x->buf);
        for (int i = 0; i < (int)size / 4; i++)
            for (int k = 0; k < 4; k++) x->out[4 * i + k] = (uint8_t)(x->alg == CALG_SHA1 ? x->h[i] >> (24 - 8 * k) : x->h[i] >> (8 * k));
        x->done = 1;
    }
    memcpy(out, x->out, size);
    *n = size;
    return TRUE;
}
BOOL CryptDestroyHash(HCRYPTHASH hh) { free((Hash *)hh); return TRUE; }

/* ---- the user's language (nnmisc.c desired_language) */
static int user_locale(char *out, size_t n) {
    const char *v = getenv("LANGUAGE");
    char b[64];
    if (!v || !*v) v = getenv("LC_ALL");
    if (!v || !*v) v = getenv("LC_MESSAGES");
    if (!v || !*v) v = getenv("LANG");
    if (!v || !*v || !strcmp(v, "C") || !strcmp(v, "POSIX") || !strncmp(v, "C.", 2)) return 0;
    snprintf(b, sizeof b, "%s", v);
    b[strcspn(b, ":.@")] = 0;                             /* first of a LANGUAGE list; no codeset / modifier */
    for (char *c = b; *c; c++) if (*c == '_') *c = '-';
    snprintf(out, n, "%s", b);
    return out[0] != 0;
}
BOOL GetUserPreferredUILanguages(DWORD flags, ULONG *count, WCHAR *list, ULONG *size) {
    (void)flags;
    char name[64];
    if (!user_locale(name, sizeof name)) { *count = 0; return FALSE; }
    ULONG need = (ULONG)strlen(name) + 2;                 /* the name, its NUL and the list's closing NUL */
    if (!list || *size < need) { *size = need; *count = 1; return list == NULL; }
    MultiByteToWideChar(CP_UTF8, 0, name, -1, list, (int)*size);
    list[need - 1] = 0;
    *count = 1; *size = need;
    return TRUE;
}
int GetUserDefaultLocaleName(WCHAR *out, int n) {
    char name[64];
    if (!user_locale(name, sizeof name)) snprintf(name, sizeof name, "en-US");
    return MultiByteToWideChar(CP_UTF8, 0, name, -1, out, n);
}

/* ---- semaphores (the count is kept in Obj.owned, the maximum in Obj.size), thread priority, system info */
HANDLE CreateSemaphoreA(SECURITY_ATTRIBUTES *sa, LONG initial, LONG max, LPCSTR name) {
    (void)sa; (void)name;
    if (max <= 0 || initial < 0 || initial > max) { SetLastError(87); return NULL; }
    Obj *o = obj_new(H_SEM);
    o->owned = initial; o->size = (uint64_t)max;
    return o;
}
BOOL ReleaseSemaphore(HANDLE h, LONG n, LONG *previous) {
    Obj *o = h;
    if (!o || o->kind != H_SEM || n <= 0) return FALSE;
    pthread_mutex_lock(&o->m);
    if ((uint64_t)o->owned + (uint64_t)n > o->size) { pthread_mutex_unlock(&o->m); SetLastError(298); return FALSE; }   /* ERROR_TOO_MANY_POSTS */
    if (previous) *previous = o->owned;
    o->owned += n;
    pthread_cond_broadcast(&o->c);
    pthread_mutex_unlock(&o->m);
    return TRUE;
}
int GetThreadPriority(HANDLE h) { (void)h; return THREAD_PRIORITY_NORMAL; }
void GetSystemInfo(SYSTEM_INFO *si) {
    memset(si, 0, sizeof *si);
#if defined(__aarch64__) || defined(__arm64__)
    si->wProcessorArchitecture = 12;                     /* PROCESSOR_ARCHITECTURE_ARM64 */
#else
    si->wProcessorArchitecture = 9;                       /* PROCESSOR_ARCHITECTURE_AMD64 */
#endif
    si->dwPageSize = (DWORD)sysconf(_SC_PAGESIZE);
    si->dwAllocationGranularity = (DWORD)GRAN;
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    si->dwNumberOfProcessors = n > 0 ? (DWORD)n : 1;
    si->dwActiveProcessorMask = si->dwNumberOfProcessors >= 64 ? ~(DWORD_PTR)0 : (((DWORD_PTR)1 << si->dwNumberOfProcessors) - 1);
    si->lpMinimumApplicationAddress = (void *)(uintptr_t)0x10000;
    si->lpMaximumApplicationAddress = (void *)(uintptr_t)0x7ffffffeffff;
}

/* ---- [Recomps#1] the freeze watchdog's thread report on macOS (nvn_vk_wd.c; windows.h: LxThread).
 * On macOS we don't have tgkill/SIGRTMIN; a signal-based version uses a free fixed signal and
 * pthread_from_mach_thread_np (plan step 3). For now: stubs return "no answer / not available". */
#include <execinfo.h>
#include <mach-o/loader.h>
#include <mach-o/nlist.h>
#include <mach-o/dyld.h>
static struct { volatile pid_t tid; volatile int seq, done, n; uint64_t rip; void *pc[64]; } g_stk;
static int lx_read_small(const char *path, char *out, size_t n);
/* [Recomps#1] Is [addr, addr+len) mapped and writable? A Mach trap, callable from the signal handler: it must
 * validate every frame pointer BEFORE it is dereferenced - the frame-pointer chain of lifted code and of a
 * fiber's stack does not end cleanly (the first frame's saved fp is zero or stale), and libc backtrace()
 * follows it into unmapped memory and takes the process down (measured: EXC_BAD_ACCESS in __thread_stack_pcs). */
static int stk_mapped(uint64_t addr, size_t len) {
    mach_vm_address_t a = addr;
    mach_vm_size_t sz = 0;
    vm_region_basic_info_data_64_t info;
    mach_msg_type_number_t cnt = VM_REGION_BASIC_INFO_COUNT_64;
    mach_port_t obj = MACH_PORT_NULL;
    if (mach_vm_region(mach_task_self(), &a, &sz, VM_REGION_BASIC_INFO_64, (vm_region_info_t)&info, &cnt, &obj) != KERN_SUCCESS) return 0;
    if (obj != MACH_PORT_NULL) mach_port_deallocate(mach_task_self(), obj);
    if (a > addr) return 0;                        /* the address falls in a hole: the region found starts later */
    return (info.protection & VM_PROT_WRITE) != 0 && addr + len <= a + sz;
}
static void stk_handler(int sig, siginfo_t *si, void *ucv) {
    int e = errno;
    (void)sig; (void)si;
    uint64_t mytid; pthread_threadid_np(NULL, &mytid);
    if (mytid == g_stk.tid) {
        int seq = g_stk.seq;
        ucontext_t *uc = (ucontext_t *)ucv;
        void *pc[64];
        int n = 0;
        uint64_t fp = (uint64_t)uc->uc_mcontext->__ss.__fp, prev = 0;
        int first = 1;                                     /* the interrupted frame's fp needs no step check yet */
        pc[n++] = (void *)(uintptr_t)uc->uc_mcontext->__ss.__pc;      /* the interrupted frame itself */
        while (n < 64 && fp && !(fp & 15) && stk_mapped(fp, 16) && (first || (fp > prev && fp - prev < (1u << 20)))) {
            uint64_t ra = *(volatile uint64_t *)(fp + 8), next = *(volatile uint64_t *)fp;
            pc[n++] = (void *)(uintptr_t)ra;
            prev = fp;
            fp = next;
            first = 0;
        }
        if (g_stk.seq == seq && !g_stk.done) {
            g_stk.rip = (uint64_t)uc->uc_mcontext->__ss.__pc;
            memcpy(g_stk.pc, pc, sizeof pc); g_stk.n = n;
            __atomic_store_n(&g_stk.done, 1, __ATOMIC_RELEASE);
        }
    }
    errno = e;
}
int lx_thread_stack(DWORD tid, uint64_t *pcs, int max) {
    (void)pcs; (void)max;
    static int sig;
    static pthread_mutex_t lk = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&lk);
    if (!sig) {
        struct sigaction sa;
        memset(&sa, 0, sizeof sa);
        sa.sa_sigaction = stk_handler; sa.sa_flags = SA_SIGINFO | SA_RESTART; sigemptyset(&sa.sa_mask);
        sig = SIGURG;     /* [Recomps#1] free fixed signal on Darwin; see plan step 3 */
        if (sigaction(sig, &sa, NULL)) sig = -1;
    }
    if (sig < 0) { pthread_mutex_unlock(&lk); return -3; }
    g_stk.done = 0; g_stk.n = 0; g_stk.seq++; g_stk.tid = (pid_t)tid;
    /* find pthread_t for this tid via mach and signal it */
    pthread_t pt = NULL;
    thread_act_port_array_t list; mach_msg_type_number_t cnt = 0;
    if (task_threads(mach_task_self(), &list, &cnt) == KERN_SUCCESS) {
        for (mach_msg_type_number_t i = 0; i < cnt; i++) {
            thread_identifier_info_data_t tii; mach_msg_type_number_t c = THREAD_IDENTIFIER_INFO_COUNT;
            if (thread_info(list[i], THREAD_IDENTIFIER_INFO, (thread_info_t)&tii, &c) == KERN_SUCCESS && (DWORD)tii.thread_id == tid) {
                pt = pthread_from_mach_thread_np(list[i]); break;
            }
        }
        for (mach_msg_type_number_t i = 0; i < cnt; i++) mach_port_deallocate(mach_task_self(), list[i]);
        vm_deallocate(mach_task_self(), (vm_address_t)list, cnt * sizeof(thread_port_t));
    }
    if (!pt) { g_stk.tid = 0; pthread_mutex_unlock(&lk); return -1; }
    if (pthread_kill(pt, sig)) { g_stk.tid = 0; pthread_mutex_unlock(&lk); return -1; }
    for (int i = 0; i < 200 && !__atomic_load_n(&g_stk.done, __ATOMIC_ACQUIRE); i++) { struct timespec ts = {0, 1000000}; nanosleep(&ts, NULL); }
    int n = -2;
    if (__atomic_load_n(&g_stk.done, __ATOMIC_ACQUIRE)) {
        int s = -1;
        for (int i = 0; i < g_stk.n; i++) if ((uint64_t)(uintptr_t)g_stk.pc[i] == g_stk.rip) { s = i; break; }
        n = 0;
        if (s < 0) { if (max > 0) pcs[n++] = g_stk.rip; s = 3; }
        for (int i = s; i < g_stk.n && n < max; i++) pcs[n++] = (uint64_t)(uintptr_t)g_stk.pc[i];
    }
    g_stk.tid = 0;
    pthread_mutex_unlock(&lk);
    return n;
}
static int lx_read_small(const char *path, char *out, size_t n) { (void)path; (void)out; (void)n; out[0] = 0; return 0; }
/* [Recomps#1] Darwin: the thread report of the freeze watchdog on Mach (plan step 3: libproc + Mach). The kernel wait
 * (wchan) is not read - the stack of the thread names it (libsystem_kernel's __psynch_* / __*wait* wrappers); the CPU
 * times come from THREAD_BASIC_INFO, the ids from THREAD_IDENTIFIER_INFO, the names from pthread. */
static void lx_fill(LxThread *t, thread_port_t port, pthread_t pt) {
    memset(t, 0, sizeof *t);
    thread_identifier_info_data_t tii;
    mach_msg_type_number_t c = THREAD_IDENTIFIER_INFO_COUNT;
    if (thread_info(port, THREAD_IDENTIFIER_INFO, (thread_info_t)&tii, &c) == KERN_SUCCESS) t->tid = (DWORD)tii.thread_id;
    thread_basic_info_data_t bi;
    mach_msg_type_number_t bc = THREAD_BASIC_INFO_COUNT;
    if (thread_info(port, THREAD_BASIC_INFO, (thread_info_t)&bi, &bc) == KERN_SUCCESS) {
        t->cpu_ns = (uint64_t)bi.user_time.seconds * 1000000000ull + (uint64_t)bi.user_time.microseconds * 1000ull
                  + (uint64_t)bi.system_time.seconds * 1000000000ull + (uint64_t)bi.system_time.microseconds * 1000ull;
        t->state = bi.run_state == TH_STATE_RUNNING ? 'R' : bi.run_state == TH_STATE_UNINTERRUPTIBLE ? 'D' : bi.run_state == TH_STATE_STOPPED ? 'T' : 'S';
    }
    char nm[64];
    if (pt && pthread_getname_np(pt, nm, sizeof nm) == 0 && nm[0]) snprintf(t->comm, sizeof t->comm, "%s", nm);
    else snprintf(t->comm, sizeof t->comm, "-");
    t->sysno = -1;                                          /* no Linux system-call number here; the stack names the wait */
}
static int lx_mach_thread_of(DWORD tid, thread_port_t *out) {      /* the Mach port of a thread id; the caller deallocates it */
    thread_act_port_array_t list;
    mach_msg_type_number_t cnt = 0;
    if (task_threads(mach_task_self(), &list, &cnt) != KERN_SUCCESS) return 0;
    int found = 0;
    for (mach_msg_type_number_t i = 0; i < cnt; i++) {
        thread_identifier_info_data_t tii;
        mach_msg_type_number_t c = THREAD_IDENTIFIER_INFO_COUNT;
        if (!found && thread_info(list[i], THREAD_IDENTIFIER_INFO, (thread_info_t)&tii, &c) == KERN_SUCCESS && (DWORD)tii.thread_id == tid) { *out = list[i]; found = 1; }
        else mach_port_deallocate(mach_task_self(), list[i]);
    }
    vm_deallocate(mach_task_self(), (vm_address_t)list, cnt * sizeof(thread_port_t));
    return found;
}
int lx_thread_read(LxThread *t) {
    thread_port_t port;
    if (!lx_mach_thread_of(t->tid, &port)) return 0;
    LxThread nf;
    lx_fill(&nf, port, pthread_from_mach_thread_np(port));
    mach_port_deallocate(mach_task_self(), port);
    if (!nf.tid) return 0;
    t->state = nf.state; t->cpu_ns = nf.cpu_ns; t->nice = nf.nice; t->sysno = -1; t->wchan[0] = 0;
    return 1;
}
int lx_threads(LxThread *t, int max) {
    thread_act_port_array_t list;
    mach_msg_type_number_t cnt = 0;
    if (task_threads(mach_task_self(), &list, &cnt) != KERN_SUCCESS) return 0;
    int n = 0;
    for (mach_msg_type_number_t i = 0; i < cnt && n < max; i++) {
        lx_fill(&t[n], list[i], pthread_from_mach_thread_np(list[i]));
        if (t[n].tid) n++;
        mach_port_deallocate(mach_task_self(), list[i]);
    }
    vm_deallocate(mach_task_self(), (vm_address_t)list, cnt * sizeof(thread_port_t));
    return n;
}
typedef struct { uint64_t a; char name[68]; } LxSym;
static LxSym *g_lxs;
static int g_nlxs, g_lxs_tried;
static int lxs_cmp(const void *x, const void *y) { const LxSym *p = x, *q = y; return p->a < q->a ? -1 : p->a > q->a; }
static void lxs_load(void) {
    g_lxs_tried = 1;
    /* [Recomps#1] Darwin: Mach-O LC_SYMTAB parsing for the main image; plan step 3 */
}
const char *lx_exe_symbol(uint64_t pc, uint64_t *off) {
    if (!g_lxs_tried) lxs_load();
    int lo = 0, hi = g_nlxs - 1, at = -1;
    while (lo <= hi) { int mid = (lo + hi) / 2; if (g_lxs[mid].a <= pc) { at = mid; lo = mid + 1; } else hi = mid - 1; }
    if (at < 0 || pc - g_lxs[at].a >= 0x100000) return NULL;
    if (off) *off = pc - g_lxs[at].a;
    return g_lxs[at].name;
}
