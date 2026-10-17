/* Code of the real SDK module (binaries\13.0.4\sdk.elf) lifted to C by sdklift.py, and the image it runs on.
 * See sdklift.py for how calls out of the lifted code are bound, sdklift_rt.c for the loader. */
#ifndef SDKLIFT_H
#define SDKLIFT_H

#include <setjmp.h>
#include "runtime.h"
#include "hostrt.h"

#define SDK_BASE 0x7500000000ULL          /* where the SDK image is mapped (text, rodata, data, bss) */
#include "image_layout.h"
#define SDK_IMAGE_SIZE SDK_IMAGE_SPAN     /* [Recomps#1] our sdk.elf, from image_layout.h */

typedef struct { uint64_t addr; void (*fn)(Ctx *); const char *name; } SdkFn;
extern const SdkFn sdk_fns[];             /* sorted by address */
extern const int sdk_fn_count;
extern const char *const sdk_host_names[];
extern const int sdk_host_name_count;

int sdk_image_ready(void);                /* 1 once sdk.elf is mapped and relocated */
ImportFn sdk_lifted(const char *name);    /* lifted function of that symbol, NULL if it was not lifted */
uint64_t sdk_symbol(const char *name);    /* mapped address of an SDK symbol (function or data), 0 if unknown */
void sdk_host_call(Ctx *c, int index);    /* lifted code -> host handler sdk_host_names[index] */
void sdk_trap_call(Ctx *c, const char *name);
void sdk_dispatch(Ctx *c, uint64_t addr); /* indirect call made by lifted SDK code */
void sdk_host_override(const char *name, ImportFn fn);   /* host handler for a cut SDK-internal callee */
int sdk_install(const char *name);        /* make the lifted code the handler of this import (replaces); 0 if not lifted */
int sdk_missing_host_callees(FILE *out);  /* host callees of lifted code that have no handler (self-test) */
void register_sdklift_imports(void);      /* maps the image; every lifted root that is a handler-less import gets its lifted code */

/* setjmp / longjmp inside lifted SDK code (libjpeg's error exit). The lifted function that contains the
 * `bl setjmp` is an ordinary host function that stays on the host stack until the guest function returns,
 * so the host's own setjmp can be used in place: the macro is expanded inside that function. The guest
 * callee-saved registers and sp are saved next to the host buffer and restored when a longjmp lands. */
typedef struct {
    jmp_buf host;
    uint64_t key;                         /* address of the guest jmp_buf */
    uint64_t x[13];                       /* x19..x30, sp */
    uint64_t d[8];                        /* d8..d15 */
    int value;
    unsigned long long stamp;
} SdkJmpRec;
SdkJmpRec *sdk_jmp_record(Ctx *c);        /* x0 = guest jmp_buf */
void sdk_jmp_restore(Ctx *c, SdkJmpRec *r);
void sdk_longjmp(Ctx *c);                 /* x0 = guest jmp_buf, w1 = value; does not return */
#define SDK_SETJMP(c) do { SdkJmpRec *j_ = sdk_jmp_record(c); if (setjmp(j_->host)) sdk_jmp_restore((c), j_); else (c)->x[0] = 0; } while (0)

#endif
