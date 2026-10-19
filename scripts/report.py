#!/usr/bin/env python3
"""[Recomps#1] Makes b2recomp-report.zip in the B2recomp folder: what someone helping a player needs to see - this PC
(Windows or Linux version, CPU, memory, graphics card and driver), the logs (the build's, the game's, its freeze /
flash / stall reports) and on Windows the crash / hang records Windows kept of our programs (windows-errors.txt,
finding 128). Nothing of the game: no dump, no targets/, no translated code, no game files.

    python scripts/report.py        (the launcher's Report... button runs it; on Linux scripts/report.sh does the same)

Standard library only (the builder package's Python, run with -I). A log above 50 MB keeps its last 50 MB.
"""
import calendar
import glob
import html
import os
import platform
import re
import subprocess
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
OUR_EXES = ('game_native.exe', 'launcher.exe', 'b2recomp.exe')
ERROR_DAYS = 30
CODES = {'c0000005': 'access violation', 'c00000fd': 'stack overflow', 'c0000409': 'fast-fail: ended at once by the program '
         'or a driver in it', 'c0000374': 'heap corruption', 'c000001d': 'illegal instruction', 'c000041d': 'crash inside a '
         'callback from Windows', '80000003': 'breakpoint (an assertion)', 'e06d7363': 'C++ exception nobody caught'}


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


def windows_errors():
    """Finding 128: the Application log's crash ("Application Error", 1000) and hang ("Application Hang", 1002) records of
    our programs over the last ERROR_DAYS days, newest first. They name the module, exception code and offset of a crash
    the game's own handler never saw (a fast-fail, a stack overflow, heap corruption, a crash inside the graphics driver):
    such a game log just stops. Read from the records' named fields, so the Windows language does not matter."""
    q = ("*[System[Provider[@Name='Application Error' or @Name='Application Hang'] and (EventID=1000 or EventID=1002) and "
         "TimeCreated[timediff(@SystemTime) <= %d]]]" % (ERROR_DAYS * 86400000))
    head = ['Windows crash (Application Error) and hang (Application Hang) records of %s over the last %d days, newest first'
            % (', '.join(OUR_EXES), ERROR_DAYS)]
    try:
        r = subprocess.run(['wevtutil', 'qe', 'Application', '/q:' + q, '/c:500', '/rd:true', '/f:xml'],
                           capture_output=True, timeout=60)
    except (OSError, subprocess.SubprocessError) as e:
        return head + ['(the Windows log could not be read: %s)' % e]
    if r.returncode:
        return head + ['(the Windows log could not be read: wevtutil exit %d: %s)'
                       % (r.returncode, r.stderr.decode('mbcs', errors='replace').strip())]
    try:
        text = r.stdout.decode('utf-8')
    except UnicodeDecodeError:
        text = r.stdout.decode('mbcs', errors='replace')
    out = []
    for ev in re.findall(r'<Event[ >].*?</Event>', text, re.S):
        d = {k: html.unescape(v) for k, v in re.findall(r"<Data Name='(\w+)'>([^<]*)</Data>", ev)}
        if d.get('AppName', '').lower() not in OUR_EXES:
            continue
        t = re.search(r"SystemTime='(\d{4}-\d\d-\d\dT\d\d:\d\d:\d\d)", ev)
        when = time.strftime('%Y-%m-%d %H:%M:%S', time.localtime(calendar.timegm(time.strptime(t.group(1), '%Y-%m-%dT%H:%M:%S')))) if t else '?'
        if 'Application Hang' in ev:
            out.append('%s  HANG   %s: Windows found its window not responding (hang type %s)  %s'
                       % (when, d.get('AppName'), d.get('HangType', '?'), d.get('ExeFileName', '')))
        else:
            code = d.get('ExceptionCode', '?').lower()
            out.append('%s  CRASH  %s: exception 0x%s%s in %s %s + 0x%s\n      module %s\n      program %s'
                       % (when, d.get('AppName'), code, ' (%s)' % CODES[code] if code in CODES else '', d.get('ModuleName', '?'),
                          d.get('ModuleVersion', ''), d.get('FaultingOffset', '?').lstrip('0') or '0', d.get('ModulePath', '?'),
                          d.get('AppPath', '?')))
        if len(out) >= 20:
            break
    return head + (out or ['(none)'])


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
    errors = None
    if os.name == 'nt':
        try:
            errors = windows_errors()
        except Exception as e:                            # as the PC details: a help, never a reason to fail
            errors = ['(the Windows crash records could not be read: %s)' % e]
    with zipfile.ZipFile(tmp, 'w', zipfile.ZIP_DEFLATED) as z:
        z.writestr('pc-info.txt', '\n'.join(lines) + '\n')
        if errors:
            z.writestr('windows-errors.txt', '\n'.join(errors) + '\n')
        for p in files:
            rel = os.path.relpath(p, ROOT).replace(os.sep, '/')
            if os.path.getsize(p) > MAX_LOG:
                with open(p, 'rb') as f:
                    f.seek(-MAX_LOG, 2)
                    z.writestr(rel, b'[... the start of this log is left out: it was larger than 50 MB]\n' + f.read())
            else:
                z.write(p, rel)
    os.replace(tmp, OUT)
    print('Made %s (%d files)' % (OUT, len(files) + 1 + (errors is not None)))
    print("Send that file to whoever helps you. It holds logs and this PC's details, nothing of the game.")
    return 0


if __name__ == '__main__':
    sys.exit(main())
