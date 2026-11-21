// timeout: 300
// covers: dyna:hash.Hasher
// Split from tests/test_audit_w2_native_2.js: the crypto sections.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_hash_hasher_audit_w2_native_2");
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

__kitS.section("SHAKE128 digest() honours a 300-byte length (control)", () => {
    const mk = () => { const h = new Hasher("shake128", { length: 300 }); h.update("abc"); return h; };
    const raw = mk().digest();
    let hex = "";
    for (const b of raw) hex += (b < 16 ? "0" : "") + b.toString(16);
    eq(raw.length, 300, "SHAKE128 digest() honours a 300-byte length (control)");
    eq(mk().digestHex(), hex, "digestHex() returns the same 300 bytes as digest()");
});
__kitS.end();
if (failures) {
    print("test_crypto_audit_w2_native_2: " + failures + " of " + checks + " FAILED");
    throw new Error("test_crypto_audit_w2_native_2 failed");
}
print("test_crypto_audit_w2_native_2: " + checks + " checks passed");
