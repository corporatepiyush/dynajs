// flags: --std
// timeout: 60
import { verifyJWT, parseBearerFromRequest, buildAuthorizationUrl } from "dyna:oauth2";
import { JWTSign } from "dyna:crypto";

let n = 0, failed = 0;
function ok(c, msg) {
    n++;
    if (!c) {
        failed++;
        console.log("  FAIL " + msg);
    }
}
function throwsMatch(fn, re, msg) {
    n++;
    try {
        fn();
    } catch (e) {
        if (re.test(String(e)))
            return;
        failed++;
        console.log("  FAIL " + msg + " (threw " + String(e).slice(0, 70) + ")");
        return;
    }
    failed++;
    console.log("  FAIL " + msg + " (no throw)");
}

const key = "0123456789abcdef";
const exp = Math.floor(Date.now() / 1000) + 600;
const tok = JWTSign({ sub: "u", iss: "https://as.example", exp }, key, { alg: "HS256" });

ok(verifyJWT(tok, key, { algorithms: ["HS256"], iss: "https://as.example" }).sub === "u",
    "a matching issuer verifies");
throwsMatch(() => verifyJWT(tok, key, { algorithms: ["HS256"], iss: 42 }),
    /iss/i, "a non-string iss is a TypeError, not silent non-enforcement");
throwsMatch(() => verifyJWT(tok, key, { algorithms: ["HS256"], iss: new String("https://as.example") }),
    /iss/i, "a boxed String iss is refused");
throwsMatch(() => verifyJWT(tok, key, { algorithms: ["HS256"], iss: "https://other.example" }),
    /iss/i, "a mismatched iss still throws");

const noExp = JWTSign({ sub: "u" }, key, { alg: "HS256" });
throwsMatch(() => verifyJWT(noExp, key, { algorithms: ["HS256"], requireExp: true }),
    /exp/i, "requireExp refuses a token without exp");
ok(!!verifyJWT(noExp, key, { algorithms: ["HS256"] }), "without requireExp the old behaviour stays");

ok(parseBearerFromRequest({ headers: { authorization: "Bearer good" } }) === "good",
    "the exact Authorization header is read");
ok(parseBearerFromRequest({ headers: { authorizationfoo: "Bearer evil" } }) === null,
    "an authorization-prefixed header name is NOT the Authorization header");
ok(parseBearerFromRequest({ headers: { "AUTHORIZATION": "Bearer up" } }) === "up",
    "case-insensitive exact match still works");

const base = { authorizationEndpoint: "https://as.example/auth", clientId: "c", redirectUri: "https://app.example/cb" };
const u = buildAuthorizationUrl(base);
ok(typeof u.state === "string" && u.state.length >= 16, "state is auto-generated");
throwsMatch(() => buildAuthorizationUrl(Object.assign({}, base, { state: 42 })),
    /state/i, "a non-string state is refused instead of dropping CSRF state");
throwsMatch(() => buildAuthorizationUrl(Object.assign({}, base, { codeChallenge: 42 })),
    /codeChallenge/i, "a non-string codeChallenge is refused instead of dropping PKCE");
throwsMatch(() => buildAuthorizationUrl(Object.assign({}, base, { codeChallengeMethod: 7 })),
    /codeChallengeMethod/i, "a non-string codeChallengeMethod is refused");
{
    const v = buildAuthorizationUrl(Object.assign({}, base, {
        state: "s".repeat(20), codeChallenge: "c".repeat(43), codeChallengeMethod: "S256",
    }));
    ok(v.url.indexOf("state=") !== -1 && v.url.indexOf("code_challenge=") !== -1,
        "valid state/PKCE values still reach the URL");
}
console.log("test_oauth2_strict: " + (n - failed) + " passed, " + failed + " failed");
if (failed)
    throw new Error("test_oauth2_strict: " + failed + " failures");
