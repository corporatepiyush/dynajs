// timeout: 120
// covers: dyna:crypto.JWTVerify
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_crypto_jwt_verify_bounds");
import { Argon2id, PBKDF2, HKDF, JWTSign, JWTVerify } from "dyna:crypto";
import { QREncode, QRToString, Base64URLEncode } from "dyna:encoding";

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
        console.log("  FAIL " + msg + " (threw " + String(e).slice(0, 80) + ")");
        return;
    }
    failed++;
    console.log("  FAIL " + msg + " (no throw)");
}

__kitS.section("JWTVerify round trip", () => {
    const key = "0123456789abcdef";
    const tok = JWTSign({ sub: "u", exp: Math.floor(Date.now() / 1000) + 600 }, key, { alg: "HS256" });
    const v = JWTVerify(tok, key, { algorithms: ["HS256"] });
    ok(v && v.sub === "u", "JWTVerify round trip");
    const parts = tok.split(".");
    const nulAlg = Base64URLEncode(JSON.stringify({ alg: "HS256\u0000evil", typ: "JWT" }));
    let threw = false;
    try {
        JWTVerify(nulAlg + "." + parts[1] + "." + parts[2], key, { algorithms: ["HS256"] });
    } catch (e) {
        threw = /alg/i.test(String(e));
    }
    ok(threw, "an alg with an embedded NUL is refused, not silently truncated to HS256");
    const longAlg = Base64URLEncode(JSON.stringify({ alg: "a".repeat(40), typ: "JWT" }));
    threw = false;
    try {
        JWTVerify(longAlg + "." + parts[1] + "." + parts[2], key, { algorithms: ["HS256"] });
    } catch (e) {
        threw = true;
    }
    ok(threw, "an over-long alg is refused");
});
__kitS.end();
console.log("test_crypto_bounds: " + (n - failed) + " passed, " + failed + " failed");
if (failed)
    throw new Error("test_crypto_bounds: " + failed + " failures");
