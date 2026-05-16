/*
 * bench_ntt — measure asm NTT/iNTT vs C reference under QEMU.
 *
 * Uses SysTick CVR, driven by virtual time. MUST be run with
 * `-icount shift=0,align=off,sleep=off` so QEMU drives the virtual clock
 * from the host TCG instruction count. Without -icount, SysTick stays
 * at 0 throughout the run.
 *
 * Numbers reported are SysTick ticks, NOT real Cortex-M4 cycles. On
 * mps2-an386 (25 MHz CPU, SysTick at the same rate, -icount shift=0
 * meaning 1 instruction = 1 ns of virtual time), one SysTick tick
 * corresponds to ~40 emulated instructions. So:
 *     reported * 40 ≈ instructions executed (rough)
 *
 * Pipeline timing, flash wait states, dual-issue, and prefetch are NOT
 * modeled. Real on-target Cortex-M4 cycle counts will differ — but the
 * RATIO between C-ref and asm should track closely, because the two
 * are run under identical (in)accuracy.
 *
 * The C reference is the same plant_18433.c body, but compiled with
 * -U__ARM_FEATURE_DSP so its function bodies are present and have
 * distinct symbol names (mq18433_NTT_c, etc.) — see bench_c_refs.c.
 */

#include <stdint.h>
#include <stddef.h>

#define Q   18433
#include "../../modq.h"
#define Q   18433

#include "semihost.h"

/* The two impls under test. */
extern void mq18433_NTT_plant(unsigned logn, uint16_t *a);
extern void mq18433_iNTT_plant(unsigned logn, uint16_t *a);

/* C reference impls with renamed symbols (in bench_c_refs.c).
 * They call the same `Zq(NTT)`/`Zq(iNTT)` static inlines from modq.h
 * that the host test uses — these are the canonical C reference. */
extern void bench_NTT_c(unsigned logn, uint16_t *a);
extern void bench_iNTT_c(unsigned logn, uint16_t *a);

/* SysTick — Cortex-M architectural timer, well-emulated by QEMU.
 * (DWT->CYCCNT isn't emulated on mps2-an386; SysTick is.) */
#define SYST_CSR    (*(volatile uint32_t *)0xE000E010)
#define SYST_RVR    (*(volatile uint32_t *)0xE000E014)
#define SYST_CVR    (*(volatile uint32_t *)0xE000E018)
#define SYST_CSR_ENABLE     (1u << 0)
#define SYST_CSR_CLKSOURCE  (1u << 2)
#define SYST_MAX            0x00FFFFFFu   /* 24-bit reload max */

static inline void cyc_init(void) {
    SYST_CSR = 0;            /* disable while we configure */
    SYST_RVR = SYST_MAX;
    SYST_CVR = 0;            /* any write clears */
    SYST_CSR = SYST_CSR_ENABLE | SYST_CSR_CLKSOURCE;
}

/* SysTick counts DOWN; convert to elapsed. Handles single wrap. */
static inline uint32_t cyc_read(void) {
    return SYST_CVR;
}
static inline uint32_t cyc_delta(uint32_t start, uint32_t end) {
    /* start > end normally (counted down). Handle wraparound. */
    if (start >= end) return start - end;
    return (SYST_MAX + 1u - end) + start;
}

static uint32_t rng_state = 0x42424242u;
static uint32_t rng_next(void) {
    rng_state = rng_state * 1664525u + 1013904223u;
    return rng_state;
}

#define MAXN 1024
static uint16_t buf_a[MAXN];
static uint16_t buf_b[MAXN];

static void fill_random(uint16_t *a, size_t n) {
    for (size_t i = 0; i < n; i++) {
        a[i] = (uint16_t)(1u + (rng_next() % Q));
    }
}

static uint32_t measure(void (*fn)(unsigned, uint16_t *), unsigned logn,
                        uint16_t *a) {
    /* Warm-up call so any cold-cache effects don't bias the measurement. */
    fn(logn, a);
    /* Refill and measure cleanly. */
    fill_random(a, (size_t)1 << logn);
    cyc_init();
    uint32_t t0 = cyc_read();
    fn(logn, a);
    uint32_t t1 = cyc_read();
    return cyc_delta(t0, t1);
}

int main(void) {
    semi_write0("bench_ntt: SysTick proxy (QEMU; not pipeline-accurate)\n");
    semi_write0("op        logn      C-ref      asm     speedup(%)\n");

    for (unsigned logn = 6; logn <= 10; logn++) {
        uint32_t c_cycles    = measure(bench_NTT_c,        logn, buf_a);
        uint32_t asm_cycles  = measure(mq18433_NTT_plant,  logn, buf_b);
        semi_write0("NTT       ");
        semi_write_u32(logn);
        semi_write0("   ");
        semi_write_u32(c_cycles);
        semi_write0("   ");
        semi_write_u32(asm_cycles);
        semi_write0("   ");
        /* (c - asm) / c * 100, integer */
        if (c_cycles > 0) {
            uint32_t speedup = ((c_cycles - asm_cycles) * 100u) / c_cycles;
            semi_write_u32(speedup);
        } else {
            semi_write0("n/a");
        }
        semi_write0("\n");

        c_cycles    = measure(bench_iNTT_c,       logn, buf_a);
        asm_cycles  = measure(mq18433_iNTT_plant, logn, buf_b);
        semi_write0("iNTT      ");
        semi_write_u32(logn);
        semi_write0("   ");
        semi_write_u32(c_cycles);
        semi_write0("   ");
        semi_write_u32(asm_cycles);
        semi_write0("   ");
        if (c_cycles > 0) {
            uint32_t speedup = ((c_cycles - asm_cycles) * 100u) / c_cycles;
            semi_write_u32(speedup);
        } else {
            semi_write0("n/a");
        }
        semi_write0("\n");
    }

    semi_write0("done.\n");
    return 0;
}
