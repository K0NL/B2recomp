/* Maxwell (Tegra X1 GPU) shader machine code -> GLSL 4.60 (NVIDIA, compatibility profile, ARB_bindless_texture).
 * Used by nvn_gl.c at run time and by mxtest.exe offline. Semantics follow the suyu shader recompiler
 * (Documents/SSBU/reference/suyu/src/shader_recompiler, read as a reference; this is independent C).
 *
 * Conventions shared with nvn_gl.c
 *   - Hardware constant buffer n is a storage block "uint cN[]" at binding in->binding_base + n.
 *     NVN: c1 = immediate constants stored after the shader's code (also BRX jump tables), c2 = the texture
 *     handle table, c3 + k = uniform buffer k bound by the game.
 *     c0 is the driver's own buffer: words (0x110 + 0x100 * hardware stage) / 4 + 4k hold the descriptor
 *     {address, size} of storage buffer k; the translator replaces them by tagged addresses and turns
 *     LDG / STG into accesses of storage block "uint sK[]" at binding in->ssbo_base + k. (Layout checked
 *     against the game's own NV assembly shaders: fragment 0x590 / 0x5e0 <-> program.storage[8] / [13].)
 *   - Texture handle table (c2, 32-bit words): words 0..7 unused; word 8 + 2u, 9 + 2u = 64-bit GL bindless
 *     handle of texture unit u (u < 32); words 72 + 2k = NVN image handle k (k < 8; images are bound to image
 *     units, the words are not read); words 96 + 4u .. 99 + 4u = width, height, depth or layers, levels of
 *     unit u (for TXQ). A texture instruction's handle offset (bits 36..48, in words) selects the unit.
 *   - Compute: storage buffer k is described at c0 byte 0x310 + 16k (checked against the game's NV assembly
 *     compute programs: program.storage[0], [2], [8] <-> 0x310, 0x330, 0x390). Uniform buffer n is described
 *     at c0 byte 0x210 + 16n and read with LDG when it has no constant buffer slot; the translator maps that
 *     to the same block c(3 + n). Local memory (LDL / STL) is a
 *     per-invocation array, shared memory (LDS / STS) a "shared" array of the work group. An image access
 *     (SULD / SUST) with handle offset 72 + 2k uses image unit in->image_base + k.
 *   - Vertex inputs/outputs and fragment inputs: generic attribute n at location n (vec4; integer vertex
 *     formats as ivec4/uvec4 when flagged in int_attrs / sint_attrs).
 *   - Fragment output render target n at location n. Alpha test: uniform uvec2 at location 0 =
 *     (NVN compare function 1..8, reference as float bits); function 0 or 8 = always pass. */
#ifndef MX_TRANSLATE_H
#define MX_TRANSLATE_H
#include <stdint.h>

enum { MX_VERTEX = 0, MX_FRAGMENT = 1, MX_GEOMETRY = 2, MX_TESS_CONTROL = 3, MX_TESS_EVAL = 4, MX_COMPUTE = 5 };

typedef struct {
    int stage;                       /* MX_* */
    const uint8_t *code;             /* program header (0x50 bytes) followed by instructions; a compute stage has no header:
                                        its code section starts with the first instruction group */
    int code_len;                    /* bytes from the start of the header (compute: of the instructions) */
    const uint8_t *cdata;            /* contents of constant buffer 1 (for jump tables), may be NULL */
    uint32_t cdata_size;
    uint32_t int_attrs, sint_attrs;  /* vertex: generic inputs with an integer format (and which of them are signed) */
    uint32_t int_tex, sint_tex;      /* texture units holding integer formats (and which are signed) */
    unsigned binding_base;           /* storage binding of constant buffer 0 */
    unsigned ssbo_base;              /* storage binding of the game's storage buffer 0 of this stage */
    int fragdebug;                   /* fragment: write magenta */
    /* compute stage (NVN control section: +0x748 work group size x, y, z; +0x754 shared memory bytes; +0x758 local memory bytes) */
    uint32_t local_size[3], smem_bytes, lmem_bytes;
    unsigned image_base;             /* image unit of the stage's image 0 (nvnCommandBufferBindImage index) */
    uint16_t image_format[8];        /* OpenGL internal format of the texture bound as image k, 0 = unknown (needed by typeless image loads) */
    int track_writes;                /* storage block "uint mx_wm" at binding wmask_binding gets bit k set when storage buffer k is written */
    unsigned wmask_binding;
    /* V1 (plan\phases\V1_vulkan.md, section 4). target 0 = OpenGL text exactly as before. target 1 = Vulkan GLSL (for glslang):
     * "#version 460", every block and image with "set = 0" (buffers: the binding numbers above) or "set = 1" (images, textures);
     * texture unit u is a named sampler "uniform <type> mx_t<u>_<type>" at set 1, binding tex_base + u (a second sampler type on
     * the same unit: tex_base + 240 + u; a third fails with its name) instead of a bindless handle read from c2; the alpha
     * test operands and the point size come from the push-constant block MXPC { uvec2 mx_alpha; float mx_point_size; };
     * gl_VertexIndex / gl_InstanceIndex - gl_BaseInstance; gl_PointCoord.y flipped (the viewport is flipped by the back end);
     * gl_PointSize is always the push constant's (the OpenGL path never enables program point size). */
    int target;
    unsigned tex_base;
    /* vertex stage: generic outputs to declare (and leave at (0, 0, 0, 1)) although the code never writes them: the inputs
     * of the fragment stage it is paired with. Vulkan requires every fragment input to have a vertex output
     * (VUID-RuntimeSpirv-OpEntryPoint-08743); in OpenGL such an input is undefined. */
    uint32_t force_out;
    /* Track O, internal resolution scale (plan\phases\O1_options.md, session 7). 0 = the text exactly as before. 1 (fragment
     * stage, target 1 only): gl_FragCoord.xy is multiplied by the push constant mx_fcs (a fourth member of MXPC: game pixels
     * per real pixel of the pass, 1.0 on a target that is not scaled), and the coordinates of a texelFetch from a 2D sampler
     * are mapped from the game's texel grid (the size table in c2) to the image's real one (textureSize). */
    int screen_scale;
} MxIn;

typedef struct {
    char *glsl;                      /* malloc'ed; NULL on failure */
    char why[200];                   /* failure reason */
    char op[40];                     /* failure: name of the instruction (or feature) that could not be translated */
    uint32_t cbufs;                  /* constant buffers referenced */
    uint32_t in_attr, out_attr;      /* generic attributes read / written */
    uint32_t tex_units;              /* texture units referenced (bound handles) */
    uint32_t ssbos, ssbo_written;    /* game storage buffers referenced (LDG / STG) and whether the shader writes them */
    uint32_t images, images_read, images_written;      /* image units used (SULD / SUST) */
    uint16_t image_gl_format[8];     /* internal format each used image must be bound with (typed accesses name their own) */
    uint32_t int_dep;                /* which bits of int_attrs / int_tex influenced the code (vertex: attrs, fragment: textures) */
    int insns, blocks, uses_alpha;
    /* target 1: the samplers the text declares in set 1 (the back end binds a view of that type, or a dummy of that type) */
    struct { uint8_t unit, shadow; uint16_t binding; char type[24]; } smp[64];
    int nsmp;
} MxOut;

/* Returns 1 on success. out->glsl must be freed by the caller. */
int mx_translate(const MxIn *in, MxOut *out);
/* Name of the Maxwell opcode of an instruction word (suyu's opcode table). */
const char *mx_opcode_name(uint64_t insn);

#endif
