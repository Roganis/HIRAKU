// SPDX-License-Identifier: GPL-3.0-or-later
// The exception glue and guest threads (docs/KERNEL.md).
//
// The guest's own kernel decides which thread runs: on the way out of every
// exception it points SPRG3 at the 384-byte register frame of the thread to
// resume. HIRAKU keeps that kernel - C and assembly alike - and only adds what
// a static recompilation needs: every guest thread's native call chain lives
// on its own host fiber.
//
//   1. An exception (sc, or an interrupt at a poll point) is taken exactly as
//      the CPU would, and the guest's vector code runs in the interpreter. It
//      saves registers into the frame and calls the (native) C handlers.
//   2. When its rfi returns to user state, SPRG3 names the resumed thread's
//      frame. If that is the frame this thread pushed, we return to the
//      native code that took the exception.
//   3. Otherwise we park this fiber under its frame address and switch to the
//      fiber parked under the target frame. It returns from ITS glue call
//      with the registers the exit path just loaded - its own.
//   4. A frame no fiber is parked under is a new thread: a new fiber starts
//      running guest code at the frame's SRR0.
//
// Only one fiber runs at a time and hk_ctx is "the CPU", shared by all.
//
// Fibers switch tens of thousands of times per second (every guest thread
// switch). On x86-64 and AArch64 the switch is a few instructions of our own:
// libc's swapcontext also saves and restores the signal mask, a system call
// each time, which cost ~14 % of a whole run (and Android's libc has no
// ucontext functions at all). Windows has fibers of its own (SwitchToFiber, no system call
// either), which also keep the thread's idea of its stack right (shell/sys.h). Other hosts use
// ucontext.
#if defined(_WIN32)
#define HK_FIBER_WIN32 1
#include "shell/sys.h"
#else
#include <sys/mman.h>
#include <unistd.h>
#if (defined(__x86_64__) || defined(__aarch64__)) && defined(__ELF__)
#define HK_FIBER_ASM 1
#else
#include <ucontext.h>
#endif
#endif

#include <cstdio>
#include <unordered_map>
#include <vector>

#include "cpu403.h"
#include "exec.h"
#include "prof.h"
#include "runtime.h"
#include "sched.h"

namespace hk {

namespace {

constexpr size_t FIBER_STACK = 1 << 20;

#if HK_FIBER_ASM
// hk_fiber_switch(&save_sp, load_sp): push the callee-saved registers and the
// floating-point control word(s) on this stack, store the stack pointer in
// *save_sp, load load_sp and pop the same from there. A new fiber's stack is
// prepared by fiber_stack_init so that this "returns" into fiber_entry.
extern "C" void hk_fiber_switch(void **save_sp, void *load_sp);
#if defined(__x86_64__)
asm(R"(
    .text
    .globl hk_fiber_switch
    .hidden hk_fiber_switch
    .type hk_fiber_switch, @function
hk_fiber_switch:
    pushq %rbp
    pushq %rbx
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
    subq $8, %rsp
    stmxcsr (%rsp)
    fnstcw 4(%rsp)
    movq %rsp, (%rdi)
    movq %rsi, %rsp
    ldmxcsr (%rsp)
    fldcw 4(%rsp)
    addq $8, %rsp
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %rbx
    popq %rbp
    ret
    .size hk_fiber_switch, .-hk_fiber_switch
)");
#else
// AArch64: x19-x28, the frame pointer x29, the link register x30 (where to
// return to), d8-d15 (the low halves of v8-v15: all the ABI preserves) and FPCR.
asm(R"(
    .text
    .globl hk_fiber_switch
    .hidden hk_fiber_switch
    .type hk_fiber_switch, %function
hk_fiber_switch:
    sub sp, sp, #176
    stp x19, x20, [sp, #0]
    stp x21, x22, [sp, #16]
    stp x23, x24, [sp, #32]
    stp x25, x26, [sp, #48]
    stp x27, x28, [sp, #64]
    stp x29, x30, [sp, #80]
    stp d8, d9, [sp, #96]
    stp d10, d11, [sp, #112]
    stp d12, d13, [sp, #128]
    stp d14, d15, [sp, #144]
    mrs x2, fpcr
    str x2, [sp, #160]
    mov x2, sp
    str x2, [x0]
    mov sp, x1
    ldr x2, [sp, #160]
    msr fpcr, x2
    ldp x19, x20, [sp, #0]
    ldp x21, x22, [sp, #16]
    ldp x23, x24, [sp, #32]
    ldp x25, x26, [sp, #48]
    ldp x27, x28, [sp, #64]
    ldp x29, x30, [sp, #80]
    ldp d8, d9, [sp, #96]
    ldp d10, d11, [sp, #112]
    ldp d12, d13, [sp, #128]
    ldp d14, d15, [sp, #144]
    add sp, sp, #176
    ret
    .size hk_fiber_switch, .-hk_fiber_switch
)");
#endif
#endif

struct Fiber {
#if HK_FIBER_WIN32
    void *handle = nullptr;    // the Win32 fiber (the host thread's: made at its first switch)
#elif HK_FIBER_ASM
    void *sp = nullptr;        // saved stack pointer while not running
#else
    ucontext_t uc{};
#endif
    void *stack = nullptr;
    uint32_t frame = 0;        // guest frame it is parked under
    uint32_t resume_pc = 0;    // the SRR0 that frame must carry for it to be ours
    int id = 0;
};

hk_ctx *g_c = nullptr;
Fiber g_main_fiber;                                // the host thread itself (boot + first guest thread)
Fiber *g_cur = &g_main_fiber;
std::unordered_map<uint32_t, Fiber *> g_parked;    // frame -> fiber
std::vector<Fiber *> g_graveyard;                  // fibers whose thread died (frame reused)
int g_next_id = 1;
bool g_fibers_enabled = false;

#if !HK_FIBER_WIN32
// A fiber's stack, with a page under it that nothing may touch. A stack that runs out (guest code
// that calls itself without end, in a build that doesn't turn its tail calls into jumps) stops the
// program there, with the crash report, where it used to write over whatever memory came next.
// (Windows' own fibers have such a page.)
size_t stack_guard() {
    static const size_t page = (size_t)sysconf(_SC_PAGESIZE);
    return page;
}

void *stack_new() {
    void *base = mmap(nullptr, stack_guard() + FIBER_STACK, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_STACK, -1, 0);
    if (base == MAP_FAILED) return MAP_FAILED;
    mprotect(base, stack_guard(), PROT_NONE);   // (the stack grows down, towards it)
    return static_cast<char *>(base) + stack_guard();
}

void stack_free(void *stack) { munmap(static_cast<char *>(stack) - stack_guard(), stack_guard() + FIBER_STACK); }
#endif

void free_fiber(Fiber *f) {
#if HK_FIBER_WIN32
    sys::fiber_delete(f->handle);
#else
    stack_free(f->stack);
#endif
    delete f;
}

// Entry of a new guest thread's fiber: run guest code from c->pc, forever.
// (A guest thread ends by entering the kernel and never being resumed.)
void thread_main() {
    hk_ctx *c = g_c;
    for (;;) {
        if (call_guest(c, c->pc)) {
            // returned with c->pc = LR (HK_RET)
        } else {
            interp_run(c, 0xffffffffu, 0, UINT64_MAX);
            fatal(c, "guest thread interpreter loop ended");
        }
    }
}

#if HK_FIBER_ASM
[[noreturn]] void fiber_entry() {
    thread_main();
    __builtin_unreachable();
}

#if defined(__x86_64__)
// The frame hk_fiber_switch pops, from the saved stack pointer up: control
// words, r15 r14 r13 r12 rbx rbp, return address (fiber_entry), then a dummy
// return address for fiber_entry itself, which keeps the ABI's alignment
// (rsp + 8 a multiple of 16 on function entry).
void *fiber_stack_init(void *stack) {
    auto *top = reinterpret_cast<uint64_t *>(static_cast<char *>(stack) + FIBER_STACK);
    uint64_t *sp = top - 9;
    uint32_t csr[2] = {};
    asm volatile("stmxcsr %0\n\tfnstcw %1" : "=m"(csr[0]), "=m"(csr[1]));
    sp[0] = (uint64_t)csr[1] << 32 | csr[0];
    for (int k = 1; k <= 6; k++) sp[k] = 0;
    sp[7] = reinterpret_cast<uint64_t>(&fiber_entry);
    sp[8] = 0;
    return sp;
}
#else
// The 176-byte frame hk_fiber_switch pops (see there), at the top of the
// stack: x30 = fiber_entry, x29 = 0 (the end of the frame chain), the current
// FPCR, the rest zero. sp is the stack's top on entry, 16-aligned as the ABI
// wants.
void *fiber_stack_init(void *stack) {
    auto *top = reinterpret_cast<uint64_t *>(static_cast<char *>(stack) + FIBER_STACK);
    uint64_t *sp = top - 22;
    for (int k = 0; k < 22; k++) sp[k] = 0;
    sp[11] = reinterpret_cast<uint64_t>(&fiber_entry);
    uint64_t fpcr;
    asm volatile("mrs %0, fpcr" : "=r"(fpcr));
    sp[20] = fpcr;
    return sp;
}
#endif
#endif

Fiber *new_thread_fiber() {
    auto *f = new Fiber;
    f->id = g_next_id++;
#if HK_FIBER_WIN32
    f->handle = sys::fiber_new(FIBER_STACK, thread_main);
    if (!f->handle) fatal(g_c, "cannot make a fiber");
#else
    f->stack = stack_new();
    if (f->stack == MAP_FAILED) fatal(g_c, "cannot allocate a fiber stack");
#if HK_FIBER_ASM
    f->sp = fiber_stack_init(f->stack);
#else
    getcontext(&f->uc);
    f->uc.uc_stack.ss_sp = f->stack;
    f->uc.uc_stack.ss_size = FIBER_STACK;
    f->uc.uc_link = nullptr;
    makecontext(&f->uc, thread_main, 0);
#endif
#endif
    return f;
}

void switch_threads(hk_ctx *c, uint32_t my_frame, uint32_t my_resume, uint32_t target_frame) {
    Fiber *me = g_cur;
    me->frame = my_frame;
    me->resume_pc = my_resume;
    if (auto old = g_parked.find(my_frame); old != g_parked.end() && old->second != me)
        g_graveyard.push_back(old->second);      // a dead thread's fiber, its frame reused
    g_parked[my_frame] = me;

    Fiber *next;
    auto it = g_parked.find(target_frame);
    if (it != g_parked.end() && it->second->resume_pc == c->pc) {
        next = it->second;
        g_parked.erase(it);
    } else {
        if (it != g_parked.end()) {              // parked there, but the frame now belongs to a new thread
            g_graveyard.push_back(it->second);
            g_parked.erase(it);
        }
        next = new_thread_fiber();
        g_stats.threads_started++;
    }
    g_stats.thread_switches++;
    g_cur = next;
#if HK_FIBER_WIN32
    if (!me->handle) me->handle = sys::fiber_this_thread();
    sys::fiber_switch(next->handle);
#elif HK_FIBER_ASM
    hk_fiber_switch(&me->sp, next->sp);
#else
    swapcontext(&me->uc, &next->uc);
#endif
    // resumed: the exit path that chose us has loaded our registers
    g_cur = me;
    for (Fiber *f : g_graveyard)
        if (f != me) free_fiber(f);
    g_graveyard.clear();
}

// Run the guest's vector code natively when it was recompiled: it saves the
// frame, calls the kernel's handlers and ends in rfi (emitted as a return with
// pc = SRR0, msr = SRR1). Anything left (no recompiled vector) is interpreted
// by the caller's loop.
void run_vector(hk_ctx *c) {
    if (g_interp.lle) return;
    if (has_guest(c->pc)) {
        g_stats.native_vectors++;
        // (the executor's: the same run as the last one from this state isn't run again, exec.cpp)
        if (!exec::call_memo(c, c->pc)) call_guest(c, c->pc);
        // its rfi returned here: as the interpreter's rfi does, an interrupt already pending is
        // taken at once, not at the next poll that finds the budget spent (up to the next event)
        if (cpu403::irq_pending()) sched_kick();
    }
}

}  // namespace

void glue_init(hk_ctx *c) {
    g_c = c;
    g_fibers_enabled = true;
}

// Take exception `vector` with return address `ret`, run the guest's vector
// code, and come back when this thread is resumed (possibly much later).
void glue_exception(hk_ctx *c, uint32_t vector, uint32_t ret) {
    const bool from_user = c->msr & msr::PR;
    const uint32_t my_frame = c->r[1] - 384;    // where the vector code will push our frame
    g_stats.exceptions++;
    if (!from_user) {
        // From supervisor state (e.g. an interrupt handler calling a kernel service
        // through `sc`): the vector code pushes the frame on the current stack and
        // its exit path returns to the same context - no thread switch is possible.
        const uint32_t msr0 = c->msr;
        prof::Scope ps(prof::GLUE);
        take_exception(c, vector, ret);
        run_vector(c);
        for (uint64_t n = 0; !(c->pc == ret && c->msr == msr0); n++) {
            interp_step(c);
            if (n > 50'000'000) fatal(c, "supervisor exception %04x from %08x never returned", vector, ret);
        }
        return;
    }
    {
        prof::Scope ps(prof::GLUE);   // the vector code only: not the time other threads run after a switch
        take_exception(c, vector, ret);
        run_vector(c);
        for (uint64_t n = 0; !(c->msr & msr::PR); n++) {
            interp_step(c);
            if (n > 50'000'000) fatal(c, "exception %04x from %08x never returned to user state", vector, ret);
        }
    }
    const uint32_t target = c->sprg[3];
    if (target == my_frame && c->pc == ret) return;
    if (!g_fibers_enabled) fatal(c, "thread switch with fibers disabled");
    switch_threads(c, my_frame, ret, target);
}

}  // namespace hk

using namespace hk;

extern "C" {

// Recompiled code: a poll point ran out of budget.
void hk_poll(hk_ctx *c, uint32_t pc) {
    if (shadow_native_active()) {   // time stands still while a shadow check runs natively
        hk_budget = 1 << 20;
        return;
    }
    sched_sync();
    if ((c->msr & msr::EE) && cpu403::irq_pending()) glue_exception(c, cpu403::pending_vector(), pc);
}

// Recompiled code: `sc`. c->pc is the address after it.
void hk_syscall(hk_ctx *c) {
    if (shadow_native_active()) shadow_abandon();
    glue_exception(c, 0xc00, c->pc);
}

}  // extern "C"
