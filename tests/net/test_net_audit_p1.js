// flags: --std
// Split from tests/test_audit_p1.js: the net sections.
import { CookieSerialize } from "dyna:net";

let n = 0, fails = 0;
function assert(c, m) { n++; if (!c) { fails++; print("FAIL: " + m); } }
function eq(a, b, m) { n++; if (a !== b) { fails++; print("FAIL: " + m + " got " + JSON.stringify(a) + " want " + JSON.stringify(b)); } }
function throws(fn, re, m) {
    n++;
    try { fn(); fails++; print("FAIL: " + m + " did not throw"); }
    catch (e) {
        if (re && !re.test(String(e.message))) { fails++; print("FAIL: " + m + " wrong message: " + e.message); }
    }
}
{
    throws(() => CookieSerialize("a", "1", { path: "a; Domain=evil\r\nSet-Cookie: x" }), /path/, "cookie path injection rejected");
    throws(() => CookieSerialize("a", "1", { domain: "evil.com\r\nSet-Cookie: x" }), /domain/, "cookie domain crlf rejected");
    throws(() => CookieSerialize("a", "1", { sameSite: "Lax; Path=/evil" }), /sameSite/, "cookie sameSite injection rejected");
    throws(() => CookieSerialize("a", "1", { sameSite: "Bogus" }), /Strict.*Lax.*None/, "cookie sameSite bogus rejected");
    eq(CookieSerialize("a", "1", { path: "/", domain: "example.com", sameSite: "Strict" }),
       "a=1; Domain=example.com; Path=/; SameSite=Strict", "cookie good path still works");
}
if (fails) { print("test_net_audit_p1: " + fails + " FAILED of " + n + " assertions"); throw new Error("test_net_audit_p1 failed"); }
print("test_net_audit_p1: " + n + " assertions, 0 failures");
