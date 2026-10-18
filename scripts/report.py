#!/usr/bin/env python3
"""[Recomps#1] Makes b2recomp-report.zip in the B2recomp folder: what someone helping a player needs to see - this PC
(Windows or Linux version, CPU, memory, graphics card and driver) and the logs (the build's, the game's, its freeze /
flash / stall reports). Nothing of the game: no dump, no targets/, no translated code, no game files.

    python scripts/report.py        (the launcher's Report... button runs it; on Linux scripts/report.sh does the same)

Standard library only (the builder package's Python, run with -I). A log above 50 MB keeps its last 50 MB.
"""
import glob
import os
import platform
import sys
import time
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, 'b2recomp-report.zip')
LOGS = ['build/logs', 'build/setup_state.json', 'build/play/stdout.txt', 'build/boot_trace.txt',
        'build/*_freeze.txt', 'build/*_flash.txt', 'build/*_stalls.txt',
        'build-linux/play/stdout.txt', 'build-linux/boot_trace.txt', 'build-linux/*_freeze.txt', 'build-linux/*_flash.txt',
        'build-linux/*_stalls.txt']
MAX_LOG = 50 << 20


def windows_info():
    import ctypes
    import winreg
    out = []
    v = sys.getwindowsversion()
    name = 'Windows 11' if v.major == 10 and v.build >= 22000 else 'Windows %d.%d' % (v.major, v.minor)   # 11 still says 10.0
    out.append('%s build %d%s' % (name, v.build, ' (%s)' % platform.win32_edition() if hasattr(platform, 'win32_edition') else ''))

    def reg(path, name):
        try:
            with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, path) as k:
                return str(winreg.QueryValueEx(k, name)[0]).strip()
        except OSError:
            return None
    out.append('CPU: %s' % (reg(r'HARDWARE\DESCRIPTION\System\CentralProcessor\0', 'ProcessorNameString') or '?'))

    class MS(ctypes.Structure):
        _fields_ = [('len', ctypes.c_ulong), ('load', ctypes.c_ulong), ('total', ctypes.c_ulonglong),
                    ('avail', ctypes.c_ulonglong), ('tp', ctypes.c_ulonglong), ('ap', ctypes.c_ulonglong),
                    ('tv', ctypes.c_ulonglong), ('av', ctypes.c_ulonglong), ('ae', ctypes.c_ulonglong)]
    ms = MS()
    ms.len = ctypes.sizeof(MS)
    if ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(ms)):
        out.append('memory: %.1f GB' % (ms.total / 2 ** 30))
    cls = r'SYSTEM\CurrentControlSet\Control\Class\{4d36e968-e325-11ce-bfc1-08002be10318}'   # display adapters
    try:
        with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, cls) as k:
            i = 0
            while True:
                try:
                    sub = winreg.EnumKey(k, i)
                except OSError:
                    break
                i += 1
                if sub.isdigit():
                    desc = reg(cls + '\\' + sub, 'DriverDesc')
                    if desc:
                        out.append('GPU: %s, driver %s (%s)' % (desc, reg(cls + '\\' + sub, 'DriverVersion') or '?',
                                                               reg(cls + '\\' + sub, 'DriverDate') or '?'))
    except OSError:
        out.append('GPU: ? (the game log names the one it used: "nvn_vk: using device")')
    return out


def linux_info():
    out = []
    try:
        with open('/etc/os-release', encoding='utf-8') as f:
            out += [l.strip() for l in f if l.startswith('PRETTY_NAME=')]
    except OSError:
        pass
    out.append('kernel %s, glibc %s' % (platform.release(), '-'.join(platform.libc_ver())))
    out.append('session: %s %s' % (os.environ.get('XDG_SESSION_TYPE', ''), os.environ.get('XDG_CURRENT_DESKTOP', '')))
    try:
        with open('/proc/cpuinfo', encoding='utf-8') as f:
            out += ['CPU: ' + l.split(':', 1)[1].strip() for l in f if l.startswith('model name')][:1]
    except OSError:
        pass
    return out


def main():
    lines = []
    try:
        with open(os.path.join(ROOT, 'VERSION'), encoding='utf-8') as f:
            lines.append('B2recomp ' + f.read().strip())
    except OSError:
        lines.append('B2recomp (source tree)')
    lines.append('report made %s' % time.strftime('%Y-%m-%d %H:%M'))
    try:
        lines += windows_info() if os.name == 'nt' else linux_info()
    except Exception as e:                                # the details are a help, never a reason to fail
        lines.append('(PC details incomplete: %s)' % e)
    lines.append('threads: %d' % (os.cpu_count() or 0))
    files = []
    for pat in LOGS:
        for p in sorted(glob.glob(os.path.join(ROOT, *pat.split('/')))):
            if os.path.isdir(p):
                files += sorted(os.path.join(d, n) for d, _, ns in os.walk(p) for n in ns)
            elif os.path.isfile(p):
                files.append(p)
    tmp = OUT + '.tmp'
    with zipfile.ZipFile(tmp, 'w', zipfile.ZIP_DEFLATED) as z:
        z.writestr('pc-info.txt', '\n'.join(lines) + '\n')
        for p in files:
            rel = os.path.relpath(p, ROOT).replace(os.sep, '/')
            if os.path.getsize(p) > MAX_LOG:
                with open(p, 'rb') as f:
                    f.seek(-MAX_LOG, 2)
                    z.writestr(rel, b'[... the start of this log is left out: it was larger than 50 MB]\n' + f.read())
            else:
                z.write(p, rel)
    os.replace(tmp, OUT)
    print('Made %s (%d files)' % (OUT, len(files) + 1))
    print("Send that file to whoever helps you. It holds logs and this PC's details, nothing of the game.")
    return 0


if __name__ == '__main__':
    sys.exit(main())
