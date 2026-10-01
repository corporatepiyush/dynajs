#include "dynajs.h"
#include "dyna-nat.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static void run(JSContext* ctx, const char* src)
{
    JSValue v = JS_Eval(ctx, src, strlen(src), "<oauth2-fuzz>",
        JS_EVAL_TYPE_MODULE);
    if (JS_IsException(v)) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        abort();
    }
    JS_FreeValue(ctx, v);
}

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    JSRuntime* rt;
    JSContext* ctx;
    JSValue global, s;
    char* exact;
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

    exact = (char*)malloc(size ? size : 1);
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
    JS_SetPropertyStr(ctx, global, "FUZZ", s);
    {
        static const uint8_t stub = 0;
        JSValue ab = JS_NewArrayBufferCopy(ctx, size ? data : &stub, size);
        if (!JS_IsException(ab)) {
            JSValueConst ta[3];
            JSValue u8;
            ta[0] = ab;
            ta[1] = JS_UNDEFINED;
            ta[2] = JS_UNDEFINED;
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
    for (i = 0; i < 16 && i < size; i++)
        hex[i] = "0123456789abcdef"[data[i] & 15];
    while (i < 16)
        hex[i++] = 'a';
    hex[16] = '\0';
    JS_SetPropertyStr(ctx, global, "FUZZH",
        JS_NewStringLen(ctx, hex, 16));
    JS_FreeValue(ctx, global);

#ifdef CONFIG_NATIVE_MODULES
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