// flags: --std
// timeout: 300
// Split from tests/test_audit_w2_native_2.js: the html sections.
import { MarkdownToHTML, HTMLParse, Selector, Sanitizer, rewriteLinks } from "dyna:html";

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
{
    const time = (src) => { const t0 = performance.now(); const out = MarkdownToHTML(src); return [performance.now() - t0, out]; };
    time("warm *up* `x`");
    for (const [name, mk] of [
        ["star run", (n) => "*".repeat(n) + "a" + "*".repeat(n)],
        ["underscore run", (n) => "_".repeat(n) + "a"],
        ["tilde run", (n) => "~".repeat(n) + "a"],
    ]) {
        const [t1] = time(mk(20000)), [t4] = time(mk(80000));
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
        const [tp] = time(plain), [tq] = time(quoted);
        ok(tq < Math.max(tp, 1) * 6, "blockquotes share the document's work budget (plain " + tp.toFixed(1) + "ms, quoted " + tq.toFixed(1) + "ms)");
    }
}
// ---- html ---------------------------------------------------------------------------------
{
    let tag = "<a";
    for (let i = 0; i < 520; i++) tag += " a" + i + "=1";
    tag += ' x="<script>alert(1)</script>">t</a>';
    const tree = HTMLParse(tag);
    const flat = JSON.stringify(tree);
    ok(!flat.includes('"script"'), "attributes past the cap are discarded, never re-read as markup");
    const a = Array.isArray(tree) ? tree[0] : tree;
    eq(JSON.stringify(a.children), '["t"]', "the element keeps its single text child");
    const sl = HTMLParse('<a/href="/x">t</a>');
    const a2 = Array.isArray(sl) ? sl[0] : sl;
    eq(a2.attrs && a2.attrs.href, "/x", "a slash between attributes is whitespace, as in browsers");
    const sc = HTMLParse("<br/>after");
    eq(JSON.stringify(sc).includes("after"), true, "a real self-closing slash still works (control)");

    const doc = HTMLParse("<p>x</p><div id=a>y</div>");
    eq(new Selector("[constructor]").all(doc).length, 0, "[constructor] matches no element");
    eq(new Selector("[__proto__]").all(doc).length, 0, "[__proto__] matches no element");
    eq(new Selector("[toString]").all(doc).length, 0, "[toString] matches no element");
    eq(new Selector("[id]").all(doc).length, 1, "[id] matches the one element that has it (control)");
    eq(new Selector("[id=a]").all(doc).length, 1, "[id=a] value match (control)");

    const s7 = new Sanitizer({ allow: { a: ["href"] }, protocols: { "a.href": ["http", "https", "mailto", "ftp", "tel", "sms", "geo"] } });
    ok(s7.clean('<a href="geo:1,2">g</a>').includes("geo:1,2"), "the seventh scheme of one protocols key is honoured");
    ok(!s7.clean('<a href="javascript:1">g</a>').includes("javascript"), "an unlisted scheme is still dropped (control)");

    let deep = { name: "b", attrs: {}, children: ["x"] };
    for (let i = 0; i < 300; i++) deep = { name: "a", attrs: { href: "/p" }, children: [deep] };
    eq(errName(() => new Selector("a b").all(deep)), "RangeError", "Selector refuses a tree deeper than the parser's bound instead of skipping it");
    eq(errName(() => rewriteLinks(deep, (u) => u)), "RangeError", "rewriteLinks refuses the same tree instead of rewriting only the top 256 levels");
}
if (failures) {
    print("test_html_audit_w2_native_2: " + failures + " of " + checks + " FAILED");
    throw new Error("test_html_audit_w2_native_2 failed");
}
print("test_html_audit_w2_native_2: " + checks + " checks passed");
