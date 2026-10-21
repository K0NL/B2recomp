"""Edición mejorada: the cheats of the in-game menu, put into the lifted C (build/gen_eh) as run-time switches.

    python scripts/cheats_patch.py [--gen=build/gen_eh]          (setup.py's lift step runs it after fps_patch.py)

Each cheat is a Switch 1.2 code (build F19B2E01E1794F93) turned into a check of game.dll's recomps_cheat[] (game_shim.c),
which the menu (runtime/menu_vk.c) sets; with every switch at 0 the game runs as the lifter wrote it. Sources and
reasoning: docs/edicion-mejorada/TRUCOS.md.
  [0] witch time (Gamerjin, "invincible/time control(stop) v4" for F19B2E01E1794F93): its hook replaces the
      `ldr w8, [x8]` at 0x1c974 (f_710001c820). 1 = the load is skipped (invincible through the witch-time check);
      2 / 3 = witch time is set on with 120.0 s and the time scale 0.0001 (stopped) / 0.1 (slow), as the code's
      cave writes it (+0x0 = 1, +0x1c = 120.0f, +0x10 = scale, +0x14 = 2.0f), then the load is skipped.
  [1] infinite health (xtatu, v1.0 "inf health+protection hit+damage0": NOP at 0x1d5cc): the call there is skipped.
  [2] invincible 1P (xtatu, v1.0 "1p invincible mode": RET at 0x20c14): the function returns at once.
      [1] and [2] are v1.0 codes; this region is the same in 1.2 (0x312f4 starts with the same `sub sp, sp, #0x150`
      the v1.0 jump code writes back, 0x20c14 is a function start, and the 1.2 witch-time code hooks 0x1c974 as v1.0).
The edit is idempotent (a file that has the mark is left alone) and fps_patch.py --inplace, run before, restores the
lifter's text of the files it patches, so running the lift step again gives the same result.
"""
import os, re, sys

MARK = '/* [cheats_patch.py] edición mejorada */'
HEAD = MARK + '\nextern volatile int recomps_cheat[16];\n'
HOOKS = [   # (label of the instruction, code put before it)
    ('L_710001c974:',
     'recomps_cheat[13]++; { int m_ = recomps_cheat[0]; if (m_ >= 2) { uint64_t p_ = c->x[8]; wr32(p_, 1u); wr32(p_ + 0x1cULL, 0x42f00000u); '
     'wr32(p_ + 0x10ULL, m_ == 3 ? 0x3dcccccdu : 0x38d1b717u); wr32(p_ + 0x14ULL, 0x40000000u); goto L_710001c978; } '
     'if (m_ == 1) goto L_710001c978; }'),
    ('L_710001d5cc:', 'recomps_cheat[14]++; if (recomps_cheat[1]) goto L_710001d5d0;'),
    ('L_7100020c14:', 'recomps_cheat[15]++; if (recomps_cheat[2]) return;'),
]


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    gen = next((a[6:] for a in sys.argv[1:] if a.startswith('--gen=')), os.path.join(root, 'build', 'gen_eh'))
    todo = {lab: code for lab, code in HOOKS}
    for name in sorted(os.listdir(gen)):
        if not (name.startswith('c_') and name.endswith('.c')) or not todo:
            continue
        path = os.path.join(gen, name)
        with open(path, encoding='utf-8') as f:
            text = f.read()
        hits = [lab for lab in todo if '\n' + lab + ' ' in text]
        if not hits:
            continue
        if MARK in text:                                   # an earlier run (maybe an older version): take it out, then patch again
            text = text.replace(HEAD, '', 1)
            text = re.sub(r'^(?:recomps_cheat\[|\{ int m_ = recomps_cheat|if \(recomps_cheat\[).*\n(?=L_[0-9a-f]+: )', '', text, flags=re.M)
        for lab in hits:
            assert text.count('\n' + lab + ' ') == 1, lab
            text = text.replace('\n' + lab + ' ', '\n' + todo.pop(lab) + '\n' + lab + ' ', 1)
            print('%s: %s patched' % (name, lab))
        assert text.count('#include "funcs.h"\n') == 1, name
        text = text.replace('#include "funcs.h"\n', '#include "funcs.h"\n' + HEAD, 1)
        with open(path, 'w', encoding='utf-8', newline='\n') as f:
            f.write(text)
    if todo:
        raise SystemExit('cheats_patch.py: labels not found (another build of the game?): ' + ', '.join(todo))


if __name__ == '__main__':
    main()
