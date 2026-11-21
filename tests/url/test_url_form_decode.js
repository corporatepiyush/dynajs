// covers: dyna:url.formDecode
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_url_form_decode");
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

eq(formDecode("a=%zz").a, "%zz", "a malformed escape stays literal");

eq(formDecode("a=%4").a, "%4", "a truncated escape stays literal");

eq(formDecode("a=%E4%BD%A0").a, "\u4F60", "UTF-8 percent escapes decode");

__kitS.section("formDecode writes __proto__ as an OWN property", () => {
    const o = formDecode("__proto__=polluted&x=1");
    assert(Object.prototype.hasOwnProperty.call(o, "__proto__"),
           "formDecode writes __proto__ as an OWN property");
    eq(({}).polluted, undefined, "formDecode polluted nothing");
    eq(o.x, "1", "the rest of the query still decoded");
});

__kitS.section("constructor/prototype keys pollute nothing", () => {
    const o = formDecode("constructor=x&prototype=y");
    eq(({}).prototype, undefined, "constructor/prototype keys pollute nothing");
    eq(o.prototype, "y", "and they still land as own properties");
});
__kitS.end();

for (const v of ["", " ", "a b", "a%b", "\u4F60\u597D",
                 "\u{1f600}", "!'()~*", "\n\t"]) {
    eq(formDecode(formEncode({ k: v })).k, v,
       "form round trip: " + JSON.stringify(v));
}

if (fails) {
    print("test_url: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_url failed");
}
print("test_url: " + n + " assertions, 0 failures");
