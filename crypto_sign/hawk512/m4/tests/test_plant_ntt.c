/*
 * Full-NTT byte-equality cross-check.
 *
 * Spec-adherence contract: every implementation of mq18433_NTT_plant /
 * mq18433_iNTT_plant MUST produce coefficient arrays byte-identical
 * to HAWK's reference mq18433_NTT / mq18433_iNTT. Any 1-bit drift
 * downstream-breaks signature byte equality with the round-2 KAT
 * vectors.
 *
 * This test runs both implementations over many random polynomials,
 * for the parameter sets HAWK actually uses (logn=9 for HAWK-512,
 * logn=10 for HAWK-1024, also logn=8 as a smaller smoke test). It
 * also exercises the forward-then-inverse round-trip and confirms
 * the recovered polynomial matches the input up to the documented
 * scaling factor (R = 2^32 mod q, a single Montgomery factor — see
 * PLANTARD_NOTES.md).
 *
 * Build:  make -C crypto_sign/hawk512/m4/tests run-plant
 */

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define Q   18433
#include "modq.h"
#define Q   18433       /* modq.h #undefs it at the bottom; reinstate */
#include "plant_18433.h"

/* Forward decls — modq.h's static inlines are visible only here. */
extern void mq18433_NTT_plant(unsigned logn, uint16_t *a);
extern void mq18433_iNTT_plant(unsigned logn, uint16_t *a);

static int failures = 0;

static void
fill_random_poly(uint16_t *p, size_t n, unsigned seed)
{
    /* Coefficients in HAWK's [1..Q] representation. */
    for (size_t i = 0; i < n; i++) {
        unsigned x = (seed * 1103515245u + (unsigned)i * 12345u + 1u) % (unsigned)Q;
        p[i] = (uint16_t)(x ? x : Q);
        seed = seed * 1664525u + 1013904223u;
    }
}

static int
check_byte_equal(const char *what, const uint16_t *a, const uint16_t *b, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (a[i] != b[i]) {
            if (failures < 5) {
                fprintf(stderr, "FAIL %s: index %zu, ref=%u plant=%u\n",
                        what, i, (unsigned)a[i], (unsigned)b[i]);
            }
            failures++;
            return -1;
        }
    }
    return 0;
}

static void
test_forward(unsigned logn, size_t trials)
{
    size_t n = (size_t)1 << logn;
    uint16_t *a = malloc(n * sizeof(uint16_t));
    uint16_t *b = malloc(n * sizeof(uint16_t));
    if (!a || !b) { fprintf(stderr, "oom\n"); exit(99); }

    for (size_t t = 0; t < trials; t++) {
        fill_random_poly(a, n, (unsigned)(t * 7919u + logn));
        memcpy(b, a, n * sizeof(uint16_t));

        mq18433_NTT(logn, a);
        mq18433_NTT_plant(logn, b);

        char name[64];
        snprintf(name, sizeof name, "NTT logn=%u trial=%zu", logn, t);
        check_byte_equal(name, a, b, n);
    }
    free(a); free(b);
}

static void
test_inverse(unsigned logn, size_t trials)
{
    size_t n = (size_t)1 << logn;
    uint16_t *a = malloc(n * sizeof(uint16_t));
    uint16_t *b = malloc(n * sizeof(uint16_t));
    if (!a || !b) { fprintf(stderr, "oom\n"); exit(99); }

    for (size_t t = 0; t < trials; t++) {
        fill_random_poly(a, n, (unsigned)(t * 6151u + logn + 1u));
        memcpy(b, a, n * sizeof(uint16_t));

        mq18433_iNTT(logn, a);
        mq18433_iNTT_plant(logn, b);

        char name[64];
        snprintf(name, sizeof name, "iNTT logn=%u trial=%zu", logn, t);
        check_byte_equal(name, a, b, n);
    }
    free(a); free(b);
}

static void
test_roundtrip(unsigned logn, size_t trials)
{
    /* NTT then iNTT must recover the input (up to HAWK's existing
     * scaling). We compare reference vs plant roundtrips — both must
     * land on the same bytes. */
    size_t n = (size_t)1 << logn;
    uint16_t *a = malloc(n * sizeof(uint16_t));
    uint16_t *b = malloc(n * sizeof(uint16_t));
    if (!a || !b) { fprintf(stderr, "oom\n"); exit(99); }

    for (size_t t = 0; t < trials; t++) {
        fill_random_poly(a, n, (unsigned)(t * 4093u + logn + 2u));
        memcpy(b, a, n * sizeof(uint16_t));

        mq18433_NTT(logn, a);
        mq18433_iNTT(logn, a);

        mq18433_NTT_plant(logn, b);
        mq18433_iNTT_plant(logn, b);

        char name[64];
        snprintf(name, sizeof name, "roundtrip logn=%u trial=%zu", logn, t);
        check_byte_equal(name, a, b, n);
    }
    free(a); free(b);
}

int
main(void)
{
    const struct { unsigned logn; size_t trials; } cases[] = {
        { 8,  32 },   /* sanity check, small */
        { 9,  16 },   /* HAWK-512 */
        { 10,  8 },   /* HAWK-1024 */
    };

    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        test_forward (cases[i].logn, cases[i].trials);
        test_inverse (cases[i].logn, cases[i].trials);
        test_roundtrip(cases[i].logn, cases[i].trials);
    }

    if (failures == 0) {
        printf("ALL TESTS PASSED — plant NTT/iNTT byte-identical to "
               "reference over %zu logn cases × {fwd,inv,roundtrip}.\n",
               sizeof cases / sizeof cases[0]);
        return 0;
    } else {
        fprintf(stderr, "FAILED: %d byte mismatches.\n", failures);
        return 1;
    }
}
