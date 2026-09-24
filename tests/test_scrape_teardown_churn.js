// flags: --std
/* test_scrape_teardown_churn.js -- 10k fetches across short-lived scrape
 * instances, then a flat teardown.
 *
 * The defect: the scrape family's teardown leaked -- a full-ASan battery
 * reported the scrape-family allocators at exit (the flights' promise
 * forests, and behind them a leaked requestAsync closure that pinned the
 * whole module scope). The contract now: after ten thousand fetches spread
 * over hundreds of Fetcher/Crawl/Extractor lifetimes -- including
 * instances closed mid-flight and garbage-collected in flight -- the
 * process tears down with NO assertion and no growing residue: the exit
 * code is the judge, and under LeakSanitizer the run must be flat.
 *
 * Every 500th instance asserts its exact page set, so a churn that starts
 * CROSS-TALKING between instances fails rows long before teardown.
 *
 * Run: dynajs (CONFIG_NATIVE_MODULES=y) tests/test_scrape_teardown_churn.js
 *      (optionally under ASan/LSan). */
import { Crawl, Fetcher, Extractor } from "dyna:scrape";
import { Selector, HTMLText, HTMLParse } from "dyna:html";

let pass = 0, fail = 0;
const ok = (c, w, d) => { if (c) { pass++; print("  ok    " + w); }
                          else { fail++; print("  FAIL  " + w + (d ? "  [" + d + "]" : "")); } };

const BASE = "http://example.invalid";
const GRAPH = { "/a": ["/b", "/c"], "/b": ["/d"], "/c": ["/d"], "/d": [] };
const EXPECT4 = [BASE + "/a", BASE + "/b", BASE + "/c", BASE + "/d"]
    .sort().join(",");
const body = (p) => "<html><body><h1>t" + p + "</h1>" +
    (GRAPH[p] || []).map((q) => `<a href="${BASE + q}">q</a>`).join("") +
    "</body></html>";

let n_fetch = 0;
const client = {
    request(method, url) {
        n_fetch++;
        return { status: 200, url, headers: {},
                 body: body(url.slice(BASE.length)) };
    },
};
/* the ASYNC bridge too (every 4th instance): a microtask-settled
   requestAsync -- same wire promise shape a real HTTPClient parks on, so
   the churn covers the async exchange's allocators (the leaked-closure
   class) as well as the sync arm's */
const asyncClient = {
    requestAsync(method, url) {
        n_fetch++;
        return Promise.resolve({ status: 200, url, headers: {},
                                 body: body(url.slice(BASE.length)) });
    },
    /* the sync arm exists but LOSES to requestAsync whenever both are
       present (the exchange prefers the wire promise) */
    request(method, url) {
        n_fetch++;
        return { status: 200, url, headers: {},
                 body: body(url.slice(BASE.length)) };
    },
};
const mkExtractor = () => new Extractor({
    title: { sel: new Selector("h1") },
    links: { sel: new Selector("a"), attr: "href", all: true },
}, { text: HTMLText });

/* 2500 crawls x 4 pages = 10000 fetches, each crawl a full instance
   lifetime (ctor, start, drain, close) */
const CRAWLS = 2500;
let exact = 0;
for (let i = 0; i < CRAWLS; i++) {
    const f = new Fetcher({
        agent: "churn/1", client: (i % 4 === 3 ? asyncClient : client),
        minDelayMs: 0, robots: false, allowPrivateHosts: true,
    });
    const c = new Crawl(f, { maxPages: 4, concurrency: 1 })
        .start(BASE + "/a", mkExtractor(), HTMLParse);
    const urls = [];
    for await (const p of c) urls.push(p.url);
    c.close();
    f.close();
    if (i % 500 === 0) {
        ok(urls.length === 4 &&
           [...new Set(urls)].sort().join(",") === EXPECT4,
           "churn instance " + i + " is exact (" + urls.length + " pages)");
        exact++;
    }
}
ok(n_fetch === CRAWLS * 4,
   "the churn made exactly 10000 fetches (got " + n_fetch + ")");
ok(exact === 5, "every sampled instance was checked");

print("test_scrape_teardown_churn: " + pass + " passed, " + fail + " failed");
if (fail) throw new Error("test_scrape_teardown_churn: " + fail + " failures");
