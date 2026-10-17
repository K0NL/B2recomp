# [Recomps#1] copy of ssbu-recomp tools/extract/nso2elf.py @3a96916 (GPL-3.0, Denkishi/ssbu-recomp), unchanged: NSO -> ELF
# (LZ4 segments, SHA-256 of each segment checked against the NSO header; reads no keys). Used by scripts/setup.py convert
# and runtime/gen/make_gen.py.
import struct, sys, hashlib, lz4.block
src, dst = sys.argv[1], sys.argv[2]
d = open(src, 'rb').read()
assert d[:4] == b'NSO0'
flags = struct.unpack('<I', d[12:16])[0]
segs = []
for i, (ho, cso) in enumerate(((0x10, 0x60), (0x20, 0x64), (0x30, 0x68))):
    foff, moff, size = struct.unpack('<III', d[ho:ho+12])
    csz = struct.unpack('<I', d[cso:cso+4])[0]
    raw = d[foff:foff+csz]
    if flags & (1 << i): raw = lz4.block.decompress(raw, uncompressed_size=size)
    assert len(raw) == size
    ok = hashlib.sha256(raw).digest() == d[0xA0+i*0x20:0xC0+i*0x20]
    print('seg', i, 'vaddr %x size %x sha256 %s' % (moff, size, 'OK' if ok else 'MISMATCH'))
    segs.append((moff, raw))
bss = struct.unpack('<I', d[0x3c:0x40])[0]
print('build id', d[0x40:0x54].hex())
(tv, text), (rv, ro), (dv, data) = segs
mem = bytearray(dv + len(data))
for v, r in segs: mem[v:v+len(r)] = r
# MOD0 -> dynamic
mod = struct.unpack('<I', text[4:8])[0]
assert mem[mod:mod+4] == b'MOD0'
dyn_off, bss_s, bss_e = struct.unpack('<iii', mem[mod+4:mod+16])
dyn = mod + dyn_off
tags = {}; n = 0
while True:
    t, v = struct.unpack('<qQ', mem[dyn+n*16:dyn+n*16+16]); n += 1
    if t == 0: break
    tags.setdefault(t, v)
dynsz = n*16
symtab, strtab, strsz = tags[6], tags[5], tags[10]
if 4 in tags: nsym = struct.unpack('<I', mem[tags[4]+4:tags[4]+8])[0]
else: nsym = (strtab - symtab)//24
print('dynamic %x dynsym %x (%d syms) dynstr %x' % (dyn, symtab, nsym, strtab))
OFF = 0x1000
names = b'\0'; secs = []
def sec(name, typ, fl, addr, off, size, link=0, info=0, align=1, ent=0):
    global names
    ni = len(names); names += name.encode() + b'\0'
    secs.append(struct.pack('<IIQQQQIIQQ', ni, typ, fl, addr, off, size, link, info, align, ent))
secs.append(bytes(64))
sec('.text', 1, 6, tv, OFF+tv, len(text), align=16)
# non-overlapping sections: Ghidra mishandles .dynsym/.dynstr nested inside .rodata
assert rv <= symtab and symtab + nsym*24 <= strtab
sec('.rodata', 1, 2, rv, OFF+rv, symtab-rv, align=16)
sec('.dynsym', 11, 2, symtab, OFF+symtab, nsym*24, link=4, info=1, align=8, ent=24)
sec('.dynstr', 3, 2, strtab, OFF+strtab, strsz)
r1 = strtab+strsz
sec('.rodata.1', 1, 2, r1, OFF+r1, rv+len(ro)-r1, align=1)
sec('.data', 1, 3, dv, OFF+dv, dyn-dv, align=16)
sec('.dynamic', 6, 3, dyn, OFF+dyn, dynsz, link=4, align=8, ent=16)
d1 = dyn+dynsz
sec('.data.1', 1, 3, d1, OFF+d1, dv+len(data)-d1, align=1)
sec('.bss', 8, 3, bss_s, OFF+dv+len(data), bss_e-bss_s, align=16)
shstr_i = len(secs); shstr_off = OFF + len(mem)
sec('.shstrtab', 3, 0, 0, shstr_off, 0)
secs[-1] = secs[-1][:32] + struct.pack('<Q', len(names)) + secs[-1][40:]
shoff = (shstr_off + len(names) + 7) & ~7
ph = b''
def phdr(t, fl, v, fsz, msz, al): return struct.pack('<IIQQQQQQ', t, fl, OFF+v, v, v, fsz, msz, al)
ph += phdr(1, 5, tv, len(text), len(text), 0x1000)
ph += phdr(1, 4, rv, len(ro), len(ro), 0x1000)
ph += phdr(1, 6, dv, len(data), max(len(data), bss_e-dv, len(data)+bss), 0x1000)
ph += phdr(2, 6, dyn, dynsz, dynsz, 8)
eh = b'\x7fELF\x02\x01\x01' + bytes(9) + struct.pack('<HHIQQQIHHHHHH', 3, 183, 1, 0, 0x40, shoff, 0, 0x40, 0x38, 4, 0x40, len(secs), shstr_i)
out = bytearray(eh + ph); out += bytes(OFF - len(out)); out += mem; out += names
out += bytes(shoff - len(out)); out += b''.join(secs)
open(dst, 'wb').write(out)
print('wrote', dst, len(out))
