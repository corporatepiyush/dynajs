// flags: --std
// timeout: 180

import { nchoosek, besselk, besselkScaled, besselh } from "dyna:mathx";
import { Random } from "dyna:random";
import { Selector, HTMLParse, MarkdownToHTML, Template } from "dyna:html";
import { domainToASCII } from "dyna:url";
import { ASN1 } from "dyna:serialize";

// --- WS-PLATFORM suite (SEC-024 nchoosek, SEC-025 besselk, SEC-026 sample) ---
{
let n = 0;
function assert(c, msg) { n++; if (!c) throw new Error("assertion failed: " + msg); }
function timed(fn) { const t0 = Date.now(); const v = fn(); return { v, dt: Date.now() - t0 }; }

{
    const r = timed(() => nchoosek(1e18, 5e17));
    assert(r.v === Infinity, "nchoosek(1e18,5e17) overflows to Infinity (got " + r.v + ")");
    assert(r.dt < 2000, "nchoosek(1e18,5e17) returns promptly (" + r.dt + " ms)");
}
{
    const r = timed(() => nchoosek(1e12, 5e11));
    assert(r.v === Infinity, "nchoosek(1e12,5e11) overflows to Infinity (got " + r.v + ")");
    assert(r.dt < 2000, "nchoosek(1e12,5e11) returns promptly (" + r.dt + " ms)");
}
assert(nchoosek(5, 2) === 10, "nchoosek(5,2) is 10");
assert(nchoosek(10, 0) === 1, "nchoosek(10,0) is 1");
assert(nchoosek(0, 0) === 1, "nchoosek(0,0) is 1");
assert(nchoosek(4, 5) === 0, "nchoosek(4,5) is 0");
assert(nchoosek(1e18, 1) === 1e18, "nchoosek(1e18,1) is n");
assert(nchoosek(67, 33) < Infinity, "nchoosek(67,33) stays finite");

{
    const r = timed(() => besselk(1e9, 0.1));
    assert(r.v === Infinity, "besselk(1e9,0.1) is +Infinity (the limit in nu), not an O(nu) recurrence (got " + r.v + ")");
    assert(r.dt < 2000, "besselk(1e9,0.1) returns promptly (" + r.dt + " ms)");
}
{
    const r = timed(() => besselk(3e9, 0.1));
    assert(r.v === Infinity, "besselk(3e9,0.1) is +Infinity without an int cast (got " + r.v + ")");
    assert(r.dt < 2000, "besselk(3e9,0.1) returns promptly (" + r.dt + " ms)");
}
{
    const r = timed(() => besselkScaled(1e9, 0.25));
    assert(r.v === Infinity, "besselkScaled(1e9,0.25) is +Infinity (got " + r.v + ")");
    assert(r.dt < 2000, "besselkScaled(1e9,0.25) returns promptly (" + r.dt + " ms)");
}
{
    const r = timed(() => besselk(1e9, 1));
    assert(r.dt < 2000, "besselk(1e9,1) terminates promptly (" + r.dt + " ms)");
}
{
    const r = timed(() => besselk(1e300, 0.25));
    assert(r.v === Infinity, "besselk(1e300,0.25) keeps the +Infinity convention");
    assert(r.dt < 2000, "besselk(1e300,0.25) prompt");
}
{
    const r = timed(() => besselk(1e6, 0.1));
    assert(r.dt < 2000, "besselk(1e6,0.1) bounded recurrence (" + r.dt + " ms)");
}
{
    let threw = false;
    try { besselh(2e7, 2e8); } catch (e) { threw = e instanceof RangeError; }
    assert(threw, "besselh(2e7,2e8) refuses an order that would recurse for seconds");
}
assert(besselh(3, 2.5)[0] !== undefined, "besselh small order still answers");

{
    const r0 = new Random(1234);
    const huge = new Array(4294967295);
    const r = timed(() => r0.sample(huge, 0));
    assert(Array.isArray(r.v) && r.v.length === 0, "sample(huge,0) is empty");
    assert(r.dt < 2000, "sample(huge,0) does not materialize indices (" + r.dt + " ms)");
}
{
    const r0 = new Random(1234);
    const huge = new Array(4294967295);
    huge[0] = "a";
    huge[7] = "b";
    huge[4294967294] = "z";
    const r = timed(() => r0.sample(huge, 2));
    assert(r.v.length === 2, "sample(huge,2) yields 2 entries");
    assert(r.dt < 2000, "sample(huge,2) does not materialize indices (" + r.dt + " ms)");
}
{
    const r0 = new Random(99);
    const small = [10, 20, 30, 40, 50];
    const got = r0.sample(small, 5).slice().sort((a, b) => a - b);
    assert(got.join(",") === "10,20,30,40,50", "sample of all elements is a permutation");
    const three = r0.sample(small, 3);
    assert(three.length === 3 && new Set(three).size === 3, "sample(n) has distinct entries");
    assert(three.every((v) => small.includes(v)), "sample entries come from the source");
}
{
    const r0 = new Random(7);
    const ta = new Uint8Array([1, 2, 3, 4, 5, 6]);
    const got = r0.sample(ta, 3);
    assert(got instanceof Uint8Array && got.length === 3, "typed-array sample keeps the type");
    assert(got.every((v) => v >= 1 && v <= 6), "typed-array sample entries in range");
}
{
    const r0 = new Random(7);
    let threw = false;
    try { r0.sample([1, 2, 3], -1); } catch (e) { threw = e instanceof RangeError; }
    assert(threw, "sample with negative n throws RangeError");
}

print("test_dos_guards: all tests passed (" + n + " assertions)");
}

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
console.log("test_dos_guards: " + (n - failed) + " passed, " + failed + " failed");
if (failed)
    throw new Error("test_dos_guards: " + failed + " failures");
}
