/*
 * Plantard NTT calibration — find the (plant_red variant, GM_plant
 * encoding) pair whose output is BYTE-IDENTICAL to HAWK's reference
 * mq18433_montymul over the entire input space.
 *
 * Goal (HAWK-spec adherence): a candidate Plantard formulation
 * passes only when, for every (a, b) ∈ [1..Q]², it produces the
 * same integer representative in [1..Q] that HAWK's reference
 * Montgomery multiplication produces. Modulo-equivalence is not
 * good enough — the HAWK signature encoding is fixed, so any
 * deviation from the reference's exact representatives would
 * desynchronise sign / verify from the round-2 KAT vectors.
 *
 * This is a STUB. It currently:
 *   - establishes the golden reference (mq18433_montymul wrapper),
 *   - enumerates a small set of candidate Plantard reductions and
 *     candidate b-encodings,
 *   - reports which (variant, encoding) pair matches over a sample
 *     of random inputs.
 *
 * The actual winning combination will be locked in
 * crypto_sign/hawk512/m4/plant_18433.{h,c} in a follow-up commit
 * once this test reports a clean win.
 *
 * Build:  make -C crypto_sign/hawk512/m4/tests run-calibrate
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

/* Pull in HAWK's reference Montgomery routines for Q=18433.
 * Note: modq.h #undefs Q at the bottom, so we re-define it for our
 * own use after the include. */
#define Q   18433
#include "modq.h"
#define Q   18433

/* ------------------------------------------------------------------ */
/* Plantard constants for Q=18433 (see PLANTARD_NOTES.md).            */
/* ------------------------------------------------------------------ */

#define PLANT_QINV   ((uint32_t)339720193u)  /* q^{-1} mod 2^32, q · Qinv ≡ +1 */
#define PLANT_QA     (1)                     /* rounding correction +1 (q<2^15) */

/* ------------------------------------------------------------------ */
/* Candidate Plantard reductions.                                     */
/*                                                                    */
/* Each takes a 32-bit (signed or unsigned) input and returns a       */
/* uint32_t result in some canonical range. The calibration loop      */
/* below also tries "fix-up to [1..q]" tweaks on the output.          */
/* ------------------------------------------------------------------ */

/* Variant 1 — straight Plantard from UIC-ESLAS C reference, unsigned. */
static int32_t
plant_red_v1(int32_t a)
{
    int32_t t = (int32_t)((uint32_t)a * PLANT_QINV);   /* low 32 bits */
    t >>= 16;
    t = (t + PLANT_QA) * (int32_t)Q;
    t >>= 16;
    return t;     /* signed, in roughly [-q/2, q/2] */
}

/* Variant 2 — same, but normalise the output to HAWK's [1..q] range. */
static uint32_t
plant_red_v2(int32_t a)
{
    int32_t r = plant_red_v1(a);
    if (r <= 0) r += Q;          /* lift [-q/2..0] into [q/2..q]      */
    return (uint32_t)r;
}

/* Variant 3 — Plantard with negated Qinv (Plantard 2021 paper sign). */
static int32_t
plant_red_v3(int32_t a)
{
    int32_t t = (int32_t)((uint32_t)a * (uint32_t)(-(int32_t)PLANT_QINV));
    t >>= 16;
    t = (t + PLANT_QA) * (int32_t)Q;
    t >>= 16;
    return t;
}

static uint32_t
plant_red_v3n(int32_t a)
{
    int32_t r = plant_red_v3(a);
    if (r <= 0) r += Q;
    return (uint32_t)r;
}

/* Variant 6 — monty-style: unsigned arithmetic, "+1" at end. This is
 * literally Zq(montyred) inlined; included as a sanity-check upper
 * bound: anything that doesn't tie with this is buggy. */
static uint32_t
plant_red_v6(int32_t a)
{
    uint32_t t = (uint32_t)a * (uint32_t)3955247103u;   /* HAWK's Q0I */
    t = (t >> 16) * (uint32_t)Q;
    return (t >> 16) + 1u;
}

/* Variant 7 — same as v1 but unsigned shifts (logical). HAWK's q < 2^15
 * and the products fit comfortably below 2^31 so the inputs are positive
 * either way; this rules out signed-shift surprises in v1. */
static uint32_t
plant_red_v7(int32_t a)
{
    uint32_t t = (uint32_t)a * PLANT_QINV;
    t >>= 16;
    t = (t + PLANT_QA) * (uint32_t)Q;
    t >>= 16;
    return t;
}

/* Variant 8 — same as v3 (Q0I path) but unsigned shifts. */
static uint32_t
plant_red_v8(int32_t a)
{
    uint32_t t = (uint32_t)a * 3955247103u;
    t >>= 16;
    t = (t + PLANT_QA) * (uint32_t)Q;
    t >>= 16;
    return t;
}

/* ------------------------------------------------------------------ */
/* Candidate twiddle pre-encodings.                                   */
/*                                                                    */
/* Given a value v in [1..q] (HAWK convention), each encoder returns  */
/* the 32-bit "b" passed to plant_mul = plant_red(a · b).             */
/* ------------------------------------------------------------------ */

#define R2_MOD_Q     ((uint32_t)(((uint64_t)1 << 32) % (uint64_t)Q))  /* 2^32 mod q */

/* encode "b" so that plant_red(a · b) lands in single-Montgomery
 * representation matching montymul(a, b_orig). */
static uint32_t encA(uint32_t v) { return v * PLANT_QINV; }                                    /* v · qinv               */
static uint32_t encB(uint32_t v) { return v * R2_MOD_Q * PLANT_QINV; }                         /* v · R² · qinv          */
static uint32_t encC(uint32_t v) { return (v << 16) * PLANT_QINV; }                            /* v · R · qinv           */
static uint32_t encD(uint32_t v) { return (uint32_t)((uint64_t)v * R2_MOD_Q * PLANT_QINV); }   /* same as B, explicit u64 */
static uint32_t encId(uint32_t v) { return v; }                                                /* identity (no pre-mul)  */

typedef uint32_t (*plant_red_fn)(int32_t);
typedef uint32_t (*enc_fn)(uint32_t);

struct combo {
    const char *name;
    plant_red_fn red;
    enc_fn enc;
};

static const struct combo combos[] = {
    /* Original 8 (kept for diagnostic continuity). */
    {"v1+encA", (plant_red_fn)plant_red_v2,  encA},
    {"v1+encB", (plant_red_fn)plant_red_v2,  encB},
    {"v1+encC", (plant_red_fn)plant_red_v2,  encC},
    {"v1+encD", (plant_red_fn)plant_red_v2,  encD},
    {"v3+encA", (plant_red_fn)plant_red_v3n, encA},
    {"v3+encB", (plant_red_fn)plant_red_v3n, encB},
    {"v3+encC", (plant_red_fn)plant_red_v3n, encC},
    {"v3+encD", (plant_red_fn)plant_red_v3n, encD},

    /* v6 is montyred itself — identity encoding must match by definition. */
    {"v6+encId", plant_red_v6, encId},

    /* Unsigned-shift variants with both Qinv signs. */
    {"v7+encId", plant_red_v7, encId},
    {"v7+encA",  plant_red_v7, encA},
    {"v8+encId", plant_red_v8, encId},
    {"v8+encA",  plant_red_v8, encA},
};

/* ------------------------------------------------------------------ */
/* Calibration driver                                                 */
/* ------------------------------------------------------------------ */

static uint32_t
golden_montymul(uint32_t a, uint32_t b)
{
    return mq18433_montymul(a, b);
}

static uint32_t
candidate(const struct combo *c, uint32_t a, uint32_t b_orig)
{
    return c->red((int32_t)((uint32_t)a * c->enc(b_orig)));
}

int
main(void)
{
    const size_t NTRIAL = 4096;
    uint32_t miscount[sizeof combos / sizeof combos[0]] = {0};
    uint32_t first_miss_a[sizeof combos / sizeof combos[0]] = {0};
    uint32_t first_miss_b[sizeof combos / sizeof combos[0]] = {0};
    uint32_t first_miss_g[sizeof combos / sizeof combos[0]] = {0};
    uint32_t first_miss_c[sizeof combos / sizeof combos[0]] = {0};

    srand(1);
    for (size_t t = 0; t < NTRIAL; t++) {
        uint32_t a = 1u + (uint32_t)(rand() % Q);   /* [1..Q] */
        uint32_t b = 1u + (uint32_t)(rand() % Q);
        uint32_t g = golden_montymul(a, b);

        for (size_t k = 0; k < sizeof combos / sizeof combos[0]; k++) {
            uint32_t got = candidate(&combos[k], a, b);
            if (got != g) {
                if (miscount[k] == 0) {
                    first_miss_a[k] = a;
                    first_miss_b[k] = b;
                    first_miss_g[k] = g;
                    first_miss_c[k] = got;
                }
                miscount[k]++;
            }
        }
    }

    printf("Plantard calibration vs mq18433_montymul over %zu random "
           "(a, b) pairs in [1..%d]:\n\n", NTRIAL, Q);
    printf("%-10s  %10s  %s\n", "combo", "mismatches", "first miss (a, b) -> "
                                                       "golden / got");
    printf("%-10s  %10s  %s\n", "-----", "----------", "-----------------------"
                                                       "-------------");
    int any_pass = 0;
    for (size_t k = 0; k < sizeof combos / sizeof combos[0]; k++) {
        if (miscount[k] == 0) {
            printf("%-10s  %10s  (matches all)\n", combos[k].name, "0");
            any_pass = 1;
        } else {
            printf("%-10s  %10u  (a=%u, b=%u) -> g=%u, got=%u\n",
                   combos[k].name, miscount[k],
                   first_miss_a[k], first_miss_b[k],
                   first_miss_g[k], first_miss_c[k]);
        }
    }

    if (!any_pass) {
        printf("\nNo candidate matched.\n");
        return 2;
    }

    /* Findings, recorded inline so future readers know exactly why we
     * landed where we did:
     *
     *   v6+encId — literally Zq(montyred) inlined, identity encoding.
     *              Matches all inputs (must, by construction).
     *
     *   v8+encId — Plantard form with HAWK's Q0I (= -q^{-1} mod 2^32),
     *              unsigned shifts, qa = +1, identity encoding. Misses
     *              ~7.6% of random inputs by exactly 1. The difference
     *              is where the rounding +1 is applied:
     *                  montyred: ((h*Q) >> 16) + 1
     *                  v8:       ((h+1)*Q) >> 16
     *              These differ when (h*Q) mod 2^16 < (2^16 - Q),
     *              empirically ~7.6% of (a,b) pairs in [1..Q]^2 (the
     *              non-uniformity of h = TOP16(c*Q0I mod 2^32) for
     *              c in [1, Q^2] keeps this far below the uniform-h
     *              prediction of ~72%).
     *
     * HAWK spec requires byte-identical NTT coefficients — any
     * non-zero mismatch breaks downstream sign/verify byte equality
     * with the reference. So the M4 asm port must reproduce montyred
     * exactly, not naive Plantard. That costs one extra instruction
     * per coefficient (3 cycles instead of 2) but keeps KAT
     * compatibility. The packed-pair structure still wins overall.
     */
    return 0;
}
