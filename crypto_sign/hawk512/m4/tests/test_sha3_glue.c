/*
 * Host-only cross-check of the SHAKE / SHA3 glue in
 * crypto_sign/hawk512/m4/sha3.c against pqm4's reference one-shot
 * SHAKE/SHA3 (mupq/common/fips202.c).
 *
 * Both implementations sit on top of the same KeccakF1600_State*
 * primitives, so a byte mismatch is a bug in our wrapper layer.
 *
 * Tests:
 *   1. sha3_256 / sha3_384 / sha3_512 over a range of input lengths.
 *   2. shake256 streaming (init+inject+flip+extract) vs one-shot,
 *      with absorb/squeeze split at varying chunk boundaries.
 *   3. shake_x4_extract_words: each lane matches 8-byte chunks
 *      extracted serially from four independent shake_contexts.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sha3.h"        /* our glue */
#include "fips202.h"     /* pqm4 reference */

/* Forward decls (pqm4 fips202.c). */
extern void shake256(uint8_t *out, size_t outlen,
                     const uint8_t *in,  size_t inlen);
extern void sha3_256(uint8_t *out, const uint8_t *in, size_t inlen);
extern void sha3_384(uint8_t *out, const uint8_t *in, size_t inlen);
extern void sha3_512(uint8_t *out, const uint8_t *in, size_t inlen);

static int failures = 0;

static void
hexdump(const char *label, const uint8_t *b, size_t n)
{
    fprintf(stderr, "  %s [%zu]: ", label, n);
    for (size_t i = 0; i < n && i < 32; i++) fprintf(stderr, "%02x", b[i]);
    if (n > 32) fprintf(stderr, "...");
    fprintf(stderr, "\n");
}

#define CHECK_EQ(name, expected, got, n) do {                       \
    if (memcmp((expected), (got), (n)) != 0) {                      \
        fprintf(stderr, "FAIL %s\n", (name));                       \
        hexdump("expected", (expected), (n));                       \
        hexdump("got     ", (got),      (n));                       \
        failures++;                                                 \
    }                                                               \
} while (0)

/* ---- Test 1: fixed-output SHA3 ---------------------------------- */

static void
fill_pattern(uint8_t *buf, size_t n, unsigned seed)
{
    for (size_t i = 0; i < n; i++) {
        buf[i] = (uint8_t)(0x6A * (i + 1) + seed * 31);
    }
}

static void
test_sha3_fixed(void)
{
    static const size_t lens[] = {0, 1, 31, 32, 33, 71, 72, 73, 135, 136, 137,
                                  200, 271, 272, 273, 1000};
    uint8_t in[2048];
    uint8_t a[64], b[64];

    for (size_t li = 0; li < sizeof lens / sizeof lens[0]; li++) {
        size_t L = lens[li];
        fill_pattern(in, L, (unsigned)L);

        /* SHA3-256: 32-byte digest. */
        sha3_context sc;
        sha3_init(&sc, 256);
        sha3_update(&sc, in, L);
        sha3_close(&sc, a);
        sha3_256(b, in, L);
        char name[64];
        snprintf(name, sizeof name, "sha3_256 len=%zu", L);
        CHECK_EQ(name, b, a, 32);

        /* SHA3-384: 48-byte digest. */
        sha3_init(&sc, 384);
        sha3_update(&sc, in, L);
        sha3_close(&sc, a);
        sha3_384(b, in, L);
        snprintf(name, sizeof name, "sha3_384 len=%zu", L);
        CHECK_EQ(name, b, a, 48);

        /* SHA3-512: 64-byte digest. */
        sha3_init(&sc, 512);
        sha3_update(&sc, in, L);
        sha3_close(&sc, a);
        sha3_512(b, in, L);
        snprintf(name, sizeof name, "sha3_512 len=%zu", L);
        CHECK_EQ(name, b, a, 64);
    }
    fprintf(stderr, "Test 1 (sha3_*) covered %zu input lengths.\n",
            sizeof lens / sizeof lens[0]);
}

/* ---- Test 2: streaming SHAKE-256 vs one-shot -------------------- */

static void
test_shake256_streaming(void)
{
    /* Sweep absorb-chunk and squeeze-chunk boundaries that cross the
     * 136-byte SHAKE-256 rate, including the edge cases right at and
     * just past rate, to catch off-by-one bugs in dptr handling. */
    static const size_t in_lens[]  = {0, 1, 135, 136, 137, 271, 272, 273, 500};
    static const size_t out_lens[] = {1, 32, 135, 136, 137, 271, 272, 1000};
    static const size_t chunks[]   = {1, 7, 33, 64, 135, 136, 137, 500};

    uint8_t in[1024], a[1024], b[1024];

    for (size_t ii = 0; ii < sizeof in_lens / sizeof in_lens[0]; ii++) {
        size_t IL = in_lens[ii];
        fill_pattern(in, IL, (unsigned)(IL ^ 0xA5));
        for (size_t oi = 0; oi < sizeof out_lens / sizeof out_lens[0]; oi++) {
            size_t OL = out_lens[oi];

            /* Reference: pqm4 shake256 one-shot. */
            shake256(b, OL, in, IL);

            for (size_t ci = 0; ci < sizeof chunks / sizeof chunks[0]; ci++) {
                size_t C = chunks[ci];

                shake_context sc;
                shake_init(&sc, 256);
                for (size_t off = 0; off < IL; off += C) {
                    size_t k = C; if (off + k > IL) k = IL - off;
                    shake_inject(&sc, in + off, k);
                }
                shake_flip(&sc);
                for (size_t off = 0; off < OL; off += C) {
                    size_t k = C; if (off + k > OL) k = OL - off;
                    shake_extract(&sc, a + off, k);
                }
                char name[80];
                snprintf(name, sizeof name,
                    "shake256 IL=%zu OL=%zu chunk=%zu", IL, OL, C);
                CHECK_EQ(name, b, a, OL);
            }
        }
    }
    fprintf(stderr, "Test 2 (streaming shake256) covered "
        "%zu x %zu x %zu = %zu (IL, OL, chunk) combinations.\n",
        sizeof in_lens / sizeof in_lens[0],
        sizeof out_lens / sizeof out_lens[0],
        sizeof chunks / sizeof chunks[0],
        (sizeof in_lens / sizeof in_lens[0]) *
        (sizeof out_lens / sizeof out_lens[0]) *
        (sizeof chunks / sizeof chunks[0]));
}

/* ---- Test 3: shake_x4 self-consistency -------------------------- */

static uint64_t
load_u64_le(const uint8_t *p)
{
    uint64_t w = 0;
    for (int i = 0; i < 8; i++) w |= (uint64_t)p[i] << (8 * i);
    return w;
}

static void
test_shake_x4(void)
{
    /* Build four independent SHAKE-256 contexts with distinct inputs,
     * then compare shake_x4_extract_words() output against four serial
     * shake_extract() calls (8 bytes at a time, reinterpreted LE). */
    static const size_t num_x4_cases[] = {1, 5, 8, 17, 32, 65};
    uint8_t in[4][200];

    for (int i = 0; i < 4; i++) fill_pattern(in[i], 200, (unsigned)(i * 13 + 1));

    for (size_t ci = 0; ci < sizeof num_x4_cases / sizeof num_x4_cases[0]; ci++) {
        size_t N = num_x4_cases[ci];

        /* Reference: four serial shake_contexts, extracted 8 bytes each. */
        uint64_t *ref = malloc(4 * N * sizeof(uint64_t));
        for (int i = 0; i < 4; i++) {
            shake_context sc;
            shake_init(&sc, 256);
            shake_inject(&sc, in[i], 50 + 10 * i);
            shake_flip(&sc);
            for (size_t j = 0; j < N; j++) {
                uint8_t buf[8];
                shake_extract(&sc, buf, 8);
                ref[4 * j + i] = load_u64_le(buf);
            }
        }

        /* Glue path: shake_x4_flip + shake_x4_extract_words. */
        shake_context in_sc[4];
        for (int i = 0; i < 4; i++) {
            shake_init(&in_sc[i], 256);
            shake_inject(&in_sc[i], in[i], 50 + 10 * i);
        }
        shake_x4_context scx4;
        shake_x4_flip(&scx4, in_sc);
        uint64_t *got = malloc(4 * N * sizeof(uint64_t));
        shake_x4_extract_words(&scx4, got, N);

        char name[64];
        snprintf(name, sizeof name, "shake_x4 num_x4=%zu", N);
        if (memcmp(ref, got, 4 * N * sizeof(uint64_t)) != 0) {
            fprintf(stderr, "FAIL %s\n", name);
            for (size_t k = 0; k < 4 * N && k < 8; k++) {
                fprintf(stderr, "  word[%zu]: ref=%016llx got=%016llx %s\n",
                    k,
                    (unsigned long long)ref[k],
                    (unsigned long long)got[k],
                    ref[k] == got[k] ? "" : "<<");
            }
            failures++;
        }

        free(ref);
        free(got);
    }
    fprintf(stderr, "Test 3 (shake_x4) covered %zu num_x4 sizes.\n",
        sizeof num_x4_cases / sizeof num_x4_cases[0]);
}

int
main(void)
{
    test_sha3_fixed();
    test_shake256_streaming();
    test_shake_x4();
    if (failures == 0) {
        fprintf(stderr, "ALL TESTS PASSED.\n");
        return 0;
    } else {
        fprintf(stderr, "FAILED: %d mismatches.\n", failures);
        return 1;
    }
}
