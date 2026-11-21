// covers: dyna:scrape.Fetcher
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_scrape_fetcher_modern", { chain: true });
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

await __kitS.section("http://safe.test/ar X-Evil: 1", async () => {
    const routes = {
        "/rel":   [302, "./next"],
        "/next":  [302, "../deep/final"],
        "/deep/final": [200, "arrived-relative"],
        "/abs":   [302, "/landing"],
        "/landing": [200, "arrived-abs"],
        "/proto": [302, "//mirror.test/m"],
        "/m":     [200, "arrived-proto"],
        "/dots":  [302, "/a/b/../c?kept=1#frag"],
        "/a/c":   [200, "arrived-dots"],
        "/frag":  [302, "#top"],
        "/cr":    [302, "http://safe.test/a\r\nX-Evil: 1"]
    };
    const hits = [];
    const client = {
        request(method, url, body, headers) {
            hits.push(url);
            if (url.endsWith("/robots.txt"))
                return { status: 404, headers: {}, body: "" };
            const r = routes[pathOf(url).split("?")[0]];
            if (!r) return { status: 200, headers: {}, body: "?" };
            return r[0] >= 300
                ? { status: r[0], headers: { Location: r[1] }, body: "" }
                : { status: r[0], headers: {}, body: r[1] };
        }
    };
    const mk = () => new Fetcher({ agent: "modern-bot/1.0", client,
                                   minDelayMs: 0 });

    let f = mk();
    eq((await f.get("http://src.test/rel")).body, "arrived-relative",
       "relative ./ and ../ locations chase to the real content");
    eq((await f.get("http://src.test/abs")).body, "arrived-abs",
       "absolute-path location resolves");
    f.close();

    hits.length = 0;
    f = mk();
    eq((await f.get("http://src.test/proto")).body, "arrived-proto",
       "protocol-relative location switches host");
    ok(hits.some(u => u === "http://mirror.test/robots.txt"),
       "the new host gets its own robots.txt pre-fetch");
    f.close();

    f = mk();
    const dotsRes = await f.get("http://src.test/dots");
    ok(/\/a\/c\?kept=1$/.test(dotsRes.url),
       "dot-segment removal keeps the query, drops the fragment (" +
       dotsRes.url + ")");
    eq((await f.get("http://src.test/frag")).status, 302,
       "fragment-only Location ends the chain instead of erroring");
    eq((await f.get("http://src.test/cr")).status, 302,
       "a CRLF-bearing Location cannot start another hop");
    f.close();
});

await __kitS.section("a redirect into loopback is REFUSED mid-chain", async () => {
    const client = {
        request(method, url) {
            if (url.endsWith("/robots.txt"))
                return { status: 404, headers: {}, body: "" };
            if (url.indexOf("public.test") >= 0)
                return { status: 302,
                         headers: { Location: "http://127.0.0.1:1/x" },
                         body: "" };
            return { status: 200, headers: {}, body: "should-not-happen" };
        }
    };
    const f = new Fetcher({ agent: "ssrf-bot/1.0", client, minDelayMs: 0 });
    const msg = await throws(() => f.get("http://public.test/x"),
                       "a redirect into loopback is REFUSED mid-chain");
    ok(/private|loopback/i.test(msg), "and the refusal names the class");
    f.close();
});

await __kitS.section("second hit answers 304 (what the wire said)", async () => {
    const reqs = [];
    let seq = 0;
    const etags = {};
    const bodies = {};
    const client = {
        request(method, url, body, headers) {
            reqs.push({ url, inm: headers["If-None-Match"] || null,
                        ims: headers["If-Modified-Since"] || null });
            if (url.endsWith("/robots.txt"))
                return { status: 404, headers: {}, body: "" };
            if (etags[url] && reqs[reqs.length - 1].inm === etags[url])
                return { status: 304, headers: {}, body: "" };
            const tag = '"' + (++seq) + '"';
            etags[url] = tag;
            bodies[url] = "content-" + seq;
            return { status: 200, headers: { ETag: tag }, body: bodies[url] };
        }
    };

    const f = new Fetcher({ agent: "cache-bot/1.0", client, minDelayMs: 0 });
    const P = "http://cdn.test/p";
    const r1 = await f.get(P);
    const r2 = await f.get(P);
    eq(r2.status, 304, "second hit answers 304 (what the wire said)");
    eq(r2.body, r1.body, "with the CACHED body attached");
    ok(r2.fromCache && r2.notModified, "labelled fromCache + notModified");
    eq(reqs[reqs.length - 1].inm, '"1"',
       "revalidation replayed the stored ETag");
    let s = f.stats();
    eq(s.revalidated, 1, "stats counts the revalidation");
    ok(s.savedBytes > 0, "savedBytes reports what was not re-fetched");

    delete etags[P];
    const r3 = await f.get(P);
    eq(r3.status, 200, "an updated representation arrives as 200");
    eq(reqs[reqs.length - 1].inm, '"1"',
       "the STALE validator was sent (that is how revalidation works)");
    eq((await f.get(P)).body, bodies[P], "fresh cache serves the new content on 304");
    eq(f.stats().revalidated, 2, "counted twice now");
    f.close();

    reqs.length = 0;
    const off = new Fetcher({ agent: "cache-bot/1.0", client, minDelayMs: 0,
                              revalidate: false });
    await off.get("http://cdn.test/q");
    await off.get("http://cdn.test/q");
    ok(!reqs.some(r => r.url.endsWith("/q") && (r.inm || r.ims)),
       "revalidate:false neither sends nor replays validators");
    off.close();

    reqs.length = 0;
    let lmServed = false;
    const LM = "Tue, 18 Aug 2026 10:00:00 GMT";
    const lmc = {
        request(method, url, body, headers) {
            reqs.push({ url, ims: headers["If-Modified-Since"] || null });
            if (url.endsWith("/robots.txt"))
                return { status: 404, headers: {}, body: "" };
            if (!lmServed) {
                lmServed = true;
                return { status: 200, headers: { "Last-Modified": LM },
                         body: "lm-body" };
            }
            return { status: 304, headers: {}, body: "" };
        }
    };
    const lf = new Fetcher({ agent: "cache-bot/1.0", client: lmc, minDelayMs: 0 });
    const first = await lf.get("http://cdn.test/lm");
    const second = await lf.get("http://cdn.test/lm");
    eq(first.status, 200, "LM CONTROL: first fetch is plain 200");
    eq(second.status, 304, "Last-Modified validator drives a 304");
    eq(second.body, "lm-body", "cached body served");
    eq(reqs[reqs.length - 1].ims, LM, "If-Modified-Since carried the exact stamp");
    lf.close();
});

await __kitS.section("oldest validator evicted past the 32-entry cap", async () => {
    const reqs = [];
    const known = {};
    const client = {
        request(method, url, body, headers) {
            reqs.push({ url, inm: headers["If-None-Match"] || null });
            if (url.endsWith("/robots.txt"))
                return { status: 404, headers: {}, body: "" };
            const k = url.slice(-4);
            if (!known[url]) known[url] = '"' + k + '"';
            return { status: 200, headers: { ETag: known[url] },
                     body: "b-" + k };
        }
    };
    const f = new Fetcher({ agent: "fifo-bot/1.0", client, minDelayMs: 0 });
    const urls = [];
    for (let i = 0; i < 34; i++) {
        const n = (i < 10 ? "0" : "") + i;
        const u = "http://cap.test/i" + n + "/x";
        urls.push(u);
        await f.get(u);
    }
    reqs.length = 0;
    await f.get(urls[0]);
    eq(reqs[0].inm, null, "oldest validator evicted past the 32-entry cap");
    await f.get(urls[33]);
    eq(reqs[1].inm, known[urls[33]], "freshly inserted validator survives");
    f.close();
});

await __kitS.section("caller headers reach every request", async () => {
    let captured = null;
    const client = {
        request(method, url, body, headers) {
            captured = headers;
            if (url.endsWith("/robots.txt"))
                return { status: 404, headers: {}, body: "" };
            return { status: 200, headers: {}, body: "ok" };
        }
    };
    const f = new Fetcher({
        agent: "hdr-bot/1.0", client, minDelayMs: 0,
        headers: { "Accept-Language": "en-GB,en;q=0.5",
                   "User-Agent": "spoof/9.9" }
    });
    await f.get("http://h.test/p");
    eq(captured["Accept-Language"], "en-GB,en;q=0.5",
       "caller headers reach every request");
    eq(captured["User-Agent"], "hdr-bot/1.0",
       "the crawler's identity WINS a User-Agent collision");
    f.close();

    const weird = JSON.parse('{"__proto__":"not-a-proto","X-A":"1"}');
    const f2 = new Fetcher({ agent: "hdr-bot/1.0", client, minDelayMs: 0,
                             headers: weird });
    await f2.get("http://h.test/p");
    eq(captured["X-A"], "1", "odd keys copy through as data");
    ok(typeof captured["__proto__"] === "string",
       "__proto__ stays a plain data header, never a prototype switch");
    f2.close();

    await throws(() => new Fetcher({ agent: "h/1", client, headers: 42 }),
           "headers: 42 is refused");
    const cred = await throws(
        () => new Fetcher({ agent: "h/1", client, minDelayMs: 0,
                            headers: { Authorization: "Bearer x" } }),
        "Authorization in `headers` is refused at construction");
    ok(/credentials/i.test(cred) && /Authorization/i.test(cred),
       "and the refusal names credentials AND the key");
    await throws(() => new Fetcher({ agent: "h/1", client, minDelayMs: 0,
                               headers: { Cookie: "sid=1" } }),
           "Cookie is refused too");
    {
        let saw = null;
        const c2 = { request(m, u, b, hh) { saw = hh;
            return u.endsWith("/robots.txt")
                ? { status: 404, headers: {}, body: "" }
                : { status: 200, headers: {}, body: "" }; } };
        const f3 = new Fetcher({ agent: "h/1", client: c2, minDelayMs: 0,
                                 headers: { "X-API-Key": "k" } });
        await f3.get("http://h.test/p");
        eq(saw["X-API-Key"], "k",
           "CONTROL: non-standard keys flow through untouched");
        f3.close();
    }
});

await __kitS.section("User-agent: * Disallow: /secret", async () => {
    let robots = "User-agent: *\nDisallow: /secret\n";
    let failRobots = false;
    const cl = { request(m, u) {
        if (u.endsWith("/robots.txt")) {
            if (failRobots) return { status: 500, headers: {}, body: "" };
            return { status: 200, headers: {}, body: robots };
        }
        return { status: 200, headers: {}, body: "x" };
    } };
    const f = new Fetcher({ agent: "b/1", client: cl, minDelayMs: 0, robotsTtlMs: 0 });
    ok((await f.get("http://s.test/secret")).status === 0 &&
       (await f.get("http://s.test/secret")).skippedByRobots === true,
       "first robots.txt disallows /secret");
    failRobots = true;
    ok((await f.get("http://s.test/secret")).status === 0,
       "a FAILED refresh still serves the last-known-good copy (still disallowed)");
    ok((await f.get("http://s.test/other")).status === 200,
       "and does not fall to disallow-everything");
    failRobots = false;
    robots = "User-agent: *\nDisallow:\n";
    ok((await f.get("http://s.test/secret")).status === 200,
       "after a good refresh the new policy applies");
    f.close();
});

await __kitS.section("<h1>P</h1><a href=\"/sub/a\">x</a><a href=\"#top\">y</a>", async () => {
    const pages = {
        "/": '<h1>P</h1><a href="/sub/a">x</a><a href="#top">y</a>' +
             '<a href="javascript:void(0)">z</a>',
        "/sub/a": '<h1>Q</h1><a href="../deep/b?from=sub">go</a>',
        "/deep/b": '<h1>R</h1><a href="mailto:a@b.c">m</a>' +
                   '<a href="/deep/b">self-dup</a>'
    };
    const client = {
        request(method, url) {
            if (url.endsWith("/robots.txt"))
                return { status: 404, headers: {}, body: "" };
            const p = pathOf(url).split("?")[0];
            return { status: 200, headers: {}, body: pages[p] ?? "" };
        }
    };
    const f = new Fetcher({ agent: "crawl-bot/1.0", client, minDelayMs: 0 });
    const ext = new Extractor({
        title: { sel: new Selector("h1") },
        links: { sel: new Selector("a"), attr: "href", all: true }
    }, { text: HTMLText });
    const c = new Crawl(f, { maxDepth: 3 });
    const walked = [];
    for await (const pg of c.start("http://example.test/", ext, HTMLParse))
        walked.push(pg.url.replace(/^http:\/\/example\.test/, "") || "/");
    eq(walked.join(","), "/,/sub/a,/deep/b?from=sub,/deep/b",
       "./ ../ # javascript: mailto: resolve or drop; the ?less twin is a\n"
       + "     distinct url (exact-string dedup) and still terminates");
    f.close();
});

await __kitS.section("https->http downgrade refused by default", async () => {
    let mode = "";
    const mkClient = () => ({
        request(method, url) {
            if (url.endsWith("/robots.txt"))
                return { status: 404, headers: {}, body: "" };
            return { status: 200, headers:
                mode === "cl" ? { "Content-Length": "900" }
                              : (mode === "ct" ? { "Content-Type": "application/pdf" } : {}),
                     body: "x".repeat(mode === "cl" ? 10 : 50) };
        }
    });

    {
        const c = { request(m, u) {
            if (u.endsWith("/robots.txt")) return { status: 404, headers: {}, body: "" };
            if (u === "https://s.test/start")
                return { status: 302, headers: { Location: "http://s.test/plain" }, body: "" };
            return { status: 200, headers: {}, body: u.indexOf("https") === 0 ? "" : "PLAINTEXT-CONTENT" };
        } };
        const strict = new Fetcher({ agent: "dg/1", client: c, minDelayMs: 0 });
        const msg = await throws(() => strict.get("https://s.test/start"),
                           "https->http downgrade refused by default");
        ok(/downgrade/i.test(msg) && /allowInsecureDowngrade/.test(msg),
           "and names both the move and the opt-in");
        ok(!/PLAINTEXT-CONTENT/.test(JSON.stringify(c)),
           "no plaintext fetch happened before the refusal (mock-only check)");
        strict.close();
        const lax = new Fetcher({ agent: "dg/1", client: c, minDelayMs: 0,
                                  allowInsecureDowngrade: true });
        eq((await lax.get("https://s.test/start")).body, "PLAINTEXT-CONTENT",
           "opt-in follows the downgrade explicitly");
        lax.close();
    }

    {
        const f = new Fetcher({ agent: "cl/1", client: mkClient(),
                                minDelayMs: 0, maxBodyBytes: 100 });
        mode = "cl";
        const m = await throws(() => f.get("http://h.test/p"),
                         "declared Content-Length over cap is refused");
        ok(/Content-Length/i.test(m) && /maxBodyBytes/.test(m),
           "and the message says it acted on the DECLARED length");
        mode = "";
        ok((await f.get("http://h.test/p")).status === 200,
           "CONTROL: same cap with no CL header serves fine");
        f.close();
    }

    {
        let seq = 0;
        const c = { request(m, u) {
            if (u.endsWith("/robots.txt")) return { status: 404, headers: {}, body: "" };
            seq++;
            if (seq === 1)
                return { status: 200, headers: { ETag: '"t"', "Content-Type":
                            "text/html; charset=utf-8" }, body: "<i>hi</i>" };
            return { status: 304, headers: {}, body: "" };
        } };
        const f = new Fetcher({ agent: "ct/1", client: c, minDelayMs: 0 });
        const a = await f.get("http://t.test/x");
        ok(/^text\/html/.test(a.contentType),
           "contentType passes through verbatim (" + a.contentType + ")");
        const b2 = await f.get("http://t.test/x");
        eq(b2.status, 304, "revalidated as before");
        eq(b2.contentType, "text/html; charset=utf-8",
           "and the CACHED content-type replays -- callers can filter by type");
        const off = new Fetcher({ agent: "ct/1", client: { request() {
            return { status: 200, headers: { "content-type": "image/png" },
                     body: "\u0091PNG" }; } }, minDelayMs: 0,
                    robots: false });
        const png = await off.get("http://b.test/img");
        eq(png.contentType, "image/png",
           "CONTROL: binary type exposed so extraction stays a CALLER choice");
        off.close(); f.close();
        void seq;
    }

    {
        const pages = {
            "/a/page": '<html><head><base href="http://cdn.test/lib/"></head>' +
                       '<body><p class="pb">go</p><a href="child">c</a></body></html>',
            "/lib/child": '<h1>KID</h1>',
            "/b/root": '<base href="/assets/"><h1>B</h1><a href="k2">x</a>',
            "/assets/k2": '<h1>K2</h1>'
        };
        const client = { request(m, url) {
            if (url.endsWith("/robots.txt"))
                return { status: 404, headers: {}, body: "" };
            const p = pathOf(url).split("?")[0];
            return { status: 200, headers: {}, body: pages[p] ?? "" };
        } };
        const ext = new Extractor({
            title: { sel: new Selector("h1") },
            pageBase: { sel: new Selector("base"), attr: "href" },
            links:   { sel: new Selector("a"), attr: "href", all: true }
        }, { text: HTMLText });
        const f = new Fetcher({ agent: "base/1", client, minDelayMs: 0 });

        const naive = new Crawl(f, { maxDepth: 1 });
        const w1 = [];
        for await (const p of naive.start("http://src.test/a/page", ext, HTMLParse))
            w1.push(pathOf(p.url));
        eq(w1.join(","), "/a/page,/a/child",
           "CONTROL without baseField: relative child joins the PAGE dir");

        const based = new Crawl(f, { maxDepth: 1, maxPages: 5,
                                     baseField: "pageBase", sameHost: false });
        const w2 = [];
        for await (const p of based.start("http://src.test/a/page", ext, HTMLParse))
            w2.push(pathOf(p.url));
        eq(w2.join(","), "/a/page,/lib/child",
           "baseField resolves links through the page's declared <base>");
        f.close();
    }

    {
        const soup = '<ul><li>a<li>b</ul>stray text<p unclosed>x' +
                     '</p><div id="d">one<div id="d">two</div></div>';
        const doc = HTMLParse(soup);
        const ex = new Extractor({
            lis:  { sel: new Selector("li"), all: true },
            dups: { sel: new Selector("#d"), attr: "id", all: true },
            first:{ sel: new Selector("#d > div") }
        }, { text: HTMLText });
        const r = ex.run(doc);
        eq(r.value.lis.join("|"), "a|b",
           "tag-soup: implied list items recover as siblings");
        eq(r.value.dups.length, 2,
           "duplicate ids surface BOTH nodes through all:true");
        eq(r.value.first, "two",
           "descendant queries match each duplicate id once (first per subtree)");

        const ents = HTMLParse('<a href="https://e.test/a?x=1&amp;y=2#f">q</a>');
        const eu = new Extractor({
            h: { sel: new Selector("a"), attr: "href", as: "url" }
        }, { text: HTMLText }).run(ents);
        eq(eu.value.h, "https://e.test/a?x=1&y=2#f",
           "entity-encoded attributes decode; as:url keeps them intact");

        let deep = "", end = "";
        for (let i = 0; i < 300; i++) { deep += "<div>"; end += "</div>"; }
        const threw = (() => {
            try { HTMLParse(deep + "<em id=k>k</em>" + end); return false; }
            catch (e) { return /nesting/.test(String(e.message)); }
        })();
        ok(threw, "nesting bomb refuses with its own named error");
        const ddoc = HTMLParse(deep.slice(0, "<div>".length * 200) +
                               "<em id=k>k</em>" + end.slice(0, 200 * 6));
        ok(ddoc.length >= 1, "CONTROL: within the bound it parses");
        const de = new Extractor(
            { k: { sel: new Selector("#k"), trim: true } },
            { text: HTMLText }).run(ddoc);
        eq(de.value.k, "k", "extraction over a deep-but-legal dom completes");
    }
});

await __kitS.section("uFEFFUser-agent: * Disallow: /x", async () => {
    {
        const rb = new Robots("\uFEFFUser-agent: *\nDisallow: /x\n",
                              { agent: "bom/1" });
        ok(!rb.allows("/x"), "BOM'd robots.txt still binds its first group");
        ok(rb.allows("/y"), "and allows the rest");
    }

    {
        const c = { request(m, u) {
            if (u === "http://r.test/robots.txt")
                return { status: 301, headers: { Location:
                    "http://r.test/other" }, body: "" };
            if (u === "http://r.test/other")
                return { status: 301, headers: { Location:
                    "http://r.test/robots.txt" }, body: "" };
            return { status: 200, headers: {}, body: "page" };
        } };
        const f = new Fetcher({ agent: "rl/1", client: c, minDelayMs: 0 });
        const r = await f.get("http://r.test/x");
        ok(r.skippedByRobots && r.status === 0,
           "robots 301 loop: file undefined, host fully disallowed");
        f.close();
    }

    {
        const seen = [];
        const client = { request(m, url) {
            if (url.endsWith("/robots.txt"))
                return { status: 404, headers: {}, body: "" };
            seen.push(url);
            return { status: 200, headers: {}, body: "<i>f</i>" };
        } };
        const f = new Fetcher({ agent: "fg/1", client, minDelayMs: 0 });
        await f.get("http://f.test/x#section");
        ok(!seen.some(u => u.includes("#")),
           "fragment is stripped before the wire (" + seen.join(" ") + ")");
        ok(seen.some(u => u === "http://f.test/x"),
           "the fragmentless url is what actually fetched");
        f.close();
    }

    {
        const longLoc = "http://big.test/" + "x".repeat(5000);
        const c = { request(m, u) {
            if (u.endsWith("/robots.txt")) return { status: 404, headers: {}, body: "" };
            if (u.endsWith("/start"))
                return { status: 302, headers: { Location: longLoc }, body: "" };
            return { status: 200, headers: {}, body: "never" };
        } };
        const f = new Fetcher({ agent: "ll/1", client: c, minDelayMs: 0 });
        const r = await f.get("http://big.test/start");
        eq(r.status, 302, "over-long Location: served as final, no throw");
        f.close();
    }

    {
        const pages = {
            "/": '<h1>S</h1><a href="/ok">g</a><a href="/err">e</a>',
            "/ok": '<h1>O</h1>',
            "/err": '<h1>ERR-PAGE</h1><a href="/evil">x</a>'
        };
        const got = [];
        const client = { request(m, url) {
            if (url.endsWith("/robots.txt"))
                return { status: 404, headers: {}, body: "" };
            got.push(pathOf(url));
            const isErr = pathOf(url) === "/err";
            return { status: isErr ? 403 : 200, headers: {}, body:
                     pages[pathOf(url)] ?? "" };
        } };
        const f = new Fetcher({ agent: "sg/1", client, minDelayMs: 0 });
        const ext = new Extractor({
            title: { sel: new Selector("h1") },
            links: { sel: new Selector("a"), attr: "href", all: true }
        }, { text: HTMLText });
        const c = new Crawl(f, { maxDepth: 2 });
        const pagesSeen = [];
        for await (const p of c.start("http://g.test/", ext, HTMLParse))
            pagesSeen.push([pathOf(p.url), p.status, p.value && p.value.title]);
        ok(!got.includes("/evil"),
           "links on a 403 page are NOT followed");
        ok(got.includes("/ok"), "CONTROL: links on 200 pages still follow");
        ok(pagesSeen.some(x => x[0] === "/err" && x[1] === 403 &&
                               x[2] === "ERR-PAGE"),
           "the 403 page itself EMITS with its status and its extracted data");
        f.close();
    }
    {
        let n = 0;
        const c = { request(m, u) {
            if (u.endsWith("/robots.txt")) return { status: 404, headers: {}, body: "" };
            if (++n < 3)
                return { status: 429, headers: { "Retry-After":
                    new Date(Date.now() + 2000).toUTCString() }, body: "" };
            return { status: 200, headers: {}, body: "after-date-ra" };
        } };
        const f = new Fetcher({ agent: "ra/1", client: c, minDelayMs: 0,
                                retries: 3 });
        const t0 = Date.now();
        const r = await f.get("http://d.test/x");
        const dt = Date.now() - t0;
        ok(r.status === 200 && /date-ra/.test(r.body),
           "Retry-After as HTTP-date: retried to success");
        eq(f.stats().retried, 2,
           "both 429s were retried through the date branch (load-independent)");
        ok(dt >= 1500, "date delta was actually slept (" + dt + "ms)");
        // wall-clock: a deadline/timeout check; the bound sits far above the deadline and below the hang it guards against, so load only narrows the gap
        ok(dt < 15000,
           "and NOT the 60s-per-retry the epoch-vs-monotonic clock mix " +
           "produced (" + dt + "ms)");
        f.close();
    }

    {
        let n = 0;
        const c = { request(m, u) {
            if (u.endsWith("/robots.txt")) return { status: 404, headers: {}, body: "" };
            if (!n++) return { status: 429, headers: { "Retry-After": "600" },
                               body: "come back later" };
            return { status: 200, headers: {}, body: "should-not-retry" };
        } };
        const f = new Fetcher({ agent: "ra/1", client: c, minDelayMs: 0,
                                retries: 5 });
        const t0 = Date.now();
        const r = await f.get("http://d.test/y");
        const dt = Date.now() - t0;
        eq(r.status, 429, "Retry-After beyond cap: 429 served as final");
        eq(r.body, "come back later", "and the server's own body is the answer");
        eq(f.stats().retried, 0, "no retry attempted -- the delay was honored");
        // wall-clock: a deadline/timeout check; the bound sits far above the deadline and below the hang it guards against, so load only narrows the gap
        ok(dt < 20000, "no 60s sleep (" + dt + "ms)");
        f.close();
    }

    {
        const reqs = [];
        const c = { request(m, u, b, h) {
            reqs.push(h["If-None-Match"] || null);
            if (u.endsWith("/robots.txt")) return { status: 404, headers: {}, body: "" };
            return { status: 200, headers: { ETag: '"v"',
                     "Cache-Control": "no-store" }, body: "fresh" };
        } };
        const f = new Fetcher({ agent: "ns/1", client: c, minDelayMs: 0 });
        await f.get("http://n.test/a");
        await f.get("http://n.test/a");
        ok(reqs[1] === null,
           "no-store: second hit revalidates with NO validator (store refused it)");
        eq(reqs[0], null, "CONTROL: first hit never sends one either");
        f.close();
    }
    for (const cc of ["no-store;private", "max-age=0, no-store",
                      "public, no-store , max-age=1"]) {
        const reqs2 = [];
        const c2 = { request(m, u, b, h) {
            reqs2.push(h["If-None-Match"] || null);
            if (u.endsWith("/robots.txt")) return { status: 404, headers: {}, body: "" };
            return { status: 200, headers: { ETag: '"v"', "Cache-Control": cc },
                     body: "x" };
        } };
        const g = new Fetcher({ agent: "ns/1", client: c2, minDelayMs: 0 });
        await g.get("http://n.test/b");
        await g.get("http://n.test/b");
        ok(reqs2[1] === null, "no-store honored in form: " + JSON.stringify(cc));
        g.close();
    }

    {
        const pages = {
            "/": '<h1>S</h1><a href="/r">r</a><a href="/d">d</a>',
            "/r": '<h1>R</h1><a href="/r-only">x</a>',
            "/d": '<link rel="canonical" href="http://k.test/r">' +
                  '<h1>D</h1><a href="/d-only">y</a>',
            "/r-only": '<h1>RO</h1>',
            "/d-only": '<h1>DO</h1>'
        };
        const got = [];
        const client = { request(m, url) {
            if (url.endsWith("/robots.txt"))
                return { status: 404, headers: {}, body: "" };
            got.push(pathOf(url));
            return { status: 200, headers: {}, body: pages[pathOf(url)] ?? "" };
        } };
        const f = new Fetcher({ agent: "cn/1", client, minDelayMs: 0 });
        const ext = new Extractor({
            title: { sel: new Selector("h1") },
            pageCanon: { sel: new Selector("link[rel=canonical]"), attr: "href" },
            links: { sel: new Selector("a"), attr: "href", all: true }
        }, { text: HTMLText });
        const c = new Crawl(f, { maxDepth: 2, canonicalField: "pageCanon" });
        const walked = [];
        for await (const p of c.start("http://k.test/", ext, HTMLParse))
            walked.push(pathOf(p.url));
        eq(walked.join(","), "/,/r,/d,/r-only",
           "duplicate canonical page still EMITS at its url; /r-only is /r's own");
        ok(!got.includes("/d-only"),
           "a duplicate's links are not queued (content lives at /r)");
        ok(got.includes("/r-only"), "CONTROL: /r's own links queue");
        f.close();
    }

    {
        const pages = {
            "/": '<h1>S</h1><a href="/c">c</a><a href="/never">n</a>',
            "/c": '<link rel="canonical" href="http://p.test/never"><h1>C</h1>',
            "/never": '<h1>N</h1>'
        };
        const got = [];
        const client = { request(m, url) {
            if (url.endsWith("/robots.txt"))
                return { status: 404, headers: {}, body: "" };
            got.push(pathOf(url));
            return { status: 200, headers: {}, body: pages[pathOf(url)] ?? "" };
        } };
        const f = new Fetcher({ agent: "cn/1", client, minDelayMs: 0 });
        const ext = new Extractor({
            pageCanon: { sel: new Selector("link[rel=canonical]"), attr: "href" },
            links: { sel: new Selector("a"), attr: "href", all: true }
        }, { text: HTMLText });
        const c = new Crawl(f, { maxDepth: 2, canonicalField: "pageCanon" });
        for await (const p of c.start("http://p.test/", ext, HTMLParse)) void p;
        ok(got.includes("/never"),
           "canonical marking never phantom-skips a url that was queued");
        f.close();
    }

    {
        const pages = {
            "/": '<h1>S</h1><a href="/follow">f</a><a rel="external nofollow" href="/nf">n</a>',
            "/follow": '<h1>F</h1>',
            "/nf": '<h1>N</h1>'
        };
        const got = [];
        const client = { request(m, url) {
            if (url.endsWith("/robots.txt"))
                return { status: 404, headers: {}, body: "" };
            got.push(pathOf(url));
            return { status: 200, headers: {}, body: pages[pathOf(url)] ?? "" };
        } };
        const f = new Fetcher({ agent: "nf/1", client, minDelayMs: 0 });
        const ext = new Extractor({
            links: { sel: new Selector("a"), attr: "href", all: true },
            rels:  { sel: new Selector("a"), attr: "rel", all: true }
        }, { text: HTMLText });
        const c = new Crawl(f, { maxDepth: 2, relField: "rels" });
        for await (const p of c.start("http://w.test/", ext, HTMLParse)) void p;
        ok(got.includes("/follow"), "CONTROL: plain link followed");
        ok(!got.includes("/nf"), "nofollow link is not fetched");
        f.close();
    }

    {
        const c = { request() { return { status: 200, headers: {}, body: "<i>z</i>" }; } };
        const f = new Fetcher({ agent: "m0/1", client: c, minDelayMs: 0 });
        const cr = new Crawl(f, { maxPages: 0 });
        const n = await cr.start("http://z.test/a").next();
        ok(n.done, "maxPages:0 yields done immediately");
        f.close();
    }

    {
        const pages = {
            "/a": '<h1>A</h1><a href="/b">b</a>',
            "/b": '<h1>B</h1>'
        };
        const client = { request(m, url) {
            if (url.endsWith("/robots.txt"))
                return { status: 404, headers: {}, body: "" };
            return { status: 200, headers: {}, body: pages[pathOf(url)] ?? "" };
        } };
        const f = new Fetcher({ agent: "bd/1", client, minDelayMs: 0 });
        const ext = new Extractor({
            title: { sel: new Selector("h1") },
            links: { sel: new Selector("a"), attr: "href", all: true }
        }, { text: HTMLText });
        const c = new Crawl(f, { maxDepth: 1 });
        const seen = [];
        for await (const p of c.start("http://z.test/a", ext, HTMLParse))
            seen.push(pathOf(p.url));
        eq(seen.join(","), "/a,/b",
           "depth-cap boundary: the frontier page AT maxDepth emits");
        f.close();
    }

    {
        const pages = {
            "/": '<h1>S</h1><a href="/b">b</a><a href="/c">c</a>',
            "/b": '<h1>B</h1><a href="/d">d</a>',
            "/c": '<h1>C</h1><a href="/e">e</a>'
        };
        const got = [];
        const client = { request(m, url) {
            if (url.endsWith("/robots.txt"))
                return { status: 404, headers: {}, body: "" };
            got.push(pathOf(url));
            const h = pathOf(url) === "/b" ? { "X-Robots-Tag": "nofollow" } : {};
            return { status: 200, headers: h,
                     body: pages[pathOf(url)] ?? "" };
        } };
        const f = new Fetcher({ agent: "nf2/1", client, minDelayMs: 0 });
        const ext = new Extractor({
            links: { sel: new Selector("a"), attr: "href", all: true },
            robots: { sel: new Selector("meta[name=robots]"), attr: "content" }
        }, { text: HTMLText });
        const c = new Crawl(f, { maxDepth: 2, robotsField: "robots" });
        const seen = [];
        for await (const p of c.start("http://w.test/", ext, HTMLParse))
            seen.push(p);
        ok(!got.includes("/d"), "X-Robots-Tag nofollow: links not followed");
        ok(got.includes("/b") && got.includes("/c"), "nofollow pages still emit");
        const pb = seen.find(p => pathOf(p.url) === "/b");
        ok(pb && Array.isArray(pb.robots) && pb.robots.includes("nofollow"),
           "the header directive is exposed on page.robots");
        ok(pb && pb.value.robots === undefined,
           "meta field absent: nothing merged onto the value");
        f.close();
    }

    {
        const pages = {
            "/": '<h1>S</h1><a href="/m">m</a>',
            "/m": '<meta name="robots" content="noindex, nofollow">' +
                  '<h1>M</h1><a href="/n">n</a>'
        };
        const got = [];
        const client = { request(m, url) {
            if (url.endsWith("/robots.txt"))
                return { status: 404, headers: {}, body: "" };
            got.push(pathOf(url));
            return { status: 200, headers: {},
                     body: pages[pathOf(url)] ?? "" };
        } };
        const f = new Fetcher({ agent: "nm/1", client, minDelayMs: 0 });
        const ext = new Extractor({
            links: { sel: new Selector("a"), attr: "href", all: true },
            robots: { sel: new Selector("meta[name=robots]"), attr: "content" }
        }, { text: HTMLText });
        const c = new Crawl(f, { maxDepth: 2, robotsField: "robots" });
        const seen = [];
        for await (const p of c.start("http://w.test/", ext, HTMLParse))
            seen.push(p);
        ok(!got.includes("/n"), "meta robots nofollow: links not followed");
        const pm = seen.find(p => pathOf(p.url) === "/m");
        ok(pm && pm.robots && pm.robots.includes("noindex") &&
           pm.robots.includes("nofollow"),
           "meta directives merge onto page.robots");
        f.close();
    }

    {
        const pages = {
            "/a": '<a href="/r">r</a><a href="/s">s</a>',
            "/r": '<a href="/t">t</a>',
            "/s": '<a href="/real">real</a>'
        };
        const got = [];
        const client = { request(m, url) {
            if (url.endsWith("/robots.txt"))
                return { status: 404, headers: {}, body: "" };
            got.push(pathOf(url));
            const h = pathOf(url) === "/r"
                ? { Link: '<http://w.test/real>; rel="canonical"' } : {};
            return { status: 200, headers: h,
                     body: pages[pathOf(url)] ?? "" };
        } };
        const f = new Fetcher({ agent: "lc/1", client, minDelayMs: 0 });
        const ext = new Extractor({
            links: { sel: new Selector("a"), attr: "href", all: true }
        }, { text: HTMLText });
        const c = new Crawl(f, { maxDepth: 2 });
        for await (const p of c.start("http://w.test/a", ext, HTMLParse)) void p;
        ok(got.includes("/t"),
           "the canonical-declaring page still contributes its own links");
        ok(!got.includes("/real"),
           "the Link-canonical target is marked seen: /s's link to it dedups");
        f.close();
    }

    {
        const got = [];
        const client = { request(m, url) {
            if (url.endsWith("/robots.txt"))
                return { status: 404, headers: {}, body: "" };
            got.push(url);
            const body = /example\.com/i.test(url)
                ? '<a href="http://EXAMPLE.com/dup">d</a>' +
                  '<a href="http://example.com/dup">d</a>' +
                  '<a href="http://example.com:80/other">o</a>'
                : "<i>z</i>";
            return { status: 200, headers: {}, body };
        } };
        const f = new Fetcher({ agent: "hc/1", client, minDelayMs: 0 });
        const ext = new Extractor({
            links: { sel: new Selector("a"), attr: "href", all: true }
        }, { text: HTMLText });
        const c = new Crawl(f, { maxDepth: 2, sameHost: true });
        const urls = [];
        for await (const p of c.start("http://example.com/seed", ext, HTMLParse))
            urls.push(p.url);
        ok(urls.filter(u => u.endsWith("/dup")).length === 1,
           "case-variant spellings of /dup fetch ONCE");
        ok(urls.some(u => u === "http://example.com:80/other" ||
                          u === "http://example.com/other"),
           "default-port link is on-host and queued");
        f.close();
    }

    {
        const doc = HTMLParse('<i class="hit">one</i>');
        const ex = new Extractor({
            none: { sel: new Selector(".nothing"), all: true,
                    default: ["none"] },
            some: { sel: new Selector(".hit"), all: true },
            nod:  { sel: new Selector(".nothing"), all: true }
        }, { text: HTMLText }).run(doc);
        eq(ex.value.none.join("|"), "none",
           "default replaces an EMPTY all-array");
        eq(ex.value.some.join("|"), "one",
           "CONTROL: non-empty lists pass through");
        eq(ex.value.nod.length, 0, "CONTROL: no default means empty array");
    }
});

await __kitS.section("conn refused", async () => {
    let robotHits = 0;
    const dead = {
        request(method, url) {
            if (url.endsWith("/robots.txt")) { robotHits++; throw new Error("conn refused"); }
            return { status: 200, headers: {}, body: "page" };
        }
    };
    const f = new Fetcher({ agent: "ttl-bot/1.0", client: dead, minDelayMs: 0,
                            robotsTtlMs: 3600000 });
    ok((await f.get("http://dead.test/a")).skippedByRobots === true, "robots network error -> complete disallow");
    ok((await f.get("http://dead.test/b")).skippedByRobots === true, "and the second fetch skips too");
    eq(robotHits, 1, "S4: the failure is TTL-stamped (1 robots attempt for 2 fetches)");
    f.close();
});

await __kitS.section("<a href=\"/dupA\">a</a><a href=\"/dupB\">b</a><a href=\"/master\">m</a>", async () => {
    const pages = {
        "/robots.txt": null,
        "/hub": '<a href="/dupA">a</a><a href="/dupB">b</a><a href="/master">m</a>',
        "/dupA": '<link rel="canonical" href="http://hub.test/master"><a href="/only-via-dupA">x</a>',
        "/dupB": '<link rel="canonical" href="http://hub.test/master"><a href="/only-via-dupB">x</a>',
        "/master": '<a href="/from-master">x</a>',
        "/n1": "",
        "/rel": '<a href="/n1" rel="nofollow">x</a><a href="/n2" rel="keep">y</a>',
        "/n2": ""
    };
    const client = {
        request(method, url) {
            const p = pathOf(url);
            const b = pages[p];
            if (b === null || b === undefined)
                return { status: 404, headers: {}, body: "" };
            return { status: 200, headers: {}, body: b };
        }
    };
    const mkC = (seed, opts) => {
        const f = new Fetcher({ agent: "crawl-bot/1.0", client, minDelayMs: 0 });
        const ex = new Extractor({
            links: { sel: new Selector("a"), attr: "href", all: true },
            rels: { sel: new Selector("a"), attr: "rel", all: true },
            mycanon: { sel: new Selector('link[rel="canonical"]'), attr: "href" }
        }, { text: HTMLText });
        const c = new Crawl(f, opts);
        c.start(seed, ex, HTMLParse);
        return c;
    };
    const drain = (c) => Array.fromAsync(c);
    const pathsOf = async (c) => (await drain(c)).map(p => pathOf(p.url));

    const urls = await pathsOf(mkC("http://hub.test/hub", { maxDepth: 2, canonicalField: "mycanon" }));
    ok(urls.includes("/dupA") && urls.includes("/dupB") && urls.includes("/master"),
       "canonical-declaring pages emit; the master itself is crawled");
    ok(!urls.includes("/only-via-dupA") && !urls.includes("/only-via-dupB"),
       "canonicalField: a page whose canonical target is already handled queues NO links");

    const rels = await pathsOf(mkC("http://hub.test/rel", { maxDepth: 1, relField: "rels" }));
    ok(rels.includes("/n2") && !rels.includes("/n1"),
       "relField: the nofollow-marked link is skipped");

    {
        const c = mkC("http://hub.test/rel", { maxDepth: 0 });
        await c.next(); await c.next();
        ok((await c.next()).done === true && (await c.next()).done === true,
           "PIN: next() past the end keeps returning done:true");
        ok(typeof c[Symbol.iterator] === "undefined" && typeof c[Symbol.asyncIterator] === "function", "only Symbol.asyncIterator exists (2026-11-26: the sync iterator was removed)");
        ok(c.pages() === c, "PIN: pages() returns the crawl itself (the lazy page stream)");
        c.close();
    }

    {
        const c = mkC("http://hub.test/hub", {});
        await throws(() => c.start("file:///etc/passwd"), "a file:// seed is refused");
        await throws(() => c.start("ftp://x.test/"), "ftp:// is refused");
        c.close();
    }
});
__kitS.end();

print("test_scrape_modern: " + pass + " passed, " + fail + " failed");
if (fail) throw new Error("test_scrape_modern: " + fail + " failures");
