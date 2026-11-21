#include "crypto-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_CRYPTO)

#ifdef CONFIG_OPENSSL
static int dyn_ec_coord(EVP_PKEY* k)
{
    int bits;
    if (!EVP_PKEY_is_a(k, "EC"))
        return 0;
    bits = EVP_PKEY_get_bits(k);
    if (bits == 256)
        return 32;
    if (bits == 384)
        return 48;
    if (bits == 521)
        return 66;
    return -1;
}

JSValue dyn_ecdsa_generate(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    const char* curve_str = "P-256";
    const char* c_ow = NULL;
    int coord = 0, nid;
    EVP_PKEY_CTX* kctx = NULL;
    EVP_PKEY* pkey = NULL;
    JSValue ret = JS_EXCEPTION;
    (void)this_val;

    if (argc > 0 && !JS_IsUndefined(argv[0])) {
        c_ow = JS_ToCString(ctx, argv[0]);
        if (!c_ow)
            return JS_EXCEPTION;
        curve_str = c_ow;
    }
    nid = dyn_curve_nid(curve_str, &coord);
    if (nid < 0) {
        if (c_ow)
            JS_FreeCString(ctx, c_ow);
        return JS_ThrowRangeError(ctx, "ECDSA.generate: curve must be P-256, P-384 or P-521");
    }
    if (c_ow)
        JS_FreeCString(ctx, c_ow);

    if (RAND_status() != 1)
        return JS_ThrowInternalError(ctx, "ECDSA.generate: OS entropy unavailable");

    kctx = EVP_PKEY_CTX_new_id(EVP_PKEY_EC, NULL);
    if (!kctx || EVP_PKEY_keygen_init(kctx) <= 0 || EVP_PKEY_CTX_set_ec_paramgen_curve_nid(kctx, nid) <= 0 || EVP_PKEY_keygen(kctx, &pkey) <= 0) {
        JS_ThrowInternalError(ctx, "ECDSA.generate: key generation failed");
        goto done;
    }
    ret = dyn_pk_export_pem(ctx, pkey);

done:
    if (pkey)
        EVP_PKEY_free(pkey);
    if (kctx)
        EVP_PKEY_CTX_free(kctx);
    return ret;
}

JSValue dyn_ecdsa_sign(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    const char* md_str = NULL;
    const EVP_MD* md;
    uint8_t* pem_bytes = NULL;
    size_t pem_len = 0;
    const uint8_t* msg;
    size_t msglen;
    const char* msg_ow = NULL;
    EVP_PKEY* k = NULL;
    EVP_MD_CTX* mctx = NULL;
    int coord, is_der = 0;
    size_t siglen = 0;
    uint8_t* sig = NULL;
    JSValue ret = JS_EXCEPTION;
    (void)this_val;

    if (argc < 3)
        return JS_ThrowTypeError(ctx, "ECDSA.sign(md, privateKey, msg[, opts])");
    md_str = JS_ToCString(ctx, argv[0]);
    if (!md_str)
        return JS_EXCEPTION;
    md = dyn_standalone_md(md_str);
    JS_FreeCString(ctx, md_str);
    if (!md)
        return JS_ThrowTypeError(ctx, "ECDSA.sign: unsupported hash algorithm");

    if (argc > 3 && JS_IsObject(argv[3])) {
        JSValue fv = JS_GetPropertyStr(ctx, argv[3], "format");
        if (JS_IsString(fv)) {
            const char* fs = JS_ToCString(ctx, fv);
            if (fs && !strcmp(fs, "der"))
                is_der = 1;
            if (fs)
                JS_FreeCString(ctx, fs);
        }
        JS_FreeValue(ctx, fv);
    }

    if (dyn_crypto_copy(ctx, argv[1], &pem_bytes, &pem_len))
        return JS_EXCEPTION;
    k = jwt_pem_key((const char*)pem_bytes, pem_len, 1);
    if (pem_bytes) {
        dyn_wipe(pem_bytes, pem_len);
        free(pem_bytes);
        pem_bytes = NULL;
    }
    if (!k)
        return JS_ThrowTypeError(ctx, "ECDSA.sign: invalid private key PEM");
    coord = dyn_ec_coord(k);
    if (coord <= 0) {
        EVP_PKEY_free(k);
        return JS_ThrowTypeError(ctx, "ECDSA.sign: key must be EC P-256, P-384 or P-521");
    }

    if (dyn_crypto_data(ctx, argv[2], &msg, &msglen, &msg_ow)) {
        EVP_PKEY_free(k);
        return JS_EXCEPTION;
    }

    mctx = EVP_MD_CTX_new();
    if (!mctx || EVP_DigestSignInit(mctx, NULL, md, NULL, k) <= 0 || EVP_DigestSign(mctx, NULL, &siglen, msg, msglen) <= 0) {
        JS_ThrowInternalError(ctx, "ECDSA.sign: initialization failed");
        goto done;
    }
    sig = (uint8_t*)malloc(siglen ? siglen : 1);
    if (!sig) {
        JS_ThrowOutOfMemory(ctx);
        goto done;
    }
    if (EVP_DigestSign(mctx, sig, &siglen, msg, msglen) <= 0) {
        JS_ThrowInternalError(ctx, "ECDSA.sign: signing failed");
        goto done;
    }

    if (is_der) {
        ret = dyn_nat_u8_copy(ctx, sig, siglen);
    } else {
        uint8_t raw[132];
        if (coord > 66) {
            JS_ThrowInternalError(ctx, "ECDSA.sign: unsupported curve size");
            goto done;
        }
        if (jwt_der_to_raw(sig, siglen, coord, raw) < 0) {
            JS_ThrowInternalError(ctx, "ECDSA.sign: DER->raw conversion failed");
            goto done;
        }
        ret = dyn_nat_u8_copy(ctx, raw, (size_t)(2 * coord));
    }

done:
    free(sig);
    if (mctx)
        EVP_MD_CTX_free(mctx);
    if (k)
        EVP_PKEY_free(k);
    if (msg_ow)
        JS_FreeCString(ctx, msg_ow);
    return ret;
}

JSValue dyn_ecdsa_verify(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    const char* md_str = NULL;
    const EVP_MD* md;
    uint8_t* pem_bytes = NULL;
    size_t pem_len = 0;
    uint8_t* sig_bytes = NULL;
    size_t sig_len = 0;
    const uint8_t* msg;
    size_t msglen;
    const char* msg_ow = NULL;
    EVP_PKEY* k = NULL;
    EVP_MD_CTX* mctx = NULL;
    int coord, is_der = 0, ok = 0;
    uint8_t* der = NULL;
    int der_len = 0;
    const uint8_t* verify_sig;
    size_t verify_siglen;
    (void)this_val;

    if (argc < 4)
        return JS_ThrowTypeError(ctx, "ECDSA.verify(md, publicKey, msg, sig[, opts])");
    md_str = JS_ToCString(ctx, argv[0]);
    if (!md_str)
        return JS_EXCEPTION;
    md = dyn_standalone_md(md_str);
    JS_FreeCString(ctx, md_str);
    if (!md)
        return JS_ThrowTypeError(ctx, "ECDSA.verify: unsupported hash algorithm");

    if (argc > 4 && JS_IsObject(argv[4])) {
        JSValue fv = JS_GetPropertyStr(ctx, argv[4], "format");
        if (JS_IsString(fv)) {
            const char* fs = JS_ToCString(ctx, fv);
            if (fs && !strcmp(fs, "der"))
                is_der = 1;
            if (fs)
                JS_FreeCString(ctx, fs);
        }
        JS_FreeValue(ctx, fv);
    }

    if (dyn_crypto_copy(ctx, argv[1], &pem_bytes, &pem_len))
        return JS_EXCEPTION;
    k = jwt_pem_key((const char*)pem_bytes, pem_len, 0);
    free(pem_bytes);
    if (!k)
        return JS_ThrowTypeError(ctx, "ECDSA.verify: invalid public key PEM");
    coord = dyn_ec_coord(k);
    if (coord <= 0) {
        EVP_PKEY_free(k);
        return JS_ThrowTypeError(ctx, "ECDSA.verify: key must be EC P-256, P-384 or P-521");
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
    if (der)
        OPENSSL_free(der);
    free(sig_bytes);
    if (mctx)
        EVP_MD_CTX_free(mctx);
    if (k)
        EVP_PKEY_free(k);
    if (msg_ow)
        JS_FreeCString(ctx, msg_ow);
    return JS_NewBool(ctx, ok);
}

JSValue dyn_ecdh_generate(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    return dyn_ecdsa_generate(ctx, this_val, argc, argv);
}

JSValue dyn_ecdh_derive(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    uint8_t* priv_pem = NULL;
    size_t priv_len = 0;
    uint8_t* peer_pem = NULL;
    size_t peer_len = 0;
    EVP_PKEY *priv = NULL, *peer = NULL;
    EVP_PKEY_CTX* pctx = NULL;
    size_t secret_len = 0;
    uint8_t* secret = NULL;
    JSValue ret = JS_EXCEPTION;
    (void)this_val;

    if (argc < 2)
        return JS_ThrowTypeError(ctx, "ECDH.derive(privateKey, peerPublicKey)");

    if (dyn_crypto_copy(ctx, argv[0], &priv_pem, &priv_len))
        return JS_EXCEPTION;
    priv = jwt_pem_key((const char*)priv_pem, priv_len, 1);
    if (priv_pem) {
        dyn_wipe(priv_pem, priv_len);
        free(priv_pem);
        priv_pem = NULL;
    }
    if (!priv || !EVP_PKEY_is_a(priv, "EC")) {
        if (priv)
            EVP_PKEY_free(priv);
        return JS_ThrowTypeError(ctx, "ECDH.derive: invalid private EC key");
    }

    if (dyn_crypto_copy(ctx, argv[1], &peer_pem, &peer_len)) {
        EVP_PKEY_free(priv);
        return JS_EXCEPTION;
    }
    peer = jwt_pem_key((const char*)peer_pem, peer_len, 0);
    free(peer_pem);
    if (!peer || !EVP_PKEY_is_a(peer, "EC")) {
        EVP_PKEY_free(priv);
        if (peer)
            EVP_PKEY_free(peer);
        return JS_ThrowTypeError(ctx, "ECDH.derive: invalid peer public EC key");
    }

    pctx = EVP_PKEY_CTX_new(priv, NULL);
    if (!pctx || EVP_PKEY_derive_init(pctx) <= 0 || EVP_PKEY_derive_set_peer(pctx, peer) <= 0 || EVP_PKEY_derive(pctx, NULL, &secret_len) <= 0) {
        JS_ThrowTypeError(ctx, "ECDH.derive: derivation initialization failed");
        goto done;
    }
    secret = (uint8_t*)malloc(secret_len ? secret_len : 1);
    if (!secret) {
        JS_ThrowOutOfMemory(ctx);
        goto done;
    }
    if (EVP_PKEY_derive(pctx, secret, &secret_len) <= 0) {
        JS_ThrowTypeError(ctx, "ECDH.derive: derivation failed (small-order point refused)");
        goto done;
    }
    ret = dyn_nat_u8_copy(ctx, secret, secret_len);

done:
    if (secret) {
        dyn_wipe(secret, secret_len);
        free(secret);
    }
    if (pctx)
        EVP_PKEY_CTX_free(pctx);
    if (peer)
        EVP_PKEY_free(peer);
    if (priv)
        EVP_PKEY_free(priv);
    return ret;
}
#endif

#endif
