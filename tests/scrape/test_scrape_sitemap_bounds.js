// timeout: 120
// covers: dyna:scrape.Sitemap
// R5-4: Sitemap.list aggregate caps at the exact boundary: 999 children with
// exactly 50000 URLs (mixed per-child counts) and the full 1000-child /
// 50000-URL index both complete; one child or URL past each cap refuses.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_scrape_sitemap_bounds");
import { Sitemap } from "dyna:scrape";
import { cpuNow } from "../lib/cpuclock.js";

let n = 0, fails = 0;
function ok(c, m) { n++; if (!c) { fails++; print("FAIL: " + m); } }
function ms(t) { return " (" + t + "ms)"; }

const indexOf = (urls) => "<sitemapindex>" + urls.map((u) =>
    "<sitemap><loc>" + u + "</loc></sitemap>").join("") + "</sitemapindex>";
const locs = (n) => "<urlset>" +
    "<url><loc>http://x.test/u</loc></url>".repeat(n) + "</urlset>";
const mock = (docs) => ({
    get(url) {
        if (!(url in docs)) throw new Error("unexpected fetch " + url);
        return { status: 200, body: docs[url] };
    }
});

await __kitS.section("999 children carrying exactly 50000 URLs is accepted", async () => {
    // 999 children, exactly 50000 URLs total: 998 x 50 + 1 x 100.
    const kids = [];
    for (let i = 0; i < 999; i++) kids.push("http://x.test/c" + i);
    const docs = { "http://x.test/idx.xml": indexOf(kids) };
    for (let i = 0; i < 998; i++) docs[kids[i]] = locs(50);
    docs[kids[998]] = locs(100);
    const t0 = cpuNow();
    const got = await Sitemap.list("http://x.test/idx.xml", mock(docs));
    const t = Math.round(cpuNow() - t0);
    ok(got.length === 50000, "999 children carrying exactly 50000 URLs is accepted (got "
        + got.length + ")" + ms(t));
    ok(got[49999] === "http://x.test/u", "the last of the 50000 URLs survives" + ms(t));
});

await __kitS.section("unexpected fetch", async () => {
    // The full allowed index: 1000 children x 50 URLs = 50000, all fetched.
    const kids = [];
    for (let i = 0; i < 1000; i++) kids.push("http://x.test/d" + i);
    const docs = { "http://x.test/idx2.xml": indexOf(kids) };
    for (const k of kids) docs[k] = locs(50);
    let fetches = 0;
    const f = {
        get(u) { fetches++; if (!(u in docs)) throw new Error("unexpected fetch " + u); return { status: 200, body: docs[u] }; }
    };
    const t0 = cpuNow();
    const got = await Sitemap.list("http://x.test/idx2.xml", f);
    const t = Math.round(cpuNow() - t0);
    ok(got.length === 50000, "the full 1000-child / 50000-URL index completes (got "
        + got.length + ")" + ms(t));
    ok(fetches === 1001, "every child was fetched exactly once (got " + fetches + ")" + ms(t));
    ok(t < 30000, "the full index finishes inside a generous CPU-time bound" + ms(t));
});

await __kitS.section("50001 aggregate URLs refuses with the named RangeError", async () => {
    // One URL past the aggregate cap: 1000 children each with 50, plus one
    // extra URL on the last child (50001 total).
    const kids = [];
    for (let i = 0; i < 1000; i++) kids.push("http://x.test/e" + i);
    const docs = { "http://x.test/idx3.xml": indexOf(kids) };
    for (let i = 0; i < 999; i++) docs[kids[i]] = locs(50);
    docs[kids[999]] = locs(51);
    let threw = null;
    try { await Sitemap.list("http://x.test/idx3.xml", mock(docs)); }
    catch (e) { threw = e; }
    ok(threw instanceof RangeError && /more than 50000 URLs/.test(threw.message),
        "50001 aggregate URLs refuses with the named RangeError (got " + threw + ")");
});

await __kitS.section("1001 children refuses with the named RangeError", async () => {
    // One child past the cap: 1001 children.
    const kids = [];
    for (let i = 0; i < 1001; i++) kids.push("http://x.test/f" + i);
    const docs = { "http://x.test/idx4.xml": indexOf(kids) };
    for (const k of kids) docs[k] = locs(1);
    let threw = null, fetches = 0;
    try {
        await Sitemap.list("http://x.test/idx4.xml", {
            get(u) { fetches++; return { status: 200, body: docs[u] }; }
        });
    } catch (e) { threw = e; }
    ok(threw instanceof RangeError && /more than 1000 child sitemaps/.test(threw.message),
        "1001 children refuses with the named RangeError (got " + threw + ")");
    ok(fetches === 1, "the child-cap refusal still precedes any child fetch (got " + fetches + ")");
});

await __kitS.section("<urlset><url><loc/></url><url><loc>https://e.test/a</loc></url><url><loc", async () => {
    const r = Sitemap.parse("<urlset><url><loc/></url><url><loc>https://e.test/a</loc></url><url><loc /></url><url><loc>https://e.test/b</loc></url></urlset>");
    ok(JSON.stringify(r) === '["https://e.test/a","https://e.test/b"]',
        "an empty <loc/> yields no URL and does not swallow the next entry (sitemaps.org: loc must be a URL beginning with the protocol) (got " + JSON.stringify(r) + ")");
    let threw = null;
    try { Sitemap.parse("<urlset><loc/></urlset>"); } catch (e) { threw = e; }
    ok(threw === null, "a lone empty <loc/> is dropped, not an error (got " + threw + ")");
});
__kitS.end();

if (fails === 0) print("test_scrape_sitemap_bounds: all " + n + " checks passed");
else {
    print("test_scrape_sitemap_bounds: " + fails + " FAILED of " + n);
    throw new Error("test_scrape_sitemap_bounds: " + fails + " failures");
}
