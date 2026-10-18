// flags: --std
// timeout: 300
// Split from tests/test_audit_w2_native_1.js: the encoding sections.
import * as enc from "dyna:encoding";
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
// ---- borrow pin ------------------------------------------------------------
{
    const ab = new ArrayBuffer(1 << 16, { maxByteLength: 1 << 17 });
    const u = new Uint8Array(ab);
    u.fill(0xE3);
    const seen = [];
    const r = enc.detectEncoding(u, {
        get fallback() {
            seen.push(errName(() => ab.transfer()));
            seen.push(errName(() => ab.resize(0)));
            seen.push(errName(() => ab.transferToFixedLength()));
            return "latin1";
        },
    });
    eq(seen.join(","), "TypeError,TypeError,TypeError", "a borrowed buffer refuses transfer/resize/transferToFixedLength");
    eq(ab.byteLength, 1 << 16, "the borrowed buffer kept its length");
    eq(typeof r, "string", "detectEncoding completed on the intact buffer");
    eq(errName(() => ab.resize(8)), "none", "the pin ends when the native call returns");
    eq(ab.byteLength, 8, "resize works again afterwards");
    const moved = ab.transfer();
    eq(ab.byteLength, 0, "transfer works again afterwards");
    eq(moved.byteLength, 8, "transfer moved the bytes");
}
{
    // A buffer borrowed by an INNER native call may be detached by the outer
    // script once that inner call has returned, even while an unrelated
    // native call (Array.prototype.forEach) is still on the stack.
    const ab = new ArrayBuffer(64);
    let name = "?";
    [1].forEach(() => {
        enc.detectEncoding(new Uint8Array(ab));
        name = errName(() => ab.transfer());
    });
    eq(name, "none", "a finished inner borrow does not outlive its call");
}
if (failures) {
    print("test_encoding_audit_w2_native_1: " + failures + " of " + checks + " FAILED");
    throw new Error("test_encoding_audit_w2_native_1 failed");
}
print("test_encoding_audit_w2_native_1: " + checks + " checks passed");
