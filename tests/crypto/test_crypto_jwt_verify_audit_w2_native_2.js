// timeout: 300
// covers: dyna:crypto.JWTVerify
// Split from tests/test_audit_w2_native_2.js: the crypto sections.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_crypto_jwt_verify_audit_w2_native_2");
import * as crypto from "dyna:crypto";
import { Hasher } from "dyna:hash";
import { cpuNow } from "../lib/cpuclock.js";

let failures = 0, checks = 0;
function ok(cond, msg) {
    checks++;
    if (!cond) {
        failures++;
        print("  FAIL:", msg);
    }
}
function eq(got, want, msg) {
    ok(Object.is(got, want), msg + " (got " + String(got).slice(0, 80) + ", want " + String(want).slice(0, 80) + ")");
}
function errName(fn) {
    try { fn(); return "none"; } catch (e) { return e.name; }
}
function errMsg(fn) {
    try { fn(); return ""; } catch (e) { return String(e.message); }
}
// ---- D3-03 -----------------------------------------------------------------

__kitS.section("an HS256 token keyed with the RSA public PEM is refused", () => {
    const kp = crypto.RSA.generate ? crypto.RSA.generate(2048) : null;
    if (kp) {
        const forged = crypto.JWTSign({ admin: true }, kp.publicKey, { alg: "HS256" });
        eq(errName(() => crypto.JWTVerify(forged, kp.publicKey, { algorithms: ["RS256", "HS256"] })), "TypeError", "an HS256 token keyed with the RSA public PEM is refused");
        const good = crypto.JWTSign({ u: 1 }, kp.privateKey, { alg: "RS256" });
        eq(JSON.stringify(crypto.JWTVerify(good, kp.publicKey, { algorithms: ["RS256", "HS256"] })), '{"u":1}', "a real RS256 token verifies under the mixed allowlist (control)");
    } else {
        print("  note: RSA.generate missing, D3-04 case skipped");
    }
    const t = crypto.JWTSign({ a: 1 }, "secret-key", { alg: "HS256" });
    const parts = t.split(".");
    const sig = parts[2];
    const B64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    const last = B64.indexOf(sig[sig.length - 1]);
    const twin = sig.slice(0, -1) + B64[last ^ 1];
    eq(sig.length % 4, 3, "an HS256 signature leaves two unused bits (precondition of the malleability case)");
    eq(errName(() => crypto.JWTVerify(parts[0] + "." + parts[1] + "." + twin, "secret-key", { algorithms: ["HS256"] })), "TypeError", "a signature with flipped unused bits is refused");
    eq(errName(() => crypto.JWTVerify(t + "=", "secret-key", { algorithms: ["HS256"] })), "TypeError", "a padded signature is refused");
    eq(JSON.stringify(crypto.JWTVerify(t, "secret-key", { algorithms: ["HS256"] })), '{"a":1}', "the canonical token verifies (control)");

    const b64u = (s) => { const bytes = new TextEncoder().encode(s); let bin = ""; for (const b of bytes) bin += String.fromCharCode(b); return btoa(bin).replace(/\+/g, "-").replace(/\//g, "_").replace(/=+$/, ""); };
    const head = b64u(JSON.stringify({ alg: "HS256", typ: "JWT", crit: ["exp2"] }));
    const body = b64u(JSON.stringify({ a: 1 }));
    const mac = crypto.HMAC("sha256", "secret-key", head + "." + body);
    let macBin = ""; for (const b of mac) macBin += String.fromCharCode(b);
    const critTok = head + "." + body + "." + btoa(macBin).replace(/\+/g, "-").replace(/\//g, "_").replace(/=+$/, "");
    ok(/crit/.test(errMsg(() => crypto.JWTVerify(critTok, "secret-key", { algorithms: ["HS256"] }))), "a correctly signed token carrying `crit` is refused for that reason");
});
__kitS.end();
if (failures) {
    print("test_crypto_audit_w2_native_2: " + failures + " of " + checks + " FAILED");
    throw new Error("test_crypto_audit_w2_native_2 failed");
}
print("test_crypto_audit_w2_native_2: " + checks + " checks passed");
