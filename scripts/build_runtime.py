"""R0: build the host runtime exe and link it against the lifted game DLL (ssbu-recomp's `link` step, our paths).

    python scripts/build_runtime.py [--skip-dll] [--skip-glslang] [--obj=<dir>] [--host-opt=O2] [--out=<dir>] [--toolchain=msvc|mingw]

TOOLCHAIN (scripts/toolchain.py): mingw (default) = llvm-mingw into the places below (game.dll.a instead of game.lib, plus
COPYING.MinGW-w64-runtime.txt); msvc = clang 9 + MSVC link, game.dll from build/obj_msvc, everything into build/out_msvc
(run it with cwd = build/ and the config variables, e.g. scripts/bench.py --exe build/out_msvc/game_native.exe).
ssbu_glslang.dll is built only by the msvc toolchain (step 4); the mingw build uses the one in build/ (MSVC-built, C
interface: notes/toolchain-mingw.md 3.1).

--obj: the lifted objects for game.dll (default build/obj); --host-opt: optimisation of the host sources (default O1);
--out: build game.dll, obj_host/ and game_native.exe there instead of build/ (a variant beside the real build; SDL3.dll and
ssbu_glslang.dll are copied from build/, run it with cwd = build/ and SSBU_GRAPHICS_CONFIG / SSBU_CONFIG set).

  1. build/game.dll (+ .lib): build/obj/*.obj (scripts/compile_main.py) + runtime/game_shim.c
  2. build/obj_host/*.obj:    runtime host sources (their HOST_SOURCES minus the Smash-only files) + gamedll.c
  3. build/game_native.exe:   host objects + game.lib + SDL3 + Windows libraries (+ icon 1 = build/bayonetta2.ico when it exists,
                              scripts/make_icon_local.py); SDL3.dll copied beside it
  4. build/ssbu_glslang.dll:  runtime/build_glslang.bat (MSVC + glslang 16.6.0 release libraries)
"""
import concurrent.futures
import os
import shutil
import subprocess
import sys
import time

import toolchain

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
RT, GENRT = os.path.join(ROOT, 'runtime'), os.path.join(ROOT, 'runtime', 'gen')
BUILD, OBJ, OBJH = os.path.join(ROOT, 'build'), toolchain.OBJ, os.path.join(toolchain.OUT, 'obj_host')
CLANG = toolchain.CLANG
MINGW = toolchain.MINGW
# libraries: environment variable, else <repo>/deps/<name> (toolchain.locate; scripts/setup.py sets the variables)
SDL3 = toolchain.need('SDL3_ROOT', 'SDL3', 'lib/x64/SDL3.lib', 'SDL3 3.4.18 (the VC development package)')
VKH = toolchain.need('VULKAN_HEADERS', 'Vulkan-Headers', 'include/vulkan/vulkan.h', 'Khronos Vulkan-Headers (vulkan-sdk-1.4.341.0)')
# ssbu_glslang.dll (step 4) needs MSVC + the glslang release, or a ready-made one: GLSLANG_DLL=<file> or deps/ssbu_glslang.dll
GLSLANG = toolchain.locate('GLSLANG_ROOT', 'glslang', 'lib/glslang.lib')
GLSLANG_DLL = next((f for f in (os.environ.get('GLSLANG_DLL'), os.path.join(toolchain.DEPS, 'ssbu_glslang.dll'))
                    if f and os.path.isfile(f)), None)
MSVC_CRT = ('msvcp140.dll', 'vcruntime140.dll', 'vcruntime140_1.dll')   # what an MSVC-built (/MD) ssbu_glslang.dll imports

# ssbu-recomp tools/lifter/build.py HOST_SOURCES at 3a96916, minus Smash-only files removed in R0
# (nnro.c, nro_registry.c, r1_state.c, debug_mode.c; stand-ins in r0_removed.c) and nn::mii (nnmii.c: our game imports none of it).
HOST_SOURCES = ['hostrt.c', 'boot.c', 'nnsdk.c', 'libcxx.c', 'os_threads.c', 'nvn_stub.c', 'nvn_gl.c', 'nvn_vk.c',
                'mx_translate.c', 'tex_decode.c', 'nnfs.c', 'clib.c', 'nntime.c', 'nnhid.c', 'nnpad_sdl.c', 'gen/sdkdata.c',
                'display.c', 'nnpl.c', 'nnfontll.c', 'nnmovie.c', 'nnmisc.c', 'nnaudio.c', 'nnmovie_mf.c', 'nniostream.c',
                'nncxxrt.c', 'nncxxlib.c', 'nnlibc2.c', 'gen/sdklift_gen.c', 'sdklift_rt.c', 'nnsdk2.c', 'nnapplet.c',
                'nnalbum.c', 'nnnet.c', 'nnfsdir.c', 'nnunwind.c', 'nvn_sdk.c', 'nvn_c4.c', 'nvn_c5.c', 'gfx_options.c',
                'hitch.c', 'r0_removed.c', 'r2_imports.c', 'watch.c', 'gamedll.c']
HOST_FLAGS = ['-O1', '-g', '-gcodeview', '-w', '-fno-strict-aliasing', '-mcx16', '-I', RT, '-I', GENRT,   # debug info: crash addresses -> source (llvm-symbolizer)
              '-Dfn_table=(*game_fn_table_p)', '-Dfn_table_count=(*game_fn_table_count_p)',
              '-Ddyn_syms=(*game_dyn_syms_p)', '-Ddyn_sym_count=(*game_dyn_sym_count_p)',
              '-I', os.path.join(SDL3, 'include'), '-I', os.path.join(VKH, 'include')]
LIBS = ['-luser32', '-lgdi32', '-lwinmm', '-lmfplat', '-lmfreadwrite', '-lmfuuid', '-lole32', '-lshlwapi']
if MINGW:
    HOST_FLAGS += ['-D_WIN32_WINNT=0x0A00']               # mingw-w64 defaults to 0x601 (Windows 7); the Windows SDK to 0x0A00
    # warning guard instead of a blanket -w (finding 100): -w hid "unknown attribute 'thread' ignored" (mingw-w64 turns
    # __declspec(thread) into __attribute__((thread))), and every thread-local became one shared global (toolchain-mingw.md
    # fix 9). -w also wins over a later -Werror=, so: all warnings off, then these three on as errors (clang 23 already makes
    # the last two errors by default; named so that an older compiler or a changed default cannot drop them)
    HOST_FLAGS[HOST_FLAGS.index('-w')] = '-Wno-everything'
    HOST_FLAGS += ['-Werror=unknown-attributes', '-Werror=implicit-function-declaration',
                   '-Werror=incompatible-function-pointer-types']
    LIBS += ['-lopengl32', '-lws2_32', '-ladvapi32', '-luuid']   # #pragma comment(lib) (nvn_gl.c, nnnet.c, nnmisc.c) and MSVC's
                                                          # default uuid.lib (GUID_NULL): not applied in mingw mode
DLL, EXE = os.path.join(toolchain.OUT, 'game.dll'), os.path.join(toolchain.OUT, 'game_native.exe')
IMPLIB = lambda dll: dll[:-4] + ('.dll.a' if MINGW else '.lib')     # game.dll's import library
ICO = os.path.join(BUILD, 'bayonetta2.ico')


def run(cmd, **kw):
    r = subprocess.run(cmd, capture_output=True, text=True, errors='replace', **kw)
    return r.returncode, r.stdout + r.stderr


def link_dll():
    objs = sorted(os.path.join(OBJ, n) for n in os.listdir(OBJ) if n.endswith('.obj'))
    rsp = os.path.join(os.path.dirname(DLL), 'game_dll.rsp')
    with open(rsp, 'w') as f:
        f.writelines('"%s"\n' % o.replace('\\', '/') for o in objs)
    t = time.time()
    # a path goes to the linker whole, through -Xlinker: -Wl, cuts its text at every comma, so an install folder such as
    # "F:\Games, Beta Builds\..." became several linker arguments (finding 149)
    code, out = run([CLANG, '-shared', '-O1', '-w', '-mcx16', '-I', RT, os.path.join(RT, 'game_shim.c'), '@' + rsp, '-o', DLL] +
                    (['-Xlinker', '--out-implib=' + IMPLIB(DLL)] if MINGW else []))
    print('game.dll: exit %d in %.0fs, %d objects' % (code, time.time() - t, len(objs)))
    if code:
        print(out[-4000:])
    return code


def compile_host(name):
    src = os.path.join(RT, name)
    obj = os.path.join(OBJH, os.path.basename(name)[:-2] + '.obj')
    code, out = run([CLANG, '-c'] + HOST_FLAGS + [src, '-o', obj])
    return name, obj, code, out


def main():
    global OBJ, OBJH, DLL, EXE
    out_dir = None
    for a in sys.argv[1:]:
        if a.startswith('--obj='):
            OBJ = os.path.abspath(a[6:])
        elif a.startswith('--host-opt='):
            HOST_FLAGS[0] = '-' + a[11:]
        elif a.startswith('--out='):
            out_dir = os.path.abspath(a[6:])
            OBJH, DLL, EXE = os.path.join(out_dir, 'obj_host'), os.path.join(out_dir, 'game.dll'), os.path.join(out_dir, 'game_native.exe')
    os.makedirs(OBJH, exist_ok=True)
    if '--skip-dll' not in sys.argv and link_dll():
        return 1
    t = time.time()
    objs, failed = [], []
    with concurrent.futures.ThreadPoolExecutor(16) as pool:
        for name, obj, code, out in pool.map(compile_host, HOST_SOURCES):
            objs.append(obj)
            if code:
                failed.append((name, out))
    print('host: %d files compiled in %.0fs, %d failed' % (len(HOST_SOURCES), time.time() - t, len(failed)))
    for name, out in failed:
        print('==== %s\n%s' % (name, out[-2500:]))
    if failed:
        return 1
    res = []
    if os.path.exists(ICO):                                  # icon 1 = the game's icon (scripts/make_icon_local.py), loaded by display.c
        rc, rs = os.path.join(os.path.dirname(EXE), 'bayonetta2_icon.rc'), os.path.join(os.path.dirname(EXE), 'bayonetta2_icon.res')
        with open(rc, 'w') as f:
            f.write('1 ICON "%s"\n' % ICO.replace('\\', '/'))
        if not run([toolchain.LLVM_RC, '/fo', rs, rc])[0]:
            res = [rs] if MINGW else ['-Xlinker', rs]        # lld in mingw mode takes the .res as an ordinary input
    # fixed base 0x140000000 + PDB for crash symbolising (paths through -Xlinker, as in link_dll: commas, finding 149)
    debug = ['-g', '-Xlinker', '--pdb=' + EXE[:-4] + '.pdb', '-Wl,--disable-dynamicbase'] if MINGW else ['-g', '-Wl,/DEBUG', '-Wl,/DYNAMICBASE:NO']
    code, out = run([CLANG] + objs + [IMPLIB(DLL), os.path.join(SDL3, 'lib', 'x64', 'SDL3.lib')] + LIBS + res + debug + ['-o', EXE])
    print('game_native.exe: link exit %d' % code)
    if code:
        print(out[-6000:])
        return code
    shutil.copy2(os.path.join(SDL3, 'lib', 'x64', 'SDL3.dll'), os.path.dirname(EXE))
    print('%s: %.1f MB (TOOLCHAIN=%s)' % (EXE, os.path.getsize(EXE) / 2 ** 20, toolchain.NAME))
    toolchain.copy_runtime_licence(os.path.dirname(EXE))
    return glslang_dll()


def glslang_dll():
    # ssbu_glslang.dll is built only with MSVC (runtime/build_glslang.bat, glslang's MSVC release libraries) and loaded
    # through a C interface, so every build uses the one in build/ (notes/toolchain-mingw.md 3.1); a variant or the msvc
    # build in build/out_msvc gets a copy beside its exe. Where it comes from: GLSLANG_DLL / deps/ssbu_glslang.dll when given,
    # else built here when it is missing (msvc toolchain: rebuilt every time unless --skip-glslang)
    glslang = os.path.join(BUILD, 'ssbu_glslang.dll')
    if GLSLANG_DLL and os.path.normcase(os.path.abspath(GLSLANG_DLL)) != os.path.normcase(glslang):
        shutil.copy2(GLSLANG_DLL, glslang)
        print('ssbu_glslang.dll: copied from %s' % GLSLANG_DLL)
        # the MSVC runtime it needs (it is built /MD): a PC without the Visual C++ Redistributable has no MSVCP140.dll, so
        # the builder package (scripts/make_package.py) puts Microsoft's app-local copies beside it; they go beside the exe
        for crt in MSVC_CRT:
            src = os.path.join(os.path.dirname(os.path.abspath(GLSLANG_DLL)), crt)
            if os.path.exists(src):
                shutil.copy2(src, os.path.join(BUILD, crt))
                print('%s: copied from %s' % (crt, os.path.dirname(src)))
    elif (out_dir_given() or MINGW or '--skip-glslang' in sys.argv) and os.path.exists(glslang):
        pass                                                 # keep the one in build/
    else:
        if not GLSLANG:
            print('ssbu_glslang.dll: missing, and glslang is not found to build it: set GLSLANG_ROOT=<glslang 16.6.0 '
                  'windows release> or put it at deps\\glslang (needs the MSVC Build Tools), or give a built one with '
                  'GLSLANG_DLL=<file> / deps\\ssbu_glslang.dll')
            return 1
        code, out = run(['cmd.exe', '/c', os.path.join(RT, 'build_glslang.bat')], cwd=RT, env=dict(os.environ, GLSLANG_ROOT=GLSLANG))
        if code or not os.path.exists(os.path.join(RT, 'ssbu_glslang.dll')):
            print('ssbu_glslang.dll: FAILED\n' + out[-3000:])
            return 1
        shutil.move(os.path.join(RT, 'ssbu_glslang.dll'), glslang)
        for junk in ('ssbu_glslang.lib', 'ssbu_glslang.exp'):
            if os.path.exists(os.path.join(RT, junk)):
                os.remove(os.path.join(RT, junk))
        print('ssbu_glslang.dll: built')
    if os.path.normcase(os.path.dirname(EXE)) != os.path.normcase(BUILD):
        shutil.copy2(glslang, os.path.dirname(EXE))
        for crt in MSVC_CRT:
            if os.path.exists(os.path.join(BUILD, crt)):
                shutil.copy2(os.path.join(BUILD, crt), os.path.dirname(EXE))
    return 0


def out_dir_given():
    return any(a.startswith('--out=') for a in sys.argv[1:])


if __name__ == '__main__':
    sys.exit(main())
