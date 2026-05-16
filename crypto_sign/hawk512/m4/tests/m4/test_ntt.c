/*
 * test_ntt — QEMU bare-metal cross-check of mq18433_NTT_plant (asm)
 * against the reference Zq(NTT) in modq.h.
 *
 * Sweeps logn from 1 to 9 (HAWK-512 max). For each, fills a random
 * polynomial in canonical [1..Q], applies both NTTs to fresh copies,
 * and compares element-by-element. Reports first-mismatch index for
 * each failing logn to localise bugs to a specific layer.
 */

#include <stdint.h>
#include <stddef.h>

#define Q   18433
#include "../../modq.h"
#define Q   18433                /* modq.h #undef's Q at bottom; restore */

#include "semihost.h"

extern void mq18433_NTT_plant(unsigned logn, uint16_t *a);
extern void mq18433_iNTT_plant(unsigned logn, uint16_t *a);

/* Simple LCG so the test is deterministic and self-contained. */
static uint32_t rng_state = 0x42424242u;
static uint32_t rng_next(void) {
    rng_state = rng_state * 1664525u + 1013904223u;
    return rng_state;
}

#define MAXN 1024   /* up to HAWK-1024 (logn=10) */

static uint16_t buf_ref[MAXN];
static uint16_t buf_asm[MAXN];

static int cmp_buffers(const char *label, unsigned logn, size_t N) {
    int mismatches = 0;
    size_t first_idx = 0;
    for (size_t i = 0; i < N; i++) {
        if (buf_ref[i] != buf_asm[i]) {
            if (mismatches == 0) first_idx = i;
            mismatches++;
        }
    }
    if (mismatches == 0) {
        semi_write0("  ");
        semi_write0(label);
        semi_write0(" logn=");
        semi_write_u32(logn);
        semi_write0(": PASS (N=");
        semi_write_u32((uint32_t)N);
        semi_write0(")\n");
    } else {
        semi_write0("  ");
        semi_write0(label);
        semi_write0(" logn=");
        semi_write_u32(logn);
        semi_write0(": FAIL ");
        semi_write_u32((uint32_t)mismatches);
        semi_write0(" mismatches, first at i=");
        semi_write_u32((uint32_t)first_idx);
        semi_write0(" (ref=");
        semi_write_u32(buf_ref[first_idx]);
        semi_write0(" got=");
        semi_write_u32(buf_asm[first_idx]);
        semi_write0(")\n");
    }
    return mismatches;
}

int main(void) {
    semi_write0("test_ntt: cross-checking NTT_plant / iNTT_plant vs Zq(NTT/iNTT)\n");

    int total_fail = 0;

    for (unsigned logn = 1; logn <= 10; logn++) {
        size_t N = (size_t)1 << logn;

        /* Random polynomial in [1..Q]. */
        for (size_t i = 0; i < N; i++) {
            uint16_t v = (uint16_t)(1u + (rng_next() % Q));
            buf_ref[i] = v;
            buf_asm[i] = v;
        }

        mq18433_NTT(logn, buf_ref);
        mq18433_NTT_plant(logn, buf_asm);
        total_fail += cmp_buffers("NTT ", logn, N);
    }

    /* iNTT sweep with fresh random inputs (random NTT-domain coefficients
     * are still valid inputs in [1..Q]). */
    for (unsigned logn = 1; logn <= 10; logn++) {
        size_t N = (size_t)1 << logn;
        for (size_t i = 0; i < N; i++) {
            uint16_t v = (uint16_t)(1u + (rng_next() % Q));
            buf_ref[i] = v;
            buf_asm[i] = v;
        }
        mq18433_iNTT(logn, buf_ref);
        mq18433_iNTT_plant(logn, buf_asm);
        total_fail += cmp_buffers("iNTT", logn, N);
    }

    if (total_fail == 0) {
        semi_write0("ALL TESTS PASSED\n");
    } else {
        semi_write0("FAIL: ");
        semi_write_u32((uint32_t)total_fail);
        semi_write0(" total mismatches\n");
    }
    return total_fail > 0 ? 1 : 0;
}
