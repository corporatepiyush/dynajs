/* Fuzz target for dyna:oauth2 (src/dyna-oauth2.c) -- the plan's promised
 * three harnesses (docs/oauth2-plan.md §3): b64token, unreserved, and
 * form+redirect, driven through the REAL module so every path a caller
 * reaches is the one being covered.
 *
 * INPUTS, on purpose (fuzz_stdlib.c's rule): FUZZ is the bytes as a JS
 * STRING (the way callers reach text parsers); FUZZB is the SAME bytes as a
 * Uint8Array, because secureCompare/verifyJWT take BytesInput and the
 * ByteView path (JS_GetArrayBufferView offsets) is only reachable with real
 * bytes. Both buffers are allocated at EXACTLY the input size, so a
 * one-past-the-end read is a heap overflow, not a read into spare capacity
 * that reports clean.
 *
 * VALUE oracle inside the unreserved harness: generateCodeChallenge(v,
 * "S256") followed by verifyCodeChallenge(v, c, "S256") must be true for
 * the SAME implementation -- a wrong-answer abort is a bug report, not a
 * memory-safety nicety.
 *
 * Run with -max_len=8192 (the module's own caps are 4096). Build with
 * LIB_FUZZING_ENGINE="-fsanitize=fuzzer,address" to catch silent overreads.
 */

#include "dynajs.h"
#include "dyna-nat.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static void run(JSContext *ctx, const char *src) {
    JSValue v = JS_Eval(ctx, src, strlen(src), "<oauth2-fuzz>",
                        JS_EVAL_TYPE_MODULE);
    if (JS_IsException(v)) {
        /* Every reachable call is wrapped in t(); the only top-level throw
         * is the PKCE oracle, or the module itself being absent -- both
         * are findings, not inputs. A run that cannot reach the code must
         * not report clean. */
        JS_FreeValue(ctx, JS_GetException(ctx));
        abort();
    }
    JS_FreeValue(ctx, v);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    JSRuntime *rt;
    JSContext *ctx;
    JSValue global, s;
    char *exact;
    char hex[17];
    size_t i;
    uint8_t nbyte;

    if (size > 8192)
        return 0;
    rt = JS_NewRuntime();
    if (!rt)
        return 0;
    ctx = JS_NewContext(rt);
    if (!ctx) {
        JS_FreeRuntime(rt);
        return 0;
    }
    JS_SetMemoryLimit(rt, 0x4000000);
    JS_SetMaxStackSize(rt, 0x40000);
#ifdef CONFIG_NATIVE_MODULES
    if (js_nat_init_all(ctx) < 0) {
        JS_FreeContext(ctx);
        JS_FreeRuntime(rt);
        return 0;
    }
#endif

    exact = (char *)malloc(size ? size : 1);
    if (!exact) {
        JS_FreeContext(ctx);
        JS_FreeRuntime(rt);
        return 0;
    }
    if (size)
        memcpy(exact, data, size);
    s = JS_NewStringLen(ctx, exact, size);
    free(exact);
    if (JS_IsException(s)) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        JS_FreeContext(ctx);
        JS_FreeRuntime(rt);
        return 0;
    }
    global = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, global, "FUZZ", s);   /* consumes s */
    {
        static const uint8_t stub = 0;
        JSValue ab = JS_NewArrayBufferCopy(ctx, size ? data : &stub, size);
        if (!JS_IsException(ab)) {
            JSValueConst ta[3];
            JSValue u8;
            ta[0] = ab; ta[1] = JS_UNDEFINED; ta[2] = JS_UNDEFINED;
            u8 = JS_NewTypedArray(ctx, 3, ta, JS_TYPED_ARRAY_UINT8);
            JS_FreeValue(ctx, ab);
            if (JS_IsException(u8))
                JS_FreeValue(ctx, JS_GetException(ctx));
            else
                JS_SetPropertyStr(ctx, global, "FUZZB", u8);
        } else {
            JS_FreeValue(ctx, JS_GetException(ctx));
        }
    }
    /* a hex-ish candidate for values that must look like text */
    for (i = 0; i < 16 && i < size; i++)
        hex[i] = "0123456789abcdef"[data[i] & 15];
    while (i < 16)
        hex[i++] = 'a';
    hex[16] = '\0';
    JS_SetPropertyStr(ctx, global, "FUZZH",
                      JS_NewStringLen(ctx, hex, 16));  /* ok if consumed */
    JS_FreeValue(ctx, global);

#ifdef CONFIG_NATIVE_MODULES
    /* ==== 1. b64token harness: bearer header/parse/request + WWW + token
     * response + client auth. b64token is a different alphabet from
     * unreserved (6750 vs 7636) and has its own validator. */
    run(ctx,
        "import * as o from 'dyna:oauth2';\n"
        "const t = f => { try { return f(); } catch (e) { return null; } };\n"
        "t(() => o.isValidBearerToken(FUZZ));\n"
        "t(() => o.buildBearerHeader(FUZZ.slice(0, 4096)));\n"
        "t(() => o.parseBearerHeader(FUZZ));\n"
        "t(() => o.parseBearerFromRequest({ headers: { authorization: FUZZ } }));\n"
        "t(() => o.parseBearerFromRequest({ headers: { Authorization: FUZZ },\n"
        "  query: FUZZ, body: FUZZ, allowQuery: true, allowBody: true }));\n"
        "t(() => o.parseBearerFromRequest({ query: FUZZ, allowQuery: true }));\n"
        "t(() => o.parseBearerFromRequest({ body: FUZZ, allowBody: true }));\n"
        "t(() => o.buildWWWAuthenticate({ realm: FUZZ.slice(0, 64),\n"
        "  error: 'invalid_token', errorDescription: FUZZ.slice(0, 64),\n"
        "  errorUri: FUZZ.slice(0, 128), scope: FUZZ.slice(0, 64) }));\n"
        "t(() => o.buildWWWAuthenticate({ error: FUZZ.slice(0, 64) }));\n"
        "t(() => o.buildClientAuthHeader(FUZZ.slice(0, 64), FUZZ.slice(0, 64)));\n"
        "t(() => o.buildClientAuthHeader(FUZZ.slice(0, 64)));\n"
        "t(() => o.parseTokenResponse(FUZZ));\n");

    /* ==== 2. unreserved harness: PKCE + state + constant-time compare.
     * The S256 round trip is a VALUE oracle: the same implementation must
     * agree with itself, and a wrong digest is a bug. */
    run(ctx,
        "import * as o from 'dyna:oauth2';\n"
        "const t = f => { try { return f(); } catch (e) { return null; } };\n"
        "const v = FUZZ.slice(0, 128);\n"
        "t(() => o.isValidCodeVerifier(v));\n"
        "t(() => o.generateCodeChallenge(v, 'S256'));\n"
        "t(() => o.generateCodeChallenge(v, 'plain'));\n"
        "const c = t(() => o.generateCodeChallenge(v, 'S256'));\n"
        "if (c !== null && !o.verifyCodeChallenge(v, c, 'S256'))\n"
        "  throw new Error('S256 round trip mismatch');\n"
        "t(() => o.verifyCodeChallenge(v, FUZZ.slice(0, 128), 'S256'));\n"
        "t(() => o.verifyCodeChallenge(v, FUZZ.slice(0, 128), 'plain'));\n"
        "t(() => o.generateState(1 + (FUZZ.length % 256)));\n"
        "t(() => o.generateState(256));\n"
        "t(() => o.secureCompare(FUZZ, FUZZB));\n"
        "t(() => o.secureCompare(FUZZ, FUZZ));\n"
        "t(() => o.secureCompare(FUZZB, FUZZ));\n"
        "t(() => o.parseScope(FUZZ.slice(0, 4096)));\n"
        "t(() => o.formatScope([FUZZ.slice(0, 64), FUZZH]));\n");

    /* ==== 3. form+redirect harness: authorization URL building (form
     * encoding with '+' for space, param ordering, extraParams), redirect
     * validation (verbatim + loopback port-ignore + private-use dot), the
     * authorization response parser, the token request body builder, and
     * the JWT wrapper (degrades to a throw without CONFIG_TLS). */
    run(ctx,
        "import * as o from 'dyna:oauth2';\n"
        "const t = f => { try { return f(); } catch (e) { return null; } };\n"
        "const opts = { authorizationEndpoint: 'https://auth.example.com/a',\n"
        "  clientId: FUZZ.slice(0, 64), redirectUri: FUZZ.slice(0, 128),\n"
        "  scope: FUZZ.slice(0, 128), state: FUZZ.slice(0, 64),\n"
        "  extraParams: { a: FUZZ.slice(0, 64), b: FUZZ.slice(0, 64) } };\n"
        "t(() => o.buildAuthorizationUrl(opts));\n"
        "t(() => o.buildAuthorizationUrl(Object.assign({}, opts,\n"
        "  { authorizationEndpoint: 'http://' + FUZZ.slice(0, 64),\n"
        "    allowInsecure: true })));\n"
        "t(() => o.isValidRedirectUri(FUZZ.slice(0, 256),\n"
        "  ['https://client.example.com/cb', 'http://127.0.0.1:8080/cb',\n"
        "   'com.example.app:/oauth2redirect']));\n"
        "t(() => o.parseAuthorizationResponse(FUZZ));\n"
        "t(() => o.parseAuthorizationResponse(FUZZ + '#access_token=x'));\n"
        "t(() => o.buildTokenRequestBody({ grant_type: 'authorization_code',\n"
        "  code: FUZZ.slice(0, 256), redirect_uri: FUZZ.slice(0, 128),\n"
        "  client_id: FUZZ.slice(0, 64) }));\n"
        "t(() => o.buildTokenRequestBody(JSON.parse(FUZZ)));\n"
        "t(() => o.verifyJWT(FUZZ, FUZZB, { algorithms: ['HS256'],\n"
        "  aud: FUZZ.slice(0, 64), iss: FUZZ.slice(0, 64),\n"
        "  requiredScope: [FUZZ.slice(0, 16)], clockSkewSec: 60 }));\n");
#endif

    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    return 0;
}