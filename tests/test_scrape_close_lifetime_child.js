// flags: --std
/* test_scrape_close_lifetime_child.js -- the exit SHAPE driver for
 * test_scrape_close_lifetime.js (one shape per invocation, argv[1]).
 *
 * Every shape leaves a crawl or fetcher in an UGLY state -- closed mid
 * flight, dropped while pages are still being fetched, resumed from a closed
 * instance while its late completions are still landing -- and then ends
 * normally. The judged outcome is this process's exit code: the class this
 * pins used to end in a nondeterministic teardown SIGSEGV or a
 * JS_FreeRuntime assertion (exit 134), or feed a phantom page into a
 * DIFFERENT crawl that had merely outlived its sibling.
 *
 * Run: dynajs (CONFIG_NATIVE_MODULES=y)
 *      tests/test_scrape_close_lifetime_child.js <shape> */
import { Crawl, Fetcher, Extractor } from "dyna:scrape";
import { Selector, HTMLText, HTMLParse } from "dyna:html";

const shape = scriptArgs[1];
const BASE = "http://example.invalid";
const GRAPH = { "/a": ["/b", "/c"], "/b": ["/d"], "/c": ["/d"], "/d": [] };
const sleep = (ms) => new Promise((res) => setTimeout(res, ms));

const body = (p) => "<html><body><h1>t" + p + "</h1>" +
    (GRAPH[p] || []).map((q) => `<a href="${BASE + q}">q</a>`).join("") +
    "</body></html>";

const mockClient = (delayMs) => ({
    requestAsync(method, url) {
        return new Promise((res) => setTimeout(() => {
            res({ status: 200, url, headers: {}, body: body(url.slice(BASE.length)) });
        }, delayMs));
    },
});

const mkFetcher = (delayMs) => new Fetcher({
    agent: "close-lifetime/1", client: mockClient(delayMs), minDelayMs: 0,
    robots: false, allowPrivateHosts: true,
});
const ex = () => new Extractor({
    title: { sel: new Selector("h1") },
    links: { sel: new Selector("a"), attr: "href", all: true },
}, { text: HTMLText });
const mkCrawl = (f, conc) => new Crawl(f, { maxPages: 4, concurrency: conc })
    .start(BASE + "/a", ex(), HTMLParse);

const drainAsync = async (c) => {
    const out = [];
    for await (const p of c) out.push(p.url);
    return out;
};

switch (shape) {
case "close-mid-flight": {
    const f = mkFetcher(40);
    const c = mkCrawl(f, 2);
    const kept = c.next();          /* a flight is in the air */
    kept.catch(() => {});
    c.close();
    f.close();
    await sleep(250);               /* the late completions land here */
    print("closed-mid-flight");
    break;
}
case "two-crawls": {
    const f1 = mkFetcher(40), f2 = mkFetcher(5);
    const c1 = mkCrawl(f1, 2), c2 = mkCrawl(f2, 2);
    const d2 = drainAsync(c2);
    c1.close();                      /* early phase: nothing pulled yet */
    f1.close();
    const urls = await d2;           /* c2 must be untouched by c1's close */
    c2.close();
    f2.close();
    if (urls.length !== 4) throw new Error("c2 saw " + urls.length + " pages");
    await sleep(250);
    print("two-crawls");
    break;
}
case "gc-drop": {
    {
        const f = mkFetcher(30);
        const c = mkCrawl(f, 2);
        c.next().catch(() => {});
        /* both dropped on scope exit, with a flight in the air */
    }
    if (globalThis.gc) globalThis.gc();
    await sleep(250);
    print("gc-drop");
    break;
}
case "unclosed-in-flight": {
    const f = mkFetcher(30);
    const c = mkCrawl(f, 2);
    c.next().catch(() => {});
    await sleep(250);               /* the flight COMPLETES, unclosed */
    print("unclosed-in-flight");
    break;
}
case "resume-after-close": {
    const f1 = mkFetcher(40);
    const c1 = new Crawl(f1, { maxPages: 4, concurrency: 2 })
        .start(BASE + "/a", ex(), HTMLParse);
    const first = await c1.next();
    const state = c1.serialize();
    c1.close();                      /* mid-flight, late settles pending */
    f1.close();
    const f2 = mkFetcher(5);
    const c2 = Crawl.resume(f2, state, ex(), HTMLParse);
    const urls = await drainAsync(c2);
    f2.close();
    if (urls.some((u) => u === first.value.url))
        throw new Error("the resumed crawl re-emitted a closed instance's page");
    await sleep(250);
    print("resume-after-close");
    break;
}
case "close-during-getter": {
    /* close() from INSIDE a response getter (user JS mid-flight): the
       flight's tail must not write into the dead instance (or anyone
       else's); the crawl ENDS instead of throwing "closed native resource"
       into unrelated code */
    let c, f2;
    const client = {
        requestAsync(method, url) {
            return new Promise((res) => setTimeout(() => {
                res({
                    get status() { c.close(); f2.close(); return 200; },
                    url, headers: {}, body: body(url.slice(BASE.length)),
                });
            }, 5));
        },
    };
    f2 = new Fetcher({
        agent: "close-lifetime/1", client, minDelayMs: 0,
        robots: false, allowPrivateHosts: true,
    });
    c = new Crawl(f2, { maxPages: 4, concurrency: 2 })
        .start(BASE + "/a", ex(), HTMLParse);
    const urls = await drainAsync(c);
    await sleep(150);
    if (urls.length > 1)
        throw new Error("pages after close-in-getter: " + urls.length);
    print("close-during-getter");
    break;
}
case "close-during-request": {
    /* close() from INSIDE the client's requestAsync itself: the exchange
       never even lands on a live crawl */
    let c, f2;
    const client = {
        requestAsync(method, url) {
            c.close();
            f2.close();
            return new Promise((res) => setTimeout(() => {
                res({ status: 200, url, headers: {}, body: body(url.slice(BASE.length)) });
            }, 20));
        },
    };
    f2 = new Fetcher({
        agent: "close-lifetime/1", client, minDelayMs: 0,
        robots: false, allowPrivateHosts: true,
    });
    c = new Crawl(f2, { maxPages: 4, concurrency: 2 })
        .start(BASE + "/a", ex(), HTMLParse);
    const urls = await drainAsync(c);
    await sleep(150);
    if (urls.length > 0)
        throw new Error("pages after close-in-request: " + urls.length);
    print("close-during-request");
    break;
}
default:
    throw new Error("unknown shape " + shape);
}
