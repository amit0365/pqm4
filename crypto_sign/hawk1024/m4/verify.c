/*
 * verify.c — HAWK-1024 signature verification (pqm4 m4 implementation).
 *
 * Hot path: decompress signature -> Plantard NTT of (s0, s1) -> verify
 * ||(s0, s1)||^2 <= bound and B*s == h mod 2.
 * Performance target: <650 k cycles on STM32L4R5ZI.
 */

#include "api.h"
#include "params.h"

int crypto_sign_open(unsigned char *m, size_t *mlen,
	const unsigned char *sm, size_t smlen,
	const unsigned char *pk) {
    (void)m;
    (void)mlen;
    (void)sm;
    (void)smlen;
    (void)pk;
    /* TODO:
     *   1. parse sm = (salt || sig); decompress sig -> s1
     *   2. SHAKE256(salt || m) -> h
     *   3. reconstruct s0 from (h, s1, q00, q01) via NTT
     *   4. test ||(s0, s1)||^2 against verification bound
     */
    return -1;
}
