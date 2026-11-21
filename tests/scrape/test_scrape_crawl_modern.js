// covers: dyna:scrape.Crawl
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_scrape_crawl_modern");
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

await __kitS.section("robots response status getter throw propagates", async () => {
    {
        const urls = [];
        const client = {
            request(method, url) {
                urls.push(String(url));
                if (String(url).endsWith("/robots.txt"))
                    return { get status() { throw new Error("boom-status"); },
                             headers: {}, body: "x" };
                return { status: 200, headers: {}, body: "page" };
            }
        };
        const f = new Fetcher({ agent: "x12-bot/1.0", client, minDelayMs: 0 });
        let msg = "";
        try { await f.get("http://seed.test/a"); } catch (e) { msg = e.message; }
        eq(msg, "boom-status", "robots response status getter throw propagates");
        eq(urls.length, 1, "no page request once the fetcher's own getter threw");
    }
    {
        let requests = 0;
        const client = {
            request() { requests++; return { get status() { throw new Error("boom-status"); },
                                             headers: {}, body: "x" }; }
        };
        const f = new Fetcher({ agent: "x12-bot/1.0", client, robots: false, minDelayMs: 0 });
        let msg = "";
        try { await f.get("http://seed.test/a"); } catch (e) { msg = e.message; }
        eq(msg, "boom-status", "page response status getter throw propagates");
        eq(requests, 1, "no retry loop after the status getter threw");
    }
    {
        const client = { request(m, u) { return { status: 200, headers: {}, body: "fine" }; } };
        const f = new Fetcher({ agent: "x12-bot/1.0", client, minDelayMs: 0 });
        eq((await f.get("http://seed.test/a")).status, 200, "CONTROL: good robots+page still fetches");
    }
    {
        let extracted = 0;
        const fetcher = { get() { return { get status() { throw new Error("boom-status"); },
                                           headers: {}, body: "hello" }; } };
        const cr = new Crawl(fetcher);
        cr.start("http://seed.test/", { run() { extracted++; return { links: [] }; } }, (s) => s);
        let msg = "";
        try { await cr.next(); } catch (e) { msg = e.message; }
        eq(msg, "boom-status", "Crawl: response status getter throw propagates");
        eq(extracted, 0, "Crawl: the extractor never runs after the getter threw");
        cr.close();
    }
    {
        const fetcher = { get() { return { status: 200, headers: {}, body: "hi",
                                           get robotsDirectives() { throw new Error("boom-rd"); } }; } };
        const cr = new Crawl(fetcher);
        cr.start("http://seed.test/", { run() { return { links: [] }; } }, (s) => s);
        let msg = "";
        try { await cr.next(); } catch (e) { msg = e.message; }
        eq(msg, "boom-rd", "Crawl: robotsDirectives getter throw propagates");
        cr.close();
    }
    {
        const fetcher = { get() { return { status: 200, headers: {}, body: "hi" }; } };
        const cr = new Crawl(fetcher);
        cr.start("http://seed.test/", { run() { return { links: [] }; } }, (s) => s);
        const it = await cr.next();
        ok(it.done === false && it.value && it.value.status === 200,
           "CONTROL: Crawl still emits good pages");
        cr.close();
    }
    {
        const client = { request() { return { status: 200, headers: {}, body: "" }; } };
        let msg = "";
        try { new Fetcher({ agent: "x12-bot/1.0", client,
                            retries: { valueOf() { throw new Error("boom-retries"); } } }); }
        catch (e) { msg = e.message; }
        eq(msg, "boom-retries", "Fetcher ctor retries getter throw propagates");
        msg = "";
        try { new Fetcher({ agent: "x12-bot/1.0", client,
                            minDelayMs: { valueOf() { throw new Error("boom-delay"); } } }); }
        catch (e) { msg = e.message; }
        eq(msg, "boom-delay", "Fetcher ctor minDelayMs getter throw propagates");
        msg = "";
        try { new Crawl({ get() { return { status: 200, headers: {}, body: "" }; } },
                         { maxPages: { valueOf() { throw new Error("boom-maxpages"); } } }); }
        catch (e) { msg = e.message; }
        eq(msg, "boom-maxpages", "Crawl ctor maxPages getter throw propagates");
        ok(new Fetcher({ agent: "x12-bot/1.0", client, retries: 2 }) instanceof Fetcher,
           "CONTROL: numeric Fetcher options still build");
        ok(new Crawl({ get() { return { status: 200, headers: {}, body: "" }; } },
                     { maxPages: 5 }) instanceof Crawl,
           "CONTROL: numeric Crawl options still build");
    }
});
__kitS.end();

print("test_scrape_modern: " + pass + " passed, " + fail + " failed");
if (fail) throw new Error("test_scrape_modern: " + fail + " failures");
