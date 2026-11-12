// SPDX-License-Identifier: GPL-3.0-or-later
// The executor's dispatcher: a function's steps, run. Included twice, by exec.cpp (the one a game
// runs on) and by exec_rec.cpp (the same over accessors that note what is read and written), so
// that the two cannot differ in what a step does. The includer defines:
//   HK_RUN            the function's name
//   HK_RUN_FAST       (exec.cpp only) each step jumps to the next one's handler by the address
//                     kept in the step (Step::h, from g_plain / g_ticked), and the shapes of
//                     exec_steps.h have handlers of their own. Without it the handler is looked
//                     up by the step's kind, every shape goes to the instruction's one handler,
//                     and HK_RUN_STEP(st) is called before each step.
//   HK_RUN_OUTSIDE()  the run is about to do something a remembered run can't hold (a poll that
//                     runs the scheduler, a system call, code outside the executor): nothing in
//                     exec.cpp
//   HK_RUN_CALL_DYN(c, t), HK_RUN_JUMP(c, t)   hk_call_dyn, hk_jump

// A handler's entry. Fast: two labels, the first for a step with cycles folded into it (charged,
// then on into the second), so that a step without any doesn't touch the budget.
#ifdef HK_RUN_FAST
#define HK_E(l) T_##l: hk_budget -= st->tick; l:
#define HK_REG(k, l) (g_plain[k] = &&l, g_ticked[k] = &&T_##l)
#define HK_DISPATCH() goto *st->h
#else
#define HK_E(l) l:
#define HK_REG(k, l) (handler[k] = &&l)
#define HK_DISPATCH() do { HK_RUN_STEP(st); hk_budget -= st->tick; goto *handler[st->k]; } while (0)
#endif

// A recompiled function's body, run: returns where the C function would return. Threaded:
// each step jumps straight to the next one's handler, charging the cycles folded into it first.
// (c null: only fills the handlers' table, once, before any step is built.)
void HK_RUN(hk_ctx *c, const Code *k) {
#ifndef HK_RUN_FAST
    static void *handler[KINDS];
    static bool ready = false;
    if (__builtin_expect(!ready, 0)) {
        ready = true;
#else
    if (__builtin_expect(!c, 0)) {
#endif
        for (size_t n = 0; n < KINDS; n++) HK_REG(n, bad);
        HK_REG(K_tick, tick), HK_REG(K_poll, poll), HK_REG(K_go, go), HK_REG(K_bc, bc), HK_REG(K_tail, tail);
        HK_REG(K_jump, jump), HK_REG(K_call, call), HK_REG(K_call_up, call_up), HK_REG(K_call_dyn, call_dyn);
        HK_REG(K_set_lr, set_lr), HK_REG(K_bclrl, bclrl), HK_REG(K_ret, ret), HK_REG(K_ret_if, ret_if);
        HK_REG(K_bcctrl, bcctrl), HK_REG(K_table, table), HK_REG(K_jump_ctr, jump_ctr), HK_REG(K_sc, sc);
        HK_REG(K_rfi, rfi), HK_REG(K_rfci, rfci), HK_REG(K_unimpl, unimpl);
        HK_REG(K_gop, gop), HK_REG(K_br, br), HK_REG(K_brp, brp);
#ifdef HK_RUN_FAST
        HK_REG(K_bc_true, bc_true), HK_REG(K_bc_false, bc_false), HK_REG(K_bc_dnz, bc_dnz);
        HK_REG(K_br_true, br_true), HK_REG(K_br_false, br_false), HK_REG(K_br_dnz, br_dnz);
        HK_REG(K_brp_true, brp_true), HK_REG(K_brp_false, brp_false), HK_REG(K_brp_dnz, brp_dnz);
        HK_REG(K_cmpi0, cmpi0), HK_REG(K_cmpli0, cmpli0), HK_REG(K_cmp0, cmp0), HK_REG(K_cmpl0, cmpl0);
        HK_REG(K_mflr, mflr), HK_REG(K_mfctr, mfctr), HK_REG(K_mtlr, mtlr), HK_REG(K_mtctr, mtctr);
#define HK_L(n, ...) HK_REG(kind(Op::n, 0), L_##n), HK_REG(kind(Op::n, 1), P_##n), HK_REG(kind(Op::n, 2), Z_##n);
#else
        HK_REG(K_bc_true, bc), HK_REG(K_bc_false, bc), HK_REG(K_bc_dnz, bc);
        HK_REG(K_br_true, br), HK_REG(K_br_false, br), HK_REG(K_br_dnz, br);
        HK_REG(K_brp_true, brp), HK_REG(K_brp_false, brp), HK_REG(K_brp_dnz, brp);
        HK_REG(K_cmpi0, L_cmpi), HK_REG(K_cmpli0, L_cmpli), HK_REG(K_cmp0, L_cmp), HK_REG(K_cmpl0, L_cmpl);
        HK_REG(K_mflr, L_mfspr), HK_REG(K_mfctr, L_mfspr), HK_REG(K_mtlr, L_mtspr), HK_REG(K_mtctr, L_mtspr);
#define HK_L(n, ...) HK_REG(kind(Op::n, 0), L_##n), HK_REG(kind(Op::n, 1), L_##n), HK_REG(kind(Op::n, 2), L_##n);
#endif
        HK_OPLIST(HK_L, HK_L, HK_L, HK_L, HK_L, HK_L, HK_L)
#undef HK_L
#ifdef HK_RUN_FAST
        return;
#endif
    }
    if (__builtin_expect(hk_patched_any, 0) && patched(k->entry)) {   // changed by a mod: interpreted
        HK_RUN_OUTSIDE();
        hk_interp_function(c, k->entry);
        return;
    }
    const Step *s = k->steps.data(), *st;
#define HK_STEP() do { st = s++; HK_DISPATCH(); } while (0)
#define HK_AT(n) do { s = k->steps.data() + (n); HK_STEP(); } while (0)
    HK_STEP();

    // Each ordinary instruction's op_*, inline (the HK_OPLIST, as the emitter calls them), with
    // the instruction's fields from the step (HK_F_), or (fast) in the two plain shapes: rc and
    // oe 0 and rA not register 0 (HK_P_), or rA register 0 (HK_Z_).
#define HK_F_rt (st->rt)
#define HK_F_ra (st->ra)
#define HK_F_rb (st->rb)
#define HK_F_sh (st->sh)
#define HK_F_mb (st->x.f.mb)
#define HK_F_me (st->x.f.me)
#define HK_F_nb (st->x.f.nb)
#define HK_F_crm (st->x.f.crm)
#define HK_F_to (st->x.f.to)
#define HK_F_e (st->x.f.e)
#define HK_F_oe (st->x.f.oe)
#define HK_F_rc (st->x.f.rc)
#define HK_F_crfd (st->y.f.crfd)
#define HK_F_crfs (st->y.f.crfs)
#define HK_F_simm ((int32_t)st->y.f.imm)
#define HK_F_uimm ((uint32_t)(uint16_t)st->y.f.imm)
#define HK_F_spr (st->a)
#define HK_F_dcr (st->a)
#define HK_F_addr (st->a)
#define HK_P_rt HK_F_rt
#define HK_P_ra HK_F_ra
#define HK_P_rb HK_F_rb
#define HK_P_sh HK_F_sh
#define HK_P_mb HK_F_mb
#define HK_P_me HK_F_me
#define HK_P_nb HK_F_nb
#define HK_P_crm HK_F_crm
#define HK_P_to HK_F_to
#define HK_P_e HK_F_e
#define HK_P_oe 0
#define HK_P_rc 0
#define HK_P_crfd HK_F_crfd
#define HK_P_crfs HK_F_crfs
#define HK_P_simm HK_F_simm
#define HK_P_uimm HK_F_uimm
#define HK_P_spr HK_F_spr
#define HK_P_dcr HK_F_dcr
#define HK_P_addr HK_F_addr
#define HK_Z_rt HK_F_rt
#define HK_Z_ra 0
#define HK_Z_rb HK_F_rb
#define HK_Z_sh HK_F_sh
#define HK_Z_mb HK_F_mb
#define HK_Z_me HK_F_me
#define HK_Z_nb HK_F_nb
#define HK_Z_crm HK_F_crm
#define HK_Z_to HK_F_to
#define HK_Z_e HK_F_e
#define HK_Z_oe 0
#define HK_Z_rc 0
#define HK_Z_crfd HK_F_crfd
#define HK_Z_crfs HK_F_crfs
#define HK_Z_simm HK_F_simm
#define HK_Z_uimm HK_F_uimm
#define HK_Z_spr HK_F_spr
#define HK_Z_dcr HK_F_dcr
#define HK_Z_addr HK_F_addr
#ifdef HK_RUN_FAST
#define HK_SHAPES(n, f, p, z) \
    HK_E(L_##n) op_##n f; HK_STEP(); \
    HK_E(P_##n) { if (st->ra == 0) __builtin_unreachable(); op_##n p; } HK_STEP(); \
    HK_E(Z_##n) op_##n z; HK_STEP();
#else
#define HK_SHAPES(n, f, p, z) HK_E(L_##n) op_##n f; HK_STEP();
#endif
#define HK_H0(n) HK_SHAPES(n, (c), (c), (c))
#define HK_H1(n, a) HK_SHAPES(n, (c, HK_F_##a), (c, HK_P_##a), (c, HK_Z_##a))
#define HK_H2(n, a, b) HK_SHAPES(n, (c, HK_F_##a, HK_F_##b), (c, HK_P_##a, HK_P_##b), (c, HK_Z_##a, HK_Z_##b))
#define HK_H3(n, a, b, d) \
    HK_SHAPES(n, (c, HK_F_##a, HK_F_##b, HK_F_##d), (c, HK_P_##a, HK_P_##b, HK_P_##d), (c, HK_Z_##a, HK_Z_##b, HK_Z_##d))
#define HK_H4(n, a, b, d, e) \
    HK_SHAPES(n, (c, HK_F_##a, HK_F_##b, HK_F_##d, HK_F_##e), (c, HK_P_##a, HK_P_##b, HK_P_##d, HK_P_##e), \
              (c, HK_Z_##a, HK_Z_##b, HK_Z_##d, HK_Z_##e))
#define HK_H5(n, a, b, d, e, f) \
    HK_SHAPES(n, (c, HK_F_##a, HK_F_##b, HK_F_##d, HK_F_##e, HK_F_##f), (c, HK_P_##a, HK_P_##b, HK_P_##d, HK_P_##e, HK_P_##f), \
              (c, HK_Z_##a, HK_Z_##b, HK_Z_##d, HK_Z_##e, HK_Z_##f))
#define HK_H6(n, a, b, d, e, f, g) \
    HK_SHAPES(n, (c, HK_F_##a, HK_F_##b, HK_F_##d, HK_F_##e, HK_F_##f, HK_F_##g), \
              (c, HK_P_##a, HK_P_##b, HK_P_##d, HK_P_##e, HK_P_##f, HK_P_##g), \
              (c, HK_Z_##a, HK_Z_##b, HK_Z_##d, HK_Z_##e, HK_Z_##f, HK_Z_##g))
    HK_OPLIST(HK_H0, HK_H1, HK_H2, HK_H3, HK_H4, HK_H5, HK_H6)
#undef HK_H0
#undef HK_H1
#undef HK_H2
#undef HK_H3
#undef HK_H4
#undef HK_H5
#undef HK_H6
#undef HK_SHAPES
#ifdef HK_RUN_FAST
    // the commonest instructions, with what is fixed in them a constant
    HK_E(cmpi0) op_cmpi(c, 0, HK_F_ra, HK_F_simm); HK_STEP();
    HK_E(cmpli0) op_cmpli(c, 0, HK_F_ra, HK_F_uimm); HK_STEP();
    HK_E(cmp0) op_cmp(c, 0, HK_F_ra, HK_F_rb); HK_STEP();
    HK_E(cmpl0) op_cmpl(c, 0, HK_F_ra, HK_F_rb); HK_STEP();
    HK_E(mflr) op_mfspr(c, HK_F_rt, 8); HK_STEP();
    HK_E(mfctr) op_mfspr(c, HK_F_rt, 9); HK_STEP();
    HK_E(mtlr) op_mtspr(c, 8, HK_F_rt); HK_STEP();
    HK_E(mtctr) op_mtspr(c, 9, HK_F_rt); HK_STEP();
    // (the commonest branches: BO's other bits make no difference to hk_bc_ok, exec.cpp's shape())
#define HK_POLL_AT(at) do { if (__builtin_expect(hk_budget <= 0, 0)) { HK_RUN_OUTSIDE(); hk_poll(c, (at)); } } while (0)
#define HK_BRANCHES(name, bo) \
    HK_E(bc_##name) \
        if (!hk_bc_ok(c, bo, st->ra)) HK_AT(st->a); \
        HK_STEP(); \
    HK_E(br_##name) \
        if (hk_bc_ok(c, bo, st->ra)) HK_AT(st->a); \
        HK_STEP(); \
    HK_E(brp_##name) \
        if (hk_bc_ok(c, bo, st->ra)) { \
            HK_POLL_AT(st->y.b); \
            HK_AT(st->a); \
        } \
        HK_STEP();
    HK_BRANCHES(true, 12)
    HK_BRANCHES(false, 4)
    HK_BRANCHES(dnz, 16)
#undef HK_BRANCHES
#else
#define HK_POLL_AT(at) do { if (__builtin_expect(hk_budget <= 0, 0)) { HK_RUN_OUTSIDE(); hk_poll(c, (at)); } } while (0)
#endif
    // control flow: a branch's bo, bi and "always" are the step's rt, ra and rb
    HK_E(tick)
        hk_budget -= (int32_t)st->a;
        HK_STEP();
    HK_E(poll)
        HK_POLL_AT(st->a);
        HK_STEP();
    HK_E(go)
        HK_AT(st->a);
    HK_E(gop)
        HK_POLL_AT(st->y.b);
        HK_AT(st->a);
    HK_E(bc)
        if (!hk_bc_ok(c, st->rt, st->ra)) HK_AT(st->a);
        HK_STEP();
    HK_E(br)
        if (hk_bc_ok(c, st->rt, st->ra)) HK_AT(st->a);
        HK_STEP();
    HK_E(brp)
        if (hk_bc_ok(c, st->rt, st->ra)) {
            HK_POLL_AT(st->y.b);
            HK_AT(st->a);
        }
        HK_STEP();
    HK_E(tail)   // f(c); return: the callee's steps instead (a sibling call, no host stack)
        k = st->x.fn;
        if (__builtin_expect(hk_patched_any, 0) && patched(k->entry)) {   // changed by a mod: interpreted
            HK_RUN_OUTSIDE();
            hk_interp_function(c, k->entry);
            return;
        }
        HK_AT(0);
    HK_E(jump)
        HK_RUN_JUMP(c, st->a);
        return;
    HK_E(call)
        c->lr = st->a;
        HK_RUN(c, st->x.fn);
        if (__builtin_expect(c->pc != st->a, 0)) hk_bad_return(c, st->a);
        HK_STEP();
    HK_E(call_up)
        c->lr = st->a;
        k = st->x.fn;
        if (__builtin_expect(hk_patched_any, 0) && patched(k->entry)) {
            HK_RUN_OUTSIDE();
            hk_interp_function(c, k->entry);
            return;
        }
        HK_AT(0);
    HK_E(call_dyn)
        c->lr = st->a;
        HK_RUN_CALL_DYN(c, st->y.b);
        if (__builtin_expect(c->pc != st->a, 0)) hk_bad_return(c, st->a);
        HK_STEP();
    HK_E(set_lr)
        c->lr = st->a;
        HK_STEP();
    HK_E(bclrl) {
        const uint32_t t = c->lr & ~3u;
        c->lr = st->a;
        if (hk_bc_ok(c, st->rt, st->ra)) {
            HK_RUN_CALL_DYN(c, t);
            if (__builtin_expect(c->pc != st->a, 0)) hk_bad_return(c, st->a);
        }
        HK_STEP();
    }
    HK_E(ret)
        c->pc = c->lr & ~3u;
        return;
    HK_E(ret_if)
        if (hk_bc_ok(c, st->rt, st->ra)) { c->pc = c->lr & ~3u; return; }
        HK_STEP();
    HK_E(bcctrl) {
        const uint32_t t = c->ctr & ~3u;
        c->lr = st->a;
        if (st->rb || hk_bc_ok(c, st->rt, st->ra)) {
            HK_RUN_CALL_DYN(c, t);
            if (__builtin_expect(c->pc != st->a, 0)) hk_bad_return(c, st->a);
        }
        HK_STEP();
    }
    HK_E(table)
        if (st->rb || hk_bc_ok(c, st->rt, st->ra)) {
            const uint32_t v = c->ctr & ~3u;
            const auto &cases = k->tables[st->y.b];
            auto it = std::lower_bound(cases.begin(), cases.end(), std::make_pair(v, 0u));
            if (it != cases.end() && it->first == v && it->second != ~0u) HK_AT(it->second);
            HK_RUN_JUMP(c, v);
            return;
        }
        HK_STEP();
    HK_E(jump_ctr)
        if (st->rb || hk_bc_ok(c, st->rt, st->ra)) { HK_RUN_JUMP(c, c->ctr & ~3u); return; }
        HK_STEP();
    HK_E(sc)
        c->pc = st->a;
        HK_RUN_OUTSIDE();
        hk_syscall(c);
        HK_STEP();
    HK_E(rfi)
        c->msr = c->srr1;
        c->pc = c->srr0 & ~3u;
        return;
    HK_E(rfci)
        c->msr = c->srr3;
        c->pc = c->srr2 & ~3u;
        return;
    HK_E(unimpl)
        hk_unimplemented(c, st->a, st->y.b);
        return;
    HK_E(bad)
        fatal(c, "the executor met a step it has no handler for (%d)", (int)st->k);
#undef HK_F_rt
#undef HK_F_ra
#undef HK_F_rb
#undef HK_F_sh
#undef HK_F_mb
#undef HK_F_me
#undef HK_F_nb
#undef HK_F_crm
#undef HK_F_to
#undef HK_F_e
#undef HK_F_oe
#undef HK_F_rc
#undef HK_F_crfd
#undef HK_F_crfs
#undef HK_F_simm
#undef HK_F_uimm
#undef HK_F_spr
#undef HK_F_dcr
#undef HK_F_addr
#undef HK_P_rt
#undef HK_P_ra
#undef HK_P_rb
#undef HK_P_sh
#undef HK_P_mb
#undef HK_P_me
#undef HK_P_nb
#undef HK_P_crm
#undef HK_P_to
#undef HK_P_e
#undef HK_P_oe
#undef HK_P_rc
#undef HK_P_crfd
#undef HK_P_crfs
#undef HK_P_simm
#undef HK_P_uimm
#undef HK_P_spr
#undef HK_P_dcr
#undef HK_P_addr
#undef HK_Z_rt
#undef HK_Z_ra
#undef HK_Z_rb
#undef HK_Z_sh
#undef HK_Z_mb
#undef HK_Z_me
#undef HK_Z_nb
#undef HK_Z_crm
#undef HK_Z_to
#undef HK_Z_e
#undef HK_Z_oe
#undef HK_Z_rc
#undef HK_Z_crfd
#undef HK_Z_crfs
#undef HK_Z_simm
#undef HK_Z_uimm
#undef HK_Z_spr
#undef HK_Z_dcr
#undef HK_Z_addr
#undef HK_POLL_AT
#undef HK_STEP
#undef HK_AT
#undef HK_E
#undef HK_REG
#undef HK_DISPATCH
}
