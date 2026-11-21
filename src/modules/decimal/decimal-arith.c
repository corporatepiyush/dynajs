#include "decimal-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_DECIMAL)

JSValue dyn_dec_arith(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    dec_t *a = dyn_dec_this(ctx, this_val), b, *r;
    uint32_t prec;
    int mode, rc;

    if (!a)
        return JS_EXCEPTION;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "Decimal arithmetic needs an operand");
    if (dyn_dec_from(ctx, argv[0], &b) < 0)
        return JS_EXCEPTION;
    if (dyn_prec(ctx, argc > 1 ? argv[1] : JS_UNDEFINED, &prec) < 0
        || dyn_rnd_mode(ctx, argc > 1 ? argv[1] : JS_UNDEFINED, &mode) < 0) {
        dec_free(&b);
        return JS_EXCEPTION;
    }
    r = dyn_dec_alloc(ctx);
    if (!r) {
        dec_free(&b);
        return JS_EXCEPTION;
    }
    switch (magic) {
    case OP_ADD:
        rc = dec_add(r, a, &b);
        break;
    case OP_SUB: {
        dec_t nb = b;
        nb.sign = (uint8_t)(b.nd ? !b.sign : 0);
        rc = dec_add(r, a, &nb);
        break;
    }
    case OP_MUL:
        rc = dec_mul(r, a, &b);
        break;
    case OP_DIV:
        rc = dec_div(r, a, &b, prec, mode);
        break;
    default:
        rc = dec_mod(r, a, &b);
        break;
    }
    dec_free(&b);
    if (rc == DEC_E_TOOSLOW) {
        dec_free(r);
        dyn_nat_free(r);
        return JS_ThrowRangeError(ctx, "Decimal: operands too large for one "
                                       "exact operation (multiply is limited to %u digit-pairs, divide to "
                                       "%u)",
            (unsigned)DEC_MAX_MUL_CELLS, (unsigned)DEC_MAX_DIV_CELLS);
    }
    if (rc == -2) {
        dec_free(r);
        dyn_nat_free(r);
        return JS_ThrowRangeError(ctx, "Decimal: division by zero");
    }
    if (rc < 0) {
        dec_free(r);
        dyn_nat_free(r);
        return JS_ThrowRangeError(ctx, "Decimal: result exceeds %u digits",
            DEC_MAX_DIGITS);
    }
    return dyn_dec_wrap(ctx, JS_UNDEFINED, r);
}

JSValue dyn_dec_round(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    dec_t *a = dyn_dec_this(ctx, this_val), t;
    int64_t dp = 0;
    int mode;
    double dpd;

    if (!a)
        return JS_EXCEPTION;
    if (argc > 0 && !JS_IsUndefined(argv[0])) {
        if (JS_ToFloat64(ctx, &dpd, argv[0]) < 0)
            return JS_EXCEPTION;
        if (dpd != trunc(dpd))
            return JS_ThrowRangeError(ctx, "Decimal: %s places must be an "
                                           "integer",
                magic ? "toFixed" : "round");
        if (JS_ToInt64(ctx, &dp, argv[0]) < 0)
            return JS_EXCEPTION;
    }
    if (dp < -1000 || dp > 1000)
        return JS_ThrowRangeError(ctx, "Decimal: %s places must be -1000 to 1000",
            magic ? "toFixed" : "round");
    if (dyn_rnd_mode(ctx, argc > 1 ? argv[1] : JS_UNDEFINED, &mode) < 0)
        return JS_EXCEPTION;
    if (magic == 1 && (argc <= 1 || (!JS_IsString(argv[1]) && !JS_IsObject(argv[1]))))
        mode = RND_HALF_UP;
    dec_init(&t);
    if (dec_copy(&t, a) < 0) {
        dec_free(&t);
        return JS_ThrowOutOfMemory(ctx);
    }
    if (dec_round_at(&t, (int32_t)-dp, mode, 0) < 0) {
        dec_free(&t);
        return JS_ThrowOutOfMemory(ctx);
    }
    if (magic == 0) {
        dec_t* r = dyn_dec_alloc(ctx);
        if (!r) {
            dec_free(&t);
            return JS_EXCEPTION;
        }
        if (dec_copy(r, &t) < 0) {
            dec_free(&t);
            dec_free(r);
            dyn_nat_free(r);
            return JS_ThrowOutOfMemory(ctx);
        }
        dec_free(&t);
        return dyn_dec_wrap(ctx, JS_UNDEFINED, r);
    }
    {
        db_t b;
        JSValue out;
        db_init(&b);
        dec_write_fixed(&b, &t, (int32_t)dp, a->sign && (t.nd || a->nd));
        dec_free(&t);
        if (b.too_long) {
            db_free(&b);
            return dyn_dec_too_long(ctx);
        }
        if (b.oom) {
            db_free(&b);
            return JS_ThrowOutOfMemory(ctx);
        }
        out = JS_NewStringLen(ctx, (const char*)b.p, b.n);
        db_free(&b);
        return out;
    }
}

JSValue dyn_dec_divmod(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dec_t *a = dyn_dec_this(ctx, this_val), b, *q, *rr;
    JSValue arr, qv, rv;
    int rc;

    if (!a)
        return JS_EXCEPTION;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "Decimal arithmetic needs an operand");
    if (dyn_dec_opts_strict(ctx, argc > 1 ? argv[1] : JS_UNDEFINED) < 0)
        return JS_EXCEPTION;
    if (dyn_dec_from(ctx, argv[0], &b) < 0)
        return JS_EXCEPTION;
    q = dyn_dec_alloc(ctx);
    rr = dyn_dec_alloc(ctx);
    if (!q || !rr) {
        dec_free(&b);
        dec_free(q);
        dyn_nat_free(q);
        dec_free(rr);
        dyn_nat_free(rr);
        return JS_EXCEPTION;
    }
    rc = dec_int_divmod(q, rr, a, &b);
    dec_free(&b);
    if (rc == -2) {
        dec_free(q);
        dyn_nat_free(q);
        dec_free(rr);
        dyn_nat_free(rr);
        return JS_ThrowRangeError(ctx, "Decimal: division by zero");
    }
    if (rc == DEC_E_TOOSLOW) {
        dec_free(q);
        dyn_nat_free(q);
        dec_free(rr);
        dyn_nat_free(rr);
        return JS_ThrowRangeError(ctx, "Decimal: operands too large for one "
                                       "exact operation (divide is limited to %u cells)",
            (unsigned)DEC_MAX_DIV_CELLS);
    }
    if (rc < 0) {
        dec_free(q);
        dyn_nat_free(q);
        dec_free(rr);
        dyn_nat_free(rr);
        return JS_ThrowRangeError(ctx, "Decimal: result exceeds %u digits",
            DEC_MAX_DIGITS);
    }
    arr = JS_NewArray(ctx);
    if (JS_IsException(arr)) {
        dec_free(q);
        dyn_nat_free(q);
        dec_free(rr);
        dyn_nat_free(rr);
        return arr;
    }
    qv = dyn_dec_wrap(ctx, JS_UNDEFINED, q);
    rv = dyn_dec_wrap(ctx, JS_UNDEFINED, rr);
    if (JS_DefinePropertyValueUint32(ctx, arr, 0, qv, JS_PROP_C_W_E) < 0
        || JS_DefinePropertyValueUint32(ctx, arr, 1, rv, JS_PROP_C_W_E) < 0) {
        JS_FreeValue(ctx, arr);
        return JS_EXCEPTION;
    }
    return arr;
}

#endif
