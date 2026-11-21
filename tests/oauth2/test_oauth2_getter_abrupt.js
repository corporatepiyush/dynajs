// flags: --std
// timeout: 120
// covers: contract dyna:oauth2.buildAuthorizationUrl dyna:oauth2.buildTokenRequestBody dyna:oauth2.parseBearerFromRequest dyna:oauth2.parseTokenResponse dyna:oauth2.verifyJWT dyna:oauth2.formatScope -- D49: every option, element and payload read propagates a throwing getter or Proxy trap
// st1m D49: these natives read options with JS_GetPropertyStr and type-tested the result before any JS_IsException
// check, so a throwing getter was read as "absent" or "wrong type": the call went on (allowInsecure/allowPlain turned
// the exception into true, state was generated instead, extraParams were dropped) with the error still pending, or a
// module TypeError replaced the caller's error. Parsed JSON (token responses, JWT payloads) inherits from
// Object.prototype, so an inherited getter for an absent member is reachable too. Oracle: each hostile call throws
// exactly the sentinel and leaves no exception pending (TOML.stringify of a Symbol must throw its own TypeError).
// Also: a GC inside every getter keeps the result intact,
// and a --timeout-ms child's interrupt ends a loop that catches everything.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_oauth2_getter_abrupt");
import * as o from "dyna:oauth2";
import * as crypto from "dyna:crypto";
import { TOML } from "dyna:config";
import { Exec, args } from "dyna:sys";
import * as std from "std";

const SELF = "tests/oauth2/test_oauth2_getter_abrupt.js";
const SENT = new Error("oauth2 getter sentinel");
const slot = () => { try { TOML.stringify({ a: Symbol() }); return "no throw"; } catch (e) { return e === SENT ? "sentinel leaked" : e instanceof TypeError ? "clean" : "threw " + e; } };
const run = (f) => { try { return { v: f() }; } catch (e) { return { e }; } };
const CC = "a".repeat(43);
const AUTH = { authorizationEndpoint: "https://a.example/auth", clientId: "c", redirectUri: "https://app.example/cb", state: "s" };
const AUTH_PLAIN = { ...AUTH, codeChallenge: CC, codeChallengeMethod: "plain", allowPlain: true };
const with_ = (base, k, get) => { const x = { ...base }; delete x[k]; Object.defineProperty(x, k, { get, enumerable: true, configurable: true }); return x; };
const thrower = () => { throw SENT; };
const key = new TextEncoder().encode("test-secret-32-bytes-long-xxxxxx");
const hasJwt = typeof crypto.JWTSign === "function";
const now = Math.floor(Date.now() / 1000);
const TOKEN = hasJwt ? crypto.JWTSign({ sub: "u1", aud: "x", iss: "i", exp: now + 3600, scope: "read" }, key, { alg: "HS256" }) : "";
const VOPTS = { algorithms: ["HS256"], aud: "x", iss: "i", requiredScope: ["read"], clockSkewSec: 5, requireExp: true };
const inherited = (k, f) => { Object.defineProperty(Object.prototype, k, { get: thrower, configurable: true }); try { return f(); } finally { delete Object.prototype[k]; } };
const arrProxy = (a, bad) => new Proxy(a, { get(t, k) { if (k === bad) throw SENT; return Reflect.get(t, k); } });

const PINNED = [
    ["auth url", () => o.buildAuthorizationUrl(AUTH).url, "https://a.example/auth?response_type=code&client_id=c&redirect_uri=https%3A%2F%2Fapp.example%2Fcb&state=s"],
    ["token body", () => o.buildTokenRequestBody({ grant_type: "code", code: "x y" }), "grant_type=code&code=x+y"],
    ["bearer", () => o.parseBearerFromRequest({ headers: { authorization: "Bearer t1" } }), "t1"],
    ["token response", () => o.parseTokenResponse('{"access_token":"a","token_type":"Bearer","expires_in":5}').access_token, "a"],
    ["format scope", () => o.formatScope(["a", "b"]), "a b"],
];

if (args().includes("--child")) {
    const mode = args()[args().indexOf("--child") + 1];
    {
        let caught = 0;
        for (;;) {
            try { o.buildAuthorizationUrl(with_(AUTH, "allowInsecure", thrower)); } catch (e) { caught++; }
            try { o.parseBearerFromRequest(with_({}, "headers", thrower)); } catch (e) { caught++; }
        }
    }
} else {
    let n = 0, fails = 0;
    const ok = (c, m) => { n++; if (!c) { fails++; print("FAIL: " + m); } };
    const sent = (name, f) => { const r = run(f); const s = slot(); ok(r.e === SENT && s === "clean", name + ": " + (r.e === SENT ? "sentinel" : r.e ? "threw " + r.e : "returned " + JSON.stringify(r.v)) + ", " + s); };
    await __kitS.section("pinned results", () => {
        for (const [name, f, want] of PINNED) { const r = run(f); ok(!r.e && r.v === want, name + ": " + (r.e ? "threw " + r.e : JSON.stringify(r.v))); }
        ok(o.buildAuthorizationUrl(AUTH_PLAIN).url.endsWith("code_challenge_method=plain"), "plain challenge with allowPlain");
    });
    await __kitS.section("buildAuthorizationUrl: every option getter and Proxy trap propagates", () => {
        for (const k of ["authorizationEndpoint", "clientId", "redirectUri", "allowInsecure", "scope", "state", "codeChallenge", "codeChallengeMethod", "responseType", "extraParams"])
            sent("auth " + k, () => o.buildAuthorizationUrl(with_(AUTH, k, thrower)));
        sent("auth allowPlain", () => o.buildAuthorizationUrl(with_(AUTH_PLAIN, "allowPlain", thrower)));
        sent("auth extraParams value", () => o.buildAuthorizationUrl({ ...AUTH, extraParams: with_({ a: "1" }, "b", thrower) }));
        sent("auth extraParams ownKeys", () => o.buildAuthorizationUrl({ ...AUTH, extraParams: new Proxy({}, { ownKeys: thrower }) }));
        sent("auth extraParams get trap", () => o.buildAuthorizationUrl({ ...AUTH, extraParams: new Proxy({ a: "1" }, { get: thrower }) }));
    });
    await __kitS.section("buildTokenRequestBody, formatScope, parseBearerFromRequest", () => {
        sent("token body value", () => o.buildTokenRequestBody(with_({ grant_type: "code" }, "code", thrower)));
        sent("formatScope element", () => o.formatScope(arrProxy(["a", "b"], "1")));
        sent("formatScope length", () => o.formatScope(arrProxy(["a", "b"], "length")));
        for (const k of ["headers", "query", "body", "allowQuery", "allowBody"])
            sent("bearer " + k, () => o.parseBearerFromRequest(with_({ query: "access_token=t", body: "access_token=t", allowQuery: true, allowBody: true }, k, thrower)));
        sent("bearer header value", () => o.parseBearerFromRequest({ headers: with_({}, "authorization", thrower) }));
    });
    await __kitS.section("parsed JSON: an inherited getter for an absent member propagates", () => {
        for (const k of ["access_token", "token_type", "expires_in", "scope", "refresh_token"]) {
            const body = { access_token: "a", token_type: "Bearer" };
            delete body[k];
            sent("token response " + k, () => inherited(k, () => o.parseTokenResponse(JSON.stringify(body))));
        }
    });
    await __kitS.section("verifyJWT: option, list element and payload getters propagate", () => {
        if (!hasJwt) { ok(true, "no JWT backend"); return; }
        ok(o.verifyJWT(TOKEN, key, VOPTS).sub === "u1", "CONTROL: token verifies");
        for (const k of ["algorithms", "aud", "iss", "requiredScope", "clockSkewSec", "requireExp"])
            sent("verify opt " + k, () => o.verifyJWT(TOKEN, key, with_(VOPTS, k, thrower)));
        sent("verify algorithms element", () => o.verifyJWT(TOKEN, key, { ...VOPTS, algorithms: arrProxy(["HS256"], "0") }));
        sent("verify requiredScope element", () => o.verifyJWT(TOKEN, key, { ...VOPTS, requiredScope: arrProxy(["read"], "0") }));
        for (const k of ["exp", "nbf", "aud", "iss", "scope"])
        {
            const p = { sub: "u1", exp: now + 3600, aud: "x", iss: "i", scope: "read" };
            delete p[k];
            const tk = crypto.JWTSign(p, key, { alg: "HS256" });
            ok(run(() => o.verifyJWT(tk, key, { algorithms: ["HS256"] })).v?.sub === "u1", "CONTROL: token without " + k + " verifies with no requirement");
            sent("verify payload " + k, () => inherited(k, () => o.verifyJWT(tk, key, { algorithms: ["HS256"], aud: "x", iss: "i", requiredScope: ["read"], requireExp: true })));
        }
    });
    await __kitS.section("a GC inside every getter keeps the result intact", () => {
        const gc = (base) => { const x = {}; for (const k of Object.keys(base)) { const v = base[k]; Object.defineProperty(x, k, { get() { std.gc(); return v; }, enumerable: true }); } return x; };
        ok(o.buildAuthorizationUrl(gc(AUTH)).url === PINNED[0][2], "gc auth url");
        ok(o.buildAuthorizationUrl(gc(AUTH_PLAIN)).url === o.buildAuthorizationUrl(AUTH_PLAIN).url, "gc auth url plain");
        ok(o.buildTokenRequestBody(gc({ grant_type: "code", code: "x y" })) === PINNED[1][2], "gc token body");
        ok(o.parseBearerFromRequest(gc({ headers: gc({ authorization: "Bearer t1" }) })) === "t1", "gc bearer");
        if (hasJwt) ok(o.verifyJWT(TOKEN, key, gc(VOPTS)).sub === "u1", "gc verify");
    });
    await __kitS.section("an interrupt that must end a catch-all loop", () => {
        const t0 = Date.now();
        const i = Exec(args()[0], ["--std", "--timeout-ms", "300", SELF, "--child", "interrupt"], { timeoutMs: 20000 });
        ok(i.code !== 0 && !i.timedOut && /interrupt/i.test(i.stderr + i.stdout), "interrupt child ends: rc " + i.code + " after " + (Date.now() - t0) + " ms " + i.stderr.slice(-200));
    });
    if (fails) throw new Error(fails + " of " + n + " checks failed");
    print("test_oauth2_getter_abrupt: " + n + " checks passed");
}
