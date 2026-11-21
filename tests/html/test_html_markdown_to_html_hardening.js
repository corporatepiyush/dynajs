// timeout: 120
// covers: dyna:html.MarkdownToHTML
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_html_markdown_to_html_hardening");
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

__kitS.section("&copy; &amp; &lt; &bogus;", () => {
    const out = MarkdownToHTML("&copy; &amp; &lt; &bogus;");
    ok(out.indexOf("&copy;") === -1, "a valid named reference is decoded (got " + out + ")");
    ok(out.indexOf("\u00A9") !== -1, "&copy; becomes the character");
    ok(out.indexOf("&amp;amp;") === -1, "&amp; is not double-escaped");
    ok(out.indexOf("&amp;bogus;") !== -1, "an invalid reference is escaped");
    ok(MarkdownToHTML("a & b").indexOf("&amp;") !== -1, "a bare ampersand is still escaped");
    ok(MarkdownToHTML("&#65;").indexOf("A") !== -1, "numeric references decode");
});

__kitS.section("adversarial markdown still returns", () => {
    const md = MarkdownToHTML("*a ".repeat(60000));
    ok(typeof md === "string" && md.length > 0, "adversarial markdown still returns");
});
__kitS.end();
console.log("test_html_hardening: " + (n - failed) + " passed, " + failed + " failed");
if (failed)
    throw new Error("test_html_hardening: " + failed + " failures");
