// covers: dyna:html.Selector
// Split from tests/test_p2_batch3.js: the html sections.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_html_selector_p2_batch3");
import { Selector, HTMLParse } from "dyna:html";

let n = 0, fails = 0;
function assert(c, m) { n++; if (!c) { fails++; print("FAIL: " + m); } }
function throws(fn, re, m) {
    n++;
    try { fn(); fails++; print("FAIL: " + m + " did not throw"); }
    catch (e) { if (re && !re.test(String(e.message))) { fails++; print("FAIL: " + m + " wrong msg: " + e.message); } }
}
__kitS.section("Selector.first undefined on miss", () => {
    const doc = HTMLParse("<div><p>hi</p></div>");
    assert(new Selector("span").first(doc) === undefined, "Selector.first undefined on miss");
    assert(new Selector("p").first(doc) !== null, "Selector.first non-null on hit");
});
__kitS.end();
if (fails) { print("test_html_p2_batch3: " + fails + " FAILED of " + n + " assertions"); throw new Error("test_html_p2_batch3 failed"); }
print("test_html_p2_batch3: " + n + " assertions, 0 failures");
