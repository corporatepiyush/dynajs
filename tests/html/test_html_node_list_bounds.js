// html node lists (HTMLStringify/HTMLText input and children, Selector input),
// found by the signature-driven input generator (tests/lib/sigfuzz.js): a list
// whose length no document could have is a RangeError at once instead of four
// billion iterations over holes.
import { HTMLParse, HTMLStringify, HTMLText, Selector } from "dyna:html";
let n = 0, fails = 0;
function ok(c, m) { n++; if (!c) { fails++; print("FAIL: " + m); } }
function eq(a, b, m) { ok(a === b, m + " (got " + JSON.stringify(a) + ", want " + JSON.stringify(b) + ")"); }
function err(f) { try { f(); return "no throw"; } catch (e) { return e.constructor.name + ": " + e.message; } }
async function aerr(f) { try { await f(); return "no throw"; } catch (e) { return e.constructor.name + ": " + e.message; } }
const huge = () => { const a = []; a.length = 2 ** 32 - 1; return a; };
function fast(label, f, re) {
    const t0 = performance.now();
    const m = err(f);
    const dt = performance.now() - t0;
    ok(re.test(m), label + " (" + m + ")");
    ok(dt < 500, label + ": refused before any work (" + dt.toFixed(1) + " ms)");
}
const RE = /^RangeError: html: a node list of 4294967295 entries is larger than any document$/;
fast("HTMLStringify: a sparse node list", () => HTMLStringify(huge()), RE);
fast("HTMLText: a sparse node list", () => HTMLText(huge()), RE);
fast("HTMLStringify: a node with sparse children", () => HTMLStringify({ name: "p", attrs: {}, children: huge() }), RE);
fast("HTMLText: a node with sparse children", () => HTMLText({ name: "p", attrs: {}, children: huge() }), RE);
fast("Selector#all: a sparse node list", () => new Selector("b").all(huge()), RE);
const doc = HTMLParse("<p>a<b>c</b></p>");
eq(HTMLStringify(doc), "<p>a<b>c</b></p>", "a parsed document round-trips (control)");
eq(HTMLText(doc), "ac", "and its text is the concatenation (control)");
eq(new Selector("b").all(doc).length, 1, "and the selector finds the one <b> (control)");

print("test_html_node_list_bounds" + ": " + n + " checks, " + fails + " failures");
if (fails) throw new Error(fails + " failures");
