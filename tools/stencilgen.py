#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Stencils for HIRAKU's run-time code generator (runtime/jit/jit.h), from an object file.

    tools/stencilgen.py OBJECT OUT.inc NAMESPACE

OBJECT is a stencil source (runtime/jit/*_stencils.c) compiled by clang for ELF with
-ffunction-sections: each function is a stencil, and each relocation the compiler left in it is a
hole (the symbols H0..H11: a value given when the stencil is copied), the jump to the stencil
copied after it (HK_CONTINUE) or a jump to a place given when it is copied (HK_TARGET0, 1). OUT.inc gets, in namespace hk::jit::NAMESPACE, the stencils' bytes,
their relocations and an enum of their names, in the source's order.

The script refuses what the copying could not honour: a relocation against anything else (a
constant the compiler put in a data section, a call by name), a relocation type it does not know.
So a compiler that starts making such code stops the build instead of making a wrong program.
"""
import struct
import sys

EM_X86_64 = 62
SHT_SYMTAB, SHT_RELA = 2, 4
STT_FUNC = 2
HOLES = [f'H{i}' for i in range(12)]
CONTINUE = 'HK_CONTINUE'
TARGETS = ['HK_TARGET0', 'HK_TARGET1']
# relocation type -> the copier's kind (runtime/jit/jit.h, Kind)
X86_64 = {10: 'ABS32', 11: 'ABS32S', 2: 'REL32', 4: 'REL32'}   # R_X86_64_32, _32S, _PC32, _PLT32


def die(msg):
    sys.exit(f'stencilgen: {msg}')


def read(path):
    d = open(path, 'rb').read()
    if d[:4] != b'\x7fELF' or d[4] != 2 or d[5] != 1:
        die(f'{path}: not a 64-bit little-endian ELF object')
    machine = struct.unpack_from('<H', d, 18)[0]
    if machine != EM_X86_64:
        die(f'{path}: machine {machine}: only x86-64 stencils so far')
    shoff, = struct.unpack_from('<Q', d, 0x28)
    shentsize, shnum, shstrndx = struct.unpack_from('<HHH', d, 0x3a)
    secs = []
    for i in range(shnum):
        name, typ, _flags, _addr, off, size, link, info, _align, entsize = struct.unpack_from('<IIQQQQIIQQ', d, shoff + i * shentsize)
        secs.append(dict(name=name, type=typ, off=off, size=size, link=link, info=info, entsize=entsize))

    def cstr(sec, at):
        base = secs[sec]['off'] + at
        return d[base:d.index(b'\0', base)].decode()
    for s in secs:
        s['name'] = cstr(shstrndx, s['name'])
    symtab = next((s for s in secs if s['type'] == SHT_SYMTAB), None)
    if not symtab:
        die(f'{path}: no symbol table')
    syms = []
    for i in range(symtab['size'] // 24):
        name, info, _other, shndx, value, size = struct.unpack_from('<IBBHQQ', d, symtab['off'] + i * 24)
        syms.append(dict(name=cstr(symtab['link'], name), type=info & 15, shndx=shndx, value=value, size=size))
    relocs = {}   # section index -> [(offset, type, symbol name, addend)]
    for s in secs:
        if s['type'] != SHT_RELA:
            continue
        for i in range(s['size'] // 24):
            off, info, addend = struct.unpack_from('<QQq', d, s['off'] + i * 24)
            relocs.setdefault(s['info'], []).append((off, info & 0xffffffff, syms[info >> 32]['name'], addend))
    out = []
    for sym in syms:
        if sym['type'] != STT_FUNC or not secs[sym['shndx']]['name'].startswith('.text'):
            continue
        sec = secs[sym['shndx']]
        code = d[sec['off'] + sym['value']:sec['off'] + sym['value'] + sym['size']]
        rs = []
        for off, typ, target, addend in sorted(relocs.get(sym['shndx'], [])):
            if not sym['value'] <= off < sym['value'] + sym['size']:
                continue
            if typ not in X86_64:
                die(f"{sym['name']}: relocation type {typ} against {target}: not one the copier knows")
            kind = X86_64[typ]
            if target == CONTINUE or target in TARGETS:
                if kind != 'REL32':
                    die(f"{sym['name']}: {target} is not reached by a jump")
                hole = 0
                if target in TARGETS:
                    kind, hole = 'REL32T', TARGETS.index(target)
            elif target in HOLES:
                if kind == 'REL32':
                    die(f"{sym['name']}: {target} used as a place in the code")
                hole = HOLES.index(target)
            else:
                die(f"{sym['name']}: a relocation against {target!r}: a stencil may only name its holes, {CONTINUE} and its "
                    'targets (a constant in a data section, a switch table or a call by name cannot be copied)')
            rs.append((off - sym['value'], kind, hole, addend))
        # the jump to the next stencil, where it is the last instruction: dropped when copied
        tail = 0
        if len(code) >= 5 and code[-5] == 0xe9 and any(o == len(code) - 4 and k == 'REL32' for o, k, _, _ in rs):
            tail = 5
        out.append(dict(name=sym['name'], index=sym['shndx'], code=code, relocs=rs, tail=tail))
    out.sort(key=lambda s: s['index'])   # the source's order
    if not out:
        die(f'{path}: no stencils (compile with -ffunction-sections)')
    return out


def emit(stencils, ns, source):
    o = [f'// Generated by tools/stencilgen.py from {source}: {len(stencils)} stencils. Do not edit.\n',
         f'namespace hk::jit::{ns} {{\n', 'enum : int {\n']
    o += [f'\tST_{s["name"]},\n' for s in stencils]
    o += ['\tST_COUNT\n};\n', 'inline constexpr unsigned char CODE[] = {\n']
    at = 0
    for s in stencils:
        s['at'] = at
        at += len(s['code'])
        o.append('\t' + ''.join(f'0x{b:02x},' for b in s['code']) + f'   // {s["name"]}\n')
    o += ['};\n', 'inline constexpr hk::jit::Reloc RELOCS[] = {\n']
    n = 0
    for s in stencils:
        s['first'] = n
        n += len(s['relocs'])
        for off, kind, hole, addend in s['relocs']:
            o.append(f'\t{{{off}, hk::jit::{kind}, {hole}, {addend}}},   // {s["name"]}\n')
    if not n:
        o.append('\t{0, hk::jit::ABS32, 0, 0},\n')
    o += ['};\n', 'inline constexpr hk::jit::Stencil STENCILS[] = {\n']
    for s in stencils:
        o.append(f'\t{{{s["at"]}, {len(s["code"])}, {s["tail"]}, {s["first"]}, {len(s["relocs"])}}},   // {s["name"]}\n')
    o += ['};\n', 'inline constexpr hk::jit::Set SET = {CODE, RELOCS, STENCILS, ST_COUNT};\n', '}\n']
    return ''.join(o)


def main():
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    obj, out, ns = sys.argv[1:]
    text = emit(read(obj), ns, obj.rsplit('/', 1)[-1])
    try:
        if open(out).read() == text:
            return
    except OSError:
        pass
    with open(out + '.tmp', 'w') as f:
        f.write(text)
    import os
    os.replace(out + '.tmp', out)


if __name__ == '__main__':
    main()
