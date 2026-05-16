/*
 * Plantard twiddle-encoding validator.
 *
 * Goal: derive the int32_t encoding of HAWK's mq18433_GM[] that lets
 * pqm4's existing `mul_twiddle_plant` asm macro (from
 * ml-dsa-{44,65,87}/m4f/macros_smallntt.i) produce results byte-identical to
 * `mq18433_montymul(a, mq18433_GM[x])` — the spec-required output.
 *
 * Simulates the asm in C so the encoding can be locked before any
 * .S file is touched. Searches over candidate encodings; the winner
 * (zero mismatches over N random (a, x) pairs) is the encoding to
 * generate the production GM_plant[] table from.
 *
 * The asm macro (verbatim from ml-dsa-44/m4f/macros_smallntt.i):
 *
 *   .macro mul_twiddle_plant a, twiddle, tmp, q, qa
 *       smulwb tmp, twiddle, a       @ tmp = (twiddle * a_low16 ) >> 16
 *       smulwt a,   twiddle, a       @ a   = (twiddle * a_high16) >> 16
 *       smlabt tmp, tmp, q, qa       @ tmp = (tmp_low16 * q_hi16) + qa
 *       smlabt a,   a,   q, qa       @ a   = (a_low16   * q_hi16) + qa
 *       pkhtb  a,   a,   tmp, asr#16 @ pack tmp_hi16 a_hi16 into a
 *   .endm
 *
 * Two packed coefficients per call. For this single-coefficient
 * validator we only use the low-half path (smulwb plus smlabt plus
 * extract tmp_hi16) ; the high-half path mirrors it.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define Q   18433
#include "modq.h"
#define Q   18433       /* modq.h #undefs at the bottom */

static int32_t
smulwb(int32_t rn, int32_t rm)
{
    /* rd = (rn * SignExt(rm[15:0])) >> 16, signed 32-bit result */
    int16_t lo = (int16_t)(rm & 0xFFFF);
    int64_t prod = (int64_t)rn * (int64_t)lo;
    return (int32_t)(prod >> 16);
}

static int32_t
smlabt(int32_t rn, int32_t rm, int32_t ra)
{
    /* rd = (SignExt(rn[15:0]) * SignExt(rm[31:16])) + ra */
    int16_t rn_lo = (int16_t)(rn & 0xFFFF);
    int16_t rm_hi = (int16_t)((rm >> 16) & 0xFFFF);
    int32_t prod = (int32_t)rn_lo * (int32_t)rm_hi;
    return prod + ra;
}

/* Simulate the low-half path of mul_twiddle_plant for a single coefficient.
 * Returns the high 16 bits of `tmp` after smlabt, which is the modular
 * residue in the asm convention. */
static int32_t
sim_plant_low(int32_t a_low16_signed, int32_t twiddle_enc,
              int32_t q_packed, int32_t qa)
{
    int32_t tmp = smulwb(twiddle_enc, a_low16_signed);
    tmp = smlabt(tmp, q_packed, qa);
    return (tmp >> 16) & 0xFFFF;
}

/* Same but treat result as unsigned uint16, mapping the modular value
 * back into HAWK's [1..Q] convention. */
static uint32_t
sim_plant_low_u(int32_t a, int32_t twiddle_enc,
                int32_t q_packed, int32_t qa)
{
    int32_t r = sim_plant_low(a, twiddle_enc, q_packed, qa);
    if (r <= 0) r += Q;
    if (r > Q)  r -= Q;
    return (uint32_t)r;
}

/* Candidate encoders: given a twiddle value ω in [1..Q] (HAWK's GM[]
 * representation, which is g^rev * 2^32 mod q), produce a 32-bit
 * encoding to pass as `twiddle` to the asm. */

#define Q0I   3955247103u                   /* -q^{-1} mod 2^32 (HAWK)        */
#define PINV  339720193u                    /* +q^{-1} mod 2^32 (Plantard pos) */

static int32_t encA(uint32_t v) { return (int32_t)(v * Q0I);       }   /* ω·Q0I  */
static int32_t encB(uint32_t v) { return (int32_t)(v * PINV);      }   /* ω·Qinv */
static int32_t encC(uint32_t v) { return (int32_t)((uint32_t)v << 16); }
static int32_t encD(uint32_t v) { return (int32_t)v;               }   /* identity */

typedef int32_t (*enc_fn)(uint32_t);
struct combo { const char *name; enc_fn enc; int32_t qa; };

static const struct combo combos[] = {
    {"encA qa=0x10000",  encA, 0x10000   },   /* +1 after >>16 (montyred)   */
    {"encA qa=q",         encA, Q         },   /* Plantard rc=1 convention   */
    {"encA qa=q<<5",      encA, Q << 5    },   /* ml-dsa-style (rc=32, fits) */
    {"encB qa=0x10000",  encB, 0x10000   },
    {"encB qa=q",         encB, Q         },
    {"encC qa=0x10000",  encC, 0x10000   },
    {"encD qa=0x10000",  encD, 0x10000   },
};

int
main(void)
{
    const size_t NTRIAL = 4096;
    int32_t q_packed = (int32_t)((Q << 16) | Q);

    uint32_t miscount[sizeof combos / sizeof combos[0]] = {0};
    uint32_t fma[sizeof combos / sizeof combos[0]] = {0};
    uint32_t fmb[sizeof combos / sizeof combos[0]] = {0};
    uint32_t fmg[sizeof combos / sizeof combos[0]] = {0};
    uint32_t fmc[sizeof combos / sizeof combos[0]] = {0};

    srand(1);
    for (size_t t = 0; t < NTRIAL; t++) {
        uint32_t a = 1u + (uint32_t)(rand() % Q);
        uint32_t b = 1u + (uint32_t)(rand() % Q);
        uint32_t g = mq18433_montymul(a, b);

        for (size_t k = 0; k < sizeof combos / sizeof combos[0]; k++) {
            uint32_t got = sim_plant_low_u((int32_t)a, combos[k].enc(b),
                                           q_packed, combos[k].qa);
            if (got != g) {
                if (miscount[k] == 0) {
                    fma[k] = a; fmb[k] = b; fmg[k] = g; fmc[k] = got;
                }
                miscount[k]++;
            }
        }
    }

    printf("Plantard twiddle-encoding search vs mq18433_montymul "
           "(%zu trials):\n\n", NTRIAL);
    printf("%-22s %12s  %s\n", "combo", "mismatches",
           "first miss (a, b) -> golden / got");
    printf("%-22s %12s  %s\n", "-----", "----------",
           "-------------------------------");
    int any_pass = 0;
    for (size_t k = 0; k < sizeof combos / sizeof combos[0]; k++) {
        if (miscount[k] == 0) {
            printf("%-22s %12s  (matches all)\n", combos[k].name, "0");
            any_pass = 1;
        } else {
            printf("%-22s %12u  (a=%u, b=%u) -> g=%u, got=%u\n",
                   combos[k].name, miscount[k],
                   fma[k], fmb[k], fmg[k], fmc[k]);
        }
    }

    return any_pass ? 0 : 2;
}
