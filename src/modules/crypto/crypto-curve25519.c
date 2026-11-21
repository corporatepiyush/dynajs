#include "crypto-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_CRYPTO)

static __maybe_unused void dyn_secure_zero(void* p, size_t n)
{
    volatile unsigned char* v = (volatile unsigned char*)p;
    while (n--)
        *v++ = 0;
}

#ifdef CONFIG_OPENSSL

#define CURVE_SEED 32
#define ED_SIG 64

static EVP_PKEY* curve_key(JSContext* ctx, int type, int is_priv,
    const uint8_t* raw, size_t len, const char* what)
{
    EVP_PKEY* k = is_priv
        ? EVP_PKEY_new_raw_private_key(type, NULL, raw, len)
        : EVP_PKEY_new_raw_public_key(type, NULL, raw, len);
    if (!k) {
        ERR_clear_error();
        JS_ThrowTypeError(ctx, "%s: not a valid %d-byte key", what,
            (int)CURVE_SEED);
    }
    return k;
}

static JSValue curve_generate(JSContext* ctx, int type, const char* what)
{
    EVP_PKEY_CTX* pc = EVP_PKEY_CTX_new_id(type, NULL);
    EVP_PKEY* k = NULL;
    uint8_t priv[CURVE_SEED], pub[CURVE_SEED];
    size_t plen = sizeof priv, qlen = sizeof pub;
    JSValue o = JS_EXCEPTION;

    if (!pc) {
        ERR_clear_error();
        return JS_ThrowInternalError(ctx, "%s: no backend", what);
    }
    if (EVP_PKEY_keygen_init(pc) <= 0 || EVP_PKEY_keygen(pc, &k) <= 0) {
        ERR_clear_error();
        JS_ThrowInternalError(ctx, "%s: key generation failed", what);
        goto done;
    }
    if (EVP_PKEY_get_raw_private_key(k, priv, &plen) <= 0 || EVP_PKEY_get_raw_public_key(k, pub, &qlen) <= 0) {
        ERR_clear_error();
        JS_ThrowInternalError(ctx, "%s: key export failed", what);
        goto done;
    }
    o = JS_NewObject(ctx);
    if (JS_IsException(o))
        goto done;
    JS_DefinePropertyValueStr(ctx, o, "privateKey",
        dyn_nat_u8_copy(ctx, priv, plen), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, o, "publicKey",
        dyn_nat_u8_copy(ctx, pub, qlen), JS_PROP_C_W_E);
done:
    dyn_secure_zero(priv, sizeof priv);
    if (k)
        EVP_PKEY_free(k);
    EVP_PKEY_CTX_free(pc);
    return o;
}

static JSValue curve_pem_to_raw(JSContext* ctx, int type, const char* algname,
    JSValueConst pemv)
{
    const char* pem;
    size_t pemlen;
    EVP_PKEY* k = NULL;
    BIO* b = NULL;
    uint8_t priv[CURVE_SEED], pub[CURVE_SEED];
    size_t plen = sizeof priv, qlen = sizeof pub;
    int is_priv;
    JSValue o = JS_EXCEPTION;

    pem = JS_ToCStringLen(ctx, &pemlen, pemv);
    if (!pem)
        return JS_EXCEPTION;
    b = BIO_new_mem_buf(pem, (int)pemlen);
    if (!b) {
        JS_FreeCString(ctx, pem);
        return JS_ThrowOutOfMemory(ctx);
    }
    k = PEM_read_bio_PrivateKey(b, NULL, dyn_pem_no_passphrase, NULL);
    if (k) {
        ERR_clear_error();
    } else {
        ERR_clear_error();
        BIO_reset(b);
        k = PEM_read_bio_PUBKEY(b, NULL, dyn_pem_no_passphrase, NULL);
        if (!k)
            ERR_clear_error();
    }
    BIO_free(b);
    JS_FreeCString(ctx, pem);
    if (!k)
        return JS_ThrowTypeError(ctx, "%s: not a parseable PEM private/public key", algname);
    if (!EVP_PKEY_is_a(k, type == EVP_PKEY_ED25519 ? "ED25519" : "X25519")) {
        EVP_PKEY_free(k);
        return JS_ThrowTypeError(ctx, "%s: PEM key is the wrong algorithm", algname);
    }
    memset(priv, 0, sizeof priv);
    memset(pub, 0, sizeof pub);
    is_priv = EVP_PKEY_get_raw_private_key(k, priv, &plen) > 0;
    if (EVP_PKEY_get_raw_public_key(k, pub, &qlen) <= 0) {
        ERR_clear_error();
        EVP_PKEY_free(k);
        dyn_secure_zero(priv, sizeof priv);
        return JS_ThrowInternalError(ctx, "%s: raw export failed", algname);
    }
    if (qlen != CURVE_SEED || (is_priv && plen != CURVE_SEED)) {
        EVP_PKEY_free(k);
        dyn_secure_zero(priv, sizeof priv);
        return JS_ThrowTypeError(ctx, "%s: not a valid %d-byte key",
            algname, (int)CURVE_SEED);
    }
    EVP_PKEY_free(k);
    o = JS_NewObject(ctx);
    if (JS_IsException(o)) {
        dyn_secure_zero(priv, sizeof priv);
        return o;
    }
    if (is_priv)
        JS_DefinePropertyValueStr(ctx, o, "privateKey",
            dyn_nat_u8_copy(ctx, priv, plen),
            JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, o, "publicKey",
        dyn_nat_u8_copy(ctx, pub, qlen),
        JS_PROP_C_W_E);
    dyn_secure_zero(priv, sizeof priv);
    return o;
}

static JSValue curve_raw_to_pem(JSContext* ctx, int type, const char* algname,
    JSValueConst rawv, int want_private)
{
    const uint8_t* raw;
    size_t rawlen;
    const char* owned = NULL;
    EVP_PKEY* k;
    BIO* b;
    char* ptr = NULL;
    long plen;
    JSValue ret;

    if (dyn_crypto_data(ctx, rawv, &raw, &rawlen, &owned))
        return JS_EXCEPTION;
    k = want_private
        ? EVP_PKEY_new_raw_private_key(type, NULL, raw, rawlen)
        : EVP_PKEY_new_raw_public_key(type, NULL, raw, rawlen);
    if (owned)
        JS_FreeCString(ctx, owned);
    if (!k) {
        ERR_clear_error();
        return JS_ThrowTypeError(ctx, "%s: not a valid %d-byte key",
            algname, (int)CURVE_SEED);
    }
    b = BIO_new(BIO_s_mem());
    if (!b) {
        EVP_PKEY_free(k);
        return JS_ThrowOutOfMemory(ctx);
    }
    if ((want_private && !PEM_write_bio_PrivateKey(b, k, NULL, NULL, 0, NULL, NULL)) || (!want_private && !PEM_write_bio_PUBKEY(b, k))) {
        ERR_clear_error();
        plen = BIO_get_mem_data(b, &ptr);
        if (ptr && plen > 0)
            dyn_wipe(ptr, (size_t)plen);
        BIO_free(b);
        EVP_PKEY_free(k);
        return JS_ThrowInternalError(ctx, "%s: PEM encoding failed", algname);
    }
    EVP_PKEY_free(k);
    plen = BIO_get_mem_data(b, &ptr);
    if (plen <= 0 || !ptr) {
        BIO_free(b);
        ERR_clear_error();
        return JS_ThrowInternalError(ctx, "%s: PEM encode failed", algname);
    }
    ret = JS_NewStringLen(ctx, ptr, (size_t)plen);
    dyn_wipe(ptr, (size_t)plen);
    BIO_free(b);
    return ret;
}

static const char* const curve_raw_keys[] = { "privateKey", "publicKey" };

static JSValue curve_raw_from(JSContext* ctx, int type, const char* algname,
    JSValueConst optsv)
{
    JSValue privv, pubv, o, pv = JS_UNDEFINED, qv = JS_UNDEFINED;

    if (!JS_IsObject(optsv))
        return JS_ThrowTypeError(ctx,
            "%s: expected { privateKey?, publicKey } (raw 32-byte keys)", algname);
    if (dyn_opts_strict(ctx, optsv, curve_raw_keys, countof(curve_raw_keys)) < 0)
        return JS_EXCEPTION;
    o = JS_NewObject(ctx);
    if (JS_IsException(o))
        return o;
    privv = JS_GetPropertyStr(ctx, optsv, "privateKey");
    if (JS_IsException(privv)) {
        JS_FreeValue(ctx, o);
        return privv;
    }
    if (!JS_IsUndefined(privv) && !JS_IsNull(privv)) {
        pv = curve_raw_to_pem(ctx, type, algname, privv, 1);
        if (JS_IsException(pv)) {
            JS_FreeValue(ctx, privv);
            JS_FreeValue(ctx, o);
            return JS_EXCEPTION;
        }
        JS_DefinePropertyValueStr(ctx, o, "privateKey", pv, JS_PROP_C_W_E);
    }
    JS_FreeValue(ctx, privv);
    pubv = JS_GetPropertyStr(ctx, optsv, "publicKey");
    if (JS_IsException(pubv)) {
        JS_FreeValue(ctx, o);
        return pubv;
    }
    if (!JS_IsUndefined(pubv) && !JS_IsNull(pubv)) {
        qv = curve_raw_to_pem(ctx, type, algname, pubv, 0);
        if (JS_IsException(qv)) {
            JS_FreeValue(ctx, pubv);
            JS_FreeValue(ctx, o);
            return JS_EXCEPTION;
        }
        JS_DefinePropertyValueStr(ctx, o, "publicKey", qv, JS_PROP_C_W_E);
    }
    JS_FreeValue(ctx, pubv);
    if (JS_IsUndefined(pv) && JS_IsUndefined(qv)) {
        JS_FreeValue(ctx, o);
        return JS_ThrowTypeError(ctx,
            "%s: expected { privateKey?, publicKey } (raw 32-byte keys)", algname);
    }
    return o;
}

JSValue dyn_ed25519_generate(JSContext* ctx, JSValueConst t, int argc,
    JSValueConst* argv)
{
    (void)t;
    (void)argc;
    (void)argv;
    return curve_generate(ctx, EVP_PKEY_ED25519, "Ed25519Generate");
}

JSValue dyn_ed25519_pem_to_raw(JSContext* ctx, JSValueConst t,
    int argc, JSValueConst* argv)
{
    (void)t;
    (void)argc;
    return curve_pem_to_raw(ctx, EVP_PKEY_ED25519, "Ed25519PemToRaw",
        argc > 0 ? argv[0] : JS_UNDEFINED);
}

JSValue dyn_ed25519_pem_from_raw(JSContext* ctx, JSValueConst t,
    int argc, JSValueConst* argv)
{
    (void)t;
    (void)argc;
    return curve_raw_from(ctx, EVP_PKEY_ED25519, "Ed25519PemFromRaw",
        argc > 0 ? argv[0] : JS_UNDEFINED);
}

JSValue dyn_x25519_pem_to_raw(JSContext* ctx, JSValueConst t,
    int argc, JSValueConst* argv)
{
    (void)t;
    (void)argc;
    return curve_pem_to_raw(ctx, EVP_PKEY_X25519, "X25519PemToRaw",
        argc > 0 ? argv[0] : JS_UNDEFINED);
}

JSValue dyn_x25519_pem_from_raw(JSContext* ctx, JSValueConst t,
    int argc, JSValueConst* argv)
{
    (void)t;
    (void)argc;
    return curve_raw_from(ctx, EVP_PKEY_X25519, "X25519PemFromRaw",
        argc > 0 ? argv[0] : JS_UNDEFINED);
}

JSValue dyn_x25519_generate(JSContext* ctx, JSValueConst t, int argc,
    JSValueConst* argv)
{
    (void)t;
    (void)argc;
    (void)argv;
    return curve_generate(ctx, EVP_PKEY_X25519, "X25519Generate");
}

JSValue dyn_ed25519_sign(JSContext* ctx, JSValueConst t, int argc,
    JSValueConst* argv)
{
    const uint8_t *key = NULL, *msg = NULL;
    size_t keylen = 0, msglen = 0, siglen = ED_SIG;
    const char *o1 = NULL, *o2 = NULL;
    EVP_PKEY* k = NULL;
    EVP_MD_CTX* md = NULL;
    uint8_t sig[ED_SIG];
    JSValue ret = JS_EXCEPTION;
    (void)t;

    if (argc < 2)
        return JS_ThrowTypeError(ctx, "Ed25519Sign(privateKey, message)");
    if (dyn_crypto_secret(ctx, argv[0], &key, &keylen, &o1))
        return JS_EXCEPTION;
    if (dyn_crypto_data(ctx, argv[1], &msg, &msglen, &o2))
        goto done;
    if (keylen != CURVE_SEED) {
        JS_ThrowTypeError(ctx, "Ed25519Sign: private key must be 32 bytes");
        goto done;
    }
    k = curve_key(ctx, EVP_PKEY_ED25519, 1, key, keylen, "Ed25519Sign");
    if (!k)
        goto done;
    md = EVP_MD_CTX_new();
    if (!md || EVP_DigestSignInit(md, NULL, NULL, NULL, k) <= 0 || EVP_DigestSign(md, sig, &siglen, msg, msglen) <= 0) {
        ERR_clear_error();
        JS_ThrowInternalError(ctx, "Ed25519Sign: signing failed");
        goto done;
    }
    ret = dyn_nat_u8_copy(ctx, sig, siglen);
done:
    if (md)
        EVP_MD_CTX_free(md);
    if (k)
        EVP_PKEY_free(k);
    if (o1)
        JS_FreeCString(ctx, o1);
    if (o2)
        JS_FreeCString(ctx, o2);
    return ret;
}

JSValue dyn_ed25519_verify(JSContext* ctx, JSValueConst t, int argc,
    JSValueConst* argv)
{
    const uint8_t *key = NULL, *msg = NULL, *sig = NULL;
    size_t keylen = 0, msglen = 0, siglen = 0;
    const char *o1 = NULL, *o2 = NULL, *o3 = NULL;
    EVP_PKEY* k = NULL;
    EVP_MD_CTX* md = NULL;
    JSValue ret = JS_EXCEPTION;
    int good = 0;
    (void)t;

    if (argc < 3)
        return JS_ThrowTypeError(ctx, "Ed25519Verify(publicKey, message, signature)");
    if (dyn_crypto_secret(ctx, argv[0], &key, &keylen, &o1))
        return JS_EXCEPTION;
    if (dyn_crypto_data(ctx, argv[1], &msg, &msglen, &o2))
        goto done;
    if (dyn_crypto_data(ctx, argv[2], &sig, &siglen, &o3))
        goto done;
    if (keylen != CURVE_SEED) {
        JS_ThrowTypeError(ctx, "Ed25519Verify: public key must be 32 bytes");
        goto done;
    }
    if (siglen != ED_SIG) {
        ret = JS_NewBool(ctx, 0);
        goto done;
    }
    k = curve_key(ctx, EVP_PKEY_ED25519, 0, key, keylen, "Ed25519Verify");
    if (!k)
        goto done;
    md = EVP_MD_CTX_new();
    if (md && EVP_DigestVerifyInit(md, NULL, NULL, NULL, k) > 0)
        good = EVP_DigestVerify(md, sig, siglen, msg, msglen) == 1;
    ERR_clear_error();
    ret = JS_NewBool(ctx, good);
done:
    if (md)
        EVP_MD_CTX_free(md);
    if (k)
        EVP_PKEY_free(k);
    if (o1)
        JS_FreeCString(ctx, o1);
    if (o2)
        JS_FreeCString(ctx, o2);
    if (o3)
        JS_FreeCString(ctx, o3);
    return ret;
}

JSValue dyn_x25519_derive(JSContext* ctx, JSValueConst t, int argc,
    JSValueConst* argv)
{
    const uint8_t *priv = NULL, *peer = NULL;
    size_t privlen = 0, peerlen = 0, outlen = CURVE_SEED;
    const char *o1 = NULL, *o2 = NULL;
    EVP_PKEY *kp = NULL, *kq = NULL;
    EVP_PKEY_CTX* dc = NULL;
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
    if (!kp)
        goto done;
    kq = curve_key(ctx, EVP_PKEY_X25519, 0, peer, peerlen, "X25519Derive");
    if (!kq)
        goto done;
    dc = EVP_PKEY_CTX_new(kp, NULL);
    if (!dc || EVP_PKEY_derive_init(dc) <= 0 || EVP_PKEY_derive_set_peer(dc, kq) <= 0 || EVP_PKEY_derive(dc, out, &outlen) <= 0) {
        ERR_clear_error();
        JS_ThrowTypeError(ctx, "X25519Derive: derivation failed "
                               "(a small-order peer key is refused)");
        goto done;
    }
    ret = dyn_nat_u8_copy(ctx, out, outlen);
done:
    dyn_secure_zero(out, sizeof out);
    if (dc)
        EVP_PKEY_CTX_free(dc);
    if (kq)
        EVP_PKEY_free(kq);
    if (kp)
        EVP_PKEY_free(kp);
    if (o1)
        JS_FreeCString(ctx, o1);
    if (o2)
        JS_FreeCString(ctx, o2);
    return ret;
}

#endif

#ifdef CONFIG_OPENSSL
int dyn_curve_nid(const char* curve, int* coord)
{
    if (!curve)
        return -1;
    if (!strcmp(curve, "P-256") || !strcmp(curve, "prime256v1") || !strcmp(curve, "secp256r1")) {
        *coord = 32;
        return NID_X9_62_prime256v1;
    }
    if (!strcmp(curve, "P-384") || !strcmp(curve, "secp384r1")) {
        *coord = 48;
        return NID_secp384r1;
    }
    if (!strcmp(curve, "P-521") || !strcmp(curve, "secp521r1")) {
        *coord = 66;
        return NID_secp521r1;
    }
    return -1;
}
#endif

#endif
