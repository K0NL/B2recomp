"""Cutscenes keep no deleted character (finding 146): patches the lifted C in place; setup.py's lift step runs it after fps_patch.py.

    python scripts/event_fix.py [--gen=build/gen_eh]

The game's events (cutscenes and scripted scenes: event manager 0x7100e19d80 -> 0x7100e1c324 -> scene update 0x7100e1c420)
bind each character of a scene once (0x7100e27b20: scene slot +0x38 = the actor manager's entry, +0x40 = the entry's +0x38
object) and use those pointers on every update. The actor manager deletes an actor 2-3 updates after a kill request
(0x71009a6e44 -> 0x71009a75f8 -> 0x71009a6d1c, then frees the entry); nothing tells a running scene. In a play at 120 fps
(2026-10-21) the cutscene after Verse 10 "Run from the Enemy!" (event 2260 / 2270) held a Loki (ID 0x10200) the game had
deleted: its first model command (hide part 9) read the deleted model (0x7100e2f5bc: part array null, count 12: 0xa9c0)
and the game stopped. The timing that leads there is rare (the next play through the same spot never met it).
With this patch:
  - when the actor manager deletes an actor (0x71009a6d1c), every running scene that holds it lets go of it (slot +0x38 /
    +0x40 = 0: the state of a character the scene did not find, which the event code checks before every use);
  - at that slot's next update (0x7100e27e1c) the game's own bind runs again (the slot's registered bits +0x94 0x2 / 0x10
    cleared first): it takes the character that now carries the ID (Loki's replacement) or makes the scene's own;
  - an "events:" line for each (none in normal play).
RECOMPS_EVENT_FIX=0 = the game's own. Test only: RECOMPS_EVENT_TEST=<event slot>,<update>,<scene slot>,<updates> asks one of
the 8 event slots at [0x7101871808] that waits to start (state 2) to start at logic update <update>, then requests the
deletion of its scene slot's character <updates> updates into the scene (finding 146's crash with RECOMPS_EVENT_FIX=0;
"let go" and "bound again" without). A save at the Verse 10 checkpoint: event slot 2 = event 2260, scene slot 4 = Loki.
"""
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MARK = '/* [event_fix.py] cutscenes keep no deleted character'

HELP = r'''
/* [event_fix.py] cutscenes keep no deleted character (finding 146) */
#include <stdio.h>
#include <stdlib.h>
static int evfix_on(void) { static int v = -1; if (v < 0) { const char *e = getenv("RECOMPS_EVENT_FIX"); v = !e || atoi(e); } return v; }
static uint64_t evfix_slot[16];
static unsigned evfix_tries[16];
/* every event that can hold a scene: the 8 slots at [0x7101871808] (64 bytes: +0 flags, +20 state, +40 scene) and the list at
   [0x7101871810] (+8 array, +16 count) */
static int evfix_events(uint64_t *ev, int max) {
    int n = 0;
    uint64_t t = rd64(0x7101871808ULL);
    for (int i = 0; t && i < 8 && n < max; i++) ev[n++] = t + 64 * (uint64_t)i;
    uint64_t lst = rd64(0x7101871810ULL);
    if (lst) { uint64_t a = rd64(lst + 8), k = rd64(lst + 16); for (uint64_t j = 0; a && j < k && n < max; j++) ev[n++] = rd64(a + 8 * j); }
    return n;
}
void recomps_evfix_deleted(Ctx *c, uint64_t entry) {
    (void)c;
    if (!entry || !evfix_on()) return;
    uint64_t ev[72];
    int n = evfix_events(ev, 72);
    for (int e = 0; e < n; e++) {
        if (!ev[e] || !(rd32(ev[e]) & 1)) continue;
        uint64_t scene = rd64(ev[e] + 40);
        if (!scene) continue;
        static const int lists[2][2] = { {0xa8, 0xb4}, {0xd8, 0xe4} };   /* the scene's two slot arrays: pointer, count */
        for (int l = 0; l < 2; l++) {
            uint64_t arr = rd64(scene + lists[l][0]);
            int cnt = (int)rd32(scene + lists[l][1]);
            for (int j = 0; arr && j < cnt && j < 256; j++) {
                uint64_t s = arr + 192 * (uint64_t)j;
                if (rd64(s + 0x38) != entry) continue;
                wr64(s + 0x38, 0);
                wr64(s + 0x40, 0);
                int k = 0;
                while (k < 16 && evfix_slot[k] && evfix_slot[k] != s) k++;
                if (k < 16) { evfix_slot[k] = s; evfix_tries[k] = 0; }
                printf("events: character %x deleted while event %x/%x/%x held it (scene slot %d): let go%s\n", rd32(s + 0x2c),
                       rd32(ev[e] + 4), rd32(ev[e] + 8), rd32(ev[e] + 12), j, k < 16 ? "; bound again at its next update" : "");
                fflush(stdout);
            }
        }
    }
}
void recomps_evfix_slot(Ctx *c, uint64_t slot) {
    for (int k = 0; k < 16; k++) {
        if (evfix_slot[k] != slot) continue;
        if (rd64(slot + 0x38)) { evfix_slot[k] = 0; return; }
        if (++evfix_tries[k] > 120) {
            printf("events: character %x not bound again (nothing found in 120 updates)\n", rd32(slot + 0x2c));
            fflush(stdout);
            evfix_slot[k] = 0;
            return;
        }
        wr32(slot + 0x94, rd32(slot + 0x94) & ~0x12u);   /* registered with the scene / in event mode: for the new one */
        Ctx save = *c;
        c->x[0] = slot; c->x[1] = 0; c->x[30] = 0;
        f_7100e27b20(c);
        *c = save;
        if (rd64(slot + 0x38)) {
            printf("events: character %x bound again (%u update%s later)\n", rd32(slot + 0x2c), evfix_tries[k], evfix_tries[k] == 1 ? "" : "s");
            fflush(stdout);
            evfix_slot[k] = 0;
        }
        return;
    }
}
void recomps_evfix_test(Ctx *c) {
    static int ev = -2, at, slot, after;
    static unsigned long long upd, started;
    static uint64_t scene;
    if (ev == -2) { const char *e = getenv("RECOMPS_EVENT_TEST"); ev = -1; if (e && sscanf(e, "%d,%d,%d,%d", &ev, &at, &slot, &after) != 4) ev = -1; }
    if (ev < 0 || ev > 7) return;
    upd++;
    uint64_t t = rd64(0x7101871808ULL), s = t ? t + 64 * (uint64_t)ev : 0;
    if (!s) return;
    if (!scene) {
        if (upd >= (unsigned long long)at && (rd32(s) & 0x80000003u) == 3 && rd32(s + 20) == 2 && !(rd32(s) & 4)) {
            wr32(s, rd32(s) | 4);
            printf("events: TEST: event slot %d (%x) asked to start at update %llu\n", ev, rd32(s + 8), upd);
            fflush(stdout);
        }
        if (rd32(s + 20) == 3 && rd64(s + 40)) { scene = rd64(s + 40); started = upd; }
        return;
    }
    if (upd != started + (unsigned)after || rd64(s + 40) != scene) return;
    uint64_t arr = rd64(scene + 0xa8);
    if (!arr || slot < 0 || slot >= (int)rd32(scene + 0xb4)) return;
    uint64_t entry = rd64(arr + 192 * (uint64_t)slot + 0x38);
    if (!entry) return;
    printf("events: TEST: deletion of character %x (scene slot %d) requested %d updates into the scene\n", rd32(arr + 192 * (uint64_t)slot + 0x2c), slot, after);
    fflush(stdout);
    Ctx save = *c;
    c->x[0] = entry; c->x[30] = 0;
    f_71009a6e44(c);
    *c = save;
    ev = -1;
}
'''
PROTOS = ('\n/* [event_fix.py] */\nvoid recomps_evfix_deleted(Ctx *c, uint64_t entry);\nvoid recomps_evfix_slot(Ctx *c, uint64_t slot);\n'
          'void recomps_evfix_test(Ctx *c);\n')
# (function, its first line, the call inserted after it); the helpers go into the file of 0x7100e27e1c (the bind 0x7100e27b20
# is there too)
HOOKS = [
    ('71009a6d1c', '  recomps_evfix_deleted(c, c->x[0]);'),      # the actor manager deletes this entry
    ('7100e27e1c', '  recomps_evfix_slot(c, c->x[0]);'),         # a scene slot's update
    ('7100e19d80', '  recomps_evfix_test(c);'),                  # the event manager, once per logic update
]
HOME = '7100e27e1c'


def main():
    opt = dict(a[2:].split('=', 1) for a in sys.argv[1:] if a.startswith('--') and '=' in a)
    gen = os.path.abspath(opt.get('gen', os.path.join(ROOT, 'build', 'gen_eh')))
    want = {fn for fn, _ in HOOKS}
    where, texts = {}, {}
    for name in sorted(n for n in os.listdir(gen) if n.startswith('c_') and n.endswith('.c')):
        with open(os.path.join(gen, name), encoding='utf-8', newline='') as f:
            text = f.read()
        found = [fn for fn in want if ('void f_%s(Ctx *c) {' % fn) in text]
        for fn in found:
            where[fn] = name
        if found:
            texts[name] = text
    missing = want - set(where)
    if missing:
        raise SystemExit('event_fix.py: not found in %s: %s' % (gen, ', '.join('f_' + m for m in sorted(missing))))
    if MARK in texts[where[HOME]]:
        print('event_fix.py: already patched (%s)' % ', '.join(sorted(set(where.values()))))
        return
    for fn, call in HOOKS:
        name = where[fn]
        text = texts[name]
        nl = '\r\n' if '\r\n' in text else '\n'
        first = 'void f_%s(Ctx *c) {%s  c->fn = 0x%sULL;%s' % (fn, nl, fn, nl)
        if text.count(first) != 1:
            raise SystemExit('event_fix.py: f_%s does not start as expected in %s' % (fn, name))
        texts[name] = text.replace(first, first + call + '   /* [event_fix.py] */' + nl)
    for name in sorted(set(where.values())):
        text = texts[name]
        nl = '\r\n' if '\r\n' in text else '\n'
        inc = '#include "funcs.h"' + nl
        if text.count(inc) != 1:
            raise SystemExit('event_fix.py: no single #include "funcs.h" in %s' % name)
        text = text.replace(inc, inc + (HELP if name == where[HOME] else PROTOS).replace('\n', nl))
        with open(os.path.join(gen, name), 'w', encoding='utf-8', newline='') as f:
            f.write(text)
    print('event_fix.py: cutscenes let go of deleted characters and bind again (%s)'
          % ', '.join('f_%s in %s' % (fn, where[fn]) for fn, _ in HOOKS))


if __name__ == '__main__':
    main()
