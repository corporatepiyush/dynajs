/*
 * Tiny float64 printing and parsing library
 *
 * Copyright (c) 2024 Fabrice Bellard
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <inttypes.h>
#include <string.h>
#include <assert.h>
#include <ctype.h>
#include <sys/time.h>
#include <math.h>
#include <setjmp.h>

#include "cutils.h"
#include "dtoa.h"

/* 
   TODO:
   - test n_digits=101 instead of 100
   - simplify subnormal handling
   - reduce max memory usage
   - free format: could add shortcut if exact result
   - use 64 bit limb_t when possible
   - use another algorithm for free format dtoa in base 10 (ryu ?)
*/

#define USE_POW5_TABLE
/* use fast path to print small integers in free format */
#define USE_FAST_INT

/* Limb width. 5 => 32-bit limbs (the default and what every published number in
 * the numeric benchmarks were measured on); 6 => 64-bit limbs, which needs a
 * 128-bit integer type for the double-limb intermediate.
 *
 * THREE WIDTH CONTRACTS LIVE HERE AND THEY ARE NOT THE SAME:
 *   - `dlimb_t` must hold the product of two limbs;
 *   - `pow_ui` returns a value that fits in 64 BITS, whatever the limb width;
 *   - `pow_ui_inv` returns a value that fits in ONE LIMB, which is a stronger
 *     claim and is guaranteed by its callers choosing the exponent from
 *     digits_per_limb_table. Narrowing the first to the second is silently
 *     wrong on a 32-bit limb build, so the two stay separately typed. */
#ifndef LIMB_LOG2_BITS
#if defined(__SIZEOF_INT128__)
/* 64-bit limbs halve the number of limbs in every bignum operation, and the
   expensive dtoa paths are bignum-bound: measured 3.22x on the subnormal
   shortest-representation search and 3.15x on 100-digit fixed formatting, the
   two rows the numeric benchmarks call out as the slow ones. Requires a
   128-bit type for the double-limb product; without one the 32-bit path is
   still correct and is selected automatically. */
#define LIMB_LOG2_BITS 6
#else
#define LIMB_LOG2_BITS 5
#endif
#endif

#define LIMB_BITS (1 << LIMB_LOG2_BITS)

#if LIMB_BITS == 64
#if !defined(__SIZEOF_INT128__)
#error "64-bit limbs need a 128-bit integer type; build with LIMB_LOG2_BITS=5"
#endif
typedef int64_t slimb_t;
typedef uint64_t limb_t;
typedef unsigned __int128 dlimb_t;
#define clz_limb(x) clz64(x)
#define PRIxLIMB "016" PRIx64
#else
typedef int32_t slimb_t;
typedef uint32_t limb_t;
typedef uint64_t dlimb_t;
#define clz_limb(x) clz32(x)
#define PRIxLIMB "08x"
#endif

#define JS_RADIX_MAX 36

/* LIMB COUNTS, not byte counts -- so they halve when the limb width doubles,
 * and the scratch footprint (JSDTOATempMem) stays the same either way. Sizing
 * them as if they were bytes is what overruns the caller's buffer: the assert
 * in js_dtoa fires immediately at 64-bit limbs without this. */
#define DBIGNUM_LEN_MAX (1664 / LIMB_BITS) /* ~ 2^(1072+53)*36^100 (dtoa) */
#define MANT_LEN_MAX    (576 / LIMB_BITS)  /* < 36^100 */

typedef intptr_t mp_size_t;

/* the represented number is sum(i, tab[i]*2^(LIMB_BITS * i)) */
typedef struct {
    int len; /* >= 1 */
    limb_t tab[];
} mpb_t;

static limb_t mp_add_ui(limb_t *tab, limb_t b, size_t n)
{
    size_t i;
    limb_t k, a;

    k=b;
    for(i=0;i<n;i++) {
        if (k == 0)
            break;
        a = tab[i] + k;
        k = (a < k);
        tab[i] = a;
    }
    return k;
}

/* tabr[] = taba[] * b + l. Return the high carry */
static limb_t mp_mul1(limb_t *tabr, const limb_t *taba, limb_t n, 
                      limb_t b, limb_t l)
{
    limb_t i;
    dlimb_t t;

    for(i = 0; i < n; i++) {
        t = (dlimb_t)taba[i] * (dlimb_t)b + l;
        tabr[i] = t;
        l = t >> LIMB_BITS;
    }
    return l;
}

/* WARNING: d must be >= 2^(LIMB_BITS-1) */
static inline limb_t udiv1norm_init(limb_t d)
{
    limb_t a0, a1;
    a1 = -d - 1;
    a0 = -1;
    return (((dlimb_t)a1 << LIMB_BITS) | a0) / d;
}

/* return the quotient and the remainder in '*pr'of 'a1*2^LIMB_BITS+a0
   / d' with 0 <= a1 < d. */
static inline limb_t udiv1norm(limb_t *pr, limb_t a1, limb_t a0,
                                limb_t d, limb_t d_inv)
{
    limb_t n1m, n_adj, q, r, ah;
    dlimb_t a;
    n1m = ((slimb_t)a0 >> (LIMB_BITS - 1));
    n_adj = a0 + (n1m & d);
    a = (dlimb_t)d_inv * (a1 - n1m) + n_adj;
    q = (a >> LIMB_BITS) + a1;
    /* compute a - q * r and update q so that the remainder is between
       0 and d - 1 */
    a = ((dlimb_t)a1 << LIMB_BITS) | a0;
    a = a - (dlimb_t)q * d - d;
    ah = a >> LIMB_BITS;
    q += 1 + ah;
    r = (limb_t)a + (ah & d);
    *pr = r;
    return q;
}

static limb_t mp_div1(limb_t *tabr, const limb_t *taba, limb_t n,
                      limb_t b, limb_t r)
{
    slimb_t i;
    dlimb_t a1;
    for(i = n - 1; i >= 0; i--) {
        a1 = ((dlimb_t)r << LIMB_BITS) | taba[i];
        tabr[i] = a1 / b;
        r = a1 % b;
    }
    return r;
}

/* r = (a + high*B^n) >> shift. Return the remainder r (0 <= r < 2^shift). 
   1 <= shift <= LIMB_BITS - 1 */
static limb_t mp_shr(limb_t *tab_r, const limb_t *tab, mp_size_t n, 
                     int shift, limb_t high)
{
    mp_size_t i;
    limb_t l, a;

    assert(shift >= 1 && shift < LIMB_BITS);
    l = high;
    for(i = n - 1; i >= 0; i--) {
        a = tab[i];
        tab_r[i] = (a >> shift) | (l << (LIMB_BITS - shift));
        l = a;
    }
    return l & (((limb_t)1 << shift) - 1);
}

/* r = (a << shift) + low. 1 <= shift <= LIMB_BITS - 1, 0 <= low <
   2^shift. */
static limb_t mp_shl(limb_t *tab_r, const limb_t *tab, mp_size_t n, 
              int shift, limb_t low)
{
    mp_size_t i;
    limb_t l, a;

    assert(shift >= 1 && shift < LIMB_BITS);
    l = low;
    for(i = 0; i < n; i++) {
        a = tab[i];
        tab_r[i] = (a << shift) | l;
        l = (a >> (LIMB_BITS - shift)); 
    }
    return l;
}

static no_inline limb_t mp_div1norm(limb_t *tabr, const limb_t *taba, limb_t n,
                                    limb_t b, limb_t r, limb_t b_inv, int shift)
{
    slimb_t i;

    if (shift != 0) {
        r = (r << shift) | mp_shl(tabr, taba, n, shift, 0);
    }
    for(i = n - 1; i >= 0; i--) {
        tabr[i] = udiv1norm(&r, r, taba[i], b, b_inv);
    }
    r >>= shift;
    return r;
}

static __maybe_unused void mpb_dump(const char *str, const mpb_t *a)
{
    int i;
    
    printf("%s= 0x", str);
    for(i = a->len - 1; i >= 0; i--) {
        printf("%" PRIxLIMB, a->tab[i]);
        if (i != 0)
            printf("_");
    }
    printf("\n");
}

static void mpb_renorm(mpb_t *r)
{
    while (r->len > 1 && r->tab[r->len - 1] == 0)
        r->len--;
}

#ifdef USE_POW5_TABLE
static const uint32_t pow5_table[17] = {
    0x00000005, 0x00000019, 0x0000007d, 0x00000271, 
    0x00000c35, 0x00003d09, 0x0001312d, 0x0005f5e1, 
    0x001dcd65, 0x009502f9, 0x02e90edd, 0x0e8d4a51, 
    0x48c27395, 0x6bcc41e9, 0x1afd498d, 0x86f26fc1, 
    0xa2bc2ec5, 
};

static const uint8_t pow5h_table[4] = {
    0x00000001, 0x00000007, 0x00000023, 0x000000b1, 
};

#if LIMB_BITS == 32
/* Reciprocals precomputed for a 32-bit divisor; meaningless at another width. */
static const uint32_t pow5_inv_table[13] = {
    0x99999999, 0x47ae147a, 0x0624dd2f, 0xa36e2eb1,
    0x4f8b588e, 0x0c6f7a0b, 0xad7f29ab, 0x5798ee23,
    0x12e0be82, 0xb7cdfd9d, 0x5fd7fe17, 0x19799812,
    0xc25c2684,
};
#endif
#endif

/* return a^b */
static uint64_t pow_ui(uint32_t a, uint32_t b)
{
    int i, n_bits;
    uint64_t r;
    if (b == 0)
        return 1;
    if (b == 1)
        return a;
#ifdef USE_POW5_TABLE
    if ((a == 5 || a == 10) && b <= 17) {
        r = pow5_table[b - 1];
        if (b >= 14) {
            r |= (uint64_t)pow5h_table[b - 14] << 32;
        }
        if (a == 10)
            r <<= b;
        return r;
    }
#endif
    r = a;
    n_bits = 32 - clz32(b);
    for(i = n_bits - 2; i >= 0; i--) {
        r *= r;
        if ((b >> i) & 1)
            r *= a;
    }
    return r;
}

/* Returns a^b together with the normalisation shift and reciprocal that
 * udiv1norm needs. UNLIKE pow_ui, the result must fit in ONE LIMB: every caller
 * takes the exponent from digits_per_limb_table, so a^b < 2^LIMB_BITS by
 * construction. The assert pins that, because the truncation it prevents is
 * silent -- pow_ui returns 64 bits and a 32-bit limb build would just drop the
 * top half. pow5_inv_table holds reciprocals computed for a 32-bit divisor, so
 * it is only valid at that width. */
static limb_t pow_ui_inv(limb_t *pr_inv, int *pshift, uint32_t a, uint32_t b)
{
    limb_t r_inv, r;
    int shift;
#if LIMB_BITS == 32 && defined(USE_POW5_TABLE)
    if (a == 5 && b >= 1 && b <= 13) {
        r = pow5_table[b - 1];
        shift = clz_limb(r);
        r <<= shift;
        r_inv = pow5_inv_table[b - 1];
    } else
#endif
    {
        uint64_t r64 = pow_ui(a, b);
        assert(LIMB_BITS == 64 || r64 <= 0xffffffffu);
        r = (limb_t)r64;
        shift = clz_limb(r);
        r <<= shift;
        r_inv = udiv1norm_init(r);
    }
    *pshift = shift;
    *pr_inv = r_inv;
    return r;
}

enum {
    JS_RNDN, /* round to nearest, ties to even */
    JS_RNDNA, /* round to nearest, ties away from zero */
    JS_RNDZ,
};

static int mpb_get_bit(const mpb_t *r, int k)
{
    int l;
    
    l = (unsigned)k / LIMB_BITS;
    k = k & (LIMB_BITS - 1);
    if (l >= r->len)
        return 0;
    else
        return (r->tab[l] >> k) & 1;
}

/* compute round(r / 2^shift). 'shift' can be negative */
static void mpb_shr_round(mpb_t *r, int shift, int rnd_mode)
{
    int l, i;

    if (shift == 0)
        return;
    if (shift < 0) {
        shift = -shift;
        l = (unsigned)shift / LIMB_BITS;
        shift = shift & (LIMB_BITS - 1);
        if (shift != 0) {
            r->tab[r->len] = mp_shl(r->tab, r->tab, r->len, shift, 0);
            r->len++;
            mpb_renorm(r);
        }
        if (l > 0) {
            for(i = r->len - 1; i >= 0; i--)
                r->tab[i + l] = r->tab[i];
            for(i = 0; i < l; i++)
                r->tab[i] = 0;
            r->len += l;
        }
    } else {
        limb_t bit1, bit2;
        int k, add_one;
        
        switch(rnd_mode) {
        default:
        case JS_RNDZ:
            add_one = 0;
            break;
        case JS_RNDN:
        case JS_RNDNA:
            bit1 = mpb_get_bit(r, shift - 1);
            if (bit1) {
                if (rnd_mode == JS_RNDNA) {
                    bit2 = 1;
                } else {
                    /* bit2 = oring of all the bits after bit1 */
                    bit2 = 0;
                    if (shift >= 2) {
                        k = shift - 1;
                        l = (unsigned)k / LIMB_BITS;
                        k = k & (LIMB_BITS - 1);
                        for(i = 0; i < min_int(l, r->len); i++)
                            bit2 |= r->tab[i];
                        if (l < r->len)
                            bit2 |= r->tab[l] & (((limb_t)1 << k) - 1);
                    }
                }
                if (bit2) {
                    add_one = 1;
                } else {
                    /* round to even */
                    add_one = mpb_get_bit(r, shift);
                }
            } else {
                add_one = 0;
            }
            break;
        }

        l = (unsigned)shift / LIMB_BITS;
        shift = shift & (LIMB_BITS - 1);
        if (l >= r->len) {
            r->len = 1;
            r->tab[0] = add_one;
        } else {
            if (l > 0) {
                r->len -= l;
                for(i = 0; i < r->len; i++)
                    r->tab[i] = r->tab[i + l];
            }
            if (shift != 0) {
                mp_shr(r->tab, r->tab, r->len, shift, 0);
                mpb_renorm(r);
            }
            if (add_one) {
                limb_t a;
                a = mp_add_ui(r->tab, 1, r->len);
                if (a)
                    r->tab[r->len++] = a;
            }
        }
    }
}

/* return -1, 0 or 1 */
static int mpb_cmp(const mpb_t *a, const mpb_t *b)
{
    mp_size_t i;
    if (a->len < b->len)
        return -1;
    else if (a->len > b->len)
        return 1;
    for(i = a->len - 1; i >= 0; i--) {
        if (a->tab[i] != b->tab[i]) {
            if (a->tab[i] < b->tab[i])
                return -1;
            else
                return 1;
        }
    }
    return 0;
}

static void mpb_set_u64(mpb_t *r, uint64_t m)
{
#if LIMB_BITS == 64
    r->tab[0] = m;
    r->len = 1;
#else
    r->tab[0] = m;
    r->tab[1] = m >> LIMB_BITS;
    if (r->tab[1] == 0)
        r->len = 1;
    else
        r->len = 2;
#endif
}

static uint64_t mpb_get_u64(mpb_t *r)
{
#if LIMB_BITS == 64
    return r->tab[0];
#else
    if (r->len == 1) {
        return r->tab[0];
    } else {
        return r->tab[0] | ((uint64_t)r->tab[1] << LIMB_BITS);
    }
#endif
}

/* floor_log2() = position of the first non zero bit or -1 if zero. */
static int mpb_floor_log2(mpb_t *a)
{
    limb_t v;
    v = a->tab[a->len - 1];
    if (v == 0)
        return -1;
    else
        /* v is a LIMB, so the count must be limb-wide: clz32 here reports the
           wrong exponent at 64-bit limbs, and the free format then fails to find
           the shortest representation (1e+300 printed as 17 digits). */
        return a->len * LIMB_BITS - 1 - clz_limb(v);
}

#define MUL_LOG2_RADIX_BASE_LOG2 24

/* round((1 << MUL_LOG2_RADIX_BASE_LOG2)/log2(i + 2)) */
static const uint32_t mul_log2_radix_table[JS_RADIX_MAX - 1] = {
    0x000000, 0xa1849d, 0x000000, 0x6e40d2, 
    0x6308c9, 0x5b3065, 0x000000, 0x50c24e, 
    0x4d104d, 0x4a0027, 0x4768ce, 0x452e54, 
    0x433d00, 0x418677, 0x000000, 0x3ea16b, 
    0x3d645a, 0x3c43c2, 0x3b3b9a, 0x3a4899, 
    0x39680b, 0x3897b3, 0x37d5af, 0x372069, 
    0x367686, 0x35d6df, 0x354072, 0x34b261, 
    0x342bea, 0x33ac62, 0x000000, 0x32bfd9, 
    0x3251dd, 0x31e8d6, 0x318465,
};

/* return floor(a / log2(radix)) for -2048 <= a <= 2047 */
static int mul_log2_radix(int a, int radix)
{
    int radix_bits, mult;

    if ((radix & (radix - 1)) == 0) {
        /* if the radix is a power of two better to do it exactly */
        radix_bits = 31 - clz32(radix);
        if (a < 0)
            a -= radix_bits - 1;
        return a / radix_bits;
    } else {
        mult = mul_log2_radix_table[radix - 2];
        return ((int64_t)a * mult) >> MUL_LOG2_RADIX_BASE_LOG2;
    }
}

#if 0
static void build_mul_log2_radix_table(void)
{
    int base, radix, mult, col, base_log2;

    base_log2 = 24;
    base = 1 << base_log2;
    col = 0;
    for(radix = 2; radix <= 36; radix++) {
        if ((radix & (radix - 1)) == 0)
            mult = 0;
        else
            mult = lrint((double)base / log2(radix));
        printf("0x%06x, ", mult);
        if (++col == 4) {
            printf("\n");
            col = 0;
        }
    }
    printf("\n");
}

static void mul_log2_radix_test(void)
{
    int radix, i, ref, r;
    
    for(radix = 2; radix <= 36; radix++) {
        double l2r = log2(radix);      /* invariant across the inner 4096 */
        for(i = -2048; i <= 2047; i++) {
            ref = (int)floor((double)i / l2r);
            r = mul_log2_radix(i, radix);
            if (ref != r) {
                printf("ERROR: radix=%d i=%d r=%d ref=%d\n",
                       radix, i, r, ref);
                exit(1);
            }
        }
    }
    if (0)
        build_mul_log2_radix_table();
}
#endif

/* two ASCII decimal digits per byte-pair, indexed by (value * 2); the array
   is exactly 200 bytes plus the literal's implicit NUL (unused) */
static const char digits_pair[] =
    "0001020304050607080910111213141516171819"
    "2021222324252627282930313233343536373839"
    "4041424344454647484950515253545556575859"
    "6061626364656667686970717273747576777879"
    "8081828384858687888990919293949596979899";

static void u32toa_len(char *buf, uint32_t n, size_t len)
{
    int digit, i;
    for(i = len - 1; i >= 0; i--) {
        digit = n % 10;
        n = n / 10;
        buf[i] = digit + '0';
    }
}

/* for power of 2 radixes. len >= 1 */
static void u64toa_bin_len(char *buf, uint64_t n, unsigned int radix_bits, int len)
{
    int digit, i;
    unsigned int mask;

    mask = (1 << radix_bits) - 1;
    for(i = len - 1; i >= 0; i--) {
        digit = n & mask;
        n >>= radix_bits;
        if (digit < 10)
            digit += '0';
        else
            digit += 'a' - 10;
        buf[i] = digit;
    }
}

/* len >= 1. 2 <= radix <= 36 */
static void limb_to_a(char *buf, limb_t n, unsigned int radix, int len)
{
    int digit, i;

    if (radix == 10) {
        /* specific case with constant divisor */
#if LIMB_BITS == 32
        u32toa_len(buf, n, len);
#else
        /* XXX: optimize */
        for(i = len - 1; i >= 0; i--) {
            digit = (limb_t)n % 10;
            n = (limb_t)n / 10;
            buf[i] = digit + '0';
        }
#endif
    } else {
        for(i = len - 1; i >= 0; i--) {
            digit = (limb_t)n % radix;
            n = (limb_t)n / radix;
            if (digit < 10)
                digit += '0';
            else
                digit += 'a' - 10;
            buf[i] = digit;
        }
    }
}

size_t u32toa(char *buf, uint32_t n)
{
    char buf1[10], *q;
    size_t len;

    q = buf1 + sizeof(buf1);
    /* two digits per step; the tail emits the leading 1 or 2 digits */
    while (n >= 100) {
        uint32_t r = (n % 100) * 2;
        n /= 100;
        *--q = digits_pair[r + 1];
        *--q = digits_pair[r];
    }
    if (n >= 10) {
        uint32_t r = n * 2;
        *--q = digits_pair[r + 1];
        *--q = digits_pair[r];
    } else {
        *--q = n + '0';
    }
    len = buf1 + sizeof(buf1) - q;
    memcpy(buf, q, len);
    return len;
}

size_t i32toa(char *buf, int32_t n)
{
    if (n >= 0) {
        return u32toa(buf, n);
    } else {
        buf[0] = '-';
        return u32toa(buf + 1, -(uint32_t)n) + 1;
    }
}

#ifdef USE_FAST_INT
size_t u64toa(char *buf, uint64_t n)
{
    if (n < 0x100000000) {
        return u32toa(buf, n);
    } else {
        uint64_t n1;
        char *q = buf;
        uint32_t n2;
        
        n1 = n / 1000000000;
        n %= 1000000000;
        if (n1 >= 0x100000000) {
            n2 = n1 / 1000000000;
            n1 = n1 % 1000000000;
            /* at most two digits */
            if (n2 >= 10) {
                *q++ = n2 / 10 + '0';
                n2 %= 10;
            }
            *q++ = n2 + '0';
            u32toa_len(q, n1, 9);
            q += 9;
        } else {
            q += u32toa(q, n1);
        }
        u32toa_len(q, n, 9);
        q += 9;
        return q - buf;
    }
}

size_t i64toa(char *buf, int64_t n)
{
    if (n >= 0) {
        return u64toa(buf, n);
    } else {
        buf[0] = '-';
        return u64toa(buf + 1, -(uint64_t)n) + 1;
    }
}

/* XXX: only tested for 1 <= n < 2^53 */
size_t u64toa_radix(char *buf, uint64_t n, unsigned int radix)
{
    int radix_bits, l;
    if (likely(radix == 10))
        return u64toa(buf, n);
    if ((radix & (radix - 1)) == 0) {
        radix_bits = 31 - clz32(radix);
        if (n == 0)
            l = 1;
        else
            l = (64 - clz64(n) + radix_bits - 1) / radix_bits;
        u64toa_bin_len(buf, n, radix_bits, l);
        return l;
    } else {
        char buf1[41], *q; /* maximum length for radix = 3 */
        size_t len;
        int digit;
        q = buf1 + sizeof(buf1);
        do {
            digit = n % radix;
            n /= radix;
            if (digit < 10)
                digit += '0';
            else
                digit += 'a' - 10;
            *--q = digit;
        } while (n != 0);
        len = buf1 + sizeof(buf1) - q;
        memcpy(buf, q, len);
        return len;
    }
}

size_t i64toa_radix(char *buf, int64_t n, unsigned int radix)
{
    if (n >= 0) {
        return u64toa_radix(buf, n, radix);
    } else {
        buf[0] = '-';
        return u64toa_radix(buf + 1, -(uint64_t)n, radix) + 1;
    }
}
#endif /* USE_FAST_INT */

static const uint8_t digits_per_limb_table[JS_RADIX_MAX - 1] = {
#if LIMB_BITS == 32
32,20,16,13,12,11,10,10, 9, 9, 8, 8, 8, 8, 8, 7, 7, 7, 7, 7, 7, 7, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6,
#else
64,40,32,27,24,22,21,20,19,18,17,17,16,16,16,15,15,15,14,14,14,14,13,13,13,13,13,13,13,12,12,12,12,12,12,
#endif
};

/* radix^digits_per_limb_table[radix-2], reduced mod 2^LIMB_BITS -- so the
 * power-of-two radices are 0, which the shift path handles instead. The VALUES
 * depend on the limb width and the compiler cannot see that: a uint32_t table
 * promotes silently into the limb-wide divisor and simply computes the wrong
 * answer. Regenerate with tools/gen-dtoa-radix.py. */
#if LIMB_BITS == 64
static const limb_t radix_base_table[JS_RADIX_MAX - 1] = {
 0x0000000000000000ULL, 0xA8B8B452291FE821ULL, 0x0000000000000000ULL,
 0x6765C793FA10079DULL, 0x41C21CB8E1000000ULL, 0x3642798750226111ULL,
 0x8000000000000000ULL, 0xA8B8B452291FE821ULL, 0x8AC7230489E80000ULL,
 0x4D28CB56C33FA539ULL, 0x1ECA170C00000000ULL, 0x780C7372621BD74DULL,
 0x1E39A5057D810000ULL, 0x5B27AC993DF97701ULL, 0x0000000000000000ULL,
 0x27B95E997E21D9F1ULL, 0x5DA0E1E53C5C8000ULL, 0xD2AE3299C1C4AEDBULL,
 0x16BCC41E90000000ULL, 0x2D04B7FDD9C0EF49ULL, 0x5658597BCAA24000ULL,
 0xA0E2073737609371ULL, 0x0C29E98000000000ULL, 0x14ADF4B7320334B9ULL,
 0x226ED36478BFA000ULL, 0x383D9170B85FF80BULL, 0x5A3C23E39C000000ULL,
 0x8E65137388122BCDULL, 0xDD41BB36D259E000ULL, 0x0AEE5720EE830681ULL,
 0x1000000000000000ULL, 0x172588AD4F5F0981ULL, 0x211E44F7D02C1000ULL,
 0x2EE56725F06E5C71ULL, 0x41C21CB8E1000000ULL,
};
#else
static const uint32_t radix_base_table[JS_RADIX_MAX - 1] = {
 0x00000000, 0xcfd41b91, 0x00000000, 0x48c27395,
 0x81bf1000, 0x75db9c97, 0x40000000, 0xcfd41b91,
 0x3b9aca00, 0x8c8b6d2b, 0x19a10000, 0x309f1021,
 0x57f6c100, 0x98c29b81, 0x00000000, 0x18754571,
 0x247dbc80, 0x3547667b, 0x4c4b4000, 0x6b5a6e1d,
 0x94ace180, 0xcaf18367, 0x0b640000, 0x0e8d4a51,
 0x1269ae40, 0x17179149, 0x1cb91000, 0x23744899,
 0x2b73a840, 0x34e63b41, 0x40000000, 0x4cfa3cc1,
 0x5c13d840, 0x6d91b519, 0x81bf1000,
};
#endif

/* XXX: remove the table ? */
static uint8_t dtoa_max_digits_table[JS_RADIX_MAX - 1] = {
    54, 35, 28, 24, 22, 20, 19, 18, 17, 17, 16, 16, 15, 15, 15, 14, 14, 14, 14, 14, 13, 13, 13, 13, 13, 13, 13, 12, 12, 12, 12, 12, 12, 12, 12,
};

/* we limit the maximum number of significant digits for atod to about
   128 bits of precision for non power of two bases. The only
   requirement for Javascript is at least 20 digits in base 10. For
   power of two bases, we do an exact rounding in all the cases. */
static uint8_t atod_max_digits_table[JS_RADIX_MAX - 1] = {
     64, 80, 32, 55, 49, 45, 21, 40, 38, 37, 35, 34, 33, 32, 16, 31, 30, 30, 29, 29, 28, 28, 27, 27, 27, 26, 26, 26, 26, 25, 12, 25, 25, 24, 24,
};

/* if abs(d) >= B^max_exponent, it is an overflow */
static const int16_t max_exponent[JS_RADIX_MAX - 1] = {
 1024,   647,   512,   442,   397,   365,   342,   324, 
  309,   297,   286,   277,   269,   263,   256,   251, 
  246,   242,   237,   234,   230,   227,   224,   221, 
  218,   216,   214,   211,   209,   207,   205,   203, 
  202,   200,   199, 
};

/* if abs(d) <= B^min_exponent, it is an underflow */
static const int16_t min_exponent[JS_RADIX_MAX - 1] = {
-1075,  -679,  -538,  -463,  -416,  -383,  -359,  -340, 
 -324,  -311,  -300,  -291,  -283,  -276,  -269,  -263, 
 -258,  -254,  -249,  -245,  -242,  -238,  -235,  -232, 
 -229,  -227,  -224,  -222,  -220,  -217,  -215,  -214, 
 -212,  -210,  -208, 
};

#if 0
void build_tables(void)
{
    int r, j, radix, n, col, i;
    
    /* radix_base_table */
    for(radix = 2; radix <= 36; radix++) {
        r = 1;
        for(j = 0; j < digits_per_limb_table[radix - 2]; j++) {
            r *= radix;
        }
        printf(" 0x%08x,", r);
        if ((radix % 4) == 1)
            printf("\n");
    }
    printf("\n");

    /* dtoa_max_digits_table */
    for(radix = 2; radix <= 36; radix++) {
        /* Note: over estimated when the radix is a power of two */
        printf(" %d,", 1 + (int)ceil(53.0 / log2(radix)));
    }
    printf("\n");

    /* atod_max_digits_table */
    for(radix = 2; radix <= 36; radix++) {
        if ((radix & (radix - 1)) == 0) {
            /* 64 bits is more than enough */
            n = (int)floor(64.0 / log2(radix));
        } else {
            n = (int)floor(128.0 / log2(radix));
        }
        printf(" %d,", n);
    }
    printf("\n");

    printf("static const int16_t max_exponent[JS_RADIX_MAX - 1] = {\n");
    col = 0;
    for(radix = 2; radix <= 36; radix++) {
        printf("%5d, ", (int)ceil(1024 / log2(radix)));
        if (++col == 8) {
            col = 0;
            printf("\n");
        }
    }
    printf("\n};\n\n");

    printf("static const int16_t min_exponent[JS_RADIX_MAX - 1] = {\n");
    col = 0; 
    for(radix = 2; radix <= 36; radix++) {
        printf("%5d, ", (int)floor(-1075 / log2(radix)));
        if (++col == 8) {
            col = 0;
            printf("\n");
        }
    }
    printf("\n};\n\n");

    printf("static const uint32_t pow5_table[16] = {\n");
    col = 0; 
    for(i = 2; i <= 17; i++) {
        r = 1;
        for(j = 0; j < i; j++) {
            r *= 5;
        }
        printf("0x%08x, ", r);
        if (++col == 4) {
            col = 0;
            printf("\n");
        }
    }
    printf("\n};\n\n");

    /* high part */
    printf("static const uint8_t pow5h_table[4] = {\n");
    col = 0; 
    for(i = 14; i <= 17; i++) {
        uint64_t r1;
        r1 = 1;
        for(j = 0; j < i; j++) {
            r1 *= 5;
        }
        printf("0x%08x, ", (uint32_t)(r1 >> 32));
        if (++col == 4) {
            col = 0;
            printf("\n");
        }
    }
    printf("\n};\n\n");
}
#endif

/* n_digits >= 1. 0 <= dot_pos <= n_digits. If dot_pos == n_digits,
   the dot is not displayed. 'a' is modified. */
static int output_digits(char *buf,
                         mpb_t *a, int radix, int n_digits1,
                         int dot_pos)
{
    int n_digits, digits_per_limb, radix_bits, n, len;

    n_digits = n_digits1;
    if ((radix & (radix - 1)) == 0) {
        /* radix = 2^radix_bits */
        radix_bits = 31 - clz32(radix);
    } else {
        radix_bits = 0;
    }
    digits_per_limb = digits_per_limb_table[radix - 2];
    if (radix_bits != 0) {
        for(;;) {
            n = min_int(n_digits, digits_per_limb);
            n_digits -= n;
            u64toa_bin_len(buf + n_digits, a->tab[0], radix_bits, n);
            if (n_digits == 0)
                break;
            mpb_shr_round(a, digits_per_limb * radix_bits, JS_RNDZ);
        }
    } else {
        limb_t r;
        while (n_digits != 0) {
            n = min_int(n_digits, digits_per_limb);
            n_digits -= n;
            r = mp_div1(a->tab, a->tab, a->len, radix_base_table[radix - 2], 0);
            mpb_renorm(a);
            limb_to_a(buf + n_digits, r, radix, n);
        }
    }

    /* add the dot */
    len = n_digits1;
    if (dot_pos != n_digits1) {
        memmove(buf + dot_pos + 1, buf + dot_pos, n_digits1 - dot_pos);
        buf[dot_pos] = '.';
        len++;
    }
    return len;
}

/* return (a, e_offset) such that a = a * (radix1*2^radix_shift)^f *
   2^-e_offset. 'f' can be negative. */
static int mul_pow(mpb_t *a, int radix1, int radix_shift, int f, BOOL is_int, int e)
{
    int e_offset, d, n, n0;

    e_offset = -f * radix_shift;
    if (radix1 != 1) {
        d = digits_per_limb_table[radix1 - 2];
        if (f >= 0) {
            limb_t h, b;
            
            b = 0;
            n0 = 0;
            while (f != 0) {
                n = min_int(f, d);
                if (n != n0) {
                    b = pow_ui(radix1, n);
                    n0 = n;
                }
                h = mp_mul1(a->tab, a->tab, a->len, b, 0);
                if (h != 0) {
                    a->tab[a->len++] = h;
                }
                f -= n;
            }
        } else {
            int extra_bits, l, shift;
            limb_t r, rem, b, b_inv;
            
            f = -f;
            l = (f + d - 1) / d; /* high bound for the number of limbs (XXX: make it better) */
            e_offset += l * LIMB_BITS;
            if (!is_int) {
                /* at least 'e' bits are needed in the final result for rounding */
                extra_bits = max_int(e - mpb_floor_log2(a), 0);
            } else {
                /* at least two extra bits are needed in the final result
                   for rounding */
                extra_bits = max_int(2 + e - e_offset, 0);
            }
            e_offset += extra_bits;
            mpb_shr_round(a, -(l * LIMB_BITS + extra_bits), JS_RNDZ);
            
            b = 0;
            b_inv = 0;
            shift = 0;
            n0 = 0;
            rem = 0;
            while (f != 0) {
                n = min_int(f, d);
                if (n != n0) {
                    b = pow_ui_inv(&b_inv, &shift, radix1, n);
                    n0 = n;
                }
                r = mp_div1norm(a->tab, a->tab, a->len, b, 0, b_inv, shift);
                rem |= r;
                mpb_renorm(a);
                f -= n;
            }
            /* if the remainder is non zero, use it for rounding */
            a->tab[0] |= (rem != 0);
        }
    }
    return e_offset;
}

/* tmp1 = round(m*2^e*radix^f). 'tmp0' is a temporary storage */
static void mul_pow_round(mpb_t *tmp1, uint64_t m, int e, int radix1, int radix_shift, int f,
                          int rnd_mode)
{
    int e_offset;

    mpb_set_u64(tmp1, m);
    e_offset = mul_pow(tmp1, radix1, radix_shift, f, TRUE, e);
    mpb_shr_round(tmp1, -e + e_offset, rnd_mode);
}

/* return round(a*2^e_offset) rounded as a float64. 'a' is modified */
static uint64_t round_to_d(int *pe, mpb_t *a, int e_offset, int rnd_mode)
{
    int e;
    uint64_t m;

    if (a->tab[0] == 0 && a->len == 1) {
        /* zero result */
        m = 0;
        e = 0; /* don't care */
    } else {
        int prec, prec1, e_min;
        e = mpb_floor_log2(a) + 1 - e_offset;
        prec1 = 53;
        e_min = -1021;
        if (e < e_min) {
            /* subnormal result or zero */
            prec = prec1 - (e_min - e);
        } else {
            prec = prec1;
        }
        mpb_shr_round(a, e + e_offset - prec, rnd_mode);
        m = mpb_get_u64(a);
        m <<= (53 - prec);
        /* mantissa overflow due to rounding */
        if (m >= (uint64_t)1 << 53) {
            m >>= 1;
            e++;
        }
    }
    *pe = e;
    return m;
}

/* return (m, e) such that m*2^(e-53) = round(a * radix^f) with 2^52
   <= m < 2^53 or m = 0.
   'a' is modified. */
static uint64_t mul_pow_round_to_d(int *pe, mpb_t *a,
                                   int radix1, int radix_shift, int f, int rnd_mode)
{
    int e_offset;

    e_offset = mul_pow(a, radix1, radix_shift, f, FALSE, 55);
    return round_to_d(pe, a, e_offset, rnd_mode);
}

#ifdef JS_DTOA_DUMP_STATS
static int out_len_count[17];

void js_dtoa_dump_stats(void)
{
    int i, sum;
    sum = 0;
    for(i = 0; i < 17; i++)
        sum += out_len_count[i];
    for(i = 0; i < 17; i++) {
        printf("%2d %8d %5.2f%%\n",
               i + 1, out_len_count[i], (double)out_len_count[i] / sum * 100);
    }
}
#endif

#if defined(__SIZEOF_INT128__)

typedef unsigned __int128 grisu_u128;

/* ------------------------------------------------------------------ */
/* Grisu-style shortest fast path for radix 10, JS_DTOA_FORMAT_FREE.   */
/*                                                                     */
/* The exact search below recomputes mul_pow_round() plus the          */
/* mul_pow_round_to_d() round-trip check for *every* candidate         */
/* precision P = 17..P_found; for d = 1e300 that is ~34 bignum         */
/* scalings by 10^+-284, each a per-limb software division             */
/* (mp_div1norm/udiv1norm), which is essentially the whole cost of     */
/* (1e300).toString().                                                 */
/*                                                                     */
/* The fast path computes, with plain 128-bit integer arithmetic:      */
/*   - the 17 significant digits in ONE fixed-point product with a     */
/*     precomputed 128-bit power of ten (error <= 2 ulp),              */
/*   - the scaled round-trip boundaries (2m-1)/2, (2m+1)/2 scaled by   */
/*     the same cached power,                                          */
/* and then finds the minimum precision P whose correctly-rounded      */
/* P-digit mantissa provably round-trips, accepting a verdict only     */
/* when the comparison margins (>= 12 scaled ulp) dwarf the arithmetic */
/* error (<= 2 ulp). Any ambiguity (boundary touch, exact decimal      */
/* tie, ambiguous digit rounding, the E++ adjust of the exact loop,    */
/* or a failed range sanity check) returns FALSE and the original      */
/* exact search runs unchanged. When it returns TRUE the result        */
/* (mant, P, E) is provably the triple the exact search produces:      */
/* round-tripping precisions form a contiguous range [P_min, 17]       */
/* (a finer rounding is at least as close to d, and the round-trip     */
/* interval is centered on d), and the engine's P_found is that P_min  */
/* (trailing-zero stripping preserves the round-trip value, so the     */
/* minimum cannot itself have a trailing zero).                        */
/*                                                                     */
/* Boundaries: d = m*2^(e-53), 2^52 <= m < 2^53, e in [-1073, 1024].   */
/* The set of reals rounding to d is the closed interval               */
/* [(2m-gb)*2^(e-54), (2m+ga)*2^(e-54)] where for NORMALS ga = 1 and   */
/* gb = 1 (gb = 2 when m == 2^52: the gap below a power of two is half */
/* an ulp), and for DENORMALS (e <= -1022) both gaps are the true      */
/* denormal grid step 2^-1074 = 2^l*2^(e-53) with l = -1021-e in       */
/* [1,52], i.e. ga = gb = 2^l. The RNDN ties-to-even endpoint parity   */
/* matters only when a candidate lands EXACTLY on a boundary -- and    */
/* exact boundary contact is inside the uncertainty window, so the     */
/* fast path falls back and the exact loop resolves parity.            */
/* Normalized 64-bit significands, all at binary exponent e - 64.      */
/* Scaling by cached power C ~= 10^(17-E) (alpha = 17-E, so the scaled */
/* value is the 17-digit mantissa in [10^16, 10^17)):                  */
/*   A2 ~= x*10^(17-E), Bp2/Bm2 ~= upper/lower boundary scaled,        */
/* all as u128 fixed-point at binary exponent Gamma = e + ec.          */
/* Generated by tools/gen-dtoa-grisu.py -- do not edit. */
#define GRISU_ALPHA_MIN (-294)
#define GRISU_ALPHA_MAX (342)
#define GRISU_ALPHA_COUNT (GRISU_ALPHA_MAX - GRISU_ALPHA_MIN + 1)

/* Cached powers 10^alpha as 128-bit normalized significands:
   (hi:lo) * 2^ec ~= 10^alpha with |err| <= 2^-128 relative. */
static const uint64_t grisu_pow_hi[GRISU_ALPHA_COUNT] = {
    0xA37FCE126597973CULL, 0xCC5FC196FEFD7D0CULL, 0xFF77B1FCBEBCDC4FULL,
    0x9FAACF3DF73609B1ULL, 0xC795830D75038C1DULL, 0xF97AE3D0D2446F25ULL,
    0x9BECCE62836AC577ULL, 0xC2E801FB244576D5ULL, 0xF3A20279ED56D48AULL,
    0x9845418C345644D6ULL, 0xBE5691EF416BD60CULL, 0xEDEC366B11C6CB8FULL,
    0x94B3A202EB1C3F39ULL, 0xB9E08A83A5E34F07ULL, 0xE858AD248F5C22C9ULL,
    0x91376C36D99995BEULL, 0xB58547448FFFFB2DULL, 0xE2E69915B3FFF9F9ULL,
    0x8DD01FAD907FFC3BULL, 0xB1442798F49FFB4AULL, 0xDD95317F31C7FA1DULL,
    0x8A7D3EEF7F1CFC52ULL, 0xAD1C8EAB5EE43B66ULL, 0xD863B256369D4A40ULL,
    0x873E4F75E2224E68ULL, 0xA90DE3535AAAE202ULL, 0xD3515C2831559A83ULL,
    0x8412D9991ED58091ULL, 0xA5178FFF668AE0B6ULL, 0xCE5D73FF402D98E3ULL,
    0x80FA687F881C7F8EULL, 0xA139029F6A239F72ULL, 0xC987434744AC874EULL,
    0xFBE9141915D7A922ULL, 0x9D71AC8FADA6C9B5ULL, 0xC4CE17B399107C22ULL,
    0xF6019DA07F549B2BULL, 0x99C102844F94E0FBULL, 0xC0314325637A1939ULL,
    0xF03D93EEBC589F88ULL, 0x96267C7535B763B5ULL, 0xBBB01B9283253CA2ULL,
    0xEA9C227723EE8BCBULL, 0x92A1958A7675175FULL, 0xB749FAED14125D36ULL,
    0xE51C79A85916F484ULL, 0x8F31CC0937AE58D2ULL, 0xB2FE3F0B8599EF07ULL,
    0xDFBDCECE67006AC9ULL, 0x8BD6A141006042BDULL, 0xAECC49914078536DULL,
    0xDA7F5BF590966848ULL, 0x888F99797A5E012DULL, 0xAAB37FD7D8F58178ULL,
    0xD5605FCDCF32E1D6ULL, 0x855C3BE0A17FCD26ULL, 0xA6B34AD8C9DFC06FULL,
    0xD0601D8EFC57B08BULL, 0x823C12795DB6CE57ULL, 0xA2CB1717B52481EDULL,
    0xCB7DDCDDA26DA268ULL, 0xFE5D54150B090B02ULL, 0x9EFA548D26E5A6E1ULL,
    0xC6B8E9B0709F109AULL, 0xF867241C8CC6D4C0ULL, 0x9B407691D7FC44F8ULL,
    0xC21094364DFB5636ULL, 0xF294B943E17A2BC4ULL, 0x979CF3CA6CEC5B5AULL,
    0xBD8430BD08277231ULL, 0xECE53CEC4A314EBDULL, 0x940F4613AE5ED136ULL,
    0xB913179899F68584ULL, 0xE757DD7EC07426E5ULL, 0x9096EA6F3848984FULL,
    0xB4BCA50B065ABE63ULL, 0xE1EBCE4DC7F16DFBULL, 0x8D3360F09CF6E4BDULL,
    0xB080392CC4349DECULL, 0xDCA04777F541C567ULL, 0x89E42CAAF9491B60ULL,
    0xAC5D37D5B79B6239ULL, 0xD77485CB25823AC7ULL, 0x86A8D39EF77164BCULL,
    0xA8530886B54DBDEBULL, 0xD267CAA862A12D66ULL, 0x8380DEA93DA4BC60ULL,
    0xA46116538D0DEB78ULL, 0xCD795BE870516656ULL, 0x806BD9714632DFF6ULL,
    0xA086CFCD97BF97F3ULL, 0xC8A883C0FDAF7DF0ULL, 0xFAD2A4B13D1B5D6CULL,
    0x9CC3A6EEC6311A63ULL, 0xC3F490AA77BD60FCULL, 0xF4F1B4D515ACB93BULL,
    0x991711052D8BF3C5ULL, 0xBF5CD54678EEF0B6ULL, 0xEF340A98172AACE4ULL,
    0x9580869F0E7AAC0EULL, 0xBAE0A846D2195712ULL, 0xE998D258869FACD7ULL,
    0x91FF83775423CC06ULL, 0xB67F6455292CBF08ULL, 0xE41F3D6A7377EECAULL,
    0x8E938662882AF53EULL, 0xB23867FB2A35B28DULL, 0xDEC681F9F4C31F31ULL,
    0x8B3C113C38F9F37EULL, 0xAE0B158B4738705EULL, 0xD98DDAEE19068C76ULL,
    0x87F8A8D4CFA417C9ULL, 0xA9F6D30A038D1DBCULL, 0xD47487CC8470652BULL,
    0x84C8D4DFD2C63F3BULL, 0xA5FB0A17C777CF09ULL, 0xCF79CC9DB955C2CCULL,
    0x81AC1FE293D599BFULL, 0xA21727DB38CB002FULL, 0xCA9CF1D206FDC03BULL,
    0xFD442E4688BD304AULL, 0x9E4A9CEC15763E2EULL, 0xC5DD44271AD3CDBAULL,
    0xF7549530E188C128ULL, 0x9A94DD3E8CF578B9ULL, 0xC13A148E3032D6E7ULL,
    0xF18899B1BC3F8CA1ULL, 0x96F5600F15A7B7E5ULL, 0xBCB2B812DB11A5DEULL,
    0xEBDF661791D60F56ULL, 0x936B9FCEBB25C995ULL, 0xB84687C269EF3BFBULL,
    0xE65829B3046B0AFAULL, 0x8FF71A0FE2C2E6DCULL, 0xB3F4E093DB73A093ULL,
    0xE0F218B8D25088B8ULL, 0x8C974F7383725573ULL, 0xAFBD2350644EEACFULL,
    0xDBAC6C247D62A583ULL, 0x894BC396CE5DA772ULL, 0xAB9EB47C81F5114FULL,
    0xD686619BA27255A2ULL, 0x8613FD0145877585ULL, 0xA798FC4196E952E7ULL,
    0xD17F3B51FCA3A7A0ULL, 0x82EF85133DE648C4ULL, 0xA3AB66580D5FDAF5ULL,
    0xCC963FEE10B7D1B3ULL, 0xFFBBCFE994E5C61FULL, 0x9FD561F1FD0F9BD3ULL,
    0xC7CABA6E7C5382C8ULL, 0xF9BD690A1B68637BULL, 0x9C1661A651213E2DULL,
    0xC31BFA0FE5698DB8ULL, 0xF3E2F893DEC3F126ULL, 0x986DDB5C6B3A76B7ULL,
    0xBE89523386091465ULL, 0xEE2BA6C0678B597FULL, 0x94DB483840B717EFULL,
    0xBA121A4650E4DDEBULL, 0xE896A0D7E51E1566ULL, 0x915E2486EF32CD60ULL,
    0xB5B5ADA8AAFF80B8ULL, 0xE3231912D5BF60E6ULL, 0x8DF5EFABC5979C8FULL,
    0xB1736B96B6FD83B3ULL, 0xDDD0467C64BCE4A0ULL, 0x8AA22C0DBEF60EE4ULL,
    0xAD4AB7112EB3929DULL, 0xD89D64D57A607744ULL, 0x87625F056C7C4A8BULL,
    0xA93AF6C6C79B5D2DULL, 0xD389B47879823479ULL, 0x843610CB4BF160CBULL,
    0xA54394FE1EEDB8FEULL, 0xCE947A3DA6A9273EULL, 0x811CCC668829B887ULL,
    0xA163FF802A3426A8ULL, 0xC9BCFF6034C13052ULL, 0xFC2C3F3841F17C67ULL,
    0x9D9BA7832936EDC0ULL, 0xC5029163F384A931ULL, 0xF64335BCF065D37DULL,
    0x99EA0196163FA42EULL, 0xC06481FB9BCF8D39ULL, 0xF07DA27A82C37088ULL,
    0x964E858C91BA2655ULL, 0xBBE226EFB628AFEAULL, 0xEADAB0ABA3B2DBE5ULL,
    0x92C8AE6B464FC96FULL, 0xB77ADA0617E3BBCBULL, 0xE55990879DDCAABDULL,
    0x8F57FA54C2A9EAB6ULL, 0xB32DF8E9F3546564ULL, 0xDFF9772470297EBDULL,
    0x8BFBEA76C619EF36ULL, 0xAEFAE51477A06B03ULL, 0xDAB99E59958885C4ULL,
    0x88B402F7FD75539BULL, 0xAAE103B5FCD2A881ULL, 0xD59944A37C0752A2ULL,
    0x857FCAE62D8493A5ULL, 0xA6DFBD9FB8E5B88EULL, 0xD097AD07A71F26B2ULL,
    0x825ECC24C873782FULL, 0xA2F67F2DFA90563BULL, 0xCBB41EF979346BCAULL,
    0xFEA126B7D78186BCULL, 0x9F24B832E6B0F436ULL, 0xC6EDE63FA05D3143ULL,
    0xF8A95FCF88747D94ULL, 0x9B69DBE1B548CE7CULL, 0xC24452DA229B021BULL,
    0xF2D56790AB41C2A2ULL, 0x97C560BA6B0919A5ULL, 0xBDB6B8E905CB600FULL,
    0xED246723473E3813ULL, 0x9436C0760C86E30BULL, 0xB94470938FA89BCEULL,
    0xE7958CB87392C2C2ULL, 0x90BD77F3483BB9B9ULL, 0xB4ECD5F01A4AA828ULL,
    0xE2280B6C20DD5232ULL, 0x8D590723948A535FULL, 0xB0AF48EC79ACE837ULL,
    0xDCDB1B2798182244ULL, 0x8A08F0F8BF0F156BULL, 0xAC8B2D36EED2DAC5ULL,
    0xD7ADF884AA879177ULL, 0x86CCBB52EA94BAEAULL, 0xA87FEA27A539E9A5ULL,
    0xD29FE4B18E88640EULL, 0x83A3EEEEF9153E89ULL, 0xA48CEAAAB75A8E2BULL,
    0xCDB02555653131B6ULL, 0x808E17555F3EBF11ULL, 0xA0B19D2AB70E6ED6ULL,
    0xC8DE047564D20A8BULL, 0xFB158592BE068D2EULL, 0x9CED737BB6C4183DULL,
    0xC428D05AA4751E4CULL, 0xF53304714D9265DFULL, 0x993FE2C6D07B7FABULL,
    0xBF8FDB78849A5F96ULL, 0xEF73D256A5C0F77CULL, 0x95A8637627989AADULL,
    0xBB127C53B17EC159ULL, 0xE9D71B689DDE71AFULL, 0x9226712162AB070DULL,
    0xB6B00D69BB55C8D1ULL, 0xE45C10C42A2B3B05ULL, 0x8EB98A7A9A5B04E3ULL,
    0xB267ED1940F1C61CULL, 0xDF01E85F912E37A3ULL, 0x8B61313BBABCE2C6ULL,
    0xAE397D8AA96C1B77ULL, 0xD9C7DCED53C72255ULL, 0x881CEA14545C7575ULL,
    0xAA242499697392D2ULL, 0xD4AD2DBFC3D07787ULL, 0x84EC3C97DA624AB4ULL,
    0xA6274BBDD0FADD61ULL, 0xCFB11EAD453994BAULL, 0x81CEB32C4B43FCF4ULL,
    0xA2425FF75E14FC31ULL, 0xCAD2F7F5359A3B3EULL, 0xFD87B5F28300CA0DULL,
    0x9E74D1B791E07E48ULL, 0xC612062576589DDAULL, 0xF79687AED3EEC551ULL,
    0x9ABE14CD44753B52ULL, 0xC16D9A0095928A27ULL, 0xF1C90080BAF72CB1ULL,
    0x971DA05074DA7BEEULL, 0xBCE5086492111AEAULL, 0xEC1E4A7DB69561A5ULL,
    0x9392EE8E921D5D07ULL, 0xB877AA3236A4B449ULL, 0xE69594BEC44DE15BULL,
    0x901D7CF73AB0ACD9ULL, 0xB424DC35095CD80FULL, 0xE12E13424BB40E13ULL,
    0x8CBCCC096F5088CBULL, 0xAFEBFF0BCB24AAFEULL, 0xDBE6FECEBDEDD5BEULL,
    0x89705F4136B4A597ULL, 0xABCC77118461CEFCULL, 0xD6BF94D5E57A42BCULL,
    0x8637BD05AF6C69B5ULL, 0xA7C5AC471B478423ULL, 0xD1B71758E219652BULL,
    0x83126E978D4FDF3BULL, 0xA3D70A3D70A3D70AULL, 0xCCCCCCCCCCCCCCCCULL,
    0x8000000000000000ULL, 0xA000000000000000ULL, 0xC800000000000000ULL,
    0xFA00000000000000ULL, 0x9C40000000000000ULL, 0xC350000000000000ULL,
    0xF424000000000000ULL, 0x9896800000000000ULL, 0xBEBC200000000000ULL,
    0xEE6B280000000000ULL, 0x9502F90000000000ULL, 0xBA43B74000000000ULL,
    0xE8D4A51000000000ULL, 0x9184E72A00000000ULL, 0xB5E620F480000000ULL,
    0xE35FA931A0000000ULL, 0x8E1BC9BF04000000ULL, 0xB1A2BC2EC5000000ULL,
    0xDE0B6B3A76400000ULL, 0x8AC7230489E80000ULL, 0xAD78EBC5AC620000ULL,
    0xD8D726B7177A8000ULL, 0x878678326EAC9000ULL, 0xA968163F0A57B400ULL,
    0xD3C21BCECCEDA100ULL, 0x84595161401484A0ULL, 0xA56FA5B99019A5C8ULL,
    0xCECB8F27F4200F3AULL, 0x813F3978F8940984ULL, 0xA18F07D736B90BE5ULL,
    0xC9F2C9CD04674EDEULL, 0xFC6F7C4045812296ULL, 0x9DC5ADA82B70B59DULL,
    0xC5371912364CE305ULL, 0xF684DF56C3E01BC6ULL, 0x9A130B963A6C115CULL,
    0xC097CE7BC90715B3ULL, 0xF0BDC21ABB48DB20ULL, 0x96769950B50D88F4ULL,
    0xBC143FA4E250EB31ULL, 0xEB194F8E1AE525FDULL, 0x92EFD1B8D0CF37BEULL,
    0xB7ABC627050305ADULL, 0xE596B7B0C643C719ULL, 0x8F7E32CE7BEA5C6FULL,
    0xB35DBF821AE4F38BULL, 0xE0352F62A19E306EULL, 0x8C213D9DA502DE45ULL,
    0xAF298D050E4395D6ULL, 0xDAF3F04651D47B4CULL, 0x88D8762BF324CD0FULL,
    0xAB0E93B6EFEE0053ULL, 0xD5D238A4ABE98068ULL, 0x85A36366EB71F041ULL,
    0xA70C3C40A64E6C51ULL, 0xD0CF4B50CFE20765ULL, 0x82818F1281ED449FULL,
    0xA321F2D7226895C7ULL, 0xCBEA6F8CEB02BB39ULL, 0xFEE50B7025C36A08ULL,
    0x9F4F2726179A2245ULL, 0xC722F0EF9D80AAD6ULL, 0xF8EBAD2B84E0D58BULL,
    0x9B934C3B330C8577ULL, 0xC2781F49FFCFA6D5ULL, 0xF316271C7FC3908AULL,
    0x97EDD871CFDA3A56ULL, 0xBDE94E8E43D0C8ECULL, 0xED63A231D4C4FB27ULL,
    0x945E455F24FB1CF8ULL, 0xB975D6B6EE39E436ULL, 0xE7D34C64A9C85D44ULL,
    0x90E40FBEEA1D3A4AULL, 0xB51D13AEA4A488DDULL, 0xE264589A4DCDAB14ULL,
    0x8D7EB76070A08AECULL, 0xB0DE65388CC8ADA8ULL, 0xDD15FE86AFFAD912ULL,
    0x8A2DBF142DFCC7ABULL, 0xACB92ED9397BF996ULL, 0xD7E77A8F87DAF7FBULL,
    0x86F0AC99B4E8DAFDULL, 0xA8ACD7C0222311BCULL, 0xD2D80DB02AABD62BULL,
    0x83C7088E1AAB65DBULL, 0xA4B8CAB1A1563F52ULL, 0xCDE6FD5E09ABCF26ULL,
    0x80B05E5AC60B6178ULL, 0xA0DC75F1778E39D6ULL, 0xC913936DD571C84CULL,
    0xFB5878494ACE3A5FULL, 0x9D174B2DCEC0E47BULL, 0xC45D1DF942711D9AULL,
    0xF5746577930D6500ULL, 0x9968BF6ABBE85F20ULL, 0xBFC2EF456AE276E8ULL,
    0xEFB3AB16C59B14A2ULL, 0x95D04AEE3B80ECE5ULL, 0xBB445DA9CA61281FULL,
    0xEA1575143CF97226ULL, 0x924D692CA61BE758ULL, 0xB6E0C377CFA2E12EULL,
    0xE498F455C38B997AULL, 0x8EDF98B59A373FECULL, 0xB2977EE300C50FE7ULL,
    0xDF3D5E9BC0F653E1ULL, 0x8B865B215899F46CULL, 0xAE67F1E9AEC07187ULL,
    0xDA01EE641A708DE9ULL, 0x884134FE908658B2ULL, 0xAA51823E34A7EEDEULL,
    0xD4E5E2CDC1D1EA96ULL, 0x850FADC09923329EULL, 0xA6539930BF6BFF45ULL,
    0xCFE87F7CEF46FF16ULL, 0x81F14FAE158C5F6EULL, 0xA26DA3999AEF7749ULL,
    0xCB090C8001AB551CULL, 0xFDCB4FA002162A63ULL, 0x9E9F11C4014DDA7EULL,
    0xC646D63501A1511DULL, 0xF7D88BC24209A565ULL, 0x9AE757596946075FULL,
    0xC1A12D2FC3978937ULL, 0xF209787BB47D6B84ULL, 0x9745EB4D50CE6332ULL,
    0xBD176620A501FBFFULL, 0xEC5D3FA8CE427AFFULL, 0x93BA47C980E98CDFULL,
    0xB8A8D9BBE123F017ULL, 0xE6D3102AD96CEC1DULL, 0x9043EA1AC7E41392ULL,
    0xB454E4A179DD1877ULL, 0xE16A1DC9D8545E94ULL, 0x8CE2529E2734BB1DULL,
    0xB01AE745B101E9E4ULL, 0xDC21A1171D42645DULL, 0x899504AE72497EBAULL,
    0xABFA45DA0EDBDE69ULL, 0xD6F8D7509292D603ULL, 0x865B86925B9BC5C2ULL,
    0xA7F26836F282B732ULL, 0xD1EF0244AF2364FFULL, 0x8335616AED761F1FULL,
    0xA402B9C5A8D3A6E7ULL, 0xCD036837130890A1ULL, 0x802221226BE55A64ULL,
    0xA02AA96B06DEB0FDULL, 0xC83553C5C8965D3DULL, 0xFA42A8B73ABBF48CULL,
    0x9C69A97284B578D7ULL, 0xC38413CF25E2D70DULL, 0xF46518C2EF5B8CD1ULL,
    0x98BF2F79D5993802ULL, 0xBEEEFB584AFF8603ULL, 0xEEAABA2E5DBF6784ULL,
    0x952AB45CFA97A0B2ULL, 0xBA756174393D88DFULL, 0xE912B9D1478CEB17ULL,
    0x91ABB422CCB812EEULL, 0xB616A12B7FE617AAULL, 0xE39C49765FDF9D94ULL,
    0x8E41ADE9FBEBC27DULL, 0xB1D219647AE6B31CULL, 0xDE469FBD99A05FE3ULL,
    0x8AEC23D680043BEEULL, 0xADA72CCC20054AE9ULL, 0xD910F7FF28069DA4ULL,
    0x87AA9AFF79042286ULL, 0xA99541BF57452B28ULL, 0xD3FA922F2D1675F2ULL,
    0x847C9B5D7C2E09B7ULL, 0xA59BC234DB398C25ULL, 0xCF02B2C21207EF2EULL,
    0x8161AFB94B44F57DULL, 0xA1BA1BA79E1632DCULL, 0xCA28A291859BBF93ULL,
    0xFCB2CB35E702AF78ULL, 0x9DEFBF01B061ADABULL, 0xC56BAEC21C7A1916ULL,
    0xF6C69A72A3989F5BULL, 0x9A3C2087A63F6399ULL, 0xC0CB28A98FCF3C7FULL,
    0xF0FDF2D3F3C30B9FULL, 0x969EB7C47859E743ULL, 0xBC4665B596706114ULL,
    0xEB57FF22FC0C7959ULL, 0x9316FF75DD87CBD8ULL, 0xB7DCBF5354E9BECEULL,
    0xE5D3EF282A242E81ULL, 0x8FA475791A569D10ULL, 0xB38D92D760EC4455ULL,
    0xE070F78D3927556AULL, 0x8C469AB843B89562ULL, 0xAF58416654A6BABBULL,
    0xDB2E51BFE9D0696AULL, 0x88FCF317F22241E2ULL, 0xAB3C2FDDEEAAD25AULL,
    0xD60B3BD56A5586F1ULL, 0x85C7056562757456ULL, 0xA738C6BEBB12D16CULL,
    0xD106F86E69D785C7ULL, 0x82A45B450226B39CULL, 0xA34D721642B06084ULL,
    0xCC20CE9BD35C78A5ULL, 0xFF290242C83396CEULL, 0x9F79A169BD203E41ULL,
    0xC75809C42C684DD1ULL, 0xF92E0C3537826145ULL, 0x9BBCC7A142B17CCBULL,
    0xC2ABF989935DDBFEULL, 0xF356F7EBF83552FEULL, 0x98165AF37B2153DEULL,
    0xBE1BF1B059E9A8D6ULL, 0xEDA2EE1C7064130CULL, 0x9485D4D1C63E8BE7ULL,
    0xB9A74A0637CE2EE1ULL, 0xE8111C87C5C1BA99ULL, 0x910AB1D4DB9914A0ULL,
    0xB54D5E4A127F59C8ULL, 0xE2A0B5DC971F303AULL, 0x8DA471A9DE737E24ULL,
    0xB10D8E1456105DADULL, 0xDD50F1996B947518ULL, 0x8A5296FFE33CC92FULL,
    0xACE73CBFDC0BFB7BULL, 0xD8210BEFD30EFA5AULL, 0x8714A775E3E95C78ULL,
    0xA8D9D1535CE3B396ULL, 0xD31045A8341CA07CULL, 0x83EA2B892091E44DULL,
    0xA4E4B66B68B65D60ULL, 0xCE1DE40642E3F4B9ULL, 0x80D2AE83E9CE78F3ULL,
    0xA1075A24E4421730ULL, 0xC94930AE1D529CFCULL, 0xFB9B7CD9A4A7443CULL,
    0x9D412E0806E88AA5ULL, 0xC491798A08A2AD4EULL, 0xF5B5D7EC8ACB58A2ULL,
    0x9991A6F3D6BF1765ULL, 0xBFF610B0CC6EDD3FULL, 0xEFF394DCFF8A948EULL,
    0x95F83D0A1FB69CD9ULL, 0xBB764C4CA7A4440FULL, 0xEA53DF5FD18D5513ULL,
    0x92746B9BE2F8552CULL, 0xB7118682DBB66A77ULL, 0xE4D5E82392A40515ULL,
    0x8F05B1163BA6832DULL, 0xB2C71D5BCA9023F8ULL, 0xDF78E4B2BD342CF6ULL,
    0x8BAB8EEFB6409C1AULL, 0xAE9672ABA3D0C320ULL, 0xDA3C0F568CC4F3E8ULL,
    0x8865899617FB1871ULL, 0xAA7EEBFB9DF9DE8DULL, 0xD51EA6FA85785631ULL,
    0x8533285C936B35DEULL, 0xA67FF273B8460356ULL, 0xD01FEF10A657842CULL,
    0x8213F56A67F6B29BULL, 0xA298F2C501F45F42ULL, 0xCB3F2F7642717713ULL,
    0xFE0EFB53D30DD4D7ULL, 0x9EC95D1463E8A506ULL, 0xC67BB4597CE2CE48ULL,
    0xF81AA16FDC1B81DAULL, 0x9B10A4E5E9913128ULL, 0xC1D4CE1F63F57D72ULL,
    0xF24A01A73CF2DCCFULL, 0x976E41088617CA01ULL, 0xBD49D14AA79DBC82ULL,
    0xEC9C459D51852BA2ULL, 0x93E1AB8252F33B45ULL, 0xB8DA1662E7B00A17ULL,
    0xE7109BFBA19C0C9DULL, 0x906A617D450187E2ULL, 0xB484F9DC9641E9DAULL,
    0xE1A63853BBD26451ULL, 0x8D07E33455637EB2ULL, 0xB049DC016ABC5E5FULL,
    0xDC5C5301C56B75F7ULL, 0x89B9B3E11B6329BAULL, 0xAC2820D9623BF429ULL,
    0xD732290FBACAF133ULL, 0x867F59A9D4BED6C0ULL, 0xA81F301449EE8C70ULL,
    0xD226FC195C6A2F8CULL, 0x83585D8FD9C25DB7ULL, 0xA42E74F3D032F525ULL,
    0xCD3A1230C43FB26FULL, 0x80444B5E7AA7CF85ULL, 0xA0555E361951C366ULL,
    0xC86AB5C39FA63440ULL, 0xFA856334878FC150ULL, 0x9C935E00D4B9D8D2ULL,
    0xC3B8358109E84F07ULL, 0xF4A642E14C6262C8ULL, 0x98E7E9CCCFBD7DBDULL,
    0xBF21E44003ACDD2CULL, 0xEEEA5D5004981478ULL, 0x95527A5202DF0CCBULL,
    0xBAA718E68396CFFDULL, 0xE950DF20247C83FDULL, 0x91D28B7416CDD27EULL,
    0xB6472E511C81471DULL, 0xE3D8F9E563A198E5ULL, 0x8E679C2F5E44FF8FULL,
    0xB201833B35D63F73ULL, 0xDE81E40A034BCF4FULL, 0x8B112E86420F6191ULL,
    0xADD57A27D29339F6ULL, 0xD94AD8B1C7380874ULL, 0x87CEC76F1C830548ULL,
    0xA9C2794AE3A3C69AULL, 0xD433179D9C8CB841ULL, 0x849FEEC281D7F328ULL,
    0xA5C7EA73224DEFF3ULL, 0xCF39E50FEAE16BEFULL, 0x81842F29F2CCE375ULL,
    0xA1E53AF46F801C53ULL, 0xCA5E89B18B602368ULL, 0xFCF62C1DEE382C42ULL,
    0x9E19DB92B4E31BA9ULL, 0xC5A05277621BE293ULL, 0xF70867153AA2DB38ULL,
    0x9A65406D44A5C903ULL, 0xC0FE908895CF3B44ULL, 0xF13E34AABB430A15ULL,
    0x96C6E0EAB509E64DULL, 0xBC789925624C5FE0ULL, 0xEB96BF6EBADF77D8ULL,
    0x933E37A534CBAAE7ULL, 0xB80DC58E81FE95A1ULL, 0xE61136F2227E3B09ULL,
    0x8FCAC257558EE4E6ULL, 0xB3BD72ED2AF29E1FULL, 0xE0ACCFA875AF45A7ULL,
    0x8C6C01C9498D8B88ULL, 0xAF87023B9BF0EE6AULL, 0xDB68C2CA82ED2A05ULL,
    0x892179BE91D43A43ULL,
};

static const uint64_t grisu_pow_lo[GRISU_ALPHA_COUNT] = {
    0xE50FF107BAB528A1ULL, 0x1E53ED49A96272C9ULL, 0x25E8E89C13BB0F7BULL,
    0x77B191618C54E9ADULL, 0xD59DF5B9EF6A2418ULL, 0x4B0573286B44AD1EULL,
    0x4EE367F9430AEC33ULL, 0x229C41F793CDA73FULL, 0x6B43527578C1110FULL,
    0x830A13896B78AAAAULL, 0x23CC986BC656D554ULL, 0x2CBFBE86B7EC8AA9ULL,
    0x7BF7D71432F3D6AAULL, 0xDAF5CCD93FB0CC54ULL, 0xD1B3400F8F9CFF69ULL,
    0x23100809B9C21FA2ULL, 0xABD40A0C2832A78AULL, 0x16C90C8F323F516DULL,
    0xAE3DA7D97F6792E4ULL, 0x99CD11CFDF41779DULL, 0x40405643D711D584ULL,
    0x482835EA666B2572ULL, 0xDA3243650005EECFULL, 0x90BED43E40076A83ULL,
    0x5A7744A6E804A292ULL, 0x711515D0A205CB36ULL, 0x0D5A5B44CA873E04ULL,
    0xE858790AFE9486C2ULL, 0x626E974DBE39A873ULL, 0xFB0A3D212DC81290ULL,
    0x7CE66634BC9D0B9AULL, 0x1C1FFFC1EBC44E80ULL, 0xA327FFB266B56220ULL,
    0x4BF1FF9F0062BAA8ULL, 0x6F773FC3603DB4A9ULL, 0xCB550FB4384D21D4ULL,
    0x7E2A53A146606A48ULL, 0x2EDA7444CBFC426DULL, 0xFA911155FEFB5309ULL,
    0x793555AB7EBA27CBULL, 0x4BC1558B2F3458DFULL, 0x9EB1AAEDFB016F16ULL,
    0x465E15A979C1CADCULL, 0x0BFACD89EC191ECAULL, 0xCEF980EC671F667CULL,
    0x82B7E12780E7401BULL, 0xD1B2ECB8B0908811ULL, 0x861FA7E6DCB4AA15ULL,
    0x67A791E093E1D49AULL, 0xE0C8BB2C5C6D24E0ULL, 0x58FAE9F773886E19ULL,
    0xAF39A475506A899FULL, 0x6D8406C952429603ULL, 0xC8E5087BA6D33B84ULL,
    0xFB1E4A9A90880A65ULL, 0x5CF2EEA09A55067FULL, 0xF42FAA48C0EA481FULL,
    0xF13B94DAF124DA27ULL, 0x76C53D08D6B70858ULL, 0x54768C4B0C64CA6EULL,
    0xA9942F5DCF7DFD0AULL, 0xD3F93B35435D7C4CULL, 0xC47BC5014A1A6DB0ULL,
    0x359AB6419CA1091BULL, 0xC30163D203C94B62ULL, 0x79E0DE63425DCF1DULL,
    0x985915FC12F542E5ULL, 0x3E6F5B7B17B2939EULL, 0xA705992CEECF9C43ULL,
    0x50C6FF782A838353ULL, 0xA4F8BF5635246428ULL, 0x871B7795E136BE99ULL,
    0x28E2557B59846E3FULL, 0x331AEADA2FE589CFULL, 0x3FF0D2C85DEF7622ULL,
    0x0FED077A756B53AAULL, 0xD3E8495912C62894ULL, 0x64712DD7ABBBD95DULL,
    0xBD8D794D96AACFB4ULL, 0xECF0D7A0FC5583A1ULL, 0xF41686C49DB57245ULL,
    0x311C2875C522CED6ULL, 0x7D633293366B828BULL, 0xAE5DFF9C02033197ULL,
    0xD9F57F830283FDFDULL, 0xD072DF63C324FD7CULL, 0x4247CB9E59F71E6DULL,
    0x52D9BE85F074E609ULL, 0x67902E276C921F8BULL, 0x00BA1CD8A3DB53B7ULL,
    0x80E8A40ECCD228A5ULL, 0x6122CD128006B2CEULL, 0x796B805720085F81ULL,
    0xCBE3303674053BB1ULL, 0xBEDBFC4411068A9DULL, 0xEE92FB5515482D44ULL,
    0x751BDD152D4D1C4BULL, 0xD262D45A78A0635DULL, 0x86FB897116C87C35ULL,
    0xD45D35E6AE3D4DA1ULL, 0x8974836059CCA109ULL, 0x2BD1A438703FC94BULL,
    0x7B6306A34627DDCFULL, 0x1A3BC84C17B1D543ULL, 0x20CABA5F1D9E4A94ULL,
    0x547EB47B7282EE9CULL, 0xE99E619A4F23AA43ULL, 0x6405FA00E2EC94D4ULL,
    0xDE83BC408DD3DD05ULL, 0x9624AB50B148D446ULL, 0x3BADD624DD9B0957ULL,
    0xE54CA5D70A80E5D6ULL, 0x5E9FCF4CCD211F4CULL, 0x7647C3200069671FULL,
    0x29ECD9F40041E073ULL, 0xF468107100525890ULL, 0x7182148D4066EEB4ULL,
    0xC6F14CD848405531ULL, 0xB8ADA00E5A506A7DULL, 0xA6D90811F0E4851CULL,
    0x908F4A166D1DA663ULL, 0x9A598E4E043287FEULL, 0x40EFF1E1853F29FEULL,
    0xD12BEE59E68EF47DULL, 0x82BB74F8301958CEULL, 0xE36A52363C1FAF02ULL,
    0xDC44E6C3CB279AC2ULL, 0x29AB103A5EF8C0B9ULL, 0x7415D448F6B6F0E8ULL,
    0x111B495B3464AD21ULL, 0xCAB10DD900BEEC35ULL, 0x3D5D514F40EEA742ULL,
    0x0CB4A5A3112A5113ULL, 0x47F0E785EABA72ACULL, 0x59ED216765690F57ULL,
    0x306869C13EC3532CULL, 0x1E414218C73A13FCULL, 0xE5D1929EF90898FBULL,
    0xDF45F746B74ABF39ULL, 0x6B8BBA8C328EB784ULL, 0x066EA92F3F326565ULL,
    0xC80A537B0EFEFEBEULL, 0xBD06742CE95F5F37ULL, 0x2C48113823B73704ULL,
    0xF75A15862CA504C5ULL, 0x9A984D73DBE722FBULL, 0xC13E60D0D2E0EBBAULL,
    0x318DF905079926A9ULL, 0xFDF17746497F7053ULL, 0xFEB6EA8BEDEFA634ULL,
    0xFE64A52EE96B8FC1ULL, 0x3DFDCE7AA3C673B1ULL, 0x06BEA10CA65C084FULL,
    0x486E494FCFF30A62ULL, 0x5A89DBA3C3EFCCFBULL, 0xF89629465A75E01DULL,
    0xF6BBB397F1135824ULL, 0x746AA07DED582E2DULL, 0xA8C2A44EB4571CDCULL,
    0x92F34D62616CE413ULL, 0x77B020BAF9C81D18ULL, 0x0ACE1474DC1D122FULL,
    0x0D819992132456BBULL, 0x10E1FFF697ED6C69ULL, 0xCA8D3FFA1EF463C2ULL,
    0xBD308FF8A6B17CB2ULL, 0xAC7CB3F6D05DDBDFULL, 0x6BCDF07A423AA96BULL,
    0x86C16C98D2C953C6ULL, 0xE871C7BF077BA8B8ULL, 0x11471CD764AD4973ULL,
    0xD598E40D3DD89BCFULL, 0x4AFF1D108D4EC2C3ULL, 0xCEDF722A585139BAULL,
    0xC2974EB4EE658829ULL, 0x733D226229FEEA33ULL, 0x0806357D5A3F5260ULL,
    0xCA07C2DCB0CF26F8ULL, 0xFC89B393DD02F0B6ULL, 0xBBAC2078D443ACE3ULL,
    0xD54B944B84AA4C0EULL, 0x0A9E795E65D4DF11ULL, 0x4D4617B5FF4A16D6ULL,
    0x504BCED1BF8E4E46ULL, 0xE45EC2862F71E1D7ULL, 0x5D767327BB4E5A4DULL,
    0x3A6A07F8D510F870ULL, 0x890489F70A55368CULL, 0x2B45AC74CCEA842FULL,
    0x3B0B8BC90012929DULL, 0x09CE6EBB40173745ULL, 0xCC420A6A101D0516ULL,
    0x9FA946824A12232EULL, 0x47939822DC96ABF9ULL, 0x59787E2B93BC56F7ULL,
    0x57EB4EDB3C55B65BULL, 0xEDE622920B6B23F1ULL, 0xE95FAB368E45ECEDULL,
    0x11DBCB0218EBB414ULL, 0xD652BDC29F26A11AULL, 0x4BE76D3346F04960ULL,
    0x6F70A4400C562DDCULL, 0xCB4CCD500F6BB953ULL, 0x7E2000A41346A7A8ULL,
    0x8ED400668C0C28C9ULL, 0x728900802F0F32FBULL, 0x4F2B40A03AD2FFBAULL,
    0xE2F610C84987BFA8ULL, 0x0DD9CA7D2DF4D7C9ULL, 0x91503D1C79720DBBULL,
    0x75A44C6397CE912AULL, 0xC986AFBE3EE11ABAULL, 0xFBE85BADCE996169ULL,
    0xFAE27299423FB9C3ULL, 0xDCCD879FC967D41AULL, 0x5400E987BBC1C921ULL,
    0x290123E9AAB23B69ULL, 0xF9A0B6720AAF6521ULL, 0xF808E40E8D5B3E6AULL,
    0xB60B1D1230B20E04ULL, 0xB1C6F22B5E6F48C3ULL, 0x1E38AEB6360B1AF3ULL,
    0x25C6DA63C38DE1B0ULL, 0x579C487E5A38AD0EULL, 0x2D835A9DF0C6D852ULL,
    0xF8E431456CF88E66ULL, 0x1B8E9ECB641B5900ULL, 0xE272467E3D222F40ULL,
    0x5B0ED81DCC6ABB10ULL, 0x98E947129FC2B4EAULL, 0x3F2398D747B36224ULL,
    0x8EEC7F0D19A03AADULL, 0x1953CF68300424ACULL, 0x5FA8C3423C052DD7ULL,
    0x3792F412CB06794DULL, 0xE2BBD88BBEE40BD0ULL, 0x5B6ACEAEAE9D0EC4ULL,
    0xF245825A5A445275ULL, 0xEED6E2F0F0D56713ULL, 0x55464DD69685606CULL,
    0xAA97E14C3C26B887ULL, 0xD53DD99F4B3066A8ULL, 0xE546A8038EFE4029ULL,
    0xDE98520472BDD033ULL, 0x963E66858F6D4440ULL, 0xDDE7001379A44AA8ULL,
    0x5560C018580D5D52ULL, 0xAAB8F01E6E10B4A7ULL, 0xCAB3961304CA70E8ULL,
    0x3D607B97C5FD0D22ULL, 0x8CB89A7DB77C506BULL, 0x77F3608E92ADB243ULL,
    0x55F038B237591ED3ULL, 0x6B6C46DEC52F6688ULL, 0x2323AC4B3B3DA015ULL,
    0xABEC975E0A0D081BULL, 0x96E7BD358C904A21ULL, 0x7E50D64177DA2E55ULL,
    0xDDE50BD1D5D0B9EAULL, 0x955E4EC64B44E864ULL, 0xBD5AF13BEF0B113FULL,
    0xECB1AD8AEACDD58EULL, 0x67DE18EDA5814AF2ULL, 0x80EACF948770CED7ULL,
    0xA1258379A94D028DULL, 0x096EE45813A04330ULL, 0x8BCA9D6E188853FCULL,
    0x775EA264CF55347EULL, 0x95364AFE032A819DULL, 0x3A83DDBD83F52205ULL,
    0xC4926A9672793543ULL, 0x75B7053C0F178294ULL, 0x5324C68B12DD6338ULL,
    0xD3F6FC16EBCA5E03ULL, 0x88F4BB1CA6BCF584ULL, 0x2B31E9E3D06C32E5ULL,
    0x3AFF322E62439FCFULL, 0x09BEFEB9FAD487C3ULL, 0x4C2EBE687989A9B4ULL,
    0x0F9D37014BF60A10ULL, 0x538484C19EF38C94ULL, 0x2865A5F206B06FBAULL,
    0xF93F87B7442E45D4ULL, 0xF78F69A51539D749ULL, 0xB573440E5A884D1BULL,
    0x31680A88F8953031ULL, 0xFDC20D2B36BA7C3DULL, 0x3D32907604691B4DULL,
    0xA63F9A49C2C1B110ULL, 0x0FCF80DC33721D54ULL, 0xD3C36113404EA4A9ULL,
    0x645A1CAC083126E9ULL, 0x3D70A3D70A3D70A4ULL, 0xCCCCCCCCCCCCCCCDULL,
    0x0000000000000000ULL, 0x0000000000000000ULL, 0x0000000000000000ULL,
    0x0000000000000000ULL, 0x0000000000000000ULL, 0x0000000000000000ULL,
    0x0000000000000000ULL, 0x0000000000000000ULL, 0x0000000000000000ULL,
    0x0000000000000000ULL, 0x0000000000000000ULL, 0x0000000000000000ULL,
    0x0000000000000000ULL, 0x0000000000000000ULL, 0x0000000000000000ULL,
    0x0000000000000000ULL, 0x0000000000000000ULL, 0x0000000000000000ULL,
    0x0000000000000000ULL, 0x0000000000000000ULL, 0x0000000000000000ULL,
    0x0000000000000000ULL, 0x0000000000000000ULL, 0x0000000000000000ULL,
    0x0000000000000000ULL, 0x0000000000000000ULL, 0x0000000000000000ULL,
    0x0000000000000000ULL, 0x4000000000000000ULL, 0x5000000000000000ULL,
    0xA400000000000000ULL, 0x4D00000000000000ULL, 0xF020000000000000ULL,
    0x6C28000000000000ULL, 0xC732000000000000ULL, 0x3C7F400000000000ULL,
    0x4B9F100000000000ULL, 0x1E86D40000000000ULL, 0x1314448000000000ULL,
    0x17D955A000000000ULL, 0x5DCFAB0800000000ULL, 0x5AA1CAE500000000ULL,
    0xF14A3D9E40000000ULL, 0x6D9CCD05D0000000ULL, 0xE4820023A2000000ULL,
    0xDDA2802C8A800000ULL, 0xD50B2037AD200000ULL, 0x4526F422CC340000ULL,
    0x9670B12B7F410000ULL, 0x3C0CDD765F114000ULL, 0xA5880A69FB6AC800ULL,
    0x8EEA0D047A457A00ULL, 0x72A4904598D6D880ULL, 0x47A6DA2B7F864750ULL,
    0x999090B65F67D924ULL, 0xFFF4B4E3F741CF6DULL, 0xBFF8F10E7A8921A4ULL,
    0xAFF72D52192B6A0DULL, 0x9BF4F8A69F764490ULL, 0x02F236D04753D5B5ULL,
    0x01D762422C946591ULL, 0x424D3AD2B7B97EF5ULL, 0xD2E0898765A7DEB2ULL,
    0x63CC55F49F88EB2FULL, 0x3CBF6B71C76B25FBULL, 0x8BEF464E3945EF7AULL,
    0x97758BF0E3CBB5ACULL, 0x3D52EEED1CBEA317ULL, 0x4CA7AAA863EE4BDDULL,
    0x8FE8CAA93E74EF6AULL, 0xB3E2FD538E122B45ULL, 0x60DBBCA87196B616ULL,
    0xBC8955E946FE31CEULL, 0x6BABAB6398BDBE41ULL, 0xC696963C7EED2DD2ULL,
    0xFC1E1DE5CF543CA3ULL, 0x3B25A55F43294BCCULL, 0x49EF0EB713F39EBFULL,
    0x6E3569326C784337ULL, 0x49C2C37F07965405ULL, 0xDC33745EC97BE906ULL,
    0x69A028BB3DED71A4ULL, 0xC40832EA0D68CE0DULL, 0xF50A3FA490C30190ULL,
    0x792667C6DA79E0FAULL, 0x577001B891185939ULL, 0xED4C0226B55E6F87ULL,
    0x544F8158315B05B4ULL, 0x696361AE3DB1C721ULL, 0x03BC3A19CD1E38EAULL,
    0x04AB48A04065C724ULL, 0x62EB0D64283F9C76ULL, 0x3BA5D0BD324F8394ULL,
    0xCA8F44EC7EE36479ULL, 0x7E998B13CF4E1ECCULL, 0x9E3FEDD8C321A67FULL,
    0xC5CFE94EF3EA101EULL, 0xBBA1F1D158724A13ULL, 0x2A8A6E45AE8EDC98ULL,
    0xF52D09D71A3293BEULL, 0x593C2626705F9C56ULL, 0x6F8B2FB00C77836CULL,
    0x0B6DFB9C0F956447ULL, 0x4724BD4189BD5EACULL, 0x58EDEC91EC2CB658ULL,
    0x2F2967B66737E3EDULL, 0xBD79E0D20082EE74ULL, 0xECD8590680A3AA11ULL,
    0xE80E6F4820CC9496ULL, 0x3109058D147FDCDEULL, 0xBD4B46F0599FD415ULL,
    0x6C9E18AC7007C91AULL, 0x03E2CF6BC604DDB0ULL, 0x84DB8346B786151DULL,
    0xE612641865679A64ULL, 0x4FCB7E8F3F60C07EULL, 0xE3BE5E330F38F09EULL,
    0x5CADF5BFD3072CC5ULL, 0x73D9732FC7C8F7F7ULL, 0x2867E7FDDCDD9AFAULL,
    0xB281E1FD541501B9ULL, 0x1F225A7CA91A4227ULL, 0x3375788DE9B06958ULL,
    0x0052D6B1641C83AEULL, 0xC0678C5DBD23A49AULL, 0xF840B7BA963646E0ULL,
    0xB650E5A93BC3D898ULL, 0xA3E51F138AB4CEBEULL, 0xC66F336C36B10137ULL,
    0xB80B0047445D4185ULL, 0xA60DC059157491E6ULL, 0x87C89837AD68DB30ULL,
    0x29BABE4598C311FCULL, 0xF4296DD6FEF3D67BULL, 0x1899E4A65F58660DULL,
    0x5EC05DCFF72E7F90ULL, 0x76707543F4FA1F74ULL, 0x6A06494A791C53A8ULL,
    0x0487DB9D17636892ULL, 0x45A9D2845D3C42B7ULL, 0x0B8A2392BA45A9B2ULL,
    0x8E6CAC7768D7141FULL, 0x3207D795430CD927ULL, 0x7F44E6BD49E807B8ULL,
    0x5F16206C9C6209A6ULL, 0x36DBA887C37A8C10ULL, 0xC2494954DA2C978AULL,
    0xF2DB9BAA10B7BD6CULL, 0x6F92829494E5ACC7ULL, 0xCB772339BA1F17F9ULL,
    0xFF2A760414536EFCULL, 0xFEF5138519684ABBULL, 0x7EB258665FC25D69ULL,
    0xEF2F773FFBD97A62ULL, 0xAAFB550FFACFD8FAULL, 0x95BA2A53F983CF39ULL,
    0xDD945A747BF26184ULL, 0x94F971119AEEF9E4ULL, 0x7A37CD5601AAB85EULL,
    0xAC62E055C10AB33BULL, 0x577B986B314D6009ULL, 0xED5A7E85FDA0B80BULL,
    0x14588F13BE847307ULL, 0x596EB2D8AE258FC9ULL, 0x6FCA5F8ED9AEF3BBULL,
    0x25DE7BB9480D5855ULL, 0xAF561AA79A10AE6AULL, 0x1B2BA1518094DA05ULL,
    0x90FB44D2F05D0843ULL, 0x353A1607AC744A54ULL, 0x42889B8997915CE9ULL,
    0x69956135FEBADA11ULL, 0x43FAB9837E699096ULL, 0x94F967E45E03F4BBULL,
    0x1D1BE0EEBAC278F5ULL, 0x6462D92A69731732ULL, 0x7D7B8F7503CFDCFFULL,
    0x5CDA735244C3D43FULL, 0x3A0888136AFA64A7ULL, 0x088AAA1845B8FDD1ULL,
    0x8AAD549E57273D45ULL, 0x36AC54E2F678864BULL, 0x84576A1BB416A7DEULL,
    0x656D44A2A11C51D5ULL, 0x9F644AE5A4B1B325ULL, 0x873D5D9F0DDE1FEFULL,
    0xA90CB506D155A7EAULL, 0x09A7F12442D588F3ULL, 0x0C11ED6D538AEB2FULL,
    0x8F1668C8A86DA5FBULL, 0xF96E017D694487BDULL, 0x37C981DCC395A9ACULL,
    0x85BBE253F47B1417ULL, 0x93956D7478CCEC8EULL, 0x387AC8D1970027B2ULL,
    0x06997B05FCC0319FULL, 0x441FECE3BDF81F03ULL, 0xD527E81CAD7626C4ULL,
    0x8A71E223D8D3B075ULL, 0xF6872D5667844E49ULL, 0xB428F8AC016561DBULL,
    0xE13336D701BEBA52ULL, 0xECC0024661173473ULL, 0x27F002D7F95D0190ULL,
    0x31EC038DF7B441F4ULL, 0x7E67047175A15271ULL, 0x0F0062C6E984D387ULL,
    0x52C07B78A3E60868ULL, 0xA7709A56CCDF8A83ULL, 0x88A66076400BB692ULL,
    0x6ACFF893D00EA436ULL, 0x0583F6B8C4124D43ULL, 0xC3727A337A8B704AULL,
    0x744F18C0592E4C5DULL, 0x1162DEF06F79DF74ULL, 0x8ADDCB5645AC2BA8ULL,
    0x6D953E2BD7173693ULL, 0xC8FA8DB6CCDD0437ULL, 0x1D9C9892400A22A2ULL,
    0x2503BEB6D00CAB4BULL, 0x2E44AE64840FD61EULL, 0x5CEAECFED289E5D3ULL,
    0x7425A83E872C5F47ULL, 0xD12F124E28F77719ULL, 0x82BD6B70D99AAA70ULL,
    0x636CC64D1001550CULL, 0x3C47F7E05401AA4FULL, 0x65ACFAEC34810A71ULL,
    0x7F1839A741A14D0DULL, 0x1EDE48111209A051ULL, 0x934AED0AAB460432ULL,
    0xF81DA84D5617853FULL, 0x36251260AB9D668FULL, 0xC1D72B7C6B426019ULL,
    0xB24CF65B8612F820ULL, 0xDEE033F26797B628ULL, 0x169840EF017DA3B1ULL,
    0x8E1F289560EE864FULL, 0xF1A6F2BAB92A27E3ULL, 0xAE10AF696774B1DBULL,
    0xACCA6DA1E0A8EF29ULL, 0x17FD090A58D32AF3ULL, 0xDDFC4B4CEF07F5B0ULL,
    0x4ABDAF101564F98EULL, 0x9D6D1AD41ABE37F2ULL, 0x84C86189216DC5EEULL,
    0x32FD3CF5B4E49BB5ULL, 0x3FBC8C33221DC2A2ULL, 0x0FABAF3FEAA5334AULL,
    0x29CB4D87F2A7400EULL, 0x743E20E9EF511012ULL, 0x914DA9246B255417ULL,
    0x1AD089B6C2F7548EULL, 0xA184AC2473B529B2ULL, 0xC9E5D72D90A2741EULL,
    0x7E2FA67C7A658893ULL, 0xDDBB901B98FEEAB8ULL, 0x552A74227F3EA565ULL,
    0xD53A88958F87275FULL, 0x8A892ABAF368F137ULL, 0x2D2B7569B0432D85ULL,
    0x9C3B29620E29FC73ULL, 0x8349F3BA91B47B90ULL, 0x241C70A936219A74ULL,
    0xED238CD383AA0111ULL, 0xF4363804324A40ABULL, 0xB143C6053EDCD0D5ULL,
    0xDD94B7868E94050AULL, 0xCA7CF2B4191C8327ULL, 0xFD1C2F611F63A3F0ULL,
    0xBC633B39673C8CECULL, 0xD5BE0503E085D814ULL, 0x4B2D8644D8A74E19ULL,
    0xDDF8E7D60ED1219FULL, 0xCABB90E5C942B503ULL, 0x3D6A751F3B936244ULL,
    0x0CC512670A783AD5ULL, 0x27FB2B80668B24C5ULL, 0xB1F9F660802DEDF6ULL,
    0x5E7873F8A0396974ULL, 0xDB0B487B6423E1E8ULL, 0x91CE1A9A3D2CDA63ULL,
    0x7641A140CC7810FBULL, 0xA9E904C87FCB0A9DULL, 0x546345FA9FBDCD44ULL,
    0xA97C177947AD4095ULL, 0x49ED8EABCCCC485DULL, 0x5C68F256BFFF5A75ULL,
    0x73832EEC6FFF3112ULL, 0xC831FD53C5FF7EABULL, 0xBA3E7CA8B77F5E56ULL,
    0x28CE1BD2E55F35EBULL, 0x7980D163CF5B81B3ULL, 0xD7E105BCC3326220ULL,
    0x8DD9472BF3FEFAA8ULL, 0xB14F98F6F0FEB952ULL, 0x6ED1BF9A569F33D3ULL,
    0x0A862F80EC4700C8ULL, 0xCD27BB612758C0FAULL, 0x8038D51CB897789CULL,
    0xE0470A63E6BD56C3ULL, 0x1858CCFCE06CAC74ULL, 0x0F37801E0C43EBC9ULL,
    0xD30560258F54E6BBULL, 0x47C6B82EF32A2069ULL, 0x4CDC331D57FA5442ULL,
    0xE0133FE4ADF8E952ULL, 0x58180FDDD97723A7ULL, 0x570F09EAA7EA7648ULL,
    0x2CD2CC6551E513DAULL, 0xF8077F7EA65E58D1ULL, 0xFB04AFAF27FAF783ULL,
    0x79C5DB9AF1F9B563ULL, 0x18375281AE7822BCULL, 0x8F2293910D0B15B6ULL,
    0xB2EB3875504DDB23ULL, 0x5FA60692A46151ECULL, 0xDBC7C41BA6BCD333ULL,
    0x12B9B522906C0800ULL, 0xD768226B34870A00ULL, 0xE6A1158300D46640ULL,
    0x60495AE3C1097FD0ULL, 0x385BB19CB14BDFC4ULL, 0x46729E03DD9ED7B5ULL,
    0x6C07A2C26A8346D1ULL, 0xC7098B7305241886ULL, 0xB8CBEE4FC66D1EA7ULL,
    0x737F74F1DC043328ULL, 0x505F522E53053FF2ULL, 0x647726B9E7C68FEFULL,
    0x5ECA783430DC19F5ULL, 0xB67D16413D132073ULL, 0xE41C5BD18C57E88FULL,
    0x8E91B962F7B6F15AULL, 0x723627BBB5A4ADB0ULL, 0xCEC3B1AAA30DD91CULL,
    0x213A4F0AA5E8A7B2ULL, 0xA988E2CD4F62D19EULL, 0x93EB1B80A33B8605ULL,
    0xBC72F130660533C3ULL, 0xEB8FAD7C7F8680B4ULL, 0xA67398DB9F6820E1ULL,
    0x88083F8943A1148DULL,
};

static const int32_t grisu_pow_ec[GRISU_ALPHA_COUNT] = {
    -1104, -1101, -1098, -1094, -1091, -1088, -1084, -1081, -1078, -1074,
    -1071, -1068, -1064, -1061, -1058, -1054, -1051, -1048, -1044, -1041,
    -1038, -1034, -1031, -1028, -1024, -1021, -1018, -1014, -1011, -1008,
    -1004, -1001, -998, -995, -991, -988, -985, -981, -978, -975,
    -971, -968, -965, -961, -958, -955, -951, -948, -945, -941,
    -938, -935, -931, -928, -925, -921, -918, -915, -911, -908,
    -905, -902, -898, -895, -892, -888, -885, -882, -878, -875,
    -872, -868, -865, -862, -858, -855, -852, -848, -845, -842,
    -838, -835, -832, -828, -825, -822, -818, -815, -812, -808,
    -805, -802, -799, -795, -792, -789, -785, -782, -779, -775,
    -772, -769, -765, -762, -759, -755, -752, -749, -745, -742,
    -739, -735, -732, -729, -725, -722, -719, -715, -712, -709,
    -706, -702, -699, -696, -692, -689, -686, -682, -679, -676,
    -672, -669, -666, -662, -659, -656, -652, -649, -646, -642,
    -639, -636, -632, -629, -626, -622, -619, -616, -613, -609,
    -606, -603, -599, -596, -593, -589, -586, -583, -579, -576,
    -573, -569, -566, -563, -559, -556, -553, -549, -546, -543,
    -539, -536, -533, -529, -526, -523, -519, -516, -513, -510,
    -506, -503, -500, -496, -493, -490, -486, -483, -480, -476,
    -473, -470, -466, -463, -460, -456, -453, -450, -446, -443,
    -440, -436, -433, -430, -426, -423, -420, -417, -413, -410,
    -407, -403, -400, -397, -393, -390, -387, -383, -380, -377,
    -373, -370, -367, -363, -360, -357, -353, -350, -347, -343,
    -340, -337, -333, -330, -327, -323, -320, -317, -314, -310,
    -307, -304, -300, -297, -294, -290, -287, -284, -280, -277,
    -274, -270, -267, -264, -260, -257, -254, -250, -247, -244,
    -240, -237, -234, -230, -227, -224, -221, -217, -214, -211,
    -207, -204, -201, -197, -194, -191, -187, -184, -181, -177,
    -174, -171, -167, -164, -161, -157, -154, -151, -147, -144,
    -141, -137, -134, -131, -127, -124, -121, -118, -114, -111,
    -108, -104, -101, -98, -94, -91, -88, -84, -81, -78,
    -74, -71, -68, -64, -61, -58, -54, -51, -48, -44,
    -41, -38, -34, -31, -28, -25, -21, -18, -15, -11,
    -8, -5, -1, 2, 5, 9, 12, 15, 19, 22,
    25, 29, 32, 35, 39, 42, 45, 49, 52, 55,
    59, 62, 65, 68, 72, 75, 78, 82, 85, 88,
    92, 95, 98, 102, 105, 108, 112, 115, 118, 122,
    125, 128, 132, 135, 138, 142, 145, 148, 152, 155,
    158, 162, 165, 168, 171, 175, 178, 181, 185, 188,
    191, 195, 198, 201, 205, 208, 211, 215, 218, 221,
    225, 228, 231, 235, 238, 241, 245, 248, 251, 255,
    258, 261, 264, 268, 271, 274, 278, 281, 284, 288,
    291, 294, 298, 301, 304, 308, 311, 314, 318, 321,
    324, 328, 331, 334, 338, 341, 344, 348, 351, 354,
    358, 361, 364, 367, 371, 374, 377, 381, 384, 387,
    391, 394, 397, 401, 404, 407, 411, 414, 417, 421,
    424, 427, 431, 434, 437, 441, 444, 447, 451, 454,
    457, 460, 464, 467, 470, 474, 477, 480, 484, 487,
    490, 494, 497, 500, 504, 507, 510, 514, 517, 520,
    524, 527, 530, 534, 537, 540, 544, 547, 550, 553,
    557, 560, 563, 567, 570, 573, 577, 580, 583, 587,
    590, 593, 597, 600, 603, 607, 610, 613, 617, 620,
    623, 627, 630, 633, 637, 640, 643, 647, 650, 653,
    656, 660, 663, 666, 670, 673, 676, 680, 683, 686,
    690, 693, 696, 700, 703, 706, 710, 713, 716, 720,
    723, 726, 730, 733, 736, 740, 743, 746, 749, 753,
    756, 759, 763, 766, 769, 773, 776, 779, 783, 786,
    789, 793, 796, 799, 803, 806, 809, 813, 816, 819,
    823, 826, 829, 833, 836, 839, 843, 846, 849, 852,
    856, 859, 862, 866, 869, 872, 876, 879, 882, 886,
    889, 892, 896, 899, 902, 906, 909, 912, 916, 919,
    922, 926, 929, 932, 936, 939, 942, 945, 949, 952,
    955, 959, 962, 965, 969, 972, 975, 979, 982, 985,
    989, 992, 995, 999, 1002, 1005, 1009,
};

static const uint64_t grisu_pow10[18] = {
    1ULL, 10ULL, 100ULL, 1000ULL,
    10000ULL, 100000ULL, 1000000ULL, 10000000ULL,
    100000000ULL, 1000000000ULL, 10000000000ULL, 100000000000ULL,
    1000000000000ULL, 10000000000000ULL, 100000000000000ULL,
    1000000000000000ULL, 10000000000000000ULL, 100000000000000000ULL
};

/* floor(f_w * (ch:cl) / 2^64): 64x128 -> high 128 bits. The product
   f_w * (ch:cl) < 2^192 and its top 128 bits fit a u128 exactly, so
   this is exact except for the 1-ulp truncation. */
static inline grisu_u128 grisu_mul_hi128(uint64_t f_w, uint64_t ch, uint64_t cl)
{
    return (grisu_u128)f_w * ch + (((grisu_u128)f_w * cl) >> 64);
}

/* compare c * 2^gn vs B (both < 2^128): -1, 0 or +1. Exact. */
static inline int grisu_cmp_shr(uint64_t c, grisu_u128 B, int gn)
{
    grisu_u128 q, r;
    q = B >> gn;
    r = B - (q << gn);
    if ((grisu_u128)c > q)
        return 1;
    if ((grisu_u128)c < q)
        return -1;
    return (r == 0) ? 0 : -1;
}

/* Round-trip verdict for the candidate value, scaled to the cached
   frame, against the safe interval:
   1  : cv*2^gn in [Bm2+12, Bp2-12] -> strictly inside the rounding
        interval with margin >= 10 scaled ulp > error <= 2: proven.
   0  : cv*2^gn outside [Bm2-4, Bp2+4] -> strictly outside with
        margin >= 2.5 scaled ulp > error: proven cannot round-trip.
   -1 : uncertain (falls back).
   With Bm2/Bp2 hoisted to quotients qm/qh (remainders rm/rh are
   ~2^(gn-1), so adding/subtracting the 12/4 margins never carries and
   the remainder-of-compare is never 0), all four scaled comparisons
   reduce to three u64 compares against qm and qh:
     cv >= Bm2+12  <=> cv > qm      cv <= Bp2-12 <=> cv <= qh
     cv >  Bp2+4   <=> cv > qh      cv <  Bm2-4  <=> cv <  qm  */
static int grisu_roundtrip(uint64_t cv, grisu_u128 Bm2, grisu_u128 Bp2, int gn)
{
    if (grisu_cmp_shr(cv, Bm2 + 12, gn) >= 0 &&
        grisu_cmp_shr(cv, Bp2 - 12, gn) <= 0)
        return 1;
    if (grisu_cmp_shr(cv, Bm2 - 4, gn) < 0 ||
        grisu_cmp_shr(cv, Bp2 + 4, gn) > 0)
        return 0;
    return -1;
}

/* Candidate division: RNDN(D17 / 10^k) replicating the exact loop's
   RNDN(x * 10^(p-E)) candidate. D17 = round(T) where T is the true
   scaled value with known fractional part: T = D17 + f with
   f*2^gn = rem17 (< half17) or rem17 - 2^gn (>= half17), |error| <= 2.
   Normally the quotient q + (r > S/2) is decided by r alone (|2r - S|
   odd or zero distance, proven since |f/S| <= (0.5+eps)/S cannot tip
   any non-exact remainder). When r == S/2 exactly, the decision rests
   on f's sign: known whenever min(rem17, 2^gn - rem17) > 2; a true tie
   (f == 0 within error) sets *pamb and falls back. */
static inline uint64_t grisu_div10k(uint64_t D17, int k, grisu_u128 rem17,
                                    grisu_u128 half17, int gn, BOOL *pamb)
{
    uint64_t S, q, r;
    if (k == 0) {               /* p = P_max: the candidate IS D17 */
        *pamb = FALSE;
        return D17;
    }
    S = grisu_pow10[k];
    q = D17 / S;
    r = D17 % S;

    if (2 * r == S) {
        grisu_u128 half = ((grisu_u128)1) << (gn - 1);
        if (rem17 < half17) {
            /* T above D17: T/S > q + 1/2 strictly (f > 2^-gn/... > 0) */
            if (rem17 > 2)
                return q + 1;
        } else {
            /* T below or at D17 */
            if ((( (grisu_u128)1 << gn) - rem17) > 2)
                return q;
        }
        (void)half;
        *pamb = TRUE;
        return 0;
    }
    *pamb = FALSE;
    return q + (r > S / 2);
}

/* Scan replicating the exact search below STEP BY STEP (same decimal
   exponent resets, same E++ adjust, same trailing-zero strip, same
   stop-at-first-failure), with every candidate and every accept/reject
   verdict proven equal to what the exact bignum computation yields:
   - candidates are RNDN(D17 / 10^k); D17 is proven equal to the exact
     loop's mant_max-digit mantissa, and the division rounding is
     proven equal unless exactly .5 (ambiguity -> fallback), which
     matches RNDN because 10^k is even for k >= 1;
   - the E++ adjust fires under the same condition (candidate == 10^p);
     after the retry it can be shown never to fire again (the retry
     zone is included in every lower-precision adjust zone), so one
     candidate per precision in the base frame suffices, rescaled by
     10 when the adjust used the E0+1 frame;
   - accept verdicts require the candidate value strictly inside the
     round-trip interval with margin > error; reject verdicts require
     strictly outside with margin > error; anything else falls back.
   On TRUE, (mant, P, E) is identical to the exact search's output. */
static BOOL js_dtoa_shortest10(uint64_t m, int e, uint64_t *pmant,
                               int *pP, int *pE)
{
    uint64_t f_w, f_p, f_m, D17;
    grisu_u128 A2, Bp2, Bm2, rem17 = 0, half17 = 0;
    int E0, gn, attempt;

    /* NOTE: this function is called only for inputs outside the binade
       class (see the caller: m == 2^52 && e >= -1021, i.e. d an exact
       power of two, is routed to js_dtoa_shortest_pow2 before this
       point). Inside the class the round-trip interval is asymmetric
       (half an ulp below, a full ulp above), the round-tripping
       precisions are NOT contiguous, and the fixed 12/4-ulp margins can
       neither prove nor refute candidate verdicts near the tight lower
       boundary; the exact straddle-pair scan computes that class. */
    E0 = 1 + mul_log2_radix(e - 1, 10);

    f_w = m << 11;              /* in [2^63, 2^64) */
    if (e >= -1021) {
        /* normal: neighbors 2^(e-53) apart (2^(e-54) below a power of 2) */
        f_p = (2 * m + 1) << 10;
        if (m == ((uint64_t)1 << 52))
            f_m = (4 * m - 1) << 9; /* lower boundary is closer by half an ulp */
        else
            f_m = (2 * m - 1) << 10;
    } else {
        /* denormal (engine frame: d = m*2^(e-53), e <= -1022): the true
           grid gap is 2^-1074 on BOTH sides, i.e. 2^l * 2^(e-53) with
           l = -1021 - e in [1, 52] (the shift the denormal normalization
           applied). Half-gaps widen the round-trip interval accordingly. */
        int l = -1021 - e;
        f_p = (2 * m + ((uint64_t)1 << l)) << 10;
        f_m = (2 * m - ((uint64_t)1 << l)) << 10; /* 0 for the smallest denormal */
    }

    D17 = 0;
    for (attempt = 0;; attempt++) {
        int alpha = 17 - E0, idx;
        grisu_u128 n, r, half;
        if (attempt >= 2)
            return FALSE;       /* the exact loop allows at most one E++ */
        idx = alpha - GRISU_ALPHA_MIN;
        if (idx < 0 || idx >= GRISU_ALPHA_COUNT)
            return FALSE;
        A2  = grisu_mul_hi128(f_w, grisu_pow_hi[idx], grisu_pow_lo[idx]);
        Bp2 = grisu_mul_hi128(f_p, grisu_pow_hi[idx], grisu_pow_lo[idx]);
        Bm2 = grisu_mul_hi128(f_m, grisu_pow_hi[idx], grisu_pow_lo[idx]);
        gn = -(e + grisu_pow_ec[idx]); /* in [68, 76] by construction */
        if (gn < 60 || gn > 90)
            return FALSE;       /* cannot happen; paranoia */
        /* D17 = round-half-even(A2 / 2^gn); uncertain near a .5 tie
           (a real tie must be resolved by the exact path) */
        n = A2 >> gn;
        r = A2 & ((((grisu_u128)1) << gn) - 1);
        half = ((grisu_u128)1) << (gn - 1);
        rem17 = r;
        half17 = half;
        if (r + 4 >= half && r <= half + 4)
            return FALSE;
        if (r > half)
            n++;
        D17 = (uint64_t)n;
        if (D17 >= (uint64_t)100000000000000000ULL) {
            /* mantissa overflow: same E++ adjust as the exact loop */
            E0++;
            continue;
        }
        if (D17 < (uint64_t)10000000000000000ULL)
            return FALSE;       /* cannot happen; paranoia */
        break;
    }

    {
        uint64_t mant_found = 0;
        int P_found = 0, E_found = 0, p;
        BOOL first = TRUE;
        p = 17;
        for (;;) {
            uint64_t mant_raw, mant_s, cv;
            int p_s, E_used;
            BOOL amb;
            /* candidate at precision p in the E0 frame */
            mant_raw = grisu_div10k(D17, 17 - p, rem17, half17, gn, &amb);
            if (amb)
                return FALSE;
            E_used = E0;
            if (mant_raw >= grisu_pow10[p]) {
                /* mantissa overflow: the exact loop's E++ adjust,
                   recompute in the E0+1 frame */
                mant_raw = grisu_div10k(D17, 18 - p, rem17, half17, gn, &amb);
                if (amb)
                    return FALSE;
                E_used = E0 + 1;
            }
            /* remove useless trailing zero digits, as the exact loop does */
            mant_s = mant_raw;
            p_s = p;
            while (mant_s % 10 == 0) {
                mant_s /= 10;
                p_s--;
            }
            /* candidate value scaled to the cached frame:
               v * 10^(17-E0) = mant_s * 10^(17-p_s) * (10 if E_used == E0+1) */
            cv = mant_s * grisu_pow10[17 - p_s];
            if (E_used != E0)
                cv *= 10;
            if (first) {
                /* P_max is guaranteed to work: accepted unconditionally */
                first = FALSE;
            } else {
                int v = grisu_roundtrip(cv, Bm2, Bp2, gn);
                if (v < 0)
                    return FALSE;
                if (v == 0)
                    break;      /* exact loop stops at the first failure */
            }
            mant_found = mant_s;
            P_found = p_s;
            E_found = E_used;
            if (p_s == 1)
                break;
            p = p_s - 1;
        }
        *pmant = mant_found;
        *pP = P_found;
        *pE = E_found;
    }
    return TRUE;
}

#endif /* __SIZEOF_INT128__ */

/* return a maximum bound of the string length. The bound depends on
   'd' only if format = JS_DTOA_FORMAT_FRAC or if JS_DTOA_EXP_DISABLED
   is enabled. */
int js_dtoa_max_len(double d, int radix, int n_digits, int flags)
{
    int fmt = flags & JS_DTOA_FORMAT_MASK;
    int n, e;
    uint64_t a;

    if (fmt != JS_DTOA_FORMAT_FRAC) {
        if (fmt == JS_DTOA_FORMAT_FREE) {
            n = dtoa_max_digits_table[radix - 2];
        } else {
            n = n_digits;
        }
        if ((flags & JS_DTOA_EXP_MASK) == JS_DTOA_EXP_DISABLED) {
            /* no exponential */
            a = float64_as_uint64(d);
            e = (a >> 52) & 0x7ff;
            if (e == 0x7ff) {
                /* NaN, Infinity */
                n = 0;
            } else {
                e -= 1023;
                /* XXX: adjust */
                n += 10 + abs(mul_log2_radix(e - 1, radix));
            }
        } else {
            /* extra: sign, 1 dot and exponent "e-1000" */
            n += 1 + 1 + 6;
        }
    } else {
        a = float64_as_uint64(d);
        e = (a >> 52) & 0x7ff;
        if (e == 0x7ff) {
            /* NaN, Infinity */
            n = 0;
        } else {
            /* high bound for the integer part */
            e -= 1023;
            /* x < 2^(e + 1) */
            if (e < 0) {
                n = 1;
            } else {
                n = 2 + mul_log2_radix(e - 1, radix);
            }
            /* sign, extra digit, 1 dot */
            n += 1 + 1 + 1 + n_digits;
        }
    }
    return max_int(n, 9); /* also include NaN and [-]Infinity */
}

#if defined(__SANITIZE_ADDRESS__) && 0
static void *dtoa_malloc(uint64_t **pptr, size_t size)
{
    return malloc(size);
}
static void dtoa_free(void *ptr)
{
    free(ptr);
}
#else
static void *dtoa_malloc(uint64_t **pptr, size_t size)
{
    void *ret;
    ret = *pptr;
    *pptr += (size + 7) / 8;
    return ret;
}

static void dtoa_free(void *ptr)
{
}
#endif

/* Shortest radix-10 form of an exact power of two: d = m*2^(e-53) with
   m == 2^52 and e >= -1021 (normal). This class has an asymmetric round
   trip interval (half an ulp to the neighbor below, a full ulp to the
   neighbor above), so the P-digit candidates that round-trip are NOT a
   contiguous run of precisions AND the nearest P-digit decimal can fall
   outside the tight lower boundary while its coarser neighbor stays
   inside -- the nearest-only first-failure search of js_dtoa and the
   fixed-margin proofs of js_dtoa_shortest10 are both unable to represent
   it (measured: 54 powers of two printed 17 digits where node prints the
   true shortest).

   Algorithm (== ECMA-262 Number::toString / V8 shortest semantics): scan
   P = 1..17; at each P the candidate grid is anchored at the engine frame
   E0 (x = d*10^(P-E0) lies in [10^(P-1), 10^P) by construction of E0), the
   two decimals of that grid straddling d are floor(x) (exact: JS_RNDZ) and
   floor(x)+1, and the nearest of the two (exact JS_RNDN, ties to even) is
   tried first. Each candidate is trailing-zero-stripped and accepted only
   if the unchanged exact bignum round-trip (mul_pow_round_to_d) proves it
   converts back to d. The first P with a proven candidate yields exactly
   node's output: node prints the in-interval decimal closest to d at the
   minimal digit count, the closest in-interval decimal is always a member
   of the straddle pair (if the nearest is outside, both straddles are
   outside, which pins the interval narrower than one grid step and rules
   out floor(x)+2 needing a wider upper half), ties-to-even is JS_RNDN, and
   a trailing-zero winner would contradict minimality. */
static no_inline int js_dtoa_shortest_pow2(mpb_t *tmp1, uint64_t m, int e,
                                           uint64_t *pmant, int *pP, int *pE)
{
    uint64_t target_m = m, cand[2];
    int target_e = e, E0, P, i;

    /* the caller's E equals this (same formula, radix 10), but recomputing
       it here keeps the caller's E out of the fast path's live ranges */
    E0 = 1 + mul_log2_radix(e - 1, 10);

    for (P = 1; P <= 17; P++) {
        uint64_t n_lo, n_near;
        int found = FALSE;
        mul_pow_round(tmp1, m, e - 53, 10, 0, P - E0, JS_RNDN);
        n_near = mpb_get_u64(tmp1);
        mul_pow_round(tmp1, m, e - 53, 10, 0, P - E0, JS_RNDZ);
        n_lo = mpb_get_u64(tmp1);
        cand[0] = n_near;                    /* closest straddle first */
        cand[1] = (n_near == n_lo) ? n_lo + 1 : n_lo;
        for (i = 0; i < 2 && !found; i++) {
            uint64_t mant = cand[i], m1;
            int Pc = P, E = E0, e1;
            /* trailing-zero strip (same as the exact loop); a candidate
               that IS a power of ten (d just below/above 10^E0) strips
               down to mant == 1 with Pc == 0, i.e. the value 10^E0, which
               is written mant = 1 at Pc = 1 in the E0+1 frame -- the
               exact loop's E++ adjust applied to the candidate value */
            while (mant % 10 == 0) {
                mant /= 10;
                Pc--;
            }
            if (Pc == 0) {
                mant = 1;
                Pc = 1;
                E = E0 + 1;
            }
            mpb_set_u64(tmp1, mant);
            m1 = mul_pow_round_to_d(&e1, tmp1, 10, 0, E - Pc, JS_RNDN);
            if (m1 == target_m && e1 == target_e) {
                *pmant = mant;
                *pP = Pc;
                *pE = E;
                found = TRUE;
            }
        }
        if (found)
            return TRUE;
    }
    return FALSE; /* unreachable: P=17 always round-trips */
}

/* return the length */
int js_dtoa(char *buf, double d, int radix, int n_digits, int flags,
            JSDTOATempMem *tmp_mem)
{
    uint64_t a, m, *mptr = tmp_mem->mem;
    int e, sgn, l, E, P, i, E_max, radix1, radix_shift;
    char *q;
    mpb_t *tmp1, *mant_max;
    int fmt = flags & JS_DTOA_FORMAT_MASK;

    tmp1 = dtoa_malloc(&mptr, sizeof(mpb_t) + sizeof(limb_t) * DBIGNUM_LEN_MAX);
    mant_max = dtoa_malloc(&mptr, sizeof(mpb_t) + sizeof(limb_t) * MANT_LEN_MAX);
    assert((mptr - tmp_mem->mem) <= sizeof(JSDTOATempMem) / sizeof(mptr[0]));

    radix_shift = ctz32(radix);
    radix1 = radix >> radix_shift;
    a = float64_as_uint64(d);
    sgn = a >> 63;
    e = (a >> 52) & 0x7ff;
    m = a & (((uint64_t)1 << 52) - 1);
    q = buf;
    if (e == 0x7ff) {
        if (m == 0) {
            if (sgn)
                *q++ = '-';
            memcpy(q, "Infinity", 8);
            q += 8;
        } else {
            memcpy(q, "NaN", 3);
            q += 3;
        }
        goto done;
    } else if (e == 0) {
        if (m == 0) {
            tmp1->len = 1;
            tmp1->tab[0] = 0;
            E = 1;
            if (fmt == JS_DTOA_FORMAT_FREE)
                P = 1;
            else if (fmt == JS_DTOA_FORMAT_FRAC)
                P = n_digits + 1;
            else
                P = n_digits;
            /* "-0" is displayed as "0" if JS_DTOA_MINUS_ZERO is not present */
            if (sgn && (flags & JS_DTOA_MINUS_ZERO))
                *q++ = '-';
            goto output;
        }
        /* denormal number: convert to a normal number */
        l = clz64(m) - 11;
        e -= l - 1;
        m <<= l;
    } else {
        m |= (uint64_t)1 << 52;
    }
    if (sgn)
        *q++ = '-';
    /* remove the bias */
    e -= 1022;
    /* d = 2^(e-53)*m */
    //    printf("m=0x%016" PRIx64 " e=%d\n", m, e);
#ifdef USE_FAST_INT
    if (fmt == JS_DTOA_FORMAT_FREE &&
        e >= 1 && e <= 53 &&
        (m & (((uint64_t)1 << (53 - e)) - 1)) == 0 &&
        (flags & JS_DTOA_EXP_MASK) != JS_DTOA_EXP_ENABLED) {
        m >>= 53 - e;
        /* 'm' is never zero */
        q += u64toa_radix(q, m, radix);
        goto done;
    }
#endif
    
    /* this choice of E implies F=round(x*B^(P-E) is such as: 
       B^(P-1) <= F < 2.B^P. */
    E = 1 + mul_log2_radix(e - 1, radix);
    
    if (fmt == JS_DTOA_FORMAT_FREE) {
        int P_max, E0, e1, E_found, P_found;
        uint64_t m1, mant_found, mant, mant_max1;
        if (radix == 10 && m == ((uint64_t)1 << 52) && e >= -1021 &&
            (flags & JS_DTOA_EXP_MASK) != JS_DTOA_EXP_DISABLED) {
            /* Binade-boundary class (d an exact power of two: m == 2^52
               and normal). The round-trip interval of d is asymmetric
               (half an ulp below, a full ulp above), which defeats both
               the Grisu fast path's fixed margins and the nearest-only
               first-failure search below (the nearest candidate can fall
               outside the tight boundary while its coarser neighbor stays
               inside). Route the class to a dedicated exact scan that
               tests both grid decimals straddling d at each precision and
               proves the round trip with the same exact machinery.
               Radix 10 only: it is the radix with a spec-mandated
               shortest form (Number::toString) and the only radix
               js_dtoa_shortest10 accelerates.
               EXP_DISABLED excluded like the Grisu path beside: the scan
               only proves shortest round-trip, not the fixed-notation
               digit count the disabled flag promises (no radix-10 caller
               combines FREE + EXP_DISABLED today; this is hygiene). */
            uint64_t pow2_mant;
            if (js_dtoa_shortest_pow2(tmp1, m, e, &pow2_mant, &P, &E)) {
                mpb_set_u64(tmp1, pow2_mant);
                goto output;
            }
        }
#if defined(__SIZEOF_INT128__)
        /* Grisu-style shortest fast path (radix 10): provably the same
           (mant, P, E) as the exact search below, or returns FALSE and
           the exact search runs unchanged. Inputs in the binade class are
           excluded by the predicate above. */
        uint64_t grisu_mant;
        if (radix == 10 && !(m == ((uint64_t)1 << 52) && e >= -1021) &&
            (flags & JS_DTOA_EXP_MASK) != JS_DTOA_EXP_DISABLED &&
            js_dtoa_shortest10(m, e, &grisu_mant, &P, &E)) {
            mpb_set_u64(tmp1, grisu_mant);
            goto output;
        }
#endif
        /* P_max is guarranteed to work by construction */
        P_max = dtoa_max_digits_table[radix - 2];
        E0 = E;
        E_found = 0;
        P_found = 0;
        mant_found = 0;
        /* find the minimum number of digits by successive tries */
        P = P_max; /* P_max is guarateed to work */
        for(;;) {
            /* mant_max always fits on 64 bits */
            mant_max1 = pow_ui(radix, P);
            /* compute the mantissa in base B */
            E = E0;
            for(;;) {
                /* XXX: add inexact flag */
                mul_pow_round(tmp1, m, e - 53, radix1, radix_shift, P - E, JS_RNDN);
                mant = mpb_get_u64(tmp1);
                if (mant < mant_max1)
                    break;
                E++; /* at most one iteration is possible */
            }
            /* remove useless trailing zero digits */
            while ((mant % radix) == 0) {
                mant /= radix;
                P--;
            }
            /* garanteed to work for P = P_max */
            if (P_found == 0)
                goto prec_found;
            /* convert back to base 2 */
            mpb_set_u64(tmp1, mant);
            m1 = mul_pow_round_to_d(&e1, tmp1, radix1, radix_shift, E - P, JS_RNDN);
            //            printf("P=%2d: m=0x%016" PRIx64 " e=%d m1=0x%016" PRIx64 " e1=%d\n", P, m, e, m1, e1);
            /* Note: (m, e) is never zero here, so the exponent for m1
               = 0 does not matter */
            if (m1 == m && e1 == e) {
            prec_found:
                P_found = P;
                E_found = E;
                mant_found = mant;
                if (P == 1)
                    break;
                P--; /* try lower exponent */
            } else {
                break;
            }
        }
        P = P_found;
        E = E_found;
        mpb_set_u64(tmp1, mant_found);
#ifdef JS_DTOA_DUMP_STATS
        if (radix == 10) {
            out_len_count[P - 1]++;
        }
#endif        
    } else if (fmt == JS_DTOA_FORMAT_FRAC) {
        int len;

        assert(n_digits >= 0 && n_digits <= JS_DTOA_MAX_DIGITS);
        /* P = max_int(E, 1) + n_digits; */
        /* frac is rounded using RNDNA */
        mul_pow_round(tmp1, m, e - 53, radix1, radix_shift, n_digits, JS_RNDNA);

        /* we add one extra digit on the left and remove it if needed
           to avoid testing if the result is < radix^P */
        len = output_digits(q, tmp1, radix, max_int(E + 1, 1) + n_digits,
                            max_int(E + 1, 1));
        if (q[0] == '0' && len >= 2 && q[1] != '.') {
            len--;
            memmove(q, q + 1, len);
        }
        q += len;
        goto done;
    } else {
        int pow_shift;
        assert(n_digits >= 1 && n_digits <= JS_DTOA_MAX_DIGITS);
        P = n_digits;
        /* mant_max = radix^P */
        mant_max->len = 1;
        mant_max->tab[0] = 1;
        pow_shift = mul_pow(mant_max, radix1, radix_shift, P, FALSE, 0);
        mpb_shr_round(mant_max, pow_shift, JS_RNDZ);
        
        for(;;) {
            /* fixed and frac are rounded using RNDNA */
            mul_pow_round(tmp1, m, e - 53, radix1, radix_shift, P - E, JS_RNDNA);
            if (mpb_cmp(tmp1, mant_max) < 0)
                break;
            E++; /* at most one iteration is possible */
        }
    }
 output:
    if (fmt == JS_DTOA_FORMAT_FIXED)
        E_max = n_digits;
    else
        E_max = dtoa_max_digits_table[radix - 2] + 4;
    if ((flags & JS_DTOA_EXP_MASK) == JS_DTOA_EXP_ENABLED ||
        ((flags & JS_DTOA_EXP_MASK) == JS_DTOA_EXP_AUTO && (E <= -6 || E > E_max))) {
        q += output_digits(q, tmp1, radix, P, 1);
        E--;
        if (radix == 10) {
            *q++ = 'e';
        } else if (radix1 == 1 && radix_shift <= 4) {
            E *= radix_shift;
            *q++ = 'p';
        } else {
            *q++ = '@';
        }
        if (E < 0) {
            *q++ = '-';
            E = -E;
        } else {
            *q++ = '+';
        }
        q += u32toa(q, E);
    } else if (E <= 0) {
        *q++ = '0';
        *q++ = '.';
        for(i = 0; i < -E; i++)
            *q++ = '0';
        q += output_digits(q, tmp1, radix, P, P);
    } else {
        q += output_digits(q, tmp1, radix, P, min_int(P, E));
        for(i = 0; i < E - P; i++)
            *q++ = '0';
    }
 done:
    *q = '\0';
    dtoa_free(mant_max);
    dtoa_free(tmp1);
    return q - buf;
}

static inline int to_digit(int c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    else if (c >= 'A' && c <= 'Z')
        return c - 'A' + 10;
    else if (c >= 'a' && c <= 'z')
        return c - 'a' + 10;
    else
        return 36;
}

/* r = r * radix_base + a. radix_base = 0 means radix_base = 2^32 */
static void mpb_mul1_base(mpb_t *r, limb_t radix_base, limb_t a)
{
    int i;
    if (r->tab[0] == 0 && r->len == 1) {
        r->tab[0] = a;
    } else {
        if (radix_base == 0) {
            for(i = r->len; i >= 0; i--) {
                r->tab[i + 1] = r->tab[i];
            }
            r->tab[0] = a;
        } else {
            r->tab[r->len] = mp_mul1(r->tab, r->tab, r->len,
                                     radix_base, a);
        }
        r->len++;
        mpb_renorm(r);
    }
}

#ifndef JS_ATOD_NO_FASTPATH
/* 10^0 .. 10^22 are all exactly representable as IEEE-754 double
   (10^k = 2^k * 5^k and 5^22 < 2^52), so they can be used as exact
   multipliers/divisors in the js_atod fast path below. */
static const double js_atod_pow10[23] = {
    1e0,  1e1,  1e2,  1e3,  1e4,  1e5,  1e6,  1e7,
    1e8,  1e9,  1e10, 1e11, 1e12, 1e13, 1e14, 1e15,
    1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22,
};

#define JS_ATOD_FAST_MAX_DIGITS 19   /* 10^19-1 still fits a uint64_t */

/* Tight scanner for the shape nearly every real input has: base 10, no digit
   separators, no radix prefix, a mantissa that fits an integer double, and a
   decimal exponent small enough that the conversion is exact.
 *
 * The exact fast path further down already skips the *bignum* tail, but the
 * general parser still runs first: a prologue that sizes radix tables and a
 * per-digit loop carrying dot, separator, radix and digit-cap tests. Measured
 * by compiling with JS_ATOD_NO_FASTPATH, that tail is only 16% of js_atod on
 * decimal input -- the scan and prologue are the other 84%, which is what this
 * bypasses.
 *
 * Returns FALSE for anything it does not fully understand, having consumed
 * nothing, and the general parser below runs unchanged. Every bail is
 * conservative: an input this rejects is not necessarily invalid, only not
 * provably exact here. `radix == 0` inputs that could carry a prefix, legacy
 * octal, "Infinity", an 'e' with no digits after it -- all fall through rather
 * than being second-guessed, because the general parser owns those rules and
 * two implementations of them would drift.
 *
 * Exactness: with a mantissa below 2^53 and |exponent| <= 22, both the
 * mantissa and 10^|exponent| are exactly representable, so one IEEE-754
 * multiply or divide under round-to-nearest is the correctly-rounded result
 * (Clinger 1990) -- bit-identical to the bignum path, which is what
 * tests/test_atod_diff.js checks against a JS_ATOD_NO_FASTPATH build. */
static BOOL js_atod_fast(const char *str, const char **pnext, int flags,
                         double *pres)
{
    const char *p = str;
    uint64_t m;
    int ndig, frac, expn, is_neg;
    double d;

    is_neg = 0;
    if (*p == '+') {
        p++;
    } else if (*p == '-') {
        is_neg = 1;
        p++;
    }
    /* a leading zero before an alphanumeric may be a radix prefix or a legacy
       octal literal; both belong to the general parser */
    if (p[0] == '0' && to_digit((uint8_t)p[1]) < 36)
        return FALSE;

    m = 0;
    ndig = 0;
    frac = 0;
    expn = 0;
    while (*p >= '0' && *p <= '9') {
        if (unlikely(ndig >= JS_ATOD_FAST_MAX_DIGITS))
            return FALSE;
        m = m * 10 + (uint64_t)(*p - '0');
        ndig++;
        p++;
    }
    if (!(flags & JS_ATOD_INT_ONLY) && *p == '.') {
        /* the general parser accepts "1." and ".5" but not "." alone */
        if (ndig == 0 && !(p[1] >= '0' && p[1] <= '9'))
            return FALSE;
        p++;
        while (*p >= '0' && *p <= '9') {
            if (unlikely(ndig >= JS_ATOD_FAST_MAX_DIGITS))
                return FALSE;
            m = m * 10 + (uint64_t)(*p - '0');
            ndig++;
            frac++;
            p++;
        }
    }
    if (ndig == 0)
        return FALSE;
    if (!(flags & JS_ATOD_INT_ONLY) && (*p == 'e' || *p == 'E')) {
        const char *q = p + 1;
        int e = 0, exp_is_neg = 0;
        if (*q == '+') {
            q++;
        } else if (*q == '-') {
            exp_is_neg = 1;
            q++;
        }
        /* the general parser fails the whole conversion when no digit follows
           the exponent marker -- it does not stop short of it. Leave that
           decision there rather than reproducing it. */
        if (!(*q >= '0' && *q <= '9'))
            return FALSE;
        do {
            if (e <= 1000)      /* anything larger leaves the exact domain */
                e = e * 10 + (*q - '0');
            q++;
        } while (*q >= '0' && *q <= '9');
        expn = exp_is_neg ? -e : e;
        p = q;
    }
    expn -= frac;
    if (m >= ((uint64_t)1 << 53) || expn < -22 || expn > 22)
        return FALSE;

    d = (double)m;
    if (expn >= 0)
        d = d * js_atod_pow10[expn];
    else
        d = d / js_atod_pow10[-expn];
    /* negation last, so "-0" yields -0.0 exactly as the general parser's
       `a |= is_neg << 63` does */
    *pres = is_neg ? -d : d;
    if (pnext)
        *pnext = p;
    return TRUE;
}
#endif

/* XXX: add fast path for small integers */
double js_atod(const char *str, const char **pnext, int radix, int flags,
               JSATODTempMem *tmp_mem)
{
    uint64_t *mptr = tmp_mem->mem;
    const char *p, *p_start;
    limb_t cur_limb, radix_base, extra_digits;
    int is_neg, digit_count, limb_digit_count, digits_per_limb, sep, radix1, radix_shift;
    int radix_bits, expn, e, max_digits, expn_offset, dot_pos, sig_pos, pos;
    mpb_t *tmp0;
    double dval;
    BOOL is_bin_exp, is_zero, expn_overflow;
    uint64_t m, a;
#ifndef JS_ATOD_NO_FASTPATH
    uint64_t fast_m;   /* significant digits accumulated as an integer */
    int fast_ndig;     /* number of significant digits in fast_m */
    BOOL fast_ok;      /* exact fast path still viable for this input */

    /* Before the prologue: it sizes radix tables and reserves the mantissa
       bignum, all of which is waste when the input is ordinary decimal. */
    if (likely((radix == 0 || radix == 10) &&
               !(flags & JS_ATOD_ACCEPT_UNDERSCORES))) {
        double fast_d;
        if (js_atod_fast(str, pnext, flags, &fast_d))
            return fast_d;
    }
#endif

    tmp0 = dtoa_malloc(&mptr, sizeof(mpb_t) + sizeof(limb_t) * DBIGNUM_LEN_MAX);
    assert((mptr - tmp_mem->mem) <= sizeof(JSATODTempMem) / sizeof(mptr[0]));
    /* optional separator between digits */
    sep = (flags & JS_ATOD_ACCEPT_UNDERSCORES) ? '_' : 256;

    p = str;
    is_neg = 0;
    if (p[0] == '+') {
        p++;
        p_start = p;
    } else if (p[0] == '-') {
        is_neg = 1;
        p++;
        p_start = p;
    } else {
        p_start = p;
    }
    
    if (p[0] == '0') {
        if ((p[1] == 'x' || p[1] == 'X') &&
            (radix == 0 || radix == 16)) {
            p += 2;
            radix = 16;
        } else if ((p[1] == 'o' || p[1] == 'O') &&
                   radix == 0 && (flags & JS_ATOD_ACCEPT_BIN_OCT)) {
            p += 2;
            radix = 8;
        } else if ((p[1] == 'b' || p[1] == 'B') &&
                   radix == 0 && (flags & JS_ATOD_ACCEPT_BIN_OCT)) {
            p += 2;
            radix = 2;
        } else if ((p[1] >= '0' && p[1] <= '9') &&
                   radix == 0 && (flags & JS_ATOD_ACCEPT_LEGACY_OCTAL)) {
            int i;
            sep = 256;
            for (i = 1; (p[i] >= '0' && p[i] <= '7'); i++)
                continue;
            if (p[i] == '8' || p[i] == '9')
                goto no_prefix;
            p += 1;
            radix = 8;
        } else {
            goto no_prefix;
        }
        /* there must be a digit after the prefix */
        if (to_digit((uint8_t)*p) >= radix)
            goto fail;
    no_prefix: ;
    } else {
        if (!(flags & JS_ATOD_INT_ONLY) && strstart(p, "Infinity", &p))
            goto overflow;
    }
    if (radix == 0)
        radix = 10;

#ifndef JS_ATOD_NO_FASTPATH
    /* exact fast-path accumulator; only armed for base 10 */
    fast_m = 0;
    fast_ndig = 0;
    fast_ok = (radix == 10);
#endif

    cur_limb = 0;
    expn_offset = 0;
    digit_count = 0;
    limb_digit_count = 0;
    max_digits = atod_max_digits_table[radix - 2];
    digits_per_limb = digits_per_limb_table[radix - 2];
    radix_base = radix_base_table[radix - 2];
    radix_shift = ctz32(radix);
    radix1 = radix >> radix_shift;
    if (radix1 == 1) {
        /* radix = 2^radix_bits */
        radix_bits = radix_shift;
    } else {
        radix_bits = 0;
    }
    tmp0->len = 1;
    tmp0->tab[0] = 0;
    extra_digits = 0;
    pos = 0;
    dot_pos = -1;
    /* skip leading zeros */
    for(;;) {
        if (*p == '.' && (p > p_start || to_digit(p[1]) < radix) &&
            !(flags & JS_ATOD_INT_ONLY)) {
            if (*p == sep)
                goto fail;
            if (dot_pos >= 0)
                break;
            dot_pos = pos;
            p++;
        }
        if (*p == sep && p > p_start && p[1] == '0')
            p++;
        if (*p != '0')
            break;
        p++;
        pos++;
    }
    
    sig_pos = pos;
    for(;;) {
        limb_t c;
        if (*p == '.' && (p > p_start || to_digit(p[1]) < radix) &&
            !(flags & JS_ATOD_INT_ONLY)) {
            if (*p == sep)
                goto fail;
            if (dot_pos >= 0)
                break;
            dot_pos = pos;
            p++;
        }
        if (*p == sep && p > p_start && to_digit(p[1]) < radix)
            p++;
        c = to_digit(*p);
        if (c >= radix)
            break;
        p++;
        pos++;
#ifndef JS_ATOD_NO_FASTPATH
        /* Accumulate the significant digits into a u64 for the exact fast
           path. Cap at 16 digits: beyond that the value cannot stay below
           2^53, and we avoid any u64 overflow. While the fast path is still
           viable the mantissa bignum is left unbuilt -- it is pure waste when
           the fast path fires. It is instead materialised from fast_m in the
           slow path below (<= 16 digits), or, once a 17th digit kills the fast
           path, seeded here from the 16 digits already in fast_m so the loop
           carries on building it inline with no re-scan. */
        if (fast_ok) {
            if (likely(fast_ndig < 16)) {
                fast_m = fast_m * 10 + (uint64_t)c;
                fast_ndig++;
                if (digit_count < max_digits)
                    digit_count++;
                continue;
            }
            fast_ok = FALSE;
            /* fast_m now holds exactly 16 base-10 digits (>= 10^15, hence two
               limbs); store it as the running bignum, then fall through to
               append this 17th digit inline. cur_limb/limb_digit_count are 0. */
            /* mpb_set_u64, not a hand-rolled 32-bit split: at 64-bit limbs the
               latter writes the whole value into tab[0] AND a garbage second
               limb, which lands as an exponent exactly LIMB_BITS too large. */
            mpb_set_u64(tmp0, fast_m);
        }
#endif
        if (digit_count < max_digits) {
            /* XXX: could be faster when radix_bits != 0 */
            cur_limb = cur_limb * radix + c;
            limb_digit_count++;
            if (limb_digit_count == digits_per_limb) {
                mpb_mul1_base(tmp0, radix_base, cur_limb);
                cur_limb = 0;
                limb_digit_count = 0;
            }
            digit_count++;
        } else {
            extra_digits |= c;
        }
    }
    if (limb_digit_count != 0) {
        mpb_mul1_base(tmp0, pow_ui(radix, limb_digit_count), cur_limb);
    }
    if (digit_count == 0) {
        is_zero = TRUE;
        expn_offset = 0;
    } else {
        is_zero = FALSE;
        if (dot_pos < 0)
            dot_pos = pos;
        expn_offset = sig_pos + digit_count - dot_pos;
    }
    
    /* Use the extra digits for rounding if the base is a power of
       two. Otherwise they are just truncated. */
    if (radix_bits != 0 && extra_digits != 0) {
        tmp0->tab[0] |= 1;
    }
    
    /* parse the exponent, if any */
    expn = 0;
    expn_overflow = FALSE;
    is_bin_exp = FALSE;
    if (!(flags & JS_ATOD_INT_ONLY) &&
        ((radix == 10 && (*p == 'e' || *p == 'E')) ||
         (radix != 10 && (*p == '@' ||
                          (radix_bits >= 1 && radix_bits <= 4 && (*p == 'p' || *p == 'P'))))) &&
        p > p_start) {
        BOOL exp_is_neg;
        int c;
        is_bin_exp = (*p == 'p' || *p == 'P');
        p++;
        exp_is_neg = 0;
        if (*p == '+') {
            p++;
        } else if (*p == '-') {
            exp_is_neg = 1;
            p++;
        }
        c = to_digit(*p);
        if (c >= 10)
            goto fail; /* XXX: could stop before the exponent part */
        expn = c;
        p++;
        for(;;) {
            if (*p == sep && to_digit(p[1]) < 10)
                p++;
            c = to_digit(*p);
            if (c >= 10)
                break;
            if (!expn_overflow) {
                if (unlikely(expn > ((INT32_MAX - 2 - 9) / 10))) {
                    expn_overflow = TRUE;
                } else {
                    expn = expn * 10 + c;
                }
            }
            p++;
        }
        if (exp_is_neg)
            expn = -expn;
        /* if zero result, the exponent can be arbitrarily large */
        if (!is_zero && expn_overflow) {
            if (exp_is_neg)
                a = 0;
            else
                a = (uint64_t)0x7ff << 52; /* infinity */
            goto done;
        }
    }

    if (p == p_start)
        goto fail;

    if (is_zero) {
        a = 0;
    } else {
        int expn1;
        if (radix_bits != 0) {
            if (!is_bin_exp)
                expn *= radix_bits;
            expn -= expn_offset * radix_bits;
            expn1 = expn + digit_count * radix_bits;
            if (expn1 >= 1024 + radix_bits)
                goto overflow;
            else if (expn1 <= -1075)
                goto underflow;
            m = round_to_d(&e, tmp0, -expn, JS_RNDN);
        } else {
            expn -= expn_offset;
#ifndef JS_ATOD_NO_FASTPATH
            /* Exact fast path (base 10 only, gated by fast_ok). When every
               significant digit fits an integer mantissa < 2^53 and the
               effective decimal exponent is small (|expn| <= 22), both the
               mantissa and 10^|expn| are exactly representable as doubles, so
               a single IEEE-754 multiply or divide (round-to-nearest, the
               engine forces correct FP rounding) yields the correctly-rounded
               result -- byte-identical to the bignum path (Clinger 1990). The
               product/quotient stays well within normal range (< ~1e38, >
               ~1e-22), so no overflow/subnormal handling is needed. Any input
               outside this domain falls through to the unchanged slow path. */
            if (fast_ok && fast_m < ((uint64_t)1 << 53) &&
                expn >= -22 && expn <= 22) {
                if (expn >= 0)
                    dval = (double)fast_m * js_atod_pow10[expn];
                else
                    dval = (double)fast_m / js_atod_pow10[-expn];
                if (is_neg)
                    dval = -dval;
                if (pnext)
                    *pnext = p;
                dtoa_free(tmp0);
                return dval;
            }
#endif
            expn1 = expn + digit_count;
            if (expn1 >= max_exponent[radix - 2] + 1)
                goto overflow;
            else if (expn1 <= min_exponent[radix - 2])
                goto underflow;
#ifndef JS_ATOD_NO_FASTPATH
            /* <= 16 base-10 digits but outside the fast-path domain (fast_m
               >= 2^53 or |expn| > 22): the whole mantissa integer is fast_m,
               so materialise the bignum from it (it was never built in the
               loop) rather than re-scanning the digits. */
            if (fast_ok) {
                mpb_set_u64(tmp0, fast_m);
            }
#endif
            m = mul_pow_round_to_d(&e, tmp0, radix1, radix_shift, expn, JS_RNDN);
        }
        if (m == 0) {
        underflow:
            a = 0;
        } else if (e > 1024) {
        overflow:
            /* overflow */
            a = (uint64_t)0x7ff << 52;
        } else if (e < -1073) {
            /* underflow */
            /* XXX: check rounding */
            a = 0;
        } else if (e < -1021) {
            /* subnormal */
            a = m >> (-e - 1021);
        } else {
            a = ((uint64_t)(e + 1022) << 52) | (m & (((uint64_t)1 << 52) - 1));
        }
    }
 done:
    a |= (uint64_t)is_neg << 63;
    dval = uint64_as_float64(a);
 done1:
    if (pnext)
        *pnext = p;
    dtoa_free(tmp0);
    return dval;
 fail:
    dval = NAN;
    goto done1;
}
