// timeout: 300
// covers: dyna:html.MarkdownToHTML
// Split from tests/test_audit_w2_native_2.js: the html sections.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_html_markdown_to_html_audit_w2_native_2");
import { MarkdownToHTML, HTMLParse, Selector, Sanitizer, rewriteLinks } from "dyna:html";
import { cpuNow } from "../lib/cpuclock.js";

let failures = 0, checks = 0;
function ok(cond, msg) {
    checks++;
    if (!cond) {
        failures++;
        print("  FAIL:", msg);
    }
}
function eq(got, want, msg) {
    ok(Object.is(got, want), msg + " (got " + String(got).slice(0, 80) + ", want " + String(want).slice(0, 80) + ")");
}
function errName(fn) {
    try { fn(); return "none"; } catch (e) { return e.name; }
}
function errMsg(fn) {
    try { fn(); return ""; } catch (e) { return String(e.message); }
}
// ---- markdown ------------------------------------------------------------------------

__kitS.section("warm *up* `x`", () => {
    const time = (src) => { const t0 = cpuNow(); const out = MarkdownToHTML(src); return [cpuNow() - t0, out]; };
    time("warm *up* `x`");
    for (const [name, mk] of [
        ["star run", (n) => "*".repeat(n) + "a" + "*".repeat(n)],
        ["underscore run", (n) => "_".repeat(n) + "a"],
        ["tilde run", (n) => "~".repeat(n) + "a"],
    ]) {
        const s1 = mk(20000), s4 = mk(80000);
        let t1 = Infinity, t4 = Infinity;
        for (let r = 0; r < 5; r++) { t1 = Math.min(t1, time(s1)[0]); t4 = Math.min(t4, time(s4)[0]); }
        ok(t4 < Math.max(t1, 0.5) * 12, "markdown " + name + " is linear (20k " + t1.toFixed(1) + "ms, 80k " + t4.toFixed(1) + "ms)");
    }
    {
        let s = "";
        for (let i = 1; i <= 1200; i++) s += "`".repeat(i) + " ";
        const [t] = time(s);
        ok(t < 3000, "backtick runs of every length stay inside the work budget (" + t.toFixed(0) + "ms)");
    }
    eq(MarkdownToHTML("*****a*****").replace(/\s+$/, ""), "<p>**<strong>a</strong>**</p>", "long delimiter runs render exactly as before (control)");
    eq(MarkdownToHTML("****a**** ~~~b~~~ ______c").replace(/\s+$/, ""), "<p>*<strong>a</strong>* ~<del>b</del>~ ______c</p>", "mixed over-long runs render exactly as before (control)");
    eq(MarkdownToHTML("**b** and `c` and ~~d~~").replace(/\s+$/, ""), "<p><strong>b</strong> and <code>c</code> and <del>d</del></p>", "ordinary inline markup (control)");
    {
        const links = MarkdownToHTML("[a](http://x) ".repeat(2000));
        eq(links.split("<a ").length - 1, 2000, "2000 links in one paragraph all render");
    }
    {
        const block = "[a".repeat(2000);
        const plain = (block + "\n\n").repeat(200), quoted = ("> " + block + "\n\n").repeat(200);
        let tp = Infinity, tq = Infinity;
        for (let r = 0; r < 3; r++) { tp = Math.min(tp, time(plain)[0]); tq = Math.min(tq, time(quoted)[0]); }
        ok(tq < Math.max(tp, 1) * 6, "blockquotes share the document's work budget (plain " + tp.toFixed(1) + "ms, quoted " + tq.toFixed(1) + "ms)");
    }
});
__kitS.end();
if (failures) {
    print("test_html_audit_w2_native_2: " + failures + " of " + checks + " FAILED");
    throw new Error("test_html_audit_w2_native_2 failed");
}
print("test_html_audit_w2_native_2: " + checks + " checks passed");
