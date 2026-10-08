// flags: --std
import { Sitemap } from "dyna:scrape";

let pass = 0, fail = 0;
const ok = (c, m) => { if (c) pass++; else { fail++; print("  FAIL: " + m); } };
const eq = (a, b, m) => ok(JSON.stringify(a) === JSON.stringify(b),
                           m + " (got " + JSON.stringify(a) + ", want " + JSON.stringify(b) + ")");
const throws = (fn, kind, rx, m) => {
    try { fn(); fail++; print("  FAIL: " + m + " (did not throw)"); }
    catch (e) {
        if (!(e instanceof kind)) { fail++; print("  FAIL: " + m + " wrong kind " + (e && e.constructor.name)); return; }
        if (rx && !rx.test(e.message)) { fail++; print("  FAIL: " + m + " message [" + e.message + "]"); return; }
        pass++;
    }
};

eq(Sitemap.parse('<?xml version="1.0" encoding="UTF-8"?>\n' +
   '<urlset xmlns="http://www.sitemaps.org/schemas/sitemap/0.9">' +
   '<url><loc>http://x.test/</loc><lastmod>2026-01-01</lastmod>' +
   '<changefreq>daily</changefreq><priority>0.8</priority></url>' +
   '<url><loc>http://x.test/b</loc></url></urlset>'),
   ["http://x.test/", "http://x.test/b"],
   "urlset parses in order; siblings of <loc> are ignored");
eq(Sitemap.parse('<sitemapindex xmlns="http://www.sitemaps.org/schemas/sitemap/0.9">' +
   '<sitemap><loc>http://x.test/s1.xml</loc></sitemap>' +
   '<sitemap><loc>http://x.test/s2.xml</loc></sitemap></sitemapindex>'),
   ["http://x.test/s1.xml", "http://x.test/s2.xml"],
   "sitemapindex parses to its child sitemap urls");
eq(Sitemap.parse("<urlset></urlset>"), [], "an empty urlset is an empty list");
eq(Sitemap.parse("<urlset/>"), [], "a self-closed root is an empty list");

eq(Sitemap.parse("<urlset>\n  <url>\n    <loc>\n    http://x.test/spaced  </loc>\n  </url>\n</urlset>"),
   ["http://x.test/spaced"], "surrounding whitespace is trimmed");
eq(Sitemap.parse('<ns:urlset><ns:url><ns:loc>http://x.test/prefixed</ns:loc></ns:url></ns:urlset>').length,
   0, "namespaced <loc> is not a sitemap-vocabulary loc (spec ships unprefixed)");
eq(Sitemap.parse('<urlset><url><LOC>http://x.test/upper</LOC></url></urlset>'),
   ["http://x.test/upper"], "element names match case-insensitively");
eq(Sitemap.parse('<!-- a comment --><urlset><!-- another --><url><loc>http://x.test/c</loc></url></urlset><!-- tail -->'),
   ["http://x.test/c"], "comments are skipped");
eq(Sitemap.parse('<?xml-stylesheet type="text/xsl" href="x"?><urlset><url><loc>http://x.test/pi</loc></url></urlset>'),
   ["http://x.test/pi"], "processing instructions are skipped");
eq(Sitemap.parse('<!DOCTYPE urlset><urlset><url><loc>http://x.test/dt</loc></url></urlset>'),
   ["http://x.test/dt"], "DOCTYPE is skipped");
eq(Sitemap.parse('<urlset><url><loc>http://x.test/a</loc><loc>http://x.test/b</loc></url></urlset>'),
   ["http://x.test/a", "http://x.test/b"], "two locs in one <url> both count");
eq(Sitemap.parse('<urlset><url><loc attr="ignored">http://x.test/attr</loc></url></urlset>'),
   ["http://x.test/attr"], "attributes on <loc> are ignored");

eq(Sitemap.parse('<urlset><url><loc>http://x.test/p?a=1&amp;b=2</loc></url></urlset>'),
   ["http://x.test/p?a=1&b=2"], "&amp; decodes");
eq(Sitemap.parse('<urlset><url><loc>http://x.test/q?a=1&#38;b=2</loc></url></urlset>'),
   ["http://x.test/q?a=1&b=2"], "&#38; decodes");
eq(Sitemap.parse('<urlset><url><loc>http://x.test/r?a=1&#x26;b=2</loc></url></urlset>'),
   ["http://x.test/r?a=1&b=2"], "&#x26; decodes");
eq(Sitemap.parse('<urlset><url><loc>http://x.test/s?w=%26amp%3B</loc></url></urlset>'),
   ["http://x.test/s?w=%26amp%3B"], "pct-encoded text rides through untouched");
eq(Sitemap.parse('<urlset><url><loc>http://x.test/t?a=1&b=2</loc></url></urlset>'),
   ["http://x.test/t?a=1&b=2"], "a bare & is literal (lenient, like the wild web)");
eq(Sitemap.parse('<urlset><url><loc>http://x.test/u?a=&nosuch;</loc></url></urlset>'),
   ["http://x.test/u?a=&nosuch;"], "an unknown entity is literal text");

throws(() => Sitemap.parse("<urlset><url><loc>http://x.test/unclosed</url></urlset>"),
       SyntaxError, /never closes/, "a <loc> with no </loc> is a SyntaxError");
throws(() => Sitemap.parse("<!-- unterminated"),
       SyntaxError, /unterminated comment at byte 0/, "an unterminated comment is a SyntaxError");
throws(() => Sitemap.parse("<urlset><url><loc>ok</loc"),
       SyntaxError, /never closes|unterminated/, "a truncated document is a SyntaxError");
throws(() => Sitemap.parse("User-agent: *\nDisallow: /"),
       TypeError, /not an XML document/, "robots.txt is not a sitemap (TypeError)");
throws(() => Sitemap.parse("just some words"),
       TypeError, /not an XML document/, "plain text is a TypeError");

throws(() => Sitemap.parse("<urlset>" + "<url><loc>http://x/</loc></url>".repeat(50001)),
       RangeError, /more than 50000/, "over 50000 locs refuses");
throws(() => Sitemap.parse("<urlset>" + "<!--" + "x".repeat(17 * 1024 * 1024) + "-->"),
       RangeError, /exceeds/, "over 16 MiB refuses");
ok(Sitemap.parse("<urlset>" + "<url><loc>http://x.test/u</loc></url>".repeat(49999)).length === 49999,
   "49999 locs parse fine (the cap is 50000)");
eq(Sitemap.parse("<urlset><url><loc>" + "x".repeat(2048) + "</loc></url></urlset>").length, 1,
   "a loc of EXACTLY 2048 bytes (the spec's url budget) parses");
throws(() => Sitemap.parse("<urlset><url><loc>" + "x".repeat(2049) + "</loc></url></urlset>"),
       SyntaxError, /never closes/, "a loc of 2049 bytes refuses -- the bound is exact, no slop");
throws(() => Sitemap.parse("<urlset><url><loc>http://x.test/" + "x".repeat(3000) + "</loc></url></urlset>"),
       SyntaxError, /never closes/, "a loc over the 2048-byte url budget is a SyntaxError");
throws(() => Sitemap.parse("<urlset><url><loc>open".repeat(2000)),
       SyntaxError, /never closes/, "a document of open loc tags fails fast, not quadratically");

const mock = (docs) => ({
    get(url) {
        if (!(url in docs)) throw new Error("unexpected fetch " + url);
        return { status: 200, body: docs[url] };
    }
});
const indexOf = (urls) => "<sitemapindex>" + urls.map((u) =>
    "<sitemap><loc>" + u + "</loc></sitemap>").join("") + "</sitemapindex>";
const locs = (n) => "<urlset>" +
    "<url><loc>http://x.test/u</loc></url>".repeat(n) + "</urlset>";

{
    const idx = "http://x.test/idx.xml";
    const two = locs(30000);
    throws(() => Sitemap.list(idx, mock({
        [idx]: indexOf(["http://x.test/b1", "http://x.test/b2"]),
        "http://x.test/b1": two, "http://x.test/b2": two })),
        RangeError, /more than 50000 URLs across the sitemapindex/,
        "a 2x30000 sitemapindex over the 50000 aggregate URL cap refuses");
}
{
    const idx = "http://x.test/idx.xml";
    const half = locs(25000);
    const got = Sitemap.list(idx, mock({
        [idx]: indexOf(["http://x.test/b1", "http://x.test/b2"]),
        "http://x.test/b1": half, "http://x.test/b2": half }));
    ok(got.length === 50000, "exactly 50000 aggregate URLs is accepted");
    ok(got[49999] === "http://x.test/u", "the last aggregate URL survives");
}
{
    const kids = [];
    for (let i = 0; i < 1001; i++) kids.push("http://x.test/c" + i);
    const idx = "http://x.test/idx.xml";
    const one = locs(1);
    const docs = { [idx]: indexOf(kids) };
    for (const k of kids) docs[k] = one;
    let fetches = 0;
    const f = { get(u) { fetches++; return { status: 200, body: docs[u] }; } };
    throws(() => Sitemap.list(idx, f), RangeError, /more than 1000 child sitemaps/,
        "an index over the 1000-child cap refuses");
    ok(fetches === 1, "the child cap refuses before fetching any child");
}
{
    const kids = [];
    for (let i = 0; i < 1000; i++) kids.push("http://x.test/c" + i);
    const idx = "http://x.test/idx.xml";
    const docs = { [idx]: indexOf(kids) };
    for (const k of kids) docs[k] = locs(1);
    const got = Sitemap.list(idx, mock(docs));
    ok(got.length === 1000, "exactly 1000 children is accepted");
    ok(got[999] === "http://x.test/u", "the last child URL survives");
}

print((pass + fail) + " asserts: " + pass + " pass, " + fail + " fail");
if (fail) throw new Error("test_scrape_sitemap: " + fail + " failures");
