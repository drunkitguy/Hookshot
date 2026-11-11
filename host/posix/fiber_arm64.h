/* (android) Fibers on arm64 without makecontext/swapcontext, which bionic lacks: the switch saves what the ABI says a call
   keeps (x19-x30, sp, d8-d15) and nothing else, as any function call would. Signal masks are not switched (swapcontext's
   were: no fiber here changes its mask). tests/fiber_arm64_test.c runs it. */
#pragma once
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint64_t x[12];   /* x19..x30 */
    uint64_t sp;
    uint64_t d[8];    /* d8..d15 */
} HosFiberCtx;

void hos_fiber_swap(HosFiberCtx *from, const HosFiberCtx *to);
void hos_fiber_entry(void);

__asm__(
    ".text\n"
    ".p2align 2\n"
    ".global hos_fiber_swap\n"
    ".hidden hos_fiber_swap\n"
    ".type hos_fiber_swap,%function\n"
    "hos_fiber_swap:\n"
    "    stp x19, x20, [x0, #0]\n"
    "    stp x21, x22, [x0, #16]\n"
    "    stp x23, x24, [x0, #32]\n"
    "    stp x25, x26, [x0, #48]\n"
    "    stp x27, x28, [x0, #64]\n"
    "    stp x29, x30, [x0, #80]\n"
    "    mov x9, sp\n"
    "    str x9, [x0, #96]\n"
    "    stp d8, d9, [x0, #104]\n"
    "    stp d10, d11, [x0, #120]\n"
    "    stp d12, d13, [x0, #136]\n"
    "    stp d14, d15, [x0, #152]\n"
    "    ldp x19, x20, [x1, #0]\n"
    "    ldp x21, x22, [x1, #16]\n"
    "    ldp x23, x24, [x1, #32]\n"
    "    ldp x25, x26, [x1, #48]\n"
    "    ldp x27, x28, [x1, #64]\n"
    "    ldp x29, x30, [x1, #80]\n"
    "    ldr x9, [x1, #96]\n"
    "    mov sp, x9\n"
    "    ldp d8, d9, [x1, #104]\n"
    "    ldp d10, d11, [x1, #120]\n"
    "    ldp d12, d13, [x1, #136]\n"
    "    ldp d14, d15, [x1, #152]\n"
    "    ret\n"
    ".size hos_fiber_swap, .-hos_fiber_swap\n"
    ".global hos_fiber_entry\n"
    ".hidden hos_fiber_entry\n"
    ".type hos_fiber_entry,%function\n"
    "hos_fiber_entry:\n"          /* a new fiber's first switch lands here: x19 its argument, x20 its function */
    "    mov x0, x19\n"
    "    mov x29, #0\n"
    "    mov x30, #0\n"
    "    br x20\n"
    ".size hos_fiber_entry, .-hos_fiber_entry\n");

static inline void hos_fiber_make(HosFiberCtx *c, void *stack, size_t size, void (*fn)(void *), void *arg) {
    for (size_t i = 0; i < sizeof *c / 8; i++) ((uint64_t *)c)[i] = 0;
    c->x[0] = (uint64_t)(uintptr_t)arg;                 /* x19 */
    c->x[1] = (uint64_t)(uintptr_t)fn;                  /* x20 */
    c->x[11] = (uint64_t)(uintptr_t)hos_fiber_entry;    /* x30 */
    c->sp = ((uint64_t)(uintptr_t)stack + size) & ~15ULL;
}
