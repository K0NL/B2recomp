"""Minimal CRI CPK reader: @UTF tables, TOC listing, CRILAYLA decompression.

Works on any object with read(offset, size) -> bytes, so a CPK can be read straight out of the
decrypted RomFS without extracting it first.
"""
import struct

STORAGE_NONE, STORAGE_CONST, STORAGE_ROW, STORAGE_CONST2 = 0x10, 0x30, 0x50, 0x70


def _unmask(b):
    """Some @UTF tables are XOR-obfuscated with a byte LCG."""
    out, m = bytearray(b), 0x655F
    for i in range(len(out)):
        out[i] ^= m & 0xFF
        m = (m * 0x4115) & 0xFFFFFFFF
    return bytes(out)


def parse_utf(blob):
    if blob[:4] != b'@UTF':
        blob = _unmask(blob)
        if blob[:4] != b'@UTF':
            raise ValueError('not a @UTF table')
    size, = struct.unpack_from('>I', blob, 4)
    t = blob[8:8 + size]
    _, rows_off, strings_off, data_off, name_off, ncols, row_w, nrows = struct.unpack_from('>HHIIIHHI', t, 0)

    def string(o):
        s = strings_off + o
        return t[s:t.index(b'\0', s)].decode('utf-8', 'replace')

    def value(typ, pos):
        fmt = {0: 'B', 1: 'b', 2: 'H', 3: 'h', 4: 'I', 5: 'i', 6: 'Q', 7: 'q', 8: 'f', 9: 'd'}.get(typ)
        if fmt:
            return struct.unpack_from('>' + fmt, t, pos)[0], struct.calcsize(fmt)
        if typ == 0xA:
            return string(struct.unpack_from('>I', t, pos)[0]), 4
        if typ == 0xB:
            o, n = struct.unpack_from('>II', t, pos)
            return t[data_off + o:data_off + o + n], 8
        raise ValueError('unknown @UTF column type %x' % typ)

    cols, pos = [], 0x18
    for _ in range(ncols):
        flags, nm = struct.unpack_from('>BI', t, pos)
        pos += 5
        storage, typ = flags & 0xF0, flags & 0x0F
        const = None
        if storage in (STORAGE_CONST, STORAGE_CONST2):
            const, n = value(typ, pos)
            pos += n
        cols.append((string(nm), storage, typ, const))
    rows = []
    for r in range(nrows):
        p, row = rows_off + r * row_w, {}
        for name, storage, typ, const in cols:
            if storage == STORAGE_ROW:
                row[name], n = value(typ, p)
                p += n
            elif storage in (STORAGE_CONST, STORAGE_CONST2):
                row[name] = const
            else:
                row[name] = None
        rows.append(row)
    return string(name_off), rows


def _packet_utf(src, offset):
    """CPK packets: 4-byte magic, u32 flags, u64 LE size, then the @UTF table at +0x10."""
    magic, _, size = struct.unpack('<4sIQ', src.read(offset, 16))
    return magic, parse_utf(src.read(offset + 16, size))[1]


class Cpk:
    def __init__(self, src):
        self.src = src
        magic, hdr = _packet_utf(src, 0)
        assert magic == b'CPK ', magic
        self.header = hdr[0]
        self.files = []
        toc = self.header.get('TocOffset')
        if toc:
            content = self.header.get('ContentOffset') or toc
            base = min(toc, content)
            magic, rows = _packet_utf(src, toc)
            assert magic == b'TOC ', magic
            for r in rows:
                path = '/'.join(x for x in (r.get('DirName'), r.get('FileName')) if x)
                self.files.append({'path': path, 'offset': base + r['FileOffset'], 'size': r['FileSize'],
                                   'extract_size': r.get('ExtractSize') or r['FileSize'], 'id': r.get('ID')})

    def read(self, entry):
        data = self.src.read(entry['offset'], entry['size'])
        if data[:8] == b'CRILAYLA':
            data = crilayla_decompress(data)
        return data


def crilayla_decompress(src):
    """CRILAYLA: LZ-style, read bit-backwards from the end; 0x100 bytes of raw prefix after the data."""
    uncompressed_size, header_offset = struct.unpack_from('<II', src, 8)
    out = bytearray(uncompressed_size + 0x100)
    out[:0x100] = src[0x10 + header_offset:0x10 + header_offset + 0x100]
    end = len(out) - 1
    pos = 0x10 + header_offset - 1                       # bit reader walks backwards
    bits_left, cur = 0, 0

    def take(n):
        nonlocal pos, bits_left, cur
        v = 0
        while n:
            if bits_left == 0:
                cur, bits_left, pos = src[pos], 8, pos - 1
            k = min(bits_left, n)
            v = (v << k) | ((cur >> (bits_left - k)) & ((1 << k) - 1))
            bits_left -= k
            n -= k
        return v

    written = 0
    while written < uncompressed_size:
        if take(1):
            back = end + 3 + take(13)
            length, lv = 3, 0
            for width in (2, 3, 5, 8):
                lv = take(width)
                length += lv
                if lv != (1 << width) - 1:
                    break
            if lv == 0xFF:
                while True:
                    lv = take(8)
                    length += lv
                    if lv != 0xFF:
                        break
            for _ in range(length):
                out[end] = out[back]
                end -= 1
                back -= 1
                written += 1
        else:
            out[end] = take(8)
            end -= 1
            written += 1
    return bytes(out)
