/* dyna:serialize -- one graph walker, three wire formats and a value hash.
   MessagePack and RFC 8949 CBOR share the walker; structuredClone fills the
   engine gap the guide names; ValueHash is canonical CBOR through xxh64.
   NOT dyna-serialize.c, which is dyna:structures' container persistence.
   Full API: docs/dynajs-guide/API.md. */
#include "dyna-nat.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_VSERIALIZE)

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifndef countof
#define countof(x) (sizeof(x) / sizeof((x)[0]))
#endif

uint64_t dyn_xxh64(const void *data, size_t len, uint64_t seed);

#define VS_MAX_DEPTH  256
#define VS_MAX_ITEMS  (1u << 26)        /* a length prefix is not a promise */

enum { FMT_MSGPACK, FMT_CBOR };

typedef struct { uint8_t *p; size_t n, cap; int oom; } vb_t;

static void vb_init(vb_t *b) { b->p = NULL; b->n = b->cap = 0; b->oom = 0; }
static void vb_free(vb_t *b) { free(b->p); b->p = NULL; b->n = b->cap = 0; }

static void vb_write(vb_t *b, const void *p, size_t n)
{
    if (b->oom || !n)
        return;
    if (b->n + n > b->cap) {
        size_t nc = b->cap ? b->cap : 128;
        uint8_t *np;
        while (nc < b->n + n) {
            if (nc < (1u << 16))      nc *= 2;
            else if (nc < (1u << 20)) nc += nc / 2;
            else                      nc += nc / 4;
        }
        np = (uint8_t *)realloc(b->p, nc);
        if (!np) { b->oom = 1; return; }
        b->p = np; b->cap = nc;
    }
    memcpy(b->p + b->n, p, n);
    b->n += n;
}

static void vb_put(vb_t *b, uint8_t c) { vb_write(b, &c, 1); }

static void vb_be(vb_t *b, uint64_t v, int bytes)
{
    uint8_t t[8];
    int i;
    for (i = 0; i < bytes; i++)
        t[i] = (uint8_t)(v >> ((bytes - 1 - i) * 8));
    vb_write(b, t, (size_t)bytes);
}

/* --------------------------------------------------------------- encoding */

typedef struct {
    JSContext *ctx;
    vb_t       b;
    int        fmt, depth, sorted;
    JSValue   *seen;                    /* the ancestor chain, for cycles */
    int        nseen, cseen;
} venc_t;

static int venc_value(venc_t *e, JSValueConst v);

/* CBOR major type + argument, and MessagePack's equivalent int headers. */
static void cbor_head(vb_t *b, int major, uint64_t n)
{
    int m = major << 5;
    if (n < 24)              vb_put(b, (uint8_t)(m | n));
    else if (n <= 0xFF)      { vb_put(b, (uint8_t)(m | 24)); vb_be(b, n, 1); }
    else if (n <= 0xFFFF)    { vb_put(b, (uint8_t)(m | 25)); vb_be(b, n, 2); }
    else if (n <= 0xFFFFFFFF){ vb_put(b, (uint8_t)(m | 26)); vb_be(b, n, 4); }
    else                     { vb_put(b, (uint8_t)(m | 27)); vb_be(b, n, 8); }
}

static void venc_int(venc_t *e, int64_t v)
{
    if (e->fmt == FMT_CBOR) {
        if (v < 0) cbor_head(&e->b, 1, (uint64_t)(-(v + 1)));
        else       cbor_head(&e->b, 0, (uint64_t)v);
        return;
    }
    if (v >= 0 && v < 128) { vb_put(&e->b, (uint8_t)v); return; }
    if (v < 0 && v >= -32) { vb_put(&e->b, (uint8_t)(0xE0 | (v + 32))); return; }
    if (v >= 0) {
        if (v <= 0xFF)            { vb_put(&e->b, 0xCC); vb_be(&e->b, (uint64_t)v, 1); }
        else if (v <= 0xFFFF)     { vb_put(&e->b, 0xCD); vb_be(&e->b, (uint64_t)v, 2); }
        else if (v <= 0xFFFFFFFF) { vb_put(&e->b, 0xCE); vb_be(&e->b, (uint64_t)v, 4); }
        else                      { vb_put(&e->b, 0xCF); vb_be(&e->b, (uint64_t)v, 8); }
        return;
    }
    if (v >= -128)            { vb_put(&e->b, 0xD0); vb_be(&e->b, (uint64_t)(uint8_t)v, 1); }
    else if (v >= -32768)     { vb_put(&e->b, 0xD1); vb_be(&e->b, (uint64_t)(uint16_t)v, 2); }
    else if (v >= -2147483648LL) { vb_put(&e->b, 0xD2); vb_be(&e->b, (uint64_t)(uint32_t)v, 4); }
    else                      { vb_put(&e->b, 0xD3); vb_be(&e->b, (uint64_t)v, 8); }
}

static void venc_len(venc_t *e, int kind, uint64_t n)
{
    /* kind: 0 string, 1 bytes, 2 array, 3 map */
    if (e->fmt == FMT_CBOR) {
        static const int MAJ[4] = { 3, 2, 4, 5 };
        cbor_head(&e->b, MAJ[kind], n);
        return;
    }
    switch (kind) {
    case 0:
        if (n < 32) vb_put(&e->b, (uint8_t)(0xA0 | n));
        else if (n <= 0xFF)   { vb_put(&e->b, 0xD9); vb_be(&e->b, n, 1); }
        else if (n <= 0xFFFF) { vb_put(&e->b, 0xDA); vb_be(&e->b, n, 2); }
        else                  { vb_put(&e->b, 0xDB); vb_be(&e->b, n, 4); }
        break;
    case 1:
        if (n <= 0xFF)        { vb_put(&e->b, 0xC4); vb_be(&e->b, n, 1); }
        else if (n <= 0xFFFF) { vb_put(&e->b, 0xC5); vb_be(&e->b, n, 2); }
        else                  { vb_put(&e->b, 0xC6); vb_be(&e->b, n, 4); }
        break;
    case 2:
        if (n < 16) vb_put(&e->b, (uint8_t)(0x90 | n));
        else if (n <= 0xFFFF) { vb_put(&e->b, 0xDC); vb_be(&e->b, n, 2); }
        else                  { vb_put(&e->b, 0xDD); vb_be(&e->b, n, 4); }
        break;
    default:
        if (n < 16) vb_put(&e->b, (uint8_t)(0x80 | n));
        else if (n <= 0xFFFF) { vb_put(&e->b, 0xDE); vb_be(&e->b, n, 2); }
        else                  { vb_put(&e->b, 0xDF); vb_be(&e->b, n, 4); }
        break;
    }
}

/* The ANCESTOR chain, not everything seen: a value repeated in two branches is
   not a cycle, and refusing it would reject perfectly ordinary data. */
static int venc_push(venc_t *e, JSValueConst v)
{
    int i;
    for (i = 0; i < e->nseen; i++)
        if (JS_VALUE_GET_PTR(e->seen[i]) == JS_VALUE_GET_PTR(v)) {
            JS_ThrowTypeError(e->ctx, "encode: the value contains a cycle, and "
                                      "neither wire format can express one");
            return -1;
        }
    if (e->nseen == e->cseen) {
        int nc = e->cseen ? e->cseen * 2 : 16;
        JSValue *np = (JSValue *)realloc(e->seen, (size_t)nc * sizeof *np);
        if (!np) { JS_ThrowOutOfMemory(e->ctx); return -1; }
        e->seen = np; e->cseen = nc;
    }
    e->seen[e->nseen++] = (JSValue)v;
    return 0;
}

static int venc_bytes_of(JSContext *ctx, JSValueConst v, const uint8_t **pp,
                         size_t *pn, JSValue *hold)
{
    size_t off, len, bpe, ab;
    uint8_t *base;
    JSValue buf = JS_GetArrayBufferView(ctx, v, &off, &len, &bpe);

    if (JS_IsException(buf)) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        return 0;
    }
    if (bpe != 1) {
        JS_FreeValue(ctx, buf);
        return 0;                       /* a wider view is an ordinary object */
    }
    base = JS_GetArrayBuffer(ctx, &ab, buf);
    if (!base || off > ab || len > ab - off) {
        JS_FreeValue(ctx, buf);
        JS_FreeValue(ctx, JS_GetException(ctx));
        return 0;
    }
    *hold = buf;
    *pp = base + off;
    *pn = len;
    return 1;
}

static int venc_string(venc_t *e, JSValueConst v)
{
    size_t n;
    const char *s = JS_ToCStringLen(e->ctx, &n, v);
    if (!s)
        return -1;
    venc_len(e, 0, n);
    vb_write(&e->b, s, n);
    JS_FreeCString(e->ctx, s);
    return 0;
}

static int venc_array(venc_t *e, JSValueConst v)
{
    JSValue lv = JS_GetPropertyStr(e->ctx, v, "length");
    int64_t len = 0, i;

    if (JS_IsException(lv) || JS_ToInt64(e->ctx, &len, lv) < 0) {
        JS_FreeValue(e->ctx, lv);
        return -1;
    }
    JS_FreeValue(e->ctx, lv);
    if (len < 0 || len > VS_MAX_ITEMS) {
        JS_ThrowRangeError(e->ctx, "encode: array too long");
        return -1;
    }
    venc_len(e, 2, (uint64_t)len);
    for (i = 0; i < len; i++) {
        JSValue el = JS_GetPropertyUint32(e->ctx, v, (uint32_t)i);
        int rc;
        if (JS_IsException(el))
            return -1;
        rc = venc_value(e, el);
        JS_FreeValue(e->ctx, el);
        if (rc < 0)
            return -1;
    }
    return 0;
}

static int venc_atom_less(JSContext *ctx, JSAtom a, JSAtom b)
{
    const char *sa = JS_AtomToCString(ctx, a), *sb = JS_AtomToCString(ctx, b);
    int r = 0;
    if (sa && sb) {
        size_t la = strlen(sa), lb = strlen(sb);
        /* RFC 8949 canonical order: shorter first, then bytewise. */
        r = la != lb ? (la < lb) : (memcmp(sa, sb, la) < 0);
    }
    if (sa) JS_FreeCString(ctx, sa);
    if (sb) JS_FreeCString(ctx, sb);
    return r;
}

#define VENC_FAST_KEYS 128       /* on the stack; beyond it, the general path */

static int venc_object(venc_t *e, JSValueConst v)
{
    JSPropertyEnum stackbuf[VENC_FAST_KEYS], *tab = stackbuf;
    uint32_t len = 0, k;
    int rc = 0, owns_tab = 0;

    /* The name table dominates: enumerating 300 keys cost 33 ns each against
       0.1 ns to write one. The fast form skips the sort and the malloc. */
    rc = JS_GetOwnFastProps(e->ctx, v, stackbuf, VENC_FAST_KEYS, &len);
    if (rc == -2) {                     /* wider than the stack buffer */
        tab = (JSPropertyEnum *)malloc(len * sizeof *tab);
        if (!tab)
            return -1;
        owns_tab = 2;
        rc = JS_GetOwnFastProps(e->ctx, v, tab, len, &len);
    }
    if (rc < 0) {
        if (owns_tab == 2)
            free(tab);
        tab = NULL;
        owns_tab = 1;
        if (JS_GetOwnPropertyNames(e->ctx, &tab, &len, v,
                                   JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) < 0)
            return -1;
    }
    rc = 0;
    if (e->sorted) {                    /* canonical form: insertion sort, n is
                                           small and this must be stable */
        uint32_t i, j;
        for (i = 1; i < len; i++) {
            JSPropertyEnum t = tab[i];
            for (j = i; j > 0 && venc_atom_less(e->ctx, t.atom, tab[j - 1].atom); j--)
                tab[j] = tab[j - 1];
            tab[j] = t;
        }
    }
    venc_len(e, 3, len);
    for (k = 0; k < len && rc == 0; k++) {
        /* Atom -> bytes in ONE step. Going through JS_AtomToString allocates a
           JSString whose only purpose is to be converted and freed, which is a
           heap round trip per key on the hot path. */
        size_t klen;
        const char *owned = NULL;
        const char *ks = JS_AtomBorrowASCII(e->ctx, &klen, tab[k].atom);
        JSValue val;
        if (!ks) {          /* not already UTF-8: convert, and pay for it */
            owned = JS_AtomToCStringLen(e->ctx, &klen, tab[k].atom);
            ks = owned;
        }
        if (!ks) { rc = -1; break; }
        venc_len(e, 0, klen);
        vb_write(&e->b, ks, klen);
        if (owned)
            JS_FreeCString(e->ctx, owned);
        val = JS_GetProperty(e->ctx, v, tab[k].atom);
        if (JS_IsException(val)) { rc = -1; break; }
        rc = venc_value(e, val);
        JS_FreeValue(e->ctx, val);
    }
    if (owns_tab == 1) {
        JS_FreePropertyEnum(e->ctx, tab, len);
    } else {
        for (k = 0; k < len; k++)
            JS_FreeAtom(e->ctx, tab[k].atom);
        if (owns_tab == 2)
            free(tab);
    }
    return rc;
}

static int venc_value(venc_t *e, JSValueConst v)
{
    if (e->depth >= VS_MAX_DEPTH) {
        JS_ThrowRangeError(e->ctx, "encode: nesting exceeds %d", VS_MAX_DEPTH);
        return -1;
    }
    if (JS_IsNull(v) || JS_IsUndefined(v)) {
        vb_put(&e->b, e->fmt == FMT_CBOR
               ? (uint8_t)(JS_IsNull(v) ? 0xF6 : 0xF7) : 0xC0);
        return 0;
    }
    if (JS_IsBool(v)) {
        int t = JS_ToBool(e->ctx, v);
        vb_put(&e->b, e->fmt == FMT_CBOR ? (uint8_t)(t ? 0xF5 : 0xF4)
                                         : (uint8_t)(t ? 0xC3 : 0xC2));
        return 0;
    }
    if (JS_IsNumber(v)) {
        double d;
        int64_t iv;
        if (JS_ToFloat64(e->ctx, &d, v) < 0)
            return -1;
        if (d == floor(d) && !isinf(d) && d >= -9.2233720368547758e18
            && d <= 9.2233720368547758e18 && !(d == 0 && signbit(d))) {
            iv = (int64_t)d;
            if ((double)iv == d) { venc_int(e, iv); return 0; }
        }
        {   /* the shortest form a double needs is the double itself */
            uint64_t bits;
            memcpy(&bits, &d, 8);
            vb_put(&e->b, e->fmt == FMT_CBOR ? 0xFB : 0xCB);
            vb_be(&e->b, bits, 8);
        }
        return 0;
    }
    if (JS_IsString(v))
        return venc_string(e, v);
    /* A function IS an object, so this check has to come first or it encodes
       as an empty map -- silently, which is the worst possible answer. */
    if (!JS_IsObject(v) || JS_IsFunction(e->ctx, v)) {
        JS_ThrowTypeError(e->ctx, "encode: a symbol or function has no encoding");
        return -1;
    }
    {
        const uint8_t *p;
        size_t n;
        JSValue hold;
        if (venc_bytes_of(e->ctx, v, &p, &n, &hold)) {
            venc_len(e, 1, n);
            vb_write(&e->b, p, n);
            JS_FreeValue(e->ctx, hold);
            return 0;
        }
    }
    if (venc_push(e, v) < 0)
        return -1;
    e->depth++;
    {
        int rc = JS_IsArray(e->ctx, v) == 1 ? venc_array(e, v) : venc_object(e, v);
        e->depth--;
        e->nseen--;
        return rc;
    }
}

/* --------------------------------------------------------------- decoding */

typedef struct {
    JSContext     *ctx;
    const uint8_t *p;
    size_t         n, i;
    int            fmt, depth;
} vdec_t;

static JSValue vdec_value(vdec_t *d);

static int vdec_need(vdec_t *d, size_t k)
{
    if (d->i + k > d->n) {
        JS_ThrowSyntaxError(d->ctx, "decode: truncated at byte %u",
                            (unsigned)d->i);
        return -1;
    }
    return 0;
}

static uint64_t vdec_be(vdec_t *d, int bytes)
{
    uint64_t v = 0;
    int k;
    for (k = 0; k < bytes; k++)
        v = (v << 8) | d->p[d->i + (size_t)k];
    d->i += (size_t)bytes;
    return v;
}

/* A declared length is checked against the bytes that REMAIN before anything
   is allocated: a four-byte count is otherwise a four-gigabyte allocation. */
static int vdec_bounded(vdec_t *d, uint64_t count, size_t per)
{
    if (count > VS_MAX_ITEMS || (per && count > (uint64_t)(d->n - d->i) / per)) {
        JS_ThrowRangeError(d->ctx, "decode: declared length %llu exceeds the "
                                   "remaining input", (unsigned long long)count);
        return -1;
    }
    return 0;
}

static JSValue vdec_str(vdec_t *d, uint64_t len)
{
    JSValue v;
    if (vdec_bounded(d, len, 1) < 0 || vdec_need(d, (size_t)len) < 0)
        return JS_EXCEPTION;
    v = JS_NewStringLen(d->ctx, (const char *)d->p + d->i, (size_t)len);
    d->i += (size_t)len;
    return v;
}

static JSValue vdec_bin(vdec_t *d, uint64_t len)
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
    ta[0] = ab; ta[1] = JS_UNDEFINED; ta[2] = JS_UNDEFINED;
    r = JS_NewTypedArray(d->ctx, 3, ta, JS_TYPED_ARRAY_UINT8);
    JS_FreeValue(d->ctx, ab);
    return r;
}

static JSValue vdec_array(vdec_t *d, uint64_t count)
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
                                            JS_PROP_C_W_E) < 0) {
            d->depth--;
            JS_FreeValue(d->ctx, out);
            return JS_EXCEPTION;
        }
    }
    d->depth--;
    return out;
}

/* A map key is a string in every document that is not deliberately odd, and
   JS_NewAtomLen interns straight from the input bytes -- where decoding it
   generically allocates a JSString only to hash it, intern it and free it. */
static int vdec_key(vdec_t *d, JSAtom *out)
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
        int bytes = b == 0xD9 ? 1 : b == 0xDA ? 2 : 4;
        d->i++;
        if (vdec_need(d, (size_t)bytes) < 0)
            return -1;
        len = vdec_be(d, bytes);
    } else {
        goto slow;
    }
    if (vdec_bounded(d, len, 1) < 0 || vdec_need(d, (size_t)len) < 0)
        return -1;
    *out = JS_NewAtomLen(d->ctx, (const char *)d->p + d->i, (size_t)len);
    d->i += (size_t)len;
    return *out == JS_ATOM_NULL ? -1 : 0;
slow:
    {   /* a non-string key still has to go the long way */
        JSValue kv = vdec_value(d);
        if (JS_IsException(kv))
            return -1;
        *out = JS_ValueToAtom(d->ctx, kv);
        JS_FreeValue(d->ctx, kv);
        return *out == JS_ATOM_NULL ? -1 : 0;
    }
}

static JSValue vdec_map(vdec_t *d, uint64_t count)
{
    JSValue out;
    uint64_t k;

    if (vdec_bounded(d, count, 2) < 0)
        return JS_EXCEPTION;
    out = JS_NewObject(d->ctx);
    if (JS_IsException(out))
        return out;
    d->depth++;
    for (k = 0; k < count; k++) {
        JSValue val;
        JSAtom a;
        if (vdec_key(d, &a) < 0) { d->depth--; JS_FreeValue(d->ctx, out); return JS_EXCEPTION; }
        val = vdec_value(d);
        /* DEFINE: a document whose key is __proto__ produces an own property
           and cannot retarget a prototype. */
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

static JSValue vdec_cbor(vdec_t *d)
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
    case 0: return JS_NewInt64(d->ctx, (int64_t)arg);
    case 1: return JS_NewInt64(d->ctx, -1 - (int64_t)arg);
    case 2: return vdec_bin(d, arg);
    case 3: return vdec_str(d, arg);
    case 4: return vdec_array(d, arg);
    case 5: return vdec_map(d, arg);
    case 6: return vdec_value(d);       /* a tag: the value is what matters */
    default: break;
    }
    switch (minor) {
    case 20: return JS_FALSE;
    case 21: return JS_TRUE;
    case 22: return JS_NULL;
    case 23: return JS_UNDEFINED;
    case 27: { double f; uint64_t bits = arg; memcpy(&f, &bits, 8);
               return JS_NewFloat64(d->ctx, f); }
    case 26: { float f; uint32_t bits = (uint32_t)arg; memcpy(&f, &bits, 4);
               return JS_NewFloat64(d->ctx, (double)f); }
    case 25: {  /* half precision, by the RFC's own reference decoding */
        uint32_t h = (uint32_t)arg, e = (h >> 10) & 0x1F, m = h & 0x3FF;
        double val;
        if (e == 0)       val = ldexp((double)m, -24);
        else if (e != 31) val = ldexp((double)(m + 1024), (int)e - 25);
        else              val = m ? NAN : INFINITY;
        return JS_NewFloat64(d->ctx, (h >> 15) ? -val : val);
    }
    default:
        return JS_ThrowSyntaxError(d->ctx, "decode: unsupported simple value %d",
                                   minor);
    }
}

/* Read the argument of a multi-byte header, or -1 having thrown. */
static int vdec_arg(vdec_t *d, int bytes, uint64_t *out)
{
    if (vdec_need(d, (size_t)bytes) < 0)
        return -1;
    *out = vdec_be(d, bytes);
    return 0;
}

/* The signed integer family: the width is in the type byte. */
static JSValue vdec_mp_int(vdec_t *d, uint8_t b)
{
    int nb = 1 << (b - 0xD0);
    uint64_t v;

    if (vdec_arg(d, nb, &v) < 0)
        return JS_EXCEPTION;
    switch (nb) {
    case 1: return JS_NewInt32(d->ctx, (int8_t)v);
    case 2: return JS_NewInt32(d->ctx, (int16_t)v);
    case 4: return JS_NewInt32(d->ctx, (int32_t)v);
    default: return JS_NewInt64(d->ctx, (int64_t)v);
    }
}

/* Everything whose length or value follows the type byte. */
static JSValue vdec_mp_wide(vdec_t *d, uint8_t b)
{
    uint64_t v;

    switch (b) {
    case 0xC4: case 0xC5: case 0xC6:
        return vdec_arg(d, b == 0xC4 ? 1 : b == 0xC5 ? 2 : 4, &v) < 0
               ? JS_EXCEPTION : vdec_bin(d, v);
    case 0xCA: {
        float f;
        uint32_t bits;
        if (vdec_arg(d, 4, &v) < 0) return JS_EXCEPTION;
        bits = (uint32_t)v;
        memcpy(&f, &bits, 4);
        return JS_NewFloat64(d->ctx, (double)f);
    }
    case 0xCB: {
        double f;
        if (vdec_arg(d, 8, &v) < 0) return JS_EXCEPTION;
        memcpy(&f, &v, 8);
        return JS_NewFloat64(d->ctx, f);
    }
    case 0xCC: case 0xCD: case 0xCE: case 0xCF:
        return vdec_arg(d, 1 << (b - 0xCC), &v) < 0
               ? JS_EXCEPTION : JS_NewInt64(d->ctx, (int64_t)v);
    case 0xD0: case 0xD1: case 0xD2: case 0xD3:
        return vdec_mp_int(d, b);
    case 0xD9: case 0xDA: case 0xDB:
        return vdec_arg(d, b == 0xD9 ? 1 : b == 0xDA ? 2 : 4, &v) < 0
               ? JS_EXCEPTION : vdec_str(d, v);
    case 0xDC: case 0xDD:
        return vdec_arg(d, b == 0xDC ? 2 : 4, &v) < 0
               ? JS_EXCEPTION : vdec_array(d, v);
    case 0xDE: case 0xDF:
        return vdec_arg(d, b == 0xDE ? 2 : 4, &v) < 0
               ? JS_EXCEPTION : vdec_map(d, v);
    default:
        return JS_ThrowSyntaxError(d->ctx, "decode: unsupported type byte 0x%02X "
                                           "at %u", b, (unsigned)(d->i - 1));
    }
}

static JSValue vdec_msgpack(vdec_t *d)
{
    uint8_t b;

    if (vdec_need(d, 1) < 0)
        return JS_EXCEPTION;
    b = d->p[d->i++];
    if (b < 0x80)           return JS_NewInt32(d->ctx, b);
    if (b >= 0xE0)          return JS_NewInt32(d->ctx, (int8_t)b);
    if ((b & 0xF0) == 0x80) return vdec_map(d, b & 0x0F);
    if ((b & 0xF0) == 0x90) return vdec_array(d, b & 0x0F);
    if ((b & 0xE0) == 0xA0) return vdec_str(d, b & 0x1F);
    if (b == 0xC0)          return JS_NULL;
    if (b == 0xC2)          return JS_FALSE;
    if (b == 0xC3)          return JS_TRUE;
    return vdec_mp_wide(d, b);
}

static JSValue vdec_value(vdec_t *d)
{
    if (d->depth >= VS_MAX_DEPTH)
        return JS_ThrowRangeError(d->ctx, "decode: nesting exceeds %d",
                                  VS_MAX_DEPTH);
    return d->fmt == FMT_CBOR ? vdec_cbor(d) : vdec_msgpack(d);
}

/* ------------------------------------------------------------ entry points */

static int vs_bytes_arg(JSContext *ctx, JSValueConst v, const uint8_t **pp,
                        size_t *pn, JSValue *hold)
{
    size_t off, len, bpe, ab;
    uint8_t *base;
    JSValue buf = JS_GetArrayBufferView(ctx, v, &off, &len, &bpe);

    if (JS_IsException(buf)) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        base = JS_GetArrayBuffer(ctx, &ab, v);
        if (!base)
            return -1;
        *hold = JS_UNDEFINED;
        *pp = base;
        *pn = ab;
        return 0;
    }
    if (bpe != 1) {
        JS_FreeValue(ctx, buf);
        JS_ThrowTypeError(ctx, "decode(bytes): expected a byte view");
        return -1;
    }
    base = JS_GetArrayBuffer(ctx, &ab, buf);
    if (!base || off > ab || len > ab - off) {
        JS_FreeValue(ctx, buf);
        return -1;
    }
    *hold = buf;
    *pp = base + off;
    *pn = len;
    return 0;
}

static JSValue vs_new_bytes(JSContext *ctx, const uint8_t *p, size_t n)
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

/* magic: bit 0 = CBOR, bit 1 = sorted keys (the canonical form) */
static JSValue vs_encode(JSContext *ctx, JSValueConst this_val,
                         int argc, JSValueConst *argv, int magic)
{
    venc_t e;
    JSValue out;

    (void)this_val;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "encode(value): a value is required");
    memset(&e, 0, sizeof e);
    e.ctx = ctx;
    e.fmt = (magic & 1) ? FMT_CBOR : FMT_MSGPACK;
    e.sorted = (magic & 2) != 0;
    vb_init(&e.b);
    if (venc_value(&e, argv[0]) < 0 || e.b.oom) {
        if (e.b.oom)
            JS_ThrowOutOfMemory(ctx);
        vb_free(&e.b);
        free(e.seen);
        return JS_EXCEPTION;
    }
    free(e.seen);
    out = vs_new_bytes(ctx, e.b.p, e.b.n);
    vb_free(&e.b);
    return out;
}

static JSValue vs_decode(JSContext *ctx, JSValueConst this_val,
                         int argc, JSValueConst *argv, int magic)
{
    vdec_t d;
    JSValue hold = JS_UNDEFINED, out;

    (void)this_val;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "decode(bytes): bytes are required");
    memset(&d, 0, sizeof d);
    d.ctx = ctx;
    d.fmt = magic ? FMT_CBOR : FMT_MSGPACK;
    if (vs_bytes_arg(ctx, argv[0], &d.p, &d.n, &hold) < 0)
        return JS_EXCEPTION;
    out = vdec_value(&d);
    if (!JS_IsException(out) && d.i != d.n) {
        JS_FreeValue(ctx, out);
        out = JS_ThrowSyntaxError(ctx, "decode: %u trailing bytes after the "
                                       "value", (unsigned)(d.n - d.i));
    }
    JS_FreeValue(ctx, hold);
    return out;
}

/* A canonical CBOR encoding through xxh64: object-hash's job, with a DEFINED
   canonical form rather than a house convention. */
static JSValue vs_value_hash(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv)
{
    venc_t e;
    char hex[17];
    uint64_t h;
    int k;

    (void)this_val;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "ValueHash(value): a value is required");
    memset(&e, 0, sizeof e);
    e.ctx = ctx;
    e.fmt = FMT_CBOR;
    e.sorted = 1;
    vb_init(&e.b);
    if (venc_value(&e, argv[0]) < 0 || e.b.oom) {
        if (e.b.oom)
            JS_ThrowOutOfMemory(ctx);
        vb_free(&e.b);
        free(e.seen);
        return JS_EXCEPTION;
    }
    free(e.seen);
    h = dyn_xxh64(e.b.p, e.b.n, 0);
    vb_free(&e.b);
    for (k = 15; k >= 0; k--) {
        hex[k] = "0123456789abcdef"[h & 15];
        h >>= 4;
    }
    hex[16] = 0;
    return JS_NewString(ctx, hex);
}

/* ------------------------------------------------------- structuredClone */

typedef struct {
    JSContext *ctx;
    JSValue   *src, *dst;               /* the memo, so CYCLES survive */
    int        n, cap, depth;
} vclone_t;

static JSValue vclone(vclone_t *c, JSValueConst v);

static int vclone_memo(vclone_t *c, JSValueConst s, JSValueConst d)
{
    if (c->n == c->cap) {
        int nc = c->cap ? c->cap * 2 : 16;
        JSValue *a = (JSValue *)realloc(c->src, (size_t)nc * sizeof *a);
        JSValue *b = (JSValue *)realloc(c->dst, (size_t)nc * sizeof *b);
        if (a) c->src = a;
        if (b) c->dst = b;
        if (!a || !b) { JS_ThrowOutOfMemory(c->ctx); return -1; }
        c->cap = nc;
    }
    c->src[c->n] = (JSValue)s;
    c->dst[c->n] = (JSValue)d;
    c->n++;
    return 0;
}

static int vclone_props(vclone_t *c, JSValueConst v, JSValue out)
{
    JSPropertyEnum *tab = NULL;
    uint32_t len = 0, k;
    int rc = 0;

    if (JS_GetOwnPropertyNames(c->ctx, &tab, &len, v,
                               JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) < 0)
        return -1;
    for (k = 0; k < len && rc == 0; k++) {
        JSValue val = JS_GetProperty(c->ctx, v, tab[k].atom), cl;
        if (JS_IsException(val)) { rc = -1; break; }
        cl = vclone(c, val);
        JS_FreeValue(c->ctx, val);
        if (JS_IsException(cl)
            || JS_DefinePropertyValue(c->ctx, out, tab[k].atom, cl,
                                      JS_PROP_C_W_E) < 0)
            rc = -1;
    }
    JS_FreePropertyEnum(c->ctx, tab, len);
    return rc;
}

static JSValue vclone(vclone_t *c, JSValueConst v)
{
    int i;
    JSValue out;

    if (!JS_IsObject(v))
        return JS_DupValue(c->ctx, v);
    for (i = 0; i < c->n; i++)
        if (JS_VALUE_GET_PTR(c->src[i]) == JS_VALUE_GET_PTR(v))
            return JS_DupValue(c->ctx, c->dst[i]);   /* the cycle closes here */
    if (c->depth >= VS_MAX_DEPTH)
        return JS_ThrowRangeError(c->ctx, "structuredClone: nesting exceeds %d",
                                  VS_MAX_DEPTH);
    if (JS_IsFunction(c->ctx, v))
        return JS_ThrowTypeError(c->ctx,
            "structuredClone: a function cannot be cloned");
    {
        const uint8_t *p;
        size_t n;
        JSValue hold;
        if (venc_bytes_of(c->ctx, v, &p, &n, &hold)) {
            out = vs_new_bytes(c->ctx, p, n);
            JS_FreeValue(c->ctx, hold);
            return out;
        }
    }
    out = JS_IsArray(c->ctx, v) == 1 ? JS_NewArray(c->ctx) : JS_NewObject(c->ctx);
    if (JS_IsException(out))
        return out;
    if (vclone_memo(c, v, out) < 0) {
        JS_FreeValue(c->ctx, out);
        return JS_EXCEPTION;
    }
    c->depth++;
    if (vclone_props(c, v, out) < 0) {
        c->depth--;
        JS_FreeValue(c->ctx, out);
        return JS_EXCEPTION;
    }
    c->depth--;
    return out;
}

static JSValue vs_structured_clone(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv)
{
    vclone_t c;
    JSValue out;

    (void)this_val;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "structuredClone(value): a value is required");
    memset(&c, 0, sizeof c);
    c.ctx = ctx;
    out = vclone(&c, argv[0]);
    free(c.src);
    free(c.dst);
    return out;
}

/* ------------------------------------------------------------ registration */

static const JSCFunctionListEntry dyn_vs_funcs[] = {
    JS_CFUNC_MAGIC_DEF("MsgPackEncode", 1, vs_encode, 0),
    JS_CFUNC_MAGIC_DEF("MsgPackDecode", 1, vs_decode, 0),
    JS_CFUNC_MAGIC_DEF("CBOREncode", 1, vs_encode, 1),
    JS_CFUNC_MAGIC_DEF("CBORDecode", 1, vs_decode, 1),
    JS_CFUNC_MAGIC_DEF("CBORCanonical", 1, vs_encode, 3),
    JS_CFUNC_DEF("ValueHash", 1, vs_value_hash),
    JS_CFUNC_DEF("structuredClone", 1, vs_structured_clone),
};

static int dyn_vs_init_module(JSContext *ctx, JSModuleDef *m)
{
    return JS_SetModuleExportList(ctx, m, dyn_vs_funcs, countof(dyn_vs_funcs));
}

int js_nat_init_vserialize(JSContext *ctx)
{
    JSModuleDef *m = JS_NewCModule(ctx, "dyna:serialize", dyn_vs_init_module);
    if (!m)
        return -1;
    return JS_AddModuleExportList(ctx, m, dyn_vs_funcs, countof(dyn_vs_funcs));
}

#endif /* CONFIG_NATIVE_MODULES && CONFIG_NATIVE_MODULE_VSERIALIZE */
