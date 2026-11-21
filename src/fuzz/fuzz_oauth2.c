#include "dynajs.h"
#include "dyna-nat.h"
#include "src/fuzz/fuzz_common.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static fz_env_t env;

void js_str_atom_unpin_all(JSRuntime* rt);

static const char SRC[] =
    "import * as o from 'dyna:oauth2';\n"
    "const bad = m => { throw new Error('oracle: ' + m); };\n"
    "const t = f => { try { return { v: f() }; } catch (e) { if (String(e && e.message).startsWith('oracle:')) throw e; return null; } };\n"
    "const B64T = /^[A-Za-z0-9\\-._~+\\/]+=*$/, B64S = /^[A-Za-z0-9\\-._~+\\/]+={0,2}$/;\n"
    "const b64t = (s, st) => st ? s.length <= 4096 && B64S.test(s) : B64T.test(s);\n"
    "const VERIF = /^[A-Za-z0-9\\-._~]{43,128}$/;\n"
    "const NQ = /^[\\x21\\x23-\\x5B\\x5D-\\x7E]+$/;\n"
    "const ERRI = /^[A-Za-z0-9\\-._~+\\/]{1,64}$/, NQS = /^[\\x20\\x21\\x23-\\x5B\\x5D-\\x7E]+$/;\n"
    "const ABC = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/';\n"
    "const formDec = s => decodeURIComponent(s.replace(/\\+/g, ' '));\n"
    "const query = u => {\n"
    "    const i = u.indexOf('?'), m = {};\n"
    "    if (i < 0) return m;\n"
    "    for (const kv of u.slice(i + 1).split('&')) {\n"
    "        if (!kv) continue;\n"
    "        const j = kv.indexOf('='), k = formDec(j < 0 ? kv : kv.slice(0, j));\n"
    "        if (Object.prototype.hasOwnProperty.call(m, k)) bad('duplicate parameter ' + k);\n"
    "        m[k] = j < 0 ? '' : formDec(kv.slice(j + 1));\n"
    "    }\n"
    "    return m;\n"
    "};\n"
    "const b64utf8 = s => {\n"
    "    let bits = 0, acc = 0, pct = '';\n"
    "    for (const c of s.replace(/=+$/, '')) {\n"
    "        const v = ABC.indexOf(c);\n"
    "        if (v < 0) bad('Basic credentials are not base64');\n"
    "        acc = acc << 6 | v; bits += 6;\n"
    "        if (bits >= 8) { bits -= 8; pct += '%' + ((acc >> bits) & 255).toString(16).padStart(2, '0'); }\n"
    "    }\n"
    "    return decodeURIComponent(pct);\n"
    "};\n"
    "const same = (a, b) => JSON.stringify(a) === JSON.stringify(b);\n"
    "globalThis.__fz = (sel, s, b, k) => {\n"
    "    const cut = Math.min(k, s.length);\n"
    "    switch (sel % 8) {\n"
    "    case 0: {\n"
    "        const st = !!(sel & 8);\n"
    "        if (sel & 32) s = \"a\".repeat(4090 + (k & 15)) + s.slice(0, k % 5);\n"
    "        const v = st || sel & 16 ? o.isValidBearerToken(s, { strict: st }) : o.isValidBearerToken(s);\n"
    "        if (v !== b64t(s, st)) bad(st ? \"isValidBearerToken strict differs from b64token with at most two = and 4096 bytes\" : \"isValidBearerToken differs from the RFC 6750 b64token grammar\");\n"
    "        if (st && v && !o.isValidBearerToken(s)) bad(\"strict accepted a token the RFC default refused\");\n"
    "        if (st) break;\n"
    "        const h = t(() => o.buildBearerHeader(s));\n"
    "        if (v) {\n"
    "            if (!h || h.v !== 'Bearer ' + s) bad('buildBearerHeader of a valid token');\n"
    "            if (o.parseBearerHeader(h.v) !== s) bad('parseBearerHeader(buildBearerHeader(x)) !== x');\n"
    "            if (o.parseBearerFromRequest({ headers: { authorization: h.v } }) !== s) bad('parseBearerFromRequest header round trip');\n"
    "            if (o.parseBearerFromRequest({ headers: { [\"authorization\\0\" + s.slice(0, cut % 3)]: h.v } }) !== null) bad(\"a header named authorization plus a NUL suffix was taken as Authorization\");\n"
    "        } else if (h) bad('buildBearerHeader accepted a token outside b64token');\n"
    "        const p = t(() => o.parseBearerHeader(s));\n"
    "        if (p && p.v !== null && !b64t(p.v)) bad('parseBearerHeader returned a non-b64token');\n"
    "        const r = t(() => o.parseBearerFromRequest({ headers: { Authorization: s }, query: s, body: s, allowQuery: true, allowBody: true }));\n"
    "        if (r && r.v !== null && !b64t(r.v)) bad('parseBearerFromRequest returned a non-b64token');\n"
    "        break;\n"
    "    }\n"
    "    case 1: {\n"
    "        const r = t(() => o.parseScope(s));\n"
    "        if (r) {\n"
    "            for (const x of r.v) if (!NQ.test(x)) bad('parseScope returned a token outside NQCHAR');\n"
    "            const r2 = o.parseScope(o.formatScope(r.v));\n"
    "            if (!same(r2, r.v)) bad('parseScope(formatScope(parseScope(s))) differs');\n"
    "        }\n"
    "        if (/^[\\x21\\x23-\\x5B\\x5D-\\x7E]+( [\\x21\\x23-\\x5B\\x5D-\\x7E]+)*$/.test(s) && !r) bad('parseScope refused a well-formed scope');\n"
    "        break;\n"
    "    }\n"
    "    case 2: {\n"
    "        const U = \"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-._~\";\n"
    "        const v = sel & 16 ? Array.from({ length: 36 + (cut + 64 * ((sel >> 5) & 1)) % 100 }, (_, i) => U[(s.charCodeAt(i % (s.length || 1)) || i) % 66]).join(\"\") : s.slice(0, 160);\n"
    "        if (o.isValidCodeVerifier(v) !== VERIF.test(v)) bad('isValidCodeVerifier differs from RFC 7636 4.1');\n"
    "        if (VERIF.test(v)) {\n"
    "            const c = o.generateCodeChallenge(v, 'S256');\n"
    "            if (!/^[A-Za-z0-9_-]{43}$/.test(c)) bad('S256 challenge is not 43 base64url chars');\n"
    "            if (!o.verifyCodeChallenge(v, c, 'S256')) bad('S256 round trip');\n"
    "            if (o.verifyCodeChallenge(v, c.slice(0, cut % 43) + (c[cut % 43] === 'A' ? 'B' : 'A') + c.slice(cut % 43 + 1), 'S256')) bad('S256 accepted a changed challenge');\n"
    "            if (o.generateCodeChallenge(v, 'plain') !== v || !o.verifyCodeChallenge(v, v, 'plain')) bad('plain challenge');\n"
    "            if (o.verifyCodeChallenge(v, v + 'x', 'plain')) bad('plain accepted a longer challenge');\n"
    "        }\n"
    "        break;\n"
    "    }\n"
    "    case 3: {\n"
    "        const id = s.slice(0, cut), sec = s.slice(cut);\n"
    "        const h = t(() => o.buildClientAuthHeader(id, sec));\n"
    "        if (h) {\n"
    "            if (!h.v.startsWith('Basic ')) bad('client auth header prefix');\n"
    "            const d = b64utf8(h.v.slice(6)), j = d.indexOf(':');\n"
    "            if (j < 0 || formDec(d.slice(0, j)) !== id || formDec(d.slice(j + 1)) !== sec) bad('client auth header does not decode to formEncode(id):formEncode(secret)');\n"
    "        }\n"
    "        break;\n"
    "    }\n"
    "    case 4: {\n"
    "        const p = { grant_type: 'authorization_code', code: s.slice(0, cut), redirect_uri: s.slice(cut, 2 * cut), ['x' + s.slice(2 * cut, 2 * cut + 4)]: s.slice(2 * cut) };\n"
    "        const r = t(() => o.buildTokenRequestBody(p));\n"
    "        if (r && !same(Object.entries(query('?' + r.v)).sort(), Object.entries(p).sort())) bad('buildTokenRequestBody does not form-decode to its params');\n"
    "        break;\n"
    "    }\n"
    "    case 5: {\n"
    "        const opts = { authorizationEndpoint: 'https://auth.example.com/a', clientId: s.slice(0, cut), redirectUri: 'https://c.example/cb/' + s.slice(cut, 2 * cut),\n"
    "            scope: s.slice(2 * cut, 3 * cut), state: s.slice(3 * cut), extraParams: {} };\n"
    "        const RES = [\"response_type\", \"client_id\", \"redirect_uri\", \"scope\", \"state\", \"code_challenge\", \"code_challenge_method\"];\n"
    "        const ek = cut % 5 === 0 ? RES[(sel >> 3) % 7] : sel & 64 ? \"e\\0\" + (sel >> 7) : \"e\" + s.slice(0, 3), ev = s.slice(cut, 3 * cut);\n"
    "        opts.extraParams[ek] = ev;\n"
    "        const r = t(() => o.buildAuthorizationUrl(opts));\n"
    "        if (r) {\n"
    "            const m = query(r.v.url);\n"
    "            if (!r.v.url.startsWith('https://auth.example.com/a?')) bad('authorization url endpoint');\n"
    "            if (m.response_type !== 'code' || m.client_id !== opts.clientId || m.redirect_uri !== opts.redirectUri) bad('authorization url params');\n"
    "            if (m.state !== r.v.state || (opts.state && r.v.state !== opts.state)) bad('authorization url state');\n"
    "            if (opts.scope && m.scope !== opts.scope) bad('authorization url scope');\n"
    "            if (!RES.includes(ek) && /[\\x00-\\x1f\\x7f]/.test(ek + ev)) bad(\"extraParams with a CTL accepted\");\n"
    "            if (!RES.includes(ek) && m[ek] !== ev) bad(\"extraParams do not round-trip\");\n"
    "        }\n"
    "        const code = s.slice(0, cut), st = s.slice(cut);\n"
    "        const a = t(() => o.parseAuthorizationResponse('https://c.example/cb?code=' + encodeURIComponent(code) + '&state=' + encodeURIComponent(st)));\n"
    "        if (a && (a.v.code !== code || (st && a.v.state !== st))) bad('parseAuthorizationResponse round trip');\n"
    "        t(() => o.parseAuthorizationResponse(s));\n"
    "        break;\n"
    "    }\n"
    "    case 6: {\n"
    "        if (!o.secureCompare(s, s) || o.secureCompare(s, s + 'x') || !o.secureCompare(b, b)) bad('secureCompare');\n"
    "        const r = t(() => o.parseTokenResponse(s));\n"
    "        if (r && (!b64t(r.v.access_token) || !/^bearer$/i.test(r.v.token_type))) bad('parseTokenResponse accepted a bad access_token or token_type');\n"
    "        const tok = s.slice(0, cut);\n"
    "        const r2 = t(() => o.parseTokenResponse(JSON.stringify({ access_token: tok, token_type: 'Bearer', expires_in: 3600 })));\n"
    "        if (!!r2 !== b64t(tok)) bad('parseTokenResponse verdict differs from the b64token grammar');\n"
    "        if (r2 && r2.v.access_token !== tok) bad('parseTokenResponse access_token');\n"
    "        break;\n"
    "    }\n"
    "    default: {\n"
    "        const T68 = \"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-._~+/\";\n"
    "        const e = k % 3 === 0 ? Array.from({ length: [64, 65, k % 70, 66][(k >> 2) & 3] }, (_, i) => T68[(s.charCodeAt(i % (s.length || 1)) || i) % 68]).join(\"\") : s.slice(0, cut);\n"
    "        const wo = { error: e }, Q = /^[\\x20\\x21\\x23-\\x5B\\x5D-\\x7E]*$/, rest = s.slice(cut);\n"
    "        if (sel & 16) wo.realm = rest.slice(0, 8);\n"
    "        if (sel & 32) wo.errorDescription = rest.slice(8, 24);\n"
    "        if (sel & 64) wo.error_uri = rest.slice(24, 40);\n"
    "        if (sel & 128) wo.scope = rest.slice(40);\n"
    "        const st = !!(sel & 8);\n"
    "        if (st || k & 128) wo.strict = st;\n"
    "        const ok = (st ? ERRI : NQS).test(e) && (wo.error_uri === undefined || /^[\\x21\\x23-\\x5B\\x5D-\\x7E]*$/.test(wo.error_uri)) && [wo.realm, wo.errorDescription].every(x => x === undefined || Q.test(x)) && (wo.scope === undefined || /^[\\x20\\x21\\x23-\\x5B\\x5D-\\x7E]*$/.test(wo.scope));\n"
    "        const w = t(() => o.buildWWWAuthenticate(wo));\n"
    "        if (!!w !== ok) bad(st ? \"buildWWWAuthenticate strict verdict differs (error token68 1..64)\" : \"buildWWWAuthenticate verdict differs from RFC 6749 A.7/A.8\");\n"
    "        if (w && wo.error_uri && w.v.indexOf('error_uri=\"' + wo.error_uri + '\"') < 0) bad(\"buildWWWAuthenticate dropped error_uri\");\n"
    "        if (w && wo.realm !== undefined && !w.v.startsWith('Bearer realm=\"' + wo.realm + '\"')) bad(\"buildWWWAuthenticate realm not first or not quoted\");\n"
    "        if (w && wo.errorDescription && w.v.indexOf('error_description=\"' + wo.errorDescription + '\"') < 0) bad(\"buildWWWAuthenticate dropped error_description\");\n"
    "        if (w && (!w.v.startsWith('Bearer ') || w.v.indexOf('error=\"' + e + '\"') < 0)) bad('buildWWWAuthenticate shape');\n"
    "        const reg = ['https://client.example.com/cb', 'http://127.0.0.1:8080/cb', 'com.example.app:/oauth2redirect'];\n"
    "        if (!o.isValidRedirectUri(reg[cut % 3], reg)) bad('isValidRedirectUri refused a registered uri');\n"
    "        const rv = t(() => o.isValidRedirectUri(s, reg));\n"
    "        if (rv && rv.v && reg.indexOf(s) < 0 && !/^http:\\/\\/(127\\.0\\.0\\.1|\\[::1\\])(:\\d+)?\\/cb$/.test(s)) bad('isValidRedirectUri accepted an unregistered uri');\n"
    "        if (sel & 8) t(() => o.verifyJWT(s, b, { algorithms: ['HS256'], aud: s.slice(0, 16), clockSkewSec: 60 }));\n"
    "    }\n"
    "    }\n"
    "};\n"
    "";

static void init(JSContext* ctx)
{
    JSValue v;
#ifdef CONFIG_NATIVE_MODULES
    if (js_nat_init_all(ctx) < 0)
        fz_fail("js_nat_init_all");
#endif
    v = JS_Eval(ctx, SRC, sizeof SRC - 1, "<oauth2-fuzz>", JS_EVAL_TYPE_MODULE);
    fuzz_drain_or_abort(ctx, v);
    JS_FreeValue(ctx, v);
}

static void oracle_check(JSContext* ctx)
{
    JSValue x = JS_GetException(ctx);
    const char* m = JS_ToCString(ctx, x);
    if (m && !strncmp(m, "Error: oracle: ", 15)) {
        fprintf(stderr, "oracle: %s\n", m + 15);
        abort();
    }
    if (m)
        JS_FreeCString(ctx, m);
    JS_Throw(ctx, x);
}

static void run(fz_env_t* e, const uint8_t* data, size_t size)
{
    JSContext* ctx = e->ctx[0];
    JSValue g, f, r, a[4];
    unsigned sel = size ? data[0] : 0, k = size > 1 ? data[1] : 0;
    const uint8_t* p = data + (size > 2 ? 2 : size);
    size_t n = size > 2 ? size - 2 : 0;
    fz_arm(500000000u, 0);
    g = JS_GetGlobalObject(ctx);
    f = JS_GetPropertyStr(ctx, g, "__fz");
    JS_FreeValue(ctx, g);
    if (JS_IsException(f))
        abort();
    a[0] = JS_NewInt32(ctx, (int32_t)sel);
    a[1] = JS_NewStringLen(ctx, (const char*)p, n);
    a[2] = JS_NewArrayBufferCopy(ctx, n ? p : data, n);
    a[3] = JS_NewInt32(ctx, (int32_t)k);
    r = JS_Call(ctx, f, JS_UNDEFINED, 4, a);
    if (JS_IsException(r))
        oracle_check(ctx);
    fz_settle(ctx, r, NULL);
    JS_FreeValue(ctx, a[0]);
    JS_FreeValue(ctx, a[1]);
    JS_FreeValue(ctx, a[2]);
    JS_FreeValue(ctx, a[3]);
    JS_FreeValue(ctx, f);
    js_str_atom_unpin_all(e->rt);
}

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    if (size <= 8192)
        fz_one(&env, data, size);
    return 0;
}

int LLVMFuzzerInitialize(int* argc, char*** argv)
{
    fz_args(*argc, *argv);
    fz_env_open(&env, 1, 256, init, run);
    return 0;
}
