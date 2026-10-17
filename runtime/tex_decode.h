/* tex_decode.h - texture layout and decoding library for the NVN-on-OpenGL backend.
 *
 * Standalone C99 (clang 8), no dependencies beyond the C library. Everything an integrator needs is
 * described here; tex_decode.c need not be read.
 *
 * Three services:
 *
 *  1. FORMAT TABLE   tex_format(nvn_format) -> TexFormat*   (NVN format enum -> GL enums, block size,
 *     sRGB / compressed / depth flags, whether the GPU can take the data as-is or software decode is
 *     needed, default component swizzle).
 *
 *  2. LAYOUT         tex_layout(...) fills a TexLayout: per-level block height / depth, per-level
 *     offsets and sizes inside one layer, the layer stride, total storage size and alignment, for
 *     every NVN target (1D, 2D, 3D, arrays, cubemaps, cube arrays, multisample, rectangle) and for
 *     linear (pitch) textures. tex_deswizzle_level() turns one level of Tegra block-linear
 *     ("GOB tiled") memory into tightly packed rows; tex_swizzle_level() is the inverse.
 *     Rules (verified against the game's packaged nutexb textures, see tex_test.c):
 *       - 2D / arrays / cubes: block height of level 0 = 16, 8, 4, 2 or 1 GOBs, the largest with
 *         8 * bh <= 1.5 * height_in_blocks; block depth 1.
 *       - 3D: block height 1 GOB, block depth = 16, 8, 4, 2 or 1 slices by the same 1.5 rule on depth.
 *       - each further level halves block height / depth while the level fits in half of it;
 *       - levels follow each other inside a layer; layers are padded to a whole level-0 block.
 *     NOTE for nvn_gl.c: its own rule differs for 3D textures (it applies the 2D block-height rule to
 *     them): same storage size, but the game's 16x16x16 colour-grading tables come out scrambled.
 *
 *  3. DECODERS       tex_decode_rgba8() / tex_decode_rgba32f() decode a level of compressed blocks
 *     (BC1..BC7, ASTC all 2D footprints) to RGBA8 or float RGBA; tex_convert_rgba8() converts the
 *     uncompressed formats (for dumps and debugging). tex_write_png() writes an RGBA8 image.
 *
 * Typical use at upload time (block-linear texture, one level of one layer):
 *
 *     TexLayout L;
 *     tex_layout(&L, target, format, w, h, d, levels, flags, stride, packaged_bh_log2_or_minus1);
 *     const TexFormat *f = tex_format(format);
 *     const uint8_t *src = base + layer * L.layer_stride + L.lvl_offset[level];
 *     tex_deswizzle_level(&L, level, src, tmp);               // tmp: L.lvl_linear_size[level] bytes
 *     if (f->sw_decode) { tex_decode_rgba8(format, tmp, lw, lh, rgba); upload rgba as f->gl_sw_internal; }
 *     else upload tmp with f->gl_internal / f->gl_format / f->gl_type (compressed: f->gl_internal).
 *
 * Coordinates: row 0 of the linear output is row 0 of the texture (the NVN / GOB origin), nothing is
 * flipped. All sizes are in bytes. Nothing here allocates per pixel; the decoders use small stack
 * buffers only. Thread safety: all functions are pure except tex_format(), which fills a static table
 * once (call it once before starting threads, or accept a benign race on first use).
 */
#ifndef TEX_DECODE_H
#define TEX_DECODE_H
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------- NVN enums (public nvn.h values) */
enum {
    TEX_TARGET_1D = 0, TEX_TARGET_2D = 1, TEX_TARGET_3D = 2, TEX_TARGET_1D_ARRAY = 3, TEX_TARGET_2D_ARRAY = 4,
    TEX_TARGET_2D_MS = 5, TEX_TARGET_2D_MS_ARRAY = 6, TEX_TARGET_RECTANGLE = 7, TEX_TARGET_CUBEMAP = 8,
    TEX_TARGET_CUBEMAP_ARRAY = 9, TEX_TARGET_BUFFER = 10
};
enum { TEX_FLAG_LINEAR = 0x10, TEX_FLAG_LINEAR_RENDER_TARGET = 0x40 };   /* NVN_TEXTURE_FLAGS_* */
/* NVN swizzle components (nvnTextureBuilderSetSwizzle): 0 zero, 1 one, 2 R, 3 G, 4 B, 5 A */
enum { TEX_SWZ_ZERO = 0, TEX_SWZ_ONE = 1, TEX_SWZ_R = 2, TEX_SWZ_G = 3, TEX_SWZ_B = 4, TEX_SWZ_A = 5 };

/* Format enum values that appear by name below (the whole table covers 0x00..0x9C) */
enum {
    TEX_FMT_NONE = 0x00, TEX_FMT_R8 = 0x01, TEX_FMT_RG8 = 0x0D, TEX_FMT_RGBA8 = 0x25, TEX_FMT_RGBA16F = 0x29,
    TEX_FMT_RGBA32F = 0x2E, TEX_FMT_STENCIL8 = 0x31, TEX_FMT_DEPTH16 = 0x32, TEX_FMT_DEPTH24 = 0x33,
    TEX_FMT_DEPTH32F = 0x34, TEX_FMT_DEPTH24_STENCIL8 = 0x35, TEX_FMT_DEPTH32F_STENCIL8 = 0x36,
    TEX_FMT_RGBX8_SRGB = 0x37, TEX_FMT_RGBA8_SRGB = 0x38, TEX_FMT_RGBA4 = 0x39, TEX_FMT_RGB5 = 0x3A,
    TEX_FMT_RGB5A1 = 0x3B, TEX_FMT_RGB565 = 0x3C, TEX_FMT_RGB10A2 = 0x3D, TEX_FMT_RGB10A2UI = 0x3E,
    TEX_FMT_R11G11B10F = 0x3F, TEX_FMT_RGB9E5F = 0x40,
    TEX_FMT_RGB_DXT1 = 0x41, TEX_FMT_RGBA_DXT1 = 0x42, TEX_FMT_RGBA_DXT3 = 0x43, TEX_FMT_RGBA_DXT5 = 0x44,
    TEX_FMT_RGB_DXT1_SRGB = 0x45, TEX_FMT_RGBA_DXT1_SRGB = 0x46, TEX_FMT_RGBA_DXT3_SRGB = 0x47, TEX_FMT_RGBA_DXT5_SRGB = 0x48,
    TEX_FMT_RGTC1_UNORM = 0x49, TEX_FMT_RGTC1_SNORM = 0x4A, TEX_FMT_RGTC2_UNORM = 0x4B, TEX_FMT_RGTC2_SNORM = 0x4C,
    TEX_FMT_BPTC_UNORM = 0x4D, TEX_FMT_BPTC_UNORM_SRGB = 0x4E, TEX_FMT_BPTC_SFLOAT = 0x4F, TEX_FMT_BPTC_UFLOAT = 0x50,
    TEX_FMT_RGBX8 = 0x6D,
    TEX_FMT_ASTC_4x4 = 0x79, TEX_FMT_ASTC_5x4 = 0x7A, TEX_FMT_ASTC_5x5 = 0x7B, TEX_FMT_ASTC_6x5 = 0x7C, TEX_FMT_ASTC_6x6 = 0x7D,
    TEX_FMT_ASTC_8x5 = 0x7E, TEX_FMT_ASTC_8x6 = 0x7F, TEX_FMT_ASTC_8x8 = 0x80, TEX_FMT_ASTC_10x5 = 0x81, TEX_FMT_ASTC_10x6 = 0x82,
    TEX_FMT_ASTC_10x8 = 0x83, TEX_FMT_ASTC_10x10 = 0x84, TEX_FMT_ASTC_12x10 = 0x85, TEX_FMT_ASTC_12x12 = 0x86,
    TEX_FMT_ASTC_4x4_SRGB = 0x87, TEX_FMT_ASTC_12x12_SRGB = 0x94,
    TEX_FMT_BGR565 = 0x95, TEX_FMT_BGR5 = 0x96, TEX_FMT_BGR5A1 = 0x97, TEX_FMT_A1BGR5 = 0x98,
    TEX_FMT_BGRX8 = 0x99, TEX_FMT_BGRA8 = 0x9A, TEX_FMT_BGRX8_SRGB = 0x9B, TEX_FMT_BGRA8_SRGB = 0x9C,
    TEX_FMT_COUNT = 0x9D
};

/* ---------------------------------------------------------------- format table */
enum { TEX_KIND_COLOR = 0, TEX_KIND_DEPTH = 1, TEX_KIND_STENCIL = 2, TEX_KIND_DEPTH_STENCIL = 3 };
enum { TEX_CODEC_NONE = 0, TEX_CODEC_BC1, TEX_CODEC_BC2, TEX_CODEC_BC3, TEX_CODEC_BC4, TEX_CODEC_BC5, TEX_CODEC_BC6H, TEX_CODEC_BC7, TEX_CODEC_ASTC };

typedef struct TexFormat {
    uint16_t nvn;            /* NVN_FORMAT value */
    const char *name;        /* "RGBA8", "BPTC_UNORM_SRGB", ... */
    uint16_t gl_internal;    /* GL internal format for glTexStorage* / glCompressedTexSubImage* (0: not mappable to GL) */
    uint16_t gl_format;      /* GL pixel format for glTexSubImage* with raw texels (0 for compressed formats) */
    uint16_t gl_type;        /* GL pixel type for the same */
    uint16_t gl_sw_internal; /* GL internal format of the software-decoded image (GL_RGBA8 / GL_SRGB8_ALPHA8 / GL_RGBA16F), 0 if none */
    uint8_t bpb;             /* bytes per block (per texel when uncompressed) */
    uint8_t bw, bh;          /* block footprint in texels (1x1 uncompressed, 4x4 BCn, ASTC footprints) */
    uint8_t compressed;      /* 1: gl_internal is a compressed format (upload with glCompressedTexSubImage) */
    uint8_t codec;           /* TEX_CODEC_* : which software decoder applies (0 = none / uncompressed) */
    uint8_t sw_decode;       /* 1: the desktop GL driver does not expose this format (ASTC): decode in software to gl_sw_internal */
    uint8_t srgb;            /* 1: sRGB colour encoding */
    uint8_t kind;            /* TEX_KIND_* */
    uint8_t snorm;           /* 1: signed normalised (BC4/5 snorm, *SN formats) */
    uint8_t is_float;        /* 1: float / HDR (BC6H, *F formats); decoders produce floats for these */
    uint8_t is_int;          /* 1: integer (UI/I) texels */
    uint8_t ncomp;           /* colour components stored (1..4) */
    uint8_t swz[4];          /* default component mapping as TEX_SWZ_* (what a sampler sees for channels the format lacks: G,B = 0, A = 1) */
    uint8_t game_uses;       /* 1: seen in SSBU logs (NVN calls) or in its packaged nutexb textures */
} TexFormat;

/* Format description, or NULL if the value is not a known NVN format. */
const TexFormat *tex_format(int nvn_format);

/* ---------------------------------------------------------------- block-linear layout */
#define TEX_MAX_LEVELS 16

typedef struct TexLayout {
    /* inputs, normalised (width/height/depth >= 1, levels >= 1, levels clipped to the mip chain) */
    int target, format, width, height, depth, levels, layers, samples, flags;
    int bpb, bw, bh;               /* from the format */
    int linear;                    /* 1: pitch-linear (TEX_FLAG_LINEAR*), a single level, rows of `stride` bytes */
    size_t stride;                 /* linear only: row pitch in bytes */
    int is3d;                      /* depth is a dimension (TEX_TARGET_3D), not a layer count */
    int bh_log2, bd_log2;          /* GOB block height / depth (log2) of level 0, each 0..4  */
    /* per level, inside one layer */
    int lvl_w[TEX_MAX_LEVELS], lvl_h[TEX_MAX_LEVELS], lvl_d[TEX_MAX_LEVELS];          /* texels / slices */
    int lvl_wb[TEX_MAX_LEVELS], lvl_hb[TEX_MAX_LEVELS];                               /* blocks (compressed footprint) */
    int lvl_bh_log2[TEX_MAX_LEVELS], lvl_bd_log2[TEX_MAX_LEVELS];                     /* block height / depth of the level */
    size_t lvl_offset[TEX_MAX_LEVELS], lvl_size[TEX_MAX_LEVELS];                       /* swizzled bytes */
    size_t lvl_linear_size[TEX_MAX_LEVELS];                                           /* wb * bpb * hb * d: output of tex_deswizzle_level */
    size_t layer_size;             /* sum of the level sizes of one layer (no padding) */
    size_t layer_stride;           /* distance between layers (cube faces, array slices): layer_size padded to a whole level-0 block */
    size_t size;                   /* storage to reserve: layer_stride * layers (always padded; use for GetStorageSize) */
    size_t data_size;              /* bytes that actually hold texels: layer_size when layers == 1, else layer_stride * layers.
                                      This is the size of the data in the game's packaged textures (verified on 4,165 nutexb
                                      files); use it for bounds checks against a memory pool, `size` may exceed it by < 1 block. */
    size_t alignment;              /* storage alignment the layout implies (one block of level 0: 512 << (bh_log2 + bd_log2); 32 for linear) */
} TexLayout;

/* Fill L. flags are NVN texture flags (TEX_FLAG_LINEAR selects the pitch layout; `stride` is the
 * pitch in bytes, 0 = width * bpb). packaged_bh_log2: byte 0 of the packaged-texture layout
 * (nvnTextureBuilderSetPackagedTextureLayout) when known, else -1 to use the rule. Returns 0, or -1
 * for an unknown format (L is then filled as if RGBA8). */
int tex_layout(TexLayout *L, int target, int nvn_format, int width, int height, int depth, int levels, int flags, size_t stride, int packaged_bh_log2);

/* Pointer to (level, layer) of a block-linear texture whose storage starts at base. */
static inline const uint8_t *tex_level_ptr(const TexLayout *L, const uint8_t *base, int level, int layer) {
    return base + (size_t)layer * L->layer_stride + L->lvl_offset[level];
}

/* One level (all slices of a 3D level) from block-linear to tightly packed rows of blocks:
 * dst receives L->lvl_linear_size[level] bytes, row pitch lvl_wb * bpb. src points at the level
 * (tex_level_ptr). For linear textures the rows are copied from the pitch layout. */
void tex_deswizzle_level(const TexLayout *L, int level, const void *src, void *dst);
/* The inverse: packed rows -> block-linear; dst must hold L->lvl_size[level] bytes. */
void tex_swizzle_level(const TexLayout *L, int level, const void *src, void *dst);

/* Low-level primitives (level-independent). wbytes = row width in bytes (blocks * bpb), hrows =
 * rows of blocks, depth = slices; bh_log2 / bd_log2 the block height / depth of that level. */
size_t tex_bl_size(int wbytes, int hrows, int depth, int bh_log2, int bd_log2);                   /* swizzled bytes */
void tex_bl_to_linear(void *dst, const void *src, int wbytes, int hrows, int depth, int bh_log2, int bd_log2);
void tex_linear_to_bl(void *dst, const void *src, int wbytes, int hrows, int depth, int bh_log2, int bd_log2);
/* Byte offset of linear byte (x, y, z) inside a block-linear level. */
size_t tex_bl_offset(int x, int y, int z, int wbytes, int hrows, int bh_log2, int bd_log2);
/* The rules. hrows is the height of level 0 in blocks (texels / bh of the format). */
int tex_rule_block_height_log2(int hrows);
int tex_rule_block_depth_log2(int depth);                  /* 0..4 */
int tex_rule_mip_block_height_log2(int hrows, int bh0_log2);
int tex_rule_mip_block_depth_log2(int depth, int bd0_log2);

/* ---------------------------------------------------------------- decoders
 * src: tightly packed blocks of one level (output of tex_deswizzle_level), w x h texels; partial edge
 * blocks are clipped. dst: w*h*4 bytes (rgba8) or w*h*4 floats. Returns 0, or -1 if the format has no
 * software decoder (uncompressed formats go through tex_convert_rgba8).
 *  - unorm / sRGB formats: RGBA8 holds the stored values (no colour-space conversion); float output is value/255.
 *  - snorm (BC4/BC5 SNORM): float output is -1..1; RGBA8 maps -1..1 to 0..255 (0 -> 128).
 *  - BC6H: float output is the decoded half values; RGBA8 is clamped to 0..1 (for previews only).
 *  - BC4: G = B = 0, A = 255. BC5: B = 0, A = 255. RGB_DXT1: A = 255 (3-colour blocks give black).
 *  - ASTC: LDR only. HDR blocks (HDR endpoint modes, HDR void-extent) decode to magenta (255,0,255,255)
 *    and are counted in tex_astc_hdr_blocks. Invalid blocks give the ASTC error colour magenta too. */
int tex_decode_rgba8(int nvn_format, const void *src, int w, int h, uint8_t *dst);
int tex_decode_rgba32f(int nvn_format, const void *src, int w, int h, float *dst);
extern unsigned tex_astc_hdr_blocks, tex_astc_bad_blocks;      /* counters, informational */
/* BC7, three-subset partition 56 (mode 2 only; mode 0 has 16 partitions): the published anchor table
 * gives texel 5 for the second subset, the NVIDIA GL driver (597.16, RTX 5000 Ada) decodes with texel 6.
 * Default 6, so that software decodes equal what the GPU shows (bit-exact on 2.4M random values);
 * set to 5 for the published table. SSBU never uses BC7 modes 0 or 2 (0 of 227M blocks in 6,036
 * textures), so the choice does not affect this game. */
extern int tex_bc7_p56_anchor;

/* Decode a single block (16 or 8 bytes) into an out[bh][bw] RGBA8 array with row pitch pitch bytes
 * (bw, bh from the format; 4x4 for BCn). Same output conventions as above. */
int tex_decode_block_rgba8(int nvn_format, const uint8_t *block, uint8_t *out, int pitch);

/* Uncompressed texels -> RGBA8 (missing channels: G,B = 0, A = 255; float/half clamped to 0..1;
 * snorm mapped like the decoders; integers clamped to 0..255; depth = top 8 bits of the depth value,
 * stencil in R). src row pitch = w * bpb. Returns 0 or -1 for unsupported formats. */
int tex_convert_rgba8(int nvn_format, const void *src, int w, int h, uint8_t *dst);

/* ---------------------------------------------------------------- helpers */
float tex_half_to_float(uint16_t h);
uint16_t tex_float_to_half(float f);
/* PNG (RGBA8, stored deflate blocks, no compression). Returns 0 or -1. */
int tex_write_png(const char *path, const uint8_t *rgba, int w, int h);

#ifdef __cplusplus
}
#endif
#endif
