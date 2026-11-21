// timeout: 120
// covers: dyna:html.HTMLStringify
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_html_html_stringify_hardening");
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

__kitS.section("textarea children are escaped", () => {
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
});

__kitS.section("NUL in text is replaced, not emitted raw", () => {
    const out = HTMLStringify({ name: "p", attrs: {}, children: ["a\u0000b"] });
    ok(out.indexOf("\u0000") === -1, "NUL in text is replaced, not emitted raw (got " + JSON.stringify(out) + ")");
    ok(out.indexOf("\uFFFD") !== -1, "NUL becomes U+FFFD");
    const out2 = HTMLStringify({ name: "script", attrs: {}, children: ["a\u0000b"] });
    ok(out2.indexOf("\u0000") === -1, "NUL in script raw text is replaced");
});
__kitS.end();
console.log("test_html_hardening: " + (n - failed) + " passed, " + failed + " failed");
if (failed)
    throw new Error("test_html_hardening: " + failed + " failures");
