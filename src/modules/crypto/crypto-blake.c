#include "crypto-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_CRYPTO)

static int blake_run(int kind, int derive, const uint8_t* key, size_t keylen,
    const uint8_t* ctxb, size_t ctxn, const uint8_t* data, size_t n,
    uint32_t seed, uint8_t* out, size_t out_len)
{
    switch (kind) {
    case 0: {
        bl3_hasher_t h;
        if (derive) {
            uint8_t ck[32];
            bl3_init_flags(&h, BL3_DERIVE_CTX);
            bl3_update(&h, ctxb, ctxn);
            bl3_final(&h, ck, sizeof ck);
            bl3_init_keyed(&h, ck, BL3_DERIVE_MAT);
            bl3_update(&h, data, n);
            bl3_final(&h, out, out_len);
        } else if (key) {
            bl3_init_keyed(&h, key, BL3_KEYED);
            bl3_update(&h, data, n);
            bl3_final(&h, out, out_len);
        } else {
            bl3_init(&h);
            bl3_update(&h, data, n);
            bl3_final(&h, out, out_len);
        }
        break;
    }
    case 1:
        bl2b_hash_key(data, n, out, out_len, key, keylen);
        break;
    case 2:
        bl2s_hash_key(data, n, out, out_len, key, keylen);
        break;
    default:
        mm3_128(data, n, seed, out);
        break;
    }
    return 0;
}

JSValue dyn_blake(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    static const struct {
        size_t def, max;
        const char* name;
    } K[] = {
        { 32, 1u << 20, "BLAKE3" },
        { 64, 64, "BLAKE2b" },
        { 32, 32, "BLAKE2s" },
        { 16, 16, "Murmur3_128" },
    };
    int kind = magic & 7, hex = (magic & 8) != 0;
    const uint8_t* data;
    const char* owned;
    size_t n, out_len = K[kind].def;
    uint32_t seed = 0;
    uint8_t* out;
    JSValue result;
    const uint8_t *key = NULL, *ctxb = NULL;
    const char *key_own = NULL, *ctx_own = NULL;
    const char* ctx_opt = NULL;
    size_t keylen = 0, ctxn = 0;
    int derive = 0;
    uint8_t* hold;
    int live;
    uint8_t keyc[64];

    (void)this_val;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "%s(data): data is required", K[kind].name);
    if (argc > 2)
        return JS_ThrowTypeError(ctx,
            "%s(data, length|opts): at most two arguments", K[kind].name);
    if (argc > 1 && JS_IsObject(argv[1])) {
        JSValue v;
        if (kind == 3)
            return JS_ThrowTypeError(ctx,
                "Murmur3_128(data, seed): the second argument is the seed, a "
                "number -- it takes no options object");
        if (kind == 0) {
            static const char* const b3_keys[] = { "key", "length", "context", "deriveKey" };
            if (dyn_opts_strict(ctx, argv[1], b3_keys, 4))
                return JS_EXCEPTION;
        } else {
            static const char* const bl2_keys[] = { "key", "length" };
            if (dyn_opts_strict(ctx, argv[1], bl2_keys, 2))
                return JS_EXCEPTION;
        }
        v = JS_GetPropertyStr(ctx, argv[1], "length");
        if (JS_IsException(v))
            return JS_EXCEPTION;
        if (!JS_IsUndefined(v) && !JS_IsNull(v)) {
            int64_t lv;
            if (JS_ToInt64(ctx, &lv, v)) {
                JS_FreeValue(ctx, v);
                return JS_EXCEPTION;
            }
            if (lv < 1 || (size_t)lv > K[kind].max) {
                JS_FreeValue(ctx, v);
                return JS_ThrowRangeError(ctx, "%s(data, length): length is 1 "
                                               "to %zu bytes",
                    K[kind].name, K[kind].max);
            }
            out_len = (size_t)lv;
        }
        JS_FreeValue(ctx, v);
        if (kind == 0) {
            int have_ctx = 0;
            v = JS_GetPropertyStr(ctx, argv[1], "key");
            if (JS_IsException(v))
                return JS_EXCEPTION;
            if (!JS_IsUndefined(v) && !JS_IsNull(v)) {
                if (dyn_crypto_secret(ctx, v, &key, &keylen, &key_own)) {
                    JS_FreeValue(ctx, v);
                    return JS_EXCEPTION;
                }
                if (keylen != 32) {
                    JS_FreeValue(ctx, v);
                    if (key_own)
                        JS_FreeCString(ctx, key_own);
                    return JS_ThrowRangeError(ctx,
                        "BLAKE3: key must be exactly 32 bytes (got %zu) -- "
                        "the keyed mode keys with the full 32, unlike "
                        "BLAKE2's variable-length key",
                        keylen);
                }
            }
            JS_FreeValue(ctx, v);
            v = JS_GetPropertyStr(ctx, argv[1], "context");
            if (JS_IsException(v)) {
                if (key_own)
                    JS_FreeCString(ctx, key_own);
                return JS_EXCEPTION;
            }
            if (!JS_IsUndefined(v) && !JS_IsNull(v)) {
                size_t cs_n = 0;
                const char* cs = JS_ToCStringLen(ctx, &cs_n, v);
                JS_FreeValue(ctx, v);
                if (!cs) {
                    if (key_own)
                        JS_FreeCString(ctx, key_own);
                    return JS_EXCEPTION;
                }
                if (!cs_n) {
                    JS_FreeCString(ctx, cs);
                    if (key_own)
                        JS_FreeCString(ctx, key_own);
                    return JS_ThrowRangeError(ctx,
                        "BLAKE3: context must be a non-empty string -- it is "
                        "domain separation, and an empty one separates "
                        "nothing");
                }
                ctx_own = cs;
                ctx_opt = "context";
                bl3_ctx_adopt(cs, cs_n, &ctxb, &ctxn);
                have_ctx = 1;
            } else {
                JS_FreeValue(ctx, v);
            }
            v = JS_GetPropertyStr(ctx, argv[1], "deriveKey");
            if (JS_IsException(v)) {
                if (key_own)
                    JS_FreeCString(ctx, key_own);
                if (ctx_own)
                    JS_FreeCString(ctx, ctx_own);
                return JS_EXCEPTION;
            }
            if (!JS_IsUndefined(v) && !JS_IsNull(v)) {
                const char* cs;
                size_t cs_n = 0;
                if (have_ctx) {
                    JS_FreeValue(ctx, v);
                    if (key_own)
                        JS_FreeCString(ctx, key_own);
                    if (ctx_own)
                        JS_FreeCString(ctx, ctx_own);
                    return JS_ThrowTypeError(ctx,
                        "BLAKE3: use either context or deriveKey -- they are "
                        "the same option, spelled twice");
                }
                cs = JS_ToCStringLen(ctx, &cs_n, v);
                JS_FreeValue(ctx, v);
                if (!cs) {
                    if (key_own)
                        JS_FreeCString(ctx, key_own);
                    return JS_EXCEPTION;
                }
                if (!cs_n) {
                    JS_FreeCString(ctx, cs);
                    if (key_own)
                        JS_FreeCString(ctx, key_own);
                    return JS_ThrowRangeError(ctx,
                        "BLAKE3: context must be a non-empty string -- it is "
                        "domain separation, and an empty one separates "
                        "nothing");
                }
                ctx_own = cs;
                ctx_opt = "deriveKey";
                bl3_ctx_adopt(cs, cs_n, &ctxb, &ctxn);
                have_ctx = 1;
            } else {
                JS_FreeValue(ctx, v);
            }
            if (have_ctx) {
                if (key) {
                    const char* opt = ctx_opt ? ctx_opt : "context";
                    if (key_own)
                        JS_FreeCString(ctx, key_own);
                    if (ctx_own)
                        JS_FreeCString(ctx, ctx_own);
                    return JS_ThrowTypeError(ctx,
                        "BLAKE3: {key} and {%s} are different modes -- keyed "
                        "MAC, or derive-key; a call is one or the other",
                        opt);
                }
                derive = 1;
                out_len = 32;
            }
        } else {
            v = JS_GetPropertyStr(ctx, argv[1], "key");
            if (JS_IsException(v))
                return JS_EXCEPTION;
            if (!JS_IsUndefined(v) && !JS_IsNull(v)) {
                if (dyn_crypto_secret(ctx, v, &key, &keylen, &key_own)) {
                    JS_FreeValue(ctx, v);
                    return JS_EXCEPTION;
                }
                if (keylen > K[kind].max) {
                    JS_FreeValue(ctx, v);
                    if (key_own)
                        JS_FreeCString(ctx, key_own);
                    return JS_ThrowRangeError(ctx,
                        "%s: key must be 0 to %zu bytes (got %zu) -- 0 means "
                        "unkeyed",
                        K[kind].name, K[kind].max, keylen);
                }
            }
            JS_FreeValue(ctx, v);
        }
        if (derive && argc > 0 && JS_IsObject(argv[1])) {
            v = JS_GetPropertyStr(ctx, argv[1], "length");
            if (JS_IsException(v)) {
                if (key_own)
                    JS_FreeCString(ctx, key_own);
                if (ctx_own)
                    JS_FreeCString(ctx, ctx_own);
                return JS_EXCEPTION;
            }
            if (!JS_IsUndefined(v) && !JS_IsNull(v)) {
                JS_FreeValue(ctx, v);
                if (key_own)
                    JS_FreeCString(ctx, key_own);
                if (ctx_own)
                    JS_FreeCString(ctx, ctx_own);
                return JS_ThrowTypeError(ctx,
                    "BLAKE3: derive-key output is exactly 32 bytes -- it "
                    "keys the next primitive, and takes no length");
            }
            JS_FreeValue(ctx, v);
        }
    } else if (argc > 1 && !JS_IsUndefined(argv[1])) {
        int64_t v;
        if (JS_ToInt64(ctx, &v, argv[1]) < 0)
            return JS_EXCEPTION;
        if (kind == 3) {
            seed = (uint32_t)v;
        } else if (v < 1 || (size_t)v > K[kind].max) {
            return JS_ThrowRangeError(ctx, "%s(data, length): length is 1 to %zu "
                                           "bytes",
                K[kind].name, K[kind].max);
        } else {
            out_len = (size_t)v;
        }
    }
    if (dyn_crypto_data(ctx, argv[0], &data, &n, &owned) < 0) {
        if (key_own)
            JS_FreeCString(ctx, key_own);
        if (ctx_own)
            JS_FreeCString(ctx, ctx_own);
        return JS_EXCEPTION;
    }
    out = (uint8_t*)malloc(out_len);
    if (!out) {
        if (owned)
            JS_FreeCString(ctx, owned);
        if (key_own)
            JS_FreeCString(ctx, key_own);
        if (ctx_own)
            JS_FreeCString(ctx, ctx_own);
        return JS_ThrowOutOfMemory(ctx);
    }
    if (dyn_crypto_private(&live, &data, n, owned, &hold) < 0) {
        free(out);
        if (key_own)
            JS_FreeCString(ctx, key_own);
        if (ctx_own)
            JS_FreeCString(ctx, ctx_own);
        return JS_ThrowOutOfMemory(ctx);
    }
    if (__builtin_expect(live, 0) && key && !key_own) {
        memcpy(keyc, key, keylen);
        key = keyc;
    }
    (void)DYN_OFFLOAD_CPU_IF(live, blake_run(kind, derive, key, keylen, ctxb, ctxn, data, n, seed, out, out_len));
    if (hold)
        free(hold);
    if (key == keyc)
        dyn_wipe(keyc, sizeof keyc);
    if (key_own)
        JS_FreeCString(ctx, key_own);
    if (ctx_own)
        JS_FreeCString(ctx, ctx_own);
    if (owned)
        JS_FreeCString(ctx, owned);
    if (hex) {
        char* h = (char*)malloc(out_len * 2 + 1);
        size_t i;
        if (!h) {
            free(out);
            return JS_ThrowOutOfMemory(ctx);
        }
        for (i = 0; i < out_len; i++) {
            h[i * 2] = "0123456789abcdef"[out[i] >> 4];
            h[i * 2 + 1] = "0123456789abcdef"[out[i] & 15];
        }
        result = JS_NewStringLen(ctx, h, out_len * 2);
        free(h);
    } else {
        result = dyn_nat_u8_copy(ctx, out, out_len);
    }
    free(out);
    return result;
}

#endif
