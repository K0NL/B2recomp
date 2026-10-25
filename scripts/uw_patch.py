"""Wide screens (graphics.ini aspect = wide): patches the lifted C in place; setup.py's lift step runs it after prompt_fix.py.

    python scripts/uw_patch.py [--gen=build/gen_eh]

The game's screen size comes from its graphics init config (0x7100dc1dc4 stores the width read at 0x7100dc1e40 into the
global the 232 callers of 0x7100dc4f2c read); the 2D projection 0x710097980c is ortho(0, width, 720, 0) over a layout
1280 wide; five buffers of the post effects are made 1280 wide by constant. At run time RECOMPS_UW_WIDTH (set by
runtime/gfx_options.c from the monitor's shape when aspect = wide; unset or <= 1280 = the game as made):
  - 0x7100dc1e40: the screen width is RECOMPS_UW_WIDTH (3D renders Hor+ at that width: the camera keeps its height);
  - 0x710097b9fc, 0x710097ba0c, 0x710097cba0, 0x710097d29c, 0x7100ed19d8: those buffers follow it (no post-effect ghosting);
  - the half and quarter buffers of depth of field and the other post effects (13 sites, 640 / 320 wide by constant)
    are half / a quarter of it (cutscene depth of field drew in the middle 16:9 area);
  - 0x710097985c: the 2D projection is shifted so the 1280 layout sits in the middle (ortho(-off, width - off)).
The renderer then fixes up full-screen pieces per draw (runtime/nvn_vk_draw.c uw2d_begin / uw_fx_begin). Running this
again rewrites the same lines (it keys on the labels), so it is safe on an already patched tree.
"""
import os, re, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MARK = '/* [uw_patch.py] wide screens */'
HELP = MARK + '''
#include <stdlib.h>
static unsigned uw_width(void) { static int w = -1; if (w < 0) { const char *e = getenv("RECOMPS_UW_WIDTH"); w = e ? atoi(e) : 0; if (w <= 1280 || w > 8192) w = 0; } return (unsigned)w; }
'''
WIDTH = '7100dc1e40'
BUFFERS = ['710097b9fc', '710097ba0c', '710097cba0', '710097d29c', '7100ed19d8']
CENTRE = '710097985c'
# the half (640 x 360) and quarter (320 x 180) buffers of depth of field and the other post effects: 16:9 by constant
HALF = ['710097cbe8', '710097d2d0', '710097d2fc', '710097d328']
QUARTER = ['710097d354', '710097d380', '710097d3a8', '710097d3d0', '710097d3f8', '710097d420', '710097d448', '710097d498', '710097d4c0']


def line_for(label, old):
    if label == WIDTH:
        return ('L_%s: {  uint64_t a = c->x[20] + 0ULL; c->x[8] = (uint64_t)(rd32(a));  } if (uw_width()) c->x[8] = uw_width();   %s'
                % (label, MARK))
    if label in HALF or label in QUARTER:
        reg = re.match(r'L_%s: c->x\[(\d+)\] = ' % label, old)
        assert reg, 'unexpected line at %s: %s' % (label, old)
        d, base = (2, '0x280U') if label in HALF else (4, '0x140U')
        return 'L_%s: c->x[%s] = (uint32_t)(uw_width() ? uw_width() / %d : %s);   %s' % (label, reg.group(1), d, base, MARK)
    if label in BUFFERS:
        reg = re.match(r'L_%s: c->x\[(\d+)\] = ' % label, old)
        assert reg, 'unexpected line at %s: %s' % (label, old)
        return 'L_%s: c->x[%s] = (uint32_t)(uw_width() ? uw_width() : 0x500U);   %s' % (label, reg.group(1), MARK)
    return ('L_%s: { V128 t = c->v[0];  c->v[3] = t; } if (uw_width()) { float off = (c->v[1].f[0] - 1280.0f) * 0.5f; '
            'if (off > 0) { c->v[0].f[0] = -off; c->v[1].f[0] -= off; } }   %s' % (label, MARK))


def main():
    gen = next((a.split('=', 1)[1] for a in sys.argv[1:] if a.startswith('--gen=')), os.path.join(ROOT, 'build', 'gen_eh'))
    labels = [WIDTH] + BUFFERS + HALF + QUARTER + [CENTRE]
    found = {}
    for name in sorted(os.listdir(gen)):
        if not (name.startswith('c_') and name.endswith('.c')):
            continue
        p = os.path.join(gen, name)
        s = open(p, encoding='utf-8', errors='surrogateescape').read()
        if not any('\nL_%s:' % l in s for l in labels):
            continue
        lines = s.split('\n')
        for i, l in enumerate(lines):
            for lab in labels:
                if l.startswith('L_%s:' % lab):
                    lines[i] = line_for(lab, l)
                    found[lab] = name
        s = '\n'.join(lines)
        if MARK + '\n#include <stdlib.h>' not in s:
            k = s.index('#include "funcs.h"') + len('#include "funcs.h"')
            s = s[:k] + '\n' + HELP + s[k:]
        open(p, 'w', encoding='utf-8', errors='surrogateescape').write(s)
    missing = [l for l in labels if l not in found]
    if missing:
        sys.exit('uw_patch.py: sites not found: ' + ', '.join(missing))
    print('uw_patch.py: %d sites in %s' % (len(found), ', '.join(sorted(set(found.values())))))


if __name__ == '__main__':
    main()
