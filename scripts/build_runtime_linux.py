"""[Recomps#1] The Linux version of build_runtime.py (notes/linux-port-plan.md step 3): game.so and the host runtime.

    python scripts/build_runtime_linux.py [--skip-so] [--host-opt=O1] [--jobs=N]

Always TOOLCHAIN=linux (scripts/toolchain.py: zig cc from Windows, clang on Linux); everything goes to build-linux/, never
build/ (the Windows build there stays as it is):
  1. build-linux/game.so:        build-linux/obj/*.o (TOOLCHAIN=linux scripts/compile_main.py) + runtime/game_shim.c
  2. build-linux/obj_host/*.o:   build_runtime.py's HOST_SOURCES + runtime/linux/win32_compat.c, with runtime/linux/ first on
                                 the include path (its windows.h: the Win32 subset the runtime uses, on POSIX)
  3. build-linux/game_native:    host objects + game.so + SDL3 + libdl / pthread / libm, when every host file compiled and a
                                 Linux SDL3 library is at hand (SDL3_LINUX_LIB=<folder with libSDL3.so>, deps/SDL3/lib, else
                                 the system's; none from Windows: there the script stops after the compile report); a
                                 libSDL3.so.0 from a folder is copied beside it. No libGL (runtime/linux/gl_stubs.c).
  4. build-linux/libssbu_glslang.so: GLSLANG_SO / deps/libssbu_glslang.so copied (the package's, scripts/package/
                                 linux_deps.sh), else runtime/vk_glslang_shim.c + the system's glslang (pacman -S glslang; 16.6.0 like the
                                 Windows release), loaded by the Vulkan renderer as "ssbu_glslang.dll" (win32_compat.c's
                                 library table); GLSLANG_LINUX_ROOT=<folder with include/ and lib/> for another one.
Until the shape-different files are ported (window, sound, OpenGL, Media Foundation ...; plan steps 4-8) step 2 reports
which host files do not compile yet and step 3 is skipped; the exit code is 0 when the rest worked (--strict: 1).
"""
import ast
import concurrent.futures
import os
import shutil
import subprocess
import sys
import time

os.environ['TOOLCHAIN'] = 'linux'
import toolchain                                         # noqa: E402  (after TOOLCHAIN is set)

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
RT, GENRT, RTLX = os.path.join(ROOT, 'runtime'), os.path.join(ROOT, 'runtime', 'gen'), os.path.join(ROOT, 'runtime', 'linux')
OUT = toolchain.OUT
OBJ, OBJH = toolchain.OBJ, os.path.join(OUT, 'obj_host')
SO, EXE = os.path.join(OUT, 'game.so'), os.path.join(OUT, 'game_native')
CC = toolchain.CC


def host_sources():
    """build_runtime.py's HOST_SOURCES, read from its text (importing it would look for the Windows SDL3 package)."""
    tree = ast.parse(open(os.path.join(ROOT, 'scripts', 'build_runtime.py'), encoding='utf-8').read())
    for node in tree.body:
        if isinstance(node, ast.Assign) and any(getattr(t, 'id', None) == 'HOST_SOURCES' for t in node.targets):
            return list(ast.literal_eval(node.value))
    raise SystemExit('HOST_SOURCES not found in scripts/build_runtime.py')


def headers(env, sub, probe, system):
    """Include folder of a header-only dependency: $env, deps/<sub>, else the system's (on Linux)."""
    d = toolchain.locate(env, sub, probe)
    if d:
        return os.path.join(d, 'include')
    if os.path.exists(os.path.join(system, probe.split('/', 1)[1])):
        return system
    raise SystemExit('%s not found: set %s=<folder> or put it at deps/%s' % (probe, env, sub))


SDL3_INC = headers('SDL3_ROOT', 'SDL3', 'include/SDL3/SDL.h', '/usr/include')
VK_INC = headers('VULKAN_HEADERS', 'Vulkan-Headers', 'include/vulkan/vulkan.h', '/usr/include')
HOST_FLAGS = ['-O1', '-g', '-w', '-fno-strict-aliasing', '-mcx16', '-D_GNU_SOURCE'] + toolchain.EXTRA + [
    '-I', RTLX, '-I', RT, '-I', GENRT,
    '-Dfn_table=(*game_fn_table_p)', '-Dfn_table_count=(*game_fn_table_count_p)',
    '-Ddyn_syms=(*game_dyn_syms_p)', '-Ddyn_sym_count=(*game_dyn_sym_count_p)',
    '-I', SDL3_INC, '-I', VK_INC]


def run(cmd):
    r = subprocess.run(cmd, capture_output=True, text=True, errors='replace')
    return r.returncode, r.stdout + r.stderr


def link_so():
    objs = sorted(os.path.join(OBJ, n) for n in os.listdir(OBJ) if n.endswith('.o'))
    if not objs:
        print('game.so: no objects in %s: run TOOLCHAIN=linux python scripts/compile_main.py first' % OBJ)
        return 1
    rsp = os.path.join(OUT, 'game_so.rsp')
    with open(rsp, 'w') as f:
        f.writelines('"%s"\n' % o.replace('\\', '/') for o in objs)
    t = time.time()
    code, out = run(CC + ['-shared', '-O1', '-w', '-mcx16'] + toolchain.EXTRA + toolchain.LIFTED_EXTRA + ['-I', RT, os.path.join(RT, 'game_shim.c'),
                                                                                '@' + rsp, '-Wl,-soname,game.so', '-o', SO])
    print('game.so: exit %d in %.0fs, %d objects%s' % (code, time.time() - t, len(objs),
                                                      ', %.0f MB' % (os.path.getsize(SO) / 2 ** 20) if not code else ''))
    if code:
        print(out[-4000:])
    return code


def compile_host(name):
    src = os.path.join(RT, name)
    obj = os.path.join(OBJH, os.path.basename(name)[:-2] + '.o')
    code, out = run(CC + ['-c'] + HOST_FLAGS + [src, '-o', obj])
    return name, obj, code, out


def sdl3_dir():
    """A folder with a Linux libSDL3.so: SDL3_LINUX_LIB, else deps/SDL3/lib (the Linux builder package's, built for glibc
    2.35 by scripts/package/linux_deps.sh), else None."""
    for d in (os.environ.get('SDL3_LINUX_LIB'), os.path.join(toolchain.DEPS, 'SDL3', 'lib')):
        if d and (os.path.exists(os.path.join(d, 'libSDL3.so')) or os.path.exists(os.path.join(d, 'libSDL3.so.0'))):
            return d
    return None


def sdl3_lib():
    d = sdl3_dir()
    if d:                                                # by path when there is no libSDL3.so link (the package: no symlinks)
        so0 = os.path.join(d, 'libSDL3.so.0')
        return [so0] if os.path.exists(so0) else ['-L' + d, '-lSDL3']
    if os.name != 'nt':                                  # on Linux: the system's (pacman -S sdl3, apt install libsdl3-dev ...)
        return ['-lSDL3']
    return None


def glslang_so():
    out_so = os.path.join(OUT, 'libssbu_glslang.so')
    ready = next((f for f in (os.environ.get('GLSLANG_SO'), os.path.join(toolchain.DEPS, 'libssbu_glslang.so')) if f and os.path.isfile(f)), None)
    if ready:                                            # the Linux builder package's (glslang inside, needs only glibc)
        shutil.copy2(ready, out_so)
        print('libssbu_glslang.so: copied from %s' % ready)
        return 0
    root = os.environ.get('GLSLANG_LINUX_ROOT')
    inc = os.path.join(root, 'include') if root else '/usr/include'
    if not os.path.exists(os.path.join(inc, 'glslang', 'Include', 'glslang_c_interface.h')):
        print('libssbu_glslang.so: not built (no glslang here: pacman -S glslang / apt install glslang-dev, or '
              'GLSLANG_LINUX_ROOT=<folder>)')
        return 0
    lib = ['-L' + os.path.join(root, 'lib'), '-Wl,-rpath,$ORIGIN'] if root else []
    code, out = run(CC + ['-shared', '-fPIC', '-O2', '-w', '-I', inc, os.path.join(RT, 'vk_glslang_shim.c')] + lib +
                    ['-lglslang', '-lglslang-default-resource-limits', '-lpthread', '-o', out_so])
    print('libssbu_glslang.so: exit %d' % code)
    if code:
        print(out[-3000:])
    return code


def main():
    jobs = next((int(a[7:]) for a in sys.argv[1:] if a.startswith('--jobs=')), 16)
    for a in sys.argv[1:]:
        if a.startswith('--host-opt='):
            HOST_FLAGS[0] = '-' + a[11:]
    os.makedirs(OBJH, exist_ok=True)
    if '--skip-so' not in sys.argv and link_so():
        return 1
    srcs = host_sources() + ['linux/win32_compat.c', 'linux/waveout_sdl.c', 'linux/gl_stubs.c']
    t = time.time()
    objs, failed = [], []
    with concurrent.futures.ThreadPoolExecutor(jobs) as pool:
        for name, obj, code, out in pool.map(compile_host, srcs):
            if code:
                failed.append((name, out))
            else:
                objs.append(obj)
    print('host: %d of %d files compiled for Linux in %.0fs' % (len(objs), len(srcs), time.time() - t))
    if failed:
        print('not ported yet (notes/linux-port-plan.md steps 4-8): ' + ' '.join(n for n, _ in failed))
        for name, out in failed[:2]:
            first = [l for l in out.splitlines() if 'error:' in l][:3]
            print('  %s: %s' % (name, ' | '.join(first)[:400]))
        return 1 if '--strict' in sys.argv else 0
    lib = sdl3_lib()
    if lib is None:
        print('game_native: not linked (no Linux SDL3 library here: set SDL3_LINUX_LIB=<folder with libSDL3.so>, or build on Linux)')
        return 0
    # no libGL: nvn_gl.c's GL 1.1 calls link to runtime/linux/gl_stubs.c (the OpenGL renderer is Windows only)
    code, out = run(CC + objs + [SO] + lib + ['-ldl', '-lpthread', '-lm', '-Wl,-rpath,$ORIGIN', '-o', EXE])
    print('game_native: link exit %d' % code)
    if code:
        print(out[-6000:])
        return code
    print('%s: %.1f MB (TOOLCHAIN=linux)' % (EXE, os.path.getsize(EXE) / 2 ** 20))
    d = sdl3_dir()
    if d:                                                # beside the program (rpath $ORIGIN): the player needs no SDL3 of their own
        shutil.copy2(os.path.join(d, 'libSDL3.so.0') if os.path.exists(os.path.join(d, 'libSDL3.so.0')) else
                     os.path.join(d, 'libSDL3.so'), os.path.join(OUT, 'libSDL3.so.0'))
        print('libSDL3.so.0: copied from %s' % d)
    return glslang_so()


if __name__ == '__main__':
    sys.exit(main())
