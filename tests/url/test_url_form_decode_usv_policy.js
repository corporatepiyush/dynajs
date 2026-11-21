// covers: dyna:url.formDecode
// P15 policy (WHATWG USVString): every URL, URLSearchParams and formDecode input is converted
// with a lone surrogate -> U+FFFD (one %EF%BF%BD), and percent-decoded bytes that are not UTF-8,
// encoded surrogates %ED%A0%80 included, decode to U+FFFD per maximal subpart. domainToASCII
// drops a disallowed code point (lone surrogate, U+FFFD) by default and refuses it under { strict: true } (owner decision D8).
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_url_form_decode_usv_policy");
import { URL, URLSearchParams, formDecode, formEncode, domainToASCII, encodeURIComponentStrict } from "dyna:url";

let n = 0, fails = 0;
function assert(c, msg) { n++; if (!c) { fails++; print("FAIL: " + msg); } }
function same(a, b, msg) { assert(a === b, msg + " (got " + JSON.stringify(a) + ", want " + JSON.stringify(b) + ")"); }
function throwsT(fn, msg) {
    let e = null;
    try { fn(); } catch (x) { e = x; }
    assert(e instanceof TypeError, msg + " (got " + e + ")");
}

__kitS.section("formDecode", () => {
    same(formDecode("a=%ED%A0%80").a, "���", "encoded surrogate value");
    same(Object.keys(formDecode("%ED%B0%80=1"))[0], "���", "encoded surrogate key");
    same(formDecode("a=%FF").a, "�", "invalid byte");
    same(formDecode("a=%F0%9F%98%80").a, "😀", "a real astral char is kept");
    same(formDecode("a=\ud800").a, "�", "lone surrogate in the input");
});

__kitS.section("URL and URLSearchParams", () => {
    same(new URL("http://h/\ud800").pathname, "/%EF%BF%BD", "path");
    same(new URL("http://h/?q=\udc00").search, "?q=%EF%BF%BD", "search");
    same(new URL("http://h/#\ud800").hash, "#%EF%BF%BD", "hash");
    const u = new URL("http://h/");
    u.pathname = "\ud800";
    same(u.href, "http://h/%EF%BF%BD", "pathname setter");
    same(new URLSearchParams({ q: "\ud800" }).toString(), "q=%EF%BF%BD", "params record");
    const p = new URLSearchParams();
    p.append("\udc00", "x\ud800");
    same(p.toString(), "%EF%BF%BD=x%EF%BF%BD", "append");
    same(new URL("http://h/😀").pathname, "/%F0%9F%98%80", "a real pair is kept");
    same(new URLSearchParams([["\ud800", "y\udc00"]]).toString(), "%EF%BF%BD=y%EF%BF%BD", "params from pairs");
    same(new URLSearchParams("q=\ud800&\udc00=1").toString(), "q=%EF%BF%BD&%EF%BF%BD=1", "params from a string");
    same(new URL("a\ud800", "http://h/d\udc00/").pathname, "/d%EF%BF%BD/a%EF%BF%BD", "relative input and base");
    same(formEncode({ "\ud800": "v\udc00" }), "%EF%BF%BD=v%EF%BF%BD", "formEncode key and value");
    same(encodeURIComponentStrict("a\ud800"), "a%EF%BF%BD", "encodeURIComponentStrict encodes U+FFFD (encodeURIComponent throws URIError)");
});

__kitS.section("domainToASCII", () => {
    same(domainToASCII("a\ud800.com"), "a.com", "default: a lone surrogate is dropped");
    same(domainToASCII("a\ufffd.com"), "a.com", "default: U+FFFD is dropped");
    throwsT(() => domainToASCII("a\ud800.com", { strict: true }), "strict: a lone surrogate is refused");
    throwsT(() => domainToASCII("a\ufffd.com", { strict: true }), "strict: U+FFFD is disallowed");
    same(domainToASCII("bücher.de"), "xn--bcher-kva.de", "valid still maps");
});

print("test_url_form_decode_usv_policy: " + n + " assertions, " + fails + " failures");
if (fails) throw new Error(fails + " failures");
