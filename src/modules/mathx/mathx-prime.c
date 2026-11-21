#include "mathx-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_MATHX)

static uint64_t dyn_mathx_mulmod64(uint64_t a, uint64_t b, uint64_t m)
{
    return (uint64_t)(((unsigned __int128)a * b) % m);
}

static uint64_t dyn_mathx_powmod64(uint64_t base, uint64_t exp, uint64_t m)
{
    uint64_t result = 1;
    base %= m;
    while (exp) {
        if (exp & 1)
            result = dyn_mathx_mulmod64(result, base, m);
        base = dyn_mathx_mulmod64(base, base, m);
        exp >>= 1;
    }
    return result;
}

static int dyn_mathx_is_prime_u64(uint64_t n)
{
    static const uint64_t small_primes[] = {
        2, 3, 5, 7, 11, 13, 17, 19, 23, 29, 31, 37
    };
    uint64_t d;
    int r, i;

    if (n < 2)
        return 0;
    for (i = 0; i < (int)countof(small_primes); i++) {
        if (n == small_primes[i])
            return 1;
        if (n % small_primes[i] == 0)
            return 0;
    }

    d = n - 1;
    r = 0;
    while ((d & 1) == 0) {
        d >>= 1;
        r++;
    }

    for (i = 0; i < (int)countof(small_primes); i++) {
        uint64_t a = small_primes[i];
        uint64_t x = dyn_mathx_powmod64(a, d, n);
        int j, is_composite;

        if (x == 1 || x == n - 1)
            continue;
        is_composite = 1;
        for (j = 0; j < r - 1; j++) {
            x = dyn_mathx_mulmod64(x, x, n);
            if (x == n - 1) {
                is_composite = 0;
                break;
            }
        }
        if (is_composite)
            return 0;
    }
    return 1;
}

JSValue dyn_mathx_is_prime(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    int64_t v;
    uint64_t n;
    (void)this_val;
    (void)argc;
    if (JS_IsBigInt(ctx, argv[0])) {
        int neg;
        if (dyn_mathx_bigint_mag_u64(ctx, argv[0], &n, &neg))
            return JS_EXCEPTION;
        if (neg)
            return JS_NewBool(ctx, 0);
    } else {
        if (dyn_mathx_to_int64(ctx, argv[0], &v))
            return JS_EXCEPTION;
        if (v < 2)
            return JS_NewBool(ctx, 0);
        n = (uint64_t)v;
    }
    if (n < 2)
        return JS_NewBool(ctx, 0);
    return JS_NewBool(ctx, dyn_mathx_is_prime_u64(n));
}

#endif
