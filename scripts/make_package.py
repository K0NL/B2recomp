"""Make the B2recomp builder package: this source tree plus every tool the build needs, so a player installs nothing
and only adds their own extracted dump.

    python scripts/make_package.py [--out <folder>] [--no-zip]

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


def main():
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
