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
@ Cooley-Tukey forward NTT. Path B canonical-form throughout.
@
@ Layer structure split:
@   - Layers with ht >= 2 (HT_bytes >= 4): PACKED-PAIR inner loop
@     processes 2 butterflies (4 cells) per iter via ldr.w/str.w +
@     uadd16/usub16/sel.
@   - Last layer (ht = 1, HT_bytes = 2): scalar fallback. Can't pack
@     within a block (only one butterfly per block); cross-block pack
@     is a v3 optimization.
@
@ Register map (callee-saved r4-r11 + lr; sub sp #16 for spills):
@   r0,r1   x1,x2 packed (inner) / scratch elsewhere
@   r2      s    (twiddle, loaded *p_GM++ at each u-iter)
@   r3,r8,r12  inner scratch (3 regs)
@   r4      Q0I = 0xEBC047FF
@   r5      Q   = 18433
@   r6      QA  = 0x10000
@   r7      Q_packed   = (Q   << 16) | Q
@   r9      HT_bytes   (per-layer; halves each layer; loop bound)
@   r10     Qp1_packed = ((Q+1) << 16) | (Q+1)
@   r11     p_k1
@   r14     v_remaining
@   [sp+0]  u_remaining
@   [sp+4]  a_base
@   [sp+8]  m_current
@   [sp+12] p_GM (spilled here because r8 is needed as inner scratch)
@
@ Loop bound: HT_bytes starts at N bytes (= 1 << logn), halves each
@ layer. Last layer has HT_bytes=2. After post-layer halve, HT_bytes=1
@ ⇒ done.
@ -------------------------------------------------------------------
    .global mq18433_NTT_plant
    .type   mq18433_NTT_plant, %function
    .thumb_func
    .align  2
mq18433_NTT_plant:
    push    {r4-r11, lr}
    sub     sp, sp, #16

    @ Load scalar constants
    movw    r4, #0x47FF
    movt    r4, #0xEBC0                @ r4 = Q0I
    movw    r5, #18433                 @ r5 = Q
    mov.w   r6, #0x10000               @ r6 = QA

    @ Packed constants
    pkhbt   r7, r5, r5, lsl #16        @ r7 = Q_packed = (Q << 16) | Q
    adds    r3, r5, #1                 @ r3 = Q + 1
    pkhbt   r10, r3, r3, lsl #16       @ r10 = Qp1_packed

    @ Spill a_base
    str     r1, [sp, #4]

    @ Initialize p_GM = &mq18433_GM[1] (skip GM[0]; the table is read in
    @ order: GM[1] for the (m=1) layer, GM[2..3] for (m=2), etc.).
    movw    r3, #:lower16:mq18433_GM
    movt    r3, #:upper16:mq18433_GM
    adds    r3, r3, #2
    str     r3, [sp, #12]              @ stash p_GM on stack

    @ HT_bytes_initial = 1 << logn (N bytes — i.e., (N halfwords)*1
    @ given ht = N/2 halfwords ⇒ ht_bytes = N bytes)
    movs    r9, #1
    lsls    r9, r9, r0                 @ r9 = HT_bytes_initial

    @ m_initial = 1
    movs    r3, #1
    str     r3, [sp, #8]               @ stash m on stack

ntt_layer:
    cmp     r9, #2
    blt     ntt_done                   @ HT_bytes < 2 ⇒ done

    @ Layer setup: p_k1 = a_base, u_remaining = m
    ldr     r11, [sp, #4]              @ p_k1 = a_base
    ldr     r3, [sp, #8]
    str     r3, [sp, #0]               @ u_remaining = m

    @ Branch: HT_bytes >= 4 ⇒ packed; HT_bytes == 2 ⇒ scalar
    cmp     r9, #4
    blt     ntt_uloop_scalar

@ ---------- PACKED u-loop (ht >= 2) ----------
ntt_uloop_packed:
    @ Reload p_GM, fetch s, save advanced p_GM
    ldr     r8, [sp, #12]
    ldrh    r2, [r8], #2               @ r2 = s = *p_GM++
    str     r8, [sp, #12]

    @ v_remaining (packed) = ht/2 = HT_bytes/4
    lsr     r14, r9, #2

ntt_vloop_packed:
    @ Load packed x1 and x2 (2 cells each)
    ldr.w   r0, [r11]                  @ r0 = x1 = (x1_hi << 16) | x1_lo
    ldr.w   r1, [r11, r9]              @ r1 = x2 (offset HT_bytes from p_k1)

    @ Plantard kernel applied to each half of x2 separately.
    @ Cannot pack because c_i = x2_i * s is up to 30 bits per cell.
    uxth    r3, r1                     @ r3 = x2_lo
    lsr     r8, r1, #16                @ r8 = x2_hi
    mul     r3, r3, r2                 @ r3 = c_lo  = x2_lo * s
    mul     r8, r8, r2                 @ r8 = c_hi
    mul     r3, r3, r4                 @ r3 = c_lo * Q0I mod 2^32
    mul     r8, r8, r4                 @ r8 = c_hi * Q0I
    lsr     r3, r3, #16                @ r3 = h_lo
    lsr     r8, r8, #16                @ r8 = h_hi
    mla     r3, r3, r5, r6             @ r3 = h_lo*Q + QA
    mla     r8, r8, r5, r6             @ r8 = h_hi*Q + QA
    lsr     r3, r3, #16                @ r3 = reduced_lo  in [1..Q]
    lsr     r8, r8, #16                @ r8 = reduced_hi  in [1..Q]
    pkhbt   r1, r3, r8, lsl #16        @ r1 = (reduced_hi << 16) | reduced_lo

    @ Canonical add: per half, if (x1+m) > Q subtract Q.
    @   sum  = uadd16(x1, m)                 ; in [2..2Q] per half, no overflow
    @   chk  = usub16(sum, Q+1)              ; GE_h set if sum_h > Q
    @   sub_ = usub16(sum, Q)                ; sum - Q per half
    @   res  = sel(sub_, sum)                ; GE → sub_; else sum
    uadd16  r3, r0, r1                 @ r3 = sum
    usub16  r8, r3, r7                 @ r8 = sum - Q
    usub16  r12, r3, r10               @ r12 = sum - (Q+1), GE flags
    sel     r8, r8, r3                 @ r8 = canon-add result
    str.w   r8, [r11]                  @ a[k1..k1+1] = canon-add result

    @ Canonical sub: per half, x1 - m mod Q in [1..Q] with 0 ⇒ Q.
    @ Sequence (using only USUB16's "no-borrow" flag is hard because
    @ the following UADD16 clobbers GE; instead we use UADD16's
    @ overflow GE flag, which is set precisely when the USUB16 had a
    @ borrow — i.e., m < x1):
    @
    @   y      = usub16(m, x1)             ; m - x1 (uint16 wrap)
    @   y_plus = uadd16(y,  Q_packed)      ; y + Q. Overflows iff y wrapped,
    @                                        which is iff x1 > m. GE set on overflow.
    @   yp     = sel(y_plus, y)            ; GE set (x1>m)  → y_plus (wrap-corrected)
    @                                        GE clear (m>=x1)→ y       (already in [0..Q-1])
    @   res    = ssub16(Q_packed, yp)      ; per-half Q - yp  ⇒ [1..Q] (Q for yp=0)
    usub16  r3, r1, r0
    uadd16  r8, r3, r7
    sel     r3, r8, r3
    ssub16  r3, r7, r3
    str.w   r3, [r11, r9]              @ a[k2..k2+1] = canon-sub result

    @ Advance p_k1 by 4 bytes (2 halfwords = next packed pair)
    adds    r11, r11, #4
    subs    r14, r14, #1
    bne     ntt_vloop_packed

    @ End of packed v-loop: r11 advanced by HT_bytes from block start
    @ (HT_bytes/4 iters × 4 bytes). Next block starts at +HT_bytes
    @ further, so add r9.
    add     r11, r11, r9

    @ u_remaining--
    ldr     r0, [sp, #0]
    subs    r0, r0, #1
    str     r0, [sp, #0]
    bne     ntt_uloop_packed
    b       ntt_layer_done

@ ---------- SCALAR u-loop (ht == 1, last layer) ----------
@ One butterfly per block, k2 = k1 + 2 bytes.
ntt_uloop_scalar:
    ldr     r8, [sp, #12]
    ldrh    r2, [r8], #2
    str     r8, [sp, #12]

ntt_v_scalar_one:
    @ Single scalar butterfly at p_k1=k1, k2=p_k1+2.
    ldrh    r0, [r11]                  @ x1
    ldrh    r1, [r11, #2]              @ x2

    mul     r1, r1, r2
    mul     r1, r1, r4
    lsr     r1, r1, #16
    mla     r1, r1, r5, r6
    lsr     r1, r1, #16                @ r1 = montymul'd x2

    add     r3, r0, r1
    cmp     r3, r5
    it      hi
    subhi   r3, r3, r5

    subs    r0, r0, r1
    it      le
    addle   r0, r0, r5

    strh    r3, [r11], #2              @ a[k1]
    strh    r0, [r11], #2              @ a[k2]; r11 now at next block start

    @ u_remaining--
    ldr     r0, [sp, #0]
    subs    r0, r0, #1
    str     r0, [sp, #0]
    bne     ntt_uloop_scalar

ntt_layer_done:
    @ HT_bytes /= 2; m *= 2
    lsr     r9, r9, #1
    ldr     r0, [sp, #8]
    lsl     r0, r0, #1
    str     r0, [sp, #8]
    b       ntt_layer

ntt_done:
    add     sp, sp, #16
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
@ -------------------------------------------------------------------
@ void mq18433_iNTT_plant(unsigned logn, uint16_t *a);
@
@ Gentleman-Sande inverse NTT.
@   - Layer 0 (t=1): scalar path; one butterfly per block, k2-k1 = 2 bytes.
@   - Layers 1..logn-1 (t>=2): packed path; r9 = 2*t bytes is k2-k1 stride.
@
@ Register map (callee-saved r4-r11 + lr; sub sp #16 for spills):
@   r0,r1   x1,x2 packed (inner)
@   r2      s = *p_iGM (per u-iter)
@   r3,r8,r12  inner scratch
@   r4      Q0I
@   r5      Q
@   r6      QA
@   r7      Q_packed
@   r9      t_bytes_doubled = 2*t (k2-k1 distance in bytes); doubles each layer
@   r10     Qp1_packed
@   r11     p_k1
@   r14     v_remaining
@   [sp+0]  u_remaining (= hm at layer start, decrements)
@   [sp+4]  a_base
@   [sp+8]  hm_current (per-layer)
@   [sp+12] p_iGM (spilled to free r8 as inner scratch)
@ -------------------------------------------------------------------
mq18433_iNTT_plant:
    push    {r4-r11, lr}
    sub     sp, sp, #16

    @ logn = 0: nothing to do.
    cmp     r0, #0
    beq.w   intti_done

    @ Scalar constants
    movw    r4, #0x47FF
    movt    r4, #0xEBC0                @ Q0I
    movw    r5, #18433                 @ Q
    mov.w   r6, #0x10000               @ QA

    @ Packed constants
    pkhbt   r7, r5, r5, lsl #16        @ Q_packed
    adds    r3, r5, #1
    pkhbt   r10, r3, r3, lsl #16       @ Qp1_packed

    @ a_base on stack
    str     r1, [sp, #4]

    @ hm = 1 << (logn - 1)
    subs    r0, r0, #1
    movs    r3, #1
    lsl     r3, r3, r0                 @ r3 = hm
    str     r3, [sp, #8]               @ stash hm

    @ t_bytes_doubled = 2 (= 2 * 1 halfword initial t)
    movs    r9, #2

intti_layer:
    ldr     r3, [sp, #8]               @ r3 = hm
    cmp     r3, #0
    beq.w   intti_done

    @ p_iGM = &iGM[hm] = iGM_base + hm*2
    movw    r0, #:lower16:mq18433_iGM
    movt    r0, #:upper16:mq18433_iGM
    add     r0, r0, r3, lsl #1
    str     r0, [sp, #12]              @ stash p_iGM

    @ p_k1 = a_base; u_remaining = hm
    ldr     r11, [sp, #4]
    str     r3, [sp, #0]

    @ Pick path: t_bytes_doubled >= 4 ⇒ packed (t>=2); == 2 ⇒ scalar (t=1)
    cmp     r9, #4
    blt     intti_uloop_scalar

@ ---------- PACKED u-loop (t >= 2) ----------
intti_uloop_packed:
    ldr     r8, [sp, #12]
    ldrh    r2, [r8], #2               @ s = *p_iGM++
    str     r8, [sp, #12]

    @ v_remaining (packed) = t/2 = r9/4
    lsr     r14, r9, #2

intti_vloop_packed:
    ldr.w   r0, [r11]                  @ x1 packed
    ldr.w   r1, [r11, r9]              @ x2 packed

    @ Canon-sub: r3 = canonical(x1 - x2) packed in [1..Q]
    usub16  r3, r1, r0
    uadd16  r8, r3, r7
    sel     r3, r8, r3
    ssub16  r3, r7, r3                 @ r3 = canon-sub result

    @ Apply Plantard kernel to (s * canon-sub) per half → a[k2]
    uxth    r8, r3
    lsr     r12, r3, #16
    mul     r8, r8, r2                 @ c_lo = s * sub_lo
    mul     r12, r12, r2               @ c_hi
    mul     r8, r8, r4                 @ c_lo * Q0I
    mul     r12, r12, r4
    lsr     r8, r8, #16                @ h_lo
    lsr     r12, r12, #16
    mla     r8, r8, r5, r6             @ h_lo*Q + QA
    mla     r12, r12, r5, r6
    lsr     r8, r8, #16                @ reduced_lo
    lsr     r12, r12, #16
    pkhbt   r3, r8, r12, lsl #16       @ packed reduced
    str.w   r3, [r11, r9]              @ a[k2..k2+1] = montymul(s, canon-sub)

    @ Canon-add: r3 = canonical(x1 + x2) packed in [1..Q]
    uadd16  r3, r0, r1
    usub16  r8, r3, r7
    usub16  r12, r3, r10
    sel     r3, r8, r3                 @ r3 = canon-add result

    @ Apply half() per half: r3 = (sum + Q*lsb) >> 1.
    @ Build r_oneone = 0x00010001 from r6 (QA = 0x10000): r8 = r6 + 1.
    @ Then lsb-mask = r3 & 0x00010001 (packed); correction = lsb * Q (packed
    @ — works because lsb_lo and lsb_hi are at bits 0 and 16 respectively).
    @ After correction, both halves are even ⇒ lsr.w #1 safely halves both.
    add     r8, r6, #1                 @ r8 = 0x00010001
    and     r12, r3, r8                @ r12 = packed lsb mask
    mul     r12, r12, r5               @ r12 = (0 or Q) per half
    uadd16  r3, r3, r12                @ r3 = sum + Q*lsb (both halves even)
    lsr     r3, r3, #1                 @ r3 = half(sum) packed
    str.w   r3, [r11]                  @ a[k1..k1+1] = half(canon-add)

    adds    r11, r11, #4
    subs    r14, r14, #1
    bne     intti_vloop_packed

    @ End of v-loop: r11 advanced by r9/2 bytes from block start (t/2
    @ packed iters × 4 bytes = 2*t bytes = r9). For iNTT, block size
    @ is 2*t halfwords = 2*r9 bytes, so r11 needs to advance by r9 more
    @ to reach next block start.
    add     r11, r11, r9

    ldr     r0, [sp, #0]
    subs    r0, r0, #1
    str     r0, [sp, #0]
    bne     intti_uloop_packed
    b       intti_layer_done

@ ---------- SCALAR u-loop (t == 1, layer 0) ----------
intti_uloop_scalar:
    ldr     r8, [sp, #12]
    ldrh    r2, [r8], #2
    str     r8, [sp, #12]

    ldrh    r0, [r11]                  @ x1
    ldrh    r1, [r11, #2]              @ x2 (k2 = k1+1 halfword)

    @ Canon-sub: r3 = canonical(x1 - x2)
    subs    r3, r0, r1
    it      le
    addle   r3, r3, r5

    @ montymul(s, canon-sub) → a[k2]
    mul     r3, r3, r2
    mul     r3, r3, r4
    lsr     r3, r3, #16
    mla     r3, r3, r5, r6
    lsr     r3, r3, #16

    @ Canon-add: r0 = canonical(x1 + x2). x1 unused now; reuse r0.
    add     r0, r0, r1
    cmp     r0, r5
    it      hi
    subhi   r0, r0, r5

    @ Half: r0 = (sum + Q*lsb) >> 1.
    and     r12, r0, #1
    mul     r12, r12, r5
    add     r0, r0, r12
    lsr     r0, r0, #1

    strh    r0, [r11], #2              @ a[k1] = half(canon-add)
    strh    r3, [r11], #2              @ a[k2] = montymul(s, canon-sub)

    @ For t=1, one butterfly per block. r11 now at next block's k1.
    ldr     r0, [sp, #0]
    subs    r0, r0, #1
    str     r0, [sp, #0]
    bne     intti_uloop_scalar

intti_layer_done:
    @ t_bytes_doubled *= 2; hm /= 2
    lsl     r9, r9, #1
    ldr     r0, [sp, #8]
    lsr     r0, r0, #1
    str     r0, [sp, #8]
    b       intti_layer

intti_done:
    add     sp, sp, #16
    pop     {r4-r11, lr}
    bx      lr
    .size   mq18433_iNTT_plant, .-mq18433_iNTT_plant

#endif /* __ARM_FEATURE_DSP */
