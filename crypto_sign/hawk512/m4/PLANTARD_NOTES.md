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

## The open question: representation alignment

`hawk_sign.c` keeps NTT coefficients in **single-Montgomery** form
`x · R mod q` (with `R = 2¹⁶`), values stored in `[1..q]` (zero is
represented as `q`, not `0`). The current butterfly is

    montymul(a, GM[x]) = (a · GM[x]) · 2⁻¹⁶ mod q ∈ [1..q]

with `GM[x] = g^rev(x) · 2³² mod q` (so the table is **double-Mont**
of the actual root of unity).

A naive Plantard `plant_red(a · b) ≡ a · b · 2⁻³² mod q` is one
extra factor of `R = 2¹⁶` away from HAWK's single-Mont output. Two
options to bridge:

1. **Re-bake the twiddle table.** Replace
   `GM[x] = g^rev · 2³² mod q` with
   `GM_plant[x] ≡ g^rev · 2¹⁶ · Qinv_Plant mod 2³²` (a 32-bit value),
   so that `plant_red(a · GM_plant[x])` lands in single-Mont. This
   is one-time table work; no other HAWK code changes.

2. **Switch HAWK to double-Mont representation.** Touches every
   `Zq(*)` function (`set_small`, `snorm`, `unorm`, the comparison
   in `hawk_vrfy.c`, the `poly_set_small` helpers in `hawk_sign.c`).
   Big blast radius, not preferred.

We will pursue (1). The exact encoding for `GM_plant[x]` — sign
convention, whether to include the rounding constant baked-in, etc.
— is what the calibration test (next section) pins down empirically.

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

## Plan (next iteration)

1. **Lock the encoding.** Run the calibration test, observe which
   `(plant_red variant, GM_plant encoding)` pair matches `montymul`
   over thousands of random inputs.
2. **Implement `plant_18433.{h,c}`** with portable C primitives
   (`plant_red`, `plant_mul`, `plant_tomonty`) plus the rebuilt
   `GM_plant[]` and `iGM_plant[]` tables.
3. **Drop in NTT/iNTT replacements** behind a build flag
   `HAWK_PLANT_NTT=1` (default off until we have on-target
   measurements). The 15 call sites in `hawk_sign.c` get switched
   en bloc.
4. **Extend the cross-check test** to compare the full per-coefficient
   NTT output `mq18433_NTT(a)` vs `mq18433_NTT_plant(a)`, byte-equal
   for every coefficient.
5. **Hand-schedule the M4 asm**: port UIC-ESLAS's `doubleplant` /
   `mul_twiddle_plant` packed-pair butterflies to a `plant_ntt_18433.S`,
   driven by the same `GM_plant[]` table. Layer-merge 2–3 NTT layers
   per pass for register-residency. Validate against the C reference
   on host before measuring on NUCLEO-L4R5ZI.

Until step 1 succeeds, **`hawk_sign.c` keeps calling upstream
`mq18433_*`** — this commit is design + scaffolding only, behavior
is unchanged.
