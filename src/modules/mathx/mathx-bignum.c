#include "mathx-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_MATHX)

int dyn_mathx_bigint_fits_i64(JSContext* ctx, JSValueConst v)
{
    const char *s, *p;
    size_t n;
    int neg, r;

    JSValue sv = JS_ToString(ctx, v);
    if (JS_IsException(sv))
        return -1;
    s = JS_ToCString(ctx, sv);
    JS_FreeValue(ctx, sv);
    if (!s)
        return -1;
    neg = (*s == '-');
    p = neg ? s + 1 : s;
    n = strlen(p);
    if (n < 19)
        r = 1;
    else if (n > 19)
        r = 0;
    else if (*p < '9')
        r = 1;
    else if (strcmp(p, "9223372036854775807") <= 0)
        r = 1;
    else if (neg && strcmp(p, "9223372036854775808") == 0)
        r = 1;
    else
        r = 0;
    JS_FreeCString(ctx, s);
    return r;
}

int dyn_mathx_bigint_mag_u64(JSContext* ctx, JSValueConst v,
    uint64_t* out, int* pneg)
{
    const char *s, *p;
    uint64_t m = 0;
    JSValue sv = JS_ToString(ctx, v);

    if (JS_IsException(sv))
        return -1;
    s = JS_ToCString(ctx, sv);
    JS_FreeValue(ctx, sv);
    if (!s)
        return -1;
    *pneg = (*s == '-');
    p = *pneg ? s + 1 : s;
    if (!*p) {
        JS_FreeCString(ctx, s);
        JS_ThrowTypeError(ctx, "dyna:mathx: expected a BigInt");
        return -1;
    }
    while (*p) {
        unsigned d;
        if (*p < '0' || *p > '9') {
            JS_FreeCString(ctx, s);
            JS_ThrowTypeError(ctx, "dyna:mathx: expected a BigInt");
            return -1;
        }
        d = (unsigned)(*p - '0');
        if (m > (UINT64_MAX - d) / 10) {
            JS_FreeCString(ctx, s);
            JS_ThrowRangeError(ctx,
                "dyna:mathx: BigInt magnitude must be below 2^64");
            return -1;
        }
        m = m * 10 + d;
        p++;
    }
    JS_FreeCString(ctx, s);
    *out = m;
    return 0;
}

int dyn_bigdec_init(JSContext* ctx, DynBigDec* b, uint64_t v)
{
    b->ctx = ctx;
    b->cap = 4;
    b->len = 0;
    b->limb = js_malloc(ctx, b->cap * sizeof(uint32_t));
    if (!b->limb)
        return -1;
    do {
        b->limb[b->len++] = (uint32_t)(v % DYN_MATHX_LIMB_BASE);
        v /= DYN_MATHX_LIMB_BASE;
    } while (v > 0);
    return 0;
}

void dyn_bigdec_free(DynBigDec* b)
{
    js_free(b->ctx, b->limb);
    b->limb = NULL;
}

static int dyn_bigdec_reserve(DynBigDec* b, size_t need)
{
    uint32_t* nl;
    size_t ncap;
    if (need <= b->cap)
        return 0;
    ncap = b->cap * 2;
    if (ncap < need)
        ncap = need;
    nl = js_realloc(b->ctx, b->limb, ncap * sizeof(uint32_t));
    if (!nl)
        return -1;
    b->limb = nl;
    b->cap = ncap;
    return 0;
}

int dyn_bigdec_mul_small(DynBigDec* b, uint32_t factor)
{
    uint64_t carry = 0;
    size_t i;
    for (i = 0; i < b->len; i++) {
        uint64_t cur = (uint64_t)b->limb[i] * factor + carry;
        b->limb[i] = (uint32_t)(cur % DYN_MATHX_LIMB_BASE);
        carry = cur / DYN_MATHX_LIMB_BASE;
    }
    while (carry > 0) {
        if (dyn_bigdec_reserve(b, b->len + 1))
            return -1;
        b->limb[b->len++] = (uint32_t)(carry % DYN_MATHX_LIMB_BASE);
        carry /= DYN_MATHX_LIMB_BASE;
    }
    return 0;
}

#endif
