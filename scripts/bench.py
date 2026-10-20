"""B2recomp standard test run: play a fixed scripted route (SSBU_SEQ) through the first fight, then score it.

    python scripts/bench.py [--exe <path>] [--name <run>] [--runs N] [--route <file>] [--seconds S]
                            [--baseline <file>] [--save-baseline] [--analyze <run dir> ...] [--env KEY=VALUE ...]
    python scripts/bench.py --name <batch> --runs 3 --ab <exe B> [--exe <exe A>]      # compare two builds
    python scripts/bench.py --analyze <A dirs> --vs <B dirs>                            # re-score an A/B batch

TO COMPARE TWO BUILDS USE --ab: the machine drifts between batches (the same build measured an hour apart: lifted CPU
11.03 -> 11.53 ms, render 4.75 -> 5.11 us per draw, finding 95), more than most changes we look for, so a build checked
against the saved baseline can be WORSE / BETTER for no reason of its own. --ab runs A B A B ... and judges B's mean
against A's with margins from that batch's own spread. The saved baseline is for a quick look at one build (health,
route, pictures, gross changes).

Each run goes to build/bench/<name>_<i>/ (stdout.txt, stderr.txt, userdata/ = a fresh copy of build/saves/after-prologue,
shots/frame_<n>.bmp). The window stays hidden and muted (SSBU_SHOW=0, SSBU_VOLUME=0), the keyboard and pads are ignored
(SSBU_SEQ is set), so it does not disturb a person at the machine; nothing else heavy may run at the same time: the
runner refuses to start while another game_native / clang / lld-link process is running (--wait waits instead).

Scoring (the method of findings 87-93): the game reports per 300-frame segment fps, 1 % low, draws per frame and CPU
ms per frame per thread. The fight window = the route's 'window' frames from the segment after the one in which the
route's last @draws step (the fight is drawn) passed (a run played by hand, --analyze without exit.txt: from the first
segment with > 600 draws to the end). The game's own 30 fps moments (finding 75: segments with > 600 draws at 29-30.5
fps; the dodging route has none) and their direct neighbours are left out; "heavy" = kept segments with > 600 draws.
Frames > 20 ms come from the hitch log (SSBU_HITCH=20), counted in the kept segments only. Screenshots every 1500
presented frames. Route check: every key step logs the draw count; inside the window a sample under 200 draws or 8
samples in a row within 20 draws (the CONTINUE? screen) mean the run left the fight (WORSE).

--save-baseline: the mean of the runs becomes tests/bench_baseline.json (numbers only) and the first run's screenshots
are copied to build/bench/baseline/shots (game images stay in build/, which is gitignored). With several runs the spread
(max - min) of every metric is stored and the noise margin of each metric is set from it (2 x spread, at least a floor).
Comparison: each metric is PASS when within its margin of the baseline, else BETTER / WORSE (direction per metric); with
--runs N > 1 the verdict on the metrics is that of the runs' mean, against margin / sqrt(N) (use 3 runs for A/B
decisions: one run of the same build can land anywhere inside the spread). Health (trap, host exception, Vulkan call failure, exit code) and the
screenshots are judged per run: pictures that are the same in every baseline run (the tutorial) pixel by pixel, but only
when the run reached the tutorial within 20 polls of the baseline (the pictures are taken at frame numbers, so a slower
chapter load shows another moment there); fight pictures differ between runs (enemies, camera, flashes) and are kept to
look at only. load_s = polls from the title screen to the drawn tutorial / 60: the route only waits fixed poll counts
in between, so it measures the chapter load (spread 0.05 s). The route is the same input in
every run, its steps begin at the same poll (within 2), but the game's random numbers are not fixed (SSBU_FIXED_SEED
knows only Smash's seed sites), so the enemies' moves differ: finding 94.
"""
import argparse, json, os, re, shutil, statistics, subprocess, sys, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.path.join(ROOT, 'build')
DEFAULT_ROUTE = os.path.join(ROOT, 'tests', 'route_first_fight.txt')
DEFAULT_BASELINE = os.path.join(ROOT, 'tests', 'bench_baseline.json')
BASE_SHOTS = os.path.join(BUILD, 'bench', 'baseline', 'shots')
SAVE = os.path.join(BUILD, 'saves', 'after-prologue')
LIFTED = ['main', 'MainJob1', 'MainJob2', 'MainDraw']
SHOT_EVERY = 1500                                         # presented frames between screenshots (frame_1500, 3000, ...)

# metric -> (higher is better?, minimum margin, format). The margin used is max(minimum, 2 x the measured spread).
METRICS = {
    'fps_window':   (True, 0.3, '%.1f'),     # mean fps of the kept fight window (all draw counts)
    'fps_heavy':    (True, 0.3, '%.1f'),     # mean fps of the heavy segments (> 600 draws), finding 92's number
    'low1_heavy':   (True, 2.0, '%.1f'),     # median of the heavy segments' 1 % lows
    'below57':      (False, 1, '%d'),        # heavy segments under 57 fps
    'slow_frames':  (False, 5, '%d'),        # frames > 20 ms in the kept window
    'cpu_render':   (False, 0.2, '%.2f'),    # render thread (host) CPU ms per frame, heavy segments
    'render_us_draw': (False, 0.15, '%.2f'), # the same per draw (us): follows the scene's draw count far less
    'cpu_lifted':   (False, 0.3, '%.2f'),    # main + MainJob1 + MainJob2 + MainDraw CPU ms per frame, heavy segments
    'heavy_segs':   (True, 3, '%d'),         # how many heavy segments the route produced (fewer = the route went elsewhere)
    'load_s':       (False, 0.3, '%.2f'),    # title screen -> tutorial drawn (fixed menu waits + loading the chapter), s
}


def read_route(path):
    """Route file: SSBU_SEQ steps, any whitespace; '#' starts a comment; 'seconds = N' sets the run length,
    'window = N' the number of presented frames scored from the fight's start."""
    steps, opt = [], {}
    for line in open(path, encoding='utf-8'):
        line = line.split('#', 1)[0].strip()
        m = re.match(r'(seconds|window)\s*=\s*(\d+)$', line)
        if m:
            opt[m[1]] = int(m[2])
        elif line:
            steps += line.split()
    if len(steps) > 256:
        sys.exit('route has %d steps; nnhid.c takes at most 256 (SEQ_MAX)' % len(steps))
    return ' '.join(steps), opt


def busy_processes(own_pid=None):
    out = subprocess.run(['tasklist', '/fo', 'csv', '/nh'], capture_output=True, text=True).stdout
    busy = []
    for line in out.splitlines():
        parts = [p.strip('"') for p in line.split('","')]
        # [Recomps#1] finding 134: llvm-mingw's compiler runs as x86_64-w64-mingw32-clang.exe and clang-23.exe (its linker as
        # ld.lld.exe), zig's as zig.exe: before, only clang.exe was matched, so a compile could run unnoticed during a run
        if len(parts) > 1 and re.match(r'(game_native\w*|ssbu_\w+|clang(-\d+)?|clang\+\+|[\w.-]+-mingw32\w*-clang(\+\+)?|ld\.lld|lld-link|link|zig|analyzeHeadless)\.exe$', parts[0], re.I):
            if own_pid is None or int(parts[1]) != own_pid:
                busy.append('%s (%s)' % (parts[0], parts[1]))
    return busy


def run_once(exe, run_dir, seq, seconds, wait, extra_env=None):
    while True:
        busy = busy_processes()
        if not busy:
            break
        if not wait:
            sys.exit('not measuring: other heavy processes are running: %s (--wait to wait for them)' % ', '.join(busy))
        print('waiting for: %s' % ', '.join(busy)); time.sleep(20)
    if os.path.exists(run_dir):
        shutil.rmtree(run_dir)
    shots = os.path.join(run_dir, 'shots')
    os.makedirs(shots)
    shutil.copytree(SAVE, os.path.join(run_dir, 'userdata'))
    env = {k: v for k, v in os.environ.items() if not k.startswith(('SSBU_', 'RECOMPS_'))}
    env.update({
        'SSBU_ROMFS': os.path.join(ROOT, 'targets', 'romfs'),
        'SSBU_USERDATA': os.path.join(run_dir, 'userdata'),
        'SSBU_TRACE': os.path.join(run_dir, 'boot_trace.txt'),
        'SSBU_SHOW': '0', 'SSBU_VOLUME': '0', 'SSBU_FOCUS_NOTIFY': '0',
        'SSBU_SEQ': seq, 'SSBU_HITCH': '20',
        # the settings files by name: the runtime looks for <exe folder>\..\config, so an exe outside build\ (build\out_msvc)
        # would read or write a defaults file of its own (findings 95/96 compared resolution scale 1.0 against 2.0: finding 97)
        'SSBU_GRAPHICS_CONFIG': os.path.join(ROOT, 'config', 'graphics.ini'),
        'SSBU_CONFIG': os.path.join(ROOT, 'config', 'controllers.ini'),
        'SSBU_SHOT_EVERY': str(SHOT_EVERY), 'SSBU_SHOT_DIR': shots,
        # the player's RomFS mods (mods\, e.g. button prompts) change the pictures the screenshot checks compare: off
        # (finding 109; --env SSBU_MODS=<folder> tests a mod)
        'SSBU_MODS': 'none',
    })
    fonts = os.path.join(ROOT, 'targets', 'sysdata', 'fonts')
    if os.path.isdir(fonts):
        env['SSBU_FONTS'] = fonts
    env.update(extra_env or {})                           # --env: test switches for a diagnosis run (not for baselines)
    t0 = time.time()
    with open(os.path.join(run_dir, 'stdout.txt'), 'wb') as out, open(os.path.join(run_dir, 'stderr.txt'), 'wb') as err:
        p = subprocess.Popen([exe, os.path.join(ROOT, 'targets', 'exefs', 'main.elf'), str(seconds)], cwd=BUILD, env=env,
                             stdout=out, stderr=err)
        # watched every 10 s: another project's build that starts and ends inside our run starves the game just the same
        # (finding 97: 16 compile jobs gave 5-6 gaps of seconds without a frame per run, in both builds)
        busy, code = set(), None
        while code is None:
            try:
                code = p.wait(10)
            except subprocess.TimeoutExpired:
                busy |= {b.split(' (')[0] for b in busy_processes(p.pid)}
                if time.time() - t0 > seconds + 120:
                    p.kill(); code = p.wait(); print('KILLED: no exit within the run time + 120 s')
    code &= 0xffffffff
    busy = sorted(busy | {b.split(' (')[0] for b in busy_processes()})
    with open(os.path.join(run_dir, 'exit.txt'), 'w') as f:
        f.write('0x%08X %.0f s%s\n' % (code, time.time() - t0, ' DISTURBED by: ' + ', '.join(busy) if busy else ''))
    if busy:
        print('WARNING: other heavy processes ran during this run: %s; its numbers are not comparable' % ', '.join(busy))
    return code


def run_measured(exe, run_dir, seq, seconds, wait, extra_env=None):
    """run_once; with --wait a run that another build disturbed is made again (at most 3 tries)."""
    for attempt in range(3):
        code = run_once(exe, run_dir, seq, seconds, wait, extra_env)
        if not wait or 'DISTURBED' not in open(os.path.join(run_dir, 'exit.txt')).read():
            return code
        if attempt < 2:
            print('disturbed: running %s again (try %d of 3)' % (os.path.basename(run_dir), attempt + 2)); sys.stdout.flush()
    print('disturbed 3 times: %s stays marked DISTURBED' % os.path.basename(run_dir))
    return code


def segments(path):
    """Per 300-frame segment: start frame, fps, 1 % low, draws per frame, CPU ms per frame by thread name."""
    out, cur = [], {}
    for n, line in enumerate(open(path, encoding='utf-8', errors='replace')):
        m = re.match(r'nvn_perf: frames (\d+)\.\.(\d+) frame time ms: avg ([\d.]+).*fps avg ([\d.]+), 1% low ([\d.]+)', line)
        if m:
            cur = {'a': int(m[1]), 'b': int(m[2]), 'fps': float(m[4]), 'low': float(m[5]), 'line': n}
            continue
        m = re.match(r'nvn_vk: frames (\d+)\.\.(\d+): ([\d.]+) fps, (\d+) draws per frame', line)
        if m and cur and cur['a'] == int(m[1]):
            cur['draws'] = int(m[4])
            continue
        m = re.match(r'nvn_perf: cpu ms/frame by thread \(all ([\d.]+)\): (.*)', line)
        if m and cur and 'draws' in cur:
            th = {}
            for name, v in re.findall(r'(\S+?)\[\d+\] ([\d.]+)', m[2]):
                th.setdefault(name, float(v))
            cur['th'] = th
            out.append(cur); cur = {}
    return out


def analyze(run_dir, fight_step=None, window_frames=None):
    path = os.path.join(run_dir, 'stdout.txt')
    text = open(path, encoding='utf-8', errors='replace').read()
    segs = segments(path)
    r = {'run': os.path.basename(run_dir)}
    # the fight window: the segments reported after the route's fight-start step (the segment it falls in is left out);
    # without a route step (a run played by hand): from the first segment with > 600 draws
    start = None
    if fight_step is not None:
        lines = text.splitlines()
        hit = [n for n, l in enumerate(lines) if re.match(r'input sequence: poll \d+ step %d: \d+\.\.\d+ draws' % fight_step, l)]
        start = hit[0] if hit else len(lines)
        later = [i for i, s in enumerate(segs) if s['line'] > start]
        first = later[1] if len(later) > 1 else None
    else:
        first = next((i for i, s in enumerate(segs) if s['draws'] > 600), None)
    window = segs[first:] if first is not None else []
    if window and window_frames:                          # a fixed length from the fight's start (the route's 'window')
        window = [s for s in window if s['b'] <= window[0]['a'] + window_frames]
    # did the route stay in the fight? every key step logs the draw count of the last frame: after the fight began, a
    # sample under 200 draws is a game over, a menu or a loading screen (the fight itself draws 500-1400)
    # (window only). Also a still screen (the game-over / CONTINUE? screen draws ~678): 8 or more samples in a row within
    # 20 draws of each other; the fight's samples jump by hundreds from one step to the next
    if start is not None and window:
        tail = '\n'.join(text.splitlines()[start:window[-1]['line'] + 1])
        samples = [int(m[1]) for m in re.finditer(r'^input sequence: poll \d+ step \d+ keys \S+ for \d+ \((\d+) draws\)', tail, re.M)]
        out = {i for i, d in enumerate(samples) if d < 200}
        for i in range(len(samples) - 7):
            if max(samples[i:i + 8]) - min(samples[i:i + 8]) <= 20:
                out |= set(range(i, i + 8))
        r['left_fight'] = len(out)
        r['fight_samples'] = len(samples)
    g30 = {s['a'] for s in window if 29.0 <= s['fps'] <= 30.5 and s['draws'] > 600}     # as findings 92/93 (fight.py)
    kept = [s for s in window if not any(abs(s['a'] - a) <= 300 for a in g30)]
    heavy = [s for s in kept if s['draws'] > 600]
    r['window'] = '%d..%d' % (window[0]['a'], window[-1]['b']) if window else '-'
    r['game30'] = len(g30)
    r['heavy_segs'] = len(heavy)
    if kept:
        r['fps_window'] = 300 * len(kept) / sum(300 / s['fps'] for s in kept if s['fps'] > 0)
    if heavy:
        r['fps_heavy'] = statistics.mean(s['fps'] for s in heavy)
        r['low1_heavy'] = statistics.median(s['low'] for s in heavy)
        r['below57'] = sum(s['fps'] < 57 for s in heavy)
        r['cpu_render'] = statistics.mean(s['th'].get('render(host)', 0) for s in heavy)
        r['cpu_lifted'] = statistics.mean(sum(s['th'].get(t, 0) for t in LIFTED) for s in heavy)
        r['draws_heavy'] = statistics.mean(s['draws'] for s in heavy)
        r['render_us_draw'] = 1000 * r['cpu_render'] / r['draws_heavy']
    ranges = [(s['a'], s['b']) for s in kept]
    slow = [int(m[1]) for m in re.finditer(r'^hitch: frame (\d+) ([\d.]+) ms', text, re.M)]
    r['slow_frames'] = sum(any(a <= f < b for a, b in ranges) for f in slow)
    # health
    r['traps'] = len(re.findall(r'^GUEST TRAP:', text, re.M))
    r['host_exceptions'] = len(re.findall(r'HOST EXCEPTION', text))
    m = re.search(r'(\d+) frames presented, (\d+) Vulkan call failures', text)
    r['vk_failures'] = int(m[2]) if m else None
    m = re.search(r'imports reached: (\d+) +\(implemented: (\d+), still stubs: (\d+)\)', text)
    r['imports'] = '%s/%s' % (m[2], m[1]) if m else '-'
    r['stopped_at_limit'] = 'still running after' in text
    ex = os.path.join(run_dir, 'exit.txt')
    r['exit'] = open(ex).read().split()[0] if os.path.exists(ex) else '-'
    r['disturbed'] = os.path.exists(ex) and 'DISTURBED' in open(ex).read()
    steps = re.findall(r'^input sequence: poll (\d+) step (\d+)', text, re.M)
    total = re.search(r'^input sequence: (\d+) steps', text, re.M)
    r['route_steps'] = '%d/%s' % (int(steps[-1][1]) + 1 if steps else 0, total[1] if total else '?')
    r['route_polls'] = {int(s): int(p) for p, s in steps}
    r['shots'] = sorted(f for f in os.listdir(os.path.join(run_dir, 'shots')) if f.endswith('.bmp')) \
        if os.path.isdir(os.path.join(run_dir, 'shots')) else []
    return r


def image_diff(a, b):
    """Mean absolute difference (0-255) and % of pixels differing by more than 24 in any channel."""
    from PIL import Image, ImageChops
    ia, ib = Image.open(a).convert('RGB'), Image.open(b).convert('RGB')
    if ia.size != ib.size:
        ib = ib.resize(ia.size)
    d = ImageChops.difference(ia, ib)
    hist = d.convert('L').histogram()
    n = ia.size[0] * ia.size[1]
    mean = sum(i * c for i, c in enumerate(hist)) / n
    big = d.point(lambda v: 255 if v > 24 else 0).convert('L').point(lambda v: 255 if v else 0)
    pct = 100 * big.histogram()[255] / n
    return mean, pct


def shot_diffs(run_dir, ref_dir):
    out = {}
    if not os.path.isdir(ref_dir):
        return out
    for f in sorted(os.listdir(ref_dir)):
        p = os.path.join(run_dir, 'shots', f)
        if f.endswith('.bmp') and os.path.exists(p):
            out[f] = image_diff(os.path.join(ref_dir, f), p)
    return out


def fmt(name, v):
    return '-' if v is None else METRICS[name][2] % v if name in METRICS else str(v)


def judge(r, base, n=1):
    """Metric verdicts of one run, or of the mean of n runs: the stored margin is for one run (2 x the spread of single
    runs); a mean of n runs varies about 1/sqrt(n) as much, so its margin is margin / sqrt(n), never below the floor."""
    verdicts = []
    for name, (higher, floor, _) in METRICS.items():
        v = r.get(name)
        line = '   %-14s %8s' % (name, fmt(name, v))
        if base and name in base['metrics'] and v is not None:
            b, margin = base['metrics'][name], max(floor, base['margin'][name] / n ** 0.5)
            delta = v - b
            verdict = 'PASS' if abs(delta) <= margin else ('BETTER' if (delta > 0) == higher else 'WORSE')
            verdicts.append(verdict)
            line += '   baseline %8s  margin %6s  %-6s' % (fmt(name, b), fmt(name, margin), verdict)
        print(line)
    return verdicts


def report(r, base):
    print('\n== %s: window frames %s, game-30fps segments %d, heavy %d (%.0f draws), route %s, exit %s%s' % (
        r['run'], r['window'], r['game30'], r['heavy_segs'], r.get('draws_heavy', 0), r['route_steps'], r['exit'],
        ' (stopped at the time limit)' if r['stopped_at_limit'] else '') + ('  DISTURBED: another game / build ran' if r['disturbed'] else ''))
    print('   health: traps %d, host exceptions %d, Vulkan call failures %s, imports %s%s' % (
        r['traps'], r['host_exceptions'], r['vk_failures'], r['imports'],
        ', route steps outside the fight %d of %d' % (r['left_fight'], r['fight_samples']) if 'left_fight' in r else ''))
    metric_verdicts, verdicts = judge(r, base), []
    if r['traps'] or r['host_exceptions'] or r['vk_failures'] or r['exit'] not in ('0x00000000',):
        verdicts.append('WORSE'); print('   HEALTH: WORSE (trap, exception, Vulkan failure or exit code != 0)')
    if r.get('left_fight'):
        verdicts.append('WORSE'); print('   ROUTE: WORSE: %d key steps found the fight gone (game over, menu or loading): the run went elsewhere' % r['left_fight'])
    if base:
        diffs = shot_diffs(os.path.join(BUILD, 'bench', r['run']), BASE_SHOTS)
        lim = base.get('shot_margin', {})
        # the pictures are taken at frame numbers: if this run reached the tutorial at another poll than the baseline
        # (a slower or faster chapter load), the same frame number shows another moment, so they cannot be compared
        shift = r.get('tutorial_poll', 0) - base['tutorial_poll'] if r.get('tutorial_poll') and base.get('tutorial_poll') else 0
        if lim and abs(shift) > 20:
            print('   shots: this run reached the tutorial %+d polls from the baseline (see load_s): the frame-numbered pictures'
                  ' show other moments, not judged' % shift)
            lim = {}
        for f, (mean, pct) in diffs.items():
            line = '   shot %-16s mean diff %5.1f  pixels > 24: %5.1f %%' % (f, mean, pct)
            if f in lim:                                  # a picture that is the same in every run: compared pixel by pixel
                verdict = 'PASS' if mean <= lim[f] else 'WORSE'
                line += '  (limit %.1f) %s' % (lim[f], verdict)
            elif f in base.get('shot_margin', {}):
                line += '  (timeline shifted: not judged)'
                verdict = ''
            else:                                         # a fight picture: differs between runs anyway, to look at only
                line += '  (fight: not judged)'
                verdict = ''
            if verdict:
                verdicts.append(verdict)
            print(line)
        missing = [f for f in os.listdir(BASE_SHOTS) if f.endswith('.bmp') and f not in diffs] if os.path.isdir(BASE_SHOTS) else []
        if missing:
            print('   shots missing against the baseline: %s' % ' '.join(missing))
    return metric_verdicts, verdicts                      # (metrics, health + screenshots)


def score(dirs, seq, opt, analyzing):
    steps = seq.split()
    fight_step = max((i for i, s in enumerate(steps) if s.startswith('@draws:')), default=None)
    scripted = [not analyzing or os.path.exists(os.path.join(d, 'exit.txt')) for d in dirs]
    results = [analyze(d, fight_step if s else None, opt.get('window') if s else None) for d, s in zip(dirs, scripted)]
    # load_s: the route's first two @draws steps are the title screen and the drawn tutorial; the polls where each screen
    # APPEARED (the step's poll minus its /<n> run length) are 900 apart in every run of both toolchains (finding 95):
    # the chapter loads behind the touch-controls page, which waits for the route's A press, so this only moves when
    # loading outlasts the route's waits (a guard, not a load-time measurement). tutorial_poll aligns the pictures.
    draws_steps = [i for i, s in enumerate(steps) if s.startswith('@draws:')]
    need = {i: int(steps[i].split('/')[1]) if '/' in steps[i] else 20 for i in draws_steps}
    for r in results:
        p = r['route_polls']
        if len(draws_steps) >= 2 and draws_steps[0] in p and draws_steps[1] in p:
            t0, t1 = p[draws_steps[0]] - need[draws_steps[0]], p[draws_steps[1]] - need[draws_steps[1]]
            r['tutorial_poll'] = t1
            r['load_s'] = (t1 - t0) / 60.0
    return results


def compare_ab(res_a, res_b):
    """B's mean against A's mean, both measured in one interleaved batch. The noise of a difference of two means of n
    runs: one run varies about +-spread (max - min of single runs), a mean of n about spread / sqrt(n), a difference of two
    such means sqrt(2) times that; the margin is 2 x that (as for one run against the baseline), never below the floor."""
    n = min(len(res_a), len(res_b))
    print('\n== B (%s) against A (%s): mean of %d runs each, same batch' % (
        ' '.join(r['run'] for r in res_b), ' '.join(r['run'] for r in res_a), n))
    verdicts = []
    for name, (higher, floor, f) in METRICS.items():
        va = [r[name] for r in res_a if r.get(name) is not None]
        vb = [r[name] for r in res_b if r.get(name) is not None]
        if not va or not vb:
            continue
        ma, mb = statistics.mean(va), statistics.mean(vb)
        spread = ((max(va) - min(va)) + (max(vb) - min(vb))) / 2
        margin = max(floor, 2 * spread * 2 ** 0.5 / n ** 0.5) if n > 1 else None
        delta = mb - ma
        verdict = '' if margin is None else 'PASS' if abs(delta) <= margin else ('BETTER' if (delta > 0) == higher else 'WORSE')
        if verdict:
            verdicts.append(verdict)
        ff = '%.1f' if '%d' in f else f                  # means of counts are fractions
        print('   %-14s A %8s  B %8s  B-A %8s (%+5.1f %%)  margin %6s  %s' % (
            name, ff % ma, ff % mb, ('+' + ff if delta >= 0 else ff) % delta, 100 * delta / ma if ma else 0,
            '-' if margin is None else ff % margin, verdict))
    return verdicts


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--exe', default=os.path.join(BUILD, 'game_native.exe'))
    ap.add_argument('--name', default='run')
    ap.add_argument('--runs', type=int, default=1)
    ap.add_argument('--route', default=DEFAULT_ROUTE)
    ap.add_argument('--seconds', type=int)
    ap.add_argument('--baseline', default=DEFAULT_BASELINE)
    ap.add_argument('--save-baseline', action='store_true')
    ap.add_argument('--wait', action='store_true', help='wait for other game runs / builds instead of refusing')
    ap.add_argument('--analyze', nargs='*', help='score existing run folders instead of running')
    ap.add_argument('--env', action='append', default=[], metavar='KEY=VALUE',
                    help='an extra environment variable for the game (diagnosis runs; repeatable)')
    ap.add_argument('--ab', metavar='EXE_B', help='A/B: run --exe (A) and EXE_B alternately, --runs times each, and '
                    'compare B with A (the way to compare two builds: the machine drifts between batches, finding 95)')
    ap.add_argument('--vs', nargs='*', help='with --analyze <A dirs>: re-score an A/B batch against these B dirs')
    ap.add_argument('--save', help='the userdata folder copied into each run (default build/saves/after-prologue; '
                    'another save only for diagnosis routes, not for baselines)')
    a = ap.parse_args()
    a.exe = os.path.abspath(a.exe)
    if a.save:
        global SAVE
        SAVE = os.path.abspath(a.save)

    seq, opt = read_route(a.route)
    seconds = a.seconds or opt.get('seconds') or 300
    env = dict(e.split('=', 1) for e in a.env)
    as_dirs = lambda names: [d if os.path.isdir(d) else os.path.join(BUILD, 'bench', d) for d in names]
    if a.ab or a.vs:                                      # A/B: two builds in one batch, compared with each other
        if a.vs:
            dirs_a, dirs_b = as_dirs(a.analyze or []), as_dirs(a.vs)
        else:
            exe_b, dirs_a, dirs_b = os.path.abspath(a.ab), [], []
            for i in range(a.runs):                       # interleaved A B A B ...: both see the same drift of the machine
                for exe, tag, lst in ((a.exe, 'a', dirs_a), (exe_b, 'b', dirs_b)):
                    d = os.path.join(BUILD, 'bench', '%s_%s%d' % (a.name, tag, i + 1))
                    print('run %d/%d %s -> %s (%d s, exe %s)' % (i + 1, a.runs, tag.upper(), d, seconds, exe)); sys.stdout.flush()
                    run_measured(exe, d, seq, seconds, a.wait, env)
                    lst.append(d)
        base = json.load(open(a.baseline)) if os.path.exists(a.baseline) else None
        res_a, res_b = score(dirs_a, seq, opt, a.analyze is not None), score(dirs_b, seq, opt, a.analyze is not None)
        worst = set()
        for r in res_a + res_b:                           # health, route and pictures per run (pictures: the saved baseline's)
            worst |= set(report(r, base)[1])
        worst |= set(compare_ab(res_a, res_b))
        print('\nOVERALL (B against A, plus health / route / pictures of every run): %s' % (
            'WORSE' if 'WORSE' in worst else 'BETTER' if 'BETTER' in worst else 'PASS'))
        return
    if a.analyze:
        dirs = as_dirs(a.analyze)
    else:
        dirs = []
        for i in range(a.runs):
            d = os.path.join(BUILD, 'bench', '%s_%d' % (a.name, i + 1))
            print('run %d/%d -> %s (%d s, exe %s)' % (i + 1, a.runs, d, seconds, a.exe)); sys.stdout.flush()
            run_measured(a.exe, d, seq, seconds, a.wait, env)
            dirs.append(d)
    results = score(dirs, seq, opt, a.analyze is not None)

    base = json.load(open(a.baseline)) if os.path.exists(a.baseline) and not a.save_baseline else None
    worst = set()
    for r in results:
        mv, other = report(r, base)
        worst |= set(other) | (set(mv) if len(results) == 1 else set())
    if base and len(results) > 1:                         # several runs: the verdict on the metrics is the mean's
        mean = {n: statistics.mean(r[n] for r in results if r.get(n) is not None) for n in METRICS
                if any(r.get(n) is not None for r in results)}
        print('\n== mean of %d runs against the baseline (the overall verdict uses these):' % len(results))
        worst |= set(judge(mean, base, len(results)))

    if len(results) > 1:
        print('\n== spread over %d runs (max - min):' % len(results))
        for name in METRICS:
            vals = [r[name] for r in results if r.get(name) is not None]
            if vals:
                print('   %-14s mean %8s  spread %8s  (%s)' % (name, fmt(name, statistics.mean(vals)), fmt(name, max(vals) - min(vals)),
                                                         ' '.join(fmt(name, v) for v in vals)))
        polls = [r['route_polls'] for r in results]
        common = sorted(set.intersection(*(set(p) for p in polls)))
        if common:
            dev = max(max(p[s] for p in polls) - min(p[s] for p in polls) for s in common)
            print('   route: %d steps reached by every run; largest difference of the poll a step began at: %d polls' % (len(common), dev))

    if a.save_baseline:
        metrics, spread, margin = {}, {}, {}
        for name, (_, floor, _) in METRICS.items():
            vals = [r[name] for r in results if r.get(name) is not None]
            if not vals:
                continue
            metrics[name] = round(statistics.mean(vals), 3)
            spread[name] = round(max(vals) - min(vals), 3)
            margin[name] = round(max(floor, 2 * spread[name]), 3)
        # screenshots: the first run's are the reference. A picture whose runs differ by at most 15 (mean absolute
        # difference; the tutorial ones: 2-12) gets a pixel limit of 1.5 x that + 2; the fight pictures differ by 30-110
        # between runs (enemies, camera) and even their brightness swings by 70+ (flashes, Witch Time), so they are kept to
        # look at but not judged
        if os.path.isdir(BASE_SHOTS):
            shutil.rmtree(BASE_SHOTS)
        os.makedirs(BASE_SHOTS)
        for f in results[0]['shots']:
            shutil.copy(os.path.join(dirs[0], 'shots', f), BASE_SHOTS)
        shot_margin, others = {}, [shot_diffs(x, BASE_SHOTS) for x in dirs[1:]]
        for f in results[0]['shots']:
            ds = [d[f][0] for d in others if f in d]
            if ds and max(ds) <= 15:
                shot_margin[f] = round(max(12, 1.5 * max(ds) + 2), 1)     # floor: 5 runs gave 6.8 once, a run of the same picture 7.3
        out = {'date': time.strftime('%Y-%m-%d'), 'exe': os.path.relpath(a.exe, ROOT), 'route': os.path.relpath(a.route, ROOT),
               'seconds': seconds, 'window': opt.get('window'), 'runs': [r['run'] for r in results], 'metrics': metrics, 'spread': spread, 'margin': margin,
               'shot_margin': shot_margin,
               'tutorial_poll': round(statistics.mean(r['tutorial_poll'] for r in results if r.get('tutorial_poll')), 1)
               if any(r.get('tutorial_poll') for r in results) else None, 'note': 'made by scripts/bench.py --save-baseline; screenshots in build/bench/baseline/shots'}
        with open(a.baseline, 'w') as f:
            json.dump(out, f, indent=1)
        print('\nbaseline saved: %s (+ %d screenshots in %s)' % (a.baseline, len(results[0]['shots']), BASE_SHOTS))
    elif base:
        print('\nOVERALL: %s' % ('WORSE' if 'WORSE' in worst else 'BETTER' if 'BETTER' in worst else 'PASS'))


if __name__ == '__main__':
    main()
