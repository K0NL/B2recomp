/* [Recomps#1] R0: stand-ins for the functions of the Smash-only files removed from this copy of the runtime:
 *   nnro.c (+ fighter_sdk/*_host.c)  nn::ro run-time modules (lua2cpp scripts); our main imports no nn::ro
 *   r1_state.c                       save/restore of Smash's simulation state (run-ahead, rollback; test only)
 *   debug_mode.c                     Smash battle test mode
 * Each returns what its caller gets when the feature is absent or switched off, as upstream does without
 * the corresponding SSBU_* variable. Declarations copied from the callers / upstream definitions at 3a96916. */
#include <stdio.h>
#include <stddef.h>
#include "runtime.h"

/* ---- nnro.c: no run-time modules exist */
int nro_call_indirect(Ctx *c, uint64_t addr) { (void)c; (void)addr; return 0; }         /* 0 = not a module address */
uint64_t nro_guest_function_of(uint64_t rip) { (void)rip; return 0; }                    /* 0 = unknown */
void register_ro_imports(void) {}
void module_coverage_start(Ctx *main_ctx) { (void)main_ctx; }
void module_coverage_report(FILE *out) { (void)out; }
int nnro_selftest(Ctx *c) { (void)c; return 0; }                                         /* 0 failures */
typedef struct { uint64_t lp; void (*fn)(Ctx *, uint64_t); } NroResume;
int nro_unwind_module(uint64_t pc, uint64_t *base, uint64_t *size, const NroResume **tab, int *count, const char **name) {
    (void)pc; (void)base; (void)size; (void)tab; (void)count; (void)name;
    return 0;                                                                              /* not in a module: main's own tables */
}
uint64_t hook_address(const char *name) { (void)name; return 0; }                       /* fighter_sdk symbol lookup: none */

/* ---- r1_state.c: state save/restore never active (upstream: only with SSBU_R1) */
int r1_on(void) { return 0; }
void r1_init(void) {}
void r1_register_heap(void) {}
int r1_poll(int ticks) { (void)ticks; return 0; }
void r1_pad(uint64_t *b, int *lx, int *ly, int *rx, int *ry) { (void)b; (void)lx; (void)ly; (void)rx; (void)ry; }
int r1_ra_phase(void) { return 0; }
int r1_ra_quiet(void) { return 0; }
int r1_ra_present_mode(void) { return 0; }
void r1_on_commit(void *p, size_t n) { (void)p; (void)n; }
void r1_on_decommit(void *p, size_t n) { (void)p; (void)n; }
void r1_wrote(uint64_t a, uint64_t n) { (void)a; (void)n; }
void r1_nosim(uint64_t a, uint64_t n) { (void)a; (void)n; }

/* ---- debug_mode.c / fighter_sdk: off */
void debug_mode_init(void) {}
void debug_mode_frame(long long frame) { (void)frame; }
int fight_seq_ticks(void) { return 1; }            /* upstream default without the fighter tools: one tick per poll (nnhid.c) */
