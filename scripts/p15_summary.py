"""Summarise a prof15.c dump (SSBU_PROF15) for our build: per sampled thread its CPU ms per frame, the share of samples
by class, and the top functions by self and inclusive samples (as ms per frame of that thread's CPU).
    python scripts/p15_summary.py <dump> [top N] [--dir=<folder of the exe>]
Lifted functions are keyed by guest address (main 0x71..., our sdk 0x75...); host functions (module + offset) are named
with llvm-symbolizer from game_native.pdb / game.pdb beside the module (build/, or --dir for a build elsewhere such as
build/out_msvc). llvm-mingw's symbolizer reads the PDBs of both toolchains; the one beside the exe is passed with --pdb,
which an lld-linked (mingw) exe needs (notes/toolchain-mingw.md 3)."""
import collections
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
# llvm-symbolizer is found the way the build scripts find their tools (scripts/toolchain.py locate(): the environment
# variable, else deps/<name>; no machine paths): llvm-mingw's first, else clang 9's, else the one on PATH. Not through
# `import toolchain`, which stops when the compiler of the configured toolchain is missing.
def _symbolizer(env, sub):
    for d in (os.environ.get(env), os.path.join(ROOT, 'deps', sub)):
        if d and os.path.exists(os.path.join(d, 'bin', 'llvm-symbolizer.exe')):
            return os.path.join(d, 'bin', 'llvm-symbolizer.exe')
    return None
SYMBOLIZER_MINGW = _symbolizer('LLVM_MINGW', 'llvm-mingw')
SYMBOLIZER = SYMBOLIZER_MINGW or _symbolizer('LLVM_ROOT', 'llvm') or 'llvm-symbolizer'
WAIT_MODS = ('ntdll.dll', 'win32u.dll', 'kernelbase.dll')

MODDIR = next((os.path.abspath(a[6:]) for a in sys.argv[1:] if a.startswith('--dir=')), os.path.join(ROOT, 'build'))
args = [a for a in sys.argv[1:] if not a.startswith('--')]
dump = args[0]
topn = int(args[1]) if len(args) > 1 else 25
head, threads, fns = {}, {}, collections.defaultdict(list)
for line in open(dump, errors='replace'):
    m = re.match(r'\[p15\] window ([\d.]+) s, (\d+) rounds, (\d+) threads, .* frames (\d+)', line)
    if m: head = {'secs': float(m[1]), 'frames': int(m[4])}
    m = re.match(r'\[p15\] T (\d+) tid (\d+) cpu_ms ([\d.]+) samples (\d+) name (.*)', line)
    if m: threads[int(m[1])] = {'cpu': float(m[3]), 'samples': int(m[4]), 'name': m[5].strip()}
    m = re.match(r'\[p15\] F (\d+) G guest ([0-9a-f]+) (\d+) (\d+)', line)
    if m: fns[int(m[1])].append(('G', None, int(m[2], 16), int(m[3]), int(m[4]))); continue
    m = re.match(r'\[p15\] F (\d+) H (\S+) ([0-9a-f]+) (\d+) (\d+)', line)
    if m: fns[int(m[1])].append(('H', m[2], int(m[3], 16), int(m[4]), int(m[5])))

names = {}
def symbolize(mod, offs):
    exe = os.path.join(MODDIR, mod)
    if not os.path.exists(exe) or not offs: return
    base = 0x140000000 if mod.lower().endswith('.exe') else 0x180000000
    inp = '\n'.join('0x%x' % (base + o) for o in offs) + '\n'
    pdb = os.path.splitext(exe)[0] + '.pdb'
    try:
        # double-dash options: LLVM 23's symbolizer reads "-obj=" as short flags; only it knows --pdb (clang 9's finds an
        # MSVC-linked PDB by itself)
        new = SYMBOLIZER == SYMBOLIZER_MINGW
        out = subprocess.run([SYMBOLIZER, '--obj=' + exe, '--functions=short', '--inlining=false'] + (['--pdb=' + pdb] if new and os.path.exists(pdb) else []),
                             input=inp, capture_output=True, text=True, timeout=120).stdout
    except Exception:
        return
    blocks = [b for b in out.split('\n\n') if b.strip()]
    for o, b in zip(offs, blocks):
        names[(mod, o)] = b.split('\n')[0].strip()

by_mod = collections.defaultdict(set)
for lst in fns.values():
    for k, mod, a, s, i in lst:
        if k == 'H' and mod.lower() in ('game_native.exe', 'game.dll'): by_mod[mod].add(a)
for mod, offs in by_mod.items(): symbolize(mod, sorted(offs))

def label(k, mod, a):
    if k == 'G': return 'f_%x%s' % (a, ' (sdk)' if a >= 0x7500000000 else '')
    return '%s!%s' % (mod, names.get((mod, a), '+%x' % a))

def klass(k, mod, a):
    if k == 'G': return 'lifted sdk' if a >= 0x7500000000 else 'lifted game'
    m = mod.lower()
    if m in WAIT_MODS: return 'wait / system call'
    if m in ('game_native.exe', 'game.dll'):
        n = names.get((mod, a), '')
        if n.startswith('f_75'): return 'lifted sdk'
        if n.startswith('f_7'): return 'lifted game'
        return 'our runtime'
    if m.startswith('nv'): return 'NVIDIA driver'
    return m

frames = max(head.get('frames', 1), 1)
print('window %.2f s, %d frames (%.1f fps)' % (head.get('secs', 0), frames, frames / max(head.get('secs', 1), 1e-9)))
for t in sorted(threads):
    th = threads[t]
    lst = fns[t]
    busy = sum(s for k, mod, a, s, i in lst if not (k == 'H' and mod.lower() in WAIT_MODS))
    scale = th['cpu'] / frames / max(busy, 1)                    # ms per frame per non-wait sample
    print('\n=== T%d %s: %.2f ms CPU per frame, %d samples' % (t, th['name'], th['cpu'] / frames, th['samples']))
    cls = collections.Counter()
    for k, mod, a, s, i in lst: cls[klass(k, mod, a)] += s
    print('  classes: ' + ', '.join('%s %d%%' % (c, 100 * n // max(th['samples'], 1)) for c, n in cls.most_common()))
    print('  top self (ms/frame):')
    for k, mod, a, s, i in sorted(lst, key=lambda e: -e[3])[:topn]:
        if s == 0: break
        print('    %6.3f  %s' % (s * scale, label(k, mod, a)))
    print('  top inclusive (ms/frame):')
    for k, mod, a, s, i in sorted(lst, key=lambda e: -e[4])[:topn]:
        print('    %6.3f  %s' % (i * scale, label(k, mod, a)))
