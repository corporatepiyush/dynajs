/* dyna-asn1.c -- ASN.1 DER codec, wired into the dyna:serialize module.
   Plan 3.12 (row 18): decode/encode for SEQUENCE/SET/INTEGER/OCTET STRING/
   BIT STRING/OID/UTCTime/GeneralizedTime/PrintableString/UTF8String/BOOLEAN/
   NULL plus context tagging.

   The DECODER is the untrusted surface: a declared length is checked against
   the enclosing content BEFORE anything is allocated, BER indefinite lengths
   and every non-minimal encoding are refused, and each refusal names its
   reason. Encode emits canonical DER (shortest length form, minimal INTEGER,
   BOOLEAN as 0x00/0xFF) so re-encoding a canonical decode is byte-identical.
   Oracle: `openssl asn1parse` on generated DER -- see tests/test_asn1.js.

   Registered from dyna-vserialize.c, the dyna-netip-inside-dyna-net idiom
   (see src/dyna-netip.c:954-968). The integrator adds, in dyna-vserialize.c:
     extern int dyn_asn1_register(JSContext *ctx, JSModuleDef *m);
     extern int dyn_asn1_add_exports(JSContext *ctx, JSModuleDef *m);
   then in dyn_vs_init_module:     if (dyn_asn1_register(ctx, m) < 0) return -1;
   and in js_nat_init_vserialize, after JS_NewCModule:
                                    if (dyn_asn1_add_exports(ctx, m) < 0) return -1;
   This TU exports those two functions and nothing else public. */
#include "dyna-nat.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_VSERIALIZE)

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "core/dyn-sb.h"

#ifndef countof
#define countof(x) (sizeof(x) / sizeof((x)[0]))
#endif

#define ASN1_MAX_DEPTH      256
#define ASN1_MAX_INT_BYTES  8      /* a DER INTEGER wider than this is refused */
#define ASN1_MAX_OID_ARCS   64
#define ASN1_ARC_MAX        0xFFFFFFFFu
#define ASN1_MAX_TAG        0x7FFFFFFF
#define ASN1_MAX_CHILDREN   (1u << 16)  /* ~10 MB of objects from ~200 KB of DER */
#define ASN1_INT52          (INT64_C(1) << 53)  /* |v| <= this is an exact Number */

/* ------------------------------------------------------------ byte buffer */

/* Thin wrapper over core/dyn-sb.h. ab_write keeps BOTH error signals the
   callers rely on -- the sticky oom flag and the -1 return -- while the core
   stays error-convention-neutral (ok/fail only, stores nothing). */
typedef struct { uint8_t *p; size_t n, cap; int oom; } ab_t;

static void ab_init(ab_t *b) { b->p = NULL; b->n = b->cap = 0; b->oom = 0; }

static void ab_free(ab_t *b) { free(b->p); b->p = NULL; b->n = b->cap = 0; }

static int ab_write(ab_t *b, const uint8_t *p, size_t n)
{
    if (b->oom || !n)
        return 0;
    if (b->n + n > b->cap
        && !dyn_sb_reserve((void **)&b->p, &b->cap, b->n + n, 64)) {
        b->oom = 1;
        return -1;
    }
    memcpy(b->p + b->n, p, n);
    b->n += n;
    return 0;
}

/* ------------------------------------------------------------- JS<->bytes */

/* Read the bytes of a Uint8Array/DataView (bpe 1) or a whole ArrayBuffer.
   Returns 1 with the window set, 0 when v is not bytes (no exception), -1 on
   a genuine error (exception set). Same idiom as dyna-vserialize.c's
   vs_bytes_arg: JS_GetArrayBufferView then JS_GetArrayBuffer. */
static int asn1_bytes(JSContext *ctx, JSValueConst v, const uint8_t **pp,
                      size_t *pn, JSValue *hold)
{
    size_t off, len, bpe, ab;
    uint8_t *base;
    JSValue buf = JS_GetArrayBufferView(ctx, v, &off, &len, &bpe);

    if (JS_IsException(buf)) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        base = JS_GetArrayBuffer(ctx, &ab, v);
        if (!base) {
            JS_FreeValue(ctx, JS_GetException(ctx));
            return 0;               /* not bytes at all: caller refuses */
        }
        *hold = JS_UNDEFINED;
        *pp = base;
        *pn = ab;
        return 1;
    }
    if (bpe != 1) {
        JS_FreeValue(ctx, buf);
        return 0;                   /* a wider view is not bytes */
    }
    base = JS_GetArrayBuffer(ctx, &ab, buf);
    if (!base || off > ab || len > ab - off) {
        JS_FreeValue(ctx, buf);
        JS_ThrowTypeError(ctx, "ASN.1: invalid byte view");
        return -1;
    }
    *hold = buf;
    *pp = base + off;
    *pn = len;
    return 1;
}

static JSValue asn1_new_bytes(JSContext *ctx, const uint8_t *p, size_t n)
{
    static const uint8_t zero = 0;
    JSValueConst ta[3];
    JSValue ab, r;

    ab = JS_NewArrayBufferCopy(ctx, n ? p : &zero, n);
    if (JS_IsException(ab))
        return ab;
    ta[0] = ab; ta[1] = JS_UNDEFINED; ta[2] = JS_UNDEFINED;
    r = JS_NewTypedArray(ctx, 3, ta, JS_TYPED_ARRAY_UINT8);
    JS_FreeValue(ctx, ab);
    return r;
}

/* --------------------------------------------------------------- DER write */

static int der_ident(ab_t *b, int cls, int constructed, uint32_t tag)
{
    uint8_t h[8];
    uint8_t t[8];
    uint32_t v;
    int i, n = 0;

    if (tag < 31) {
        h[0] = (uint8_t)((cls << 6) | (constructed << 5) | (int)tag);
        return ab_write(b, h, 1);
    }
    h[0] = (uint8_t)((cls << 6) | (constructed << 5) | 0x1F);
    v = tag;
    do { t[n++] = (uint8_t)(v & 0x7F); v >>= 7; } while (v);
    for (i = n - 1; i >= 0; i--)
        h[1 + (n - 1 - i)] = (uint8_t)(t[i] | (i ? 0x80 : 0));
    return ab_write(b, h, 1 + (size_t)n);
}

static int der_length(ab_t *b, size_t n)
{
    uint8_t h[9];
    uint8_t t[8];
    size_t v = n;
    int k = 0, i;

    if (n < 128) {
        h[0] = (uint8_t)n;
        return ab_write(b, h, 1);
    }
    while (v) { t[k++] = (uint8_t)(v & 0xFF); v >>= 8; }
    h[0] = (uint8_t)(0x80 | k);
    for (i = 0; i < k; i++)
        h[1 + i] = t[k - 1 - i];
    return ab_write(b, h, 1 + (size_t)k);
}

/* The two's-complement MINIMAL content octets for v (X.690 8.3.2): the
   shortest length such that the sign bit does the sign extension. Returns
   the byte count (1..8). */
static int der_int_bytes(int64_t v, uint8_t *out)
{
    int n, i;

    if (v >= 0) {
        for (n = 1; n < 8; n++)
            if ((uint64_t)v <= (UINT64_C(1) << (8 * n - 1)) - 1)
                break;
    } else {
        for (n = 1; n < 8; n++)
            if (v >= -((int64_t)UINT64_C(1) << (8 * n - 1)))
                break;
    }
    for (i = 0; i < n; i++)
        out[i] = (uint8_t)((uint64_t)v >> (8 * (n - 1 - i)));
    return n;
}

/* --------------------------------------------------------------- DER read */

typedef struct {
    JSContext *ctx;
    const uint8_t *p;
    size_t n, i;
    int depth;
} dr_t;

/* Read an identifier octet sequence. Every read is bounded by `limit`, the
   end of the enclosing content, so a header can never overrun its parent. */
static int dr_tag(dr_t *d, size_t limit, int *cls, int *constructed,
                  uint32_t *tag)
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
                "ASN.1: truncated high-tag-number"), -1;
        b = d->p[d->i++];
        if (nsub == 0)
            first = b & 0x7F;
        if (v > ((uint64_t)ASN1_MAX_TAG >> 7))
            return JS_ThrowSyntaxError(d->ctx,
                "ASN.1: tag number exceeds 2^31-1"), -1;
        v = (v << 7) | (b & 0x7F);
        nsub++;
        if (!(b & 0x80))
            break;
    }
    if (v < 31 || (nsub > 1 && first == 0))
        return JS_ThrowSyntaxError(d->ctx,
            "ASN.1: non-minimal high-tag-number"), -1;
    if (v > (uint64_t)ASN1_MAX_TAG)
        return JS_ThrowSyntaxError(d->ctx,
            "ASN.1: tag number exceeds 2^31-1"), -1;
    *tag = (uint32_t)v;
    return 0;
}

/* Read a definite length, validated against `limit` BEFORE it is returned:
   a lying length must not allocate anything. Refuses indefinite (BER) and
   every non-minimal long form. */
static int dr_len(dr_t *d, size_t limit, size_t *out)
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
            "declared"), -1;
    } else {
        k = b & 0x7F;
        if (k > 4)
            return JS_ThrowSyntaxError(d->ctx,
                "ASN.1: length-of-length %d exceeds 4 bytes", k), -1;
        if ((size_t)k > limit - d->i)
            return JS_ThrowSyntaxError(d->ctx, "ASN.1: truncated length"), -1;
        if (d->p[d->i] == 0)
            return JS_ThrowSyntaxError(d->ctx,
                "ASN.1: long-form length with a leading zero octet"), -1;
        v = 0;
        for (j = 0; j < k; j++)
            v = (v << 8) | d->p[d->i + (size_t)j];
        d->i += (size_t)k;
        if (k == 1 && v < 128)
            return JS_ThrowSyntaxError(d->ctx,
                "ASN.1: non-minimal length: long form used where a single "
                "octet fits"), -1;
    }
    if (v > limit - d->i)
        return JS_ThrowSyntaxError(d->ctx,
            "ASN.1: declared length %llu exceeds the remaining input",
            (unsigned long long)v), -1;
    *out = (size_t)v;
    return 0;
}

/* ------------------------------------------------------- content decoders */

static JSValue asn1_decode_bool(dr_t *d, size_t start, size_t end)
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

static JSValue asn1_decode_int(dr_t *d, size_t start, size_t end)
{
    size_t len = end - start, k;
    uint64_t u = 0;
    int64_t v;

    if (len == 0)
        return JS_ThrowSyntaxError(d->ctx, "ASN.1: empty INTEGER content");
    if (len > ASN1_MAX_INT_BYTES)
        return JS_ThrowSyntaxError(d->ctx,
            "ASN.1: INTEGER content of %u bytes exceeds the %d-byte cap",
            (unsigned)len, ASN1_MAX_INT_BYTES);
    if (len > 1 && d->p[start] == 0x00 && (d->p[start + 1] & 0x80) == 0)
        return JS_ThrowSyntaxError(d->ctx,
            "ASN.1: non-minimal INTEGER: redundant leading 0x00");
    if (len > 1 && d->p[start] == 0xFF && (d->p[start + 1] & 0x80) != 0)
        return JS_ThrowSyntaxError(d->ctx,
            "ASN.1: non-minimal INTEGER: redundant leading 0xFF");
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
    /* An exact Number for |v| <= 2^53, an exact BigInt beyond -- never a
       silently-rounded double. */
    if (v >= -ASN1_INT52 && v <= ASN1_INT52)
        return JS_NewInt64(d->ctx, v);
    return JS_NewBigInt64(d->ctx, v);
}

static JSValue asn1_decode_bits(dr_t *d, size_t start, size_t end)
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
                                  JS_PROP_C_W_E) < 0) {
        JS_FreeValue(d->ctx, vb);
        JS_FreeValue(d->ctx, obj);
        return JS_EXCEPTION;
    }
    if (JS_DefinePropertyValueStr(d->ctx, obj, "bytes", vb,
                                  JS_PROP_C_W_E) < 0) {
        JS_FreeValue(d->ctx, obj);
        return JS_EXCEPTION;
    }
    return obj;
}

static JSValue asn1_decode_null(dr_t *d, size_t start, size_t end)
{
    (void)d;
    if (start != end)
        return JS_ThrowSyntaxError(d->ctx,
            "ASN.1: NULL must have empty content");
    return JS_NULL;
}

/* One base-128 subidentifier. Returns 1 with the value, 0 for a NON-MINIMAL
   encoding (leading zero chunk), -1 for truncated or oversized. */
static int oid_subid(const uint8_t *p, size_t end, size_t *i, uint64_t *out,
                     uint64_t cap)
{
    uint64_t v = 0;
    int chunks = 0, first = 0;

    for (;;) {
        uint8_t b;
        if (*i >= end)
            return -1;              /* truncated */
        b = p[(*i)++];
        if (chunks == 0)
            first = b & 0x7F;
        if (v > (cap >> 7) || ((v << 7) | (uint64_t)(b & 0x7F)) > cap)
            return -1;              /* oversized */
        v = (v << 7) | (b & 0x7F);
        chunks++;
        if (!(b & 0x80))
            break;
    }
    if (chunks > 1 && first == 0)
        return 0;                   /* non-minimal */
    *out = v;
    return 1;
}

static JSValue asn1_decode_oid(dr_t *d, size_t start, size_t end)
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
        arcs[0] = 0; arcs[1] = (uint32_t)subid;
    } else if (subid < 80) {
        arcs[0] = 1; arcs[1] = (uint32_t)(subid - 40);
    } else {
        arcs[0] = 2; arcs[1] = (uint32_t)(subid - 80);
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
        do { t[tn++] = (char)('0' + v % 10); v /= 10; } while (v);
        while (tn)
            buf[pos++] = t[--tn];
    }
    return JS_NewStringLen(d->ctx, buf, (size_t)pos);
}

static int utf8_ok(const uint8_t *p, size_t n)
{
    size_t i = 0;
    while (i < n) {
        uint8_t b = p[i++];
        if (b < 0x80)
            continue;
        if (b >= 0xC2 && b <= 0xDF) {
            if (i >= n || (p[i] & 0xC0) != 0x80) return 0;
            i++;
        } else if (b == 0xE0) {
            if (i + 1 >= n || p[i] < 0xA0 || (p[i] & 0xC0) != 0x80 ||
                (p[i + 1] & 0xC0) != 0x80) return 0;
            i += 2;
        } else if ((b >= 0xE1 && b <= 0xEC) || (b >= 0xEE && b <= 0xEF)) {
            if (i + 1 >= n || (p[i] & 0xC0) != 0x80 ||
                (p[i + 1] & 0xC0) != 0x80) return 0;
            i += 2;
        } else if (b == 0xED) {
            if (i + 1 >= n || p[i] >= 0xA0 || (p[i] & 0xC0) != 0x80 ||
                (p[i + 1] & 0xC0) != 0x80) return 0;
            i += 2;
        } else if (b == 0xF0) {
            if (i + 2 >= n || p[i] < 0x90 || (p[i] & 0xC0) != 0x80 ||
                (p[i + 1] & 0xC0) != 0x80 || (p[i + 2] & 0xC0) != 0x80)
                return 0;
            i += 3;
        } else if (b >= 0xF1 && b <= 0xF3) {
            if (i + 2 >= n || (p[i] & 0xC0) != 0x80 ||
                (p[i + 1] & 0xC0) != 0x80 || (p[i + 2] & 0xC0) != 0x80)
                return 0;
            i += 3;
        } else if (b == 0xF4) {
            if (i + 2 >= n || p[i] > 0x8F || (p[i] & 0xC0) != 0x80 ||
                (p[i + 1] & 0xC0) != 0x80 || (p[i + 2] & 0xC0) != 0x80)
                return 0;
            i += 3;
        } else {
            return 0;
        }
    }
    return 1;
}

static int printable_ok(const uint8_t *p, size_t n)
{
    static const char set[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz"
        "0123456789 '()+,-./:=?";
    size_t i;
    for (i = 0; i < n; i++) {
        uint8_t c = p[i];
        if (c == 0 || !strchr(set, (int)c))
            return 0;
    }
    return 1;
}

static int time_field(const uint8_t *p, int a, int b)
{
    int v = 0, i;
    for (i = a; i <= b; i++) {
        if (p[i] < '0' || p[i] > '9')
            return -1;
        v = v * 10 + (p[i] - '0');
    }
    return v;
}

/* Canonical DER time shapes: UTCTime YYMMDDHHMMSSZ (13 octets),
   GeneralizedTime YYYYMMDDHHMMSSZ (15). No fractions, no zone offsets --
   DER has no other forms. 60 is allowed for the seconds (leap second). */
static int time_ok(const uint8_t *p, size_t n, int tag)
{
    int mo, dd, hh, mi, ss;
    if (tag == 23) {
        if (n != 13 || p[12] != 'Z' || time_field(p, 0, 1) < 0)
            return 0;
        mo = 2; dd = 4; hh = 6; mi = 8; ss = 10;
    } else {
        if (n != 15 || p[14] != 'Z' || time_field(p, 0, 3) < 0)
            return 0;
        mo = 4; dd = 6; hh = 8; mi = 10; ss = 12;
    }
    if (time_field(p, mo, mo + 1) < 1 || time_field(p, mo, mo + 1) > 12 ||
        time_field(p, dd, dd + 1) < 1 || time_field(p, dd, dd + 1) > 31 ||
        time_field(p, hh, hh + 1) > 23 ||
        time_field(p, mi, mi + 1) > 59 ||
        time_field(p, ss, ss + 1) > 60)
        return 0;
    return 1;
}

static JSValue asn1_decode_time(dr_t *d, size_t start, size_t end, int tag)
{
    if (!time_ok(d->p + start, end - start, tag))
        return JS_ThrowSyntaxError(d->ctx, tag == 23
            ? "ASN.1: UTCTime is not canonical YYMMDDHHMMSSZ"
            : "ASN.1: GeneralizedTime is not canonical YYYYMMDDHHMMSSZ");
    return JS_NewStringLen(d->ctx, (const char *)d->p + start, end - start);
}

static JSValue asn1_decode_utf8(dr_t *d, size_t start, size_t end)
{
    const uint8_t *p = d->p + start;
    size_t n = end - start;
    if (!utf8_ok(p, n))
        return JS_ThrowSyntaxError(d->ctx,
            "ASN.1: UTF8String is not well-formed UTF-8");
    return JS_NewStringLen(d->ctx, (const char *)p, n);
}

static JSValue asn1_decode_printable(dr_t *d, size_t start, size_t end)
{
    const uint8_t *p = d->p + start;
    size_t n = end - start;
    if (!printable_ok(p, n))
        return JS_ThrowSyntaxError(d->ctx,
            "ASN.1: PrintableString contains characters outside the printable "
            "set");
    return JS_NewStringLen(d->ctx, (const char *)p, n);
}

static JSValue asn1_decode_tlv(dr_t *d, size_t limit);
static JSValue asn1_node(JSContext *ctx, int cls, int tag, int constructed,
                         JSValue value);

static JSValue asn1_decode_children(dr_t *d, size_t end)
{
    JSValue arr = JS_NewArray(d->ctx);
    uint64_t count = 0;

    if (JS_IsException(arr))
        return arr;
    d->depth++;
    while (d->i < end) {
        JSValue el = asn1_decode_tlv(d, end);
        if (JS_IsException(el) ||
            JS_DefinePropertyValueUint32(d->ctx, arr, (uint32_t)count, el,
                                         JS_PROP_C_W_E) < 0) {
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

/* The node: { cls, tag, constructed, value }. Universal tags are interpreted;
   application/context/private tags and uninterpreted universal tags keep the
   raw content octets, so re-encoding a decode is byte-identical by
   construction. */
static JSValue asn1_decode_tlv(dr_t *d, size_t limit)
{
    int cls, constructed;
    uint32_t tag;
    size_t len, start, end;

    if (d->depth >= ASN1_MAX_DEPTH)
        return JS_ThrowRangeError(d->ctx, "ASN.1: nesting exceeds %d",
                                  ASN1_MAX_DEPTH);
    if (dr_tag(d, limit, &cls, &constructed, &tag) < 0)
        return JS_EXCEPTION;
    if (dr_len(d, limit, &len) < 0)
        return JS_EXCEPTION;
    start = d->i;
    end = start + len;
    if (cls == 0 && constructed && tag != 16 && tag != 17)
        return JS_ThrowSyntaxError(d->ctx,
            "ASN.1: a constructed UNIVERSAL tag %u is BER, not DER; only "
            "SEQUENCE and SET construct", (unsigned)tag);
    if (cls == 0 && (tag == 16 || tag == 17)) {
        JSValue kids = asn1_decode_children(d, end);
        if (JS_IsException(kids))
            return kids;
        return asn1_node(d->ctx, 0, (int)tag, 1, kids);
    }
    d->i = end;                 /* simple content: consume the span */
    {
        JSValue v = JS_UNDEFINED;
        if (cls == 0) {
            switch (tag) {
            case 1:  v = asn1_decode_bool(d, start, end); break;
            case 2:  v = asn1_decode_int(d, start, end); break;
            case 3:  v = asn1_decode_bits(d, start, end); break;
            case 4:  v = asn1_new_bytes(d->ctx, d->p + start, end - start); break;
            case 5:  v = asn1_decode_null(d, start, end); break;
            case 6:  v = asn1_decode_oid(d, start, end); break;
            case 12: v = asn1_decode_utf8(d, start, end); break;
            case 19: v = asn1_decode_printable(d, start, end); break;
            case 23:
            case 24: v = asn1_decode_time(d, start, end, (int)tag); break;
            default: v = asn1_new_bytes(d->ctx, d->p + start, end - start); break;
            }
        } else {
            v = asn1_new_bytes(d->ctx, d->p + start, end - start);
        }
        if (JS_IsException(v))
            return v;
        return asn1_node(d->ctx, cls, (int)tag, constructed, v);
    }
}

/* -------------------------------------------------------------- encode */

/* A node's numeric property; numbers only (no coercion of strings), within
   [lo, hi]. Integrality is checked for doubles. */
static int node_int(JSContext *ctx, JSValueConst v, const char *name,
                    int64_t lo, int64_t hi, int64_t *out)
{
    int64_t x;
    if (!JS_IsNumber(v)) {
        JS_ThrowTypeError(ctx, "ASN.1: node '%s' must be a number", name);
        return -1;
    }
    if (JS_TAG_IS_FLOAT64(JS_VALUE_GET_TAG(v))) {
        double dv;
        JS_ToFloat64(ctx, &dv, v);
        if (dv != floor(dv)) {
            JS_ThrowTypeError(ctx, "ASN.1: node '%s' must be an integer", name);
            return -1;
        }
    }
    if (JS_ToInt64(ctx, &x, v) < 0)
        return -1;
    if (x < lo || x > hi) {
        JS_ThrowRangeError(ctx, "ASN.1: node '%s' out of range", name);
        return -1;
    }
    *out = x;
    return 0;
}

static int asn1_enc_int(JSContext *ctx, JSValueConst v, uint8_t *content,
                        int *clen)
{
    int64_t iv;
    if (JS_IsBigInt(ctx, v)) {
        if (JS_ToBigInt64(ctx, &iv, v) < 0)
            return -1;
    } else if (JS_IsNumber(v)) {
        double d;
        if (JS_ToFloat64(ctx, &d, v) < 0)
            return -1;
        if (d != floor(d) || isinf(d)) {
            JS_ThrowTypeError(ctx, "ASN.1: INTEGER value must be an integer");
            return -1;
        }
        if (d < -9.223372036854775808e18 || d >= 9.223372036854775808e18) {
            JS_ThrowRangeError(ctx, "ASN.1: INTEGER out of the int64 range");
            return -1;
        }
        iv = (int64_t)d;
    } else {
        JS_ThrowTypeError(ctx, "ASN.1: INTEGER value must be a Number or "
                            "BigInt");
        return -1;
    }
    *clen = der_int_bytes(iv, content);
    return 0;
}

static int asn1_enc_bits(JSContext *ctx, JSValueConst vval, ab_t *out)
{
    const uint8_t *p;
    size_t n;
    JSValue hold = JS_UNDEFINED, vu, vb;
    int unused = 0;
    int br;
    uint8_t h;

    vu = JS_GetPropertyStr(ctx, vval, "unused");
    vb = JS_GetPropertyStr(ctx, vval, "bytes");
    if (JS_IsException(vu) || JS_IsException(vb)) {
        JS_FreeValue(ctx, vu);
        JS_FreeValue(ctx, vb);
        return -1;
    }
    if (!JS_IsUndefined(vu)) {
        if (!JS_IsNumber(vu)) {
            JS_FreeValue(ctx, vu);
            JS_FreeValue(ctx, vb);
            JS_ThrowTypeError(ctx, "ASN.1: BIT STRING 'unused' must be a "
                                "number");
            return -1;
        }
        if (JS_ToInt32(ctx, &unused, vu) < 0) {
            JS_FreeValue(ctx, vu);
            JS_FreeValue(ctx, vb);
            return -1;
        }
    }
    JS_FreeValue(ctx, vu);
    if (unused < 0 || unused > 7) {
        JS_FreeValue(ctx, vb);
        JS_ThrowRangeError(ctx, "ASN.1: BIT STRING unused-bits count %d is "
                            "not 0..7", unused);
        return -1;
    }
    br = asn1_bytes(ctx, vb, &p, &n, &hold);
    JS_FreeValue(ctx, vb);
    if (br < 0)
        return -1;
    if (br == 0) {
        JS_ThrowTypeError(ctx, "ASN.1: BIT STRING 'bytes' must be a "
                            "Uint8Array");
        return -1;
    }
    if (n == 0 && unused > 0) {
        JS_FreeValue(ctx, hold);
        JS_ThrowSyntaxError(ctx, "ASN.1: BIT STRING has unused bits but no "
                            "payload octet");
        return -1;
    }
    if (n > 0 && unused > 0 && (p[n - 1] & ((1u << unused) - 1)) != 0) {
        JS_FreeValue(ctx, hold);
        JS_ThrowSyntaxError(ctx, "ASN.1: BIT STRING trailing unused bits must "
                            "be zero in DER");
        return -1;
    }
    h = (uint8_t)unused;
    if (ab_write(out, &h, 1) < 0 || ab_write(out, p, n) < 0) {
        JS_FreeValue(ctx, hold);
        return -1;
    }
    JS_FreeValue(ctx, hold);
    return 0;
}

static int oid_emit_subid(ab_t *b, uint64_t v)
{
    uint8_t t[8];
    int n = 0, i;
    do { t[n++] = (uint8_t)(v & 0x7F); v >>= 7; } while (v);
    for (i = n - 1; i >= 0; i--) {
        uint8_t c = (uint8_t)(t[i] | (i ? 0x80 : 0));
        if (ab_write(b, &c, 1) < 0)
            return -1;
    }
    return 0;
}

static int asn1_enc_oid(JSContext *ctx, JSValueConst v, ab_t *out)
{
    size_t n;
    const char *s = JS_ToCStringLen(ctx, &n, v);
    uint32_t arcs[ASN1_MAX_OID_ARCS];
    int narc = 0;
    size_t i = 0;
    int rc = -1;

    if (!s)
        return -1;
    while (i < n) {
        uint64_t a = 0;
        int digits = 0;
        if (narc >= ASN1_MAX_OID_ARCS) {
            JS_ThrowRangeError(ctx, "ASN.1: more than %d OID arcs",
                               ASN1_MAX_OID_ARCS);
            goto done;
        }
        while (i < n && s[i] >= '0' && s[i] <= '9') {
            a = a * 10 + (uint64_t)(s[i] - '0');
            if (a > ASN1_ARC_MAX) {
                JS_ThrowRangeError(ctx, "ASN.1: OID arc exceeds 2^32-1");
                goto done;
            }
            i++;
            digits++;
        }
        if (digits == 0) {
            JS_ThrowSyntaxError(ctx, "ASN.1: malformed OID string");
            goto done;
        }
        arcs[narc++] = (uint32_t)a;
        if (i == n)
            break;
        if (s[i] != '.') {
            JS_ThrowSyntaxError(ctx, "ASN.1: malformed OID string");
            goto done;
        }
        i++;
    }
    if (narc < 2) {
        JS_ThrowSyntaxError(ctx, "ASN.1: an OID needs at least two arcs");
        goto done;
    }
    if (n > 0 && s[n - 1] == '.') {
        JS_ThrowSyntaxError(ctx, "ASN.1: malformed OID string (trailing dot)");
        goto done;
    }
    if (arcs[0] > 2) {
        JS_ThrowRangeError(ctx, "ASN.1: first OID arc must be 0, 1 or 2");
        goto done;
    }
    if (arcs[0] < 2 && arcs[1] >= 40) {
        JS_ThrowRangeError(ctx, "ASN.1: second arc must be < 40 when the "
                            "first arc is 0 or 1");
        goto done;
    }
    if (oid_emit_subid(out, (uint64_t)arcs[0] * 40 + arcs[1]) < 0)
        goto done;
    rc = 0;
    for (i = 2; i < (size_t)narc; i++)
        if (oid_emit_subid(out, arcs[i]) < 0) {
            rc = -1;
            break;
        }
done:
    JS_FreeCString(ctx, s);
    return rc;
}

static int asn1_enc_string(JSContext *ctx, JSValueConst v, int tag, ab_t *out)
{
    size_t n;
    const char *s = JS_ToCStringLen(ctx, &n, v);
    int rc;

    if (!s)
        return -1;
    if (tag == 19) {
        if (!printable_ok((const uint8_t *)s, n)) {
            JS_FreeCString(ctx, s);
            JS_ThrowTypeError(ctx, "ASN.1: PrintableString contains characters "
                                "outside the printable set");
            return -1;
        }
    } else if (tag == 23 || tag == 24) {
        if (!time_ok((const uint8_t *)s, n, tag)) {
            JS_FreeCString(ctx, s);
            JS_ThrowSyntaxError(ctx, tag == 23
                ? "ASN.1: UTCTime must be canonical YYMMDDHHMMSSZ"
                : "ASN.1: GeneralizedTime must be canonical "
                  "YYYYMMDDHHMMSSZ");
            return -1;
        }
    }
    rc = ab_write(out, (const uint8_t *)s, n);
    JS_FreeCString(ctx, s);
    return rc;
}

static int asn1_enc_node(JSContext *ctx, JSValueConst node, ab_t *out,
                         int depth);

/* X.690 11.6: the encodings of a SET OF's components appear in ascending
 * order -- shortest first, and lexicographically by unsigned octet comparison
 * among equals. The sort key is the ENCODED form, so children are encoded
 * individually and the finished TLVs are sorted, never the JS values. */
static int asn1_der_set_cmp(const void *a, const void *b)
{
    const ab_t *x = (const ab_t *)a, *y = (const ab_t *)b;
    size_t n = x->n < y->n ? x->n : y->n;
    int c = n ? memcmp(x->p, y->p, n) : 0;
    if (c)
        return c;
    return x->n < y->n ? -1 : (x->n > y->n ? 1 : 0);
}

static int asn1_enc_children(JSContext *ctx, JSValueConst arr, ab_t *out,
                             int depth)
{
    JSValue lv = JS_GetPropertyStr(ctx, arr, "length");
    int64_t len, k;

    if (JS_IsException(lv) || JS_ToInt64(ctx, &len, lv) < 0) {
        JS_FreeValue(ctx, lv);
        return -1;
    }
    JS_FreeValue(ctx, lv);
    if (len < 0 || len > ASN1_MAX_CHILDREN) {
        JS_ThrowRangeError(ctx, "ASN.1: too many children (%lld)",
                           (long long)len);
        return -1;
    }
    for (k = 0; k < len; k++) {
        JSValue el = JS_GetPropertyUint32(ctx, arr, (uint32_t)k);
        int rc;
        if (JS_IsException(el))
            return -1;
        rc = asn1_enc_node(ctx, el, out, depth + 1);
        JS_FreeValue(ctx, el);
        if (rc < 0)
            return -1;
    }
    return 0;
}

/* A SET (tag 17): encode each child into its own buffer, then write them in
 * DER order (asn1_der_set_cmp). Equal TLVs are byte-identical, so qsort's
 * instability cannot reorder anything observably. */
static int asn1_enc_set_children(JSContext *ctx, JSValueConst arr, ab_t *out,
                                 int depth)
{
    JSValue lv = JS_GetPropertyStr(ctx, arr, "length");
    int64_t len, k;
    ab_t *kids;
    size_t total = 0;
    int rc = -1;

    if (JS_IsException(lv) || JS_ToInt64(ctx, &len, lv) < 0) {
        JS_FreeValue(ctx, lv);
        return -1;
    }
    JS_FreeValue(ctx, lv);
    if (len < 0 || len > ASN1_MAX_CHILDREN) {
        JS_ThrowRangeError(ctx, "ASN.1: too many children (%lld)",
                           (long long)len);
        return -1;
    }
    kids = (ab_t *)calloc((size_t)len ? (size_t)len : 1, sizeof(*kids));
    if (!kids) {
        JS_ThrowOutOfMemory(ctx);
        return -1;
    }
    for (k = 0; k < len; k++) {
        JSValue el = JS_GetPropertyUint32(ctx, arr, (uint32_t)k);
        int rc2;
        if (JS_IsException(el))
            goto out;
        ab_init(&kids[k]);
        rc2 = asn1_enc_node(ctx, el, &kids[k], depth + 1);
        JS_FreeValue(ctx, el);
        if (rc2 < 0) {
            if (kids[k].oom)
                JS_ThrowOutOfMemory(ctx);
            goto out;
        }
        if (kids[k].oom) {
            JS_ThrowOutOfMemory(ctx);
            goto out;
        }
    }
    if (len > 1)
        qsort(kids, (size_t)len, sizeof(*kids), asn1_der_set_cmp);
    for (k = 0; k < len; k++)
        total += kids[k].n;
    rc = der_length(out, total);
    for (k = 0; rc == 0 && k < len; k++)
        rc = ab_write(out, kids[k].p, kids[k].n);
out:
    for (k = 0; k < len; k++)
        ab_free(&kids[k]);
    free(kids);
    return rc;
}

static int asn1_enc_node(JSContext *ctx, JSValueConst node, ab_t *out,
                         int depth)
{
    JSValue vcls, vtag, vcon, vval;
    int cls = 0, constructed = 0, isa;
    int64_t itag;
    uint32_t tag;
    int rc;

    if (depth >= ASN1_MAX_DEPTH) {
        JS_ThrowRangeError(ctx, "ASN.1: encode nesting exceeds %d",
                           ASN1_MAX_DEPTH);
        return -1;
    }
    if (!JS_IsObject(node) || JS_IsArray(ctx, node) == 1) {
        JS_ThrowTypeError(ctx, "ASN1.encode: a node must be a plain object");
        return -1;
    }
    vcls = JS_GetPropertyStr(ctx, node, "cls");
    vtag = JS_GetPropertyStr(ctx, node, "tag");
    vcon = JS_GetPropertyStr(ctx, node, "constructed");
    vval = JS_GetPropertyStr(ctx, node, "value");
    if (JS_IsException(vcls) || JS_IsException(vtag) ||
        JS_IsException(vcon) || JS_IsException(vval))
        goto fail;
    if (!JS_IsUndefined(vcls)) {
        if (node_int(ctx, vcls, "cls", 0, 3, &itag) < 0)
            goto fail;
        cls = (int)itag;
    }
    if (node_int(ctx, vtag, "tag", 0, ASN1_MAX_TAG, &itag) < 0)
        goto fail;
    tag = (uint32_t)itag;
    if (!JS_IsUndefined(vcon)) {
        int t = JS_ToBool(ctx, vcon);
        if (t < 0)
            goto fail;
        constructed = t;
    }
    isa = JS_IsArray(ctx, vval);
    if (isa < 0)
        goto fail;
    if (isa == 1) {
        ab_t tmp;
        if (!constructed) {
            JS_ThrowTypeError(ctx, "ASN.1: an array value requires "
                                "constructed:true");
            goto fail;
        }
        if (cls == 0 && tag != 16 && tag != 17) {
            JS_ThrowTypeError(ctx, "ASN.1: constructed content is not DER for "
                                "universal tag %u", tag);
            goto fail;
        }
        ab_init(&tmp);
        if (cls == 0 && tag == 17) {
            /* SET: DER sorts the ENCODED children (X.690 11.6), so the set
             * encoder emits the ident, then the sorted content's length and
             * bytes -- there is no temp payload to copy through. */
            rc = der_ident(out, cls, 1, tag);
            if (rc == 0)
                rc = asn1_enc_set_children(ctx, vval, out, depth);
            ab_free(&tmp);
            if (rc < 0)
                goto fail;
        } else {
            if (asn1_enc_children(ctx, vval, &tmp, depth) < 0) {
                if (tmp.oom)
                    JS_ThrowOutOfMemory(ctx);
                ab_free(&tmp);
                goto fail;
            }
            rc = der_ident(out, cls, 1, tag);
            if (rc == 0) rc = der_length(out, tmp.n);
            if (rc == 0) rc = ab_write(out, tmp.p, tmp.n);
            ab_free(&tmp);
            if (rc < 0)
                goto fail;
        }
    } else if (cls != 0) {
        /* application/context/private: the content is raw octets, verbatim */
        const uint8_t *raw;
        size_t rn;
        JSValue hold = JS_UNDEFINED;
        int br = asn1_bytes(ctx, vval, &raw, &rn, &hold);
        if (br < 0)
            goto fail;
        if (br == 0) {
            JS_ThrowTypeError(ctx, "ASN.1: content of a class %d tag %u node "
                                "must be a Uint8Array", cls, tag);
            goto fail;
        }
        rc = der_ident(out, cls, constructed, tag);
        if (rc == 0) rc = der_length(out, rn);
        if (rc == 0) rc = ab_write(out, raw, rn);
        JS_FreeValue(ctx, hold);
        if (rc < 0)
            goto fail;
    } else {
        switch (tag) {
        case 1: {   /* BOOLEAN */
            int t = JS_ToBool(ctx, vval);
            uint8_t c;
            if (t < 0)
                goto fail;
            c = t ? 0xFF : 0x00;
            rc = der_ident(out, 0, 0, 1);
            if (rc == 0) rc = der_length(out, 1);
            if (rc == 0) rc = ab_write(out, &c, 1);
            if (rc < 0)
                goto fail;
            break;
        }
        case 2: {   /* INTEGER */
            uint8_t content[ASN1_MAX_INT_BYTES];
            int clen;
            if (asn1_enc_int(ctx, vval, content, &clen) < 0)
                goto fail;
            rc = der_ident(out, 0, 0, 2);
            if (rc == 0) rc = der_length(out, (size_t)clen);
            if (rc == 0) rc = ab_write(out, content, (size_t)clen);
            if (rc < 0)
                goto fail;
            break;
        }
        case 3: {   /* BIT STRING: value is {unused, bytes} */
            ab_t tmp;
            if (!JS_IsObject(vval) || JS_IsArray(ctx, vval) == 1) {
                JS_ThrowTypeError(ctx, "ASN.1: BIT STRING value must be "
                                    "{unused, bytes}");
                goto fail;
            }
            ab_init(&tmp);
            if (asn1_enc_bits(ctx, vval, &tmp) < 0) {
                if (tmp.oom)
                    JS_ThrowOutOfMemory(ctx);
                ab_free(&tmp);
                goto fail;
            }
            rc = der_ident(out, 0, 0, 3);
            if (rc == 0) rc = der_length(out, tmp.n);
            if (rc == 0) rc = ab_write(out, tmp.p, tmp.n);
            ab_free(&tmp);
            if (rc < 0)
                goto fail;
            break;
        }
        case 4: {   /* OCTET STRING: content is the payload */
            const uint8_t *raw;
            size_t rn;
            JSValue hold = JS_UNDEFINED;
            int br = asn1_bytes(ctx, vval, &raw, &rn, &hold);
            if (br < 0)
                goto fail;
            if (br == 0) {
                JS_ThrowTypeError(ctx, "ASN.1: OCTET STRING value must be a "
                                    "Uint8Array");
                goto fail;
            }
            rc = der_ident(out, 0, 0, 4);
            if (rc == 0) rc = der_length(out, rn);
            if (rc == 0) rc = ab_write(out, raw, rn);
            JS_FreeValue(ctx, hold);
            if (rc < 0)
                goto fail;
            break;
        }
        case 5: {   /* NULL */
            if (!JS_IsNull(vval) && !JS_IsUndefined(vval)) {
                JS_ThrowTypeError(ctx, "ASN.1: NULL value must be null");
                goto fail;
            }
            rc = der_ident(out, 0, 0, 5);
            if (rc == 0) rc = der_length(out, 0);
            if (rc < 0)
                goto fail;
            break;
        }
        case 6: {   /* OID: dotted string */
            ab_t tmp;
            if (!JS_IsString(vval)) {
                JS_ThrowTypeError(ctx, "ASN.1: OID value must be a dotted "
                                    "string");
                goto fail;
            }
            ab_init(&tmp);
            if (asn1_enc_oid(ctx, vval, &tmp) < 0) {
                if (tmp.oom)
                    JS_ThrowOutOfMemory(ctx);
                ab_free(&tmp);
                goto fail;
            }
            rc = der_ident(out, 0, 0, 6);
            if (rc == 0) rc = der_length(out, tmp.n);
            if (rc == 0) rc = ab_write(out, tmp.p, tmp.n);
            ab_free(&tmp);
            if (rc < 0)
                goto fail;
            break;
        }
        case 12: case 19: case 23: case 24: {  /* string types */
            ab_t tmp;
            if (!JS_IsString(vval)) {
                JS_ThrowTypeError(ctx, "ASN.1: tag %u needs a string value",
                                  tag);
                goto fail;
            }
            ab_init(&tmp);
            if (asn1_enc_string(ctx, vval, (int)tag, &tmp) < 0) {
                if (tmp.oom)
                    JS_ThrowOutOfMemory(ctx);
                ab_free(&tmp);
                goto fail;
            }
            rc = der_ident(out, 0, 0, tag);
            if (rc == 0) rc = der_length(out, tmp.n);
            if (rc == 0) rc = ab_write(out, tmp.p, tmp.n);
            ab_free(&tmp);
            if (rc < 0)
                goto fail;
            break;
        }
        default: {  /* an uninterpreted universal tag: raw content octets */
            const uint8_t *raw;
            size_t rn;
            JSValue hold = JS_UNDEFINED;
            int br = asn1_bytes(ctx, vval, &raw, &rn, &hold);
            if (br < 0)
                goto fail;
            if (br == 0) {
                JS_ThrowTypeError(ctx, "ASN.1: universal tag %u needs a "
                                    "Uint8Array value", tag);
                goto fail;
            }
            rc = der_ident(out, 0, 0, tag);
            if (rc == 0) rc = der_length(out, rn);
            if (rc == 0) rc = ab_write(out, raw, rn);
            JS_FreeValue(ctx, hold);
            if (rc < 0)
                goto fail;
            break;
        }
        }
    }
    JS_FreeValue(ctx, vcls);
    JS_FreeValue(ctx, vtag);
    JS_FreeValue(ctx, vcon);
    JS_FreeValue(ctx, vval);
    return 0;

fail:
    JS_FreeValue(ctx, vcls);
    JS_FreeValue(ctx, vtag);
    JS_FreeValue(ctx, vcon);
    JS_FreeValue(ctx, vval);
    return -1;
}

/* ------------------------------------------------------------- node maker */

/* { cls, tag, constructed, value } with C_W_E, exactly the decode shape. The
   value is CONSUMED on every path (JS_DefinePropertyValue frees it even when
   a define fails -- see object/property_set_convert.inc.c:1017). */
static JSValue asn1_node(JSContext *ctx, int cls, int tag, int constructed,
                         JSValue value)
{
    JSValue o = JS_NewObject(ctx);
    JSValue vc, vt, vk;

    if (JS_IsException(o)) {
        JS_FreeValue(ctx, value);
        return o;
    }
    vc = JS_NewInt32(ctx, cls);
    vt = JS_NewInt32(ctx, tag);
    vk = JS_NewBool(ctx, constructed);
    if (JS_DefinePropertyValueStr(ctx, o, "cls", vc, JS_PROP_C_W_E) < 0 ||
        JS_DefinePropertyValueStr(ctx, o, "tag", vt, JS_PROP_C_W_E) < 0 ||
        JS_DefinePropertyValueStr(ctx, o, "constructed", vk,
                                  JS_PROP_C_W_E) < 0) {
        JS_FreeValue(ctx, value);
        JS_FreeValue(ctx, o);
        return JS_EXCEPTION;
    }
    if (JS_DefinePropertyValueStr(ctx, o, "value", value,
                                  JS_PROP_C_W_E) < 0) {
        JS_FreeValue(ctx, o);
        return JS_EXCEPTION;   /* value consumed by the failed define */
    }
    return o;
}

/* ------------------------------------------------------------- JS entry */

static JSValue asn1_decode(JSContext *ctx, JSValueConst this_val,
                           int argc, JSValueConst *argv)
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
                                      "the value", (unsigned)(d.n - d.i));
    }
    JS_FreeValue(ctx, hold);
    return out;
}

static JSValue asn1_encode(JSContext *ctx, JSValueConst this_val,
                           int argc, JSValueConst *argv)
{
    ab_t b;
    JSValue out;

    (void)this_val;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "ASN1.encode(node): a node is required");
    ab_init(&b);
    if (asn1_enc_node(ctx, argv[0], &b, 0) < 0 || b.oom) {
        if (b.oom)
            JS_ThrowOutOfMemory(ctx);
        ab_free(&b);
        return JS_EXCEPTION;
    }
    out = asn1_new_bytes(ctx, b.p, b.n);
    ab_free(&b);
    return out;
}

static int asn1_arg_tag(JSContext *ctx, JSValueConst v, uint32_t *out)
{
    int64_t t;
    if (!JS_IsNumber(v)) {
        JS_ThrowTypeError(ctx, "ASN.1: tag number must be a number");
        return -1;
    }
    if (JS_ToInt64(ctx, &t, v) < 0)
        return -1;
    if (t < 0 || t > ASN1_MAX_TAG) {
        JS_ThrowRangeError(ctx, "ASN.1: tag number out of 0..2^31-1");
        return -1;
    }
    *out = (uint32_t)t;
    return 0;
}

/* The constructors are deliberately shallow: encode is the single validation
   choke point, so a bad value fails at encode with one clear reason. */

static JSValue asn1_seq(JSContext *ctx, JSValueConst this_val,
                        int argc, JSValueConst *argv)
{
    int isa;
    (void)this_val;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "ASN1.seq(children): children are "
                                    "required");
    isa = JS_IsArray(ctx, argv[0]);
    if (isa < 0)
        return JS_EXCEPTION;
    if (isa != 1)
        return JS_ThrowTypeError(ctx, "ASN1.seq: children must be an array");
    return asn1_node(ctx, 0, 16, 1, JS_DupValue(ctx, argv[0]));
}

static JSValue asn1_set(JSContext *ctx, JSValueConst this_val,
                        int argc, JSValueConst *argv)
{
    int isa;
    (void)this_val;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "ASN1.set(children): children are "
                                    "required");
    isa = JS_IsArray(ctx, argv[0]);
    if (isa < 0)
        return JS_EXCEPTION;
    if (isa != 1)
        return JS_ThrowTypeError(ctx, "ASN1.set: children must be an array");
    return asn1_node(ctx, 0, 17, 1, JS_DupValue(ctx, argv[0]));
}

static JSValue asn1_int(JSContext *ctx, JSValueConst this_val,
                        int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "ASN1.int(value): a value is required");
    return asn1_node(ctx, 0, 2, 0, JS_DupValue(ctx, argv[0]));
}

static JSValue asn1_bool(JSContext *ctx, JSValueConst this_val,
                         int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "ASN1.bool(value): a value is required");
    return asn1_node(ctx, 0, 1, 0, JS_DupValue(ctx, argv[0]));
}

static JSValue asn1_null(JSContext *ctx, JSValueConst this_val,
                         int argc, JSValueConst *argv)
{
    (void)this_val;
    (void)argc;
    (void)argv;
    return asn1_node(ctx, 0, 5, 0, JS_NULL);
}

static JSValue asn1_octets(JSContext *ctx, JSValueConst this_val,
                           int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "ASN1.octets(bytes): bytes are required");
    return asn1_node(ctx, 0, 4, 0, JS_DupValue(ctx, argv[0]));
}

static JSValue asn1_bit_string(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv)
{
    JSValue obj, vu, vb;
    int unused = 0;
    (void)this_val;
    if (argc < 2)
        return JS_ThrowTypeError(ctx, "ASN1.bitString(bytes, unused): two "
                                    "arguments are required");
    if (!JS_IsNumber(argv[1]))
        return JS_ThrowTypeError(ctx, "ASN1.bitString: unused must be a "
                                    "number");
    if (JS_ToInt32(ctx, &unused, argv[1]) < 0)
        return JS_EXCEPTION;
    if (unused < 0 || unused > 7)
        return JS_ThrowRangeError(ctx, "ASN1.bitString: unused-bits count "
                                    "must be 0..7");
    obj = JS_NewObject(ctx);
    if (JS_IsException(obj))
        return obj;
    vu = JS_NewInt32(ctx, unused);
    vb = JS_DupValue(ctx, argv[0]);
    if (JS_DefinePropertyValueStr(ctx, obj, "unused", vu,
                                  JS_PROP_C_W_E) < 0) {
        JS_FreeValue(ctx, vb);
        JS_FreeValue(ctx, obj);
        return JS_EXCEPTION;
    }
    if (JS_DefinePropertyValueStr(ctx, obj, "bytes", vb,
                                  JS_PROP_C_W_E) < 0) {
        JS_FreeValue(ctx, obj);
        return JS_EXCEPTION;
    }
    return asn1_node(ctx, 0, 3, 0, obj);
}

static JSValue asn1_str_node(JSContext *ctx, int tag, JSValueConst v)
{
    return asn1_node(ctx, 0, tag, 0, JS_DupValue(ctx, v));
}

static JSValue asn1_oid(JSContext *ctx, JSValueConst this_val,
                        int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "ASN1.oid(oid): an OID string is "
                                    "required");
    return asn1_str_node(ctx, 6, argv[0]);
}

static JSValue asn1_utf8(JSContext *ctx, JSValueConst this_val,
                         int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "ASN1.utf8(str): a string is required");
    return asn1_str_node(ctx, 12, argv[0]);
}

static JSValue asn1_printable(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "ASN1.printable(str): a string is "
                                     "required");
    return asn1_str_node(ctx, 19, argv[0]);
}

static JSValue asn1_utc_time(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "ASN1.utcTime(str): a string is "
                                     "required");
    return asn1_str_node(ctx, 23, argv[0]);
}

static JSValue asn1_gen_time(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "ASN1.generalizedTime(str): a string is "
                                     "required");
    return asn1_str_node(ctx, 24, argv[0]);
}

static JSValue asn1_context(JSContext *ctx, JSValueConst this_val,
                            int argc, JSValueConst *argv)
{
    uint32_t tag;
    (void)this_val;
    if (argc < 2)
        return JS_ThrowTypeError(ctx, "ASN1.context(tag, content): two "
                                    "arguments are required");
    if (asn1_arg_tag(ctx, argv[0], &tag) < 0)
        return JS_EXCEPTION;
    return asn1_node(ctx, 2, (int)tag, 0, JS_DupValue(ctx, argv[1]));
}

static JSValue asn1_context_c(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv)
{
    uint32_t tag;
    int isa;
    (void)this_val;
    if (argc < 2)
        return JS_ThrowTypeError(ctx, "ASN1.contextC(tag, children): two "
                                    "arguments are required");
    if (asn1_arg_tag(ctx, argv[0], &tag) < 0)
        return JS_EXCEPTION;
    isa = JS_IsArray(ctx, argv[1]);
    if (isa < 0)
        return JS_EXCEPTION;
    if (isa != 1)
        return JS_ThrowTypeError(ctx, "ASN1.contextC: children must be an "
                                    "array");
    return asn1_node(ctx, 2, (int)tag, 1, JS_DupValue(ctx, argv[1]));
}

/* ------------------------------------------------------------ registration */

static const JSCFunctionListEntry asn1_methods[] = {
    JS_CFUNC_DEF("decode", 1, asn1_decode),
    JS_CFUNC_DEF("encode", 1, asn1_encode),
    JS_CFUNC_DEF("seq", 1, asn1_seq),
    JS_CFUNC_DEF("set", 1, asn1_set),
    JS_CFUNC_DEF("int", 1, asn1_int),
    JS_CFUNC_DEF("bool", 1, asn1_bool),
    JS_CFUNC_DEF("null", 0, asn1_null),
    JS_CFUNC_DEF("octets", 1, asn1_octets),
    JS_CFUNC_DEF("bitString", 2, asn1_bit_string),
    JS_CFUNC_DEF("oid", 1, asn1_oid),
    JS_CFUNC_DEF("utf8", 1, asn1_utf8),
    JS_CFUNC_DEF("printable", 1, asn1_printable),
    JS_CFUNC_DEF("utcTime", 1, asn1_utc_time),
    JS_CFUNC_DEF("generalizedTime", 1, asn1_gen_time),
    JS_CFUNC_DEF("context", 2, asn1_context),
    JS_CFUNC_DEF("contextC", 2, asn1_context_c),
};

static const JSCFunctionListEntry asn1_exports[] = {
    JS_OBJECT_DEF("ASN1", asn1_methods, countof(asn1_methods),
                  JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE),
};

int dyn_asn1_register(JSContext *ctx, JSModuleDef *m)
{
    return JS_SetModuleExportList(ctx, m, asn1_exports, countof(asn1_exports));
}

int dyn_asn1_add_exports(JSContext *ctx, JSModuleDef *m)
{
    return JS_AddModuleExportList(ctx, m, asn1_exports, countof(asn1_exports));
}

#endif /* CONFIG_NATIVE_MODULES && CONFIG_NATIVE_MODULE_VSERIALIZE */