// covers: integration dyna:html.Selector dyna:scrape.Extractor -- Selector.first and the Extractor.run result built on Selector matches, one fixture document
// Split from tests/test_p2_batch2.js: the html sections.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_html_p2_batch2");
import { Selector, HTMLParse } from "dyna:html";
import { Extractor } from "dyna:scrape";

let n = 0, fails = 0;
function assert(c, m) { n++; if (!c) { fails++; print("FAIL: " + m); } }
function eq(a, b, m) { assert(a === b, m + " (got " + JSON.stringify(a) + " want " + JSON.stringify(b) + ")"); }
await __kitS.section("Selector.first returns undefined on no match", async () => {
    const doc = HTMLParse("<div><p>hi</p></div>");
    const sel = new Selector("span");
    const first = sel.first(doc);
    assert(first === undefined, "Selector.first returns undefined on no match (got " + String(first) + ")");
    const sel2 = new Selector("p");
    const first2 = sel2.first(doc);
    assert(first2 !== null && first2 !== undefined, "Selector.first returns element on match");
});
await __kitS.section("<div><p class=\"a\">hello</p></div>", async () => {
    const html = `<div><p class="a">hello</p></div>`;
    const doc = HTMLParse(html);
    const ex = new Extractor({
        title: { sel: new Selector("p"), attr: "text" }
    });
    const res = ex.run(doc);
    assert(typeof res.ok === "boolean", "Extractor.run has ok boolean");
    assert(res.value && typeof res.value === "object", "Extractor.run has value object");
    assert(Array.isArray(res.missing), "Extractor.run has missing array");
    const ex2 = new Extractor({
        req: { sel: new Selector("span"), required: true }
    });
    const res2 = ex2.run(doc);
    eq(res2.ok, false, "Extractor.run ok false when required missing");
    assert(res2.missing.includes("req"), "Extractor.run missing includes req");
});
__kitS.end();
if (fails) { print("test_html_p2_batch2: " + fails + " FAILED of " + n + " assertions"); throw new Error("test_html_p2_batch2 failed"); }
print("test_html_p2_batch2: " + n + " assertions, 0 failures");
