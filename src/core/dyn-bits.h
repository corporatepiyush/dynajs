/*
 * dyn-bits -- fixed-width bit primitives. PURE C: no
 * JSValue, no JSContext. Compiles standalone with `-Isrc/core`.
 *
 * HEADER-ONLY, and deliberately so. Every operation here is one to three
 * machine instructions -- popcount, clz, ctz, a rotate, a widening multiply.
 * An out-of-line call would cost several times the operation it wraps, so these
 * are `static inline` and there is no dyn-bits.c. That is also why the API takes
 * an explicit `width` rather than generating a function per width: the width is
 * a compile-time constant at every call site, so the masks and the `width - n`
 * fold away.
 *
 * VALUE CONVENTION: an argument is a uint64_t already masked to its width, and
 * a result is masked to its width. The caller owns the masking on the way in --
 * dyna:bits does it while coercing from JS, and a C caller working at a known
 * width has nothing to mask.
 *
 * EDGE CASES, defined here so the JS binding is a straight pass-through:
 *   - LeadingZeros(0) == width, TrailingZeros(0) == width, Len(0) == 0.
 *     __builtin_clzll(0)/__builtin_ctzll(0) are UB; zero is special-cased here
 *     rather than at each call site.
 *   - RotateLeft reduces k modulo the width (s = (unsigned)k & (width-1)), so a
 *     negative k rotates right and any k wraps. s == 0 returns x unchanged,
 *     avoiding the UB of `x >> width`.
 *   - Div/Rem report y == 0 and quotient overflow as a status instead of
 *     trapping, and the JS binding turns the status into a RangeError.
 *     Keeping the policy here means one definition of "overflow".
 */
#ifndef DYN_BITS_H
#define DYN_BITS_H

#include <stdint.h>

#ifndef __SIZEOF_INT128__
#error "dyn-bits requires unsigned __int128 (GCC/Clang) for the 64-bit widening ops"
#endif

typedef unsigned __int128 dyn_u128;

/* Status of a double-width divide. The binding throws on both. */
typedef enum {
    DYN_BITS_OK = 0,
    DYN_BITS_DIV_ZERO,   /* y == 0 */
    DYN_BITS_OVERFLOW    /* y <= hi: the quotient would not fit the width */
} dyn_bits_status;

/* ---- counting ---------------------------------------------------------- */

/* Minimum bits needed to represent x; 0 for x == 0. Width-independent: it acts
 * on the true highest set bit of the already-masked value. */
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

/* ---- bit and byte reversal --------------------------------------------- */

/* Reverse all 64 bits: swap within each byte, then swap the byte order. */
static inline uint64_t dyn_bits_reverse64(uint64_t x)
{
    x = ((x >> 1) & 0x5555555555555555ULL) | ((x & 0x5555555555555555ULL) << 1);
    x = ((x >> 2) & 0x3333333333333333ULL) | ((x & 0x3333333333333333ULL) << 2);
    x = ((x >> 4) & 0x0F0F0F0F0F0F0F0FULL) | ((x & 0x0F0F0F0F0F0F0F0FULL) << 4);
    return __builtin_bswap64(x);
}

/* Reverse the low `width` bits. A narrower value reverses across all 64 bits
 * and lands in the high end, so it is shifted back down. */
static inline uint64_t dyn_bits_reverse(uint64_t x, int width)
{
    return dyn_bits_reverse64(x) >> (64 - width);
}

static inline uint16_t dyn_bits_bswap16(uint16_t x) { return __builtin_bswap16(x); }
static inline uint32_t dyn_bits_bswap32(uint32_t x) { return __builtin_bswap32(x); }
static inline uint64_t dyn_bits_bswap64(uint64_t x) { return __builtin_bswap64(x); }

/* ---- rotation ---------------------------------------------------------- */

/* Rotate the low `width` bits left by k; k < 0 rotates right. Result masked. */
static inline uint64_t dyn_bits_rotate_left(uint64_t x, int k, int width)
{
    unsigned s = (unsigned)k & (unsigned)(width - 1);
    if (!s)
        return x;
    x = (x << s) | (x >> (width - s));
    /* width == 64 would make the mask expression `1 << 64`, which is UB. */
    return width == 64 ? x : (x & ((((uint64_t)1) << width) - 1));
}

/* Fixed-width rotates, for the hash and compression cores. */
static inline uint32_t dyn_bits_rotl32(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }
static inline uint32_t dyn_bits_rotr32(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
static inline uint64_t dyn_bits_rotl64(uint64_t x, int n) { return (x << n) | (x >> (64 - n)); }
static inline uint64_t dyn_bits_rotr64(uint64_t x, int n) { return (x >> n) | (x << (64 - n)); }

/* ---- multi-precision arithmetic ---------------------------------------- */

/* sum = a + b + carry_in; *carry_out gets 0 or 1. */
static inline uint32_t dyn_bits_add32(uint32_t a, uint32_t b, uint32_t carry_in,
                                      uint32_t *carry_out)
{
    uint64_t s = (uint64_t)a + b + carry_in;
    *carry_out = (uint32_t)(s >> 32);
    return (uint32_t)s;
}

static inline uint64_t dyn_bits_add64(uint64_t a, uint64_t b, uint64_t carry_in,
                                      uint64_t *carry_out)
{
    dyn_u128 s = (dyn_u128)a + b + carry_in;
    *carry_out = (uint64_t)(s >> 64);
    return (uint64_t)s;
}

/* diff = a - b - borrow_in; *borrow_out gets 0 or 1. */
static inline uint32_t dyn_bits_sub32(uint32_t a, uint32_t b, uint32_t borrow_in,
                                      uint32_t *borrow_out)
{
    uint64_t sub = (uint64_t)b + borrow_in;
    *borrow_out = (uint64_t)a < sub ? 1u : 0u;
    return (uint32_t)((uint64_t)a - sub);
}

static inline uint64_t dyn_bits_sub64(uint64_t a, uint64_t b, uint64_t borrow_in,
                                      uint64_t *borrow_out)
{
    dyn_u128 sub = (dyn_u128)b + borrow_in;
    *borrow_out = (dyn_u128)a < sub ? 1u : 0u;
    return (uint64_t)((dyn_u128)a - sub);
}

/* Full double-width product: *hi:*lo = a * b. */
static inline void dyn_bits_mul32(uint32_t a, uint32_t b, uint32_t *hi, uint32_t *lo)
{
    uint64_t p = (uint64_t)a * b;
    *hi = (uint32_t)(p >> 32);
    *lo = (uint32_t)p;
}

static inline void dyn_bits_mul64(uint64_t a, uint64_t b, uint64_t *hi, uint64_t *lo)
{
    dyn_u128 p = (dyn_u128)a * b;
    *hi = (uint64_t)(p >> 64);
    *lo = (uint64_t)p;
}

/* [hi:lo] / y -> *quo, *rem. Rejects y == 0 and y <= hi (quotient overflow). */
static inline dyn_bits_status dyn_bits_div32(uint32_t hi, uint32_t lo, uint32_t y,
                                             uint32_t *quo, uint32_t *rem)
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
                                             uint64_t *quo, uint64_t *rem)
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

/* [hi:lo] % y. Only y == 0 is rejected -- a quotient that would overflow is
 * still a well-defined remainder, which is why this is not div's twin. */
static inline dyn_bits_status dyn_bits_rem32(uint32_t hi, uint32_t lo, uint32_t y,
                                             uint32_t *rem)
{
    if (y == 0)
        return DYN_BITS_DIV_ZERO;
    *rem = (uint32_t)((((uint64_t)hi << 32) | lo) % y);
    return DYN_BITS_OK;
}

static inline dyn_bits_status dyn_bits_rem64(uint64_t hi, uint64_t lo, uint64_t y,
                                             uint64_t *rem)
{
    if (y == 0)
        return DYN_BITS_DIV_ZERO;
    *rem = (uint64_t)(((((dyn_u128)hi) << 64) | lo) % y);
    return DYN_BITS_OK;
}

#endif /* DYN_BITS_H */
