// flags: --std
// timeout: 300
// Split from tests/test_audit_w2_native_2.js: the net sections.
import { HTTPClient } from "dyna:http";
import { TCPProxy } from "dyna:net";

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
// ---- N1-27 / N1-29 ----------------------------------------------------------------
{
    eq(errName(() => new TCPProxy({ port: 0, upstream: { port: 9 }, host: "" })), "RangeError", "TCPProxy refuses an empty host");
    eq(errName(() => new TCPProxy({ port: 0, upstream: { port: 9 }, hots: "x" })), "TypeError", "TCPProxy option bag stays strict");
    const p = new TCPProxy({ port: 0, host: "127.0.0.1", upstream: { host: "127.0.0.1", port: 9 } });
    p.start();
    ok(p.port > 0, "TCPProxy binds the requested host");
    p.close();

    const c = new HTTPClient();
    eq(errName(() => c.get("http://127.0.0.1:9/", "Content-Length : 5")), "TypeError", "a header string whose name is not a token is refused");
    eq(errName(() => c.get("http://127.0.0.1:9/", "no colon here")), "TypeError", "a header string without a colon is refused");
    c.close();
}
if (failures) {
    print("test_net_audit_w2_native_2: " + failures + " of " + checks + " FAILED");
    throw new Error("test_net_audit_w2_native_2 failed");
}
print("test_net_audit_w2_native_2: " + checks + " checks passed");
