#ifndef DYN_BITS_H
#define DYN_BITS_H

#include <stdint.h>

#ifndef __SIZEOF_INT128__
#error "dyn-bits requires unsigned __int128 (GCC/Clang) for the 64-bit widening ops"
#endif

typedef unsigned __int128 dyn_u128;

typedef enum {
    DYN_BITS_OK = 0,
    DYN_BITS_DIV_ZERO,
    DYN_BITS_OVERFLOW
} dyn_bits_status;

static inline int dyn_bits_len64(uint64_t x)
{
    return x ? 64 - __builtin_clzll(x) : 0;
}

static inline int dyn_bits_leading_zeros(uint64_t x, int width)
{
    return width - dyn_bits_len64(x);
}

static inline int dyn_bits_trailing_zeros(uint64_t x, int width)
{
    return x ? __builtin_ctzll(x) : width;
}

static inline int dyn_bits_ones_count(uint64_t x)
{
    return __builtin_popcountll(x);
}

static inline uint64_t dyn_bits_reverse64(uint64_t x)
{
    x = ((x >> 1) & 0x5555555555555555ULL) | ((x & 0x5555555555555555ULL) << 1);
    x = ((x >> 2) & 0x3333333333333333ULL) | ((x & 0x3333333333333333ULL) << 2);
    x = ((x >> 4) & 0x0F0F0F0F0F0F0F0FULL) | ((x & 0x0F0F0F0F0F0F0F0FULL) << 4);
    return __builtin_bswap64(x);
}

static inline uint64_t dyn_bits_reverse(uint64_t x, int width)
{
    return dyn_bits_reverse64(x) >> (64 - width);
}

static inline uint16_t dyn_bits_bswap16(uint16_t x) { return __builtin_bswap16(x); }
static inline uint32_t dyn_bits_bswap32(uint32_t x) { return __builtin_bswap32(x); }
static inline uint64_t dyn_bits_bswap64(uint64_t x) { return __builtin_bswap64(x); }

static inline uint64_t dyn_bits_rotate_left(uint64_t x, int k, int width)
{
    unsigned s = (unsigned)k & (unsigned)(width - 1);
    if (!s)
        return x;
    x = (x << s) | (x >> (width - s));
    return width == 64 ? x : (x & ((((uint64_t)1) << width) - 1));
}

static inline uint32_t dyn_bits_rotl32(uint32_t x, int n)
{
    unsigned s = (unsigned)n & 31u;
    if (!s)
        return x;
    return (x << s) | (x >> (32 - s));
}

static inline uint32_t dyn_bits_rotr32(uint32_t x, int n)
{
    unsigned s = (unsigned)n & 31u;
    if (!s)
        return x;
    return (x >> s) | (x << (32 - s));
}

static inline uint64_t dyn_bits_rotl64(uint64_t x, int n)
{
    unsigned s = (unsigned)n & 63u;
    if (!s)
        return x;
    return (x << s) | (x >> (64 - s));
}

static inline uint64_t dyn_bits_rotr64(uint64_t x, int n)
{
    unsigned s = (unsigned)n & 63u;
    if (!s)
        return x;
    return (x >> s) | (x << (64 - s));
}

static inline uint32_t dyn_bits_add32(uint32_t a, uint32_t b, uint32_t carry_in,
    uint32_t* carry_out)
{
    uint64_t s = (uint64_t)a + b + carry_in;
    *carry_out = (uint32_t)(s >> 32);
    return (uint32_t)s;
}

static inline uint64_t dyn_bits_add64(uint64_t a, uint64_t b, uint64_t carry_in,
    uint64_t* carry_out)
{
    dyn_u128 s = (dyn_u128)a + b + carry_in;
    *carry_out = (uint64_t)(s >> 64);
    return (uint64_t)s;
}

static inline uint32_t dyn_bits_sub32(uint32_t a, uint32_t b, uint32_t borrow_in,
    uint32_t* borrow_out)
{
    uint64_t sub = (uint64_t)b + borrow_in;
    *borrow_out = (uint64_t)a < sub ? 1u : 0u;
    return (uint32_t)((uint64_t)a - sub);
}

static inline uint64_t dyn_bits_sub64(uint64_t a, uint64_t b, uint64_t borrow_in,
    uint64_t* borrow_out)
{
    dyn_u128 sub = (dyn_u128)b + borrow_in;
    *borrow_out = (dyn_u128)a < sub ? 1u : 0u;
    return (uint64_t)((dyn_u128)a - sub);
}

static inline void dyn_bits_mul32(uint32_t a, uint32_t b, uint32_t* hi, uint32_t* lo)
{
    uint64_t p = (uint64_t)a * b;
    *hi = (uint32_t)(p >> 32);
    *lo = (uint32_t)p;
}

static inline void dyn_bits_mul64(uint64_t a, uint64_t b, uint64_t* hi, uint64_t* lo)
{
    dyn_u128 p = (dyn_u128)a * b;
    *hi = (uint64_t)(p >> 64);
    *lo = (uint64_t)p;
}

static inline dyn_bits_status dyn_bits_div32(uint32_t hi, uint32_t lo, uint32_t y,
    uint32_t* quo, uint32_t* rem)
{
    uint64_t d;
    if (y == 0)
        return DYN_BITS_DIV_ZERO;
    if (y <= hi)
        return DYN_BITS_OVERFLOW;
    d = ((uint64_t)hi << 32) | lo;
    *quo = (uint32_t)(d / y);
    *rem = (uint32_t)(d % y);
    return DYN_BITS_OK;
}

static inline dyn_bits_status dyn_bits_div64(uint64_t hi, uint64_t lo, uint64_t y,
    uint64_t* quo, uint64_t* rem)
{
    dyn_u128 d;
    if (y == 0)
        return DYN_BITS_DIV_ZERO;
    if (y <= hi)
        return DYN_BITS_OVERFLOW;
    d = ((dyn_u128)hi << 64) | lo;
    *quo = (uint64_t)(d / y);
    *rem = (uint64_t)(d % y);
    return DYN_BITS_OK;
}

static inline dyn_bits_status dyn_bits_rem32(uint32_t hi, uint32_t lo, uint32_t y,
    uint32_t* rem)
{
    if (y == 0)
        return DYN_BITS_DIV_ZERO;
    *rem = (uint32_t)((((uint64_t)hi << 32) | lo) % y);
    return DYN_BITS_OK;
}

static inline dyn_bits_status dyn_bits_rem64(uint64_t hi, uint64_t lo, uint64_t y,
    uint64_t* rem)
{
    if (y == 0)
        return DYN_BITS_DIV_ZERO;
    *rem = (uint64_t)(((((dyn_u128)hi) << 64) | lo) % y);
    return DYN_BITS_OK;
}

#endif
