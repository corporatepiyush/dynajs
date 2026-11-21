// covers: dyna:url.encodeURIComponentStrict
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_url_encode_uri_component_strict");
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

__kitS.section("strict encoding escapes !'()~", () => {
eq(encodeURIComponentStrict("!'()~"), "%21%27%28%29%7E",
   "strict encoding escapes !'()~");
});

__kitS.section("fault injection: the builtin really does leave them alone", () => {
assert(encodeURIComponent("!'()~") === "!'()~",
       "fault injection: the builtin really does leave them alone");
});

__kitS.section("strict encoding uses %20 for space", () => {
eq(encodeURIComponentStrict("a b"), "a%20b", "strict encoding uses %20 for space");
});

__kitS.section("strict encoding refuses a non-string", () => {
throws(() => encodeURIComponentStrict(42), "strict encoding refuses a non-string");
});
__kitS.end();

if (fails) {
    print("test_url: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_url failed");
}
print("test_url: " + n + " assertions, 0 failures");
