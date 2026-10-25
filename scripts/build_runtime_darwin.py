"""[Recomps#1] The macOS version of build_runtime.py (notes/macos-port-plan.md step 1): outputs build-darwin/, reports
not-yet-ported host files and exits 0 unless --strict. Mirrors scripts/build_runtime_linux.py."""
import ast
import concurrent.futures
import os
import shutil
import subprocess
import sys
import time

os.environ['TOOLCHAIN'] = 'darwin'
import toolchain

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
RT, GENRT, RTLX = os.path.join(ROOT, 'runtime'), os.path.join(ROOT, 'runtime', 'gen'), os.path.join(ROOT, 'runtime', 'darwin')
OUT = toolchain.OUT
OBJ, OBJH = toolchain.OBJ, os.path.join(OUT, 'obj_host')
SO, EXE = os.path.join(OUT, 'game.dylib'), os.path.join(OUT, 'game_native')

CC = toolchain.CC

def host_sources():
    tree = ast.parse(open(os.path.join(ROOT, 'scripts', 'build_runtime.py'), encoding='utf-8').read())
    for node in tree.body:
        if isinstance(node, ast.Assign) and any(getattr(t, 'id', None) == 'HOST_SOURCES' for t in node.targets):
            return list(ast.literal_eval(node.value))
    raise SystemExit('HOST_SOURCES not found in scripts/build_runtime.py')

def headers(env, sub, probe, system):
    d = toolchain.locate(env, sub, probe)
    if d:
        return os.path.join(d, 'include')
    if os.path.exists(os.path.join(system, probe.split('/', 1)[1])):
        return system
    raise SystemExit('%s not found: set %s=<folder> or put it at deps/%s' % (probe, env, sub))

BREW = subprocess.run(['brew', '--prefix'], capture_output=True, text=True).stdout.strip() or '/opt/homebrew'   # a system query, not a path baked in
SDL3_INC = headers('SDL3_ROOT', 'SDL3', 'include/SDL3/SDL.h', os.path.join(BREW, 'include'))
VK_INC = headers('VULKAN_HEADERS', 'Vulkan-Headers', 'include/vulkan/vulkan.h', os.path.join(BREW, 'include'))
HOST_FLAGS = ['-O1', '-g', '-w', '-fno-strict-aliasing', '-fdeclspec', '-fPIC'] + toolchain.EXTRA + [
    '-I', RTLX, '-I', RT, '-I', GENRT,
    '-Dfn_table=(*game_fn_table_p)', '-Dfn_table_count=(*game_fn_table_count_p)',
    '-Ddyn_syms=(*game_dyn_syms_p)', '-Ddyn_sym_count=(*game_dyn_sym_count_p)',
    '-I', SDL3_INC, '-I', VK_INC]

def run(cmd):
    r = subprocess.run(cmd, capture_output=True, text=True, errors='replace')
    return r.returncode, r.stdout + r.stderr

def link_so():
    """Link game.dylib: lifted objects + game_shim.c"""
    lifted_dir = toolchain.OBJ
    if not os.path.isdir(lifted_dir) or not os.listdir(lifted_dir):
        print('game.dylib: no lifted objects in %s: run TOOLCHAIN=darwin python scripts/compile_main.py first' % lifted_dir)
        return 1
    objs = sorted(os.path.join(lifted_dir, n) for n in os.listdir(lifted_dir) if n.endswith('.o'))
    rsp = os.path.join(OUT, 'game_dylib.rsp')
    with open(rsp, 'w') as f:
        f.writelines('"%s"\n' % o.replace('\\', '/') for o in objs)
    t = time.time()
    sdl3_libs = []
    sdl3_dylib = os.path.join(os.environ.get('SDL3_ROOT', '/opt/homebrew'), 'lib', 'libSDL3.dylib')
    if os.path.isfile(sdl3_dylib):
        sdl3_libs = [sdl3_dylib, '-framework', 'Cocoa', '-framework', 'IOKit', '-framework', 'CoreVideo', '-framework', 'OpenGL']
    code, out = run(CC + ['-shared', '-O1', '-w', '-fPIC'] + toolchain.EXTRA + toolchain.LIFTED_EXTRA + ['-I', RT, os.path.join(RT, 'game_shim.c'),
                               '@' + rsp] + sdl3_libs + ['-Wl,-install_name,game.dylib', '-o', SO])
    print('game.dylib: exit %d in %.0fs, %d objects%s' % (code, time.time() - t, len(objs),
                                                      ', %.0f MB' % (os.path.getsize(SO) / 2 ** 20) if not code else ''))
    if code:
        print(out[-4000:])
    return code

def link_exe():
    """Link game_native: host objects + game.dylib"""
    objs = sorted(os.path.join(OBJH, n) for n in os.listdir(OBJH) if n.endswith('.o') and n != 'compat_test.o')
    if not objs:
        print('game_native: no host objects in %s' % OBJH)
        return 1
    rsp = os.path.join(OUT, 'game_exe.rsp')
    with open(rsp, 'w') as f:
        f.writelines('"%s"\n' % o.replace('\\', '/') for o in objs)
    t = time.time()
    sdl3_libs = []
    sdl3_dylib = os.path.join(os.environ.get('SDL3_ROOT', '/opt/homebrew'), 'lib', 'libSDL3.dylib')
    if os.path.isfile(sdl3_dylib):
        sdl3_libs = [sdl3_dylib, '-framework', 'Cocoa', '-framework', 'IOKit', '-framework', 'CoreVideo', '-framework', 'OpenGL']
    # link host exe against game.dylib which exports game_bind()
    game_lib = os.path.join(OUT, 'game.dylib')
    if not os.path.isfile(game_lib):
        print('game_native: game.dylib missing at %s: link_so() must run first' % game_lib)
        return 1
    code, out = run(CC + ['-O1', '-w', '-fPIC'] + toolchain.EXTRA + ['-I', RT] + ['@' + rsp] + sdl3_libs + [game_lib, '-framework', 'CoreFoundation', '-o', EXE])
    print('game_native: exit %d in %.0fs, %d objects%s' % (code, time.time() - t, len(objs),
                                                      ', %.0f MB' % (os.path.getsize(EXE) / 2 ** 20) if not code else ''))
    if code:
        print(out[-4000:])
    return code

def compile_host(name):
    src = os.path.join(RT, name)
    obj = os.path.join(OBJH, os.path.basename(name)[:-2] + '.o')
    code, out = run(CC + ['-c'] + HOST_FLAGS + [src, '-o', obj])
    return name, obj, code, out

def sdl3_dir():
    d = os.path.join(toolchain.DEPS, 'SDL3', 'lib')
    if os.path.exists(os.path.join(d, 'libSDL3.so')):
        return d
    return None

def sdl3_lib():
    d = sdl3_dir()
    if d:
        so0 = os.path.join(d, 'libSDL3.so.0')
        return [so0] if os.path.exists(so0) else ['-L' + d, '-lSDL3']
    return None

def glslang_so():
    """build-darwin/libssbu_glslang.so: the GLSL->SPIR-V translator the Vulkan renderer loads as "ssbu_glslang.dll"
    (notes/macos-port-plan.md step 6). GLSLANG_SO / deps/libssbu_glslang.so copied, else runtime/vk_glslang_shim.c
    linked against Homebrew's glslang (16.6.0, the version of the Windows release and of the Linux builder)."""
    out_so = os.path.join(OUT, 'libssbu_glslang.so')
    ready = next((f for f in (os.environ.get('GLSLANG_SO'), os.path.join(toolchain.DEPS, 'libssbu_glslang.so'))
                  if f and os.path.isfile(f)), None)
    if ready:
        shutil.copy2(ready, out_so)
        print('libssbu_glslang.so: copied from %s' % ready)
        return 0
    root = os.environ.get('GLSLANG_LINUX_ROOT')            # a glslang of one's own (it has include/ and lib/)
    for prefix in ([root] if root else ['/opt/homebrew', '/usr/local']):      # Homebrew: arm64 then Intel prefixes
        inc = os.path.join(prefix, 'include')
        if not os.path.exists(os.path.join(inc, 'glslang', 'Include', 'glslang_c_interface.h')):
            continue
        code, out = run(CC + ['-shared', '-fPIC', '-O2', '-w', '-I', inc, os.path.join(RT, 'vk_glslang_shim.c'),
                              '-L' + os.path.join(prefix, 'lib'), '-lglslang', '-lglslang-default-resource-limits',
                              '-lpthread', '-o', out_so])
        print('libssbu_glslang.so: exit %d (glslang of %s)' % (code, prefix))
        if code:
            print(out[-3000:])
        return code
    print('libssbu_glslang.so: not built (no glslang here: brew install glslang, or GLSLANG_LINUX_ROOT=<folder>)')
    return 0

def main():
    if '--link' in sys.argv:
        rc1 = link_so()
        rc2 = link_exe()
        return rc1 or rc2 or glslang_so()
    jobs = next((int(a[7:]) for a in sys.argv[1:] if a.startswith('--jobs=')), 16)
    for a in sys.argv[1:]:
        if a.startswith('--host-opt='):
            HOST_FLAGS[0] = '-' + a[11:]
    os.makedirs(OBJH, exist_ok=True)
    srcs = host_sources() + ['darwin/win32_compat.c', 'darwin/waveout_sdl.c', 'darwin/compat_test.c']
    t = time.time()
    objs, failed = [], []
    with concurrent.futures.ThreadPoolExecutor(jobs) as pool:
        for name, obj, code, out in pool.map(compile_host, srcs):
            if code:
                failed.append((name, out))
            else:
                objs.append(obj)
    print('host: %d of %d files compiled for Darwin in %.0fs' % (len(objs), len(srcs), time.time() - t))
    if failed:
        print('not ported yet (notes/macos-port-plan.md steps 4-8): ' + ' '.join(n for n, _ in failed))
        for name, out in failed[:2]:
            first = [l for l in out.splitlines() if 'error:' in l][:3]
            print('  %s: %s' % (name, ' | '.join(first)[:400]))
        return 1 if failed and '--strict' in sys.argv else 0
    return glslang_so()          # [Recomps#1] plan step 6: the GLSL->SPIR-V translator beside the game (libssbu_glslang.so)

if __name__ == '__main__':
    sys.exit(main())