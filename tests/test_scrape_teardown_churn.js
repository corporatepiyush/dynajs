// flags: --std
import "./httpc.js";
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
const asyncClient = {
    requestAsync(method, url) {
        n_fetch++;
        return Promise.resolve({ status: 200, url, headers: {},
                                 body: body(url.slice(BASE.length)) });
    },
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
