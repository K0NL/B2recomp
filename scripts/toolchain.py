"""Which compiler the build scripts use: TOOLCHAIN=mingw (the default since 2026-10-16) or TOOLCHAIN=msvc (environment
variable, or --toolchain=<name> on the command line of compile_main.py / build_runtime.py / build_launcher.py).

  mingw  llvm-mingw 20260922 (clang 23.1.2, UCRT, x86_64-w64-mingw32; folder: LLVM_MINGW or deps/llvm-mingw): clang + lld +
         mingw-w64 import libraries, all redistributable; the programs need only Windows 10/11 (notes/toolchain-mingw.md,
         findings 95-101). Outputs: build/obj, build/game.dll, build/game_native.exe (+ COPYING.MinGW-w64-runtime.txt).
  msvc   clang 9.0.0 (folder: LLVM_ROOT or deps/llvm) for the MSVC target: links with MSVC's link.exe, CRT and Windows SDK libraries
         (needs the Visual Studio Build Tools). Outputs beside the default build: build/obj_msvc, build/out_msvc/.
The default toolchain's outputs live in the standard places, which every run script, the launcher and bench.py use; an
exe in build/out_msvc must get SSBU_GRAPHICS_CONFIG / SSBU_CONFIG (bench.py and play.ps1 -Msvc set them, finding 97).

Every tool and library is found the same way (as ssbu-recomp's setup.py does): an environment variable (scripts/setup.py
sets them from its options), else <repo>/deps/<name> (a copy or a junction; ignored by git). No machine paths are built in.
"""
import os
import sys

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
BUILD = os.path.join(ROOT, 'build')
DEPS = os.path.join(ROOT, 'deps')
NAMES = ('mingw', 'msvc')
DEFAULT = 'mingw'


def _name():
    for a in sys.argv[1:]:
        if a.startswith('--toolchain='):
            return a.split('=', 1)[1]
    return os.environ.get('TOOLCHAIN', DEFAULT) or DEFAULT


def locate(env, sub, probe):
    """A dependency folder: $env, else <repo>/deps/<sub>; the first that holds probe (a path relative to it), or None."""
    for d in (os.environ.get(env), os.path.join(DEPS, sub)):
        if d and os.path.exists(os.path.join(d, probe)):
            return os.path.abspath(d)
    return None


def need(env, sub, probe, what):
    """locate(), or stop with what to do."""
    d = locate(env, sub, probe)
    if not d:
        sys.exit('%s not found: set %s=<folder> or put it at deps\\%s (the folder that contains %s)'
                 % (what, env, sub, probe.replace('/', '\\')))
    return d


NAME = _name()
if NAME not in NAMES:
    sys.exit('TOOLCHAIN must be one of %s, not %r' % ('/'.join(NAMES), NAME))
MINGW = NAME == 'mingw'
# the default toolchain builds into the standard places; the other one beside them, in folders named after it
OBJ = os.path.join(BUILD, 'obj' if NAME == DEFAULT else 'obj_' + NAME)        # lifted objects (compile_main.py)
OUT = BUILD if NAME == DEFAULT else os.path.join(BUILD, 'out_' + NAME)       # game.dll, obj_host/, game_native.exe

if MINGW:
    LLVM_MINGW = need('LLVM_MINGW', 'llvm-mingw', 'bin/x86_64-w64-mingw32-clang.exe', 'llvm-mingw (TOOLCHAIN=mingw)')
    BIN = os.path.join(LLVM_MINGW, 'bin')
    CLANG = os.path.join(BIN, 'x86_64-w64-mingw32-clang.exe')
    # clang 23 optimises a few huge lifted functions very slowly (one 8,749-statement function: 150 s at -O1): at 5,000
    # statements 19-23 functions get optnone and the whole -O2 build takes ~5 min (notes/toolchain-mingw.md 2.1)
    OPTNONE = 5000
    # what a program statically linked against the MinGW-w64 runtime should carry beside it (the file's own words: "copy
    # COPYING.MinGW-w64-runtime.txt into your package"); llvm-mingw keeps it under each target, not at its top level
    RUNTIME_LICENCE = os.path.join(LLVM_MINGW, 'x86_64-w64-mingw32', 'share', 'mingw32', 'COPYING.MinGW-w64-runtime.txt')
else:
    BIN = os.path.join(need('LLVM_ROOT', 'llvm', 'bin/clang.exe', 'LLVM/clang 9.0.0 (TOOLCHAIN=msvc)'), 'bin')
    CLANG = os.path.join(BIN, 'clang.exe')
    OPTNONE = 20000
    RUNTIME_LICENCE = None                               # MSVC's CRT is the system's / the redistributable's
LLVM_RC = os.path.join(BIN, 'llvm-rc.exe')


def copy_runtime_licence(folder):
    """mingw: put COPYING.MinGW-w64-runtime.txt beside the programs just linked in folder (no-op for msvc)."""
    if RUNTIME_LICENCE:
        import shutil
        shutil.copy2(RUNTIME_LICENCE, folder)
        print('%s: copied beside the programs (MinGW-w64 runtime notices)' % os.path.basename(RUNTIME_LICENCE))


if not os.path.exists(CLANG):
    sys.exit('TOOLCHAIN=%s: compiler not found: %s' % (NAME, CLANG))
