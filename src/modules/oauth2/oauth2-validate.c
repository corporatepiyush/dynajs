#include "oauth2-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_OAUTH2)

static int is_b64token_char(unsigned char c)
{
    static const uint64_t m[4] = { 0x03ffe80000000000ull, 0x47fffffe87fffffeull, 0, 0 };
    return (int)((m[c >> 6] >> (c & 63)) & 1u);
}

JSValue oauth_is_valid_verifier(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    const char* s;
    size_t n, i;
    (void)this_val;
    if (argc < 1 || !JS_IsString(argv[0]))
        return JS_FALSE;
    s = JS_ToCStringLen(ctx, &n, argv[0]);
    if (!s)
        return JS_EXCEPTION;
    if (n < 43 || n > 128) {
        JS_FreeCString(ctx, s);
        return JS_FALSE;
    }
    for (i = 0; i < n; i++)
        if (!is_unreserved((unsigned char)s[i])) {
            JS_FreeCString(ctx, s);
            return JS_FALSE;
        }
    JS_FreeCString(ctx, s);
    return JS_TRUE;
}

int is_valid_b64token(const char* s, size_t n, int strict)
{
    size_t pad = 0;
    if (n == 0 || (strict && n > 4096))
        return 0;
    while (pad < n && s[n - 1 - pad] == '=')
        pad++;
    if (strict && pad > 2)
        return 0;
    if (n - pad == 0)
        return 0;
    for (size_t i = 0; i < n - pad; i++)
        if (!is_b64token_char((unsigned char)s[i]))
            return 0;
    return 1;
}

static int is_nqschar(unsigned char c) { return c >= 0x20 && c <= 0x7E && c != 0x22 && c != 0x5C; }

int is_valid_error(const char* s, size_t n, int strict)
{
    size_t i;
    if (n == 0 || (strict && n > 64))
        return 0;
    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (strict ? !is_b64token_char(c) : !is_nqschar(c))
            return 0;
    }
    return 1;
}

int is_valid_error_desc(const char* s, size_t n, int sp)
{
    size_t i;
    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (!is_nqschar(c) || (c == 0x20 && !sp))
            return 0;
    }
    return 1;
}

static const char* const oauth_bearer_keys[] = { "strict" };

JSValue oauth_is_valid_bearer(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    const char* s;
    size_t n;
    int ok, strict = 0;
    (void)this_val;
    if (dyn_opts_bag(ctx, argc, argv, 1, oauth_bearer_keys, countof(oauth_bearer_keys), "isValidBearerToken") < 0
        || (argc > 1 && dyn_opt_bool(ctx, argv[1], "strict", "isValidBearerToken", &strict) < 0))
        return JS_EXCEPTION;
    if (argc < 1 || !JS_IsString(argv[0]))
        return JS_FALSE;
    s = JS_ToCStringLen(ctx, &n, argv[0]);
    if (!s)
        return JS_EXCEPTION;
    ok = is_valid_b64token(s, n, strict);
    JS_FreeCString(ctx, s);
    return JS_NewBool(ctx, ok);
}

static int loopback_prefix(const char* s, size_t n, const char* pfx)
{
    size_t pl = strlen(pfx);
    return n >= pl && !memcmp(s, pfx, pl);
}

static int is_loopback_candidate(const char* s, size_t n)
{
    return loopback_prefix(s, n, "http://127.0.0.1") || loopback_prefix(s, n, "http://[::1]") || loopback_prefix(s, n, "http://localhost");
}

static int split_host_port(const char* auth, size_t alen, const char** host, size_t* hlen)
{
    const char *p, *end = auth + alen;
    if (memchr(auth, '@', alen))
        return 0;
    if (alen && auth[0] == '[') {
        const char* ce = memchr(auth, ']', alen);
        if (!ce)
            return 0;
        *host = auth;
        *hlen = (size_t)(ce - auth) + 1;
        p = ce + 1;
    } else {
        p = memchr(auth, ':', alen);
        if (!p) {
            *host = auth;
            *hlen = alen;
            return 1;
        }
        *host = auth;
        *hlen = (size_t)(p - auth);
    }
    if (p == end)
        return 1;
    if (*p != ':')
        return 0;
    for (p++; p < end; p++)
        if (*p < '0' || *p > '9')
            return 0;
    return 1;
}

static int host_match_ignore_port(const char* a, size_t an, const char* b, size_t bn)
{
    const char* ap = strstr(a, "://");
    const char* bp = strstr(b, "://");
    const char *ae, *be, *ahost, *bhost;
    size_t alen, blen, ah, bh;
    if (!ap || !bp)
        return 0;
    ap += 3;
    bp += 3;
    ae = memchr(ap, '/', an - (size_t)(ap - a));
    be = memchr(bp, '/', bn - (size_t)(bp - b));
    alen = ae ? (size_t)(ae - ap) : an - (size_t)(ap - a);
    blen = be ? (size_t)(be - bp) : bn - (size_t)(bp - b);
    if (!split_host_port(ap, alen, &ahost, &ah))
        return 0;
    if (!split_host_port(bp, blen, &bhost, &bh))
        return 0;
    if (ah != bh || memcmp(ahost, bhost, ah))
        return 0;
    const char* ap_path = ae ? ae : a + an;
    const char* bp_path = be ? be : b + bn;
    size_t ap_l = (size_t)(a + an - ap_path);
    size_t bp_l = (size_t)(b + bn - bp_path);
    return ap_l == bp_l && !memcmp(ap_path, bp_path, ap_l);
}

JSValue oauth_is_valid_redirect(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    const char* cand;
    size_t cn;
    JSValue lenv;
    uint32_t n = 0, i;
    int ok = 0;
    (void)this_val;
    if (argc < 2 || !JS_IsString(argv[0]))
        return JS_ThrowTypeError(ctx, "isValidRedirectUri(candidate, registered[])");
    if (!JS_IsArray(ctx, argv[1]))
        return JS_ThrowTypeError(ctx, "registered must be string[]");
    cand = JS_ToCStringLen(ctx, &cn, argv[0]);
    if (!cand)
        return JS_EXCEPTION;
    if (has_ctl_or_crlf(cand, cn)) {
        JS_FreeCString(ctx, cand);
        return JS_FALSE;
    }
    {
        const char* col = strchr(cand, ':');
        if (col) {
            size_t sl = (size_t)(col - cand);
            int has_dot = 0;
            for (size_t k = 0; k < sl; k++)
                if (cand[k] == '.')
                    has_dot = 1;
            if (!has_dot && strncmp(cand, "http:", 5) != 0 && strncmp(cand, "https:", 6) != 0) {
                JS_FreeCString(ctx, cand);
                return JS_FALSE;
            }
        }
    }
    lenv = JS_GetPropertyStr(ctx, argv[1], "length");
    if (JS_ToUint32(ctx, &n, lenv)) {
        JS_FreeValue(ctx, lenv);
        return JS_EXCEPTION;
    }
    JS_FreeValue(ctx, lenv);
    for (i = 0; i < n; i++) {
        JSValue e = JS_GetPropertyUint32(ctx, argv[1], i);
        const char* reg;
        size_t rn;
        if (JS_IsException(e)) {
            JS_FreeCString(ctx, cand);
            return JS_EXCEPTION;
        }
        if (!JS_IsString(e)) {
            JS_FreeValue(ctx, e);
            JS_FreeCString(ctx, cand);
            return JS_ThrowTypeError(ctx, "isValidRedirectUri: registered[%u] must be a string", (unsigned)i);
        }
        reg = JS_ToCStringLen(ctx, &rn, e);
        JS_FreeValue(ctx, e);
        if (!reg) {
            JS_FreeCString(ctx, cand);
            return JS_EXCEPTION;
        }
        if (!ok && ((cn == rn && !memcmp(cand, reg, cn)) || (is_loopback_candidate(cand, cn) && is_loopback_candidate(reg, rn) && host_match_ignore_port(cand, cn, reg, rn))))
            ok = 1;
        JS_FreeCString(ctx, reg);
    }
    JS_FreeCString(ctx, cand);
    return JS_NewBool(ctx, ok);
}

#endif
