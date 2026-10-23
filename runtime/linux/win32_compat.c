/* [Recomps#1] Linux only: implementations behind runtime/linux/windows.h (notes/linux-port-plan.md step 1).
 * Kernel objects (threads, events, waitable timers) are one handle type with a mutex, a condition and a "signalled"
 * flag; fibers are ucontext; virtual memory keeps its reservations in a table so MEM_RELEASE and VirtualQuery answer as
 * Windows does; paths accept '\\'. Never built on Windows. */
#include "windows.h"
#include "psapi.h"
#include <dirent.h>
#include <dlfcn.h>
#include <fnmatch.h>
#include <limits.h>
#include <fcntl.h>
#include <link.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/random.h>
#include <sys/resource.h>
#include <pwd.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <ucontext.h>

/* ---- process arguments and environment */
int __argc;
char **__argv;
__attribute__((constructor)) static void compat_args(int argc, char **argv, char **envp) { (void)envp; __argc = argc; __argv = argv; }
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
DWORD GetCurrentProcessorNumber(void) { int c = sched_getcpu(); return c < 0 ? 0 : (DWORD)c; }
static struct timespec deadline_ms(DWORD ms) {           /* CLOCK_MONOTONIC deadline ms from now */
    int64_t d = now_ns() + (int64_t)ms * 1000000;
    return (struct timespec){ d / 1000000000, d % 1000000000 };
}
static struct timespec ts_of(int64_t ns) { return (struct timespec){ ns / 1000000000, ns % 1000000000 }; }

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
static BOOL cv_sleep(CONDITION_VARIABLE *v, DWORD ms, void (*release)(void *), void (*acquire)(void *), void *lock) {
    struct timespec dl = ms == INFINITE ? (struct timespec){0, 0} : deadline_ms(ms);
    int timed_out = 0;
    pthread_mutex_lock(&v->m);
    uint64_t seq = v->seq;
    release(lock);
    while (v->seq == seq) {
        if (ms == INFINITE) pthread_cond_wait(&v->c, &v->m);
        else if (pthread_cond_clockwait(&v->c, &v->m, CLOCK_MONOTONIC, &dl) == ETIMEDOUT) { timed_out = v->seq == seq; break; }
    }
    pthread_mutex_unlock(&v->m);
    acquire(lock);
    if (timed_out) { SetLastError(ERROR_TIMEOUT); return FALSE; }
    return TRUE;
}
static void cs_release(void *l) { LeaveCriticalSection(l); }
static void cs_acquire(void *l) { EnterCriticalSection(l); }
static void srw_release(void *l) { ReleaseSRWLockExclusive(l); }   /* pthread_rwlock_unlock releases either kind */
static void srw_acquire_x(void *l) { AcquireSRWLockExclusive(l); }
static void srw_acquire_s(void *l) { AcquireSRWLockShared(l); }
BOOL SleepConditionVariableCS(CONDITION_VARIABLE *v, CRITICAL_SECTION *c, DWORD ms) { return cv_sleep(v, ms, cs_release, cs_acquire, c); }
BOOL SleepConditionVariableSRW(CONDITION_VARIABLE *v, SRWLOCK *s, DWORD ms, ULONG flags) {
    return cv_sleep(v, ms, srw_release, (flags & CONDITION_VARIABLE_LOCKMODE_SHARED) ? srw_acquire_s : srw_acquire_x, s);
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
    pthread_condattr_t a; pthread_condattr_init(&a); pthread_condattr_setclock(&a, CLOCK_MONOTONIC);
    pthread_cond_init(&o->c, &a); pthread_condattr_destroy(&a);
    return o;
}
static void obj_unref(Obj *o) {
    if (__atomic_sub_fetch(&o->refs, 1, __ATOMIC_ACQ_REL)) return;
    if (o->fd >= 0) close(o->fd);
    pthread_mutex_destroy(&o->m); pthread_cond_destroy(&o->c); free(o);
}
DWORD GetCurrentThreadId(void) { return (DWORD)syscall(SYS_gettid); }
static void *thread_main(void *p) {
    Obj *o = p;
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
    cpu_set_t s, old;
    pid_t t = tid_of(h);
    DWORD_PTR prev = 0;
    if (!sched_getaffinity(t, sizeof old, &old)) for (int i = 0; i < 64; i++) if (CPU_ISSET(i, &old)) prev |= (DWORD_PTR)1 << i;
    CPU_ZERO(&s);
    for (int i = 0; i < 64; i++) if (mask >> i & 1) CPU_SET(i, &s);
    return sched_setaffinity(t, sizeof s, &s) ? 0 : (prev ? prev : 1);
}
BOOL GetProcessAffinityMask(HANDLE p, DWORD_PTR *proc, DWORD_PTR *sys) {
    (void)p;
    cpu_set_t s;
    DWORD_PTR m = 0;
    if (sched_getaffinity(0, sizeof s, &s)) return FALSE;
    for (int i = 0; i < 64; i++) if (CPU_ISSET(i, &s)) m |= (DWORD_PTR)1 << i;
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
        else { struct timespec t = ts_of(wake); pthread_cond_timedwait(&o->c, &o->m, &t); }   /* cond made on CLOCK_MONOTONIC */
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
    size_t page = 4096, size = (reserve ? reserve : 1 << 20) + page;
    uint8_t *mem = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_STACK, -1, 0);
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
#define WW_UFFD_API 0xAAull
#define WW_UFFD_USER_MODE_ONLY 1
#define WW_UFFD_FEATURE_WP_UNPOPULATED (1ull << 13)
#define WW_UFFD_FEATURE_WP_ASYNC (1ull << 15)
#define WW_UFFDIO_REGISTER_MODE_WP (1ull << 1)
#define WW_UFFDIO_WRITEPROTECT_MODE_WP (1ull << 0)
struct ww_uffdio_api { uint64_t api, features, ioctls; };
struct ww_uffdio_range { uint64_t start, len; };
struct ww_uffdio_register { struct ww_uffdio_range range; uint64_t mode, ioctls; };
struct ww_uffdio_writeprotect { struct ww_uffdio_range range; uint64_t mode; };
#define WW_UFFDIO_API _IOWR(0xAA, 0x3F, struct ww_uffdio_api)
#define WW_UFFDIO_REGISTER _IOWR(0xAA, 0x00, struct ww_uffdio_register)
#define WW_UFFDIO_WRITEPROTECT _IOWR(0xAA, 0x06, struct ww_uffdio_writeprotect)
struct ww_page_region { uint64_t start, end, categories; };
struct ww_pm_scan_arg { uint64_t size, flags, start, end, walk_end, vec, vec_len, max_pages, category_inverted, category_mask, category_anyof_mask, return_mask; };
#define WW_PAGEMAP_SCAN _IOWR('f', 16, struct ww_pm_scan_arg)
#define WW_PM_SCAN_WP_MATCHING (1ull << 0)
#define WW_PM_SCAN_CHECK_WPASYNC (1ull << 1)
#define WW_PAGE_IS_WRITTEN (1ull << 1)
#ifndef __NR_userfaultfd
#define __NR_userfaultfd 323                                  /* x86-64 */
#endif
static int g_uffd = -1, g_pagemap = -1;
static pthread_once_t g_ww_once = PTHREAD_ONCE_INIT;
static void ww_init(void) {
    struct ww_uffdio_api api = { WW_UFFD_API, WW_UFFD_FEATURE_WP_ASYNC | WW_UFFD_FEATURE_WP_UNPOPULATED, 0 };
    if (getenv("SSBU_NO_WRITEWATCH")) return;                 /* the renderer's own switch: then nothing asks anyway */
    int fd = (int)syscall(__NR_userfaultfd, O_CLOEXEC | O_NONBLOCK | WW_UFFD_USER_MODE_ONLY);
    if (fd >= 0 && ioctl(fd, WW_UFFDIO_API, &api) == 0) g_pagemap = open("/proc/self/pagemap", O_RDONLY | O_CLOEXEC);
    if (g_pagemap >= 0) { g_uffd = fd; printf("win32_compat: write watch tracked by the kernel (userfaultfd async write-protect, PAGEMAP_SCAN)\n"); }
    else {
        printf("win32_compat: no write watch from this kernel (userfaultfd async write-protect / PAGEMAP_SCAN, Linux 6.7+: %s): textures are hashed\n", strerror(errno));
        if (fd >= 0) close(fd);
    }
}
static int ww_protect(uintptr_t a, size_t n) {                 /* from now on [a, a+n) counts as not written */
    struct ww_uffdio_writeprotect wp = { { a, n }, WW_UFFDIO_WRITEPROTECT_MODE_WP };
    return ioctl(g_uffd, WW_UFFDIO_WRITEPROTECT, &wp) == 0;
}
static int ww_register(uintptr_t a, size_t n) {
    pthread_once(&g_ww_once, ww_init);
    struct ww_uffdio_register reg = { { a, n }, WW_UFFDIO_REGISTER_MODE_WP, 0 };
    return g_uffd >= 0 && ioctl(g_uffd, WW_UFFDIO_REGISTER, &reg) == 0 && ww_protect(a, n);
}

/* ---- virtual memory */
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
    size_t page = 4096;
    uintptr_t a = (uintptr_t)addr;
    if (!size) { SetLastError(87); return NULL; }
    pthread_mutex_lock(&g_res_m);
    Res *r = a ? res_find(a) : NULL;
    if (r && (type & MEM_COMMIT)) {                       /* commit inside an existing reservation */
        uintptr_t s = a & ~(page - 1), e = (a + size + page - 1) & ~(page - 1);
        int ok = mprotect((void *)s, e - s, prot_of(prot)) == 0;
        if (ok && r->ww) ww_protect(s, e - s);              /* newly committed pages are not written (as on Windows) */
        pthread_mutex_unlock(&g_res_m);
        return ok ? (void *)s : NULL;
    }
    if (r) { pthread_mutex_unlock(&g_res_m); SetLastError(487); return NULL; }   /* ERROR_INVALID_ADDRESS: already reserved */
    size_t n = (size + GRAN - 1) & ~(GRAN - 1);
    int p = (type & MEM_COMMIT) ? prot_of(prot) : PROT_NONE, fl = MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE;
    uint8_t *m;
    if (a) {
        a &= ~(GRAN - 1);
        m = mmap((void *)a, n, p, fl | MAP_FIXED_NOREPLACE, -1, 0);
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
    res_add((uintptr_t)m, n, (type & MEM_WRITE_WATCH) && ww_register((uintptr_t)m, n));
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
        size_t page = 4096;
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
    uintptr_t s = (uintptr_t)base & ~(uintptr_t)4095, e = ((uintptr_t)base + n + 4095) & ~(uintptr_t)4095;
    pthread_mutex_lock(&g_res_m);
    Res *r = n ? res_find(s) : NULL;
    int ww = r && r->ww && e <= r->base + r->size;
    pthread_mutex_unlock(&g_res_m);
    if (!ww) return 1;
    *gran = 4096;
    ULONG_PTR cap = *count, k = 0;
    struct ww_page_region vec[256];
    while (s < e && k < cap) {
        struct ww_pm_scan_arg a = { sizeof a, WW_PM_SCAN_CHECK_WPASYNC | ((flags & WRITE_WATCH_FLAG_RESET) ? WW_PM_SCAN_WP_MATCHING : 0), s, e, 0,
                                    (uintptr_t)vec, 256, cap - k, 0, WW_PAGE_IS_WRITTEN, 0, WW_PAGE_IS_WRITTEN };
        long got = ioctl(g_pagemap, WW_PAGEMAP_SCAN, &a);
        if (got < 0) return 1;                                /* cannot ask: as if not watched (the callers hash) */
        for (long i = 0; i < got; i++)
            for (uint64_t pg = vec[i].start; pg < vec[i].end && k < cap; pg += 4096) addrs[k++] = (void *)(uintptr_t)pg;
        if (a.walk_end <= s) break;
        s = a.walk_end;                                       /* the scan stopped early (vec full or max_pages): go on from there */
    }
    *count = k;
    return 0;
}
UINT ResetWriteWatch(void *base, SIZE_T n) {
    uintptr_t s = (uintptr_t)base & ~(uintptr_t)4095, e = ((uintptr_t)base + n + 4095) & ~(uintptr_t)4095;
    pthread_mutex_lock(&g_res_m);
    Res *r = n ? res_find(s) : NULL;
    int ww = r && r->ww && e <= r->base + r->size;
    pthread_mutex_unlock(&g_res_m);
    return !ww || !ww_protect(s, e - s);
}
/* /proc/self/maps: the mapping holding a (or the gap before the next one) */
static int maps_find(uintptr_t a, uintptr_t *lo, uintptr_t *hi, int *prot, uintptr_t *next) {
    FILE *f = fopen("/proc/self/maps", "r");
    char line[512];
    int found = 0;
    *next = UINTPTR_MAX;
    if (!f) return 0;
    while (fgets(line, sizeof line, f)) {
        unsigned long s, e; char perm[8];
        if (sscanf(line, "%lx-%lx %7s", &s, &e, perm) != 3) continue;
        if (a >= s && a < e) {
            *lo = s; *hi = e;
            *prot = (perm[0] == 'r' ? PROT_READ : 0) | (perm[1] == 'w' ? PROT_WRITE : 0) | (perm[2] == 'x' ? PROT_EXEC : 0);
            found = 1; break;
        }
        if (s > a && s < *next) *next = s;
    }
    fclose(f);
    return found;
}
static DWORD page_of(int p) {
    if (p == PROT_NONE) return PAGE_NOACCESS;
    if (p & PROT_EXEC) return (p & PROT_WRITE) ? PAGE_EXECUTE_READWRITE : (p & PROT_READ) ? PAGE_EXECUTE_READ : PAGE_EXECUTE;
    return (p & PROT_WRITE) ? PAGE_READWRITE : PAGE_READONLY;
}
SIZE_T VirtualQuery(const void *addr, MEMORY_BASIC_INFORMATION *mbi, SIZE_T len) {
    if (len < sizeof *mbi) return 0;
    uintptr_t a = (uintptr_t)addr & ~(uintptr_t)4095, lo, hi, next;
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
    uintptr_t s = (uintptr_t)addr & ~(uintptr_t)4095, e = ((uintptr_t)addr + size + 4095) & ~(uintptr_t)4095;
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
    {"vulkan-1.dll", "libvulkan.so.1"}, {"sdl3.dll", "libSDL3.so.0"}, {"opengl32.dll", "libGL.so.1"},
    {"game.dll", "game.so"}, {"ssbu_glslang.dll", "libssbu_glslang.so"}, {"libopus-0.dll", "libopus.so.0"}, {"opus.dll", "libopus.so.0"},
};
static void *open_named(const char *name, int flags) {
    char low[MAX_PATH];
    size_t i;
    for (i = 0; name[i] && i + 1 < sizeof low; i++) low[i] = (char)(name[i] >= 'A' && name[i] <= 'Z' ? name[i] + 32 : name[i]);
    low[i] = 0;
    const char *base = strrchr(low, '\\') ? strrchr(low, '\\') + 1 : strrchr(low, '/') ? strrchr(low, '/') + 1 : low;
    for (size_t k = 0; k < sizeof k_libs / sizeof k_libs[0]; k++)
        if (!strcmp(base, k_libs[k].dll)) {
            if (!strcmp(k_libs[k].so, "game.so") || !strcmp(k_libs[k].so, "libssbu_glslang.so")) {   /* ours: beside the program */
                char p[PATH_MAX], *s;
                if (readlink("/proc/self/exe", p, sizeof p - 1) > 0 && (s = strrchr(p, '/'))) {
                    snprintf(s + 1, sizeof p - (size_t)(s + 1 - p), "%s", k_libs[k].so);
                    void *h = dlopen(p, flags);
                    if (h) return h;
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
    if (!m) k = readlink("/proc/self/exe", p, sizeof p - 1);
    else { struct link_map *lm; if (!dlinfo(m, RTLD_DI_LINKMAP, &lm) && lm->l_name && *lm->l_name) { snprintf(p, sizeof p, "%s", lm->l_name); k = (ssize_t)strlen(p); } }
    if (k <= 0 || !n) return 0;
    p[k] = 0;
    snprintf(out, n, "%s", p);
    for (char *c = out; *c; c++) if (*c == '/') *c = '\\';  /* as Windows spells it: the runtime cuts the path at '\\' (its
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
    ft_of(st.st_ctim.tv_sec, st.st_ctim.tv_nsec, &d->ftCreationTime);
    ft_of(st.st_atim.tv_sec, st.st_atim.tv_nsec, &d->ftLastAccessTime);
    ft_of(st.st_mtim.tv_sec, st.st_mtim.tv_nsec, &d->ftLastWriteTime);
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
    while (len) { ssize_t r = getrandom(p, len, 0); if (r < 0) { if (errno == EINTR) continue; return FALSE; } p += r; len -= (ULONG)r; }
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
            ft_of(st.st_mtim.tv_sec, st.st_mtim.tv_nsec, &fd->ftLastWriteTime);
            ft_of(st.st_ctim.tv_sec, st.st_ctim.tv_nsec, &fd->ftCreationTime);
            ft_of(st.st_atim.tv_sec, st.st_atim.tv_nsec, &fd->ftLastAccessTime);
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
    FILE *f = fopen("/proc/self/status", "r");
    char line[256];
    if (!f || cb < sizeof(DWORD) * 2) { if (f) fclose(f); return FALSE; }
    memset(pmc, 0, cb < sizeof *pmc ? cb : sizeof *pmc);
    while (fgets(line, sizeof line, f)) {
        unsigned long kb;
        if (sscanf(line, "VmRSS: %lu", &kb) == 1) pmc->WorkingSetSize = kb << 10;
        else if (sscanf(line, "VmHWM: %lu", &kb) == 1) pmc->PeakWorkingSetSize = kb << 10;
        else if (sscanf(line, "VmData: %lu", &kb) == 1) { pmc->PagefileUsage = kb << 10; if (cb >= sizeof(PROCESS_MEMORY_COUNTERS_EX)) pmc->PrivateUsage = kb << 10; }
        else if (sscanf(line, "VmPeak: %lu", &kb) == 1) pmc->PeakPagefileUsage = kb << 10;
    }
    fclose(f);
    pmc->cb = cb;
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
    struct iovec l = { to, n }, r = { (void *)from, n };
    ssize_t k = process_vm_readv(getpid(), &l, 1, &r, 1, 0);   /* fails cleanly on unmapped memory, unlike memcpy */
    if (got) *got = k > 0 ? (SIZE_T)k : 0;
    return k == (ssize_t)n;
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
        else { struct timespec t = ts_of(until); pthread_cond_timedwait(&o->c, &o->m, &t); }
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
    si->wProcessorArchitecture = 9;                       /* PROCESSOR_ARCHITECTURE_AMD64 */
    si->dwPageSize = (DWORD)sysconf(_SC_PAGESIZE);
    si->dwAllocationGranularity = (DWORD)GRAN;
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    si->dwNumberOfProcessors = n > 0 ? (DWORD)n : 1;
    si->dwActiveProcessorMask = si->dwNumberOfProcessors >= 64 ? ~(DWORD_PTR)0 : (((DWORD_PTR)1 << si->dwNumberOfProcessors) - 1);
    si->lpMinimumApplicationAddress = (void *)(uintptr_t)0x10000;
    si->lpMaximumApplicationAddress = (void *)(uintptr_t)0x7ffffffeffff;
}

/* ---- [Recomps#1] the freeze watchdog's thread report on Linux (nvn_vk_wd.c; windows.h: LxThread). Windows suspends a thread
 * and unwinds it from outside; here each thread records its own call stack in a signal handler (SIGRTMIN + 7, sent to that
 * one thread with tgkill; SA_RESTART, so a wait it interrupts goes on), with glibc's backtrace() and the unwind tables of
 * the program and game.so. What it waits for comes from /proc/self/task/<tid>: state, kernel wait channel, the system call
 * it is in and its first argument (a futex address). */
#include <execinfo.h>
#include <elf.h>
static struct { volatile pid_t tid; volatile int seq, done, n; uint64_t rip; void *pc[64]; } g_stk;
static int lx_read_small(const char *path, char *out, size_t n);
static void stk_handler(int sig, siginfo_t *si, void *ucv) {
    int e = errno;
    (void)sig; (void)si;
    if ((pid_t)syscall(SYS_gettid) == g_stk.tid) {
        int seq = g_stk.seq;
        void *pc[64];
        int n = backtrace(pc, 64);
        if (g_stk.seq == seq && !g_stk.done) {
            g_stk.rip = (uint64_t)((ucontext_t *)ucv)->uc_mcontext.gregs[REG_RIP];
            memcpy(g_stk.pc, pc, sizeof pc); g_stk.n = n;
            __atomic_store_n(&g_stk.done, 1, __ATOMIC_RELEASE);
        }
    }
    errno = e;
}
int lx_thread_stack(DWORD tid, uint64_t *pcs, int max) {
    static pthread_mutex_t lk = PTHREAD_MUTEX_INITIALIZER;
    static int sig;
    int n = -2;
    pthread_mutex_lock(&lk);
    if (!sig) {
        struct sigaction sa;
        void *w[4];
        memset(&sa, 0, sizeof sa);
        sa.sa_sigaction = stk_handler; sa.sa_flags = SA_SIGINFO | SA_RESTART; sigemptyset(&sa.sa_mask);
        sig = sigaction(SIGRTMIN + 7, &sa, NULL) ? -1 : SIGRTMIN + 7;
        backtrace(w, 4);                                  /* glibc loads its unwinder at the first call: here, never in the handler */
    }
    if (sig < 0) { pthread_mutex_unlock(&lk); return -3; }
    {   /* a thread that blocks the signal (the threads of Mesa, PulseAudio ...) would not answer: not asked (no 200 ms wait each) */
        char p[64], b[2048];
        const char *q;
        unsigned long long blk = 0;
        snprintf(p, sizeof p, "/proc/self/task/%lu/status", (unsigned long)tid);
        if (lx_read_small(p, b, sizeof b) && (q = strstr(b, "SigBlk:")) != NULL && sscanf(q + 7, "%llx", &blk) == 1 && (blk >> (sig - 1) & 1)) { pthread_mutex_unlock(&lk); return -4; }
    }
    g_stk.done = 0; g_stk.n = 0; g_stk.seq++; g_stk.tid = (pid_t)tid;
    if (syscall(SYS_tgkill, getpid(), (pid_t)tid, sig)) { g_stk.tid = 0; pthread_mutex_unlock(&lk); return -1; }
    for (int i = 0; i < 200 && !__atomic_load_n(&g_stk.done, __ATOMIC_ACQUIRE); i++) { struct timespec ts = {0, 1000000}; nanosleep(&ts, NULL); }
    if (__atomic_load_n(&g_stk.done, __ATOMIC_ACQUIRE)) {
        int s = -1;
        for (int i = 0; i < g_stk.n; i++) if ((uint64_t)(uintptr_t)g_stk.pc[i] == g_stk.rip) { s = i; break; }      /* the frames before it: the handler and the signal trampoline */
        n = 0;
        if (s < 0) { if (max > 0) pcs[n++] = g_stk.rip; s = 3; }
        for (int i = s; i < g_stk.n && n < max; i++) pcs[n++] = (uint64_t)(uintptr_t)g_stk.pc[i];
    }
    g_stk.tid = 0;                                         /* a late handler (a thread that blocked the signal) records nothing */
    pthread_mutex_unlock(&lk);
    return n;
}
static int lx_read_small(const char *path, char *out, size_t n) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    ssize_t k = fd >= 0 ? read(fd, out, n - 1) : -1;
    if (fd >= 0) close(fd);
    if (k < 0) { out[0] = 0; return 0; }
    out[k] = 0;
    while (k > 0 && (out[k - 1] == '\n' || out[k - 1] == ' ')) out[--k] = 0;
    return 1;
}
int lx_thread_read(LxThread *t) {
    char p[96], b[512];
    snprintf(p, sizeof p, "/proc/self/task/%lu/stat", (unsigned long)t->tid);
    if (!lx_read_small(p, b, sizeof b)) return 0;
    const char *o = strchr(b, '('), *c = strrchr(b, ')');
    if (o && c && c > o) { size_t l = (size_t)(c - o - 1); if (l >= sizeof t->comm) l = sizeof t->comm - 1; memcpy(t->comm, o + 1, l); t->comm[l] = 0; }
    if (c) {                                               /* after the name: state ppid pgrp session tty tpgid flags minflt cminflt majflt cmajflt utime stime cutime cstime priority nice */
        unsigned long long ut = 0, st = 0; long pri = 0, ni = 0;
        sscanf(c + 2, "%c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %llu %llu %*d %*d %ld %ld", &t->state, &ut, &st, &pri, &ni);
        t->nice = (int)ni;
        t->cpu_ns = (ut + st) * (1000000000ull / (unsigned long long)sysconf(_SC_CLK_TCK));
    }
    snprintf(p, sizeof p, "/proc/self/task/%lu/schedstat", (unsigned long)t->tid);      /* nanoseconds on a CPU: finer than the clock ticks of stat */
    if (lx_read_small(p, b, sizeof b)) { unsigned long long ns; if (sscanf(b, "%llu", &ns) == 1) t->cpu_ns = ns; }
    snprintf(p, sizeof p, "/proc/self/task/%lu/wchan", (unsigned long)t->tid);
    lx_read_small(p, t->wchan, sizeof t->wchan);
    snprintf(p, sizeof p, "/proc/self/task/%lu/syscall", (unsigned long)t->tid);      /* "<nr> <arg1> ... <sp> <pc>", "running" or "-1 <sp> <pc>" */
    t->sysno = -1; t->sysarg[0] = t->sysarg[1] = 0;
    if (lx_read_small(p, b, sizeof b) && b[0] >= '0' && b[0] <= '9') { unsigned long long a0 = 0, a1 = 0; long nr = -1; if (sscanf(b, "%ld %llx %llx", &nr, &a0, &a1) >= 1) { t->sysno = nr; t->sysarg[0] = a0; t->sysarg[1] = a1; } }
    return 1;
}
int lx_threads(LxThread *t, int max) {
    DIR *d = opendir("/proc/self/task");
    struct dirent *e;
    int n = 0;
    if (!d) return 0;
    while (n < max && (e = readdir(d)) != NULL) {
        if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;
        memset(&t[n], 0, sizeof t[n]);
        t[n].tid = (DWORD)strtoul(e->d_name, NULL, 10);
        if (lx_thread_read(&t[n])) n++;
    }
    closedir(d);
    return n;
}
typedef struct { uint64_t a; char name[68]; } LxSym;
static LxSym *g_lxs;
static int g_nlxs, g_lxs_tried;
static int lxs_cmp(const void *x, const void *y) { const LxSym *p = x, *q = y; return p->a < q->a ? -1 : p->a > q->a; }
static void lxs_load(void) {                               /* the functions of the program's own symbol table (.symtab: the build keeps it) */
    Elf64_Ehdr eh;
    Elf64_Shdr *sh = NULL;
    Elf64_Sym *sy = NULL;
    char *str = NULL;
    Dl_info di;
    FILE *f = fopen("/proc/self/exe", "rb");
    g_lxs_tried = 1;
    if (!f) return;
    if (fread(&eh, sizeof eh, 1, f) != 1 || memcmp(eh.e_ident, ELFMAG, SELFMAG) || eh.e_ident[EI_CLASS] != ELFCLASS64 || eh.e_shentsize != sizeof(Elf64_Shdr) || !eh.e_shnum) goto done;
    sh = calloc(eh.e_shnum, sizeof *sh);
    if (!sh || fseek(f, (long)eh.e_shoff, SEEK_SET) || fread(sh, sizeof *sh, eh.e_shnum, f) != eh.e_shnum) goto done;
    for (int i = 0; i < eh.e_shnum; i++) {
        if (sh[i].sh_type != SHT_SYMTAB || sh[i].sh_link >= eh.e_shnum) continue;
        size_t ns = sh[i].sh_size / sizeof *sy, nstr = sh[sh[i].sh_link].sh_size;
        sy = malloc(sh[i].sh_size); str = malloc(nstr + 1);
        if (!sy || !str || fseek(f, (long)sh[i].sh_offset, SEEK_SET) || fread(sy, sizeof *sy, ns, f) != ns) goto done;
        if (fseek(f, (long)sh[sh[i].sh_link].sh_offset, SEEK_SET) || fread(str, 1, nstr, f) != nstr) goto done;
        str[nstr] = 0;
        uint64_t bias = eh.e_type == ET_DYN && dladdr((void *)lxs_load, &di) ? (uint64_t)(uintptr_t)di.dli_fbase : 0;
        g_lxs = calloc(ns ? ns : 1, sizeof *g_lxs);
        if (!g_lxs) goto done;
        for (size_t k = 0; k < ns; k++) {
            if (ELF64_ST_TYPE(sy[k].st_info) != STT_FUNC || !sy[k].st_value || sy[k].st_shndx == SHN_UNDEF || sy[k].st_name >= nstr) continue;
            g_lxs[g_nlxs].a = sy[k].st_value + bias;
            snprintf(g_lxs[g_nlxs].name, sizeof g_lxs[0].name, "%s", str + sy[k].st_name);
            g_nlxs++;
        }
        qsort(g_lxs, (size_t)g_nlxs, sizeof *g_lxs, lxs_cmp);
        break;
    }
done:
    free(sh); free(sy); free(str); fclose(f);
}
const char *lx_exe_symbol(uint64_t pc, uint64_t *off) {
    if (!g_lxs_tried) lxs_load();
    int lo = 0, hi = g_nlxs - 1, at = -1;
    while (lo <= hi) { int mid = (lo + hi) / 2; if (g_lxs[mid].a <= pc) { at = mid; lo = mid + 1; } else hi = mid - 1; }
    if (at < 0 || pc - g_lxs[at].a >= 0x100000) return NULL;
    *off = pc - g_lxs[at].a;
    return g_lxs[at].name;
}
