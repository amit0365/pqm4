/*
 * basis_m2_mul_cm4.S -- Cortex-M4 (ARMv7E-M) helpers for HAWK's
 * basis_m2_mul (GF(2)[x] / (x^n + 1) polynomial multiplication for
 * the sign-side B*h mod 2 step).
 *
 * Provides:
 *
 *   uint64_t bp_mul_32_cm4(uint32_t x, uint32_t y);
 *
 *     Carryless multiply 32x32 -> 64 over GF(2)[X]. Output bit i of
 *     the 64-bit result is XOR over all (j,k) with j+k=i of bit-j of
 *     x times bit-k of y. The return value is packed in r0:r1
 *     (low:high) per AAPCS uint64_t convention. Byte-identical to
 *     bp_mul_32() in hawk_sign.c.
 *
 *   void bp_muladd_64_cm4(uint8_t *d, const uint8_t *a,
 *                         const uint8_t *b);
 *
 *     d[0..15] ^= a[0..7] * b[0..7]   (GF(2)[X], 128-bit product
 *     XOR'd into 16-byte d). Internally INLINES the three bp_mul_32
 *     Karatsuba phases (no `bl` calls inside) using the
 *     BP_MUL32_BODY asm macro below. Byte-identical to
 *     bp_muladd_64() in hawk_sign.c. The fourth argument to the C
 *     version (`tmp`) is unused, so we drop it from the asm
 *     signature.
 *
 *   void bp_mul_64_cm4(uint8_t *d, const uint8_t *a,
 *                      const uint8_t *b);
 *
 *     d[0..15] = a[0..7] * b[0..7]   (GF(2)[X], 128-bit product
 *     ASSIGNED into 16-byte d). Same inlined Karatsuba as
 *     bp_muladd_64_cm4, but writes the result directly (no read-
 *     modify-write of d). Used by Path-1 bp_mul_n macros in
 *     hawk_sign.c to eliminate the memset(t1, 0, 64) /
 *     memset(d, 0, 64) calls that bracket the "first muladd" inside
 *     bp_mulmod_512.
 *
 * Design: GF(2)[X] 32x32 via Pornin's "holes" technique. Each
 * operand is split into 4 cosets (x AND m, x AND m<<1, x AND m<<2,
 * x AND m<<3 with m = 0x11111111). The 16 native umulls produce
 * four 64-bit accumulators z_0..z_3 that, when masked with the
 * corresponding 0x11111111<<k pattern, land on disjoint bit slots
 * and can be OR'd together for the final result. Same algorithm as
 * the C bp_mul_32 in hawk_sign.c.
 *
 * Why inline bp_mul_32 inside bp_mul_64_cm4 / bp_muladd_64_cm4 (vs
 * the prior agent's separate bp_mul_32_cm4 leaf)? Eliminating three
 * push/pop r4-r11 frames per leaf call (~18 inst/frame × 3 = 54
 * inst saved) plus three bl/bx pairs and three arg-setup movs.
 * Register pressure (which the prior comment cited as blocking the
 * inline) is resolved by spilling (a0, a1, b0, b1) to the stack at
 * function entry and reloading per phase — the saved push/pop pair
 * is bigger than the spill/reload overhead.
 *
 * The standalone bp_mul_32_cm4 is kept for any external callers
 * (currently none in HAWK-512 sign path after the inline) and so
 * that the BP_MUL32_BODY macro is exercised at the symbol-export
 * boundary too (catches asm typos that would otherwise lurk only
 * inside macro expansions).
 */

#if defined(__ARM_FEATURE_DSP) && __ARM_FEATURE_DSP

    .syntax unified
    .cpu cortex-m4
    .thumb

    .text


@ ===================================================================
@ BP_MUL32_BODY: GF(2)[X] 32x32 -> 64 inner kernel ("holes" technique)
@
@ Inputs:
@   r0 = x, r1 = y
@   r12 = mask = 0x11111111 (preloaded by caller)
@
@ Outputs:
@   lr = result_lo
@   [sp, #0] = result_hi
@
@ Clobbers: r0-r11 (lr is the running result_lo; r12 mask preserved).
@
@ Stack: requires sp[#0..3] to be writable (caller reserves it). No
@ push/pop inside the macro — caller owns the frame.
@
@ Length: ~62 instructions. Identical schedule to the bp_mul_32_cm4
@ standalone function below (which itself just wraps the macro).
@ ===================================================================
.macro BP_MUL32_BODY
    @ Split x and y into 4 cosets each (8 ANDs total)
    and     r2, r0, r12
    and     r3, r0, r12, lsl #1
    and     r4, r0, r12, lsl #2
    and     r5, r0, r12, lsl #3
    and     r6, r1, r12
    and     r7, r1, r12, lsl #1
    and     r8, r1, r12, lsl #2
    and     r9, r1, r12, lsl #3

    @ ----- z_0 = (x0*y0) ^ (x1*y3) ^ (x2*y2) ^ (x3*y1) -----
    umull   r10, r11, r2, r6             @ seed
    umull   r0,  r1,  r3, r9
    eor     r10, r10, r0
    eor     r11, r11, r1
    umull   r0,  r1,  r4, r8
    eor     r10, r10, r0
    eor     r11, r11, r1
    umull   r0,  r1,  r5, r7
    eor     r10, r10, r0
    eor     r11, r11, r1
    @ Mask z_0 with 0x11111111 (this z_k seeds the running result)
    and     lr,  r10, r12                @ result_lo = z_0_lo
    and     r0,  r11, r12                @ result_hi seed
    str     r0,  [sp]

    @ ----- z_1 = (x0*y1) ^ (x1*y0) ^ (x2*y3) ^ (x3*y2) -----
    umull   r10, r11, r2, r7
    umull   r0,  r1,  r3, r6
    eor     r10, r10, r0
    eor     r11, r11, r1
    umull   r0,  r1,  r4, r9
    eor     r10, r10, r0
    eor     r11, r11, r1
    umull   r0,  r1,  r5, r8
    eor     r10, r10, r0
    eor     r11, r11, r1
    @ Mask z_1 with 0x22222222 and OR into running result
    and     r10, r10, r12, lsl #1
    and     r11, r11, r12, lsl #1
    orr     lr,  lr,  r10
    ldr     r0,  [sp]
    orr     r0,  r0,  r11
    str     r0,  [sp]

    @ ----- z_2 = (x0*y2) ^ (x1*y1) ^ (x2*y0) ^ (x3*y3) -----
    umull   r10, r11, r2, r8
    umull   r0,  r1,  r3, r7
    eor     r10, r10, r0
    eor     r11, r11, r1
    umull   r0,  r1,  r4, r6
    eor     r10, r10, r0
    eor     r11, r11, r1
    umull   r0,  r1,  r5, r9
    eor     r10, r10, r0
    eor     r11, r11, r1
    @ Mask z_2 with 0x44444444 and OR into running result
    and     r10, r10, r12, lsl #2
    and     r11, r11, r12, lsl #2
    orr     lr,  lr,  r10
    ldr     r0,  [sp]
    orr     r0,  r0,  r11
    str     r0,  [sp]

    @ ----- z_3 = (x0*y3) ^ (x1*y2) ^ (x2*y1) ^ (x3*y0) -----
    umull   r10, r11, r2, r9
    umull   r0,  r1,  r3, r8
    eor     r10, r10, r0
    eor     r11, r11, r1
    umull   r0,  r1,  r4, r7
    eor     r10, r10, r0
    eor     r11, r11, r1
    umull   r0,  r1,  r5, r6
    eor     r10, r10, r0
    eor     r11, r11, r1
    @ Mask z_3 with 0x88888888, OR into result (final z_k phase, leave
    @ result_lo in lr and result_hi in r0 directly — caller skips a
    @ str/ldr round-trip vs the older "park at [sp,#0]" convention).
    and     r10, r10, r12, lsl #3
    and     r11, r11, r12, lsl #3
    orr     lr,  lr,  r10                @ final result_lo
    ldr     r0,  [sp]
    orr     r0,  r0,  r11                @ final result_hi (held in r0)
.endm


@ ===================================================================
@ uint64_t bp_mul_32_cm4(uint32_t x, uint32_t y);
@
@   r0 = x (in)         r0 = result_lo (out)
@   r1 = y (in)         r1 = result_hi (out)
@
@ Standalone leaf function: wraps BP_MUL32_BODY with the AAPCS frame.
@ Kept for any external callers and as a smoke-check that the macro
@ instantiates correctly at the symbol-export boundary. The HAWK-512
@ sign path no longer calls this — bp_mul_64_cm4 and
@ bp_muladd_64_cm4 below inline the body directly.
@ ===================================================================

    .global bp_mul_32_cm4
    .type   bp_mul_32_cm4, %function
    .thumb_func
    .align  2
bp_mul_32_cm4:
    push    {r4-r11, lr}                 @ save callee-saves + lr
    sub     sp, sp, #4                   @ result_hi park slot used by macro

    @ Load mask = 0x11111111
    movw    r12, #0x1111
    movt    r12, #0x1111

    BP_MUL32_BODY                        @ -> lr = res_lo, r0 = res_hi

    @ Return result in r0:r1 (lo:hi) per AAPCS uint64_t convention.
    mov     r1, r0                       @ result_hi
    mov     r0, lr                       @ result_lo

    add     sp, sp, #4
    pop     {r4-r11, pc}
    .size   bp_mul_32_cm4, .-bp_mul_32_cm4


@ ===================================================================
@ Inlined-leaf stack layout for bp_mul_64_cm4 / bp_muladd_64_cm4
@
@ After `push {r4-r11, lr}` + `sub sp, sp, #48`:
@
@   sp+0..3   : result_hi z-acc park (used inside BP_MUL32_BODY)
@   sp+4..7   : c0_lo (saved between phases 1 and 3)
@   sp+8..11  : c0_hi
@   sp+12..15 : c1_lo
@   sp+16..19 : c1_hi
@   sp+20..23 : d pointer (for the final write-back)
@   sp+24..27 : a0^a1 (precomputed, for phase 3)
@   sp+28..31 : a1 (for phase 2)
@   sp+32..35 : b0^b1 (precomputed, for phase 3)
@   sp+36..39 : b1 (for phase 2)
@   sp+40..47 : (padding to keep frame size a multiple of 8)
@
@ 48 + 36 (push) = 84 bytes total frame. SP stays 8-aligned across
@ the function body — no nested calls but we keep AAPCS hygiene in
@ case future maintainers add one.
@
@ Per-phase entry:
@   r0 = x, r1 = y, r12 = mask. Body clobbers r0-r11 + lr; on exit
@   lr = result_lo, r0 = result_hi.
@ ===================================================================

@ ===================================================================
@ void bp_mul_64_cm4(uint8_t *d, const uint8_t *a, const uint8_t *b);
@
@   d[0..15] = a[0..7] * b[0..7]   over GF(2)[X]   (ASSIGN)
@
@ Karatsuba structure (byte-identical to bp_muladd_64 in hawk_sign.c
@ except for the final XOR-into-d -> direct-store-into-d step):
@   a0 = a[0..3], a1 = a[4..7]
@   b0 = b[0..3], b1 = b[4..7]
@   c0 = bp_mul_32(a0, b0)         (64-bit)
@   c1 = bp_mul_32(a1, b1)         (64-bit)
@   c2 = bp_mul_32(a0^a1, b0^b1) ^ c0 ^ c1
@   d[0..3]   = c0_lo
@   d[4..7]   = c0_hi ^ c2_lo
@   d[8..11]  = c1_lo ^ c2_hi
@   d[12..15] = c1_hi
@
@ Three bp_mul_32 phases INLINED via BP_MUL32_BODY (no `bl` inside).
@ Inputs: r0 = d, r1 = a, r2 = b.
@ ===================================================================

    .global bp_mul_64_cm4
    .type   bp_mul_64_cm4, %function
    .thumb_func
    .align  2
bp_mul_64_cm4:
    push    {r4-r11, lr}                 @ 9 stores
    sub     sp, sp, #48

    @ Spill d, a1, b1, and pre-computed (a0^a1) / (b0^b1) to stack so
    @ phase bodies can clobber the entire register file (r0-r11 + lr)
    @ for the BP_MUL32 body. Phase 1 gets (a0, b0) directly in r0/r1
    @ (no mov needed) by loading them straight from memory into the
    @ argument slots; phase 2 reloads (a1, b1) from spill; phase 3
    @ reloads the pre-XOR'd (a0^a1, b0^b1) from spill (saves the 2
    @ eors that would otherwise sit between the ldrs).
    str     r0, [sp, #20]                @ d
    ldrd    r0, r4, [r1]                 @ r0=a0, r4=a1
    ldrd    r1, r5, [r2]                 @ r1=b0, r5=b1
    str     r4, [sp, #28]                @ stash a1
    str     r5, [sp, #36]                @ stash b1
    eor     r4, r0, r4                   @ a0^a1
    eor     r5, r1, r5                   @ b0^b1
    str     r4, [sp, #24]                @ stash a0^a1 (for phase 3)
    str     r5, [sp, #32]                @ stash b0^b1

    @ Load mask = 0x11111111 (preserved across all 3 phases by the macro)
    movw    r12, #0x1111
    movt    r12, #0x1111

    @ ===================================================================
    @ Phase 1: c0 = bp_mul_32(a0, b0) — r0=a0, r1=b0 already loaded
    @ ===================================================================
    BP_MUL32_BODY                        @ -> lr = c0_lo, r0 = c0_hi
    str     lr, [sp, #4]                 @ save c0_lo
    str     r0, [sp, #8]                 @ save c0_hi

    @ ===================================================================
    @ Phase 2: c1 = bp_mul_32(a1, b1)
    @ ===================================================================
    ldr     r0, [sp, #28]                @ x = a1
    ldr     r1, [sp, #36]                @ y = b1
    BP_MUL32_BODY                        @ -> lr = c1_lo, r0 = c1_hi
    str     lr, [sp, #12]                @ save c1_lo
    str     r0, [sp, #16]                @ save c1_hi

    @ ===================================================================
    @ Phase 3: c2_raw = bp_mul_32(a0^a1, b0^b1)
    @ ===================================================================
    ldr     r0, [sp, #24]                @ x = a0 ^ a1 (precomputed in preamble)
    ldr     r1, [sp, #32]                @ y = b0 ^ b1
    BP_MUL32_BODY                        @ -> lr = c2_raw_lo, r0 = c2_raw_hi

    @ ===================================================================
    @ Fold c2 = c2_raw ^ c0 ^ c1
    @ ===================================================================
    @ c2_lo = c2_raw_lo (lr) ^ c0_lo ^ c1_lo, c2_hi = r0 ^ c0_hi ^ c1_hi
    ldrd    r2, r3, [sp, #4]             @ r2 = c0_lo, r3 = c0_hi
    eor     lr, lr, r2                   @ lr ^= c0_lo
    eor     r0, r0, r3                   @ r0 ^= c0_hi
    ldrd    r2, r3, [sp, #12]            @ r2 = c1_lo, r3 = c1_hi
    eor     lr, lr, r2                   @ lr = c2_lo
    eor     r0, r0, r3                   @ r0 = c2_hi

    @ ===================================================================
    @ Write d (ASSIGN, no read-modify-write):
    @   d[0..3]   = c0_lo
    @   d[4..7]   = c0_hi ^ c2_lo
    @   d[8..11]  = c1_lo ^ c2_hi
    @   d[12..15] = c1_hi
    @ ===================================================================
    ldr     r3, [sp, #20]                @ d ptr
    ldrd    r1, r2, [sp, #4]             @ r1 = c0_lo, r2 = c0_hi
    eor     r2, r2, lr                   @ c0_hi ^ c2_lo
    strd    r1, r2, [r3, #0]             @ d[0..7]
    ldrd    r1, r2, [sp, #12]            @ r1 = c1_lo, r2 = c1_hi
    eor     r1, r1, r0                   @ c1_lo ^ c2_hi
    strd    r1, r2, [r3, #8]             @ d[8..15]

    add     sp, sp, #48
    pop     {r4-r11, pc}
    .size   bp_mul_64_cm4, .-bp_mul_64_cm4


@ ===================================================================
@ void bp_muladd_64_cm4(uint8_t *d, const uint8_t *a, const uint8_t *b);
@
@   d[0..15] ^= a[0..7] * b[0..7]   over GF(2)[X]   (XOR-into-d)
@
@ Same as bp_mul_64_cm4, but final step reads d, XORs the product
@ in, and writes back. Three bp_mul_32 phases INLINED via
@ BP_MUL32_BODY.
@
@ Inputs: r0 = d, r1 = a, r2 = b.
@ ===================================================================

    .global bp_muladd_64_cm4
    .type   bp_muladd_64_cm4, %function
    .thumb_func
    .align  2
bp_muladd_64_cm4:
    push    {r4-r11, lr}
    sub     sp, sp, #48

    @ Same preamble as bp_mul_64_cm4: pre-compute (a0^a1) / (b0^b1)
    @ so phase 3 just loads them directly into r0, r1 (saves 4 inst
    @ vs computing the eors at phase 3 entry).
    str     r0, [sp, #20]                @ d
    ldrd    r0, r4, [r1]                 @ r0=a0, r4=a1
    ldrd    r1, r5, [r2]                 @ r1=b0, r5=b1
    str     r4, [sp, #28]                @ a1
    str     r5, [sp, #36]                @ b1
    eor     r4, r0, r4                   @ a0^a1
    eor     r5, r1, r5                   @ b0^b1
    str     r4, [sp, #24]                @ a0^a1
    str     r5, [sp, #32]                @ b0^b1

    movw    r12, #0x1111
    movt    r12, #0x1111

    @ ===================================================================
    @ Phase 1: c0 = bp_mul_32(a0, b0) — r0=a0, r1=b0 already loaded
    @ ===================================================================
    BP_MUL32_BODY                        @ -> lr = c0_lo, r0 = c0_hi
    str     lr, [sp, #4]                 @ c0_lo
    str     r0, [sp, #8]                 @ c0_hi

    @ ===================================================================
    @ Phase 2: c1 = bp_mul_32(a1, b1)
    @ ===================================================================
    ldr     r0, [sp, #28]
    ldr     r1, [sp, #36]
    BP_MUL32_BODY                        @ -> lr = c1_lo, r0 = c1_hi
    str     lr, [sp, #12]                @ c1_lo
    str     r0, [sp, #16]                @ c1_hi

    @ ===================================================================
    @ Phase 3: c2_raw = bp_mul_32(a0^a1, b0^b1)
    @ ===================================================================
    ldr     r0, [sp, #24]                @ x = a0^a1 (precomputed)
    ldr     r1, [sp, #32]                @ y = b0^b1
    BP_MUL32_BODY                        @ -> lr = c2_raw_lo, r0 = c2_raw_hi

    @ ===================================================================
    @ Fold c2 = c2_raw ^ c0 ^ c1
    @ ===================================================================
    ldrd    r2, r3, [sp, #4]             @ r2 = c0_lo, r3 = c0_hi
    eor     lr, lr, r2
    eor     r0, r0, r3
    ldrd    r2, r3, [sp, #12]            @ r2 = c1_lo, r3 = c1_hi
    eor     lr, lr, r2                   @ lr = c2_lo
    eor     r0, r0, r3                   @ r0 = c2_hi

    @ ===================================================================
    @ Update d (XOR-into):
    @   d[0..3]   ^= c0_lo
    @   d[4..7]   ^= c0_hi ^ c2_lo
    @   d[8..11]  ^= c1_lo ^ c2_hi
    @   d[12..15] ^= c1_hi
    @ ===================================================================
    ldr     r3, [sp, #20]                @ d ptr
    ldrd    r1, r2, [r3, #0]             @ r1 = d[0..3], r2 = d[4..7]
    ldrd    r4, r5, [sp, #4]             @ r4 = c0_lo, r5 = c0_hi
    eor     r1, r1, r4                   @ d[0..3] ^= c0_lo
    eor     r2, r2, r5                   @ d[4..7] ^= c0_hi
    eor     r2, r2, lr                   @ d[4..7] ^= c2_lo
    strd    r1, r2, [r3, #0]

    ldrd    r1, r2, [r3, #8]             @ r1 = d[8..11], r2 = d[12..15]
    ldrd    r4, r5, [sp, #12]            @ r4 = c1_lo, r5 = c1_hi
    eor     r1, r1, r4                   @ d[8..11] ^= c1_lo
    eor     r1, r1, r0                   @ d[8..11] ^= c2_hi
    eor     r2, r2, r5                   @ d[12..15] ^= c1_hi
    strd    r1, r2, [r3, #8]

    add     sp, sp, #48
    pop     {r4-r11, pc}
    .size   bp_muladd_64_cm4, .-bp_muladd_64_cm4

#endif  /* __ARM_FEATURE_DSP */
