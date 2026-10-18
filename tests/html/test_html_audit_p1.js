// flags: --std
// Split from tests/test_audit_p1.js: the html sections.
import { Sanitizer } from "dyna:html";

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
    const s = new Sanitizer({ allow: { a: ["href"] } });
    const out = s.clean('<a href="javascript:alert(1)">x</a>');
    assert(!out.includes("javascript:"), "sanitizer fail-open: javascript: href dropped when allow has no protocols rule");
}
if (fails) { print("test_html_audit_p1: " + fails + " FAILED of " + n + " assertions"); throw new Error("test_html_audit_p1 failed"); }
print("test_html_audit_p1: " + n + " assertions, 0 failures");
