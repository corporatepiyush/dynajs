// timeout: 60
// covers: dyna:oauth2.verifyJWT
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_oauth2_verify_jwt_strict");
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

__kitS.section("a matching issuer verifies", () => {
ok(verifyJWT(tok, key, { algorithms: ["HS256"], iss: "https://as.example" }).sub === "u",
    "a matching issuer verifies");
});

throwsMatch(() => verifyJWT(tok, key, { algorithms: ["HS256"], iss: 42 }),
    /iss/i, "a non-string iss is a TypeError, not silent non-enforcement");

throwsMatch(() => verifyJWT(tok, key, { algorithms: ["HS256"], iss: new String("https://as.example") }),
    /iss/i, "a boxed String iss is refused");

throwsMatch(() => verifyJWT(tok, key, { algorithms: ["HS256"], iss: "https://other.example" }),
    /iss/i, "a mismatched iss still throws");

const noExp = JWTSign({ sub: "u" }, key, { alg: "HS256" });
throwsMatch(() => verifyJWT(noExp, key, { algorithms: ["HS256"], requireExp: true }),
    /exp/i, "requireExp refuses a token without exp");

__kitS.section("without requireExp the old behaviour stays", () => {
ok(!!verifyJWT(noExp, key, { algorithms: ["HS256"] }), "without requireExp the old behaviour stays");
console.log("test_oauth2_strict: " + (n - failed) + " passed, " + failed + " failed");
if (failed)
    throw new Error("test_oauth2_strict: " + failed + " failures");
});
__kitS.end();
