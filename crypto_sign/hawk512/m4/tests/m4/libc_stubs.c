/*
 * Minimal freestanding libc stubs needed by the HAWK end-to-end smoke
 * test under QEMU. Implements only the functions actually called: a few
 * string helpers and a deterministic randombytes for repeatable runs.
 *
 * The implementations are correct (not just stubs that return 0) so the
 * smoke test exercises real HAWK keygen/sign/verify behavior.
 */

#include <stddef.h>
#include <stdint.h>

/* ---- string helpers ---- */

void *memcpy(void *restrict d, const void *restrict s, size_t n) {
    unsigned char *dp = (unsigned char *)d;
    const unsigned char *sp = (const unsigned char *)s;
    while (n--) *dp++ = *sp++;
    return d;
}

void *memmove(void *d, const void *s, size_t n) {
    unsigned char *dp = (unsigned char *)d;
    const unsigned char *sp = (const unsigned char *)s;
    if (dp == sp || n == 0) return d;
    if (dp < sp) {
        while (n--) *dp++ = *sp++;
    } else {
        dp += n; sp += n;
        while (n--) *--dp = *--sp;
    }
    return d;
}

void *memset(void *s, int c, size_t n) {
    unsigned char *p = (unsigned char *)s;
    while (n--) *p++ = (unsigned char)c;
    return s;
}

int memcmp(const void *a, const void *b, size_t n) {
    const unsigned char *pa = (const unsigned char *)a;
    const unsigned char *pb = (const unsigned char *)b;
    while (n--) {
        if (*pa != *pb) return (int)*pa - (int)*pb;
        pa++; pb++;
    }
    return 0;
}

size_t strlen(const char *s) {
    const char *p = s;
    while (*p) p++;
    return (size_t)(p - s);
}

/* ---- deterministic randombytes for testing ---- */

static uint32_t rng_state = 0x12345678u;

void randombytes(unsigned char *out, size_t outlen) {
    while (outlen-- > 0) {
        rng_state = rng_state * 1664525u + 1013904223u;
        *out++ = (unsigned char)(rng_state >> 24);
    }
}

void randombytes_reseed(uint32_t seed) { rng_state = seed; }
