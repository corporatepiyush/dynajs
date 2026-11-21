#include "asn1-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_ASN1)

#define ASN1_MAX_INT_BYTES 8
#define ASN1_MAX_NODES (1u << 20)
#define ASN1_INT52 (INT64_C(1) << 53)

typedef struct {
    JSContext* ctx;
    const uint8_t* p;
    size_t n, i;
    int depth;
    uint64_t nodes;
} dr_t;

static int dr_tag(dr_t* d, size_t limit, int* cls, int* constructed,
    uint32_t* tag)
{
    uint8_t b;
    uint64_t v = 0;
    uint32_t t;
    int nsub = 0, first = 0;

    if (limit - d->i < 1)
        return JS_ThrowSyntaxError(d->ctx, "ASN.1: truncated identifier"), -1;
    b = d->p[d->i++];
    *cls = b >> 6;
    *constructed = (b >> 5) & 1;
    t = b & 0x1F;
    if (t != 0x1F) {
        *tag = t;
        return 0;
    }
    for (;;) {
        if (limit - d->i < 1)
            return JS_ThrowSyntaxError(d->ctx,
                       "ASN.1: truncated high-tag-number"),
                   -1;
        b = d->p[d->i++];
        if (nsub == 0)
            first = b & 0x7F;
        if (v > ((uint64_t)ASN1_MAX_TAG >> 7))
            return JS_ThrowSyntaxError(d->ctx,
                       "ASN.1: tag number exceeds 2^31-1"),
                   -1;
        v = (v << 7) | (b & 0x7F);
        nsub++;
        if (!(b & 0x80))
            break;
    }
    if (v < 31 || (nsub > 1 && first == 0))
        return JS_ThrowSyntaxError(d->ctx,
                   "ASN.1: non-minimal high-tag-number"),
               -1;
    if (v > (uint64_t)ASN1_MAX_TAG)
        return JS_ThrowSyntaxError(d->ctx,
                   "ASN.1: tag number exceeds 2^31-1"),
               -1;
    *tag = (uint32_t)v;
    return 0;
}

static int dr_len(dr_t* d, size_t limit, size_t* out)
{
    uint8_t b;
    size_t v;
    int k, j;

    if (limit - d->i < 1)
        return JS_ThrowSyntaxError(d->ctx, "ASN.1: truncated length"), -1;
    b = d->p[d->i++];
    if (b < 0x80) {
        v = b;
    } else if (b == 0x80) {
        return JS_ThrowSyntaxError(d->ctx,
                   "ASN.1: indefinite length is BER, not DER; every length must be "
                   "declared"),
               -1;
    } else {
        k = b & 0x7F;
        if (k > 4)
            return JS_ThrowSyntaxError(d->ctx,
                       "ASN.1: length-of-length %d exceeds 4 bytes", k),
                   -1;
        if ((size_t)k > limit - d->i)
            return JS_ThrowSyntaxError(d->ctx, "ASN.1: truncated length"), -1;
        if (d->p[d->i] == 0)
            return JS_ThrowSyntaxError(d->ctx,
                       "ASN.1: long-form length with a leading zero octet"),
                   -1;
        v = 0;
        for (j = 0; j < k; j++)
            v = (v << 8) | d->p[d->i + (size_t)j];
        d->i += (size_t)k;
        if (k == 1 && v < 128)
            return JS_ThrowSyntaxError(d->ctx,
                       "ASN.1: non-minimal length: long form used where a single "
                       "octet fits"),
                   -1;
    }
    if (v > limit - d->i)
        return JS_ThrowSyntaxError(d->ctx,
                   "ASN.1: declared length %llu exceeds the remaining input",
                   (unsigned long long)v),
               -1;
    *out = (size_t)v;
    return 0;
}

static JSValue asn1_decode_bool(dr_t* d, size_t start, size_t end)
{
    if (end - start != 1)
        return JS_ThrowSyntaxError(d->ctx,
            "ASN.1: BOOLEAN content must be a single octet");
    if (d->p[start] == 0)
        return JS_FALSE;
    if (d->p[start] == 0xFF)
        return JS_TRUE;
    return JS_ThrowSyntaxError(d->ctx,
        "ASN.1: BOOLEAN content 0x%02X is BER, not DER (only 0x00 and 0xFF)",
        d->p[start]);
}

static JSValue asn1_bigint_from_digits(JSContext* ctx, int neg,
    const char* digits, int nd)
{
    char buf[ASN1_MAX_BIGINT_BYTES * 3 + 2];
    JSValue global, ctor, arg, out;
    JSValueConst a1[1];
    int pos = 0;

    if (neg)
        buf[pos++] = '-';
    memcpy(buf + pos, digits, (size_t)nd);
    pos += nd;
    buf[pos] = '\0';

    global = JS_GetGlobalObject(ctx);
    ctor = JS_GetPropertyStr(ctx, global, "BigInt");
    JS_FreeValue(ctx, global);
    if (!JS_IsFunction(ctx, ctor)) {
        JS_FreeValue(ctx, ctor);
        return JS_ThrowInternalError(ctx,
            "ASN.1: the BigInt constructor is unavailable");
    }
    arg = JS_NewStringLen(ctx, buf, (size_t)pos);
    if (JS_IsException(arg)) {
        JS_FreeValue(ctx, ctor);
        return arg;
    }
    a1[0] = arg;
    out = JS_Call(ctx, ctor, JS_UNDEFINED, 1, a1);
    JS_FreeValue(ctx, ctor);
    if (JS_IsException(out))
        return out;
    JS_FreeValue(ctx, arg);
    return out;
}

static JSValue asn1_decode_wide_int(dr_t* d, size_t start, size_t len)
{
    uint8_t mag[ASN1_MAX_BIGINT_BYTES];
    char digits[ASN1_MAX_BIGINT_BYTES * 3 + 1];
    int neg = (d->p[start] & 0x80) != 0;
    int n = (int)len, nd = 0, i;

    for (i = 0; i < n; i++)
        mag[i] = d->p[start + (size_t)(n - 1 - i)];
    if (neg) {
        unsigned carry = 1;
        for (i = 0; i < n; i++) {
            unsigned x = (unsigned)(uint8_t)~mag[i] + carry;
            mag[i] = (uint8_t)(x & 0xFF);
            carry = x >> 8;
        }
    }
    while (n > 0 && mag[n - 1] == 0)
        n--;
    if (n == 0)
        digits[nd++] = '0';
    while (n > 0) {
        unsigned rem = 0;
        int k;
        for (k = n - 1; k >= 0; k--) {
            unsigned x = ((unsigned)rem << 8) | mag[k];
            mag[k] = (uint8_t)(x / 10);
            rem = x % 10;
        }
        digits[nd++] = (char)('0' + rem);
        while (n > 0 && mag[n - 1] == 0)
            n--;
    }
    for (i = 0; i < nd / 2; i++) {
        char t = digits[i];
        digits[i] = digits[nd - 1 - i];
        digits[nd - 1 - i] = t;
    }
    return asn1_bigint_from_digits(d->ctx, neg, digits, nd);
}

static JSValue asn1_decode_int(dr_t* d, size_t start, size_t end)
{
    size_t len = end - start, k;
    uint64_t u = 0;
    int64_t v;

    if (len == 0)
        return JS_ThrowSyntaxError(d->ctx, "ASN.1: empty INTEGER content");
    if (len > ASN1_MAX_BIGINT_BYTES)
        return JS_ThrowSyntaxError(d->ctx,
            "ASN.1: INTEGER content of %u bytes exceeds the %d-byte cap",
            (unsigned)len, ASN1_MAX_BIGINT_BYTES);
    if (len > 1 && d->p[start] == 0x00 && (d->p[start + 1] & 0x80) == 0)
        return JS_ThrowSyntaxError(d->ctx,
            "ASN.1: non-minimal INTEGER: redundant leading 0x00");
    if (len > 1 && d->p[start] == 0xFF && (d->p[start + 1] & 0x80) != 0)
        return JS_ThrowSyntaxError(d->ctx,
            "ASN.1: non-minimal INTEGER: redundant leading 0xFF");
    if (len > ASN1_MAX_INT_BYTES)
        return asn1_decode_wide_int(d, start, len);
    for (k = 0; k < len; k++)
        u = (u << 8) | d->p[start + k];
    if (d->p[start] & 0x80) {
        if (len == 8)
            v = (int64_t)u;
        else
            v = (int64_t)(u | ~((UINT64_C(1) << (8 * len)) - 1));
    } else {
        v = (int64_t)u;
    }
    if (v >= -ASN1_INT52 && v <= ASN1_INT52)
        return JS_NewInt64(d->ctx, v);
    return JS_NewBigInt64(d->ctx, v);
}

static JSValue asn1_decode_bits(dr_t* d, size_t start, size_t end)
{
    size_t len = end - start;
    int unused;
    JSValue obj, vu, vb;

    if (len == 0)
        return JS_ThrowSyntaxError(d->ctx,
            "ASN.1: BIT STRING content must include the unused-bits octet");
    unused = d->p[start];
    if (unused > 7)
        return JS_ThrowSyntaxError(d->ctx,
            "ASN.1: BIT STRING unused-bits count %d is not 0..7", unused);
    if (unused > 0 && len == 1)
        return JS_ThrowSyntaxError(d->ctx,
            "ASN.1: BIT STRING has unused bits but no payload octet");
    if (unused > 0 && (d->p[end - 1] & ((1u << unused) - 1)) != 0)
        return JS_ThrowSyntaxError(d->ctx,
            "ASN.1: BIT STRING trailing unused bits must be zero in DER");
    obj = JS_NewObject(d->ctx);
    if (JS_IsException(obj))
        return obj;
    vu = JS_NewInt32(d->ctx, unused);
    vb = asn1_new_bytes(d->ctx, d->p + start + 1, len - 1);
    if (JS_IsException(vb)) {
        JS_FreeValue(d->ctx, obj);
        return vb;
    }
    if (JS_DefinePropertyValueStr(d->ctx, obj, "unused", vu,
            JS_PROP_C_W_E)
        < 0) {
        JS_FreeValue(d->ctx, vb);
        JS_FreeValue(d->ctx, obj);
        return JS_EXCEPTION;
    }
    if (JS_DefinePropertyValueStr(d->ctx, obj, "bytes", vb,
            JS_PROP_C_W_E)
        < 0) {
        JS_FreeValue(d->ctx, obj);
        return JS_EXCEPTION;
    }
    return obj;
}

static JSValue asn1_decode_null(dr_t* d, size_t start, size_t end)
{
    (void)d;
    if (start != end)
        return JS_ThrowSyntaxError(d->ctx,
            "ASN.1: NULL must have empty content");
    return JS_NULL;
}

static int oid_subid(const uint8_t* p, size_t end, size_t* i, uint64_t* out,
    uint64_t cap)
{
    uint64_t v = 0;
    int chunks = 0, first = 0;

    for (;;) {
        uint8_t b;
        if (*i >= end)
            return -1;
        b = p[(*i)++];
        if (chunks == 0)
            first = b & 0x7F;
        if (v > (cap >> 7) || ((v << 7) | (uint64_t)(b & 0x7F)) > cap)
            return -1;
        v = (v << 7) | (b & 0x7F);
        chunks++;
        if (!(b & 0x80))
            break;
    }
    if (chunks > 1 && first == 0)
        return 0;
    *out = v;
    return 1;
}

static JSValue asn1_decode_oid(dr_t* d, size_t start, size_t end)
{
    uint32_t arcs[ASN1_MAX_OID_ARCS];
    char buf[ASN1_MAX_OID_ARCS * 11 + 1];
    size_t i = start, k;
    int narc = 0, pos = 0, r;
    uint64_t subid;

    if (i >= end)
        return JS_ThrowSyntaxError(d->ctx, "ASN.1: empty OID content");
    r = oid_subid(d->p, end, &i, &subid, (uint64_t)ASN1_ARC_MAX + 80);
    if (r < 0)
        return JS_ThrowSyntaxError(d->ctx,
            "ASN.1: truncated or oversized first OID subidentifier");
    if (r == 0)
        return JS_ThrowSyntaxError(d->ctx,
            "ASN.1: non-minimal first OID subidentifier");
    if (subid < 40) {
        arcs[0] = 0;
        arcs[1] = (uint32_t)subid;
    } else if (subid < 80) {
        arcs[0] = 1;
        arcs[1] = (uint32_t)(subid - 40);
    } else {
        arcs[0] = 2;
        arcs[1] = (uint32_t)(subid - 80);
    }
    narc = 2;
    while (i < end) {
        if (narc >= ASN1_MAX_OID_ARCS)
            return JS_ThrowSyntaxError(d->ctx,
                "ASN.1: more than %d OID arcs", ASN1_MAX_OID_ARCS);
        r = oid_subid(d->p, end, &i, &subid, ASN1_ARC_MAX);
        if (r < 0)
            return JS_ThrowSyntaxError(d->ctx,
                "ASN.1: truncated or oversized OID subidentifier (arc exceeds "
                "2^32-1)");
        if (r == 0)
            return JS_ThrowSyntaxError(d->ctx,
                "ASN.1: non-minimal OID subidentifier");
        arcs[narc++] = (uint32_t)subid;
    }
    for (k = 0; k < (size_t)narc; k++) {
        uint32_t v = arcs[k];
        char t[11];
        int tn = 0;
        if (k)
            buf[pos++] = '.';
        do {
            t[tn++] = (char)('0' + v % 10);
            v /= 10;
        } while (v);
        while (tn)
            buf[pos++] = t[--tn];
    }
    return JS_NewStringLen(d->ctx, buf, (size_t)pos);
}

static JSValue asn1_decode_time(dr_t* d, size_t start, size_t end, int tag)
{
    if (!time_ok(d->p + start, end - start, tag))
        return JS_ThrowSyntaxError(d->ctx, tag == 23 ? "ASN.1: UTCTime is not canonical YYMMDDHHMMSSZ" : "ASN.1: GeneralizedTime is not canonical YYYYMMDDHHMMSSZ");
    return JS_NewStringLen(d->ctx, (const char*)d->p + start, end - start);
}

static JSValue asn1_decode_utf8(dr_t* d, size_t start, size_t end)
{
    const uint8_t* p = d->p + start;
    size_t n = end - start;
    if (!utf8_ok(p, n))
        return JS_ThrowSyntaxError(d->ctx,
            "ASN.1: UTF8String is not well-formed UTF-8");
    return JS_NewStringLen(d->ctx, (const char*)p, n);
}

static JSValue asn1_decode_printable(dr_t* d, size_t start, size_t end)
{
    const uint8_t* p = d->p + start;
    size_t n = end - start;
    if (!printable_ok(p, n))
        return JS_ThrowSyntaxError(d->ctx,
            "ASN.1: PrintableString contains characters outside the printable "
            "set");
    return JS_NewStringLen(d->ctx, (const char*)p, n);
}

static JSValue asn1_decode_ia5(dr_t* d, size_t start, size_t end)
{
    const uint8_t* p = d->p + start;
    size_t n = end - start, i;

    for (i = 0; i < n; i++)
        if (p[i] > 0x7F)
            return JS_ThrowSyntaxError(d->ctx,
                "ASN.1: IA5String byte 0x%02X at offset %u is outside ASCII",
                p[i], (unsigned)i);
    return JS_NewStringLen(d->ctx, (const char*)p, n);
}

static JSValue asn1_decode_ucs(dr_t* d, size_t start, size_t end, int bmp)
{
    const uint8_t* p = d->p + start;
    size_t n = end - start, i, unit = bmp ? 2 : 4, w = 0, need;
    uint8_t* out;
    JSValue r;

    if (n % unit != 0)
        return JS_ThrowSyntaxError(d->ctx,
            bmp ? "ASN.1: BMPString content is %u octets, not a whole number "
                  "of 2-octet groups"
                : "ASN.1: UniversalString content is %u octets, not a whole "
                  "number of 4-octet groups",
            (unsigned)n);
    if (n / unit > (SIZE_MAX - 1) / 4)
        return JS_ThrowRangeError(d->ctx, "ASN.1: %s content too large to "
                                          "re-encode",
            bmp ? "BMPString" : "UniversalString");
    need = n / unit * 4 + 1;
    out = (uint8_t*)malloc(need);
    if (!out)
        return JS_ThrowOutOfMemory(d->ctx);
    for (i = 0; i < n; i += unit) {
        uint32_t cp = 0;
        size_t k;
        for (k = 0; k < unit; k++)
            cp = (cp << 8) | p[i + k];
        if (cp >= 0xD800 && cp <= 0xDFFF) {
            free(out);
            return JS_ThrowSyntaxError(d->ctx,
                "ASN.1: %s content contains surrogate 0x%04X, which is "
                "reserved and not a character",
                bmp ? "BMPString" : "UniversalString", cp);
        }
        if (cp > 0x10FFFF) {
            free(out);
            return JS_ThrowSyntaxError(d->ctx,
                "ASN.1: UniversalString code point 0x%X is above U+10FFFF",
                cp);
        }
        w += (size_t)utf8_put(out + w, cp);
    }
    r = JS_NewStringLen(d->ctx, (const char*)out, w);
    free(out);
    return r;
}

static JSValue asn1_decode_tlv(dr_t* d, size_t limit);

static JSValue asn1_decode_children(dr_t* d, size_t end)
{
    JSValue arr = JS_NewArray(d->ctx);
    uint64_t count = 0;

    if (JS_IsException(arr))
        return arr;
    d->depth++;
    while (d->i < end) {
        JSValue el = asn1_decode_tlv(d, end);
        if (JS_IsException(el) || JS_DefinePropertyValueUint32(d->ctx, arr, (uint32_t)count, el, JS_PROP_C_W_E) < 0) {
            d->depth--;
            JS_FreeValue(d->ctx, arr);
            return JS_EXCEPTION;
        }
        if (++count > ASN1_MAX_CHILDREN) {
            d->depth--;
            JS_FreeValue(d->ctx, arr);
            return JS_ThrowRangeError(d->ctx,
                "ASN.1: more than %d children in one construct",
                ASN1_MAX_CHILDREN);
        }
    }
    d->depth--;
    if (d->i != end) {
        JS_FreeValue(d->ctx, arr);
        return JS_ThrowSyntaxError(d->ctx, "ASN.1: construct content "
                                           "mis-parsed");
    }
    return arr;
}

static JSValue asn1_decode_tlv(dr_t* d, size_t limit)
{
    int cls, constructed;
    uint32_t tag;
    size_t len, start, end;

    if (d->depth >= ASN1_MAX_DEPTH)
        return JS_ThrowRangeError(d->ctx, "ASN.1: nesting exceeds %d",
            ASN1_MAX_DEPTH);
    if (++d->nodes > ASN1_MAX_NODES)
        return JS_ThrowRangeError(d->ctx,
            "ASN.1: more than %u decoded nodes", ASN1_MAX_NODES);
    if (dr_tag(d, limit, &cls, &constructed, &tag) < 0)
        return JS_EXCEPTION;
    if (dr_len(d, limit, &len) < 0)
        return JS_EXCEPTION;
    start = d->i;
    end = start + len;
    if (cls == 0 && constructed && tag != 16 && tag != 17)
        return JS_ThrowSyntaxError(d->ctx,
            "ASN.1: a constructed UNIVERSAL tag %u is BER, not DER; only "
            "SEQUENCE and SET construct",
            (unsigned)tag);
    if (cls == 0 && (tag == 16 || tag == 17)) {
        JSValue kids;
        if (!constructed)
            return JS_ThrowSyntaxError(d->ctx,
                "ASN.1: a primitive SEQUENCE/SET is not DER; the encoding of "
                "a SEQUENCE or SET is constructed");
        kids = asn1_decode_children(d, end);
        if (JS_IsException(kids))
            return kids;
        return asn1_node(d->ctx, 0, (int)tag, 1, kids);
    }
    d->i = end;
    {
        JSValue v = JS_UNDEFINED;
        if (cls == 0) {
            switch (tag) {
            case 1:
                v = asn1_decode_bool(d, start, end);
                break;
            case 2:
                v = asn1_decode_int(d, start, end);
                break;
            case 3:
                v = asn1_decode_bits(d, start, end);
                break;
            case 4:
                v = asn1_new_bytes(d->ctx, d->p + start, end - start);
                break;
            case 5:
                v = asn1_decode_null(d, start, end);
                break;
            case 6:
                v = asn1_decode_oid(d, start, end);
                break;
            case 12:
                v = asn1_decode_utf8(d, start, end);
                break;
            case 19:
                v = asn1_decode_printable(d, start, end);
                break;
            case 22:
                v = asn1_decode_ia5(d, start, end);
                break;
            case 28:
                v = asn1_decode_ucs(d, start, end, 0);
                break;
            case 30:
                v = asn1_decode_ucs(d, start, end, 1);
                break;
            case 23:
            case 24:
                v = asn1_decode_time(d, start, end, (int)tag);
                break;
            default:
                v = asn1_new_bytes(d->ctx, d->p + start, end - start);
                break;
            }
        } else {
            v = asn1_new_bytes(d->ctx, d->p + start, end - start);
        }
        if (JS_IsException(v))
            return v;
        return asn1_node(d->ctx, cls, (int)tag, constructed, v);
    }
}

JSValue asn1_decode(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dr_t d;
    JSValue hold = JS_UNDEFINED, out;
    int br;

    (void)this_val;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "ASN1.decode(bytes): bytes are required");
    memset(&d, 0, sizeof d);
    d.ctx = ctx;
    br = asn1_bytes(ctx, argv[0], &d.p, &d.n, &hold);
    if (br < 0)
        return JS_EXCEPTION;
    if (br == 0)
        return JS_ThrowTypeError(ctx, "ASN1.decode(bytes): expected a "
                                      "Uint8Array or ArrayBuffer");
    if (d.n == 0) {
        JS_FreeValue(ctx, hold);
        return JS_ThrowSyntaxError(ctx, "ASN1.decode: empty input");
    }
    out = asn1_decode_tlv(&d, d.n);
    if (!JS_IsException(out) && d.i != d.n) {
        JS_FreeValue(ctx, out);
        out = JS_ThrowSyntaxError(ctx, "ASN1.decode: %u trailing bytes after "
                                       "the value",
            (unsigned)(d.n - d.i));
    }
    JS_FreeValue(ctx, hold);
    return out;
}

#endif
