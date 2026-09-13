/* test_httpmsg.js -- the HTTP message codecs in dyna:net (design 15).
 *
 * These parse bytes that arrive from the network, so the cases that matter are
 * the hostile ones: a comma inside a quoted parameter must not split a list, a
 * cookie named __proto__ must not reach the prototype, and a value carrying a
 * delimiter must be REFUSED rather than escaped into a second header.
 *
 * Run: dynajs (built with CONFIG_NATIVE_MODULES=y) tests/test_httpmsg.js
 */
import { ContentTypeParse, ContentTypeFormat, Negotiate, NegotiateToken,
         RangeParse, CookieParse, CookieSerialize, ETagMatch } from "dyna:net";

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

/* --------------------------------------------------------- Content-Type */

{
    const c = ContentTypeParse("text/html; charset=utf-8");
    eq(c.type, "text", "type");
    eq(c.subtype, "html", "subtype");
    eq(c.parameters.charset, "utf-8", "parameter");
}
/* Type and parameter NAMES are case-insensitive; the parameter VALUE is not. */
{
    const c = ContentTypeParse("TEXT/HTML; CharSet=UTF-8");
    eq(c.type, "text", "type lowercased");
    eq(c.subtype, "html", "subtype lowercased");
    eq(c.parameters.charset, "UTF-8", "parameter name lowercased, value preserved");
}
eq(ContentTypeParse("application/json").parameters.charset, undefined, "no parameters");
eq(ContentTypeParse("  text/plain  ").type, "text", "leading space tolerated");
eq(ContentTypeParse("application/vnd.api+json").subtype, "vnd.api+json",
   "a subtype with + and . is one token");
/* A quoted parameter value is unquoted, and a quoted-pair is unescaped. */
eq(ContentTypeParse('multipart/form-data; boundary="a b"').parameters.boundary, "a b",
   "quoted parameter is unquoted");
eq(ContentTypeParse('x/y; p="a\\"b"').parameters.p, 'a"b', "quoted-pair unescaped");
/* A semicolon INSIDE a quoted value must not end the parameter. */
eq(ContentTypeParse('x/y; p="a;b"; q=2').parameters.p, "a;b",
   "a semicolon inside quotes does not split");
eq(ContentTypeParse('x/y; p="a;b"; q=2').parameters.q, "2",
   "the parameter after a quoted one still parses");
/* Malformed input returns null rather than a plausible wrong object. */
for (const bad of ["", "notype", "/subtype", "type/", "   "])
    eq(ContentTypeParse(bad), null, "malformed returns null: " + JSON.stringify(bad));
throws(() => ContentTypeParse(42), "ContentTypeParse refuses a non-string");

/* Format, and the round trip. */
eq(ContentTypeFormat({ type: "text", subtype: "html" }), "text/html", "format bare");
eq(ContentTypeFormat({ type: "text", subtype: "html", parameters: { charset: "utf-8" } }),
   "text/html; charset=utf-8", "format with a parameter");
/* THE DRIFT RULE: a value the parser would not accept as a token must be
 * quoted by the serializer, or the two disagree and that is a smuggling bug. */
eq(ContentTypeFormat({ type: "m", subtype: "f", parameters: { b: "a b" } }),
   'm/f; b="a b"', "a value with a space is quoted");
eq(ContentTypeFormat({ type: "m", subtype: "f", parameters: { b: 'a"b' } }),
   'm/f; b="a\\"b"', "a quote in a value is escaped");
eq(ContentTypeFormat({ type: "m", subtype: "f", parameters: { b: "" } }),
   'm/f; b=""', "an empty value is quoted");
for (const v of ["plain", "a b", 'a"b', "a;b", "a,b", "", "a\\b"]) {
    const s = ContentTypeFormat({ type: "x", subtype: "y", parameters: { p: v } });
    eq(ContentTypeParse(s).parameters.p, v, "format/parse round trip: " + JSON.stringify(v));
}
throws(() => ContentTypeFormat({ type: "x" }), "format requires a subtype");
throws(() => ContentTypeFormat("x/y"), "format refuses a non-object");

/* ------------------------------------------------------------ negotiation */

const OFFER = ["application/json", "text/html"];
eq(Negotiate("text/html", OFFER), "text/html", "exact match");
eq(Negotiate("application/json", OFFER), "application/json", "exact match 2");
eq(Negotiate("text/*", OFFER), "text/html", "type wildcard");
eq(Negotiate("*/*", OFFER), "application/json", "full wildcard takes the first offer");
eq(Negotiate("image/png", OFFER), null, "no acceptable offer -> null");
eq(Negotiate("", OFFER), "application/json", "an empty header accepts anything");
/* q-values order the result. */
eq(Negotiate("text/html;q=0.1, application/json;q=0.9", OFFER), "application/json",
   "higher q wins");
eq(Negotiate("text/html;q=0.9, application/json;q=0.1", OFFER), "text/html",
   "higher q wins the other way");
/* q=0 is an explicit REFUSAL, not merely a low rank. */
eq(Negotiate("text/html;q=0, application/json", OFFER), "application/json",
   "q=0 refuses that type");
eq(Negotiate("text/html;q=0", ["text/html"]), null, "q=0 with no alternative -> null");
/* Specificity outranks order at equal q: exact beats wildcard. */
eq(Negotiate("*/*, text/html", OFFER), "text/html",
   "an exact range beats a wildcard at the same q");
/* A comma inside a quoted parameter must not split the list. */
eq(Negotiate('text/html;x="a,b"', OFFER), "text/html",
   "a comma inside quotes does not split the header");

/* Token negotiation: language, encoding, charset. */
eq(NegotiateToken("en", ["en", "fr"]), "en", "exact language");
eq(NegotiateToken("fr", ["en", "fr"]), "fr", "second language");
eq(NegotiateToken("*", ["en", "fr"]), "en", "language wildcard");
eq(NegotiateToken("de", ["en", "fr"]), null, "no acceptable language");
/* The prefix rule: `en` matches `en-GB`, which is what makes Accept-Language
 * usable at all. */
eq(NegotiateToken("en", ["en-GB"]), "en-GB", "en matches en-GB by prefix");
eq(NegotiateToken("en", ["end"]), null, "but NOT `end` -- the dash is required");
eq(NegotiateToken("EN", ["en"]), "en", "token matching is case-insensitive");
eq(NegotiateToken("gzip;q=0.5, br;q=1.0", ["gzip", "br"]), "br", "encoding by q");
throws(() => Negotiate("x", "not an array"), "Negotiate requires an array");

/* ----------------------------------------------------------------- Range */

eq(JSON.stringify(RangeParse("bytes=0-499", 1000)), '[{"start":0,"end":499}]',
   "a simple range");
eq(JSON.stringify(RangeParse("bytes=500-", 1000)), '[{"start":500,"end":999}]',
   "an open-ended range runs to the last byte");
eq(JSON.stringify(RangeParse("bytes=-500", 1000)), '[{"start":500,"end":999}]',
   "a suffix range counts from the end");
eq(JSON.stringify(RangeParse("bytes=0-0", 1000)), '[{"start":0,"end":0}]',
   "a one-byte range -- ranges are INCLUSIVE");
eq(RangeParse("bytes=0-499,600-699", 1000).length, 2, "multiple ranges");
/* An end past the resource is clamped, not rejected. */
eq(JSON.stringify(RangeParse("bytes=900-2000", 1000)), '[{"start":900,"end":999}]',
   "an over-long end is clamped");
eq(JSON.stringify(RangeParse("bytes=-2000", 1000)), '[{"start":0,"end":999}]',
   "a suffix longer than the resource is the whole resource");
/* Unsatisfiable is a DIFFERENT answer from absent: one is a 416, the other
 * means there was no Range header to honour. */
eq(RangeParse("bytes=1000-2000", 1000), "unsatisfiable", "start past the end");
eq(RangeParse("bytes=-0", 1000), "unsatisfiable", "a zero-length suffix");
eq(RangeParse("bytes=0-499", 0), "unsatisfiable", "any range on an empty resource");
eq(RangeParse("items=0-499", 1000), null, "a non-bytes unit is ignored -> null");
eq(RangeParse("nonsense", 1000), null, "junk -> null");
eq(RangeParse("bytes=abc-def", 1000), "unsatisfiable", "non-numeric bounds select nothing");
eq(RangeParse("bytes=499-0", 1000), "unsatisfiable", "a reversed range is unsatisfiable");
throws(() => RangeParse("bytes=0-1", -1), "a negative size is refused");

/* ---------------------------------------------------------------- cookies */

eq(CookieParse("a=1; b=2").a, "1", "cookie parse");
eq(CookieParse("a=1; b=2").b, "2", "cookie parse second");
eq(CookieParse("a=1").a, "1", "a single cookie");
eq(CookieParse("").a, undefined, "an empty header");
eq(CookieParse("a=").a, "", "an empty value");
eq(CookieParse('a="quoted"').a, "quoted", "a quoted value is unquoted");
eq(CookieParse("a=1; a=2").a, "2", "a repeated name takes the last");
eq(CookieParse("  a = 1 ; b = 2 ").a, "1", "surrounding space is trimmed");
eq(CookieParse("a=b=c").a, "b=c", "only the FIRST = splits");

/* PROTOTYPE POLLUTION from a network-supplied cookie name. */
{
    const c = CookieParse("__proto__=polluted; x=1");
    assert(Object.prototype.hasOwnProperty.call(c, "__proto__"),
           "a __proto__ cookie is an OWN property");
    eq(({}).polluted, undefined, "a __proto__ cookie polluted nothing");
    eq(c.x, "1", "the rest of the header still parsed");
}

/* Serialize */
eq(CookieSerialize("a", "1"), "a=1", "serialize bare");
eq(CookieSerialize("a", "1", { path: "/" }), "a=1; Path=/", "with a path");
eq(CookieSerialize("a", "1", { httpOnly: true, secure: true }),
   "a=1; Secure; HttpOnly", "flags");
eq(CookieSerialize("a", "1", { maxAge: 60 }), "a=1; Max-Age=60", "max age");
eq(CookieSerialize("sid", "x", { domain: "e.com", path: "/", sameSite: "Lax",
                                 secure: true, httpOnly: true }),
   "sid=x; Domain=e.com; Path=/; SameSite=Lax; Secure; HttpOnly", "everything");

/* HEADER INJECTION: a value carrying a delimiter must be REFUSED, not escaped.
 * Escaping would let a caller append an attribute or a second cookie. */
for (const bad of ["a;b", "a,b", 'a"b', "a\\b", "a b", "a\nb", "a\rb"])
    throws(() => CookieSerialize("k", bad),
           "a value containing " + JSON.stringify(bad.slice(1, 2)) + " is refused");
for (const bad of ["a b", "a;b", "a=b", ""])
    throws(() => CookieSerialize(bad, "v"),
           "a name that is not a token is refused: " + JSON.stringify(bad));
/* And the round trip holds for everything it DOES accept. */
for (const v of ["1", "abc", "a-b_c.d", "%20encoded%21"])
    eq(CookieParse(CookieSerialize("k", v)).k, v, "cookie round trip " + JSON.stringify(v));

/* ------------------------------------------------------------------ ETag */

assert(ETagMatch('"abc"', '"abc"'), "an exact etag matches");
assert(!ETagMatch('"abc"', '"xyz"'), "a different etag does not match");
assert(ETagMatch("*", '"anything"'), "* matches anything");
assert(ETagMatch('"a", "b", "c"', '"b"'), "a list matches any member");
assert(!ETagMatch('"a", "b"', '"c"'), "a list that does not contain it");
/* If-None-Match uses WEAK comparison: the W/ prefix is ignored on both sides. */
assert(ETagMatch('W/"abc"', '"abc"'), "a weak header matches a strong etag");
assert(ETagMatch('"abc"', 'W/"abc"'), "a strong header matches a weak etag");
assert(ETagMatch('W/"abc"', 'W/"abc"'), "weak matches weak");
assert(!ETagMatch("", '"abc"'), "an empty header matches nothing");
assert(ETagMatch('  "a" ,  "b"  ', '"b"'), "spacing in the list is tolerated");
throws(() => ETagMatch('"a"', 42), "ETagMatch refuses a non-string etag");

if (fails) {
    print("test_httpmsg: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_httpmsg failed");
}
print("test_httpmsg: " + n + " assertions, 0 failures");
