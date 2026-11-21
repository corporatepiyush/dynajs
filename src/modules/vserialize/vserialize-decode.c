#include "vserialize-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_VSERIALIZE)

#define VDEC_TPL_MAX 32

static int vdec_need(vdec_t* d, size_t k)
{
    if (d->i + k > d->n) {
        JS_ThrowSyntaxError(d->ctx, "decode: truncated at byte %u",
            (unsigned)d->i);
        return -1;
    }
    return 0;
}

static uint64_t vdec_be(vdec_t* d, int bytes)
{
    uint64_t v = 0;
    int k;
    for (k = 0; k < bytes; k++)
        v = (v << 8) | d->p[d->i + (size_t)k];
    d->i += (size_t)bytes;
    return v;
}

static int vdec_bounded(vdec_t* d, uint64_t count, size_t per)
{
    if (count > VS_MAX_ITEMS || (per && count > (uint64_t)(d->n - d->i) / per)) {
        JS_ThrowRangeError(d->ctx, "decode: declared length %llu exceeds the "
                                   "remaining input",
            (unsigned long long)count);
        return -1;
    }
    return 0;
}

static JSValue vdec_i64(vdec_t* d, int64_t v)
{
    if (d->bigint && (v > 9007199254740991LL || v < -9007199254740991LL))
        return JS_NewBigInt64(d->ctx, v);
    return JS_NewInt64(d->ctx, v);
}

static JSValue vdec_u64(vdec_t* d, uint64_t v)
{
    if (v > 9007199254740991ULL)
        return d->bigint ? JS_NewBigUint64(d->ctx, v) : JS_NewFloat64(d->ctx, (double)v);
    return JS_NewInt64(d->ctx, (int64_t)v);
}

static JSValue vdec_neg_big(vdec_t* d, uint64_t arg)
{
    char buf[32];
    JSValue g, f, s, r;
    if (!d->bigint)
        return JS_NewFloat64(d->ctx, arg == UINT64_MAX ? -18446744073709551616.0 : -(double)(arg + 1));
    if (arg == UINT64_MAX)
        snprintf(buf, sizeof buf, "-18446744073709551616");
    else
        snprintf(buf, sizeof buf, "-%llu", (unsigned long long)arg + 1);
    g = JS_GetGlobalObject(d->ctx);
    f = JS_GetPropertyStr(d->ctx, g, "BigInt");
    JS_FreeValue(d->ctx, g);
    if (JS_IsException(f))
        return f;
    s = JS_NewString(d->ctx, buf);
    r = JS_IsException(s) ? s : JS_Call(d->ctx, f, JS_UNDEFINED, 1, (JSValueConst*)&s);
    JS_FreeValue(d->ctx, s);
    JS_FreeValue(d->ctx, f);
    return r;
}


static JSValue vdec_bignum(vdec_t* d, int neg)
{
    static const char hx[] = "0123456789abcdef";
    uint8_t b;
    uint64_t len = 0;
    const uint8_t* p;
    size_t k, n, o;
    char* s;
    JSValue g, ctor, sv, r;
    if (vdec_need(d, 1) < 0)
        return JS_EXCEPTION;
    b = d->p[d->i];
    if ((b >> 5) != 2 || (b & 31) > 27)
        return JS_ThrowSyntaxError(d->ctx, "decode: tag %d must wrap a definite byte string", neg ? 3 : 2);
    d->i++;
    if ((b & 31) < 24) {
        len = b & 31;
    } else {
        int bytes = 1 << ((b & 31) - 24);
        if (vdec_need(d, (size_t)bytes) < 0)
            return JS_EXCEPTION;
        len = vdec_be(d, bytes);
    }
    if (len > VS_BIGNUM_MAX_BYTES)
        return JS_ThrowRangeError(d->ctx, "decode: bignum over %d bytes", VS_BIGNUM_MAX_BYTES);
    if (vdec_need(d, (size_t)len) < 0)
        return JS_EXCEPTION;
    p = d->p + d->i;
    d->i += (size_t)len;
    for (k = 0; k < len && !p[k]; k++)
        ;
    n = (size_t)len - k;
    if (n <= 8) {
        uint64_t u = 0;
        for (; k < len; k++)
            u = (u << 8) | p[k];
        if (!neg)
            return vdec_u64(d, u);
        if (u > (uint64_t)INT64_MAX)
            return vdec_neg_big(d, u);
        return vdec_i64(d, -1 - (int64_t)u);
    }
    s = (char*)js_malloc(d->ctx, 2 * n + 5);
    if (!s)
        return JS_EXCEPTION;
    o = 0;
    s[o++] = '0';
    s[o++] = 'x';
    if (neg) {
        s[o++] = 'f';
        s[o++] = 'f';
    }
    for (; k < len; k++) {
        uint8_t c = neg ? (uint8_t)~p[k] : p[k];
        s[o++] = hx[c >> 4];
        s[o++] = hx[c & 15];
    }
    g = JS_GetGlobalObject(d->ctx);
    ctor = JS_GetPropertyStr(d->ctx, g, "BigInt");
    JS_FreeValue(d->ctx, g);
    sv = JS_NewStringLen(d->ctx, s, o);
    js_free(d->ctx, s);
    if (JS_IsException(ctor) || JS_IsException(sv)) {
        JS_FreeValue(d->ctx, ctor);
        JS_FreeValue(d->ctx, sv);
        return JS_EXCEPTION;
    }
    r = JS_Call(d->ctx, ctor, JS_UNDEFINED, 1, (JSValueConst*)&sv);
    JS_FreeValue(d->ctx, sv);
    if (neg && !JS_IsException(r)) {
        JSValue as = JS_GetPropertyStr(d->ctx, ctor, "asIntN"), args[2];
        args[0] = JS_NewInt64(d->ctx, (int64_t)(8 * (n + 1)));
        args[1] = r;
        r = JS_IsException(as) ? as : JS_Call(d->ctx, as, ctor, 2, (JSValueConst*)args);
        JS_FreeValue(d->ctx, as);
        JS_FreeValue(d->ctx, args[1]);
    }
    JS_FreeValue(d->ctx, ctor);
    if (!d->bigint)
        r = dyn_bigint_to_number(d->ctx, r);
    return r;
}

static JSValue vdec_str(vdec_t* d, uint64_t len)
{
    JSValue v;
    if (vdec_bounded(d, len, 1) < 0 || vdec_need(d, (size_t)len) < 0)
        return JS_EXCEPTION;
    v = dyn_nat_new_string_wf(d->ctx, (const char*)d->p + d->i, (size_t)len);
    d->i += (size_t)len;
    return v;
}

static JSValue vdec_bin(vdec_t* d, uint64_t len)
{
    static const uint8_t zero = 0;
    JSValueConst ta[3];
    JSValue ab, r;

    if (vdec_bounded(d, len, 1) < 0 || vdec_need(d, (size_t)len) < 0)
        return JS_EXCEPTION;
    ab = JS_NewArrayBufferCopy(d->ctx, len ? d->p + d->i : &zero, (size_t)len);
    d->i += (size_t)len;
    if (JS_IsException(ab))
        return ab;
    ta[0] = ab;
    ta[1] = JS_UNDEFINED;
    ta[2] = JS_UNDEFINED;
    r = JS_NewTypedArray(d->ctx, 3, ta, JS_TYPED_ARRAY_UINT8);
    JS_FreeValue(d->ctx, ab);
    return r;
}

static JSValue vdec_array(vdec_t* d, uint64_t count)
{
    JSValue out;
    uint64_t k;

    if (vdec_bounded(d, count, 1) < 0)
        return JS_EXCEPTION;
    out = JS_NewArray(d->ctx);
    if (JS_IsException(out))
        return out;
    d->depth++;
    for (k = 0; k < count; k++) {
        JSValue el = vdec_value(d);
        if (JS_IsException(el)
            || JS_DefinePropertyValueUint32(d->ctx, out, (uint32_t)k, el,
                   JS_PROP_C_W_E)
                < 0) {
            d->depth--;
            JS_FreeValue(d->ctx, out);
            return JS_EXCEPTION;
        }
    }
    d->depth--;
    return out;
}

static int vdec_key(vdec_t* d, JSAtom* out)
{
    uint64_t len;
    uint8_t b;

    if (d->i >= d->n)
        goto slow;
    b = d->p[d->i];
    if (d->fmt == FMT_CBOR) {
        int minor = b & 31;
        if ((b >> 5) != 3)
            goto slow;
        if (minor < 24) {
            len = (uint64_t)minor;
            d->i++;
        } else if (minor <= 27) {
            int bytes = 1 << (minor - 24);
            d->i++;
            if (vdec_need(d, (size_t)bytes) < 0)
                return -1;
            len = vdec_be(d, bytes);
        } else {
            goto slow;
        }
    } else if ((b & 0xE0) == 0xA0) {
        len = b & 0x1F;
        d->i++;
    } else if (b == 0xD9 || b == 0xDA || b == 0xDB) {
        int bytes = b == 0xD9 ? 1 : b == 0xDA ? 2
                                              : 4;
        d->i++;
        if (vdec_need(d, (size_t)bytes) < 0)
            return -1;
        len = vdec_be(d, bytes);
    } else {
        goto slow;
    }
    if (vdec_bounded(d, len, 1) < 0 || vdec_need(d, (size_t)len) < 0)
        return -1;
    *out = JS_NewAtomLen(d->ctx, (const char*)d->p + d->i, (size_t)len);
    d->i += (size_t)len;
    return *out == JS_ATOM_NULL ? -1 : 0;
slow:
    {
        JSValue kv = vdec_value(d);
        if (JS_IsException(kv))
            return -1;
        *out = JS_ValueToAtom(d->ctx, kv);
        JS_FreeValue(d->ctx, kv);
        return *out == JS_ATOM_NULL ? -1 : 0;
    }
}

static JSValue vdec_map(vdec_t* d, uint64_t count)
{
    JSValue out;
    uint64_t k;

    if (vdec_bounded(d, count, 2) < 0)
        return JS_EXCEPTION;
    if (count <= VDEC_TPL_MAX) {
        JSAtom atoms[VDEC_TPL_MAX];
        JSValue vals[VDEC_TPL_MAX];
        int depth = d->depth, n = 0;
        d->depth++;
        for (k = 0; k < count; k++) {
            if (vdec_key(d, &atoms[n]) < 0)
                break;
            vals[n] = vdec_value(d);
            if (JS_IsException(vals[n])) {
                JS_FreeAtom(d->ctx, atoms[n]);
                break;
            }
            n++;
        }
        d->depth--;
        if (k < count) {
            while (n-- > 0) {
                JS_FreeAtom(d->ctx, atoms[n]);
                JS_FreeValue(d->ctx, vals[n]);
            }
            return JS_EXCEPTION;
        }
        return JS_NewObjectFromProps(d->ctx,
            depth < VDEC_TPL_DEPTH ? &d->tpl[depth] : NULL, n, atoms, vals);
    }
    out = JS_NewObject(d->ctx);
    if (JS_IsException(out))
        return out;
    d->depth++;
    for (k = 0; k < count; k++) {
        JSValue val;
        JSAtom a;
        if (vdec_key(d, &a) < 0) {
            d->depth--;
            JS_FreeValue(d->ctx, out);
            return JS_EXCEPTION;
        }
        val = vdec_value(d);
        if (JS_IsException(val)
            || JS_DefinePropertyValue(d->ctx, out, a, val, JS_PROP_C_W_E) < 0) {
            JS_FreeAtom(d->ctx, a);
            d->depth--;
            JS_FreeValue(d->ctx, out);
            return JS_EXCEPTION;
        }
        JS_FreeAtom(d->ctx, a);
    }
    d->depth--;
    return out;
}

static JSValue vdec_cbor(vdec_t* d)
{
    uint8_t b;
    int major, minor;
    uint64_t arg = 0;

    if (vdec_need(d, 1) < 0)
        return JS_EXCEPTION;
    b = d->p[d->i++];
    major = b >> 5;
    minor = b & 31;
    if (minor < 24) {
        arg = (uint64_t)minor;
    } else if (minor == 24 || minor == 25 || minor == 26 || minor == 27) {
        int bytes = 1 << (minor - 24);
        if (vdec_need(d, (size_t)bytes) < 0)
            return JS_EXCEPTION;
        arg = vdec_be(d, bytes);
    } else if (major != 7) {
        return JS_ThrowSyntaxError(d->ctx, "decode: indefinite lengths are not "
                                           "accepted; every length is declared");
    }
    switch (major) {
    case 0:
        return vdec_u64(d, arg);
    case 1:
        if (arg > (uint64_t)INT64_MAX)
            return vdec_neg_big(d, arg);
        return vdec_i64(d, -1 - (int64_t)arg);
    case 2:
        return vdec_bin(d, arg);
    case 3:
        return vdec_str(d, arg);
    case 4:
        return vdec_array(d, arg);
    case 5:
        return vdec_map(d, arg);
    case 6: {
        JSValue inner;
        if (arg == 2 || arg == 3)
            return vdec_bignum(d, arg == 3);
        d->depth++;
        inner = vdec_value(d);
        d->depth--;
        return inner;
    }
    default:
        break;
    }
    switch (minor) {
    case 20:
        return JS_FALSE;
    case 21:
        return JS_TRUE;
    case 22:
        return JS_NULL;
    case 23:
        return JS_UNDEFINED;
    case 27: {
        double f;
        uint64_t bits = arg;
        memcpy(&f, &bits, 8);
        return JS_NewFloat64(d->ctx, f);
    }
    case 26: {
        float f;
        uint32_t bits = (uint32_t)arg;
        memcpy(&f, &bits, 4);
        return JS_NewFloat64(d->ctx, (double)f);
    }
    case 25: {
        uint32_t h = (uint32_t)arg, e = (h >> 10) & 0x1F, m = h & 0x3FF;
        double val;
        if (e == 0)
            val = ldexp((double)m, -24);
        else if (e != 31)
            val = ldexp((double)(m + 1024), (int)e - 25);
        else
            val = m ? DYN_NAN : DYN_INFINITY;
        return JS_NewFloat64(d->ctx, (h >> 15) ? -val : val);
    }
    default:
        return JS_ThrowSyntaxError(d->ctx, "decode: unsupported simple value %d",
            minor);
    }
}

static int vdec_arg(vdec_t* d, int bytes, uint64_t* out)
{
    if (vdec_need(d, (size_t)bytes) < 0)
        return -1;
    *out = vdec_be(d, bytes);
    return 0;
}

static JSValue vdec_mp_int(vdec_t* d, uint8_t b)
{
    int nb = 1 << (b - 0xD0);
    uint64_t v;

    if (vdec_arg(d, nb, &v) < 0)
        return JS_EXCEPTION;
    switch (nb) {
    case 1:
        return JS_NewInt32(d->ctx, (int8_t)v);
    case 2:
        return JS_NewInt32(d->ctx, (int16_t)v);
    case 4:
        return JS_NewInt32(d->ctx, (int32_t)v);
    default:
        return vdec_i64(d, (int64_t)v);
    }
}

static JSValue vdec_mp_wide(vdec_t* d, uint8_t b)
{
    uint64_t v;

    switch (b) {
    case 0xC4:
    case 0xC5:
    case 0xC6:
        return vdec_arg(d, b == 0xC4 ? 1 : b == 0xC5 ? 2
                                                     : 4,
                   &v)
                < 0
            ? JS_EXCEPTION
            : vdec_bin(d, v);
    case 0xCA: {
        float f;
        uint32_t bits;
        if (vdec_arg(d, 4, &v) < 0)
            return JS_EXCEPTION;
        bits = (uint32_t)v;
        memcpy(&f, &bits, 4);
        return JS_NewFloat64(d->ctx, (double)f);
    }
    case 0xCB: {
        double f;
        if (vdec_arg(d, 8, &v) < 0)
            return JS_EXCEPTION;
        memcpy(&f, &v, 8);
        return JS_NewFloat64(d->ctx, f);
    }
    case 0xCC:
    case 0xCD:
    case 0xCE:
    case 0xCF:
        if (vdec_arg(d, 1 << (b - 0xCC), &v) < 0)
            return JS_EXCEPTION;
        return vdec_u64(d, v);
    case 0xD0:
    case 0xD1:
    case 0xD2:
    case 0xD3:
        return vdec_mp_int(d, b);
    case 0xD9:
    case 0xDA:
    case 0xDB:
        return vdec_arg(d, b == 0xD9 ? 1 : b == 0xDA ? 2
                                                     : 4,
                   &v)
                < 0
            ? JS_EXCEPTION
            : vdec_str(d, v);
    case 0xDC:
    case 0xDD:
        return vdec_arg(d, b == 0xDC ? 2 : 4, &v) < 0
            ? JS_EXCEPTION
            : vdec_array(d, v);
    case 0xDE:
    case 0xDF:
        return vdec_arg(d, b == 0xDE ? 2 : 4, &v) < 0
            ? JS_EXCEPTION
            : vdec_map(d, v);
    default:
        return JS_ThrowSyntaxError(d->ctx, "decode: unsupported type byte 0x%02X "
                                           "at %u",
            b, (unsigned)(d->i - 1));
    }
}

static JSValue vdec_msgpack(vdec_t* d)
{
    uint8_t b;

    if (vdec_need(d, 1) < 0)
        return JS_EXCEPTION;
    b = d->p[d->i++];
    if (b < 0x80)
        return JS_NewInt32(d->ctx, b);
    if (b >= 0xE0)
        return JS_NewInt32(d->ctx, (int8_t)b);
    if ((b & 0xF0) == 0x80)
        return vdec_map(d, b & 0x0F);
    if ((b & 0xF0) == 0x90)
        return vdec_array(d, b & 0x0F);
    if ((b & 0xE0) == 0xA0)
        return vdec_str(d, b & 0x1F);
    if (b == 0xC0)
        return JS_NULL;
    if (b == 0xC2)
        return JS_FALSE;
    if (b == 0xC3)
        return JS_TRUE;
    return vdec_mp_wide(d, b);
}

JSValue vdec_value(vdec_t* d)
{
    if (d->depth >= VS_MAX_DEPTH)
        return JS_ThrowRangeError(d->ctx, "decode: nesting exceeds %d",
            VS_MAX_DEPTH);
    return d->fmt == FMT_CBOR ? vdec_cbor(d) : vdec_msgpack(d);
}

#endif
