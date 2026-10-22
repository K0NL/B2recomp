/* Track O (options), first set (track V session 4): config\graphics.ini.
 *
 * Path: SSBU_GRAPHICS_CONFIG=<file> when set; else graphics.ini beside the file SSBU_CONFIG names (SSBU_CONFIG is the
 * path of controllers.ini, nnpad_sdl.c); else <exe folder>\..\config\graphics.ini. Written with the defaults and their
 * documentation when it does not exist. Read ONCE at start (the window, the device and the swapchain are made from it).
 * Environment switches of the test scripts (SSBU_RENDERER, SSBU_VK_DEVICE, SSBU_VK_PRESENT) win over the file.
 * The controllers.ini reader of nnpad_sdl.c is built around its device sections and is not reusable here: this file has
 * its own 30-line reader (key = value, ';' or '#' comments, section headers ignored). */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gfx_options.h"
#ifndef L1_LATENCY_DEFAULT
#define L1_LATENCY_DEFAULT 0      /* track L1: normal (coordinator 2026-10-14); low is an option */
#endif
static int g_gfx_latency = L1_LATENCY_DEFAULT;      /* track L1: latency = normal 0 | low 1 | lowest 2 (display.c); read with gfx_latency() */
int gfx_latency(void) { gfx_options(); return g_gfx_latency; }
static int g_gfx_runahead;                           /* track R1 round 5: runahead = off 0 | on 1 (r1_state.c); read with gfx_runahead() */
int gfx_runahead(void) { gfx_options(); return g_gfx_runahead; }

static const char *const k_default_ini =
"; graphics.ini: display and rendering options of the native build (read once at start by gfx_options.c).\r\n"
"; Delete the file to get it written again with these defaults. A line starting with ';' is a comment.\r\n"
"; Every value below is the default; the defaults draw exactly what the game draws on the console.\r\n"
"\r\n"
"; renderer = vulkan | opengl\r\n"
";   vulkan: the default since 2026-10-09. opengl: the first renderer (NVIDIA only), kept as a fallback.\r\n"
"renderer = vulkan\r\n"
"\r\n"
"; window = windowed | borderless | fullscreen\r\n"
";   windowed:   a normal window with the client size below.\r\n"
";   borderless: no frame, covers the monitor the window is on (the desktop keeps its resolution).\r\n"
";   fullscreen: as borderless, and always on top. (Exclusive fullscreen with a mode change is not implemented.)\r\n"
"window = windowed\r\n"
"width = 1280\r\n"
"height = 720\r\n"
"\r\n"
"; vsync = auto | display | clock | off | on | mailbox      (Vulkan)\r\n"
";   auto:    the refresh rate of the monitor is measured at start. 60, 120, 240 Hz (within 1.1 %: 59.94, 119.88 too):\r\n"
";            as 'display', and checked every 5 s: when the game's speed leaves 60 +-1.1 % (a variable refresh rate:\r\n"
";            G-Sync / FreeSync), as 'clock' until the refresh is steady again. Any other rate (75, 100, 144, 165 Hz):\r\n"
";            as 'clock'. The log says which ('display:' lines).\r\n"
";   display: the game's frame clock IS the display's refresh (every refresh at 60 Hz, every second one at 120 Hz):\r\n"
";            each frame is on screen for exactly the same time. No tearing. The game runs at refresh / k frames per\r\n"
";            second (59.94 on a 119.88 Hz panel). Falls back to 'clock' when the rate is not a multiple of 60 at start;\r\n"
";            keeps the lock if the refresh drifts later (the game then runs slower or faster than real time).\r\n"
";   clock:   the program's own 60.000 Hz clock; a finished frame is shown at the next tick of that clock (mailbox when\r\n"
";            the GPU offers it, else FIFO: no tearing). On a display that is not 60 / 120 / 240 Hz some frames stay on screen\r\n"
";            one refresh longer than others: that cannot be avoided there.\r\n"
";   off:     immediate presentation as soon as a frame is ready, tearing allowed. Lowest latency. Still 60 frames per second.\r\n"
";   on:      the swapchain's own vertical sync (FIFO) next to the program's clock: two clocks, not recommended. (auto,\r\n"
";            display and clock use FIFO too on a GPU without mailbox, e.g. AMD on Windows.)\r\n"
";   mailbox: as clock; FIFO when the GPU has no mailbox.\r\n"
"vsync = auto\r\n"
"\r\n"
"; latency = normal | low | lowest      (track L1, input latency; Vulkan)\r\n"
";   normal:  the game is let go at the tick. (The game's picture is shown one frame sooner than before 2026-10-14 in\r\n"
";            every mode: it goes to the window as soon as the game has submitted it.)\r\n"
";   low:     the game is let go late in each frame, just in time for its picture to be ready for the next tick, so it\r\n"
";            reads the controller 2-5 ms later = closer to the screen. Same cadence; with four players a few more late frames.\r\n"
";   lowest:  as low, and the picture goes to the window as soon as it is ready (no wait for the tick): about 4 ms\r\n"
";            less again, but the frames are no longer all on screen for the same time (judder).\r\n"
"latency = normal\r\n"
"\r\n"
"; runahead = off | on      (track R1, input latency; Vulkan; needs the build with dirty-page marks)\r\n"
";   on: in a fight each frame's picture is drawn one game frame earlier: the state is saved after the game's\r\n"
";       submission, the next frame is run ahead only to draw the current one, then the state is restored. One frame\r\n"
";       less delay; costs a second pass of the game's frame (CPU) every frame. Leave it off on a weak PC.\r\n"
"runahead = off\r\n"
"\r\n"
"; gpu = auto | <number> | <part of the name>      (Vulkan)\r\n"
";   auto: a discrete GPU first, then an integrated one. The log lists the devices with their numbers:\r\n"
";   'nvn_vk: device 0: ...'. Examples: gpu = 1      gpu = Intel      gpu = RTX\r\n"
"gpu = auto\r\n"
"\r\n"
"; console_mode = docked | handheld\r\n"
";   What nn::oe::GetOperationMode answers. MEASURED on 13.0.4 (2026-10-09): with handheld the game still makes its\r\n"
";   1920x1080 window texture, so this alone does not lower the render size; kept for study, leave it at docked.\r\n"
"console_mode = docked\r\n"
"\r\n"
"; resolution_scale = 1.0\r\n"
/* PR #8 (stephenjayakar): the range goes above 1.0 (supersampling); capped at 2.0 there (review 2026-10-16), strong GPUs only */
/* [Recomps#1] Bayonetta 2's sizes (1280x720) and the measurements of finding 83; up to 3.0 = 4K (finding 105) */
";   Internal resolution scale, 0.25 to 3.0 (Vulkan only; OpenGL ignores it). 1.0 = the game's own size: Bayonetta 2\r\n"
";   renders at 1280x720. 1.5 = 1920x1080, 2.0 = 2560x1440, 3.0 = 3840x2160 (4K) (drawn larger, reduced to the window:\r\n"
";   sharper edges). Measured in the first tutorial fight on an RTX 5070 (2026-10-16/17): GPU time per frame 1.68 ms at\r\n"
";   1.0, 1.96 ms at 1.5, 2.32 ms at 2.0; 59.3-59.5 fps up to 3.0; 2.0 takes about 300 MB more video memory, 3.0 about\r\n"
";   640 MB (strong GPUs only). Below 1.0 not tested.\r\n"
"resolution_scale = 1.0\r\n"
"\r\n"
"; anisotropic_filtering = game | 2 | 4 | 8 | 16      (Vulkan)\r\n"
";   game: each texture sampler keeps the value the game gave it. A number: at least that much on every sampler that\r\n"
";   filters between mip levels (sharper textures at flat angles; changes the picture, costs little on any GPU).\r\n"
"anisotropic_filtering = game\r\n"
"\r\n"
"; shader_wait = hold | skip      (Vulkan)\r\n"
";   What happens when a draw needs a shader or pipeline that is not ready yet (mostly the first time something is shown\r\n"
";   on this PC). hold: the frame waits for it (a late frame, a correct picture). skip: in and around a fight the draw is\r\n"
";   left out until it is ready (no late frame; the object or effect is missing for some frames). The log counts both.\r\n"
"shader_wait = hold\r\n"
"\r\n"
"; shader_prepare = first_run | background | off      (Vulkan)\r\n"
";   No effect in B2recomp ([Recomps#1]): preparing every shader ahead reads Smash's data.arc, which Bayonetta 2 does not\r\n"
";   have. Shaders and pipelines are made the first time the game needs them and kept in ..\\cache\\vk, so later starts\r\n"
";   reuse them (delete that folder to start over, e.g. after a new graphics driver).\r\n"
"shader_prepare = first_run\r\n"
"\r\n"
"; profile = custom | low\r\n"
";   custom: every key of this file as written. low: the set measured for a weak or integrated GPU (2026-10-12, Intel\r\n"
";   integrated: 4.2 ms of GPU time per fight frame instead of 9.0): resolution_scale = 0.5, anisotropic_filtering = game,\r\n"
";   vsync = clock; those three keys of the file are then not used. The log says which profile is active.\r\n"
"profile = custom\r\n"
"\r\n"
"; aspect = keep | stretch      (Vulkan)\r\n"
";   [Recomps#1] keep: the game's 16:9 picture keeps its shape; a wider or taller window (an ultrawide monitor in\r\n"
";   borderless or fullscreen mode) gets black bars at the sides or at the top and bottom. stretch: the picture fills\r\n"
";   the window whatever its shape.\r\n"
"aspect = keep\r\n"
"\r\n"
"; frame_time_overlay = off | on\r\n"
";   on: the average frame time of the last second in the window title ('.. fps - 16.67 ms per frame') and as a\r\n"
";   'frame-time:' line in the log. (Bars drawn into the picture are not implemented yet.)\r\n"
"frame_time_overlay = off\r\n"
"\r\n"
"; frame_rate = 60      (60 .. 120; EXPERIMENTAL above 60)\r\n"
";   60: the game as made (60 frames per second, its own 30 fps cinematics). 61 .. 120: the game's frame clock measures\r\n"
";   real time and the frame rate is capped at this value; its timers that count frames are converted to game time\r\n"
";   (scripts/fps_patch.py, notes/fps-research.md). Not every frame counter is converted yet: some effects, menus and\r\n"
";   button-mash prompts may run faster than at 60, and the cinematics run in real time. vsync = clock / display /\r\n"
";   auto still show each frame at once above 60 (no tearing: mailbox, else FIFO, which shows at most one frame per refresh).\r\n"
"frame_rate = 60\r\n"
"\r\n"
/* [Recomps#1] finding 147 */
"; hdr = auto | off | on      (Vulkan, Windows)\r\n"
";   auto: while Windows HDR is on for the monitor the game is on, the picture goes out as HDR (the game's colours at\r\n"
";   the brightness below, as Windows shows other programs); otherwise as before. on: always. off: never.\r\n"
"hdr = auto\r\n"
"\r\n"
"; hdr_format = hdr10 | scrgb\r\n"
";   hdr10: 10 bits a colour, PQ, BT.2020 (what HDR monitors take). scrgb: 16-bit floating point, linear.\r\n"
"hdr_format = hdr10\r\n"
"\r\n"
"; hdr_paper_white = auto | 80 .. 1000\r\n"
";   How bright the game's white is with HDR on, in nits. auto: Windows' 'SDR content brightness' of that monitor\r\n"
";   (Settings > System > Display > HDR), followed while the game runs.\r\n"
"hdr_paper_white = auto\r\n"
"\r\n"
"; hdr_gamma = 2.2 | srgb\r\n"
";   How the game's colours are read with HDR on. 2.2: as an SDR monitor shows them (deep blacks). srgb: as Windows\r\n"
";   shows SDR programs in HDR (lighter dark parts).\r\n"
"hdr_gamma = 2.2\r\n";

#define GFX_LOW_SCALE 0.5f
#define GFX_LOW_SCALE_TEXT "0.50"
static GfxOptions g_opt;
static INIT_ONCE g_once = INIT_ONCE_STATIC_INIT;

static void trim(char *s) {
    char *e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n')) *--e = 0;
    char *b = s;
    while (*b == ' ' || *b == '\t') b++;
    if (b != s) memmove(s, b, strlen(b) + 1);
}
static void opt_path(char *out) {
    const char *e = getenv("SSBU_GRAPHICS_CONFIG"), *c = getenv("SSBU_CONFIG");
    char tmp[MAX_PATH], exe[MAX_PATH];
    if (e && e[0]) { snprintf(out, MAX_PATH, "%s", e); return; }
    if (c && c[0]) {
        snprintf(tmp, sizeof tmp, "%s", c);
        char *s = strrchr(tmp, '\\'), *s2 = strrchr(tmp, '/');
        if (s2 > s) s = s2;
        if (s) { *s = 0; snprintf(out, MAX_PATH, "%s\\graphics.ini", tmp); } else snprintf(out, MAX_PATH, "graphics.ini");
        return;
    }
    GetModuleFileNameA(NULL, exe, MAX_PATH);
    char *s = strrchr(exe, '\\');
    if (s) *s = 0;
    snprintf(tmp, sizeof tmp, "%s\\..\\config\\graphics.ini", exe);
    if (!_fullpath(out, tmp, MAX_PATH)) snprintf(out, MAX_PATH, "%s", tmp);
}
static BOOL CALLBACK opt_load(PINIT_ONCE once, PVOID arg, PVOID *ctx) {
    (void)once; (void)arg; (void)ctx;
    GfxOptions *o = &g_opt;
    int written = 0, unknown = 0;
    memset(o, 0, sizeof *o);
    o->width = 1280; o->height = 720; o->scale = 1.0f; o->present = 5;      /* vsync = auto when the key is missing (track V session 14) */
    opt_path(o->path);
    FILE *f = fopen(o->path, "rb");
    if (!f) {
        char b[MAX_PATH];
        snprintf(b, sizeof b, "%s", o->path);
        for (char *p = b + 3; *p; p++) if (*p == '\\' || *p == '/') { *p = 0; CreateDirectoryA(b, NULL); *p = '\\'; }
        if ((f = fopen(o->path, "wb")) != NULL) { fwrite(k_default_ini, 1, strlen(k_default_ini), f); fclose(f); written = 1; }
        f = fopen(o->path, "rb");
    }
    char line[512];
    while (f && fgets(line, sizeof line, f)) {
        char *eq, *key = line, *val;
        trim(line);
        if (!line[0] || line[0] == ';' || line[0] == '#' || line[0] == '[' || !(eq = strchr(line, '='))) continue;
        *eq = 0; val = eq + 1;
        trim(key); trim(val);
        if (!_stricmp(key, "renderer")) o->renderer = !_stricmp(val, "opengl") || !_stricmp(val, "gl");
        else if (!_stricmp(key, "window")) o->window_mode = !_stricmp(val, "borderless") ? 1 : !_stricmp(val, "fullscreen") ? 2 : 0;
        else if (!_stricmp(key, "width")) { int v = atoi(val); if (v >= 320 && v <= 16384) o->width = v; }
        else if (!_stricmp(key, "height")) { int v = atoi(val); if (v >= 180 && v <= 16384) o->height = v; }
        else if (!_stricmp(key, "vsync")) o->present = !_stricmp(val, "on") ? 1 : !_stricmp(val, "off") ? 2 : !_stricmp(val, "mailbox") ? 3 : !_stricmp(val, "display") ? 4 : !_stricmp(val, "auto") ? 5 : 0;
        else if (!_stricmp(key, "gpu")) { if (_stricmp(val, "auto")) snprintf(o->gpu, sizeof o->gpu, "%s", val); }
        else if (!_stricmp(key, "console_mode")) o->handheld = !_stricmp(val, "handheld");
        else if (!_stricmp(key, "resolution_scale")) { float v = (float)atof(val); if (v > 0) o->scale = v; }
        else if (!_stricmp(key, "anisotropic_filtering")) { int v = atoi(val); o->aniso = v == 2 || v == 4 || v == 8 || v == 16 ? v : 0; }
        else if (!_stricmp(key, "frame_time_overlay")) o->overlay = !_stricmp(val, "on") || !strcmp(val, "1");
        else if (!_stricmp(key, "profile")) o->profile = !_stricmp(val, "low");
        else if (!_stricmp(key, "shader_wait")) o->shader_skip = !_stricmp(val, "skip");
        else if (!_stricmp(key, "runahead")) g_gfx_runahead = !_stricmp(val, "on") || !_stricmp(val, "1");      /* track R1 round 5 (r1_state.c) */
        else if (!_stricmp(key, "latency")) g_gfx_latency = !_stricmp(val, "lowest") ? 2 : !_stricmp(val, "low") ? 1 : 0;      /* track L1 (display.c latency_mode) */
        else if (!_stricmp(key, "aspect")) o->aspect_stretch = !_stricmp(val, "stretch");      /* [Recomps#1] */
        else if (!_stricmp(key, "shader_prepare")) o->shader_prepare = !_stricmp(val, "background") ? 1 : !_stricmp(val, "off") ? 2 : 0;
        else if (!_stricmp(key, "frame_rate")) { int v = atoi(val); o->frame_rate = v < 60 ? 60 : v > 120 ? 120 : v; }      /* [Recomps#1] */
        else if (!_stricmp(key, "hdr")) o->hdr = !_stricmp(val, "off") ? 1 : !_stricmp(val, "on") ? 2 : 0;      /* [Recomps#1] finding 147 */
        else if (!_stricmp(key, "hdr_format")) o->hdr_scrgb = !_stricmp(val, "scrgb");
        else if (!_stricmp(key, "hdr_paper_white")) { float v = (float)atof(val); o->hdr_paper_white = v >= 80.0f && v <= 1000.0f ? v : 0.0f; }      /* auto, or a value out of range */
        else if (!_stricmp(key, "hdr_gamma")) o->hdr_srgb = !_stricmp(val, "srgb");
        else unknown++;
    }
    if (f) fclose(f);
    if (!o->frame_rate) o->frame_rate = 60;
    /* [Recomps#1] frame_rate above 60: switch on the lifted code's frame-rate patch (scripts/fps_patch.py), which reads
     * RECOMPS_UNCAP / RECOMPS_UNCAP_FPS with getenv the first time the game's clock runs (after this, at the first frame).
     * The UCRT environment is one per process, so game.dll sees what is set here. Set variables (test runs) win. */
    if (o->frame_rate > 60 && !getenv("RECOMPS_UNCAP")) {
        char n[16]; snprintf(n, sizeof n, "%d", o->frame_rate);
        const char *pe = getenv("SSBU_PACE_120");      /* [Recomps#1] long frames at 120: the runtime paces (gfx_pace_rate below), not the lifted limiter */
        int paced = !(pe && !atoi(pe));
        _putenv_s("RECOMPS_UNCAP", "1");
        if (!getenv("RECOMPS_UNCAP_FPS")) _putenv_s("RECOMPS_UNCAP_FPS", paced ? "0" : n);
        printf("graphics options: frame_rate=%d: the game's clock measures real time, %s %d fps (experimental; notes/fps-research.md)\n", o->frame_rate,
               paced ? "the game released and its pictures presented on a steady tick of" : "the lifted limiter caps it at (SSBU_PACE_120=0)", o->frame_rate);
    }
    if (o->profile == 1) {       /* the low-end profile wins over the three keys it sets, wherever they stand in the file (values: O1_options.md, session 9) */
        o->scale = GFX_LOW_SCALE; o->aniso = 0; o->present = 0;
    }
    printf("graphics profile: %s\n", o->profile == 1 ? "low (weak or integrated GPU): resolution_scale=" GFX_LOW_SCALE_TEXT ", anisotropic_filtering=game, vsync=clock; these three keys of the file are not used"
                                                     : "custom (every key of graphics.ini as written; 'profile = low' is the set for a weak or integrated GPU)");
    static const char *const k_win[3] = {"windowed", "borderless", "fullscreen"}, *const k_vs[6] = {"clock", "on", "off", "mailbox", "display", "auto"};
    printf("graphics options: %s (%s): renderer=%s window=%s %dx%d vsync=%s gpu=%s console_mode=%s resolution_scale=%.2f anisotropic_filtering=%d frame_time_overlay=%s aspect=%s%s\n", o->path,
           written ? "written with the defaults" : f ? "read" : "NOT readable, defaults used", o->renderer ? "opengl" : "vulkan", k_win[o->window_mode], o->width, o->height, k_vs[o->present],
           o->gpu[0] ? o->gpu : "auto", o->handheld ? "handheld" : "docked", o->scale, o->aniso, o->overlay ? "on" : "off", o->aspect_stretch ? "stretch" : "keep", unknown ? " (lines with an unknown key were ignored)" : "");
    {   char pw[16]; static const char *const k_hdr[3] = {"auto", "off", "on"};      /* [Recomps#1] finding 147 */
        if (o->hdr_paper_white > 0) snprintf(pw, sizeof pw, "%.0f", o->hdr_paper_white); else snprintf(pw, sizeof pw, "auto");
        printf("graphics options: hdr=%s hdr_format=%s hdr_paper_white=%s hdr_gamma=%s\n", k_hdr[o->hdr], o->hdr_scrgb ? "scrgb" : "hdr10", pw, o->hdr_srgb ? "srgb" : "2.2"); }
    if (o->scale != 1.0f && o->renderer) printf("graphics options: resolution_scale=%.2f is ignored by the OpenGL renderer (Vulkan only)\n", o->scale);
    if (o->scale != 1.0f && (o->scale < 0.25f || o->scale > 3.0f)) printf("graphics options: resolution_scale=%.2f is outside 0.25 .. 3.0: the nearest of the two is used\n", o->scale);      /* PR #8 (stephenjayakar): above 1.0 allowed (nvn_vk_draw.c RS_MAX = 2.0) */
    fflush(stdout);
    return TRUE;
}
const GfxOptions *gfx_options(void) {
    InitOnceExecuteOnce(&g_once, opt_load, NULL, NULL);
    return &g_opt;
}
/* [Recomps#1] long frames at 120 fps (2026-10-21): the rate display.c paces the game at. Above 60 fps the lifted limiter
 * started every frame on its own 120.000 Hz clock and each picture was presented the moment it was ready, so every
 * millisecond of jitter in the game's logic and our replay reached the screen (on a 238 Hz panel 10-20 % of the frames
 * were not shown for two refreshes, against 1 % at 60). Now display.c paces at frame_rate as it does at 60: the game is
 * released on a tick (the display's vblank when the refresh is k x frame_rate, else our own clock) and its picture is
 * held for the next tick. SSBU_PACE_120=0: the lifted limiter and pictures as soon as ready, as before. */
int gfx_pace_rate(void) {
    static int r = -1;
    if (r < 0) {
        const GfxOptions *o = gfx_options();
        const char *e = getenv("SSBU_PACE_120"), *u = getenv("RECOMPS_UNCAP_FPS");
        r = o->frame_rate > 60 && !(e && !atoi(e)) && u && !atoi(u) ? o->frame_rate : 60;      /* a test's own limiter rate (RECOMPS_UNCAP_FPS) keeps the old way */
    }
    return r;
}
