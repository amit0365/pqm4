# crypto_sign/hawk{512,1024}/m4 — implementation notes

## Source provenance

The HAWK core sources (`hawk_kgen.c`, `hawk_sign.c`, `hawk_vrfy.c`,
`hawk*.h`, `ng_*.{c,h}`, `modq.h`, `sha3.{c,h}`) are vendored
verbatim from the per-variant NIST package emitted by the
`build.py` script of:

    https://github.com/hawk-sign/dev   (MIT license,
    © 2022 Hawk Project, Pornin & Pulles)

Specifically, `dev/NIST/Reference_Implementation/hawk512/`. The HAWK
keygen subtree (`ng_*.{c,h}`) is the NTRUgen sampler/solver
embedded in HAWK (Pornin, eprint 2025/1239), already inlined into the
HAWK distribution; no separate ntrugen clone is needed.

Files **omitted** from the vendoring:
- `Makefile`, `Makefile.win32` — pqm4 supplies its own build rules.
- `PQCgenKAT_sign.c`, `rng.c`, `rng.h` — pqm4 provides its own test
  harness and `randombytes()`.

Files **added** by us:
- `api.h`, `api.c` — adapted from `dev/extra/{api.h,api.c}`, with
  `unsigned long long` swapped to `size_t` (pqm4 convention) and
  `randombytes()` plumbed to pqm4's PRNG.

## Cross-variant source sharing

`crypto_sign/hawk1024/m4/` symlinks every shared `.c`/`.h` file to
`../../hawk512/m4/` (matching the ml-dsa-44/65/87 pattern in
upstream pqm4). The only per-variant files are `api.h` (different
`CRYPTO_*BYTES`) and `api.c` (different `LOGN`).

## Optimisation roadmap

This commit is the **reference C baseline** — every entry point now
runs the full HAWK protocol end-to-end on Cortex-M4. The next steps
swap targeted hot paths for M4-tuned versions:

1. ~~**SHAKE-256 → pqm4 assembly.**~~ ✅ Done.
   `sha3.c` is now a ~150-line shim onto pqm4's
   `KeccakF1600_State{Permute,XORBytes,ExtractBytes}` (provided by
   `libsymcrypto.a` built from `common/keccakf1600.S`). The 1.2 k-line
   Cortex-M4 Keccak inline-asm shipped by hawk-sign/dev is removed; the
   permutation is now shared with every other pqm4 m4 scheme. The
   public `shake_context` ABI is preserved (same struct layout, same
   entry-point names) so no other HAWK source needed to change.
   End-to-end host smoke test (keygen → sign → verify roundtrip) still
   passes, confirming hash-output equivalence with the upstream
   reference.

2. **Plantard NTT for HAWK's modulus.** HAWK keygen and verify use
   small-prime NTTs (`modq.h`, default Q = 18433) for polynomial
   multiplication. Port the Plantard reduction technique from
   <https://github.com/UIC-ESLAS/ImprovedPlantardArithmetic> —
   register-packed dual-coefficient butterflies via `smlad`, with
   layer-merged NTT passes and lazy reduction. Q = 18433 fits in
   a halfword so the same packing strategy applies.

3. **NTRUgen recursive base case.** `ng_ntru.c`'s recursive NTRU
   solve calls into the small-prime NTT layer (`ng_mp31.c`). The
   leaf recursion (small-degree polynomial Bezout) is the keygen
   hot-spot — replace it with hand-written M4 inline assembly,
   exploiting register pairs and `umaal`.

4. **Isochronous discrete-Gaussian sampler.** HAWK's sampler is
   centred on 0 or 1/2 with two small standard deviations; a
   constant-time CDT table fits comfortably and lets us advertise
   a TIMA-friendly sign primitive.

## Targets (STM32L4R5ZI, NUCLEO-L4R5ZI)

| operation  | aspirational | publishable threshold |
|------------|--------------|-----------------------|
| sign-512   |   < 500 k    |   < 600 k             |
| verify-512 |   < 300 k    |   < 350 k             |
| sign-1024  |   < 1.1 M    |   < 1.3 M             |
| verify-1024|   < 650 k    |   < 750 k             |

If overall speedup over the reference C drops below 2×, pivot the
work towards side-channel / fault-injection analysis on this same
codebase (Stage 2).

## License

MIT, inherited from the upstream HAWK project. See `LICENSE.txt` in
`https://github.com/hawk-sign/dev` for the full text.
