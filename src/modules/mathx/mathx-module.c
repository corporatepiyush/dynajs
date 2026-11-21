#include "mathx-internal.h"
#include "mathx-nd.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_MATHX)

#define DYN_MATHX_UNARY(name, cexpr)                                       \
    static JSValue dyn_mathx_##name(JSContext* ctx, JSValueConst this_val, \
        int argc, JSValueConst* argv)                                      \
    {                                                                      \
        double x;                                                          \
        (void)this_val;                                                    \
        (void)argc;                                                        \
        if (JS_ToFloat64(ctx, &x, argv[0]))                                \
            return JS_EXCEPTION;                                           \
        return JS_NewFloat64(ctx, (cexpr));                                \
    }

#define DYN_MATHX_BINARY(name, cexpr)                                         \
    static JSValue dyn_mathx_##name(JSContext* ctx, JSValueConst this_val,    \
        int argc, JSValueConst* argv)                                         \
    {                                                                         \
        double x, y;                                                          \
        (void)this_val;                                                       \
        (void)argc;                                                           \
        if (JS_ToFloat64(ctx, &x, argv[0]) || JS_ToFloat64(ctx, &y, argv[1])) \
            return JS_EXCEPTION;                                              \
        return JS_NewFloat64(ctx, (cexpr));                                   \
    }

#define DYN_MATHX_FACTORIAL_MAX 10000

#define BITS_COUNT_GROUP(W)                                                        \
    static JSValue mxbits_leading_zeros##W(JSContext* ctx, JSValueConst this_val,  \
        int argc, JSValueConst* argv)                                              \
    {                                                                              \
        uint64_t x;                                                                \
        (void)this_val;                                                            \
        (void)argc;                                                                \
        if (mxbits_read##W(ctx, argv[0], &x))                                      \
            return JS_EXCEPTION;                                                   \
        return JS_NewInt32(ctx, dyn_bits_leading_zeros(x, (W)));                   \
    }                                                                              \
    static JSValue mxbits_trailing_zeros##W(JSContext* ctx, JSValueConst this_val, \
        int argc, JSValueConst* argv)                                              \
    {                                                                              \
        uint64_t x;                                                                \
        (void)this_val;                                                            \
        (void)argc;                                                                \
        if (mxbits_read##W(ctx, argv[0], &x))                                      \
            return JS_EXCEPTION;                                                   \
        return JS_NewInt32(ctx, dyn_bits_trailing_zeros(x, (W)));                  \
    }                                                                              \
    static JSValue mxbits_ones_count##W(JSContext* ctx, JSValueConst this_val,     \
        int argc, JSValueConst* argv)                                              \
    {                                                                              \
        uint64_t x;                                                                \
        (void)this_val;                                                            \
        (void)argc;                                                                \
        if (mxbits_read##W(ctx, argv[0], &x))                                      \
            return JS_EXCEPTION;                                                   \
        return JS_NewInt32(ctx, dyn_bits_ones_count(x));                           \
    }                                                                              \
    static JSValue mxbits_len##W(JSContext* ctx, JSValueConst this_val,            \
        int argc, JSValueConst* argv)                                              \
    {                                                                              \
        uint64_t x;                                                                \
        (void)this_val;                                                            \
        (void)argc;                                                                \
        if (mxbits_read##W(ctx, argv[0], &x))                                      \
            return JS_EXCEPTION;                                                   \
        return JS_NewInt32(ctx, dyn_bits_len64(x));                                \
    }

#define BITS_REVERSE_SMALL(W)                                               \
    static JSValue mxbits_reverse##W(JSContext* ctx, JSValueConst this_val, \
        int argc, JSValueConst* argv)                                       \
    {                                                                       \
        uint64_t x;                                                         \
        (void)this_val;                                                     \
        (void)argc;                                                         \
        if (mxbits_read##W(ctx, argv[0], &x))                               \
            return JS_EXCEPTION;                                            \
        return JS_NewUint32(ctx, (uint32_t)dyn_bits_reverse(x, (W)));       \
    }

#define BITS_ROTATE_SMALL(W)                                                    \
    static JSValue mxbits_rotate_left##W(JSContext* ctx, JSValueConst this_val, \
        int argc, JSValueConst* argv)                                           \
    {                                                                           \
        uint64_t x;                                                             \
        int32_t k;                                                              \
        (void)this_val;                                                         \
        (void)argc;                                                             \
        if (mxbits_read##W(ctx, argv[0], &x))                                   \
            return JS_EXCEPTION;                                                \
        if (JS_ToInt32(ctx, &k, argv[1]))                                       \
            return JS_EXCEPTION;                                                \
        return JS_NewUint32(ctx, (uint32_t)dyn_bits_rotate_left(x, k, (W)));    \
    }

#define BITS_ROTATE_RIGHT_SMALL(W)                                               \
    static JSValue mxbits_rotate_right##W(JSContext* ctx, JSValueConst this_val, \
        int argc, JSValueConst* argv)                                            \
    {                                                                            \
        uint64_t x;                                                              \
        int32_t k;                                                               \
        (void)this_val;                                                          \
        (void)argc;                                                              \
        if (mxbits_read##W(ctx, argv[0], &x))                                    \
            return JS_EXCEPTION;                                                 \
        if (JS_ToInt32(ctx, &k, argv[1]))                                        \
            return JS_EXCEPTION;                                                 \
        return JS_NewUint32(ctx, (uint32_t)mxbits_rotate_right(x, k, (W)));      \
    }

static double dyn_mathx_rte(double x)
{
    double t, diff;
    if (!isfinite(x))
        return x;
    t = trunc(x);
    diff = fabs(x - t);
    if (diff < 0.5)
        return t;
    if (diff > 0.5)
        return t + copysign(1.0, x);
    return (fmod(t, 2.0) == 0.0) ? t : t + copysign(1.0, x);
}

DYN_MATHX_UNARY(gamma, dyn_gamma(x))

DYN_MATHX_UNARY(erf, erf(x))

DYN_MATHX_UNARY(erfc, erfc(x))

static double mathx_cbrt_exact(double x)
{
    double r = cbrt(x);
    double whole = nearbyint(r);
    return (whole != r && whole * whole * whole == x) ? whole : r;
}

DYN_MATHX_UNARY(cbrt, mathx_cbrt_exact(x))

DYN_MATHX_UNARY(expm1, expm1(x))

DYN_MATHX_UNARY(log1p, log1p(x))

DYN_MATHX_UNARY(log2, log2(x))

DYN_MATHX_UNARY(logb, logb(x))

DYN_MATHX_UNARY(trunc, trunc(x))

DYN_MATHX_UNARY(round, round(x))

DYN_MATHX_UNARY(roundToEven, dyn_mathx_rte(x))

DYN_MATHX_BINARY(hypot, hypot(x, y))

DYN_MATHX_BINARY(copysign, copysign(x, y))

DYN_MATHX_BINARY(nextafter, nextafter(x, y))

DYN_MATHX_BINARY(remainder, remainder(x, y))

DYN_MATHX_BINARY(fmod, fmod(x, y))

static const double dyn_mathx_fact[23] = { 1.0, 1.0, 2.0, 6.0, 24.0, 120.0, 720.0, 5040.0, 40320.0, 362880.0, 3628800.0, 39916800.0, 479001600.0, 6227020800.0, 87178291200.0, 1307674368000.0, 20922789888000.0, 355687428096000.0, 6402373705728000.0, 121645100408832000.0, 2432902008176640000.0, 51090942171709440000.0, 1124000727777607680000.0 };

static double dyn_lgamma_signed(double x, int* sign)
{
    if (x >= 1.0 && x <= 23.0 && x == floor(x)) {
        *sign = 1;
        return log(dyn_mathx_fact[(int)x - 1]);
    }
#if defined(_WIN32)
    *sign = x < 0.0 && fmod(floor(x), 2.0) != 0.0 ? -1 : 1;
    return lgamma(x);
#else
    return lgamma_r(x, sign);
#endif
}

static JSValue dyn_mathx_lgamma(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    double x, val;
    int sign = 1;
    JSValue arr;

    (void)this_val;
    (void)argc;
    if (JS_ToFloat64(ctx, &x, argv[0]))
        return JS_EXCEPTION;
    val = dyn_lgamma_signed(x, &sign);

    arr = JS_NewArray(ctx);
    if (JS_IsException(arr))
        return JS_EXCEPTION;
    if (JS_DefinePropertyValueUint32(ctx, arr, 0, JS_NewFloat64(ctx, val),
            JS_PROP_C_W_E)
            < 0
        || JS_DefinePropertyValueUint32(ctx, arr, 1, JS_NewInt32(ctx, sign),
               JS_PROP_C_W_E)
            < 0) {
        JS_FreeValue(ctx, arr);
        return JS_EXCEPTION;
    }
    return arr;
}

static int dyn_mathx_exponent_arg(double e)
{
    if (e != e)
        return 0;
    if (e > 100000.0)
        return 100000;
    if (e < -100000.0)
        return -100000;
    return (int)e;
}

static JSValue dyn_mathx_scalbn(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    double x, dn;
    (void)this_val;
    (void)argc;
    if (JS_ToFloat64(ctx, &x, argv[0]) || JS_ToFloat64(ctx, &dn, argv[1]))
        return JS_EXCEPTION;
    return JS_NewFloat64(ctx, scalbn(x, dyn_mathx_exponent_arg(dn)));
}

static JSValue dyn_mathx_ldexp(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    double frac, dexp;
    (void)this_val;
    (void)argc;
    if (JS_ToFloat64(ctx, &frac, argv[0]) || JS_ToFloat64(ctx, &dexp, argv[1]))
        return JS_EXCEPTION;
    return JS_NewFloat64(ctx, ldexp(frac, dyn_mathx_exponent_arg(dexp)));
}

static JSValue dyn_mathx_ilogb(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    double x;
    (void)this_val;
    (void)argc;
    if (JS_ToFloat64(ctx, &x, argv[0]))
        return JS_EXCEPTION;
    if (isnan(x) || isinf(x))
        return JS_NewInt32(ctx, INT32_MAX);
    if (x == 0.0)
        return JS_NewInt32(ctx, INT32_MIN);
    return JS_NewInt32(ctx, (int32_t)ilogb(x));
}

static JSValue dyn_mathx_modf(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    double x, ip, fp;
    JSValue arr;

    (void)this_val;
    (void)argc;
    if (JS_ToFloat64(ctx, &x, argv[0]))
        return JS_EXCEPTION;
    if (isnan(x)) {
        ip = x;
        fp = x;
    } else if (isinf(x)) {
        ip = x;
        fp = copysign(DYN_NAN, x);
    } else {
        fp = modf(x, &ip);
    }

    arr = JS_NewArray(ctx);
    if (JS_IsException(arr))
        return JS_EXCEPTION;
    if (JS_DefinePropertyValueUint32(ctx, arr, 0, JS_NewFloat64(ctx, ip),
            JS_PROP_C_W_E)
            < 0
        || JS_DefinePropertyValueUint32(ctx, arr, 1, JS_NewFloat64(ctx, fp),
               JS_PROP_C_W_E)
            < 0) {
        JS_FreeValue(ctx, arr);
        return JS_EXCEPTION;
    }
    return arr;
}

static JSValue dyn_mathx_frexp(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    double x, frac;
    int exp;
    JSValue arr;

    (void)this_val;
    (void)argc;
    if (JS_ToFloat64(ctx, &x, argv[0]))
        return JS_EXCEPTION;
    if (isnan(x) || isinf(x) || x == 0.0) {
        frac = x;
        exp = 0;
    } else {
        frac = frexp(x, &exp);
    }

    arr = JS_NewArray(ctx);
    if (JS_IsException(arr))
        return JS_EXCEPTION;
    if (JS_DefinePropertyValueUint32(ctx, arr, 0, JS_NewFloat64(ctx, frac),
            JS_PROP_C_W_E)
            < 0
        || JS_DefinePropertyValueUint32(ctx, arr, 1, JS_NewInt32(ctx, exp),
               JS_PROP_C_W_E)
            < 0) {
        JS_FreeValue(ctx, arr);
        return JS_EXCEPTION;
    }
    return arr;
}

static JSValue dyn_mathx_is_inf(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    double x;
    int32_t sign = 0;
    int r;

    (void)this_val;
    if (JS_ToFloat64(ctx, &x, argv[0]))
        return JS_EXCEPTION;
    if (argc > 1 && !JS_IsUndefined(argv[1])) {
        if (JS_ToInt32(ctx, &sign, argv[1]))
            return JS_EXCEPTION;
    }
    if (sign > 0)
        r = isinf(x) && x > 0;
    else if (sign < 0)
        r = isinf(x) && x < 0;
    else
        r = isinf(x) != 0;
    return JS_NewBool(ctx, r);
}

static JSValue dyn_mathx_is_nan(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    double x;
    (void)this_val;
    (void)argc;
    if (JS_ToFloat64(ctx, &x, argv[0]))
        return JS_EXCEPTION;
    return JS_NewBool(ctx, isnan(x) != 0);
}

static JSValue dyn_mathx_signbit(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    double x;
    (void)this_val;
    (void)argc;
    if (JS_ToFloat64(ctx, &x, argv[0]))
        return JS_EXCEPTION;
    return JS_NewBool(ctx, signbit(x) != 0);
}

int dyn_mathx_to_int64(JSContext* ctx, JSValueConst v, int64_t* out)
{
    double d;

    if (JS_IsBigInt(ctx, v)) {
        int fits = dyn_mathx_bigint_fits_i64(ctx, v);
        if (fits < 0)
            return -1;
        if (!fits) {
            JS_ThrowRangeError(ctx,
                "dyna:mathx: BigInt is outside the 64-bit integer range");
            return -1;
        }
        return JS_ToBigInt64(ctx, out, v);
    }

    if (JS_ToFloat64(ctx, &d, v))
        return -1;
    if (!isfinite(d) || d != trunc(d)) {
        JS_ThrowRangeError(ctx, "dyna:mathx: expected an integer or a BigInt");
        return -1;
    }
    if (d < -9223372036854775808.0 || d >= 9223372036854775808.0) {
        JS_ThrowRangeError(ctx, "dyna:mathx: integer out of 64-bit range");
        return -1;
    }
    *out = (int64_t)d;
    return 0;
}

static uint64_t dyn_mathx_u64_abs(int64_t v)
{
    if (v == INT64_MIN)
        return (uint64_t)INT64_MAX + 1ULL;
    return v < 0 ? (uint64_t)(-v) : (uint64_t)v;
}

static uint64_t dyn_mathx_gcd_u64(uint64_t a, uint64_t b)
{
    while (b != 0) {
        uint64_t t = a % b;
        a = b;
        b = t;
    }
    return a;
}

static JSValue dyn_mathx_limbs_to_bigint(JSContext* ctx, const uint32_t* limb,
    size_t n)
{
    uint64_t stackw[8];
    uint64_t* w = stackw;
    size_t nw = 0, i, k;
    JSValue result;

    if (n > 16) {
        w = js_malloc(ctx, (n / 2 + 1) * sizeof(uint64_t));
        if (!w)
            return JS_EXCEPTION;
    }
    i = n;
    while (i > 0) {
        uint64_t mul, carry;
        if (i >= 2) {
            mul = (uint64_t)DYN_MATHX_LIMB_BASE * DYN_MATHX_LIMB_BASE;
            carry = (uint64_t)limb[i - 1] * DYN_MATHX_LIMB_BASE + limb[i - 2];
            i -= 2;
        } else {
            mul = DYN_MATHX_LIMB_BASE;
            carry = limb[i - 1];
            i -= 1;
        }
        for (k = 0; k < nw; k++) {
            unsigned __int128 t = (unsigned __int128)w[k] * mul + carry;
            w[k] = (uint64_t)t;
            carry = (uint64_t)(t >> 64);
        }
        if (carry)
            w[nw++] = carry;
    }
    {
        uint32_t stack32[16];
        uint32_t* w32 = nw <= 8 ? stack32 : js_malloc(ctx, nw * 2 * sizeof(uint32_t));
        if (!w32) {
            result = JS_EXCEPTION;
        } else {
            for (k = 0; k < nw; k++) {
                w32[2 * k] = (uint32_t)w[k];
                w32[2 * k + 1] = (uint32_t)(w[k] >> 32);
            }
            result = JS_NewBigUintFromWords32(ctx, w32, nw * 2);
            if (w32 != stack32)
                js_free(ctx, w32);
        }
    }
    if (w != stackw)
        js_free(ctx, w);
    return result;
}

static JSValue dyn_mathx_gcd(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    int64_t a, b;
    (void)this_val;
    (void)argc;
    if (dyn_mathx_to_int64(ctx, argv[0], &a) || dyn_mathx_to_int64(ctx, argv[1], &b))
        return JS_EXCEPTION;
    return JS_NewBigUint64(ctx, dyn_mathx_gcd_u64(dyn_mathx_u64_abs(a), dyn_mathx_u64_abs(b)));
}

static JSValue dyn_mathx_lcm(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    int64_t a, b;
    uint64_t ua, ub, g, reduced;
    unsigned __int128 prod;

    (void)this_val;
    (void)argc;
    if (dyn_mathx_to_int64(ctx, argv[0], &a) || dyn_mathx_to_int64(ctx, argv[1], &b))
        return JS_EXCEPTION;

    ua = dyn_mathx_u64_abs(a);
    ub = dyn_mathx_u64_abs(b);
    if (ua == 0 || ub == 0)
        return JS_NewBigInt64(ctx, 0);

    g = dyn_mathx_gcd_u64(ua, ub);
    reduced = ua / g;
    prod = (unsigned __int128)reduced * ub;

    if (prod <= UINT64_MAX)
        return JS_NewBigUint64(ctx, (uint64_t)prod);
    {
        uint32_t limb[5];
        size_t n = 0;
        unsigned __int128 v = prod;
        while (v > 0) {
            limb[n++] = (uint32_t)(v % DYN_MATHX_LIMB_BASE);
            v /= DYN_MATHX_LIMB_BASE;
        }
        return dyn_mathx_limbs_to_bigint(ctx, limb, n);
    }
}

static JSValue dyn_mathx_factorial(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    int64_t nv;
    int32_t n, i;
    uint64_t acc = 1;
    int overflowed = 0;
    DynBigDec big;
    JSValue result;

    (void)this_val;
    (void)argc;
    if (dyn_mathx_to_int64(ctx, argv[0], &nv))
        return JS_EXCEPTION;
    if (nv < 0)
        return JS_ThrowRangeError(ctx,
            "dyna:mathx: factorial requires a non-negative integer");
    if (nv > DYN_MATHX_FACTORIAL_MAX)
        return JS_ThrowRangeError(ctx,
            "dyna:mathx: factorial argument too large (max %d)",
            DYN_MATHX_FACTORIAL_MAX);
    n = (int32_t)nv;

    for (i = 2; i <= n; i++) {
        if (!overflowed) {
            if (acc > UINT64_MAX / (uint64_t)i) {
                if (dyn_bigdec_init(ctx, &big, acc))
                    return JS_EXCEPTION;
                overflowed = 1;
            } else {
                acc *= (uint64_t)i;
                continue;
            }
        }
        if (dyn_bigdec_mul_small(&big, (uint32_t)i)) {
            dyn_bigdec_free(&big);
            return JS_EXCEPTION;
        }
    }

    if (!overflowed)
        return JS_NewBigUint64(ctx, acc);

    result = dyn_mathx_limbs_to_bigint(ctx, big.limb, big.len);
    dyn_bigdec_free(&big);
    return result;
}

static JSValue dyn_mathx_abs(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    uint64_t mag;
    int neg;
    (void)this_val;
    (void)argc;
    if (!JS_IsBigInt(ctx, argv[0]))
        return JS_ThrowTypeError(ctx, "dyna:mathx: abs expects a BigInt");
    if (dyn_mathx_bigint_mag_u64(ctx, argv[0], &mag, &neg))
        return JS_EXCEPTION;
    return JS_NewBigUint64(ctx, mag);
}

static JSValue dyn_mathx_bit_len(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    uint64_t mag;
    int neg;
    (void)this_val;
    (void)argc;
    if (!JS_IsBigInt(ctx, argv[0]))
        return JS_ThrowTypeError(ctx, "dyna:mathx: bitLen expects a BigInt");
    if (dyn_mathx_bigint_mag_u64(ctx, argv[0], &mag, &neg))
        return JS_EXCEPTION;
    if (mag == 0)
        return JS_NewInt32(ctx, 0);
    return JS_NewInt32(ctx, 64 - __builtin_clzll(mag));
}

static JSValue dyn_mathx_popcount(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    uint64_t mag;
    int neg;
    (void)this_val;
    (void)argc;
    if (!JS_IsBigInt(ctx, argv[0]))
        return JS_ThrowTypeError(ctx, "dyna:mathx: popcount expects a BigInt");
    if (dyn_mathx_bigint_mag_u64(ctx, argv[0], &mag, &neg))
        return JS_EXCEPTION;
    return JS_NewInt32(ctx, __builtin_popcountll(mag));
}

static double dyn_mathx_mod_floored(double x, double y)
{
    double r;
    if (y == 0.0)
        return x;
    r = fma(-floor(x / y), y, x);
    if (y > 0.0 ? (r >= 0.0 && r < y) : (r <= 0.0 && r > y))
        return r;
    r = fmod(x, y);
    if (r != 0.0 && ((r < 0.0) != (y < 0.0)))
        r += y;
    return r;
}

DYN_MATHX_BINARY(mod, dyn_mathx_mod_floored(x, y))

DYN_MATHX_BINARY(rem, fmod(x, y))

DYN_MATHX_UNARY(fix, trunc(x))

DYN_MATHX_UNARY(sign, (x > 0.0) ? 1.0 : (x < 0.0) ? -1.0
                                                  : x)

DYN_MATHX_UNARY(deg2rad, x * (3.14159265358979323846 / 180.0))

DYN_MATHX_UNARY(rad2deg, x * (180.0 / 3.14159265358979323846))

DYN_MATHX_UNARY(pow2, exp2(x))

DYN_MATHX_BINARY(nthroot,
    (y == 3.0)       ? mathx_cbrt_exact(x)
        : (y == 1.0) ? x
        : (x < 0.0 && y == floor(y) && fmod(y, 2.0) != 0.0)
        ? -pow(-x, 1.0 / y)
        : pow(x, 1.0 / y))

DYN_MATHX_UNARY(nextpow2,
    (x == 0.0 || isnan(x) || isinf(x)) ? (isnan(x) || isinf(x) ? x : 0.0)
                                       : ceil(log2(fabs(x))))

static JSValue dyn_mathx_eps(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    double x = 1.0, a, na;
    (void)this_val;
    if (argc >= 1 && !JS_IsUndefined(argv[0]) && JS_ToFloat64(ctx, &x, argv[0]))
        return JS_EXCEPTION;
    if (isnan(x) || isinf(x))
        return JS_NewFloat64(ctx, DYN_NAN);
    a = fabs(x);
    na = nextafter(a, DYN_INFINITY);
    if (isinf(na)) {
        return JS_NewFloat64(ctx, ldexp(1.0, 1023 - 52));
    }
    return JS_NewFloat64(ctx, na - a);
}

static double dyn_beta_ln(double a, double b, int* sign)
{
    int sa, sb, sab;
    double la, lb, lab;
    if (a <= 0.0 && a == floor(a)) {
        *sign = 1;
        return DYN_INFINITY;
    }
    if (b <= 0.0 && b == floor(b)) {
        *sign = 1;
        return DYN_INFINITY;
    }
    if (a < b) {
        double t = a;
        a = b;
        b = t;
    }
    if (a >= 10.0 && b > 0.0 && isfinite(a)) {
        lb = dyn_lgamma_signed(b, &sb);
        *sign = sb;
        return lb - dyn_lgamma_rise(a, b);
    }
    la = dyn_lgamma_signed(a, &sa);
    lb = dyn_lgamma_signed(b, &sb);
    lab = dyn_lgamma_signed(a + b, &sab);
    *sign = sa * sb * sab;
    return la + lb - lab;
}

static double dyn_beta(double a, double b)
{
    int sign;
    double ln = dyn_beta_ln(a, b, &sign);
    return (double)sign * exp(ln);
}

static double dyn_betaln(double a, double b)
{
    int sign;
    return dyn_beta_ln(a, b, &sign);
}

DYN_MATHX_BINARY(beta, dyn_beta(x, y))

DYN_MATHX_BINARY(betaln, dyn_betaln(x, y))

static double dyn_mathx_lgamma_r(double x)
{
    int sign;
    return dyn_lgamma_signed(x, &sign);
}

DYN_MATHX_UNARY(gammaln, dyn_mathx_lgamma_r(x))

DYN_MATHX_UNARY(psi, dyn_digamma(x))

DYN_MATHX_UNARY(erfinv, dyn_erfinv(x))

DYN_MATHX_UNARY(erfcinv, dyn_erfcinv(x))

DYN_MATHX_UNARY(erfcx, (x < 25.0) ? exp(x * x) * erfc(x) : (1.0 / (x * 1.7724538509055160273)) * (1.0 - 0.5 / (x * x) + 0.75 / (x * x * x * x) - 1.875 / (x * x * x * x * x * x) + 6.5625 / (x * x * x * x * x * x * x * x) - 29.53125 / (x * x * x * x * x * x * x * x * x * x)))

static double dyn_expint(double x)
{
    if (isnan(x))
        return DYN_NAN;
    if (x < 0.0)
        return DYN_NAN;
    if (x == 0.0)
        return DYN_INFINITY;
    if (x <= 1.0) {
        double s = -0.57721566490153286061 - log(x), term = 1.0;
        for (int k = 1; k <= 60; k++) {
            term *= -x / k;
            s -= term / k;
            if (fabs(term / k) < 1e-18 * fabs(s))
                break;
        }
        return s;
    } else {
        double b = x + 1.0, c = 1e300, d = 1.0 / b, h = d;
        for (int i = 1; i <= 200; i++) {
            double a = -(double)i * (double)i;
            b += 2.0;
            d = 1.0 / (a * d + b);
            c = b + a / c;
            {
                double del = c * d;
                h *= del;
                if (fabs(del - 1.0) < 1e-16)
                    break;
            }
        }
        return h * exp(-x);
    }
}

DYN_MATHX_UNARY(expint, dyn_expint(x))

static JSValue dyn_mathx_realmin(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    (void)this_val;
    (void)argc;
    (void)argv;
    return JS_NewFloat64(ctx, DBL_MIN);
}

static JSValue dyn_mathx_realmax(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    (void)this_val;
    (void)argc;
    (void)argv;
    return JS_NewFloat64(ctx, DBL_MAX);
}

static JSValue dyn_mathx_flintmax(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    (void)this_val;
    (void)argc;
    (void)argv;
    return JS_NewFloat64(ctx, 9007199254740992.0);
}

static JSValue dyn_mathx_arr(JSContext* ctx, const double* v, size_t n)
{
    JSValue a, *tab;
    size_t i;
    if (n > INT32_MAX)
        return JS_ThrowRangeError(ctx, "array too long");
    tab = (JSValue*)js_malloc(ctx, (n ? n : 1) * sizeof(JSValue));
    if (!tab)
        return JS_EXCEPTION;
    for (i = 0; i < n; i++)
        tab[i] = JS_NewFloat64(ctx, v[i]);
    a = JS_NewArrayFrom(ctx, (int)n, tab);
    js_free(ctx, tab);
    return a;
}

static uint64_t dyn_isqrt_u64(uint64_t x)
{
    uint64_t r = (uint64_t)sqrtl((long double)x);

    while (r * r > x)
        r--;
    while ((r + 1) * (r + 1) <= x)
        r++;
    return r;
}

static JSValue dyn_mathx_factor(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    double xd;
    uint64_t x, d, limit;
    double out[64] = { 0 };
    size_t n = 0;
    (void)this_val;
    (void)argc;
    if (JS_ToFloat64(ctx, &xd, argv[0]))
        return JS_EXCEPTION;
    if (!(xd >= 1.0) || xd != floor(xd) || xd > 9007199254740992.0)
        return JS_ThrowRangeError(ctx, "factor: expects a positive integer <= 2^53");
    x = (uint64_t)xd;
    if (x == 1)
        return dyn_mathx_arr(ctx, out, 0);
    while ((x & 1) == 0) {
        out[n++] = 2.0;
        x >>= 1;
    }
    limit = dyn_isqrt_u64(x);
    for (d = 3; d <= limit; d += 2) {
        if (x % d == 0) {
            do {
                out[n++] = (double)d;
                x /= d;
            } while (x % d == 0);
            limit = dyn_isqrt_u64(x);
        }
    }
    if (x > 1)
        out[n++] = (double)x;
    return dyn_mathx_arr(ctx, out, n);
}

static JSValue dyn_mathx_primes(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    double nd;
    uint32_t n, i, j, count = 0;
    uint8_t* sieve;
    double* out;
    JSValue r;
    (void)this_val;
    (void)argc;
    if (JS_ToFloat64(ctx, &nd, argv[0]))
        return JS_EXCEPTION;
    if (isnan(nd) || nd < 2.0)
        return dyn_mathx_arr(ctx, NULL, 0);
    if (nd > 50000000.0)
        return JS_ThrowRangeError(ctx, "primes: limit too large (max 5e7)");
    n = (uint32_t)nd;
    sieve = (uint8_t*)dyn_nat_calloc(n + 1, 1);
    if (!sieve)
        return JS_ThrowOutOfMemory(ctx);
    for (i = 2; (uint64_t)i * i <= n; i++)
        if (!sieve[i])
            for (j = i * i; j <= n; j += i)
                sieve[j] = 1;
    for (i = 2; i <= n; i++)
        if (!sieve[i])
            count++;
    out = (double*)dyn_nat_malloc((count + 1) * sizeof(double));
    if (!out) {
        dyn_nat_free(sieve);
        return JS_ThrowOutOfMemory(ctx);
    }
    count = 0;
    for (i = 2; i <= n; i++) {
        out[count] = (double)i;
        count += !sieve[i];
    }
    dyn_nat_free(sieve);
    r = dyn_mathx_arr(ctx, out, count);
    dyn_nat_free(out);
    return r;
}

static JSValue dyn_mathx_nchoosek(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    double nd, kd, r = 1.0;
    uint64_t n, k, i;
    (void)this_val;
    (void)argc;
    if (JS_ToFloat64(ctx, &nd, argv[0]) || JS_ToFloat64(ctx, &kd, argv[1]))
        return JS_EXCEPTION;
    if (isnan(nd) || isnan(kd) || nd < 0.0 || kd < 0.0 || nd != floor(nd) || kd != floor(kd) || nd >= 18446744073709551616.0 || kd >= 18446744073709551616.0)
        return JS_ThrowRangeError(ctx, "nchoosek: expects non-negative integers");
    if (kd > nd)
        return JS_NewFloat64(ctx, 0.0);
    n = (uint64_t)nd;
    k = (uint64_t)kd;
    if (k > n - k)
        k = n - k;
    for (i = 1; i <= k; i++) {
        r = r * (double)(n - k + i) / (double)i;
        if (!isfinite(r))
            break;
    }
    return JS_NewFloat64(ctx, floor(r + 0.5));
}

static JSValue dyn_mathx_rat(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    double x, tol = 1e-6, v, out[2];
    double h0 = 0, h1 = 1, k0 = 1, k1 = 0;
    int i;
    (void)this_val;
    if (JS_ToFloat64(ctx, &x, argv[0]))
        return JS_EXCEPTION;
    if (argc >= 2 && !JS_IsUndefined(argv[1]) && JS_ToFloat64(ctx, &tol, argv[1]))
        return JS_EXCEPTION;
    if (isnan(x) || isinf(x)) {
        out[0] = x;
        out[1] = isnan(x) ? DYN_NAN : 0.0;
        return dyn_mathx_arr(ctx, out, 2);
    }
    v = x;
    for (i = 0; i < 64; i++) {
        double a = floor(v), h2 = a * h1 + h0, k2 = a * k1 + k0;
        h0 = h1;
        h1 = h2;
        k0 = k1;
        k1 = k2;
        if (k1 != 0.0 && fabs(x - h1 / k1) <= fabs(x) * tol)
            break;
        if (v == a)
            break;
        v = 1.0 / (v - a);
        if (isinf(v))
            break;
    }
    if (k1 < 0) {
        h1 = -h1;
        k1 = -k1;
    }
    out[0] = h1;
    out[1] = k1;
    return dyn_mathx_arr(ctx, out, 2);
}

static JSValue dyn_mathx_linspace(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int logspace)
{
    double a, b, *v;
    int32_t n = 100, i;
    JSValue r;
    (void)this_val;
    if (JS_ToFloat64(ctx, &a, argv[0]) || JS_ToFloat64(ctx, &b, argv[1]))
        return JS_EXCEPTION;
    if (argc >= 3 && !JS_IsUndefined(argv[2]) && JS_ToInt32(ctx, &n, argv[2]))
        return JS_EXCEPTION;
    if (n < 0 || n > 1000000)
        return JS_ThrowRangeError(ctx, "linspace: n must be 0..1000000");
    v = (double*)dyn_nat_malloc((n ? n : 1) * sizeof(double));
    if (!v)
        return JS_ThrowOutOfMemory(ctx);
    for (i = 0; i < n; i++) {
        double t = (n == 1) ? b : a + (b - a) * ((double)i / (double)(n - 1));
        if (i == n - 1)
            t = b;
        v[i] = logspace ? pow(10.0, t) : t;
    }
    r = dyn_mathx_arr(ctx, v, (size_t)n);
    dyn_nat_free(v);
    return r;
}

static JSValue dyn_mathx_scan(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    uint32_t len, i;
    double *v = NULL, acc;
    JSValue r;
    mxst_src s;
    (void)this_val;
    (void)argc;
    if (mxst_src_of(ctx, argv[0], &s))
        return JS_EXCEPTION;
    len = s.len;
    v = (double*)dyn_nat_malloc((len ? len : 1) * sizeof(double));
    if (!v)
        return JS_ThrowOutOfMemory(ctx);
    if (s.p) {
        if (len)
            memcpy(v, s.p, (size_t)len * sizeof(double));
    } else {
        for (i = 0; i < len; i++) {
            if (mxst_elem(ctx, argv[0], i, &v[i])) {
                dyn_nat_free(v);
                return JS_EXCEPTION;
            }
        }
    }
    if (magic == 2) {
        for (i = 0; i + 1 < len; i++)
            v[i] = v[i + 1] - v[i];
        r = dyn_mathx_arr(ctx, v, len ? len - 1 : 0);
    } else {
        acc = (magic == 0) ? 0.0 : 1.0;
        for (i = 0; i < len; i++) {
            acc = (magic == 0) ? acc + v[i] : acc * v[i];
            v[i] = acc;
        }
        r = dyn_mathx_arr(ctx, v, len);
    }
    dyn_nat_free(v);
    return r;
}

_Atomic JSClassID mxst_cid_f64;

static const JSCFunctionListEntry dyn_mathx_stats_funcs[] = {
    JS_CFUNC_DEF("sum", 1, dyn_stats_sum),
    JS_CFUNC_DEF("mean", 1, dyn_stats_mean),
    JS_CFUNC_MAGIC_DEF("variance", 1, dyn_stats_var, 0),
    JS_CFUNC_MAGIC_DEF("stddev", 1, dyn_stats_var, 1),
    JS_CFUNC_MAGIC_DEF("median", 1, dyn_stats_quantile, 1),
    JS_CFUNC_MAGIC_DEF("quantile", 2, dyn_stats_quantile, 0),
    JS_CFUNC_MAGIC_DEF("min", 1, dyn_stats_minmax, 0),
    JS_CFUNC_MAGIC_DEF("max", 1, dyn_stats_minmax, 1),
    JS_CFUNC_DEF("cov", 2, dyn_stats_cov),
    JS_CFUNC_DEF("corr", 2, dyn_stats_corr),
};

BITS_COUNT_GROUP(8)

BITS_COUNT_GROUP(16)

BITS_COUNT_GROUP(32)

BITS_COUNT_GROUP(64)

BITS_REVERSE_SMALL(8)

BITS_REVERSE_SMALL(16)

BITS_REVERSE_SMALL(32)

BITS_ROTATE_SMALL(8)

BITS_ROTATE_SMALL(16)

BITS_ROTATE_SMALL(32)

BITS_ROTATE_RIGHT_SMALL(8)

BITS_ROTATE_RIGHT_SMALL(16)

BITS_ROTATE_RIGHT_SMALL(32)

static JSValue dyn_mathx_gammainc(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    double x, a;
    int upper = 0;
    (void)this_val;
    if (JS_ToFloat64(ctx, &x, argv[0]) || JS_ToFloat64(ctx, &a, argv[1]))
        return JS_EXCEPTION;
    if (argc > 2 && !JS_IsUndefined(argv[2])) {
        const char* tail = JS_ToCString(ctx, argv[2]);
        if (!tail)
            return JS_EXCEPTION;
        upper = strcmp(tail, "upper") == 0;
        int bad = strcmp(tail, "lower") != 0 && !upper;
        JS_FreeCString(ctx, tail);
        if (bad)
            return JS_ThrowRangeError(ctx,
                "gammainc: tail must be \"lower\" or \"upper\"");
    }
    return JS_NewFloat64(ctx, upper ? dyn_gammainc_q(a, x) : dyn_gammainc_p(a, x));
}

static JSValue dyn_mathx_gammaincinv(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    double p, a;
    (void)this_val;
    (void)argc;
    if (JS_ToFloat64(ctx, &p, argv[0]) || JS_ToFloat64(ctx, &a, argv[1]))
        return JS_EXCEPTION;
    return JS_NewFloat64(ctx, dyn_gammaincinv(a, p));
}

static JSValue dyn_mathx_betainc(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    double x, a, b;
    (void)this_val;
    (void)argc;
    if (JS_ToFloat64(ctx, &x, argv[0]) || JS_ToFloat64(ctx, &a, argv[1]) || JS_ToFloat64(ctx, &b, argv[2]))
        return JS_EXCEPTION;
    return JS_NewFloat64(ctx, dyn_betainc(x, a, b));
}

static JSValue dyn_mathx_betaincinv(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    double p, a, b;
    (void)this_val;
    (void)argc;
    if (JS_ToFloat64(ctx, &p, argv[0]) || JS_ToFloat64(ctx, &a, argv[1]) || JS_ToFloat64(ctx, &b, argv[2]))
        return JS_EXCEPTION;
    return JS_NewFloat64(ctx, dyn_betaincinv(p, a, b));
}

static JSValue dyn_mathx_bessel(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    double nu, x;
    (void)this_val;
    (void)argc;
    if (JS_ToFloat64(ctx, &nu, argv[0]) || JS_ToFloat64(ctx, &x, argv[1]))
        return JS_EXCEPTION;
    switch (magic) {
    case 0:
        if (!isfinite(nu) || nu != floor(nu) || nu < (double)INT_MIN || nu > (double)INT_MAX)
            return JS_ThrowRangeError(ctx,
                "besselj: order must be an integer within int32 range");
        if (!dyn_bessel_order_ok((int)nu, x))
            return JS_ThrowRangeError(ctx, "besselj: |order| above 16777216 "
                                           "would run libm's O(n) recurrence for seconds");
        return JS_NewFloat64(ctx, dyn_besselj((int)nu, x));
    case 1:
        if (!isfinite(nu) || nu != floor(nu) || nu < (double)INT_MIN || nu > (double)INT_MAX)
            return JS_ThrowRangeError(ctx,
                "bessely: order must be an integer within int32 range");
        if (fabs(nu) > 16777216.0)
            return JS_ThrowRangeError(ctx, "bessely: |order| above 16777216 "
                                           "would run libm's O(n) recurrence for seconds");
        return JS_NewFloat64(ctx, dyn_bessely((int)nu, x));
    case 2:
        return JS_NewFloat64(ctx, dyn_besseli_nu(nu, x));
    case 3:
        return JS_NewFloat64(ctx, dyn_besselk_nu(nu, x));
    case 4:
        return JS_NewFloat64(ctx, dyn_besseli_scaled(nu, x));
    default:
        return JS_NewFloat64(ctx, dyn_besselk_scaled(nu, x));
    }
}

static JSValue dyn_mathx_besselh(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    double nu, x, re, im;
    int kind = 1;
    JSValue out;
    (void)this_val;
    if (JS_ToFloat64(ctx, &nu, argv[0]) || JS_ToFloat64(ctx, &x, argv[1]))
        return JS_EXCEPTION;
    if (argc > 2 && !JS_IsUndefined(argv[2])) {
        double k;
        if (JS_ToFloat64(ctx, &k, argv[2]))
            return JS_EXCEPTION;
        if (!(k >= -2147483648.0 && k <= 2147483647.0) || k != floor(k))
            return JS_ThrowRangeError(ctx, "besselh: kind must be 1 or 2");
        kind = (int)k;
    }
    if (nu != floor(nu) || !(nu >= -2147483648.0 && nu <= 2147483647.0))
        return JS_ThrowRangeError(ctx, "besselh: order must be an integer");
    if (!dyn_bessel_order_ok((int)nu, x))
        return JS_ThrowRangeError(ctx, "besselh: |order| above 16777216 "
                                       "would run libm's O(n) recurrence for seconds");
    if (dyn_besselh(kind, (int)nu, x, &re, &im) < 0)
        return JS_ThrowRangeError(ctx, "besselh: kind must be 1 or 2");
    out = JS_NewArray(ctx);
    if (JS_IsException(out))
        return out;
    JS_DefinePropertyValueUint32(ctx, out, 0, JS_NewFloat64(ctx, re), JS_PROP_C_W_E);
    JS_DefinePropertyValueUint32(ctx, out, 1, JS_NewFloat64(ctx, im), JS_PROP_C_W_E);
    return out;
}

static JSValue dyn_mathx_ellipke(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    double m;
    JSValue out;
    (void)this_val;
    (void)argc;
    if (JS_ToFloat64(ctx, &m, argv[0]))
        return JS_EXCEPTION;
    out = JS_NewArray(ctx);
    if (JS_IsException(out))
        return out;
    JS_DefinePropertyValueUint32(ctx, out, 0,
        JS_NewFloat64(ctx, dyn_ellipk(m)), JS_PROP_C_W_E);
    JS_DefinePropertyValueUint32(ctx, out, 1,
        JS_NewFloat64(ctx, dyn_ellipe(m)), JS_PROP_C_W_E);
    return out;
}

static JSValue dyn_mathx_ellipj(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    double u, m, sn, cn, dn;
    JSValue out;
    (void)this_val;
    (void)argc;
    if (JS_ToFloat64(ctx, &u, argv[0]) || JS_ToFloat64(ctx, &m, argv[1]))
        return JS_EXCEPTION;
    if (dyn_ellipj(u, m, &sn, &cn, &dn) < 0)
        return JS_ThrowRangeError(ctx, "ellipj: m must be in [0, 1]");
    out = JS_NewObject(ctx);
    if (JS_IsException(out))
        return out;
    JS_DefinePropertyValueStr(ctx, out, "sn", JS_NewFloat64(ctx, sn), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, out, "cn", JS_NewFloat64(ctx, cn), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, out, "dn", JS_NewFloat64(ctx, dn), JS_PROP_C_W_E);
    return out;
}

static JSValue dyn_mathx_legendre(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    double dn, x;
    int n, m;
    JSValue out;
    (void)this_val;
    (void)argc;
    if (JS_ToFloat64(ctx, &dn, argv[0]) || JS_ToFloat64(ctx, &x, argv[1]))
        return JS_EXCEPTION;
    if (dn != floor(dn) || dn < 0 || dn > 1024)
        return JS_ThrowRangeError(ctx, "legendre: degree must be an integer in [0, 1024]");
    n = (int)dn;
    out = JS_NewArray(ctx);
    if (JS_IsException(out))
        return out;
    for (m = 0; m <= n; m++)
        JS_DefinePropertyValueUint32(ctx, out, (uint32_t)m,
            JS_NewFloat64(ctx, dyn_legendre(n, m, x)),
            JS_PROP_C_W_E);
    return out;
}

static JSValue dyn_mathx_legendre_p(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    double dn, dm, x;
    (void)this_val;
    (void)argc;
    if (JS_ToFloat64(ctx, &dn, argv[0]) || JS_ToFloat64(ctx, &dm, argv[1]) || JS_ToFloat64(ctx, &x, argv[2]))
        return JS_EXCEPTION;
    if (dn != floor(dn) || dm != floor(dm) || dn < 0 || dm < 0)
        return JS_ThrowRangeError(ctx, "legendreP: degree and order must be integers");
    if (dn > 150.0)
        return JS_ThrowRangeError(ctx, "legendreP: degree exceeds 150");
    if (dm > dn)
        return JS_NewFloat64(ctx, DYN_NAN);
    return JS_NewFloat64(ctx, dyn_legendre((int)dn, (int)dm, x));
}

static JSValue dyn_mathx_polygamma(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    double dn, x;
    (void)this_val;
    (void)argc;
    if (JS_ToFloat64(ctx, &dn, argv[0]) || JS_ToFloat64(ctx, &x, argv[1]))
        return JS_EXCEPTION;
    if (dn != floor(dn) || dn < 0 || dn > 64)
        return JS_ThrowRangeError(ctx, "polygamma: order must be an integer in [0, 64]");
    return JS_NewFloat64(ctx, dyn_polygamma((int)dn, x));
}

static JSValue dyn_mathx_airy(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    double x, ai, aip, bi, bip;
    JSValue out;
    (void)this_val;
    (void)argc;
    if (JS_ToFloat64(ctx, &x, argv[0]))
        return JS_EXCEPTION;
    dyn_airy(x, &ai, &aip, &bi, &bip);
    out = JS_NewObject(ctx);
    if (JS_IsException(out))
        return out;
    JS_DefinePropertyValueStr(ctx, out, "ai", JS_NewFloat64(ctx, ai), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, out, "aip", JS_NewFloat64(ctx, aip), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, out, "bi", JS_NewFloat64(ctx, bi), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, out, "bip", JS_NewFloat64(ctx, bip), JS_PROP_C_W_E);
    return out;
}

static JSValue dyn_mathx_idivide(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    double a, b, q;
    (void)this_val;
    if (JS_ToFloat64(ctx, &a, argv[0]) || JS_ToFloat64(ctx, &b, argv[1]))
        return JS_EXCEPTION;
    q = a / b;
    if (argc > 2 && !JS_IsUndefined(argv[2])) {
        const char* mode = JS_ToCString(ctx, argv[2]);
        if (!mode)
            return JS_EXCEPTION;
        if (strcmp(mode, "floor") == 0)
            q = floor(q);
        else if (strcmp(mode, "ceil") == 0)
            q = ceil(q);
        else if (strcmp(mode, "round") == 0)
            q = dyn_mathx_rte(q);
        else if (strcmp(mode, "fix") == 0)
            q = trunc(q);
        else {
            JS_FreeCString(ctx, mode);
            return JS_ThrowRangeError(ctx,
                "idivide: mode must be \"fix\", \"floor\", \"ceil\" or \"round\"");
        }
        JS_FreeCString(ctx, mode);
        return JS_NewFloat64(ctx, q);
    }
    return JS_NewFloat64(ctx, trunc(q));
}

static JSValue dyn_mathx_perms(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    double* v = NULL;
    int n = 0, i;
    uint32_t out_i = 0;
    JSValue out;
    int* idx = NULL;
    (void)this_val;
    (void)argc;

    if (!JS_IsArray(ctx, argv[0]))
        return JS_ThrowTypeError(ctx, "perms(v): v must be an array");
    {
        JSValue lv = JS_GetPropertyStr(ctx, argv[0], "length");
        int64_t len;
        if (JS_IsException(lv))
            return JS_EXCEPTION;
        if (JS_ToInt64(ctx, &len, lv)) {
            JS_FreeValue(ctx, lv);
            return JS_EXCEPTION;
        }
        JS_FreeValue(ctx, lv);
        if (len < 0 || len > 8)
            return JS_ThrowRangeError(ctx,
                "perms: at most 8 elements (9! rows is past useful)");
        n = (int)len;
    }
    v = (double*)dyn_nat_malloc((size_t)(n ? n : 1) * sizeof(double));
    idx = (int*)dyn_nat_malloc((size_t)(n ? n : 1) * sizeof(int));
    if (!v || !idx) {
        dyn_nat_free(v);
        dyn_nat_free(idx);
        return JS_ThrowOutOfMemory(ctx);
    }
    for (i = 0; i < n; i++) {
        JSValue e = JS_GetPropertyUint32(ctx, argv[0], (uint32_t)i);
        if (JS_IsException(e) || JS_ToFloat64(ctx, &v[i], e)) {
            JS_FreeValue(ctx, e);
            dyn_nat_free(v);
            dyn_nat_free(idx);
            return JS_EXCEPTION;
        }
        JS_FreeValue(ctx, e);
        idx[i] = n - 1 - i;
    }
    out = JS_NewArray(ctx);
    if (JS_IsException(out)) {
        dyn_nat_free(v);
        dyn_nat_free(idx);
        return out;
    }

    for (;;) {
        JSValue row = JS_NewArray(ctx);
        int a, b;
        if (JS_IsException(row)) {
            JS_FreeValue(ctx, out);
            dyn_nat_free(v);
            dyn_nat_free(idx);
            return row;
        }
        for (i = 0; i < n; i++)
            JS_DefinePropertyValueUint32(ctx, row, (uint32_t)i,
                JS_NewFloat64(ctx, v[idx[i]]), JS_PROP_C_W_E);
        JS_DefinePropertyValueUint32(ctx, out, out_i++, row, JS_PROP_C_W_E);
        if (n < 2)
            break;
        for (a = n - 2; a >= 0 && idx[a] <= idx[a + 1]; a--) { }
        if (a < 0)
            break;
        for (b = n - 1; idx[b] >= idx[a]; b--) { }
        {
            int t = idx[a];
            idx[a] = idx[b];
            idx[b] = t;
        }
        for (a = a + 1, b = n - 1; a < b; a++, b--) {
            int t = idx[a];
            idx[a] = idx[b];
            idx[b] = t;
        }
    }
    dyn_nat_free(v);
    dyn_nat_free(idx);
    return out;
}

static const JSCFunctionListEntry dyn_mathx_bits_funcs[] = {
    JS_PROP_INT32_DEF("uintSize", 64, JS_PROP_C_W_E),
    JS_CFUNC_DEF("leadingZeros8", 1, mxbits_leading_zeros8),
    JS_CFUNC_DEF("leadingZeros16", 1, mxbits_leading_zeros16),
    JS_CFUNC_DEF("leadingZeros32", 1, mxbits_leading_zeros32),
    JS_CFUNC_DEF("leadingZeros64", 1, mxbits_leading_zeros64),
    JS_CFUNC_DEF("trailingZeros8", 1, mxbits_trailing_zeros8),
    JS_CFUNC_DEF("trailingZeros16", 1, mxbits_trailing_zeros16),
    JS_CFUNC_DEF("trailingZeros32", 1, mxbits_trailing_zeros32),
    JS_CFUNC_DEF("trailingZeros64", 1, mxbits_trailing_zeros64),
    JS_CFUNC_DEF("onesCount8", 1, mxbits_ones_count8),
    JS_CFUNC_DEF("onesCount16", 1, mxbits_ones_count16),
    JS_CFUNC_DEF("onesCount32", 1, mxbits_ones_count32),
    JS_CFUNC_DEF("onesCount64", 1, mxbits_ones_count64),
    JS_CFUNC_DEF("len8", 1, mxbits_len8),
    JS_CFUNC_DEF("len16", 1, mxbits_len16),
    JS_CFUNC_DEF("len32", 1, mxbits_len32),
    JS_CFUNC_DEF("len64", 1, mxbits_len64),
    JS_CFUNC_DEF("reverse8", 1, mxbits_reverse8),
    JS_CFUNC_DEF("reverse16", 1, mxbits_reverse16),
    JS_CFUNC_DEF("reverse32", 1, mxbits_reverse32),
    JS_CFUNC_DEF("reverse64", 1, mxbits_reverse64),
    JS_CFUNC_DEF("reverseBytes16", 1, mxbits_reverse_bytes16),
    JS_CFUNC_DEF("reverseBytes32", 1, mxbits_reverse_bytes32),
    JS_CFUNC_DEF("reverseBytes64", 1, mxbits_reverse_bytes64),
    JS_CFUNC_DEF("rotateLeft8", 2, mxbits_rotate_left8),
    JS_CFUNC_DEF("rotateLeft16", 2, mxbits_rotate_left16),
    JS_CFUNC_DEF("rotateLeft32", 2, mxbits_rotate_left32),
    JS_CFUNC_DEF("rotateLeft64", 2, mxbits_rotate_left64),
    JS_CFUNC_DEF("rotateRight8", 2, mxbits_rotate_right8),
    JS_CFUNC_DEF("rotateRight16", 2, mxbits_rotate_right16),
    JS_CFUNC_DEF("rotateRight32", 2, mxbits_rotate_right32),
    JS_CFUNC_DEF("rotateRight64", 2, mxbits_rotate_right64),
    JS_CFUNC_DEF("add32", 3, mxbits_add32),
    JS_CFUNC_DEF("add64", 3, mxbits_add64),
    JS_CFUNC_DEF("sub32", 3, mxbits_sub32),
    JS_CFUNC_DEF("sub64", 3, mxbits_sub64),
    JS_CFUNC_DEF("mul32", 2, mxbits_mul32),
    JS_CFUNC_DEF("mul64", 2, mxbits_mul64),
    JS_CFUNC_DEF("div32", 3, mxbits_div32),
    JS_CFUNC_DEF("div64", 3, mxbits_div64),
    JS_CFUNC_DEF("rem32", 3, mxbits_rem32),
    JS_CFUNC_DEF("rem64", 3, mxbits_rem64),
};

static JSValue dyn_mathx_solve_spd(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    double *A, *b, *L = NULL, *x = NULL, pivot = 0.0;
    size_t na, nb, n, i, j, k;
    JSValue ab, out, ta[3];
    (void)this_val;
    if (argc < 2)
        return JS_ThrowTypeError(ctx, "solveSPD(A, b): A and b are required");
    if (mx_f64_view(ctx, argv[0], "A", &A, &na) || mx_f64_view(ctx, argv[1], "b", &b, &nb))
        return JS_EXCEPTION;
    n = nb;
    if (n == 0 || n > 4096 || na != n * n)
        return JS_ThrowRangeError(ctx, "solveSPD: A must hold n*n values for b of length n (1 <= n <= 4096); got %zu and %zu", na, nb);
    L = (double*)calloc(n * n, sizeof(double));
    x = (double*)malloc(n * sizeof(double));
    if (!L || !x) {
        free(L);
        free(x);
        return JS_ThrowOutOfMemory(ctx);
    }
    switch (mx_cholesky(A, L, n, &i, &j, &pivot)) {
    case 1:
        free(L);
        free(x);
        return JS_ThrowRangeError(ctx, "solveSPD: A must be finite and symmetric (A[%zu][%zu] vs A[%zu][%zu])", i, j, j, i);
    case 2:
        free(L);
        free(x);
        return JS_ThrowRangeError(ctx, "solveSPD: A is not positive definite (pivot %zu is %g)", i, pivot);
    default:
        break;
    }
    for (i = 0; i < n; i++) {
        double sum = b[i];
        for (k = 0; k < i; k++)
            sum -= L[i * n + k] * x[k];
        x[i] = sum / L[i * n + i];
    }
    for (i = n; i-- > 0;) {
        double sum = x[i];
        for (k = i + 1; k < n; k++)
            sum -= L[k * n + i] * x[k];
        x[i] = sum / L[i * n + i];
    }
    free(L);
    ab = JS_NewArrayBufferCopy(ctx, (const uint8_t*)x, n * sizeof(double));
    free(x);
    if (JS_IsException(ab))
        return ab;
    ta[0] = ab;
    ta[1] = JS_NewInt32(ctx, 0);
    ta[2] = JS_NewInt64(ctx, (int64_t)n);
    out = JS_NewTypedArray(ctx, 3, ta, JS_TYPED_ARRAY_FLOAT64);
    JS_FreeValue(ctx, ab);
    return out;
}

static const JSCFunctionListEntry dyn_mathx_funcs[] = {
    JS_CFUNC_DEF("mod", 2, dyn_mathx_mod),
    JS_CFUNC_DEF("rem", 2, dyn_mathx_rem),
    JS_CFUNC_DEF("fix", 1, dyn_mathx_fix),
    JS_CFUNC_DEF("sign", 1, dyn_mathx_sign),
    JS_CFUNC_DEF("nthroot", 2, dyn_mathx_nthroot),
    JS_CFUNC_DEF("nextpow2", 1, dyn_mathx_nextpow2),
    JS_CFUNC_DEF("pow2", 1, dyn_mathx_pow2),
    JS_CFUNC_DEF("deg2rad", 1, dyn_mathx_deg2rad),
    JS_CFUNC_DEF("rad2deg", 1, dyn_mathx_rad2deg),
    JS_CFUNC_DEF("eps", 0, dyn_mathx_eps),
    JS_CFUNC_DEF("realmin", 0, dyn_mathx_realmin),
    JS_CFUNC_DEF("realmax", 0, dyn_mathx_realmax),
    JS_CFUNC_DEF("flintmax", 0, dyn_mathx_flintmax),
    JS_CFUNC_DEF("beta", 2, dyn_mathx_beta),
    JS_CFUNC_DEF("betaln", 2, dyn_mathx_betaln),
    JS_CFUNC_DEF("gammaln", 1, dyn_mathx_gammaln),
    JS_CFUNC_DEF("psi", 1, dyn_mathx_psi),
    JS_CFUNC_DEF("erfinv", 1, dyn_mathx_erfinv),
    JS_CFUNC_DEF("erfcinv", 1, dyn_mathx_erfcinv),
    JS_CFUNC_DEF("erfcx", 1, dyn_mathx_erfcx),
    JS_CFUNC_DEF("expint", 1, dyn_mathx_expint),
    JS_CFUNC_DEF("factor", 1, dyn_mathx_factor),
    JS_CFUNC_DEF("primes", 1, dyn_mathx_primes),
    JS_CFUNC_DEF("nchoosek", 2, dyn_mathx_nchoosek),
    JS_CFUNC_DEF("idivide", 2, dyn_mathx_idivide),
    JS_CFUNC_DEF("perms", 1, dyn_mathx_perms),
    JS_CFUNC_DEF("rat", 1, dyn_mathx_rat),
    JS_CFUNC_MAGIC_DEF("linspace", 2, dyn_mathx_linspace, 0),
    JS_CFUNC_MAGIC_DEF("logspace", 2, dyn_mathx_linspace, 1),
    JS_CFUNC_MAGIC_DEF("cumsum", 1, dyn_mathx_scan, 0),
    JS_CFUNC_MAGIC_DEF("cumprod", 1, dyn_mathx_scan, 1),
    JS_CFUNC_MAGIC_DEF("diff", 1, dyn_mathx_scan, 2),
    JS_CFUNC_DEF("solveSPD", 2, dyn_mathx_solve_spd),
    JS_PROP_DOUBLE_DEF("E", 2.71828182845904523536028747135266249775724709369995957496696763, 0),
    JS_PROP_DOUBLE_DEF("Pi", 3.14159265358979323846264338327950288419716939937510582097494459, 0),
    JS_PROP_DOUBLE_DEF("Phi", 1.61803398874989484820458683436563811772030917980576286213544862, 0),
    JS_PROP_DOUBLE_DEF("Sqrt2", 1.41421356237309504880168872420969807856967187537694807317667974, 0),
    JS_PROP_DOUBLE_DEF("SqrtE", 1.64872127070012814684865078781416357165377610071014801157507931, 0),
    JS_PROP_DOUBLE_DEF("SqrtPi", 1.77245385090551602729816748334114518279754945612238712821380779, 0),
    JS_PROP_DOUBLE_DEF("Ln2", 0.693147180559945309417232121458176568075500134360255254120680009, 0),
    JS_PROP_DOUBLE_DEF("Log2E", 1.44269504088896340735992468100189213742664595415298593413544940693, 0),
    JS_PROP_DOUBLE_DEF("Ln10", 2.30258509299404568401799145468436420760110148862877297603332790, 0),
    JS_PROP_DOUBLE_DEF("Log10E", 0.434294481903251827651128918916605082294397005803663526424946929, 0),
    JS_PROP_INT32_DEF("MaxInt32", INT32_MAX, 0),
    JS_PROP_INT32_DEF("MinInt32", INT32_MIN, 0),
    JS_PROP_INT64_DEF("MaxSafeInteger", 9007199254740991LL, 0),

    JS_CFUNC_DEF("gamma", 1, dyn_mathx_gamma),
    JS_CFUNC_DEF("lgamma", 1, dyn_mathx_lgamma),
    JS_CFUNC_DEF("erf", 1, dyn_mathx_erf),
    JS_CFUNC_DEF("erfc", 1, dyn_mathx_erfc),
    JS_CFUNC_DEF("cbrt", 1, dyn_mathx_cbrt),
    JS_CFUNC_DEF("hypot", 2, dyn_mathx_hypot),
    JS_CFUNC_DEF("copysign", 2, dyn_mathx_copysign),
    JS_CFUNC_DEF("nextafter", 2, dyn_mathx_nextafter),
    JS_CFUNC_DEF("expm1", 1, dyn_mathx_expm1),
    JS_CFUNC_DEF("log1p", 1, dyn_mathx_log1p),
    JS_CFUNC_DEF("log2", 1, dyn_mathx_log2),
    JS_CFUNC_DEF("logb", 1, dyn_mathx_logb),
    JS_CFUNC_DEF("scalbn", 2, dyn_mathx_scalbn),
    JS_CFUNC_DEF("ilogb", 1, dyn_mathx_ilogb),
    JS_CFUNC_DEF("modf", 1, dyn_mathx_modf),
    JS_CFUNC_DEF("frexp", 1, dyn_mathx_frexp),
    JS_CFUNC_DEF("ldexp", 2, dyn_mathx_ldexp),
    JS_CFUNC_DEF("remainder", 2, dyn_mathx_remainder),
    JS_CFUNC_DEF("fmod", 2, dyn_mathx_fmod),
    JS_CFUNC_DEF("isInf", 1, dyn_mathx_is_inf),
    JS_CFUNC_DEF("isNaN", 1, dyn_mathx_is_nan),
    JS_CFUNC_DEF("signbit", 1, dyn_mathx_signbit),
    JS_CFUNC_DEF("trunc", 1, dyn_mathx_trunc),
    JS_CFUNC_DEF("round", 1, dyn_mathx_round),
    JS_CFUNC_DEF("roundToEven", 1, dyn_mathx_roundToEven),

    JS_CFUNC_DEF("gcd", 2, dyn_mathx_gcd),
    JS_CFUNC_DEF("lcm", 2, dyn_mathx_lcm),
    JS_CFUNC_DEF("factorial", 1, dyn_mathx_factorial),
    JS_CFUNC_DEF("isPrime", 1, dyn_mathx_is_prime),
    JS_CFUNC_DEF("abs", 1, dyn_mathx_abs),
    JS_CFUNC_DEF("bitLen", 1, dyn_mathx_bit_len),
    JS_CFUNC_DEF("popcount", 1, dyn_mathx_popcount),
    JS_CFUNC_DEF("gammainc", 2, dyn_mathx_gammainc),
    JS_CFUNC_DEF("gammaincinv", 2, dyn_mathx_gammaincinv),
    JS_CFUNC_DEF("betainc", 3, dyn_mathx_betainc),
    JS_CFUNC_DEF("betaincinv", 3, dyn_mathx_betaincinv),
    JS_CFUNC_MAGIC_DEF("besselj", 2, dyn_mathx_bessel, 0),
    JS_CFUNC_MAGIC_DEF("bessely", 2, dyn_mathx_bessel, 1),
    JS_CFUNC_MAGIC_DEF("besseli", 2, dyn_mathx_bessel, 2),
    JS_CFUNC_MAGIC_DEF("besselk", 2, dyn_mathx_bessel, 3),
    JS_CFUNC_MAGIC_DEF("besseliScaled", 2, dyn_mathx_bessel, 4),
    JS_CFUNC_MAGIC_DEF("besselkScaled", 2, dyn_mathx_bessel, 5),
    JS_CFUNC_DEF("besselh", 3, dyn_mathx_besselh),
    JS_CFUNC_DEF("ellipke", 1, dyn_mathx_ellipke),
    JS_CFUNC_DEF("ellipj", 2, dyn_mathx_ellipj),
    JS_CFUNC_DEF("legendre", 2, dyn_mathx_legendre),
    JS_CFUNC_DEF("legendreP", 3, dyn_mathx_legendre_p),
    JS_CFUNC_DEF("polygamma", 2, dyn_mathx_polygamma),
    JS_CFUNC_DEF("airy", 1, dyn_mathx_airy),
};

static int dyn_mathx_init_module(JSContext* ctx, JSModuleDef* m)
{
    if (dyn_register_plain_class(ctx, m, &dyn_expr_class_id,
            &dyn_expr_class, dyn_expr_proto,
            countof(dyn_expr_proto),
            dyn_expr_ctor, "Expression")
        < 0)
        return -1;
    if (JS_SetModuleExportList(ctx, m, dyn_mathx_funcs,
            countof(dyn_mathx_funcs))
        < 0)
        return -1;
    {
        JSValue bits = JS_NewObject(ctx);
        if (JS_IsException(bits))
            return -1;
        if (JS_SetPropertyFunctionList(ctx, bits, dyn_mathx_bits_funcs,
                countof(dyn_mathx_bits_funcs)),
            JS_SetModuleExport(ctx, m, "bits", bits) < 0)
            return -1;
    }
    {
        JSValue stats = JS_NewObject(ctx);
        if (JS_IsException(stats))
            return -1;
        if (JS_SetPropertyFunctionList(ctx, stats, dyn_mathx_stats_funcs,
                countof(dyn_mathx_stats_funcs)),
            dyn_mathx_tests_register(ctx, stats),
            JS_SetModuleExport(ctx, m, "stats", stats) < 0)
            return -1;
    }
    {
        JSValue sig = JS_NewObject(ctx);
        if (JS_IsException(sig))
            return -1;
        if (dyn_mathx_signal_register(ctx, sig) < 0) {
            JS_FreeValue(ctx, sig);
            return -1;
        }
        if (JS_SetModuleExport(ctx, m, "signal", sig) < 0)
            return -1;
    }
    {
        JSValue geo = JS_NewObject(ctx);
        if (JS_IsException(geo))
            return -1;
        if (dyn_mathx_geo_register(ctx, geo) < 0) {
            JS_FreeValue(ctx, geo);
            return -1;
        }
        if (JS_SetModuleExport(ctx, m, "geo", geo) < 0)
            return -1;
    }
    {
        JSValue nd = dyna_nd_ctor_value(ctx), la;
        if (JS_IsException(nd))
            return -1;
        la = JS_NewObject(ctx);
        if (JS_IsException(la)) {
            JS_FreeValue(ctx, nd);
            return -1;
        }
        if (JS_DefinePropertyValueStr(ctx, la, "NDArray", JS_DupValue(ctx, nd), JS_PROP_C_W_E) < 0
            || JS_SetModuleExport(ctx, m, "NDArray", nd) < 0 || JS_SetModuleExport(ctx, m, "linalg", la) < 0)
            return -1;
    }
    return JS_SetModuleExport(ctx, m, "MaxInt64", JS_NewBigInt64(ctx, INT64_MAX));
}

int js_nat_init_mathx(JSContext* ctx)
{
    JSModuleDef* m = JS_NewCModule(ctx, "dyna:mathx", dyn_mathx_init_module);
    if (!m)
        return -1;
    mxst_cid_f64 = mxst_capture_f64_cid(ctx);
    JS_AddModuleExport(ctx, m, "Expression");
    if (JS_AddModuleExportList(ctx, m, dyn_mathx_funcs,
            countof(dyn_mathx_funcs))
        < 0)
        return -1;
    if (JS_AddModuleExport(ctx, m, "bits") < 0)
        return -1;
    if (JS_AddModuleExport(ctx, m, "stats") < 0)
        return -1;
    if (JS_AddModuleExport(ctx, m, "signal") < 0)
        return -1;
    if (JS_AddModuleExport(ctx, m, "geo") < 0)
        return -1;
    if (JS_AddModuleExport(ctx, m, "NDArray") < 0 || JS_AddModuleExport(ctx, m, "linalg") < 0)
        return -1;
    return JS_AddModuleExport(ctx, m, "MaxInt64");
}

#endif
