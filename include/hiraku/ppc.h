/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * HIRAKU: PowerPC 403GCX guest state and instruction semantics.
 *
 * This header is the ONLY definition of what an instruction does. The
 * interpreter calls these functions with operands decoded at run time; the
 * recompiler emits calls to them with constant operands, which the C compiler
 * folds. Valid C99 and C++20.
 *
 * Conventions:
 *   - guest memory is big-endian; every access goes through hk_r* / hk_w*;
 *   - CR is one 32-bit word in architectural bit order (bit 0 = MSB = CR0.LT);
 *   - XER keeps SO/OV/CA in bits 31/30/29 and the string byte count in 6:0.
 */
#ifndef HIRAKU_PPC_H
#define HIRAKU_PPC_H

#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------------ */
/* Guest state                                                               */
/* ------------------------------------------------------------------------ */

typedef struct hk_ctx {
    uint32_t r[32];
    uint32_t cr;
    uint32_t xer;
    uint32_t lr;
    uint32_t ctr;
    uint32_t pc;        /* set at returns/traps/diagnostics, not every insn */
    uint32_t msr;
    uint32_t srr0, srr1, srr2, srr3;
    uint32_t sprg[4];
    uint32_t reserve;   /* lwarx/stwcx. reservation: 0 = none, else address|1 */
    void *fiber;        /* owned by the runtime */
} hk_ctx;

#define HK_XER_SO 0x80000000u
#define HK_XER_OV 0x40000000u
#define HK_XER_CA 0x20000000u

/* ------------------------------------------------------------------------ */
/* Memory                                                                    */
/* ------------------------------------------------------------------------ */

#define HK_RAM_SIZE 0x02000000u           /* 32 MiB main RAM at physical 0 */
#define HK_PHYS(ea) ((ea) & 0x7fffffffu)  /* 4xx: bit 31 mirrors (0x8xxxxxxx = 0x0xxxxxxx) */

extern uint8_t *hk_ram;                   /* HK_RAM_SIZE bytes, big-endian guest order */

/* Slow paths (runtime): everything that is not RAM. size = 1, 2 or 4. */
uint32_t hk_mmio_read(hk_ctx *c, uint32_t phys, int size);
void hk_mmio_write(hk_ctx *c, uint32_t phys, uint32_t value, int size);

static inline uint32_t hk_bswap32(uint32_t v) { return __builtin_bswap32(v); }
static inline uint16_t hk_bswap16(uint16_t v) { return __builtin_bswap16(v); }

#ifdef HK_MEM_HOOKS
/* A file with accessors of its own, defined after this header (runtime/core/exec_rec.cpp: the
 * executor's, which note what a run reads and writes). */
static inline uint32_t hk_r8(hk_ctx *c, uint32_t ea);
static inline uint32_t hk_r16(hk_ctx *c, uint32_t ea);
static inline uint32_t hk_r32(hk_ctx *c, uint32_t ea);
static inline void hk_w8(hk_ctx *c, uint32_t ea, uint32_t v);
static inline void hk_w16(hk_ctx *c, uint32_t ea, uint32_t v);
static inline void hk_w32(hk_ctx *c, uint32_t ea, uint32_t v);
#else
static inline uint32_t hk_r8(hk_ctx *c, uint32_t ea) {
    uint32_t p = HK_PHYS(ea);
    if (__builtin_expect(p < HK_RAM_SIZE, 1)) return hk_ram[p];
    return hk_mmio_read(c, p, 1) & 0xff;
}
static inline uint32_t hk_r16(hk_ctx *c, uint32_t ea) {
    uint32_t p = HK_PHYS(ea);
    if (__builtin_expect(p <= HK_RAM_SIZE - 2, 1)) { uint16_t v; memcpy(&v, hk_ram + p, 2); return hk_bswap16(v); }
    return hk_mmio_read(c, p, 2) & 0xffff;
}
static inline uint32_t hk_r32(hk_ctx *c, uint32_t ea) {
    uint32_t p = HK_PHYS(ea);
    if (__builtin_expect(p <= HK_RAM_SIZE - 4, 1)) { uint32_t v; memcpy(&v, hk_ram + p, 4); return hk_bswap32(v); }
    return hk_mmio_read(c, p, 4);
}
static inline void hk_w8(hk_ctx *c, uint32_t ea, uint32_t v) {
    uint32_t p = HK_PHYS(ea);
    if (__builtin_expect(p < HK_RAM_SIZE, 1)) { hk_ram[p] = (uint8_t)v; return; }
    hk_mmio_write(c, p, v & 0xff, 1);
}
static inline void hk_w16(hk_ctx *c, uint32_t ea, uint32_t v) {
    uint32_t p = HK_PHYS(ea);
    if (__builtin_expect(p <= HK_RAM_SIZE - 2, 1)) { uint16_t s = hk_bswap16((uint16_t)v); memcpy(hk_ram + p, &s, 2); return; }
    hk_mmio_write(c, p, v & 0xffff, 2);
}
static inline void hk_w32(hk_ctx *c, uint32_t ea, uint32_t v) {
    uint32_t p = HK_PHYS(ea);
    if (__builtin_expect(p <= HK_RAM_SIZE - 4, 1)) { uint32_t s = hk_bswap32(v); memcpy(hk_ram + p, &s, 4); return; }
    hk_mmio_write(c, p, v, 4);
}
#endif

/* ------------------------------------------------------------------------ */
/* Runtime services the semantics call out to                                */
/* ------------------------------------------------------------------------ */

uint32_t hk_mfspr(hk_ctx *c, uint32_t spr);          /* non-LR/CTR/XER SPRs */
void hk_mtspr(hk_ctx *c, uint32_t spr, uint32_t v);
uint32_t hk_mfdcr(hk_ctx *c, uint32_t dcr);
void hk_mtdcr(hk_ctx *c, uint32_t dcr, uint32_t v);
void hk_mtmsr(hk_ctx *c, uint32_t v);                 /* may make an interrupt deliverable */
void hk_trap(hk_ctx *c, uint32_t pc);                 /* tw/twi condition met */
void hk_unimplemented(hk_ctx *c, uint32_t pc, uint32_t insn);

/* ------------------------------------------------------------------------ */
/* Helpers                                                                   */
/* ------------------------------------------------------------------------ */

#define HK_RA0(c, ra) ((ra) ? (c)->r[ra] : 0u)   /* (rA|0) */

static inline uint32_t hk_rotl(uint32_t v, unsigned n) { n &= 31; return n ? (v << n) | (v >> (32 - n)) : v; }

static inline uint32_t hk_mask(unsigned mb, unsigned me) {   /* MASK(mb, me), wraps when mb > me */
    uint32_t a = 0xffffffffu >> mb, b = me == 31 ? 0xffffffffu : ~(0xffffffffu >> (me + 1));
    return mb <= me ? (a & b) : (a | b);
}

static inline uint32_t hk_crbit(const hk_ctx *c, unsigned bi) { return (c->cr >> (31 - bi)) & 1; }
static inline void hk_setcrbit(hk_ctx *c, unsigned bi, uint32_t v) {
    uint32_t m = 1u << (31 - bi);
    c->cr = (c->cr & ~m) | (v ? m : 0);
}
static inline void hk_setcrf(hk_ctx *c, unsigned crf, uint32_t nib) {
    unsigned sh = 28 - 4 * crf;
    c->cr = (c->cr & ~(0xfu << sh)) | ((nib & 0xf) << sh);
}
static inline void hk_cr0(hk_ctx *c, uint32_t r) {
    uint32_t nib = (int32_t)r < 0 ? 8 : (int32_t)r > 0 ? 4 : 2;
    hk_setcrf(c, 0, nib | (c->xer >> 31));
}
static inline void hk_setca(hk_ctx *c, uint32_t ca) { c->xer = ca ? (c->xer | HK_XER_CA) : (c->xer & ~HK_XER_CA); }
static inline uint32_t hk_ca(const hk_ctx *c) { return (c->xer >> 29) & 1; }
static inline void hk_setov(hk_ctx *c, uint32_t ov) {
    c->xer = ov ? (c->xer | HK_XER_OV | HK_XER_SO) : (c->xer & ~HK_XER_OV);
}

/* a + b + cin with carry and signed overflow; every add/sub form reduces to this */
static inline uint32_t hk_adde3(hk_ctx *c, uint32_t a, uint32_t b, uint32_t cin, int setca, int oe) {
    uint64_t s = (uint64_t)a + b + cin;
    uint32_t r = (uint32_t)s;
    if (setca) hk_setca(c, (uint32_t)(s >> 32));
    if (oe) hk_setov(c, ((a ^ r) & (b ^ r)) >> 31);
    return r;
}

/* ------------------------------------------------------------------------ */
/* Integer arithmetic                                                        */
/* ------------------------------------------------------------------------ */

#define HK_FIN(rd) do { if (rc) hk_cr0(c, c->r[rd]); } while (0)

static inline void op_add(hk_ctx *c, int rd, int ra, int rb, int oe, int rc)   { c->r[rd] = hk_adde3(c, c->r[ra], c->r[rb], 0, 0, oe); HK_FIN(rd); }
static inline void op_addc(hk_ctx *c, int rd, int ra, int rb, int oe, int rc)  { c->r[rd] = hk_adde3(c, c->r[ra], c->r[rb], 0, 1, oe); HK_FIN(rd); }
static inline void op_adde(hk_ctx *c, int rd, int ra, int rb, int oe, int rc)  { c->r[rd] = hk_adde3(c, c->r[ra], c->r[rb], hk_ca(c), 1, oe); HK_FIN(rd); }
static inline void op_addme(hk_ctx *c, int rd, int ra, int oe, int rc)         { c->r[rd] = hk_adde3(c, c->r[ra], 0xffffffffu, hk_ca(c), 1, oe); HK_FIN(rd); }
static inline void op_addze(hk_ctx *c, int rd, int ra, int oe, int rc)         { c->r[rd] = hk_adde3(c, c->r[ra], 0, hk_ca(c), 1, oe); HK_FIN(rd); }
static inline void op_subf(hk_ctx *c, int rd, int ra, int rb, int oe, int rc)  { c->r[rd] = hk_adde3(c, ~c->r[ra], c->r[rb], 1, 0, oe); HK_FIN(rd); }
static inline void op_subfc(hk_ctx *c, int rd, int ra, int rb, int oe, int rc) { c->r[rd] = hk_adde3(c, ~c->r[ra], c->r[rb], 1, 1, oe); HK_FIN(rd); }
static inline void op_subfe(hk_ctx *c, int rd, int ra, int rb, int oe, int rc) { c->r[rd] = hk_adde3(c, ~c->r[ra], c->r[rb], hk_ca(c), 1, oe); HK_FIN(rd); }
static inline void op_subfme(hk_ctx *c, int rd, int ra, int oe, int rc)        { c->r[rd] = hk_adde3(c, ~c->r[ra], 0xffffffffu, hk_ca(c), 1, oe); HK_FIN(rd); }
static inline void op_subfze(hk_ctx *c, int rd, int ra, int oe, int rc)        { c->r[rd] = hk_adde3(c, ~c->r[ra], 0, hk_ca(c), 1, oe); HK_FIN(rd); }
static inline void op_neg(hk_ctx *c, int rd, int ra, int oe, int rc)           { c->r[rd] = hk_adde3(c, ~c->r[ra], 0, 1, 0, oe); HK_FIN(rd); }

static inline void op_addi(hk_ctx *c, int rd, int ra, int32_t simm)   { c->r[rd] = HK_RA0(c, ra) + (uint32_t)simm; }
static inline void op_addis(hk_ctx *c, int rd, int ra, int32_t simm)  { c->r[rd] = HK_RA0(c, ra) + ((uint32_t)simm << 16); }
static inline void op_addic(hk_ctx *c, int rd, int ra, int32_t simm, int rc) { c->r[rd] = hk_adde3(c, c->r[ra], (uint32_t)simm, 0, 1, 0); HK_FIN(rd); }
static inline void op_subfic(hk_ctx *c, int rd, int ra, int32_t simm) { c->r[rd] = hk_adde3(c, ~c->r[ra], (uint32_t)simm, 1, 1, 0); }

static inline void op_mulli(hk_ctx *c, int rd, int ra, int32_t simm) { c->r[rd] = (uint32_t)((int32_t)c->r[ra] * simm); }
static inline void op_mullw(hk_ctx *c, int rd, int ra, int rb, int oe, int rc) {
    int64_t p = (int64_t)(int32_t)c->r[ra] * (int32_t)c->r[rb];
    c->r[rd] = (uint32_t)p;
    if (oe) hk_setov(c, p != (int64_t)(int32_t)p);
    HK_FIN(rd);
}
static inline void op_mulhw(hk_ctx *c, int rd, int ra, int rb, int rc) {
    c->r[rd] = (uint32_t)(((int64_t)(int32_t)c->r[ra] * (int32_t)c->r[rb]) >> 32); HK_FIN(rd);
}
static inline void op_mulhwu(hk_ctx *c, int rd, int ra, int rb, int rc) {
    c->r[rd] = (uint32_t)(((uint64_t)c->r[ra] * c->r[rb]) >> 32); HK_FIN(rd);
}
/* Division by zero / INT_MIN / -1 are architecturally undefined. The values
 * below are a placeholder until checked against MAME's PPC core (TODO M2). */
static inline void op_divw(hk_ctx *c, int rd, int ra, int rb, int oe, int rc) {
    int32_t a = (int32_t)c->r[ra], b = (int32_t)c->r[rb];
    int bad = b == 0 || (a == INT32_MIN && b == -1);
    c->r[rd] = bad ? (a < 0 ? 0xffffffffu : 0) : (uint32_t)(a / b);
    if (oe) hk_setov(c, bad);
    HK_FIN(rd);
}
static inline void op_divwu(hk_ctx *c, int rd, int ra, int rb, int oe, int rc) {
    uint32_t a = c->r[ra], b = c->r[rb];
    c->r[rd] = b ? a / b : 0;
    if (oe) hk_setov(c, b == 0);
    HK_FIN(rd);
}

/* ------------------------------------------------------------------------ */
/* Logical, shifts, rotates                                                  */
/* ------------------------------------------------------------------------ */

#define HK_LOGIC(name, expr) \
    static inline void op_##name(hk_ctx *c, int ra, int rs, int rb, int rc) { \
        uint32_t s = c->r[rs], b = c->r[rb]; (void)b; c->r[ra] = (expr); HK_FIN(ra); }
HK_LOGIC(and_, s & b)
HK_LOGIC(andc, s & ~b)
HK_LOGIC(or_, s | b)
HK_LOGIC(orc, s | ~b)
HK_LOGIC(xor_, s ^ b)
HK_LOGIC(nand, ~(s & b))
HK_LOGIC(nor, ~(s | b))
HK_LOGIC(eqv, ~(s ^ b))
#undef HK_LOGIC

static inline void op_ori(hk_ctx *c, int ra, int rs, uint32_t ui)   { c->r[ra] = c->r[rs] | ui; }
static inline void op_oris(hk_ctx *c, int ra, int rs, uint32_t ui)  { c->r[ra] = c->r[rs] | (ui << 16); }
static inline void op_xori(hk_ctx *c, int ra, int rs, uint32_t ui)  { c->r[ra] = c->r[rs] ^ ui; }
static inline void op_xoris(hk_ctx *c, int ra, int rs, uint32_t ui) { c->r[ra] = c->r[rs] ^ (ui << 16); }
static inline void op_andi_(hk_ctx *c, int ra, int rs, uint32_t ui)  { c->r[ra] = c->r[rs] & ui; hk_cr0(c, c->r[ra]); }
static inline void op_andis_(hk_ctx *c, int ra, int rs, uint32_t ui) { c->r[ra] = c->r[rs] & (ui << 16); hk_cr0(c, c->r[ra]); }

static inline void op_extsb(hk_ctx *c, int ra, int rs, int rc)  { c->r[ra] = (uint32_t)(int32_t)(int8_t)c->r[rs]; HK_FIN(ra); }
static inline void op_extsh(hk_ctx *c, int ra, int rs, int rc)  { c->r[ra] = (uint32_t)(int32_t)(int16_t)c->r[rs]; HK_FIN(ra); }
static inline void op_cntlzw(hk_ctx *c, int ra, int rs, int rc) { uint32_t s = c->r[rs]; c->r[ra] = s ? (uint32_t)__builtin_clz(s) : 32; HK_FIN(ra); }

static inline void op_slw(hk_ctx *c, int ra, int rs, int rb, int rc) {
    uint32_t n = c->r[rb] & 63; c->r[ra] = n > 31 ? 0 : c->r[rs] << n; HK_FIN(ra);
}
static inline void op_srw(hk_ctx *c, int ra, int rs, int rb, int rc) {
    uint32_t n = c->r[rb] & 63; c->r[ra] = n > 31 ? 0 : c->r[rs] >> n; HK_FIN(ra);
}
static inline void op_sraw(hk_ctx *c, int ra, int rs, int rb, int rc) {
    uint32_t n = c->r[rb] & 63; int32_t s = (int32_t)c->r[rs];
    if (n > 31) { c->r[ra] = (uint32_t)(s >> 31); hk_setca(c, s < 0); }
    else { c->r[ra] = (uint32_t)(s >> n); hk_setca(c, s < 0 && n && ((uint32_t)s << (32 - n)) != 0); }
    HK_FIN(ra);
}
static inline void op_srawi(hk_ctx *c, int ra, int rs, unsigned sh, int rc) {
    int32_t s = (int32_t)c->r[rs];
    c->r[ra] = (uint32_t)(s >> sh);
    hk_setca(c, s < 0 && sh && ((uint32_t)s << (32 - sh)) != 0);
    HK_FIN(ra);
}
static inline void op_rlwinm(hk_ctx *c, int ra, int rs, unsigned sh, unsigned mb, unsigned me, int rc) {
    c->r[ra] = hk_rotl(c->r[rs], sh) & hk_mask(mb, me); HK_FIN(ra);
}
static inline void op_rlwnm(hk_ctx *c, int ra, int rs, int rb, unsigned mb, unsigned me, int rc) {
    c->r[ra] = hk_rotl(c->r[rs], c->r[rb] & 31) & hk_mask(mb, me); HK_FIN(ra);
}
static inline void op_rlwimi(hk_ctx *c, int ra, int rs, unsigned sh, unsigned mb, unsigned me, int rc) {
    uint32_t m = hk_mask(mb, me);
    c->r[ra] = (hk_rotl(c->r[rs], sh) & m) | (c->r[ra] & ~m); HK_FIN(ra);
}

/* ------------------------------------------------------------------------ */
/* Compares and condition register                                           */
/* ------------------------------------------------------------------------ */

static inline uint32_t hk_cmps(const hk_ctx *c, int32_t a, int32_t b) { return (a < b ? 8u : a > b ? 4u : 2u) | (c->xer >> 31); }
static inline uint32_t hk_cmpu(const hk_ctx *c, uint32_t a, uint32_t b) { return (a < b ? 8u : a > b ? 4u : 2u) | (c->xer >> 31); }
static inline void op_cmp(hk_ctx *c, int crf, int ra, int rb)          { hk_setcrf(c, crf, hk_cmps(c, (int32_t)c->r[ra], (int32_t)c->r[rb])); }
static inline void op_cmpl(hk_ctx *c, int crf, int ra, int rb)         { hk_setcrf(c, crf, hk_cmpu(c, c->r[ra], c->r[rb])); }
static inline void op_cmpi(hk_ctx *c, int crf, int ra, int32_t simm)   { hk_setcrf(c, crf, hk_cmps(c, (int32_t)c->r[ra], simm)); }
static inline void op_cmpli(hk_ctx *c, int crf, int ra, uint32_t uimm) { hk_setcrf(c, crf, hk_cmpu(c, c->r[ra], uimm)); }

#define HK_CRLOGIC(name, expr) \
    static inline void op_##name(hk_ctx *c, int bt, int ba, int bb) { \
        uint32_t a = hk_crbit(c, ba), b = hk_crbit(c, bb); hk_setcrbit(c, bt, (expr) & 1); }
HK_CRLOGIC(crand, a & b)
HK_CRLOGIC(crandc, a & ~b)
HK_CRLOGIC(creqv, ~(a ^ b))
HK_CRLOGIC(crnand, ~(a & b))
HK_CRLOGIC(crnor, ~(a | b))
HK_CRLOGIC(cror, a | b)
HK_CRLOGIC(crorc, a | ~b)
HK_CRLOGIC(crxor, a ^ b)
#undef HK_CRLOGIC

static inline void op_mcrf(hk_ctx *c, int crfd, int crfs) { hk_setcrf(c, crfd, (c->cr >> (28 - 4 * crfs)) & 0xf); }
static inline void op_mcrxr(hk_ctx *c, int crfd) { hk_setcrf(c, crfd, c->xer >> 28); c->xer &= 0x0fffffffu; }
static inline void op_mfcr(hk_ctx *c, int rd) { c->r[rd] = c->cr; }
static inline void op_mtcrf(hk_ctx *c, unsigned crm, int rs) {
    uint32_t m = 0;
    for (int i = 0; i < 8; i++) if (crm & (0x80u >> i)) m |= 0xf0000000u >> (4 * i);
    c->cr = (c->cr & ~m) | (c->r[rs] & m);
}

/* ------------------------------------------------------------------------ */
/* Special registers                                                         */
/* ------------------------------------------------------------------------ */

static inline void op_mfspr(hk_ctx *c, int rd, unsigned spr) {
    switch (spr) {
    case 1: c->r[rd] = c->xer; break;
    case 8: c->r[rd] = c->lr; break;
    case 9: c->r[rd] = c->ctr; break;
    default: c->r[rd] = hk_mfspr(c, spr); break;
    }
}
static inline void op_mtspr(hk_ctx *c, unsigned spr, int rs) {
    switch (spr) {
    case 1: c->xer = c->r[rs] & 0xe000007fu; break;
    case 8: c->lr = c->r[rs]; break;
    case 9: c->ctr = c->r[rs]; break;
    default: hk_mtspr(c, spr, c->r[rs]); break;
    }
}
static inline void op_mfmsr(hk_ctx *c, int rd) { c->r[rd] = c->msr; }
static inline void op_mtmsr(hk_ctx *c, int rs) { hk_mtmsr(c, c->r[rs]); }
static inline void op_wrtee(hk_ctx *c, int rs) { hk_mtmsr(c, (c->msr & ~0x8000u) | (c->r[rs] & 0x8000u)); }
static inline void op_wrteei(hk_ctx *c, int e) { hk_mtmsr(c, (c->msr & ~0x8000u) | (e ? 0x8000u : 0)); }
static inline void op_mfdcr(hk_ctx *c, int rd, unsigned dcr) { c->r[rd] = hk_mfdcr(c, dcr); }
static inline void op_mtdcr(hk_ctx *c, unsigned dcr, int rs) { hk_mtdcr(c, dcr, c->r[rs]); }

/* ------------------------------------------------------------------------ */
/* Loads and stores                                                          */
/* ------------------------------------------------------------------------ */

#define HK_LD(name, fn, conv) \
    static inline void op_##name(hk_ctx *c, int rd, int ra, int32_t d) { c->r[rd] = conv(fn(c, HK_RA0(c, ra) + (uint32_t)d)); } \
    static inline void op_##name##u(hk_ctx *c, int rd, int ra, int32_t d) { uint32_t ea = c->r[ra] + (uint32_t)d; c->r[rd] = conv(fn(c, ea)); c->r[ra] = ea; } \
    static inline void op_##name##x(hk_ctx *c, int rd, int ra, int rb) { c->r[rd] = conv(fn(c, HK_RA0(c, ra) + c->r[rb])); } \
    static inline void op_##name##ux(hk_ctx *c, int rd, int ra, int rb) { uint32_t ea = c->r[ra] + c->r[rb]; c->r[rd] = conv(fn(c, ea)); c->r[ra] = ea; }
#define HK_U(v) (v)
#define HK_SH(v) ((uint32_t)(int32_t)(int16_t)(v))
HK_LD(lbz, hk_r8, HK_U)
HK_LD(lhz, hk_r16, HK_U)
HK_LD(lha, hk_r16, HK_SH)
HK_LD(lwz, hk_r32, HK_U)
#undef HK_LD

#define HK_ST(name, fn) \
    static inline void op_##name(hk_ctx *c, int rs, int ra, int32_t d) { fn(c, HK_RA0(c, ra) + (uint32_t)d, c->r[rs]); } \
    static inline void op_##name##u(hk_ctx *c, int rs, int ra, int32_t d) { uint32_t ea = c->r[ra] + (uint32_t)d; fn(c, ea, c->r[rs]); c->r[ra] = ea; } \
    static inline void op_##name##x(hk_ctx *c, int rs, int ra, int rb) { fn(c, HK_RA0(c, ra) + c->r[rb], c->r[rs]); } \
    static inline void op_##name##ux(hk_ctx *c, int rs, int ra, int rb) { uint32_t ea = c->r[ra] + c->r[rb]; fn(c, ea, c->r[rs]); c->r[ra] = ea; }
HK_ST(stb, hk_w8)
HK_ST(sth, hk_w16)
HK_ST(stw, hk_w32)
#undef HK_ST

static inline void op_lhbrx(hk_ctx *c, int rd, int ra, int rb)  { c->r[rd] = hk_bswap16((uint16_t)hk_r16(c, HK_RA0(c, ra) + c->r[rb])); }
static inline void op_lwbrx(hk_ctx *c, int rd, int ra, int rb)  { c->r[rd] = hk_bswap32(hk_r32(c, HK_RA0(c, ra) + c->r[rb])); }
static inline void op_sthbrx(hk_ctx *c, int rs, int ra, int rb) { hk_w16(c, HK_RA0(c, ra) + c->r[rb], hk_bswap16((uint16_t)c->r[rs])); }
static inline void op_stwbrx(hk_ctx *c, int rs, int ra, int rb) { hk_w32(c, HK_RA0(c, ra) + c->r[rb], hk_bswap32(c->r[rs])); }

static inline void op_lmw(hk_ctx *c, int rd, int ra, int32_t d) {
    uint32_t ea = HK_RA0(c, ra) + (uint32_t)d;
    for (int r = rd; r < 32; r++, ea += 4) c->r[r] = hk_r32(c, ea);
}
static inline void op_stmw(hk_ctx *c, int rs, int ra, int32_t d) {
    uint32_t ea = HK_RA0(c, ra) + (uint32_t)d;
    for (int r = rs; r < 32; r++, ea += 4) hk_w32(c, ea, c->r[r]);
}

/* String loads/stores: n bytes, 4 per register, wrapping r31 -> r0; the last
 * register is left-justified and zero-padded on load. */
static inline void hk_lsw(hk_ctx *c, int rd, uint32_t ea, uint32_t n) {
    int r = rd - 1; unsigned i = 0;
    while (n > 0) {
        if (i == 0) { r = (r + 1) & 31; c->r[r] = 0; }
        c->r[r] |= hk_r8(c, ea) << (24 - i);
        i = (i + 8) & 31; ea++; n--;
    }
}
static inline void hk_stsw(hk_ctx *c, int rs, uint32_t ea, uint32_t n) {
    int r = rs - 1; unsigned i = 0;
    while (n > 0) {
        if (i == 0) r = (r + 1) & 31;
        hk_w8(c, ea, c->r[r] >> (24 - i));
        i = (i + 8) & 31; ea++; n--;
    }
}
static inline void op_lswi(hk_ctx *c, int rd, int ra, unsigned nb)   { hk_lsw(c, rd, HK_RA0(c, ra), nb ? nb : 32); }
static inline void op_stswi(hk_ctx *c, int rs, int ra, unsigned nb)  { hk_stsw(c, rs, HK_RA0(c, ra), nb ? nb : 32); }
static inline void op_lswx(hk_ctx *c, int rd, int ra, int rb)        { hk_lsw(c, rd, HK_RA0(c, ra) + c->r[rb], c->xer & 0x7f); }
static inline void op_stswx(hk_ctx *c, int rs, int ra, int rb)       { hk_stsw(c, rs, HK_RA0(c, ra) + c->r[rb], c->xer & 0x7f); }

static inline void op_lwarx(hk_ctx *c, int rd, int ra, int rb) {
    uint32_t ea = HK_RA0(c, ra) + c->r[rb];
    c->r[rd] = hk_r32(c, ea); c->reserve = ea | 1;
}
static inline void op_stwcx_(hk_ctx *c, int rs, int ra, int rb) {
    uint32_t ea = HK_RA0(c, ra) + c->r[rb];
    int ok = c->reserve != 0;
    if (ok) hk_w32(c, ea, c->r[rs]);
    c->reserve = 0;
    hk_setcrf(c, 0, (ok ? 2u : 0u) | (c->xer >> 31));
}

/* 403GCX cache line = 16 bytes. Only dcbz has an architectural effect here. */
static inline void op_dcbz(hk_ctx *c, int ra, int rb) {
    uint32_t ea = (HK_RA0(c, ra) + c->r[rb]) & ~15u;
    for (int i = 0; i < 16; i += 4) hk_w32(c, ea + i, 0);
}

/* ------------------------------------------------------------------------ */
/* Traps                                                                     */
/* ------------------------------------------------------------------------ */

static inline int hk_trap_cond(uint32_t to, uint32_t a, uint32_t b) {
    return ((to & 16) && (int32_t)a < (int32_t)b) || ((to & 8) && (int32_t)a > (int32_t)b) ||
           ((to & 4) && a == b) || ((to & 2) && a < b) || ((to & 1) && a > b);
}
static inline void op_tw(hk_ctx *c, unsigned to, int ra, int rb, uint32_t pc)     { if (hk_trap_cond(to, c->r[ra], c->r[rb])) hk_trap(c, pc); }
static inline void op_twi(hk_ctx *c, unsigned to, int ra, int32_t si, uint32_t pc) { if (hk_trap_cond(to, c->r[ra], (uint32_t)si)) hk_trap(c, pc); }

/* ------------------------------------------------------------------------ */
/* Branch conditions (BO/BI). Decrements CTR when BO says so.                */
/* ------------------------------------------------------------------------ */

static inline int hk_bc_ok(hk_ctx *c, unsigned bo, unsigned bi) {
    int ctr_ok = 1, cond_ok = 1;
    if (!(bo & 4)) { c->ctr--; ctr_ok = (c->ctr != 0) ^ ((bo >> 1) & 1); }
    if (!(bo & 16)) cond_ok = hk_crbit(c, bi) == ((bo >> 3) & 1);
    return ctr_ok && cond_ok;
}

#undef HK_FIN

#ifdef __cplusplus
}
#endif
#endif /* HIRAKU_PPC_H */
