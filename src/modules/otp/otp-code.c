#include "otp-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_OTP)

static const uint32_t OTP_POW10[9] = {
    1, 10, 100, 1000, 10000, 100000, 1000000, 10000000, 100000000
};

static const char* const hotp_keys[] = { "digits", "algo" };
static const char* const totp_keys[] = { "atSec", "period", "digits", "algo" };
static const char* const totp_verify_keys[] = { "atSec", "period", "digits", "algo", "window" };

static uint32_t otp_truncate(const uint8_t* mac, size_t maclen, int digits)
{
    size_t off = mac[maclen - 1] & 0x0f;
    uint32_t bin = ((uint32_t)(mac[off] & 0x7f) << 24) | ((uint32_t)mac[off + 1] << 16) | ((uint32_t)mac[off + 2] << 8) | ((uint32_t)mac[off + 3]);
    return bin % OTP_POW10[digits];
}

static int otp_algo_allowed(const char* name)
{
    return !strcmp(name, "sha1") || !strcmp(name, "sha256") || !strcmp(name, "sha512");
}

int dyn_otp_compute(const char* algo, const uint8_t* key, size_t keylen,
    uint64_t counter, int digits, char* out, size_t outsz)
{
    const char* name = algo ? algo : "sha1";
    const dyn_hash_algo_t* a = dyn_hash_algo_by_name(name);
    uint8_t msg[8], mac[DYN_HASH_MAX_DIGEST];
    int i;

    if (!a || digits < 1 || digits > 8 || !otp_algo_allowed(name))
        return -1;
    memset(mac, 0, sizeof mac);
    for (i = 7; i >= 0; i--) {
        msg[i] = (uint8_t)(counter & 0xff);
        counter >>= 8;
    }
    dyn_hmac(a, key, keylen, msg, sizeof(msg), mac);
    snprintf(out, outsz, "%0*u", digits, otp_truncate(mac, a->digest_size, digits));
    dyn_wipe(mac, sizeof mac);
    return 0;
}

JSValue dyn_otp_hotp(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv)
{
    const uint8_t* key;
    size_t keylen;
    const char* kown;
    int64_t counter = 0;
    int32_t digits = 6;
    const char* algo = NULL;
    char out[16];
    JSValue av = JS_UNDEFINED, dv = JS_UNDEFINED, r;
    (void)this_val;

    if (argc < 2)
        return JS_ThrowTypeError(ctx, "HOTPGenerate(secret, counter[, opts])");
    if (JS_ToInt64Ext(ctx, &counter, argv[1]) < 0)
        return JS_EXCEPTION;
    if (counter < 0)
        return JS_ThrowRangeError(ctx, "counter must not be negative");
    if (argc > 2 && JS_IsObject(argv[2])) {
        if (dyn_opts_strict(ctx, argv[2], hotp_keys, countof(hotp_keys)) < 0)
            return JS_EXCEPTION;
        dv = JS_GetPropertyStr(ctx, argv[2], "digits");
        if (JS_IsException(dv)) {
            JS_FreeValue(ctx, dv);
            return JS_EXCEPTION;
        }
        if (!JS_IsUndefined(dv) && JS_ToInt32(ctx, &digits, dv)) {
            JS_FreeValue(ctx, dv);
            return JS_EXCEPTION;
        }
        av = JS_GetPropertyStr(ctx, argv[2], "algo");
        if (JS_IsException(av)) {
            JS_FreeValue(ctx, av);
            JS_FreeValue(ctx, dv);
            return JS_EXCEPTION;
        }
        if (!JS_IsUndefined(av)) {
            algo = JS_ToCString(ctx, av);
            if (!algo) {
                JS_FreeValue(ctx, av);
                JS_FreeValue(ctx, dv);
                return JS_EXCEPTION;
            }
        }
    }
    if (dyn_otp_secret(ctx, argv[0], &key, &keylen, &kown)) {
        if (algo)
            JS_FreeCString(ctx, algo);
        JS_FreeValue(ctx, av);
        JS_FreeValue(ctx, dv);
        return JS_EXCEPTION;
    }
    if (digits < 6 || digits > 8) {
        if (algo)
            JS_FreeCString(ctx, algo);
        if (kown)
            JS_FreeCString(ctx, kown);
        JS_FreeValue(ctx, av);
        JS_FreeValue(ctx, dv);
        return JS_ThrowRangeError(ctx, "digits must be 6..8");
    }
    if (dyn_otp_compute(algo, key, keylen, (uint64_t)counter, digits,
            out, sizeof(out))
        < 0) {
        if (algo)
            JS_FreeCString(ctx, algo);
        if (kown)
            JS_FreeCString(ctx, kown);
        JS_FreeValue(ctx, av);
        JS_FreeValue(ctx, dv);
        return JS_ThrowTypeError(ctx,
            "HOTPGenerate: algo must be sha1, sha256 or sha512 (RFC 4226)");
    }
    if (algo)
        JS_FreeCString(ctx, algo);
    if (kown)
        JS_FreeCString(ctx, kown);
    JS_FreeValue(ctx, av);
    JS_FreeValue(ctx, dv);
    r = JS_NewString(ctx, out);
    return r;
}

JSValue dyn_otp_totp(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv)
{
    const uint8_t* key;
    size_t keylen;
    const char* kown;
    int64_t at = (int64_t)time(NULL), period = 30;
    int32_t digits = 6;
    const char* algo = NULL;
    char out[16];
    JSValue v;
    (void)this_val;

    if (argc < 1)
        return JS_ThrowTypeError(ctx, "TOTPGenerate(secret[, opts])");
    if (argc > 1 && JS_IsObject(argv[1])) {
        if (dyn_opts_strict(ctx, argv[1], totp_keys, countof(totp_keys)) < 0)
            return JS_EXCEPTION;
        v = JS_GetPropertyStr(ctx, argv[1], "atSec");
        if (JS_IsException(v))
            goto optfail;
        if (!JS_IsUndefined(v) && JS_ToInt64(ctx, &at, v)) {
            JS_FreeValue(ctx, v);
            return JS_EXCEPTION;
        }
        JS_FreeValue(ctx, v);
        v = JS_GetPropertyStr(ctx, argv[1], "period");
        if (JS_IsException(v))
            goto optfail;
        if (!JS_IsUndefined(v) && JS_ToInt64(ctx, &period, v)) {
            JS_FreeValue(ctx, v);
            return JS_EXCEPTION;
        }
        JS_FreeValue(ctx, v);
        v = JS_GetPropertyStr(ctx, argv[1], "digits");
        if (JS_IsException(v))
            goto optfail;
        if (!JS_IsUndefined(v) && JS_ToInt32(ctx, &digits, v)) {
            JS_FreeValue(ctx, v);
            return JS_EXCEPTION;
        }
        JS_FreeValue(ctx, v);
        v = JS_GetPropertyStr(ctx, argv[1], "algo");
        if (JS_IsException(v))
            goto optfail;
        if (!JS_IsUndefined(v)) {
            algo = JS_ToCString(ctx, v);
            JS_FreeValue(ctx, v);
            if (!algo)
                return JS_EXCEPTION;
        }
    }
    if (period <= 0 || digits < 6 || digits > 8 || at < 0) {
        if (algo)
            JS_FreeCString(ctx, algo);
        return JS_ThrowRangeError(ctx, "period > 0, digits 6..8, atSec >= 0");
    }
    if (dyn_otp_secret(ctx, argv[0], &key, &keylen, &kown)) {
        if (algo)
            JS_FreeCString(ctx, algo);
        return JS_EXCEPTION;
    }
    if (dyn_otp_compute(algo, key, keylen, (uint64_t)(at / period), digits,
            out, sizeof(out))
        < 0) {
        if (algo)
            JS_FreeCString(ctx, algo);
        if (kown)
            JS_FreeCString(ctx, kown);
        return JS_ThrowTypeError(ctx,
            "TOTPGenerate: algo must be sha1, sha256 or sha512 (RFC 6238)");
    }
    if (algo)
        JS_FreeCString(ctx, algo);
    if (kown)
        JS_FreeCString(ctx, kown);
    return JS_NewString(ctx, out);
optfail:
    JS_FreeValue(ctx, v);
    if (algo)
        JS_FreeCString(ctx, algo);
    return JS_EXCEPTION;
}

int dyn_otp_wellformed(const char* code, size_t codelen, int digits)
{
    int i;
    if (codelen != (size_t)digits)
        return 0;
    for (i = 0; i < digits; i++)
        if (code[i] < '0' || code[i] > '9')
            return 0;
    return 1;
}

static int otp_verify_opts(JSContext* ctx, JSValueConst obj, int32_t* pdigits,
    int64_t* pat, int64_t* pperiod, int64_t* pwindow,
    const char** palgo, int read_window, int read_at)
{
    JSValue v;

    if (read_at) {
        v = JS_GetPropertyStr(ctx, obj, "atSec");
        if (JS_IsException(v))
            return -1;
        if (!JS_IsUndefined(v) && JS_ToInt64(ctx, pat, v)) {
            JS_FreeValue(ctx, v);
            return -1;
        }
        JS_FreeValue(ctx, v);
    }
    if (pperiod) {
        v = JS_GetPropertyStr(ctx, obj, "period");
        if (JS_IsException(v))
            return -1;
        if (!JS_IsUndefined(v) && JS_ToInt64(ctx, pperiod, v)) {
            JS_FreeValue(ctx, v);
            return -1;
        }
        JS_FreeValue(ctx, v);
    }
    v = JS_GetPropertyStr(ctx, obj, "digits");
    if (JS_IsException(v))
        return -1;
    if (!JS_IsUndefined(v) && JS_ToInt32(ctx, pdigits, v)) {
        JS_FreeValue(ctx, v);
        return -1;
    }
    JS_FreeValue(ctx, v);
    if (read_window) {
        v = JS_GetPropertyStr(ctx, obj, "window");
        if (JS_IsException(v))
            return -1;
        if (!JS_IsUndefined(v) && JS_ToInt64(ctx, pwindow, v)) {
            JS_FreeValue(ctx, v);
            return -1;
        }
        JS_FreeValue(ctx, v);
    }
    v = JS_GetPropertyStr(ctx, obj, "algo");
    if (JS_IsException(v))
        return -1;
    if (!JS_IsUndefined(v)) {
        *palgo = JS_ToCString(ctx, v);
        JS_FreeValue(ctx, v);
        if (!*palgo)
            return -1;
    }
    return 0;
}

JSValue dyn_otp_hotp_verify(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    const uint8_t* key;
    size_t keylen;
    const char *kown = NULL, *algo = NULL, *code;
    size_t codelen;
    const char* cown = NULL;
    int64_t counter = 0;
    int32_t digits = 6;
    char out[16];
    int ok;
    (void)this_val;

    if (argc < 3)
        return JS_ThrowTypeError(ctx, "HOTPVerify(secret, counter, code[, opts])");
    if (JS_ToInt64Ext(ctx, &counter, argv[1]) < 0)
        return JS_EXCEPTION;
    if (counter < 0)
        return JS_ThrowRangeError(ctx, "counter must not be negative");
    if (argc > 3 && JS_IsObject(argv[3])) {
        if (dyn_opts_strict(ctx, argv[3], hotp_keys, countof(hotp_keys)) < 0)
            return JS_EXCEPTION;
        if (otp_verify_opts(ctx, argv[3], &digits, NULL, NULL, NULL,
                &algo, 0, 0))
            return JS_EXCEPTION;
    }
    code = JS_ToCStringLen(ctx, &codelen, argv[2]);
    if (!code) {
        if (algo)
            JS_FreeCString(ctx, algo);
        return JS_EXCEPTION;
    }
    cown = code;
    if (digits < 6 || digits > 8) {
        JS_FreeCString(ctx, cown);
        if (algo)
            JS_FreeCString(ctx, algo);
        return JS_ThrowRangeError(ctx, "digits must be 6..8");
    }
    ok = dyn_otp_wellformed(code, codelen, (int)digits);
    if (dyn_otp_secret(ctx, argv[0], &key, &keylen, &kown)) {
        JS_FreeCString(ctx, cown);
        if (algo)
            JS_FreeCString(ctx, algo);
        return JS_EXCEPTION;
    }
    if (ok && dyn_otp_compute(algo, key, keylen, (uint64_t)counter, (int)digits, out, sizeof(out)) < 0) {
        JS_FreeCString(ctx, cown);
        JS_FreeCString(ctx, kown);
        if (algo)
            JS_FreeCString(ctx, algo);
        return JS_ThrowTypeError(ctx,
            "HOTPVerify: algo must be sha1, sha256 or sha512 (RFC 4226)");
    }
    if (kown)
        JS_FreeCString(ctx, kown);
    ok = ok && dyn_ct_equal((const uint8_t*)out, (const uint8_t*)code, (size_t)digits) == 1;
    JS_FreeCString(ctx, cown);
    if (algo)
        JS_FreeCString(ctx, algo);
    return JS_NewBool(ctx, ok);
}

JSValue dyn_otp_totp_verify(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    const uint8_t* key;
    size_t keylen;
    const char *kown = NULL, *algo = NULL, *code;
    size_t codelen;
    const char* cown = NULL;
    int64_t at = (int64_t)time(NULL), period = 30, window = 1;
    int32_t digits = 6;
    char out[16];
    int ok = 0, wellformed, algo_bad = 0;
    int64_t off;
    (void)this_val;

    if (argc < 2)
        return JS_ThrowTypeError(ctx, "TOTPVerify(secret, code[, opts])");
    if (argc > 2 && JS_IsObject(argv[2])) {
        if (dyn_opts_strict(ctx, argv[2], totp_verify_keys, countof(totp_verify_keys)) < 0)
            return JS_EXCEPTION;
        if (otp_verify_opts(ctx, argv[2], &digits, &at, &period, &window,
                &algo, 1, 1))
            return JS_EXCEPTION;
    }
    if (period <= 0 || digits < 6 || digits > 8 || at < 0 || window < 0) {
        if (algo)
            JS_FreeCString(ctx, algo);
        return JS_ThrowRangeError(ctx,
            "period > 0, digits 6..8, atSec >= 0, window >= 0");
    }
    if (window > 4096) {
        if (algo)
            JS_FreeCString(ctx, algo);
        return JS_ThrowRangeError(ctx, "window is at most 4096 periods");
    }
    code = JS_ToCStringLen(ctx, &codelen, argv[1]);
    if (!code) {
        if (algo)
            JS_FreeCString(ctx, algo);
        return JS_EXCEPTION;
    }
    cown = code;
    if (dyn_otp_secret(ctx, argv[0], &key, &keylen, &kown)) {
        JS_FreeCString(ctx, cown);
        if (algo)
            JS_FreeCString(ctx, algo);
        return JS_EXCEPTION;
    }
    wellformed = dyn_otp_wellformed(code, codelen, (int)digits);
    if (wellformed) {
        int64_t base = at / period;
        for (off = -(int64_t)window; off <= (int64_t)window; off++) {
            int64_t c = base + off;
            if (c < 0)
                continue;
            if (dyn_otp_compute(algo, key, keylen, (uint64_t)c, (int)digits,
                    out, sizeof(out))
                < 0) {
                algo_bad = 1;
                break;
            }
            ok |= dyn_ct_equal((const uint8_t*)out, (const uint8_t*)code,
                      (size_t)digits)
                == 1;
        }
    }
    if (kown)
        JS_FreeCString(ctx, kown);
    JS_FreeCString(ctx, cown);
    if (algo_bad) {
        if (algo)
            JS_FreeCString(ctx, algo);
        return JS_ThrowTypeError(ctx,
            "TOTPVerify: algo must be sha1, sha256 or sha512 (RFC 6238)");
    }
    if (algo)
        JS_FreeCString(ctx, algo);
    return JS_NewBool(ctx, ok);
}

#endif
