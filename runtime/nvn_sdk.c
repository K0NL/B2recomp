/* [Recomps#1] Port of ssbu-recomp's nvn_sdk.c to OUR sdk (NintendoSDK / NVN 3.6.0). Same API (nvn_sdk.h); the SDK code is
 * lifted from targets/exefs/sdk.elf by lifter/sdklift.py (roots: lifter/sdklift_roots.txt). Offsets are our sdk's.
 *
 * NVN 3.6.0 keeps its entry points in a name table without the "nvn" prefix ([sdk 0x2c113c]: binary search over 461 names
 * at [0x7ab63c] -> function table [0x925130]); evidence/sdk-nvn-procs.json has all of them:
 *   nvnTextureBuilderGetStorageSize      [sdk 0x2c9748] -> layout block [0x2c97ac]
 *   nvnTextureBuilderGetStorageAlignment [sdk 0x2c8d10]
 *   nvnTextureBuilderGetStorageClass     [sdk 0x2c8e58]
 *   nvnTextureBuilderGetSparseTileLayout [sdk 0x2ca370]   nvnTextureGetSparseTileLayout [sdk 0x2ca418]
 *   nvnTextureGetStorageClass            [sdk 0x2ca01c]  = 0x100 | byte +0x70 of the NVNtexture
 *   nvnTextureGetStorageSize             [sdk 0x2ca9a8]  = u32 +0x78 of the NVNtexture
 *   (no GetRawStorageClass in NVN 3.6.0: not in the name table, and our game does not ask for it)
 *
 * The memory kind comes from the GPU library, as in Smash's SDK, through NvRmGpuDeviceChooseKind [sdk 0x267d94]:
 *   GPU device (vtable +0x30, our class NvRmNvgpuDevice [vtable 0x8ead88]: +0x30 = [0x26c84c] returns [this+0x370])
 *   -> HAL object NvRmGpu_HwHalFunctions_GM200 [vtable 0x8ea858]: method +0x50 [0x269d00] dispatches to +0x58..+0x70.
 *   The HAL object is what the descriptor's factory [0x272884] builds: 0x28 bytes, [0] vtable, [1] the chip descriptor,
 *   rest 0. Descriptor "NVIDIA Tegra X1" [0x922c00]: id 0x12b at +8, factory at +0x10, kind table at +0x38 (0x100
 *   entries, count at +0x40). (Smash's SDK: same scheme, method +0x80, descriptor 0xc11e88.)
 *
 * NVNdevice model: only the fields the lifted code reads.
 *   +0x80    u32  0x10000   big page size: returned as the alignment of page-aligned kinds [0x2c8e3c]. Same hardware and
 *                           same NVN constant as in Smash's analysis (nvn_sdk.c upstream, device +0x90) - inferred, not
 *                           re-derived from our sdk's device initialisation.
 *   +0x918   ptr  GPU device object ({lib, device, address space} at +0x910; the chooser gets +0x910 and reads +8)
 *   +0x1998  u32  0, +0x199c u32 0: per-category compression switches; the code disables compression only when one equals
 *                           0x76165656 [0x2c8dd4 / 0x2c8e0c], a value that appears nowhere in our sdk's data (a debug /
 *                           developer setting): a retail device keeps them at their zero-initialised value (inferred).
 *   +0x1ab0  u8   1         compression supported (the gate in front of those checks). Writer not found in our sdk:
 *                           NOT VERIFIED; 1 matches Smash's equivalent (+0x1acc = 1) on the same GPU.
 * The format table [0x923078] (0x34-byte records, 0xa1 of them) and the kind tables are read from the mapped SDK image.
 * Nothing in this file computes a size or an alignment. */
#include <stdio.h>
#include <string.h>
#include <windows.h>
#include "sdklift.h"
#include "nvn_sdk.h"
#include "nvn_c4.h"
#include "nvn_c5.h"

#define SDK(a) (SDK_BASE + (uint64_t)(a))
#define P(v) ((uint64_t)(uintptr_t)(v))
#define FMT_TABLE 0x923078ull
#define FMT_REC 0x34ull

static uint8_t g_dev[0x1b00];                 /* NVNdevice model */
static uint64_t g_gpu_dev[0x378 / 8];         /* NvRmNvgpuDevice: vtable, HAL object at +0x370 */
static uint64_t g_hal[0x28 / 8];              /* NvRmGpu_HwHalFunctions_GM200 [0x272884] */
static volatile LONG g_state;                 /* 0 not built, 1 building, 2 ready */

static void build_model(void) {
    if (!sdk_image_ready()) {
        printf("\nnvn_sdk: sdk.elf is not mapped; the texture storage rules are the SDK's own code and cannot be answered without it.\n");
        report_and_exit(3);
    }
    g_hal[0] = SDK(0x8ea858);                  /* vtable symbol 0x8ea848 + 0x10 [0x2728ac..0x2728c0] */
    g_hal[1] = SDK(0x922c00);                  /* "NVIDIA Tegra X1", id 0x12b */
    g_gpu_dev[0] = SDK(0x8ead88);              /* NvRmNvgpuDevice vtable (typeinfo 0x8eae80) */
    g_gpu_dev[0x370 / 8] = P(g_hal);
    *(uint32_t *)(g_dev + 0x80) = 0x10000;
    *(uint64_t *)(g_dev + 0x918) = P(g_gpu_dev);
    *(uint32_t *)(g_dev + 0x1998) = 0;
    *(uint32_t *)(g_dev + 0x199c) = 0;
    g_dev[0x1ab0] = 1;
}
static void model(void) {
    for (;;) {
        LONG s = InterlockedCompareExchange(&g_state, 1, 0);
        if (s == 2) return;
        if (s == 0) { build_model(); InterlockedExchange(&g_state, 2); return; }
        Sleep(0);
    }
}

/* The NVNtextureBuilder of NVN 3.6.0, as its setters write it [0x2c8ba8 .. 0x2c8c7c, 0x2c8f68]:
 * +0 device, +8 storage pool, +0x10 offset, +0x18 flags, +0x1c target, +0x20 width, +0x24 height, +0x28 depth,
 * +0x2c levels, +0x30 format, +0x34 samples, +0x38 swizzle (4 words), +0x48 depth-stencil mode, +0x50 packaged data,
 * +0x58 u16 packaged layout (1 | (word & 0xfff) << 4: same packing as Smash's SDK), +0x5c stride, +0x60 GL texture name. */
static void real_builder(uint8_t *rb, const NvnSdkTexBuilder *b) {
    memset(rb, 0, 0x80);
    *(uint64_t *)(rb + 0) = P(g_dev);
    *(uint32_t *)(rb + 0x18) = b->flags;
    *(uint32_t *)(rb + 0x1c) = b->target;
    *(uint32_t *)(rb + 0x20) = b->width;
    *(uint32_t *)(rb + 0x24) = b->height;
    *(uint32_t *)(rb + 0x28) = b->depth;
    *(uint32_t *)(rb + 0x2c) = b->levels;
    *(uint32_t *)(rb + 0x30) = b->format;
    *(uint32_t *)(rb + 0x34) = b->samples;
    if (b->has_layout) *(uint16_t *)(rb + 0x58) = (uint16_t)(1u | ((b->layout & 0xfffu) << 4));
    *(uint32_t *)(rb + 0x5c) = b->stride;
    *(uint32_t *)(rb + 0x60) = b->glname;
}

static uint64_t run(Ctx *c, uint64_t fn, const NvnSdkTexBuilder *b) {
    uint8_t rb[0x80];
    model();
    real_builder(rb, b);
    c->x[0] = P(rb);
    sdk_dispatch(c, SDK(fn));
    return c->x[0];
}
uint64_t nvn_sdk_texture_storage_alignment(Ctx *c, const NvnSdkTexBuilder *b) { return run(c, 0x2c8d10, b); }
uint64_t nvn_sdk_texture_storage_size(Ctx *c, const NvnSdkTexBuilder *b) { return run(c, 0x2c9748, b); }
uint32_t nvn_sdk_texture_storage_class(Ctx *c, const NvnSdkTexBuilder *b) { return (uint32_t)run(c, 0x2c8e58, b); }

/* The NVNtexture nvnTextureInitialize [sdk 0x2c9b18] builds from a builder without a GL texture name (0xb0 bytes zeroed):
 * +8 device, +0x10 pool, +0x18 offset, +0x20 .. +0x50 = builder +0x18 .. +0x48 [0x2c9bc0 .. 0x2c9c24], +0x54 bit 0 / bit 1 =
 * format record +0xe / +0xf non-zero [0x2c9c38 .. 0x2c9c64], layout block at +0x58 written by [0x2c97ac](texture + 0x58,
 * builder) (kind at +0x70, storage size at +0x78). The pool check [0x2c9b9c: pool byte +0x11 bit 0] is a validity check of
 * the caller's pool, not part of the result. A builder with a GL texture name takes the interop path: not modelled. */
static int real_texture(Ctx *c, uint8_t *rt, uint8_t *rb, const NvnSdkTexBuilder *b) {
    if (b->glname) return 0;
    model();
    real_builder(rb, b);
    memset(rt, 0, 0xb0);
    memcpy(rt + 8, rb + 0, 8);
    memcpy(rt + 0x20, rb + 0x18, 0x34);
    const uint8_t *fr = (const uint8_t *)(uintptr_t)(SDK(FMT_TABLE) + FMT_REC * (b->format < NVN_SDK_FORMAT_COUNT ? b->format : 0));
    rt[0x54] = (uint8_t)((fr[0xe] ? 1 : 0) | (fr[0xf] ? 2 : 0));
    c->x[0] = P(rt + 0x58); c->x[1] = P(rb);
    sdk_dispatch(c, SDK(0x2c97ac));
    return 1;
}
static uint64_t run_tex(Ctx *c, uint64_t fn, const NvnSdkTexBuilder *b, uint64_t x1, int *ok) {
    uint8_t rb[0x80], rt[0xb0];
    *ok = real_texture(c, rt, rb, b);
    if (!*ok) return 0;
    c->x[0] = P(rt); c->x[1] = x1;
    sdk_dispatch(c, SDK(fn));
    return c->x[0];
}
/* NVN 3.6.0 has no GetRawStorageClass (neither builder nor texture form): only a game built for a later NVN calls these. */
uint32_t nvn_sdk_texture_raw_storage_class(Ctx *c, const NvnSdkTexBuilder *b) {
    (void)b; guest_trap("nvnTextureBuilderGetRawStorageClass does not exist in NVN 3.6.0", c->x[30]); return 0;
}
int nvn_sdk_tex_raw_storage_class(Ctx *c, const NvnSdkTexBuilder *b, uint32_t *out) {
    (void)b; (void)out; guest_trap("nvnTextureGetRawStorageClass does not exist in NVN 3.6.0", c->x[30]); return 0;
}
void nvn_sdk_texture_sparse_layout(Ctx *c, const NvnSdkTexBuilder *b, uint32_t out[4]) {
    uint8_t rb[0x80];
    model();
    real_builder(rb, b);
    c->x[0] = P(rb); c->x[1] = P(out);
    sdk_dispatch(c, SDK(0x2ca370));
}
int nvn_sdk_tex_storage_class(Ctx *c, const NvnSdkTexBuilder *b, uint32_t *out) { int ok; *out = (uint32_t)run_tex(c, 0x2ca01c, b, 0, &ok); return ok; }
int nvn_sdk_tex_sparse_layout(Ctx *c, const NvnSdkTexBuilder *b, uint32_t out[4]) { int ok; run_tex(c, 0x2ca418, b, P(out), &ok); return ok; }
int nvn_sdk_tex_storage_size(Ctx *c, const NvnSdkTexBuilder *b, uint32_t *out) {
    uint8_t rb[0x80], rt[0xb0];
    if (!real_texture(c, rt, rb, b)) return 0;
    memcpy(out, rt + 0x78, 4);                                 /* [0x2ca9a8] ldr w0, [x0, #0x78] */
    return 1;
}

/* Format table [sdk 0x923078]: 0x34-byte records; bit 0 of the first word = "this format exists" ([0x2c976c]). */
int nvn_sdk_format_known(uint32_t format) {
    model();
    return format < NVN_SDK_FORMAT_COUNT && (*(const uint8_t *)(uintptr_t)(SDK(FMT_TABLE) + FMT_REC * format) & 1);
}

/* Self-test (SSBU_SDK_SELFTEST): answers read in our sdk's disassembly, plus a sweep that must not stop anywhere
 * (every method the choosers reach must have been lifted). SSBU_NVN_STORAGE_TABLE=<file> writes the sweep. */
int nvn_sdk_selftest(Ctx *c) {
    int bad = 0, n = 0;
    NvnSdkTexBuilder b;
    const char *out = getenv("SSBU_NVN_STORAGE_TABLE");
    FILE *f = out && *out ? fopen(out, "w") : NULL;
#define SET(fl, tg, w_, h_, d_, lv, fm, sm) do { memset(&b, 0, sizeof b); b.flags = (fl); b.target = (tg); b.width = (w_); b.height = (h_); b.depth = (d_); b.levels = (lv); b.format = (fm); b.samples = (sm); } while (0)
#define WANT(what, got, want) do { uint64_t g_ = (got), w_ = (want); n++; if (g_ != w_) { bad++; printf("  FAIL nvn storage %s: 0x%llx, expected 0x%llx\n", what, (unsigned long long)g_, (unsigned long long)w_); } } while (0)
    SET(0x40, 1, 64, 64, 1, 1, 0x25, 0); WANT("alignment, flag bit 6 [0x2c8d24]", nvn_sdk_texture_storage_alignment(c, &b), 0x80);
    SET(0x10, 1, 64, 64, 1, 1, 0x25, 0); WANT("alignment, flag bit 4 [0x2c8d2c]", nvn_sdk_texture_storage_alignment(c, &b), 0x20);
    b.stride = 256; WANT("size, flag bit 4 = stride x height [0x2c9870]", nvn_sdk_texture_storage_size(c, &b), 256 * 64);
    for (uint32_t fm = 1; fm < NVN_SDK_FORMAT_COUNT; fm++) {
        if (!nvn_sdk_format_known(fm)) continue;
        uint32_t bpe = *(const uint8_t *)(uintptr_t)(SDK(FMT_TABLE) + FMT_REC * fm + 0x10);
        SET(0, 10, 100, 1, 1, 1, fm, 0);
        WANT("alignment, buffer target = bytes per element, 12 counts as 4 [0x2c8d34]", nvn_sdk_texture_storage_alignment(c, &b), bpe == 12 ? 4 : bpe);
        WANT("size, buffer target = width x bytes per element [0x2c9848]", nvn_sdk_texture_storage_size(c, &b), 100ull * bpe);
        static const uint32_t flags[] = {0, 8, 0x100, 0x108, 0x1008};
        for (int tg = 0; tg < 10; tg++) for (int fi = 0; fi < 5; fi++) for (uint32_t sm = 0; sm <= 4; sm += 2) {
            if ((tg == 5 || tg == 6) != (sm != 0)) continue;
            SET(flags[fi], (uint32_t)tg, 128, tg == 0 || tg == 3 ? 1 : 64, tg == 8 ? 1 : tg == 9 ? 6 : tg == 2 || tg == 4 || tg == 6 || tg == 3 ? 4 : 1, 1, fm, sm);
            uint64_t al = nvn_sdk_texture_storage_alignment(c, &b), sz = nvn_sdk_texture_storage_size(c, &b);
            uint32_t cl = nvn_sdk_texture_storage_class(c, &b);
            n++;
            if (al != 0x200 && al != 0x10000) { bad++; printf("  FAIL nvn storage alignment 0x%llx for format 0x%x target %d flags 0x%x: neither 0x200 nor the page size\n", (unsigned long long)al, fm, tg, flags[fi]); }
            if (al == 0x10000 && (sz & 0xffff)) { bad++; printf("  FAIL nvn storage size 0x%llx not a multiple of the page size for format 0x%x target %d\n", (unsigned long long)sz, fm, tg); }
            if (f) fprintf(f, "%x\t%d\t%x\t%u\t%llx\t%llx\t%x\n", fm, tg, flags[fi], sm, (unsigned long long)al, (unsigned long long)sz, cl);
            uint32_t tsz = 0;
            n++;
            if (!nvn_sdk_tex_storage_size(c, &b, &tsz) || (sz && tsz != (uint32_t)sz)) { bad++; printf("  FAIL nvn texture storage size: format 0x%x target %d flags 0x%x: texture 0x%x, builder 0x%llx\n", fm, tg, flags[fi], tsz, (unsigned long long)sz); }
        }
    }
    if (f) fclose(f);
    {
        uint32_t v, sp[4];
        SET(0x10, 1, 64, 64, 1, 1, 0x25, 0); b.stride = 256; v = 0;
        WANT("texture class, flag bit 4: kind byte cleared, 0x100 | 0 [0x2ca01c]", (nvn_sdk_tex_storage_class(c, &b, &v), v), 0x100);
        WANT("builder class, flag bits 4/6: 0x100 | 0 [0x2c8e7c -> 0x2c8f40]", nvn_sdk_texture_storage_class(c, &b), 0x100);
        SET(0, 10, 100, 1, 1, 1, 0x25, 0);
        WANT("builder class, buffer target: 0x100 | 0 [0x2c8e64 -> 0x2c8f40]", nvn_sdk_texture_storage_class(c, &b), 0x100);
        SET(0, 1, 256, 256, 1, 1, 0x25, 0); sp[0] = sp[1] = sp[2] = sp[3] = 0xdeadbeef;
        nvn_sdk_texture_sparse_layout(c, &b, sp);
        WANT("sparse layout without the sparse flag: zero words 0,1", (uint64_t)sp[0] | sp[1], 0);
        WANT("sparse layout without the sparse flag: zero words 2,3", (uint64_t)sp[2] | sp[3], 0);
        b.glname = 7; v = 0x55;
        WANT("texture made from an OpenGL name is not modelled", nvn_sdk_tex_storage_class(c, &b, &v), 0);
    }
    printf("  nvn_sdk: %d texture storage cases, %d failed\n", n, bad);
    bad += nvn_c4_selftest();
    bad += nvn_c5_selftest(c);
    return bad;
}
