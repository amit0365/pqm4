/*
 * profile_hawk — top-level cycle profile of HAWK-512 keygen/sign/verify
 * under QEMU. Reports cycles per operation so we can see where to
 * optimize next (Phase 1 of profiling — coarse breakdown).
 *
 * Build with HAWK_PLANT_NTT=1 (asm NTT path). Run via:
 *   make -C tests/m4 run-profile
 *
 * Note: cycles reported are SysTick ticks under -icount shift=0. NOT
 * pipeline-accurate M4 cycles. Use ratios for relative analysis;
 * absolute counts will differ on real silicon.
 */

#include <stdint.h>
#include <stddef.h>
#include "../../hawk.h"
#include "../../api.h"
#include "semihost.h"

#define LOGN 9

extern void randombytes(unsigned char *out, size_t outlen);
extern void randombytes_reseed(uint32_t seed);

static unsigned char priv[CRYPTO_SECRETKEYBYTES];
static unsigned char pub [CRYPTO_PUBLICKEYBYTES];
static unsigned char sig [CRYPTO_BYTES];
static unsigned char tmp_kgen[12288] __attribute__((aligned(16)));
static unsigned char tmp_sign[4096]  __attribute__((aligned(16)));
static unsigned char tmp_vrfy[6144]  __attribute__((aligned(16)));

static void hrng(void *ctx, void *dst, size_t len) {
    (void)ctx;
    randombytes((unsigned char *)dst, len);
}

/* ---- SysTick cycle counter (same approach as bench_ntt) ---- */
#define SYST_CSR    (*(volatile uint32_t *)0xE000E010)
#define SYST_RVR    (*(volatile uint32_t *)0xE000E014)
#define SYST_CVR    (*(volatile uint32_t *)0xE000E018)
#define SYST_MAX    0x00FFFFFFu

volatile uint32_t systick_wrap_count;
void SysTick_Handler(void) { systick_wrap_count++; }

static inline void cyc_init(void) {
    SYST_CSR = 0;
    SYST_RVR = SYST_MAX;
    SYST_CVR = 0;
    systick_wrap_count = 0;
    SYST_CSR = (1u << 0) | (1u << 1) | (1u << 2);  /* enable + tickint + clksource */
    while (SYST_CVR == 0) { /* wait for reload */ }
}

typedef struct { uint32_t wraps; uint32_t cvr; } cyc_snap_t;
static inline cyc_snap_t cyc_snap(void) {
    cyc_snap_t s;
    asm volatile ("cpsid i" ::: "memory");
    uint32_t cvr_a = SYST_CVR;
    uint32_t wraps = systick_wrap_count;
    uint32_t cvr_b = SYST_CVR;
    if (cvr_b > cvr_a) {
        wraps = systick_wrap_count;
        s.cvr = cvr_b;
    } else {
        s.cvr = cvr_a;
    }
    s.wraps = wraps;
    asm volatile ("cpsie i" ::: "memory");
    return s;
}

static inline uint64_t cyc_elapsed(cyc_snap_t t0, cyc_snap_t t1) {
    uint32_t wraps = t1.wraps - t0.wraps;
    int32_t  cvr_delta = (int32_t)t0.cvr - (int32_t)t1.cvr;
    if (cvr_delta < 0) {
        wraps += 1;
        cvr_delta += (int32_t)(SYST_MAX + 1);
    }
    return (uint64_t)wraps * ((uint64_t)SYST_MAX + 1u)
         + (uint64_t)cvr_delta;
}

/* External (non-inline) variants callable from hawk_sign.c when HAWK_PROFILE
 * is defined. The struct layout matches hawk_sign.c's forward declaration. */
cyc_snap_t cyc_snap_ext(void) { return cyc_snap(); }
uint64_t cyc_elapsed_ext(cyc_snap_t t0, cyc_snap_t t1) {
    return cyc_elapsed(t0, t1);
}

/* ============================================================
 * Function wrappers for cycle profiling (-Wl,--wrap=fn).
 * Each wrapper snaps cycles, calls __real_<fn>, snaps again,
 * accumulates elapsed cycles + call count.
 * ============================================================ */

#define PROF_VOID(name, arg_decl, arg_call) \
    extern void __real_##name arg_decl; \
    uint64_t prof_##name##_cyc; \
    uint32_t prof_##name##_calls; \
    void __wrap_##name arg_decl { \
        cyc_snap_t _t0 = cyc_snap(); \
        __real_##name arg_call; \
        cyc_snap_t _t1 = cyc_snap(); \
        prof_##name##_cyc += cyc_elapsed(_t0, _t1); \
        prof_##name##_calls += 1; \
    }

#define PROF_U32(name, arg_decl, arg_call) \
    extern uint32_t __real_##name arg_decl; \
    uint64_t prof_##name##_cyc; \
    uint32_t prof_##name##_calls; \
    uint32_t __wrap_##name arg_decl { \
        cyc_snap_t _t0 = cyc_snap(); \
        uint32_t _r = __real_##name arg_call; \
        cyc_snap_t _t1 = cyc_snap(); \
        prof_##name##_cyc += cyc_elapsed(_t0, _t1); \
        prof_##name##_calls += 1; \
        return _r; \
    }

PROF_VOID(mq18433_NTT_plant,         (unsigned logn, uint16_t *a), (logn, a))
PROF_VOID(mq18433_NTT_pair_plant,    (unsigned logn, uint16_t *a, uint16_t *b), (logn, a, b))
PROF_VOID(mq18433_iNTT_plant,        (unsigned logn, uint16_t *a), (logn, a))
PROF_U32 (mq18433_montymul_plant,    (uint32_t x, uint32_t y),     (x, y))
PROF_VOID(KeccakF1600_StatePermute,  (uint64_t *state), (state))
PROF_VOID(KeccakF1600_StateXORBytes, (uint64_t *state, const unsigned char *data, unsigned int offset, unsigned int length), (state, data, offset, length))
PROF_VOID(KeccakF1600_StateExtractBytes, (uint64_t *state, unsigned char *data, unsigned int offset, unsigned int length), (state, data, offset, length))

/* Sign-internal hot functions (static removed in hawk_sign.c). */
#define PROF_RET(rettype, name, arg_decl, arg_call) \
    extern rettype __real_##name arg_decl; \
    uint64_t prof_##name##_cyc; \
    uint32_t prof_##name##_calls; \
    rettype __wrap_##name arg_decl { \
        cyc_snap_t _t0 = cyc_snap(); \
        rettype _r = __real_##name arg_call; \
        cyc_snap_t _t1 = cyc_snap(); \
        prof_##name##_cyc += cyc_elapsed(_t0, _t1); \
        prof_##name##_calls += 1; \
        return _r; \
    }

PROF_VOID(extract_lowbit, (unsigned logn, uint8_t *f2, const int8_t *f), (logn, f2, f))
PROF_VOID(basis_m2_mul, (unsigned logn, uint8_t *t0, uint8_t *t1,
        const uint8_t *h0, const uint8_t *h1, const uint8_t *f2,
        const uint8_t *g2, const uint8_t *F2, const uint8_t *G2, uint8_t *xx),
        (logn, t0, t1, h0, h1, f2, g2, F2, G2, xx))
PROF_RET(uint32_t, sig_gauss,
        (unsigned logn, void (*rng)(void *ctx, void *dst, size_t len),
         void *rng_context, void *sc_data, int8_t *x0, const uint8_t *t0),
        (logn, rng, rng_context, sc_data, x0, t0))
PROF_RET(uint32_t, sig_gauss_alt,
        (unsigned logn, void (*rng)(void *ctx, void *dst, size_t len),
         void *rng_context, int8_t *x0, const uint8_t *t0),
        (logn, rng, rng_context, x0, t0))
PROF_RET(int32_t, poly_symbreak, (unsigned logn, const int16_t *s), (logn, s))
PROF_RET(int, encode_sig,
        (unsigned logn, void *sig, size_t sig_len,
         const uint8_t *salt, size_t salt_len, const int16_t *s1),
        (logn, sig, sig_len, salt, salt_len, s1))

/* Verify-side counters (call-site instrumented in hawk_vrfy.c). */
uint64_t prof_mp_NTT_cyc;             uint32_t prof_mp_NTT_calls;
uint64_t prof_mp_NTT_autoadj_cyc;     uint32_t prof_mp_NTT_autoadj_calls;
uint64_t prof_fx32_FFT_cyc;           uint32_t prof_fx32_FFT_calls;
uint64_t prof_fx32_iFFT_cyc;          uint32_t prof_fx32_iFFT_calls;
uint64_t prof_decode_q00_cyc;         uint32_t prof_decode_q00_calls;
uint64_t prof_decode_q01_cyc;         uint32_t prof_decode_q01_calls;
uint64_t prof_decode_s1_cyc;          uint32_t prof_decode_s1_calls;

/* Keygen-side counters (call-site instrumented in ng_hawk.c). */
uint64_t prof_solve_NTRU_cyc;         uint32_t prof_solve_NTRU_calls;
uint64_t prof_vect_FFT_cyc;           uint32_t prof_vect_FFT_calls;
uint64_t prof_vect_iFFT_cyc;          uint32_t prof_vect_iFFT_calls;
uint64_t prof_recover_G_cyc;          uint32_t prof_recover_G_calls;

static void prof_reset(void) {
    prof_mq18433_NTT_plant_cyc = 0;       prof_mq18433_NTT_plant_calls = 0;
    prof_mq18433_NTT_pair_plant_cyc = 0;  prof_mq18433_NTT_pair_plant_calls = 0;
    prof_mq18433_iNTT_plant_cyc = 0;      prof_mq18433_iNTT_plant_calls = 0;
    prof_mq18433_montymul_plant_cyc = 0;  prof_mq18433_montymul_plant_calls = 0;
    prof_KeccakF1600_StatePermute_cyc = 0;     prof_KeccakF1600_StatePermute_calls = 0;
    prof_KeccakF1600_StateXORBytes_cyc = 0;    prof_KeccakF1600_StateXORBytes_calls = 0;
    prof_KeccakF1600_StateExtractBytes_cyc = 0; prof_KeccakF1600_StateExtractBytes_calls = 0;
    prof_extract_lowbit_cyc = 0;       prof_extract_lowbit_calls = 0;
    prof_basis_m2_mul_cyc = 0;         prof_basis_m2_mul_calls = 0;
    prof_sig_gauss_cyc = 0;            prof_sig_gauss_calls = 0;
    prof_sig_gauss_alt_cyc = 0;        prof_sig_gauss_alt_calls = 0;
    prof_poly_symbreak_cyc = 0;        prof_poly_symbreak_calls = 0;
    prof_encode_sig_cyc = 0;           prof_encode_sig_calls = 0;
    prof_mp_NTT_cyc = 0;               prof_mp_NTT_calls = 0;
    prof_mp_NTT_autoadj_cyc = 0;       prof_mp_NTT_autoadj_calls = 0;
    prof_fx32_FFT_cyc = 0;             prof_fx32_FFT_calls = 0;
    prof_fx32_iFFT_cyc = 0;            prof_fx32_iFFT_calls = 0;
    prof_decode_q00_cyc = 0;           prof_decode_q00_calls = 0;
    prof_decode_q01_cyc = 0;           prof_decode_q01_calls = 0;
    prof_decode_s1_cyc = 0;            prof_decode_s1_calls = 0;
    prof_solve_NTRU_cyc = 0;           prof_solve_NTRU_calls = 0;
    prof_vect_FFT_cyc = 0;             prof_vect_FFT_calls = 0;
    prof_vect_iFFT_cyc = 0;            prof_vect_iFFT_calls = 0;
    prof_recover_G_cyc = 0;            prof_recover_G_calls = 0;
}

static void semi_write_u64(uint64_t v) {
    char buf[21];
    int n = 0;
    if (v == 0) { buf[n++] = '0'; }
    else {
        char tmp[20];
        int k = 0;
        while (v > 0) { tmp[k++] = '0' + (int)(v % 10); v /= 10; }
        while (k > 0) { buf[n++] = tmp[--k]; }
    }
    buf[n] = 0;
    semi_write0(buf);
}

static void report(const char *label, uint64_t cycles, uint64_t total) {
    semi_write0("  ");
    semi_write0(label);
    semi_write0(": ");
    semi_write_u64(cycles);
    semi_write0(" ticks (");
    if (total > 0) {
        uint32_t pct = (uint32_t)((cycles * 1000u) / total);
        semi_write_u32(pct / 10);
        semi_write0(".");
        semi_write_u32(pct % 10);
        semi_write0("%)");
    }
    semi_write0("\n");
}

int main(void) {
    semi_write0("profile_hawk: top-level breakdown of HAWK-512 ops\n");
    semi_write0("  Methodology: QEMU mps2-an386 -icount shift=0; SysTick proxy.\n");
    semi_write0("  HAWK_PLANT_NTT=1 (asm NTT engaged).\n\n");

    randombytes_reseed(0xDEADBEEFu);

    cyc_init();
    cyc_snap_t t0, t1;

    /* === Keygen === */
    prof_reset();
    t0 = cyc_snap();
    int rc = hawk_keygen(LOGN, priv, pub, &hrng, 0, tmp_kgen, sizeof tmp_kgen);
    t1 = cyc_snap();
    uint64_t cyc_keygen = cyc_elapsed(t0, t1);
    if (!rc) { semi_write0("FAIL keygen\n"); return 1; }

    uint64_t kgen_solve_cyc      = prof_solve_NTRU_cyc;
    uint32_t kgen_solve_calls    = prof_solve_NTRU_calls;
    uint64_t kgen_vfft_cyc       = prof_vect_FFT_cyc;
    uint32_t kgen_vfft_calls     = prof_vect_FFT_calls;
    uint64_t kgen_viFFT_cyc      = prof_vect_iFFT_cyc;
    uint32_t kgen_viFFT_calls    = prof_vect_iFFT_calls;
    uint64_t kgen_keccak_cyc     = prof_KeccakF1600_StatePermute_cyc
                                 + prof_KeccakF1600_StateXORBytes_cyc
                                 + prof_KeccakF1600_StateExtractBytes_cyc;
    uint32_t kgen_keccak_calls   = prof_KeccakF1600_StatePermute_calls;
    uint64_t kgen_mp_NTT_cyc     = prof_mp_NTT_cyc;
    uint32_t kgen_mp_NTT_calls   = prof_mp_NTT_calls;
    uint64_t kgen_mp_NTT_aa_cyc  = prof_mp_NTT_autoadj_cyc;
    uint32_t kgen_mp_NTT_aa_calls= prof_mp_NTT_autoadj_calls;

    /* === Sign === */
    const char *msg = "HAWK-m4 profile message";
    size_t mlen = 23;
    shake_context sc;

    prof_reset();
    t0 = cyc_snap();
    hawk_sign_start(&sc);
    shake_inject(&sc, msg, mlen);
    rc = hawk_sign_finish(LOGN, &hrng, 0, sig, &sc, priv,
                          tmp_sign, sizeof tmp_sign);
    t1 = cyc_snap();
    uint64_t cyc_sign = cyc_elapsed(t0, t1);
    if (!rc) { semi_write0("FAIL sign\n"); return 2; }

    /* Snapshot of sign-side profile counters. */
    uint64_t sign_ntt_cyc        = prof_mq18433_NTT_plant_cyc;
    uint64_t sign_ntt_pair_cyc   = prof_mq18433_NTT_pair_plant_cyc;
    uint64_t sign_intt_cyc       = prof_mq18433_iNTT_plant_cyc;
    uint64_t sign_montymul_cyc   = prof_mq18433_montymul_plant_cyc;
    uint64_t sign_keccak_cyc     = prof_KeccakF1600_StatePermute_cyc
                                 + prof_KeccakF1600_StateXORBytes_cyc
                                 + prof_KeccakF1600_StateExtractBytes_cyc;
    uint32_t sign_ntt_calls      = prof_mq18433_NTT_plant_calls;
    uint32_t sign_ntt_pair_calls = prof_mq18433_NTT_pair_plant_calls;
    uint32_t sign_intt_calls     = prof_mq18433_iNTT_plant_calls;
    uint32_t sign_montymul_calls = prof_mq18433_montymul_plant_calls;
    uint32_t sign_keccak_calls   = prof_KeccakF1600_StatePermute_calls;
    uint64_t sign_extr_cyc       = prof_extract_lowbit_cyc;
    uint32_t sign_extr_calls     = prof_extract_lowbit_calls;
    uint64_t sign_basis_cyc      = prof_basis_m2_mul_cyc;
    uint32_t sign_basis_calls    = prof_basis_m2_mul_calls;
    uint64_t sign_gauss_cyc      = prof_sig_gauss_cyc + prof_sig_gauss_alt_cyc;
    uint32_t sign_gauss_calls    = prof_sig_gauss_calls + prof_sig_gauss_alt_calls;
    uint64_t sign_symbrk_cyc     = prof_poly_symbreak_cyc;
    uint32_t sign_symbrk_calls   = prof_poly_symbreak_calls;
    uint64_t sign_encode_cyc     = prof_encode_sig_cyc;
    uint32_t sign_encode_calls   = prof_encode_sig_calls;

    /* === Verify === */
    prof_reset();
    t0 = cyc_snap();
    hawk_verify_start(&sc);
    shake_inject(&sc, msg, mlen);
    rc = hawk_verify_finish(LOGN, sig, CRYPTO_BYTES, &sc, pub,
                            CRYPTO_PUBLICKEYBYTES, tmp_vrfy, sizeof tmp_vrfy);
    t1 = cyc_snap();
    uint64_t cyc_verify = cyc_elapsed(t0, t1);
    if (!rc) { semi_write0("FAIL verify\n"); return 3; }

    /* Snapshot of verify-side profile counters. */
    uint64_t vrfy_ntt_cyc        = prof_mq18433_NTT_plant_cyc;
    uint64_t vrfy_intt_cyc       = prof_mq18433_iNTT_plant_cyc;
    uint64_t vrfy_keccak_cyc     = prof_KeccakF1600_StatePermute_cyc
                                 + prof_KeccakF1600_StateXORBytes_cyc
                                 + prof_KeccakF1600_StateExtractBytes_cyc;
    uint32_t vrfy_keccak_calls   = prof_KeccakF1600_StatePermute_calls;

    uint64_t total = cyc_keygen + cyc_sign + cyc_verify;

    semi_write0("=== HAWK-512 cycle breakdown ===\n");
    report("keygen", cyc_keygen, total);
    report("sign  ", cyc_sign,   total);
    report("verify", cyc_verify, total);
    semi_write0("\n");

    /* === Keygen-side breakdown === */
    semi_write0("=== KEYGEN-side breakdown (");
    semi_write_u64(cyc_keygen);
    semi_write0(" ticks total) ===\n");
    report("solve_NTRU       ", kgen_solve_cyc, cyc_keygen);
    semi_write0("    ("); semi_write_u32(kgen_solve_calls); semi_write0(" calls)\n");
    report("vect_FFT         ", kgen_vfft_cyc, cyc_keygen);
    semi_write0("    ("); semi_write_u32(kgen_vfft_calls); semi_write0(" calls)\n");
    report("vect_iFFT        ", kgen_viFFT_cyc, cyc_keygen);
    semi_write0("    ("); semi_write_u32(kgen_viFFT_calls); semi_write0(" calls)\n");
    report("mp_NTT (31-bit)  ", kgen_mp_NTT_cyc, cyc_keygen);
    semi_write0("    ("); semi_write_u32(kgen_mp_NTT_calls); semi_write0(" calls)\n");
    report("mp_NTT_autoadj   ", kgen_mp_NTT_aa_cyc, cyc_keygen);
    semi_write0("    ("); semi_write_u32(kgen_mp_NTT_aa_calls); semi_write0(" calls)\n");
    report("Keccak (SHAKE)   ", kgen_keccak_cyc, cyc_keygen);
    semi_write0("    ("); semi_write_u32(kgen_keccak_calls); semi_write0(" permutes)\n");
    uint64_t kgen_other = cyc_keygen - kgen_solve_cyc - kgen_vfft_cyc - kgen_viFFT_cyc
                        - kgen_mp_NTT_cyc - kgen_mp_NTT_aa_cyc - kgen_keccak_cyc;
    report("other (residual) ", kgen_other, cyc_keygen);
    semi_write0("\n");

    /* === Sign-side breakdown === */
    semi_write0("=== SIGN-side breakdown (");
    semi_write_u64(cyc_sign);
    semi_write0(" ticks total) ===\n");
    report("NTT      ", sign_ntt_cyc, cyc_sign);
    semi_write0("    ("); semi_write_u32(sign_ntt_calls); semi_write0(" calls)\n");
    report("NTT_pair ", sign_ntt_pair_cyc, cyc_sign);
    semi_write0("    ("); semi_write_u32(sign_ntt_pair_calls); semi_write0(" calls)\n");
    report("iNTT     ", sign_intt_cyc, cyc_sign);
    semi_write0("    ("); semi_write_u32(sign_intt_calls); semi_write0(" calls)\n");
    report("montymul ", sign_montymul_cyc, cyc_sign);
    semi_write0("    ("); semi_write_u32(sign_montymul_calls); semi_write0(" calls)\n");
    report("Keccak   ", sign_keccak_cyc, cyc_sign);
    semi_write0("    ("); semi_write_u32(sign_keccak_calls); semi_write0(" permutes)\n");
    report("extract_lowbit", sign_extr_cyc, cyc_sign);
    semi_write0("    ("); semi_write_u32(sign_extr_calls); semi_write0(" calls)\n");
    report("basis_m2_mul  ", sign_basis_cyc, cyc_sign);
    semi_write0("    ("); semi_write_u32(sign_basis_calls); semi_write0(" calls)\n");
    report("sig_gauss[_alt]", sign_gauss_cyc, cyc_sign);
    semi_write0("    ("); semi_write_u32(sign_gauss_calls); semi_write0(" calls)\n");
    report("poly_symbreak ", sign_symbrk_cyc, cyc_sign);
    semi_write0("    ("); semi_write_u32(sign_symbrk_calls); semi_write0(" calls)\n");
    report("encode_sig    ", sign_encode_cyc, cyc_sign);
    semi_write0("    ("); semi_write_u32(sign_encode_calls); semi_write0(" calls)\n");
    uint64_t sign_other = cyc_sign - sign_ntt_cyc - sign_ntt_pair_cyc - sign_intt_cyc
                        - sign_montymul_cyc - sign_keccak_cyc
                        - sign_extr_cyc - sign_basis_cyc - sign_gauss_cyc
                        - sign_symbrk_cyc - sign_encode_cyc;
    report("other (residual)", sign_other, cyc_sign);
    semi_write0("\n");

    /* === Verify-side breakdown === */
    semi_write0("=== VERIFY-side breakdown (");
    semi_write_u64(cyc_verify);
    semi_write0(" ticks total) ===\n");
    report("Keccak           ", vrfy_keccak_cyc, cyc_verify);
    semi_write0("    ("); semi_write_u32(vrfy_keccak_calls); semi_write0(" permutes)\n");
    report("mp_NTT (31-bit)  ", prof_mp_NTT_cyc, cyc_verify);
    semi_write0("    ("); semi_write_u32(prof_mp_NTT_calls); semi_write0(" calls)\n");
    report("mp_NTT_autoadj   ", prof_mp_NTT_autoadj_cyc, cyc_verify);
    semi_write0("    ("); semi_write_u32(prof_mp_NTT_autoadj_calls); semi_write0(" calls)\n");
    report("fx32_FFT         ", prof_fx32_FFT_cyc, cyc_verify);
    semi_write0("    ("); semi_write_u32(prof_fx32_FFT_calls); semi_write0(" calls)\n");
    report("fx32_iFFT        ", prof_fx32_iFFT_cyc, cyc_verify);
    semi_write0("    ("); semi_write_u32(prof_fx32_iFFT_calls); semi_write0(" calls)\n");
    report("decode_q00       ", prof_decode_q00_cyc, cyc_verify);
    semi_write0("    ("); semi_write_u32(prof_decode_q00_calls); semi_write0(" calls)\n");
    report("decode_q01       ", prof_decode_q01_cyc, cyc_verify);
    semi_write0("    ("); semi_write_u32(prof_decode_q01_calls); semi_write0(" calls)\n");
    report("decode_s1        ", prof_decode_s1_cyc, cyc_verify);
    semi_write0("    ("); semi_write_u32(prof_decode_s1_calls); semi_write0(" calls)\n");
    uint64_t vrfy_other = cyc_verify - vrfy_keccak_cyc
                        - prof_mp_NTT_cyc - prof_mp_NTT_autoadj_cyc
                        - prof_fx32_FFT_cyc - prof_fx32_iFFT_cyc
                        - prof_decode_q00_cyc - prof_decode_q01_cyc - prof_decode_s1_cyc;
    report("other (residual) ", vrfy_other, cyc_verify);

    semi_write0("\nALL OK\n");
    return 0;
}
