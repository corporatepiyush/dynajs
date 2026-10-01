// bb_oauth2.js -- black-box contract tests for dyna:oauth2 (pure surface:
// PKCE, state, bearer/scope codecs, authorize-URL builder, token bodies).
//
// Contract: dynajs.d.ts lines 6085-6137 (the "dyna:oauth2" module section).
// Slice used: /tmp/dyna_contract/oauth2.d.ts.
//
// Expectation bases (all hand-derived, NO network, NO external runtime):
//  * RFC 7636 Appendix B worked example:
//      verifier  "dBjftJeZ4CVP-mB92K27uhbUJU1p1r_wW1gFWFOEjXk"
//      challenge "E9Melhoa2OwvFrEMTJguCHaoeK1t8URWbuGJSstw-cM"
//      (S256 = BASE64URL(SHA256(verifier)))
//  * RFC 6749 2.3.1 Basic auth: form-encode id and secret, join with ":",
//    base64 -- "cid:secret" groups as 63 69 64 / 3a 73 65 / 63 72 65 / 74,
//    which is base64 "Y2lkOnNlY3JldA==" (hand-checked).
//  * RFC 6750 b64token = 1*( ALPHA / DIGIT / "-" / "." / "_" / "~" / "+" / "/" )
//    *( "=" ); NQCHAR (RFC 6749 A.4) = %x21 / %x23-5B / %x5D-7E; PKCE
//    verifier = 43..128 unreserved chars (32 octets -> 43-char base64url).

import * as oauth2 from "dyna:oauth2";
import { URLSearchParams } from "dyna:url";

let n = 0;
function assert(cond, msg) { n++; if (!cond) throw new Error("assertion failed: " + msg); }
function assertEq(actual, expected, msg) { n++; const ok = Object.is(actual, expected) || (typeof actual === "number" && typeof expected === "number" && Number.isNaN(actual) && Number.isNaN(expected)); if (!ok) throw new Error("assertion failed: " + msg + " — got |" + actual + "| expected |" + expected + "|"); }
function assertDeepEq(a, b, msg) { n++; if (JSON.stringify(a) !== JSON.stringify(b)) throw new Error("assertion failed (deep): " + msg); }
function assertThrows(fn, msg, ErrType, errPattern) { n++; let threw = false, e = null; try { fn(); } catch (err) { threw = true; e = err; } if (!threw) throw new Error("expected throw: " + msg); if (ErrType && !(e instanceof ErrType)) throw new Error("wrong error type: " + msg); }

function runTable(tname, rows, fn) {
    for (const row of rows) {
        try { fn(row); }
        catch (e) { throw new Error("table " + tname + " row [" + row[0] + "]: " + (e && e.message ? e.message : String(e))); }
    }
}

const B64URL = /^[A-Za-z0-9_-]*$/;
const RFC7636_VERIFIER = "dBjftJeZ4CVP-mB92K27uhbUJU1p1r_wW1gFWFOEjXk";
const RFC7636_CHALLENGE = "E9Melhoa2OwvFrEMTJguCHaoeK1t8URWbuGJSstw-cM";

// --------------------------------------------------------------
// T1: generateCodeVerifier / isValidCodeVerifier (slice: 43..128
// unreserved chars; 32 octet CSPRNG -> 43 char base64url)
// --------------------------------------------------------------
runTable("codeVerifier", [
    ["generated verifier is 43 chars (32 bytes -> base64url)", () => {
        const v = oauth2.generateCodeVerifier();
        assertEq(v.length, 43, "verifier length");
        assert(B64URL.test(v), "verifier is base64url charset: " + v);
    }],
    ["two generated verifiers differ (CSPRNG)", () => {
        assert(oauth2.generateCodeVerifier() !== oauth2.generateCodeVerifier(), "verifiers differ");
    }],
    ["generated verifier passes isValidCodeVerifier", () => assertEq(oauth2.isValidCodeVerifier(oauth2.generateCodeVerifier()), true, "generated is valid")],
    ["43 chars (min) is valid", () => assertEq(oauth2.isValidCodeVerifier("a".repeat(43)), true, "43 valid")],
    ["44 chars is valid", () => assertEq(oauth2.isValidCodeVerifier("a".repeat(44)), true, "44 valid")],
    ["128 chars (max) is valid", () => assertEq(oauth2.isValidCodeVerifier("a".repeat(128)), true, "128 valid")],
    ["42 chars (below min) is invalid", () => assertEq(oauth2.isValidCodeVerifier("a".repeat(42)), false, "42 invalid")],
    ["129 chars (above max) is invalid", () => assertEq(oauth2.isValidCodeVerifier("a".repeat(129)), false, "129 invalid")],
    ["empty string is invalid", () => assertEq(oauth2.isValidCodeVerifier(""), false, "empty invalid")],
    ["'+' is not an unreserved char", () => assertEq(oauth2.isValidCodeVerifier("+".repeat(43)), false, "plus invalid")],
    ["'=' is not an unreserved char", () => assertEq(oauth2.isValidCodeVerifier("=".repeat(43)), false, "equals invalid")],
    ["'$' is not an unreserved char", () => assertEq(oauth2.isValidCodeVerifier("$".repeat(43)), false, "dollar invalid")],
    ["space is not an unreserved char", () => assertEq(oauth2.isValidCodeVerifier("a b".padEnd(43, "c")), false, "space invalid")],
    ["'.' '_' '~' are unreserved members", () => assertEq(oauth2.isValidCodeVerifier("a.b_c~" + "d".repeat(37)), true, "dot/underscore/tilde valid")],
], row => row[1]());

// --------------------------------------------------------------
// T2: generateCodeChallenge / verifyCodeChallenge (slice: S256 =
// BASE64URL(SHA256(verifier)), plain = verifier, S256 default)
// --------------------------------------------------------------
runTable("codeChallenge", [
    ["S256 matches the RFC 7636 Appendix B vector", () => assertEq(oauth2.generateCodeChallenge(RFC7636_VERIFIER, "S256"), RFC7636_CHALLENGE, "S256 vector")],
    ["S256 is the default method", () => assertEq(oauth2.generateCodeChallenge(RFC7636_VERIFIER), RFC7636_CHALLENGE, "default is S256")],
    ["plain returns the verifier itself", () => assertEq(oauth2.generateCodeChallenge(RFC7636_VERIFIER, "plain"), RFC7636_VERIFIER, "plain passthrough")],
    ["challenge is 43-char base64url for a 43-char verifier", () => {
        const c = oauth2.generateCodeChallenge(RFC7636_VERIFIER);
        assertEq(c.length, 43, "challenge length (SHA256 32 bytes -> base64url)");
        assert(B64URL.test(c), "challenge charset");
    }],
    ["verifyCodeChallenge accepts the matching pair (S256)", () => assertEq(oauth2.verifyCodeChallenge(RFC7636_VERIFIER, RFC7636_CHALLENGE, "S256"), true, "S256 verify true")],
    ["verifyCodeChallenge accepts the plain pair", () => assertEq(oauth2.verifyCodeChallenge(RFC7636_VERIFIER, RFC7636_VERIFIER, "plain"), true, "plain verify true")],
    ["verifyCodeChallenge rejects a wrong challenge", () => assertEq(oauth2.verifyCodeChallenge(RFC7636_VERIFIER, "AAAA" + RFC7636_CHALLENGE.slice(4), "S256"), false, "wrong challenge false")],
    // refusal rows: bad verifier length -> predicate false
    ["verifyCodeChallenge rejects a too-short verifier", () => assertEq(oauth2.verifyCodeChallenge("short", RFC7636_CHALLENGE, "S256"), false, "short verifier false")],
    ["verifyCodeChallenge rejects a mismatched verifier", () => assertEq(oauth2.verifyCodeChallenge("X" + RFC7636_VERIFIER.slice(1), RFC7636_CHALLENGE, "S256"), false, "wrong verifier false")],
    // TEST-FIX (DOC-TENSION resolved): an unknown method is an ARGUMENT refusal, not a
    // verification failure -- the house doctrine "argument refusals throw synchronously", and
    // verifyCodeChallenge is symmetric with generateCodeChallenge, which refuses the same
    // outside-union method with a TypeError (RFC 7636: an unsupported transform is an error).
    ["verifyCodeChallenge with an unknown method throws", () => assertThrows(() => oauth2.verifyCodeChallenge(RFC7636_VERIFIER, RFC7636_CHALLENGE, "S512"), "unknown method", TypeError)],
], row => row[1]());

// --------------------------------------------------------------
// T3: generateState (slice: CSPRNG bytes -> base64url; default 32
// bytes).  base64url unpadded length = ceil(bytes*8/6).
// --------------------------------------------------------------
runTable("state", [
    ["default is 43 chars (32 bytes)", () => assertEq(oauth2.generateState().length, 43, "default length")],
    ["16 bytes -> 22 chars", () => assertEq(oauth2.generateState(16).length, 22, "16-byte length")],
    ["32 bytes -> 43 chars", () => assertEq(oauth2.generateState(32).length, 43, "32-byte length")],
    ["48 bytes -> 64 chars (exact group)", () => assertEq(oauth2.generateState(48).length, 64, "48-byte length")],
    ["charset is base64url", () => assert(B64URL.test(oauth2.generateState()), "state charset")],
    ["two calls differ (CSPRNG)", () => assert(oauth2.generateState() !== oauth2.generateState(), "states differ")],
], row => row[1]());

// --------------------------------------------------------------
// T4: secureCompare (slice: constant-time over UTF-8 bytes or raw
// ByteView bytes; same truth table as crypto's TimingSafeEqual)
// --------------------------------------------------------------
runTable("secureCompare", [
    ["equal strings", () => assertEq(oauth2.secureCompare("abc", "abc"), true, "equal strings")],
    ["differing strings", () => assertEq(oauth2.secureCompare("abc", "abd"), false, "differing strings")],
    ["prefix is not equal (length mismatch)", () => assertEq(oauth2.secureCompare("abc", "abcd"), false, "length mismatch")],
    ["both empty", () => assertEq(oauth2.secureCompare("", ""), true, "empty equal")],
    ["equal byte arrays", () => assertEq(oauth2.secureCompare(new Uint8Array([1, 2, 3]), new Uint8Array([1, 2, 3])), true, "equal bytes")],
    ["differing byte arrays", () => assertEq(oauth2.secureCompare(new Uint8Array([1, 2, 3]), new Uint8Array([1, 2, 4])), false, "differing bytes")],
    ["byte arrays of different length", () => assertEq(oauth2.secureCompare(new Uint8Array([1, 2]), new Uint8Array([1, 2, 3])), false, "byte length mismatch")],
    ["string vs bytes (UTF-8 form documented)", () => assertEq(oauth2.secureCompare("abc", new Uint8Array([97, 98, 99])), true, "string vs utf8 bytes")],
    ["multibyte UTF-8 equal", () => assertEq(oauth2.secureCompare("é", "é"), true, "multibyte equal")],
    ["multibyte vs single byte char", () => assertEq(oauth2.secureCompare("é", "e"), false, "multibyte vs ascii")],
], row => row[1]());

// --------------------------------------------------------------
// T5: buildClientAuthHeader (RFC 6749 2.3.1)
// --------------------------------------------------------------
runTable("clientAuthHeader", [
    ["id:secret base64 (hand-computed)", () => assertEq(oauth2.buildClientAuthHeader("cid", "secret"), "Basic Y2lkOnNlY3JldA==", "Basic payload")],
    // DOC-TENSION: with no secret the payload is the encoding of "cid:" (the
    // colon still separates id from an empty secret per RFC 6749); the exact
    // expectation is computed with the engine's own documented btoa codec.
    ["no secret -> id + empty secret after the colon", () => {
        const h = oauth2.buildClientAuthHeader("cid");
        assert(h.indexOf("Basic ") === 0, "scheme Basic, got " + h);
        assertEq(h, "Basic " + btoa("cid:"), "no-secret payload");
    }],
    // TEST-FIX: the doc pins "form-encodes id:secret, then base64" (RFC 6749 2.3.1), so the
    // base64 payload holds the FORM-ENCODED pair: '/' -> %2F and '+' -> %2B. The row previously
    // expected the raw characters after atob.
    ["payload is the form-encoded id:secret", () => {
        const h = oauth2.buildClientAuthHeader("my_id", "my/secret+");
        assertEq(atob(h.slice("Basic ".length)), "my_id:my%2Fsecret%2B", "decoded id:secret");
    }],
], row => row[1]());

// --------------------------------------------------------------
// T6: scope codecs (slice: NQCHAR scope tokens, space-delimited,
// RFC 6749 A.4)
// --------------------------------------------------------------
runTable("scope", [
    ["parseScope splits on spaces", () => assertDeepEq(oauth2.parseScope("read write"), ["read", "write"], "parse two tokens")],
    ["parseScope single token", () => assertDeepEq(oauth2.parseScope("single"), ["single"], "parse one token")],
    ["formatScope joins with spaces", () => assertEq(oauth2.formatScope(["read", "write"]), "read write", "format join")],
    ["formatScope of one token", () => assertEq(oauth2.formatScope(["a"]), "a", "format single")],
    ["format/parse round trip", () => {
        const scopes = ["openid", "profile", "email"];
        assertDeepEq(oauth2.parseScope(oauth2.formatScope(scopes)), scopes, "round trip");
    }],
    // DOC-TENSION: the slice fixes the grammar (NQCHAR, space-delimited) but
    // not empty-run behavior; RFC-wise an empty string carries no tokens.
    ["parseScope of empty string has no tokens", () => assertDeepEq(oauth2.parseScope(""), [], "empty scope")],
    // refusal row: '"' is not an NQCHAR (%x21 excluded), and the slice
    // declares the tokens as NQCHAR -- validation is the documented reading.
    ["parseScope rejects a non-NQCHAR token", () => assertThrows(() => oauth2.parseScope('a"b'), "quote is not NQCHAR")],
], row => row[1]());

// --------------------------------------------------------------
// T7: bearer headers + token validation (RFC 6750 b64token)
// --------------------------------------------------------------
runTable("bearer", [
    ["buildBearerHeader prefixes 'Bearer '", () => assertEq(oauth2.buildBearerHeader("tok123"), "Bearer tok123", "build")],
    ["parseBearerHeader extracts the token", () => assertEq(oauth2.parseBearerHeader("Bearer tok123"), "tok123", "parse")],
    ["build/parse round trip", () => assertEq(oauth2.parseBearerHeader(oauth2.buildBearerHeader("a.b-c_d~e+f/==")), "a.b-c_d~e+f/==", "round trip")],
    ["wrong scheme parses to null", () => assertEq(oauth2.parseBearerHeader("Basic abc"), null, "Basic rejected")],
    ["missing token parses to null", () => assertEq(oauth2.parseBearerHeader("Bearer"), null, "no token")],
    ["empty header parses to null", () => assertEq(oauth2.parseBearerHeader(""), null, "empty")],
    ["token with a space parses to null (b64token has no SP)", () => assertEq(oauth2.parseBearerHeader("Bearer a b"), null, "space token")],
    ["isValidBearerToken accepts b64token chars + trailing =", () => assertEq(oauth2.isValidBearerToken("a.b_c-d~e+f/=="), true, "valid b64token")],
    ["isValidBearerToken accepts plain words", () => assertEq(oauth2.isValidBearerToken("abc123"), true, "plain token")],
    ["isValidBearerToken rejects interior '='", () => assertEq(oauth2.isValidBearerToken("a=b"), false, "interior equals")],
    ["isValidBearerToken rejects spaces", () => assertEq(oauth2.isValidBearerToken("bad token"), false, "space")],
    ["isValidBearerToken rejects empty", () => assertEq(oauth2.isValidBearerToken(""), false, "empty")],
], row => row[1]());

// --------------------------------------------------------------
// T8: buildWWWAuthenticate (slice: realm, error, errorDescription,
// errorUri, scope in that order; error is REQUIRED per RFC 6750;
// error_description/error_uri snake aliases accepted)
// --------------------------------------------------------------
runTable("wwwAuthenticate", [
    ["error only", () => assertEq(oauth2.buildWWWAuthenticate({ error: "invalid_token" }), 'Bearer error="invalid_token"', "error only")],
    ["realm comes first", () => assertEq(oauth2.buildWWWAuthenticate({ realm: "rs", error: "invalid_token" }), 'Bearer realm="rs", error="invalid_token"', "realm+error")],
    ["error_description follows error", () => assertEq(oauth2.buildWWWAuthenticate({ error: "invalid_token", errorDescription: "the token expired" }), 'Bearer error="invalid_token", error_description="the token expired"', "camel description")],
    ["snake error_description alias gives the same output", () => assertEq(oauth2.buildWWWAuthenticate({ error: "invalid_token", error_description: "the token expired" }), 'Bearer error="invalid_token", error_description="the token expired"', "snake description")],
    ["error_uri snake alias", () => assertEq(oauth2.buildWWWAuthenticate({ error: "invalid_token", error_uri: "https://as.example/docs" }), 'Bearer error="invalid_token", error_uri="https://as.example/docs"', "snake uri")],
    ["errorUri camel alias", () => assertEq(oauth2.buildWWWAuthenticate({ error: "invalid_token", errorUri: "https://as.example/docs" }), 'Bearer error="invalid_token", error_uri="https://as.example/docs"', "camel uri")],
    ["scope is one quoted space-delimited string", () => assertEq(oauth2.buildWWWAuthenticate({ error: "insufficient_scope", scope: "read write" }), 'Bearer error="insufficient_scope", scope="read write"', "scope")],
    ["full header in documented order", () => assertEq(
        oauth2.buildWWWAuthenticate({ realm: "r", error: "invalid_token", errorDescription: "d", errorUri: "u", scope: "a b" }),
        'Bearer realm="r", error="invalid_token", error_description="d", error_uri="u", scope="a b"',
        "full order")],
    // refusal row: error is REQUIRED (RFC 6750, and the slice says so)
    ["missing error is refused", () => assertThrows(() => oauth2.buildWWWAuthenticate({ realm: "r" }), "error is required")],
], row => row[1]());

// --------------------------------------------------------------
// T9: isValidRedirectUri (slice: verbatim exact except loopback
// port-ignore per RFC 8252)
// --------------------------------------------------------------
runTable("redirectUri", [
    ["exact match", () => assertEq(oauth2.isValidRedirectUri("https://rp.example/cb", ["https://rp.example/cb"]), true, "exact")],
    ["different path", () => assertEq(oauth2.isValidRedirectUri("https://rp.example/other", ["https://rp.example/cb"]), false, "different path")],
    ["no registered URIs", () => assertEq(oauth2.isValidRedirectUri("https://rp.example/cb", []), false, "empty list")],
    ["loopback exact match", () => assertEq(oauth2.isValidRedirectUri("http://127.0.0.1:8300/cb", ["http://127.0.0.1:8300/cb"]), true, "loopback exact")],
    ["loopback port is ignored (RFC 8252)", () => assertEq(oauth2.isValidRedirectUri("http://127.0.0.1:9999/cb", ["http://127.0.0.1:8300/cb"]), true, "loopback port-ignore")],
    ["IPv6 loopback port is ignored", () => assertEq(oauth2.isValidRedirectUri("http://[::1]:9999/cb", ["http://[::1]:8300/cb"]), true, "ipv6 loopback port-ignore")],
    ["loopback path must still match", () => assertEq(oauth2.isValidRedirectUri("http://127.0.0.1:9999/other", ["http://127.0.0.1:8300/cb"]), false, "loopback path differs")],
    ["non-loopback port change is NOT ignored", () => assertEq(oauth2.isValidRedirectUri("https://rp.example:9999/cb", ["https://rp.example:8300/cb"]), false, "remote port differs")],
    // TEST-FIX (DOC-TENSION resolved): the docs say "loopback port-ignore per RFC 8252"
    // without restricting it to IP literals, and the engine's established contract
    // (tests/test_oauth2.js:348) treats "localhost" as a loopback host for the port-ignore.
    // A non-loopback host still matches exactly (row below).
    ["localhost is a loopback host for the port-ignore", () => assertEq(oauth2.isValidRedirectUri("http://localhost:9999/cb", ["http://localhost:8300/cb"]), true, "localhost port ignored")],
], row => row[1]());

// --------------------------------------------------------------
// T10: buildAuthorizationUrl (slice: ?response_type=code&client_id&
// redirect_uri&scope&state&code_challenge; auto-generates state if
// absent, always returned).  The slice does not pin parameter ORDER,
// so value rows are asserted order-independently via URLSearchParams.
// --------------------------------------------------------------
const EP = "https://as.example.com/authorize";
function paramsOf(url) { return new URLSearchParams(url.slice(url.indexOf("?") + 1)); }
runTable("authorizeUrl", [
    ["minimal build with a fixed state", () => {
        const r = oauth2.buildAuthorizationUrl({ authorizationEndpoint: EP, clientId: "cid", redirectUri: "https://rp.example/cb", state: "st123" });
        assertEq(r.state, "st123", "fixed state returned");
        assert(r.url.indexOf(EP + "?") === 0, "endpoint + query, got " + r.url);
        const q = paramsOf(r.url);
        assertEq(q.get("response_type"), "code", "response_type");
        assertEq(q.get("client_id"), "cid", "client_id");
        assertEq(q.get("redirect_uri"), "https://rp.example/cb", "redirect_uri (decoded back)");
        assertEq(q.get("state"), "st123", "state param");
    }],
    ["explicit responseType 'code' equals the default", () => {
        const a = paramsOf(oauth2.buildAuthorizationUrl({ authorizationEndpoint: EP, clientId: "cid", redirectUri: "https://rp.example/cb", state: "s", responseType: "code" }).url);
        const b = paramsOf(oauth2.buildAuthorizationUrl({ authorizationEndpoint: EP, clientId: "cid", redirectUri: "https://rp.example/cb", state: "s" }).url);
        assertEq(a.get("response_type"), "code", "explicit response_type");
        assertEq(a.toString(), b.toString(), "explicit === default");
    }],
    ["scope and PKCE params carried through", () => {
        const q = paramsOf(oauth2.buildAuthorizationUrl({
            authorizationEndpoint: EP, clientId: "cid", redirectUri: "https://rp.example/cb",
            scope: "read write", state: "s1", codeChallenge: RFC7636_CHALLENGE, codeChallengeMethod: "S256",
        }).url);
        assertEq(q.get("scope"), "read write", "scope value");
        assertEq(q.get("code_challenge"), RFC7636_CHALLENGE, "code_challenge value");
        assertEq(q.get("code_challenge_method"), "S256", "code_challenge_method value");
    }],
    ["extraParams carried through", () => {
        const q = paramsOf(oauth2.buildAuthorizationUrl({
            authorizationEndpoint: EP, clientId: "cid", redirectUri: "https://rp.example/cb", state: "s",
            extraParams: { prompt: "login", ui_locales: "fr" },
        }).url);
        assertEq(q.get("prompt"), "login", "extra prompt");
        assertEq(q.get("ui_locales"), "fr", "extra ui_locales");
    }],
    ["absent state is auto-generated and always returned", () => {
        const r = oauth2.buildAuthorizationUrl({ authorizationEndpoint: EP, clientId: "cid", redirectUri: "https://rp.example/cb" });
        assertEq(typeof r.state, "string", "state is a string");
        assertEq(r.state.length, 43, "auto state is the default 32-byte base64url");
        assert(B64URL.test(r.state), "auto state charset");
        assertEq(paramsOf(r.url).get("state"), r.state, "url carries the returned state");
    }],
    // refusal rows
    ["plain method without allowPlain is refused", () => assertThrows(() => oauth2.buildAuthorizationUrl({ authorizationEndpoint: EP, clientId: "cid", redirectUri: "https://rp.example/cb", codeChallenge: "cc", codeChallengeMethod: "plain" }), "plain needs opt-in")],
    ["unknown challenge method is refused", () => assertThrows(() => oauth2.buildAuthorizationUrl({ authorizationEndpoint: EP, clientId: "cid", redirectUri: "https://rp.example/cb", codeChallenge: "cc", codeChallengeMethod: "S512" }), "unknown method")],
    ["plain method WITH allowPlain builds a plain challenge", () => {
        // TEST-FIX: a code_challenge must itself be 43..128 unreserved chars (RFC 7636 4.6) and
        // the builder validates it ("cc" is refused); use a well-formed plain challenge.
        const pc = "p".repeat(43);
        const q = paramsOf(oauth2.buildAuthorizationUrl({ authorizationEndpoint: EP, clientId: "cid", redirectUri: "https://rp.example/cb", codeChallenge: pc, codeChallengeMethod: "plain", allowPlain: true }).url);
        assertEq(q.get("code_challenge_method"), "plain", "plain method present");
        assertEq(q.get("code_challenge"), pc, "plain challenge value");
    }],
    // DOC-TENSION: allowInsecure is documented only by name; the natural
    // reading (an http:// authorization endpoint needs the opt-in) is pinned here.
    ["http endpoint without allowInsecure is refused", () => assertThrows(() => oauth2.buildAuthorizationUrl({ authorizationEndpoint: "http://as.example.com/authorize", clientId: "cid", redirectUri: "https://rp.example/cb" }), "insecure endpoint")],
    ["http endpoint with allowInsecure builds", () => {
        const r = oauth2.buildAuthorizationUrl({ authorizationEndpoint: "http://as.example.com/authorize", clientId: "cid", redirectUri: "https://rp.example/cb", allowInsecure: true });
        assert(r.url.indexOf("http://as.example.com/authorize?") === 0, "insecure build ok, got " + r.url);
    }],
], row => row[1]());

// --------------------------------------------------------------
// T11: parseAuthorizationResponse (slice: ?code/state/error from the
// redirect URL query; refuses implicit-flow fragments, code+error,
// duplicate params)
// --------------------------------------------------------------
runTable("parseAuthorizationResponse", [
    ["code + state", () => {
        const r = oauth2.parseAuthorizationResponse("https://rp.example/cb?code=c1&state=s1");
        assertEq(r.code, "c1", "code");
        assertEq(r.state, "s1", "state");
    }],
    ["state only", () => {
        const r = oauth2.parseAuthorizationResponse("https://rp.example/cb?state=s1");
        assertEq(r.state, "s1", "state");
        assertEq(r.code, undefined, "no code");
        assertEq(r.error, undefined, "no error");
    }],
    ["error + error_description + error_uri", () => {
        const r = oauth2.parseAuthorizationResponse("https://rp.example/cb?error=access_denied&error_description=Oops+here&error_uri=https%3A%2F%2Fas.example%2Fdocs");
        assertEq(r.error, "access_denied", "error");
        assertEq(r.errorDescription, "Oops here", "errorDescription");
        assertEq(r.errorUri, "https://as.example/docs", "errorUri");
    }],
    ["empty query gives an empty result", () => {
        const r = oauth2.parseAuthorizationResponse("https://rp.example/cb");
        assertEq(r.code, undefined, "no code");
        assertEq(r.state, undefined, "no state");
        assertEq(r.error, undefined, "no error");
    }],
    // refusal rows
    ["code together with error is refused", () => assertThrows(() => oauth2.parseAuthorizationResponse("https://rp.example/cb?code=c1&error=access_denied"), "code+error")],
    ["duplicate params are refused", () => assertThrows(() => oauth2.parseAuthorizationResponse("https://rp.example/cb?code=a&code=b"), "duplicate code")],
    ["duplicate state is refused too", () => assertThrows(() => oauth2.parseAuthorizationResponse("https://rp.example/cb?code=a&state=s&state=t"), "duplicate state")],
    ["implicit-flow fragment is refused", () => assertThrows(() => oauth2.parseAuthorizationResponse("https://rp.example/cb#access_token=abc"), "fragment response")],
], row => row[1]());

// --------------------------------------------------------------
// T12: buildTokenRequestBody (slice: application/x-www-form-urlencoded;
// every value must be a string)
// --------------------------------------------------------------
runTable("tokenRequestBody", [
    ["simple body in key order", () => assertEq(oauth2.buildTokenRequestBody({ grant_type: "authorization_code", code: "x1" }), "grant_type=authorization_code&code=x1", "basic body")],
    ["redirect_uri is percent-encoded", () => assertEq(oauth2.buildTokenRequestBody({ code: "x1", redirect_uri: "https://rp/cb" }), "code=x1&redirect_uri=https%3A%2F%2Frp%2Fcb", "encoded redirect_uri")],
    ["space as + and reserved bytes percent-encoded", () => assertEq(oauth2.buildTokenRequestBody({ code: "a b/c&d" }), "code=a+b%2Fc%26d", "encoding")],
    // refusal row: slice pins "every value must be a string"
    ["non-string value is refused", () => assertThrows(() => oauth2.buildTokenRequestBody({ code: 42 }), "number value")],
], row => row[1]());

// --------------------------------------------------------------
// T13: parseTokenResponse (slice per RFC 6749: access_token +
// token_type Bearer required, b64token access_token)
// --------------------------------------------------------------
runTable("tokenResponse", [
    ["minimal Bearer response", () => {
        const r = oauth2.parseTokenResponse('{"access_token":"tok","token_type":"Bearer"}');
        assertEq(r.access_token, "tok", "access_token");
        assertEq(r.token_type, "Bearer", "token_type");
    }],
    ["optional members and extra keys are preserved", () => {
        const r = oauth2.parseTokenResponse('{"access_token":"tok","token_type":"Bearer","expires_in":3600,"refresh_token":"r1","scope":"read write","extra":"v"}');
        assertEq(r.expires_in, 3600, "expires_in");
        assertEq(r.refresh_token, "r1", "refresh_token");
        assertEq(r.scope, "read write", "scope");
        assertEq(r.extra, "v", "unknown key preserved");
    }],
    ["missing access_token is refused", () => assertThrows(() => oauth2.parseTokenResponse('{"token_type":"Bearer"}'), "no access_token")],
    ["missing token_type is refused", () => assertThrows(() => oauth2.parseTokenResponse('{"access_token":"tok"}'), "no token_type")],
    ["non-Bearer token_type is refused", () => assertThrows(() => oauth2.parseTokenResponse('{"access_token":"tok","token_type":"MAC"}'), "MAC token_type")],
    ["access_token must be a b64token", () => assertThrows(() => oauth2.parseTokenResponse('{"access_token":"bad token","token_type":"Bearer"}'), "space in access_token")],
    ["invalid JSON is refused", () => assertThrows(() => oauth2.parseTokenResponse("{not json"), "invalid body")],
], row => row[1]());

// --------------------------------------------------------------
// T14: verifyJWT -- structural refusal row ONLY.  The slice says it
// needs CONFIG_TLS=y (else it throws), then verifies the signature
// via dyna:crypto: a garbage token throws under either condition, so
// the only safe black-box row is "throws".  No network, no signed
// fixture is attempted.
// --------------------------------------------------------------
runTable("verifyJWT", [
    ["a garbage token throws (no-TLS throw or signature failure)", () => assertThrows(() => oauth2.verifyJWT("not.a.jwt", "topsecret", { algorithms: ["HS256"] }), "garbage token")],
], row => row[1]());

// Regression (fail-open exp/nbf): exp/nbf are checked against wall-clock -- a
// non-finite value (JSON "1e999" parses to Infinity) must be refused, not
// silently pass both comparisons. Tokens are signed here with HS256 exactly
// the way the d.ts documents verification ("checks signature via dyna:crypto").
{
    const { HMAC } = await import("dyna:crypto");
    const { Base64URLEncode } = await import("dyna:encoding");
    const { fromUtf8 } = await import("dyna:bytes");
    const enc = (s) => Base64URLEncode(fromUtf8(s));
    const mk = (payload) => {
        const h = enc('{"alg":"HS256","typ":"JWT"}');
        const p = enc(payload);
        return h + "." + p + "." + Base64URLEncode(HMAC("sha256", "topsecret", fromUtf8(h + "." + p)));
    };
    runTable("verifyJWT signed fixtures", [
        ["a properly signed unexpired token verifies", () => assertDeepEq(oauth2.verifyJWT(mk('{"sub":"bb","exp":4102444800}'), "topsecret", { algorithms: ["HS256"] }), { sub: "bb", exp: 4102444800 })],
        ["exp: 1e999 (Infinity) is refused", () => assertThrows(() => oauth2.verifyJWT(mk('{"sub":"bb","exp":1e999}'), "topsecret", { algorithms: ["HS256"] }), "non-finite exp refused", TypeError, /exp/)],
        ["nbf: 1e999 (Infinity) is refused", () => assertThrows(() => oauth2.verifyJWT(mk('{"sub":"bb","nbf":1e999,"exp":4102444800}'), "topsecret", { algorithms: ["HS256"] }), "non-finite nbf refused", TypeError, /nbf/)],
    ], row => row[1]());
}

// --------------------------------------------------------------
// T15: parseBearerFromRequest (slice: "headers.authorization/query/
// body; allowQuery/allowBody opt-in (default false)"; RFC 6750 ships
// the token as "access_token" in the query string or urlencoded body).
// Missing -> null; header names are case-insensitive per HTTP (RFC
// 9110 7.6.1), which is what a mock request echoes.
// --------------------------------------------------------------
runTable("parseBearerFromRequest", [
    ["header default extraction (lowercase key)", () => {
        assertEq(oauth2.parseBearerFromRequest({ headers: { authorization: "Bearer t1" } }), "t1", "from headers.authorization");
    }],
    ["header lookup is case-insensitive", () => {
        assertEq(oauth2.parseBearerFromRequest({ headers: { Authorization: "Bearer t2" } }), "t2", "from headers.Authorization");
    }],
    ["query opt-in (allowQuery: true)", () => {
        assertEq(oauth2.parseBearerFromRequest({ query: "access_token=t3", allowQuery: true }), "t3", "from query when allowed");
    }],
    ["body opt-in (allowBody: true)", () => {
        assertEq(oauth2.parseBearerFromRequest({ body: "access_token=t4", allowBody: true }), "t4", "from body when allowed");
    }],
    ["query is refused by default (opt-in flag absent)", () => {
        assertEq(oauth2.parseBearerFromRequest({ query: "access_token=t5" }), null, "allowQuery defaults to false");
    }],
    ["body is refused by default (opt-in flag absent)", () => {
        assertEq(oauth2.parseBearerFromRequest({ body: "access_token=t6" }), null, "allowBody defaults to false");
    }],
    ["explicit false also refuses", () => {
        assertEq(oauth2.parseBearerFromRequest({ query: "access_token=t7", allowQuery: false }), null, "allowQuery: false");
        assertEq(oauth2.parseBearerFromRequest({ body: "access_token=t7", allowBody: false }), null, "allowBody: false");
    }],
    ["missing everything is null", () => {
        assertEq(oauth2.parseBearerFromRequest({}), null, "empty request");
    }],
    ["headers without an authorization member is null", () => {
        assertEq(oauth2.parseBearerFromRequest({ headers: { "content-type": "text/plain" } }), null, "no authorization header");
    }],
    ["allowed query without an access_token is null", () => {
        assertEq(oauth2.parseBearerFromRequest({ query: "other=1", allowQuery: true }), null, "no access_token in query");
    }],
    ["a non-Bearer authorization header is not a bearer token", () => {
        assertEq(oauth2.parseBearerFromRequest({ headers: { authorization: "Basic dGlkOnNlYw==" } }), null, "Basic scheme is not Bearer");
    }],
], row => row[1]());

print("bb_oauth2: all tests passed (" + n + " assertions)");
