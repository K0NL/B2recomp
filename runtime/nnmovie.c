/* movie:: (the SDK's video player library): no video decoding yet.
 *
 * Extractor and Decoder are C++ objects the game drives through their virtual tables. Here they get
 * a table whose every entry reports failure, so the game's movie player gives up on the film instead
 * of jumping through an uninitialised pointer. The opening movie is therefore skipped.
 * Every virtual call is logged once with its slot number, which is what a real implementation
 * (for example on top of Media Foundation) will need. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include "runtime.h"
#include "hostrt.h"

#define A0 (c->x[0])
#define RET(v) (c->x[0] = (uint64_t)(v))
#define SLOTS 64
#define STATUS_FAILED 0x4000u                     /* the code the game itself uses when it cannot create the player */

static uint64_t g_extractor_vtable[SLOTS], g_decoder_vtable[SLOTS];
static char g_names[2][SLOTS][40];
static unsigned char g_seen[2][SLOTS];

static void virtual_call(Ctx *c) {
    const char *name = hostfn_current_name();
    for (int k = 0; k < 2; k++)
        for (int i = 0; i < SLOTS; i++)
            if (name && strcmp(name, g_names[k][i]) == 0 && !g_seen[k][i]) {
                g_seen[k][i] = 1;
                printf("movie: %s called (x1=%llx x2=%llx) -> failure\n", name, (unsigned long long)c->x[1], (unsigned long long)c->x[2]);
                for (int r = 1; r <= 2; r++) {                    /* show arguments that are readable strings (file paths) */
                    MEMORY_BASIC_INFORMATION mi;
                    const char *s = (const char *)(uintptr_t)c->x[r];
                    if (c->x[r] > 0x10000 && VirtualQuery(s, &mi, sizeof mi) && mi.State == MEM_COMMIT &&
                        (const char *)mi.BaseAddress + mi.RegionSize - s >= 64 && s[0] >= 0x20 && s[0] < 0x7f)
                        printf("movie:   x%d -> \"%.60s\"\n", r, s);
                }
            }
    RET(STATUS_FAILED);
}

static void build(int kind, const char *what, uint64_t *table) {
    for (int i = 0; i < SLOTS; i++) {
        snprintf(g_names[kind][i], sizeof g_names[kind][i], "movie::%s vtable+0x%x", what, i * 8);
        table[i] = hostfn_address(g_names[kind][i], virtual_call);
    }
}

static void extractor_ctor(Ctx *c) {
    if (!g_extractor_vtable[0]) build(0, "Extractor", g_extractor_vtable);
    printf("movie: Extractor created (container type %u)\n", (unsigned)c->x[1]);
    wr64(A0, (uint64_t)(uintptr_t)g_extractor_vtable);
    wr64(A0 + 8, 0);
}

static void decoder_ctor(Ctx *c) {
    if (!g_decoder_vtable[0]) build(1, "Decoder", g_decoder_vtable);
    printf("movie: Decoder created (type %u)\n", (unsigned)c->x[1]);
    wr64(A0, (uint64_t)(uintptr_t)g_decoder_vtable);
}

static void nothing(Ctx *c) { (void)c; }
static void ret0(Ctx *c) { RET(0); }

static const ImportDef movie_imports[] = {
    {"_ZN5movie9ExtractorC1ENS_13ContainerTypeE", extractor_ctor},
    {"_ZN5movie7DecoderC1ENS_11DecoderTypeENS_19DecoderOutputFormatENS_11DecoderModeEi", decoder_ctor},
    {"_ZN5movie12SetAllocatorEPFPvmmS0_EPFvS0_S0_EPFS0_S0_mS0_ES0_", nothing}, {"_ZN5movie11SetCoreMaskEm", nothing},
    {"_ZN5movie17SetThreadPriorityEi", nothing}, {"_ZN5movie17GetThreadPriorityEv", ret0},
};

void register_movie_imports(void) {
    register_imports(movie_imports, (int)(sizeof movie_imports / sizeof movie_imports[0]));
}
