/* NVN functions answered by the SDK's own lifted code: see nvn_sdk.c. */
#ifndef NVN_SDK_H
#define NVN_SDK_H
#include "runtime.h"

#define NVN_SDK_FORMAT_COUNT 0xa1u        /* records 0 .. 0xa0 of the format table: [sdk 0x923078] in our NVN 3.6.0 (0x34-byte records); Smash: 0xc26568 */

typedef struct {
    uint32_t flags, target, width, height, depth, levels, format, samples, stride, glname;
    uint32_t has_layout, layout;          /* nvnTextureBuilderSetPackagedTextureLayout was called; first word of the layout */
} NvnSdkTexBuilder;

uint64_t nvn_sdk_texture_storage_alignment(Ctx *c, const NvnSdkTexBuilder *b);
uint64_t nvn_sdk_texture_storage_size(Ctx *c, const NvnSdkTexBuilder *b);
uint32_t nvn_sdk_texture_storage_class(Ctx *c, const NvnSdkTexBuilder *b);     /* 0x100 | GPU memory kind */
/* close4: storage class and sparse tile layout, builder and texture forms (see nvn_sdk.c). The texture forms return 0 when the
 * texture cannot be modelled (made from an OpenGL texture name), 1 otherwise. NVNtextureSparseTileLayout = 4 words. */
uint32_t nvn_sdk_texture_raw_storage_class(Ctx *c, const NvnSdkTexBuilder *b);
void nvn_sdk_texture_sparse_layout(Ctx *c, const NvnSdkTexBuilder *b, uint32_t out[4]);
int nvn_sdk_tex_storage_class(Ctx *c, const NvnSdkTexBuilder *b, uint32_t *out);
int nvn_sdk_tex_raw_storage_class(Ctx *c, const NvnSdkTexBuilder *b, uint32_t *out);
int nvn_sdk_tex_sparse_layout(Ctx *c, const NvnSdkTexBuilder *b, uint32_t out[4]);
int nvn_sdk_tex_storage_size(Ctx *c, const NvnSdkTexBuilder *b, uint32_t *out);
int nvn_sdk_format_known(uint32_t format);
int nvn_sdk_selftest(Ctx *c);
#endif
