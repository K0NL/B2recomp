"""DRAFT keyless replacement for scripts/make_icon.py (public repo: scripts/make_icon_local.py).

Makes build/bayonetta2.ico from the icon file of the user's OWN extracted control data; decrypts nothing.

    python scripts/make_icon_local.py <icon_AmericanEnglish.dat> [<out.ico>]

The input is the 256x256 JPEG HOME-menu icon (any image Pillow can open works, e.g. a picture of your own). The .ico
holds it at every size Windows asks for (16 to 256 px; 256 as PNG, the rest as bitmaps), scaled with Lanczos.
scripts/build_launcher.py and scripts/build_runtime.py link it as icon resource 1 when it exists; without it the exes
have the default icon. The .ico is generated from your copy of the game: it lives in build/ and is never committed.
"""
import io
import os
import struct
import sys

from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..'))
OUT = os.path.join(ROOT, 'build', 'bayonetta2.ico')
SIZES = [16, 20, 24, 32, 40, 48, 64, 96, 128, 256]


def write_ico(src, out_path):
    images = []
    for s in SIZES:
        im = src.resize((s, s), Image.LANCZOS)
        if s >= 256:
            b = io.BytesIO(); im.save(b, 'PNG'); data = b.getvalue()
        else:                                                # 32-bit BMP entry: BITMAPINFOHEADER (double height), BGRA bottom-up, AND mask
            px = im.tobytes('raw', 'BGRA')
            rows = [px[y * s * 4:(y + 1) * s * 4] for y in range(s)][::-1]
            mask = b'\0' * (((s + 31) // 32) * 4 * s)
            data = struct.pack('<IiiHHIIiiII', 40, s, s * 2, 1, 32, 0, s * s * 4, 0, 0, 0, 0) + b''.join(rows) + mask
        images.append((s, data))
    out = struct.pack('<HHH', 0, 1, len(images))
    off = 6 + 16 * len(images)
    for s, data in images:
        out += struct.pack('<BBBBHHII', s % 256, s % 256, 0, 0, 1, 32, len(data), off)
        off += len(data)
    out += b''.join(d for _, d in images)
    os.makedirs(os.path.dirname(os.path.abspath(out_path)), exist_ok=True)
    with open(out_path, 'wb') as f:
        f.write(out)
    print('%s: %d sizes, %d bytes' % (out_path, len(images), len(out)))


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    out = sys.argv[2] if len(sys.argv) > 2 else OUT
    write_ico(Image.open(sys.argv[1]).convert('RGBA'), out)


if __name__ == '__main__':
    main()
