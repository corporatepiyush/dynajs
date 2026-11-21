#include "crypto-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_CRYPTO)

typedef struct {
    dyn_hmac_ctx_t h;
} dyn_hmac_res_t;

static inline int dyn_hex_val(uint8_t ch)
{
    unsigned d = (unsigned)ch - '0';
    if (d < 10)
        return (int)d;
    d = (unsigned)(ch | 0x20) - 'a';
    return d < 6 ? (int)(d + 10) : -1;
}

JSValue dyn_crypto_hmac(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv, int magic)
{
    const char* aname;
    const dyn_hash_algo_t* a;
    const uint8_t *kp, *dp;
    size_t kl, dl;
    const char *kown, *down;
    uint8_t k0[DYN_HASH_MAX_BLOCK], out[DYN_HASH_MAX_DIGEST];
    (void)this_val;
    (void)argc;

    aname = JS_ToCString(ctx, argv[0]);
    if (!aname)
        return JS_EXCEPTION;
    a = dyn_hash_algo_by_name(aname);
    JS_FreeCString(ctx, aname);
    if (!a)
        return JS_ThrowTypeError(ctx, "hmac: unknown algorithm");

    if (dyn_crypto_secret(ctx, argv[1], &kp, &kl, &kown))
        return JS_EXCEPTION;
    memset(k0, 0, sizeof(k0));
    dyn_hmac_key0(a, kp, kl, k0);
    if (kown)
        JS_FreeCString(ctx, kown);

    if (dyn_crypto_data(ctx, argv[2], &dp, &dl, &down)) {
        dyn_wipe(k0, sizeof(k0));
        return JS_EXCEPTION;
    }
    dyn_hmac_finish(a, k0, dp, dl, out);
    if (down)
        JS_FreeCString(ctx, down);
    dyn_wipe(k0, sizeof(k0));

    return magic ? dyn_crypto_hex(ctx, out, a->digest_size)
                 : dyn_nat_u8_copy(ctx, out, a->digest_size);
}

static void dyn_hmac_dispose(void* native)
{
    dyn_hmac_res_t* m = (dyn_hmac_res_t*)native;
    if (!m)
        return;
    dyn_wipe(m, sizeof(*m));
    free(m);
}

JSValue dyn_hmac_ctor(JSContext* ctx, JSValueConst new_target, int argc,
    JSValueConst* argv)
{
    const dyn_hash_algo_t* a;
    const char* name;
    const uint8_t* key;
    size_t keylen;
    const char* owned;
    uint8_t* keycopy;
    dyn_hmac_res_t* m;

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
    if (dyn_crypto_secret(ctx, argv[1], &key, &keylen, &owned))
        return JS_EXCEPTION;
    keycopy = (uint8_t*)malloc(keylen ? keylen : 1);
    if (!keycopy) {
        if (owned)
            JS_FreeCString(ctx, owned);
        return JS_ThrowOutOfMemory(ctx);
    }
    if (keylen)
        memcpy(keycopy, key, keylen);
    if (owned)
        JS_FreeCString(ctx, owned);
    m = (dyn_hmac_res_t*)calloc(1, sizeof(*m));
    if (!m) {
        dyn_wipe(keycopy, keylen ? keylen : 1);
        free(keycopy);
        return JS_ThrowOutOfMemory(ctx);
    }
    dyn_hmac_init(&m->h, a, keycopy, keylen);
    dyn_wipe(keycopy, keylen ? keylen : 1);
    free(keycopy);
    return dyn_res_wrap(ctx, new_target, dyn_hmac_class_id, m, dyn_hmac_dispose);
}

JSValue dyn_hmac_sign(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv, int magic)
{
    dyn_hmac_res_t* m;
    const uint8_t* data;
    size_t len;
    const char* owned;
    uint8_t mac[DYN_HASH_MAX_DIGEST];
    size_t dsize;
    (void)argc;

    if (dyn_crypto_data(ctx, argv[0], &data, &len, &owned))
        return JS_EXCEPTION;
    m = (dyn_hmac_res_t*)dyn_res_native(ctx, this_val, dyn_hmac_class_id);
    if (!m) {
        if (owned)
            JS_FreeCString(ctx, owned);
        return JS_EXCEPTION;
    }
    dsize = m->h.algo->digest_size;
    dyn_hmac_reset(&m->h);
    dyn_hmac_update(&m->h, data, len);
    dyn_hmac_final(&m->h, mac);
    if (owned)
        JS_FreeCString(ctx, owned);
    return magic ? dyn_crypto_hex(ctx, mac, dsize)
                 : dyn_nat_u8_copy(ctx, mac, dsize);
}

JSValue dyn_hmac_update_js(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_hmac_res_t* m;
    const uint8_t* data;
    size_t len;
    const char* owned;
    (void)argc;

    if (dyn_crypto_data(ctx, argv[0], &data, &len, &owned))
        return JS_EXCEPTION;
    m = (dyn_hmac_res_t*)dyn_res_native(ctx, this_val, dyn_hmac_class_id);
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

JSValue dyn_hmac_digest(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv, int magic)
{
    dyn_hmac_res_t* m = (dyn_hmac_res_t*)dyn_res_native(ctx, this_val,
        dyn_hmac_class_id);
    uint8_t mac[DYN_HASH_MAX_DIGEST];
    size_t dsize;
    (void)argc;
    (void)argv;

    if (!m)
        return JS_EXCEPTION;
    dsize = m->h.algo->digest_size;
    dyn_hmac_final(&m->h, mac);
    return magic ? dyn_crypto_hex(ctx, mac, dsize)
                 : dyn_nat_u8_copy(ctx, mac, dsize);
}

JSValue dyn_hmac_reset_js(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_hmac_res_t* m;
    (void)argc;
    (void)argv;

    m = (dyn_hmac_res_t*)dyn_res_native(ctx, this_val, dyn_hmac_class_id);
    if (!m)
        return JS_EXCEPTION;
    dyn_hmac_reset(&m->h);
    return JS_DupValue(ctx, this_val);
}

JSValue dyn_hmac_verify(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv)
{
    dyn_hmac_res_t* m;
    const uint8_t *data, *tag;
    size_t len, taglen, dsize;
    const char *owned, *towned;
    uint8_t* tagcopy;
    uint8_t mac[DYN_HASH_MAX_DIGEST];
    int r;
    (void)argc;

    if (dyn_crypto_data(ctx, argv[1], &tag, &taglen, &towned))
        return JS_EXCEPTION;
    tagcopy = (uint8_t*)malloc(taglen ? taglen : 1);
    if (!tagcopy) {
        if (towned)
            JS_FreeCString(ctx, towned);
        return JS_ThrowOutOfMemory(ctx);
    }
    if (taglen)
        memcpy(tagcopy, tag, taglen);
    if (towned)
        JS_FreeCString(ctx, towned);
    if (dyn_crypto_data(ctx, argv[0], &data, &len, &owned)) {
        free(tagcopy);
        return JS_EXCEPTION;
    }
    m = (dyn_hmac_res_t*)dyn_res_native(ctx, this_val, dyn_hmac_class_id);
    if (!m) {
        if (owned)
            JS_FreeCString(ctx, owned);
        free(tagcopy);
        return JS_EXCEPTION;
    }
    dsize = m->h.algo->digest_size;
    dyn_hmac_reset(&m->h);
    dyn_hmac_update(&m->h, data, len);
    dyn_hmac_final(&m->h, mac);
    if (owned)
        JS_FreeCString(ctx, owned);
    if (taglen == dsize) {
        r = dyn_ct_equal(mac, tagcopy, dsize);
    } else if (taglen == dsize * 2) {
        uint8_t dec[DYN_HASH_MAX_DIGEST];
        size_t i;
        int ok = 1;
        for (i = 0; i < dsize; i++) {
            int hi = dyn_hex_val(tagcopy[i * 2]);
            int lo = dyn_hex_val(tagcopy[i * 2 + 1]);
            if (hi < 0 || lo < 0) {
                ok = 0;
                break;
            }
            dec[i] = (uint8_t)((hi << 4) | lo);
        }
        r = ok && dyn_ct_equal(mac, dec, dsize);
    } else {
        r = 0;
    }
    free(tagcopy);
    return JS_NewBool(ctx, r);
}

JSValue dyn_hmac_get_algorithm(JSContext* ctx, JSValueConst this_val)
{
    dyn_hmac_res_t* m = (dyn_hmac_res_t*)dyn_res_native(ctx, this_val,
        dyn_hmac_class_id);
    if (!m)
        return JS_EXCEPTION;
    return JS_NewString(ctx, m->h.algo->name);
}

JSValue dyn_hmac_get_size(JSContext* ctx, JSValueConst this_val)
{
    dyn_hmac_res_t* m = (dyn_hmac_res_t*)dyn_res_native(ctx, this_val,
        dyn_hmac_class_id);
    if (!m)
        return JS_EXCEPTION;
    return JS_NewInt32(ctx, (int32_t)m->h.algo->digest_size);
}

#endif
