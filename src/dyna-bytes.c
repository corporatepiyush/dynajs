#include "dyna-nat.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_BYTES)

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "dyna-simd-kernels.h"

#ifndef countof
#define countof(x) (sizeof(x) / sizeof((x)[0]))
#endif

static JSClassID dyn_bh_class_id;
static JSValue dyn_bh_backing(JSValueConst v);
static void dyn_bh_dirty_handle(JSValueConst v);

static int dyn_bytes_view(JSContext* ctx, JSValueConst v, uint8_t** pp, size_t* pn)
{
    JSValue buf;
    uint8_t* base;
    size_t off, len, bpe, ab;

    {
        JSValue inner = dyn_bh_backing(v);
        if (!JS_IsUndefined(inner))
            v = inner;
    }

    buf = JS_GetBufferKind(v) == JS_BUFFER_KIND_VIEW ? JS_GetArrayBufferView(ctx, v, &off, &len, &bpe) : JS_EXCEPTION;
    if (!JS_IsException(buf)) {
        if (bpe != 1) {
            JS_FreeValue(ctx, buf);
            JS_ThrowTypeError(ctx, "expected a byte view (Uint8Array, Int8Array, "
                                   "Uint8ClampedArray, DataView) or an ArrayBuffer; "
                                   "use bytesOf() to reinterpret a wider view");
            return -1;
        }
        base = JS_GetArrayBuffer(ctx, &ab, buf);
        JS_FreeValue(ctx, buf);
        if (!base) {
            JS_ThrowTypeError(ctx, "typed array buffer is detached");
            return -1;
        }
        if (off > ab || len > ab - off) {
            JS_ThrowRangeError(ctx, "typed array out of bounds");
            return -1;
        }
        *pp = base + off;
        *pn = len;
        return 0;
    }
    JS_FreeValue(ctx, JS_GetException(ctx));

    base = JS_GetArrayBuffer(ctx, &ab, v);
    if (base) {
        *pp = base;
        *pn = ab;
        return 0;
    }
    return -1;
}

static JSValue dyn_bytes_new_u8array(JSContext* ctx, const uint8_t* data, size_t len)
{
    static const uint8_t zero_stub = 0;
    JSValue ab, out;
    JSValueConst ta_args[3];

    if (len == 0)
        data = &zero_stub;
    ab = JS_NewArrayBufferCopy(ctx, data, len);
    if (JS_IsException(ab))
        return ab;
    ta_args[0] = ab;
    ta_args[1] = JS_UNDEFINED;
    ta_args[2] = JS_UNDEFINED;
    out = JS_NewTypedArray(ctx, 3, ta_args, JS_TYPED_ARRAY_UINT8);
    JS_FreeValue(ctx, ab);
    return out;
}

static JSValue dyn_bytes_bytes_of(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    JSValue buf, off_v, len_v, out;
    JSValueConst ta_args[3];
    uint8_t* base;
    size_t off, len, bpe, ab;

    (void)this_val;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "bytesOf(view)");

    base = (JS_GetBufferKind(argv[0]) == JS_BUFFER_KIND_BUFFER ? JS_GetArrayBuffer(ctx, &ab, argv[0]) : NULL);
    if (base) {
        buf = JS_DupValue(ctx, argv[0]);
        off = 0;
        len = ab;
    } else {
        JS_FreeValue(ctx, JS_GetException(ctx));
        buf = JS_GetArrayBufferView(ctx, argv[0], &off, &len, &bpe);
        if (JS_IsException(buf))
            return JS_EXCEPTION;
    }
    off_v = JS_NewInt64(ctx, (int64_t)off);
    len_v = JS_NewInt64(ctx, (int64_t)len);
    ta_args[0] = buf;
    ta_args[1] = off_v;
    ta_args[2] = len_v;
    out = JS_NewTypedArray(ctx, 3, ta_args, JS_TYPED_ARRAY_UINT8);
    JS_FreeValue(ctx, buf);
    JS_FreeValue(ctx, off_v);
    JS_FreeValue(ctx, len_v);
    return out;
}

static JSValue dyn_bytes_compare(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    uint8_t *a, *b;
    size_t an, bn, minlen;
    int cmp;
    (void)this_val;
    (void)argc;

    if (dyn_bytes_view(ctx, argv[0], &a, &an))
        return JS_EXCEPTION;
    if (dyn_bytes_view(ctx, argv[1], &b, &bn))
        return JS_EXCEPTION;

    minlen = an < bn ? an : bn;
    cmp = minlen ? memcmp(a, b, minlen) : 0;
    if (cmp != 0)
        cmp = cmp < 0 ? -1 : 1;
    else
        cmp = (an > bn) - (an < bn);
    return JS_NewInt32(ctx, cmp);
}

static JSValue dyn_bytes_equal(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    uint8_t *a, *b;
    size_t an, bn;
    (void)this_val;
    (void)argc;

    if (dyn_bytes_view(ctx, argv[0], &a, &an))
        return JS_EXCEPTION;
    if (dyn_bytes_view(ctx, argv[1], &b, &bn))
        return JS_EXCEPTION;

    return JS_NewBool(ctx, an == bn && (an == 0 || memcmp(a, b, an) == 0));
}

static int dyn_bytes_needle(JSContext* ctx, JSValueConst v, uint8_t* byte_out,
    const uint8_t** pat, size_t* plen)
{
    if (JS_IsNumber(v)) {
        int32_t b;
        if (JS_ToInt32(ctx, &b, v))
            return -1;
        *byte_out = (uint8_t)b;
        *pat = byte_out;
        *plen = 1;
        return 0;
    }
    {
        uint8_t* p;
        size_t n;
        if (dyn_bytes_view(ctx, v, &p, &n))
            return -1;
        *pat = p;
        *plen = n;
        return 0;
    }
}

#define DYN_BYTES_IDX_UNSET ((size_t)-1)

static int dyn_bytes_from_index(JSContext* ctx, JSValueConst v, size_t* out)
{
    int64_t from;
    if (JS_IsUndefined(v)) {
        *out = DYN_BYTES_IDX_UNSET;
        return 0;
    }
    if (JS_ToInt64(ctx, &from, v))
        return -1;
    if (from < 0)
        from = 0;
    *out = (size_t)from;
    return 0;
}

static size_t dyn_bytes_last_find(const uint8_t* text, size_t tlen,
    const uint8_t* pat, size_t plen)
{
    size_t lps_small[256];
    size_t* lps = lps_small;
    size_t i = 0, j = 0, last = (size_t)-1;

    if (plen == 0 || tlen < plen)
        return (size_t)-1;
    if (plen > countof(lps_small)) {
        lps = (size_t*)dyn_nat_malloc(plen * sizeof(*lps));
        if (!lps) {
            size_t k = tlen - plen + 1;
            while (k > 0) {
                k--;
                if (memcmp(text + k, pat, plen) == 0)
                    return k;
            }
            return (size_t)-1;
        }
    }
    lps[0] = 0;
    for (i = 1; i < plen; i++) {
        size_t len = lps[i - 1];
        while (len && pat[i] != pat[len])
            len = lps[len - 1];
        if (pat[i] == pat[len])
            len++;
        lps[i] = len;
    }
    for (i = 0; i < tlen; i++) {
        while (j && text[i] != pat[j])
            j = lps[j - 1];
        if (text[i] == pat[j])
            j++;
        if (j == plen) {
            last = i + 1 - plen;
            j = lps[j - 1];
        }
    }
    if (lps != lps_small)
        dyn_nat_free(lps);
    return last;
}

static JSValue dyn_bytes_index_of(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    uint8_t needle_byte;
    const uint8_t* pat;
    size_t plen;
    uint8_t* buf;
    size_t n, pos, start = 0;
    (void)this_val;

    if (argc < 2)
        return JS_ThrowTypeError(ctx, "indexOf(buf, needle)");
    if (dyn_bytes_from_index(ctx, argv[2], &start))
        return JS_EXCEPTION;
    if (dyn_bytes_needle(ctx, argv[1], &needle_byte, &pat, &plen))
        return JS_EXCEPTION;
    if (dyn_bytes_view(ctx, argv[0], &buf, &n))
        return JS_EXCEPTION;
    if (start == DYN_BYTES_IDX_UNSET)
        start = 0;
    if (start > n)
        start = n;

    if (plen == 0)
        return JS_NewInt64(ctx, (int64_t)start);
    if (plen > n || start > n - plen)
        return JS_NewInt32(ctx, -1);
    pos = simd.strfind(buf + start, n - start, pat, plen);
    return pos == SIZE_MAX ? JS_NewInt32(ctx, -1)
                           : JS_NewInt64(ctx, (int64_t)(pos + start));
}

static JSValue dyn_bytes_last_index_of(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    uint8_t needle_byte;
    const uint8_t* pat;
    size_t plen;
    uint8_t* buf;
    size_t n, pos, start = 0, hi;
    (void)this_val;

    if (argc < 2)
        return JS_ThrowTypeError(ctx, "lastIndexOf(buf, needle)");
    if (dyn_bytes_from_index(ctx, argv[2], &start))
        return JS_EXCEPTION;
    if (dyn_bytes_needle(ctx, argv[1], &needle_byte, &pat, &plen))
        return JS_EXCEPTION;
    if (dyn_bytes_view(ctx, argv[0], &buf, &n))
        return JS_EXCEPTION;
    if (start == DYN_BYTES_IDX_UNSET || start > n)
        start = n;

    if (plen == 0)
        return JS_NewInt64(ctx, (int64_t)start);
    if (plen > n)
        return JS_NewInt32(ctx, -1);
    hi = n - plen;
    if (start < hi)
        hi = start;
    pos = dyn_bytes_last_find(buf, hi + plen, pat, plen);
    return pos == (size_t)-1 ? JS_NewInt32(ctx, -1) : JS_NewInt64(ctx, (int64_t)pos);
}

static JSValue dyn_bytes_contains(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    uint8_t needle_byte;
    const uint8_t* pat;
    size_t plen;
    uint8_t* buf;
    size_t n;
    (void)this_val;
    (void)argc;

    if (dyn_bytes_needle(ctx, argv[1], &needle_byte, &pat, &plen))
        return JS_EXCEPTION;
    if (dyn_bytes_view(ctx, argv[0], &buf, &n))
        return JS_EXCEPTION;

    if (plen == 0)
        return JS_NewBool(ctx, 1);
    if (plen > n)
        return JS_NewBool(ctx, 0);
    return JS_NewBool(ctx, simd.strfind(buf, n, pat, plen) != SIZE_MAX);
}

static JSValue dyn_bytes_count(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    uint8_t needle_byte;
    const uint8_t* pat;
    size_t plen;
    uint8_t* buf;
    size_t n, scan, cnt, start = 0;
    (void)this_val;

    if (argc < 2)
        return JS_ThrowTypeError(ctx, "count(buf, needle)");
    if (dyn_bytes_from_index(ctx, argv[2], &start))
        return JS_EXCEPTION;
    if (dyn_bytes_needle(ctx, argv[1], &needle_byte, &pat, &plen))
        return JS_EXCEPTION;
    if (dyn_bytes_view(ctx, argv[0], &buf, &n))
        return JS_EXCEPTION;
    if (start == DYN_BYTES_IDX_UNSET)
        start = 0;
    if (start > n)
        start = n;

    if (plen == 0)
        return JS_NewInt64(ctx, (int64_t)(n - start) + 1);
    if (plen > n || start > n - plen)
        return JS_NewInt32(ctx, 0);
    if (plen == 1)
        return JS_NewInt64(ctx, (int64_t)simd.count_u8(buf + start, pat[0], n - start));

    cnt = 0;
    scan = start;
    while (scan <= n - plen) {
        size_t pos = simd.strfind(buf + scan, n - scan, pat, plen);
        if (pos == SIZE_MAX)
            break;
        cnt++;
        scan += pos + plen;
    }
    return JS_NewInt64(ctx, (int64_t)cnt);
}

static JSValue dyn_bytes_starts_with(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    uint8_t needle_byte;
    const uint8_t* pat;
    size_t plen;
    uint8_t* buf;
    size_t n, start = 0;
    (void)this_val;

    if (argc < 2)
        return JS_ThrowTypeError(ctx, "startsWith(buf, needle)");
    if (dyn_bytes_from_index(ctx, argv[2], &start))
        return JS_EXCEPTION;
    if (dyn_bytes_needle(ctx, argv[1], &needle_byte, &pat, &plen))
        return JS_EXCEPTION;
    if (dyn_bytes_view(ctx, argv[0], &buf, &n))
        return JS_EXCEPTION;
    if (start == DYN_BYTES_IDX_UNSET)
        start = 0;
    if (start > n)
        start = n;

    if (plen > n - start)
        return JS_NewBool(ctx, 0);
    return JS_NewBool(ctx, plen == 0 || memcmp(buf + start, pat, plen) == 0);
}

static JSValue dyn_bytes_ends_with(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    uint8_t needle_byte;
    const uint8_t* pat;
    size_t plen;
    uint8_t* buf;
    size_t n, end;
    (void)this_val;

    if (argc < 2)
        return JS_ThrowTypeError(ctx, "endsWith(buf, needle)");
    if (dyn_bytes_from_index(ctx, argv[2], &end))
        return JS_EXCEPTION;
    if (dyn_bytes_needle(ctx, argv[1], &needle_byte, &pat, &plen))
        return JS_EXCEPTION;
    if (dyn_bytes_view(ctx, argv[0], &buf, &n))
        return JS_EXCEPTION;
    if (end == DYN_BYTES_IDX_UNSET)
        end = n;
    if (end > n)
        end = n;

    if (plen > end)
        return JS_NewBool(ctx, 0);
    return JS_NewBool(ctx, plen == 0 || memcmp(buf + end - plen, pat, plen) == 0);
}

static JSValue dyn_bytes_concat(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    JSValue len_val, elem, result = JS_EXCEPTION;
    uint32_t n, i;
    uint8_t* out;
    size_t total, used;
    (void)this_val;
    (void)argc;

    if (!JS_IsArray(ctx, argv[0]))
        return JS_ThrowTypeError(ctx, "concat(list) requires an array");
    len_val = JS_GetPropertyStr(ctx, argv[0], "length");
    if (JS_IsException(len_val))
        return JS_EXCEPTION;
    if (JS_ToUint32(ctx, &n, len_val)) {
        JS_FreeValue(ctx, len_val);
        return JS_EXCEPTION;
    }
    JS_FreeValue(ctx, len_val);

    total = 0;
    for (i = 0; i < n; i++) {
        uint8_t* p;
        size_t plen;
        elem = JS_GetPropertyUint32(ctx, argv[0], i);
        if (JS_IsException(elem))
            return JS_EXCEPTION;
        if (dyn_bytes_view(ctx, elem, &p, &plen)) {
            JS_FreeValue(ctx, elem);
            return JS_EXCEPTION;
        }
        JS_FreeValue(ctx, elem);
        total += plen;
    }

    out = (uint8_t*)dyn_nat_malloc(total ? total : 1);
    if (!out)
        return JS_ThrowOutOfMemory(ctx);

    used = 0;
    for (i = 0; i < n; i++) {
        uint8_t* p;
        size_t plen, remaining;
        elem = JS_GetPropertyUint32(ctx, argv[0], i);
        if (JS_IsException(elem))
            goto done;
        if (dyn_bytes_view(ctx, elem, &p, &plen)) {
            JS_FreeValue(ctx, elem);
            goto done;
        }
        JS_FreeValue(ctx, elem);
        remaining = total - used;
        if (plen > remaining)
            plen = remaining;
        if (plen)
            memcpy(out + used, p, plen);
        used += plen;
    }
    result = dyn_bytes_new_u8array(ctx, out, used);

done:
    dyn_nat_free(out);
    return result;
}

static JSValue dyn_bytes_copy(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    uint64_t dst_off = 0, src_off = 0, len = UINT64_MAX;
    uint8_t *dst, *src;
    size_t dn, sn;
    (void)this_val;
    (void)argc;

    if (!JS_IsUndefined(argv[2]) && JS_ToIndex(ctx, &dst_off, argv[2]))
        return JS_EXCEPTION;
    if (!JS_IsUndefined(argv[3]) && JS_ToIndex(ctx, &src_off, argv[3]))
        return JS_EXCEPTION;
    if (!JS_IsUndefined(argv[4]) && JS_ToIndex(ctx, &len, argv[4]))
        return JS_EXCEPTION;

    if (dyn_bytes_view(ctx, argv[0], &dst, &dn))
        return JS_EXCEPTION;
    if (dyn_bytes_view(ctx, argv[1], &src, &sn))
        return JS_EXCEPTION;

    if (dst_off > (uint64_t)dn)
        return JS_ThrowRangeError(ctx, "copy: dstOffset out of bounds");
    if (src_off > (uint64_t)sn)
        return JS_ThrowRangeError(ctx, "copy: srcOffset out of bounds");

    {
        uint64_t davail = (uint64_t)dn - dst_off, savail = (uint64_t)sn - src_off;
        uint64_t maxlen = davail < savail ? davail : savail;
        if (len == UINT64_MAX)
            len = maxlen;
        else if (len > maxlen)
            return JS_ThrowRangeError(ctx, "copy: length out of bounds");
    }

    if (len)
        memmove(dst + (size_t)dst_off, src + (size_t)src_off, (size_t)len);
    return JS_NewInt64(ctx, (int64_t)len);
}

static JSValue dyn_bytes_fill(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    int32_t val;
    uint64_t start = 0, end = UINT64_MAX;
    uint8_t* buf;
    size_t n;
    (void)this_val;
    (void)argc;

    if (JS_ToInt32(ctx, &val, argv[1]))
        return JS_EXCEPTION;
    if (!JS_IsUndefined(argv[2]) && JS_ToIndex(ctx, &start, argv[2]))
        return JS_EXCEPTION;
    if (!JS_IsUndefined(argv[3]) && JS_ToIndex(ctx, &end, argv[3]))
        return JS_EXCEPTION;

    if (dyn_bytes_view(ctx, argv[0], &buf, &n))
        return JS_EXCEPTION;

    if (end == UINT64_MAX)
        end = (uint64_t)n;
    if (start > (uint64_t)n || end > (uint64_t)n || start > end)
        return JS_ThrowRangeError(ctx, "fill: start/end out of bounds");

    memset(buf + start, (uint8_t)val, (size_t)(end - start));
    dyn_bh_dirty_handle(argv[0]);
    return JS_DupValue(ctx, argv[0]);
}

enum {
    DYN_U8,
    DYN_I8,
    DYN_U16LE,
    DYN_U16BE,
    DYN_I16LE,
    DYN_I16BE,
    DYN_U32LE,
    DYN_U32BE,
    DYN_I32LE,
    DYN_I32BE,
    DYN_U64LE,
    DYN_U64BE,
    DYN_I64LE,
    DYN_I64BE,
    DYN_F32LE,
    DYN_F32BE,
    DYN_F64LE,
    DYN_F64BE,
    DYN_FIELD_COUNT
};

typedef enum { DK_UINT,
    DK_INT,
    DK_FLOAT,
    DK_BIGUINT,
    DK_BIGINT } DynBytesKind;

typedef struct {
    uint8_t width;
    uint8_t be;
    uint8_t kind;
} DynBytesField;

static const DynBytesField dyn_bytes_fields[DYN_FIELD_COUNT] = {
    [DYN_U8] = { 1, 0, DK_UINT },
    [DYN_I8] = { 1, 0, DK_INT },
    [DYN_U16LE] = { 2, 0, DK_UINT },
    [DYN_U16BE] = { 2, 1, DK_UINT },
    [DYN_I16LE] = { 2, 0, DK_INT },
    [DYN_I16BE] = { 2, 1, DK_INT },
    [DYN_U32LE] = { 4, 0, DK_UINT },
    [DYN_U32BE] = { 4, 1, DK_UINT },
    [DYN_I32LE] = { 4, 0, DK_INT },
    [DYN_I32BE] = { 4, 1, DK_INT },
    [DYN_U64LE] = { 8, 0, DK_BIGUINT },
    [DYN_U64BE] = { 8, 1, DK_BIGUINT },
    [DYN_I64LE] = { 8, 0, DK_BIGINT },
    [DYN_I64BE] = { 8, 1, DK_BIGINT },
    [DYN_F32LE] = { 4, 0, DK_FLOAT },
    [DYN_F32BE] = { 4, 1, DK_FLOAT },
    [DYN_F64LE] = { 8, 0, DK_FLOAT },
    [DYN_F64BE] = { 8, 1, DK_FLOAT },
};

static uint64_t dyn_bytes_load(const uint8_t* p, int width, int be)
{
    uint64_t v = 0;
    int i;
    if (be) {
        for (i = 0; i < width; i++)
            v = (v << 8) | p[i];
    } else {
        for (i = width - 1; i >= 0; i--)
            v = (v << 8) | p[i];
    }
    return v;
}

static void dyn_bytes_store(uint8_t* p, uint64_t v, int width, int be)
{
    int i;
    if (be) {
        for (i = width - 1; i >= 0; i--) {
            p[i] = (uint8_t)v;
            v >>= 8;
        }
    } else {
        for (i = 0; i < width; i++) {
            p[i] = (uint8_t)v;
            v >>= 8;
        }
    }
}

static JSValue dyn_bytes_read(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    const DynBytesField* f = &dyn_bytes_fields[magic];
    uint64_t off;
    uint8_t* buf;
    size_t n;
    uint64_t v;
    (void)this_val;
    (void)argc;

    if (JS_ToIndex(ctx, &off, argv[1]))
        return JS_EXCEPTION;
    if (dyn_bytes_view(ctx, argv[0], &buf, &n))
        return JS_EXCEPTION;
    if (off + f->width > (uint64_t)n)
        return JS_ThrowRangeError(ctx, "read: offset out of bounds");

    v = dyn_bytes_load(buf + off, f->width, f->be);
    switch (f->kind) {
    case DK_UINT:
        if (f->width == 4)
            return JS_NewUint32(ctx, (uint32_t)v);
        return JS_NewInt32(ctx, (int32_t)v);
    case DK_INT:
        if (f->width == 1)
            return JS_NewInt32(ctx, (int8_t)v);
        if (f->width == 2)
            return JS_NewInt32(ctx, (int16_t)v);
        return JS_NewInt32(ctx, (int32_t)v);
    case DK_BIGUINT:
        return JS_NewBigUint64(ctx, v);
    case DK_BIGINT:
        return JS_NewBigInt64(ctx, (int64_t)v);
    case DK_FLOAT:
    default:
        if (f->width == 4) {
            union {
                uint32_t i;
                float f;
            } u;
            u.i = (uint32_t)v;
            return JS_NewFloat64(ctx, (double)u.f);
        } else {
            union {
                uint64_t i;
                double f;
            } u;
            u.i = v;
            return JS_NewFloat64(ctx, u.f);
        }
    }
}

static JSValue dyn_bytes_write(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    const DynBytesField* f = &dyn_bytes_fields[magic];
    uint64_t off;
    uint32_t u32 = 0;
    int64_t i64 = 0;
    double d = 0;
    uint64_t v = 0;
    uint8_t* buf;
    size_t n;
    (void)this_val;
    (void)argc;

    if (JS_ToIndex(ctx, &off, argv[1]))
        return JS_EXCEPTION;

    switch (f->kind) {
    case DK_UINT:
    case DK_INT:
        if (JS_ToUint32(ctx, &u32, argv[2]))
            return JS_EXCEPTION;
        v = u32;
        break;
    case DK_BIGUINT:
    case DK_BIGINT:
        if (JS_ToBigInt64(ctx, &i64, argv[2]))
            return JS_EXCEPTION;
        v = (uint64_t)i64;
        break;
    case DK_FLOAT:
    default:
        if (JS_ToFloat64(ctx, &d, argv[2]))
            return JS_EXCEPTION;
        if (f->width == 4) {
            union {
                uint32_t i;
                float f;
            } u;
            u.f = (float)d;
            v = u.i;
        } else {
            union {
                uint64_t i;
                double f;
            } u;
            u.f = d;
            v = u.i;
        }
        break;
    }

    if (dyn_bytes_view(ctx, argv[0], &buf, &n))
        return JS_EXCEPTION;
    if (off + f->width > (uint64_t)n)
        return JS_ThrowRangeError(ctx, "write: offset out of bounds");

    dyn_bytes_store(buf + off, v, f->width, f->be);
    dyn_bh_dirty_handle(argv[0]);
    return JS_NewInt64(ctx, (int64_t)(off + f->width));
}

#define DYN_UTF8_LOSSY_HAS_SIMD
#include "dyna-utf8-lossy.inc.c"

static JSValue dyn_bytes_to_utf8(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    uint8_t* buf;
    size_t n;
    (void)this_val;
    (void)argc;

    if (dyn_bytes_view(ctx, argv[0], &buf, &n))
        return JS_EXCEPTION;
    return dyn_utf8_lossy_string(ctx, buf, n);
}

static JSValue dyn_bytes_from_utf8(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    const char* str;
    size_t len;
    JSValue result;
    (void)this_val;
    (void)argc;

    str = JS_ToCStringLen(ctx, &len, argv[0]);
    if (!str)
        return JS_EXCEPTION;
    result = dyn_bytes_new_u8array(ctx, (const uint8_t*)str, len);
    JS_FreeCString(ctx, str);
    return result;
}

#define DYN_BH_UNKNOWN (-1)

typedef struct {
    JSValue u8;
    int is_ascii;
    int is_valid_utf8;
} dyn_bh_t;

static JSValue dyn_bh_backing(JSValueConst v)
{
    dyn_bh_t* b = (dyn_bh_t*)JS_GetOpaque(v, dyn_bh_class_id);
    return b ? b->u8 : JS_UNDEFINED;
}

static void dyn_bh_finalizer(JSRuntime* rt, JSValue val)
{
    dyn_bh_t* b = (dyn_bh_t*)JS_GetOpaque(val, dyn_bh_class_id);
    if (!b)
        return;
    JS_FreeValueRT(rt, b->u8);
    dyn_nat_free(b);
}

static void dyn_bh_mark(JSRuntime* rt, JSValueConst val, JS_MarkFunc* mark_func)
{
    dyn_bh_t* b = (dyn_bh_t*)JS_GetOpaque(val, dyn_bh_class_id);
    if (b)
        JS_MarkValue(rt, b->u8, mark_func);
}

static const JSClassDef dyn_bh_class = {
    "Bytes",
    .finalizer = dyn_bh_finalizer,
    .gc_mark = dyn_bh_mark,
};

static void dyn_bh_scan(const uint8_t* p, size_t n, int* ascii, int* utf8)
{
    size_t i = 0;

    while (i + 8 <= n) {
        uint64_t word;
        memcpy(&word, p + i, 8);
        if (word & 0x8080808080808080ULL)
            break;
        i += 8;
    }
    while (i < n && p[i] < 0x80)
        i++;
    if (i == n) {
        *ascii = 1;
        *utf8 = 1;
        return;
    }

    *ascii = 0;
    *utf8 = (simd.validate_utf8(p + i, n - i) == n - i);
}

static JSValue dyn_bh_wrap_flags(JSContext* ctx, JSValue u8, int inherit_ascii,
    JSValueConst new_target);

static JSValue dyn_bh_wrap(JSContext* ctx, JSValue u8)
{
    return dyn_bh_wrap_flags(ctx, u8, 0, JS_UNDEFINED);
}

static JSValue dyn_bh_wrap_flags(JSContext* ctx, JSValue u8, int inherit_ascii,
    JSValueConst new_target)
{
    dyn_bh_t* b;
    uint8_t* p = NULL;
    size_t n = 0;
    JSValue obj, proto;

    if (JS_IsException(u8))
        return u8;
    proto = dyn_ctor_proto(ctx, new_target, dyn_bh_class_id);
    if (JS_IsException(proto)) {
        JS_FreeValue(ctx, u8);
        return proto;
    }
    if (dyn_bytes_view(ctx, u8, &p, &n) < 0) {
        JS_FreeValue(ctx, proto);
        JS_FreeValue(ctx, u8);
        return JS_EXCEPTION;
    }
    b = (dyn_bh_t*)dyn_nat_calloc(1, sizeof(*b));
    if (!b) {
        JS_FreeValue(ctx, proto);
        JS_FreeValue(ctx, u8);
        return JS_ThrowOutOfMemory(ctx);
    }
    if (inherit_ascii) {
        b->is_ascii = 1;
        b->is_valid_utf8 = 1;
    } else {
        dyn_bh_scan(p, n, &b->is_ascii, &b->is_valid_utf8);
    }
    b->u8 = u8;
    obj = JS_NewObjectProtoClass(ctx, proto, dyn_bh_class_id);
    JS_FreeValue(ctx, proto);
    if (JS_IsException(obj)) {
        JS_FreeValue(ctx, u8);
        dyn_nat_free(b);
        return obj;
    }
    JS_SetOpaque(obj, b);
    return obj;
}

static dyn_bh_t* dyn_bh_of(JSContext* ctx, JSValueConst v)
{
    return (dyn_bh_t*)JS_GetOpaque2(ctx, v, dyn_bh_class_id);
}

static void dyn_bh_dirty(dyn_bh_t* b)
{
    b->is_ascii = DYN_BH_UNKNOWN;
    b->is_valid_utf8 = DYN_BH_UNKNOWN;
}

static int dyn_bh_resolve(JSContext* ctx, dyn_bh_t* b)
{
    uint8_t* p;
    size_t n;
    if (b->is_ascii != DYN_BH_UNKNOWN && b->is_valid_utf8 != DYN_BH_UNKNOWN)
        return 0;
    if (dyn_bytes_view(ctx, b->u8, &p, &n) < 0)
        return -1;
    dyn_bh_scan(p, n, &b->is_ascii, &b->is_valid_utf8);
    return 0;
}

static void dyn_bh_dirty_handle(JSValueConst v)
{
    dyn_bh_t* b = (dyn_bh_t*)JS_GetOpaque(v, dyn_bh_class_id);
    if (b)
        dyn_bh_dirty(b);
}

static JSValue dyn_bh_ctor(JSContext* ctx, JSValueConst new_target, int argc,
    JSValueConst* argv)
{
    uint8_t* p = NULL;
    size_t n = 0;

    if (argc < 1)
        return JS_ThrowTypeError(ctx, "new Bytes(data) requires an argument");

    if (JS_IsString(argv[0])) {
        size_t sl;
        const char* cs = JS_ToCStringLen(ctx, &sl, argv[0]);
        JSValue u8;
        if (!cs)
            return JS_EXCEPTION;
        u8 = dyn_bytes_new_u8array(ctx, (const uint8_t*)cs, sl);
        JS_FreeCString(ctx, cs);
        return dyn_bh_wrap_flags(ctx, u8, 0, new_target);
    }
    if (dyn_bytes_view(ctx, argv[0], &p, &n) < 0)
        return JS_EXCEPTION;
    return dyn_bh_wrap_flags(ctx, dyn_bytes_new_u8array(ctx, p, n), 0,
        new_target);
}

static JSValue dyn_bh_static(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv, int magic)
{
    (void)this_val;
    if (magic == 0) {
        uint64_t n = 0;
        if (argc < 1 || JS_ToIndex(ctx, &n, argv[0]))
            return JS_ThrowTypeError(ctx, "Bytes.alloc(n) requires a length");
        if (n > (uint64_t)INT32_MAX)
            return JS_ThrowRangeError(ctx, "Bytes.alloc: length too large");
        return dyn_bh_wrap(ctx, dyn_bytes_new_u8array(ctx, NULL, (size_t)n));
    }
    if (magic == 1) {
        return JS_NewBool(ctx, argc >= 1 && JS_GetOpaque(argv[0], dyn_bh_class_id) != NULL);
    }
    {
        int64_t alen = 0, i, total;
        size_t total_bytes = 0, o = 0;
        uint8_t* out;
        JSValue res;
        JSValueConst list = argv[0];
        int is_list;

        if (argc < 1)
            return JS_ThrowTypeError(ctx, "Bytes.concat(list) requires an array");
        is_list = JS_IsArray(ctx, argv[0]);
        if (is_list) {
            JSValue lv = JS_GetPropertyStr(ctx, argv[0], "length");
            if (JS_IsException(lv) || JS_ToInt64(ctx, &alen, lv)) {
                JS_FreeValue(ctx, lv);
                return JS_EXCEPTION;
            }
            JS_FreeValue(ctx, lv);
            if (alen < 0 || alen > (1LL << 20))
                return JS_ThrowRangeError(ctx,
                    "Bytes.concat: the list length must be in [0, 1048576]");
        }
        total = is_list ? alen + (argc - 1) : argc;

#define DYN_CONCAT_ELEM(idx)                                         \
    ((idx) < alen ? JS_GetPropertyUint32(ctx, list, (uint32_t)(idx)) \
                  : JS_DupValue(ctx, argv[(idx) - alen + first_extra]))
        {
            int first_extra = is_list ? 1 : 0;

            for (i = 0; i < total; i++) {
                JSValue e = DYN_CONCAT_ELEM(i);
                uint8_t* p;
                size_t n;
                if (JS_IsException(e))
                    return JS_EXCEPTION;
                if (dyn_bytes_view(ctx, e, &p, &n) < 0) {
                    JS_FreeValue(ctx, e);
                    return JS_EXCEPTION;
                }
                JS_FreeValue(ctx, e);
                total_bytes += n;
            }
            out = (uint8_t*)dyn_nat_malloc(total_bytes ? total_bytes : 1);
            if (!out)
                return JS_ThrowOutOfMemory(ctx);
            for (i = 0; i < total; i++) {
                JSValue e = DYN_CONCAT_ELEM(i);
                uint8_t* p;
                size_t n;
                if (JS_IsException(e) || dyn_bytes_view(ctx, e, &p, &n) < 0) {
                    JS_FreeValue(ctx, e);
                    dyn_nat_free(out);
                    return JS_EXCEPTION;
                }
                if (o + n > total_bytes) {
                    JS_FreeValue(ctx, e);
                    dyn_nat_free(out);
                    return JS_ThrowTypeError(ctx, "Bytes.concat: the list changed during concatenation");
                }
                memcpy(out + o, p, n);
                o += n;
                JS_FreeValue(ctx, e);
            }
#undef DYN_CONCAT_ELEM
        }
        res = dyn_bh_wrap(ctx, dyn_bytes_new_u8array(ctx, out, o));
        dyn_nat_free(out);
        return res;
    }
}

static JSValue dyn_bh_get(JSContext* ctx, JSValueConst this_val, int magic)
{
    dyn_bh_t* b = dyn_bh_of(ctx, this_val);
    uint8_t* p;
    size_t n;
    if (!b)
        return JS_EXCEPTION;
    switch (magic) {
    case 0:
        if (dyn_bytes_view(ctx, b->u8, &p, &n) < 0)
            return JS_EXCEPTION;
        return JS_NewInt64(ctx, (int64_t)n);
    case 1:
        if (dyn_bh_resolve(ctx, b) < 0)
            return JS_EXCEPTION;
        return JS_NewBool(ctx, b->is_ascii);
    case 2:
        if (dyn_bh_resolve(ctx, b) < 0)
            return JS_EXCEPTION;
        return JS_NewBool(ctx, b->is_valid_utf8);
    default:
        dyn_bh_dirty(b);
        return JS_DupValue(ctx, b->u8);
    }
}

static JSValue dyn_bh_slice(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv)
{
    dyn_bh_t* b = dyn_bh_of(ctx, this_val);
    uint8_t* p;
    size_t n;
    int64_t start = 0, end;
    JSValue sub, ret;

    if (!b)
        return JS_EXCEPTION;
    if (dyn_bytes_view(ctx, b->u8, &p, &n) < 0)
        return JS_EXCEPTION;
    end = (int64_t)n;
    if (argc >= 1 && !JS_IsUndefined(argv[0]) && JS_ToInt64(ctx, &start, argv[0]))
        return JS_EXCEPTION;
    if (argc >= 2 && !JS_IsUndefined(argv[1]) && JS_ToInt64(ctx, &end, argv[1]))
        return JS_EXCEPTION;
    if (start < 0)
        start += (int64_t)n;
    if (end < 0)
        end += (int64_t)n;
    if (start < 0)
        start = 0;
    if (start > (int64_t)n)
        start = (int64_t)n;
    if (end > (int64_t)n)
        end = (int64_t)n;
    if (end < start)
        end = start;

    {
        JSValue ab;
        size_t off = 0, blen = 0, bpe = 0;
        JSValueConst a[3];
        JSValue av[3];

        ab = JS_GetTypedArrayBuffer(ctx, b->u8, &off, &blen, &bpe);
        if (JS_IsException(ab))
            return ab;
        av[0] = ab;
        av[1] = JS_NewInt64(ctx, (int64_t)off + start);
        av[2] = JS_NewInt64(ctx, end - start);
        a[0] = av[0];
        a[1] = av[1];
        a[2] = av[2];
        sub = JS_NewTypedArray(ctx, 3, a, JS_TYPED_ARRAY_UINT8);
        JS_FreeValue(ctx, av[1]);
        JS_FreeValue(ctx, av[2]);
        JS_FreeValue(ctx, ab);
        if (JS_IsException(sub))
            return sub;
    }
    if (dyn_bh_resolve(ctx, b) < 0) {
        JS_FreeValue(ctx, sub);
        return JS_EXCEPTION;
    }
    ret = dyn_bh_wrap_flags(ctx, sub, b->is_ascii, JS_UNDEFINED);
    return ret;
}

#define DYN_BH_FORWARD(name, fn, maxargs)                                \
    static JSValue name(JSContext* ctx, JSValueConst this_val, int argc, \
        JSValueConst* argv)                                              \
    {                                                                    \
        dyn_bh_t* b = dyn_bh_of(ctx, this_val);                          \
        JSValueConst a[(maxargs) + 1];                                   \
        int i;                                                           \
        if (!b)                                                          \
            return JS_EXCEPTION;                                         \
        a[0] = b->u8;                                                    \
        for (i = 0; i < (maxargs); i++)                                  \
            a[i + 1] = (i < argc) ? argv[i] : JS_UNDEFINED;              \
        return fn(ctx, JS_UNDEFINED, (maxargs) + 1, a);                  \
    }

DYN_BH_FORWARD(dyn_bh_compare, dyn_bytes_compare, 1)
DYN_BH_FORWARD(dyn_bh_equals, dyn_bytes_equal, 1)
DYN_BH_FORWARD(dyn_bh_index_of, dyn_bytes_index_of, 2)
DYN_BH_FORWARD(dyn_bh_last_index_of, dyn_bytes_last_index_of, 2)
DYN_BH_FORWARD(dyn_bh_includes, dyn_bytes_contains, 1)
DYN_BH_FORWARD(dyn_bh_count, dyn_bytes_count, 2)
DYN_BH_FORWARD(dyn_bh_starts_with, dyn_bytes_starts_with, 2)
DYN_BH_FORWARD(dyn_bh_ends_with, dyn_bytes_ends_with, 2)
static JSValue dyn_bh_fill(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv)
{
    dyn_bh_t* b = dyn_bh_of(ctx, this_val);
    JSValueConst a[4];
    JSValue r;
    int i;
    if (!b)
        return JS_EXCEPTION;
    a[0] = b->u8;
    for (i = 0; i < 3; i++)
        a[i + 1] = (i < argc) ? argv[i] : JS_UNDEFINED;
    r = dyn_bytes_fill(ctx, JS_UNDEFINED, 4, a);
    if (JS_IsException(r))
        return r;
    JS_FreeValue(ctx, r);
    dyn_bh_dirty(b);
    return JS_DupValue(ctx, this_val);
}
DYN_BH_FORWARD(dyn_bh_to_utf8, dyn_bytes_to_utf8, 0)
static JSValue dyn_text_index_of_any(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);
DYN_BH_FORWARD(dyn_bh_index_of_any, dyn_text_index_of_any, 2)

static JSValue dyn_bh_read_bytes(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_bh_t* b = dyn_bh_of(ctx, this_val);
    uint64_t off = 0, len = 0;
    uint8_t* p;
    size_t n;
    (void)this_val;

    if (!b)
        return JS_EXCEPTION;
    if (argc < 2)
        return JS_ThrowTypeError(ctx, "readBytes(off, len) requires two offsets");
    if (JS_ToIndex(ctx, &off, argv[0]) || JS_ToIndex(ctx, &len, argv[1]))
        return JS_EXCEPTION;
    if (dyn_bytes_view(ctx, b->u8, &p, &n) < 0)
        return JS_EXCEPTION;
    if (off > (uint64_t)n || len > (uint64_t)n - off)
        return JS_ThrowRangeError(ctx, "readBytes: offset/length out of bounds");
    return dyn_bytes_new_u8array(ctx, p + off, (size_t)len);
}

static JSValue dyn_bh_write_bytes(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_bh_t* b = dyn_bh_of(ctx, this_val);
    uint64_t off = 0;
    uint8_t *p, *src;
    size_t n, sn;
    (void)this_val;

    if (!b)
        return JS_EXCEPTION;
    if (argc < 2)
        return JS_ThrowTypeError(ctx, "writeBytes(off, src) requires an offset and a view");
    if (JS_ToIndex(ctx, &off, argv[0]))
        return JS_EXCEPTION;
    if (dyn_bytes_view(ctx, argv[1], &src, &sn))
        return JS_EXCEPTION;
    if (dyn_bytes_view(ctx, b->u8, &p, &n) < 0)
        return JS_EXCEPTION;
    if (off > (uint64_t)n || sn > (uint64_t)n - off)
        return JS_ThrowRangeError(ctx, "writeBytes: offset/length out of bounds");
    if (sn)
        memmove(p + off, src, sn);
    dyn_bh_dirty(b);
    return JS_NewInt64(ctx, (int64_t)sn);
}

static JSValue dyn_bh_read(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv, int magic)
{
    dyn_bh_t* b = dyn_bh_of(ctx, this_val);
    JSValueConst a[2];
    if (!b)
        return JS_EXCEPTION;
    a[0] = b->u8;
    a[1] = (argc > 0) ? argv[0] : JS_UNDEFINED;
    return dyn_bytes_read(ctx, JS_UNDEFINED, 2, a, magic);
}

static JSValue dyn_bh_write(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv, int magic)
{
    dyn_bh_t* b = dyn_bh_of(ctx, this_val);
    JSValueConst a[3];
    JSValue r;
    if (!b)
        return JS_EXCEPTION;
    a[0] = b->u8;
    a[1] = (argc > 0) ? argv[0] : JS_UNDEFINED;
    a[2] = (argc > 1) ? argv[1] : JS_UNDEFINED;
    r = dyn_bytes_write(ctx, JS_UNDEFINED, 3, a, magic);
    if (JS_IsException(r))
        return r;
    dyn_bh_dirty(b);
    return r;
}

static JSValue dyn_bh_to_string(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv)
{
    (void)argc;
    (void)argv;
    return dyn_bh_to_utf8(ctx, this_val, 0, NULL);
}

static int dyn_text_bytes(JSContext* ctx, JSValueConst v, const uint8_t** data,
    size_t* len, const char** owned)
{
    *owned = NULL;
    if (JS_IsString(v)) {
        const char* s = JS_ToCStringLen(ctx, len, v);
        if (!s)
            return -1;
        *owned = s;
        *data = (const uint8_t*)s;
        return 0;
    }
    {
        size_t off = 0, blen = 0, bpe = 0;
        JSValue buf = JS_GetBufferKind(v) == JS_BUFFER_KIND_VIEW ? JS_GetArrayBufferView(ctx, v, &off, &blen, &bpe) : JS_EXCEPTION;
        if (!JS_IsException(buf)) {
            size_t absize = 0;
            uint8_t* ab;
            if (bpe != 1) {
                JS_FreeValue(ctx, buf);
                JS_ThrowTypeError(ctx, "expected a byte view (Uint8Array, Int8Array, "
                                       "Uint8ClampedArray, DataView) or an ArrayBuffer; "
                                       "use bytesOf() to reinterpret a wider view");
                return -1;
            }
            ab = JS_GetArrayBuffer(ctx, &absize, buf);
            JS_FreeValue(ctx, buf);
            if (!ab)
                return -1;
            *data = ab + off;
            *len = blen;
            return 0;
        }
        JS_FreeValue(ctx, JS_GetException(ctx));
    }
    {
        uint8_t* ab = JS_GetArrayBuffer(ctx, len, v);
        if (ab) {
            *data = ab;
            return 0;
        }
        if (JS_IsObject(v))
            return -1;
        JS_FreeValue(ctx, JS_GetException(ctx));
    }
    {
        const char* s = JS_ToCStringLen(ctx, len, v);
        if (!s)
            return -1;
        *owned = s;
        *data = (const uint8_t*)s;
        return 0;
    }
}

static JSValue dyn_text_index_of_any(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    const uint8_t *data, *set;
    size_t len, setlen, pos, start = 0;
    const char *owned_data, *owned_set;
    (void)this_val;

    if (argc < 2)
        return JS_ThrowTypeError(ctx, "indexOfAny(text, chars)");
    if (dyn_text_bytes(ctx, argv[1], &set, &setlen, &owned_set))
        return JS_EXCEPTION;
    if (dyn_bytes_from_index(ctx, argc > 2 ? argv[2] : JS_UNDEFINED, &start)) {
        if (owned_set)
            JS_FreeCString(ctx, owned_set);
        return JS_EXCEPTION;
    }
    if (dyn_text_bytes(ctx, argv[0], &data, &len, &owned_data)) {
        if (owned_set)
            JS_FreeCString(ctx, owned_set);
        return JS_EXCEPTION;
    }
    if (start == DYN_BYTES_IDX_UNSET)
        start = 0;
    if (start > len)
        start = len;
    pos = simd.find_first_of(data + start, len - start, set, setlen);
    if (owned_data)
        JS_FreeCString(ctx, owned_data);
    if (owned_set)
        JS_FreeCString(ctx, owned_set);
    if (pos == SIZE_MAX)
        return JS_NewInt32(ctx, -1);
    return JS_NewInt64(ctx, (int64_t)(pos + start));
}

static JSValue dyn_text_is_valid_utf8(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    const uint8_t* data;
    size_t len;
    const char* owned;
    int ok;
    (void)this_val;

    if (argc < 1)
        return JS_ThrowTypeError(ctx, "isValidUtf8(data)");
    if (dyn_text_bytes(ctx, argv[0], &data, &len, &owned))
        return JS_EXCEPTION;
    ok = (simd.validate_utf8(data, len) == len);
    if (owned)
        JS_FreeCString(ctx, owned);
    return JS_NewBool(ctx, ok);
}

static JSValue dyn_text_latin1_to_utf8(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    const uint8_t* data;
    size_t len, out_len;
    const char* owned;
    uint8_t* out;
    JSValue result;
    (void)this_val;

    if (argc < 1)
        return JS_ThrowTypeError(ctx, "latin1ToUtf8(bytes)");
    if (dyn_text_bytes(ctx, argv[0], &data, &len, &owned))
        return JS_EXCEPTION;
    out = (uint8_t*)dyn_nat_malloc(len ? len * 2 : 1);
    if (!out) {
        if (owned)
            JS_FreeCString(ctx, owned);
        return JS_ThrowOutOfMemory(ctx);
    }
    out_len = simd.latin1_to_utf8(data, len, out);
    if (owned)
        JS_FreeCString(ctx, owned);
    result = dyn_bytes_new_u8array(ctx, out, out_len);
    dyn_nat_free(out);
    return result;
}

static JSValue dyn_text_utf8_to_latin1(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    const uint8_t* data;
    size_t len, out_len = 0;
    const char* owned;
    uint8_t* out;
    JSValue result;
    int rc, bad_utf8 = 0;
    (void)this_val;

    if (argc < 1)
        return JS_ThrowTypeError(ctx, "utf8ToLatin1(bytes)");
    if (dyn_text_bytes(ctx, argv[0], &data, &len, &owned))
        return JS_EXCEPTION;
    out = (uint8_t*)dyn_nat_malloc(len ? len : 1);
    if (!out) {
        if (owned)
            JS_FreeCString(ctx, owned);
        return JS_ThrowOutOfMemory(ctx);
    }
    rc = simd.utf8_to_latin1(data, len, out, &out_len);
    if (rc != 0)
        bad_utf8 = (simd.validate_utf8(data, len) != len);
    if (owned)
        JS_FreeCString(ctx, owned);
    if (rc != 0) {
        dyn_nat_free(out);
        if (bad_utf8)
            return JS_ThrowRangeError(ctx, "utf8ToLatin1: invalid UTF-8");
        return JS_ThrowRangeError(
            ctx, "utf8ToLatin1: code point > 0xFF is not representable in latin1");
    }
    result = dyn_bytes_new_u8array(ctx, out, out_len);
    dyn_nat_free(out);
    return result;
}

static JSValue dyn_text_count_utf8(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    const uint8_t* data;
    size_t len;
    const char* owned;
    (void)this_val;

    if (argc < 1)
        return JS_ThrowTypeError(ctx, "countUtf8(data)");
    if (dyn_text_bytes(ctx, argv[0], &data, &len, &owned))
        return JS_EXCEPTION;
    {
        size_t c = simd.count_utf8(data, len);
        if (owned)
            JS_FreeCString(ctx, owned);
        return JS_NewInt64(ctx, (int64_t)c);
    }
}

static uint16_t* dyn_text_u16_copy(JSContext* ctx, JSValueConst v, size_t* units,
    int* odd)
{
    const uint8_t* data;
    size_t len;
    const char* owned;
    uint16_t* u16;

    if (dyn_text_bytes(ctx, v, &data, &len, &owned))
        return NULL;
    *odd = (int)(len & 1);
    *units = len >> 1;
    u16 = (uint16_t*)dyn_nat_malloc(*units ? *units * 2 : 2);
    if (!u16) {
        if (owned)
            JS_FreeCString(ctx, owned);
        JS_ThrowOutOfMemory(ctx);
        return NULL;
    }
    memcpy(u16, data, *units * 2);
    if (owned)
        JS_FreeCString(ctx, owned);
    return u16;
}

static JSValue dyn_text_utf8_to_utf16(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    const uint8_t* data;
    size_t len, out_units = 0;
    const char* owned;
    uint16_t* out;
    JSValue result;
    int rc;
    (void)this_val;

    if (argc < 1)
        return JS_ThrowTypeError(ctx, "utf8ToUtf16(bytesOrString)");
    if (dyn_text_bytes(ctx, argv[0], &data, &len, &owned))
        return JS_EXCEPTION;
    out = (uint16_t*)dyn_nat_malloc(len ? len * 2 : 2);
    if (!out) {
        if (owned)
            JS_FreeCString(ctx, owned);
        return JS_ThrowOutOfMemory(ctx);
    }
    rc = simd.utf8_to_utf16le(data, len, out, &out_units);
    if (owned)
        JS_FreeCString(ctx, owned);
    if (rc != 0) {
        dyn_nat_free(out);
        return JS_ThrowRangeError(ctx, "utf8ToUtf16: invalid UTF-8");
    }
    result = dyn_bytes_new_u8array(ctx, (const uint8_t*)out, out_units * 2);
    dyn_nat_free(out);
    return result;
}

static JSValue dyn_text_utf16_to_utf8(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    size_t units = 0, out_len = 0;
    int odd, rc;
    uint16_t* u16;
    uint8_t* out;
    JSValue result;
    (void)this_val;

    if (argc < 1)
        return JS_ThrowTypeError(ctx, "utf16ToUtf8(u16bytes)");
    u16 = dyn_text_u16_copy(ctx, argv[0], &units, &odd);
    if (!u16)
        return JS_EXCEPTION;
    if (odd) {
        dyn_nat_free(u16);
        return JS_ThrowRangeError(ctx, "utf16ToUtf8: byte length must be even");
    }
    out = (uint8_t*)dyn_nat_malloc(units ? units * 3 : 1);
    if (!out) {
        dyn_nat_free(u16);
        return JS_ThrowOutOfMemory(ctx);
    }
    rc = simd.utf16le_to_utf8(u16, units, out, &out_len);
    dyn_nat_free(u16);
    if (rc != 0) {
        dyn_nat_free(out);
        return JS_ThrowRangeError(ctx,
            "utf16ToUtf8: ill-formed UTF-16 surrogate");
    }
    result = dyn_bytes_new_u8array(ctx, out, out_len);
    dyn_nat_free(out);
    return result;
}

static JSValue dyn_text_is_valid_utf16(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    size_t units = 0;
    int odd, ok;
    uint16_t* u16;
    (void)this_val;

    if (argc < 1)
        return JS_ThrowTypeError(ctx, "isValidUtf16(u16bytes)");
    u16 = dyn_text_u16_copy(ctx, argv[0], &units, &odd);
    if (!u16)
        return JS_EXCEPTION;
    ok = !odd && simd.validate_utf16le(u16, units);
    dyn_nat_free(u16);
    return JS_NewBool(ctx, ok);
}

static JSValue dyn_text_count_utf16(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    size_t units = 0, c;
    int odd;
    uint16_t* u16;
    (void)this_val;

    if (argc < 1)
        return JS_ThrowTypeError(ctx, "countUtf16(u16bytes)");
    u16 = dyn_text_u16_copy(ctx, argv[0], &units, &odd);
    if (!u16)
        return JS_EXCEPTION;
    if (odd) {
        dyn_nat_free(u16);
        return JS_ThrowRangeError(ctx, "countUtf16: byte length must be even");
    }
    c = simd.count_utf16(u16, units);
    dyn_nat_free(u16);
    return JS_NewInt64(ctx, (int64_t)c);
}

typedef struct {
    JSValue str;
    int is_wide;
} dyn_txt_t;

static JSClassID dyn_txt_class_id;

static void dyn_txt_finalizer(JSRuntime* rt, JSValue val)
{
    dyn_txt_t* t = (dyn_txt_t*)JS_GetOpaque(val, dyn_txt_class_id);
    if (!t)
        return;
    JS_FreeValueRT(rt, t->str);
    dyn_nat_free(t);
}

static void dyn_txt_mark(JSRuntime* rt, JSValueConst val, JS_MarkFunc* mark_func)
{
    dyn_txt_t* t = (dyn_txt_t*)JS_GetOpaque(val, dyn_txt_class_id);
    if (t)
        JS_MarkValue(rt, t->str, mark_func);
}

static const JSClassDef dyn_txt_class = {
    "Text",
    .finalizer = dyn_txt_finalizer,
    .gc_mark = dyn_txt_mark,
};

static dyn_txt_t* dyn_txt_of(JSContext* ctx, JSValueConst v)
{
    return (dyn_txt_t*)JS_GetOpaque2(ctx, v, dyn_txt_class_id);
}

static JSValue dyn_txt_ctor(JSContext* ctx, JSValueConst new_target, int argc,
    JSValueConst* argv)
{
    dyn_txt_t* t;
    JSValue str, obj, proto;

    if (argc < 1)
        return JS_ThrowTypeError(ctx, "new Text(s) requires a string");
    proto = dyn_ctor_proto(ctx, new_target, dyn_txt_class_id);
    if (JS_IsException(proto))
        return proto;
    str = JS_ToString(ctx, argv[0]);
    if (JS_IsException(str)) {
        JS_FreeValue(ctx, proto);
        return str;
    }

    t = (dyn_txt_t*)dyn_nat_calloc(1, sizeof(*t));
    if (!t) {
        JS_FreeValue(ctx, proto);
        JS_FreeValue(ctx, str);
        return JS_ThrowOutOfMemory(ctx);
    }
    {
        size_t n = 0;
        const char* cs = JS_ToCStringLen(ctx, &n, str);
        if (!cs) {
            JS_FreeValue(ctx, proto);
            JS_FreeValue(ctx, str);
            dyn_nat_free(t);
            return JS_EXCEPTION;
        }
        t->is_wide = 0;
        for (size_t i = 0; i < n; i++)
            if ((unsigned char)cs[i] >= 0xC4) {
                t->is_wide = 1;
                break;
            }
        JS_FreeCString(ctx, cs);
    }
    t->str = str;
    obj = JS_NewObjectProtoClass(ctx, proto, dyn_txt_class_id);
    JS_FreeValue(ctx, proto);
    if (JS_IsException(obj)) {
        JS_FreeValue(ctx, str);
        dyn_nat_free(t);
        return obj;
    }
    JS_SetOpaque(obj, t);
    return obj;
}

#define DYN_TXT_FORWARD(name, fn, maxargs)                               \
    static JSValue name(JSContext* ctx, JSValueConst this_val, int argc, \
        JSValueConst* argv)                                              \
    {                                                                    \
        dyn_txt_t* t = dyn_txt_of(ctx, this_val);                        \
        JSValueConst a[(maxargs) + 1];                                   \
        int i;                                                           \
        if (!t)                                                          \
            return JS_EXCEPTION;                                         \
        a[0] = t->str;                                                   \
        for (i = 0; i < (maxargs); i++)                                  \
            a[i + 1] = (i < argc) ? argv[i] : JS_UNDEFINED;              \
        return fn(ctx, JS_UNDEFINED, (maxargs) + 1, a);                  \
    }

static size_t dyn_txt_u16_scan(JSContext* ctx, dyn_txt_t* t, int* valid)
{
    const uint8_t* p;
    const char* cs;
    size_t n, i, units = 0;

    {
        size_t bn;
        p = JS_GetNarrowStringBytes(ctx, t->str, &bn);
        if (p) {
            if (valid)
                *valid = 1;
            return bn;
        }
    }
    cs = JS_ToCStringLen2(ctx, &n, t->str, 1);
    if (!cs) {
        if (valid)
            *valid = 0;
        return 0;
    }
    p = (const uint8_t*)cs;
    i = 0;
    while (i < n) {
        if (p[i] < 0x80) {
            units++;
            i++;
        } else if ((p[i] & 0xE0) == 0xC0) {
            units++;
            i += 2;
        } else if (p[i] == 0xED && i + 2 < n && p[i + 1] >= 0xA0 && p[i + 1] <= 0xAF) {
            if (i + 5 < n && p[i + 3] == 0xED && p[i + 4] >= 0xB0 && p[i + 4] <= 0xBF) {
                units++;
                i += 6;
            } else {
                units++;
                if (valid) {
                    *valid = 0;
                    JS_FreeCString(ctx, cs);
                    return 0;
                }
                i += 3;
            }
        } else if (p[i] == 0xED && i + 2 < n && p[i + 1] >= 0xB0 && p[i + 1] <= 0xBF) {
            units++;
            if (valid) {
                *valid = 0;
                JS_FreeCString(ctx, cs);
                return 0;
            }
            i += 3;
        } else {
            units++;
            i += ((p[i] & 0xF0) == 0xE0) ? 3 : 1;
        }
    }
    JS_FreeCString(ctx, cs);
    if (valid)
        *valid = 1;
    return units;
}

static JSValue dyn_txt_m_valid_utf16(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_txt_t* t = dyn_txt_of(ctx, this_val);
    int valid = 0;
    (void)argc;
    (void)argv;
    if (!t)
        return JS_EXCEPTION;
    dyn_txt_u16_scan(ctx, t, &valid);
    return JS_NewBool(ctx, valid);
}

static JSValue dyn_txt_m_count_utf16_str(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_txt_t* t = dyn_txt_of(ctx, this_val);
    size_t c;
    (void)argc;
    (void)argv;
    if (!t)
        return JS_EXCEPTION;
    c = dyn_txt_u16_scan(ctx, t, NULL);
    return JS_NewInt64(ctx, (int64_t)c);
}
DYN_TXT_FORWARD(dyn_txt_m_count_utf8, dyn_text_count_utf8, 0)
DYN_TXT_FORWARD(dyn_txt_m_latin1_to_utf8, dyn_text_latin1_to_utf8, 0)
DYN_TXT_FORWARD(dyn_txt_m_utf8_to_latin1, dyn_text_utf8_to_latin1, 0)
DYN_TXT_FORWARD(dyn_txt_m_utf8_to_utf16, dyn_text_utf8_to_utf16, 0)

static JSValue dyn_txt_get(JSContext* ctx, JSValueConst this_val, int magic)
{
    dyn_txt_t* t = dyn_txt_of(ctx, this_val);
    JSValueConst a[1];
    if (!t)
        return JS_EXCEPTION;
    if (magic == 0)
        return JS_NewBool(ctx, t->is_wide);
    if (magic == 4) {
        a[0] = t->str;
        return dyn_text_is_valid_utf8(ctx, JS_UNDEFINED, 1, a);
    }
    return JS_DupValue(ctx, t->str);
}

static JSValue dyn_txt_to_string(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_txt_t* t = dyn_txt_of(ctx, this_val);
    (void)argc;
    (void)argv;
    if (!t)
        return JS_EXCEPTION;
    return JS_DupValue(ctx, t->str);
}

static JSValue dyn_txt_m_to_utf8(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_txt_t* t = dyn_txt_of(ctx, this_val);
    size_t sl;
    const char* cs;
    JSValue u8;
    (void)argc;
    (void)argv;
    if (!t)
        return JS_EXCEPTION;
    cs = JS_ToCStringLen(ctx, &sl, t->str);
    if (!cs)
        return JS_EXCEPTION;
    u8 = dyn_bytes_new_u8array(ctx, (const uint8_t*)cs, sl);
    JS_FreeCString(ctx, cs);
    return u8;
}

static JSValue dyn_txt_to_bytes(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    JSValue u8 = dyn_txt_m_to_utf8(ctx, this_val, 0, NULL);
    if (JS_IsException(u8))
        return u8;
    return dyn_bh_wrap(ctx, u8);
}

static JSValue dyn_txt_m_utf16_to_utf8(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_txt_t* t = dyn_txt_of(ctx, this_val);
    int valid = 0;
    (void)argc;
    (void)argv;
    if (!t)
        return JS_EXCEPTION;
    dyn_txt_u16_scan(ctx, t, &valid);
    if (!valid) {
        if (JS_HasException(ctx))
            return JS_EXCEPTION;
        return JS_ThrowRangeError(ctx, "utf16ToUtf8: ill-formed UTF-16 surrogate");
    }
    return dyn_txt_m_to_utf8(ctx, this_val, 0, NULL);
}

static const JSCFunctionListEntry dyn_txt_proto[] = {
    JS_CGETSET_MAGIC_DEF("isWide", dyn_txt_get, NULL, 0),
    JS_CGETSET_MAGIC_DEF("value", dyn_txt_get, NULL, 1),
    JS_CGETSET_MAGIC_DEF("isValidUtf8", dyn_txt_get, NULL, 4),
    JS_CFUNC_DEF("isValidUtf16", 0, dyn_txt_m_valid_utf16),
    JS_CFUNC_DEF("countUtf8", 0, dyn_txt_m_count_utf8),
    JS_CFUNC_DEF("countUtf16", 0, dyn_txt_m_count_utf16_str),
    JS_CFUNC_DEF("toUtf8", 0, dyn_txt_m_to_utf8),
    JS_CFUNC_DEF("latin1ToUtf8", 0, dyn_txt_m_latin1_to_utf8),
    JS_CFUNC_DEF("utf8ToLatin1", 0, dyn_txt_m_utf8_to_latin1),
    JS_CFUNC_DEF("utf8ToUtf16", 0, dyn_txt_m_utf8_to_utf16),
    JS_CFUNC_DEF("utf16ToUtf8", 0, dyn_txt_m_utf16_to_utf8),
    JS_CFUNC_DEF("toBytes", 0, dyn_txt_to_bytes),
    JS_CFUNC_DEF("toString", 0, dyn_txt_to_string),
    JS_CFUNC_DEF("toJSON", 0, dyn_txt_to_string),
};

static const JSCFunctionListEntry dyn_text_funcs[] = {
    JS_CFUNC_DEF("isValidUtf8", 1, dyn_text_is_valid_utf8),
    JS_CFUNC_DEF("latin1ToUtf8", 1, dyn_text_latin1_to_utf8),
    JS_CFUNC_DEF("utf8ToLatin1", 1, dyn_text_utf8_to_latin1),
    JS_CFUNC_DEF("countUtf8", 1, dyn_text_count_utf8),
    JS_CFUNC_DEF("utf8ToUtf16", 1, dyn_text_utf8_to_utf16),
    JS_CFUNC_DEF("utf16ToUtf8", 1, dyn_text_utf16_to_utf8),
    JS_CFUNC_DEF("isValidUtf16", 1, dyn_text_is_valid_utf16),
    JS_CFUNC_DEF("countUtf16", 1, dyn_text_count_utf16),
};

static const JSCFunctionListEntry dyn_bh_proto[] = {
    JS_CGETSET_MAGIC_DEF("length", dyn_bh_get, NULL, 0),
    JS_CGETSET_MAGIC_DEF("isAscii", dyn_bh_get, NULL, 1),
    JS_CGETSET_MAGIC_DEF("isValidUtf8", dyn_bh_get, NULL, 2),
    JS_CGETSET_MAGIC_DEF("array", dyn_bh_get, NULL, 3),
    JS_CFUNC_DEF("slice", 0, dyn_bh_slice),
    JS_CFUNC_DEF("compare", 1, dyn_bh_compare),
    JS_CFUNC_DEF("equals", 1, dyn_bh_equals),
    JS_CFUNC_DEF("indexOf", 2, dyn_bh_index_of),
    JS_CFUNC_DEF("lastIndexOf", 2, dyn_bh_last_index_of),
    JS_CFUNC_DEF("includes", 1, dyn_bh_includes),
    JS_CFUNC_DEF("count", 2, dyn_bh_count),
    JS_CFUNC_DEF("indexOfAny", 2, dyn_bh_index_of_any),
    JS_CFUNC_DEF("startsWith", 2, dyn_bh_starts_with),
    JS_CFUNC_DEF("endsWith", 2, dyn_bh_ends_with),
    JS_CFUNC_DEF("readBytes", 2, dyn_bh_read_bytes),
    JS_CFUNC_DEF("writeBytes", 2, dyn_bh_write_bytes),
    JS_CFUNC_DEF("fill", 1, dyn_bh_fill),
    JS_CFUNC_DEF("toUtf8", 0, dyn_bh_to_utf8),
    JS_CFUNC_DEF("toString", 0, dyn_bh_to_string),
    JS_CFUNC_MAGIC_DEF("readUint8", 1, dyn_bh_read, DYN_U8),
    JS_CFUNC_MAGIC_DEF("readInt8", 1, dyn_bh_read, DYN_I8),
    JS_CFUNC_MAGIC_DEF("readUint16LE", 1, dyn_bh_read, DYN_U16LE),
    JS_CFUNC_MAGIC_DEF("readUint16BE", 1, dyn_bh_read, DYN_U16BE),
    JS_CFUNC_MAGIC_DEF("readInt16LE", 1, dyn_bh_read, DYN_I16LE),
    JS_CFUNC_MAGIC_DEF("readInt16BE", 1, dyn_bh_read, DYN_I16BE),
    JS_CFUNC_MAGIC_DEF("readUint32LE", 1, dyn_bh_read, DYN_U32LE),
    JS_CFUNC_MAGIC_DEF("readUint32BE", 1, dyn_bh_read, DYN_U32BE),
    JS_CFUNC_MAGIC_DEF("readInt32LE", 1, dyn_bh_read, DYN_I32LE),
    JS_CFUNC_MAGIC_DEF("readInt32BE", 1, dyn_bh_read, DYN_I32BE),
    JS_CFUNC_MAGIC_DEF("readBigUint64LE", 1, dyn_bh_read, DYN_U64LE),
    JS_CFUNC_MAGIC_DEF("readBigUint64BE", 1, dyn_bh_read, DYN_U64BE),
    JS_CFUNC_MAGIC_DEF("readBigInt64LE", 1, dyn_bh_read, DYN_I64LE),
    JS_CFUNC_MAGIC_DEF("readBigInt64BE", 1, dyn_bh_read, DYN_I64BE),
    JS_CFUNC_MAGIC_DEF("readFloatLE", 1, dyn_bh_read, DYN_F32LE),
    JS_CFUNC_MAGIC_DEF("readFloatBE", 1, dyn_bh_read, DYN_F32BE),
    JS_CFUNC_MAGIC_DEF("readDoubleLE", 1, dyn_bh_read, DYN_F64LE),
    JS_CFUNC_MAGIC_DEF("readDoubleBE", 1, dyn_bh_read, DYN_F64BE),
    JS_CFUNC_MAGIC_DEF("writeUint8", 2, dyn_bh_write, DYN_U8),
    JS_CFUNC_MAGIC_DEF("writeInt8", 2, dyn_bh_write, DYN_I8),
    JS_CFUNC_MAGIC_DEF("writeUint16LE", 2, dyn_bh_write, DYN_U16LE),
    JS_CFUNC_MAGIC_DEF("writeUint16BE", 2, dyn_bh_write, DYN_U16BE),
    JS_CFUNC_MAGIC_DEF("writeInt16LE", 2, dyn_bh_write, DYN_I16LE),
    JS_CFUNC_MAGIC_DEF("writeInt16BE", 2, dyn_bh_write, DYN_I16BE),
    JS_CFUNC_MAGIC_DEF("writeUint32LE", 2, dyn_bh_write, DYN_U32LE),
    JS_CFUNC_MAGIC_DEF("writeUint32BE", 2, dyn_bh_write, DYN_U32BE),
    JS_CFUNC_MAGIC_DEF("writeInt32LE", 2, dyn_bh_write, DYN_I32LE),
    JS_CFUNC_MAGIC_DEF("writeInt32BE", 2, dyn_bh_write, DYN_I32BE),
    JS_CFUNC_MAGIC_DEF("writeBigUint64LE", 2, dyn_bh_write, DYN_U64LE),
    JS_CFUNC_MAGIC_DEF("writeBigUint64BE", 2, dyn_bh_write, DYN_U64BE),
    JS_CFUNC_MAGIC_DEF("writeBigInt64LE", 2, dyn_bh_write, DYN_I64LE),
    JS_CFUNC_MAGIC_DEF("writeBigInt64BE", 2, dyn_bh_write, DYN_I64BE),
    JS_CFUNC_MAGIC_DEF("writeFloatLE", 2, dyn_bh_write, DYN_F32LE),
    JS_CFUNC_MAGIC_DEF("writeFloatBE", 2, dyn_bh_write, DYN_F32BE),
    JS_CFUNC_MAGIC_DEF("writeDoubleLE", 2, dyn_bh_write, DYN_F64LE),
    JS_CFUNC_MAGIC_DEF("writeDoubleBE", 2, dyn_bh_write, DYN_F64BE),
};

#include "dyna-iconv.inc.c"

static const JSCFunctionListEntry dyn_bytes_funcs[] = {
    JS_CFUNC_DEF("decode", 2, dyn_iconv_decode),
    JS_CFUNC_DEF("encode", 2, dyn_iconv_encode),
    JS_CFUNC_DEF("encodingExists", 1, dyn_iconv_exists),
    JS_CFUNC_DEF("encodings", 0, dyn_iconv_list),
    JS_CFUNC_DEF("bytesOf", 1, dyn_bytes_bytes_of),
    JS_CFUNC_DEF("compare", 2, dyn_bytes_compare),
    JS_CFUNC_DEF("equal", 2, dyn_bytes_equal),
    JS_CFUNC_DEF("indexOf", 3, dyn_bytes_index_of),
    JS_CFUNC_DEF("lastIndexOf", 3, dyn_bytes_last_index_of),
    JS_CFUNC_DEF("contains", 2, dyn_bytes_contains),
    JS_CFUNC_DEF("count", 3, dyn_bytes_count),
    JS_CFUNC_DEF("concat", 1, dyn_bytes_concat),
    JS_CFUNC_DEF("copy", 5, dyn_bytes_copy),
    JS_CFUNC_DEF("fill", 4, dyn_bytes_fill),

    JS_CFUNC_MAGIC_DEF("readUint8", 2, dyn_bytes_read, DYN_U8),
    JS_CFUNC_MAGIC_DEF("readInt8", 2, dyn_bytes_read, DYN_I8),
    JS_CFUNC_MAGIC_DEF("readUint16LE", 2, dyn_bytes_read, DYN_U16LE),
    JS_CFUNC_MAGIC_DEF("readUint16BE", 2, dyn_bytes_read, DYN_U16BE),
    JS_CFUNC_MAGIC_DEF("readInt16LE", 2, dyn_bytes_read, DYN_I16LE),
    JS_CFUNC_MAGIC_DEF("readInt16BE", 2, dyn_bytes_read, DYN_I16BE),
    JS_CFUNC_MAGIC_DEF("readUint32LE", 2, dyn_bytes_read, DYN_U32LE),
    JS_CFUNC_MAGIC_DEF("readUint32BE", 2, dyn_bytes_read, DYN_U32BE),
    JS_CFUNC_MAGIC_DEF("readInt32LE", 2, dyn_bytes_read, DYN_I32LE),
    JS_CFUNC_MAGIC_DEF("readInt32BE", 2, dyn_bytes_read, DYN_I32BE),
    JS_CFUNC_MAGIC_DEF("readBigUint64LE", 2, dyn_bytes_read, DYN_U64LE),
    JS_CFUNC_MAGIC_DEF("readBigUint64BE", 2, dyn_bytes_read, DYN_U64BE),
    JS_CFUNC_MAGIC_DEF("readBigInt64LE", 2, dyn_bytes_read, DYN_I64LE),
    JS_CFUNC_MAGIC_DEF("readBigInt64BE", 2, dyn_bytes_read, DYN_I64BE),
    JS_CFUNC_MAGIC_DEF("readFloatLE", 2, dyn_bytes_read, DYN_F32LE),
    JS_CFUNC_MAGIC_DEF("readFloatBE", 2, dyn_bytes_read, DYN_F32BE),
    JS_CFUNC_MAGIC_DEF("readDoubleLE", 2, dyn_bytes_read, DYN_F64LE),
    JS_CFUNC_MAGIC_DEF("readDoubleBE", 2, dyn_bytes_read, DYN_F64BE),

    JS_CFUNC_MAGIC_DEF("writeUint8", 3, dyn_bytes_write, DYN_U8),
    JS_CFUNC_MAGIC_DEF("writeInt8", 3, dyn_bytes_write, DYN_I8),
    JS_CFUNC_MAGIC_DEF("writeUint16LE", 3, dyn_bytes_write, DYN_U16LE),
    JS_CFUNC_MAGIC_DEF("writeUint16BE", 3, dyn_bytes_write, DYN_U16BE),
    JS_CFUNC_MAGIC_DEF("writeInt16LE", 3, dyn_bytes_write, DYN_I16LE),
    JS_CFUNC_MAGIC_DEF("writeInt16BE", 3, dyn_bytes_write, DYN_I16BE),
    JS_CFUNC_MAGIC_DEF("writeUint32LE", 3, dyn_bytes_write, DYN_U32LE),
    JS_CFUNC_MAGIC_DEF("writeUint32BE", 3, dyn_bytes_write, DYN_U32BE),
    JS_CFUNC_MAGIC_DEF("writeInt32LE", 3, dyn_bytes_write, DYN_I32LE),
    JS_CFUNC_MAGIC_DEF("writeInt32BE", 3, dyn_bytes_write, DYN_I32BE),
    JS_CFUNC_MAGIC_DEF("writeBigUint64LE", 3, dyn_bytes_write, DYN_U64LE),
    JS_CFUNC_MAGIC_DEF("writeBigUint64BE", 3, dyn_bytes_write, DYN_U64BE),
    JS_CFUNC_MAGIC_DEF("writeBigInt64LE", 3, dyn_bytes_write, DYN_I64LE),
    JS_CFUNC_MAGIC_DEF("writeBigInt64BE", 3, dyn_bytes_write, DYN_I64BE),
    JS_CFUNC_MAGIC_DEF("writeFloatLE", 3, dyn_bytes_write, DYN_F32LE),
    JS_CFUNC_MAGIC_DEF("writeFloatBE", 3, dyn_bytes_write, DYN_F32BE),
    JS_CFUNC_MAGIC_DEF("writeDoubleLE", 3, dyn_bytes_write, DYN_F64LE),
    JS_CFUNC_MAGIC_DEF("writeDoubleBE", 3, dyn_bytes_write, DYN_F64BE),

    JS_CFUNC_DEF("toUtf8", 1, dyn_bytes_to_utf8),
    JS_CFUNC_DEF("fromUtf8", 1, dyn_bytes_from_utf8),
};

static int dyn_bytes_register(JSContext* ctx, JSModuleDef* m)
{
    JSRuntime* rt = JS_GetRuntime(ctx);
    JSValue proto, ctor;

    JS_NewClassID(&dyn_bh_class_id);
    if (JS_NewClass(rt, dyn_bh_class_id, &dyn_bh_class) < 0)
        return -1;
    proto = JS_NewObject(ctx);
    if (JS_IsException(proto))
        return -1;
    JS_SetPropertyFunctionList(ctx, proto, dyn_bh_proto, (int)countof(dyn_bh_proto));
    JS_SetClassProto(ctx, dyn_bh_class_id, proto);
    ctor = JS_NewCFunction2(ctx, dyn_bh_ctor, "Bytes", 1, JS_CFUNC_constructor, 0);
    if (JS_IsException(ctor))
        return -1;
    JS_SetConstructor(ctx, ctor, proto);
    JS_DefinePropertyValueStr(ctx, ctor, "alloc",
        JS_NewCFunctionMagic(ctx, dyn_bh_static, "alloc", 1, JS_CFUNC_generic_magic, 0), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, ctor, "isBytes",
        JS_NewCFunctionMagic(ctx, dyn_bh_static, "isBytes", 1, JS_CFUNC_generic_magic, 1), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, ctor, "concat",
        JS_NewCFunctionMagic(ctx, dyn_bh_static, "concat", 1, JS_CFUNC_generic_magic, 2), JS_PROP_C_W_E);
    if (JS_SetModuleExport(ctx, m, "Bytes", ctor) < 0)
        return -1;

    JS_NewClassID(&dyn_txt_class_id);
    if (JS_NewClass(rt, dyn_txt_class_id, &dyn_txt_class) < 0)
        return -1;
    proto = JS_NewObject(ctx);
    if (JS_IsException(proto))
        return -1;
    JS_SetPropertyFunctionList(ctx, proto, dyn_txt_proto, (int)countof(dyn_txt_proto));
    JS_SetClassProto(ctx, dyn_txt_class_id, proto);
    ctor = JS_NewCFunction2(ctx, dyn_txt_ctor, "Text", 1, JS_CFUNC_constructor, 0);
    if (JS_IsException(ctor))
        return -1;
    JS_SetConstructor(ctx, ctor, proto);
    return JS_SetModuleExport(ctx, m, "Text", ctor);
}

static int dyn_bytes_init_module(JSContext* ctx, JSModuleDef* m)
{
    if (dyn_bytes_register(ctx, m) < 0)
        return -1;
    if (JS_SetModuleExportList(ctx, m, dyn_text_funcs,
            countof(dyn_text_funcs))
        < 0)
        return -1;
    return JS_SetModuleExportList(ctx, m, dyn_bytes_funcs,
        countof(dyn_bytes_funcs));
}

int js_nat_init_bytes(JSContext* ctx)
{
    JSModuleDef* m;
    simd_init();
    m = JS_NewCModule(ctx, "dyna:bytes", dyn_bytes_init_module);
    if (!m)
        return -1;
    if (JS_AddModuleExport(ctx, m, "Bytes") < 0 || JS_AddModuleExport(ctx, m, "Text") < 0)
        return -1;
    if (JS_AddModuleExportList(ctx, m, dyn_text_funcs,
            countof(dyn_text_funcs))
        < 0)
        return -1;
    return JS_AddModuleExportList(ctx, m, dyn_bytes_funcs,
        countof(dyn_bytes_funcs));
}

#endif
