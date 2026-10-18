"""Frame rate above 60 fps for Bayonetta 2: patches a copy of the lifted C (notes/fps-research.md, findings 107, 113, 114).

    python scripts/fps_patch.py [--gen=build/gen_eh] --out=<dir> [--ticks=lifter/fps_ticks.txt]
    python scripts/fps_patch.py [--gen=build/gen_eh] --inplace      (the real build: setup.py's lift step runs this)

Copies <gen> to <out> and patches it, or patches <gen> itself (--inplace; the lifter's text of each patched file is kept in
<gen>/fps_orig/ so a second run, or a changed fps_ticks.txt, starts from it). The settings come from graphics.ini's
frame_rate (runtime/gfx_options.c sets the variables below for 61..120) or from the environment (tests). Everything is off unless RECOMPS_UNCAP=1 at run time (then the game runs as before):
  - the clock 0x7100dd1d60 always measures the step (the game's 1.0 / 2.0 fixed steps are replaced) and its 1.0 floor
    becomes RECOMPS_UNCAP_MIN (default 0.0625 = 960 fps); the 2.0 ceiling stays (slow motion below 30 fps, as the game does);
  - the present 0x7100ed6ca4 asks for interval 0 and 0x7100dc4f4c's 30 fps request is ignored; RECOMPS_UNCAP_FPS=<n> limits
    the rate at the present (0 / unset = none; the display's vsync setting applies too);
  - the frame counters of lifter/fps_ticks.txt advance by recomps_fps_tick instead of 1: once per logic pass the clock adds
    the step to an accumulator and the tick is 1 when a whole 60 Hz frame is due, else 0 (always 1 with a step >= 1, so
    60 fps and slower behave as the original). RECOMPS_FPS_TICKS=0 keeps the counters per pass (for comparisons);
  - RECOMPS_UNCAP_LOG=1: a line per 600 clock updates (step mean / min / max, ticks).
The patched files include their helpers after '#include "funcs.h"'; funcs.h gains 'extern int recomps_fps_tick;'.
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
static void uncap_tick(float step) {
    static int on = -1; static double acc; static long n, ticks; static double sum, mn = 99, mx;
    if (on < 0) { const char *e = getenv("RECOMPS_FPS_TICKS"); on = !e || atoi(e); }
    if (!on) { recomps_fps_tick = 1; return; }
    acc += step;
    if (acc >= 0.999) { recomps_fps_tick = 1; acc -= 1.0; if (acc >= 0.999) acc = 0; } else recomps_fps_tick = 0;
    {   static int log = -1; if (log < 0) { const char *e = getenv("RECOMPS_UNCAP_LOG"); log = e && atoi(e); }
        if (log) { n++; ticks += recomps_fps_tick; sum += step; if (step < mn) mn = step; if (step > mx) mx = step;
            if (n % 600 == 0) { printf("uncap: 600 clock updates: step mean %.3f min %.3f max %.3f, %ld ticks\n", sum / 600, mn, mx, ticks);
                fflush(stdout); sum = 0; mn = 99; mx = 0; ticks = 0; } } }
}
'''


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


INT_OP = [  # (regex on the defining line, replacement): the immediate 1 or 2 becomes tick x 1 or 2
    (re.compile(r'(c->x\[(\d+)\] \+ |c->x\[(\d+)\] - )([12])ULL\)'), lambda m: m.group(1) + '(uint64_t)recomps_fps_tick * %sULL)' % m.group(4)),
    (re.compile(r'~\(uint(32|64)_t\)([12])ULL, 1\)'), lambda m: '~(uint%s_t)(recomps_fps_tick * %sULL), 1)' % (m.group(1), m.group(2))),
    (re.compile(r'\(uint(32|64)_t\)([12])ULL, 0\)'), lambda m: '(uint%s_t)(recomps_fps_tick * %sULL), 0)' % (m.group(1), m.group(2))),
]
ASSIGN = lambda r: re.compile(r'^L_[0-9a-f]+: c->x\[%s\] = ' % r)
MOV = re.compile(r'= \(uint(?:32|64)_t\)\(0U? \| (?:\(uint32_t\))?c->x\[(\d+)\]\);')
VDEF = lambda r: re.compile(r'(c->v\[%s\]\.f\[0\] = t;|c->v\[%s\] = rdv|vzero\(&c->v\[%s\]\);|c->v\[%s\] = t[12]?;)' % (r, r, r, r))
CALL = re.compile(r'\bf_[0-9a-f]+\(c\)')


def patch_int(lines, lo, i):
    m = re.search(r'wr(?:8|16|32|64)\(a, \(uint\d+_t\)c->x\[(\d+)\]\)', lines[i])
    reg = m.group(1)
    for k in range(i - 1, max(lo, i - 14), -1):
        if not ASSIGN(reg).match(lines[k]):
            continue
        mv = MOV.search(lines[k])
        if mv:
            reg = mv.group(1); continue
        for rx, rep in INT_OP:
            new, n = rx.subn(rep, lines[k], count=1)
            if n:
                lines[k] = new
                return k
        raise SystemExit('no +/-1 on the line defining x%s for %s: %s' % (reg, lines[i][:16], lines[k].strip()))
    raise SystemExit('no definition of x%s found before %s' % (reg, lines[i][:16]))


def patch_f1(lines, lo, i, op=False):
    if op:
        j = i
    else:
        v = re.search(r'wrv\(a, &c->v\[(\d+)\]', lines[i]).group(1)
        j = next(k for k in range(i - 1, lo, -1) if VDEF(v).search(lines[k]))
    m = re.search(r'fnan32\(c->v\[(\d+)\]\.f\[0\] ([+-]) c->v\[(\d+)\]\.f\[0\]\)', lines[j])
    if not m:
        raise SystemExit('no float add at or before %s: %s' % (lines[i][:16], lines[j].strip()))
    for r in (m.group(1), m.group(3)):
        for k in range(j - 1, lo, -1):
            if int(r) < 8 and CALL.search(lines[k]):
                break
            if VDEF(r).search(lines[k]):
                if re.search(r'fnan32\(\(float\)-?1\.0\)', lines[k]):
                    old = 'c->v[%s].f[0]' % r
                    a, b = lines[j].split('fnan32(', 1)
                    lines[j] = a + 'fnan32(' + b.replace(old, '(%s * (float)recomps_fps_tick)' % old, 1)
                    return j
                break
    raise SystemExit('the float add before %s has no constant 1.0 operand: %s' % (lines[i][:16], lines[j].strip()))


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
    def lines_of(fn):
        name = find_file(gen, fn)
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
    # the counters
    done = 0
    for fn, lab, kind in sites:
        name, ls = lines_of(fn)
        lo, hi = func_range(ls, fn)
        i = next(k for k in range(lo, hi) if ls[k].startswith(lab + ':'))
        k = patch_int(ls, lo, i) if kind == 'int' else patch_f1(ls, lo, i, kind == 'f1op')
        print('%s %s %-4s %s' % (fn, lab, kind, ls[k][:150]))
        done += 1
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
            extra = HELP + (LIMIT if fname == present_file else '') + (TICK if fname == clock_file else '')
            assert text.count('#include "funcs.h"\n') == 1, fname
            write(os.path.join(out, fname), text.replace('#include "funcs.h"\n', '#include "funcs.h"\n' + extra, 1))
        elif fname == 'funcs.h':
            h = open(src, encoding='utf-8').read()
            if 'extern int recomps_fps_tick;' not in h:
                h = h.replace('\n', '\nextern int recomps_fps_tick;\n', 1)
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
    print('patched %d counters in %d files (+ clock, present); out: %s' % (done, len(files), out))


if __name__ == '__main__':
    main()
