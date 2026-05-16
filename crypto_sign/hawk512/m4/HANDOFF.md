# HANDOFF — HAWK on Cortex-M4 (pqm4 submission)

Read this first. It supersedes any quick-look at the commit log.

## 1. Goal (one line)

First optimised Cortex-M4 implementation of HAWK (keygen, sign,
verify) for the pqm4 benchmarking framework, targeting NIST PQC
2027 constrained-device evaluation.

## 2. Where everything lives

| Thing | Path / URL |
|-------|-----------|
| Working tree | `/home/user/rust/pqm4` (this repo) |
| Fork repo | <https://github.com/amit0365/pqm4> |
| Branch | `hawk-m4-asm` (off `master`) |
| Push status | **Not pushed.** The sandbox has no GitHub credentials; commits are local-only. The user must push from their own machine, or the next agent must set up auth. |
| Commit signing | All commits in this branch are **unsigned** (`-c commit.gpgsign=false`). The sandbox's signing server is scoped to `amit0365/evm-asm` only. |
| HAWK source provenance | <https://github.com/hawk-sign/dev> (MIT, © 2022 Hawk Project, Pornin & Pulles). Cloned at `/tmp/hawk-sign-dev/`, generated NIST trees at `/tmp/hawk-sign-dev/NIST/Reference_Implementation/hawk{512,1024}/`. **Already vendored** into `crypto_sign/hawk512/m4/`. |
| Plantard NTT reference (M4 asm) | `crypto_sign/ml-dsa-44/m4f/{macros_smallntt.i, smallntt_769.S}` — Huang et al. TCHES 2024, vendored from UIC-ESLAS/Dilithium-Multi-Moduli. Re-usable template if Path B asm is written. |
| Plantard reference (C) | `/tmp/ImprovedPlantardArithmetic/` (UIC-ESLAS, q=3329 Kyber port) |
| NTRUgen (for keygen) | Already vendored inside HAWK's `ng_*.{c,h}` files — no separate integration needed. |

## 3. What's done (chronological commits on `hawk-m4-asm`)

```
f1f8df2 Plantard NTT: experiment (a) follow-up — ml-dsa's wrap arithmetic doesn't fit HAWK's Q
88a8239 Plantard NTT: experiment (a) result — ml-dsa uses pipeline equivalence, not per-NTT byte equality
ad9001d Plantard NTT: record the Path A dead-end and reset to Path B baseline
229e94e Plantard NTT: portable C plant path + cross-check + HAWK_PLANT_NTT flag
92d2152 Plantard NTT: lock the encoding via calibration
31f6b25 Plantard NTT: design note and calibration test scaffold
bb3b0ef Add host cross-check tests for the SHAKE/SHA3 glue
4afd73c SHAKE-256: route HAWK's sha3 API to pqm4's Keccak asm
b254828 Vendor HAWK reference C as pqm4 m4 baseline
2f546e0 Add HAWK-512 and HAWK-1024 m4 scheme scaffolds
```

### What's verified and working on host

- **Baseline pipeline**: end-to-end keygen → sign → verify roundtrip
  passes for HAWK-512 (and the same code parameterised for HAWK-1024).
  Smoke test at `/tmp/hawk_smoke.c` (regenerate from the version
  inside commit messages if lost).
- **SHAKE-256 swap**: `sha3.c` now a 150-line shim onto pqm4's
  `KeccakF1600_State{Permute,XORBytes,ExtractBytes}`. Verified
  byte-identical to pqm4's `shake256/sha3_256/384/512` across
  576+ test combinations.
- **`HAWK_PLANT_NTT=1` flag**: wired into `hawk_sign.c`, macro-
  redirects all 15 `mq18433_*` call sites to the `_plant`
  variants. Default (flag undefined) is unchanged.
- **Cross-check infrastructure**: `tests/test_plant_ntt.c`
  currently passes trivially because `plant_18433.c` is a
  line-for-line copy of the reference NTT (Path B structural
  form). The test is the regression net for any future asm port.

### Test commands (all run from `/home/user/rust/pqm4/`)

```
make -C crypto_sign/hawk512/m4/tests build
make -C crypto_sign/hawk512/m4/tests run             # SHAKE/SHA3 glue
make -C crypto_sign/hawk512/m4/tests run-calibrate   # Plantard kernel
make -C crypto_sign/hawk512/m4/tests run-plant       # full NTT byte equality
make -C crypto_sign/hawk512/m4/tests run-twiddle     # twiddle encoding search
```

All four currently exit 0 (the calibration test exits 0 once a
matching combo exists in its array; today `v6+encId` matches).

## 4. The Plantard NTT saga (read PLANTARD_NOTES.md first)

`crypto_sign/hawk512/m4/PLANTARD_NOTES.md` is the canonical
record. The short version:

1. **HAWK's actual modulus is `Q = 18433`** in `hawk_sign.c`
   (only file that uses `mq18433_*`). hawk_vrfy.c and hawk_kgen.c
   use 31-bit primes — outside Plantard's sweet spot, not in
   scope.
2. **Plantard kernel locked**: 3-instruction `mul / lsr / smlatb`
   reducer with `Q0I = 3955247103` and `qa = 1 << 16`. This is
   byte-identical to HAWK's `Zq(montyred)` — verified by
   `tests/test_plantard_calibrate.c` over 4096 random `(a, b)`
   pairs.
3. **Path A failed twice**. Both failures are now documented.
   First attempt: signed centred-form arithmetic. Failed because
   the int16 cast on cell read sign-extended bit-15-set cells.
   Second attempt (after experiment a): uint16-wrap arithmetic
   mirroring ml-dsa. Failed because `2^16 mod Q = 10237 ≠ 0`,
   and wraps happen routinely with HAWK's `Q ≈ 2¹⁴`. Each wrap
   silently injects `+10237` into the mod-Q residue.
4. **Conclusion: take Path B for v1.** Canonical-form NTT
   throughout — the asm still wins via the Plantard twiddle
   multiply kernel (~3 cycles vs C's ~6–7), but uses explicit
   canonical add/sub per butterfly (~5 extra cycles per packed
   half). Expected net speedup vs reference C: ~3–4× instead of
   the hypothetical ~5× from Path A.

### Pitfalls a fresh agent will trip over

- **`modq.h` `#undef`s `Q`, `Q0I`, `R`, `R2`, `MSF`, `Q0Ilo`,
  `Q0Ihi`, `Zq`, `MQ_UNUSED` at the bottom**. Any file that
  uses these constants after the include must redefine them.
  See `plant_18433.c` and the tests for the pattern.
- **Block-comments and `*/` in URLs**: `ml-dsa-*/m4f/` inside
  a `/* */` block closes the comment early. Use
  `ml-dsa-{44,65,87}/m4f/` instead.
- **`int16_t` vs `uint16_t` cast on cell read**: HAWK stores
  coefficients in canonical `[1..Q]` as `uint16_t`. Casting
  through `(int16_t)` sign-extends values ≥ 32768 to negative
  int32. This breaks the Plantard math silently.
- **uint16 wrap**: `(uint16_t)(x + y)` wraps mod 2¹⁶, not mod Q.
  These are not the same when `2¹⁶ mod Q ≠ 0` (which is HAWK's
  case). Don't use uint16 wrap arithmetic for HAWK's NTT.
- **`smulwb` / `smulwt` are SIGNED multiplies**. They sign-
  extend the 16-bit operand. For HAWK's canonical `[1..Q]` (all
  < 2¹⁵) this is fine, but anything in `[2¹⁵, 2¹⁶)` would be
  treated as negative.

## 5. The four-item plan from the original brief

Status of each item the user originally asked for (updated 2026-05-16):

| # | item | status |
|---|------|--------|
| 1 | `plant_18433_cm4.S` kernel | ✅ **Done (scalar v1)**. Cross-checks byte-identical to `Zq(NTT/iNTT/montymul)` across logn = 1..10 under QEMU mps2-an386. See `tests/m4/`. Packed-pair optimization deferred to v2 (see §6 below). |
| 2 | Wire into `hawk_sign.c` behind `HAWK_PLANT_NTT=1` | ✅ Done (commit `229e94e`). |
| 3 | Cross-check `mq18433_NTT/iNTT` byte-for-byte vs plant | ✅ **Done for both C path AND asm path**. Host test (`tests/test_plant_ntt.c`) covers C fallback; QEMU test (`tests/m4/test_ntt.c`) covers asm. Both report `ALL TESTS PASSED`. |
| 4 | Benchmark on NUCLEO-L4R5ZI via `speed_test` | **Not done.** Needs hardware. Build instructions in §6.5 below. |

## 6. What's already implemented in `plant_18433_cm4.S` (and what's next)

**Current state (2026-05-16)**: Scalar v1 is in place and cross-checked.

- `mq18433_montymul_plant`: 5-instruction Plantard kernel (mul/mul/lsr/mla/lsr)
  plus constant loads + return. Verified byte-identical to `Zq(montymul)` over
  4096 random + 8 spot test cases.
- `mq18433_NTT_plant`: scalar Cooley-Tukey forward NTT. Per-coefficient
  butterfly. Verified byte-identical to `Zq(NTT)` for logn = 1..10 (HAWK-512
  AND HAWK-1024).
- `mq18433_iNTT_plant`: scalar Gentleman-Sande inverse NTT. Verified
  byte-identical to `Zq(iNTT)` for logn = 1..10.

**Verification harness**: `crypto_sign/hawk512/m4/tests/m4/` — bare-metal
Cortex-M4 ELF for QEMU `mps2-an386` board with semihosting. Run via:
```
make -C crypto_sign/hawk512/m4/tests/m4 run        # montymul test
make -C crypto_sign/hawk512/m4/tests/m4 run-ntt    # NTT + iNTT test
make -C crypto_sign/hawk512/m4/tests/m4 run-bench  # asm vs C-ref benchmark
```
Requires `arm-none-eabi-gcc` and `qemu-system-arm` (both available via
homebrew on macOS).

**Benchmark results (QEMU, SysTick proxy under -icount shift=0)**:

| op   | logn | C-ref | asm  | speedup |
|------|------|-------|------|---------|
| NTT  | 6    | 133   | 102  | 23%     |
| iNTT | 6    | 145   | 120  | 17%     |
| NTT  | 9 (HAWK-512)  | 1483  | 1154 | 22%     |
| iNTT | 9 (HAWK-512)  | 1643  | 1372 | 16%     |
| NTT  | 10 (HAWK-1024) | 3257  | 2536 | 22%     |
| iNTT | 10 (HAWK-1024) | 3615  | 3023 | 16%     |

Numbers are SysTick ticks under `-icount shift=0` — roughly 40 emulated
instructions per tick on mps2-an386. NOT pipeline-accurate (QEMU doesn't
model M4 dual-issue, flash waits, or prefetch). The RATIOS should
track real-silicon RATIOS reasonably well; absolute numbers won't.

**Caveat**: ~20% is the modest-scalar-asm ceiling. Compiler-generated
C with inlined montymul/add/sub already runs ~21-25 instructions per
butterfly, and our scalar asm shaves ~3-5 instructions. The 3-4× target
in the original design needs packed-pair (see v2 below).

**Next: packed-pair optimization (v2)**. The scalar version is correct but
modest in speedup — roughly parity with inlined C, since the per-butterfly
cycle count is similar (~20 cycles per butterfly each). The real 3-4×
speedup HANDOFF targeted needs **packed-pair butterflies** processing 2
coefficients per iteration via `usub16` / `uadd16` / `sel` for canonical
add/sub. v2 work order:

1. Add a packed-pair inner loop that fires when `ht >= 2` (i.e., all but
   the last layer).
2. Keep the last layer (ht=1) on the scalar path.
3. Re-run the QEMU cross-check — it WILL catch any bugs the per-layer
   sweep covers.

**For the curious — original Path B spec, retained for reference**:

1. **Symbols to export**: `mq18433_NTT_plant`, `mq18433_iNTT_plant`,
   `mq18433_montymul_plant` (matching `plant_18433.h`).
2. **Storage convention**: `uint16_t a[]`, canonical `[1..Q]` per
   cell, in and out. No wrap-form drift.
3. **Inner reducer (per coefficient)**:
   ```
   mul     tmp, c, q0i               @ tmp = c · 3955247103 mod 2^32
   lsr     tmp, tmp, #16             @ tmp = h
   smlatb  out, tmp, q, qa_const     @ out_hi = h · Q + 65536
                                     @ qa_const literal = 1<<16; needs register
                                     @ q register loaded with Q in low half: movw q, #18433
   @ extract bits 16..31 of out for the modular value
   ```
   Or equivalent using `smultb` if the register packing differs.
   Validated against `Zq(montyred)` in `tests/test_plantard_calibrate.c`.
4. **Per-butterfly add/sub**: explicit canonical `[1..Q]` form.
   For packed pairs (two halves), use `usub16`/`uadd16` followed
   by `sel`/`uadd16` for the conditional `+Q` correction. About
   5 instructions per packed half.
5. **Twiddle table**: HAWK's existing `mq18433_GM[]` / `mq18433_iGM[]`
   (from `modq.h`) feeds the asm unchanged. No re-encoding needed.
   To access from asm, either:
   - Add a non-static alias `const uint16_t mq18433_GM_extern[] = ...;`
     in a small `.c` file, OR
   - Re-declare with `.extern` in the asm and add `__attribute__((used))`
     in C.
6. **Layer-merging**: optional first pass. ml-dsa's
   `smallntt_769.S` does 3+3+2 layer-merging for n=256; HAWK
   needs +1 layer for n=512, +2 for n=1024. Don't bother with
   layer-merging for v1 — a straightforward per-layer loop is
   fine. The win is the inner kernel, not the layer-merging.
7. **Compilation guard**: wrap the entire `.S` file in
   `#if defined(__ARM_FEATURE_DSP) && __ARM_FEATURE_DSP` so it
   only contributes symbols on M4 builds. Add the symmetric
   guard `#if !(defined(__ARM_FEATURE_DSP) && __ARM_FEATURE_DSP)`
   around `plant_18433.c`'s function bodies so the C and asm
   don't both provide the same symbol on M4.

### Verification cycle for the asm

1. Set up an ARM cross-toolchain (`arm-none-eabi-gcc`).
2. `make PLATFORM=nucleo-l4r5zi IMPLEMENTATION_PATH=crypto_sign/hawk512/m4 bin/crypto_sign_hawk512_m4_test.bin`.
3. Cross-compile the host test for the target (or run via QEMU).
4. Run `test_plant_ntt`. **It must report `ALL TESTS PASSED`** on
   the first try, or the asm has a bug. The expected first-pass
   bug count is 1–3 (typos, register confusion, stride off-by-one);
   each one will produce a specific "ref=X plant=Y" diagnostic that
   localises it.
5. Once cross-check is green, build the rest of pqm4 with
   `EXTRA_CFLAGS=-DHAWK_PLANT_NTT=1` and run `speed.elf` /
   `stack.elf`. Compare to `EXTRA_CFLAGS=` baseline.

## 7. Things deliberately *not* done (so the next agent doesn't re-do them)

- **Packed-pair NTT optimization (v2)**: deferred. Scalar v1 is correct
  and validates the kernel + canonical-form arithmetic. v2 is a refactor
  for speed, not correctness. See §6 above.
- **No `config.mk`** inside `crypto_sign/hawk{512,1024}/m4/` yet. pqm4
  picks up `config.mk` per scheme if it exists; set `HAWK_PLANT_NTT=1`
  there (via `CPPFLAGS += -DHAWK_PLANT_NTT=1`). Once v2 lands AND
  on-target benchmarks confirm the win, flip the default to on.
- **No actual on-target benchmarks.** QEMU cycle counts are not
  representative — Cortex-M4 cycle accuracy in QEMU is best-effort, and
  flash wait states, instruction prefetch, and bus contention on real
  silicon will move the numbers. Run `speed.elf` on NUCLEO-L4R5ZI for
  real numbers.
- **Q0I constant typo in earlier HANDOFF**: the previous draft of §4
  / §6 referred to `Q0I = 3955247103 = 0xEBC97FFF`. The correct hex is
  `0xEBC047FF`. The math (3955247103 decimal) is correct everywhere
  except that one stray hex value, and the asm uses the correct
  constant. Mentioned here in case a future agent searches for the bad
  hex.

## 8. Files in the m4 directory (current layout)

| File | Status | Notes |
|------|--------|-------|
| `plant_18433_cm4.S` | ✅ Scalar v1 in place | NTT, iNTT, montymul. Cross-checked. Packed-pair v2 pending. |
| `plant_18433_cm4_tables.c` | ✅ Created | Externalized copies of `mq18433_GM[]` / `mq18433_iGM[]`. Guarded by `__ARM_FEATURE_DSP` so they're emitted only on M4. |
| `plant_18433.c` | ✅ Guarded | C bodies wrapped in `#if !(__ARM_FEATURE_DSP)`. Host build uses C; M4 build uses asm. |
| `plant_18433.h` | Unchanged | Function prototypes for both impls. |
| `tests/test_plant_ntt.c` | Host cross-check | Passes byte-identical (C path). |
| `tests/m4/` | ✅ New | QEMU bare-metal harness: `qemu.ld`, `startup.S`, `semihost.h`, `test_montymul.c`, `test_ntt.c`, `Makefile`. |
| `hawk_sign.c` | Unchanged | `HAWK_PLANT_NTT` macro layer at top. Macro substitution applies to both C and asm paths. |
| `../../hawk1024/m4/plant_18433*` | ✅ Symlinked | hawk1024 picks up the same scalar v1 via symlinks to hawk512/m4. |

## 9. What to push when you finally have GitHub auth

```
git push -u origin hawk-m4-asm
```

Then open a PR against `master` of `amit0365/pqm4`, eventually
upstream to `mupq/pqm4` once benchmarks justify it.

## 10. Open questions worth flagging to the user

- **HAWK-1024**: most of the m4 dir is symlinked to hawk512/m4.
  Per-variant differences (LOGN, signature size constants) are
  in `api.h` / `api.c`. Verify nothing else needs to differ.
- **Verify path optimisation**: `hawk_vrfy.c` uses its own 31-bit-
  prime NTT (not `mq18433_*`). Plantard doesn't apply. If
  verify-512 < 300k cycles is a paper-critical target, the verify
  hot path needs separate analysis (probably Solinas / Mersenne
  reduction on the 31-bit prime, with packed 32-bit arithmetic).
  Out of scope for the current Plantard work.
- **NTRUgen keygen base case**: HAWK's vendored `ng_ntru.c`
  recursive NTRU solve has a base case that's the keygen hot
  spot. The user's original brief mentioned "M4 inline assembly
  for the recursive base case" but no work has been done on it
  yet. Lower priority than sign-path Plantard.

## 11. Quick reference: things that were USEFUL discoveries

- HAWK's `R = 2³² mod q` (not `2¹⁶`). `Zq(montyred)` is a 32→16
  reduction. So Plantard's natural `2⁻³²` output factor matches
  HAWK's Montgomery scaling — no twiddle re-baking at the math
  level.
- HAWK's `Q0I = 3955247103 = −q⁻¹ mod 2³²` (negative-sign
  convention). UIC-ESLAS Plantard uses `+q⁻¹ mod 2³²` instead.
  For byte equality with `Zq(montyred)`, use `Q0I`.
- ml-dsa's `small_ntt` is **not** mid-NTT byte-identical to its
  canonical reference. It's pipeline-equivalent. This is a
  feature of their architecture, not a bug.
- pqm4's optimised Keccak (`common/keccakf1600.S`) is the one
  every scheme should use. It exposes `KeccakF1600_StatePermute`,
  `KeccakF1600_StateXORBytes`, `KeccakF1600_StateExtractBytes`.
  HAWK's sha3.c now does.
- `(int16_t)v` for `uint16_t v` with `v >= 0x8000` sign-extends
  to negative `int32_t` after promotion. Always be explicit about
  signed vs unsigned arithmetic on cell reads.
