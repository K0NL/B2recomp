"""Build build/launcher.exe (launcher/launcher.c: settings window + Play) with the build toolchain.

    python scripts/build_launcher.py [<capture.bmp>] [--out=<exe>] [--no-icon] [--toolchain=msvc|mingw]

TOOLCHAIN (scripts/toolchain.py): mingw (default) = llvm-mingw; msvc = clang 9 + MSVC link. The launcher belongs in build/
(it starts build\\game_native.exe and reads config\\ beside it) or at the top of the repository: it finds the repository
as the folder that holds scripts\\setup.py, its own or the one above. --out writes it elsewhere (scripts/make_package.py:
B2recomp.exe at the top of the builder package, with --no-icon so that no icon made from a dump gets into it).

Resources (build/launcher.rc, written here, compiled with clang's llvm-rc because this machine has no rc.exe, so the
linker's /manifest:embed cannot be used): launcher/launcher.manifest (common controls 6 = themed controls; DPI aware) and
icon 1 = build/bayonetta2.ico when it exists, the game's own icon (scripts/make_icon_local.py, from the dump's control data;
the launcher also loads that file at start-up, so a launcher built before it was made still shows it).

Header art: build/launcher_art.bmp, a renderer capture of the title screen (SSBU_SHOT; the launcher crops it round the
logo, 45 % down the picture), read at start-up. Copied from the capture given as argument (e.g. build/run24/shots/
frame_1560.bmp, the title with the logo complete); there is no default, and the launcher shows the title as text without it.
"""
import os
import shutil
import subprocess
import sys

import toolchain

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
SRCD, BUILD = os.path.join(ROOT, 'launcher'), os.path.join(ROOT, 'build')
EXE, RC, RES = os.path.join(BUILD, 'launcher.exe'), os.path.join(BUILD, 'launcher.rc'), os.path.join(BUILD, 'launcher.res')
OUT = next((os.path.abspath(a[6:]) for a in sys.argv[1:] if a.startswith('--out=')), None)
if OUT:                                                  # resource files of its own: build/launcher.res stays the real one's
    EXE, RC, RES = OUT, os.path.join(BUILD, 'launcher_out.rc'), os.path.join(BUILD, 'launcher_out.res')
ARGS = [a for a in sys.argv[1:] if not a.startswith('--')]
ICO = os.path.join(BUILD, 'bayonetta2.ico')
NO_ICON = '--no-icon' in sys.argv
ART_OUT = os.path.join(BUILD, 'launcher_art.bmp')


def run(cmd, **kw):
    r = subprocess.run(cmd, capture_output=True, text=True, errors='replace', **kw)
    sys.stdout.write(r.stdout + r.stderr)
    return r.returncode


os.makedirs(BUILD, exist_ok=True)
if ARGS and os.path.exists(ARGS[0]):
    shutil.copyfile(ARGS[0], ART_OUT)
    print('launcher_art.bmp: from %s' % ARGS[0])
if not os.path.exists(ICO) and not NO_ICON:                              # made by scripts/make_icon_local.py (setup.py's icon step)
    print('bayonetta2.ico: not in build/, the launcher gets the default icon')
with open(RC, 'w') as f:
    f.write('1 24 "%s"\n' % os.path.join(SRCD, 'launcher.manifest').replace('\\', '/'))
    if os.path.exists(ICO) and not NO_ICON:
        f.write('1 ICON "%s"\n' % ICO.replace('\\', '/'))
code = run([toolchain.LLVM_RC, '/fo', RES, RC])
if not code:
    # entry point wWinMain: MSVC's linker finds it with /subsystem:windows; mingw needs -municode -mwindows, and lld in
    # mingw mode takes the .res as an ordinary input
    target = ['-municode', '-mwindows', RES] if toolchain.MINGW else ['-Wl,/subsystem:windows', '-Xlinker', RES]   # a path whole (commas, F149)
    code = run([toolchain.CLANG, '-O2', '-Wall', '-Wno-missing-braces', '-D_CRT_SECURE_NO_WARNINGS',
                os.path.join(SRCD, 'launcher.c'), '-o', EXE, '-luser32', '-lgdi32', '-lcomctl32', '-lshell32', '-luxtheme',
                '-ldwmapi', '-lmsimg32', '-lole32', '-luuid'] + target)
print('%s: exit %d (TOOLCHAIN=%s)' % (EXE, code, toolchain.NAME))
if not code:
    toolchain.copy_runtime_licence(os.path.dirname(EXE))
sys.exit(code)
