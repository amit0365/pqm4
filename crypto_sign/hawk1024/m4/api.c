/*
 * pqm4 NIST API wrapper for HAWK-1024.
 *
 * Adapted from extra/api.c in https://github.com/hawk-sign/dev with
 * `unsigned long long` replaced by `size_t` (pqm4 convention) and LOGN
 * set to 10 for the 1024-degree variant. The shared core sources are
 * symlinked from ../../hawk512/m4/.
 */

#include <stddef.h>
#include <string.h>

#include "api.h"
#include "hawk.h"
#include "randombytes.h"

#define LOGN   10   /* HAWK-1024: n = 2^10 = 1024 */

#if HAWK_PRIVKEY_SIZE(LOGN) != CRYPTO_SECRETKEYBYTES \
	|| HAWK_PUBKEY_SIZE(LOGN) != CRYPTO_PUBLICKEYBYTES \
	|| HAWK_SIG_SIZE(LOGN) != CRYPTO_BYTES
#error Invalid scheme sizes
#endif

static void
hrng(void *ctx, void *dst, size_t len)
{
	(void)ctx;
	randombytes((unsigned char *)dst, len);
}

int
crypto_sign_keypair(unsigned char *pk, unsigned char *sk)
{
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
