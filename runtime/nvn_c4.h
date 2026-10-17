/* NVN functions closed in the round "close4" that do not need nvn_gl.c's state: see nvn_c4.c. */
#ifndef NVN_C4_H
#define NVN_C4_H
#include <stdint.h>

/* The twelve words nvnCommandBufferDrawTexture gives to GPU method 0x420 (suyu maxwell_3d.h Regs::DrawTexture, byte 0x1080). */
typedef struct {
    int32_t dst_x0, dst_y0, dst_width, dst_height;     /* 20.12 fixed point */
    int64_t dx_du, dy_dv;                              /* 32.32 fixed point: texels per pixel */
    uint32_t src_sampler, src_texture;
    int32_t src_x0, src_y0;                            /* 20.12 fixed point, texels */
} NvnC4DrawTexture;

/* dst / src = NVNdrawTextureRegion {x0, y0, x1, y1} (floats). Returns 0 when the encoder writes nothing (empty destination). */
int nvn_c4_draw_texture_words(const float dst[4], const float src[4], NvnC4DrawTexture *o);
/* Destination rectangle (pixels) and source rectangle (texels) the GPU derives from the words: out = dx0, dy0, dx1, dy1, sx0, sy0, sx1, sy1. */
void nvn_c4_draw_texture_rect(const NvnC4DrawTexture *w, float out[8]);
/* Render thread: draw with GL_NV_draw_texture into the bound framebuffer. alpha_func = NVN compare function (0 / 8 = no test).
 * Returns 0 if the texture is not a 2D or rectangle texture (nothing drawn). */
int nvn_c4_draw_texture_gl(unsigned tex, unsigned smp, const NvnC4DrawTexture *w, int alpha_func, float alpha_ref);
/* Render thread, after the context is ready: with SSBU_NVN_GLTEST set, run the OpenGL self-test and exit (exit code = failures). */
void nvn_c4_gl_selftest_hook(void);
/* Part of the SDK self-test (no OpenGL): the encoder arithmetic against words produced by the emulator on sdk.elf. */
int nvn_c4_selftest(void);
#endif
