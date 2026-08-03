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
 * Full API: docs/dynajs-guide/API.md.
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
    if (dyn_crypto_data(ctx, argv[2], &dp, &dl, &down))
        return JS_EXCEPTION;
    dyn_hmac_finish(a, k0, dp, dl, out); /* pure C; no JS between resolve and use */
    if (down)
        JS_FreeCString(ctx, down);

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
        v = JS_GetPropertyStr(ctx, argv[0], "iterations");
        if (JS_IsException(v)) goto done;
        if (!JS_IsUndefined(v) && JS_ToInt64(ctx, &iters, v)) { JS_FreeValue(ctx, v); goto done; }
        JS_FreeValue(ctx, v);
        if (iters < 1 || iters > (1 << 26)) {
            JS_ThrowRangeError(ctx, "iterations must be 1..67108864");
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
    if (n)
        dyn_os_entropy(buf, (size_t)n);
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

    JS_CFUNC_MAGIC_DEF("XXHash64", 2, dyn_hash_xx, 0),
    JS_CFUNC_MAGIC_DEF("XXHash32", 2, dyn_hash_xx, 1),
    JS_CFUNC_MAGIC_DEF("SHA3_224", 1, dyn_sha3, 0),
    JS_CFUNC_MAGIC_DEF("SHA3_256", 1, dyn_sha3, 1),
    JS_CFUNC_MAGIC_DEF("SHA3_384", 1, dyn_sha3, 2),
    JS_CFUNC_MAGIC_DEF("SHA3_512", 1, dyn_sha3, 3),
    JS_CFUNC_MAGIC_DEF("Keccak256", 1, dyn_sha3, 4),
    JS_CFUNC_MAGIC_DEF("SHAKE128", 2, dyn_sha3, 5),
    JS_CFUNC_MAGIC_DEF("SHAKE256", 2, dyn_sha3, 6),
    JS_CFUNC_MAGIC_DEF("SHA3_224Hex", 1, dyn_sha3, 8),
    JS_CFUNC_MAGIC_DEF("SHA3_256Hex", 1, dyn_sha3, 9),
    JS_CFUNC_MAGIC_DEF("SHA3_384Hex", 1, dyn_sha3, 10),
    JS_CFUNC_MAGIC_DEF("SHA3_512Hex", 1, dyn_sha3, 11),
    JS_CFUNC_MAGIC_DEF("Keccak256Hex", 1, dyn_sha3, 12),
    JS_CFUNC_MAGIC_DEF("SHAKE128Hex", 2, dyn_sha3, 13),
    JS_CFUNC_MAGIC_DEF("SHAKE256Hex", 2, dyn_sha3, 14),

    JS_CFUNC_MAGIC_DEF("BLAKE3",     2, dyn_blake, 0),
    JS_CFUNC_MAGIC_DEF("BLAKE2b",    2, dyn_blake, 1),
    JS_CFUNC_MAGIC_DEF("BLAKE2s",    2, dyn_blake, 2),
    JS_CFUNC_MAGIC_DEF("Murmur3_128", 2, dyn_blake, 3),
    JS_CFUNC_MAGIC_DEF("BLAKE3Hex",  2, dyn_blake, 8),
    JS_CFUNC_MAGIC_DEF("BLAKE2bHex", 2, dyn_blake, 9),
    JS_CFUNC_MAGIC_DEF("BLAKE2sHex", 2, dyn_blake, 10),
    JS_CFUNC_MAGIC_DEF("Murmur3_128Hex", 2, dyn_blake, 11),
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
    JS_CFUNC_MAGIC_DEF("seal", 3, dyn_aead_op, 0),
    JS_CFUNC_MAGIC_DEF("open", 3, dyn_aead_op, 1),
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

static const JSCFunctionListEntry dyn_crypto_funcs[] = {
    JS_CFUNC_MAGIC_DEF("HMAC",    3, dyn_crypto_hmac, 0),
    JS_CFUNC_MAGIC_DEF("HMACHex", 3, dyn_crypto_hmac, 1),
    JS_CFUNC_DEF("TimingSafeEqual", 2, dyn_crypto_ct_equal),
    JS_CFUNC_MAGIC_DEF("HKDF",    1, dyn_crypto_kdf, 0),
    JS_CFUNC_MAGIC_DEF("PBKDF2",  1, dyn_crypto_kdf, 1),
    JS_CFUNC_DEF("RandomBytes", 1, dyn_crypto_random_bytes),
    JS_CFUNC_DEF("HOTPGenerate", 3, dyn_crypto_hotp),
    JS_CFUNC_DEF("TOTPGenerate", 2, dyn_crypto_totp),
#ifdef CONFIG_TLS
    JS_CFUNC_DEF("Ed25519Generate", 0, dyn_ed25519_generate),
    JS_CFUNC_DEF("Ed25519Sign", 2, dyn_ed25519_sign),
    JS_CFUNC_DEF("Ed25519Verify", 3, dyn_ed25519_verify),
    JS_CFUNC_DEF("X25519Generate", 0, dyn_x25519_generate),
    JS_CFUNC_DEF("X25519Derive", 2, dyn_x25519_derive),
#endif
    JS_CFUNC_DEF("JWTSign", 3, dyn_crypto_jwt_sign),
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
