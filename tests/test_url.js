/* test_url.js -- URL and the form-urlencoded codec in dyna:url (design 01).
 *
 * Relative resolution is checked against RFC 3986 section 5.4's OWN table of
 * normal and abnormal examples, which is the reference implementations are
 * expected to agree with. A round trip through our own parser would agree with
 * its own bugs.
 *
 * Run: dynajs (built with CONFIG_NATIVE_MODULES=y) tests/test_url.js
 */
import { URL, formEncode, formDecode, encodeURIComponentStrict } from "dyna:url";

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

/* -------------------------------------------------------- components */

{
    const u = new URL("https://user:pw@example.com:8443/a/b?x=1&y=2#frag");
    eq(u.protocol, "https:", "protocol keeps its colon");
    eq(u.username, "user", "username");
    eq(u.password, "pw", "password");
    eq(u.hostname, "example.com", "hostname excludes the port");
    eq(u.port, "8443", "port");
    eq(u.host, "example.com:8443", "host includes the port");
    eq(u.pathname, "/a/b", "pathname");
    eq(u.search, "?x=1&y=2", "search keeps its question mark");
    eq(u.hash, "#frag", "hash keeps its octothorpe");
    eq(u.origin, "https://example.com:8443", "origin");
    eq(u.href, "https://user:pw@example.com:8443/a/b?x=1&y=2#frag", "href round-trips");
    eq(String(u), u.href, "toString is href");
    eq(JSON.stringify({ u }), '{"u":"' + u.href + '"}', "toJSON is href");
}

/* A default port is DROPPED, which is what makes two spellings of one origin
 * compare equal. */
eq(new URL("http://a.com:80/").port, "", "http port 80 is dropped");
eq(new URL("https://a.com:443/").port, "", "https port 443 is dropped");
eq(new URL("http://a.com:8080/").port, "8080", "a non-default port is kept");
eq(new URL("http://a.com:80/").href, "http://a.com/", "the dropped port is gone from href");
eq(new URL("http://a.com/").origin, new URL("http://a.com:80/").origin,
   "the two spellings of one origin agree");

/* Scheme is lowercased; host is kept as given. */
eq(new URL("HTTP://a.com/").protocol, "http:", "scheme is lowercased");
eq(new URL("hTtPs://a.com/").protocol, "https:", "mixed-case scheme");

/* Empty components read as empty strings, not undefined. */
{
    const u = new URL("http://a.com/p");
    eq(u.search, "", "no query -> empty search");
    eq(u.hash, "", "no fragment -> empty hash");
    eq(u.username, "", "no userinfo -> empty username");
    eq(u.port, "", "no port -> empty port");
}
/* A present-but-empty query or fragment is distinguishable from absent. */
eq(new URL("http://a.com/?").search, "", "an empty query serialises as empty");
eq(new URL("http://a.com/#").hash, "", "an empty fragment serialises as empty");

/* IPv6 literals keep their brackets and still take a port. */
eq(new URL("http://[::1]/x").hostname, "[::1]", "IPv6 hostname keeps brackets");
eq(new URL("http://[::1]:8080/x").port, "8080", "IPv6 with a port");
eq(new URL("http://[::1]:8080/x").host, "[::1]:8080", "IPv6 host");
eq(new URL("http://[2001:db8::1]/").hostname, "[2001:db8::1]", "full IPv6 literal");

/* A non-special scheme has an opaque path and a null origin. */
{
    const u = new URL("mailto:ada@example.com");
    eq(u.protocol, "mailto:", "mailto protocol");
    eq(u.pathname, "ada@example.com", "mailto path is opaque");
    eq(u.origin, "null", "a scheme with no authority has origin null");
}
eq(new URL("data:text/plain,hi").pathname, "text/plain,hi", "data: opaque path");

/* The query and fragment may contain the delimiters of earlier components. */
{
    const u = new URL("http://a.com/p?q=a?b/c#f#g?h");
    eq(u.search, "?q=a?b/c", "a query may contain ? and /");
    eq(u.hash, "#f#g?h", "a fragment may contain # and ?");
    eq(u.pathname, "/p", "the path stops at the first ?");
}

/* --------------------------- RFC 3986 section 5.4: the reference table */

const BASE = "http://a/b/c/d;p?q";
/* 5.4.1 normal examples */
const NORMAL = [
    ["g:h", "g:h"], ["g", "http://a/b/c/g"], ["./g", "http://a/b/c/g"],
    ["g/", "http://a/b/c/g/"], ["/g", "http://a/g"], ["//g", "http://g"],
    ["?y", "http://a/b/c/d;p?y"], ["g?y", "http://a/b/c/g?y"],
    ["#s", "http://a/b/c/d;p?q#s"], ["g#s", "http://a/b/c/g#s"],
    ["g?y#s", "http://a/b/c/g?y#s"], [";x", "http://a/b/c/;x"],
    ["g;x", "http://a/b/c/g;x"], ["g;x?y#s", "http://a/b/c/g;x?y#s"],
    ["", "http://a/b/c/d;p?q"], [".", "http://a/b/c/"], ["./", "http://a/b/c/"],
    ["..", "http://a/b/"], ["../", "http://a/b/"], ["../g", "http://a/b/g"],
    ["../..", "http://a/"], ["../../", "http://a/"], ["../../g", "http://a/g"],
];
let normalOk = 0;
for (const [ref, want] of NORMAL) {
    let got;
    try { got = new URL(ref, BASE).href; } catch (e) { got = "THREW: " + e.message; }
    if (got === want) normalOk++;
    else assert(false, "RFC 3986 5.4.1 <" + ref + "> -> " + JSON.stringify(got)
                       + " want " + JSON.stringify(want));
}
eq(normalOk, NORMAL.length, "all " + NORMAL.length + " RFC 3986 normal examples");

/* 5.4.2 abnormal examples -- the ones that separate a real resolver from a
 * string concatenation. `..` past the root is dropped, not allowed to escape. */
const ABNORMAL = [
    ["../../../g", "http://a/g"], ["../../../../g", "http://a/g"],
    ["/./g", "http://a/g"], ["/../g", "http://a/g"],
    ["g.", "http://a/b/c/g."], [".g", "http://a/b/c/.g"],
    ["g..", "http://a/b/c/g.."], ["..g", "http://a/b/c/..g"],
    ["./../g", "http://a/b/g"], ["./g/.", "http://a/b/c/g/"],
    ["g/./h", "http://a/b/c/g/h"], ["g/../h", "http://a/b/c/h"],
    ["g;x=1/./y", "http://a/b/c/g;x=1/y"], ["g;x=1/../y", "http://a/b/c/y"],
];
let abnormalOk = 0;
for (const [ref, want] of ABNORMAL) {
    let got;
    try { got = new URL(ref, BASE).href; } catch (e) { got = "THREW: " + e.message; }
    if (got === want) abnormalOk++;
    else assert(false, "RFC 3986 5.4.2 <" + ref + "> -> " + JSON.stringify(got)
                       + " want " + JSON.stringify(want));
}
eq(abnormalOk, ABNORMAL.length, "all " + ABNORMAL.length + " RFC 3986 abnormal examples");

/* PROVE THE TABLE DISCRIMINATES: naive concatenation gets these wrong, so a
 * passing run above is not vacuous. */
assert("http://a/b/c/" + "../../../g" !== "http://a/g",
       "fault injection: concatenation really does differ from resolution");

/* ------------------------------------------------------------- refusals */

throws(() => new URL("not a url"), "a bare string with no scheme and no base");
throws(() => new URL("/relative/only"), "a relative reference with no base");
throws(() => new URL(42), "a non-string input");
throws(() => new URL("http://a.com", "also not a url"), "an invalid base");
throws(() => new URL("http://a.com:99999/"), "a port above 65535");
throws(() => new URL("http://a.com:abc/"), "a non-numeric port");
throws(() => new URL("http://[::1/"), "an unterminated IPv6 literal");
/* An over-long input is refused rather than parsed. */
throws(() => new URL("http://a.com/" + "x".repeat(70000)), "an over-long URL");

/* Leading and trailing control characters are stripped, per spec. */
eq(new URL("  http://a.com/  ").href, "http://a.com/", "surrounding space is stripped");
eq(new URL("\thttp://a.com/\n").href, "http://a.com/", "tabs and newlines are stripped");

/* --------------------------------------------------- form-urlencoded */

eq(formDecode("a=1&b=2").a, "1", "formDecode simple");
eq(formDecode("a=1&b=2").b, "2", "formDecode second key");
eq(formDecode("?a=1").a, "1", "formDecode tolerates a leading ?");
eq(formDecode("a=hello+world").a, "hello world", "+ decodes to a space");
eq(formDecode("a=%20%2B%3D").a, " +=", "percent escapes decode");
eq(formDecode("a").a, "", "a key with no = has an empty value");
eq(formDecode("a=").a, "", "a key with an empty value");
eq(formDecode("").a, undefined, "an empty string decodes to nothing");
eq(formDecode("a=1&a=2").a, "2", "a repeated key takes the last value");
eq(formDecode("a=1&&b=2").b, "2", "an empty pair is skipped");
/* A malformed escape is kept literal, not dropped -- losing bytes silently
 * turns a bad request into a different one. */
eq(formDecode("a=%zz").a, "%zz", "a malformed escape stays literal");
eq(formDecode("a=%4").a, "%4", "a truncated escape stays literal");
/* Non-ASCII survives as UTF-8. */
eq(formDecode("a=%E4%BD%A0").a, "\u4F60", "UTF-8 percent escapes decode");

/* PROTOTYPE POLLUTION at the decoder, which is the qs CVE class. */
{
    const o = formDecode("__proto__=polluted&x=1");
    assert(Object.prototype.hasOwnProperty.call(o, "__proto__"),
           "formDecode writes __proto__ as an OWN property");
    eq(({}).polluted, undefined, "formDecode polluted nothing");
    eq(o.x, "1", "the rest of the query still decoded");
}
{
    const o = formDecode("constructor=x&prototype=y");
    eq(({}).prototype, undefined, "constructor/prototype keys pollute nothing");
    eq(o.prototype, "y", "and they still land as own properties");
}

/* encode */
eq(formEncode({ a: "1", b: "2" }), "a=1&b=2", "formEncode simple");
eq(formEncode({ a: "hello world" }), "a=hello+world", "space encodes to +");
eq(formEncode({ a: "x&y=z" }), "a=x%26y%3Dz", "delimiters are escaped");
eq(formEncode({ a: "\u4F60" }), "a=%E4%BD%A0", "non-ASCII encodes as UTF-8");
eq(formEncode({}), "", "an empty object encodes to an empty string");
eq(formEncode({ a: undefined, b: "1" }), "b=1", "undefined values are omitted");
eq(formEncode({ a: 1, b: true }), "a=1&b=true", "non-string values are coerced");
throws(() => formEncode("not an object"), "formEncode refuses a non-object");

/* Round trip over the awkward characters. */
for (const v of ["", " ", "a b", "a+b", "a%b", "a&b=c", "\u4F60\u597D",
                 "\u{1f600}", "!'()~*", "\n\t"]) {
    eq(formDecode(formEncode({ k: v })).k, v,
       "form round trip: " + JSON.stringify(v));
}

/* encodeURIComponentStrict escapes what encodeURIComponent leaves alone. */
eq(encodeURIComponentStrict("!'()~"), "%21%27%28%29%7E",
   "strict encoding escapes !'()~");
assert(encodeURIComponent("!'()~") === "!'()~",
       "fault injection: the builtin really does leave them alone");
eq(encodeURIComponentStrict("a b"), "a+b", "strict encoding uses + for space");
throws(() => encodeURIComponentStrict(42), "strict encoding refuses a non-string");

if (fails) {
    print("test_url: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_url failed");
}
print("test_url: " + n + " assertions, 0 failures");
