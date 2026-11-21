// covers: c:src/core/crawlkit/crawlkit-sitemap.c
// Sitemap.parse jumps between tags with memchr('<') instead of stepping one
// byte at a time, finds `</loc` inside the 2048-byte url budget with memchr
// too, and hands the entity-decoded url buffer to the result list instead of
// copying it again. Modperf round 2026-11-16: 10K-url sitemap 2.06 ms ->
// 1.6 ms. Oracle: a JS scanner over the same grammar (comments, <?..?>,
// CDATA and <!..> skipped; <loc> case-insensitive, may carry attributes or
// self-close; body trimmed of space/tab/CR/LF; &amp; &lt; &gt; &quot; &apos;
// &#N; &#xH; decoded) and the budget edge computed from the body length.
import { suite } from "../lib/kit.js";
import { Sitemap } from "dyna:scrape";

const t = suite("test_scrape_sitemap_parse_scan");

const ENT = { amp: "&", lt: "<", gt: ">", quot: '"', apos: "'" };
function decode(s) {
    let out = "";
    for (let i = 0; i < s.length; ) {
        if (s[i] === "&" && s.length - i >= 4) {
            const m = /^&(?:(amp|lt|gt|quot|apos);|#([0-9]+);|#[xX]([0-9a-fA-F]+);)/.exec(s.slice(i));
            if (m) {
                const cp = m[1] ? null : m[2] ? parseInt(m[2], 10) : parseInt(m[3], 16);
                if (m[1]) out += ENT[m[1]];
                else out += String.fromCodePoint(cp);
                i += m[0].length;
                continue;
            }
        }
        out += s[i++];
    }
    return out;
}
function oracle(x) {
    const urls = [];
    let i = 0;
    while (i < x.length) {
        const lt = x.indexOf("<", i);
        if (lt < 0) break;
        i = lt;
        if (x.startsWith("<!--", i)) { i = x.indexOf("-->", i + 4) + 3; continue; }
        if (x.startsWith("<?", i)) { i = x.indexOf("?>", i + 2) + 2; continue; }
        if (/^<!\[CDATA\[/i.test(x.slice(i, i + 9))) { i = x.indexOf("]]>", i + 9) + 3; continue; }
        if (x.startsWith("<!", i)) { i = x.indexOf(">", i + 2) + 1; continue; }
        const m = /^<(\/?)([^>\/ \t\r\n]*)/.exec(x.slice(i));
        const gt = x.indexOf(">", i);
        if (!m[1] && m[2].toLowerCase() === "loc") {
            if (x[gt - 1] === "/") { i = gt + 1; continue; }
            const body = gt + 1;
            const close = x.slice(body).search(/<\/loc/i);
            const raw = x.slice(body, body + close).replace(/^[ \t\r\n]+|[ \t\r\n]+$/g, "");
            if (raw) urls.push(decode(raw));
            i = x.indexOf(">", body) + 1;
            continue;
        }
        i = gt + 1;
    }
    return urls;
}

let seed = 99;
const rnd = (n) => ((seed = (seed * 1103515245 + 12345) & 0x7fffffff) % n);
const BODY = ["https://e.com/", "a", "&amp;", "&lt;b&gt;", "&#65;", "&#x263A;", "&quot;&apos;", "é", "?q=1", " ", "\t", "&bogus;", "&am"];
const FILL = ["<!-- <loc>no</loc> -->", "<?pi <loc>no</loc> ?>", "<![CDATA[<loc>no</loc>]]>", "<!DOCTYPE urlset>", "<lastmod>2026-01-01</lastmod>", "<priority>0.5</priority>", "\n  ", "<url>", "</url>", "<image:loc>https://i/</image:loc>"];

t.test("generated sitemaps agree with the JS scanner", ({ eq }) => {
    for (let round = 0; round < 600; round++) {
        let x = rnd(3) ? '<?xml version="1.0"?>\n<urlset xmlns="http://www.sitemaps.org/schemas/sitemap/0.9">' : "<sitemapindex>";
        for (let k = rnd(25); k > 0; k--) {
            if (rnd(3) === 0) { x += FILL[rnd(FILL.length)]; continue; }
            let body = "";
            for (let b = 1 + rnd(5); b > 0; b--) body += BODY[rnd(BODY.length)];
            const open = ["<loc>", "<LOC>", "<Loc a='1'>", "<loc\n>"][rnd(4)];
            x += rnd(15) ? "<url>" + open + body + ["</loc>", "</LOC>", "</loc >"][rnd(3)] + "</url>" : "<url><loc/></url>";
        }
        x += "</urlset>";
        eq(JSON.stringify(Sitemap.parse(x)), JSON.stringify(oracle(x)), "round " + round);
    }
});

t.test("the 2048-byte url budget edge", ({ eq, throws }) => {
    for (const L of [1, 63, 64, 65, 2046, 2047, 2048]) {
        const u = "h" + "x".repeat(L - 1);
        eq(JSON.stringify(Sitemap.parse("<urlset><url><loc>" + u + "</loc></url></urlset>")), JSON.stringify([u]), "body of " + L + " bytes");
    }
    for (const L of [2049, 2050, 4000])
        throws(() => Sitemap.parse("<urlset><url><loc>" + "x".repeat(L) + "</loc></url></urlset>"), SyntaxError, "body of " + L + " bytes never closes inside the budget");
    eq(JSON.stringify(Sitemap.parse("<urlset><loc>" + "<b>".repeat(500) + "</loc></urlset>")), JSON.stringify(["<b>".repeat(500)]), "many '<' inside the body before </loc");
    throws(() => Sitemap.parse("<urlset><loc>abc</lo"), SyntaxError, "a close cut short at the end of input");
    throws(() => Sitemap.parse("<urlset><loc>abc</loc"), SyntaxError, "a close tag with no '>' after it");
    eq(JSON.stringify(Sitemap.parse("<urlset><loc>abc</loc\n>x")), JSON.stringify(["abc"]), "the close tag ends at the next '>'");
});

t.test("many urls in one document", ({ eq }) => {
    let x = "<urlset>";
    const want = [];
    for (let i = 0; i < 20000; i++) {
        x += "<url>\n <loc> https://e.com/p/" + i + "?a=1&amp;b=" + (i % 7) + " </loc>\n <lastmod>2026-01-0" + ((i % 9) + 1) + "</lastmod></url>";
        want.push("https://e.com/p/" + i + "?a=1&b=" + (i % 7));
    }
    x += "</urlset>";
    const got = Sitemap.parse(x);
    eq(got.length, want.length, "count");
    eq(got.every((u, i) => u === want[i]), true, "every url");
});

await t.run();
