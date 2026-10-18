// flags: --std
// timeout: 120
import { HTMLParse, HTMLStringify, Selector, MarkdownToHTML } from "dyna:html";

let n = 0, failed = 0;
function ok(c, msg) {
    n++;
    if (!c) {
        failed++;
        console.log("  FAIL " + msg);
    }
}

{
    const doc = HTMLParse("<textarea></textarea><script></script><title></title>");
    const ta = { name: "textarea", attrs: {}, children: ["</textarea><script>alert(1)</script>"] };
    const out = HTMLStringify(ta);
    ok(out.indexOf("</textarea><script>") === -1,
        "textarea children are escaped (got " + out + ")");
    ok(out === "<textarea>&lt;/textarea&gt;&lt;script&gt;alert(1)&lt;/script&gt;</textarea>",
        "textarea RCDATA round-trips escaped");
    const ti = { name: "title", attrs: {}, children: ["a </title><b>x</b>"] };
    ok(HTMLStringify(ti).indexOf("</title><b>") === -1, "title children are escaped");
    const sc = { name: "script", attrs: {}, children: ["if (a < b) { x(); }"] };
    ok(HTMLStringify(sc) === "<script>if (a < b) { x(); }</script>",
        "script stays raw (got " + HTMLStringify(sc) + ")");
    ok(HTMLStringify(doc[0]).indexOf("textarea") !== -1, "parse+stringify still works");
}
{
    const out = HTMLStringify({ name: "p", attrs: {}, children: ["a\u0000b"] });
    ok(out.indexOf("\u0000") === -1, "NUL in text is replaced, not emitted raw (got " + JSON.stringify(out) + ")");
    ok(out.indexOf("\uFFFD") !== -1, "NUL becomes U+FFFD");
    const out2 = HTMLStringify({ name: "script", attrs: {}, children: ["a\u0000b"] });
    ok(out2.indexOf("\u0000") === -1, "NUL in script raw text is replaced");
}
{
    const deep = "<div>".repeat(200) + "x" + "</div>".repeat(200);
    const doc = HTMLParse(deep);
    const t0 = Date.now();
    const r = new Selector("span div div div div div div").first(doc);
    const dt = Date.now() - t0;
    ok(r === undefined && dt < 2000,
        "a 7-unit descendant selector over a 200-deep tree terminates (dt=" + dt + "ms)");
    const t1 = Date.now();
    const r2 = new Selector("div div div").first(doc);
    ok(r2 !== undefined && Date.now() - t1 < 2000, "a short descendant selector still matches");
}
{
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
}
{
    const out = MarkdownToHTML("&copy; &amp; &lt; &bogus;");
    ok(out.indexOf("&copy;") === -1, "a valid named reference is decoded (got " + out + ")");
    ok(out.indexOf("\u00A9") !== -1, "&copy; becomes the character");
    ok(out.indexOf("&amp;amp;") === -1, "&amp; is not double-escaped");
    ok(out.indexOf("&amp;bogus;") !== -1, "an invalid reference is escaped");
    ok(MarkdownToHTML("a & b").indexOf("&amp;") !== -1, "a bare ampersand is still escaped");
    ok(MarkdownToHTML("&#65;").indexOf("A") !== -1, "numeric references decode");
}
{
    const md = MarkdownToHTML("*a ".repeat(60000));
    ok(typeof md === "string" && md.length > 0, "adversarial markdown still returns");
}
console.log("test_html_hardening: " + (n - failed) + " passed, " + failed + " failed");
if (failed)
    throw new Error("test_html_hardening: " + failed + " failures");
