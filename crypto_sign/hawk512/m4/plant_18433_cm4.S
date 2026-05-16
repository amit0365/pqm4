/*
 * plant_18433_cm4.S — Cortex-M4 (ARMv7E-M) implementation of HAWK's
 * Q=18433 Plantard/Montgomery NTT entry points.
 *
 * Provides:
 *   mq18433_NTT_plant(unsigned logn, uint16_t *a)        — TODO (not yet impl)
 *   mq18433_iNTT_plant(unsigned logn, uint16_t *a)       — TODO (not yet impl)
 *   mq18433_montymul_plant(uint32_t x, uint32_t y)       — done, cross-check pending
 *
 * SPEC ADHERENCE
 * --------------
 * Every entry point produces output that is BYTE-IDENTICAL to the
 * static-inline references mq18433_NTT / mq18433_iNTT / mq18433_montymul
 * in modq.h. Drift breaks signature byte-equality with the HAWK Round-2
 * KAT vectors and is a defect, not a tradeoff.
 *
 * KERNEL
 * ------
 * The locked-down Montgomery reducer (validated on host by
 * tests/test_plantard_calibrate.c, combination `v6+encId`) is the
 * literal inline of Zq(montyred):
 *
 *     t = c * Q0I                   mod 2^32     (32x32 -> low 32)
 *     h = t >> 16                                (logical, unsigned)
 *     acc = h * Q + (1 << 16)
 *     result = acc >> 16            in [1..Q]
 *
 * Encoded in 4 instructions:
 *     mul   t, c, q0i
 *     lsr   h, t, #16
 *     mla   acc, h, q, qa            ; qa = 1 << 16, prepared once
 *     lsr   result, acc, #16
 *
 * CANONICAL FORM (Path B per PLANTARD_NOTES.md)
 * ---------------------------------------------
 * HAWK keeps coefficients in [1..Q] with 0 represented as Q. The
 * Plantard reducer above ALREADY emits canonical [1..Q]. The butterfly
 * add/sub must preserve that form:
 *
 *     add(x1, x2'):  s = x1 + x2'   (in [2..2Q]).  If s > Q, s -= Q.
 *     sub(x1, x2'):  d = x1 - x2'.  If d <= 0, d += Q.  Maps zero to Q.
 *
 * STATUS (this file)
 * ------------------
 *   - mq18433_montymul_plant is implemented and pending cross-check
 *     against modq.h's Zq(montymul) under QEMU semihosting.
 *   - mq18433_NTT_plant / mq18433_iNTT_plant are NOT YET WRITTEN. The
 *     scalar v1 of NTT in this file's predecessor had a register
 *     allocation bug (clobbered GM_base mid-loop) that was caught
 *     before commit. Re-implementation is gated on having the cross-
 *     check harness running so we can iterate per-layer.
 *
 *     Until they ship, plant_18433.c (C reference) is the source of
 *     truth for the NTT path even on M4 — its function bodies are
 *     guarded by !__ARM_FEATURE_DSP, but until this file provides
 *     mq18433_NTT_plant the guard must invert (or hawk_sign.c must
 *     compile with HAWK_PLANT_NTT undefined on M4).
 */

#if defined(__ARM_FEATURE_DSP) && __ARM_FEATURE_DSP

    .syntax unified
    .cpu cortex-m4
    .thumb

    .text

@ -------------------------------------------------------------------
@ uint32_t mq18433_montymul_plant(uint32_t x, uint32_t y);
@
@ Single Zq(montymul) — used in hawk_sign.c's pointwise multiplies.
@ Leaf function; no callee-saves.
@
@ Cycles (Cortex-M4, no wait states): 8 incl. constant loads + return.
@   movw r2, #imm16        (1)
@   movt r2, #imm16        (1)
@   movw r3, #18433        (1)
@   mov.w r12, #0x10000    (1)
@   mul r0, r0, r1         (1)
@   mul r0, r0, r2         (1)
@   lsr r0, r0, #16        (1)
@   mla r0, r0, r3, r12    (1-2 depending on operand forwarding)
@   lsr r0, r0, #16        (1)
@   bx lr                  (1-3, pipeline)
@ -------------------------------------------------------------------
    .global mq18433_montymul_plant
    .type   mq18433_montymul_plant, %function
    .thumb_func
    .align  2
mq18433_montymul_plant:
    @ r0 = x, r1 = y; return result in r0.
    @ Q0I = 3955247103 = 0xEBC047FF  (NOT 0xEBC97FFF — earlier HANDOFF
    @ had a typo; the constant was caught wrong in v0 of this asm.
    @ Verified via the literal pool the C compiler picks for the
    @ reference `* Q0I` in test_montymul.c.)
    movw    r2, #0x47FF                @ r2 = lower16(Q0I)
    movt    r2, #0xEBC0                @ r2 = Q0I = 3955247103
    movw    r3, #18433                 @ r3 = Q
    mov.w   r12, #0x10000              @ r12 = QA = 1 << 16  (Thumb-2 modified imm)
    mul     r0, r0, r1                 @ r0 = x * y                   (c, 30-bit max)
    mul     r0, r0, r2                 @ r0 = c * Q0I mod 2^32        (t)
    lsr     r0, r0, #16                @ r0 = h = top16(t)            ([0..65535])
    mla     r0, r0, r3, r12            @ r0 = h*Q + QA                (acc)
    lsr     r0, r0, #16                @ r0 = result in [1..Q]
    bx      lr
    .size   mq18433_montymul_plant, .-mq18433_montymul_plant


@ -------------------------------------------------------------------
@ void mq18433_NTT_plant(unsigned logn, uint16_t *a);
@
@ Cooley-Tukey forward NTT, scalar (v1). Mirrors Zq(NTT) in modq.h
@ exactly: same loop structure, same per-coefficient kernel + canonical
@ [1..Q] add/sub.
@
@ Register map (callee-saved r4-r11 + lr; sub sp #8 for u counter):
@   r0,r1   x1,x2 (inner loop scratch)
@   r2      s    (twiddle, loaded *p_GM++ at each u-iter)
@   r3      scratch (canonical-add result)
@   r4      Q0I = 0xEBC047FF
@   r5      Q   = 18433
@   r6      QA  = 0x10000
@   r7      a_base (function argument)
@   r8      p_GM (advances through &mq18433_GM[1..N-1] across all layers)
@   r9      T_bytes (block stride in bytes; halves each layer)
@   r10     m  (block count; doubles each layer)
@   r11     p_k1 (inner pointer, post-incremented by 2 per butterfly)
@   r12     p_k2 (inner pointer, post-incremented by 2 per butterfly)
@   r14     v_remaining (inner counter, decrements to 0)
@   [sp+0]  u_remaining (across u-iter)
@
@ Loop bound: T_bytes starts at 2*N (= 2<<logn), halves each layer.
@ Last layer has T_bytes=4 (single-butterfly blocks). After the post-
@ layer halve, T_bytes=2 ⇒ done.
@ -------------------------------------------------------------------
    .global mq18433_NTT_plant
    .type   mq18433_NTT_plant, %function
    .thumb_func
    .align  2
mq18433_NTT_plant:
    push    {r4-r11, lr}
    sub     sp, sp, #8                 @ [sp+0] = u_remaining; [sp+4] = pad

    @ Load constants
    movw    r4, #0x47FF
    movt    r4, #0xEBC0                @ r4 = Q0I
    movw    r5, #18433                 @ r5 = Q
    mov.w   r6, #0x10000               @ r6 = QA

    mov     r7, r1                     @ r7 = a_base

    movw    r8, #:lower16:mq18433_GM
    movt    r8, #:upper16:mq18433_GM
    adds    r8, r8, #2                 @ r8 = &mq18433_GM[1] (skip GM[0])

    @ T_bytes = 2 << logn
    movs    r9, #2
    lsls    r9, r9, r0                 @ r9 = T_bytes

    @ m = 1
    movs    r10, #1

ntt_layer:
    cmp     r9, #4
    blt     ntt_done                   @ T_bytes < 4 means we're past the last layer

    @ p_k1 = a_base; HT_bytes = T_bytes >> 1; p_k2 = p_k1 + HT_bytes
    mov     r11, r7
    lsr     r0, r9, #1                 @ r0 = HT_bytes
    add     r12, r11, r0

    @ u_remaining = m
    str     r10, [sp, #0]

ntt_uloop:
    ldrh    r2, [r8], #2               @ r2 = s = *p_GM++
    lsr     r14, r9, #2                @ r14 = v_remaining = HT_halfwords = T_bytes/4

ntt_vloop:
    @ Butterfly: x1 = a[k1]; x2 = a[k2]; x2' = montymul(x2, s)
    @            a[k1] = add(x1, x2'); a[k2] = sub(x1, x2')
    ldrh    r0, [r11]                  @ r0 = x1 = a[k1]
    ldrh    r1, [r12]                  @ r1 = a[k2]

    @ Inner Plantard kernel (= Zq(montyred)(a[k2] * s))
    mul     r1, r1, r2                 @ r1 = c = a[k2] * s
    mul     r1, r1, r4                 @ r1 = t = c * Q0I (low 32)
    lsr     r1, r1, #16                @ r1 = h
    mla     r1, r1, r5, r6             @ r1 = h*Q + QA
    lsr     r1, r1, #16                @ r1 = x2' (in [1..Q])

    @ Canonical add: r3 = (x1 + x2'); if > Q, sub Q
    add     r3, r0, r1
    cmp     r3, r5
    it      hi
    subhi   r3, r3, r5

    @ Canonical sub: r0 = (x1 - x2') (signed); if <= 0, add Q
    subs    r0, r0, r1
    it      le
    addle   r0, r0, r5

    strh    r3, [r11], #2              @ a[k1] = add result
    strh    r0, [r12], #2              @ a[k2] = sub result

    subs    r14, r14, #1
    bne     ntt_vloop

    @ End of v-loop: advance pointers to next block. Both pointers have
    @ moved by HT_bytes from their starts; next block starts T_bytes
    @ further along, so advance by HT_bytes more (= T_bytes - HT_bytes).
    lsr     r0, r9, #1                 @ r0 = HT_bytes
    add     r11, r11, r0
    add     r12, r12, r0

    @ u_remaining--; loop if non-zero
    ldr     r0, [sp, #0]
    subs    r0, r0, #1
    str     r0, [sp, #0]
    bne     ntt_uloop

    @ Next layer: T_bytes /= 2; m *= 2
    lsr     r9, r9, #1
    lsl     r10, r10, #1
    b       ntt_layer

ntt_done:
    add     sp, sp, #8
    pop     {r4-r11, lr}
    bx      lr
    .size   mq18433_NTT_plant, .-mq18433_NTT_plant


@ -------------------------------------------------------------------
@ void mq18433_iNTT_plant(unsigned logn, uint16_t *a);
@
@ Gentleman-Sande inverse NTT, scalar. Mirrors Zq(iNTT) in modq.h.
@ Differences vs forward:
@   - Uses iGM table, indexed iGM[hm..2hm-1] per layer (hm halves)
@   - t_bytes doubles each layer (starts at 2, ends at N)
@   - Butterfly:  a[k1] = half(add(x1, x2)); a[k2] = montymul(s, sub(x1, x2))
@
@ Register map: identical to mq18433_NTT_plant except r8 holds p_iGM
@ (re-derived from iGM_base + hm*2 at each layer entry, since the per-
@ layer iGM windows go BACKWARDS through the table — top half first).
@ iGM_base is stashed on stack[+4].
@
@ The "half" operation: half(x) for x in [1..Q] with Q odd:
@     even x → x/2
@     odd  x → (x + Q) / 2   (x+Q is even)
@ Encoded as 4 instructions per call:
@     and  r_lsb, r_sum, #1
@     mul  r_lsb, r_lsb, r5      ; (lsb)*Q  → 0 or Q
@     add  r_sum, r_sum, r_lsb
@     lsr  r_half, r_sum, #1
@ -------------------------------------------------------------------
    .global mq18433_iNTT_plant
    .type   mq18433_iNTT_plant, %function
    .thumb_func
    .align  2
mq18433_iNTT_plant:
    push    {r4-r11, lr}
    sub     sp, sp, #8                 @ [sp+0] = u_remaining; [sp+4] = iGM_base

    @ Load constants
    movw    r4, #0x47FF
    movt    r4, #0xEBC0                @ r4 = Q0I
    movw    r5, #18433                 @ r5 = Q
    mov.w   r6, #0x10000               @ r6 = QA

    mov     r7, r1                     @ r7 = a_base

    @ hm = 1 << (logn - 1)  (consume r0 before clobbering it for iGM_base)
    @ Special case logn = 0: hm should be 0 (no layers). Handle by
    @ checking r0 first. CBZ has narrow branch range and intti_done
    @ is far away, so use cmp+beq.w instead.
    cmp     r0, #0
    beq.w   intti_done
    subs    r0, r0, #1
    movs    r10, #1
    lsl     r10, r10, r0               @ r10 = hm = 1 << (logn - 1)

    @ Stash iGM_base on stack so we can re-derive p_iGM each layer.
    movw    r0, #:lower16:mq18433_iGM
    movt    r0, #:upper16:mq18433_iGM
    str     r0, [sp, #4]

    @ t_bytes = 2 (one halfword), doubles each layer
    movs    r9, #2

intti_layer:
    @ Stop when hm == 0 (CBZ only allows r0-r7; use cmp+beq)
    cmp     r10, #0
    beq     intti_done

    @ p_iGM = iGM_base + hm*2
    ldr     r3, [sp, #4]
    add     r8, r3, r10, lsl #1

    @ p_k1 = a_base; p_k2 = p_k1 + t_bytes
    mov     r11, r7
    add     r12, r11, r9

    @ u_remaining = hm
    str     r10, [sp, #0]

intti_uloop:
    ldrh    r2, [r8], #2               @ r2 = s = *p_iGM++
    @ v_remaining = t_halfwords = t_bytes / 2
    lsr     r14, r9, #1

intti_vloop:
    ldrh    r0, [r11]                  @ r0 = x1
    ldrh    r1, [r12]                  @ r1 = x2

    @ d = canonical sub(x1, x2): r3 = x1 - x2; if <= 0, +Q
    subs    r3, r0, r1
    it      le
    addle   r3, r3, r5

    @ a[k2] = montymul(s, d): apply kernel to (r2 * r3)
    mul     r3, r3, r2
    mul     r3, r3, r4
    lsr     r3, r3, #16
    mla     r3, r3, r5, r6
    lsr     r3, r3, #16
    strh    r3, [r12], #2

    @ s' = canonical add(x1, x2): r3 = x1 + x2; if > Q, -Q
    add     r3, r0, r1
    cmp     r3, r5
    it      hi
    subhi   r3, r3, r5

    @ a[k1] = half(s'): r3 = (r3 + Q*(r3&1)) >> 1
    and     r0, r3, #1                 @ r0 = lsb (x1 dead at this point)
    mul     r0, r0, r5                 @ r0 = 0 or Q
    add     r3, r3, r0
    lsr     r3, r3, #1
    strh    r3, [r11], #2

    subs    r14, r14, #1
    bne     intti_vloop

    @ Advance pointers to next block: each += t_bytes
    add     r11, r11, r9
    add     r12, r12, r9

    @ u_remaining--; loop if non-zero
    ldr     r0, [sp, #0]
    subs    r0, r0, #1
    str     r0, [sp, #0]
    bne     intti_uloop

    @ Next layer: t_bytes *= 2; hm /= 2
    lsl     r9, r9, #1
    lsr     r10, r10, #1
    b       intti_layer

intti_done:
    add     sp, sp, #8
    pop     {r4-r11, lr}
    bx      lr
    .size   mq18433_iNTT_plant, .-mq18433_iNTT_plant

#endif /* __ARM_FEATURE_DSP */
