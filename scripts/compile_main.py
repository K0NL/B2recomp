"""Compile the lifted main (build/gen_eh) and link a test DLL against runtime stand-ins.

Mirrors ssbu-recomp's tools/lifter/build_opt.py (-ffp-contract=off -w -fno-strict-aliasing, + -mcx16 for rt_ext.h's
16-byte CAS), but at -O2 instead of their -O1 ([Recomps#1], finding 91: clang 9's -O1 inlines nothing, so every guest
memory access and flag helper was a call); functions over OPTNONE statements get __attribute__((optnone)); a file over the
time limit is retried with smaller thresholds, then compiled at -O0. A full build takes ~10 min with 16 jobs.

    python scripts/compile_main.py [jobs] [--link] [--opt=O2] [--gen=<dir>] [--obj=<dir>] [--toolchain=msvc|mingw]
                                   [--force] [--adopt]

--opt sets the optimisation level (default O2); --gen / --obj replace build/gen_eh / build/obj (to build variants);
--no-force-inline drops the always_inline on the header helpers (it matters at -O1 only).

Up-to-date check ([Recomps#1] 2026-10-17, so that switching one code mod rebuilds in well under a minute): a C file is
compiled again only when it changed. <obj>/_stamps.json keeps, per file, the SHA-256 of the C source, the times of
source and object, and a hash of everything else its object depends on: compiler (path, size, time), level, flags,
optnone threshold and the contents of the headers the lifted files include (runtime.h, rt_ext.h, fpexact.h, neon.h...;
found by following #include "..."). funcs.h, the prototype list the lifter rewrites on every run, is compared with the
copy kept from the last compile (<obj>/_funcs.h.prev): an added prototype cannot change an unchanged file's object, a
removed or changed one recompiles the files that name it. The lifter rewrites only C files whose text changed
(recomp_eh.py), so a file's time decides at once; a changed time with the same text costs one hash. Objects without a
C file are deleted (the link takes every .obj). --force compiles everything; --adopt records the existing objects as
up to date without compiling (only when you know they were made from these sources with these settings).

Outputs build/obj/*.obj, build/obj/_report.txt, and with --link build/main_test.dll (stand-ins: scripts/rt_stubs.c).
Compiler: scripts/toolchain.py (TOOLCHAIN=mingw, the default: llvm-mingw clang 23, build/obj, OPTNONE 5000, ~5 min;
TOOLCHAIN=msvc: clang 9, build/obj_msvc, OPTNONE 20000, ~10 min; TOOLCHAIN=linux [Recomps#1]: build-linux/obj/*.o and
build-linux/main_test.so, notes/linux-port-plan.md step 3).
"""
import concurrent.futures
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import time

import toolchain

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..')
GEN = os.path.join(ROOT, 'build', 'gen_eh')
OBJ = toolchain.OBJ
INC = os.path.join(ROOT, 'runtime')                      # shared with the host runtime (same Ctx layout)
CLANG = toolchain.CLANG
CC = toolchain.CC                                        # [Recomps#1] the command's first words: [clang], or zig cc -target ... (linux)
EXT = toolchain.OBJEXT                                   # .obj; .o for linux
# clang 9 runs only the always-inliner at -O1: without this every rd64 / wr64 / cond / adds32 of runtime.h, neon.h and
# rt_ext.h stays a call there (finding 91). Harmless at -O2, which inlines them anyway.
FORCE_INLINE = '-Dinline=inline __attribute__((always_inline))'
FLAGS = ['-ffp-contract=off', '-w', '-fno-strict-aliasing', FORCE_INLINE, '-I', INC, '-I', GEN]
OPT = '-O2'
OPTNONE = toolchain.OPTNONE
TIME_LIMIT = 900                                         # -O2: the largest file takes ~300 s with 16 jobs
DEF = re.compile(r'^(?:static )?void (\w+)\((Ctx \*c(?:, uint64_t rp)?)\) \{$')
RULES = 1                                                # bump when compile_one's choices change (invalidates every stamp)
INCLUDE = re.compile(r'^\s*#\s*include\s+"([^"]+)"', re.M)
PROTO = re.compile(r'^void (\w+)\(', re.M)


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for block in iter(lambda: f.read(1 << 20), b''):
            h.update(block)
    return h.hexdigest()


def config_hash(names):
    """Everything an object depends on besides its C file and funcs.h: compiler, level, flags, rules, included headers."""
    h = hashlib.sha256()
    st = os.stat(CLANG)
    h.update(json.dumps([RULES, CLANG, st.st_size, st.st_mtime_ns, OPT, FLAGS, OPTNONE, toolchain.NAME] +
                        ([CC, toolchain.EXTRA, toolchain.LIFTED_EXTRA] if toolchain.LINUX else [])).encode())   # Windows stamps stay valid
    todo, seen = set(), {}
    for n in names:                                      # the lifted files' own includes are in their first lines
        with open(os.path.join(GEN, n), encoding='latin-1') as f:
            todo.update(INCLUDE.findall(f.read(400)))
    while todo:
        inc = todo.pop()
        if inc in seen or inc == 'funcs.h':
            continue
        path = next((os.path.join(d, inc) for d in (INC, GEN) if os.path.isfile(os.path.join(d, inc))), None)
        if path is None:
            raise SystemExit('header %s (included by the lifted files) not found in %s or %s' % (inc, INC, GEN))
        with open(path, 'rb') as f:
            data = f.read()
        seen[inc] = hashlib.sha256(data).digest()
        todo.update(INCLUDE.findall(data.decode('latin-1')))
    for inc in sorted(seen):                             # sorted: a set's order changes from run to run
        h.update(inc.encode() + b'\0' + seen[inc])
    return h.hexdigest(), sorted(seen)


def removed_prototypes():
    """Names whose funcs.h line is gone or changed since the last compile; None when no earlier copy exists."""
    prev = os.path.join(OBJ, '_funcs.h.prev')
    if not os.path.isfile(prev):
        return None
    with open(prev, encoding='latin-1') as f:
        old = set(f.read().splitlines())
    with open(os.path.join(GEN, 'funcs.h'), encoding='latin-1') as f:
        new = set(f.read().splitlines())
    return set(PROTO.findall('\n'.join(old - new)))


def up_to_date(name, stamps, cfg, gone):
    """The stamp of name when its object needs no compile, else None (stamps[name] gets the new times on a match)."""
    s = stamps.get(name)
    src, obj = os.path.join(GEN, name), os.path.join(OBJ, name[:-2] + EXT)
    if not s or s.get('cfg') != cfg or not os.path.isfile(obj) or os.stat(obj).st_mtime_ns != s.get('obj'):
        return None
    st = os.stat(src)
    if (st.st_size, st.st_mtime_ns) != (s.get('size'), s.get('mtime')):
        if sha256_file(src) != s.get('src'):
            return None
        s['size'], s['mtime'] = st.st_size, st.st_mtime_ns
    if gone:                                             # a prototype this file may use was removed or changed
        with open(src, encoding='latin-1') as f:
            if gone.search(f.read()):
                return None
    return s


def stamp(name, cfg):
    src, obj = os.path.join(GEN, name), os.path.join(OBJ, name[:-2] + EXT)
    st = os.stat(src)
    return {'src': sha256_file(src), 'size': st.st_size, 'mtime': st.st_mtime_ns, 'obj': os.stat(obj).st_mtime_ns, 'cfg': cfg}


def save_stamps(stamps):
    tmp = os.path.join(OBJ, '_stamps.json.tmp')
    with open(tmp, 'w') as f:
        json.dump(stamps, f)
    os.replace(tmp, os.path.join(OBJ, '_stamps.json'))


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
        if os.name == 'nt':
            subprocess.run(['taskkill', '/PID', str(p.pid), '/T', '/F'], capture_output=True)
        else:
            p.kill()
        p.communicate()
        return None, '', time.time() - t


def compile_one(name):
    src, obj = os.path.join(GEN, name), os.path.join(OBJ, name[:-2] + EXT)
    total = 0.0
    for limit in (OPTNONE, OPTNONE // 4, OPTNONE // 16):
        big = big_functions(src, limit)
        hdr = os.path.splitext(obj)[0] + '.optnone.h'
        extra = []
        if big:
            with open(hdr, 'w') as f:
                f.write('#include "runtime.h"\n')
                f.writelines('void %s(%s) __attribute__((optnone, noinline));\n' % (fn, params) for _, fn, params in big)
            extra = ['-include', hdr]
        elif limit != OPTNONE:
            continue
        code, out, secs = run(CC + ['-c', OPT] + FLAGS + toolchain.EXTRA + toolchain.LIFTED_EXTRA + extra + [src, '-o', obj], TIME_LIMIT)
        total += secs
        if code is not None:
            note = '%d fn(s) > %d stmts optnone' % (len(big), limit) if big else ''
            return name, code, out, total, note
    code, out, secs = run(CC + ['-c', '-O0'] + FLAGS + toolchain.EXTRA + toolchain.LIFTED_EXTRA + [src, '-o', obj], None)
    return name, code, out, total + secs, 'WHOLE FILE -O0 (time limit hit 3x)'


def link():
    objs = sorted(os.path.join(OBJ, n) for n in os.listdir(OBJ) if n.endswith(EXT))
    rsp = os.path.join(OBJ, '_objs.rsp')
    with open(rsp, 'w') as f:
        f.writelines('"%s"\n' % o.replace('\\', '/') for o in objs)
    dll = os.path.join(ROOT, 'build', 'main_test.dll' if toolchain.NAME == toolchain.DEFAULT else 'main_test_%s.dll' % toolchain.NAME)
    if toolchain.LINUX:
        dll = os.path.join(toolchain.OUT, 'main_test.so')
    stubs = os.path.join(ROOT, 'scripts', 'rt_stubs.c')
    t = time.time()
    r = subprocess.run(CC + ['-shared', '-O1', '-w', '-mcx16'] + toolchain.EXTRA + ['-I', INC, '-I', GEN, stubs, '@' + rsp, '-o', dll],
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
    t0 = time.time()
    names = sorted(n for n in os.listdir(GEN) if n.endswith('.c'))
    keep = {n[:-2] for n in names}
    for n in os.listdir(OBJ):                            # objects of C files the lifter no longer writes
        base = n.split('.')[0]
        if (n.endswith(EXT) or n.endswith('.optnone.h')) and base not in keep:
            os.remove(os.path.join(OBJ, n))
            print('removed %s (its C file is gone)' % n)
    cfg, headers = config_hash(names)
    try:
        with open(os.path.join(OBJ, '_stamps.json')) as f:
            stamps = {k: v for k, v in json.load(f).items() if k in set(names)}
    except (OSError, ValueError):
        stamps = {}
    gone = removed_prototypes()
    if gone is None and stamps:
        print('no copy of the last funcs.h (%s): every file is compiled' % os.path.join(OBJ, '_funcs.h.prev'))
        stamps = {}
    gone_re = re.compile(r'\b(?:%s)\b' % '|'.join(map(re.escape, sorted(gone)))) if gone else None
    if '--adopt' in sys.argv:
        missing = [n for n in names if not os.path.isfile(os.path.join(OBJ, n[:-2] + EXT))]
        if missing:
            raise SystemExit('--adopt: %d C file(s) have no object (%s ...)' % (len(missing), missing[0]))
        stamps = {n: stamp(n, cfg) for n in names}
        save_stamps(stamps)
        shutil.copyfile(os.path.join(GEN, 'funcs.h'), os.path.join(OBJ, '_funcs.h.prev'))
        print('adopted %d existing objects as up to date (no compile)' % len(names))
        return link() if '--link' in sys.argv else 0
    force = '--force' in sys.argv
    stale = [n for n in names if force or not up_to_date(n, stamps, cfg, gone_re)]
    for n in stale:
        stamps.pop(n, None)
    stale.sort(key=lambda n: -os.path.getsize(os.path.join(GEN, n)))     # biggest first: better packing
    print('%d of %d C files to compile (%s; headers: %s; %d prototype(s) removed or changed in funcs.h) after %.1fs'
          % (len(stale), len(names), 'forced' if force else '%d up to date' % (len(names) - len(stale)), ' '.join(headers),
             len(gone or ()), time.time() - t0), flush=True)
    failed, notes, done = [], [], 0
    with concurrent.futures.ThreadPoolExecutor(jobs) as pool:
        for name, code, out, secs, note in pool.map(compile_one, stale):
            done += 1
            if code:
                failed.append((name, out))
            else:
                stamps[name] = stamp(name, cfg)
            if note or secs > 120:
                notes.append('%-12s %6.0fs  %s' % (name, secs, note))
            if done % 25 == 0 or code or done == len(stale):
                print('[%d/%d] %.0fs%s' % (done, len(stale), time.time() - t0, '  FAILED ' + name if code else ''), flush=True)
                save_stamps(stamps)                      # an interrupted run keeps what it finished
    save_stamps(stamps)
    if not failed:                                       # the prototypes every object is now consistent with
        shutil.copyfile(os.path.join(GEN, 'funcs.h'), os.path.join(OBJ, '_funcs.h.prev'))
    with open(os.path.join(OBJ, '_report.txt'), 'w') as f:
        f.write('compiled %d files in %.0fs (%d up to date), %d failed\n' % (len(stale), time.time() - t0, len(names) - len(stale), len(failed)))
        f.writelines(n + '\n' for n in notes)
        for name, out in failed:
            f.write('\n==== %s\n%s\n' % (name, out[-4000:]))
    print('compiled %d files in %.0fs (%d up to date; TOOLCHAIN=%s); %d failed; %d slow/fallback (see %s)'
          % (len(stale), time.time() - t0, len(names) - len(stale), toolchain.NAME, len(failed), len(notes),
             os.path.join(OBJ, '_report.txt')))
    for name, out in failed[:5]:
        print('==== %s\n%s' % (name, out[-1500:]))
    if failed:
        return 1
    return link() if '--link' in sys.argv else 0


if __name__ == '__main__':
    sys.exit(main())
