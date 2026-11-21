// covers: dyna:html.HTMLStringify
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_html_html_stringify");
import { HTMLParse, HTMLStringify, Selector, Sanitizer } from "dyna:html";

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
const S = (h) => HTMLStringify(HTMLParse(h));
const j = (v) => JSON.stringify(v);

__kitS.section("Lorem ipsum dolor sit amet, consectetur adipiscing elit, sed.", () => {
    const para = "Lorem ipsum dolor sit amet, consectetur adipiscing elit, sed.";
    eq(HTMLStringify(HTMLParse("<p>" + para + "</p>")), "<p>" + para + "</p>",
       "an escape-free 70-byte text run survives the escaper");
    eq(HTMLStringify({ name: "p", attrs: { t: "a".repeat(70) + "<x" }, children: [] }),
       '<p t="' + "a".repeat(70) + "&lt;x" + '"></p>',
       "and so does an attribute value with its escape past the window");
});
__kitS.end();

if (fails) {
    print("test_html: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_html failed");
}
print("test_html: " + n + " assertions, 0 failures");
