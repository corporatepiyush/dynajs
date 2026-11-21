#include "crypto-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_CRYPTO)

int dyn_crypto_data(JSContext* ctx, JSValueConst v, const uint8_t** pdata,
    size_t* plen, const char** powned)
{
    *powned = NULL;
    if (JS_IsString(v)) {
        size_t n;
        const char* s = JS_ToCStringLen(ctx, &n, v);
        if (!s)
            return -1;
        *powned = s;
        *pdata = (const uint8_t*)s;
        *plen = n;
        return 0;
    }
    {
        size_t off, len, bpe, ab_size;
        uint8_t* base;
        JSValue ab = JS_GetBufferKind(v) == JS_BUFFER_KIND_VIEW ? JS_GetArrayBufferView(ctx, v, &off, &len, &bpe) : JS_EXCEPTION;
        if (!JS_IsException(ab)) {
            base = JS_GetArrayBuffer(ctx, &ab_size, ab);
            JS_FreeValue(ctx, ab);
            if (!base)
                return -1;
            if (off > ab_size || len > ab_size - off) {
                JS_ThrowRangeError(ctx, "typed array out of bounds");
                return -1;
            }
            *pdata = base + off;
            *plen = len;
            return 0;
        }
        JS_FreeValue(ctx, JS_GetException(ctx));
        {
            size_t n;
            uint8_t* p = JS_GetArrayBuffer(ctx, &n, v);
            if (p) {
                *pdata = p;
                *plen = n;
                return 0;
            }
            JS_FreeValue(ctx, JS_GetException(ctx));
        }
    }
    {
        size_t n;
        const char* s = JS_ToCStringLen(ctx, &n, v);
        if (!s)
            return -1;
        *powned = s;
        *pdata = (const uint8_t*)s;
        *plen = n;
        return 0;
    }
}

int dyn_crypto_secret(JSContext* ctx, JSValueConst v, const uint8_t** pdata,
    size_t* plen, const char** powned)
{
    *powned = NULL;
    if (!JS_IsString(v)) {
        JSValue view = JS_GetBufferKind(v) == JS_BUFFER_KIND_VIEW ? JS_GetArrayBufferView(ctx, v, NULL, NULL, NULL) : JS_EXCEPTION;
        if (JS_IsException(view)) {
            size_t n;
            JS_FreeValue(ctx, JS_GetException(ctx));
            if (!JS_GetArrayBuffer(ctx, &n, v)) {
                JS_FreeValue(ctx, JS_GetException(ctx));
                JS_ThrowTypeError(ctx, "a key or secret must be a string, an ArrayBuffer or a byte view (got %s)",
                    JS_IsUndefined(v) ? "undefined" : JS_IsNull(v) ? "null"
                        : JS_IsNumber(v)                           ? "a number"
                        : JS_IsBool(v)                             ? "a boolean"
                                                                   : "an object that is not a byte view");
                return -1;
            }
        } else {
            JS_FreeValue(ctx, view);
        }
    }
    return dyn_crypto_data(ctx, v, pdata, plen, powned);
}

#ifdef CONFIG_OPENSSL
int dyn_pem_no_passphrase(char* buf, int size, int rwflag, void* userdata)
{
    (void)buf;
    (void)size;
    (void)rwflag;
    (void)userdata;
    return 0;
}
#endif

JSValue dyn_crypto_hex(JSContext* ctx, const uint8_t* data, size_t len)
{
    char stackbuf[DYN_HASH_MAX_DIGEST * 2];
    if (len > DYN_HASH_MAX_DIGEST) {
        char* heap;
        JSValue r;
        if (len > (SIZE_MAX - 1) / 2)
            return JS_ThrowRangeError(ctx, "hex: input too large to encode");
        heap = (char*)malloc(len * 2);
        if (!heap)
            return JS_ThrowOutOfMemory(ctx);
        dyn_codec_hex_encode(data, len, heap);
        r = JS_NewStringLen(ctx, heap, len * 2);
        free(heap);
        return r;
    }
    dyn_codec_hex_encode(data, len, stackbuf);
    return JS_NewStringLen(ctx, stackbuf, len * 2);
}

static uint32_t dyn_adler32(const uint8_t* p, size_t n);

static JSValue dyn_crypto_crc32(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv, int magic)
{
    const uint8_t* data;
    size_t len;
    const char* owned;
    uint8_t* hold;
    int live;
    uint32_t crc;
    (void)this_val;
    (void)argc;

    if (dyn_crypto_data(ctx, argv[0], &data, &len, &owned))
        return JS_EXCEPTION;
    if (dyn_crypto_private(&live, &data, len, owned, &hold) < 0)
        return JS_ThrowOutOfMemory(ctx);
    crc = DYN_OFFLOAD_CPU_IF(live, magic == 2 ? dyn_adler32(data, len) : magic ? dyn_crc32c(data, len)
                                                                      : dyn_crc32(data, len));
    if (hold)
        free(hold);
    if (owned)
        JS_FreeCString(ctx, owned);
    return JS_NewUint32(ctx, crc);
}

static uint32_t dyn_adler32(const uint8_t* p, size_t n)
{
    uint32_t a = 1, b = 0;
    while (n) {
        size_t k = n < 5552 ? n : 5552;
        n -= k;
        while (k--) {
            a += *p++;
            b += a;
        }
        a %= 65521u;
        b %= 65521u;
    }
    return (b << 16) | a;
}

static JSValue dyn_crypto_ct_equal(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    const uint8_t *a, *b;
    size_t alen, blen;
    const char *aowned, *bowned;
    uint8_t* acopy;
    int r;
    (void)this_val;
    (void)argc;

    if (dyn_crypto_secret(ctx, argv[0], &a, &alen, &aowned))
        return JS_EXCEPTION;
    acopy = (uint8_t*)malloc(alen ? alen : 1);
    if (!acopy) {
        if (aowned)
            JS_FreeCString(ctx, aowned);
        return JS_ThrowOutOfMemory(ctx);
    }
    if (alen)
        memcpy(acopy, a, alen);
    if (aowned)
        JS_FreeCString(ctx, aowned);
    if (dyn_crypto_secret(ctx, argv[1], &b, &blen, &bowned)) {
        dyn_wipe(acopy, alen ? alen : 1);
        free(acopy);
        return JS_EXCEPTION;
    }
    r = (alen == blen) && dyn_ct_equal(acopy, b, alen);
    if (bowned)
        JS_FreeCString(ctx, bowned);
    dyn_wipe(acopy, alen ? alen : 1);
    free(acopy);
    return JS_NewBool(ctx, r);
}

static JSValue dyn_crypto_random_bytes(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    int64_t n = 32;
    uint8_t* buf;
    JSValue out;
    (void)this_val;

    if (argc > 0 && !JS_IsUndefined(argv[0])) {
        if (JS_ToInt64(ctx, &n, argv[0]))
            return JS_EXCEPTION;
    }
    if (n < 0 || n > (1 << 24))
        return JS_ThrowRangeError(ctx, "count must be 0..16777216");
    buf = (uint8_t*)malloc(n ? (size_t)n : 1);
    if (!buf)
        return JS_ThrowOutOfMemory(ctx);
    if (n && dyn_os_entropy(buf, (size_t)n) < 0) {
        dyn_wipe(buf, (size_t)n);
        free(buf);
        return JS_ThrowInternalError(ctx,
            "randomBytes: OS entropy unavailable");
    }
    out = dyn_nat_u8_copy(ctx, buf, (size_t)n);
    dyn_wipe(buf, n ? (size_t)n : 1);
    free(buf);
    return out;
}

JSClassID dyn_hmac_class_id;

static const JSClassDef dyn_hmac_class = { "Hmac", .finalizer = dyn_res_finalizer };

static const JSCFunctionListEntry dyn_hmac_proto[] = {
    JS_CFUNC_MAGIC_DEF("sign", 1, dyn_hmac_sign, 0),
    JS_CFUNC_MAGIC_DEF("signHex", 1, dyn_hmac_sign, 1),
    JS_CFUNC_DEF("verify", 2, dyn_hmac_verify),
    JS_CFUNC_DEF("update", 1, dyn_hmac_update_js),
    JS_CFUNC_MAGIC_DEF("digest", 0, dyn_hmac_digest, 0),
    JS_CFUNC_MAGIC_DEF("digestHex", 0, dyn_hmac_digest, 1),
    JS_CFUNC_DEF("reset", 0, dyn_hmac_reset_js),
    JS_CGETSET_DEF("algorithm", dyn_hmac_get_algorithm, NULL),
    JS_CGETSET_DEF("digestSize", dyn_hmac_get_size, NULL),
};

const JSCFunctionListEntry dyn_hash_funcs[] = {
    JS_CFUNC_MAGIC_DEF("MD5", 1, dyn_crypto_hash, DYN_HASH_MD5),
    JS_CFUNC_MAGIC_DEF("MD5Hex", 1, dyn_crypto_hash_hex, DYN_HASH_MD5),
    JS_CFUNC_MAGIC_DEF("SHA1", 1, dyn_crypto_hash, DYN_HASH_SHA1),
    JS_CFUNC_MAGIC_DEF("SHA1Hex", 1, dyn_crypto_hash_hex, DYN_HASH_SHA1),
    JS_CFUNC_MAGIC_DEF("SHA224", 1, dyn_crypto_hash, DYN_HASH_SHA224),
    JS_CFUNC_MAGIC_DEF("SHA224Hex", 1, dyn_crypto_hash_hex, DYN_HASH_SHA224),
    JS_CFUNC_MAGIC_DEF("SHA256", 1, dyn_crypto_hash, DYN_HASH_SHA256),
    JS_CFUNC_MAGIC_DEF("SHA256Hex", 1, dyn_crypto_hash_hex, DYN_HASH_SHA256),
    JS_CFUNC_MAGIC_DEF("SHA384", 1, dyn_crypto_hash, DYN_HASH_SHA384),
    JS_CFUNC_MAGIC_DEF("SHA384Hex", 1, dyn_crypto_hash_hex, DYN_HASH_SHA384),
    JS_CFUNC_MAGIC_DEF("SHA512", 1, dyn_crypto_hash, DYN_HASH_SHA512),
    JS_CFUNC_MAGIC_DEF("SHA512Hex", 1, dyn_crypto_hash_hex, DYN_HASH_SHA512),

    JS_CFUNC_MAGIC_DEF("CRC32", 1, dyn_crypto_crc32, 0),
    JS_CFUNC_MAGIC_DEF("CRC32C", 1, dyn_crypto_crc32, 1),
    JS_CFUNC_MAGIC_DEF("Adler32", 1, dyn_crypto_crc32, 2),
    JS_CFUNC_DEF("CDCChunks", 2, dyn_hash_cdc),

    JS_CFUNC_MAGIC_DEF("XXHash64", 1, dyn_hash_xx, 0),
    JS_CFUNC_MAGIC_DEF("XXHash32", 1, dyn_hash_xx, 1),
    JS_CFUNC_MAGIC_DEF("XXH3_64", 1, dyn_hash_xx, 2),
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

    JS_CFUNC_MAGIC_DEF("BLAKE3", 1, dyn_blake, 0),
    JS_CFUNC_MAGIC_DEF("BLAKE2b", 1, dyn_blake, 1),
    JS_CFUNC_MAGIC_DEF("BLAKE2s", 1, dyn_blake, 2),
    JS_CFUNC_MAGIC_DEF("Murmur3_128", 1, dyn_blake, 3),
    JS_CFUNC_MAGIC_DEF("BLAKE3Hex", 1, dyn_blake, 8),
    JS_CFUNC_MAGIC_DEF("BLAKE2bHex", 1, dyn_blake, 9),
    JS_CFUNC_MAGIC_DEF("BLAKE2sHex", 1, dyn_blake, 10),
    JS_CFUNC_MAGIC_DEF("Murmur3_128Hex", 1, dyn_blake, 11),
};

#ifdef CONFIG_OPENSSL
JSClassID dyn_aesgcm_class_id;

JSClassID dyn_chapoly_class_id;

static const JSClassDef dyn_aesgcm_class = { "AESGCM", .finalizer = dyn_res_finalizer };

static const JSClassDef dyn_chapoly_class = { "ChaCha20Poly1305", .finalizer = dyn_res_finalizer };

static JSValue dyn_aesgcm_ctor(JSContext* ctx, JSValueConst nt, int argc,
    JSValueConst* argv)
{
    return dyn_aead_ctor_common(ctx, nt, argc, argv, 0);
}

static JSValue dyn_chapoly_ctor(JSContext* ctx, JSValueConst nt, int argc,
    JSValueConst* argv)
{
    return dyn_aead_ctor_common(ctx, nt, argc, argv, 1);
}

static const JSCFunctionListEntry dyn_aead_proto[] = {
    JS_CFUNC_MAGIC_DEF("seal", 2, dyn_aead_op, 0),
    JS_CFUNC_MAGIC_DEF("open", 2, dyn_aead_op, 1),
    JS_CFUNC_MAGIC_DEF("sealInto", 3, dyn_aead_op_into, 0),
    JS_CFUNC_MAGIC_DEF("openInto", 3, dyn_aead_op_into, 1),
    JS_CFUNC_DEF("sealRandom", 1, dyn_aead_seal_random),
};

static int dyn_streq_ci(const char* a, const char* b)
{
    while (*a && *b) {
        char ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z')
            ca += 32;
        if (cb >= 'A' && cb <= 'Z')
            cb += 32;
        if (ca != cb)
            return 0;
        a++;
        b++;
    }
    return *a == *b;
}

const EVP_MD* dyn_standalone_md(const char* name)
{
    if (!name)
        return NULL;
    if (dyn_streq_ci(name, "sha1") || dyn_streq_ci(name, "sha-1"))
        return EVP_sha1();
    if (dyn_streq_ci(name, "sha256") || dyn_streq_ci(name, "sha-256"))
        return EVP_sha256();
    if (dyn_streq_ci(name, "sha384") || dyn_streq_ci(name, "sha-384"))
        return EVP_sha384();
    if (dyn_streq_ci(name, "sha512") || dyn_streq_ci(name, "sha-512"))
        return EVP_sha512();
    return NULL;
}

int dyn_crypto_copy(JSContext* ctx, JSValueConst v, uint8_t** out, size_t* outlen)
{
    const uint8_t* d;
    size_t n;
    const char* ow;
    if (dyn_crypto_data(ctx, v, &d, &n, &ow))
        return -1;
    *out = (uint8_t*)malloc(n ? n : 1);
    if (!*out) {
        if (ow)
            JS_FreeCString(ctx, ow);
        JS_ThrowOutOfMemory(ctx);
        return -1;
    }
    if (n)
        memcpy(*out, d, n);
    *outlen = n;
    if (ow)
        JS_FreeCString(ctx, ow);
    return 0;
}

JSValue dyn_pk_export_pem(JSContext* ctx, EVP_PKEY* k)
{
    BIO *bio_priv = NULL, *bio_pub = NULL;
    char *priv_ptr = NULL, *pub_ptr = NULL;
    long priv_len = 0, pub_len = 0;
    JSValue obj = JS_EXCEPTION, priv_val = JS_UNDEFINED, pub_val = JS_UNDEFINED;

    bio_priv = BIO_new(BIO_s_mem());
    bio_pub = BIO_new(BIO_s_mem());
    if (!bio_priv || !bio_pub) {
        JS_ThrowOutOfMemory(ctx);
        goto done;
    }
    if (!PEM_write_bio_PrivateKey(bio_priv, k, NULL, NULL, 0, NULL, NULL) || !PEM_write_bio_PUBKEY(bio_pub, k)) {
        JS_ThrowInternalError(ctx, "failed to serialize PEM key");
        goto done;
    }
    priv_len = BIO_get_mem_data(bio_priv, &priv_ptr);
    pub_len = BIO_get_mem_data(bio_pub, &pub_ptr);
    if (priv_len <= 0 || pub_len <= 0 || !priv_ptr || !pub_ptr) {
        JS_ThrowInternalError(ctx, "failed to read PEM data");
        goto done;
    }
    priv_val = JS_NewStringLen(ctx, priv_ptr, (size_t)priv_len);
    pub_val = JS_NewStringLen(ctx, pub_ptr, (size_t)pub_len);
    if (JS_IsException(priv_val) || JS_IsException(pub_val))
        goto done;

    obj = JS_NewObject(ctx);
    if (JS_IsException(obj))
        goto done;
    JS_DefinePropertyValueStr(ctx, obj, "privateKey", priv_val, JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, obj, "publicKey", pub_val, JS_PROP_C_W_E);
    priv_val = pub_val = JS_UNDEFINED;

done:
    if (bio_priv) {
        char* pp = NULL;
        long pn = BIO_get_mem_data(bio_priv, &pp);
        if (pp && pn > 0)
            dyn_wipe(pp, (size_t)pn);
        BIO_free(bio_priv);
    }
    if (bio_pub)
        BIO_free(bio_pub);
    JS_FreeValue(ctx, priv_val);
    JS_FreeValue(ctx, pub_val);
    return obj;
}
#endif

typedef struct {
    int refs;
    int sync;
    int started;
    int has_err;
    JSValue err;
} DynKdfState;

typedef struct {
    int which;
    int argc;
    DynKdfState* st;
    JSValue argv[];
} DynKdfJob;

typedef struct DynKdfWait {
    JSValue resolve;
    int woken;
    struct DynKdfWait* next;
} DynKdfWait;

static _Thread_local DynKdfJob* dyn_kdf_cur;
static _Thread_local int dyn_kdf_inflight;
static _Thread_local int dyn_kdf_inflight_peak;
static _Thread_local DynKdfWait* dyn_kdf_waiters;

static void dyn_kdf_state_put(JSContext* ctx, DynKdfState* st)
{
    if (--st->refs > 0)
        return;
    if (st->has_err)
        JS_FreeValue(ctx, st->err);
    free(st);
}

int dyn_kdf_meta(JSContext* ctx, JSValueConst opts, const char* who, uint32_t* cap)
{
    static const char* const keys[] = { "maxConcurrent" };
    JSValue d, v;
    double x;
    d = JS_GetPropertyStr(ctx, opts, "dyna");
    if (JS_IsException(d))
        return -1;
    if (JS_IsUndefined(d))
        return 0;
    if (!JS_IsObject(d) || JS_IsFunction(ctx, d)) {
        JS_FreeValue(ctx, d);
        JS_ThrowTypeError(ctx, "%s: dyna must be an object", who);
        return -1;
    }
    if (dyn_opts_strict(ctx, d, keys, 1)) {
        JS_FreeValue(ctx, d);
        return -1;
    }
    v = JS_GetPropertyStr(ctx, d, "maxConcurrent");
    JS_FreeValue(ctx, d);
    if (JS_IsException(v))
        return -1;
    if (JS_IsUndefined(v))
        return 0;
    if (!JS_IsNumber(v)) {
        JS_FreeValue(ctx, v);
        JS_ThrowTypeError(ctx, "%s: dyna.maxConcurrent must be a number", who);
        return -1;
    }
    JS_ToFloat64(ctx, &x, v);
    if (!(x >= 1 && x <= 2147483647.0) || x != floor(x)) {
        JS_ThrowRangeError(ctx, "%s: dyna.maxConcurrent must be an integer 1..2147483647", who);
        return -1;
    }
    *cap = (uint32_t)x;
    return 0;
}

int dyn_kdf_begin(JSContext* ctx, int memhard, uint32_t cap)
{
    if (dyn_kdf_cur && dyn_kdf_cur->st)
        dyn_kdf_cur->st->started = 1;
    if (!memhard)
        return 0;
    while (cap && (uint32_t)dyn_kdf_inflight >= cap) {
        JSValue rf[2], p, r;
        DynKdfWait w, **pp;
        p = JS_NewPromiseCapability(ctx, rf);
        if (JS_IsException(p))
            return -1;
        JS_FreeValue(ctx, rf[1]);
        w.resolve = rf[0];
        w.woken = 0;
        w.next = NULL;
        for (pp = &dyn_kdf_waiters; *pp; pp = &(*pp)->next)
            ;
        *pp = &w;
        r = dyn_fiber_await(ctx, p);
        if (!w.woken) {
            for (pp = &dyn_kdf_waiters; *pp; pp = &(*pp)->next)
                if (*pp == &w) {
                    *pp = w.next;
                    break;
                }
            JS_FreeValue(ctx, w.resolve);
        }
        if (JS_IsException(r))
            return -1;
        JS_FreeValue(ctx, r);
    }
    if (++dyn_kdf_inflight > dyn_kdf_inflight_peak)
        dyn_kdf_inflight_peak = dyn_kdf_inflight;
    return 0;
}

void dyn_kdf_end(JSContext* ctx, int memhard)
{
    DynKdfWait* w;
    if (!memhard)
        return;
    dyn_kdf_inflight--;
    w = dyn_kdf_waiters;
    dyn_kdf_waiters = NULL;
    while (w) {
        DynKdfWait* nx = w->next;
        JSValue r = JS_Call(ctx, w->resolve, JS_UNDEFINED, 0, NULL);
        JS_FreeValue(ctx, r);
        JS_FreeValue(ctx, w->resolve);
        w->resolve = JS_UNDEFINED;
        w->woken = 1;
        w = nx;
    }
}

static JSValue dyn_kdf_stats(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    JSValue o = JS_NewObject(ctx);
    (void)this_val;
    if (JS_IsException(o))
        return o;
    JS_SetPropertyStr(ctx, o, "inFlight", JS_NewInt32(ctx, dyn_kdf_inflight));
    JS_SetPropertyStr(ctx, o, "peak", JS_NewInt32(ctx, dyn_kdf_inflight_peak));
    if (argc > 0 && JS_ToBool(ctx, argv[0]))
        dyn_kdf_inflight_peak = dyn_kdf_inflight;
    return o;
}

static JSValue dyn_kdf_sync(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    switch (magic) {
    case 0:
        return dyn_bcrypt_hash_js(ctx, this_val, argc, argv);
    case 1:
        return dyn_bcrypt_verify_js(ctx, this_val, argc, argv);
    case 2:
        return dyn_argon2_hash_js(ctx, this_val, argc, argv);
    case 3:
        return dyn_argon2_verify_js(ctx, this_val, argc, argv);
#ifdef CONFIG_OPENSSL
    case 5:
        return dyn_scrypt_js(ctx, this_val, argc, argv);
#endif
    default:
        return dyn_crypto_kdf(ctx, this_val, argc, argv, 1);
    }
}

static JSValue dyn_kdf_entry(JSContext* ctx, void* arg)
{
    DynKdfJob* j = arg;
    JSValue fn, r;
    int i;
    dyn_kdf_cur = j;
    JS_SwapStackFrame(JS_GetRuntime(ctx), NULL);
    fn = JS_NewCFunctionMagic(ctx, dyn_kdf_sync, "kdf", 4, JS_CFUNC_generic_magic, j->which);
    r = JS_IsException(fn) ? fn : JS_Call(ctx, fn, JS_UNDEFINED, j->argc, (JSValueConst*)j->argv);
    JS_FreeValue(ctx, fn);
    if (JS_IsException(r) && j->st->sync && !j->st->started) {
        j->st->err = JS_GetException(ctx);
        j->st->has_err = 1;
        r = JS_UNDEFINED;
    }
    dyn_kdf_state_put(ctx, j->st);
    for (i = 0; i < j->argc; i++)
        JS_FreeValue(ctx, j->argv[i]);
    free(j);
    return r;
}

static JSValue dyn_kdf_async(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    DynKdfJob* j;
    DynKdfJob* prev;
    DynKdfState* st;
    JSValue p;
    int i;
    (void)this_val;
    j = malloc(sizeof(*j) + (size_t)(argc > 0 ? argc : 1) * sizeof(JSValue));
    if (!j)
        return JS_ThrowOutOfMemory(ctx);
    st = calloc(1, sizeof(*st));
    if (!st) {
        free(j);
        return JS_ThrowOutOfMemory(ctx);
    }
    st->refs = 2;
    st->sync = 1;
    j->which = magic;
    j->argc = argc;
    j->st = st;
    for (i = 0; i < argc; i++)
        j->argv[i] = JS_DupValue(ctx, argv[i]);
    prev = dyn_kdf_cur;
    p = dyn_fiber_start(ctx, dyn_kdf_entry, j);
    dyn_kdf_cur = prev;
    st->sync = 0;
    if (st->has_err) {
        JS_FreeValue(ctx, p);
        p = JS_Throw(ctx, st->err);
        st->has_err = 0;
    }
    dyn_kdf_state_put(ctx, st);
    return p;
}

static const JSCFunctionListEntry dyn_bcrypt_funcs[] = {
    JS_CFUNC_MAGIC_DEF("hash", 1, dyn_kdf_async, 0),
    JS_CFUNC_MAGIC_DEF("verify", 2, dyn_kdf_async, 1),
};

static const JSCFunctionListEntry dyn_argon2_funcs[] = {
    JS_CFUNC_MAGIC_DEF("hash", 2, dyn_kdf_async, 2),
    JS_CFUNC_MAGIC_DEF("verify", 3, dyn_kdf_async, 3),
};

#ifdef CONFIG_OPENSSL
static const JSCFunctionListEntry dyn_oaep_funcs[] = {
    JS_CFUNC_DEF("seal", 2, dyn_rsa_oaep_seal),
    JS_CFUNC_DEF("open", 2, dyn_rsa_oaep_open),
};

static const JSCFunctionListEntry dyn_rsa_funcs[] = {
    JS_CFUNC_DEF("generate", 0, dyn_rsa_generate),
    JS_CFUNC_DEF("sign", 3, dyn_rsa_sign),
    JS_CFUNC_DEF("verify", 4, dyn_rsa_verify),
    JS_CFUNC_DEF("signPSS", 3, dyn_rsa_sign_pss),
    JS_CFUNC_DEF("verifyPSS", 4, dyn_rsa_verify_pss),
    JS_OBJECT_DEF("OAEP", dyn_oaep_funcs, countof(dyn_oaep_funcs), JS_PROP_CONFIGURABLE),
};

static const JSCFunctionListEntry dyn_ecdsa_funcs[] = {
    JS_CFUNC_DEF("generate", 0, dyn_ecdsa_generate),
    JS_CFUNC_DEF("sign", 3, dyn_ecdsa_sign),
    JS_CFUNC_DEF("verify", 4, dyn_ecdsa_verify),
};

static const JSCFunctionListEntry dyn_ecdh_funcs[] = {
    JS_CFUNC_DEF("generate", 0, dyn_ecdh_generate),
    JS_CFUNC_DEF("derive", 2, dyn_ecdh_derive),
};

static const JSCFunctionListEntry dyn_x509_funcs[] = {
    JS_CFUNC_DEF("parse", 1, dyn_x509_parse),
    JS_CFUNC_DEF("generateSelfSigned", 1, dyn_x509_generate),
};
#endif

static const JSCFunctionListEntry dyn_crypto_funcs[] = {
    JS_CFUNC_MAGIC_DEF("HMAC", 3, dyn_crypto_hmac, 0),
    JS_CFUNC_MAGIC_DEF("HMACHex", 3, dyn_crypto_hmac, 1),
    JS_CFUNC_DEF("TimingSafeEqual", 2, dyn_crypto_ct_equal),
    JS_CFUNC_MAGIC_DEF("HKDF", 1, dyn_crypto_kdf, 0),
    JS_CFUNC_MAGIC_DEF("PBKDF2", 1, dyn_kdf_async, 4),
    JS_CFUNC_DEF("RandomBytes", 0, dyn_crypto_random_bytes),
#ifdef CONFIG_OPENSSL
    JS_CFUNC_DEF("Ed25519Generate", 0, dyn_ed25519_generate),
    JS_CFUNC_DEF("Ed25519Sign", 2, dyn_ed25519_sign),
    JS_CFUNC_DEF("Ed25519Verify", 3, dyn_ed25519_verify),
    JS_CFUNC_DEF("Ed25519PemToRaw", 1, dyn_ed25519_pem_to_raw),
    JS_CFUNC_DEF("Ed25519PemFromRaw", 1, dyn_ed25519_pem_from_raw),
    JS_CFUNC_DEF("X25519Generate", 0, dyn_x25519_generate),
    JS_CFUNC_DEF("X25519Derive", 2, dyn_x25519_derive),
    JS_CFUNC_DEF("X25519PemToRaw", 1, dyn_x25519_pem_to_raw),
    JS_CFUNC_DEF("X25519PemFromRaw", 1, dyn_x25519_pem_from_raw),
    JS_CFUNC_MAGIC_DEF("Scrypt", 2, dyn_kdf_async, 5),
    JS_OBJECT_DEF("RSA", dyn_rsa_funcs, countof(dyn_rsa_funcs), JS_PROP_CONFIGURABLE),
    JS_OBJECT_DEF("ECDSA", dyn_ecdsa_funcs, countof(dyn_ecdsa_funcs), JS_PROP_CONFIGURABLE),
    JS_OBJECT_DEF("ECDH", dyn_ecdh_funcs, countof(dyn_ecdh_funcs), JS_PROP_CONFIGURABLE),
    JS_OBJECT_DEF("X509", dyn_x509_funcs, countof(dyn_x509_funcs), JS_PROP_CONFIGURABLE),
#endif
    JS_OBJECT_DEF("Bcrypt", dyn_bcrypt_funcs, countof(dyn_bcrypt_funcs), JS_PROP_CONFIGURABLE),
    JS_CFUNC_DEF("JWTSign", 2, dyn_crypto_jwt_sign),
    JS_CFUNC_DEF("JWTVerify", 3, dyn_crypto_jwt_verify),
};

static JSValue dyn_argon2_object(JSContext* ctx)
{
    JSValue o = JS_NewObject(ctx), g, sym, forfn, key, fn;
    JSAtom a;
    if (JS_IsException(o))
        return o;
    if (JS_SetPropertyFunctionList(ctx, o, dyn_argon2_funcs, countof(dyn_argon2_funcs)) < 0)
        goto fail;
    g = JS_GetGlobalObject(ctx);
    sym = JS_GetPropertyStr(ctx, g, "Symbol");
    JS_FreeValue(ctx, g);
    forfn = JS_GetPropertyStr(ctx, sym, "for");
    key = JS_NewString(ctx, "dyna.crypto.kdfStats");
    g = JS_Call(ctx, forfn, sym, 1, (JSValueConst*)&key);
    JS_FreeValue(ctx, key);
    JS_FreeValue(ctx, forfn);
    JS_FreeValue(ctx, sym);
    if (JS_IsException(g))
        goto fail;
    a = JS_ValueToAtom(ctx, g);
    JS_FreeValue(ctx, g);
    fn = JS_NewCFunction(ctx, dyn_kdf_stats, "kdfStats", 0);
    if (a == JS_ATOM_NULL || JS_DefinePropertyValue(ctx, o, a, fn, JS_PROP_CONFIGURABLE) < 0) {
        JS_FreeAtom(ctx, a);
        goto fail;
    }
    JS_FreeAtom(ctx, a);
    return o;
fail:
    JS_FreeValue(ctx, o);
    return JS_EXCEPTION;
}

static int dyn_crypto_init_module(JSContext* ctx, JSModuleDef* m)
{
    JSValue argon2;
    if (dyn_register_class(ctx, m, &dyn_hmac_class_id, &dyn_hmac_class,
            dyn_hmac_proto, countof(dyn_hmac_proto),
            dyn_hmac_ctor, "Hmac")
        < 0)
        return -1;
#ifdef CONFIG_OPENSSL
    if (dyn_register_class(ctx, m, &dyn_aesgcm_class_id, &dyn_aesgcm_class,
            dyn_aead_proto, countof(dyn_aead_proto),
            dyn_aesgcm_ctor, "AESGCM")
        < 0)
        return -1;
    if (dyn_register_class(ctx, m, &dyn_chapoly_class_id, &dyn_chapoly_class,
            dyn_aead_proto, countof(dyn_aead_proto),
            dyn_chapoly_ctor, "ChaCha20Poly1305")
        < 0)
        return -1;
#endif
    argon2 = dyn_argon2_object(ctx);
    if (JS_IsException(argon2) || JS_SetModuleExport(ctx, m, "Argon2id", argon2) < 0)
        return -1;
    if (JS_SetModuleExportList(ctx, m, dyn_crypto_funcs,
            countof(dyn_crypto_funcs))
        < 0)
        return -1;
    return JS_SetModuleExportList(ctx, m, dyn_hash_funcs,
        countof(dyn_hash_funcs));
}

int js_nat_init_crypto(JSContext* ctx)
{
    JSModuleDef* m = JS_NewCModule(ctx, "dyna:crypto", dyn_crypto_init_module);
    if (!m)
        return -1;
    JS_AddModuleExport(ctx, m, "Hmac");
    JS_AddModuleExport(ctx, m, "Argon2id");
#ifdef CONFIG_OPENSSL
    JS_AddModuleExport(ctx, m, "AESGCM");
    JS_AddModuleExport(ctx, m, "ChaCha20Poly1305");
#endif
    if (JS_AddModuleExportList(ctx, m, dyn_crypto_funcs,
            countof(dyn_crypto_funcs))
        < 0)
        return -1;
    return JS_AddModuleExportList(ctx, m, dyn_hash_funcs,
        countof(dyn_hash_funcs));
}

#endif
