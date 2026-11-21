#include "mathx-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_MATHX)

static int mxbits_read_mask(JSContext* ctx, JSValueConst v,
    uint64_t mask, uint64_t* out)
{
    uint32_t u;
    if (JS_ToUint32(ctx, &u, v))
        return -1;
    *out = u & mask;
    return 0;
}

int mxbits_read8(JSContext* ctx, JSValueConst v, uint64_t* out)
{
    return mxbits_read_mask(ctx, v, 0xFFu, out);
}

int mxbits_read16(JSContext* ctx, JSValueConst v, uint64_t* out)
{
    return mxbits_read_mask(ctx, v, 0xFFFFu, out);
}

int mxbits_read32(JSContext* ctx, JSValueConst v, uint64_t* out)
{
    return mxbits_read_mask(ctx, v, 0xFFFFFFFFu, out);
}

int mxbits_read64(JSContext* ctx, JSValueConst v, uint64_t* out)
{
    int64_t s;
    if (JS_ToBigInt64(ctx, &s, v))
        return -1;
    *out = (uint64_t)s;
    return 0;
}

JSValue mxbits_reverse64(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    uint64_t x;
    (void)this_val;
    (void)argc;
    if (mxbits_read64(ctx, argv[0], &x))
        return JS_EXCEPTION;
    return JS_NewBigUint64(ctx, dyn_bits_reverse64(x));
}

JSValue mxbits_reverse_bytes16(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    uint64_t x;
    (void)this_val;
    (void)argc;
    if (mxbits_read16(ctx, argv[0], &x))
        return JS_EXCEPTION;
    return JS_NewUint32(ctx, dyn_bits_bswap16((uint16_t)x));
}

JSValue mxbits_reverse_bytes32(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    uint64_t x;
    (void)this_val;
    (void)argc;
    if (mxbits_read32(ctx, argv[0], &x))
        return JS_EXCEPTION;
    return JS_NewUint32(ctx, dyn_bits_bswap32((uint32_t)x));
}

JSValue mxbits_reverse_bytes64(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    uint64_t x;
    (void)this_val;
    (void)argc;
    if (mxbits_read64(ctx, argv[0], &x))
        return JS_EXCEPTION;
    return JS_NewBigUint64(ctx, dyn_bits_bswap64(x));
}

JSValue mxbits_rotate_left64(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    uint64_t x;
    int32_t k;
    (void)this_val;
    (void)argc;
    if (mxbits_read64(ctx, argv[0], &x))
        return JS_EXCEPTION;
    if (JS_ToInt32(ctx, &k, argv[1]))
        return JS_EXCEPTION;
    return JS_NewBigUint64(ctx, dyn_bits_rotate_left(x, k, 64));
}

JSValue mxbits_rotate_right64(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    uint64_t x;
    int32_t k;
    (void)this_val;
    (void)argc;
    if (mxbits_read64(ctx, argv[0], &x))
        return JS_EXCEPTION;
    if (JS_ToInt32(ctx, &k, argv[1]))
        return JS_EXCEPTION;
    return JS_NewBigUint64(ctx, mxbits_rotate_right(x, k, 64));
}

static JSValue mxbits_pair_u32(JSContext* ctx, uint32_t a, uint32_t b)
{
    JSValue arr = JS_NewArray(ctx);
    if (JS_IsException(arr))
        return JS_EXCEPTION;
    if (JS_DefinePropertyValueUint32(ctx, arr, 0, JS_NewUint32(ctx, a),
            JS_PROP_C_W_E)
            < 0
        || JS_DefinePropertyValueUint32(ctx, arr, 1, JS_NewUint32(ctx, b),
               JS_PROP_C_W_E)
            < 0) {
        JS_FreeValue(ctx, arr);
        return JS_EXCEPTION;
    }
    return arr;
}

static JSValue mxbits_pair_u64(JSContext* ctx, uint64_t a, uint64_t b)
{
    JSValue arr = JS_NewArray(ctx);
    if (JS_IsException(arr))
        return JS_EXCEPTION;
    if (JS_DefinePropertyValueUint32(ctx, arr, 0, JS_NewBigUint64(ctx, a),
            JS_PROP_C_W_E)
            < 0
        || JS_DefinePropertyValueUint32(ctx, arr, 1, JS_NewBigUint64(ctx, b),
               JS_PROP_C_W_E)
            < 0) {
        JS_FreeValue(ctx, arr);
        return JS_EXCEPTION;
    }
    return arr;
}

JSValue mxbits_add32(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    uint32_t a, b, c, sum, carry;
    (void)this_val;
    (void)argc;
    if (JS_ToUint32(ctx, &a, argv[0]) || JS_ToUint32(ctx, &b, argv[1]) || JS_ToUint32(ctx, &c, argv[2]))
        return JS_EXCEPTION;
    sum = dyn_bits_add32(a, b, c, &carry);
    return mxbits_pair_u32(ctx, sum, carry);
}

JSValue mxbits_add64(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    uint64_t a, b, c, sum, carry;
    (void)this_val;
    (void)argc;
    if (mxbits_read64(ctx, argv[0], &a) || mxbits_read64(ctx, argv[1], &b) || mxbits_read64(ctx, argv[2], &c))
        return JS_EXCEPTION;
    sum = dyn_bits_add64(a, b, c, &carry);
    return mxbits_pair_u64(ctx, sum, carry);
}

JSValue mxbits_sub32(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    uint32_t a, b, c, diff, borrow;
    (void)this_val;
    (void)argc;
    if (JS_ToUint32(ctx, &a, argv[0]) || JS_ToUint32(ctx, &b, argv[1]) || JS_ToUint32(ctx, &c, argv[2]))
        return JS_EXCEPTION;
    diff = dyn_bits_sub32(a, b, c, &borrow);
    return mxbits_pair_u32(ctx, diff, borrow);
}

JSValue mxbits_sub64(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    uint64_t a, b, c, diff, borrow;
    (void)this_val;
    (void)argc;
    if (mxbits_read64(ctx, argv[0], &a) || mxbits_read64(ctx, argv[1], &b) || mxbits_read64(ctx, argv[2], &c))
        return JS_EXCEPTION;
    diff = dyn_bits_sub64(a, b, c, &borrow);
    return mxbits_pair_u64(ctx, diff, borrow);
}

JSValue mxbits_mul32(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    uint32_t a, b, hi, lo;
    (void)this_val;
    (void)argc;
    if (JS_ToUint32(ctx, &a, argv[0]) || JS_ToUint32(ctx, &b, argv[1]))
        return JS_EXCEPTION;
    dyn_bits_mul32(a, b, &hi, &lo);
    return mxbits_pair_u32(ctx, hi, lo);
}

JSValue mxbits_mul64(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    uint64_t a, b, hi, lo;
    (void)this_val;
    (void)argc;
    if (mxbits_read64(ctx, argv[0], &a) || mxbits_read64(ctx, argv[1], &b))
        return JS_EXCEPTION;
    dyn_bits_mul64(a, b, &hi, &lo);
    return mxbits_pair_u64(ctx, hi, lo);
}

JSValue mxbits_div32(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    uint32_t hi, lo, y, quo, rem;
    (void)this_val;
    (void)argc;
    if (JS_ToUint32(ctx, &hi, argv[0]) || JS_ToUint32(ctx, &lo, argv[1]) || JS_ToUint32(ctx, &y, argv[2]))
        return JS_EXCEPTION;
    switch ((int)(dyn_bits_div32(hi, lo, y, &quo, &rem))) {
    case DYN_BITS_DIV_ZERO:
        return JS_ThrowRangeError(ctx, "dyna:bits: Div32 division by zero");
    case DYN_BITS_OVERFLOW:
        return JS_ThrowRangeError(ctx, "dyna:bits: Div32 quotient overflow");
    default:
        break;
    }
    return mxbits_pair_u32(ctx, quo, rem);
}

JSValue mxbits_div64(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    uint64_t hi, lo, y, quo, rem;
    (void)this_val;
    (void)argc;
    if (mxbits_read64(ctx, argv[0], &hi) || mxbits_read64(ctx, argv[1], &lo) || mxbits_read64(ctx, argv[2], &y))
        return JS_EXCEPTION;
    switch ((int)(dyn_bits_div64(hi, lo, y, &quo, &rem))) {
    case DYN_BITS_DIV_ZERO:
        return JS_ThrowRangeError(ctx, "dyna:bits: Div64 division by zero");
    case DYN_BITS_OVERFLOW:
        return JS_ThrowRangeError(ctx, "dyna:bits: Div64 quotient overflow");
    default:
        break;
    }
    return mxbits_pair_u64(ctx, quo, rem);
}

JSValue mxbits_rem32(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    uint32_t hi, lo, y, rem;
    (void)this_val;
    (void)argc;
    if (JS_ToUint32(ctx, &hi, argv[0]) || JS_ToUint32(ctx, &lo, argv[1]) || JS_ToUint32(ctx, &y, argv[2]))
        return JS_EXCEPTION;
    if (dyn_bits_rem32(hi, lo, y, &rem) != DYN_BITS_OK)
        return JS_ThrowRangeError(ctx, "dyna:bits: Rem32 division by zero");
    return JS_NewUint32(ctx, rem);
}

JSValue mxbits_rem64(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    uint64_t hi, lo, y, rem;
    (void)this_val;
    (void)argc;
    if (mxbits_read64(ctx, argv[0], &hi) || mxbits_read64(ctx, argv[1], &lo) || mxbits_read64(ctx, argv[2], &y))
        return JS_EXCEPTION;
    if (dyn_bits_rem64(hi, lo, y, &rem) != DYN_BITS_OK)
        return JS_ThrowRangeError(ctx, "dyna:bits: Rem64 division by zero");
    return JS_NewBigUint64(ctx, rem);
}

#endif
