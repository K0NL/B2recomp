/* tex_decode.c - NVN texture formats, Tegra block-linear layout, BCn / ASTC software decoders.
 * See tex_decode.h for the API. Written from the public specifications (Khronos Data Format
 * Specification for BCn / ASTC, the GOB layout as documented by the open emulators and tegra_swizzle)
 * and verified by tex_test.c against the game's own packaged textures and the GL driver's decoders. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "tex_decode.h"

unsigned tex_astc_hdr_blocks, tex_astc_bad_blocks;

/* ================================================================ format table */
/* GL enums, so that this file needs no GL header */
enum {
    GL_R8_ = 0x8229, GL_R8_SNORM_ = 0x8F94, GL_R8UI_ = 0x8232, GL_R8I_ = 0x8231, GL_R16F_ = 0x822D, GL_R16_ = 0x822A, GL_R16_SNORM_ = 0x8F98,
    GL_R16UI_ = 0x8234, GL_R16I_ = 0x8233, GL_R32F_ = 0x822E, GL_R32UI_ = 0x8236, GL_R32I_ = 0x8235,
    GL_RG8_ = 0x822B, GL_RG8_SNORM_ = 0x8F95, GL_RG8UI_ = 0x8238, GL_RG8I_ = 0x8237, GL_RG16F_ = 0x822F, GL_RG16_ = 0x822C, GL_RG16_SNORM_ = 0x8F99,
    GL_RG16UI_ = 0x823A, GL_RG16I_ = 0x8239, GL_RG32F_ = 0x8230, GL_RG32UI_ = 0x823C, GL_RG32I_ = 0x823B,
    GL_RGB8_ = 0x8051, GL_RGB8_SNORM_ = 0x8F96, GL_RGB8UI_ = 0x8D7D, GL_RGB8I_ = 0x8D8F, GL_RGB16F_ = 0x881B, GL_RGB16_ = 0x8054, GL_RGB16_SNORM_ = 0x8F9A,
    GL_RGB16UI_ = 0x8D77, GL_RGB16I_ = 0x8D89, GL_RGB32F_ = 0x8815, GL_RGB32UI_ = 0x8D71, GL_RGB32I_ = 0x8D83,
    GL_RGBA8_ = 0x8058, GL_RGBA8_SNORM_ = 0x8F97, GL_RGBA8UI_ = 0x8D7C, GL_RGBA8I_ = 0x8D8E, GL_RGBA16F_ = 0x881A, GL_RGBA16_ = 0x805B, GL_RGBA16_SNORM_ = 0x8F9B,
    GL_RGBA16UI_ = 0x8D76, GL_RGBA16I_ = 0x8D88, GL_RGBA32F_ = 0x8814, GL_RGBA32UI_ = 0x8D70, GL_RGBA32I_ = 0x8D82,
    GL_UNSIGNED_BYTE_ = 0x1401, GL_BYTE_ = 0x1400, GL_UNSIGNED_SHORT_ = 0x1403, GL_SHORT_ = 0x1402, GL_UNSIGNED_INT_ = 0x1405, GL_INT_ = 0x1404,
    GL_FLOAT_ = 0x1406, GL_HALF_FLOAT_ = 0x140B,
    GL_RED_ = 0x1903, GL_RG_ = 0x8227, GL_RGB_ = 0x1907, GL_RGBA_ = 0x1908, GL_BGR_ = 0x80E0, GL_BGRA_ = 0x80E1,
    GL_RED_INTEGER_ = 0x8D94, GL_RG_INTEGER_ = 0x8228, GL_RGB_INTEGER_ = 0x8D98, GL_RGBA_INTEGER_ = 0x8D99, GL_BGRA_INTEGER_ = 0x8D9B,
    GL_DEPTH_COMPONENT_ = 0x1902, GL_DEPTH_STENCIL_ = 0x84F9, GL_STENCIL_INDEX_ = 0x1901,
    GL_STENCIL_INDEX8_ = 0x8D48, GL_DEPTH_COMPONENT16_ = 0x81A5, GL_DEPTH_COMPONENT24_ = 0x81A6, GL_DEPTH_COMPONENT32F_ = 0x8CAC,
    GL_DEPTH24_STENCIL8_ = 0x88F0, GL_DEPTH32F_STENCIL8_ = 0x8CAD, GL_UNSIGNED_INT_24_8_ = 0x84FA, GL_FLOAT_32_UNSIGNED_INT_24_8_REV_ = 0x8DAD,
    GL_SRGB8_ = 0x8C41, GL_SRGB8_ALPHA8_ = 0x8C43, GL_RGBA4_ = 0x8056, GL_RGB5_ = 0x8050, GL_RGB5_A1_ = 0x8057, GL_RGB565_ = 0x8D62,
    GL_RGB10_A2_ = 0x8059, GL_RGB10_A2UI_ = 0x906F, GL_R11F_G11F_B10F_ = 0x8C3A, GL_RGB9_E5_ = 0x8C3D,
    GL_UNSIGNED_SHORT_4_4_4_4_ = 0x8033, GL_UNSIGNED_SHORT_5_5_5_1_ = 0x8034, GL_UNSIGNED_SHORT_1_5_5_5_REV_ = 0x8366,
    GL_UNSIGNED_SHORT_5_6_5_ = 0x8363, GL_UNSIGNED_SHORT_5_6_5_REV_ = 0x8364, GL_UNSIGNED_INT_2_10_10_10_REV_ = 0x8368,
    GL_UNSIGNED_INT_10F_11F_11F_REV_ = 0x8C3B, GL_UNSIGNED_INT_5_9_9_9_REV_ = 0x8C3E,
    GL_COMPRESSED_RGB_S3TC_DXT1_ = 0x83F0, GL_COMPRESSED_RGBA_S3TC_DXT1_ = 0x83F1, GL_COMPRESSED_RGBA_S3TC_DXT3_ = 0x83F2, GL_COMPRESSED_RGBA_S3TC_DXT5_ = 0x83F3,
    GL_COMPRESSED_SRGB_S3TC_DXT1_ = 0x8C4C, GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT1_ = 0x8C4D, GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT3_ = 0x8C4E, GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT5_ = 0x8C4F,
    GL_COMPRESSED_RED_RGTC1_ = 0x8DBB, GL_COMPRESSED_SIGNED_RED_RGTC1_ = 0x8DBC, GL_COMPRESSED_RG_RGTC2_ = 0x8DBD, GL_COMPRESSED_SIGNED_RG_RGTC2_ = 0x8DBE,
    GL_COMPRESSED_RGBA_BPTC_UNORM_ = 0x8E8C, GL_COMPRESSED_SRGB_ALPHA_BPTC_UNORM_ = 0x8E8D, GL_COMPRESSED_RGB_BPTC_SIGNED_FLOAT_ = 0x8E8E, GL_COMPRESSED_RGB_BPTC_UNSIGNED_FLOAT_ = 0x8E8F,
    GL_COMPRESSED_RGBA_ASTC_4x4_ = 0x93B0, GL_COMPRESSED_SRGB8_ALPHA8_ASTC_4x4_ = 0x93D0
};

static TexFormat g_fmt[TEX_FMT_COUNT];
static int g_fmt_ready;

/* Formats the game has been seen to use: from the backend logs (NVN texture builder formats at the
 * title screen) and from a scan of 4,165 of its 104,186 .nutexb files (see tex_test.c nutexb mode). */
static const uint8_t k_game_formats[] = {
    0x11, 0x16, 0x22, 0x25, 0x2E, 0x35, 0x38, 0x3D, 0x42, 0x44, 0x46, 0x48, 0x49, 0x4B, 0x4D, 0x4E, 0x50, 0x9A
};

static void fmt_set_default_swz(TexFormat *f) {
    f->swz[0] = TEX_SWZ_R;
    f->swz[1] = f->ncomp >= 2 ? TEX_SWZ_G : TEX_SWZ_ZERO;
    f->swz[2] = f->ncomp >= 3 ? TEX_SWZ_B : TEX_SWZ_ZERO;
    f->swz[3] = f->ncomp >= 4 ? TEX_SWZ_A : TEX_SWZ_ONE;
}

static void fmt_build(void) {
    static const char *k_comp[4] = {"R", "RG", "RGB", "RGBA"};
    static const char *k_t12[12] = {"8", "8SN", "8UI", "8I", "16F", "16", "16SN", "16UI", "16I", "32F", "32UI", "32I"};
    static const uint16_t k_int12[4][12] = {
        {GL_R8_, GL_R8_SNORM_, GL_R8UI_, GL_R8I_, GL_R16F_, GL_R16_, GL_R16_SNORM_, GL_R16UI_, GL_R16I_, GL_R32F_, GL_R32UI_, GL_R32I_},
        {GL_RG8_, GL_RG8_SNORM_, GL_RG8UI_, GL_RG8I_, GL_RG16F_, GL_RG16_, GL_RG16_SNORM_, GL_RG16UI_, GL_RG16I_, GL_RG32F_, GL_RG32UI_, GL_RG32I_},
        {GL_RGB8_, GL_RGB8_SNORM_, GL_RGB8UI_, GL_RGB8I_, GL_RGB16F_, GL_RGB16_, GL_RGB16_SNORM_, GL_RGB16UI_, GL_RGB16I_, GL_RGB32F_, GL_RGB32UI_, GL_RGB32I_},
        {GL_RGBA8_, GL_RGBA8_SNORM_, GL_RGBA8UI_, GL_RGBA8I_, GL_RGBA16F_, GL_RGBA16_, GL_RGBA16_SNORM_, GL_RGBA16UI_, GL_RGBA16I_, GL_RGBA32F_, GL_RGBA32UI_, GL_RGBA32I_}};
    static const uint16_t k_type12[12] = {GL_UNSIGNED_BYTE_, GL_BYTE_, GL_UNSIGNED_BYTE_, GL_BYTE_, GL_HALF_FLOAT_, GL_UNSIGNED_SHORT_, GL_SHORT_, GL_UNSIGNED_SHORT_, GL_SHORT_, GL_FLOAT_, GL_UNSIGNED_INT_, GL_INT_};
    static const uint8_t k_size12[12] = {1, 1, 1, 1, 2, 2, 2, 2, 2, 4, 4, 4}, k_isint12[12] = {0, 0, 1, 1, 0, 0, 0, 1, 1, 0, 1, 1};
    static const uint8_t k_snorm12[12] = {0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0}, k_float12[12] = {0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0};
    static const uint16_t k_pix[4] = {GL_RED_, GL_RG_, GL_RGB_, GL_RGBA_}, k_pixint[4] = {GL_RED_INTEGER_, GL_RG_INTEGER_, GL_RGB_INTEGER_, GL_RGBA_INTEGER_};
    static const uint8_t k_astc[14][2] = {{4, 4}, {5, 4}, {5, 5}, {6, 5}, {6, 6}, {8, 5}, {8, 6}, {8, 8}, {10, 5}, {10, 6}, {10, 8}, {10, 10}, {12, 10}, {12, 12}};
    static const char *k_astc_name[14] = {"ASTC_4x4", "ASTC_5x4", "ASTC_5x5", "ASTC_6x5", "ASTC_6x6", "ASTC_8x5", "ASTC_8x6", "ASTC_8x8", "ASTC_10x5", "ASTC_10x6", "ASTC_10x8", "ASTC_10x10", "ASTC_12x10", "ASTC_12x12"};
    static const char *k_astc_name_srgb[14] = {"ASTC_4x4_SRGB", "ASTC_5x4_SRGB", "ASTC_5x5_SRGB", "ASTC_6x5_SRGB", "ASTC_6x6_SRGB", "ASTC_8x5_SRGB", "ASTC_8x6_SRGB", "ASTC_8x8_SRGB", "ASTC_10x5_SRGB", "ASTC_10x6_SRGB", "ASTC_10x8_SRGB", "ASTC_10x10_SRGB", "ASTC_12x10_SRGB", "ASTC_12x12_SRGB"};
    static char names[48][16];
    int i, nf;
    memset(g_fmt, 0, sizeof g_fmt);
    for (nf = 0; nf < TEX_FMT_COUNT; nf++) {
        TexFormat *f = &g_fmt[nf];
        f->nvn = (uint16_t)nf; f->name = "?"; f->bw = f->bh = 1; f->bpb = 1; f->ncomp = 4;
        if (nf == 0) { f->name = "NONE"; f->bpb = 0; continue; }
        if ((nf >= 0x01 && nf <= 0x30) || (nf >= 0x6D && nf <= 0x78) || (nf >= 0x51 && nf <= 0x68)) {
            int comps, t, ui2f = nf >= 0x51 && nf <= 0x68;
            if (nf >= 0x6D) { comps = 4; t = nf - 0x6D; }                                  /* RGBX: stored as 4 components, X ignored */
            else if (ui2f) { static const uint8_t m[6] = {2, 3, 7, 8, 10, 11}; comps = (nf - 0x51) / 6 + 1; t = m[(nf - 0x51) % 6]; }   /* vertex-only formats: described like their integer base */
            else { comps = (nf - 1) / 12 + 1; t = (nf - 1) % 12; }
            if (nf < 0x51) { sprintf(names[nf - 1], "%s%s", k_comp[comps - 1], k_t12[t]); f->name = names[nf - 1]; }
            else if (nf >= 0x6D) { f->name = (const char *[]){"RGBX8", "RGBX8SN", "RGBX8UI", "RGBX8I", "RGBX16F", "RGBX16", "RGBX16SN", "RGBX16UI", "RGBX16I", "RGBX32F", "RGBX32UI", "RGBX32I"}[t]; }
            else f->name = "UI2F/I2F (vertex only)";
            f->gl_internal = k_int12[comps - 1][t]; f->gl_format = k_isint12[t] ? k_pixint[comps - 1] : k_pix[comps - 1]; f->gl_type = k_type12[t];
            f->bpb = (uint8_t)(comps * k_size12[t]); f->ncomp = (uint8_t)(nf >= 0x6D ? 3 : comps);
            f->is_int = k_isint12[t]; f->snorm = k_snorm12[t]; f->is_float = k_float12[t];
            fmt_set_default_swz(f);
            continue;
        }
        if (nf >= 0x79 && nf <= 0x94) {
            i = (nf - 0x79) % 14;
            f->srgb = nf >= 0x87; f->name = f->srgb ? k_astc_name_srgb[i] : k_astc_name[i];
            f->bw = k_astc[i][0]; f->bh = k_astc[i][1]; f->bpb = 16; f->compressed = 1; f->codec = TEX_CODEC_ASTC;
            f->gl_internal = (uint16_t)((f->srgb ? GL_COMPRESSED_SRGB8_ALPHA8_ASTC_4x4_ : GL_COMPRESSED_RGBA_ASTC_4x4_) + i);
            f->sw_decode = 1; f->gl_sw_internal = f->srgb ? GL_SRGB8_ALPHA8_ : GL_RGBA8_;
            fmt_set_default_swz(f);
            continue;
        }
#define U(nm, in, fo, ty, b, nc) (f->name = nm, f->gl_internal = in, f->gl_format = fo, f->gl_type = ty, f->bpb = b, f->ncomp = nc)
#define BCF(nm, in, b, nc, cd) (f->name = nm, f->gl_internal = in, f->bpb = b, f->bw = f->bh = 4, f->compressed = 1, f->codec = cd, f->ncomp = nc)
        switch (nf) {
        case 0x31: U("STENCIL8", GL_STENCIL_INDEX8_, GL_STENCIL_INDEX_, GL_UNSIGNED_BYTE_, 1, 1); f->kind = TEX_KIND_STENCIL; f->is_int = 1; break;
        case 0x32: U("DEPTH16", GL_DEPTH_COMPONENT16_, GL_DEPTH_COMPONENT_, GL_UNSIGNED_SHORT_, 2, 1); f->kind = TEX_KIND_DEPTH; break;
        case 0x33: U("DEPTH24", GL_DEPTH_COMPONENT24_, GL_DEPTH_COMPONENT_, GL_UNSIGNED_INT_, 4, 1); f->kind = TEX_KIND_DEPTH; break;
        case 0x34: U("DEPTH32F", GL_DEPTH_COMPONENT32F_, GL_DEPTH_COMPONENT_, GL_FLOAT_, 4, 1); f->kind = TEX_KIND_DEPTH; f->is_float = 1; break;
        case 0x35: U("DEPTH24_STENCIL8", GL_DEPTH24_STENCIL8_, GL_DEPTH_STENCIL_, GL_UNSIGNED_INT_24_8_, 4, 2); f->kind = TEX_KIND_DEPTH_STENCIL; break;
        case 0x36: U("DEPTH32F_STENCIL8", GL_DEPTH32F_STENCIL8_, GL_DEPTH_STENCIL_, GL_FLOAT_32_UNSIGNED_INT_24_8_REV_, 8, 2); f->kind = TEX_KIND_DEPTH_STENCIL; f->is_float = 1; break;
        case 0x37: U("RGBX8_SRGB", GL_SRGB8_ALPHA8_, GL_RGBA_, GL_UNSIGNED_BYTE_, 4, 3); f->srgb = 1; break;
        case 0x38: U("RGBA8_SRGB", GL_SRGB8_ALPHA8_, GL_RGBA_, GL_UNSIGNED_BYTE_, 4, 4); f->srgb = 1; break;
        /* packed 16-bit formats: taken in the OpenGL sense of the name (first component in the most significant bits),
         * the same choice as nvn_gl.c. NOT verified against hardware; SSBU does not use them. */
        case 0x39: U("RGBA4", GL_RGBA4_, GL_RGBA_, GL_UNSIGNED_SHORT_4_4_4_4_, 2, 4); break;
        case 0x3A: U("RGB5", GL_RGB5_, GL_RGBA_, GL_UNSIGNED_SHORT_5_5_5_1_, 2, 3); break;
        case 0x3B: U("RGB5A1", GL_RGB5_A1_, GL_RGBA_, GL_UNSIGNED_SHORT_5_5_5_1_, 2, 4); break;
        case 0x3C: U("RGB565", GL_RGB565_, GL_RGB_, GL_UNSIGNED_SHORT_5_6_5_, 2, 3); break;
        case 0x3D: U("RGB10A2", GL_RGB10_A2_, GL_RGBA_, GL_UNSIGNED_INT_2_10_10_10_REV_, 4, 4); break;
        case 0x3E: U("RGB10A2UI", GL_RGB10_A2UI_, GL_RGBA_INTEGER_, GL_UNSIGNED_INT_2_10_10_10_REV_, 4, 4); f->is_int = 1; break;
        case 0x3F: U("R11G11B10F", GL_R11F_G11F_B10F_, GL_RGB_, GL_UNSIGNED_INT_10F_11F_11F_REV_, 4, 3); f->is_float = 1; break;
        case 0x40: U("RGB9E5F", GL_RGB9_E5_, GL_RGB_, GL_UNSIGNED_INT_5_9_9_9_REV_, 4, 3); f->is_float = 1; break;
        case 0x41: BCF("RGB_DXT1", GL_COMPRESSED_RGB_S3TC_DXT1_, 8, 3, TEX_CODEC_BC1); break;
        case 0x42: BCF("RGBA_DXT1", GL_COMPRESSED_RGBA_S3TC_DXT1_, 8, 4, TEX_CODEC_BC1); break;
        case 0x43: BCF("RGBA_DXT3", GL_COMPRESSED_RGBA_S3TC_DXT3_, 16, 4, TEX_CODEC_BC2); break;
        case 0x44: BCF("RGBA_DXT5", GL_COMPRESSED_RGBA_S3TC_DXT5_, 16, 4, TEX_CODEC_BC3); break;
        case 0x45: BCF("RGB_DXT1_SRGB", GL_COMPRESSED_SRGB_S3TC_DXT1_, 8, 3, TEX_CODEC_BC1); f->srgb = 1; break;
        case 0x46: BCF("RGBA_DXT1_SRGB", GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT1_, 8, 4, TEX_CODEC_BC1); f->srgb = 1; break;
        case 0x47: BCF("RGBA_DXT3_SRGB", GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT3_, 16, 4, TEX_CODEC_BC2); f->srgb = 1; break;
        case 0x48: BCF("RGBA_DXT5_SRGB", GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT5_, 16, 4, TEX_CODEC_BC3); f->srgb = 1; break;
        case 0x49: BCF("RGTC1_UNORM", GL_COMPRESSED_RED_RGTC1_, 8, 1, TEX_CODEC_BC4); break;
        case 0x4A: BCF("RGTC1_SNORM", GL_COMPRESSED_SIGNED_RED_RGTC1_, 8, 1, TEX_CODEC_BC4); f->snorm = 1; break;
        case 0x4B: BCF("RGTC2_UNORM", GL_COMPRESSED_RG_RGTC2_, 16, 2, TEX_CODEC_BC5); break;
        case 0x4C: BCF("RGTC2_SNORM", GL_COMPRESSED_SIGNED_RG_RGTC2_, 16, 2, TEX_CODEC_BC5); f->snorm = 1; break;
        case 0x4D: BCF("BPTC_UNORM", GL_COMPRESSED_RGBA_BPTC_UNORM_, 16, 4, TEX_CODEC_BC7); break;
        case 0x4E: BCF("BPTC_UNORM_SRGB", GL_COMPRESSED_SRGB_ALPHA_BPTC_UNORM_, 16, 4, TEX_CODEC_BC7); f->srgb = 1; break;
        case 0x4F: BCF("BPTC_SFLOAT", GL_COMPRESSED_RGB_BPTC_SIGNED_FLOAT_, 16, 3, TEX_CODEC_BC6H); f->is_float = 1; f->snorm = 1; break;
        case 0x50: BCF("BPTC_UFLOAT", GL_COMPRESSED_RGB_BPTC_UNSIGNED_FLOAT_, 16, 3, TEX_CODEC_BC6H); f->is_float = 1; break;
        case 0x69: U("RGB10A2SN", GL_RGB10_A2_, GL_RGBA_, GL_UNSIGNED_INT_2_10_10_10_REV_, 4, 4); f->snorm = 1; break;     /* vertex only */
        case 0x6A: U("RGB10A2I", GL_RGB10_A2UI_, GL_RGBA_INTEGER_, GL_UNSIGNED_INT_2_10_10_10_REV_, 4, 4); f->is_int = 1; break;
        case 0x6B: U("RGB10A2_UI2F", GL_RGB10_A2UI_, GL_RGBA_INTEGER_, GL_UNSIGNED_INT_2_10_10_10_REV_, 4, 4); break;
        case 0x6C: U("RGB10A2_I2F", GL_RGB10_A2UI_, GL_RGBA_INTEGER_, GL_UNSIGNED_INT_2_10_10_10_REV_, 4, 4); break;
        case 0x95: U("BGR565", GL_RGB565_, GL_BGR_, GL_UNSIGNED_SHORT_5_6_5_, 2, 3); break;
        case 0x96: U("BGR5", GL_RGB5_, GL_BGRA_, GL_UNSIGNED_SHORT_5_5_5_1_, 2, 3); break;
        case 0x97: U("BGR5A1", GL_RGB5_A1_, GL_BGRA_, GL_UNSIGNED_SHORT_5_5_5_1_, 2, 4); break;
        case 0x98: U("A1BGR5", GL_RGB5_A1_, GL_RGBA_, GL_UNSIGNED_SHORT_1_5_5_5_REV_, 2, 4); break;
        case 0x99: U("BGRX8", GL_RGBA8_, GL_BGRA_, GL_UNSIGNED_BYTE_, 4, 3); break;
        case 0x9A: U("BGRA8", GL_RGBA8_, GL_BGRA_, GL_UNSIGNED_BYTE_, 4, 4); break;
        case 0x9B: U("BGRX8_SRGB", GL_SRGB8_ALPHA8_, GL_BGRA_, GL_UNSIGNED_BYTE_, 4, 3); f->srgb = 1; break;
        case 0x9C: U("BGRA8_SRGB", GL_SRGB8_ALPHA8_, GL_BGRA_, GL_UNSIGNED_BYTE_, 4, 4); f->srgb = 1; break;
        default: f->name = NULL; break;
        }
#undef U
#undef BCF
        if (f->name) fmt_set_default_swz(f);
    }
    for (i = 0; i < (int)sizeof k_game_formats; i++) g_fmt[k_game_formats[i]].game_uses = 1;
    g_fmt_ready = 1;
}

const TexFormat *tex_format(int nf) {
    if (!g_fmt_ready) fmt_build();
    if (nf <= 0 || nf >= TEX_FMT_COUNT || !g_fmt[nf].name) return NULL;
    return &g_fmt[nf];
}

/* ================================================================ block-linear layout
 * A GOB is 64 bytes x 8 rows (512 bytes). Inside a GOB the byte at (x, y) sits at
 *   ((x & 32) << 3) | ((y & 6) << 5) | ((x & 16) << 1) | ((y & 1) << 4) | (x & 15).
 * A block stacks 2^bh GOBs vertically and 2^bd slices; blocks are laid out in x then y then z. */
static inline size_t gob_offset(int x, int y) {
    return (size_t)(((x & 32) << 3) | ((y & 6) << 5) | ((x & 16) << 1) | ((y & 1) << 4) | (x & 15));
}

int tex_rule_block_height_log2(int hrows) {
    int h = hrows + hrows / 2;
    return h >= 128 ? 4 : h >= 64 ? 3 : h >= 32 ? 2 : h >= 16 ? 1 : 0;
}
int tex_rule_block_depth_log2(int depth) {
    int d = depth + depth / 2;
    return d >= 16 ? 4 : d >= 8 ? 3 : d >= 4 ? 2 : d >= 2 ? 1 : 0;
}
int tex_rule_mip_block_height_log2(int hrows, int bh0) {
    while (bh0 > 0 && hrows <= (8 << bh0) / 2) bh0--;
    return bh0;
}
int tex_rule_mip_block_depth_log2(int depth, int bd0) {
    while (bd0 > 0 && depth <= (1 << bd0) / 2) bd0--;
    return bd0;
}

size_t tex_bl_size(int wbytes, int hrows, int depth, int bh_log2, int bd_log2) {
    size_t pitch = ((size_t)wbytes + 63) / 64 * 64, rows = 8 << bh_log2, slices = (size_t)1 << bd_log2;
    rows = ((size_t)hrows + rows - 1) / rows * rows;
    slices = ((size_t)depth + slices - 1) / slices * slices;
    return pitch * rows * slices;
}

size_t tex_bl_offset(int x, int y, int z, int wbytes, int hrows, int bh_log2, int bd_log2) {
    size_t gobs_x = ((size_t)wbytes + 63) / 64, block = (size_t)512 << (bh_log2 + bd_log2), rob = block * gobs_x;
    size_t slice = rob * (((size_t)hrows + (8 << bh_log2) - 1) >> (3 + bh_log2));
    int gy = y >> 3;
    return (size_t)(z >> bd_log2) * slice + (size_t)(gy >> bh_log2) * rob + (size_t)(x >> 6) * block
         + (size_t)(z & ((1 << bd_log2) - 1)) * ((size_t)512 << bh_log2) + (size_t)(gy & ((1 << bh_log2) - 1)) * 512 + gob_offset(x & 63, y & 7);
}

/* Copies a row of wbytes between linear memory and a GOB row; 16-byte runs are contiguous in a GOB. */
static void bl_row(uint8_t *lin, uint8_t *bl, int wbytes, int y, size_t block, int to_linear) {
    int x = 0;
    while (x < wbytes) {
        int n = 16 - (x & 15);
        if (n > wbytes - x) n = wbytes - x;
        uint8_t *g = bl + (size_t)(x >> 6) * block + gob_offset(x & 63, y & 7);
        if (to_linear) memcpy(lin + x, g, (size_t)n); else memcpy(g, lin + x, (size_t)n);
        x += n;
    }
}

static void bl_copy(void *linear, void *bl, int wbytes, int hrows, int depth, int bh_log2, int bd_log2, int to_linear) {
    size_t gobs_x = ((size_t)wbytes + 63) / 64, block = (size_t)512 << (bh_log2 + bd_log2), rob = block * gobs_x;
    size_t slice = rob * (((size_t)hrows + (8 << bh_log2) - 1) >> (3 + bh_log2));
    int z, y;
    for (z = 0; z < depth; z++)
        for (y = 0; y < hrows; y++) {
            int gy = y >> 3;
            uint8_t *b = (uint8_t *)bl + (size_t)(z >> bd_log2) * slice + (size_t)(gy >> bh_log2) * rob
                       + (size_t)(z & ((1 << bd_log2) - 1)) * ((size_t)512 << bh_log2) + (size_t)(gy & ((1 << bh_log2) - 1)) * 512;
            bl_row((uint8_t *)linear + ((size_t)z * hrows + y) * wbytes, b, wbytes, y, block, to_linear);
        }
}

void tex_bl_to_linear(void *dst, const void *src, int wbytes, int hrows, int depth, int bh_log2, int bd_log2) {
    bl_copy(dst, (void *)src, wbytes, hrows, depth, bh_log2, bd_log2, 1);
}
void tex_linear_to_bl(void *dst, const void *src, int wbytes, int hrows, int depth, int bh_log2, int bd_log2) {
    bl_copy((void *)src, dst, wbytes, hrows, depth, bh_log2, bd_log2, 0);
}

static int mipdim(int v, int l) { v >>= l; return v > 0 ? v : 1; }

int tex_layout(TexLayout *L, int target, int nf, int width, int height, int depth, int levels, int flags, size_t stride, int packaged_bh_log2) {
    const TexFormat *f = tex_format(nf);
    int ok = f != NULL, l, maxdim, maxl;
    if (!f) f = tex_format(TEX_FMT_RGBA8);
    memset(L, 0, sizeof *L);
    L->target = target; L->format = nf; L->flags = flags;
    L->width = width > 0 ? width : 1; L->height = height > 0 ? height : 1; L->depth = depth > 0 ? depth : 1;
    L->bpb = f->bpb; L->bw = f->bw; L->bh = f->bh; L->samples = 1;
    L->is3d = target == TEX_TARGET_3D;
    switch (target) {
    case TEX_TARGET_1D_ARRAY: case TEX_TARGET_2D_ARRAY: case TEX_TARGET_2D_MS_ARRAY: L->layers = L->depth; L->depth = 1; break;
    case TEX_TARGET_CUBEMAP: L->layers = 6; L->depth = 1; break;
    case TEX_TARGET_CUBEMAP_ARRAY: L->layers = L->depth; L->depth = 1; break;   /* NVN counts a cube array's depth in faces */
    case TEX_TARGET_3D: L->layers = 1; break;
    default: L->layers = 1; L->depth = 1; break;
    }
    if (target == TEX_TARGET_1D || target == TEX_TARGET_1D_ARRAY) L->height = 1;
    maxdim = L->width > L->height ? L->width : L->height;
    if (L->is3d && L->depth > maxdim) maxdim = L->depth;
    for (maxl = 1; (maxdim >> maxl) > 0; maxl++) {}
    L->levels = levels > 0 ? levels : 1;
    if (L->levels > maxl) L->levels = maxl;
    if (L->levels > TEX_MAX_LEVELS) L->levels = TEX_MAX_LEVELS;
    if (target == TEX_TARGET_2D_MS || target == TEX_TARGET_2D_MS_ARRAY || target == TEX_TARGET_RECTANGLE || target == TEX_TARGET_BUFFER) L->levels = 1;
    if (flags & (TEX_FLAG_LINEAR | TEX_FLAG_LINEAR_RENDER_TARGET)) {
        L->linear = 1; L->levels = 1; L->layers = 1; L->depth = 1;
        L->stride = stride ? stride : (size_t)((L->width + f->bw - 1) / f->bw) * f->bpb;
        L->lvl_w[0] = L->width; L->lvl_h[0] = L->height; L->lvl_d[0] = 1;
        L->lvl_wb[0] = (L->width + f->bw - 1) / f->bw; L->lvl_hb[0] = (L->height + f->bh - 1) / f->bh;
        L->lvl_size[0] = L->stride * (size_t)L->lvl_hb[0];
        L->lvl_linear_size[0] = (size_t)L->lvl_wb[0] * f->bpb * (size_t)L->lvl_hb[0];
        L->layer_size = L->layer_stride = L->size = L->data_size = L->lvl_size[0];
        L->alignment = 32;
        return ok ? 0 : -1;
    }
    {
        int hb0 = (L->height + f->bh - 1) / f->bh;
        /* 3D textures use one-GOB-high blocks and stack slices instead (seen on the game's 16x16x16 colour
         * grading tables: only bh 0 / bd 16 gives an ordered cube; same rule as the tegra_swizzle crate) */
        L->bh_log2 = packaged_bh_log2 >= 0 && packaged_bh_log2 <= 4 ? packaged_bh_log2 : L->is3d ? 0 : tex_rule_block_height_log2(hb0);
        L->bd_log2 = L->is3d ? tex_rule_block_depth_log2(L->depth) : 0;
    }
    {
        size_t off = 0, block;
        for (l = 0; l < L->levels; l++) {
            int w = mipdim(L->width, l), h = mipdim(L->height, l), d = L->is3d ? mipdim(L->depth, l) : 1;
            int wb = (w + f->bw - 1) / f->bw, hb = (h + f->bh - 1) / f->bh;
            L->lvl_w[l] = w; L->lvl_h[l] = h; L->lvl_d[l] = d; L->lvl_wb[l] = wb; L->lvl_hb[l] = hb;
            if (L->levels == 1) { L->lvl_bh_log2[l] = L->bh_log2; L->lvl_bd_log2[l] = L->bd_log2; }      /* a single level keeps the block as given */
            else { L->lvl_bh_log2[l] = tex_rule_mip_block_height_log2(hb, L->bh_log2); L->lvl_bd_log2[l] = tex_rule_mip_block_depth_log2(d, L->bd_log2); }
            L->lvl_offset[l] = off;
            L->lvl_size[l] = tex_bl_size(wb * f->bpb, hb, d, L->lvl_bh_log2[l], L->lvl_bd_log2[l]);
            L->lvl_linear_size[l] = (size_t)wb * f->bpb * (size_t)hb * (size_t)d;
            off += L->lvl_size[l];
        }
        block = (size_t)512 << (tex_rule_mip_block_height_log2(L->lvl_hb[0], L->bh_log2) + tex_rule_mip_block_depth_log2(L->lvl_d[0], L->bd_log2));
        L->layer_size = off;
        L->layer_stride = (off + block - 1) / block * block;
        L->alignment = block;
    }
    L->size = L->layer_stride * (size_t)L->layers;
    L->data_size = L->layers == 1 ? L->layer_size : L->size;
    return ok ? 0 : -1;
}

void tex_deswizzle_level(const TexLayout *L, int level, const void *src, void *dst) {
    int wbytes = L->lvl_wb[level] * L->bpb, y;
    if (L->linear) {
        for (y = 0; y < L->lvl_hb[level]; y++) memcpy((uint8_t *)dst + (size_t)y * wbytes, (const uint8_t *)src + (size_t)y * L->stride, (size_t)wbytes);
        return;
    }
    tex_bl_to_linear(dst, src, wbytes, L->lvl_hb[level], L->lvl_d[level], L->lvl_bh_log2[level], L->lvl_bd_log2[level]);
}

void tex_swizzle_level(const TexLayout *L, int level, const void *src, void *dst) {
    int wbytes = L->lvl_wb[level] * L->bpb, y;
    if (L->linear) {
        for (y = 0; y < L->lvl_hb[level]; y++) memcpy((uint8_t *)dst + (size_t)y * L->stride, (const uint8_t *)src + (size_t)y * wbytes, (size_t)wbytes);
        return;
    }
    tex_linear_to_bl(dst, src, wbytes, L->lvl_hb[level], L->lvl_d[level], L->lvl_bh_log2[level], L->lvl_bd_log2[level]);
}

/* ================================================================ half floats */
float tex_half_to_float(uint16_t h) {
    uint32_t sign = (uint32_t)(h & 0x8000) << 16, exp = (h >> 10) & 0x1F, mant = h & 0x3FF, bits;
    float r;
    if (exp == 0) {
        if (mant == 0) bits = sign;
        else { int e = 113; while (!(mant & 0x400)) { mant <<= 1; e--; } mant &= 0x3FF; bits = sign | ((uint32_t)e << 23) | (mant << 13); }
    } else if (exp == 31) bits = sign | 0x7F800000u | (mant << 13);
    else bits = sign | ((exp + 112) << 23) | (mant << 13);
    memcpy(&r, &bits, 4);
    return r;
}

uint16_t tex_float_to_half(float f) {
    uint32_t b, sign, exp, mant;
    memcpy(&b, &f, 4);
    sign = (b >> 16) & 0x8000; exp = (b >> 23) & 0xFF; mant = b & 0x7FFFFF;
    if (exp == 255) return (uint16_t)(sign | 0x7C00 | (mant ? 0x200 : 0));
    if (exp > 142) return (uint16_t)(sign | 0x7C00);
    if (exp < 103) return (uint16_t)sign;
    if (exp < 113) { mant |= 0x800000; mant >>= (113 - exp); return (uint16_t)(sign | ((mant + 0x1000) >> 13)); }
    return (uint16_t)(sign | ((exp - 112) << 10) | ((mant + 0x1000) >> 13));
}

/* ================================================================ BC1 .. BC5 */
static inline uint32_t rd32le(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static inline uint64_t rd64le(const uint8_t *p) { return (uint64_t)rd32le(p) | (uint64_t)rd32le(p + 4) << 32; }

/* BC1 colour block (8 bytes) into out[4][4] RGBA8 (pitch bytes per row). four_colors: force the
 * 4-colour mode (BC2/BC3 colour blocks); alpha0: 3-colour mode gives alpha 0 (DXT1 with alpha). */
static void bc1_block(const uint8_t *b, uint8_t *out, int pitch, int four_colors, int alpha0) {
    uint32_t c0 = (uint32_t)b[0] | (uint32_t)b[1] << 8, c1 = (uint32_t)b[2] | (uint32_t)b[3] << 8, idx = rd32le(b + 4);
    uint8_t pal[4][4];
    int i, y, x;
    pal[0][0] = (uint8_t)((c0 >> 11) << 3 | (c0 >> 13)); pal[0][1] = (uint8_t)(((c0 >> 5) & 63) << 2 | ((c0 >> 9) & 3)); pal[0][2] = (uint8_t)((c0 & 31) << 3 | ((c0 >> 2) & 7)); pal[0][3] = 255;
    pal[1][0] = (uint8_t)((c1 >> 11) << 3 | (c1 >> 13)); pal[1][1] = (uint8_t)(((c1 >> 5) & 63) << 2 | ((c1 >> 9) & 3)); pal[1][2] = (uint8_t)((c1 & 31) << 3 | ((c1 >> 2) & 7)); pal[1][3] = 255;
    if (four_colors || c0 > c1) {
        for (i = 0; i < 3; i++) { pal[2][i] = (uint8_t)((2 * pal[0][i] + pal[1][i] + 1) / 3); pal[3][i] = (uint8_t)((pal[0][i] + 2 * pal[1][i] + 1) / 3); }
        pal[2][3] = pal[3][3] = 255;
    } else {
        for (i = 0; i < 3; i++) { pal[2][i] = (uint8_t)((pal[0][i] + pal[1][i]) / 2); pal[3][i] = 0; }
        pal[2][3] = 255; pal[3][3] = alpha0 ? 0 : 255;
    }
    for (y = 0; y < 4; y++)
        for (x = 0; x < 4; x++, idx >>= 2) memcpy(out + y * pitch + x * 4, pal[idx & 3], 4);
}

/* BC4 channel block (8 bytes) -> 16 values. unorm: 0..255. snorm: signed endpoints, output scaled by 35
 * (-4445..4445 for -1..1) so that the /7 and /5 interpolants stay exact, as the float decode in the GL spec. */
static void bc4_block(const uint8_t *b, int *out, int snorm) {
    int a0 = b[0], a1 = b[1], pal[8], i;
    uint64_t idx = rd64le(b) >> 16;
    if (snorm) { a0 = (int8_t)b[0]; a1 = (int8_t)b[1]; if (a0 < -127) a0 = -127; if (a1 < -127) a1 = -127; }
    pal[0] = snorm ? a0 * 35 : a0; pal[1] = snorm ? a1 * 35 : a1;
    if (a0 > a1) {
        for (i = 1; i <= 6; i++) pal[i + 1] = snorm ? ((7 - i) * a0 + i * a1) * 5 : ((7 - i) * a0 + i * a1 + 3) / 7;
    } else {
        for (i = 1; i <= 4; i++) pal[i + 1] = snorm ? ((5 - i) * a0 + i * a1) * 7 : ((5 - i) * a0 + i * a1 + 2) / 5;
        pal[6] = snorm ? -4445 : 0; pal[7] = snorm ? 4445 : 255;
    }
    for (i = 0; i < 16; i++, idx >>= 3) out[i] = pal[idx & 7];
}

static inline uint8_t snorm_to_u8(int v) { return (uint8_t)(((v + 4445) * 255 + 4445) / 8890); }   /* -4445..4445 (-1..1) -> 0..255, rounded */

/* ================================================================ BC6H / BC7 shared tables */
static const uint8_t k_part2[64][16] = {
    {0,0,1,1,0,0,1,1,0,0,1,1,0,0,1,1}, {0,0,0,1,0,0,0,1,0,0,0,1,0,0,0,1}, {0,1,1,1,0,1,1,1,0,1,1,1,0,1,1,1}, {0,0,0,1,0,0,1,1,0,0,1,1,0,1,1,1},
    {0,0,0,0,0,0,0,1,0,0,0,1,0,0,1,1}, {0,0,1,1,0,1,1,1,0,1,1,1,1,1,1,1}, {0,0,0,1,0,0,1,1,0,1,1,1,1,1,1,1}, {0,0,0,0,0,0,0,1,0,0,1,1,0,1,1,1},
    {0,0,0,0,0,0,0,0,0,0,0,1,0,0,1,1}, {0,0,1,1,0,1,1,1,1,1,1,1,1,1,1,1}, {0,0,0,0,0,0,0,1,0,1,1,1,1,1,1,1}, {0,0,0,0,0,0,0,0,0,0,0,1,0,1,1,1},
    {0,0,0,1,0,1,1,1,1,1,1,1,1,1,1,1}, {0,0,0,0,0,0,0,0,1,1,1,1,1,1,1,1}, {0,0,0,0,1,1,1,1,1,1,1,1,1,1,1,1}, {0,0,0,0,0,0,0,0,0,0,0,0,1,1,1,1},
    {0,0,0,0,1,0,0,0,1,1,1,0,1,1,1,1}, {0,1,1,1,0,0,0,1,0,0,0,0,0,0,0,0}, {0,0,0,0,0,0,0,0,1,0,0,0,1,1,1,0}, {0,1,1,1,0,0,1,1,0,0,0,1,0,0,0,0},
    {0,0,1,1,0,0,0,1,0,0,0,0,0,0,0,0}, {0,0,0,0,1,0,0,0,1,1,0,0,1,1,1,0}, {0,0,0,0,0,0,0,0,1,0,0,0,1,1,0,0}, {0,1,1,1,0,0,1,1,0,0,1,1,0,0,0,1},
    {0,0,1,1,0,0,0,1,0,0,0,1,0,0,0,0}, {0,0,0,0,1,0,0,0,1,0,0,0,1,1,0,0}, {0,1,1,0,0,1,1,0,0,1,1,0,0,1,1,0}, {0,0,1,1,0,1,1,0,0,1,1,0,1,1,0,0},
    {0,0,0,1,0,1,1,1,1,1,1,0,1,0,0,0}, {0,0,0,0,1,1,1,1,1,1,1,1,0,0,0,0}, {0,1,1,1,0,0,0,1,1,0,0,0,1,1,1,0}, {0,0,1,1,1,0,0,1,1,0,0,1,1,1,0,0},
    {0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1}, {0,0,0,0,1,1,1,1,0,0,0,0,1,1,1,1}, {0,1,0,1,1,0,1,0,0,1,0,1,1,0,1,0}, {0,0,1,1,0,0,1,1,1,1,0,0,1,1,0,0},
    {0,0,1,1,1,1,0,0,0,0,1,1,1,1,0,0}, {0,1,0,1,0,1,0,1,1,0,1,0,1,0,1,0}, {0,1,1,0,1,0,0,1,0,1,1,0,1,0,0,1}, {0,1,0,1,1,0,1,0,1,0,1,0,0,1,0,1},
    {0,1,1,1,0,0,1,1,1,1,0,0,1,1,1,0}, {0,0,0,1,0,0,1,1,1,1,0,0,1,0,0,0}, {0,0,1,1,0,0,1,0,0,1,0,0,1,1,0,0}, {0,0,1,1,1,0,1,1,1,1,0,1,1,1,0,0},
    {0,1,1,0,1,0,0,1,1,0,0,1,0,1,1,0}, {0,0,1,1,1,1,0,0,1,1,0,0,0,0,1,1}, {0,1,1,0,0,1,1,0,1,0,0,1,1,0,0,1}, {0,0,0,0,0,1,1,0,0,1,1,0,0,0,0,0},
    {0,1,0,0,1,1,1,0,0,1,0,0,0,0,0,0}, {0,0,1,0,0,1,1,1,0,0,1,0,0,0,0,0}, {0,0,0,0,0,0,1,0,0,1,1,1,0,0,1,0}, {0,0,0,0,0,1,0,0,1,1,1,0,0,1,0,0},
    {0,1,1,0,1,1,0,0,1,0,0,1,0,0,1,1}, {0,0,1,1,0,1,1,0,1,1,0,0,1,0,0,1}, {0,1,1,0,0,0,1,1,1,0,0,1,1,1,0,0}, {0,0,1,1,1,0,0,1,1,1,0,0,0,1,1,0},
    {0,1,1,0,1,1,0,0,1,1,0,0,1,0,0,1}, {0,1,1,0,0,0,1,1,0,0,1,1,1,0,0,1}, {0,1,1,1,1,1,1,0,1,0,0,0,0,0,0,1}, {0,0,0,1,1,0,0,0,1,1,1,0,0,1,1,1},
    {0,0,0,0,1,1,1,1,0,0,1,1,0,0,1,1}, {0,0,1,1,0,0,1,1,1,1,1,1,0,0,0,0}, {0,0,1,0,0,0,1,0,1,1,1,0,1,1,1,0}, {0,1,0,0,0,1,0,0,0,1,1,1,0,1,1,1}};
static const uint8_t k_part3[64][16] = {
    {0,0,1,1,0,0,1,1,0,2,2,1,2,2,2,2}, {0,0,0,1,0,0,1,1,2,2,1,1,2,2,2,1}, {0,0,0,0,2,0,0,1,2,2,1,1,2,2,1,1}, {0,2,2,2,0,0,2,2,0,0,1,1,0,1,1,1},
    {0,0,0,0,0,0,0,0,1,1,2,2,1,1,2,2}, {0,0,1,1,0,0,1,1,0,0,2,2,0,0,2,2}, {0,0,2,2,0,0,2,2,1,1,1,1,1,1,1,1}, {0,0,1,1,0,0,1,1,2,2,1,1,2,2,1,1},
    {0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2}, {0,0,0,0,1,1,1,1,1,1,1,1,2,2,2,2}, {0,0,0,0,1,1,1,1,2,2,2,2,2,2,2,2}, {0,0,1,2,0,0,1,2,0,0,1,2,0,0,1,2},
    {0,1,1,2,0,1,1,2,0,1,1,2,0,1,1,2}, {0,1,2,2,0,1,2,2,0,1,2,2,0,1,2,2}, {0,0,1,1,0,1,1,2,1,1,2,2,1,2,2,2}, {0,0,1,1,2,0,0,1,2,2,0,0,2,2,2,0},
    {0,0,0,1,0,0,1,1,0,1,1,2,1,1,2,2}, {0,1,1,1,0,0,1,1,2,0,0,1,2,2,0,0}, {0,0,0,0,1,1,2,2,1,1,2,2,1,1,2,2}, {0,0,2,2,0,0,2,2,0,0,2,2,1,1,1,1},
    {0,1,1,1,0,1,1,1,0,2,2,2,0,2,2,2}, {0,0,0,1,0,0,0,1,2,2,2,1,2,2,2,1}, {0,0,0,0,0,0,1,1,0,1,2,2,0,1,2,2}, {0,0,0,0,1,1,0,0,2,2,1,0,2,2,1,0},
    {0,1,2,2,0,1,2,2,0,0,1,1,0,0,0,0}, {0,0,1,2,0,0,1,2,1,1,2,2,2,2,2,2}, {0,1,1,0,1,2,2,1,1,2,2,1,0,1,1,0}, {0,0,0,0,0,1,1,0,1,2,2,1,1,2,2,1},
    {0,0,2,2,1,1,0,2,1,1,0,2,0,0,2,2}, {0,1,1,0,0,1,1,0,2,0,0,2,2,2,2,2}, {0,0,1,1,0,1,2,2,0,1,2,2,0,0,1,1}, {0,0,0,0,2,0,0,0,2,2,1,1,2,2,2,1},
    {0,0,0,0,0,0,0,2,1,1,2,2,1,2,2,2}, {0,2,2,2,0,0,2,2,0,0,1,2,0,0,1,1}, {0,0,1,1,0,0,1,2,0,0,2,2,0,2,2,2}, {0,1,2,0,0,1,2,0,0,1,2,0,0,1,2,0},
    {0,0,0,0,1,1,1,1,2,2,2,2,0,0,0,0}, {0,1,2,0,1,2,0,1,2,0,1,2,0,1,2,0}, {0,1,2,0,2,0,1,2,1,2,0,1,0,1,2,0}, {0,0,1,1,2,2,0,0,1,1,2,2,0,0,1,1},
    {0,0,1,1,1,1,2,2,2,2,0,0,0,0,1,1}, {0,1,0,1,0,1,0,1,2,2,2,2,2,2,2,2}, {0,0,0,0,0,0,0,0,2,1,2,1,2,1,2,1}, {0,0,2,2,1,1,2,2,0,0,2,2,1,1,2,2},
    {0,0,2,2,0,0,1,1,0,0,2,2,0,0,1,1}, {0,2,2,0,1,2,2,1,0,2,2,0,1,2,2,1}, {0,1,0,1,2,2,2,2,2,2,2,2,0,1,0,1}, {0,0,0,0,2,1,2,1,2,1,2,1,2,1,2,1},
    {0,1,0,1,0,1,0,1,0,1,0,1,2,2,2,2}, {0,2,2,2,0,1,1,1,0,2,2,2,0,1,1,1}, {0,0,0,2,1,1,1,2,0,0,0,2,1,1,1,2}, {0,0,0,0,2,1,1,2,2,1,1,2,2,1,1,2},
    {0,2,2,2,0,1,1,1,0,1,1,1,0,2,2,2}, {0,0,0,2,1,1,1,2,1,1,1,2,0,0,0,2}, {0,1,1,0,0,1,1,0,0,1,1,0,2,2,2,2}, {0,0,0,0,0,0,0,0,2,1,1,2,2,1,1,2},
    {0,1,1,0,0,1,1,0,2,2,2,2,2,2,2,2}, {0,0,2,2,0,0,1,1,0,0,1,1,0,0,2,2}, {0,0,2,2,1,1,2,2,1,1,2,2,0,0,2,2}, {0,0,0,0,0,0,0,0,0,0,0,0,2,1,1,2},
    {0,0,0,2,0,0,0,1,0,0,0,2,0,0,0,1}, {0,2,2,2,1,2,2,2,0,2,2,2,1,2,2,2}, {0,1,0,1,2,2,2,2,2,2,2,2,2,2,2,2}, {0,1,1,1,2,0,1,1,2,2,0,1,2,2,2,0}};
static const uint8_t k_anchor2[64] = {
    15,15,15,15,15,15,15,15,15,15,15,15,15,15,15,15, 15, 2, 8, 2, 2, 8, 8,15, 2, 8, 2, 2, 8, 8, 2, 2,
    15,15, 6, 8, 2, 8,15,15, 2, 8, 2, 2, 2,15,15, 6,  6, 2, 6, 8,15,15, 2, 2,15,15,15,15,15, 2, 2,15};
static const uint8_t k_anchor3a[64] = {
     3, 3,15,15, 8, 3,15,15, 8, 8, 6, 6, 6, 5, 3, 3,  3, 3, 8,15, 3, 3, 6,10, 5, 8, 8, 6, 8, 5,15,15,
     8,15, 3, 5, 6,10, 8,15,15, 3,15, 5,15,15,15,15,  3,15, 5, 5, 5, 8, 5,10, 5,10, 8,13,15,12, 3, 3};
static const uint8_t k_anchor3b[64] = {
    15, 8, 8, 3,15,15, 3, 8,15,15,15,15,15,15,15, 8, 15, 8,15, 3,15, 8,15, 8, 3,15, 6,10,15,15,10, 8,
    15, 3,15,10,10, 8, 9,10, 6,15, 8,15, 3, 6, 6, 8, 15, 3,15,15,15,15,15,15,15,15,15,15, 3,15,15, 8};
static const uint8_t k_w2[4] = {0, 21, 43, 64}, k_w3[8] = {0, 9, 18, 27, 37, 46, 55, 64}, k_w4[16] = {0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64};

/* n (0..32) bits at bit position pos of a 16-byte block, least significant first; bits past 128 read as zero */
static inline uint32_t bits128(const uint8_t *p, int pos, int n) {
    uint64_t v = 0;
    int byte = pos >> 3;
    if (n <= 0 || pos >= 128) return 0;
    if (byte <= 8) memcpy(&v, p + byte, 8); else memcpy(&v, p + byte, (size_t)(16 - byte));
    return (uint32_t)((v >> (pos & 7)) & (((uint64_t)1 << n) - 1));
}
typedef struct { const uint8_t *p; int pos; } BitRd;
static inline uint32_t br_get(BitRd *r, int n) {
    uint32_t v = bits128(r->p, r->pos, n);
    r->pos += n;
    return v;
}
static inline uint32_t br_get_rev(BitRd *r, int n) {   /* first stored bit becomes the most significant */
    uint32_t v = 0;
    int i;
    for (i = 0; i < n; i++, r->pos++) v |= (uint32_t)((r->p[r->pos >> 3] >> (r->pos & 7)) & 1) << (n - 1 - i);
    return v;
}

/* ================================================================ BC6H
 * Field sequences after the mode bits, from the public BC6H mode table: {endpoint, low bit, count, reversed}.
 * endpoint = 3 * index + channel (index 0..3 = r0 g0 b0 ... stored as r0,g0,b0,r1,g1,b1,r2,g2,b2,r3,g3,b3). */
#define F(e, lo, n) {e, lo, n, 0}
#define FR(e, lo, n) {e, lo, n, 1}
enum { R0 = 0, G0, B0, R1, G1, B1, R2, G2, B2, R3, G3, B3 };
typedef struct { uint8_t e, lo, n, rev; } Bc6Field;
static const Bc6Field k_bc6_m1[] = {F(G2,4,1), F(B2,4,1), F(B3,4,1), F(R0,0,10), F(G0,0,10), F(B0,0,10), F(R1,0,5), F(G3,4,1), F(G2,0,4), F(G1,0,5), F(B3,0,1), F(G3,0,4), F(B1,0,5), F(B3,1,1), F(B2,0,4), F(R2,0,5), F(B3,2,1), F(R3,0,5), F(B3,3,1)};
static const Bc6Field k_bc6_m2[] = {F(G2,5,1), F(G3,4,1), F(G3,5,1), F(R0,0,7), F(B3,0,1), F(B3,1,1), F(B2,4,1), F(G0,0,7), F(B2,5,1), F(B3,2,1), F(G2,4,1), F(B0,0,7), F(B3,3,1), F(B3,5,1), F(B3,4,1), F(R1,0,6), F(G2,0,4), F(G1,0,6), F(G3,0,4), F(B1,0,6), F(B2,0,4), F(R2,0,6), F(R3,0,6)};
static const Bc6Field k_bc6_m3[] = {F(R0,0,10), F(G0,0,10), F(B0,0,10), F(R1,0,5), F(R0,10,1), F(G2,0,4), F(G1,0,4), F(G0,10,1), F(B3,0,1), F(G3,0,4), F(B1,0,4), F(B0,10,1), F(B3,1,1), F(B2,0,4), F(R2,0,5), F(B3,2,1), F(R3,0,5), F(B3,3,1)};
static const Bc6Field k_bc6_m4[] = {F(R0,0,10), F(G0,0,10), F(B0,0,10), F(R1,0,4), F(R0,10,1), F(G3,4,1), F(G2,0,4), F(G1,0,5), F(G0,10,1), F(G3,0,4), F(B1,0,4), F(B0,10,1), F(B3,1,1), F(B2,0,4), F(R2,0,4), F(B3,0,1), F(B3,2,1), F(R3,0,4), F(G2,4,1), F(B3,3,1)};
static const Bc6Field k_bc6_m5[] = {F(R0,0,10), F(G0,0,10), F(B0,0,10), F(R1,0,4), F(R0,10,1), F(B2,4,1), F(G2,0,4), F(G1,0,4), F(G0,10,1), F(B3,0,1), F(G3,0,4), F(B1,0,5), F(B0,10,1), F(B2,0,4), F(R2,0,4), F(B3,1,1), F(B3,2,1), F(R3,0,4), F(B3,4,1), F(B3,3,1)};
static const Bc6Field k_bc6_m6[] = {F(R0,0,9), F(B2,4,1), F(G0,0,9), F(G2,4,1), F(B0,0,9), F(B3,4,1), F(R1,0,5), F(G3,4,1), F(G2,0,4), F(G1,0,5), F(B3,0,1), F(G3,0,4), F(B1,0,5), F(B3,1,1), F(B2,0,4), F(R2,0,5), F(B3,2,1), F(R3,0,5), F(B3,3,1)};
static const Bc6Field k_bc6_m7[] = {F(R0,0,8), F(G3,4,1), F(B2,4,1), F(G0,0,8), F(B3,2,1), F(G2,4,1), F(B0,0,8), F(B3,3,1), F(B3,4,1), F(R1,0,6), F(G2,0,4), F(G1,0,5), F(B3,0,1), F(G3,0,4), F(B1,0,5), F(B3,1,1), F(B2,0,4), F(R2,0,6), F(R3,0,6)};
static const Bc6Field k_bc6_m8[] = {F(R0,0,8), F(B3,0,1), F(B2,4,1), F(G0,0,8), F(G2,5,1), F(G2,4,1), F(B0,0,8), F(G3,5,1), F(B3,4,1), F(R1,0,5), F(G3,4,1), F(G2,0,4), F(G1,0,6), F(G3,0,4), F(B1,0,5), F(B3,1,1), F(B2,0,4), F(R2,0,5), F(B3,2,1), F(R3,0,5), F(B3,3,1)};
static const Bc6Field k_bc6_m9[] = {F(R0,0,8), F(B3,1,1), F(B2,4,1), F(G0,0,8), F(B2,5,1), F(G2,4,1), F(B0,0,8), F(B3,5,1), F(B3,4,1), F(R1,0,5), F(G3,4,1), F(G2,0,4), F(G1,0,5), F(B3,0,1), F(G3,0,4), F(B1,0,6), F(B2,0,4), F(R2,0,5), F(B3,2,1), F(R3,0,5), F(B3,3,1)};
static const Bc6Field k_bc6_m10[] = {F(R0,0,6), F(G3,4,1), F(B3,0,1), F(B3,1,1), F(B2,4,1), F(G0,0,6), F(G2,5,1), F(B2,5,1), F(B3,2,1), F(G2,4,1), F(B0,0,6), F(G3,5,1), F(B3,3,1), F(B3,5,1), F(B3,4,1), F(R1,0,6), F(G2,0,4), F(G1,0,6), F(G3,0,4), F(B1,0,6), F(B2,0,4), F(R2,0,6), F(R3,0,6)};
static const Bc6Field k_bc6_m11[] = {F(R0,0,10), F(G0,0,10), F(B0,0,10), F(R1,0,10), F(G1,0,10), F(B1,0,10)};
static const Bc6Field k_bc6_m12[] = {F(R0,0,10), F(G0,0,10), F(B0,0,10), F(R1,0,9), F(R0,10,1), F(G1,0,9), F(G0,10,1), F(B1,0,9), F(B0,10,1)};
static const Bc6Field k_bc6_m13[] = {F(R0,0,10), F(G0,0,10), F(B0,0,10), F(R1,0,8), FR(R0,10,2), F(G1,0,8), FR(G0,10,2), F(B1,0,8), FR(B0,10,2)};
static const Bc6Field k_bc6_m14[] = {F(R0,0,10), F(G0,0,10), F(B0,0,10), F(R1,0,4), FR(R0,10,6), F(G1,0,4), FR(G0,10,6), F(B1,0,4), FR(B0,10,6)};
#undef F
#undef FR
typedef struct { const Bc6Field *f; uint8_t nf, epb, dr, dg, db, subsets; } Bc6Mode;
#define M(t, epb, dr, dg, db, s) {t, (uint8_t)(sizeof t / sizeof t[0]), epb, dr, dg, db, s}
static const Bc6Mode k_bc6_modes[14] = {
    M(k_bc6_m1, 10, 5, 5, 5, 2), M(k_bc6_m2, 7, 6, 6, 6, 2), M(k_bc6_m3, 11, 5, 4, 4, 2), M(k_bc6_m4, 11, 4, 5, 4, 2), M(k_bc6_m5, 11, 4, 4, 5, 2),
    M(k_bc6_m6, 9, 5, 5, 5, 2), M(k_bc6_m7, 8, 6, 5, 5, 2), M(k_bc6_m8, 8, 5, 6, 5, 2), M(k_bc6_m9, 8, 5, 5, 6, 2), M(k_bc6_m10, 6, 0, 0, 0, 2),
    M(k_bc6_m11, 10, 0, 0, 0, 1), M(k_bc6_m12, 11, 9, 9, 9, 1), M(k_bc6_m13, 12, 8, 8, 8, 1), M(k_bc6_m14, 16, 4, 4, 4, 1)};
#undef M

static inline int sext(int v, int bits) { return (v << (32 - bits)) >> (32 - bits); }

static int bc6_unquant(int v, int epb, int is_signed) {
    if (is_signed) {
        int s = 0;
        if (epb >= 16) return v;
        if (v < 0) { s = 1; v = -v; }
        if (v == 0) v = 0;
        else if (v >= ((1 << (epb - 1)) - 1)) v = 0x7FFF;
        else v = ((v << 15) + 0x4000) >> (epb - 1);
        return s ? -v : v;
    }
    if (epb >= 15) return v;
    if (v == 0) return 0;
    if (v == (1 << epb) - 1) return 0xFFFF;
    return ((v << 16) + 0x8000) >> epb;
}

static uint16_t bc6_finish(int v, int is_signed) {
    if (is_signed) {
        int s = v < 0;
        if (s) v = -v;
        v = (v * 31) >> 5;
        return (uint16_t)(s ? 0x8000 | v : v);
    }
    return (uint16_t)((v * 31) >> 6);
}

/* 16 bytes -> out[16][3] floats (RGB) */
static void bc6h_block(const uint8_t *b, float *out, int is_signed) {
    BitRd r = {b, 0};
    int e[12], mode, i, partition = 0, subsets, epb;
    const Bc6Mode *m;
    uint32_t mb = br_get(&r, 2);
    memset(e, 0, sizeof e);
    if (mb < 2) mode = (int)mb;
    else {
        mb |= br_get(&r, 3) << 2;
        switch (mb) {
        case 2: mode = 2; break; case 6: mode = 3; break; case 10: mode = 4; break; case 14: mode = 5; break;
        case 18: mode = 6; break; case 22: mode = 7; break; case 26: mode = 8; break; case 30: mode = 9; break;
        case 3: mode = 10; break; case 7: mode = 11; break; case 11: mode = 12; break; case 15: mode = 13; break;
        default: for (i = 0; i < 48; i++) out[i] = 0.0f; return;      /* reserved: all zero */
        }
    }
    m = &k_bc6_modes[mode]; subsets = m->subsets; epb = m->epb;
    for (i = 0; i < m->nf; i++) {
        const Bc6Field *f = &m->f[i];
        e[f->e] |= (int)((f->rev ? br_get_rev(&r, f->n) : br_get(&r, f->n)) << f->lo);
    }
    if (subsets == 2) partition = (int)br_get(&r, 5);
    if (is_signed) { e[R0] = sext(e[R0], epb); e[G0] = sext(e[G0], epb); e[B0] = sext(e[B0], epb); }
    if (m->dr) {    /* delta modes */
        int mask = (1 << epb) - 1, k;
        for (k = 1; k < subsets * 2; k++) {
            e[k * 3 + 0] = sext(e[k * 3 + 0], m->dr); e[k * 3 + 1] = sext(e[k * 3 + 1], m->dg); e[k * 3 + 2] = sext(e[k * 3 + 2], m->db);
            for (i = 0; i < 3; i++) {
                int v = (e[k * 3 + i] + e[i]) & mask;
                e[k * 3 + i] = is_signed ? sext(v, epb) : v;
            }
        }
    } else if (is_signed) {
        for (i = 3; i < subsets * 6; i++) e[i] = sext(e[i], epb);
    }
    for (i = 0; i < subsets * 6; i++) e[i] = bc6_unquant(e[i], epb, is_signed);
    for (i = 0; i < 16; i++) {
        int s = subsets == 2 ? k_part2[partition][i] : 0, anchor = i == 0 || (subsets == 2 && i == k_anchor2[partition]), w, c;
        if (subsets == 2) w = k_w3[br_get(&r, anchor ? 2 : 3)];
        else w = k_w4[br_get(&r, anchor ? 3 : 4)];
        for (c = 0; c < 3; c++) {
            int a = e[s * 6 + c], bb = e[s * 6 + 3 + c], v = (a * (64 - w) + bb * w + 32) >> 6;
            out[i * 3 + c] = tex_half_to_float(bc6_finish(v, is_signed));
        }
    }
}

/* ================================================================ BC7 */
typedef struct { uint8_t subsets, pbits, rbits, isb, cb, ab, epb, spb, ib, ib2; } Bc7Mode;   /* partition bits, rotation bits, index-selection bit, colour/alpha bits, per-endpoint / shared p-bits, index bits */
static const Bc7Mode k_bc7_modes[8] = {
    {3, 4, 0, 0, 4, 0, 1, 0, 3, 0}, {2, 6, 0, 0, 6, 0, 0, 1, 3, 0}, {3, 6, 0, 0, 5, 0, 0, 0, 2, 0}, {2, 6, 0, 0, 7, 0, 1, 0, 2, 0},
    {1, 0, 2, 1, 5, 6, 0, 0, 2, 3}, {1, 0, 2, 0, 7, 8, 0, 0, 2, 2}, {1, 0, 0, 0, 7, 7, 1, 0, 4, 0}, {2, 6, 0, 0, 5, 5, 1, 0, 2, 0}};

int tex_bc7_p56_anchor = 6;      /* see tex_decode.h */
static inline int expand8(int v, int bits) { v <<= (8 - bits); return v | (v >> bits); }

static void bc7_block(const uint8_t *b, uint8_t *out, int pitch) {
    BitRd r = {b, 0};
    int mode = 0, i, c, s, partition = 0, rot = 0, isb = 0, ne;
    int ep[6][4];            /* endpoints: subset * 2 + which, RGBA */
    int idx[16], idx2[16];
    const Bc7Mode *m;
    while (mode < 8 && !br_get(&r, 1)) mode++;
    if (mode == 8) { for (i = 0; i < 4; i++) memset(out + i * pitch, 0, 16); return; }     /* reserved */
    m = &k_bc7_modes[mode]; ne = m->subsets * 2;
    if (m->pbits) partition = (int)br_get(&r, m->pbits);
    if (m->rbits) rot = (int)br_get(&r, m->rbits);
    if (m->isb) isb = (int)br_get(&r, 1);
    for (c = 0; c < 3; c++) for (i = 0; i < ne; i++) ep[i][c] = (int)br_get(&r, m->cb);
    if (m->ab) for (i = 0; i < ne; i++) ep[i][3] = (int)br_get(&r, m->ab); else for (i = 0; i < ne; i++) ep[i][3] = 255;
    if (m->epb) {
        for (i = 0; i < ne; i++) {
            int p = (int)br_get(&r, 1);
            for (c = 0; c < 3; c++) ep[i][c] = ep[i][c] << 1 | p;
            if (m->ab) ep[i][3] = ep[i][3] << 1 | p;
        }
    } else if (m->spb) {
        for (s = 0; s < m->subsets; s++) {
            int p = (int)br_get(&r, 1);
            for (i = 0; i < 2; i++) { for (c = 0; c < 3; c++) ep[s * 2 + i][c] = ep[s * 2 + i][c] << 1 | p; if (m->ab) ep[s * 2 + i][3] = ep[s * 2 + i][3] << 1 | p; }
        }
    }
    for (i = 0; i < ne; i++) {
        for (c = 0; c < 3; c++) ep[i][c] = expand8(ep[i][c], m->cb + (m->epb || m->spb));
        if (m->ab) ep[i][3] = expand8(ep[i][3], m->ab + (m->epb || m->spb));
    }
    for (i = 0; i < 16; i++) {
        int anchor = i == 0;
        if (m->subsets == 2 && i == k_anchor2[partition]) anchor = 1;
        if (m->subsets == 3 && (i == (partition == 56 ? tex_bc7_p56_anchor : k_anchor3a[partition]) || i == k_anchor3b[partition])) anchor = 1;
        idx[i] = (int)br_get(&r, m->ib - anchor);
    }
    if (m->ib2) for (i = 0; i < 16; i++) idx2[i] = (int)br_get(&r, m->ib2 - (i == 0));
    for (i = 0; i < 16; i++) {
        int sub = m->subsets == 2 ? k_part2[partition][i] : m->subsets == 3 ? k_part3[partition][i] : 0, px[4], wc, wa;
        const int *e0 = ep[sub * 2], *e1 = ep[sub * 2 + 1];
        int wcol = m->ib == 2 ? k_w2[idx[i]] : m->ib == 3 ? k_w3[idx[i]] : k_w4[idx[i]];
        int walp = wcol;
        if (m->ib2) walp = m->ib2 == 2 ? k_w2[idx2[i]] : k_w3[idx2[i]];
        if (isb) { int t = wcol; wcol = walp; walp = t; }
        wc = wcol; wa = walp;
        for (c = 0; c < 3; c++) px[c] = (e0[c] * (64 - wc) + e1[c] * wc + 32) >> 6;
        px[3] = (e0[3] * (64 - wa) + e1[3] * wa + 32) >> 6;
        if (rot) { int t = px[3]; px[3] = px[rot - 1]; px[rot - 1] = t; }
        out[(i >> 2) * pitch + (i & 3) * 4 + 0] = (uint8_t)px[0]; out[(i >> 2) * pitch + (i & 3) * 4 + 1] = (uint8_t)px[1];
        out[(i >> 2) * pitch + (i & 3) * 4 + 2] = (uint8_t)px[2]; out[(i >> 2) * pitch + (i & 3) * 4 + 3] = (uint8_t)px[3];
    }
}

/* ================================================================ ASTC (LDR, 2D)
 * Khronos Data Format Specification, chapter "ASTC Compressed Texture Image Format". */
typedef struct { uint8_t bits, trits, quints; } AstcEnc;     /* bounded integer sequence encoding of one quantisation level */
static const uint16_t k_astc_levels[21] = {2, 3, 4, 5, 6, 8, 10, 12, 16, 20, 24, 32, 40, 48, 64, 80, 96, 128, 160, 192, 256};

static AstcEnc astc_enc(int level) {
    AstcEnc e = {0, 0, 0};
    int b = 0, l = level;
    if (level % 3 == 0 && ((level / 3) & (level / 3 - 1)) == 0) { e.trits = 1; l = level / 3; }
    else if (level % 5 == 0 && ((level / 5) & (level / 5 - 1)) == 0) { e.quints = 1; l = level / 5; }
    while (l > 1) { l >>= 1; b++; }
    e.bits = (uint8_t)b;
    return e;
}
static int astc_bit_cost(AstcEnc e, int n) {
    return e.bits * n + (e.trits ? (n * 8 + 4) / 5 : e.quints ? (n * 7 + 2) / 3 : 0);
}

typedef struct { const uint8_t *p; int pos, end; } AstcRd;        /* bits beyond `end` read as zero */
static inline uint32_t ar_get(AstcRd *r, int n) {
    int avail = r->end - r->pos;
    uint32_t v = bits128(r->p, r->pos, n < avail ? n : avail);
    r->pos += n;
    return v;
}

/* Decode n integers; out_m = bit part, out_tq = trit / quint part (0 when neither). */
/* Test aid: 1 = read whole trit / quint groups even when the sequence ends inside one, i.e. let bits after
 * the end of the colour sequence influence the last values. That is what the reference decoder used by
 * tex_test astccmp does; the specification (and this decoder by default) takes absent bits as zero. */
int tex_astc_test_full_bise;
static void astc_bise(AstcRd *r, AstcEnc e, int n, uint8_t *out_m, uint8_t *out_tq) {
    int i = 0;
    if (e.trits) {
        while (i < n) {
            uint32_t m[5], T, C, t[5];
            int k, cnt = n - i < 5 ? n - i : 5, rd = tex_astc_test_full_bise ? 5 : cnt;
            memset(m, 0, sizeof m); T = 0;
            m[0] = ar_get(r, e.bits); T = ar_get(r, 2);
            if (rd > 1) { m[1] = ar_get(r, e.bits); T |= ar_get(r, 2) << 2; }
            if (rd > 2) { m[2] = ar_get(r, e.bits); T |= ar_get(r, 1) << 4; }
            if (rd > 3) { m[3] = ar_get(r, e.bits); T |= ar_get(r, 2) << 5; }
            if (rd > 4) { m[4] = ar_get(r, e.bits); T |= ar_get(r, 1) << 7; }
            if (((T >> 2) & 7) == 7) { C = (((T >> 5) & 7) << 2) | (T & 3); t[4] = t[3] = 2; }
            else {
                C = T & 0x1F;
                if (((T >> 5) & 3) == 3) { t[4] = 2; t[3] = (T >> 7) & 1; }
                else { t[4] = (T >> 7) & 1; t[3] = (T >> 5) & 3; }
            }
            if ((C & 3) == 3) { t[2] = 2; t[1] = (C >> 4) & 1; t[0] = (((C >> 3) & 1) << 1) | (((C >> 2) & 1) & ~((C >> 3) & 1)); }
            else if (((C >> 2) & 3) == 3) { t[2] = 2; t[1] = 2; t[0] = C & 3; }
            else { t[2] = (C >> 4) & 1; t[1] = (C >> 2) & 3; t[0] = (((C >> 1) & 1) << 1) | ((C & 1) & ~((C >> 1) & 1)); }
            for (k = 0; k < cnt; k++, i++) { out_m[i] = (uint8_t)m[k]; out_tq[i] = (uint8_t)t[k]; }
        }
    } else if (e.quints) {
        while (i < n) {
            uint32_t m[3], Q, C, q[3];
            int k, cnt = n - i < 3 ? n - i : 3, rd = tex_astc_test_full_bise ? 3 : cnt;
            memset(m, 0, sizeof m);
            m[0] = ar_get(r, e.bits); Q = ar_get(r, 3);
            if (rd > 1) { m[1] = ar_get(r, e.bits); Q |= ar_get(r, 2) << 3; }
            if (rd > 2) { m[2] = ar_get(r, e.bits); Q |= ar_get(r, 2) << 5; }
            if (((Q >> 1) & 3) == 3 && ((Q >> 5) & 3) == 0) {
                q[0] = q[1] = 4;
                q[2] = ((Q & 1) << 2) | ((((Q >> 4) & 1) & ~(Q & 1)) << 1) | (((Q >> 3) & 1) & ~(Q & 1));
            } else {
                if (((Q >> 1) & 3) == 3) { q[2] = 4; C = (((Q >> 3) & 3) << 3) | ((~(Q >> 5) & 3) << 1) | (Q & 1); }
                else { q[2] = (Q >> 5) & 3; C = Q & 0x1F; }
                if ((C & 7) == 5) { q[1] = 4; q[0] = (C >> 3) & 3; }
                else { q[1] = (C >> 3) & 3; q[0] = C & 7; }
            }
            for (k = 0; k < cnt; k++, i++) { out_m[i] = (uint8_t)m[k]; out_tq[i] = (uint8_t)q[k]; }
        }
    } else {
        for (; i < n; i++) { out_m[i] = (uint8_t)ar_get(r, e.bits); out_tq[i] = 0; }
    }
}

/* replicate a b-bit value to 8 bits (b = 1..8) */
static inline int rep8(int v, int b) {
    int r = v << (8 - b);
    switch (b) { case 1: return v ? 255 : 0; case 2: return r | (r >> 2) | (r >> 4) | (r >> 6); case 3: return r | (r >> 3) | (r >> 6); default: return r | (r >> b); }
}

/* colour integer -> 0..255 */
static int astc_unquant_color(AstcEnc e, int m, int tq) {
    int A = (m & 1) ? 0x1FF : 0, B = 0, C = 0, T;
    if (!e.trits && !e.quints) return e.bits ? rep8(m, e.bits) : 0;
    if (e.trits) {
        switch (e.bits) {
        case 1: C = 204; break;
        case 2: { int b = (m >> 1) & 1; C = 93; B = (b << 8) | (b << 4) | (b << 2) | (b << 1); } break;
        case 3: { int cb = (m >> 1) & 3; C = 44; B = (cb << 7) | (cb << 2) | cb; } break;
        case 4: { int d = (m >> 1) & 7; C = 22; B = (d << 6) | d; } break;
        case 5: { int d = (m >> 1) & 15; C = 11; B = (d << 5) | (d >> 2); } break;
        case 6: { int d = (m >> 1) & 31; C = 5; B = (d << 4) | (d >> 4); } break;
        default: return 0;
        }
    } else {
        switch (e.bits) {
        case 1: C = 113; break;
        case 2: { int b = (m >> 1) & 1; C = 54; B = (b << 8) | (b << 3) | (b << 2); } break;
        case 3: { int cb = (m >> 1) & 3; C = 26; B = (cb << 7) | (cb << 1) | (cb >> 1); } break;
        case 4: { int d = (m >> 1) & 7; C = 13; B = (d << 6) | (d >> 1); } break;
        case 5: { int d = (m >> 1) & 15; C = 6; B = (d << 5) | (d >> 3); } break;
        default: return 0;
        }
    }
    T = tq * C + B;
    T ^= A;
    T = (A & 0x80) | (T >> 2);
    return T;
}

/* weight integer -> 0..64 */
static int astc_unquant_weight(AstcEnc e, int m, int tq) {
    int A = (m & 1) ? 0x7F : 0, B = 0, C = 0, T;
    if (!e.trits && !e.quints) {
        int b = e.bits, v = m << (6 - b);
        switch (b) { case 1: T = m ? 63 : 0; break; case 2: T = v | (v >> 2) | (v >> 4); break; case 3: T = v | (v >> 3); break; case 4: T = v | (v >> 4); break; case 5: T = v | (v >> 5); break; default: T = 0; }
    } else if (e.trits) {
        switch (e.bits) {
        case 0: T = (int[3]){0, 32, 63}[tq]; return T > 32 ? T + 1 : T;
        case 1: C = 50; break;
        case 2: { int b = (m >> 1) & 1; C = 23; B = (b << 6) | (b << 2) | b; } break;
        case 3: { int cb = (m >> 1) & 3; C = 11; B = (cb << 5) | cb; } break;
        default: return 0;
        }
        T = tq * C + B; T ^= A; T = (A & 0x20) | (T >> 2);
    } else {
        switch (e.bits) {
        case 0: T = (int[5]){0, 16, 32, 47, 63}[tq]; return T > 32 ? T + 1 : T;
        case 1: C = 28; break;
        case 2: { int b = (m >> 1) & 1; C = 13; B = (b << 6) | (b << 1); } break;
        default: return 0;
        }
        T = tq * C + B; T ^= A; T = (A & 0x20) | (T >> 2);
    }
    return T > 32 ? T + 1 : T;
}

static uint32_t astc_hash52(uint32_t p) {
    p ^= p >> 15; p -= p << 17; p += p << 7; p += p << 4; p ^= p >> 5; p += p << 16; p ^= p >> 7; p ^= p >> 3; p ^= p << 6; p ^= p >> 17;
    return p;
}

/* The partition function is four linear forms of (x, y); the coefficients depend only on the block. */
typedef struct { int kx[4], ky[4], add[4], count, shift; } AstcPart;
static void astc_partition_setup(AstcPart *P, int seed, int count, int small_block) {
    uint32_t rnum;
    int s1, s2, s3, s4, s5, s6, s7, s8, sh1, sh2;
    P->count = count; P->shift = small_block ? 1 : 0;
    if (count == 1) return;
    seed += (count - 1) * 1024;
    rnum = astc_hash52((uint32_t)seed);
    s1 = rnum & 15; s2 = (rnum >> 4) & 15; s3 = (rnum >> 8) & 15; s4 = (rnum >> 12) & 15; s5 = (rnum >> 16) & 15; s6 = (rnum >> 20) & 15;
    s7 = (rnum >> 24) & 15; s8 = (rnum >> 28) & 15;
    s1 *= s1; s2 *= s2; s3 *= s3; s4 *= s4; s5 *= s5; s6 *= s6; s7 *= s7; s8 *= s8;
    if (seed & 1) { sh1 = (seed & 2) ? 4 : 5; sh2 = count == 3 ? 6 : 5; } else { sh1 = count == 3 ? 6 : 5; sh2 = (seed & 2) ? 4 : 5; }
    P->kx[0] = s1 >> sh1; P->ky[0] = s2 >> sh2; P->kx[1] = s3 >> sh1; P->ky[1] = s4 >> sh2;
    P->kx[2] = s5 >> sh1; P->ky[2] = s6 >> sh2; P->kx[3] = s7 >> sh1; P->ky[3] = s8 >> sh2;
    P->add[0] = (int)(rnum >> 14); P->add[1] = (int)(rnum >> 10); P->add[2] = (int)(rnum >> 6); P->add[3] = (int)(rnum >> 2);
}
static inline int astc_partition_of(const AstcPart *P, int x, int y) {
    int a, b, c, d;
    if (P->count == 1) return 0;
    x <<= P->shift; y <<= P->shift;
    a = (P->kx[0] * x + P->ky[0] * y + P->add[0]) & 0x3F;
    b = (P->kx[1] * x + P->ky[1] * y + P->add[1]) & 0x3F;
    c = P->count < 3 ? 0 : (P->kx[2] * x + P->ky[2] * y + P->add[2]) & 0x3F;
    d = P->count < 4 ? 0 : (P->kx[3] * x + P->ky[3] * y + P->add[3]) & 0x3F;
    if (a >= b && a >= c && a >= d) return 0;
    if (b >= c && b >= d) return 1;
    if (c >= d) return 2;
    return 3;
}
/* The same function written as in the specification (2D: z = 0); tex_test checks the two agree. */
int tex_astc_partition_reference(int seed, int x, int y, int count, int small_block) {
    uint32_t rnum;
    int s1, s2, s3, s4, s5, s6, s7, s8, s9, s10, s11, s12, sh1, sh2, sh3, a, b, c, d, z = 0;
    if (count == 1) return 0;
    if (small_block) { x <<= 1; y <<= 1; }
    seed += (count - 1) * 1024;
    rnum = astc_hash52((uint32_t)seed);
    s1 = rnum & 15; s2 = (rnum >> 4) & 15; s3 = (rnum >> 8) & 15; s4 = (rnum >> 12) & 15; s5 = (rnum >> 16) & 15; s6 = (rnum >> 20) & 15;
    s7 = (rnum >> 24) & 15; s8 = (rnum >> 28) & 15; s9 = (rnum >> 18) & 15; s10 = (rnum >> 22) & 15; s11 = (rnum >> 26) & 15; s12 = ((rnum >> 30) | (rnum << 2)) & 15;
    s1 *= s1; s2 *= s2; s3 *= s3; s4 *= s4; s5 *= s5; s6 *= s6; s7 *= s7; s8 *= s8; s9 *= s9; s10 *= s10; s11 *= s11; s12 *= s12;
    if (seed & 1) { sh1 = (seed & 2) ? 4 : 5; sh2 = count == 3 ? 6 : 5; } else { sh1 = count == 3 ? 6 : 5; sh2 = (seed & 2) ? 4 : 5; }
    sh3 = (seed & 0x10) ? sh1 : sh2;
    s1 >>= sh1; s2 >>= sh2; s3 >>= sh1; s4 >>= sh2; s5 >>= sh1; s6 >>= sh2; s7 >>= sh1; s8 >>= sh2; s9 >>= sh3; s10 >>= sh3; s11 >>= sh3; s12 >>= sh3;
    a = (s1 * x + s2 * y + s11 * z + (int)(rnum >> 14)) & 0x3F;
    b = (s3 * x + s4 * y + s12 * z + (int)(rnum >> 10)) & 0x3F;
    c = (s5 * x + s6 * y + s9 * z + (int)(rnum >> 6)) & 0x3F;
    d = (s7 * x + s8 * y + s10 * z + (int)(rnum >> 2)) & 0x3F;
    if (count < 4) d = 0;
    if (count < 3) c = 0;
    if (a >= b && a >= c && a >= d) return 0;
    if (b >= c && b >= d) return 1;
    if (c >= d) return 2;
    return 3;
}

static inline int clamp255(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }
static void bit_transfer_signed(int *a, int *b) {
    *b >>= 1; *b |= *a & 0x80; *a >>= 1; *a &= 0x3F; if (*a & 0x20) *a -= 0x40;
}
static void blue_contract(int *e) { e[0] = (e[0] + e[2]) >> 1; e[1] = (e[1] + e[2]) >> 1; }

/* LDR colour endpoint modes -> e0[4], e1[4] in 0..255. Returns 0, or -1 for HDR modes. */
static int astc_endpoints(int cem, const int *v, int *e0, int *e1) {
    int i;
    switch (cem) {
    case 0: e0[0] = e0[1] = e0[2] = v[0]; e1[0] = e1[1] = e1[2] = v[1]; e0[3] = e1[3] = 255; break;
    case 1: { int l0 = (v[0] >> 2) | (v[1] & 0xC0), l1 = l0 + (v[1] & 0x3F); if (l1 > 255) l1 = 255;
              e0[0] = e0[1] = e0[2] = l0; e1[0] = e1[1] = e1[2] = l1; e0[3] = e1[3] = 255; } break;
    case 4: e0[0] = e0[1] = e0[2] = v[0]; e0[3] = v[2]; e1[0] = e1[1] = e1[2] = v[1]; e1[3] = v[3]; break;
    case 5: { int a[4] = {v[0], v[1], v[2], v[3]}; bit_transfer_signed(&a[1], &a[0]); bit_transfer_signed(&a[3], &a[2]);
              e0[0] = e0[1] = e0[2] = clamp255(a[0]); e0[3] = clamp255(a[2]); e1[0] = e1[1] = e1[2] = clamp255(a[0] + a[1]); e1[3] = clamp255(a[2] + a[3]); } break;
    case 6: e0[0] = v[0] * v[3] >> 8; e0[1] = v[1] * v[3] >> 8; e0[2] = v[2] * v[3] >> 8; e0[3] = 255; e1[0] = v[0]; e1[1] = v[1]; e1[2] = v[2]; e1[3] = 255; break;
    case 8:
        if (v[1] + v[3] + v[5] >= v[0] + v[2] + v[4]) { e0[0] = v[0]; e0[1] = v[2]; e0[2] = v[4]; e1[0] = v[1]; e1[1] = v[3]; e1[2] = v[5]; }
        else { e0[0] = v[1]; e0[1] = v[3]; e0[2] = v[5]; e1[0] = v[0]; e1[1] = v[2]; e1[2] = v[4]; blue_contract(e0); blue_contract(e1); }
        e0[3] = e1[3] = 255; break;
    case 9: { int a[6] = {v[0], v[1], v[2], v[3], v[4], v[5]};
              bit_transfer_signed(&a[1], &a[0]); bit_transfer_signed(&a[3], &a[2]); bit_transfer_signed(&a[5], &a[4]);
              if (a[1] + a[3] + a[5] >= 0) { e0[0] = a[0]; e0[1] = a[2]; e0[2] = a[4]; e1[0] = a[0] + a[1]; e1[1] = a[2] + a[3]; e1[2] = a[4] + a[5]; }
              else { e0[0] = a[0] + a[1]; e0[1] = a[2] + a[3]; e0[2] = a[4] + a[5]; e1[0] = a[0]; e1[1] = a[2]; e1[2] = a[4]; blue_contract(e0); blue_contract(e1); }
              for (i = 0; i < 3; i++) { e0[i] = clamp255(e0[i]); e1[i] = clamp255(e1[i]); }
              e0[3] = e1[3] = 255; } break;
    case 10: e0[0] = v[0] * v[3] >> 8; e0[1] = v[1] * v[3] >> 8; e0[2] = v[2] * v[3] >> 8; e0[3] = v[4]; e1[0] = v[0]; e1[1] = v[1]; e1[2] = v[2]; e1[3] = v[5]; break;
    case 12:
        if (v[1] + v[3] + v[5] >= v[0] + v[2] + v[4]) { e0[0] = v[0]; e0[1] = v[2]; e0[2] = v[4]; e0[3] = v[6]; e1[0] = v[1]; e1[1] = v[3]; e1[2] = v[5]; e1[3] = v[7]; }
        else { e0[0] = v[1]; e0[1] = v[3]; e0[2] = v[5]; e0[3] = v[7]; e1[0] = v[0]; e1[1] = v[2]; e1[2] = v[4]; e1[3] = v[6]; blue_contract(e0); blue_contract(e1); }
        break;
    case 13: { int a[8] = {v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7]};
              bit_transfer_signed(&a[1], &a[0]); bit_transfer_signed(&a[3], &a[2]); bit_transfer_signed(&a[5], &a[4]); bit_transfer_signed(&a[7], &a[6]);
              if (a[1] + a[3] + a[5] >= 0) { e0[0] = a[0]; e0[1] = a[2]; e0[2] = a[4]; e0[3] = a[6]; e1[0] = a[0] + a[1]; e1[1] = a[2] + a[3]; e1[2] = a[4] + a[5]; e1[3] = a[6] + a[7]; }
              else { e0[0] = a[0] + a[1]; e0[1] = a[2] + a[3]; e0[2] = a[4] + a[5]; e0[3] = a[6] + a[7]; e1[0] = a[0]; e1[1] = a[2]; e1[2] = a[4]; e1[3] = a[6]; blue_contract(e0); blue_contract(e1); }
              for (i = 0; i < 4; i++) { e0[i] = clamp255(e0[i]); e1[i] = clamp255(e1[i]); }
            } break;
    default: return -1;    /* 2, 3, 7, 11, 14, 15: HDR */
    }
    return 0;
}

static void astc_fill(uint8_t *out, int pitch, int bw, int bh, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    int x, y;
    for (y = 0; y < bh; y++) for (x = 0; x < bw; x++) { uint8_t *p = out + y * pitch + x * 4; p[0] = r; p[1] = g; p[2] = b; p[3] = a; }
}

static void astc_block(const uint8_t *blk, uint8_t *out, int pitch, int bw, int bh, int srgb) {
    AstcRd r = {blk, 0, 128};
    uint32_t mode = ar_get(&r, 11);
    int gw, gh, dual = 0, high, R, layout, wlevel, nparts, part_index = 0, cem[4], base_cem = 0, nweights, wbits, extra = 0, ccs = 0, color_start, color_bits;
    int nvals = 0, i, p, cidx, clevel = -1;
    uint8_t cm[18], ctq[18], wm[128], wtq[128], cbuf[16], wbuf[16];
    int cv[18], e0[4][4], e1[4][4], wq[2][144], wt[2][144], is_hdr = 0;
    AstcEnc cenc = {0, 0, 0}, wenc;
    AstcRd cr, wr;
    AstcPart parts;
    int Ds, Dt, x, y, small = bw * bh < 31;

    if ((mode & 0x1FF) == 0x1FC) {                       /* void extent */
        int hdr = (mode >> 9) & 1;
        if (!(mode & 0x400) || !ar_get(&r, 1)) { tex_astc_bad_blocks++; astc_fill(out, pitch, bw, bh, 255, 0, 255, 255); return; }
        ar_get(&r, 13); ar_get(&r, 13); ar_get(&r, 13); ar_get(&r, 13);
        {
            uint32_t c[4];
            uint8_t px[4];
            for (i = 0; i < 4; i++) c[i] = ar_get(&r, 16);
            if (hdr) {
                tex_astc_hdr_blocks++;
                for (i = 0; i < 4; i++) { float f = tex_half_to_float((uint16_t)c[i]); px[i] = (uint8_t)(f <= 0 ? 0 : f >= 1 ? 255 : (int)(f * 255.0f + 0.5f)); }
            } else for (i = 0; i < 4; i++) px[i] = (uint8_t)(c[i] >> 8);
            astc_fill(out, pitch, bw, bh, px[0], px[1], px[2], px[3]);
        }
        return;
    }
    /* block mode (Table "block mode layout") */
    if ((mode & 0xF) == 0 || ((mode & 3) == 0 && (mode & 0x1C0) == 0x1C0)) goto bad;
    if (mode & 3) {
        if (mode & 8) { if (mode & 4) layout = (mode & 0x100) ? 4 : 3; else layout = 2; }
        else layout = (mode & 4) ? 1 : 0;
    } else {
        if (mode & 0x100) { if (mode & 0x80) layout = (mode & 0x20) ? 8 : 7; else layout = 9; }
        else layout = (mode & 0x80) ? 6 : 5;
    }
    R = (int)((mode >> 4) & 1);
    if (layout < 5) R |= (int)(mode & 3) << 1; else R |= (int)(mode & 0xC) >> 1;
    {
        int A = (int)(mode >> 5) & 3, B = (int)(mode >> 7) & 3;
        switch (layout) {
        case 0: gw = B + 4; gh = A + 2; break;
        case 1: gw = B + 8; gh = A + 2; break;
        case 2: gw = A + 2; gh = B + 8; break;
        case 3: gw = A + 2; gh = (B & 1) + 6; break;
        case 4: gw = (B & 1) + 2; gh = A + 2; break;
        case 5: gw = 12; gh = A + 2; break;
        case 6: gw = A + 2; gh = 12; break;
        case 7: gw = 6; gh = 10; break;
        case 8: gw = 10; gh = 6; break;
        default: gw = A + 6; gh = (int)((mode >> 9) & 3) + 6; break;
        }
    }
    dual = layout != 9 && (mode & 0x400) != 0;
    high = layout != 9 && (mode & 0x200) != 0;
    {
        static const uint8_t lo[6] = {2, 3, 4, 5, 6, 8}, hi[6] = {10, 12, 16, 20, 24, 32};
        wlevel = high ? hi[R - 2] : lo[R - 2];
    }
    if (gw > bw || gh > bh) goto bad;
    nweights = gw * gh * (dual ? 2 : 1);
    wenc = astc_enc(wlevel);
    wbits = astc_bit_cost(wenc, nweights);
    if (nweights > 64 || wbits < 24 || wbits > 96) goto bad;
    nparts = (int)ar_get(&r, 2) + 1;
    if (nparts == 4 && dual) goto bad;
    if (nparts == 1) { cem[0] = (int)ar_get(&r, 4); color_start = 17; }
    else { part_index = (int)ar_get(&r, 10); base_cem = (int)ar_get(&r, 6); color_start = 29; }
    if (nparts > 1 && (base_cem & 3)) extra = 3 * nparts - 4;
    color_bits = 128 - wbits - extra - (dual ? 2 : 0) - color_start;
    if (color_bits < 0) goto bad;
    /* config bits just below the weights: [extra CEM bits][CCS] */
    {
        AstcRd c2 = {blk, 128 - wbits - extra, 128};
        if (nparts > 1) {
            if (base_cem & 3) {
                int bits = base_cem | ((int)ar_get(&c2, extra) << 6), cls = (base_cem & 3) - 1, C[4], Mv[4];
                bits >>= 2;
                for (i = 0; i < nparts; i++) { C[i] = bits & 1; bits >>= 1; }
                for (i = 0; i < nparts; i++) { Mv[i] = bits & 3; bits >>= 2; }
                for (i = 0; i < nparts; i++) cem[i] = ((cls + C[i]) << 2) | Mv[i];
            } else for (i = 0; i < nparts; i++) cem[i] = base_cem >> 2;
        }
        if (dual) { AstcRd c3 = {blk, 128 - wbits - extra - 2, 128}; ccs = (int)ar_get(&c3, 2); }
    }
    for (i = 0; i < nparts; i++) nvals += ((cem[i] >> 2) + 1) * 2;
    if (nvals > 18) goto bad;
    for (i = 20; i >= 4; i--) {     /* largest quantisation (>= 6 levels) whose encoding fits */
        AstcEnc e = astc_enc(k_astc_levels[i]);
        if (astc_bit_cost(e, nvals) <= color_bits) { clevel = i; cenc = e; break; }
    }
    if (clevel < 0) goto bad;
    /* colour integers: copy the colour bits to a zeroed buffer so that a partial last block reads zeros */
    memset(cbuf, 0, sizeof cbuf);
    for (i = 0; i < color_bits; i += 32) {
        uint32_t v = bits128(blk, color_start + i, color_bits - i < 32 ? color_bits - i : 32);
        cbuf[i >> 3] = (uint8_t)v; cbuf[(i >> 3) + 1] = (uint8_t)(v >> 8); cbuf[(i >> 3) + 2] = (uint8_t)(v >> 16); cbuf[(i >> 3) + 3] = (uint8_t)(v >> 24);
    }
    cr.p = cbuf; cr.pos = 0; cr.end = color_bits;
    astc_bise(&cr, cenc, nvals, cm, ctq);
    for (i = 0; i < nvals; i++) cv[i] = astc_unquant_color(cenc, cm[i], ctq[i]);
    for (i = 0, cidx = 0; i < nparts; i++) {
        if (astc_endpoints(cem[i], cv + cidx, e0[i], e1[i]) < 0) is_hdr = 1;
        cidx += ((cem[i] >> 2) + 1) * 2;
    }
    if (is_hdr) { tex_astc_hdr_blocks++; astc_fill(out, pitch, bw, bh, 255, 0, 255, 255); return; }
    /* weights: stored bit-reversed from the top of the block */
    for (i = 0; i < 16; i++) {
        uint8_t v = blk[15 - i];
        v = (uint8_t)(((v * 0x80200802ULL) & 0x0884422110ULL) * 0x0101010101ULL >> 32);
        wbuf[i] = v;
    }
    wr.p = wbuf; wr.pos = 0; wr.end = wbits;
    astc_bise(&wr, wenc, nweights, wm, wtq);
    for (i = 0; i < gw * gh; i++) {
        if (dual) { wq[0][i] = astc_unquant_weight(wenc, wm[2 * i], wtq[2 * i]); wq[1][i] = astc_unquant_weight(wenc, wm[2 * i + 1], wtq[2 * i + 1]); }
        else wq[0][i] = astc_unquant_weight(wenc, wm[i], wtq[i]);
    }
    /* infill */
    Ds = (1024 + bw / 2) / (bw - 1); Dt = (1024 + bh / 2) / (bh - 1);
    for (p = 0; p <= dual; p++)
        for (y = 0; y < bh; y++)
            for (x = 0; x < bw; x++) {
                int cs = Ds * x, ct = Dt * y, gs = (cs * (gw - 1) + 32) >> 6, gt = (ct * (gh - 1) + 32) >> 6;
                int js = gs >> 4, fs = gs & 15, jt = gt >> 4, ft = gt & 15, w11 = (fs * ft + 8) >> 4, w10 = ft - w11, w01 = fs - w11, w00 = 16 - fs - ft + w11;
                int v0 = js + jt * gw, p00 = wq[p][v0], p01 = js + 1 < gw ? wq[p][v0 + 1] : 0, p10 = jt + 1 < gh ? wq[p][v0 + gw] : 0, p11 = js + 1 < gw && jt + 1 < gh ? wq[p][v0 + gw + 1] : 0;
                wt[p][y * bw + x] = (p00 * w00 + p01 * w01 + p10 * w10 + p11 * w11 + 8) >> 4;
            }
    astc_partition_setup(&parts, part_index, nparts, small);
    for (i = 0; i < nparts; i++)
        for (p = 0; p < 4; p++) {        /* endpoints widened to 16 bits */
            e0[i][p] = srgb ? (e0[i][p] << 8) | 0x80 : (e0[i][p] << 8) | e0[i][p];
            e1[i][p] = srgb ? (e1[i][p] << 8) | 0x80 : (e1[i][p] << 8) | e1[i][p];
        }
    for (y = 0; y < bh; y++)
        for (x = 0; x < bw; x++) {
            int part = astc_partition_of(&parts, x, y), c, w0 = wt[0][y * bw + x], w1 = dual ? wt[1][y * bw + x] : w0;
            uint8_t *px = out + y * pitch + x * 4;
            for (c = 0; c < 4; c++) {
                int w = dual && c == ccs ? w1 : w0, v = (e0[part][c] * (64 - w) + e1[part][c] * w + 32) >> 6;
                px[c] = (uint8_t)(srgb ? v >> 8 : v == 65535 ? 255 : (v * 255 + 32768) >> 16);
            }
        }
    return;
bad:
    tex_astc_bad_blocks++;
    astc_fill(out, pitch, bw, bh, 255, 0, 255, 255);
}

/* ================================================================ public decoders */
static void bc45_to_rgba8(const uint8_t *blk, uint8_t *out, int pitch, int two, int snorm) {
    int r[16], g[16], i;
    bc4_block(blk, r, snorm);
    if (two) bc4_block(blk + 8, g, snorm);
    for (i = 0; i < 16; i++) {
        uint8_t *p = out + (i >> 2) * pitch + (i & 3) * 4;
        p[0] = snorm ? snorm_to_u8(r[i]) : (uint8_t)r[i];
        p[1] = two ? (snorm ? snorm_to_u8(g[i]) : (uint8_t)g[i]) : (uint8_t)(snorm ? 128 : 0);
        p[2] = (uint8_t)(snorm ? 128 : 0); p[3] = 255;
    }
}

int tex_decode_block_rgba8(int nf, const uint8_t *blk, uint8_t *out, int pitch) {
    const TexFormat *f = tex_format(nf);
    int i;
    if (!f) return -1;
    switch (f->codec) {
    case TEX_CODEC_BC1: bc1_block(blk, out, pitch, 0, f->ncomp == 4); return 0;
    case TEX_CODEC_BC2:
        bc1_block(blk + 8, out, pitch, 1, 0);
        for (i = 0; i < 16; i++) { int a = (blk[i >> 1] >> ((i & 1) * 4)) & 15; out[(i >> 2) * pitch + (i & 3) * 4 + 3] = (uint8_t)(a | a << 4); }
        return 0;
    case TEX_CODEC_BC3: {
        int a[16];
        bc1_block(blk + 8, out, pitch, 1, 0);
        bc4_block(blk, a, 0);
        for (i = 0; i < 16; i++) out[(i >> 2) * pitch + (i & 3) * 4 + 3] = (uint8_t)a[i];
        return 0; }
    case TEX_CODEC_BC4: bc45_to_rgba8(blk, out, pitch, 0, f->snorm); return 0;
    case TEX_CODEC_BC5: bc45_to_rgba8(blk, out, pitch, 1, f->snorm); return 0;
    case TEX_CODEC_BC6H: {
        float v[48];
        bc6h_block(blk, v, f->snorm);
        for (i = 0; i < 16; i++) {
            uint8_t *p = out + (i >> 2) * pitch + (i & 3) * 4;
            int c;
            for (c = 0; c < 3; c++) { float x = v[i * 3 + c]; p[c] = (uint8_t)(x <= 0 ? 0 : x >= 1 ? 255 : (int)(x * 255.0f + 0.5f)); }
            p[3] = 255;
        }
        return 0; }
    case TEX_CODEC_BC7: bc7_block(blk, out, pitch); return 0;
    case TEX_CODEC_ASTC: astc_block(blk, out, pitch, f->bw, f->bh, f->srgb); return 0;
    }
    return -1;
}

int tex_decode_rgba8(int nf, const void *src, int w, int h, uint8_t *dst) {
    const TexFormat *f = tex_format(nf);
    int bx, by, wb, hb, pitch = w * 4;
    uint8_t tmp[12 * 12 * 4];
    if (!f || !f->codec) return -1;
    wb = (w + f->bw - 1) / f->bw; hb = (h + f->bh - 1) / f->bh;
    for (by = 0; by < hb; by++)
        for (bx = 0; bx < wb; bx++) {
            const uint8_t *blk = (const uint8_t *)src + ((size_t)by * wb + bx) * f->bpb;
            int x0 = bx * f->bw, y0 = by * f->bh;
            if (x0 + f->bw <= w && y0 + f->bh <= h) tex_decode_block_rgba8(nf, blk, dst + (size_t)y0 * pitch + x0 * 4, pitch);
            else {
                int cw = w - x0 < f->bw ? w - x0 : f->bw, ch = h - y0 < f->bh ? h - y0 : f->bh, y;
                tex_decode_block_rgba8(nf, blk, tmp, f->bw * 4);
                for (y = 0; y < ch; y++) memcpy(dst + (size_t)(y0 + y) * pitch + x0 * 4, tmp + y * f->bw * 4, (size_t)cw * 4);
            }
        }
    return 0;
}

int tex_decode_rgba32f(int nf, const void *src, int w, int h, float *dst) {
    const TexFormat *f = tex_format(nf);
    int bx, by, wb, hb, i;
    if (!f || !f->codec) return -1;
    wb = (w + f->bw - 1) / f->bw; hb = (h + f->bh - 1) / f->bh;
    for (by = 0; by < hb; by++)
        for (bx = 0; bx < wb; bx++) {
            const uint8_t *blk = (const uint8_t *)src + ((size_t)by * wb + bx) * f->bpb;
            int x0 = bx * f->bw, y0 = by * f->bh, cw = w - x0 < f->bw ? w - x0 : f->bw, ch = h - y0 < f->bh ? h - y0 : f->bh, x, y;
            float tmp[12 * 12 * 4];
            if (f->codec == TEX_CODEC_BC6H) {
                float v[48];
                bc6h_block(blk, v, f->snorm);
                for (i = 0; i < 16; i++) { tmp[i * 4] = v[i * 3]; tmp[i * 4 + 1] = v[i * 3 + 1]; tmp[i * 4 + 2] = v[i * 3 + 2]; tmp[i * 4 + 3] = 1.0f; }
            } else if ((f->codec == TEX_CODEC_BC4 || f->codec == TEX_CODEC_BC5) && f->snorm) {
                int r[16], g[16];
                bc4_block(blk, r, 1);
                if (f->codec == TEX_CODEC_BC5) bc4_block(blk + 8, g, 1);
                for (i = 0; i < 16; i++) { tmp[i * 4] = r[i] / 4445.0f; tmp[i * 4 + 1] = f->codec == TEX_CODEC_BC5 ? g[i] / 4445.0f : 0.0f; tmp[i * 4 + 2] = 0.0f; tmp[i * 4 + 3] = 1.0f; }
            } else {
                uint8_t t8[12 * 12 * 4];
                tex_decode_block_rgba8(nf, blk, t8, f->bw * 4);
                for (i = 0; i < f->bw * f->bh * 4; i++) tmp[i] = t8[i] / 255.0f;
            }
            for (y = 0; y < ch; y++) for (x = 0; x < cw; x++) memcpy(dst + ((size_t)(y0 + y) * w + x0 + x) * 4, tmp + (y * f->bw + x) * 4, 16);
        }
    return 0;
}

/* ================================================================ uncompressed -> RGBA8 */
static inline uint8_t f2u8(float f) { return (uint8_t)(f <= 0 ? 0 : f >= 1 ? 255 : (int)(f * 255.0f + 0.5f)); }
static inline float u11f(uint32_t v) {     /* 11-bit float (5e6m), 10-bit float (5e5m) via m bits */
    uint32_t e = v >> 6, m = v & 63; float r;
    if (e == 0) r = m / 64.0f * (1.0f / 16384.0f); else if (e == 31) r = m ? 0.0f : 1e30f; else r = (float)ldexp(1.0 + m / 64.0, (int)e - 15);
    return r;
}
static inline float u10f(uint32_t v) {
    uint32_t e = v >> 5, m = v & 31; float r;
    if (e == 0) r = m / 32.0f * (1.0f / 16384.0f); else if (e == 31) r = m ? 0.0f : 1e30f; else r = (float)ldexp(1.0 + m / 32.0, (int)e - 15);
    return r;
}

int tex_convert_rgba8(int nf, const void *src, int w, int h, uint8_t *dst) {
    const TexFormat *f = tex_format(nf);
    size_t n = (size_t)w * h, i;
    const uint8_t *s = (const uint8_t *)src;
    int c;
    if (!f || f->codec) return -1;
    if ((nf >= 0x01 && nf <= 0x30) || (nf >= 0x6D && nf <= 0x78)) {
        int sz = (f->gl_type == GL_UNSIGNED_BYTE_ || f->gl_type == GL_BYTE_) ? 1 : (f->gl_type == GL_UNSIGNED_SHORT_ || f->gl_type == GL_SHORT_ || f->gl_type == GL_HALF_FLOAT_) ? 2 : 4;
        int comps = f->bpb / sz;
        for (i = 0; i < n; i++) {
            uint8_t *d = dst + i * 4;
            const uint8_t *p = s + i * f->bpb;
            d[0] = d[1] = d[2] = 0; d[3] = 255;
            for (c = 0; c < comps && c < 4; c++) {
                const uint8_t *q = p + c * sz;
                int v;
                if (f->is_float) d[c] = f2u8(sz == 2 ? tex_half_to_float((uint16_t)(q[0] | q[1] << 8)) : (float)(*(const float *)q));
                else if (f->is_int) { v = sz == 1 ? (f->gl_type == GL_BYTE_ ? (int8_t)q[0] : q[0]) : sz == 2 ? (f->gl_type == GL_SHORT_ ? (int16_t)(q[0] | q[1] << 8) : (q[0] | q[1] << 8)) : (int)rd32le(q); d[c] = (uint8_t)clamp255(v); }
                else if (f->snorm) { v = sz == 1 ? (int8_t)q[0] : (int16_t)(q[0] | q[1] << 8) >> 8; if (v < -127) v = -127; d[c] = snorm_to_u8(v * 35); }
                else d[c] = sz == 1 ? q[0] : q[1];
            }
            if (nf >= 0x6D) d[3] = 255;   /* RGBX */
        }
        return 0;
    }
    for (i = 0; i < n; i++) {
        uint8_t *d = dst + i * 4;
        const uint8_t *p = s + i * f->bpb;
        uint32_t v = f->bpb == 2 ? (uint32_t)(p[0] | p[1] << 8) : f->bpb >= 4 ? rd32le(p) : p[0];
        d[3] = 255;
        switch (nf) {
        case 0x31: d[0] = p[0]; d[1] = d[2] = 0; break;
        case 0x32: d[0] = d[1] = d[2] = (uint8_t)(v >> 8); break;
        case 0x33: d[0] = d[1] = d[2] = (uint8_t)(v >> 16); break;
        case 0x34: d[0] = d[1] = d[2] = f2u8(*(const float *)p); break;
        case 0x35: d[0] = d[1] = d[2] = (uint8_t)(v >> 24); d[3] = (uint8_t)(v & 0xFF); break;      /* depth in top 24 bits, stencil low byte */
        case 0x36: d[0] = d[1] = d[2] = f2u8(*(const float *)p); d[3] = p[4]; break;
        case 0x37: case 0x38: d[0] = p[0]; d[1] = p[1]; d[2] = p[2]; d[3] = nf == 0x38 ? p[3] : 255; break;
        case 0x39: d[0] = rep8(v >> 12, 4); d[1] = rep8((v >> 8) & 15, 4); d[2] = rep8((v >> 4) & 15, 4); d[3] = rep8(v & 15, 4); break;
        case 0x3A: case 0x3B: d[0] = rep8(v >> 11, 5); d[1] = rep8((v >> 6) & 31, 5); d[2] = rep8((v >> 1) & 31, 5); d[3] = nf == 0x3B ? ((v & 1) ? 255 : 0) : 255; break;
        case 0x3C: d[0] = rep8(v >> 11, 5); d[1] = rep8((v >> 5) & 63, 6); d[2] = rep8(v & 31, 5); break;
        case 0x3D: case 0x3E: d[0] = (uint8_t)((v & 1023) >> 2); d[1] = (uint8_t)(((v >> 10) & 1023) >> 2); d[2] = (uint8_t)(((v >> 20) & 1023) >> 2); d[3] = rep8(v >> 30, 2); break;
        case 0x3F: d[0] = f2u8(u11f(v & 2047)); d[1] = f2u8(u11f((v >> 11) & 2047)); d[2] = f2u8(u10f(v >> 22)); break;
        case 0x40: { float sc = (float)ldexp(1.0, (int)(v >> 27) - 15 - 9); d[0] = f2u8((v & 511) * sc); d[1] = f2u8(((v >> 9) & 511) * sc); d[2] = f2u8(((v >> 18) & 511) * sc); } break;
        case 0x95: d[2] = rep8(v >> 11, 5); d[1] = rep8((v >> 5) & 63, 6); d[0] = rep8(v & 31, 5); break;
        case 0x96: case 0x97: d[2] = rep8(v >> 11, 5); d[1] = rep8((v >> 6) & 31, 5); d[0] = rep8((v >> 1) & 31, 5); d[3] = nf == 0x97 ? ((v & 1) ? 255 : 0) : 255; break;
        case 0x98: d[0] = rep8(v & 31, 5); d[1] = rep8((v >> 5) & 31, 5); d[2] = rep8((v >> 10) & 31, 5); d[3] = (v >> 15) ? 255 : 0; break;
        case 0x99: case 0x9A: case 0x9B: case 0x9C: d[0] = p[2]; d[1] = p[1]; d[2] = p[0]; d[3] = (nf == 0x9A || nf == 0x9C) ? p[3] : 255; break;
        default: return -1;
        }
    }
    return 0;
}

/* ================================================================ PNG writer (stored deflate) */
static uint32_t crc_table[256];
static uint32_t crc32_update(uint32_t c, const uint8_t *p, size_t n) {
    size_t i;
    if (!crc_table[1]) { uint32_t k, j; for (k = 0; k < 256; k++) { uint32_t v = k; for (j = 0; j < 8; j++) v = (v & 1) ? 0xEDB88320u ^ (v >> 1) : v >> 1; crc_table[k] = v; } }
    for (i = 0; i < n; i++) c = crc_table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c;
}
static void png_chunk(FILE *fp, const char *type, const uint8_t *data, uint32_t len) {
    uint8_t hdr[8];
    uint32_t crc = 0xFFFFFFFFu;
    hdr[0] = (uint8_t)(len >> 24); hdr[1] = (uint8_t)(len >> 16); hdr[2] = (uint8_t)(len >> 8); hdr[3] = (uint8_t)len;
    memcpy(hdr + 4, type, 4);
    fwrite(hdr, 1, 8, fp);
    if (len) fwrite(data, 1, len, fp);
    crc = crc32_update(crc, hdr + 4, 4);
    if (len) crc = crc32_update(crc, data, len);
    crc ^= 0xFFFFFFFFu;
    hdr[0] = (uint8_t)(crc >> 24); hdr[1] = (uint8_t)(crc >> 16); hdr[2] = (uint8_t)(crc >> 8); hdr[3] = (uint8_t)crc;
    fwrite(hdr, 1, 4, fp);
}

int tex_write_png(const char *path, const uint8_t *rgba, int w, int h) {
    static const uint8_t sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    FILE *fp = fopen(path, "wb");
    size_t rowlen = (size_t)w * 4 + 1, raw_len = rowlen * (size_t)h, nblocks = (raw_len + 65534) / 65535, zlen = 2 + raw_len + nblocks * 5 + 4, pos = 0, done = 0;
    uint8_t *z, ihdr[13];
    uint32_t a = 1, b = 0;
    int y;
    if (!fp) return -1;
    if (nblocks == 0) nblocks = 1;
    z = (uint8_t *)malloc(zlen);
    if (!z) { fclose(fp); return -1; }
    z[pos++] = 0x78; z[pos++] = 0x01;
    for (y = 0; y < h; y++) {
        const uint8_t *row = rgba + (size_t)y * w * 4;
        size_t i;
        uint8_t filt = 0;
        a = (a + filt) % 65521; b = (b + a) % 65521;
        for (i = 0; i < (size_t)w * 4; i++) { a = (a + row[i]) % 65521; b = (b + a) % 65521; }
        (void)filt;
    }
    {
        /* stored blocks over the filtered rows (filter byte 0 before each row) */
        size_t remaining = raw_len, ry = 0, rx = 0;          /* (ry, rx): position in the row stream, rx 0 = filter byte */
        while (remaining > 0 || done == 0) {
            size_t len = remaining < 65535 ? remaining : 65535, k;
            z[pos++] = (uint8_t)(len == remaining ? 1 : 0);
            z[pos++] = (uint8_t)len; z[pos++] = (uint8_t)(len >> 8); z[pos++] = (uint8_t)~len; z[pos++] = (uint8_t)(~len >> 8);
            for (k = 0; k < len; k++) {
                z[pos++] = rx == 0 ? 0 : rgba[(ry * (size_t)w * 4) + rx - 1];
                if (++rx == rowlen) { rx = 0; ry++; }
            }
            remaining -= len; done = 1;
        }
    }
    z[pos++] = (uint8_t)(b >> 8); z[pos++] = (uint8_t)b; z[pos++] = (uint8_t)(a >> 8); z[pos++] = (uint8_t)a;
    fwrite(sig, 1, 8, fp);
    ihdr[0] = (uint8_t)(w >> 24); ihdr[1] = (uint8_t)(w >> 16); ihdr[2] = (uint8_t)(w >> 8); ihdr[3] = (uint8_t)w;
    ihdr[4] = (uint8_t)(h >> 24); ihdr[5] = (uint8_t)(h >> 16); ihdr[6] = (uint8_t)(h >> 8); ihdr[7] = (uint8_t)h;
    ihdr[8] = 8; ihdr[9] = 6; ihdr[10] = 0; ihdr[11] = 0; ihdr[12] = 0;
    png_chunk(fp, "IHDR", ihdr, 13);
    png_chunk(fp, "IDAT", z, (uint32_t)pos);
    png_chunk(fp, "IEND", NULL, 0);
    free(z);
    fclose(fp);
    return 0;
}
