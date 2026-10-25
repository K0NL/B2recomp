/* [Recomps#1] Darwin only: the <io.h> names the runtime uses (nnlibc2_c5b.inc: truncate an open file). */
#ifndef RECOMPS_DARWIN_IO_H
#define RECOMPS_DARWIN_IO_H
#include <windows.h>
#include <stdio.h>
#define _fileno fileno
static inline intptr_t _get_osfhandle(int fd) { return ((intptr_t)fd << 2) | 3; }   /* a file handle of windows.h (win32_compat.c) */
#endif
