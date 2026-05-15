/*
 * keygen.c — HAWK-512 key generation (pqm4 m4 implementation).
 *
 * Strategy:
 *   1. Sample (f, g) using NTRUgen (Pornin, eprint 2025/1239); the portable
 *      C is reused unchanged, with M4 inline assembly grafted into the
 *      recursive base case.
 *   2. NTRU-solve to get (F, G).
 *   3. Compute the HAWK public key (q00, q01) in Plantard-NTT domain.
 *   4. Encode pk = (q00, q01); sk = compressed (f, g, F, G).
 */

#include "api.h"
#include "params.h"

int crypto_sign_keypair(unsigned char *pk, unsigned char *sk) {
    (void)pk;
    (void)sk;
    return -1;
}
