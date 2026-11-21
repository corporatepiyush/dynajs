#include "decimal-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_DECIMAL)

JSValue dyn_dec_pow(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dec_t *a = dyn_dec_this(ctx, this_val), acc, base, *r;
    int64_t e = 0;
    uint32_t prec;
    int mode, rc, neg;
    double ed;

    if (!a)
        return JS_EXCEPTION;
    if (argc < 1 || JS_ToFloat64(ctx, &ed, argv[0]) < 0)
        return JS_ThrowTypeError(ctx, "Decimal.pow(n): n must be an integer");
    if (ed != trunc(ed))
        return JS_ThrowRangeError(ctx, "Decimal.pow(n): n must be an integer");
    if (JS_ToInt64(ctx, &e, argv[0]) < 0)
        return JS_ThrowTypeError(ctx, "Decimal.pow(n): n must be an integer");
    if (e < -10000 || e > 10000)
        return JS_ThrowRangeError(ctx, "Decimal.pow(n): |n| must be at most 10000");
    if (dyn_prec(ctx, argc > 1 ? argv[1] : JS_UNDEFINED, &prec) < 0
        || dyn_rnd_mode(ctx, argc > 1 ? argv[1] : JS_UNDEFINED, &mode) < 0)
        return JS_EXCEPTION;
    neg = e < 0;
    dec_init(&acc);
    dec_init(&base);
    rc = dec_copy(&base, a);
    if (rc == 0)
        rc = dec_pow_uint(&acc, &base, (uint64_t)(neg ? -e : e));
    if (rc == 0 && neg) {
        dec_t one, t;
        dec_init(&one);
        dec_init(&t);
        rc = dec_grow(&one, 1);
        if (rc == 0) {
            one.d[0] = 1;
            one.nd = 1;
            rc = dec_div(&t, &one, &acc, prec, mode);
            if (rc == 0)
                rc = dec_copy(&acc, &t);
        }
        dec_free(&one);
        dec_free(&t);
    }
    dec_free(&base);
    if (rc == -2) {
        dec_free(&acc);
        return JS_ThrowRangeError(ctx, "Decimal: division by zero");
    }
    if (rc == DEC_E_TOOSLOW) {
        dec_free(&acc);
        return JS_ThrowRangeError(ctx, "Decimal: pow exceeds the operation "
                                       "budget (multiply is limited to %u digit-pairs, divide to %u)",
            (unsigned)DEC_MAX_MUL_CELLS, (unsigned)DEC_MAX_DIV_CELLS);
    }
    if (rc < 0) {
        dec_free(&acc);
        return JS_ThrowRangeError(ctx, "Decimal: result exceeds %u digits",
            DEC_MAX_DIGITS);
    }
    r = dyn_dec_alloc(ctx);
    if (!r) {
        dec_free(&acc);
        return JS_EXCEPTION;
    }
    if (dec_copy(r, &acc) < 0) {
        dec_free(&acc);
        dec_free(r);
        dyn_nat_free(r);
        return JS_ThrowOutOfMemory(ctx);
    }
    dec_free(&acc);
    return dyn_dec_wrap(ctx, JS_UNDEFINED, r);
}

JSValue dyn_dec_math(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    dec_t *a = dyn_dec_this(ctx, this_val), *r;
    uint32_t prec;
    int mode, rc;
    static const char* const NAMES[] = { "sqrt", "exp", "ln", "log10" };

    if (!a)
        return JS_EXCEPTION;
    if (dyn_prec(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, &prec) < 0
        || dyn_rnd_mode(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, &mode) < 0)
        return JS_EXCEPTION;
    if ((magic == 2 || magic == 3) && dec_is_zero(a))
        return JS_ThrowRangeError(ctx,
            "Decimal.%s: the logarithm of zero is -Infinity, and this module "
            "has no Infinity -- it would not round-trip",
            NAMES[magic]);
    r = dyn_dec_alloc(ctx);
    if (!r)
        return JS_EXCEPTION;
    switch (magic) {
    case 0:
        rc = dec_sqrt(r, a, prec);
        break;
    case 1:
        rc = dec_exp(r, a, prec);
        break;
    case 2:
        rc = dec_ln(r, a, prec);
        break;
    default:
        rc = dec_log10(r, a, prec);
        break;
    }
    if (rc == DEC_E_INVALID) {
        dec_free(r);
        dyn_nat_free(r);
        return JS_ThrowRangeError(ctx,
            magic == 0
                ? "Decimal.sqrt: the square root of a negative number is not "
                  "a decimal"
                : "Decimal.%s: the logarithm of a negative number is not a "
                  "decimal",
            NAMES[magic]);
    }
    if (rc == DEC_E_OVERFLOW) {
        dec_free(r);
        dyn_nat_free(r);
        return JS_ThrowRangeError(ctx,
            "Decimal.exp: e^x overflows the context (the adjusted exponent "
            "passes 999999, the same Emax python decimal's default context "
            "sets); exp of a very negative x is 0, not an error");
    }
    if (rc == DEC_E_BUDGET) {
        dec_free(r);
        dyn_nat_free(r);
        return JS_ThrowRangeError(ctx,
            "Decimal.%s: exceeds the operation budget at this precision (the "
            "series core is schoolbook; sqrt has no such cap)",
            NAMES[magic]);
    }
    if (rc < 0) {
        dec_free(r);
        dyn_nat_free(r);
        return JS_ThrowRangeError(ctx, "Decimal: result exceeds %u digits",
            DEC_MAX_DIGITS);
    }
    return dyn_dec_wrap(ctx, JS_UNDEFINED, r);
}

#endif
