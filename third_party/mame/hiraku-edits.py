#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Apply HIRAKU's edits to pristine MAME sources (third_party/mame/README.md).

    third_party/mame/hiraku-edits.py SRC_DEVICES_DIR

SRC_DEVICES_DIR is a MAME src/devices tree, or third_party/mame/src/devices
holding fresh copies of the files listed in the README (and minimp3.h in the
src/3rdparty next to it). The edits are applied
in place. Every anchor must be found exactly once, so the script fails loudly
when upstream changes the code an edit relies on. The H8 files need no edits.
"""
import sys
from pathlib import Path


def edit(text, old, new, name):
    n = text.count(old)
    if n != 1:
        sys.exit(f"hiraku-edits: {name}: anchor found {n} times, expected once:\n{old[:200]}")
    return text.replace(old, new)


def cut(text, start, end, replacement, name):
    """Replace [start, end) (end itself is kept) with replacement."""
    a = text.find(start)
    b = text.find(end, a + 1) if a >= 0 else -1
    if a < 0 or b < 0 or text.count(start) != 1:
        sys.exit(f"hiraku-edits: {name}: span not found")
    return text[:a] + replacement + text[b:]


STEP_DECODED = r'''// HIRAKU: the MEG program pre-decoded. step() extracts every field of every
// instruction on every one of 17 M steps a second; decode() does it when a
// program word changes and step_decoded() is step() working from the fields:
// the multiplier's operands through pointers chosen at decode time, and the
// pipeline's 3- and 2-step delay slots as template constants (run_decoded()
// cycles through the 6 phases) instead of counters wrapped on every step.
// HK_XT446_MEG_REF=1 runs step() instead, to check the two agree.
void swp30_device::meg_state::decode()
{
	for(int pc = 0; pc != 0x180; pc++) {
		u64 opcode = m_program[pc];
		decoded &d = m_dec[pc];
		d.sm = BIT(opcode, 0x04, 6);
		d.sr = BIT(opcode, 0x0b, 7);
		d.dm = BIT(opcode, 0x27, 6);
		d.dr = BIT(opcode, 0x30, 7);
		d.t  = BIT(opcode, 0x38, 3);
		d.mmode = BIT(opcode, 0x16, 2);
		d.alu = d.mmode != 0 || BIT(opcode, 0x1a, 6);
		u32 m1t = BIT(opcode, 0x14, 2);
		d.m1_from_t = m1t == 1 || m1t == 2;
		d.m1_expand = BIT(opcode, 0x13);
		d.m2_from_m = BIT(opcode, 0x12);
		d.m1p = d.m1_from_t ? &m_t[d.t] : &m_const[pc];
		d.m2p = d.m2_from_m ? &m_m[d.sm] : &m_r[d.sr];
		d.amode = BIT(opcode, 0x18, 2);
		d.op = BIT(opcode, 0x1a, 2);
		u32 shift = BIT(opcode, 0x1c, 2);
		d.shift = shift == 3 ? 4 : shift;
		d.sat = BIT(opcode, 0x1e, 2);
		d.dsrc = BIT(opcode, 0x2d, 3);
		d.add_rand = !BIT(opcode, 0x0a);
		d.r_from_r = BIT(opcode, 0x37);
		d.memw = BIT(opcode, 0x3d);
		d.idx = BIT(opcode, 0x3e);
		d.twr = BIT(opcode, 0x3b, 1);
		d.t_from_value = BIT(opcode, 0x3c);
		d.memop = BIT(opcode, 0x24, 2);
		d.use_index = BIT(opcode, 0x21);
		d.absolute = BIT(opcode, 0x23);
	}
	m_dec_dirty = false;

	// the compiled code for this program, if tools/meggen.py made some (FNV-1a over the words)
	u64 h = 0xcbf29ce484222325ULL;
	for(int pc = 0; pc != 0x180; pc++)
		for(int b = 0; b != 8; b++) {
			h ^= (m_program[pc] >> (8 * b)) & 0xff;
			h *= 0x100000001b3ULL;
		}
	m_prog_hash = h;
	m_compiled = nullptr;
	for(int i = 0; i != s_program_count; i++)   // the table is at the end of the file
		if(s_programs[i].hash == h) {
			m_compiled = s_programs[i].run;
			m_compiled_k0 = s_programs[i].k0;
			break;
		}
	m_record_samples = 0;
	m_compiled_armed = false;   // the delay slots still hold the old program's steps
}

template<int D3, int D2> inline __attribute__((always_inline)) void swp30_device::meg_state::step_decoded(s64 &p)
{
	if(m_mw_reg[D3])
		m_m[m_mw_reg[D3]] = m_mw_value[D3];

	if(m_rw_reg[D3])
		m_r[m_rw_reg[D3]] = m_rw_value[D3];

	if(m_index_active[D3])
		m_ram_index = m_index_value[D3];

	if(m_memw_active[D2]) {
		m_ram_write = m_memw_value[D2];
		m_memw_active[D2] = false;
	}
	if(m_memr_active[D2]) {
		m_ram_read = m_memr_value[D2];
		m_memr_active[D2] = false;
	}

	const decoded &d = m_dec[m_pc];
	const int sm = d.sm, sr = d.sr, dm = d.dm, dr = d.dr, t = d.t;

	if(d.alu) {
		s64 m1 = *d.m1p;
		if(d.m1_expand)
			m1 = m1_expand(m1);

		s64 m2 = *d.m2p;

		s64 m;
		switch(d.mmode) {
		case 0: m = 0; break;
		case 1: m = m1 << (8+15); break;
		case 2: m = m1 * m2; break;
		default: m = m2 << 15; break;
		}

		s64 a;
		switch(d.amode) {
		case 0: a = p; break;
		case 1: a = sr ? (s64(m_r[sr]) << 15) : (p >> 15); break;
		case 2: a = sm ? (s64(m_m[sm]) << 15) : (p >> 15); break;
		default: a = 0; break;
		}

		s64 r;
		switch(d.op) {
		case 0: r = m + a; break;
		case 1: r = m - a; break;
		case 2: r = m + ((a < 0) ? -a : a); break;
		default: r = m & a; break;
		}

		r <<= d.shift;

		switch(d.sat) {
		case 0: r = util::sext(r, 42); break;
		case 1: r = std::clamp<s64>(r, -0x4000000000, 0x3fffffffff); break;
		case 2: r = std::clamp<s64>(r, 0, 0x3fffffffff); break;
		default: r = std::min<s64>((r < 0) ? -r : r, 0x3fffffffff); break;
		}

		p = r;
	}

	m_mw_reg[D3] = dm;
	if(dm) {
		u32 v;
		switch(d.dsrc) {
		case 0: case 1: case 2: case 3:
			v = get_lfo(m_pc >> 4);
			break;
		case 4: v = m_ram_read; break;
		case 5: v = m_swp->machine().rand() & 0xffffff; if(v & 0x00800000) v |= 0xff000000; break;
		case 6: {
			s64 pr = p;
			if(d.add_rand)
				pr += m_swp->machine().rand() & 0x07e0;
			v = pack24(pr);
			break;
		}
		default: v = m_m[sm]; break;
		}
		m_mw_value[D3] = v;
	}

	m_rw_reg[D3] = dr;
	if(dr) {
		u32 v;
		if(d.r_from_r)
			v = m_r[sr];
		else {
			s64 pr = p;
			if(d.add_rand)
				pr += m_swp->machine().rand() & 0x07e0;
			v = pack24(pr);
		}
		m_rw_value[D3] = v;
	}

	if(d.memw) {
		m_memw_active[D2] = true;
		m_memw_value[D2] = p / 32768;
	} else
		m_memw_active[D2] = false;

	if(d.idx) {
		m_index_active[D3] = true;
		m_index_value[D3] = p >> (15+8);
	} else
		m_index_active[D3] = false;

	if(d.twr) {
		if(d.t_from_value)
			m_t[t] = m_t_value[D2];
		else
			m_t[t] = m_const[m_pc];
	}
	m_t_value[D2] = s16(d.idx ? (p >> 8) & 0x7fff : std::clamp<s64>(p >> (15+8), -0x8000, 0x7fff));

	switch(d.memop) {
	case 0:
		break;
	case 1:
		if(!BIT(m_swp->m_revram_enable, map_bank(m_pc))) {
			u32 address = resolve_address(m_pc, m_offset[m_pc/3] + (d.use_index ? m_ram_index : 0) - m_sample_counter);
			if(address != 0xffffffff)
				m_swp->m_reverb_cache.write_word(address, revram_encode(m_ram_write));
		}
		break;
	default: {
		const s32 plus = d.memop == 3 ? 1 : 0;
		if(!d.absolute && BIT(m_swp->m_revram_enable, map_bank(m_pc))) {
			m_memr_value[D2] = 0;
			m_memr_active[D2] = true;
		} else {
			u32 address = d.absolute ?
				(m_offset[m_pc/3] + (d.use_index ? m_ram_index : 0) + plus) & 0x3ffff :
				resolve_address(m_pc, m_offset[m_pc/3] + (d.use_index ? m_ram_index : 0) - m_sample_counter + plus);
			if(address != 0xffffffff) {
				u16 val = m_swp->m_reverb_cache.read_word(address);
				m_memr_value[D2] = revram_decode(val);
				m_memr_active[D2] = true;
			}
		}
		break;
	}
	}

	m_pc ++;
}

// n steps from m_pc, not past the end of the program (the caller wraps m_pc)
void swp30_device::meg_state::run_decoded(int n)
{
	// phase k: D3 = k % 3, D2 = k % 2
	static constexpr int phase_of[3][2] = {{0, 3}, {4, 1}, {2, 5}};
	int k = phase_of[m_delay_3][m_delay_2];
	m_icount -= n;
	s64 p = m_p;   // in a register for the whole run
	while(n) {
		switch(k) {
		case 0: step_decoded<0, 0>(p); if(!--n) { k = 1; break; } [[fallthrough]];
		case 1: step_decoded<1, 1>(p); if(!--n) { k = 2; break; } [[fallthrough]];
		case 2: step_decoded<2, 0>(p); if(!--n) { k = 3; break; } [[fallthrough]];
		case 3: step_decoded<0, 1>(p); if(!--n) { k = 4; break; } [[fallthrough]];
		case 4: step_decoded<1, 0>(p); if(!--n) { k = 5; break; } [[fallthrough]];
		case 5: step_decoded<2, 1>(p); --n; k = 0; break;
		}
	}
	m_p = p;
	m_delay_3 = k % 3;
	m_delay_2 = k % 2;
}

// HIRAKU: MEG programs compiled ahead of time. The firmware loads a handful of
// effect programs; HK_MEG_RECORD=file appends each one that has run unchanged
// for 256 samples, with its pipeline phase, and tools/meggen.py turns the
// recording into hk_meg_programs.inc: per program, run_prog<hash, k0>() is the
// 384 steps as step_fixed() calls, a step_decoded() whose fields, program
// counter and delay slots are compile-time constants. decode() looks the
// program up by its hash; a run uses the compiled code only when the pipeline
// phase is the one it was compiled for, the interpreter otherwise.
static constexpr u64 hk_f(u64 op, int start, int len) { return (op >> start) & ((u64(1) << len) - 1); }

template<int D3, int D2, int PC, u64 OP, u64 OP3, u64 OP2> inline __attribute__((always_inline)) void swp30_device::meg_state::step_fixed(s64 &p)
{
	// what the steps 3 and 2 before this one (OP3, OP2: the same program's, from the previous
	// sample for the first steps) left in the delay slots: known here, so not tested at run time
	constexpr int dm3 = hk_f(OP3, 0x27, 6), dr3 = hk_f(OP3, 0x30, 7);
	constexpr bool idx3 = hk_f(OP3, 0x3e, 1), memw2 = hk_f(OP2, 0x3d, 1), memr2 = hk_f(OP2, 0x24, 2) >= 2;
	constexpr int sm = hk_f(OP, 0x04, 6), sr = hk_f(OP, 0x0b, 7), dm = hk_f(OP, 0x27, 6), dr = hk_f(OP, 0x30, 7), t = hk_f(OP, 0x38, 3);
	constexpr int mmode = hk_f(OP, 0x16, 2);
	constexpr bool alu = mmode != 0 || hk_f(OP, 0x1a, 6);
	constexpr int m1t = hk_f(OP, 0x14, 2);
	constexpr bool m1_from_t = m1t == 1 || m1t == 2, m1_exp = hk_f(OP, 0x13, 1), m2_from_m = hk_f(OP, 0x12, 1);
	constexpr int amode = hk_f(OP, 0x18, 2), op = hk_f(OP, 0x1a, 2), shift = hk_f(OP, 0x1c, 2) == 3 ? 4 : hk_f(OP, 0x1c, 2);
	constexpr int sat = hk_f(OP, 0x1e, 2), dsrc = hk_f(OP, 0x2d, 3), memop = hk_f(OP, 0x24, 2);
	constexpr bool add_rand = !hk_f(OP, 0x0a, 1), r_from_r = hk_f(OP, 0x37, 1), memw = hk_f(OP, 0x3d, 1), idx = hk_f(OP, 0x3e, 1);
	constexpr bool twr = hk_f(OP, 0x3b, 1), t_from_value = hk_f(OP, 0x3c, 1), use_index = hk_f(OP, 0x21, 1), absolute = hk_f(OP, 0x23, 1);

	if constexpr(dm3 != 0)
		m_m[dm3] = m_mw_value[D3];

	if constexpr(dr3 != 0)
		m_r[dr3] = m_rw_value[D3];

	if constexpr(idx3)
		m_ram_index = m_index_value[D3];

	if constexpr(memw2) {
		m_ram_write = m_memw_value[D2];
		m_memw_active[D2] = false;
	}
	if constexpr(memr2) {   // set only if the address was valid (or the revram path): still tested
		if(m_memr_active[D2]) {
			m_ram_read = m_memr_value[D2];
			m_memr_active[D2] = false;
		}
	}

	if constexpr(alu) {
		s64 m;
		if constexpr(mmode == 0)
			m = 0;
		else if constexpr(mmode == 3)
			m = s64(m2_from_m ? m_m[sm] : m_r[sr]) << 15;
		else {
			s64 m1 = m1_from_t ? m_t[t] : m_const[PC];
			if constexpr(m1_exp)
				m1 = m1_expand(m1);
			if constexpr(mmode == 1)
				m = m1 << (8+15);
			else
				m = m1 * s64(m2_from_m ? m_m[sm] : m_r[sr]);
		}

		s64 a;
		if constexpr(amode == 0) a = p;
		else if constexpr(amode == 1) a = sr ? (s64(m_r[sr]) << 15) : (p >> 15);
		else if constexpr(amode == 2) a = sm ? (s64(m_m[sm]) << 15) : (p >> 15);
		else a = 0;

		s64 r;
		if constexpr(op == 0) r = m + a;
		else if constexpr(op == 1) r = m - a;
		else if constexpr(op == 2) r = m + ((a < 0) ? -a : a);
		else r = m & a;

		r <<= shift;

		if constexpr(sat == 0) r = util::sext(r, 42);
		else if constexpr(sat == 1) r = std::clamp<s64>(r, -0x4000000000, 0x3fffffffff);
		else if constexpr(sat == 2) r = std::clamp<s64>(r, 0, 0x3fffffffff);
		else r = std::min<s64>((r < 0) ? -r : r, 0x3fffffffff);

		p = r;
	}

	m_mw_reg[D3] = dm;
	if constexpr(dm != 0) {
		u32 v;
		if constexpr(dsrc <= 3)
			v = get_lfo(PC >> 4);
		else if constexpr(dsrc == 4)
			v = m_ram_read;
		else if constexpr(dsrc == 5) {
			v = m_swp->machine().rand() & 0xffffff;
			if(v & 0x00800000)
				v |= 0xff000000;
		} else if constexpr(dsrc == 6) {
			s64 pr = p;
			if constexpr(add_rand)
				pr += m_swp->machine().rand() & 0x07e0;
			v = pack24(pr);
		} else
			v = m_m[sm];
		m_mw_value[D3] = v;
	}

	m_rw_reg[D3] = dr;
	if constexpr(dr != 0) {
		u32 v;
		if constexpr(r_from_r)
			v = m_r[sr];
		else {
			s64 pr = p;
			if constexpr(add_rand)
				pr += m_swp->machine().rand() & 0x07e0;
			v = pack24(pr);
		}
		m_rw_value[D3] = v;
	}

	if constexpr(memw) {
		m_memw_active[D2] = true;
		m_memw_value[D2] = p / 32768;
	} else
		m_memw_active[D2] = false;

	if constexpr(idx) {
		m_index_active[D3] = true;
		m_index_value[D3] = p >> (15+8);
	} else
		m_index_active[D3] = false;

	if constexpr(twr) {
		if constexpr(t_from_value)
			m_t[t] = m_t_value[D2];
		else
			m_t[t] = m_const[PC];
	}
	m_t_value[D2] = s16(idx ? (p >> 8) & 0x7fff : std::clamp<s64>(p >> (15+8), -0x8000, 0x7fff));

	if constexpr(memop == 1) {
		if(!BIT(m_swp->m_revram_enable, map_bank(PC))) {
			u32 address = resolve_address(PC, m_offset[PC/3] + (use_index ? m_ram_index : 0) - m_sample_counter);
			if(address != 0xffffffff)
				m_swp->m_reverb_cache.write_word(address, revram_encode(m_ram_write));
		}
	} else if constexpr(memop >= 2) {
		constexpr s32 plus = memop == 3 ? 1 : 0;
		if(!absolute && BIT(m_swp->m_revram_enable, map_bank(PC))) {
			m_memr_value[D2] = 0;
			m_memr_active[D2] = true;
		} else {
			u32 address = absolute ?
				(m_offset[PC/3] + (use_index ? m_ram_index : 0) + plus) & 0x3ffff :
				resolve_address(PC, m_offset[PC/3] + (use_index ? m_ram_index : 0) - m_sample_counter + plus);
			if(address != 0xffffffff) {
				u16 val = m_swp->m_reverb_cache.read_word(address);
				m_memr_value[D2] = revram_decode(val);
				m_memr_active[D2] = true;
			}
		}
	}
}

// n steps from m_pc with the compiled program, if it is there and the phase is right
bool swp30_device::meg_state::run_compiled(int n)
{
	static constexpr int phase_of[3][2] = {{0, 3}, {4, 1}, {2, 5}};
	int k = phase_of[m_delay_3][m_delay_2];
	if(!m_compiled || !m_compiled_armed || k != (m_compiled_k0 + m_pc) % 6)
		return false;
	m_icount -= n;
	s64 p = m_p;
	(this->*m_compiled)(p, m_pc, n);
	m_p = p;
	m_pc += n;
	k = (k + n) % 6;
	m_delay_3 = k % 3;
	m_delay_2 = k % 2;
	return true;
}

// at pc 0: a program that has run unchanged for 256 samples goes to HK_MEG_RECORD; and its
// compiled code may run from here (the delay slots now hold this program's last steps)
void swp30_device::meg_state::record_program()
{
	m_compiled_armed = true;
	static FILE *f = getenv("HK_MEG_RECORD") ? fopen(getenv("HK_MEG_RECORD"), "a") : nullptr;
	if(!f || m_compiled || m_record_samples < 0 || ++m_record_samples < 256)
		return;
	static constexpr int phase_of[3][2] = {{0, 3}, {4, 1}, {2, 5}};
	fprintf(f, "%016llx %d", (unsigned long long)m_prog_hash, phase_of[m_delay_3][m_delay_2]);
	for(int pc = 0; pc != 0x180; pc++)
		fprintf(f, " %llx", (unsigned long long)m_program[pc]);
	fprintf(f, "\n");
	fflush(f);
	m_record_samples = -1;   // once per program
}

'''


def swp30_cpp(s):
    s = edit(s, '#include "cpu/drcumlsh.h"\n\n#include "debugger.h"\n#include "emuopts.h"\n',
             '// HIRAKU: no DRC back-end (the MEG runs on the interpreter), no debugger/options headers\n', 'includes')
    s = edit(s, ''',
	  m_drccache(32*1024*1024 + sizeof(meg_state))
{''', '''
{''', 'drccache member init')
    s = edit(s, '''	m_drccache.allocate_cache(mconfig().options().drc_rwx());
	m_meg = m_drccache.alloc_near<meg_state>();''', '''	m_meg = new meg_state();   // HIRAKU: no DRC cache to allocate it in''', 'meg allocation')
    s = cut(s, '	// SWP30 compiles the entire MEG program as one block', '	m_meg_program_changed = true;\n\tm_meg_drc_active',
            '', 'drcuml setup')
    s = edit(s, '	m_meg_drc_active = allow_drc();', '	m_meg_drc_active = false;   // HIRAKU', 'drc active')
    s = cut(s, '// pack24 inline, result in I0', 'void swp30_device::meg_state::step()',
            '// HIRAKU: the UML code generator (drc_pack24, drc_t_value, drc) removed\n\n', 'drc code generator')
    # the program fetch: what meg_prg_map_r returns, without going through the bus
    s = edit(s, '	u64 opcode = m_swp->m_program_cache.read_qword(m_pc);',
             '	u64 opcode = m_program[m_pc];   // HIRAKU: what m_program_cache.read_qword(m_pc) returns (meg_prg_map_r), without the bus',
             'program fetch')
    s = edit(s, '''	m_program[m_program_address] = (m_program[m_program_address] & mask) | (u64(data) << shift);
''', '''	m_program[m_program_address] = (m_program[m_program_address] & mask) | (u64(data) << shift);
	m_dec_dirty = true;   // HIRAKU
''', 'prg_w')
    s = edit(s, '''	m_sample_counter = 0;
	m_retval = 0;
}''', '''	m_sample_counter = 0;
	m_retval = 0;
	m_dec_dirty = true;   // HIRAKU
}''', 'meg reset')
    a = s.find('void swp30_device::execute_run()\n{')
    b = s.find('void swp30_device::adc_step()')
    if a < 0 or b < 0:
        sys.exit('hiraku-edits: execute_run not found')
    s = s[:a] + STEP_DECODED + '''void swp30_device::execute_run()
{
	// HIRAKU: the interpreter path only, from the pre-decoded program
	static const bool reference = getenv("HK_XT446_MEG_REF") != nullptr;
	m_meg_program_changed = false;
	if(m_meg->m_dec_dirty)
		m_meg->decode();

	while(m_meg->m_icount > 0) {
		if(m_meg->m_pc == 0) {
			sample_step();
			m_meg->record_program();
		}
		if(reference)
			m_meg->step();
		else {
			const int n = std::min<int>(m_meg->m_icount, 0x180 - m_meg->m_pc);
			if(!m_meg->run_compiled(n))
				m_meg->run_decoded(n);
			if(m_meg->m_pc == 0x180)
				m_meg->m_pc = 0;
		}
	}
}

''' + s[b:]
    s = s + '''
// HIRAKU: the compiled MEG programs, generated by tools/meggen.py from a recording
#if __has_include("hk_meg_programs.inc")
#include "hk_meg_programs.inc"
#else
const swp30_device::meg_state::compiled_prog swp30_device::meg_state::s_programs[1] = {};
const int swp30_device::meg_state::s_program_count = 0;
#endif
'''
    s = edit(s, '''	m_meg->lfo_step();
	m_meg->m_sample_counter ++;
}''', '''	m_meg->lfo_step();
	m_meg->m_sample_counter ++;
	if(hk_on_sample)
		hk_on_sample(m_adc);   // HIRAKU: the DAC outputs of this sample
}''', 'sample hook')
    return s


def swp30_h(s):
    s = edit(s, '#include "cpu/drcuml.h"\n', '// HIRAKU: no DRC (cpu/drcuml.h)\n#include <functional>\n', 'include')
    s = edit(s, '''	void map(address_map &map) ATTR_COLD;

protected:''', '''	void map(address_map &map) ATTR_COLD;

	// HIRAKU: called after each sample with the four DAC outputs (MAME's stream outputs 0-3)
	std::function<void(const std::array<s32, 4> &)> hk_on_sample;

protected:''', 'hook member')
    s = edit(s, '''		void drc(drcuml_block &block, u16 pc);
		void drc_pack24(drcuml_block &block, bool dither, uml::code_label label);
		void drc_t_value(drcuml_block &block, u32 index2);
''', '''		// HIRAKU: the program pre-decoded (see step_decoded() in swp30.cpp)
		struct decoded {
			u8 sm, sr, dm, dr, t;
			u8 mmode, alu, m1_from_t, m1_expand, m2_from_m, amode, op, shift, sat;
			u8 dsrc, add_rand, r_from_r, memw, idx, twr, t_from_value, memop, use_index, absolute;
			const s16 *m1p;   // the multiplier's operands: m_t[t] or m_const[pc], and m_m[sm] or m_r[sr]
			const s32 *m2p;
		};
		std::array<decoded, 0x180> m_dec;
		bool m_dec_dirty = true;
		void decode();
		template<int D3, int D2> void step_decoded(s64 &p);
		void run_decoded(int n);
		// HIRAKU: programs compiled ahead of time from a recording (tools/meggen.py)
		using prog_fn = void (meg_state::*)(s64 &p, int pc, int n);
		template<int D3, int D2, int PC, u64 OP, u64 OP3, u64 OP2> void step_fixed(s64 &p);
		template<u64 HASH, int K0> void run_prog(s64 &p, int pc, int n);
		struct compiled_prog { u64 hash; int k0; prog_fn run; };
		static const compiled_prog s_programs[];   // generated (hk_meg_programs.inc)
		static const int s_program_count;
		prog_fn m_compiled = nullptr;   // the current program's compiled code, if any
		int m_compiled_k0 = 0;          // the pipeline phase at pc 0 it was compiled for
		bool m_compiled_armed = false;  // false from a program change to the next sample's start
		u64 m_prog_hash = 0;
		int m_record_samples = 0;       // samples the program has run unchanged; -1: recorded
		bool run_compiled(int n);
		void record_program();
''', 'drc decls')
    s = edit(s, '''	drc_cache m_drccache;
	std::unique_ptr<drcuml_state> m_drcuml;
	uml::code_handle *m_meg_drc_entry;
	uml::code_handle *m_meg_drc_nocode;
	uml::code_handle *m_meg_drc_out_of_cycles;
''', '''	// HIRAKU: DRC state removed
''', 'drc members')
    return s


# the interpreter's small steps compiled into each instruction's case (psx_cpp, below): the edits
# to MAME's psx.cpp after the ones above, as (what is there, what it becomes, a name)
PSX_HOT = [
    ('#include "emu.h"\n#include "psx.h"\n',
     '''#include "emu.h"
#include "psx.h"

// HIRAKU: the interpreter's small steps (the pipeline, the bus) compiled into each instruction's
// case, and the instruction's body into execute_run()'s loop: MAME calls them, a dozen host
// instructions each time for the call alone. The same code in the same order.
#define HK_HOT __attribute__((always_inline)) inline
''', 'HK_HOT'),
    ('\nuint8_t psxcpu_device::readbyte( uint32_t address )\n{',
     '\nHK_HOT uint8_t psxcpu_device::readbyte( uint32_t address )\n{', 'hot readbyte'),
    ('\nuint16_t psxcpu_device::readhalf( uint32_t address )\n{',
     '\nHK_HOT uint16_t psxcpu_device::readhalf( uint32_t address )\n{', 'hot readhalf'),
    ('\nuint32_t psxcpu_device::readword( uint32_t address )\n{',
     '\nHK_HOT uint32_t psxcpu_device::readword( uint32_t address )\n{', 'hot readword'),
    ('\nuint32_t psxcpu_device::readword_masked( uint32_t address, uint32_t mask )\n{',
     '\nHK_HOT uint32_t psxcpu_device::readword_masked( uint32_t address, uint32_t mask )\n{', 'hot readword_masked'),
    ('\nvoid psxcpu_device::writeword( uint32_t address, uint32_t data )\n{',
     '\nHK_HOT void psxcpu_device::writeword( uint32_t address, uint32_t data )\n{', 'hot writeword'),
    ('\nvoid psxcpu_device::writeword_masked( uint32_t address, uint32_t data, uint32_t mask )\n{',
     '\nHK_HOT void psxcpu_device::writeword_masked( uint32_t address, uint32_t data, uint32_t mask )\n{', 'hot writeword_masked'),
    ('\nvoid psxcpu_device::commit_delayed_load()\n{',
     '\nHK_HOT void psxcpu_device::commit_delayed_load()\n{', 'hot commit_delayed_load'),
    ('\nvoid psxcpu_device::advance_pc()\n{',
     '\nHK_HOT void psxcpu_device::advance_pc()\n{', 'hot advance_pc'),
    ('\nvoid psxcpu_device::load( uint32_t reg, uint32_t value )\n{',
     '\nHK_HOT void psxcpu_device::load( uint32_t reg, uint32_t value )\n{', 'hot load'),
    ('\nvoid psxcpu_device::delayed_load( uint32_t reg, uint32_t value )\n{',
     '\nHK_HOT void psxcpu_device::delayed_load( uint32_t reg, uint32_t value )\n{', 'hot delayed_load'),
    ('\nvoid psxcpu_device::branch( uint32_t address )\n{',
     '\nHK_HOT void psxcpu_device::branch( uint32_t address )\n{', 'hot branch'),
    ('\nvoid psxcpu_device::conditional_branch( int takeBranch )\n{',
     '\nHK_HOT void psxcpu_device::conditional_branch( int takeBranch )\n{', 'hot conditional_branch'),
    ('\nvoid psxcpu_device::unconditional_branch()\n{',
     '\nHK_HOT void psxcpu_device::unconditional_branch()\n{', 'hot unconditional_branch'),
    ('\nint psxcpu_device::data_address_breakpoint( int dcic_rw, int dcic_status, uint32_t address )\n{',
     '\nHK_HOT int psxcpu_device::data_address_breakpoint( int dcic_rw, int dcic_status, uint32_t address )\n{', 'hot data_address_breakpoint'),
    ('\nint psxcpu_device::program_counter_breakpoint()\n{',
     '\nHK_HOT int psxcpu_device::program_counter_breakpoint()\n{', 'hot program_counter_breakpoint'),
    ('\nint psxcpu_device::load_data_address_breakpoint( uint32_t address )\n{',
     '\nHK_HOT int psxcpu_device::load_data_address_breakpoint( uint32_t address )\n{', 'hot load_data_address_breakpoint'),
    ('\nint psxcpu_device::store_data_address_breakpoint( uint32_t address )\n{',
     '\nHK_HOT int psxcpu_device::store_data_address_breakpoint( uint32_t address )\n{', 'hot store_data_address_breakpoint'),
    ('''// HIRAKU: one instruction, the body of MAME's execute_run() loop (without the m_icount--)
void psxcpu_device::execute_one()
{
''',
     '''// HIRAKU: one instruction, the body of MAME's execute_run() loop (without the m_icount--)
HK_HOT void psxcpu_device::hk_one()
{
''', 'hk_one head'),
    ('''	}
}

void psxcpu_device::execute_run()
{
	// HIRAKU: a waiting loop's turns taken at once (runtime/sys573/idle.cpp)
''',
     '''	}
}

void psxcpu_device::execute_one()
{
	hk_one();
}

void psxcpu_device::execute_run()
{
	// HIRAKU: a waiting loop's turns taken at once (runtime/sys573/idle.cpp)
''', 'execute_one wrapper'),
    ('''		execute_one();
		m_icount--;
	} while( m_icount > 0 );
}
''',
     '''		hk_one();
		m_icount--;
	} while( m_icount > 0 );
}
''', 'hk_one in the loop'),
]


def psx_cpp(s):
    # the loop body becomes execute_one(), so a recompiled block can hand one instruction back to
    # the interpreter; execute_run() runs blocks where it has them (runtime/sys573/mips.cpp)
    s = edit(s, '''void psxcpu_device::execute_run()
{
	do
	{
		if( LOG_BIOSCALL ) log_bioscall();
''', '''// HIRAKU: one instruction, the body of MAME's execute_run() loop (without the m_icount--)
void psxcpu_device::execute_one()
{
	{
		if( LOG_BIOSCALL ) log_bioscall();
''', 'execute_one head')
    s = edit(s, '''		m_icount--;
	} while( m_icount > 0 );
}
''', '''	}
}

void psxcpu_device::execute_run()
{
	// HIRAKU: a waiting loop's turns taken at once (runtime/sys573/idle.cpp)
	if( m_hk_idle && m_hk_idle( *this ) )
		return;

	do
	{
		// HIRAKU: recompiled blocks (runtime/sys573/mips.cpp). Outside a branch delay slot, a
		// block compiled from the code at m_pc runs in place of the interpreter when there is
		// one; it takes its own cycles off m_icount. m_hk_rec records where blocks start.
		if( m_hk_rec ) m_hk_rec( *this );
		if( m_hk_tab && m_delayr < PSXCPU_DELAYR_PC )
		{
			const void *b = m_hk_tab[ ( m_pc >> 2 ) & 0x3fffff ];
			if( b && m_hk_run( *this, b ) )
				continue;
		}

		execute_one();
		m_icount--;
	} while( m_icount > 0 );
}
''', 'execute_run loop')
    # SIO1 as a UART (sio_uart.h) for 573 boards with a multi-session unit; MAME's otherwise
    s = edit(s, '''		map(0x1f801050 | i, 0x1f80105f | i).rw("sio1", FUNC(psxsio_device::read), FUNC(psxsio_device::write));
''', '''		if( hk_psx_sio1_uart ) // HIRAKU
			map(0x1f801050 | i, 0x1f80105f | i).rw("sio1", FUNC(psxsio1_uart_device::read), FUNC(psxsio1_uart_device::write));
		else
			map(0x1f801050 | i, 0x1f80105f | i).rw("sio1", FUNC(psxsio_device::read), FUNC(psxsio_device::write));
''', 'sio1 map')
    s = edit(s, '''	auto &sio1(PSX_SIO1(config, "sio1", DERIVED_CLOCK(1, 2)));
	sio1.irq_handler().set("irq", FUNC(psxirq_device::intin8));
}
''', '''	if( hk_psx_sio1_uart ) // HIRAKU
	{
		auto &sio1(PSX_SIO1_UART(config, "sio1", DERIVED_CLOCK(1, 2)));
		sio1.irq_handler().set("irq", FUNC(psxirq_device::intin8));
	}
	else
	{
		auto &sio1(PSX_SIO1(config, "sio1", DERIVED_CLOCK(1, 2)));
		sio1.irq_handler().set("irq", FUNC(psxirq_device::intin8));
	}
}

// HIRAKU: set before the machine is configured (runtime/sys573/board.cpp)
bool hk_psx_sio1_uart = false;
''', 'sio1 config')
    # the interpreter's small steps inlined into each instruction's case, and the instruction's
    # body into execute_run()'s loop (execute_one() stays, for the blocks and the waiting loops)
    for old, new, name in PSX_HOT:
        s = edit(s, old, new, name)
    return s


def psx_h(s):
    s = edit(s, '''	virtual void execute_run() override;
''', '''	virtual void execute_run() override;
	void execute_one(); // HIRAKU
	void hk_one(); // HIRAKU: its body, inlined into execute_run()
''', 'execute_one decl')
    s = edit(s, '''#include "sio.h"
''', '''#include "sio.h"
#include "sio_uart.h" // HIRAKU

// HIRAKU: SIO1 as a UART (sio_uart.h) when set, before the machine is configured: 573 boards with
// a multi-session unit
extern bool hk_psx_sio1_uart;
''', 'sio_uart include')
    s = edit(s, '''	gte m_gte;
''', '''	gte m_gte;

	// HIRAKU: recompiled blocks (runtime/sys573/mips.cpp): m_hk_tab is indexed by
	// (pc >> 2) & 0x3fffff, m_hk_run runs the block it holds (false: interpret instead)
	friend struct hk_psx;
public:
	const void *const *m_hk_tab = nullptr;
	bool ( *m_hk_run )( psxcpu_device &, const void * ) = nullptr;
	void ( *m_hk_rec )( psxcpu_device & ) = nullptr;
	// waiting loops counted instead of run (runtime/sys573/idle.cpp), at the start of a
	// timeslice: true when it used the timeslice up
	bool ( *m_hk_idle )( psxcpu_device & ) = nullptr;
protected:
''', 'hk members')
    return s


def mdec_cpp(s):
    # the IDCT over the nonzero coefficients only (most of a block's 64 are zero), from a transposed
    # table; the sums are the same integers in another order (unsigned, so they wrap as MAME's do)
    s = edit(s, '''void psxmdec_device::mdec_idct( int32_t *p_n_src, int32_t *p_n_dst )
{
''', '''void psxmdec_device::mdec_idct( int32_t *p_n_src, int32_t *p_n_dst )
{
	// HIRAKU: the same sums, over the nonzero coefficients
	{
		uint32_t n_acc[ DCTSIZE2 ] = {};
		for( uint32_t n_vu = 0; n_vu < DCTSIZE2; n_vu++ )
		{
			const uint32_t n_s = p_n_src[ n_vu ];
			if( n_s == 0 )
				continue;
			const int32_t *p_n_c = &m_hk_cos_t[ n_vu * DCTSIZE2 ];
			for( uint32_t n_yx = 0; n_yx < DCTSIZE2; n_yx++ )
				n_acc[ n_yx ] += n_s * (uint32_t)p_n_c[ n_yx ];
		}
		for( uint32_t n_yx = 0; n_yx < DCTSIZE2; n_yx++ )
			p_n_dst[ n_yx ] = (int32_t)n_acc[ n_yx ] >> ( MDEC_COS_PRECALC_BITS + 2 );
		return;
	}

''', 'mdec_idct')
    s = edit(s, '''						p_n_cos[ ( n_v * 8 ) + n_y ] ) >> ( 30 - MDEC_COS_PRECALC_BITS ) );
				}
			}
		}
	}
}
''', '''						p_n_cos[ ( n_v * 8 ) + n_y ] ) >> ( 30 - MDEC_COS_PRECALC_BITS ) );
				}
			}
		}
	}

	// HIRAKU: transposed, for mdec_idct()
	for( uint32_t n_yx = 0; n_yx < DCTSIZE2; n_yx++ )
		for( uint32_t n_vu = 0; n_vu < DCTSIZE2; n_vu++ )
			m_hk_cos_t[ n_vu * DCTSIZE2 + n_yx ] = p_n_cos_precalc[ n_yx * DCTSIZE2 + n_vu ];
}
''', 'mdec_cos_precalc')
    return s


def mdec_h(s):
    s = edit(s, '''	int32_t p_n_cos_precalc[ DCTSIZE2 * DCTSIZE2 ];
''', '''	int32_t p_n_cos_precalc[ DCTSIZE2 * DCTSIZE2 ];
	int32_t m_hk_cos_t[ DCTSIZE2 * DCTSIZE2 ]; // HIRAKU: p_n_cos_precalc transposed
''', 'cos_t')
    return s


def video_psx_cpp(s):
    # textured pixels: the level tables are (component << 8), computed instead of read from three
    # 128 KiB tables; and shading at 0x80 (raw textures, most sprites) gives the texel back unchanged
    # (each shade table maps level l at 0x80 to l), so the shade tables are skipped then.
    s = edit(s, """#define SHADEDPIXEL( PIXELUPDATE ) \\
		if( n_bgr != 0 ) \\
		{ \\
			WRITE_PIXEL( \\
				p_n_redshade[ p_n_redlevel[ n_bgr ] | n_r.w.h ] | \\
				p_n_greenshade[ p_n_greenlevel[ n_bgr ] | n_g.w.h ] | \\
				p_n_blueshade[ p_n_bluelevel[ n_bgr ] | n_b.w.h ] | \\
				( n_bgr & 0x8000 ) ) \\
		} \\
""", """/* HIRAKU: p_n_redlevel[] and friends, computed */
#define HK_RLEVEL( c ) ( ( ( c ) & 0x1f ) << 8 )
#define HK_GLEVEL( c ) ( ( ( ( c ) >> 5 ) & 0x1f ) << 8 )
#define HK_BLEVEL( c ) ( ( ( ( c ) >> 10 ) & 0x1f ) << 8 )
#define HK_NEUTRAL ( n_r.w.h == 0x80 && n_g.w.h == 0x80 && n_b.w.h == 0x80 )

#define SHADEDPIXEL( PIXELUPDATE ) \\
		if( n_bgr != 0 ) \\
		{ \\
			if( HK_NEUTRAL ) \\
				WRITE_PIXEL( n_bgr ) \\
			else \\
				WRITE_PIXEL( \\
					p_n_redshade[ HK_RLEVEL( n_bgr ) | n_r.w.h ] | \\
					p_n_greenshade[ HK_GLEVEL( n_bgr ) | n_g.w.h ] | \\
					p_n_blueshade[ HK_BLEVEL( n_bgr ) | n_b.w.h ] | \\
					( n_bgr & 0x8000 ) ) \\
		} \\
""", 'SHADEDPIXEL')
    s = edit(s, """				WRITE_PIXEL( \\
					p_n_redtrans[ p_n_f[ p_n_redlevel[ n_bgr ] | n_r.w.h ] | p_n_redb[ *( p_vram ) ] ] | \\
					p_n_greentrans[ p_n_f[ p_n_greenlevel[ n_bgr ] | n_g.w.h ] | p_n_greenb[ *( p_vram ) ] ] | \\
					p_n_bluetrans[ p_n_f[ p_n_bluelevel[ n_bgr ] | n_b.w.h ] | p_n_blueb[ *( p_vram ) ] ] | \\
					0x8000 ) \\
			} \\
			else \\
			{ \\
				WRITE_PIXEL( \\
					p_n_redshade[ p_n_redlevel[ n_bgr ] | n_r.w.h ] | \\
					p_n_greenshade[ p_n_greenlevel[ n_bgr ] | n_g.w.h ] | \\
					p_n_blueshade[ p_n_bluelevel[ n_bgr ] | n_b.w.h ] ) \\
			} \\
""", """				WRITE_PIXEL( \\
					p_n_redtrans[ p_n_f[ HK_RLEVEL( n_bgr ) | n_r.w.h ] | p_n_redb[ *( p_vram ) ] ] | \\
					p_n_greentrans[ p_n_f[ HK_GLEVEL( n_bgr ) | n_g.w.h ] | p_n_greenb[ *( p_vram ) ] ] | \\
					p_n_bluetrans[ p_n_f[ HK_BLEVEL( n_bgr ) | n_b.w.h ] | p_n_blueb[ *( p_vram ) ] ] | \\
					0x8000 ) \\
			} \\
			else if( HK_NEUTRAL ) \\
			{ \\
				WRITE_PIXEL( n_bgr ) \\
			} \\
			else \\
			{ \\
				WRITE_PIXEL( \\
					p_n_redshade[ HK_RLEVEL( n_bgr ) | n_r.w.h ] | \\
					p_n_greenshade[ HK_GLEVEL( n_bgr ) | n_g.w.h ] | \\
					p_n_blueshade[ HK_BLEVEL( n_bgr ) | n_b.w.h ] ) \\
			} \\
""", 'TRANSPARENTPIXEL')
    # FlatRectangle (the 573's install screens' solid fills): opaque and no mask test, every pixel
    # of a row gets the same value, so the row is filled at once (std::fill_n), the same pixels
    s = edit(s, """			if( ( (int32_t)n_drawarea_x1 - drawx ) > 0 )
			{
				n_distance -= ( n_drawarea_x1 - drawx );
				drawx = n_drawarea_x1;
			}

			SOLIDFILL( FLATRECTANGEUPDATE )
		}

		n_y++;
		n_h--;
	}
}

void psxgpu_device::FlatRectangle8x8()""", """			if( ( (int32_t)n_drawarea_x1 - drawx ) > 0 )
			{
				n_distance -= ( n_drawarea_x1 - drawx );
				drawx = n_drawarea_x1;
			}

			if( ( n_cmd & 0x02 ) == 0 && !m_check_stp )
			{
				/* HIRAKU: opaque and no mask test: every pixel the same colour, filled at once */
				if( n_distance > ( (int32_t)n_drawarea_x2 - drawx ) + 1 )
					n_distance = ( n_drawarea_x2 - drawx ) + 1;
				if( n_distance > 0 )
					std::fill_n( p_p_vram[ drawy ] + drawx, n_distance, (uint16_t)( (
						p_n_redshade[ MID_LEVEL | n_r.w.h ] |
						p_n_greenshade[ MID_LEVEL | n_g.w.h ] |
						p_n_blueshade[ MID_LEVEL | n_b.w.h ] ) | ( m_draw_stp ? 0x8000 : 0 ) ) );
			}
			else
			{
				SOLIDFILL( FLATRECTANGEUPDATE )
			}
		}

		n_y++;
		n_h--;
	}
}

void psxgpu_device::FlatRectangle8x8()""", "video/psx.cpp FlatRectangle fill")
    return video_psx_cpp_worker(s)


DRAWFNS = ['FlatPolygon', 'FlatTexturedPolygon', 'GouraudPolygon', 'GouraudTexturedPolygon', 'MonochromeLine',
           'GouraudLine', 'FrameBufferRectangleDraw', 'FlatRectangle', 'FlatRectangle8x8', 'FlatRectangle16x16',
           'FlatTexturedRectangle', 'Sprite8x8', 'Sprite16x16', 'Dot', 'TexturedDot', 'MoveImage']
POLY = {'FlatPolygon', 'FlatTexturedPolygon', 'GouraudPolygon', 'GouraudTexturedPolygon'}
TPAGE = {'FlatTexturedPolygon', 'GouraudTexturedPolygon'}


def video_psx_h(s):
    s = edit(s, '#include "machine/ram.h"\n', '#include "machine/ram.h"\n\n#include <atomic>\n#include <deque>\n#include <memory>\n#include <thread>\n', 'video/psx.h includes')
    s = edit(s, '\tstatic constexpr feature_type imperfect_features() { return feature::GRAPHICS; }\n',
             '\tstatic constexpr feature_type imperfect_features() { return feature::GRAPHICS; }\n\n'
             '\tvirtual ~psxgpu_device(); // HIRAKU: the drawing thread\n', 'video/psx.h destructor')
    s = edit(s, '\tvirtual void device_reset() override ATTR_COLD;\n',
             '\tvirtual void device_reset() override ATTR_COLD;\n\tvirtual void device_stop() override; // HIRAKU\n', 'video/psx.h stop')
    decls = ''.join(f'\tvoid {f}({" int n_points " if f in POLY else ""});\n' for f in DRAWFNS)
    rdecls = decls.replace('\tvoid ', '\t\tvoid ')
    ops = ', '.join(f'HK_{f}' for f in DRAWFNS)
    wrappers = ''
    for f in DRAWFNS:
        arg = ' int n_points ' if f in POLY else ''
        n = 'n_points' if f in POLY else '0'
        tp = f' decode_tpage( m_packet.{f}.vertex[ 1 ].n_texture.w.h );' if f in TPAGE else ''
        wrappers += f'\tvoid {f}({arg}) {{ hk_draw( HK_{f}, {n} );{tp} }}\n'
    s = edit(s, '\tvoid decode_tpage( uint32_t tpage );\n' + decls, '''\tvoid decode_tpage( uint32_t tpage );

	// HIRAKU: the drawing on a worker thread (PLAN.md section 19). The command parser, the status
	// and every register stay here, on the emulation thread; what writes VRAM (the primitives,
	// the CPU-to-VRAM image, MoveImage) goes into a queue with a copy of the state the drawing
	// reads (draw_state), and raster draws it on the worker, in order. What reads VRAM waits for
	// the queued items that could change what it reads: VRAM-to-CPU for all of them; the screen
	// update for those whose rectangle meets the rows and columns it shows (games draw the next
	// picture into the other buffer). So every read sees what MAME's would.
	// The textured polygons' texture page reaches the status here as in MAME (decode_tpage()
	// beside the queueing), and again in raster's own copy of the state.
	// HK_GPU_THREAD=0: no worker; each item is drawn on the emulation thread as it is queued.
	struct draw_state
	{
		int32_t m_n_tx, m_n_ty, n_abr, n_tp, n_ix, n_iy, n_ti;
		uint32_t n_vramx, n_vramy, n_twy, n_twx, n_twh, n_tww;
		uint32_t n_drawarea_x1, n_drawarea_y1, n_drawarea_x2, n_drawarea_y2;
		int32_t n_drawoffset_x, n_drawoffset_y;
		int m_n_gputype;
		uint32_t n_gpustatus;
		bool m_draw_stp, m_check_stp;
		PACKET m_packet;
	};
	struct raster : draw_state
	{
		uint16_t *p_p_vram[ 1024 ];
		template <typename... T> void logerror( T &&... ) const { }
		void decode_tpage( uint32_t tpage );
''' + rdecls + '''		void image( const uint32_t *p_data, uint32_t n_words );
	};
	enum : uint32_t { ''' + ops + ''', HK_IMAGE, HK_SKIP };
''' + wrappers + '''	void hk_snapshot( draw_state &s ) const;
	uint32_t *hk_reserve( uint32_t n_words );
	void hk_publish();
	void hk_draw( uint32_t op, int n_points );
	void hk_image( uint32_t data );
	void hk_close() { if( m_hk_image != nullptr ) hk_publish(); }
	void hk_sync();
	void hk_sync_area( int32_t x, int32_t w, int32_t y, int32_t h );
	void hk_run();
	void hk_worker();

	static constexpr uint32_t HK_RING = 1 << 20; // words
	static constexpr uint32_t HK_HEADER = 2 + ( sizeof( draw_state ) + 3 ) / 4;
	std::unique_ptr<uint32_t[]> m_hk_ring;
	std::atomic<uint64_t> m_hk_head{ 0 }; // published: the worker draws up to here
	std::atomic<uint64_t> m_hk_tail{ 0 }; // drawn
	std::atomic<uint32_t> m_hk_sleep{ 0 }, m_hk_wait{ 0 }; // the worker asleep; the emulation thread waiting
	std::atomic<bool> m_hk_stop{ false };
	uint64_t m_hk_write = 0; // the end of what is queued, published or not
	uint32_t *m_hk_image = nullptr; // the open image item, words still being added
	raster m_hk_raster;
	std::thread m_hk_thread;
	// what the queued items may still write: a rectangle of VRAM per run of items, with the queue
	// position where the run ends. Columns and rows are taken modulo 1024 (x, y) with a length.
	struct hk_area { int32_t x, w, y, h; bool operator==( const hk_area & ) const = default; };
	std::deque<std::pair<uint64_t, hk_area>> m_hk_pending;
	int32_t m_hk_rows = 0; // VRAM's height (rows beyond it alias)

''', 'video/psx.h raster')
    # the tables never change once built, and raster (a nested class) reads them by name: static
    a = s.index('\tuint16_t p_n_redshade[ MAX_LEVEL * MAX_SHADE ];')
    b = s.index('\tdevcb_write_line m_vblank_handler;')
    block = s[a:b]
    block = '\t// HIRAKU: static, for raster\n' + '\n'.join(
        l.replace('\tuint', '\tstatic inline uint', 1) if l.startswith('\tuint') else l for l in block.split('\n'))
    s = s[:a] + block + s[b:]
    return video_psx_between_h(s)


def video_psx_cpp_worker(s):
    for f in DRAWFNS:
        arg = ' int n_points ' if f in POLY else ''
        s = edit(s, f'void psxgpu_device::{f}({arg})\n', f'void psxgpu_device::raster::{f}({arg})\n', f'video/psx.cpp {f}')
    # raster's own decode_tpage: the same function, a second time
    a = s.index('void psxgpu_device::decode_tpage( uint32_t tpage )\n')
    b = s.index('#define SPRITESETUP', a)
    s = s[:b] + '/* HIRAKU: the same, for raster */\n' + s[a:b].replace('psxgpu_device::decode_tpage', 'psxgpu_device::raster::decode_tpage') + s[b:]
    # CPU-to-VRAM: the words queued; the counting stays (the parser needs the transfer's end)
    s = edit(s, """				for( int n_pixel = 0; n_pixel < 2; n_pixel++ )
				{
					LOGMASKED(LOG_WRITE, "%s: send image to framebuffer ( pixel %u,%u = %u )\\n",
						machine().describe_context(),
						( n_vramx + m_packet.n_entry[ 1 ] ) & 1023,
						( n_vramy + ( m_packet.n_entry[ 1 ] >> 16 ) ) & 1023,
						data & 0xffff );

					uint16_t *p_vram = p_p_vram[ ( n_vramy + ( m_packet.n_entry[ 1 ] >> 16 ) ) & 1023 ] + ( ( n_vramx + m_packet.n_entry[ 1 ] ) & 1023 );
					WRITE_PIXEL( data & 0xffff )
					n_vramx++;
""", """				hk_image( data ); // HIRAKU: drawn by raster::image()
				for( int n_pixel = 0; n_pixel < 2; n_pixel++ )
				{
					LOGMASKED(LOG_WRITE, "%s: send image to framebuffer ( pixel %u,%u = %u )\\n",
						machine().describe_context(),
						( n_vramx + m_packet.n_entry[ 1 ] ) & 1023,
						( n_vramy + ( m_packet.n_entry[ 1 ] >> 16 ) ) & 1023,
						data & 0xffff );

					n_vramx++;
""", 'video/psx.cpp image')
    # any word but an image's data ends the open image item
    s = edit(s, """		uint32_t data = *( p_ram );

		LOG("PSX Packet #%u %08x\\n", n_gpu_buffer_offset, data);
""", """		uint32_t data = *( p_ram );

		if( m_hk_image != nullptr && ( n_gpu_buffer_offset != 3 || ( m_packet.n_entry[ 0 ] >> 24 ) != 0xa0 ) )
			hk_close(); // HIRAKU
		LOG("PSX Packet #%u %08x\\n", n_gpu_buffer_offset, data);
""", 'video/psx.cpp image end')
    s = edit(s, """void psxgpu_device::gpu_read( uint32_t *p_ram, int32_t n_size )
{
""", """void psxgpu_device::gpu_read( uint32_t *p_ram, int32_t n_size )
{
	if( ( n_gpustatus & ( 1L << 0x1b ) ) != 0 )
		hk_sync(); // HIRAKU: VRAM to the CPU
""", 'video/psx.cpp gpu_read')
    s = edit(s, """			/* 24bit */
			n_line = n_lines;
""", """			/* 24bit */
			hk_sync(); // HIRAKU
			n_line = n_lines;
""", 'video/psx.cpp update_screen 24bit')
    s = edit(s, """			/* 15bit */
			n_line = n_lines;
""", """			/* 15bit */
			hk_sync_area( n_x + n_displaystartx, n_columns, n_y + n_displaystarty, n_lines ); // HIRAKU: what it reads
			n_line = n_lines;
""", 'video/psx.cpp update_screen 15bit')
    s = edit(s, """	else
	{
		psx_gpu_init( 2 );
	}
}
""", """	else
	{
		psx_gpu_init( 2 );
	}

	// HIRAKU: the drawing thread
	std::copy_n( p_p_vram, 1024, m_hk_raster.p_p_vram );
	m_hk_rows = ( m_ram->size() / 1024 ) / sizeof( uint16_t );
	m_hk_ring = std::make_unique<uint32_t[]>( HK_RING );
	const char *thread = getenv( "HK_GPU_THREAD" );
	if( thread == nullptr || strcmp( thread, "0" ) != 0 )
		m_hk_thread = std::thread( &psxgpu_device::hk_worker, this );
}
""", 'video/psx.cpp start')
    s = s.rstrip('\n') + '\n' + WORKER
    return video_psx_between_cpp(s)


WORKER = r'''
// HIRAKU: the drawing thread (psx.h)

psxgpu_device::~psxgpu_device()
{
	device_stop();
}

void psxgpu_device::device_stop()
{
	if( m_hk_thread.joinable() )
	{
		hk_sync();
		m_hk_stop.store( true );
		m_hk_sleep.store( 0 );
		m_hk_sleep.notify_one();
		m_hk_thread.join();
	}
}

static inline void hk_pause()
{
#if defined( __x86_64__ ) || defined( __i386__ )
	__builtin_ia32_pause();
#elif defined( __aarch64__ )
	asm volatile( "yield" );
#endif
}

void psxgpu_device::hk_snapshot( draw_state &s ) const
{
	s.m_n_tx = m_n_tx; s.m_n_ty = m_n_ty; s.n_abr = n_abr; s.n_tp = n_tp; s.n_ix = n_ix; s.n_iy = n_iy; s.n_ti = n_ti;
	s.n_vramx = n_vramx; s.n_vramy = n_vramy; s.n_twy = n_twy; s.n_twx = n_twx; s.n_twh = n_twh; s.n_tww = n_tww;
	s.n_drawarea_x1 = n_drawarea_x1; s.n_drawarea_y1 = n_drawarea_y1; s.n_drawarea_x2 = n_drawarea_x2; s.n_drawarea_y2 = n_drawarea_y2;
	s.n_drawoffset_x = n_drawoffset_x; s.n_drawoffset_y = n_drawoffset_y;
	s.m_n_gputype = m_n_gputype; s.n_gpustatus = n_gpustatus;
	s.m_draw_stp = m_draw_stp; s.m_check_stp = m_check_stp;
	s.m_packet = m_packet;
}

// n_words free and contiguous at m_hk_write (an item never wraps: a skip item fills the ring's end)
uint32_t *psxgpu_device::hk_reserve( uint32_t n_words )
{
	uint32_t pos = m_hk_write & ( HK_RING - 1 );
	uint32_t need = n_words;
	if( pos + n_words > HK_RING )
		need += HK_RING - pos;
	uint64_t tail = m_hk_tail.load( std::memory_order_acquire );
	if( m_hk_write + need - tail > HK_RING )
	{
		if( !m_hk_thread.joinable() )
			hk_run();
		else
		{
			// full: wait for the worker
			for( int spin = 0; m_hk_write + need - ( tail = m_hk_tail.load( std::memory_order_acquire ) ) > HK_RING; spin++ )
			{
				if( spin < 4096 )
					hk_pause();
				else
				{
					m_hk_wait.store( 1 );
					if( m_hk_write + need - m_hk_tail.load() > HK_RING )
						m_hk_wait.wait( 1 );
					m_hk_wait.store( 0 );
				}
			}
		}
	}
	if( pos + n_words > HK_RING )
	{
		m_hk_ring[ pos ] = HK_SKIP;
		m_hk_ring[ pos + 1 ] = HK_RING - pos;
		m_hk_write += HK_RING - pos;
		pos = 0;
	}
	return &m_hk_ring[ pos ];
}

void psxgpu_device::hk_publish()
{
	uint32_t *item = m_hk_image != nullptr ? m_hk_image : &m_hk_ring[ m_hk_write & ( HK_RING - 1 ) ];
	m_hk_write += item[ 1 ];
	m_hk_image = nullptr;
	if( m_hk_thread.joinable() )
	{
		// the rectangle it may write (its run's, when the last item's was the same)
		const draw_state &d = *reinterpret_cast<const draw_state *>( &item[ 2 ] );
		const PACKET &p = d.m_packet;
		hk_area a;
		switch( item[ 0 ] & 0xff )
		{
		case HK_FrameBufferRectangleDraw: // rows and columns wrap
			a = { COORD_X( p.FlatRectangle.n_coord ) & 1023, std::min<int32_t>( SIZE_W( p.FlatRectangle.n_size ), 1024 ),
				COORD_Y( p.FlatRectangle.n_coord ) & 1023, std::min<int32_t>( SIZE_H( p.FlatRectangle.n_size ), 1024 ) };
			break;
		case HK_MoveImage: // to vertex 1, wrapping; its sizes are int16_t there
			a = { COORD_X( p.MoveImage.vertex[ 1 ].n_coord ) & 1023, std::min<int32_t>( (int16_t)SIZE_W( p.MoveImage.n_size ), 1024 ),
				COORD_Y( p.MoveImage.vertex[ 1 ].n_coord ) & 1023, std::min<int32_t>( (int16_t)SIZE_H( p.MoveImage.n_size ), 1024 ) };
			break;
		case HK_IMAGE: // from n_vramx/y within the transfer's rectangle, wrapping; a size of 0 writes one pixel
			a = { int32_t( p.n_entry[ 1 ] & 1023 ), std::clamp<int32_t>( p.n_entry[ 2 ] & 0xffff, 1, 1024 ),
				int32_t( ( p.n_entry[ 1 ] >> 16 ) & 1023 ), std::clamp<int32_t>( p.n_entry[ 2 ] >> 16, 1, 1024 ) };
			break;
		case HK_FlatRectangle16x16: // MAME tests the row before the drawing offset: any row
			a = { int32_t( d.n_drawarea_x1 ), int32_t( d.n_drawarea_x2 ) - int32_t( d.n_drawarea_x1 ) + 1, 0, 1024 };
			break;
		default: // the primitives clip to the drawing area
			a = { int32_t( d.n_drawarea_x1 ), int32_t( d.n_drawarea_x2 ) - int32_t( d.n_drawarea_x1 ) + 1,
				int32_t( d.n_drawarea_y1 ), int32_t( d.n_drawarea_y2 ) - int32_t( d.n_drawarea_y1 ) + 1 };
			break;
		}
		if( a.w > 0 && a.h > 0 )
		{
			if( !m_hk_pending.empty() && m_hk_pending.back().second == a )
				m_hk_pending.back().first = m_hk_write;
			else
				m_hk_pending.emplace_back( m_hk_write, a );
		}
	}
	m_hk_head.store( m_hk_write, std::memory_order_seq_cst );
	if( !m_hk_thread.joinable() )
		hk_run();
	else if( m_hk_sleep.load( std::memory_order_seq_cst ) != 0 )
	{
		m_hk_sleep.store( 0 );
		m_hk_sleep.notify_one();
	}
}

void psxgpu_device::hk_draw( uint32_t op, int n_points )
{
	hk_close();
	uint32_t *item = hk_reserve( HK_HEADER );
	item[ 0 ] = op | ( n_points << 8 );
	item[ 1 ] = HK_HEADER;
	hk_snapshot( *reinterpret_cast<draw_state *>( &item[ 2 ] ) );
	hk_publish();
}

void psxgpu_device::hk_image( uint32_t data )
{
	// the item's words stay at m_hk_write until it is published: [1] counts them
	if( m_hk_image != nullptr && m_hk_image[ 1 ] < 0x4000 && ( m_hk_image - m_hk_ring.get() ) + m_hk_image[ 1 ] < HK_RING )
	{
		m_hk_image[ m_hk_image[ 1 ]++ ] = data;
		return;
	}
	hk_close();
	uint32_t *item = hk_reserve( HK_HEADER + 0x4000 );
	item[ 0 ] = HK_IMAGE;
	item[ 1 ] = HK_HEADER;
	hk_snapshot( *reinterpret_cast<draw_state *>( &item[ 2 ] ) );
	item[ item[ 1 ]++ ] = data;
	m_hk_image = item;
}

// the queue drawn: VRAM as MAME's would be
void psxgpu_device::hk_sync()
{
	hk_close();
	if( !m_hk_thread.joinable() )
		return;
	const uint64_t head = m_hk_write;
	for( int spin = 0; m_hk_tail.load( std::memory_order_acquire ) != head; spin++ )
	{
		if( spin < 4096 )
			hk_pause();
		else
		{
			m_hk_wait.store( 1 );
			if( m_hk_tail.load() != head )
				m_hk_wait.wait( 1 );
			m_hk_wait.store( 0 );
		}
	}
	m_hk_pending.clear();
}

// VRAM's columns x to x + w (not wrapping: a row read past column 1023 runs into the next) and
// rows y to y + h (wrapping), as they would be with the queue drawn: waits only if a queued item
// may still write there
void psxgpu_device::hk_sync_area( int32_t x, int32_t w, int32_t y, int32_t h )
{
	if( !m_hk_thread.joinable() || w <= 0 || h <= 0 )
		return;
	hk_close();
	if( x < 0 || x + w > 1024 || m_hk_rows != 1024 )
	{
		hk_sync();
		return;
	}
	const uint64_t tail = m_hk_tail.load( std::memory_order_acquire );
	while( !m_hk_pending.empty() && m_hk_pending.front().first <= tail )
		m_hk_pending.pop_front();
	y &= 1023;
	h = std::min( h, 1024 );
	// two stretches of a circle of 1024 meet when either starts inside the other
	auto meet = []( int32_t a, int32_t la, int32_t b, int32_t lb ) { return ( ( b - a ) & 1023 ) < la || ( ( a - b ) & 1023 ) < lb; };
	for( const auto &[ end, a ] : m_hk_pending )
		if( meet( a.x, a.w, x, w ) && meet( a.y, a.h, y, h ) )
		{
			hk_sync();
			return;
		}
}

// draw what is published
void psxgpu_device::hk_run()
{
	uint64_t tail = m_hk_tail.load( std::memory_order_relaxed );
	const uint64_t head = m_hk_head.load( std::memory_order_acquire );
	raster &r = m_hk_raster;
	while( tail != head )
	{
		const uint32_t *item = &m_hk_ring[ tail & ( HK_RING - 1 ) ];
		const uint32_t op = item[ 0 ] & 0xff;
		const int n_points = item[ 0 ] >> 8;
		if( op != HK_SKIP )
			static_cast<draw_state &>( r ) = *reinterpret_cast<const draw_state *>( &item[ 2 ] );
		switch( op )
		{
		case HK_FlatPolygon: r.FlatPolygon( n_points ); break;
		case HK_FlatTexturedPolygon: r.FlatTexturedPolygon( n_points ); break;
		case HK_GouraudPolygon: r.GouraudPolygon( n_points ); break;
		case HK_GouraudTexturedPolygon: r.GouraudTexturedPolygon( n_points ); break;
		case HK_MonochromeLine: r.MonochromeLine(); break;
		case HK_GouraudLine: r.GouraudLine(); break;
		case HK_FrameBufferRectangleDraw: r.FrameBufferRectangleDraw(); break;
		case HK_FlatRectangle: r.FlatRectangle(); break;
		case HK_FlatRectangle8x8: r.FlatRectangle8x8(); break;
		case HK_FlatRectangle16x16: r.FlatRectangle16x16(); break;
		case HK_FlatTexturedRectangle: r.FlatTexturedRectangle(); break;
		case HK_Sprite8x8: r.Sprite8x8(); break;
		case HK_Sprite16x16: r.Sprite16x16(); break;
		case HK_Dot: r.Dot(); break;
		case HK_TexturedDot: r.TexturedDot(); break;
		case HK_MoveImage: r.MoveImage(); break;
		case HK_IMAGE: r.image( &item[ HK_HEADER ], item[ 1 ] - HK_HEADER ); break;
		case HK_SKIP: break;
		}
		tail += item[ 1 ];
		m_hk_tail.store( tail, std::memory_order_seq_cst );
		if( m_hk_wait.load( std::memory_order_seq_cst ) != 0 )
		{
			m_hk_wait.store( 0 );
			m_hk_wait.notify_one();
		}
	}
}

void psxgpu_device::hk_worker()
{
	for( int spin = 0; !m_hk_stop.load( std::memory_order_relaxed ); )
	{
		if( m_hk_head.load( std::memory_order_acquire ) != m_hk_tail.load( std::memory_order_relaxed ) )
		{
			hk_run();
			spin = 0;
		}
		else if( spin++ < 256 )
			hk_pause(); // a moment for the next item (a longer spin bought little, for much more CPU)
		else
		{
			// idle: sleep until hk_publish() rings
			m_hk_sleep.store( 1 );
			if( m_hk_head.load() == m_hk_tail.load( std::memory_order_relaxed ) && !m_hk_stop.load() )
				m_hk_sleep.wait( 1 );
			m_hk_sleep.store( 0 );
			spin = 0;
		}
	}
}

// the CPU-to-VRAM transfer's words (gpu_write's loop, writing)
void psxgpu_device::raster::image( const uint32_t *p_data, uint32_t n_words )
{
	while( n_words-- > 0 )
	{
		uint32_t data = *( p_data++ );
		for( int n_pixel = 0; n_pixel < 2; n_pixel++ )
		{
			uint16_t *p_vram = p_p_vram[ ( n_vramy + ( m_packet.n_entry[ 1 ] >> 16 ) ) & 1023 ] + ( ( n_vramx + m_packet.n_entry[ 1 ] ) & 1023 );
			WRITE_PIXEL( data & 0xffff )
			n_vramx++;
			if( n_vramx >= ( m_packet.n_entry[ 2 ] & 0xffff ) )
			{
				n_vramx = 0;
				n_vramy++;
				if( n_vramy >= ( m_packet.n_entry[ 2 ] >> 16 ) )
				{
					n_vramx = 0;
					n_vramy = 0;
					break;
				}
			}
			data >>= 16;
		}
	}
}
'''

BETWEEN_H_DECL = '''
	// HIRAKU: in-between pictures (Core::render_between, --smooth-motion). Once asked for, the
	// queued items are kept (each holds all it draws from), a frame's from one flip of the display
	// to the next (a write of a new display start: the game shows what it drew since its last
	// flip); a game that never flips (it draws where it shows) has a frame each VBLANK. hk_between()
	// draws the frame on show again, what moved whole carried on along its last move (matched with
	// the frame before's), `t` of a VBLANK on from this one's (a game at 30 frames a second moves half
	// a step a VBLANK), into VRAM, composes it into `bitmap` with update_screen(), and puts VRAM
	// back. false: nothing to draw from yet.
public:
	bool hk_between( double t, screen_device &screen, bitmap_rgb32 &bitmap );
private:
	void hk_dispatch( raster &r, const uint32_t *item );
	hk_area hk_item_area( const uint32_t *item ) const;
	void hk_new_frame( uint32_t vblanks );
	bool m_hk_keep = false;
	std::vector<uint32_t> m_hk_drawing, m_hk_shown, m_hk_shown_before; // items, back to back
	uint32_t m_hk_display = ~0u;    // the display start last written (x | y << 16)
	uint32_t m_hk_vblanks = 0;      // VBLANKs since the last frame began to show
	uint32_t m_hk_frame_vblanks = 1; // ... and how many the frame before it showed for
	uint32_t m_hk_no_flip = 1000;   // VBLANKs since the last flip
	uint64_t m_hk_frames = 0;       // frames shown (hk_new_frame): the scroll guess counts once a frame
	hk::ScrollGuess m_hk_scroll;    // what scrolls with the frame's other moving items: its steady line
	struct hk_move { size_t i; int n, dx, dy; };
	std::vector<hk_move> m_hk_moves; // the frame's pairing: what moved whole, and by how much (its item, its number among them)
	uint64_t m_hk_moves_frame = ~uint64_t( 0 ); // ... for this frame
	size_t m_hk_moves_size = 0;
	// what the uploads (CPU-to-VRAM, MoveImage, fills) wrote over, kept by the drawing thread from the
	// queue's position where the frame after the one on show begins: the textures and palettes that
	// frame changes (the game prepares it while this one shows) put back while this one is drawn again
	struct hk_undo { uint64_t at; hk_area a; std::vector<uint16_t> pixels; };
	void hk_keep_undo( const uint32_t *item, uint64_t at );
	static void hk_area_copy( uint16_t **p_p_vram, const hk_area &a, uint16_t *pixels, bool into );
	std::atomic<bool> m_hk_undo_on{ false };
	std::atomic<uint64_t> m_hk_frame_at{ 0 };
	std::deque<hk_undo> m_hk_undo;
'''


def video_psx_between_h(s):
    s = edit(s, '#include <thread>\n', '#include <thread>\n#include <unordered_map>\n#include <vector>\n\n#include "hiraku/scroll_guess.h"\n', 'video/psx.h between includes')
    rows = "\tint32_t m_hk_rows = 0; // VRAM's height (rows beyond it alias)\n"
    return edit(s, rows, rows + BETWEEN_H_DECL, 'video/psx.h between')


BETWEEN_CPP = r'''
// HIRAKU: in-between pictures (psx.h)

// an item drawn by r, the state it was queued with first
void psxgpu_device::hk_dispatch( raster &r, const uint32_t *item )
{
	const uint32_t op = item[ 0 ] & 0xff;
	const int n_points = item[ 0 ] >> 8;
	static_cast<draw_state &>( r ) = *reinterpret_cast<const draw_state *>( &item[ 2 ] );
	switch( op )
	{
	case HK_FlatPolygon: r.FlatPolygon( n_points ); break;
	case HK_FlatTexturedPolygon: r.FlatTexturedPolygon( n_points ); break;
	case HK_GouraudPolygon: r.GouraudPolygon( n_points ); break;
	case HK_GouraudTexturedPolygon: r.GouraudTexturedPolygon( n_points ); break;
	case HK_MonochromeLine: r.MonochromeLine(); break;
	case HK_GouraudLine: r.GouraudLine(); break;
	case HK_FrameBufferRectangleDraw: r.FrameBufferRectangleDraw(); break;
	case HK_FlatRectangle: r.FlatRectangle(); break;
	case HK_FlatRectangle8x8: r.FlatRectangle8x8(); break;
	case HK_FlatRectangle16x16: r.FlatRectangle16x16(); break;
	case HK_FlatTexturedRectangle: r.FlatTexturedRectangle(); break;
	case HK_Sprite8x8: r.Sprite8x8(); break;
	case HK_Sprite16x16: r.Sprite16x16(); break;
	case HK_Dot: r.Dot(); break;
	case HK_TexturedDot: r.TexturedDot(); break;
	case HK_MoveImage: r.MoveImage(); break;
	case HK_IMAGE: r.image( &item[ HK_HEADER ], item[ 1 ] - HK_HEADER ); break;
	}
}

// the rectangle of VRAM an item may write: columns and rows modulo 1024, with a length
psxgpu_device::hk_area psxgpu_device::hk_item_area( const uint32_t *item ) const
{
	const draw_state &d = *reinterpret_cast<const draw_state *>( &item[ 2 ] );
	const PACKET &p = d.m_packet;
	switch( item[ 0 ] & 0xff )
	{
	case HK_FrameBufferRectangleDraw: // rows and columns wrap
		return { COORD_X( p.FlatRectangle.n_coord ) & 1023, std::min<int32_t>( SIZE_W( p.FlatRectangle.n_size ), 1024 ),
			COORD_Y( p.FlatRectangle.n_coord ) & 1023, std::min<int32_t>( SIZE_H( p.FlatRectangle.n_size ), 1024 ) };
	case HK_MoveImage: // to vertex 1, wrapping; its sizes are int16_t there
		return { COORD_X( p.MoveImage.vertex[ 1 ].n_coord ) & 1023, std::min<int32_t>( (int16_t)SIZE_W( p.MoveImage.n_size ), 1024 ),
			COORD_Y( p.MoveImage.vertex[ 1 ].n_coord ) & 1023, std::min<int32_t>( (int16_t)SIZE_H( p.MoveImage.n_size ), 1024 ) };
	case HK_IMAGE: // from n_vramx/y within the transfer's rectangle, wrapping; a size of 0 writes one pixel
		return { int32_t( p.n_entry[ 1 ] & 1023 ), std::clamp<int32_t>( p.n_entry[ 2 ] & 0xffff, 1, 1024 ),
			int32_t( ( p.n_entry[ 1 ] >> 16 ) & 1023 ), std::clamp<int32_t>( p.n_entry[ 2 ] >> 16, 1, 1024 ) };
	case HK_FlatRectangle16x16: // MAME tests the row before the drawing offset: any row
		return { int32_t( d.n_drawarea_x1 ), int32_t( d.n_drawarea_x2 ) - int32_t( d.n_drawarea_x1 ) + 1, 0, 1024 };
	default: // the primitives clip to the drawing area
		return { int32_t( d.n_drawarea_x1 ), int32_t( d.n_drawarea_x2 ) - int32_t( d.n_drawarea_x1 ) + 1,
			int32_t( d.n_drawarea_y1 ), int32_t( d.n_drawarea_y2 ) - int32_t( d.n_drawarea_y1 ) + 1 };
	}
}

// the items since the last frame are the frame on show now
void psxgpu_device::hk_new_frame( uint32_t vblanks )
{
	m_hk_shown_before = std::move( m_hk_shown );
	m_hk_shown = std::move( m_hk_drawing );
	m_hk_drawing.clear();
	m_hk_frame_vblanks = std::clamp<uint32_t>( vblanks, 1, 4 );
	m_hk_vblanks = 0;
	m_hk_frames++;
	m_hk_frame_at.store( m_hk_write, std::memory_order_release );
}

// (the drawing thread) before an upload: what it writes over
void psxgpu_device::hk_keep_undo( const uint32_t *item, uint64_t at )
{
	const uint64_t from = m_hk_frame_at.load( std::memory_order_acquire );
	while( !m_hk_undo.empty() && m_hk_undo.front().at < from )
		m_hk_undo.pop_front();
	hk_undo u{ at, hk_item_area( item ), {} };
	if( u.a.w > 0 && u.a.h > 0 )
	{
		u.pixels.resize( size_t( u.a.w ) * u.a.h );
		hk_area_copy( m_hk_raster.p_p_vram, u.a, u.pixels.data(), false );
	}
	m_hk_undo.push_back( std::move( u ) );
}

// An area's pixels out of VRAM (into: back into it), in the order the per-pixel loops took them
// (rows, then columns, both wrapping at 1024), a stretch of a row at a time: a frame's clear is
// 640x240 pixels, kept by the drawing thread and swapped twice by every in-between picture.
void psxgpu_device::hk_area_copy( uint16_t **p_p_vram, const hk_area &a, uint16_t *pixels, bool into )
{
	for( int32_t y = 0; y < a.h; y++ )
	{
		uint16_t *row = p_p_vram[ ( a.y + y ) & 1023 ];
		for( int32_t x = 0; x < a.w; )
		{
			const int32_t c = ( a.x + x ) & 1023, n = std::min( a.w - x, 1024 - c );
			if( into )
				std::copy_n( pixels, n, row + c );
			else
				std::copy_n( row + c, n, pixels );
			pixels += n;
			x += n;
		}
	}
}

bool psxgpu_device::hk_between( double t, screen_device &screen, bitmap_rgb32 &bitmap )
{
	if( !m_hk_keep )
	{
		m_hk_keep = true; // from now on
		m_hk_drawing.clear();
		m_hk_frame_at.store( m_hk_write );
		m_hk_undo_on.store( true );
		return false;
	}
	if( m_hk_shown.empty() || m_hk_shown_before.empty() )
		return false;
	hk_sync(); // VRAM as the queue leaves it, the worker idle
	// how far on, in the frame's moves: a frame shown for two VBLANKs moves half its step a VBLANK
	const double on = std::min( ( m_hk_vblanks + t ) / m_hk_frame_vblanks, 1.5 );
	// where an item's vertices are in its packet (n_entry), and how many: none for what doesn't move
	auto vertices = []( uint32_t op, int n_points, int *at ) {
		int n = 0, step = 1;
		switch( op )
		{
		case HK_FlatPolygon: n = n_points; break;
		case HK_FlatTexturedPolygon: case HK_GouraudPolygon: n = n_points, step = 2; break;
		case HK_GouraudTexturedPolygon: n = n_points, step = 3; break;
		case HK_MonochromeLine: n = 2; break;
		case HK_GouraudLine: n = 2, step = 2; break;
		case HK_FlatRectangle: case HK_FlatRectangle8x8: case HK_FlatRectangle16x16: case HK_FlatTexturedRectangle:
		case HK_Sprite8x8: case HK_Sprite16x16: case HK_Dot: case HK_TexturedDot: n = 1; break;
		}
		for( int i = 0; i < n; i++ )
			at[ i ] = 1 + i * step;
		return n;
	};

	// Each primitive drawn is matched with one of the frame before's (its first vertex within
	// ScrollGuess::REACH pixels) with the same command: a rectangle or sprite with the same colour and
	// at most one other word of its packet different (its size: a note clipped shorter going off the
	// lane; its texture: an animated one); a polygon or line with the same texture (a shaded one's
	// colours change with its light). The scroll is voted first (hiraku/scroll_guess.h, pair: what
	// is fast may be further than 16 pixels on).
	std::vector<uint32_t> items = m_hk_shown;
	bool moved = false;
	// once a frame: its other refreshes' pictures share the pairing (a screen of identical tiles
	// between scenes made it 18 ms, and at 240 Hz a frame has four or five pictures)
	if( m_hk_moves_frame != m_hk_frames || m_hk_moves_size != items.size() )
	{
		m_hk_moves_frame = m_hk_frames;
		m_hk_moves_size = items.size();
		m_hk_moves.clear();
		std::vector<const uint32_t *> before;
		for( size_t i = 0; i < m_hk_shown_before.size(); i += m_hk_shown_before[ i + 1 ] )
			before.push_back( &m_hk_shown_before[ i ] );
		auto packet = []( const uint32_t *item ) { return reinterpret_cast<const draw_state *>( &item[ 2 ] )->m_packet.n_entry; };
		// the frame before's primitives by their command, colour and place (REACH-sized cells): only
		// those can match (a screen of 943 identical tiles took 10 ms compared every one with every one)
		auto look = [&packet]( const uint32_t *it, int cx, int cy ) {
			const uint32_t op = it[ 0 ] & 0xff, w = packet( it )[ 0 ];
			const bool g = op == HK_GouraudPolygon || op == HK_GouraudTexturedPolygon || op == HK_GouraudLine;
			const uint64_t l = uint64_t( it[ 0 ] ) << 32 | ( g ? w >> 24 : w );
			return l * 0x9e3779b97f4a7c15ull ^ ( uint64_t( uint16_t( cx ) ) << 16 | uint16_t( cy ) );
		};
		auto cell = [&packet, &vertices]( const uint32_t *it, int &cx, int &cy ) {
			int at[ 4 ];
			if( vertices( it[ 0 ] & 0xff, it[ 0 ] >> 8, at ) == 0 )
				return false;
			const uint32_t v = packet( it )[ at[ 0 ] ];
			cx = int( int16_t( v & 0xffff ) ) >> 6, cy = int( int16_t( v >> 16 ) ) >> 6; // floor: REACH is 64
			return true;
		};
		static_assert( hk::ScrollGuess::REACH == 64 );
		std::unordered_map<uint64_t, std::vector<int>> alike;
		for( size_t kb = 0; kb < before.size(); kb++ )
			if( int cx, cy; cell( before[ kb ], cx, cy ) )
				alike[ look( before[ kb ], cx, cy ) ].push_back( (int)kb );
		std::vector<size_t> objects; // each candidate's object: its item
		std::vector<int> number; // ... and its number among the items (as the frame before's are in before)
		int items_n = 0;
		std::vector<hk::ScrollGuess::Candidate> cands;
		for( size_t i = 0; i < items.size(); i += items[ i + 1 ], items_n++ )
		{
			uint32_t *item = &items[ i ];
			int at[ 4 ];
			const int n = vertices( item[ 0 ] & 0xff, item[ 0 ] >> 8, at );
			if( n == 0 )
				continue;
			uint32_t *c = reinterpret_cast<draw_state *>( &item[ 2 ] )->m_packet.n_entry;
			if( n > 1 )
			{
				// a polygon only if it's a rectangle on the screen's axes (a sprite drawn as a quad: an
				// arrow, a note), a line only if it's level or upright (a bar line). A 3D model's
				// polygons, each moving its own way, would come apart at their seams
				int xs[ 4 ], ys[ 4 ], nx = 0, ny = 0;
				for( int v = 0; v < n; v++ )
				{
					const int px = (int16_t)( c[ at[ v ] ] & 0xffff ), py = (int16_t)( c[ at[ v ] ] >> 16 );
					if( std::find( xs, xs + nx, px ) == xs + nx ) xs[ nx++ ] = px;
					if( std::find( ys, ys + ny, py ) == ys + ny ) ys[ ny++ ] = py;
				}
				if( n == 4 ? nx > 2 || ny > 2 : n == 2 ? nx > 1 && ny > 1 : true )
					continue;
			}
			const int words = at[ n - 1 ] + 4 > 12 ? 12 : at[ n - 1 ] + 4; // the packet's words, and a few after
			const int x = (int16_t)( c[ at[ 0 ] ] & 0xffff ), y = (int16_t)( c[ at[ 0 ] ] >> 16 );
			const uint32_t op = item[ 0 ] & 0xff;
			const bool gouraud = op == HK_GouraudPolygon || op == HK_GouraudTexturedPolygon || op == HK_GouraudLine;
			const bool textured = op == HK_FlatTexturedPolygon || op == HK_GouraudTexturedPolygon;
			const int obj = (int)objects.size();
			objects.push_back( i );
			number.push_back( items_n );
			std::vector<int> near; // the frame before's like it in its cell and the eight around
			int cx, cy;
			cell( item, cx, cy );
			for( int ey = -1; ey <= 1; ey++ )
				for( int ex = -1; ex <= 1; ex++ )
					if( const auto same = alike.find( look( item, cx + ex, cy + ey ) ); same != alike.end() )
						near.insert( near.end(), same->second.begin(), same->second.end() );
			std::sort( near.begin(), near.end() ); // in the frame before's order, as compared before
			for( const int kbi : near )
			{
				const size_t kb = (size_t)kbi;
				const uint32_t *b = before[ kb ];
				if( b[ 0 ] != item[ 0 ] ) // the same primitive, with as many points
					continue;
				const uint32_t *p = packet( b );
				if( gouraud ? ( p[ 0 ] >> 24 ) != ( c[ 0 ] >> 24 ) : p[ 0 ] != c[ 0 ] )
					continue;
				int other = 0;
				if( n > 1 )
				{
					// polygons and lines: the same texture (each vertex's word after its coordinates);
					// a shaded one's colours change with its light as a model moves
					for( int v = 0; textured && v < n; v++ )
						other += p[ at[ v ] + 1 ] != c[ at[ v ] + 1 ] ? 2 : 0;
				}
				else
				{
					for( int w = 1, v = 0; w < words && other < 2; w++ )
					{
						if( v < n && w == at[ v ] )
						{
							v++;
							continue;
						}
						other += p[ w ] != c[ w ];
					}
				}
				if( other > 1 )
					continue;
				const int bx = (int16_t)( p[ at[ 0 ] ] & 0xffff ), by = (int16_t)( p[ at[ 0 ] ] >> 16 );
				if( std::abs( x - bx ) > hk::ScrollGuess::REACH || std::abs( y - by ) > hk::ScrollGuess::REACH )
					continue;
				// the one whose other vertices moved most as the first did (a model's polygons crowd each
				// other; the same polygon keeps its shape), a shaded one's colours closest; carried on
				// only if it moved whole, every vertex by the same step (within a pixel): what changes
				// shape (grows, turns) stays as the game drew it
				const int dx = x - bx, dy = y - by;
				int cost = 8 * other;
				bool whole = true;
				for( int v = 0; v < n; v++ )
				{
					const int ex = (int16_t)( c[ at[ v ] ] & 0xffff ) - (int16_t)( p[ at[ v ] ] & 0xffff ) - dx;
					const int ey = (int16_t)( c[ at[ v ] ] >> 16 ) - (int16_t)( p[ at[ v ] ] >> 16 ) - dy;
					cost += std::abs( ex ) + std::abs( ey );
					whole = whole && std::abs( ex ) <= 1 && std::abs( ey ) <= 1;
					for( int k = 0; gouraud && k < 24; k += 8 )
						cost += std::abs( int( ( c[ at[ v ] - 1 ] >> k ) & 0xff ) - int( ( p[ at[ v ] - 1 ] >> k ) & 0xff ) ) / 16;
				}
				cands.push_back( { obj, (int)kb, dx, dy, cost, whole } );
			}
		}
		const std::vector<int> paired = m_hk_scroll.pair( m_hk_frames, (int)objects.size(), cands );
		// each item's move and its number in the frame before (its own track), by its number
		std::vector<std::pair<int, int>> steps( items_n, { 0, 0 } );
		std::vector<int> was( items_n, -1 );
		for( size_t o = 0; o < objects.size(); o++ )
		{
			if( paired[ o ] < 0 )
				continue;
			const hk::ScrollGuess::Candidate &m = cands[ (size_t)paired[ o ] ];
			if( !m.whole )
				continue;
			was[ number[ o ] ] = m.from;
			steps[ number[ o ] ] = { m.dx, m.dy };
			if( m.dx != 0 || m.dy != 0 )
				m_hk_moves.push_back( { objects[ o ], number[ o ], m.dx, m.dy } );
		}
		// carried on: what scrolls with the frame's other moving items along the scroll's steady line
		// (hiraku/scroll_guess.h: notes step whole pixels, and carried on by their last step they
		// judder), the rest along its own track's, else by its last move
		m_hk_scroll.frame( m_hk_frames, steps, was );
	}
	for( const auto &m : m_hk_moves )
	{
		int mx, my;
		m_hk_scroll.offset( m.dx, m.dy, on, mx, my, m.n );
		if( mx == 0 && my == 0 )
			continue;
		moved = true;
		uint32_t *item = &items[ m.i ];
		int at[ 4 ];
		const int n = vertices( item[ 0 ] & 0xff, item[ 0 ] >> 8, at );
		uint32_t *c = reinterpret_cast<draw_state *>( &item[ 2 ] )->m_packet.n_entry;
		for( int v = 0; v < n; v++ )
		{
			const uint32_t w = c[ at[ v ] ];
			c[ at[ v ] ] = ( uint32_t( uint16_t( int16_t( w >> 16 ) + my ) ) << 16 ) | uint16_t( int16_t( w & 0xffff ) + mx );
		}
	}

	if( !moved )
		return false; // nothing on by a whole pixel yet: the frame as it is
	// the uploads since the frame on show undone, newest first (and done again at the end)
	std::vector<std::pair<hk_area, std::vector<uint16_t>>> redo;
	const uint64_t from = m_hk_frame_at.load();
	for( auto u = m_hk_undo.rbegin(); u != m_hk_undo.rend() && u->at >= from; ++u )
	{
		std::vector<uint16_t> now;
		if( u->a.w > 0 && u->a.h > 0 && u->a.h <= m_hk_rows )
		{
			// no pixel twice (the area is at most 1024 columns, and no taller than VRAM): its
			// pixels as they are kept, then the kept ones put back, as the loop below does one by one
			now.resize( u->pixels.size() );
			hk_area_copy( p_p_vram, u->a, now.data(), false );
			hk_area_copy( p_p_vram, u->a, const_cast<uint16_t *>( u->pixels.data() ), true );
		}
		else
		{
			now.reserve( u->pixels.size() );
			size_t k = 0;
			for( int32_t y = 0; y < u->a.h; y++ )
				for( int32_t x = 0; x < u->a.w; x++ )
				{
					uint16_t &px = p_p_vram[ ( u->a.y + y ) & 1023 ][ ( u->a.x + x ) & 1023 ];
					now.push_back( px );
					px = u->pixels[ k++ ];
				}
		}
		redo.emplace_back( u->a, std::move( now ) );
	}

	// the rows the frame draws on kept as they are, the frame drawn again, composed, and the rows
	// put back
	std::vector<uint8_t> kept( 1024, 0 );
	std::vector<uint32_t> rows;
	std::vector<uint16_t> pixels;
	for( size_t i = 0; i < items.size(); i += items[ i + 1 ] )
	{
		const hk_area a = hk_item_area( &items[ i ] );
		for( int32_t r = 0; r < std::min<int32_t>( a.h, 1024 ); r++ )
		{
			const uint32_t y = uint32_t( a.y + r ) & 1023;
			if( kept[ y ] )
				continue;
			kept[ y ] = 1;
			rows.push_back( y );
			pixels.insert( pixels.end(), p_p_vram[ y ], p_p_vram[ y ] + 1024 );
		}
	}
	auto r = std::make_unique<raster>();
	std::copy_n( p_p_vram, 1024, r->p_p_vram );
	for( size_t i = 0; i < items.size(); i += items[ i + 1 ] )
		hk_dispatch( *r, &items[ i ] );
	bitmap.resize( screen.m_bitmap.width(), screen.m_bitmap.height() );
	update_screen( screen, bitmap, screen.visible_area() );
	for( size_t i = rows.size(); i-- > 0; ) // in reverse: rows beyond VRAM's height alias
		std::copy_n( &pixels[ i * 1024 ], 1024, p_p_vram[ rows[ i ] ] );
	for( auto r = redo.rbegin(); r != redo.rend(); ++r ) // oldest first
	{
		if( r->first.w > 0 && r->first.h > 0 )
			hk_area_copy( p_p_vram, r->first, r->second.data(), true );
	}
	return true;
}
'''


def video_psx_between_cpp(s):
    # hk_run draws through hk_dispatch()
    s = cut(s, '''		if( op != HK_SKIP )
			static_cast<draw_state &>( r ) = *reinterpret_cast<const draw_state *>( &item[ 2 ] );
		switch( op )
''', '''		tail += item[ 1 ];
		m_hk_tail.store( tail, std::memory_order_seq_cst );''', '''		if( op != HK_SKIP )
		{
			if( m_hk_undo_on.load( std::memory_order_relaxed ) && ( op == HK_IMAGE || op == HK_MoveImage || op == HK_FrameBufferRectangleDraw ) )
				hk_keep_undo( item, tail ); // in-between pictures
			hk_dispatch( r, item );
		}
''', 'video/psx.cpp between hk_run')
    s = edit(s, '''		const int n_points = item[ 0 ] >> 8;
		if( op != HK_SKIP )
		{
''', '''		if( op != HK_SKIP )
		{
''', 'video/psx.cpp between hk_run n_points')
    # hk_publish's rectangle from hk_item_area()
    s = cut(s, '''		const draw_state &d = *reinterpret_cast<const draw_state *>( &item[ 2 ] );
		const PACKET &p = d.m_packet;
		hk_area a;
''', '''		if( a.w > 0 && a.h > 0 )''', '''		const hk_area a = hk_item_area( item );
''', 'video/psx.cpp between hk_publish area')
    # each frame's items kept
    s = edit(s, '''	m_hk_write += item[ 1 ];
	m_hk_image = nullptr;
''', '''	if( m_hk_keep ) // in-between pictures: the frame's items
		m_hk_drawing.insert( m_hk_drawing.end(), item, item + item[ 1 ] );
	m_hk_write += item[ 1 ];
	m_hk_image = nullptr;
''', 'video/psx.cpp between keep')
    # a new frame at VBLANK start, which the game's VBLANK interrupt then flips to show
    s = edit(s, '''		m_vblank_handler(1);
''', '''		if( m_hk_keep ) // HIRAKU: in-between pictures
		{
			m_hk_vblanks++;
			if( ++m_hk_no_flip > 16 && !m_hk_drawing.empty() ) // a game that doesn't flip: a frame a VBLANK
				hk_new_frame( 1 );
		}
		m_vblank_handler(1);
''', 'video/psx.cpp between vblank')
    # a flip: the display start changed
    s = edit(s, """			LOGMASKED(LOG_WRITE, "%s: start of display area %d %d\\n", machine().describe_context(), m_n_displaystartx, n_displaystarty);
""", """			LOGMASKED(LOG_WRITE, "%s: start of display area %d %d\\n", machine().describe_context(), m_n_displaystartx, n_displaystarty);
			if( m_hk_keep && ( m_n_displaystartx | n_displaystarty << 16 ) != m_hk_display ) // HIRAKU: in-between pictures
			{
				m_hk_display = m_n_displaystartx | n_displaystarty << 16;
				if( !m_hk_drawing.empty() )
					hk_new_frame( m_hk_vblanks );
				m_hk_no_flip = 0;
			}
""", 'video/psx.cpp between flip')
    # the frame-time report's GPU part (runtime/core/prof.h): its commands and scanout
    s = edit(s, '#include "psx.h"\n',
             '#include "psx.h"\n#include "core/prof.h" // HIRAKU: the frame\'s time by part\n', 'video/psx.cpp prof include')
    s = edit(s, 'uint32_t psxgpu_device::update_screen(screen_device &screen, bitmap_rgb32 &bitmap, const rectangle &cliprect)\n{\n',
             'uint32_t psxgpu_device::update_screen(screen_device &screen, bitmap_rgb32 &bitmap, const rectangle &cliprect)\n{\n\thk::prof::Scope hk_ps( hk::prof::GPU ); // HIRAKU\n', 'video/psx.cpp update_screen scope')
    s = edit(s, 'void psxgpu_device::gpu_write( uint32_t *p_ram, int32_t n_size )\n{\n',
             'void psxgpu_device::gpu_write( uint32_t *p_ram, int32_t n_size )\n{\n\thk::prof::Scope hk_ps( hk::prof::GPU ); // HIRAKU\n', 'video/psx.cpp gpu_write scope')
    return s.rstrip('\n') + '\n' + BETWEEN_CPP


def atapihle_cpp(s):
    # a packet's byte count limit below one sector (the multi-session unit's firmware asks
    # for 8 bytes of REQUEST SENSE's 18): MAME rounds the chunk down to a whole sector, 0,
    # and offers no data at all. As windyfairy's System 573 fork does: send all of it.
    return edit(s, """			if (m_buffer_size % m_sector_bytes)
				m_buffer_size = m_buffer_size - (m_buffer_size % m_sector_bytes);
""", """			if (m_buffer_size % m_sector_bytes)
			{
				// HIRAKU: below one sector, all of it (windyfairy's System 573 fork)
				const uint32_t whole = m_buffer_size - (m_buffer_size % m_sector_bytes);
				m_buffer_size = whole ? whole : m_data_size;
			}
""", 'fill_buffer sector rounding')


def ins8250_cpp(s):
    # the TX FIFO by head and tail alone: 16 bytes queued (a THRE handler filling the FIFO
    # while the last byte still shifts out, as the multi-session unit's and Keyboardmania's
    # firmware do) look like none, and all 16 are lost. A count, as the RX FIFO has.
    s = edit(s, """	if(m_ttail != m_thead)
	{
		transmit_register_setup(m_tfifo[m_ttail]);
		++m_ttail &= 0x0f;
		m_regs.lsr &= ~INS8250_LSR_TSRE;
		if(m_ttail == m_thead)
""", """	if(m_tnum) // HIRAKU: by count, a full FIFO isn't an empty one
	{
		transmit_register_setup(m_tfifo[m_ttail]);
		++m_ttail &= 0x0f;
		m_tnum--;
		m_regs.lsr &= ~INS8250_LSR_TSRE;
		if(!m_tnum)
""", 'tra_complete count')
    s = edit(s, """	m_tfifo[m_thead] = data;
	++m_thead &= 0x0f;
""", """	if(m_tnum == 16) // HIRAKU: a write to a full FIFO is lost
		return;
	m_tfifo[m_thead] = data;
	++m_thead &= 0x0f;
	m_tnum++;
""", 'push_tx count')
    s = edit(s, """	save_item(NAME(m_ttail));
""", """	save_item(NAME(m_ttail));
	save_item(NAME(m_tnum));
""", 'save m_tnum')
    s = edit(s, """	m_thead = m_ttail = 0;
	m_timeout->adjust""", """	m_thead = m_ttail = m_tnum = 0;
	m_timeout->adjust""", 'reset m_tnum')
    s = edit(s, """		std::fill(std::begin(m_tfifo), std::end(m_tfifo), 0);
		m_thead = m_ttail = 0;
""", """		std::fill(std::begin(m_tfifo), std::end(m_tfifo), 0);
		m_thead = m_ttail = m_tnum = 0;
""", 'fcr clear m_tnum')
    return s


def ins8250_h(s):
    return edit(s, """	int m_thead, m_ttail;
""", """	int m_thead, m_ttail, m_tnum; // HIRAKU: m_tnum
""", 'm_tnum')


def minimp3_h(s):
    # On AArch64 the NEON path rounds the 16-bit output as the SSE path does (clamp, then round
    # to nearest even), not half away from zero: the same samples on every host.
    def neon(s, indent, store, name):
        i = ' ' * indent
        old = (f"{i}a = VADD(a, VSET(0.5f));\n"
               f"{i}b = VADD(b, VSET(0.5f));\n"
               f"{i}pcma = vqmovn_s32(vqaddq_s32(vcvtq_s32_f32(a), vreinterpretq_s32_u32(vcltq_f32(a, VSET(0)))));\n"
               f"{i}pcmb = vqmovn_s32(vqaddq_s32(vcvtq_s32_f32(b), vreinterpretq_s32_u32(vcltq_f32(b, VSET(0)))));\n"
               f"{i}{store}\n")
        new = ("#if defined(__aarch64__) || defined(_M_ARM64)\n"
               f"{i}/* HIRAKU: as the SSE path (clamp, round to nearest even), for the same samples on every host */\n"
               f"{i}pcma = vqmovn_s32(vcvtnq_s32_f32(vmaxq_f32(vminq_f32(a, VSET(32767.0f)), VSET(-32768.0f))));\n"
               f"{i}pcmb = vqmovn_s32(vcvtnq_s32_f32(vmaxq_f32(vminq_f32(b, VSET(32767.0f)), VSET(-32768.0f))));\n"
               "#else\n" + old[:-len(f"{i}{store}\n")] + "#endif\n" + f"{i}{store}\n")
        return edit(s, old, new, name)
    s = neon(s, 12, 'vst1_lane_s16(dstr + (15 - i)*nch, pcma, 1);', 'minimp3 synth rounding')
    return neon(s, 8, 'vst1_lane_s16(out+i  , pcma, 0);', 'minimp3 f32_to_s16 rounding')


def rs232_cpp(s):
    # Twinkle: its RS-232 ports hold the DVD player (xvd701) or nothing. MAME's default option
    # list names ~20 serial devices (terminals, printers, pty...) that HIRAKU doesn't carry.
    s = cut(s, '#include "adsp2181ekl.h"', 'void default_rs232_devices(device_slot_interface &device)',
            '// HIRAKU: no serial devices of MAME\'s default list (the explicit instantiations of\n'
            '// MAME\'s two-parameter device_finder go too: the shim\'s finder has one)\n\n', 'rs232.cpp includes')
    s = cut(s, '\tdevice.option_add("adsp2181ekl",', '}',
            '\t// HIRAKU: none (see above)\n', 'rs232.cpp default devices')
    return s


def xvd701_h(s):
    # Twinkle's DVD player: its state after each command, for HIRAKU's picture (PLAN.md section 21,
    # TW-3). MAME models the serial protocol and shows nothing.
    s = edit(s, '\tvirtual void input_txd(int state) override { device_serial_interface::rx_w(state); }\n',
             '\tvirtual void input_txd(int state) override { device_serial_interface::rx_w(state); }\n'
             '\n'
             '\t// HIRAKU: the player\'s state after each command it takes: the chapter, STOP/PLAYING/PAUSE\n'
             '\t// (0/1/2), the power, and whether playback starts again at the chapter\'s beginning\n'
             '\tstd::function<void(int chapter, int status, bool powered, bool from_start)> m_hk_state;\n'
             '\t// HIRAKU: each command the player takes, its 11 bytes, before it acts on it (a DVD\'s\n'
             '\t// chapter: MAME reads the number as a Video CD\'s, divided by ten)\n'
             '\tstd::function<void(const unsigned char *command)> m_hk_command;\n',
             'xvd701.h hook')
    s = edit(s, '\tint m_chapter;\n',
             '\tint m_chapter;\n\tbool m_hk_seek = false; // HIRAKU: this command (re)started the chapter\n', 'xvd701.h seek flag')
    return s


def xvd701_cpp(s):
    s = edit(s, '\tm_chapter = chapter;\n', '\tm_chapter = chapter;\n\tm_hk_seek = true; // HIRAKU\n', 'xvd701.cpp seek')
    s = edit(s, '\t\tm_command[10] == sum(m_command, sizeof(m_command) - 1))\n\t{\n',
             '\t\tm_command[10] == sum(m_command, sizeof(m_command) - 1))\n\t{\n\t\tm_hk_seek = false; // HIRAKU\n'
             '\t\tif (m_hk_command) m_hk_command(m_command); // HIRAKU\n',
             'xvd701.cpp command start')
    s = edit(s, '\t\t\tcreate_packet(STATUS_OK, response);\n\t\t}\n\t}\n}\n\nDEFINE_DEVICE_TYPE(JVC_XVD701',
             '\t\t\tcreate_packet(STATUS_OK, response);\n\t\t}\n\n'
             '\t\tif (m_hk_state) m_hk_state(m_chapter, m_playback_status, m_is_powered, m_hk_seek); // HIRAKU\n'
             '\t}\n}\n\nDEFINE_DEVICE_TYPE(JVC_XVD701', 'xvd701.cpp command end')
    return s


def mp3_audio_h(s):
    s = edit(s, '''	void clear();

private:''', '''	void clear();

	// HIRAKU: this decoder's state made a copy of `o`'s (not its data: each decodes from its own
	// base), so mas3507d's hk_predict() can decode ahead on a copy, leaving the real one as it was
	void hk_copy_state(const mp3_audio &o);
	bool hk_found_stream() const { return m_found_stream; }

private:''', 'mp3_audio.h hk_copy_state')
    return s


def mp3_audio_cpp(s):
    s = edit(s, '''void mp3_audio::clear()
{
	mp3dec_init(dec.get());
	m_found_stream = false;
}
''', '''void mp3_audio::clear()
{
	mp3dec_init(dec.get());
	m_found_stream = false;
}

// HIRAKU
void mp3_audio::hk_copy_state(const mp3_audio &o)
{
	*dec = *o.dec;
	m_found_stream = o.m_found_stream;
}
''', 'mp3_audio.cpp hk_copy_state')
    return s


def mas3507d_h(s):
    s = edit(s, '''	void reset_playback();

protected:''', '''	void reset_playback();

	// HIRAKU: the first `n` samples (fewer: as far as is certain) the next sound_stream_update()
	// will put out, worked out ahead of it on copies; for the host's --fine-sound
	// (runtime/sys573/board.cpp). Returns how many are ready, at `l` and `r`.
	int hk_predict(int n, const float *&l, const float *&r);

protected:''', 'mas3507d.h hk_predict')
    s = edit(s, '''	std::unique_ptr<mp3_audio> mp3dec;
};''', '''	std::unique_ptr<mp3_audio> mp3dec;

	// HIRAKU: hk_predict()'s copies, valid until the stream next updates (or the data, the
	// gains or the playback change)
	bool m_hk_valid = false, m_hk_end = false;
	std::vector<float> m_hk_out[2];
	std::array<uint8_t, 0xe00> m_hk_data;
	std::array<short, 1152*2> m_hk_samples;
	uint32_t m_hk_data_count = 0;
	int32_t m_hk_sample_count = 0, m_hk_samples_idx = 0, m_hk_channels = 2;
	std::unique_ptr<mp3_audio> m_hk_dec;
};''', 'mas3507d.h members')
    return s


MAS3507D_PREDICT = r'''
// HIRAKU: the samples the next sound_stream_update() will put out first, ahead of it, for the
// host's --fine-sound (runtime/sys573/board.cpp). The decoder decodes a frame when its stream
// updates and asks for more data then, which the board sees, so the stream can't be updated
// sooner; this runs fill_buffer() and append_buffer() on copies (the decoder's state, its data
// and its frame) and leaves the device as it was. It stops where the outcome isn't certain: a
// frame the copy can't decode while a stream plays (its data may still be on the way), or one at
// another sample rate. Before a stream is found (nothing plays) the update puts out silence, and
// so does this, until data comes.
int mas3507d_device::hk_predict(int n, const float *&l, const float *&r)
{
	if (!m_hk_dec)
		m_hk_dec = std::make_unique<mp3_audio>(m_hk_data.data());
	if (!m_hk_valid) {
		m_hk_valid = true;
		m_hk_end = false;
		m_hk_out[0].clear();
		m_hk_out[1].clear();
		m_hk_data = mp3data;
		m_hk_data_count = mp3data_count;
		m_hk_dec->hk_copy_state(*mp3dec);
		m_hk_samples = samples;
		m_hk_sample_count = sample_count;
		m_hk_samples_idx = samples_idx;
		m_hk_channels = frame_channels;
	}

	const sound_stream::sample_t sample_scale = 1.0 / 32768.0;
	const sound_stream::sample_t mute_scale = is_muted ? 0.0 : 1.0;
	while (int(m_hk_out[0].size()) < n && !m_hk_end) {
		if (m_hk_sample_count == 0) {
			int pos = 0, frame_sample_rate = 0;
			bool decoded_frame = m_hk_dec->decode_buffer(pos, m_hk_data_count, &m_hk_samples[0], m_hk_sample_count, frame_sample_rate, m_hk_channels);
			m_hk_samples_idx = 0;
			if (!decoded_frame || m_hk_sample_count == 0) {
				if (m_hk_sample_count <= 0) {
					if (m_hk_dec->hk_found_stream()) {
						m_hk_end = true;   // data may still come before the update
						break;
					}
					m_hk_out[0].resize(n, 0.0f);   // no stream yet: silence
					m_hk_out[1].resize(n, 0.0f);
					break;
				}
			} else {
				std::copy(m_hk_data.begin() + pos, m_hk_data.end(), m_hk_data.begin());
				m_hk_data_count -= pos;
				if (u32(frame_sample_rate) != stream->sample_rate()) {
					m_hk_end = true;   // the update changes the stream's rate here
					break;
				}
			}
		}

		const int bytes_per_sample = std::min(m_hk_channels, 2);
		while (m_hk_sample_count > 0 && int(m_hk_out[0].size()) < n) {
			m_hk_out[0].push_back(sound_stream::sample_t(m_hk_samples[m_hk_samples_idx * bytes_per_sample]) * sample_scale * mute_scale * gain_ll);
			m_hk_out[1].push_back(sound_stream::sample_t(m_hk_samples[m_hk_samples_idx * bytes_per_sample + (bytes_per_sample >> 1)]) * sample_scale * mute_scale * gain_rr);
			if (++m_hk_samples_idx >= m_hk_sample_count)
				m_hk_sample_count = 0;
		}
	}
	l = m_hk_out[0].data();
	r = m_hk_out[1].data();
	return int(std::min<size_t>(n, m_hk_out[0].size()));
}
'''


def mas3507d_cpp(s):
    # the prediction starts again whenever what it copied changes
    s = edit(s, '''	mp3data[mp3data_count++] = byte;

	cb_demand(mp3data_count < mp3data.size());
}''', '''	mp3data[mp3data_count++] = byte;
	if (m_hk_end || (m_hk_valid && !mp3dec->hk_found_stream()))
		m_hk_valid = false;   // HIRAKU: hk_predict() may get further

	cb_demand(mp3data_count < mp3data.size());
}''', 'mas3507d.cpp sid_w')
    s = edit(s, '''		gain_ll = gain_to_percentage(val);''', '''		gain_ll = gain_to_percentage(val);
		m_hk_valid = false; // HIRAKU''', 'mas3507d.cpp gain_ll')
    s = edit(s, '''		gain_rr = gain_to_percentage(val);''', '''		gain_rr = gain_to_percentage(val);
		m_hk_valid = false; // HIRAKU''', 'mas3507d.cpp gain_rr')
    s = edit(s, '''		is_muted = val == 1;''', '''		is_muted = val == 1;
		m_hk_valid = false; // HIRAKU''', 'mas3507d.cpp mute')
    s = edit(s, '''void mas3507d_device::reset_playback()
{''', '''void mas3507d_device::reset_playback()
{
	m_hk_valid = false; // HIRAKU''', 'mas3507d.cpp reset_playback')
    s = edit(s, '''void mas3507d_device::sound_stream_update(sound_stream &stream)
{''', MAS3507D_PREDICT.lstrip('\n') + '''
void mas3507d_device::sound_stream_update(sound_stream &stream)
{
	m_hk_valid = false; // HIRAKU''', 'mas3507d.cpp hk_predict')
    return s
def bus_nscsi_cd_h(s):
    s = edit(s, """	nscsi_cdrom_device(const machine_config &mconfig, const char *tag, device_t *owner, uint32_t clock = 0);
""", """	nscsi_cdrom_device(const machine_config &mconfig, const char *tag, device_t *owner, uint32_t clock = 0);

	// HIRAKU: the drive's read speed. With sectors_per_second > 0, READ's data comes a sector at
	// a time, each when the drive would have read it (the first after one sector's time), at the
	// bus's speed within a sector; 0 (the default) is MAME's model: every byte as fast as the bus
	// takes it
	void hk_set_read_rate(uint32_t sectors_per_second) { m_hk_rate = sectors_per_second; }
""", 'bus/nscsi/cd.h rate')
    s = edit(s, """	virtual void scsi_put_data(int buf, int offset, uint8_t data) override;
	virtual bool scsi_command_done(uint8_t command, uint8_t length) override;
""", """	virtual void scsi_put_data(int buf, int offset, uint8_t data) override;
	virtual bool scsi_command_done(uint8_t command, uint8_t length) override;
	virtual attotime scsi_data_byte_period() override; // HIRAKU
	virtual attotime scsi_data_command_delay() override; // HIRAKU
	void hk_read_begins(int size);
""", 'bus/nscsi/cd.h overrides')
    s = edit(s, """	std::vector<uint8_t> m_xfer_buffer;
};
""", """	std::vector<uint8_t> m_xfer_buffer;

	// HIRAKU: the read in progress, for the read speed
	uint32_t m_hk_rate = 0;
	bool m_hk_reading = false;
	int m_hk_size = 0, m_hk_sent = 0;
	attotime m_hk_start;
};
""", 'bus/nscsi/cd.h state')
    return s


def bus_nscsi_cd_cpp(s):
    n = s.count("\t\tscsi_data_in(2, blocks*bytes_per_block);\n")
    if n != 2:
        sys.exit(f"hiraku-edits: bus/nscsi/cd.cpp READ: anchor found {n} times, expected twice")
    s = s.replace("\t\tscsi_data_in(2, blocks*bytes_per_block);\n",
                  "\t\thk_read_begins(blocks*bytes_per_block); // HIRAKU\n\t\tscsi_data_in(2, blocks*bytes_per_block);\n")
    s = edit(s, """	if(id != 2)
		return nscsi_full_device::scsi_get_data(id, pos);
""", """	if(id != 2)
		return nscsi_full_device::scsi_get_data(id, pos);
	m_hk_sent = pos + 1; // HIRAKU: the read's progress
""", 'bus/nscsi/cd.cpp progress')
    s = edit(s, """void nscsi_cdrom_device::scsi_command()
{
""", """// HIRAKU: the drive's read speed (hk_set_read_rate())
void nscsi_cdrom_device::hk_read_begins(int size)
{
	m_hk_reading = m_hk_rate != 0 && size > 0;
	m_hk_size = size;
	m_hk_sent = 0;
	m_hk_start = machine().time();
}

// the data phase starts when the first sector is read
attotime nscsi_cdrom_device::scsi_data_command_delay()
{
	if (m_hk_reading && m_hk_sent == 0)
		return attotime::from_ticks(1, m_hk_rate);
	return nscsi_full_device::scsi_data_command_delay();
}

// after the last byte of a sector, the next one waits until the drive has read it: sector k (from
// 0) is there one sector's time after sector k - 1, the first one a sector's time after the command
attotime nscsi_cdrom_device::scsi_data_byte_period()
{
	if (m_hk_reading)
	{
		if (m_hk_sent >= m_hk_size)
			m_hk_reading = false; // the status phase follows at once
		else if (m_hk_sent % bytes_per_sector == 0)
		{
			const attotime ready = m_hk_start + attotime::from_ticks(m_hk_sent / bytes_per_sector + 1, m_hk_rate);
			const attotime now = machine().time();
			if (ready > now)
				return ready - now;
		}
	}
	return nscsi_full_device::scsi_data_byte_period();
}

void nscsi_cdrom_device::scsi_command()
{
	m_hk_reading = false; // HIRAKU
""", 'bus/nscsi/cd.cpp model')
    return s


# the ATA hard disk's DMA a buffer at a time (Twinkle's SPU board loads its keysounds by DMA,
# word by word through the interface's slots, two virtual calls and four mode checks each)
ATA_BURST_DOC = """	// HIRAKU: what read_dma() would give for as many words as it can take from the device's
	// buffer with no other effect, at once; 0 when it can't say (then read_dma())
"""


def bus_ata_atadev_h(s):
    return edit(s, """	virtual void read_dma(PAIR16 &data) = 0;
""", """	virtual void read_dma(PAIR16 &data) = 0;
""" + ATA_BURST_DOC + """	virtual int hk_read_dma_burst(uint16_t *dst, int max) { return 0; }
""", 'bus/ata/atadev.h burst')


def bus_ata_hdd_h(s):
    return edit(s, """	ide_hdd_device(const machine_config &mconfig, const char *tag, device_t *owner, uint32_t clock = 0);

	// device_ata_interface implementation
	virtual void read_dma(PAIR16 &data) override { dma_r(data); }
""", """	ide_hdd_device(const machine_config &mconfig, const char *tag, device_t *owner, uint32_t clock = 0);

	// device_ata_interface implementation
	virtual void read_dma(PAIR16 &data) override { dma_r(data); }
	virtual int hk_read_dma_burst(uint16_t *dst, int max) override { return hk_dma_burst(dst, max); } // HIRAKU
""", 'bus/ata/hdd.h burst')


def bus_ata_ataintf_h(s):
    return edit(s, """	uint16_t read_dma();
""", """	uint16_t read_dma();
""" + ATA_BURST_DOC + """	int hk_read_dma_burst(uint16_t *dst, int max);
""", 'bus/ata/ataintf.h burst')


def bus_ata_ataintf_cpp(s):
    return edit(s, """uint16_t abstract_ata_interface_device::internal_read_cs0(offs_t offset)
""", """// HIRAKU: the selected device's (the others leave read_dma()'s data alone)
int abstract_ata_interface_device::hk_read_dma_burst(uint16_t *dst, int max)
{
	for (auto & elem : m_slot)
		if (elem->dev() != nullptr)
			if (int n = elem->dev()->hk_read_dma_burst(dst, max))
				return n;
	return 0;
}

uint16_t abstract_ata_interface_device::internal_read_cs0(offs_t offset)
""", 'bus/ata/ataintf.cpp burst')


def machine_atahle_h(s):
    return edit(s, """	void dma_r(PAIR16 &data);
""", """	void dma_r(PAIR16 &data);
	int hk_dma_burst(uint16_t *dst, int max); // HIRAKU
""", 'machine/atahle.h burst')


def machine_atahle_cpp(s):
    s = edit(s, '#include "atahle.h"\n', '#include "atahle.h"\n\n#include <bit> // HIRAKU: hk_dma_burst\n#include <cstring>\n', 'machine/atahle.cpp includes')
    return edit(s, """void device_ata_hle_interface::command_r(offs_t offset, PAIR16 &data, uint16_t mem_mask)
""", """// HIRAKU: dma_r() for the words before the buffer's last, at once: each takes dma_r()'s
// read_data() branch (nothing those checks look at changes until the buffer ends) and only moves
// m_buffer_offset on. The last word, which refills the buffer or ends the transfer, and any case
// with another effect (single-word DMA's DMARQ) are left to dma_r().
int device_ata_hle_interface::hk_dma_burst(uint16_t *dst, int max)
{
	if (!device_selected() || !m_dmack || m_8bit_data_transfers || device().machine().side_effects_disabled())
		return 0;
	if (single_word_dma_mode() >= 0 || (!m_dmarq && (multi_word_dma_mode() >= 0 || ultra_dma_mode() >= 0)))
		return 0;
	if ((m_status & IDE_STATUS_BSY) || !(m_status & IDE_STATUS_DRQ))
		return 0;
	// the words dma_r() would take one at a time (while offset + 2 < size): on a little-endian
	// host each is its two bytes as they are
	const int room = int(m_buffer_size) - int(m_buffer_offset) - 2;
	const int n = room > 0 ? std::min(max, (room + 1) / 2) : 0;
	if constexpr (std::endian::native == std::endian::little)
		memcpy(dst, &m_buffer[m_buffer_offset], 2 * size_t(n));
	else
		for (int i = 0; i < n; i++)
			dst[i] = m_buffer[m_buffer_offset + 2 * i] | (m_buffer[m_buffer_offset + 2 * i + 1] << 8);
	m_buffer_offset += 2 * n;
	return n;
}

void device_ata_hle_interface::command_r(offs_t offset, PAIR16 &data, uint16_t mem_mask)
""", 'machine/atahle.cpp burst')


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    root = Path(sys.argv[1])
    for rel, fn in [('sound/swp30.cpp', swp30_cpp), ('sound/swp30.h', swp30_h),
                    ('cpu/psx/psx.cpp', psx_cpp), ('cpu/psx/psx.h', psx_h),
                    ('cpu/psx/mdec.cpp', mdec_cpp), ('cpu/psx/mdec.h', mdec_h),
                    ('video/psx.cpp', video_psx_cpp), ('video/psx.h', video_psx_h),
                    ('bus/ata/atapihle.cpp', atapihle_cpp),
                    ('machine/ins8250.cpp', ins8250_cpp), ('machine/ins8250.h', ins8250_h),
                    ('../3rdparty/minimp3/minimp3.h', minimp3_h),
                    ('bus/rs232/rs232.cpp', rs232_cpp),
                    ('bus/rs232/xvd701.h', xvd701_h), ('bus/rs232/xvd701.cpp', xvd701_cpp),
                    ('sound/mp3_audio.h', mp3_audio_h), ('sound/mp3_audio.cpp', mp3_audio_cpp),
                    ('sound/mas3507d.h', mas3507d_h), ('sound/mas3507d.cpp', mas3507d_cpp),
                    ('bus/nscsi/cd.h', bus_nscsi_cd_h), ('bus/nscsi/cd.cpp', bus_nscsi_cd_cpp),
                    ('bus/ata/atadev.h', bus_ata_atadev_h), ('bus/ata/hdd.h', bus_ata_hdd_h),
                    ('bus/ata/ataintf.h', bus_ata_ataintf_h), ('bus/ata/ataintf.cpp', bus_ata_ataintf_cpp),
                    ('machine/atahle.h', machine_atahle_h), ('machine/atahle.cpp', machine_atahle_cpp)]:
        p = root / rel
        text = p.read_text()
        if 'HIRAKU' in text:
            sys.exit(f'hiraku-edits: {p} already edited (start from a pristine copy)')
        p.write_text(fn(text))
        print(f'edited {p}')


if __name__ == '__main__':
    main()
