"""Make the B2recomp builder package: this source tree plus every tool the build needs, so a player installs nothing
and only adds their own extracted dump.

    python scripts/make_package.py [--out <folder>] [--no-zip]
    python scripts/make_package.py --linux [--out <folder>] [--tar]   the Linux builder package, .zip (--tar: .tar.gz; main_linux)

Writes <out>/B2recomp/ and <out>/B2recomp-builder-<commit>.zip (default <out> = build/package; the zip holds the folder
B2recomp\\). Contents:
  the source files of commit HEAD    in the private working repo the ones notes/release-draft/public.gitignore lets
                                     through (with that file as .gitignore); in the public repo every tracked file
  B2recomp.exe                       the launcher (launcher/launcher.c, built here with --no-icon): its first start shows the
                                     build screen (dump folder, Build: scripts/setup.py), then the settings and Play
  Build B2recomp.bat                 scripts/package/build.bat: the same build from a console (fallback), then the launcher
  deps\\llvm-mingw                    llvm-mingw trimmed to the x86_64 build (notes/toolchain-mingw.md section 1)
  deps\\SDL3, deps\\Vulkan-Headers     headers, the x64 import library and SDL3.dll
  deps\\ssbu_glslang.dll              the shader-compiler shim (runtime/vk_glslang_shim.c + glslang), game-independent,
    + msvcp140.dll, vcruntime140.dll, vcruntime140_1.dll   Microsoft's app-local runtime it needs (a PC without the
                                     Visual C++ Redistributable has no MSVCP140.dll); build_runtime.py copies them beside the exe
  deps\\python                        the Python running this script (3.10+, 64-bit) without tests / IDLE / tkinter, with lz4
                                     and Pillow; no ._pth file, so scripts import from their own folder as usual
  licenses\\, THIRD-PARTY-NOTICES.md  every component's licence, version and source
Nothing of the game: the package is made only from the repository and the tools; the last step refuses to finish when a
file of the game or of a local build (ELF/NSO/NCA/keys/CPK/icons/lifted code/built programs) is in it.
Tools are found as the build scripts find them (environment variable, else deps\\<name>); ssbu_glslang.dll from
GLSLANG_DLL, deps\\ or build\\; the MSVC runtime from MSVC_CRT_DIR, else the newest Visual Studio Redist folder (vswhere).
"""
import glob
import importlib.metadata
import io
import os
import shutil
import subprocess
import sys
import tarfile
import time
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEPS = os.path.join(ROOT, 'deps')
ALLOWLIST = 'notes/release-draft/public.gitignore'
PINNED = {'llvm-mingw': '20260922', 'SDL3': '3.4.18', 'Vulkan-Headers': 'vulkan-sdk-1.4.341.0', 'glslang': '16.6.0'}
MSVC_CRT = ('msvcp140.dll', 'vcruntime140.dll', 'vcruntime140_1.dll')
LLVM_BIN = ('clang-23.exe', 'clang.exe', 'clang-target-wrapper.exe', 'x86_64-w64-mingw32-clang.exe', 'x86_64-w64-windows-gnu.cfg',
            'x86_64-pc-windows-gnu.cfg', 'mingw32-common.cfg', 'ld.lld.exe', 'llvm-rc.exe', 'llvm-symbolizer.exe', 'libLLVM-23.dll',
            'libclang-cpp.dll', 'libc++.dll', 'libunwind.dll')
FORBIDDEN_EXT = ('.elf', '.nso', '.nca', '.nsp', '.xci', '.nsz', '.xcz', '.keys', '.cpk', '.npdm', '.ico', '.bmp', '.res', '.dat',
                 '.pchtxt', '.ips')                  # players' code mods (scripts/pchtxt.py): never shipped
FORBIDDEN_NAME = ('game.dll', 'game_native.exe', 'launcher.exe', 'main', 'sdk', 'subsdk0', 'rtld', 'eh_main_tables.json',
                  'sdklift_gen.c', 'sdkdata.c', 'image_layout.h', 'imports_all.txt', 'sdklift_report.txt')


def git(*args, binary=False):
    r = subprocess.run(['git', *args], cwd=ROOT, capture_output=True)
    if r.returncode:
        sys.exit('git %s: %s' % (' '.join(args), r.stderr.decode(errors='replace')))
    return r.stdout if binary else r.stdout.decode('utf-8')


def find(env, sub, probe, what):
    for d in (os.environ.get(env), os.path.join(DEPS, sub)):
        if d and os.path.exists(os.path.join(d, probe)):
            return os.path.realpath(d)
    sys.exit('%s not found: set %s=<folder> or put it at deps\\%s' % (what, env, sub))


def copy(src, dst):
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    shutil.copy2(src, dst)


def copytree(src, dst, skip=()):
    shutil.copytree(src, dst, ignore=lambda d, names: [n for n in names if n in skip or n == '__pycache__'], dirs_exist_ok=True)


def source_tree(pkg):
    tree = set(git('ls-tree', '-r', '--name-only', 'HEAD').splitlines())
    private = ALLOWLIST in tree
    if private:
        ignored = set(git('ls-files', '-c', '-i', '--exclude-from=' + ALLOWLIST).splitlines())
        files = sorted(tree - ignored - {'.gitignore'})
    else:
        files = sorted(tree)
    tar = git('archive', '--format=tar', 'HEAD', '--', *files, binary=True)
    with tarfile.open(fileobj=io.BytesIO(tar)) as t:
        t.extractall(pkg, filter='data')
    if private:
        with open(os.path.join(pkg, '.gitignore'), 'wb') as f:
            f.write(git('show', 'HEAD:' + ALLOWLIST, binary=True))
    print('source: %d files of %s (%s)' % (len(files) + private, git('rev-parse', '--short', 'HEAD').strip(),
                                           'public allowlist' if private else 'all tracked files'))
    return len(files) + private


def python_runtime(dst):
    base = sys.base_prefix
    if sys.maxsize < 2 ** 32 or sys.version_info < (3, 10) or not os.path.exists(os.path.join(base, 'python.exe')):
        sys.exit('run this with a 64-bit Python 3.10+ from python.org (its folder is copied into the package)')
    ver = 'python3%d.dll' % sys.version_info[1]
    for f in ('python.exe', 'pythonw.exe', 'python3.dll', ver, 'vcruntime140.dll', 'vcruntime140_1.dll', 'LICENSE.txt'):
        if os.path.exists(os.path.join(base, f)):
            copy(os.path.join(base, f), os.path.join(dst, f))
    os.makedirs(os.path.join(dst, 'DLLs'))
    for f in os.listdir(os.path.join(base, 'DLLs')):
        if not f.lower().startswith(('tcl', 'tk', '_tkinter', '_test')) and not f.lower().endswith('.ico'):
            copy(os.path.join(base, 'DLLs', f), os.path.join(dst, 'DLLs', f))
    copytree(os.path.join(base, 'Lib'), os.path.join(dst, 'Lib'),
             skip=('test', 'tests', 'idlelib', 'tkinter', 'turtledemo', 'ensurepip', 'site-packages', 'turtle.py'))
    site = os.path.join(dst, 'Lib', 'site-packages')
    os.makedirs(site)
    pkgs = {}
    for name in ('lz4', 'pillow'):
        dist = importlib.metadata.distribution(name)
        pkgs[name] = dist.version
        for f in dist.files:
            src = os.path.realpath(f.locate())
            rel = os.path.relpath(src, os.path.realpath(os.path.join(base, 'Lib', 'site-packages')))
            if rel.startswith('..') or '__pycache__' in rel or not os.path.isfile(src):
                continue
            copy(src, os.path.join(site, rel))
    print('python: %s + lz4 %s, Pillow %s' % (sys.version.split()[0], pkgs['lz4'], pkgs['pillow']))
    return sys.version.split()[0], pkgs


def msvc_crt_dir():
    d = os.environ.get('MSVC_CRT_DIR')
    if d:
        return d
    vsw = os.path.join(os.environ.get('ProgramFiles(x86)', r'C:\Program Files (x86)'), 'Microsoft Visual Studio', 'Installer', 'vswhere.exe')
    vs = subprocess.run([vsw, '-latest', '-products', '*', '-property', 'installationPath'], capture_output=True,
                        text=True).stdout.strip() if os.path.exists(vsw) else ''
    cands = sorted(glob.glob(os.path.join(vs, 'VC', 'Redist', 'MSVC', '*', 'x64', 'Microsoft.VC*.CRT'))) if vs else []
    if not cands:
        sys.exit('the MSVC runtime DLLs (msvcp140.dll ...) were not found: set MSVC_CRT_DIR=<folder with them> '
                 '(Visual Studio: VC\\Redist\\MSVC\\<version>\\x64\\Microsoft.VC*.CRT)')
    return cands[-1]


NOTICES = """# Third-party components in the B2recomp builder package

This package holds the B2recomp source code (GPL-3.0-or-later, see `LICENSE` and `NOTICE.md`) and, in `deps\\`, the tools
the build needs, so nothing has to be installed. **It contains nothing of the game**: the game is translated and compiled
on your PC from your own extracted dump by `B2recomp.exe` (or `Build B2recomp.bat`; both run `scripts/setup.py`). The
licence texts are in `licenses\\`.

| Component | Version | In the package | Licence (file in `licenses\\`) | Source |
|---|---|---|---|---|
| llvm-mingw (LLVM / clang, lld, mingw-w64 headers and runtime), trimmed to x86_64 | {llvm_mingw} (clang {clang}) | `deps\\llvm-mingw` | Apache-2.0 with LLVM exception and others (`llvm-mingw-LICENSE.TXT`); mingw-w64 runtime: ZPL 2.1, public domain and others (`COPYING.MinGW-w64-runtime.txt`) | https://github.com/mstorsjo/llvm-mingw/releases/tag/{llvm_mingw} |
| Python | {python} | `deps\\python` (without tests, IDLE, tkinter) | PSF License (`Python-LICENSE.txt`) | https://www.python.org/downloads/ |
| lz4 (Python package) | {lz4} | `deps\\python\\Lib\\site-packages` | BSD-3-Clause (`lz4-LICENSE`) | https://pypi.org/project/lz4/ |
| Pillow (Python package) | {pillow} | `deps\\python\\Lib\\site-packages` | MIT-CMU (`Pillow-LICENSE`) | https://pypi.org/project/pillow/ |
| SDL3 (headers, import library, SDL3.dll) | {sdl3} | `deps\\SDL3` | zlib (`SDL3-LICENSE.txt`) | https://github.com/libsdl-org/SDL/releases |
| Vulkan-Headers | {vkh} | `deps\\Vulkan-Headers` | Apache-2.0 / MIT (`Vulkan-Headers-LICENSE.md`, `Vulkan-Headers-LICENSES\\`) | https://github.com/KhronosGroup/Vulkan-Headers |
| `ssbu_glslang.dll` | built from `runtime/vk_glslang_shim.c` (this package) with MSVC (`runtime/build_glslang.bat`) against glslang {glslang}'s release libraries (glslang, SPIRV-Tools) | `deps\\ssbu_glslang.dll` | our code GPL-3.0-or-later (`LICENSE`); glslang: BSD-3-Clause and others (`glslang-LICENSE.txt`); SPIRV-Tools: Apache-2.0 (`SPIRV-Tools-LICENSE.txt`) | glslang https://github.com/KhronosGroup/glslang/tree/{glslang} (SPIRV-Tools as that release bundles it) |
| Microsoft Visual C++ runtime: msvcp140.dll, vcruntime140.dll, vcruntime140_1.dll | {crt} | `deps\\` (copied beside the game by the build) | Microsoft Visual Studio licence terms, "Distributable Code" (app-local deployment of the Visual C++ Redistributable files) | part of Visual Studio / the Visual C++ Redistributable |

Corresponding source (GPL-3.0 section 6): the complete source of B2recomp is this package itself (and the public
repository). Our programs in compiled form are `B2recomp.exe` (source: `launcher/launcher.c`, built with
`scripts/build_launcher.py --no-icon` and the llvm-mingw above) and `ssbu_glslang.dll` (source: `runtime/vk_glslang_shim.c`
and `runtime/build_glslang.bat` here, plus glslang {glslang} at the address above).
"""


# ---------------------------------------------------------------------------------------------------------------- Linux
# [Recomps#1] The Linux builder package (notes/linux-port-plan.md step 10): build/package-linux/B2recomp-linux-builder-
# <commit>.zip (or .tar.gz with --tar), one folder B2recomp/ that a player unpacks anywhere and starts with ./B2recomp (the
# launcher, Python + Tk: its build screen runs scripts/setup.py; "START HERE.txt" says how), nothing to install but a
# Vulkan driver. Made on Windows (or Linux) from the same HEAD source tree as the Windows package plus deps/linux-deps
# (or LINUX_DEPS):
#   zig-x86_64-linux-0.17.0.tar.xz                        -> deps/zig       (the compiler: zig cc for glibc 2.35)
#   cpython-3.13.16+20261013-...-install_only.tar.gz     -> deps/python    (python-build-standalone; Tcl/Tk 9.0.4 included)
#   wheels/lz4-*.whl, wheels/pillow-*.whl                -> deps/python/lib/python3.13/site-packages
#   SDL3/ (include/, lib/libSDL3.so.0, LICENSE.txt)      -> deps/SDL3
#   libssbu_glslang.so, glslang-LICENSE.txt              -> deps/          (scripts/package/linux_deps.sh made both SDL3 and this)
#   tk-xft/libtcl9tk9.0.so, tk-xft/license.terms         -> deps/python/lib/libtcl9tk9.0.so and lib/tk-xft/: Tk with Xft
#                                                           (scripts/package/linux_tk.sh); the bundled Tk goes to lib/tk-bitmap/
#                                                           (./B2recomp uses it on a PC without libXft)
# plus deps/Vulkan-Headers (include/) as in the Windows package. The archive records the Unix modes; it holds no symlinks
# (a zip tool may not make them): Python's are left out (./B2recomp runs bin/python3.13, the file they point to), SDL3 is
# linked by its file name. The tool trees are copied member by member from their archives, nothing is unpacked on NTFS.
LINUX_TOOLS = {'zig': '0.17.0', 'python': '3.13.16', 'pbs': '20261013', 'tcltk': '9.0.4', 'SDL3': '3.4.18', 'glslang': '16.6.0'}
LINUX_EXEC = ('B2recomp', 'scripts/play.sh', 'scripts/report.sh', 'scripts/package/B2recomp.sh',
              'scripts/package/linux_deps.sh', 'scripts/package/linux_tk.sh')
PY_SKIP = ('/lib/python3.13/test/', '/lib/python3.13/idlelib/', '/lib/python3.13/turtledemo/', '/lib/python3.13/ensurepip/',
           '/__pycache__/', '/site-packages/pip', '/bin/pip', '/bin/idle', '/share/man/', '/share/terminfo/',
           '/lib/pkgconfig/', '/lib/python3.13/config-3.13')

LINUX_NOTICES = """# Third-party components in the B2recomp Linux builder package

This package holds the B2recomp source code (GPL-3.0-or-later, see `LICENSE` and `NOTICE.md`) and, in `deps/`, the tools
the build needs, so nothing has to be installed. **It contains nothing of the game**: the game is translated and compiled
on your PC from your own extracted dump by `./B2recomp` (its build screen runs `scripts/setup.py`). The licence texts
are in `licenses/`.

| Component | Version | In the package | Licence (file in `licenses/`) | Source |
|---|---|---|---|---|
| zig (the compiler: clang, lld, glibc / libc++ headers and sources) | {zig} | `deps/zig` | MIT (`zig-LICENSE`), with the licences of the libraries it carries in `deps/zig/lib/` | https://ziglang.org/download/ |
| Python (python-build-standalone build {pbs}) with its bundled libraries (OpenSSL, SQLite, libffi, XZ, zlib, bzip2, Tcl/Tk {tcltk} and others) | {python} | `deps/python` (without tests, IDLE, pip) | PSF License and the licences of the bundled libraries (`Python-LICENSE.txt`) | https://github.com/astral-sh/python-build-standalone/releases/tag/{pbs} |
| lz4 (Python package) | {lz4} | `deps/python/lib/python3.13/site-packages` | BSD-3-Clause (`lz4-LICENSE`) | https://pypi.org/project/lz4/ |
| Pillow (Python package, with the libraries its wheel bundles) | {pillow} | `deps/python/lib/python3.13/site-packages` | MIT-CMU (`Pillow-LICENSE`) | https://pypi.org/project/pillow/ |
| SDL3 (headers, `libSDL3.so.0` built by `scripts/package/linux_deps.sh` with zig for glibc 2.35) | {sdl3} | `deps/SDL3` | zlib (`SDL3-LICENSE.txt`) | https://github.com/libsdl-org/SDL/releases/tag/release-{sdl3} |
| Vulkan-Headers | {vkh} | `deps/Vulkan-Headers` | Apache-2.0 / MIT (`Vulkan-Headers-LICENSE.md`) | https://github.com/KhronosGroup/Vulkan-Headers |
| `libssbu_glslang.so` | built from `runtime/vk_glslang_shim.c` (this package) by `scripts/package/linux_deps.sh` with zig, glslang {glslang} linked in (ENABLE_OPT off: no SPIRV-Tools) and LLVM libc++ / libunwind (static) | `deps/libssbu_glslang.so` | our code GPL-3.0-or-later (`LICENSE`); glslang: BSD-3-Clause and others (`glslang-LICENSE.txt`); libc++ / libunwind: Apache-2.0 WITH LLVM-exception (`libcxx-LICENSE.TXT`, `libunwind-LICENSE.TXT`) | glslang https://github.com/KhronosGroup/glslang/tree/{glslang} |
{tk_row}
Corresponding source (GPL-3.0 section 6): the complete source of B2recomp is this package itself (and the public
repository). The libraries above in compiled form were built from the sources at the addresses given, with
`scripts/package/linux_deps.sh` (SDL3, `libssbu_glslang.so`) and `scripts/package/linux_tk.sh` (Tk). The launcher
(`launcher/launcher_linux.py`) and the build scripts are Python source.
"""


class ArchiveOut:
    """The package archive: .zip (Unix modes in the entries' external attributes, deflate) or .tar.gz; regular files only."""

    def __init__(self, path, as_zip):
        self.zip, self.n, self.size, self.now = as_zip, 0, 0, time.time()
        if as_zip:
            self.z = zipfile.ZipFile(path, 'w', zipfile.ZIP_DEFLATED, compresslevel=6)
        else:
            self.t = tarfile.open(path, 'w:gz', compresslevel=6)

    def put(self, arc, data, mode=0o644):
        self.n += 1
        self.size += len(data)
        if self.zip:
            zi = zipfile.ZipInfo(arc, date_time=time.localtime(self.now)[:6])
            zi.create_system = 3                             # Unix: unzip / file managers restore the mode
            zi.external_attr = (0o100000 | (mode & 0o7777)) << 16
            zi.compress_type = zipfile.ZIP_DEFLATED
            self.z.writestr(zi, data)
        else:
            ti = tarfile.TarInfo(arc)
            ti.size, ti.mode, ti.mtime, ti.uid, ti.gid, ti.uname, ti.gname = len(data), mode, int(self.now), 0, 0, '', ''
            self.t.addfile(ti, io.BytesIO(data))

    def close(self):
        (self.z if self.zip else self.t).close()


def main_linux():
    out = next((os.path.abspath(a[6:]) for a in sys.argv[1:] if a.startswith('--out=')), os.path.join(ROOT, 'build', 'package-linux'))
    as_zip = '--tar' not in sys.argv[1:]
    ld = find('LINUX_DEPS', 'linux-deps', 'libssbu_glslang.so', 'the Linux package inputs (downloads + scripts/package/linux_deps.sh)')
    need = {
        'zig': os.path.join(ld, 'zig-x86_64-linux-%s.tar.xz' % LINUX_TOOLS['zig']),
        'python': os.path.join(ld, 'cpython-%s+%s-x86_64-unknown-linux-gnu-install_only.tar.gz' % (LINUX_TOOLS['python'], LINUX_TOOLS['pbs'])),
        'sdl': os.path.join(ld, 'SDL3', 'lib', 'libSDL3.so.0'),
        'shim': os.path.join(ld, 'libssbu_glslang.so'),
    }
    for k, f in need.items():
        if not os.path.isfile(f):
            sys.exit('%s: missing %s' % (k, f))
    wheels = sorted(glob.glob(os.path.join(ld, 'wheels', '*.whl')))
    if not any(os.path.basename(w).startswith('lz4-') for w in wheels) or not any(os.path.basename(w).startswith('pillow-') for w in wheels):
        sys.exit('wheels: lz4 and pillow (manylinux x86_64, cp313) are needed in %s' % os.path.join(ld, 'wheels'))
    vkh = find('VULKAN_HEADERS', 'Vulkan-Headers', 'include/vulkan/vulkan.h', 'Vulkan-Headers')
    tk = os.path.join(ld, 'tk-xft', 'libtcl9tk9.0.so')
    have_tk = os.path.isfile(tk)

    pkg = os.path.join(out, 'B2recomp')
    if os.path.exists(pkg):
        used = [n for n in os.listdir(pkg) if n.lower() in ('build', 'build-linux', 'targets', 'dump')]
        if used:
            sys.exit('%s holds %s (a build was made there): move it away or choose another --out=<folder>' % (pkg, ', '.join(used)))
        shutil.rmtree(pkg)
    os.makedirs(pkg)
    lic = os.path.join(pkg, 'licenses')
    os.makedirs(lic)
    nsrc = source_tree(pkg)
    # the default settings go to config/defaults (./B2recomp copies them to config/ when missing): unpacking a newer
    # package over an old folder keeps the player's own config/*.ini; VERSION tells the launcher an update arrived
    os.makedirs(os.path.join(pkg, 'config', 'defaults'))
    for n in ('graphics.ini', 'controllers.ini'):
        if os.path.isfile(os.path.join(pkg, 'config', n)):
            os.replace(os.path.join(pkg, 'config', n), os.path.join(pkg, 'config', 'defaults', n))
    with open(os.path.join(pkg, 'VERSION'), 'w', encoding='utf-8', newline='\n') as f:
        f.write(git('rev-parse', '--short', 'HEAD').strip() + '\n')
    for src, dst in (('B2recomp.sh', 'B2recomp'), ('README-linux.txt', 'START HERE.txt')):
        with open(os.path.join(ROOT, 'scripts', 'package', src), 'rb') as f:
            with open(os.path.join(pkg, dst), 'wb') as g:
                g.write(f.read().replace(b'\r\n', b'\n'))
    copy(os.path.join(ld, 'glslang-LICENSE.txt'), os.path.join(lic, 'glslang-LICENSE.txt'))
    copy(os.path.join(ld, 'SDL3', 'LICENSE.txt'), os.path.join(lic, 'SDL3-LICENSE.txt'))
    copy(os.path.join(vkh, 'LICENSE.md'), os.path.join(lic, 'Vulkan-Headers-LICENSE.md'))
    if have_tk and os.path.isfile(os.path.join(ld, 'tk-xft', 'license.terms')):
        copy(os.path.join(ld, 'tk-xft', 'license.terms'), os.path.join(lic, 'Tk-LICENSE.terms'))
    with tarfile.open(need['zig'], 'r:xz') as z:              # licence files out of the tool archives
        top = 'zig-x86_64-linux-%s/' % LINUX_TOOLS['zig']
        for src, dst in (('LICENSE', 'zig-LICENSE'), ('lib/libcxx/LICENSE.TXT', 'libcxx-LICENSE.TXT'),
                         ('lib/libunwind/LICENSE.TXT', 'libunwind-LICENSE.TXT')):
            with open(os.path.join(lic, dst), 'wb') as f:
                f.write(z.extractfile(top + src).read())
    with tarfile.open(need['python'], 'r:gz') as z:
        with open(os.path.join(lic, 'Python-LICENSE.txt'), 'wb') as f:
            f.write(z.extractfile('python/lib/python3.13/LICENSE.txt').read())
    vers = {}
    for w in wheels:
        name = os.path.basename(w).split('-')[0].lower()
        vers[name] = os.path.basename(w).split('-')[1]
        with zipfile.ZipFile(w) as zw:
            lics = [n for n in zw.namelist() if '.dist-info/' in n and os.path.basename(n).upper().startswith('LICENSE')]
            if lics:
                with open(os.path.join(lic, {'lz4': 'lz4', 'pillow': 'Pillow'}.get(name, name) + '-LICENSE'), 'wb') as f:
                    f.write(zw.read(sorted(lics, key=len)[0]))
    tk_row = ('| Tk {0} built with Xft (antialiased fonts) by `scripts/package/linux_tk.sh`, in place of the bundled one (kept in '
              '`deps/python/lib/tk-bitmap/` for PCs without libXft) | {0} | `deps/python/lib/libtcl9tk9.0.so`, `deps/python/lib/tk-xft/` | '
              'Tcl/Tk licence (BSD-style, `Tk-LICENSE.terms`) | https://sourceforge.net/projects/tcl/files/Tcl/{0}/ |'
              .format(LINUX_TOOLS['tcltk'])) if have_tk else ''
    with open(os.path.join(pkg, 'THIRD-PARTY-NOTICES.md'), 'w', encoding='utf-8', newline='\n') as f:
        f.write(LINUX_NOTICES.format(zig=LINUX_TOOLS['zig'], python=LINUX_TOOLS['python'], pbs=LINUX_TOOLS['pbs'],
                                     tcltk=LINUX_TOOLS['tcltk'], lz4=vers.get('lz4', '?'), pillow=vers.get('pillow', '?'),
                                     sdl3=LINUX_TOOLS['SDL3'], vkh=PINNED['Vulkan-Headers'], glslang=LINUX_TOOLS['glslang'],
                                     tk_row=tk_row))
    # nothing of the game or of a local build may be in our part (the tool trees below come only from their archives)
    bad, nours = [], 0
    for dp, dn, fn in os.walk(pkg):
        for n in fn:
            rel = os.path.relpath(os.path.join(dp, n), pkg).replace(os.sep, '/')
            nours += 1
            if n.lower().endswith(FORBIDDEN_EXT) or n in FORBIDDEN_NAME or \
                    rel.startswith(('build/', 'build-linux/', 'targets/', 'evidence/', 'notes/')) or \
                    (rel.startswith('runtime/gen/') and rel != 'runtime/gen/make_gen.py') or rel == 'CLAUDE.md':
                bad.append(rel)
    if bad:
        sys.exit('REFUSED: the package would contain %d file(s) that must not be shipped:\n  %s' % (len(bad), '\n  '.join(bad[:30])))

    name = os.path.join(out, 'B2recomp-linux-builder-%s.%s' % (git('rev-parse', '--short', 'HEAD').strip(), 'zip' if as_zip else 'tar.gz'))
    t0, skipped = time.time(), [0]
    a = ArchiveOut(name + '.part', as_zip)
    for dp, dn, fn in os.walk(pkg):                           # our part: the source tree, ./B2recomp, readme, licences, notices
        dn.sort()
        for n in sorted(fn):
            p = os.path.join(dp, n)
            rel = os.path.relpath(p, pkg).replace(os.sep, '/')
            with open(p, 'rb') as f:
                a.put('B2recomp/' + rel, f.read(), 0o755 if rel in LINUX_EXEC else 0o644)

    def from_archive(path, mode, strip, prefix, skip=(), rename=None):
        with tarfile.open(path, mode) as z:                   # streamed: one pass through the compressed archive
            for m in z:
                if not m.name.startswith(strip) or not m.isreg():
                    if m.issym() or m.islnk():
                        skipped[0] += 1
                    continue
                rest = m.name[len(strip):]
                if any(s in '/' + rest for s in skip) or rest.endswith('.a'):
                    continue
                rest = (rename or {}).get(rest, rest)
                a.put(prefix + rest, z.extractfile(m).read(), m.mode & 0o7777)
    from_archive(need['zig'], 'r|xz', 'zig-x86_64-linux-%s/' % LINUX_TOOLS['zig'], 'B2recomp/deps/zig/')
    from_archive(need['python'], 'r|gz', 'python/', 'B2recomp/deps/python/', PY_SKIP,
                 {'lib/libtcl9tk9.0.so': 'lib/tk-bitmap/libtcl9tk9.0.so'} if have_tk else None)
    if have_tk:                                               # Tk with Xft in place of the bundled one, and a copy for ./B2recomp's choice
        with open(tk, 'rb') as f:
            data = f.read()
        a.put('B2recomp/deps/python/lib/libtcl9tk9.0.so', data, 0o755)
        a.put('B2recomp/deps/python/lib/tk-xft/libtcl9tk9.0.so', data, 0o755)
    site = 'B2recomp/deps/python/lib/python3.13/site-packages/'
    for w in wheels:                                          # a wheel is a zip of site-packages content
        with zipfile.ZipFile(w) as zw:
            for n in zw.namelist():
                if not n.endswith('/'):
                    a.put(site + n, zw.read(n), 0o755 if n.endswith('.so') or '.so.' in n else 0o644)
    sdl = os.path.join(ld, 'SDL3')
    for dp, dn, fn in os.walk(os.path.join(sdl, 'include')):
        for n in sorted(fn):
            p = os.path.join(dp, n)
            with open(p, 'rb') as f:
                a.put('B2recomp/deps/SDL3/' + os.path.relpath(p, sdl).replace(os.sep, '/'), f.read())
    for src, arc in ((need['sdl'], 'deps/SDL3/lib/libSDL3.so.0'), (need['shim'], 'deps/libssbu_glslang.so')):
        with open(src, 'rb') as f:
            a.put('B2recomp/' + arc, f.read(), 0o755)
    with open(os.path.join(sdl, 'LICENSE.txt'), 'rb') as f:
        a.put('B2recomp/deps/SDL3/LICENSE.txt', f.read())
    for dp, dn, fn in os.walk(os.path.join(vkh, 'include')):
        for n in sorted(fn):
            p = os.path.join(dp, n)
            with open(p, 'rb') as f:
                a.put('B2recomp/deps/Vulkan-Headers/' + os.path.relpath(p, vkh).replace(os.sep, '/'), f.read())
    with open(os.path.join(vkh, 'LICENSE.md'), 'rb') as f:
        a.put('B2recomp/deps/Vulkan-Headers/LICENSE.md', f.read())
    a.close()
    os.replace(name + '.part', name)
    print('linux package: %s: %d files (%d ours, %d source; %d symlinks of the tool archives left out), %.0f MB unpacked, '
          '%.0f MB packed, %.0f s; Tk with Xft: %s'
          % (name, a.n, nours, nsrc, skipped[0], a.size / 2 ** 20, os.path.getsize(name) / 2 ** 20, time.time() - t0,
             'yes (bitmap-font Tk kept for PCs without libXft)' if have_tk else 'no (the bundled Tk: bitmap fonts only)'))
    return 0


def main():
    if '--linux' in sys.argv[1:]:
        return main_linux()
    out = next((os.path.abspath(a[6:]) for a in sys.argv[1:] if a.startswith('--out=')), os.path.join(ROOT, 'build', 'package'))
    pkg = os.path.join(out, 'B2recomp')
    if os.path.exists(pkg):
        used = [n for n in os.listdir(pkg) if n.lower() in ('build', 'targets', 'dump')]
        if used:                                             # someone unpacked / built in it: never delete their files
            sys.exit('%s holds %s (a build was made there): move it away or choose another --out=<folder>' % (pkg, ', '.join(used)))
        shutil.rmtree(pkg)
    os.makedirs(pkg)
    lic = os.path.join(pkg, 'licenses')
    os.makedirs(lic)
    nsrc = source_tree(pkg)
    with open(os.path.join(ROOT, 'scripts', 'package', 'build.bat'), 'rb') as f:
        bat = f.read().replace(b'\r\n', b'\n').replace(b'\n', b'\r\n')
    with open(os.path.join(pkg, 'Build B2recomp.bat'), 'wb') as f:
        f.write(bat)
    # the launcher, ready-made: its build screen runs setup.py; no icon (none made from a dump may get into the package)
    r = subprocess.run([sys.executable, os.path.join(ROOT, 'scripts', 'build_launcher.py'), '--out=' + os.path.join(pkg, 'B2recomp.exe'),
                        '--no-icon'], cwd=ROOT, capture_output=True, text=True)
    if r.returncode:
        sys.exit('B2recomp.exe: build_launcher.py failed\n' + r.stdout[-2000:] + r.stderr[-2000:])
    os.remove(os.path.join(pkg, 'COPYING.MinGW-w64-runtime.txt'))        # build_launcher.py copied it; it is in licenses\ (below)
    print('B2recomp.exe: the launcher with its build screen')
    d = os.path.join(pkg, 'deps')

    lm = find('LLVM_MINGW', 'llvm-mingw', 'bin/x86_64-w64-mingw32-clang.exe', 'llvm-mingw')
    for f in LLVM_BIN:
        copy(os.path.join(lm, 'bin', f), os.path.join(d, 'llvm-mingw', 'bin', f))
    cv = os.listdir(os.path.join(lm, 'lib', 'clang'))[0]
    copytree(os.path.join(lm, 'lib', 'clang', cv, 'include'), os.path.join(d, 'llvm-mingw', 'lib', 'clang', cv, 'include'))
    rt = os.path.join('lib', 'clang', cv, 'lib', 'windows', 'libclang_rt.builtins-x86_64.a')
    copy(os.path.join(lm, rt), os.path.join(d, 'llvm-mingw', rt))
    copytree(os.path.join(lm, 'include'), os.path.join(d, 'llvm-mingw', 'include'))
    copytree(os.path.join(lm, 'x86_64-w64-mingw32'), os.path.join(d, 'llvm-mingw', 'x86_64-w64-mingw32'))
    copy(os.path.join(lm, 'LICENSE.TXT'), os.path.join(d, 'llvm-mingw', 'LICENSE.TXT'))
    copy(os.path.join(lm, 'LICENSE.TXT'), os.path.join(lic, 'llvm-mingw-LICENSE.TXT'))
    copy(os.path.join(lm, 'x86_64-w64-mingw32', 'share', 'mingw32', 'COPYING.MinGW-w64-runtime.txt'),
         os.path.join(lic, 'COPYING.MinGW-w64-runtime.txt'))
    clang = subprocess.run([os.path.join(lm, 'bin', 'clang.exe'), '--version'], capture_output=True, text=True).stdout.split()
    clang = clang[clang.index('version') + 1] if 'version' in clang else '?'
    print('llvm-mingw: %s (clang %s), trimmed' % (lm, clang))

    sdl = find('SDL3_ROOT', 'SDL3', 'lib/x64/SDL3.lib', 'SDL3')
    copytree(os.path.join(sdl, 'include'), os.path.join(d, 'SDL3', 'include'))
    for f in ('SDL3.lib', 'SDL3.dll'):
        copy(os.path.join(sdl, 'lib', 'x64', f), os.path.join(d, 'SDL3', 'lib', 'x64', f))
    copy(os.path.join(sdl, 'LICENSE.txt'), os.path.join(d, 'SDL3', 'LICENSE.txt'))
    copy(os.path.join(sdl, 'LICENSE.txt'), os.path.join(lic, 'SDL3-LICENSE.txt'))
    vkh = find('VULKAN_HEADERS', 'Vulkan-Headers', 'include/vulkan/vulkan.h', 'Vulkan-Headers')
    copytree(os.path.join(vkh, 'include'), os.path.join(d, 'Vulkan-Headers', 'include'))
    copy(os.path.join(vkh, 'LICENSE.md'), os.path.join(d, 'Vulkan-Headers', 'LICENSE.md'))
    copy(os.path.join(vkh, 'LICENSE.md'), os.path.join(lic, 'Vulkan-Headers-LICENSE.md'))
    if os.path.isdir(os.path.join(vkh, 'LICENSES')):
        copytree(os.path.join(vkh, 'LICENSES'), os.path.join(lic, 'Vulkan-Headers-LICENSES'))
    print('SDL3: %s; Vulkan-Headers: %s' % (sdl, vkh))

    gd = next((f for f in (os.environ.get('GLSLANG_DLL'), os.path.join(DEPS, 'ssbu_glslang.dll'), os.path.join(ROOT, 'build', 'ssbu_glslang.dll'))
               if f and os.path.isfile(f)), None)
    if not gd:
        sys.exit('ssbu_glslang.dll not found: build it once (python scripts/build_runtime.py with glslang + MSVC) or set GLSLANG_DLL')
    copy(gd, os.path.join(d, 'ssbu_glslang.dll'))
    crt = msvc_crt_dir()
    for f in MSVC_CRT:
        copy(os.path.join(crt, f), os.path.join(d, f))
    for f in ('glslang-LICENSE.txt', 'SPIRV-Tools-LICENSE.txt'):
        copy(os.path.join(ROOT, 'scripts', 'package', 'licenses', f), os.path.join(lic, f))
    crt_ver = os.path.basename(os.path.dirname(os.path.dirname(crt)))
    print('ssbu_glslang.dll: %s; MSVC runtime %s: %s' % (gd, crt_ver, crt))

    pyver, pkgs = python_runtime(os.path.join(d, 'python'))
    copy(os.path.join(d, 'python', 'LICENSE.txt'), os.path.join(lic, 'Python-LICENSE.txt'))
    site = os.path.join(d, 'python', 'Lib', 'site-packages')
    for name, pattern in (('lz4', 'lz4-*.dist-info'), ('Pillow', 'pillow-*.dist-info')):
        found = [p for p in glob.glob(os.path.join(site, pattern, 'LICENSE*')) + glob.glob(os.path.join(site, pattern, 'licenses', 'LICENSE*'))
                 if os.path.isfile(p)]
        if found:
            copy(found[0], os.path.join(lic, name + '-LICENSE'))
        else:
            print('warning: no licence file found for %s in its dist-info' % name)

    with open(os.path.join(pkg, 'THIRD-PARTY-NOTICES.md'), 'w', encoding='utf-8', newline='\n') as f:
        f.write(NOTICES.format(llvm_mingw=PINNED['llvm-mingw'], clang=clang, python=pyver, lz4=pkgs['lz4'], pillow=pkgs['pillow'],
                               sdl3=PINNED['SDL3'], vkh=PINNED['Vulkan-Headers'], glslang=PINNED['glslang'], crt=crt_ver))

    # nothing of the game or of a local build may be in it
    bad, total, size = [], 0, 0
    for dp, dn, fn in os.walk(pkg):
        for n in fn:
            p = os.path.join(dp, n)
            rel = os.path.relpath(p, pkg).replace(os.sep, '/')
            total += 1
            size += os.path.getsize(p)
            if n.lower().endswith(FORBIDDEN_EXT) or n in FORBIDDEN_NAME or rel.startswith(('build/', 'targets/', 'evidence/', 'notes/')) \
                    or (rel.startswith('runtime/gen/') and rel != 'runtime/gen/make_gen.py') or rel == 'CLAUDE.md':
                bad.append(rel)
    if bad:
        sys.exit('REFUSED: the package would contain %d file(s) that must not be shipped:\n  %s' % (len(bad), '\n  '.join(bad[:30])))
    print('package: %s, %d files (%d source), %.0f MB, checked: no game or build files' % (pkg, total, nsrc, size / 2 ** 20))

    if '--no-zip' not in sys.argv:
        name = os.path.join(out, 'B2recomp-builder-%s' % git('rev-parse', '--short', 'HEAD').strip())
        if os.path.exists(name + '.zip'):
            os.remove(name + '.zip')
        z = shutil.make_archive(name, 'zip', out, 'B2recomp')
        print('zip: %s, %.0f MB' % (z, os.path.getsize(z) / 2 ** 20))
    return 0


if __name__ == '__main__':
    sys.exit(main())
