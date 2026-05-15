/*
 * sha3.c — pqm4 backend for HAWK's SHAKE / SHA3 API.
 *
 * Replaces the in-tree Cortex-M4 Keccak permutation shipped by hawk-sign/dev
 * with pqm4's tuned bit-interleaved KeccakF1600_StatePermute, plus its
 * KeccakF1600_State{XOR,Extract}Bytes helpers. This is the same Keccak
 * primitive every other m4 scheme in pqm4 already uses, so optimisations
 * here amortise across the whole submission.
 *
 * The public surface (sha3.h) is preserved unchanged: shake_context and
 * its dptr/rate/A[25] fields keep the same layout, and the {init, inject,
 * flip, extract} cycle behaves exactly as the upstream sha3.c did.
 * Internally the state is stored bit-interleaved (pqm4 convention);
 * because no caller dereferences sc->A directly, this is invisible.
 *
 * The shake_x4 batch API is implemented as four serial SHAKE instances —
 * HAWK's reference does not call shake_x4_* from any cross-file path (it
 * is only used by the AVX2 build), and keeping a portable fallback here
 * preserves the API.
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "sha3.h"
#include "keccakf1600.h"

/* ------------------------------------------------------------------ */
/* SHAKE                                                              */
/* ------------------------------------------------------------------ */

void
shake_init(shake_context *sc, unsigned size)
{
	sc->rate = 200u - (size >> 2);
	sc->dptr = 0;
	memset(sc->A, 0, sizeof sc->A);
}

void
shake_inject(shake_context *sc, const void *in, size_t len)
{
	const uint8_t *buf = in;
	unsigned dptr = sc->dptr;
	unsigned rate = sc->rate;

	while (len > 0) {
		size_t clen = rate - dptr;
		if (clen > len) {
			clen = len;
		}
		KeccakF1600_StateXORBytes(sc->A, buf, dptr, (unsigned int)clen);
		dptr += (unsigned)clen;
		buf  += clen;
		len  -= clen;
		if (dptr == rate) {
			KeccakF1600_StatePermute(sc->A);
			dptr = 0;
		}
	}
	sc->dptr = dptr;
}

void
shake_flip(shake_context *sc)
{
	/* SHAKE padding: 0x1F at dptr, 0x80 at rate-1. Set dptr=rate so the
	 * next shake_extract() triggers a permutation, matching upstream. */
	uint8_t pad;
	pad = 0x1F;
	KeccakF1600_StateXORBytes(sc->A, &pad, sc->dptr, 1);
	pad = 0x80;
	KeccakF1600_StateXORBytes(sc->A, &pad, sc->rate - 1, 1);
	sc->dptr = sc->rate;
}

void
shake_extract(shake_context *sc, void *out, size_t len)
{
	uint8_t *buf = out;
	unsigned dptr = sc->dptr;
	unsigned rate = sc->rate;

	while (len > 0) {
		size_t clen;
		if (dptr == rate) {
			KeccakF1600_StatePermute(sc->A);
			dptr = 0;
		}
		clen = rate - dptr;
		if (clen > len) {
			clen = len;
		}
		KeccakF1600_StateExtractBytes(sc->A, buf, dptr, (unsigned int)clen);
		dptr += (unsigned)clen;
		buf  += clen;
		len  -= clen;
	}
	sc->dptr = dptr;
}

/* ------------------------------------------------------------------ */
/* shake_x4 — four-way batch. Portable serial fallback (no AVX2 here). */
/* ------------------------------------------------------------------ */

void
shake_x4_flip(shake_x4_context *scx4, const shake_context *sc_in)
{
	scx4->rate = sc_in[0].rate;
	scx4->dptr = sc_in[0].rate;   /* extract triggers permutation */
	for (int i = 0; i < 4; i++) {
		shake_context tmp = sc_in[i];
		shake_flip(&tmp);
		memcpy(scx4->A + i * 25, tmp.A, 25 * sizeof(uint64_t));
	}
}

void
shake_x4_extract_words(shake_x4_context *scx4, uint64_t *dst, size_t num_x4)
{
	/* Extract num_x4 groups of 4 little-endian 64-bit words, one from
	 * each of the four parallel SHAKE instances. Bytes go through the
	 * bit-interleaved permutation; we squeeze 8 bytes from each lane,
	 * then reinterpret as a 64-bit LE word. */
	uint8_t buf[8];
	while (num_x4 > 0) {
		size_t avail = (scx4->rate - scx4->dptr) / 8;
		if (avail == 0) {
			for (int i = 0; i < 4; i++) {
				KeccakF1600_StatePermute(scx4->A + i * 25);
			}
			scx4->dptr = 0;
			avail = scx4->rate / 8;
		}
		if (avail > num_x4) {
			avail = num_x4;
		}
		for (size_t k = 0; k < avail; k++) {
			for (int i = 0; i < 4; i++) {
				KeccakF1600_StateExtractBytes(scx4->A + i * 25,
					buf, scx4->dptr, 8);
				uint64_t w = 0;
				for (int j = 0; j < 8; j++) {
					w |= (uint64_t)buf[j] << (8 * j);
				}
				*dst++ = w;
			}
			scx4->dptr += 8;
		}
		num_x4 -= avail;
	}
}

/* ------------------------------------------------------------------ */
/* SHA3 (fixed-output)                                                */
/* ------------------------------------------------------------------ */

void
sha3_init(sha3_context *sc, unsigned size)
{
	sc->rate = 200u - (size >> 2);
	sc->dptr = 0;
	memset(sc->A, 0, sizeof sc->A);
}

void
sha3_update(sha3_context *sc, const void *in, size_t len)
{
	shake_inject(sc, in, len);
}

void
sha3_close(sha3_context *sc, void *out)
{
	/* SHA3 padding: 0x06 at dptr, 0x80 at rate-1. */
	uint8_t pad;
	pad = 0x06;
	KeccakF1600_StateXORBytes(sc->A, &pad, sc->dptr, 1);
	pad = 0x80;
	KeccakF1600_StateXORBytes(sc->A, &pad, sc->rate - 1, 1);
	KeccakF1600_StatePermute(sc->A);
	/* Digest length in bytes = size/8 where size = 4*(200 - rate) bits,
	 * i.e. (200 - rate) / 2. */
	unsigned digest_bytes = (200u - sc->rate) >> 1;
	KeccakF1600_StateExtractBytes(sc->A, out, 0, digest_bytes);
}
