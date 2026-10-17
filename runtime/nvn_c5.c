/* NVN: round "close5" (2026-10-09), helper C. Self-test of the guest-visible handle values.
 *
 * Until this round nvn_gl.c answered  nvnDeviceGetTextureHandle = textureID | samplerID << 32  and the bare ID for the texel
 * fetch, separate texture and separate sampler handles. The real functions (sdk.elf, addresses from sdknvn.py):
 *   nvnDeviceGetTextureHandle         [sdk 0x41bc80]  orr w8, w1, w2, lsl #20; sxtw x8, w8; orr x0, x8, #0x100000000
 *   nvnDeviceGetTexelFetchHandle      [sdk 0x41bc70]  sxtw x8, w1; orr x0, x8, #0x100000000
 *   nvnDeviceGetImageHandle           [sdk 0x41bc00]  sxtw x0, w1
 *   nvnDeviceGetSeparateTextureHandle [sdk 0x41bc60]  sxtw x8, w1; orr x0, x8, #0x100000000
 *   nvnDeviceGetSeparateSamplerHandle [sdk 0x41bc50]  sbfiz x0, x1, #20, #32
 * The table below is the output of `PYTHONPATH=. python tools_audit/patches/close5/uc_nvn_e.py handles` (those five functions
 * run in the Unicorn emulator on sdk.elf, upper halves of the argument registers filled with garbage).
 * What the game does with the value (Ghidra, main 13.0.4): FUN_71038037d0 and its five siblings (pointer kept at 0x710593f938)
 * and FUN_71000333b0 (pointer through 0x71052a2500) pass it unchanged to the next NVN call (nvnCommandBufferBindTexture);
 * no arithmetic on it was found at those call sites.
 */
#include <stdio.h>
#include <stdint.h>
#include "runtime.h"
#include "nvn_c5.h"

int nvn_c5_selftest(Ctx *c) {
    static const struct { uint32_t tex, smp; uint64_t want[5]; } t[] = {      /* want: texture, texel fetch, image, separate texture, separate sampler */
        {0x0u, 0x0u, {0x0000000100000000ull, 0x0000000100000000ull, 0x0000000000000000ull, 0x0000000100000000ull, 0x0000000000000000ull}},
        {0x1u, 0x0u, {0x0000000100000001ull, 0x0000000100000001ull, 0x0000000000000001ull, 0x0000000100000001ull, 0x0000000000000000ull}},
        {0x7u, 0x5u, {0x0000000100500007ull, 0x0000000100000007ull, 0x0000000000000007ull, 0x0000000100000007ull, 0x0000000000500000ull}},
        {0x123u, 0x45u, {0x0000000104500123ull, 0x0000000100000123ull, 0x0000000000000123ull, 0x0000000100000123ull, 0x0000000004500000ull}},
        {0xfffffu, 0xfffu, {0xffffffffffffffffull, 0x00000001000fffffull, 0x00000000000fffffull, 0x00000001000fffffull, 0x00000000fff00000ull}},
        {0x100000u, 0x1u, {0x0000000100100000ull, 0x0000000100100000ull, 0x0000000000100000ull, 0x0000000100100000ull, 0x0000000000100000ull}},
        {0x80000u, 0x800u, {0xffffffff80080000ull, 0x0000000100080000ull, 0x0000000000080000ull, 0x0000000100080000ull, 0x0000000080000000ull}},
        {0x100u, 0x100u, {0x0000000110000100ull, 0x0000000100000100ull, 0x0000000000000100ull, 0x0000000100000100ull, 0x0000000010000000ull}},
        {0x7fffffffu, 0x0u, {0x000000017fffffffull, 0x000000017fffffffull, 0x000000007fffffffull, 0x000000017fffffffull, 0x0000000000000000ull}},
        {0x80000000u, 0x0u, {0xffffffff80000000ull, 0xffffffff80000000ull, 0xffffffff80000000ull, 0xffffffff80000000ull, 0x0000000000000000ull}},
        {0x7ffu, 0x7ffu, {0x000000017ff007ffull, 0x00000001000007ffull, 0x00000000000007ffull, 0x00000001000007ffull, 0x000000007ff00000ull}},
        {0x12345u, 0xfffu, {0xfffffffffff12345ull, 0x0000000100012345ull, 0x0000000000012345ull, 0x0000000100012345ull, 0x00000000fff00000ull}},
    };
    static const char *const name[5] = {"nvnDeviceGetTextureHandle", "nvnDeviceGetTexelFetchHandle", "nvnDeviceGetImageHandle", "nvnDeviceGetSeparateTextureHandle", "nvnDeviceGetSeparateSamplerHandle"};
    int bad = 0, n = 0;
    for (size_t i = 0; i < sizeof t / sizeof t[0]; i++) for (int k = 0; k < 5; k++) {
        ImportFn fn = nvn_gl_pick(name[k]);
        uint64_t got = 0;
        n++;
        if (fn) {
            c->x[0] = 0x1000;                                           /* the device: not read by any of the five */
            c->x[1] = 0xabcdef0000000000ull | (k == 4 ? t[i].smp : t[i].tex);
            c->x[2] = 0xabcdef0000000000ull | t[i].smp;
            fn(c);
            got = c->x[0];
        }
        if (!fn || got != t[i].want[k]) { bad++; printf("  FAIL %s(texture 0x%x, sampler 0x%x) = 0x%llx, the SDK's code gives 0x%llx\n", name[k], t[i].tex, t[i].smp, (unsigned long long)got, (unsigned long long)t[i].want[k]); }
    }
    printf("  nvn_c5: %d handle cases, %d failed\n", n, bad);
    return bad;
}
