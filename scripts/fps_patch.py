"""Frame rate above 60 fps for Bayonetta 2: patches a copy of the lifted C (notes/fps-research.md, findings 107, 113, 114, 129, 134, 136, 144).

    python scripts/fps_patch.py [--gen=build/gen_eh] --out=<dir> [--ticks=lifter/fps_ticks.txt]
    python scripts/fps_patch.py [--gen=build/gen_eh] --inplace      (the real build: setup.py's lift step runs this)

Copies <gen> to <out> and patches it, or patches <gen> itself (--inplace; the lifter's text of each patched file is kept in
<gen>/fps_orig/ so a second run, or a changed fps_ticks.txt, starts from it). The settings come from graphics.ini's
frame_rate (runtime/gfx_options.c sets the variables below for 61..120) or from the environment (tests). Everything is off unless RECOMPS_UNCAP=1 at run time (then the game runs as before):
  - the clock 0x7100dd1d60 always measures the step (the game's 1.0 / 2.0 fixed steps are replaced) and its 1.0 floor
    becomes RECOMPS_UNCAP_MIN (default 0.0625 = 960 fps); the 2.0 ceiling stays (slow motion below 30 fps, as the game does);
  - the present 0x7100ed6ca4 asks for interval 0 and 0x7100dc4f4c's 30 fps request is ignored; RECOMPS_UNCAP_FPS=<n> limits
    the rate at the present (0 / unset = none; the display's vsync setting applies too);
  - the frame counters of lifter/fps_ticks.txt advance by the 60 Hz frames of game time due to them (finding 134, the
    "smart tick"): once per logic pass the clock adds the step to an accumulator and counts the
    whole frames due (G, at most 4 a pass); per (counter site, field address) a counter that also ran in the previous
    pass moves by the frames due since its last visit, any other visit (a one-off event) by 1 as made; a countdown never
    passes 0 in one visit, an up-counter never skips a value (both owe the rest to the next visit), a countdown at 0 with
    no frame due takes the stock step; the four float values x = 0.9 x + 1 of 0x710093caec run their recurrence once per
    frame due. With a step >= 1 every visit moves 1 or more, so 60 fps behaves as the original.
  - finding 136: float timers that run out (x -= their time this update, then a constant P at or below 0) keep
    their overshoot (P + r, found by their form in every lifted file: find_reloads), and the 48 smoothings
    x += k (T - x) of lifter/fps_smooth.txt take the closed form T + (x - T)(1 - k)^f (f = the step when the site also ran
    in the previous update, else 1);
    RECOMPS_FPS_SMART=0: finding 114's tick instead (1 when a frame is due, else 0, for every counter; no reloads, no smoothing);
    RECOMPS_FPS_TICKS=0 keeps the counters per pass (for comparisons);
  - RECOMPS_UNCAP_LOG=1: a line per 600 clock updates (step mean / min / max, ticks; the counters' visits);
    RECOMPS_FPS_SITELOG=1: per counter site its visits every 6,000 clock updates, and where the first visits that ran off the
    clock's thread came from (guest return addresses); RECOMPS_FPS_SITELOG=<site key, hex>: also that site's first 400 visits.
  - a cinematic's shot and scene changes (0x7100e322e8, 0x7100e32758) keep their game time: the hold on a shot's last
    keyframe lasts as long as at 30 fps instead of one update (finding 129: the pictures ran ahead of the voices);
    RECOMPS_CINE_FIX=0 = the game's own.
  - the slow motion of time channel 0 (0x7100b55938): at its end the event's timeline lands on the 30 fps frame grid at
    any step (the game's scale assumed a step of 2.0; at 0.5 the timeline stopped one float ulp short of the even frame:
    the world freeze, finding 144), and its timer loses its amount x step / k, so it lasts as long as at 30 / 60 fps;
    RECOMPS_SLOWMO_FIX=0 = the game's own, =1 = the alignment only.
The patched files include their helpers after '#include "funcs.h"'; funcs.h gains the declarations of PROTOS.
"""
import os, re, shutil, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

MARK = '/* [fps_patch.py] frame rate above 60 fps'
HELP = r'''
/* [fps_patch.py] frame rate above 60 fps (RECOMPS_UNCAP=1); notes/fps-research.md */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static int uncap_on(void) { static int v = -1; if (v < 0) { const char *e = getenv("RECOMPS_UNCAP"); v = e && atoi(e); } return v; }
static float uncap_min(void) { static float m = -1; if (m < 0) { const char *e = getenv("RECOMPS_UNCAP_MIN"); m = e ? (float)atof(e) : 0.0625f; } return m; }
static float uncap_f(uint64_t a) { uint32_t b = rd32(a); float f; memcpy(&f, &b, 4); return f; }
'''
LIMIT = r'''
#ifdef _WIN32
__declspec(dllimport) int __stdcall QueryPerformanceCounter(long long *);
__declspec(dllimport) int __stdcall QueryPerformanceFrequency(long long *);
__declspec(dllimport) void __stdcall Sleep(unsigned long);
#else /* [fps_patch.py] Linux (notes/linux-port-plan.md step 2): the same three, on the monotonic clock */
#include <time.h>
static int QueryPerformanceCounter(long long *c) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); *c = (long long)t.tv_sec * 1000000000 + t.tv_nsec; return 1; }
static int QueryPerformanceFrequency(long long *f) { *f = 1000000000; return 1; }
static void Sleep(unsigned long ms) { struct timespec t = { (time_t)(ms / 1000), (long)(ms % 1000) * 1000000 }; nanosleep(&t, 0); }
#endif
/* RECOMPS_UNCAP_FPS=<n>: frame limiter at the game's present (0 / unset = none) */
static void uncap_limit(void) {
    static long long freq, next; static int fps = -1;
    if (fps < 0) { const char *e = getenv("RECOMPS_UNCAP_FPS"); fps = e ? atoi(e) : 0; QueryPerformanceFrequency(&freq); }
    if (fps <= 0) return;
    long long now, per = freq / fps; QueryPerformanceCounter(&now);
    if (!next || now - next > 4 * per) next = now;
    next += per;
    while (now < next) { if ((next - now) * 1000 > 2 * freq) Sleep(1); QueryPerformanceCounter(&now); }
}
'''
TICK = r'''
int recomps_fps_tick = 1;                  /* 1 when a whole 60 Hz frame of game time is due this logic pass, else 0 */
/* finding 134: N = clock updates so far, G = whole 60 Hz frames of game time due so far (0..4 an update; a longer freeze
 * keeps 4), and a mark on the thread that runs the clock. Counters also run in jobs on the game's worker threads (object
 * updates, a stage effect: finding 134), which read the clock while it moves: G of update n is kept at fps_gtab[n & 255]
 * before n is published, so a reader takes n, then its G. */
static uint64_t recomps_fps_n, recomps_fps_g, fps_gtab[256];
static float fps_stab[256];               /* the step of update n at [n & 255] (finding 136: smoothing) */
static _Thread_local int recomps_fps_logic;
static void fps_sites_log(long n);         /* the counters' visits (the sites' block) */
static void uncap_tick(float step) {
    static int on = -1; static double acc, acc_g; static long n, ticks, due; static double sum, mn = 99, mx;
    if (on < 0) { const char *e = getenv("RECOMPS_FPS_TICKS"); on = !e || atoi(e); }
    recomps_fps_logic = 1;
    if (!on) { recomps_fps_tick = 1; return; }
    acc += step;
    if (acc >= 0.999) { recomps_fps_tick = 1; acc -= 1.0; if (acc >= 0.999) acc = 0; } else recomps_fps_tick = 0;
    acc_g += step;
    int t = (int)(acc_g + 1e-3);
    if (t > 4) { t = 4; acc_g = 4.0; }
    acc_g -= t; if (acc_g < 0) acc_g = 0;
    recomps_fps_g += (uint64_t)t;
    fps_gtab[(recomps_fps_n + 1) & 255] = recomps_fps_g;
    fps_stab[(recomps_fps_n + 1) & 255] = step;
    __atomic_store_n(&recomps_fps_n, recomps_fps_n + 1, __ATOMIC_RELEASE);
    {   static int log = -1; if (log < 0) { const char *e = getenv("RECOMPS_UNCAP_LOG"); log = e && atoi(e); }
        if (log) { n++; ticks += recomps_fps_tick; due += t; sum += step; if (step < mn) mn = step; if (step > mx) mx = step;
            if (n % 600 == 0) { printf("uncap: 600 clock updates: step mean %.3f min %.3f max %.3f, %ld ticks, %ld frames due\n", sum / 600, mn, mx, ticks, due);
                fps_sites_log(n); fflush(stdout); sum = 0; mn = 99; mx = 0; ticks = 0; due = 0; } } }
}
'''
# finding 134: the counters' rules (the design of a dynamic-FPS mod's counters, written again
# for this game; none of the mod's code). Defined in the clock's file, declared in funcs.h (PROTOS).
SITES = r'''
/* One record per (counter site, field address): the update it last ran in (U), the frames due by then (S), the frames
 * this update gave it (now) and what a limit still owes it (carry, at most 8). */
typedef struct { uint64_t key, field, S, U; int64_t now, carry; float f; } FpsRec;
#define FPS_RECS 4096
static FpsRec fps_rec[FPS_RECS];
static uint64_t fps_st[8];                 /* visits credited 0 / 1 / more frames, first visits (moved by 1), visits again in the same
                                              update, visits off the clock's thread, records given way, visits a limit held back */
static int fps_smart_on(void) { static int v = -1; if (v < 0) { const char *e = getenv("RECOMPS_FPS_SMART"); v = !e || atoi(e); } return v; }
static uint64_t fps_seq_key;              /* RECOMPS_FPS_SITELOG=<site key, hex>: that site's first 400 visits too */
static int fps_sitelog_on(void) {
    static int v = -1;
    if (v < 0) { const char *e = getenv("RECOMPS_FPS_SITELOG"); v = e && strtoull(e, 0, 16); if (v) fps_seq_key = strtoull(e, 0, 16); }
    return v;
}
static struct { uint64_t key, visits, held, de0, again, off, fields, last; int chains; } fps_sl[64];     /* RECOMPS_FPS_SITELOG: per site */
static void fps_chain(Ctx *c, char *out, int n) {     /* the guest return addresses: x30, then the frame records from x29 */
    int k = snprintf(out, n, "%llx", (unsigned long long)c->x[30]);
    uint64_t fp = c->x[29];
    for (int i = 0; i < 8 && fp && !(fp & 7) && k < n - 20; i++) { k += snprintf(out + k, n - k, " %llx", (unsigned long long)rd64(fp + 8)); fp = rd64(fp); }
}
static FpsRec *fps_find(uint64_t key, uint64_t field) {
    uint64_t h = (key * 0x9E3779B97F4A7C15ull) ^ (field * 0xC2B2AE3D27D4EB4Full);
    h ^= h >> 29;
    FpsRec *victim = 0;
    for (int i = 0; i < 8; i++) {
        FpsRec *r = &fps_rec[(h + (uint64_t)i) % FPS_RECS];
        uint64_t k = __atomic_load_n(&r->key, __ATOMIC_ACQUIRE);
        if (k == key && r->field == field) return r;
        if (!k) {                                                /* a free slot: claimed with a CAS (two threads may insert) */
            if (__atomic_compare_exchange_n(&r->key, &k, key, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
                r->field = field; r->U = ~0ull; r->now = r->carry = 0;
                return r;
            }
            if (k == key && r->field == field) return r;
            continue;
        }
        if (!victim || r->U < victim->U) victim = r;            /* all 8 taken: the one visited longest ago gives way */
    }
    if (!victim) victim = &fps_rec[h % FPS_RECS];
    fps_st[6]++;
    victim->key = key; victim->field = field; victim->U = ~0ull; victim->now = victim->carry = 0;
    return victim;
}
/* The frames due to one visit: a counter that also ran in the previous update gets the frames due since its last visit
 * (at most 4) plus what an earlier visit was held back from; a second visit in the same update gets this update's frames
 * again (a counter the game steps twice a frame); any other visit (the first one, a one-off event) moves 1, as made. */
static int64_t fps_visit(Ctx *c, uint64_t key, uint64_t field, FpsRec **rp) {
    uint64_t n = __atomic_load_n(&recomps_fps_n, __ATOMIC_ACQUIRE), g = fps_gtab[n & 255];
    FpsRec *r = fps_find(key, field);
    int held = r->U == n - 1, again = r->U == n;
    int64_t de;
    if (again) { de = r->now; fps_st[4]++; }
    else {
        if (held) { de = (int64_t)(g - r->S); if (de < 0) de = 0; if (de > 4) de = 4; de += r->carry; }
        else { de = 1; fps_st[3]++; }
        r->carry = 0;
        if (de > 8) { r->carry = de - 8; de = 8; }
        r->now = de;
    }
    r->S = g; r->U = n;
    if (!recomps_fps_logic) fps_st[5]++;
    if (fps_sitelog_on()) {
        unsigned s = (unsigned)((key >> 2) * 2654435761u) & 63u;
        for (int i = 0; i < 64; i++, s = (s + 1) & 63u)
            if (fps_sl[s].key == key || !fps_sl[s].key) {
                fps_sl[s].key = key; fps_sl[s].visits++; fps_sl[s].held += held; fps_sl[s].de0 += de == 0; fps_sl[s].again += again;
                fps_sl[s].off += !recomps_fps_logic; if (fps_sl[s].last != field) { fps_sl[s].fields++; fps_sl[s].last = field; }
                if (!recomps_fps_logic && fps_sl[s].chains < 3) {      /* where a visit off the clock's thread comes from */
                    char ch[200]; fps_chain(c, ch, sizeof ch); fps_sl[s].chains++;
                    printf("fps_site: %llx off the clock's thread (thread %p, update %llu): return addresses %s\n", (unsigned long long)key, (void *)&recomps_fps_logic, (unsigned long long)n, ch);
                }
                break;
            }
    }
    if (fps_seq_key == key) {
        static int seq;
        if (seq++ < 400) printf("fps_seq: %llx update %llu frames due %llu field %llx: %s, credit %lld, thread %p%s\n", (unsigned long long)key, (unsigned long long)n, (unsigned long long)g,
                                (unsigned long long)field, again ? "again in this update" : held ? "ran in the previous update" : "first visit", (long long)de, (void *)&recomps_fps_logic, recomps_fps_logic ? " (the clock's)" : "");
    }
    *rp = r;
    return de;
}
static void fps_limit(FpsRec *r, int64_t *de, int64_t keep) {      /* at most keep frames now; the rest is owed */
    if (*de > keep) { r->carry += *de - keep; if (r->carry > 8) r->carry = 8; *de = keep; fps_st[7]++; }
}
/* An integer counter: what its add / sub of k moves it by. k x the frames due to the visit; a countdown (sign < 0, w = its
 * value before the sub) never passes 0 in one visit (the stock step still runs: 0 -> -1 as made) and with no frame due
 * at 0 takes the stock step (a test of the result for 0 would fire again); an up-counter moves at most one step a visit
 * (a test for an exact value elsewhere would be passed over). Off: k; RECOMPS_FPS_SMART=0: k x recomps_fps_tick. */
uint32_t recomps_fps_ci(Ctx *c, uint64_t key, uint64_t field, uint32_t wb, int k, int sign) {
    if (!uncap_on() || !recomps_fps_n) return (uint32_t)k;
    if (!fps_smart_on()) return (uint32_t)(k * recomps_fps_tick);
    FpsRec *r;
    int64_t de = fps_visit(c, key, field, &r), w = (int32_t)wb;
    if (sign < 0) {
        if (de == 0 && w == 0) de = 1;
        fps_limit(r, &de, w > k ? 1 + (w - k) / k : 1);
    } else fps_limit(r, &de, 1);
    fps_st[de == 0 ? 0 : de == 1 ? 1 : 2]++;
    return (uint32_t)(k * de);
}
/* A float counter (an add of +-1.0): the frames due to the visit, which multiply the 1.0; the same limits (w = the value
 * before the add). Off: 1; RECOMPS_FPS_SMART=0: recomps_fps_tick. */
int recomps_fps_cf(Ctx *c, uint64_t key, uint64_t field, float w, int sign) {
    if (!uncap_on() || !recomps_fps_n) return 1;
    if (!fps_smart_on()) return recomps_fps_tick;
    FpsRec *r;
    int64_t de = fps_visit(c, key, field, &r);
    fps_limit(r, &de, sign < 0 ? (w >= 2.0f ? 1 + (int64_t)(w - 1.0f) : 1) : 1);
    fps_st[de == 0 ? 0 : de == 1 ? 1 : 2]++;
    return (int)de;
}
/* x = a x + b once per frame (0x710093caec's four values, a = 0.9, b = 1.0: they approach 10): at the multiply the visit's
 * frames d and a^d, at the add b x (1 + a + ... + a^(d-1)); d = 1 is the stock statement, d = 0 keeps x. Off: a and 1;
 * RECOMPS_FPS_SMART=0: a and recomps_fps_tick, as finding 114 (which kept the multiply per pass). */
static _Thread_local struct { int64_t d; float a; } fps_lin[4];
float recomps_fps_lmul(Ctx *c, int slot, uint64_t key, uint64_t field, float a) {
    int64_t d = 1;
    float p = a;
    if (uncap_on() && recomps_fps_n) {
        if (!fps_smart_on()) d = recomps_fps_tick;
        else {
            FpsRec *r;
            d = fps_visit(c, key, field, &r);
            fps_st[d == 0 ? 0 : d == 1 ? 1 : 2]++;
            p = 1.0f;
            for (int64_t i = 0; i < d; i++) p *= a;
        }
    }
    fps_lin[slot].d = d; fps_lin[slot].a = a;
    return p;
}
float recomps_fps_lsum(int slot) {
    float s = 0.0f, p = 1.0f;
    for (int64_t i = 0; i < fps_lin[slot].d; i++) { s += p; p *= fps_lin[slot].a; }
    return s;
}
/* finding 136: x += k (T - x) once per update with a constant k (lifter/fps_smooth.txt) settled twice as fast per
 * second at 120 fps. The multiply e = (T - x) k takes the rate 1 - (1 - k)^f, f = the step (60 Hz frames) of this update
 * when the site also ran in the previous update (a second visit in the same update: the same f), else 1: the closed form
 * T + (x - T)(1 - k)^f. f = 1 returns k itself (the stock statement). Off: k. */
static uint64_t fps_sm[2];                 /* smoothing visits, of them given the step */
float recomps_fps_smk(Ctx *c, float k, uint64_t key, uint64_t field) {
    (void)c;
    if (!uncap_on() || !fps_smart_on() || !recomps_fps_n) return k;
    uint64_t n = __atomic_load_n(&recomps_fps_n, __ATOMIC_ACQUIRE);
    FpsRec *r = fps_find(key, field);
    float f = r->U == n ? r->f : r->U == n - 1 ? fps_stab[n & 255] : 1.0f;
    r->f = f; r->U = n; r->S = fps_gtab[n & 255];
    fps_sm[0]++;
    if (f == 1.0f || !(k > 0.0f && k < 1.0f) || !(f > 0.0f)) return k;
    fps_sm[1]++;
    return 1.0f - powf(1.0f - k, f);
}
/* finding 136: a float timer that ran out (s_X = x - its time this update, r <= 0) and gets its constant P again
 * (the bits in w_C, stored next): P + r keeps the overshoot, which stock drops, so whenever the step varies each period
 * lasted up to one step longer (P = 1 frame: about 20 % at 120 fps); at or below -P the phase is kept (the expiries in
 * between are not run). Off: P. */
static uint64_t fps_rl[2];                 /* reloads given their overshoot, of them a whole period or more behind */
void recomps_fps_reload(Ctx *c, int C, int X) {
    if (!uncap_on() || !fps_smart_on()) return;
    uint32_t b = (uint32_t)c->x[C];
    float p, r = c->v[X].f[0], v;
    memcpy(&p, &b, 4);
    if (!(p > 0.0f && r < 0.0f)) return;
    if (r > -p) v = p + r;
    else { v = p + fmodf(r, p); if (!(v > 0.0f && v <= p)) v = p; fps_rl[1]++; }
    memcpy(&b, &v, 4);
    c->x[C] = b;
    fps_rl[0]++;
}
static void fps_sites_log(long n) {
    printf("uncap: counters: visits credited 0 / 1 / more frames %llu / %llu / %llu, first visits %llu, again in the same update %llu, "
           "off the clock's thread %llu, records given way %llu, held back by a limit %llu\n", (unsigned long long)fps_st[0], (unsigned long long)fps_st[1],
           (unsigned long long)fps_st[2], (unsigned long long)fps_st[3], (unsigned long long)fps_st[4], (unsigned long long)fps_st[5], (unsigned long long)fps_st[6],
           (unsigned long long)fps_st[7]);
    memset(fps_st, 0, sizeof fps_st);
    printf("uncap: reloads: %llu float timers kept their overshoot (%llu of them a period or more behind)\n", (unsigned long long)fps_rl[0], (unsigned long long)fps_rl[1]);
    memset(fps_rl, 0, sizeof fps_rl);
    printf("uncap: smoothing: %llu visits, %llu of them given the step\n", (unsigned long long)fps_sm[0], (unsigned long long)fps_sm[1]);
    memset(fps_sm, 0, sizeof fps_sm);
    if (fps_sitelog_on() && n % 6000 == 0) {
        for (int i = 0; i < 64; i++)
            if (fps_sl[i].visits)
                printf("fps_site: %llx visits %llu, ran in the previous update %llu, credited 0 %llu, again in the same update %llu, off the clock's thread %llu, field changes %llu\n",
                       (unsigned long long)fps_sl[i].key, (unsigned long long)fps_sl[i].visits, (unsigned long long)fps_sl[i].held, (unsigned long long)fps_sl[i].de0,
                       (unsigned long long)fps_sl[i].again, (unsigned long long)fps_sl[i].off, (unsigned long long)fps_sl[i].fields);
        memset(fps_sl, 0, sizeof fps_sl);
    }
}
'''
PROTOS = ('extern int recomps_fps_tick;\n'
          'uint32_t recomps_fps_ci(Ctx *c, uint64_t key, uint64_t field, uint32_t wb, int k, int sign);\n'
          'int recomps_fps_cf(Ctx *c, uint64_t key, uint64_t field, float w, int sign);\n'
          'float recomps_fps_lmul(Ctx *c, int slot, uint64_t key, uint64_t field, float a);\n'
          'float recomps_fps_lsum(int slot);\n'
          'void recomps_fps_reload(Ctx *c, int C, int X);\n'
          'float recomps_fps_smk(Ctx *c, float k, uint64_t key, uint64_t field);\n')
CINE = r'''
/* [fps_patch.py] finding 129: a cinematic's shots above 60 fps. 0x7100e322e8 moves the shot position (shot i, frame f)
 * by the step; at the end of a shot it holds f at length - k (k = 2 while the game asks for 30 fps) and switches to frame
 * 0 of the next shot one update later; on the scene's last shot 0x7100e32758 reports the end as soon as f is at
 * length - k, and the next scene starts one update later. One update is 33 ms at 30 fps and 8 ms at 120: every shot and
 * scene change gained up to 1.5 frames and the pictures ran ahead of the voices. Here the hold lasts k frames of game time
 * (what is left over carries into the next shot): the same as before at 30 fps. RECOMPS_CINE_FIX=0: the game's own (tests). */
static int cine_fix_on(void) { static int v = -1; if (v < 0) { const char *e = getenv("RECOMPS_CINE_FIX"); v = uncap_on() && (!e || atoi(e)); } return v; }
static struct { uint64_t cur; int32_t shot; float excess, delta; } cine_st[8];     /* per position: the hold so far, the last step */
static int cine_slot(uint64_t cur, int add) {
    int s;
    for (s = 0; s < 8 && cine_st[s].cur != cur; s++) {}
    if (s < 8 || !add) return s < 8 ? s : -1;
    for (s = 0; s < 8 && cine_st[s].cur; s++) {}
    if (s == 8) s = 0;
    cine_st[s].cur = cur; cine_st[s].shot = -1; cine_st[s].excess = cine_st[s].delta = 0;
    return s;
}
/* where the frame would be without the hold, after this update's step (fc = now, fn = after the step, lim = length - k) */
static double cine_hold(int s, int32_t i, float fc, float fn, float lim) {
    double t = (fc >= lim && cine_st[s].shot == i ? (double)lim + cine_st[s].excess : (double)fc) + ((double)fn - (double)fc);
    cine_st[s].shot = i; cine_st[s].excess = (float)(t - lim); cine_st[s].delta = fn - fc;
    return t;
}
static void cine_shot_end(Ctx *c) {         /* a next shot exists: x20 = the position, x19 = the next one, x23 = shot + 1,
                                               x21 = k; v0 = the next frame (>= v1), v1 = length - k */
    uint64_t cur = c->x[20], nxt = c->x[19];
    float fc = uncap_f(cur + 4), lim = c->v[1].f[0], out;
    int s = cine_slot(cur, 1);
    double t = cine_hold(s, (int32_t)rd32(cur), fc, c->v[0].f[0], lim), k = (double)(int32_t)(uint32_t)c->x[21];
    if (t < (double)lim + k) out = lim;
    else { wr32(nxt, (uint32_t)c->x[23]); out = (float)(t - lim - k); cine_st[s].shot = (int32_t)c->x[23]; cine_st[s].excess = 0; }
    uint32_t b; memcpy(&b, &out, 4); wr32(nxt + 4, b);
}
static void cine_last_hold(Ctx *c) {        /* the scene's last shot: x20 = the position, v0 = length - k, v1 = the next frame */
    uint64_t cur = c->x[20];
    cine_hold(cine_slot(cur, 1), (int32_t)rd32(cur), uncap_f(cur + 4), c->v[1].f[0], c->v[0].f[0]);
}
static int cine_end_early(Ctx *c) {         /* 0x7100e32758, f at length - k: x20 = the position, x11 = length, x10 = length - k */
    uint64_t cur = c->x[20];
    int s = cine_slot(cur, 0);
    if (s < 0 || cine_st[s].shot != (int32_t)rd32(cur)) return 0;
    return (double)cine_st[s].excess + cine_st[s].delta < (double)((int32_t)(uint32_t)c->x[11] - (int32_t)(uint32_t)c->x[10]) - 1e-3;
}
'''
CINE_HOOKS = [                              # (label, code put before it); the three functions are in one file
    ('L_7100e3242c: fcmp', 'if (cine_fix_on() && c->v[0].f[0] >= c->v[1].f[0]) { cine_shot_end(c); goto L_7100e3244c; }'),
    ('L_7100e32408: fcmp', 'if (cine_fix_on() && c->v[1].f[0] >= c->v[0].f[0]) cine_last_hold(c);'),
    ('L_7100e327d0: c->x[20] =', 'if (cine_fix_on() && cine_end_early(c)) goto L_7100e32854;'),
]
SLOWMO = r'''
/* [fps_patch.py] finding 144: the slow motion of time channel 0 (0x7100b55938) above 60 fps. While it runs, channel 0's
 * scale ([clock + 120]; the channel's step is the clock's step times it) is its rate, and its timer loses rate (mode 2) or
 * 2 x rate (the game's 30 fps mode) each update. When it ends in the 30 fps mode the scale is set, for one update, to the
 * frames left to the next even frame of the event's timeline x 0.5: a step of 2.0 lands the timeline on the 30 fps frame
 * grid. With a step of 0.5 each update covers a quarter of what is left; the timeline creeps up to the even frame, and one
 * float ulp short of it the advance (0.25 ulp) rounds away: the event stands still (the 5-30 s world freeze at 120 fps,
 * music looping) until one update has a step of 1.0 or more (0.5 ulp rounds up: a long frame). Here the scale is
 * (frames left) / step, at most 1.0, and the timeline counts as aligned within 1/1000 frame of an even frame; the timer
 * loses its stock amount x step / k (k = 2 while the game asks for 30 fps, else 1: the step the game would have), so a
 * slow motion lasts as long as at 30 / 60 fps (it was 2-4 times shorter). RECOMPS_SLOWMO_FIX=0: the game's own; =1: the
 * alignment only. */
static int slowmo_fix(void) { static int v = -1; if (v < 0) { const char *e = getenv("RECOMPS_SLOWMO_FIX"); v = uncap_on() ? (e ? atoi(e) : 2) : 0; } return v; }
static void slowmo_timer(Ctx *c) {          /* 0x7100b55978: v2 = what the timer loses this update (rate or 2 x rate) */
    float k = (rd32(rd64(0x71018668e0ULL) + 4) >> 5 & 1) ? 2.0f : 1.0f;     /* bit 37 of the flag word: 30 fps wanted */
    c->v[2].f[0] = c->v[2].f[0] * uncap_f(rd64(0x7101867708ULL) + 200) / k;
}
static int slowmo_align(Ctx *c) {           /* 0x7100b55b3c: v0 = frames to the next even frame (0 .. 2], x0 = the clock;
                                               1 = aligned (the stock path for a frame already on the grid) */
    float rem = c->v[0].f[0], s = uncap_f(c->x[0] + 200);
    if (!(rem > 1e-3f) || rem > 2.0f - 1e-3f || !(s > 0.0f)) return 1;
    vzero(&c->v[0]); c->v[0].f[0] = rem / s < 1.0f ? rem / s : 1.0f;
    return 0;
}
'''
SLOWMO_HOOKS = [                            # (label, code put before it), in 0x7100b55938
    ('L_7100b55978: {', 'if (slowmo_fix() >= 2) slowmo_timer(c);'),
    ('L_7100b55b3c: {', 'if (slowmo_fix()) { if (slowmo_align(c)) goto L_7100b55ba8; goto L_7100b55b40; }'),
]


def find_file(gen, fn):
    pat = 'void f_%s(' % fn
    for name in sorted(os.listdir(gen)):
        if name.startswith('c_') and name.endswith('.c'):
            with open(os.path.join(gen, name), encoding='utf-8', errors='replace') as f:
                if pat in f.read():
                    return name
    raise SystemExit('function %s not found in %s' % (fn, gen))


def func_range(lines, fn):
    start = next(i for i, l in enumerate(lines) if l.startswith('void f_%s(' % fn))
    end = next(i for i in range(start, len(lines)) if lines[i].startswith('}'))
    return start, end


# finding 136: float reloads, found in every lifted file by their form (B2 has no site table):
#   x = [B + off]; s = x - b; fcmp s, 0 and str s, [B + off] (either order); b.hi / b.gt past the reload (the timer still
#   above 0); then straight-line lines (no call, no goto, no direct jump into them, s not written) to str wC, [B + off]
#   with wC a float constant P (mov, or mov + movk) in 0.01..100000. b is the timer's time this update (B2: mostly the
#   per-object time the getter 0x7100dd1914 left at [obj + 3916] / [obj + 3108]).
RL_FSUB = re.compile(r'^L_([0-9a-f]+): \{ float t = fnan32\(c->v\[(\d+)\]\.f\[0\] - c->v\[(\d+)\]\.f\[0\]\); vzero\(&c->v\[(\d+)\]\); c->v\[\4\]\.f\[0\] = t; \}$')
RL_FCMP0 = lambda s: re.compile(r'^L_[0-9a-f]+: fcmp(?:e)?\(c, \(double\)c->v\[%s\]\.f\[0\], \(double\)0\);$' % s)
RL_STV = lambda s: re.compile(r'^L_[0-9a-f]+: \{  uint64_t a = c->x\[(\d+)\] \+ (\d+)ULL; wrv\(a, &c->v\[%s\], 4\);  \}$' % s)
RL_LDV = lambda b, off, r: re.compile(r'^L_[0-9a-f]+: \{  uint64_t a = c->x\[%s\] \+ %sULL; c->v\[%s\] = rdv\(a, 4\);  \}$' % (b, off, r))
RL_BR = re.compile(r'^L_([0-9a-f]+): if \(cond\(c, (8|12)\)\) goto L_([0-9a-f]+);$')
RL_MOVW = re.compile(r'^L_[0-9a-f]+: c->x\[(\d+)\] = \(uint32_t\)\((?:0U \| )?(0x[0-9a-f]+)ULL\);$')
RL_MOVK = lambda r: re.compile(r'^L_[0-9a-f]+: c->x\[%s\] = \(uint32_t\)\(\(\(uint32_t\)c->x\[%s\] & ~0xffffULL\) \| (0x[0-9a-f]+)ULL\);$' % (r, r))
RL_STW = lambda b, off, r: re.compile(r'^L_([0-9a-f]+): \{  uint64_t a = c->x\[%s\] \+ %sULL; wr32\(a, \(uint32_t\)c->x\[%s\]\);  \}$' % (b, off, r))
RL_DEF = re.compile(r'^(?:static )?void f_([0-9a-f]+)\(')
RL_STOP = re.compile(r'\bf_[0-9a-f]+\(c\)|call_indirect|return|goto ')


def find_reloads(lines):
    """[(store line index, w register, s register, P)] of one file's pristine lines"""
    import struct
    jumped = set()                                        # labels a direct goto reaches (the dispatch switch's cases do not count)
    for l in lines:
        if not l.lstrip().startswith('case 0x'):
            jumped.update(re.findall(r'goto L_([0-9a-f]+);', l))
    out, fstart = [], 0
    for i, l in enumerate(lines):
        if RL_DEF.match(l):
            fstart = i
        m = RL_FSUB.match(l)
        if not m:
            continue
        a_, s_ = m.group(2), m.group(4)
        k, cmp_, st, base, off = i + 1, False, False, None, None
        while k < min(len(lines), i + 3) and not (cmp_ and st):
            if RL_FCMP0(s_).match(lines[k]):
                cmp_ = True
            elif RL_STV(s_).match(lines[k]):
                base, off = RL_STV(s_).match(lines[k]).groups(); st = True
            else:
                break
            k += 1
        if not (cmp_ and st) or k >= len(lines) or not RL_BR.match(lines[k]):
            continue
        if not any(RL_LDV(base, off, a_).match(lines[j]) for j in range(max(fstart, i - 10), i)):
            continue                                      # not x = x - b on the stored field
        found = None
        for j in range(k + 1, min(len(lines), k + 9)):
            lab = lines[j].split(':', 1)[0][2:] if lines[j].startswith('L_') else None
            if lab is None or lab in jumped or RL_STOP.search(lines[j]):
                break
            if re.search(r'c->v\[%s\]' % s_, lines[j]):
                break                                     # s read or written in between: not the plain form
            mw = RL_MOVW.match(lines[j])
            if mw and not found:
                r, v = mw.group(1), int(mw.group(2), 16)
                if j + 1 < len(lines) and RL_MOVK(r).match(lines[j + 1]):
                    v = (v & 0xffff0000) | int(RL_MOVK(r).match(lines[j + 1]).group(1), 16)
                found = (r, struct.unpack('<f', struct.pack('<I', v & 0xffffffff))[0])
                continue
            if found and RL_STW(base, off, found[0]).match(lines[j]):
                if 0.01 <= found[1] <= 100000.0:
                    out.append((j, found[0], s_, found[1]))
                break
            if found and re.search(r'c->x\[%s\] =' % found[0], lines[j]) and not RL_MOVK(found[0]).match(lines[j]):
                break                                     # w register rewritten before the store
    return out


INT_OP = [  # the +/- 1 or 2 of a counter as the lifter writes it: (regex, sign); groups: source register, the immediate
    (re.compile(r'\(uint32_t\)\(\(uint32_t\)c->x\[(\d+)\] \+ ([12])ULL\)'), 1),
    (re.compile(r'\(uint32_t\)\(\(uint32_t\)c->x\[(\d+)\] - ([12])ULL\)'), -1),
    (re.compile(r'adds32\(c, \(uint32_t\)c->x\[(\d+)\], ~\(uint32_t\)([12])ULL, 1\)'), -1),
    (re.compile(r'adds32\(c, \(uint32_t\)c->x\[(\d+)\], \(uint32_t\)([12])ULL, 0\)'), 1),
]
ASSIGN = lambda r: re.compile(r'^L_[0-9a-f]+: c->x\[%s\] = ' % r)
MOV = re.compile(r'= \(uint(?:32|64)_t\)\(0U? \| (?:\(uint32_t\))?c->x\[(\d+)\]\);')
VDEF = lambda r: re.compile(r'(c->v\[%s\]\.f\[0\] = t;|c->v\[%s\] = rdv|vzero\(&c->v\[%s\]\);|c->v\[%s\] = t[12]?;)' % (r, r, r, r))
CALL = re.compile(r'\bf_[0-9a-f]+\(c\)')
STORE = re.compile(r'\{  uint64_t a = c->x\[(\d+)\] \+ (\d+)ULL; wr(?:32\(a, \(uint32_t\)c->x\[\d+\]\)|v\(a, &c->v\[(\d+)\], 4\));  \}')
LOADX = lambda r, b, off: re.compile(r'^L_[0-9a-f]+: \{  uint64_t a = c->x\[%s\] \+ %sULL; c->x\[%s\] = \(uint64_t\)\(rd32\(a\)\);  \}' % (b, off, r))
LOADV = lambda r, b, off: re.compile(r'^L_[0-9a-f]+: \{  uint64_t a = c->x\[%s\] \+ %sULL; c->v\[%s\] = rdv\(a, 4\);  \}' % (b, off, r))


def store_field(line):
    """(base register, offset) of a counter's store: its field is c->x[base] + offset"""
    m = STORE.search(line)
    if not m:
        raise SystemExit('not a store to c->x[n] + imm: %s' % line.strip())
    return m.group(1), m.group(2)


def check_base(lines, a, b, base, what):
    """the field's base register keeps its value over lines a..b (the field is computed at the patched line)"""
    for k in range(a, b):
        if ASSIGN(base).match(lines[k]) or CALL.search(lines[k]):
            raise SystemExit('%s: x%s changes between the patched line and the store: %s' % (what, base, lines[k].strip()))


def check_load(lines, lo, k, regex, what):
    """the value the counter steps was loaded from its own field (load; step; store)"""
    for j in range(k - 1, max(lo, k - 12), -1):
        if regex.match(lines[j]):
            return
    raise SystemExit('%s: the stepped value is not loaded from the stored field before it' % what)


def patch_int(lines, lo, i, key):
    """the counter's +/- k becomes recomps_fps_ci(...) (finding 134): key = its store's address"""
    m = re.search(r'wr(?:8|16|32|64)\(a, \(uint\d+_t\)c->x\[(\d+)\]\)', lines[i])
    reg = m.group(1)
    base, off = store_field(lines[i])
    for k in range(i - 1, max(lo, i - 14), -1):
        if not ASSIGN(reg).match(lines[k]):
            continue
        mv = MOV.search(lines[k])
        if mv:
            reg = mv.group(1); continue
        for rx, sign in INT_OP:
            mm = rx.search(lines[k])
            if not mm:
                continue
            src, imm = mm.group(1), mm.group(2)
            if reg == base:
                raise SystemExit('%s: the counter is its own base register' % lines[i][:16])
            check_base(lines, k + 1, i, base, lines[i][:16])
            check_load(lines, lo, k, LOADX(src, base, off), lines[i][:16])
            call = 'recomps_fps_ci(c, 0x%xULL, c->x[%s] + %sULL, (uint32_t)c->x[%s], %s, %d)' % (key, base, off, src, imm, sign)
            a, b = mm.span()
            old = lines[k][a:b]
            new = old.replace('%sULL)' % imm, '(uint64_t)%s)' % call, 1) if sign > 0 and 'adds32' not in old else \
                old.replace('- %sULL)' % imm, '- (uint64_t)%s)' % call, 1) if 'adds32' not in old else \
                old.replace('(uint32_t)%sULL, ' % imm, '(uint32_t)%s, ' % call, 1)
            assert new != old, old
            lines[k] = lines[k][:a] + new + lines[k][b:]
            return k
        raise SystemExit('no +/-1 on the line defining x%s for %s: %s' % (reg, lines[i][:16], lines[k].strip()))
    raise SystemExit('no definition of x%s found before %s' % (reg, lines[i][:16]))


def patch_f1(lines, lo, hi, i, key, op=False, slot=None, anystep=False):
    """the 1.0 of a float counter's add is multiplied by recomps_fps_cf(...) (finding 134); slot (kind f1lin): x = a x + 1
    made of a multiply and the add, a^d at the multiply and 1 + a + ... + a^(d-1) at the add"""
    if op:                                                # the label names the add: its store follows (two paths may join there)
        j = i
        d = re.search(r'fnan32\(.*?\); vzero\(&c->v\[(\d+)\]\);', lines[j]).group(1)
        i = next(k for k in range(j + 1, min(hi, j + 8)) if re.search(r'wrv\(a, &c->v\[%s\], 4\)' % d, lines[k]))
    else:
        v = re.search(r'wrv\(a, &c->v\[(\d+)\]', lines[i]).group(1)
        j = next(k for k in range(i - 1, lo, -1) if VDEF(v).search(lines[k]))
    base, off = store_field(lines[i])
    m = re.search(r'fnan32\(c->v\[(\d+)\]\.f\[0\] ([+-]) c->v\[(\d+)\]\.f\[0\]\)', lines[j])
    if not m:
        raise SystemExit('no float add at or before %s: %s' % (lines[i][:16], lines[j].strip()))
    if anystep:                                           # finding 139, kind f1step: x + step, any step (a table value)
        return patch_fstep(lines, lo, i, j, m, key, base, off)
    for r, other in ((m.group(1), m.group(3)), (m.group(3), m.group(1))):
        for k in range(j - 1, lo, -1):
            if int(r) < 8 and CALL.search(lines[k]):
                break
            if VDEF(r).search(lines[k]):
                c1 = re.search(r'fnan32\(\(float\)(-?)1\.0\)', lines[k])
                if not c1:
                    break
                sign = (-1 if c1.group(1) else 1) * (-1 if m.group(2) == '-' and r == m.group(3) else 1)
                if m.group(2) == '-' and r == m.group(1):
                    raise SystemExit('%s: 1.0 - x is not a counter' % lines[i][:16])
                field = 'c->x[%s] + %sULL' % (base, off)
                old = 'c->v[%s].f[0]' % r
                if slot is None:
                    check_base(lines, j, i, base, lines[i][:16])
                    check_load(lines, lo, j, LOADV(other, base, off), lines[i][:16])
                    rep = '(%s * (float)recomps_fps_cf(c, 0x%xULL, %s, c->v[%s].f[0], %d))' % (old, key, field, other, sign)
                else:                                     # the other operand is x * a, made from the field's value
                    mul = next(q for q in range(j - 1, lo, -1) if VDEF(other).search(lines[q]))
                    mm = re.search(r'fnan32\(c->v\[%s\]\.f\[0\] \* c->v\[(\d+)\]\.f\[0\]\); vzero\(&c->v\[%s\]\);' % (other, other), lines[mul])
                    if not mm or mm.group(1) == other:
                        raise SystemExit('%s: f1lin, but no x = x * a before the add: %s' % (lines[i][:16], lines[mul].strip()))
                    check_base(lines, mul, i, base, lines[i][:16])
                    check_load(lines, lo, mul, LOADV(other, base, off), lines[i][:16])
                    fa = 'c->v[%s].f[0]' % mm.group(1)
                    a2, b2 = lines[mul].split('fnan32(', 1)
                    lines[mul] = a2 + 'fnan32(' + b2.replace('* ' + fa, '* recomps_fps_lmul(c, %d, 0x%xULL, %s, %s)' % (slot, key, field, fa), 1)
                    rep = '(%s * recomps_fps_lsum(%d))' % (old, slot)
                a, b = lines[j].split('fnan32(', 1)
                lines[j] = a + 'fnan32(' + b.replace(old, rep, 1)
                return j
    raise SystemExit('the float add before %s has no constant 1.0 operand: %s' % (lines[i][:16], lines[j].strip()))


def patch_fstep(lines, lo, i, j, m, key, base, off):
    """finding 139, kind f1step: x = x + step, the step any value the pass adds (B2's 0x71009ebf04: a per-difficulty table
    entry). The step is multiplied by the visit's frames, at most one step a visit (the rest is owed, as an up-counter's).
    The other operand must be the field's value: loaded from it with no other write of that register, and no call that
    could change it (v0-v7), in between."""
    if m.group(2) != '+':
        raise SystemExit('%s: f1step takes x + step only: %s' % (lines[i][:16], lines[j].strip()))
    for step, x in ((m.group(3), m.group(1)), (m.group(1), m.group(3))):
        loaded = False
        for k in range(j - 1, max(lo, j - 24), -1):
            if LOADV(x, base, off).match(lines[k]):
                loaded = True
                break
            if VDEF(x).search(lines[k]) or (int(x) < 8 and CALL.search(lines[k])) or ASSIGN(base).match(lines[k]):
                break
        if not loaded:
            continue
        check_base(lines, j, i, base, lines[i][:16])
        old = 'c->v[%s].f[0]' % step
        rep = '(%s * (float)recomps_fps_cf(c, 0x%xULL, c->x[%s] + %sULL, c->v[%s].f[0], 1))' % (old, key, base, off, x)
        a, b = lines[j].split('fnan32(', 1)
        lines[j] = a + 'fnan32(' + b.replace(old, rep, 1)
        return j
    raise SystemExit('%s: f1step, but neither operand of the add is the stored field\'s value: %s' % (lines[i][:16], lines[j].strip()))


def main():
    opt = dict(a[2:].split('=', 1) for a in sys.argv[1:] if a.startswith('--') and '=' in a)
    gen = os.path.abspath(opt.get('gen', os.path.join(ROOT, 'build', 'gen_eh')))
    inplace = '--inplace' in sys.argv                     # the real build: patch <gen> itself (setup.py's lift step)
    out = gen if inplace else os.path.abspath(opt['out'])
    ticks = opt.get('ticks', os.path.join(ROOT, 'lifter', 'fps_ticks.txt'))
    if not inplace and gen == out:
        raise SystemExit('--out must differ from --gen (or use --inplace)')
    os.makedirs(out, exist_ok=True)
    # --inplace keeps the lifter's text of every patched file in <gen>/fps_orig/ (not compiled: a subfolder): a file that
    # still carries the patch mark is patched again from that copy, so running twice or changing fps_ticks.txt is safe;
    # a file the lifter wrote again (no mark) replaces its copy
    orig = os.path.join(gen, 'fps_orig')
    sites = [l.split()[:3] for l in open(ticks) if l.strip() and not l.startswith('#')]
    files = {}                                            # file -> lines
    def lines_of(fn, name=None):
        name = name or find_file(gen, fn)
        if name not in files:
            text = open(os.path.join(gen, name), encoding='utf-8').read()
            if inplace:
                os.makedirs(orig, exist_ok=True)
                if MARK in text:
                    text = open(os.path.join(orig, name), encoding='utf-8').read()
                else:
                    with open(os.path.join(orig, name), 'w', encoding='utf-8', newline='\n') as f:
                        f.write(text)
            files[name] = text.split('\n')
        return name, files[name]

    def replace_once(lines, old, new):
        hits = [k for k, l in enumerate(lines) if l.startswith(old)]
        assert len(hits) == 1, old
        lines[hits[0]] = new + lines[hits[0]][len(old):]

    # finding 136: float reloads in every lifted file (found by their form, before the other patches touch a line)
    nrl, rl_files = 0, 0
    for name in sorted(n for n in os.listdir(gen) if n.startswith('c_') and n.endswith('.c')):
        text = open(os.path.join(gen, name), encoding='utf-8').read()
        if inplace and MARK in text:
            text = open(os.path.join(orig, name), encoding='utf-8').read()
        found = find_reloads(text.split('\n'))
        if not found:
            continue
        rl_files += 1
        name, ls = lines_of('', name)
        for j, w, sreg, p in found:
            lab = ls[j].split(':', 1)[0]
            ls[j] = lab + ': recomps_fps_reload(c, %s, %s); ' % (w, sreg) + ls[j][len(lab) + 1:].lstrip(' ')
            nrl += 1
    print('reloads: %d float timers in %d files keep their overshoot' % (nrl, rl_files))

    # the clock: measured step always, floor RECOMPS_UNCAP_MIN, tick after the step is known
    name, ls = lines_of('7100dd1d60')
    replace_once(ls, 'L_7100dd1d60: {', 'if (uncap_on()) c->x[1] = 1;\nL_7100dd1d60: {')
    replace_once(ls, 'L_7100dd1e00: { float t = fnan32((float)1.0);', 'L_7100dd1e00: { float t = fnan32(uncap_on() ? uncap_min() : (float)1.0);')
    replace_once(ls, 'L_7100dd1e14: c->x[8] = (uint32_t)(0U | 0x3f800000ULL);',
                 'L_7100dd1e14: if (uncap_on()) { float m = uncap_min(); uint32_t b; memcpy(&b, &m, 4); c->x[8] = b; } else c->x[8] = (uint32_t)(0U | 0x3f800000ULL);')
    replace_once(ls, 'L_7100dd1e30: {  uint64_t a = c->x[19] + 208ULL; wr64(a, c->x[20]);  }',
                 'L_7100dd1e30: {  uint64_t a = c->x[19] + 208ULL; wr64(a, c->x[20]);  }\nif (uncap_on()) uncap_tick(uncap_f(c->x[19] + 200));')
    clock_file = name
    # the present: interval 0 and the limiter; the 30 fps request: ignored
    name, ls = lines_of('7100ed6ca4')
    replace_once(ls, 'L_7100ed6ca4: {', 'if (uncap_on()) { c->x[2] = 0; uncap_limit(); }\nL_7100ed6ca4: {')
    present_file = name
    name, ls = lines_of('7100dc4f4c')
    replace_once(ls, 'L_7100dc4f60: {', 'if (uncap_on()) c->x[8] = 0;\nL_7100dc4f60: {')
    # a cinematic's shot changes (finding 129): the end of a shot keeps its game time
    name, ls = lines_of('7100e322e8')
    for lab, hook in CINE_HOOKS:
        replace_once(ls, lab, hook + '\n' + lab)
    cine_file = name
    # the slow motion of time channel 0 (finding 144): its end lands the event's timeline on the frame grid at any step
    name, ls = lines_of('7100b55938')
    for lab, hook in SLOWMO_HOOKS:
        replace_once(ls, lab, hook + '\n' + lab)
    slowmo_file = name
    # the counters (finding 134): each moves by the frames due to it; the key of its record is its store's address
    done, slots = 0, 0
    for fn, lab, kind in sites:
        name, ls = lines_of(fn)
        lo, hi = func_range(ls, fn)
        i = next(k for k in range(lo, hi) if ls[k].startswith(lab + ':'))
        key = int(lab[2:], 16)
        if kind == 'int':
            k = patch_int(ls, lo, i, key)
        elif kind in ('f1', 'f1op'):
            k = patch_f1(ls, lo, hi, i, key, op=kind == 'f1op')
        elif kind == 'f1step':
            k = patch_f1(ls, lo, hi, i, key, anystep=True)
        elif kind == 'f1lin':
            k = patch_f1(ls, lo, hi, i, key, slot=slots)
            slots += 1
        else:
            raise SystemExit('%s %s: unknown kind %s' % (fn, lab, kind))
        print('%s %s %-5s %s' % (fn, lab, kind, ls[k][:170]))
        done += 1
    assert slots <= 4, 'more f1lin sites than fps_lin[] slots'
    # finding 136: the reviewed smoothing sites (lifter/fps_smooth.txt): the multiply's constant k becomes recomps_fps_smk(...)
    smooth = opt.get('smooth', os.path.join(ROOT, 'lifter', 'fps_smooth.txt'))
    nsm = 0
    for row in ([l.split() for l in open(smooth) if l.strip() and not l.startswith('#')] if os.path.isfile(smooth) else []):
        fn, stl, mul, kreg, kval = row[:5]
        name, ls = lines_of(fn)
        lo, hi = func_range(ls, fn)
        mi = next(k for k in range(lo, hi) if ls[k].startswith(mul + ':'))
        si = next(k for k in range(lo, hi) if ls[k].startswith(stl + ':'))
        base, off = store_field(ls[si])
        kr = kreg[1:]
        m = re.search(r'fnan32\(c->v\[(\d+)\]\.f\[0\] \* c->v\[(\d+)\]\.f\[0\]\)', ls[mi])
        if not m or kr not in (m.group(1), m.group(2)) or m.group(1) == m.group(2):
            raise SystemExit('%s %s: not the multiply by v%s: %s' % (fn, mul, kr, ls[mi].strip()))
        check_base(ls, mi, si, base, mul)
        old_k = 'c->v[%s].f[0]' % kr
        a, b = ls[mi].split('fnan32(', 1)
        ls[mi] = a + 'fnan32(' + b.replace(old_k, 'recomps_fps_smk(c, %s, 0x%sULL, c->x[%s] + %sULL)' % (old_k, stl[2:], base, off), 1)
        nsm += 1
    print('smoothing: %d sites take the closed form' % nsm)
    # write: helpers after #include "funcs.h" in every patched file (in place: only files whose text changes, so the
    # compile's up-to-date check keeps the others)
    def write(path, text):
        if os.path.isfile(path) and open(path, encoding='utf-8').read() == text:
            return
        with open(path, 'w', encoding='utf-8', newline='\n') as f:
            f.write(text)
    for fname in sorted(os.listdir(gen)):
        src = os.path.join(gen, fname)
        if fname in files:
            text = '\n'.join(files[fname])
            extra = HELP + (LIMIT if fname == present_file else '') + (TICK + SITES if fname == clock_file else '') + (CINE if fname == cine_file else '') + \
                    (SLOWMO if fname == slowmo_file else '')
            assert text.count('#include "funcs.h"\n') == 1, fname
            write(os.path.join(out, fname), text.replace('#include "funcs.h"\n', '#include "funcs.h"\n' + extra, 1))
        elif fname == 'funcs.h':                          # the sites' helpers (defined in the clock's file); older lists replaced
            h = open(src, encoding='utf-8').read()
            h = '\n'.join(l for l in h.split('\n') if not l.startswith(('extern int recomps_fps_tick;', 'uint32_t recomps_fps_', 'int recomps_fps_', 'float recomps_fps_', 'void recomps_fps_')))
            h = h.replace('\n', '\n' + PROTOS, 1)         # always: earlier lists and duplicates go
            write(os.path.join(out, fname), h)
        elif inplace:
            if fname.endswith('.c') and os.path.isfile(os.path.join(orig, fname)):      # patched before, no longer: the lifter's text
                if MARK in open(src, encoding='utf-8').read():
                    write(src, open(os.path.join(orig, fname), encoding='utf-8').read())
                os.remove(os.path.join(orig, fname))
        elif os.path.isfile(src):
            dst = os.path.join(out, fname)
            if not os.path.exists(dst) or os.path.getmtime(dst) < os.path.getmtime(src) or os.path.getsize(dst) != os.path.getsize(src):
                shutil.copy2(src, dst)
    print('patched %d counters in %d files (+ clock, present, cinematic shots); out: %s' % (done, len(files), out))


if __name__ == '__main__':
    main()
