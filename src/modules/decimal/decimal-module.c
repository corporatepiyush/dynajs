#include "decimal-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_DECIMAL)

#define DEC_DEF_PREC 34

JSClassID dyn_dec_class_id;

void dyn_dec_dispose(void* p)
{
    dec_free(p);
    dyn_nat_free(p);
}

static void dyn_dec_finalizer(JSRuntime* rt, JSValue val)
{
    dec_t* x = (dec_t*)JS_GetOpaque(val, dyn_dec_class_id);
    (void)rt;
    if (x) {
        dec_free(x);
        dyn_nat_free(x);
    }
}

static const JSClassDef dyn_dec_class = {
    "Decimal",
    .finalizer = dyn_dec_finalizer,
};

static const char* const RND_NAMES[] = {
    "up", "down", "ceil", "floor", "halfUp", "halfDown", "halfEven", "halfOdd"
};

int dyn_dec_opts_strict(JSContext* ctx, JSValueConst o)
{
    if (!JS_IsObject(o))
        return 0;
    {
        JSPropertyEnum* props = NULL;
        uint32_t nprops = 0, i;
        int bad = 0;
        if (JS_GetOwnPropertyNames(ctx, &props, &nprops, o,
                JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY))
            return -1;
        for (i = 0; i < nprops && !bad; i++) {
            const char* name = JS_AtomToCString(ctx, props[i].atom);
            if (!name) {
                bad = 1;
                break;
            }
            if (strcmp(name, "precision") != 0 && strcmp(name, "rounding") != 0) {
                JS_ThrowTypeError(ctx,
                    "unknown option \"%s\" (valid: precision, rounding)", name);
                bad = 1;
            }
            JS_FreeCString(ctx, name);
        }
        for (i = 0; i < nprops; i++)
            JS_FreeAtom(ctx, props[i].atom);
        js_free(ctx, props);
        return bad ? -1 : 0;
    }
}

int dyn_rnd_mode(JSContext* ctx, JSValueConst o, int* mode)
{
    JSValue v;
    const char* s;
    size_t k;

    *mode = RND_HALF_EVEN;
    if (JS_IsString(o)) {
        v = JS_DupValue(ctx, o);
    } else if (JS_IsObject(o)) {
        if (dyn_dec_opts_strict(ctx, o))
            return -1;
        v = JS_GetPropertyStr(ctx, o, "rounding");
        if (JS_IsException(v))
            return -1;
        if (JS_IsUndefined(v)) {
            JS_FreeValue(ctx, v);
            return 0;
        }
    } else {
        return 0;
    }
    s = JS_ToCString(ctx, v);
    JS_FreeValue(ctx, v);
    if (!s)
        return -1;
    for (k = 0; k < countof(RND_NAMES); k++)
        if (strcmp(s, RND_NAMES[k]) == 0) {
            *mode = (int)k;
            JS_FreeCString(ctx, s);
            return 0;
        }
    JS_ThrowRangeError(ctx, "rounding must be one of up, down, ceil, floor, "
                            "halfUp, halfDown, halfEven, halfOdd");
    JS_FreeCString(ctx, s);
    return -1;
}

int dyn_prec(JSContext* ctx, JSValueConst o, uint32_t* prec)
{
    JSValue v;
    int64_t p = DEC_DEF_PREC;

    *prec = DEC_DEF_PREC;
    if (!JS_IsObject(o))
        return 0;
    if (dyn_dec_opts_strict(ctx, o))
        return -1;
    v = JS_GetPropertyStr(ctx, o, "precision");
    if (JS_IsException(v))
        return -1;
    if (JS_IsUndefined(v)) {
        JS_FreeValue(ctx, v);
        return 0;
    }
    if (JS_ToInt64(ctx, &p, v) < 0) {
        JS_FreeValue(ctx, v);
        return -1;
    }
    {
        double d;
        if (JS_ToFloat64(ctx, &d, v) < 0) {
            JS_FreeValue(ctx, v);
            return -1;
        }
        if (d != (double)p) {
            JS_FreeValue(ctx, v);
            JS_ThrowTypeError(ctx, "precision must be an integer");
            return -1;
        }
    }
    JS_FreeValue(ctx, v);
    if (p < 1 || p > 5000) {
        JS_ThrowRangeError(ctx, "precision must be 1 to 5000 significant digits");
        return -1;
    }
    *prec = (uint32_t)p;
    return 0;
}

int dyn_dec_from(JSContext* ctx, JSValueConst v, dec_t* out)
{
    const char* s;
    size_t n;
    JSValue sv;
    int rc;

    if (JS_IsObject(v)) {
        dec_t* o = (dec_t*)JS_GetOpaque(v, dyn_dec_class_id);
        if (o) {
            dec_init(out);
            return dec_copy(out, o) < 0 ? -1 : 0;
        }
    }
    if (JS_IsNumber(v)) {
        double d;
        if (JS_ToFloat64(ctx, &d, v) < 0)
            return -1;
        if (d != d || d == DYN_INFINITY || d == -DYN_INFINITY) {
            JS_ThrowRangeError(ctx, "Decimal: %s has no decimal value",
                d != d ? "NaN" : "Infinity");
            return -1;
        }
    } else if (!JS_IsString(v)) {
        JS_ThrowTypeError(ctx, "Decimal: expected a string, a number or a Decimal");
        return -1;
    }
    sv = JS_ToString(ctx, v);
    if (JS_IsException(sv))
        return -1;
    s = JS_ToCStringLen(ctx, &n, sv);
    JS_FreeValue(ctx, sv);
    if (!s)
        return -1;
    rc = dec_parse(out, s, n);
    if (rc < 0)
        JS_ThrowSyntaxError(ctx, "Decimal: not a decimal number: %s", s);
    JS_FreeCString(ctx, s);
    return rc;
}

static JSValue dyn_dec_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv)
{
    dec_t* x;

    if (argc < 1)
        return JS_ThrowTypeError(ctx, "new Decimal(value): a value is required");
    x = dyn_dec_alloc(ctx);
    if (!x)
        return JS_EXCEPTION;
    if (dyn_dec_from(ctx, argv[0], x) < 0) {
        dec_free(x);
        dyn_nat_free(x);
        return JS_EXCEPTION;
    }
    return dyn_dec_wrap(ctx, new_target, x);
}

static JSValue dyn_dec_unary(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    dec_t *a = dyn_dec_this(ctx, this_val), *r;

    (void)argc;
    (void)argv;
    if (!a)
        return JS_EXCEPTION;
    r = dyn_dec_alloc(ctx);
    if (!r)
        return JS_EXCEPTION;
    if (dec_copy(r, a) < 0) {
        dec_free(r);
        dyn_nat_free(r);
        return JS_ThrowOutOfMemory(ctx);
    }
    if (magic == 0)
        r->sign = 0;
    else if (r->nd)
        r->sign = (uint8_t)!r->sign;
    return dyn_dec_wrap(ctx, JS_UNDEFINED, r);
}

static JSValue dyn_dec_cmp(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    dec_t *a = dyn_dec_this(ctx, this_val), b;
    int c;

    if (!a)
        return JS_EXCEPTION;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "Decimal comparison needs an operand");
    if (dyn_dec_from(ctx, argv[0], &b) < 0)
        return JS_EXCEPTION;
    c = dec_cmp(a, &b);
    dec_free(&b);
    return magic ? JS_NewBool(ctx, c == 0) : JS_NewInt32(ctx, c);
}

static JSValue dyn_dec_query(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    dec_t* a = dyn_dec_this(ctx, this_val);
    db_t b;
    JSValue out;

    (void)argc;
    (void)argv;
    if (!a)
        return JS_EXCEPTION;
    if (magic == 2)
        return JS_NewBool(ctx, dec_is_zero(a));
    if (magic == 3)
        return JS_NewInt32(ctx, a->nd == 0 ? 0 : (a->sign ? -1 : 1));
    if (magic == 4)
        return JS_NewUint32(ctx, a->nd);
    db_init(&b);
    dec_write(&b, a);
    if (b.too_long) {
        db_free(&b);
        return dyn_dec_too_long(ctx);
    }
    if (b.oom) {
        db_free(&b);
        return JS_ThrowOutOfMemory(ctx);
    }
    if (magic == 1) {
        JSValue sv = JS_NewStringLen(ctx, (const char*)b.p, b.n);
        double d;
        db_free(&b);
        if (JS_IsException(sv))
            return sv;
        if (JS_ToFloat64(ctx, &d, sv) < 0) {
            JS_FreeValue(ctx, sv);
            return JS_EXCEPTION;
        }
        JS_FreeValue(ctx, sv);
        return JS_NewFloat64(ctx, d);
    }
    out = JS_NewStringLen(ctx, (const char*)b.p, b.n);
    db_free(&b);
    return out;
}

static JSValue dyn_dec_integral(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    dec_t *a = dyn_dec_this(ctx, this_val), t, *r;
    static const int MODES[] = { RND_FLOOR, RND_CEIL, RND_DOWN };

    (void)argc;
    (void)argv;
    if (!a)
        return JS_EXCEPTION;
    dec_init(&t);
    if (dec_copy(&t, a) < 0) {
        dec_free(&t);
        return JS_ThrowOutOfMemory(ctx);
    }
    if (dec_round_at(&t, 0, MODES[magic], 0) < 0) {
        dec_free(&t);
        return JS_ThrowOutOfMemory(ctx);
    }
    r = dyn_dec_alloc(ctx);
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

static JSValue dyn_dec_to_bigint(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dec_t *a = dyn_dec_this(ctx, this_val), t;
    uint64_t v = 0;
    int i;

    (void)argc;
    (void)argv;
    if (!a)
        return JS_EXCEPTION;
    dec_init(&t);
    if (dec_copy(&t, a) < 0 || dec_round_at(&t, 0, RND_DOWN, 0) < 0) {
        dec_free(&t);
        return JS_ThrowOutOfMemory(ctx);
    }
    if (dec_cmp(a, &t) != 0) {
        dec_free(&t);
        return JS_ThrowRangeError(ctx,
            "Decimal.toBigInt: the value has a fractional part");
    }
    if (t.nd > 19 || (t.nd && t.exp > 0 && (uint32_t)t.nd + (uint32_t)t.exp > 19u)) {
        dec_free(&t);
        return JS_ThrowRangeError(ctx,
            "Decimal.toBigInt: the value is outside the int64 range "
            "[-2^63, 2^63) this bridge returns");
    }
    for (i = (int)t.nd - 1; i >= 0; i--)
        v = v * 10 + (uint64_t)t.d[i];
    for (i = 0; i < (int)t.exp; i++)
        v *= 10;
    dec_free(&t);
    if (!a->sign && v > (uint64_t)INT64_MAX) {
        return JS_ThrowRangeError(ctx,
            "Decimal.toBigInt: the value is outside the int64 range "
            "[-2^63, 2^63) this bridge returns");
    }
    if (a->sign && v > (uint64_t)INT64_MAX + 1u) {
        return JS_ThrowRangeError(ctx,
            "Decimal.toBigInt: the value is outside the int64 range "
            "[-2^63, 2^63) this bridge returns");
    }
    return JS_NewBigInt64(ctx, a->sign ? (int64_t)(0u - v) : (int64_t)v);
}

static JSValue dyn_dec_const(JSContext* ctx, const char* text)
{
    dec_t* x = dyn_dec_alloc(ctx);

    if (!x)
        return JS_EXCEPTION;
    if (dec_parse(x, text, strlen(text)) < 0) {
        dec_free(x);
        dyn_nat_free(x);
        return JS_ThrowInternalError(ctx, "decimal: bad constant %s", text);
    }
    return dyn_dec_wrap(ctx, JS_UNDEFINED, x);
}

static const JSCFunctionListEntry dyn_dec_proto[] = {
    JS_CFUNC_MAGIC_DEF("add", 1, dyn_dec_arith, OP_ADD),
    JS_CFUNC_MAGIC_DEF("sub", 1, dyn_dec_arith, OP_SUB),
    JS_CFUNC_MAGIC_DEF("mul", 1, dyn_dec_arith, OP_MUL),
    JS_CFUNC_MAGIC_DEF("div", 1, dyn_dec_arith, OP_DIV),
    JS_CFUNC_MAGIC_DEF("mod", 1, dyn_dec_arith, OP_MOD),
    JS_CFUNC_DEF("divmod", 1, dyn_dec_divmod),
    JS_CFUNC_DEF("pow", 1, dyn_dec_pow),
    JS_CFUNC_MAGIC_DEF("sqrt", 0, dyn_dec_math, 0),
    JS_CFUNC_MAGIC_DEF("exp", 0, dyn_dec_math, 1),
    JS_CFUNC_MAGIC_DEF("ln", 0, dyn_dec_math, 2),
    JS_CFUNC_MAGIC_DEF("log10", 0, dyn_dec_math, 3),
    JS_CFUNC_MAGIC_DEF("floor", 0, dyn_dec_integral, 0),
    JS_CFUNC_MAGIC_DEF("ceil", 0, dyn_dec_integral, 1),
    JS_CFUNC_MAGIC_DEF("trunc", 0, dyn_dec_integral, 2),
    JS_CFUNC_DEF("toBigInt", 0, dyn_dec_to_bigint),
    JS_CFUNC_MAGIC_DEF("abs", 0, dyn_dec_unary, 0),
    JS_CFUNC_MAGIC_DEF("neg", 0, dyn_dec_unary, 1),
    JS_CFUNC_MAGIC_DEF("cmp", 1, dyn_dec_cmp, 0),
    JS_CFUNC_MAGIC_DEF("equals", 1, dyn_dec_cmp, 1),
    JS_CFUNC_MAGIC_DEF("round", 0, dyn_dec_round, 0),
    JS_CFUNC_MAGIC_DEF("toFixed", 0, dyn_dec_round, 1),
    JS_CFUNC_MAGIC_DEF("toString", 0, dyn_dec_query, 0),
    JS_CFUNC_MAGIC_DEF("toJSON", 0, dyn_dec_query, 0),
    JS_CFUNC_MAGIC_DEF("toNumber", 0, dyn_dec_query, 1),
    JS_CFUNC_MAGIC_DEF("isZero", 0, dyn_dec_query, 2),
    JS_CFUNC_MAGIC_DEF("sign", 0, dyn_dec_query, 3),
    JS_CFUNC_MAGIC_DEF("digits", 0, dyn_dec_query, 4),
};

JSClassID dyn_money_class_id;

static const JSClassDef dyn_money_class = {
    "Money",
    .finalizer = dyn_money_finalizer,
};

static const JSCFunctionListEntry dyn_money_proto[] = {
    JS_CFUNC_MAGIC_DEF("add", 1, dyn_money_op, 0),
    JS_CFUNC_MAGIC_DEF("sub", 1, dyn_money_op, 1),
    JS_CFUNC_MAGIC_DEF("cmp", 1, dyn_money_op, 2),
    JS_CFUNC_MAGIC_DEF("equals", 1, dyn_money_op, 3),
    JS_CFUNC_DEF("mul", 1, dyn_money_mul),
    JS_CFUNC_DEF("allocate", 1, dyn_money_allocate),
    JS_CFUNC_MAGIC_DEF("toString", 0, dyn_money_query, 0),
    JS_CFUNC_MAGIC_DEF("toJSON", 0, dyn_money_query, 0),
    JS_CFUNC_MAGIC_DEF("amount", 0, dyn_money_query, 1),
    JS_CFUNC_MAGIC_DEF("currency", 0, dyn_money_query, 2),
    JS_CFUNC_MAGIC_DEF("format", 0, dyn_money_query, 3),
    JS_CFUNC_MAGIC_DEF("toDecimal", 0, dyn_money_query, 4),
};

static const dyn_dec_allocator_t dyn_dec_nat_allocator = {
    dyn_nat_malloc,
    dyn_nat_calloc,
    dyn_nat_realloc,
    dyn_nat_free,
};

static int dyn_decimal_init_module(JSContext* ctx, JSModuleDef* m)
{
    static const struct {
        const char *name, *text;
    } DEC_CONSTS[] = {
        { "ZERO", "0" },
        { "ONE", "1" },
        { "TWO", "2" },
        { "TEN", "10" },
        { "NEG_ONE", "-1" },
    };
    unsigned k;

    dyn_dec_set_allocator(&dyn_dec_nat_allocator);
    if (dyn_register_plain_class(ctx, m, &dyn_dec_class_id, &dyn_dec_class,
            dyn_dec_proto, countof(dyn_dec_proto),
            dyn_dec_ctor, "Decimal")
        < 0)
        return -1;
    {
        JSValue proto = JS_GetClassProto(ctx, dyn_dec_class_id);
        JSValue ctor;
        if (!JS_IsObject(proto))
            return -1;
        ctor = JS_GetPropertyStr(ctx, proto, "constructor");
        JS_FreeValue(ctx, proto);
        if (!JS_IsFunction(ctx, ctor))
            return -1;
        for (k = 0; k < countof(DEC_CONSTS); k++) {
            JSValue v = dyn_dec_const(ctx, DEC_CONSTS[k].text);
            if (JS_IsException(v) || JS_DefinePropertyValueStr(ctx, ctor, DEC_CONSTS[k].name, v, JS_PROP_C_W_E) < 0) {
                JS_FreeValue(ctx, ctor);
                return -1;
            }
        }
        JS_FreeValue(ctx, ctor);
    }
    if (dyn_register_plain_class(ctx, m, &dyn_money_class_id,
            &dyn_money_class, dyn_money_proto,
            countof(dyn_money_proto), dyn_money_ctor,
            "Money")
        < 0)
        return -1;
    {
        JSValue proto = JS_GetClassProto(ctx, dyn_money_class_id);
        JSValue ctor;
        static const struct {
            const char* name;
            JSCFunction* fn;
            int arity;
        } MONEY_STATICS[] = {
            { "fromString", dyn_money_from_string, 2 },
            { "fromDecimal", dyn_money_from_decimal, 2 },
            { "fromMinor", dyn_money_from_minor, 2 },
        };
        unsigned mk;
        if (!JS_IsObject(proto))
            return -1;
        ctor = JS_GetPropertyStr(ctx, proto, "constructor");
        JS_FreeValue(ctx, proto);
        if (!JS_IsFunction(ctx, ctor))
            return -1;
        for (mk = 0; mk < countof(MONEY_STATICS); mk++) {
            JSValue fn = JS_NewCFunction(ctx, MONEY_STATICS[mk].fn,
                MONEY_STATICS[mk].name,
                MONEY_STATICS[mk].arity);
            if (JS_IsException(fn) || JS_DefinePropertyValueStr(ctx, ctor, MONEY_STATICS[mk].name, fn, JS_PROP_C_W_E) < 0) {
                JS_FreeValue(ctx, ctor);
                return -1;
            }
        }
        JS_FreeValue(ctx, ctor);
    }
    return 0;
}

int js_nat_init_decimal(JSContext* ctx)
{
    JSModuleDef* m = JS_NewCModule(ctx, "dyna:decimal", dyn_decimal_init_module);
    if (!m)
        return -1;
    JS_AddModuleExport(ctx, m, "Decimal");
    return JS_AddModuleExport(ctx, m, "Money");
}

#endif
