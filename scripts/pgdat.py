"""PlatinumGames DAT archive: list and extract.

Header (LE): 'DAT\\0', count, offsets_table, extensions_table, names_table, sizes_table, hashmap.
Names table starts with a u32 entry width; each name is a fixed-width NUL-padded slot.

    python scripts/pgdat.py list <file.dat>
    python scripts/pgdat.py extract <file.dat> <out_dir>
"""
import os
import struct
import sys


def entries(blob):
    magic, count, off_t, ext_t, name_t, size_t, _ = struct.unpack_from('<4s6I', blob, 0)
    assert magic == b'DAT\0', magic
    width, = struct.unpack_from('<I', blob, name_t)
    for i in range(count):
        off, = struct.unpack_from('<I', blob, off_t + 4 * i)
        size, = struct.unpack_from('<I', blob, size_t + 4 * i)
        ext = blob[ext_t + 4 * i:ext_t + 4 * i + 4].split(b'\0')[0].decode('latin-1')
        s = name_t + 4 + width * i
        name = blob[s:s + width].split(b'\0')[0].decode('latin-1')
        yield name, ext, off, size


if __name__ == '__main__':
    cmd, path = sys.argv[1], sys.argv[2]
    blob = open(path, 'rb').read()
    if cmd == 'list':
        for name, ext, off, size in entries(blob):
            print('%8d  %-5s %s  [%s]' % (size, ext, name, blob[off:off + 4].hex()))
    else:
        out = sys.argv[3]
        os.makedirs(out, exist_ok=True)
        n = 0
        for name, _, off, size in entries(blob):
            with open(os.path.join(out, name), 'wb') as f:
                f.write(blob[off:off + size])
            n += 1
        print('%d files -> %s' % (n, out))
