#!/usr/bin/env python3
"""Emulator-style code mods for the player build: IPSwitch text patches (.pchtxt) and IPS / IPS32 patches (.ips), the
formats Yuzu / Ryujinx mods use, applied to targets/exefs/main.elf before the lift (scripts/setup.py step `mods`).

    python scripts/pchtxt.py list  <mods folder> [<main NSO>] [--ini=<mods.ini>]   what would be applied (and whether it fits)
    python scripts/pchtxt.py apply <mods folder> <main NSO> <in.elf> <out.elf> [--ini=<mods.ini>]
    python scripts/pchtxt.py tree  <mods folder> [<main NSO>] [--ini=<mods.ini>]   every mod, file and block with its switch
                                                                                     (tab-separated; the launcher's Mods window)

The repository ships only this mechanism, never anyone's patches. Files are read from the mods folder and every folder
under it (an emulator mod pack's `<mod>/exefs/` layout works as it is), in path order; a later file wins where two
patch the same bytes.

On / off switches, like the emulators' per-mod check boxes: config/mods.ini (the default --ini; the launcher's Mods window
writes it), section [mods], lines `<key> = off` or `<key> = on`. A key is a mod (the first folder below the mods folder,
or a file lying directly in it), a file (its path below the mods folder, with /), or one block of a .pchtxt
(`<file> :: <block title>`, or `<file> :: #<n>` for an untitled or repeated title; `on` also applies a block written
under @disabled). Keys are compared without case. Anything not listed keeps its default: mods and files on, blocks as
their @enabled / @disabled line says. A mod or file switched off is not read at all, so one for another version of the
game does not stop the build. Renaming a file (e.g. to .pchtxt.off) also leaves it out.

Offsets, as the emulators apply them: the patched buffer is the 0x100-byte NSO header followed by the program image
(text, rodata, data at their memory offsets). So a .pchtxt offset X means image offset X + offset_shift - 0x100
(mods written with `@flag offset_shift 0x100` give image offsets directly), and an .ips offset means offset - 0x100.
Image offset = the address minus the module base; nso2elf.py writes the image at file offset 0x1000 + image offset
(found here through the ELF's PT_LOAD headers). Every file must name the build ID of your main (`@nsobid-<id>` /
`<id>.ips`); a file for another build or module is refused.
"""
import hashlib
import os
import re
import struct
import sys

NSO_HEADER = 0x100
EXTS = ('.pchtxt', '.ips')


DEFAULT_INI = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), 'config', 'mods.ini')


class ModError(Exception):
    pass


def rel_key(rel):
    return rel.replace('\\', '/')


def norm_key(k):
    return re.sub(r'\s*::\s*', ' :: ', rel_key(k.strip())).lower()


def read_settings(path):
    """mods.ini -> {normalised key: True (on) / False (off)}; a missing file = every switch at its default."""
    out, sect = {}, ''
    if not path or not os.path.isfile(path):
        return out
    with open(path, encoding='utf-8-sig', errors='replace') as f:
        for raw in f:
            line = raw.strip()
            if not line or line[0] in ';#':
                continue
            if line.startswith('[') and line.endswith(']'):
                sect = line[1:-1].strip().lower()
                continue
            if sect not in ('', 'mods') or '=' not in line:
                continue
            k, v = line.rsplit('=', 1)                       # the value is the last word: a block title may hold '='
            v = re.split(r'\s[;#]', v)[0].strip().lower()   # "off ; comment", as the runtime reads [mods] (nnfs.c)
            if v in ('off', '0', 'no', 'false', 'disabled'):
                out[norm_key(k)] = False
            elif v in ('on', '1', 'yes', 'true', 'enabled'):
                out[norm_key(k)] = True
    return out


def mod_of(rel):
    return rel_key(rel).split('/')[0]


def file_on(rel, settings):
    return settings.get(norm_key(mod_of(rel)), True) and settings.get(norm_key(rel), True)


def block_keys(rel, blocks):
    """The mods.ini key of each block: '<file> :: <title>', or '<file> :: #<n>' (1-based) for an empty or repeated title."""
    titles = [t for t, _, _ in blocks]
    return ['%s :: %s' % (rel_key(rel), t) if t and titles.count(t) == 1 else '%s :: #%d' % (rel_key(rel), i + 1)
            for i, t in enumerate(titles)]


def nso_build_id(path):
    with open(path, 'rb') as f:
        head = f.read(0x60)
    if head[:4] != b'NSO0':
        raise ModError('%s is not an NSO file' % path)
    return head[0x40:0x60].hex()


def same_build(a, b):
    """Build IDs as the emulators compare them: hex, case-insensitive, zero-padded (trailing zeros carry nothing)."""
    return a.lower().rstrip('0') == b.lower().rstrip('0')


def find_mods(folder):
    if not folder or not os.path.isdir(folder):
        return []
    found = []
    for d, dirs, files in os.walk(folder):
        dirs.sort(key=str.lower)
        found += [os.path.join(d, f) for f in files if f.lower().endswith(EXTS)]
    return sorted(found, key=lambda p: os.path.relpath(p, folder).lower())


def _hex_bytes(s, little_endian):
    if len(s) % 2 or not re.fullmatch(r'[0-9a-fA-F]+', s):
        raise ValueError('not an even number of hex digits: %r' % s)
    b = bytes.fromhex(s)
    return b[::-1] if little_endian else b


def _unescape(s):
    return s.encode('latin-1').decode('unicode_escape').encode('latin-1')


def parse_pchtxt(text, name='<pchtxt>'):
    """-> (build id or None, [(title, enabled, [(image offset, bytes, line no)])]). IPSwitch syntax as Yuzu reads it:
    @nsobid-<id>, @flag offset_shift <n>, @little-endian / @big-endian, @enabled / @disabled start a block (its title is
    the last // comment), lines `<hex offset> <hex bytes | "string">`, @stop ends the file; # and // are comments."""
    bid, shift, little, blocks, title, cur = None, 0, False, [], '', None
    for n, raw in enumerate(text.splitlines(), 1):
        line = raw.strip()
        if not line:
            continue
        if line.startswith('//'):
            title = line[2:].strip() or title
            continue
        if line.startswith('#'):
            continue
        if line.startswith('@'):
            word = line.split()[0].lower()
            if word == '@stop':
                break
            if word.startswith('@nsobid-'):
                bid = line.split()[0][8:]
            elif word == '@flag':
                parts = line.split()
                if len(parts) >= 3 and parts[1].lower() == 'offset_shift':
                    shift = int(parts[2], 0)
                elif len(parts) >= 2 and parts[1].lower() in ('little-endian', 'big-endian'):
                    little = parts[1].lower() == 'little-endian'
            elif word in ('@little-endian', '@big-endian'):
                little = word == '@little-endian'
            elif word in ('@enabled', '@disabled'):
                cur = (title, word == '@enabled', [])
                blocks.append(cur)
            continue
        if cur is None:
            continue                                         # text outside a block, as the emulators do
        m = re.match(r'([0-9a-fA-F]+)\s+("(?:[^"\\]|\\.)*"|[0-9a-fA-F]+)(\s|//|$)', line)   # anything after: a comment
        if not m:
            raise ModError('%s line %d: expected "<hex offset> <hex bytes>" or <hex offset> "string": %r' % (name, n, raw))
        val = m.group(2)
        try:
            if val.startswith('"'):
                data = _unescape(val[1:-1])
            else:
                data = _hex_bytes(val, little)
        except ValueError as e:
            raise ModError('%s line %d: %s' % (name, n, e))
        cur[2].append((int(m.group(1), 16) + shift - NSO_HEADER, data, n))
    return bid, blocks


def parse_ips(data, name='<ips>'):
    """IPS ('PATCH' ... 'EOF', 3-byte offsets) or IPS32 ('IPS32' ... 'EEOF', 4-byte offsets), RLE records included."""
    if data[:5] == b'PATCH':
        width, eof = 3, b'EOF'
    elif data[:5] == b'IPS32':
        width, eof = 4, b'EEOF'
    else:
        raise ModError('%s: not an IPS / IPS32 file' % name)
    recs, p = [], 5
    while True:
        if data[p:p + width] == eof and p + width >= len(data) - 4:   # EOF, optionally followed by a truncation size
            break
        if p + width + 2 > len(data):
            raise ModError('%s: ends without %s' % (name, eof.decode()))
        off = int.from_bytes(data[p:p + width], 'big')
        size, = struct.unpack_from('>H', data, p + width)
        p += width + 2
        if size:
            chunk = data[p:p + size]
            p += size
        else:                                                # RLE: 2-byte count, 1 byte value
            count, = struct.unpack_from('>H', data, p)
            chunk = data[p + 2:p + 3] * count
            p += 3
        recs.append((off - NSO_HEADER, chunk, len(recs) + 1))
    return recs


def read_mod(folder, path):
    """-> (build id, [(title, enabled, recs)]) of one .pchtxt / .ips file."""
    rel = os.path.relpath(path, folder)
    if path.lower().endswith('.ips'):
        with open(path, 'rb') as f:
            data = f.read()
        bid = os.path.splitext(os.path.basename(path))[0]
        if not re.fullmatch(r'[0-9a-fA-F]{2,64}', bid):
            raise ModError('%s: an .ips file is named after the build ID it patches (<build id>.ips)' % rel)
        return bid, [('', True, parse_ips(data, rel))]
    with open(path, 'rb') as f:
        text = f.read().decode('utf-8-sig', 'replace')
    bid, blocks = parse_pchtxt(text, rel)
    if not bid:
        raise ModError('%s: no @nsobid-<build id> line, so it is not known which program it patches' % rel)
    return bid, blocks


def check_build(rel, bid, want):
    if want and not same_build(bid, want):
        raise ModError('%s is for build ID %s, but your main is %s: it was written for another version of the game '
                       '(or another module, e.g. sdk / subsdk). Remove it from the mods folder or switch it off.'
                       % (rel, bid.lower().rstrip('0'), want.rstrip('0')))


def load_mods(folder, main_nso=None, settings=None):
    """-> [(file, title, [(image offset, bytes, where)])] for the enabled patches; refuses a file for another build.
    settings: read_settings() of mods.ini (None = every switch at its default)."""
    want = nso_build_id(main_nso) if main_nso else None
    settings = settings or {}
    out = []
    for path in find_mods(folder):
        rel = os.path.relpath(path, folder)
        if not file_on(rel, settings):
            continue                                         # switched off: not even read
        bid, blocks = read_mod(folder, path)
        check_build(rel, bid, want)
        for key, (title, enabled, recs) in zip(block_keys(rel, blocks), blocks):
            if settings.get(norm_key(key), enabled) and recs:
                out.append((rel, title, [(o, b, '%s:%s' % (rel, w)) for o, b, w in recs]))
    return out


def has_romfs(mod):
    """<mod>/romfs, or <mod>/<one folder>/romfs (an archive unpacked into a folder of its own), as runtime/nnfs.c looks."""
    if not os.path.isdir(mod):
        return False
    if os.path.isdir(os.path.join(mod, 'romfs')):
        return True
    return any(os.path.isdir(os.path.join(mod, d, 'romfs')) for d in os.listdir(mod) if not d.startswith('.'))


def tree(folder, main_nso=None, settings=None):
    """Every mod, file and block with its switch, for the launcher's Mods window: lines of tab-separated fields
         mod    <key> <on> <label> <note> <romfs>        a folder below the mods folder, or a file lying directly in it;
                                                         romfs: 'romfs' when the folder also holds RomFS files (romfs/ in it
                                                         or one folder down, as runtime/nnfs.c finds them), else ''. A
                                                         folder with RomFS files and no code patches is
                                                         listed too, with romfs = 'romfs-only'.
         file   <key> <on> <label> <note>                a file inside a mod folder (label: its path inside the mod)
         block  <key> <on> <default> <records> <label>   a .pchtxt block (default 1 = written under @enabled)
         state  <fingerprint | none | error> <message>   what setup.py would build with these switches (setup_state.json 'mods')
    in this order: a mod, then its files, each followed by its blocks. on: 1 / 0, as mods.ini or the default says."""
    want = nso_build_id(main_nso) if main_nso else None
    settings = settings or {}
    lines, seen = [], set()
    for path in find_mods(folder):
        rel = os.path.relpath(path, folder)
        mod, rk = mod_of(rel), rel_key(rel)
        ips = path.lower().endswith('.ips')
        try:
            bid, blocks = read_mod(folder, path)
            note = 'IPS, %d record(s)' % len(blocks[0][2]) if ips else ''
            if want and not same_build(bid, want):
                blocks, note = [], 'for build ID %s, not your main: another version of the game or another module' \
                                   % bid.lower().rstrip('0')
        except ModError as e:
            blocks, note = [], str(e).replace(rel + ': ', '')
        if mod not in seen:
            seen.add(mod)
            lines.append(['mod', mod, int(settings.get(norm_key(mod), True)), mod, note if mod == rk else '',
                          'romfs' if has_romfs(os.path.join(folder, mod)) else ''])
        if mod != rk:
            lines.append(['file', rk, int(settings.get(norm_key(rk), True)), rk[len(mod) + 1:], note])
        if not ips:
            for key, (title, enabled, recs) in zip(block_keys(rel, blocks), blocks):
                lines.append(['block', key, int(settings.get(norm_key(key), enabled)), int(enabled), len(recs),
                              title or 'block %s' % key.rsplit('#', 1)[-1]])
    if os.path.isdir(folder):                            # RomFS-only mods: the game reads them at start, no rebuild
        for d in sorted(os.listdir(folder), key=str.lower):
            if d not in seen and has_romfs(os.path.join(folder, d)):
                lines.append(['mod', d, int(settings.get(norm_key(d), True)), d, '', 'romfs-only'])
    try:
        lines.append(['state', fingerprint(load_mods(folder, main_nso, settings)) or 'none', ''])
    except ModError as e:
        lines.append(['state', 'error', str(e)])
    return ['\t'.join(' '.join(str(x).split()) for x in l) for l in lines]   # no tabs or line breaks inside a field


def fingerprint(mods):
    """None when nothing would change; else a hash of exactly what would be written (not of comments or file names)."""
    h, any_bytes = hashlib.sha256(), False
    for _, _, recs in mods:
        for off, data, _ in recs:
            h.update(struct.pack('<qI', off, len(data)) + data)
            any_bytes |= bool(data)
    return h.hexdigest() if any_bytes else None


def load_segments(elf):
    if elf[:4] != b'\x7fELF':
        raise ModError('not an ELF file')
    phoff, = struct.unpack_from('<Q', elf, 0x20)
    phentsize, phnum = struct.unpack_from('<HH', elf, 0x36)
    segs = []
    for i in range(phnum):
        t, _, off, va, _, fsz = struct.unpack_from('<IIQQQQ', elf, phoff + i * phentsize)
        if t == 1:
            segs.append((va, off, fsz))
    return segs


def apply(elf, mods, log=print):
    """Patch elf (bytearray) in place. Every byte must fall inside a PT_LOAD segment's file image (not the .bss)."""
    segs = load_segments(elf)
    written = {}
    for rel, title, recs in mods:
        log('  %s%s' % (rel, ': ' + title if title else ''))
        for off, data, where in recs:
            if not data:
                continue
            seg = next((s for s in segs if s[0] <= off and off + len(data) <= s[0] + s[2]), None)
            if off < 0 or seg is None:
                raise ModError('%s: image offset 0x%x (+%d bytes) is outside the program\'s code and data in the file '
                               '(offset_shift wrong, or the patch is for another program)' % (where, off, len(data)))
            fo = seg[1] + off - seg[0]
            old = bytes(elf[fo:fo + len(data)])
            over = sorted({written[o] for o in range(off, off + len(data)) if o in written and written[o] != rel})
            elf[fo:fo + len(data)] = data
            for o in range(off, off + len(data)):
                written[o] = rel
            log('    0x%08x  %s -> %s%s%s' % (off, _short(old), _short(data), '  (unchanged)' if old == data else '',
                                              '  (overrides %s)' % ', '.join(over) if over else ''))
    return len(written)


def _short(b):
    return b.hex() if len(b) <= 16 else '%s.. (%d bytes)' % (b[:16].hex(), len(b))


def main(argv):
    ini = next((a[6:] for a in argv if a.startswith('--ini=')), DEFAULT_INI)
    argv = [a for a in argv if not a.startswith('--ini=')]
    settings = read_settings(ini)
    try:
        if len(argv) >= 2 and argv[0] == 'tree':
            sys.stdout.reconfigure(encoding='utf-8')
            print('\n'.join(tree(argv[1], argv[2] if len(argv) > 2 else None, settings)))
            return 0
        if len(argv) >= 2 and argv[0] == 'list':
            mods = load_mods(argv[1], argv[2] if len(argv) > 2 else None, settings)
            if not mods:
                print('no enabled patches in %s' % argv[1])
            for rel, title, recs in mods:
                print('%s%s: %d record(s), %d bytes' % (rel, ': ' + title if title else '', len(recs),
                                                        sum(len(r[1]) for r in recs)))
            return 0
        if len(argv) == 5 and argv[0] == 'apply':
            mods = load_mods(argv[1], argv[2], settings)
            with open(argv[3], 'rb') as f:
                elf = bytearray(f.read())
            n = apply(elf, mods)
            with open(argv[4], 'wb') as f:
                f.write(elf)
            print('%d bytes patched -> %s' % (n, argv[4]))
            return 0
    except ModError as e:
        print('error: %s' % e, file=sys.stderr)
        return 1
    print(__doc__, file=sys.stderr)
    return 2


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
