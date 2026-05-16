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
 * IRQ-driven wrap counter gives a 56-bit virtual cycle count so long
 * benchmarks (e.g., HAWK keygen / sign / verify) don't silently wrap
 * past 2^24 ticks. The handler is wired through startup.S's vector
 * table at offset 0x3C.
 *
 * Total cycle count between cyc_start() and cyc_end() is:
 *     (wrap_count_at_end - wrap_count_at_start) * (SYST_MAX + 1)
 *   + (cvr_at_start - cvr_at_end)               // SysTick counts DOWN
 */
#define SYST_CSR    (*(volatile uint32_t *)0xE000E010)
#define SYST_RVR    (*(volatile uint32_t *)0xE000E014)
#define SYST_CVR    (*(volatile uint32_t *)0xE000E018)
#define SYST_CSR_ENABLE     (1u << 0)
#define SYST_CSR_TICKINT    (1u << 1)
#define SYST_CSR_CLKSOURCE  (1u << 2)
#define SYST_MAX            0x00FFFFFFu   /* 24-bit reload max */

/* Incremented by SysTick_Handler in this TU (overrides the weak stub
 * in startup.S). Read inside cyc_* with interrupts briefly disabled to
 * avoid torn reads across a handler firing mid-snapshot. */
volatile uint32_t systick_wrap_count;

void SysTick_Handler(void) {
    systick_wrap_count++;
}

static inline void cyc_init(void) {
    SYST_CSR = 0;                            /* disable while we configure */
    SYST_RVR = SYST_MAX;
    SYST_CVR = 0;                            /* any write clears CVR */
    systick_wrap_count = 0;
    SYST_CSR = SYST_CSR_ENABLE
             | SYST_CSR_TICKINT
             | SYST_CSR_CLKSOURCE;
    /* SysTick takes 1 hardware clock to reload CVR from RVR after enable.
     * Under QEMU -icount shift=0, this is ~40 emulated instructions.
     * Spin until CVR has loaded so the first snapshot doesn't observe
     * the transient CVR=0 (which would be misinterpreted as a wrap). */
    while (SYST_CVR == 0) {
        /* spin */
    }
}

/* Snapshot { wrap_count, cvr } atomically. Disable IRQs around the
 * pair so we can't observe a wrap_count from before a handler fire
 * paired with a cvr from after (or vice versa). */
typedef struct { uint32_t wraps; uint32_t cvr; } cyc_snapshot;

static inline cyc_snapshot cyc_snap(void) {
    cyc_snapshot s;
    asm volatile ("cpsid i" ::: "memory");
    /* Read CVR first, then wraps. If wraps changed between CVR and
     * the wraps read, the CVR is from the OLD period; re-read. */
    uint32_t cvr_a = SYST_CVR;
    uint32_t wraps = systick_wrap_count;
    uint32_t cvr_b = SYST_CVR;
    if (cvr_b > cvr_a) {
        /* CVR went UP across our reads ⇒ a wrap happened. Re-snap
         * with wraps re-read AFTER the second CVR. */
        wraps = systick_wrap_count;
        s.cvr = cvr_b;
    } else {
        s.cvr = cvr_a;
    }
    s.wraps = wraps;
    asm volatile ("cpsie i" ::: "memory");
    return s;
}

/* 64-bit elapsed cycles between two snapshots.
 *
 * SysTick counts DOWN, so the in-period component is (t0.cvr - t1.cvr).
 * Across W wrap events, total elapsed = W * (SYST_MAX + 1) + (t0.cvr - t1.cvr).
 * If t1.cvr > t0.cvr (counter went "up"), an additional unaccounted wrap
 * happened between the two snapshots — add one period and accept the
 * negative cvr_delta.
 */
static inline uint64_t cyc_elapsed(cyc_snapshot t0, cyc_snapshot t1) {
    uint32_t wraps = t1.wraps - t0.wraps;
    int32_t  cvr_delta = (int32_t)t0.cvr - (int32_t)t1.cvr;
    if (cvr_delta < 0) {
        wraps += 1;
        cvr_delta += (int32_t)(SYST_MAX + 1);
    }
    return (uint64_t)wraps * ((uint64_t)SYST_MAX + 1u)
         + (uint64_t)cvr_delta;
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

static uint64_t measure(void (*fn)(unsigned, uint16_t *), unsigned logn,
                        uint16_t *a) {
    /* Warm-up call so any cold-cache effects don't bias the measurement. */
    fn(logn, a);
    /* Refill and measure cleanly. */
    fill_random(a, (size_t)1 << logn);
    cyc_init();
    cyc_snapshot t0 = cyc_snap();
    fn(logn, a);
    cyc_snapshot t1 = cyc_snap();
    return cyc_elapsed(t0, t1);
}

/* Print an unsigned 64-bit value. (semi_write_u32 only handles 32 bits;
 * once wrap_count > 0, ticks can exceed 2^32.) */
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

int main(void) {
    semi_write0("bench_ntt: SysTick proxy (QEMU; not pipeline-accurate)\n");
    semi_write0("op        logn      C-ref      asm     speedup(%)\n");

    for (unsigned logn = 6; logn <= 10; logn++) {
        uint64_t c_cycles    = measure(bench_NTT_c,        logn, buf_a);
        uint64_t asm_cycles  = measure(mq18433_NTT_plant,  logn, buf_b);
        semi_write0("NTT       ");
        semi_write_u32(logn);
        semi_write0("   ");
        semi_write_u64(c_cycles);
        semi_write0("   ");
        semi_write_u64(asm_cycles);
        semi_write0("   ");
        if (c_cycles > 0) {
            /* Speedup fits in 32-bit even when raw cycles do not. */
            uint32_t c32 = (uint32_t)c_cycles;
            uint32_t a32 = (uint32_t)asm_cycles;
            uint32_t speedup = ((c32 - a32) * 100u) / c32;
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
        semi_write_u64(c_cycles);
        semi_write0("   ");
        semi_write_u64(asm_cycles);
        semi_write0("   ");
        if (c_cycles > 0) {
            /* Speedup fits in 32-bit even when raw cycles do not. */
            uint32_t c32 = (uint32_t)c_cycles;
            uint32_t a32 = (uint32_t)asm_cycles;
            uint32_t speedup = ((c32 - a32) * 100u) / c32;
            semi_write_u32(speedup);
        } else {
            semi_write0("n/a");
        }
        semi_write0("\n");
    }

    semi_write0("done.\n");
    return 0;
}
