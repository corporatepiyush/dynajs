// timeout: 60
// covers: dyna:oauth2.parseBearerFromRequest
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_oauth2_parse_bearer_from_request_strict");
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

__kitS.section("Bearer good", () => {
ok(parseBearerFromRequest({ headers: { authorization: "Bearer good" } }) === "good",
    "the exact Authorization header is read");
});

__kitS.section("Bearer evil", () => {
ok(parseBearerFromRequest({ headers: { authorizationfoo: "Bearer evil" } }) === null,
    "an authorization-prefixed header name is NOT the Authorization header");
});

__kitS.section("Bearer up", () => {
ok(parseBearerFromRequest({ headers: { "AUTHORIZATION": "Bearer up" } }) === "up",
    "case-insensitive exact match still works");
console.log("test_oauth2_strict: " + (n - failed) + " passed, " + failed + " failed");
if (failed)
    throw new Error("test_oauth2_strict: " + failed + " failures");
});
__kitS.end();
