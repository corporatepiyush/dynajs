#ifndef OAUTH2_INTERNAL_H
#define OAUTH2_INTERNAL_H

#include "dyna-nat.h"
#include "cutils.h"
#include "core/dyn-pct.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_OAUTH2)

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <strings.h>
#include <time.h>
#include <math.h>
#include "core/dyn-hash.h"
#include "core/dyn-codec.h"
#include "core/dyn-prng.h"

#ifndef countof
#define countof(x) (sizeof(x) / sizeof((x)[0]))
#endif

static inline void oauth_wipe(void* p, size_t n)
{
#if defined(__GLIBC__) || defined(__OpenBSD__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__DragonFly__) || defined(__sun__)
    explicit_bzero(p, n);
#else
    memset(p, 0, n);
#endif
}

static inline int is_unreserved(unsigned char c)
{
    static const uint64_t m[4] = { 0x03ff600000000000ull, 0x47fffffe87fffffeull, 0, 0 };
    return (int)((m[c >> 6] >> (c & 63)) & 1u);
}

static inline int is_nqchar(unsigned char c)
{
    return (c == 0x21) || (c >= 0x23 && c <= 0x5B) || (c >= 0x5D && c <= 0x7E);
}

static inline int has_ctl_or_crlf(const char* s, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c < 0x20 || c == 0x7F || c == '\r' || c == '\n')
            return 1;
    }
    return 0;
}

static inline JSValue b64url_new(JSContext* ctx, const uint8_t* data, size_t n)
{
    char* out = (char*)js_malloc(ctx, 4 * ((n + 2) / 3) + 1);
    size_t m;
    JSValue r;
    if (!out)
        return JS_ThrowOutOfMemory(ctx);
    m = dyn_codec_base64url_encode(data, n, out);
    r = JS_NewStringLen(ctx, out, m);
    js_free(ctx, out);
    return r;
}

JSValue oauth_gen_verifier(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);

JSValue oauth_is_valid_verifier(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);

JSValue oauth_gen_challenge(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);

JSValue oauth_verify_challenge(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);

JSValue oauth_gen_state(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);

JSValue oauth_parse_scope(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);

int is_valid_b64token(const char* s, size_t n, int strict);

JSValue oauth_build_bearer(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);

JSValue oauth_parse_bearer(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);

JSValue oauth_parse_bearer_from_req(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);

int is_valid_error(const char* s, size_t n, int strict);

int is_valid_error_desc(const char* s, size_t n, int sp);

JSValue oauth_build_www(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);

JSValue oauth_is_valid_bearer(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);

JSValue oauth_is_valid_redirect(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);

JSValue oauth_build_auth_url(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);

JSValue oauth_parse_auth_resp(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);

JSValue oauth_build_token_body(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);

JSValue oauth_parse_token_resp(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);

static inline void oauth_str_list_free(char** l, uint32_t n)
{
    uint32_t i;
    if (!l)
        return;
    for (i = 0; i < n; i++)
        free(l[i]);
    free(l);
}

int oauth_str_list(JSContext* ctx, JSValueConst v, const char* what,
    char*** out, uint32_t* pn);

JSValue oauth_verify_jwt(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);

JSValue oauth_build_client_auth(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);

#endif

#endif
