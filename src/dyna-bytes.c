/*
 * dyna:bytes -- byte-buffer construction, slicing, search and conversion.
 *
 * Nothing native escapes: every result is copied into fresh JS values at the
 * boundary and the native side is freed before returning.
 * Full API: docs/dynajs-guide/API.md.
 */
#include "dyna-nat.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_BYTES)

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "dyna-simd-kernels.h" /* strfind / base64 kernels, shared with strings + text */

#ifndef countof
#define countof(x) (sizeof(x) / sizeof((x)[0]))
#endif

/* ---------- buffer boundary: a byte-addressed view (Uint8Array/Int8Array/
 * Uint8ClampedArray/DataView) or a plain ArrayBuffer -> raw byte pointer +
 * length ---------- */

/* Resolve `v` to its backing byte pointer and length. Accepts a plain
 * ArrayBuffer (whole buffer), any 1-byte-per-element TypedArray view, or a
 * DataView (byte-addressed by definition); rejects a wider-element view
 * (Uint16Array etc) as a type error -- this module is byte-oriented only, and
 * silently reinterpreting a Float64Array as bytes would make `offset` ambiguous
 * between an element index and a byte offset. Pass such a view through
 * bytesOf() to state the reinterpretation explicitly. On failure a pending
 * exception is set and -1 is returned. The returned pointer is valid only for
 * the synchronous remainder of the call (see the module-level reentrancy note
 * above). */
/* Forward: a Bytes handle unwraps to the Uint8Array it owns, so every function
 * below accepts one wherever it accepts a raw view. Declared here because
 * dyn_bytes_view is the single door they all come through. */
static JSClassID dyn_bh_class_id;
static JSValue dyn_bh_backing(JSValueConst v);

static int dyn_bytes_view(JSContext *ctx, JSValueConst v, uint8_t **pp, size_t *pn)
{
    JSValue buf;
    uint8_t *base;
    size_t off, len, bpe, ab;

    {
        JSValue inner = dyn_bh_backing(v);
        if (!JS_IsUndefined(inner))
            v = inner;   /* borrowed: the handle owns it and outlives this call */
    }

    /* THE VIEW IS TRIED FIRST, and the order is worth 11.5x.
     *
     * JS_GetArrayBuffer THROWS when handed a Uint8Array -- it allocates an
     * Error object, which this function then discards. Probing the ArrayBuffer
     * case first therefore paid for a thrown-and-swallowed exception on EVERY
     * call with the argument type that is overwhelmingly the common one.
     * Measured, compare() over 4 KiB: 0.177 us when the argument is an
     * ArrayBuffer (first branch, no throw) against 2.042 us when it is a
     * Uint8Array (throw, clear, retry) -- for an operation whose actual work
     * is a memcmp of about 0.2 us. The exception WAS the function.
     *
     * A view is now resolved first and the ArrayBuffer is the fallback, so the
     * common path never constructs an exception. The rare path pays what the
     * common one used to. */
    buf = JS_GetArrayBufferView(ctx, v, &off, &len, &bpe);
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
        if (!base)
            return -1;   /* detached mid-resolve; JS_GetArrayBuffer threw */
        if (off > ab || len > ab - off) {
            JS_ThrowRangeError(ctx, "typed array out of bounds");
            return -1;
        }
        *pp = base + off;
        *pn = len;
        return 0;
    }
    JS_FreeValue(ctx, JS_GetException(ctx)); /* not a view: clear, try ArrayBuffer */

    base = JS_GetArrayBuffer(ctx, &ab, v);
    if (base) {
        *pp = base;
        *pn = ab;
        return 0;
    }
    return -1;
}

/* Build a fresh Uint8Array copying `len` bytes from `data` (never aliases a
 * native pointer into JS). `data` may be NULL only when len==0. */
static JSValue dyn_bytes_new_u8array(JSContext *ctx, const uint8_t *data, size_t len)
{
    static const uint8_t zero_stub = 0;
    JSValue ab, out;
    JSValueConst ta_args[3];

    if (len == 0)
        data = &zero_stub; /* never pass NULL into JS_NewArrayBufferCopy */
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

/* ---------- bytesOf: the explicit "reinterpret as bytes" operator ---------- */

/* bytesOf(view) -> a Uint8Array ALIASING exactly the bytes `view` spans.
 *
 * The one function here that deliberately does not copy. It exists so a wider
 * view (Float64Array, Uint16Array, ...) or a DataView can be fed to the byte
 * functions above with the reinterpretation stated in the source rather than
 * guessed by the module -- and so callers stop hand-rolling
 * `new Uint8Array(v.buffer, v.byteOffset, v.byteLength)`, whose classic bug is
 * dropping the offset/length pair and silently covering the WHOLE buffer instead
 * of the view's own range. Writes through the result are visible through the
 * original view and vice versa. Nothing native escapes: the alias is an ordinary
 * JS ArrayBuffer reference, exactly what the JS one-liner produces. An
 * ArrayBuffer argument yields a view over the whole buffer.
 *
 * Resolution is structural (JS_GetArrayBuffer / JS_GetArrayBufferView never run
 * JS), and there is no native handle to invalidate, so this needs no ordering
 * discipline beyond taking its single argument as given. */
static JSValue dyn_bytes_bytes_of(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv)
{
    JSValue buf, off_v, len_v, out;
    JSValueConst ta_args[3];
    uint8_t *base;
    size_t off, len, bpe, ab;

    (void)this_val;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "bytesOf(view)");

    base = JS_GetArrayBuffer(ctx, &ab, argv[0]);
    if (base) {
        buf = JS_DupValue(ctx, argv[0]);
        off = 0;
        len = ab;
    } else {
        JS_FreeValue(ctx, JS_GetException(ctx)); /* not an ArrayBuffer: retry as a view */
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

/* ---------- compare / equal ---------- */

static JSValue dyn_bytes_compare(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    uint8_t *a, *b;
    size_t an, bn, minlen;
    int cmp;
    (void)this_val; (void)argc;

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

static JSValue dyn_bytes_equal(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv)
{
    uint8_t *a, *b;
    size_t an, bn;
    (void)this_val; (void)argc;

    if (dyn_bytes_view(ctx, argv[0], &a, &an))
        return JS_EXCEPTION;
    if (dyn_bytes_view(ctx, argv[1], &b, &bn))
        return JS_EXCEPTION;

    return JS_NewBool(ctx, an == bn && (an == 0 || memcmp(a, b, an) == 0));
}

/* ---------- needle resolution shared by indexOf/lastIndexOf/contains/count ---------- */

/* Resolve a search needle: either a byte value (a JS Number, 0..255 via
 * ToInt32 truncation) or a byte view (Uint8Array/ArrayBuffer). A numeric
 * needle is always a primitive (JS_IsNumber is a pure tag check), so its
 * ToInt32 can never invoke user JS -- neither path here risks running JS
 * between resolving the needle and resolving `buf` (done by the caller
 * right after this returns). `byte_out` is 1 byte of caller-owned storage
 * used as the pattern buffer for the numeric case. */
static int dyn_bytes_needle(JSContext *ctx, JSValueConst v, uint8_t *byte_out,
                            const uint8_t **pat, size_t *plen)
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
        uint8_t *p;
        size_t n;
        if (dyn_bytes_view(ctx, v, &p, &n))
            return -1;
        *pat = p;
        *plen = n;
        return 0;
    }
}

/* Last occurrence of pat[0..plen) in text[0..tlen), or (size_t)-1. Plain
 * backward memcmp scan -- the shared SIMD engine has no reverse-search
 * kernel, and this is the standard textbook algorithm. Requires
 * 1 <= plen <= tlen (checked by every caller before this runs). */
static size_t dyn_bytes_last_find(const uint8_t *text, size_t tlen,
                                  const uint8_t *pat, size_t plen)
{
    size_t i = tlen - plen + 1;
    while (i > 0) {
        i--;
        if (memcmp(text + i, pat, plen) == 0)
            return i;
    }
    return (size_t)-1;
}

static JSValue dyn_bytes_index_of(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv)
{
    uint8_t needle_byte;
    const uint8_t *pat;
    size_t plen;
    uint8_t *buf;
    size_t n, pos;
    (void)this_val; (void)argc;

    if (dyn_bytes_needle(ctx, argv[1], &needle_byte, &pat, &plen))
        return JS_EXCEPTION;
    if (dyn_bytes_view(ctx, argv[0], &buf, &n))
        return JS_EXCEPTION;

    if (plen == 0)
        return JS_NewInt32(ctx, 0); /* bytes.Index(s, []) == 0 */
    if (plen > n)
        return JS_NewInt32(ctx, -1);
    pos = simd.strfind(buf, n, pat, plen);
    return pos == SIZE_MAX ? JS_NewInt32(ctx, -1) : JS_NewInt64(ctx, (int64_t)pos);
}

static JSValue dyn_bytes_last_index_of(JSContext *ctx, JSValueConst this_val,
                                       int argc, JSValueConst *argv)
{
    uint8_t needle_byte;
    const uint8_t *pat;
    size_t plen;
    uint8_t *buf;
    size_t n, pos;
    (void)this_val; (void)argc;

    if (dyn_bytes_needle(ctx, argv[1], &needle_byte, &pat, &plen))
        return JS_EXCEPTION;
    if (dyn_bytes_view(ctx, argv[0], &buf, &n))
        return JS_EXCEPTION;

    if (plen == 0)
        return JS_NewInt64(ctx, (int64_t)n); /* bytes.LastIndex(s, []) == len(s) */
    if (plen > n)
        return JS_NewInt32(ctx, -1);
    pos = dyn_bytes_last_find(buf, n, pat, plen);
    return pos == (size_t)-1 ? JS_NewInt32(ctx, -1) : JS_NewInt64(ctx, (int64_t)pos);
}

static JSValue dyn_bytes_contains(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv)
{
    uint8_t needle_byte;
    const uint8_t *pat;
    size_t plen;
    uint8_t *buf;
    size_t n;
    (void)this_val; (void)argc;

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

/* count(buf, needle): NON-overlapping occurrences, unlike dyna:strings'
 * indexOfAll (which is deliberately overlapping). See the module doc
 * comment for the empty-needle convention. */
static JSValue dyn_bytes_count(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv)
{
    uint8_t needle_byte;
    const uint8_t *pat;
    size_t plen;
    uint8_t *buf;
    size_t n, scan, cnt;
    (void)this_val; (void)argc;

    if (dyn_bytes_needle(ctx, argv[1], &needle_byte, &pat, &plen))
        return JS_EXCEPTION;
    if (dyn_bytes_view(ctx, argv[0], &buf, &n))
        return JS_EXCEPTION;

    if (plen == 0)
        return JS_NewInt64(ctx, (int64_t)n + 1);
    if (plen > n)
        return JS_NewInt32(ctx, 0);

    cnt = 0;
    scan = 0;
    while (scan <= n - plen) {
        size_t pos = simd.strfind(buf + scan, n - scan, pat, plen);
        if (pos == SIZE_MAX)
            break;
        cnt++;
        scan += pos + plen; /* non-overlapping: skip past this match */
    }
    return JS_NewInt64(ctx, (int64_t)cnt);
}

/* ---------- concat / copy / fill ---------- */

/* concat(arrayOfByteViews) -> Uint8Array. Pass 1 validates every element and
 * sums lengths (each element is fetched, resolved, and immediately
 * released -- no pointer is held across the next element's fetch, which may
 * run arbitrary JS via a getter/Proxy trap). Pass 2 re-resolves each
 * element fresh right before copying it; the copy length is clamped to
 * whatever remains of the pass-1-sized destination, so a length change from
 * JS run during element access (getter/Proxy) can never overflow this
 * module's own allocation -- it can only yield fewer/more source bytes than
 * pass 1 counted. */
static JSValue dyn_bytes_concat(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv)
{
    JSValue len_val, elem, result = JS_EXCEPTION;
    uint32_t n, i;
    uint8_t *out;
    size_t total, used;
    (void)this_val; (void)argc;

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
        uint8_t *p;
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

    out = (uint8_t *)malloc(total ? total : 1);
    if (!out)
        return JS_ThrowOutOfMemory(ctx);

    used = 0;
    for (i = 0; i < n; i++) {
        uint8_t *p;
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
    free(out);
    return result;
}

/* copy(dst, src, dstOff=0, srcOff=0, len=min(dst.length-dstOff, src.length-srcOff))
 * -> number of bytes copied. Overlap-safe (memmove), and the length defaults
 * to whichever buffer runs out first, as Node's Buffer.prototype.copy does.
 * All three optional scalars are coerced before either buffer is resolved. */
static JSValue dyn_bytes_copy(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv)
{
    uint64_t dst_off = 0, src_off = 0, len = UINT64_MAX; /* MAX = "not given" */
    uint8_t *dst, *src;
    size_t dn, sn;
    (void)this_val; (void)argc;

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

/* fill(buf, val, start=0, end=buf.length): sets buf[start..end) to the low 8
 * bits of val; returns buf. start/end/val are all coerced before buf is
 * resolved. */
static JSValue dyn_bytes_fill(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv)
{
    int32_t val;
    uint64_t start = 0, end = UINT64_MAX; /* MAX = "not given" */
    uint8_t *buf;
    size_t n;
    (void)this_val; (void)argc;

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
    return JS_DupValue(ctx, argv[0]);
}

/* ---------- fixed-width read/write, magic-dispatched over every width/sign/endian ---------- */

enum {
    DYN_U8, DYN_I8,
    DYN_U16LE, DYN_U16BE, DYN_I16LE, DYN_I16BE,
    DYN_U32LE, DYN_U32BE, DYN_I32LE, DYN_I32BE,
    DYN_U64LE, DYN_U64BE, DYN_I64LE, DYN_I64BE,
    DYN_F32LE, DYN_F32BE, DYN_F64LE, DYN_F64BE,
    DYN_FIELD_COUNT
};

typedef enum { DK_UINT, DK_INT, DK_FLOAT, DK_BIGUINT, DK_BIGINT } DynBytesKind;

typedef struct {
    uint8_t width; /* bytes: 1, 2, 4, or 8 */
    uint8_t be;    /* 1 = big-endian, 0 = little-endian (irrelevant when width==1) */
    uint8_t kind;  /* DynBytesKind */
} DynBytesField;

static const DynBytesField dyn_bytes_fields[DYN_FIELD_COUNT] = {
    [DYN_U8]    = { 1, 0, DK_UINT },
    [DYN_I8]    = { 1, 0, DK_INT },
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

/* Assemble `width` bytes at `p` (LE or BE per `be`) into a uint64_t (the raw
 * bit pattern, zero-extended). Host-endianness independent. */
static uint64_t dyn_bytes_load(const uint8_t *p, int width, int be)
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

/* Inverse of dyn_bytes_load: write the low `width` bytes of `v` to `p`. */
static void dyn_bytes_store(uint8_t *p, uint64_t v, int width, int be)
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

static JSValue dyn_bytes_read(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv, int magic)
{
    const DynBytesField *f = &dyn_bytes_fields[magic];
    uint64_t off;
    uint8_t *buf;
    size_t n;
    uint64_t v;
    (void)this_val; (void)argc;

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
        return JS_NewInt32(ctx, (int32_t)v); /* width 1 or 2: always non-negative */
    case DK_INT:
        if (f->width == 1)
            return JS_NewInt32(ctx, (int8_t)v);
        if (f->width == 2)
            return JS_NewInt32(ctx, (int16_t)v);
        return JS_NewInt32(ctx, (int32_t)v); /* width 4 */
    case DK_BIGUINT:
        return JS_NewBigUint64(ctx, v);
    case DK_BIGINT:
        return JS_NewBigInt64(ctx, (int64_t)v);
    case DK_FLOAT:
    default:
        if (f->width == 4) {
            union { uint32_t i; float f; } u;
            u.i = (uint32_t)v;
            return JS_NewFloat64(ctx, (double)u.f);
        } else {
            union { uint64_t i; double f; } u;
            u.i = v;
            return JS_NewFloat64(ctx, u.f);
        }
    }
}

static JSValue dyn_bytes_write(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv, int magic)
{
    const DynBytesField *f = &dyn_bytes_fields[magic];
    uint64_t off;
    uint32_t u32 = 0;
    int64_t i64 = 0;
    double d = 0;
    uint64_t v = 0;
    uint8_t *buf;
    size_t n;
    (void)this_val; (void)argc;

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
            union { uint32_t i; float f; } u;
            u.f = (float)d;
            v = u.i;
        } else {
            union { uint64_t i; double f; } u;
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
    return JS_NewInt64(ctx, (int64_t)(off + f->width));
}

/* ---------- utf8 conversion ----------
 *
 * Only the byte<->string boundary lives here. hex/base64 used to as well, under
 * toHex/fromHex/toBase64/toBase64 -- they were the same shared SIMD kernels
 * dyna:encoding already exposes, under a third set of names, so they are gone;
 * dyna:encoding is the single owner of every binary-to-text codec. */

/* toUtf8(buf) decodes the raw bytes as UTF-8 (lone/invalid sequences become
 * U+FFFD, matching JS_NewStringLen's general string-construction contract --
 * this is NOT validated UTF-8; use isValidUtf8 first if that
 * matters). fromUtf8(str) is the inverse (UTF-8 encode via JS_ToCStringLen,
 * the same encoder every other native module uses). */
static JSValue dyn_bytes_to_utf8(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    uint8_t *buf;
    size_t n;
    (void)this_val; (void)argc;

    if (dyn_bytes_view(ctx, argv[0], &buf, &n))
        return JS_EXCEPTION;
    return JS_NewStringLen(ctx, (const char *)buf, n);
}

static JSValue dyn_bytes_from_utf8(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv)
{
    const char *str;
    size_t len;
    JSValue result;
    (void)this_val; (void)argc;

    str = JS_ToCStringLen(ctx, &len, argv[0]);
    if (!str)
        return JS_EXCEPTION;
    result = dyn_bytes_new_u8array(ctx, (const uint8_t *)str, len);
    JS_FreeCString(ctx, str);
    return result;
}


/* ==================================================================== *
 *  Bytes -- the value handle over one contiguous buffer
 *
 *  Constructed WITH its data, so it is a value handle and not a compiled
 *  capability: it must be about as cheap as the Uint8Array it wraps, not
 *  merely cheaper after N uses.
 *
 *  It owns a Uint8Array (held as a JSValue, hence the gc_mark) and caches
 *  the two predicates that downstream code keeps recomputing: isAscii and
 *  isValidUtf8. Both are BOSCC summaries in the CLAUDE.md sec.4 sense --
 *  a cheap bit that lets a caller bypass a full UTF-8 validation pass --
 *  and both are computed once, in the constructor, never lazily. A lazy
 *  build would be a hidden write and the plan forbids it.
 *
 *  .slice() RETURNS A VIEW, not a copy: a fresh handle over a subarray of
 *  the same ArrayBuffer. That is the point of the method and it is also
 *  the hazard -- a short-lived view keeps its whole owner alive, because
 *  the JS reference to the backing Uint8Array is a strong one. That is the
 *  correct behaviour (a view into freed memory would be far worse) and it
 *  is what the RSS plateau test exists to characterise.
 * ==================================================================== */

typedef struct {
    JSValue u8;        /* the Uint8Array this handle owns */
    int is_ascii;
    int is_valid_utf8;
} dyn_bh_t;

static JSValue dyn_bh_backing(JSValueConst v)
{
    dyn_bh_t *b = (dyn_bh_t *)JS_GetOpaque(v, dyn_bh_class_id);
    return b ? b->u8 : JS_UNDEFINED;
}

static void dyn_bh_finalizer(JSRuntime *rt, JSValue val)
{
    dyn_bh_t *b = (dyn_bh_t *)JS_GetOpaque(val, dyn_bh_class_id);
    if (!b)
        return;
    JS_FreeValueRT(rt, b->u8);
    free(b);
}

/* Required: the handle holds a JSValue, so the cycle collector has to be able
 * to trace it. Without this a Bytes in a cycle would leak. */
static void dyn_bh_mark(JSRuntime *rt, JSValueConst val, JS_MarkFunc *mark_func)
{
    dyn_bh_t *b = (dyn_bh_t *)JS_GetOpaque(val, dyn_bh_class_id);
    if (b)
        JS_MarkValue(rt, b->u8, mark_func);
}

static const JSClassDef dyn_bh_class = {
    "Bytes", .finalizer = dyn_bh_finalizer, .gc_mark = dyn_bh_mark,
};

/* The two cached predicates. One pass, computed together, because a byte that
 * decides isAscii is a byte already loaded for isValidUtf8.
 *
 * THE ASCII PREFIX IS SWAR, and that is a BOSCC (CLAUDE.md sec.4): a cheap
 * summary that bypasses expensive work, where the bypass is usually taken.
 * Eight bytes at a time against 0x80..80 answers "is any high bit set", and
 * if none ever is then the buffer is ASCII and therefore also valid UTF-8 --
 * the whole multi-byte validator is skipped. That matters because it is not a
 * micro-optimisation of a rare path: EVERY Bytes construction and every
 * .slice() runs this, and the cost gate measured the byte-at-a-time version at
 * 17.8x a raw copy, with .slice() coming out O(n) instead of O(1).
 *
 * The word load is a memcpy, not a cast: a cast is UB on a misaligned address
 * and traps on some targets, and every compiler that matters emits one
 * instruction for it. */
static void dyn_bh_scan(const uint8_t *p, size_t n, int *ascii, int *utf8)
{
    size_t i = 0;

    /* ASCII prefix: SWAR, eight bytes at a time against 0x80..80. This is the
     * BOSCC (CLAUDE.md sec.4) -- pure ASCII is valid UTF-8 by definition, so
     * finding no high bit answers BOTH questions and the validator is never
     * called. `memcpy` for the load, not a cast: a cast is UB on a misaligned
     * address and traps on some targets. */
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

    /* Non-ASCII: hand the TAIL to the shared kernel rather than hand-rolling a
     * second UTF-8 validator. There was one here -- a scalar DFA covering
     * overlong, surrogate and out-of-range forms -- and it duplicated
     * simd.validate_utf8, which this very file already calls from
     * isValidUtf8(). Two implementations of one predicate is how they drift,
     * and this module has shipped that defect once already (psi vs polygamma).
     *
     * FROM `i`, NOT FROM 0. `i` is the first byte with the high bit set, so it
     * is the start of a multi-byte sequence (or a stray continuation byte,
     * which the kernel flags just as correctly). Re-validating the ASCII
     * prefix the SWAR loop just cleared is pure duplicated work, and it is not
     * theoretical: passing the whole buffer measured 37% SLOWER than the
     * scalar DFA on a 64 KiB buffer whose only bad sequence is at the end,
     * because the kernel rescanned 65,000 bytes the loop above had already
     * proved were ASCII. */
    *ascii = 0;
    *utf8 = (simd.validate_utf8(p + i, n - i) == n - i);
}

/* Wrap an existing Uint8Array VALUE (consumed) as a handle. `inherit_ascii`
 * skips the scan entirely: a slice of an ASCII buffer is ASCII, and an ASCII
 * buffer is valid UTF-8, so both flags are known without looking. This is what
 * makes .slice() O(1) rather than O(n) -- the cost gate caught it as 1.35x
 * between an 8-byte and a 4 KB slice, which is not what a view should look
 * like. isValidUtf8 does NOT inherit in general: a slice can cut a multi-byte
 * sequence in half, so the non-ASCII case still scans. */
static JSValue dyn_bh_wrap_flags(JSContext *ctx, JSValue u8, int inherit_ascii);

static JSValue dyn_bh_wrap(JSContext *ctx, JSValue u8)
{
    return dyn_bh_wrap_flags(ctx, u8, 0);
}

static JSValue dyn_bh_wrap_flags(JSContext *ctx, JSValue u8, int inherit_ascii)
{
    dyn_bh_t *b;
    uint8_t *p = NULL;
    size_t n = 0;
    JSValue obj;

    if (JS_IsException(u8))
        return u8;
    if (dyn_bytes_view(ctx, u8, &p, &n) < 0) {
        JS_FreeValue(ctx, u8);
        return JS_EXCEPTION;
    }
    b = (dyn_bh_t *)calloc(1, sizeof(*b));
    if (!b) {
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
    obj = JS_NewObjectClass(ctx, dyn_bh_class_id);
    if (JS_IsException(obj)) {
        JS_FreeValue(ctx, u8);
        free(b);
        return obj;
    }
    JS_SetOpaque(obj, b);
    return obj;
}

static dyn_bh_t *dyn_bh_of(JSContext *ctx, JSValueConst v)
{
    return (dyn_bh_t *)JS_GetOpaque2(ctx, v, dyn_bh_class_id);
}

static JSValue dyn_bh_ctor(JSContext *ctx, JSValueConst new_target, int argc,
                           JSValueConst *argv)
{
    uint8_t *p = NULL;
    size_t n = 0;

    (void)new_target;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "new Bytes(data) requires an argument");

    if (JS_IsString(argv[0])) {
        /* A string is encoded as UTF-8 -- the only interpretation that round
         * trips through Text, and the one every codec here assumes. */
        size_t sl;
        const char *cs = JS_ToCStringLen(ctx, &sl, argv[0]);
        JSValue u8;
        if (!cs)
            return JS_EXCEPTION;
        u8 = dyn_bytes_new_u8array(ctx, (const uint8_t *)cs, sl);
        JS_FreeCString(ctx, cs);
        return dyn_bh_wrap(ctx, u8);
    }
    if (dyn_bytes_view(ctx, argv[0], &p, &n) < 0)
        return JS_EXCEPTION;
    /* COPIED, not aliased. `new Bytes(u8)` must not let a later write through
     * the original Uint8Array change a handle's cached isAscii/isValidUtf8 --
     * the flags would silently become lies. .slice() is the deliberate
     * exception, and it is a view of a handle this class already owns. */
    return dyn_bh_wrap(ctx, dyn_bytes_new_u8array(ctx, p, n));
}

static JSValue dyn_bh_static(JSContext *ctx, JSValueConst this_val, int argc,
                             JSValueConst *argv, int magic)
{
    (void)this_val;
    if (magic == 0) { /* alloc(n) */
        uint64_t n = 0;
        if (argc < 1 || JS_ToIndex(ctx, &n, argv[0]))
            return JS_ThrowTypeError(ctx, "Bytes.alloc(n) requires a length");
        if (n > (uint64_t)1 << 31)
            return JS_ThrowRangeError(ctx, "Bytes.alloc: length too large");
        return dyn_bh_wrap(ctx, dyn_bytes_new_u8array(ctx, NULL, (size_t)n));
    }
    if (magic == 1) { /* isBytes(v) */
        return JS_NewBool(ctx, argc >= 1 &&
                          JS_GetOpaque(argv[0], dyn_bh_class_id) != NULL);
    }
    /* concat(list) -- one allocation, sized in a first pass. */
    {
        int64_t len = 0, i;
        size_t total = 0, o = 0;
        uint8_t *out;
        JSValue res;

        if (argc < 1 || !JS_IsArray(ctx, argv[0]))
            return JS_ThrowTypeError(ctx, "Bytes.concat(list) requires an array");
        {
            JSValue lv = JS_GetPropertyStr(ctx, argv[0], "length");
            if (JS_IsException(lv) || JS_ToInt64(ctx, &len, lv)) {
                JS_FreeValue(ctx, lv);
                return JS_EXCEPTION;
            }
            JS_FreeValue(ctx, lv);
        }
        for (i = 0; i < len; i++) {
            JSValue e = JS_GetPropertyUint32(ctx, argv[0], (uint32_t)i);
            uint8_t *p;
            size_t n;
            if (JS_IsException(e))
                return JS_EXCEPTION;
            if (dyn_bytes_view(ctx, e, &p, &n) < 0) {
                JS_FreeValue(ctx, e);
                return JS_EXCEPTION;
            }
            JS_FreeValue(ctx, e);
            total += n;
        }
        out = (uint8_t *)malloc(total ? total : 1);
        if (!out)
            return JS_ThrowOutOfMemory(ctx);
        for (i = 0; i < len; i++) {
            JSValue e = JS_GetPropertyUint32(ctx, argv[0], (uint32_t)i);
            uint8_t *p;
            size_t n;
            if (JS_IsException(e) || dyn_bytes_view(ctx, e, &p, &n) < 0) {
                JS_FreeValue(ctx, e);
                free(out);
                return JS_EXCEPTION;
            }
            if (o + n > total) { JS_FreeValue(ctx, e); free(out);
                return JS_ThrowTypeError(ctx, "Bytes.concat: the list changed during concatenation"); }
            memcpy(out + o, p, n);
            o += n;
            JS_FreeValue(ctx, e);
        }
        res = dyn_bh_wrap(ctx, dyn_bytes_new_u8array(ctx, out, o));
        free(out);
        return res;
    }
}

/* ---- getters: the cached summaries and the length ---------------------- */

static JSValue dyn_bh_get(JSContext *ctx, JSValueConst this_val, int magic)
{
    dyn_bh_t *b = dyn_bh_of(ctx, this_val);
    uint8_t *p;
    size_t n;
    if (!b)
        return JS_EXCEPTION;
    switch (magic) {
    case 0:
        if (dyn_bytes_view(ctx, b->u8, &p, &n) < 0)
            return JS_EXCEPTION;
        return JS_NewInt64(ctx, (int64_t)n);
    case 1: return JS_NewBool(ctx, b->is_ascii);
    case 2: return JS_NewBool(ctx, b->is_valid_utf8);
    default: return JS_DupValue(ctx, b->u8);   /* .array -- the owned view */
    }
}

/* ---- slice: a VIEW, sharing the owner's ArrayBuffer -------------------- */

static JSValue dyn_bh_slice(JSContext *ctx, JSValueConst this_val, int argc,
                            JSValueConst *argv)
{
    dyn_bh_t *b = dyn_bh_of(ctx, this_val);
    uint8_t *p;
    size_t n;
    int64_t start = 0, end;
    JSValue sub, ret;

    if (!b)
        return JS_EXCEPTION;
    if (dyn_bytes_view(ctx, b->u8, &p, &n) < 0)
        return JS_EXCEPTION;
    end = (int64_t)n;
    /* Coerce both bounds BEFORE touching the buffer: JS_ToInt64 can run a
     * valueOf. Nothing here can free the handle -- Bytes has no close() -- but
     * the ordering is the standing rule and costs nothing. */
    if (argc >= 1 && !JS_IsUndefined(argv[0]) && JS_ToInt64(ctx, &start, argv[0]))
        return JS_EXCEPTION;
    if (argc >= 2 && !JS_IsUndefined(argv[1]) && JS_ToInt64(ctx, &end, argv[1]))
        return JS_EXCEPTION;
    if (start < 0) start += (int64_t)n;
    if (end < 0) end += (int64_t)n;
    if (start < 0) start = 0;
    if (end > (int64_t)n) end = (int64_t)n;
    if (end < start) end = start;

    /* subarray, not slice: the result SHARES the ArrayBuffer. The new handle
     * holds a strong reference to that buffer, so the owner stays alive for
     * exactly as long as any view of it does. */
    /* Built with the C typed-array constructor over the owner's ArrayBuffer,
     * NOT by looking up and calling `subarray` on the JS object.
     *
     * The JS route measured 2.96 us for an EIGHT-BYTE slice -- ten times the
     * cost of copying the whole 4 KiB buffer -- because a property lookup plus
     * a full JS call dominates everything a view actually does. A view that
     * costs more than a copy is not a view; this is the same "O(1) in name
     * only" defect the cost gate caught when .slice() was re-scanning. */
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
        a[0] = av[0]; a[1] = av[1]; a[2] = av[2];
        sub = JS_NewTypedArray(ctx, 3, a, JS_TYPED_ARRAY_UINT8);
        JS_FreeValue(ctx, av[1]);
        JS_FreeValue(ctx, av[2]);
        JS_FreeValue(ctx, ab);
        if (JS_IsException(sub))
            return sub;
    }
    ret = dyn_bh_wrap_flags(ctx, sub, b->is_ascii);
    return ret;
}

/* ---- methods that delegate to the free functions -----------------------
 *
 * Each builds a stack argv with `this`'s buffer first, then forwards. No
 * allocation, and one implementation of every algorithm rather than two. */
#define DYN_BH_FORWARD(name, fn, maxargs)                                     \
    static JSValue name(JSContext *ctx, JSValueConst this_val, int argc,      \
                        JSValueConst *argv)                                   \
    {                                                                         \
        dyn_bh_t *b = dyn_bh_of(ctx, this_val);                               \
        JSValueConst a[(maxargs) + 1];                                        \
        int i;                                                                \
        if (!b)                                                               \
            return JS_EXCEPTION;                                              \
        a[0] = b->u8;                                                         \
        for (i = 0; i < (maxargs); i++)                                       \
            a[i + 1] = (i < argc) ? argv[i] : JS_UNDEFINED;                   \
        return fn(ctx, JS_UNDEFINED, (maxargs) + 1, a);                       \
    }

DYN_BH_FORWARD(dyn_bh_compare, dyn_bytes_compare, 1)
DYN_BH_FORWARD(dyn_bh_equals, dyn_bytes_equal, 1)
DYN_BH_FORWARD(dyn_bh_index_of, dyn_bytes_index_of, 2)
DYN_BH_FORWARD(dyn_bh_last_index_of, dyn_bytes_last_index_of, 2)
DYN_BH_FORWARD(dyn_bh_includes, dyn_bytes_contains, 1)
DYN_BH_FORWARD(dyn_bh_count, dyn_bytes_count, 1)
DYN_BH_FORWARD(dyn_bh_fill, dyn_bytes_fill, 3)
DYN_BH_FORWARD(dyn_bh_to_utf8, dyn_bytes_to_utf8, 0)
/* The one dyna:text byte scan with no dyna:bytes equivalent: first
 * position holding ANY of a set of bytes. It moves onto the handle
 * rather than staying a free function. Declared ahead of the Text
 * section, which is where the body lives. */
static JSValue dyn_text_index_of_any(JSContext *ctx, JSValueConst this_val,
                                     int argc, JSValueConst *argv);
DYN_BH_FORWARD(dyn_bh_index_of_any, dyn_text_index_of_any, 1)

/* read/write are magic-dispatched over 18 widths each; forwarding keeps that
 * single switch rather than duplicating 36 entries. */
static JSValue dyn_bh_read(JSContext *ctx, JSValueConst this_val, int argc,
                           JSValueConst *argv, int magic)
{
    dyn_bh_t *b = dyn_bh_of(ctx, this_val);
    JSValueConst a[2];
    if (!b)
        return JS_EXCEPTION;
    a[0] = b->u8;
    a[1] = (argc > 0) ? argv[0] : JS_UNDEFINED;
    return dyn_bytes_read(ctx, JS_UNDEFINED, 2, a, magic);
}

static JSValue dyn_bh_write(JSContext *ctx, JSValueConst this_val, int argc,
                            JSValueConst *argv, int magic)
{
    dyn_bh_t *b = dyn_bh_of(ctx, this_val);
    JSValueConst a[3];
    if (!b)
        return JS_EXCEPTION;
    a[0] = b->u8;
    a[1] = (argc > 0) ? argv[0] : JS_UNDEFINED;
    a[2] = (argc > 1) ? argv[1] : JS_UNDEFINED;
    return dyn_bytes_write(ctx, JS_UNDEFINED, 3, a, magic);
}

static JSValue dyn_bh_to_string(JSContext *ctx, JSValueConst this_val, int argc,
                                JSValueConst *argv)
{
    (void)argc; (void)argv;
    return dyn_bh_to_utf8(ctx, this_val, 0, NULL);
}


/* ==================================================================== *
 *  Text -- an interpretation of bytes, folded in from the retired
 *  dyna:text module.
 *
 *  Bytes is storage; Text is a reading of it. They are a pair of views on
 *  the same material, which is why they live in one module rather than
 *  two -- the same argument that folded dyna:path into dyna:file.
 *
 *  A Text wraps a JS string and caches isWideChar: whether any code unit
 *  is above U+00FF. That is the summary bit the engine itself uses to
 *  decide between a byte array and a uint16 array, and it is what lets a
 *  caller skip a UTF-16 scan when there is nothing above Latin-1 to find.
 * ==================================================================== */

static int dyn_text_bytes(JSContext *ctx, JSValueConst v, const uint8_t **data,
                          size_t *len, const char **owned)
{
    *owned = NULL;
    if (JS_IsString(v)) {
        const char *s = JS_ToCStringLen(ctx, len, v);
        if (!s)
            return -1;
        *owned = s;
        *data = (const uint8_t *)s;
        return 0;
    }
    {
        /* VIEW FIRST, and GetArrayBufferView rather than GetTypedArrayBuffer.
         *
         * Two defects fixed here, both already found and fixed elsewhere:
         * probing ArrayBuffer first made every TypedArray argument allocate,
         * format and discard a TypeError -- dyn_bytes_view in this same file
         * measured that at 0.177us against 2.042us -- and every caller here
         * passes a view (Bytes.indexOfAny forwards b->u8 unconditionally).
         * And GetTypedArrayBuffer REJECTS a DataView, so one fell through to
         * the string branch below and was read as "[object DataView]": 17
         * bytes of ASCII that validate, count and transcode without error. */
        size_t off = 0, blen = 0, bpe = 0;
        JSValue buf = JS_GetArrayBufferView(ctx, v, &off, &blen, &bpe);
        if (!JS_IsException(buf)) {
            size_t absize = 0;
            uint8_t *ab = JS_GetArrayBuffer(ctx, &absize, buf);
            JS_FreeValue(ctx, buf);
            if (ab) {
                *data = ab + off;
                *len = blen;
                return 0;
            }
        }
        JS_FreeValue(ctx, JS_GetException(ctx));
    }
    {
        uint8_t *ab = JS_GetArrayBuffer(ctx, len, v);
        if (ab) {
            *data = ab;
            return 0;
        }
        JS_FreeValue(ctx, JS_GetException(ctx));
    }
    /* fall back to string coercion for other types (numbers, etc.) */
    {
        const char *s = JS_ToCStringLen(ctx, len, v);
        if (!s)
            return -1;
        *owned = s;
        *data = (const uint8_t *)s;
        return 0;
    }
}



/* indexOfAny(text, chars) -> first byte index of any char in `chars`, or -1. */
static JSValue dyn_text_index_of_any(JSContext *ctx, JSValueConst this_val,
                                     int argc, JSValueConst *argv)
{
    const uint8_t *data, *set;
    size_t len, setlen, pos;
    const char *owned_data, *owned_set;
    (void)this_val;

    if (argc < 2)
        return JS_ThrowTypeError(ctx, "indexOfAny(text, chars)");
    if (dyn_text_bytes(ctx, argv[1], &set, &setlen, &owned_set))
        return JS_EXCEPTION;
    if (dyn_text_bytes(ctx, argv[0], &data, &len, &owned_data)) {
        if (owned_set)
            JS_FreeCString(ctx, owned_set);
        return JS_EXCEPTION;
    }
    pos = simd.find_first_of(data, len, set, setlen);
    if (owned_data)
        JS_FreeCString(ctx, owned_data);
    if (owned_set)
        JS_FreeCString(ctx, owned_set);
    return pos == SIZE_MAX ? JS_NewInt32(ctx, -1)
                           : JS_NewInt64(ctx, (int64_t)pos);
}

/* isValidUtf8(data) -> true if `data`'s bytes are well-formed UTF-8. */
static JSValue dyn_text_is_valid_utf8(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv)
{
    const uint8_t *data;
    size_t len;
    const char *owned;
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

/* base64Encode(data) -> standard base64 string ('+/' alphabet, '=' padded). */
/* latin1ToUtf8(bytes) -> Uint8Array; each byte is a latin1 code point re-encoded
 * as UTF-8 (bytes <0x80 copy, 0x80..0xFF expand to two bytes). */
static JSValue dyn_text_latin1_to_utf8(JSContext *ctx, JSValueConst this_val,
                                       int argc, JSValueConst *argv)
{
    const uint8_t *data;
    size_t len, out_len;
    const char *owned;
    uint8_t *out;
    JSValue result;
    (void)this_val;

    if (argc < 1)
        return JS_ThrowTypeError(ctx, "latin1ToUtf8(bytes)");
    if (dyn_text_bytes(ctx, argv[0], &data, &len, &owned))
        return JS_EXCEPTION;
    out = (uint8_t *)malloc(len ? len * 2 : 1);
    if (!out) {
        if (owned)
            JS_FreeCString(ctx, owned);
        return JS_ThrowOutOfMemory(ctx);
    }
    out_len = simd.latin1_to_utf8(data, len, out);
    if (owned)
        JS_FreeCString(ctx, owned);
    result = dyn_bytes_new_u8array(ctx, out, out_len);
    free(out);
    return result;
}

/* utf8ToLatin1(bytes) -> Uint8Array; throws RangeError if the input is invalid
 * UTF-8 or contains any code point > 0xFF (not representable in latin1). */
static JSValue dyn_text_utf8_to_latin1(JSContext *ctx, JSValueConst this_val,
                                       int argc, JSValueConst *argv)
{
    const uint8_t *data;
    size_t len, out_len = 0;
    const char *owned;
    uint8_t *out;
    JSValue result;
    int rc;
    (void)this_val;

    if (argc < 1)
        return JS_ThrowTypeError(ctx, "utf8ToLatin1(bytes)");
    if (dyn_text_bytes(ctx, argv[0], &data, &len, &owned))
        return JS_EXCEPTION;
    out = (uint8_t *)malloc(len ? len : 1);
    if (!out) {
        if (owned)
            JS_FreeCString(ctx, owned);
        return JS_ThrowOutOfMemory(ctx);
    }
    rc = simd.utf8_to_latin1(data, len, out, &out_len);
    if (owned)
        JS_FreeCString(ctx, owned);
    if (rc != 0) {
        free(out);
        return JS_ThrowRangeError(
            ctx, "utf8ToLatin1: invalid UTF-8 or code point > 0xFF");
    }
    result = dyn_bytes_new_u8array(ctx, out, out_len);
    free(out);
    return result;
}

/* countUtf8(data) -> number of UTF-8 code points (assumes valid UTF-8). */
static JSValue dyn_text_count_utf8(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv)
{
    const uint8_t *data;
    size_t len;
    const char *owned;
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

/* Borrow input as UTF-16 code units: read raw bytes via dyn_text_bytes, then
 * copy into a fresh 2-byte-aligned uint16_t buffer (a TypedArray view may start
 * at an odd byte offset, and the scalar kernels do aligned u16 reads). On
 * success returns the malloc'd buffer (release with free()), sets *units to
 * byte_len/2 and *odd to the trailing-odd-byte flag. Returns NULL with a
 * pending exception on error/OOM. A 0-unit input returns a valid non-NULL stub.
 * On this engine's little-endian targets the input bytes ARE UTF-16LE, so the
 * host-endian uint16_t copy is a straight reinterpretation. */
static uint16_t *dyn_text_u16_copy(JSContext *ctx, JSValueConst v, size_t *units,
                                   int *odd)
{
    const uint8_t *data;
    size_t len;
    const char *owned;
    uint16_t *u16;

    if (dyn_text_bytes(ctx, v, &data, &len, &owned))
        return NULL;
    *odd = (int)(len & 1);
    *units = len >> 1;
    u16 = (uint16_t *)malloc(*units ? *units * 2 : 2);
    if (!u16) {
        if (owned)
            JS_FreeCString(ctx, owned);
        JS_ThrowOutOfMemory(ctx);
        return NULL;
    }
    memcpy(u16, data, *units * 2); /* whole units only; a trailing odd byte drops */
    if (owned)
        JS_FreeCString(ctx, owned);
    return u16;
}

/* utf8ToUtf16(bytesOrString) -> Uint8Array of UTF-16LE bytes. Strict/lossless:
 * throws RangeError on malformed UTF-8 (matching simdutf's convert path). */
static JSValue dyn_text_utf8_to_utf16(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv)
{
    const uint8_t *data;
    size_t len, out_units = 0;
    const char *owned;
    uint16_t *out;
    JSValue result;
    int rc;
    (void)this_val;

    if (argc < 1)
        return JS_ThrowTypeError(ctx, "utf8ToUtf16(bytesOrString)");
    if (dyn_text_bytes(ctx, argv[0], &data, &len, &owned))
        return JS_EXCEPTION;
    out = (uint16_t *)malloc(len ? len * 2 : 2); /* <= len units => 2*len bytes */
    if (!out) {
        if (owned)
            JS_FreeCString(ctx, owned);
        return JS_ThrowOutOfMemory(ctx);
    }
    rc = simd.utf8_to_utf16le(data, len, out, &out_units);
    if (owned)
        JS_FreeCString(ctx, owned);
    if (rc != 0) {
        free(out);
        return JS_ThrowRangeError(ctx, "utf8ToUtf16: invalid UTF-8");
    }
    result = dyn_bytes_new_u8array(ctx, (const uint8_t *)out, out_units * 2);
    free(out);
    return result;
}

/* utf16ToUtf8(u16bytes) -> Uint8Array of UTF-8 bytes. Strict/lossless: throws
 * RangeError on an odd byte length or an ill-formed surrogate. */
static JSValue dyn_text_utf16_to_utf8(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv)
{
    size_t units = 0, out_len = 0;
    int odd, rc;
    uint16_t *u16;
    uint8_t *out;
    JSValue result;
    (void)this_val;

    if (argc < 1)
        return JS_ThrowTypeError(ctx, "utf16ToUtf8(u16bytes)");
    u16 = dyn_text_u16_copy(ctx, argv[0], &units, &odd);
    if (!u16)
        return JS_EXCEPTION;
    if (odd) {
        free(u16);
        return JS_ThrowRangeError(ctx, "utf16ToUtf8: byte length must be even");
    }
    out = (uint8_t *)malloc(units ? units * 3 : 1); /* <= 3 UTF-8 bytes per unit */
    if (!out) {
        free(u16);
        return JS_ThrowOutOfMemory(ctx);
    }
    rc = simd.utf16le_to_utf8(u16, units, out, &out_len);
    free(u16);
    if (rc != 0) {
        free(out);
        return JS_ThrowRangeError(ctx,
                                  "utf16ToUtf8: ill-formed UTF-16 surrogate");
    }
    result = dyn_bytes_new_u8array(ctx, out, out_len);
    free(out);
    return result;
}

/* isValidUtf16(u16bytes) -> true if the bytes are well-formed UTF-16LE (even
 * length, every high surrogate paired with a following low surrogate). */
static JSValue dyn_text_is_valid_utf16(JSContext *ctx, JSValueConst this_val,
                                       int argc, JSValueConst *argv)
{
    size_t units = 0;
    int odd, ok;
    uint16_t *u16;
    (void)this_val;

    if (argc < 1)
        return JS_ThrowTypeError(ctx, "isValidUtf16(u16bytes)");
    u16 = dyn_text_u16_copy(ctx, argv[0], &units, &odd);
    if (!u16)
        return JS_EXCEPTION;
    ok = !odd && simd.validate_utf16le(u16, units); /* odd byte length is ill-formed */
    free(u16);
    return JS_NewBool(ctx, ok);
}

/* countUtf16(u16bytes) -> number of code points (surrogate pairs count once).
 * Does not validate; a trailing odd byte is ignored. */
static JSValue dyn_text_count_utf16(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv)
{
    size_t units = 0, c;
    int odd;
    uint16_t *u16;
    (void)this_val;

    if (argc < 1)
        return JS_ThrowTypeError(ctx, "countUtf16(u16bytes)");
    u16 = dyn_text_u16_copy(ctx, argv[0], &units, &odd);
    if (!u16)
        return JS_EXCEPTION;
    c = simd.count_utf16(u16, units);
    free(u16);
    return JS_NewInt64(ctx, (int64_t)c);
}


typedef struct {
    JSValue str;
    int is_wide;
} dyn_txt_t;

static JSClassID dyn_txt_class_id;

static void dyn_txt_finalizer(JSRuntime *rt, JSValue val)
{
    dyn_txt_t *t = (dyn_txt_t *)JS_GetOpaque(val, dyn_txt_class_id);
    if (!t)
        return;
    JS_FreeValueRT(rt, t->str);
    free(t);
}

static void dyn_txt_mark(JSRuntime *rt, JSValueConst val, JS_MarkFunc *mark_func)
{
    dyn_txt_t *t = (dyn_txt_t *)JS_GetOpaque(val, dyn_txt_class_id);
    if (t)
        JS_MarkValue(rt, t->str, mark_func);
}

static const JSClassDef dyn_txt_class = {
    "Text", .finalizer = dyn_txt_finalizer, .gc_mark = dyn_txt_mark,
};

static dyn_txt_t *dyn_txt_of(JSContext *ctx, JSValueConst v)
{
    return (dyn_txt_t *)JS_GetOpaque2(ctx, v, dyn_txt_class_id);
}

static JSValue dyn_txt_ctor(JSContext *ctx, JSValueConst new_target, int argc,
                            JSValueConst *argv)
{
    dyn_txt_t *t;
    JSValue str, obj;

    (void)new_target;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "new Text(s) requires a string");
    str = JS_ToString(ctx, argv[0]);
    if (JS_IsException(str))
        return str;

    t = (dyn_txt_t *)calloc(1, sizeof(*t));
    if (!t) {
        JS_FreeValue(ctx, str);
        return JS_ThrowOutOfMemory(ctx);
    }
    /* One scan at construction, never lazily: a lazy build would be a hidden
     * write to shared state, which the plan forbids for exactly the reason
     * TSan would find it. */
    {
        const char *cs = JS_ToCString(ctx, str);
        t->is_wide = 0;
        if (cs) {
            size_t i, n = strlen(cs);
            for (i = 0; i < n; i++)
                if ((unsigned char)cs[i] >= 0xC4) { t->is_wide = 1; break; }
            JS_FreeCString(ctx, cs);
        }
    }
    t->str = str;
    obj = JS_NewObjectClass(ctx, dyn_txt_class_id);
    if (JS_IsException(obj)) {
        JS_FreeValue(ctx, str);
        free(t);
        return obj;
    }
    JS_SetOpaque(obj, t);
    return obj;
}

/* Same forwarding trick as Bytes: `this`'s string leads the argument list, so
 * there is one implementation of each conversion rather than two. */
#define DYN_TXT_FORWARD(name, fn, maxargs)                                    \
    static JSValue name(JSContext *ctx, JSValueConst this_val, int argc,      \
                        JSValueConst *argv)                                   \
    {                                                                         \
        dyn_txt_t *t = dyn_txt_of(ctx, this_val);                             \
        JSValueConst a[(maxargs) + 1];                                        \
        int i;                                                                \
        if (!t)                                                               \
            return JS_EXCEPTION;                                              \
        a[0] = t->str;                                                        \
        for (i = 0; i < (maxargs); i++)                                       \
            a[i + 1] = (i < argc) ? argv[i] : JS_UNDEFINED;                   \
        return fn(ctx, JS_UNDEFINED, (maxargs) + 1, a);                       \
    }

DYN_TXT_FORWARD(dyn_txt_m_valid_utf8, dyn_text_is_valid_utf8, 0)
/* THE UTF-16-INPUT FAMILY NEEDS UTF-16 BYTES, AND t->str COERCES TO UTF-8.
   Forwarding the string straight through handed them the wrong encoding:
   Text("abc").countUtf16() answered 1 instead of 3, isValidUtf16() answered
   FALSE for plain ASCII, and utf16ToUtf8() threw "byte length must be even"
   on any odd-length string. The UTF-8-input methods were fine, which is why
   this looked like it worked.

   Worse, it gave RIGHT ANSWERS BY ACCIDENT on the obvious probes -- a lone
   surrogate really did come back invalid -- so a test written against those
   would have passed and pinned the bug in place. */
static uint16_t *dyn_txt_u16_of(JSContext *ctx, dyn_txt_t *t, size_t *punits)
{
    size_t sl, i;
    const char *cs = JS_ToCStringLen(ctx, &sl, t->str);
    uint16_t *u16;
    JSValue lv;
    uint32_t n = 0;
    if (!cs)
        return NULL;
    JS_FreeCString(ctx, cs);
    /* The string's own UTF-16 code units, taken from the engine rather than
       re-derived: charCodeAt is the definition of a unit here. */
    lv = JS_GetPropertyStr(ctx, t->str, "length");
    JS_ToUint32(ctx, &n, lv);
    JS_FreeValue(ctx, lv);
    u16 = (uint16_t *)malloc((n ? n : 1) * sizeof(uint16_t));
    if (!u16)
        return NULL;
    /* charCodeAt, NOT str[i]: indexing a JS string yields a ONE-CHARACTER
       STRING, and JS_ToUint32("a") is 0 -- so every unit came back zero, no
       surrogate was ever seen, and "abc" counted correctly BY LUCK while
       "a\u{1F600}" did not. */
    {
        JSAtom cca = JS_NewAtom(ctx, "charCodeAt");
        for (i = 0; i < n; i++) {
            JSValue idx = JS_NewUint32(ctx, (uint32_t)i);
            JSValue c = JS_Invoke(ctx, t->str, cca, 1, (JSValueConst *)&idx);
            uint32_t v = 0;
            JS_ToUint32(ctx, &v, c);
            JS_FreeValue(ctx, c);
            JS_FreeValue(ctx, idx);
            u16[i] = (uint16_t)v;
        }
        JS_FreeAtom(ctx, cca);
    }
    *punits = n;
    return u16;
}

/* isValidUtf16() on a TEXT means "no lone surrogate", which is the only thing
   that can be ill-formed about a JS string. */
static JSValue dyn_txt_m_valid_utf16(JSContext *ctx, JSValueConst this_val,
                                     int argc, JSValueConst *argv)
{
    dyn_txt_t *t = dyn_txt_of(ctx, this_val);
    size_t units = 0, i;
    uint16_t *u16;
    int ok = 1;
    (void)argc; (void)argv;
    if (!t)
        return JS_EXCEPTION;
    u16 = dyn_txt_u16_of(ctx, t, &units);
    if (!u16)
        return JS_EXCEPTION;
    for (i = 0; i < units; i++) {
        if (u16[i] >= 0xD800 && u16[i] <= 0xDBFF) {
            if (i + 1 >= units || u16[i + 1] < 0xDC00 || u16[i + 1] > 0xDFFF)
                { ok = 0; break; }
            i++;
        } else if (u16[i] >= 0xDC00 && u16[i] <= 0xDFFF) {
            ok = 0; break;          /* a low surrogate with no high before it */
        }
    }
    free(u16);
    return JS_NewBool(ctx, ok);
}

/* countUtf16() counts CODE POINTS, matching countUtf8 and this class's own
   documented contract ("surrogate pairs count once"). */
static JSValue dyn_txt_m_count_utf16_str(JSContext *ctx, JSValueConst this_val,
                                         int argc, JSValueConst *argv)
{
    dyn_txt_t *t = dyn_txt_of(ctx, this_val);
    size_t units = 0, i, n = 0;
    uint16_t *u16;
    (void)argc; (void)argv;
    if (!t)
        return JS_EXCEPTION;
    u16 = dyn_txt_u16_of(ctx, t, &units);
    if (!u16)
        return JS_EXCEPTION;
    for (i = 0; i < units; i++, n++)
        if (u16[i] >= 0xD800 && u16[i] <= 0xDBFF && i + 1 < units &&
            u16[i + 1] >= 0xDC00 && u16[i + 1] <= 0xDFFF)
            i++;
    free(u16);
    return JS_NewInt64(ctx, (int64_t)n);
}
DYN_TXT_FORWARD(dyn_txt_m_count_utf8, dyn_text_count_utf8, 0)
DYN_TXT_FORWARD(dyn_txt_m_latin1_to_utf8, dyn_text_latin1_to_utf8, 0)
DYN_TXT_FORWARD(dyn_txt_m_utf8_to_latin1, dyn_text_utf8_to_latin1, 0)
/* The two transcoders the first version of this class left off. Same defect as
 * Bytes shipping 18 of 36 accessors: a partial surface reads as complete. */
DYN_TXT_FORWARD(dyn_txt_m_utf8_to_utf16, dyn_text_utf8_to_utf16, 0)

static JSValue dyn_txt_get(JSContext *ctx, JSValueConst this_val, int magic)
{
    dyn_txt_t *t = dyn_txt_of(ctx, this_val);
    if (!t)
        return JS_EXCEPTION;
    if (magic == 0)
        return JS_NewBool(ctx, t->is_wide);
    return JS_DupValue(ctx, t->str);
}

static JSValue dyn_txt_to_string(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    dyn_txt_t *t = dyn_txt_of(ctx, this_val);
    (void)argc; (void)argv;
    if (!t)
        return JS_EXCEPTION;
    return JS_DupValue(ctx, t->str);
}

/* The string's own UTF-8 encoding.
   THIS FORWARDED TO dyn_text_utf16_to_utf8 -- the same target as
   utf16ToUtf8() -- so it transcoded the string's storage as if that were
   UTF-16 bytes. Measured against TextEncoder: "ab" returned [230,137,161]
   instead of [97,98], and every odd-length string threw "byte length must be
   even". Nothing referenced it, which is why it shipped. */
static JSValue dyn_txt_m_to_utf8(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    dyn_txt_t *t = dyn_txt_of(ctx, this_val);
    size_t sl;
    const char *cs;
    JSValue u8;
    (void)argc; (void)argv;
    if (!t)
        return JS_EXCEPTION;
    cs = JS_ToCStringLen(ctx, &sl, t->str);
    if (!cs)
        return JS_EXCEPTION;
    u8 = dyn_bytes_new_u8array(ctx, (const uint8_t *)cs, sl);
    JS_FreeCString(ctx, cs);
    return u8;
}

static JSValue dyn_txt_to_bytes(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv)
{
    dyn_txt_t *t = dyn_txt_of(ctx, this_val);
    size_t sl;
    const char *cs;
    JSValue u8;
    (void)argc; (void)argv;
    if (!t)
        return JS_EXCEPTION;
    cs = JS_ToCStringLen(ctx, &sl, t->str);
    if (!cs)
        return JS_EXCEPTION;
    u8 = dyn_bytes_new_u8array(ctx, (const uint8_t *)cs, sl);
    JS_FreeCString(ctx, cs);
    return dyn_bh_wrap(ctx, u8);
}

static const JSCFunctionListEntry dyn_txt_proto[] = {
    JS_CGETSET_MAGIC_DEF("isWide", dyn_txt_get, NULL, 0),
    JS_CGETSET_MAGIC_DEF("value", dyn_txt_get, NULL, 1),
    JS_CFUNC_DEF("isValidUtf8", 0, dyn_txt_m_valid_utf8),
    JS_CFUNC_DEF("isValidUtf16", 0, dyn_txt_m_valid_utf16),
    JS_CFUNC_DEF("countUtf8", 0, dyn_txt_m_count_utf8),
    JS_CFUNC_DEF("countUtf16", 0, dyn_txt_m_count_utf16_str),
    JS_CFUNC_DEF("toUtf8", 0, dyn_txt_m_to_utf8),
    JS_CFUNC_DEF("latin1ToUtf8", 0, dyn_txt_m_latin1_to_utf8),
    JS_CFUNC_DEF("utf8ToLatin1", 0, dyn_txt_m_utf8_to_latin1),
    JS_CFUNC_DEF("utf8ToUtf16", 0, dyn_txt_m_utf8_to_utf16),
    /* On a Text this IS toUtf8: the instance holds a string, whose
   UTF-16 form encodes to exactly its UTF-8 form. */
    JS_CFUNC_DEF("utf16ToUtf8", 0, dyn_txt_m_to_utf8),
    JS_CFUNC_DEF("toBytes", 0, dyn_txt_to_bytes),
    JS_CFUNC_DEF("toString", 0, dyn_txt_to_string),
    JS_CFUNC_DEF("toJSON", 0, dyn_txt_to_string),
};

static const JSCFunctionListEntry dyn_text_funcs[] = {
    /* `count` and `indexOfAny` are NOT re-exported here. dyna:bytes already
     * has count() over bytes, and String.prototype.indexOfAny covers the
     * string case since W2.1 -- so both of dyna:text's spellings were the
     * duplication this fold exists to remove. The C is kept because Bytes
     * uses it. */
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
    JS_CFUNC_DEF("slice", 2, dyn_bh_slice),
    JS_CFUNC_DEF("compare", 1, dyn_bh_compare),
    JS_CFUNC_DEF("equals", 1, dyn_bh_equals),
    JS_CFUNC_DEF("indexOf", 2, dyn_bh_index_of),
    JS_CFUNC_DEF("lastIndexOf", 2, dyn_bh_last_index_of),
    JS_CFUNC_DEF("includes", 1, dyn_bh_includes),
    JS_CFUNC_DEF("count", 1, dyn_bh_count),
    JS_CFUNC_DEF("indexOfAny", 1, dyn_bh_index_of_any),
    JS_CFUNC_DEF("fill", 3, dyn_bh_fill),
    JS_CFUNC_DEF("toUtf8", 0, dyn_bh_to_utf8),
    JS_CFUNC_DEF("toString", 0, dyn_bh_to_string),
    /* ALL 36 accessors, not a hand-picked subset. The first version of this
     * class carried 18 of them, chosen by which ones came to mind -- so
     * readInt16LE and every 64-bit and big-endian accessor were simply absent
     * from the handle while present as free functions. A partial surface is
     * worse than none: it reads as complete and fails only on the one width a
     * caller happens to need. */
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

/* ---------- module registration ----------
 *
 * The `length` in every JS_C[FUNC_MAGIC]_DEF below MUST equal the highest
 * argv[] index the handler unconditionally reads, NOT the count of
 * "conceptually required" JS arguments -- js_call_c_function only pads
 * argv[] with JS_UNDEFINED up to the declared length when the CALLER passes
 * fewer arguments than that; if the caller passes as many or more, argv is
 * used as-is with only the caller's actual argc slots valid, so reading a
 * higher index is an out-of-bounds read of whatever happens to be on the VM
 * stack next. copy/fill take optional trailing args (dstOff/srcOff/len,
 * start/end) accessed unconditionally, so they declare 5/4, not 2. */
/* Legacy single-byte charsets: a charset is a reading of bytes (design 11). */
#include "dyna-iconv.inc.c"

static const JSCFunctionListEntry dyn_bytes_funcs[] = {
    JS_CFUNC_DEF("decode", 2, dyn_iconv_decode),
    JS_CFUNC_DEF("encode", 2, dyn_iconv_encode),
    JS_CFUNC_DEF("encodingExists", 1, dyn_iconv_exists),
    JS_CFUNC_DEF("encodings", 0, dyn_iconv_list),
    JS_CFUNC_DEF("bytesOf", 1, dyn_bytes_bytes_of),
    JS_CFUNC_DEF("compare", 2, dyn_bytes_compare),
    JS_CFUNC_DEF("equal", 2, dyn_bytes_equal),
    JS_CFUNC_DEF("indexOf", 2, dyn_bytes_index_of),
    JS_CFUNC_DEF("lastIndexOf", 2, dyn_bytes_last_index_of),
    JS_CFUNC_DEF("contains", 2, dyn_bytes_contains),
    JS_CFUNC_DEF("count", 2, dyn_bytes_count),
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

static int dyn_bytes_register(JSContext *ctx, JSModuleDef *m)
{
    JSRuntime *rt = JS_GetRuntime(ctx);
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
    JS_SetPropertyStr(ctx, ctor, "alloc",
        JS_NewCFunctionMagic(ctx, dyn_bh_static, "alloc", 1, JS_CFUNC_generic_magic, 0));
    JS_SetPropertyStr(ctx, ctor, "isBytes",
        JS_NewCFunctionMagic(ctx, dyn_bh_static, "isBytes", 1, JS_CFUNC_generic_magic, 1));
    JS_SetPropertyStr(ctx, ctor, "concat",
        JS_NewCFunctionMagic(ctx, dyn_bh_static, "concat", 1, JS_CFUNC_generic_magic, 2));
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

static int dyn_bytes_init_module(JSContext *ctx, JSModuleDef *m)
{
    if (dyn_bytes_register(ctx, m) < 0)
        return -1;
    if (JS_SetModuleExportList(ctx, m, dyn_text_funcs,
                               countof(dyn_text_funcs)) < 0)
        return -1;
    return JS_SetModuleExportList(ctx, m, dyn_bytes_funcs,
                                  countof(dyn_bytes_funcs));
}

int js_nat_init_bytes(JSContext *ctx)
{
    JSModuleDef *m;
    simd_init(); /* idempotent (pthread_once): select the best strfind/base64 kernels */
    m = JS_NewCModule(ctx, "dyna:bytes", dyn_bytes_init_module);
    if (!m)
        return -1;
    if (JS_AddModuleExport(ctx, m, "Bytes") < 0 ||
        JS_AddModuleExport(ctx, m, "Text") < 0)
        return -1;
    if (JS_AddModuleExportList(ctx, m, dyn_text_funcs,
                               countof(dyn_text_funcs)) < 0)
        return -1;
    return JS_AddModuleExportList(ctx, m, dyn_bytes_funcs,
                                  countof(dyn_bytes_funcs));
}

#endif /* CONFIG_NATIVE_MODULES && CONFIG_NATIVE_MODULE_BYTES */
