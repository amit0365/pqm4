/*
 * smoke_hawk — end-to-end HAWK-512 roundtrip under QEMU Cortex-M4,
 * built with HAWK_PLANT_NTT=1 so the NTT path goes through our asm
 * (plant_18433_cm4.S).
 *
 * Steps:
 *   1. Generate a keypair via hawk_keygen (deterministic RNG seed).
 *   2. Sign a fixed message via hawk_sign_start/inject/finish.
 *   3. Verify the signature via hawk_verify_start/inject/finish.
 *   4. Report PASS/FAIL via semihosting.
 *
 * If the asm has any subtle bug not caught by the per-primitive
 * cross-check (e.g., a register clobber in a code path the NTT test
 * doesn't exercise), the verify will fail.
 *
 * Test sizes (HAWK-512, logn=9):
 *   priv: 184 B  |  pub: 1024 B  |  sig: 555 B
 *   tmp keygen:  ~22*512 = 11280 B
 *   tmp sign:     ~6*512 = 3079 B
 *   tmp verify:  ~10*512 = 5127 B
 */

#include <stdint.h>
#include <stddef.h>
#include "../../hawk.h"
#include "../../api.h"
#include "semihost.h"

#define LOGN 9

extern void randombytes(unsigned char *out, size_t outlen);
extern void randombytes_reseed(uint32_t seed);

static unsigned char priv[CRYPTO_SECRETKEYBYTES];
static unsigned char pub [CRYPTO_PUBLICKEYBYTES];
static unsigned char sig [CRYPTO_BYTES];

/* HAWK_TMPSIZE_KEYGEN(9) = 22*512+7 = 11271. Round up to 16-byte align. */
static unsigned char tmp_kgen[12288] __attribute__((aligned(16)));
/* HAWK_TMPSIZE_SIGN(9) = 6*512+7 = 3079. */
static unsigned char tmp_sign[4096]  __attribute__((aligned(16)));
/* HAWK_TMPSIZE_VERIFY(9) = 10*512+7 = 5127. */
static unsigned char tmp_vrfy[6144]  __attribute__((aligned(16)));

/* RNG wrapper matching hawk_rng prototype. */
static void hrng(void *ctx, void *dst, size_t len) {
    (void)ctx;
    randombytes((unsigned char *)dst, len);
}

int main(void) {
    semi_write0("smoke_hawk: HAWK-512 keygen+sign+verify with asm NTT\n");

    /* Deterministic seed → reproducible run. */
    randombytes_reseed(0xDEADBEEFu);

    /* --- 1. Keygen --- */
    int rc = hawk_keygen(LOGN, priv, pub, &hrng, 0, tmp_kgen, sizeof tmp_kgen);
    if (!rc) {
        semi_write0("FAIL: hawk_keygen returned 0\n");
        return 1;
    }
    semi_write0("  keygen: OK\n");

    /* --- 2. Sign --- */
    const char *msg = "HAWK-m4 smoke test message";
    size_t mlen = 26;

    shake_context sc;
    hawk_sign_start(&sc);
    shake_inject(&sc, msg, mlen);
    rc = hawk_sign_finish(LOGN, &hrng, 0, sig, &sc, priv,
                          tmp_sign, sizeof tmp_sign);
    if (!rc) {
        semi_write0("FAIL: hawk_sign_finish returned 0\n");
        return 2;
    }
    semi_write0("  sign:   OK\n");

    /* --- 3. Verify --- */
    hawk_verify_start(&sc);
    shake_inject(&sc, msg, mlen);
    rc = hawk_verify_finish(LOGN, sig, CRYPTO_BYTES, &sc, pub,
                            CRYPTO_PUBLICKEYBYTES, tmp_vrfy, sizeof tmp_vrfy);
    if (!rc) {
        semi_write0("FAIL: hawk_verify_finish returned 0\n");
        return 3;
    }
    semi_write0("  verify: OK\n");

    /* --- 4. Tamper test: verify FAILS on modified signature --- */
    sig[0] ^= 1;
    hawk_verify_start(&sc);
    shake_inject(&sc, msg, mlen);
    rc = hawk_verify_finish(LOGN, sig, CRYPTO_BYTES, &sc, pub,
                            CRYPTO_PUBLICKEYBYTES, tmp_vrfy, sizeof tmp_vrfy);
    if (rc) {
        semi_write0("FAIL: tampered signature verified (rc=1)\n");
        return 4;
    }
    semi_write0("  tamper: rejected (OK)\n");
    sig[0] ^= 1;  /* restore */

    semi_write0("ALL TESTS PASSED — HAWK-512 sign/verify works under asm NTT\n");
    return 0;
}
