# TEST ONLY (resolution scale, finding 83): compares two SSBU_VK_DUMPTEX dumps (runtime/nvn_vk_draw.c) image by image.
# Each image is decoded, the scaled one reduced to the game's size (box filter), and the two compared per channel.
# Usage: python scripts/dumptex_compare.py <dump dir A> <dump dir B> [<out dir for side-by-side PNGs>] [seq,seq,...]
import os, sys, array
from PIL import Image

VK_R8G8B8A8_UNORM, VK_R8G8B8A8_SRGB, VK_B8G8R8A8_UNORM, VK_B8G8R8A8_SRGB = 37, 43, 44, 50
VK_R32_SFLOAT, VK_D24S8, VK_D32, VK_D32S8, VK_X8D24 = 100, 129, 126, 130, 125


def read_index(d):
    passes, dumps = [], {}
    for line in open(os.path.join(d, 'index.tsv')):
        f = line.rstrip('\n').split('\t')
        if f[0] == 'pass':
            continue
        if f[0] == '#dump':
            seq, w, h, bpb, vkfmt, gw, gh = map(int, f[1:8])
            dumps[seq] = dict(w=w, h=h, bpb=bpb, vkfmt=vkfmt, gw=gw, gh=gh, nfmt=f[8])
        else:
            passes.append(f)
    return passes, dumps


def load(d, seq, info):
    """-> list of PIL 'F' images (channels) at the game's size"""
    raw = open(os.path.join(d, 'd%03d.raw' % seq), 'rb').read()
    w, h, vk = info['w'], info['h'], info['vkfmt']
    if vk in (VK_D24S8, VK_X8D24):
        a = array.array('I'); a.frombytes(raw)
        f = array.array('f', ((v & 0xffffff) / 16777215.0 for v in a))
        chans = [Image.frombytes('F', (w, h), f.tobytes())]
    elif vk in (VK_R32_SFLOAT, VK_D32, VK_D32S8) and info['bpb'] == 4:
        chans = [Image.frombytes('F', (w, h), raw, 'raw', 'F;32F')]
    elif info['bpb'] == 4:
        im = Image.frombytes('RGBA', (w, h), raw)
        if vk in (VK_B8G8R8A8_UNORM, VK_B8G8R8A8_SRGB):
            b, g, r, a = im.split(); im = Image.merge('RGBA', (r, g, b, a))
        chans = [c.convert('F') for c in im.split()]
    else:
        return None
    if (w, h) != (info['gw'], info['gh']):
        chans = [c.resize((info['gw'], info['gh']), Image.BOX) for c in chans]
    return chans


def stats(c):
    data = c.getdata()
    n = len(data)
    return sum(data) / n


def diff(a, b):
    da, db = a.getdata(), b.getdata()
    n = len(da)
    s = big = 0
    for x, y in zip(da, db):
        e = abs(x - y); s += e
    return s / n


def viz(chans):
    if len(chans) == 1:
        c = chans[0]
        lo, hi = c.getextrema()
        if hi - lo < 1e-12: hi = lo + 1
        return c.point(lambda v: (v - lo) * 255.0 / (hi - lo)).convert('L').convert('RGB')
    return Image.merge('RGB', [x.convert('L') for x in chans[:3]])


def main():
    da, db = sys.argv[1], sys.argv[2]
    out = sys.argv[3] if len(sys.argv) > 3 else None
    only = set(int(x) for x in sys.argv[4].split(',')) if len(sys.argv) > 4 else None
    pa, ia = read_index(da)
    pb, ib = read_index(db)
    if out: os.makedirs(out, exist_ok=True)
    for f in pa:
        dumps = [x.split('=') for x in f[7].split()] if len(f) > 7 and f[7] else []
        for role, seq in dumps:
            seq = int(seq)
            if only and seq not in only: continue
            if seq not in ia or seq not in ib: continue
            ca, cb = load(da, seq, ia[seq]), load(db, seq, ib[seq])
            if not ca or not cb:
                print('%3s %-10s %3d %s: format not decoded' % (f[0], f[1], seq, role)); continue
            names = 'RGBA' if len(ca) == 4 else 'v'
            parts = []
            for i in range(len(ca)):
                parts.append('%s %.4g/%.4g d%.3g' % (names[i], stats(ca[i]), stats(cb[i]), diff(ca[i], cb[i])))
            print('%3s %-10s d%03d %-3s %s nfmt %s | %s' % (f[0], f[1], seq, role, f[2], ia[seq]['nfmt'], ' | '.join(parts)), flush=True)
            if out:
                va, vb = viz(ca), viz(cb)
                m = Image.new('RGB', (va.width * 2, va.height)); m.paste(va, (0, 0)); m.paste(vb, (va.width, 0))
                m.save(os.path.join(out, 'd%03d_%s.png' % (seq, role)))


main()
