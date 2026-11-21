#include "crypto-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_CRYPTO)

#ifdef CONFIG_OPENSSL
#define DYN_RSA_MIN_BITS 2048

int dyn_rsa_too_small(const EVP_PKEY* k)
{
    return (EVP_PKEY_is_a(k, "RSA") || EVP_PKEY_is_a(k, "RSA-PSS"))
        && EVP_PKEY_get_bits(k) < DYN_RSA_MIN_BITS;
}

JSValue dyn_rsa_generate(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    int64_t bits = 2048;
    EVP_PKEY_CTX* kctx = NULL;
    EVP_PKEY* pkey = NULL;
    JSValue ret = JS_EXCEPTION;
    (void)this_val;

    if (argc > 0 && !JS_IsUndefined(argv[0])) {
        if (JS_ToInt64(ctx, &bits, argv[0]))
            return JS_EXCEPTION;
        if (bits != 2048 && bits != 3072 && bits != 4096)
            return JS_ThrowRangeError(ctx, "RSA.generate: bits must be 2048, 3072, or 4096");
    }
    if (RAND_status() != 1)
        return JS_ThrowInternalError(ctx, "RSA.generate: OS entropy unavailable");

    kctx = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, NULL);
    if (!kctx || EVP_PKEY_keygen_init(kctx) <= 0 || EVP_PKEY_CTX_set_rsa_keygen_bits(kctx, (int)bits) <= 0 || EVP_PKEY_keygen(kctx, &pkey) <= 0) {
        JS_ThrowInternalError(ctx, "RSA.generate: key generation failed");
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

JSValue dyn_rsa_sign(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
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
    size_t siglen = 0;
    uint8_t* sig = NULL;
    JSValue ret = JS_EXCEPTION;
    (void)this_val;

    if (argc < 3)
        return JS_ThrowTypeError(ctx, "RSA.sign(md, privateKey, msg)");
    md_str = JS_ToCString(ctx, argv[0]);
    if (!md_str)
        return JS_EXCEPTION;
    md = dyn_standalone_md(md_str);
    JS_FreeCString(ctx, md_str);
    if (!md)
        return JS_ThrowTypeError(ctx, "RSA.sign: unsupported hash algorithm");

    if (dyn_crypto_copy(ctx, argv[1], &pem_bytes, &pem_len))
        return JS_EXCEPTION;
    k = jwt_pem_key((const char*)pem_bytes, pem_len, 1);
    if (pem_bytes) {
        dyn_wipe(pem_bytes, pem_len);
        free(pem_bytes);
        pem_bytes = NULL;
    }
    if (!k)
        return JS_ThrowTypeError(ctx, "RSA.sign: invalid private key PEM");
    if (!EVP_PKEY_is_a(k, "RSA")) {
        EVP_PKEY_free(k);
        return JS_ThrowTypeError(ctx, "RSA.sign: key is not an RSA key");
    }
    if (dyn_rsa_too_small(k)) {
        int bits = EVP_PKEY_get_bits(k);
        EVP_PKEY_free(k);
        return JS_ThrowRangeError(ctx,
            "RSA.sign: key is %d bits; the minimum accepted is %d",
            bits, DYN_RSA_MIN_BITS);
    }

    if (dyn_crypto_data(ctx, argv[2], &msg, &msglen, &msg_ow)) {
        EVP_PKEY_free(k);
        return JS_EXCEPTION;
    }

    mctx = EVP_MD_CTX_new();
    if (!mctx || EVP_DigestSignInit(mctx, NULL, md, NULL, k) <= 0 || EVP_DigestSign(mctx, NULL, &siglen, msg, msglen) <= 0) {
        JS_ThrowInternalError(ctx, "RSA.sign: signing initialization failed");
        goto done;
    }
    sig = (uint8_t*)malloc(siglen ? siglen : 1);
    if (!sig) {
        JS_ThrowOutOfMemory(ctx);
        goto done;
    }
    if (EVP_DigestSign(mctx, sig, &siglen, msg, msglen) <= 0) {
        JS_ThrowInternalError(ctx, "RSA.sign: signing failed");
        goto done;
    }
    ret = dyn_nat_u8_copy(ctx, sig, siglen);

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

JSValue dyn_rsa_verify(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
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
    int ok = 0;
    (void)this_val;

    if (argc < 4)
        return JS_ThrowTypeError(ctx, "RSA.verify(md, publicKey, msg, sig)");
    md_str = JS_ToCString(ctx, argv[0]);
    if (!md_str)
        return JS_EXCEPTION;
    md = dyn_standalone_md(md_str);
    JS_FreeCString(ctx, md_str);
    if (!md)
        return JS_ThrowTypeError(ctx, "RSA.verify: unsupported hash algorithm");

    if (dyn_crypto_copy(ctx, argv[1], &pem_bytes, &pem_len))
        return JS_EXCEPTION;
    k = jwt_pem_key((const char*)pem_bytes, pem_len, 0);
    free(pem_bytes);
    if (!k)
        return JS_ThrowTypeError(ctx, "RSA.verify: invalid public key PEM");
    if (!EVP_PKEY_is_a(k, "RSA")) {
        EVP_PKEY_free(k);
        return JS_ThrowTypeError(ctx, "RSA.verify: key is not an RSA key");
    }
    if (dyn_rsa_too_small(k)) {
        int bits = EVP_PKEY_get_bits(k);
        EVP_PKEY_free(k);
        return JS_ThrowRangeError(ctx,
            "RSA.verify: key is %d bits; the minimum accepted is %d",
            bits, DYN_RSA_MIN_BITS);
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
    if (mctx)
        EVP_MD_CTX_free(mctx);
    if (k)
        EVP_PKEY_free(k);
    if (msg_ow)
        JS_FreeCString(ctx, msg_ow);
    return JS_NewBool(ctx, ok);
}

static int rsa_pss_apply_opts(EVP_PKEY_CTX* pctx, const EVP_MD* md,
    const EVP_MD* mgf_md, int saltlen)
{
    if (EVP_PKEY_CTX_set_rsa_padding(pctx, RSA_PKCS1_PSS_PADDING) <= 0)
        return -1;
    if (EVP_PKEY_CTX_set_rsa_pss_saltlen(pctx, saltlen) <= 0)
        return -1;
    if (mgf_md && EVP_PKEY_CTX_set_rsa_mgf1_md(pctx, mgf_md) <= 0)
        return -1;
    return 0;
}

static int rsa_pss_read_opts(JSContext* ctx, JSValueConst opts, int signing,
    const EVP_MD* md, const EVP_MD** pmgf, int* psaltlen)
{
    JSValue v;
    *pmgf = NULL;
    *psaltlen = signing ? RSA_PSS_SALTLEN_AUTO : RSA_PSS_SALTLEN_AUTO;

    v = JS_GetPropertyStr(ctx, opts, "saltLen");
    if (JS_IsException(v))
        return -1;
    if (!JS_IsUndefined(v) && !JS_IsNull(v)) {
        if (JS_IsString(v)) {
            const char* sv = JS_ToCString(ctx, v);
            if (!sv) {
                JS_FreeValue(ctx, v);
                return -1;
            }
            if (!strcmp(sv, "auto"))
                *psaltlen = RSA_PSS_SALTLEN_AUTO;
            else if (!strcmp(sv, "max"))
                *psaltlen = signing ? RSA_PSS_SALTLEN_MAX : RSA_PSS_SALTLEN_AUTO;
            else {
                JS_ThrowTypeError(ctx,
                    "RSA.signPSS: saltLen string must be \"auto\" or \"max\" (got \"%s\")", sv);
                JS_FreeCString(ctx, sv);
                JS_FreeValue(ctx, v);
                return -1;
            }
            JS_FreeCString(ctx, sv);
        } else {
            int64_t n = 0;
            if (JS_ToInt64(ctx, &n, v)) {
                JS_FreeValue(ctx, v);
                return -1;
            }
            if (n < 0 || n > 4096) {
                JS_ThrowRangeError(ctx, "RSA.signPSS: saltLen must be 0..4096");
                JS_FreeValue(ctx, v);
                return -1;
            }
            *psaltlen = (int)n;
        }
    }
    JS_FreeValue(ctx, v);
    v = JS_GetPropertyStr(ctx, opts, "mgf1Hash");
    if (JS_IsException(v))
        return -1;
    if (!JS_IsUndefined(v) && !JS_IsNull(v)) {
        const char* mv = JS_ToCString(ctx, v);
        JS_FreeValue(ctx, v);
        if (!mv)
            return -1;
        *pmgf = dyn_standalone_md(mv);
        if (!*pmgf) {
            JS_ThrowTypeError(ctx,
                "RSA.signPSS: unsupported mgf1Hash algorithm (sha1|sha256|sha384|sha512)");
            JS_FreeCString(ctx, mv);
            return -1;
        }
        JS_FreeCString(ctx, mv);
    }
    return 0;
}

JSValue dyn_rsa_sign_pss(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    const char* md_str = NULL;
    const EVP_MD *md, *mgf = NULL;
    uint8_t* pem_bytes = NULL;
    size_t pem_len = 0;
    const uint8_t* msg;
    size_t msglen;
    const char* msg_ow = NULL;
    EVP_PKEY* k = NULL;
    EVP_MD_CTX* mctx = NULL;
    EVP_PKEY_CTX* pctx = NULL;
    int saltlen = RSA_PSS_SALTLEN_AUTO;
    size_t siglen = 0;
    uint8_t* sig = NULL;
    JSValue ret = JS_EXCEPTION;
    (void)this_val;

    if (argc < 3)
        return JS_ThrowTypeError(ctx, "RSA.signPSS(md, privateKey, msg[, opts])");
    md_str = JS_ToCString(ctx, argv[0]);
    if (!md_str)
        return JS_EXCEPTION;
    md = dyn_standalone_md(md_str);
    JS_FreeCString(ctx, md_str);
    if (!md)
        return JS_ThrowTypeError(ctx, "RSA.signPSS: unsupported hash algorithm");

    if (argc > 3 && !JS_IsUndefined(argv[3]) && !JS_IsNull(argv[3])) {
        if (!JS_IsObject(argv[3]))
            return JS_ThrowTypeError(ctx, "RSA.signPSS: opts must be an object");
        if (rsa_pss_read_opts(ctx, argv[3], 1, md, &mgf, &saltlen))
            return JS_EXCEPTION;
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
        return JS_ThrowTypeError(ctx, "RSA.signPSS: invalid private key PEM");
    if (!EVP_PKEY_is_a(k, "RSA") && !EVP_PKEY_is_a(k, "RSA-PSS")) {
        EVP_PKEY_free(k);
        return JS_ThrowTypeError(ctx, "RSA.signPSS: key is not an RSA key");
    }
    if (dyn_rsa_too_small(k)) {
        int bits = EVP_PKEY_get_bits(k);
        EVP_PKEY_free(k);
        return JS_ThrowRangeError(ctx,
            "RSA.signPSS: key is %d bits; the minimum accepted is %d",
            bits, DYN_RSA_MIN_BITS);
    }
    if (dyn_crypto_data(ctx, argv[2], &msg, &msglen, &msg_ow)) {
        EVP_PKEY_free(k);
        return JS_EXCEPTION;
    }

    mctx = EVP_MD_CTX_new();
    if (!mctx || EVP_DigestSignInit(mctx, &pctx, md, NULL, k) <= 0 || rsa_pss_apply_opts(pctx, md, mgf ? mgf : md, saltlen) != 0 || EVP_DigestSign(mctx, NULL, &siglen, msg, msglen) <= 0) {
        ERR_clear_error();
        JS_ThrowInternalError(ctx, "RSA.signPSS: signing initialization failed");
        goto done;
    }
    sig = (uint8_t*)malloc(siglen ? siglen : 1);
    if (!sig) {
        JS_ThrowOutOfMemory(ctx);
        goto done;
    }
    if (EVP_DigestSign(mctx, sig, &siglen, msg, msglen) <= 0) {
        ERR_clear_error();
        JS_ThrowInternalError(ctx, "RSA.signPSS: signing failed");
        goto done;
    }
    ret = dyn_nat_u8_copy(ctx, sig, siglen);

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

JSValue dyn_rsa_verify_pss(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    const char* md_str = NULL;
    const EVP_MD *md, *mgf = NULL;
    uint8_t* pem_bytes = NULL;
    size_t pem_len = 0;
    uint8_t* sig_bytes = NULL;
    size_t sig_len = 0;
    const uint8_t* msg;
    size_t msglen;
    const char* msg_ow = NULL;
    EVP_PKEY* k = NULL;
    EVP_MD_CTX* mctx = NULL;
    EVP_PKEY_CTX* pctx = NULL;
    int saltlen = RSA_PSS_SALTLEN_AUTO;
    int ok = 0;
    (void)this_val;

    if (argc < 4)
        return JS_ThrowTypeError(ctx, "RSA.verifyPSS(md, publicKey, msg, sig[, opts])");
    md_str = JS_ToCString(ctx, argv[0]);
    if (!md_str)
        return JS_EXCEPTION;
    md = dyn_standalone_md(md_str);
    JS_FreeCString(ctx, md_str);
    if (!md)
        return JS_ThrowTypeError(ctx, "RSA.verifyPSS: unsupported hash algorithm");

    if (argc > 4 && !JS_IsUndefined(argv[4]) && !JS_IsNull(argv[4])) {
        if (!JS_IsObject(argv[4]))
            return JS_ThrowTypeError(ctx, "RSA.verifyPSS: opts must be an object");
        if (rsa_pss_read_opts(ctx, argv[4], 0, md, &mgf, &saltlen))
            return JS_EXCEPTION;
    }

    if (dyn_crypto_copy(ctx, argv[1], &pem_bytes, &pem_len))
        return JS_EXCEPTION;
    k = jwt_pem_key((const char*)pem_bytes, pem_len, 0);
    free(pem_bytes);
    if (!k)
        return JS_ThrowTypeError(ctx, "RSA.verifyPSS: invalid public key PEM");
    if (!EVP_PKEY_is_a(k, "RSA") && !EVP_PKEY_is_a(k, "RSA-PSS")) {
        EVP_PKEY_free(k);
        return JS_ThrowTypeError(ctx, "RSA.verifyPSS: key is not an RSA key");
    }
    if (dyn_rsa_too_small(k)) {
        int bits = EVP_PKEY_get_bits(k);
        EVP_PKEY_free(k);
        return JS_ThrowRangeError(ctx,
            "RSA.verifyPSS: key is %d bits; the minimum accepted is %d",
            bits, DYN_RSA_MIN_BITS);
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
    if (mctx && EVP_DigestVerifyInit(mctx, &pctx, md, NULL, k) > 0 && EVP_PKEY_CTX_set_rsa_padding(pctx, RSA_PKCS1_PSS_PADDING) > 0 && EVP_PKEY_CTX_set_rsa_pss_saltlen(pctx, saltlen) > 0 && EVP_PKEY_CTX_set_rsa_mgf1_md(pctx, mgf ? mgf : md) > 0) {
        if (EVP_DigestVerify(mctx, sig_bytes, sig_len, msg, msglen) == 1)
            ok = 1;
    }
    ERR_clear_error();

    free(sig_bytes);
    if (mctx)
        EVP_MD_CTX_free(mctx);
    if (k)
        EVP_PKEY_free(k);
    if (msg_ow)
        JS_FreeCString(ctx, msg_ow);
    return JS_NewBool(ctx, ok);
}

static JSValue dyn_rsa_oaep(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    const char* md_str = NULL;
    const EVP_MD *md = NULL, *mgf = NULL;
    uint8_t* pem_bytes = NULL;
    size_t pem_len = 0;
    const uint8_t* in;
    size_t inlen;
    const char* in_ow = NULL;
    const uint8_t* label = NULL;
    size_t labellen = 0;
    const char* label_ow = NULL;
    EVP_PKEY* k = NULL;
    EVP_PKEY_CTX* pctx = NULL;
    uint8_t* out = NULL;
    size_t outlen = 0;
    char* label_copy = NULL;
    JSValue ret = JS_EXCEPTION;

    if (argc < 2)
        return JS_ThrowTypeError(ctx, magic ? "RSA-OAEP open(privateKey, sealed[, opts])" : "RSA-OAEP seal(publicKey, plaintext[, opts])");
    if (argc > 2 && !JS_IsUndefined(argv[2]) && !JS_IsNull(argv[2])) {
        JSValue v;
        if (!JS_IsObject(argv[2]))
            return JS_ThrowTypeError(ctx, "RSA.OAEP: opts must be an object");
        v = JS_GetPropertyStr(ctx, argv[2], "md");
        if (JS_IsException(v))
            return JS_EXCEPTION;
        if (!JS_IsUndefined(v) && !JS_IsNull(v)) {
            md_str = JS_ToCString(ctx, v);
            JS_FreeValue(ctx, v);
            if (!md_str)
                return JS_EXCEPTION;
            md = dyn_standalone_md(md_str);
            JS_FreeCString(ctx, md_str);
            md_str = NULL;
            if (!md) {
                JS_ThrowTypeError(ctx,
                    "RSA.OAEP: unsupported md algorithm (sha1|sha256|sha384|sha512)");
                return JS_EXCEPTION;
            }
        }
        v = JS_GetPropertyStr(ctx, argv[2], "mgf1Hash");
        if (JS_IsException(v))
            return JS_EXCEPTION;
        if (!JS_IsUndefined(v) && !JS_IsNull(v)) {
            const char* mv = JS_ToCString(ctx, v);
            JS_FreeValue(ctx, v);
            if (!mv)
                return JS_EXCEPTION;
            mgf = dyn_standalone_md(mv);
            JS_FreeCString(ctx, mv);
            if (!mgf) {
                JS_ThrowTypeError(ctx,
                    "RSA.OAEP: unsupported mgf1Hash algorithm (sha1|sha256|sha384|sha512)");
                return JS_EXCEPTION;
            }
        }
        v = JS_GetPropertyStr(ctx, argv[2], "label");
        if (JS_IsException(v))
            return JS_EXCEPTION;
        if (!JS_IsUndefined(v) && !JS_IsNull(v)) {
            if (dyn_crypto_data(ctx, v, &label, &labellen, &label_ow)) {
                JS_FreeValue(ctx, v);
                return JS_EXCEPTION;
            }
            label_copy = (char*)malloc(labellen ? labellen : 1);
            if (!label_copy) {
                if (label_ow)
                    JS_FreeCString(ctx, label_ow);
                JS_FreeValue(ctx, v);
                return JS_ThrowOutOfMemory(ctx);
            }
            if (labellen)
                memcpy(label_copy, label, labellen);
            if (label_ow)
                JS_FreeCString(ctx, label_ow);
            label = NULL;
        }
        JS_FreeValue(ctx, v);
    }
    if (!md)
        md = EVP_sha256();

    if (dyn_crypto_copy(ctx, argv[0], &pem_bytes, &pem_len)) {
        free(label_copy);
        return JS_EXCEPTION;
    }
    k = jwt_pem_key((const char*)pem_bytes, pem_len, magic);
    if (pem_bytes) {
        dyn_wipe(pem_bytes, pem_len);
        free(pem_bytes);
        pem_bytes = NULL;
    }
    if (!k) {
        free(label_copy);
        return JS_ThrowTypeError(ctx, magic ? "RSA.OAEP.open: invalid private key PEM" : "RSA.OAEP.seal: invalid public key PEM");
    }
    if (!EVP_PKEY_is_a(k, "RSA")) {
        EVP_PKEY_free(k);
        free(label_copy);
        return JS_ThrowTypeError(ctx, "RSA.OAEP: key is not an RSA key");
    }
    if (dyn_rsa_too_small(k)) {
        int bits = EVP_PKEY_get_bits(k);
        EVP_PKEY_free(k);
        free(label_copy);
        return JS_ThrowRangeError(ctx,
            "RSA.OAEP: key is %d bits; the minimum accepted is %d",
            bits, DYN_RSA_MIN_BITS);
    }
    if (dyn_crypto_data(ctx, argv[1], &in, &inlen, &in_ow)) {
        EVP_PKEY_free(k);
        free(label_copy);
        return JS_EXCEPTION;
    }
    if (inlen > (size_t)INT32_MAX) {
        EVP_PKEY_free(k);
        free(label_copy);
        return JS_ThrowRangeError(ctx, "RSA.OAEP: input exceeds %d bytes", INT32_MAX);
    }

    pctx = EVP_PKEY_CTX_new(k, NULL);
    if (!pctx) {
        JS_ThrowOutOfMemory(ctx);
        goto done;
    }
    if (magic) {
        outlen = (size_t)EVP_PKEY_get_bits(k) / 8;
        out = (uint8_t*)malloc(outlen ? outlen : 1);
        if (!out) {
            JS_ThrowOutOfMemory(ctx);
            goto done;
        }
        if (EVP_PKEY_decrypt_init(pctx) <= 0 || EVP_PKEY_CTX_set_rsa_padding(pctx, RSA_PKCS1_OAEP_PADDING) <= 0 || EVP_PKEY_CTX_set_rsa_oaep_md(pctx, md) <= 0 || EVP_PKEY_CTX_set_rsa_mgf1_md(pctx, mgf ? mgf : md) <= 0)
            goto oaepfail;
        if (label_copy && EVP_PKEY_CTX_set0_rsa_oaep_label(pctx, label_copy, (int)labellen) <= 0)
            goto oaepfail;
        label_copy = NULL;
        if (EVP_PKEY_decrypt(pctx, out, &outlen, in, inlen) <= 0) {
            ERR_clear_error();
            JS_ThrowTypeError(ctx, "RSA.OAEP.open: decryption failed (wrong key, truncated input, or tampered blob)");
            goto done;
        }
        ret = dyn_nat_u8_copy(ctx, out, outlen);
        goto done;
    } else {
        if (EVP_PKEY_encrypt_init(pctx) <= 0 || EVP_PKEY_CTX_set_rsa_padding(pctx, RSA_PKCS1_OAEP_PADDING) <= 0 || EVP_PKEY_CTX_set_rsa_oaep_md(pctx, md) <= 0 || EVP_PKEY_CTX_set_rsa_mgf1_md(pctx, mgf ? mgf : md) <= 0)
            goto oaepfail;
        if (label_copy && EVP_PKEY_CTX_set0_rsa_oaep_label(pctx, label_copy, (int)labellen) <= 0)
            goto oaepfail;
        label_copy = NULL;
        if (EVP_PKEY_encrypt(pctx, NULL, &outlen, in, inlen) <= 0)
            goto oaepfail;
        out = (uint8_t*)malloc(outlen ? outlen : 1);
        if (!out) {
            JS_ThrowOutOfMemory(ctx);
            goto done;
        }
        if (EVP_PKEY_encrypt(pctx, out, &outlen, in, inlen) <= 0) {
            ERR_clear_error();
            JS_ThrowInternalError(ctx, "RSA.OAEP.seal: encryption failed");
            goto done;
        }
        ret = dyn_nat_u8_copy(ctx, out, outlen);
        goto done;
    }
oaepfail:
    ERR_clear_error();
    JS_ThrowInternalError(ctx, "RSA.OAEP: initialization failed");
done:
    free(label_copy);
    free(out);
    if (pctx)
        EVP_PKEY_CTX_free(pctx);
    if (k)
        EVP_PKEY_free(k);
    if (in_ow)
        JS_FreeCString(ctx, in_ow);
    return ret;
}

JSValue dyn_rsa_oaep_seal(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    return dyn_rsa_oaep(ctx, this_val, argc, argv, 0);
}

JSValue dyn_rsa_oaep_open(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    return dyn_rsa_oaep(ctx, this_val, argc, argv, 1);
}
#endif

#endif
