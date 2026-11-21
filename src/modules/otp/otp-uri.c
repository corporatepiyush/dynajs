#include "otp-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_OTP)

#ifdef CONFIG_NATIVE_MODULE_ENCODING
#include "../encoding/encoding-internal.h"
#endif

static const char* const build_keys[] = { "type", "account", "issuer", "secret", "algo", "digits", "period", "counter" };
static const char* const enroll_keys[] = { "type", "account", "issuer", "secret", "secretBytes", "algo", "digits", "period", "counter", "qr", "dyna" };
static const char* const enroll_meta_keys[] = { "qr" };
static const char hexu[] = "0123456789ABCDEF";

static int otp_hexv(int c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

static int otp_pct(const char* s, size_t n, int plus, char* out, size_t* on)
{
    size_t i, o = 0;
    for (i = 0; i < n; i++) {
        int c = (unsigned char)s[i];
        if (c == '%') {
            int h, l;
            if (i + 2 >= n)
                return -1;
            h = otp_hexv((unsigned char)s[i + 1]);
            l = otp_hexv((unsigned char)s[i + 2]);
            if (h < 0 || l < 0)
                return -1;
            out[o++] = (char)(h * 16 + l);
            i += 2;
        } else if (c == '+' && plus) {
            out[o++] = ' ';
        } else {
            out[o++] = (char)c;
        }
    }
    *on = o;
    return 0;
}

static int otp_text_ok(const char* p, size_t n)
{
    size_t i = 0;
    while (i < n) {
        unsigned c = (unsigned char)p[i];
        uint32_t cp;
        size_t k, len;
        if (c < 0x80) {
            if (c < 0x20 || c == 0x7f)
                return 0;
            i++;
            continue;
        }
        if (c >= 0xc2 && c <= 0xdf) {
            len = 2;
            cp = c & 0x1f;
        } else if (c >= 0xe0 && c <= 0xef) {
            len = 3;
            cp = c & 0x0f;
        } else if (c >= 0xf0 && c <= 0xf4) {
            len = 4;
            cp = c & 0x07;
        } else {
            return 0;
        }
        if (i + len > n)
            return 0;
        for (k = 1; k < len; k++) {
            unsigned d = (unsigned char)p[i + k];
            if ((d & 0xc0) != 0x80)
                return 0;
            cp = (cp << 6) | (d & 0x3f);
        }
        if ((len == 3 && (cp < 0x800 || (cp >= 0xd800 && cp <= 0xdfff))) || (len == 4 && (cp < 0x10000 || cp > 0x10ffff)))
            return 0;
        if (cp >= 0x80 && cp <= 0x9f)
            return 0;
        i += len;
    }
    return 1;
}

static int otp_ieq(const char* a, size_t n, const char* b)
{
    size_t i;
    if (strlen(b) != n)
        return 0;
    for (i = 0; i < n; i++) {
        int c = (unsigned char)a[i];
        if (c >= 'A' && c <= 'Z')
            c += 32;
        if (c != b[i])
            return 0;
    }
    return 1;
}

static int otp_dec_num(const char* s, size_t n, double lo, double hi, int64_t* out)
{
    size_t i;
    double v = 0;
    if (n == 0 || n > 16)
        return -1;
    for (i = 0; i < n; i++) {
        if (s[i] < '0' || s[i] > '9')
            return -1;
        v = v * 10 + (s[i] - '0');
    }
    if (v < lo || v > hi)
        return -1;
    *out = (int64_t)v;
    return 0;
}

static int otp_set_str(JSContext* ctx, JSValue o, const char* k, const char* s, size_t n)
{
    return JS_DefinePropertyValueStr(ctx, o, k, JS_NewStringLen(ctx, s, n), JS_PROP_C_W_E);
}

enum { P_SECRET,
    P_ISSUER,
    P_ALGO,
    P_DIGITS,
    P_COUNTER,
    P_PERIOD,
    P_N };

static const char* const pnames[P_N] = { "secret", "issuer", "algorithm", "digits", "counter", "period" };

JSValue dyn_otp_uri_parse(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    const char *s = NULL, *p, *end, *q, *why = NULL;
    size_t n, i, lablen, alen;
    char *buf = NULL, *val[P_N];
    size_t vlen[P_N];
    char* lab;
    const char *issuer = NULL, *account;
    size_t ilen = 0;
    int hotp, k, colons = 0;
    uint8_t* sec = NULL;
    size_t slen = 0;
    int64_t digits = 6, counter = 0, period = 30;
    const char* algo = "sha1";
    JSValue r = JS_EXCEPTION, o;
    (void)this_val;
    if (argc < 1 || !JS_IsString(argv[0]))
        return JS_ThrowTypeError(ctx, "KeyURIParse(uri): uri must be a string");
    s = JS_ToCStringLen(ctx, &n, argv[0]);
    if (!s)
        return JS_EXCEPTION;
    for (k = 0; k < P_N; k++) {
        val[k] = NULL;
        vlen[k] = 0;
    }
    buf = (char*)malloc(n * 2 + 2);
    sec = (uint8_t*)malloc(n + 1);
    if (!buf || !sec) {
        JS_ThrowOutOfMemory(ctx);
        goto out;
    }
    if (memchr(s, '#', n)) {
        why = "a fragment (#) is not allowed";
        goto bad;
    }
    if (n < 10 || !otp_ieq(s, 10, "otpauth://")) {
        why = "the scheme must be otpauth://";
        goto bad;
    }
    p = s + 10;
    end = s + n;
    q = memchr(p, '/', (size_t)(end - p));
    if (!q) {
        why = "missing /label";
        goto bad;
    }
    if (otp_ieq(p, (size_t)(q - p), "hotp"))
        hotp = 1;
    else if (otp_ieq(p, (size_t)(q - p), "totp"))
        hotp = 0;
    else {
        why = "the type must be totp or hotp";
        goto bad;
    }
    p = q + 1;
    q = memchr(p, '?', (size_t)(end - p));
    if (!q)
        q = end;
    lab = buf;
    if (otp_pct(p, (size_t)(q - p), 0, lab, &lablen) < 0) {
        why = "bad percent-encoding in the label";
        goto bad;
    }
    if (!otp_text_ok(lab, lablen)) {
        why = "the label is not valid UTF-8 text";
        goto bad;
    }
    account = lab;
    alen = lablen;
    for (i = 0; i < lablen; i++)
        if (lab[i] == ':') {
            colons++;
            if (colons == 1) {
                issuer = lab;
                ilen = i;
                account = lab + i + 1;
                alen = lablen - i - 1;
            }
        }
    if (colons > 1) {
        why = "the label holds more than one colon";
        goto bad;
    }
    while (alen > 0 && *account == ' ') {
        account++;
        alen--;
    }
    if (alen == 0 || (issuer && ilen == 0)) {
        why = "the label needs a non-empty account name (and issuer, when prefixed)";
        goto bad;
    }
    {
        char* w = buf + lablen + 1;
        const char* a = q < end ? q + 1 : end;
        while (a < end) {
            const char* amp = memchr(a, '&', (size_t)(end - a));
            const char* seg_end = amp ? amp : end;
            const char* eq = memchr(a, '=', (size_t)(seg_end - a));
            const char* kend = eq ? eq : seg_end;
            size_t kl;
            if (otp_pct(a, (size_t)(kend - a), 1, w, &kl) < 0) {
                why = "bad percent-encoding in a parameter";
                goto bad;
            }
            for (k = 0; k < P_N; k++)
                if (kl == strlen(pnames[k]) && !memcmp(w, pnames[k], kl))
                    break;
            if (k < P_N) {
                size_t vl;
                if (val[k]) {
                    why = "a parameter appears twice";
                    goto bad;
                }
                if (otp_pct(eq ? eq + 1 : seg_end, eq ? (size_t)(seg_end - eq - 1) : 0, 1, w, &vl) < 0) {
                    why = "bad percent-encoding in a parameter";
                    goto bad;
                }
                if (!otp_text_ok(w, vl)) {
                    why = "a parameter is not valid UTF-8 text";
                    goto bad;
                }
                val[k] = w;
                vlen[k] = vl;
                w += vl + 1;
            }
            a = amp ? amp + 1 : end;
        }
    }
    if (!val[P_SECRET] || dyn_otp_b32_decode(val[P_SECRET], vlen[P_SECRET], sec, &slen) < 0) {
        why = "secret must be present and valid base32";
        goto bad;
    }
    if (val[P_ISSUER]) {
        if (vlen[P_ISSUER] == 0 || memchr(val[P_ISSUER], ':', vlen[P_ISSUER])) {
            why = "issuer must be non-empty without a colon";
            goto bad;
        }
        if (issuer && (ilen != vlen[P_ISSUER] || memcmp(issuer, val[P_ISSUER], ilen))) {
            why = "the issuer parameter differs from the label prefix";
            goto bad;
        }
        issuer = val[P_ISSUER];
        ilen = vlen[P_ISSUER];
    }
    if (val[P_ALGO]) {
        if (otp_ieq(val[P_ALGO], vlen[P_ALGO], "sha1"))
            algo = "sha1";
        else if (otp_ieq(val[P_ALGO], vlen[P_ALGO], "sha256"))
            algo = "sha256";
        else if (otp_ieq(val[P_ALGO], vlen[P_ALGO], "sha512"))
            algo = "sha512";
        else {
            why = "algorithm must be SHA1, SHA256 or SHA512";
            goto bad;
        }
    }
    if (val[P_DIGITS] && (otp_dec_num(val[P_DIGITS], vlen[P_DIGITS], 6, 8, &digits) < 0 || digits == 7)) {
        why = "digits must be 6 or 8";
        goto bad;
    }
    if (hotp && (!val[P_COUNTER] || otp_dec_num(val[P_COUNTER], vlen[P_COUNTER], 0, DYN_OTP_MAX_SAFE, &counter) < 0)) {
        why = "a hotp URI needs counter, an integer in [0, 2^53-1]";
        goto bad;
    }
    if (!hotp && val[P_PERIOD] && otp_dec_num(val[P_PERIOD], vlen[P_PERIOD], 1, DYN_OTP_MAX_SAFE, &period) < 0) {
        why = "period must be an integer in [1, 2^53-1]";
        goto bad;
    }
    o = JS_NewObject(ctx);
    if (JS_IsException(o))
        goto out;
    if (otp_set_str(ctx, o, "type", hotp ? "hotp" : "totp", 4) < 0
        || (issuer && otp_set_str(ctx, o, "issuer", issuer, ilen) < 0)
        || otp_set_str(ctx, o, "account", account, alen) < 0
        || JS_DefinePropertyValueStr(ctx, o, "secret", dyn_otp_u8(ctx, sec, slen), JS_PROP_C_W_E) < 0
        || otp_set_str(ctx, o, "algo", algo, strlen(algo)) < 0
        || JS_DefinePropertyValueStr(ctx, o, "digits", JS_NewInt32(ctx, (int32_t)digits), JS_PROP_C_W_E) < 0
        || JS_DefinePropertyValueStr(ctx, o, hotp ? "counter" : "period", JS_NewInt64(ctx, hotp ? counter : period), JS_PROP_C_W_E) < 0) {
        JS_FreeValue(ctx, o);
        goto out;
    }
    r = o;
    goto out;
bad:
    JS_ThrowSyntaxError(ctx, "KeyURIParse: %s", why);
out:
    if (sec) {
        dyn_wipe(sec, n + 1);
        free(sec);
    }
    if (buf) {
        dyn_wipe(buf, n * 2 + 2);
        free(buf);
    }
    JS_FreeCString(ctx, s);
    return r;
}

typedef struct {
    char* p;
    size_t n, cap;
    int oom;
} otp_sb;

static void sb_put(otp_sb* b, const char* s, size_t n)
{
    if (b->oom)
        return;
    if (b->n + n + 1 > b->cap) {
        size_t c = b->cap ? b->cap : 128;
        char* np;
        while (c < b->n + n + 1)
            c *= 2;
        np = (char*)realloc(b->p, c);
        if (!np) {
            b->oom = 1;
            return;
        }
        b->p = np;
        b->cap = c;
    }
    memcpy(b->p + b->n, s, n);
    b->n += n;
    b->p[b->n] = 0;
}

static void sb_cstr(otp_sb* b, const char* s)
{
    sb_put(b, s, strlen(s));
}

static void sb_enc(otp_sb* b, const char* s, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_' || c == '~') {
            sb_put(b, (const char*)&s[i], 1);
        } else {
            char e[3] = { '%', hexu[c >> 4], hexu[c & 15] };
            sb_put(b, e, 3);
        }
    }
}

static int otp_name(JSContext* ctx, JSValueConst o, const char* fn, const char* name, int required,
    const char** out, size_t* len)
{
    JSValue v = JS_GetPropertyStr(ctx, o, name);
    *out = NULL;
    if (JS_IsException(v))
        return -1;
    if (JS_IsUndefined(v)) {
        if (required) {
            JS_ThrowTypeError(ctx, "%s: %s is required", fn, name);
            return -1;
        }
        return 0;
    }
    if (!JS_IsString(v)) {
        JS_FreeValue(ctx, v);
        JS_ThrowTypeError(ctx, "%s: %s must be a string", fn, name);
        return -1;
    }
    *out = JS_ToCStringLen(ctx, len, v);
    JS_FreeValue(ctx, v);
    if (!*out)
        return -1;
    if (*len == 0 || memchr(*out, ':', *len) || !otp_text_ok(*out, *len)) {
        JS_FreeCString(ctx, *out);
        *out = NULL;
        JS_ThrowRangeError(ctx, "%s: %s must be non-empty text without a colon or control characters", fn, name);
        return -1;
    }
    return 1;
}

static int otp_type(JSContext* ctx, JSValueConst o, const char* fn, int* hotp)
{
    JSValue v = JS_GetPropertyStr(ctx, o, "type");
    const char* s;
    *hotp = 0;
    if (JS_IsException(v))
        return -1;
    if (JS_IsUndefined(v))
        return 0;
    s = JS_IsString(v) ? JS_ToCString(ctx, v) : NULL;
    JS_FreeValue(ctx, v);
    if (s && !strcmp(s, "hotp"))
        *hotp = 1;
    else if (!s || strcmp(s, "totp")) {
        if (s)
            JS_FreeCString(ctx, s);
        JS_ThrowTypeError(ctx, "%s: type must be \"totp\" or \"hotp\"", fn);
        return -1;
    }
    JS_FreeCString(ctx, s);
    return 0;
}

static JSValue otp_build(JSContext* ctx, JSValueConst o, const char* fn, const uint8_t* sec, size_t slen)
{
    const char *account = NULL, *issuer = NULL, *algo;
    size_t alen = 0, ilen = 0, i;
    int hotp, has_p, has_c;
    int64_t digits = 6, period = 30, counter = 0;
    char num[32], *b32 = NULL, alg[8];
    otp_sb b = { NULL, 0, 0, 0 };
    JSValue r = JS_EXCEPTION;
    if (otp_type(ctx, o, fn, &hotp) < 0
        || otp_name(ctx, o, fn, "account", 1, &account, &alen) < 0
        || otp_name(ctx, o, fn, "issuer", 0, &issuer, &ilen) < 0
        || dyn_otp_opt_algo(ctx, o, fn, &algo) < 0
        || dyn_otp_opt_int(ctx, o, fn, "digits", 6, 8, &digits) < 0
        || (has_p = dyn_otp_opt_int(ctx, o, fn, "period", 1, DYN_OTP_MAX_SAFE, &period)) < 0
        || (has_c = dyn_otp_opt_int(ctx, o, fn, "counter", 0, DYN_OTP_MAX_SAFE, &counter)) < 0)
        goto out;
    if (digits == 7) {
        JS_ThrowRangeError(ctx, "%s: digits must be 6 or 8 (Key URI format)", fn);
        goto out;
    }
    if ((hotp && has_p) || (!hotp && has_c)) {
        JS_ThrowTypeError(ctx, "%s: period applies only to totp and counter only to hotp", fn);
        goto out;
    }
    if (slen == 0) {
        JS_ThrowRangeError(ctx, "%s: secret must not be empty", fn);
        goto out;
    }
    b32 = (char*)malloc(slen * 2 + 8);
    if (!b32) {
        JS_ThrowOutOfMemory(ctx);
        goto out;
    }
    dyn_otp_b32_encode(sec, slen, b32);
    for (i = 0; algo[i]; i++)
        alg[i] = (char)(algo[i] >= 'a' && algo[i] <= 'z' ? algo[i] - 32 : algo[i]);
    alg[i] = 0;
    sb_cstr(&b, hotp ? "otpauth://hotp/" : "otpauth://totp/");
    if (issuer) {
        sb_enc(&b, issuer, ilen);
        sb_cstr(&b, ":");
    }
    sb_enc(&b, account, alen);
    sb_cstr(&b, "?secret=");
    sb_cstr(&b, b32);
    if (issuer) {
        sb_cstr(&b, "&issuer=");
        sb_enc(&b, issuer, ilen);
    }
    sb_cstr(&b, "&algorithm=");
    sb_cstr(&b, alg);
    snprintf(num, sizeof num, "&digits=%d", (int)digits);
    sb_cstr(&b, num);
    snprintf(num, sizeof num, hotp ? "&counter=%lld" : "&period=%lld", (long long)(hotp ? counter : period));
    sb_cstr(&b, num);
    if (b.oom) {
        JS_ThrowOutOfMemory(ctx);
        goto out;
    }
    r = JS_NewStringLen(ctx, b.p, b.n);
out:
    if (b32) {
        dyn_wipe(b32, slen * 2 + 8);
        free(b32);
    }
    if (b.p) {
        dyn_wipe(b.p, b.cap);
        free(b.p);
    }
    if (account)
        JS_FreeCString(ctx, account);
    if (issuer)
        JS_FreeCString(ctx, issuer);
    return r;
}

JSValue dyn_otp_uri_build(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    static const char fn[] = "KeyURIBuild";
    const uint8_t* sec;
    size_t slen;
    const char* own;
    JSValue sv, r;
    (void)this_val;
    if (argc < 1 || !JS_IsObject(argv[0]) || JS_IsFunction(ctx, argv[0]))
        return JS_ThrowTypeError(ctx, "KeyURIBuild(key): key must be an object");
    if (dyn_opts_strict(ctx, argv[0], build_keys, countof(build_keys)) < 0)
        return JS_EXCEPTION;
    sv = JS_GetPropertyStr(ctx, argv[0], "secret");
    if (JS_IsException(sv))
        return sv;
    if (JS_IsUndefined(sv))
        return JS_ThrowTypeError(ctx, "KeyURIBuild: secret is required");
    if (dyn_otp_secret(ctx, sv, &sec, &slen, &own)) {
        JS_FreeValue(ctx, sv);
        return JS_EXCEPTION;
    }
    r = otp_build(ctx, argv[0], fn, sec, slen);
    if (own)
        JS_FreeCString(ctx, own);
    JS_FreeValue(ctx, sv);
    return r;
}

static int otp_set(JSContext* ctx, JSValue o, const char* k, JSValue v)
{
    if (JS_IsException(v))
        return -1;
    return JS_DefinePropertyValueStr(ctx, o, k, v, JS_PROP_C_W_E);
}

JSValue dyn_otp_enroll(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    static const char fn[] = "Enroll";
    JSValueConst o = argc > 0 ? argv[0] : JS_UNDEFINED;
    uint8_t rnd[64], *sec = NULL;
    const uint8_t* given;
    size_t slen = 20;
    const char* own = NULL;
    int64_t want = 20;
    int has_want, qr = 1;
    char b32[112];
    JSValue sv = JS_UNDEFINED, mv = JS_UNDEFINED, qv = JS_UNDEFINED, uri = JS_UNDEFINED, r = JS_EXCEPTION, res;
    (void)this_val;
    if (!JS_IsObject(o) || JS_IsFunction(ctx, o))
        return JS_ThrowTypeError(ctx, "Enroll(opts): opts must be an object with account");
    if (dyn_opts_strict(ctx, o, enroll_keys, countof(enroll_keys)) < 0
        || (has_want = dyn_otp_opt_int(ctx, o, fn, "secretBytes", 16, 64, &want)) < 0)
        return JS_EXCEPTION;
    mv = JS_GetPropertyStr(ctx, o, "dyna");
    if (JS_IsException(mv))
        return mv;
    if (!JS_IsUndefined(mv)) {
        if (!JS_IsObject(mv) || JS_IsFunction(ctx, mv)) {
            JS_FreeValue(ctx, mv);
            return JS_ThrowTypeError(ctx, "Enroll: dyna must be an object");
        }
        if (dyn_opts_strict(ctx, mv, enroll_meta_keys, countof(enroll_meta_keys)) < 0
            || dyn_opt_bool(ctx, mv, "qr", "Enroll: dyna", &qr) < 0) {
            JS_FreeValue(ctx, mv);
            return JS_EXCEPTION;
        }
    }
    JS_FreeValue(ctx, mv);
    sv = JS_GetPropertyStr(ctx, o, "secret");
    if (JS_IsException(sv))
        return sv;
    if (!JS_IsUndefined(sv)) {
        if (has_want) {
            JS_FreeValue(ctx, sv);
            return JS_ThrowTypeError(ctx, "Enroll: give secret or secretBytes, not both");
        }
        if (dyn_otp_secret(ctx, sv, &given, &slen, &own))
            goto out;
        if (slen < 16 || slen > 64) {
            JS_ThrowRangeError(ctx, "Enroll: secret must be 16..64 bytes (RFC 4226 asks for at least 128 bits)");
            goto out;
        }
        memcpy(rnd, given, slen);
    } else {
        slen = (size_t)want;
        if (dyn_os_entropy(rnd, slen) < 0) {
            JS_ThrowInternalError(ctx, "Enroll: the OS entropy source failed");
            goto out;
        }
    }
    sec = rnd;
    uri = otp_build(ctx, o, fn, sec, slen);
    if (JS_IsException(uri))
        goto out;
    res = JS_NewObject(ctx);
    if (JS_IsException(res))
        goto out;
    dyn_otp_b32_encode(sec, slen, b32);
    if (otp_set(ctx, res, "uri", JS_DupValue(ctx, uri)) < 0
        || otp_set(ctx, res, "secret", dyn_otp_u8(ctx, sec, slen)) < 0
        || otp_set(ctx, res, "secretBase32", JS_NewString(ctx, b32)) < 0) {
        JS_FreeValue(ctx, res);
        goto out;
    }
    if (qr) {
#ifdef CONFIG_NATIVE_MODULE_ENCODING
        JSValueConst a[2];
        qv = JS_GetPropertyStr(ctx, o, "qr");
        if (JS_IsException(qv)) {
            JS_FreeValue(ctx, res);
            goto out;
        }
        a[0] = uri;
        a[1] = qv;
        if (otp_set(ctx, res, "qr", dyn_qr_encode(ctx, JS_UNDEFINED, 2, a, 0)) < 0
            || otp_set(ctx, res, "qrText", dyn_qr_encode(ctx, JS_UNDEFINED, 2, a, 1)) < 0) {
            JS_FreeValue(ctx, res);
            goto out;
        }
#else
        JS_FreeValue(ctx, res);
        JS_ThrowTypeError(ctx, "Enroll: the QR code needs dyna:encoding, absent from this build (pass dyna: { qr: false })");
        goto out;
#endif
    }
    r = res;
out:
    dyn_wipe(rnd, sizeof rnd);
    dyn_wipe(b32, sizeof b32);
    if (own)
        JS_FreeCString(ctx, own);
    JS_FreeValue(ctx, sv);
    JS_FreeValue(ctx, qv);
    JS_FreeValue(ctx, uri);
    return r;
}

#endif
