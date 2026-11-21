// flags: --std
// timeout: 180
// covers: contract dyna:scrape.Extractor dyna:scrape.Fetcher dyna:scrape.Crawl dyna:scrape.Sitemap -- D49: every option, response, extractor-result and global read propagates a throwing getter
// st1q D49: the scrape natives read options, client responses, extractor results and globals with JS_GetProperty* and
// type-tested the result before any JS_IsException check, so a throwing getter was read as "absent": the call went on
// with the error pending (later surfacing as the wrong error, or lost), or a module TypeError/RangeError replaced it.
// fe_header/fe_headers_all now return NULL with the error pending and every caller propagates it; header values
// whose toString throws reject instead of being dropped. Crawl reads of the extractor result, the response and the
// page (inherited Object.prototype accessors included) reject next(); a thenable whose then getter throws rejects
// with that error instead of undefined. Oracle: each hostile call throws or rejects with exactly the sentinel.
// Also: a GC inside every getter keeps the result intact, and a --timeout-ms child's interrupt inside a
// getter ends a loop that catches everything.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_scrape_getter_abrupt");
import { Extractor, Fetcher, Crawl, Sitemap } from "dyna:scrape";
import { Exec, args } from "dyna:sys";
import * as std from "std";

const SELF = "tests/scrape/test_scrape_getter_abrupt.js";
const SENT = new Error("scrape getter sentinel");
const thrower = () => { throw SENT; };
const run = (f) => { try { return { v: f() }; } catch (e) { return { e }; } };
const arun = async (f) => { try { return { v: await f() }; } catch (e) { return { e }; } };
const with_ = (base, k, get = thrower) => { const x = { ...base }; delete x[k]; Object.defineProperty(x, k, { get, enumerable: true, configurable: true }); return x; };
const own = (k) => function (v) { Object.defineProperty(this, k, { value: v, writable: true, enumerable: true, configurable: true }); };
const bad = { toString: thrower };
const sel = (nodes) => ({ all: () => nodes });
const FIELD = { sel: sel([]), attr: "href", all: true, required: false, trim: true, source: false, default: "d", as: "url" };
const resp = (o) => ({ status: 200, headers: {}, body: "<p>ok</p>", ...o });
const F = (client, o = {}) => new Fetcher({ agent: "t-bot/1.0", client, robots: false, minDelayMs: 0, retries: 0, ...o });
const cl = (mk) => ({ request: () => mk() });
const swap = async (obj, k, d, f) => { const old = Object.getOwnPropertyDescriptor(obj, k); Object.defineProperty(obj, k, d); try { return await f(); } finally { if (old) Object.defineProperty(obj, k, old); else delete obj[k]; } };
const crawlNext = async (resF, runF, opts = {}, start) => {
    const cr = new Crawl({ get: (u) => resF(u) }, { maxDepth: 2, ...opts });
    if (start) start(cr); else cr.start("http://seed.test/", { run: runF }, (s) => s);
    try { return await arun(() => cr.next()); } finally { cr.close(); }
};
const gcw = (base) => { const x = {}; for (const k of Object.keys(base)) { const v = base[k]; Object.defineProperty(x, k, { get() { std.gc(); return v; }, enumerable: true }); } return x; };

const PINNED = [
    ["Extractor attr url", () => new Extractor({ f: { sel: sel([{ attrs: { href: "/x" } }]), attr: "href", as: "url" } }).run({}, { base: "http://h.test/" }).value.f],
    ["Extractor all trim number json", () => { const v = new Extractor({ n: { sel: sel([{ attrs: { v: " 42 " } }, { attrs: { v: "7" } }]), attr: "v", all: true, trim: true, as: "number" }, j: { sel: sel([{ attrs: { v: "[1,2]" } }]), attr: "v", as: "json" }, m: { sel: sel([]), attr: "v", required: true } }).run({}).value; return v.n.join("+") + "|" + v.j.length; }],
    ["Extractor source", () => new Extractor({ f: { sel: sel([{ tag: "p", attrs: {}, children: ["a", { tag: "b", attrs: {}, children: ["c"] }] }]), source: true } }).run({}).value.f],
];
const APINNED = [
    ["Fetcher get", async () => (await F(cl(() => resp({ headers: { "content-type": "text/html", "x-robots-tag": "noindex", link: "<http://h.test/c>; rel=canonical" } }))).get("http://h.test/a")).contentType],
    ["Crawl next", async () => { const r = await crawlNext(() => resp({}), () => ({ value: { links: ["/n"] } })); if (r.e !== undefined) throw r.e; return r.v.value.value.links[0]; }],
];

if (args().includes("--child")) {
    const mode = args()[args().indexOf("--child") + 1];
    if (mode === "interrupt") {
        const spin = () => { let i = 0; for (;;) i++; };
        for (;;) { try { new Extractor({ f: with_(FIELD, "trim", spin) }); } catch (e) {} }
    }
    std.exit(0);
}

{
    let n = 0, fails = 0;
    const ok = (c, w) => { n++; if (!c) { fails++; print("  FAIL  " + w); } };
    const desc = (r) => r.e !== undefined ? "threw " + (r.e === SENT ? "SENT" : String(r.e)) : "returned " + String(JSON.stringify(r.v)).slice(0, 160);
    const sent = (w, f) => { const r = run(f); ok(r.e === SENT, w + ": " + desc(r)); };
    const asent = async (w, f) => { const r = await arun(f); ok(r.e === SENT, w + ": " + desc(r)); };

    await __kitS.section("Extractor constructor and run propagate every getter", () => {
        sent("Extractor spec field getter", () => new Extractor(with_({}, "f")));
        for (const k of Object.keys(FIELD))
            sent("Extractor field " + k, () => new Extractor({ f: with_(FIELD, k) }));
        sent("Extractor opts text", () => new Extractor({ f: FIELD }, with_({}, "text")));
        const ex = new Extractor({ f: { sel: sel([{ attrs: { href: "/x" } }]), attr: "href" } });
        sent("run opts base", () => ex.run({}, with_({}, "base")));
        sent("run sel.all getter", () => new Extractor({ f: { sel: with_({}, "all"), attr: "x" } }).run({}));
        sent("run node attrs getter", () => new Extractor({ f: { sel: sel([with_({}, "attrs")]), attr: "href" } }).run({}));
        sent("run node children getter", () => new Extractor({ f: { sel: sel([with_({ tag: "p", attrs: {} }, "children")]), source: true } }).run({}));
        const kids = ["a", "b"];
        Object.defineProperty(kids, 1, { get: thrower });
        sent("run node children[1] getter", () => new Extractor({ f: { sel: sel([{ tag: "p", attrs: {}, children: kids }]), source: true } }).run({}));
        for (const [w, f] of PINNED) { const r = run(f); ok(r.e === undefined && typeof r.v === "string" && r.v.length > 0, "CONTROL " + w + ": " + desc(r)); }
        ok(PINNED[0][1]() === "http://h.test/x", "CONTROL attr url resolves");
        const g = new Extractor({ f: gcw({ sel: sel([gcw({ attrs: gcw({ href: "/x" }) })]), attr: "href", as: "url", trim: true }) });
        ok(g.run({}, gcw({ base: "http://h.test/" })).value.f === "http://h.test/x", "gc in every getter keeps the extractor result");
    });

    await __kitS.section("Fetcher responses, headers and globals propagate every getter", async () => {
        const U = "http://h.test/a";
        await asent("get response headers getter", () => F(cl(() => with_(resp({}), "headers"))).get(U));
        for (const h of ["content-length", "content-type", "x-robots-tag", "link", "etag", "last-modified"])
            await asent("get header " + h + " toString", () => F(cl(() => resp({ headers: { [h]: bad } }))).get(U));
        await asent("get 404 content-type toString", () => F(cl(() => resp({ status: 404, headers: { "content-type": bad } }))).get(U));
        {
            let reqs = 0;
            await asent("get 503 retry-after toString", () => F(cl(() => (reqs++, resp({ status: 503, headers: { "retry-after": bad } }))), { retries: 1 }).get(U));
            ok(reqs === 1, "no retry after the retry-after toString threw: " + reqs + " requests");
        }
        await asent("run as:number valueOf", () => new Extractor({ f: { sel: sel([{}]), as: "number" } }, { text: () => ({ valueOf: thrower }) }).run({}));
        {
            const r = run(() => new Extractor({ f: { sel: sel([{ attrs: { j: "{bad" } }]), attr: "j", as: "json" } }).run({}));
            ok(r.e === undefined && r.v.value.f === undefined, "CONTROL as:json SyntaxError still reads as absent: " + desc(r));
        }
        await asent("get 302 location toString", () => F(cl(() => resp({ status: 302, headers: { location: bad } }))).get(U));
        {
            const X = with_({}, "message");
            const r = await arun(() => F({ request: () => { throw X; } }).get(U));
            ok(r.e === X, "get: a client throw with a hostile message getter rejects with the thrown value unchanged: " + desc(r));
        }
        await asent("robots redirect location toString", () => new Fetcher({ agent: "t-bot/1.0", minDelayMs: 0, retries: 0,
            client: { request: (m, u) => String(u).endsWith("/robots.txt") ? resp({ status: 302, headers: { location: bad } }) : resp({}) } }).get(U));
        {
            const f = F(cl(() => resp({})), { minDelayMs: 400 });
            await f.get("http://p.test/1");
            await swap(globalThis, "setTimeout", { get: thrower, configurable: true }, () => asent("pacing pause setTimeout getter", () => f.get("http://p.test/2")));
        }
        {
            const f = new Fetcher({ agent: "t-bot/1.0", minDelayMs: 0, retries: 0,
                client: { request: (m, u) => resp({ body: "User-agent: *\nDisallow: /", headers: { "content-type": "text/plain" } }), getStream: () => resp({}) } });
            const s = await arun(() => f.getStream("http://r.test/x"));
            ok(s.v && s.v.skippedByRobots === true, "CONTROL robots-skipped stream: " + desc(s));
            await swap(globalThis, "Symbol", { get: thrower, configurable: true }, () => asent("skipped stream Symbol getter", () => f.getStream("http://r.test/y")));
        }
        await swap(globalThis, "__dyna_scrape_client", { get: thrower, set(v) {}, configurable: true }, () => asent("auto client holder getter", () => new Fetcher({ agent: "t-bot/1.0" }).get(U)));
        await swap(globalThis, "__dyna_scrape_fef", { get: thrower, set(v) {}, configurable: true }, () => asent("Sitemap own fetcher holder getter", () => Sitemap.list("http://h.test/sitemap.xml")));
        for (const [w, f] of APINNED) { const r = await arun(f); ok(r.e === undefined && typeof r.v === "string", "CONTROL " + w + ": " + desc(r)); }
        const r = await arun(() => F(cl(() => gcw(resp({ headers: gcw({ "content-type": "text/html" }) })))).get(U));
        ok(r.v && r.v.contentType === "text/html", "gc in every response getter keeps the fetch result: " + desc(r));
    });

    await __kitS.section("Crawl constructor, resume and page reads propagate every getter", async () => {
        const fe = { get: () => resp({}) };
        await swap(globalThis, "Symbol", { get: thrower, configurable: true }, () => sent("Crawl ctor Symbol getter", () => new Crawl(fe)));
        await swap(Crawl.prototype, "pages", { get: thrower, configurable: true }, () => sent("Crawl ctor pages getter", () => new Crawl(fe)));
        const st = new Crawl(fe).start("http://seed.test/").serialize();
        await swap(globalThis, "Symbol", { get: thrower, configurable: true }, () => sent("Crawl.resume Symbol getter", () => Crawl.resume(fe, st)));
        const V = (v) => () => ({ value: v });
        await asent("page extractor result value getter", async () => { const r = await crawlNext(() => resp({}), () => with_({}, "value")); if (r.e !== undefined) throw r.e; return r.v; });
        const cases = [
            ["robotsField getter", { robotsField: "robots" }, V(with_({ links: [] }, "robots"))],
            ["canonicalField getter", { canonicalField: "canon" }, V(with_({ links: [] }, "canon"))],
            ["baseField getter", { baseField: "base" }, V(with_({ links: [] }, "base"))],
            ["linkField getter", { linkField: "links" }, V(with_({}, "links"))],
            ["relField getter", { relField: "rels" }, V(with_({ links: ["/a"] }, "rels"))],
        ];
        const rels = ["x"];
        Object.defineProperty(rels, 0, { get: thrower });
        cases.push(["rels[0] getter", { relField: "rels" }, V({ links: ["/a"], rels })]);
        for (const [w, o, rf] of cases) {
            const r = await crawlNext(() => resp({}), rf, o);
            ok(r.e === SENT, "page " + w + ": " + desc(r));
        }
        for (const [w, rf] of [["body", () => with_(resp({}), "body")], ["canonicalUrl", () => with_(resp({}), "canonicalUrl")], ["url", () => with_(resp({}), "url")], ["then", () => with_({}, "then")]]) {
            const r = await crawlNext(rf, V({ links: [] }));
            ok(r.e === SENT, "page response " + w + " getter: " + desc(r));
        }
        {
            Object.defineProperty(Object.prototype, "robots", { get: thrower, set: own("robots"), configurable: true });
            let r;
            try { r = await crawlNext(() => resp({}), V({ links: [] })); } finally { delete Object.prototype.robots; }
            ok(r.e === SENT, "page inherited robots getter: " + desc(r));
        }
        for (const k of ["_extractor", "_parse"]) {
            const r = await swap(Crawl.prototype, k, { get: thrower, configurable: true }, () => crawlNext(() => resp({}), null, {}, (cr) => k === "_extractor" ? cr.start("http://seed.test/") : cr.start("http://seed.test/", { run: V({ links: [] }) })));
            ok(r.e === SENT, "Crawl.prototype." + k + " getter: " + desc(r));
        }
        const good = await crawlNext(() => resp({}), V({ links: ["/n"] }));
        ok(good.v && good.v.value && good.v.value.value.links[0] === "/n", "CONTROL crawl page: " + desc(good));
        const g = await crawlNext(() => gcw(resp({})), () => gcw({ value: gcw({ links: ["/n"] }) }), { linkField: "links" });
        ok(g.v && g.v.value.status === 200 && g.v.value.value.links[0] === "/n", "gc in every crawl getter keeps the page: " + desc(g));
    });

    await __kitS.section("an interrupt that must end a catch-all loop", () => {
        const t0 = Date.now();
        const i = Exec(args()[0], ["--std", "--timeout-ms", "300", SELF, "--child", "interrupt"], { timeoutMs: 20000 });
        ok(i.code !== 0 && !i.timedOut && /interrupt/i.test(i.stderr + i.stdout), "interrupt child ends: rc " + i.code + " after " + (Date.now() - t0) + " ms " + i.stderr.slice(-200));
    });
    if (fails) throw new Error(fails + " of " + n + " checks failed");
    print("test_scrape_getter_abrupt: " + n + " checks passed");
}
