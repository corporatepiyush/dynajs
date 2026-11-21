// timeout: 120
// covers: dyna:html.Selector
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_html_selector_hardening");
import { HTMLParse, HTMLStringify, Selector, MarkdownToHTML } from "dyna:html";
import { cpuNow } from "../lib/cpuclock.js";

let n = 0, failed = 0;
function ok(c, msg) {
    n++;
    if (!c) {
        failed++;
        console.log("  FAIL " + msg);
    }
}

__kitS.section("span div div div div div div", () => {
    const deep = "<div>".repeat(200) + "x" + "</div>".repeat(200);
    const doc = HTMLParse(deep);
    const t0 = cpuNow();
    const r = new Selector("span div div div div div div").first(doc);
    const dt = cpuNow() - t0;
    ok(r === undefined && dt < 2000,
        "a 7-unit descendant selector over a 200-deep tree terminates (dt=" + dt + "ms)");
    const t1 = cpuNow();
    const r2 = new Selector("div div div").first(doc);
    ok(r2 !== undefined && cpuNow() - t1 < 2000, "a short descendant selector still matches");
});

__kitS.section(":first-child counts element children only (the leading text node is skipped)", () => {
    const kids = ["text", { name: "b", attrs: {}, children: [] }, { name: "i", attrs: {}, children: [] }];
    const doc = [{ name: "div", attrs: {}, children: kids }];
    const selB = new Selector("b:first-child");
    const bEl = selB.first(doc);
    ok(bEl !== undefined && bEl.name === "b",
        ":first-child counts element children only (the leading text node is skipped)");
    const selI = new Selector("i:last-child");
    ok(selI.first(doc) !== undefined, ":last-child sees the last element child");
    const upper = new Selector("I:LAST-CHILD");
    ok(upper.first(doc) !== undefined, "pseudo-class names match case-insensitively");
    ok(new Selector("i:first-child").first(doc) === undefined,
        "i:first-child does not match (b is the first element child)");
});
__kitS.end();
console.log("test_html_hardening: " + (n - failed) + " passed, " + failed + " failed");
if (failed)
    throw new Error("test_html_hardening: " + failed + " failures");
