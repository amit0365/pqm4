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

## Plan (next iteration)

Step 1 (lock the encoding) is done — see the table above. Remaining:

1. ~~**Lock the encoding.**~~ ✅ — use `Q0I = 3955247103` with
   identity twiddle encoding; the existing `GM[]` / `iGM[]` tables
   are reusable as-is.
2. **Implement `plant_18433.S`** (M4 asm) using the three-instruction
   `mul / lsr / smlabt+rounding` reducer derived above, packed-pair
   form via `smulwb / smulwt / smlabt / smlabt` for two coefficients
   per butterfly. Layer-merge 2–3 layers per pass for register
   residency.
3. **Drop in NTT/iNTT replacements** behind a build flag
   `HAWK_PLANT_NTT=1` (default off until we have on-target
   measurements). The 15 call sites in `hawk_sign.c` get switched
   en bloc.
4. **Extend the cross-check test** to compare a full
   `mq18433_NTT(a) / mq18433_iNTT(a)` byte-for-byte against the
   Plantard versions over many random polynomials.
5. **Benchmark on NUCLEO-L4R5ZI** using pqm4's `speed_test`
   harness, compare to the upstream-C baseline.

Until step 2 lands, **`hawk_sign.c` keeps calling upstream
`mq18433_*`** — current commit is design + calibration only,
behaviour is unchanged.
