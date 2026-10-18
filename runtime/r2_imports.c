/* [Recomps#1] R2: imports our game (SDK 3.6.0) calls that ssbu-recomp's runtime has no handler for, in the order the
 * R1 boot trace reached them (evidence/r1-run1-*.txt). Sources are marked as in nnmisc.c:
 *   [sdk]   our SDK module (targets/exefs/sdk.elf), disassembled at the quoted offset;
 *   [game]  our main, at the quoted address;
 *   [doc]   public documentation of the SDK, not checked against code.
 * Registered last (boot.c), so these are the handlers in effect for these names. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include "runtime.h"
#include "hostrt.h"
#include "image_layout.h"

#define A0 (c->x[0])
#define A1 (c->x[1])
#define A2 (c->x[2])
#define RET(v) (c->x[0] = (uint64_t)(v))

/* ---------------------------------------------------------------- nn::os memory heap
 * [game 0x71011fc6a8] nninitStartup: QueryMemoryInfo; heap = (MemoryInfo+0 - MemoryInfo+8) & ~(2 MB - 1);
 * SetMemoryHeapSize(heap), on failure SetMemoryHeapSize(32 MB); AllocateMemoryBlock(&addr, heap); then hands
 * [addr, heap) to its own malloc (f_71011fc460). So the game takes the whole heap as one block.
 *
 * MemoryInfo [doc]: +0x00 totalAvailableMemorySize, +0x08 totalUsedMemorySize, +0x10 totalMemoryHeapSize,
 * +0x18 allocatedMemoryHeapSize, +0x20 programSize, +0x28 totalThreadStackSize, +0x30 int threadCount.
 * ([sdk 0x865fc] only forwards to another system module, so the layout is not readable in our sdk.)
 * The numbers are ours: an application gets about 3.2 GB on the console; we offer 3 GB (RECOMPS_HEAP_MB overrides
 * the heap size directly). The heap is one host region, committed up front: guest code touches it directly. */
#define MEMORY_BLOCK_UNIT 0x200000ull                    /* [doc] nn::os::MemoryBlockUnitSize */
#define RESULT_OS_OUT_OF_MEMORY 0xca01ull                /* [doc] os module 1, description 101 (NOT VERIFIED against our sdk) */
static uint64_t g_total_available = 3ull << 30;
static uint8_t *g_heap;
static uint64_t g_heap_size, g_heap_used;
static SRWLOCK g_heap_lock = SRWLOCK_INIT;

static uint64_t used_memory(void) { return IMAGE_SIZE + 0x100000ull; }   /* program image + main thread stack */

static void os_QueryMemoryInfo(Ctx *c) {
    uint64_t out = A0;
    AcquireSRWLockShared(&g_heap_lock);
    wr64(out + 0x00, g_total_available);
    wr64(out + 0x08, used_memory() + g_heap_size);
    wr64(out + 0x10, g_heap_size);
    wr64(out + 0x18, g_heap_used);
    wr64(out + 0x20, IMAGE_SIZE);
    wr64(out + 0x28, 0x100000ull);
    wr32(out + 0x30, 1);
    ReleaseSRWLockShared(&g_heap_lock);
}

static void os_SetMemoryHeapSize(Ctx *c) {
    uint64_t size = A0;
    const char *env = getenv("RECOMPS_HEAP_MB");
    if (env && atoi(env) > 0 && size) size = (uint64_t)atoi(env) << 20 & ~(MEMORY_BLOCK_UNIT - 1);
    if (size % MEMORY_BLOCK_UNIT) { RET(RESULT_OS_OUT_OF_MEMORY); return; }
    AcquireSRWLockExclusive(&g_heap_lock);
    uint64_t r = 0;
    if (size == g_heap_size) r = 0;
    else if (g_heap_used) r = RESULT_OS_OUT_OF_MEMORY;     /* resizing a heap with blocks in use: not supported here */
    else {
        if (g_heap) { VirtualFree(g_heap, 0, MEM_RELEASE); g_heap = NULL; g_heap_size = 0; }
        if (size) {
            /* MEM_WRITE_WATCH: the renderer asks the OS which texture pages the game wrote (nvn_gl.c ww_newest). Without it
             * every texture in this heap was hashed in full every batch: 40-54 MB, 2-4 ms of the render thread per frame in
             * a fight (finding 88). SSBU_NO_WRITEWATCH=1 (nvn_gl.c) still turns the use of it off. */
            g_heap = VirtualAlloc(NULL, (SIZE_T)size, MEM_RESERVE | MEM_COMMIT | MEM_WRITE_WATCH, PAGE_READWRITE);
            if (!g_heap) g_heap = VirtualAlloc(NULL, (SIZE_T)size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
            if (g_heap) { g_heap_size = size; void watch_heap_created(uint64_t); void watch_heap_bounds(uint64_t, uint64_t); watch_heap_bounds((uint64_t)(uintptr_t)g_heap, size); watch_heap_created((uint64_t)(uintptr_t)g_heap); }
            else r = RESULT_OS_OUT_OF_MEMORY;
        }
    }
    ReleaseSRWLockExclusive(&g_heap_lock);
    printf("nn::os::SetMemoryHeapSize(0x%llx): %s, heap at %p\n", (unsigned long long)size, r ? "FAILED" : "ok", (void *)g_heap);
    RET(r);
}

static void os_AllocateMemoryBlock(Ctx *c) {             /* (uintptr_t *out, size_t size) -> Result */
    uint64_t out = A0, size = A1, r = RESULT_OS_OUT_OF_MEMORY, addr = 0;
    AcquireSRWLockExclusive(&g_heap_lock);
    if (size && size % MEMORY_BLOCK_UNIT == 0 && g_heap && g_heap_used + size <= g_heap_size) {
        addr = (uint64_t)(uintptr_t)g_heap + g_heap_used;   /* blocks are never freed by our game (no FreeMemoryBlock import) */
        g_heap_used += size;
        r = 0;
    }
    ReleaseSRWLockExclusive(&g_heap_lock);
    if (!r) wr64(out, addr);
    printf("nn::os::AllocateMemoryBlock(0x%llx): %s at 0x%llx\n", (unsigned long long)size, r ? "FAILED" : "ok", (unsigned long long)addr);
    RET(r);
}

/* [sdk 0x89cb8]: stores (allocate, free) once; aborts on a null pointer or on a second call. Our runtime's threads
 * do not use guest-allocated thread-local memory, so the pair is only kept (and the SDK's checks are kept too). */
static uint64_t g_tls_alloc, g_tls_free;
static void os_SetMemoryAllocatorForThreadLocal(Ctx *c) {
    if (!A0 || !A1 || g_tls_alloc || g_tls_free)
        guest_trap("nn::os::SetMemoryAllocatorForThreadLocal: null allocator or called twice (the SDK aborts here)", c->x[30] - 4);
    g_tls_alloc = A0;
    g_tls_free = A1;
}

/* ---------------------------------------------------------------- nn::mem::StandardAllocator, per instance
 * Our game builds its whole memory system on StandardAllocator (R2 boot trace, evidence/r2-*):
 *   [game 0x71011fc460] malloc's allocator: Initialize(heap block, 0xbb600000); then one Allocate(0xb6100000) from it;
 *   [game 0x7100da7fdc, 0x7100da8460] further allocators Initialize(buffer, size, false) over 8 KB-aligned pieces of it.
 * nnsdk.c's model (every allocator = the host heap, one global size, the buffer ignored) is a Smash shortcut: here it
 * put a 2.9 GB request on the Windows heap and ended in STATUS_HEAP_CORRUPTION. So each allocator here manages
 * exactly the buffer it was given, like the SDK's. The SDK's own allocator is not pure (TLS slots, mutexes,
 * MapPhysicalMemory / GetInfo SVCs in its closure: Query.java, 173 functions), so this is a host implementation:
 * first fit over an address-ordered free list with coalescing; block layout and policy are ours, not the SDK's.
 * Allocated block: [our 32-byte header: block start, block size, requested size, magic][64 bytes left to the caller]
 * [user data]. The caller's 32 bytes are needed: [game 0x7100da86ac..b4] writes its own size / tag / owner at p-0x18,
 * p-0x10, p-8 of every block it gets, and [game 0x7100da86ec] reads p-0x18 back before Free (on the host heap those
 * writes hit the CRT's aligned-block header), and [game 0x7100da8718] walks a list whose nodes are at p-0x30 (next at p-0x28):
 * the game owns at least 0x30 bytes in front of p. That the SDK's allocator leaves such room is inferred. An invalid
 * Free / GetSizeOf / Reallocate pointer traps (loud) instead of corrupting the list. */
/* Blocks (finding 89: the first version kept one address-ordered free list, so every Free walked it and every Allocate
 * scanned it from the start; with thousands of fragments in a fight that was 1-3 ms of the main thread per frame).
 * Now: boundary tags + segregated free lists (the TLSF scheme), every operation O(1) apart from the lock.
 *   Every block, free or allocated, is [start, start + size): size a multiple of 16, at least SA_MIN; the word at
 *   start and the word at start + size - 8 both hold size | SA_USED for an allocated block, size for a free one.
 *   Free block: [tag][prev free][next free] ... [tag], in one of SA_BINS lists (two-level size classes, a bitmap).
 *   Allocated block: [tag][gap < SA_MIN][our header: tag, block start, requested size, magic][64 bytes for the caller]
 *   [user data][at least 8 bytes][tag]; with no gap the header's first word IS the block's tag.
 * Free merges with the free neighbours found through the tags (the next block's first word, the previous block's last). */
typedef struct FreeBlk { uint64_t tag; struct FreeBlk *prev, *next; } FreeBlk;
typedef struct { uint64_t tag, start, req, magic; } AllocHdr;
#define SA_MAGIC 0x52454341534d454dull                   /* "MEMSACER" */
#define SA_MIN 128ull                                    /* smallest block */
#define SA_HDR 96ull                                     /* our 32-byte header + 64 bytes in front of the block for the caller */
#define SA_TAIL 16ull                                    /* room behind the user data for the end tag */
#define SA_USED 1ull
#define SA_FL0 7                                         /* first level: floor(log2(size)) from 7 (128) ... */
#define SA_FLN 34                                        /* ... to 40 (a block of up to 2 TB) */
#define SA_SL 16                                         /* second level: 16 classes per power of two */
#define SA_BINS (SA_FLN * SA_SL)
typedef struct { uint64_t self, base, size, used, peak; FreeBlk *bin[SA_BINS]; uint64_t map[(SA_BINS + 63) / 64]; SRWLOCK lock; } SaHeap;
static SaHeap g_sa[256];
static int g_sa_count;
static SRWLOCK g_sa_table = SRWLOCK_INIT;

static uint64_t up(uint64_t v, uint64_t a) { return (v + a - 1) & ~(a - 1); }
static int sa_fls(uint64_t v) { int r = 63; while (r && !(v >> r)) r--; return r; }
static int sa_bin_of(uint64_t size) {                    /* the class a free block of this size is kept in */
    int fl = sa_fls(size), sl = (int)((size >> (fl - 4)) & (SA_SL - 1));
    int b = (fl - SA_FL0) * SA_SL + sl;
    return b < 0 ? 0 : b >= SA_BINS ? SA_BINS - 1 : b;
}
static int sa_bin_search(uint64_t size) {                /* the first class whose every block holds this size */
    int fl = sa_fls(size);
    size += (1ull << (fl - 4)) - 1;
    return sa_bin_of(size);
}
static uint64_t sa_tag(uint64_t a) { return *(uint64_t *)(uintptr_t)a; }
static void sa_set_tags(uint64_t start, uint64_t size, uint64_t used) {
    *(uint64_t *)(uintptr_t)start = size | used;
    *(uint64_t *)(uintptr_t)(start + size - 8) = size | used;
}
static void sa_unlink(SaHeap *h, FreeBlk *f) {
    int b = sa_bin_of(f->tag & ~15ull);
    if (f->prev) f->prev->next = f->next; else h->bin[b] = f->next;
    if (f->next) f->next->prev = f->prev;
    if (!h->bin[b]) h->map[b >> 6] &= ~(1ull << (b & 63));
}
static void sa_push(SaHeap *h, uint64_t start, uint64_t size) {
    FreeBlk *f = (FreeBlk *)(uintptr_t)start;
    int b = sa_bin_of(size);
    sa_set_tags(start, size, 0);
    f->prev = NULL; f->next = h->bin[b];
    if (f->next) f->next->prev = f;
    h->bin[b] = f;
    h->map[b >> 6] |= 1ull << (b & 63);
}
static FreeBlk *sa_first_from(SaHeap *h, int b) {        /* the first block of the first non-empty class >= b */
    for (int w = b >> 6; w < (SA_BINS + 63) / 64; w++) {
        uint64_t m = h->map[w] & (w == (b >> 6) ? ~0ull << (b & 63) : ~0ull);
        if (m) { int i = 0; while (!(m >> i & 1)) i++; return h->bin[w * 64 + i]; }
    }
    return NULL;
}
static SaHeap *sa_find(uint64_t self) {
    SaHeap *h = NULL;
    AcquireSRWLockShared(&g_sa_table);
    for (int i = 0; i < g_sa_count; i++) if (g_sa[i].self == self) { h = &g_sa[i]; break; }
    ReleaseSRWLockShared(&g_sa_table);
    return h;
}
static SaHeap *sa_need(Ctx *c, uint64_t self, const char *op) {
    SaHeap *h = sa_find(self);
    if (!h) { char m[128]; snprintf(m, sizeof m, "nn::mem::StandardAllocator::%s on an allocator that was never initialised", op); guest_trap(m, c->x[30] - 4); }
    return h;
}
static void sa_setup(SaHeap *h, uint64_t self, uint64_t base, uint64_t size) {
    memset(h, 0, sizeof *h);
    InitializeSRWLock(&h->lock);
    h->self = self; h->base = base; h->size = size;
    sa_push(h, base, size);
}
static void sa_init(Ctx *c, uint64_t self, uint64_t buf, uint64_t size) {
    uint64_t base = up(buf, 16), end = (buf + size) & ~15ull;
    if (!buf || end <= base + SA_MIN) guest_trap("nn::mem::StandardAllocator::Initialize: buffer too small", c->x[30] - 4);
    AcquireSRWLockExclusive(&g_sa_table);
    SaHeap *h = NULL;
    for (int i = 0; i < g_sa_count; i++) if (g_sa[i].self == self) h = &g_sa[i];           /* re-initialised: start over */
    for (int i = 0; !h && i < g_sa_count; i++) if (!g_sa[i].self) h = &g_sa[i];            /* a slot Finalize left dead */
    if (!h) {
        if (g_sa_count == (int)(sizeof g_sa / sizeof g_sa[0])) { ReleaseSRWLockExclusive(&g_sa_table); guest_trap("too many StandardAllocators", c->x[30] - 4); }
        h = &g_sa[g_sa_count++];
    }
    sa_setup(h, self, base, end - base);
    ReleaseSRWLockExclusive(&g_sa_table);
}
static uint64_t sa_alloc_locked(SaHeap *h, uint64_t size, uint64_t align) {
    if (align < 16) align = 16;
    if (align & (align - 1)) return 0;
    uint64_t need = up(size ? size : 1, 16), want = SA_HDR + need + SA_TAIL + (align > 16 ? align - 16 : 0);
    if (want < SA_MIN) want = SA_MIN;
    if (want >= (1ull << (SA_FL0 + SA_FLN - 1))) return 0;
    FreeBlk *f = NULL;
    {   /* the exact class first (a few blocks: a fitting one is often there), then the first class that surely fits */
        int b = sa_bin_of(want), n = 0;
        for (FreeBlk *g = h->bin[b]; g && n < 8; g = g->next, n++) if ((g->tag & ~15ull) >= want) { f = g; break; }
        if (!f) f = sa_first_from(h, sa_bin_search(want));
    }
    if (!f) return 0;
    sa_unlink(h, f);
    uint64_t a = (uint64_t)(uintptr_t)f, stop = a + (f->tag & ~15ull), user = up(a + SA_HDR, align), start = a;
    if (user - SA_HDR - a >= SA_MIN) { sa_push(h, a, user - SA_HDR - a); start = user - SA_HDR; }     /* the front, given back */
    uint64_t end = user + need + SA_TAIL;
    if (stop - end >= SA_MIN) { sa_push(h, end, stop - end); stop = end; }                          /* the rest, given back */
    sa_set_tags(start, stop - start, SA_USED);
    AllocHdr *hd = (AllocHdr *)(uintptr_t)(user - SA_HDR);
    hd->tag = (stop - start) | SA_USED; hd->start = start; hd->req = size; hd->magic = SA_MAGIC ^ user;
    h->used += stop - start;
    if (h->used > h->peak) h->peak = h->used;
    return user;
}
static uint64_t sa_alloc(SaHeap *h, uint64_t size, uint64_t align) {
    AcquireSRWLockExclusive(&h->lock);
    uint64_t p = sa_alloc_locked(h, size, align);
    ReleaseSRWLockExclusive(&h->lock);
    return p;
}
static int sa_hdr_ok(SaHeap *h, uint64_t p) {
    AllocHdr *hd = (AllocHdr *)(uintptr_t)(p - SA_HDR);
    if (p < h->base + SA_HDR || p >= h->base + h->size || (p & 15) || hd->magic != (SA_MAGIC ^ p)) return 0;
    uint64_t s = hd->start, n = hd->tag & ~15ull;
    return (hd->tag & SA_USED) && s >= h->base && s + n <= h->base + h->size && s <= p - SA_HDR && sa_tag(s) == hd->tag && sa_tag(s + n - 8) == hd->tag;
}
static AllocHdr *sa_hdr(Ctx *c, SaHeap *h, uint64_t p, const char *op) {
    if (!sa_hdr_ok(h, p)) {
        char m[160]; snprintf(m, sizeof m, "nn::mem::StandardAllocator::%s: 0x%llx is not a live block of allocator 0x%llx", op,
                              (unsigned long long)p, (unsigned long long)h->self);
        guest_trap(m, c->x[30] - 4);
    }
    return (AllocHdr *)(uintptr_t)(p - SA_HDR);
}
static void sa_free_locked(SaHeap *h, AllocHdr *hd) {
    uint64_t start = hd->start, size = hd->tag & ~15ull, lim = h->base + h->size;
    hd->magic = 0;
    h->used -= size;
    if (start + size < lim) {                                          /* the next block, if free */
        uint64_t t = sa_tag(start + size);
        if (!(t & SA_USED)) { sa_unlink(h, (FreeBlk *)(uintptr_t)(start + size)); size += t & ~15ull; }
    }
    if (start > h->base) {                                             /* the previous block, if free (its end tag) */
        uint64_t t = sa_tag(start - 8);
        if (!(t & SA_USED)) { uint64_t ps = t & ~15ull; sa_unlink(h, (FreeBlk *)(uintptr_t)(start - ps)); start -= ps; size += ps; }
    }
    sa_push(h, start, size);
}
static void sa_free(Ctx *c, SaHeap *h, uint64_t p) {
    AcquireSRWLockExclusive(&h->lock);
    sa_free_locked(h, sa_hdr(c, h, p, "Free"));
    ReleaseSRWLockExclusive(&h->lock);
}
static uint64_t sa_largest_free(SaHeap *h) {
    for (int b = SA_BINS - 1; b >= 0; b--) {
        if (!(h->map[b >> 6] >> (b & 63) & 1)) continue;
        uint64_t best = 0;
        for (FreeBlk *f = h->bin[b]; f; f = f->next) if ((f->tag & ~15ull) > best) best = f->tag & ~15ull;
        return best;
    }
    return 0;
}
/* RECOMPS_SA_SELFTEST=1: random Allocate / Free / alignments on a 64 MB buffer, every block checked after each step
 * (tags at both ends agree, no two free blocks side by side, every free block in its class, used bytes add up). */
static int sa_check(SaHeap *h, int nlive) {
    uint64_t a = h->base, lim = h->base + h->size, used = 0;
    int prev_free = 0, nfree = 0, nused = 0;
    while (a < lim) {
        uint64_t t = sa_tag(a), n = t & ~15ull;
        if (n < SA_MIN || (n & 15) || a + n > lim || sa_tag(a + n - 8) != t) return 1;
        if (t & SA_USED) { used += n; nused++; prev_free = 0; }
        else {
            if (prev_free) return 2;
            int b = sa_bin_of(n), found = 0;
            for (FreeBlk *f = h->bin[b]; f; f = f->next) if ((uint64_t)(uintptr_t)f == a) { found = 1; break; }
            if (!found) return 3;
            prev_free = 1; nfree++;
        }
        a += n;
    }
    if (a != lim || used != h->used || nused != nlive) return 4;
    int listed = 0;
    for (int b = 0; b < SA_BINS; b++) for (FreeBlk *f = h->bin[b]; f; f = f->next) listed++;
    return listed != nfree ? 5 : 0;
}
static void sa_selftest(void) {
    enum { N = 4000 };
    static uint64_t live[N], sz[N];
    uint64_t bytes = 64ull << 20;
    uint8_t *buf = VirtualAlloc(NULL, (SIZE_T)bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    SaHeap *h = calloc(1, sizeof *h);
    unsigned x = 12345;
    int n = 0, bad = 0, step;
    if (!buf || !h) return;
    sa_setup(h, 1, (uint64_t)(uintptr_t)buf, bytes);
    for (step = 0; step < 200000 && !bad; step++) {
        x = x * 1103515245u + 12345u;
        if (n < N && (n == 0 || (x >> 16) % 100 < 55)) {
            x = x * 1103515245u + 12345u;
            uint64_t s = (x >> 8) % 100 < 90 ? 1 + (x >> 12) % 512 : 1 + (x >> 12) % 200000, al = 16ull << ((x >> 4) % 6);
            uint64_t p = sa_alloc_locked(h, s, al);
            if (!p) continue;
            if (p % al || !sa_hdr_ok(h, p)) { bad = 10; break; }
            memset((void *)(uintptr_t)p, 0xa5, (size_t)s);          /* the caller's bytes must not touch our tags */
            memset((void *)(uintptr_t)(p - 64), 0x5a, 64);
            live[n] = p; sz[n] = s; n++;
        } else if (n) {
            int k = (int)((x >> 8) % (unsigned)n);
            if (!sa_hdr_ok(h, live[k])) { bad = 11; break; }
            sa_free_locked(h, (AllocHdr *)(uintptr_t)(live[k] - SA_HDR));
            live[k] = live[--n]; sz[k] = sz[n];
        }
        if (step % 997 == 0 || step < 2000) bad = sa_check(h, n);
    }
    while (n && !bad) { sa_free_locked(h, (AllocHdr *)(uintptr_t)(live[--n] - SA_HDR)); bad = sa_check(h, n); }
    if (!bad && (h->used || sa_largest_free(h) != bytes)) bad = 12;
    printf("  sa: StandardAllocator (boundary tags, %d size classes): %d steps, random sizes and alignments, every block checked: %s (code %d)\n", SA_BINS, step, bad ? "FAILED" : "ok", bad);
    VirtualFree(buf, 0, MEM_RELEASE); free(h);
}

/* RECOMPS_SA_LOG=<file>: every Initialize / Allocate / Free / Finalize with allocator, pointer, size, alignment, caller. */
static FILE *g_sa_log;
static SRWLOCK g_sa_log_lock = SRWLOCK_INIT;
static void sa_log(const char *op, uint64_t self, uint64_t p, uint64_t size, uint64_t align, uint64_t caller) {
    static int state;                                    /* 0 unknown, 1 on, 2 off */
    if (state == 2) return;
    AcquireSRWLockExclusive(&g_sa_log_lock);
    if (!state) { const char *e = getenv("RECOMPS_SA_LOG"); g_sa_log = e && e[0] ? fopen(e, "w") : NULL; state = g_sa_log ? 1 : 2; }
    if (g_sa_log) fprintf(g_sa_log, "%s\t%llx\t%llx\t%llx\t%llx\t%llx\t%lu\n", op, (unsigned long long)self, (unsigned long long)p,
                          (unsigned long long)size, (unsigned long long)align, (unsigned long long)caller, GetCurrentThreadId());
    ReleaseSRWLockExclusive(&g_sa_log_lock);
}

static void sa_Initialize(Ctx *c) { sa_log("init", A0, A1, A2, 0, c->x[30]); sa_init(c, A0, A1, A2); }   /* (this, buf, size[, bool]) */
/* Finalize marks the slot dead instead of moving another allocator into it: other threads may hold a pointer to a slot
 * (sa_find returns one and drops the table lock), so slots never move. sa_init reuses a dead slot. */
static void sa_Finalize(Ctx *c) {
    sa_log("fini", A0, 0, 0, 0, c->x[30]);
    AcquireSRWLockExclusive(&g_sa_table);
    for (int i = 0; i < g_sa_count; i++) if (g_sa[i].self == A0) { g_sa[i].self = 0; break; }
    ReleaseSRWLockExclusive(&g_sa_table);
}
static void sa_Allocate(Ctx *c) { uint64_t p = sa_alloc(sa_need(c, A0, "Allocate"), A1, 16); sa_log("alloc", A0, p, A1, 16, c->x[30]); RET(p); }
static void sa_AllocateAligned(Ctx *c) { uint64_t p = sa_alloc(sa_need(c, A0, "Allocate"), A1, A2); sa_log("alloc", A0, p, A1, A2, c->x[30]); RET(p); }
static void sa_Free(Ctx *c) { if (A1) { sa_log("free", A0, A1, 0, 0, c->x[30]); sa_free(c, sa_need(c, A0, "Free"), A1); } }
static void sa_Reallocate(Ctx *c) {                      /* (this, void *p, size_t size) */
    SaHeap *h = sa_need(c, A0, "Reallocate");
    uint64_t p = A1, size = A2;
    if (!p) { RET(sa_alloc(h, size, 16)); return; }
    if (!size) { sa_free(c, h, p); RET(0); return; }
    AcquireSRWLockShared(&h->lock); uint64_t old = sa_hdr(c, h, p, "Reallocate")->req; ReleaseSRWLockShared(&h->lock);
    uint64_t q = sa_alloc(h, size, 16);
    if (q) { memcpy((void *)(uintptr_t)q, (void *)(uintptr_t)p, (size_t)(old < size ? old : size)); sa_free(c, h, p); }
    RET(q);
}
static void sa_GetSizeOf(Ctx *c) { SaHeap *h = sa_need(c, A0, "GetSizeOf"); AcquireSRWLockShared(&h->lock); RET(sa_hdr(c, h, A1, "GetSizeOf")->req); ReleaseSRWLockShared(&h->lock); }
static void sa_GetTotalFreeSize(Ctx *c) { SaHeap *h = sa_need(c, A0, "GetTotalFreeSize"); RET(h->size - h->used); }
static void sa_GetAllocatableSize(Ctx *c) {             /* the largest single request that would succeed now */
    SaHeap *h = sa_need(c, A0, "GetAllocatableSize");
    AcquireSRWLockShared(&h->lock);
    uint64_t best = sa_largest_free(h);
    ReleaseSRWLockShared(&h->lock);
    RET(best > SA_HDR + SA_TAIL ? best - SA_HDR - SA_TAIL : 0);
}
static void sa_Dump(Ctx *c) {
    SaHeap *h = sa_need(c, A0, "Dump");
    printf("StandardAllocator 0x%llx: buffer 0x%llx size 0x%llx, used 0x%llx (peak 0x%llx)\n", (unsigned long long)h->self,
           (unsigned long long)h->base, (unsigned long long)h->size, (unsigned long long)h->used, (unsigned long long)h->peak);
}

/* ---------------------------------------------------------------- nn::err::ErrorCode
 * [sdk 0x10eee4] GetInvalidErrorCode(): returns 0 in x0 (category 0, number 0).
 * [sdk 0x10eed4] IsValid() const: the 32-bit category at +0 is not 0. */
static void err_GetInvalidErrorCode(Ctx *c) { RET(0); }
static void err_ErrorCode_IsValid(Ctx *c) { RET(rd32(A0) != 0); }

/* ---------------------------------------------------------------- nn::settings::operator==(LanguageCode, LanguageCode)
 * [sdk 0x15bb08]: equal when the two 8-byte codes agree up to and including the first NUL (at most 8 bytes). */
static void settings_LanguageCode_eq(Ctx *c) {
    const uint8_t *a = (const uint8_t *)(uintptr_t)A0, *b = (const uint8_t *)(uintptr_t)A1;
    int eq = 1;
    for (int i = 0; i < 8; i++) {
        if (a[i] != b[i]) { eq = 0; break; }
        if (!a[i]) break;
    }
    RET(eq);
}

/* nn::account OpenPreselectedUser / OpenUser / CloseUser: registered in nnfs.c, in its one-user model. */

/* ---------------------------------------------------------------- nn::vi::SetLayerScalingMode(Layer *, ScalingMode) -> Result
 * [sdk 0x1be2a8]: forwards to the display service (IApplicationDisplayService method, IPC id 0x835 [0x1b8f84]) and returns
 * its Result. The host window does the scaling (display.c, graphics.ini), so there is nothing to change: success. */
static void vi_SetLayerScalingMode(Ctx *c) { RET(0); }

/* ---------------------------------------------------------------- nn::os::IsVirtualAddressMemoryEnabled() -> bool
 * [sdk] asks the kernel (svc GetInfo). Our game uses it once [game 0x7100da80bc]: a "free memory" helper that answers 0
 * when it is true and the heap allocator's free size otherwise. A 3.6.0-era title does not use the virtual address
 * memory manager (and imports none of its functions): false (inferred). */
static void os_IsVirtualAddressMemoryEnabled(Ctx *c) { RET(0); }

/* ---------------------------------------------------------------- nn::friends::UserPresence::Commit() -> Result
 * [sdk] sends the presence to the friends service (IPC). Offline console: the service keeps the local presence, nothing
 * goes out; success (inferred; the same policy as the other offline answers in nnmisc.c). Initialize / SetDescription are
 * the SDK's own code (lifted). */
static void friends_UserPresence_Commit(Ctx *c) { RET(0); }

/* ---------------------------------------------------------------- nn::oe::EnableRecording()
 * Allows the console's gameplay video capture. Not lifted: its code fetches the applet manager's service object
 * (nn::am::GetApplicationFunctions [sdk 0xba598]), which only the SDK's own oe initialisation creates; lifting it read
 * null (R2 run 16). The PC has no console capture: nothing to do. */
static void oe_EnableRecording(Ctx *c) { (void)c; }

static const ImportDef r2_imports[] = {
    {"_ZN2nn2oe15EnableRecordingEv", oe_EnableRecording},
    {"_ZN2nn2os29IsVirtualAddressMemoryEnabledEv", os_IsVirtualAddressMemoryEnabled},
    {"_ZN2nn7friends12UserPresence6CommitEv", friends_UserPresence_Commit},
    {"_ZN2nn2vi19SetLayerScalingModeEPNS0_5LayerENS0_11ScalingModeE", vi_SetLayerScalingMode},
    {"_ZN2nn2os15QueryMemoryInfoEPNS0_10MemoryInfoE", os_QueryMemoryInfo},
    {"_ZN2nn2os17SetMemoryHeapSizeEm", os_SetMemoryHeapSize},
    {"_ZN2nn2os19AllocateMemoryBlockEPmm", os_AllocateMemoryBlock},
    {"_ZN2nn2os32SetMemoryAllocatorForThreadLocalEPFPvmmEPFvS1_mE", os_SetMemoryAllocatorForThreadLocal},
#define SA(n) "_ZN2nn3mem17StandardAllocator" n
    {SA("10InitializeEPvm"), sa_Initialize}, {SA("10InitializeEPvmb"), sa_Initialize}, {SA("8FinalizeEv"), sa_Finalize},
    {SA("8AllocateEm"), sa_Allocate}, {SA("8AllocateEmm"), sa_AllocateAligned}, {SA("4FreeEPv"), sa_Free},
    {SA("10ReallocateEPvm"), sa_Reallocate},
#define SAK(n) "_ZNK2nn3mem17StandardAllocator" n                /* const member functions */
    {SAK("9GetSizeOfEPKv"), sa_GetSizeOf}, {SAK("18GetAllocatableSizeEv"), sa_GetAllocatableSize}, {SAK("4DumpEv"), sa_Dump},
    {SAK("16GetTotalFreeSizeEv"), sa_GetTotalFreeSize},         /* not imported by our main today; kept for completeness */
    {"_ZN2nn3err9ErrorCode19GetInvalidErrorCodeEv", err_GetInvalidErrorCode},
    {"_ZNK2nn3err9ErrorCode7IsValidEv", err_ErrorCode_IsValid},
    {"_ZN2nn8settingseqERKNS0_12LanguageCodeES3_", settings_LanguageCode_eq},
};

void acc_ListOpenUsers(Ctx *c);                          /* nnfs.c: the host account model */
void sdk_host_override(const char *name, ImportFn fn);   /* sdklift_rt.c */
void register_r2_imports(void) {
    if (getenv("RECOMPS_SA_SELFTEST")) { sa_selftest(); fflush(stdout); }
    register_imports(r2_imports, (int)(sizeof r2_imports / sizeof r2_imports[0]));
    /* cut in sdklift_roots.txt: lifted friends::detail::GetCurrentUser asks the account library for the open users */
    sdk_host_override("_ZN2nn7account13ListOpenUsersEPiPNS0_3UidEi", acc_ListOpenUsers);
}
