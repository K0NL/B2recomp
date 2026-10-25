/* nn::pl: the system's shared fonts.
 *
 * Each font is looked up in %SSBU_FONTS% or ..\sysdata\fonts, first as a plain TrueType file (<name>.ttf), then
 * in the form the console stores it (<name>.bfttf, decoded here). The files come from the player's own console.
 * When neither is there, a font of this PC stands in for it (k_standin, from the Windows fonts folder): the game
 * runs and draws its system text in another typeface; characters only the console's fonts have (Nintendo's
 * button symbols) are missing. The game stops only when no stand-in exists either.
 *
 * SharedFontType: 0 Standard, 1 ChineseSimple, 2 ChineseSimpleExtension, 3 ChineseTraditional,
 *                 4 Korean, 5 NintendoExtension. SharedFontLoadState: 0 Loading, 1 Loaded. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <limits.h>
#include "runtime.h"
#include "hostrt.h"

#define A0 (c->x[0])
#define RET(v) (c->x[0] = (uint64_t)(v))
#define FONT_TYPES 7
#define BFTTF_PLAIN_MAGIC 0x7f9a0218u                     /* first big-endian word of a decoded .bfttf */

static const char *const font_files[FONT_TYPES] = {
    "nintendo_udsg-r_std_003", "nintendo_udsg-r_org_zh-cn_003", "nintendo_udsg-r_ext_zh-cn_003",
    "nintendo_udjxh-db_zh-tw_003", "nintendo_udsg-r_ko_003", "nintendo_ext_003", "nintendo_ext2_003",
};
/* Stand-ins in %WINDIR%\Fonts, first one present wins. Face 0 of each is the one the game gets (it loads index 0):
 * Yu Gothic and Meiryo are proportional Japanese gothics with Latin, like the console's UD Shin Go. */
static const char *const k_standin[FONT_TYPES][5] = {
    {"YuGothR.ttc", "meiryo.ttc", "segoeui.ttf", "arial.ttf"},
    {"msyh.ttc", "simsun.ttc", "segoeui.ttf", "arial.ttf"},
    {"msyh.ttc", "simsun.ttc", "segoeui.ttf", "arial.ttf"},
    {"msjh.ttc", "mingliu.ttc", "segoeui.ttf", "arial.ttf"},
    {"malgun.ttf", "gulim.ttc", "segoeui.ttf", "arial.ttf"},
    {"segoeui.ttf", "arial.ttf"},                                 /* no PC font has the Nintendo extension's symbols */
    {"segoeui.ttf", "arial.ttf"},
};
static struct { void *data; size_t size; int tried; char path[MAX_PATH]; } g_fonts[FONT_TYPES];
#ifndef _WIN32
/* [Recomps#1] Linux: the stand-in is the first of these TrueType fonts (glyf outlines, which nnfontll.c draws; not CFF)
 * found in the usual font folders, up to three levels down (distributions keep them in different subfolders):
 * Noto Sans (KDE, SteamOS, Fedora: its variable file), DejaVu Sans, Liberation Sans, Ubuntu ... */
#include <dirent.h>
#include <sys/stat.h>
static const char *const k_linux_fonts[] = { "NotoSans-Regular.ttf", "NotoSans[wght].ttf", "DejaVuSans.ttf",
    "LiberationSans-Regular.ttf", "Ubuntu-R.ttf", "OpenSans-Regular.ttf", "Arimo-Regular.ttf", "FreeSans.ttf", "arial.ttf",
    "Arial.ttf" };
static int font_find(const char *dir, const char *name, int depth, char *out, size_t n) {
    char p[PATH_MAX];
    struct stat st;
    snprintf(p, sizeof p, "%s/%s", dir, name);
    if (!stat(p, &st) && S_ISREG(st.st_mode)) { snprintf(out, n, "%s", p); return 1; }
    DIR *d = depth > 0 ? opendir(dir) : NULL;
    if (!d) return 0;
    int found = 0;
    for (struct dirent *e; !found && (e = readdir(d)); ) {
        if (e->d_name[0] == '.' || (e->d_type != DT_DIR && e->d_type != DT_UNKNOWN)) continue;
        snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        if (e->d_type == DT_DIR || (!stat(p, &st) && S_ISDIR(st.st_mode))) found = font_find(p, name, depth - 1, out, n);
    }
    closedir(d);
    return found;
}
static int linux_font(char *out, size_t n) {              /* once: every font type gets the same file (one copy) */
    static char found[PATH_MAX];
    static int done;
    if (!done) {
        char a[PATH_MAX] = "", b[PATH_MAX] = "";
        const char *home = getenv("HOME");
        if (home) { snprintf(a, sizeof a, "%s/.local/share/fonts", home); snprintf(b, sizeof b, "%s/.fonts", home); }
        const char *roots[] = { "/usr/share/fonts", "/usr/local/share/fonts", a, b };
        done = 1;
        for (size_t f = 0; f < sizeof k_linux_fonts / sizeof k_linux_fonts[0] && !found[0]; f++)
            for (size_t r = 0; r < sizeof roots / sizeof roots[0] && !found[0]; r++)
                if (roots[r][0]) font_find(roots[r], k_linux_fonts[f], 3, found, sizeof found);
    }
    if (found[0]) snprintf(out, n, "%s", found);
    return found[0] != 0;
}
#endif
#ifdef __APPLE__
/* [Recomps#1] macOS (notes/macos-port-plan.md step 7): the stand-in is Arial Unicode - glyf outlines, which
 * nnfontll.c draws (the system fonts of /System/Library/Fonts are CFF: no outlines), with the CJK glyphs the
 * game's system text needs - else Arial, from the system's Supplemental folder. */
static int darwin_font(char *out, size_t n) {
    static const char *const k_mac_fonts[] = { "/System/Library/Fonts/Supplemental/Arial Unicode.ttf",
        "/System/Library/Fonts/Supplemental/Arial.ttf", "/Library/Fonts/Arial Unicode.ttf" };
    struct stat st;
    for (size_t i = 0; i < sizeof k_mac_fonts / sizeof k_mac_fonts[0]; i++)
        if (!stat(k_mac_fonts[i], &st) && S_ISREG(st.st_mode)) { snprintf(out, n, "%s", k_mac_fonts[i]); return 1; }
    return 0;
}
#endif
static CRITICAL_SECTION g_lock;

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static int is_sfnt(const uint8_t *p) { return !memcmp(p, "\0\1\0\0", 4) || !memcmp(p, "OTTO", 4) || !memcmp(p, "ttcf", 4) || !memcmp(p, "true", 4); }

static void *read_file(const char *path, size_t *size) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    void *data = n > 0 ? _aligned_malloc((size_t)n + 16, 4096) : NULL;
    if (data && fread(data, 1, (size_t)n, f) != (size_t)n) { _aligned_free(data); data = NULL; }
    fclose(f);
    *size = data ? (size_t)n : 0;
    return data;
}

/* .bfttf: big-endian words XORed with one key, the first two words (magic, font size) before the font. The key is
 * the first word XOR the known magic. Decodes in place and moves the font to the start; 0 = not a font. */
static size_t decode_bfttf(uint8_t *p, size_t size) {
    if (size < 12) return 0;
    uint32_t key = be32(p) ^ BFTTF_PLAIN_MAGIC, len = be32(p + 4) ^ key;
    const uint8_t k[4] = {(uint8_t)(key >> 24), (uint8_t)(key >> 16), (uint8_t)(key >> 8), (uint8_t)key};
    for (size_t i = 8; i < (size & ~(size_t)3); i++) p[i] ^= k[i & 3];
    memmove(p, p + 8, size - 8);
    return len <= size - 8 && is_sfnt(p) ? len : 0;
}

static void load_font(unsigned type) {
    if (type >= FONT_TYPES) return;
    EnterCriticalSection(&g_lock);
    if (!g_fonts[type].tried) {
        g_fonts[type].tried = 1;
        const char *dir = getenv("SSBU_FONTS");
        if (!dir) dir = "..\\sysdata\\fonts";
        char *path = g_fonts[type].path;
        size_t size = 0;
        snprintf(path, MAX_PATH, "%s\\%s.ttf", dir, font_files[type]);
        uint8_t *data = read_file(path, &size);
        if (data && !is_sfnt(data)) { printf("nn::pl: %s is no TrueType font: not used\n", path); _aligned_free(data); data = NULL; }
        if (data) printf("nn::pl: shared font %u loaded (%s, %zu bytes)\n", type, path, size);
        if (!data) {
            snprintf(path, MAX_PATH, "%s\\%s.bfttf", dir, font_files[type]);
            if ((data = read_file(path, &size)) && !(size = decode_bfttf(data, size))) {
                printf("nn::pl: %s does not decode to a font: not used\n", path);
                _aligned_free(data);
                data = NULL;
            }
            if (data) printf("nn::pl: shared font %u loaded (%s, decoded: %zu bytes)\n", type, path, size);
        }
        if (!data) {                                      /* a console always has its fonts: a real font stands in, never "size 0" */
            char fonts[MAX_PATH];
            UINT n = GetWindowsDirectoryA(fonts, MAX_PATH);
            for (int i = 0; n && n < MAX_PATH - 32 && !data && k_standin[type][i]; i++) {
                snprintf(path, MAX_PATH, "%s\\Fonts\\%s", fonts, k_standin[type][i]);
                for (unsigned t = 0; t < FONT_TYPES && !data; t++)        /* the same file for another type: one copy */
                    if (t != type && g_fonts[t].data && !_stricmp(g_fonts[t].path, path)) data = g_fonts[t].data, size = g_fonts[t].size;
                if (!data) data = read_file(path, &size);
            }
#ifndef _WIN32
            if (!data && linux_font(path, MAX_PATH)) {        /* [Recomps#1] Linux: a TrueType font of the system */
                for (unsigned t = 0; t < FONT_TYPES && !data; t++)
                    if (t != type && g_fonts[t].data && !strcmp(g_fonts[t].path, path)) data = g_fonts[t].data, size = g_fonts[t].size;
                if (!data) data = read_file(path, &size);
                if (data && !is_sfnt(data)) { _aligned_free(data); data = NULL; }
            }
#endif
#ifdef __APPLE__
            if (!data && darwin_font(path, MAX_PATH)) {       /* [Recomps#1] macOS plan step 7: a glyf font of the system */
                for (unsigned t = 0; t < FONT_TYPES && !data; t++)
                    if (t != type && g_fonts[t].data && !strcmp(g_fonts[t].path, path)) data = g_fonts[t].data, size = g_fonts[t].size;
                if (!data) data = read_file(path, &size);
                if (data && !is_sfnt(data)) { _aligned_free(data); data = NULL; }
            }
#endif
            if (data)
                printf("nn::pl: shared font %u (%s.ttf or .bfttf) NOT FOUND in %s: STAND-IN %s (%zu bytes), a font of this PC: "
                       "the game's system text looks different from the console's\n", type, font_files[type], dir, path, size);
        }
        g_fonts[type].data = data;
        g_fonts[type].size = size;
        if (!data) {
            printf("nn::pl: shared font %u (%s.ttf or .bfttf) not in %s and no stand-in in the Windows fonts folder: put your "
                   "console's shared fonts into targets\\sysdata\\fonts (or SSBU_FONTS=<folder>): stopping\n", type, font_files[type], dir);
            fflush(stdout);
            report_and_exit(7);
        }
    }
    LeaveCriticalSection(&g_lock);
}

static void pl_RequestSharedFontLoad(Ctx *c) { load_font((unsigned)A0); }
static void pl_GetSharedFontLoadState(Ctx *c) { load_font((unsigned)A0); RET(1); }
static void pl_GetSharedFontAddress(Ctx *c) { unsigned t = (unsigned)A0; load_font(t); RET(t < FONT_TYPES ? (uint64_t)(uintptr_t)g_fonts[t].data : 0); }
static void pl_GetSharedFontSize(Ctx *c) { unsigned t = (unsigned)A0; load_font(t); RET(t < FONT_TYPES ? g_fonts[t].size : 0); }
static void pl_IsVrMode(Ctx *c) { RET(0); }
static void nothing(Ctx *c) { (void)c; }

static const ImportDef pl_imports[] = {
    {"_ZN2nn2pl21RequestSharedFontLoadENS0_14SharedFontTypeE", pl_RequestSharedFontLoad},
    {"_ZN2nn2pl22GetSharedFontLoadStateENS0_14SharedFontTypeE", pl_GetSharedFontLoadState},
    {"_ZN2nn2pl20GetSharedFontAddressENS0_14SharedFontTypeE", pl_GetSharedFontAddress},
    {"_ZN2nn2pl17GetSharedFontSizeENS0_14SharedFontTypeE", pl_GetSharedFontSize},
    {"_ZN2nn2pl8IsVrModeEv", pl_IsVrMode}, {"_ZN2nn2pl11BeginVrModeEv", nothing}, {"_ZN2nn2pl9EndVrModeEv", nothing},
};

void register_pl_imports(void) {
    InitializeCriticalSection(&g_lock);
    register_imports(pl_imports, (int)(sizeof pl_imports / sizeof pl_imports[0]));
}
