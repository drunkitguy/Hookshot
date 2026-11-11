/* host/posix/fiber_arm64.h's switch: two fibers and the thread hand a counter round, with callee-saved integer and
   float values held across each switch. Build for arm64 and run (qemu-aarch64 does):
     aarch64-linux-gnu-gcc -O2 -static -Ihost/posix tests/fiber_arm64_test.c -o fiber_test && qemu-aarch64 ./fiber_test */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "fiber_arm64.h"

static HosFiberCtx main_ctx, fa, fb;
static int counter;

static void body(void *arg) {
    const long id = (long)arg;
    HosFiberCtx *self = id == 1 ? &fa : &fb;
    volatile double keep = 1.5 * (double)id;
    register long r = id * 1000;
    for (int i = 0; i < 1000; i++) {
        counter++;
        r++;
        hos_fiber_swap(self, &main_ctx);
        assert(keep == 1.5 * (double)id);
        assert(r == id * 1000 + i + 1);
    }
    for (;;) hos_fiber_swap(self, &main_ctx);
}

int main(void) {
    const size_t size = 1 << 16;
    hos_fiber_make(&fa, malloc(size), size, body, (void *)1);
    hos_fiber_make(&fb, malloc(size), size, body, (void *)2);
    double d = 3.25;
    for (int i = 0; i < 1000; i++) {
        hos_fiber_swap(&main_ctx, &fa);
        hos_fiber_swap(&main_ctx, &fb);
        d += 1.0;
    }
    assert(counter == 2000 && d == 1003.25);
    puts("fiber_arm64: ok");
    return 0;
}
