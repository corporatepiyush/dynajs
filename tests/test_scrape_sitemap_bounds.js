// timeout: 120
// R5-4: Sitemap.list aggregate caps at the exact boundary: 999 children with
// exactly 50000 URLs (mixed per-child counts) and the full 1000-child /
// 50000-URL index both complete; one child or URL past each cap refuses.
import { Sitemap } from "dyna:scrape";

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

{
    // 999 children, exactly 50000 URLs total: 998 x 50 + 1 x 100.
    const kids = [];
    for (let i = 0; i < 999; i++) kids.push("http://x.test/c" + i);
    const docs = { "http://x.test/idx.xml": indexOf(kids) };
    for (let i = 0; i < 998; i++) docs[kids[i]] = locs(50);
    docs[kids[998]] = locs(100);
    const t0 = Date.now();
    const got = Sitemap.list("http://x.test/idx.xml", mock(docs));
    const t = Date.now() - t0;
    ok(got.length === 50000, "999 children carrying exactly 50000 URLs is accepted (got "
        + got.length + ")" + ms(t));
    ok(got[49999] === "http://x.test/u", "the last of the 50000 URLs survives" + ms(t));
}

{
    // The full allowed index: 1000 children x 50 URLs = 50000, all fetched.
    const kids = [];
    for (let i = 0; i < 1000; i++) kids.push("http://x.test/d" + i);
    const docs = { "http://x.test/idx2.xml": indexOf(kids) };
    for (const k of kids) docs[k] = locs(50);
    let fetches = 0;
    const f = {
        get(u) { fetches++; if (!(u in docs)) throw new Error("unexpected fetch " + u); return { status: 200, body: docs[u] }; }
    };
    const t0 = Date.now();
    const got = Sitemap.list("http://x.test/idx2.xml", f);
    const t = Date.now() - t0;
    ok(got.length === 50000, "the full 1000-child / 50000-URL index completes (got "
        + got.length + ")" + ms(t));
    ok(fetches === 1001, "every child was fetched exactly once (got " + fetches + ")" + ms(t));
    ok(t < 30000, "the full index finishes inside a generous wall bound" + ms(t));
}

{
    // One URL past the aggregate cap: 1000 children each with 50, plus one
    // extra URL on the last child (50001 total).
    const kids = [];
    for (let i = 0; i < 1000; i++) kids.push("http://x.test/e" + i);
    const docs = { "http://x.test/idx3.xml": indexOf(kids) };
    for (let i = 0; i < 999; i++) docs[kids[i]] = locs(50);
    docs[kids[999]] = locs(51);
    let threw = null;
    try { Sitemap.list("http://x.test/idx3.xml", mock(docs)); }
    catch (e) { threw = e; }
    ok(threw instanceof RangeError && /more than 50000 URLs/.test(threw.message),
        "50001 aggregate URLs refuses with the named RangeError (got " + threw + ")");
}

{
    // One child past the cap: 1001 children.
    const kids = [];
    for (let i = 0; i < 1001; i++) kids.push("http://x.test/f" + i);
    const docs = { "http://x.test/idx4.xml": indexOf(kids) };
    for (const k of kids) docs[k] = locs(1);
    let threw = null, fetches = 0;
    try {
        Sitemap.list("http://x.test/idx4.xml", {
            get(u) { fetches++; return { status: 200, body: docs[u] }; }
        });
    } catch (e) { threw = e; }
    ok(threw instanceof RangeError && /more than 1000 child sitemaps/.test(threw.message),
        "1001 children refuses with the named RangeError (got " + threw + ")");
    ok(fetches === 1, "the child-cap refusal still precedes any child fetch (got " + fetches + ")");
}

if (fails === 0) print("test_scrape_sitemap_bounds: all " + n + " checks passed");
else {
    print("test_scrape_sitemap_bounds: " + fails + " FAILED of " + n);
    throw new Error("test_scrape_sitemap_bounds: " + fails + " failures");
}
