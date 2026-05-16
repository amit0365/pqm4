/*
 * plant_18433.c — portable C reference for HAWK Q=18433 NTT/iNTT.
 *
 * This is the host-buildable correctness reference for the
 * forthcoming Cortex-M4 assembly version. It is structurally
 * identical to the static inline mq18433_NTT / mq18433_iNTT in
 * modq.h (Cooley-Tukey forward, Gentleman-Sande inverse), which
 * makes its output byte-identical to the reference by construction
 * — verified end-to-end by tests/test_plant_ntt.c.
 *
 * Why a separate translation unit rather than a #define alias?
 *
 *   - It gives the M4 asm a stable, externally-linkable symbol set
 *     to satisfy (mq18433_NTT_plant, mq18433_iNTT_plant,
 *     mq18433_montymul_plant). The asm file replaces this .c file
 *     when built for ARMv7E-M (see plant_18433_cm4.S in a follow-up).
 *
 *   - It lets hawk_sign.c toggle the optimised path via
 *     HAWK_PLANT_NTT without touching the upstream modq.h static
 *     inlines.
 *
 * The performance win comes from the asm; this C version exists
 * purely to (a) prove the wiring (cross-check test passes), and
 * (b) give a portable fallback for host builds and platforms
 * without M4 DSP support.
 *
 * Spec-adherence: see PLANTARD_NOTES.md. Byte-identical output to
 * the reference is REQUIRED — any divergence breaks signature
 * byte-equality with HAWK round-2 KAT vectors.
 */

#include <stddef.h>
#include <stdint.h>

#define Q   18433
#include "modq.h"

#include "plant_18433.h"

/* Forward NTT — Cooley-Tukey, in-place. Mirrors Zq(NTT) in modq.h
 * line-for-line; calls into the static inline mq18433_montymul. The
 * planned M4 asm replaces this body with a layer-merged hand-scheduled
 * kernel using packed-pair butterflies (smulwb/smulwt + smlatb), with
 * the rounding correction encoded as a constant '+1<<16' added pre-
 * shift in smlatb (see PLANTARD_NOTES.md for the locked-down 3-instr
 * reducer that's byte-identical to Zq(montyred)). */
void
mq18433_NTT_plant(unsigned logn, uint16_t *a)
{
	size_t t = (size_t)1 << logn;
	for (unsigned lm = 0; lm < logn; lm++) {
		size_t m = (size_t)1 << lm;
		size_t ht = t >> 1;
		size_t v0 = 0;
		for (size_t u = 0; u < m; u++) {
			uint32_t s = mq18433_GM[u + m];
			for (size_t v = 0; v < ht; v++) {
				size_t k1 = v0 + v;
				size_t k2 = k1 + ht;
				uint32_t x1 = a[k1];
				uint32_t x2 = mq18433_montymul(a[k2], s);
				a[k1] = mq18433_add(x1, x2);
				a[k2] = mq18433_sub(x1, x2);
			}
			v0 += t;
		}
		t = ht;
	}
}

/* Inverse NTT — Gentleman-Sande, in-place. Mirrors Zq(iNTT). */
void
mq18433_iNTT_plant(unsigned logn, uint16_t *a)
{
	size_t t = 1;
	for (unsigned lm = 0; lm < logn; lm++) {
		size_t hm = (size_t)1 << (logn - 1 - lm);
		size_t dt = t << 1;
		size_t v0 = 0;
		for (size_t u = 0; u < hm; u++) {
			uint32_t s = mq18433_iGM[u + hm];
			for (size_t v = 0; v < t; v++) {
				size_t k1 = v0 + v;
				size_t k2 = k1 + t;
				uint32_t x1 = a[k1];
				uint32_t x2 = a[k2];
				a[k1] = mq18433_half(mq18433_add(x1, x2));
				a[k2] = mq18433_montymul(s, mq18433_sub(x1, x2));
			}
			v0 += dt;
		}
		t = dt;
	}
}

uint32_t
mq18433_montymul_plant(uint32_t x, uint32_t y)
{
	return mq18433_montymul(x, y);
}
