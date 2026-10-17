/* Minimal stand-ins for the runtime entry points the lifted main calls, so it can be linked into a test DLL.
 * Nothing here is real behaviour: each one reports and aborts. The real versions come from the runtime (Step 3). */
#include <stdio.h>
#include <stdlib.h>
#include "runtime.h"

static void die(const char *what, uint64_t a, uint64_t b) {
    fprintf(stderr, "[rt_stubs] %s 0x%llx 0x%llx: no runtime in this build\n", what, (unsigned long long)a, (unsigned long long)b);
    abort();
}

void call_indirect(Ctx *c, uint64_t addr) { die("call_indirect", addr, c->fn); }
void host_import(Ctx *c, int sym) { die("host_import", (uint64_t)sym, c->fn); }
void guest_trap(const char *what, uint64_t addr) { die(what, addr, 0); }
void guest_unimpl(uint64_t pc, uint32_t insn, const char *what) { (void)what; die("guest_unimpl", pc, insn); }
void guest_svc(Ctx *c, uint32_t number) { die("guest_svc", number, c->fn); }
void stx_diag(Ctx *c, uint64_t a, uint64_t expected, uint64_t seen, uint64_t v, int bits) {
    (void)c; (void)expected; (void)seen; (void)v; (void)bits; /* a failed exclusive store is normal: the guest retries */
    (void)a;
}
void nro_catch_call(Ctx *c, void (*body)(Ctx *, uint64_t)) { body(c, 0); }
