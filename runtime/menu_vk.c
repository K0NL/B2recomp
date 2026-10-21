/* Edición mejorada: the in-game menu (Insert), drawn by the Vulkan back end. Included at the end of nvn_vk_draw.c: it uses
 * that file's device (V), recording state (R), memory blocks, image barriers and the glslang DLL.
 *
 * The panel is drawn on the CPU with GDI (Segoe UI, antialiased) into a 32-bit DIB only when something on it changes (at
 * most 4 times a second for the live numbers), copied into a sampled image, and put over the presented frame by one
 * full-screen triangle: the frame behind is darkened, the panel is slightly translucent (premultiplied alpha). Nothing is
 * drawn and nothing is read while the menu is closed except the key poll and the cheats that are switched on.
 *
 * Input: the open key (config\menu.ini "tecla", default INSERT) or L3+R3 on the pad. While open the game gets no keyboard,
 * mouse or pad input (nnhid.c asks menu_captures_input), the mouse cursor is free and clicks / the wheel go to the menu.
 * Off in scripted test runs (SSBU_SEQ / SSBU_SCRIPT) and with SSBU_MENU=0.
 *
 * Cheats (docs/edicion-mejorada/TRUCOS.md):
 *   - code switches: game.dll exports recomps_cheat[] (game_shim.c); scripts/cheats_patch.py puts the checks into the lifted
 *     C at the addresses of the Switch 1.2 cheats (build F19B2E01E1794F93). Read every time the game runs that code.
 *   - data writes (character, slot A weapon): the bytes of pearl23's "Misc Bayonetta 2 Cheats" 1.1 at main + offset,
 *     written every frame while chosen (as Atmosphere's cheat VM does); the bytes found there first are put back when the
 *     choice returns to Normal. Changed only outside a fight (the renderer's fight flag). */
#include <wchar.h>

int menu_captures_input(void);
int menu_wndmsg(HWND h, UINT m, WPARAM w, LPARAM l);
void menu_pad(uint64_t buttons, int lx, int ly);
static void menu_mods_read(void);

enum { MC_PERSONAJE, MC_TRUCOS, MC_ARMAS, MC_CAMARA, MC_GRAFICOS, MC_MODS, MC_DIAG, MC_COUNT };
static const char *const k_mcat[MC_COUNT] = { "Personaje", "Trucos", "Armas", "Cámara", "Gráficos", "Mods", "Diagnóstico" };

/* ---- data cheats: pearl23, Misc Bayonetta 2 Cheats 1.1 (F19B2E01E1794F93.txt), one-byte writes at main + offset */
typedef struct { uint32_t off; uint8_t val; } MPoke;
typedef struct { const char *name; MPoke p[6]; int n; const char *warn; } MChoice;
static const MChoice k_chars[] = {
    { "Normal (sin cambio)", {{0}}, 0, "El personaje que elijas en el juego." },
    { "Balder", {{0x194C3CC, 0x08}, {0x34E5898, 0x08}, {0x34E5790, 0x11}, {0x34E5794, 0x11}}, 4,
      "Puede bloquear algunos capítulos; morir con Balder bloquea el juego. Combina con el mod C02 Better Balder." },
    { "Rodin", {{0x194C3CC, 0x0A}, {0x19598C4, 0x0A}, {0x34E5898, 0x0A}, {0x34E5790, 0x12}, {0x34E5794, 0x12}}, 5,
      "Puede bloquear algunos capítulos; morir con Rodin bloquea el juego." },
    { "King Zero (inestable)", {{0x194C3CC, 0x39}, {0x19598C4, 0x39}, {0x34E5898, 0x39}}, 3,
      "El autor avisa: suele cerrarse el juego al activar Witch Time. No lo uses en partidas serias." },
    { "Rosa sin restricciones", {{0x194C3CC, 0x0C}, {0x19598C4, 0x0C}, {0x34E5898, 0x0C}}, 3,
      "Rosa con las armas de Bayonetta. Solo funcionan los Wicked Weaves de Madama Khepri." },
};
static const MChoice k_weapons[] = {
    { "Normal (sin cambio)", {{0}}, 0, "El arma que equipes en el juego." },
    { "Umbran Sisters", {{0x34E5790, 0x10}, {0x34E5794, 0x10}}, 2, "Arma no seleccionable normalmente." },
    { "Arwing", {{0x34E5790, 0x0B}, {0x34E5794, 0x0B}}, 2, "Arma del contenido de Nintendo." },
    { "Gatling descartada", {{0x34E5790, 0x09}, {0x34E5794, 0x09}}, 2, "Arma descartada: movimientos con el mod B06 βTartarus Moveset." },
    { "Híbrida Killgore/Sai-Fung", {{0x34E5790, 0x0A}, {0x34E5794, 0x0A}}, 2, "Arma descartada: movimientos con el mod B05." },
};
#define NCHARS ((int)(sizeof k_chars / sizeof k_chars[0]))
#define NWEAP ((int)(sizeof k_weapons / sizeof k_weapons[0]))
static const char *const k_wt[] = { "Normal", "Invencible (comprobación de Witch Time)", "Permanente: tiempo detenido", "Permanente: cámara lenta" };

static struct {
    int disabled, open, cat, row, nrows, focus_side, vk_open, vk_wt, dirty, made_fail;
    float fade;                                 /* 0 closed .. 1 open */
    LARGE_INTEGER t_last, qpf;
    double ft[120]; int ftn, fti;               /* frame times of the last presents, ms */
    double fps, ft_avg, ft_max;
    LONG64 next_stats;
    volatile int *cheat;                        /* game.dll recomps_cheat[16], NULL when the build has no cheat patch */
    int character, weapon;                      /* chosen data cheats */
    uint8_t saved_char[6], saved_weap[6]; int have_saved_char, have_saved_weap;
    char msg[200]; LONG64 msg_until;
    volatile LONG pad_buttons, pad_y, pad_x, pad_seen;
    int mouse_x, mouse_y, mouse_click, mouse_wheel;
    /* mods page, read at each opening */
    char mods_line[48][160]; int nmods_lines, mods_on, mods_conflicts;
    char version[32], cpu[96];
} M;

/* ---- Vulkan side */
static struct {
    VkImage img; VMem mem; VkImageView view; VkSampler smp; int w, h, layout_ready;
    VkBuffer stage[3]; VMem stage_mem[3]; int si; uint64_t stage_size;
    VkDescriptorSetLayout dsl; VkDescriptorPool dp; VkDescriptorSet ds; VkPipelineLayout pl; VkPipeline pipe;
    VkImage sc_img[16]; VkImageView sc_view[16];
    int pending_upload;
} MV;

/* ---- GDI side */
static struct { HDC dc; HBITMAP bmp; uint32_t *px; int w, h; HFONT f_title, f_body, f_small, f_cat; int font_h; } MG;

static int menu_vk_of(const char *s) {
    static const struct { const char *n; int vk; } k[] = { {"INSERT", VK_INSERT}, {"INS", VK_INSERT}, {"HOME", VK_HOME}, {"INICIO", VK_HOME},
        {"END", VK_END}, {"FIN", VK_END}, {"DELETE", VK_DELETE}, {"SUPR", VK_DELETE}, {"PAGEUP", VK_PRIOR}, {"PAGEDOWN", VK_NEXT}, {"PAUSE", VK_PAUSE} };
    while (*s == ' ') s++;
    for (size_t i = 0; i < sizeof k / sizeof k[0]; i++) if (!_strnicmp(s, k[i].n, strlen(k[i].n))) return k[i].vk;
    if ((s[0] == 'F' || s[0] == 'f') && atoi(s + 1) >= 1 && atoi(s + 1) <= 24) return VK_F1 + atoi(s + 1) - 1;
    return atoi(s) > 0 && atoi(s) < 256 ? atoi(s) : 0;
}
static void menu_path(char *out, size_t n, const char *rel) {      /* <exe folder>\..\<rel> */
    char exe[MAX_PATH] = ""; GetModuleFileNameA(NULL, exe, sizeof exe);
    char *s = strrchr(exe, '\\'); if (s) *s = 0;
    snprintf(out, n, "%s\\..\\%s", exe, rel);
}
static void menu_config(void) {
    char p[MAX_PATH + 32]; menu_path(p, sizeof p, "config\\menu.ini");
    M.vk_open = VK_INSERT; M.vk_wt = VK_F5;
    FILE *f = fopen(p, "rb");
    if (!f) {
        f = fopen(p, "wb");
        if (f) { fputs("; Menu de la edicion mejorada (se abre dentro del juego). Lo lee runtime\\menu_vk.c al arrancar.\r\n"
                       "; tecla = la tecla que abre y cierra el menu: INSERT (por defecto), HOME, END, DELETE, PAGEUP, PAGEDOWN, PAUSE,\r\n"
                       ";         F1..F24 o el numero de una tecla virtual de Windows. En el mando: L3 + R3 a la vez.\r\n"
                       "; tecla_witch_time = alterna Witch Time permanente (tiempo detenido) y Normal sin abrir el menu (por defecto F5).\r\n"
                       "tecla = INSERT\r\ntecla_witch_time = F5\r\n", f); fclose(f); }
        return;
    }
    char line[256];
    while (fgets(line, sizeof line, f)) {
        char *eq = strchr(line, '='); if (!eq || line[0] == ';') continue;
        *eq = 0; int vk = menu_vk_of(eq + 1);
        if (!_strnicmp(line, "tecla_witch_time", 16)) { if (vk) M.vk_wt = vk; }
        else if (!_strnicmp(line, "tecla", 5) && vk) M.vk_open = vk;
    }
    fclose(f);
}
static void menu_say(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); vsnprintf(M.msg, sizeof M.msg, fmt, ap); va_end(ap);
    M.msg_until = GetTickCount64() + 5000; M.dirty = 1;
    printf("menu: %s\n", M.msg); fflush(stdout);
}
static void menu_init_once(void) {
    static int done;
    if (done) return;
    done = 1;
    M.disabled = getenv("SSBU_SEQ") || getenv("SSBU_SCRIPT") || (getenv("SSBU_MENU") && !atoi(getenv("SSBU_MENU")));
    menu_config();
    if (getenv("SSBU_MENU_TEST")) { M.disabled = 0; M.open = 1; M.cat = atoi(getenv("SSBU_MENU_TEST")) % MC_COUNT; M.focus_side = 1; menu_mods_read(); }   /* TEST: open at start on page n */
    QueryPerformanceFrequency(&M.qpf);
    HMODULE g = GetModuleHandleA("game.dll");
    M.cheat = g ? (volatile int *)(void *)GetProcAddress(g, "recomps_cheat") : NULL;
    char p[MAX_PATH + 32]; menu_path(p, sizeof p, "VERSION");
    FILE *f = fopen(p, "rb"); if (f) { if (fgets(M.version, sizeof M.version, f)) M.version[strcspn(M.version, "\r\n")] = 0; fclose(f); }
    HKEY k; DWORD n = sizeof M.cpu;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", 0, KEY_READ, &k) == ERROR_SUCCESS) {
        RegQueryValueExA(k, "ProcessorNameString", NULL, NULL, (BYTE *)M.cpu, &n); RegCloseKey(k);
    }
    printf("menu: %s; tecla 0x%02x (config\\menu.ini), mando L3+R3; trucos de codigo %s\n", M.disabled ? "desactivado en esta ejecucion" : "listo",
           M.vk_open, M.cheat ? "disponibles (recomps_cheat de game.dll)" : "NO disponibles: game.dll se compilo sin scripts\\cheats_patch.py");
    fflush(stdout);
}

/* ---- the mods page: config\mods.ini [mods] and each mod's mod.ini; conflicts = the same rom: path in two switched-on mods */
static int mods_ini_on(const char *ini, const char *name) {          /* 1 on (also when not listed: the default), 0 off */
    const char *p = ini; size_t n = strlen(name);
    while (p && *p) {
        while (*p == ' ' || *p == '\t') p++;
        if (!_strnicmp(p, name, n)) { const char *q = p + n; while (*q == ' ' || *q == '\t') q++; if (*q == '=') { q++; while (*q == ' ') q++; return _strnicmp(q, "off", 3) != 0; } }
        p = strchr(p, '\n'); if (p) p++;
    }
    return 1;
}
static void mod_card(const char *dir, const char *key, char *out, size_t n) {
    char p[MAX_PATH * 2], line[400]; snprintf(p, sizeof p, "%s\\mod.ini", dir);
    out[0] = 0;
    FILE *f = fopen(p, "rb"); if (!f) return;
    while (fgets(line, sizeof line, f)) {
        size_t kl = strlen(key);
        if (!_strnicmp(line, key, kl) && (line[kl] == ' ' || line[kl] == '=')) { char *v = strchr(line, '='); if (v) { v++; while (*v == ' ') v++; snprintf(out, n, "%s", v); out[strcspn(out, "\r\n")] = 0; } }
    }
    fclose(f);
}
typedef struct { char path[96]; int mod; } RomPath;
static void romfs_collect(const char *root, const char *rel, int mod, RomPath **list, int *n, int *cap) {
    char pat[MAX_PATH * 2]; WIN32_FIND_DATAA fd;
    snprintf(pat, sizeof pat, "%s\\%s*", root, rel);
    HANDLE h = FindFirstFileA(pat, &fd); if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.cFileName[0] == '.') continue;
        char sub[200]; snprintf(sub, sizeof sub, "%s%s", rel, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) { strncat(sub, "\\", sizeof sub - strlen(sub) - 1); romfs_collect(root, sub, mod, list, n, cap); continue; }
        if (*n == *cap) { *cap = *cap ? *cap * 2 : 256; *list = realloc(*list, (size_t)*cap * sizeof **list); }
        snprintf((*list)[*n].path, sizeof (*list)[*n].path, "%s", sub); _strlwr((*list)[*n].path); (*list)[*n].mod = mod; (*n)++;
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}
static void menu_mods_read(void) {
    char ini_p[MAX_PATH + 32], dir[MAX_PATH + 32], pat[MAX_PATH + 40];
    menu_path(ini_p, sizeof ini_p, "config\\mods.ini"); menu_path(dir, sizeof dir, "mods");
    static char ini[65536]; ini[0] = 0;
    FILE *f = fopen(ini_p, "rb"); if (f) { size_t n = fread(ini, 1, sizeof ini - 1, f); ini[n] = 0; fclose(f); }
    M.nmods_lines = M.mods_on = M.mods_conflicts = 0;
    snprintf(pat, sizeof pat, "%s\\*", dir);
    WIN32_FIND_DATAA fd; HANDLE h = FindFirstFileA(pat, &fd);
    char names[64][100]; int on[64], nm = 0;
    RomPath *paths = NULL; int np = 0, cap = 0;
    char combine[64][200];
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == '.' || nm == 64) continue;
            snprintf(names[nm], sizeof names[nm], "%s", fd.cFileName);
            on[nm] = mods_ini_on(ini, fd.cFileName);
            char md[MAX_PATH * 2], ign[16]; snprintf(md, sizeof md, "%s\\%s", dir, fd.cFileName);
            mod_card(md, "combina_con", combine[nm], sizeof combine[nm]); mod_card(md, "conflictos", ign, sizeof ign);
            if (on[nm] && _stricmp(ign, "ignorar")) { char rr[MAX_PATH * 2]; snprintf(rr, sizeof rr, "%s\\romfs", md); romfs_collect(rr, "", nm, &paths, &np, &cap); }
            nm++;
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    for (int i = 0; i < np; i++) for (int j = i + 1; j < np; j++)        /* a pair that a fusion of both settles does not count */
        if (paths[i].mod != paths[j].mod && !strcmp(paths[i].path, paths[j].path)) {
            int ok = 0;
            for (int k = 0; k < np && !ok; k++) if (!strcmp(paths[k].path, paths[i].path) && strstr(combine[paths[k].mod], names[paths[i].mod]) && strstr(combine[paths[k].mod], names[paths[j].mod])) ok = 1;
            if (strstr(combine[paths[i].mod], names[paths[j].mod]) || strstr(combine[paths[j].mod], names[paths[i].mod])) ok = 1;
            if (!ok) M.mods_conflicts++;
        }
    free(paths);
    for (int i = 0; i < nm; i++) {
        if (!on[i]) continue;
        M.mods_on++;
        char md[MAX_PATH * 2], ver[40], ap[60]; snprintf(md, sizeof md, "%s\\%s", dir, names[i]);
        mod_card(md, "version", ver, sizeof ver); mod_card(md, "aplicar", ap, sizeof ap);
        if (M.nmods_lines < 48) snprintf(M.mods_line[M.nmods_lines++], sizeof M.mods_line[0], "%s|%s%s%s", names[i], ver[0] ? "v" : "", ver, !_stricmp(ap, "recompilar") ? " · código (recompilar)" : "");
    }
}

/* ---- the rows of the current page */
typedef struct { char label[96], value[160], note[300]; int kind, avail, color; } MRow;   /* kind 0 info, 1 choice; color 0 normal 1 on 2 off/no 3 warning */
static MRow g_rows[64];
static void row(int kind, int avail, int color, const char *label, const char *value, const char *note) {
    if (M.nrows >= 64) return;
    MRow *r = &g_rows[M.nrows++];
    snprintf(r->label, sizeof r->label, "%s", label); snprintf(r->value, sizeof r->value, "%s", value ? value : ""); snprintf(r->note, sizeof r->note, "%s", note ? note : "");
    r->kind = kind; r->avail = avail; r->color = color;
}
static void rows_build(void) {
    char v[160];
    M.nrows = 0;
    const char *nocode = "Este game.dll se compiló sin el parche de trucos: vuelve a compilar (Mods... > Apply, o Build B2recomp.bat).";
    switch (M.cat) {
    case MC_PERSONAJE:
        row(1, 1, M.character ? 1 : 0, "Personaje jugable", k_chars[M.character].name, k_chars[M.character].warn);
        row(0, 1, 0, "Cuándo cambiarlo", "Selección de capítulos", "Elige aquí y después entra en la selección de personaje del juego. Durante un combate el cambio se bloquea (puede colgar el juego).");
        row(0, 1, 0, "Bayonetta y Jeanne", "En el menú del juego", "No necesitan truco: el juego ya permite elegirlas.");
        row(0, 1, 0, "Origen", "pearl23 · Misc Bayonetta 2 Cheats 1.1", "Códigos para la versión 1.2 (build F19B2E01E1794F93), aplicados como escrituras nativas. Al volver a Normal se restauran los valores originales.");
        break;
    case MC_TRUCOS:
        if (M.cheat) {
            row(1, 1, M.cheat[1] ? 1 : 2, "Salud infinita", M.cheat[1] ? "Activado" : "Desactivado", "Salta la llamada de daño del jugador (0x1D5CC). Código de xtatu (v1.0) en una zona idéntica en la 1.2.");
            row(1, 1, M.cheat[2] ? 1 : 2, "Invencibilidad (1P)", M.cheat[2] ? "Activado" : "Desactivado", "La función 0x20C14 vuelve al instante, como el código «1p invincible». Zona idéntica en la 1.2.");
            snprintf(v, sizeof v, "%s", k_wt[M.cheat[0] & 3]);
            row(1, 1, M.cheat[0] ? 1 : 2, "Witch Time", v, "Código de Gamerjin para la 1.2 (0x1C974). Permanente = Witch Time sin fin (120 s que se renuevan).");
            snprintf(v, sizeof v, "Tecla %s%d", M.vk_wt >= VK_F1 && M.vk_wt <= VK_F24 ? "F" : "#", M.vk_wt >= VK_F1 && M.vk_wt <= VK_F24 ? M.vk_wt - VK_F1 + 1 : M.vk_wt);
            row(0, 1, 0, "Witch Time manual", v, "Alterna entre Normal y Permanente (tiempo detenido) sin abrir el menú. Se cambia en config\\menu.ini.");
        } else {
            row(0, 0, 2, "Salud infinita", "No disponible", nocode);
            row(0, 0, 2, "Invencibilidad (1P)", "No disponible", nocode);
            row(0, 0, 2, "Witch Time", "No disponible", nocode);
        }
        row(0, 0, 2, "Magia infinita", "No disponible", "No hay un código verificado para la versión 1.2.");
        row(0, 0, 3, "Daño multiplicado", "Usa el mod B04", "Buffed Weapons (perfil Combate expandido) sube el daño de armas y Wicked Weaves.");
        row(0, 0, 2, "Golpear a cualquier distancia", "No disponible", "El código existe solo para la v1.0, en una zona del programa que cambió en la 1.2.");
        break;
    case MC_ARMAS:
        row(1, 1, M.weapon ? 1 : 0, "Arma de la ranura A", k_weapons[M.weapon].name, M.character == 1 || M.character == 2 ? "Balder y Rodin fijan su propia arma: este ajuste no se aplica con ellos." : k_weapons[M.weapon].warn);
        row(0, 1, 0, "Cuándo cambiarla", "Selección de capítulos", "Igual que el personaje: el cambio se bloquea durante un combate.");
        row(0, 1, 3, "βTartarus", "Pendiente de verificar", "Su ficha usa el ID 9 de la tabla de PC; en la 1.2 el ID 9 es la Gatling según pearl23. Prueba la Gatling con el mod B06.");
        row(0, 1, 0, "Colour My World", "Mod B02 (perfil)", "Sustituye los movimientos de Love is Blue. Se activa en el launcher (Mods...).");
        break;
    case MC_CAMARA:
        row(0, 0, 2, "Cámara libre", "No disponible", "La cámara del juego aún no está localizada en la 1.2: no se activa sin verificarla (riesgo de bloqueos).");
        row(0, 0, 2, "Distancia y campo de visión", "No disponible", "Mismo motivo.");
        row(0, 0, 2, "Ocultar interfaz", "No disponible", "Mismo motivo.");
        break;
    case MC_GRAFICOS: {
        const GfxOptions *g = gfx_options();
        static const char *const wm[] = {"Ventana", "Sin bordes", "Pantalla completa"}, *const pm[] = {"Reloj propio 60 Hz", "Vsync (FIFO)", "Sin vsync", "Mailbox", "Pantalla", "Automático"};
        row(0, 1, 0, "Renderizador", "Vulkan", NULL);
        snprintf(v, sizeof v, "%s · %ux%u", wm[g->window_mode % 3], V.extent.width, V.extent.height); row(0, 1, 0, "Ventana", v, NULL);
        snprintf(v, sizeof v, "x%.2f (%dx%d)", g->scale, (int)(1280 * g->scale), (int)(720 * g->scale)); row(0, 1, 0, "Resolución interna", v, NULL);
        snprintf(v, sizeof v, "%s", pm[g->present >= 0 && g->present < 6 ? g->present : 0]); row(0, 1, 0, "Sincronización", v, NULL);
        snprintf(v, sizeof v, "%d fps", g->frame_rate ? g->frame_rate : 60); row(0, 1, g->frame_rate > 60 ? 3 : 1, "Objetivo", v, g->frame_rate > 60 ? "Por encima de 60 algunos temporizadores del juego cambian." : "La velocidad original del juego.");
        snprintf(v, sizeof v, "%s", g->aniso ? (g->aniso == 16 ? "16x" : g->aniso == 8 ? "8x" : g->aniso == 4 ? "4x" : "2x") : "del juego"); row(0, 1, 0, "Filtrado anisotrópico", v, NULL);
        snprintf(v, sizeof v, "%s · Vulkan %u.%u", V.props.deviceName, VK_API_VERSION_MAJOR(V.props.apiVersion), VK_API_VERSION_MINOR(V.props.apiVersion)); row(0, 1, 0, "GPU", v, NULL);
        row(0, 1, 0, "Cambiar ajustes", "En el launcher", "Los ajustes gráficos se cambian en B2recomp.exe y se aplican al reiniciar el juego.");
        break; }
    case MC_MODS:
        snprintf(v, sizeof v, "%d activados", M.mods_on); row(0, 1, 0, "Mods", v, "Leídos al arrancar el juego. Para cambiarlos: launcher > Mods... (perfiles incluidos).");
        snprintf(v, sizeof v, "%d", M.mods_conflicts); row(0, 1, M.mods_conflicts ? 3 : 1, "Conflictos (misma ruta)", M.mods_conflicts ? v : "Ninguno", M.mods_conflicts ? "Gana el mod cuyo nombre va después en orden alfabético." : NULL);
        for (int i = 0; i < M.nmods_lines; i++) {
            char n[160]; snprintf(n, sizeof n, "%s", M.mods_line[i]); char *bar = strchr(n, '|'); if (bar) *bar++ = 0;
            row(0, 1, 1, n, bar ? bar : "", "Cambiar mods de texturas o modelos: reiniciar el juego. Mods de código: recompilar.");
        }
        break;
    case MC_DIAG:
        snprintf(v, sizeof v, "%.1f fps", M.fps); row(0, 1, M.fps >= 58.5 ? 1 : 3, "FPS", v, "Media del último segundo (presentaciones a la ventana).");
        snprintf(v, sizeof v, "media %.2f ms · máx %.2f ms", M.ft_avg, M.ft_max); row(0, 1, M.ft_max > 20 ? 3 : 0, "Tiempo de fotograma", v, NULL);
        row(0, 1, 0, "CPU", M.cpu[0] ? M.cpu : "?", NULL);
        row(0, 1, 0, "GPU", V.props.deviceName, NULL);
        snprintf(v, sizeof v, "%d activados, %d conflictos", M.mods_on, M.mods_conflicts); row(0, 1, 0, "Mods", v, NULL);
        row(0, 1, M.cheat ? 1 : 2, "Trucos de código", M.cheat ? "Disponibles" : "No compilados", M.cheat ? "scripts\\cheats_patch.py está en este game.dll." : nocode);
        snprintf(v, sizeof v, "Personaje: %s · Arma: %s", k_chars[M.character].name, k_weapons[M.weapon].name); row(0, 1, M.character || M.weapon ? 1 : 0, "Trucos de datos", v, NULL);
        snprintf(v, sizeof v, "%s (edición mejorada)", M.version[0] ? M.version : "?"); row(0, 1, 0, "Versión de B2recomp", v, NULL);
        row(0, 1, g_in_fight ? 3 : 0, "Estado", g_in_fight ? "En combate" : "Fuera de combate", "Lo detecta el renderizador por la escena dibujada.");
        break;
    }
    if (M.row >= M.nrows) M.row = M.nrows - 1;
    if (M.row < 0) M.row = 0;
}

/* ---- changes */
static void poke_apply(const MChoice *c, uint8_t *saved, int *have) {
    for (int i = 0; i < c->n; i++) {
        volatile uint8_t *p = (volatile uint8_t *)(uintptr_t)(GUEST_BASE + c->p[i].off);
        if (!*have) saved[i] = *p;
        *p = c->p[i].val;
    }
    *have = 1;
}
static void poke_restore(const MChoice *c, uint8_t *saved, int *have) {
    if (!*have) return;
    for (int i = 0; i < c->n; i++) *(volatile uint8_t *)(uintptr_t)(GUEST_BASE + c->p[i].off) = saved[i];
    *have = 0;
}
static void menu_cheats_frame(void) {          /* every present: the data cheats, as Atmosphere's cheat VM rewrites them */
    static LONG64 next_hits; static int last[3];       /* how often the game ran the three hooked places (cheats_patch.py counters 13..15) */
    if (M.cheat && (LONG64)GetTickCount64() >= next_hits) { next_hits = (LONG64)GetTickCount64() + 5000;
        if (M.cheat[13] != last[0] || M.cheat[14] != last[1] || M.cheat[15] != last[2]) { printf("menu: trucos: el juego paso por 0x1c974 %d, 0x1d5cc %d, 0x20c14 %d veces\n", M.cheat[13], M.cheat[14], M.cheat[15]); fflush(stdout); last[0] = M.cheat[13]; last[1] = M.cheat[14]; last[2] = M.cheat[15]; } }
    if (M.character) poke_apply(&k_chars[M.character], M.saved_char, &M.have_saved_char);
    if (M.weapon && M.character != 1 && M.character != 2) poke_apply(&k_weapons[M.weapon], M.saved_weap, &M.have_saved_weap);
}
static void menu_change(int delta) {
    if (M.row < 0 || M.row >= M.nrows || !g_rows[M.row].kind) return;
    if (M.cat == MC_PERSONAJE || M.cat == MC_ARMAS) {
        if (g_in_fight) { menu_say("Bloqueado durante un combate: cámbialo en la selección de capítulos."); return; }
        if (M.cat == MC_PERSONAJE) {
            int n = (M.character + delta + NCHARS) % NCHARS;
            if (M.character) poke_restore(&k_chars[M.character], M.saved_char, &M.have_saved_char);
            M.character = n;
            menu_say("Personaje: %s.%s", k_chars[n].name, n ? " Entra en la selección de personaje del juego." : " Valores originales restaurados.");
        } else {
            int n = (M.weapon + delta + NWEAP) % NWEAP;
            if (M.weapon) poke_restore(&k_weapons[M.weapon], M.saved_weap, &M.have_saved_weap);
            M.weapon = n;
            menu_say("Arma de la ranura A: %s.", k_weapons[n].name);
        }
    } else if (M.cat == MC_TRUCOS && M.cheat) {
        if (M.row == 0) M.cheat[1] = !M.cheat[1];
        else if (M.row == 1) M.cheat[2] = !M.cheat[2];
        else if (M.row == 2) M.cheat[0] = (M.cheat[0] + delta + 4) % 4;
        menu_say("%s: %s", g_rows[M.row].label, M.row == 2 ? k_wt[M.cheat[0]] : (M.row == 0 ? M.cheat[1] : M.cheat[2]) ? "activado" : "desactivado");
    }
    M.dirty = 1;
}

/* ---- input */
static int key_edge(int vk) {
    static uint8_t was[256];
    int down = (GetAsyncKeyState(vk) & 0x8000) != 0, e = down && !was[vk & 0xff];
    was[vk & 0xff] = (uint8_t)down;
    return e;
}
static const char *g_menu_why = "";
static void menu_set_open(int on) {
    if (M.open == on) return;
    M.open = on; M.dirty = 1;
    if (on) { menu_mods_read(); M.focus_side = 0; }
    printf("menu: %s (%s)\n", on ? "abierto" : "cerrado", g_menu_why); fflush(stdout);
}
int menu_captures_input(void) { return M.open; }
void menu_pad(uint64_t b, int lx, int ly) {         /* game thread, at every pad poll of the keyboard's player */
    InterlockedExchange(&M.pad_buttons, (LONG)(b & 0xffff)); InterlockedExchange(&M.pad_x, lx); InterlockedExchange(&M.pad_y, ly);
    InterlockedExchange(&M.pad_seen, 1);
}
int menu_wndmsg(HWND h, UINT m, WPARAM w, LPARAM l) {
    (void)h; (void)w;
    if (!M.open) return 0;
    if (m == WM_MOUSEMOVE || m == WM_LBUTTONDOWN) { M.mouse_x = (short)LOWORD(l); M.mouse_y = (short)HIWORD(l); if (m == WM_LBUTTONDOWN) M.mouse_click = 1; return m == WM_LBUTTONDOWN; }
    if (m == WM_MOUSEWHEEL) { M.mouse_wheel += GET_WHEEL_DELTA_WPARAM(w) > 0 ? -1 : 1; return 1; }
    return 0;
}
/* panel geometry in window pixels (also used for the mouse) */
static void menu_rect(int *x0, int *y0, int *w, int *h) {
    int W = (int)V.extent.width, H = (int)V.extent.height;
    *h = H * 74 / 100; *w = *h * 150 / 100; if (*w > W * 92 / 100) { *w = W * 92 / 100; *h = *w * 100 / 150; }
    *x0 = (W - *w) / 2; *y0 = (H - *h) / 2;
}
static void menu_input(void) {
    if (M.disabled) return;
    int front = GetForegroundWindow() == V.hwnd;
    static int pad_prev;
    int pb = (int)InterlockedCompareExchange(&M.pad_buttons, 0, 0), py = (int)InterlockedCompareExchange(&M.pad_y, 0, 0), px = (int)InterlockedCompareExchange(&M.pad_x, 0, 0);
    /* stick as d-pad bits: up 13, down 15, left 12, right 14 */
    if (py > 20000) pb |= 1 << 13; if (py < -20000) pb |= 1 << 15; if (px < -20000) pb |= 1 << 12; if (px > 20000) pb |= 1 << 14;
    int pe = pb & ~pad_prev; pad_prev = pb;
    int combo = (pb & 0x30) == 0x30 && (pe & 0x30);                  /* L3 + R3 */
    if (front && key_edge(M.vk_open)) { g_menu_why = "tecla"; menu_set_open(!M.open); }
    else if (combo) { g_menu_why = "L3+R3"; menu_set_open(!M.open); }
    if (front && !M.open && M.cheat && key_edge(M.vk_wt)) { M.cheat[0] = M.cheat[0] ? 0 : 2; menu_say("Witch Time: %s", k_wt[M.cheat[0]]); }
    if (!M.open) { key_edge(VK_ESCAPE); key_edge(VK_UP); key_edge(VK_DOWN); key_edge(VK_LEFT); key_edge(VK_RIGHT); key_edge(VK_RETURN); key_edge(VK_TAB); return; }
    int up = (front && key_edge(VK_UP)) || (pe & 1 << 13), down = (front && key_edge(VK_DOWN)) || (pe & 1 << 15);
    int left = (front && key_edge(VK_LEFT)) || (pe & 1 << 12), right = (front && key_edge(VK_RIGHT)) || (pe & 1 << 14);
    int ok = (front && key_edge(VK_RETURN)) || (pe & 1 << 0), back = (front && key_edge(VK_ESCAPE)) || (pe & 1 << 1);
    int tab = (front && key_edge(VK_TAB)) || (pe & 1 << 7), stab = (pe & 1 << 6);   /* R / L: next / previous page */
    if (back) { if (M.focus_side) M.focus_side = 0; else { g_menu_why = back & 2 ? "B" : "Esc"; menu_set_open(0); } return; }
    if (tab) { M.cat = (M.cat + 1) % MC_COUNT; M.row = 0; M.dirty = 1; }
    if (stab) { M.cat = (M.cat + MC_COUNT - 1) % MC_COUNT; M.row = 0; M.dirty = 1; }
    if (M.focus_side == 0) {                               /* the page list on the left */
        if (up) { M.cat = (M.cat + MC_COUNT - 1) % MC_COUNT; M.row = 0; M.dirty = 1; }
        if (down) { M.cat = (M.cat + 1) % MC_COUNT; M.row = 0; M.dirty = 1; }
        if (right || ok) { M.focus_side = 1; M.dirty = 1; }
    } else {
        if (up && M.row > 0) { M.row--; M.dirty = 1; }
        if (down && M.row < M.nrows - 1) { M.row++; M.dirty = 1; }
        if (left && (M.row >= M.nrows || !g_rows[M.row].kind)) { M.focus_side = 0; M.dirty = 1; }
        else if (left) menu_change(-1);
        if (right || ok) menu_change(1);
    }
    if (M.mouse_wheel) { int d = M.mouse_wheel; M.mouse_wheel = 0; M.focus_side = 1; M.row += d; M.dirty = 1; }
    if (M.mouse_click) {                                   /* the same layout as menu_paint: side list 26 %, rows below the title */
        M.mouse_click = 0;
        int x0, y0, w, h; menu_rect(&x0, &y0, &w, &h);
        int x = M.mouse_x - x0, y = M.mouse_y - y0, top = h * 15 / 100, rh = h * 8 / 100, side = w * 26 / 100;
        if (x >= 0 && y >= 0 && x < w && y < h) {
            if (x < side) { int c = (y - top) / (h * 7 / 100); if (y >= top && c >= 0 && c < MC_COUNT) { M.cat = c; M.row = 0; M.focus_side = 0; M.dirty = 1; } }
            else { int r = (y - top) / rh; if (y >= top && r >= 0 && r < M.nrows) { M.focus_side = 1; if (M.row == r) menu_change(1); M.row = r; M.dirty = 1; } }
        } else { g_menu_why = "clic fuera"; menu_set_open(0); }
    }
}

/* ---- painting (GDI) */
static HFONT menu_font(int px, int weight) {
    return CreateFontW(-px, 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, L"Segoe UI");
}
static void gtext(int x, int y, int w, int h, HFONT f, COLORREF c, const char *s, UINT fmt) {
    wchar_t ws[400]; if (!MultiByteToWideChar(CP_UTF8, 0, s, -1, ws, 400)) return;
    RECT r = { x, y, x + w, y + h };
    SelectObject(MG.dc, f); SetTextColor(MG.dc, c);
    DrawTextW(MG.dc, ws, -1, &r, fmt | DT_NOPREFIX);
}
static void gfill(int x, int y, int w, int h, COLORREF c) {
    HBRUSH b = CreateSolidBrush(c); RECT r = { x, y, x + w, y + h }; FillRect(MG.dc, &r, b); DeleteObject(b);
}
#define C_BG RGB(14, 16, 26)
#define C_SIDE RGB(19, 22, 35)
#define C_SEL RGB(34, 38, 58)
#define C_GOLD RGB(214, 176, 92)
#define C_TEXT RGB(232, 235, 244)
#define C_DIM RGB(140, 148, 170)
#define C_ON RGB(111, 207, 151)
#define C_OFF RGB(235, 108, 108)
#define C_WARN RGB(242, 190, 90)
static int menu_paint(int w, int h) {
    if (MG.w != w || MG.h != h) {
        if (MG.bmp) { DeleteObject(MG.bmp); DeleteObject(MG.f_title); DeleteObject(MG.f_body); DeleteObject(MG.f_small); DeleteObject(MG.f_cat); }
        if (!MG.dc) MG.dc = CreateCompatibleDC(NULL);
        BITMAPINFO bi; memset(&bi, 0, sizeof bi);
        bi.bmiHeader.biSize = sizeof bi.bmiHeader; bi.bmiHeader.biWidth = w; bi.bmiHeader.biHeight = -h; bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32;
        MG.bmp = CreateDIBSection(MG.dc, &bi, DIB_RGB_COLORS, (void **)&MG.px, NULL, 0);
        if (!MG.bmp) return 0;
        SelectObject(MG.dc, MG.bmp); SetBkMode(MG.dc, TRANSPARENT);
        MG.f_title = menu_font(h * 40 / 1000, FW_SEMIBOLD); MG.f_cat = menu_font(h * 30 / 1000, FW_NORMAL);
        MG.f_body = menu_font(h * 28 / 1000, FW_NORMAL); MG.f_small = menu_font(h * 21 / 1000, FW_NORMAL);
        MG.w = w; MG.h = h;
    }
    rows_build();
    int side = w * 26 / 100, top = h * 15 / 100, pad = w * 2 / 100, rh = h * 8 / 100;
    gfill(0, 0, w, h, C_BG);
    gfill(0, 0, side, h, C_SIDE);
    gtext(pad, h * 3 / 100, w - 2 * pad, h * 6 / 100, MG.f_title, C_GOLD, "B2RECOMP  ·  EDICIÓN MEJORADA", DT_LEFT | DT_SINGLELINE | DT_VCENTER);
    gfill(pad, h * 10 / 100, w * 6 / 100, h * 4 / 1000 + 1, C_GOLD);
    for (int c = 0; c < MC_COUNT; c++) {                  /* page list */
        int y = top + c * (h * 7 / 100), sel = c == M.cat;
        if (sel) { gfill(0, y, side, h * 7 / 100, M.focus_side ? C_SIDE : C_SEL); gfill(0, y, w * 5 / 1000 + 2, h * 7 / 100, C_GOLD); }
        gtext(pad, y, side - pad, h * 7 / 100, MG.f_cat, sel ? C_TEXT : C_DIM, k_mcat[c], DT_LEFT | DT_SINGLELINE | DT_VCENTER);
    }
    int cx = side + pad, cw = w - side - 2 * pad;
    gtext(cx, h * 3 / 100, cw, h * 6 / 100, MG.f_cat, C_DIM, k_mcat[M.cat], DT_RIGHT | DT_SINGLELINE | DT_VCENTER);
    int first = M.row >= 8 ? M.row - 7 : 0;               /* 8 rows on screen, scrolled to keep the selection visible */
    for (int i = first; i < M.nrows && i < first + 8; i++) {
        MRow *r = &g_rows[i];
        int y = top + (i - first) * rh, sel = i == M.row && M.focus_side;
        if (sel) { gfill(side, y, w - side, rh, C_SEL); gfill(side, y, w * 5 / 1000 + 2, rh, C_GOLD); }
        COLORREF vc = r->color == 1 ? C_ON : r->color == 2 ? C_OFF : r->color == 3 ? C_WARN : C_TEXT;
        gtext(cx, y + rh * 8 / 100, cw * 45 / 100, rh * 50 / 100, MG.f_body, r->avail ? C_TEXT : C_DIM, r->label, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
        char val[180]; snprintf(val, sizeof val, "%s%s%s", r->kind && sel ? "‹  " : "", r->value, r->kind && sel ? "  ›" : "");
        gtext(cx + cw * 45 / 100, y + rh * 8 / 100, cw * 55 / 100, rh * 50 / 100, MG.f_body, vc, val, DT_RIGHT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
        if (r->note[0]) gtext(cx, y + rh * 56 / 100, cw, rh * 42 / 100, MG.f_small, C_DIM, r->note, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
    }
    int fy = h - h * 8 / 100;
    gfill(side, fy - 1, w - side, 1, RGB(40, 44, 64));
    const char *hint = M.focus_side ? "↑↓ elegir   ←→ / Intro cambiar   Esc volver   Tab página   Insert cerrar"
                                    : "↑↓ página   → / Intro entrar   Esc / Insert cerrar   Mando: L3+R3 · A/B · L/R";
    if (M.msg_until > (LONG64)GetTickCount64()) gtext(cx, fy, cw, h * 8 / 100, MG.f_small, C_WARN, M.msg, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
    else gtext(cx, fy, cw, h * 8 / 100, MG.f_small, C_DIM, hint, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
    GdiFlush();
    /* alpha: opaque inside a rounded rectangle (antialiased corners), 0 outside; GDI leaves the alpha byte at 0 */
    float rad = (float)h * 0.03f;
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
        float dx = x + 0.5f < rad ? rad - (x + 0.5f) : x + 0.5f > w - rad ? x + 0.5f - (w - rad) : 0;
        float dy = y + 0.5f < rad ? rad - (y + 0.5f) : y + 0.5f > h - rad ? y + 0.5f - (h - rad) : 0;
        float a = dx > 0 && dy > 0 ? rad + 0.5f - sqrtf(dx * dx + dy * dy) : 1.0f;
        a = a < 0 ? 0 : a > 1 ? 1 : a;
        uint32_t p = MG.px[(size_t)y * w + x];
        MG.px[(size_t)y * w + x] = (p & 0x00ffffffu) | (uint32_t)(a * 255.0f + 0.5f) << 24;
    }
    return 1;
}

/* ---- Vulkan */
static void menu_vk_forget(void) {                         /* swapchain_destroy: the views of the old images go */
    for (int i = 0; i < 16; i++) if (MV.sc_view[i]) { vkDestroyImageView(V.device, MV.sc_view[i], NULL); MV.sc_view[i] = NULL; MV.sc_img[i] = NULL; }
}
static VkShaderModule menu_shader(int stage, const char *src) {
    if (!g_glsl_to_spirv) {
        HMODULE m = LoadLibraryA("ssbu_glslang.dll");
        g_glsl_to_spirv = m ? (void *)GetProcAddress(m, "ssbu_glsl_to_spirv") : NULL; g_spirv_free = m ? (void *)GetProcAddress(m, "ssbu_spirv_free") : NULL;
    }
    uint32_t *words = NULL; size_t count = 0; char log[1024];
    if (!g_glsl_to_spirv || !g_glsl_to_spirv(stage, src, &words, &count, log, sizeof log)) { printf("menu: shader %d: %s\n", stage, g_glsl_to_spirv ? log : "ssbu_glslang.dll no disponible"); return NULL; }
    VkShaderModuleCreateInfo ci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = count * 4; ci.pCode = words;
    VkShaderModule sm = NULL;
    if (vkCreateShaderModule(V.device, &ci, NULL, &sm) != VK_SUCCESS) sm = NULL;
    g_spirv_free(words);
    return sm;
}
static int menu_vk_make(void) {
    if (MV.pipe) return 1;
    if (M.made_fail) return 0;
    static const char *vs = "#version 460\nvoid main(){ vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2); gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0); }\n";
    static const char *fs = "#version 460\nlayout(set = 0, binding = 0) uniform sampler2D tex;\n"
        "layout(push_constant) uniform P { vec4 rect; vec4 k; } pc;\nlayout(location = 0) out vec4 o;\n"
        "void main(){ vec2 f = gl_FragCoord.xy; vec4 t = vec4(0.0);\n"
        "  if (f.x >= pc.rect.x && f.y >= pc.rect.y && f.x < pc.rect.z && f.y < pc.rect.w) t = texture(tex, (f - pc.rect.xy) / (pc.rect.zw - pc.rect.xy));\n"
        "  float ta = t.a * pc.k.z * pc.k.x, dim = pc.k.y * pc.k.x;\n"
        "  o = vec4(t.rgb * ta, 1.0 - (1.0 - ta) * (1.0 - dim)); }\n";
    VkShaderModule v = menu_shader(0, vs), f = menu_shader(1, fs);
    if (!v || !f) { M.made_fail = 1; return 0; }
    VkDescriptorSetLayoutBinding b = {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL};
    VkDescriptorSetLayoutCreateInfo dl = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO}; dl.bindingCount = 1; dl.pBindings = &b;
    VKC(vkCreateDescriptorSetLayout(V.device, &dl, NULL, &MV.dsl));
    VkDescriptorPoolSize ps = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1};
    VkDescriptorPoolCreateInfo dp = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO}; dp.maxSets = 1; dp.poolSizeCount = 1; dp.pPoolSizes = &ps;
    VKC(vkCreateDescriptorPool(V.device, &dp, NULL, &MV.dp));
    VkDescriptorSetAllocateInfo da = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO}; da.descriptorPool = MV.dp; da.descriptorSetCount = 1; da.pSetLayouts = &MV.dsl;
    VKC(vkAllocateDescriptorSets(V.device, &da, &MV.ds));
    VkPushConstantRange pr = {VK_SHADER_STAGE_FRAGMENT_BIT, 0, 32};
    VkPipelineLayoutCreateInfo lc = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO}; lc.setLayoutCount = 1; lc.pSetLayouts = &MV.dsl; lc.pushConstantRangeCount = 1; lc.pPushConstantRanges = &pr;
    VKC(vkCreatePipelineLayout(V.device, &lc, NULL, &MV.pl));
    VkSamplerCreateInfo sc = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sc.magFilter = sc.minFilter = VK_FILTER_LINEAR; sc.addressModeU = sc.addressModeV = sc.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE; sc.maxLod = 0.25f;
    VKC(vkCreateSampler(V.device, &sc, NULL, &MV.smp));
    VkPipelineShaderStageCreateInfo st[2] = {{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}, {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}};
    st[0].stage = VK_SHADER_STAGE_VERTEX_BIT; st[0].module = v; st[0].pName = "main";
    st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; st[1].module = f; st[1].pName = "main";
    VkPipelineVertexInputStateCreateInfo vi = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO}; ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO}; vp.viewportCount = vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO}; rs.cullMode = VK_CULL_MODE_NONE; rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO}; ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState ba; memset(&ba, 0, sizeof ba);
    ba.blendEnable = VK_TRUE; ba.srcColorBlendFactor = ba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE; ba.dstColorBlendFactor = ba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    ba.colorBlendOp = ba.alphaBlendOp = VK_BLEND_OP_ADD; ba.colorWriteMask = 0xf;
    VkPipelineColorBlendStateCreateInfo cb = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO}; cb.attachmentCount = 1; cb.pAttachments = &ba;
    VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo ds = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO}; ds.dynamicStateCount = 2; ds.pDynamicStates = dyn;
    VkFormat fmt = V.format.format;
    VkPipelineRenderingCreateInfo rinfo = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO}; rinfo.colorAttachmentCount = 1; rinfo.pColorAttachmentFormats = &fmt;
    VkGraphicsPipelineCreateInfo gi = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    gi.pNext = &rinfo; gi.stageCount = 2; gi.pStages = st; gi.pVertexInputState = &vi; gi.pInputAssemblyState = &ia; gi.pViewportState = &vp;
    gi.pRasterizationState = &rs; gi.pMultisampleState = &ms; gi.pColorBlendState = &cb; gi.pDynamicState = &ds; gi.layout = MV.pl;
    VkResult r = vkCreateGraphicsPipelines(V.device, NULL, 1, &gi, NULL, &MV.pipe);
    vkDestroyShaderModule(V.device, v, NULL); vkDestroyShaderModule(V.device, f, NULL);
    if (r != VK_SUCCESS) { printf("menu: vkCreateGraphicsPipelines: %s\n", vk_result_name(r)); M.made_fail = 1; return 0; }
    printf("menu: pipeline listo (formato de la ventana %d)\n", (int)fmt); fflush(stdout);
    return 1;
}
static int menu_vk_texture(int w, int h) {                 /* the sampled image and 3 staging buffers for a w x h panel */
    if (MV.img && MV.w == w && MV.h == h) return 1;
    if (MV.img) {                                          /* a new size (another window size): the GPU may still read the old one */
        wait_last_submit(); vkDeviceWaitIdle(V.device);
        vkDestroyImageView(V.device, MV.view, NULL); vkDestroyImage(V.device, MV.img, NULL); vmem_free(MV.mem);
        for (int i = 0; i < 3; i++) { vkDestroyBuffer(V.device, MV.stage[i], NULL); vmem_free(MV.stage_mem[i]); }
        MV.img = NULL;
    }
    VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D; ci.format = VK_FORMAT_B8G8R8A8_UNORM; ci.extent.width = (uint32_t)w; ci.extent.height = (uint32_t)h; ci.extent.depth = 1;
    ci.mipLevels = 1; ci.arrayLayers = 1; ci.samples = VK_SAMPLE_COUNT_1_BIT; ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT; ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VKC(vkCreateImage(V.device, &ci, NULL, &MV.img));
    VkMemoryRequirements rq; vkGetImageMemoryRequirements(V.device, MV.img, &rq);
    MV.mem = vmem_alloc(rq, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0);
    VKC(vkBindImageMemory(V.device, MV.img, MV.mem.mem, MV.mem.off));
    VkImageViewCreateInfo vc = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vc.image = MV.img; vc.viewType = VK_IMAGE_VIEW_TYPE_2D; vc.format = ci.format; vc.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; vc.subresourceRange.levelCount = 1; vc.subresourceRange.layerCount = 1;
    VKC(vkCreateImageView(V.device, &vc, NULL, &MV.view));
    for (int i = 0; i < 3; i++) MV.stage[i] = vbuf_new((uint64_t)w * h * 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &MV.stage_mem[i]);
    VkDescriptorImageInfo ii = {MV.smp, MV.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet wd = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}; wd.dstSet = MV.ds; wd.descriptorCount = 1; wd.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; wd.pImageInfo = &ii;
    vkUpdateDescriptorSets(V.device, 1, &wd, 0, NULL);
    MV.w = w; MV.h = h; MV.layout_ready = 0;
    return 1;
}
static VkImageView menu_sc_view(uint32_t index) {
    if (index >= 16) return NULL;
    if (MV.sc_img[index] != V.images[index] && MV.sc_view[index]) { vkDestroyImageView(V.device, MV.sc_view[index], NULL); MV.sc_view[index] = NULL; }
    if (!MV.sc_view[index]) {
        VkImageViewCreateInfo vc = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vc.image = V.images[index]; vc.viewType = VK_IMAGE_VIEW_TYPE_2D; vc.format = V.format.format;
        vc.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; vc.subresourceRange.levelCount = 1; vc.subresourceRange.layerCount = 1;
        if (vkCreateImageView(V.device, &vc, NULL, &MV.sc_view[index]) != VK_SUCCESS) return NULL;
        MV.sc_img[index] = V.images[index];
    }
    return MV.sc_view[index];
}
static void menu_stats(void) {
    LARGE_INTEGER now; QueryPerformanceCounter(&now);
    if (M.t_last.QuadPart) {
        double ms = (double)(now.QuadPart - M.t_last.QuadPart) * 1000.0 / (double)M.qpf.QuadPart;
        M.ft[M.fti] = ms; M.fti = (M.fti + 1) % 120; if (M.ftn < 120) M.ftn++;
    }
    M.t_last = now;
    LONG64 t = (LONG64)GetTickCount64();
    if (t >= M.next_stats && M.ftn) {
        double sum = 0, mx = 0, acc = 0; int n = 0;
        for (int i = 0; i < M.ftn && acc < 1000.0; i++) { double v = M.ft[(M.fti - 1 - i + 240) % 120]; sum += v; acc += v; if (v > mx) mx = v; n++; }
        M.ft_avg = sum / n; M.ft_max = mx; M.fps = M.ft_avg > 0 ? 1000.0 / M.ft_avg : 0;
        M.next_stats = t + 250;
        if (M.open && (M.cat == MC_DIAG)) M.dirty = 1;
    }
}
/* Called by vk_present2 with the swapchain image in TRANSFER_DST_OPTIMAL, outside a render pass; leaves it so. */
static void menu_vk_draw(uint32_t index) {
    menu_init_once();
    menu_stats();
    menu_cheats_frame();
    if (M.disabled) return;
    menu_input();
    float target = M.open ? 1.0f : 0.0f;
    if (M.fade != target) { float step = (float)(M.ft_avg > 0 ? M.ft_avg : 16.7) / 120.0f; M.fade += M.fade < target ? step : -step; if (M.fade < 0) M.fade = 0; if (M.fade > 1) M.fade = 1; }
    if (M.fade <= 0.0f) return;
    if (!menu_vk_make()) return;
    int x0, y0, w, h; menu_rect(&x0, &y0, &w, &h);
    if (w < 64 || h < 64 || !menu_vk_texture(w, h)) return;
    VkImageView view = menu_sc_view(index);
    if (!view) return;
    if (M.dirty || !MV.layout_ready) {                     /* repaint and upload */
        if (!menu_paint(w, h)) return;
        MV.si = (MV.si + 1) % 3;
        memcpy(MV.stage_mem[MV.si].map, MG.px, (size_t)w * h * 4);
        image_barrier(MV.img, VK_IMAGE_ASPECT_COLOR_BIT, MV.layout_ready ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkBufferImageCopy bc; memset(&bc, 0, sizeof bc);
        bc.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; bc.imageSubresource.layerCount = 1; bc.imageExtent.width = (uint32_t)w; bc.imageExtent.height = (uint32_t)h; bc.imageExtent.depth = 1;
        vkCmdCopyBufferToImage(R.cmd, MV.stage[MV.si], MV.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bc);
        image_barrier(MV.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        MV.layout_ready = 1; M.dirty = 0;
    }
    image_barrier(V.images[index], VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    VkRenderingAttachmentInfo ca = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    ca.imageView = view; ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL; ca.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo ri = {VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea.extent = V.extent; ri.layerCount = 1; ri.colorAttachmentCount = 1; ri.pColorAttachments = &ca;
    vkCmdBeginRendering(R.cmd, &ri);
    VkViewport vpt = {0, 0, (float)V.extent.width, (float)V.extent.height, 0, 1};
    VkRect2D sc = {{0, 0}, V.extent};
    vkCmdSetViewport(R.cmd, 0, 1, &vpt); vkCmdSetScissor(R.cmd, 0, 1, &sc);
    vkCmdBindPipeline(R.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, MV.pipe);
    vkCmdBindDescriptorSets(R.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, MV.pl, 0, 1, &MV.ds, 0, NULL);
    float e = M.fade * M.fade * (3 - 2 * M.fade), slide = (1.0f - e) * (float)h * 0.02f;      /* smoothstep fade, a short rise */
    float pcv[8] = {(float)x0, (float)y0 + slide, (float)(x0 + w), (float)(y0 + h) + slide, e, 0.45f, 0.94f, 0};
    vkCmdPushConstants(R.cmd, MV.pl, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pcv, pcv);
    vkCmdDraw(R.cmd, 3, 1, 0, 0);
    vkCmdEndRendering(R.cmd);
    image_barrier(V.images[index], VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    R.pipe_bound = NULL; R.dyn_dirty = 1;                  /* the game's own state tracking must bind again */
}
