// covers: dyna:url.formEncode
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_url_form_encode");
import { URL, URLSearchParams, formEncode, formDecode, encodeURIComponentStrict } from "dyna:url";

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

__kitS.section("formEncode simple", () => {
eq(formEncode({ a: "1", b: "2" }), "a=1&b=2", "formEncode simple");
});

__kitS.section("hello world", () => {
eq(formEncode({ a: "hello world" }), "a=hello+world", "space encodes to +");
});

__kitS.section("delimiters are escaped", () => {
eq(formEncode({ a: "x&y=z" }), "a=x%26y%3Dz", "delimiters are escaped");
});

__kitS.section("non-ASCII encodes as UTF-8", () => {
eq(formEncode({ a: "\u4F60" }), "a=%E4%BD%A0", "non-ASCII encodes as UTF-8");
});

__kitS.section("an empty object encodes to an empty string", () => {
eq(formEncode({}), "", "an empty object encodes to an empty string");
});

__kitS.section("undefined values are omitted", () => {
eq(formEncode({ a: undefined, b: "1" }), "b=1", "undefined values are omitted");
});

__kitS.section("non-string values are coerced", () => {
eq(formEncode({ a: 1, b: true }), "a=1&b=true", "non-string values are coerced");
});

__kitS.section("not an object", () => {
throws(() => formEncode("not an object"), "formEncode refuses a non-object");
});

__kitS.section("formEncode keeps its strict delimiters", () => {
eq(formEncode({ a: "x&y=z" }), "a=x%26y%3Dz", "formEncode keeps its strict delimiters");
});
__kitS.end();

if (fails) {
    print("test_url: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_url failed");
}
print("test_url: " + n + " assertions, 0 failures");
