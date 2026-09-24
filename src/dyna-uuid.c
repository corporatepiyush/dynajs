/*
 * dyna:uuid -- UUID v3/v4/v5/v7, parsing, and version/variant inspection.
 *
 * Random versions draw from the OS CSPRNG (core/dyn-prng.c) directly, never
 * from Math.random. v7 embeds a millisecond timestamp plus the RFC 9562
 * same-millisecond counter, so it is monotonic within a process; v3/v5 are
 * deterministic hashes of (namespace, name).
 * Full API: see the dyna:uuid section of API.md (and dynajs.d.ts).
 */
#include "dyna-nat.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_UUID)

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <stdatomic.h>
#include <pthread.h>

/* the shared pure-C digest library (src/core/dyn-hash.c): v3 = MD5, v5 = SHA-1 */
#include "core/dyn-hash.h"
/* the shared OS entropy source (src/core/dyn-prng.c): every version and ID
 * generator here draws from it -- a private copy would be a second thing to
 * audit and a way for failure semantics to drift. */
#include "core/dyn-prng.h"

#ifndef countof
#define countof(x) (sizeof(x) / sizeof((x)[0]))
#endif

/* Canonical hyphenated string is exactly 36 chars (no NUL stored/needed). */
#define DYN_UUID_STRLEN 36

/* ---------- hex + canonical formatting ---------- */

static const char dyn_uuid_hex[] = "0123456789abcdef";

static int dyn_uuid_hexval(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Render 16 bytes as lowercase 8-4-4-4-12 into out[36] (no terminator). */
static void dyn_uuid_format(const uint8_t b[16], char out[DYN_UUID_STRLEN])
{
    int i, j = 0;
    for (i = 0; i < 16; i++) {
        if (i == 4 || i == 6 || i == 8 || i == 10)
            out[j++] = '-';
        out[j++] = dyn_uuid_hex[b[i] >> 4];
        out[j++] = dyn_uuid_hex[b[i] & 0x0f];
    }
}

/* ---------- parsing ---------- */

/* ASCII case-insensitive compare of the first n bytes; 0 iff equal. */
static int dyn_uuid_ci_ne(const char *a, const char *b, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) {
        int ca = (unsigned char)a[i], cb = (unsigned char)b[i];
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb)
            return 1;
    }
    return 0;
}

static int dyn_uuid_hexpair(const char *p, uint8_t *out)
{
    int hi = dyn_uuid_hexval((unsigned char)p[0]);
    int lo = dyn_uuid_hexval((unsigned char)p[1]);
    if (hi < 0 || lo < 0)
        return -1;
    *out = (uint8_t)((hi << 4) | lo);
    return 0;
}

/* Parse a 36-char canonical "8-4-4-4-12" span at `s` into b[16]. */
static int dyn_uuid_parse_canonical(const char *s, uint8_t b[16])
{
    static const int pos[16] = {
        0, 2, 4, 6, 9, 11, 14, 16, 19, 21, 24, 26, 28, 30, 32, 34
    };
    int i;
    if (s[8] != '-' || s[13] != '-' || s[18] != '-' || s[23] != '-')
        return -1;
    for (i = 0; i < 16; i++)
        if (dyn_uuid_hexpair(s + pos[i], &b[i]))
            return -1;
    return 0;
}

/* Parse any of the four accepted forms into b[16]. 0 on success, -1 if
 * malformed. `len` is the exact byte length (embedded NUL safe). */
static int dyn_uuid_parse_bytes(const char *s, size_t len, uint8_t b[16])
{
    int i;
    switch (len) {
    case 36:
        return dyn_uuid_parse_canonical(s, b);
    case 45: /* urn:uuid:xxxxxxxx-... */
        if (dyn_uuid_ci_ne(s, "urn:uuid:", 9))
            return -1;
        return dyn_uuid_parse_canonical(s + 9, b);
    case 38: /* {xxxxxxxx-...} */
        if (s[0] != '{' || s[37] != '}')
            return -1;
        return dyn_uuid_parse_canonical(s + 1, b);
    case 32: /* raw hex, no hyphens */
        for (i = 0; i < 16; i++)
            if (dyn_uuid_hexpair(s + i * 2, &b[i]))
                return -1;
        return 0;
    default:
        return -1;
    }
}

static const char *dyn_uuid_variant_name(uint8_t b8)
{
    if ((b8 & 0xc0) == 0x80) return "RFC4122";
    if ((b8 & 0xe0) == 0xc0) return "Microsoft";
    if ((b8 & 0xe0) == 0xe0) return "Future";
    return "NCS";
}

/* ---------- byte-view boundary (Uint8Array/Int8Array/Uint8ClampedArray or a
 * plain ArrayBuffer -> raw pointer + length), same shape as dyna:encoding's
 * dyn_enc_view. Pure structural query, never invokes JS. ---------- */
static int dyn_uuid_view(JSContext *ctx, JSValueConst v, uint8_t **pp, size_t *pn)
{
    JSValue buf;
    uint8_t *base;
    size_t off, len, bpe, ab;

    base = JS_GetArrayBuffer(ctx, &ab, v);
    if (base) {
        *pp = base;
        *pn = ab;
        return 0;
    }
    JS_FreeValue(ctx, JS_GetException(ctx)); /* clear: not a plain ArrayBuffer */

    buf = JS_GetTypedArrayBuffer(ctx, v, &off, &len, &bpe);
    if (JS_IsException(buf))
        return -1; /* not a TypedArray either; propagate that TypeError */
    if (bpe != 1) {
        JS_FreeValue(ctx, buf);
        JS_ThrowTypeError(ctx, "expected a Uint8Array or ArrayBuffer");
        return -1;
    }
    base = JS_GetArrayBuffer(ctx, &ab, buf);
    JS_FreeValue(ctx, buf);
    if (!base)
        return -1; /* detached mid-resolve; JS_GetArrayBuffer already threw */
    if (off > ab || len > ab - off) {
        JS_ThrowRangeError(ctx, "typed array out of bounds");
        return -1;
    }
    *pp = base + off;
    *pn = len;
    return 0;
}

/* Build a fresh Uint8Array copying 16 bytes (never aliases a native pointer). */
static JSValue dyn_uuid_new_u8array(JSContext *ctx, const uint8_t *data, size_t len)
{
    JSValue ab, out;
    JSValueConst ta_args[3];

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

/* MD5 and SHA-1 come from the shared pure-C digest library: dyna:uuid needs
 * them only to derive v3/v5 names, and a second copy is a second thing to audit.
 * dyn_md5/dyn_sha1 have exactly the signatures this file used. */

/* ---------- strict options bags (fixes the site) ---------- */

/* v4() used to ignore its arguments entirely: v4({as: "bytes"}) silently
 * returned a string. Options are now checked STRICTLY -- an unknown key is a
 * TypeError, not a silent ignore -- via this shared checker. */
static int dyn_uuid_opts_check(JSContext *ctx, JSValueConst opts,
                               const char *const *allowed, size_t n_allowed,
                               const char *who)
{
    JSPropertyEnum *tab = NULL;
    uint32_t len = 0, i;
    size_t j;

    if (JS_IsUndefined(opts))
        return 0;                     /* absent / undefined: all defaults */
    if (!JS_IsObject(opts)) {
        JS_ThrowTypeError(ctx, "%s: opts must be an object", who);
        return -1;
    }
    if (JS_GetOwnPropertyNames(ctx, &tab, &len, opts,
                               JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY))
        return -1;
    for (i = 0; i < len; i++) {
        /* non-ASCII keys cannot match an allowed name; borrow the atom's
         * bytes when they are plain ASCII to build a precise message */
        size_t name_len = 0;
        const char *name = JS_AtomBorrowASCII(ctx, &name_len, tab[i].atom);
        int ok = 0;

        if (name) {
            for (j = 0; j < n_allowed; j++) {
                if (strcmp(name, allowed[j]) == 0) {
                    ok = 1;
                    break;
                }
            }
        }
        if (!ok) {
            const char *shown = JS_AtomToCString(ctx, tab[i].atom);
            JS_ThrowTypeError(ctx, "%s: unknown option \"%s\"", who,
                              shown ? shown : "?");
            if (shown)
                JS_FreeCString(ctx, shown);
            JS_FreePropertyEnum(ctx, tab, len);
            return -1;
        }
    }
    JS_FreePropertyEnum(ctx, tab, len);
    return 0;
}

/* The shared {as: "string" | "bytes"} resolution for v4/v7. Returns 1 for
 * bytes, 0 for string (the default), -1 on a throwing rejection. */
static int dyn_uuid_opt_as(JSContext *ctx, JSValueConst opts, const char *who)
{
    JSValue as_val;
    int out = 0;

    if (!(JS_IsObject(opts)))
        return 0;                     /* no bag: default (opts_check passed) */
    as_val = JS_GetPropertyStr(ctx, opts, "as");
    if (JS_IsException(as_val))
        return -1;
    if (JS_IsString(as_val)) {
        size_t n;
        const char *a = JS_ToCStringLen(ctx, &n, as_val);
        if (!a) {
            JS_FreeValue(ctx, as_val);
            return -1;
        }
        if (strcmp(a, "bytes") == 0)
            out = 1;
        else if (strcmp(a, "string") != 0) {
            JS_ThrowTypeError(ctx, "%s: as must be \"string\" or \"bytes\"", who);
            out = -1;
        }
        JS_FreeCString(ctx, a);
    } else if (!JS_IsUndefined(as_val)) {
        JS_ThrowTypeError(ctx, "%s: as must be \"string\" or \"bytes\"", who);
        out = -1;
    }
    JS_FreeValue(ctx, as_val);
    return out;
}

/* ---------- generation: v4 / v7 ---------- */

static JSValue dyn_uuid_v4(JSContext *ctx, JSValueConst this_val,
                           int argc, JSValueConst *argv)
{
    static const char *const allowed[] = { "as" };
    uint8_t b[16];
    char s[DYN_UUID_STRLEN];
    int as_bytes;

    (void)this_val;
    if (dyn_uuid_opts_check(ctx, argc > 0 ? argv[0] : JS_UNDEFINED,
                            allowed, 1, "v4(opts)"))
        return JS_EXCEPTION;
    as_bytes = dyn_uuid_opt_as(ctx, argc > 0 ? argv[0] : JS_UNDEFINED,
                               "v4(opts)");
    if (as_bytes < 0)
        return JS_EXCEPTION;

    if (dyn_os_entropy(b, sizeof(b)) < 0)
        return JS_ThrowInternalError(ctx, "dyna:uuid: OS entropy unavailable");
    b[6] = (uint8_t)((b[6] & 0x0f) | 0x40); /* version 4 */
    b[8] = (uint8_t)((b[8] & 0x3f) | 0x80); /* variant 10xx */
    if (as_bytes)
        return dyn_uuid_new_u8array(ctx, b, 16);
    dyn_uuid_format(b, s);
    return JS_NewStringLen(ctx, s, DYN_UUID_STRLEN);
}

static JSValue dyn_uuid_v7(JSContext *ctx, JSValueConst this_val,
                           int argc, JSValueConst *argv)
{
    static const char *const allowed[] = { "as" };
    uint8_t b[16];
    char s[DYN_UUID_STRLEN];
    struct timespec ts;
    uint64_t ms, ctr;
    int as_bytes;

    (void)this_val;
    if (dyn_uuid_opts_check(ctx, argc > 0 ? argv[0] : JS_UNDEFINED,
                            allowed, 1, "v7(opts)"))
        return JS_EXCEPTION;
    as_bytes = dyn_uuid_opt_as(ctx, argc > 0 ? argv[0] : JS_UNDEFINED,
                               "v7(opts)");
    if (as_bytes < 0)
        return JS_EXCEPTION;

    if (clock_gettime(CLOCK_REALTIME, &ts) != 0)
        return JS_ThrowInternalError(ctx, "dyna:uuid: clock_gettime failed");
    ms = (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;

    /* Fail closed BEFORE any state changes: a failed entropy source produces
     * no id. rand_b (62 bits) comes from these bytes, and so does rand_a's
     * fresh-per-millisecond seed (12 bits) -- both are placed below. */
    if (dyn_os_entropy(b, sizeof(b)) < 0)
        return JS_ThrowInternalError(ctx, "dyna:uuid: OS entropy unavailable");

    /* Monotonicity, RFC 9562: one atomic word packs the millisecond and
     * a 12-bit rand_a counter as (ms << 12) | ctr. A new millisecond seeds
     * ctr from fresh entropy; each further call within the same millisecond
     * does ctr += 1, so ids generated in the same ms sort after one another
     * instead of ordering randomly on 74 fresh random bits. On ctr rollover
     * the millisecond advances by one -- the RFC's fixed-length-counter
     * method -- so the ordering never stalls or spins.
     *
     * CLOCK_REALTIME can go BACKWARDS -- an NTP step, a VM restore, an
     * administrator setting the clock. A wall clock below the packed floor
     * holds the previous millisecond and keeps counting, exactly the clamp
     * the old ms-only floor gave (RFC 9562 allows reusing the timestamp).
     *
     * Atomic because ids are generated from any JS thread. CAS rather than a
     * store so two racing threads serialize on one word: the loser retries
     * against the winner's value and every winner takes a distinct (ms, ctr).
     */
    {
        static _Atomic uint64_t v7_mono;
        uint64_t cur = atomic_load_explicit(&v7_mono, memory_order_relaxed);
        uint64_t fresh = ((((uint64_t)b[0] & 0xF0) << 4) | b[1]) & 0xFFF;
        uint64_t next;
        for (;;) {
            uint64_t cur_ms = cur >> 12;
            if (ms < cur_ms)
                ms = cur_ms;                      /* clock went back: hold */
            if (cur_ms == ms) {
                if ((cur & 0xFFF) == 0xFFF) {     /* counter full: bump the ms */
                    ms += 1;
                    next = (ms << 12) | fresh;
                } else {
                    next = cur + 1;               /* same ms: +1 per */
                }
            } else {
                next = (ms << 12) | fresh;        /* new ms: re-randomize */
            }
            if (atomic_compare_exchange_weak_explicit(
                    &v7_mono, &cur, next,
                    memory_order_relaxed, memory_order_relaxed))
                break;
            /* cur was reloaded by the CAS; retry against the new value. */
        }
        ms = next >> 12;
        ctr = next & 0xFFF;
    }

    b[0] = (uint8_t)(ms >> 40);
    b[1] = (uint8_t)(ms >> 32);
    b[2] = (uint8_t)(ms >> 24);
    b[3] = (uint8_t)(ms >> 16);
    b[4] = (uint8_t)(ms >> 8);
    b[5] = (uint8_t)ms;
    b[6] = (uint8_t)((ctr >> 8) | 0x70); /* version 7 | rand_a high nibble */
    b[7] = (uint8_t)ctr;                 /* rand_a low byte */
    b[8] = (uint8_t)((b[8] & 0x3f) | 0x80); /* variant 10xx */
    if (as_bytes)
        return dyn_uuid_new_u8array(ctx, b, 16);
    dyn_uuid_format(b, s);
    return JS_NewStringLen(ctx, s, DYN_UUID_STRLEN);
}

/* ----------: compare (v7-sortable byte order) + v8 (RFC 9562 custom) ---- */

/* compare(a, b): -1 | 0 | 1 comparing the CANONICAL 16-byte forms -- exactly
 * memcmp on bytes(parse(a)) vs bytes(parse(b)), which is the sort order v7
 * ids already have (big-endian ms then counter then randomness). All four
 * accepted string forms parse to the same bytes, so the comparison is form-
 * agnostic. Malformed or non-string input is a TypeError. */
static JSValue dyn_uuid_compare(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv)
{
    const char *sa, *sb;
    size_t la, lb;
    uint8_t ba[16], bb[16];
    int r;

    (void)this_val;
    if (argc < 2 || !JS_IsString(argv[0]) || !JS_IsString(argv[1]))
        return JS_ThrowTypeError(ctx,
            "compare(a, b): two UUID strings are required");
    sa = JS_ToCStringLen(ctx, &la, argv[0]);
    if (!sa)
        return JS_EXCEPTION;
    sb = JS_ToCStringLen(ctx, &lb, argv[1]);
    if (!sb) {
        JS_FreeCString(ctx, sa);
        return JS_EXCEPTION;
    }
    if (dyn_uuid_parse_bytes(sa, la, ba) || dyn_uuid_parse_bytes(sb, lb, bb)) {
        JS_FreeCString(ctx, sa);
        JS_FreeCString(ctx, sb);
        return JS_ThrowTypeError(ctx, "compare(a, b): invalid UUID string");
    }
    JS_FreeCString(ctx, sa);
    JS_FreeCString(ctx, sb);
    r = memcmp(ba, bb, 16);
    return JS_NewInt32(ctx, (r < 0) ? -1 : (r > 0));
}

/* v8({fill}): RFC 9562 section 5.8 custom. The caller supplies ALL 16 bytes;
 * this function overwrites EXACTLY twelve bits and nothing else:
 *   - byte 6 high nibble <- 1000  (version 8)
 *   - byte 8 top two bits <- 10   (RFC 4122 variant)
 * so the caller's low nibble of byte 6 and low six bits of byte 8 survive
 * (csleep fields per the RFC). The input view is copied, never mutated; a
 * wrong-length fill is a RangeError, a non-byte-view fill a TypeError. */
static JSValue dyn_uuid_v8(JSContext *ctx, JSValueConst this_val,
                           int argc, JSValueConst *argv)
{
    static const char *const allowed[] = { "fill" };
    uint8_t b[16];
    uint8_t *p;
    size_t n;
    JSValue fv;
    char s[DYN_UUID_STRLEN];

    (void)this_val;
    if (argc < 1 || JS_IsUndefined(argv[0]))
        return JS_ThrowTypeError(ctx,
            "v8(opts): opts with {fill} is required");
    if (dyn_uuid_opts_check(ctx, argv[0], allowed, 1, "v8(opts)"))
        return JS_EXCEPTION;
    fv = JS_GetPropertyStr(ctx, argv[0], "fill");
    if (JS_IsException(fv))
        return JS_EXCEPTION;
    if (JS_IsUndefined(fv)) {
        JS_FreeValue(ctx, fv);
        return JS_ThrowTypeError(ctx, "v8(opts): fill is required");
    }
    if (dyn_uuid_view(ctx, fv, &p, &n)) {
        JS_FreeValue(ctx, fv);
        return JS_EXCEPTION;      /* not a byte view (TypeError) */
    }
    if (n != 16) {
        JS_FreeValue(ctx, fv);
        return JS_ThrowRangeError(ctx,
            "v8(opts): fill must be exactly 16 bytes");
    }
    memcpy(b, p, 16);
    JS_FreeValue(ctx, fv);
    b[6] = (uint8_t)((b[6] & 0x0f) | 0x80); /* version 8 */
    b[8] = (uint8_t)((b[8] & 0x3f) | 0x80); /* variant 10xx */
    dyn_uuid_format(b, s);
    return JS_NewStringLen(ctx, s, DYN_UUID_STRLEN);
}

/* ---------- generation: v3 (MD5) / v5 (SHA-1), name-based ---------- */

/* Resolve a namespace argument: a UUID string (any parse form) or a 16-byte
 * view -> ns[16]. Returns 0 or -1 (throwing). */
static int dyn_uuid_namespace(JSContext *ctx, JSValueConst v, uint8_t ns[16])
{
    if (JS_IsString(v)) {
        size_t len;
        const char *s = JS_ToCStringLen(ctx, &len, v);
        int r;
        if (!s)
            return -1;
        r = dyn_uuid_parse_bytes(s, len, ns);
        JS_FreeCString(ctx, s);
        if (r) {
            JS_ThrowSyntaxError(ctx, "v3/v5: invalid namespace UUID");
            return -1;
        }
        return 0;
    }
    {
        uint8_t *p;
        size_t n;
        if (dyn_uuid_view(ctx, v, &p, &n))
            return -1;
        if (n != 16) {
            JS_ThrowTypeError(ctx, "v3/v5: namespace must be a UUID string or 16-byte view");
            return -1;
        }
        memcpy(ns, p, 16);
        return 0;
    }
}

/* magic: version (3 = MD5, 5 = SHA-1). */
static JSValue dyn_uuid_named(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv, int magic)
{
    uint8_t ns[16], b[16], digest[20];
    const uint8_t *name = NULL;
    const char *owned = NULL;
    size_t namelen = 0;
    uint8_t *msg;
    char s[DYN_UUID_STRLEN];
    (void)this_val; (void)argc;

    /* Coerce both args to C locals FIRST. */
    if (dyn_uuid_namespace(ctx, argv[0], ns))
        return JS_EXCEPTION;
    if (JS_IsString(argv[1])) {
        owned = JS_ToCStringLen(ctx, &namelen, argv[1]);
        if (!owned)
            return JS_EXCEPTION;
        name = (const uint8_t *)owned;
    } else {
        uint8_t *p;
        size_t n;
        if (dyn_uuid_view(ctx, argv[1], &p, &n))
            return JS_EXCEPTION;
        name = p;
        namelen = n;
    }

    msg = (uint8_t *)malloc(16 + namelen);
    if (!msg) {
        if (owned)
            JS_FreeCString(ctx, owned);
        return JS_ThrowOutOfMemory(ctx);
    }
    memcpy(msg, ns, 16);
    if (namelen)
        memcpy(msg + 16, name, namelen);
    if (magic == 5) {
        dyn_sha1(msg, 16 + namelen, digest);
        memcpy(b, digest, 16);
    } else {
        dyn_md5(msg, 16 + namelen, b);
    }
    free(msg);
    if (owned)
        JS_FreeCString(ctx, owned);

    b[6] = (uint8_t)((b[6] & 0x0f) | (magic << 4)); /* version 3 or 5 */
    b[8] = (uint8_t)((b[8] & 0x3f) | 0x80);         /* variant 10xx */
    dyn_uuid_format(b, s);
    return JS_NewStringLen(ctx, s, DYN_UUID_STRLEN);
}

/* ---------- parse / validate / version / variant / bytes / fromBytes ------- */

static JSValue dyn_uuid_parse(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv)
{
    const char *str;
    size_t len;
    uint8_t b[16];
    char out[DYN_UUID_STRLEN];
    int r;
    (void)this_val; (void)argc;

    str = JS_ToCStringLen(ctx, &len, argv[0]);
    if (!str)
        return JS_EXCEPTION;
    r = dyn_uuid_parse_bytes(str, len, b);
    JS_FreeCString(ctx, str);
    if (r)
        return JS_ThrowSyntaxError(ctx, "parse: invalid UUID string");
    dyn_uuid_format(b, out);
    return JS_NewStringLen(ctx, out, DYN_UUID_STRLEN);
}

static JSValue dyn_uuid_validate(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    const char *str;
    size_t len;
    uint8_t b[16];
    int ok;
    (void)this_val; (void)argc;

    /* A non-string is simply not a UUID -> false (never throws). */
    if (!JS_IsString(argv[0]))
        return JS_FALSE;
    str = JS_ToCStringLen(ctx, &len, argv[0]);
    if (!str)
        return JS_EXCEPTION;
    ok = (dyn_uuid_parse_bytes(str, len, b) == 0);
    JS_FreeCString(ctx, str);
    return JS_NewBool(ctx, ok);
}

static JSValue dyn_uuid_version(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv)
{
    const char *str;
    size_t len;
    uint8_t b[16];
    int r;
    (void)this_val; (void)argc;

    str = JS_ToCStringLen(ctx, &len, argv[0]);
    if (!str)
        return JS_EXCEPTION;
    r = dyn_uuid_parse_bytes(str, len, b);
    JS_FreeCString(ctx, str);
    if (r)
        return JS_ThrowSyntaxError(ctx, "version: invalid UUID string");
    return JS_NewInt32(ctx, b[6] >> 4);
}

static JSValue dyn_uuid_variant(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv)
{
    const char *str;
    size_t len;
    uint8_t b[16];
    int r;
    (void)this_val; (void)argc;

    str = JS_ToCStringLen(ctx, &len, argv[0]);
    if (!str)
        return JS_EXCEPTION;
    r = dyn_uuid_parse_bytes(str, len, b);
    JS_FreeCString(ctx, str);
    if (r)
        return JS_ThrowSyntaxError(ctx, "variant: invalid UUID string");
    return JS_NewString(ctx, dyn_uuid_variant_name(b[8]));
}

static JSValue dyn_uuid_bytes(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv)
{
    const char *str;
    size_t len;
    uint8_t b[16];
    int r;
    (void)this_val; (void)argc;

    str = JS_ToCStringLen(ctx, &len, argv[0]);
    if (!str)
        return JS_EXCEPTION;
    r = dyn_uuid_parse_bytes(str, len, b);
    JS_FreeCString(ctx, str);
    if (r)
        return JS_ThrowSyntaxError(ctx, "bytes: invalid UUID string");
    return dyn_uuid_new_u8array(ctx, b, 16);
}

static JSValue dyn_uuid_from_bytes(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv)
{
    uint8_t *p;
    size_t n;
    char out[DYN_UUID_STRLEN];
    (void)this_val; (void)argc;

    if (dyn_uuid_view(ctx, argv[0], &p, &n))
        return JS_EXCEPTION;
    if (n != 16)
        return JS_ThrowRangeError(ctx, "fromBytes: expected exactly 16 bytes");
    dyn_uuid_format(p, out);
    return JS_NewStringLen(ctx, out, DYN_UUID_STRLEN);
}

/* ---------- module registration ---------- */

/* NanoID / ULID: different specs, same entropy source and the same question. */
#include "dyna-nanoid.inc.c"

static const JSCFunctionListEntry dyn_uuid_funcs[] = {
    JS_CFUNC_DEF("NanoID", 0, dyn_nanoid),
    JS_CFUNC_DEF("NanoIDAlphabet", 1, dyn_nanoid_alphabet),
    JS_CFUNC_DEF("ULID", 2, dyn_ulid),
    JS_CFUNC_DEF("ULIDTime", 1, dyn_ulid_time),
    JS_CFUNC_DEF("v4", 1, dyn_uuid_v4),
    JS_CFUNC_DEF("v7", 1, dyn_uuid_v7),
    JS_CFUNC_DEF("v8", 1, dyn_uuid_v8),
    JS_CFUNC_DEF("compare", 2, dyn_uuid_compare),
    JS_CFUNC_MAGIC_DEF("v3", 2, dyn_uuid_named, 3),
    JS_CFUNC_MAGIC_DEF("v5", 2, dyn_uuid_named, 5),
    JS_CFUNC_DEF("parse", 1, dyn_uuid_parse),
    JS_CFUNC_DEF("validate", 1, dyn_uuid_validate),
    JS_CFUNC_DEF("version", 1, dyn_uuid_version),
    JS_CFUNC_DEF("variant", 1, dyn_uuid_variant),
    JS_CFUNC_DEF("bytes", 1, dyn_uuid_bytes),
    JS_CFUNC_DEF("fromBytes", 1, dyn_uuid_from_bytes),
    /* All-zero (NIL) and all-ones (MAX) UUIDs, canonical lowercase. */
    JS_PROP_STRING_DEF("NIL", "00000000-0000-0000-0000-000000000000", 0),
    JS_PROP_STRING_DEF("MAX", "ffffffff-ffff-ffff-ffff-ffffffffffff", 0),
    /* Predefined namespace UUIDs (RFC 9562 sec.6.6 / RFC 4122 appendix C). */
    JS_PROP_STRING_DEF("NAMESPACE_DNS",  "6ba7b810-9dad-11d1-80b4-00c04fd430c8", 0),
    JS_PROP_STRING_DEF("NAMESPACE_URL",  "6ba7b811-9dad-11d1-80b4-00c04fd430c8", 0),
    JS_PROP_STRING_DEF("NAMESPACE_OID",  "6ba7b812-9dad-11d1-80b4-00c04fd430c8", 0),
    JS_PROP_STRING_DEF("NAMESPACE_X500", "6ba7b814-9dad-11d1-80b4-00c04fd430c8", 0),
};

static int dyn_uuid_init_module(JSContext *ctx, JSModuleDef *m)
{
    return JS_SetModuleExportList(ctx, m, dyn_uuid_funcs, countof(dyn_uuid_funcs));
}

int js_nat_init_uuid(JSContext *ctx)
{
    JSModuleDef *m = JS_NewCModule(ctx, "dyna:uuid", dyn_uuid_init_module);
    if (!m)
        return -1;
    return JS_AddModuleExportList(ctx, m, dyn_uuid_funcs, countof(dyn_uuid_funcs));
}

#endif /* CONFIG_NATIVE_MODULES && CONFIG_NATIVE_MODULE_UUID */
