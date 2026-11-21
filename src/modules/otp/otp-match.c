#include "otp-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_OTP)

static const char* const hotp_match_keys[] = { "digits", "algo", "lookAhead" };
static const char* const totp_match_keys[] = { "atSec", "period", "digits", "algo", "window", "drift", "lastCounter" };
static const char* const throttle_keys[] = { "atSec", "stepSec", "lockoutAfter" };

int dyn_otp_secret(JSContext* ctx, JSValueConst v, const uint8_t** pdata,
    size_t* plen, const char** powned)
{
    size_t off, len, bpe, n;
    uint8_t* base;
    JSValue ab;
    *powned = NULL;
    if (JS_IsString(v)) {
        const char* s = JS_ToCStringLen(ctx, &n, v);
        if (!s)
            return -1;
        *powned = s;
        *pdata = (const uint8_t*)s;
        *plen = n;
        return 0;
    }
    ab = JS_GetBufferKind(v) == JS_BUFFER_KIND_VIEW ? JS_GetArrayBufferView(ctx, v, &off, &len, &bpe) : JS_EXCEPTION;
    if (!JS_IsException(ab)) {
        base = JS_GetArrayBuffer(ctx, &n, ab);
        JS_FreeValue(ctx, ab);
        if (!base)
            return -1;
        if (off > n || len > n - off) {
            JS_ThrowRangeError(ctx, "typed array out of bounds");
            return -1;
        }
        *pdata = base + off;
        *plen = len;
        return 0;
    }
    JS_FreeValue(ctx, JS_GetException(ctx));
    base = JS_GetArrayBuffer(ctx, &n, v);
    if (base) {
        *pdata = base;
        *plen = n;
        return 0;
    }
    JS_FreeValue(ctx, JS_GetException(ctx));
    JS_ThrowTypeError(ctx, "a key or secret must be a string, an ArrayBuffer or a byte view (got %s)",
        JS_IsUndefined(v) ? "undefined" : JS_IsNull(v) ? "null"
            : JS_IsNumber(v)                           ? "a number"
            : JS_IsBool(v)                             ? "a boolean"
                                                       : "an object that is not a byte view");
    return -1;
}

int dyn_otp_int(JSContext* ctx, JSValueConst v, const char* fn, const char* name,
    double lo, double hi, int64_t* out)
{
    double d;
    if (!JS_IsNumber(v)) {
        JS_ThrowTypeError(ctx, "%s: %s must be a number", fn, name);
        return -1;
    }
    if (JS_ToFloat64(ctx, &d, v))
        return -1;
    if (!(d >= lo && d <= hi) || d != floor(d)) {
        JS_ThrowRangeError(ctx, "%s: %s must be an integer in [%.17g, %.17g]", fn, name, lo, hi);
        return -1;
    }
    *out = (int64_t)d;
    return 0;
}

int dyn_otp_opt_int(JSContext* ctx, JSValueConst o, const char* fn, const char* name,
    double lo, double hi, int64_t* out)
{
    JSValue v;
    int r;
    if (!JS_IsObject(o))
        return 0;
    v = JS_GetPropertyStr(ctx, o, name);
    if (JS_IsException(v))
        return -1;
    if (JS_IsUndefined(v))
        return 0;
    r = dyn_otp_int(ctx, v, fn, name, lo, hi, out);
    JS_FreeValue(ctx, v);
    return r < 0 ? -1 : 1;
}

int dyn_otp_opt_algo(JSContext* ctx, JSValueConst o, const char* fn, const char** out)
{
    JSValue v;
    const char* s;
    *out = "sha1";
    if (!JS_IsObject(o))
        return 0;
    v = JS_GetPropertyStr(ctx, o, "algo");
    if (JS_IsException(v))
        return -1;
    if (JS_IsUndefined(v))
        return 0;
    if (!JS_IsString(v)) {
        JS_FreeValue(ctx, v);
        JS_ThrowTypeError(ctx, "%s: algo must be \"sha1\", \"sha256\" or \"sha512\"", fn);
        return -1;
    }
    s = JS_ToCString(ctx, v);
    JS_FreeValue(ctx, v);
    if (!s)
        return -1;
    *out = !strcmp(s, "sha1") ? "sha1" : !strcmp(s, "sha256") ? "sha256" : !strcmp(s, "sha512") ? "sha512" : NULL;
    JS_FreeCString(ctx, s);
    if (!*out) {
        JS_ThrowTypeError(ctx, "%s: algo must be \"sha1\", \"sha256\" or \"sha512\"", fn);
        return -1;
    }
    return 1;
}

static const char b32_alpha[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";

size_t dyn_otp_b32_encode(const uint8_t* in, size_t n, char* out)
{
    size_t i, o = 0;
    uint32_t acc = 0;
    int bits = 0;
    for (i = 0; i < n; i++) {
        acc = (acc << 8) | in[i];
        bits += 8;
        while (bits >= 5) {
            out[o++] = b32_alpha[(acc >> (bits - 5)) & 31];
            bits -= 5;
        }
    }
    if (bits > 0)
        out[o++] = b32_alpha[(acc << (5 - bits)) & 31];
    out[o] = 0;
    return o;
}

int dyn_otp_b32_decode(const char* s, size_t n, uint8_t* out, size_t* outn)
{
    size_t i, o = 0, pad = 0, rem;
    uint32_t acc = 0;
    int bits = 0;
    while (n > 0 && s[n - 1] == '=') {
        n--;
        pad++;
    }
    rem = n % 8;
    if (n == 0 || rem == 1 || rem == 3 || rem == 6)
        return -1;
    if (pad && (n + pad) % 8)
        return -1;
    for (i = 0; i < n; i++) {
        int c = (unsigned char)s[i], v;
        if (c >= 'A' && c <= 'Z')
            v = c - 'A';
        else if (c >= 'a' && c <= 'z')
            v = c - 'a';
        else if (c >= '2' && c <= '7')
            v = c - '2' + 26;
        else
            return -1;
        acc = (acc << 5) | (uint32_t)v;
        bits += 5;
        if (bits >= 8) {
            out[o++] = (uint8_t)(acc >> (bits - 8));
            bits -= 8;
        }
    }
    if (bits && (acc & ((1u << bits) - 1)))
        return -1;
    *outn = o;
    return 0;
}

JSValue dyn_otp_u8(JSContext* ctx, const uint8_t* data, size_t len)
{
    JSValue ab, out;
    JSValueConst a[3];
    ab = JS_NewArrayBufferCopy(ctx, data, len);
    if (JS_IsException(ab))
        return ab;
    a[0] = ab;
    a[1] = JS_UNDEFINED;
    a[2] = JS_UNDEFINED;
    out = JS_NewTypedArray(ctx, 3, a, JS_TYPED_ARRAY_UINT8);
    JS_FreeValue(ctx, ab);
    return out;
}

static int otp_scan(const char* algo, const uint8_t* key, size_t keylen, int digits,
    const char* code, int64_t lo, int64_t hi, int64_t center, int64_t last, int64_t* found)
{
    char out[16];
    int64_t c, best = -1;
    uint64_t bestd = 0;
    for (c = lo; c <= hi; c++) {
        uint64_t d;
        if (c < 0 || c <= last || (double)c > DYN_OTP_MAX_SAFE)
            continue;
        if (dyn_otp_compute(algo, key, keylen, (uint64_t)c, digits, out, sizeof out) < 0)
            return -1;
        if (dyn_ct_equal((const uint8_t*)out, (const uint8_t*)code, (size_t)digits) != 1)
            continue;
        d = c >= center ? (uint64_t)(c - center) : (uint64_t)(center - c);
        if (best < 0 || d < bestd) {
            best = c;
            bestd = d;
        }
    }
    dyn_wipe(out, sizeof out);
    *found = best;
    return best >= 0;
}

static JSValue otp_result(JSContext* ctx, const char* k1, int64_t v1, const char* k2, int64_t v2)
{
    JSValue o = JS_NewObject(ctx);
    if (JS_IsException(o))
        return o;
    if (JS_DefinePropertyValueStr(ctx, o, k1, JS_NewInt64(ctx, v1), JS_PROP_C_W_E) < 0
        || JS_DefinePropertyValueStr(ctx, o, k2, JS_NewInt64(ctx, v2), JS_PROP_C_W_E) < 0) {
        JS_FreeValue(ctx, o);
        return JS_EXCEPTION;
    }
    return o;
}

static int otp_digits(JSContext* ctx, JSValueConst o, const char* fn, int* digits)
{
    int64_t d = 6;
    if (dyn_otp_opt_int(ctx, o, fn, "digits", 6, 8, &d) < 0)
        return -1;
    *digits = (int)d;
    return 0;
}

static JSValue otp_match_finish(JSContext* ctx, JSValueConst secret, JSValueConst codev,
    const char* fn, const char* algo, int digits, int64_t lo, int64_t hi, int64_t center,
    int64_t last, int64_t* found)
{
    const uint8_t* key;
    size_t keylen, codelen;
    const char *kown, *code;
    int r = 0;
    if (!JS_IsString(codev))
        return JS_ThrowTypeError(ctx, "%s: code must be a string", fn);
    code = JS_ToCStringLen(ctx, &codelen, codev);
    if (!code)
        return JS_EXCEPTION;
    if (dyn_otp_secret(ctx, secret, &key, &keylen, &kown)) {
        JS_FreeCString(ctx, code);
        return JS_EXCEPTION;
    }
    if (dyn_otp_wellformed(code, codelen, digits))
        r = otp_scan(algo, key, keylen, digits, code, lo, hi, center, last, found);
    JS_FreeCString(ctx, code);
    if (kown)
        JS_FreeCString(ctx, kown);
    if (r < 0)
        return JS_ThrowTypeError(ctx, "%s: algo must be \"sha1\", \"sha256\" or \"sha512\"", fn);
    return r ? JS_TRUE : JS_NULL;
}

JSValue dyn_otp_hotp_match(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    static const char fn[] = "HOTPMatch";
    JSValueConst o = argc > 3 ? argv[3] : JS_UNDEFINED;
    int64_t counter, ahead = 0, found = -1;
    const char* algo;
    int digits;
    JSValue r;
    (void)this_val;
    if (argc < 3)
        return JS_ThrowTypeError(ctx, "HOTPMatch(secret, counter, code[, opts])");
    if (dyn_opts_bag(ctx, argc, argv, 3, hotp_match_keys, countof(hotp_match_keys), fn) < 0
        || dyn_otp_int(ctx, argv[1], fn, "counter", 0, DYN_OTP_MAX_SAFE, &counter) < 0
        || otp_digits(ctx, o, fn, &digits) < 0
        || dyn_otp_opt_algo(ctx, o, fn, &algo) < 0
        || dyn_otp_opt_int(ctx, o, fn, "lookAhead", 0, DYN_OTP_MAX_WINDOW, &ahead) < 0)
        return JS_EXCEPTION;
    r = otp_match_finish(ctx, argv[0], argv[2], fn, algo, digits, counter, counter + ahead, counter, -1, &found);
    if (!JS_IsBool(r))
        return r;
    return otp_result(ctx, "counter", found, "next", found + 1);
}

JSValue dyn_otp_totp_match(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    static const char fn[] = "TOTPMatch";
    JSValueConst o = argc > 2 ? argv[2] : JS_UNDEFINED;
    int64_t at = (int64_t)time(NULL), period = 30, window = 0, drift = 0, last = -1, base, center, found = -1;
    const char* algo;
    int digits;
    JSValue r;
    (void)this_val;
    if (argc < 2)
        return JS_ThrowTypeError(ctx, "TOTPMatch(secret, code[, opts])");
    if (dyn_opts_bag(ctx, argc, argv, 2, totp_match_keys, countof(totp_match_keys), fn) < 0
        || dyn_otp_opt_int(ctx, o, fn, "atSec", 0, DYN_OTP_MAX_SAFE, &at) < 0
        || dyn_otp_opt_int(ctx, o, fn, "period", 1, DYN_OTP_MAX_SAFE, &period) < 0
        || otp_digits(ctx, o, fn, &digits) < 0
        || dyn_otp_opt_algo(ctx, o, fn, &algo) < 0
        || dyn_otp_opt_int(ctx, o, fn, "window", 0, DYN_OTP_MAX_WINDOW, &window) < 0
        || dyn_otp_opt_int(ctx, o, fn, "drift", -DYN_OTP_MAX_SAFE, DYN_OTP_MAX_SAFE, &drift) < 0
        || dyn_otp_opt_int(ctx, o, fn, "lastCounter", 0, DYN_OTP_MAX_SAFE, &last) < 0)
        return JS_EXCEPTION;
    base = at / period;
    center = base + drift;
    r = otp_match_finish(ctx, argv[0], argv[1], fn, algo, digits, center - window, center + window, center, last, &found);
    if (!JS_IsBool(r))
        return r;
    return otp_result(ctx, "counter", found, "drift", found - base);
}

JSValue dyn_otp_throttle(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    static const char fn[] = "Throttle";
    JSValueConst o = argc > 2 ? argv[2] : JS_UNDEFINED;
    int64_t failures, lastf = 0, at = (int64_t)time(NULL), step = 5, lockout = 10;
    double retry;
    int locked;
    JSValue r;
    (void)this_val;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "Throttle(failures, lastFailureSec[, opts])");
    if (dyn_otp_int(ctx, argv[0], fn, "failures", 0, DYN_OTP_MAX_SAFE, &failures) < 0
        || dyn_opts_bag(ctx, argc, argv, 2, throttle_keys, countof(throttle_keys), fn) < 0
        || dyn_otp_opt_int(ctx, o, fn, "atSec", 0, DYN_OTP_MAX_SAFE, &at) < 0
        || dyn_otp_opt_int(ctx, o, fn, "stepSec", 0, 86400, &step) < 0
        || dyn_otp_opt_int(ctx, o, fn, "lockoutAfter", 0, DYN_OTP_MAX_SAFE, &lockout) < 0)
        return JS_EXCEPTION;
    if (failures > 0) {
        if (argc < 2 || JS_IsUndefined(argv[1]))
            return JS_ThrowTypeError(ctx, "Throttle: lastFailureSec is required when failures > 0");
        if (dyn_otp_int(ctx, argv[1], fn, "lastFailureSec", 0, DYN_OTP_MAX_SAFE, &lastf) < 0)
            return JS_EXCEPTION;
    }
    locked = lockout > 0 && failures >= lockout;
    retry = locked ? HUGE_VAL : failures ? (double)lastf + (double)step * (double)failures : 0.0;
    r = JS_NewObject(ctx);
    if (JS_IsException(r))
        return r;
    if (JS_DefinePropertyValueStr(ctx, r, "allowed", JS_NewBool(ctx, !locked && (double)at >= retry), JS_PROP_C_W_E) < 0
        || JS_DefinePropertyValueStr(ctx, r, "locked", JS_NewBool(ctx, locked), JS_PROP_C_W_E) < 0
        || JS_DefinePropertyValueStr(ctx, r, "retryAtSec", JS_NewFloat64(ctx, retry), JS_PROP_C_W_E) < 0) {
        JS_FreeValue(ctx, r);
        return JS_EXCEPTION;
    }
    return r;
}

#endif
