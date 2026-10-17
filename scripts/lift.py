"""Run the lifter (lifter/recomp.py, or upstream's tools/lifter/recomp.py via RECOMP_UPSTREAM) against our main.elf.

recomp.py hard-codes SSBU 13.0.4's ELF path and layout at module level, so this loads its source,
swaps in our values (from main.elf's DYNAMIC segment; .text + .plt = first LOAD segment), and runs it.

Usage:
    python scripts/lift.py cover                       whole-binary coverage statistics
    python scripts/lift.py lift <out.c> <addr>[,...]   lift roots and their callees

`full` (multiprocessing) is not supported through this wrapper: Windows spawn re-imports the
main module, which this exec-based approach doesn't survive. Use a patched copy for that.
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
# our lifter copy (its constants are ours already, so the substitutions below change nothing), or an upstream checkout's
# tools/lifter/recomp.py given with RECOMP_UPSTREAM
RECOMP = os.path.abspath(os.environ.get('RECOMP_UPSTREAM') or os.path.join(HERE, '..', 'lifter', 'recomp.py'))

OURS = {
    'ELF': repr(os.path.join(HERE, '..', 'targets', 'exefs', 'main.elf')),
    'TEXT_SIZE': '0x120dbe0',                      # .text + .plt (PLT stubs must be inside)
    'RELA, RELASZ': '0x120e050, 0x247818',
    'JMPREL, PLTRELSZ': '0x1455868, 0x2c40',
    'SYMTAB, STRTAB': '0x1459668, 0x145cbc0',
}

src = open(RECOMP, encoding='utf-8').read()
for name, value in OURS.items():
    pattern = r'^%s = .*$' % re.escape(name)
    line = '%s = %s' % (name, value)
    src, n = re.subn(pattern, lambda _: line, src, count=1, flags=re.M)   # function: no escape processing
    if n != 1:
        sys.exit('recomp.py no longer defines %s at top level; update scripts/lift.py' % name)

sys.argv[0] = RECOMP
sys.path.insert(0, os.path.dirname(RECOMP))
exec(compile(src, RECOMP, 'exec'), {'__name__': '__main__', '__file__': RECOMP})
