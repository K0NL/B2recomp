"""The action prompt's ring turns with its button's place (finding 148): patches the lifted C in place; setup.py's lift step
runs it after event_fix.py.

    python scripts/prompt_fix.py [--gen=build/gen_eh]

The HUD's action prompt ("Check", ...) is two parts. The 0xa3 part (0x7100d34050) draws the button's picture in one of four
places around a ring: top / left / right / bottom, from the masks of the table at 0x7101464340 (the game's: X, Y, A, B, the
Switch places). The 0xa1 part (0x7100d322b4) draws the ring, a crescent whose hollow faces the button: its maker stores an
angle in degrees at +0x1bc. 0x7100c018b0 (two stores) and 0x7100c02bf0 choose it from the button (A 0, B 90, X -90, Y 180,
A + X -45, R / ZR -90, else 0); 0x7100c02ecc, the prompt with two buttons (X and A), stores -90 and 0.
With the face buttons by printed letter (a PlayStation or Xbox prompt mod) the runtime rewrites the table to Y, X, B, A
(runtime/nnfs.c prompt_places_by_letter: Y at the top, A at the bottom, as on those pads). After each of the five stores
recomps_prompt_ring() gives the ring the angle of its button's place in that table, so the crescent turns with the picture.
With the game's own table nothing changes. RECOMPS_PROMPT_RING=0 = the game's angles whatever the table holds.
A lift these sites do not match (another version, a code mod that changes them) is left as it is, with a message.
"""
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MARK = '/* [prompt_fix.py] the action prompt\'s ring'

HELP = r'''
/* [prompt_fix.py] the action prompt's ring turns with its button's place (finding 148) */
#include <stdio.h>
#include <stdlib.h>
void recomps_prompt_ring(uint64_t at, uint32_t mask) {
    static int on = -1, said;
    if (on < 0) { const char *e = getenv("RECOMPS_PROMPT_RING"); on = !e || atoi(e); }
    /* the places of the table at 0x7101464340 (0x7100d34050) and their angles: top, left, right, bottom */
    static const int deg[4] = { 270, 180, 0, 90 };
    uint32_t t[4];
    int p[4], n = 0, a;
    for (int i = 0; i < 4; i++) t[i] = rd32(0x7101464340ULL + 4 * (uint64_t)i);
    if (!on || (t[0] == 0x40 && t[1] == 0x80 && t[2] == 0x10 && t[3] == 0x20)) return;   /* the game's own places */
    if (mask != 0x10 && mask != 0x20 && mask != 0x40 && mask != 0x80 && mask != 0x50) return;   /* the face buttons only */
    for (int i = 0; i < 4; i++) if (t[i] & mask) p[n++] = i;
    if (n == 1) a = deg[p[0]];
    else if (n == 2 && (deg[p[0]] - deg[p[1]]) % 180) {      /* two neighbouring places (A + X): between them */
        int x = deg[p[0]], y = deg[p[1]];
        if (x - y > 180) y += 360; else if (y - x > 180) x += 360;
        a = (x + y) / 2 % 360;
    } else return;
    float f = (float)(a > 180 ? a - 360 : a), old;
    uint32_t b, ob = rd32(at);
    memcpy(&b, &f, 4); memcpy(&old, &ob, 4);
    wr32(at, b);
    if (said < 8 && b != ob) {
        said++;
        printf("prompts: the action prompt's ring for button %x turns to %g degrees (the game's: %g), its place in the table\n", mask, f, old);
        fflush(stdout);
    }
}
'''
PROTOS = '\n/* [prompt_fix.py] */\nvoid recomps_prompt_ring(uint64_t at, uint32_t mask);\n'
# (function, label, the store as lifted, the button's mask there): the ring is at x8 in each; x20 = the prompt's description
# (its +20 = the button) in the first two functions
STORE = '{  uint64_t a = c->x[8] + 444ULL; %s  }'
SITES = [
    ('7100c018b0', '7100c01b64', STORE % 'wrv(a, &c->v[8], 4);', 'rd32(c->x[20] + 20ULL)'),
    ('7100c018b0', '7100c01d0c', STORE % 'wrv(a, &c->v[0], 4);', 'rd32(c->x[20] + 20ULL)'),
    ('7100c02bf0', '7100c02d24', STORE % 'wrv(a, &c->v[8], 4);', 'rd32(c->x[20] + 20ULL)'),
    ('7100c02ecc', '7100c02fc4', STORE % 'wr32(a, (uint32_t)c->x[9]);', '0x40'),
    ('7100c02ecc', '7100c03060', STORE % 'wr32(a, 0U);', '0x10'),
]
HOME = '7100c018b0'


def main():
    opt = dict(a[2:].split('=', 1) for a in sys.argv[1:] if a.startswith('--') and '=' in a)
    gen = os.path.abspath(opt.get('gen', os.path.join(ROOT, 'build', 'gen_eh')))
    want = {fn for fn, _, _, _ in SITES}
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
        print('prompt_fix.py: not found in %s: %s; the action prompt\'s ring keeps the game\'s angles'
              % (gen, ', '.join('f_' + m for m in sorted(missing))))
        return
    if any(MARK in texts[n] for n in set(where.values())):
        print('prompt_fix.py: already patched (%s)' % ', '.join(sorted(set(where.values()))))
        return
    out = dict(texts)
    for fn, label, store, mask in SITES:
        name = where[fn]
        nl = '\r\n' if '\r\n' in out[name] else '\n'
        line = 'L_%s: %s%s' % (label, store, nl)
        if out[name].count(line) != 1:
            print('prompt_fix.py: f_%s in %s is not as expected at %s (another version or a code mod?); the action '
                  'prompt\'s ring keeps the game\'s angles' % (fn, name, label))
            return
        out[name] = out[name].replace(line, 'L_%s: %s recomps_prompt_ring(c->x[8] + 444ULL, %s);   /* [prompt_fix.py] */%s'
                                      % (label, store, mask, nl))
    for name in sorted(set(where.values())):
        text = out[name]
        nl = '\r\n' if '\r\n' in text else '\n'
        inc = '#include "funcs.h"' + nl
        if text.count(inc) != 1:
            print('prompt_fix.py: no single #include "funcs.h" in %s; the action prompt\'s ring keeps the game\'s angles' % name)
            return
        out[name] = text.replace(inc, inc + (HELP if name == where[HOME] else PROTOS).replace('\n', nl))
    for name in sorted(set(where.values())):
        with open(os.path.join(gen, name), 'w', encoding='utf-8', newline='') as f:
            f.write(out[name])
    print('prompt_fix.py: the action prompt\'s ring turns with its button\'s place (%d stores in %s)'
          % (len(SITES), ', '.join('f_%s in %s' % (fn, where[fn]) for fn in sorted(want))))


if __name__ == '__main__':
    main()
