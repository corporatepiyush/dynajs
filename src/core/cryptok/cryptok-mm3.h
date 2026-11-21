#ifndef DYN_CRYPTOK_CRYPTOK_MM3_H
#define DYN_CRYPTOK_CRYPTOK_MM3_H

#include "cryptok.h"

static inline uint64_t mm3_fmix(uint64_t k)
{
    k ^= k >> 33;
    k *= 0xFF51AFD7ED558CCDULL;
    k ^= k >> 33;
    k *= 0xC4CEB9FE1A85EC53ULL;
    k ^= k >> 33;
    return k;
}

static inline uint64_t mm3_load64(const uint8_t* p)
{
    uint64_t v = 0;
    int i;
    for (i = 0; i < 8; i++)
        v |= (uint64_t)p[i] << (8 * i);
    return v;
}

static inline void mm3_128(const uint8_t* p, size_t n, uint32_t seed, uint8_t out[16])
{
    const uint64_t c1 = 0x87C37B91114253D5ULL, c2 = 0x4CF5AD432745937FULL;
    uint64_t h1 = seed, h2 = seed, k1 = 0, k2 = 0;
    size_t blocks = n / 16, i;
    const uint8_t* tail;

    for (i = 0; i < blocks; i++) {
        uint64_t a = mm3_load64(p + i * 16), b = mm3_load64(p + i * 16 + 8);
        a *= c1;
        a = dyn_rotl64(a, 31);
        a *= c2;
        h1 ^= a;
        h1 = dyn_rotl64(h1, 27);
        h1 += h2;
        h1 = h1 * 5 + 0x52DCE729;
        b *= c2;
        b = dyn_rotl64(b, 33);
        b *= c1;
        h2 ^= b;
        h2 = dyn_rotl64(h2, 31);
        h2 += h1;
        h2 = h2 * 5 + 0x38495AB5;
    }
    tail = p + blocks * 16;
    switch (n & 15) {
    case 15:
        k2 ^= (uint64_t)tail[14] << 48;
        __attribute__((fallthrough));
    case 14:
        k2 ^= (uint64_t)tail[13] << 40;
        __attribute__((fallthrough));
    case 13:
        k2 ^= (uint64_t)tail[12] << 32;
        __attribute__((fallthrough));
    case 12:
        k2 ^= (uint64_t)tail[11] << 24;
        __attribute__((fallthrough));
    case 11:
        k2 ^= (uint64_t)tail[10] << 16;
        __attribute__((fallthrough));
    case 10:
        k2 ^= (uint64_t)tail[9] << 8;
        __attribute__((fallthrough));
    case 9:
        k2 ^= (uint64_t)tail[8];
        k2 *= c2;
        k2 = dyn_rotl64(k2, 33);
        k2 *= c1;
        h2 ^= k2;
        __attribute__((fallthrough));
    case 8:
        k1 ^= (uint64_t)tail[7] << 56;
        __attribute__((fallthrough));
    case 7:
        k1 ^= (uint64_t)tail[6] << 48;
        __attribute__((fallthrough));
    case 6:
        k1 ^= (uint64_t)tail[5] << 40;
        __attribute__((fallthrough));
    case 5:
        k1 ^= (uint64_t)tail[4] << 32;
        __attribute__((fallthrough));
    case 4:
        k1 ^= (uint64_t)tail[3] << 24;
        __attribute__((fallthrough));
    case 3:
        k1 ^= (uint64_t)tail[2] << 16;
        __attribute__((fallthrough));
    case 2:
        k1 ^= (uint64_t)tail[1] << 8;
        __attribute__((fallthrough));
    case 1:
        k1 ^= (uint64_t)tail[0];
        k1 *= c1;
        k1 = dyn_rotl64(k1, 31);
        k1 *= c2;
        h1 ^= k1;
        break;
    default:
        break;
    }
    h1 ^= (uint64_t)n;
    h2 ^= (uint64_t)n;
    h1 += h2;
    h2 += h1;
    h1 = mm3_fmix(h1);
    h2 = mm3_fmix(h2);
    h1 += h2;
    h2 += h1;
    for (i = 0; i < 8; i++)
        out[i] = (uint8_t)(h1 >> (8 * i));
    for (i = 0; i < 8; i++)
        out[8 + i] = (uint8_t)(h2 >> (8 * i));
}

#endif
