#include "crypto-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_CRYPTO)

#ifdef CONFIG_OPENSSL
#define AEAD_TAG 16
#define AEAD_NONCE 12

typedef struct {
    const EVP_CIPHER* cipher;
    uint8_t key[32];
    unsigned keylen;
    const char* name;
    EVP_CIPHER_CTX* keyed[2];
} dyn_aead_res_t;

static EVP_CIPHER_CTX* dyn_aead_keyed_ctx(dyn_aead_res_t* a, int decrypt)
{
    EVP_CIPHER_CTX* c = a->keyed[decrypt];
    int ok;
    if (c)
        return c;
    c = EVP_CIPHER_CTX_new();
    if (!c)
        return NULL;
    if (decrypt)
        ok = EVP_DecryptInit_ex(c, a->cipher, NULL, NULL, NULL)
            && EVP_CIPHER_CTX_ctrl(c, EVP_CTRL_AEAD_SET_IVLEN, 12, NULL) == 1
            && EVP_DecryptInit_ex(c, NULL, NULL, a->key, NULL);
    else
        ok = EVP_EncryptInit_ex(c, a->cipher, NULL, NULL, NULL)
            && EVP_CIPHER_CTX_ctrl(c, EVP_CTRL_AEAD_SET_IVLEN, 12, NULL) == 1
            && EVP_EncryptInit_ex(c, NULL, NULL, a->key, NULL);
    if (!ok) {
        EVP_CIPHER_CTX_free(c);
        ERR_clear_error();
        return NULL;
    }
    a->keyed[decrypt] = c;
    return c;
}

static void dyn_aead_keyed_drop(dyn_aead_res_t* a, int decrypt)
{
    if (a->keyed[decrypt]) {
        EVP_CIPHER_CTX_free(a->keyed[decrypt]);
        a->keyed[decrypt] = NULL;
    }
}

static void dyn_aead_dispose(void* native)
{
    dyn_aead_res_t* a = (dyn_aead_res_t*)native;
    if (!a)
        return;
    dyn_aead_keyed_drop(a, 0);
    dyn_aead_keyed_drop(a, 1);
    dyn_wipe(a, sizeof(*a));
    free(a);
}

JSValue dyn_aead_ctor_common(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv, int chacha)
{
    const uint8_t* key;
    size_t keylen;
    const char* owned;
    dyn_aead_res_t* a;

    if (argc < 1)
        return JS_ThrowTypeError(ctx, chacha ? "new ChaCha20Poly1305(key): key must be 32 bytes" : "new AESGCM(key): key must be 16, 24 or 32 bytes");
    if (dyn_crypto_secret(ctx, argv[0], &key, &keylen, &owned))
        return JS_EXCEPTION;

    a = (dyn_aead_res_t*)calloc(1, sizeof(*a));
    if (!a) {
        if (owned)
            JS_FreeCString(ctx, owned);
        return JS_ThrowOutOfMemory(ctx);
    }
    if (chacha) {
        if (keylen != 32)
            goto badkey;
        a->cipher = EVP_chacha20_poly1305();
        a->name = "ChaCha20Poly1305";
    } else {
        switch (keylen) {
        case 16:
            a->cipher = EVP_aes_128_gcm();
            break;
        case 24:
            a->cipher = EVP_aes_192_gcm();
            break;
        case 32:
            a->cipher = EVP_aes_256_gcm();
            break;
        default:
            goto badkey;
        }
        a->name = "AESGCM";
    }
    memcpy(a->key, key, keylen);
    a->keylen = (unsigned)keylen;
    if (owned)
        JS_FreeCString(ctx, owned);
    return dyn_res_wrap(ctx, new_target,
        chacha ? dyn_chapoly_class_id : dyn_aesgcm_class_id,
        a, dyn_aead_dispose);
badkey:
    free(a);
    if (owned)
        JS_FreeCString(ctx, owned);
    return JS_ThrowTypeError(ctx, chacha ? "ChaCha20Poly1305: key must be exactly 32 bytes" : "AESGCM: key must be 16, 24 or 32 bytes");
}

JSValue dyn_aead_op(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv, int magic)
{
    dyn_aead_res_t* a;
    const uint8_t *nonce = NULL, *in = NULL, *aad = NULL;
    size_t noncelen = 0, inlen = 0, aadlen = 0;
    const char *o1 = NULL, *o2 = NULL, *o3 = NULL;
    EVP_CIPHER_CTX* c = NULL;
    JSClassID cid;
    uint8_t* out = NULL;
    size_t outcap;
    int l1 = 0, l2 = 0, ok = 0;
    JSValue ret = JS_EXCEPTION;

    if (argc < 2)
        return JS_ThrowTypeError(ctx, magic ? "open(nonce, sealed[, aad])" : "seal(nonce, plaintext[, aad])");
    if (dyn_crypto_data(ctx, argv[0], &nonce, &noncelen, &o1))
        return JS_EXCEPTION;
    if (dyn_crypto_data(ctx, argv[1], &in, &inlen, &o2))
        goto done;
    if (argc > 2 && !JS_IsUndefined(argv[2]) && !JS_IsNull(argv[2])) {
        if (dyn_crypto_data(ctx, argv[2], &aad, &aadlen, &o3))
            goto done;
    }

    cid = JS_GetClassID(this_val) == dyn_chapoly_class_id
        ? dyn_chapoly_class_id
        : dyn_aesgcm_class_id;
    a = (dyn_aead_res_t*)dyn_res_native(ctx, this_val, cid);
    if (!a)
        goto done;

    if (inlen > (size_t)INT32_MAX || aadlen > (size_t)INT32_MAX) {
        JS_ThrowRangeError(ctx, "%s: input exceeds %d bytes", a->name, INT32_MAX);
        goto done;
    }
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
    out = (uint8_t*)malloc(outcap ? outcap : 1);
    if (!out) {
        JS_ThrowOutOfMemory(ctx);
        goto done;
    }

    c = dyn_aead_keyed_ctx(a, magic ? 1 : 0);
    if (!c) {
        JS_ThrowOutOfMemory(ctx);
        goto done;
    }

    if (!magic) {
        ok = EVP_EncryptInit_ex(c, NULL, NULL, NULL, nonce);
        if (ok && aadlen)
            ok = EVP_EncryptUpdate(c, NULL, &l1, aad, (int)aadlen);
        if (ok)
            ok = EVP_EncryptUpdate(c, out, &l1, in, (int)inlen);
        if (ok)
            ok = EVP_EncryptFinal_ex(c, out + l1, &l2);
        if (ok)
            ok = EVP_CIPHER_CTX_ctrl(c, EVP_CTRL_AEAD_GET_TAG, AEAD_TAG,
                     out + inlen)
                == 1;
        if (!ok) {
            JS_ThrowInternalError(ctx, "%s: seal failed", a->name);
            goto done;
        }
    } else {
        ok = EVP_DecryptInit_ex(c, NULL, NULL, NULL, nonce);
        if (ok && aadlen)
            ok = EVP_DecryptUpdate(c, NULL, &l1, aad, (int)aadlen);
        if (ok)
            ok = EVP_DecryptUpdate(c, out, &l1, in, (int)(inlen - AEAD_TAG));
        if (ok)
            ok = EVP_CIPHER_CTX_ctrl(c, EVP_CTRL_AEAD_SET_TAG, AEAD_TAG,
                     DYN_UNCONST(in + inlen - AEAD_TAG))
                == 1;
        if (ok)
            ok = EVP_DecryptFinal_ex(c, out + l1, &l2) > 0;
        if (!ok) {
            dyn_wipe(out, outcap ? outcap : 1);
            ERR_clear_error();
            JS_ThrowTypeError(ctx, "%s: authentication failed", a->name);
            goto done;
        }
    }
    ret = dyn_nat_u8_copy(ctx, out, outcap);
done:
    if (c && !ok)
        dyn_aead_keyed_drop(a, magic ? 1 : 0);
    if (out) {
        dyn_wipe(out, outcap ? outcap : 1);
        free(out);
    }
    if (o1)
        JS_FreeCString(ctx, o1);
    if (o2)
        JS_FreeCString(ctx, o2);
    if (o3)
        JS_FreeCString(ctx, o3);
    return ret;
}

JSValue dyn_aead_seal_random(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    uint8_t nonce[AEAD_NONCE];
    JSValueConst opargv[3];
    JSValue noncev, sealed, obj;

    if (argc < 1)
        return JS_ThrowTypeError(ctx, "sealRandom(plaintext[, aad])");
    if (dyn_os_entropy(nonce, AEAD_NONCE) < 0)
        return JS_ThrowInternalError(ctx, "entropy source failed");
    noncev = dyn_nat_u8_copy(ctx, nonce, AEAD_NONCE);
    if (JS_IsException(noncev))
        return JS_EXCEPTION;
    opargv[0] = noncev;
    opargv[1] = argv[0];
    opargv[2] = argc > 1 ? argv[1] : JS_UNDEFINED;
    sealed = dyn_aead_op(ctx, this_val, 3, opargv, 0);
    if (JS_IsException(sealed)) {
        JS_FreeValue(ctx, noncev);
        return JS_EXCEPTION;
    }
    obj = JS_NewObject(ctx);
    if (JS_IsException(obj)) {
        JS_FreeValue(ctx, noncev);
        JS_FreeValue(ctx, sealed);
        return JS_EXCEPTION;
    }
    JS_DefinePropertyValueStr(ctx, obj, "nonce", noncev, JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, obj, "sealed", sealed, JS_PROP_C_W_E);
    return obj;
}

JSValue dyn_aead_op_into(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    dyn_aead_res_t* a;
    const uint8_t *nonce = NULL, *in = NULL, *aad = NULL, *tagbuf = NULL;
    size_t noncelen = 0, inlen = 0, aadlen = 0;
    size_t off, blen, bpe, ab_size;
    size_t write_at, need, written;
    const char *o1 = NULL, *o2 = NULL, *o3 = NULL;
    uint8_t *base = NULL, *tagbase = NULL;
    size_t tagoff = 0, tagblen = 0, tagbpe = 0, tagab = 0;
    int prefix_nonce = 0, detached;
    EVP_CIPHER_CTX* c = NULL;
    JSClassID cid;
    int l1 = 0, l2 = 0, ok = 0;
    JSValue ret = JS_EXCEPTION;

    if (argc < 3)
        return JS_ThrowTypeError(ctx, magic ? "openInto(out, nonce, sealed[, aad][, opts])" : "sealInto(out, nonce, plaintext[, aad][, opts])");
    if (JS_CheckWritableBuffer(ctx, argv[0]) < 0)
        return JS_EXCEPTION;
    {
        JSValue ab = JS_GetTypedArrayBuffer(ctx, argv[0], &off, &blen, &bpe);
        if (JS_IsException(ab))
            return ab;
        base = JS_GetArrayBuffer(ctx, &ab_size, ab);
        JS_FreeValue(ctx, ab);
        if (!base)
            return JS_EXCEPTION;
        if (off > ab_size || blen > ab_size - off) {
            JS_ThrowRangeError(ctx, "typed array out of bounds");
            return JS_EXCEPTION;
        }
    }
    if (JS_IsUndefined(argv[1]) || JS_IsNull(argv[1])) {
        nonce = NULL;
        noncelen = 0;
    } else if (dyn_crypto_data(ctx, argv[1], &nonce, &noncelen, &o1)) {
        goto done;
    }
    if (dyn_crypto_data(ctx, argv[2], &in, &inlen, &o2))
        goto done;
    if (argc > 3 && !JS_IsUndefined(argv[3]) && !JS_IsNull(argv[3])) {
        if (JS_IsObject(argv[3]) && !JS_IsString(argv[3])) {
            int is_bytes = 0;
            JSValue probe = JS_GetTypedArrayBuffer(ctx, argv[3], NULL, NULL, NULL);
            if (!JS_IsException(probe)) {
                JS_FreeValue(ctx, probe);
                is_bytes = 1;
            } else {
                size_t absize;
                uint8_t* abp;
                JS_FreeValue(ctx, JS_GetException(ctx));
                abp = JS_GetArrayBuffer(ctx, &absize, argv[3]);
                if (abp) {
                    is_bytes = 1;
                } else {
                    JS_FreeValue(ctx, JS_GetException(ctx));
                }
            }
            if (!is_bytes) {
                JS_ThrowTypeError(ctx, "%sInto: aad must be bytes or a string, not an options object",
                    magic ? "open" : "seal");
                goto done;
            }
        }
        if (dyn_crypto_data(ctx, argv[3], &aad, &aadlen, &o3))
            goto done;
    }
    if (argc > 4 && !JS_IsUndefined(argv[4]) && !JS_IsNull(argv[4])) {
        JSValue v;
        if (!JS_IsObject(argv[4])) {
            JS_ThrowTypeError(ctx, "%sInto: opts must be an object",
                magic ? "open" : "seal");
            goto done;
        }
        v = JS_GetPropertyStr(ctx, argv[4], "prefixNonce");
        if (JS_IsException(v))
            goto done;
        if (!JS_IsUndefined(v) && !JS_IsNull(v)) {
            int b = JS_ToBool(ctx, v);
            if (b == -1) {
                JS_FreeValue(ctx, v);
                goto done;
            }
            prefix_nonce = b;
        }
        JS_FreeValue(ctx, v);
        v = JS_GetPropertyStr(ctx, argv[4], magic ? "tag" : "tagOut");
        if (JS_IsException(v))
            goto done;
        if (!JS_IsUndefined(v) && !JS_IsNull(v)) {
            JSValue tab;
            if (!magic && JS_CheckWritableBuffer(ctx, v) < 0) {
                JS_FreeValue(ctx, v);
                goto done;
            }
            tab = JS_GetTypedArrayBuffer(ctx, v, &tagoff, &tagblen,
                &tagbpe);
            if (JS_IsException(tab)) {
                JS_FreeValue(ctx, v);
                goto done;
            }
            tagbase = JS_GetArrayBuffer(ctx, &tagab, tab);
            JS_FreeValue(ctx, tab);
            if (!tagbase) {
                JS_FreeValue(ctx, v);
                goto done;
            }
            if (tagoff > tagab || tagblen > tagab - tagoff) {
                JS_FreeValue(ctx, v);
                JS_ThrowRangeError(ctx, "typed array out of bounds");
                goto done;
            }
        }
        JS_FreeValue(ctx, v);
        {
            JSPropertyEnum* props = NULL;
            uint32_t nprops = 0, i;
            if (JS_GetOwnPropertyNames(ctx, &props, &nprops, argv[4],
                    JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY)
                < 0)
                goto done;
            for (i = 0; i < nprops; i++) {
                const char* k = JS_AtomToCString(ctx, props[i].atom);
                if (!k) {
                    JS_FreePropertyEnum(ctx, props, nprops);
                    goto done;
                }
                if (strcmp(k, "prefixNonce") && strcmp(k, magic ? "tag" : "tagOut")) {
                    JS_ThrowTypeError(ctx,
                        "%sInto: unknown option \"%s\" (valid: %s, prefixNonce)",
                        magic ? "open" : "seal", k, magic ? "tag" : "tagOut");
                }
                JS_FreeCString(ctx, k);
            }
            JS_FreePropertyEnum(ctx, props, nprops);
            if (JS_HasException(ctx))
                goto done;
        }
    }
    detached = tagbase != NULL;

    cid = JS_GetClassID(this_val) == dyn_chapoly_class_id
        ? dyn_chapoly_class_id
        : dyn_aesgcm_class_id;
    a = (dyn_aead_res_t*)dyn_res_native(ctx, this_val, cid);
    if (!a)
        goto done;

    if (inlen > (size_t)INT32_MAX || aadlen > (size_t)INT32_MAX) {
        JS_ThrowRangeError(ctx, "%s: input exceeds %d bytes", a->name, INT32_MAX);
        goto done;
    }
    if (magic) {
        if (detached) {
            if (tagblen < AEAD_TAG) {
                JS_ThrowRangeError(ctx, "%s: openInto opts.tag must be %d bytes",
                    a->name, AEAD_TAG);
                goto done;
            }
            tagbuf = tagbase + tagoff;
        }
        if (prefix_nonce) {
            if (inlen < AEAD_NONCE + (detached ? 0 : AEAD_TAG)) {
                JS_ThrowTypeError(ctx, "%s: sealed blob is shorter than its nonce",
                    a->name);
                goto done;
            }
            nonce = in;
            noncelen = AEAD_NONCE;
            in += AEAD_NONCE;
            inlen -= AEAD_NONCE;
        } else if (noncelen != AEAD_NONCE) {
            JS_ThrowTypeError(ctx, "%s: nonce must be %d bytes", a->name,
                AEAD_NONCE);
            goto done;
        }
        if (!detached && inlen < AEAD_TAG) {
            JS_ThrowTypeError(ctx, "%s: sealed message is shorter than its tag",
                a->name);
            goto done;
        }
        need = inlen - (detached ? 0 : AEAD_TAG);
    } else {
        if (detached && tagblen < AEAD_TAG) {
            JS_ThrowRangeError(ctx, "%s: sealInto opts.tagOut must be %d bytes",
                a->name, AEAD_TAG);
            goto done;
        }
        if (noncelen != AEAD_NONCE) {
            JS_ThrowTypeError(ctx, "%s: nonce must be %d bytes", a->name,
                AEAD_NONCE);
            goto done;
        }
        need = (prefix_nonce ? AEAD_NONCE : 0) + inlen + (detached ? 0 : AEAD_TAG);
    }
    if (blen < need) {
        JS_ThrowRangeError(ctx,
            "%sInto: output buffer byteLength %zu is smaller than the %zu bytes required",
            magic ? "open" : "seal", blen, need);
        goto done;
    }
    write_at = off;
    if (prefix_nonce && !magic) {
        memcpy(base + off, nonce, AEAD_NONCE);
        write_at = off + AEAD_NONCE;
    }

    c = dyn_aead_keyed_ctx(a, magic ? 1 : 0);
    if (!c) {
        JS_ThrowOutOfMemory(ctx);
        goto done;
    }

    if (!magic) {
        uint8_t* outp = base + write_at;
        ok = EVP_EncryptInit_ex(c, NULL, NULL, NULL, nonce);
        if (ok && aadlen)
            ok = EVP_EncryptUpdate(c, NULL, &l1, aad, (int)aadlen);
        if (ok)
            ok = EVP_EncryptUpdate(c, outp, &l1, in, (int)inlen);
        if (ok)
            ok = EVP_EncryptFinal_ex(c, outp + l1, &l2);
        if (ok) {
            ok = EVP_CIPHER_CTX_ctrl(c, EVP_CTRL_AEAD_GET_TAG, AEAD_TAG,
                     detached ? (void*)(tagbase + tagoff)
                              : (void*)(outp + inlen))
                == 1;
        }
        if (!ok) {
            JS_ThrowInternalError(ctx, "%s: seal failed", a->name);
            goto done;
        }
        written = (prefix_nonce ? AEAD_NONCE : 0) + inlen + (detached ? 0 : AEAD_TAG);
    } else {
        uint8_t* outp = base + off;
        const uint8_t* tagp = detached ? tagbuf : in + inlen - AEAD_TAG;
        ok = EVP_DecryptInit_ex(c, NULL, NULL, NULL, nonce);
        if (ok && aadlen)
            ok = EVP_DecryptUpdate(c, NULL, &l1, aad, (int)aadlen);
        if (ok)
            ok = EVP_DecryptUpdate(c, outp, &l1, in,
                (int)(inlen - (detached ? 0 : AEAD_TAG)));
        if (ok)
            ok = EVP_CIPHER_CTX_ctrl(c, EVP_CTRL_AEAD_SET_TAG, AEAD_TAG,
                     DYN_UNCONST(tagp))
                == 1;
        if (ok)
            ok = EVP_DecryptFinal_ex(c, outp + l1, &l2) > 0;
        if (!ok) {
            dyn_wipe(base + off, inlen - (detached ? 0 : AEAD_TAG));
            ERR_clear_error();
            JS_ThrowTypeError(ctx, "%s: authentication failed", a->name);
            goto done;
        }
        written = inlen - (detached ? 0 : AEAD_TAG);
    }
    ret = JS_NewInt64(ctx, (int64_t)written);
done:
    if (c && !ok)
        dyn_aead_keyed_drop(a, magic ? 1 : 0);
    if (o1)
        JS_FreeCString(ctx, o1);
    if (o2)
        JS_FreeCString(ctx, o2);
    if (o3)
        JS_FreeCString(ctx, o3);
    return ret;
}
#endif

#endif
