// flags: --std
// timeout: 180
// Split from tests/test_dos_guards.js: the html sections.
import { Selector, HTMLParse, MarkdownToHTML, Template } from "dyna:html";
import { domainToASCII } from "dyna:url";
import { ASN1 } from "dyna:serialize";
import { hostScale } from "../lib/hostscale.js";

// --- WS-NETWORK suite (SEC-037 selector, SEC-038 markdown, SEC-092/093 idna, SEC-057 asn1) ---
{
let n = 0, failed = 0;
function ok(c, msg) {
    n++;
    if (!c) {
        failed++;
        console.log("  FAIL " + msg);
    }
}
function budget(label, ms, fn) {
    ms = Math.round(ms * hostScale());
    const t0 = Date.now();
    let err = null;
    try { fn(); } catch (e) { err = e; }
    const dt = Date.now() - t0;
    ok(dt <= ms, label + " within " + ms + "ms (dt=" + dt + "ms" + (err ? ", " + String(err).slice(0, 40) : "") + ")");
}

{
    let html = "";
    for (let i = 0; i < 250; i++) html += "<div>";
    html += "x";
    for (let i = 0; i < 250; i++) html += "</div>";
    const doc = HTMLParse(html);
    budget("selector: 7-unit descendant miss over a 250-deep tree", 2000,
        () => new Selector("span div div div div div div").first(doc));
    budget("selector: 15-unit descendant miss over a 250-deep tree", 2000,
        () => new Selector("span div div div div div div div div div div div div div div").first(doc));
    budget("selector: matching 5-unit descendant", 2000,
        () => new Selector("div div div div div").first(doc));
}
{
    budget("markdown: 700k '*a ' openers", 2000,
        () => MarkdownToHTML("*a ".repeat(175000)));
    budget("markdown: 500k '[' openers", 2000,
        () => MarkdownToHTML("[".repeat(500000)));
    budget("markdown: 300k backticks", 2000,
        () => MarkdownToHTML("`".repeat(300000)));
    budget("markdown: mixed emphasis flood", 2000,
        () => MarkdownToHTML("_a *b ~c ".repeat(80000)));
}
{
    const label = "\u00FC".repeat(300000) + ".com";
    budget("idna: 300k-code-point label", 2500, () => {
        try { domainToASCII(label); } catch (e) {}
    });
    const ascii = "a".repeat(63) + ".com";
    budget("idna: a normal label stays fast", 500, () => domainToASCII(ascii));
}
{
    const a = [];
    a.length = 50000000;
    budget("template: 50M-element sparse section", 2000, () => {
        try { new Template("{{#a}}x{{/a}}").render({ a }); } catch (e) {}
    });
}
{
    const total = 1800000;
    const per = 60000;
    const parts = [];
    let left = total;
    while (left > 0) {
        const cnt = left > per ? per : left;
        const len = cnt * 2;
        const blob = new Uint8Array(5 + len);
        blob[0] = 0x30;
        blob[1] = 0x83;
        blob[2] = (len >> 16) & 0xff;
        blob[3] = (len >> 8) & 0xff;
        blob[4] = len & 0xff;
        for (let i = 0; i < cnt; i++) {
            blob[5 + i * 2] = 0x05;
            blob[6 + i * 2] = 0x00;
        }
        parts.push(blob);
        left -= cnt;
    }
    budget("asn1: 1.8M-node decode is refused fast", 2500, () => {
        for (const p of parts) {
            try { ASN1.decode(p); } catch (e) {}
        }
    });
}
console.log("test_html_dos_guards: " + (n - failed) + " passed, " + failed + " failed");
if (failed)
    throw new Error("test_html_dos_guards: " + failed + " failures");
}
