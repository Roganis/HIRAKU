#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The H8S/2600's instructions as stencils for the run-time code generator (PLAN.md section 45).

    tools/h8jitgen.py H8_DIR OUT_DIR

H8_DIR is MAME's cpu/h8 (h8.lst, h8make.py, h8.cpp). MAME's interpreter is made from h8.lst by
h8make.py; this reads the same list through h8make.py's own parser, for the same chip (the
H8S/2600), and writes into OUT_DIR:

  hk_h8_ops.h      one stencil a translated instruction (included by runtime/jit/h8_stencils.c):
                   the instruction's own statements from h8.lst, its memory accesses through the
                   made code's fast path (RAM and ROM in place; anything else leaves the block
                   before the instruction, its effects undone, for the interpreter), its cycles
                   counted as MAME's core counts them; and MAME's flag helpers from h8.cpp
  hk_h8_table.inc  for runtime/dev/xt446_h8jit.cpp: a decoder that follows MAME's dispatch
                   (h8make.py's save_exec) to an instruction's index, and per instruction its
                   stencil, its length in words, the most cycles it can take, whether it ends a block

An instruction is translated when nothing in it needs more than registers, flags and memory:
whatever calls into the CPU's other parts (the interrupt filter, sleep, trapa, the multiplier's
MAC, EXR changes) stays MAME's, and a block ends before it.
"""
import os
import re
import sys

UNTRANSLATED = ['update_irq_filter', 'exr_in_stack', 'trapa_setup', 'debugger_', 'total_cycles', 'suspend',
                'notify_standby', 'm_standby', 'while', 'eat-all-cycles', 'm_irq_vector', 'm_taken_irq_vector',
                'm_MAC', 'm_mac_saturating', 'illegal', 'm_EXR =', 'm_EXR |=', 'm_EXR &=',
                'm_EXR ^=', 'm_CCR = read', 'm_TMPR']
HELPER_RE = re.compile(r'^(u8|u16|u32|void) h8_device::((?:do_|set_nz)\w+)\(([^)]*)\)\n\{\n(.*?)\n\}\n', re.S | re.M)
ACCESS = ('read16i(', 'read8(', 'read16(', 'write8(', 'write16(')


def helpers(h8cpp):
    out = []
    for ret, name, args, body in HELPER_RE.findall(h8cpp):
        body = body.replace('m_has_hc', '1')
        args = 'u8 *ccrp' + (', ' + args if args.strip() else '')
        out.append(f'static inline {ret} {name}({args})\n{{\n#define m_CCR (*ccrp)\n{body}\n#undef m_CCR\n}}\n')
    return out


def convert(op, idx):
    """C for one instruction's stencil, or None (with why) where it stays MAME's."""
    src = list(op.source)
    text = '\n'.join(src)
    for w in UNTRANSLATED:
        if w in text:
            return None, w
    if 'F_I' in text or 'F_UI' in text:
        return None, 'interrupt mask'
    words = len(op.val) // 2 + op.skip + op.extra_words   # IR[0..words-1], the instruction's own words
    disp = len(op.val) // 2 + op.skip - 1                 # read by MAME's dispatch steps (IR[0] was prefetched)
    # m_PC set to anything but the next word: a jump (bsr adds its displacement)
    moves_pc = re.compile(r'm_PC\s*(=[^=]|\+=(?!\s*2;)|-=)')
    branch = 'prefetch_switch' in text or moves_pc.search(text) is not None
    cost = disp
    pc_static = True
    # a jump whose target the instruction's words give (relative, absolute): the builder makes it
    # only where the target is memory, so its fetch there needs no test
    static_target = re.search(r'_(rel8|rel16|abs24e|abs16e)\b', op.function_name()) is not None
    lines = []
    # Does a register write come before a later memory access? Then a slow access must put the
    # registers back (the instruction is then MAME's, from the start). Memory written before a slow
    # access needs nothing: the interpreter writes the same words again, from the same registers,
    # as long as nothing the instruction reads comes after something it writes (else it stays MAME's).
    seen_write = seen_mem_write = False
    need_snap = False
    for l in src:
        s = l.strip()
        reads = 'read8(' in s or 'read16(' in s or ('read16i(' in s and not re.match(r'(m_IR\[\d\]|m_PIR) = read16i\(m_PC\);', s))
        if reads and seen_mem_write:
            return None, 'reads after it writes'
        if reads or 'write8(' in s or 'write16(' in s:
            if seen_write:
                need_snap = True
        if re.search(r'\br(8|16|32)_w\(', s):
            seen_write = True
        if 'write8(' in s or 'write16(' in s:
            seen_mem_write = True
    for l in src:
        # the H8S/2600 has the multiplier (MAC): its multiplies take 2 internal cycles
        l = re.sub(r'm_has_mac \? (\d+) : \d+', r'\1', l)
        if 'm_has_mac' in l:
            return None, 'm_has_mac'
        s = l
        st = s.strip()
        m = re.match(r'm_IR\[(\d)\] = read16i\(m_PC\);$', st)
        if m:
            cost += 1
            lines.append('\tic -= 1;')
            continue
        if re.match(r'm_PIR = read16i\(m_PC\);$', st):
            cost += 1
            lines.append('\tic -= 1;' if pc_static or static_target else '\tFETCH(PC);')
            continue
        if re.match(r'm_TMP2 = read16i\(m_TMP1\);$', st):
            cost += 1
            lines.append('\tic -= 1;' if static_target else '\tFETCH(TMP1);')
            continue
        if 'read16i' in st:
            return None, 'fetch ' + st
        m = re.match(r'prefetch_switch\((.*), (.*)\);$', st)
        if m:
            s = s.replace(st, f'{{ NPC = ({m.group(1)}) & 0xffffff; PC = ({m.group(1)}) + 2; }}')
            pc_static = False
        if moves_pc.search(st):
            pc_static = False
        if re.match(r'prefetch_done\w*\(\);$', st):
            continue
        m = re.match(r'internal\((\d+)\);$', st)
        if m:
            cost += int(m.group(1))
            lines.append(f'\tic -= {m.group(1)};')
            continue
        if 'internal(' in st:
            return None, 'internal ' + st
        for a in ('read8(', 'read16(', 'write8(', 'write16('):
            cost += st.count(a)
        if re.search(r'm_IR\[\d\]\s*=[^=]', st):
            return None, 'IR written ' + st
        if 'm_PIR' in st:
            return None, 'PIR ' + st
        s = re.sub(r'\bm_IR\[(\d)\]', r'IR\1', s)
        s = s.replace('m_TMP1', 'TMP1').replace('m_TMP2', 'TMP2').replace('m_NPC', 'NPC').replace('m_PC', 'PC')
        s = s.replace('m_CCR', 'CCRL').replace('m_EXR', 'S->EXR')
        s = s.replace('m_mode_advanced', '1').replace('m_has_hc', '1')
        s = re.sub(r'\b((?:do_|set_nz)\w+)\(', r'\1(&CCRL, ', s)
        s = re.sub(r'\br(8|16|32)_([rw])\(', r'r\1_\2(S, ', s)
        s = re.sub(r'\bread8\(', 'RD8(', s)
        s = re.sub(r'\bread16\(', 'RD16(', s)
        wr = 'WR'
        s = re.sub(r'\bwrite8\(', wr + '8(', s)
        s = re.sub(r'\bwrite16\(', wr + '16(', s)
        s = re.sub(r'\babs\(', '__builtin_abs(', s)
        if re.search(r'\bm_\w+', s):
            return None, 'member ' + s.strip()
        lines.append(s)
    body = '\n'.join(lines)
    # registers named by the instruction's words: their places in the state worked out when the
    # block is made (holes 6 to 11), not decoded from the words as it runs
    regs = {}
    body = static_registers(body, regs)
    used_ir = sorted(set(int(k) for k in re.findall(r'\bIR(\d)\b', body)))
    name = f'op_{idx}_{op.function_name()}'
    c = [f'// {op.name} {op.am1} {op.am2}', f'HK_STENCIL({name}) {{']
    for (width, expr), h in sorted(regs.items(), key=lambda kv: kv[1]):
        c.append(f'\t// H{h}: the place of r{width} {expr}')
    for k in used_ir:
        c.append(f'\tconst u32 IR{k} = (u32)HK_IMM(H{k});')
    uses = lambda w: re.search(r'\b' + w + r'\b', body) is not None
    if uses('PC'):
        # (m_PC is past the words MAME's dispatch steps read: H5 is the instruction's address + 2)
        c.append(f'\tu32 PC = (u32)HK_IMM(H5) + {2 * disp};' if disp else '\tu32 PC = (u32)HK_IMM(H5);')
    if uses('NPC'):
        c.append('\tu32 NPC;')
    for t in ('TMP1', 'TMP2'):
        if uses(t):
            c.append(f'\tu32 {t};')
    accesses = any(x in body for x in ('RD8(', 'RD16(', 'WR8(', 'WR16(', 'FETCH('))
    if accesses:
        c.append('\tconst int64_t ic0 = ic;')
    c.append(f'\tif (ic - {cost} <= bc) HK_GOTO(0);')
    if disp:
        c.append(f'\tic -= {disp};')
    if uses('CCRL'):
        c.append('\tu8 CCRL = (u8)f;')
    if need_snap:
        c.append('\tuint64_t snap[4]; __builtin_memcpy(snap, S->R, 32);')
    c.append(body)
    if uses('CCRL'):
        c.append('\tf = CCRL;')
    if uses('NPC'):
        c.append('\t(void)NPC;')
    if branch:
        # taken (anywhere but the next instruction): target 1; else on with the next one
        c.append('\ta = PC;')
        c.append(f'\tif (a != (u32)HK_IMM(H5) + {2 * words}) HK_GOTO(1);')
    c.append('\tHK_NEXT;')
    if accesses:
        c.append('slow:')
        if need_snap:
            c.append('\t__builtin_memcpy(S->R, snap, 32);')
        c.append('\tic = ic0;')
        c.append('\tHK_GOTO(0);')
    c.append('}')
    return dict(name=name, code='\n'.join(c), words=words, cost=cost, branch=branch, regs=regs), None


STATIC = re.compile(r'^[\sIR0-9xa-fA-F<>&|+\-()~*]*$')


def static_registers(body, regs):
    """rN_r(S, E) and rN_w(S, E, V) with E made of the instruction's words alone: R8(Hn) and the
    others, n from 6 on, the hole's value (E's place) recorded in regs[(N, E)]."""
    out, i = [], 0
    pat = re.compile(r'\br(8|16|32)_([rw])\(S, ')
    while True:
        m = pat.search(body, i)
        if not m:
            out.append(body[i:])
            return ''.join(out)
        # the arguments, to the call's closing parenthesis
        j, depth, args, start = m.end(), 0, [], m.end()
        while True:
            ch = body[j]
            if ch == '(':
                depth += 1
            elif ch == ')':
                if depth == 0:
                    args.append(body[start:j])
                    break
                depth -= 1
            elif ch == ',' and depth == 0:
                args.append(body[start:j])
                start = j + 1
            j += 1
        width, rw, e = m.group(1), m.group(2), args[0].strip()
        key = (width, e)
        if STATIC.match(e) and 'IR' in e or re.fullmatch(r'\d+', e):
            if key not in regs and len(regs) < 6:
                regs[key] = 6 + len(regs)
        if key in regs:
            h = regs[key]
            if rw == 'r':
                rep = f'R{width}(H{h})'
            else:
                rep = f'R{width}W(H{h}, {static_registers(",".join(args[1:]), regs).strip()})'
            out.append(body[i:m.start()] + rep)
        else:   # (its arguments may still name registers by the words)
            out.append(body[i:m.end()] + static_registers(body[m.end():j], regs) + ')')
        i = j + 1


def decoder(ol):
    """MAME's do_exec_full (h8make.py save_exec) returning the instruction's index instead."""
    index = {id(o): i for i, o in enumerate(ol.opcode_info)}
    out = ['static int hk_h8_decode(const uint16_t *w)', '{',
           '\tuint32_t st = w[0];', '\tfor (;;) {', '\tswitch (st >> 16) {']

    def target(n):
        if n.is_dispatch():
            start = n.pos // 2
            end = start + n.skip
            return f'{{ st = 0x{n.id:x}0000u | w[{end}]; continue; }}'
        return f'return {index[id(n)]};'
    for i in range(0, len(ol.dispatch_info) + 2):
        if i == 1:
            continue
        if not (i == 0 or ol.dispatch_info[i - 2].enabled):
            continue
        out.append(f'\tcase 0x{i:02x}: {{')
        h = ol.get(i)
        out.append(f'\t\tswitch ((st >> 8) & 0x{h.mask:02x}) {{')
        for val, h2 in sorted(h.d.items()):
            if not h2.enabled:
                continue
            fmask = h2.premask | (h.mask ^ 0xff)
            c = ''
            s = 0
            while s < 0x100:
                c += 'case 0x%02x: ' % (val | s)
                s += 1
                while s & fmask:
                    s += s & fmask
            out.append(f'\t\t{c}{{')
            if h2.mask == 0x00:
                out.append('\t\t\t' + target(h2.d[0]))
            else:
                out.append(f'\t\t\tswitch (st & 0x{h2.mask:02x}) {{')
                mpos = 1 if i == 0 else ol.dispatch_info[i - 2].pos + 1
                for val2, n in sorted(h2.d.items()):
                    if not n.enabled:
                        continue
                    fm = h2.mask ^ 0xff
                    fm |= n.mask if n.is_dispatch() else n.mask[mpos]
                    c = ''
                    s = 0
                    while s < 0x100:
                        c += 'case 0x%02x: ' % (val2 | s)
                        s += 1
                        while s & fm:
                            s += s & fm
                    out.append(f'\t\t\t{c}{target(n)}')
                out.append('\t\t\tdefault: return -1;')
                out.append('\t\t\t}')
            out.append('\t\t}')
        out.append('\t\tdefault: return -1;')
        out.append('\t\t}')
        out.append('\t}')
    out += ['\tdefault: return -1;', '\t}', '\t}', '}']
    return out


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    h8dir, outdir = sys.argv[1], sys.argv[2]
    sys.path.insert(0, h8dir)
    import h8make
    ol = h8make.OpcodeList(os.path.join(h8dir, 'h8.lst'), 3)   # the H8S/2600
    ol.build_dispatch()
    h8cpp = open(os.path.join(h8dir, 'h8.cpp')).read()
    ops, rows, skipped = [], [], {}
    for i, op in enumerate(ol.opcode_info):
        if not op.enabled:
            rows.append(None)
            continue
        r, why = convert(op, i)
        if r is None:
            skipped[op.function_name()] = why
            words = len(op.val) // 2 + op.skip + op.extra_words
            rows.append(dict(op=op, name=op.function_name(), stencil=None, words=words, cost=0, branch=False))
            continue
        ops.append(r)
        rows.append(dict(op=op, name=op.function_name(), stencil=r['name'], words=r['words'], cost=r['cost'], branch=r['branch'], regs=r['regs']))
    os.makedirs(outdir, exist_ok=True)
    hdr = ['// Generated by tools/h8jitgen.py from MAME\'s h8.lst and h8.cpp. Do not edit.',
           f'// {len(ops)} instructions made into stencils; left to MAME\'s interpreter:',
           *[f'//   {k}: {v}' for k, v in sorted(skipped.items())], '']
    hdr += helpers(h8cpp)
    hdr += [o['code'] + '\n' for o in ops]
    tmp = os.path.join(outdir, 'hk_h8_ops.h')
    write_if_changed(tmp, '\n'.join(hdr))
    tab = ['// Generated by tools/h8jitgen.py from MAME\'s h8.lst. Do not edit.',
           '// the places of registers (h8_stencils.c: R8 and the others) in hk_h8_state::R',
           'static inline int64_t hk_h8_r8(uint32_t r) { return 2 * (r & 7) + ((r & 8) ? 0 : 1); }   // (a little-endian host)',
           'static inline int64_t hk_h8_r16(uint32_t r) { return 2 * (r & 15); }',
           'static inline int64_t hk_h8_r32(uint32_t r) { return 2 * (r & 7); }']
    for r in rows:
        if r is None or not r['stencil'] or not r['regs']:
            continue
        fn = f'hk_h8_regs_{r["stencil"]}'
        r['fn'] = fn
        tab.append(f'static void {fn}(const uint16_t *w, int64_t *h) {{')
        tab.append('\tconst uint32_t IR0 = w[0], IR1 = w[1], IR2 = w[2], IR3 = w[3], IR4 = w[4];')
        tab.append('\t(void)IR0; (void)IR1; (void)IR2; (void)IR3; (void)IR4;')
        for (width, expr), h in sorted(r['regs'].items(), key=lambda kv: kv[1]):
            tab.append(f'\th[{h}] = hk_h8_r{width}((uint32_t)({expr}));')
        tab.append('}')
    tab += [
           '// the opcode (its words but the skipped one: val/mask, nval words) for tests\n'
           'struct hk_h8_op { const char *name; int stencil; uint8_t words, cost, branch, nval, skip; uint16_t val[3], mask[3]; void (*regs)(const uint16_t *, int64_t *); };',
           'static const hk_h8_op hk_h8_ops[] = {']
    for r in rows:
        if r is None:
            tab.append('\t{nullptr, -1, 0, 0, 0, 0, 0, {}, {}, nullptr},')
        else:
            st = f'hk::jit::h8::ST_{r["stencil"]}' if r['stencil'] else '-1'
            o = r['op']
            nv = len(o.val) // 2
            val = ', '.join(f'0x{(o.val[2*k] << 8) | o.val[2*k+1]:04x}' for k in range(nv))
            mask = ', '.join(f'0x{(o.mask[2*k] << 8) | o.mask[2*k+1]:04x}' for k in range(nv))
            tab.append(f'\t{{"{r["name"]}", {st}, {r["words"]}, {r["cost"]}, {int(r["branch"])}, {nv}, {o.skip}, {{{val}}}, {{{mask}}}, {r.get("fn", "nullptr")}}},')
    tab.append('};')
    tab += decoder(ol)
    write_if_changed(os.path.join(outdir, 'hk_h8_table.inc'), '\n'.join(tab) + '\n')


def write_if_changed(path, text):
    try:
        if open(path).read() == text:
            return
    except OSError:
        pass
    with open(path, 'w') as f:
        f.write(text)


if __name__ == '__main__':
    main()
