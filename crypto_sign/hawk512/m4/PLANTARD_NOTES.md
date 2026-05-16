# Plantard NTT for HAWK at Q=18433 — design and calibration plan

## Why we touch only `hawk_sign.c`

HAWK uses three different prime moduli, only one of which is in
Plantard's sweet spot:

| File             | Modulus                | Used by                          |
|------------------|------------------------|----------------------------------|
| `hawk_sign.c`    | Q = 18433 (~2¹⁴.²)     | `mq18433_*` (15 call sites)      |
| `hawk_vrfy.c`    | A 31-bit prime p       | local `mp_NTT` / `mp_NTT_autoadj`|
| `hawk_kgen.c`    | `ng_mp31.c`'s 31-bit p | NTRUgen small-prime layer        |

Plantard arithmetic is designed for `q < 2¹⁶` (and is fastest for
`q < 2¹⁵`), exactly matching HAWK's `Q = 18433`. The 31-bit-prime
NTTs in verify and keygen need separate work (Solinas / Mersenne /
Barrett are the usual choices at that size) and are out of scope for
this commit. The headline target is therefore the **sign hot path**
only.

## Plantard primitive (target form)

Following UIC-ESLAS/ImprovedPlantardArithmetic
(`crypto_sign/dilithium3/new/macros_smallntt.i`, the `plant_red` and
`mul_twiddle_plant` macros), each butterfly multiplication becomes
two M4 instructions:

```
mul    tmp, a, qinv          @ tmp = (a · Qinv_Plant) mod 2³²
smlatt tmp, tmp, q, qa       @ tmp_high = (tmp_high · q_high) + qa
                             @ — final value sits in the top 16 bits
```

The trick: a packed pair `(c0, c1)` of int16 coefficients in one
register can be reduced in **4 instructions for both** using
`smulwb / smulwt / smlabt / smlabt` (UIC-ESLAS's
`mul_twiddle_plant`), versus Montgomery's typical 6-instruction
sequence — and importantly, the packed-pair form keeps both
coefficients alive in a single register through the layer.

## Plantard constants for Q = 18433

Derived once, used everywhere:

| symbol         | value          | derivation                                   |
|----------------|----------------|----------------------------------------------|
| `Q`            | `18433`        | HAWK Round-2 spec                            |
| `Qinv_Plant`   | `339720193`    | `q⁻¹ mod 2³²`; verified `q · Qinv ≡ +1 mod 2³²`. HAWK's existing `Q0I = 3955247103` is `−q⁻¹ mod 2³²`, so `Qinv_Plant = 2³² − Q0I = 339720193`. |
| `qa` (round)   | `+1`           | `2^(l − ⌈log₂ q⌉ − 1) = 2^(16 − 15 − 1) = 1`. For comparison Kyber's `q = 3329 < 2¹²` uses `+8`. |
| `q_packed`     | `(Q<<16) | Q`  | Lives in both halves so packed instructions can hit it from either side. |

Sanity check (mental): `18433 · 339720193 mod 2³²`:
`18433 · 339720193 = 6 262 062 317 569 = 1458 · 2³² + 1`, so the
product is exactly `1 mod 2³²`. ✓

## Representation alignment — resolved

(Initial design note had this section open; resolved by the
calibration test, recorded here.)

HAWK's `R` is actually **`2³² mod q`** (not `2¹⁶` — `modq.h` comment
`R = 2^32 mod q` confused me on the first pass), and `Zq(montyred)`
is a 32→16-bit Montgomery reduction giving `c · 2⁻³² mod q` in
`[1..q]`. **Plantard's natural output factor `2⁻³²` already matches
HAWK's Montgomery scaling exactly.** No twiddle re-baking is
required at the math level — feed the existing `GM[x]` table to a
Plantard reducer and the modular value is correct.

### What the calibration test proved

`tests/test_plantard_calibrate.c` runs 4096 random `(a, b)` pairs
in `[1..Q]²` and compares various Plantard formulations against
`mq18433_montymul(a, b)` (golden). Highlights:

| variant   | encoding   | mismatches / 4096 |
|-----------|------------|-------------------|
| `v6+encId`| identity   | **0** (literally `montyred`) |
| `v8+encId`| identity   | 313 (7.6%), always off by ±1 |
| others    | various    | all 4096 mismatch |

The takeaway:

- **Sign of `Qinv`**: HAWK's `Q0I = −q⁻¹ mod 2³²` is the right value
  to use (matches the convention behind `Zq(montyred)`). UIC-ESLAS's
  `Qinv_Plant` (`+q⁻¹ mod 2³²`) gives a negated result and so does
  not match HAWK directly.
- **Rounding placement**: `montyred` adds `+1` *after* the second
  shift; naive Plantard adds the rounding `qa` *before* the second
  multiplication. With unsigned `Q0I` and `qa = 1` these agree on
  92.4% of inputs and disagree by exactly 1 on the other 7.6% — the
  algebraic cause is whether `(h·Q) mod 2¹⁶ ≥ 2¹⁶ − Q`; the
  empirical 7.6% (vs the uniform-h prediction of ~72%) reflects that
  `h = TOP16(c·Q0I mod 2³²)` for `c ∈ [1, Q²]` is far from uniform.

### Spec-adherence consequence

HAWK signatures encode NTT-domain coefficients byte-for-byte. Any
1-bit divergence between the optimised reducer and `montyred`
ripples into adds, subtracts, and comparisons downstream and breaks
KAT-vector byte equality. So **the M4 asm port has to reproduce
`montyred`'s exact output, not Plantard's natural output**.

In asm that means a three-instruction sequence per coefficient:

```
mul    tmp, c, q0i             @ tmp = c·Q0I  mod 2³²
lsr    tmp, tmp, #16           @ tmp = h     (top 16 of c·Q0I)
smlabt tmp, tmp, q, #1<<16     @ tmp = h·Q  + 2¹⁶     (low 16 bits of result; rounding +1 baked into the high half added pre-shift)
                               @ — result lives in the high 16 bits, ready for a final >>16
```

versus UIC-ESLAS's two-instruction `mul / smlatt` for naive
Plantard. One extra cycle per coefficient — the packed-pair
butterfly structure (4 instructions per 2 coefficients via
`smulwb / smulwt / smlabt / smlabt`) is still a clear win over the
~6–7 cycle pure-C `Zq(montymul)`.

No twiddle re-baking is needed: the existing `GM[]` / `iGM[]`
tables in `modq.h` feed the asm directly.

## Spec adherence

HAWK's specification fixes the byte encoding of signatures and the
deterministic SHAKE-256-based commitment. The Plantard NTT
replacement must therefore be **byte-identical** to the reference
Montgomery NTT, not just modulo-equivalent: every output coefficient
must hit the same integer representative in `[1..q]` that the
reference produces.

This is what the calibration test in
`tests/test_plantard_calibrate.c` checks. It runs over random
`(a, b) ∈ [1..q]²`, computes the golden product via
`mq18433_montymul`, and searches over a small set of candidate
Plantard encodings to find the one that reproduces the golden value
exactly on every input. Anything less is a spec violation.

## pqm4 already ships UIC-ESLAS Plantard NTT — reusable scaffolding

After the encoding above was locked I noticed pqm4 already vendors
the full Plantard NTT from Huang et al. (TCHES 2024) at:

    crypto_sign/ml-dsa-{44,65,87}/m4f/macros_smallntt.i
    crypto_sign/ml-dsa-{44,65,87}/m4f/smallntt_769.S

The macros file is fully parameterised by register names (`q`, `qa`,
`qinv`, `tmp`) so the entire asm template is reusable. What changes
for HAWK:

  | concern                 | ml-dsa (q=769, n=256)                | HAWK (Q=18433, n=512/1024)                 |
  |-------------------------|--------------------------------------|--------------------------------------------|
  | `q` immediate           | `movt q, #769`                       | `movt q, #18433` (still 16-bit immediate)  |
  | `qa` constant           | `movw qa, #24608` (= q·32, plant rc=32) | depends on representation choice (below) |
  | twiddle table           | `zetas_asm_769[128]` (int32, pre-encoded) | derive `mq18433_GM_plant[]` from `GM[]` |
  | layer-merge structure   | hand-unrolled for n=256              | +1 layer-group for n=512, +2 for n=1024    |

## The representation gap (path-A vs path-B)

`tests/test_plant_twiddle.c` simulates the asm `mul_twiddle_plant` in
C and searches the encoding space. Best candidate (`encA qa=0x10000`,
i.e. twiddle pre-multiplied by `Q0I`, rounding `+1<<16`) still misses
~51 % of random `(a, b)` pairs in `[1..Q]²` against `mq18433_montymul`.

Root cause: `smulwb` is a **signed** multiply with a **signed** `>>16`,
so the high-half it produces differs from HAWK's `montyred`-style
**unsigned** `>>16` by exactly `2¹⁶` whenever bit 31 of `c·Q0I` is set
(~50 % of inputs). Plantard's natural output range is `[-q, q]`
(roughly), HAWK's intermediate convention is `[1..Q]`.

`hawk_sign.c` interleaves NTT calls with `mq18433_montymul` /
`mq18433_sub` / `mq18433_tomonty` on individual coefficients, so the
post-NTT data must be in HAWK's canonical `[1..Q]` representation —
we cannot just leave it in Plantard's centred form mid-protocol.

Two ways forward, both keep the locked-down `Q0I = 3955247103` and
`mul_twiddle_plant` macro:

  **Path A** (mirrors ml-dsa): run the whole NTT in Plantard's
  centred-signed representation, then sweep one normalisation pass
  at NTT exit to convert each coefficient from `[-q, q]` to `[1..Q]`.
  Pointwise helpers between NTT calls need matching signed/unsigned
  versions, *or* the normalisation pass runs both at NTT exit AND
  before re-entry. The fastest variant, matches the upstream Plantard
  literature.

  **Path B** (touches the inner kernel): bake the conditional `+Q`
  into the inner butterfly so the running representation stays
  `[1..Q]` throughout. Easier to drop in (no other code changes
  needed), but ~1–2 extra cycles per butterfly.

The choice is a perf/intrusiveness tradeoff; either honours the
HAWK spec because the cross-check test (`test_plant_ntt.c`) will
catch any byte-drift in the integrated NTT output.

## Path A attempt — what failed and why

(Recorded so the next iteration doesn't repeat the dead end.)

I tried implementing Path A in C: signed Plantard reduction
`plant_red(c)` keeping coefficients in centred form `[-Q, Q]`,
plus a normalisation pass at NTT exit. The single-coefficient
plant_red was correct against `mq18433_montymul` (a separate
test, `dbg_plant.c`, ran clean over 50 random `(a, b)` pairs).
The 2-layer NTT (`n=4`) also matched byte-for-byte after
normalisation.

But for `n=256` the byte-equality test failed at layer 1
onward: ~12 positions diverged after layer 1 (always by `±1`),
growing to ~all 256 positions by layer 6. The values
**weren't even congruent mod Q** (`12382` vs `3362`, diff `9020`
which isn't a multiple of `Q=18433`).

Switching `plant_red` to *unsigned* arithmetic (literally
`Zq(montyred)` inlined) didn't fix it — the failure mode
changed but the test still failed with non-congruent diffs.
The bug isn't in `plant_red`: I verified `plant_red` returns
the same value as `montyred` for `int32` inputs in
`{1, -1, ±100, ±1000, ±5000, ±Q, ±100·Q, ±Q²/2}` and for
`±x · twiddle` pairs that are mod-Q equivalents.

So the bug is somewhere in the butterfly's combination of
`plant_red` output with the running centred state — likely in
`centred_reduce` interacting with the signed/unsigned read of
storage cells, or in `to_canonical`'s representative choice
when the centred value lands on a boundary. I couldn't
isolate it in this iteration without an ARM toolchain and
on-target verification to corroborate.

**Reverted to the byte-identical-by-construction baseline.**
`plant_18433.c` is once again line-for-line `Zq(NTT)` /
`Zq(iNTT)` (the cross-check passes trivially). This is
effectively the **Path B** structural choice for the C path,
even though the test now is just verifying our wiring, not
exercising any speedup.

### Open question for the next iteration

Is the signed-shift rounding fundamentally incompatible with
HAWK's spec, or is there a subtle bug in the
`centred_reduce` / `to_canonical` / storage-cell-cast triplet?

Two concrete experiments to disambiguate:

  1. Run pqm4's existing `small_ntt_asm_769` (ml-dsa Plantard
     NTT) on hardware against a coefficient-by-coefficient
     reference, see if it claims byte-identity or only
     "byte-identity after the post-NTT pointwise pipeline".
     If the latter, that confirms signed Plantard is
     inherently mid-NTT-divergent and HAWK has to either
     adopt the same post-NTT-pipeline semantics or stay on
     Path B.

  2. Try a C version of Path A where the per-butterfly
     storage uses `int16_t a[]` directly (not a `uint16_t`
     bit-cast). That eliminates any sign-extension ambiguity
     on cell read/write and would isolate whether the bug is
     in the cast or in the math.

### Experiment (a) result — answered by reading ml-dsa's code

ml-dsa's Plantard NTT does **not** claim mid-NTT byte equality
with the reference. The architecture in
`crypto_sign/ml-dsa-44/m4f/smallpoly.c`:

```c
small_ntt(out->coeffs);                   // canonical -> Plantard rep
small_point_mul(out2, out->coeffs);       // Plantard-aware pointwise
...
small_asymmetric_mul(tmp, b, a, aprime);  // Plantard-aware composed mul
small_invntt_tomont(tmp->coeffs);         // Plantard rep -> Mont canonical
```

There is no `reduce`/`caddq` step between `small_ntt` and the
pointwise helpers (in contrast, the regular ml-dsa NTT — used
for keygen — DOES call `polyveck_reduce` / `polyveck_caddq` after
`invntt`, see `sign.c:51, 58, 145, 149`). The Plantard pipeline
is **byte-equivalent at the pipeline boundary**, not at
per-NTT boundaries.

### Experiment (a) result — what the asm actually does

The butterfly macros (`macros_smallntt.i:76-84`) use
**`uadd16` / `usub16`** — unsigned packed 16-bit arithmetic
that wraps mod 2¹⁶ per half. So the running representation
is "uint16 modulo 2¹⁶, congruent to the true value mod Q",
**not** centred signed int16. Coefficients can drift in
`[0, 2¹⁶)` between layers; the invariant kept across layers
is modular equivalence mod Q, not a centred range.

### Why ml-dsa's wrap arithmetic doesn't apply to HAWK

The `uadd16` trick **only preserves mod-Q residue when no
actual wrap occurs**. A wrap by 2¹⁶ shifts the residue by
`2¹⁶ mod Q` — a nonzero offset that silently corrupts the
math.

  - ml-dsa: `q = 769`, `2¹⁶ mod q = 171`. Their coefficients
    are bounded by a few `q`s (~2¹¹), so `x + y` for any
    pair never reaches 2¹⁶ — wraps simply don't happen, and
    `uadd16` is exact integer add for them.
  - HAWK: `Q = 18433`, `2¹⁶ mod Q = 10237`. Initial canonical
    values fit `[1..Q]≈[1..2¹⁴]`. After **one butterfly**, the
    sum `x1 + x2` can reach `2·Q ≈ 36866 < 2¹⁶`, still safe.
    But after `x2 = plant_red(...) ∈ [1..Q]` is added to a
    previous-layer's wrap-form `x1` (which could already be
    in `[0, 2¹⁶)`), the sum overflows 2¹⁶ regularly. Each
    such wrap injects `+10237` into the running residue.

A single host-side check verifies the math
(`/tmp/dbg_wrap.c`):

    x = 50000  // wrap form, represents 13134 mod Q
    y = 18000  // canonical, mod Q is 18000
    true (x + y) mod Q = 12701
    uint16-wrap (x + y) = 2464, that mod Q = 2464
    offset introduced by the wrap = -10237 (= -(2¹⁶ mod Q))

So ml-dsa-style lazy-reduce Plantard isn't a drop-in for
HAWK's modulus. The constraint is `magnitude < 2¹⁶`, and
HAWK's `Q ≈ 2¹⁴` lets a single butterfly add break it.

### Why my Path A attempt failed (root cause)

My C attempt cast cells to `int16_t` (sign-extending any cell
with bit 15 set) and then used signed int32 arithmetic. That
introduced a **representation mismatch** with what the asm
would actually do (uint16 wrap arithmetic). The single-
coefficient `plant_red` was correct, the n=4 NTT happened to
work because no cell had bit 15 set, and divergence started at
n≥256 when post-butterfly values landed in cells with bit 15
set and subsequent reads sign-extended them into different
modular residues than the uint16 wrap would have.

The right C reference for Path A is:
  - Cells as `uint16_t a[N]` throughout (no int16 cast on read).
  - Add/sub as `(uint16_t)(a + b)` / `(uint16_t)(a - b)` — wraps
    mod 2¹⁶ per the same semantics as `uadd16`/`usub16`.
  - `plant_red` returns a value to be **packed back into uint16**
    via the same wrap (matching `pkhtb \a, \a, \tmp, asr#16` in
    the asm).
  - No "centred_reduce" inside the loop — drift is allowed.
  - A **final** reduction pass at iNTT exit (analog of
    `small_invntt_tomont`) converts back to canonical `[1..Q]`.
  - Plantard-aware pointwise helpers — composed
    `mq18433_basemul_plant(w1, w2)` (combines `montymul`-equivalent
    for the pointwise loop), `mq18433_sub_plant`,
    `mq18433_tomonty_plant` — mirroring `small_point_mul` /
    `small_asymmetric_mul` from ml-dsa.

This rewrites my attempt cleanly. The test changes from
"per-NTT byte equality" to "**after full pipeline** byte
equality" (e.g., compare `mq18433_NTT(w);  pointwise;
mq18433_iNTT(w)` against `mq18433_NTT_plant(w);  pointwise_plant;
mq18433_iNTT_plant(w)`, byte-equal at the end).

## Updated recommendation (after experiment a)

ml-dsa's Plantard NTT architecture (lazy reduction with
uint16-wrap arithmetic) **does not directly port to HAWK**
because HAWK's `Q ≈ 2¹⁴` is too large relative to the 2¹⁶
storage width: wraps happen routinely, and each wrap injects
a `2¹⁶ mod Q = 10237` offset into the mod-Q residue. A
HAWK-specific Plantard variant would need one of:

  - **Wider intermediate storage (int32 / uint32 per
    coefficient)**: no wrap issue, but the packed-pair
    `smulwb / smulwt` advantage is gone (one coefficient per
    register instead of two). Per-butterfly cycle count grows
    proportionally; the asm port is no longer dramatically
    faster than the reference C.
  - **Per-layer canonical reduction (= Path B)**: each
    butterfly's add/sub reduces to canonical `[1..Q]`. The C
    body becomes byte-identical to `Zq(NTT)` by construction;
    the asm benefits only from the packed-pair `plant_red`
    twiddle multiply, not from the running-state packing. Cycle
    count is ~Path-B from the earlier analysis (~25 % slower
    than Path A would have been).
  - **A novel reduction algorithm** tuned for HAWK's specific
    modulus (e.g., signed Plantard with explicit overflow
    handling, or a variant of Solinas / Barrett that exploits
    `Q = 2¹⁴ + 2¹¹ + 1`). This is research; not appropriate
    for a single-paper deliverable.

**Pragmatic recommendation for the v1 paper**: take **Path B**.
Accept the ~25 % cycle cost relative to the (hypothetical) Path
A. The numbers should still beat the reference-C baseline by a
substantial margin because the asm gets to use the `plant_red`
twiddle multiply (which is the actual algorithmic win — 2-3
instructions per coefficient versus ~6-7 for the C `montymul`)
even when the running state is canonical. The packed-pair
register residency is what we give up, not the reduction
algorithm itself.

A v2 paper can revisit Path A using one of the routes above
(wider storage, or research) if the v1 numbers warrant the
follow-up.

## Plan (next iteration)

Take Path B for HAWK-m4 v1. Concrete steps:

1. ~~Lock the kernel encoding (`Q0I = 3955247103`, `qa = 1<<16`).~~ ✅

2. ~~Wire `HAWK_PLANT_NTT=1` macro layer into `hawk_sign.c`.~~ ✅

3. ~~Cross-check infrastructure (`tests/test_plant_ntt.c`).~~ ✅
   Currently byte-identical-by-construction (the C `plant_*`
   functions are line-for-line copies of `Zq(*)`). That's the
   right state for Path B.

4. **Write the M4 asm** `plant_18433_cm4.S` using:
     - canonical `[1..Q]` cells throughout (storage matches HAWK
       expectations; the existing cross-check at NTT boundaries
       remains byte-for-byte),
     - the locked-down 3-instruction `mul / lsr / smlatb`
       `plant_red` kernel (byte-identical to `Zq(montyred)` per
       calibration),
     - explicit canonical add/sub at each butterfly — about 5
       instructions per packed half for the conditional-`+Q`
       adjustment (`usub16 / sel / uadd16 / usub16`-style on
       Cortex-M4).

   Without packed-pair register residency, each butterfly is
   ~8–10 instructions instead of Path A's ~5.5; expect about
   3-4× speedup over the reference C `mq18433_NTT`, not the
   ~5× Path A would have given.

5. **Benchmark on NUCLEO-L4R5ZI** using pqm4's `speed_test`
   harness; compare to the upstream-C baseline and to ml-dsa
   numbers as a sanity check.

6. **v2 paper (deferred)**: explore wider-storage Plantard
   (int32 cells) or a HAWK-specific reduction to recover the
   Path A speedup. This is research, not engineering — defer
   until v1 lands.

`hawk_sign.c` ships with `HAWK_PLANT_NTT` undefined by default,
so the production path remains upstream `mq18433_*`. The flag
flips it on once on-target measurements justify the switch.
