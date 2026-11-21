#include "crypto-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_CRYPTO)

#define DYN_PBKDF2_MAX_COMPRESSIONS (1 << 24)
#define DYN_SCRYPT_MAX_WORK ((uint64_t)1 << 26)

JSValue dyn_crypto_kdf(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv, int magic)
{
    const dyn_hash_algo_t* a;
    uint8_t *ikm = NULL, *salt = NULL, *info = NULL, *out = NULL;
    size_t ikm_len = 0, salt_len = 0, info_len = 0;
    int64_t length = 32, iters = 100000;
    JSValue v, result = JS_EXCEPTION;
    const char* name = NULL;
    int rc;
    (void)this_val;
    (void)argc;

    if (!JS_IsObject(argv[0]))
        return JS_ThrowTypeError(ctx, "expected an options object");
    if (magic) {
        static const char* const pbkdf2_keys[] = { "password", "hash", "salt", "length", "iterations" };
        if (dyn_opts_strict(ctx, argv[0], pbkdf2_keys, 5))
            return JS_EXCEPTION;
    } else {
        static const char* const hkdf_keys[] = { "key", "hash", "salt", "info", "length" };
        if (dyn_opts_strict(ctx, argv[0], hkdf_keys, 5))
            return JS_EXCEPTION;
    }

#define DYN_KDF_BYTES(field, dst, dstlen, required)              \
    do {                                                         \
        const uint8_t* p;                                        \
        size_t n;                                                \
        const char* ow;                                          \
        v = JS_GetPropertyStr(ctx, argv[0], field);              \
        if (JS_IsException(v))                                   \
            goto done;                                           \
        if (JS_IsUndefined(v) || JS_IsNull(v)) {                 \
            JS_FreeValue(ctx, v);                                \
            if (required) {                                      \
                JS_ThrowTypeError(ctx, "%s is required", field); \
                goto done;                                       \
            }                                                    \
        } else {                                                 \
            if (dyn_crypto_data(ctx, v, &p, &n, &ow)) {          \
                JS_FreeValue(ctx, v);                            \
                goto done;                                       \
            }                                                    \
            (dst) = (uint8_t*)malloc(n ? n : 1);                 \
            if (!(dst)) {                                        \
                if (ow)                                          \
                    JS_FreeCString(ctx, ow);                     \
                JS_FreeValue(ctx, v);                            \
                JS_ThrowOutOfMemory(ctx);                        \
                goto done;                                       \
            }                                                    \
            if (n)                                               \
                memcpy((dst), p, n);                             \
            (dstlen) = n;                                        \
            if (ow)                                              \
                JS_FreeCString(ctx, ow);                         \
            JS_FreeValue(ctx, v);                                \
        }                                                        \
    } while (0)

    v = JS_GetPropertyStr(ctx, argv[0], "hash");
    if (JS_IsException(v))
        return JS_EXCEPTION;
    if (JS_IsUndefined(v)) {
        name = NULL;
        a = dyn_hash_algo_by_name("sha256");
    } else {
        name = JS_ToCString(ctx, v);
        if (!name) {
            JS_FreeValue(ctx, v);
            return JS_EXCEPTION;
        }
        a = dyn_hash_algo_by_name(name);
    }
    JS_FreeValue(ctx, v);
    if (!a) {
        JS_ThrowTypeError(ctx, "unknown hash \"%s\"", name ? name : "");
        if (name)
            JS_FreeCString(ctx, name);
        return JS_EXCEPTION;
    }
    if (name)
        JS_FreeCString(ctx, name);

    DYN_KDF_BYTES(magic ? "password" : "key", ikm, ikm_len, 1);
    DYN_KDF_BYTES("salt", salt, salt_len, magic ? 1 : 0);
    if (!magic)
        DYN_KDF_BYTES("info", info, info_len, 0);

    v = JS_GetPropertyStr(ctx, argv[0], "length");
    if (JS_IsException(v))
        goto done;
    if (!JS_IsUndefined(v) && JS_ToInt64(ctx, &length, v)) {
        JS_FreeValue(ctx, v);
        goto done;
    }
    JS_FreeValue(ctx, v);
    if (length < 1 || length > (1 << 20)) {
        JS_ThrowRangeError(ctx, "length must be 1..1048576");
        goto done;
    }
    if (magic) {
        int64_t blocks;
        v = JS_GetPropertyStr(ctx, argv[0], "iterations");
        if (JS_IsException(v))
            goto done;
        if (!JS_IsUndefined(v) && JS_ToInt64(ctx, &iters, v)) {
            JS_FreeValue(ctx, v);
            goto done;
        }
        JS_FreeValue(ctx, v);
        if (iters < 1 || iters > (1 << 26)) {
            JS_ThrowRangeError(ctx, "iterations must be 1..67108864");
            goto done;
        }
        blocks = (length + a->digest_size - 1) / a->digest_size;
        if (iters * blocks > DYN_PBKDF2_MAX_COMPRESSIONS) {
            JS_ThrowRangeError(ctx,
                "PBKDF2: iterations x output-blocks exceeds %d (DoS bound)",
                DYN_PBKDF2_MAX_COMPRESSIONS);
            goto done;
        }
    }

    out = (uint8_t*)malloc((size_t)length);
    if (!out) {
        JS_ThrowOutOfMemory(ctx);
        goto done;
    }
    if (magic && dyn_kdf_begin(ctx, 0, 0) == 0)
        rc = DYN_OFFLOAD_CPU(dyn_pbkdf2(a, ikm, ikm_len, salt, salt_len, (uint32_t)iters,
            out, (size_t)length));
    else
        rc = dyn_hkdf(a, ikm, ikm_len, salt, salt_len, info, info_len,
            out, (size_t)length);
    if (rc) {
        JS_ThrowRangeError(ctx, "length exceeds what this construction can derive");
        goto done;
    }
    result = dyn_nat_u8_copy(ctx, out, (size_t)length);
done:
    if (out) {
        dyn_wipe(out, (size_t)length);
        free(out);
    }
    if (ikm) {
        dyn_wipe(ikm, ikm_len);
        free(ikm);
    }
    free(salt);
    free(info);
    return result;
#undef DYN_KDF_BYTES
}

#ifdef CONFIG_OPENSSL
JSValue dyn_scrypt_js(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    const uint8_t *pwd = NULL, *salt = NULL;
    size_t pwdlen = 0, saltlen = 0;
    const char *pwd_ow = NULL, *salt_ow = NULL;
    int64_t N = 16384, r = 8, p = 1, keyLen = 32;
    uint8_t* out = NULL;
    uint8_t *pwc = NULL, *saltc = NULL;
    JSValue ret = JS_EXCEPTION, v;
    uint32_t cap = 0;
    int sc_rc;
    (void)this_val;

    if (argc < 2)
        return JS_ThrowTypeError(ctx, "Scrypt(password, salt[, opts])");

    if (argc > 2 && JS_IsObject(argv[2])) {
        static const char* const sc_keys[] = { "N", "r", "p", "keyLen", "dyna" };
        if (dyn_opts_strict(ctx, argv[2], sc_keys, 5) || dyn_kdf_meta(ctx, argv[2], "Scrypt", &cap))
            return JS_EXCEPTION;
        v = JS_GetPropertyStr(ctx, argv[2], "N");
        if (JS_IsException(v))
            return JS_EXCEPTION;
        if (!JS_IsUndefined(v) && JS_ToInt64(ctx, &N, v)) {
            JS_FreeValue(ctx, v);
            return JS_EXCEPTION;
        }
        JS_FreeValue(ctx, v);
        v = JS_GetPropertyStr(ctx, argv[2], "r");
        if (JS_IsException(v))
            return JS_EXCEPTION;
        if (!JS_IsUndefined(v) && JS_ToInt64(ctx, &r, v)) {
            JS_FreeValue(ctx, v);
            return JS_EXCEPTION;
        }
        JS_FreeValue(ctx, v);
        v = JS_GetPropertyStr(ctx, argv[2], "p");
        if (JS_IsException(v))
            return JS_EXCEPTION;
        if (!JS_IsUndefined(v) && JS_ToInt64(ctx, &p, v)) {
            JS_FreeValue(ctx, v);
            return JS_EXCEPTION;
        }
        JS_FreeValue(ctx, v);
        v = JS_GetPropertyStr(ctx, argv[2], "keyLen");
        if (JS_IsException(v))
            return JS_EXCEPTION;
        if (!JS_IsUndefined(v) && JS_ToInt64(ctx, &keyLen, v)) {
            JS_FreeValue(ctx, v);
            return JS_EXCEPTION;
        }
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
    if (128ULL * (uint64_t)r * ((uint64_t)N + (uint64_t)p + 2) > (1ULL << 30))
        return JS_ThrowRangeError(ctx, "Scrypt: 128*r*(N+p+2) exceeds memory budget (1 GiB)");
    if ((uint64_t)N * (uint64_t)r * (uint64_t)p > DYN_SCRYPT_MAX_WORK)
        return JS_ThrowRangeError(ctx, "Scrypt: N*r*p exceeds the work budget (%llu)",
            (unsigned long long)DYN_SCRYPT_MAX_WORK);

    if (dyn_crypto_data(ctx, argv[0], &pwd, &pwdlen, &pwd_ow))
        return JS_EXCEPTION;
    if (dyn_crypto_data(ctx, argv[1], &salt, &saltlen, &salt_ow)) {
        if (pwd_ow)
            JS_FreeCString(ctx, pwd_ow);
        return JS_EXCEPTION;
    }

    out = (uint8_t*)malloc((size_t)keyLen);
    if (!out) {
        if (pwd_ow)
            JS_FreeCString(ctx, pwd_ow);
        if (salt_ow)
            JS_FreeCString(ctx, salt_ow);
        return JS_ThrowOutOfMemory(ctx);
    }
    pwc = (uint8_t*)malloc(pwdlen ? pwdlen : 1);
    saltc = (uint8_t*)malloc(saltlen ? saltlen : 1);
    if (!pwc || !saltc) {
        JS_ThrowOutOfMemory(ctx);
        goto done;
    }
    if (pwdlen)
        memcpy(pwc, pwd, pwdlen);
    if (saltlen)
        memcpy(saltc, salt, saltlen);
    if (dyn_kdf_begin(ctx, 1, cap))
        goto done;
    sc_rc = DYN_OFFLOAD_CPU(EVP_PBE_scrypt((const char*)pwc, pwdlen, saltc, saltlen,
        (uint64_t)N, (uint64_t)r, (uint64_t)p,
        1ULL << 30, out, (size_t)keyLen));
    dyn_kdf_end(ctx, 1);
    if (sc_rc != 1) {
        JS_ThrowInternalError(ctx, "Scrypt: derivation failed");
        goto done;
    }
    ret = dyn_nat_u8_copy(ctx, out, (size_t)keyLen);

done:
    if (out) {
        dyn_wipe(out, (size_t)keyLen);
        free(out);
    }
    if (pwc) {
        dyn_wipe(pwc, pwdlen);
        free(pwc);
    }
    free(saltc);
    if (pwd_ow)
        JS_FreeCString(ctx, pwd_ow);
    if (salt_ow)
        JS_FreeCString(ctx, salt_ow);
    return ret;
}
#endif

#endif
