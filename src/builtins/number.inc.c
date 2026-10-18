#include "poll.inc.c"

static JSValue js_number_constructor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv)
{
    JSValue val, obj;
    if (argc == 0) {
        val = JS_NewInt32(ctx, 0);
    } else {
        val = JS_ToNumeric(ctx, argv[0]);
        if (JS_IsException(val))
            return val;
        switch (JS_VALUE_GET_TAG(val)) {
        case JS_TAG_SHORT_BIG_INT:
            val = JS_NewInt64(ctx, JS_VALUE_GET_SHORT_BIG_INT(val));
            if (JS_IsException(val))
                return val;
            break;
        case JS_TAG_BIG_INT: {
            JSBigInt* p = JS_VALUE_GET_PTR(val);
            double d;
            d = js_bigint_to_float64(ctx, p);
            JS_FreeValue(ctx, val);
            val = JS_NewFloat64(ctx, d);
        } break;
        default:
            break;
        }
    }
    if (!JS_IsUndefined(new_target)) {
        obj = js_create_from_ctor(ctx, new_target, JS_CLASS_NUMBER);
        if (!JS_IsException(obj))
            JS_SetObjectData(ctx, obj, val);
        return obj;
    } else {
        return val;
    }
}

#if 0
static JSValue js_number___toInteger(JSContext *ctx, JSValueConst this_val,
                                     int argc, JSValueConst *argv)
{
    return JS_ToIntegerFree(ctx, JS_DupValue(ctx, argv[0]));
}

static JSValue js_number___toLength(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv)
{
    int64_t v;
    if (JS_ToLengthFree(ctx, &v, JS_DupValue(ctx, argv[0])))
        return JS_EXCEPTION;
    return JS_NewInt64(ctx, v);
}
#endif

static JSValue js_number_isNaN(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    if (!JS_IsNumber(argv[0]))
        return JS_FALSE;
    return js_global_isNaN(ctx, this_val, argc, argv);
}

static JSValue js_number_isFinite(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    if (!JS_IsNumber(argv[0]))
        return JS_FALSE;
    return js_global_isFinite(ctx, this_val, argc, argv);
}

static JSValue js_number_isInteger(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    int ret;
    ret = JS_NumberIsInteger(ctx, argv[0]);
    if (ret < 0)
        return JS_EXCEPTION;
    else
        return JS_NewBool(ctx, ret);
}

static JSValue js_number_isSafeInteger(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    double d;
    if (!JS_IsNumber(argv[0]))
        return JS_FALSE;
    if (unlikely(JS_ToFloat64(ctx, &d, argv[0])))
        return JS_EXCEPTION;
    return JS_NewBool(ctx, is_safe_integer(d));
}

static JSValue js_number_static_range(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

static const JSCFunctionListEntry js_number_funcs[] = {
    JS_CFUNC_DEF("range", 2, js_number_static_range),
    JS_ALIAS_BASE_DEF("parseInt", "parseInt", 0),
    JS_ALIAS_BASE_DEF("parseFloat", "parseFloat", 0),
    JS_CFUNC_DEF("isNaN", 1, js_number_isNaN),
    JS_CFUNC_DEF("isFinite", 1, js_number_isFinite),
    JS_CFUNC_DEF("isInteger", 1, js_number_isInteger),
    JS_CFUNC_DEF("isSafeInteger", 1, js_number_isSafeInteger),
    JS_PROP_DOUBLE_DEF("MAX_VALUE", 1.7976931348623157e+308, 0),
    JS_PROP_DOUBLE_DEF("MIN_VALUE", 5e-324, 0),
    JS_PROP_DOUBLE_DEF("NaN", DYN_NAN, 0),
    JS_PROP_DOUBLE_DEF("NEGATIVE_INFINITY", -DYN_INFINITY, 0),
    JS_PROP_DOUBLE_DEF("POSITIVE_INFINITY", DYN_INFINITY, 0),
    JS_PROP_DOUBLE_DEF("EPSILON", 2.220446049250313e-16, 0),
    JS_PROP_DOUBLE_DEF("MAX_SAFE_INTEGER", 9007199254740991.0, 0),
    JS_PROP_DOUBLE_DEF("MIN_SAFE_INTEGER", -9007199254740991.0, 0),
};

static JSValue js_thisNumberValue(JSContext* ctx, JSValueConst this_val)
{
    if (JS_IsNumber(this_val))
        return JS_DupValue(ctx, this_val);

    if (JS_VALUE_GET_TAG(this_val) == JS_TAG_OBJECT) {
        JSObject* p = JS_VALUE_GET_OBJ(this_val);
        if (p->class_id == JS_CLASS_NUMBER) {
            if (JS_IsNumber(p->u.object_data))
                return JS_DupValue(ctx, p->u.object_data);
        }
    }
    return JS_ThrowTypeError(ctx, "not a number");
}

static JSValue js_number_valueOf(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    return js_thisNumberValue(ctx, this_val);
}

static int js_get_radix(JSContext* ctx, JSValueConst val)
{
    int radix;
    if (JS_ToInt32Sat(ctx, &radix, val))
        return -1;
    if (radix < 2 || radix > 36) {
        JS_ThrowRangeError(ctx, "radix must be between 2 and 36");
        return -1;
    }
    return radix;
}

static JSValue js_number_toString(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    JSValue val;
    int base, flags;
    double d;

    val = js_thisNumberValue(ctx, this_val);
    if (JS_IsException(val))
        return val;
    if (magic || JS_IsUndefined(argv[0])) {
        base = 10;
    } else {
        base = js_get_radix(ctx, argv[0]);
        if (base < 0)
            goto fail;
    }
    if (JS_VALUE_GET_TAG(val) == JS_TAG_INT) {
        char buf1[70];
        int len;
        len = i64toa_radix(buf1, JS_VALUE_GET_INT(val), base);
        return js_new_string8_len(ctx, buf1, len);
    }
    if (JS_ToFloat64Free(ctx, &d, val))
        return JS_EXCEPTION;
    flags = JS_DTOA_FORMAT_FREE;
    if (base != 10)
        flags |= JS_DTOA_EXP_DISABLED;
    return js_dtoa2(ctx, d, base, 0, flags);
fail:
    JS_FreeValue(ctx, val);
    return JS_EXCEPTION;
}

static JSValue js_number_toFixed(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    JSValue val;
    int f, flags;
    double d;

    val = js_thisNumberValue(ctx, this_val);
    if (JS_IsException(val))
        return val;
    if (JS_ToFloat64Free(ctx, &d, val))
        return JS_EXCEPTION;
    if (JS_ToInt32Sat(ctx, &f, argv[0]))
        return JS_EXCEPTION;
    if (f < 0 || f > 100)
        return JS_ThrowRangeError(ctx, "invalid number of digits");
    if (fabs(d) >= 1e21)
        flags = JS_DTOA_FORMAT_FREE;
    else
        flags = JS_DTOA_FORMAT_FRAC;
    return js_dtoa2(ctx, d, 10, f, flags);
}

static JSValue js_number_toExponential(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    JSValue val;
    int f, flags;
    double d;

    val = js_thisNumberValue(ctx, this_val);
    if (JS_IsException(val))
        return val;
    if (JS_ToFloat64Free(ctx, &d, val))
        return JS_EXCEPTION;
    if (JS_ToInt32Sat(ctx, &f, argv[0]))
        return JS_EXCEPTION;
    if (!isfinite(d)) {
        return JS_ToStringFree(ctx, __JS_NewFloat64(ctx, d));
    }
    if (JS_IsUndefined(argv[0])) {
        flags = JS_DTOA_FORMAT_FREE;
        f = 0;
    } else {
        if (f < 0 || f > 100)
            return JS_ThrowRangeError(ctx, "invalid number of digits");
        f++;
        flags = JS_DTOA_FORMAT_FIXED;
    }
    return js_dtoa2(ctx, d, 10, f, flags | JS_DTOA_EXP_ENABLED);
}

static JSValue js_number_toPrecision(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    JSValue val;
    int p;
    double d;

    val = js_thisNumberValue(ctx, this_val);
    if (JS_IsException(val))
        return val;
    if (JS_ToFloat64Free(ctx, &d, val))
        return JS_EXCEPTION;
    if (JS_IsUndefined(argv[0]))
        goto to_string;
    if (JS_ToInt32Sat(ctx, &p, argv[0]))
        return JS_EXCEPTION;
    if (!isfinite(d)) {
    to_string:
        return JS_ToStringFree(ctx, __JS_NewFloat64(ctx, d));
    }
    if (p < 1 || p > 100)
        return JS_ThrowRangeError(ctx, "invalid number of digits");
    return js_dtoa2(ctx, d, 10, p, JS_DTOA_FORMAT_FIXED);
}

static double js_math_round(double a);

enum {
    NUM_EXT_ABS,
    NUM_EXT_SQRT,
    NUM_EXT_EXP,
    NUM_EXT_SIN,
    NUM_EXT_COS,
    NUM_EXT_TAN,
    NUM_EXT_ASIN,
    NUM_EXT_ACOS,
    NUM_EXT_ATAN,
    NUM_EXT_NEGATE,
    NUM_EXT_INC,
    NUM_EXT_DEC,
};

static JSValue js_number_ext_unary(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    JSValue val;
    double d;
    (void)argc;
    (void)argv;
    val = js_thisNumberValue(ctx, this_val);
    if (JS_IsException(val))
        return val;
    if (JS_ToFloat64Free(ctx, &d, val))
        return JS_EXCEPTION;
    switch (magic) {
    case NUM_EXT_ABS:
        d = fabs(d);
        break;
    case NUM_EXT_SQRT:
        d = sqrt(d);
        break;
    case NUM_EXT_EXP:
        d = exp(d);
        break;
    case NUM_EXT_SIN:
        d = sin(d);
        break;
    case NUM_EXT_COS:
        d = cos(d);
        break;
    case NUM_EXT_TAN:
        d = tan(d);
        break;
    case NUM_EXT_ASIN:
        d = asin(d);
        break;
    case NUM_EXT_ACOS:
        d = acos(d);
        break;
    case NUM_EXT_ATAN:
        d = atan(d);
        break;
    case NUM_EXT_NEGATE:
        d = -d;
        break;
    case NUM_EXT_INC:
        d = d + 1;
        break;
    case NUM_EXT_DEC:
        d = d - 1;
        break;
    }
    return JS_NewFloat64(ctx, d);
}

enum {
    NUM_EXT_ADD,
    NUM_EXT_SUB,
    NUM_EXT_MUL,
    NUM_EXT_DIV,
    NUM_EXT_MOD,
    NUM_EXT_POW,
};

static JSValue js_number_ext_binary(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    JSValue val;
    double d, a;
    val = js_thisNumberValue(ctx, this_val);
    if (JS_IsException(val))
        return val;
    if (JS_ToFloat64Free(ctx, &d, val))
        return JS_EXCEPTION;
    if (JS_ToFloat64(ctx, &a, argc > 0 ? argv[0] : JS_UNDEFINED))
        return JS_EXCEPTION;
    switch (magic) {
    case NUM_EXT_ADD:
        d = d + a;
        break;
    case NUM_EXT_SUB:
        d = d - a;
        break;
    case NUM_EXT_MUL:
        d = d * a;
        break;
    case NUM_EXT_DIV:
        d = d / a;
        break;
    case NUM_EXT_MOD:
        d = fmod(d, a);
        break;
    case NUM_EXT_POW:
        d = js_pow(d, a);
        break;
    }
    return JS_NewFloat64(ctx, d);
}

enum {
    NUM_EXT_GT,
    NUM_EXT_GTE,
    NUM_EXT_LT,
    NUM_EXT_LTE,
};

static JSValue js_number_ext_compare(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    JSValue val;
    double d, a;
    BOOL r = FALSE;
    val = js_thisNumberValue(ctx, this_val);
    if (JS_IsException(val))
        return val;
    if (JS_ToFloat64Free(ctx, &d, val))
        return JS_EXCEPTION;
    if (JS_ToFloat64(ctx, &a, argc > 0 ? argv[0] : JS_UNDEFINED))
        return JS_EXCEPTION;
    switch (magic) {
    case NUM_EXT_GT:
        r = d > a;
        break;
    case NUM_EXT_GTE:
        r = d >= a;
        break;
    case NUM_EXT_LT:
        r = d < a;
        break;
    case NUM_EXT_LTE:
        r = d <= a;
        break;
    }
    return JS_NewBool(ctx, r);
}

enum {
    NUM_EXT_IS_INTEGER,
    NUM_EXT_IS_ODD,
    NUM_EXT_IS_EVEN,
};

static JSValue js_number_ext_predicate(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    JSValue val;
    double d;
    BOOL is_int, r = FALSE;
    (void)argc;
    (void)argv;
    val = js_thisNumberValue(ctx, this_val);
    if (JS_IsException(val))
        return val;
    if (JS_ToFloat64Free(ctx, &d, val))
        return JS_EXCEPTION;
    is_int = isfinite(d) && floor(d) == d;
    switch (magic) {
    case NUM_EXT_IS_INTEGER:
        r = is_int;
        break;
    case NUM_EXT_IS_ODD:
        r = is_int && fmod(d, 2.0) != 0.0;
        break;
    case NUM_EXT_IS_EVEN:
        r = is_int && fmod(d, 2.0) == 0.0;
        break;
    }
    return JS_NewBool(ctx, r);
}

static JSValue js_number_ext_isMultipleOf(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    JSValue val;
    double d, a;
    val = js_thisNumberValue(ctx, this_val);
    if (JS_IsException(val))
        return val;
    if (JS_ToFloat64Free(ctx, &d, val))
        return JS_EXCEPTION;
    if (JS_ToFloat64(ctx, &a, argc > 0 ? argv[0] : JS_UNDEFINED))
        return JS_EXCEPTION;
    return JS_NewBool(ctx, a != 0.0 && fmod(d, a) == 0.0);
}

static JSValue js_number_ext_mathMod(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    JSValue val;
    double d, a, r;
    val = js_thisNumberValue(ctx, this_val);
    if (JS_IsException(val))
        return val;
    if (JS_ToFloat64Free(ctx, &d, val))
        return JS_EXCEPTION;
    if (JS_ToFloat64(ctx, &a, argc > 0 ? argv[0] : JS_UNDEFINED))
        return JS_EXCEPTION;
    if (!(isfinite(d) && floor(d) == d) || !(isfinite(a) && floor(a) == a) || a < 1.0)
        return JS_NewFloat64(ctx, DYN_NAN);
    r = fmod(fmod(d, a) + a, a);
    return JS_NewFloat64(ctx, r);
}

static JSValue js_number_ext_clamp(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    JSValue val;
    double d, lo, hi;
    val = js_thisNumberValue(ctx, this_val);
    if (JS_IsException(val))
        return val;
    if (JS_ToFloat64Free(ctx, &d, val))
        return JS_EXCEPTION;
    if (JS_ToFloat64(ctx, &lo, argc > 0 ? argv[0] : JS_UNDEFINED))
        return JS_EXCEPTION;
    if (JS_ToFloat64(ctx, &hi, argc > 1 ? argv[1] : JS_UNDEFINED))
        return JS_EXCEPTION;
    return JS_NewFloat64(ctx, d < lo ? lo : (d > hi ? hi : d));
}

static JSValue js_number_ext_log(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    JSValue val;
    double d, base;
    val = js_thisNumberValue(ctx, this_val);
    if (JS_IsException(val))
        return val;
    if (JS_ToFloat64Free(ctx, &d, val))
        return JS_EXCEPTION;
    if (argc > 0 && !JS_IsUndefined(argv[0])) {
        if (JS_ToFloat64(ctx, &base, argv[0]))
            return JS_EXCEPTION;
        return JS_NewFloat64(ctx, log(d) / log(base));
    }
    return JS_NewFloat64(ctx, log(d));
}

enum {
    NUM_EXT_ROUND,
    NUM_EXT_CEIL,
    NUM_EXT_FLOOR,
};

static JSValue js_number_ext_roundp(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    JSValue val;
    double d, factor, scaled;
    int prec = 0;
    val = js_thisNumberValue(ctx, this_val);
    if (JS_IsException(val))
        return val;
    if (JS_ToFloat64Free(ctx, &d, val))
        return JS_EXCEPTION;
    if (argc > 0 && !JS_IsUndefined(argv[0])) {
        if (JS_ToInt32Sat(ctx, &prec, argv[0]))
            return JS_EXCEPTION;
    }
    if (prec > 308)
        prec = 308;
    else if (prec < -308)
        prec = -308;
    if (prec >= 0 && prec <= 15) {
        static const double pow10[16] = {
            1e0,
            1e1,
            1e2,
            1e3,
            1e4,
            1e5,
            1e6,
            1e7,
            1e8,
            1e9,
            1e10,
            1e11,
            1e12,
            1e13,
            1e14,
            1e15,
        };
        factor = pow10[prec];
    } else {
        factor = pow(10.0, (double)prec);
    }
    if (!isfinite(factor) || factor == 0.0)
        return JS_NewFloat64(ctx, d);
    scaled = d * factor;
    switch (magic) {
    case NUM_EXT_ROUND:

        scaled = scaled < 0.0 ? -js_math_round(-scaled) : js_math_round(scaled);
        break;
    case NUM_EXT_CEIL:
        scaled = ceil(scaled);
        break;
    case NUM_EXT_FLOOR:
        scaled = floor(scaled);
        break;
    }
    return JS_NewFloat64(ctx, scaled / factor);
}

static JSValue js_number_ext_chr(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    JSValue val;
    StringBuffer b_s, *b = &b_s;
    int32_t c;
    (void)argc;
    (void)argv;
    val = js_thisNumberValue(ctx, this_val);
    if (JS_IsException(val))
        return val;
    if (JS_ToInt32(ctx, &c, val)) {
        JS_FreeValue(ctx, val);
        return JS_EXCEPTION;
    }
    JS_FreeValue(ctx, val);
    if (c < 0 || c > 0x10ffff)
        return JS_ThrowRangeError(ctx, "invalid code point");
    if (string_buffer_init(ctx, b, 1))
        return JS_EXCEPTION;

    if (string_buffer_putc(b, c)) {
        string_buffer_free(b);
        return JS_EXCEPTION;
    }
    return string_buffer_end(b);
}

#define NUM_RANGE_MAX 100000000
#define NUM_PAD_MAX 65536

static int js_number_pad_into(StringBuffer* b, uint64_t uv, int place, int base,
    int neg, int force_sign)
{
    char digs[72];
    int len, zeros, i;
    len = u64toa_radix(digs, uv, base);
    if (place > NUM_PAD_MAX)
        place = NUM_PAD_MAX;
    zeros = place - len;
    if (zeros < 0)
        zeros = 0;
    if (neg) {
        if (string_buffer_putc8(b, '-'))
            return -1;
    } else if (force_sign) {
        if (string_buffer_putc8(b, '+'))
            return -1;
    }
    for (i = 0; i < zeros; i++)
        if (string_buffer_putc8(b, '0'))
            return -1;
    for (i = 0; i < len; i++)
        if (string_buffer_putc8(b, digs[i]))
            return -1;
    return 0;
}

static JSValue js_number_ext_pad(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    JSValue val;
    double d;
    int place, base = magic ? 16 : 10, force_sign = 0, neg = 0;
    int64_t iv;
    uint64_t uv;
    StringBuffer b_s, *b = &b_s;
    val = js_thisNumberValue(ctx, this_val);
    if (JS_IsException(val))
        return val;
    if (JS_ToFloat64Free(ctx, &d, val))
        return JS_EXCEPTION;
    place = magic ? 1 : 0;
    if (argc > 0 && !JS_IsUndefined(argv[0]) && JS_ToInt32Sat(ctx, &place, argv[0]))
        return JS_EXCEPTION;
    if (!magic) {
        force_sign = (argc > 1) ? JS_ToBool(ctx, argv[1]) : 0;
        if (argc > 2 && !JS_IsUndefined(argv[2])) {
            if (JS_ToInt32Sat(ctx, &base, argv[2]))
                return JS_EXCEPTION;
            if (base < 2 || base > 36)
                return JS_ThrowRangeError(ctx, "base must be 2..36");
        }
    }
    if (place < 0)
        place = 0;
    if (place > NUM_PAD_MAX)
        place = NUM_PAD_MAX;
    if (!isfinite(d))
        return JS_ToStringFree(ctx, __JS_NewFloat64(ctx, d));
    if (d >= 9223372036854775808.0)
        d = 9223372036854775808.0 - 1024.0;
    else if (d < -9223372036854775808.0)
        d = -9223372036854775808.0;
    iv = (int64_t)d;
    if (iv < 0) {
        neg = 1;
        uv = (uint64_t)0 - (uint64_t)iv;
    } else
        uv = (uint64_t)iv;
    if (string_buffer_init(ctx, b, place > 0 ? place + 1 : 8))
        return JS_EXCEPTION;
    if (js_number_pad_into(b, uv, place, base, neg, force_sign)) {
        string_buffer_free(b);
        return JS_EXCEPTION;
    }
    return string_buffer_end(b);
}

static JSValue js_number_ext_format(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    JSValue val, ret = JS_EXCEPTION;
    double d;
    int place = 0;
    JSValue thou = JS_UNDEFINED, dec = JS_UNDEFINED;
    char stack_num[64];
    char* num = stack_num;
    BOOL num_heap = FALSE;
    char* dot;
    int intlen, i, first, grp;
    StringBuffer b_s, *b = &b_s;
    (void)magic;
    val = js_thisNumberValue(ctx, this_val);
    if (JS_IsException(val))
        return val;
    if (JS_ToFloat64Free(ctx, &d, val))
        return JS_EXCEPTION;
    if (argc > 0 && !JS_IsUndefined(argv[0]) && JS_ToInt32Sat(ctx, &place, argv[0]))
        return JS_EXCEPTION;
    if (place < 0)
        place = 0;
    if (place > 20)
        place = 20;
    if (argc > 1 && !JS_IsUndefined(argv[1])) {
        thou = JS_ToString(ctx, argv[1]);
        if (JS_IsException(thou))
            return JS_EXCEPTION;
    }
    if (argc > 2 && !JS_IsUndefined(argv[2])) {
        dec = JS_ToString(ctx, argv[2]);
        if (JS_IsException(dec)) {
            JS_FreeValue(ctx, thou);
            return JS_EXCEPTION;
        }
    }
    if (!isfinite(d)) {
        ret = JS_ToStringFree(ctx, __JS_NewFloat64(ctx, d));
        goto done;
    }
    {
        int nlen = snprintf(NULL, 0, "%.*f", place, d);
        if (nlen < 0)
            goto done;
        if (nlen >= (int)sizeof(stack_num)) {
            num = js_malloc(ctx, (size_t)nlen + 1);
            if (!num)
                goto done;
            num_heap = TRUE;
            snprintf(num, (size_t)nlen + 1, "%.*f", place, d);
        } else {
            snprintf(num, sizeof(stack_num), "%.*f", place, d);
        }
        dot = num;
    }
    while (*dot && *dot != '.')
        dot++;
    intlen = (int)(dot - num);
    if (!*dot)
        dot = NULL;
    first = 0;
    if (num[0] == '-')
        first = 1;
    if (string_buffer_init(ctx, b, intlen + 8))
        goto done;
    if (first) {
        if (string_buffer_putc8(b, '-'))
            goto sb_fail;
    }
    grp = (intlen - first) % 3;
    if (grp == 0)
        grp = 3;
    for (i = first; i < intlen; i++) {
        if (i > first && grp == 0) {
            if (!JS_IsUndefined(thou)) {
                if (string_buffer_concat_value(b, thou))
                    goto sb_fail;
            } else if (string_buffer_putc8(b, ','))
                goto sb_fail;
            grp = 3;
        }
        if (string_buffer_putc8(b, num[i]))
            goto sb_fail;
        grp--;
    }
    if (dot) {
        if (!JS_IsUndefined(dec)) {
            if (string_buffer_concat_value(b, dec))
                goto sb_fail;
        } else if (string_buffer_putc8(b, '.'))
            goto sb_fail;
        if (string_buffer_puts8(b, dot + 1))
            goto sb_fail;
    }
    ret = string_buffer_end(b);
    goto done;
sb_fail:
    string_buffer_free(b);
done:
    if (num_heap)
        js_free(ctx, num);
    JS_FreeValue(ctx, thou);
    JS_FreeValue(ctx, dec);
    return ret;
}

static JSValue js_number_ext_abbr(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    static const char* const abbr_u[] = { "", "k", "m", "b", "t" };
    static const char* const metric_u[] = { "", "k", "M", "G", "T", "P", "E" };
    static const char* const bytes_u[] = { "B", "KB", "MB", "GB", "TB", "PB", "EB" };
    JSValue val;
    double d, scale = (magic == 2) ? 1024.0 : 1000.0, av;
    int precision = 0, idx = 0, maxidx;
    char out[96];
    const char* const* units;
    val = js_thisNumberValue(ctx, this_val);
    if (JS_IsException(val))
        return val;
    if (JS_ToFloat64Free(ctx, &d, val))
        return JS_EXCEPTION;
    if (argc > 0 && !JS_IsUndefined(argv[0]) && JS_ToInt32Sat(ctx, &precision, argv[0]))
        return JS_EXCEPTION;
    if (precision < 0)
        precision = 0;
    if (precision > 20)
        precision = 20;
    switch (magic) {
    case 0:
        units = abbr_u;
        maxidx = 4;
        break;
    case 1:
        units = metric_u;
        maxidx = 6;
        break;
    default:
        units = bytes_u;
        maxidx = 6;
        break;
    }
    if (!isfinite(d))
        return JS_ToStringFree(ctx, __JS_NewFloat64(ctx, d));
    av = fabs(d);
    while (av >= scale && idx < maxidx) {
        av /= scale;
        d /= scale;
        idx++;
    }
    {
        int olen = snprintf(NULL, 0, "%.*f%s", precision, d, units[idx]);
        char* outp = out;
        JSValue res;
        if (olen < 0)
            return JS_EXCEPTION;
        if (olen >= (int)sizeof(out)) {
            outp = js_malloc(ctx, (size_t)olen + 1);
            if (!outp)
                return JS_EXCEPTION;
            snprintf(outp, (size_t)olen + 1, "%.*f%s", precision, d, units[idx]);
        } else {
            snprintf(outp, sizeof(out), "%.*f%s", precision, d, units[idx]);
        }
        res = js_new_string8(ctx, outp);
        if (outp != out)
            js_free(ctx, outp);
        return res;
    }
}

static JSValue js_number_ext_ordinalize(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    JSValue val;
    double d;
    int64_t iv, last2, last1;
    uint64_t ab;
    const char* suf;
    char out[80];
    (void)argc;
    (void)argv;
    val = js_thisNumberValue(ctx, this_val);
    if (JS_IsException(val))
        return val;
    if (JS_ToFloat64Free(ctx, &d, val))
        return JS_EXCEPTION;
    if (!isfinite(d))
        return JS_ToStringFree(ctx, __JS_NewFloat64(ctx, d));
    if (d >= 9223372036854775808.0)
        d = 9223372036854775808.0 - 1024.0;
    else if (d < -9223372036854775808.0)
        d = -9223372036854775808.0;
    iv = (int64_t)d;
    ab = iv < 0 ? (uint64_t)0 - (uint64_t)iv : (uint64_t)iv;
    last2 = (int64_t)(ab % 100);
    last1 = (int64_t)(ab % 10);
    if (last2 >= 11 && last2 <= 13)
        suf = "th";
    else if (last1 == 1)
        suf = "st";
    else if (last1 == 2)
        suf = "nd";
    else if (last1 == 3)
        suf = "rd";
    else
        suf = "th";
    snprintf(out, sizeof(out), "%lld%s", (long long)iv, suf);
    return js_new_string8(ctx, out);
}

static JSValue js_number_ext_duration(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    static const struct {
        double ms;
        const char* name;
    } units[] = {
        { 31557600000.0, "year" },
        { 2629800000.0, "month" },
        { 604800000.0, "week" },
        { 86400000.0, "day" },
        { 3600000.0, "hour" },
        { 60000.0, "minute" },
        { 1000.0, "second" },
        { 1.0, "millisecond" },
    };
    JSValue val;
    double d, av;
    long long n;
    unsigned i;
    char out[96];
    (void)argc;
    (void)argv;
    val = js_thisNumberValue(ctx, this_val);
    if (JS_IsException(val))
        return val;
    if (JS_ToFloat64Free(ctx, &d, val))
        return JS_EXCEPTION;
    if (!isfinite(d))
        return JS_ToStringFree(ctx, __JS_NewFloat64(ctx, d));
    av = fabs(d);
    if (av >= 9223372036854775808.0)
        av = 9223372036854775808.0 - 1024.0;
    for (i = 0; i < countof(units) - 1; i++)
        if (av >= units[i].ms)
            break;
    n = (long long)(av / units[i].ms);
    snprintf(out, sizeof(out), "%lld %s%s", n, units[i].name, n == 1 ? "" : "s");
    return js_new_string8(ctx, out);
}

static JSValue js_number_build_range(JSContext* ctx, double start, double step,
    int64_t count, JSValueConst fn)
{
    JSValue arr;
    int64_t i;
    BOOL has_fn = JS_IsFunction(ctx, fn);
    arr = JS_NewArray(ctx);
    if (JS_IsException(arr))
        return arr;
    for (i = 0; i < count; i++) {
        double v = start + (double)i * step;
        JSValue item;
        if (dyn_poll_interrupts(ctx, i)) {
            JS_FreeValue(ctx, arr);
            return JS_EXCEPTION;
        }
        if (has_fn) {
            JSValueConst args[2];
            JSValue nv = JS_NewFloat64(ctx, v), iv = JS_NewInt64(ctx, i);
            args[0] = nv;
            args[1] = iv;
            item = JS_Call(ctx, fn, JS_UNDEFINED, 2, args);
            JS_FreeValue(ctx, nv);
            JS_FreeValue(ctx, iv);
            if (JS_IsException(item)) {
                JS_FreeValue(ctx, arr);
                return JS_EXCEPTION;
            }
        } else {
            item = JS_NewFloat64(ctx, v);
        }
        if (JS_SetPropertyInt64(ctx, arr, i, item) < 0) {
            JS_FreeValue(ctx, arr);
            return JS_EXCEPTION;
        }
    }
    return arr;
}

static JSValue js_number_ext_times(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    JSValue val;
    double d;
    int64_t n;
    val = js_thisNumberValue(ctx, this_val);
    if (JS_IsException(val))
        return val;
    if (JS_ToFloat64Free(ctx, &d, val))
        return JS_EXCEPTION;
    if (!isfinite(d) || d <= 0)
        n = 0;
    else if (d >= 9223372036854775808.0)
        n = 9223372036854775807LL;
    else
        n = (int64_t)d;
    if (n > NUM_RANGE_MAX)
        return JS_ThrowRangeError(ctx, "times count too large");
    return js_number_build_range(ctx, 0, 1, n,
        argc > 0 ? argv[0] : JS_UNDEFINED);
}

static JSValue js_number_ext_upto(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    JSValue val;
    double d, end, step = 1.0, span;
    int64_t count;
    val = js_thisNumberValue(ctx, this_val);
    if (JS_IsException(val))
        return val;
    if (JS_ToFloat64Free(ctx, &d, val))
        return JS_EXCEPTION;
    if (JS_ToFloat64(ctx, &end, argc > 0 ? argv[0] : JS_UNDEFINED))
        return JS_EXCEPTION;
    if (argc > 1 && !JS_IsUndefined(argv[1])) {
        if (JS_ToFloat64(ctx, &step, argv[1]))
            return JS_EXCEPTION;
    }
    step = fabs(step);
    if (magic)
        step = -step;
    if (!isfinite(d) || !isfinite(end) || !isfinite(step) || step == 0.0)
        return JS_ThrowRangeError(ctx, "invalid range (non-finite or zero step)");
    span = magic ? (d - end) : (end - d);
    if (span < 0)
        return JS_NewArray(ctx);
    {
        double q = span / fabs(step);
        if (!(q < (double)NUM_RANGE_MAX))
            return JS_ThrowRangeError(ctx, "range too large");
        count = (int64_t)q + 1;
    }
    if (count > NUM_RANGE_MAX)
        return JS_ThrowRangeError(ctx, "range too large");
    return js_number_build_range(ctx, d, step, count,
        argc > 2 ? argv[2] : JS_UNDEFINED);
}

static JSValue js_number_static_range(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    double start, end, step = 1.0, span;
    int64_t count;
    (void)this_val;
    if (JS_ToFloat64(ctx, &start, argc > 0 ? argv[0] : JS_UNDEFINED))
        return JS_EXCEPTION;
    if (argc > 1 && !JS_IsUndefined(argv[1])) {
        if (JS_ToFloat64(ctx, &end, argv[1]))
            return JS_EXCEPTION;
    } else {

        end = start;
        start = 0.0;
    }
    if (argc > 2 && !JS_IsUndefined(argv[2])) {
        if (JS_ToFloat64(ctx, &step, argv[2]))
            return JS_EXCEPTION;
    }
    if (!isfinite(start) || !isfinite(end) || !isfinite(step) || step == 0.0)
        return JS_ThrowRangeError(ctx, "invalid range (non-finite or zero step)");
    span = end - start;
    if ((span > 0) != (step > 0))
        return JS_NewArray(ctx);
    {
        double q = ceil(span / step);
        if (!(q < (double)NUM_RANGE_MAX))
            return JS_ThrowRangeError(ctx, "range too large");
        count = q < 0 ? 0 : (int64_t)q;
    }
    if (count > NUM_RANGE_MAX)
        return JS_ThrowRangeError(ctx, "range too large");
    return js_number_build_range(ctx, start, step, count, JS_UNDEFINED);
}

static const JSCFunctionListEntry js_number_ext_funcs[] = {
    JS_CFUNC_MAGIC_DEF("abs", 0, js_number_ext_unary, NUM_EXT_ABS),
    JS_CFUNC_MAGIC_DEF("sqrt", 0, js_number_ext_unary, NUM_EXT_SQRT),
    JS_CFUNC_MAGIC_DEF("exp", 0, js_number_ext_unary, NUM_EXT_EXP),
    JS_CFUNC_MAGIC_DEF("sin", 0, js_number_ext_unary, NUM_EXT_SIN),
    JS_CFUNC_MAGIC_DEF("cos", 0, js_number_ext_unary, NUM_EXT_COS),
    JS_CFUNC_MAGIC_DEF("tan", 0, js_number_ext_unary, NUM_EXT_TAN),
    JS_CFUNC_MAGIC_DEF("asin", 0, js_number_ext_unary, NUM_EXT_ASIN),
    JS_CFUNC_MAGIC_DEF("acos", 0, js_number_ext_unary, NUM_EXT_ACOS),
    JS_CFUNC_MAGIC_DEF("atan", 0, js_number_ext_unary, NUM_EXT_ATAN),
    JS_CFUNC_MAGIC_DEF("negate", 0, js_number_ext_unary, NUM_EXT_NEGATE),
    JS_CFUNC_MAGIC_DEF("inc", 0, js_number_ext_unary, NUM_EXT_INC),
    JS_CFUNC_MAGIC_DEF("dec", 0, js_number_ext_unary, NUM_EXT_DEC),
    JS_CFUNC_MAGIC_DEF("add", 1, js_number_ext_binary, NUM_EXT_ADD),
    JS_CFUNC_MAGIC_DEF("subtract", 1, js_number_ext_binary, NUM_EXT_SUB),
    JS_CFUNC_MAGIC_DEF("multiply", 1, js_number_ext_binary, NUM_EXT_MUL),
    JS_CFUNC_MAGIC_DEF("divide", 1, js_number_ext_binary, NUM_EXT_DIV),
    JS_CFUNC_MAGIC_DEF("modulo", 1, js_number_ext_binary, NUM_EXT_MOD),
    JS_CFUNC_MAGIC_DEF("pow", 1, js_number_ext_binary, NUM_EXT_POW),
    JS_CFUNC_MAGIC_DEF("gt", 1, js_number_ext_compare, NUM_EXT_GT),
    JS_CFUNC_MAGIC_DEF("gte", 1, js_number_ext_compare, NUM_EXT_GTE),
    JS_CFUNC_MAGIC_DEF("lt", 1, js_number_ext_compare, NUM_EXT_LT),
    JS_CFUNC_MAGIC_DEF("lte", 1, js_number_ext_compare, NUM_EXT_LTE),
    JS_CFUNC_MAGIC_DEF("isInteger", 0, js_number_ext_predicate, NUM_EXT_IS_INTEGER),
    JS_CFUNC_MAGIC_DEF("isOdd", 0, js_number_ext_predicate, NUM_EXT_IS_ODD),
    JS_CFUNC_MAGIC_DEF("isEven", 0, js_number_ext_predicate, NUM_EXT_IS_EVEN),
    JS_CFUNC_DEF("isMultipleOf", 1, js_number_ext_isMultipleOf),
    JS_CFUNC_DEF("mathMod", 1, js_number_ext_mathMod),
    JS_CFUNC_DEF("clamp", 2, js_number_ext_clamp),
    JS_CFUNC_DEF("log", 0, js_number_ext_log),
    JS_CFUNC_MAGIC_DEF("round", 0, js_number_ext_roundp, NUM_EXT_ROUND),
    JS_CFUNC_MAGIC_DEF("ceil", 0, js_number_ext_roundp, NUM_EXT_CEIL),
    JS_CFUNC_MAGIC_DEF("floor", 0, js_number_ext_roundp, NUM_EXT_FLOOR),
    JS_CFUNC_DEF("chr", 0, js_number_ext_chr),
    JS_CFUNC_MAGIC_DEF("pad", 0, js_number_ext_pad, 0),
    JS_CFUNC_MAGIC_DEF("hex", 0, js_number_ext_pad, 1),
    JS_CFUNC_MAGIC_DEF("format", 0, js_number_ext_format, 0),
    JS_CFUNC_MAGIC_DEF("abbr", 0, js_number_ext_abbr, 0),
    JS_CFUNC_MAGIC_DEF("metric", 0, js_number_ext_abbr, 1),
    JS_CFUNC_MAGIC_DEF("bytes", 0, js_number_ext_abbr, 2),
    JS_CFUNC_DEF("ordinalize", 0, js_number_ext_ordinalize),
    JS_CFUNC_DEF("duration", 0, js_number_ext_duration),
    JS_CFUNC_DEF("times", 0, js_number_ext_times),
    JS_CFUNC_MAGIC_DEF("upto", 1, js_number_ext_upto, 0),
    JS_CFUNC_MAGIC_DEF("downto", 1, js_number_ext_upto, 1),
};

static const JSCFunctionListEntry js_number_proto_funcs[] = {
    JS_CFUNC_DEF("toExponential", 1, js_number_toExponential),
    JS_CFUNC_DEF("toFixed", 1, js_number_toFixed),
    JS_CFUNC_DEF("toPrecision", 1, js_number_toPrecision),
    JS_CFUNC_MAGIC_DEF("toString", 1, js_number_toString, 0),
    JS_CFUNC_MAGIC_DEF("toLocaleString", 0, js_number_toString, 1),
    JS_CFUNC_DEF("valueOf", 0, js_number_valueOf),
};

static JSValue js_parseInt(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    const char *str, *p;
    int radix, flags;
    JSValue ret;

    if (argc >= 1 && JS_IsNumber(argv[0])) {
        int radix0;
        if (JS_ToInt32(ctx, &radix0, argv[1]))
            return JS_EXCEPTION;
        if (radix0 == 0 || radix0 == 10) {
            if (JS_VALUE_GET_TAG(argv[0]) == JS_TAG_INT)
                return JS_DupValue(ctx, argv[0]);
            {
                double d = JS_VALUE_GET_FLOAT64(argv[0]);
                if (isfinite(d) && d == floor(d) && fabs(d) <= 9007199254740992.0) {
                    if (d == 0)
                        return JS_NewInt32(ctx, 0);
                    return JS_NewFloat64(ctx, d);
                }
            }
        }
        str = JS_ToCString(ctx, argv[0]);
        if (!str)
            return JS_EXCEPTION;
        radix = radix0;
        if (radix != 0 && (radix < 2 || radix > 36)) {
            ret = JS_NAN;
        } else {
            p = str;
            p += skip_spaces(p);
            flags = ATOD_INT_ONLY | ATOD_ACCEPT_PREFIX_AFTER_SIGN;
            ret = js_atof(ctx, p, NULL, radix, flags);
        }
        JS_FreeCString(ctx, str);
        return ret;
    }

    str = JS_ToCString(ctx, argv[0]);
    if (!str)
        return JS_EXCEPTION;
    if (JS_ToInt32(ctx, &radix, argv[1])) {
        JS_FreeCString(ctx, str);
        return JS_EXCEPTION;
    }
    if (radix != 0 && (radix < 2 || radix > 36)) {
        ret = JS_NAN;
    } else {
        p = str;
        p += skip_spaces(p);
        flags = ATOD_INT_ONLY | ATOD_ACCEPT_PREFIX_AFTER_SIGN;
        ret = js_atof(ctx, p, NULL, radix, flags);
    }
    JS_FreeCString(ctx, str);
    return ret;
}

static JSValue js_parseFloat(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    const char *str, *p;
    JSValue ret;

    str = JS_ToCString(ctx, argv[0]);
    if (!str)
        return JS_EXCEPTION;
    p = str;
    p += skip_spaces(p);
    ret = js_atof(ctx, p, NULL, 10, 0);
    JS_FreeCString(ctx, str);
    return ret;
}

static JSValue js_boolean_constructor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv)
{
    JSValue val, obj;
    val = JS_NewBool(ctx, JS_ToBool(ctx, argv[0]));
    if (!JS_IsUndefined(new_target)) {
        obj = js_create_from_ctor(ctx, new_target, JS_CLASS_BOOLEAN);
        if (!JS_IsException(obj))
            JS_SetObjectData(ctx, obj, val);
        return obj;
    } else {
        return val;
    }
}

static JSValue js_thisBooleanValue(JSContext* ctx, JSValueConst this_val)
{
    if (JS_VALUE_GET_TAG(this_val) == JS_TAG_BOOL)
        return JS_DupValue(ctx, this_val);

    if (JS_VALUE_GET_TAG(this_val) == JS_TAG_OBJECT) {
        JSObject* p = JS_VALUE_GET_OBJ(this_val);
        if (p->class_id == JS_CLASS_BOOLEAN) {
            if (JS_VALUE_GET_TAG(p->u.object_data) == JS_TAG_BOOL)
                return p->u.object_data;
        }
    }
    return JS_ThrowTypeError(ctx, "not a boolean");
}

static JSValue js_boolean_toString(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    JSValue val = js_thisBooleanValue(ctx, this_val);
    if (JS_IsException(val))
        return val;
    return JS_AtomToString(ctx, JS_VALUE_GET_BOOL(val) ? JS_ATOM_true : JS_ATOM_false);
}

static JSValue js_boolean_valueOf(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    return js_thisBooleanValue(ctx, this_val);
}

static const JSCFunctionListEntry js_boolean_proto_funcs[] = {
    JS_CFUNC_DEF("toString", 0, js_boolean_toString),
    JS_CFUNC_DEF("valueOf", 0, js_boolean_valueOf),
};
