/* Loader and glue for the code lifted out of the real SDK module (sdklift_gen.c, made by sdklift.py).
 *
 * The SDK image (binaries\13.0.4\sdk.elf, the user's own dump converted with nso2elf) is mapped at
 * SDK_BASE with its three PT_LOAD segments and relocated like the game image, so the lifted functions
 * find the SDK's own tables and globals at the addresses their code computes. The SDK's static
 * constructors are NOT run and no SDK service session exists: only functions that are self-contained
 * (accessors, table getters, codecs, struct setters) are lifted; whatever needs a system service is
 * answered by hand in the nn*.c files.
 *
 * Relocations ([sdk] .rela.dyn 0x541010, .rela.plt 0x5ce118): R_AARCH64_RELATIVE (0x403),
 * GLOB_DAT (0x401), JUMP_SLOT (0x402), ABS64 (0x101). The 13 undefined symbols (malloc, free, calloc,
 * realloc, aligned_alloc, nnMain, ...) are left 0: lifted code never calls through the GOT, sdklift.py
 * binds PLT stubs statically (to lifted code or to the host handler of the same name). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include "sdklift.h"

static int g_ready;
static uint8_t *g_file;                   /* sdk.elf as read from disk */
static size_t g_file_size;
static uint64_t g_symtab, g_strtab, g_nsym;
static ImportFn *g_host_fn;               /* per sdk_host_names entry */

int sdk_image_ready(void) { return g_ready; }

static void die(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    printf("\nsdklift: ");
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
    report_and_exit(3);
}

static int load_image(void) {
    char path[1024];
    const char *env = getenv("SSBU_SDK_ELF");
    const char *g_elf = __argc > 1 ? __argv[1] : NULL;       /* same argument boot.c uses for main.elf */
    if (env && env[0]) snprintf(path, sizeof path, "%s", env);
    else {
        snprintf(path, sizeof path, "%s", g_elf ? g_elf : "main.elf");        /* [Recomps#1] */
        char *slash = strrchr(path, '\\'), *slash2 = strrchr(path, '/');
        if (slash2 > slash) slash = slash2;
        snprintf(slash ? slash + 1 : path, 16, "sdk.elf");
    }
    FILE *f = fopen(path, "rb");
    if (!f) { printf("sdklift: cannot open %s (set SSBU_SDK_ELF): lifted SDK functions are unavailable\n", path); return 0; }
    fseek(f, 0, SEEK_END);
    g_file_size = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    g_file = malloc(g_file_size);
    if (fread(g_file, 1, g_file_size, f) != g_file_size || memcmp(g_file, "\x7f" "ELF", 4)) { fclose(f); printf("sdklift: %s is not readable as an ELF\n", path); return 0; }
    fclose(f);
    uint8_t *img = VirtualAlloc((void *)(uintptr_t)SDK_BASE, SDK_IMAGE_SIZE, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (img != (uint8_t *)(uintptr_t)SDK_BASE) { printf("sdklift: cannot map the SDK image at %llx\n", (unsigned long long)SDK_BASE); return 0; }
    uint64_t phoff = *(uint64_t *)(g_file + 0x20);
    int phnum = *(uint16_t *)(g_file + 0x38);
    uint64_t dynoff = 0;
    for (int i = 0; i < phnum; i++) {
        uint8_t *ph = g_file + phoff + (uint64_t)i * 56;
        uint32_t type = *(uint32_t *)ph;
        uint64_t off = *(uint64_t *)(ph + 8), va = *(uint64_t *)(ph + 16), filesz = *(uint64_t *)(ph + 32), memsz = *(uint64_t *)(ph + 40);
        if (type == 1) {
            if (va + memsz > SDK_IMAGE_SIZE || off + filesz > g_file_size) die("segment %d of sdk.elf does not fit", i);
            memcpy(img + va, g_file + off, filesz);
        } else if (type == 2) dynoff = va;
    }
    if (!dynoff) die("sdk.elf has no dynamic section");
    uint64_t rela = 0, relasz = 0, jmprel = 0, pltrelsz = 0, hash = 0;
    for (uint64_t *dp = (uint64_t *)(img + dynoff); dp[0]; dp += 2) {
        switch (dp[0]) {
        case 7: rela = dp[1]; break;
        case 8: relasz = dp[1]; break;
        case 23: jmprel = dp[1]; break;
        case 2: pltrelsz = dp[1]; break;
        case 6: g_symtab = dp[1]; break;
        case 5: g_strtab = dp[1]; break;
        case 4: hash = dp[1]; break;
        }
    }
    g_nsym = hash ? *(uint32_t *)(img + hash + 4) : 0;       /* DT_HASH nchain = number of symbols */
    if (!g_nsym) die("sdk.elf: no DT_HASH");
    uint64_t tables[2][2] = {{rela, relasz}, {jmprel, pltrelsz}};
    long counts[4] = {0, 0, 0, 0};
    for (int t = 0; t < 2; t++)
        for (uint64_t i = 0; i < tables[t][1] / 24; i++) {
            uint64_t *r = (uint64_t *)(img + tables[t][0] + i * 24);
            uint64_t off = r[0], info = r[1], addend = r[2];
            uint32_t type = (uint32_t)info, sym = (uint32_t)(info >> 32);
            if (off + 8 > SDK_IMAGE_SIZE) die("relocation outside the image");
            if (type == 0x403) { wr64(SDK_BASE + off, SDK_BASE + addend); counts[0]++; continue; }
            if (type != 0x401 && type != 0x402 && type != 0x101) die("unknown relocation type %x", type);
            uint8_t *s = img + g_symtab + (uint64_t)sym * 24;
            uint16_t shndx = *(uint16_t *)(s + 6);
            uint64_t value = *(uint64_t *)(s + 8);
            if (shndx) { wr64(SDK_BASE + off, SDK_BASE + value + (type == 0x101 ? addend : 0)); counts[1]++; }
            else { wr64(SDK_BASE + off, 0); counts[2]++; }
        }
    trace_printf("sdklift: SDK image mapped at %llx (%lu relative, %lu symbolic, %lu undefined relocations)\n",
                 (unsigned long long)SDK_BASE, counts[0], counts[1], counts[2]);
    return 1;
}

uint64_t sdk_symbol(const char *name) {
    if (!g_ready) return 0;
    uint8_t *img = (uint8_t *)(uintptr_t)SDK_BASE;
    for (uint64_t i = 0; i < g_nsym; i++) {
        uint8_t *s = img + g_symtab + i * 24;
        if (*(uint16_t *)(s + 6) && strcmp((const char *)img + g_strtab + *(uint32_t *)s, name) == 0) return SDK_BASE + *(uint64_t *)(s + 8);
    }
    return 0;
}

static const SdkFn *find_fn(uint64_t addr) {
    int lo = 0, hi = sdk_fn_count - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (sdk_fns[mid].addr == addr) return &sdk_fns[mid];
        if (sdk_fns[mid].addr < addr) lo = mid + 1; else hi = mid - 1;
    }
    return NULL;
}

ImportFn sdk_lifted(const char *name) {
    if (!g_ready) return NULL;
    uint64_t a = sdk_symbol(name);
    const SdkFn *f = a ? find_fn(a) : NULL;
    return f ? f->fn : NULL;
}

/* Name of the SDK symbol that contains `addr`, for diagnostics. */
static const char *sdk_name_of(uint64_t addr, uint64_t *offset) {
    uint8_t *img = (uint8_t *)(uintptr_t)SDK_BASE;
    const char *best = "?";
    uint64_t best_off = ~0ull;
    for (uint64_t i = 0; i < g_nsym; i++) {
        uint8_t *s = img + g_symtab + i * 24;
        uint64_t v = SDK_BASE + *(uint64_t *)(s + 8);
        if (*(uint16_t *)(s + 6) && v <= addr && addr - v < best_off) { best_off = addr - v; best = (const char *)img + g_strtab + *(uint32_t *)s; }
    }
    *offset = best_off;
    return best;
}

void sdk_dispatch(Ctx *c, uint64_t addr) {
    if (addr >= SDK_BASE && addr < SDK_BASE + SDK_IMAGE_SIZE) {
        const SdkFn *f = find_fn(addr);
        if (!f) {
            uint64_t off;
            const char *n = sdk_name_of(addr, &off);
            printf("\nsdklift: lifted SDK code (f_%llx) calls SDK address %llx (%s+0x%llx) through a pointer, and that function was not lifted.\n"
                   "Add `addr %llx` to sdklift_roots.txt and run `python sdklift.py gen`.\n",
                   (unsigned long long)c->fn, (unsigned long long)(addr - SDK_BASE), n, (unsigned long long)off, (unsigned long long)(addr - SDK_BASE));
            guest_trap("indirect call to an SDK function that was not lifted", addr);
        }
        f->fn(c);
        return;
    }
    call_indirect(c, addr);
}

/* Host handlers for SDK-internal callees that are cut (`cut <name>` in sdklift_roots.txt) and are not
 * imports of the game: a service call inside otherwise pure SDK code. */
static struct { const char *name; ImportFn fn; } g_override[32];
static int g_override_count;
void sdk_host_override(const char *name, ImportFn fn) {
    if (g_override_count < 32) { g_override[g_override_count].name = name; g_override[g_override_count].fn = fn; g_override_count++; }
    for (int i = 0; g_host_fn && i < sdk_host_name_count; i++) if (strcmp(sdk_host_names[i], name) == 0) g_host_fn[i] = fn;
}

/* Address of a function the game itself defines (the SDK module imports malloc, free, calloc, realloc and
 * aligned_alloc from the application). */
static uint64_t game_symbol(const char *name) {
    for (int i = 0; i < dyn_sym_count; i++)
        if (dyn_syms[i].defined && strcmp(dyn_syms[i].name, name) == 0) return GUEST_BASE + dyn_syms[i].value;
    return 0;
}

void sdk_host_call(Ctx *c, int index) {
    ImportFn f = g_host_fn[index];
    if (!f) {
        const char *name = sdk_host_names[index];
        for (int i = 0; i < g_override_count && !f; i++) if (strcmp(g_override[i].name, name) == 0) f = g_override[i].fn;
        if (!f) f = import_lookup(name);
        if (!f) {                                           /* malloc & co: functions the GAME defines and the SDK imports */
            uint64_t g = game_symbol(name);
            if (g) { call_indirect(c, g); return; }
        }
        if (!f) {
            printf("\nsdklift: lifted SDK code (f_%llx) calls %s, which has no host handler.\n", (unsigned long long)c->fn, name);
            guest_trap("lifted SDK code called a function without a host handler", c->x[30] - 4);
        }
        g_host_fn[index] = f;
    }
    f(c);
}

void sdk_trap_call(Ctx *c, const char *name) {
    printf("\nsdklift: lifted SDK code (f_%llx, called from guest %llx) reached %s: an abort path or a cut service boundary.\n",
           (unsigned long long)c->fn, (unsigned long long)c->x[30] - 4, name);
    trace_printf("sdklift: lifted SDK code f_%llx reached %s\n", (unsigned long long)c->fn, name);
    guest_trap("lifted SDK code reached an abort path", c->x[30] - 4);
}

/* ---------------------------------------------------------------- setjmp / longjmp (see sdklift.h) */
#define SDK_JMP_SLOTS 8
static _Thread_local SdkJmpRec *t_jmp;
static _Thread_local unsigned long long t_jmp_stamp;

SdkJmpRec *sdk_jmp_record(Ctx *c) {
    if (!t_jmp) t_jmp = _aligned_malloc(sizeof(SdkJmpRec) * SDK_JMP_SLOTS, 16), memset(t_jmp, 0, sizeof(SdkJmpRec) * SDK_JMP_SLOTS);
    uint64_t key = c->x[0];
    SdkJmpRec *r = NULL;
    for (int i = 0; i < SDK_JMP_SLOTS; i++) if (t_jmp[i].key == key) r = &t_jmp[i];
    if (!r) {                                               /* the least recently set record is the one to reuse */
        r = &t_jmp[0];
        for (int i = 1; i < SDK_JMP_SLOTS; i++) if (t_jmp[i].stamp < r->stamp) r = &t_jmp[i];
    }
    r->key = key;
    r->stamp = ++t_jmp_stamp;
    for (int i = 0; i < 12; i++) r->x[i] = c->x[19 + i];
    r->x[12] = c->x[31];
    for (int i = 0; i < 8; i++) r->d[i] = c->v[8 + i].d[0];
    return r;
}
void sdk_jmp_restore(Ctx *c, SdkJmpRec *r) {
    for (int i = 0; i < 12; i++) c->x[19 + i] = r->x[i];
    c->x[31] = r->x[12];
    for (int i = 0; i < 8; i++) { c->v[8 + i].d[0] = r->d[i]; c->v[8 + i].d[1] = 0; }
    c->x[0] = (uint64_t)(r->value ? r->value : 1);
}
void sdk_longjmp(Ctx *c) {
    SdkJmpRec *r = NULL;
    for (int i = 0; t_jmp && i < SDK_JMP_SLOTS; i++) if (t_jmp[i].key == c->x[0] && t_jmp[i].stamp) r = &t_jmp[i];
    if (!r) {
        printf("\nsdklift: longjmp to a jmp_buf (%llx) no lifted setjmp filled on this thread\n", (unsigned long long)c->x[0]);
        guest_trap("lifted SDK longjmp without setjmp", c->x[30] - 4);
    }
    r->value = (int)c->x[1];
    longjmp(r->host, 1);
}

/* ---------------------------------------------------------------- registration
 * Every import of the game that still has no handler and whose SDK implementation was lifted gets the
 * lifted code as its handler (looked up by symbol, so aliases such as C1/C2 work). Handlers that
 * already exist are only replaced where an nn*.c file asks for it with sdk_install(). */
int sdk_install(const char *name) {
    ImportFn f = sdk_lifted(name);
    if (!f || !import_data(name)) return 0;
    ImportDef d = {name, f};
    register_imports(&d, 1);
    return 1;
}

int sdk_missing_host_callees(FILE *out) {
    int missing = 0;
    for (int i = 0; i < sdk_host_name_count; i++) {
        int found = import_lookup(sdk_host_names[i]) != NULL || game_symbol(sdk_host_names[i]) != 0;
        for (int k = 0; k < g_override_count; k++) if (strcmp(g_override[k].name, sdk_host_names[i]) == 0) found = 1;
        if (!found) { if (out) fprintf(out, "  lifted SDK code needs a host handler for %s\n", sdk_host_names[i]); missing++; }
    }
    return missing;
}

void register_sdklift_imports(void) {
    g_host_fn = calloc((size_t)sdk_host_name_count + 1, sizeof *g_host_fn);
    g_ready = load_image();
    if (!g_ready) return;
    {   /* close5 (c5b): the SDK's .eh_frame_hdr (through its MOD0 header at +8) is given to the exception runtime, so a
         * throw that starts in lifted SDK code (libc++'s __throw_length_error ...) is walked through the SDK's frames with
         * their own CFI up to the game's frames. Clean-up pads of SDK frames have no resume entry: nnunwind.c skips them. */
        int eh_register_image(uint64_t base, uint64_t size, uint64_t eh_frame_hdr);
        if (!eh_register_image(SDK_BASE, SDK_IMAGE_SIZE, 0)) trace_printf("sdklift: the exception tables of the SDK image were not found\n");
    }
    int installed = 0;
    for (int i = 0; i < dyn_sym_count; i++) {
        const char *name = dyn_syms[i].name;
        if (dyn_syms[i].defined || !name[0] || import_lookup(name)) continue;
        if (strncmp(name, "_ZN2nn", 6) && strncmp(name, "_ZNK2nn", 7) && strncmp(name, "nn", 2)) continue;
        installed += sdk_install(name);
    }
    trace_printf("sdklift: %d SDK functions lifted, %d installed as import handlers, %d host callees\n", sdk_fn_count, installed, sdk_host_name_count);
}
