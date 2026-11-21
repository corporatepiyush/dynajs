// covers: dyna:http.CookieParse
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_http_cookie_parse_httpmsg");
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

eq(CookieParse("a=1; b=2").a, "1", "cookie parse");

eq(CookieParse("a=1; b=2").b, "2", "cookie parse second");

eq(CookieParse("a=1").a, "1", "a single cookie");

eq(CookieParse("").a, undefined, "an empty header");

eq(CookieParse("a=").a, "", "an empty value");

eq(CookieParse('a="quoted"').a, "quoted", "a quoted value is unquoted");

eq(CookieParse("a=1; a=2").a, "2", "a repeated name takes the last");

eq(CookieParse("  a = 1 ; b = 2 ").a, "1", "surrounding space is trimmed");

eq(CookieParse("a=b=c").a, "b=c", "only the FIRST = splits");

__kitS.section("__proto__=polluted; x=1", () => {
    const c = CookieParse("__proto__=polluted; x=1");
    assert(Object.prototype.hasOwnProperty.call(c, "__proto__"),
           "a __proto__ cookie is an OWN property");
    eq(({}).polluted, undefined, "a __proto__ cookie polluted nothing");
    eq(c.x, "1", "the rest of the header still parsed");
});
__kitS.end();

for (const v of ["1", "abc", "a-b_c.d", "%20encoded%21"])
    eq(CookieParse(CookieSerialize("k", v)).k, v, "cookie round trip " + JSON.stringify(v));

if (fails) {
    print("test_httpmsg: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_httpmsg failed");
}
print("test_httpmsg: " + n + " assertions, 0 failures");
