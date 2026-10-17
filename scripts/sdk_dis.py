"""Small helper for reading our sdk.elf: disassemble with relocation-resolved GOT/vtable loads.

    python scripts/sdk_dis.py dis <hex addr> [max instructions]   disassemble until ret (resolves `adrp+ldr` slots)
    python scripts/sdk_dis.py vt <hex slot>                       slot -> vtable symbol; prints table entries
"""
import os
import struct
import sys

import capstone

SDK = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'targets', 'exefs', 'sdk.elf')
s = open(SDK, 'rb').read()
F = 0x1000
phoff, = struct.unpack_from('<Q', s, 0x20)
phentsize, phnum = struct.unpack_from('<HH', s, 0x36)
for i in range(phnum):
    t, _, off, va, _, fsz, msz = struct.unpack_from('<IIQQQQQ', s, phoff + i * phentsize)
    if t == 2:
        dyn = off
tags = {}
while True:
    tag, val = struct.unpack_from('<qQ', s, dyn)
    dyn += 16
    if tag == 0:
        break
    tags.setdefault(tag, val)
REL = {}
for i in range(tags[8] // 24):
    off, info, add = struct.unpack_from('<QQq', s, F + tags[7] + i * 24)
    if info & 0xffffffff == 0x403:
        REL[off] = add
md = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)
md.detail = True


def dis(lo, n=80):
    page = {}
    for ins in md.disasm(s[F + lo:F + lo + 4 * n], lo):
        note = ''
        if ins.mnemonic == 'adrp':
            page[ins.op_str.split(',')[0]] = int(ins.op_str.split('#')[1], 16)
        elif ins.mnemonic == 'ldr' and '#' in ins.op_str and '[' in ins.op_str:
            base = ins.op_str.split('[')[1].split(',')[0]
            if base in page:
                slot = page[base] + int(ins.op_str.split('#')[1].rstrip(']'), 16)
                note = '   ; [%x] = %s' % (slot, hex(REL[slot]) if slot in REL else '?')
        elif ins.mnemonic == 'add' and '#' in ins.op_str:
            parts = [p.strip() for p in ins.op_str.split(',')]
            if parts[1] in page and parts[2].startswith('#'):
                note = '   ; = %x' % (page[parts[1]] + int(parts[2][1:], 16))
        print('  %x  %s %s%s' % (ins.address, ins.mnemonic, ins.op_str, note))
        if ins.mnemonic == 'ret':
            break


def vt(slot):
    sym = REL[slot]
    table = sym + 0x10
    print('slot %x -> %x, table %x' % (slot, sym, table))
    for k in range(0, 0xd0, 8):
        if table + k in REL:
            print('   +%02x -> %x' % (k, REL[table + k]))


if __name__ == '__main__':
    if sys.argv[1] == 'dis':
        dis(int(sys.argv[2], 16), int(sys.argv[3]) if len(sys.argv) > 3 else 80)
    else:
        vt(int(sys.argv[2], 16))
