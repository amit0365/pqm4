/*
 * sign.c — HAWK-1024 signature generation (pqm4 m4 implementation).
 *
 * Hot path: isochronous discrete-Gaussian sampler -> Plantard NTT -> packing.
 * Performance target: <1.1 M cycles on STM32L4R5ZI.
 */

#include "api.h"
#include "params.h"

int crypto_sign(unsigned char *sm, size_t *smlen,
	const unsigned char *m, size_t mlen,
	const unsigned char *sk) {
    (void)sm;
    (void)smlen;
    (void)m;
    (void)mlen;
    (void)sk;
    /* TODO:
     *   1. SHAKE256(salt || m) -> challenge hash h
     *   2. solve B * x = h with sampled (x0, x1) ~ D_sigma
     *      (isochronous CDT sampler from precomputed table)
     *   3. compress signature (Golomb-Rice on s1)
     *   4. emit sm = (salt || sig); *smlen = mlen + CRYPTO_BYTES
     */
    return -1;
}
