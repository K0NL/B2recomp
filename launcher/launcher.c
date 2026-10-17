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
static const Opt o_prepare[] = { {"first_run", L"Before the first start (once)"}, {"background", L"In the background"},
                                 {"off", L"Off (build when first needed)"} };
static const Opt o_layout[] = { {"auto", L"Auto"}, {"positional", L"By position (Switch)"}, {"labels", L"By printed letter"} };
#define OPTS(a) a, (int)(sizeof a / sizeof a[0])

enum { S_RENDERER, S_PROFILE, S_WINDOW, S_WIDTH, S_HEIGHT, S_VSYNC, S_LATENCY, S_SCALE, S_ANISO, S_PREPARE, S_OVERLAY,
       S_LAYOUT, S_DEADZONE, S_RUMBLE, S_STRENGTH, S_COUNT };
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
                     .tip = L"Auto: 'display' on a 60, 120 or 240 Hz monitor, else the program's own 60 Hz clock. Off: tearing allowed, lowest latency." },
    [S_LATENCY]  = { F_GFX, "latency", K_COMBO, OPTS(o_latency), 0, .def = "normal", .label = L"Input latency",
                     .tip = L"Low: the controller is read 2-5 ms later, closer to the screen. Lowest: about 4 ms less again, but frames no longer last the same time." },
    [S_SCALE]    = { F_GFX, "resolution_scale", K_SLIDER, .lo = 0.5, .hi = 2.0, .step = 0.05, .def = "1.0", .label = L"Resolution scale",
                     .tip = L"Internal resolution. 1.0 = the game's 1280x720; 1.5 = 1920x1080; 2.0 = 2560x1440 (sharper edges)." },
    [S_ANISO]    = { F_GFX, "anisotropic_filtering", K_COMBO, OPTS(o_aniso), 0, .def = "game", .label = L"Anisotropic filtering",
                     .tip = L"Sharper textures at flat angles; costs little on any GPU." },
    [S_PREPARE]  = { F_GFX, "shader_prepare", K_COMBO, OPTS(o_prepare), 0, .def = "first_run", .label = L"Shader preparation",
                     .tip = L"No effect in B2recomp: preparing shaders ahead reads a data file only Smash has. Shaders are made the "
                             L"first time the game needs them and kept in the cache folder, so later starts reuse them." },
    [S_OVERLAY]  = { F_GFX, "frame_time_overlay", K_CHECK, .def = "off", .label = L"Show frame time in the window title" },
    [S_LAYOUT]   = { F_PAD, "layout", K_COMBO, OPTS(o_layout), 1, .def = "auto", .label = L"Face buttons",
                     .tip = L"By position: the right button is A, bottom B, top X, left Y (as on a Switch). By printed letter: an Xbox pad's A is A. Auto: by letter on GameCube pads, by position on all others." },
    [S_DEADZONE] = { F_PAD, "deadzone", K_SLIDER, .lo = 0.0, .hi = 0.5, .step = 0.01, .def = "0.10", .label = L"Stick dead zone",
                     .tip = L"Part of the stick travel around the centre that counts as no movement." },
    [S_RUMBLE]   = { F_PAD, "rumble", K_CHECK, .def = "on", .label = L"Vibration" },
    [S_STRENGTH] = { F_PAD, "rumble_strength", K_SLIDER, .lo = 0.0, .hi = 2.0, .step = 0.05, .def = "1.0", .label = L"Vibration strength" },
};

/* ================================================================ paths and state */
static wchar_t g_root[MAX_PATH], g_buildd[MAX_PATH], g_ini[2][MAX_PATH], g_game[MAX_PATH], g_elf[MAX_PATH];
static int g_exists[2], g_loading, g_devover;
static HWND g_main, g_tip, g_status, g_save, g_play, g_sizes, g_padnote, g_gfxnote;
static HFONT g_font, g_bigfont, g_smallfont;
static HANDLE g_proc;
static int g_dpi = 96;
#define S(x) MulDiv((x), g_dpi, 96)
enum { ID_SET = 100, ID_SIZES = 200, ID_REMAP, ID_TEST, ID_SAVE, ID_PLAY, ID_CLOSE, ID_CONFIG, ID_STATUS, ID_TIMER = 1 };
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
        if (s == &g_s[S_DEADZONE]) snprintf(out, n, "%.2f", slider_value(s, slider_pos(s))); else fmt_num(slider_value(s, slider_pos(s)), out, n);
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
    EnableWindow(g_s[S_PREPARE].hw, FALSE);                /* [Recomps#1] no effect for Bayonetta 2 (tip below) */
    int rumble = SendMessageW(g_s[S_RUMBLE].hw, BM_GETCHECK, 0, 0) == BST_CHECKED;
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
            for (int k = S_LAYOUT; k < S_COUNT; k++) if (!_stricmp(key, g_s[k].key)) { g_devover++; counted = 1; break; }
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
#define WIN_H 724
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
static const RECT k_panel[2] = { {14, 232, 454, 660}, {466, 232, 766, 660} };   /* graphics, controller (96-dpi px) */
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
    mk_setting(S_LATENCY, lx, cx, cw, y); y += dy;
    mk_setting(S_SCALE, lx, cx, cw, y); y += dy + 18;
    mk_setting(S_ANISO, lx, cx, cw, y); y += dy;
    mk_setting(S_PREPARE, lx, cx, cw, y); y += dy;
    mk_setting(S_OVERLAY, lx, cx, cw, y); y += dy;
    g_gfxnote = mk(L"STATIC", L"", SS_LEFT, lx, y + 2, gp->right - gp->left - 32, 34, -1);
    SendMessageW(g_gfxnote, WM_SETFONT, (WPARAM)g_smallfont, FALSE);
    /* controller: right panel */
    const RECT *pp = &k_panel[1];
    int pw = pp->right - pp->left;
    lx = pp->left + 16; cx = pp->left + 122; cw = pp->right - 16 - cx; y = pp->top + 46;
    mk_setting(S_LAYOUT, lx, cx, cw, y); y += dy;
    mk_setting(S_DEADZONE, lx, cx, cw, y); y += dy;
    mk_setting(S_RUMBLE, lx, cx, cw, y); y += dy;
    mk_setting(S_STRENGTH, lx, cx, cw, y); y += dy + 12;
    HWND b = mk(L"BUTTON", L"Remap controller...", BS_PUSHBUTTON | WS_TABSTOP, lx, y, pw - 32, 30, ID_REMAP);
    tip(b, L"Runs game_native.exe --pad-map 1 in a console window: press each button it names on player 1's pad (skipped after 8 s). "
           L"It writes a [device] section for that pad to controllers.ini.");
    y += 38;
    b = mk(L"BUTTON", L"Test controller...", BS_PUSHBUTTON | WS_TABSTOP, lx, y, pw - 32, 30, ID_TEST);
    tip(b, L"Runs game_native.exe --pad-test in a console window: lists the pads SDL sees and shows the buttons you press. Close the window to stop.");
    y += 44;
    g_padnote = mk(L"STATIC", L"", SS_LEFT, lx, y, pw - 32, 64, -1);
    SendMessageW(g_padnote, WM_SETFONT, (WPARAM)g_smallfont, FALSE);
    /* bottom bar: status, small buttons, Play */
    int by = k_panel[0].bottom + 12, px = WIN_W - 14 - 150;
    g_status = mk(L"STATIC", L"", SS_LEFT | SS_ENDELLIPSIS, 16, by + 11, px - 12 - 3 * 90 - 2 * 8 - 20 - 16, 20, ID_STATUS);
    b = mk(L"BUTTON", L"Config folder", BS_PUSHBUTTON | WS_TABSTOP, px - 12 - 3 * 90 - 2 * 8, by + 3, 90, 34, ID_CONFIG);
    tip(b, L"Opens the folder with graphics.ini and controllers.ini (every option is explained there).");
    g_save = mk(L"BUTTON", L"Save", BS_PUSHBUTTON | WS_TABSTOP, px - 12 - 2 * 90 - 8, by + 3, 90, 34, ID_SAVE);
    mk(L"BUTTON", L"Close", BS_PUSHBUTTON | WS_TABSTOP, px - 12 - 90, by + 3, 90, 34, ID_CLOSE);
    g_play = mk(L"BUTTON", L"Play", BS_OWNERDRAW | WS_TABSTOP, px, by, 150, 40, ID_PLAY);
    SetWindowSubclass(g_play, play_hover, 1, 0);
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
static const RECT k_bpanel = { 14, 232, 766, 660 };
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
 * targets\exefs\main.elf is missing (or with --build) the window shows this screen instead of the settings. The player
 * chooses the dump folder (Browse, typed, or dragged onto the window); Build runs scripts\setup.py --dump <folder> with
 * deps\python\python.exe when the package has it (else python.exe from PATH), in a hidden console of its own inside a job
 * object, so Close stops it and every compiler it started. Its output goes to the log box; the nine steps are ticked off
 * from its "[i/9] step ..." lines and the compile step's progress comes from the "[n/388]" lines of build\logs\compile.log.
 * When it succeeds the window turns into the launcher. */
enum { ID_BPATH = 400, ID_BBROWSE, ID_BUILD, ID_BLOGS, ID_BCHECK, ID_BLOG, ID_BPROG, ID_BSTEP0 = 420 };
enum { WM_APP_LINE = WM_APP + 1, WM_APP_DONE };
#define NSTEP 9
static const wchar_t *k_step_name[NSTEP] = { L"Check the bundled tools", L"Check your dump", L"Convert the program files",
    L"Generate tables", L"Translate the game code", L"Compile it (the longest step)", L"Make the icon",
    L"Link the game and the launcher", L"Self-test" };
static const int k_step_weight[NSTEP] = { 1, 2, 1, 2, 11, 240, 1, 8, 2 };   /* seconds on a 16-thread PC (finding 102) */
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
        else if (g_bstate[i] == 1 && i == 5) done += k_step_weight[i] * g_bcompile / 1000;
    }
    SendMessageW(g_bprog, PBM_SETPOS, (WPARAM)(total ? done * 1000 / total : 0), 0);
}
static void compile_progress(void) {                        /* the last "[n/m]" line of build\logs\compile.log */
    if (g_bstate[5] != 1) return;
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
static void log_append(const wchar_t *s) {
    int n = GetWindowTextLengthW(g_blog);
    if (n > 200000) { SendMessageW(g_blog, EM_SETSEL, 0, n / 2); SendMessageW(g_blog, EM_REPLACESEL, FALSE, (LPARAM)L""); n = GetWindowTextLengthW(g_blog); }
    SendMessageW(g_blog, EM_SETSEL, n, n);
    SendMessageW(g_blog, EM_REPLACESEL, FALSE, (LPARAM)s);
    SendMessageW(g_blog, EM_REPLACESEL, FALSE, (LPARAM)L"\r\n");
}
static void build_line(const wchar_t *s) {                 /* one line of setup.py's output */
    log_append(s);
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

typedef struct { HANDLE rd, proc; } Reader;
static void post_line(const char *s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    wchar_t *w = malloc((size_t)n * sizeof *w);
    if (!w) return;
    MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n);
    if (!PostMessageW(g_main, WM_APP_LINE, 0, (LPARAM)w)) free(w);
}
static DWORD WINAPI reader(LPVOID arg) {                   /* setup.py's stdout, line by line, to the window */
    Reader *r = arg;
    static char line[8192];
    char buf[4096];
    DWORD got, code = 1;
    int n = 0;
    while (ReadFile(r->rd, buf, sizeof buf, &got, NULL) && got) {
        for (DWORD k = 0; k < got; k++) {
            if (buf[k] == '\r') continue;
            if (buf[k] == '\n' || n == (int)sizeof line - 1) { line[n] = 0; post_line(line); n = 0; if (buf[k] == '\n') continue; }
            line[n++] = buf[k];
        }
    }
    if (n) { line[n] = 0; post_line(line); }
    WaitForSingleObject(r->proc, INFINITE);
    GetExitCodeProcess(r->proc, &code);
    CloseHandle(r->rd);
    free(r);
    PostMessageW(g_main, WM_APP_DONE, code, 0);
    return 0;
}
static void build_enable(int on) {
    EnableWindow(g_bpath, on); EnableWindow(g_bbrowse, on);
    EnableWindow(g_bbtn, on && g_bok);
    SetWindowTextW(g_bbtn, on ? L"Build" : L"Building");
}
static void build_start(void) {
    dump_check();
    if (!g_bok || g_bproc) return;
    if (!*g_python) {
        MessageBoxW(g_main, L"Python was not found. The builder package brings its own in deps\\python; from the source, install "
                            L"Python 3.10+ (python.org) and start this again.", L"B2recomp", MB_ICONERROR);
        return;
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
    if (!CreatePipe(&rd, &wr, &sa, 0)) return;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    /* a console of its own, hidden: the compilers setup.py starts are console programs and share it instead of each
       opening a window */
    STARTUPINFOW si = { sizeof si };
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW; si.wShowWindow = SW_HIDE;
    si.hStdInput = nul; si.hStdOutput = si.hStdError = wr;
    PROCESS_INFORMATION pi;
    static wchar_t cmd[4 * MAX_PATH];
    swprintf(cmd, 4 * MAX_PATH, L"\"%s\" -u \"%s\\scripts\\setup.py\" --dump \"%s\"", g_python, g_root, g_dump);
    BOOL ok = CreateProcessW(g_python, cmd, NULL, NULL, TRUE, CREATE_NEW_CONSOLE | CREATE_SUSPENDED, NULL, g_root, &si, &pi);
    CloseHandle(wr);
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    if (!ok) { CloseHandle(rd); MessageBoxW(g_main, L"Could not start scripts\\setup.py.", L"B2recomp", MB_ICONERROR); return; }
    g_bjob = CreateJobObjectW(NULL, NULL);                  /* closing it (or B2recomp) stops setup.py and its compilers */
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION li = { 0 };
    li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (g_bjob) { SetInformationJobObject(g_bjob, JobObjectExtendedLimitInformation, &li, sizeof li); AssignProcessToJobObject(g_bjob, pi.hProcess); }
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    g_bproc = pi.hProcess;
    for (int i = 0; i < NSTEP; i++) { g_bstate[i] = 0; step_show(i); }
    g_bcompile = 0;
    progress_update();
    SetWindowTextW(g_blog, L"");
    build_enable(0);
    g_bt0 = GetTickCount();
    Reader *r = malloc(sizeof *r);
    r->rd = rd; r->proc = pi.hProcess;
    HANDLE th = CreateThread(NULL, 0, reader, r, 0, NULL);
    if (th) CloseHandle(th);
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
    for (int i = 0; i < NSTEP; i++) g_bstep[i] = mk(L"STATIC", L"", SS_LEFT, lx, y + i * 24, 270, 20, ID_BSTEP0 + i);
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
    SetWindowTextW(g_status, *g_python ? L"Choose your dump folder, then press Build."
                                       : L"Python was not found: the builder package brings its own (deps\\python).");
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

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR cmdline, int show) {
    (void)prev;
    SetProcessDPIAware();
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);   /* the folder dialog of the build screen */
    INITCOMMONCONTROLSEX icc = { sizeof icc, ICC_STANDARD_CLASSES | ICC_BAR_CLASSES | ICC_TAB_CLASSES };
    InitCommonControlsEx(&icc);
    {   HDC dc = GetDC(NULL); g_dpi = GetDeviceCaps(dc, LOGPIXELSY); ReleaseDC(NULL, dc); }
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
    swprintf(tmp, MAX_PATH, L"%s\\scripts\\setup.py", g_root);
    g_mode = file_exists(tmp) && (!game_built() || (cmdline && wcsstr(cmdline, L"--build")));
    find_python();
    /* the game and the pad tools must read the files edited here and nothing of a test run (play.ps1's list + config overrides) */
    static const wchar_t *k_clear[] = { L"SSBU_SEQ", L"SSBU_SCRIPT", L"SSBU_PRESS", L"SSBU_SHOT", L"SSBU_SHOT_EVERY", L"SSBU_SHOT_DIR",
                                        L"SSBU_PROF15", L"SSBU_HITCH", L"SSBU_TRACE", L"SSBU_CONFIG", L"SSBU_GRAPHICS_CONFIG", L"SSBU_PAD_LAYOUT" };
    for (int i = 0; i < (int)(sizeof k_clear / sizeof k_clear[0]); i++) SetEnvironmentVariableW(k_clear[i], NULL);

    NONCLIENTMETRICSW ncm = { sizeof ncm };
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0);
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
        if (IsDialogMessageW(w, &msg)) continue;
        TranslateMessage(&msg); DispatchMessageW(&msg);
    }
    return 0;
}
