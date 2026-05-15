/*
 * params.h — HAWK-512 parameters (Round-2 spec, May 2025).
 *
 * The HAWK modulus q is fixed at 12289 for compatibility with Falcon-style
 * NTTs, but HAWK works in Z[x]/(x^n + 1) with secret-key entries in a
 * small range so the dominant arithmetic is over a smaller modulus. The
 * exact moduli used by the Plantard NTT and FFT are recorded here.
 */

#ifndef HAWK512_M4_PARAMS_H
#define HAWK512_M4_PARAMS_H

#define HAWK_LOGN     9
#define HAWK_N      (1 << HAWK_LOGN)   /* 512 */

/* Plantard arithmetic modulus (Q = 18433 = 2^14 + 2^11 + 1 candidate;
 * final value set once the NTT layer is implemented). */
#define HAWK_Q       18433
#define HAWK_QINV    /* TODO */
#define HAWK_R2MOD   /* TODO */

#endif /* HAWK512_M4_PARAMS_H */
