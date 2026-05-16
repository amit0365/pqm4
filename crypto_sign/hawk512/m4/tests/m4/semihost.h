/*
 * Minimal ARM semihosting helpers for the QEMU asm cross-check harness.
 *
 * Uses bkpt #0xAB with r0=op, r1=arg to invoke semihosting requests.
 * QEMU intercepts and forwards to the host (stdout / process exit).
 *
 * Reference: ARM Semihosting v2 (SH_SYS_*).
 *
 * IMPORTANT: semihosting clobbers r0 (operation returns a value
 * there). The inline asm below uses explicit `movs r0, #op` per call
 * and clobbers r0/r1 so the compiler doesn't fold redundant loads
 * across calls.
 */

#ifndef SEMIHOST_H__
#define SEMIHOST_H__

#include <stdint.h>

#define SYS_WRITE0   0x04   /* r1 = null-terminated string */
#define SYS_WRITEC   0x03   /* r1 = address of single char */

static inline void semi_write0(const char *s) {
    asm volatile (
        "movs r0, #4\n"
        "mov  r1, %0\n"
        "bkpt #0xAB\n"
        :: "r"(s) : "r0", "r1", "memory"
    );
}

static inline void semi_writec(char c) {
    /* SYS_WRITEC takes the address of the char, not the char itself. */
    asm volatile (
        "movs r0, #3\n"
        "mov  r1, %0\n"
        "bkpt #0xAB\n"
        :: "r"(&c) : "r0", "r1", "memory"
    );
}

/* Print an 8-digit lowercase hex value, no "0x" prefix, no newline. */
static inline void semi_write_hex32(uint32_t v) {
    static const char hex[] = "0123456789abcdef";
    char buf[9];
    for (int i = 0; i < 8; i++) {
        buf[i] = hex[(v >> ((7 - i) * 4)) & 0xF];
    }
    buf[8] = 0;
    semi_write0(buf);
}

/* Print an unsigned decimal up to 10 digits (uint32). */
static inline void semi_write_u32(uint32_t v) {
    char buf[11];
    int n = 0;
    if (v == 0) { buf[n++] = '0'; }
    else {
        char tmp[10];
        int k = 0;
        while (v > 0) { tmp[k++] = '0' + (v % 10); v /= 10; }
        while (k > 0) { buf[n++] = tmp[--k]; }
    }
    buf[n] = 0;
    semi_write0(buf);
}

#endif /* SEMIHOST_H__ */
