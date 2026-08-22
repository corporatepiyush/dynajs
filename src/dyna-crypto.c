/*
 * Two modules, one file, split by whether an operation DEPENDS ON A SECRET:
 *
 *   dyna:hash    unkeyed reductions of public input -- MD5, SHA-1,
 *                SHA-224/256/384/512, CRC32/CRC32C, XXHash32/64, Hasher.
 *   dyna:crypto  secret-dependent and constant-time -- HMAC/HMACHex and the
 *                keyed Hmac capability, HKDF, PBKDF2, RandomBytes,
 *                TimingSafeEqual.
 *
 * That is why HMAC is not in dyna:hash: it takes a key. There is no public-key
 * or AEAD primitive here; the export lists at the bottom are the whole surface.
 *
 * EXPORTED FUNCTIONS are capitalised (SHA256Hex, Base64Encode); class INSTANCE
 * METHODS stay camelCase (h.digestHex()). Algorithm names passed as arguments
 * are lowercase and matched with strcmp -- new Hasher("sha256"), not "SHA256".
 *
 * Every method coerces ALL JS arguments to C locals BEFORE resolving the
 * native handle: coercion runs arbitrary JS (toString/valueOf/Proxy) that can
 * call this.close(), and resolving first would be a use-after-free. This
 * module is entirely ToString-based, so the hook to test with is toString().
 * Full API: see the dyna:* module in dyna-libc.h.
 */
#include "dyna-nat.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_CRYPTO)

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* the extracted pure-C digest library (src/core/dyn-hash.c) */
#include "core/dyn-hash.h"
#include "core/dyn-codec.h"   /* base64url for JWT */
#include "core/dyn-prng.h"   /* dyn_os_entropy: randomBytes */

#ifndef countof
#define countof(x) (sizeof(x) / sizeof((x)[0]))
#endif

/* PBKDF2 DoS bound: total HMAC compressions = iters * ceil(length/hashLen).
 * See the product check in dyn_crypto_kdf for the arithmetic. */
#define DYN_PBKDF2_MAX_COMPRESSIONS (1 << 24)

/* ==================================================================== *
 *  JS boundary: argument coercion + result builders                     *
 * ==================================================================== */

/* Coerce a data/key argument to a byte range. A string is materialised to an
 * OWNED UTF-8 buffer (*powned, release with JS_FreeCString); an ArrayBuffer or
 * TypedArray/DataView yields a zero-copy backing pointer (valid only for the
 * synchronous remainder of the call); anything else is coerced to a string via
 * ToString (which may run user JS -- callers relying on reentrancy safety must
 * therefore call this BEFORE resolving any native handle or buffer pointer that
 * must survive). Returns 0 (exactly one of *powned/borrowed pointer is set) or
 * -1 with a pending exception. */
static int dyn_crypto_data(JSContext *ctx, JSValueConst v, const uint8_t **pdata,
                           size_t *plen, const char **powned)
{
    *powned = NULL;
    if (JS_IsString(v)) {
        size_t n;
        const char *s = JS_ToCStringLen(ctx, &n, v);
        if (!s)
            return -1;
        *powned = s;
        *pdata = (const uint8_t *)s;
        *plen = n;
        return 0;
    }
    /* THE TYPED-ARRAY PATH IS TRIED FIRST (below), and the ArrayBuffer probe
     * that used to sit here has moved after it. JS_GetArrayBuffer THROWS for a
     * typed array -- allocating an Error this function then discards -- so
     * probing the buffer case first charged a thrown-and-swallowed exception
     * to the argument type callers overwhelmingly pass. Measured: sha256Hex of
     * 1 KiB was 1.23x slower with a Uint8Array than with an ArrayBuffer, and
     * crc32 1.42x. */
    {
        size_t off, len, bpe, ab_size;
        uint8_t *base;
        /* GetArrayBufferView, not GetTypedArrayBuffer: the latter rejects a
           DataView, which then fell through to the string path and hashed the
           literal "[object DataView]" -- a wrong digest that looks like a
           digest, on every function in this module. The header has always
           claimed DataView is accepted. */
        JSValue ab = JS_GetArrayBufferView(ctx, v, &off, &len, &bpe);
        if (!JS_IsException(ab)) {
            base = JS_GetArrayBuffer(ctx, &ab_size, ab);
            JS_FreeValue(ctx, ab);
            if (!base)
                return -1; /* detached mid-resolve; already threw */
            if (off > ab_size || len > ab_size - off) {
                JS_ThrowRangeError(ctx, "typed array out of bounds");
                return -1;
            }
            *pdata = base + off;
            *plen = len;
            return 0;
        }
        JS_FreeValue(ctx, JS_GetException(ctx)); /* not a view: try a buffer */
        {
            size_t n;
            uint8_t *p = JS_GetArrayBuffer(ctx, &n, v);
            if (p) {
                *pdata = p;
                *plen = n;
                return 0;
            }
            JS_FreeValue(ctx, JS_GetException(ctx)); /* neither; fall through */
        }
    }
    {   /* generic: ToString (runs user JS) -> owned UTF-8 bytes */
        size_t n;
        const char *s = JS_ToCStringLen(ctx, &n, v);
        if (!s)
            return -1;
        *powned = s;
        *pdata = (const uint8_t *)s;
        *plen = n;
        return 0;
    }
}

static const char dyn_hex_digits[] = "0123456789abcdef";

/* Fresh lowercase hex string of `data[0..len)`. */
static JSValue dyn_crypto_hex(JSContext *ctx, const uint8_t *data, size_t len)
{
    char stackbuf[DYN_HASH_MAX_DIGEST * 2];
    size_t i;
    for (i = 0; i < len; i++) {
        stackbuf[i * 2]     = dyn_hex_digits[data[i] >> 4];
        stackbuf[i * 2 + 1] = dyn_hex_digits[data[i] & 0xF];
    }
    return JS_NewStringLen(ctx, stackbuf, len * 2);
}

/* Fresh Uint8Array copying `data[0..len)` (never aliases native memory). */
static JSValue dyn_crypto_u8array(JSContext *ctx, const uint8_t *data, size_t len)
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

/* ==================================================================== *
 *  one-shot hash functions (magic = algorithm index; hex vs binary)     *
 * ==================================================================== */

static JSValue dyn_crypto_hash(JSContext *ctx, JSValueConst this_val, int argc,
                               JSValueConst *argv, int magic)
{
    const dyn_hash_algo_t *a = dyn_hash_algo_by_id(magic);
    const uint8_t *data;
    size_t len;
    const char *owned;
    dyn_hash_ctx_t c;
    uint8_t digest[DYN_HASH_MAX_DIGEST];
    (void)this_val; (void)argc;

    if (dyn_crypto_data(ctx, argv[0], &data, &len, &owned))
        return JS_EXCEPTION;
    dyn_hash_init(&c, a);
    dyn_hash_update(&c, data, len);
    dyn_hash_final(&c, digest);
    if (owned)
        JS_FreeCString(ctx, owned);
    return dyn_crypto_u8array(ctx, digest, a->digest_size);
}

static JSValue dyn_crypto_hash_hex(JSContext *ctx, JSValueConst this_val, int argc,
                                   JSValueConst *argv, int magic)
{
    const dyn_hash_algo_t *a = dyn_hash_algo_by_id(magic);
    const uint8_t *data;
    size_t len;
    const char *owned;
    dyn_hash_ctx_t c;
    uint8_t digest[DYN_HASH_MAX_DIGEST];
    (void)this_val; (void)argc;

    if (dyn_crypto_data(ctx, argv[0], &data, &len, &owned))
        return JS_EXCEPTION;
    dyn_hash_init(&c, a);
    dyn_hash_update(&c, data, len);
    dyn_hash_final(&c, digest);
    if (owned)
        JS_FreeCString(ctx, owned);
    return dyn_crypto_hex(ctx, digest, a->digest_size);
}

/* ==================================================================== *
 *  HMAC (magic = 1 -> hex, 0 -> Uint8Array)                             *
 * ==================================================================== */

static JSValue dyn_crypto_hmac(JSContext *ctx, JSValueConst this_val, int argc,
                               JSValueConst *argv, int magic)
{
    const char *aname;
    const dyn_hash_algo_t *a;
    const uint8_t *kp, *dp;
    size_t kl, dl;
    const char *kown, *down;
    uint8_t k0[DYN_HASH_MAX_BLOCK], out[DYN_HASH_MAX_DIGEST];
    (void)this_val; (void)argc;

    /* 1. algorithm name (before any buffer pointer is held) */
    aname = JS_ToCString(ctx, argv[0]);
    if (!aname)
        return JS_EXCEPTION;
    a = dyn_hash_algo_by_name(aname);
    JS_FreeCString(ctx, aname);
    if (!a)
        return JS_ThrowTypeError(ctx, "hmac: unknown algorithm");

    /* 2. key -> block-sized K0 immediately (owned/stable), then release the key.
     * Consuming the key before coercing `data` means a later user-JS coercion
     * can never dangle a borrowed key pointer. */
    if (dyn_crypto_data(ctx, argv[1], &kp, &kl, &kown))
        return JS_EXCEPTION;
    memset(k0, 0, sizeof(k0));
    dyn_hmac_key0(a, kp, kl, k0);
    if (kown)
        JS_FreeCString(ctx, kown);

    /* 3. data (may run user JS; only its own borrowed pointer is live now) */
    if (dyn_crypto_data(ctx, argv[2], &dp, &dl, &down)) {
        memset(k0, 0, sizeof(k0)); /* the key schedule does not survive a bail */
        return JS_EXCEPTION;
    }
    dyn_hmac_finish(a, k0, dp, dl, out); /* pure C; no JS between resolve and use */
    if (down)
        JS_FreeCString(ctx, down);
    memset(k0, 0, sizeof(k0)); /* the block-sized key schedule, gone */

    return magic ? dyn_crypto_hex(ctx, out, a->digest_size)
                 : dyn_crypto_u8array(ctx, out, a->digest_size);
}

/* ==================================================================== *
 *  CRC-32 / CRC-32C (magic selects the polynomial)                      *
 * ==================================================================== */

static JSValue dyn_crypto_crc32(JSContext *ctx, JSValueConst this_val, int argc,
                                JSValueConst *argv, int magic)
{
    const uint8_t *data;
    size_t len;
    const char *owned;
    uint32_t crc;
    (void)this_val; (void)argc;

    if (dyn_crypto_data(ctx, argv[0], &data, &len, &owned))
        return JS_EXCEPTION;
    crc = magic ? dyn_crc32c(data, len) : dyn_crc32(data, len);
    if (owned)
        JS_FreeCString(ctx, owned);
    return JS_NewUint32(ctx, crc);
}

/* ==================================================================== *
 *  streaming Hasher (dyna-nat resource)                               *
 * ==================================================================== */

static JSClassID dyn_hasher_class_id;

static void dyn_hasher_dispose(void *native)
{
    free(native);
}

/* Plain GC class: a hash state is a couple of hundred bytes of scratch, not a
 * scarce resource, so it is reclaimed when unreachable like any other object
 * rather than requiring a close(). */
static void dyn_hasher_finalizer(JSRuntime *rt, JSValue val)
{
    (void)rt;
    dyn_hasher_dispose(JS_GetOpaque(val, dyn_hasher_class_id));
}

static const JSClassDef dyn_hasher_class = {
    "Hasher",
    .finalizer = dyn_hasher_finalizer,
};

static JSValue dyn_hasher_ctor(JSContext *ctx, JSValueConst new_target, int argc,
                               JSValueConst *argv)
{
    const char *name;
    const dyn_hash_algo_t *a;
    dyn_hash_ctx_t *h;
    (void)new_target;

    if (argc < 1 || JS_IsUndefined(argv[0]))
        return JS_ThrowTypeError(ctx, "Hasher(algorithm) requires an algorithm name");
    name = JS_ToCString(ctx, argv[0]); /* coerce first (nothing allocated yet) */
    if (!name)
        return JS_EXCEPTION;
    a = dyn_hash_algo_by_name(name);
    JS_FreeCString(ctx, name);
    if (!a)
        return JS_ThrowTypeError(ctx,
            "Hasher: unknown algorithm (md5|sha1|sha224|sha256|sha384|sha512)");

    h = (dyn_hash_ctx_t *)malloc(sizeof(*h));
    if (!h)
        return JS_ThrowOutOfMemory(ctx);
    dyn_hash_init(h, a);
    return dyn_plain_wrap(ctx, dyn_hasher_class_id, h, dyn_hasher_dispose);
}

/* update(data): absorb bytes; returns `this` for chaining. Coerces the data
 * argument to C locals BEFORE resolving the native handle (CLAUDE.md), so a
 * valueOf/toString that closes `this` mid-coercion yields a clean "closed"
 * error, never a use-after-free. */
static JSValue dyn_hasher_update(JSContext *ctx, JSValueConst this_val, int argc,
                                 JSValueConst *argv)
{
    const uint8_t *data;
    size_t len;
    const char *owned;
    dyn_hash_ctx_t *h;
    (void)argc;

    if (dyn_crypto_data(ctx, argv[0], &data, &len, &owned)) /* may run user JS */
        return JS_EXCEPTION;
    h = (dyn_hash_ctx_t *)dyn_plain_get(ctx, this_val, dyn_hasher_class_id);
    if (!h) {
        if (owned)
            JS_FreeCString(ctx, owned);
        return JS_EXCEPTION;
    }
    dyn_hash_update(h, data, len); /* pure C; no JS between resolve and use */
    if (owned)
        JS_FreeCString(ctx, owned);
    return JS_DupValue(ctx, this_val);
}

/* digest()/digestHex(): finalize a COPY so the streaming state stays usable
 * (further update()s and repeated digests are well-defined). No JS args, so
 * resolving first is safe. */
static JSValue dyn_hasher_digest(JSContext *ctx, JSValueConst this_val, int argc,
                                 JSValueConst *argv)
{
    dyn_hash_ctx_t *h, copy;
    uint8_t digest[DYN_HASH_MAX_DIGEST];
    (void)argc; (void)argv;

    h = (dyn_hash_ctx_t *)dyn_plain_get(ctx, this_val, dyn_hasher_class_id);
    if (!h)
        return JS_EXCEPTION;
    copy = *h;
    dyn_hash_final(&copy, digest);
    return dyn_crypto_u8array(ctx, digest, h->algo->digest_size);
}

static JSValue dyn_hasher_digest_hex(JSContext *ctx, JSValueConst this_val, int argc,
                                     JSValueConst *argv)
{
    dyn_hash_ctx_t *h, copy;
    uint8_t digest[DYN_HASH_MAX_DIGEST];
    (void)argc; (void)argv;

    h = (dyn_hash_ctx_t *)dyn_plain_get(ctx, this_val, dyn_hasher_class_id);
    if (!h)
        return JS_EXCEPTION;
    copy = *h;
    dyn_hash_final(&copy, digest);
    return dyn_crypto_hex(ctx, digest, h->algo->digest_size);
}

/* reset(): return the hasher to its initial state (reuse without reallocating). */
static JSValue dyn_hasher_reset(JSContext *ctx, JSValueConst this_val, int argc,
                                JSValueConst *argv)
{
    dyn_hash_ctx_t *h;
    (void)argc; (void)argv;

    h = (dyn_hash_ctx_t *)dyn_plain_get(ctx, this_val, dyn_hasher_class_id);
    if (!h)
        return JS_EXCEPTION;
    dyn_hash_reset(h);
    return JS_DupValue(ctx, this_val);
}

static JSValue dyn_hasher_get_algorithm(JSContext *ctx, JSValueConst this_val)
{
    dyn_hash_ctx_t *h = (dyn_hash_ctx_t *)dyn_plain_get(ctx, this_val, dyn_hasher_class_id);
    if (!h)
        return JS_EXCEPTION;
    return JS_NewString(ctx, h->algo->name);
}

static JSValue dyn_hasher_get_digest_size(JSContext *ctx, JSValueConst this_val)
{
    dyn_hash_ctx_t *h = (dyn_hash_ctx_t *)dyn_plain_get(ctx, this_val, dyn_hasher_class_id);
    if (!h)
        return JS_EXCEPTION;
    return JS_NewInt32(ctx, (int32_t)h->algo->digest_size);
}

static const JSCFunctionListEntry dyn_hasher_proto[] = {
    JS_CFUNC_DEF("update", 1, dyn_hasher_update),
    JS_CFUNC_DEF("digest", 0, dyn_hasher_digest),
    JS_CFUNC_DEF("digestHex", 0, dyn_hasher_digest_hex),
    JS_CFUNC_DEF("reset", 0, dyn_hasher_reset),
    JS_CGETSET_DEF("algorithm", dyn_hasher_get_algorithm, NULL),
    JS_CGETSET_DEF("digestSize", dyn_hasher_get_digest_size, NULL),
};


/* ==================================================================== *
 *  dyna:hash -- the unkeyed 64/32-bit hashes                            *
 * ==================================================================== */

/* xxhash64(data, seed?) / xxhash32(data, seed?) (magic 0/1).
 *
 * Returned as a NUMBER, which for xxhash64 means the low 53 bits are exact and
 * the top 11 are not -- so it is returned as a hex STRING instead when the
 * caller asks for one, and the doc says which. A silently-rounded 64-bit hash
 * is worse than no 64-bit hash: it collides in ways the algorithm does not. */
static JSValue dyn_hash_xx(JSContext *ctx, JSValueConst this_val, int argc,
                           JSValueConst *argv, int magic)
{
    const uint8_t *data;
    size_t len;
    const char *owned;
    uint64_t seed = 0;
    char buf[17];
    static const char hexd[] = "0123456789abcdef";
    int i;
    (void)this_val;

    /* the seed is coerced FIRST: it is the argument most likely to be an
     * object with a valueOf, and the data pointer must not be live when it runs */
    if (argc > 1 && !JS_IsUndefined(argv[1])) {
        double d;
        if (JS_ToFloat64(ctx, &d, argv[1]))
            return JS_EXCEPTION;
        /* the double->int64 cast is UB outside this range; a NaN fails both
           bounds, so the && rejects it (and +/-Inf) without math.h */
        if (!(d >= -9223372036854775808.0 && d < 9223372036854775808.0))
            return JS_ThrowRangeError(ctx,
                "xxhash seed must be a finite number in the int64 range");
        seed = (uint64_t)(int64_t)d;
    }
    if (dyn_crypto_data(ctx, argv[0], &data, &len, &owned))
        return JS_EXCEPTION;
    if (magic == 0) {
        uint64_t h = dyn_xxh64(data, len, seed);
        if (owned)
            JS_FreeCString(ctx, owned);
        for (i = 15; i >= 0; i--) {
            buf[i] = hexd[h & 0xf];
            h >>= 4;
        }
        buf[16] = 0;
        /* hex, not a double: a 64-bit value does not survive a JS number */
        return JS_NewStringLen(ctx, buf, 16);
    } else {
        uint32_t h = dyn_xxh32(data, len, (uint32_t)seed);
        if (owned)
            JS_FreeCString(ctx, owned);
        return JS_NewUint32(ctx, h);
    }
}

/* ==================================================================== *
 *  dyna:crypto -- secret-dependent and constant-time operations         *
 * ==================================================================== */

/* timingSafeEqual(a, b) -> boolean.
 *
 * FALSE for different lengths, and that is not a timing leak worth closing:
 * the length of a MAC is public. What must not leak is WHERE two equal-length
 * values first differ, and dyn_ct_equal accumulates over the whole input for
 * that reason -- verified branch-free in the generated asm, not assumed. */
static JSValue dyn_crypto_ct_equal(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv)
{
    const uint8_t *a, *b;
    size_t alen, blen;
    const char *aowned, *bowned;
    uint8_t *acopy;
    int r;
    (void)this_val; (void)argc;

    if (dyn_crypto_data(ctx, argv[0], &a, &alen, &aowned))
        return JS_EXCEPTION;
    /* Coercing the SECOND argument can run user JS, which can detach the first
     * argument's buffer. Copy it before that can happen. */
    acopy = (uint8_t *)malloc(alen ? alen : 1);
    if (!acopy) {
        if (aowned)
            JS_FreeCString(ctx, aowned);
        return JS_ThrowOutOfMemory(ctx);
    }
    if (alen)
        memcpy(acopy, a, alen);
    if (aowned)
        JS_FreeCString(ctx, aowned);
    if (dyn_crypto_data(ctx, argv[1], &b, &blen, &bowned)) {
        free(acopy);
        return JS_EXCEPTION;
    }
    r = (alen == blen) && dyn_ct_equal(acopy, b, alen);
    if (bowned)
        JS_FreeCString(ctx, bowned);
    free(acopy);
    return JS_NewBool(ctx, r);
}

/* hkdf({hash, key, salt, info, length}) / pbkdf2({hash, password, salt,
 * iterations, length}) (magic 0/1) -> Uint8Array.
 *
 * An options object rather than positional arguments because the ORDER of a
 * key and a salt is exactly the kind of thing a caller gets wrong silently:
 * swapping them produces a perfectly good-looking key that is the wrong one. */
static JSValue dyn_crypto_kdf(JSContext *ctx, JSValueConst this_val, int argc,
                              JSValueConst *argv, int magic)
{
    const dyn_hash_algo_t *a;
    uint8_t *ikm = NULL, *salt = NULL, *info = NULL, *out = NULL;
    size_t ikm_len = 0, salt_len = 0, info_len = 0;
    int64_t length = 32, iters = 100000;
    JSValue v, result = JS_EXCEPTION;
    const char *name = NULL;
    int rc;
    (void)this_val; (void)argc;

    if (!JS_IsObject(argv[0]))
        return JS_ThrowTypeError(ctx, "expected an options object");

    /* Every field is copied into libc memory BEFORE the next getter runs: a
     * getter is user JS and can detach any buffer read before it. */
#define DYN_KDF_BYTES(field, dst, dstlen, required)                            \
    do {                                                                       \
        const uint8_t *p; size_t n; const char *ow;                            \
        v = JS_GetPropertyStr(ctx, argv[0], field);                            \
        if (JS_IsException(v)) goto done;                                      \
        if (JS_IsUndefined(v) || JS_IsNull(v)) {                               \
            JS_FreeValue(ctx, v);                                              \
            if (required) {                                                    \
                JS_ThrowTypeError(ctx, "%s is required", field);               \
                goto done;                                                     \
            }                                                                  \
        } else {                                                               \
            if (dyn_crypto_data(ctx, v, &p, &n, &ow)) { JS_FreeValue(ctx, v); goto done; } \
            (dst) = (uint8_t *)malloc(n ? n : 1);                              \
            if (!(dst)) { if (ow) JS_FreeCString(ctx, ow); JS_FreeValue(ctx, v); \
                          JS_ThrowOutOfMemory(ctx); goto done; }               \
            if (n) memcpy((dst), p, n);                                        \
            (dstlen) = n;                                                      \
            if (ow) JS_FreeCString(ctx, ow);                                   \
            JS_FreeValue(ctx, v);                                              \
        }                                                                      \
    } while (0)

    v = JS_GetPropertyStr(ctx, argv[0], "hash");
    if (JS_IsException(v))
        return JS_EXCEPTION;
    if (JS_IsUndefined(v)) {
        name = NULL;
        a = dyn_hash_algo_by_name("sha256");
    } else {
        name = JS_ToCString(ctx, v);
        if (!name) { JS_FreeValue(ctx, v); return JS_EXCEPTION; }
        a = dyn_hash_algo_by_name(name);
    }
    JS_FreeValue(ctx, v);
    if (!a) {
        JS_ThrowTypeError(ctx, "unknown hash \"%s\"", name ? name : "");
        if (name) JS_FreeCString(ctx, name);
        return JS_EXCEPTION;
    }
    if (name)
        JS_FreeCString(ctx, name);

    DYN_KDF_BYTES(magic ? "password" : "key", ikm, ikm_len, 1);
    DYN_KDF_BYTES("salt", salt, salt_len, 0);
    if (!magic)
        DYN_KDF_BYTES("info", info, info_len, 0);

    v = JS_GetPropertyStr(ctx, argv[0], "length");
    if (JS_IsException(v)) goto done;
    if (!JS_IsUndefined(v) && JS_ToInt64(ctx, &length, v)) { JS_FreeValue(ctx, v); goto done; }
    JS_FreeValue(ctx, v);
    if (length < 1 || length > (1 << 20)) {
        JS_ThrowRangeError(ctx, "length must be 1..1048576");
        goto done;
    }
    if (magic) {
        int64_t blocks;
        v = JS_GetPropertyStr(ctx, argv[0], "iterations");
        if (JS_IsException(v)) goto done;
        if (!JS_IsUndefined(v) && JS_ToInt64(ctx, &iters, v)) { JS_FreeValue(ctx, v); goto done; }
        JS_FreeValue(ctx, v);
        if (iters < 1 || iters > (1 << 26)) {
            JS_ThrowRangeError(ctx, "iterations must be 1..67108864");
            goto done;
        }
        /* Bound the PRODUCT: {2^26 iters, 2^20 len} pass the per-factor caps
         * yet cost 2^41 HMAC compressions. iters*ceil(len/hLen)<=2^24 caps the
         * worst case at ~2^26 hash compressions (~seconds; OWASP Jun-2026:
         * 600k iterations for PBKDF2-HMAC-SHA256, ~28x inside this cap). RFC
         * 8018 sets no product bound, so this is a local resource limit. */
        blocks = (length + a->digest_size - 1) / a->digest_size;
        if (iters * blocks > DYN_PBKDF2_MAX_COMPRESSIONS) {
            JS_ThrowRangeError(ctx,
                "PBKDF2: iterations x output-blocks exceeds %d (DoS bound)",
                DYN_PBKDF2_MAX_COMPRESSIONS);
            goto done;
        }
    }

    out = (uint8_t *)malloc((size_t)length);
    if (!out) {
        JS_ThrowOutOfMemory(ctx);
        goto done;
    }
    if (magic)
        rc = dyn_pbkdf2(a, ikm, ikm_len, salt, salt_len, (uint32_t)iters,
                        out, (size_t)length);
    else
        rc = dyn_hkdf(a, ikm, ikm_len, salt, salt_len, info, info_len,
                      out, (size_t)length);
    if (rc) {
        JS_ThrowRangeError(ctx, "length exceeds what this construction can derive");
        goto done;
    }
    result = dyn_crypto_u8array(ctx, out, (size_t)length);
done:
    if (out) {
        memset(out, 0, (size_t)length);   /* derived key material */
        free(out);
    }
    if (ikm) {
        memset(ikm, 0, ikm_len);          /* the secret input */
        free(ikm);
    }
    free(salt);
    free(info);
    return result;
#undef DYN_KDF_BYTES
}

/* randomBytes(n) -> Uint8Array of OS entropy. Not the seeded PRNG in
 * dyna:random: that one is reproducible, which is the opposite of what this is
 * for, and mixing them up is a real failure mode. */
static JSValue dyn_crypto_random_bytes(JSContext *ctx, JSValueConst this_val,
                                       int argc, JSValueConst *argv)
{
    int64_t n = 32;
    uint8_t *buf;
    JSValue out;
    (void)this_val;

    if (argc > 0 && !JS_IsUndefined(argv[0])) {
        if (JS_ToInt64(ctx, &n, argv[0]))
            return JS_EXCEPTION;
    }
    if (n < 0 || n > (1 << 24))
        return JS_ThrowRangeError(ctx, "count must be 0..16777216");
    buf = (uint8_t *)malloc(n ? (size_t)n : 1);
    if (!buf)
        return JS_ThrowOutOfMemory(ctx);
    if (n && dyn_os_entropy(buf, (size_t)n) < 0) {
        memset(buf, 0, (size_t)n);   /* a partial fill is not entropy */
        free(buf);
        return JS_ThrowInternalError(ctx,
            "randomBytes: OS entropy unavailable");
    }
    out = dyn_crypto_u8array(ctx, buf, (size_t)n);
    memset(buf, 0, n ? (size_t)n : 1);
    free(buf);
    return out;
}

/* ---- class Hmac: the key IS the configuration ------------------------------
 *
 * `new Hmac("sha256", key)` derives the block-sized key schedule once; every
 * signHex() after that reuses it. Unlike `Hasher` -- whose crossover is "never"
 * because it has no configuration to hoist -- this one has a real per-call cost
 * to remove, and the bench says so.
 *
 * It holds a KEY, so it is a resource with close(), not a plain GC object: the
 * point of close() here is to zero the key schedule at a moment the caller
 * chooses rather than whenever the collector gets to it. */

typedef struct {
    dyn_hmac_ctx_t h;
} dyn_hmac_res_t;

static JSClassID dyn_hmac_class_id;

static void dyn_hmac_dispose(void *native)
{
    dyn_hmac_res_t *m = (dyn_hmac_res_t *)native;
    if (!m)
        return;
    memset(m, 0, sizeof(*m));      /* the key schedule, gone on close() */
    free(m);
}

static const JSClassDef dyn_hmac_class = { "Hmac", .finalizer = dyn_res_finalizer };

static JSValue dyn_hmac_ctor(JSContext *ctx, JSValueConst new_target, int argc,
                             JSValueConst *argv)
{
    const dyn_hash_algo_t *a;
    const char *name;
    const uint8_t *key;
    size_t keylen;
    const char *owned;
    uint8_t *keycopy;
    dyn_hmac_res_t *m;
    (void)new_target;

    if (argc < 2)
        return JS_ThrowTypeError(ctx, "new Hmac(algorithm, key)");
    name = JS_ToCString(ctx, argv[0]);
    if (!name)
        return JS_EXCEPTION;
    a = dyn_hash_algo_by_name(name);
    if (!a) {
        JS_ThrowTypeError(ctx, "unknown hash algorithm \"%s\"", name);
        JS_FreeCString(ctx, name);
        return JS_EXCEPTION;
    }
    JS_FreeCString(ctx, name);
    if (dyn_crypto_data(ctx, argv[1], &key, &keylen, &owned))
        return JS_EXCEPTION;
    keycopy = (uint8_t *)malloc(keylen ? keylen : 1);
    if (!keycopy) {
        if (owned)
            JS_FreeCString(ctx, owned);
        return JS_ThrowOutOfMemory(ctx);
    }
    if (keylen)
        memcpy(keycopy, key, keylen);
    if (owned)
        JS_FreeCString(ctx, owned);
    m = (dyn_hmac_res_t *)calloc(1, sizeof(*m));
    if (!m) {
        memset(keycopy, 0, keylen ? keylen : 1);
        free(keycopy);
        return JS_ThrowOutOfMemory(ctx);
    }
    /* the key schedule is derived HERE, which is the whole point of the class */
    dyn_hmac_init(&m->h, a, keycopy, keylen);
    memset(keycopy, 0, keylen ? keylen : 1);
    free(keycopy);
    return dyn_res_wrap(ctx, dyn_hmac_class_id, m, dyn_hmac_dispose);
}

/* sign(msg) / signHex(msg) (magic 0/1): a complete MAC, leaving the object
 * ready for the next message. */
static JSValue dyn_hmac_sign(JSContext *ctx, JSValueConst this_val, int argc,
                             JSValueConst *argv, int magic)
{
    dyn_hmac_res_t *m;
    const uint8_t *data;
    size_t len;
    const char *owned;
    uint8_t mac[DYN_HASH_MAX_DIGEST];
    size_t dsize;
    (void)argc;

    /* Coerce (which can run user JS and close `this`) BEFORE resolving, then
     * use the borrowed pointer directly: nothing between the resolve and the
     * absorb runs JS, so the borrow cannot be invalidated. Copying the message
     * "to be safe" costs a malloc per call and was measured -- it is what made
     * the first version of this class SLOWER than the free function. */
    if (dyn_crypto_data(ctx, argv[0], &data, &len, &owned))
        return JS_EXCEPTION;
    m = (dyn_hmac_res_t *)dyn_res_native(ctx, this_val, dyn_hmac_class_id);
    if (!m) {
        if (owned)
            JS_FreeCString(ctx, owned);
        return JS_EXCEPTION;
    }
    dsize = m->h.algo->digest_size;
    dyn_hmac_update(&m->h, data, len);  /* pure C between resolve and use */
    dyn_hmac_final(&m->h, mac);         /* finalises AND resets */
    if (owned)
        JS_FreeCString(ctx, owned);
    return magic ? dyn_crypto_hex(ctx, mac, dsize)
                 : dyn_crypto_u8array(ctx, mac, dsize);
}

/* update(msg): streaming, for a message not held in memory at once. */
static JSValue dyn_hmac_update_js(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv)
{
    dyn_hmac_res_t *m;
    const uint8_t *data;
    size_t len;
    const char *owned;
    (void)argc;

    if (dyn_crypto_data(ctx, argv[0], &data, &len, &owned))
        return JS_EXCEPTION;
    m = (dyn_hmac_res_t *)dyn_res_native(ctx, this_val, dyn_hmac_class_id);
    if (!m) {
        if (owned)
            JS_FreeCString(ctx, owned);
        return JS_EXCEPTION;
    }
    dyn_hmac_update(&m->h, data, len);
    if (owned)
        JS_FreeCString(ctx, owned);
    return JS_DupValue(ctx, this_val);
}

static JSValue dyn_hmac_digest(JSContext *ctx, JSValueConst this_val, int argc,
                               JSValueConst *argv, int magic)
{
    dyn_hmac_res_t *m = (dyn_hmac_res_t *)dyn_res_native(ctx, this_val,
                                                         dyn_hmac_class_id);
    uint8_t mac[DYN_HASH_MAX_DIGEST];
    size_t dsize;
    (void)argc; (void)argv;

    if (!m)
        return JS_EXCEPTION;
    dsize = m->h.algo->digest_size;
    dyn_hmac_final(&m->h, mac);
    return magic ? dyn_crypto_hex(ctx, mac, dsize)
                 : dyn_crypto_u8array(ctx, mac, dsize);
}

/* verify(msg, tag) -> boolean, compared in constant time. Exists so a caller
 * cannot write `signHex(m) === tag`, which compares two strings with an early
 * exit and publishes how much of the MAC was guessed correctly. */
/* Two compares per nibble. This was a 16-iteration scan of the digit table --
 * with no early exit, because it assigned rather than broke -- so verifying a
 * SHA-256 tag spent 512 iterations decoding it against 2 compressions of
 * actual cryptography.
 *
 * The old form also accepted bytes 0x10-0x19 as '0'-'9', because it compared
 * `ch | 0x20` and 0x10 | 0x20 is '0'. Harmless (the bytes still went through
 * the constant-time compare) but a tag parser should not be lax. */
static inline int dyn_hex_val(uint8_t ch)
{
    unsigned d = (unsigned)ch - '0';
    if (d < 10)
        return (int)d;
    d = (unsigned)(ch | 0x20) - 'a';
    return d < 6 ? (int)(d + 10) : -1;
}

static JSValue dyn_hmac_verify(JSContext *ctx, JSValueConst this_val, int argc,
                               JSValueConst *argv)
{
    dyn_hmac_res_t *m;
    const uint8_t *data, *tag;
    size_t len, taglen, dsize;
    const char *owned, *towned;
    uint8_t *tagcopy;
    uint8_t mac[DYN_HASH_MAX_DIGEST];
    int r;
    (void)argc;

    /* The TAG is coerced first and copied, because coercing it runs user JS
     * that could detach the message's buffer. The MESSAGE is then borrowed and
     * used with no JS in between -- the tag is 32 bytes and the message is not,
     * so this is the copy worth making and the other is not. */
    if (dyn_crypto_data(ctx, argv[1], &tag, &taglen, &towned))
        return JS_EXCEPTION;
    tagcopy = (uint8_t *)malloc(taglen ? taglen : 1);
    if (!tagcopy) {
        if (towned) JS_FreeCString(ctx, towned);
        return JS_ThrowOutOfMemory(ctx);
    }
    if (taglen) memcpy(tagcopy, tag, taglen);
    if (towned) JS_FreeCString(ctx, towned);
    if (dyn_crypto_data(ctx, argv[0], &data, &len, &owned)) {
        free(tagcopy);
        return JS_EXCEPTION;
    }
    m = (dyn_hmac_res_t *)dyn_res_native(ctx, this_val, dyn_hmac_class_id);
    if (!m) {
        if (owned) JS_FreeCString(ctx, owned);
        free(tagcopy);
        return JS_EXCEPTION;
    }
    dsize = m->h.algo->digest_size;
    dyn_hmac_update(&m->h, data, len);
    dyn_hmac_final(&m->h, mac);
    if (owned) JS_FreeCString(ctx, owned);
    if (taglen == dsize) {
        r = dyn_ct_equal(mac, tagcopy, dsize);
    } else if (taglen == dsize * 2) {
        /* a hex tag: decode and compare the bytes, so the comparison is over
         * the MAC rather than over its spelling */
        uint8_t dec[DYN_HASH_MAX_DIGEST];
        size_t i;
        int ok = 1;
        for (i = 0; i < dsize; i++) {
            int hi = dyn_hex_val(tagcopy[i * 2]);
            int lo = dyn_hex_val(tagcopy[i * 2 + 1]);
            if (hi < 0 || lo < 0) { ok = 0; break; }
            dec[i] = (uint8_t)((hi << 4) | lo);
        }
        r = ok && dyn_ct_equal(mac, dec, dsize);
    } else {
        r = 0;
    }
    free(tagcopy);
    return JS_NewBool(ctx, r);
}

static JSValue dyn_hmac_get_algorithm(JSContext *ctx, JSValueConst this_val)
{
    dyn_hmac_res_t *m = (dyn_hmac_res_t *)dyn_res_native(ctx, this_val,
                                                         dyn_hmac_class_id);
    if (!m)
        return JS_EXCEPTION;
    return JS_NewString(ctx, m->h.algo->name);
}

static JSValue dyn_hmac_get_size(JSContext *ctx, JSValueConst this_val)
{
    dyn_hmac_res_t *m = (dyn_hmac_res_t *)dyn_res_native(ctx, this_val,
                                                         dyn_hmac_class_id);
    if (!m)
        return JS_EXCEPTION;
    return JS_NewInt32(ctx, (int32_t)m->h.algo->digest_size);
}

static const JSCFunctionListEntry dyn_hmac_proto[] = {
    JS_CFUNC_MAGIC_DEF("sign", 1, dyn_hmac_sign, 0),
    JS_CFUNC_MAGIC_DEF("signHex", 1, dyn_hmac_sign, 1),
    JS_CFUNC_DEF("verify", 2, dyn_hmac_verify),
    JS_CFUNC_DEF("update", 1, dyn_hmac_update_js),
    JS_CFUNC_MAGIC_DEF("digest", 0, dyn_hmac_digest, 0),
    JS_CFUNC_MAGIC_DEF("digestHex", 0, dyn_hmac_digest, 1),
    JS_CGETSET_DEF("algorithm", dyn_hmac_get_algorithm, NULL),
    JS_CGETSET_DEF("digestSize", dyn_hmac_get_size, NULL),
};

/* ==================================================================== *
 *  module registration                                                  *
 * ==================================================================== */

/* SHA-3, Keccak and SHAKE: one permutation, two padding bytes. */
#include "dyna-sha3.inc.c"

/* BLAKE3's tree, BLAKE2b/2s and Murmur3-128. */
#include "dyna-blake.inc.c"

static const JSCFunctionListEntry dyn_hash_funcs[] = {
    JS_CFUNC_MAGIC_DEF("MD5",    1, dyn_crypto_hash,     DYN_HASH_MD5),
    JS_CFUNC_MAGIC_DEF("MD5Hex", 1, dyn_crypto_hash_hex, DYN_HASH_MD5),
    JS_CFUNC_MAGIC_DEF("SHA1",    1, dyn_crypto_hash,     DYN_HASH_SHA1),
    JS_CFUNC_MAGIC_DEF("SHA1Hex", 1, dyn_crypto_hash_hex, DYN_HASH_SHA1),
    JS_CFUNC_MAGIC_DEF("SHA224",    1, dyn_crypto_hash,     DYN_HASH_SHA224),
    JS_CFUNC_MAGIC_DEF("SHA224Hex", 1, dyn_crypto_hash_hex, DYN_HASH_SHA224),
    JS_CFUNC_MAGIC_DEF("SHA256",    1, dyn_crypto_hash,     DYN_HASH_SHA256),
    JS_CFUNC_MAGIC_DEF("SHA256Hex", 1, dyn_crypto_hash_hex, DYN_HASH_SHA256),
    JS_CFUNC_MAGIC_DEF("SHA384",    1, dyn_crypto_hash,     DYN_HASH_SHA384),
    JS_CFUNC_MAGIC_DEF("SHA384Hex", 1, dyn_crypto_hash_hex, DYN_HASH_SHA384),
    JS_CFUNC_MAGIC_DEF("SHA512",    1, dyn_crypto_hash,     DYN_HASH_SHA512),
    JS_CFUNC_MAGIC_DEF("SHA512Hex", 1, dyn_crypto_hash_hex, DYN_HASH_SHA512),

    JS_CFUNC_MAGIC_DEF("CRC32",  1, dyn_crypto_crc32, 0),
    JS_CFUNC_MAGIC_DEF("CRC32C", 1, dyn_crypto_crc32, 1),

    JS_CFUNC_MAGIC_DEF("XXHash64", 1, dyn_hash_xx, 0),
    JS_CFUNC_MAGIC_DEF("XXHash32", 1, dyn_hash_xx, 1),
    JS_CFUNC_MAGIC_DEF("SHA3_224", 1, dyn_sha3, 0),
    JS_CFUNC_MAGIC_DEF("SHA3_256", 1, dyn_sha3, 1),
    JS_CFUNC_MAGIC_DEF("SHA3_384", 1, dyn_sha3, 2),
    JS_CFUNC_MAGIC_DEF("SHA3_512", 1, dyn_sha3, 3),
    JS_CFUNC_MAGIC_DEF("Keccak256", 1, dyn_sha3, 4),
    JS_CFUNC_MAGIC_DEF("SHAKE128", 1, dyn_sha3, 5),
    JS_CFUNC_MAGIC_DEF("SHAKE256", 1, dyn_sha3, 6),
    JS_CFUNC_MAGIC_DEF("SHA3_224Hex", 1, dyn_sha3, 8),
    JS_CFUNC_MAGIC_DEF("SHA3_256Hex", 1, dyn_sha3, 9),
    JS_CFUNC_MAGIC_DEF("SHA3_384Hex", 1, dyn_sha3, 10),
    JS_CFUNC_MAGIC_DEF("SHA3_512Hex", 1, dyn_sha3, 11),
    JS_CFUNC_MAGIC_DEF("Keccak256Hex", 1, dyn_sha3, 12),
    JS_CFUNC_MAGIC_DEF("SHAKE128Hex", 1, dyn_sha3, 13),
    JS_CFUNC_MAGIC_DEF("SHAKE256Hex", 1, dyn_sha3, 14),

    JS_CFUNC_MAGIC_DEF("BLAKE3",     1, dyn_blake, 0),
    JS_CFUNC_MAGIC_DEF("BLAKE2b",    1, dyn_blake, 1),
    JS_CFUNC_MAGIC_DEF("BLAKE2s",    1, dyn_blake, 2),
    JS_CFUNC_MAGIC_DEF("Murmur3_128", 1, dyn_blake, 3),
    JS_CFUNC_MAGIC_DEF("BLAKE3Hex",  1, dyn_blake, 8),
    JS_CFUNC_MAGIC_DEF("BLAKE2bHex", 1, dyn_blake, 9),
    JS_CFUNC_MAGIC_DEF("BLAKE2sHex", 1, dyn_blake, 10),
    JS_CFUNC_MAGIC_DEF("Murmur3_128Hex", 1, dyn_blake, 11),
};

/* ---- HOTP / TOTP (RFC 4226, RFC 6238) --------------------------------
   HMAC plus a dynamic truncation. The whole algorithm is below; the primitives
   already existed, which is why this ships with the auth story. */

static const uint32_t OTP_POW10[9] = {
    1, 10, 100, 1000, 10000, 100000, 1000000, 10000000, 100000000
};

/* Dynamic truncation: the low nibble of the last byte selects the offset. */
static uint32_t otp_truncate(const uint8_t *mac, size_t maclen, int digits)
{
    size_t off = mac[maclen - 1] & 0x0f;
    uint32_t bin = ((uint32_t)(mac[off] & 0x7f) << 24) |
                   ((uint32_t)mac[off + 1] << 16) |
                   ((uint32_t)mac[off + 2] << 8)  |
                   ((uint32_t)mac[off + 3]);
    return bin % OTP_POW10[digits];
}

static int otp_compute(const char *algo, const uint8_t *key, size_t keylen,
                       uint64_t counter, int digits, char *out, size_t outsz)
{
    const dyn_hash_algo_t *a = dyn_hash_algo_by_name(algo ? algo : "sha1");
    uint8_t msg[8], mac[64];
    int i;

    if (!a || digits < 1 || digits > 8 || a->digest_size > sizeof(mac))
        return -1;
    for (i = 7; i >= 0; i--) { msg[i] = (uint8_t)(counter & 0xff); counter >>= 8; }
    dyn_hmac(a, key, keylen, msg, sizeof(msg), mac);
    snprintf(out, outsz, "%0*u", digits, otp_truncate(mac, a->digest_size, digits));
    return 0;
}

/* HOTPGenerate(secret, counter[, { digits, algo }]) -> "123456" */
static JSValue dyn_crypto_hotp(JSContext *ctx, JSValueConst this_val, int argc,
                               JSValueConst *argv)
{
    const uint8_t *key; size_t keylen; const char *kown;
    int64_t counter = 0; int32_t digits = 6;
    const char *algo = NULL;
    char out[16];
    JSValue av = JS_UNDEFINED, dv = JS_UNDEFINED, r;
    (void)this_val;

    if (argc < 2)
        return JS_ThrowTypeError(ctx, "HOTPGenerate(secret, counter[, opts])");
    if (JS_ToInt64(ctx, &counter, argv[1]) < 0) return JS_EXCEPTION;
    if (dyn_crypto_data(ctx, argv[0], &key, &keylen, &kown))
        return JS_EXCEPTION;
    if (counter < 0)
        return JS_ThrowRangeError(ctx, "counter must not be negative");
    if (argc > 2 && JS_IsObject(argv[2])) {
        dv = JS_GetPropertyStr(ctx, argv[2], "digits");
        if (!JS_IsUndefined(dv)) JS_ToInt32(ctx, &digits, dv);
        av = JS_GetPropertyStr(ctx, argv[2], "algo");
        if (!JS_IsUndefined(av)) algo = JS_ToCString(ctx, av);
    }
    if (digits < 6 || digits > 8) {
        if (algo) JS_FreeCString(ctx, algo);
        if (kown) JS_FreeCString(ctx, kown);
        JS_FreeValue(ctx, av); JS_FreeValue(ctx, dv);
        return JS_ThrowRangeError(ctx, "digits must be 6..8");
    }
    if (otp_compute(algo, key, keylen, (uint64_t)counter, digits,
                    out, sizeof(out)) < 0) {
        if (algo) JS_FreeCString(ctx, algo);
        if (kown) JS_FreeCString(ctx, kown);
        JS_FreeValue(ctx, av); JS_FreeValue(ctx, dv);
        return JS_ThrowTypeError(ctx, "unknown algo");
    }
    if (algo) JS_FreeCString(ctx, algo);
    if (kown) JS_FreeCString(ctx, kown);
    JS_FreeValue(ctx, av); JS_FreeValue(ctx, dv);
    r = JS_NewString(ctx, out);
    return r;
}

/* TOTPGenerate(secret[, { atSec, period, digits, algo }]) -> "123456" */
static JSValue dyn_crypto_totp(JSContext *ctx, JSValueConst this_val, int argc,
                               JSValueConst *argv)
{
    const uint8_t *key; size_t keylen; const char *kown;
    int64_t at = 0, period = 30;
    int32_t digits = 6;
    const char *algo = NULL;
    char out[16];
    JSValue v;
    (void)this_val;

    if (argc < 1)
        return JS_ThrowTypeError(ctx, "TOTPGenerate(secret[, opts])");
    at = 0;   /* atSec is REQUIRED: a clock read here would make the result
                 untestable against RFC 6238's fixed vectors. */
    if (argc > 1 && JS_IsObject(argv[1])) {
        v = JS_GetPropertyStr(ctx, argv[1], "atSec");
        if (!JS_IsUndefined(v)) JS_ToInt64(ctx, &at, v);
        JS_FreeValue(ctx, v);
        v = JS_GetPropertyStr(ctx, argv[1], "period");
        if (!JS_IsUndefined(v)) JS_ToInt64(ctx, &period, v);
        JS_FreeValue(ctx, v);
        v = JS_GetPropertyStr(ctx, argv[1], "digits");
        if (!JS_IsUndefined(v)) JS_ToInt32(ctx, &digits, v);
        JS_FreeValue(ctx, v);
        v = JS_GetPropertyStr(ctx, argv[1], "algo");
        if (!JS_IsUndefined(v)) algo = JS_ToCString(ctx, v);
        JS_FreeValue(ctx, v);
    }
    if (period <= 0 || digits < 6 || digits > 8 || at < 0) {
        if (algo) JS_FreeCString(ctx, algo);
        return JS_ThrowRangeError(ctx, "period > 0, digits 6..8, atSec >= 0");
    }
    /* Key LAST: every option has been coerced, so no user JS runs while the
       borrowed key pointer is live. */
    if (dyn_crypto_data(ctx, argv[0], &key, &keylen, &kown)) {
        if (algo) JS_FreeCString(ctx, algo);
        return JS_EXCEPTION;
    }
    if (otp_compute(algo, key, keylen, (uint64_t)(at / period), digits,
                    out, sizeof(out)) < 0) {
        if (algo) JS_FreeCString(ctx, algo);
        if (kown) JS_FreeCString(ctx, kown);
        return JS_ThrowTypeError(ctx, "unknown algo");
    }
    if (algo) JS_FreeCString(ctx, algo);
    if (kown) JS_FreeCString(ctx, kown);
    return JS_NewString(ctx, out);
}

/* ---- JWT asymmetric algorithms: RS256/384/512 and ES256/384/512 -------
 *
 * The key arrives as PEM. Signing takes a PRIVATE key, verification a PUBLIC
 * one, and neither ever reads `alg` from the token to choose a key -- that is
 * the rule the whole design rests on and it is unchanged here.
 *
 * ES* HAS A FORMAT TRAP: JWS wants the raw R||S pair, each half zero-padded to
 * the curve's coordinate size (64 bytes for P-256), while OpenSSL emits and
 * expects DER SEQUENCE{INTEGER r, INTEGER s}. Getting this wrong produces a
 * token every other library rejects, and a round trip against ourselves would
 * not notice.
 */
#ifdef CONFIG_TLS

#include <openssl/evp.h>
#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/ecdsa.h>
#include <openssl/bn.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

/* Coordinate size per ES alg, which is also half the JWS signature. */
static int jwt_es_coord(const char *alg)
{
    if (!strcmp(alg, "ES256")) return 32;
    if (!strcmp(alg, "ES384")) return 48;
    if (!strcmp(alg, "ES512")) return 66;   /* P-521: 521 bits -> 66 bytes */
    return 0;
}

static const EVP_MD *jwt_md_for(const char *alg)
{
    if (!strncmp(alg + 2, "256", 3)) return EVP_sha256();
    if (!strncmp(alg + 2, "384", 3)) return EVP_sha384();
    if (!strncmp(alg + 2, "512", 3)) return EVP_sha512();
    return NULL;
}

/* RS-family and ES-family only. 0 = symmetric or unknown, which the
   existing path already handles. */
static int jwt_is_asym(const char *alg)
{
    return alg && strlen(alg) == 5 &&
           (alg[0] == 'R' || alg[0] == 'E') && alg[1] == 'S' &&
           jwt_md_for(alg) != NULL;
}

static EVP_PKEY *jwt_pem_key(const char *pem, size_t pemlen, int want_private)
{
    BIO *b = BIO_new_mem_buf(pem, (int)pemlen);
    EVP_PKEY *k = NULL;
    if (!b)
        return NULL;
    k = want_private ? PEM_read_bio_PrivateKey(b, NULL, NULL, NULL)
                     : PEM_read_bio_PUBKEY(b, NULL, NULL, NULL);
    BIO_free(b);
    if (!k)
        ERR_clear_error();
    return k;
}

/* DER SEQUENCE{r,s} -> R||S, each padded to `coord`. Returns 0 on success. */
static int jwt_der_to_raw(const uint8_t *der, size_t derlen, int coord,
                          uint8_t *out)
{
    const uint8_t *p = der;
    ECDSA_SIG *sig = d2i_ECDSA_SIG(NULL, &p, (long)derlen);
    const BIGNUM *r, *s;
    int ok;
    if (!sig) { ERR_clear_error(); return -1; }
    ECDSA_SIG_get0(sig, &r, &s);
    ok = BN_bn2binpad(r, out, coord) == coord &&
         BN_bn2binpad(s, out + coord, coord) == coord;
    ECDSA_SIG_free(sig);
    return ok ? 0 : -1;
}

/* R||S -> DER. Caller frees *der with OPENSSL_free. Returns 0 on success. */
static int jwt_raw_to_der(const uint8_t *raw, int coord, uint8_t **der,
                          int *derlen)
{
    ECDSA_SIG *sig = ECDSA_SIG_new();
    BIGNUM *r = BN_bin2bn(raw, coord, NULL);
    BIGNUM *s = BN_bin2bn(raw + coord, coord, NULL);
    int n;
    *der = NULL;
    if (!sig || !r || !s) {
        if (r) BN_free(r);
        if (s) BN_free(s);
        if (sig) ECDSA_SIG_free(sig);
        ERR_clear_error();
        return -1;
    }
    ECDSA_SIG_set0(sig, r, s);      /* takes ownership of both */
    n = i2d_ECDSA_SIG(sig, der);
    ECDSA_SIG_free(sig);
    if (n <= 0) { ERR_clear_error(); return -1; }
    *derlen = n;
    return 0;
}

/* Sign `msg` with a PEM private key. *sig is malloc'd; caller frees. */
static int jwt_asym_sign(const char *alg, const char *pem, size_t pemlen,
                         const uint8_t *msg, size_t msglen,
                         uint8_t **sigout, size_t *sigoutlen)
{
    EVP_PKEY *k = jwt_pem_key(pem, pemlen, 1);
    EVP_MD_CTX *md = NULL;
    uint8_t *buf = NULL;
    size_t n = 0;
    int coord = jwt_es_coord(alg), rc = -1;

    *sigout = NULL; *sigoutlen = 0;
    if (!k)
        return -1;
    md = EVP_MD_CTX_new();
    if (!md || EVP_DigestSignInit(md, NULL, jwt_md_for(alg), NULL, k) <= 0)
        goto done;
    if (EVP_DigestSign(md, NULL, &n, msg, msglen) <= 0)
        goto done;
    buf = (uint8_t *)malloc(n ? n : 1);
    if (!buf) goto done;
    if (EVP_DigestSign(md, buf, &n, msg, msglen) <= 0)
        goto done;
    if (coord) {
        /* DER out of OpenSSL, raw R||S into the token. */
        uint8_t *raw = (uint8_t *)malloc((size_t)coord * 2);
        if (!raw || jwt_der_to_raw(buf, n, coord, raw) != 0) { free(raw); goto done; }
        free(buf);
        buf = raw;
        n = (size_t)coord * 2;
    }
    *sigout = buf; *sigoutlen = n; buf = NULL;
    rc = 0;
done:
    ERR_clear_error();
    free(buf);
    if (md) EVP_MD_CTX_free(md);
    EVP_PKEY_free(k);
    return rc;
}

/* 1 = good signature, 0 = bad, -1 = the KEY could not be read. */
static int jwt_asym_verify(const char *alg, const char *pem, size_t pemlen,
                           const uint8_t *msg, size_t msglen,
                           const uint8_t *sig, size_t siglen)
{
    EVP_PKEY *k = jwt_pem_key(pem, pemlen, 0);
    EVP_MD_CTX *md = NULL;
    uint8_t *der = NULL;
    int coord = jwt_es_coord(alg), derlen = 0, rc = 0;

    if (!k)
        return -1;
    if (coord) {
        /* A JWS ES signature is EXACTLY 2*coord. Anything else is a forgery,
           not a caller error, so it is a plain false. */
        if (siglen != (size_t)coord * 2) goto done;
        if (jwt_raw_to_der(sig, coord, &der, &derlen) != 0) goto done;
        sig = der; siglen = (size_t)derlen;
    }
    md = EVP_MD_CTX_new();
    if (md && EVP_DigestVerifyInit(md, NULL, jwt_md_for(alg), NULL, k) > 0)
        rc = EVP_DigestVerify(md, sig, siglen, msg, msglen) == 1;
done:
    ERR_clear_error();
    if (der) OPENSSL_free(der);
    if (md) EVP_MD_CTX_free(md);
    EVP_PKEY_free(k);
    return rc;
}

#endif /* CONFIG_TLS */

/* ---- JWT / JWS, HS256 family (design 13) ------------------------------
   The danger in JWT is entirely in verification: `alg:none`, and RS256<->HS256
   confusion where an attacker signs with the public key as an HMAC secret.
   Both are closed by ONE rule -- verify REQUIRES an explicit algorithms
   allowlist and never reads `alg` from the token to choose a key. */

/* Exact decoded length of an UNPADDED base64url segment of `n` chars. The
   decoder re-pads through scratch, so its reported length can include the
   padding it added; this is the authority. */
static size_t jwt_b64_len(size_t n)
{
    size_t r = n % 4;
    return n / 4 * 3 + (r == 2 ? 1 : r == 3 ? 2 : 0);
}

static const char *jwt_hash_for(const char *alg)
{
    if (!strcmp(alg, "HS256")) return "sha256";
    if (!strcmp(alg, "HS384")) return "sha384";
    if (!strcmp(alg, "HS512")) return "sha512";
    return NULL;    /* "none" and every asymmetric alg land here: refused */
}

static int jwt_b64url_json(JSContext *ctx, JSValueConst obj, char **out,
                           size_t *outn)
{
    JSValue j = JS_JSONStringify(ctx, obj, JS_UNDEFINED, JS_UNDEFINED);
    const char *t; size_t tn;
    char *b;
    if (JS_IsException(j)) return -1;
    t = JS_ToCStringLen(ctx, &tn, j);
    JS_FreeValue(ctx, j);
    if (!t) return -1;
    b = (char *)js_malloc(ctx, tn * 4 / 3 + 8);
    if (!b) { JS_FreeCString(ctx, t); return -1; }
    *outn = dyn_codec_base64url_encode((const uint8_t *)t, tn, b);
    JS_FreeCString(ctx, t);
    *out = b;
    return 0;
}

/* JWTSign(payload, key[, { alg }]) -> "h.p.s" */
static JSValue dyn_crypto_jwt_sign(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv)
{
    const char *alg = NULL, *hname, *kown = NULL;
    const uint8_t *key; size_t keylen;
    const dyn_hash_algo_t *a;
    char *ph = NULL, *pp = NULL, hdr[128], *signing = NULL;
    size_t phn = 0, ppn = 0, sn, hn;
    uint8_t mac[DYN_HASH_MAX_DIGEST];
    JSValue hv = JS_UNDEFINED, hobj, r = JS_EXCEPTION;
    (void)this_val;

    if (argc < 2)
        return JS_ThrowTypeError(ctx, "JWTSign(payload, key[, { alg }])");
    if (argc > 2 && JS_IsObject(argv[2])) {
        hv = JS_GetPropertyStr(ctx, argv[2], "alg");
        if (!JS_IsUndefined(hv)) alg = JS_ToCString(ctx, hv);
    }
    hname = jwt_hash_for(alg ? alg : "HS256");
#ifdef CONFIG_TLS
    if (!hname && jwt_is_asym(alg)) {
        a = NULL;                       /* asymmetric: no HMAC algorithm */
    } else
#endif
    {
        if (!hname) {
            JS_ThrowTypeError(ctx, "JWTSign: alg must be HS256, HS384 or HS512"
#ifdef CONFIG_TLS
                                   ", RS256/384/512 or ES256/384/512"
#endif
                              );
            goto done;
        }
        a = dyn_hash_algo_by_name(hname);
    }
    hn = (size_t)snprintf(hdr, sizeof(hdr), "{\"alg\":\"%s\",\"typ\":\"JWT\"}",
                          alg ? alg : "HS256");
    ph = (char *)js_malloc(ctx, hn * 4 / 3 + 8);
    if (!ph) goto done;
    phn = dyn_codec_base64url_encode((const uint8_t *)hdr, hn, ph);
    if (jwt_b64url_json(ctx, argv[1 - 1 + 0], &pp, &ppn) < 0) goto done;

    signing = (char *)js_malloc(ctx, phn + 1 + ppn + 1);
    if (!signing) goto done;
    memcpy(signing, ph, phn);
    signing[phn] = '.';
    memcpy(signing + phn + 1, pp, ppn);
    sn = phn + 1 + ppn;
    signing[sn] = 0;

    if (dyn_crypto_data(ctx, argv[1], &key, &keylen, &kown)) goto done;
    {
        /* An RS256 signature is 256 bytes and base64urls to 342 chars, so the
           signature buffer is sized per call rather than fixed. */
        const uint8_t *rawsig; size_t rawlen; uint8_t *heapsig = NULL;
        char *b64; size_t bn; char *tok;
#ifdef CONFIG_TLS
        if (!a) {
            if (jwt_asym_sign(alg, (const char *)key, keylen,
                              (const uint8_t *)signing, sn,
                              &heapsig, &rawlen) != 0) {
                JS_ThrowTypeError(ctx,
                    "JWTSign: could not sign with that PEM private key");
                goto done;
            }
            rawsig = heapsig;
        } else
#endif
        {
            dyn_hmac(a, key, keylen, (const uint8_t *)signing, sn, mac);
            rawsig = mac; rawlen = a->digest_size;
        }
        b64 = (char *)js_malloc(ctx, rawlen * 4 / 3 + 8);
        if (!b64) { free(heapsig); goto done; }
        bn = dyn_codec_base64url_encode(rawsig, rawlen, b64);
        free(heapsig);
        tok = (char *)js_malloc(ctx, sn + 1 + bn + 1);
        if (tok) {
            memcpy(tok, signing, sn);
            tok[sn] = '.';
            memcpy(tok + sn + 1, b64, bn);
            tok[sn + 1 + bn] = 0;
            r = JS_NewString(ctx, tok);
            js_free(ctx, tok);
        }
        js_free(ctx, b64);
    }
done:
    if (kown) JS_FreeCString(ctx, kown);
    if (alg) JS_FreeCString(ctx, alg);
    JS_FreeValue(ctx, hv);
    js_free(ctx, ph); js_free(ctx, pp); js_free(ctx, signing);
    (void)hobj;
    return r;
}

/* JWTVerify(token, key, { algorithms }) -> payload, or throws naming the check */
static JSValue dyn_crypto_jwt_verify(JSContext *ctx, JSValueConst this_val,
                                     int argc, JSValueConst *argv)
{
    const char *tok = NULL, *kown = NULL;
    const uint8_t *key; size_t keylen, tn = 0;
    const char *d1, *d2;
    char alg[16];
    const dyn_hash_algo_t *a;
    uint8_t mac[DYN_HASH_MAX_DIGEST], got[DYN_HASH_MAX_DIGEST];
    JSValue algs = JS_UNDEFINED, r = JS_EXCEPTION, hdrv = JS_UNDEFINED;
    size_t hn, bn, i;
    uint32_t nalg = 0;
    int allowed = 0;
    (void)this_val;

    if (argc < 3 || !JS_IsObject(argv[2]))
        return JS_ThrowTypeError(ctx,
            "JWTVerify(token, key, { algorithms }) -- algorithms is REQUIRED");
    algs = JS_GetPropertyStr(ctx, argv[2], "algorithms");
    if (!JS_IsArray(ctx, algs)) {
        JS_ThrowTypeError(ctx, "JWTVerify: `algorithms` must be an array; "
                               "without it alg:none and HS/RS confusion are open");
        goto done;
    }
    tok = JS_ToCStringLen(ctx, &tn, argv[0]);
    if (!tok) goto done;
    d1 = memchr(tok, '.', tn);
    if (!d1) { JS_ThrowTypeError(ctx, "JWTVerify: malformed token"); goto done; }
    d2 = memchr(d1 + 1, '.', tn - (size_t)(d1 + 1 - tok));
    if (!d2) { JS_ThrowTypeError(ctx, "JWTVerify: malformed token"); goto done; }

    /* the header's alg is read only to CHECK it against the allowlist */
    {
        uint8_t hb[512];
        char hs[520];
        size_t hlen = (size_t)(d1 - tok);
        size_t hl = (hlen > 500) ? 0
                  : dyn_codec_base64url_decode(tok, hlen, hb, hs);
        if (hl == DYN_CODEC_BAD) hl = 0;
        /* the decoder re-pads through scratch and can report the padded
           length; the JSON must not carry a trailing NUL into the parser */
        if (hl > jwt_b64_len(hlen)) hl = jwt_b64_len(hlen);
        hb[hl] = 0;                 /* JS_ParseJSON needs a terminated buffer */
        JSValue h;
        const char *av;
        if (hl == 0) { JS_ThrowTypeError(ctx, "JWTVerify: bad header"); goto done; }
        h = JS_ParseJSON(ctx, (const char *)hb, hl, "<jwt>");
        if (JS_IsException(h)) goto done;
        hdrv = JS_GetPropertyStr(ctx, h, "alg");
        JS_FreeValue(ctx, h);
        av = JS_IsString(hdrv) ? JS_ToCString(ctx, hdrv) : NULL;
        if (!av) { JS_ThrowTypeError(ctx, "JWTVerify: header has no alg"); goto done; }
        snprintf(alg, sizeof(alg), "%s", av);
        JS_FreeCString(ctx, av);
    }
    {
        JSValue lv = JS_GetPropertyStr(ctx, algs, "length");
        JS_ToUint32(ctx, &nalg, lv);
        JS_FreeValue(ctx, lv);
    }
    for (i = 0; i < nalg; i++) {
        JSValue e = JS_GetPropertyUint32(ctx, algs, (uint32_t)i);
        const char *es = JS_ToCString(ctx, e);
        if (es && !strcmp(es, alg)) allowed = 1;
        if (es) JS_FreeCString(ctx, es);
        JS_FreeValue(ctx, e);
    }
    if (!allowed) {
        JS_ThrowTypeError(ctx, "JWTVerify: alg %s is not in the allowlist", alg);
        goto done;
    }
    if (!jwt_hash_for(alg)
#ifdef CONFIG_TLS
        && !jwt_is_asym(alg)
#endif
       ) {
        JS_ThrowTypeError(ctx, "JWTVerify: alg %s is not supported here", alg);
        goto done;
    }
    if (dyn_crypto_data(ctx, argv[1], &key, &keylen, &kown)) goto done;
    hn = (size_t)(d2 - tok);
#ifdef CONFIG_TLS
    if (jwt_is_asym(alg)) {
        /* Heap, not the HMAC path's fixed buffers: an RS256 signature decodes
           to 256 bytes and got[] holds 64. */
        size_t slen = tn - (size_t)(d2 + 1 - tok), sl;
        uint8_t *sig = (uint8_t *)js_malloc(ctx, slen + 4);
        char *scratch = (char *)js_malloc(ctx, slen + 8);
        int v;
        if (!sig || !scratch) { js_free(ctx, sig); js_free(ctx, scratch); goto done; }
        sl = dyn_codec_base64url_decode(d2 + 1, slen, sig, scratch);
        js_free(ctx, scratch);
        if (sl == DYN_CODEC_BAD) sl = 0;
        v = jwt_asym_verify(alg, (const char *)key, keylen,
                            (const uint8_t *)tok, hn, sig, sl);
        js_free(ctx, sig);
        if (v < 0) {
            JS_ThrowTypeError(ctx, "JWTVerify: could not read that PEM public key");
            goto done;
        }
        if (v == 0) {
            JS_ThrowTypeError(ctx, "JWTVerify: signature does not verify");
            goto done;
        }
    } else
#endif
    {
    a = dyn_hash_algo_by_name(jwt_hash_for(alg));
    dyn_hmac(a, key, keylen, (const uint8_t *)tok, hn, mac);
    {
        char ss[192];
        size_t slen = tn - (size_t)(d2 + 1 - tok);
        bn = (slen > 180) ? 0 : dyn_codec_base64url_decode(d2 + 1, slen, got, ss);
        if (bn == DYN_CODEC_BAD) bn = 0;
    }
    /* constant time: a length-dependent early return leaks the boundary */
    if (bn != a->digest_size || dyn_ct_equal(mac, got, bn) != 1) {
        JS_ThrowTypeError(ctx, "JWTVerify: signature does not verify");
        goto done;
    }
    }
    {
        uint8_t *pb = (uint8_t *)js_malloc(ctx, tn + 1);
        char *ps = (char *)js_malloc(ctx, tn + 8);
        size_t pl;
        if (!pb || !ps) { js_free(ctx, pb); js_free(ctx, ps); goto done; }
        pl = dyn_codec_base64url_decode(d1 + 1, (size_t)(d2 - d1 - 1), pb, ps);
        js_free(ctx, ps);
        if (pl != DYN_CODEC_BAD) {
            size_t want = jwt_b64_len((size_t)(d2 - d1 - 1));
            if (pl > want) pl = want;
            pb[pl] = 0;             /* JS_ParseJSON needs a terminated buffer */
        }
        if (pl == DYN_CODEC_BAD) {
            js_free(ctx, pb);
            JS_ThrowTypeError(ctx, "JWTVerify: bad payload encoding");
            goto done;
        }
        r = JS_ParseJSON(ctx, (const char *)pb, pl, "<jwt>");
        js_free(ctx, pb);
    }
done:
    if (kown) JS_FreeCString(ctx, kown);
    if (tok) JS_FreeCString(ctx, tok);
    JS_FreeValue(ctx, hdrv);
    JS_FreeValue(ctx, algs);
    return r;
}

/* ---- AEAD: AESGCM and ChaCha20Poly1305 (design 12) --------------------
 *
 * Adapters over the LINKED backend, not implementations. That is deliberate:
 * AES-NI and the ARMv8 crypto extensions are an order of magnitude, not a
 * percentage, and OpenSSL already selects them at runtime -- a software AES
 * here would be correct and ten times slow on the machine it was written on.
 *
 * Compiled capability: the key is bound once, like `new RegExp`. Both classes
 * take a 12-byte nonce and append a 16-byte tag, and `open()` THROWS on
 * authentication failure rather than returning a boolean, so a caller cannot
 * use the plaintext of a forged message by forgetting to check.
 *
 * NEEDS CONFIG_TLS=y. Without it these classes are absent from the module --
 * a missing export, not a silently weaker cipher.
 */
#ifdef CONFIG_TLS

#include <openssl/evp.h>
#include <openssl/err.h>

#define AEAD_TAG   16
#define AEAD_NONCE 12

typedef struct {
    const EVP_CIPHER *cipher;
    uint8_t  key[32];
    unsigned keylen;
    const char *name;
} dyn_aead_res_t;

static JSClassID dyn_aesgcm_class_id;
static JSClassID dyn_chapoly_class_id;

static void dyn_aead_dispose(void *native)
{
    dyn_aead_res_t *a = (dyn_aead_res_t *)native;
    if (!a)
        return;
    memset(a, 0, sizeof(*a));   /* the key, gone on close() */
    free(a);
}

static const JSClassDef dyn_aesgcm_class  = { "AESGCM",  .finalizer = dyn_res_finalizer };
static const JSClassDef dyn_chapoly_class = { "ChaCha20Poly1305", .finalizer = dyn_res_finalizer };

/* magic 0 = AES-GCM (key picks the variant), 1 = ChaCha20-Poly1305. */
static JSValue dyn_aead_ctor_common(JSContext *ctx, int argc, JSValueConst *argv,
                                    int chacha)
{
    const uint8_t *key;
    size_t keylen;
    const char *owned;
    dyn_aead_res_t *a;

    if (argc < 1)
        return JS_ThrowTypeError(ctx, chacha
            ? "new ChaCha20Poly1305(key): key must be 32 bytes"
            : "new AESGCM(key): key must be 16, 24 or 32 bytes");
    if (dyn_crypto_data(ctx, argv[0], &key, &keylen, &owned))
        return JS_EXCEPTION;

    a = (dyn_aead_res_t *)calloc(1, sizeof(*a));
    if (!a) {
        if (owned) JS_FreeCString(ctx, owned);
        return JS_ThrowOutOfMemory(ctx);
    }
    if (chacha) {
        if (keylen != 32) goto badkey;
        a->cipher = EVP_chacha20_poly1305();
        a->name = "ChaCha20Poly1305";
    } else {
        switch (keylen) {
        case 16: a->cipher = EVP_aes_128_gcm(); break;
        case 24: a->cipher = EVP_aes_192_gcm(); break;
        case 32: a->cipher = EVP_aes_256_gcm(); break;
        default: goto badkey;
        }
        a->name = "AESGCM";
    }
    memcpy(a->key, key, keylen);
    a->keylen = (unsigned)keylen;
    if (owned) JS_FreeCString(ctx, owned);
    return dyn_res_wrap(ctx, chacha ? dyn_chapoly_class_id : dyn_aesgcm_class_id,
                        a, dyn_aead_dispose);
badkey:
    free(a);
    if (owned) JS_FreeCString(ctx, owned);
    return JS_ThrowTypeError(ctx, chacha
        ? "ChaCha20Poly1305: key must be exactly 32 bytes"
        : "AESGCM: key must be 16, 24 or 32 bytes");
}

static JSValue dyn_aesgcm_ctor(JSContext *ctx, JSValueConst nt, int argc,
                               JSValueConst *argv)
{ (void)nt; return dyn_aead_ctor_common(ctx, argc, argv, 0); }

static JSValue dyn_chapoly_ctor(JSContext *ctx, JSValueConst nt, int argc,
                                JSValueConst *argv)
{ (void)nt; return dyn_aead_ctor_common(ctx, argc, argv, 1); }

/* seal(nonce, plaintext [, aad]) -> ciphertext||tag
   open(nonce, sealed   [, aad]) -> plaintext, or THROW.
   magic 0 = seal, 1 = open. */
static JSValue dyn_aead_op(JSContext *ctx, JSValueConst this_val, int argc,
                           JSValueConst *argv, int magic)
{
    dyn_aead_res_t *a;
    const uint8_t *nonce = NULL, *in = NULL, *aad = NULL;
    size_t noncelen = 0, inlen = 0, aadlen = 0;
    const char *o1 = NULL, *o2 = NULL, *o3 = NULL;
    EVP_CIPHER_CTX *c = NULL;
    JSClassID cid;
    uint8_t *out = NULL;
    size_t outcap;
    int l1 = 0, l2 = 0, ok = 0;
    JSValue ret = JS_EXCEPTION;

    if (argc < 2)
        return JS_ThrowTypeError(ctx, magic ? "open(nonce, sealed[, aad])"
                                            : "seal(nonce, plaintext[, aad])");
    /* COERCE EVERY ARGUMENT FIRST: coercion runs arbitrary user JS, which can
       close() this object, so nothing may be resolved before it finishes. */
    if (dyn_crypto_data(ctx, argv[0], &nonce, &noncelen, &o1))
        return JS_EXCEPTION;
    if (dyn_crypto_data(ctx, argv[1], &in, &inlen, &o2))
        goto done;
    if (argc > 2 && !JS_IsUndefined(argv[2]) && !JS_IsNull(argv[2])) {
        if (dyn_crypto_data(ctx, argv[2], &aad, &aadlen, &o3))
            goto done;
    }

    cid = JS_GetClassID(this_val) == dyn_chapoly_class_id
            ? dyn_chapoly_class_id : dyn_aesgcm_class_id;
    a = (dyn_aead_res_t *)dyn_res_native(ctx, this_val, cid);
    if (!a)
        goto done;

    if (noncelen != AEAD_NONCE) {
        JS_ThrowTypeError(ctx, "%s: nonce must be %d bytes", a->name, AEAD_NONCE);
        goto done;
    }
    if (magic && inlen < AEAD_TAG) {
        JS_ThrowTypeError(ctx, "%s: sealed message is shorter than its tag",
                          a->name);
        goto done;
    }
    outcap = magic ? (inlen - AEAD_TAG) : (inlen + AEAD_TAG);
    out = (uint8_t *)malloc(outcap ? outcap : 1);
    if (!out) { JS_ThrowOutOfMemory(ctx); goto done; }

    c = EVP_CIPHER_CTX_new();
    if (!c) { JS_ThrowOutOfMemory(ctx); goto done; }

    if (!magic) {
        ok = EVP_EncryptInit_ex(c, a->cipher, NULL, NULL, NULL)
          && EVP_CIPHER_CTX_ctrl(c, EVP_CTRL_AEAD_SET_IVLEN, AEAD_NONCE, NULL)
          && EVP_EncryptInit_ex(c, NULL, NULL, a->key, nonce);
        if (ok && aadlen)
            ok = EVP_EncryptUpdate(c, NULL, &l1, aad, (int)aadlen);
        if (ok)
            ok = EVP_EncryptUpdate(c, out, &l1, in, (int)inlen);
        if (ok)
            ok = EVP_EncryptFinal_ex(c, out + l1, &l2);
        if (ok)
            ok = EVP_CIPHER_CTX_ctrl(c, EVP_CTRL_AEAD_GET_TAG, AEAD_TAG,
                                     out + inlen);
        if (!ok) { JS_ThrowInternalError(ctx, "%s: seal failed", a->name); goto done; }
    } else {
        ok = EVP_DecryptInit_ex(c, a->cipher, NULL, NULL, NULL)
          && EVP_CIPHER_CTX_ctrl(c, EVP_CTRL_AEAD_SET_IVLEN, AEAD_NONCE, NULL)
          && EVP_DecryptInit_ex(c, NULL, NULL, a->key, nonce);
        if (ok && aadlen)
            ok = EVP_DecryptUpdate(c, NULL, &l1, aad, (int)aadlen);
        if (ok)
            ok = EVP_DecryptUpdate(c, out, &l1, in, (int)(inlen - AEAD_TAG));
        if (ok)
            ok = EVP_CIPHER_CTX_ctrl(c, EVP_CTRL_AEAD_SET_TAG, AEAD_TAG,
                                     (void *)(in + inlen - AEAD_TAG));
        /* The tag check lives in Final and is constant-time. THROW rather than
           return false: a caller cannot then use a forged plaintext by
           forgetting to test the result. */
        if (ok)
            ok = EVP_DecryptFinal_ex(c, out + l1, &l2) > 0;
        if (!ok) {
            memset(out, 0, outcap ? outcap : 1);
            ERR_clear_error();
            JS_ThrowTypeError(ctx, "%s: authentication failed", a->name);
            goto done;
        }
    }
    ret = dyn_crypto_u8array(ctx, out, outcap);
done:
    if (c) EVP_CIPHER_CTX_free(c);
    if (out) { memset(out, 0, outcap ? outcap : 1); free(out); }
    if (o1) JS_FreeCString(ctx, o1);
    if (o2) JS_FreeCString(ctx, o2);
    if (o3) JS_FreeCString(ctx, o3);
    return ret;
}

static const JSCFunctionListEntry dyn_aead_proto[] = {
    JS_CFUNC_MAGIC_DEF("seal", 2, dyn_aead_op, 0),
    JS_CFUNC_MAGIC_DEF("open", 2, dyn_aead_op, 1),
};

#endif /* CONFIG_TLS */

/* ---- Ed25519 signatures and X25519 key agreement (design 12) -----------
 *
 * Module FUNCTIONS, not compiled capabilities: a class here would bind a key
 * once, and CLAUDE.md sec 10 says a compiled capability is not automatically a
 * win -- it needs a published crossover. These have no preprocessing worth
 * caching (EVP_PKEY construction from raw bytes is the whole setup), so the
 * class would be a second surface to keep correct for nothing.
 *
 * Verify returns a BOOLEAN, matching Hmac.verify. That is safe here and not in
 * AEAD: a bad signature yields no plaintext a caller could misuse.
 *
 * NEEDS CONFIG_TLS=y.
 */
#ifdef CONFIG_TLS

#define CURVE_SEED 32
#define ED_SIG     64

/* Build a raw key, run one operation, free. `is_priv` picks the constructor. */
static EVP_PKEY *curve_key(JSContext *ctx, int type, int is_priv,
                           const uint8_t *raw, size_t len, const char *what)
{
    EVP_PKEY *k = is_priv
        ? EVP_PKEY_new_raw_private_key(type, NULL, raw, len)
        : EVP_PKEY_new_raw_public_key(type, NULL, raw, len);
    if (!k) {
        ERR_clear_error();
        JS_ThrowTypeError(ctx, "%s: not a valid %d-byte key", what,
                          (int)CURVE_SEED);
    }
    return k;
}

/* Return { privateKey, publicKey } as raw byte arrays. */
static JSValue curve_generate(JSContext *ctx, int type, const char *what)
{
    EVP_PKEY_CTX *pc = EVP_PKEY_CTX_new_id(type, NULL);
    EVP_PKEY *k = NULL;
    uint8_t priv[CURVE_SEED], pub[CURVE_SEED];
    size_t plen = sizeof priv, qlen = sizeof pub;
    JSValue o = JS_EXCEPTION;

    if (!pc) { ERR_clear_error(); return JS_ThrowInternalError(ctx, "%s: no backend", what); }
    if (EVP_PKEY_keygen_init(pc) <= 0 || EVP_PKEY_keygen(pc, &k) <= 0) {
        ERR_clear_error();
        JS_ThrowInternalError(ctx, "%s: key generation failed", what);
        goto done;
    }
    if (EVP_PKEY_get_raw_private_key(k, priv, &plen) <= 0 ||
        EVP_PKEY_get_raw_public_key(k, pub, &qlen) <= 0) {
        ERR_clear_error();
        JS_ThrowInternalError(ctx, "%s: key export failed", what);
        goto done;
    }
    o = JS_NewObject(ctx);
    if (JS_IsException(o))
        goto done;
    /* Define, not Set: a prototype setter must not be able to intercept a key. */
    JS_DefinePropertyValueStr(ctx, o, "privateKey",
                              dyn_crypto_u8array(ctx, priv, plen), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, o, "publicKey",
                              dyn_crypto_u8array(ctx, pub, qlen), JS_PROP_C_W_E);
done:
    memset(priv, 0, sizeof priv);
    if (k) EVP_PKEY_free(k);
    EVP_PKEY_CTX_free(pc);
    return o;
}

static JSValue dyn_ed25519_generate(JSContext *ctx, JSValueConst t, int argc,
                                    JSValueConst *argv)
{ (void)t; (void)argc; (void)argv;
  return curve_generate(ctx, EVP_PKEY_ED25519, "Ed25519Generate"); }

static JSValue dyn_x25519_generate(JSContext *ctx, JSValueConst t, int argc,
                                   JSValueConst *argv)
{ (void)t; (void)argc; (void)argv;
  return curve_generate(ctx, EVP_PKEY_X25519, "X25519Generate"); }

/* Ed25519Sign(privateKey, message) -> 64-byte signature. */
static JSValue dyn_ed25519_sign(JSContext *ctx, JSValueConst t, int argc,
                                JSValueConst *argv)
{
    const uint8_t *key = NULL, *msg = NULL;
    size_t keylen = 0, msglen = 0, siglen = ED_SIG;
    const char *o1 = NULL, *o2 = NULL;
    EVP_PKEY *k = NULL;
    EVP_MD_CTX *md = NULL;
    uint8_t sig[ED_SIG];
    JSValue ret = JS_EXCEPTION;
    (void)t;

    if (argc < 2)
        return JS_ThrowTypeError(ctx, "Ed25519Sign(privateKey, message)");
    if (dyn_crypto_data(ctx, argv[0], &key, &keylen, &o1))
        return JS_EXCEPTION;
    if (dyn_crypto_data(ctx, argv[1], &msg, &msglen, &o2))
        goto done;
    if (keylen != CURVE_SEED) {
        JS_ThrowTypeError(ctx, "Ed25519Sign: private key must be 32 bytes");
        goto done;
    }
    k = curve_key(ctx, EVP_PKEY_ED25519, 1, key, keylen, "Ed25519Sign");
    if (!k) goto done;
    md = EVP_MD_CTX_new();
    /* Ed25519 is one-shot by construction: there is no streaming DigestSign
       update for it, which is why this takes the whole message. */
    if (!md || EVP_DigestSignInit(md, NULL, NULL, NULL, k) <= 0 ||
        EVP_DigestSign(md, sig, &siglen, msg, msglen) <= 0) {
        ERR_clear_error();
        JS_ThrowInternalError(ctx, "Ed25519Sign: signing failed");
        goto done;
    }
    ret = dyn_crypto_u8array(ctx, sig, siglen);
done:
    if (md) EVP_MD_CTX_free(md);
    if (k) EVP_PKEY_free(k);
    if (o1) JS_FreeCString(ctx, o1);
    if (o2) JS_FreeCString(ctx, o2);
    return ret;
}

/* Ed25519Verify(publicKey, message, signature) -> boolean. */
static JSValue dyn_ed25519_verify(JSContext *ctx, JSValueConst t, int argc,
                                  JSValueConst *argv)
{
    const uint8_t *key = NULL, *msg = NULL, *sig = NULL;
    size_t keylen = 0, msglen = 0, siglen = 0;
    const char *o1 = NULL, *o2 = NULL, *o3 = NULL;
    EVP_PKEY *k = NULL;
    EVP_MD_CTX *md = NULL;
    JSValue ret = JS_EXCEPTION;
    int good = 0;
    (void)t;

    if (argc < 3)
        return JS_ThrowTypeError(ctx, "Ed25519Verify(publicKey, message, signature)");
    if (dyn_crypto_data(ctx, argv[0], &key, &keylen, &o1))
        return JS_EXCEPTION;
    if (dyn_crypto_data(ctx, argv[1], &msg, &msglen, &o2))
        goto done;
    if (dyn_crypto_data(ctx, argv[2], &sig, &siglen, &o3))
        goto done;
    /* A wrong-size key is a caller error and throws; a wrong-size SIGNATURE is
       just a bad signature and returns false, because that is what an attacker
       supplies and it must not be distinguishable from any other forgery. */
    if (keylen != CURVE_SEED) {
        JS_ThrowTypeError(ctx, "Ed25519Verify: public key must be 32 bytes");
        goto done;
    }
    if (siglen != ED_SIG) { ret = JS_NewBool(ctx, 0); goto done; }
    k = curve_key(ctx, EVP_PKEY_ED25519, 0, key, keylen, "Ed25519Verify");
    if (!k) goto done;
    md = EVP_MD_CTX_new();
    if (md && EVP_DigestVerifyInit(md, NULL, NULL, NULL, k) > 0)
        good = EVP_DigestVerify(md, sig, siglen, msg, msglen) == 1;
    ERR_clear_error();
    ret = JS_NewBool(ctx, good);
done:
    if (md) EVP_MD_CTX_free(md);
    if (k) EVP_PKEY_free(k);
    if (o1) JS_FreeCString(ctx, o1);
    if (o2) JS_FreeCString(ctx, o2);
    if (o3) JS_FreeCString(ctx, o3);
    return ret;
}

/* X25519Derive(privateKey, peerPublicKey) -> 32-byte shared secret. */
static JSValue dyn_x25519_derive(JSContext *ctx, JSValueConst t, int argc,
                                 JSValueConst *argv)
{
    const uint8_t *priv = NULL, *peer = NULL;
    size_t privlen = 0, peerlen = 0, outlen = CURVE_SEED;
    const char *o1 = NULL, *o2 = NULL;
    EVP_PKEY *kp = NULL, *kq = NULL;
    EVP_PKEY_CTX *dc = NULL;
    uint8_t out[CURVE_SEED];
    JSValue ret = JS_EXCEPTION;
    (void)t;

    if (argc < 2)
        return JS_ThrowTypeError(ctx, "X25519Derive(privateKey, peerPublicKey)");
    if (dyn_crypto_data(ctx, argv[0], &priv, &privlen, &o1))
        return JS_EXCEPTION;
    if (dyn_crypto_data(ctx, argv[1], &peer, &peerlen, &o2))
        goto done;
    if (privlen != CURVE_SEED || peerlen != CURVE_SEED) {
        JS_ThrowTypeError(ctx, "X25519Derive: both keys must be 32 bytes");
        goto done;
    }
    kp = curve_key(ctx, EVP_PKEY_X25519, 1, priv, privlen, "X25519Derive");
    if (!kp) goto done;
    kq = curve_key(ctx, EVP_PKEY_X25519, 0, peer, peerlen, "X25519Derive");
    if (!kq) goto done;
    dc = EVP_PKEY_CTX_new(kp, NULL);
    /* An all-zero shared secret means a small-order peer point. OpenSSL
       reports that as a derive FAILURE rather than handing it back, and it
       must stay a failure -- a caller who used it would have no secret. */
    if (!dc || EVP_PKEY_derive_init(dc) <= 0 ||
        EVP_PKEY_derive_set_peer(dc, kq) <= 0 ||
        EVP_PKEY_derive(dc, out, &outlen) <= 0) {
        ERR_clear_error();
        JS_ThrowTypeError(ctx, "X25519Derive: derivation failed "
                               "(a small-order peer key is refused)");
        goto done;
    }
    ret = dyn_crypto_u8array(ctx, out, outlen);
done:
    memset(out, 0, sizeof out);
    if (dc) EVP_PKEY_CTX_free(dc);
    if (kq) EVP_PKEY_free(kq);
    if (kp) EVP_PKEY_free(kp);
    if (o1) JS_FreeCString(ctx, o1);
    if (o2) JS_FreeCString(ctx, o2);
    return ret;
}

#endif /* CONFIG_TLS */

/* ---- Bcrypt (OpenBSD $2b$) ---------------------------------------------
 * A faithful port of OpenBSD's bcrypt.c / blowfish.c (Provos, Mazieres,
 * Unangst; ISC-style). Diffed against OpenBSD blowfish.c v1.21 and
 * bcrypt.c v1.58: same pi-digit tables, same key-expansion order, same
 * 23-of-24-byte base64 output encoding. KATs are py-bcrypt / Go x/crypto
 * vectors, cross-checked against OpenBSD semantics.
 *
 * Deliberate divergences from OpenBSD, both fail-closed:
 *  - hash() REFUSES a password over 72 bytes rather than truncating it
 *    silently (a truncated password changes what was hashed without error).
 *  - verify() caps the cost it will compute at DYN_BCRYPT_MAX_VERIFY_COST:
 *    the hash string is UNTRUSTED input, and cost 31 in a stored hash is a
 *    multi-year CPU bill an attacker can hand us. hash() still accepts the
 *    full 4..31 range per the API contract (there the caller pays).
 */

#define BLF_N 16

typedef struct {
    uint32_t S[4][256];
    uint32_t P[BLF_N + 2];
} dyn_blf_ctx;

#define DYN_BCRYPT_MIN_COST 4
#define DYN_BCRYPT_MAX_COST 31
/* Verify is fed attacker-controlled hash strings; computing a legit-looking
 * but cost-31 hash would peg the CPU for years. 2^20 rounds ~ a second.
 * Everything hash() emits with cost <= 20 verifies. */
#define DYN_BCRYPT_MAX_VERIFY_COST 20

static const char dyn_bcrypt_b64[] =
    "./ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";

static const uint8_t dyn_bcrypt_b64_inv[128] = {
    255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
    255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
    255,255,255,255,255,255,255,255,255,255,255,255,255,255,  0,  1,
     54, 55, 56, 57, 58, 59, 60, 61, 62, 63,255,255,255,255,255,255,
    255,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14, 15, 16,
     17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27,255,255,255,255,255,
    255, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42,
     43, 44, 45, 46, 47, 48, 49, 50, 51, 52, 53,255,255,255,255,255,
};
#define DYN_B64_INV(c) (((c) > 127) ? 255 : dyn_bcrypt_b64_inv[(c)])

/* bcrypt's base64 is bit-reversed relative to RFC 4648 and unpadded: a group
 * emits the LOW 6 bits first, and a trailing 1- or 2-byte group emits 2 or 3
 * chars with no padding. Both routines are the OpenBSD originals, including
 * the partial final group that reads only the chars it needs. */
static int dyn_bcrypt_b64_decode(uint8_t *out, size_t outlen, const char *b64)
{
    uint8_t *bp = out;
    const uint8_t *p = (const uint8_t *)b64;
    while (bp < out + outlen) {
        uint8_t c1 = DYN_B64_INV(*p);
        uint8_t c2, c3, c4;
        if (c1 == 255) return -1;
        c2 = DYN_B64_INV(*(p + 1));
        if (c2 == 255) return -1;
        *bp++ = (uint8_t)((c1 << 2) | ((c2 & 0x30) >> 4));
        if (bp >= out + outlen) break;
        c3 = DYN_B64_INV(*(p + 2));
        if (c3 == 255) return -1;
        *bp++ = (uint8_t)(((c2 & 0x0f) << 4) | ((c3 & 0x3c) >> 2));
        if (bp >= out + outlen) break;
        c4 = DYN_B64_INV(*(p + 3));
        if (c4 == 255) return -1;
        *bp++ = (uint8_t)(((c3 & 0x03) << 6) | c4);
        p += 4;
    }
    return 0;
}

static void dyn_bcrypt_b64_encode(char *out, const uint8_t *data, size_t len)
{
    uint8_t *bp = (uint8_t *)out;
    const uint8_t *p = data;
    while (p < data + len) {
        uint8_t c1 = *p++;
        *bp++ = (uint8_t)dyn_bcrypt_b64[c1 >> 2];
        c1 = (uint8_t)((c1 & 0x03) << 4);
        if (p >= data + len) { *bp++ = (uint8_t)dyn_bcrypt_b64[c1]; break; }
        {
            uint8_t c2 = *p++;
            c1 |= (uint8_t)((c2 >> 4) & 0x0f);
            *bp++ = (uint8_t)dyn_bcrypt_b64[c1];
            c1 = (uint8_t)((c2 & 0x0f) << 2);
            if (p >= data + len) { *bp++ = (uint8_t)dyn_bcrypt_b64[c1]; break; }
            {
                uint8_t c3 = *p++;
                c1 |= (uint8_t)((c3 >> 6) & 0x03);
                *bp++ = (uint8_t)dyn_bcrypt_b64[c1];
                *bp++ = (uint8_t)dyn_bcrypt_b64[c3 & 0x3f];
            }
        }
    }
    *bp = '\0';
}

static void dyn_blowfish_initstate(dyn_blf_ctx *c)
{
    /* P-box and S-box tables initialized with digits of Pi (verbatim from
     * OpenBSD blowfish.c v1.21). */
    static const dyn_blf_ctx initstate = { {
        { /* S[0] */
            0xd1310ba6,0x98dfb5ac,0x2ffd72db,0xd01adfb7,0xb8e1afed,0x6a267e96,
            0xba7c9045,0xf12c7f99,0x24a19947,0xb3916cf7,0x0801f2e2,0x858efc16,
            0x636920d8,0x71574e69,0xa458fea3,0xf4933d7e,0x0d95748f,0x728eb658,
            0x718bcd58,0x82154aee,0x7b54a41d,0xc25a59b5,0x9c30d539,0x2af26013,
            0xc5d1b023,0x286085f0,0xca417918,0xb8db38ef,0x8e79dcb0,0x603a180e,
            0x6c9e0e8b,0xb01e8a3e,0xd71577c1,0xbd314b27,0x78af2fda,0x55605c60,
            0xe65525f3,0xaa55ab94,0x57489862,0x63e81440,0x55ca396a,0x2aab10b6,
            0xb4cc5c34,0x1141e8ce,0xa15486af,0x7c72e993,0xb3ee1411,0x636fbc2a,
            0x2ba9c55d,0x741831f6,0xce5c3e16,0x9b87931e,0xafd6ba33,0x6c24cf5c,
            0x7a325381,0x28958677,0x3b8f4898,0x6b4bb9af,0xc4bfe81b,0x66282193,
            0x61d809cc,0xfb21a991,0x487cac60,0x5dec8032,0xef845d5d,0xe98575b1,
            0xdc262302,0xeb651b88,0x23893e81,0xd396acc5,0x0f6d6ff3,0x83f44239,
            0x2e0b4482,0xa4842004,0x69c8f04a,0x9e1f9b5e,0x21c66842,0xf6e96c9a,
            0x670c9c61,0xabd388f0,0x6a51a0d2,0xd8542f68,0x960fa728,0xab5133a3,
            0x6eef0b6c,0x137a3be4,0xba3bf050,0x7efb2a98,0xa1f1651d,0x39af0176,
            0x66ca593e,0x82430e88,0x8cee8619,0x456f9fb4,0x7d84a5c3,0x3b8b5ebe,
            0xe06f75d8,0x85c12073,0x401a449f,0x56c16aa6,0x4ed3aa62,0x363f7706,
            0x1bfedf72,0x429b023d,0x37d0d724,0xd00a1248,0xdb0fead3,0x49f1c09b,
            0x075372c9,0x80991b7b,0x25d479d8,0xf6e8def7,0xe3fe501a,0xb6794c3b,
            0x976ce0bd,0x04c006ba,0xc1a94fb6,0x409f60c4,0x5e5c9ec2,0x196a2463,
            0x68fb6faf,0x3e6c53b5,0x1339b2eb,0x3b52ec6f,0x6dfc511f,0x9b30952c,
            0xcc814544,0xaf5ebd09,0xbee3d004,0xde334afd,0x660f2807,0x192e4bb3,
            0xc0cba857,0x45c8740f,0xd20b5f39,0xb9d3fbdb,0x5579c0bd,0x1a60320a,
            0xd6a100c6,0x402c7279,0x679f25fe,0xfb1fa3cc,0x8ea5e9f8,0xdb3222f8,
            0x3c7516df,0xfd616b15,0x2f501ec8,0xad0552ab,0x323db5fa,0xfd238760,
            0x53317b48,0x3e00df82,0x9e5c57bb,0xca6f8ca0,0x1a87562e,0xdf1769db,
            0xd542a8f6,0x287effc3,0xac6732c6,0x8c4f5573,0x695b27b0,0xbbca58c8,
            0xe1ffa35d,0xb8f011a0,0x10fa3d98,0xfd2183b8,0x4afcb56c,0x2dd1d35b,
            0x9a53e479,0xb6f84565,0xd28e49bc,0x4bfb9790,0xe1ddf2da,0xa4cb7e33,
            0x62fb1341,0xcee4c6e8,0xef20cada,0x36774c01,0xd07e9efe,0x2bf11fb4,
            0x95dbda4d,0xae909198,0xeaad8e71,0x6b93d5a0,0xd08ed1d0,0xafc725e0,
            0x8e3c5b2f,0x8e7594b7,0x8ff6e2fb,0xf2122b64,0x8888b812,0x900df01c,
            0x4fad5ea0,0x688fc31c,0xd1cff191,0xb3a8c1ad,0x2f2f2218,0xbe0e1777,
            0xea752dfe,0x8b021fa1,0xe5a0cc0f,0xb56f74e8,0x18acf3d6,0xce89e299,
            0xb4a84fe0,0xfd13e0b7,0x7cc43b81,0xd2ada8d9,0x165fa266,0x80957705,
            0x93cc7314,0x211a1477,0xe6ad2065,0x77b5fa86,0xc75442f5,0xfb9d35cf,
            0xebcdaf0c,0x7b3e89a0,0xd6411bd3,0xae1e7e49,0x00250e2d,0x2071b35e,
            0x226800bb,0x57b8e0af,0x2464369b,0xf009b91e,0x5563911d,0x59dfa6aa,
            0x78c14389,0xd95a537f,0x207d5ba2,0x02e5b9c5,0x83260376,0x6295cfa9,
            0x11c81968,0x4e734a41,0xb3472dca,0x7b14a94a,0x1b510052,0x9a532915,
            0xd60f573f,0xbc9bc6e4,0x2b60a476,0x81e67400,0x08ba6fb5,0x571be91f,
            0xf296ec6b,0x2a0dd915,0xb6636521,0xe7b9f9b6,0xff34052e,0xc5855664,
            0x53b02d5d,0xa99f8fa1,0x08ba4799,0x6e85076a
        },
        { /* S[1] */
            0x4b7a70e9,0xb5b32944,0xdb75092e,0xc4192623,0xad6ea6b0,0x49a7df7d,
            0x9cee60b8,0x8fedb266,0xecaa8c71,0x699a17ff,0x5664526c,0xc2b19ee1,
            0x193602a5,0x75094c29,0xa0591340,0xe4183a3e,0x3f54989a,0x5b429d65,
            0x6b8fe4d6,0x99f73fd6,0xa1d29c07,0xefe830f5,0x4d2d38e6,0xf0255dc1,
            0x4cdd2086,0x8470eb26,0x6382e9c6,0x021ecc5e,0x09686b3f,0x3ebaefc9,
            0x3c971814,0x6b6a70a1,0x687f3584,0x52a0e286,0xb79c5305,0xaa500737,
            0x3e07841c,0x7fdeae5c,0x8e7d44ec,0x5716f2b8,0xb03ada37,0xf0500c0d,
            0xf01c1f04,0x0200b3ff,0xae0cf51a,0x3cb574b2,0x25837a58,0xdc0921bd,
            0xd19113f9,0x7ca92ff6,0x94324773,0x22f54701,0x3ae5e581,0x37c2dadc,
            0xc8b57634,0x9af3dda7,0xa9446146,0x0fd0030e,0xecc8c73e,0xa4751e41,
            0xe238cd99,0x3bea0e2f,0x3280bba1,0x183eb331,0x4e548b38,0x4f6db908,
            0x6f420d03,0xf60a04bf,0x2cb81290,0x24977c79,0x5679b072,0xbcaf89af,
            0xde9a771f,0xd9930810,0xb38bae12,0xdccf3f2e,0x5512721f,0x2e6b7124,
            0x501adde6,0x9f84cd87,0x7a584718,0x7408da17,0xbc9f9abc,0xe94b7d8c,
            0xec7aec3a,0xdb851dfa,0x63094366,0xc464c3d2,0xef1c1847,0x3215d908,
            0xdd433b37,0x24c2ba16,0x12a14d43,0x2a65c451,0x50940002,0x133ae4dd,
            0x71dff89e,0x10314e55,0x81ac77d6,0x5f11199b,0x043556f1,0xd7a3c76b,
            0x3c11183b,0x5924a509,0xf28fe6ed,0x97f1fbfa,0x9ebabf2c,0x1e153c6e,
            0x86e34570,0xeae96fb1,0x860e5e0a,0x5a3e2ab3,0x771fe71c,0x4e3d06fa,
            0x2965dcb9,0x99e71d0f,0x803e89d6,0x5266c825,0x2e4cc978,0x9c10b36a,
            0xc6150eba,0x94e2ea78,0xa5fc3c53,0x1e0a2df4,0xf2f74ea7,0x361d2b3d,
            0x1939260f,0x19c27960,0x5223a708,0xf71312b6,0xebadfe6e,0xeac31f66,
            0xe3bc4595,0xa67bc883,0xb17f37d1,0x018cff28,0xc332ddef,0xbe6c5aa5,
            0x65582185,0x68ab9802,0xeecea50f,0xdb2f953b,0x2aef7dad,0x5b6e2f84,
            0x1521b628,0x29076170,0xecdd4775,0x619f1510,0x13cca830,0xeb61bd96,
            0x0334fe1e,0xaa0363cf,0xb5735c90,0x4c70a239,0xd59e9e0b,0xcbaade14,
            0xeecc86bc,0x60622ca7,0x9cab5cab,0xb2f3846e,0x648b1eaf,0x19bdf0ca,
            0xa02369b9,0x655abb50,0x40685a32,0x3c2ab4b3,0x319ee9d5,0xc021b8f7,
            0x9b540b19,0x875fa099,0x95f7997e,0x623d7da8,0xf837889a,0x97e32d77,
            0x11ed935f,0x16681281,0x0e358829,0xc7e61fd6,0x96dedfa1,0x7858ba99,
            0x57f584a5,0x1b227263,0x9b83c3ff,0x1ac24696,0xcdb30aeb,0x532e3054,
            0x8fd948e4,0x6dbc3128,0x58ebf2ef,0x34c6ffea,0xfe28ed61,0xee7c3c73,
            0x5d4a14d9,0xe864b7e3,0x42105d14,0x203e13e0,0x45eee2b6,0xa3aaabea,
            0xdb6c4f15,0xfacb4fd0,0xc742f442,0xef6abbb5,0x654f3b1d,0x41cd2105,
            0xd81e799e,0x86854dc7,0xe44b476a,0x3d816250,0xcf62a1f2,0x5b8d2646,
            0xfc8883a0,0xc1c7b6a3,0x7f1524c3,0x69cb7492,0x47848a0b,0x5692b285,
            0x095bbf00,0xad19489d,0x1462b174,0x23820e00,0x58428d2a,0x0c55f5ea,
            0x1dadf43e,0x233f7061,0x3372f092,0x8d937e41,0xd65fecf1,0x6c223bdb,
            0x7cde3759,0xcbee7460,0x4085f2a7,0xce77326e,0xa6078084,0x19f8509e,
            0xe8efd855,0x61d99735,0xa969a7aa,0xc50c06c2,0x5a04abfc,0x800bcadc,
            0x9e447a2e,0xc3453484,0xfdd56705,0x0e1e9ec9,0xdb73dbd3,0x105588cd,
            0x675fda79,0xe3674340,0xc5c43465,0x713e38d8,0x3d28f89e,0xf16dff20,
            0x153e21e7,0x8fb03d4a,0xe6e39f2b,0xdb83adf7
        },
        { /* S[2] */
            0xe93d5a68,0x948140f7,0xf64c261c,0x94692934,0x411520f7,0x7602d4f7,
            0xbcf46b2e,0xd4a20068,0xd4082471,0x3320f46a,0x43b7d4b7,0x500061af,
            0x1e39f62e,0x97244546,0x14214f74,0xbf8b8840,0x4d95fc1d,0x96b591af,
            0x70f4ddd3,0x66a02f45,0xbfbc09ec,0x03bd9785,0x7fac6dd0,0x31cb8504,
            0x96eb27b3,0x55fd3941,0xda2547e6,0xabca0a9a,0x28507825,0x530429f4,
            0x0a2c86da,0xe9b66dfb,0x68dc1462,0xd7486900,0x680ec0a4,0x27a18dee,
            0x4f3ffea2,0xe887ad8c,0xb58ce006,0x7af4d6b6,0xaace1e7c,0xd3375fec,
            0xce78a399,0x406b2a42,0x20fe9e35,0xd9f385b9,0xee39d7ab,0x3b124e8b,
            0x1dc9faf7,0x4b6d1856,0x26a36631,0xeae397b2,0x3a6efa74,0xdd5b4332,
            0x6841e7f7,0xca7820fb,0xfb0af54e,0xd8feb397,0x454056ac,0xba489527,
            0x55533a3a,0x20838d87,0xfe6ba9b7,0xd096954b,0x55a867bc,0xa1159a58,
            0xcca92963,0x99e1db33,0xa62a4a56,0x3f3125f9,0x5ef47e1c,0x9029317c,
            0xfdf8e802,0x04272f70,0x80bb155c,0x05282ce3,0x95c11548,0xe4c66d22,
            0x48c1133f,0xc70f86dc,0x07f9c9ee,0x41041f0f,0x404779a4,0x5d886e17,
            0x325f51eb,0xd59bc0d1,0xf2bcc18f,0x41113564,0x257b7834,0x602a9c60,
            0xdff8e8a3,0x1f636c1b,0x0e12b4c2,0x02e1329e,0xaf664fd1,0xcad18115,
            0x6b2395e0,0x333e92e1,0x3b240b62,0xeebeb922,0x85b2a20e,0xe6ba0d99,
            0xde720c8c,0x2da2f728,0xd0127845,0x95b794fd,0x647d0862,0xe7ccf5f0,
            0x5449a36f,0x877d48fa,0xc39dfd27,0xf33e8d1e,0x0a476341,0x992eff74,
            0x3a6f6eab,0xf4f8fd37,0xa812dc60,0xa1ebddf8,0x991be14c,0xdb6e6b0d,
            0xc67b5510,0x6d672c37,0x2765d43b,0xdcd0e804,0xf1290dc7,0xcc00ffa3,
            0xb5390f92,0x690fed0b,0x667b9ffb,0xcedb7d9c,0xa091cf0b,0xd9155ea3,
            0xbb132f88,0x515bad24,0x7b9479bf,0x763bd6eb,0x37392eb3,0xcc115979,
            0x8026e297,0xf42e312d,0x6842ada7,0xc66a2b3b,0x12754ccc,0x782ef11c,
            0x6a124237,0xb79251e7,0x06a1bbe6,0x4bfb6350,0x1a6b1018,0x11caedfa,
            0x3d25bdd8,0xe2e1c3c9,0x44421659,0x0a121386,0xd90cec6e,0xd5abea2a,
            0x64af674e,0xda86a85f,0xbebfe988,0x64e4c3fe,0x9dbc8057,0xf0f7c086,
            0x60787bf8,0x6003604d,0xd1fd8346,0xf6381fb0,0x7745ae04,0xd736fccc,
            0x83426b33,0xf01eab71,0xb0804187,0x3c005e5f,0x77a057be,0xbde8ae24,
            0x55464299,0xbf582e61,0x4e58f48f,0xf2ddfda2,0xf474ef38,0x8789bdc2,
            0x5366f9c3,0xc8b38e74,0xb475f255,0x46fcd9b9,0x7aeb2661,0x8b1ddf84,
            0x846a0e79,0x915f95e2,0x466e598e,0x20b45770,0x8cd55591,0xc902de4c,
            0xb90bace1,0xbb8205d0,0x11a86248,0x7574a99e,0xb77f19b6,0xe0a9dc09,
            0x662d09a1,0xc4324633,0xe85a1f02,0x09f0be8c,0x4a99a025,0x1d6efe10,
            0x1ab93d1d,0x0ba5a4df,0xa186f20f,0x2868f169,0xdcb7da83,0x573906fe,
            0xa1e2ce9b,0x4fcd7f52,0x50115e01,0xa70683fa,0xa002b5c4,0x0de6d027,
            0x9af88c27,0x773f8641,0xc3604c06,0x61a806b5,0xf0177a28,0xc0f586e0,
            0x006058aa,0x30dc7d62,0x11e69ed7,0x2338ea63,0x53c2dd94,0xc2c21634,
            0xbbcbee56,0x90bcb6de,0xebfc7da1,0xce591d76,0x6f05e409,0x4b7c0188,
            0x39720a3d,0x7c927c24,0x86e3725f,0x724d9db9,0x1ac15bb4,0xd39eb8fc,
            0xed545578,0x08fca5b5,0xd83d7cd3,0x4dad0fc4,0x1e50ef5e,0xb161e6f8,
            0xa28514d9,0x6c51133c,0x6fd5c7e7,0x56e14ec4,0x362abfce,0xddc6c837,
            0xd79a3234,0x92638212,0x670efa8e,0x406000e0
        },
        { /* S[3] */
            0x3a39ce37,0xd3faf5cf,0xabc27737,0x5ac52d1b,0x5cb0679e,0x4fa33742,
            0xd3822740,0x99bc9bbe,0xd5118e9d,0xbf0f7315,0xd62d1c7e,0xc700c47b,
            0xb78c1b6b,0x21a19045,0xb26eb1be,0x6a366eb4,0x5748ab2f,0xbc946e79,
            0xc6a376d2,0x6549c2c8,0x530ff8ee,0x468dde7d,0xd5730a1d,0x4cd04dc6,
            0x2939bbdb,0xa9ba4650,0xac9526e8,0xbe5ee304,0xa1fad5f0,0x6a2d519a,
            0x63ef8ce2,0x9a86ee22,0xc089c2b8,0x43242ef6,0xa51e03aa,0x9cf2d0a4,
            0x83c061ba,0x9be96a4d,0x8fe51550,0xba645bd6,0x2826a2f9,0xa73a3ae1,
            0x4ba99586,0xef5562e9,0xc72fefd3,0xf752f7da,0x3f046f69,0x77fa0a59,
            0x80e4a915,0x87b08601,0x9b09e6ad,0x3b3ee593,0xe990fd5a,0x9e34d797,
            0x2cf0b7d9,0x022b8b51,0x96d5ac3a,0x017da67d,0xd1cf3ed6,0x7c7d2d28,
            0x1f9f25cf,0xadf2b89b,0x5ad6b472,0x5a88f54c,0xe029ac71,0xe019a5e6,
            0x47b0acfd,0xed93fa9b,0xe8d3c48d,0x283b57cc,0xf8d56629,0x79132e28,
            0x785f0191,0xed756055,0xf7960e44,0xe3d35e8c,0x15056dd4,0x88f46dba,
            0x03a16125,0x0564f0bd,0xc3eb9e15,0x3c9057a2,0x97271aec,0xa93a072a,
            0x1b3f6d9b,0x1e6321f5,0xf59c66fb,0x26dcf319,0x7533d928,0xb155fdf5,
            0x03563482,0x8aba3cbb,0x28517711,0xc20ad9f8,0xabcc5167,0xccad925f,
            0x4de81751,0x3830dc8e,0x379d5862,0x9320f991,0xea7a90c2,0xfb3e7bce,
            0x5121ce64,0x774fbe32,0xa8b6e37e,0xc3293d46,0x48de5369,0x6413e680,
            0xa2ae0810,0xdd6db224,0x69852dfd,0x09072166,0xb39a460a,0x6445c0dd,
            0x586cdecf,0x1c20c8ae,0x5bbef7dd,0x1b588d40,0xccd2017f,0x6bb4e3bb,
            0xdda26a7e,0x3a59ff45,0x3e350a44,0xbcb4cdd5,0x72eacea8,0xfa6484bb,
            0x8d6612ae,0xbf3c6f47,0xd29be463,0x542f5d9e,0xaec2771b,0xf64e6370,
            0x740e0d8d,0xe75b1357,0xf8721671,0xaf537d5d,0x4040cb08,0x4eb4e2cc,
            0x34d2466a,0x0115af84,0xe1b00428,0x95983a1d,0x06b89fb4,0xce6ea048,
            0x6f3f3b82,0x3520ab82,0x011a1d4b,0x277227f8,0x611560b1,0xe7933fdc,
            0xbb3a792b,0x344525bd,0xa08839e1,0x51ce794b,0x2f32c9b7,0xa01fbac9,
            0xe01cc87e,0xbcc7d1f6,0xcf0111c3,0xa1e8aac7,0x1a908749,0xd44fbd9a,
            0xd0dadecb,0xd50ada38,0x0339c32a,0xc6913667,0x8df9317c,0xe0b12b4f,
            0xf79e59b7,0x43f5bb3a,0xf2d519ff,0x27d9459c,0xbf97222c,0x15e6fc2a,
            0x0f91fc71,0x9b941525,0xfae59361,0xceb69ceb,0xc2a86459,0x12baa8d1,
            0xb6c1075e,0xe3056a0c,0x10d25065,0xcb03a442,0xe0ec6e0e,0x1698db3b,
            0x4c98a0be,0x3278e964,0x9f1f9532,0xe0d392df,0xd3a0342b,0x8971f21e,
            0x1b0a7441,0x4ba3348c,0xc5be7120,0xc37632d8,0xdf359f8d,0x9b992f2e,
            0xe60b6f47,0x0fe3f11d,0xe54cda54,0x1edad891,0xce6279cf,0xcd3e7e6f,
            0x1618b166,0xfd2c1d05,0x848fd2c5,0xf6fb2299,0xf523f357,0xa6327623,
            0x93a83531,0x56cccd02,0xacf08162,0x5a75ebb5,0x6e163697,0x88d273cc,
            0xde966292,0x81b949d0,0x4c50901b,0x71c65614,0xe6c6c7bd,0x327a140a,
            0x45e1d006,0xc3f27b9a,0xc9aa53fd,0x62a80f00,0xbb25bfe2,0x35bdd2f6,
            0x71126905,0xb2040222,0xb6cbcf7c,0xcd769c2b,0x53113ec0,0x1640e3d3,
            0x38abbd60,0x2547adf0,0xba38209c,0xf746ce76,0x77afa1c5,0x20756060,
            0x85cbfe4e,0x8ae88dd8,0x7aaaf9b0,0x4cf9aa7e,0x1948c25c,0x02fb8a8c,
            0x01c36ae4,0xd6ebe1f9,0x90d4f869,0xa65cdea0,0x3f09252d,0xc208e69f,
            0xb74e6132,0xce77e25b,0x578fdfe3,0x3ac372e6
        }
    }, {
        /* P-array */
        0x243f6a88,0x85a308d3,0x13198a2e,0x03707344,0xa4093822,0x299f31d0,
        0x082efa98,0xec4e6c89,0x452821e6,0x38d01377,0xbe5466cf,0x34e90c6c,
        0xc0ac29b7,0xc97c50dd,0x3f84d5b5,0xb5470917,0x9216d5d9,0x8979fb1b
    } };

    *c = initstate;
}

#define DYN_F(s, x) ((((s)[(((x)>>24)&0xFF)] + (s)[0x100 + (((x)>>16)&0xFF)]) \
                      ^ (s)[0x200 + (((x)>> 8)&0xFF)]) + (s)[0x300 + ((x)&0xFF)])

#define DYN_BLFRND(s,p,i,j,n) (i ^= DYN_F(s,j) ^ (p)[n])

static void dyn_blowfish_encipher(dyn_blf_ctx *c, uint32_t *xl, uint32_t *xr)
{
    uint32_t Xl = *xl, Xr = *xr;
    uint32_t *s = c->S[0], *p = c->P;
    Xl ^= p[0];
    DYN_BLFRND(s, p, Xr, Xl,  1); DYN_BLFRND(s, p, Xl, Xr,  2);
    DYN_BLFRND(s, p, Xr, Xl,  3); DYN_BLFRND(s, p, Xl, Xr,  4);
    DYN_BLFRND(s, p, Xr, Xl,  5); DYN_BLFRND(s, p, Xl, Xr,  6);
    DYN_BLFRND(s, p, Xr, Xl,  7); DYN_BLFRND(s, p, Xl, Xr,  8);
    DYN_BLFRND(s, p, Xr, Xl,  9); DYN_BLFRND(s, p, Xl, Xr, 10);
    DYN_BLFRND(s, p, Xr, Xl, 11); DYN_BLFRND(s, p, Xl, Xr, 12);
    DYN_BLFRND(s, p, Xr, Xl, 13); DYN_BLFRND(s, p, Xl, Xr, 14);
    DYN_BLFRND(s, p, Xr, Xl, 15); DYN_BLFRND(s, p, Xl, Xr, 16);
    *xl = Xr ^ p[17];
    *xr = Xl;
}

static uint32_t dyn_blowfish_stream2word(const uint8_t *data, uint16_t databytes,
                                         uint16_t *current)
{
    uint8_t i;
    uint16_t j = *current;
    uint32_t temp = 0;
    for (i = 0; i < 4; i++, j++) {
        if (j >= databytes) j = 0;
        temp = (temp << 8) | data[j];
    }
    *current = j;
    return temp;
}

/* P[i] ^= key-word[i], then 521 encipherings of (0,0). The OpenBSD rounds
 * alternate expand0state(key) then expand0state(salt). */
static void dyn_blowfish_expand0state(dyn_blf_ctx *c, const uint8_t *key,
                                      uint16_t keybytes)
{
    uint16_t i, k, j = 0;
    uint32_t datal = 0, datar = 0;
    for (i = 0; i < BLF_N + 2; i++)
        c->P[i] ^= dyn_blowfish_stream2word(key, keybytes, &j);
    for (i = 0; i < BLF_N + 2; i += 2) {
        dyn_blowfish_encipher(c, &datal, &datar);
        c->P[i] = datal; c->P[i + 1] = datar;
    }
    for (i = 0; i < 4; i++)
        for (k = 0; k < 256; k += 2) {
            dyn_blowfish_encipher(c, &datal, &datar);
            c->S[i][k] = datal; c->S[i][k + 1] = datar;
        }
}

/* The first-pass expansion: key XORed into P, salt interleaved into the
 * enciphering stream. Argument order is (salt, key), matching OpenBSD.
 *
 * THE SALT STREAM RESTARTS AT 0, NOT AT WHERE THE KEY STREAM ENDED: OpenBSD
 * resets j between the two phases (blowfish.c v1.21). A port that carries j
 * across phases hashes every password with a shifted salt and silently
 * disagrees with every other bcrypt implementation -- the KATs catch it. */
static void dyn_blowfish_expandstate(dyn_blf_ctx *c, const uint8_t *data,
                                     uint16_t databytes, const uint8_t *key,
                                     uint16_t keybytes)
{
    uint16_t i, k, j;
    uint32_t datal, datar;
    j = 0;
    datal = datar = 0;
    for (i = 0; i < BLF_N + 2; i++)
        c->P[i] ^= dyn_blowfish_stream2word(key, keybytes, &j);
    j = 0;                                  /* restart the salt stream */
    for (i = 0; i < BLF_N + 2; i += 2) {
        datal ^= dyn_blowfish_stream2word(data, databytes, &j);
        datar ^= dyn_blowfish_stream2word(data, databytes, &j);
        dyn_blowfish_encipher(c, &datal, &datar);
        c->P[i] = datal; c->P[i + 1] = datar;
    }
    for (i = 0; i < 4; i++)
        for (k = 0; k < 256; k += 2) {
            datal ^= dyn_blowfish_stream2word(data, databytes, &j);
            datar ^= dyn_blowfish_stream2word(data, databytes, &j);
            dyn_blowfish_encipher(c, &datal, &datar);
            c->S[i][k] = datal; c->S[i][k + 1] = datar;
        }
}

static void dyn_blowfish_blf_enc(dyn_blf_ctx *c, uint32_t *data, uint16_t blocks)
{
    while (blocks--) {
        dyn_blowfish_encipher(c, data, data + 1);
        data += 2;
    }
}

/* The core: key (<= 72 bytes, NUL appended -- 'b' semantics), 16-byte raw
 * salt, cost logr. Writes the 60-char "$2b$CC$<22 salt><31 hash>" string.
 * Returns 0, or -1 if logr is out of range. */
static int dyn_bcrypt_core(const uint8_t *key, size_t keylen,
                           const uint8_t csalt[16], uint32_t logr, char *out)
{
    dyn_blf_ctx state;
    uint8_t ckey[73], ciphertext[24];
    uint32_t cdata[6], rounds, i, k;
    uint16_t j;
    size_t kl = keylen > 72 ? 72 : keylen;

    if (logr < DYN_BCRYPT_MIN_COST || logr > DYN_BCRYPT_MAX_COST)
        return -1;
    memcpy(ckey, key, kl);
    ckey[kl] = 0;                       /* the NUL is part of the key */
    /* memcpy, NOT a string-literal initializer: the 24-char literal would
     * have no room for its NUL, and "initializer-string too long" is a
     * warning that fails the zero-warning gate. */
    memcpy(ciphertext, "OrpheanBeholderScryDoubt", 24);
    dyn_blowfish_initstate(&state);
    dyn_blowfish_expandstate(&state, csalt, 16, ckey, (uint16_t)(kl + 1));
    rounds = 1U << logr;
    for (k = 0; k < rounds; k++) {
        dyn_blowfish_expand0state(&state, ckey, (uint16_t)(kl + 1));
        dyn_blowfish_expand0state(&state, csalt, 16);
    }
    j = 0;
    for (i = 0; i < 6; i++)
        cdata[i] = dyn_blowfish_stream2word(ciphertext, 24, &j);
    for (k = 0; k < 64; k++)
        dyn_blowfish_blf_enc(&state, cdata, 3);
    for (i = 0; i < 6; i++) {           /* big-endian back into the buffer */
        ciphertext[4 * i + 3] = (uint8_t)(cdata[i] & 0xff); cdata[i] >>= 8;
        ciphertext[4 * i + 2] = (uint8_t)(cdata[i] & 0xff); cdata[i] >>= 8;
        ciphertext[4 * i + 1] = (uint8_t)(cdata[i] & 0xff); cdata[i] >>= 8;
        ciphertext[4 * i + 0] = (uint8_t)(cdata[i] & 0xff);
    }
    snprintf(out, 8, "$2b$%2.2u$", (unsigned)logr);
    dyn_bcrypt_b64_encode(out + 7, csalt, 16);
    dyn_bcrypt_b64_encode(out + 7 + 22, ciphertext, 23);
    memset(&state, 0, sizeof state);
    memset(ckey, 0, sizeof ckey);
    memset(ciphertext, 0, sizeof ciphertext);
    memset(cdata, 0, sizeof cdata);
    return 0;
}

/* Parse a stored hash: exactly 60 chars, $2x$CC$ (x in a/b/y), cost 4..20
 * (the verify cap; higher is a DoS bill on untrusted input). */
static int dyn_bcrypt_parse(const char *s, size_t len, uint32_t *logr,
                            uint8_t csalt[16])
{
    uint32_t cost;
    if (len != 60 || s[0] != '$' || s[3] != '$' || s[6] != '$')
        return -1;
    if (s[1] != '2' || (s[2] != 'a' && s[2] != 'b' && s[2] != 'y'))
        return -1;
    if (s[4] < '0' || s[4] > '9' || s[5] < '0' || s[5] > '9')
        return -1;
    cost = (uint32_t)((s[4] - '0') * 10 + (s[5] - '0'));
    if (cost < DYN_BCRYPT_MIN_COST || cost > DYN_BCRYPT_MAX_VERIFY_COST)
        return -1;
    if (dyn_bcrypt_b64_decode(csalt, 16, s + 7) != 0)
        return -1;
    *logr = cost;
    return 0;
}

static JSValue dyn_bcrypt_hash_js(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv)
{
    const uint8_t *pw; size_t pwlen; const char *powned;
    int64_t rounds = 10;
    uint8_t salt[16];
    char out[61];
    (void)this_val;

    if (argc < 1)
        return JS_ThrowTypeError(ctx, "Bcrypt.hash(password[, rounds])");
    /* Rounds FIRST: JS_ToInt64 runs user code (valueOf), which could detach
     * the password TypedArray. The password is coerced last and used with no
     * JS in between (same rule as dyn_hmac_verify). */
    if (argc > 1 && !JS_IsUndefined(argv[1])) {
        if (JS_ToInt64(ctx, &rounds, argv[1]))
            return JS_EXCEPTION;
    }
    if (rounds < DYN_BCRYPT_MIN_COST || rounds > DYN_BCRYPT_MAX_COST)
        return JS_ThrowRangeError(ctx, "Bcrypt.hash: rounds must be 4..31");
    if (dyn_crypto_data(ctx, argv[0], &pw, &pwlen, &powned))
        return JS_EXCEPTION;
    if (pwlen > 72) {
        if (powned) JS_FreeCString(ctx, powned);
        return JS_ThrowRangeError(ctx,
            "Bcrypt.hash: password exceeds 72 bytes; bcrypt's limit is 72 "
            "and truncating silently would hash a different password");
    }
    if (dyn_os_entropy(salt, 16) < 0) {          /* fail closed on entropy */
        if (powned) JS_FreeCString(ctx, powned);
        return JS_ThrowInternalError(ctx, "Bcrypt.hash: OS entropy unavailable");
    }
    dyn_bcrypt_core(pw, pwlen, salt, (uint32_t)rounds, out);
    memset(salt, 0, sizeof salt);
    if (powned) JS_FreeCString(ctx, powned);
    return JS_NewString(ctx, out);
}

static JSValue dyn_bcrypt_verify_js(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv)
{
    const uint8_t *pw; size_t pwlen; const char *powned;
    const char *hash = NULL; size_t hlen = 0;
    uint8_t salt[16];
    char out[61];
    uint32_t logr;
    int r = 0;
    (void)this_val;

    if (argc < 2)
        return JS_ThrowTypeError(ctx, "Bcrypt.verify(password, hash)");
    /* The hash is coerced FIRST (owned string); the password is coerced
     * last, so a ToString on a non-string hash cannot detach a borrowed
     * password buffer. */
    hash = JS_ToCStringLen(ctx, &hlen, argv[1]);
    if (!hash)
        return JS_EXCEPTION;
    if (dyn_crypto_data(ctx, argv[0], &pw, &pwlen, &powned)) {
        JS_FreeCString(ctx, hash);
        return JS_EXCEPTION;
    }
    /* A >72-byte candidate is not the preimage of any hash hash() emits. */
    if (pwlen <= 72 && dyn_bcrypt_parse(hash, hlen, &logr, salt) == 0 &&
        dyn_bcrypt_core(pw, pwlen, salt, logr, out) == 0) {
        /* Compare from "$" onward (the $2x$ prefix variants are equal for
         * byte-clean passwords): cost, salt and hash, in constant time. */
        r = dyn_ct_equal((const uint8_t *)out + 7, (const uint8_t *)hash + 7, 53);
    }
    memset(salt, 0, sizeof salt);
    JS_FreeCString(ctx, hash);
    if (powned) JS_FreeCString(ctx, powned);
    return JS_NewBool(ctx, r);
}

/* ---- Argon2id (RFC 9106, version 0x13) ---------------------------------
 * A port of the PHC reference implementation (ref.c / core.c / blamka-
 * round-ref.h; CC0 / Apache-2.0). Argon2id: the first half of the first
 * pass uses data-independent addressing, the rest data-dependent.
 *
 * DoS bounds (the reference allows far more): memory <= 4 GiB, iterations
 * and lanes <= 16, hashLen <= 1 MiB -- every bound RFC 9106 does not set
 * itself. The caller's own values are their resource; the point is only
 * that a runaway call cannot be demanded for free.
 *
 * Hardened vs the reference: salt length >= 8 is REQUIRED (the reference
 * returns ARGON2_SALT_TOO_SHORT below it; RFC 9106 recommends 16), and
 * every stack buffer that ever holds password-derived bytes is cleared.
 */

#define ARG2_VERSION_NUMBER     0x13
#define ARG2_SYNC_POINTS        4
#define ARG2_QWORDS_IN_BLOCK    128
#define ARG2_BLOCK_SIZE         1024
#define ARG2_ADDRESSES_IN_BLOCK 128
#define ARG2_PREHASH_DIGEST_LENGTH 64
#define ARG2_PREHASH_SEED_LENGTH  72
#define ARG2_TYPE_ID            2       /* Argon2id */
#define ARG2_MAX_LANES          16
#define ARG2_MAX_MEMORY         (1 << 22)   /* KiB: 4 GiB */
#define ARG2_MAX_TIME           16
#define ARG2_MAX_PWD             (1 << 24)
#define ARG2_MAX_SALT            (1 << 24)
#define ARG2_MAX_OUT             (1 << 20)
#define ARG2_MIN_SALT_LEN        8       /* reference ARGON2_SALT_TOO_SHORT */

typedef struct {
    uint64_t v[ARG2_QWORDS_IN_BLOCK];
} arg_block;

typedef struct {
    arg_block *memory;
    uint32_t passes;
    uint32_t memory_blocks;
    uint32_t segment_length;
    uint32_t lane_length;
    uint32_t lanes;
} arg_instance;

static inline uint64_t arg_rotr64(uint64_t x, int n)
{
    return (x >> n) | (x << (64 - n));
}

static inline uint64_t arg_fBlaMka(uint64_t x, uint64_t y)
{
    const uint64_t m = 0xFFFFFFFFULL;
    const uint64_t xy = (x & m) * (y & m);
    return x + y + 2 * xy;
}

#define ARG_G(a, b, c, d) do { \
        a = arg_fBlaMka(a, b); d = arg_rotr64(d ^ a, 32); \
        c = arg_fBlaMka(c, d); b = arg_rotr64(b ^ c, 24); \
        a = arg_fBlaMka(a, b); d = arg_rotr64(d ^ a, 16); \
        c = arg_fBlaMka(c, d); b = arg_rotr64(b ^ c, 63); \
    } while (0)

#define ARG_ROUND(v0, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15) \
    do { \
        ARG_G(v0, v4,  v8, v12); ARG_G(v1, v5,  v9, v13); \
        ARG_G(v2, v6, v10, v14); ARG_G(v3, v7, v11, v15); \
        ARG_G(v0, v5, v10, v15); ARG_G(v1, v6, v11, v12); \
        ARG_G(v2, v7,  v8, v13); ARG_G(v3, v4,  v9, v14); \
    } while (0)

static inline void arg_store64(uint8_t *p, uint64_t v)
{
    int i;
    for (i = 0; i < 8; i++)
        p[i] = (uint8_t)(v >> (8 * i));
}

static inline void arg_store32(uint8_t *p, uint32_t v)
{
    int i;
    for (i = 0; i < 4; i++)
        p[i] = (uint8_t)(v >> (8 * i));
}

static void arg_store_block(uint8_t *out, const arg_block *b)
{
    unsigned i;
    for (i = 0; i < ARG2_QWORDS_IN_BLOCK; i++)
        arg_store64(out + i * 8, b->v[i]);
}

/* ---- BLAKE2b (streaming, over the TU's bl2b_compress) and H' -----------
 * bl2b_compress is a pure function (no static state), so this is
 * thread-safe. The context buffers raw input (password bytes in H0), so
 * arg_blake2b_final clears both the local block and the context. */

typedef struct {
    uint64_t h[8];
    uint64_t t;
    uint8_t buf[128];
    size_t buflen;
    uint8_t outlen;
} arg_blake2b_ctx;

static void arg_blake2b_init(arg_blake2b_ctx *s, size_t outlen)
{
    memset(s, 0, sizeof *s);
    memcpy(s->h, BL2B_IV, sizeof s->h);
    s->h[0] ^= 0x01010000ULL ^ (uint64_t)outlen;   /* unkeyed, fanout 1 */
    s->outlen = (uint8_t)outlen;
}

static void arg_blake2b_update(arg_blake2b_ctx *s, const uint8_t *in,
                               size_t inlen)
{
    if (inlen == 0)
        return;
    if (s->buflen + inlen > 128) {
        size_t fill = 128 - s->buflen;
        memcpy(s->buf + s->buflen, in, fill);
        s->t += 128;
        bl2b_compress(s->h, s->buf, s->t, 0);
        s->buflen = 0;
        in += fill; inlen -= fill;
        while (inlen > 128) {
            s->t += 128;
            bl2b_compress(s->h, in, s->t, 0);
            in += 128; inlen -= 128;
        }
    }
    memcpy(s->buf + s->buflen, in, inlen);
    s->buflen += inlen;
}

static void arg_blake2b_final(arg_blake2b_ctx *s, uint8_t *out)
{
    uint8_t block[128];
    size_t i;
    memset(block, 0, sizeof block);
    memcpy(block, s->buf, s->buflen);
    s->t += s->buflen;
    bl2b_compress(s->h, block, s->t, 1);
    for (i = 0; i < s->outlen; i++)
        out[i] = (uint8_t)(s->h[i / 8] >> (8 * (i % 8)));
    memset(block, 0, sizeof block);          /* held the last input chunk */
    memset(s, 0, sizeof *s);
}

/* H' (RFC 9106 s3.3): T <= 64 -> single hash of LE32(T)||A; else the
 * V_1..V_r chain emitting the first 32 bytes of each 64-byte block. */
static int arg_blake2b_long(uint8_t *out, size_t outlen,
                            const uint8_t *in, size_t inlen)
{
    arg_blake2b_ctx s;
    uint8_t ob[4];
    uint8_t outbuf[64];
    if (outlen == 0 || outlen > 0xFFFFFFFFULL)
        return -1;
    ob[0] = (uint8_t)(outlen & 0xff);        ob[1] = (uint8_t)((outlen >> 8) & 0xff);
    ob[2] = (uint8_t)((outlen >> 16) & 0xff); ob[3] = (uint8_t)((outlen >> 24) & 0xff);
    if (outlen <= 64) {
        arg_blake2b_init(&s, outlen);
        arg_blake2b_update(&s, ob, 4);
        arg_blake2b_update(&s, in, inlen);
        arg_blake2b_final(&s, out);
        return 0;
    }
    arg_blake2b_init(&s, 64);
    arg_blake2b_update(&s, ob, 4);
    arg_blake2b_update(&s, in, inlen);
    arg_blake2b_final(&s, outbuf);
    memcpy(out, outbuf, 32);
    out += 32;
    outlen -= 32;
    while (outlen > 64) {
        arg_blake2b_init(&s, 64);
        arg_blake2b_update(&s, outbuf, 64);
        arg_blake2b_final(&s, outbuf);
        memcpy(out, outbuf, 32);
        out += 32;
        outlen -= 32;
    }
    arg_blake2b_init(&s, outlen);
    arg_blake2b_update(&s, outbuf, 64);
    arg_blake2b_final(&s, out);
    memset(outbuf, 0, sizeof outbuf);
    return 0;
}

/* ---- the compression function G ---------------------------------------- */

/* G(prev, ref) -> next: R = prev XOR ref; P applied row-wise then
 * column-wise to R; output = (prev XOR ref XOR next_old) XOR P(...).
 * with_xor=0 overwrites next (pass 0), with_xor=1 XORs it (passes >= 1). */
static void arg_fill_block(const arg_block *prev, const arg_block *ref,
                           arg_block *next, int with_xor)
{
    arg_block blockR, block_tmp;
    unsigned i;

    memcpy(blockR.v, ref->v, sizeof blockR.v);
    for (i = 0; i < ARG2_QWORDS_IN_BLOCK; i++)
        blockR.v[i] ^= prev->v[i];
    memcpy(block_tmp.v, blockR.v, sizeof block_tmp.v);
    if (with_xor)
        for (i = 0; i < ARG2_QWORDS_IN_BLOCK; i++)
            block_tmp.v[i] ^= next->v[i];

    for (i = 0; i < 8; i++)
        ARG_ROUND(blockR.v[16 * i],     blockR.v[16 * i + 1],
                  blockR.v[16 * i + 2], blockR.v[16 * i + 3],
                  blockR.v[16 * i + 4], blockR.v[16 * i + 5],
                  blockR.v[16 * i + 6], blockR.v[16 * i + 7],
                  blockR.v[16 * i + 8], blockR.v[16 * i + 9],
                  blockR.v[16 * i + 10], blockR.v[16 * i + 11],
                  blockR.v[16 * i + 12], blockR.v[16 * i + 13],
                  blockR.v[16 * i + 14], blockR.v[16 * i + 15]);
    for (i = 0; i < 8; i++)
        ARG_ROUND(blockR.v[2 * i],       blockR.v[2 * i + 1],
                  blockR.v[2 * i + 16],  blockR.v[2 * i + 17],
                  blockR.v[2 * i + 32],  blockR.v[2 * i + 33],
                  blockR.v[2 * i + 48],  blockR.v[2 * i + 49],
                  blockR.v[2 * i + 64],  blockR.v[2 * i + 65],
                  blockR.v[2 * i + 80],  blockR.v[2 * i + 81],
                  blockR.v[2 * i + 96],  blockR.v[2 * i + 97],
                  blockR.v[2 * i + 112], blockR.v[2 * i + 113]);

    memcpy(next->v, block_tmp.v, sizeof next->v);
    for (i = 0; i < ARG2_QWORDS_IN_BLOCK; i++)
        next->v[i] ^= blockR.v[i];
    memset(blockR.v, 0, sizeof blockR.v);        /* derived key material */
    memset(block_tmp.v, 0, sizeof block_tmp.v);
}

static void arg_next_addresses(arg_block *address_block,
                               arg_block *input_block,
                               const arg_block *zero_block)
{
    input_block->v[6]++;
    arg_fill_block(zero_block, input_block, address_block, 0);
    arg_fill_block(zero_block, address_block, address_block, 0);
}

/* Map J1 into [0, |W|): x = J1^2 / 2^32; zz = |W|-1 - |W|*x / 2^32, all in
 * integer arithmetic exactly as the reference does it. The reference_area
 * size rules are RFC 9106 s3.4.2; the -1 for a segment's first block is
 * the reference's intentional unsigned wrap. */
static uint32_t arg_index_alpha(const arg_instance *instance, uint32_t pass,
                                uint32_t slice, uint32_t index,
                                uint32_t pseudo_rand, int same_lane)
{
    uint32_t reference_area_size;
    uint64_t relative_position;
    uint32_t start_position, absolute_position;

    if (0 == pass) {
        if (0 == slice)
            reference_area_size = index - 1;
        else if (same_lane)
            reference_area_size = slice * instance->segment_length + index - 1;
        else
            reference_area_size = slice * instance->segment_length +
                                  ((index == 0) ? UINT32_MAX : 0);
    } else if (same_lane) {
        reference_area_size = instance->lane_length - instance->segment_length +
                              index - 1;
    } else {
        reference_area_size = instance->lane_length - instance->segment_length +
                              ((index == 0) ? UINT32_MAX : 0);
    }
    relative_position = pseudo_rand;
    relative_position = relative_position * relative_position >> 32;
    relative_position = reference_area_size - 1 -
                        (reference_area_size * relative_position >> 32);
    start_position = 0;
    if (0 != pass)
        start_position = (slice == ARG2_SYNC_POINTS - 1)
                             ? 0 : (slice + 1) * instance->segment_length;
    absolute_position = (start_position + (uint32_t)relative_position) %
                        instance->lane_length;
    return absolute_position;
}

/* Fill one segment. Data-independent addressing only in slices 0..1 of
 * pass 0 (Argon2id, RFC 9106 s3.4.1.3). */
static void arg_fill_segment(const arg_instance *instance, uint32_t pass,
                             uint32_t lane, uint32_t slice)
{
    arg_block address_block, input_block, zero_block;
    uint64_t pseudo_rand, ref_lane;
    uint32_t ref_index, prev_offset, curr_offset, starting_index, i;
    int data_independent_addressing, blocks_init = 0;

    data_independent_addressing =
        (pass == 0) && (slice < ARG2_SYNC_POINTS / 2);

    if (data_independent_addressing) {
        memset(&zero_block, 0, sizeof zero_block);
        memset(&input_block, 0, sizeof input_block);
        input_block.v[0] = pass;
        input_block.v[1] = lane;
        input_block.v[2] = slice;
        input_block.v[3] = instance->memory_blocks;
        input_block.v[4] = instance->passes;
        input_block.v[5] = ARG2_TYPE_ID;
        blocks_init = 1;
    }

    starting_index = 0;
    if ((0 == pass) && (0 == slice)) {
        starting_index = 2;               /* blocks 0,1 precomputed */
        if (data_independent_addressing) {
            arg_next_addresses(&address_block, &input_block, &zero_block);
            blocks_init = 1;
        }
    }
    curr_offset = lane * instance->lane_length +
                  slice * instance->segment_length + starting_index;
    prev_offset = (0 == curr_offset % instance->lane_length)
                      ? curr_offset + instance->lane_length - 1
                      : curr_offset - 1;

    for (i = starting_index; i < instance->segment_length;
         i++, curr_offset++, prev_offset++) {
        if (curr_offset % instance->lane_length == 1)
            prev_offset = curr_offset - 1;
        if (data_independent_addressing) {
            if (i % ARG2_ADDRESSES_IN_BLOCK == 0)
                arg_next_addresses(&address_block, &input_block, &zero_block);
            pseudo_rand = address_block.v[i % ARG2_ADDRESSES_IN_BLOCK];
        } else {
            pseudo_rand = instance->memory[prev_offset].v[0];
        }
        ref_lane = (pseudo_rand >> 32) % instance->lanes;
        if ((pass == 0) && (slice == 0))
            ref_lane = lane;              /* no cross-lane refs in pass 0 */
        ref_index = arg_index_alpha(instance, pass, slice, i,
                                    (uint32_t)(pseudo_rand & 0xFFFFFFFF),
                                    ref_lane == lane);
        {
            const arg_block *ref = &instance->memory[(size_t)ref_lane *
                                                     instance->lane_length +
                                                     ref_index];
            arg_block *curr = &instance->memory[curr_offset];
            arg_fill_block(&instance->memory[prev_offset], ref, curr,
                           pass == 0 ? 0 : 1);
        }
    }
    if (blocks_init) {
        memset(&address_block, 0, sizeof address_block);
        memset(&input_block, 0, sizeof input_block);
        memset(&zero_block, 0, sizeof zero_block);
    }
}

/* H0: LE32(p)||LE32(T)||LE32(m)||LE32(t)||LE32(0x13)||LE32(2)||
 * LE32(len(P))||P||LE32(len(S))||S||LE32(0)||LE32(0). No secret/AD. */
static void arg_initial_hash(uint8_t *blockhash, uint32_t lanes,
                             uint32_t outlen, uint32_t m_cost, uint32_t t_cost,
                             const uint8_t *pwd, size_t pwdlen,
                             const uint8_t *salt, size_t saltlen)
{
    arg_blake2b_ctx S;
    uint8_t value[4];

    arg_blake2b_init(&S, ARG2_PREHASH_DIGEST_LENGTH);
    arg_store32(value, lanes);    arg_blake2b_update(&S, value, 4);
    arg_store32(value, outlen);   arg_blake2b_update(&S, value, 4);
    arg_store32(value, m_cost);   arg_blake2b_update(&S, value, 4);
    arg_store32(value, t_cost);   arg_blake2b_update(&S, value, 4);
    arg_store32(value, ARG2_VERSION_NUMBER); arg_blake2b_update(&S, value, 4);
    arg_store32(value, ARG2_TYPE_ID); arg_blake2b_update(&S, value, 4);
    arg_store32(value, (uint32_t)pwdlen); arg_blake2b_update(&S, value, 4);
    if (pwdlen) arg_blake2b_update(&S, pwd, pwdlen);
    arg_store32(value, (uint32_t)saltlen); arg_blake2b_update(&S, value, 4);
    if (saltlen) arg_blake2b_update(&S, salt, saltlen);
    arg_store32(value, 0); arg_blake2b_update(&S, value, 4);   /* secret */
    arg_store32(value, 0); arg_blake2b_update(&S, value, 4);   /* ad */
    arg_blake2b_final(&S, blockhash);
}

/* 0 ok, -1 bad params, -2 OOM. Argon2id only. */
static int arg_hash_id(const uint8_t *pwd, size_t pwdlen,
                       const uint8_t *salt, size_t saltlen,
                       uint32_t t, uint32_t m, uint32_t lanes,
                       uint8_t *out, size_t outlen)
{
    arg_instance inst;
    arg_block blockhash_b;
    uint8_t blockhash[ARG2_PREHASH_SEED_LENGTH];
    uint8_t blockhash_bytes[ARG2_BLOCK_SIZE];
    uint32_t memory_blocks, segment_length, l, i, r, s;
    int rc = -1;

    if (lanes < 1 || lanes > ARG2_MAX_LANES) return -1;
    if (m < 8 * lanes || m > ARG2_MAX_MEMORY) return -1;   /* RFC 9106: m>=8p */
    if (t < 1 || t > ARG2_MAX_TIME) return -1;
    if (outlen < 4 || outlen > ARG2_MAX_OUT) return -1;
    if (pwdlen > ARG2_MAX_PWD || saltlen > ARG2_MAX_SALT) return -1;
    if (saltlen < ARG2_MIN_SALT_LEN) return -1;   /* reference: SALT_TOO_SHORT */

    memory_blocks = m;
    if (memory_blocks < 2 * ARG2_SYNC_POINTS * lanes)
        memory_blocks = 2 * ARG2_SYNC_POINTS * lanes;
    segment_length = memory_blocks / (lanes * ARG2_SYNC_POINTS);
    memory_blocks = segment_length * (lanes * ARG2_SYNC_POINTS);

    inst.passes = t;
    inst.memory_blocks = memory_blocks;
    inst.segment_length = segment_length;
    inst.lane_length = segment_length * ARG2_SYNC_POINTS;
    inst.lanes = lanes;

    inst.memory = (arg_block *)calloc(memory_blocks, sizeof(arg_block));
    if (!inst.memory)
        return -2;

    arg_initial_hash(blockhash, lanes, (uint32_t)outlen, m, t,
                     pwd, pwdlen, salt, saltlen);
    memset(blockhash + ARG2_PREHASH_DIGEST_LENGTH, 0,
           ARG2_PREHASH_SEED_LENGTH - ARG2_PREHASH_DIGEST_LENGTH);

    /* B[i][0] = H'(H0||LE32(0)||LE32(i)); B[i][1] = H'(H0||LE32(1)||LE32(i)) */
    for (l = 0; l < lanes; l++) {
        arg_store32(blockhash + ARG2_PREHASH_DIGEST_LENGTH, 0);
        arg_store32(blockhash + ARG2_PREHASH_DIGEST_LENGTH + 4, l);
        arg_blake2b_long((uint8_t *)&inst.memory[(size_t)l * inst.lane_length],
                         ARG2_BLOCK_SIZE, blockhash, ARG2_PREHASH_SEED_LENGTH);
        arg_store32(blockhash + ARG2_PREHASH_DIGEST_LENGTH, 1);
        arg_blake2b_long((uint8_t *)&inst.memory[(size_t)l * inst.lane_length + 1],
                         ARG2_BLOCK_SIZE, blockhash, ARG2_PREHASH_SEED_LENGTH);
    }
    memset(blockhash, 0, sizeof blockhash);

    for (r = 0; r < t; r++)
        for (s = 0; s < ARG2_SYNC_POINTS; s++)
            for (l = 0; l < lanes; l++)
                arg_fill_segment(&inst, r, l, s);

    /* C = XOR of the last column; tag = H'(C). */
    memcpy(blockhash_b.v, inst.memory[inst.lane_length - 1].v,
           ARG2_BLOCK_SIZE);
    for (l = 1; l < lanes; l++)
        for (i = 0; i < ARG2_QWORDS_IN_BLOCK; i++)
            blockhash_b.v[i] ^=
                inst.memory[(size_t)l * inst.lane_length +
                            (inst.lane_length - 1)].v[i];
    arg_store_block(blockhash_bytes, &blockhash_b);
    arg_blake2b_long(out, outlen, blockhash_bytes, ARG2_BLOCK_SIZE);
    rc = 0;

    memset(blockhash_bytes, 0, sizeof blockhash_bytes);
    memset(blockhash_b.v, 0, sizeof blockhash_b.v);
    memset(inst.memory, 0, (size_t)memory_blocks * sizeof(arg_block));
    free(inst.memory);
    return rc;
}

/* ---- Argon2id JS wrappers ----------------------------------------------
 * Options object: { iterations: 3, memory: 65536, parallelism: 4,
 * hashLen: 32 }. Password and salt are copied to libc memory BEFORE any
 * options getter runs: a getter is user JS and can detach a borrowed
 * TypedArray. */

static JSValue dyn_argon2_hash_js(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv)
{
    uint8_t *pwd = NULL, *salt = NULL, *out = NULL;
    size_t pwdlen = 0, saltlen = 0;
    int64_t t = 3, m = 65536, p = 4, outlen = 32;
    JSValue ret = JS_EXCEPTION, v;
    (void)this_val;

    if (argc < 2)
        return JS_ThrowTypeError(ctx, "Argon2id.hash(password, salt[, opts])");
    {
        const uint8_t *d; size_t n; const char *ow;
        if (dyn_crypto_data(ctx, argv[0], &d, &n, &ow)) return JS_EXCEPTION;
        pwd = (uint8_t *)malloc(n ? n : 1);
        if (!pwd) { if (ow) JS_FreeCString(ctx, ow); return JS_ThrowOutOfMemory(ctx); }
        if (n) memcpy(pwd, d, n);
        pwdlen = n;
        if (ow) JS_FreeCString(ctx, ow);
        if (dyn_crypto_data(ctx, argv[1], &d, &n, &ow)) goto done;
        salt = (uint8_t *)malloc(n ? n : 1);
        if (!salt) { if (ow) JS_FreeCString(ctx, ow); JS_ThrowOutOfMemory(ctx); goto done; }
        if (n) memcpy(salt, d, n);
        saltlen = n;
        if (ow) JS_FreeCString(ctx, ow);
    }
    if (argc > 2 && JS_IsObject(argv[2])) {
        v = JS_GetPropertyStr(ctx, argv[2], "iterations");
        if (JS_IsException(v)) goto done;
        if (!JS_IsUndefined(v) && JS_ToInt64(ctx, &t, v)) { JS_FreeValue(ctx, v); goto done; }
        JS_FreeValue(ctx, v);
        v = JS_GetPropertyStr(ctx, argv[2], "memory");
        if (JS_IsException(v)) goto done;
        if (!JS_IsUndefined(v) && JS_ToInt64(ctx, &m, v)) { JS_FreeValue(ctx, v); goto done; }
        JS_FreeValue(ctx, v);
        v = JS_GetPropertyStr(ctx, argv[2], "parallelism");
        if (JS_IsException(v)) goto done;
        if (!JS_IsUndefined(v) && JS_ToInt64(ctx, &p, v)) { JS_FreeValue(ctx, v); goto done; }
        JS_FreeValue(ctx, v);
        v = JS_GetPropertyStr(ctx, argv[2], "hashLen");
        if (JS_IsException(v)) goto done;
        if (!JS_IsUndefined(v) && JS_ToInt64(ctx, &outlen, v)) { JS_FreeValue(ctx, v); goto done; }
        JS_FreeValue(ctx, v);
    }
    if (t < 1 || t > ARG2_MAX_TIME)
        { JS_ThrowRangeError(ctx, "Argon2id: iterations must be 1..%d", ARG2_MAX_TIME); goto done; }
    if (p < 1 || p > ARG2_MAX_LANES)
        { JS_ThrowRangeError(ctx, "Argon2id: parallelism must be 1..%d", ARG2_MAX_LANES); goto done; }
    if (m < 8 * p || m > ARG2_MAX_MEMORY)
        { JS_ThrowRangeError(ctx, "Argon2id: memory (KiB) must be 8*parallelism..%d", ARG2_MAX_MEMORY); goto done; }
    if (outlen < 4 || outlen > ARG2_MAX_OUT)
        { JS_ThrowRangeError(ctx, "Argon2id: hashLen must be 4..%d", ARG2_MAX_OUT); goto done; }
    if (pwdlen > ARG2_MAX_PWD || saltlen > ARG2_MAX_SALT || saltlen < ARG2_MIN_SALT_LEN)
        { JS_ThrowRangeError(ctx, "Argon2id: salt must be 8..%d bytes", ARG2_MAX_SALT); goto done; }

    out = (uint8_t *)malloc((size_t)outlen);
    if (!out) { JS_ThrowOutOfMemory(ctx); goto done; }
    switch (arg_hash_id(pwd, pwdlen, salt, saltlen, (uint32_t)t, (uint32_t)m,
                        (uint32_t)p, out, (size_t)outlen)) {
    case 0:
        ret = dyn_crypto_u8array(ctx, out, (size_t)outlen);
        break;
    case -2:
        JS_ThrowOutOfMemory(ctx);
        break;
    default:
        JS_ThrowRangeError(ctx, "Argon2id: parameters rejected");
        break;
    }
done:
    if (out) { memset(out, 0, (size_t)outlen); free(out); }
    if (pwd) { memset(pwd, 0, pwdlen); free(pwd); }
    if (salt) { memset(salt, 0, saltlen); free(salt); }
    return ret;
}

static JSValue dyn_argon2_verify_js(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv)
{
    uint8_t *pwd = NULL, *salt = NULL, *expected = NULL, *out = NULL;
    size_t pwdlen = 0, saltlen = 0, expectedlen = 0;
    int64_t t = 3, m = 65536, p = 4, outlen = 32;
    JSValue ret = JS_EXCEPTION, v;
    int rc = 0, r = 0;
    (void)this_val;

    if (argc < 3)
        return JS_ThrowTypeError(ctx, "Argon2id.verify(password, salt, expectedHash[, opts])");
    {
        const uint8_t *d; size_t n; const char *ow;
        if (dyn_crypto_data(ctx, argv[0], &d, &n, &ow)) return JS_EXCEPTION;
        pwd = (uint8_t *)malloc(n ? n : 1);
        if (!pwd) { if (ow) JS_FreeCString(ctx, ow); return JS_ThrowOutOfMemory(ctx); }
        if (n) memcpy(pwd, d, n);
        pwdlen = n;
        if (ow) JS_FreeCString(ctx, ow);
        if (dyn_crypto_data(ctx, argv[1], &d, &n, &ow)) goto done;
        salt = (uint8_t *)malloc(n ? n : 1);
        if (!salt) { if (ow) JS_FreeCString(ctx, ow); JS_ThrowOutOfMemory(ctx); goto done; }
        if (n) memcpy(salt, d, n);
        saltlen = n;
        if (ow) JS_FreeCString(ctx, ow);
        /* expected is argv[2]: copy it before any opts getter can detach it */
        if (dyn_crypto_data(ctx, argv[2], &d, &n, &ow)) goto done;
        expected = (uint8_t *)malloc(n ? n : 1);
        if (!expected) { if (ow) JS_FreeCString(ctx, ow); JS_ThrowOutOfMemory(ctx); goto done; }
        if (n) memcpy(expected, d, n);
        expectedlen = n;
        if (ow) JS_FreeCString(ctx, ow);
    }
    /* same option shape and caps as hash(); verify re-derives with them */
    if (argc > 3 && JS_IsObject(argv[3])) {
        v = JS_GetPropertyStr(ctx, argv[3], "iterations");
        if (JS_IsException(v)) goto done;
        if (!JS_IsUndefined(v) && JS_ToInt64(ctx, &t, v)) { JS_FreeValue(ctx, v); goto done; }
        JS_FreeValue(ctx, v);
        v = JS_GetPropertyStr(ctx, argv[3], "memory");
        if (JS_IsException(v)) goto done;
        if (!JS_IsUndefined(v) && JS_ToInt64(ctx, &m, v)) { JS_FreeValue(ctx, v); goto done; }
        JS_FreeValue(ctx, v);
        v = JS_GetPropertyStr(ctx, argv[3], "parallelism");
        if (JS_IsException(v)) goto done;
        if (!JS_IsUndefined(v) && JS_ToInt64(ctx, &p, v)) { JS_FreeValue(ctx, v); goto done; }
        JS_FreeValue(ctx, v);
        v = JS_GetPropertyStr(ctx, argv[3], "hashLen");
        if (JS_IsException(v)) goto done;
        if (!JS_IsUndefined(v) && JS_ToInt64(ctx, &outlen, v)) { JS_FreeValue(ctx, v); goto done; }
        JS_FreeValue(ctx, v);
    }
    if (t < 1 || t > ARG2_MAX_TIME || p < 1 || p > ARG2_MAX_LANES ||
        m < 8 * p || m > ARG2_MAX_MEMORY ||
        outlen < 4 || outlen > ARG2_MAX_OUT ||
        pwdlen > ARG2_MAX_PWD || saltlen > ARG2_MAX_SALT ||
        saltlen < ARG2_MIN_SALT_LEN) {
        JS_ThrowRangeError(ctx, "Argon2id: parameter out of range");
        goto done;
    }
    out = (uint8_t *)malloc((size_t)outlen);
    if (!out) { JS_ThrowOutOfMemory(ctx); goto done; }
    rc = arg_hash_id(pwd, pwdlen, salt, saltlen, (uint32_t)t, (uint32_t)m,
                     (uint32_t)p, out, (size_t)outlen);
    if (rc == 0) {
        /* The work is done regardless; only the comparison depends on the
         * expected value's length, and it is constant-time over the hash. */
        r = (expectedlen == (size_t)outlen) &&
            dyn_ct_equal(out, expected, expectedlen);
        ret = JS_NewBool(ctx, r);
    } else if (rc == -2) {
        JS_ThrowOutOfMemory(ctx);
    } else {
        JS_ThrowRangeError(ctx, "Argon2id: parameters rejected");
    }
done:
    if (out) { memset(out, 0, (size_t)outlen); free(out); }
    if (expected) { memset(expected, 0, expectedlen); free(expected); }
    if (pwd) { memset(pwd, 0, pwdlen); free(pwd); }
    if (salt) { memset(salt, 0, saltlen); free(salt); }
    return ret;
}


#ifdef CONFIG_TLS

#include <openssl/rand.h>
#include <openssl/kdf.h>

/* ---- Standalone RSA / ECDSA / ECDH / Scrypt ---------------------------- */

static int dyn_streq_ci(const char *a, const char *b)
{
    while (*a && *b) {
        char ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb) return 0;
        a++; b++;
    }
    return *a == *b;
}

static const EVP_MD *dyn_standalone_md(const char *name)
{
    if (!name) return NULL;
    if (dyn_streq_ci(name, "sha1")   || dyn_streq_ci(name, "sha-1"))
        return EVP_sha1();
    if (dyn_streq_ci(name, "sha256") || dyn_streq_ci(name, "sha-256"))
        return EVP_sha256();
    if (dyn_streq_ci(name, "sha384") || dyn_streq_ci(name, "sha-384"))
        return EVP_sha384();
    if (dyn_streq_ci(name, "sha512") || dyn_streq_ci(name, "sha-512"))
        return EVP_sha512();
    return NULL;
}

static int dyn_crypto_copy(JSContext *ctx, JSValueConst v, uint8_t **out, size_t *outlen)
{
    const uint8_t *d; size_t n; const char *ow;
    if (dyn_crypto_data(ctx, v, &d, &n, &ow))
        return -1;
    *out = (uint8_t *)malloc(n ? n : 1);
    if (!*out) {
        if (ow) JS_FreeCString(ctx, ow);
        JS_ThrowOutOfMemory(ctx);
        return -1;
    }
    if (n) memcpy(*out, d, n);
    *outlen = n;
    if (ow) JS_FreeCString(ctx, ow);
    return 0;
}

static int dyn_ec_coord(EVP_PKEY *k)
{
    int bits;
    if (!EVP_PKEY_is_a(k, "EC"))
        return 0;
    bits = EVP_PKEY_get_bits(k);
    if (bits == 256) return 32;
    if (bits == 384) return 48;
    return -1;
}

static int dyn_curve_nid(const char *curve, int *coord)
{
    if (!curve) return -1;
    if (!strcmp(curve, "P-256") || !strcmp(curve, "prime256v1") || !strcmp(curve, "secp256r1")) {
        *coord = 32; return NID_X9_62_prime256v1;
    }
    if (!strcmp(curve, "P-384") || !strcmp(curve, "secp384r1")) {
        *coord = 48; return NID_secp384r1;
    }
    return -1;
}

static JSValue dyn_pk_export_pem(JSContext *ctx, EVP_PKEY *k)
{
    BIO *bio_priv = NULL, *bio_pub = NULL;
    char *priv_ptr = NULL, *pub_ptr = NULL;
    long priv_len = 0, pub_len = 0;
    JSValue obj = JS_EXCEPTION, priv_val = JS_UNDEFINED, pub_val = JS_UNDEFINED;

    bio_priv = BIO_new(BIO_s_mem());
    bio_pub  = BIO_new(BIO_s_mem());
    if (!bio_priv || !bio_pub) {
        JS_ThrowOutOfMemory(ctx);
        goto done;
    }
    if (!PEM_write_bio_PrivateKey(bio_priv, k, NULL, NULL, 0, NULL, NULL) ||
        !PEM_write_bio_PUBKEY(bio_pub, k)) {
        JS_ThrowInternalError(ctx, "failed to serialize PEM key");
        goto done;
    }
    priv_len = BIO_get_mem_data(bio_priv, &priv_ptr);
    pub_len  = BIO_get_mem_data(bio_pub, &pub_ptr);
    if (priv_len <= 0 || pub_len <= 0 || !priv_ptr || !pub_ptr) {
        JS_ThrowInternalError(ctx, "failed to read PEM data");
        goto done;
    }
    priv_val = JS_NewStringLen(ctx, priv_ptr, (size_t)priv_len);
    pub_val  = JS_NewStringLen(ctx, pub_ptr, (size_t)pub_len);
    if (JS_IsException(priv_val) || JS_IsException(pub_val))
        goto done;

    obj = JS_NewObject(ctx);
    if (JS_IsException(obj)) goto done;
    JS_DefinePropertyValueStr(ctx, obj, "privateKey", priv_val, JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, obj, "publicKey",  pub_val,  JS_PROP_C_W_E);
    priv_val = pub_val = JS_UNDEFINED;

done:
    if (bio_priv) BIO_free(bio_priv);
    if (bio_pub)  BIO_free(bio_pub);
    JS_FreeValue(ctx, priv_val);
    JS_FreeValue(ctx, pub_val);
    return obj;
}

/* RSA */
static JSValue dyn_rsa_generate(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    int64_t bits = 2048;
    EVP_PKEY_CTX *kctx = NULL;
    EVP_PKEY *pkey = NULL;
    JSValue ret = JS_EXCEPTION;
    (void)this_val;

    if (argc > 0 && !JS_IsUndefined(argv[0])) {
        if (JS_ToInt64(ctx, &bits, argv[0])) return JS_EXCEPTION;
        if (bits != 2048 && bits != 3072 && bits != 4096)
            return JS_ThrowRangeError(ctx, "RSA.generate: bits must be 2048, 3072, or 4096");
    }
    if (RAND_status() != 1)
        return JS_ThrowInternalError(ctx, "RSA.generate: OS entropy unavailable");

    kctx = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, NULL);
    if (!kctx || EVP_PKEY_keygen_init(kctx) <= 0 ||
        EVP_PKEY_CTX_set_rsa_keygen_bits(kctx, (int)bits) <= 0 ||
        EVP_PKEY_keygen(kctx, &pkey) <= 0) {
        JS_ThrowInternalError(ctx, "RSA.generate: key generation failed");
        goto done;
    }
    ret = dyn_pk_export_pem(ctx, pkey);

done:
    if (pkey) EVP_PKEY_free(pkey);
    if (kctx) EVP_PKEY_CTX_free(kctx);
    return ret;
}

static JSValue dyn_rsa_sign(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    const char *md_str = NULL;
    const EVP_MD *md;
    uint8_t *pem_bytes = NULL; size_t pem_len = 0;
    const uint8_t *msg; size_t msglen; const char *msg_ow = NULL;
    EVP_PKEY *k = NULL;
    EVP_MD_CTX *mctx = NULL;
    size_t siglen = 0;
    uint8_t *sig = NULL;
    JSValue ret = JS_EXCEPTION;
    (void)this_val;

    if (argc < 3)
        return JS_ThrowTypeError(ctx, "RSA.sign(md, privateKey, msg)");
    md_str = JS_ToCString(ctx, argv[0]);
    if (!md_str) return JS_EXCEPTION;
    md = dyn_standalone_md(md_str);
    JS_FreeCString(ctx, md_str);
    if (!md) return JS_ThrowTypeError(ctx, "RSA.sign: unsupported hash algorithm");

    if (dyn_crypto_copy(ctx, argv[1], &pem_bytes, &pem_len)) return JS_EXCEPTION;
    k = jwt_pem_key((const char *)pem_bytes, pem_len, 1);
    free(pem_bytes);
    if (!k) return JS_ThrowTypeError(ctx, "RSA.sign: invalid private key PEM");
    if (!EVP_PKEY_is_a(k, "RSA")) {
        EVP_PKEY_free(k);
        return JS_ThrowTypeError(ctx, "RSA.sign: key is not an RSA key");
    }

    if (dyn_crypto_data(ctx, argv[2], &msg, &msglen, &msg_ow)) {
        EVP_PKEY_free(k);
        return JS_EXCEPTION;
    }

    mctx = EVP_MD_CTX_new();
    if (!mctx || EVP_DigestSignInit(mctx, NULL, md, NULL, k) <= 0 ||
        EVP_DigestSign(mctx, NULL, &siglen, msg, msglen) <= 0) {
        JS_ThrowInternalError(ctx, "RSA.sign: signing initialization failed");
        goto done;
    }
    sig = (uint8_t *)malloc(siglen ? siglen : 1);
    if (!sig) { JS_ThrowOutOfMemory(ctx); goto done; }
    if (EVP_DigestSign(mctx, sig, &siglen, msg, msglen) <= 0) {
        JS_ThrowInternalError(ctx, "RSA.sign: signing failed");
        goto done;
    }
    ret = dyn_crypto_u8array(ctx, sig, siglen);

done:
    free(sig);
    if (mctx) EVP_MD_CTX_free(mctx);
    if (k) EVP_PKEY_free(k);
    if (msg_ow) JS_FreeCString(ctx, msg_ow);
    return ret;
}

static JSValue dyn_rsa_verify(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    const char *md_str = NULL;
    const EVP_MD *md;
    uint8_t *pem_bytes = NULL; size_t pem_len = 0;
    uint8_t *sig_bytes = NULL; size_t sig_len = 0;
    const uint8_t *msg; size_t msglen; const char *msg_ow = NULL;
    EVP_PKEY *k = NULL;
    EVP_MD_CTX *mctx = NULL;
    int ok = 0;
    (void)this_val;

    if (argc < 4)
        return JS_ThrowTypeError(ctx, "RSA.verify(md, publicKey, msg, sig)");
    md_str = JS_ToCString(ctx, argv[0]);
    if (!md_str) return JS_EXCEPTION;
    md = dyn_standalone_md(md_str);
    JS_FreeCString(ctx, md_str);
    if (!md) return JS_ThrowTypeError(ctx, "RSA.verify: unsupported hash algorithm");

    if (dyn_crypto_copy(ctx, argv[1], &pem_bytes, &pem_len)) return JS_EXCEPTION;
    k = jwt_pem_key((const char *)pem_bytes, pem_len, 0);
    free(pem_bytes);
    if (!k) return JS_ThrowTypeError(ctx, "RSA.verify: invalid public key PEM");
    if (!EVP_PKEY_is_a(k, "RSA")) {
        EVP_PKEY_free(k);
        return JS_ThrowTypeError(ctx, "RSA.verify: key is not an RSA key");
    }

    if (dyn_crypto_copy(ctx, argv[3], &sig_bytes, &sig_len)) {
        EVP_PKEY_free(k);
        return JS_EXCEPTION;
    }
    if (dyn_crypto_data(ctx, argv[2], &msg, &msglen, &msg_ow)) {
        free(sig_bytes);
        EVP_PKEY_free(k);
        return JS_EXCEPTION;
    }

    mctx = EVP_MD_CTX_new();
    if (mctx && EVP_DigestVerifyInit(mctx, NULL, md, NULL, k) > 0) {
        if (EVP_DigestVerify(mctx, sig_bytes, sig_len, msg, msglen) == 1)
            ok = 1;
    }

    free(sig_bytes);
    if (mctx) EVP_MD_CTX_free(mctx);
    if (k) EVP_PKEY_free(k);
    if (msg_ow) JS_FreeCString(ctx, msg_ow);
    return JS_NewBool(ctx, ok);
}

/* ECDSA */
static JSValue dyn_ecdsa_generate(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    const char *curve_str = "P-256";
    const char *c_ow = NULL;
    int coord = 0, nid;
    EVP_PKEY_CTX *kctx = NULL;
    EVP_PKEY *pkey = NULL;
    JSValue ret = JS_EXCEPTION;
    (void)this_val;

    if (argc > 0 && !JS_IsUndefined(argv[0])) {
        c_ow = JS_ToCString(ctx, argv[0]);
        if (!c_ow) return JS_EXCEPTION;
        curve_str = c_ow;
    }
    nid = dyn_curve_nid(curve_str, &coord);
    if (nid < 0) {
        if (c_ow) JS_FreeCString(ctx, c_ow);
        return JS_ThrowRangeError(ctx, "ECDSA.generate: curve must be P-256 or P-384");
    }
    if (c_ow) JS_FreeCString(ctx, c_ow);

    if (RAND_status() != 1)
        return JS_ThrowInternalError(ctx, "ECDSA.generate: OS entropy unavailable");

    kctx = EVP_PKEY_CTX_new_id(EVP_PKEY_EC, NULL);
    if (!kctx || EVP_PKEY_keygen_init(kctx) <= 0 ||
        EVP_PKEY_CTX_set_ec_paramgen_curve_nid(kctx, nid) <= 0 ||
        EVP_PKEY_keygen(kctx, &pkey) <= 0) {
        JS_ThrowInternalError(ctx, "ECDSA.generate: key generation failed");
        goto done;
    }
    ret = dyn_pk_export_pem(ctx, pkey);

done:
    if (pkey) EVP_PKEY_free(pkey);
    if (kctx) EVP_PKEY_CTX_free(kctx);
    return ret;
}

static JSValue dyn_ecdsa_sign(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    const char *md_str = NULL;
    const EVP_MD *md;
    uint8_t *pem_bytes = NULL; size_t pem_len = 0;
    const uint8_t *msg; size_t msglen; const char *msg_ow = NULL;
    EVP_PKEY *k = NULL;
    EVP_MD_CTX *mctx = NULL;
    int coord, is_der = 0;
    size_t siglen = 0;
    uint8_t *sig = NULL;
    JSValue ret = JS_EXCEPTION;
    (void)this_val;

    if (argc < 3)
        return JS_ThrowTypeError(ctx, "ECDSA.sign(md, privateKey, msg[, opts])");
    md_str = JS_ToCString(ctx, argv[0]);
    if (!md_str) return JS_EXCEPTION;
    md = dyn_standalone_md(md_str);
    JS_FreeCString(ctx, md_str);
    if (!md) return JS_ThrowTypeError(ctx, "ECDSA.sign: unsupported hash algorithm");

    if (argc > 3 && JS_IsObject(argv[3])) {
        JSValue fv = JS_GetPropertyStr(ctx, argv[3], "format");
        if (JS_IsString(fv)) {
            const char *fs = JS_ToCString(ctx, fv);
            if (fs && !strcmp(fs, "der")) is_der = 1;
            if (fs) JS_FreeCString(ctx, fs);
        }
        JS_FreeValue(ctx, fv);
    }

    if (dyn_crypto_copy(ctx, argv[1], &pem_bytes, &pem_len)) return JS_EXCEPTION;
    k = jwt_pem_key((const char *)pem_bytes, pem_len, 1);
    free(pem_bytes);
    if (!k) return JS_ThrowTypeError(ctx, "ECDSA.sign: invalid private key PEM");
    coord = dyn_ec_coord(k);
    if (coord <= 0) {
        EVP_PKEY_free(k);
        return JS_ThrowTypeError(ctx, "ECDSA.sign: key must be EC P-256 or P-384");
    }

    if (dyn_crypto_data(ctx, argv[2], &msg, &msglen, &msg_ow)) {
        EVP_PKEY_free(k);
        return JS_EXCEPTION;
    }

    mctx = EVP_MD_CTX_new();
    if (!mctx || EVP_DigestSignInit(mctx, NULL, md, NULL, k) <= 0 ||
        EVP_DigestSign(mctx, NULL, &siglen, msg, msglen) <= 0) {
        JS_ThrowInternalError(ctx, "ECDSA.sign: initialization failed");
        goto done;
    }
    sig = (uint8_t *)malloc(siglen ? siglen : 1);
    if (!sig) { JS_ThrowOutOfMemory(ctx); goto done; }
    if (EVP_DigestSign(mctx, sig, &siglen, msg, msglen) <= 0) {
        JS_ThrowInternalError(ctx, "ECDSA.sign: signing failed");
        goto done;
    }

    if (is_der) {
        ret = dyn_crypto_u8array(ctx, sig, siglen);
    } else {
        uint8_t raw[96];
        if (jwt_der_to_raw(sig, siglen, coord, raw) < 0) {
            JS_ThrowInternalError(ctx, "ECDSA.sign: DER->raw conversion failed");
            goto done;
        }
        ret = dyn_crypto_u8array(ctx, raw, (size_t)(2 * coord));
    }

done:
    free(sig);
    if (mctx) EVP_MD_CTX_free(mctx);
    if (k) EVP_PKEY_free(k);
    if (msg_ow) JS_FreeCString(ctx, msg_ow);
    return ret;
}

static JSValue dyn_ecdsa_verify(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    const char *md_str = NULL;
    const EVP_MD *md;
    uint8_t *pem_bytes = NULL; size_t pem_len = 0;
    uint8_t *sig_bytes = NULL; size_t sig_len = 0;
    const uint8_t *msg; size_t msglen; const char *msg_ow = NULL;
    EVP_PKEY *k = NULL;
    EVP_MD_CTX *mctx = NULL;
    int coord, is_der = 0, ok = 0;
    uint8_t *der = NULL;
    int der_len = 0;
    const uint8_t *verify_sig; size_t verify_siglen;
    (void)this_val;

    if (argc < 4)
        return JS_ThrowTypeError(ctx, "ECDSA.verify(md, publicKey, msg, sig[, opts])");
    md_str = JS_ToCString(ctx, argv[0]);
    if (!md_str) return JS_EXCEPTION;
    md = dyn_standalone_md(md_str);
    JS_FreeCString(ctx, md_str);
    if (!md) return JS_ThrowTypeError(ctx, "ECDSA.verify: unsupported hash algorithm");

    if (argc > 4 && JS_IsObject(argv[4])) {
        JSValue fv = JS_GetPropertyStr(ctx, argv[4], "format");
        if (JS_IsString(fv)) {
            const char *fs = JS_ToCString(ctx, fv);
            if (fs && !strcmp(fs, "der")) is_der = 1;
            if (fs) JS_FreeCString(ctx, fs);
        }
        JS_FreeValue(ctx, fv);
    }

    if (dyn_crypto_copy(ctx, argv[1], &pem_bytes, &pem_len)) return JS_EXCEPTION;
    k = jwt_pem_key((const char *)pem_bytes, pem_len, 0);
    free(pem_bytes);
    if (!k) return JS_ThrowTypeError(ctx, "ECDSA.verify: invalid public key PEM");
    coord = dyn_ec_coord(k);
    if (coord <= 0) {
        EVP_PKEY_free(k);
        return JS_ThrowTypeError(ctx, "ECDSA.verify: key must be EC P-256 or P-384");
    }

    if (dyn_crypto_copy(ctx, argv[3], &sig_bytes, &sig_len)) {
        EVP_PKEY_free(k);
        return JS_EXCEPTION;
    }
    if (dyn_crypto_data(ctx, argv[2], &msg, &msglen, &msg_ow)) {
        free(sig_bytes);
        EVP_PKEY_free(k);
        return JS_EXCEPTION;
    }

    if (is_der) {
        verify_sig = sig_bytes;
        verify_siglen = sig_len;
    } else {
        if (sig_len != (size_t)(2 * coord)) {
            /* raw signature with incorrect length is simply not valid */
            goto finish;
        }
        if (jwt_raw_to_der(sig_bytes, coord, &der, &der_len) < 0)
            goto finish;
        verify_sig = der;
        verify_siglen = (size_t)der_len;
    }

    mctx = EVP_MD_CTX_new();
    if (mctx && EVP_DigestVerifyInit(mctx, NULL, md, NULL, k) > 0) {
        if (EVP_DigestVerify(mctx, verify_sig, verify_siglen, msg, msglen) == 1)
            ok = 1;
    }

finish:
    if (der) OPENSSL_free(der);
    free(sig_bytes);
    if (mctx) EVP_MD_CTX_free(mctx);
    if (k) EVP_PKEY_free(k);
    if (msg_ow) JS_FreeCString(ctx, msg_ow);
    return JS_NewBool(ctx, ok);
}

/* ECDH */
static JSValue dyn_ecdh_generate(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    return dyn_ecdsa_generate(ctx, this_val, argc, argv);
}

static JSValue dyn_ecdh_derive(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    uint8_t *priv_pem = NULL; size_t priv_len = 0;
    uint8_t *peer_pem = NULL; size_t peer_len = 0;
    EVP_PKEY *priv = NULL, *peer = NULL;
    EVP_PKEY_CTX *pctx = NULL;
    size_t secret_len = 0;
    uint8_t *secret = NULL;
    JSValue ret = JS_EXCEPTION;
    (void)this_val;

    if (argc < 2)
        return JS_ThrowTypeError(ctx, "ECDH.derive(privateKey, peerPublicKey)");

    if (dyn_crypto_copy(ctx, argv[0], &priv_pem, &priv_len)) return JS_EXCEPTION;
    priv = jwt_pem_key((const char *)priv_pem, priv_len, 1);
    free(priv_pem);
    if (!priv || !EVP_PKEY_is_a(priv, "EC")) {
        if (priv) EVP_PKEY_free(priv);
        return JS_ThrowTypeError(ctx, "ECDH.derive: invalid private EC key");
    }

    if (dyn_crypto_copy(ctx, argv[1], &peer_pem, &peer_len)) {
        EVP_PKEY_free(priv);
        return JS_EXCEPTION;
    }
    peer = jwt_pem_key((const char *)peer_pem, peer_len, 0);
    free(peer_pem);
    if (!peer || !EVP_PKEY_is_a(peer, "EC")) {
        EVP_PKEY_free(priv);
        if (peer) EVP_PKEY_free(peer);
        return JS_ThrowTypeError(ctx, "ECDH.derive: invalid peer public EC key");
    }

    pctx = EVP_PKEY_CTX_new(priv, NULL);
    if (!pctx || EVP_PKEY_derive_init(pctx) <= 0 ||
        EVP_PKEY_derive_set_peer(pctx, peer) <= 0 ||
        EVP_PKEY_derive(pctx, NULL, &secret_len) <= 0) {
        JS_ThrowTypeError(ctx, "ECDH.derive: derivation initialization failed");
        goto done;
    }
    secret = (uint8_t *)malloc(secret_len ? secret_len : 1);
    if (!secret) { JS_ThrowOutOfMemory(ctx); goto done; }
    if (EVP_PKEY_derive(pctx, secret, &secret_len) <= 0) {
        JS_ThrowTypeError(ctx, "ECDH.derive: derivation failed (small-order point refused)");
        goto done;
    }
    ret = dyn_crypto_u8array(ctx, secret, secret_len);

done:
    if (secret) {
        memset(secret, 0, secret_len);
        free(secret);
    }
    if (pctx) EVP_PKEY_CTX_free(pctx);
    if (peer) EVP_PKEY_free(peer);
    if (priv) EVP_PKEY_free(priv);
    return ret;
}

/* Scrypt */
static JSValue dyn_scrypt_js(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    const uint8_t *pwd = NULL, *salt = NULL;
    size_t pwdlen = 0, saltlen = 0;
    const char *pwd_ow = NULL, *salt_ow = NULL;
    int64_t N = 16384, r = 8, p = 1, keyLen = 32;
    uint8_t *out = NULL;
    JSValue ret = JS_EXCEPTION, v;
    (void)this_val;

    if (argc < 2)
        return JS_ThrowTypeError(ctx, "Scrypt(password, salt[, opts])");

    if (argc > 2 && JS_IsObject(argv[2])) {
        v = JS_GetPropertyStr(ctx, argv[2], "N");
        if (JS_IsException(v)) return JS_EXCEPTION;
        if (!JS_IsUndefined(v) && JS_ToInt64(ctx, &N, v)) { JS_FreeValue(ctx, v); return JS_EXCEPTION; }
        JS_FreeValue(ctx, v);
        v = JS_GetPropertyStr(ctx, argv[2], "r");
        if (JS_IsException(v)) return JS_EXCEPTION;
        if (!JS_IsUndefined(v) && JS_ToInt64(ctx, &r, v)) { JS_FreeValue(ctx, v); return JS_EXCEPTION; }
        JS_FreeValue(ctx, v);
        v = JS_GetPropertyStr(ctx, argv[2], "p");
        if (JS_IsException(v)) return JS_EXCEPTION;
        if (!JS_IsUndefined(v) && JS_ToInt64(ctx, &p, v)) { JS_FreeValue(ctx, v); return JS_EXCEPTION; }
        JS_FreeValue(ctx, v);
        v = JS_GetPropertyStr(ctx, argv[2], "keyLen");
        if (JS_IsException(v)) return JS_EXCEPTION;
        if (!JS_IsUndefined(v) && JS_ToInt64(ctx, &keyLen, v)) { JS_FreeValue(ctx, v); return JS_EXCEPTION; }
        JS_FreeValue(ctx, v);
    }
    if (N <= 1 || (N & (N - 1)) != 0 || N > (1 << 20))
        return JS_ThrowRangeError(ctx, "Scrypt: N must be a power of 2 between 2 and 1048576");
    if (r < 1 || r > 64)
        return JS_ThrowRangeError(ctx, "Scrypt: r must be 1..64");
    if (p < 1 || p > 32)
        return JS_ThrowRangeError(ctx, "Scrypt: p must be 1..32");
    if (keyLen < 1 || keyLen > (1 << 20))
        return JS_ThrowRangeError(ctx, "Scrypt: keyLen must be 1..1048576");
    if (128ULL * (uint64_t)N * (uint64_t)r > (1ULL << 30))
        return JS_ThrowRangeError(ctx, "Scrypt: 128*N*r exceeds memory budget (1 GiB)");

    if (dyn_crypto_data(ctx, argv[0], &pwd, &pwdlen, &pwd_ow)) return JS_EXCEPTION;
    if (dyn_crypto_data(ctx, argv[1], &salt, &saltlen, &salt_ow)) {
        if (pwd_ow) JS_FreeCString(ctx, pwd_ow);
        return JS_EXCEPTION;
    }

    out = (uint8_t *)malloc((size_t)keyLen);
    if (!out) {
        if (pwd_ow) JS_FreeCString(ctx, pwd_ow);
        if (salt_ow) JS_FreeCString(ctx, salt_ow);
        return JS_ThrowOutOfMemory(ctx);
    }
    if (EVP_PBE_scrypt((const char *)pwd, pwdlen, salt, saltlen,
                       (uint64_t)N, (uint64_t)r, (uint64_t)p,
                       1ULL << 30, out, (size_t)keyLen) != 1) {
        JS_ThrowInternalError(ctx, "Scrypt: derivation failed");
        goto done;
    }
    ret = dyn_crypto_u8array(ctx, out, (size_t)keyLen);

done:
    if (out) {
        memset(out, 0, (size_t)keyLen);
        free(out);
    }
    if (pwd_ow) JS_FreeCString(ctx, pwd_ow);
    if (salt_ow) JS_FreeCString(ctx, salt_ow);
    return ret;
}

/* X.509 certificate parser and self-signed generator */
static JSValue dyn_x509_parse(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    const uint8_t *data = NULL;
    size_t len = 0;
    const char *owned = NULL;
    X509 *x = NULL;
    BIO *bio = NULL;
    JSValue res = JS_EXCEPTION;
    (void)this_val;

    if (argc < 1)
        return JS_ThrowTypeError(ctx, "X509.parse(cert): certificate (PEM string or DER bytes) is required");

    if (dyn_crypto_data(ctx, argv[0], &data, &len, &owned) < 0)
        return JS_EXCEPTION;

    if (len >= 10 && !memcmp(data, "-----BEGIN", 10)) {
        bio = BIO_new_mem_buf(data, (int)len);
        if (bio) {
            x = PEM_read_bio_X509(bio, NULL, NULL, NULL);
            BIO_free(bio);
        }
    }
    if (!x) {
        const unsigned char *p = (const unsigned char *)data;
        x = d2i_X509(NULL, &p, (long)len);
    }
    if (owned)
        JS_FreeCString(ctx, owned);

    if (!x)
        return JS_ThrowTypeError(ctx, "X509.parse: invalid X.509 certificate (cannot decode PEM or DER)");

    res = JS_NewObject(ctx);
    if (JS_IsException(res)) {
        X509_free(x);
        return res;
    }

    {
        char *s_str = X509_NAME_oneline(X509_get_subject_name(x), NULL, 0);
        if (s_str) {
            JS_DefinePropertyValueStr(ctx, res, "subject", JS_NewString(ctx, s_str), JS_PROP_C_W_E);
            OPENSSL_free(s_str);
        }
        char *i_str = X509_NAME_oneline(X509_get_issuer_name(x), NULL, 0);
        if (i_str) {
            JS_DefinePropertyValueStr(ctx, res, "issuer", JS_NewString(ctx, i_str), JS_PROP_C_W_E);
            OPENSSL_free(i_str);
        }
    }

    {
        ASN1_INTEGER *serial = X509_get_serialNumber(x);
        BIGNUM *bn = ASN1_INTEGER_to_BN(serial, NULL);
        if (bn) {
            char *hex = BN_bn2hex(bn);
            if (hex) {
                JS_DefinePropertyValueStr(ctx, res, "serialNumber", JS_NewString(ctx, hex), JS_PROP_C_W_E);
                OPENSSL_free(hex);
            }
            BN_free(bn);
        }
    }

    JS_DefinePropertyValueStr(ctx, res, "version", JS_NewInt32(ctx, (int)X509_get_version(x) + 1), JS_PROP_C_W_E);

    {
        const ASN1_TIME *nb = X509_get0_notBefore(x);
        const ASN1_TIME *na = X509_get0_notAfter(x);
        BIO *b_nb = BIO_new(BIO_s_mem());
        BIO *b_na = BIO_new(BIO_s_mem());
        if (b_nb && nb) {
            ASN1_TIME_print(b_nb, nb);
            BUF_MEM *bm;
            BIO_get_mem_ptr(b_nb, &bm);
            JS_DefinePropertyValueStr(ctx, res, "notBefore", JS_NewStringLen(ctx, bm->data, bm->length), JS_PROP_C_W_E);
            BIO_free(b_nb);
        }
        if (b_na && na) {
            ASN1_TIME_print(b_na, na);
            BUF_MEM *bm;
            BIO_get_mem_ptr(b_na, &bm);
            JS_DefinePropertyValueStr(ctx, res, "notAfter", JS_NewStringLen(ctx, bm->data, bm->length), JS_PROP_C_W_E);
            BIO_free(b_na);
        }
    }

    {
        unsigned char md[32];
        unsigned int md_len = 0;
        if (X509_digest(x, EVP_sha256(), md, &md_len) && md_len == 32) {
            char hex_fp[65];
            size_t k;
            for (k = 0; k < 32; k++)
                snprintf(hex_fp + k * 2, 3, "%02x", md[k]);
            hex_fp[64] = 0;
            JS_DefinePropertyValueStr(ctx, res, "fingerprint", JS_NewString(ctx, hex_fp), JS_PROP_C_W_E);
        }
    }

    {
        JSValue sans_obj = JS_NewObject(ctx);
        JSValue dns_arr = JS_NewArray(ctx);
        JSValue ip_arr = JS_NewArray(ctx);
        JSValue email_arr = JS_NewArray(ctx);
        uint32_t dns_idx = 0, ip_idx = 0, email_idx = 0;
        GENERAL_NAMES *gens = (GENERAL_NAMES *)X509_get_ext_d2i(x, NID_subject_alt_name, NULL, NULL);
        if (gens) {
            int num = sk_GENERAL_NAME_num(gens);
            int i;
            for (i = 0; i < num; i++) {
                const GENERAL_NAME *gn = sk_GENERAL_NAME_value(gens, i);
                if (gn->type == GEN_DNS) {
                    const unsigned char *d = ASN1_STRING_get0_data(gn->d.dNSName);
                    int dlen = ASN1_STRING_length(gn->d.dNSName);
                    if (d && dlen > 0) {
                        JS_DefinePropertyValueUint32(ctx, dns_arr, dns_idx++, JS_NewStringLen(ctx, (const char *)d, (size_t)dlen), JS_PROP_C_W_E);
                    }
                } else if (gn->type == GEN_IPADD) {
                    const unsigned char *d = ASN1_STRING_get0_data(gn->d.iPAddress);
                    int dlen = ASN1_STRING_length(gn->d.iPAddress);
                    if (d && dlen == 4) {
                        char ipbuf[32];
                        snprintf(ipbuf, sizeof(ipbuf), "%u.%u.%u.%u", d[0], d[1], d[2], d[3]);
                        JS_DefinePropertyValueUint32(ctx, ip_arr, ip_idx++, JS_NewString(ctx, ipbuf), JS_PROP_C_W_E);
                    } else if (d && dlen == 16) {
                        char ipbuf[64];
                        snprintf(ipbuf, sizeof(ipbuf), "%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x",
                                 d[0], d[1], d[2], d[3], d[4], d[5], d[6], d[7],
                                 d[8], d[9], d[10], d[11], d[12], d[13], d[14], d[15]);
                        JS_DefinePropertyValueUint32(ctx, ip_arr, ip_idx++, JS_NewString(ctx, ipbuf), JS_PROP_C_W_E);
                    }
                } else if (gn->type == GEN_EMAIL) {
                    const unsigned char *d = ASN1_STRING_get0_data(gn->d.rfc822Name);
                    int dlen = ASN1_STRING_length(gn->d.rfc822Name);
                    if (d && dlen > 0) {
                        JS_DefinePropertyValueUint32(ctx, email_arr, email_idx++, JS_NewStringLen(ctx, (const char *)d, (size_t)dlen), JS_PROP_C_W_E);
                    }
                }
            }
            GENERAL_NAMES_free(gens);
        }
        JS_DefinePropertyValueStr(ctx, sans_obj, "dns", dns_arr, JS_PROP_C_W_E);
        JS_DefinePropertyValueStr(ctx, sans_obj, "ip", ip_arr, JS_PROP_C_W_E);
        JS_DefinePropertyValueStr(ctx, sans_obj, "email", email_arr, JS_PROP_C_W_E);
        JS_DefinePropertyValueStr(ctx, res, "sans", sans_obj, JS_PROP_C_W_E);
    }

    X509_free(x);
    return res;
}

static JSValue dyn_x509_generate(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    const char *subj = "localhost", *key_str = NULL;
    int64_t days = 365;
    size_t key_len = 0;
    EVP_PKEY *pkey = NULL;
    X509 *x = NULL;
    BIO *bio = NULL;
    JSValue ret = JS_EXCEPTION, v;
    (void)this_val;

    if (argc < 1 || !JS_IsObject(argv[0]))
        return JS_ThrowTypeError(ctx, "X509.generateSelfSigned({ key, subject?, days? })");

    v = JS_GetPropertyStr(ctx, argv[0], "key");
    if (JS_IsException(v)) return JS_EXCEPTION;
    key_str = JS_ToCStringLen(ctx, &key_len, v);
    JS_FreeValue(ctx, v);
    if (!key_str)
        return JS_ThrowTypeError(ctx, "X509.generateSelfSigned: key (PEM private key) is required");

    v = JS_GetPropertyStr(ctx, argv[0], "subject");
    if (!JS_IsUndefined(v) && !JS_IsNull(v)) {
        subj = JS_ToCString(ctx, v);
        JS_FreeValue(ctx, v);
    }
    v = JS_GetPropertyStr(ctx, argv[0], "days");
    if (!JS_IsUndefined(v) && !JS_IsNull(v)) {
        JS_ToInt64(ctx, &days, v);
        JS_FreeValue(ctx, v);
        if (days < 1) days = 1;
    }

    bio = BIO_new_mem_buf(key_str, (int)key_len);
    if (bio) {
        pkey = PEM_read_bio_PrivateKey(bio, NULL, NULL, NULL);
        BIO_free(bio);
    }
    JS_FreeCString(ctx, key_str);

    if (!pkey)
        return JS_ThrowTypeError(ctx, "X509.generateSelfSigned: invalid private key PEM");

    x = X509_new();
    if (!x) {
        EVP_PKEY_free(pkey);
        return JS_ThrowOutOfMemory(ctx);
    }

    ASN1_INTEGER_set(X509_get_serialNumber(x), 1);
    X509_set_version(x, 2); /* v3 */
    X509_gmtime_adj(X509_getm_notBefore(x), 0);
    X509_gmtime_adj(X509_getm_notAfter(x), (long)days * 86400L);
    X509_set_pubkey(x, pkey);

    {
        X509_NAME *name = X509_get_subject_name(x);
        X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC, (const unsigned char *)(subj ? subj : "localhost"), -1, -1, 0);
        X509_set_issuer_name(x, name);
    }

    if (!X509_sign(x, pkey, EVP_sha256())) {
        X509_free(x);
        EVP_PKEY_free(pkey);
        return JS_ThrowInternalError(ctx, "X509.generateSelfSigned: certificate signing failed");
    }

    bio = BIO_new(BIO_s_mem());
    if (bio && PEM_write_bio_X509(bio, x)) {
        BUF_MEM *bm;
        BIO_get_mem_ptr(bio, &bm);
        ret = JS_NewStringLen(ctx, bm->data, bm->length);
    } else {
        ret = JS_ThrowOutOfMemory(ctx);
    }

    if (bio) BIO_free(bio);
    X509_free(x);
    EVP_PKEY_free(pkey);
    return ret;
}

#endif /* CONFIG_TLS */



static const JSCFunctionListEntry dyn_bcrypt_funcs[] = {
    JS_CFUNC_DEF("hash",   1, dyn_bcrypt_hash_js),
    JS_CFUNC_DEF("verify", 2, dyn_bcrypt_verify_js),
};

static const JSCFunctionListEntry dyn_argon2_funcs[] = {
    JS_CFUNC_DEF("hash",   2, dyn_argon2_hash_js),
    JS_CFUNC_DEF("verify", 3, dyn_argon2_verify_js),
};

#ifdef CONFIG_TLS
static const JSCFunctionListEntry dyn_rsa_funcs[] = {
    JS_CFUNC_DEF("generate", 0, dyn_rsa_generate),
    JS_CFUNC_DEF("sign",     3, dyn_rsa_sign),
    JS_CFUNC_DEF("verify",   4, dyn_rsa_verify),
};

static const JSCFunctionListEntry dyn_ecdsa_funcs[] = {
    JS_CFUNC_DEF("generate", 0, dyn_ecdsa_generate),
    JS_CFUNC_DEF("sign",     3, dyn_ecdsa_sign),
    JS_CFUNC_DEF("verify",   4, dyn_ecdsa_verify),
};

static const JSCFunctionListEntry dyn_ecdh_funcs[] = {
    JS_CFUNC_DEF("generate", 0, dyn_ecdh_generate),
    JS_CFUNC_DEF("derive",   2, dyn_ecdh_derive),
};

static const JSCFunctionListEntry dyn_x509_funcs[] = {
    JS_CFUNC_DEF("parse",              1, dyn_x509_parse),
    JS_CFUNC_DEF("generateSelfSigned", 1, dyn_x509_generate),
};
#endif

static const JSCFunctionListEntry dyn_crypto_funcs[] = {
    JS_CFUNC_MAGIC_DEF("HMAC",    3, dyn_crypto_hmac, 0),
    JS_CFUNC_MAGIC_DEF("HMACHex", 3, dyn_crypto_hmac, 1),
    JS_CFUNC_DEF("TimingSafeEqual", 2, dyn_crypto_ct_equal),
    JS_CFUNC_MAGIC_DEF("HKDF",    1, dyn_crypto_kdf, 0),
    JS_CFUNC_MAGIC_DEF("PBKDF2",  1, dyn_crypto_kdf, 1),
    JS_CFUNC_DEF("RandomBytes", 0, dyn_crypto_random_bytes),
    JS_CFUNC_DEF("HOTPGenerate", 2, dyn_crypto_hotp),
    JS_CFUNC_DEF("TOTPGenerate", 1, dyn_crypto_totp),
#ifdef CONFIG_TLS
    JS_CFUNC_DEF("Ed25519Generate", 0, dyn_ed25519_generate),
    JS_CFUNC_DEF("Ed25519Sign", 2, dyn_ed25519_sign),
    JS_CFUNC_DEF("Ed25519Verify", 3, dyn_ed25519_verify),
    JS_CFUNC_DEF("X25519Generate", 0, dyn_x25519_generate),
    JS_CFUNC_DEF("X25519Derive", 2, dyn_x25519_derive),
    JS_CFUNC_DEF("Scrypt", 2, dyn_scrypt_js),
    JS_OBJECT_DEF("RSA", dyn_rsa_funcs, countof(dyn_rsa_funcs), JS_PROP_CONFIGURABLE),
    JS_OBJECT_DEF("ECDSA", dyn_ecdsa_funcs, countof(dyn_ecdsa_funcs), JS_PROP_CONFIGURABLE),
    JS_OBJECT_DEF("ECDH", dyn_ecdh_funcs, countof(dyn_ecdh_funcs), JS_PROP_CONFIGURABLE),
    JS_OBJECT_DEF("X509", dyn_x509_funcs, countof(dyn_x509_funcs), JS_PROP_CONFIGURABLE),
#endif
    JS_OBJECT_DEF("Bcrypt", dyn_bcrypt_funcs, countof(dyn_bcrypt_funcs), JS_PROP_CONFIGURABLE),
    JS_OBJECT_DEF("Argon2id", dyn_argon2_funcs, countof(dyn_argon2_funcs), JS_PROP_CONFIGURABLE),
    JS_CFUNC_DEF("JWTSign", 2, dyn_crypto_jwt_sign),
    JS_CFUNC_DEF("JWTVerify", 3, dyn_crypto_jwt_verify),
};

static int dyn_hash_init_module(JSContext *ctx, JSModuleDef *m)
{
    if (dyn_register_plain_class(ctx, m, &dyn_hasher_class_id, &dyn_hasher_class,
                           dyn_hasher_proto, countof(dyn_hasher_proto),
                           dyn_hasher_ctor, "Hasher") < 0)
        return -1;
    return JS_SetModuleExportList(ctx, m, dyn_hash_funcs,
                                  countof(dyn_hash_funcs));
}

int js_nat_init_hash(JSContext *ctx)
{
    JSModuleDef *m = JS_NewCModule(ctx, "dyna:hash", dyn_hash_init_module);
    if (!m)
        return -1;
    JS_AddModuleExport(ctx, m, "Hasher");
    return JS_AddModuleExportList(ctx, m, dyn_hash_funcs,
                                  countof(dyn_hash_funcs));
}

static int dyn_crypto_init_module(JSContext *ctx, JSModuleDef *m)
{
    if (dyn_register_class(ctx, m, &dyn_hmac_class_id, &dyn_hmac_class,
                           dyn_hmac_proto, countof(dyn_hmac_proto),
                           dyn_hmac_ctor, "Hmac") < 0)
        return -1;
#ifdef CONFIG_TLS
    if (dyn_register_class(ctx, m, &dyn_aesgcm_class_id, &dyn_aesgcm_class,
                           dyn_aead_proto, countof(dyn_aead_proto),
                           dyn_aesgcm_ctor, "AESGCM") < 0)
        return -1;
    if (dyn_register_class(ctx, m, &dyn_chapoly_class_id, &dyn_chapoly_class,
                           dyn_aead_proto, countof(dyn_aead_proto),
                           dyn_chapoly_ctor, "ChaCha20Poly1305") < 0)
        return -1;
#endif
    return JS_SetModuleExportList(ctx, m, dyn_crypto_funcs,
                                  countof(dyn_crypto_funcs));
}

int js_nat_init_crypto(JSContext *ctx)
{
    JSModuleDef *m = JS_NewCModule(ctx, "dyna:crypto", dyn_crypto_init_module);
    if (!m)
        return -1;
    JS_AddModuleExport(ctx, m, "Hmac");
#ifdef CONFIG_TLS
    JS_AddModuleExport(ctx, m, "AESGCM");
    JS_AddModuleExport(ctx, m, "ChaCha20Poly1305");
#endif
    return JS_AddModuleExportList(ctx, m, dyn_crypto_funcs,
                                  countof(dyn_crypto_funcs));
}


#endif /* CONFIG_NATIVE_MODULES && CONFIG_NATIVE_MODULE_CRYPTO */
