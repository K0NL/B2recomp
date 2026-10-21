#!/usr/bin/env python3
"""From a fresh clone and YOUR OWN dump to a runnable build, on your PC.

    python scripts/setup.py --dump <folder> [--jobs N] [--only <step> | --from <step>] [--status]
                            [--toolchain mingw|msvc] [--llvm-mingw <dir>] [--llvm <dir>] [--sdl3 <dir>]
                            [--vulkan-headers <dir>] [--glslang <dir> | --glslang-dll <file>] [--accept-modified]
                            [--mods <folder>]

Modelled on ssbu-recomp's scripts/setup.py. <folder> holds what you extracted from your own console and copy of
Bayonetta 2 with your own tools (this project decrypts nothing and reads no keys):

    <folder>/exefs/     main  sdk  subsdk0  rtld  main.npdm          (version 1.2 = update v131072; plain NSO files)
    <folder>/romfs/     data000.cpk ... data006.cpk  movie/  sound/   (version 1.2 = update v131072)
    optional: <folder>/sysdata/fonts        the console's shared fonts, .ttf or .bfttf (without them a PC font stands in)
    optional: <folder>/control/icon_AmericanEnglish.dat   the HOME-menu icon (for the window / launcher icon)

Nothing of the game is in the repository. Every step derives its files from the dump into targets/ and build/ (both
ignored by git; never upload them). Steps, resumable (a finished step is skipped; --from <step> repeats it and the rest):

    deps     Python packages, the compiler, SDL3 / Vulkan-Headers, glslang or a built ssbu_glslang.dll (always runs)
    verify   the dump is the exact version this port handles (SHA-256 / NSO build ID / RomFS manifest); explains a mismatch
    convert  main, sdk (NSO) -> targets/exefs/*.elf (LZ4 only; segment hashes checked); ExeFS copied beside them;
             targets/romfs (and targets/sysdata/fonts) = junctions to the dump's folders, where the game reads them
    mods     optional: YOUR emulator-style code mods (mods/ and its subfolders: *.pchtxt, *.ips) patched into
             targets/exefs/main.elf by scripts/pchtxt.py (always runs, cheap; no mods = main.elf untouched; a changed
             set of patches makes tables, lift, compile, link and check run again). On / off switches: config/mods.ini
             (the launcher's Mods window). compile then rebuilds only the C files the lift changed (seconds, not minutes)
    tables   runtime/gen/* from YOUR sdk and main (layout, imports, lifted SDK functions, SDK constants, test vectors)
    lift     lifter/recomp_eh.py: your main.elf -> build/gen_eh (C); then scripts/fps_patch.py --inplace (the frame-rate
             patch: off at graphics.ini frame_rate = 60, the default)
    compile  scripts/compile_main.py: build/gen_eh -> build/obj (clang -O2; the longest step, ~5 min with 16 threads)
    icon     optional: build/bayonetta2.ico from <folder>/control/icon_AmericanEnglish.dat
    link     scripts/build_runtime.py: build/game.dll, build/game_native.exe (+ SDL3.dll, ssbu_glslang.dll);
             scripts/build_launcher.py: build/launcher.exe
    check    lifter/test_ext.py (needs unicorn; skipped without it)

Tools and libraries: the option, else the environment variable, else <repo>/deps/<name>, else what an earlier run found
(remembered in build/setup_state.json; scripts/toolchain.py looks the same way, without the last one):
    llvm-mingw      --llvm-mingw  LLVM_MINGW      deps/llvm-mingw   llvm-mingw 20260922 UCRT x86_64 (the default toolchain)
    clang 9         --llvm        LLVM_ROOT       deps/llvm         only with --toolchain msvc (+ the MSVC Build Tools)
    SDL3            --sdl3        SDL3_ROOT       deps/SDL3         SDL3 3.4.18, the VC development package
    Vulkan-Headers  --vulkan-headers VULKAN_HEADERS deps/Vulkan-Headers  vulkan-sdk-1.4.341.0
    glslang         --glslang     GLSLANG_ROOT    deps/glslang      glslang 16.6.0 windows release + MSVC Build Tools, to build
                                                                    ssbu_glslang.dll; or a built one:
    ssbu_glslang.dll --glslang-dll GLSLANG_DLL    deps/ssbu_glslang.dll
"""
import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
import time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEPS = os.path.join(REPO, 'deps')
STEPS = ['deps', 'verify', 'convert', 'mods', 'tables', 'lift', 'compile', 'icon', 'link', 'check']
ALWAYS = {'deps', 'mods'}                # cheap: deps fills the tool locations every later step passes on, mods notices
                                         # a changed mods folder

# ---- the one version this port handles (reference: notes/release-audit.md section 6.2, our own dump, 2026-10-16)
VERSION = 'version 1.2 (update v131072, the second update)'   # display version / update version of the game's own NACP / CNMT
EXEFS = {   # name: (size, sha256, NSO build id or None)
    'main':      (13166635, '95b2d654ec9d9fe391f1a5ad991bcb5c0d1ed97105dce46e4360dce1d8a3356e', 'f19b2e01e1794f933a9720c1404bb6f600000000'),
    'sdk':       (4181288,  '2b8824ba37e7ea3e0b7cc9853e5f81b643334374ce3a92efb2a49ae040f77d09', '8ceabb1750539c3874cc9fab45cab335f784cc38'),
    'subsdk0':   (6072185,  '348d4bf6ca468fdabd455ed41bebb6b37b221c5c694cf2a7b4e81fc55feae779', 'a82aa14c5d5a359ad04dc4eab9f5d5901315ee5e'),
    'rtld':      (7099,     'c2d9a6e220bf93bc8196b3ea58080f1cd88cf6820508bd9b10e76dc5bb79b92a', '4044eca5abc5055b1b0bb8b9babe1c9b524a4579'),
    'main.npdm': (1476,     '4b9cbd4e3baf419381d70560cacea255b9c2e5726a82ba779f893e8a22aac702', None),
}
REQUIRED = ('main', 'sdk', 'main.npdm')
KNOWN_OTHER = {   # NSO build id of main -> what it is (for the message)
    '40ea84adf0a7bc26cc7a3b61638f848500000000': 'the base game (v0) without the update to version 1.2',
}
ROMFS_FILES = {   # small enough to hash on every verify
    'data006.cpk': (24378176, 'a6aaa3c1f036eba30011e758ae2b42cdcbb0045ad54ad6bea953ebbcc3e11ac7'),
    'data005.cpk': (183508728, 'ce8fb1b0a7cf2f698554f4f82ac00e8dc04e6afbfb82d6ecc8d296ef0dfa39ac'),
    'sound/WwiseInfo.wai': (113996, '45461675e561ae8fa903467a953a53841b5c366b56e1b8b0b6ccdc044c4dd43f'),
}
ROMFS_MANIFEST = (1643, 13480389777, '4d5e288d43f669353e483f279e325984544906c253d932863b649f8f681c340d')
ENCRYPTED = ('.nsp', '.xci', '.nca', '.nsz', '.xcz')


class Fail(Exception):
    pass


def say(msg=''):
    print(msg, flush=True)


def fmt(secs):
    return '%dm%02ds' % divmod(int(secs), 60) if secs >= 60 else '%.1fs' % secs


def sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for block in iter(lambda: f.read(1 << 20), b''):
            h.update(block)
    return h.hexdigest()


def build_id(path):
    with open(path, 'rb') as f:
        head = f.read(0x54)
    return head[0x40:0x54].hex() if head[:4] == b'NSO0' else None


def romfs_manifest(root):
    """(count, total bytes, sha256 of '<rel path>\\t<size>\\n' lines sorted by byte value) and the {path: size} map."""
    sizes = {}
    for d, _, files in os.walk(root):
        for f in files:
            p = os.path.join(d, f)
            sizes[os.path.relpath(p, root).replace(os.sep, '/')] = os.path.getsize(p)
    lines = sorted(('%s\t%d\n' % kv).encode('utf-8') for kv in sizes.items())
    return (len(sizes), sum(sizes.values()), hashlib.sha256(b''.join(lines)).hexdigest()), sizes


def find_first(cands, probe):
    for c in cands:
        if c and os.path.isdir(c) and os.path.exists(os.path.join(c, probe)):
            return os.path.abspath(c)
    return None


def is_junction(p):
    if hasattr(os.path, 'isjunction'):                       # Python 3.12+
        return os.path.isjunction(p)
    try:
        return bool(os.readlink(p))
    except (OSError, ValueError):
        return False


def link_folder(link, target):
    """link = a junction to target (made again when it points elsewhere). A real folder at link is kept as it is."""
    if os.path.lexists(link):
        if is_junction(link) or os.path.islink(link):
            if os.path.normcase(os.path.realpath(link)) == os.path.normcase(os.path.realpath(target)):
                return 'kept'
            os.rmdir(link)                                   # removes the junction only, never what it points to
        else:
            return 'a real folder, kept (not replaced by the dump\'s)'
    os.makedirs(os.path.dirname(link), exist_ok=True)
    r = subprocess.run(['cmd', '/c', 'mklink', '/J', link, target], capture_output=True, text=True)
    if r.returncode:
        raise Fail('could not make the junction %s -> %s: %s' % (link, target, (r.stdout + r.stderr).strip()))
    return 'junction to ' + target


class Ctx:
    def __init__(self, a):
        self.a = a
        self.build = os.path.join(REPO, 'build')
        self.targets = os.path.join(REPO, 'targets')
        self.logs = os.path.join(self.build, 'logs')
        self.state_file = os.path.join(self.build, 'setup_state.json')
        self.state = {}
        if os.path.exists(self.state_file):
            with open(self.state_file) as f:
                self.state = json.load(f)
        self.dump = os.path.abspath(a.dump) if a.dump else self.state.get('dump')
        self.jobs = a.jobs or min(24, os.cpu_count() or 8)
        self.tools = {}                                      # environment variables for the children, filled by deps

    def save(self):
        os.makedirs(self.build, exist_ok=True)
        with open(self.state_file, 'w') as f:
            json.dump(self.state, f, indent=1)

    def env(self):
        # test switches of the runtime / lifter (SSBU_*, RECOMP_*, RECOMPS_*) never reach the build
        env = {k: v for k, v in os.environ.items() if not k.upper().startswith(('SSBU_', 'RECOMP_', 'RECOMPS_'))}
        env['PYTHONIOENCODING'] = 'utf-8'
        env['PYTHONUNBUFFERED'] = '1'
        env.update(self.tools)
        return env

    def run(self, step, argv, cwd=None):
        os.makedirs(self.logs, exist_ok=True)
        log = os.path.join(self.logs, step + '.log')
        t0 = time.time()
        with open(log, 'ab') as f:
            f.write(('\n$ %s\n' % ' '.join(argv)).encode())
            f.flush()
            start = f.tell()
            p = subprocess.run(argv, cwd=cwd or REPO, env=self.env(), stdout=f, stderr=subprocess.STDOUT)
        with open(log, 'rb') as f:
            f.seek(max(start, os.path.getsize(log) - 6000))
            tail = f.read().decode('utf-8', 'replace')
        if p.returncode:
            raise Fail('%s failed (exit %d) after %s; see %s\n%s' % (' '.join(argv[:3]), p.returncode, fmt(time.time() - t0),
                                                                   log, tail[-1500:]))
        return tail


# ---------------------------------------------------------------------------------------------- steps
def step_deps(cx):
    a = cx.a
    missing = []
    for mod, pip in (('lz4.block', 'lz4'), ('PIL', 'Pillow')):
        try:
            __import__(mod)
        except ImportError:
            missing.append(pip)
    if missing:
        raise Fail('missing Python packages: pip install %s' % ' '.join(missing))
    say('  python %s, packages lz4 Pillow: ok' % sys.version.split()[0])

    saved = cx.state.get('tools', {})                    # what an earlier run found (its options need not be given again)
    name = a.toolchain or os.environ.get('TOOLCHAIN') or saved.get('TOOLCHAIN') or 'mingw'
    if name not in ('mingw', 'msvc'):
        raise Fail('--toolchain must be mingw or msvc, not %r' % name)
    cx.tools['TOOLCHAIN'] = name
    if name == 'mingw':
        root = find_first([a.llvm_mingw, os.environ.get('LLVM_MINGW'), os.path.join(DEPS, 'llvm-mingw'), saved.get('LLVM_MINGW')],
                          os.path.join('bin', 'x86_64-w64-mingw32-clang.exe'))
        if not root:
            raise Fail('llvm-mingw not found (--llvm-mingw <folder>, LLVM_MINGW, or unpack llvm-mingw-20260922-ucrt-x86_64 '
                       'into deps\\llvm-mingw)')
        cx.tools['LLVM_MINGW'] = root
        clang = os.path.join(root, 'bin', 'x86_64-w64-mingw32-clang.exe')
    else:
        root = find_first([a.llvm, os.environ.get('LLVM_ROOT'), os.path.join(DEPS, 'llvm'), saved.get('LLVM_ROOT')], os.path.join('bin', 'clang.exe'))
        if not root:
            raise Fail('LLVM/clang 9.0.0 not found (--llvm <folder>, LLVM_ROOT, or deps\\llvm)')
        cx.tools['LLVM_ROOT'] = root
        clang = os.path.join(root, 'bin', 'clang.exe')
    ver = subprocess.run([clang, '--version'], capture_output=True, text=True).stdout.splitlines()[0]
    say('  compiler (%s): %s   [%s]' % (name, ver, clang))
    probe = os.path.join(cx.build, 'probe')
    os.makedirs(probe, exist_ok=True)
    with open(os.path.join(probe, 'p.c'), 'w') as f:
        f.write('#include <windows.h>\n#include <stdio.h>\nint main(void){puts("ok");return 0;}\n')
    exe = os.path.join(probe, 'p.exe')
    r = subprocess.run([clang, os.path.join(probe, 'p.c'), '-o', exe, '-luser32'], capture_output=True, text=True)
    if r.returncode or subprocess.run([exe], capture_output=True, text=True).stdout.strip() != 'ok':
        hint = 'the llvm-mingw folder is incomplete' if name == 'mingw' else \
               'install the MSVC Build Tools (C++ x64 tools) and a Windows 10/11 SDK'
        raise Fail('the compiler cannot build and run a Windows program: %s\n%s' % (hint, (r.stdout + r.stderr)[-800:]))
    say('  compiler builds and runs a Windows program: ok')

    for env, opt, sub, probe_rel, what in (
            ('SDL3_ROOT', a.sdl3, 'SDL3', os.path.join('lib', 'x64', 'SDL3.lib'), 'SDL3 3.4.18 (VC development package)'),
            ('VULKAN_HEADERS', a.vulkan_headers, 'Vulkan-Headers', os.path.join('include', 'vulkan', 'vulkan.h'), 'Vulkan-Headers')):
        found = find_first([opt, os.environ.get(env), os.path.join(DEPS, sub), saved.get(env)], probe_rel)
        if not found:
            raise Fail('%s not found (--%s <folder containing %s>, %s, or deps\\%s)'
                       % (what, {'SDL3_ROOT': 'sdl3', 'VULKAN_HEADERS': 'vulkan-headers'}[env], probe_rel, env, sub))
        cx.tools[env] = found
        say('  %-15s %s' % (sub, found))

    # ssbu_glslang.dll: a built one (option / GLSLANG_DLL / deps), the one already in build/, or built from glslang with MSVC
    dll = next((os.path.abspath(f) for f in (a.glslang_dll, os.environ.get('GLSLANG_DLL'), os.path.join(DEPS, 'ssbu_glslang.dll'),
                                                     saved.get('GLSLANG_DLL'))
                if f and os.path.isfile(f)), None)
    glslang = find_first([a.glslang, os.environ.get('GLSLANG_ROOT'), os.path.join(DEPS, 'glslang'), saved.get('GLSLANG_ROOT')], os.path.join('lib', 'glslang.lib'))
    if dll:
        cx.tools['GLSLANG_DLL'] = dll
        say('  ssbu_glslang.dll %s (copied into build/ by link)' % dll)
    elif glslang or name == 'msvc':
        if not glslang:
            raise Fail('glslang not found (--glslang <glslang 16.6.0 windows release>, GLSLANG_ROOT, or deps\\glslang)')
        vsw = os.path.join(os.environ.get('ProgramFiles(x86)', r'C:\Program Files (x86)'), 'Microsoft Visual Studio', 'Installer',
                           'vswhere.exe')
        vs = subprocess.run([vsw, '-latest', '-products', '*', '-requires', 'Microsoft.VisualStudio.Component.VC.Tools.x86.x64',
                             '-property', 'installationPath'], capture_output=True, text=True).stdout.strip() \
            if os.path.exists(vsw) else ''
        if not vs:
            raise Fail('glslang is there, but the MSVC Build Tools (C++ x64 tools) that build ssbu_glslang.dll from it are not; '
                       'install them, or give a built ssbu_glslang.dll (--glslang-dll <file>)')
        cx.tools['GLSLANG_ROOT'] = glslang
        say('  glslang         %s (ssbu_glslang.dll built with MSVC: %s)' % (glslang, vs))
    elif os.path.isfile(os.path.join(cx.build, 'ssbu_glslang.dll')):
        say('  ssbu_glslang.dll: the one in build/')
    else:
        raise Fail('ssbu_glslang.dll (the shader compiler shim) has no source: give a built one (--glslang-dll <file>, '
                   'GLSLANG_DLL, deps\\ssbu_glslang.dll), or glslang 16.6.0 + the MSVC Build Tools to build it (--glslang <folder>)')
    cx.state['tools'] = dict(cx.tools)


def step_verify(cx):
    if not cx.dump:
        raise Fail('--dump <folder> is required')
    for root, dirs, files in os.walk(cx.dump):
        bad = [f for f in files if f.lower().endswith(ENCRYPTED)]
        if bad:
            raise Fail('%s: encrypted packages are not accepted (%s). Give the extracted ExeFS and RomFS folders '
                       '(README, "What you extract from your own console").' % (root, bad[0]))
        dirs[:] = [d for d in dirs if d.lower() != 'romfs']          # the RomFS is checked below, no need to walk it twice
    exefs, romfs = os.path.join(cx.dump, 'exefs'), os.path.join(cx.dump, 'romfs')
    for d in (exefs, romfs):
        if not os.path.isdir(d):
            raise Fail('%s is missing: the dump folder needs exefs\\ and romfs\\' % d)
    warnings = []
    for name, (size, digest, bid) in EXEFS.items():
        p = os.path.join(exefs, name)
        if not os.path.isfile(p):
            if name in REQUIRED:
                raise Fail('exefs\\%s is missing: extract the whole ExeFS (main, sdk, subsdk0, rtld, main.npdm)' % name)
            warnings.append('exefs\\%s is missing' % name)
            continue
        with open(p, 'rb') as f:
            magic = f.read(4)
        if magic != (b'META' if name == 'main.npdm' else b'NSO0'):
            raise Fail('exefs\\%s is not a plain %s file (magic %r): give decrypted, extracted files'
                       % (name, 'NPDM' if name == 'main.npdm' else 'NSO', magic))
        if sha256(p) == digest:
            continue
        got = build_id(p)
        if bid and got == bid:          # same build, other compression: convert's segment-hash check decides
            say('  exefs\\%s: same build ID, different file (recompressed?); segment hashes are checked in convert' % name)
        elif name in ('main', 'sdk'):
            what = KNOWN_OTHER.get(got, 'a different version (another update or region), or a modified file')
            raise Fail('exefs\\%s is %s (build ID %s). This port handles only Bayonetta 2 %s '
                       '(main build ID %s).' % (name, what, got, VERSION, EXEFS['main'][2]))
        else:
            warnings.append('exefs\\%s differs from the expected version (modified or other version)' % name)
    (count, total, digest), sizes = romfs_manifest(romfs)
    if digest != ROMFS_MANIFEST[2]:
        missing = [p for p in ROMFS_FILES if p not in sizes]
        msg = 'romfs: %d files / %d bytes, expected %d / %d' % (count, total, ROMFS_MANIFEST[0], ROMFS_MANIFEST[1])
        if count < ROMFS_MANIFEST[0] or missing:
            raise Fail(msg + ': files are missing (incomplete extraction, or not %s)' % VERSION)
        warnings.append(msg + ': the file list differs (other version, or modded)')
    for rel, (size, want) in ROMFS_FILES.items():
        p = os.path.join(romfs, *rel.split('/'))
        if os.path.getsize(p) != size or sha256(p) != want:
            warnings.append('romfs\\%s differs (corrupt, other version, or modded)' % rel)
    for w in warnings:
        say('  warning: ' + w)
    if warnings and not cx.a.accept_modified:
        raise Fail('the dump differs from the supported version, Bayonetta 2 %s (above).' % VERSION + ' Fix it, or run again with --accept-modified '
                   'to continue at your own risk (mods are not blocked, but are unsupported).')
    cx.state['dump'] = cx.dump
    say('  exefs and romfs match the supported version: Bayonetta 2 %s (%d RomFS files)' % (VERSION, count))


def need_dump(cx):
    if not cx.dump or not os.path.isdir(cx.dump):
        raise Fail('the dump folder is not known: run with --dump <folder>')


def step_convert(cx):
    need_dump(cx)
    d = os.path.join(cx.dump, 'exefs')
    out = os.path.join(cx.targets, 'exefs')
    os.makedirs(out, exist_ok=True)
    conv = os.path.join(REPO, 'tools', 'extract', 'nso2elf.py')            # GPL-3.0 copy from ssbu-recomp
    for name in os.listdir(d):
        if os.path.isfile(os.path.join(d, name)):
            shutil.copy2(os.path.join(d, name), os.path.join(out, name))
    for name in ('main', 'sdk'):
        r = subprocess.run([sys.executable, conv, os.path.join(out, name), os.path.join(out, name + '.elf')],
                           capture_output=True, text=True)
        if r.returncode or 'MISMATCH' in r.stdout:
            raise Fail('%s: NSO segment hash mismatch or conversion error: corrupt or modified file\n%s'
                       % (name, r.stdout[-800:] + r.stderr[-800:]))
        say('  %s -> targets\\exefs\\%s.elf, segment hashes ok' % (name, name))
    # the game, the launcher and the run scripts read targets\romfs and targets\sysdata\fonts
    say('  targets\\romfs: %s' % link_folder(os.path.join(cx.targets, 'romfs'), os.path.join(cx.dump, 'romfs')))
    fonts = os.path.join(cx.dump, 'sysdata', 'fonts')
    if os.path.isdir(fonts):
        say('  targets\\sysdata\\fonts: %s' % link_folder(os.path.join(cx.targets, 'sysdata', 'fonts'), fonts))
    else:
        say('  no sysdata\\fonts in the dump: a font of this PC will stand in for the console\'s system fonts (BUILDING.md)')


def step_mods(cx):
    import pchtxt                                            # scripts/pchtxt.py
    folder = os.path.abspath(cx.a.mods or cx.state.get('mods_dir') or os.path.join(REPO, 'mods'))
    nso, elf = os.path.join(cx.targets, 'exefs', 'main'), os.path.join(cx.targets, 'exefs', 'main.elf')
    applied = cx.state.get('mods')               # hash of the patches in main.elf and in what the later steps made from it
    files = pchtxt.find_mods(folder)
    if not files and not applied:
        say('  no mods (no .pchtxt / .ips files in %s): main.elf as converted' % folder)
        return
    if not os.path.isfile(nso) or not os.path.isfile(elf):
        raise Fail('targets\\exefs\\main / main.elf are missing: run convert first')
    settings = pchtxt.read_settings(pchtxt.DEFAULT_INI)     # config\mods.ini: the launcher's Mods window switches
    off = [f for f in files if not pchtxt.file_on(os.path.relpath(f, folder), settings)]
    if off:
        say('  switched off in config\\mods.ini: %s' % ', '.join(sorted({pchtxt.rel_key(os.path.relpath(f, folder)) for f in off})))
    try:
        mods = pchtxt.load_mods(folder, nso, settings)
    except pchtxt.ModError as e:
        raise Fail(str(e))
    want = pchtxt.fingerprint(mods)
    if want == applied and (not want or sha256(elf) == cx.state.get('mods_elf')):
        say('  %d mod file(s), %s' % (len(files), 'unchanged since the last build' if want else 'no enabled patches'))
        return
    # main.elf again from the NSO, exactly as convert makes it, then the patches on top
    tmp = elf + '.tmp'
    r = subprocess.run([sys.executable, os.path.join(REPO, 'tools', 'extract', 'nso2elf.py'), nso, tmp],
                       capture_output=True, text=True)
    if r.returncode or 'MISMATCH' in r.stdout:
        raise Fail('main: NSO conversion error\n%s' % (r.stdout[-800:] + r.stderr[-800:]))
    with open(tmp, 'rb') as f:
        img = bytearray(f.read())
    lines = []
    try:
        n = pchtxt.apply(img, mods, log=lines.append)
    except pchtxt.ModError as e:
        os.remove(tmp)
        raise Fail(str(e))
    with open(tmp, 'wb') as f:
        f.write(img)
    os.replace(tmp, elf)
    os.makedirs(cx.logs, exist_ok=True)
    with open(os.path.join(cx.logs, 'mods.log'), 'a', encoding='utf-8') as f:
        f.write('\n%s  %s  %s\n%s\n' % (time.strftime('%Y-%m-%d %H:%M:%S'), folder, want, '\n'.join(lines)))
    for line in lines:
        say(line)
    say('  main.elf: ' + ('%d bytes patched by %d enabled patch(es) from %d file(s)' % (n, len(mods), len(files)) if want
                          else 'unpatched again (no enabled patches left)'))
    if want != applied:
        redo = [s for s in STEPS[STEPS.index('mods') + 1:] if s in cx.state.get('done', {}) and s != 'icon']
        for s in redo:
            cx.state['done'].pop(s)
        if redo:
            say('  the mods changed: %s run again' % ', '.join(redo))
    cx.state['mods'] = want
    cx.state['mods_elf'] = sha256(elf) if want else None
    if cx.a.mods:
        cx.state['mods_dir'] = folder


def step_tables(cx):
    cx.run('tables', [sys.executable, os.path.join(REPO, 'runtime', 'gen', 'make_gen.py')])


def step_lift(cx):
    cx.run('lift', [sys.executable, 'recomp_eh.py', os.path.join(cx.build, 'gen_eh'), str(cx.jobs)],
           cwd=os.path.join(REPO, 'lifter'))
    # the frame-rate patch (graphics.ini frame_rate above 60; off at 60): the clock, the present and the timers of
    # lifter/fps_ticks.txt, patched into the lifted C in place (scripts/fps_patch.py)
    cx.run('lift', [sys.executable, os.path.join(REPO, 'scripts', 'fps_patch.py'), '--gen=' + os.path.join(cx.build, 'gen_eh'), '--inplace'])
    # edición mejorada: the in-game menu's cheats as run-time switches (scripts/cheats_patch.py; all off = the game as lifted)
    cx.run('lift', [sys.executable, os.path.join(REPO, 'scripts', 'cheats_patch.py'), '--gen=' + os.path.join(cx.build, 'gen_eh')])


def step_compile(cx):
    tail = cx.run('compile', [sys.executable, os.path.join(REPO, 'scripts', 'compile_main.py'), str(cx.jobs)])
    say('  ' + next((l for l in tail.splitlines() if l.startswith('compiled ')), 'compiled'))


def step_icon(cx):
    need_dump(cx)
    src = os.path.join(cx.dump, 'control', 'icon_AmericanEnglish.dat')
    if not os.path.isfile(src):
        say('  no control\\icon_AmericanEnglish.dat in the dump: the programs get the default icon')
        return
    cx.run('icon', [sys.executable, os.path.join(REPO, 'scripts', 'make_icon_local.py'), src])
    say('  build\\bayonetta2.ico')


def step_link(cx):
    cx.run('link', [sys.executable, os.path.join(REPO, 'scripts', 'build_runtime.py')])
    cx.run('link', [sys.executable, os.path.join(REPO, 'scripts', 'build_launcher.py')])
    for f in ('game.dll', 'game_native.exe', 'SDL3.dll', 'ssbu_glslang.dll', 'launcher.exe'):
        if not os.path.isfile(os.path.join(cx.build, f)):
            raise Fail('build\\%s is missing after the link step (see %s)' % (f, os.path.join(cx.logs, 'link.log')))
    say('  build\\game_native.exe %.1f MB, game.dll %.0f MB, launcher.exe'
        % (os.path.getsize(os.path.join(cx.build, 'game_native.exe')) / 2 ** 20,
           os.path.getsize(os.path.join(cx.build, 'game.dll')) / 2 ** 20))


def step_check(cx):
    try:
        import unicorn  # noqa: F401
    except ImportError:
        say('  skipped (pip install unicorn for the lifter self-test)')
        return
    tail = cx.run('check', [sys.executable, 'test_ext.py'], cwd=os.path.join(REPO, 'lifter'))
    say('  ' + (tail.strip().splitlines() or ['ok'])[-1])


# ---------------------------------------------------------------------------------------------- main
def main():
    ap = argparse.ArgumentParser(description='Build the native port from your own extracted dump.')
    ap.add_argument('--dump', help='folder with exefs\\ and romfs\\ (remembered after the first run)')
    ap.add_argument('--jobs', type=int)
    ap.add_argument('--only', choices=STEPS)
    ap.add_argument('--from', dest='from_step', choices=STEPS)
    ap.add_argument('--status', action='store_true')
    ap.add_argument('--accept-modified', action='store_true')
    ap.add_argument('--mods', help='folder of emulator-style code mods (.pchtxt / .ips); default <repo>\\mods (remembered)')
    ap.add_argument('--toolchain', choices=('mingw', 'msvc'))
    ap.add_argument('--llvm-mingw', help='llvm-mingw folder (bin\\x86_64-w64-mingw32-clang.exe)')
    ap.add_argument('--llvm', help='LLVM 9.0.0 folder (bin\\clang.exe), --toolchain msvc only')
    ap.add_argument('--sdl3', help='SDL3 development package (VC) folder: include\\, lib\\x64\\')
    ap.add_argument('--vulkan-headers', help='Khronos Vulkan-Headers folder (include\\vulkan\\vulkan.h)')
    ap.add_argument('--glslang', help='glslang windows release folder (include\\, lib\\glslang.lib); needs MSVC')
    ap.add_argument('--glslang-dll', help='a built ssbu_glslang.dll instead of building it')
    a = ap.parse_args()
    cx = Ctx(a)
    done = cx.state.setdefault('done', {})
    if a.status:
        for s in STEPS:
            say('%-8s %s' % (s, 'done (%s)' % fmt(done[s]) if s in done else '-'))
        return 0
    if a.dump and cx.state.get('dump') and os.path.normcase(cx.dump) != os.path.normcase(cx.state['dump']):
        done.clear()                                         # another dump: everything is made again
    todo = sorted(ALWAYS | {a.only}, key=STEPS.index) if a.only else STEPS
    if a.from_step:
        for s in STEPS[STEPS.index(a.from_step):]:
            done.pop(s, None)
    t_all = time.time()
    for i, s in enumerate(STEPS):
        if s not in todo or (s in done and s not in ALWAYS and not a.only):
            continue
        say('[%d/%d] %s ...' % (i + 1, len(STEPS), s))
        t0 = time.time()
        try:
            globals()['step_' + s](cx)
        except Fail as e:
            say('[%d/%d] %s: FAILED\n%s' % (i + 1, len(STEPS), s, e))
            cx.save()
            return 1
        done[s] = round(time.time() - t0, 1)
        cx.save()
        say('[%d/%d] %s: ok  %s' % (i + 1, len(STEPS), s, fmt(done[s])))
    say('setup: %s this run' % fmt(time.time() - t_all))
    if all(s in done for s in STEPS):
        say('Start the game: build\\launcher.exe (Play), or scripts\\play.ps1')
    return 0


if __name__ == '__main__':
    sys.exit(main())
