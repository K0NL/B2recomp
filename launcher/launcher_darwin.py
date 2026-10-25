#!/usr/bin/env python3
"""[Recomps#1] launcher_darwin.py: the macOS version of launcher/launcher.c (notes/macos-port-plan.md step 2), in Python + Tk.

The same window as on Windows: the build screen while the game is not built yet (or with --build), then the settings
(config/graphics.ini, the [general] section of config/controllers.ini; saving rewrites only the values of the keys changed
here, in place, as launcher.c does), Play, the controller tools, the keyboard and mouse window and the code mods window.
Colours, layout and wording follow launcher.c; keep the two in step. The header shows the title as text only (no art).

Started by ./B2recomp at the top of the macOS builder package with its own Python (deps/python, Tk included), so the
player installs nothing; from a source tree: python3 launcher/launcher_darwin.py (needs Tk: python-tk / tk packages).
  Build    deps/python/bin/python3 -u scripts/setup.py --dump <folder>, in a process group of its own (Close stops it and
           every compiler it started); its "[i/10] step" lines tick off the steps, compile progress from build/logs/compile.log
  Play     build-darwin/game_native targets/exefs/main.elf from build-darwin/, window and sound on, saves in
           build-darwin/play/userdata, log build-darwin/play/stdout.txt (scripts/play.sh does the same from a terminal)
Made easier for players (2026-10-18; the Windows launcher should get the same, notes/roadmap.md "Later ideas"):
  Report...  scripts/report.sh: b2recomp-report.tar.gz with the logs and this PC's details, nothing of the game
  when the game stops with an error (exit code not 0, or a signal), the reason from its log and the offer of a report
  before Play, a check for a Vulkan loader and driver (what to install when one is missing)
  the app-menu entry (~/.local/share/applications/b2recomp.desktop): made at the first start, so from then on players
  open B2recomp from their desktop's app launcher (KDE, GNOME, wofi / rofi ...) instead of a script; after the build it
  gets the game's icon (made from the player's own dump); kept pointing at this folder; not made again once the player
  removed it (./B2recomp --add-to-menu / --remove-from-menu)
Test with another screen scale: B2RECOMP_LAUNCHER_DPI=<dpi> (96 = 100 %).
"""
import json
import os
import re
import shutil
import signal
import subprocess
import sys
import threading
import queue
import time
import base64
import io

try:
    import tkinter as tk
    from tkinter import ttk, filedialog, messagebox
    import tkinter.font as tkfont
except ImportError:                                       # a system Python without Tk
    sys.exit('B2recomp: this Python has no Tk (tkinter). The Linux builder package brings its own Python (deps/python); '
             'from a source tree install your distribution\'s python Tk package (python-tk, python3-tk, tk).')

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.path.join(ROOT, 'build')                       # setup.py's logs, state, lifted C
OUT = os.path.join(ROOT, 'build-darwin')                 # the macOS game
GAME, GAME_SO = os.path.join(OUT, 'game_native'), os.path.join(OUT, 'game.dylib')
ELF = os.path.join(ROOT, 'targets', 'exefs', 'main.elf')
CFG = os.path.join(ROOT, 'config')
INI = [os.path.join(CFG, 'graphics.ini'), os.path.join(CFG, 'controllers.ini')]
MODS_DIR, MODS_INI = os.path.join(ROOT, 'mods'), os.path.join(CFG, 'mods.ini')
F_GFX, F_PAD = 0, 1

WIN_W, WIN_H, HEADER = 780, 756, 244
C_BG, C_PANEL, C_EDGE, C_FIELD = '#070a14', '#0f1424', '#26304e', '#191f34'
C_TEXT, C_DIM, C_NOTE, C_VALUE, C_GOLD = '#dee4f2', '#626c86', '#808eac', '#eece8c', '#deb25c'
C_SUB, C_ERR, C_ACCENT = '#96b2e2', '#f07064', '#2f7fd6'
PANELS = [(14, 232, 454, 692), (466, 232, 766, 692)]     # graphics, controller (96-dpi pixels, as launcher.c)
BPANEL = (14, 232, 766, 660)
CLEAR_ENV = ('SSBU_SEQ', 'SSBU_SCRIPT', 'SSBU_PRESS', 'SSBU_SHOT', 'SSBU_SHOT_EVERY', 'SSBU_SHOT_DIR', 'SSBU_PROF15', 'SSBU_HITCH',
             'SSBU_TRACE', 'SSBU_CONFIG', 'SSBU_GRAPHICS_CONFIG', 'SSBU_PAD_LAYOUT', 'VK_DRIVER_FILES', 'VK_ICD_FILENAMES')


# ================================================================ ini files, edited in place (launcher.c's Ini)
class Ini:
    """Lines split at '\\n', each keeping its own '\\r'; values replaced in place, new keys at the end of their section."""

    def __init__(self, path):
        self.path, self.lines, self.crlf, self.exists = path, [''], True, False
        try:
            with open(path, 'rb') as f:
                data = f.read().decode('utf-8', 'surrogateescape')
        except OSError:
            return
        self.exists = True
        self.lines = data.split('\n')
        self.crlf = (bool(self.lines[0]) and self.lines[0].endswith('\r')) if len(self.lines) > 1 else True

    @staticmethod
    def section(l):
        s = l.lstrip(' \t')
        if not s.startswith('['):
            return None
        e = len(s)
        for i, c in enumerate(s[1:], 1):
            if c in ']\r':
                e = i
                break
        return s[1:e]

    @staticmethod
    def kv(l, inl):
        """(key, value start, value end) of a key = value line; inl: ' ;' or ' #' starts an inline comment."""
        p = len(l) - len(l.lstrip(' \t'))
        if p >= len(l) or l[p] in ';#[\r':
            return None
        eq = l.find('=', p)
        if eq < 0:
            return None
        key = l[p:eq].rstrip(' \t')
        v = eq + 1
        while v < len(l) and l[v] in ' \t':
            v += 1
        e = len(l)
        if inl:
            for q in range(v + 1, len(l)):
                if l[q] in ';#' and l[q - 1] in ' \t':
                    e = q
                    break
        while e > v and l[e - 1] in ' \t\r':
            e -= 1
        return key, v, e

    def get(self, sect, key, inl):
        cur, found = '', None
        for l in self.lines:
            s = self.section(l)
            if s is not None:
                cur = s
                continue
            if sect is None or cur.lower() == sect.lower():
                r = self.kv(l, inl)
                if r and r[0].lower() == key.lower():
                    found = l[r[1]:r[2]]                  # the last one counts
        return found

    def ensure_eol(self):
        if not self.lines[-1]:
            return
        if self.crlf and not self.lines[-1].endswith('\r'):
            self.lines[-1] += '\r'
        self.lines.append('')

    def set(self, sect, key, inl, val):
        cur, done, last, header = '', False, -1, -1
        for i, l in enumerate(self.lines):
            s = self.section(l)
            if s is not None:
                cur = s
                if sect is None or cur.lower() == sect.lower():
                    header = last = i
                continue
            if sect is not None and cur.lower() != sect.lower():
                continue
            if l.strip(' \t\r'):
                last = i
            r = self.kv(l, inl)
            if r and r[0].lower() == key.lower():        # every occurrence: no stale duplicate wins
                self.lines[i] = l[:r[1]] + val + l[r[2]:]
                done = True
        if done:
            return
        eol = '\r' if self.crlf else ''
        if sect is not None and header < 0:              # the section is missing: append it
            self.ensure_eol()
            at = len(self.lines) - 1
            if at > 0 and self.lines[at - 1].strip(' \t\r'):
                self.lines.insert(at, eol)
                at += 1
            self.lines.insert(at, '[%s]%s' % (sect, eol))
            self.lines.insert(at + 1, '%s = %s%s' % (key, val, eol))
            return
        at = last + 1
        if at >= len(self.lines):
            self.ensure_eol()
            at = len(self.lines) - 1
        self.lines.insert(at, '%s = %s%s' % (key, val, eol))

    def save(self, path=None):
        path = path or self.path
        os.makedirs(os.path.dirname(path), exist_ok=True)
        tmp = path + '.tmp'
        try:
            with open(tmp, 'wb') as f:
                f.write('\n'.join(self.lines).encode('utf-8', 'surrogateescape'))
            for _ in range(20):                          # a running game may have the file open for a moment
                try:
                    os.replace(tmp, path)
                    return True
                except OSError:
                    time.sleep(0.05)
        except OSError:
            pass
        try:
            os.remove(tmp)
        except OSError:
            pass
        return False


# ================================================================ the settings (launcher.c's g_s, same keys and rules)
K_COMBO, K_NUM, K_SLIDER, K_CHECK = range(4)
O_RENDERER = [('vulkan', 'Vulkan (recommended)'), ('opengl', 'OpenGL (old renderer, NVIDIA only)')]
O_PROFILE = [('custom', 'Custom (the settings below)'), ('low', 'Low (weak or integrated GPU)')]
O_WINDOW = [('windowed', 'Windowed'), ('borderless', 'Borderless window'), ('fullscreen', 'Fullscreen (borderless, on top)')]
O_VSYNC = [('auto', 'Auto (by refresh rate)'), ('display', 'Display refresh (60/120/240 Hz)'), ('clock', 'Program clock (60 Hz)'),
           ('off', 'Off (tearing, lowest latency)'), ('on', 'Swapchain FIFO (not recommended)'), ('mailbox', 'Mailbox')]
O_LATENCY = [('normal', 'Normal'), ('low', 'Low'), ('lowest', 'Lowest (frames may judder)')]
O_ANISO = [('game', 'Game default'), ('2', '2x'), ('4', '4x'), ('8', '8x'), ('16', '16x')]
O_ASPECT = [('keep', 'Keep 16:9 (black bars)'), ('stretch', 'Stretch to the window')]
O_LAYOUT = [('auto', 'Auto'), ('positional', 'By position (Switch)'), ('labels', 'By printed letter')]
O_PROMPTS = [('auto', 'Auto (by controller)'), ('switch', 'Switch'), ('playstation', 'PlayStation'), ('xbox', 'Xbox')]
O_INPUT = [('both', 'Controller and keyboard'), ('keyboard', 'Keyboard and mouse only'), ('controller', 'Controller only')]


class Setting:
    def __init__(self, file, key, kind, opt=None, fallback=0, lo=0.0, hi=0.0, step=0.0, nlo=0, nhi=0, d='', label=None, tip=None):
        self.file, self.key, self.kind, self.opt, self.fallback = file, key, kind, opt or [], fallback
        self.lo, self.hi, self.step, self.nlo, self.nhi, self.d, self.label, self.tip = lo, hi, step, nlo, nhi, d, label, tip
        self.w = self.val = self.lab = self.var = None
        self.loaded, self.loadpos, self.exact, self.moved, self.filev = '', 0, False, False, 0.0


S_ = dict(
    renderer=Setting(F_GFX, 'renderer', K_COMBO, O_RENDERER, 0, d='vulkan', label='Renderer',
                     tip='Vulkan is the default. OpenGL is the first renderer, kept as a fallback (NVIDIA only, Windows only).'),
    profile=Setting(F_GFX, 'profile', K_COMBO, O_PROFILE, 0, d='custom', label='Preset',
                    tip='Low: resolution scale 0.5, game anisotropic filtering and the program clock for vsync; those three settings are then not used.'),
    window=Setting(F_GFX, 'window', K_COMBO, O_WINDOW, 0, d='windowed', label='Window mode',
                   tip='Borderless and fullscreen cover the monitor the window is on (the desktop keeps its resolution).'),
    width=Setting(F_GFX, 'width', K_NUM, nlo=320, nhi=16384, d='1280', label='Window size'),
    height=Setting(F_GFX, 'height', K_NUM, nlo=180, nhi=16384, d='720'),
    vsync=Setting(F_GFX, 'vsync', K_COMBO, O_VSYNC, 2, d='auto', label='Vertical sync',
                  tip="Auto: 'display' on a 60, 120 or 240 Hz monitor, else the program's own 60 Hz clock. Off: tearing allowed, lowest latency."),
    latency=Setting(F_GFX, 'latency', K_COMBO, O_LATENCY, 0, d='normal', label='Input latency',
                    tip='Low: the controller is read 2-5 ms later, closer to the screen. Lowest: about 4 ms less again, but frames no longer last the same time.'),
    scale=Setting(F_GFX, 'resolution_scale', K_SLIDER, lo=0.5, hi=3.0, step=0.05, d='1.0', label='Resolution scale',
                  tip="Internal resolution. 1.0 = the game's 1280x720; 1.5 = 1920x1080; 2.0 = 2560x1440 (sharper edges)."),
    aniso=Setting(F_GFX, 'anisotropic_filtering', K_COMBO, O_ANISO, 0, d='game', label='Anisotropic filtering',
                  tip='Sharper textures at flat angles; costs little on any GPU.'),
    aspect=Setting(F_GFX, 'aspect', K_COMBO, O_ASPECT, 0, d='keep', label='Aspect ratio',
                   tip="The game's picture is 16:9. Keep: in a wider or taller window (an ultrawide monitor in borderless or "
                       'fullscreen mode) it keeps its shape, with black bars. Stretch: it fills the window.'),
    overlay=Setting(F_GFX, 'frame_time_overlay', K_CHECK, d='off', label='Show frame time in the window title'),
    fps=Setting(F_GFX, 'frame_rate', K_SLIDER, lo=60, hi=120, step=1, d='60', label='Frame rate (beta)',
                tip='60: the game as made. Above 60 (experimental): the game runs up to this many frames per second at its normal speed. '
                    'Its main timers are converted; some effects, menus and button-mash prompts may still run faster, and cinematics run in real time.'),
    layout=Setting(F_PAD, 'layout', K_COMBO, O_LAYOUT, 1, d='auto', label='Face buttons',
                   tip="By position: the right button is A, bottom B, top X, left Y (as on a Switch). By printed letter: an Xbox pad's A is A; "
                       'on a PlayStation pad Cross is A, Circle B, Square X, Triangle Y. The PlayStation and Xbox button pictures follow this choice. '
                       'Auto: by letter on GameCube pads, by position on all others.'),
    prompts=Setting(F_PAD, 'button_prompts', K_COMBO, O_PROMPTS, 0, d='auto', label='Button prompts',
                    tip='Which button pictures the game shows. PlayStation and Xbox need the free mod "Controller Prompts '
                        'Replacement (DS4/XBOX)" by Timo654 (gamebanana.com/mods/360121, Switch version): put its DS4Prompts / '
                        "XBOXPrompts folder into the mods folder. Auto: by player 1's controller when the game starts."),
    deadzone=Setting(F_PAD, 'deadzone', K_SLIDER, lo=0.0, hi=0.5, step=0.01, d='0.10', label='Stick dead zone',
                     tip='Part of the stick travel around the centre that counts as no movement.'),
    rumble=Setting(F_PAD, 'rumble', K_CHECK, d='on', label='Vibration'),
    strength=Setting(F_PAD, 'rumble_strength', K_SLIDER, lo=0.0, hi=2.0, step=0.05, d='1.0', label='Vibration strength'),
    input=Setting(F_PAD, 'input', K_COMBO, O_INPUT, 0, d='both', label='Input',
                  tip='Lock the game to one kind of input. Keyboard and mouse only: controllers are ignored (no buttons, no vibration). '
                      'Controller only: the keyboard and mouse are ignored (from the next game start; the controller part applies at once).'),
)
PAD_KEYS = ('layout', 'deadzone', 'rumble', 'strength')   # keys a [device ...] section may set for its pad
SIZES = [(1280, 720), (1600, 900), (1920, 1080), (2560, 1440), (3840, 2160)]


def fmt_num(v):                                          # 1.5 -> "1.5", 1 -> "1.0", 0.75 -> "0.75" (launcher.c fmt_num)
    s = '%.2f' % v
    while len(s) > 3 and s.endswith('0') and s[-2] != '.':
        s = s[:-1]
    return s


def to_int(t, d=0):
    m = re.match(r'\s*([+-]?\d+)', t or '')
    return int(m.group(1)) if m else d


def to_float(t):
    m = re.match(r'\s*([+-]?(\d+\.?\d*|\.\d+)([eE][+-]?\d+)?)', t or '')
    return float(m.group(1)) if m else 0.0


def combo_index(s, raw):                                 # as gfx_options.c / nnpad_sdl.c read it
    if s is S_['aniso']:
        v = to_int(raw)
        return next((i for i in range(1, len(s.opt)) if int(s.opt[i][0]) == v), 0)
    if s is S_['renderer'] and raw.lower() == 'gl':
        return 1
    if s is S_['layout'] and raw.lower() == 'label':
        return 2
    return next((i for i, (v, _) in enumerate(s.opt) if raw.lower() == v), s.fallback)


def check_value(s, raw):
    if s is S_['rumble']:
        return raw.lower() not in ('off', '0', 'no', 'false')
    return raw.lower() == 'on' or raw == '1'


def slider_max(s):
    return int((s.hi - s.lo) / s.step + 0.5)


def slider_value(s, pos):
    return s.lo + pos * s.step


# ================================================================ small helpers
def is_file(p):
    return os.path.isfile(p)


def open_folder(path):
    os.makedirs(path, exist_ok=True)
    if shutil.which('open'):
        subprocess.Popen(['open', path], stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                         start_new_session=True)
        return
    messagebox.showinfo('B2recomp', 'Open this folder in your file manager:\n%s' % path)


def game_built():
    return is_file(GAME) and is_file(GAME_SO) and is_file(ELF)


def needs_update():
    """A newer builder unpacked over a finished build: its VERSION is not the one setup.py built (build/setup_state.json)."""
    try:
        with open(os.path.join(ROOT, 'VERSION'), encoding='utf-8') as f:
            v = f.read().strip()
        with open(os.path.join(BUILD, 'setup_state.json'), encoding='utf-8') as f:
            st = json.load(f)
    except (OSError, ValueError):
        return False
    return bool(v) and bool(st.get('done')) and st.get('version') != v


def vulkan_problem():
    """None when a Vulkan loader and at least one Vulkan driver are installed; else what to tell the player."""
    import ctypes
    import glob
    try:
        ctypes.CDLL('libvulkan.dylib')
    except OSError:
        return ('No Vulkan loader on this PC (libvulkan.dylib). Install the loader via Homebrew: vulkan-loader, or with '
                'the Vulkan SDK.')
    if os.environ.get('VK_DRIVER_FILES') or os.environ.get('VK_ICD_FILENAMES'):
        return None
    dirs = ['/opt/homebrew/lib/vulkan/icd.d', '/usr/local/lib/vulkan/icd.d',
            '/opt/homebrew/etc/vulkan/icd.d',
            os.path.expanduser('~/Library/Application Support/Vulkan/icd.d')]
    for f in os.environ.get('VK_DRIVER_FILES', '').split(':') + os.environ.get('VK_ICD_FILENAMES', '').split(':'):
        if f and os.path.isfile(f):
            return None
    # Also check via MoltenVK / Homebrew loader
    if any(glob.glob(os.path.join(d, '*.json')) for d in dirs):
        return None
    return ('No Vulkan driver found. Install MoltenVK / Vulkan drivers via Homebrew (vulkan-loader, molten-vk) '
            'or the macOS Vulkan SDK.')


DESKTOP = os.path.join(os.path.expanduser('~'), '.local', 'share', 'applications', 'b2recomp.desktop')


def add_to_menu():
    """The app-menu entry of this folder's B2recomp (the icon: build/bayonetta2.ico, made from the player's own dump)."""
    icon = 'applications-games'
    ico = os.path.join(BUILD, 'bayonetta2.ico')
    if is_file(ico):
        try:
            from PIL import Image
            im = Image.open(ico)
            im.size = max(im.info.get('sizes', [im.size]))
            png = os.path.join(BUILD, 'b2recomp-icon.png')
            im.convert('RGBA').save(png)
            icon = png
        except Exception:
            pass

    def esc(t):                                           # a desktop-file string: backslash escapes, Exec's % doubled
        return t.replace('\\', '\\\\')
    exe = os.path.join(ROOT, 'B2recomp')
    quoted = '"%s"' % exe.replace('\\', '\\\\').replace('"', '\\"').replace('`', '\\`').replace('$', '\\$')
    lines = ['[Desktop Entry]', 'Type=Application', 'Name=B2recomp',
             'Comment=Bayonetta 2 Recompiled, built on this PC from your own dump',
             'Exec=sh ' + esc(quoted).replace('%', '%%'), 'Path=' + esc(ROOT), 'Icon=' + esc(icon), 'Terminal=false',
             'Categories=Game;', 'StartupWMClass=B2recomp']
    os.makedirs(os.path.dirname(DESKTOP), exist_ok=True)
    with open(DESKTOP, 'w', encoding='utf-8') as f:
        f.write('\n'.join(lines) + '\n')
    os.chmod(DESKTOP, 0o755)
    if shutil.which('update-desktop-database'):
        subprocess.run(['update-desktop-database', os.path.dirname(DESKTOP)], capture_output=True)
    return DESKTOP


def menu_entry_needed():
    """None, or why this folder's app-menu entry has to be (re)written: missing on the first start, another folder's, or
    without the game's icon that the build has made since. Once made, an entry the player removed stays removed."""
    if not os.path.isfile(DESKTOP):
        return None if launcher_state().get('menu_added') else 'added'
    try:
        with open(DESKTOP, encoding='utf-8') as f:
            text = f.read()
    except OSError:
        return None
    if 'Path=%s\n' % ROOT.replace('\\', '\\\\') not in text:
        return 'moved'
    if is_file(os.path.join(BUILD, 'bayonetta2.ico')) and 'b2recomp-icon.png' not in text:
        return 'icon'
    return None


def remove_from_menu():
    if os.path.isfile(DESKTOP):
        os.remove(DESKTOP)
    launcher_state({'menu_added': True})                  # not made again by itself


def launcher_state(update=None):                          # build/launcher_state.json: what the launcher asked once
    p = os.path.join(BUILD, 'launcher_state.json')
    try:
        with open(p, encoding='utf-8') as f:
            st = json.load(f)
    except (OSError, ValueError):
        st = {}
    if update:
        st.update(update)
        try:
            os.makedirs(BUILD, exist_ok=True)
            with open(p, 'w', encoding='utf-8') as f:
                json.dump(st, f)
        except OSError:
            pass
    return st


def find_python():
    for n in ('python3.13', 'python3'):                   # the package has python3.13 (python3 is a symlink, not in the zip)
        p = os.path.join(ROOT, 'deps', 'python', 'bin', n)
        if os.access(p, os.X_OK):
            return p, True
    return (shutil.which('python3') or sys.executable or ''), False


def game_env():
    env = {k: v for k, v in os.environ.items() if k not in CLEAR_ENV}
    lib_path = '/opt/homebrew/lib:' + env.get('DYLD_LIBRARY_PATH', '') if env.get('DYLD_LIBRARY_PATH') else '/opt/homebrew/lib'
    env['DYLD_LIBRARY_PATH'] = OUT + ':' + lib_path
    env['SDL_VIDEODRIVER'] = 'cocoa'
    env['VK_ICD_FILENAMES'] = os.path.join('/opt/homebrew/etc/vulkan/icd.d', 'MoltenVK_icd.json')
    return env


def proc_running(exe):
    """Is a process of this program running (started by Play or anything else)?"""
    want = os.path.realpath(exe)
    # On macOS, use ps to find processes matching the executable
    import subprocess
    try:
        # Use ps to find processes
        result = subprocess.run(['ps', 'aux'], capture_output=True, text=True)
        for line in result.stdout.splitlines():
            # Extract command path from ps output
            parts = line.split()
            if len(parts) >= 11:
                cmd = parts[10]
                # Check if the command contains our executable path
                if want in cmd or os.path.basename(cmd) == os.path.basename(want):
                    return True
    except (OSError, ValueError):
        pass
    return False


def kill_group(p):
    if p and p.poll() is None:
        try:
            os.killpg(p.pid, signal.SIGTERM)
        except OSError:
            pass


class Runner:
    """A program with its output read line by line on a thread; lines and the exit code arrive through poll()."""

    def __init__(self, argv, cwd, env):
        self.q = queue.Queue()
        self.p = subprocess.Popen(argv, cwd=cwd, env=env, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                                  stderr=subprocess.STDOUT, start_new_session=True)
        threading.Thread(target=self._read, daemon=True).start()

    def _read(self):
        buf = b''
        while True:
            b = self.p.stdout.read1(4096) if hasattr(self.p.stdout, 'read1') else self.p.stdout.read(4096)
            if not b:
                break
            buf += b
            while b'\n' in buf:
                line, buf = buf.split(b'\n', 1)
                self.q.put(('line', line.rstrip(b'\r').decode('utf-8', 'replace')))
        if buf:
            self.q.put(('line', buf.decode('utf-8', 'replace')))
        self.q.put(('done', self.p.wait()))

    def poll(self):
        out = []
        try:
            while True:
                out.append(self.q.get_nowait())
        except queue.Empty:
            pass
        return out

    def stop(self):
        kill_group(self.p)


# ================================================================ the window
class Launcher:
    def __init__(self, root, build_mode, update=False):
        self.root = root
        self.mode = 1 if build_mode else 0
        self.update = update                              # a newer B2recomp over an old build: BUILD brings the game up to date
        self.python, self.bundled = find_python()
        self.proc = None                                  # the game started by Play
        self.brun = None                                  # setup.py of the build screen
        self.kwin = self.mwin = None
        self.widgets = []                                 # this screen's controls (destroyed when the screen changes)
        self.tips = {}
        self._scale()
        self._style()
        root.title('B2recomp' if self.mode else 'B2recomp Launcher')
        root.configure(bg=C_BG)
        root.resizable(False, False)
        W, H = self.S(WIN_W), self.S(WIN_H)
        sw, sh = root.winfo_screenwidth(), root.winfo_screenheight()
        root.geometry('%dx%d+%d+%d' % (W, H, max(0, (sw - W) // 2), max(0, (sh - H) // 2 - self.S(16))))
        self.cv = tk.Canvas(root, width=W, height=H, bg=C_BG, highlightthickness=0, bd=0)
        self.cv.place(x=0, y=0)
        self.set_icon(root)
        self.paint()
        if self.mode:
            self.create_build_controls()
        else:
            self.create_controls()
            self.load_all()
        root.protocol('WM_DELETE_WINDOW', self.on_close)
        root.after(100, self.tick)
        root.after(600, self.ensure_menu)

    # ---------------------------------------------------------------- scale, fonts, theme
    def _scale(self):
        try:
            dpi = float(os.environ.get('B2RECOMP_LAUNCHER_DPI') or 0) or self.root.winfo_fpixels('1i')
        except (ValueError, tk.TclError):
            dpi = 96.0
        k = max(1.0, dpi / 96.0) if not os.environ.get('B2RECOMP_LAUNCHER_DPI') else dpi / 96.0
        sh = self.root.winfo_screenheight()                # the window must fit the screen (finding 123)
        if WIN_H * k > sh - 48:
            k = max(0.6, (sh - 48) / WIN_H)
        self.k = k
        fams = set(tkfont.families(self.root))
        base = tkfont.nametofont('TkDefaultFont').actual('family')
        for f in ('Segoe UI', 'Noto Sans', 'Cantarell', 'Ubuntu', 'DejaVu Sans', 'Liberation Sans'):
            if f in fams:
                base = f
                break
        serif = next((f for f in ('Palatino Linotype', 'TeX Gyre Pagella', 'URW Palladio L', 'P052', 'Palatino', 'C059',
                                  'Noto Serif', 'DejaVu Serif', 'Liberation Serif') if f in fams), 'Times')
        px = lambda n: -max(6, int(round(n * k)))          # negative size = pixels
        self.font = tkfont.Font(family=base, size=px(12))
        self.small = tkfont.Font(family=base, size=px(11))
        self.head = tkfont.Font(family=base, size=px(12), weight='bold')
        self.big = tkfont.Font(family=base, size=px(16), weight='bold')
        self.title = tkfont.Font(family=serif, size=px(45), weight='bold')

    def S(self, v):
        return int(round(v * self.k))

    def _style(self):
        st = ttk.Style(self.root)
        st.theme_use('clam')
        r = self.root
        r.option_add('*TCombobox*Listbox.background', C_FIELD)
        r.option_add('*TCombobox*Listbox.foreground', C_TEXT)
        r.option_add('*TCombobox*Listbox.selectBackground', C_ACCENT)
        r.option_add('*TCombobox*Listbox.selectForeground', '#ffffff')
        r.option_add('*TCombobox*Listbox.font', self.font)
        st.configure('.', background=C_PANEL, foreground=C_TEXT, fieldbackground=C_FIELD, bordercolor=C_EDGE,
                     lightcolor=C_FIELD, darkcolor=C_FIELD, troughcolor=C_FIELD, focuscolor=C_EDGE, font=self.font,
                     selectbackground=C_FIELD, selectforeground=C_TEXT, insertcolor=C_TEXT, arrowcolor=C_TEXT)
        st.configure('TCombobox', padding=(self.S(4), self.S(1)), arrowsize=self.S(12))
        st.map('TCombobox', fieldbackground=[('readonly', C_FIELD), ('disabled', C_PANEL)],
               foreground=[('disabled', C_DIM), ('readonly', C_TEXT)], background=[('active', '#232a46'), ('!active', C_FIELD)],
               arrowcolor=[('disabled', C_DIM)], bordercolor=[('focus', C_ACCENT)],
               selectbackground=[('readonly', C_FIELD)], selectforeground=[('readonly', C_TEXT)])
        for name, font in (('Dark.TButton', self.font), ('Small.Dark.TButton', self.small)):   # Small: a long text in a narrow button
            st.configure(name, background=C_FIELD, foreground=C_TEXT, bordercolor='#4a5680', lightcolor=C_FIELD,
                         darkcolor=C_FIELD, padding=(self.S(2), self.S(1)), anchor='center', font=font)
            st.map(name, background=[('disabled', C_PANEL), ('pressed', '#2b3456'), ('active', '#232a46')],
                   foreground=[('disabled', C_DIM)], bordercolor=[('focus', C_ACCENT), ('disabled', C_EDGE)])
        st.configure('Slot.TButton', background=C_FIELD, foreground=C_TEXT, bordercolor=C_EDGE, lightcolor=C_FIELD,
                     darkcolor=C_FIELD, padding=(self.S(2), self.S(2)))
        st.map('Slot.TButton', background=[('pressed', '#2b3456'), ('active', '#232a46')], bordercolor=[('focus', C_ACCENT)])
        st.configure('Wait.TButton', background='#3a3420', foreground=C_VALUE, bordercolor=C_GOLD, lightcolor='#3a3420',
                     darkcolor='#3a3420', padding=(self.S(2), self.S(2)))
        st.configure('Dark.TCheckbutton', background=C_PANEL, foreground=C_TEXT, indicatorbackground=C_FIELD,
                     indicatorforeground='#ffffff', indicatorrelief='flat', indicatormargin=0, padding=0)
        st.map('Dark.TCheckbutton', background=[('active', C_PANEL)], indicatorbackground=[('disabled', C_PANEL),
               ('selected', C_ACCENT), ('active', '#232a46')])
        st.configure('Dark.Horizontal.TScale', background=C_ACCENT, troughcolor=C_FIELD, bordercolor=C_EDGE,
                     lightcolor=C_ACCENT, darkcolor=C_ACCENT, sliderlength=self.S(12), sliderthickness=self.S(18),
                     gripcount=0)
        st.map('Dark.Horizontal.TScale', background=[('disabled', C_EDGE), ('active', '#4b98ee')],
               lightcolor=[('disabled', C_EDGE)], darkcolor=[('disabled', C_EDGE)])
        st.configure('Dark.Vertical.TScrollbar', background=C_EDGE, troughcolor=C_FIELD, bordercolor=C_FIELD,
                     arrowcolor=C_TEXT, lightcolor=C_EDGE, darkcolor=C_EDGE, gripcount=0)
        st.map('Dark.Vertical.TScrollbar', background=[('disabled', C_FIELD), ('pressed', '#4a5680'), ('active', '#3a4468')],
               arrowcolor=[('disabled', C_DIM)], lightcolor=[('disabled', C_FIELD)], darkcolor=[('disabled', C_FIELD)])
        st.configure('Dark.Treeview', background=C_FIELD, fieldbackground=C_FIELD, foreground=C_TEXT, bordercolor=C_EDGE,
                     rowheight=self.S(22), font=self.font)
        st.map('Dark.Treeview', background=[('selected', C_ACCENT)], foreground=[('selected', '#ffffff')])

    def set_icon(self, win):
        """build/bayonetta2.ico (made from the player's own dump by setup.py's icon step) as the window icon."""
        ico = os.path.join(BUILD, 'bayonetta2.ico')
        if not is_file(ico):
            return
        try:
            from PIL import Image
            im = Image.open(ico)
            im.size = max(im.info.get('sizes', [im.size]))
            buf = io.BytesIO()
            im.convert('RGBA').resize((64, 64)).save(buf, 'PNG')
            self._icon = tk.PhotoImage(data=base64.b64encode(buf.getvalue()))
            win.iconphoto(True, self._icon)
        except Exception:
            pass

    # ---------------------------------------------------------------- drawing
    def spaced(self, x, y, text, font, color, extra, anchor='nw'):
        """Text with extra letter spacing (as SetTextCharacterExtra), drawn letter by letter."""
        w = sum(font.measure(c) + extra for c in text)
        if anchor == 'n':
            x -= w // 2
        for c in text:
            self.cv.create_text(x, y, text=c, font=font, fill=color, anchor='nw', tags='paint')
            x += font.measure(c) + extra
        return w

    def round_rect(self, x0, y0, x1, y1, r, fill, outline):
        pts = [x0 + r, y0, x1 - r, y0, x1, y0, x1, y0 + r, x1, y1 - r, x1, y1, x1 - r, y1, x0 + r, y1, x0, y1, x0, y1 - r,
               x0, y0 + r, x0, y0]
        self.cv.create_polygon(pts, smooth=True, fill=fill, outline=outline, tags='paint')

    def paint(self):
        cv, S = self.cv, self.S
        cv.delete('paint')
        # the header: the title as text (the Windows launcher's no-art header); never any game picture
        for i in range(S(HEADER)):                       # a faint glow behind the title, fading into the background
            t = i / max(1, S(HEADER) - 1)
            f = max(0.0, 1.0 - abs(t - 0.42) / 0.42) ** 2 * 0.55
            c = tuple(int(a + (b - a) * f) for a, b in ((7, 22), (10, 30), (20, 58)))
            cv.create_line(0, i, S(WIN_W), i, fill='#%02x%02x%02x' % c, tags='paint')
        self.spaced(S(WIN_W) // 2, S(70), 'B2RECOMP', self.title, C_GOLD, S(6), anchor='n')
        self.spaced(S(18), S(14), 'BAYONETTA 2 RECOMPILED', self.head, C_SUB, S(3))
        titles = ['BUILD THE GAME'] if self.mode else ['GRAPHICS', 'CONTROLLER']
        for k, title in enumerate(titles):
            l, t, r, b = BPANEL if self.mode else PANELS[k]
            self.round_rect(S(l), S(t), S(r), S(b), S(6), C_PANEL, C_EDGE)
            cv.create_line(S(l + 16), S(t + 36), S(r - 16), S(t + 36), fill=C_EDGE, tags='paint')
            cv.create_line(S(l + 16), S(t + 36), S(l + 52), S(t + 36), fill=C_GOLD, width=max(2, S(2)), tags='paint')
            self.spaced(S(l + 16), S(t + 12), title, self.head, C_GOLD, S(3))
        cv.tag_lower('paint')

    # ---------------------------------------------------------------- controls
    def place(self, w, x, y, wd=None, ht=None):
        kw = dict(x=self.S(x), y=self.S(y))
        if wd is not None:
            kw['width'] = self.S(wd)
        if ht is not None:
            kw['height'] = self.S(ht)
        w.place(**kw)
        self.widgets.append(w)
        return w

    def label(self, text, x, y, wd, ht=20, color=C_TEXT, font=None, bg=C_PANEL, anchor='w', wrap=True, justify='left'):
        font = font or self.font
        if ht <= 20 and text and font.measure(text) > self.S(wd):   # one line that does not fit: the small font
            font = self.small
        lb = tk.Label(self.cv, text=text, bg=bg, fg=color, font=font, anchor=anchor, justify=justify,
                      wraplength=self.S(wd) if wrap else 0, padx=0, pady=0, bd=0)
        return self.place(lb, x, y, wd, ht)

    def button(self, text, x, y, wd, ht, cmd, tip=None, parent=None):
        style = 'Small.Dark.TButton' if self.font.measure(text) > self.S(wd - 10) else 'Dark.TButton'
        b = ttk.Button(parent or self.cv, text=text, style=style, command=cmd, takefocus=True)
        if parent is None:
            self.place(b, x, y, wd, ht)
        else:
            b.place(x=self.S(x), y=self.S(y), width=self.S(wd), height=self.S(ht))
        if tip:
            Tip(self, b, tip)
        return b

    def mk_setting(self, name, lx, cx, cw, y):
        s, S = S_[name], self.S
        if s.kind == K_CHECK:
            s.var = tk.IntVar(value=0)
            if name == 'overlay':
                s.w = self.place(CheckBox(self, self.cv, s.var, self.update_ui), lx, y + 3)
                s.lab = self.label(s.label, lx + 24, y + 4, cx + cw - lx - 24)
            else:
                s.lab = self.label(s.label, lx, y + 4, cx - lx - 6)
                s.w = self.place(CheckBox(self, self.cv, s.var, self.update_ui), cx, y + 3)
                s.val = self.label('On', cx + 24, y + 4, 60)
                s.val.bind('<Button-1>', lambda e, s=s: s.w.invoke())
            if name == 'overlay':
                s.lab.bind('<Button-1>', lambda e, s=s: s.w.invoke())
        else:
            if s.label:
                s.lab = self.label(s.label, lx, y + 4, cx - lx - 6)
            if s.kind == K_COMBO:
                s.w = ttk.Combobox(self.cv, values=[t for _, t in s.opt], state='readonly', font=self.font, takefocus=True)
                self.place(s.w, cx, y, cw, 24)
                s.w.bind('<<ComboboxSelected>>', lambda e: (e.widget.selection_clear(), self.update_ui()))
            elif s.kind == K_SLIDER:
                big = name == 'scale'
                s.w = Slider(self, self.cv, slider_max(s), 5 if big else 10, lambda pos, s=s: self.slider_moved(s, pos))
                self.place(s.w, cx - 4, y, cw + 8 if big else cw - 52, 26)
                if big:
                    s.val = self.label('', cx, y + 28, cw, 18, color=C_VALUE)
                else:
                    s.val = self.label('', cx + cw - 48, y + 4, 48, color=C_VALUE, anchor='e', justify='right')
        if s.tip and s.w is not None:
            Tip(self, s.w, s.tip)
            if s.lab is not None:
                Tip(self, s.lab, s.tip)

    def slider_moved(self, s, pos):                       # the user moved it (Slider calls this only then)
        s.moved = True
        self.slider_label(s)
        self.update_ui()

    def slider_pos(self, s):
        return s.w.get()

    def slider_label(self, s):
        v = s.filev if s.exact and not s.moved else slider_value(s, self.slider_pos(s))
        if s is S_['scale']:
            t = '%.2fx   (%d x %d)' % (v, int(1280 * v + 0.5), int(720 * v + 0.5))
        elif s is S_['fps']:
            t = '%d fps' % int(v + 0.5)
        else:
            t = '%d %%' % int(v * 100 + 0.5)
        s.val.configure(text=t)

    def create_controls(self):
        self._loading = False
        gp = PANELS[0]
        lx, cx = gp[0] + 16, gp[0] + 158
        cw, y, dy = gp[2] - 16 - cx, gp[1] + 46, 32
        for n in ('renderer', 'profile', 'window'):
            self.mk_setting(n, lx, cx, cw, y)
            y += dy
        W, H = S_['width'], S_['height']
        W.lab = self.label(W.label, lx, y + 4, cx - lx - 6)
        vcmd = (self.root.register(lambda t: t.isdigit() and len(t) <= 5 or t == ''), '%P')
        for s, x in ((W, cx), (H, cx + 72)):
            s.var = tk.StringVar()
            s.w = tk.Entry(self.cv, textvariable=s.var, justify='center', bg=C_FIELD, fg=C_TEXT, insertbackground=C_TEXT,
                           disabledbackground=C_PANEL, disabledforeground=C_DIM, relief='flat', highlightthickness=1,
                           highlightbackground='#4a5680', highlightcolor=C_ACCENT, font=self.font, validate='key',
                           validatecommand=vcmd)
            self.place(s.w, x, y, 56, 23)
            s.var.trace_add('write', lambda *a: self.update_ui())
            Tip(self, s.w, 'Client size of the window in windowed mode.')
        H.lab = self.label('x', cx + 56, y + 4, 16, anchor='center', justify='center')
        self.sizes = ttk.Combobox(self.cv, values=['Custom'] + ['%d x %d' % wh for wh in SIZES], state='readonly', font=self.font)
        self.place(self.sizes, cx + 136, y, cw - 136, 24)
        self.sizes.bind('<<ComboboxSelected>>', self.size_chosen)
        y += dy
        for n, extra in (('vsync', 0), ('fps', 0), ('latency', 0), ('scale', 18), ('aniso', 0), ('aspect', 0), ('overlay', 0)):
            self.mk_setting(n, lx, cx, cw, y)
            y += dy + extra
        self.gfxnote = self.label('', lx, y + 2, gp[2] - gp[0] - 32, 34, color=C_NOTE, font=self.small)
        pp = PANELS[1]
        pw = pp[2] - pp[0]
        lx, cx = pp[0] + 16, pp[0] + 122
        cw, y = pp[2] - 16 - cx, pp[1] + 46
        for n, extra in (('input', 0), ('layout', 0), ('prompts', 0), ('deadzone', 0), ('rumble', 0), ('strength', 12)):
            self.mk_setting(n, lx, cx, cw, y)
            y += dy + extra
        self.button('Remap controller...', lx, y, pw - 32, 30, lambda: self.run_tool(['--pad-map', '1'], 'Remap controller (player 1)'),
                    'Runs game_native --pad-map 1 in a window of its own: press each button it names on player 1\'s pad '
                    '(skipped after 8 s). It writes a [device] section for that pad to controllers.ini.')
        y += 38
        self.button('Test controller...', lx, y, pw - 32, 30, lambda: self.run_tool(['--pad-test'], 'Test controller'),
                    'Runs game_native --pad-test in a window of its own: lists the pads SDL sees and shows the buttons you press. '
                    'Close the window to stop.')
        y += 38
        self.button('Keyboard and mouse...', lx, y, pw - 32, 30, self.keys_window,
                    'Rebind the keyboard and mouse (two keys or mouse buttons per action) and set the mouse camera. '
                    'Writes the [keyboard] section of controllers.ini.')
        y += 44
        self.padnote = self.label('', lx, y, pw - 32, 64, color=C_NOTE, font=self.small)
        by, px, bw = PANELS[0][3] + 12, WIN_W - 14 - 150, 76
        x0 = px - 12 - 5 * bw - 4 * 8                     # Report..., Mods..., Config folder, Save, Close
        self.status = self.label('', 16, by + 3, x0 - 16 - 16, 36, color=C_NOTE, font=self.small, bg=C_BG)
        self.button('Report...', x0, by + 3, bw, 34, self.make_report,
                    'Makes b2recomp-report.tar.gz in the B2recomp folder: the logs and this PC\'s details (Linux version, '
                    'desktop, CPU, graphics driver), nothing of the game. Send it to whoever helps you.')
        self.button('Mods...', x0 + (bw + 8), by + 3, bw, 34, self.mods_window,
                    'Code mods: switch the emulator patches (.pchtxt, .ips) in the mods folder on and off. They are compiled into '
                    'the game, so a change needs a rebuild (about a minute; the window does it).')
        self.button('Config...', x0 + 2 * (bw + 8), by + 3, bw, 34, lambda: open_folder(CFG),
                    'Opens the folder with graphics.ini and controllers.ini (every option is explained there).')
        self.save_btn = self.button('Save', x0 + 3 * (bw + 8), by + 3, bw, 34, self.save_all)
        self.button('Close', x0 + 4 * (bw + 8), by + 3, bw, 34, self.on_close)
        self.play_btn = GoldButton(self, 'Play', self.play)
        self.place(self.play_btn, px, by, 150, 40)
        self.root.bind('<Return>', lambda e: self.play_btn.invoke() if not isinstance(e.widget, (tk.Entry,)) else None)

    def size_chosen(self, e=None):
        i = self.sizes.current()
        self.sizes.selection_clear()
        if i > 0:
            S_['width'].var.set(str(SIZES[i - 1][0]))
            S_['height'].var.set(str(SIZES[i - 1][1]))
        self.update_ui()

    # ---------------------------------------------------------------- values: file text <-> controls
    def current(self, s):
        if s.kind == K_COMBO:
            i = s.w.current()
            return s.opt[i if i >= 0 else 0][0]
        if s.kind == K_NUM:
            return '%d' % to_int(s.var.get())
        if s.kind == K_CHECK:
            return 'on' if s.var.get() else 'off'
        v = slider_value(s, self.slider_pos(s))
        if s is S_['deadzone']:
            return '%.2f' % v
        if s is S_['fps']:
            return '%d' % int(v + 0.5)
        return fmt_num(v)

    def changed(self, s):
        if s.kind == K_SLIDER:
            return self.slider_pos(s) != s.loadpos or (s.exact and s.moved)
        return self.current(s) != s.loaded

    def enable(self, s, on):
        if s.kind == K_COMBO:
            s.w.configure(state='readonly' if on else 'disabled')
        elif s.kind == K_NUM:
            s.w.configure(state='normal' if on else 'disabled')
        else:
            s.w.state(['!disabled'] if on else ['disabled'])
        for lb in (s.lab, s.val):
            if lb is not None:
                lb.configure(fg=(C_VALUE if (lb is s.val and s.kind == K_SLIDER) else C_TEXT) if on else C_DIM)

    def update_ui(self, *_):
        if getattr(self, '_loading', False) or self.mode:
            return
        vk = self.current(S_['renderer']) == 'vulkan'
        custom = self.current(S_['profile']) == 'custom'
        windowed = self.current(S_['window']) == 'windowed'
        for n in ('width', 'height'):
            self.enable(S_[n], windowed)
        self.sizes.configure(state='readonly' if windowed else 'disabled')
        for n in ('vsync', 'scale', 'aniso'):
            self.enable(S_[n], vk and custom)
        for n in ('latency', 'aspect'):
            self.enable(S_[n], vk)
        for n in ('renderer', 'profile', 'window', 'fps', 'overlay', 'layout', 'prompts', 'deadzone', 'rumble', 'input'):
            self.enable(S_[n], True)
        rumble = bool(S_['rumble'].var.get())
        self.enable(S_['strength'], rumble)
        S_['rumble'].val.configure(text='On' if rumble else 'Off')
        self.gfxnote.configure(text='OpenGL ignores the greyed settings (they are Vulkan only).' if not vk
                               else 'The Low preset sets vsync, resolution scale and filtering itself.' if not custom
                               else 'Graphics settings take effect the next time the game starts.')
        w, h = to_int(S_['width'].var.get()), to_int(S_['height'].var.get())
        self.sizes.current(next((i + 1 for i, wh in enumerate(SIZES) if wh == (w, h)), 0))
        dirty = any(self.changed(s) for s in S_.values())
        self.save_btn.state(['!disabled'] if dirty else ['disabled'])
        if not self.proc:
            self.status.configure(text='Unsaved changes (Play saves them first).' if dirty else '')

    def load_all(self):
        f = [Ini(INI[0]), Ini(INI[1])]
        self._loading = True
        for s in S_.values():
            raw = f[s.file].get('general' if s.file == F_PAD else None, s.key, s.file == F_PAD)
            if raw is None:
                raw = s.d
            if s.kind == K_COMBO:
                x = combo_index(s, raw)
                s.w.current(x)
                s.loaded = s.opt[x][0]
            elif s.kind == K_CHECK:
                on = check_value(s, raw)
                s.var.set(1 if on else 0)
                s.loaded = 'on' if on else 'off'
            elif s.kind == K_NUM:
                v = to_int(raw)
                if v < s.nlo or v > s.nhi:
                    v = int(s.d)                         # the parser keeps its default for a value out of range
                s.loaded = '%d' % v
                s.var.set(s.loaded)
            else:
                v = to_float(raw)
                if s is S_['scale'] and not v > 0:
                    v = 1.0
                pos = min(max(int((v - s.lo) / s.step + 0.5) if v >= s.lo else 0, 0), slider_max(s))
                s.filev, s.loadpos, s.moved = v, pos, False
                s.exact = abs(slider_value(s, pos) - v) > 1e-6
                s.w.set(pos)
                self.slider_label(s)
        # [device <GUID>] sections with their own value of one of these keys win over [general] for that pad
        cur, counted, n = '', False, 0
        for l in f[F_PAD].lines:
            sec = Ini.section(l)
            if sec is not None:
                cur, counted = sec, False
                continue
            if counted or not cur.lower().startswith('device '):
                continue
            r = Ini.kv(l, True)
            if r and any(r[0].lower() == S_[k].key for k in PAD_KEYS):
                n += 1
                counted = True
        self.padnote.configure(text='%d pad section(s) of controllers.ini set their own value of one of these; that value wins '
                                    'for that pad.' % n if n else 'Controller settings take effect at once, also while the game runs.')
        self._loading = False
        self.update_ui()

    def save_all(self):
        for n in ('width', 'height'):
            s = S_[n]
            v = to_int(s.var.get())
            if self.changed(s) and not s.nlo <= v <= s.nhi:
                messagebox.showwarning('B2recomp', 'The window %s must be between %d and %d.' % (n, s.nlo, s.nhi), parent=self.root)
                s.w.focus_set()
                return False
        nch = [0, 0]
        for k in (F_GFX, F_PAD):
            todo = [s for s in S_.values() if s.file == k and self.changed(s)]
            if not todo:
                continue
            f = Ini(INI[k])                               # fresh from disk: --pad-map or an editor may have written it
            for s in todo:
                f.set('general' if k == F_PAD else None, s.key, k == F_PAD, self.current(s))
                nch[k] += 1
            if not f.save():
                messagebox.showerror('B2recomp', 'Could not write %s.' % INI[k], parent=self.root)
                return False
        self.load_all()
        if nch[0] + nch[1] and not self.proc:
            self.set_status('Saved: %d graphics and %d controller setting(s).' % (nch[0], nch[1]))
        return True

    def set_status(self, t):
        self.status.configure(text=t)

    # ---------------------------------------------------------------- starting things
    def have_game(self):
        if not is_file(GAME) or not is_file(GAME_SO):
            messagebox.showinfo('B2recomp', 'The game is not built yet: start B2recomp with --build, or run '
                                'deps/python/bin/python3 scripts/setup.py --dump <your dump>', parent=self.root)
            return False
        if is_file(ELF):
            return True
        messagebox.showerror('B2recomp', 'Not found:\n%s' % ELF, parent=self.root)
        return False

    def run_tool(self, args, title):
        if self.have_game():
            ToolWindow(self, title, [GAME] + args)

    def play(self):
        if self.proc or not self.save_all() or not self.have_game():
            return
        problem = vulkan_problem()
        if problem and not messagebox.askyesno('B2recomp', problem + '\n\nStart the game anyway?', icon='warning',
                                               parent=self.root):
            return
        user = os.path.join(OUT, 'play', 'userdata')
        os.makedirs(user, exist_ok=True)
        if not os.path.exists(os.path.join(user, 'save')):  # first start: slot 1 at the first chapter, when there is one
            pro = os.path.join(BUILD, 'saves', 'after-prologue')
            if os.path.exists(os.path.join(pro, 'save')):
                shutil.copytree(pro, user, dirs_exist_ok=True)
        env = game_env()
        env.update(SSBU_ROMFS=os.path.join(ROOT, 'targets', 'romfs'), SSBU_USERDATA=user, SSBU_SHOW='1', SSBU_VOLUME='100',
                   SSBU_FOCUS_NOTIFY='0')
        cfg_dir = os.path.join(ROOT, 'config')
        controllers_ini = os.path.join(cfg_dir, 'controllers.ini')
        graphics_ini = os.path.join(cfg_dir, 'graphics.ini')
        if os.path.isfile(controllers_ini):
            env['SSBU_CONFIG'] = controllers_ini
        if os.path.isfile(graphics_ini):
            env['SSBU_GRAPHICS_CONFIG'] = graphics_ini
        fonts = os.path.join(ROOT, 'targets', 'sysdata', 'fonts')
        if os.path.isdir(fonts):
            env['SSBU_FONTS'] = fonts
        else:
            env.pop('SSBU_FONTS', None)
        log = os.path.join(OUT, 'play', 'stdout.txt')
        try:
            with open(log, 'wb') as lf:
                self.proc = subprocess.Popen([GAME, ELF], cwd=OUT, env=env, stdin=subprocess.DEVNULL, stdout=lf,
                                             stderr=subprocess.STDOUT, start_new_session=True)
        except OSError as e:
            messagebox.showerror('B2recomp', 'Could not start build-darwin/game_native (%s).' % e, parent=self.root)
            return
        self.play_btn.set_text('Running')
        self.play_btn.enable(False)
        self.status.configure(text='The game is running (log: build-darwin/play/stdout.txt).')

    def tick(self):
        if self.proc and self.proc.poll() is not None:
            code = self.proc.returncode
            self.proc = None
            self.play_btn.set_text('Play')
            self.play_btn.enable(True)
            self.status.configure(text='')
            self.update_ui()
            if code:                                      # an error or a crash (closing the window is exit code 0)
                self.root.after(100, lambda: self.game_stopped(code))
        if self.mode and self.brun:
            self.build_poll()
        self.root.after(100, self.tick)

    def game_stopped(self, code):
        """The game ended with an error: the reason from its log (the line before its summary), and a report."""
        why = ''
        try:
            with open(os.path.join(OUT, 'play', 'stdout.txt'), 'rb') as f:
                f.seek(0, 2)
                f.seek(max(0, f.tell() - 262144))
                lines = f.read().decode('utf-8', 'replace').splitlines()
            for keys in (('stopping',), ('FATAL', 'fatal', 'Fatal'), ('cannot', 'failed', 'not found', 'no Vulkan')):
                why = next((l.strip() for l in reversed(lines) if any(k in l for k in keys)), '')
                if why:
                    break
        except OSError:
            pass
        what = 'crashed (signal %d)' % -code if code < 0 else 'stopped with an error (exit code %d)' % code
        msg = 'The game %s.' % what
        if why:
            msg += '\n\nWhat it said last:\n' + (why if len(why) < 500 else why[:500] + ' ...')
        msg += '\n\nMake a report now? It collects the logs to send to whoever helps you (the game\'s log alone: ' \
               'build-darwin/play/stdout.txt).'
        if messagebox.askyesno('B2recomp', msg, icon='warning', parent=self.root):
            self.make_report()

    def make_report(self):
        sh = os.path.join(ROOT, 'scripts', 'report.sh')
        out = os.path.join(ROOT, 'b2recomp-report.tar.gz')
        if not is_file(sh):
            messagebox.showerror('B2recomp', 'scripts/report.sh is missing from this folder.', parent=self.root)
            return
        try:
            r = subprocess.run(['sh', sh], cwd=ROOT, capture_output=True, text=True, timeout=120)
            text = r.stdout + r.stderr
        except (OSError, subprocess.TimeoutExpired) as e:
            r, text = None, str(e)
        if not r or r.returncode or not is_file(out):
            messagebox.showerror('B2recomp', 'The report could not be made:\n' + text[-600:], parent=self.root)
            return
        self.set_status('Report made: b2recomp-report.tar.gz in the B2recomp folder.')
        if messagebox.askyesno('B2recomp', 'Report made:\n%s\n\nIt holds the logs and this PC\'s details, nothing of the game. '
                               'Send that file to whoever helps you, and say what happened.\n\nOpen its folder?' % out,
                               parent=self.root):
            open_folder(ROOT)

    def ensure_menu(self):
        """The app-menu entry: made at the first start (players then open B2recomp from their app launcher), given the
        game's icon after the build, kept pointing at this folder."""
        why = menu_entry_needed()
        if not why:
            return
        try:
            add_to_menu()
            launcher_state({'menu_added': True})
        except OSError as e:
            print('B2recomp: the app-menu entry could not be written (%s)' % e)
            return
        if why == 'added' and not self.status.cget('text'):
            self.set_status('B2recomp is now in your app menu (under Games): next time, start it from there.')

    def on_close(self):
        if self.brun:
            if not messagebox.askyesno('B2recomp', 'Stop the build? Press Build again later: finished steps are kept.', parent=self.root):
                return
            self.brun.stop()
        if self.mwin and self.mwin.run:
            if not messagebox.askyesno('B2recomp', 'Stop the rebuild? Press Apply again later: the game must be rebuilt before you play.',
                                       parent=self.root):
                return
            self.mwin.run.stop()
        self.root.destroy()

    # ---------------------------------------------------------------- build screen (launcher.c "build screen")
    STEPS = ['Check the bundled tools', 'Check your dump', 'Convert the program files', 'Apply your code mods (if any)',
             'Generate tables', 'Translate the game code', 'Compile it (the longest step)', 'Make the icon', 'Link the game',
             'Self-test']
    WEIGHT = [1, 2, 1, 1, 2, 11, 240, 1, 8, 2]           # seconds on a 16-thread PC (finding 102)
    COMPILE = 6

    def create_build_controls(self):
        p = BPANEL
        lx, w = p[0] + 16, p[2] - p[0] - 32
        self.label('B2recomp builds the game on this PC from your own extracted copy of Bayonetta 2, version 1.2 (update '
                   'v131072). Nothing of the game is downloaded; it takes about 5 minutes on a fast PC.', lx, p[1] + 46, w, 36)
        self.label('Dump folder', lx, p[1] + 96, 100)
        self.bpath = tk.StringVar()
        e = tk.Entry(self.cv, textvariable=self.bpath, bg=C_FIELD, fg=C_TEXT, insertbackground=C_TEXT, relief='flat',
                     highlightthickness=1, highlightbackground='#4a5680', highlightcolor=C_ACCENT, font=self.font,
                     disabledbackground=C_PANEL, disabledforeground=C_DIM)
        self.bentry = self.place(e, lx + 100, p[1] + 92, w - 100 - 108, 24)
        Tip(self, e, 'The folder you extracted from your own console and copy: it holds exefs/ (main, sdk, subsdk0, rtld, main.npdm) '
                     'and romfs/, optionally sysdata/fonts/ and control/. Encrypted packages are not accepted.')
        self.bbrowse = self.button('Browse...', p[2] - 16 - 100, p[1] + 91, 100, 26, self.browse)
        self.bcheck = self.label('', lx + 100, p[1] + 122, w - 100, 20, color=C_NOTE)
        y = p[1] + 160
        self.bstate = [0] * len(self.STEPS)
        self.bstep = [self.label('', lx, y + i * 22, 270, 20) for i in range(len(self.STEPS))]
        fr = tk.Frame(self.cv, bg=C_EDGE, bd=0)
        self.place(fr, lx + 280, y - 4, w - 280, 220)
        self.blog = tk.Text(fr, bg=C_FIELD, fg=C_TEXT, font=self.small, relief='flat', bd=0, highlightthickness=0, wrap='char',
                            insertbackground=C_TEXT, padx=self.S(4), pady=self.S(2))
        sb = ttk.Scrollbar(fr, orient='vertical', command=self.blog.yview, style='Dark.Vertical.TScrollbar')
        self.blog.configure(yscrollcommand=sb.set, state='disabled')
        sb.pack(side='right', fill='y', padx=(0, 1), pady=1)
        self.blog.pack(side='left', fill='both', expand=True, padx=(1, 0), pady=1)
        self.bprog = tk.Canvas(self.cv, bg=C_FIELD, highlightthickness=0, bd=0)
        self.place(self.bprog, lx, p[3] - 30, w, 12)
        self.bprog_w = self.S(w)
        by, px = p[3] + 12, WIN_W - 14 - 150
        self.status = self.label('', 16, by + 3, px - 12 - 3 * 90 - 2 * 8 - 16 - 16, 36, color=C_NOTE, font=self.small, bg=C_BG)
        self.button('Report...', px - 12 - 3 * 90 - 2 * 8, by + 3, 90, 34, self.make_report,
                    'Makes b2recomp-report.tar.gz in the B2recomp folder: the logs and this PC\'s details, nothing of the game. '
                    'Send it to whoever helps you.')
        self.button('Logs folder', px - 12 - 2 * 90 - 8, by + 3, 90, 34, lambda: open_folder(os.path.join(BUILD, 'logs')),
                    'Opens build/logs: one log per step (setup.py).')
        self.button('Close', px - 12 - 90, by + 3, 90, 34, self.on_close)
        self.bbtn = GoldButton(self, 'Build', self.build_start)
        self.place(self.bbtn, px, by, 150, 40)
        for i in range(len(self.STEPS)):
            self.step_show(i)
        self.bok = False
        self.bcompile = 0
        self.prefill_dump()
        self.bpath.trace_add('write', lambda *a: self.dump_check())
        self.dump_check()
        self.set_status('B2recomp was updated: press Build to bring your game up to date (a minute or two; settings and '
                        'saves stay).' if self.update and self.python else
                        'Choose your dump folder, then press Build.' if self.python else
                        'Python was not found: the builder package brings its own (deps/python).')

    def prefill_dump(self):                               # the dump folder of an earlier run (build/setup_state.json)
        try:
            with open(os.path.join(BUILD, 'setup_state.json')) as f:
                d = json.load(f).get('dump')
            if d:
                self.bpath.set(d)
        except (OSError, ValueError, AttributeError):
            pass

    def dump_check(self):                                 # does the folder in the box look like an extracted dump?
        d = self.bpath.get().strip().strip('"\'')
        if d.startswith('file://'):
            d = d[7:]
        d = os.path.expanduser(d.rstrip('/') or d)
        self.dump = d
        self.bok = False
        if not d:
            msg = 'Paste or type the folder that holds exefs/ and romfs/, or choose it with Browse.'
        elif not os.path.isdir(d):
            msg = 'This folder does not exist.'
        elif is_file(os.path.join(d, 'main.npdm')):
            msg = 'This is the exefs folder itself: choose the folder above it, the one that holds exefs/ and romfs/.'
        elif is_file(os.path.join(d, 'data000.cpk')):
            msg = 'This is the romfs folder itself: choose the folder above it, the one that holds exefs/ and romfs/.'
        elif not is_file(os.path.join(d, 'exefs', 'main')):
            msg = 'No exefs/main here: choose the folder that holds exefs/ and romfs/.'
        elif not os.path.isdir(os.path.join(d, 'romfs')):
            msg = 'No romfs/ folder here: the dump needs both exefs/ and romfs/.'
        else:
            self.bok = True
            msg = '✓  exefs/ and romfs/ found. Press Build: the exact game version is checked first.'
        self.bcheck.configure(text=msg, fg=C_VALUE if self.bok else C_NOTE)
        self.bbtn.enable(self.bok and not self.brun)

    def browse(self):
        start = self.dump if os.path.isdir(getattr(self, 'dump', '') or '') else os.path.expanduser('~')
        title = 'Your dump folder (the one that holds exefs and romfs)'
        d = None
        for argv in (['kdialog', '--getexistingdirectory', start, '--title', title],
                     ['zenity', '--file-selection', '--directory', '--filename=' + start + '/', '--title=' + title]):
            if shutil.which(argv[0]):                     # the desktop's own folder dialog (KDE on the Steam Deck, GNOME ...)
                try:
                    r = subprocess.run(argv, capture_output=True, text=True)
                    d = r.stdout.strip() if r.returncode == 0 else ''
                    break
                except OSError:
                    d = None
        if d is None:
            d = filedialog.askdirectory(parent=self.root, initialdir=start, title=title, mustexist=True)
        if d:
            self.bpath.set(d)

    def step_show(self, i):
        glyph = ['○', '▶', '✓', '✗'][self.bstate[i]]   # waiting, running, done, failed
        color = [C_DIM, C_VALUE, C_TEXT, C_ERR][self.bstate[i]]
        self.bstep[i].configure(text='%s   %s' % (glyph, self.STEPS[i]), fg=color)

    def progress_update(self):
        total = sum(self.WEIGHT)
        done = sum(w for i, w in enumerate(self.WEIGHT) if self.bstate[i] == 2)
        if self.bstate[self.COMPILE] == 1:
            done += self.WEIGHT[self.COMPILE] * self.bcompile / 1000
        self.bprog.delete('all')
        self.bprog.create_rectangle(0, 0, int(self.bprog_w * done / total), self.S(12), fill=C_GOLD, width=0)

    def compile_progress(self):                           # the last "[n/m]" line of build/logs/compile.log
        if self.bstate[self.COMPILE] != 1:
            return
        try:
            with open(os.path.join(BUILD, 'logs', 'compile.log'), 'rb') as f:
                f.seek(0, 2)
                n = f.tell()
                f.seek(max(0, n - 511))
                tail = f.read().decode('utf-8', 'replace')
        except OSError:
            return
        m = re.findall(r'\n\[(\d+)/(\d+)\]', tail)
        if m:
            a, n = int(m[-1][0]), int(m[-1][1])
            if 0 < n and a <= n:
                self.bcompile = a * 1000 // n

    def log_append(self, box, s):
        box.configure(state='normal')
        if int(box.index('end-1c').split('.')[0]) > 4000:
            box.delete('1.0', '2000.0')
        box.insert('end', s + '\n')
        box.see('end')
        box.configure(state='disabled')

    def build_line(self, s):                              # one line of setup.py's output
        self.log_append(self.blog, s)
        m = re.match(r'\[(\d+)/(\d+)\](.*)', s)
        if not m or int(m.group(2)) != len(self.STEPS):
            return
        i, rest = int(m.group(1)) - 1, m.group(3)
        if not 0 <= i < len(self.STEPS):
            return
        if ': FAILED' in rest:
            self.bstate[i] = 3
        elif ': ok' in rest:
            self.bstate[i] = 2
        elif ' ...' in rest:
            self.bstate[i] = 1
            for k in range(i):                            # done in an earlier run
                if self.bstate[k] == 0:
                    self.bstate[k] = 2
                    self.step_show(k)
        else:
            return
        self.step_show(i)
        self.progress_update()

    def setup_env(self):
        env = dict(os.environ)
        if self.bundled:                                  # only the bundled tools: nothing of this PC's setup may leak in
            for k in ('PYTHONHOME', 'PYTHONPATH', 'PYTHONSTARTUP', 'ZIG', 'CLANG', 'SDL3_ROOT', 'SDL3_LINUX_LIB', 'VULKAN_HEADERS',
                      'GLSLANG_SO', 'GLSLANG_LINUX_ROOT', 'TOOLCHAIN'):
                env.pop(k, None)
            env['PYTHONNOUSERSITE'] = '1'
        env['PYTHONIOENCODING'] = 'utf-8'
        env['PYTHONUNBUFFERED'] = '1'
        return env

    def start_setup(self, args):
        if not self.python:
            messagebox.showerror('B2recomp', 'Python was not found. The builder package brings its own in deps/python; from the '
                                 'source, install Python 3.10+ and start this again.', parent=self.root)
            return None
        try:
            return Runner([self.python, '-u', os.path.join(ROOT, 'scripts', 'setup.py')] + args, ROOT, self.setup_env())
        except OSError as e:
            messagebox.showerror('B2recomp', 'Could not start scripts/setup.py (%s).' % e, parent=self.root)
            return None

    def build_start(self):
        self.dump_check()
        if not self.bok or self.brun:
            return
        self.brun = self.start_setup(['--dump', self.dump])
        if not self.brun:
            return
        self.bstate = [0] * len(self.STEPS)
        for i in range(len(self.STEPS)):
            self.step_show(i)
        self.bcompile = 0
        self.progress_update()
        self.blog.configure(state='normal')
        self.blog.delete('1.0', 'end')
        self.blog.configure(state='disabled')
        self.build_enable(False)
        self.bt0 = time.time()

    def build_enable(self, on):
        self.bentry.configure(state='normal' if on else 'disabled')
        self.bbrowse.state(['!disabled'] if on else ['disabled'])
        self.bbtn.set_text('Build' if on else 'Building')
        self.bbtn.enable(on and self.bok)

    def build_poll(self):
        for kind, v in self.brun.poll():
            if kind == 'line':
                self.build_line(v)
            else:
                self.build_done(v)
                return
        self.compile_progress()
        self.progress_update()
        s = int(time.time() - self.bt0)
        if self.bstate[self.COMPILE] == 1:
            self.status.configure(text='Building... %d:%02d   (compiling: %d %%)' % (s // 60, s % 60, self.bcompile // 10))
        else:
            self.status.configure(text='Building... %d:%02d' % (s // 60, s % 60))

    def build_done(self, code):
        self.brun = None
        s = int(time.time() - self.bt0)
        if code == 0 and game_built():                   # the window becomes the launcher
            for w in self.widgets:
                w.destroy()
            self.widgets = []
            self.mode = 0
            self.root.title('B2recomp Launcher')
            self.paint()
            self.create_controls()
            self.load_all()
            self.set_icon(self.root)
            self.set_status('Built in %d min %02d s. Ready: press Play.' % (s // 60, s % 60))
            self.play_btn.focus_set()
            self.root.after(400, self.ensure_menu)          # the entry gets the game's icon the build has made
            return
        for i in range(len(self.STEPS)):
            if self.bstate[i] == 1:
                self.bstate[i] = 3
                self.step_show(i)
        self.set_status('The build stopped after %d min %02d s: the log says why. Fix it and press Build again (finished steps '
                        'are kept).' % (s // 60, s % 60))
        self.build_enable(True)

    # ---------------------------------------------------------------- other windows
    def keys_window(self):
        if self.kwin and self.kwin.top.winfo_exists():
            self.kwin.top.lift()
            return
        self.kwin = KeysWindow(self)

    def mods_window(self):
        if self.mwin and self.mwin.top.winfo_exists():
            self.mwin.top.lift()
            return
        self.mwin = ModsWindow(self)


class Tip:
    """A tooltip: the text after half a second over the widget, in the launcher's colours."""

    def __init__(self, app, w, text):
        self.app, self.w, self.text, self.tw, self.job = app, w, text, None, None
        w.bind('<Enter>', self.enter, add='+')
        w.bind('<Leave>', self.leave, add='+')
        w.bind('<ButtonPress>', self.leave, add='+')

    def enter(self, e):
        self.job = self.w.after(500, self.show)

    def leave(self, e=None):
        if self.job:
            self.w.after_cancel(self.job)
            self.job = None
        if self.tw:
            self.tw.destroy()
            self.tw = None

    def show(self):
        self.job = None
        if not self.w.winfo_exists():
            return
        self.tw = tk.Toplevel(self.w)
        self.tw.wm_overrideredirect(True)
        self.tw.configure(bg=C_EDGE)
        tk.Label(self.tw, text=self.text, bg=C_FIELD, fg=C_TEXT, font=self.app.small, justify='left',
                 wraplength=self.app.S(360), padx=self.app.S(6), pady=self.app.S(4)).pack(padx=1, pady=1)
        x = self.w.winfo_rootx() + self.app.S(8)
        y = self.w.winfo_rooty() + self.w.winfo_height() + self.app.S(4)
        self.tw.wm_geometry('+%d+%d' % (x, y))


class Slider(tk.Canvas):
    """A trackbar as on Windows: a thin track filled up to the thumb, tick marks below, a blue thumb; positions 0..n.
    command(pos) runs when the user moves it (not on set()); keys: arrows one step, Page Up / Down five, Home / End."""

    def __init__(self, app, parent, n, ticks, command, bg=C_PANEL):
        super().__init__(parent, bg=bg, highlightthickness=0, bd=0, takefocus=1)
        self.app, self.n, self.ticks, self.command = app, max(1, n), ticks, command
        self.pos, self.on = 0, True
        self.bind('<Configure>', lambda e: self.draw())
        self.bind('<ButtonPress-1>', lambda e: (self.focus_set(), self._move(self._at(e.x))))
        self.bind('<B1-Motion>', lambda e: self._move(self._at(e.x)))
        for key, d in (('<Left>', -1), ('<Down>', -1), ('<Right>', 1), ('<Up>', 1), ('<Prior>', 5), ('<Next>', -5)):
            self.bind(key, lambda e, d=d: self._move(self.pos + d))
        self.bind('<Home>', lambda e: self._move(0))
        self.bind('<End>', lambda e: self._move(self.n))
        self.bind('<FocusIn>', lambda e: self.draw())
        self.bind('<FocusOut>', lambda e: self.draw())

    def _span(self):
        m = self.app.S(6)
        return m, max(1, self.winfo_width() - 2 * m)

    def _at(self, x):
        m, w = self._span()
        return int(round((x - m) / w * self.n))

    def _move(self, pos):
        if not self.on:
            return
        pos = min(max(pos, 0), self.n)
        if pos != self.pos:
            self.pos = pos
            self.draw()
            self.command(pos)

    def set(self, pos):
        self.pos = min(max(int(pos), 0), self.n)
        self.draw()

    def get(self):
        return self.pos

    def state(self, spec):
        self.on = '!disabled' in spec
        self.configure(takefocus=1 if self.on else 0)
        self.draw()

    def draw(self):
        self.delete('all')
        S = self.app.S
        m, w = self._span()
        cy = S(9)
        x = m + w * self.pos / self.n
        fill = C_ACCENT if self.on else C_DIM
        self.create_rectangle(m, cy - S(2), m + w, cy + S(2), fill='#3a4468' if self.on else C_EDGE, width=0)
        self.create_rectangle(m, cy - S(2), x, cy + S(2), fill=fill, width=0)
        if self.ticks:
            for t in range(0, self.n + 1, self.ticks):
                tx = m + w * t / self.n
                self.create_line(tx, cy + S(9), tx, cy + S(13), fill=C_DIM)
        r = S(4)
        self.create_rectangle(x - r, cy - S(9), x + r, cy + S(9), fill=fill if self.on else '#4a5270',
                              outline='#9cc3f0' if self.on and self.focus_get() is self else fill)


class CheckBox(tk.Canvas):
    """A check box as Windows 11 draws it in dark mode: a rounded square, blue with a white tick when on."""

    def __init__(self, app, parent, variable, command=None, bg=C_PANEL):
        n = app.S(18)
        super().__init__(parent, width=n, height=n, bg=bg, highlightthickness=0, bd=0, takefocus=1, cursor='hand2')
        self.app, self.var, self.command, self.on = app, variable, command, True
        self.bind('<ButtonRelease-1>', lambda e: self.invoke())
        self.bind('<space>', lambda e: self.invoke())
        self.bind('<Configure>', lambda e: self.draw())
        self.bind('<FocusIn>', lambda e: self.draw())
        self.bind('<FocusOut>', lambda e: self.draw())
        variable.trace_add('write', lambda *a: self.draw())

    def invoke(self):
        if not self.on:
            return
        self.var.set(0 if self.var.get() else 1)
        if self.command:
            self.command()

    def state(self, spec):
        self.on = '!disabled' in spec
        self.configure(cursor='hand2' if self.on else 'arrow', takefocus=1 if self.on else 0)
        self.draw()

    def draw(self):
        self.delete('all')
        n = int(self['width'])
        checked = bool(self.var.get())
        fill = (C_ACCENT if self.on else C_DIM) if checked else C_FIELD
        edge = fill if checked else ('#8a96b8' if self.on else C_EDGE)
        if self.on and self.focus_get() is self:
            edge = '#9cc3f0'
        self.create_rectangle(1, 1, n - 2, n - 2, fill=fill, outline=edge)
        if checked:
            pts = [n * 0.24, n * 0.52, n * 0.43, n * 0.70, n * 0.78, n * 0.30]
            self.create_line(*pts, fill='#ffffff', width=max(2, self.app.S(2)), capstyle='round', joinstyle='round')


class GoldButton(tk.Canvas):
    """Play / Build: a gold gradient, rounded, the text in capitals with wide spacing (launcher.c draw_play)."""

    def __init__(self, app, text, cmd):
        super().__init__(app.cv, bg=C_BG, highlightthickness=0, bd=0, takefocus=1, cursor='hand2')
        self.app, self.text, self.cmd = app, text, cmd
        self.hot = self.down = False
        self.on = True
        for ev, fn in (('<Enter>', lambda e: self._set(hot=True)), ('<Leave>', lambda e: self._set(hot=False, down=False)),
                       ('<ButtonPress-1>', lambda e: self._set(down=True)), ('<ButtonRelease-1>', self._release),
                       ('<Configure>', lambda e: self.draw()), ('<FocusIn>', lambda e: self.draw()),
                       ('<FocusOut>', lambda e: self.draw()), ('<space>', lambda e: self.invoke())):
            self.bind(ev, fn)

    def _set(self, **kw):
        for k, v in kw.items():
            setattr(self, k, v)
        self.draw()

    def _release(self, e):
        inside = 0 <= e.x < self.winfo_width() and 0 <= e.y < self.winfo_height()
        was = self.down
        self._set(down=False)
        if was and inside:
            self.invoke()

    def invoke(self):
        if self.on:
            self.cmd()

    def enable(self, on):
        self.on = bool(on)
        self.configure(cursor='hand2' if self.on else 'arrow', takefocus=1 if self.on else 0)
        self.draw()

    def set_text(self, t):
        self.text = t
        self.draw()

    def draw(self):
        self.delete('all')
        W, H = self.winfo_width(), self.winfo_height()
        if W < 4 or H < 4:
            return
        if not self.on:
            a, b = (52, 56, 70), (40, 43, 56)
        elif self.down:
            a, b = (196, 140, 52), (150, 100, 30)
        elif self.hot:
            a, b = (255, 226, 150), (214, 156, 62)
        else:
            a, b = (244, 208, 124), (194, 136, 48)
        r = self.app.S(5)
        for y in range(H):
            t = y / max(1, H - 1)
            c = '#%02x%02x%02x' % tuple(int(a[i] + (b[i] - a[i]) * t) for i in range(3))
            dy = r - y if y < r else y - (H - 1 - r) if y > H - 1 - r else 0
            inset = r - int((r * r - dy * dy) ** 0.5) if dy > 0 else 0
            self.create_line(inset, y, W - inset, y, fill=c)
        edge = C_EDGE if not self.on else '#784e16'
        pts = [r, 0, W - 1 - r, 0, W - 1, 0, W - 1, r, W - 1, H - 1 - r, W - 1, H - 1, W - 1 - r, H - 1, r, H - 1, 0, H - 1,
               0, H - 1 - r, 0, r, 0, 0]
        self.create_polygon(pts, smooth=True, fill='', outline=edge)
        font, extra = self.app.big, self.app.S(4)
        t = self.text.upper()
        w = sum(font.measure(ch) + extra for ch in t) - extra
        x = (W - w) // 2
        fg = C_DIM if not self.on else '#1e1204'
        for ch in t:
            self.create_text(x, H // 2, text=ch, font=font, fill=fg, anchor='w')
            x += font.measure(ch) + extra
        if self.on and self.focus_get() is self:
            i = self.app.S(4)
            self.create_rectangle(i, i, W - 1 - i, H - 1 - i, outline='#5a3a10', dash=(1, 2))


class Dialog:
    """A window of the launcher's own (keyboard, mods, controller tools): dark, centred on the launcher, modal while open."""

    def __init__(self, app, title, w, h, modal=True):
        self.app = app
        self.top = tk.Toplevel(app.root, bg=C_PANEL)
        self.top.title(title)
        self.top.resizable(False, False)
        W, H = app.S(w), app.S(h)
        rx, ry = app.root.winfo_rootx(), app.root.winfo_rooty()
        x = max(0, rx + (app.S(WIN_W) - W) // 2)
        y = max(0, ry + (app.S(WIN_H) - H) // 2)
        self.top.geometry('%dx%d+%d+%d' % (W, H, x, y))
        self.top.transient(app.root)
        self.modal = modal
        if modal:
            self.top.after(50, self._grab)
        self.top.protocol('WM_DELETE_WINDOW', self.close)

    def _grab(self):
        try:
            self.top.grab_set()
        except tk.TclError:
            pass

    def lab(self, text, x, y, w, h=20, color=C_TEXT, font=None, anchor='w'):
        lb = tk.Label(self.top, text=text, bg=C_PANEL, fg=color, font=font or self.app.font, anchor=anchor, justify='left',
                      wraplength=self.app.S(w), padx=0, pady=0, bd=0)
        lb.place(x=self.app.S(x), y=self.app.S(y), width=self.app.S(w), height=self.app.S(h))
        return lb

    def textbox(self, x, y, w, h):
        fr = tk.Frame(self.top, bg=C_EDGE, bd=0)
        fr.place(x=self.app.S(x), y=self.app.S(y), width=self.app.S(w), height=self.app.S(h))
        t = tk.Text(fr, bg=C_FIELD, fg=C_TEXT, font=self.app.small, relief='flat', bd=0, highlightthickness=0, wrap='char',
                    padx=self.app.S(4), pady=self.app.S(2))
        sb = ttk.Scrollbar(fr, orient='vertical', command=t.yview, style='Dark.Vertical.TScrollbar')
        t.configure(yscrollcommand=sb.set, state='disabled')
        sb.pack(side='right', fill='y', padx=(0, 1), pady=1)
        t.pack(side='left', fill='both', expand=True, padx=(1, 0), pady=1)
        return t

    def close(self):
        try:
            self.top.grab_release()
        except tk.TclError:
            pass
        self.top.destroy()
        self.app.root.focus_force()


class ToolWindow(Dialog):
    """--pad-map / --pad-test: their output in a window (the console window of the Windows launcher); Close stops them."""

    def __init__(self, app, title, argv):
        super().__init__(app, title, 640, 420, modal=False)
        self.box = self.textbox(16, 16, 608, 340)
        self.note = self.lab('', 16, 370, 400, 34, color=C_NOTE, font=app.small)
        app.button('Close', 524, 372, 100, 32, self.close, parent=self.top)
        env = game_env()
        env['SSBU_FOCUS_NOTIFY'] = '0'
        try:
            self.run = Runner(argv, OUT, env)
            self.note.configure(text='Running: ' + ' '.join(os.path.basename(a) for a in argv))
        except OSError as e:
            self.run = None
            self.note.configure(text='Could not start the controller tool (%s).' % e)
        self.poll()

    def poll(self):
        if not self.top.winfo_exists() or not self.run:
            return
        for kind, v in self.run.poll():
            if kind == 'line':
                self.app.log_append(self.box, v)
            else:
                self.app.log_append(self.box, '')
                self.note.configure(text='Finished (exit %d). Close the window.' % v)
                self.run = None
                return
        self.top.after(100, self.poll)

    def close(self):
        if self.run:
            self.run.stop()
        super().close()


# ================================================================ keyboard and mouse window (launcher.c keys_window)
KROWS = [('LSTICK_UP', 'Move forward', 'W'), ('LSTICK_DOWN', 'Move back', 'S'), ('LSTICK_LEFT', 'Move left', 'A'),
         ('LSTICK_RIGHT', 'Move right', 'D'), ('X', 'Punch  (X)', 'MOUSE1'), ('A', 'Kick  (A)', 'MOUSE2, ENTER'),
         ('B', 'Jump  (B)', 'SPACE, BACKSPACE'), ('Y', 'Shoot  (Y)', 'E, MOUSE3'), ('ZR', 'Dodge  (ZR)', 'LSHIFT'),
         ('ZL', 'Lock-on  (ZL)', 'LALT'), ('L', 'Umbran Climax  (L)', 'Q'), ('R', 'R button', 'F'),
         ('RSTICK_UP', 'Camera up', 'UP'), ('RSTICK_DOWN', 'Camera down', 'DOWN'), ('RSTICK_LEFT', 'Camera left', 'LEFT'),
         ('RSTICK_RIGHT', 'Camera right', 'RIGHT'), ('STICK_R', 'Camera reset  (R stick)', 'R'), ('STICK_L', 'Left stick click', 'C'),
         ('PLUS', 'Pause  (+)', 'ESC'), ('MINUS', 'Minus  (-)', 'TAB'), ('UP', 'D-pad up', '1'), ('RIGHT', 'D-pad right', '2'),
         ('DOWN', 'D-pad down', '3'), ('LEFT', 'D-pad left', '4')]
KNAMES = [('MOUSE1', 'Left mouse'), ('MOUSE2', 'Right mouse'), ('MOUSE3', 'Middle mouse'), ('MOUSE4', 'Mouse 4'),
          ('MOUSE5', 'Mouse 5'), ('LEFT', 'Left arrow'), ('UP', 'Up arrow'), ('RIGHT', 'Right arrow'), ('DOWN', 'Down arrow'),
          ('ENTER', 'Enter'), ('BACKSPACE', 'Backspace'), ('SPACE', 'Space'), ('TAB', 'Tab'), ('ESC', 'Esc'), ('SHIFT', 'Shift'),
          ('CTRL', 'Ctrl'), ('ALT', 'Alt'), ('LSHIFT', 'Left Shift'), ('RSHIFT', 'Right Shift'), ('LCTRL', 'Left Ctrl'),
          ('RCTRL', 'Right Ctrl'), ('LALT', 'Left Alt'), ('RALT', 'Right Alt')]
SHOW = dict(KNAMES)
TK_KEYS = {'Return': 'ENTER', 'KP_Enter': 'ENTER', 'BackSpace': 'BACKSPACE', 'space': 'SPACE', 'Tab': 'TAB',
           'ISO_Left_Tab': 'TAB', 'Escape': 'ESC', 'Shift_L': 'LSHIFT', 'Shift_R': 'RSHIFT', 'Control_L': 'LCTRL',
           'Control_R': 'RCTRL', 'Alt_L': 'LALT', 'Alt_R': 'RALT', 'ISO_Level3_Shift': 'RALT', 'Meta_L': 'LALT', 'Meta_R': 'RALT',
           'Up': 'UP', 'Down': 'DOWN', 'Left': 'LEFT', 'Right': 'RIGHT',
           'KP_Insert': 'NUM0', 'KP_End': 'NUM1', 'KP_Down': 'NUM2', 'KP_Next': 'NUM3', 'KP_Left': 'NUM4', 'KP_Begin': 'NUM5',
           'KP_Right': 'NUM6', 'KP_Home': 'NUM7', 'KP_Up': 'NUM8', 'KP_Prior': 'NUM9',
           'semicolon': '0xBA', 'equal': '0xBB', 'comma': '0xBC', 'minus': '0xBD', 'period': '0xBE', 'slash': '0xBF',
           'grave': '0xC0', 'bracketleft': '0xDB', 'backslash': '0xDC', 'bracketright': '0xDD', 'apostrophe': '0xDE'}
MOUSE_BUTTONS = {1: 'MOUSE1', 3: 'MOUSE2', 2: 'MOUSE3', 8: 'MOUSE4', 9: 'MOUSE5'}   # X11 button numbers


def key_canon(t):
    """One key name as nnpad_sdl.c parse_vk1 reads it -> the canonical file name ('' = none, None = not readable)."""
    b = (t or '').strip(' \t\r')
    if not b or b.lower() == 'none':
        return ''
    if len(b) == 1:
        return b.upper()
    for n, _ in KNAMES:
        if b.lower() == n.lower():
            return n
    m = re.fullmatch(r'[Ff](\d+)', b)
    if m and 1 <= int(m.group(1)) <= 12:
        return 'F%d' % int(m.group(1))
    m = re.match(r'(?i)NUM(\d)', b)
    if m:
        return 'NUM' + m.group(1)
    if re.fullmatch(r'0[xX][0-9A-Fa-f]+', b):
        return '0x%02X' % int(b, 16)
    return ''


def key_show(n):
    if not n:
        return '-'
    if n in SHOW:
        return SHOW[n]
    m = re.fullmatch(r'NUM(\d)', n)
    return 'Num %s' % m.group(1) if m else n


class KeysWindow(Dialog):
    W, H = 900, 536

    def __init__(self, app):
        super().__init__(app, 'Keyboard and mouse', self.W, self.H)
        self.cap = -1
        self.lab('Two keys or mouse buttons per action. The layout follows the original Bayonetta on PC.', 20, 14, self.W - 40)
        self.slot = []
        for r, (_, label, _) in enumerate(KROWS):
            col, row = divmod(r, 12)
            x, y = 20 + col * 440, 46 + row * 32
            self.lab(label, x, y + 5, 170)
            pair = []
            for k in range(2):
                b = ttk.Button(self.top, text='', style='Slot.TButton', command=lambda i=r * 2 + k: self.start_cap(i))
                b.place(x=app.S(x + 172 + k * 126), y=app.S(y), width=app.S(120), height=app.S(28))
                pair.append(b)
            self.slot.append(pair)
        y = 46 + 12 * 32 + 14
        self.mouse = tk.IntVar()
        self.inv = tk.IntVar()
        CheckBox(app, self.top, self.mouse).place(x=app.S(20), y=app.S(y + 3))
        self.lab('The mouse moves the camera', 44, y + 4, 220)
        self.lab('Mouse sensitivity', 300, y + 4, 130)
        self.sens_scale = Slider(app, self.top, 49, 5, lambda pos: self.sens_moved(pos))   # 0.1 .. 5.0 (positions 1..50)
        self.sens_scale.place(x=app.S(430), y=app.S(y), width=app.S(200), height=app.S(26))
        self.sensv = self.lab('', 636, y + 4, 40, color=C_VALUE)
        CheckBox(app, self.top, self.inv).place(x=app.S(690), y=app.S(y + 3))
        self.lab('Invert vertical', 714, y + 4, 160)
        y += 40
        self.note = self.lab('Click a slot, then press a key or a mouse button. Delete clears a slot.', 20, y + 8, 470, 34,
                             color=C_NOTE, font=app.small)
        app.button('Reset to defaults', 500, y, 150, 34, self.defaults, parent=self.top)
        app.button('Save', 660, y, 100, 34, self.save, parent=self.top)
        app.button('Cancel', 770, y, 100, 34, self.close, parent=self.top)
        for w in [self.top] + self.top.winfo_children():  # the capture sees keys and clicks before the buttons do
            w.bindtags(('KCap',) + tuple(t for t in w.bindtags() if t != 'KCap'))
        self.top.bind_class('KCap', '<KeyPress>', self.on_key)
        self.top.bind_class('KCap', '<ButtonPress>', self.on_button)
        self.top.bind_class('KCap', '<ButtonRelease>', lambda e: 'break' if self.cap >= 0 or self._eat else None)
        self.top.bind('<Escape>', lambda e: self.close() if self.cap < 0 else None)
        self._eat = False
        self.load(False)

    def sens_moved(self, pos):                            # slider position 0..49 = sensitivity 1..50 tenths
        self.sensv.configure(text='%.1f' % ((pos + 1) / 10.0))

    def sens_value(self):
        return self.sens_scale.get() + 1

    def show_all(self):
        for r in range(len(KROWS)):
            for k in range(2):
                b = self.slot[r][k]
                if self.cap == r * 2 + k:
                    b.configure(text='press a key...', style='Wait.TButton')
                else:
                    b.configure(text=key_show(self.kv[r][k]), style='Slot.TButton')

    def load(self, defaults):
        f = Ini(INI[F_PAD])
        self.kv = []
        for key, _, d in KROWS:
            v = None if defaults else f.get('keyboard', key, True)
            parts = (v if v is not None else d).split(',', 1)
            self.kv.append([key_canon(parts[0]), key_canon(parts[1]) if len(parts) > 1 else ''])
        cam, inv, sens = 1, 0, 10
        if not defaults:
            v = f.get('keyboard', 'mouse_camera', True)
            if v is not None:
                cam = 0 if v.lower() in ('off', '0', 'no') else 1
            v = f.get('keyboard', 'mouse_invert_y', True)
            if v is not None:
                inv = 1 if v.lower() in ('on', '1', 'yes') else 0
            v = f.get('keyboard', 'mouse_sensitivity', True)
            if v is not None and to_float(v) > 0:
                sens = int(to_float(v) * 10 + 0.5)
            self.kv0 = [list(p) for p in self.kv]
            self.mouse0, self.inv0, self.sens0 = cam, inv, sens
        self.mouse.set(cam)
        self.inv.set(inv)
        sens = min(max(sens, 1), 50)
        self.sens_scale.set(sens - 1)
        self.sens_moved(sens - 1)
        self.cap = -1
        self.show_all()

    def defaults(self):
        self.load(True)
        self.note.configure(text='Defaults restored: Save keeps them.')

    def start_cap(self, i):
        self.cap = i
        self.show_all()
        self.note.configure(text='Press a key or click a mouse button for this slot (Delete clears it).')
        self.top.focus_set()

    def assign(self, name):                               # the slot waiting for a key gets name ('' = cleared)
        r, k = divmod(self.cap, 2)
        t = ''
        if name:
            for r2 in range(len(KROWS)):
                for k2 in range(2):
                    if (r2, k2) != (r, k) and self.kv[r2][k2] == name:
                        t = '%s moved here from %s.' % (key_show(name), KROWS[r2][1])
                        self.kv[r2][k2] = ''
        self.kv[r][k] = name
        self.cap = -1
        self.show_all()
        self.note.configure(text=t or 'Click a slot, then press a key or a mouse button. Delete clears a slot.')

    def on_key(self, e):
        if self.cap < 0:
            return None
        if e.keysym in ('Delete', 'KP_Delete'):
            self.assign('')
            return 'break'
        n = TK_KEYS.get(e.keysym)
        if n is None and len(e.keysym) == 1 and e.keysym.isalnum():
            n = e.keysym.upper()
        if n is None:
            m = re.fullmatch(r'F(\d+)', e.keysym) or re.fullmatch(r'KP_(\d)', e.keysym)
            if m and e.keysym.startswith('F') and 1 <= int(m.group(1)) <= 12:
                n = 'F%d' % int(m.group(1))
            elif m and e.keysym.startswith('KP_'):
                n = 'NUM' + m.group(1)
        if n is None:
            self.note.configure(text='%s cannot be used: choose another key (Delete clears the slot).' % e.keysym)
            return 'break'
        self.assign(n)
        return 'break'

    def on_button(self, e):
        if self.cap < 0:
            return None
        n = MOUSE_BUTTONS.get(e.num)
        if n is None:                                    # the wheel (4-7)
            return 'break'
        self._eat = True                                  # its release must not press the button under the pointer
        self.top.after(300, lambda: setattr(self, '_eat', False))
        self.assign(n)
        return 'break'

    def value(self, r):                                   # a row as the file writes it: "E, MOUSE3"
        a, b = self.kv[r]
        return '%s, %s' % (a or 'none', b) if b else (a or 'none')

    def save(self):
        f = Ini(INI[F_PAD])                               # fresh from disk, as save_all
        n = 0
        for r, (key, _, _) in enumerate(KROWS):
            if self.kv[r] != self.kv0[r]:
                f.set('keyboard', key, True, self.value(r))
                n += 1
        cam, inv, sens = self.mouse.get(), self.inv.get(), self.sens_value()
        if cam != self.mouse0:
            f.set('keyboard', 'mouse_camera', True, 'on' if cam else 'off')
            n += 1
        if inv != self.inv0:
            f.set('keyboard', 'mouse_invert_y', True, 'on' if inv else 'off')
            n += 1
        if sens != self.sens0:
            f.set('keyboard', 'mouse_sensitivity', True, '%.1f' % (sens / 10.0))
            n += 1
        if n and not f.save():
            messagebox.showerror('B2recomp', 'Could not write %s.' % INI[F_PAD], parent=self.top)
            return
        if not self.app.proc:
            self.app.set_status('Keyboard and mouse: %d setting(s) saved.' % n)
        self.close()


# ================================================================ code mods window (launcher.c mods window)
MSTEP = ['checking the tools', 'checking the dump', 'converting', 'applying the mods', 'generating tables', 'translating',
         'compiling', 'icon', 'linking', 'self-test']
MODS_HEAD = [
    '; Code mod switches: which of the mods in mods/ (emulator .pchtxt / .ips files) are built into the game.',
    '; Written by the launcher\'s Mods window, read by scripts/setup.py (step mods) through scripts/pchtxt.py.',
    '; <key> = on | off. A key is a mod (the first folder below mods/, or a file lying directly in it), a file',
    '; (its path below mods/, with /), or a block of a .pchtxt (<file> :: <block title>, or <file> :: #<n>).',
    '; Not listed = the default: on (a block as its @enabled / @disabled line says). A change needs a rebuild:',
    '; the Mods window\'s Apply, or python scripts/setup.py. A mod switched off also leaves out its romfs/ files',
    '; (read by the game at start, no rebuild). [prompts]: the button-prompt mods (BUILDING.md).', '[mods]']


def mods_norm(s):                                         # pchtxt.py norm_key: '\' = '/', blanks around '::', no case
    s = s.strip(' \t\r').replace('\\', '/').lower()
    return re.sub(r'[ \t]*::[ \t]*', ' :: ', s)


class ModsWindow(Dialog):
    W, H = 760, 620

    def __init__(self, app):
        super().__init__(app, 'Mods', self.W, self.H)
        self.run = None
        self.items = []
        self.lab('The mods in the mods folder, switched on and off as in an emulator. Code patches (.pchtxt, .ips) are compiled '
                 'into the game: a change to them takes effect after Apply (rebuild), about a minute, with the game closed. '
                 'RomFS files (textures, models, sounds) are read when the game starts.', 20, 14, self.W - 40, 54)
        fr = tk.Frame(self.top, bg=C_EDGE, bd=0)
        fr.place(x=app.S(20), y=app.S(74), width=app.S(self.W - 40), height=app.S(280))
        self.tree = ttk.Treeview(fr, style='Dark.Treeview', show='tree', selectmode='browse')
        sb = ttk.Scrollbar(fr, orient='vertical', command=self.tree.yview, style='Dark.Vertical.TScrollbar')
        self.tree.configure(yscrollcommand=sb.set)
        sb.pack(side='right', fill='y', padx=(0, 1), pady=1)
        self.tree.pack(side='left', fill='both', expand=True, padx=(1, 0), pady=1)
        self.tree.tag_configure('off', foreground=C_DIM)
        self.tree.tag_configure('bad', foreground=C_ERR)
        self.img = [self._box(False), self._box(True)]
        self.tree.bind('<ButtonRelease-1>', self.click)
        self.tree.bind('<space>', self.space)
        self.tree.bind('<<TreeviewSelect>>', lambda e: self.note_show())
        self.note = self.lab('', 20, 360, self.W - 40, 52, color=C_NOTE, font=app.small)
        self.log = self.textbox(20, 416, self.W - 40, 136)
        by = 566
        self.status = self.lab('', 20, by, 300, 40, color=C_NOTE, font=app.small)
        app.button('Mods folder', 330, by, 100, 34, lambda: open_folder(MODS_DIR), parent=self.top)
        app.button('Refresh', 438, by, 84, 34, lambda: None if self.run else self.load(), parent=self.top)
        self.apply_btn = app.button('Apply (rebuild)', 530, by, 120, 34, self.apply, parent=self.top)
        app.button('Close', 658, by, 82, 34, self.close, parent=self.top)
        self.load()

    def _box(self, on):
        n = self.app.S(14)
        im = tk.PhotoImage(width=n, height=n)
        bg, edge = (C_ACCENT, C_ACCENT) if on else (C_FIELD, '#7884a8')
        im.put(edge, to=(0, 0, n, n))
        im.put(bg, to=(1, 1, n - 1, n - 1))
        if on:                                            # a check mark
            pts = [(0.22, 0.52), (0.42, 0.72), (0.80, 0.28)]
            for (x0, y0), (x1, y1) in zip(pts, pts[1:]):
                steps = n * 2
                for s in range(steps + 1):
                    x = int((x0 + (x1 - x0) * s / steps) * n)
                    y = int((y0 + (y1 - y0) * s / steps) * n)
                    im.put('#ffffff', to=(x, y, min(n, x + 2), min(n, y + 2)))
        return im

    def on_with_parents(self, i):
        while i is not None and i >= 0:
            if not self.items[i]['on']:
                return False
            i = self.items[i]['parent']
        return True

    def dirty(self):
        return any(it['on'] != it['on0'] for it in self.items)

    def dirty_code(self):                                 # a changed switch that needs a rebuild (romfs 2 = RomFS-only mod)
        return any(it['on'] != it['on0'] and it['romfs'] != 2 for it in self.items)

    def read_built(self):                                 # setup_state.json "mods": the fingerprint the game was built with
        try:
            with open(os.path.join(BUILD, 'setup_state.json')) as f:
                return json.load(f).get('mods') or 'none'
        except (OSError, ValueError, AttributeError):
            return 'none'

    def load(self):                                       # (re)reads the list; unsaved switches are kept by key
        keep = {it['key']: it['on'] for it in self.items if it['on'] != it['on0']}
        self.items, self.err, self.want = [], '', 'none'
        self.built = self.read_built()
        self.tree.delete(*self.tree.get_children())
        out = None
        if self.app.python:
            nso = os.path.join(ROOT, 'targets', 'exefs', 'main')
            argv = [self.app.python, '-I', os.path.join(ROOT, 'scripts', 'pchtxt.py'), 'tree', MODS_DIR] + \
                   ([nso] if is_file(nso) else []) + ['--ini=' + MODS_INI]
            try:
                r = subprocess.run(argv, cwd=ROOT, capture_output=True, timeout=20)
                out = r.stdout.decode('utf-8', 'replace') if r.returncode == 0 else None
            except (OSError, subprocess.TimeoutExpired):
                out = None
            if out is None:
                self.err = 'scripts/pchtxt.py tree failed: run it in a terminal to see why.'
        cur_mod = cur_file = None
        for line in (out or '').splitlines():
            f = line.rstrip('\r').split('\t')
            if f[0] == 'state' and len(f) >= 2:
                self.want = f[1]
                if len(f) >= 3 and f[2]:
                    self.err = f[2]
                continue
            kind = {'mod': 0, 'file': 1, 'block': 2}.get(f[0])
            if kind is None or len(f) < (6 if kind == 2 else 4):
                continue
            it = dict(kind=kind, key=f[1], on=int(f[2] or 0), on0=int(f[2] or 0), dflt=1, nrec=0, note='', romfs=0, parent=None)
            if kind == 2:
                it.update(dflt=int(f[3] or 0), nrec=int(f[4] or 0), label=f[5])
            else:
                it['label'] = f[3]
                if len(f) >= 5:
                    it['note'] = f[4]
            if kind == 0 and len(f) >= 6:
                it['romfs'] = 2 if f[5] == 'romfs-only' else 1 if f[5] == 'romfs' else 0
            n = len(self.items)
            if kind == 0:
                cur_mod, cur_file = n, None
            elif kind == 1:
                it['parent'] = cur_mod
                cur_file = n
            else:
                it['parent'] = cur_file if cur_file is not None else cur_mod
            if it['key'] in keep:
                it['on'] = keep[it['key']]
            text = '%s%s%s' % ('! ' if it['note'] else '', it['label'],
                               '   (@disabled in the file)' if kind == 2 and not it['dflt'] else
                               '   (RomFS files: no rebuild)' if it['romfs'] == 2 else '   (+ RomFS files)' if it['romfs'] else '')
            parent = self.items[it['parent']]['iid'] if it['parent'] is not None else ''
            it['iid'] = self.tree.insert(parent, 'end', text=' ' + text, open=True)
            self.items.append(it)
        self.paint_items()
        self.note_show()
        self.status_show()

    def paint_items(self):
        for i, it in enumerate(self.items):
            tag = 'off' if not self.on_with_parents(i) else 'bad' if it['note'] else ''
            self.tree.item(it['iid'], image=self.img[1 if it['on'] else 0], tags=(tag,) if tag else ())

    def index_of(self, iid):
        return next((i for i, it in enumerate(self.items) if it['iid'] == iid), -1)

    def toggle(self, i):
        if self.run or i < 0:
            return
        self.items[i]['on'] = 0 if self.items[i]['on'] else 1
        self.paint_items()
        self.status_show()

    def click(self, e):
        if self.tree.identify_element(e.x, e.y) == 'image':
            self.toggle(self.index_of(self.tree.identify_row(e.y)))

    def space(self, e):
        sel = self.tree.selection()
        if sel:
            self.toggle(self.index_of(sel[0]))
        return 'break'

    def note_show(self):
        sel = self.tree.selection()
        i = self.index_of(sel[0]) if sel else -1
        if i < 0:
            if not self.items:
                t = 'No mods in %s. Copy an emulator mod\'s folder there as it is (exefs/ code patches, romfs/ files), then Refresh.' % MODS_DIR
            elif self.err:
                t = self.err
            else:
                t = 'Select a mod to see its details. Grey = switched off by itself or with the mod or file above it.'
        else:
            it = self.items[i]
            what = ('block, written under @enabled' if it['dflt'] else 'block, written under @disabled (off unless switched on here)') \
                if it['kind'] == 2 else 'file' if it['kind'] == 1 else 'mod'
            lines = ', %d patch line%s' % (it['nrec'], '' if it['nrec'] == 1 else 's') if it['kind'] == 2 else ''
            rf = '\nRomFS files only (textures, models, sounds...): the game reads them when it starts, no rebuild needed.' \
                if it['romfs'] == 2 else '\nAlso has RomFS files: switching the mod off leaves them out as well (no rebuild needed for those).' \
                if it['romfs'] else ''
            t = '%s   (%s%s)%s%s%s' % (it['key'], what, lines, '\n' if it['note'] else '', it['note'], rf)
        self.note.configure(text=t)

    def status_show(self):
        if self.run:
            s = int(time.time() - self.t0)
            t = 'Rebuilding... %d:%02d   (%s)' % (s // 60, s % 60, MSTEP[self.step] if self.step >= 0 else 'starting')
        elif not self.app.python:
            t = 'Python was not found: the mod list and the rebuild need it.'
        elif self.dirty() and not self.dirty_code():
            t = 'Changed. Apply saves it: RomFS files need no rebuild (read when the game starts).'
        elif self.dirty():
            t = 'Changed. Apply (rebuild) saves the switches and builds the game with them.'
        elif self.want == 'error':
            t = 'A mod that is on cannot be used (marked with !): switch it off or remove it.'
        elif self.want != self.built:
            t = 'The game was built with other mods than these: press Apply (rebuild).'
        else:
            t = 'The game is built without code mods.' if self.want == 'none' else 'The game is built with these mods.'
        self.status.configure(text=t)
        self.apply_btn.state(['!disabled'] if not self.run and self.app.python else ['disabled'])

    def save(self):
        if not self.dirty():
            return True
        f = Ini(MODS_INI)
        if not f.exists:                                  # a new file starts with what it is
            f.lines, f.crlf = [l + '\r' for l in MODS_HEAD] + [''], True
        for it in self.items:
            if it['on'] != it['on0']:
                self.mods_set(f, it['key'], 'on' if it['on'] else 'off')
        if not f.save():
            messagebox.showerror('B2recomp', 'Could not write %s.' % MODS_INI, parent=self.top)
            return False
        for it in self.items:
            it['on0'] = it['on']
        return True

    @staticmethod
    def mods_set(f, key, val):                            # in place: the value after the last '=' of key's line in [mods]
        want, cur, last, header, done = mods_norm(key), '', -1, -1, False
        eol = '\r' if f.crlf else ''
        for i, l in enumerate(f.lines):
            s = Ini.section(l)
            if s is not None:
                cur = s
                if cur.lower() == 'mods':
                    header = last = i
                continue
            if cur.lower() != 'mods':
                continue
            if l.strip(' \t\r'):
                last = i
            p = l.lstrip(' \t')
            eq = l.rfind('=')
            if not p or p[0] in ';#' or eq < 0 or mods_norm(l[:eq]) != want:
                continue
            f.lines[i] = l[:eq + 1] + ' ' + val + eol
            done = True
        if done:
            return
        if header < 0:
            f.ensure_eol()
            at = len(f.lines) - 1
            if at > 0 and f.lines[at - 1].strip(' \t\r'):
                f.lines.insert(at, eol)
                at += 1
            f.lines.insert(at, '[mods]' + eol)
            last = at
        at = last + 1
        if at >= len(f.lines):
            f.ensure_eol()
            at = len(f.lines) - 1
        f.lines.insert(at, '%s = %s%s' % (key, val, eol))

    def apply(self):
        if self.run:
            return
        rebuild = self.dirty_code() or self.want != self.built   # else only RomFS-only mods changed
        if rebuild and ((self.app.proc and self.app.proc.poll() is None) or proc_running(GAME)):
            messagebox.showinfo('B2recomp', 'The game is running: close it first (the rebuild replaces build-darwin/game.dylib).',
                                parent=self.top)
            return
        if not self.save():
            return
        if not rebuild:
            self.load()
            self.status.configure(text='Saved. RomFS mods need no rebuild: the game reads them the next time it starts.')
            return
        self.run = self.app.start_setup([])
        if not self.run:
            return
        self.log.configure(state='normal')
        self.log.delete('1.0', 'end')
        self.log.configure(state='disabled')
        self.step, self.t0 = -1, time.time()
        self.tree.state(['disabled'])
        self.poll()

    def poll(self):
        if not self.top.winfo_exists() or not self.run:
            return
        for kind, v in self.run.poll():
            if kind == 'line':
                self.app.log_append(self.log, v)
                m = re.match(r'\[(\d+)/\d+\].* \.\.\.', v)
                if m and 1 <= int(m.group(1)) <= len(MSTEP):
                    self.step = int(m.group(1)) - 1
            else:
                self.done(v)
                return
        self.status_show()
        self.top.after(200, self.poll)

    def done(self, code):
        s = int(time.time() - self.t0)
        self.run = None
        self.tree.state(['!disabled'])
        self.load()
        self.status.configure(text='Rebuilt in %d:%02d. The game has these mods from the next Play.' % (s // 60, s % 60) if code == 0
                              else 'The rebuild stopped (the log says why). Fix it and press Apply again, before you play.')

    def close(self):
        if self.run:
            if not messagebox.askyesno('B2recomp', 'Stop the rebuild? Press Apply again later: the game must be rebuilt before you play.',
                                       parent=self.top):
                return
            self.run.stop()
            self.run = None
        super().close()


def main():
    if '--add-to-menu' in sys.argv[1:]:                   # no window: the app-menu entry only
        print('B2recomp: app-menu entry %s' % add_to_menu())
        launcher_state({'menu_added': True})
        return 0
    if '--remove-from-menu' in sys.argv[1:]:
        remove_from_menu()
        print('B2recomp: removed from the app menu (./B2recomp --add-to-menu puts it back)')
        return 0
    build = '--build' in sys.argv[1:]
    update = needs_update()
    root = tk.Tk(className='B2recomp')
    app = Launcher(root, build or update or (is_file(os.path.join(ROOT, 'scripts', 'setup.py')) and not game_built()), update)
    root.mainloop()
    return 0


if __name__ == '__main__':
    sys.exit(main())
