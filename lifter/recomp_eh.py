"""Lifts MAIN with C++ landing-pad resume entries, the scheme ro_study\\recomp_nro.py uses for the modules
(see nnunwind.c and eh_runtime.h). recomp.py itself is not modified: this script reuses its instruction lifter and
only changes how a function is wrapped and what table.c lists.

    python recomp_eh.py <outdir> [jobs]        e.g.  python recomp_eh.py gen_eh 24

Differences from `python recomp.py full <outdir>`:
  - a lifted function that contains landing pads becomes
        void u_<entry>(Ctx *c, uint64_t rp)    rp = 0 runs it from the top, rp = landing pad address resumes there
        void f_<entry>(Ctx *c)                 { u_<entry>(c, 0); }
  - a function with a catch clause whose handler can return (i.e. not a pad that only calls __clang_call_terminate)
    is entered through nro_catch_call(c, u_<entry>), which gives the unwinder a host frame to come back to;
  - the start of every function that has an LSDA is a function entry;
  - table.c also holds resume_table_main (landing pad -> u_ function), exported by game_shim_eh.c.
The instruction translation is byte-for-byte recomp.py's (the same Fn.lift produces every statement)."""
import bisect
import collections
import json
import os
import struct
import sys
import recomp
import regcache
from recomp import IMG, FILE_OFF, BASE, TEXT_SIZE, STRTAB, SYMTAB, Env, Fn, Unimpl, word

EH_CACHE = 'eh_main_tables.json'
# [Recomps#1] guest registers in C locals (regcache.py); RECOMPS_REGCACHE=1 turns it on. Functions over
# REGCACHE_MAX instructions are left as they are (compile_main.py compiles those optnone anyway).
REGCACHE = os.environ.get('RECOMPS_REGCACHE', '0') == '1'      # off: no measurable gain at -O2 (finding 91)
REGCACHE_MAX = 20000
REGCACHE_BR = os.environ.get('RECOMPS_REGCACHE_BR', 'off')  # functions with a jump table: all / x (V in the Ctx) / off (3.5x / 2.6x larger code)


def _u32(o): return struct.unpack_from('<I', IMG, FILE_OFF + o)[0]
def _s32(o): return struct.unpack_from('<i', IMG, FILE_OFF + o)[0]
def _uleb(o):
    r = s = 0
    while True:
        b = IMG[FILE_OFF + o]; o += 1
        r |= (b & 0x7f) << s; s += 7
        if not b & 0x80: return r, o
def _sleb(o):
    r = s = 0
    while True:
        b = IMG[FILE_OFF + o]; o += 1
        r |= (b & 0x7f) << s; s += 7
        if not b & 0x80:
            if b & 0x40: r -= 1 << s
            return r, o
def _enc(o, e):
    """value of a DW_EH_PE field as an image offset (indirect pointers are not followed: not needed here)"""
    f = e & 0x0f
    if f in (0, 4): v = struct.unpack_from('<Q', IMG, FILE_OFF + o)[0]; n = 8
    elif f == 0xc: v = struct.unpack_from('<q', IMG, FILE_OFF + o)[0]; n = 8
    elif f == 3: v = _u32(o); n = 4
    elif f == 0xb: v = _s32(o); n = 4
    elif f == 1: v, o2 = _uleb(o); n = o2 - o
    else: raise ValueError(hex(e))
    if v and (e & 0x70) == 0x10: v += o
    return v, o + n


def _bl_target(a):
    w = word(a)
    if (w & 0xfc000000) != 0x94000000: return None
    return a + recomp.sext(w & 0x3ffffff, 26) * 4


def parse_main_eh():
    """{'pads': sorted landing pads, 'catchers': FDE starts whose handlers can return, 'starts': FDE starts with an LSDA}"""
    env = Env(); env.imports = set(); env.full = True
    term = {}
    def is_terminate_helper(t):                           # __clang_call_terminate: bl __cxa_begin_catch; bl std::terminate
        if t not in term:
            calls = []
            for i in range(6):
                x = _bl_target(t + i * 4)
                if x: calls.append(env.plt_symbol(x))
            term[t] = calls[:2] == ['__cxa_begin_catch', '_ZSt9terminatev']
        return term[t]
    def terminate_pad(lp):                                # the pad's first instruction (after an optional register move) calls the helper
        for i in range(2):
            t = _bl_target(lp + i * 4)
            if t: return is_terminate_helper(t)
        return False
    mod0 = _u32(4)
    hdr = mod0 + _s32(mod0 + 0x10)
    count = _u32(hdr + 8)
    cies = {}
    pads, catchers, starts = set(), set(), set()
    fde_starts = set()
    n_term = 0
    for i in range(count):
        fde = hdr + _s32(hdr + 16 + i * 8)
        cie = fde + 4 - _u32(fde + 4)
        if cie not in cies:
            p = cie + 9
            aug = b''
            while IMG[FILE_OFF + p]: aug += bytes([IMG[FILE_OFF + p]]); p += 1
            p += 1
            _, p = _uleb(p); _, p = _sleb(p); p += 1
            le, fe = 0xff, 0
            if aug[:1] == b'z':
                _, p = _uleb(p)
                for ch in aug[1:]:
                    if ch == 76: le = IMG[FILE_OFF + p]; p += 1
                    elif ch == 82: fe = IMG[FILE_OFF + p]; p += 1
                    elif ch == 80: e = IMG[FILE_OFF + p]; p += 1; _, p = _enc(p, e & 0x7f)
            cies[cie] = (le, fe)
        le, fe = cies[cie]
        p = fde + 8
        start, p = _enc(p, fe); _, p = _enc(p, fe & 0x0f)
        if start % 4 == 0 and 0 <= start < TEXT_SIZE:    # [Recomps#1] every FDE start is an exact function entry
            fde_starts.add(BASE + start)
        _, p = _uleb(p)
        lsda = _enc(p, le)[0] if le != 0xff else 0
        if not lsda or start % 4 or not 0 <= start < TEXT_SIZE: continue
        starts.add(BASE + start)
        p = lsda
        assert IMG[FILE_OFF + p] == 0xff; p += 1
        tte = IMG[FILE_OFF + p]; p += 1
        if tte != 0xff: _, p = _uleb(p)
        cse = IMG[FILE_OFF + p]; p += 1
        csl, p = _uleb(p); cs_end = p + csl
        while p < cs_end:
            s, p = _enc(p, cse & 0x0f); l, p = _enc(p, cse & 0x0f); lp, p = _enc(p, cse & 0x0f); act, p = _uleb(p)
            if not lp: continue
            pad = BASE + start + lp
            pads.add(pad)
            if act:
                a = cs_end + act - 1
                clause = False
                while True:
                    filt, a2 = _sleb(a); nxt, _ = _sleb(a2)
                    if filt: clause = True
                    if not nxt: break
                    a = a2 + nxt
                if clause:
                    if terminate_pad(pad): n_term += 1
                    else: catchers.add(BASE + start)
    print('exception tables of main: %d functions with an LSDA, %d landing pads, %d functions entered through nro_catch_call '
          '(%d call-site records lead straight to __clang_call_terminate and need none)' % (len(starts), len(pads), len(catchers), n_term))
    print('FDE function starts: %d' % len(fde_starts))
    return {'pads': sorted(pads), 'catchers': sorted(catchers), 'starts': sorted(starts), 'fde_starts': sorted(fde_starts)}


_eh = None
def eh_info():
    global _eh
    if _eh is None:
        with open(EH_CACHE) as f:
            d = json.load(f)
        _eh = (d['pads'], set(d['catchers']), d['starts'])
    return _eh


TAILS = ('return;', 'goto', '{ jt', '{ f_', '{ imp_', '{ h_')


def lift_function(env, entry):
    """recomp.lift_function with resume entries; returns (text, instruction count, bad, [(pad, entry)])."""
    plt = env.plt_entry(entry)
    if plt:
        text, n, bad = recomp.lift_function(env, entry)
        return text, n, bad, []
    end = env.extent(entry)
    fn = Fn(entry, end, env)
    lines, bad = [], []
    pc = entry
    while pc < end:
        w = word(pc)
        try:
            stmt = fn.lift(w, pc)
        except Unimpl as e:
            stmt = 'guest_unimpl(0x%xULL, 0x%08xU, "%s");' % (pc, w, e)
            bad.append((pc, w, str(e)))
        lines.append((pc, stmt))
        pc += 4
    all_pads, catchers, _ = eh_info()
    pads = all_pads[bisect.bisect_left(all_pads, entry):bisect.bisect_left(all_pads, end)]
    two = bool(pads) or entry in catchers
    out = ['void %s_%x(Ctx *c%s) {' % ('u' if two else 'f', entry, ', uint64_t rp' if two else ''), '  c->fn = 0x%xULL;' % entry]
    if fn.has_br:
        out.append('  uint64_t jt = 0;')
    if pads:
        out.append('  if (rp) switch (rp) {')
        out.extend('  case 0x%xULL: goto L_%x;' % (lp, lp) for lp in pads)
        out.append('  default: guest_trap("resume at an address that is not a landing pad of this function", rp); }')
    falls = end < BASE + TEXT_SIZE and not lines[-1][1].startswith(TAILS)
    cache = regcache.ALL if not fn.has_br or REGCACHE_BR == 'all' else regcache.X_FLAGS if REGCACHE_BR == 'x' else 0
    rc = regcache.transform(lines, end if falls else None, cache) if REGCACHE and cache and len(lines) <= REGCACHE_MAX else None
    if rc:
        decl, lines, trailer, dispatch_reload = rc
        out[2:2] = decl                                  # after c->fn, before the resume switch and the first label
    for pc, stmt in lines:
        out.append('L_%x: %s' % (pc, stmt))
    out.append('  ' + trailer if rc else '  f_%x(c); return;' % end if falls else '  return;')
    if fn.has_br:
        out.append('L_dispatch: %sswitch (jt) {' % (dispatch_reload if rc else ''))
        out.extend('  case 0x%xULL: goto L_%x;' % (pc, pc) for pc, _ in lines)
        out.append('  default: call_indirect(c, jt); return; }')
    out.append('}')
    if two:
        out.append('void f_%x(Ctx *c) { %s }' % (entry, 'nro_catch_call(c, u_%x);' % entry if entry in catchers else 'u_%x(c, 0);' % entry))
    if falls:
        env.wanted.setdefault(end, None)
    return '\n'.join(out), len(lines), bad, [(lp, entry) for lp in pads]


_worker_env = None


def _worker_init(starts):
    global _worker_env
    _worker_env = Env()
    _worker_env.imports = set()
    _worker_env.full = True
    _worker_env.starts = starts


def _worker_scan(entries):
    env = _worker_env
    env.wanted = collections.OrderedDict()
    total = bad = 0
    for a in entries:
        _, n, b, _ = lift_function(env, a)
        total += n
        bad += len(b)
    return total, bad, list(env.wanted)


def _worker_write(job):
    outdir, name, entries = job
    env = _worker_env
    env.wanted = collections.OrderedDict()
    texts, resume = [], []
    for a in entries:
        t, _, _, r = lift_function(env, a)
        texts.append(t)
        resume += r
    content = '/* generated by recomp_eh.py -- do not edit */\n#include "runtime.h"\n#include "rt_ext.h"\n#include "funcs.h"\n\n' + '\n\n'.join(texts) + '\n'
    path = os.path.join(outdir, name)
    if os.path.exists(path):
        with open(path) as f:
            if f.read() == content:
                return name, False, resume
    with open(path, 'w') as f:
        f.write(content)
    return name, True, resume


def main(outdir, jobs):
    import multiprocessing
    import time
    t0 = time.time()
    info = parse_main_eh()
    with open(EH_CACHE, 'w') as f:
        json.dump(info, f)
    env = Env()
    env.imports = set()
    env.full = True
    starts = sorted(set(env.compute_starts()) | set(info['starts']) | set(info['fde_starts']))
    env.starts = starts
    print('function entries: %d  (%.0fs)' % (len(starts), time.time() - t0))
    os.makedirs(outdir, exist_ok=True)
    known = set(starts)
    pending = list(starts)
    all_funcs = []
    total = bad = 0
    with multiprocessing.Pool(jobs, _worker_init, (starts,)) as pool:
        while pending:
            pending.sort()
            parts = [pending[i:i + 400] for i in range(0, len(pending), 400)]
            new = set()
            for n, b, wanted in pool.imap_unordered(_worker_scan, parts):
                total += n
                bad += b
                new.update(w for w in wanted if w not in known and BASE <= w < BASE + TEXT_SIZE)
            all_funcs += pending
            print('discovery: %d functions, %d extra branch targets  (%.0fs)' % (len(pending), len(new), time.time() - t0))
            known |= new
            pending = list(new)
        all_funcs.sort()
        buckets = collections.OrderedDict()
        for a in all_funcs:
            buckets.setdefault((a - BASE) // recomp.BUCKET, []).append(a)
        jobs_list = [(outdir, 'c_%04x.c' % b, entries) for b, entries in buckets.items()]
        changed = 0
        files, resume = [], []
        for name, was_changed, res in pool.imap_unordered(_worker_write, jobs_list):
            files.append(name)
            changed += was_changed
            resume += res
    first = {}
    for lp, e in sorted(resume):                         # a pad lifted twice (it is also a branch target lifted on its own): the outer function
        first.setdefault(lp, e)
    resume = sorted(first.items())
    catchers = set(info['catchers']) & set(all_funcs)
    with open(os.path.join(outdir, 'funcs.h'), 'w') as f:
        f.write('/* generated by recomp_eh.py -- do not edit */\n')
        f.write('void nro_catch_call(Ctx *c, void (*body)(Ctx *, uint64_t));\n')
        f.writelines('void f_%x(Ctx *c);\n' % a for a in all_funcs)
        f.writelines('void u_%x(Ctx *c, uint64_t rp);\n' % a for a in sorted(set(e for _, e in resume) | catchers))
    with open(os.path.join(outdir, 'table.c'), 'w') as f:
        f.write('/* generated by recomp_eh.py -- do not edit */\n#include "runtime.h"\n#include "funcs.h"\n\n')
        f.write('const FnEntry fn_table[] = {\n')
        f.writelines('  { 0x%xULL, f_%x },\n' % (a, a) for a in all_funcs)
        f.write('};\nconst int fn_table_count = %d;\n' % len(all_funcs))
        f.write('\n/* landing pad -> the function to call as fn(c, landing pad); sorted by address */\n')
        f.write('typedef struct { uint64_t lp; void (*fn)(Ctx *, uint64_t); } NroResume;\n')
        f.write('const NroResume resume_table_main[] = {\n')
        f.writelines('  { 0x%xULL, u_%x },\n' % (lp, e) for lp, e in resume)
        f.write('  { 0, 0 }\n};\nconst int resume_count_main = %d;\n' % len(resume))
    nsym = (STRTAB - SYMTAB) // 24
    with open(os.path.join(outdir, 'imports.c'), 'w') as f:
        f.write('/* generated by recomp.py -- do not edit */\n#include "runtime.h"\n\nconst DynSym dyn_syms[] = {\n')
        for i in range(nsym):
            name_off, info_b, other, shndx, value, size = struct.unpack_from('<IBBHQQ', IMG, FILE_OFF + SYMTAB + i * 24)
            p = FILE_OFF + STRTAB + name_off
            name = IMG[p:IMG.index(b'\0', p)].decode('latin-1')
            f.write('  { "%s", 0x%xULL, %d },\n' % (name, value, 1 if shndx else 0))
        f.write('};\nconst int dyn_sym_count = %d;\n' % nsym)
    keep = set(files) | {'table.c', 'imports.c'}
    for n in os.listdir(outdir):
        if n.endswith('.c') and n not in keep:
            os.remove(os.path.join(outdir, n))
    missing = [lp for lp in info['pads'] if lp not in first]
    print('functions: %d   files: %d (%d rewritten)   instructions: %d   not translated: %d   (%.0fs)'
          % (len(all_funcs), len(files), changed, total, bad, time.time() - t0))
    print('resume entries: %d landing pads (%d pads of the tables are in no lifted function), %d functions through nro_catch_call'
          % (len(resume), len(missing), len(catchers)))


if __name__ == '__main__':
    main(sys.argv[1], int(sys.argv[2]) if len(sys.argv) > 2 else 16)
