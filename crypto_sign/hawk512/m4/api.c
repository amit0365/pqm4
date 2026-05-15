/*
 * pqm4 NIST API wrapper for HAWK.
 *
 * Adapted from extra/api.c in https://github.com/hawk-sign/dev — the
 * size types are switched from `unsigned long long` (NIST round-2 reference
 * API) to `size_t` to match pqm4's test/speed harness.
 */

#include <stddef.h>
#include <string.h>

#include "api.h"
#include "hawk.h"
#include "randombytes.h"

#ifndef LOGN
#define LOGN   9   /* HAWK-512: n = 2^9 = 512 */
#endif

/* Compile-time check: sizes in api.h match the scheme. */
#if HAWK_PRIVKEY_SIZE(LOGN) != CRYPTO_SECRETKEYBYTES \
	|| HAWK_PUBKEY_SIZE(LOGN) != CRYPTO_PUBLICKEYBYTES \
	|| HAWK_SIG_SIZE(LOGN) != CRYPTO_BYTES
#error Invalid scheme sizes
#endif

/*
 * Wrapper for pqm4's randombytes() that matches the hawk_rng prototype.
 * pqm4's randombytes() takes (uint8_t *, size_t).
 */
static void
hrng(void *ctx, void *dst, size_t len)
{
	(void)ctx;
	randombytes((unsigned char *)dst, len);
}

int
crypto_sign_keypair(unsigned char *pk, unsigned char *sk)
{
	/* The temporary buffer is large (~22 KiB for n=512, ~45 KiB for n=1024).
	 * pqm4's stack benchmarks measure usage end-to-end; keeping the buffer
	 * on the stack avoids any heap dependency on Cortex-M4. */
	unsigned char tmp[HAWK_TMPSIZE_KEYGEN(LOGN)];

	if (!hawk_keygen(LOGN, sk, pk, &hrng, 0, tmp, sizeof tmp)) {
		return -1;
	}
	return 0;
}

int
crypto_sign(unsigned char *sm, size_t *smlen,
	const unsigned char *m, size_t mlen,
	const unsigned char *sk)
{
	unsigned char tmp[HAWK_TMPSIZE_SIGN(LOGN)];
	shake_context sc;

	if (m != sm) {
		memmove(sm, m, mlen);
	}
	hawk_sign_start(&sc);
	shake_inject(&sc, sm, mlen);
	if (!hawk_sign_finish(LOGN, &hrng, 0,
		sm + mlen, &sc, sk, tmp, sizeof tmp))
	{
		return -1;
	}
	*smlen = mlen + HAWK_SIG_SIZE(LOGN);
	return 0;
}

int
crypto_sign_open(unsigned char *m, size_t *mlen,
	const unsigned char *sm, size_t smlen,
	const unsigned char *pk)
{
	unsigned char tmp[HAWK_TMPSIZE_VERIFY(LOGN)];
	shake_context sc;
	size_t dlen;

	if (smlen < HAWK_SIG_SIZE(LOGN)) {
		return -1;
	}
	dlen = smlen - HAWK_SIG_SIZE(LOGN);
	hawk_verify_start(&sc);
	shake_inject(&sc, sm, dlen);
	if (!hawk_verify_finish(LOGN, sm + dlen, HAWK_SIG_SIZE(LOGN),
		&sc, pk, HAWK_PUBKEY_SIZE(LOGN), tmp, sizeof tmp))
	{
		return -1;
	}
	if (m != sm) {
		memmove(m, sm, dlen);
	}
	*mlen = dlen;
	return 0;
}
