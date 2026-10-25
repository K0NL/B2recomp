/* [Recomps#1] macOS only: the subset of <windows.h> the B2recomp runtime uses, on Darwin (notes/macos-port-plan.md step 1;
 * a copy of the Linux layer of the same name, tuned where Darwin differs — the measured differences are listed in the
 * plan's section 2).
 * The runtime keeps its Win32 calls; the build puts runtime/darwin/ first on the include path, so `#include
 * <windows.h>` lands here. Implementations that need state (handles, threads, events, timers, fibers, fiber-local
 * storage, virtual memory records) are in win32_compat.c. Only what the runtime calls is here, with the behaviour the
 * runtime relies on; window, GDI, dialogs, waveOut, Media Foundation and OpenGL are NOT here (shape-different: real
 * Darwin code behind #ifdef, plan steps 4-8). Never included on Windows.
 * _XOPEN_SOURCE (700) makes the deprecated-but-working ucontext routines visible (fibers); _DARWIN_C_SOURCE gives the
 * BSD names back that _XOPEN_SOURCE hides (pthread_threadid_np, arc4random_buf, libproc) — both measured needed. */
#ifndef RECOMPS_DARWIN_WINDOWS_H
#define RECOMPS_DARWIN_WINDOWS_H
#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE 700
#endif
#ifndef _DARWIN_C_SOURCE
#define _DARWIN_C_SOURCE 1
#endif
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <wchar.h>
#include <errno.h>
#include <time.h>
#include <sched.h>
#include <pthread.h>
#include <unistd.h>
#include <malloc/malloc.h>                              /* [Recomps#1] macOS: malloc_usable_size lives here, not in <malloc.h> */

#ifdef __cplusplus
extern "C" {
#endif

/* ---- calling conventions and annotations (no meaning on arm64 macOS) */
#define WINAPI
#define APIENTRY
#define CALLBACK
#define NTAPI
#define __stdcall
#define __cdecl
/* __declspec(dllexport): the macOS build compiles with -fdeclspec (clang then ignores dllexport on Mach-O; symbols are
 * visible by default) */
#define DECLSPEC_ALIGN(x) __attribute__((aligned(x)))

/* ---- basic types (LLP64 sizes kept: LONG / DWORD are 32-bit as on Windows) */
typedef int32_t BOOL;
typedef uint8_t BOOLEAN, BYTE, UCHAR;
typedef int16_t SHORT;
typedef uint16_t WORD, USHORT;
typedef int32_t INT, LONG;
typedef uint32_t UINT, DWORD, ULONG;
typedef int64_t LONG64, LONGLONG, INT64;
typedef uint64_t ULONG64, ULONGLONG, DWORD64, UINT64;
typedef intptr_t INT_PTR, LONG_PTR;
typedef uintptr_t UINT_PTR, ULONG_PTR, DWORD_PTR, SIZE_T;
typedef intptr_t SSIZE_T;
typedef char CHAR;
typedef wchar_t WCHAR;                                  /* 32-bit on macOS: only for text the runtime makes itself */
typedef void *HANDLE, *HMODULE, *HINSTANCE, *LPVOID, *PVOID;
typedef const void *LPCVOID;
typedef char *LPSTR;
typedef const char *LPCSTR;
typedef WCHAR *LPWSTR;
typedef const WCHAR *LPCWSTR;
typedef DWORD *LPDWORD;
typedef LONG HRESULT;
typedef union { struct { DWORD LowPart; LONG HighPart; } u; struct { DWORD LowPart; LONG HighPart; }; LONGLONG QuadPart; } LARGE_INTEGER;
typedef union { struct { DWORD LowPart; DWORD HighPart; }; ULONGLONG QuadPart; } ULARGE_INTEGER;
typedef struct { DWORD dwLowDateTime, dwHighDateTime; } FILETIME;
typedef struct { DWORD nLength; void *lpSecurityDescriptor; BOOL bInheritHandle; } SECURITY_ATTRIBUTES, *LPSECURITY_ATTRIBUTES;
typedef DWORD (*LPTHREAD_START_ROUTINE)(void *);
typedef void (*LPFIBER_START_ROUTINE)(void *);
typedef void (*PFLS_CALLBACK_FUNCTION)(void *);

#define TRUE 1
#define FALSE 0
#define MAX_PATH 260
#define INFINITE 0xFFFFFFFFu
#define WAIT_OBJECT_0 0u
#define WAIT_TIMEOUT 258u
#define WAIT_FAILED 0xFFFFFFFFu
#define INVALID_HANDLE_VALUE ((HANDLE)(intptr_t)-1)
#define S_OK 0
#define ERROR_SUCCESS 0
#define ERROR_ALREADY_EXISTS 183
#define ERROR_FILE_NOT_FOUND 2
#define ERROR_PATH_NOT_FOUND 3

/* ---- C runtime names */
#define _stricmp strcasecmp
#define _strnicmp strncasecmp
#define _strdup strdup
#define _snprintf snprintf
#define _vsnprintf vsnprintf
#define _access access
#define _unlink unlink
#define _getpid getpid
/* fopen with the path translation of CreateFileA ('\' -> '/', then the case of existing folders and files): the runtime
 * builds Windows paths such as "%s\%s.ttf" (nnpl.c) or "..\cache\vk" */
FILE *recomps_fopen(const char *path, const char *mode);
#define fopen recomps_fopen
static inline void *_aligned_malloc(size_t size, size_t align) {
    void *p = NULL;
    if (align < sizeof(void *)) align = sizeof(void *);
    return posix_memalign(&p, align, size ? size : 1) ? NULL : p;
}
static inline void _aligned_free(void *p) { free(p); }
static inline size_t _aligned_msize(void *p, size_t align, size_t off) { (void)align; (void)off; return p ? malloc_size(p) : 0; }
static inline void *_aligned_realloc(void *p, size_t size, size_t align) {
    if (!p) return _aligned_malloc(size, align);
    if (!size) { free(p); return NULL; }
    void *q = _aligned_malloc(size, align);
    if (!q) return NULL;
    size_t old = malloc_size(p);
    memcpy(q, p, old < size ? old : size);
    free(p);
    return q;
}
int _putenv(const char *kv);
static inline int _putenv_s(const char *k, const char *v) { return setenv(k, v, 1) ? EINVAL : 0; }
char *_fullpath(char *out, const char *path, size_t n);
extern int __argc;
extern char **__argv;

/* ---- atomics (full barriers, as the Interlocked functions) */
#define InterlockedIncrement(p) __atomic_add_fetch((p), 1, __ATOMIC_SEQ_CST)
#define InterlockedDecrement(p) __atomic_sub_fetch((p), 1, __ATOMIC_SEQ_CST)
#define InterlockedIncrement64 InterlockedIncrement
#define InterlockedDecrement64 InterlockedDecrement
#define InterlockedExchange(p, v) __atomic_exchange_n((p), (v), __ATOMIC_SEQ_CST)
#define InterlockedExchange64 InterlockedExchange
#define InterlockedExchangePointer InterlockedExchange
#define InterlockedExchangeAdd(p, v) __atomic_fetch_add((p), (v), __ATOMIC_SEQ_CST)
#define InterlockedExchangeAdd64 InterlockedExchangeAdd
#define InterlockedAdd(p, v) __atomic_add_fetch((p), (v), __ATOMIC_SEQ_CST)
#define InterlockedAdd64 InterlockedAdd
#define InterlockedOr(p, v) __atomic_fetch_or((p), (v), __ATOMIC_SEQ_CST)
#define InterlockedAnd(p, v) __atomic_fetch_and((p), (v), __ATOMIC_SEQ_CST)
#define InterlockedOr64 InterlockedOr
#define InterlockedAnd64 InterlockedAnd
#define InterlockedCompareExchange(p, xchg, cmp) __extension__({ __typeof__(*(p)) c_ = (cmp); \
    __atomic_compare_exchange_n((p), &c_, (xchg), 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); c_; })
#define InterlockedCompareExchange64 InterlockedCompareExchange
#define InterlockedCompareExchangePointer InterlockedCompareExchange
#define MemoryBarrier() __atomic_thread_fence(__ATOMIC_SEQ_CST)
#if defined(__x86_64__)
#define YieldProcessor() __builtin_ia32_pause()          /* spin-wait hint; the pause instruction is x86-only */
#elif defined(__aarch64__) || defined(__arm64__)
#define YieldProcessor() __asm__ __volatile__("yield")  /* [Recomps#1] the arm64 equivalent (YIELD), for Apple Silicon */
#else
#define YieldProcessor() ((void)0)
#endif
#define _ReadWriteBarrier() __atomic_signal_fence(__ATOMIC_SEQ_CST)

/* ---- time */
BOOL QueryPerformanceCounter(LARGE_INTEGER *c);         /* CLOCK_MONOTONIC in nanoseconds */
BOOL QueryPerformanceFrequency(LARGE_INTEGER *f);       /* 1,000,000,000 */
ULONGLONG GetTickCount64(void);
#define _time64(p) time(p)                              /* time_t is 64-bit on macOS (LP64) */
typedef int64_t __time64_t;
static inline int _gmtime64_s(struct tm *out, const __time64_t *t) { time_t x = (time_t)*t; return gmtime_r(&x, out) ? 0 : EINVAL; }
static inline int _localtime64_s(struct tm *out, const __time64_t *t) { time_t x = (time_t)*t; return localtime_r(&x, out) ? 0 : EINVAL; }
#define _mkgmtime64(tm) ((__time64_t)timegm(tm))
#define _mkgmtime(tm) timegm(tm)
static inline DWORD GetTickCount(void) { return (DWORD)GetTickCount64(); }
void Sleep(DWORD ms);
static inline BOOL SwitchToThread(void) { return sched_yield() == 0; }
void GetSystemTimeAsFileTime(FILETIME *ft);
typedef struct { WORD wYear, wMonth, wDayOfWeek, wDay, wHour, wMinute, wSecond, wMilliseconds; } SYSTEMTIME, *LPSYSTEMTIME;
void GetLocalTime(SYSTEMTIME *st);
void GetSystemTime(SYSTEMTIME *st);

/* ---- locks and condition variables (zero-initialised = ready, as on Windows) */
/* [Recomps#1] SRWLOCK on Darwin is this port's own read-write lock, NOT Apple's pthread_rwlock: in frozen runs of
 * the game every thread ended up queued in __psynch_rw_wrlock on locks that no thread held (os_threads.c's
 * g_table_lock, nvn_stub.c's g_objs_lock; a stopped-process dump showed no thread inside any locked region, and it
 * hit at random times - frame 60, 392, 1346 of different runs). Apple's rwlock hands its write ownership over in
 * the kernel; whatever strand it left, a mutex with two condition variables has no such state machine. Shared
 * acquires are recursive, as Windows' SRWLock's are. */
typedef struct {
    pthread_mutex_t m;
    pthread_cond_t cv_r, cv_w;          /* cv_r: readers waiting for a writer / write-waiters to clear; cv_w: the next writer */
    int readers, writer, wwait;
} SRWLOCK, *PSRWLOCK;
#define SRWLOCK_INIT { PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, PTHREAD_COND_INITIALIZER, 0, 0, 0 }
/* the shared holds of this thread (a shared acquire may nest, as on Windows) */
typedef struct { const SRWLOCK *l; int depth; } SrwHold;
static _Thread_local SrwHold t_srw_holds[16];
static inline void srw_hold_add(const SRWLOCK *s) {
    for (int i = 0; i < 16; i++) if (t_srw_holds[i].l == s) { t_srw_holds[i].depth++; return; }
    for (int i = 0; i < 16; i++) if (!t_srw_holds[i].l) { t_srw_holds[i].l = s; t_srw_holds[i].depth = 1; return; }
    /* more than 16 distinct shared holds at once: not expected; the counting below stays balanced without it */
}
static inline int srw_hold_sub(const SRWLOCK *s) {      /* 1 when this shared hold ends (or was never registered) */
    for (int i = 0; i < 16; i++) if (t_srw_holds[i].l == s) {
        int d = --t_srw_holds[i].depth;
        if (d <= 0) t_srw_holds[i].l = NULL;
        return d <= 0;
    }
    return 1;
}
static inline void InitializeSRWLock(SRWLOCK *s) {
    pthread_mutex_init(&s->m, NULL);
    pthread_cond_init(&s->cv_r, NULL);
    pthread_cond_init(&s->cv_w, NULL);
    s->readers = s->writer = s->wwait = 0;
}
static inline void AcquireSRWLockExclusive(SRWLOCK *s) {
    pthread_mutex_lock(&s->m);
    s->wwait++;                                          /* writers first: a reader stream must not starve them */
    while (s->writer || s->readers) pthread_cond_wait(&s->cv_w, &s->m);
    s->wwait--;
    s->writer = 1;
    pthread_mutex_unlock(&s->m);
}
static inline void ReleaseSRWLockExclusive(SRWLOCK *s) {
    pthread_mutex_lock(&s->m);
    s->writer = 0;
    if (s->wwait) pthread_cond_signal(&s->cv_w);         /* the next writer; it lets the readers in when done */
    else pthread_cond_broadcast(&s->cv_r);
    pthread_mutex_unlock(&s->m);
}
static inline void AcquireSRWLockShared(SRWLOCK *s) {
    pthread_mutex_lock(&s->m);
    while (s->writer || s->wwait) pthread_cond_wait(&s->cv_r, &s->m);
    s->readers++;
    pthread_mutex_unlock(&s->m);
    srw_hold_add(s);
}
static inline void ReleaseSRWLockShared(SRWLOCK *s) {
    if (srw_hold_sub(s)) {
        pthread_mutex_lock(&s->m);
        if (--s->readers == 0 && s->wwait) pthread_cond_signal(&s->cv_w);
        pthread_mutex_unlock(&s->m);
    }
}
static inline BOOLEAN TryAcquireSRWLockExclusive(SRWLOCK *s) {
    pthread_mutex_lock(&s->m);
    BOOLEAN ok = !s->writer && !s->readers;
    if (ok) s->writer = 1;
    pthread_mutex_unlock(&s->m);
    return ok;
}
DWORD GetCurrentThreadId(void);                         /* gettid() */
/* recursive, as on Windows; OwningThread / RecursionCount kept as Windows does (os_threads.c reads the owner for hitch.c) */
typedef struct { pthread_mutex_t m; int ready; LONG RecursionCount; HANDLE OwningThread; LONG LockCount; } CRITICAL_SECTION, *LPCRITICAL_SECTION;   /* LockCount: always -1 ("free"), read by diagnostics only */
void InitializeCriticalSection(CRITICAL_SECTION *c);
static inline BOOL InitializeCriticalSectionAndSpinCount(CRITICAL_SECTION *c, DWORD n) { (void)n; InitializeCriticalSection(c); return TRUE; }
static inline void EnterCriticalSection(CRITICAL_SECTION *c) {
    pthread_mutex_lock(&c->m);
    if (c->RecursionCount++ == 0) c->OwningThread = (HANDLE)(uintptr_t)GetCurrentThreadId();
}
static inline void LeaveCriticalSection(CRITICAL_SECTION *c) {
    if (--c->RecursionCount == 0) c->OwningThread = NULL;
    pthread_mutex_unlock(&c->m);
}
static inline BOOL TryEnterCriticalSection(CRITICAL_SECTION *c) {
    if (pthread_mutex_trylock(&c->m)) return FALSE;
    if (c->RecursionCount++ == 0) c->OwningThread = (HANDLE)(uintptr_t)GetCurrentThreadId();
    return TRUE;
}
static inline void DeleteCriticalSection(CRITICAL_SECTION *c) { pthread_mutex_destroy(&c->m); c->ready = 0; }
/* A Windows condition variable waits with either lock kind; a pthread one only with a mutex. So it is an internal mutex,
 * a pthread condition and a wake counter: the sleeper takes the internal mutex, reads the counter, releases the caller's
 * lock and waits; a waker takes the internal mutex to bump the counter, so no wake-up between "release" and "wait" is
 * lost. All-zero = ready (CONDITION_VARIABLE_INIT, static storage), as on Windows. */
typedef struct { pthread_mutex_t m; pthread_cond_t c; uint64_t seq; } CONDITION_VARIABLE, *PCONDITION_VARIABLE;
#define CONDITION_VARIABLE_INIT { PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, 0 }
/* [Recomps#1] Darwin: pthread_mutex_init/pthread_cond_init, NOT memset(0): an all-zero pthread_cond_t is not a
 * condition variable on Darwin (pthread_cond_wait rejects it with EINVAL and returns at once), and every wait
 * became a spin loop - measured at 130,000,000 loop iterations a second, ~17 cores of pure spin, the whole
 * slowness of the port (the game ran at 4-15 fps). */
static inline void InitializeConditionVariable(CONDITION_VARIABLE *v) { pthread_mutex_init(&v->m, NULL); pthread_cond_init(&v->c, NULL); v->seq = 0; }
static inline void WakeConditionVariable(CONDITION_VARIABLE *v) { pthread_mutex_lock(&v->m); v->seq++; pthread_cond_signal(&v->c); pthread_mutex_unlock(&v->m); }
static inline void WakeAllConditionVariable(CONDITION_VARIABLE *v) { pthread_mutex_lock(&v->m); v->seq++; pthread_cond_broadcast(&v->c); pthread_mutex_unlock(&v->m); }BOOL SleepConditionVariableCS(CONDITION_VARIABLE *v, CRITICAL_SECTION *c, DWORD ms);   /* FALSE + ERROR_TIMEOUT on time-out */
#define CONDITION_VARIABLE_LOCKMODE_SHARED 1
BOOL SleepConditionVariableSRW(CONDITION_VARIABLE *v, SRWLOCK *s, DWORD ms, ULONG flags);
#define ERROR_TIMEOUT 1460
typedef struct { volatile int state; } INIT_ONCE, *PINIT_ONCE;
#define INIT_ONCE_STATIC_INIT { 0 }
typedef BOOL (*PINIT_ONCE_FN)(INIT_ONCE *, void *, void **);
BOOL InitOnceExecuteOnce(INIT_ONCE *o, PINIT_ONCE_FN fn, void *param, void **ctx);

/* ---- handles: threads, events, waitable timers */
#define CREATE_SUSPENDED 0x4u
#define STACK_SIZE_PARAM_IS_A_RESERVATION 0x10000u
HANDLE CreateThread(SECURITY_ATTRIBUTES *sa, SIZE_T stack, LPTHREAD_START_ROUTINE fn, void *arg, DWORD flags, DWORD *tid);
DWORD ResumeThread(HANDLE h);
HANDLE GetCurrentThread(void);                          /* a pseudo handle, as on Windows */
static inline HANDLE GetCurrentProcess(void) { return (HANDLE)(intptr_t)-1; }
DWORD GetThreadId(HANDLE h);
static inline DWORD GetCurrentProcessId(void) { return (DWORD)getpid(); }
DWORD GetCurrentProcessorNumber(void);
#define THREAD_PRIORITY_LOWEST -2
#define THREAD_PRIORITY_BELOW_NORMAL -1
#define THREAD_PRIORITY_NORMAL 0
#define THREAD_PRIORITY_ABOVE_NORMAL 1
#define THREAD_PRIORITY_HIGHEST 2
#define THREAD_PRIORITY_TIME_CRITICAL 15
BOOL SetThreadPriority(HANDLE h, int prio);             /* best effort: an unprivileged process cannot raise it */
static inline BOOL SetThreadPriorityBoost(HANDLE h, BOOL off) { (void)h; (void)off; return TRUE; }
DWORD_PTR SetThreadAffinityMask(HANDLE h, DWORD_PTR mask);
BOOL GetProcessAffinityMask(HANDLE p, DWORD_PTR *proc, DWORD_PTR *sys);
HANDLE CreateEventA(SECURITY_ATTRIBUTES *sa, BOOL manual, BOOL initial, LPCSTR name);
#define CreateEvent CreateEventA
BOOL SetEvent(HANDLE h);
HANDLE CreateSemaphoreA(SECURITY_ATTRIBUTES *sa, LONG initial, LONG max, LPCSTR name);
#define CreateSemaphore CreateSemaphoreA
BOOL ReleaseSemaphore(HANDLE h, LONG n, LONG *previous);
int GetThreadPriority(HANDLE h);                        /* THREAD_PRIORITY_NORMAL (priorities are hints here) */
BOOL ResetEvent(HANDLE h);
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x2u
#define TIMER_ALL_ACCESS 0x1F0003u
HANDLE CreateWaitableTimerExW(SECURITY_ATTRIBUTES *sa, LPCWSTR name, DWORD flags, DWORD access);
BOOL SetWaitableTimer(HANDLE h, const LARGE_INTEGER *due, LONG period, void *fn, void *arg, BOOL resume);
DWORD WaitForSingleObject(HANDLE h, DWORD ms);
DWORD WaitForMultipleObjects(DWORD n, const HANDLE *hs, BOOL all, DWORD ms);
BOOL CloseHandle(HANDLE h);
DWORD GetLastError(void);
void SetLastError(DWORD e);
void ExitProcess(UINT code) __attribute__((noreturn));

/* ---- fibers and fiber-local storage (ucontext) */
void *ConvertThreadToFiber(void *param);
void *CreateFiberEx(SIZE_T commit, SIZE_T reserve, DWORD flags, LPFIBER_START_ROUTINE fn, void *param);
static inline void *CreateFiber(SIZE_T stack, LPFIBER_START_ROUTINE fn, void *param) { return CreateFiberEx(stack, stack, 0, fn, param); }
void SwitchToFiber(void *fiber);
void DeleteFiber(void *fiber);
void *GetCurrentFiber(void);
void *GetFiberData(void);
#define FLS_OUT_OF_INDEXES 0xFFFFFFFFu
DWORD FlsAlloc(PFLS_CALLBACK_FUNCTION cb);
void *FlsGetValue(DWORD i);
BOOL FlsSetValue(DWORD i, void *v);
BOOL FlsFree(DWORD i);

/* ---- virtual memory (reservations recorded, so release / query behave as on Windows; 64 KiB granularity) */
#define MEM_COMMIT 0x1000u
#define MEM_RESERVE 0x2000u
#define MEM_DECOMMIT 0x4000u
#define MEM_RELEASE 0x8000u
#define MEM_FREE 0x10000u
#define MEM_PRIVATE 0x20000u
#define MEM_IMAGE 0x1000000u
#define MEM_MAPPED 0x40000u
#define MEM_WRITE_WATCH 0x200000u                       /* tracked by the kernel where it can (win32_compat.c, GetWriteWatch) */
#define PAGE_NOACCESS 0x01u
#define PAGE_READONLY 0x02u
#define PAGE_READWRITE 0x04u
#define PAGE_EXECUTE 0x10u
#define PAGE_EXECUTE_READ 0x20u
#define PAGE_EXECUTE_READWRITE 0x40u
#define PAGE_GUARD 0x100u
typedef struct { void *BaseAddress, *AllocationBase; DWORD AllocationProtect; SIZE_T RegionSize; DWORD State, Protect, Type; } MEMORY_BASIC_INFORMATION;
void *VirtualAlloc(void *addr, SIZE_T size, DWORD type, DWORD prot);
BOOL VirtualFree(void *addr, SIZE_T size, DWORD type);
BOOL VirtualProtect(void *addr, SIZE_T size, DWORD prot, DWORD *old);
SIZE_T VirtualQuery(const void *addr, MEMORY_BASIC_INFORMATION *mbi, SIZE_T len);
BOOL IsBadReadPtr(const void *p, UINT_PTR n);
BOOL IsBadWritePtr(void *p, UINT_PTR n);
#define WRITE_WATCH_FLAG_RESET 1u
UINT GetWriteWatch(DWORD flags, void *base, SIZE_T n, void **addrs, ULONG_PTR *count, ULONG *gran);   /* 1 = not watched: callers hash */
UINT ResetWriteWatch(void *base, SIZE_T n);

/* ---- modules */
HMODULE LoadLibraryA(LPCSTR name);                      /* "x.dll" names of the runtime map to macOS dylibs */
#define LOAD_WITH_ALTERED_SEARCH_PATH 0x8u
static inline HMODULE LoadLibraryExA(LPCSTR name, HANDLE file, DWORD flags) { (void)file; (void)flags; return LoadLibraryA(name); }
#define LoadLibrary LoadLibraryA
HMODULE GetModuleHandleA(LPCSTR name);
#define GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS 0x4u
#define GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT 0x2u
BOOL GetModuleHandleExA(DWORD flags, LPCSTR name, HMODULE *out);
void *GetProcAddress(HMODULE m, LPCSTR name);
BOOL FreeLibrary(HMODULE m);
DWORD GetModuleFileNameA(HMODULE m, char *out, DWORD n);

/* ---- files and folders (A functions: UTF-8 paths; '\\' is accepted as a separator) */
#define FILE_ATTRIBUTE_READONLY 0x1u
#define FILE_ATTRIBUTE_DIRECTORY 0x10u
#define FILE_ATTRIBUTE_NORMAL 0x80u
#define INVALID_FILE_ATTRIBUTES 0xFFFFFFFFu
typedef enum { GetFileExInfoStandard } GET_FILEEX_INFO_LEVELS;
typedef struct { DWORD dwFileAttributes; FILETIME ftCreationTime, ftLastAccessTime, ftLastWriteTime; DWORD nFileSizeHigh, nFileSizeLow; } WIN32_FILE_ATTRIBUTE_DATA;
BOOL CreateDirectoryA(LPCSTR path, SECURITY_ATTRIBUTES *sa);
BOOL RemoveDirectoryA(LPCSTR path);
BOOL DeleteFileA(LPCSTR path);
BOOL MoveFileExA(LPCSTR from, LPCSTR to, DWORD flags);
#define MOVEFILE_REPLACE_EXISTING 0x1u
#define MOVEFILE_WRITE_THROUGH 0x8u
static inline BOOL MoveFileA(LPCSTR from, LPCSTR to) { return MoveFileExA(from, to, 0); }
DWORD GetFileAttributesA(LPCSTR path);
BOOL GetFileAttributesExA(LPCSTR path, GET_FILEEX_INFO_LEVELS lvl, void *out);
DWORD GetTempPathA(DWORD n, char *out);
static inline UINT GetWindowsDirectoryA(char *out, UINT n) { (void)out; (void)n; return 0; }   /* none: fonts stand-in, plan step 8 */
#define FILE_BEGIN 0u
#define FILE_CURRENT 1u
#define FILE_END 2u
BOOL SetFilePointerEx(HANDLE h, LARGE_INTEGER to, LARGE_INTEGER *now, DWORD how);   /* h from _get_osfhandle (io.h) */
BOOL SetEndOfFile(HANDLE h);
/* File handles: the descriptor tagged into the handle value (fd << 2 | 3; INVALID_HANDLE_VALUE excluded), so
 * _get_osfhandle (io.h) and CreateFileA give the same kind and CloseHandle closes the descriptor. Paths: '\\' accepted;
 * a name that does not exist is looked up case-insensitively, folder by folder (macOS volumes are usually
 * case-insensitive but the case-sensitive option exists, and the lookup costs nothing when the name is right). */
#define GENERIC_READ 0x80000000u
#define GENERIC_WRITE 0x40000000u
#define FILE_SHARE_READ 0x1u
#define FILE_SHARE_WRITE 0x2u
#define FILE_SHARE_DELETE 0x4u
#define CREATE_NEW 1u
#define CREATE_ALWAYS 2u
#define OPEN_EXISTING 3u
#define OPEN_ALWAYS 4u
#define TRUNCATE_EXISTING 5u
#define FILE_FLAG_SEQUENTIAL_SCAN 0x08000000u
#define FILE_FLAG_BACKUP_SEMANTICS 0x02000000u
#define ERROR_ACCESS_DENIED 5
#define ERROR_FILE_EXISTS 80
#define ERROR_HANDLE_EOF 38
typedef struct { ULONG_PTR Internal, InternalHigh; union { struct { DWORD Offset, OffsetHigh; }; void *Pointer; }; HANDLE hEvent; } OVERLAPPED, *LPOVERLAPPED;
/* ReadFile / WriteFile with an OVERLAPPED on a synchronous handle: at Offset, OffsetHigh (pread / pwrite) */
BOOL GetUserNameW(WCHAR *out, DWORD *n);              /* the login name ($USER or the password entry) */
HANDLE CreateFileA(LPCSTR path, DWORD access, DWORD share, SECURITY_ATTRIBUTES *sa, DWORD disposition, DWORD flags, HANDLE tmpl);
#define CreateFile CreateFileA
BOOL ReadFile(HANDLE h, void *buf, DWORD n, DWORD *got, void *overlapped);
BOOL WriteFile(HANDLE h, const void *buf, DWORD n, DWORD *put, void *overlapped);
BOOL GetFileSizeEx(HANDLE h, LARGE_INTEGER *size);
DWORD GetFileSize(HANDLE h, DWORD *high);
BOOL FlushFileBuffers(HANDLE h);
BOOL CopyFileA(LPCSTR from, LPCSTR to, BOOL fail_if_exists);
BOOL SetFileAttributesA(LPCSTR path, DWORD attrs);
BOOL GetDiskFreeSpaceExA(LPCSTR path, ULARGE_INTEGER *avail, ULARGE_INTEGER *total, ULARGE_INTEGER *free_);
/* file mappings (read views of files: nvn_vk_store.c) */
#define FILE_MAP_WRITE 0x2u
#define FILE_MAP_READ 0x4u
HANDLE CreateFileMappingA(HANDLE file, SECURITY_ATTRIBUTES *sa, DWORD prot, DWORD hi, DWORD lo, LPCSTR name);
void *MapViewOfFile(HANDLE map, DWORD access, DWORD off_hi, DWORD off_lo, SIZE_T n);
BOOL UnmapViewOfFile(const void *p);
/* mutexes: a named one is a lock file (flock) under $XDG_RUNTIME_DIR or /tmp, so it is shared between processes as on
 * Windows (nnfs.c: one save commit at a time); processes: is one still running (nnfs.c journal recovery) */
HANDLE CreateMutexA(SECURITY_ATTRIBUTES *sa, BOOL owned, LPCSTR name);
BOOL ReleaseMutex(HANDLE h);
#define SYNCHRONIZE 0x100000u
#define PROCESS_QUERY_LIMITED_INFORMATION 0x1000u
HANDLE OpenProcess(DWORD access, BOOL inherit, DWORD pid);
typedef struct {
    DWORD dwFileAttributes; FILETIME ftCreationTime, ftLastAccessTime, ftLastWriteTime;
    DWORD nFileSizeHigh, nFileSizeLow, dwReserved0, dwReserved1; CHAR cFileName[MAX_PATH]; CHAR cAlternateFileName[14];
} WIN32_FIND_DATAA, *LPWIN32_FIND_DATAA;
typedef struct {
    DWORD dwFileAttributes; FILETIME ftCreationTime, ftLastAccessTime, ftLastWriteTime;
    DWORD nFileSizeHigh, nFileSizeLow, dwReserved0, dwReserved1; WCHAR cFileName[MAX_PATH]; WCHAR cAlternateFileName[14];
} WIN32_FIND_DATAW, *LPWIN32_FIND_DATAW;
HANDLE FindFirstFileA(LPCSTR pattern, WIN32_FIND_DATAA *fd);  /* "dir\\*.ext": '*' and '?' in the last part, case-insensitive */
BOOL FindNextFileA(HANDLE h, WIN32_FIND_DATAA *fd);
HANDLE FindFirstFileW(LPCWSTR pattern, WIN32_FIND_DATAW *fd);
BOOL FindNextFileW(HANDLE h, WIN32_FIND_DATAW *fd);
BOOL FindClose(HANDLE h);

/* ---- text */
#define CP_ACP 0u
#define CP_UTF8 65001u
int MultiByteToWideChar(UINT cp, DWORD flags, LPCSTR s, int n, WCHAR *out, int on);
int WideCharToMultiByte(UINT cp, DWORD flags, LPCWSTR s, int n, char *out, int on, LPCSTR def, BOOL *used);
static inline int lstrcmpiW(LPCWSTR a, LPCWSTR b) { return wcscasecmp(a, b); }
static inline char *_strlwr(char *s) { for (char *c = s; *c; c++) if (*c >= 'A' && *c <= 'Z') *c = (char)(*c + 32); return s; }
static inline char *_strupr(char *s) { for (char *c = s; *c; c++) if (*c >= 'a' && *c <= 'z') *c = (char)(*c - 32); return s; }
#define _fseeki64(f, o, w) fseeko((f), (off_t)(o), (w))
#define _ftelli64(f) ((int64_t)ftello(f))
typedef struct { WORD wProcessorArchitecture, wReserved; DWORD dwPageSize; void *lpMinimumApplicationAddress, *lpMaximumApplicationAddress;
                 DWORD_PTR dwActiveProcessorMask; DWORD dwNumberOfProcessors, dwProcessorType, dwAllocationGranularity; WORD wProcessorLevel, wProcessorRevision; } SYSTEM_INFO;
void GetSystemInfo(SYSTEM_INFO *si);
/* the user's language, from $LANGUAGE / $LC_ALL / $LC_MESSAGES / $LANG ("de_DE.UTF-8" -> "de-DE"); "C" / "POSIX": none */
#define LOCALE_NAME_MAX_LENGTH 85
#define MUI_LANGUAGE_NAME 0x8u
BOOL GetUserPreferredUILanguages(DWORD flags, ULONG *count, WCHAR *list, ULONG *size);
int GetUserDefaultLocaleName(WCHAR *out, int n);
#define ES_SYSTEM_REQUIRED 0x1u
#define ES_DISPLAY_REQUIRED 0x2u
#define ES_CONTINUOUS 0x80000000u
static inline DWORD SetThreadExecutionState(DWORD flags) { return flags | ES_CONTINUOUS; }   /* SDL keeps the screen on while its window is open */

/* ---- misc */
BOOLEAN SystemFunction036(void *buf, ULONG len);        /* RtlGenRandom: getrandom() */
#define RtlGenRandom SystemFunction036
void RaiseException(DWORD code, DWORD flags, DWORD n, const ULONG_PTR *args) __attribute__((noreturn));
typedef enum { RelationProcessorCore = 0 } LOGICAL_PROCESSOR_RELATIONSHIP;
typedef struct SYSTEM_CPU_SET_INFORMATION SYSTEM_CPU_SET_INFORMATION, *PSYSTEM_CPU_SET_INFORMATION;
static inline BOOL GetSystemCpuSetInformation(void *info, ULONG len, ULONG *ret, HANDLE p, ULONG f) {
    (void)info; (void)len; (void)p; (void)f; if (ret) *ret = 0; return FALSE;   /* callers fall back to plain affinity */
}
static inline BOOL SetThreadSelectedCpuSets(HANDLE h, const ULONG *ids, ULONG n) { (void)h; (void)ids; (void)n; return FALSE; }

/* ---- CPU context, exceptions, thread snapshots: the runtime's crash reports, hang sampler and write-watch test tools.
 * On macOS they compile and report "not available" (OpenThread NULL, no unwind data, handlers never called) until
 * plan step 3 gives them a signal-based version. */
#define CONTEXT_AMD64 0x100000u
#define CONTEXT_CONTROL (CONTEXT_AMD64 | 0x1u)
#define CONTEXT_INTEGER (CONTEXT_AMD64 | 0x2u)
#define CONTEXT_DEBUG_REGISTERS (CONTEXT_AMD64 | 0x10u)
#define CONTEXT_FULL (CONTEXT_AMD64 | 0xBu)
typedef struct {
    DWORD ContextFlags, MxCsr, EFlags;
    DWORD64 Dr0, Dr1, Dr2, Dr3, Dr6, Dr7;
    DWORD64 Rax, Rcx, Rdx, Rbx, Rsp, Rbp, Rsi, Rdi, R8, R9, R10, R11, R12, R13, R14, R15, Rip;
} CONTEXT, *PCONTEXT;
#define EXCEPTION_MAXIMUM_PARAMETERS 15
typedef struct EXCEPTION_RECORD {
    DWORD ExceptionCode, ExceptionFlags; struct EXCEPTION_RECORD *ExceptionRecord; void *ExceptionAddress;
    DWORD NumberParameters; ULONG_PTR ExceptionInformation[EXCEPTION_MAXIMUM_PARAMETERS];
} EXCEPTION_RECORD, *PEXCEPTION_RECORD;
typedef struct { EXCEPTION_RECORD *ExceptionRecord; CONTEXT *ContextRecord; } EXCEPTION_POINTERS, *PEXCEPTION_POINTERS;
#define EXCEPTION_ACCESS_VIOLATION 0xC0000005u
#define EXCEPTION_SINGLE_STEP 0x80000004u
#define EXCEPTION_EXECUTE_HANDLER 1
#define EXCEPTION_CONTINUE_SEARCH 0
#define EXCEPTION_CONTINUE_EXECUTION (-1)
typedef LONG (*PVECTORED_EXCEPTION_HANDLER)(EXCEPTION_POINTERS *);
typedef LONG (*LPTOP_LEVEL_EXCEPTION_FILTER)(EXCEPTION_POINTERS *);
void *AddVectoredExceptionHandler(ULONG first, PVECTORED_EXCEPTION_HANDLER h);
ULONG RemoveVectoredExceptionHandler(void *h);
LPTOP_LEVEL_EXCEPTION_FILTER SetUnhandledExceptionFilter(LPTOP_LEVEL_EXCEPTION_FILTER f);
typedef struct { DWORD BeginAddress, EndAddress, UnwindData; } RUNTIME_FUNCTION, *PRUNTIME_FUNCTION;
#define UNW_FLAG_NHANDLER 0u
void RtlCaptureContext(CONTEXT *c);
static inline RUNTIME_FUNCTION *RtlLookupFunctionEntry(DWORD64 pc, DWORD64 *base, void *h) { (void)pc; (void)h; if (base) *base = 0; return NULL; }
static inline void *RtlVirtualUnwind(DWORD t, DWORD64 b, DWORD64 pc, RUNTIME_FUNCTION *f, CONTEXT *c, void **hd, DWORD64 *est, void *p) {
    (void)t; (void)b; (void)pc; (void)f; (void)hd; (void)est; (void)p; c->Rip = 0; return NULL;
}
#define THREAD_SUSPEND_RESUME 0x2u
#define THREAD_GET_CONTEXT 0x8u
#define THREAD_SET_CONTEXT 0x10u
#define THREAD_QUERY_INFORMATION 0x40u
#define THREAD_ALL_ACCESS 0x1FFFFFu
static inline HANDLE OpenThread(DWORD access, BOOL inherit, DWORD tid) { (void)access; (void)inherit; (void)tid; return NULL; }
static inline DWORD SuspendThread(HANDLE h) { (void)h; return (DWORD)-1; }
static inline BOOL GetThreadContext(HANDLE h, CONTEXT *c) { (void)h; (void)c; return FALSE; }
static inline BOOL SetThreadContext(HANDLE h, const CONTEXT *c) { (void)h; (void)c; return FALSE; }
#define DUPLICATE_SAME_ACCESS 0x2u
static inline BOOL DuplicateHandle(HANDLE sp, HANDLE s, HANDLE tp, HANDLE *t, DWORD a, BOOL i, DWORD o) {
    (void)sp; (void)s; (void)tp; (void)a; (void)i; (void)o; if (t) *t = NULL; return FALSE;
}
static inline BOOL QueryThreadCycleTime(HANDLE h, ULONG64 *c) { (void)h; if (c) *c = 0; return FALSE; }
static inline BOOL GetThreadTimes(HANDLE h, FILETIME *a, FILETIME *b, FILETIME *k, FILETIME *u) { (void)h; (void)a; (void)b; (void)k; (void)u; return FALSE; }
static inline BOOL GetProcessTimes(HANDLE h, FILETIME *a, FILETIME *b, FILETIME *k, FILETIME *u) { (void)h; (void)a; (void)b; (void)k; (void)u; return FALSE; }
static inline BOOL GetSystemTimes(FILETIME *i, FILETIME *k, FILETIME *u) { (void)i; (void)k; (void)u; return FALSE; }
BOOL ReadProcessMemory(HANDLE p, LPCVOID from, void *to, SIZE_T n, SIZE_T *got);     /* this process only */
/* [Recomps#1] the freeze watchdog's thread report (nvn_vk_wd.c) has a Darwin form of its own instead of the stubs above
 * (win32_compat.c): the threads from libproc/Mach, a call stack recorded by the thread itself in a signal handler */
typedef struct { DWORD tid; char comm[20]; char state; int nice; uint64_t cpu_ns; char wchan[48]; long sysno; uint64_t sysarg[2]; } LxThread;
int lx_threads(LxThread *t, int max);                 /* every thread of this process; returns how many */
int lx_thread_read(LxThread *t);                      /* again for t->tid: state, CPU time (ns), kernel wait, system call */
int lx_thread_stack(DWORD tid, uint64_t *pcs, int max);   /* return addresses, innermost first; -1 thread gone, -2 no answer in 200 ms, -4 it blocks the signal */
const char *lx_exe_symbol(uint64_t pc, uint64_t *off);    /* the nearest function of the program's own symbol table, or NULL */

/* ---- the window: on macOS an SDL3 window (display.c), whose thread answers these (display.c, non-Windows part) */
typedef void *HWND;
typedef void *HDC, *HFONT, *HGLRC, *HMONITOR, *HMENU;
typedef struct { LONG left, top, right, bottom; } RECT, *LPRECT;
typedef struct { LONG x, y; } POINT;
BOOL GetClientRect(HWND h, RECT *r);                    /* the game window's drawable size in pixels (SDL) */
int GetWindowTextA(HWND h, char *out, int n);
/* WGL / GDI for the OpenGL renderer, which is not available on macOS: every call says "no" (the Vulkan renderer is used) */
typedef struct { WORD nSize, nVersion; DWORD dwFlags; BYTE iPixelType, cColorBits, cRedBits, cRedShift, cGreenBits, cGreenShift, cBlueBits, cBlueShift,
                 cAlphaBits, cAlphaShift, cAccumBits, cAccumRedBits, cAccumGreenBits, cAccumBlueBits, cAccumAlphaBits, cDepthBits, cStencilBits,
                 cAuxBuffers, iLayerType, bReserved; DWORD dwLayerMask, dwVisibleMask, dwDamageMask; } PIXELFORMATDESCRIPTOR;
#define PFD_DRAW_TO_WINDOW 0x4u
#define PFD_SUPPORT_OPENGL 0x20u
#define PFD_DOUBLEBUFFER 0x1u
#define PFD_TYPE_RGBA 0
#define PFD_MAIN_PLANE 0
#define WS_POPUP 0x80000000u
#define WS_VISIBLE 0x10000000u
#define WS_OVERLAPPEDWINDOW 0xCF0000u
static inline HWND CreateWindowExA(DWORD ex, LPCSTR cls, LPCSTR name, DWORD style, int x, int y, int w, int h, HWND parent, HMENU menu, HINSTANCE inst, void *param) {
    (void)ex; (void)cls; (void)name; (void)style; (void)x; (void)y; (void)w; (void)h; (void)parent; (void)menu; (void)inst; (void)param; return NULL;
}
static inline HDC GetDC(HWND h) { (void)h; return NULL; }
static inline int ReleaseDC(HWND h, HDC dc) { (void)h; (void)dc; return 0; }
static inline int ChoosePixelFormat(HDC dc, const PIXELFORMATDESCRIPTOR *p) { (void)dc; (void)p; return 0; }
static inline BOOL SetPixelFormat(HDC dc, int f, const PIXELFORMATDESCRIPTOR *p) { (void)dc; (void)f; (void)p; return FALSE; }
static inline HGLRC wglCreateContext(HDC dc) { (void)dc; return NULL; }
static inline BOOL wglMakeCurrent(HDC dc, HGLRC rc) { (void)dc; (void)rc; return FALSE; }
static inline BOOL wglDeleteContext(HGLRC rc) { (void)rc; return FALSE; }
static inline void *wglGetProcAddress(LPCSTR name) { (void)name; return NULL; }
static inline BOOL SwapBuffers(HDC dc) { (void)dc; return FALSE; }
static inline BOOL DestroyWindow(HWND h) { (void)h; return FALSE; }
#define LF_FACESIZE 32
static inline HDC CreateCompatibleDC(HDC dc) { (void)dc; return NULL; }   /* no GDI on macOS (nnfontll.c reads outlines itself) */
typedef intptr_t LPARAM, LRESULT;
typedef uintptr_t WPARAM;
typedef BOOL (*WNDENUMPROC)(HWND, LPARAM);
BOOL EnumWindows(WNDENUMPROC cb, LPARAM l);             /* the game window, if any */
BOOL IsWindowVisible(HWND h);
#define GW_OWNER 4
HWND GetWindow(HWND h, UINT cmd);                       /* no owner windows: NULL */
#define SW_HIDE 0
#define SW_SHOWNORMAL 1
#define SW_SHOWMINIMIZED 2
#define SW_SHOWNOACTIVATE 4
#define SW_SHOW 5
#define SW_MINIMIZE 6
#define SW_SHOWMINNOACTIVE 7
#define SW_RESTORE 9
BOOL ShowWindow(HWND h, int cmd);                       /* minimise / restore, done by the window thread */
HWND GetForegroundWindow(void);                         /* the game window when it has the keyboard focus */
BOOL IsIconic(HWND h);
BOOL SetForegroundWindow(HWND h);                       /* never steals the focus: FALSE */
BOOL SetWindowTextA(HWND h, LPCSTR text);
DWORD GetWindowThreadProcessId(HWND h, DWORD *pid);
SHORT GetAsyncKeyState(int vk);                         /* keyboard and mouse buttons of the game window (SDL state) */
#define KEYEVENTF_KEYUP 0x2u
void keybd_event(BYTE vk, BYTE scan, DWORD flags, ULONG_PTR extra);   /* a Windows test aid: does nothing */
#define MB_OK 0x0u
#define MB_YESNO 0x4u
#define MB_ICONERROR 0x10u
#define MB_ICONQUESTION 0x20u
#define MB_ICONWARNING 0x30u
#define MB_ICONINFORMATION 0x40u
#define MB_TOPMOST 0x40000u
#define MB_TASKMODAL 0x2000u
#define MB_SETFOREGROUND 0x10000u
#define IDOK 1
#define IDCANCEL 2
#define IDYES 6
#define IDNO 7
int MessageBoxA(HWND h, LPCSTR text, LPCSTR caption, UINT type);    /* SDL_ShowMessageBox */
int MessageBoxW(HWND h, LPCWSTR text, LPCWSTR caption, UINT type);
typedef UINT MMRESULT;
#define TIMERR_NOERROR 0u
static inline MMRESULT timeBeginPeriod(UINT ms) { (void)ms; return TIMERR_NOERROR; }   /* Darwin timers are fine-grained already */
static inline MMRESULT timeEndPeriod(UINT ms) { (void)ms; return TIMERR_NOERROR; }

/* Virtual-key codes: only numbers here (the keyboard bindings of controllers.ini are stored as them); reading the
 * keyboard is plan step 5 (SDL3). */
#define VK_LBUTTON 0x01
#define VK_RBUTTON 0x02
#define VK_MBUTTON 0x04
#define VK_XBUTTON1 0x05
#define VK_XBUTTON2 0x06
#define VK_BACK 0x08
#define VK_TAB 0x09
#define VK_RETURN 0x0D
#define VK_SHIFT 0x10
#define VK_CONTROL 0x11
#define VK_MENU 0x12
#define VK_ESCAPE 0x1B
#define VK_SPACE 0x20
#define VK_LEFT 0x25
#define VK_UP 0x26
#define VK_RIGHT 0x27
#define VK_DOWN 0x28
#define VK_NUMPAD0 0x60
#define VK_F1 0x70
#define VK_LSHIFT 0xA0
#define VK_RSHIFT 0xA1
#define VK_LCONTROL 0xA2
#define VK_RCONTROL 0xA3
#define VK_LMENU 0xA4
#define VK_RMENU 0xA5

#ifdef __cplusplus
}
#endif
#endif
