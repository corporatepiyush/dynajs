// timeout: 60
// covers: dyna:oauth2.buildAuthorizationUrl
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_oauth2_build_authorization_url_strict");
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

const base = { authorizationEndpoint: "https://as.example/auth", clientId: "c", redirectUri: "https://app.example/cb" };
const u = buildAuthorizationUrl(base);
ok(typeof u.state === "string" && u.state.length >= 16, "state is auto-generated");

throwsMatch(() => buildAuthorizationUrl(Object.assign({}, base, { state: 42 })),
    /state/i, "a non-string state is refused instead of dropping CSRF state");

throwsMatch(() => buildAuthorizationUrl(Object.assign({}, base, { codeChallenge: 42 })),
    /codeChallenge/i, "a non-string codeChallenge is refused instead of dropping PKCE");

throwsMatch(() => buildAuthorizationUrl(Object.assign({}, base, { codeChallengeMethod: 7 })),
    /codeChallengeMethod/i, "a non-string codeChallengeMethod is refused");

__kitS.section("valid state/PKCE values still reach the URL", () => {
    const v = buildAuthorizationUrl(Object.assign({}, base, {
        state: "s".repeat(20), codeChallenge: "c".repeat(43), codeChallengeMethod: "S256",
    }));
    ok(v.url.indexOf("state=") !== -1 && v.url.indexOf("code_challenge=") !== -1,
        "valid state/PKCE values still reach the URL");
});
__kitS.end();
console.log("test_oauth2_strict: " + (n - failed) + " passed, " + failed + " failed");
if (failed)
    throw new Error("test_oauth2_strict: " + failed + " failures");
