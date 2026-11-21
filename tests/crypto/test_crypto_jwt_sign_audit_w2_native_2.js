// timeout: 300
// covers: dyna:crypto.JWTSign
// Split from tests/test_audit_w2_native_2.js: the crypto sections.
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

{
    const tok = crypto.JWTSign({ a: 1 }, "undefined", { alg: "HS256" });
    for (const [label, bad] of [["undefined", undefined], ["null", null], ["number", 5], ["object", {}], ["boolean", true]]) {
        eq(errName(() => crypto.JWTVerify(tok, bad, { algorithms: ["HS256"] })), "TypeError", "JWTVerify refuses a " + label + " key");
        eq(errName(() => crypto.JWTSign({ a: 1 }, bad, { alg: "HS256" })), "TypeError", "JWTSign refuses a " + label + " key");
        eq(errName(() => crypto.HMACHex("sha256", bad, "x")), "TypeError", "HMACHex refuses a " + label + " key");
        eq(errName(() => new crypto.Hmac("sha256", bad)), "TypeError", "Hmac refuses a " + label + " key");
        eq(errName(() => crypto.TimingSafeEqual(bad, "undefined")), "TypeError", "TimingSafeEqual refuses a " + label);
    }
    eq(JSON.stringify(crypto.JWTVerify(tok, "undefined", { algorithms: ["HS256"] })), '{"a":1}', "a real string key still verifies (control)");
    eq(crypto.HMACHex("sha256", new Uint8Array([1, 2, 3]), "x").length, 64, "a byte-view key still works (control)");
    eq(errName(() => crypto.JWTVerify(crypto.JWTSign({ a: 1 }, "k", { alg: "HS256" }), "", { algorithms: ["HS256"] })), "TypeError", "JWTVerify refuses an empty HMAC key");
    eq(crypto.SHA256Hex({ toString() { return "q"; } }), crypto.SHA256Hex("q"), "DATA arguments keep their string coercion (control)");
}
if (failures) {
    print("test_crypto_audit_w2_native_2: " + failures + " of " + checks + " FAILED");
    throw new Error("test_crypto_audit_w2_native_2 failed");
}
print("test_crypto_audit_w2_native_2: " + checks + " checks passed");
