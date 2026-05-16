/*
 * bench_c_refs.c — C reference NTT/iNTT compiled for M4, with renamed
 * symbols so they co-exist with the asm versions in the benchmark ELF.
 *
 * Bodies are line-for-line copies of plant_18433.c's mq18433_NTT_plant /
 * mq18433_iNTT_plant (which are themselves line-for-line copies of
 * modq.h's Zq(NTT) / Zq(iNTT)). Only the function names differ.
 */

#include <stdint.h>
#include <stddef.h>

#define Q   18433
#include "../../modq.h"

void bench_NTT_c(unsigned logn, uint16_t *a) {
    size_t t = (size_t)1 << logn;
    for (unsigned lm = 0; lm < logn; lm++) {
        size_t m = (size_t)1 << lm;
        size_t ht = t >> 1;
        size_t v0 = 0;
        for (size_t u = 0; u < m; u++) {
            uint32_t s = mq18433_GM[u + m];
            for (size_t v = 0; v < ht; v++) {
                size_t k1 = v0 + v;
                size_t k2 = k1 + ht;
                uint32_t x1 = a[k1];
                uint32_t x2 = mq18433_montymul(a[k2], s);
                a[k1] = mq18433_add(x1, x2);
                a[k2] = mq18433_sub(x1, x2);
            }
            v0 += t;
        }
        t = ht;
    }
}

void bench_iNTT_c(unsigned logn, uint16_t *a) {
    size_t t = 1;
    for (unsigned lm = 0; lm < logn; lm++) {
        size_t hm = (size_t)1 << (logn - 1 - lm);
        size_t dt = t << 1;
        size_t v0 = 0;
        for (size_t u = 0; u < hm; u++) {
            uint32_t s = mq18433_iGM[u + hm];
            for (size_t v = 0; v < t; v++) {
                size_t k1 = v0 + v;
                size_t k2 = k1 + t;
                uint32_t x1 = a[k1];
                uint32_t x2 = a[k2];
                a[k1] = mq18433_half(mq18433_add(x1, x2));
                a[k2] = mq18433_montymul(s, mq18433_sub(x1, x2));
            }
            v0 += dt;
        }
        t = dt;
    }
}
