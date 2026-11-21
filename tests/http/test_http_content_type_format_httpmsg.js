// covers: dyna:http.ContentTypeFormat
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_http_content_type_format_httpmsg");
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

__kitS.section("format bare", () => {
eq(ContentTypeFormat({ type: "text", subtype: "html" }), "text/html", "format bare");
});

__kitS.section("text/html; charset=utf-8", () => {
eq(ContentTypeFormat({ type: "text", subtype: "html", parameters: { charset: "utf-8" } }),
   "text/html; charset=utf-8", "format with a parameter");
});

__kitS.section("m/f; b=\"a b\"", () => {
eq(ContentTypeFormat({ type: "m", subtype: "f", parameters: { b: "a b" } }),
   'm/f; b="a b"', "a value with a space is quoted");
});

__kitS.section("m/f; b=\"a\\\"b\"", () => {
eq(ContentTypeFormat({ type: "m", subtype: "f", parameters: { b: 'a"b' } }),
   'm/f; b="a\\"b"', "a quote in a value is escaped");
});

__kitS.section("m/f; b=\"\"", () => {
eq(ContentTypeFormat({ type: "m", subtype: "f", parameters: { b: "" } }),
   'm/f; b=""', "an empty value is quoted");
});

__kitS.section("format/parse round trip:", () => {
for (const v of ["plain", "a b", 'a"b', "a;b", "a,b", "", "a\\b"]) {
    const s = ContentTypeFormat({ type: "x", subtype: "y", parameters: { p: v } });
    eq(ContentTypeParse(s).parameters.p, v, "format/parse round trip: " + JSON.stringify(v));
}
});

__kitS.section("format requires a subtype", () => {
throws(() => ContentTypeFormat({ type: "x" }), "format requires a subtype");
});

__kitS.section("format refuses a non-object", () => {
throws(() => ContentTypeFormat("x/y"), "format refuses a non-object");
});

__kitS.section("plainr X-Evil: 1", () => {
throws(() => ContentTypeFormat({ type: "text", subtype: "plain\r\nX-Evil: 1",
                                 parameters: {} }),
       "a CRLF in the subtype is refused");
});

__kitS.section("xr Set-Cookie: p=1", () => {
throws(() => ContentTypeFormat({ type: "text", subtype: "plain",
                                 parameters: { a: "x\r\nSet-Cookie: p=1" } }),
       "a CRLF in a parameter value is refused");
});

__kitS.section("text/plain; a=\"x y\"", () => {
eq(ContentTypeFormat({ type: "text", subtype: "plain", parameters: { a: "x y" } }),
   'text/plain; a="x y"', "non-token parameter values still quote");
});
__kitS.end();

if (fails) {
    print("test_httpmsg: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_httpmsg failed");
}
print("test_httpmsg: " + n + " assertions, 0 failures");
