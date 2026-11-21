// timeout: 300
// covers: dyna:html.Selector
// Split from tests/test_audit_w2_native_2.js: the html sections.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_html_selector_audit_w2_native_2");
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

__kitS.section("x=\"<script>alert(1)</script>\">t</a>", () => {
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
});
__kitS.end();
if (failures) {
    print("test_html_audit_w2_native_2: " + failures + " of " + checks + " FAILED");
    throw new Error("test_html_audit_w2_native_2 failed");
}
print("test_html_audit_w2_native_2: " + checks + " checks passed");
