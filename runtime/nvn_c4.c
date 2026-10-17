/* NVN entry points closed in the round "close4" (2026-10-09) whose code does not need nvn_gl.c's state.
 *
 * nvnCommandBufferDrawTexture(cmdbuf, textureHandle, const NVNdrawTextureRegion *dst, const NVNdrawTextureRegion *src)  [sdk 0x438210]
 *   NVNdrawTextureRegion = four floats {x0, y0, x1, y1} (read with `ldp d0, d1, [x2]` and `ldp s3, s4, [x3]` / `ldp s1, s2, [x3, #8]`).
 *   The encoder computes twelve words and gives them to GPU method 0x420 (count 12) = suyu maxwell_3d.h Regs::DrawTexture
 *   (byte offset 0x1080): dst_x0, dst_y0, dst_width, dst_height (20.12 fixed), dx_du, dy_dv (32.32 fixed, texels per pixel),
 *   src_sampler, src_texture, src_x0, src_y0 (20.12 fixed, texels). Arithmetic, as in the code:
 *     q(v)      = floor(double(v) * 256 + 0.5) * 16                       [0x438234 .. 0x43826c; constant 256.0 = 0x4070000000000000]
 *     dst_x0/y0 = q(dst x0 / y0); dst_width / height = q(x1) - q(x0), q(y1) - q(y0) as 32-bit; nothing at all is written when
 *                 either is 0                                               [0x438270 .. 0x438284]
 *     dx_du     = floor(double(float(src x1 - src x0)) / (dst_width * 2^-12) * 2^32 + 0.5)   [0x438288 .. 0x4382cc; constants at
 *                 0xa12fe0 = 2^-12, 0xa12d10 = 2^32]; dy_dv the same with y  [0x4382d0 .. 0x4382ec]
 *     src_x0/y0 = q(src x0 / y0) (32-bit conversion)                        [0x4383d4 .. 0x438434; constant at 0xa13138 = 256.0]
 *     sampler / texture = bits 20..31 / 0..19 of the handle                 [0x4382f0, 0x4382f4]
 *   Around the method the encoder switches the shadow RAM to pass-through, writes depth_test_enable = 0 (0x4b3),
 *   stencil_enable = 0 (0x4e0), depth_bounds_enable = 0 (0x66f), const_color_rendering = 0 (0x3d0), vertex program B off
 *   (0x810 = 0x10), a driver fragment program on (0x850 = 0x51, 0x851 = its offset: device +0x94c plus [device +0x1ab0] +0x120),
 *   viewport 0 = full (0x300), tir_mode = 0 (0x3ed), multisample_coverage_to_color = 0 (0x47e), and afterwards replays the
 *   shadowed values (0x49 = 3) (decoded from the words the emulator produces: tools_audit/patches/close4/uc_nvn_cmd.py drawtex).
 *   So: depth, stencil and depth bounds tests are off for this one operation, the game's shaders are not used, everything
 *   else (scissor, blending, colour mask, render targets) applies.
 * What the GPU does with the twelve words: suyu video_core/engines/draw_manager.cpp DrawTexture (destination rectangle =
 *   x0, y0, x0 + width, y0 + height in pixels; source = src_x0, src_y0, src_x0 + dx_du * width, src_y0 + dy_dv * height in
 *   texels) and renderer_opengl/gl_rasterizer.cpp DrawTexture: glDrawTextureNV(texture, sampler, destination, 0, source divided
 *   by the texture size). The same call is used here; unlike suyu the destination keeps its fraction (suyu truncates to int).
 *   UNVERIFIED: the driver's fragment program was not read (it is taken to be a plain texture fetch, as the reference does);
 *   the alpha test is applied with fixed function when the game has one set (the encoder does not turn it off).
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <GL/gl.h>
#include "nvn_c4.h"

/* AArch64 conversions used by the encoder */
static int64_t fcvtzs64(double v) {                 /* double -> int64, toward zero, saturating, NaN -> 0 */
    if (v != v) return 0;
    if (v >= 9223372036854775808.0) return INT64_MAX;
    if (v <= -9223372036854775808.0) return INT64_MIN;
    return (int64_t)v;
}
static int32_t fcvtzs32(double v) {
    if (v != v) return 0;
    if (v >= 2147483648.0) return INT32_MAX;
    if (v <= -2147483648.0) return INT32_MIN;
    return (int32_t)v;
}
static double q256(float v) { return floor((double)v * 256.0 + 0.5); }

int nvn_c4_draw_texture_words(const float dst[4], const float src[4], NvnC4DrawTexture *o) {
    int32_t x0 = (int32_t)(uint32_t)(uint64_t)fcvtzs64(q256(dst[0]) * 16.0), y0 = (int32_t)(uint32_t)(uint64_t)fcvtzs64(q256(dst[1]) * 16.0);
    int32_t x1 = (int32_t)(uint32_t)(uint64_t)fcvtzs64(q256(dst[2]) * 16.0), y1 = (int32_t)(uint32_t)(uint64_t)fcvtzs64(q256(dst[3]) * 16.0);
    int32_t w = (int32_t)((uint32_t)x1 - (uint32_t)x0), h = (int32_t)((uint32_t)y1 - (uint32_t)y0);
    memset(o, 0, sizeof *o);
    if (w == 0 || h == 0) return 0;
    float du = src[2] - src[0], dv = src[3] - src[1];
    o->dst_x0 = x0; o->dst_y0 = y0; o->dst_width = w; o->dst_height = h;
    o->dx_du = fcvtzs64(floor((double)du / ((double)w * 0.000244140625) * 4294967296.0 + 0.5));
    o->dy_dv = fcvtzs64(floor((double)dv / ((double)h * 0.000244140625) * 4294967296.0 + 0.5));
    o->src_x0 = fcvtzs32(q256(src[0]) * 16.0);
    o->src_y0 = fcvtzs32(q256(src[1]) * 16.0);
    return 1;
}

void nvn_c4_draw_texture_rect(const NvnC4DrawTexture *w, float out[8]) {       /* suyu draw_manager.cpp DrawTexture */
    float dw = (float)w->dst_width / 4096.f, dh = (float)w->dst_height / 4096.f;
    out[0] = (float)w->dst_x0 / 4096.f; out[1] = (float)w->dst_y0 / 4096.f;
    out[2] = out[0] + dw; out[3] = out[1] + dh;
    out[4] = (float)w->src_x0 / 4096.f; out[5] = (float)w->src_y0 / 4096.f;
    out[6] = ((float)w->dx_du / 4294967296.f) * dw + out[4];
    out[7] = ((float)w->dy_dv / 4294967296.f) * dh + out[5];
}

/* ---------------------------------------------------------------- OpenGL (render thread) */
static void (APIENTRY *p_DrawTextureNV)(GLuint, GLuint, GLfloat, GLfloat, GLfloat, GLfloat, GLfloat, GLfloat, GLfloat, GLfloat, GLfloat);
static void (APIENTRY *p_GetTextureParameteriv)(GLuint, GLenum, GLint *);
static void (APIENTRY *p_GetTextureLevelParameteriv)(GLuint, GLint, GLenum, GLint *);
static int g_gl_loaded;
static void gl_load(void) {
    if (g_gl_loaded) return;
    g_gl_loaded = 1;
    p_DrawTextureNV = (void *)wglGetProcAddress("glDrawTextureNV");
    p_GetTextureParameteriv = (void *)wglGetProcAddress("glGetTextureParameteriv");
    p_GetTextureLevelParameteriv = (void *)wglGetProcAddress("glGetTextureLevelParameteriv");
}

int nvn_c4_draw_texture_gl(unsigned tex, unsigned smp, const NvnC4DrawTexture *w, int alpha_func, float alpha_ref) {
    float r[8];
    GLint target = 0, tw = 0, th = 0, base = 0;
    gl_load();
    if (!p_DrawTextureNV || !p_GetTextureParameteriv || !p_GetTextureLevelParameteriv) {
        printf("nvn: stopping: nvnCommandBufferDrawTexture needs GL_NV_draw_texture and OpenGL 4.5, which this driver lacks\n");
        fflush(stdout);
        ExitProcess(6);
    }
    p_GetTextureParameteriv(tex, 0x1006 /* TEXTURE_TARGET */, &target);
    if (target != GL_TEXTURE_2D && target != 0x84F5 /* TEXTURE_RECTANGLE */) return 0;
    p_GetTextureParameteriv(tex, 0x813C /* TEXTURE_BASE_LEVEL */, &base);
    p_GetTextureLevelParameteriv(tex, base, GL_TEXTURE_WIDTH, &tw);
    p_GetTextureLevelParameteriv(tex, base, GL_TEXTURE_HEIGHT, &th);
    if (tw <= 0 || th <= 0) return 0;
    nvn_c4_draw_texture_rect(w, r);
    GLboolean depth = glIsEnabled(GL_DEPTH_TEST), stencil = glIsEnabled(GL_STENCIL_TEST), bounds = glIsEnabled(0x8890 /* DEPTH_BOUNDS_TEST_EXT */);
    GLboolean alpha = glIsEnabled(GL_ALPHA_TEST);
    GLint afunc = GL_ALWAYS; GLfloat aref = 0;
    glGetIntegerv(GL_ALPHA_TEST_FUNC, &afunc); glGetFloatv(GL_ALPHA_TEST_REF, &aref);
    if (depth) glDisable(GL_DEPTH_TEST);
    if (stencil) glDisable(GL_STENCIL_TEST);
    if (bounds) glDisable(0x8890);
    if (alpha_func >= 1 && alpha_func <= 7) { glAlphaFunc(GL_NEVER + (GLenum)(alpha_func - 1), alpha_ref); glEnable(GL_ALPHA_TEST); }   /* NVN compare functions 1..8 = NEVER..ALWAYS */
    else if (alpha) glDisable(GL_ALPHA_TEST);
    if (target == GL_TEXTURE_2D) p_DrawTextureNV(tex, smp, r[0], r[1], r[2], r[3], 0.f, r[4] / (float)tw, r[5] / (float)th, r[6] / (float)tw, r[7] / (float)th);
    else p_DrawTextureNV(tex, smp, r[0], r[1], r[2], r[3], 0.f, r[4], r[5], r[6], r[7]);                                          /* rectangle textures take texels */
    glAlphaFunc((GLenum)afunc, aref);
    if (alpha) glEnable(GL_ALPHA_TEST); else glDisable(GL_ALPHA_TEST);
    if (bounds) glEnable(0x8890);
    if (stencil) glEnable(GL_STENCIL_TEST);
    if (depth) glEnable(GL_DEPTH_TEST);
    return 1;
}

/* ---------------------------------------------------------------- self-tests */
int nvn_c4_selftest(void) {
    /* generated by `PYTHONPATH=. python tools_audit/patches/close4/uc_nvn_cmd.py drawtex-table`: the real encoder [sdk 0x438210] run in
     * the Unicorn emulator; handle 0x00500007 (sampler 5, texture 7) */
    static const struct { float dst[4], src[4]; int written; uint32_t w[12]; } t[] = {
        {{10.0f, 20.0f, 110.0f, 70.0f}, {0.0f, 0.0f, 64.0f, 32.0f}, 1, {0x0000a000, 0x00014000, 0x00064000, 0x00032000, 0xa3d70a3d, 0x00000000, 0xa3d70a3d, 0x00000000, 0x00000005, 0x00000007, 0x00000000, 0x00000000}},
        {{0.5f, 0.25f, 3.75f, 9.126f}, {1.5f, 2.25f, 7.0f, 3.0f}, 1, {0x00000800, 0x00000400, 0x00003400, 0x00008e00, 0xb13b13b1, 0x00000001, 0x15a240e7, 0x00000000, 0x00000005, 0x00000007, 0x00001800, 0x00002400}},
        {{100.0f, 50.0f, 20.0f, 10.0f}, {0.0f, 0.0f, 16.0f, 16.0f}, 1, {0x00064000, 0x00032000, 0xfffb0000, 0xfffd8000, 0xcccccccd, 0xffffffff, 0x9999999a, 0xffffffff, 0x00000005, 0x00000007, 0x00000000, 0x00000000}},
        {{-8.3f, -2.7f, 40.1f, 33.3f}, {5.0f, 6.0f, 1.0f, 2.0f}, 1, {0xffff7b30, 0xffffd4d0, 0x00030670, 0x00024000, 0xead8105d, 0xffffffff, 0xe38e38e4, 0xffffffff, 0x00000005, 0x00000007, 0x00005000, 0x00006000}},
        {{0.0f, 0.0f, 1280.0f, 720.0f}, {0.0f, 0.0f, 1920.0f, 1080.0f}, 1, {0x00000000, 0x00000000, 0x00500000, 0x002d0000, 0x80000000, 0x00000001, 0x80000000, 0x00000001, 0x00000005, 0x00000007, 0x00000000, 0x00000000}},
        {{3.0f, 3.0f, 3.001f, 9.0f}, {0.0f, 0.0f, 4.0f, 4.0f}, 0, {0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000}},
        {{3.0f, 3.0f, 3.002f, 9.0f}, {0.0f, 0.0f, 4.0f, 4.0f}, 1, {0x00003000, 0x00003000, 0x00000010, 0x00006000, 0x00000000, 0x00000400, 0xaaaaaaab, 0x00000000, 0x00000005, 0x00000007, 0x00000000, 0x00000000}},
        {{7.0f, 9.0f, 7.0f, 30.0f}, {0.0f, 0.0f, 4.0f, 4.0f}, 0, {0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000}},
        {{7.0f, 9.0f, 30.0f, 9.0f}, {0.0f, 0.0f, 4.0f, 4.0f}, 0, {0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000}},
        {{1.998f, 0.002f, 600.37f, 4000.49f}, {0.1f, 0.9f, 1023.7f, 511.2f}, 1, {0x00001ff0, 0x00000010, 0x00256600, 0x00fa07c0, 0xb5ec0bc4, 0x00000001, 0x20a7be42, 0x00000000, 0x00000005, 0x00000007, 0x000001a0, 0x00000e60}},
        {{16.0f, 16.0f, 48.0f, 80.0f}, {64.0f, 0.0f, 0.0f, 128.0f}, 1, {0x00010000, 0x00010000, 0x00020000, 0x00040000, 0x00000000, 0xfffffffe, 0x00000000, 0x00000002, 0x00000005, 0x00000007, 0x00040000, 0x00000000}},
    };
    int bad = 0, n = 0;
    for (size_t i = 0; i < sizeof t / sizeof t[0]; i++) {
        NvnC4DrawTexture w;
        uint32_t g[12];
        int wr = nvn_c4_draw_texture_words(t[i].dst, t[i].src, &w);
        w.src_sampler = 5; w.src_texture = 7;
        g[0] = (uint32_t)w.dst_x0; g[1] = (uint32_t)w.dst_y0; g[2] = (uint32_t)w.dst_width; g[3] = (uint32_t)w.dst_height;
        g[4] = (uint32_t)w.dx_du; g[5] = (uint32_t)((uint64_t)w.dx_du >> 32); g[6] = (uint32_t)w.dy_dv; g[7] = (uint32_t)((uint64_t)w.dy_dv >> 32);
        g[8] = w.src_sampler; g[9] = w.src_texture; g[10] = (uint32_t)w.src_x0; g[11] = (uint32_t)w.src_y0;
        n++;
        if (wr != t[i].written || (wr && memcmp(g, t[i].w, sizeof g))) {
            bad++;
            printf("  FAIL nvn DrawTexture words, case %d: written %d (emulator %d):", (int)i, wr, t[i].written);
            for (int k = 0; k < 12; k++) printf(" %08x%s", g[k], g[k] == t[i].w[k] ? "" : "!");
            printf("\n");
        }
    }
    {   /* suyu's rectangle from the first case: destination (10,20)-(110,70), source (0,0)-(64,32) */
        NvnC4DrawTexture w;
        float r[8];
        static const float want[8] = {10, 20, 110, 70, 0, 0, 64, 32};
        nvn_c4_draw_texture_words(t[0].dst, t[0].src, &w);
        nvn_c4_draw_texture_rect(&w, r);
        n++;
        for (int k = 0; k < 8; k++) if (fabsf(r[k] - want[k]) > 1e-4f) { bad++; printf("  FAIL nvn DrawTexture rectangle word %d: %f, expected %f\n", k, r[k], want[k]); break; }
    }
    printf("  nvn_c4: %d DrawTexture encoder cases, %d failed\n", n, bad);
    return bad;
}

/* OpenGL self-test (SSBU_NVN_GLTEST=1, run by the render thread once the context exists): draws a 4x2 texture with
 * nvn_c4_draw_texture_gl into a 16x16 target and reads the pixels back. */
static void (APIENTRY *p_GenFramebuffers)(GLsizei, GLuint *);
static void (APIENTRY *p_BindFramebuffer)(GLenum, GLuint);
static void (APIENTRY *p_FramebufferTexture)(GLenum, GLenum, GLuint, GLint);
static GLenum (APIENTRY *p_CheckFramebufferStatus)(GLenum);
static void (APIENTRY *p_TexStorage2D)(GLenum, GLsizei, GLenum, GLsizei, GLsizei);
static void (APIENTRY *p_GenSamplers)(GLsizei, GLuint *);
static void (APIENTRY *p_SamplerParameteri)(GLuint, GLenum, GLint);

static int gltest_case(const char *name, GLuint tex, GLuint smp, const float dst[4], const float src[4], const uint8_t texels[2][4][4], int flip_x, int flip_y) {
    static uint8_t px[16 * 16 * 4];
    NvnC4DrawTexture w;
    int bad = 0;
    glClearColor(0, 0, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    if (!nvn_c4_draw_texture_words(dst, src, &w)) { printf("  FAIL %s: the encoder wrote nothing\n", name); return 1; }
    if (!nvn_c4_draw_texture_gl(tex, smp, &w, 0, 0.f)) { printf("  FAIL %s: not drawn\n", name); return 1; }
    glReadPixels(0, 0, 16, 16, GL_RGBA, GL_UNSIGNED_BYTE, px);
    for (int y = 0; y < 16; y++) for (int x = 0; x < 16; x++) {
        uint8_t want[4] = {0, 0, 0, 0};
        if (x >= 4 && x < 12 && y >= 2 && y < 6) {
            int tx = (x - 4) / 2, ty = (y - 2) / 2;
            memcpy(want, texels[flip_y ? 1 - ty : ty][flip_x ? 3 - tx : tx], 4);
        }
        if (memcmp(want, px + (y * 16 + x) * 4, 4)) {
            if (bad < 4) printf("  FAIL %s: pixel (%d,%d) = %02x %02x %02x %02x, expected %02x %02x %02x %02x\n", name, x, y, px[(y * 16 + x) * 4], px[(y * 16 + x) * 4 + 1],
                                px[(y * 16 + x) * 4 + 2], px[(y * 16 + x) * 4 + 3], want[0], want[1], want[2], want[3]);
            bad++;
        }
    }
    return bad != 0;
}
void nvn_c4_gl_selftest_hook(void) {
    if (!getenv("SSBU_NVN_GLTEST")) return;
    static const uint8_t texels[2][4][4] = {{{255, 0, 0, 255}, {0, 255, 0, 255}, {0, 0, 255, 255}, {255, 255, 0, 255}}, {{0, 255, 255, 255}, {255, 0, 255, 255}, {64, 128, 192, 255}, {255, 255, 255, 255}}};
    static const float dst[4] = {4, 2, 12, 6}, src[4] = {0, 0, 4, 2}, src_fx[4] = {4, 0, 0, 2}, src_fy[4] = {0, 2, 4, 0};
    GLuint tex, target, depth, fbo, smp;
    int bad = 0, n = 0;
    gl_load();
    p_GenFramebuffers = (void *)wglGetProcAddress("glGenFramebuffers"); p_BindFramebuffer = (void *)wglGetProcAddress("glBindFramebuffer");
    p_FramebufferTexture = (void *)wglGetProcAddress("glFramebufferTexture"); p_CheckFramebufferStatus = (void *)wglGetProcAddress("glCheckFramebufferStatus");
    p_TexStorage2D = (void *)wglGetProcAddress("glTexStorage2D"); p_GenSamplers = (void *)wglGetProcAddress("glGenSamplers"); p_SamplerParameteri = (void *)wglGetProcAddress("glSamplerParameteri");
    printf("NVN OpenGL self-test (close4): GL_NV_draw_texture %s\n", p_DrawTextureNV ? "present" : "MISSING");
    if (!p_DrawTextureNV) { fflush(stdout); ExitProcess(1); }
    glGenTextures(1, &tex); glBindTexture(GL_TEXTURE_2D, tex); p_TexStorage2D(GL_TEXTURE_2D, 1, 0x8058 /* RGBA8 */, 4, 2);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1); glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 4, 2, GL_RGBA, GL_UNSIGNED_BYTE, texels);
    glGenTextures(1, &target); glBindTexture(GL_TEXTURE_2D, target); p_TexStorage2D(GL_TEXTURE_2D, 1, 0x8058, 16, 16);
    glGenTextures(1, &depth); glBindTexture(GL_TEXTURE_2D, depth); p_TexStorage2D(GL_TEXTURE_2D, 1, 0x88F0 /* DEPTH24_STENCIL8 */, 16, 16);
    glBindTexture(GL_TEXTURE_2D, 0);
    p_GenSamplers(1, &smp);
    p_SamplerParameteri(smp, GL_TEXTURE_MIN_FILTER, GL_NEAREST); p_SamplerParameteri(smp, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    p_SamplerParameteri(smp, GL_TEXTURE_WRAP_S, 0x812F /* CLAMP_TO_EDGE */); p_SamplerParameteri(smp, GL_TEXTURE_WRAP_T, 0x812F);
    p_GenFramebuffers(1, &fbo); p_BindFramebuffer(0x8D40, fbo);
    p_FramebufferTexture(0x8D40, 0x8CE0 /* COLOR_ATTACHMENT0 */, target, 0); p_FramebufferTexture(0x8D40, 0x821A /* DEPTH_STENCIL_ATTACHMENT */, depth, 0);
    if (p_CheckFramebufferStatus(0x8D40) != 0x8CD5) { printf("  FAIL framebuffer incomplete\n"); fflush(stdout); ExitProcess(1); }
    glViewport(0, 0, 16, 16);
    n++; bad += gltest_case("each texel -> 2x2 pixels, destination (4,2)-(12,6), row 0 of the texture at the lower window y", tex, smp, dst, src, texels, 0, 0);
    n++; bad += gltest_case("source x reversed", tex, smp, dst, src_fx, texels, 1, 0);
    n++; bad += gltest_case("source y reversed", tex, smp, dst, src_fy, texels, 0, 1);
    /* depth and stencil tests that would reject every fragment: the operation turns them off [sdk: methods 0x4b3, 0x4e0 = 0] and puts them back */
    glEnable(GL_DEPTH_TEST); glDepthFunc(GL_NEVER); glEnable(GL_STENCIL_TEST); glStencilFunc(GL_NEVER, 0, 0xff);
    n++; bad += gltest_case("depth func NEVER and stencil func NEVER do not apply", tex, smp, dst, src, texels, 0, 0);
    n++; if (!glIsEnabled(GL_DEPTH_TEST) || !glIsEnabled(GL_STENCIL_TEST)) { bad++; printf("  FAIL depth / stencil enables were not restored\n"); }
    glDisable(GL_DEPTH_TEST); glDepthFunc(GL_LESS); glDisable(GL_STENCIL_TEST); glStencilFunc(GL_ALWAYS, 0, 0xff);
    {   /* scissor applies: only x < 8 may be written */
        static uint8_t px[16 * 16 * 4];
        NvnC4DrawTexture w;
        int wrong = 0;
        glDisable(GL_SCISSOR_TEST); glClearColor(0, 0, 0, 0); glClear(GL_COLOR_BUFFER_BIT);
        glEnable(GL_SCISSOR_TEST); glScissor(0, 0, 8, 16);
        nvn_c4_draw_texture_words(dst, src, &w); nvn_c4_draw_texture_gl(tex, smp, &w, 0, 0.f);
        glScissor(0, 0, 16384, 16384);
        glReadPixels(0, 0, 16, 16, GL_RGBA, GL_UNSIGNED_BYTE, px);
        for (int y = 0; y < 16; y++) for (int x = 0; x < 16; x++) { int in = x >= 4 && x < 8 && y >= 2 && y < 6; if ((px[(y * 16 + x) * 4 + 3] != 0) != in) wrong++; }
        n++; if (wrong) { bad++; printf("  FAIL scissor: %d pixels wrong\n", wrong); }
    }
    {   /* alpha test (fixed function) with the NVN function 5 = GREATER, reference 2.0: nothing passes; state restored */
        static uint8_t px[16 * 16 * 4];
        NvnC4DrawTexture w;
        int written = 0;
        glClearColor(0, 0, 0, 0); glClear(GL_COLOR_BUFFER_BIT);
        nvn_c4_draw_texture_words(dst, src, &w); nvn_c4_draw_texture_gl(tex, smp, &w, 5, 2.0f);
        glReadPixels(0, 0, 16, 16, GL_RGBA, GL_UNSIGNED_BYTE, px);
        for (int i = 0; i < 16 * 16; i++) if (px[i * 4 + 3]) written++;
        n++; if (written || glIsEnabled(GL_ALPHA_TEST)) { bad++; printf("  FAIL alpha test GREATER 2.0: %d pixels written, alpha test left %s\n", written, glIsEnabled(GL_ALPHA_TEST) ? "on" : "off"); }
    }
    GLenum err = glGetError();
    n++; if (err) { bad++; printf("  FAIL OpenGL error 0x%x\n", err); }
    printf("NVN OpenGL self-test: %d cases, %d failure(s)\n", n, bad);
    fflush(stdout);
    ExitProcess((UINT)bad);
}
