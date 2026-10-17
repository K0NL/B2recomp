# Local lifter copy (GPL-3.0, from ssbu-recomp)

Copied from https://github.com/Denkishi/ssbu-recomp `tools/lifter/` at commit `3a96916` (2026-10-16).
License: GPL-3.0 (`LICENSE`).

Changes from upstream:
- `recomp.py`: `ELF` points at `../targets/exefs/main.elf`; `TEXT_SIZE`, `RELA/RELASZ`, `JMPREL/PLTRELSZ`,
  `SYMTAB/STRTAB` set from our main.elf's DYNAMIC segment; added `import os`.
- `recomp_eh.py`: unchanged.

Full lift (writes `eh_main_tables.json` in the current directory; deletes stray `.c` files in `<outdir>`):

    cd lifter
    python recomp_eh.py ..\build\gen_eh 16

Further changes (2026-10-15):
- `recomp.py`: exclusive pairs (`ldxp/ldaxp/stxp/stlxp`), `{s,u}{mull,mlal,mlsl}{2}`, `{s,u}qxtn{2}`,
  `{s,u}rshr`, `sq{r}shrn{2}`, `fcvtz{s,u}` (fixed-point). Marked `[Recomps#1]`.
- `rt_ext.h` (new): the C helpers those emit. Runtime must add `uint64_t ex_val2;` to `Ctx` and build with `-mcx16`.
- `recomp_eh.py`: every `.eh_frame` FDE start is a function entry; generated files include `rt_ext.h`.
- `test_ext.py` (new): decoder + semantics check of the added forms against Unicorn (`pip install unicorn capstone`).
