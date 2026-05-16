/*
 * test_montymul — QEMU bare-metal Cortex-M4 cross-check of
 * mq18433_montymul_plant (the asm leaf in ../../plant_18433_cm4.S)
 * against the reference Zq(montymul) static inline in
 * ../../modq.h.
 *
 * Strategy: drive a deterministic sequence of (a, b) pairs through
 * BOTH paths and report any mismatch via semihosting before exit.
 * Exit code = number of mismatches (0 on success).
 *
 * Build/run: see Makefile in this directory.
 *   make
 *   make run
 */

#include <stdint.h>
#include <stddef.h>              /* modq.h uses size_t in poly_* helpers */

#define Q   18433
#include "../../modq.h"          /* exposes static inline mq18433_montymul */
#define Q   18433                /* modq.h #undef's Q at the bottom; restore */

#include "semihost.h"

/* Asm leaf under test. */
extern uint32_t mq18433_montymul_plant(uint32_t x, uint32_t y);

/* Tiny linear-congruential RNG so we don't pull in libc's rand. */
static uint32_t rng_state = 0x12345678u;
static uint32_t rng_next(void) {
    rng_state = rng_state * 1664525u + 1013904223u;
    return rng_state;
}

int main(void) {
    semi_write0("test_montymul: cross-checking mq18433_montymul_plant vs Zq(montymul)\n");

    /* Spot-check a few known small cases first to catch gross breakage. */
    struct { uint32_t a, b; } spot[] = {
        {1, 1}, {1, Q}, {Q, 1}, {Q, Q},
        {2, 2}, {100, 200}, {Q/2, Q/2}, {17, 33},
    };
    int n_spot = (int)(sizeof(spot) / sizeof(spot[0]));
    int fail = 0;
    for (int i = 0; i < n_spot; i++) {
        uint32_t a = spot[i].a, b = spot[i].b;
        uint32_t ref = mq18433_montymul(a, b);
        uint32_t got = mq18433_montymul_plant(a, b);
        if (ref != got) {
            fail++;
            semi_write0("  SPOT MISMATCH a="); semi_write_u32(a);
            semi_write0(" b="); semi_write_u32(b);
            semi_write0(" ref="); semi_write_u32(ref);
            semi_write0(" got="); semi_write_u32(got);
            semi_write0("\n");
        }
    }

    /* Random sweep over [1..Q]x[1..Q]. */
    const int NTRIAL = 4096;
    for (int i = 0; i < NTRIAL; i++) {
        uint32_t a = 1u + (rng_next() % Q);
        uint32_t b = 1u + (rng_next() % Q);
        uint32_t ref = mq18433_montymul(a, b);
        uint32_t got = mq18433_montymul_plant(a, b);
        if (ref != got) {
            if (fail < 8) {     /* throttle to first 8 to keep semihosting output sane */
                semi_write0("  RAND MISMATCH a="); semi_write_u32(a);
                semi_write0(" b="); semi_write_u32(b);
                semi_write0(" ref="); semi_write_u32(ref);
                semi_write0(" got="); semi_write_u32(got);
                semi_write0("\n");
            }
            fail++;
        }
    }

    if (fail == 0) {
        semi_write0("ALL TESTS PASSED (spot 8 + random 4096)\n");
    } else {
        semi_write0("FAIL: ");
        semi_write_u32((uint32_t)fail);
        semi_write0(" mismatches\n");
    }
    return fail;
}
