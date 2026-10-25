"""Which compiler the build scripts use: TOOLCHAIN=mingw (the default since 2026-10-16) or TOOLCHAIN=msvc (environment
variable, or --toolchain=<name> on the command line of compile_main.py / build_runtime.py / build_launcher.py).

  mingw  llvm-mingw 20260922 (clang 23.1.2, UCRT, x86_64-w64-mingw32; folder: LLVM_MINGW or deps/llvm-mingw): clang + lld +
         mingw-w64 import libraries, all redistributable; the programs need only Windows 10/11 (notes/toolchain-mingw.md,
         findings 95-101). Outputs: build/obj, build/game.dll, build/game_native.exe (+ COPYING.MinGW-w64-runtime.txt).
  msvc   clang 9.0.0 (folder: LLVM_ROOT or deps/llvm) for the MSVC target: links with MSVC's link.exe, CRT and Windows SDK libraries
         (needs the Visual Studio Build Tools). Outputs beside the default build: build/obj_msvc, build/out_msvc/.
  linux  [Recomps#1] the native Linux version (notes/linux-port-plan.md step 3): x86_64-linux-gnu, glibc 2.35 or newer.
         zig cc (folder: ZIG or deps/zig, holding zig / zig.exe; zig 0.17.0 = clang 22) for glibc TARGET_GLIBC, from Windows
         or Linux (the Linux builder package ships it); on Linux without zig: clang (CLANG=<path> or the one on PATH): its
         programs need the build machine's glibc. Outputs: build-linux/obj (*.o), build-linux/ (game.so, game_native);
         never build/. The host runtime is built by scripts/build_runtime_linux.py.
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
NAMES = ('mingw', 'msvc', 'linux', 'darwin')
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
LINUX = NAME == 'linux'
DARWIN = NAME == 'darwin'
# the default toolchain builds into the standard places; the other one beside them, in folders named after it
OBJ = os.path.join(BUILD, 'obj' if NAME == DEFAULT else 'obj_' + NAME)        # lifted objects (compile_main.py)
OUT = BUILD if NAME == DEFAULT else os.path.join(BUILD, 'out_' + NAME)       # game.dll, obj_host/, game_native.exe
OBJEXT, SHLIB, EXEEXT = '.obj', '.dll', '.exe'
EXTRA = []                                               # compiler flags of this toolchain only (none for mingw / msvc)
LIFTED_EXTRA = []                                        # ... for the lifted code only (compile_main.py, game.so)
TARGET_GLIBC = '2.35'                                    # linux: oldest glibc the programs need (Ubuntu 22.04; SteamOS 3.6 has 2.39)
if LINUX:                                                # [Recomps#1] the Linux version never shares build/ with Windows
    OBJ = os.path.join(ROOT, 'build-linux', 'obj')
    OUT = os.path.join(ROOT, 'build-linux')
    OBJEXT, SHLIB, EXEEXT = '.o', '.so', ''
    EXTRA = ['-fdeclspec', '-fPIC']                      # __declspec(dllexport) of the shared headers; game.so is shared
    LIFTED_EXTRA = ['-g0']                               # the lifted code without debug info, as on Windows (zig cc adds -g)
elif DARWIN:                                             # [Recomps#1] macOS: build-darwin/, .dylib shared libraries
    DARWIN_BUILD = os.path.join(ROOT, 'build-darwin')
    OBJ = os.path.join(DARWIN_BUILD, 'obj')
    OUT = DARWIN_BUILD
    OBJEXT, SHLIB, EXEEXT = '.o', '.dylib', ''
    EXTRA = ['-D_XOPEN_SOURCE=700', '-D_DARWIN_C_SOURCE', '-fdeclspec', '-fPIC']
    LIFTED_EXTRA = ['-g0']

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
elif LINUX:
    import shutil as _sh
    _zig = locate('ZIG', 'zig', 'zig.exe' if os.name == 'nt' else 'zig')
    _clang = None if os.name == 'nt' else (os.environ.get('CLANG') or _sh.which('clang'))
    if _zig and not os.environ.get('CLANG'):             # zig cc: clang with glibc headers for TARGET_GLIBC, from any host
        CLANG = os.path.join(_zig, 'zig.exe' if os.name == 'nt' else 'zig')
        CC = [CLANG, 'cc', '-target', 'x86_64-linux-gnu.' + TARGET_GLIBC]
    elif _clang:                                         # on Linux without zig (or CLANG=<path>): the system's clang
        CLANG = _clang
        CC = [CLANG]
    else:
        sys.exit('TOOLCHAIN=linux: no compiler: on Windows set ZIG=<zig 0.17.0 folder> or put it at deps/zig; on Linux install '
                 'clang (or set CLANG=<path>)')
    BIN = os.path.dirname(CLANG)
    OPTNONE = 5000                                       # clang 22 / 23 behave alike on the huge functions (toolchain-mingw.md 2.1)
    RUNTIME_LICENCE = None                               # glibc is the system's
elif DARWIN:                             # macOS Apple Silicon / Intel (clang system)
    import shutil as _sh
    _clang = os.environ.get('CLANG') or _sh.which('clang')
    if not _clang:
        sys.exit('TOOLCHAIN=darwin: no compiler: install clang (xcode-select --install, Xcode, or set CLANG=<path>)')
    CLANG = _clang
    BIN = os.path.dirname(CLANG)
    CC = [CLANG]
    OPTNONE = 20000
    RUNTIME_LICENCE = None
    DARWIN_BUILD = os.path.join(ROOT, 'build-darwin')
    OBJ = os.path.join(DARWIN_BUILD, 'obj')
    OUT = DARWIN_BUILD
    EXTRA = ['-D_XOPEN_SOURCE=700', '-D_DARWIN_C_SOURCE', '-fdeclspec', '-fPIC']
    LIFTED_EXTRA = ['-g0']
    # macOS uses .dylib for shared libraries; binary outputs: obj_darwin/ build-darwin/
else:                                                 # msvc / default
    LLVM_ROOT = need('LLVM_ROOT', 'llvm', 'bin/clang.exe', 'LLVM/clang 9.0.0 (TOOLCHAIN=msvc)')
    BIN = os.path.join(LLVM_ROOT, 'bin')
    CLANG = os.path.join(BIN, 'clang.exe')
    CC = [CLANG]
    OPTNONE = 20000
    RUNTIME_LICENCE = None                               # MSVC's CRT is the system's / the redistributable's
if not LINUX:
    CC = [CLANG]                                         # the compile / link command's first words (linux: zig cc -target ...)
LLVM_RC = os.path.join(BIN, 'llvm-rc.exe')


def copy_runtime_licence(folder):
    """mingw: put COPYING.MinGW-w64-runtime.txt beside the programs just linked in folder (no-op for msvc)."""
    if RUNTIME_LICENCE:
        import shutil
        shutil.copy2(RUNTIME_LICENCE, folder)
        print('%s: copied beside the programs (MinGW-w64 runtime notices)' % os.path.basename(RUNTIME_LICENCE))


if not os.path.exists(CLANG):
    sys.exit('TOOLCHAIN=%s: compiler not found: %s' % (NAME, CLANG))
