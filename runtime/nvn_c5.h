/* NVN, round "close5": see nvn_c5.c. */
#ifndef NVN_C5_H
#define NVN_C5_H
#include "runtime.h"
#include "hostrt.h"
ImportFn nvn_gl_pick(const char *name);         /* nvn_gl.c */
/* Part of the SDK self-test: the five handle functions against the values the SDK's code gives (emulator on sdk.elf). */
int nvn_c5_selftest(Ctx *c);
#endif
