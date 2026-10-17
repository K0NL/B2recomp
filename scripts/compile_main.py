"""Compile the lifted main (build/gen_eh) and link a test DLL against runtime stand-ins.

Mirrors ssbu-recomp's tools/lifter/build_opt.py (-ffp-contract=off -w -fno-strict-aliasing, + -mcx16 for rt_ext.h's
16-byte CAS), but at -O2 instead of their -O1 ([Recomps#1], finding 91: clang 9's -O1 inlines nothing, so every guest
memory access and flag helper was a call); functions over OPTNONE statements get __attribute__((optnone)); a file over the
time limit is retried with smaller thresholds, then compiled at -O0. A full build takes ~10 min with 16 jobs.

    python scripts/compile_main.py [jobs] [--link] [--opt=O2] [--gen=<dir>] [--obj=<dir>] [--toolchain=msvc|mingw]

--opt sets the optimisation level (default O2); --gen / --obj replace build/gen_eh / build/obj (to build variants);
--no-force-inline drops the always_inline on the header helpers (it matters at -O1 only).

Outputs build/obj/*.obj, build/obj/_report.txt, and with --link build/main_test.dll (stand-ins: scripts/rt_stubs.c).
Compiler: scripts/toolchain.py (TOOLCHAIN=mingw, the default: llvm-mingw clang 23, build/obj, OPTNONE 5000, ~5 min;
TOOLCHAIN=msvc: clang 9, build/obj_msvc, OPTNONE 20000, ~10 min).
"""
import concurrent.futures
import os
import re
import subprocess
import sys
import time

import toolchain

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..')
GEN = os.path.join(ROOT, 'build', 'gen_eh')
OBJ = toolchain.OBJ
INC = os.path.join(ROOT, 'runtime')                      # shared with the host runtime (same Ctx layout)
CLANG = toolchain.CLANG
# clang 9 runs only the always-inliner at -O1: without this every rd64 / wr64 / cond / adds32 of runtime.h, neon.h and
# rt_ext.h stays a call there (finding 91). Harmless at -O2, which inlines them anyway.
FORCE_INLINE = '-Dinline=inline __attribute__((always_inline))'
FLAGS = ['-ffp-contract=off', '-w', '-fno-strict-aliasing', '-mcx16', FORCE_INLINE, '-I', INC, '-I', GEN]
OPT = '-O2'
OPTNONE = toolchain.OPTNONE
TIME_LIMIT = 900                                         # -O2: the largest file takes ~300 s with 16 jobs
DEF = re.compile(r'^(?:static )?void (\w+)\((Ctx \*c(?:, uint64_t rp)?)\) \{$')


def big_functions(src, limit):
    out, cur, n = [], None, 0
    with open(src, encoding='latin-1') as f:
        for line in f:
            if cur is None:
                m = DEF.match(line.rstrip('\n'))
                if m:
                    cur, n = m.groups(), 0
            elif line.startswith('}'):
                if n > limit:
                    out.append((n, cur[0], cur[1]))
                cur = None
            else:
                n += 1
    return out


def run(cmd, limit):
    t = time.time()
    p = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    try:
        out, _ = p.communicate(timeout=limit)
        return p.returncode, out.decode('latin-1'), time.time() - t
    except subprocess.TimeoutExpired:
        subprocess.run(['taskkill', '/PID', str(p.pid), '/T', '/F'], capture_output=True)
        p.communicate()
        return None, '', time.time() - t


def compile_one(name):
    src, obj = os.path.join(GEN, name), os.path.join(OBJ, name[:-2] + '.obj')
    total = 0.0
    for limit in (OPTNONE, OPTNONE // 4, OPTNONE // 16):
        big = big_functions(src, limit)
        hdr = obj[:-4] + '.optnone.h'
        extra = []
        if big:
            with open(hdr, 'w') as f:
                f.write('#include "runtime.h"\n')
                f.writelines('void %s(%s) __attribute__((optnone, noinline));\n' % (fn, params) for _, fn, params in big)
            extra = ['-include', hdr]
        elif limit != OPTNONE:
            continue
        code, out, secs = run([CLANG, '-c', OPT] + FLAGS + extra + [src, '-o', obj], TIME_LIMIT)
        total += secs
        if code is not None:
            note = '%d fn(s) > %d stmts optnone' % (len(big), limit) if big else ''
            return name, code, out, total, note
    code, out, secs = run([CLANG, '-c', '-O0'] + FLAGS + [src, '-o', obj], None)
    return name, code, out, total + secs, 'WHOLE FILE -O0 (time limit hit 3x)'


def link():
    objs = sorted(os.path.join(OBJ, n) for n in os.listdir(OBJ) if n.endswith('.obj'))
    rsp = os.path.join(OBJ, '_objs.rsp')
    with open(rsp, 'w') as f:
        f.writelines('"%s"\n' % o.replace('\\', '/') for o in objs)
    dll = os.path.join(ROOT, 'build', 'main_test.dll' if toolchain.NAME == toolchain.DEFAULT else 'main_test_%s.dll' % toolchain.NAME)
    stubs = os.path.join(ROOT, 'scripts', 'rt_stubs.c')
    t = time.time()
    r = subprocess.run([CLANG, '-shared', '-O1', '-w', '-mcx16', '-I', INC, '-I', GEN, stubs, '@' + rsp, '-o', dll],
                       capture_output=True, text=True)
    print('link: exit %d in %.0fs' % (r.returncode, time.time() - t))
    print((r.stdout + r.stderr)[-6000:])
    if r.returncode == 0:
        print('%s: %.0f MB' % (dll, os.path.getsize(dll) / 2 ** 20))
    return r.returncode


def main():
    global GEN, OBJ, OPT, FLAGS
    for a in sys.argv[1:]:
        if a.startswith('--opt='):
            OPT = '-' + a[6:]
        elif a.startswith('--gen='):
            GEN = os.path.abspath(a[6:])
            FLAGS = FLAGS[:-1] + [GEN]
        elif a.startswith('--obj='):
            OBJ = os.path.abspath(a[6:])
        elif a == '--no-force-inline':
            FLAGS = [f for f in FLAGS if f != FORCE_INLINE]
    jobs = int(sys.argv[1]) if len(sys.argv) > 1 and sys.argv[1].isdigit() else 16
    os.makedirs(OBJ, exist_ok=True)
    names = sorted(n for n in os.listdir(GEN) if n.endswith('.c'))
    names.sort(key=lambda n: -os.path.getsize(os.path.join(GEN, n)))     # biggest first: better packing
    t0 = time.time()
    failed, notes, done = [], [], 0
    with concurrent.futures.ThreadPoolExecutor(jobs) as pool:
        for name, code, out, secs, note in pool.map(compile_one, names):
            done += 1
            if code:
                failed.append((name, out))
            if note or secs > 120:
                notes.append('%-12s %6.0fs  %s' % (name, secs, note))
            if done % 25 == 0 or code:
                print('[%d/%d] %.0fs%s' % (done, len(names), time.time() - t0, '  FAILED ' + name if code else ''), flush=True)
    with open(os.path.join(OBJ, '_report.txt'), 'w') as f:
        f.write('compiled %d files in %.0fs, %d failed\n' % (len(names), time.time() - t0, len(failed)))
        f.writelines(n + '\n' for n in notes)
        for name, out in failed:
            f.write('\n==== %s\n%s\n' % (name, out[-4000:]))
    print('compiled %d files in %.0fs (TOOLCHAIN=%s); %d failed; %d slow/fallback (see %s)'
          % (len(names), time.time() - t0, toolchain.NAME, len(failed), len(notes), os.path.join(OBJ, '_report.txt')))
    for name, out in failed[:5]:
        print('==== %s\n%s' % (name, out[-1500:]))
    if failed:
        return 1
    return link() if '--link' in sys.argv else 0


if __name__ == '__main__':
    sys.exit(main())
