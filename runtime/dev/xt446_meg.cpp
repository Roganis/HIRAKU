// SPDX-License-Identifier: GPL-3.0-or-later
// The SWP30's MEG programs made into the host's own code (PLAN.md section 45): the part of
// Keyboardmania's XT446 card that costs most. A development build compiles the firmware's effect
// programs with HIRAKU (tools/meggen.py, from a recording); a release has no recording, so here a
// program is made at run time from the stencils of runtime/jit/meg_stencils.c, once it has run
// unchanged for a few samples.
//
// What the made code must equal is swp30.cpp's step_decoded(), state for state: each step below
// is that function with the step's fields decided here instead of tested there. Three things are
// decided that the interpreter tests on every step:
// - what the steps 3 and 2 before left in the delay slots (they are the same program's; the
//   program's first steps, whose slots are the previous sample's, still test them);
// - the slots' flags, which only those tests read: written by the program's last steps alone;
// - a T value nobody reads two steps later: not kept (the last two steps' always are).
// So the code is entered at any step but always runs to the program's end, where the state is
// whole again for whoever runs next; a run that stops short is the interpreter's.
//
// HK_MEG_JIT=0 (or HK_JIT=0): none of this, the programs interpreted. HK_MEG_STATS=1: at exit,
// how many steps ran each way.
#include "emu.h"

#include <bit>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "jit/jit.h"
#include "sound/swp30.h"

#ifdef HK_MEG_STENCILS
#include "hk_meg_stencils.inc"
#endif

struct swp30_device::meg_state::jit_prog {
	void *mem = nullptr;
	size_t size = 0;
	int k0 = 0;                                    // the pipeline phase at step 0 it was made for
	std::array<u64, 0x180> program{};              // the words it was made from
	std::array<hk::jit::Entry, 0x180> entry{};     // where each step starts
};

// What the made code's calls into the delay memory work from, worked out once and again only when
// a map register or the enable register has changed (the interpreter works it out at every access)
struct swp30_device::meg_state::jit_side {
	// per 12 steps (resolve_address()'s key): an offset's address is (offset & mask) + base;
	// off: the bank is turned off (map_bank()'s bit of the enable register)
	struct bank { u32 mask, base; bool off, none; };
	std::array<bank, 0x180 / 12> banks{};
	std::array<u16, 8> map{};   // what they were worked out from
	u16 enable = 0;
	bool valid = false;
	u8 *ram = nullptr;          // the memory itself, where it is one block of the host's: else through the space
};

swp30_device::meg_state::~meg_state() { delete m_jit_side; }

void swp30_device::meg_state::jit_side_update()
{
	jit_side &j = *m_jit_side;
	if(j.valid && j.map == m_map && j.enable == m_swp->m_revram_enable)
		return;
	j.map = m_map;
	j.enable = m_swp->m_revram_enable;
	j.valid = true;
	for(int g = 0; g != 0x180 / 12; g++) {
		const u16 pc = g * 12;
		const u32 base = resolve_address(pc, 0), top = resolve_address(pc, -1);   // offsets of no bits and of all
		j.banks[g] = {top - base, base, bool(BIT(j.enable, map_bank(pc))), base == 0xffffffff};
	}
}

// (the self-test below makes thousands of programs: each its own, freed when compared)
static thread_local bool g_meg_jit_own = false;

// At a sample's start: the current program's code, once the program has stayed unchanged for a
// while (the firmware writes a program a word at a time while the MEG runs: the hundreds of
// programs in between are not worth making)
void swp30_device::meg_state::jit_arm()
{
	static const bool stats = [] {
		const char *e = getenv("HK_MEG_STATS");
		if(e && strcmp(e, "0") != 0)
			atexit([] {
				const u64 all = s_steps[0] + s_steps[1] + s_steps[2];
				fprintf(stderr, "xt446: MEG steps: %llu made into host code (%.1f %%), %llu compiled with HIRAKU, %llu interpreted\n",
				        (unsigned long long)s_steps[0], all ? 100.0 * (double)s_steps[0] / (double)all : 0.0,
				        (unsigned long long)s_steps[1], (unsigned long long)s_steps[2]);
			});
		return true;
	}();
	(void)stats;
#ifdef HK_MEG_STENCILS
	static const bool on = [] {
		const char *e = getenv("HK_MEG_JIT");
		return hk::jit::wanted() && !(e && strcmp(e, "0") == 0);
	}();
	if(m_jit || !on || ++m_jit_samples < 16)
		return;
	static constexpr int phase_of[3][2] = {{0, 3}, {4, 1}, {2, 5}};
	m_jit = jit_make(phase_of[m_delay_3][m_delay_2]);
	if(!m_jit)
		m_jit_samples = INT_MIN / 2;   // no code for this program: not asked again
#endif
}

// n steps from m_pc with the made code, if it is there, the phase is its own, and the run goes
// to the program's end
HK_JIT_ENTERS bool swp30_device::meg_state::run_jit(int n)
{
	if(!m_jit || n != 0x180 - m_pc)
		return false;
	static constexpr int phase_of[3][2] = {{0, 3}, {4, 1}, {2, 5}};
	int k = phase_of[m_delay_3][m_delay_2];
	if(k != (m_jit->k0 + m_pc) % 6)
		return false;
	m_icount -= n;
	jit_side_update();   // (the registers behind it are the H8's to write, and it is not running now)
	m_p = m_jit->entry[m_pc](reinterpret_cast<char *>(this), m_p, 0, 0);
	m_pc = 0x180;
	k = (k + n) % 6;
	m_delay_3 = k % 3;
	m_delay_2 = k % 2;
	return true;
}

// The step pc's write to the delay memory and its read (delay slot d2): step_decoded()'s last
// part, with the bank from jit_side
void swp30_device::meg_state::jit_mem_w(int pc)
{
	const jit_side &j = *m_jit_side;
	const jit_side::bank &b = j.banks[pc / 12];
	if(b.off || b.none)
		return;
	const u32 address = ((u32(m_offset[pc/3]) + u32(m_dec[pc].use_index ? m_ram_index : 0) - m_sample_counter) & b.mask) + b.base;
	const u16 v = revram_encode(m_ram_write);
	if(j.ram)
		memcpy(j.ram + 2 * (address & 0x3ffff), &v, 2);
	else
		m_swp->m_reverb_cache.write_word(address, v);
}

void swp30_device::meg_state::jit_mem_r(int pc, int d2)
{
	const jit_side &j = *m_jit_side;
	const jit_side::bank &b = j.banks[pc / 12];
	const decoded &d = m_dec[pc];
	const u32 plus = d.memop == 3 ? 1 : 0;
	if(!d.absolute && b.off) {
		m_memr_value[d2] = 0;
		m_memr_active[d2] = true;
		return;
	}
	const u32 offset = u32(m_offset[pc/3]) + u32(d.use_index ? m_ram_index : 0) + plus;
	u32 address;
	if(d.absolute)
		address = offset & 0x3ffff;
	else {
		if(b.none)
			return;
		address = ((offset - m_sample_counter) & b.mask) + b.base;
	}
	u16 val;
	if(j.ram)
		memcpy(&val, j.ram + 2 * (address & 0x3ffff), 2);
	else
		val = m_swp->m_reverb_cache.read_word(address);
	m_memr_value[d2] = revram_decode(val);
	m_memr_active[d2] = true;
}

// The current (decoded) program's code for the phase k0 at its step 0: from the ones made
// before, or made now. Null where there is none to have.
const swp30_device::meg_state::jit_prog *swp30_device::meg_state::jit_make(int k0)
{
#ifdef HK_MEG_STENCILS
	using namespace hk::jit::meg;
	// what the code calls, by pointers kept in the state
	struct calls {
		static u32 HK_JIT_ABI rand(meg_state *s) { return s->m_swp->machine().rand(); }
		static u32 HK_JIT_ABI lfo(meg_state *s, s32 lfo) { return s->get_lfo(lfo); }
		static void HK_JIT_ABI mem_w(meg_state *s, s32 pc, s32) { s->jit_mem_w(pc); }
		static void HK_JIT_ABI mem_r(meg_state *s, s32 pc, s32 d2) { s->jit_mem_r(pc, d2); }
	};
	enum { FN_RAND, FN_LFO, FN_MEM_W, FN_MEM_R };
	m_jit_fn[FN_RAND] = reinterpret_cast<void *>(&calls::rand);
	m_jit_fn[FN_LFO] = reinterpret_cast<void *>(&calls::lfo);
	m_jit_fn[FN_MEM_W] = reinterpret_cast<void *>(&calls::mem_w);
	m_jit_fn[FN_MEM_R] = reinterpret_cast<void *>(&calls::mem_r);
	if(!m_jit_side) {
		m_jit_side = new jit_side;
		// the delay memory as one block of the host's: every page of its space plain memory, one
		// after another (a space laid out otherwise is read and written through, as the interpreter does)
		address_space &sp = *m_swp->m_reverb;
		const u64 bytes = u64(0x40000) * 2;
		u8 *base = sp.pages()[0].direct_w;
		bool ok = base && sp.addr_shift() == -1 && sp.byte_mask() == bytes - 1 && std::endian::native == std::endian::little;
		for(u64 a = 0; ok && a < bytes; a += u64(1) << sp.page_bits()) {
			const address_space::page_t &pg = sp.pages()[a >> sp.page_bits()];
			ok = pg.direct_r == base + a && pg.direct_w == base + a;
		}
		m_jit_side->ram = ok ? base : nullptr;
		if(getenv("HK_MEG_STATS"))
			fprintf(stderr, "xt446: the MEG's delay memory %s\n", ok ? "read and written in place" : "read and written through its address space");
	}

	// programs made, by their words' hash and phase (and the words themselves compared: two
	// programs of one hash are not each other's): the code names the state only by offsets, so
	// one is good for every card. Never freed (a card's thread may be in one as the process ends).
	static std::mutex mu;
	static auto &made = *new std::map<std::pair<u64, int>, jit_prog *>;
	std::unique_lock<std::mutex> lock(mu);
	if(g_meg_jit_own)
		lock.unlock();
	else {
		if(auto it = made.find({m_prog_hash, k0}); it != made.end())
			return it->second && it->second->program == m_program ? it->second : nullptr;
		if(made.size() >= 256)   // (the firmware has a handful)
			return nullptr;
	}

	const auto off = [this](const void *field) { return (int64_t)((const char *)field - reinterpret_cast<const char *>(this)); };
	hk::jit::Code c(SET);
	size_t at[0x180];
	for(int pc = 0; pc != 0x180; pc++) {
		at[pc] = c.size();
		const decoded &d = m_dec[pc];
		const int k = (k0 + pc) % 6, D3 = k % 3, D2 = k % 2;
		const bool last3 = pc >= 0x180 - 3, last2 = pc >= 0x180 - 2;

		// the delay slots of the steps 3 and 2 before
		if(pc >= 3) {
			const decoded &d3 = m_dec[pc - 3];
			if(d3.dm) c.put(ST_copy32, off(&m_m[d3.dm]), off(&m_mw_value[D3]));
			if(d3.dr) c.put(ST_copy32, off(&m_r[d3.dr]), off(&m_rw_value[D3]));
			if(d3.idx) c.put(ST_copy32, off(&m_ram_index), off(&m_index_value[D3]));
		} else {
			c.put(ST_commit_reg, off(&m_mw_reg[D3]), off(&m_m[0]), off(&m_mw_value[D3]));
			c.put(ST_commit_reg, off(&m_rw_reg[D3]), off(&m_r[0]), off(&m_rw_value[D3]));
			c.put(ST_commit_flag, off(&m_index_active[D3]), off(&m_ram_index), off(&m_index_value[D3]));
		}
		if(pc >= 2) {
			const decoded &d2 = m_dec[pc - 2];
			if(d2.memw) c.put(ST_copy32, off(&m_ram_write), off(&m_memw_value[D2]));
			// (a read's slot is set by jit_mem only where the address was one: still tested)
			if(d2.memop >= 2) c.put(ST_commit_flag_clear, off(&m_memr_active[D2]), off(&m_ram_read), off(&m_memr_value[D2]));
		} else {
			c.put(ST_commit_flag_clear, off(&m_memw_active[D2]), off(&m_ram_write), off(&m_memw_value[D2]));
			c.put(ST_commit_flag_clear, off(&m_memr_active[D2]), off(&m_ram_read), off(&m_memr_value[D2]));
		}

		// the multiplier and the ALU
		if(d.alu) {
			const bool a_zero = d.amode == 3;
			if(a_zero && d.op == 3)
				c.put(ST_m_zero);                       // m & 0
			else {
				switch(d.mmode) {
				case 0: c.put(ST_m_zero); break;
				case 1: c.put(d.m1_expand ? ST_m_m1_exp : ST_m_m1, off(d.m1p)); break;
				case 2: c.put(d.m1_expand ? ST_m_mul_exp : ST_m_mul, off(d.m1p), off(d.m2p)); break;
				default: c.put(ST_m_m2, off(d.m2p)); break;
				}
				if(d.amode == 0) {
					static constexpr int ops[4] = {ST_op_add_p, ST_op_sub_p, ST_op_addabs_p, ST_op_and_p};
					c.put(ops[d.op]);
				} else if(!a_zero) {                    // (m + 0, m - 0, m + |0|: m)
					if(d.amode == 1 && d.sr) c.put(ST_a_reg, off(&m_r[d.sr]));
					else if(d.amode == 2 && d.sm) c.put(ST_a_reg, off(&m_m[d.sm]));
					else c.put(ST_a_psar);
					static constexpr int ops[4] = {ST_op_add, ST_op_sub, ST_op_addabs, ST_op_and};
					c.put(ops[d.op]);
				}
			}
			if(d.shift == 1) c.put(ST_shl1);
			else if(d.shift == 2) c.put(ST_shl2);
			else if(d.shift == 4) c.put(ST_shl4);
			static constexpr int sats[4] = {ST_sat_sext, ST_sat_clamp, ST_sat_clamp0, ST_sat_abs};
			c.put(sats[d.sat]);
		}

		// a value for an M register
		if(last3) c.put(ST_set8, off(&m_mw_reg[D3]), d.dm);
		if(d.dm) {
			const int64_t to = off(&m_mw_value[D3]);
			switch(d.dsrc) {
			case 0: case 1: case 2: case 3: c.put(ST_st_lfo, to, off(&m_jit_fn[FN_LFO]), pc >> 4); break;
			case 4: c.put(ST_copy32, to, off(&m_ram_read)); break;
			case 5: c.put(ST_st_rand24, to, off(&m_jit_fn[FN_RAND])); break;
			case 6:
				if(d.add_rand) c.put(ST_st_pack_rand, to, off(&m_jit_fn[FN_RAND]));
				else c.put(ST_st_pack, to);
				break;
			default: c.put(ST_copy32, to, off(&m_m[d.sm])); break;
			}
		}

		// ... for an R register
		if(last3) c.put(ST_set8, off(&m_rw_reg[D3]), d.dr);
		if(d.dr) {
			const int64_t to = off(&m_rw_value[D3]);
			if(d.r_from_r) c.put(ST_copy32, to, off(&m_r[d.sr]));
			else if(d.add_rand) c.put(ST_st_pack_rand, to, off(&m_jit_fn[FN_RAND]));
			else c.put(ST_st_pack, to);
		}

		// ... for the delay memory, the index, the T registers
		if(last2) c.put(ST_set8, off(&m_memw_active[D2]), d.memw ? 1 : 0);
		if(d.memw) c.put(ST_st_memw, off(&m_memw_value[D2]));
		if(last3) c.put(ST_set8, off(&m_index_active[D3]), d.idx ? 1 : 0);
		if(d.idx) c.put(ST_st_idx, off(&m_index_value[D3]));
		if(d.twr) c.put(ST_copy16, off(&m_t[d.t]), d.t_from_value ? off(&m_t_value[D2]) : off(&m_const[pc]));
		if(last2 || (m_dec[pc + 2].twr && m_dec[pc + 2].t_from_value))
			c.put(d.idx ? ST_st_tval_idx : ST_st_tval, off(&m_t_value[D2]));

		// the delay memory
		if(d.memop) c.put(ST_mem_op, off(&m_jit_fn[d.memop == 1 ? FN_MEM_W : FN_MEM_R]), pc, D2);
	}
	c.put(ST_done);

	void *mem = c.ok() ? hk::jit::executable(c.bytes()) : nullptr;
	if(!mem) {
		if(!g_meg_jit_own)
			made[{m_prog_hash, k0}] = nullptr;   // (not tried again)
		return nullptr;
	}
	jit_prog *p = new jit_prog;
	p->mem = mem;
	p->size = c.size();
	p->k0 = k0;
	p->program = m_program;
	for(int pc = 0; pc != 0x180; pc++)
		p->entry[pc] = reinterpret_cast<hk::jit::Entry>(static_cast<char *>(mem) + at[pc]);
	if(!g_meg_jit_own)
		made[{m_prog_hash, k0}] = p;
	return p;
#else
	(void)k0;
	return nullptr;
#endif
}

// ---- the made code against the interpreter (tests/jit/test_meg_jit.cpp) -------------------------
// Two cards, each in a machine of its own, given the same programs and states: programs of random
// words and of few set fields, states of random numbers. Each round runs three samples, here by
// step_decoded() alone, there by the made code entered at a random step of each sample, and
// everything a step can change must come out the same: the registers, the delay slots and their
// flags, the delay memory, the LFOs, the machines' random numbers.
std::string swp30_device::hk_meg_selftest(swp30_device &other, u32 seed, int rounds) { return m_meg->jit_selftest(*other.m_meg, seed, rounds); }

std::string swp30_device::meg_state::jit_selftest(meg_state &made, u32 seed, int rounds)
{
#ifdef HK_MEG_STENCILS
	u64 rs = seed * 0x9e3779b97f4a7c15ULL + 1;
	const auto rnd = [&rs]() -> u64 {   // xorshift64*
		rs ^= rs >> 12, rs ^= rs << 25, rs ^= rs >> 27;
		return rs * 0x2545f4914f6cdd1dULL;
	};
	const u32 words = 0x40000;
	for(u32 a = 0; a != words; a++) {
		const u16 w = u16(rnd());
		m_swp->m_reverb_cache.write_word(a, w);
		made.m_swp->m_reverb_cache.write_word(a, w);
	}

	for(int round = 0; round != rounds; round++) {
		// a program: every other round of random words, else of words with few fields set
		for(int pc = 0; pc != 0x180; pc++) {
			u64 w = rnd();
			if(round & 1) {
				w &= rnd() & rnd();
				if(rnd() % 4 == 0)
					w = 0;
			}
			m_program[pc] = w;
		}
		for(auto &v : m_const) v = s16(rnd());
		for(auto &v : m_offset) v = u16(rnd());
		for(auto &v : m_lfo) v = u16(rnd());
		for(auto &v : m_lfo_increment) v = u32(rnd()) & 0x3fffff;
		for(auto &v : m_lfo_counter) v = u32(rnd()) & 0x3fffff;
		for(auto &v : m_map) v = u16(rnd());
		if(round % 3 == 0)   // ... or a map in rising order, as the firmware sets one
			for(int i = 0; i != 8; i++)
				m_map[i] = u16((i * 4) << 11 | (rnd() & 0x7ff));
		for(auto &v : m_m) v = s32(rnd());
		for(auto &v : m_r) v = s32(rnd());
		for(auto &v : m_t) v = s16(rnd());
		m_p = util::sext(rnd(), 42);
		for(auto &v : m_mw_value) v = s32(rnd());
		for(auto &v : m_mw_reg) v = u8(rnd() % 0x40);
		for(auto &v : m_rw_value) v = s32(rnd());
		for(auto &v : m_rw_reg) v = u8(rnd() % 0x80);
		for(auto &v : m_index_value) v = s32(rnd());
		for(auto &v : m_index_active) v = rnd() & 1;
		for(auto &v : m_memw_value) v = s32(rnd());
		for(auto &v : m_memr_value) v = s32(rnd());
		for(auto &v : m_t_value) v = s16(rnd());
		for(auto &v : m_memw_active) v = rnd() & 1;
		for(auto &v : m_memr_active) v = rnd() & 1;
		const int k0 = int(rnd() % 6);
		m_delay_3 = k0 % 3;
		m_delay_2 = k0 % 2;
		m_ram_read = u32(rnd());
		m_ram_write = u32(rnd());
		m_ram_index = s32(rnd());
		m_sample_counter = u32(rnd());
		m_swp->m_revram_enable = round % 4 == 1 || round % 4 == 2 ? u16(rnd()) : 0;   // banks turned off: half the rounds
		m_pc = 0;
		m_icount = 1 << 20;

		// the other card the same
#define HK_EACH(F) F(m_program) F(m_const) F(m_offset) F(m_lfo) F(m_lfo_increment) F(m_lfo_counter) F(m_map) \
		F(m_m) F(m_r) F(m_t) F(m_p) F(m_mw_value) F(m_mw_reg) F(m_rw_value) F(m_rw_reg) F(m_index_value) F(m_index_active) \
		F(m_memw_value) F(m_memr_value) F(m_t_value) F(m_memw_active) F(m_memr_active) F(m_delay_3) F(m_delay_2) \
		F(m_ram_read) F(m_ram_write) F(m_ram_index) F(m_sample_counter) F(m_pc) F(m_icount)
#define HK_COPY(field) made.field = field;
		HK_EACH(HK_COPY)
#undef HK_COPY
		made.m_swp->m_revram_enable = m_swp->m_revram_enable;
		decode();
		made.decode();

		// here by the interpreter
		for(int sample = 0; sample != 3; sample++) {
			m_pc = 0;
			run_decoded(0x180);
			lfo_step();
			m_sample_counter++;
		}
		// there by the made code, the first sample whole, the others from some step on
		g_meg_jit_own = true;
		const jit_prog *code = made.jit_make(k0);
		g_meg_jit_own = false;
		made.m_jit = code;
		if(!code)
			return "round " + std::to_string(round) + ": no code made";
		for(int sample = 0; sample != 3; sample++) {
			made.m_pc = 0;
			const int enter = sample == 0 ? 0 : int(rnd() % 0x180);
			if(enter)
				made.run_decoded(enter);
			if(!made.run_jit(0x180 - enter))
				return "round " + std::to_string(round) + ": the made code refused a run from step " + std::to_string(enter);
			made.lfo_step();
			made.m_sample_counter++;
		}
		made.m_jit = nullptr;
		hk::jit::release(code->mem, code->size);
		delete code;

		const char *what = nullptr;
#define HK_SAME(field) if(!what && !(made.field == field)) what = #field;
		HK_EACH(HK_SAME)
#undef HK_SAME
#undef HK_EACH
		if(!what && m_swp->machine().rand() != made.m_swp->machine().rand())
			what = "the machine's random number";
		// (the delay memory every 16th round and at the end: what differs there stays different)
		if(round % 16 == 15 || round == rounds - 1)
			for(u32 a = 0; !what && a != words; a++)
				if(m_swp->m_reverb_cache.read_word(a) != made.m_swp->m_reverb_cache.read_word(a))
					what = "the delay memory (since the last look, up to 16 rounds back)";
		if(what)
			return "round " + std::to_string(round) + " (seed " + std::to_string(seed) + ", phase " + std::to_string(k0) + "): " + what + " differs";
	}
	return "";
#else
	(void)made, (void)seed, (void)rounds;
	return "skip: no MEG stencils in this build";
#endif
}
