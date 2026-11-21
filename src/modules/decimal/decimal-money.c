#include "decimal-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_DECIMAL)

typedef struct {
    int64_t amount;
    char code[4];
    uint8_t minor;
} money_t;

static void dyn_money_dispose(void* p)
{
    dyn_nat_free(p);
}

void dyn_money_finalizer(JSRuntime* rt, JSValue val)
{
    (void)rt;
    dyn_nat_free(JS_GetOpaque(val, dyn_money_class_id));
}

static money_t* dyn_money_this(JSContext* ctx, JSValueConst t)
{
    return (money_t*)JS_GetOpaque2(ctx, t, dyn_money_class_id);
}

static JSValue dyn_money_new(JSContext* ctx, JSValueConst new_target,
    int64_t amount, const char* code, uint8_t minor)
{
    money_t* m = (money_t*)dyn_nat_malloc(sizeof *m);

    if (!m)
        return JS_ThrowOutOfMemory(ctx);
    m->amount = amount;
    m->minor = minor;
    memcpy(m->code, code, 3);
    m->code[3] = 0;
    return dyn_plain_wrap(ctx, new_target, dyn_money_class_id, m, dyn_money_dispose);
}

static int money_code(JSContext* ctx, JSValueConst v, char out[4])
{
    const char* code = JS_ToCString(ctx, v);
    int k;

    if (!code)
        return -1;
    if (strlen(code) != 3) {
        JS_FreeCString(ctx, code);
        JS_ThrowRangeError(ctx, "Money: a currency is a 3-letter code");
        return -1;
    }
    for (k = 0; k < 3; k++) {
        char c = code[k];
        if (c >= 'a' && c <= 'z')
            c = (char)(c - 'a' + 'A');
        if (c < 'A' || c > 'Z') {
            JS_FreeCString(ctx, code);
            JS_ThrowRangeError(ctx, "Money: a currency is 3 letters");
            return -1;
        }
        out[k] = c;
    }
    out[3] = 0;
    JS_FreeCString(ctx, code);
    return 0;
}

static int money_minor_opt(JSContext* ctx, JSValueConst opts,
    const char* what, uint8_t base, uint8_t* minor);

static int money_amount(JSContext* ctx, JSValueConst v, int64_t* out)
{
    double d;

    if (!JS_IsNumber(v)) {
        JS_ThrowTypeError(ctx, "Money: minorUnits is an integer count of the "
                               "smallest unit");
        return -1;
    }
    if (JS_ToInt64(ctx, out, v) < 0 || JS_ToFloat64(ctx, &d, v) < 0)
        return -1;
    if (d != (double)*out) {
        JS_ThrowRangeError(ctx, "Money: minorUnits must be an integer -- 1999 "
                                "is $19.99, and a fractional cent is not money");
        return -1;
    }
    return 0;
}

JSValue dyn_money_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv)
{
    int64_t amount;
    char up[4];
    uint8_t minor;

    if (argc < 2 || !JS_IsString(argv[1]))
        return JS_ThrowTypeError(ctx,
            "new Money(minorUnits, currency): a currency code is required");
    if (money_amount(ctx, argv[0], &amount) < 0 || money_code(ctx, argv[1], up) < 0)
        return JS_EXCEPTION;
    if (money_minor_opt(ctx, argc > 2 ? argv[2] : JS_UNDEFINED, "new Money",
            money_minor(up), &minor))
        return JS_EXCEPTION;
    return dyn_money_new(ctx, new_target, amount, up, minor);
}

static int money_pair(JSContext* ctx, JSValueConst a_val, JSValueConst b_val,
    money_t** a, money_t** b)
{
    *a = dyn_money_this(ctx, a_val);
    if (!*a)
        return -1;
    *b = JS_IsObject(b_val) ? (money_t*)JS_GetOpaque(b_val, dyn_money_class_id)
                            : NULL;
    if (!*b) {
        JS_ThrowTypeError(ctx, "Money: the operand must be a Money");
        return -1;
    }
    if (strcmp((*a)->code, (*b)->code) != 0) {
        JS_ThrowTypeError(ctx, "Money: cannot combine %s and %s",
            (*a)->code, (*b)->code);
        return -1;
    }
    if ((*a)->minor != (*b)->minor) {
        JS_ThrowTypeError(ctx, "Money: cannot combine %s amounts with %u and %u "
                               "minor digits; the scales must match",
            (*a)->code, (unsigned)(*a)->minor,
            (unsigned)(*b)->minor);
        return -1;
    }
    return 0;
}

JSValue dyn_money_op(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    money_t *a, *b;
    int64_t r;

    if (argc < 1)
        return JS_ThrowTypeError(ctx, "Money: this operation needs one argument");
    if (money_pair(ctx, this_val, argv[0], &a, &b) < 0)
        return JS_EXCEPTION;
    if (magic == 2)
        return JS_NewInt32(ctx, a->amount < b->amount ? -1 : a->amount > b->amount);
    if (magic == 3)
        return JS_NewBool(ctx, a->amount == b->amount);
    r = magic == 0 ? a->amount + b->amount : a->amount - b->amount;
    if ((magic == 0 && ((b->amount > 0 && r < a->amount) || (b->amount < 0 && r > a->amount)))
        || (magic == 1 && ((b->amount < 0 && r < a->amount) || (b->amount > 0 && r > a->amount))))
        return JS_ThrowRangeError(ctx, "Money: the amount overflows an int64");
    return dyn_money_new(ctx, JS_UNDEFINED, r, a->code, a->minor);
}

JSValue dyn_money_mul(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    money_t* a = dyn_money_this(ctx, this_val);
    int64_t k;
    double d;

    if (!a)
        return JS_EXCEPTION;
    if (argc < 1 || JS_ToInt64(ctx, &k, argv[0]) < 0)
        return JS_ThrowTypeError(ctx, "Money.mul(n): n must be an integer");
    if (JS_ToFloat64(ctx, &d, argv[0]) < 0)
        return JS_EXCEPTION;
    if (d != (double)k)
        return JS_ThrowRangeError(ctx, "Money.mul(n): n must be an integer -- "
                                       "use allocate() to split an amount");
    if (k != 0 && ((__int128)a->amount * (__int128)k > (__int128)INT64_MAX || (__int128)a->amount * (__int128)k < (__int128)INT64_MIN))
        return JS_ThrowRangeError(ctx, "Money: the amount overflows an int64");
    return dyn_money_new(ctx, JS_UNDEFINED, a->amount * k, a->code, a->minor);
}

static int64_t* money_weights(JSContext* ctx, JSValueConst arr, int64_t n,
    int64_t* total)
{
    int64_t *w = (int64_t*)dyn_nat_calloc((size_t)n, sizeof *w), i;

    *total = 0;
    if (!w) {
        JS_ThrowOutOfMemory(ctx);
        return NULL;
    }
    {
        __int128 sum = 0;
        for (i = 0; i < n; i++) {
            JSValue e = JS_GetPropertyUint32(ctx, arr, (uint32_t)i);
            double d;
            if (JS_IsException(e) || JS_ToFloat64(ctx, &d, e) < 0) {
                JS_FreeValue(ctx, e);
                dyn_nat_free(w);
                return NULL;
            }
            JS_FreeValue(ctx, e);
            if (d >= 9223372036854775808.0) {
                dyn_nat_free(w);
                JS_ThrowRangeError(ctx,
                    "Money.allocate: the weights overflow an int64");
                return NULL;
            }
            if (!(d >= 0) || d != (double)(int64_t)d) {
                dyn_nat_free(w);
                JS_ThrowRangeError(ctx,
                    "Money.allocate: every share is a non-negative integer weight");
                return NULL;
            }
            w[i] = (int64_t)d;
            sum += (__int128)w[i];
            if (sum > (__int128)INT64_MAX) {
                dyn_nat_free(w);
                JS_ThrowRangeError(ctx,
                    "Money.allocate: the weights overflow an int64");
                return NULL;
            }
        }
        *total = (int64_t)sum;
    }
    if (*total == 0) {
        dyn_nat_free(w);
        JS_ThrowRangeError(ctx, "Money.allocate: the weights sum to zero");
        return NULL;
    }
    return w;
}

JSValue dyn_money_allocate(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    money_t* a = dyn_money_this(ctx, this_val);
    JSValue lv, out;
    int64_t n = 0, total = 0, given = 0, i, abs_amount, rem;
    int64_t* w;
    int neg;

    if (!a)
        return JS_EXCEPTION;
    if (argc < 1 || JS_IsArray(ctx, argv[0]) != 1)
        return JS_ThrowTypeError(ctx, "Money.allocate(shares): shares is an array");
    lv = JS_GetPropertyStr(ctx, argv[0], "length");
    if (JS_IsException(lv) || JS_ToInt64(ctx, &n, lv) < 0) {
        JS_FreeValue(ctx, lv);
        return JS_EXCEPTION;
    }
    JS_FreeValue(ctx, lv);
    if (n < 1 || n > 100000)
        return JS_ThrowRangeError(ctx, "Money.allocate: 1 to 100000 shares");
    w = money_weights(ctx, argv[0], n, &total);
    if (!w)
        return JS_EXCEPTION;
    if (a->amount == INT64_MIN) {
        dyn_nat_free(w);
        return JS_ThrowRangeError(ctx, "Money: the amount overflows an int64");
    }
    neg = a->amount < 0;
    abs_amount = neg ? -a->amount : a->amount;
    for (i = 0; i < n; i++) {
        int64_t part = abs_amount % total, share;
        if (w[i] && (part > INT64_MAX / w[i] || abs_amount / total > INT64_MAX / w[i])) {
            dyn_nat_free(w);
            return JS_ThrowRangeError(ctx,
                "Money.allocate: the weights overflow an int64");
        }
        share = abs_amount / total * w[i] + part * w[i] / total;
        given += share;
        w[i] = share;
    }
    for (i = 0, rem = abs_amount - given; i < n && rem > 0; i++, rem--)
        w[i]++;
    out = JS_NewArray(ctx);
    if (JS_IsException(out)) {
        dyn_nat_free(w);
        return out;
    }
    for (i = 0; i < n; i++) {
        JSValue m = dyn_money_new(ctx, JS_UNDEFINED, neg ? -w[i] : w[i], a->code, a->minor);
        if (JS_IsException(m)
            || JS_DefinePropertyValueUint32(ctx, out, (uint32_t)i, m,
                   JS_PROP_C_W_E)
                < 0) {
            dyn_nat_free(w);
            JS_FreeValue(ctx, out);
            return JS_EXCEPTION;
        }
    }
    dyn_nat_free(w);
    return out;
}

static size_t money_fmt_i64(char* p, int64_t v)
{
    char t[24];
    size_t n = 0, k = 0;
    uint64_t u = v < 0 ? (uint64_t)0 - (uint64_t)v : (uint64_t)v;
    do {
        t[n++] = (char)('0' + (int)(u % 10u));
        u /= 10u;
    } while (u);
    if (v < 0)
        p[k++] = '-';
    while (n)
        p[k++] = t[--n];
    return k;
}

JSValue dyn_money_query(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    money_t* a = dyn_money_this(ctx, this_val);
    char buf[64];
    size_t bl = 0;
    int64_t whole, frac, div = 1;
    int k;

    (void)argc;
    (void)argv;
    if (!a)
        return JS_EXCEPTION;
    if (magic == 1)
        return JS_NewInt64(ctx, a->amount);
    if (magic == 2)
        return JS_NewString(ctx, a->code);
    for (k = 0; k < a->minor; k++)
        div *= 10;
    whole = a->amount / div;
    frac = a->amount % div;
    if (frac < 0)
        frac = -frac;
    if (a->minor != 0 && a->amount < 0 && whole == 0)
        buf[bl++] = '-';
    bl += money_fmt_i64(buf + bl, whole);
    if (a->minor != 0) {
        uint64_t f = (uint64_t)frac;
        buf[bl++] = '.';
        for (k = a->minor; k-- > 0; f /= 10u)
            buf[bl + (size_t)k] = (char)('0' + (int)(f % 10u));
        bl += a->minor;
    }
    buf[bl] = 0;
    if (magic == 3) {
        static const struct {
            const char *code, *sym;
        } SYM[] = {
            { "USD", "$" },
            { "EUR", "\xE2\x82\xAC" },
            { "GBP", "\xC2\xA3" },
            { "JPY", "\xC2\xA5" },
            { "CNY", "\xC2\xA5" },
            { "INR", "\xE2\x82\xB9" },
            { "KRW", "\xE2\x82\xA9" },
            { "CAD", "CA$" },
            { "AUD", "A$" },
        };
        char out[96];
        size_t si;
        for (si = 0; si < countof(SYM); si++)
            if (strcmp(SYM[si].code, a->code) == 0) {
                size_t ol = 0, sl = strlen(SYM[si].sym), skip = buf[0] == '-';
                if (a->amount < 0)
                    out[ol++] = '-';
                memcpy(out + ol, SYM[si].sym, sl);
                ol += sl;
                memcpy(out + ol, buf + skip, bl - skip);
                ol += bl - skip;
                return JS_NewStringLen(ctx, out, ol);
            }
        {
            size_t cl = strlen(a->code);
            memcpy(out, buf, bl);
            out[bl] = ' ';
            memcpy(out + bl + 1, a->code, cl);
            return JS_NewStringLen(ctx, out, bl + 1 + cl);
        }
    }
    if (magic == 4) {
        dec_t* r = dyn_dec_alloc(ctx);
        if (!r)
            return JS_EXCEPTION;
        if (dec_parse(r, buf, strlen(buf)) < 0) {
            dec_free(r);
            dyn_nat_free(r);
            return JS_ThrowInternalError(ctx, "Money.toDecimal failed");
        }
        return dyn_dec_wrap(ctx, JS_UNDEFINED, r);
    }
    return JS_NewString(ctx, buf);
}

static int money_opts_strict(JSContext* ctx, JSValueConst o)
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
            if (strcmp(name, "minorDigits") != 0) {
                JS_ThrowTypeError(ctx,
                    "unknown option \"%s\" (valid: minorDigits)", name);
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

static int money_minor_opt(JSContext* ctx, JSValueConst opts,
    const char* what, uint8_t base, uint8_t* minor)
{
    JSValue v;
    int64_t md;
    double mdd;

    *minor = base;
    if (JS_IsUndefined(opts) || JS_IsNull(opts))
        return 0;
    if (!JS_IsObject(opts)) {
        JS_ThrowTypeError(ctx, "%s: options must be an object", what);
        return -1;
    }
    if (money_opts_strict(ctx, opts))
        return -1;
    v = JS_GetPropertyStr(ctx, opts, "minorDigits");
    if (JS_IsException(v))
        return -1;
    if (!JS_IsUndefined(v)) {
        if (JS_ToFloat64(ctx, &mdd, v) < 0
            || JS_ToInt64(ctx, &md, v) < 0 || mdd != (double)md) {
            JS_FreeValue(ctx, v);
            JS_ThrowRangeError(ctx, "%s: minorDigits must be an integer", what);
            return -1;
        }
        if (md < 0 || md > 6) {
            JS_FreeValue(ctx, v);
            JS_ThrowRangeError(ctx, "%s: minorDigits is 0 to 6", what);
            return -1;
        }
        *minor = (uint8_t)md;
    }
    JS_FreeValue(ctx, v);
    return 0;
}

static int money_parse_decimal_str(JSContext* ctx, const char* s, size_t n,
    const char* what, uint8_t minor,
    int64_t* out)
{
    size_t i = 0;
    int neg = 0, nfrac = 0, comma = 0, seen_dot = 0, seen_digit = 0;
    __int128 acc = 0, frac = 0, val;

    if (n == 0) {
        JS_ThrowTypeError(ctx, "%s: the amount string is empty", what);
        return -1;
    }
    if (s[0] == '-') {
        neg = 1;
        i = 1;
    }
    for (; i < n; i++) {
        char c = s[i];
        if (c == '.') {
            if (seen_dot || nfrac == -1) {
                JS_ThrowTypeError(ctx,
                    "%s: more than one decimal point in \"%s\"", what, s);
                return -1;
            }
            if (!seen_digit) {
                JS_ThrowTypeError(ctx,
                    "%s: \"%s\" needs a digit before the decimal point", what, s);
                return -1;
            }
            seen_dot = 1;
            nfrac = -1;
            continue;
        }
        if (c == ',') {
            if (seen_dot) {
                JS_ThrowTypeError(ctx,
                    "%s: a group separator after the decimal point in \"%s\"",
                    what, s);
                return -1;
            }
            comma = 1;
            continue;
        }
        if (c < '0' || c > '9') {
            JS_ThrowTypeError(ctx,
                "%s: \"%s\" is not a decimal amount", what, s);
            return -1;
        }
        if (seen_dot) {
            if (nfrac < 0)
                nfrac = 0;
            nfrac++;
            if (nfrac <= (int)minor) {
                frac = frac * 10 + (c - '0');
            } else if (c != '0') {
                JS_ThrowRangeError(ctx,
                    "%s: the amount has value past the %u minor digits -- "
                    "a fractional minor unit is not money",
                    what,
                    (unsigned)minor);
                return -1;
            }
        } else {
            seen_digit = 1;
            acc = acc * 10 + (c - '0');
            if (acc > (__int128)INT64_MAX + 1) {
                JS_ThrowRangeError(ctx, "Money: the amount overflows an int64");
                return -1;
            }
        }
    }
    if (nfrac == -1) {
        JS_ThrowTypeError(ctx, "%s: \"%s\" has a decimal point with no "
                               "fraction digits",
            what, s);
        return -1;
    }
    if (!seen_digit) {
        JS_ThrowTypeError(ctx, "%s: the amount string has no digits", what);
        return -1;
    }
    if (comma) {
        size_t g = 0, glen = 0, k;
        for (k = (s[0] == '-' ? 1 : 0); k < n && s[k] != '.'; k++) {
            if (s[k] == ',') {
                if (glen == 0 || glen > 3 || (g && glen != 3)) {
                    JS_ThrowTypeError(ctx,
                        "%s: \"%s\" has malformed ',' grouping", what, s);
                    return -1;
                }
                g++;
                glen = 0;
            } else {
                glen++;
            }
        }
        if (glen == 0 || glen > 3 || (g && glen != 3)) {
            JS_ThrowTypeError(ctx,
                "%s: \"%s\" has malformed ',' grouping", what, s);
            return -1;
        }
    }
    {
        int k;
        for (k = 0; k < (int)minor; k++)
            acc *= 10;
        for (k = nfrac; k < (int)minor; k++)
            frac *= 10;
    }
    val = acc + frac;
    if (neg) {
        if (val > (__int128)INT64_MAX + 1) {
            JS_ThrowRangeError(ctx, "Money: the amount overflows an int64");
            return -1;
        }
        *out = (int64_t)-val;
    } else {
        if (val > (__int128)INT64_MAX) {
            JS_ThrowRangeError(ctx, "Money: the amount overflows an int64");
            return -1;
        }
        *out = (int64_t)val;
    }
    return 0;
}

static int money_factory_args(JSContext* ctx, JSValueConst csv,
    JSValueConst optsv, const char* what,
    char up[4], uint8_t* minor)
{
    if (!JS_IsString(csv)) {
        JS_ThrowTypeError(ctx,
            "%s: a 3-letter currency code is required", what);
        return -1;
    }
    if (money_code(ctx, csv, up) < 0)
        return -1;
    return money_minor_opt(ctx, optsv, what, money_minor(up), minor);
}

JSValue dyn_money_from_string(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    char up[4];
    uint8_t minor;
    const char* s;
    size_t n;
    int64_t amount;
    (void)this_val;

    if (argc < 2 || !JS_IsString(argv[0]))
        return JS_ThrowTypeError(ctx,
            "Money.fromString(str, currency[, opts])");
    if (money_factory_args(ctx, argv[1], argc > 2 ? argv[2] : JS_UNDEFINED,
            "Money.fromString", up, &minor))
        return JS_EXCEPTION;
    s = JS_ToCStringLen(ctx, &n, argv[0]);
    if (!s)
        return JS_EXCEPTION;
    if (money_parse_decimal_str(ctx, s, n, "Money.fromString", minor, &amount)) {
        JS_FreeCString(ctx, s);
        return JS_EXCEPTION;
    }
    JS_FreeCString(ctx, s);
    return dyn_money_new(ctx, JS_UNDEFINED, amount, up, minor);
}

JSValue dyn_money_from_decimal(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    char up[4];
    uint8_t minor;
    dec_t* x;
    __int128 acc = 0;
    int32_t p, top;
    int64_t amount;
    (void)this_val;

    if (argc < 2 || !JS_IsObject(argv[0]))
        return JS_ThrowTypeError(ctx,
            "Money.fromDecimal(d, currency[, opts]): d must be a Decimal");
    x = (dec_t*)JS_GetOpaque(argv[0], dyn_dec_class_id);
    if (!x)
        return JS_ThrowTypeError(ctx,
            "Money.fromDecimal(d, currency[, opts]): d must be a Decimal");
    if (money_factory_args(ctx, argv[1], argc > 2 ? argv[2] : JS_UNDEFINED,
            "Money.fromDecimal", up, &minor))
        return JS_EXCEPTION;
    if (dec_is_zero(x)) {
        amount = 0;
    } else {
        top = x->exp + (int32_t)x->nd - 1;
        for (p = x->exp; p < -(int32_t)minor && p <= top; p++)
            if (dec_at(x, p) != 0)
                return JS_ThrowRangeError(ctx,
                    "Money.fromDecimal: the Decimal has fraction digits past "
                    "the %u minor digits -- a fractional minor unit is not "
                    "money",
                    (unsigned)minor);
        if (top >= 19)
            return JS_ThrowRangeError(ctx, "Money: the amount overflows an int64");
        for (p = top; p >= -(int32_t)minor; p--) {
            acc = acc * 10 + dec_at(x, p);
            if (acc > (__int128)INT64_MAX + 1)
                return JS_ThrowRangeError(ctx, "Money: the amount overflows an int64");
        }
        if (x->sign) {
            if (acc > (__int128)INT64_MAX + 1)
                return JS_ThrowRangeError(ctx, "Money: the amount overflows an int64");
            amount = (int64_t)-acc;
        } else {
            if (acc > (__int128)INT64_MAX)
                return JS_ThrowRangeError(ctx, "Money: the amount overflows an int64");
            amount = (int64_t)acc;
        }
    }
    return dyn_money_new(ctx, JS_UNDEFINED, amount, up, minor);
}

JSValue dyn_money_from_minor(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    char up[4];
    uint8_t minor;
    int64_t amount;
    (void)this_val;

    if (argc < 2)
        return JS_ThrowTypeError(ctx,
            "Money.fromMinor(n, currency[, opts])");
    if (money_factory_args(ctx, argv[1], argc > 2 ? argv[2] : JS_UNDEFINED,
            "Money.fromMinor", up, &minor))
        return JS_EXCEPTION;
    if (JS_IsString(argv[0])) {
        const char* s;
        size_t n;
        s = JS_ToCStringLen(ctx, &n, argv[0]);
        if (!s)
            return JS_EXCEPTION;
        if (money_parse_decimal_str(ctx, s, n, "Money.fromMinor", 0, &amount)) {
            JS_FreeCString(ctx, s);
            return JS_EXCEPTION;
        }
        JS_FreeCString(ctx, s);
    } else if (JS_IsBigInt(ctx, argv[0])) {
        const char* s;
        size_t n, start = 0;
        int neg = 0, bad = 0;
        s = JS_ToCStringLen(ctx, &n, argv[0]);
        if (!s)
            return JS_EXCEPTION;
        if (n > 0 && s[0] == '-') {
            neg = 1;
            start = 1;
        }
        while (start < n && s[start] == '0')
            start++;
        if (n - start > 19)
            bad = 1;
        else if (n - start == 19) {
            const char* lim = neg ? "9223372036854775808" : "9223372036854775807";
            bad = memcmp(s + start, lim, 19) > 0;
        }
        JS_FreeCString(ctx, s);
        if (bad)
            return JS_ThrowRangeError(ctx, "Money: the amount overflows an int64");
        if (JS_ToBigInt64(ctx, &amount, argv[0]))
            return JS_EXCEPTION;
    } else if (JS_IsNumber(argv[0])) {
        if (money_amount(ctx, argv[0], &amount))
            return JS_EXCEPTION;
    } else {
        return JS_ThrowTypeError(ctx,
            "Money.fromMinor(n): n must be a number, bigint or digit string");
    }
    return dyn_money_new(ctx, JS_UNDEFINED, amount, up, minor);
}

#endif
