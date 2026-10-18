// flags: --std
// timeout: 300
// Split from tests/test_audit_w2_native_1.js: the crypto sections.
import { PBKDF2 } from "dyna:crypto";
import { args } from "dyna:sys";

const BIN = args()[0];
let failures = 0, checks = 0;
function ok(cond, msg) {
    checks++;
    if (!cond) {
        failures++;
        print("  FAIL:", msg);
    }
}
function eq(got, want, msg) {
    ok(Object.is(got, want), msg + " (got " + String(got) + ", want " + String(want) + ")");
}
function errName(fn) {
    try { fn(); return "none"; } catch (e) { return e.name; }
}
async function errNameAsync(fn) {
    try { await fn(); return "none"; } catch (e) { return e.name; }
}
{
    // PBKDF2 reads `iterations`/`length` AFTER it has borrowed `password`
    // (D3-01 shape): the getters must not be able to shrink that buffer.
    const ab = new ArrayBuffer(4096, { maxByteLength: 8192 });
    const pw = new Uint8Array(ab);
    pw.fill(3);
    const salt = new Uint8Array(16);
    const want = Array.from(PBKDF2({ password: pw, salt, iterations: 2, length: 32 })).join();
    const inner = [];
    const opts = { password: pw, salt };
    for (const k of ["iterations", "length"]) {
        Object.defineProperty(opts, k, {
            enumerable: true,
            get() { inner.push(errName(() => ab.resize(1))); return k === "iterations" ? 2 : 32; },
        });
    }
    const got = Array.from(PBKDF2(opts)).join();
    eq(inner.join(","), "TypeError,TypeError", "PBKDF2: option getters cannot shrink the borrowed password");
    eq(got, want, "PBKDF2 derived the key from the intact password");
}
if (failures) {
    print("test_crypto_audit_w2_native_1: " + failures + " of " + checks + " FAILED");
    throw new Error("test_crypto_audit_w2_native_1 failed");
}
print("test_crypto_audit_w2_native_1: " + checks + " checks passed");
