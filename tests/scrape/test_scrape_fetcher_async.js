// covers: integration dyna:scrape.Fetcher dyna:scrape.Crawl dyna:scrape.Sitemap -- the asynchronous Fetcher
// 2026-11-26 owner decision, a clean break: the hidden synchronous HTTPClient hook
// (Symbol.for("dyna.http.requestSync")) is gone and Fetcher runs its policy (robots, pacing, retries,
// redirect gates) as an async engine over HTTPClient's non-blocking transport. get()/getStream()/
// Sitemap.list() return Promises and Crawl.next() always does, so a Fetcher can crawl an App served by
// the same process. Pacing is reserved up front (concurrent gets to one host each claim the next
// minDelayMs slot), robots.txt is fetched once per host however many requests wait on it, closing the
// Fetcher mid-flight rejects, getStream's own client refuses a private resolved peer, and a crawl does
// not fetch ahead of next() at concurrency 1 (serialize() after next() keeps the frontier whole).
// Mutants: claim without reservation (pacing case red), robots single-flight map dropped (robots-once
// case red), the getStream onConnect guard not installed (guard case red), the crawl dispatch cap
// removed (serialize case red).
import { suite } from "../lib/kit.js";
import { App } from "dyna:http";
import { Fetcher, Crawl, Extractor, Sitemap } from "dyna:scrape";
import { HTMLParse, Selector } from "dyna:html";

const t = suite("scrape_fetcher_async");

function site() {
    const hits = {}, stamps = [];
    const app = new App({ port: 0 });
    const hit = (p) => { hits[p] = (hits[p] || 0) + 1; stamps.push({ p, at: Date.now() }); };
    app.get("/robots.txt", () => { hit("/robots.txt"); return "User-agent: *\nDisallow: /no\n"; });
    app.get("/a", () => { hit("/a"); return "<html><a href='/b'>b</a><a href='/c'>c</a></html>"; });
    app.get("/b", () => { hit("/b"); return "<html><a href='/c'>c</a></html>"; });
    app.get("/c", () => { hit("/c"); return "<html>c</html>"; });
    app.get("/p", () => { hit("/p"); return "p"; });
    app.get("/slow", async () => { hit("/slow"); await new Promise((r) => setTimeout(r, 300)); return "slow"; });
    app.get("/sm.xml", () => "<urlset><url><loc>http://x.test/1</loc></url><url><loc>http://x.test/2</loc></url></urlset>");
    app.start();
    return { app, hits, stamps, base: "http://127.0.0.1:" + app.port };
}
const ex = () => new Extractor({ links: { sel: new Selector("a"), attr: "href", all: true } });

t.test("get, getStream and Sitemap.list return Promises and fetch from an App in the same process", async (a) => {
    const s = site();
    const f = new Fetcher({ agent: "async/1", allowPrivateHosts: true, minDelayMs: 0 });
    const p = f.get(s.base + "/p");
    a.ok(p instanceof Promise, "get() returns a Promise");
    const r = await p;
    a.eq(r.status, 200);
    a.eq(r.body, "p");
    const sp = f.getStream(s.base + "/p");
    a.ok(sp instanceof Promise, "getStream() returns a Promise");
    const st = await sp;
    const buf = new Uint8Array(16);
    a.eq(await st.read(buf), 1);
    st.close();
    const sk = await f.getStream(s.base + "/no");
    a.eq(sk.skippedByRobots, true, "a robots-refused stream resolves as skipped");
    a.eq(await sk.read(buf), 0);
    const lp = Sitemap.list(s.base + "/sm.xml", f);
    a.ok(lp instanceof Promise, "Sitemap.list() returns a Promise");
    a.eq((await lp).join(","), "http://x.test/1,http://x.test/2");
    a.throws(() => f.get("ftp://x.test/"), TypeError, "a non-http(s) url throws synchronously");
    a.throws(() => new Fetcher({ agent: "async/1" }).get("http://127.0.0.1/"), TypeError, "a private host throws synchronously");
    f.close();
    a.throws(() => f.get(s.base + "/p"), TypeError, "a closed Fetcher throws synchronously");
    s.app.close();
});

t.test("concurrent gets to one host each claim the next minDelayMs slot", async (a) => {
    const s = site();
    const f = new Fetcher({ agent: "async/1", allowPrivateHosts: true, minDelayMs: 120, robots: false });
    await Promise.all([f.get(s.base + "/p"), f.get(s.base + "/p"), f.get(s.base + "/p")]);
    const at = s.stamps.filter((x) => x.p === "/p").map((x) => x.at);
    a.eq(at.length, 3);
    a.ok(at[1] - at[0] >= 100 && at[2] - at[1] >= 100, "requests spaced by the floor: " + (at[1] - at[0]) + ", " + (at[2] - at[1]));
    a.ok(f.stats().throttledMs >= 200, "the reserved waits are counted: " + f.stats().throttledMs);
    f.close(); s.app.close();
}, { timeoutMs: 20000 });

t.test("robots.txt is fetched once per host however many gets wait on it", async (a) => {
    const s = site();
    const f = new Fetcher({ agent: "async/1", allowPrivateHosts: true, minDelayMs: 0 });
    const rs = await Promise.all(Array.from({ length: 5 }, () => f.get(s.base + "/p")));
    a.eq(rs.filter((r) => r.status === 200).length, 5);
    a.eq(s.hits["/robots.txt"], 1, "one robots.txt fetch");
    f.close(); s.app.close();
});

t.test("closing the Fetcher mid-flight rejects the pending get", async (a) => {
    const s = site();
    const f = new Fetcher({ agent: "async/1", allowPrivateHosts: true, minDelayMs: 0, robots: false });
    const p = f.get(s.base + "/slow");
    await new Promise((r) => setTimeout(r, 50));
    f.close();
    let e = null;
    try { await p; } catch (x) { e = x; }
    a.ok(e instanceof TypeError && /closed while get\(\) was in flight/.test(e.message), "rejected: " + e);
    s.app.close();
});

t.test("getStream's own client refuses a private resolved peer unless allowPrivateHosts", async (a) => {
    const f = new Fetcher({ agent: "async/1", minDelayMs: 0, robots: false, retries: 0 });
    let e = null;
    try { await f.getStream("http://no-such-host.invalid/x"); } catch (x) { e = x; }
    a.ok(e instanceof RangeError, "an unresolvable host rejects: " + e);
    const sc = f._sclient;
    a.eq(typeof sc.onConnect, "function", "the stream client carries the peer guard");
    a.eq(sc.onConnect("127.0.0.1"), false, "loopback peer refused");
    a.eq(sc.onConnect("10.1.2.3"), false, "RFC1918 peer refused");
    a.eq(sc.onConnect("::1"), false, "IPv6 loopback refused");
    a.eq(sc.onConnect("93.184.216.34"), true, "a public peer passes");
    f.close();
    const g = new Fetcher({ agent: "async/1", minDelayMs: 0, robots: false, retries: 0, allowPrivateHosts: true });
    try { await g.getStream("http://no-such-host.invalid/x"); } catch (x) { }
    a.eq(g._sclient.onConnect, undefined, "allowPrivateHosts leaves the peer unguarded");
    g.close();
});

t.test("Crawl.next is always a Promise and does not fetch ahead at concurrency 1", async (a) => {
    const s = site();
    const f = new Fetcher({ agent: "async/1", allowPrivateHosts: true, minDelayMs: 0 });
    const c = new Crawl(f, { maxPages: 3, linkField: "links" }).start(s.base + "/a", ex(), HTMLParse);
    a.eq(typeof c[Symbol.iterator], "undefined", "no sync iterator");
    a.eq(typeof c[Symbol.asyncIterator], "function");
    const p = c.next();
    a.ok(p instanceof Promise, "next() returns a Promise at concurrency 1");
    a.eq((await p).value.url, s.base + "/a");
    await new Promise((r) => setTimeout(r, 100));
    a.eq(s.hits["/b"] || 0, 0, "nothing fetched ahead of the next next()");
    const state = JSON.parse(c.serialize());
    const rest = (await Array.fromAsync(Crawl.resume(f, JSON.stringify(state), ex(), HTMLParse))).map((x) => x.url.replace(s.base, ""));
    a.eq(rest.join(","), "/b,/c", "the resumed crawl keeps the whole frontier");
    c.close(); f.close(); s.app.close();
});

await t.run();
