/* dyna:oauth2 -- OAuth 2.0 + PKCE + Bearer helpers.
 *
 * Pure utilities, no IO, no reactor, no persistence.  This is the
 * "optional feature" the plan promised: it compiles to nothing without
 * CONFIG_NATIVE_MODULE_OAUTH2, and within it JWT-dependent helpers
 * degrade to a named error when CONFIG_TLS is absent (see the JWT
 * note below).  The goal is BCP 9700 by default: S256 PKCE, exact
 * redirect_uri, state CSRF, no implicit, no ROPC, no bearer-in-URL
 * unless explicitly allowed.
 *
 * Implementation notes that cut across the file:
 *  - Every JS-exposed function coerces ALL JS args to C locals FIRST
 *    (ToCString/ToString may run user JS that closes resources).  This
 *    module has no native handle, so the rule is simpler, but the copy
 *    discipline still matters for borrowed buffers.
 *  - b64token vs unreserved are different alphabets (6750 vs 7636) and
 *    get separate validators (review pass 3).
 *  - form urlencoding uses '+' for space (Appendix B), not %20.
 *  - Redirect validation is verbatim exact-string except loopback
 *    port-ignore (8252 §7.3, 9700 §4.1). No WHATWG normalisation.
 * Full API: see the dyna:* module in dyna-libc.h (and types/dynajs.d.ts).
 */
#include "dyna-nat.h"
#include "core/dyn-pct.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_OAUTH2)

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <strings.h>
#include <time.h>

#include "core/dyn-hash.h"
#include "core/dyn-codec.h"
#include "core/dyn-prng.h"

#ifndef countof
#define countof(x) (sizeof(x) / sizeof((x)[0]))
#endif

/* ---- small helpers ------------------------------------------------- */

static int is_unreserved(unsigned char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_' || c == '~';
}
static int is_b64token_char(unsigned char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_' || c == '~' ||
           c == '+' || c == '/' ;
}
static int is_nqchar(unsigned char c) {
    return (c == 0x21) || (c >= 0x23 && c <= 0x5B) || (c >= 0x5D && c <= 0x7E);
}
static int has_ctl_or_crlf(const char *s, size_t n) {
    size_t i;
    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c < 0x20 || c == 0x7F || c == '\r' || c == '\n')
            return 1;
    }
    return 0;
}

/* base64url encode n bytes -> JS string (no padding). */
static JSValue b64url_new(JSContext *ctx, const uint8_t *data, size_t n) {
    char *out = (char *)js_malloc(ctx, 4 * ((n + 2) / 3) + 1);
    size_t m;
    JSValue r;
    if (!out) return JS_ThrowOutOfMemory(ctx);
    m = dyn_codec_base64url_encode(data, n, out);
    r = JS_NewStringLen(ctx, out, m);
    js_free(ctx, out);
    return r;
}

/* The x-www-form-urlencoded codec: the SHARED core (src/core/dyn-pct.h)
 * with a plain-buffer sink -- this was a byte-identical copy of
 * dyna-url.c's form=1 encode / plus_space decode (round-8 census), and
 * copies drift. */
static char *form_encode_component(JSContext *ctx, const char *s, size_t n, size_t *out_len) {
    /* worst: 3*n */
    char *out = (char *)js_malloc(ctx, n * 3 + 1);
    dyn_pct_buf_t b;
    if (!out) return NULL;
    b.p = out; b.n = 0;
    dyn_pct_encode_core(&b, dyn_pct_buf_sink, s, n, 1);
    out[b.n] = '\0';
    if (out_len) *out_len = b.n;
    return out;
}
static void form_pct_decode(char *out, size_t *out_len, const char *s, size_t n) {
    dyn_pct_buf_t b = { out, 0 };
    dyn_pct_decode_core(&b, dyn_pct_buf_sink, s, n, 1);
    out[b.n] = '\0';
    if (out_len) *out_len = b.n;
}

/* ---- PKCE ---------------------------------------------------------- */

/* generateCodeVerifier(): 32 octets CSPRNG -> base64url 43 chars (7636 §4.1) */
static JSValue oauth_gen_verifier(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    uint8_t raw[32];
    (void)this_val; (void)argc; (void)argv;
    if (dyn_os_entropy(raw, sizeof raw) < 0)
        return JS_ThrowInternalError(ctx, "generateCodeVerifier: OS entropy unavailable");
    return b64url_new(ctx, raw, sizeof raw);
}

/* isValidCodeVerifier(s): 43*128unreserved */
static JSValue oauth_is_valid_verifier(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    const char *s; size_t n, i;
    (void)this_val;
    if (argc < 1 || !JS_IsString(argv[0])) return JS_FALSE;
    s = JS_ToCStringLen(ctx, &n, argv[0]);
    if (!s) return JS_EXCEPTION;
    if (n < 43 || n > 128) { JS_FreeCString(ctx, s); return JS_FALSE; }
    for (i = 0; i < n; i++) if (!is_unreserved((unsigned char)s[i])) { JS_FreeCString(ctx, s); return JS_FALSE; }
    JS_FreeCString(ctx, s);
    return JS_TRUE;
}

/* generateCodeChallenge(verifier, method="S256"): S256 | plain */
static JSValue oauth_gen_challenge(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    const char *ver = NULL, *meth = NULL;
    size_t vn = 0;
    int is_s256 = 1;
    JSValue r = JS_EXCEPTION;
    (void)this_val;
    if (argc < 1) return JS_ThrowTypeError(ctx, "generateCodeChallenge(verifier[, method])");
    ver = JS_ToCStringLen(ctx, &vn, argv[0]);
    if (!ver) return JS_EXCEPTION;
    if (vn < 43 || vn > 128) { JS_FreeCString(ctx, ver); return JS_ThrowRangeError(ctx, "code_verifier must be 43..128 unreserved chars"); }
    { size_t i; for (i = 0; i < vn; i++) if (!is_unreserved((unsigned char)ver[i])) { JS_FreeCString(ctx, ver); return JS_ThrowTypeError(ctx, "code_verifier charset must be unreserved"); } }
    if (argc > 1 && JS_IsString(argv[1])) {
        meth = JS_ToCString(ctx, argv[1]);
        if (!meth) { JS_FreeCString(ctx, ver); return JS_EXCEPTION; }
        if (!strcmp(meth, "plain")) is_s256 = 0;
        else if (!strcmp(meth, "S256")) is_s256 = 1;
        else { JS_FreeCString(ctx, ver); JS_FreeCString(ctx, meth); return JS_ThrowTypeError(ctx, "code_challenge_method must be S256 or plain"); }
    }
    if (is_s256) {
        uint8_t digest[32];
        dyn_sha256((const uint8_t *)ver, vn, digest);
        r = b64url_new(ctx, digest, 32);
    } else {
        r = JS_NewStringLen(ctx, ver, vn);
    }
    JS_FreeCString(ctx, ver);
    if (meth) JS_FreeCString(ctx, meth);
    return r;
}

static JSValue oauth_verify_challenge(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    const char *ver = NULL, *chal = NULL, *meth = NULL;
    size_t vn = 0, cn = 0;
    int is_s256 = 1, ok = 0;
    (void)this_val;
    if (argc < 3) return JS_ThrowTypeError(ctx, "verifyCodeChallenge(verifier, challenge, method)");
    ver = JS_ToCStringLen(ctx, &vn, argv[0]);
    if (!ver) return JS_EXCEPTION;
    chal = JS_ToCStringLen(ctx, &cn, argv[1]);
    if (!chal) { JS_FreeCString(ctx, ver); return JS_EXCEPTION; }
    meth = JS_ToCString(ctx, argv[2]);
    if (!meth) { JS_FreeCString(ctx, ver); JS_FreeCString(ctx, chal); return JS_EXCEPTION; }
    if (!strcmp(meth, "plain")) is_s256 = 0;
    else if (!strcmp(meth, "S256")) is_s256 = 1;
    else { JS_FreeCString(ctx, ver); JS_FreeCString(ctx, chal); JS_FreeCString(ctx, meth); return JS_ThrowTypeError(ctx, "method must be S256 or plain"); }
    if (is_s256) {
        uint8_t digest[32];
        JSValue enc;
        const char *es; size_t el;
        dyn_sha256((const uint8_t *)ver, vn, digest);
        enc = b64url_new(ctx, digest, 32);          /* the shared helper, not a re-inlined encode */
        if (JS_IsException(enc)) { JS_FreeCString(ctx, ver); JS_FreeCString(ctx, chal); JS_FreeCString(ctx, meth); return JS_EXCEPTION; }
        es = JS_ToCStringLen(ctx, &el, enc);
        JS_FreeValue(ctx, enc);
        if (!es) { JS_FreeCString(ctx, ver); JS_FreeCString(ctx, chal); JS_FreeCString(ctx, meth); return JS_EXCEPTION; }
        ok = (el == cn && dyn_ct_equal((const uint8_t *)es, (const uint8_t *)chal, el));
        JS_FreeCString(ctx, es);
    } else {
        ok = (vn == cn && dyn_ct_equal((const uint8_t *)ver, (const uint8_t *)chal, vn));
    }
    JS_FreeCString(ctx, ver); JS_FreeCString(ctx, chal); JS_FreeCString(ctx, meth);
    return JS_NewBool(ctx, ok);
}

/* ---- state / secureCompare ----------------------------------------- */

static JSValue oauth_gen_state(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    int64_t n = 32;
    uint8_t *buf;
    JSValue r;
    (void)this_val;
    if (argc > 0 && !JS_IsUndefined(argv[0])) {
        if (JS_ToInt64(ctx, &n, argv[0])) return JS_EXCEPTION;
    }
    if (n < 1 || n > 256) return JS_ThrowRangeError(ctx, "generateState: bytes must be 1..256");
    buf = (uint8_t *)js_malloc(ctx, (size_t)n);
    if (!buf) return JS_ThrowOutOfMemory(ctx);
    if (dyn_os_entropy(buf, (size_t)n) < 0) { js_free(ctx, buf); return JS_ThrowInternalError(ctx, "OS entropy unavailable"); }
    r = b64url_new(ctx, buf, (size_t)n);
    js_free(ctx, buf);
    return r;
}

/* BytesInput: string -> UTF-8, ByteView -> raw bytes. Constant-time over bytes. */
static int oauth_bytes(JSContext *ctx, JSValueConst v, const uint8_t **pdata, size_t *plen, const char **powned, JSValue *pkeep) {
    *powned = NULL; *pkeep = JS_UNDEFINED;
    if (JS_IsString(v)) {
        size_t n; const char *s = JS_ToCStringLen(ctx, &n, v);
        if (!s) return -1;
        *powned = s; *pdata = (const uint8_t *)s; *plen = n; return 0;
    }
    {
        size_t off=0, len=0, bpe=0, ab=0;
        JSValue abv = JS_GetArrayBufferView(ctx, v, &off, &len, &bpe);
        if (!JS_IsException(abv)) {
            uint8_t *base;
            if (bpe != 1) { JS_FreeValue(ctx, abv); JS_ThrowTypeError(ctx, "secureCompare: ByteView must be 1-byte (Uint8Array/DataView)"); return -1; }
            base = JS_GetArrayBuffer(ctx, &ab, abv);
            JS_FreeValue(ctx, abv);
            if (!base) return -1;
            if (off > ab || len > ab - off) { JS_ThrowRangeError(ctx, "view out of bounds"); return -1; }
            /* keep the ArrayBuffer alive for the call */
            *pkeep = JS_DupValue(ctx, v);
            *pdata = base + off; *plen = len; return 0;
        }
        JS_FreeValue(ctx, JS_GetException(ctx));
        {
            size_t n; uint8_t *p = JS_GetArrayBuffer(ctx, &n, v);
            if (p) { *pkeep = JS_DupValue(ctx, v); *pdata = p; *plen = n; return 0; }
            JS_FreeValue(ctx, JS_GetException(ctx));
        }
    }
    /* fallback: ToString then UTF-8 */
    {
        size_t n; const char *s = JS_ToCStringLen(ctx, &n, v);
        if (!s) return -1;
        *powned = s; *pdata = (const uint8_t *)s; *plen = n; return 0;
    }
}
static JSValue oauth_secure_compare(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    const uint8_t *a=NULL,*b=NULL; size_t al=0,bl=0; const char *ao=NULL,*bo=NULL; JSValue ak=JS_UNDEFINED,bk=JS_UNDEFINED;
    uint8_t *acopy=NULL;
    int eq=0;
    (void)this_val;
    if (argc < 2) return JS_ThrowTypeError(ctx, "secureCompare(a, b): two BytesInput required");
    if (oauth_bytes(ctx, argv[0], &a, &al, &ao, &ak) < 0) return JS_EXCEPTION;
    /* Coercing the SECOND argument can run user JS (a plain object's
       toString), which can detach or overwrite the first argument's buffer.
       Copy arg0's bytes before that can happen -- the same defense
       dyna:crypto's timingSafeEqual applies. */
    acopy = (uint8_t *)js_malloc(ctx, al ? al : 1);
    if (!acopy) { if(ao) JS_FreeCString(ctx,ao); JS_FreeValue(ctx,ak); return JS_ThrowOutOfMemory(ctx); }
    if (al) memcpy(acopy, a, al);
    if(ao) JS_FreeCString(ctx,ao);
    JS_FreeValue(ctx,ak);
    ak = JS_UNDEFINED;
    if (oauth_bytes(ctx, argv[1], &b, &bl, &bo, &bk) < 0) { js_free(ctx,acopy); return JS_EXCEPTION; }
    eq = (al == bl) && dyn_ct_equal(acopy, b, al);
    js_free(ctx,acopy);
    if(bo) JS_FreeCString(ctx,bo);
    JS_FreeValue(ctx,bk);
    return JS_NewBool(ctx, eq);
}

/* ---- scope ---------------------------------------------------------- */

static JSValue oauth_parse_scope(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    const char *s; size_t n, i, start = 0;
    JSValue arr;
    uint32_t idx = 0;
    (void)this_val;
    if (argc < 1 || !JS_IsString(argv[0])) return JS_ThrowTypeError(ctx, "parseScope(scope): string required");
    s = JS_ToCStringLen(ctx, &n, argv[0]);
    if (!s) return JS_EXCEPTION;
    if (n > 4096) { JS_FreeCString(ctx, s); return JS_ThrowRangeError(ctx, "scope exceeds 4096 bytes"); }
    if (n > 0 && (s[0]==' ' || s[n-1]==' ')) { JS_FreeCString(ctx, s); return JS_ThrowTypeError(ctx, "scope: leading/trailing space"); }
    for (i = 0; i <= n; i++) {
        int is_end = (i == n);
        int is_sp = (!is_end && s[i] == ' ');
        if (is_end || is_sp) {
            size_t len = i - start;
            if (len == 0 && !is_end) { /* consecutive spaces */ JS_FreeCString(ctx, s); return JS_ThrowTypeError(ctx, "scope: empty token"); }
            if (len) {
                size_t k; for (k = start; k < i; k++) if (!is_nqchar((unsigned char)s[k])) { JS_FreeCString(ctx, s); return JS_ThrowTypeError(ctx, "scope token contains invalid char"); }
            }
            start = i + 1;
        }
    }
    arr = JS_NewArray(ctx);
    if (JS_IsException(arr)) { JS_FreeCString(ctx, s); return arr; }
    start = 0;
    for (i = 0; i <= n; i++) {
        int is_end = (i == n);
        int is_sp = (!is_end && s[i] == ' ');
        if (is_end || is_sp) {
            size_t len = i - start;
            if (len) {
                JS_SetPropertyUint32(ctx, arr, idx++, JS_NewStringLen(ctx, s + start, len));
            }
            start = i + 1;
        }
    }
    JS_FreeCString(ctx, s);
    return arr;
}
static JSValue oauth_format_scope(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    JSValue lenv;
    uint32_t n = 0, i;
    size_t tot = 0;
    char *out;
    JSValue r;
    (void)this_val;
    if (argc < 1 || !JS_IsArray(ctx, argv[0])) return JS_ThrowTypeError(ctx, "formatScope(scopes: string[])");
    lenv = JS_GetPropertyStr(ctx, argv[0], "length");
    JS_ToUint32(ctx, &n, lenv); JS_FreeValue(ctx, lenv);
    for (i = 0; i < n; i++) {
        JSValue v = JS_GetPropertyUint32(ctx, argv[0], i);
        const char *s; size_t sl;
        if (!JS_IsString(v)) { JS_FreeValue(ctx, v); return JS_ThrowTypeError(ctx, "scope element must be string"); }
        s = JS_ToCStringLen(ctx, &sl, v); JS_FreeValue(ctx, v);
        if (!s) return JS_EXCEPTION;
        if (sl == 0 || sl > 64) { JS_FreeCString(ctx, s); return JS_ThrowRangeError(ctx, "scope token length 1..64"); }
        { size_t k; for (k = 0; k < sl; k++) if (!is_nqchar((unsigned char)s[k])) { JS_FreeCString(ctx, s); return JS_ThrowTypeError(ctx, "scope token invalid char"); } }
        tot += sl + 1;
        JS_FreeCString(ctx, s);
    }
    if (tot == 0) return JS_NewString(ctx, "");
    out = (char *)js_malloc(ctx, tot);
    if (!out) return JS_ThrowOutOfMemory(ctx);
    {
        size_t off = 0;
        for (i = 0; i < n; i++) {
            JSValue v = JS_GetPropertyUint32(ctx, argv[0], i);
            const char *s; size_t sl;
            s = JS_ToCStringLen(ctx, &sl, v); JS_FreeValue(ctx, v);
            if (off) out[off++] = ' ';
            memcpy(out + off, s, sl); off += sl;
            JS_FreeCString(ctx, s);
        }
        out[off] = '\0';
        r = JS_NewStringLen(ctx, out, off);
    }
    js_free(ctx, out);
    return r;
}

/* ---- bearer --------------------------------------------------------- */

static int is_valid_b64token(const char *s, size_t n) {
    size_t pad = 0;
    if (n == 0 || n > 4096) return 0;
    while (pad < n && s[n-1-pad] == '=') pad++;
    if (pad > 2) return 0;
    if (n - pad == 0) return 0; /* need at least one data char */
    for (size_t i = 0; i < n - pad; i++) if (!is_b64token_char((unsigned char)s[i])) return 0;
    return 1;
}

static JSValue oauth_build_bearer(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    const char *t; size_t n;
    char *out;
    JSValue r;
    (void)this_val;
    if (argc < 1 || !JS_IsString(argv[0])) return JS_ThrowTypeError(ctx, "buildBearerHeader(token: string)");
    t = JS_ToCStringLen(ctx, &n, argv[0]);
    if (!t) return JS_EXCEPTION;
    if (!is_valid_b64token(t, n) || has_ctl_or_crlf(t, n)) { JS_FreeCString(ctx, t); return JS_ThrowTypeError(ctx, "buildBearerHeader: token must be b64token (ALPHA/DIGIT/-._~+/ and = padding)"); }
    out = (char *)js_malloc(ctx, 7 + n + 1);
    if (!out) { JS_FreeCString(ctx, t); return JS_ThrowOutOfMemory(ctx); }
    memcpy(out, "Bearer ", 7);
    memcpy(out + 7, t, n); out[7+n] = '\0';
    JS_FreeCString(ctx, t);
    r = JS_NewStringLen(ctx, out, 7 + n);
    js_free(ctx, out);
    return r;
}
static JSValue oauth_parse_bearer(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    const char *h; size_t n, i = 0;
    JSValue r;
    (void)this_val;
    if (argc < 1 || !JS_IsString(argv[0])) return JS_NULL;
    h = JS_ToCStringLen(ctx, &n, argv[0]);
    if (!h) return JS_EXCEPTION;
    /* case-insensitive "Bearer" + 1*SP + b64token */
    while (i < n && (h[i] == ' ' || h[i] == '\t')) i++;
    if (n - i < 6) { JS_FreeCString(ctx, h); return JS_NULL; }
    if ((h[i]!='B'&&h[i]!='b')||(h[i+1]!='e'&&h[i+1]!='E')||(h[i+2]!='a'&&h[i+2]!='A')||(h[i+3]!='r'&&h[i+3]!='R')||(h[i+4]!='e'&&h[i+4]!='E')||(h[i+5]!='r'&&h[i+5]!='R')) { JS_FreeCString(ctx,h); return JS_NULL; }
    i += 6;
    if (i >= n || (h[i] != ' ' && h[i] != '\t')) { JS_FreeCString(ctx,h); return JS_NULL; }
    while (i < n && (h[i]==' '||h[i]=='\t')) i++;
    if (i >= n) { JS_FreeCString(ctx,h); return JS_NULL; }
    {
        size_t tok = n - i;
        /* trim trailing OWS */
        while (tok && (h[i+tok-1]==' '||h[i+tok-1]=='\t')) tok--;
        if (!is_valid_b64token(h+i, tok)) { JS_FreeCString(ctx,h); return JS_NULL; }
        r = JS_NewStringLen(ctx, h+i, tok);
    }
    JS_FreeCString(ctx,h);
    return r;
}

static JSValue oauth_parse_bearer_from_req(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    JSValue v;
    const char *s; size_t n;
    int allowQuery = 0, allowBody = 0;
    int found = 0;
    JSValue foundVal = JS_NULL;
    (void)this_val;
    if (argc < 1 || !JS_IsObject(argv[0])) return JS_ThrowTypeError(ctx, "parseBearerFromRequest(req: {headers, query, body})");
    v = JS_GetPropertyStr(ctx, argv[0], "allowQuery");
    allowQuery = JS_ToBool(ctx, v); JS_FreeValue(ctx, v);
    v = JS_GetPropertyStr(ctx, argv[0], "allowBody");
    allowBody = JS_ToBool(ctx, v); JS_FreeValue(ctx, v);
    /* headers.Authorization or headers.authorization (case-insensitive) */
    v = JS_GetPropertyStr(ctx, argv[0], "headers");
    if (JS_IsObject(v)) {
        JSValue hv = JS_GetPropertyStr(ctx, v, "authorization");
        if (JS_IsUndefined(hv)) { JS_FreeValue(ctx, hv); hv = JS_GetPropertyStr(ctx, v, "Authorization"); }
        if (JS_IsString(hv)) {
            JSValue p = oauth_parse_bearer(ctx, JS_UNDEFINED, 1, &hv);
            if (!JS_IsNull(p) && !JS_IsException(p)) { found++; foundVal = p; }
            else JS_FreeValue(ctx, p);
        }
        JS_FreeValue(ctx, hv);
    }
    JS_FreeValue(ctx, v);
    if (allowBody) {
        v = JS_GetPropertyStr(ctx, argv[0], "body");
        if (JS_IsString(v)) {
            s = JS_ToCStringLen(ctx, &n, v);
            if (s) {
                size_t i = 0;
                while (i < n) {
                    size_t b = i, eq = (size_t)-1;
                    while (i < n && s[i] != '&') { if (s[i]=='=' && eq==(size_t)-1) eq = i; i++; }
                    if (eq != (size_t)-1 && i - b > 13 && !memcmp(s+b, "access_token", 12) && s[b+12]=='=') {
                        size_t vl = i - eq - 1;
                        char *dec = (char *)js_malloc(ctx, vl+1);
                        size_t dl;
                        if (dec) {
                            form_pct_decode(dec, &dl, s+eq+1, vl);
                            if (is_valid_b64token(dec, dl)) {
                                if (found) { js_free(ctx, dec); JS_FreeCString(ctx, s); JS_FreeValue(ctx, v); JS_FreeValue(ctx, foundVal); return JS_ThrowTypeError(ctx, "parseBearerFromRequest: must not use more than one method (header + body)"); }
                                found++; foundVal = JS_NewStringLen(ctx, dec, dl);
                            }
                            js_free(ctx, dec);
                        }
                    }
                    if (i < n && s[i]=='&') i++;
                }
                JS_FreeCString(ctx, s);
            }
        }
        JS_FreeValue(ctx, v);
    }
    if (allowQuery) {
        v = JS_GetPropertyStr(ctx, argv[0], "query");
        if (JS_IsString(v)) {
            s = JS_ToCStringLen(ctx, &n, v);
            if (s) {
                size_t i=0;
                const char *qs = s; size_t qn = n;
                if (qn && qs[0]=='?') { qs++; qn--; }
                while (i < qn) {
                    size_t b=i, eq=(size_t)-1;
                    while (i<qn && qs[i]!='&'){ if(qs[i]=='='&&eq==(size_t)-1) eq=i; i++; }
                    if (eq!=(size_t)-1 && i-b>13 && !memcmp(qs+b,"access_token",12) && qs[b+12]=='=') {
                        size_t vl=i-eq-1;
                        char *dec=(char*)js_malloc(ctx, vl+1);
                        size_t dl;
                        if(dec){ form_pct_decode(dec,&dl,qs+eq+1,vl); if(is_valid_b64token(dec,dl)) { if(found){ js_free(ctx,dec); JS_FreeCString(ctx,s); JS_FreeValue(ctx,v); JS_FreeValue(ctx,foundVal); return JS_ThrowTypeError(ctx,"parseBearerFromRequest: must not use more than one method (header/query/body)"); } found++; foundVal=JS_NewStringLen(ctx,dec,dl); } js_free(ctx,dec); }
                    }
                    if(i<qn && qs[i]=='&') i++;
                }
                JS_FreeCString(ctx,s);
            }
        }
        JS_FreeValue(ctx,v);
    }
    if (found) return foundVal;
    return JS_NULL;
}

/* ---- WWW-Authenticate ----------------------------------------------- */

static int is_token68_char(unsigned char c) { return is_b64token_char(c); }
static int is_valid_error(const char *s,size_t n){
    size_t i;
    if(n==0||n>64) return 0;
    for(i=0;i<n;i++){ unsigned char c=(unsigned char)s[i]; if(!is_token68_char(c)) return 0; }
    return 1;
}
static int is_valid_error_desc(const char *s,size_t n){
    size_t i;
    for(i=0;i<n;i++){ unsigned char c=(unsigned char)s[i]; if(c==0x22||c==0x5C||c<0x20||c>0x7E) return 0; }
    return 1;
}
static JSValue oauth_build_www(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv){
    JSValue opts=JS_UNDEFINED, v;
    const char *realm=NULL,*error=NULL,*desc=NULL,*uri=NULL,*scope=NULL;
    size_t rn=0,en=0,dn=0,un=0,sn=0;
    char *out;
    size_t cap=512;
    size_t off=0;
    JSValue r;
    (void)this_val;
    if(argc<1 || !JS_IsObject(argv[0])) return JS_ThrowTypeError(ctx,"buildWWWAuthenticate({realm, error, ...})");
    opts=argv[0];
    v=JS_GetPropertyStr(ctx, opts,"realm"); if(JS_IsString(v)){ realm=JS_ToCStringLen(ctx,&rn,v);} JS_FreeValue(ctx,v);
    v=JS_GetPropertyStr(ctx, opts,"error"); if(JS_IsString(v)){ error=JS_ToCStringLen(ctx,&en,v);} JS_FreeValue(ctx,v);
    v=JS_GetPropertyStr(ctx, opts,"errorDescription"); if(JS_IsUndefined(v)){ JS_FreeValue(ctx,v); v=JS_GetPropertyStr(ctx, opts,"error_description"); } if(JS_IsString(v)){ desc=JS_ToCStringLen(ctx,&dn,v);} JS_FreeValue(ctx,v);
    v=JS_GetPropertyStr(ctx, opts,"errorUri"); if(JS_IsUndefined(v)){ JS_FreeValue(ctx,v); v=JS_GetPropertyStr(ctx, opts,"error_uri"); } if(JS_IsString(v)){ uri=JS_ToCStringLen(ctx,&un,v);} JS_FreeValue(ctx,v);
    v=JS_GetPropertyStr(ctx, opts,"scope"); if(JS_IsString(v)){ scope=JS_ToCStringLen(ctx,&sn,v);} JS_FreeValue(ctx,v);
    /* realm and error_uri are emitted inside a quoted-string: the desc rule
       (no '"', no '\', no CTL, printable ASCII) is what keeps a value from
       breaking out and injecting extra auth-params. has_ctl_or_crlf alone
       allowed realm:'a", error="fake' to forge parameters. */
    if(realm && !is_valid_error_desc(realm,rn)) goto bad;
    if(error && !is_valid_error(error,en)) goto bad;
    if(desc && !is_valid_error_desc(desc,dn)) goto bad;
    if(uri && !is_valid_error_desc(uri,un)) goto bad;
    if(scope){ size_t i; for(i=0;i<sn;i++) if(!is_nqchar((unsigned char)scope[i]) && scope[i]!=' ') goto bad; }
    /* RFC 6750 §3: error is REQUIRED in a WWW-Authenticate challenge. A
       header without it is malformed and every client will refuse it. */
    if(!error) goto need_error;
    /* cap must hold realm+error+desc+uri+scope with ALL their fixed bytes:
       " realm=\""+'"' is 9 beyond the value, ", error=\""+'"' is 10,
       ", error_description=\""+'"' is 22, ", error_uri=\""+'"' is 14 and
       ", scope=\""+'"' is 10; the 7 covers "Bearer" plus the NUL. */
    {
        size_t need = 7 + (rn? rn+9:0) + (en? en+11:0) + (dn? dn+22:0)
                    + (un? un+14:0) + (sn? sn+10:0);
        if(need > cap){ cap = need; }
    }
    out=(char*)js_malloc(ctx, cap);
    if(!out) goto oom;
    /* snprintf returns the UNTRUNCATED length, so off can pass cap on a
       truncation and cap-off below would wrap to a huge size_t: clamp after
       every write. */
    #define WWW_CLAMP() do{ if(off >= cap) off = cap-1; }while(0)
    off+= snprintf(out+off, cap-off, "Bearer"); WWW_CLAMP();
    if(realm){ off+= snprintf(out+off, cap-off, " realm=\"%.*s\"", (int)rn, realm); WWW_CLAMP(); }
    if(error){ off+= snprintf(out+off, cap-off, "%s error=\"%.*s\"", realm?",":"", (int)en, error); WWW_CLAMP(); }
    if(desc){ off+= snprintf(out+off, cap-off, ", error_description=\"%.*s\"", (int)dn, desc); WWW_CLAMP(); }
    if(uri){ off+= snprintf(out+off, cap-off, ", error_uri=\"%.*s\"", (int)un, uri); WWW_CLAMP(); }
    if(scope){ off+= snprintf(out+off, cap-off, ", scope=\"%.*s\"", (int)sn, scope); WWW_CLAMP(); }
    #undef WWW_CLAMP
    if(off >= cap) off = cap-1;
    r=JS_NewStringLen(ctx,out,off);
    js_free(ctx,out);
    if(realm) JS_FreeCString(ctx,realm);
    if(error) JS_FreeCString(ctx,error);
    if(desc) JS_FreeCString(ctx,desc);
    if(uri) JS_FreeCString(ctx,uri);
    if(scope) JS_FreeCString(ctx,scope);
    return r;
bad:
    if(realm) JS_FreeCString(ctx,realm);
    if(error) JS_FreeCString(ctx,error);
    if(desc) JS_FreeCString(ctx,desc);
    if(uri) JS_FreeCString(ctx,uri);
    if(scope) JS_FreeCString(ctx,scope);
    return JS_ThrowTypeError(ctx,"buildWWWAuthenticate: invalid chars in realm/error/description/uri/scope");
need_error:
    if(realm) JS_FreeCString(ctx,realm);
    if(error) JS_FreeCString(ctx,error);
    if(desc) JS_FreeCString(ctx,desc);
    if(uri) JS_FreeCString(ctx,uri);
    if(scope) JS_FreeCString(ctx,scope);
    return JS_ThrowTypeError(ctx,"buildWWWAuthenticate: error is required (RFC 6750 §3)");
oom:
    if(realm) JS_FreeCString(ctx,realm);
    if(error) JS_FreeCString(ctx,error);
    if(desc) JS_FreeCString(ctx,desc);
    if(uri) JS_FreeCString(ctx,uri);
    if(scope) JS_FreeCString(ctx,scope);
    return JS_ThrowOutOfMemory(ctx);
}

/* ---- isValidBearerToken --------------------------------------------- */
static JSValue oauth_is_valid_bearer(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv){
    const char *s; size_t n; int ok;
    (void)this_val;
    if(argc<1 || !JS_IsString(argv[0])) return JS_FALSE;
    s=JS_ToCStringLen(ctx,&n,argv[0]); if(!s) return JS_EXCEPTION;
    ok = is_valid_b64token(s,n);
    JS_FreeCString(ctx,s);
    return JS_NewBool(ctx, ok);
}

/* ---- redirect uri validation ---------------------------------------- */
/* verbatim exact except loopback port-ignore (8252 §7.3). Also check private-use scheme contains '.' */
static int loopback_prefix(const char *s,size_t n, const char *pfx){
    size_t pl=strlen(pfx);
    return n>=pl && !memcmp(s,pfx,pl);
}
static int is_loopback_candidate(const char *s,size_t n){
    return loopback_prefix(s,n,"http://127.0.0.1") || loopback_prefix(s,n,"http://[::1]") || loopback_prefix(s,n,"http://localhost");
}
/* Parse an authority span into its host (including any IPv6 brackets) and
 * port. The port is IGNORED only when it is empty or digits-only (8252 §7.3);
 * anything else -- userinfo '@', a malformed bracket, a non-digit port -- is
 * a mismatch, never a silent host rewrite. Without the '@' rejection,
 * "127.0.0.1:8080@evil.com" parses as host "127.0.0.1" plus a discarded
 * ":8080@evil.com" and forges any loopback registration. */
static int split_host_port(const char *auth,size_t alen, const char **host,size_t *hlen){
    const char *p, *end = auth + alen;
    if(memchr(auth,'@',alen)) return 0;          /* userinfo: never equal */
    if(alen && auth[0]=='['){
        const char *ce=memchr(auth,']',alen);
        if(!ce) return 0;                        /* unterminated IPv6 literal */
        *host=auth; *hlen=(size_t)(ce-auth)+1;   /* brackets are atomic */
        p=ce+1;
    } else {
        p=memchr(auth,':',alen);
        if(!p){ *host=auth; *hlen=alen; return 1; }
        *host=auth; *hlen=(size_t)(p-auth);
    }
    if(p==end) return 1;                         /* no port at all */
    if(*p!=':') return 0;                        /* junk after the host */
    for(p++; p<end; p++)
        if(*p<'0'||*p>'9') return 0;             /* port must be *DIGIT */
    return 1;
}
static int host_match_ignore_port(const char *a,size_t an, const char *b,size_t bn){
    const char *ap=strstr(a,"://");
    const char *bp=strstr(b,"://");
    const char *ae,*be,*ahost,*bhost;
    size_t alen, blen, ah, bh;
    if(!ap||!bp) return 0;
    ap+=3; bp+=3;
    ae=memchr(ap,'/',an-(size_t)(ap-a));
    be=memchr(bp,'/',bn-(size_t)(bp-b));
    alen= ae? (size_t)(ae-ap): an-(size_t)(ap-a);
    blen= be? (size_t)(be-bp): bn-(size_t)(bp-b);
    if(!split_host_port(ap,alen,&ahost,&ah)) return 0;
    if(!split_host_port(bp,blen,&bhost,&bh)) return 0;
    if(ah!=bh || memcmp(ahost,bhost,ah)) return 0;
    const char *ap_path= ae? ae: a+an;
    const char *bp_path= be? be: b+bn;
    size_t ap_l = (size_t)(a+an - ap_path);
    size_t bp_l = (size_t)(b+bn - bp_path);
    return ap_l==bp_l && !memcmp(ap_path,bp_path,ap_l);
}
static JSValue oauth_is_valid_redirect(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv){
    const char *cand; size_t cn;
    JSValue lenv;
    uint32_t n=0,i;
    int ok=0;
    (void)this_val;
    if(argc<2 || !JS_IsString(argv[0])) return JS_ThrowTypeError(ctx,"isValidRedirectUri(candidate, registered[])");
    if(!JS_IsArray(ctx, argv[1])) return JS_ThrowTypeError(ctx,"registered must be string[]");
    cand=JS_ToCStringLen(ctx,&cn,argv[0]); if(!cand) return JS_EXCEPTION;
    if(has_ctl_or_crlf(cand,cn)){ JS_FreeCString(ctx,cand); return JS_FALSE; }
    /* private-use scheme check: if scheme is not http/https, require '.' in scheme (8252 §7.1) */
    {
        const char *col=strchr(cand,':');
        if(col){ size_t sl=(size_t)(col-cand); int has_dot=0; for(size_t k=0;k<sl;k++) if(cand[k]=='.') has_dot=1; if(!has_dot && strncmp(cand,"http:",5)!=0 && strncmp(cand,"https:",6)!=0) { JS_FreeCString(ctx,cand); return JS_FALSE; } }
    }
    lenv=JS_GetPropertyStr(ctx, argv[1],"length"); JS_ToUint32(ctx,&n,lenv); JS_FreeValue(ctx,lenv);
    for(i=0;i<n;i++){
        JSValue e=JS_GetPropertyUint32(ctx,argv[1],i);
        const char *reg; size_t rn;
        if(!JS_IsString(e)){ JS_FreeValue(ctx,e); continue; }
        reg=JS_ToCStringLen(ctx,&rn,e); JS_FreeValue(ctx,e);
        if(!reg) { JS_FreeCString(ctx,cand); return JS_EXCEPTION; }
        if(cn==rn && !memcmp(cand,reg,cn)) ok=1;
        else if(is_loopback_candidate(cand,cn) && is_loopback_candidate(reg,rn) && host_match_ignore_port(cand,cn,reg,rn)) ok=1;
        JS_FreeCString(ctx,reg);
        if(ok) break;
    }
    JS_FreeCString(ctx,cand);
    return JS_NewBool(ctx, ok);
}

/* ---- buildAuthorizationUrl ------------------------------------------ */
static JSValue oauth_build_auth_url(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv){
    JSValue opts, v;
    const char *endpoint=NULL,*clientId=NULL,*redirectUri=NULL,*scope=NULL,*state=NULL,*cc=NULL,*ccm=NULL;
    size_t en=0,cn=0,rn=0,sn=0,scn=0,ccn=0,ccmn=0;
    int has_code_challenge=0;
    int ccm_is_malloc=0;
    char *url=NULL;
    size_t url_cap=4096, url_len=0;
    JSValue r=JS_EXCEPTION;
    (void)this_val;
    if(argc<1 || !JS_IsObject(argv[0])) return JS_ThrowTypeError(ctx,"buildAuthorizationUrl(opts: {authorizationEndpoint, clientId, redirectUri})");
    opts=argv[0];
    v=JS_GetPropertyStr(ctx, opts,"authorizationEndpoint"); if(JS_IsString(v)) endpoint=JS_ToCStringLen(ctx,&en,v); JS_FreeValue(ctx,v);
    v=JS_GetPropertyStr(ctx, opts,"clientId"); if(JS_IsString(v)) clientId=JS_ToCStringLen(ctx,&cn,v); JS_FreeValue(ctx,v);
    v=JS_GetPropertyStr(ctx, opts,"redirectUri"); if(JS_IsString(v)) redirectUri=JS_ToCStringLen(ctx,&rn,v); JS_FreeValue(ctx,v);
    if(!endpoint||!clientId||!redirectUri || en==0 || cn==0 || rn==0){ goto fail; }
    if(has_ctl_or_crlf(endpoint,en)||has_ctl_or_crlf(clientId,cn)||has_ctl_or_crlf(redirectUri,rn)) goto bad;
    {
        JSValue av=JS_GetPropertyStr(ctx, opts,"allowInsecure");
        int allowInsecure = JS_ToBool(ctx, av); JS_FreeValue(ctx, av);
        if(!allowInsecure){
            int is_https = (en>=8 && !memcmp(endpoint,"https://",8));
            int is_loopback = (rn>=22 && (!memcmp(redirectUri,"http://127.0.0.1",16) || !memcmp(redirectUri,"http://[::1]",12) || !memcmp(redirectUri,"http://localhost",16)));
            if(!is_https && !is_loopback){ r=JS_ThrowTypeError(ctx,"buildAuthorizationUrl: authorizationEndpoint must be https (except loopback); pass allowInsecure:true to override"); goto cleanup; }
        }
    }
    v=JS_GetPropertyStr(ctx, opts,"scope"); if(JS_IsString(v)){ scope=JS_ToCStringLen(ctx,&sn,v);} JS_FreeValue(ctx,v);
    /* scope grammar (6749 A.4): scope-token *( SP scope-token ) -- no leading,
       trailing or consecutive space. Same rules parseScope enforces. */
    if(scope){ size_t i; int in_tok=0; if(sn==0 || scope[0]==' ' || scope[sn-1]==' ') goto bad; for(i=0;i<sn;i++){ unsigned char c=(unsigned char)scope[i]; if(c==' '){ if(!in_tok) goto bad; in_tok=0; } else { if(!is_nqchar(c)) goto bad; in_tok=1; } } }
    int state_is_malloc = 0;
    v=JS_GetPropertyStr(ctx, opts,"state"); if(JS_IsString(v)){ state=JS_ToCStringLen(ctx,&scn,v);} else if(JS_IsUndefined(v) || JS_IsNull(v)){
        /* generate state if not supplied (BCP 9700 CSRF). A request without
           state must not be built: entropy failure throws, never a silent
           state-less URL. */
        uint8_t sraw[32];
        if(dyn_os_entropy(sraw, sizeof sraw)!=0){ JS_FreeValue(ctx,v); r=JS_ThrowInternalError(ctx,"buildAuthorizationUrl: OS entropy unavailable"); goto cleanup; }
        {
            char tmp[64]; size_t tm=dyn_codec_base64url_encode(sraw, sizeof sraw, tmp);
            char *ms = (char*)js_malloc(ctx, tm+1);
            if(!ms){ JS_FreeValue(ctx,v); r=JS_ThrowOutOfMemory(ctx); goto cleanup; }
            memcpy(ms, tmp, tm); ms[tm]='\0'; state=ms; scn=tm; state_is_malloc=1;
        }
    }
    JS_FreeValue(ctx,v);
    v=JS_GetPropertyStr(ctx, opts,"codeChallenge"); if(JS_IsString(v)){ cc=JS_ToCStringLen(ctx,&ccn,v); has_code_challenge=1; } JS_FreeValue(ctx,v);
    v=JS_GetPropertyStr(ctx, opts,"codeChallengeMethod"); if(JS_IsString(v)){ ccm=JS_ToCStringLen(ctx,&ccmn,v);} JS_FreeValue(ctx,v);
    if(cc && ccm && strcmp(ccm,"S256") && strcmp(ccm,"plain")) goto bad;
    /* challenge shape per method (7636 §4.2): S256 is exactly 43 unreserved
       (base64url of 32 octets); plain is a verifier, 43..128 unreserved.
       Method absent defaults to S256 below, so NULL ccm means S256 here. */
    if(cc){
        size_t k; int chal_is_s256 = (ccm && !strcmp(ccm,"plain")) ? 0 : 1;
        if(chal_is_s256 ? (ccn != 43) : (ccn < 43 || ccn > 128)) goto bad;
        for(k=0;k<ccn;k++) if(!is_unreserved((unsigned char)cc[k])) goto bad;
    }
    /* S256 is mandatory; plain only if caller explicitly allows it; default to S256 when challenge without method */
    if(has_code_challenge && !ccm){
        ccm = (const char*)js_malloc(ctx, 5); if(!ccm) goto oom;
        memcpy((char*)ccm, "S256", 4); ((char*)ccm)[4]='\0'; ccmn=4;
        ccm_is_malloc=1;
    }
    if(cc && ccm && !strcmp(ccm,"plain")){
        JSValue av=JS_GetPropertyStr(ctx, opts,"allowPlain");
        int allow = JS_ToBool(ctx, av); JS_FreeValue(ctx, av);
        if(!allow) goto bad;
    }
    /* response_type must be code or absent */
    v=JS_GetPropertyStr(ctx, opts,"responseType"); if(JS_IsString(v)){ const char *rt; size_t rtn; rt=JS_ToCStringLen(ctx,&rtn,v); int ok = (rtn==4 && !memcmp(rt,"code",4)); JS_FreeCString(ctx,rt); JS_FreeValue(ctx,v); if(!ok) { r=JS_ThrowTypeError(ctx,"unsupported_response_type: only 'code' is supported"); goto cleanup; } } else JS_FreeValue(ctx,v);
    /* build ?response_type=code&client_id=... */
    url=(char*)js_malloc(ctx, en+1); if(!url) goto oom;
    memcpy(url, endpoint, en); url[en]='\0'; url_len=en; url_cap=en+1;
    {
        int has_q = (memchr(endpoint,'?',en)!=NULL);
        char *tmp; size_t tmpl;
        #define APPEND_PARAM(k,vn,v) do{ \
            tmp = form_encode_component(ctx, v, vn, &tmpl); \
            if(!tmp) goto oom; \
            size_t need = url_len + 1 + strlen(k) + 1 + tmpl + 1; \
            if(need > url_cap){ char*nb=(char*)js_realloc(ctx,url,need+256); if(!nb){js_free(ctx,tmp); goto oom;} url=nb; url_cap=need+256; } \
            url[url_len++] = has_q ? '&' : '?'; has_q=1; \
            memcpy(url+url_len, k, strlen(k)); url_len+=strlen(k); url[url_len++]='='; memcpy(url+url_len, tmp, tmpl); url_len+=tmpl; url[url_len]='\0'; js_free(ctx,tmp);}while(0)
        APPEND_PARAM("response_type",4,"code");
        APPEND_PARAM("client_id",cn,clientId);
        APPEND_PARAM("redirect_uri",rn,redirectUri);
        if(scope) APPEND_PARAM("scope",sn,scope);
        if(state) APPEND_PARAM("state",scn,state);
        if(has_code_challenge){ APPEND_PARAM("code_challenge",ccn,cc); if(ccm) APPEND_PARAM("code_challenge_method",ccmn,ccm); }
        /* extraParams */
        v=JS_GetPropertyStr(ctx, opts,"extraParams");
        if(JS_IsObject(v) && !JS_IsArray(ctx, v)){
            JSPropertyEnum *tab=NULL; uint32_t len=0;
            if(JS_GetOwnPropertyNames(ctx,&tab,&len,v,JS_GPN_STRING_MASK|JS_GPN_ENUM_ONLY)>=0){
                for(uint32_t i=0;i<len;i++){
                    const char *k=JS_AtomToCString(ctx, tab[i].atom);
                    JSValue ev=JS_GetProperty(ctx, v, tab[i].atom);
                    const char *es=NULL; size_t esn=0;
                    if(k){
                        /* reject collision with required params (param pollution) */
                        if(!strcmp(k,"response_type")||!strcmp(k,"client_id")||!strcmp(k,"redirect_uri")||!strcmp(k,"scope")||!strcmp(k,"state")||!strcmp(k,"code_challenge")||!strcmp(k,"code_challenge_method")){ JS_FreeCString(ctx,k); JS_FreeValue(ctx,ev); continue; }
                    }
                    if(k && JS_IsString(ev)) es=JS_ToCStringLen(ctx,&esn,ev);
                    if(k && es){
                        if(has_ctl_or_crlf(k,strlen(k))||has_ctl_or_crlf(es,esn)){ JS_FreeCString(ctx,k); JS_FreeCString(ctx,es); JS_FreeValue(ctx,ev); continue; }
                        APPEND_PARAM(k,esn,es);
                        JS_FreeCString(ctx,es);
                    }
                    if(k) JS_FreeCString(ctx,k);
                    JS_FreeValue(ctx,ev);
                }
                JS_FreePropertyEnum(ctx,tab,len);
            }
        }
        JS_FreeValue(ctx,v);
        #undef APPEND_PARAM
    }
    {
        JSValue obj=JS_NewObject(ctx);
        JS_DefinePropertyValueStr(ctx,obj,"url",JS_NewStringLen(ctx,url,url_len),JS_PROP_C_W_E);
        if(state) JS_DefinePropertyValueStr(ctx,obj,"state",JS_NewStringLen(ctx,state,scn),JS_PROP_C_W_E);
        r=obj;
    }
    goto cleanup;
bad:
    r=JS_ThrowTypeError(ctx,"buildAuthorizationUrl: invalid chars or scope");
    goto cleanup;
oom:
    r=JS_ThrowOutOfMemory(ctx);
    goto cleanup;
fail:
    r=JS_ThrowTypeError(ctx,"buildAuthorizationUrl: authorizationEndpoint, clientId, redirectUri required strings");
cleanup:
    if(endpoint) JS_FreeCString(ctx,endpoint);
    if(clientId) JS_FreeCString(ctx,clientId);
    if(redirectUri) JS_FreeCString(ctx,redirectUri);
    if(scope) JS_FreeCString(ctx,scope);
    if(state) { if(state_is_malloc) js_free(ctx,(void*)state); else JS_FreeCString(ctx,state); }
    if(cc) JS_FreeCString(ctx,cc);
    if(ccm) { if(ccm_is_malloc) js_free(ctx,(void*)ccm); else JS_FreeCString(ctx,ccm); }
    if(url) js_free(ctx,url);
    return r;
}

/* parseAuthorizationResponse(url): query ?code or ?error
 * Refuses loudly what a code-flow client must never see: an implicit-flow
 * response (tokens in the fragment, OAuth 2.1 §2.1.2), code+error together
 * (6749 §4.1.2.1), and duplicate code/state/error params (request forgery
 * tell -- a duplicated state is how CSRF defeats get laundered). */
static JSValue oauth_parse_auth_resp(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv){
    const char *url; size_t n;
    JSValue obj;
    int saw_code=0, saw_error=0, saw_state=0, saw_desc=0, saw_uri=0;
    (void)this_val;
    if(argc<1 || !JS_IsString(argv[0])) return JS_ThrowTypeError(ctx,"parseAuthorizationResponse(url: string)");
    url=JS_ToCStringLen(ctx,&n,argv[0]); if(!url) return JS_EXCEPTION;
    const char *q=strchr(url,'?');
    const char *f=strchr(url,'#');
    const char *search = q? q+1: NULL;
    size_t slen = 0;
    if(search){
        const char *end = f && f>search ? f : url+n;
        slen = end - search;
    } else if(f){
        /* fragment not used for code flow; but parse empty */
        search = ""; slen=0;
    }
    if(f){
        /* implicit-flow responses carry the token in the fragment (6749
           §4.2.2); a code-flow client must refuse them, not silently
           return {} (OAuth 2.1 removes the implicit flow) */
        const char *fs = f + 1;
        size_t flen = (size_t)(url + n - fs);
        size_t i = 0;
        while(i < flen){
            size_t b=i, eq=(size_t)-1;
            while(i < flen && fs[i]!='&'){ if(fs[i]=='=' && eq==(size_t)-1) eq=i; i++; }
            size_t kl = (eq==(size_t)-1? i-b: eq-b);
            if((kl==12 && !memcmp(fs+b,"access_token",12)) ||
               (kl==10 && !memcmp(fs+b,"token_type",10)) ||
               (kl==10 && !memcmp(fs+b,"expires_in",10)) ||
               (kl==5  && !memcmp(fs+b,"scope",5)) ||
               (kl==8  && !memcmp(fs+b,"id_token",8))){
                JS_FreeCString(ctx,url);
                return JS_ThrowTypeError(ctx,"parseAuthorizationResponse: implicit-flow token in fragment not supported (code flow only)");
            }
            if(i < flen && fs[i]=='&') i++;
        }
    }
    obj=JS_NewObject(ctx);
    if(search && slen){
        size_t i=0;
        while(i < slen){
            size_t b=i, eq=(size_t)-1;
            while(i < slen && search[i]!='&'){ if(search[i]=='=' && eq==(size_t)-1) eq=i; i++; }
            size_t kl = (eq==(size_t)-1? i-b: eq-b);
            size_t vl = (eq==(size_t)-1? 0: i-eq-1);
            char *k=(char*)js_malloc(ctx, kl+1); char *v=(char*)js_malloc(ctx, vl+1);
            size_t kl2,vl2;
            if(k&&v){
                form_pct_decode(k,&kl2, search+b, kl);
                if(eq!=(size_t)-1) form_pct_decode(v,&vl2, search+eq+1, vl); else {v[0]='\0'; vl2=0;}
                if(!strcmp(k,"code")){ if(saw_code){ js_free(ctx,k); js_free(ctx,v); JS_FreeValue(ctx,obj); JS_FreeCString(ctx,url); return JS_ThrowTypeError(ctx,"parseAuthorizationResponse: duplicate code param"); } saw_code=1; JS_DefinePropertyValueStr(ctx,obj,"code",JS_NewStringLen(ctx,v,vl2),JS_PROP_C_W_E); }
                else if(!strcmp(k,"state")){ if(saw_state){ js_free(ctx,k); js_free(ctx,v); JS_FreeValue(ctx,obj); JS_FreeCString(ctx,url); return JS_ThrowTypeError(ctx,"parseAuthorizationResponse: duplicate state param"); } saw_state=1; JS_DefinePropertyValueStr(ctx,obj,"state",JS_NewStringLen(ctx,v,vl2),JS_PROP_C_W_E); }
                else if(!strcmp(k,"error")){ if(saw_error){ js_free(ctx,k); js_free(ctx,v); JS_FreeValue(ctx,obj); JS_FreeCString(ctx,url); return JS_ThrowTypeError(ctx,"parseAuthorizationResponse: duplicate error param"); } saw_error=1; JS_DefinePropertyValueStr(ctx,obj,"error",JS_NewStringLen(ctx,v,vl2),JS_PROP_C_W_E); }
                else if(!strcmp(k,"error_description")){
                    if(saw_desc){ js_free(ctx,k); js_free(ctx,v); JS_FreeValue(ctx,obj); JS_FreeCString(ctx,url); return JS_ThrowTypeError(ctx,"parseAuthorizationResponse: duplicate error_description param"); }
                    saw_desc=1; JS_DefinePropertyValueStr(ctx,obj,"errorDescription",JS_NewStringLen(ctx,v,vl2),JS_PROP_C_W_E);
                }
                else if(!strcmp(k,"error_uri")){
                    if(saw_uri){ js_free(ctx,k); js_free(ctx,v); JS_FreeValue(ctx,obj); JS_FreeCString(ctx,url); return JS_ThrowTypeError(ctx,"parseAuthorizationResponse: duplicate error_uri param"); }
                    saw_uri=1; JS_DefinePropertyValueStr(ctx,obj,"errorUri",JS_NewStringLen(ctx,v,vl2),JS_PROP_C_W_E);
                }
            }
            if(k) js_free(ctx,k); if(v) js_free(ctx,v);
            if(i < slen && search[i]=='&') i++;
        }
    }
    JS_FreeCString(ctx,url);
    /* 6749 §4.1.2.1: an error response MUST NOT also carry a code */
    if(saw_code && saw_error){ JS_FreeValue(ctx,obj); return JS_ThrowTypeError(ctx,"parseAuthorizationResponse: code and error must not both be present (6749 §4.1.2.1)"); }
    return obj;
}

/* buildTokenRequestBody(params) -> x-www-form-urlencoded string.
 * Every param must be a string; a non-string value is a caller bug and is
 * REFUSED (a token request that silently omits a field fails confusingly
 * at the server). OOM anywhere is a throw, never a truncated body. */
static JSValue oauth_build_token_body(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv){
    JSValue obj;
    JSPropertyEnum *tab=NULL; uint32_t len=0;
    char *out=NULL; size_t cap=256, off=0;
    JSValue r=JS_EXCEPTION;
    (void)this_val;
    if(argc<1 || !JS_IsObject(argv[0])) return JS_ThrowTypeError(ctx,"buildTokenRequestBody(params: Record<string,string>)");
    obj=argv[0];
    if(JS_GetOwnPropertyNames(ctx,&tab,&len,obj,JS_GPN_STRING_MASK|JS_GPN_ENUM_ONLY)<0) return JS_EXCEPTION;
    out=(char*)js_malloc(ctx, cap); if(!out){ JS_FreePropertyEnum(ctx,tab,len); return JS_ThrowOutOfMemory(ctx); }
    out[0]='\0';
    for(uint32_t i=0;i<len;i++){
        const char *k=JS_AtomToCString(ctx, tab[i].atom);
        JSValue ev=JS_GetProperty(ctx,obj,tab[i].atom);
        const char *es=NULL; size_t esn=0;
        char *ek=NULL,*evv=NULL;
        if(!k){ JS_FreeValue(ctx,ev); r=JS_ThrowOutOfMemory(ctx); goto done; }
        if(!JS_IsString(ev)){ r=JS_ThrowTypeError(ctx,"buildTokenRequestBody: value of '%s' must be a string", k); JS_FreeCString(ctx,k); JS_FreeValue(ctx,ev); goto done; }
        es=JS_ToCStringLen(ctx,&esn,ev);
        JS_FreeValue(ctx,ev);
        if(!es){ JS_FreeCString(ctx,k); r=JS_ThrowOutOfMemory(ctx); goto done; }
        ek=form_encode_component(ctx,k,strlen(k),NULL);
        evv=form_encode_component(ctx,es,esn,NULL);
        JS_FreeCString(ctx,k); JS_FreeCString(ctx,es);
        if(!ek || !evv){ if(ek) js_free(ctx,ek); if(evv) js_free(ctx,evv); r=JS_ThrowOutOfMemory(ctx); goto done; }
        {
            size_t need=off+(off?1:0)+strlen(ek)+1+strlen(evv)+1;
            if(need>cap){ size_t nc=need*2; char*nb=(char*)js_realloc(ctx,out,nc); if(!nb){ js_free(ctx,ek); js_free(ctx,evv); r=JS_ThrowOutOfMemory(ctx); goto done; } out=nb; cap=nc; }
            if(off) out[off++]='&';
            memcpy(out+off, ek, strlen(ek)); off+=strlen(ek);
            out[off++]='=';
            memcpy(out+off, evv, strlen(evv)); off+=strlen(evv);
            out[off]='\0';
        }
        js_free(ctx,ek); js_free(ctx,evv);
    }
    r=JS_NewStringLen(ctx,out,off);
done:
    if(out) js_free(ctx,out);
    JS_FreePropertyEnum(ctx,tab,len);
    return r;
}

/* parseTokenResponse(body, headers?): JSON; RFC 6749 §5.1 REQUIRED fields
 * access_token + token_type (Bearer case-insensitive, per 6750 §2.1 the
 * access_token must itself be a b64token), optional numeric expires_in and
 * string scope/refresh_token. A malformed response is refused, not passed
 * through for the caller to index undefined. */
static JSValue oauth_parse_token_resp(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv){
    const char *body; size_t n;
    JSValue j;
    (void)this_val;
    if(argc<1 || !JS_IsString(argv[0])) return JS_ThrowTypeError(ctx,"parseTokenResponse(body: string)");
    body=JS_ToCStringLen(ctx,&n,argv[0]); if(!body) return JS_EXCEPTION;
    j=JS_ParseJSON(ctx, body, n, "<token>");
    JS_FreeCString(ctx,body);
    if(JS_IsException(j)) return j;
    /* a bare JSON value (123, "x", true) is not a token response: refusing is
       §5.1 validation, returning it as-is skips that entirely. */
    if(!JS_IsObject(j)){ JS_FreeValue(ctx,j); return JS_ThrowTypeError(ctx,"parseTokenResponse: body must be a JSON object"); }
    /* token_type REQUIRED and must be Bearer case-insensitive */
    {
        JSValue tv=JS_GetPropertyStr(ctx, j,"token_type");
        if(!JS_IsString(tv)){ JS_FreeValue(ctx,tv); JS_FreeValue(ctx,j); return JS_ThrowTypeError(ctx,"parseTokenResponse: token_type (string) required"); }
        {
            const char *ts=JS_ToCString(ctx,tv);
            int ok = ts && ( !strcasecmp(ts,"Bearer") || !strcasecmp(ts,"bearer"));
            if(ts) JS_FreeCString(ctx,ts);
            if(!ok){ JS_FreeValue(ctx,tv); JS_FreeValue(ctx,j); return JS_ThrowTypeError(ctx,"parseTokenResponse: token_type must be Bearer"); }
        }
        JS_FreeValue(ctx,tv);
    }
    /* access_token REQUIRED, must be a b64token (6750 §2.1) */
    {
        JSValue tv=JS_GetPropertyStr(ctx, j,"access_token");
        size_t an=0; int ok=0;
        if(!JS_IsString(tv)){ JS_FreeValue(ctx,tv); JS_FreeValue(ctx,j); return JS_ThrowTypeError(ctx,"parseTokenResponse: access_token (string) required"); }
        {
            const char *ts=JS_ToCStringLen(ctx,&an,tv);
            if(ts){ ok = is_valid_b64token(ts,an); JS_FreeCString(ctx,ts); }
        }
        JS_FreeValue(ctx,tv);
        if(!ok){ JS_FreeValue(ctx,j); return JS_ThrowTypeError(ctx,"parseTokenResponse: access_token must be a b64token"); }
    }
    /* optional fields must have their documented types (6749 §5.1) */
    {
        JSValue tv=JS_GetPropertyStr(ctx, j,"expires_in");
        if(!JS_IsUndefined(tv) && !JS_IsNull(tv) && !JS_IsNumber(tv)){ JS_FreeValue(ctx,tv); JS_FreeValue(ctx,j); return JS_ThrowTypeError(ctx,"parseTokenResponse: expires_in must be a number"); }
        JS_FreeValue(ctx,tv);
        tv=JS_GetPropertyStr(ctx, j,"scope");
        if(!JS_IsUndefined(tv) && !JS_IsNull(tv) && !JS_IsString(tv)){ JS_FreeValue(ctx,tv); JS_FreeValue(ctx,j); return JS_ThrowTypeError(ctx,"parseTokenResponse: scope must be a string"); }
        JS_FreeValue(ctx,tv);
        tv=JS_GetPropertyStr(ctx, j,"refresh_token");
        if(!JS_IsUndefined(tv) && !JS_IsNull(tv) && !JS_IsString(tv)){ JS_FreeValue(ctx,tv); JS_FreeValue(ctx,j); return JS_ThrowTypeError(ctx,"parseTokenResponse: refresh_token must be a string"); }
        JS_FreeValue(ctx,tv);
    }
    return j;
}

/* ---- JWT claim verification (optional, needs CONFIG_TLS) ------------- */
#define OAUTH_JWT_HOLDER "__oauth_jwt_verify"
static JSValue oauth_get_jwt_verify(JSContext *ctx){
    JSValue g, holder, v, r;
    /* Always (re)install the bridge BEFORE trusting the slot: a function a
       caller pre-planted on globalThis.__oauth_jwt_verify must never be
       accepted as the crypto bridge -- that is a signature-check bypass.
       Re-defining a frozen slot with the same value is a no-op, so the
       second-and-later calls are cheap (the module eval is cached). */
    const char *src =
        "import * as m from \"dyna:crypto\";\n"
        "Object.defineProperty(globalThis, \"" OAUTH_JWT_HOLDER "\",\n"
        "  { value: m.JWTVerify, writable: false, enumerable: false, configurable: false });";
    r = JS_Eval(ctx, src, strlen(src), "<oauth-jwt-bridge>", JS_EVAL_TYPE_MODULE|JS_EVAL_FLAG_COMPILE_ONLY);
    if(JS_IsException(r)) return r;
    v = JS_EvalFunction(ctx, r);
    if(JS_IsException(v)) return v;
    if(JS_PromiseState(ctx, v)==JS_PROMISE_REJECTED){
        JSValue err = JS_DupValue(ctx, JS_PromiseResult(ctx, v));
        JS_FreeValue(ctx, v);
        return JS_Throw(ctx, err);
    }
    JS_FreeValue(ctx, v);
    g = JS_GetGlobalObject(ctx);
    holder = JS_GetPropertyStr(ctx, g, OAUTH_JWT_HOLDER);
    JS_FreeValue(ctx, g);
    return holder;
}
static JSValue oauth_verify_jwt(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv){
    JSValue jwtVerify = JS_UNDEFINED, payload = JS_UNDEFINED, opts = JS_UNDEFINED, v = JS_UNDEFINED;
    int64_t skew = 0;
    time_t now = time(NULL);
    (void)this_val;
    if(argc < 2) return JS_ThrowTypeError(ctx, "verifyJWT(token, key, {algorithms, aud, iss, requiredScope, clockSkewSec})");
    if(!JS_IsString(argv[0])) return JS_ThrowTypeError(ctx, "verifyJWT: token must be string");
    /* opts is argv[2] */
    if(argc < 3 || !JS_IsObject(argv[2])) return JS_ThrowTypeError(ctx, "verifyJWT: opts with algorithms required");
    opts = argv[2];
    v = JS_GetPropertyStr(ctx, opts, "algorithms");
    if(!JS_IsArray(ctx, v)){ JS_FreeValue(ctx, v); return JS_ThrowTypeError(ctx, "verifyJWT: algorithms must be array"); }
    {
        JSValue lenv = JS_GetPropertyStr(ctx, v, "length");
        uint32_t n=0; JS_ToUint32(ctx,&n,lenv); JS_FreeValue(ctx,lenv);
        for(uint32_t i=0;i<n;i++){
            JSValue e = JS_GetPropertyUint32(ctx, v, i);
            const char *es = JS_ToCString(ctx, e);
            if(es){
                if(!strcasecmp(es,"none")){ JS_FreeCString(ctx,es); JS_FreeValue(ctx,e); JS_FreeValue(ctx,v); return JS_ThrowTypeError(ctx, "verifyJWT: alg:none not allowed"); }
                /* disallow mixing HS* with RS*/ 
            }
            if(es) JS_FreeCString(ctx,es);
            JS_FreeValue(ctx,e);
        }
    }
    JS_FreeValue(ctx, v);
    v = JS_GetPropertyStr(ctx, opts, "clockSkewSec");
    if(JS_IsNumber(v)){ if(JS_ToInt64(ctx, &skew, v)){ JS_FreeValue(ctx, v); return JS_EXCEPTION; } }
    JS_FreeValue(ctx, v);
    /* a typo like clockSkewSec: 60*1000 must not silently become strict mode */
    if(skew < 0 || skew > 300) return JS_ThrowRangeError(ctx, "verifyJWT: clockSkewSec must be 0..300");
    jwtVerify = oauth_get_jwt_verify(ctx);
    if(JS_IsException(jwtVerify)) return jwtVerify;
    if(!JS_IsFunction(ctx, jwtVerify)){ JS_FreeValue(ctx, jwtVerify); return JS_ThrowTypeError(ctx, "verifyJWT: JWT not available (rebuild with CONFIG_TLS=y)"); }
    {
        JSValueConst args[3] = {argv[0], argv[1], opts};
        payload = JS_Call(ctx, jwtVerify, JS_UNDEFINED, 3, args);
        JS_FreeValue(ctx, jwtVerify);
        if(JS_IsException(payload)) return payload;
    }
    if(!JS_IsObject(payload) || JS_IsArray(ctx, payload)){ JS_FreeValue(ctx,payload); return JS_ThrowTypeError(ctx, "verifyJWT: payload must be a JSON object (RFC 7519 §4)"); }
    /* exp */
    v = JS_GetPropertyStr(ctx, payload, "exp");
    if(JS_IsNumber(v)){
        double expd; JS_ToFloat64(ctx, &expd, v);
        /* RFC 7519 §4.1.4: the current time must be STRICTLY before exp
           (skew extends validity); exp == now is expired. */
        if((double)now >= expd + skew){ JS_FreeValue(ctx,v); JS_FreeValue(ctx,payload); return JS_ThrowTypeError(ctx, "verifyJWT: token expired"); }
    } else if(!JS_IsUndefined(v) && !JS_IsNull(v)){ JS_FreeValue(ctx,v); JS_FreeValue(ctx,payload); return JS_ThrowTypeError(ctx, "verifyJWT: exp must be number"); }
    JS_FreeValue(ctx,v);
    v = JS_GetPropertyStr(ctx, payload, "nbf");
    if(JS_IsNumber(v)){
        double nbfd; JS_ToFloat64(ctx, &nbfd, v);
        if(nbfd > (double)now + skew){ JS_FreeValue(ctx,v); JS_FreeValue(ctx,payload); return JS_ThrowTypeError(ctx, "verifyJWT: token not yet valid (nbf)"); }
    } else if(!JS_IsUndefined(v) && !JS_IsNull(v)){ JS_FreeValue(ctx,v); JS_FreeValue(ctx,payload); return JS_ThrowTypeError(ctx, "verifyJWT: nbf must be number"); }
    JS_FreeValue(ctx,v);
    /* aud */
    v = JS_GetPropertyStr(ctx, opts, "aud");
    if(!JS_IsUndefined(v) && !JS_IsNull(v)){
        JSValue tokAud = JS_GetPropertyStr(ctx, payload, "aud");
        int ok = 0;
        int expIsArray = JS_IsArray(ctx, v);
        if(JS_IsString(tokAud) && JS_IsString(v)){
            const char *a = JS_ToCString(ctx, tokAud); const char *b = JS_ToCString(ctx, v);
            ok = (a && b && !strcmp(a,b)); if(a) JS_FreeCString(ctx,a); if(b) JS_FreeCString(ctx,b);
        } else if(JS_IsArray(ctx, tokAud) && JS_IsString(v)){
            JSValue lenv = JS_GetPropertyStr(ctx, tokAud, "length"); uint32_t n=0; JS_ToUint32(ctx,&n,lenv); JS_FreeValue(ctx,lenv);
            const char *b = JS_ToCString(ctx, v);
            for(uint32_t i=0;i<n;i++){ JSValue e=JS_GetPropertyUint32(ctx,tokAud,i); const char *a=JS_ToCString(ctx,e); if(a&&b&&!strcmp(a,b)) ok=1; if(a) JS_FreeCString(ctx,a); JS_FreeValue(ctx,e); if(ok) break; }
            if(b) JS_FreeCString(ctx,b);
        } else if(JS_IsString(tokAud) && expIsArray){
            JSValue lenv = JS_GetPropertyStr(ctx, v, "length"); uint32_t n=0; JS_ToUint32(ctx,&n,lenv); JS_FreeValue(ctx,lenv);
            const char *a = JS_ToCString(ctx, tokAud);
            for(uint32_t i=0;i<n;i++){ JSValue e=JS_GetPropertyUint32(ctx,v,i); const char *b=JS_ToCString(ctx,e); if(a&&b&&!strcmp(a,b)) ok=1; if(b) JS_FreeCString(ctx,b); JS_FreeValue(ctx,e); if(ok) break; }
            if(a) JS_FreeCString(ctx,a);
        } else if(JS_IsArray(ctx, tokAud) && expIsArray){
            JSValue lenv1 = JS_GetPropertyStr(ctx, tokAud, "length"); uint32_t n1=0; JS_ToUint32(ctx,&n1,lenv1); JS_FreeValue(ctx,lenv1);
            JSValue lenv2 = JS_GetPropertyStr(ctx, v, "length"); uint32_t n2=0; JS_ToUint32(ctx,&n2,lenv2); JS_FreeValue(ctx,lenv2);
            for(uint32_t i=0;i<n1 && !ok;i++){ JSValue e1=JS_GetPropertyUint32(ctx,tokAud,i); const char *a=JS_ToCString(ctx,e1); for(uint32_t j=0;j<n2;j++){ JSValue e2=JS_GetPropertyUint32(ctx,v,j); const char *b=JS_ToCString(ctx,e2); if(a&&b&&!strcmp(a,b)) ok=1; if(b) JS_FreeCString(ctx,b); JS_FreeValue(ctx,e2); if(ok) break; } if(a) JS_FreeCString(ctx,a); JS_FreeValue(ctx,e1); }
        }
        JS_FreeValue(ctx, tokAud);
        if(!ok){ JS_FreeValue(ctx,v); JS_FreeValue(ctx,payload); return JS_ThrowTypeError(ctx, "verifyJWT: aud mismatch"); }
    }
    JS_FreeValue(ctx,v);
    v = JS_GetPropertyStr(ctx, opts, "iss");
    if(JS_IsString(v)){
        JSValue tokIss = JS_GetPropertyStr(ctx, payload, "iss");
        int ok=0;
        if(JS_IsString(tokIss)){
            const char *a=JS_ToCString(ctx,tokIss); const char *b=JS_ToCString(ctx,v);
            ok = a&&b&&!strcmp(a,b); if(a) JS_FreeCString(ctx,a); if(b) JS_FreeCString(ctx,b);
        }
        JS_FreeValue(ctx,tokIss);
        if(!ok){ JS_FreeValue(ctx,v); JS_FreeValue(ctx,payload); return JS_ThrowTypeError(ctx, "verifyJWT: iss mismatch"); }
    }
    JS_FreeValue(ctx,v);
    v = JS_GetPropertyStr(ctx, opts, "requiredScope");
    /* a non-array requiredScope used to be ignored -- i.e. NO scope check at
       all. A caller who asked for a scope requirement must get one or an
       error, never a silent skip. */
    if(!JS_IsUndefined(v) && !JS_IsNull(v) && !JS_IsArray(ctx, v)){
        JS_FreeValue(ctx,v); JS_FreeValue(ctx,payload);
        return JS_ThrowTypeError(ctx, "verifyJWT: requiredScope must be an array of strings");
    }
    if(JS_IsArray(ctx, v)){
        JSValue tokScope = JS_GetPropertyStr(ctx, payload, "scope");
        const char *scopeStr = JS_IsString(tokScope)? JS_ToCString(ctx,tokScope): NULL;
        JSValue lenv = JS_GetPropertyStr(ctx, v, "length"); uint32_t n=0; JS_ToUint32(ctx,&n,lenv); JS_FreeValue(ctx,lenv);
        for(uint32_t i=0;i<n;i++){
            JSValue e=JS_GetPropertyUint32(ctx,v,i); const char *need=JS_ToCString(ctx,e);
            int found=0;
            if(scopeStr){
                size_t sl=strlen(scopeStr), off=0;
                while(off<sl){
                    while(off<sl && scopeStr[off]==' ') off++;
                    size_t b=off; while(off<sl && scopeStr[off]!=' ') off++;
                    size_t l=off-b;
                    if(l && need && strlen(need)==l && !memcmp(need,scopeStr+b,l)) found=1;
                }
            }
            if(need) JS_FreeCString(ctx,need);
            JS_FreeValue(ctx,e);
            if(!found){ if(scopeStr) JS_FreeCString(ctx,scopeStr); JS_FreeValue(ctx,tokScope); JS_FreeValue(ctx,v); JS_FreeValue(ctx,payload); return JS_ThrowTypeError(ctx, "verifyJWT: required scope missing"); }
        }
        if(scopeStr) JS_FreeCString(ctx,scopeStr);
        JS_FreeValue(ctx,tokScope);
    }
    JS_FreeValue(ctx,v);
    return payload;
}

static JSValue oauth_build_client_auth(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv){
    const char *id=NULL,*sec=NULL; size_t idn=0,secn=0;
    char *ei=NULL,*es=NULL; size_t ein=0,esn=0;
    char *joined=NULL, *b64=NULL; size_t blen;
    JSValue r=JS_EXCEPTION;
    (void)this_val;
    if(argc<1 || !JS_IsString(argv[0])) return JS_ThrowTypeError(ctx,"buildClientAuthHeader(clientId, clientSecret?)");
    id=JS_ToCStringLen(ctx,&idn,argv[0]); if(!id) return JS_EXCEPTION;
    if(argc>1 && JS_IsString(argv[1])) sec=JS_ToCStringLen(ctx,&secn,argv[1]);
    else secn=0;
    if(has_ctl_or_crlf(id,idn) || (sec && has_ctl_or_crlf(sec,secn))) goto bad;
    ei=form_encode_component(ctx,id,idn,&ein);
    es= sec? form_encode_component(ctx,sec,secn,&esn): (char*)js_malloc(ctx,1);
    if(!ei || !es) goto oom;
    if(!sec) { es[0]='\0'; esn=0; }
    joined=(char*)js_malloc(ctx, ein+1+esn+1);
    if(!joined) goto oom;
    memcpy(joined, ei, ein); joined[ein]=':'; memcpy(joined+ein+1, es, esn); joined[ein+1+esn]='\0';
    b64=(char*)js_malloc(ctx, 4*((ein+1+esn+2)/3)+1);
    if(!b64) goto oom;
    blen=dyn_codec_base64_encode((const uint8_t*)joined, ein+1+esn, b64);
    {
        char *out=(char*)js_malloc(ctx, 6+blen+1);
        if(!out) goto oom;
        memcpy(out,"Basic ",6); memcpy(out+6,b64,blen); out[6+blen]='\0';
        r=JS_NewStringLen(ctx,out,6+blen);
        js_free(ctx,out);
    }
    goto cleanup;
bad: r=JS_ThrowTypeError(ctx,"buildClientAuthHeader: clientId/secret must not contain CTL/CRLF");
    goto cleanup;
oom: r=JS_ThrowOutOfMemory(ctx);
cleanup:
    if(id) JS_FreeCString(ctx,id);
    if(sec) JS_FreeCString(ctx,sec);
    if(ei) js_free(ctx,ei);
    if(es) js_free(ctx,es);
    if(joined) js_free(ctx,joined);
    if(b64) js_free(ctx,b64);
    return r;
}

/* ---- module def --------------------------------------------------- */

static const JSCFunctionListEntry oauth_funcs[] = {
    JS_CFUNC_DEF("generateCodeVerifier", 0, oauth_gen_verifier),
    JS_CFUNC_DEF("isValidCodeVerifier", 1, oauth_is_valid_verifier),
    JS_CFUNC_DEF("generateCodeChallenge", 1, oauth_gen_challenge),
    JS_CFUNC_DEF("verifyCodeChallenge", 3, oauth_verify_challenge),
    JS_CFUNC_DEF("generateState", 0, oauth_gen_state),
    JS_CFUNC_DEF("secureCompare", 2, oauth_secure_compare),
    JS_CFUNC_DEF("parseScope", 1, oauth_parse_scope),
    JS_CFUNC_DEF("formatScope", 1, oauth_format_scope),
    JS_CFUNC_DEF("buildBearerHeader", 1, oauth_build_bearer),
    JS_CFUNC_DEF("parseBearerHeader", 1, oauth_parse_bearer),
    JS_CFUNC_DEF("parseBearerFromRequest", 1, oauth_parse_bearer_from_req),
    JS_CFUNC_DEF("isValidBearerToken", 1, oauth_is_valid_bearer),
    JS_CFUNC_DEF("buildWWWAuthenticate", 1, oauth_build_www),
    JS_CFUNC_DEF("isValidRedirectUri", 2, oauth_is_valid_redirect),
    JS_CFUNC_DEF("buildAuthorizationUrl", 1, oauth_build_auth_url),
    JS_CFUNC_DEF("parseAuthorizationResponse", 1, oauth_parse_auth_resp),
    JS_CFUNC_DEF("buildTokenRequestBody", 1, oauth_build_token_body),
    JS_CFUNC_DEF("parseTokenResponse", 1, oauth_parse_token_resp),
    JS_CFUNC_DEF("buildClientAuthHeader", 1, oauth_build_client_auth),
    JS_CFUNC_DEF("verifyJWT", 3, oauth_verify_jwt),
};

static int oauth_init(JSContext *ctx, JSModuleDef *m){
    return JS_SetModuleExportList(ctx,m,oauth_funcs,countof(oauth_funcs));
}
int js_nat_init_oauth2(JSContext *ctx){
    JSModuleDef *m=JS_NewCModule(ctx,"dyna:oauth2",oauth_init);
    if(!m) return -1;
    return JS_AddModuleExportList(ctx,m,oauth_funcs,countof(oauth_funcs));
}

#else /* !CONFIG_NATIVE_MODULE_OAUTH2 */
int js_nat_init_oauth2(JSContext *ctx){ (void)ctx; return 0; }
#endif
