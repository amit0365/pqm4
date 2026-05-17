/*
 * plant_18433_cm4.S — Cortex-M4 (ARMv7E-M) implementation of HAWK's
 * Q=18433 Plantard/Montgomery NTT entry points.
 *
 * Provides:
 *   mq18433_NTT_plant(unsigned logn, uint16_t *a)
 *   mq18433_iNTT_plant(unsigned logn, uint16_t *a)
 *   mq18433_montymul_plant(uint32_t x, uint32_t y)
 *
 * Cross-checked byte-identical to modq.h's Zq(NTT)/Zq(iNTT)/Zq(montymul)
 * over logn = 1..10 (HAWK-512 and HAWK-1024) via tests/m4/test_ntt.c
 * and tests/m4/test_montymul.c.
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
 * PACKING STRATEGY
 * ----------------
 * Layers with ht >= 2 (HT_bytes >= 4) use SAME-TWIDDLE packing:
 *   (a[k1], a[k1+1]) packed into r0, (a[k2], a[k2+1]) into r1, twiddle s
 *   shared across both lanes — smulbb/smultb fuse unpack+multiply.
 *
 * The LAST layer of NTT and FIRST layer of iNTT (stride = 1 halfword)
 * use CROSS-COEFFICIENT packing: blocks u=2i and u=2i+1 share the
 * packed kernel, with each lane carrying its OWN twiddle (smulbb /
 * smultt). The single-butterfly fallback runs only at logn==1.
 *
 * Frame-level optimizations applied on top of the inner kernel:
 *   - The explicit "advance p_k1 by 4 bytes" at the end of every v-loop
 *     is folded into the last store via post-indexed writeback.
 *   - The scalar-layer u-loops keep u_remaining in r14 and p_GM/p_iGM
 *     in r8 across the loop body (no per-iter ldr/str against the
 *     stack-spilled copies).
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
    @ Cannot pack the c_i = x2_i * s product (up to 30 bits per cell),
    @ but smulbb/smultb fuse the unpack+multiply: HAWK's Q < 2^15 so
    @ x2 halves never have bit 15 set ⇒ signed = unsigned multiply.
    @ Pattern borrowed from Pornin's fndsa mq_cm4.s.
    smulbb  r3, r1, r2                 @ r3 = bot16(r1) * bot16(r2) = x2_lo * s
    smultb  r8, r1, r2                 @ r8 = top16(r1) * bot16(r2) = x2_hi * s
    mul     r3, r3, r4                 @ r3 = c_lo * Q0I mod 2^32
    mul     r8, r8, r4                 @ r8 = c_hi * Q0I
    lsr     r3, r3, #16                @ r3 = h_lo
    lsr     r8, r8, #16                @ r8 = h_hi
    mla     r3, r3, r5, r6             @ r3 = h_lo*Q + QA
    mla     r8, r8, r5, r6             @ r8 = h_hi*Q + QA
    lsr     r3, r3, #16                @ r3 = reduced_lo  in [1..Q]
    lsr     r8, r8, #16                @ r8 = reduced_hi  in [1..Q]
    pkhbt   r1, r3, r8, lsl #16        @ r1 = (reduced_hi << 16) | reduced_lo

    @ Canonical sub FIRST (no writeback on r11): per half, x1 - m mod Q
    @ in [1..Q] with 0 ⇒ Q. Sequence (using only USUB16's "no-borrow"
    @ flag is hard because the following UADD16 clobbers GE; instead we
    @ use UADD16's overflow GE flag, which is set precisely when the
    @ USUB16 had a borrow — i.e., m < x1):
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

    @ Canonical add: per half, if (x1+m) > Q subtract Q.
    @   sum  = uadd16(x1, m)                 ; in [2..2Q] per half, no overflow
    @   chk  = usub16(sum, Q+1)              ; GE_h set if sum_h > Q
    @   sub_ = usub16(sum, Q)                ; sum - Q per half
    @   res  = sel(sub_, sum)                ; GE → sub_; else sum
    @
    @ Stored with post-inc writeback so the explicit `adds r11, #4`
    @ at loop tail is folded in (1 cycle / packed v-iter).
    uadd16  r3, r0, r1                 @ r3 = sum
    usub16  r8, r3, r7                 @ r8 = sum - Q
    usub16  r12, r3, r10               @ r12 = sum - (Q+1), GE flags
    sel     r8, r8, r3                 @ r8 = canon-add result
    str.w   r8, [r11], #4              @ a[k1..k1+1] = canon-add result; r11+=4

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
@ Two butterflies per packed iter via CROSS-COEFFICIENT packing:
@ blocks u=2i and u=2i+1 (adjacent in memory at HT_bytes=2) processed
@ together, packing (x1_a, x1_b) into r3 and (x2_a, x2_b) into r1.
@ The same Plantard kernel that already runs in the ht>=2 path applies
@ per lane (Q < 2^15 makes signed multiply == unsigned).
@
@ Falls back to single-butterfly when m == 1 (only for logn==1).
ntt_uloop_scalar:
    ldr     r0, [sp, #0]               @ u_remaining = m
    cmp     r0, #1
    beq     ntt_scalar_single          @ m == 1 (logn==1): single butterfly fallback

    @ m is even (m = 2^(logn-1) >= 2 for logn>=2). Process pairs.
    lsr     r14, r0, #1                @ v_remaining = m/2 packed iters
    ldr     r8, [sp, #12]              @ r8 = p_GM, held across the loop body
                                       @ (no scratch inside body conflicts).

ntt_vloop_scalar_packed:
    ldr     r2, [r8], #4               @ r2 = (s_a) | (s_b << 16); p_GM += 2 halfwords
    ldr     r0, [r11]                  @ word0 = (x1_a) | (x2_a << 16)
    ldr     r1, [r11, #4]              @ word1 = (x1_b) | (x2_b << 16)

    @ Repack: r3 = x1_packed = (x1_a, x1_b); r1 = x2_packed = (x2_a, x2_b).
    pkhbt   r3, r0, r1, lsl #16        @ r3 = x1_a | (x1_b << 16)
    pkhtb   r1, r1, r0, asr #16        @ r1 = x2_a | (x2_b << 16)

    @ Plantard kernel: m = montymul(x2, s) per lane → r1 = packed reduced.
    @ Each lane has its OWN twiddle (s_a in low, s_b in high), unlike the
    @ ht>=2 path that shares one twiddle across both lanes. So use
    @ smulbb (bot*bot = x2_a * s_a) and smultt (top*top = x2_b * s_b).
    smulbb  r0, r1, r2                 @ c_lo = x2_a * s_a
    smultt  r12, r1, r2                @ c_hi = x2_b * s_b
    mul     r0, r0, r4
    mul     r12, r12, r4
    lsr     r0, r0, #16
    lsr     r12, r12, #16
    mla     r0, r0, r5, r6
    mla     r12, r12, r5, r6
    lsr     r0, r0, #16
    lsr     r12, r12, #16
    pkhbt   r1, r0, r12, lsl #16       @ r1 = m_packed

    @ canon-sub: r0 = canonical(x1 - m) per lane
    usub16  r0, r1, r3
    uadd16  r12, r0, r7
    sel     r0, r12, r0
    ssub16  r0, r7, r0                 @ r0 = canon-sub packed

    @ canon-add: r1 = canonical(x1 + m) per lane (r3 dies here)
    uadd16  r3, r3, r1
    usub16  r1, r3, r7
    usub16  r12, r3, r10
    sel     r1, r1, r3                 @ r1 = canon-add packed

    @ Interleave back to the memory layout (k1_a, k2_a, k1_b, k2_b):
    @   word0 = canon_add_lo | (canon_sub_lo << 16) → a[2u..2u+1]
    @   word1 = canon_add_hi | (canon_sub_hi << 16) → a[2u+2..2u+3]
    pkhbt   r3, r1, r0, lsl #16        @ r3 = word0
    pkhtb   r12, r0, r1, asr #16       @ r12 = word1
    str     r3, [r11], #4              @ store word0, r11+=4
    str     r12, [r11], #4             @ store word1, r11+=4

    subs    r14, r14, #1
    bne     ntt_vloop_scalar_packed

    str     r8, [sp, #12]              @ stash advanced p_GM back
    b       ntt_layer_done

@ Single-butterfly fallback for m == 1 (logn==1 only).
ntt_scalar_single:
    ldr     r8, [sp, #12]
    ldrh    r2, [r8], #2
    str     r8, [sp, #12]

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
    strh    r0, [r11], #2              @ a[k2]

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
    @ smulbb/smultb fuse the unpack+multiply (canon-sub result is in
    @ [1..Q] < 2^15 ⇒ signed = unsigned).
    smulbb  r8, r3, r2                 @ c_lo = s * sub_lo
    smultb  r12, r3, r2                @ c_hi = s * sub_hi
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
    str.w   r3, [r11], #4              @ a[k1..k1+1] = half(canon-add); r11+=4

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
@ Two butterflies per packed iter via CROSS-COEFFICIENT packing of
@ adjacent blocks (u=2i, u=2i+1). Falls back to single butterfly when
@ hm == 1 (logn==1 only).
intti_uloop_scalar:
    ldr     r0, [sp, #0]               @ u_remaining = hm
    cmp     r0, #1
    beq     intti_scalar_single        @ hm == 1: single fallback (logn==1)

    @ hm even (hm = 2^(logn-1) >= 2 for logn>=2). Process in pairs.
    lsr     r14, r0, #1                @ v_remaining = hm/2 packed iters
    ldr     r8, [sp, #12]              @ r8 = p_iGM, held across body

intti_vloop_scalar_packed:
    ldr     r2, [r8], #4               @ r2 = (s_a) | (s_b << 16)
    ldr     r0, [r11]                  @ word0 = (x1_a) | (x2_a << 16)
    ldr     r1, [r11, #4]              @ word1 = (x1_b) | (x2_b << 16)

    @ Repack: r3 = x1_packed (a, b); r1 = x2_packed (a, b).
    pkhbt   r3, r0, r1, lsl #16        @ r3 = x1_a | (x1_b << 16)
    pkhtb   r1, r1, r0, asr #16        @ r1 = x2_a | (x2_b << 16)

    @ Canon-sub: r12 = canonical(x1 - x2) packed in [1..Q]. Kept alive
    @ through canon-add and consumed by Plantard.
    usub16  r12, r1, r3
    uadd16  r0, r12, r7
    sel     r12, r0, r12
    ssub16  r12, r7, r12               @ r12 = canon-sub packed

    @ Canon-add: r0 = canonical(x1 + x2). x1 (r3) and x2 (r1) die here.
    uadd16  r0, r3, r1
    usub16  r3, r0, r7
    usub16  r1, r0, r10
    sel     r0, r3, r0                 @ r0 = canon-add packed

    @ half() on r0: r0 = (sum + Q*lsb) >> 1 per lane.
    add     r3, r6, #1                 @ r3 = 0x00010001
    and     r3, r0, r3                 @ packed lsb mask
    mul     r3, r3, r5                 @ lsb * Q packed
    uadd16  r0, r0, r3                 @ sum + Q*lsb (both halves even)
    lsr     r0, r0, #1                 @ r0 = half_packed

    @ Plantard kernel: r12 = montymul(s_packed, canon_sub_packed).
    @ Per-lane twiddles → smulbb / smultt.
    smulbb  r1, r12, r2                @ c_lo
    smultt  r3, r12, r2                @ c_hi
    mul     r1, r1, r4
    mul     r3, r3, r4
    lsr     r1, r1, #16
    lsr     r3, r3, #16
    mla     r1, r1, r5, r6
    mla     r3, r3, r5, r6
    lsr     r1, r1, #16
    lsr     r3, r3, #16
    pkhbt   r12, r1, r3, lsl #16       @ r12 = m_packed (new k2 values)

    @ Interleave back to memory layout:
    @   word0 = a[2u]   | (a[2u+1] << 16) = half_lo | (m_lo << 16)
    @   word1 = a[2u+2] | (a[2u+3] << 16) = half_hi | (m_hi << 16)
    pkhbt   r3, r0, r12, lsl #16       @ word0
    pkhtb   r1, r12, r0, asr #16       @ word1
    str     r3, [r11], #4
    str     r1, [r11], #4

    subs    r14, r14, #1
    bne     intti_vloop_scalar_packed

    str     r8, [sp, #12]              @ stash advanced p_iGM
    b       intti_layer_done

@ Single-butterfly fallback for hm == 1 (logn==1 only).
intti_scalar_single:
    ldr     r8, [sp, #12]
    ldrh    r2, [r8], #2
    str     r8, [sp, #12]

    ldrh    r0, [r11]                  @ x1
    ldrh    r1, [r11, #2]              @ x2

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

    @ Canon-add: r0 = canonical(x1 + x2)
    add     r0, r0, r1
    cmp     r0, r5
    it      hi
    subhi   r0, r0, r5

    @ Half: r0 = (sum + Q*lsb) >> 1.
    and     r12, r0, #1
    mul     r12, r12, r5
    add     r0, r0, r12
    lsr     r0, r0, #1

    strh    r0, [r11], #2
    strh    r3, [r11], #2

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


@ -------------------------------------------------------------------
@ void mq18433_NTT_pair_plant(unsigned logn, uint16_t *a, uint16_t *b);
@
@ Performs the forward NTT on TWO polynomials a and b simultaneously.
@ For each butterfly group at each layer, the twiddle ω is loaded ONCE
@ from the GM table and applied to BOTH a and b before advancing to the
@ next twiddle.
@
@ Output is byte-identical to two sequential mq18433_NTT_plant calls
@ on a and b — both polynomials end in canonical [1..Q] form.
@
@ Register layout — to fit p_k1_b in a register we replace one usage of
@ r12 in each inner body:
@   - Packed body: `usub16 r12, r3, r10` (sets GE flags, target was dead
@     after) becomes `usub16 r0, r3, r10` since r0 is dead at that point.
@   - Scalar body: substitute r9 → as the high-lane scratch throughout
@     (was r12 in the single-NTT scalar body). r9 is HT_bytes and is
@     not used inside the scalar body — offsets are fixed `#4`. r9
@     gets clobbered, so we restore it to #2 after the loop (or simply
@     branch directly to done, since this is always the LAST layer).
@
@ Result: r12 carries p_k1_b across every body iter and across layer
@ transitions. The merged v-loop processes one a-butterfly + one
@ b-butterfly per iter, sharing twiddle r2 and the loop counter r14.
@
@ Stack frame (24 bytes, 8-byte aligned):
@   [sp+0]   u_remaining  (per-u-iter; reset at layer start)
@   [sp+4]   a_base       (constant for the call)
@   [sp+8]   m_current    (per-layer; doubles each layer transition)
@   [sp+12]  p_GM         (advances by 2 bytes per u-iter)
@   [sp+16]  b_base       (constant for the call; reloaded into r12 each layer)
@
@ Register map:
@   r0,r1   x1, x2 (packed) / scratch (r0 reused for GE-only usub16)
@   r2      s (twiddle, shared across a and b)
@   r3,r8   inner scratch (r8 holds p_GM in the scalar merged loop)
@   r4      Q0I
@   r5      Q
@   r6      QA
@   r7      Q_packed
@   r9      HT_bytes (packed body) / high-lane scratch (scalar body)
@   r10     Qp1_packed
@   r11     p_k1_a
@   r12     p_k1_b
@   r14     v_remaining
@ -------------------------------------------------------------------
    .global mq18433_NTT_pair_plant
    .type   mq18433_NTT_pair_plant, %function
    .thumb_func
    .align  2
mq18433_NTT_pair_plant:
    push    {r4-r11, lr}
    sub     sp, sp, #24

    @ Scalar constants
    movw    r4, #0x47FF
    movt    r4, #0xEBC0                @ r4 = Q0I
    movw    r5, #18433                 @ r5 = Q
    mov.w   r6, #0x10000               @ r6 = QA

    @ Packed constants
    pkhbt   r7, r5, r5, lsl #16        @ r7 = Q_packed
    adds    r3, r5, #1
    pkhbt   r10, r3, r3, lsl #16       @ r10 = Qp1_packed

    @ Stash bases
    str     r1, [sp, #4]               @ a_base
    str     r2, [sp, #16]              @ b_base

    @ Initialize p_GM = &mq18433_GM[1]
    movw    r3, #:lower16:mq18433_GM
    movt    r3, #:upper16:mq18433_GM
    adds    r3, r3, #2
    str     r3, [sp, #12]

    @ HT_bytes_initial = 1 << logn
    movs    r9, #1
    lsls    r9, r9, r0

    @ m_initial = 1
    movs    r3, #1
    str     r3, [sp, #8]

nttp_layer:
    cmp     r9, #2
    blt.w   nttp_done

    @ Layer setup: r11 = a_base, r12 = b_base, u_remaining = m
    ldr     r11, [sp, #4]
    ldr     r12, [sp, #16]
    ldr     r3, [sp, #8]
    str     r3, [sp, #0]               @ u_remaining = m

    @ Branch: HT_bytes >= 4 ⇒ packed; HT_bytes == 2 ⇒ scalar
    cmp     r9, #4
    blt.w   nttp_uloop_scalar

@ ---------- PACKED u-loop (ht >= 2) ----------
nttp_uloop_packed:
    @ Shared ω load (once for both a and b)
    ldr     r8, [sp, #12]
    ldrh    r2, [r8], #2
    str     r8, [sp, #12]

    @ v_remaining = ht/2 = HT_bytes/4
    lsr     r14, r9, #2

@ === MERGED v-loop: process one a-butterfly + one b-butterfly per iter ===
@ The two bodies share twiddle r2 and constants r4-r7, r10. The first body
@ (a) advances r11; the second body (b) advances r12. Loop control runs
@ once per merged iter (was twice in the separate two-loop layout).
nttp_vloop_packed_merged:
    @ ---- a's butterfly (uses r11) ----
    ldr.w   r0, [r11]
    ldr.w   r1, [r11, r9]
    smulbb  r3, r1, r2
    smultb  r8, r1, r2
    mul     r3, r3, r4
    mul     r8, r8, r4
    lsr     r3, r3, #16
    lsr     r8, r8, #16
    mla     r3, r3, r5, r6
    mla     r8, r8, r5, r6
    lsr     r3, r3, #16
    lsr     r8, r8, #16
    pkhbt   r1, r3, r8, lsl #16
    usub16  r3, r1, r0
    uadd16  r8, r3, r7
    sel     r3, r8, r3
    ssub16  r3, r7, r3
    str.w   r3, [r11, r9]
    uadd16  r3, r0, r1
    usub16  r8, r3, r7
    usub16  r0, r3, r10
    sel     r8, r8, r3
    str.w   r8, [r11], #4

    @ ---- b's butterfly (uses r12) ----
    ldr.w   r0, [r12]
    ldr.w   r1, [r12, r9]
    smulbb  r3, r1, r2
    smultb  r8, r1, r2
    mul     r3, r3, r4
    mul     r8, r8, r4
    lsr     r3, r3, #16
    lsr     r8, r8, #16
    mla     r3, r3, r5, r6
    mla     r8, r8, r5, r6
    lsr     r3, r3, #16
    lsr     r8, r8, #16
    pkhbt   r1, r3, r8, lsl #16
    usub16  r3, r1, r0
    uadd16  r8, r3, r7
    sel     r3, r8, r3
    ssub16  r3, r7, r3
    str.w   r3, [r12, r9]
    uadd16  r3, r0, r1
    usub16  r8, r3, r7
    usub16  r0, r3, r10
    sel     r8, r8, r3
    str.w   r8, [r12], #4

    subs    r14, r14, #1
    bne     nttp_vloop_packed_merged

    @ Advance r11 and r12 past second-half cells to next u-block start
    add     r11, r11, r9
    add     r12, r12, r9

    @ u_remaining--
    ldr     r0, [sp, #0]
    subs    r0, r0, #1
    str     r0, [sp, #0]
    bne     nttp_uloop_packed
    b       nttp_layer_done

@ ---------- SCALAR u-loop (ht == 1, last layer) ----------
@ Two butterflies per iter via cross-coefficient packing. r9 is repurposed
@ as the high-lane scratch (no [r11, r9] addressing here — offsets are
@ fixed `#4`); r12 carries p_k1_b. p_GM lives in r8 across the merged
@ loop. Since this is the last layer, we branch directly to nttp_done
@ at the end and skip the layer_done bookkeeping.
nttp_uloop_scalar:
    ldr     r0, [sp, #0]
    cmp     r0, #1
    beq     nttp_scalar_single         @ m == 1 fallback (logn==1)

    @ v_remaining (packed) = m/2
    lsr     r14, r0, #1

    @ Load p_GM into r8 (held across the loop).
    ldr     r8, [sp, #12]

@ === Merged scalar v-loop: a-pair + b-pair per iter (cross-coef packing) ===
@ Sharing the same ω pair (4-byte read advances p_GM by 4 = 2 halfwords).
nttp_vloop_scalar_merged:
    ldr     r2, [r8], #4               @ shared ω pair (s_a | s_b<<16)

    @ ---- a's pair butterfly (r11 = p_k1_a) ----
    ldr     r0, [r11]
    ldr     r1, [r11, #4]
    pkhbt   r3, r0, r1, lsl #16
    pkhtb   r1, r1, r0, asr #16
    smulbb  r0, r1, r2
    smultt  r9, r1, r2
    mul     r0, r0, r4
    mul     r9, r9, r4
    lsr     r0, r0, #16
    lsr     r9, r9, #16
    mla     r0, r0, r5, r6
    mla     r9, r9, r5, r6
    lsr     r0, r0, #16
    lsr     r9, r9, #16
    pkhbt   r1, r0, r9, lsl #16
    usub16  r0, r1, r3
    uadd16  r9, r0, r7
    sel     r0, r9, r0
    ssub16  r0, r7, r0
    uadd16  r3, r3, r1
    usub16  r1, r3, r7
    usub16  r9, r3, r10
    sel     r1, r1, r3
    pkhbt   r3, r1, r0, lsl #16
    pkhtb   r9, r0, r1, asr #16
    str     r3, [r11], #4
    str     r9, [r11], #4

    @ ---- b's pair butterfly (r12 = p_k1_b) ----
    ldr     r0, [r12]
    ldr     r1, [r12, #4]
    pkhbt   r3, r0, r1, lsl #16
    pkhtb   r1, r1, r0, asr #16
    smulbb  r0, r1, r2
    smultt  r9, r1, r2
    mul     r0, r0, r4
    mul     r9, r9, r4
    lsr     r0, r0, #16
    lsr     r9, r9, #16
    mla     r0, r0, r5, r6
    mla     r9, r9, r5, r6
    lsr     r0, r0, #16
    lsr     r9, r9, #16
    pkhbt   r1, r0, r9, lsl #16
    usub16  r0, r1, r3
    uadd16  r9, r0, r7
    sel     r0, r9, r0
    ssub16  r0, r7, r0
    uadd16  r3, r3, r1
    usub16  r1, r3, r7
    usub16  r9, r3, r10
    sel     r1, r1, r3
    pkhbt   r3, r1, r0, lsl #16
    pkhtb   r9, r0, r1, asr #16
    str     r3, [r12], #4
    str     r9, [r12], #4

    subs    r14, r14, #1
    bne     nttp_vloop_scalar_merged

    @ Scalar layer is the LAST layer — branch directly to done.
    @ (Saves the layer_done bookkeeping and one cmp.)
    b       nttp_done

@ Single-butterfly fallback for m == 1 (logn==1 only).
nttp_scalar_single:
    @ Inline two single butterflies (a then b) sharing the same ω.
    ldr     r8, [sp, #12]
    ldrh    r2, [r8], #2
    str     r8, [sp, #12]

    @ ---- a's single butterfly ----
    ldrh    r0, [r11]
    ldrh    r1, [r11, #2]
    mul     r1, r1, r2
    mul     r1, r1, r4
    lsr     r1, r1, #16
    mla     r1, r1, r5, r6
    lsr     r1, r1, #16
    add     r3, r0, r1
    cmp     r3, r5
    it      hi
    subhi   r3, r3, r5
    subs    r0, r0, r1
    it      le
    addle   r0, r0, r5
    strh    r3, [r11], #2
    strh    r0, [r11], #2

    @ ---- b's single butterfly (r12 = p_k1_b) ----
    ldrh    r0, [r12]
    ldrh    r1, [r12, #2]
    mul     r1, r1, r2
    mul     r1, r1, r4
    lsr     r1, r1, #16
    mla     r1, r1, r5, r6
    lsr     r1, r1, #16
    add     r3, r0, r1
    cmp     r3, r5
    it      hi
    subhi   r3, r3, r5
    subs    r0, r0, r1
    it      le
    addle   r0, r0, r5
    strh    r3, [r12]
    strh    r0, [r12, #2]
    b       nttp_done                  @ logn==1 is the only path here; done.

nttp_layer_done:
    @ HT_bytes /= 2; m *= 2
    lsr     r9, r9, #1
    ldr     r0, [sp, #8]
    lsl     r0, r0, #1
    str     r0, [sp, #8]
    b.w     nttp_layer

nttp_done:
    add     sp, sp, #24
    pop     {r4-r11, lr}
    bx      lr
    .size   mq18433_NTT_pair_plant, .-mq18433_NTT_pair_plant


#endif /* __ARM_FEATURE_DSP */
