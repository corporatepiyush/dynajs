// covers: dyna:http.CookieSerialize
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_http_cookie_serialize_httpmsg");
import { ContentTypeParse, ContentTypeFormat, Negotiate, NegotiateToken, RangeParse, CookieParse, CookieSerialize, ETagMatch } from "dyna:http";

let n = 0, fails = 0;
function assert(c, msg) { n++; if (!c) { fails++; print("FAIL: " + msg); } }
function eq(a, b, msg) {
    assert(a === b, msg + " (got " + JSON.stringify(a) + ", want " + JSON.stringify(b) + ")");
}
function throws(fn, msg) {
    let t = false;
    try { fn(); } catch (e) { t = true; }
    assert(t, msg);
}

eq(CookieSerialize("a", "1"), "a=1", "serialize bare");

eq(CookieSerialize("a", "1", { path: "/" }), "a=1; Path=/", "with a path");

eq(CookieSerialize("a", "1", { httpOnly: true, secure: true }),
   "a=1; Secure; HttpOnly", "flags");

eq(CookieSerialize("a", "1", { maxAge: 60 }), "a=1; Max-Age=60", "max age");

eq(CookieSerialize("sid", "x", { domain: "e.com", path: "/", sameSite: "Lax",
                                 secure: true, httpOnly: true }),
   "sid=x; Domain=e.com; Path=/; SameSite=Lax; Secure; HttpOnly", "everything");

__kitS.section("cookie bag names key + valid set", () => {
    let m = null;
    try { CookieSerialize("a", "1", { maxAgee: 5 }); } catch (e) { m = e; }
    assert(m !== null && m instanceof TypeError &&
           /unknown option "maxAgee" \(valid: maxAge, domain, path, sameSite, secure, httpOnly\)/.test(m.message),
           "cookie bag names key + valid set (got " + (m && m.message) + ")");
});
__kitS.end();

eq(CookieSerialize("a", "1", { maxAge: undefined, secure: true }), "a=1; Secure",
   "undefined keys are absent; valid bag unchanged");

for (const bad of ["a;b", "a,b", 'a"b', "a\\b", "a b", "a\nb", "a\rb"])
    throws(() => CookieSerialize("k", bad),
           "a value containing " + JSON.stringify(bad.slice(1, 2)) + " is refused");

for (const bad of ["a b", "a;b", "a=b", ""])
    throws(() => CookieSerialize(bad, "v"),
           "a name that is not a token is refused: " + JSON.stringify(bad));

if (fails) {
    print("test_httpmsg: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_httpmsg failed");
}
print("test_httpmsg: " + n + " assertions, 0 failures");
