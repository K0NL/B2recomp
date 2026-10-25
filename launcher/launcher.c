/* launcher.c: settings window and Play button of the native build (build\launcher.exe; python scripts\build_launcher.py).
 *
 * Shows the main options of config\graphics.ini (read by runtime\gfx_options.c at start) and of the [general] section of
 * config\controllers.ini (runtime\nnpad_sdl.c, reloaded by a running game within a second). Saving rewrites only the
 * value text of the keys changed here, in place: comments, inline comments, unknown keys, other sections and the line
 * endings stay as they are. A key that is missing is added at the end of its section. Values are read the way the two
 * parsers read them (same aliases, same fallbacks), so the window shows what the game will use.
 *
 * Play starts the game the way scripts\play.ps1 does: build\game_native.exe targets\exefs\main.elf from build\, window
 * and sound on (SSBU_SHOW=1, SSBU_VOLUME=100), no focus pause (SSBU_FOCUS_NOTIFY=0), saves in build\play\userdata (first start: a copy of
 * build\saves\after-prologue), log in build\play\stdout.txt, no console window.
 * Remap / Test controller run game_native.exe --pad-map 1 / --pad-test in a console window of their own.
 * Mods... lists the code mods in mods\ with on / off switches (config\mods.ini) and rebuilds the game with them (below).
 *
 * Build screen: while the game is not built yet (or with --build), the same window first builds it from the player's own
 * dump with scripts\setup.py, then turns into the launcher (see "build screen" below). The builder package ships this
 * program ready-made as B2recomp.exe at its top (scripts\make_package.py); setup.py's link step also builds it into
 * build\launcher.exe. It finds the repository as the folder holding scripts\setup.py: its own folder, else the one above.
 */
#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#ifndef UNICODE                                          /* llvm-mingw's -municode defines both already */
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <uxtheme.h>
#include <dwmapi.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <math.h>

/* ================================================================ ini files, edited in place */
typedef struct { char **line; int n, cap; int crlf; } Ini;   /* split at '\n'; each line keeps its own '\r' */

static void ini_free(Ini *f) { for (int i = 0; i < f->n; i++) free(f->line[i]); free(f->line); memset(f, 0, sizeof *f); }
static void ini_insert(Ini *f, int at, const char *s, size_t len) {
    if (f->n == f->cap) { f->cap = f->cap ? f->cap * 2 : 64; f->line = realloc(f->line, (size_t)f->cap * sizeof *f->line); }
    memmove(f->line + at + 1, f->line + at, (size_t)(f->n - at) * sizeof *f->line);
    f->line[at] = malloc(len + 1); memcpy(f->line[at], s, len); f->line[at][len] = 0;
    f->n++;
}
static int ini_load(Ini *f, const wchar_t *path) {           /* 1 = the file exists */
    memset(f, 0, sizeof *f);
    f->crlf = 1;                                             /* both files are written by the runtime with \r\n */
    FILE *fp = _wfopen(path, L"rb");
    if (!fp) { ini_insert(f, 0, "", 0); return 0; }
    fseek(fp, 0, SEEK_END); long n = ftell(fp); fseek(fp, 0, SEEK_SET);
    char *b = malloc((size_t)n + 1);
    size_t got = fread(b, 1, (size_t)n, fp);
    fclose(fp);
    size_t start = 0;
    for (size_t i = 0; i < got; i++) if (b[i] == '\n') { ini_insert(f, f->n, b + start, i - start); start = i + 1; }
    ini_insert(f, f->n, b + start, got - start);             /* "" when the file ends with a newline */
    f->crlf = f->n > 1 ? (strlen(f->line[0]) && f->line[0][strlen(f->line[0]) - 1] == '\r') : 1;
    free(b);
    return 1;
}
static int is_blank(const char *l) { for (; *l; l++) if (*l != ' ' && *l != '\t' && *l != '\r') return 0; return 1; }
static int line_section(const char *l, char *name, size_t n) {   /* 1 for a [section] line */
    while (*l == ' ' || *l == '\t') l++;
    if (*l != '[') return 0;
    l++;
    size_t k = 0;
    while (*l && *l != ']' && *l != '\r' && k + 1 < n) name[k++] = *l++;
    name[k] = 0;
    return 1;
}
/* key = value line, as the parsers see it: value = line[*vs, *ve). inl: an inline comment (' ;' or ' #') ends the value */
static int line_kv(const char *l, int inl, char *key, size_t kn, int *vs, int *ve) {
    const char *p = l;
    while (*p == ' ' || *p == '\t') p++;
    if (!*p || *p == ';' || *p == '#' || *p == '[' || *p == '\r') return 0;
    const char *eq = strchr(p, '=');
    if (!eq) return 0;
    const char *ke = eq;
    while (ke > p && (ke[-1] == ' ' || ke[-1] == '\t')) ke--;
    if ((size_t)(ke - p) >= kn) return 0;
    memcpy(key, p, (size_t)(ke - p)); key[ke - p] = 0;
    const char *v = eq + 1;
    while (*v == ' ' || *v == '\t') v++;
    const char *e = v + strlen(v);
    if (inl) for (const char *q = v; *q; q++) if ((*q == ';' || *q == '#') && q > v && (q[-1] == ' ' || q[-1] == '\t')) { e = q; break; }
    while (e > v && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r')) e--;
    *vs = (int)(v - l); *ve = (int)(e - l);
    return 1;
}
/* sect NULL: every line (graphics.ini: gfx_options.c ignores section headers); else only the lines of [sect] */
static int in_sect(const char *cur, const char *sect) { return !sect || !_stricmp(cur, sect); }
static int ini_get(const Ini *f, const char *sect, const char *key, int inl, char *out, size_t on) {
    char cur[128] = "", k[64];
    int vs, ve, found = 0;
    for (int i = 0; i < f->n; i++) {
        if (line_section(f->line[i], cur, sizeof cur)) continue;
        if (in_sect(cur, sect) && line_kv(f->line[i], inl, k, sizeof k, &vs, &ve) && !_stricmp(k, key)) {   /* the last one counts */
            size_t len = (size_t)(ve - vs) < on - 1 ? (size_t)(ve - vs) : on - 1;
            memcpy(out, f->line[i] + vs, len); out[len] = 0;
            found = 1;
        }
    }
    return found;
}
static void ensure_eol(Ini *f) {                            /* the file ends with a newline: its last line is "" */
    char *l = f->line[f->n - 1];
    if (!*l) return;
    size_t n = strlen(l);
    if (f->crlf && l[n - 1] != '\r') { char *p = malloc(n + 2); memcpy(p, l, n); p[n] = '\r'; p[n + 1] = 0; free(l); f->line[f->n - 1] = p; }
    ini_insert(f, f->n, "", 0);
}
static void ini_set(Ini *f, const char *sect, const char *key, int inl, const char *val) {
    char cur[128] = "", k[64];
    int vs, ve, done = 0, last = -1, header = -1;
    for (int i = 0; i < f->n; i++) {
        if (line_section(f->line[i], cur, sizeof cur)) { if (!sect || !_stricmp(cur, sect)) header = last = i; continue; }
        if (!in_sect(cur, sect)) continue;
        if (!is_blank(f->line[i])) last = i;
        if (line_kv(f->line[i], inl, k, sizeof k, &vs, &ve) && !_stricmp(k, key)) {   /* every occurrence: no stale duplicate wins */
            const char *l = f->line[i];
            size_t tail = strlen(l + ve), vl = strlen(val);
            char *n = malloc((size_t)vs + vl + tail + 1);
            memcpy(n, l, (size_t)vs); memcpy(n + vs, val, vl); memcpy(n + vs + vl, l + ve, tail + 1);
            free(f->line[i]); f->line[i] = n;
            done = 1;
        }
    }
    if (done) return;
    char nl[256];
    const char *eol = f->crlf ? "\r" : "";
    if (sect && header < 0) {                                /* the section is missing: append it */
        ensure_eol(f);
        int at = f->n - 1;
        if (at > 0 && !is_blank(f->line[at - 1])) ini_insert(f, at++, eol, strlen(eol));
        snprintf(nl, sizeof nl, "[%s]%s", sect, eol); ini_insert(f, at++, nl, strlen(nl));
        snprintf(nl, sizeof nl, "%s = %s%s", key, val, eol); ini_insert(f, at, nl, strlen(nl));
        return;
    }
    int at = last + 1;                                       /* after the last line of the section */
    if (at >= f->n) { ensure_eol(f); at = f->n - 1; }
    snprintf(nl, sizeof nl, "%s = %s%s", key, val, eol);
    ini_insert(f, at, nl, strlen(nl));
}
static int ini_save(const Ini *f, const wchar_t *path) {
    wchar_t tmp[MAX_PATH + 8], dir[MAX_PATH];
    wcscpy(dir, path);
    wchar_t *s = wcsrchr(dir, L'\\');
    if (s) { *s = 0; CreateDirectoryW(dir, NULL); }
    swprintf(tmp, MAX_PATH + 8, L"%s.tmp", path);
    FILE *fp = _wfopen(tmp, L"wb");
    if (!fp) return 0;
    for (int i = 0; i < f->n; i++) { fputs(f->line[i], fp); if (i + 1 < f->n) fputc('\n', fp); }
    if (fclose(fp)) return 0;
    for (int t = 0; t < 20; t++) {                           /* a running game may have the file open for a moment */
        if (MoveFileExW(tmp, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return 1;
        Sleep(50);
    }
    DeleteFileW(tmp);
    return 0;
}

/* ================================================================ the settings */
enum { K_COMBO, K_NUM, K_SLIDER, K_CHECK };
enum { F_GFX, F_PAD };
typedef struct { const char *val; const wchar_t *text; } Opt;
typedef struct {
    int file; const char *key; int kind;
    const Opt *opt; int nopt, fallback;                      /* combo: what an unknown value means (as the parser) */
    double lo, hi, step;                                     /* slider */
    int nlo, nhi;                                            /* number: accepted range */
    const char *def;                                         /* the runtime's value when the key is missing */
    const wchar_t *label, *tip;
    HWND hw, hval, hlabel;                                   /* control, value text beside it, row label */
    char loaded[64];                                         /* combo / number / check: canonical value read */
    int loadpos, exact, moved;                               /* slider: position read; the file value is off the grid */
    double filev;
} Setting;

static const Opt o_renderer[] = { {"vulkan", L"Vulkan (recommended)"}, {"opengl", L"OpenGL (old renderer, NVIDIA only)"} };
static const Opt o_profile[] = { {"custom", L"Custom (the settings below)"}, {"low", L"Low (weak or integrated GPU)"} };
static const Opt o_window[] = { {"windowed", L"Windowed"}, {"borderless", L"Borderless window"}, {"fullscreen", L"Fullscreen (borderless, on top)"} };
static const Opt o_vsync[] = { {"auto", L"Auto (by refresh rate)"}, {"display", L"Display refresh (60/120/240 Hz)"},
                               {"clock", L"Program clock (60 Hz)"}, {"off", L"Off (tearing, lowest latency)"},
                               {"on", L"Swapchain FIFO (not recommended)"}, {"mailbox", L"Mailbox"} };
static const Opt o_latency[] = { {"normal", L"Normal"}, {"low", L"Low"}, {"lowest", L"Lowest (frames may judder)"} };
static const Opt o_aniso[] = { {"game", L"Game default"}, {"2", L"2x"}, {"4", L"4x"}, {"8", L"8x"}, {"16", L"16x"} };
static const Opt o_aspect[] = { {"keep", L"Keep 16:9 (black bars)"}, {"stretch", L"Stretch to the window"}, {"wide", L"Wide screen (21:9, 32:9)"} };
static const Opt o_hdr[] = { {"auto", L"Auto (when Windows HDR is on)"}, {"on", L"On (always)"}, {"off", L"Off"} };
static const Opt o_layout[] = { {"auto", L"Auto"}, {"positional", L"By position (Switch)"}, {"labels", L"By printed letter"} };
static const Opt o_prompts[] = { {"auto", L"Auto (by controller)"}, {"switch", L"Switch"}, {"playstation", L"PlayStation"}, {"xbox", L"Xbox"} };
#define OPTS(a) a, (int)(sizeof a / sizeof a[0])

enum { S_RENDERER, S_PROFILE, S_WINDOW, S_WIDTH, S_HEIGHT, S_VSYNC, S_LATENCY, S_SCALE, S_ANISO, S_ASPECT, S_OVERLAY, S_FPS, S_HDR,
       S_LAYOUT, S_PROMPTS, S_DEADZONE, S_RUMBLE, S_STRENGTH, S_INPUT, S_COUNT };
static const Opt o_input[] = { {"both", L"Controller and keyboard"}, {"keyboard", L"Keyboard and mouse only"}, {"controller", L"Controller only"} };
static Setting g_s[S_COUNT] = {
    [S_RENDERER] = { F_GFX, "renderer", K_COMBO, OPTS(o_renderer), 0, .def = "vulkan", .label = L"Renderer",
                     .tip = L"Vulkan is the default. OpenGL is the first renderer, kept as a fallback (NVIDIA only)." },
    [S_PROFILE]  = { F_GFX, "profile", K_COMBO, OPTS(o_profile), 0, .def = "custom", .label = L"Preset",
                     .tip = L"Low: resolution scale 0.5, game anisotropic filtering and the program clock for vsync; those three settings are then not used." },
    [S_WINDOW]   = { F_GFX, "window", K_COMBO, OPTS(o_window), 0, .def = "windowed", .label = L"Window mode",
                     .tip = L"Borderless and fullscreen cover the monitor the window is on (the desktop keeps its resolution)." },
    [S_WIDTH]    = { F_GFX, "width", K_NUM, .nlo = 320, .nhi = 16384, .def = "1280", .label = L"Window size" },
    [S_HEIGHT]   = { F_GFX, "height", K_NUM, .nlo = 180, .nhi = 16384, .def = "720" },
    [S_VSYNC]    = { F_GFX, "vsync", K_COMBO, OPTS(o_vsync), 2, .def = "auto", .label = L"Vertical sync",
                     .tip = L"Auto: 'display' on a 60, 120 or 240 Hz monitor (the program's clock while its refresh drifts: G-Sync / FreeSync), else the program's own 60 Hz clock. Off: tearing allowed, lowest latency." },
    [S_LATENCY]  = { F_GFX, "latency", K_COMBO, OPTS(o_latency), 0, .def = "normal", .label = L"Input latency",
                     .tip = L"Low: the controller is read 2-5 ms later, closer to the screen. Lowest: about 4 ms less again, but frames no longer last the same time." },
    [S_SCALE]    = { F_GFX, "resolution_scale", K_SLIDER, .lo = 0.5, .hi = 3.0, .step = 0.05, .def = "1.0", .label = L"Resolution scale",
                     .tip = L"Internal resolution. 1.0 = the game's 1280x720; 1.5 = 1920x1080; 2.0 = 2560x1440 (sharper edges)." },
    [S_ANISO]    = { F_GFX, "anisotropic_filtering", K_COMBO, OPTS(o_aniso), 0, .def = "game", .label = L"Anisotropic filtering",
                     .tip = L"Sharper textures at flat angles; costs little on any GPU." },
    [S_ASPECT]   = { F_GFX, "aspect", K_COMBO, OPTS(o_aspect), 0, .def = "keep", .label = L"Aspect ratio",      /* [Recomps#1] */
                     .tip = L"The game's picture is 16:9. Keep: in a wider or taller window (an ultrawide monitor in borderless or "
                            L"fullscreen mode) it keeps its shape, with black bars. Stretch: it fills the window. Wide screen: on a 21:9 "
                            L"or 32:9 monitor the game draws at the monitor's shape (more of the world at the sides, menus in the middle, "
                            L"movies at 16:9); on a 16:9 monitor it is the same as Keep. Use Borderless or Fullscreen." },
    [S_OVERLAY]  = { F_GFX, "frame_time_overlay", K_CHECK, .def = "off", .label = L"Show frame time in the window title" },
    [S_FPS]      = { F_GFX, "frame_rate", K_SLIDER, .lo = 60, .hi = 120, .step = 1, .def = "60", .label = L"Frame rate (beta)",   /* [Recomps#1] gfx_options.c */
                     .tip = L"60: the game as made. Above 60 (experimental): the game runs up to this many frames per second at its normal speed. "
                            L"Its main timers are converted; some effects, menus and button-mash prompts may still run faster, and cinematics run in real time." },
    [S_HDR]      = { F_GFX, "hdr", K_COMBO, OPTS(o_hdr), 0, .def = "auto", .label = L"HDR",      /* [Recomps#1] finding 147 */
                     .tip = L"Auto: while Windows HDR is on for the monitor the game is on, the picture goes out as HDR10, its white as bright "
                            L"as Windows' 'SDR content brightness' (Settings > System > Display > HDR); otherwise as before. On: always HDR10. "
                            L"Off: never (Windows shows the game like other SDR programs)." },
    [S_LAYOUT]   = { F_PAD, "layout", K_COMBO, OPTS(o_layout), 1, .def = "auto", .label = L"Face buttons",
                     .tip = L"By position: the right button is A, bottom B, top X, left Y (as on a Switch). By printed letter: an Xbox pad's A is A; "
                            L"on a PlayStation pad Cross is A, Circle B, Square X, Triangle Y. The PlayStation and Xbox button pictures follow this choice. "
                            L"Auto: by letter on GameCube pads, by position on all others." },
    [S_PROMPTS]  = { F_PAD, "button_prompts", K_COMBO, OPTS(o_prompts), 0, .def = "auto", .label = L"Button prompts",   /* [Recomps#1] nnpad_sdl.c */
                     .tip = L"Which button pictures the game shows. PlayStation and Xbox need the free mod \"Controller Prompts "
                            L"Replacement (DS4/XBOX)\" by Timo654 (gamebanana.com/mods/360121, Switch version): put its DS4Prompts / "
                            L"XBOXPrompts folder into the mods folder. Auto: by player 1's controller when the game starts." },
    [S_DEADZONE] = { F_PAD, "deadzone", K_SLIDER, .lo = 0.0, .hi = 0.5, .step = 0.01, .def = "0.10", .label = L"Stick dead zone",
                     .tip = L"Part of the stick travel around the centre that counts as no movement." },
    [S_RUMBLE]   = { F_PAD, "rumble", K_CHECK, .def = "on", .label = L"Vibration" },
    [S_STRENGTH] = { F_PAD, "rumble_strength", K_SLIDER, .lo = 0.0, .hi = 2.0, .step = 0.05, .def = "1.0", .label = L"Vibration strength" },
    [S_INPUT]    = { F_PAD, "input", K_COMBO, OPTS(o_input), 0, .def = "both", .label = L"Input",       /* [Recomps#1] nnpad_sdl.c */
                     .tip = L"Lock the game to one kind of input. Keyboard and mouse only: controllers are ignored (no buttons, no vibration). "
                            L"Controller only: the keyboard and mouse are ignored (from the next game start; the controller part applies at once)." },
};

/* ================================================================ paths and state */
static wchar_t g_root[MAX_PATH], g_buildd[MAX_PATH], g_ini[2][MAX_PATH], g_game[MAX_PATH], g_elf[MAX_PATH];
static int g_exists[2], g_loading, g_devover;
static HWND g_main, g_tip, g_status, g_save, g_play, g_sizes, g_padnote, g_gfxnote;
static HFONT g_font, g_bigfont, g_smallfont;
static HANDLE g_proc;
static int g_dpi = 96;
#define S(x) MulDiv((x), g_dpi, 96)
enum { ID_SET = 100, ID_SIZES = 200, ID_REMAP, ID_TEST, ID_SAVE, ID_PLAY, ID_CLOSE, ID_CONFIG, ID_STATUS, ID_KEYS, ID_MODS, ID_TIMER = 1 };
static const int k_sizes[][2] = { {1280, 720}, {1600, 900}, {1920, 1080}, {2560, 1440}, {3840, 2160} };

static int file_exists(const wchar_t *p) { return GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES; }

/* ================================================================ values: file text <-> controls */
static void fmt_num(double v, char *out, size_t n) {          /* 1.5 -> "1.5", 1 -> "1.0", 0.75 -> "0.75" */
    snprintf(out, n, "%.2f", v);
    size_t l = strlen(out);
    while (l > 3 && out[l - 1] == '0' && out[l - 2] != '.') out[--l] = 0;
}
static int combo_index(const Setting *s, const char *raw) {   /* as gfx_options.c / nnpad_sdl.c read it */
    if (s == &g_s[S_ANISO]) { int v = atoi(raw); for (int i = 1; i < s->nopt; i++) if (atoi(s->opt[i].val) == v) return i; return 0; }
    if (s == &g_s[S_RENDERER] && !_stricmp(raw, "gl")) return 1;
    if (s == &g_s[S_LAYOUT] && !_stricmp(raw, "label")) return 2;
    for (int i = 0; i < s->nopt; i++) if (!_stricmp(raw, s->opt[i].val)) return i;
    return s->fallback;
}
static int check_value(const Setting *s, const char *raw) {
    if (s == &g_s[S_RUMBLE]) return _stricmp(raw, "off") && _stricmp(raw, "0") && _stricmp(raw, "no") && _stricmp(raw, "false");
    return !_stricmp(raw, "on") || !strcmp(raw, "1");
}
static int slider_max(const Setting *s) { return (int)floor((s->hi - s->lo) / s->step + 0.5); }
static double slider_value(const Setting *s, int pos) { return s->lo + pos * s->step; }
static int slider_pos(const Setting *s) { return (int)SendMessageW(s->hw, TBM_GETPOS, 0, 0); }

static void current(const Setting *s, char *out, size_t n) {  /* the value the control shows, as file text */
    switch (s->kind) {
    case K_COMBO: { int i = (int)SendMessageW(s->hw, CB_GETCURSEL, 0, 0); snprintf(out, n, "%s", s->opt[i < 0 ? 0 : i].val); break; }
    case K_NUM: { wchar_t w[32]; GetWindowTextW(s->hw, w, 32); snprintf(out, n, "%d", _wtoi(w)); break; }
    case K_CHECK: snprintf(out, n, "%s", SendMessageW(s->hw, BM_GETCHECK, 0, 0) == BST_CHECKED ? "on" : "off"); break;
    case K_SLIDER:                                           /* dead zone as the default file writes it: 0.10 */
        if (s == &g_s[S_DEADZONE]) snprintf(out, n, "%.2f", slider_value(s, slider_pos(s)));
        else if (s == &g_s[S_FPS]) snprintf(out, n, "%d", (int)(slider_value(s, slider_pos(s)) + 0.5));
        else fmt_num(slider_value(s, slider_pos(s)), out, n);
        break;
    }
}
static int changed(const Setting *s) {
    if (s->kind == K_SLIDER) return slider_pos(s) != s->loadpos || (s->exact && s->moved);
    char c[64]; current(s, c, sizeof c);
    return strcmp(c, s->loaded) != 0;
}
static void slider_label(Setting *s) {
    wchar_t t[96];
    double v = s->exact && !s->moved ? s->filev : slider_value(s, slider_pos(s));
    if (s == &g_s[S_SCALE]) swprintf(t, 96, L"%.2fx   (%d x %d)", v, (int)(1280 * v + 0.5), (int)(720 * v + 0.5));
    else if (s == &g_s[S_FPS]) swprintf(t, 96, L"%d fps", (int)(v + 0.5));
    else swprintf(t, 96, L"%d %%", (int)floor(v * 100 + 0.5));
    SetWindowTextW(s->hval, t);
}

static void update_ui(void) {
    char v[64];
    current(&g_s[S_RENDERER], v, sizeof v); int vk = !strcmp(v, "vulkan");
    current(&g_s[S_PROFILE], v, sizeof v); int custom = !strcmp(v, "custom");
    current(&g_s[S_WINDOW], v, sizeof v); int windowed = !strcmp(v, "windowed");
    EnableWindow(g_s[S_WIDTH].hw, windowed); EnableWindow(g_s[S_HEIGHT].hw, windowed); EnableWindow(g_sizes, windowed);
    EnableWindow(g_s[S_VSYNC].hw, vk && custom);
    EnableWindow(g_s[S_SCALE].hw, vk && custom);
    EnableWindow(g_s[S_ANISO].hw, vk && custom);
    EnableWindow(g_s[S_LATENCY].hw, vk);
    EnableWindow(g_s[S_ASPECT].hw, vk);
    EnableWindow(g_s[S_HDR].hw, vk);
    int rumble =SendMessageW(g_s[S_RUMBLE].hw, BM_GETCHECK, 0, 0) == BST_CHECKED;
    EnableWindow(g_s[S_STRENGTH].hw, rumble);
    SetWindowTextW(g_s[S_RUMBLE].hval, rumble ? L"On" : L"Off");
    for (int i = 0; i < S_COUNT; i++) {                      /* labels are dimmed with their control (WM_CTLCOLORSTATIC) */
        if (g_s[i].hlabel) InvalidateRect(g_s[i].hlabel, NULL, TRUE);
        if (g_s[i].hval) InvalidateRect(g_s[i].hval, NULL, TRUE);
    }
    SetWindowTextW(g_gfxnote, !vk ? L"OpenGL ignores the greyed settings (they are Vulkan only)."
                            : !custom ? L"The Low preset sets vsync, resolution scale and filtering itself."
                            : L"Graphics settings take effect the next time the game starts.");
    {   /* window size presets follow the two numbers */
        wchar_t w[32], h[32]; GetWindowTextW(g_s[S_WIDTH].hw, w, 32); GetWindowTextW(g_s[S_HEIGHT].hw, h, 32);
        int sel = 0;
        for (int i = 0; i < 5; i++) if (_wtoi(w) == k_sizes[i][0] && _wtoi(h) == k_sizes[i][1]) sel = i + 1;
        SendMessageW(g_sizes, CB_SETCURSEL, sel, 0);
    }
    int dirty = 0;
    for (int i = 0; i < S_COUNT; i++) dirty |= changed(&g_s[i]);
    EnableWindow(g_save, dirty);
    if (!g_proc) SetWindowTextW(g_status, dirty ? L"Unsaved changes (Play saves them first)." : L"");
}

static void load_all(void) {
    Ini f[2];
    char raw[128];
    g_loading = 1;
    for (int k = 0; k < 2; k++) g_exists[k] = ini_load(&f[k], g_ini[k]);
    for (int i = 0; i < S_COUNT; i++) {
        Setting *s = &g_s[i];
        if (!ini_get(&f[s->file], s->file == F_PAD ? "general" : NULL, s->key, s->file == F_PAD, raw, sizeof raw)) snprintf(raw, sizeof raw, "%s", s->def);
        switch (s->kind) {
        case K_COMBO: { int x = combo_index(s, raw); SendMessageW(s->hw, CB_SETCURSEL, x, 0); snprintf(s->loaded, sizeof s->loaded, "%s", s->opt[x].val); break; }
        case K_CHECK: { int on = check_value(s, raw); SendMessageW(s->hw, BM_SETCHECK, on ? BST_CHECKED : BST_UNCHECKED, 0); snprintf(s->loaded, sizeof s->loaded, "%s", on ? "on" : "off"); break; }
        case K_NUM: {
            int v = atoi(raw);
            if (v < s->nlo || v > s->nhi) v = atoi(s->def);     /* the parser keeps its default for a value out of range */
            snprintf(s->loaded, sizeof s->loaded, "%d", v);
            wchar_t w[32]; swprintf(w, 32, L"%d", v); SetWindowTextW(s->hw, w);
            break;
        }
        case K_SLIDER: {
            double v = atof(raw);
            if (s == &g_s[S_SCALE] && !(v > 0)) v = 1.0;
            int pos = (int)floor((v - s->lo) / s->step + 0.5);
            if (pos < 0) pos = 0;
            if (pos > slider_max(s)) pos = slider_max(s);
            s->filev = v; s->loadpos = pos; s->moved = 0;
            s->exact = fabs(slider_value(s, pos) - v) > 1e-6;
            SendMessageW(s->hw, TBM_SETPOS, TRUE, pos);
            slider_label(s);
            break;
        }
        }
    }
    {   /* [device <GUID>] sections with their own value of one of these keys win over [general] for that pad */
        char cur[128] = "", key[64]; int vs, ve, counted = 0;
        g_devover = 0;
        for (int i = 0; i < f[F_PAD].n; i++) {
            if (line_section(f[F_PAD].line[i], cur, sizeof cur)) { counted = 0; continue; }
            if (counted || _strnicmp(cur, "device ", 7) || !line_kv(f[F_PAD].line[i], 1, key, sizeof key, &vs, &ve)) continue;
            for (int k = S_LAYOUT; k < S_COUNT; k++) if (k != S_PROMPTS && k != S_INPUT && !_stricmp(key, g_s[k].key)) { g_devover++; counted = 1; break; }   /* prompts, input: [general] only */
        }
        wchar_t t[256];
        if (g_devover) swprintf(t, 256, L"%d pad section(s) of controllers.ini set their own value of one of these; that value wins for that pad.", g_devover);
        else swprintf(t, 256, L"Controller settings take effect at once, also while the game runs.");
        SetWindowTextW(g_padnote, t);
    }
    for (int k = 0; k < 2; k++) ini_free(&f[k]);
    g_loading = 0;
    update_ui();
}

static int save_all(void) {                                   /* 1 = saved (or nothing to save) */
    for (int i = S_WIDTH; i <= S_HEIGHT; i++) {
        wchar_t w[32]; GetWindowTextW(g_s[i].hw, w, 32);
        int v = _wtoi(w);
        if (changed(&g_s[i]) && (v < g_s[i].nlo || v > g_s[i].nhi)) {
            wchar_t m[128]; swprintf(m, 128, L"The window %s must be between %d and %d.", i == S_WIDTH ? L"width" : L"height", g_s[i].nlo, g_s[i].nhi);
            MessageBoxW(g_main, m, L"B2recomp", MB_ICONWARNING);
            SetFocus(g_s[i].hw);
            return 0;
        }
    }
    int nch[2] = {0, 0};
    for (int k = 0; k < 2; k++) {
        Ini f;
        int any = 0;
        for (int i = 0; i < S_COUNT; i++) any |= g_s[i].file == k && changed(&g_s[i]);
        if (!any) continue;
        ini_load(&f, g_ini[k]);                               /* fresh from disk: --pad-map or an editor may have written it */
        for (int i = 0; i < S_COUNT; i++) {
            Setting *s = &g_s[i];
            if (s->file != k || !changed(s)) continue;
            char v[64]; current(s, v, sizeof v);
            ini_set(&f, k == F_PAD ? "general" : NULL, s->key, k == F_PAD, v);
            nch[k]++;
        }
        int ok = ini_save(&f, g_ini[k]);
        ini_free(&f);
        if (!ok) {
            wchar_t m[MAX_PATH + 64]; swprintf(m, MAX_PATH + 64, L"Could not write %s.", g_ini[k]);
            MessageBoxW(g_main, m, L"B2recomp", MB_ICONERROR);
            return 0;
        }
    }
    load_all();
    if (nch[0] + nch[1]) {
        wchar_t t[128]; swprintf(t, 128, L"Saved: %d graphics and %d controller setting(s).", nch[0], nch[1]);
        if (!g_proc) SetWindowTextW(g_status, t);
    }
    return 1;
}

/* ================================================================ starting things */
static void copy_tree(const wchar_t *from, const wchar_t *to) {
    wchar_t pat[MAX_PATH], a[MAX_PATH], b[MAX_PATH];
    WIN32_FIND_DATAW fd;
    CreateDirectoryW(to, NULL);
    swprintf(pat, MAX_PATH, L"%s\\*", from);
    HANDLE h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
        swprintf(a, MAX_PATH, L"%s\\%s", from, fd.cFileName); swprintf(b, MAX_PATH, L"%s\\%s", to, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) copy_tree(a, b); else CopyFileW(a, b, FALSE);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}
static int have_game(void) {
    wchar_t dll[MAX_PATH];
    swprintf(dll, MAX_PATH, L"%s\\game.dll", g_buildd);
    if (!file_exists(g_game) || !file_exists(dll)) {         /* a fresh clone: setup.py makes both from the player's dump */
        MessageBoxW(g_main, L"The game is not built yet: start B2recomp with --build, or run python scripts/setup.py --dump <your dump>",
                    L"B2recomp", MB_ICONINFORMATION);
        return 0;
    }
    if (file_exists(g_elf)) return 1;
    wchar_t m[MAX_PATH + 64];
    swprintf(m, MAX_PATH + 64, L"Not found:\n%s", g_elf);
    MessageBoxW(g_main, m, L"B2recomp", MB_ICONERROR);
    return 0;
}
static void run_tool(const wchar_t *args, const wchar_t *title) {   /* the pad tools print to a console of their own */
    if (!have_game()) return;
    wchar_t cmd[3 * MAX_PATH], comspec[MAX_PATH];
    if (!GetEnvironmentVariableW(L"ComSpec", comspec, MAX_PATH)) wcscpy(comspec, L"cmd.exe");
    swprintf(cmd, 3 * MAX_PATH, L"\"%s\" /s /c \"title %s & \"%s\" %s & echo. & pause\"", comspec, title, g_game, args);
    STARTUPINFOW si = { sizeof si };
    PROCESS_INFORMATION pi;
    if (CreateProcessW(NULL, cmd, NULL, NULL, FALSE, CREATE_NEW_CONSOLE, NULL, g_buildd, &si, &pi)) { CloseHandle(pi.hThread); CloseHandle(pi.hProcess); }
    else MessageBoxW(g_main, L"Could not start the controller tool.", L"B2recomp", MB_ICONERROR);
}
static void play(void) {
    if (g_proc || !save_all() || !have_game()) return;
    wchar_t user[MAX_PATH], tmp[MAX_PATH], log[MAX_PATH], cmd[3 * MAX_PATH];
    swprintf(tmp, MAX_PATH, L"%s\\play", g_buildd); CreateDirectoryW(tmp, NULL);
    swprintf(user, MAX_PATH, L"%s\\play\\userdata", g_buildd);
    swprintf(tmp, MAX_PATH, L"%s\\save", user);
    if (!file_exists(tmp)) {                                  /* first start: slot 1 at the first chapter (play.ps1) */
        CreateDirectoryW(user, NULL);
        wchar_t pro[MAX_PATH]; swprintf(pro, MAX_PATH, L"%s\\saves\\after-prologue", g_buildd);
        swprintf(tmp, MAX_PATH, L"%s\\save", pro);
        if (file_exists(tmp)) copy_tree(pro, user);
    }
    swprintf(tmp, MAX_PATH, L"%s\\targets\\romfs", g_root); SetEnvironmentVariableW(L"SSBU_ROMFS", tmp);
    SetEnvironmentVariableW(L"SSBU_USERDATA", user);
    SetEnvironmentVariableW(L"SSBU_SHOW", L"1"); SetEnvironmentVariableW(L"SSBU_VOLUME", L"100");
    SetEnvironmentVariableW(L"SSBU_FOCUS_NOTIFY", L"0");      /* keep running when another window is in front (play.ps1, finding 98) */
    swprintf(tmp, MAX_PATH, L"%s\\targets\\sysdata\\fonts", g_root);
    SetEnvironmentVariableW(L"SSBU_FONTS", file_exists(tmp) ? tmp : NULL);
    swprintf(log, MAX_PATH, L"%s\\play\\stdout.txt", g_buildd);
    SECURITY_ATTRIBUTES sa = { sizeof sa, NULL, TRUE };
    HANDLE hl = CreateFileW(log, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    STARTUPINFOW si = { sizeof si };                          /* no STARTF_USESHOWWINDOW: a hidden start state would reach the game window */
    if (hl != INVALID_HANDLE_VALUE) { si.dwFlags = STARTF_USESTDHANDLES; si.hStdOutput = si.hStdError = hl; }
    PROCESS_INFORMATION pi;
    swprintf(cmd, 3 * MAX_PATH, L"\"%s\" \"%s\"", g_game, g_elf);
    BOOL ok = CreateProcessW(g_game, cmd, NULL, NULL, hl != INVALID_HANDLE_VALUE, CREATE_NO_WINDOW, NULL, g_buildd, &si, &pi);
    if (hl != INVALID_HANDLE_VALUE) CloseHandle(hl);
    if (!ok) { MessageBoxW(g_main, L"Could not start build\\game_native.exe.", L"B2recomp", MB_ICONERROR); return; }
    CloseHandle(pi.hThread);
    g_proc = pi.hProcess;
    EnableWindow(g_play, FALSE);
    SetWindowTextW(g_play, L"Running");
    SetWindowTextW(g_status, L"The game is running (log: build\\play\\stdout.txt).");
}

/* ================================================================ window
 * Dark, in the manner of current PC launchers: key art from the game's own title screen across the top (build\launcher_art.bmp,
 * a renderer capture copied there by scripts\build_launcher.py; cropped round the logo, fading into the background), the
 * settings on two panels with the standard Windows controls in their dark theme, a gold Play button. Without the art file
 * the header shows the title in text. */
#define WIN_W 780
#define WIN_H 788                                          /* [Recomps#1] finding 147: one row more (HDR) */
#define HEADER 244
#define C_BG     RGB(7, 10, 20)
#define C_PANEL  RGB(15, 20, 36)
#define C_EDGE   RGB(38, 48, 78)
#define C_FIELD  RGB(25, 31, 52)
#define C_TEXT   RGB(222, 228, 242)
#define C_DIM    RGB(98, 108, 134)
#define C_NOTE   RGB(128, 142, 172)
#define C_VALUE  RGB(238, 206, 140)
#define C_GOLD   RGB(222, 178, 92)
enum { ID_LBL = 300 };                                      /* clickable labels of the check boxes: ID_LBL + setting */
static HBRUSH g_brbg, g_brpanel, g_brfield;
static HBITMAP g_art;
static COLORREF g_caption = C_BG;
static HFONT g_headfont, g_titlefont;
static int g_playhot;
static const RECT k_panel[2] = { {14, 232, 454, 724}, {466, 232, 766, 724} };   /* graphics, controller (96-dpi px) */
static BOOL (WINAPI *p_AllowDarkModeForWindow)(HWND, BOOL);

static HWND mk(const wchar_t *cls, const wchar_t *text, DWORD style, int x, int y, int w, int h, int id) {
    HWND c = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, S(x), S(y), S(w), S(h), g_main, (HMENU)(INT_PTR)id, GetModuleHandleW(NULL), NULL);
    SendMessageW(c, WM_SETFONT, (WPARAM)g_font, FALSE);
    const wchar_t *theme = !wcscmp(cls, L"COMBOBOX") || !wcscmp(cls, L"EDIT") ? L"DarkMode_CFD"
                         : (!wcscmp(cls, L"BUTTON") && (style & BS_TYPEMASK) != BS_OWNERDRAW) || !wcscmp(cls, TRACKBAR_CLASSW) ? L"DarkMode_Explorer" : NULL;
    if (theme) { if (p_AllowDarkModeForWindow) p_AllowDarkModeForWindow(c, TRUE); SetWindowTheme(c, theme, NULL); }
    return c;
}
static void tip(HWND c, const wchar_t *text) {
    if (!text) return;
    TOOLINFOW ti = { sizeof ti };
    ti.uFlags = TTF_IDISHWND | TTF_SUBCLASS; ti.hwnd = g_main; ti.uId = (UINT_PTR)c; ti.lpszText = (wchar_t *)text;
    SendMessageW(g_tip, TTM_ADDTOOLW, 0, (LPARAM)&ti);
}
static void mk_setting(int i, int lx, int cx, int cw, int y) {
    Setting *s = &g_s[i];
    int id = ID_SET + i;
    if (s->kind == K_CHECK) {                                /* box without text (themed box text ignores the colours) + a label */
        if (i == S_OVERLAY) {
            s->hw = mk(L"BUTTON", L"", BS_AUTOCHECKBOX | WS_TABSTOP, lx, y + 2, 20, 20, id);
            s->hlabel = mk(L"STATIC", s->label, SS_LEFT | SS_NOTIFY, lx + 24, y + 4, cx + cw - lx - 24, 20, ID_LBL + i);
        } else {
            s->hlabel = mk(L"STATIC", s->label, SS_LEFT, lx, y + 4, cx - lx - 6, 20, -1);
            s->hw = mk(L"BUTTON", L"", BS_AUTOCHECKBOX | WS_TABSTOP, cx, y + 2, 20, 20, id);
            s->hval = mk(L"STATIC", L"On", SS_LEFT | SS_NOTIFY, cx + 24, y + 4, 60, 20, ID_LBL + i);
        }
    } else {
        if (s->label) s->hlabel = mk(L"STATIC", s->label, SS_LEFT, lx, y + 4, cx - lx - 6, 20, -1);
        if (s->kind == K_COMBO) {
            s->hw = mk(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, cx, y, cw, 200, id);
            for (int k = 0; k < s->nopt; k++) SendMessageW(s->hw, CB_ADDSTRING, 0, (LPARAM)s->opt[k].text);
        } else if (s->kind == K_SLIDER) {
            int big = i == S_SCALE;
            s->hw = mk(TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_AUTOTICKS | WS_TABSTOP, cx - 4, y, big ? cw + 8 : cw - 52, 26, id);
            SendMessageW(s->hw, TBM_SETRANGE, TRUE, MAKELPARAM(0, slider_max(s)));
            SendMessageW(s->hw, TBM_SETTICFREQ, i == S_SCALE ? 5 : 10, 0);
            SendMessageW(s->hw, TBM_SETLINESIZE, 0, 1);
            SendMessageW(s->hw, TBM_SETPAGESIZE, 0, 5);
            s->hval = big ? mk(L"STATIC", L"", SS_LEFT, cx, y + 28, cw, 18, -1) : mk(L"STATIC", L"", SS_RIGHT, cx + cw - 48, y + 4, 48, 20, -1);
        }
    }
    tip(s->hw, s->tip);
}

static LRESULT CALLBACK play_hover(HWND w, UINT m, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR ref) {
    (void)id; (void)ref;
    if (m == WM_MOUSEMOVE && !g_playhot) {
        TRACKMOUSEEVENT t = { sizeof t, TME_LEAVE, w, 0 };
        TrackMouseEvent(&t);
        g_playhot = 1; InvalidateRect(w, NULL, FALSE);
    } else if (m == WM_MOUSELEAVE) { g_playhot = 0; InvalidateRect(w, NULL, FALSE); }
    return DefSubclassProc(w, m, wp, lp);
}

static void create_controls(void) {
    g_tip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, NULL, WS_POPUP | TTS_ALWAYSTIP, 0, 0, 0, 0, g_main, NULL, GetModuleHandleW(NULL), NULL);
    SendMessageW(g_tip, TTM_SETMAXTIPWIDTH, 0, S(360));
    SendMessageW(g_tip, TTM_SETDELAYTIME, TTDT_AUTOPOP, 20000);
    if (p_AllowDarkModeForWindow) p_AllowDarkModeForWindow(g_tip, TRUE);
    SetWindowTheme(g_tip, L"DarkMode_Explorer", NULL);
    /* graphics: left panel */
    const RECT *gp = &k_panel[0];
    int lx = gp->left + 16, cx = gp->left + 158, cw = gp->right - 16 - cx, y = gp->top + 46, dy = 32;
    mk_setting(S_RENDERER, lx, cx, cw, y); y += dy;
    mk_setting(S_PROFILE, lx, cx, cw, y); y += dy;
    mk_setting(S_WINDOW, lx, cx, cw, y); y += dy;
    g_s[S_WIDTH].hlabel = mk(L"STATIC", g_s[S_WIDTH].label, SS_LEFT, lx, y + 4, cx - lx - 6, 20, -1);
    g_s[S_WIDTH].hw = mk(L"EDIT", L"", ES_NUMBER | ES_CENTER | WS_BORDER | WS_TABSTOP, cx, y, 56, 23, ID_SET + S_WIDTH);
    g_s[S_HEIGHT].hlabel = mk(L"STATIC", L"x", SS_CENTER, cx + 56, y + 4, 16, 20, -1);
    g_s[S_HEIGHT].hw = mk(L"EDIT", L"", ES_NUMBER | ES_CENTER | WS_BORDER | WS_TABSTOP, cx + 72, y, 56, 23, ID_SET + S_HEIGHT);
    SendMessageW(g_s[S_WIDTH].hw, EM_SETLIMITTEXT, 5, 0); SendMessageW(g_s[S_HEIGHT].hw, EM_SETLIMITTEXT, 5, 0);
    g_sizes = mk(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, cx + 136, y, cw - 136, 200, ID_SIZES);
    SendMessageW(g_sizes, CB_ADDSTRING, 0, (LPARAM)L"Custom");
    for (int i = 0; i < 5; i++) { wchar_t t[32]; swprintf(t, 32, L"%d x %d", k_sizes[i][0], k_sizes[i][1]); SendMessageW(g_sizes, CB_ADDSTRING, 0, (LPARAM)t); }
    tip(g_s[S_WIDTH].hw, L"Client size of the window in windowed mode."); tip(g_s[S_HEIGHT].hw, L"Client size of the window in windowed mode.");
    y += dy;
    mk_setting(S_VSYNC, lx, cx, cw, y); y += dy;
    mk_setting(S_FPS, lx, cx, cw, y); y += dy;
    mk_setting(S_LATENCY, lx, cx, cw, y); y += dy;
    mk_setting(S_SCALE, lx, cx, cw, y); y += dy + 18;
    mk_setting(S_ANISO, lx, cx, cw, y); y += dy;
    mk_setting(S_ASPECT, lx, cx, cw, y); y += dy;
    mk_setting(S_HDR, lx, cx, cw, y); y += dy;
    mk_setting(S_OVERLAY, lx, cx, cw, y); y += dy;
    g_gfxnote = mk(L"STATIC", L"", SS_LEFT, lx, y + 2, gp->right - gp->left - 32, 34, -1);
    SendMessageW(g_gfxnote, WM_SETFONT, (WPARAM)g_smallfont, FALSE);
    /* controller: right panel */
    const RECT *pp = &k_panel[1];
    int pw = pp->right - pp->left;
    lx = pp->left + 16; cx = pp->left + 122; cw = pp->right - 16 - cx; y = pp->top + 46;
    mk_setting(S_INPUT, lx, cx, cw, y); y += dy;
    mk_setting(S_LAYOUT, lx, cx, cw, y); y += dy;
    mk_setting(S_PROMPTS, lx, cx, cw, y); y += dy;
    mk_setting(S_DEADZONE, lx, cx, cw, y); y += dy;
    mk_setting(S_RUMBLE, lx, cx, cw, y); y += dy;
    mk_setting(S_STRENGTH, lx, cx, cw, y); y += dy + 12;
    HWND b = mk(L"BUTTON", L"Remap controller...", BS_PUSHBUTTON | WS_TABSTOP, lx, y, pw - 32, 30, ID_REMAP);
    tip(b, L"Runs game_native.exe --pad-map 1 in a console window: press each button it names on player 1's pad (skipped after 8 s). "
           L"It writes a [device] section for that pad to controllers.ini.");
    y += 38;
    b = mk(L"BUTTON", L"Test controller...", BS_PUSHBUTTON | WS_TABSTOP, lx, y, pw - 32, 30, ID_TEST);
    tip(b, L"Runs game_native.exe --pad-test in a console window: lists the pads SDL sees and shows the buttons you press. Close the window to stop.");
    y += 38;
    b = mk(L"BUTTON", L"Keyboard and mouse...", BS_PUSHBUTTON | WS_TABSTOP, lx, y, pw - 32, 30, ID_KEYS);
    tip(b, L"Rebind the keyboard and mouse (two keys or mouse buttons per action) and set the mouse camera. "
           L"Writes the [keyboard] section of controllers.ini.");
    y += 44;
    g_padnote = mk(L"STATIC", L"", SS_LEFT, lx, y, pw - 32, 64, -1);
    SendMessageW(g_padnote, WM_SETFONT, (WPARAM)g_smallfont, FALSE);
    /* bottom bar: status, small buttons, Play */
    int by = k_panel[0].bottom + 12, px = WIN_W - 14 - 150, bw = 84;   /* status: two lines beside four buttons */
    g_status = mk(L"STATIC", L"", SS_LEFT, 16, by + 3, px - 12 - 4 * bw - 3 * 8 - 16 - 16, 36, ID_STATUS);
    b = mk(L"BUTTON", L"Mods...", BS_PUSHBUTTON | WS_TABSTOP, px - 12 - 4 * bw - 3 * 8, by + 3, bw, 34, ID_MODS);
    tip(b, L"Code mods: switch the emulator patches (.pchtxt, .ips) in the mods folder on and off. They are compiled into the game, "
           L"so a change needs a rebuild (about a minute; the window does it).");
    b = mk(L"BUTTON", L"Config folder", BS_PUSHBUTTON | WS_TABSTOP, px - 12 - 3 * bw - 2 * 8, by + 3, bw, 34, ID_CONFIG);
    tip(b, L"Opens the folder with graphics.ini and controllers.ini (every option is explained there).");
    g_save = mk(L"BUTTON", L"Save", BS_PUSHBUTTON | WS_TABSTOP, px - 12 - 2 * bw - 8, by + 3, bw, 34, ID_SAVE);
    mk(L"BUTTON", L"Close", BS_PUSHBUTTON | WS_TABSTOP, px - 12 - bw, by + 3, bw, 34, ID_CLOSE);
    g_play = mk(L"BUTTON", L"Play", BS_OWNERDRAW | WS_TABSTOP, px, by, 150, 40, ID_PLAY);
    SetWindowSubclass(g_play, play_hover, 1, 0);
}

/* ================================================================ keyboard and mouse window
 * [Recomps#1] "Keyboard and mouse..." in the controller panel: the [keyboard] bindings of controllers.ini (read by
 * runtime\nnpad_sdl.c) without a console. Every action has two slots; a click on a slot waits for the next key or mouse
 * button (Delete clears the slot); a key already used by another action moves to this one. The mouse camera options sit
 * below. Save rewrites only the [keyboard] values that changed, in place, like the main window. The defaults are those of
 * nnpad_sdl.c (g_kb_default); keep both in step. */
enum { ID_KSLOT = 600, ID_KMOUSE = 700, ID_KSENS, ID_KINV, ID_KRESET, ID_KSAVE, ID_KCANCEL, ID_KSENSV, ID_KNOTE };
#define KROWS 24
static const struct { const char *key; const wchar_t *label; const char *def; } k_krow[KROWS] = {
    {"LSTICK_UP", L"Move forward", "W"}, {"LSTICK_DOWN", L"Move back", "S"}, {"LSTICK_LEFT", L"Move left", "A"}, {"LSTICK_RIGHT", L"Move right", "D"},
    {"X", L"Punch  (X)", "MOUSE1"}, {"A", L"Kick  (A)", "MOUSE2, ENTER"}, {"B", L"Jump  (B)", "SPACE, BACKSPACE"}, {"Y", L"Shoot  (Y)", "E, MOUSE3"},
    {"ZR", L"Dodge  (ZR)", "LSHIFT"}, {"ZL", L"Lock-on  (ZL)", "LALT"}, {"L", L"Umbran Climax  (L)", "Q"}, {"R", L"R button", "F"},
    {"RSTICK_UP", L"Camera up", "UP"}, {"RSTICK_DOWN", L"Camera down", "DOWN"}, {"RSTICK_LEFT", L"Camera left", "LEFT"}, {"RSTICK_RIGHT", L"Camera right", "RIGHT"},
    {"STICK_R", L"Camera reset  (R stick)", "R"}, {"STICK_L", L"Left stick click", "C"}, {"PLUS", L"Pause  (+)", "ESC"}, {"MINUS", L"Minus  (-)", "TAB"},
    {"UP", L"D-pad up", "1"}, {"RIGHT", L"D-pad right", "2"}, {"DOWN", L"D-pad down", "3"}, {"LEFT", L"D-pad left", "4"},
};
static const struct { int vk; const char *name; const wchar_t *show; } k_vkn[] = {
    {VK_LBUTTON, "MOUSE1", L"Left mouse"}, {VK_RBUTTON, "MOUSE2", L"Right mouse"}, {VK_MBUTTON, "MOUSE3", L"Middle mouse"},
    {VK_XBUTTON1, "MOUSE4", L"Mouse 4"}, {VK_XBUTTON2, "MOUSE5", L"Mouse 5"}, {VK_LEFT, "LEFT", L"Left arrow"}, {VK_UP, "UP", L"Up arrow"},
    {VK_RIGHT, "RIGHT", L"Right arrow"}, {VK_DOWN, "DOWN", L"Down arrow"}, {VK_RETURN, "ENTER", L"Enter"}, {VK_BACK, "BACKSPACE", L"Backspace"},
    {VK_SPACE, "SPACE", L"Space"}, {VK_TAB, "TAB", L"Tab"}, {VK_ESCAPE, "ESC", L"Esc"}, {VK_SHIFT, "SHIFT", L"Shift"}, {VK_CONTROL, "CTRL", L"Ctrl"},
    {VK_MENU, "ALT", L"Alt"}, {VK_LSHIFT, "LSHIFT", L"Left Shift"}, {VK_RSHIFT, "RSHIFT", L"Right Shift"}, {VK_LCONTROL, "LCTRL", L"Left Ctrl"},
    {VK_RCONTROL, "RCTRL", L"Right Ctrl"}, {VK_LMENU, "LALT", L"Left Alt"}, {VK_RMENU, "RALT", L"Right Alt"},
};
static HWND g_kwin, g_kslot[KROWS][2], g_ksens, g_ksensv, g_kmouse, g_kinv, g_knote;
static int g_kv[KROWS][2], g_kv0[KROWS][2], g_kcap = -1, g_kmouse0, g_kinv0, g_ksens0;
static int kvk1(const char *t) {                       /* one key name, as nnpad_sdl.c parse_vk1 reads it */
    char b[24];
    while (*t == ' ' || *t == '\t') t++;
    snprintf(b, sizeof b, "%s", t);
    for (char *p = b + strlen(b); p > b && (p[-1] == ' ' || p[-1] == '\t' || p[-1] == '\r'); ) *--p = 0;
    if (!b[0] || !_stricmp(b, "none")) return 0;
    if (!b[1]) { char ch = b[0]; if (ch >= 'a' && ch <= 'z') ch -= 32; return (unsigned char)ch; }
    for (size_t i = 0; i < sizeof k_vkn / sizeof k_vkn[0]; i++) if (!_stricmp(b, k_vkn[i].name)) return k_vkn[i].vk;
    if ((b[0] == 'F' || b[0] == 'f') && atoi(b + 1) >= 1 && atoi(b + 1) <= 12) return VK_F1 + atoi(b + 1) - 1;
    if (!_strnicmp(b, "NUM", 3) && b[3] >= '0' && b[3] <= '9') return VK_NUMPAD0 + b[3] - '0';
    if (b[0] == '0' && (b[1] == 'x' || b[1] == 'X')) return (int)strtol(b, NULL, 16);
    return 0;
}
static void kparse(const char *v, int out[2]) {
    char b[64];
    snprintf(b, sizeof b, "%s", v);
    char *c = strchr(b, ',');
    if (c) *c = 0;
    out[0] = kvk1(b); out[1] = c ? kvk1(c + 1) : 0;
}
static void kname(int vk, char *o, size_t n) {          /* the file's name of a key */
    if (!vk) { snprintf(o, n, "none"); return; }
    for (size_t i = 0; i < sizeof k_vkn / sizeof k_vkn[0]; i++) if (k_vkn[i].vk == vk) { snprintf(o, n, "%s", k_vkn[i].name); return; }
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) { snprintf(o, n, "%c", vk); return; }
    if (vk >= VK_F1 && vk <= VK_F12) { snprintf(o, n, "F%d", vk - VK_F1 + 1); return; }
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) { snprintf(o, n, "NUM%d", vk - VK_NUMPAD0); return; }
    snprintf(o, n, "0x%02X", vk);
}
static void kshow(int vk, wchar_t *o, size_t n) {       /* the window's name of a key */
    if (!vk) { swprintf(o, n, L"-"); return; }
    for (size_t i = 0; i < sizeof k_vkn / sizeof k_vkn[0]; i++) if (k_vkn[i].vk == vk) { swprintf(o, n, L"%s", k_vkn[i].show); return; }
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) { swprintf(o, n, L"Num %d", vk - VK_NUMPAD0); return; }
    char b[16]; kname(vk, b, sizeof b);
    swprintf(o, n, L"%hs", b);
}
static void kvalue(int r, char *o, size_t n) {          /* a row as the file writes it: "E, MOUSE3" */
    char a[16], b[16];
    kname(g_kv[r][0], a, sizeof a); kname(g_kv[r][1], b, sizeof b);
    if (g_kv[r][1]) snprintf(o, n, "%s, %s", g_kv[r][0] ? a : "none", b); else snprintf(o, n, "%s", a);
}
static void ksens_show(void) {
    wchar_t t[16];
    swprintf(t, 16, L"%.1f", (int)SendMessageW(g_ksens, TBM_GETPOS, 0, 0) / 10.0);
    SetWindowTextW(g_ksensv, t);
}
static void kshow_all(void) {
    for (int r = 0; r < KROWS; r++) for (int k = 0; k < 2; k++) {
        wchar_t t[48];
        if (g_kcap == r * 2 + k) swprintf(t, 48, L"press a key...");
        else kshow(g_kv[r][k], t, 48);
        SetWindowTextW(g_kslot[r][k], t);
    }
}
static void kload(int defaults) {
    Ini f;
    ini_load(&f, g_ini[F_PAD]);
    char v[64];
    for (int r = 0; r < KROWS; r++) {
        if (defaults || !ini_get(&f, "keyboard", k_krow[r].key, 1, v, sizeof v)) snprintf(v, sizeof v, "%s", k_krow[r].def);
        kparse(v, g_kv[r]);
        if (!defaults) { g_kv0[r][0] = g_kv[r][0]; g_kv0[r][1] = g_kv[r][1]; }
    }
    int cam = 1, inv = 0, sens = 10;
    if (!defaults) {
        if (ini_get(&f, "keyboard", "mouse_camera", 1, v, sizeof v)) cam = !(!_stricmp(v, "off") || !strcmp(v, "0") || !_stricmp(v, "no"));
        if (ini_get(&f, "keyboard", "mouse_invert_y", 1, v, sizeof v)) inv = !_stricmp(v, "on") || !strcmp(v, "1") || !_stricmp(v, "yes");
        if (ini_get(&f, "keyboard", "mouse_sensitivity", 1, v, sizeof v) && atof(v) > 0) sens = (int)(atof(v) * 10 + 0.5);
        g_kmouse0 = cam; g_kinv0 = inv; g_ksens0 = sens;
    }
    ini_free(&f);
    SendMessageW(g_kmouse, BM_SETCHECK, cam ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(g_kinv, BM_SETCHECK, inv ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(g_ksens, TBM_SETPOS, TRUE, sens < 1 ? 1 : sens > 50 ? 50 : sens);
    ksens_show();
    g_kcap = -1;
    kshow_all();
}
static int ksave(void) {
    Ini f;
    int n = 0;
    ini_load(&f, g_ini[F_PAD]);                           /* fresh from disk, as save_all */
    for (int r = 0; r < KROWS; r++) if (g_kv[r][0] != g_kv0[r][0] || g_kv[r][1] != g_kv0[r][1]) {
        char v[64]; kvalue(r, v, sizeof v);
        ini_set(&f, "keyboard", k_krow[r].key, 1, v); n++;
    }
    int cam = SendMessageW(g_kmouse, BM_GETCHECK, 0, 0) == BST_CHECKED, inv = SendMessageW(g_kinv, BM_GETCHECK, 0, 0) == BST_CHECKED;
    int sens = (int)SendMessageW(g_ksens, TBM_GETPOS, 0, 0);
    if (cam != g_kmouse0) { ini_set(&f, "keyboard", "mouse_camera", 1, cam ? "on" : "off"); n++; }
    if (inv != g_kinv0) { ini_set(&f, "keyboard", "mouse_invert_y", 1, inv ? "on" : "off"); n++; }
    if (sens != g_ksens0) { char v[16]; snprintf(v, sizeof v, "%.1f", sens / 10.0); ini_set(&f, "keyboard", "mouse_sensitivity", 1, v); n++; }
    int ok = !n || ini_save(&f, g_ini[F_PAD]);
    ini_free(&f);
    if (!ok) { wchar_t m[MAX_PATH + 64]; swprintf(m, MAX_PATH + 64, L"Could not write %s.", g_ini[F_PAD]); MessageBoxW(g_kwin, m, L"B2recomp", MB_ICONERROR); return 0; }
    wchar_t t[96]; swprintf(t, 96, L"Keyboard and mouse: %d setting(s) saved.", n);
    if (!g_proc) SetWindowTextW(g_status, t);
    return 1;
}
static void kassign(int vk) {                           /* the slot waiting for a key gets vk (0 = cleared) */
    int r = g_kcap / 2, k = g_kcap % 2;
    wchar_t t[160] = L"";
    if (vk) for (int r2 = 0; r2 < KROWS; r2++) for (int k2 = 0; k2 < 2; k2++) if ((r2 != r || k2 != k) && g_kv[r2][k2] == vk) {
        wchar_t nm[48]; kshow(vk, nm, 48);
        swprintf(t, 160, L"%s moved here from %s.", nm, k_krow[r2].label);
        g_kv[r2][k2] = 0;
    }
    g_kv[r][k] = vk;
    g_kcap = -1;
    kshow_all();
    SetWindowTextW(g_knote, t[0] ? t : L"Click a slot, then press a key or a mouse button. Delete clears a slot.");
}
/* the launcher's message loop asks this first: while a slot waits, the next key or mouse button in this window is its key */
static int kcapture(const MSG *m) {
    if (!g_kwin || g_kcap < 0 || (m->hwnd != g_kwin && !IsChild(g_kwin, m->hwnd))) return 0;
    int vk = 0;
    switch (m->message) {
    case WM_KEYDOWN: case WM_SYSKEYDOWN: {
        vk = (int)m->wParam;
        int ext = (m->lParam >> 24) & 1;
        if (vk == VK_SHIFT) vk = (int)MapVirtualKeyW((m->lParam >> 16) & 0xff, MAPVK_VSC_TO_VK_EX);
        else if (vk == VK_CONTROL) vk = ext ? VK_RCONTROL : VK_LCONTROL;
        else if (vk == VK_MENU) vk = ext ? VK_RMENU : VK_LMENU;
        if (vk == VK_DELETE) vk = 0;
        kassign(vk);
        return 1;
    }
    case WM_KEYUP: case WM_SYSKEYUP: case WM_CHAR: case WM_SYSCHAR: return 1;
    case WM_LBUTTONDOWN: vk = VK_LBUTTON; break;
    case WM_RBUTTONDOWN: vk = VK_RBUTTON; break;
    case WM_MBUTTONDOWN: vk = VK_MBUTTON; break;
    case WM_XBUTTONDOWN: vk = HIWORD(m->wParam) == XBUTTON1 ? VK_XBUTTON1 : VK_XBUTTON2; break;
    case WM_LBUTTONUP: case WM_RBUTTONUP: case WM_MBUTTONUP: case WM_XBUTTONUP: case WM_CONTEXTMENU: return 1;
    default: return 0;
    }
    kassign(vk);
    return 1;
}
#define KW_W 900
#define KW_H 536
static HWND kmk(const wchar_t *cls, const wchar_t *text, DWORD style, int x, int y, int w, int h, int id) {
    HWND c = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, S(x), S(y), S(w), S(h), g_kwin, (HMENU)(INT_PTR)id, GetModuleHandleW(NULL), NULL);
    SendMessageW(c, WM_SETFONT, (WPARAM)g_font, FALSE);
    if (!wcscmp(cls, L"BUTTON") || !wcscmp(cls, TRACKBAR_CLASSW)) { if (p_AllowDarkModeForWindow) p_AllowDarkModeForWindow(c, TRUE); SetWindowTheme(c, L"DarkMode_Explorer", NULL); }
    return c;
}
static LRESULT CALLBACK kwndproc(HWND w, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
    case WM_CREATE: {
        g_kwin = w;
        HWND t = kmk(L"STATIC", L"Two keys or mouse buttons per action. The layout follows the original Bayonetta on PC.", SS_LEFT, 20, 14, KW_W - 40, 20, -1);
        (void)t;
        for (int r = 0; r < KROWS; r++) {
            int col = r / 12, row = r % 12, x = 20 + col * 440, y = 46 + row * 32;
            kmk(L"STATIC", k_krow[r].label, SS_LEFT, x, y + 5, 170, 20, -1);
            for (int k = 0; k < 2; k++) g_kslot[r][k] = kmk(L"BUTTON", L"", BS_PUSHBUTTON | WS_TABSTOP, x + 172 + k * 126, y, 120, 28, ID_KSLOT + r * 2 + k);
        }
        int y = 46 + 12 * 32 + 14;
        g_kmouse = kmk(L"BUTTON", L"", BS_AUTOCHECKBOX | WS_TABSTOP, 20, y + 2, 20, 20, ID_KMOUSE);
        kmk(L"STATIC", L"The mouse moves the camera", SS_LEFT, 44, y + 4, 220, 20, -1);
        kmk(L"STATIC", L"Mouse sensitivity", SS_LEFT, 300, y + 4, 130, 20, -1);
        g_ksens = kmk(TRACKBAR_CLASSW, L"", TBS_HORZ | WS_TABSTOP, 430, y, 200, 26, ID_KSENS);
        SendMessageW(g_ksens, TBM_SETRANGE, TRUE, MAKELPARAM(1, 50));
        SendMessageW(g_ksens, TBM_SETTICFREQ, 5, 0);
        g_ksensv = kmk(L"STATIC", L"", SS_LEFT, 636, y + 4, 40, 20, ID_KSENSV);
        g_kinv = kmk(L"BUTTON", L"", BS_AUTOCHECKBOX | WS_TABSTOP, 690, y + 2, 20, 20, ID_KINV);
        kmk(L"STATIC", L"Invert vertical", SS_LEFT, 714, y + 4, 160, 20, -1);
        y += 40;
        g_knote = kmk(L"STATIC", L"Click a slot, then press a key or a mouse button. Delete clears a slot.", SS_LEFT, 20, y + 8, 470, 34, ID_KNOTE);
        SendMessageW(g_knote, WM_SETFONT, (WPARAM)g_smallfont, FALSE);
        kmk(L"BUTTON", L"Reset to defaults", BS_PUSHBUTTON | WS_TABSTOP, 500, y, 150, 34, ID_KRESET);
        kmk(L"BUTTON", L"Save", BS_PUSHBUTTON | WS_TABSTOP, 660, y, 100, 34, ID_KSAVE);
        kmk(L"BUTTON", L"Cancel", BS_PUSHBUTTON | WS_TABSTOP, 770, y, 100, 34, ID_KCANCEL);
        kload(0);
        return 0;
    }
    case WM_ERASEBKGND: { RECT r; GetClientRect(w, &r); FillRect((HDC)wp, &r, g_brpanel); return 1; }
    case WM_CTLCOLORSTATIC: {
        HDC dc = (HDC)wp;
        SetTextColor(dc, (HWND)lp == g_knote ? C_NOTE : (HWND)lp == g_ksensv ? C_VALUE : C_TEXT);
        SetBkColor(dc, C_PANEL);
        return (LRESULT)g_brpanel;
    }
    case WM_HSCROLL: if ((HWND)lp == g_ksens) ksens_show(); return 0;
    case WM_COMMAND: {
        int id = LOWORD(wp);
        if (id >= ID_KSLOT && id < ID_KSLOT + KROWS * 2) {
            g_kcap = id - ID_KSLOT;
            kshow_all();
            SetWindowTextW(g_knote, L"Press a key or click a mouse button for this slot (Delete clears it).");
        } else if (id == ID_KRESET) { kload(1); SetWindowTextW(g_knote, L"Defaults restored: Save keeps them."); }
        else if (id == ID_KSAVE) { if (ksave()) DestroyWindow(w); }
        else if (id == ID_KCANCEL || id == IDCANCEL) DestroyWindow(w);
        return 0;
    }
    case WM_CLOSE: DestroyWindow(w); return 0;
    case WM_DESTROY: g_kwin = NULL; g_kcap = -1; EnableWindow(g_main, TRUE); SetForegroundWindow(g_main); return 0;
    }
    return DefWindowProcW(w, m, wp, lp);
}
static void keys_window(void) {
    if (g_kwin) { SetForegroundWindow(g_kwin); return; }
    static int reg;
    if (!reg) {
        WNDCLASSEXW wc = { sizeof wc };
        wc.lpfnWndProc = kwndproc; wc.hInstance = GetModuleHandleW(NULL); wc.lpszClassName = L"B2recompKeys";
        wc.hCursor = LoadCursorW(NULL, IDC_ARROW); wc.hbrBackground = g_brpanel;
        reg = RegisterClassExW(&wc) != 0;
    }
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN;
    RECT r = { 0, 0, S(KW_W), S(KW_H) };
    AdjustWindowRectEx(&r, style, FALSE, WS_EX_CONTROLPARENT | WS_EX_DLGMODALFRAME);
    RECT mr; GetWindowRect(g_main, &mr);
    int ww = r.right - r.left, wh = r.bottom - r.top;
    HWND w = CreateWindowExW(WS_EX_CONTROLPARENT | WS_EX_DLGMODALFRAME, L"B2recompKeys", L"Keyboard and mouse", style,
                             mr.left + (mr.right - mr.left - ww) / 2, mr.top + (mr.bottom - mr.top - wh) / 2, ww, wh, g_main, NULL, GetModuleHandleW(NULL), NULL);
    if (!w) return;
    {   BOOL on = TRUE; COLORREF text = RGB(200, 214, 240);  /* the main window's dark title bar */
        DwmSetWindowAttribute(w, 20, &on, sizeof on);
        DwmSetWindowAttribute(w, 35, &g_caption, sizeof g_caption);
        DwmSetWindowAttribute(w, 36, &text, sizeof text); }
    EnableWindow(g_main, FALSE);
    ShowWindow(w, SW_SHOW);
}

static void load_art(void) {                                 /* g_art = the header, W x H device pixels */
    wchar_t p[MAX_PATH];
    swprintf(p, MAX_PATH, L"%s\\launcher_art.bmp", g_buildd);
    HBITMAP src = (HBITMAP)LoadImageW(NULL, p, IMAGE_BITMAP, 0, 0, LR_LOADFROMFILE | LR_CREATEDIBSECTION);
    if (!src) return;
    BITMAP bm; GetObjectW(src, sizeof bm, &bm);
    int W = S(WIN_W), H = S(HEADER);
    BITMAPINFO bi = { { sizeof(BITMAPINFOHEADER), W, -H, 1, 32, BI_RGB } };
    unsigned char *px;
    g_art = CreateDIBSection(NULL, &bi, DIB_RGB_COLORS, (void **)&px, NULL, 0);
    if (!g_art) { DeleteObject(src); return; }
    HDC sd = CreateCompatibleDC(NULL), dd = CreateCompatibleDC(NULL);
    HGDIOBJ os = SelectObject(sd, src), od = SelectObject(dd, g_art);
    SetStretchBltMode(dd, HALFTONE); SetBrushOrgEx(dd, 0, 0, NULL);
    int sw = bm.bmWidth, sh = (int)((double)sw * HEADER / WIN_W), sy = (int)(bm.bmHeight * 0.45) - sh / 2;   /* the logo: 45 % down the title screen */
    if (sy < 0) sy = 0;
    if (sy + sh > bm.bmHeight) sh = bm.bmHeight - sy;
    StretchBlt(dd, 0, 0, W, H, sd, 0, sy, sw, sh, SRCCOPY);
    GdiFlush();
    const int bg[3] = { GetBValue(C_BG), GetGValue(C_BG), GetRValue(C_BG) };
    for (int y = 0; y < H; y++) {                            /* the lower 45 % fades into the background */
        double t = (double)y / (H - 1), f = t < 0.55 ? 1.0 : 1.0 - (t - 0.55) / 0.45;
        f = f * f * (3 - 2 * f);
        for (int x = 0; x < W; x++) {
            unsigned char *q = px + ((size_t)y * W + x) * 4;
            for (int c = 0; c < 3; c++) q[c] = (unsigned char)(q[c] * f + bg[c] * (1 - f) + 0.5);
        }
    }
    g_caption = RGB(px[2], px[1], px[0]);                    /* the title bar takes the colour of the art's top edge */
    SelectObject(sd, os); SelectObject(dd, od); DeleteDC(sd); DeleteDC(dd); DeleteObject(src);
}

static int g_mode;                                          /* 0 = settings + Play, 1 = build screen (below) */
static const RECT k_bpanel = { 14, 232, 766, 692 };
static void spaced_text(HDC dc, const wchar_t *t, int x, int y, int extra) {
    SetTextCharacterExtra(dc, extra);
    TextOutW(dc, x, y, t, (int)wcslen(t));
    SetTextCharacterExtra(dc, 0);
}
static void paint(HWND w) {
    PAINTSTRUCT ps;
    HDC wdc = BeginPaint(w, &ps);
    RECT rc; GetClientRect(w, &rc);
    HDC dc = CreateCompatibleDC(wdc);
    HBITMAP bb = CreateCompatibleBitmap(wdc, rc.right, rc.bottom);
    HGDIOBJ ob = SelectObject(dc, bb), of = SelectObject(dc, g_font);
    FillRect(dc, &rc, g_brbg);
    SetBkMode(dc, TRANSPARENT);
    if (g_art) {
        HDC ad = CreateCompatibleDC(wdc);
        HGDIOBJ oa = SelectObject(ad, g_art);
        BitBlt(dc, 0, 0, S(WIN_W), S(HEADER), ad, 0, 0, SRCCOPY);
        SelectObject(ad, oa); DeleteDC(ad);
    } else {
        RECT t = { 0, S(60), rc.right, S(150) };
        SelectObject(dc, g_titlefont); SetTextColor(dc, C_GOLD);
        SetTextCharacterExtra(dc, S(6));
        DrawTextW(dc, L"B2RECOMP", -1, &t, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
        SetTextCharacterExtra(dc, 0);
    }
    SelectObject(dc, g_headfont); SetTextColor(dc, RGB(150, 178, 226));
    spaced_text(dc, L"BAYONETTA 2 RECOMPILED", S(18), S(14), S(3));
    HPEN edge = CreatePen(PS_SOLID, 1, C_EDGE), gold = CreatePen(PS_SOLID, S(2), C_GOLD);
    static const wchar_t *k_title[2] = { L"GRAPHICS", L"CONTROLLER" };
    for (int k = 0; k < (g_mode ? 1 : 2); k++) {
        const RECT *pr = g_mode ? &k_bpanel : &k_panel[k];
        RECT p = { S(pr->left), S(pr->top), S(pr->right), S(pr->bottom) };
        HGDIOBJ op = SelectObject(dc, edge), obr = SelectObject(dc, g_brpanel);
        RoundRect(dc, p.left, p.top, p.right, p.bottom, S(12), S(12));
        MoveToEx(dc, p.left + S(16), p.top + S(36), NULL); LineTo(dc, p.right - S(16), p.top + S(36));
        SelectObject(dc, gold);
        MoveToEx(dc, p.left + S(16), p.top + S(36), NULL); LineTo(dc, p.left + S(52), p.top + S(36));
        SelectObject(dc, op); SelectObject(dc, obr);
        SetTextColor(dc, C_GOLD);
        spaced_text(dc, g_mode ? L"BUILD THE GAME" : k_title[k], p.left + S(16), p.top + S(12), S(3));
    }
    DeleteObject(edge); DeleteObject(gold);
    BitBlt(wdc, 0, 0, rc.right, rc.bottom, dc, 0, 0, SRCCOPY);
    SelectObject(dc, of); SelectObject(dc, ob); DeleteObject(bb); DeleteDC(dc);
    EndPaint(w, &ps);
}
static void draw_play(const DRAWITEMSTRUCT *d) {
    RECT r = d->rcItem;
    int dis = d->itemState & ODS_DISABLED, down = d->itemState & ODS_SELECTED;
    FillRect(d->hDC, &r, g_brbg);
    HRGN rg = CreateRoundRectRgn(r.left, r.top, r.right + 1, r.bottom + 1, S(10), S(10));
    SelectClipRgn(d->hDC, rg);
    COLORREF a = dis ? RGB(52, 56, 70) : down ? RGB(196, 140, 52) : g_playhot ? RGB(255, 226, 150) : RGB(244, 208, 124);
    COLORREF b = dis ? RGB(40, 43, 56) : down ? RGB(150, 100, 30) : g_playhot ? RGB(214, 156, 62) : RGB(194, 136, 48);
    TRIVERTEX v[2] = { { r.left, r.top, (COLOR16)(GetRValue(a) << 8), (COLOR16)(GetGValue(a) << 8), (COLOR16)(GetBValue(a) << 8), 0 },
                       { r.right, r.bottom, (COLOR16)(GetRValue(b) << 8), (COLOR16)(GetGValue(b) << 8), (COLOR16)(GetBValue(b) << 8), 0 } };
    GRADIENT_RECT gr = { 0, 1 };
    GradientFill(d->hDC, v, 2, &gr, 1, GRADIENT_FILL_RECT_V);
    SelectClipRgn(d->hDC, NULL);
    HBRUSH fr = CreateSolidBrush(dis ? C_EDGE : RGB(120, 78, 22));
    FrameRgn(d->hDC, rg, fr, 1, 1);
    DeleteObject(fr); DeleteObject(rg);
    wchar_t t[32]; GetWindowTextW(d->hwndItem, t, 32); CharUpperW(t);
    SetBkMode(d->hDC, TRANSPARENT);
    SetTextColor(d->hDC, dis ? C_DIM : RGB(30, 18, 4));
    HGDIOBJ of = SelectObject(d->hDC, g_bigfont);
    SetTextCharacterExtra(d->hDC, S(4));
    RECT tr = r; tr.left += S(4);                            /* the extra spacing also follows the last letter */
    DrawTextW(d->hDC, t, -1, &tr, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
    SetTextCharacterExtra(d->hDC, 0);
    SelectObject(d->hDC, of);
    if ((d->itemState & ODS_FOCUS) && !(d->itemState & ODS_NOFOCUSRECT)) { RECT f = r; InflateRect(&f, -S(4), -S(4)); DrawFocusRect(d->hDC, &f); }
}

/* ================================================================ build screen
 * The first start of the builder package (B2recomp.exe at its top): while build\game.dll, build\game_native.exe or
 * targets\exefs\main.elf is missing (or with --build, or after an update: needs_update) the window shows this screen
 * instead of the settings. The player
 * chooses the dump folder (Browse, typed, or dragged onto the window); Build runs scripts\setup.py --dump <folder> with
 * deps\python\python.exe when the package has it (else python.exe from PATH), in a hidden console of its own inside a job
 * object, so Close stops it and every compiler it started. Its output goes to the log box; the ten steps are ticked off
 * from its "[i/10] step ..." lines and the compile step's progress comes from the "[n/388]" lines of build\logs\compile.log.
 * When it succeeds the window turns into the launcher. */
enum { ID_BPATH = 400, ID_BBROWSE, ID_BUILD, ID_BLOGS, ID_BCHECK, ID_BLOG, ID_BPROG, ID_BSTEP0 = 420 };
enum { WM_APP_LINE = WM_APP + 1, WM_APP_DONE };
#define NSTEP 10
#define BSTEP_COMPILE 6                                     /* index of setup.py's compile step */
static const wchar_t *k_step_name[NSTEP] = { L"Check the bundled tools", L"Check your dump", L"Convert the program files",
    L"Apply your code mods (if any)", L"Generate tables", L"Translate the game code", L"Compile it (the longest step)", L"Make the icon",
    L"Link the game and the launcher", L"Self-test" };
static const int k_step_weight[NSTEP] = { 1, 2, 1, 1, 2, 11, 240, 1, 8, 2 };   /* seconds on a 16-thread PC (finding 102) */
static int g_bstate[NSTEP];                                 /* 0 waiting, 1 running, 2 done, 3 failed */
static int g_bok, g_bcompile, g_bundled;                    /* dump folder looks right; compile per mille; deps\python */
static HWND g_bpath, g_bbrowse, g_bcheck, g_blog, g_bprog, g_bstep[NSTEP], g_bbtn, g_blogs, g_bclose, g_bintro, g_blabel;
static HANDLE g_bproc, g_bjob;
static DWORD g_bt0;
static wchar_t g_python[MAX_PATH], g_dump[MAX_PATH];

static int is_dir(const wchar_t *p) { DWORD a = GetFileAttributesW(p); return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY); }
static int game_built(void) {
    wchar_t dll[MAX_PATH];
    swprintf(dll, MAX_PATH, L"%s\\game.dll", g_buildd);
    return file_exists(g_game) && file_exists(dll) && file_exists(g_elf);
}
/* [Recomps#1] An update unpacked over the old folder (as on Linux, launcher_linux.py needs_update): the package's VERSION is
 * not the one setup.py last built (build\setup_state.json "version", kept after a complete run; a build from before v0.2.10
 * has none). The build screen then opens with that build's dump folder, and Build redoes what changed (setup.py: tables,
 * lift, compile of the changed files, link, check; a minute or two). Settings and saves stay: the package brings its
 * settings as config\defaults (make_package.py), copied to config\ only when missing (wWinMain). */
static int g_update;
static int needs_update(void) {
    wchar_t p[MAX_PATH];
    char v[64];
    static char b[65536];
    swprintf(p, MAX_PATH, L"%s\\VERSION", g_root);
    FILE *f = _wfopen(p, L"rb");
    if (!f) return 0;                                       /* a source checkout, not a builder package */
    size_t n = fread(v, 1, sizeof v - 1, f);
    fclose(f);
    v[n] = 0;
    while (n && (v[n - 1] == '\n' || v[n - 1] == '\r' || v[n - 1] == ' ')) v[--n] = 0;
    if (!n) return 0;
    swprintf(p, MAX_PATH, L"%s\\setup_state.json", g_buildd);
    if (!(f = _wfopen(p, L"rb"))) return 0;                 /* never built: the first build, not an update */
    n = fread(b, 1, sizeof b - 1, f);
    fclose(f);
    b[n] = 0;
    const char *d = strstr(b, "\"done\": {"), *q = strstr(b, "\"version\": \"");
    if (!d || d[9] == '}') return 0;                        /* no step done yet */
    if (!q) return 1;
    q += 12;
    return strncmp(q, v, strlen(v)) != 0 || q[strlen(v)] != '"';
}
static void find_python(void) {
    swprintf(g_python, MAX_PATH, L"%s\\deps\\python\\python.exe", g_root);
    g_bundled = file_exists(g_python);
    if (g_bundled) return;
    if (!SearchPathW(NULL, L"python.exe", NULL, MAX_PATH, g_python, NULL) || wcsstr(g_python, L"WindowsApps")) g_python[0] = 0;
}
static void prefill_dump(void) {                            /* the dump folder of an earlier run (build\setup_state.json) */
    wchar_t p[MAX_PATH], out[MAX_PATH];
    swprintf(p, MAX_PATH, L"%s\\setup_state.json", g_buildd);
    FILE *f = _wfopen(p, L"rb");
    if (!f) return;
    static char b[65536];
    size_t n = fread(b, 1, sizeof b - 1, f);
    fclose(f);
    b[n] = 0;
    char *q = strstr(b, "\"dump\": \"");
    if (!q) return;
    size_t k = 0;
    for (q += 9; *q && *q != '"' && k < MAX_PATH - 1; q++) {   /* JSON string: \\ \" and \uXXXX (json.dump writes ASCII) */
        if (*q == '\\' && q[1] == 'u' && q[2] && q[3] && q[4] && q[5]) { char h[5] = { q[2], q[3], q[4], q[5], 0 }; out[k++] = (wchar_t)strtol(h, NULL, 16); q += 5; }
        else if (*q == '\\' && q[1]) out[k++] = (wchar_t)(unsigned char)*++q;
        else out[k++] = (wchar_t)(unsigned char)*q;
    }
    out[k] = 0;
    SetWindowTextW(g_bpath, out);
}
static void dump_check(void) {                              /* does the folder in the box look like an extracted dump? */
    wchar_t p[MAX_PATH], a[MAX_PATH + 32], b[MAX_PATH + 32];
    GetWindowTextW(g_bpath, p, MAX_PATH);
    wchar_t *s = p;
    while (*s == L' ' || *s == L'"') s++;
    size_t n = wcslen(s);
    while (n > 3 && (s[n - 1] == L'\\' || s[n - 1] == L' ' || s[n - 1] == L'"')) s[--n] = 0;
    wcscpy(g_dump, s);
    const wchar_t *msg;
    g_bok = 0;
    swprintf(a, MAX_PATH + 32, L"%s\\exefs\\main", g_dump); swprintf(b, MAX_PATH + 32, L"%s\\romfs", g_dump);
    if (!*g_dump) msg = L"Drag the folder that holds exefs\\ and romfs\\ onto this window, or choose it with Browse.";
    else if (!is_dir(g_dump)) msg = L"This folder does not exist.";
    else if (swprintf(a, MAX_PATH + 32, L"%s\\main.npdm", g_dump), file_exists(a))
        msg = L"This is the exefs folder itself: choose the folder above it, the one that holds exefs\\ and romfs\\.";
    else if (swprintf(a, MAX_PATH + 32, L"%s\\data000.cpk", g_dump), file_exists(a))
        msg = L"This is the romfs folder itself: choose the folder above it, the one that holds exefs\\ and romfs\\.";
    else if (swprintf(a, MAX_PATH + 32, L"%s\\exefs\\main", g_dump), !file_exists(a))
        msg = L"No exefs\\main here: choose the folder that holds exefs\\ and romfs\\.";
    else if (!is_dir(b)) msg = L"No romfs\\ folder here: the dump needs both exefs\\ and romfs\\.";
    else { g_bok = 1; msg = L"\x2713  exefs\\ and romfs\\ found. Press Build: the exact game version is checked first."; }
    SetWindowTextW(g_bcheck, msg);
    InvalidateRect(g_bcheck, NULL, TRUE);
    EnableWindow(g_bbtn, g_bok && !g_bproc);
}
static void step_show(int i) {
    static const wchar_t *glyph[4] = { L"\x25CB", L"\x25B6", L"\x2713", L"\x2717" };   /* waiting, running, done, failed */
    wchar_t t[96];
    swprintf(t, 96, L"%s   %s", glyph[g_bstate[i]], k_step_name[i]);
    SetWindowTextW(g_bstep[i], t);
    InvalidateRect(g_bstep[i], NULL, TRUE);
}
static void progress_update(void) {
    int total = 0, done = 0;
    for (int i = 0; i < NSTEP; i++) {
        total += k_step_weight[i];
        if (g_bstate[i] == 2) done += k_step_weight[i];
        else if (g_bstate[i] == 1 && i == BSTEP_COMPILE) done += k_step_weight[i] * g_bcompile / 1000;
    }
    SendMessageW(g_bprog, PBM_SETPOS, (WPARAM)(total ? done * 1000 / total : 0), 0);
}
static void compile_progress(void) {                        /* the last "[n/m]" line of build\logs\compile.log */
    if (g_bstate[BSTEP_COMPILE] != 1) return;
    wchar_t p[MAX_PATH];
    swprintf(p, MAX_PATH, L"%s\\logs\\compile.log", g_buildd);
    HANDLE h = CreateFileW(p, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    LARGE_INTEGER sz, off;
    char b[512];
    DWORD got = 0;
    GetFileSizeEx(h, &sz);
    off.QuadPart = sz.QuadPart > 511 ? sz.QuadPart - 511 : 0;
    SetFilePointerEx(h, off, NULL, FILE_BEGIN);
    ReadFile(h, b, 511, &got, NULL);
    CloseHandle(h);
    b[got] = 0;
    char *q = NULL;
    for (char *s = b; (s = strstr(s, "\n[")) != NULL; s++) q = s + 1;
    int a, n;
    if (q && sscanf(q, "[%d/%d]", &a, &n) == 2 && n > 0 && a <= n) g_bcompile = a * 1000 / n;
}
static void log_append(HWND box, const wchar_t *s) {
    int n = GetWindowTextLengthW(box);
    if (n > 200000) { SendMessageW(box, EM_SETSEL, 0, n / 2); SendMessageW(box, EM_REPLACESEL, FALSE, (LPARAM)L""); n = GetWindowTextLengthW(box); }
    SendMessageW(box, EM_SETSEL, n, n);
    SendMessageW(box, EM_REPLACESEL, FALSE, (LPARAM)s);
    SendMessageW(box, EM_REPLACESEL, FALSE, (LPARAM)L"\r\n");
}
static void build_line(const wchar_t *s) {                 /* one line of setup.py's output */
    log_append(g_blog, s);
    if (s[0] != L'[') return;
    wchar_t *e;
    long i = wcstol(s + 1, &e, 10), n;
    if (*e != L'/') return;
    n = wcstol(e + 1, &e, 10);
    if (*e != L']' || n != NSTEP || i < 1 || i > NSTEP) return;
    i--;
    if (wcsstr(e, L": FAILED")) g_bstate[i] = 3;
    else if (wcsstr(e, L": ok")) g_bstate[i] = 2;
    else if (wcsstr(e, L" ...")) {
        g_bstate[i] = 1;
        for (int k = 0; k < i; k++) if (g_bstate[k] == 0) { g_bstate[k] = 2; step_show(k); }   /* done in an earlier run */
    } else return;
    step_show((int)i);
    progress_update();
}

typedef struct { HANDLE rd, proc; HWND to; } Reader;
static void post_line(HWND to, const char *s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    wchar_t *w = malloc((size_t)n * sizeof *w);
    if (!w) return;
    MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n);
    if (!PostMessageW(to, WM_APP_LINE, 0, (LPARAM)w)) free(w);
}
static DWORD WINAPI reader(LPVOID arg) {                   /* setup.py's stdout, line by line, to the window r->to */
    Reader *r = arg;
    static char line[8192];
    char buf[4096];
    DWORD got, code = 1;
    int n = 0;
    while (ReadFile(r->rd, buf, sizeof buf, &got, NULL) && got) {
        for (DWORD k = 0; k < got; k++) {
            if (buf[k] == '\r') continue;
            if (buf[k] == '\n' || n == (int)sizeof line - 1) { line[n] = 0; post_line(r->to, line); n = 0; if (buf[k] == '\n') continue; }
            line[n++] = buf[k];
        }
    }
    if (n) { line[n] = 0; post_line(r->to, line); }
    WaitForSingleObject(r->proc, INFINITE);
    GetExitCodeProcess(r->proc, &code);
    CloseHandle(r->rd);
    PostMessageW(r->to, WM_APP_DONE, code, 0);
    free(r);
    return 0;
}
static void build_enable(int on) {
    EnableWindow(g_bpath, on); EnableWindow(g_bbrowse, on);
    EnableWindow(g_bbtn, on && g_bok);
    SetWindowTextW(g_bbtn, on ? L"Build" : L"Building");
}
/* python scripts\setup.py <args> in a hidden console of its own inside a job object (closing *job stops it and every
   compiler it started); its output arrives at window `to` as WM_APP_LINE (one line, malloc'd) and WM_APP_DONE (exit code) */
static int start_setup(const wchar_t *args, HWND to, HANDLE *proc, HANDLE *job) {
    if (!*g_python) {
        MessageBoxW(to, L"Python was not found. The builder package brings its own in deps\\python; from the source, install "
                        L"Python 3.10+ (python.org) and start this again.", L"B2recomp", MB_ICONERROR);
        return 0;
    }
    if (g_bundled) {                                        /* only the bundled tools: nothing of this PC's setup may leak in */
        static const wchar_t *k_off[] = { L"PYTHONHOME", L"PYTHONPATH", L"PYTHONSTARTUP", L"LLVM_MINGW", L"LLVM_ROOT", L"SDL3_ROOT",
                                          L"VULKAN_HEADERS", L"GLSLANG_ROOT", L"GLSLANG_DLL", L"TOOLCHAIN" };
        for (int i = 0; i < (int)(sizeof k_off / sizeof k_off[0]); i++) SetEnvironmentVariableW(k_off[i], NULL);
        SetEnvironmentVariableW(L"PYTHONNOUSERSITE", L"1");
    }
    SetEnvironmentVariableW(L"PYTHONIOENCODING", L"utf-8"); SetEnvironmentVariableW(L"PYTHONUNBUFFERED", L"1");
    SECURITY_ATTRIBUTES sa = { sizeof sa, NULL, TRUE };
    HANDLE rd, wr, nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, NULL);
    if (!CreatePipe(&rd, &wr, &sa, 0)) return 0;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    /* a console of its own, hidden: the compilers setup.py starts are console programs and share it instead of each
       opening a window */
    STARTUPINFOW si = { sizeof si };
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW; si.wShowWindow = SW_HIDE;
    si.hStdInput = nul; si.hStdOutput = si.hStdError = wr;
    PROCESS_INFORMATION pi;
    static wchar_t cmd[4 * MAX_PATH];
    swprintf(cmd, 4 * MAX_PATH, L"\"%s\" -u \"%s\\scripts\\setup.py\"%s%s", g_python, g_root, *args ? L" " : L"", args);
    BOOL ok = CreateProcessW(g_python, cmd, NULL, NULL, TRUE, CREATE_NEW_CONSOLE | CREATE_SUSPENDED, NULL, g_root, &si, &pi);
    CloseHandle(wr);
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    if (!ok) { CloseHandle(rd); MessageBoxW(to, L"Could not start scripts\\setup.py.", L"B2recomp", MB_ICONERROR); return 0; }
    *job = CreateJobObjectW(NULL, NULL);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION li = { 0 };
    li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (*job) { SetInformationJobObject(*job, JobObjectExtendedLimitInformation, &li, sizeof li); AssignProcessToJobObject(*job, pi.hProcess); }
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    *proc = pi.hProcess;
    Reader *r = malloc(sizeof *r);
    r->rd = rd; r->proc = pi.hProcess; r->to = to;
    HANDLE th = CreateThread(NULL, 0, reader, r, 0, NULL);
    if (th) CloseHandle(th);
    return 1;
}
static void build_start(void) {
    dump_check();
    if (!g_bok || g_bproc) return;
    wchar_t args[MAX_PATH + 16];
    swprintf(args, MAX_PATH + 16, L"--dump \"%s\"", g_dump);
    if (!start_setup(args, g_main, &g_bproc, &g_bjob)) return;
    for (int i = 0; i < NSTEP; i++) { g_bstate[i] = 0; step_show(i); }
    g_bcompile = 0;
    progress_update();
    SetWindowTextW(g_blog, L"");
    build_enable(0);
    g_bt0 = GetTickCount();
}
static void set_window_icon(HWND w) {                       /* build\bayonetta2.ico, made from the player's dump by setup.py */
    wchar_t ico[MAX_PATH];
    swprintf(ico, MAX_PATH, L"%s\\bayonetta2.ico", g_buildd);
    HICON big = (HICON)LoadImageW(NULL, ico, IMAGE_ICON, GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), LR_LOADFROMFILE);
    HICON sm = (HICON)LoadImageW(NULL, ico, IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_LOADFROMFILE);
    if (big) SendMessageW(w, WM_SETICON, ICON_BIG, (LPARAM)big);
    if (sm) SendMessageW(w, WM_SETICON, ICON_SMALL, (LPARAM)sm);
}
static void build_done(DWORD code) {
    CloseHandle(g_bproc); g_bproc = NULL;
    if (g_bjob) { CloseHandle(g_bjob); g_bjob = NULL; }
    DWORD s = (GetTickCount() - g_bt0) / 1000;
    wchar_t t[256];
    if (code == 0 && game_built()) {                        /* the window becomes the launcher */
        HWND kids[] = { g_bpath, g_bbrowse, g_bcheck, g_blog, g_bprog, g_bbtn, g_blogs, g_bclose, g_bintro, g_blabel, g_status };
        for (int i = 0; i < (int)(sizeof kids / sizeof kids[0]); i++) DestroyWindow(kids[i]);
        for (int i = 0; i < NSTEP; i++) DestroyWindow(g_bstep[i]);
        DragAcceptFiles(g_main, FALSE);
        g_mode = 0;
        create_controls();
        load_all();
        set_window_icon(g_main);
        swprintf(t, 256, L"Built in %lu min %02lu s. Ready: press Play.", (unsigned long)(s / 60), (unsigned long)(s % 60));
        SetWindowTextW(g_status, t);
        InvalidateRect(g_main, NULL, TRUE);
        SetFocus(g_play);
        return;
    }
    for (int i = 0; i < NSTEP; i++) if (g_bstate[i] == 1) { g_bstate[i] = 3; step_show(i); }
    swprintf(t, 256, L"The build stopped after %lu min %02lu s: the log says why. Fix it and press Build again (finished steps are kept).",
             (unsigned long)(s / 60), (unsigned long)(s % 60));
    SetWindowTextW(g_status, t);
    build_enable(1);
}
static void browse(void) {
    IFileOpenDialog *d;
    if (FAILED(CoCreateInstance(&CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER, &IID_IFileOpenDialog, (void **)&d))) return;
    DWORD o = 0;
    IFileOpenDialog_GetOptions(d, &o);
    IFileOpenDialog_SetOptions(d, o | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    IFileOpenDialog_SetTitle(d, L"Your dump folder (the one that holds exefs and romfs)");
    if (SUCCEEDED(IFileOpenDialog_Show(d, g_main))) {
        IShellItem *it;
        if (SUCCEEDED(IFileOpenDialog_GetResult(d, &it))) {
            PWSTR p;
            if (SUCCEEDED(IShellItem_GetDisplayName(it, SIGDN_FILESYSPATH, &p))) { SetWindowTextW(g_bpath, p); CoTaskMemFree(p); }
            IShellItem_Release(it);
        }
    }
    IFileOpenDialog_Release(d);
}
static void create_build_controls(void) {
    const RECT *p = &k_bpanel;
    int lx = p->left + 16, w = p->right - p->left - 32;
    g_bintro = mk(L"STATIC", L"B2recomp builds the game on this PC from your own extracted copy of Bayonetta 2, version 1.2 "
                             L"(update v131072). Nothing of the game is downloaded; it takes about 5 minutes on a fast PC.",
                  SS_LEFT, lx, p->top + 46, w, 36, -1);
    g_blabel = mk(L"STATIC", L"Dump folder", SS_LEFT, lx, p->top + 96, 100, 20, -1);
    g_bpath = mk(L"EDIT", L"", ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP, lx + 100, p->top + 92, w - 100 - 108, 24, ID_BPATH);
    g_bbrowse = mk(L"BUTTON", L"Browse...", BS_PUSHBUTTON | WS_TABSTOP, p->right - 16 - 100, p->top + 91, 100, 26, ID_BBROWSE);
    tip(g_bpath, L"The folder you extracted from your own console and copy: it holds exefs\\ (main, sdk, subsdk0, rtld, main.npdm) "
                 L"and romfs\\, optionally sysdata\\fonts\\ and control\\. Encrypted packages are not accepted.");
    g_bcheck = mk(L"STATIC", L"", SS_LEFT, lx + 100, p->top + 122, w - 100, 20, ID_BCHECK);
    int y = p->top + 160;
    for (int i = 0; i < NSTEP; i++) g_bstep[i] = mk(L"STATIC", L"", SS_LEFT, lx, y + i * 22, 270, 20, ID_BSTEP0 + i);
    g_blog = mk(L"EDIT", L"", ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL | WS_BORDER, lx + 280, y - 4, w - 280, 220, ID_BLOG);
    SendMessageW(g_blog, WM_SETFONT, (WPARAM)g_smallfont, FALSE);
    SendMessageW(g_blog, EM_SETLIMITTEXT, 0, 0);
    g_bprog = CreateWindowExW(0, PROGRESS_CLASSW, L"", WS_CHILD | WS_VISIBLE, S(lx), S(p->bottom - 30), S(w), S(12), g_main,
                              (HMENU)(INT_PTR)ID_BPROG, GetModuleHandleW(NULL), NULL);
    SetWindowTheme(g_bprog, L"", L"");                      /* unthemed, so the colours below apply */
    SendMessageW(g_bprog, PBM_SETRANGE32, 0, 1000);
    SendMessageW(g_bprog, PBM_SETBARCOLOR, 0, (LPARAM)C_GOLD);
    SendMessageW(g_bprog, PBM_SETBKCOLOR, 0, (LPARAM)C_FIELD);
    int by = p->bottom + 12, px = WIN_W - 14 - 150;
    g_status = mk(L"STATIC", L"", SS_LEFT | SS_ENDELLIPSIS, 16, by + 11, px - 12 - 2 * 90 - 8 - 16 - 16, 20, ID_STATUS);
    g_blogs = mk(L"BUTTON", L"Logs folder", BS_PUSHBUTTON | WS_TABSTOP, px - 12 - 2 * 90 - 8, by + 3, 90, 34, ID_BLOGS);
    tip(g_blogs, L"Opens build\\logs: one log per step (setup.py).");
    g_bclose = mk(L"BUTTON", L"Close", BS_PUSHBUTTON | WS_TABSTOP, px - 12 - 90, by + 3, 90, 34, ID_CLOSE);
    g_bbtn = mk(L"BUTTON", L"Build", BS_OWNERDRAW | WS_TABSTOP, px, by, 150, 40, ID_BUILD);
    SetWindowSubclass(g_bbtn, play_hover, 1, 0);
    for (int i = 0; i < NSTEP; i++) step_show(i);
    prefill_dump();
    dump_check();
    DragAcceptFiles(g_main, TRUE);
    SetWindowTextW(g_status, !*g_python ? L"Python was not found: the builder package brings its own (deps\\python)."
                             : g_update ? L"B2recomp was updated: press Build to bring your game up to date (a minute or two; settings and saves stay)."
                                        : L"Choose your dump folder, then press Build.");
}

/* ================================================================ code mods window
 * [Recomps#1] "Mods..." in the bottom bar (RomFS-only mod folders are listed too: their switch is only saved, as the runtime's
 * nnfs.c reads [mods] at game start). The emulator-style code mods under mods\ (.pchtxt / .ips, any layout; the first
 * folder below mods\ is the mod's name) with a check box per mod, per file inside a mod folder and per .pchtxt block,
 * like the emulators' mod lists. The list comes from python scripts\pchtxt.py tree (the parser setup.py uses, so the
 * window shows exactly what a build would apply); the switches live in config\mods.ini (section [mods], "<key> = on|off",
 * keys as pchtxt.py documents them), rewritten in place like the other ini files. Code mods are compiled into the game, so
 * Apply (rebuild) saves the switches and runs scripts\setup.py (as the build screen: hidden console, job object, log box):
 * its mods step patches main.elf, and the lift and the compile redo only what changed (about a minute). Not while the game
 * runs: the link replaces build\game.dll. */
enum { ID_MTREE = 800, ID_MNOTE, ID_MLOG, ID_MFOLDER, ID_MREFRESH, ID_MAPPLY, ID_MCLOSE, ID_MSTATUS, ID_MTIMER = 2 };
enum { WM_APP_MSYNC = WM_APP + 3 };
enum { MK_MOD, MK_FILE, MK_BLOCK };
typedef struct { int kind, on, on0, def, parent, nrec, romfs; char key[600]; wchar_t label[300], note[600]; HTREEITEM h; } ModItem;
static ModItem *g_mi;
static int g_mn, g_mloading, g_mstep, g_mclosing;
static HWND g_mwin, g_mtree, g_mnote, g_mlog, g_mstatus, g_mapply;
static HANDLE g_mproc, g_mjob;
static DWORD g_mt0;
static char g_mwant[80], g_mbuilt[80];                      /* fingerprint of the switches as saved / of the build */
static wchar_t g_merr[600], g_modsini[MAX_PATH], g_modsdir[MAX_PATH];
static const wchar_t *k_mstep[NSTEP] = { L"checking the tools", L"checking the dump", L"converting", L"applying the mods",
    L"generating tables", L"translating", L"compiling", L"icon", L"linking", L"self-test" };

static void utf8w(const char *s, wchar_t *o, int n) { if (!MultiByteToWideChar(CP_UTF8, 0, s, -1, o, n)) o[0] = 0; o[n - 1] = 0; }
static int mod_on(int i) { for (; i >= 0; i = g_mi[i].parent) if (!g_mi[i].on) return 0; return 1; }   /* with its parents */
static int mods_dirty(void) { for (int i = 0; i < g_mn; i++) if (g_mi[i].on != g_mi[i].on0) return 1; return 0; }
static int mods_dirty_code(void) {                          /* a changed switch that needs a rebuild (romfs 2 = RomFS-only mod) */
    for (int i = 0; i < g_mn; i++) if (g_mi[i].on != g_mi[i].on0 && g_mi[i].romfs != 2) return 1;
    return 0;
}

/* python scripts\pchtxt.py tree mods\ targets\exefs\main -> the whole output (malloc'd, UTF-8), NULL on failure */
static char *run_capture(const wchar_t *cmd) {
    SECURITY_ATTRIBUTES sa = { sizeof sa, NULL, TRUE };
    HANDLE rd, wr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return NULL;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si = { sizeof si };
    si.dwFlags = STARTF_USESTDHANDLES; si.hStdOutput = si.hStdError = wr; si.hStdInput = NULL;
    PROCESS_INFORMATION pi;
    static wchar_t c[4 * MAX_PATH];
    wcsncpy(c, cmd, 4 * MAX_PATH - 1);
    BOOL ok = CreateProcessW(NULL, c, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, g_root, &si, &pi);
    CloseHandle(wr);
    if (!ok) { CloseHandle(rd); return NULL; }
    size_t n = 0, cap = 65536;
    char *b = malloc(cap);
    DWORD got;
    while (b && ReadFile(rd, b + n, (DWORD)(cap - n - 1), &got, NULL) && got) {
        n += got;
        if (cap - n < 4096) { char *nb = realloc(b, cap *= 2); if (!nb) { free(b); b = NULL; } else b = nb; }
    }
    CloseHandle(rd);
    WaitForSingleObject(pi.hProcess, 10000);
    DWORD code = 1; GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    if (b) b[n] = 0;
    if (b && code) { free(b); b = NULL; }
    return b;
}
static void read_built(void) {                              /* setup_state.json "mods": the fingerprint the game was built with */
    wchar_t p[MAX_PATH];
    snprintf(g_mbuilt, sizeof g_mbuilt, "none");
    swprintf(p, MAX_PATH, L"%s\\setup_state.json", g_buildd);
    FILE *f = _wfopen(p, L"rb");
    if (!f) return;
    static char b[65536];
    size_t n = fread(b, 1, sizeof b - 1, f);
    fclose(f);
    b[n] = 0;
    char *q = strstr(b, "\"mods\": \"");
    if (!q) return;
    q += 9;
    size_t k = 0;
    while (q[k] && q[k] != '"' && k + 1 < sizeof g_mbuilt) { g_mbuilt[k] = q[k]; k++; }
    g_mbuilt[k] = 0;
}
static void mods_status(void) {
    wchar_t t[400];
    if (g_mproc) {
        DWORD s = (GetTickCount() - g_mt0) / 1000;
        swprintf(t, 400, L"Rebuilding... %lu:%02lu   (%s)", (unsigned long)(s / 60), (unsigned long)(s % 60), g_mstep >= 0 ? k_mstep[g_mstep] : L"starting");
    } else if (!*g_python) wcscpy(t, L"Python was not found: the mod list and the rebuild need it.");
    else if (mods_dirty() && !mods_dirty_code()) wcscpy(t, L"Changed. Apply saves it: RomFS files need no rebuild (read when the game starts).");
    else if (mods_dirty()) wcscpy(t, L"Changed. Apply (rebuild) saves the switches and builds the game with them.");
    else if (!strcmp(g_mwant, "error")) wcscpy(t, L"A mod that is on cannot be used (marked with !): switch it off or remove it.");
    else if (strcmp(g_mwant, g_mbuilt)) wcscpy(t, L"The game was built with other mods than these: press Apply (rebuild).");
    else wcscpy(t, !strcmp(g_mwant, "none") ? L"The game is built without code mods." : L"The game is built with these mods.");
    SetWindowTextW(g_mstatus, t);
    EnableWindow(g_mapply, !g_mproc && *g_python);
}
static void mods_note(void) {                               /* the selected item, or the overall state */
    HTREEITEM h = TreeView_GetSelection(g_mtree);
    wchar_t t[1400];
    int i = -1;
    for (int k = 0; k < g_mn; k++) if (g_mi[k].h == h) i = k;
    if (i < 0) {
        if (!g_mn) swprintf(t, 1400, L"No mods in %s. Copy an emulator mod's folder there as it is (exefs\\ code patches, romfs\\ files), then Refresh.", g_modsdir);
        else if (*g_merr) swprintf(t, 1400, L"%s", g_merr);
        else swprintf(t, 1400, L"Select a mod to see its details. Grey = switched off by itself or with the mod or file above it.");
    } else {
        wchar_t key[600]; utf8w(g_mi[i].key, key, 600);
        const wchar_t *what = g_mi[i].kind == MK_BLOCK ? (g_mi[i].def ? L"block, written under @enabled" : L"block, written under @disabled (off unless switched on here)")
                            : g_mi[i].kind == MK_FILE ? L"file" : L"mod";
        wchar_t lines[48] = L"";
        if (g_mi[i].kind == MK_BLOCK) swprintf(lines, 48, L", %d patch line%s", g_mi[i].nrec, g_mi[i].nrec == 1 ? L"" : L"s");
        const wchar_t *rf = g_mi[i].romfs == 2 ? L"\nRomFS files only (textures, models, sounds...): the game reads them when it starts, no rebuild needed."
                          : g_mi[i].romfs ? L"\nAlso has RomFS files: switching the mod off leaves them out as well (no rebuild needed for those)." : L"";
        swprintf(t, 1400, L"%s   (%s%s)%s%s%s", key, what, lines, g_mi[i].note[0] ? L"\n" : L"", g_mi[i].note, rf);
    }
    SetWindowTextW(g_mnote, t);
}
static void mods_load(void) {                               /* (re)reads the list; unsaved switches are kept by key */
    int keep_n = g_mn;
    ModItem *keep = g_mi;
    g_mi = NULL; g_mn = 0; g_merr[0] = 0;
    snprintf(g_mwant, sizeof g_mwant, "none");
    read_built();
    g_mloading = 1;
    TreeView_DeleteAllItems(g_mtree);
    char *out = NULL;
    if (*g_python) {
        wchar_t cmd[4 * MAX_PATH], nso[MAX_PATH], ini[MAX_PATH + 8];
        swprintf(nso, MAX_PATH, L"%s\\targets\\exefs\\main", g_root);
        swprintf(ini, MAX_PATH + 8, L"--ini=%s", g_modsini);
        swprintf(cmd, 4 * MAX_PATH, L"\"%s\" -I \"%s\\scripts\\pchtxt.py\" tree \"%s\" %s%s%s \"%s\"", g_python, g_root, g_modsdir,
                 file_exists(nso) ? L"\"" : L"", file_exists(nso) ? nso : L"", file_exists(nso) ? L"\"" : L"", ini);
        out = run_capture(cmd);
        if (!out) swprintf(g_merr, 600, L"scripts\\pchtxt.py tree failed: run it in a console to see why.");
    }
    int cap = 0, cur_mod = -1, cur_file = -1;
    for (char *line = out, *next; line && *line; line = next) {
        next = strchr(line, '\n');
        if (next) *next++ = 0; else next = line + strlen(line);
        size_t len = strlen(line);
        if (len && line[len - 1] == '\r') line[--len] = 0;
        char *f[8] = { 0 };
        int nf = 0;
        for (char *p = line; nf < 8; ) { f[nf++] = p; p = strchr(p, '\t'); if (!p) break; *p++ = 0; }
        if (!strcmp(f[0], "state") && nf >= 2) {
            snprintf(g_mwant, sizeof g_mwant, "%s", f[1]);
            if (nf >= 3 && f[2][0]) utf8w(f[2], g_merr, 600);
            continue;
        }
        int kind = !strcmp(f[0], "mod") ? MK_MOD : !strcmp(f[0], "file") ? MK_FILE : !strcmp(f[0], "block") ? MK_BLOCK : -1;
        if (kind < 0 || nf < (kind == MK_BLOCK ? 6 : 4)) continue;
        if (g_mn == cap) { cap = cap ? cap * 2 : 32; g_mi = realloc(g_mi, (size_t)cap * sizeof *g_mi); }
        ModItem *it = &g_mi[g_mn];
        memset(it, 0, sizeof *it);
        it->kind = kind;
        snprintf(it->key, sizeof it->key, "%s", f[1]);
        it->on = it->on0 = atoi(f[2]);
        it->def = 1;
        if (kind == MK_BLOCK) { it->def = atoi(f[3]); it->nrec = atoi(f[4]); utf8w(f[5], it->label, 300); }
        else { utf8w(f[3], it->label, 300); if (nf >= 5) utf8w(f[4], it->note, 600); }
        if (kind == MK_MOD && nf >= 6) it->romfs = !strcmp(f[5], "romfs-only") ? 2 : !strcmp(f[5], "romfs") ? 1 : 0;
        if (kind == MK_MOD) { it->parent = -1; cur_mod = g_mn; cur_file = -1; }
        else if (kind == MK_FILE) { it->parent = cur_mod; cur_file = g_mn; }
        else it->parent = cur_file >= 0 ? cur_file : cur_mod;
        for (int k = 0; k < keep_n; k++) if (keep[k].on != keep[k].on0 && !strcmp(keep[k].key, it->key)) it->on = keep[k].on;
        wchar_t text[400];
        swprintf(text, 400, L"%s%s%s", it->note[0] ? L"! " : L"", it->label,
                 kind == MK_BLOCK && !it->def ? L"   (@disabled in the file)"
                 : it->romfs == 2 ? L"   (RomFS files: no rebuild)" : it->romfs ? L"   (+ RomFS files)" : L"");
        TVINSERTSTRUCTW ins = { 0 };
        ins.hParent = it->parent >= 0 ? g_mi[it->parent].h : TVI_ROOT;
        ins.hInsertAfter = TVI_LAST;
        ins.item.mask = TVIF_TEXT | TVIF_PARAM | TVIF_STATE;
        ins.item.pszText = text;
        ins.item.lParam = g_mn;
        ins.item.stateMask = TVIS_STATEIMAGEMASK | TVIS_EXPANDED;
        ins.item.state = INDEXTOSTATEIMAGEMASK(it->on ? 2 : 1) | TVIS_EXPANDED;
        it->h = (HTREEITEM)SendMessageW(g_mtree, TVM_INSERTITEMW, 0, (LPARAM)&ins);
        g_mn++;
    }
    free(out);
    free(keep);
    for (int i = 0; i < g_mn; i++) if (g_mi[i].parent >= 0) TreeView_Expand(g_mtree, g_mi[g_mi[i].parent].h, TVE_EXPAND);
    g_mloading = 0;
    InvalidateRect(g_mtree, NULL, TRUE);
    mods_note();
    mods_status();
}
static void mods_sync(void) {                               /* the check boxes after a click or Space */
    for (int i = 0; i < g_mn; i++) g_mi[i].on = TreeView_GetCheckState(g_mtree, g_mi[i].h) == 1;
    InvalidateRect(g_mtree, NULL, TRUE);
    mods_status();
}

/* mods.ini, in place: the line of key in [mods] gets the value after its last '=' replaced; a new key goes at the end of
   [mods] (the section is added when missing). Keys compare as pchtxt.py norm_key: '\' = '/', blanks around '::', no case. */
static void mods_norm(const char *s, char *o, size_t n) {
    size_t k = 0;
    while (*s == ' ' || *s == '\t') s++;
    for (; *s && k + 5 < n; s++) {
        if (*s == ':' && s[1] == ':') {
            while (k && (o[k - 1] == ' ' || o[k - 1] == '\t')) k--;
            memcpy(o + k, " :: ", 4); k += 4; s++;
            while (s[1] == ' ' || s[1] == '\t') s++;
            continue;
        }
        char c = *s == '\\' ? '/' : *s;
        o[k++] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
    }
    while (k && (o[k - 1] == ' ' || o[k - 1] == '\t' || o[k - 1] == '\r')) k--;
    o[k] = 0;
}
static void mods_set(Ini *f, const char *key, const char *val) {
    char want[700], have[700], cur[128] = "";
    int last = -1, header = -1, done = 0;
    mods_norm(key, want, sizeof want);
    const char *eol = f->crlf ? "\r" : "";
    for (int i = 0; i < f->n; i++) {
        const char *l = f->line[i];
        if (line_section(l, cur, sizeof cur)) { if (!_stricmp(cur, "mods")) header = last = i; continue; }
        if (_stricmp(cur, "mods")) continue;
        if (!is_blank(l)) last = i;
        const char *p = l;
        while (*p == ' ' || *p == '\t') p++;
        const char *eq = strrchr(l, '=');
        if (!*p || *p == ';' || *p == '#' || !eq || (size_t)(eq - l) >= sizeof have) continue;
        char k[700]; memcpy(k, l, (size_t)(eq - l)); k[eq - l] = 0;
        mods_norm(k, have, sizeof have);
        if (strcmp(have, want)) continue;
        size_t kl = (size_t)(eq - l) + 1;
        char *nl = malloc(kl + strlen(val) + 4);
        memcpy(nl, l, kl); sprintf(nl + kl, " %s%s", val, eol);
        free(f->line[i]); f->line[i] = nl;
        done = 1;
    }
    if (done) return;
    char nl[800];
    if (header < 0) {
        ensure_eol(f);
        int at = f->n - 1;
        if (at > 0 && !is_blank(f->line[at - 1])) ini_insert(f, at++, eol, strlen(eol));
        snprintf(nl, sizeof nl, "[mods]%s", eol); ini_insert(f, at++, nl, strlen(nl));
        last = at - 1;
    }
    int at = last + 1;
    if (at >= f->n) { ensure_eol(f); at = f->n - 1; }
    snprintf(nl, sizeof nl, "%s = %s%s", key, val, eol);
    ini_insert(f, at, nl, strlen(nl));
}
static int mods_save(void) {
    if (!mods_dirty()) return 1;
    Ini f;
    if (!ini_load(&f, g_modsini)) {                         /* a new file starts with what it is */
        static const char *k_head[] = {
            "; Code mod switches: which of the mods in mods\\ (emulator .pchtxt / .ips files) are built into the game.",
            "; Written by the launcher's Mods window, read by scripts\\setup.py (step mods) through scripts\\pchtxt.py.",
            "; <key> = on | off. A key is a mod (the first folder below mods\\, or a file lying directly in it), a file",
            "; (its path below mods\\, with /), or a block of a .pchtxt (<file> :: <block title>, or <file> :: #<n>).",
            "; Not listed = the default: on (a block as its @enabled / @disabled line says). A change needs a rebuild:",
            "; the Mods window's Apply, or python scripts\\setup.py. A mod switched off also leaves out its romfs\\ files",
            "; (read by the game at start, no rebuild). [prompts]: the button-prompt mods (BUILDING.md).", "[mods]" };
        ini_free(&f); memset(&f, 0, sizeof f); f.crlf = 1;
        for (int i = 0; i < (int)(sizeof k_head / sizeof k_head[0]); i++) { char l[200]; snprintf(l, sizeof l, "%s\r", k_head[i]); ini_insert(&f, f.n, l, strlen(l)); }
        ini_insert(&f, f.n, "", 0);
    }
    for (int i = 0; i < g_mn; i++) if (g_mi[i].on != g_mi[i].on0) mods_set(&f, g_mi[i].key, g_mi[i].on ? "on" : "off");
    int ok = ini_save(&f, g_modsini);
    ini_free(&f);
    if (!ok) { wchar_t m[MAX_PATH + 64]; swprintf(m, MAX_PATH + 64, L"Could not write %s.", g_modsini); MessageBoxW(g_mwin, m, L"B2recomp", MB_ICONERROR); return 0; }
    for (int i = 0; i < g_mn; i++) g_mi[i].on0 = g_mi[i].on;
    return 1;
}
static int game_running(void) {                             /* build\game_native.exe, started by Play or anything else */
    if (g_proc && WaitForSingleObject(g_proc, 0) != WAIT_OBJECT_0) return 1;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W pe = { sizeof pe };
    int found = 0;
    for (BOOL ok = Process32FirstW(snap, &pe); ok && !found; ok = Process32NextW(snap, &pe)) {
        if (_wcsicmp(pe.szExeFile, L"game_native.exe")) continue;
        HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID);
        if (!h) continue;
        wchar_t p[MAX_PATH]; DWORD n = MAX_PATH;
        if (QueryFullProcessImageNameW(h, 0, p, &n) && !_wcsicmp(p, g_game)) found = 1;
        CloseHandle(h);
    }
    CloseHandle(snap);
    return found;
}
static void mods_apply(void) {
    if (g_mproc) return;
    int rebuild = mods_dirty_code() || strcmp(g_mwant, g_mbuilt);   /* else only RomFS-only mods changed */
    if (rebuild && game_running()) { MessageBoxW(g_mwin, L"The game is running: close it first (the rebuild replaces build\\game.dll).", L"B2recomp", MB_ICONINFORMATION); return; }
    if (!mods_save()) return;
    if (!rebuild) {
        mods_load();
        SetWindowTextW(g_mstatus, L"Saved. RomFS mods need no rebuild: the game reads them the next time it starts.");
        return;
    }
    if (!start_setup(L"", g_mwin, &g_mproc, &g_mjob)) return;
    SetWindowTextW(g_mlog, L"");
    g_mstep = -1;
    g_mt0 = GetTickCount();
    EnableWindow(g_mtree, FALSE);
    SetTimer(g_mwin, ID_MTIMER, 500, NULL);
    mods_status();
}
static void mods_done(DWORD code) {
    CloseHandle(g_mproc); g_mproc = NULL;
    if (g_mjob) { CloseHandle(g_mjob); g_mjob = NULL; }
    KillTimer(g_mwin, ID_MTIMER);
    EnableWindow(g_mtree, TRUE);
    if (g_mclosing) { DestroyWindow(g_mwin); return; }      /* stopped by Close */
    DWORD s = (GetTickCount() - g_mt0) / 1000;
    mods_load();
    wchar_t t[300];
    if (code == 0) swprintf(t, 300, L"Rebuilt in %lu:%02lu. The game has these mods from the next Play.", (unsigned long)(s / 60), (unsigned long)(s % 60));
    else swprintf(t, 300, L"The rebuild stopped (the log says why). Fix it and press Apply again, before you play.");
    SetWindowTextW(g_mstatus, t);
}
#define MW_W 760
#define MW_H 620
static HWND mmk(const wchar_t *cls, const wchar_t *text, DWORD style, int x, int y, int w, int h, int id) {
    HWND c = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, S(x), S(y), S(w), S(h), g_mwin, (HMENU)(INT_PTR)id, GetModuleHandleW(NULL), NULL);
    SendMessageW(c, WM_SETFONT, (WPARAM)g_font, FALSE);
    if (!wcscmp(cls, L"BUTTON") || !wcscmp(cls, WC_TREEVIEWW)) { if (p_AllowDarkModeForWindow) p_AllowDarkModeForWindow(c, TRUE); SetWindowTheme(c, L"DarkMode_Explorer", NULL); }
    return c;
}
static LRESULT CALLBACK mwndproc(HWND w, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
    case WM_CREATE: {
        g_mwin = w;
        mmk(L"STATIC", L"The mods in the mods folder, switched on and off as in an emulator. Code patches (.pchtxt, .ips) are compiled "
                       L"into the game: a change to them takes effect after Apply (rebuild), about a minute, with the game closed. "
                       L"RomFS files (textures, models, sounds) are read when the game starts.", SS_LEFT, 20, 14, MW_W - 40, 54, -1);
        g_mtree = mmk(WC_TREEVIEWW, L"", WS_BORDER | WS_TABSTOP | TVS_HASBUTTONS | TVS_HASLINES | TVS_LINESATROOT | TVS_SHOWSELALWAYS,
                      20, 74, MW_W - 40, 280, ID_MTREE);
        SetWindowLongPtrW(g_mtree, GWL_STYLE, GetWindowLongPtrW(g_mtree, GWL_STYLE) | TVS_CHECKBOXES);   /* after creation, as documented */
        TreeView_SetBkColor(g_mtree, C_FIELD); TreeView_SetTextColor(g_mtree, C_TEXT); TreeView_SetLineColor(g_mtree, C_EDGE);
        g_mnote = mmk(L"STATIC", L"", SS_LEFT, 20, 360, MW_W - 40, 52, ID_MNOTE);
        SendMessageW(g_mnote, WM_SETFONT, (WPARAM)g_smallfont, FALSE);
        g_mlog = mmk(L"EDIT", L"", ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL | WS_BORDER, 20, 416, MW_W - 40, 136, ID_MLOG);
        SendMessageW(g_mlog, WM_SETFONT, (WPARAM)g_smallfont, FALSE);
        SendMessageW(g_mlog, EM_SETLIMITTEXT, 0, 0);
        if (p_AllowDarkModeForWindow) p_AllowDarkModeForWindow(g_mlog, TRUE);
        SetWindowTheme(g_mlog, L"DarkMode_Explorer", NULL);
        int by = 566;
        g_mstatus = mmk(L"STATIC", L"", SS_LEFT, 20, by, 300, 40, ID_MSTATUS);
        HWND b = mmk(L"BUTTON", L"Mods folder", BS_PUSHBUTTON | WS_TABSTOP, 330, by, 100, 34, ID_MFOLDER);
        tip(b, L"Opens mods\\ (made when missing): put .pchtxt / .ips files or an emulator mod's folder there.");
        b = mmk(L"BUTTON", L"Refresh", BS_PUSHBUTTON | WS_TABSTOP, 438, by, 84, 34, ID_MREFRESH);
        tip(b, L"Reads the mods folder again (switches you changed and did not apply yet are kept).");
        g_mapply = mmk(L"BUTTON", L"Apply (rebuild)", BS_PUSHBUTTON | WS_TABSTOP, 530, by, 120, 34, ID_MAPPLY);
        tip(g_mapply, L"Saves the switches to config\\mods.ini and runs scripts\\setup.py: the mods step patches main.elf, then only "
                      L"what changed is translated and compiled again, and the game is linked (about a minute).");
        mmk(L"BUTTON", L"Close", BS_PUSHBUTTON | WS_TABSTOP, 658, by, 82, 34, ID_MCLOSE);
        mods_load();
        return 0;
    }
    case WM_ERASEBKGND: { RECT r; GetClientRect(w, &r); FillRect((HDC)wp, &r, g_brpanel); return 1; }
    case WM_CTLCOLOREDIT: case WM_CTLCOLORLISTBOX:
        SetTextColor((HDC)wp, C_TEXT); SetBkColor((HDC)wp, C_FIELD);
        return (LRESULT)g_brfield;
    case WM_CTLCOLORSTATIC: {
        HDC dc = (HDC)wp; HWND c = (HWND)lp;
        if (c == g_mlog) { SetTextColor(dc, C_TEXT); SetBkColor(dc, C_FIELD); return (LRESULT)g_brfield; }   /* read-only edit */
        SetTextColor(dc, c == g_mnote || c == g_mstatus ? C_NOTE : C_TEXT);
        SetBkColor(dc, C_PANEL);
        return (LRESULT)g_brpanel;
    }
    case WM_NOTIFY: {
        NMHDR *h = (NMHDR *)lp;
        if (h->hwndFrom != g_mtree) break;
        if (h->code == NM_CUSTOMDRAW) {                     /* grey: off, by itself or through the mod / file above it */
            NMTVCUSTOMDRAW *cd = (NMTVCUSTOMDRAW *)lp;
            if (cd->nmcd.dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
            if (cd->nmcd.dwDrawStage == CDDS_ITEMPREPAINT) {
                int i = (int)cd->nmcd.lItemlParam;
                if (i >= 0 && i < g_mn && !(cd->nmcd.uItemState & CDIS_SELECTED)) {
                    cd->clrText = !mod_on(i) ? C_DIM : g_mi[i].note[0] ? RGB(240, 112, 100) : C_TEXT;
                    cd->clrTextBk = C_FIELD;
                }
                return CDRF_NEWFONT;
            }
            return CDRF_DODEFAULT;
        }
        if (h->code == NM_CLICK || (h->code == TVN_KEYDOWN && ((NMTVKEYDOWN *)lp)->wVKey == VK_SPACE))
            PostMessageW(w, WM_APP_MSYNC, 0, 0);            /* the box changes after this notification */
        if (h->code == TVN_SELCHANGEDW) mods_note();
        return 0;
    }
    case WM_APP_MSYNC: if (!g_mloading && !g_mproc) mods_sync(); return 0;
    case WM_APP_LINE: {                                     /* a line of setup.py's output */
        const wchar_t *s = (const wchar_t *)lp;
        log_append(g_mlog, s);
        wchar_t *e;
        long i = s[0] == L'[' ? wcstol(s + 1, &e, 10) : 0;
        if (i >= 1 && i <= NSTEP && *e == L'/' && wcsstr(e, L" ...")) { g_mstep = (int)i - 1; mods_status(); }
        free((void *)lp);
        return 0;
    }
    case WM_APP_DONE: mods_done((DWORD)wp); return 0;
    case WM_TIMER: if (wp == ID_MTIMER && g_mproc) mods_status(); return 0;
    case WM_COMMAND: {
        int id = LOWORD(wp);
        if (id == ID_MFOLDER) { CreateDirectoryW(g_modsdir, NULL); ShellExecuteW(w, L"open", g_modsdir, NULL, NULL, SW_SHOWNORMAL); }
        else if (id == ID_MREFRESH) { if (!g_mproc) mods_load(); }
        else if (id == ID_MAPPLY) mods_apply();
        else if (id == ID_MCLOSE || id == IDCANCEL) SendMessageW(w, WM_CLOSE, 0, 0);
        return 0;
    }
    case WM_CLOSE:
        if (g_mproc) {
            if (MessageBoxW(w, L"Stop the rebuild? Press Apply again later: the game must be rebuilt before you play.", L"B2recomp", MB_YESNO | MB_ICONQUESTION) != IDYES) return 0;
            g_mclosing = 1;
            TerminateJobObject(g_mjob, 1);
            return 0;                                       /* WM_APP_DONE follows and closes the window */
        }
        if (mods_dirty()) {
            int r = MessageBoxW(w, L"Save the changed switches (without rebuilding)?", L"B2recomp", MB_YESNOCANCEL | MB_ICONQUESTION);
            if (r == IDCANCEL || (r == IDYES && !mods_save())) return 0;
        }
        DestroyWindow(w);
        return 0;
    case WM_DESTROY:
        g_mwin = NULL; free(g_mi); g_mi = NULL; g_mn = 0; g_mclosing = 0;
        EnableWindow(g_main, TRUE); SetForegroundWindow(g_main);
        return 0;
    }
    return DefWindowProcW(w, m, wp, lp);
}
static void mods_window(void) {
    if (g_mwin) { SetForegroundWindow(g_mwin); return; }
    static int reg;
    if (!reg) {
        WNDCLASSEXW wc = { sizeof wc };
        wc.lpfnWndProc = mwndproc; wc.hInstance = GetModuleHandleW(NULL); wc.lpszClassName = L"B2recompMods";
        wc.hCursor = LoadCursorW(NULL, IDC_ARROW); wc.hbrBackground = g_brpanel;
        reg = RegisterClassExW(&wc) != 0;
    }
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN;
    RECT r = { 0, 0, S(MW_W), S(MW_H) };
    AdjustWindowRectEx(&r, style, FALSE, WS_EX_CONTROLPARENT | WS_EX_DLGMODALFRAME);
    RECT mr; GetWindowRect(g_main, &mr);
    int ww = r.right - r.left, wh = r.bottom - r.top;
    HWND w = CreateWindowExW(WS_EX_CONTROLPARENT | WS_EX_DLGMODALFRAME, L"B2recompMods", L"Mods", style,
                             mr.left + (mr.right - mr.left - ww) / 2, mr.top + (mr.bottom - mr.top - wh) / 2, ww, wh, g_main, NULL, GetModuleHandleW(NULL), NULL);
    if (!w) return;
    {   BOOL on = TRUE; COLORREF text = RGB(200, 214, 240);  /* the main window's dark title bar */
        DwmSetWindowAttribute(w, 20, &on, sizeof on);
        DwmSetWindowAttribute(w, 35, &g_caption, sizeof g_caption);
        DwmSetWindowAttribute(w, 36, &text, sizeof text); }
    EnableWindow(g_main, FALSE);
    ShowWindow(w, SW_SHOW);
}

static LRESULT CALLBACK wndproc(HWND w, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
    case WM_CREATE:
        g_main = w;
        if (g_mode) create_build_controls();
        else {
            create_controls();
            load_all();
            if (!g_exists[F_GFX] || !g_exists[F_PAD]) SetWindowTextW(g_status, L"A config file is missing: the game writes it, with comments, at its first start.");
        }
        SetTimer(w, ID_TIMER, 500, NULL);
        return 0;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: paint(w); return 0;
    case WM_DRAWITEM: if (wp == ID_PLAY || wp == ID_BUILD) { draw_play((const DRAWITEMSTRUCT *)lp); return TRUE; } break;
    case WM_CTLCOLOREDIT: case WM_CTLCOLORLISTBOX:
        SetTextColor((HDC)wp, C_TEXT); SetBkColor((HDC)wp, C_FIELD);
        return (LRESULT)g_brfield;
    case WM_CTLCOLORSTATIC: {                                /* labels, notes, sliders, check boxes, disabled edits */
        HDC dc = (HDC)wp; HWND c = (HWND)lp;
        COLORREF fg = C_TEXT, bk = C_PANEL; HBRUSH br = g_brpanel;
        if (c == g_gfxnote || c == g_padnote) fg = C_NOTE;
        if (c == g_status) { fg = C_NOTE; bk = C_BG; br = g_brbg; }
        if (g_mode) {                                       /* build screen */
            if (c == g_blog || c == g_bpath) { fg = C_TEXT; bk = C_FIELD; br = g_brfield; }
            if (c == g_bcheck) fg = g_bok ? C_VALUE : C_NOTE;
            for (int i = 0; i < NSTEP; i++)
                if (c == g_bstep[i]) fg = g_bstate[i] == 1 ? C_GOLD : g_bstate[i] == 2 ? C_TEXT : g_bstate[i] == 3 ? RGB(240, 112, 100) : C_DIM;
            SetTextColor(dc, fg); SetBkColor(dc, bk);
            return (LRESULT)br;
        }
        for (int i = 0; i < S_COUNT; i++) {
            Setting *s = &g_s[i];
            if (c == s->hw && s->kind == K_NUM) { fg = C_DIM; bk = C_FIELD; br = g_brfield; }   /* a disabled edit */
            if (c == s->hval && s->kind == K_SLIDER) fg = C_VALUE;
            if ((c == s->hlabel || c == s->hval) && !IsWindowEnabled(s->hw)) fg = C_DIM;
        }
        SetTextColor(dc, fg); SetBkColor(dc, bk);
        return (LRESULT)br;
    }
    case WM_HSCROLL:
        for (int i = 0; i < S_COUNT; i++) if (g_s[i].kind == K_SLIDER && (HWND)lp == g_s[i].hw) { g_s[i].moved = 1; slider_label(&g_s[i]); }
        update_ui();
        return 0;
    case WM_COMMAND: {
        int id = LOWORD(wp), code = HIWORD(wp);
        if (g_mode) {
            if (id == ID_BPATH && code == EN_CHANGE) dump_check();
            else if (id == ID_BBROWSE) browse();
            else if (id == ID_BUILD) build_start();
            else if (id == ID_BLOGS) { wchar_t d[MAX_PATH]; swprintf(d, MAX_PATH, L"%s\\logs", g_buildd); CreateDirectoryW(g_buildd, NULL); CreateDirectoryW(d, NULL); ShellExecuteW(w, L"open", d, NULL, NULL, SW_SHOWNORMAL); }
            else if (id == ID_CLOSE || id == IDCANCEL) SendMessageW(w, WM_CLOSE, 0, 0);
            return 0;
        }
        if (id == ID_SIZES && code == CBN_SELCHANGE) {
            int i = (int)SendMessageW(g_sizes, CB_GETCURSEL, 0, 0);
            if (i > 0) {
                wchar_t t[16];
                swprintf(t, 16, L"%d", k_sizes[i - 1][0]); SetWindowTextW(g_s[S_WIDTH].hw, t);
                swprintf(t, 16, L"%d", k_sizes[i - 1][1]); SetWindowTextW(g_s[S_HEIGHT].hw, t);
            }
            update_ui();
        } else if (id >= ID_SET && id < ID_SET + S_COUNT) {
            if (!g_loading && (code == CBN_SELCHANGE || code == EN_CHANGE || code == BN_CLICKED)) update_ui();
        } else if (id >= ID_LBL && id < ID_LBL + S_COUNT && code == STN_CLICKED) SendMessageW(g_s[id - ID_LBL].hw, BM_CLICK, 0, 0);
        else if (id == ID_REMAP) run_tool(L"--pad-map 1", L"Remap controller (player 1)");
        else if (id == ID_TEST) run_tool(L"--pad-test", L"Test controller");
        else if (id == ID_KEYS) keys_window();
        else if (id == ID_MODS) mods_window();
        else if (id == ID_SAVE) save_all();
        else if (id == ID_PLAY) play();
        else if (id == ID_CONFIG) { wchar_t d[MAX_PATH]; swprintf(d, MAX_PATH, L"%s\\config", g_root); ShellExecuteW(w, L"open", d, NULL, NULL, SW_SHOWNORMAL); }
        else if (id == ID_CLOSE || id == IDCANCEL) SendMessageW(w, WM_CLOSE, 0, 0);
        return 0;
    }
    case WM_TIMER:
        if (g_mode && g_bproc) {
            DWORD s = (GetTickCount() - g_bt0) / 1000;
            wchar_t t[128];
            compile_progress();
            progress_update();
            if (g_bstate[5] == 1) swprintf(t, 128, L"Building... %lu:%02lu   (compiling: %d %%)", (unsigned long)(s / 60), (unsigned long)(s % 60), g_bcompile / 10);
            else swprintf(t, 128, L"Building... %lu:%02lu", (unsigned long)(s / 60), (unsigned long)(s % 60));
            SetWindowTextW(g_status, t);
        }
        if (g_proc && WaitForSingleObject(g_proc, 0) == WAIT_OBJECT_0) {
            DWORD code = 0; GetExitCodeProcess(g_proc, &code);
            CloseHandle(g_proc); g_proc = NULL;
            EnableWindow(g_play, TRUE); SetWindowTextW(g_play, L"Play");
            wchar_t t[128]; swprintf(t, 128, L"The game has exited (code %ld; log: build\\play\\stdout.txt).", (long)code);
            SetWindowTextW(g_status, t);
        }
        return 0;
    case WM_APP_LINE: build_line((const wchar_t *)lp); free((void *)lp); return 0;
    case WM_APP_DONE: build_done((DWORD)wp); return 0;
    case WM_DROPFILES: {                                    /* a folder dragged onto the build screen */
        wchar_t p[MAX_PATH];
        if (g_mode && !g_bproc && DragQueryFileW((HDROP)wp, 0, p, MAX_PATH)) SetWindowTextW(g_bpath, p);
        DragFinish((HDROP)wp);
        return 0;
    }
    case WM_CLOSE: {
        if (g_mode) {
            if (g_bproc && MessageBoxW(w, L"Stop the build? The steps already finished are kept.", L"B2recomp", MB_YESNO | MB_ICONQUESTION) != IDYES) return 0;
            if (g_bjob) TerminateJobObject(g_bjob, 1);
            DestroyWindow(w);
            return 0;
        }
        int dirty = 0;
        for (int i = 0; i < S_COUNT; i++) dirty |= changed(&g_s[i]);
        if (dirty) {
            int r = MessageBoxW(w, L"Save the changed settings?", L"B2recomp", MB_YESNOCANCEL | MB_ICONQUESTION);
            if (r == IDCANCEL || (r == IDYES && !save_all())) return 0;
        }
        DestroyWindow(w);
        return 0;
    }
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(w, m, wp, lp);
}

/* [Recomps#1] finding 123: the window is S(WIN_W) x S(WIN_H) plus its frame, and at a high display scaling that can be
 * larger than the screen (a player's 4K screen: the bottom row with Play was below it; a 1080p screen at 150 % too). Then
 * everything is laid out at a smaller scale (g_dpi), so the whole window fits the work area; g_dpi_sys stays the system's,
 * for the system's message font. TEST: B2RECOMP_LAUNCHER_DPI=<dpi> lays out as at that display scaling (144 = 150 %). */
static int g_dpi_sys = 96;
static void fit_dpi(void) {
    wchar_t e[16];
    int want = g_dpi_sys;
    if (GetEnvironmentVariableW(L"B2RECOMP_LAUNCHER_DPI", e, 16) && _wtoi(e) >= 48 && _wtoi(e) <= 960) want = _wtoi(e);
    g_dpi = want;
    static const int k_win[3][2] = { {WIN_W, WIN_H}, {KW_W, KW_H}, {MW_W, MW_H} };     /* the main window, Keyboard and mouse, Mods */
    RECT wa, r = { 0, 0, S(WIN_W), S(WIN_H) };
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    AdjustWindowRectEx(&r, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN, FALSE, WS_EX_CONTROLPARENT);
    int ncw = (r.right - r.left) - S(WIN_W), nch = (r.bottom - r.top) - S(WIN_H);   /* frame and title bar: drawn by the system at its scale */
    int aw = wa.right - wa.left - 16 - ncw, ah = wa.bottom - wa.top - 16 - nch;      /* room for a client area, 8 px margin each side */
    for (int i = 0; i < 3; i++) {
        int dw = MulDiv(aw, 96, k_win[i][0]), dh = MulDiv(ah, 96, k_win[i][1]), d = dw < dh ? dw : dh;
        if (d < g_dpi) g_dpi = d < 48 ? 48 : d;
    }
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR cmdline, int show) {
    (void)prev;
    SetProcessDPIAware();
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);   /* the folder dialog of the build screen */
    INITCOMMONCONTROLSEX icc = { sizeof icc, ICC_STANDARD_CLASSES | ICC_BAR_CLASSES | ICC_TAB_CLASSES | ICC_TREEVIEW_CLASSES };
    InitCommonControlsEx(&icc);
    {   HDC dc = GetDC(NULL); g_dpi = g_dpi_sys = GetDeviceCaps(dc, LOGPIXELSY); ReleaseDC(NULL, dc); }
    fit_dpi();                                              /* finding 123: smaller than the system's scale when the window would not fit */
    {   /* dark controls: uxtheme's unnamed exports SetPreferredAppMode (135; 2 = force dark) and AllowDarkModeForWindow (133),
           as used by Explorer and Notepad; without them (older Windows) the controls stay light and everything still works */
        HMODULE ux = LoadLibraryExW(L"uxtheme.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
        int (WINAPI *set_mode)(int) = ux ? (int (WINAPI *)(int))(void *)GetProcAddress(ux, MAKEINTRESOURCEA(135)) : NULL;
        if (set_mode) set_mode(2);
        if (ux) p_AllowDarkModeForWindow = (BOOL (WINAPI *)(HWND, BOOL))(void *)GetProcAddress(ux, MAKEINTRESOURCEA(133));
    }
    /* paths: the repository is the folder holding scripts\setup.py: this exe's own (B2recomp.exe of the builder package),
       else the one above it (build\launcher.exe) */
    wchar_t exe[MAX_PATH], tmp[MAX_PATH];
    GetModuleFileNameW(NULL, exe, MAX_PATH);
    *wcsrchr(exe, L'\\') = 0;
    swprintf(tmp, MAX_PATH, L"%s\\scripts\\setup.py", exe);
    if (file_exists(tmp)) wcscpy(g_root, exe);
    else { swprintf(tmp, MAX_PATH, L"%s\\..", exe); GetFullPathNameW(tmp, MAX_PATH, g_root, NULL); }
    swprintf(g_buildd, MAX_PATH, L"%s\\build", g_root);
    swprintf(g_ini[F_GFX], MAX_PATH, L"%s\\config\\graphics.ini", g_root);
    swprintf(g_ini[F_PAD], MAX_PATH, L"%s\\config\\controllers.ini", g_root);
    swprintf(g_game, MAX_PATH, L"%s\\game_native.exe", g_buildd);
    swprintf(g_elf, MAX_PATH, L"%s\\targets\\exefs\\main.elf", g_root);
    swprintf(g_modsdir, MAX_PATH, L"%s\\mods", g_root);
    swprintf(g_modsini, MAX_PATH, L"%s\\config\\mods.ini", g_root);
    {   /* [Recomps#1] the package's default settings (config\defaults) become config\*.ini only when those are missing, so an
           update unpacked over the old folder keeps the player's own (as B2recomp.sh on Linux; needs_update above) */
        static const wchar_t *k_def[] = { L"graphics.ini", L"controllers.ini" };
        for (int i = 0; i < 2; i++) {
            wchar_t s[MAX_PATH], d[MAX_PATH];
            swprintf(s, MAX_PATH, L"%s\\config\\defaults\\%s", g_root, k_def[i]);
            swprintf(d, MAX_PATH, L"%s\\config\\%s", g_root, k_def[i]);
            if (file_exists(s) && !file_exists(d)) CopyFileW(s, d, TRUE);
        }
    }
    swprintf(tmp, MAX_PATH, L"%s\\scripts\\setup.py", g_root);
    g_update = file_exists(tmp) && game_built() && needs_update();
    g_mode = file_exists(tmp) && (!game_built() || g_update || (cmdline && wcsstr(cmdline, L"--build")));
    find_python();
    /* the game and the pad tools must read the files edited here and nothing of a test run (play.ps1's list + config overrides) */
    static const wchar_t *k_clear[] = { L"SSBU_SEQ", L"SSBU_SCRIPT", L"SSBU_PRESS", L"SSBU_SHOT", L"SSBU_SHOT_EVERY", L"SSBU_SHOT_DIR",
                                        L"SSBU_PROF15", L"SSBU_HITCH", L"SSBU_TRACE", L"SSBU_CONFIG", L"SSBU_GRAPHICS_CONFIG", L"SSBU_PAD_LAYOUT" };
    for (int i = 0; i < (int)(sizeof k_clear / sizeof k_clear[0]); i++) SetEnvironmentVariableW(k_clear[i], NULL);

    NONCLIENTMETRICSW ncm = { sizeof ncm };
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0);
    ncm.lfMessageFont.lfHeight = MulDiv(ncm.lfMessageFont.lfHeight, g_dpi, g_dpi_sys);      /* finding 123: the system's font, at our scale */
    g_font = CreateFontIndirectW(&ncm.lfMessageFont);
    LOGFONTW lf = ncm.lfMessageFont;
    lf.lfWeight = FW_BOLD; lf.lfHeight = -MulDiv(12, g_dpi, 72);
    g_bigfont = CreateFontIndirectW(&lf);                    /* Play */
    lf = ncm.lfMessageFont; lf.lfWeight = FW_SEMIBOLD; lf.lfHeight = -MulDiv(9, g_dpi, 72);
    g_headfont = CreateFontIndirectW(&lf);
    lf = ncm.lfMessageFont; lf.lfHeight = -MulDiv(8, g_dpi, 72);
    g_smallfont = CreateFontIndirectW(&lf);
    lf = ncm.lfMessageFont; lf.lfWeight = FW_BOLD; lf.lfHeight = -MulDiv(34, g_dpi, 72); wcscpy(lf.lfFaceName, L"Palatino Linotype");
    g_titlefont = CreateFontIndirectW(&lf);
    g_brbg = CreateSolidBrush(C_BG); g_brpanel = CreateSolidBrush(C_PANEL); g_brfield = CreateSolidBrush(C_FIELD);
    load_art();

    WNDCLASSEXW wc = { sizeof wc };                          /* icon resource 1: the game's icon (scripts\make_icon_local.py) */
    wc.lpfnWndProc = wndproc; wc.hInstance = inst; wc.lpszClassName = L"B2recompLauncher";
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW); wc.hbrBackground = g_brbg;
    wc.hIcon = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), 0);
    wc.hIconSm = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
    if (!wc.hIcon) wc.hIcon = LoadIconW(NULL, IDI_APPLICATION);
    RegisterClassExW(&wc);
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN;
    RECT r = { 0, 0, S(WIN_W), S(WIN_H) };
    AdjustWindowRectEx(&r, style, FALSE, WS_EX_CONTROLPARENT);
    int ww = r.right - r.left, wh = r.bottom - r.top;
    RECT wa; SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    HWND w = CreateWindowExW(WS_EX_CONTROLPARENT, wc.lpszClassName, g_mode ? L"B2recomp" : L"B2recomp Launcher", style,
                             wa.left + (wa.right - wa.left - ww) / 2, wa.top + (wa.bottom - wa.top - wh) / 2, ww, wh, NULL, NULL, inst, NULL);
    set_window_icon(w);                                     /* build\bayonetta2.ico wins over resource 1: a launcher built
                                                               without the icon still shows the player's own */
    {   BOOL on = TRUE; COLORREF text = RGB(200, 214, 240);  /* dark title bar in the colour of the art (Windows 11; ignored before) */
        DwmSetWindowAttribute(w, 20, &on, sizeof on);        /* DWMWA_USE_IMMERSIVE_DARK_MODE */
        DwmSetWindowAttribute(w, 35, &g_caption, sizeof g_caption);   /* DWMWA_CAPTION_COLOR */
        DwmSetWindowAttribute(w, 36, &text, sizeof text); }  /* DWMWA_TEXT_COLOR */
    ShowWindow(w, show);
    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (kcapture(&msg)) continue;                       /* a keybinds slot waiting for its key */
        if (g_kwin && IsDialogMessageW(g_kwin, &msg)) continue;
        if (g_mwin && IsDialogMessageW(g_mwin, &msg)) continue;
        if (IsDialogMessageW(w, &msg)) continue;
        TranslateMessage(&msg); DispatchMessageW(&msg);
    }
    return 0;
}
