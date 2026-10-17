"""[Recomps#1] Check the instructions added to recomp.py against Unicorn.

For every instruction of main in the forms the [Recomps#1] extensions added (ext_words(): found by encoding mask in the
player's own main.elf, so no list of the game's words is kept; on our dump these are exactly the 71 non-trivial gaps of
the old evidence/lift-gaps.tsv, finding 102):
  1. lift it with recomp.py and parse the emitted helper call (checks the decoder's field extraction);
  2. execute it in Unicorn on random inputs and compare with a Python mirror of the rt_ext.h helper,
     called with the parameters the lifter emitted (checks the semantics the C transcribes).
The C itself is not compiled here (no clang yet); the mirrors follow rt_ext.h line by line.

    python test_ext.py [trials per word]
"""
import math
import os
import random
import re
import struct
import sys

from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM
from unicorn.arm64_const import UC_ARM64_REG_CPACR_EL1, UC_ARM64_REG_V0, UC_ARM64_REG_X0, UC_ARM64_REG_PC

import recomp

HERE = os.path.dirname(os.path.abspath(__file__))
M64 = (1 << 64) - 1


# ---------------------------------------------------------------- mirrors of neon.h / rt_ext.h
def vget(v, es, i):
    return (v >> (i * (8 << es))) & ((1 << (8 << es)) - 1)


def vset(v, es, i, x):
    bitsz = 8 << es
    mask = ((1 << bitsz) - 1) << (i * bitsz)
    return (v & ~mask) | ((x & ((1 << bitsz) - 1)) << (i * bitsz))


def vsext(x, es):
    b = 8 << es
    x &= (1 << b) - 1
    return x - (1 << b) if x >> (b - 1) else x


def sat_s(v, es):
    hi = (1 << ((8 << es) - 1)) - 1
    lo = -hi - 1
    return (hi if v > hi else lo if v < lo else v) & ((1 << (8 << es)) - 1)


def sat_u(v, es):
    hi = (1 << (8 << es)) - 1
    return hi if v > hi else v


def v_long3(d, n, m, op, es, u, q):
    r, cnt = d, 8 >> es
    for i in range(cnt):
        a, b = vget(n, es, i + q * cnt), vget(m, es, i + q * cnt)
        p = (a * b if u else vsext(a, es) * vsext(b, es)) & M64
        acc = vget(d, es + 1, i) if op else 0
        r = vset(r, es + 1, i, (p if op == 0 else acc + p if op == 1 else acc - p) & M64)
    return r


def v_qxtn(d, n, es, q, u):
    r, cnt = d, 8 >> es
    if not q:
        r &= M64
    for i in range(cnt):
        x = vget(n, es + 1, i)
        r = vset(r, es, i + q * cnt, sat_u(x, es) if u else sat_s(vsext(x, es + 1), es))
    return r


def v_rshr(d, n, sh, es, u, count):
    r = 0
    for i in range(count):
        x = vget(n, es, i)
        y = (x + (1 << (sh - 1))) >> sh if u else (vsext(x, es) + (1 << (sh - 1))) >> sh
        r = vset(r, es, i, y & M64)
    return r


def v_sqshrn(d, n, sh, es, q, rnd):
    r, cnt = d, 8 >> es
    if not q:
        r &= M64
    for i in range(cnt):
        v = vsext(vget(n, es + 1, i), es + 1)
        if rnd:
            v += 1 << (sh - 1)
        r = vset(r, es, i + q * cnt, sat_s(v >> sh, es))
    return r


def v_fcvtz_fix(d, n, fbits, dbl, u, count):
    r = 0
    for i in range(count):
        raw = vget(n, 3 if dbl else 2, i)
        x = struct.unpack('<d' if dbl else '<f', struct.pack('<Q' if dbl else '<I', raw))[0] * math.ldexp(1.0, fbits)
        if x != x:
            y = 0
        else:
            bits = 64 if dbl else 32
            t = math.trunc(x) if math.isfinite(x) else (1 << 200 if x > 0 else -(1 << 200))
            y = (min(max(t, 0), (1 << bits) - 1)) if u else sat_s(t, 3 if dbl else 2)
        r = vset(r, 3 if dbl else 2, i, y)
    return r


# (mask, value, name): the instruction forms recomp.py's [Recomps#1] extensions added (lifter/README.md)
FORMS = [
    (0xBF20FC00, 0x0E20C000, 'smull'),       # SMULL{2} <Vd>.<Ta>, <Vn>.<Tb>, <Vm>.<Tb>  (U=0, opcode 1100)
    (0xBF20FC00, 0x0E20A000, 'smlsl'),       # SMLSL{2} (opcode 1010)
    (0xBF3FFC00, 0x0E214800, 'sqxtn'),       # SQXTN{2} (U=0)
    (0xBF80FC00, 0x0F00FC00, 'fcvtzs'),      # FCVTZS (vector, fixed-point), immh != 0
    (0xBF80FC00, 0x0F009C00, 'sqrshrn'),     # SQRSHRN{2}, immh != 0
    (0xBF80FC00, 0x0F002400, 'srshr'),       # SRSHR (vector), immh != 0
    (0xBFFF8000, 0x887F8000, 'ldaxp'),       # LDAXP <Rt>, <Rt2>, [<Xn>]
    (0xBFE08000, 0x88208000, 'stlxp'),       # STLXP <Ws>, <Rt>, <Rt2>, [<Xn>]
]


def ext_words(img=None):
    """{word: disassembly} of every instruction of those forms in main's .text (the first LOAD segment)."""
    img = recomp.IMG if img is None else img
    words = {}
    text = img[recomp.FILE_OFF:recomp.FILE_OFF + recomp.TEXT_SIZE]
    for (w,) in struct.iter_unpack('<I', text):
        for mask, val, name in FORMS:
            if w & mask == val:
                if name in ('fcvtzs', 'sqrshrn', 'srshr') and not (w >> 19) & 0xf:
                    continue
                rt, rn, rt2, rs = w & 31, (w >> 5) & 31, (w >> 10) & 31, (w >> 16) & 31
                r = 'x' if w >> 30 & 1 else 'w'
                if name == 'ldaxp':
                    dis = 'ldaxp %s%d, %s%d, [x%d]' % (r, rt, r, rt2, rn)
                elif name == 'stlxp':
                    dis = 'stlxp w%d, %s%d, %s%d, [x%d]' % (rs, r, rt, r, rt2, rn)
                else:
                    dis = '%s%s %08x' % (name, '2' if w >> 30 & 1 and name in ('smull', 'smlsl', 'sqxtn', 'sqrshrn') else '', w)
                words[w] = dis
                break
    return words


HELPERS = {'v_long3': v_long3, 'v_qxtn': v_qxtn, 'v_rshr': v_rshr, 'v_sqshrn': v_sqshrn, 'v_fcvtz_fix': v_fcvtz_fix}
CALL = re.compile(r'^(v_\w+)\(&c->v\[(\d+)\], &c->v\[(\d+)\](?:, &c->v\[(\d+)\])?((?:, -?\d+)*)\);$')


def rand_vec(kind):
    if kind == 'float':
        pick = [lambda: random.uniform(-4, 4), lambda: random.uniform(-1e5, 1e5), lambda: float('nan'),
                lambda: random.choice([1e30, -1e30, float('inf'), -float('inf')]), lambda: random.uniform(-1, 1) * 2.0 ** -20]
        return sum(struct.unpack('<I', struct.pack('<f', random.choice(pick)()))[0] << (32 * i) for i in range(4))
    if random.random() < 0.3:
        return random.choice([0, (1 << 128) - 1, sum(0x7fff << (16 * i) for i in range(8)), sum(0x8000 << (16 * i) for i in range(8)),
                              sum(0x7fffffff << (32 * i) for i in range(4)), sum(0x80000000 << (32 * i) for i in range(4))])
    return random.getrandbits(128)


def run_unicorn(word, vin):
    uc = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
    uc.reg_write(UC_ARM64_REG_CPACR_EL1, 3 << 20)            # enable FP/SIMD
    uc.mem_map(0x10000, 0x1000)
    uc.mem_write(0x10000, struct.pack('<I', word))
    for r, v in vin.items():
        uc.reg_write(UC_ARM64_REG_V0 + r, v)
    uc.emu_start(0x10000, 0x10004)
    return uc


def main(trials):
    words = ext_words()
    fn = recomp.Fn(recomp.BASE, recomp.BASE + recomp.TEXT_SIZE, recomp.Env())
    simd_ok = pair_ok = 0
    failures = []
    for word, dis in sorted(words.items(), key=lambda x: x[1]):
        stmt = fn.lift(word, recomp.BASE + 0x1000000)
        if dis.startswith(('ldaxp', 'stlxp')):
            regs = [int(x) for x in re.findall(r'[wx](\d+)', dis)]
            if dis.startswith('ldaxp'):
                rt, rt2, rn = regs
                want = ['c->x[%d] = (uint64_t)(t1);' % rt, 'c->x[%d] = (uint64_t)(t2);' % rt2, 'uint64_t a = c->x[%d];' % rn]
            else:
                rs, rt, rt2, rn = regs
                want = ['c->x[%d] = (uint32_t)(stx128(c, c->x[%d], %s, %s));' % (rs, rn,
                        '0ULL' if rt == 31 else 'c->x[%d]' % rt, '0ULL' if rt2 == 31 else 'c->x[%d]' % rt2)]
            missing = [s for s in want if s not in stmt]
            if missing:
                failures.append((dis, 'decode', stmt, missing))
            else:
                pair_ok += 1
            continue
        m = CALL.match(stmt)
        if not m:
            failures.append((dis, 'no helper call', stmt, None))
            continue
        name, rd, rn, rm = m.group(1), int(m.group(2)), int(m.group(3)), m.group(4)
        rm = int(rm) if rm else None
        args = [int(x) for x in m.group(5).split(',')[1:]]
        kind = 'float' if name == 'v_fcvtz_fix' else 'int'
        for _ in range(trials):
            vin = {rd: rand_vec('int'), rn: rand_vec(kind)}
            if rm is not None:
                vin[rm] = rand_vec('int')
            uc = run_unicorn(word, vin)
            got = uc.reg_read(UC_ARM64_REG_V0 + rd)
            d, n = vin[rd], vin[rn]
            if rm is not None:
                want = HELPERS[name](d, n, vin[rm], *args)
            else:
                want = HELPERS[name](d, n, *args)
            if got != want:
                failures.append((dis, 'value', stmt, 'in %s\n    unicorn %032x\n    mirror  %032x' %
                                 ({k: '%032x' % v for k, v in vin.items()}, got, want)))
                break
        else:
            simd_ok += 1
    print('pair words: %d decode as expected' % pair_ok)
    print('SIMD words: %d match Unicorn on %d random inputs each' % (simd_ok, trials))
    for dis, what, stmt, detail in failures[:20]:
        print('FAIL %-32s %s\n    %s\n    %s' % (dis, what, stmt, detail))
    print('failures: %d' % len(failures))
    return 1 if failures else 0


def emit_c(path, trials):
    """Write a C test that runs the real rt_ext.h helpers on Unicorn's inputs and compares with Unicorn's outputs."""
    words = {w: d for w, d in ext_words().items() if not d.startswith(('ldaxp', 'stlxp'))}
    fn = recomp.Fn(recomp.BASE, recomp.BASE + recomp.TEXT_SIZE, recomp.Env())
    cases = []
    for word, dis in sorted(words.items()):
        stmt = fn.lift(word, recomp.BASE + 0x1000000)
        m = CALL.match(stmt)
        rd, rn, rm = int(m.group(2)), int(m.group(3)), m.group(4)
        kind = 'float' if m.group(1) == 'v_fcvtz_fix' else 'int'
        for _ in range(trials):
            vin = {rd: rand_vec('int'), rn: rand_vec(kind)}
            if rm:
                vin[int(rm)] = rand_vec('int')
            got = run_unicorn(word, vin).reg_read(UC_ARM64_REG_V0 + rd)
            cases.append((dis, stmt, vin, rd, got))
    v = lambda x: '{ .d = { 0x%xULL, 0x%xULL } }' % (x & M64, x >> 64)
    with open(path, 'w') as f:
        f.write('#include <stdio.h>\n#include "runtime.h"\n#include "rt_ext.h"\n\n')
        f.write('static int fails;\nstatic void check(Ctx *c, int rd, uint64_t lo, uint64_t hi, const char *what) {\n'
                '    if (c->v[rd].d[0] != lo || c->v[rd].d[1] != hi) { if (fails++ < 10) printf("FAIL %s: got %016llx%016llx want %016llx%016llx\\n", what,\n'
                '        (unsigned long long)c->v[rd].d[1], (unsigned long long)c->v[rd].d[0], (unsigned long long)hi, (unsigned long long)lo); }\n}\n\n')
        f.write('int main(void) {\n    static Ctx ctx; Ctx *c = &ctx;\n')
        for dis, stmt, vin, rd, got in cases:
            sets = ' '.join('{ V128 t = %s; c->v[%d] = t; }' % (v(x), r) for r, x in vin.items())
            f.write('    %s %s check(c, %d, 0x%xULL, 0x%xULL, "%s");\n' % (sets, stmt, rd, got & M64, got >> 64, dis))
        f.write('    /* stx128: succeeds when memory still holds what LDXP saw, fails otherwise */\n'
                '    { static uint64_t mem[2] __attribute__((aligned(16))) = { 1, 2 }; uint64_t a = (uint64_t)(uintptr_t)mem;\n'
                '      c->ex_addr = a; c->ex_val = 1; c->ex_val2 = 2;\n'
                '      if (stx128(c, a, 5, 6) != 0 || mem[0] != 5 || mem[1] != 6) { fails++; printf("FAIL stx128 success case\\n"); }\n'
                '      c->ex_addr = a; c->ex_val = 1; c->ex_val2 = 2;\n'
                '      if (stx128(c, a, 7, 8) != 1 || mem[0] != 5 || mem[1] != 6) { fails++; printf("FAIL stx128 stale case\\n"); }\n'
                '      if (stx128(c, a, 7, 8) != 1) { fails++; printf("FAIL stx128 without monitor\\n"); } }\n')
        f.write('    printf("%%d cases, %%d failures\\n", %d + 3, fails);\n    return fails != 0;\n}\n' % len(cases))
    print('%d cases -> %s' % (len(cases), path))


if __name__ == '__main__':
    random.seed(1)
    if len(sys.argv) > 2 and sys.argv[1] == '--emit-c':
        emit_c(sys.argv[2], int(sys.argv[3]) if len(sys.argv) > 3 else 100)
        sys.exit(0)
    sys.exit(main(int(sys.argv[1]) if len(sys.argv) > 1 else 300))
