// Split from tests/test_cov_sys_time_random_uuid_misc.js: the scrape sections.
import { Robots } from "dyna:scrape";

let n = 0, fails = 0;
function assert(c, m) { n++; if (!c) { fails++; print("FAIL: " + m); } }
function eq(a, b, m) { assert(a === b, m + " got " + JSON.stringify(a) + " want " + JSON.stringify(b)); }
function throws(fn, re, m) {
    n++;
    try { fn(); fails++; print("FAIL: " + m + " did not throw"); }
    catch (e) { if (re && !re.test(String(e.message))) { fails++; print("FAIL: " + m + " wrong msg: " + e.message); } }
}
function assertClose(a, b, tol, m) {
    n++;
    if (!(Math.abs(a - b) <= tol)) { fails++; print("FAIL: " + m + " |" + a + "-" + b + "|>" + tol); }
}
print("=== sys ===");
print("=== scrape ===");
{
    const rb = new Robots("# c\nUser-agent: *\nDisallow: /private/\nAllow: /private/pub\nCrawl-delay: 2\nSitemap: https://x.test/s.xml\n");
    eq(rb.allows("/open"), true, "robots open allowed");
    eq(rb.allows("/private/x"), false, "robots disallowed path");
    eq(rb.allows("/private/pub/y"), true, "robots Allow beats Disallow (longest match)");
    eq(JSON.stringify(rb.sitemaps()), '["https://x.test/s.xml"]', "sitemap captured");
    eq(rb.crawlDelay(), 2, "crawlDelay numeric");
    assert(rb.ruleCount >= 1 || rb.ruleCount === 0, "ruleCount numeric");
    const rb2 = new Robots("User-agent: good\nDisallow:\nUser-agent: *\nDisallow: /\n");
    eq(rb2.allows("/x"), false, "star blocks default agent");
    const rbGood = new Robots("User-agent: good\nDisallow:\nUser-agent: *\nDisallow: /\n", { agent: "good" });
    eq(rbGood.allows("/x"), true, "specific agent unrestricted (opts.agent)");
    const rbEmpty = new Robots("");
    eq(rbEmpty.allows("/"), true, "empty robots allow all");
    const rbJunk = new Robots("\x00\x01garbage lines without fields\n");
    assert(typeof rbJunk.allows("/x") === "boolean", "junk robots still answers");
}
if (fails) {
    print("test_scrape_cov_sys_time_random_uuid_misc: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_scrape_cov_sys_time_random_uuid_misc failed");
}
print("test_scrape_cov_sys_time_random_uuid_misc: " + n + " assertions, 0 failures");
