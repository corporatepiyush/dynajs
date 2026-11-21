#include "crypto-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_CRYPTO)

static const char* const arg_opts_keys[] = { "iterations", "memory", "parallelism", "hashLen", "encoded", "dyna" };

static int arg_read_opts(JSContext* ctx, JSValueConst obj, int64_t* pt,
    int64_t* pm, int64_t* pp, int64_t* poutlen,
    int* pencoded, uint32_t* pcap)
{
    if (dyn_opts_strict(ctx, obj, arg_opts_keys, 6) || dyn_kdf_meta(ctx, obj, "Argon2id", pcap))
        return -1;
    static const char* const names[] = { "iterations", "memory",
        "parallelism", "hashLen" };
    int64_t* const slots[] = { pt, pm, pp, poutlen };
    int i;
    for (i = 0; i < 4; i++) {
        JSValue v = JS_GetPropertyStr(ctx, obj, names[i]);
        if (JS_IsException(v))
            return -1;
        if (!JS_IsUndefined(v) && JS_ToInt64(ctx, slots[i], v)) {
            JS_FreeValue(ctx, v);
            return -1;
        }
        JS_FreeValue(ctx, v);
    }
    if (pencoded) {
        JSValue v = JS_GetPropertyStr(ctx, obj, "encoded");
        if (JS_IsException(v))
            return -1;
        if (!JS_IsUndefined(v)) {
            *pencoded = JS_ToBool(ctx, v);
            if (*pencoded == -1) {
                JS_FreeValue(ctx, v);
                return -1;
            }
        }
        JS_FreeValue(ctx, v);
    }
    if (*pt < 1 || *pt > ARG2_MAX_TIME) {
        JS_ThrowRangeError(ctx, "Argon2id: iterations must be 1..%d", ARG2_MAX_TIME);
        return -1;
    }
    if (*pp < 1 || *pp > ARG2_MAX_LANES) {
        JS_ThrowRangeError(ctx, "Argon2id: parallelism must be 1..%d", ARG2_MAX_LANES);
        return -1;
    }
    if (*pm < 8 * *pp || *pm > ARG2_MAX_SYNC_MEMORY) {
        JS_ThrowRangeError(ctx, "Argon2id: memory (KiB) must be 8*parallelism..%d (1 GiB sync cap)", ARG2_MAX_SYNC_MEMORY);
        return -1;
    }
    if (*poutlen < 4 || *poutlen > ARG2_MAX_OUT) {
        JS_ThrowRangeError(ctx, "Argon2id: hashLen must be 4..%d", ARG2_MAX_OUT);
        return -1;
    }
    return 0;
}

JSValue dyn_argon2_hash_js(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    uint8_t *pwd = NULL, *salt = NULL, *out = NULL;
    size_t pwdlen = 0, saltlen = 0;
    int64_t t = 3, m = 65536, p = 4, outlen = 32;
    int encoded = 1, rc;
    uint32_t cap = 0;
    JSValue ret = JS_EXCEPTION;
    (void)this_val;

    if (argc < 2)
        return JS_ThrowTypeError(ctx, "Argon2id.hash(password, salt[, opts])");
    {
        const uint8_t* d;
        size_t n;
        const char* ow;
        if (dyn_crypto_data(ctx, argv[0], &d, &n, &ow))
            return JS_EXCEPTION;
        pwd = (uint8_t*)malloc(n ? n : 1);
        if (!pwd) {
            if (ow)
                JS_FreeCString(ctx, ow);
            return JS_ThrowOutOfMemory(ctx);
        }
        if (n)
            memcpy(pwd, d, n);
        pwdlen = n;
        if (ow)
            JS_FreeCString(ctx, ow);
        if (dyn_crypto_data(ctx, argv[1], &d, &n, &ow))
            goto done;
        salt = (uint8_t*)malloc(n ? n : 1);
        if (!salt) {
            if (ow)
                JS_FreeCString(ctx, ow);
            JS_ThrowOutOfMemory(ctx);
            goto done;
        }
        if (n)
            memcpy(salt, d, n);
        saltlen = n;
        if (ow)
            JS_FreeCString(ctx, ow);
    }
    if (argc > 2 && JS_IsObject(argv[2])) {
        if (arg_read_opts(ctx, argv[2], &t, &m, &p, &outlen, &encoded, &cap))
            goto done;
    }
    if (pwdlen > ARG2_MAX_PWD || saltlen > ARG2_MAX_SALT || saltlen < ARG2_MIN_SALT_LEN) {
        JS_ThrowRangeError(ctx, "Argon2id: salt must be 8..%d bytes", ARG2_MAX_SALT);
        goto done;
    }

    out = (uint8_t*)malloc((size_t)outlen);
    if (!out) {
        JS_ThrowOutOfMemory(ctx);
        goto done;
    }
    if (dyn_kdf_begin(ctx, 1, cap))
        goto done;
    rc = DYN_OFFLOAD_CPU(arg_hash_id(pwd, pwdlen, salt, saltlen, (uint32_t)t, (uint32_t)m,
        (uint32_t)p, out, (size_t)outlen));
    dyn_kdf_end(ctx, 1);
    switch (rc) {
    case 0:
        if (encoded) {
            char* phc = arg_phc_encode(salt, saltlen, out, (size_t)outlen,
                (uint32_t)t, (uint32_t)m, (uint32_t)p);
            if (!phc) {
                JS_ThrowOutOfMemory(ctx);
                break;
            }
            ret = JS_NewString(ctx, phc);
            dyn_wipe(phc, strlen(phc));
            free(phc);
        } else {
            ret = dyn_nat_u8_copy(ctx, out, (size_t)outlen);
        }
        break;
    case -2:
        JS_ThrowOutOfMemory(ctx);
        break;
    default:
        JS_ThrowRangeError(ctx, "Argon2id: parameters rejected");
        break;
    }
done:
    if (out) {
        dyn_wipe(out, (size_t)outlen);
        free(out);
    }
    if (pwd) {
        dyn_wipe(pwd, pwdlen);
        free(pwd);
    }
    if (salt) {
        dyn_wipe(salt, saltlen);
        free(salt);
    }
    return ret;
}

static JSValue dyn_argon2_verify_phc(JSContext* ctx, JSValueConst hashstr,
    JSValueConst password, uint32_t cap)
{
    uint8_t *pwd = NULL, *salt = NULL, *expected = NULL, *out = NULL;
    size_t pwdlen = 0, saltlen = 0, expectedlen = 0;
    size_t salt_need, tag_need;
    uint32_t t, m, p;
    const char *phc = NULL, *psalt, *ptag;
    size_t phclen, psaltlen, ptaglen;
    JSValue ret = JS_EXCEPTION;
    int rc;

    phc = JS_ToCStringLen(ctx, &phclen, hashstr);
    if (!phc)
        return JS_EXCEPTION;
    rc = arg_phc_parse(phc, &psalt, &psaltlen, &ptag, &ptaglen, &t, &m, &p);
    if (rc) {
        JS_FreeCString(ctx, phc);
        phc = NULL;
        if (rc == -2)
            return JS_ThrowRangeError(ctx,
                "Argon2id.verify: PHC memory (KiB) exceeds the %d (1 GiB) verify cap",
                ARG2_MAX_SYNC_MEMORY);
        return JS_ThrowTypeError(ctx,
            "Argon2id.verify: not a valid $argon2id$ PHC string "
            "(expected $argon2id$v=19$m=..,t=..,p=..<b64 salt>$<b64 hash>)");
    }
    salt_need = psaltlen / 4 * 3 + 3;
    tag_need = ptaglen / 4 * 3 + 3;
    salt = (uint8_t*)malloc(salt_need);
    expected = (uint8_t*)malloc(tag_need);
    pwdlen = 0;
    saltlen = 0;
    expectedlen = 0;
    if (!salt || !expected) {
        JS_FreeCString(ctx, phc);
        JS_ThrowOutOfMemory(ctx);
        goto done;
    }
    if (arg_b64u_decode(psalt, psaltlen, salt, &saltlen) || arg_b64u_decode(ptag, ptaglen, expected, &expectedlen)) {
        JS_FreeCString(ctx, phc);
        phc = NULL;
        JS_ThrowTypeError(ctx, "Argon2id.verify: PHC string has corrupt base64");
        goto done;
    }
    JS_FreeCString(ctx, phc);
    phc = NULL;
    if (saltlen < ARG2_MIN_SALT_LEN || saltlen > ARG2_MAX_SALT || expectedlen < 4 || expectedlen > ARG2_MAX_OUT) {
        JS_FreeCString(ctx, phc);
        phc = NULL;
        JS_ThrowTypeError(ctx, "Argon2id.verify: PHC string has out-of-range salt or hash length");
        goto done;
    }
    {
        const uint8_t* d;
        size_t n;
        const char* ow;
        if (dyn_crypto_data(ctx, password, &d, &n, &ow))
            goto done;
        pwd = (uint8_t*)malloc(n ? n : 1);
        if (!pwd) {
            if (ow)
                JS_FreeCString(ctx, ow);
            JS_ThrowOutOfMemory(ctx);
            goto done;
        }
        if (n)
            memcpy(pwd, d, n);
        pwdlen = n;
        if (ow)
            JS_FreeCString(ctx, ow);
    }
    out = (uint8_t*)malloc(expectedlen);
    if (!out) {
        JS_ThrowOutOfMemory(ctx);
        goto done;
    }
    if (dyn_kdf_begin(ctx, 1, cap))
        goto done;
    rc = DYN_OFFLOAD_CPU(arg_hash_id(pwd, pwdlen, salt, saltlen, t, m, p, out, expectedlen));
    dyn_kdf_end(ctx, 1);
    if (rc == 0) {
        ret = JS_NewBool(ctx, dyn_ct_equal(out, expected, expectedlen) == 1);
    } else if (rc == -2) {
        JS_ThrowOutOfMemory(ctx);
    } else {
        JS_ThrowRangeError(ctx, "Argon2id: parameters rejected");
    }
done:
    if (phc)
        JS_FreeCString(ctx, phc);
    if (out) {
        dyn_wipe(out, expectedlen);
        free(out);
    }
    if (expected) {
        dyn_wipe(expected, expectedlen);
        free(expected);
    }
    if (pwd) {
        dyn_wipe(pwd, pwdlen);
        free(pwd);
    }
    if (salt) {
        dyn_wipe(salt, saltlen);
        free(salt);
    }
    return ret;
}

JSValue dyn_argon2_verify_js(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    uint8_t *pwd = NULL, *salt = NULL, *expected = NULL, *out = NULL;
    size_t pwdlen = 0, saltlen = 0, expectedlen = 0;
    int64_t t = 3, m = 65536, p = 4, outlen = 32;
    JSValue ret = JS_EXCEPTION;
    int rc = 0, r = 0;
    uint32_t cap = 0;
    (void)this_val;

    if (argc == 2 || (argc == 3 && JS_IsObject(argv[2]) && JS_GetBufferKind(argv[2]) == JS_BUFFER_KIND_NONE)) {
        static const char* const phc_keys[] = { "dyna" };
        if (argc == 3 && (dyn_opts_strict(ctx, argv[2], phc_keys, 1) || dyn_kdf_meta(ctx, argv[2], "Argon2id.verify", &cap)))
            return JS_EXCEPTION;
        return dyn_argon2_verify_phc(ctx, argv[0], argv[1], cap);
    }
    if (argc < 3)
        return JS_ThrowTypeError(ctx, "Argon2id.verify(password, salt, expectedHash[, opts]) or verify(hashString, password)");
    {
        const uint8_t* d;
        size_t n;
        const char* ow;
        if (dyn_crypto_data(ctx, argv[0], &d, &n, &ow))
            return JS_EXCEPTION;
        pwd = (uint8_t*)malloc(n ? n : 1);
        if (!pwd) {
            if (ow)
                JS_FreeCString(ctx, ow);
            return JS_ThrowOutOfMemory(ctx);
        }
        if (n)
            memcpy(pwd, d, n);
        pwdlen = n;
        if (ow)
            JS_FreeCString(ctx, ow);
        if (dyn_crypto_data(ctx, argv[1], &d, &n, &ow))
            goto done;
        salt = (uint8_t*)malloc(n ? n : 1);
        if (!salt) {
            if (ow)
                JS_FreeCString(ctx, ow);
            JS_ThrowOutOfMemory(ctx);
            goto done;
        }
        if (n)
            memcpy(salt, d, n);
        saltlen = n;
        if (ow)
            JS_FreeCString(ctx, ow);
        if (dyn_crypto_data(ctx, argv[2], &d, &n, &ow))
            goto done;
        expected = (uint8_t*)malloc(n ? n : 1);
        if (!expected) {
            if (ow)
                JS_FreeCString(ctx, ow);
            JS_ThrowOutOfMemory(ctx);
            goto done;
        }
        if (n)
            memcpy(expected, d, n);
        expectedlen = n;
        if (ow)
            JS_FreeCString(ctx, ow);
    }
    if (argc > 3 && JS_IsObject(argv[3])) {
        if (arg_read_opts(ctx, argv[3], &t, &m, &p, &outlen, NULL, &cap))
            goto done;
    }
    if (pwdlen > ARG2_MAX_PWD || saltlen > ARG2_MAX_SALT || saltlen < ARG2_MIN_SALT_LEN) {
        JS_ThrowRangeError(ctx, "Argon2id: salt must be 8..%d bytes", ARG2_MAX_SALT);
        goto done;
    }
    out = (uint8_t*)malloc((size_t)outlen);
    if (!out) {
        JS_ThrowOutOfMemory(ctx);
        goto done;
    }
    if (dyn_kdf_begin(ctx, 1, cap))
        goto done;
    rc = DYN_OFFLOAD_CPU(arg_hash_id(pwd, pwdlen, salt, saltlen, (uint32_t)t, (uint32_t)m,
        (uint32_t)p, out, (size_t)outlen));
    dyn_kdf_end(ctx, 1);
    if (rc == 0) {
        r = (expectedlen == (size_t)outlen) && dyn_ct_equal(out, expected, expectedlen);
        ret = JS_NewBool(ctx, r);
    } else if (rc == -2) {
        JS_ThrowOutOfMemory(ctx);
    } else {
        JS_ThrowRangeError(ctx, "Argon2id: parameters rejected");
    }
done:
    if (out) {
        dyn_wipe(out, (size_t)outlen);
        free(out);
    }
    if (expected) {
        dyn_wipe(expected, expectedlen);
        free(expected);
    }
    if (pwd) {
        dyn_wipe(pwd, pwdlen);
        free(pwd);
    }
    if (salt) {
        dyn_wipe(salt, saltlen);
        free(salt);
    }
    return ret;
}

#endif
