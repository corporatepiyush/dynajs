// covers: dyna:http.ContentTypeParse
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_http_content_type_parse_httpmsg");
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

__kitS.section("text/html; charset=utf-8", () => {
    const c = ContentTypeParse("text/html; charset=utf-8");
    eq(c.type, "text", "type");
    eq(c.subtype, "html", "subtype");
    eq(c.parameters.charset, "utf-8", "parameter");
});

__kitS.section("TEXT/HTML; CharSet=UTF-8", () => {
    const c = ContentTypeParse("TEXT/HTML; CharSet=UTF-8");
    eq(c.type, "text", "type lowercased");
    eq(c.subtype, "html", "subtype lowercased");
    eq(c.parameters.charset, "UTF-8", "parameter name lowercased, value preserved");
});
__kitS.end();

eq(ContentTypeParse("application/json").parameters.charset, undefined, "no parameters");

eq(ContentTypeParse("  text/plain  ").type, "text", "leading space tolerated");

eq(ContentTypeParse("application/vnd.api+json").subtype, "vnd.api+json",
   "a subtype with + and . is one token");

eq(ContentTypeParse('multipart/form-data; boundary="a b"').parameters.boundary, "a b",
   "quoted parameter is unquoted");

eq(ContentTypeParse('x/y; p="a\\"b"').parameters.p, 'a"b', "quoted-pair unescaped");

eq(ContentTypeParse('x/y; p="a;b"; q=2').parameters.p, "a;b",
   "a semicolon inside quotes does not split");

eq(ContentTypeParse('x/y; p="a;b"; q=2').parameters.q, "2",
   "the parameter after a quoted one still parses");

for (const bad of ["", "notype", "/subtype", "type/", "   "])
    eq(ContentTypeParse(bad), null, "malformed returns null: " + JSON.stringify(bad));

throws(() => ContentTypeParse(42), "ContentTypeParse refuses a non-string");

assert(ContentTypeParse("text/plain JUNK") === null,
       "trailing junk after the subtype is not a parameter");

assert(ContentTypeParse("text/plain ; a=1").parameters.a === "1",
       "OWS before the parameter semicolon is tolerated");

assert(ContentTypeParse('text/plain; a="x\r\nSet-Cookie: 1"') === null,
       "a CRLF in a quoted parameter is refused (CWE-113)");

if (fails) {
    print("test_httpmsg: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_httpmsg failed");
}
print("test_httpmsg: " + n + " assertions, 0 failures");
