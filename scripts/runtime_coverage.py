"""How much of our main's imports and NVN usage ssbu-recomp's runtime already handles.

    python scripts/runtime_coverage.py <ssbu-recomp/runtime> <main.elf> <main-strings.tsv> <out.tsv>

Imports: undefined dynamic symbols of main.elf. Handled: names in the runtime's import tables
({"name", fn} entries, incl. nnhid.c's H("...") prefix macro). NVN: nvn* names in main's strings
(the game resolves NVN entry points by name through nvnBootstrapLoader) vs nvn* names in the runtime.
"""
import collections
import glob
import os
import re
import struct
import sys

rt_dir, elf, strings_tsv, out = sys.argv[1:5]

handled, where = set(), {}
for path in glob.glob(os.path.join(rt_dir, '*.c')) + glob.glob(os.path.join(rt_dir, '*.inc')):
    text = open(path, encoding='latin-1').read()
    for name in re.findall(r'\{\s*"([^"]+)"\s*,', text):
        handled.add(name)
        where.setdefault(name, os.path.basename(path))
    # prefix macros such as  #define OS(n) "_ZN2nn2os" n   used as  {OS("12CreateThread..."), fn}
    macros = dict(re.findall(r'#define\s+(\w+)\(\w+\)\s+"([^"]+)"\s*\w+', text))
    for mac, name in re.findall(r'\{\s*(\w+)\("([^"]+)"\)\s*,', text):
        if mac in macros:
            handled.add(macros[mac] + name)
            where.setdefault(macros[mac] + name, os.path.basename(path))
rt_nvn = set()
for path in glob.glob(os.path.join(rt_dir, 'nvn*.c')):
    rt_nvn.update(re.findall(r'"(nvn[A-Z]\w+)"', open(path, encoding='latin-1').read()))

img = open(elf, 'rb').read()
FILE_OFF, SYMTAB, STRTAB = 0x1000, 0x1459668, 0x145cbc0
imports = []
for i in range((STRTAB - SYMTAB) // 24):
    name_off, info, other, shndx, value, size = struct.unpack_from('<IBBHQQ', img, FILE_OFF + SYMTAB + i * 24)
    if name_off and shndx == 0:
        p = FILE_OFF + STRTAB + name_off
        imports.append(img[p:img.index(b'\0', p)].decode('latin-1'))


def family(n):
    m = re.match(r'_ZN(?:K)?2nn(\d+)(\w+)', n)
    if m:
        k = int(m.group(1))
        return 'nn::' + m.group(2)[:k]
    if n.startswith(('_ZNSt3__1', '_ZNKSt3__1', '_ZSt', '_ZdlPv', '_Znwm', '_Zna', '_Zdl', '_Zda', '__cxa')):
        return 'libc++/C++ ABI'
    if n.startswith('nvn'):
        return 'nvn'
    if n.startswith('glslc'):
        return 'glslc'
    if n.startswith('_Z'):
        return 'other C++'
    return 'libc / C'


fam = collections.defaultdict(lambda: [0, 0])
with open(out, 'w', encoding='utf-8') as f:
    f.write('import\tfamily\thandled_in\n')
    for n in sorted(imports):
        h = n in handled
        fam[family(n)][0] += 1
        fam[family(n)][1] += h
        f.write('%s\t%s\t%s\n' % (n, family(n), where.get(n, '')))
print('imports: %d, handled by ssbu-recomp runtime: %d (%.0f%%)'
      % (len(imports), sum(v[1] for v in fam.values()), 100.0 * sum(v[1] for v in fam.values()) / len(imports)))
for k, (n, h) in sorted(fam.items(), key=lambda x: -x[1][0]):
    print('  %-22s %4d imported  %4d handled' % (k, n, h))

game_nvn = set()
for line in open(strings_tsv, encoding='utf-8').read().splitlines()[1:]:
    v = line.split('\t', 2)[-1]
    if re.fullmatch(r'nvn[A-Z]\w+', v):
        game_nvn.add(v)
print('NVN entry points named in main: %d; also named in their nvn*.c: %d; missing: %d'
      % (len(game_nvn), len(game_nvn & rt_nvn), len(game_nvn - rt_nvn)))
for n in sorted(game_nvn - rt_nvn)[:40]:
    print('   missing', n)
