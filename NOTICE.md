# Notices and attribution

B2recomp (Bayonetta 2 Recompiled)
Copyright (C) 2026 K0NL (https://github.com/K0NL), and the ssbu-recomp authors for the parts listed below.

This program is free software: you can redistribute it and/or modify it under the terms of the GNU General Public
License as published by the Free Software Foundation, either version 3 of the License, or (at your option) any later
version. This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the
implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for more
details. The full text is in `LICENSE` (also kept as `runtime/LICENSE` and `lifter/LICENSE`).

Our changes to copied files are marked `[Recomps#1]` (the project's working name before B2recomp).

AI helped in reverse engineering the game, with Ghidra and the open-source project REA.

## Code copied from other projects

| Our path | Origin | Licence | Changes |
|---|---|---|---|
| `runtime/` | [ssbu-recomp](https://github.com/Denkishi/ssbu-recomp) `runtime/`, commit `3a96916` (2026-10-16), by Denkishi | GPL-3.0-or-later | Marked `[Recomps#1]` in the code; listed with dates in `runtime/UPSTREAM.md`; Smash-only files removed (same file) |
| `lifter/recomp.py`, `lifter/recomp_eh.py` | ssbu-recomp `tools/lifter/`, same commit | GPL-3.0-or-later | Marked `[Recomps#1]`; listed in `lifter/README.md` |
| `lifter/gen_quad_tests.py` | ssbu-recomp `tools/lifter/`, same commit (binary128 test vectors, game-independent) | GPL-3.0-or-later | None (a provenance comment added at the top) |
| `tools/extract/nso2elf.py` | ssbu-recomp `tools/extract/`, same commit (NSO to ELF, LZ4 only, no keys) | GPL-3.0-or-later | None (a provenance comment added at the top) |
| `runtime/mx_ops.h` (via ssbu-recomp) | Generated from suyu `src/video_core/.../maxwell.inc` (Maxwell opcode table) | GPL-3.0-or-later (suyu) | None by us |
| `runtime/mx_translate.c`, `runtime/nnalbum.c`, `runtime/nnapplet.c`, `runtime/nnnet_c4a.inc`, ... (via ssbu-recomp) | Behaviour described after suyu sources, cited in comments as `[suyu] <file>` | GPL-3.0-or-later (suyu) | — |
| `runtime/tex_decode.c` (via ssbu-recomp) | Tables from the Khronos Data Format Specification (BCn / ASTC) | Specification | — |

Our own files (everything else: `scripts/`, `launcher/`, `tests/`, `lifter/regcache.py`, `lifter/sdklift.py`,
`runtime/gen/make_gen.py`, new runtime files) are under the same licence. `scripts/setup.py` follows the structure of
ssbu-recomp's `scripts/setup.py` (resumable steps, state file, logs).

## Public sources cited for values in the runtime

No code is copied from these; they are named in comments as the source of numbers the runtime uses.

| Where | Values | Source |
|---|---|---|
| `runtime/nncxxlib.c` `classic_table` | `std::ctype<char>` masks of the "C" locale | LLVM libc++ (`src/locale.cpp` builtin table, `include/__locale` `ctype_base`; Apache-2.0 with LLVM exception) and the glibc `<ctype.h>` `_ISbit` layout (LGPL-2.1) |
| `runtime/nvn_stub.c` `g_devinfo` (most answers come from the player's own SDK at build time, `runtime/gen/make_gen.py`) | Tegra X1 / Maxwell limits (descriptor sizes, handle layout, alignments) | deko3d `include/deko3d.h` (devkitPro, zlib licence); the yuzu-lineage emulators (Citron `src/video_core/engines/maxwell_3d.cpp`, GPL-3.0); the Vulkan specification (Khronos) |

## Tools used to reverse engineer the game (not included, not needed to build)

| Tool | Version used | Licence |
|---|---|---|
| [Ghidra](https://github.com/NationalSecurityAgency/ghidra), by the NSA | 12.1.4 | Apache-2.0 |
| [Ghidra-Switch-Loader](https://github.com/Adubbz/Ghidra-Switch-Loader), by Adubbz (loads Switch NSO files into Ghidra) | 12.1.4 build | ISC-style (Copyright 2019 Adubbz) |
| [REA](https://github.com/morluto/rea) (`rea-agents`), by morluto | 6.5.0 | MIT |

## Third-party components used at build time (not included in this repository)

| Component | Version | Licence |
|---|---|---|
| llvm-mingw (LLVM / clang 23.1.2, lld, mingw-w64 headers and runtime), the default toolchain | 20260922 | LLVM: Apache-2.0 with LLVM exception; mingw-w64: ZPL 2.1, public domain and others (`COPYING.MinGW-w64-runtime.txt`) |
| SDL3 | 3.4.18 | zlib |
| Vulkan-Headers | vulkan-sdk-1.4.341.0 | Apache-2.0 / MIT |
| glslang (built into `ssbu_glslang.dll`) | 16.6.0 | BSD-3-Clause, Apache-2.0 and others (see its LICENSE.txt) |
| LLVM / clang (optional MSVC build, `--toolchain msvc`) | 9.0.0 | Apache-2.0 with LLVM exception |
| Python packages: lz4, Pillow, (self-test) unicorn, capstone | — | BSD / HPND / GPL-2.0 / BSD |

The game is never distributed in any form: every player builds it locally with `scripts/setup.py`. The **builder
package** (`scripts/make_package.py`) contains this source plus the tools above in `deps\` (llvm-mingw trimmed to x86_64,
Python with lz4 and Pillow, SDL3, Vulkan-Headers, `ssbu_glslang.dll`, the Microsoft Visual C++ runtime files that DLL
needs), with their licences in `licenses\` and `THIRD-PARTY-NOTICES.md`. `ssbu_glslang.dll` is the one program of ours
in it in compiled form (`runtime/vk_glslang_shim.c` linked with glslang and SPIRV-Tools); its corresponding source is
in the package itself (GPL-3.0 section 6), plus glslang 16.6.0's public source. A build with
llvm-mingw links parts of the MinGW-w64 runtime into `game_native.exe` and `launcher.exe`; the build copies the
runtime's notices (`COPYING.MinGW-w64-runtime.txt`, from the toolchain's `x86_64-w64-mingw32/share/mingw32/`) beside
them.

## Not included

No code, data, assets, icons, fonts, firmware or keys of Nintendo, PlatinumGames or SEGA, and nothing generated from
them. Names of SDK functions and addresses used for interoperability appear in comments and in
`lifter/sdklift_roots.txt`. `scripts/setup.py` holds SHA-256 hashes and sizes of the supported version's files, to
check a player's dump before building.
<!-- OPEN before publishing (notes/release-audit.md section 5): confirm with the ssbu-recomp author how they want to be
     credited (no per-file copyright lines upstream); confirm the suyu revision mx_ops.h came from (ssbu-recomp's
     gen_mxops.py). -->
