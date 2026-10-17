"""Report the untranslated instructions of a full lift: what they are and which functions hold them.

    python scripts/lift_gaps.py <gen_dir> <main.elf> <functions.tsv> <out.tsv>

functions.tsv is Ghidra's export (evidence/main-functions.tsv). Also compares the lifter's function
set with the .eh_frame FDE starts (exact compiler-emitted function boundaries).
"""
import bisect
import glob
import os
import re
import struct
import sys

import capstone

gen, elf, ftsv, out = sys.argv[1:5]
BASE, FILE_OFF = 0x7100000000, 0x1000
img = open(elf, 'rb').read()

sites = {}
for path in glob.glob(os.path.join(gen, 'c_*.c')):
    for m in re.finditer(r'guest_unimpl\(0x([0-9a-f]+)ULL, 0x([0-9a-f]+)U, "([^"]*)"\)', open(path).read()):
        sites[int(m.group(1), 16)] = (int(m.group(2), 16), m.group(3))

funcs = []
for line in open(ftsv, encoding='utf-8').read().splitlines()[1:]:
    a, size, name, callers = line.split('\t')
    funcs.append((int(a, 16), int(size), name, int(callers)))
funcs.sort()
starts = [f[0] for f in funcs]

md = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)
by_func = {}
with open(out, 'w', encoding='utf-8') as f:
    f.write('address\tword\tkind\tdisassembly\tghidra_function\tfunc_size\tfunc_refs\n')
    for addr in sorted(sites):
        word, kind = sites[addr]
        dis = next(md.disasm(struct.pack('<I', word), addr), None)
        text = '%s %s' % (dis.mnemonic, dis.op_str) if dis else '(invalid)'
        i = bisect.bisect_right(starts, addr) - 1
        fn = funcs[i] if i >= 0 and addr < funcs[i][0] + max(funcs[i][1], 4) else None
        f.write('%x\t%08x\t%s\t%s\t%s\t%s\t%s\n' % (addr, word, kind, text, fn[2] if fn else '(none)',
                                                    fn[1] if fn else '', fn[3] if fn else ''))
        key = fn[2] if fn else '(outside any Ghidra function)'
        by_func.setdefault(key, []).append((kind, text.split()[0]))

print('%d untranslated sites in %d places' % (len(sites), len(by_func)))
mn = {}
for v in by_func.values():
    for kind, m in v:
        mn[(kind, m)] = mn.get((kind, m), 0) + 1
for (kind, m), n in sorted(mn.items(), key=lambda x: -x[1]):
    print('  %4d  %-12s %s' % (n, kind, m))
print('functions holding them (most sites first):')
for k, v in sorted(by_func.items(), key=lambda x: -len(x[1]))[:25]:
    print('  %3d  %s' % (len(v), k))

# FDE starts vs lifted functions
mod0 = struct.unpack_from('<I', img, FILE_OFF + 4)[0]
hdr = mod0 + struct.unpack_from('<i', img, FILE_OFF + mod0 + 0x10)[0]
count = struct.unpack_from('<I', img, FILE_OFF + hdr + 8)[0]
fde_starts = {BASE + hdr + struct.unpack_from('<i', img, FILE_OFF + hdr + 12 + i * 8)[0] for i in range(count)}
lifted = set()
for path in glob.glob(os.path.join(gen, 'c_*.c')):
    lifted.update(int(x, 16) for x in re.findall(r'^void f_([0-9a-f]+)\(Ctx \*c\) \{', open(path).read(), re.M))
print('FDE function starts: %d; lifted functions: %d; FDE starts not lifted as their own function: %d; lifted not in FDEs: %d'
      % (len(fde_starts), len(lifted), len(fde_starts - lifted), len(lifted - fde_starts)))
