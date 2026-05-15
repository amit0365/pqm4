#include <stddef.h>

#define CRYPTO_SECRETKEYBYTES   360
#define CRYPTO_PUBLICKEYBYTES   2440
#define CRYPTO_BYTES            1221
#define CRYPTO_ALGNAME          "HAWK-1024"

int crypto_sign_keypair(unsigned char *pk, unsigned char *sk);
int crypto_sign(unsigned char *sm, size_t *smlen,
	const unsigned char *m, size_t mlen,
	const unsigned char *sk);
int crypto_sign_open(unsigned char *m, size_t *mlen,
	const unsigned char *sm, size_t smlen,
	const unsigned char *pk);
