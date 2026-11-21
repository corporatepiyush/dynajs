// covers: dyna:scrape.Robots
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_scrape_robots_modern");
import { Robots, Extractor, Fetcher, Crawl } from "dyna:scrape";
import { Selector, HTMLText, HTMLParse } from "dyna:html";

let pass = 0, fail = 0;
const ok = (c, w, d) => { if (c) { pass++; print("  ok    " + w); }
                          else { fail++; print("  FAIL  " + w + (d ? "  [" + d + "]" : "")); } };
const eq = (a, b, w) => ok(a === b, w, "got [" + String(a) + "] want [" + String(b) + "]");
async function throws(fn, w) {
    let t = false, m = "";
    try { await fn(); } catch (e) { t = true; m = String(e.message || e); }
    ok(t, w, t ? "" : "did NOT throw");
    return m;
}
const pathOf = (u) => {
    const s = u.indexOf("://");
    if (s < 0) return u;
    const slash = u.indexOf("/", s + 3);
    return slash < 0 ? "/" : u.slice(slash);
};

await __kitS.section("User-agent: * Disallow: /", async () => {
    const g = new Robots(
        "User-agent: *\nDisallow: /\n" +
        "\nUser-agent: politebot\nAllow: /\nDisallow: /secret\n",
        { agent: "politebot/2.3 (+https://ops.test/bot)" });
    ok(g.allows("/a"), "token match: politebot/2.3 hits User-agent: politebot");
    ok(!g.allows("/secret/x"), "and its Disallow binds");

    const e = new Robots(
        "User-agent: bot\nDisallow: /a\n" +
        "\nUser-agent: bot/9\nDisallow: /b\n",
        { agent: "bot/9" });
    ok(!e.allows("/b"), "exact UA match selects its own group");
    ok(e.allows("/a"), "and not the token-lookalike group");

    const m = new Robots(
        "User-agent: alpha\nUser-agent: beta\nDisallow: /x\n",
        { agent: "beta/1.0" });
    ok(!m.allows("/x"), "multi-UA record applies to any listed member");

    const l = new Robots(
        "User-agent: bo\nDisallow: /long\n" +
        "\nUser-agent: bot\nDisallow: /short\n",
        { agent: "bot/1" });
    ok(!l.allows("/short"), "longest matching group name wins");
    ok(l.allows("/long"), "the shorter neighbour group stays inert");

    const d = new Robots(
        "User-agent: *\nCrawl-delay: 9\n" +
        "\nUser-agent: quick\nCrawl-delay: 1\n",
        { agent: "quick/1" });
    eq(d.crawlDelay(), 1, "delay of the winning group, not the wildcard's");

    let big = "User-agent: last\nDisallow: /hidden\n\n";
    for (let i = 0; i < 120; i++)
        big += "User-agent: *\nDisallow: /g" + i + "\n\n";
    const b = new Robots(big, { agent: "last/1.0" });
    ok(!b.allows("/hidden/x"), "record storm parses; the leading named group wins");
    ok(b.allows("/elsewhere"), "its Allow-silence leaves other paths open");
});

await __kitS.section("User-agent: *", async () => {
    const g = new Robots(
        "User-agent: *\n" +
        "Disallow: /a%2Ab\n" +
        "Disallow: /x%24y\n" +
        "Disallow: /z%24\n" +
        "Disallow: /q%2Fw\n",
        { agent: "t/1" });
    ok(!g.allows("/a%2Ab"), "%2A rule matches its own encoded form");
    ok(!g.allows("/a*b"), "%2A rule matches the literal * octet (RFC table)");
    ok(g.allows("/aXXb"), "%2A is NOT a wildcard");
    ok(!g.allows("/x%24y"), "%24 literal mid-path");
    ok(!g.allows("/x$y"), "%24 rule matches the literal $ octet (RFC table)");
    ok(!g.allows("/z%24"), "trailing %24 literal: exact form is matched");
    ok(!g.allows("/z%24x"), "and prefix semantics still apply to the literal");
    ok(g.allows("/z"), "trailing %24 is NOT an end-anchor");
    ok(!g.allows("/z$"),
       "the raw $ octet is normalized and meets the %24 rule (RFC table)");
    ok(!g.allows("/q%2Fw"), "%2F stays encoded: separator never matches");
    ok(g.allows("/q/w"), "a real slash does not meet the %2F rule");
    const w = new Robots("User-agent: *\nDisallow: /p*q\n", { agent: "t/1" });
    ok(!w.allows("/pXXq"), "unencoded * still wildcards");
    ok(!w.allows("/pq"), "* matches the empty run");
    const u = new Robots("User-agent: *\nDisallow: /a%41\n", { agent: "t/1" });
    ok(!u.allows("/aA"), "rule %41 is the unreserved A (RFC 9309 2.2.2): it meets the raw A");
    ok(!u.allows("/a%41") && !u.allows("/a%41x"), "and the encoded form of it");
    ok(u.allows("/aB"), "CONTROL: another letter is not matched");
});

await __kitS.section("noindex, nofollow", async () => {
    const cl = { request(m, u) {
        if (u.endsWith("/robots.txt"))
            return { status: 404, headers: {}, body: "" };
        if (u.endsWith("/plain"))
            return { status: 200, headers: { "X-Robots-Tag": "noindex, nofollow" }, body: "x" };
        if (u.endsWith("/scoped"))
            return { status: 200, headers: { "X-Robots-Tag":
                "googlebot: noindex, mybot: nofollow" }, body: "x" };
        if (u.endsWith("/star"))
            return { status: 200, headers: { "X-Robots-Tag": "*: noindex" }, body: "x" };
        if (u.endsWith("/multi"))
            return { status: 200, headers: { "x-robots-tag": "none",
                                             "X-Robots-Tag": "max-snippet:-1" }, body: "x" };
        if (u.endsWith("/canon"))
            return { status: 200, headers: { Link: '<https://site.test/real>; rel="canonical"' }, body: "x" };
        if (u.endsWith("/canonrel"))
            return { status: 200, headers: { Link: '<https://other.test/x>; rel="alternate", <https://site.test/real2>; rel="canonical"' }, body: "x" };
        return { status: 200, headers: {}, body: "x" };
    } };
    const f = new Fetcher({ agent: "mybot/1.0", client: cl, minDelayMs: 0 });
    const r1 = await f.get("http://site.test/plain");
    ok(JSON.stringify(r1.robotsDirectives) === JSON.stringify(["noindex","nofollow"]),
       "X-Robots-Tag: comma-separated rules parsed");
    const r2 = await f.get("http://site.test/scoped");
    ok(JSON.stringify(r2.robotsDirectives) === JSON.stringify(["nofollow"]),
       "bot-scoped: only OUR token's rule is honored");
    const r3 = await f.get("http://site.test/star");
    ok(JSON.stringify(r3.robotsDirectives) === JSON.stringify(["noindex"]),
       "'*' scope applies to every agent");
    const r4 = await f.get("http://site.test/multi");
    ok(JSON.stringify(r4.robotsDirectives) ===
         JSON.stringify(["noindex","nofollow","max-snippet:-1"]),
       "none expands; arg-directives are not treated as a scope; multi-header joins");
    const r5 = await f.get("http://site.test/canon");
    ok(r5.canonicalUrl === "https://site.test/real",
       "Link: <url>; rel=canonical exposed, resolved");
    const r6 = await f.get("http://site.test/canonrel");
    ok(r6.canonicalUrl === "https://site.test/real2",
       "non-canonical Link relations are skipped");
    f.close();
});
__kitS.end();

print("test_scrape_modern: " + pass + " passed, " + fail + " failed");
if (fail) throw new Error("test_scrape_modern: " + fail + " failures");
