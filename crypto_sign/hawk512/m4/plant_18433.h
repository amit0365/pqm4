/*
 * plant_18433.h — drop-in alternative NTT entry points for HAWK Q=18433.
 *
 * Same prototype shape as the static inlines in modq.h (mq18433_NTT,
 * mq18433_iNTT, mq18433_montymul, ...) — but with external linkage
 * so an out-of-line implementation (C reference here, hand-scheduled
 * Cortex-M4 asm in plant_18433_cm4.S, later) can be selected by the
 * HAWK_PLANT_NTT build flag.
 *
 * Spec-adherence contract (PLANTARD_NOTES.md): every implementation
 * MUST produce coefficients byte-identical to mq18433_NTT / mq18433_iNTT.
 * Drift breaks downstream signature byte equality with the HAWK
 * round-2 KAT vectors and is therefore a defect, not a tradeoff.
 *
 * The cross-check test tests/test_plant_ntt.c enforces this.
 */

#ifndef PLANT_18433_H__
#define PLANT_18433_H__

#include <stddef.h>
#include <stdint.h>

/* Forward NTT (Cooley-Tukey, in-place). Inputs/outputs in HAWK's
 * single-Montgomery representation, values in [1..18433]. */
void mq18433_NTT_plant(unsigned logn, uint16_t *a);

/* Inverse NTT (Gentleman-Sande, in-place). */
void mq18433_iNTT_plant(unsigned logn, uint16_t *a);

/* Plantard-equivalent of mq18433_montymul. Lives behind the same flag
 * so hawk_sign.c's per-coefficient pointwise multiply can be routed
 * through the same kernel as the NTT. */
uint32_t mq18433_montymul_plant(uint32_t x, uint32_t y);

#endif /* PLANT_18433_H__ */
