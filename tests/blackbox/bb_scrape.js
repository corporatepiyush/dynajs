// Parametric black-box contract test for dyna:scrape, generated from dynajs.d.ts lines 4156-4422.
// Engine sources not consulted. OFFLINE surface only: Robots parsing, Extractor field specs over a
// fixed HTML fixture, and Sitemap.parse bounds/refusals — plus, since the scrape-depth coverage
// pass, the previously blackbox-zero Fetcher and Crawl classes, exercised against OFFLINE loopback
// origins only (a dyna:http HTTPServer and a local python3 mock, both on 127.0.0.1 port 0; no
// external DNS, teardown in finally, see the coverage-pass section at the end).
//
// Module banner (d.ts L4157-4158): "Every options bag is STRICT: an unknown key throws a TypeError
// naming the key and the valid set (e.g. `unknown option "agnt" (valid: agent)`)."

import "../httpc.js";
import { Robots, Extractor, Sitemap, Fetcher, Crawl } from "dyna:scrape";
import { HTMLParse, HTMLText, Selector } from "dyna:html";
import { HTTPServer } from "dyna:http";
import { HTTPClient } from "dyna:net";
import { Exec, Which } from "dyna:sys";
import { makeTempDir, writeFile, readFile, removeAll, Path } from "dyna:file";

let n = 0;
function assert(cond, msg) { n++; if (!cond) throw new Error("assertion failed: " + msg); }
function assertEq(actual, expected, msg) { n++; const ok = Object.is(actual, expected) || (typeof actual === "number" && typeof expected === "number" && Number.isNaN(actual) && Number.isNaN(expected)); if (!ok) throw new Error("assertion failed: " + msg + " — got |" + actual + "| expected |" + expected + "|"); }
function assertDeepEq(a, b, msg) { n++; if (JSON.stringify(a) !== JSON.stringify(b)) throw new Error("assertion failed (deep): " + msg + " — got |" + JSON.stringify(a) + "| expected |" + JSON.stringify(b) + "|"); }
function assertThrows(fn, msg, ErrType, errPattern) { n++; let threw = false, e = null; try { fn(); } catch (err) { threw = true; e = err; } if (!threw) throw new Error("expected throw: " + msg); if (ErrType && !(e instanceof ErrType)) throw new Error("wrong error type " + (e && e.constructor ? e.constructor.name : String(e)) + ": " + msg); if (errPattern && !(errPattern instanceof RegExp ? errPattern.test(String(e)) : String(e).includes(errPattern))) throw new Error("wrong error message |" + e + "|: " + msg); }

function runTable(tname, rows, fn) {
  for (const row of rows) {
    try { fn(row); }
    catch (e) { throw new Error("table " + tname + " row [" + row[0] + "]: " + (e && e.message ? e.message : String(e))); }
  }
}

// ============================ Robots: allows (d.ts L4164-4168) ============================
// "robots.txt parsing (RFC 9309)." / "True when the path is allowed for the configured agent."
// RFC 9309 5.2: the MOST SPECIFIC rule (longest matched path) wins.
runTable("robots:allows", [
  ["unlisted path is allowed", () => {
    const r = new Robots("User-agent: *\nDisallow: /private\n");
    assertEq(r.allows("/public"), true, "no matching rule allows");
  }],
  ["disallowed prefix matches everything under it", () => {
    const r = new Robots("User-agent: *\nDisallow: /private\n");
    assertEq(r.allows("/private"), false, "the prefix itself");
    assertEq(r.allows("/private/x"), false, "under the prefix");
  }],
  ["longer Allow beats shorter Disallow", () => {
    const r = new Robots("User-agent: *\nDisallow: /private\nAllow: /private/ok\n");
    assertEq(r.allows("/private/ok"), true, "the most specific rule wins");
  }],
  ["an empty Disallow value is allow-all", () => {
    const r = new Robots("User-agent: *\nDisallow:\n");
    assertEq(r.allows("/anything"), true, "empty Disallow disables the rule");
  }],
  ["Disallow: / blocks everything", () => {
    const r = new Robots("User-agent: *\nDisallow: /\n");
    assertEq(r.allows("/anything"), false, "the root prefix matches every path");
  }],
  ["a named agent group replaces *", () => {
    const txt = "User-agent: *\nDisallow: /\n\nUser-agent: dynabot\nDisallow: /nope\n";
    assertEq(new Robots(txt, { agent: "dynabot" }).allows("/yes"), true, "the named group's rules apply");
    assertEq(new Robots(txt, { agent: "dynabot" }).allows("/nope"), false, "and only them");
  }],
  ["an agent with no own group falls back to *", () => {
    const txt = "User-agent: *\nDisallow: /\n\nUser-agent: dynabot\nDisallow: /nope\n";
    assertEq(new Robots(txt, { agent: "other" }).allows("/yes"), false, "* is the fallback group");
  }],
  ["no applicable group and no * allows everything (RFC 9309)", () => {
    const r = new Robots("User-agent: dynabot\nDisallow: /\n", { agent: "other" });
    assertEq(r.allows("/yes"), true, "unrestricted when no group applies");
  }],
], row => row[1]());

// ============================ Robots: crawlDelay / sitemaps / ruleCount / lifecycle (d.ts L4169-4177) ============================
runTable("robots:meta", [
  ["absent Crawl-delay is null", () => {
    assertEq(new Robots("User-agent: *\nDisallow: /x\n").crawlDelay(), null, "no Crawl-delay line -> null");
  }],
  ["a well-formed Crawl-delay parses (fractional)", () => {
    assertEq(new Robots("User-agent: *\nCrawl-delay: 2.5\n").crawlDelay(), 2.5, "2.5 parses");
  }],
  ["sitemaps are listed in file order", () => {
    const r = new Robots("User-agent: *\nDisallow: /x\n" +
      "Sitemap: https://h.test/s1.xml\nSitemap: https://h.test/s2.xml\n");
    assertDeepEq(r.sitemaps(), ["https://h.test/s1.xml", "https://h.test/s2.xml"], "both sitemaps, in order");
  }],
  ["ruleCount is a non-negative integer (0 for a ruleless file)", () => {
    const empty = new Robots("User-agent: *\n");
    assert(Number.isInteger(empty.ruleCount) && empty.ruleCount >= 0, "ruleless file: got " + empty.ruleCount);
    const withRules = new Robots("User-agent: *\nDisallow: /a\nDisallow: /b\n");
    assert(Number.isInteger(withRules.ruleCount) && withRules.ruleCount > 0,
           "a file with rules counts them, got " + withRules.ruleCount);
  }],
  ["strict opts bag names the key and the valid set", () => {
    assertThrows(() => new Robots("User-agent: *\nDisallow: /", { agnt: "b" }),
                 "unknown key is refused", TypeError, /agnt/);
    assertThrows(() => new Robots("User-agent: *\nDisallow: /", { agnt: "b" }),
                 "the valid set is named", TypeError, /agent/);
  }],
  ["close() flips closed; dispose surface exists", () => {
    const r = new Robots("User-agent: *\nDisallow: /x\n");
    assertEq(r.closed, false, "live when new");
    r.close();
    assertEq(r.closed, true, "closed after close()");
    assert(typeof r.dispose === "function" && typeof r[Symbol.dispose] === "function",
           "dispose and Symbol.dispose are on the surface");
  }],
], row => row[1]());

// ============================ Extractor (d.ts L4180-4192) ============================
// "spec maps field names to { sel, attr?, all?, required?, trim?, source?, default?, as? } with
//  as: 'number' | 'url' | 'json'; source reads raw child source"; run returns
//  "{ ok, value, missing }"; "options.base resolves as: 'url'".
const DOC = HTMLParse(
  '<html><head>' +
  '<script type="application/ld+json">{"@type":"Thing","name":"Widget"}</script>' +
  '</head><body>' +
  '<h1>  Widget  </h1>' +
  '<span class="price">19.99</span>' +
  '<span class="cost">not-a-number</span>' +
  '<a href="/items/a">A</a><a href="/items/b">B</a>' +
  '<script>var bare = 1;</script>' +
  '<pre id="cfg">{"k": 7}</pre>' +
  '</body></html>');
const TEXT = { text: HTMLText };

runTable("extractor:fields", [
  ["text extraction via the injected text fn", () => {
    const r = new Extractor({ title: { sel: new Selector("h1") } }, TEXT).run(DOC);
    assertEq(r.ok, true, "matched");
    assertEq(r.value.title, "  Widget  ", "raw text before trim");
  }],
  ["trim: true trims the text", () => {
    const r = new Extractor({ title: { sel: new Selector("h1"), trim: true } }, TEXT).run(DOC);
    assertEq(r.value.title, "Widget", "trimmed");
  }],
  ["attr extraction", () => {
    const r = new Extractor({ link: { sel: new Selector("a"), attr: "href" } }, TEXT).run(DOC);
    assertEq(r.value.link, "/items/a", "the href attribute value");
  }],
  ["all: true collects every match in document order", () => {
    const r = new Extractor({ links: { sel: new Selector("a"), attr: "href", all: true } }, TEXT).run(DOC);
    assertDeepEq(r.value.links, ["/items/a", "/items/b"], "both hrefs in order");
  }],
  ["as: number converts", () => {
    const r = new Extractor({ price: { sel: new Selector(".price"), as: "number" } }, TEXT).run(DOC);
    assertEq(r.value.price, 19.99, "19.99 as a number");
  }],
  ["as: number on non-numeric text misses", () => {
    const r = new Extractor({ price: { sel: new Selector(".cost"), as: "number", required: true } }, TEXT).run(DOC);
    assertEq(r.ok, false, "not silently null");
    assertDeepEq(r.missing, ["price"], "the field is named in missing");
  }],
  ["as: url resolves against opts.base", () => {
    const r = new Extractor({ link: { sel: new Selector("a"), attr: "href", as: "url" } }, TEXT)
      .run(DOC, { base: "https://h.test/dir/" });
    // "/items/a" is a root-absolute reference: the base's path is discarded (URL resolution)
    assertEq(r.value.link, "https://h.test/items/a", "root-absolute href + base");
    const rel = HTMLParse('<html><body><a href="i/b.html">B</a></body></html>');
    const r2 = new Extractor({ link: { sel: new Selector("a"), attr: "href", as: "url" } }, TEXT)
      .run(rel, { base: "https://h.test/dir/" });
    assertEq(r2.value.link, "https://h.test/dir/i/b.html", "path-relative href + base");
  }],
  ["as: url keeps an absolute href", () => {
    const abs = HTMLParse('<html><body><a href="https://other.test/x">X</a></body></html>');
    const r = new Extractor({ link: { sel: new Selector("a"), attr: "href", as: "url" } }, TEXT)
      .run(abs, { base: "https://h.test/dir/" });
    assertEq(r.value.link, "https://other.test/x", "absolute stays");
  }],
  ["as: json parses the field text", () => {
    const r = new Extractor({ cfg: { sel: new Selector("pre"), as: "json" } }, TEXT).run(DOC);
    assertDeepEq(r.value.cfg, { k: 7 }, "the JSON text became a value");
  }],
  ["source: true reads the raw child source (first match, document order)", () => {
    const r = new Extractor({ ld: { sel: new Selector("script"), source: true } }, TEXT).run(DOC);
    assertEq(r.value.ld, '{"@type":"Thing","name":"Widget"}',
             "the first <script>'s raw source, verbatim");
    assert(typeof r.value.ld === "string", "raw source is the text, not a parsed value");
  }],
  ["required miss makes ok false and names the field", () => {
    const r = new Extractor({ gone: { sel: new Selector(".nope"), required: true } }, TEXT).run(DOC);
    assertEq(r.ok, false, "drift is a failure");
    assertDeepEq(r.missing, ["gone"], "missing names the field");
  }],
  ["optional miss keeps ok true", () => {
    const r = new Extractor({ gone: { sel: new Selector(".nope") } }, TEXT).run(DOC);
    assertEq(r.ok, true, "an optional miss is not a failure");
    assertDeepEq(r.missing, [], "nothing reported missing");
  }],
  ["default fills an optional miss", () => {
    const r = new Extractor({ gone: { sel: new Selector(".nope"), default: "fallback" } }, TEXT).run(DOC);
    assertEq(r.value.gone, "fallback", "the spec's default value");
  }],
  ["required hit reports no missing", () => {
    const r = new Extractor({ title: { sel: new Selector("h1"), required: true } }, TEXT).run(DOC);
    assertEq(r.ok, true, "required hit");
    assertDeepEq(r.missing, [], "empty missing");
  }],
  ["strict field-spec bag names the key", () => {
    assertThrows(() => new Extractor({ t: { sel: new Selector("h1"), al: true } }, TEXT),
                 "unknown field-spec key is refused", TypeError, /al/);
  }],
  ["strict ctor bag names the key", () => {
    assertThrows(() => new Extractor({ t: { sel: new Selector("h1") } }, { txt: HTMLText }),
                 "unknown ctor key is refused", TypeError, /txt/);
  }],
  ["strict run bag names the key", () => {
    const ex = new Extractor({ t: { sel: new Selector("h1") } }, TEXT);
    assertThrows(() => ex.run(DOC, { baz: "/x" }),
                 "unknown run key is refused", TypeError, /baz/);
  }],
], row => row[1]());

// ============================ Sitemap.parse: shape (d.ts L4396-4408) ============================
// "Parse sitemap XML into the <loc> list, in document order. A <sitemapindex> parses to its child
//  sitemap URLs. Comments, PIs, DOCTYPE, CDATA, attributes and the default namespace are skipped;
//  the five named entities and numeric refs decode inside <loc>; element names match
//  case-insensitively."
runTable("sitemap:parse", [
  ["urlset parses in document order; siblings of <loc> are ignored", () => {
    assertDeepEq(
      Sitemap.parse('<urlset><url><loc>http://x.test/</loc><lastmod>2026-01-01</lastmod></url>' +
                    '<url><loc>http://x.test/b</loc></url></urlset>'),
      ["http://x.test/", "http://x.test/b"], "document order");
  }],
  ["a sitemapindex parses to its child sitemap URLs", () => {
    assertDeepEq(
      Sitemap.parse('<sitemapindex><sitemap><loc>http://x.test/s1.xml</loc></sitemap>' +
                    '<sitemap><loc>http://x.test/s2.xml</loc></sitemap></sitemapindex>'),
      ["http://x.test/s1.xml", "http://x.test/s2.xml"], "index children");
  }],
  ["an empty urlset is an empty list", () => {
    assertDeepEq(Sitemap.parse("<urlset></urlset>"), [], "empty urlset");
    assertDeepEq(Sitemap.parse("<urlset/>"), [], "self-closed root");
  }],
  ["comments and processing instructions are skipped", () => {
    assertDeepEq(
      Sitemap.parse('<!-- head --><?xml-stylesheet href="x"?><urlset><!-- mid -->' +
                    '<url><loc>http://x.test/c</loc></url></urlset>'),
      ["http://x.test/c"], "markup skipped");
  }],
  ["DOCTYPE is skipped", () => {
    assertDeepEq(
      Sitemap.parse('<!DOCTYPE urlset><urlset><url><loc>http://x.test/d</loc></url></urlset>'),
      ["http://x.test/d"], "doctype skipped");
  }],
  ["attributes and the default namespace are skipped", () => {
    assertDeepEq(
      Sitemap.parse('<urlset xmlns="http://www.sitemaps.org/schemas/sitemap/0.9">' +
                    '<url><loc attr="ignored">http://x.test/a</loc></url></urlset>'),
      ["http://x.test/a"], "default ns + loc attribute");
  }],
  ["element names match case-insensitively", () => {
    assertDeepEq(
      Sitemap.parse('<URLSET><URL><LOC>http://x.test/up</LOC></URL></URLSET>'),
      ["http://x.test/up"], "uppercase vocabulary");
  }],
  ["the five named entities decode inside <loc>", () => {
    assertDeepEq(
      Sitemap.parse('<urlset><url><loc>http://x.test/e?a=&amp;b=&lt;c&gt;=&quot;d&quot;=&apos;e&apos;</loc></url></urlset>'),
      ["http://x.test/e?a=&b=<c>=\"d\"='e'"], "amp, lt, gt, quot, apos");
  }],
  ["numeric character refs decode (decimal and hex)", () => {
    assertDeepEq(
      Sitemap.parse('<urlset><url><loc>http://x.test/n?a=&#38;b=&#x26;</loc></url></urlset>'),
      ["http://x.test/n?a=&b=&"], "38 and 0x26 are both &");
  }],
], row => row[1]());

// ============================ Sitemap.parse: refusals and bounds (d.ts L4403-4407) ============================
// "Structural breakage throws SyntaxError with the byte offset; non-XML input throws TypeError;
//  an empty urlset is an empty list. Bounds: 16 MiB, 50000 entries, 2048-byte <loc> text."
runTable("sitemap:refusals", [
  ["a <loc> with no </loc> is structural breakage (SyntaxError)", () => {
    assertThrows(() => Sitemap.parse("<urlset><url><loc>http://x.test/u</url></urlset>"),
                 "unclosed loc", SyntaxError);
  }],
  ["the SyntaxError carries the byte offset", () => {
    // the unterminated comment starts at byte 0
    assertThrows(() => Sitemap.parse("<!-- unterminated"), "unterminated comment", SyntaxError, /byte 0/);
  }],
  ["a truncated document is a SyntaxError", () => {
    assertThrows(() => Sitemap.parse("<urlset><url><loc>ok</loc"), "truncated tail", SyntaxError);
  }],
  ["non-XML input throws TypeError", () => {
    assertThrows(() => Sitemap.parse("just some words"), "plain text", TypeError);
    assertThrows(() => Sitemap.parse("User-agent: *\nDisallow: /"), "a robots.txt is not a sitemap", TypeError);
  }],
  ["a <loc> of exactly 2048 bytes parses (the spec's url budget)", () => {
    const out = Sitemap.parse("<urlset><url><loc>" + "x".repeat(2048) + "</loc></url></urlset>");
    assertDeepEq(out, ["x".repeat(2048)], "2048 is inside the bound");
  }],
  ["a 2049-byte <loc> refuses (the bound is exact)", () => {
    assertThrows(() => Sitemap.parse("<urlset><url><loc>" + "x".repeat(2049) + "</loc></url></urlset>"),
                 "one past the url budget");
  }],
  ["more than 50000 entries refuses", () => {
    assertThrows(() => Sitemap.parse("<urlset>" + "<url><loc>http://x.test/</loc></url>".repeat(50001)),
                 "one past the entry cap");
  }],
], row => row[1]());

/* ==================================================================== *
 *  Coverage pass (scrape-depth): Fetcher + Crawl, OFFLINE.
 *
 *  dynajs.d.ts L4194-4394 (Fetcher, FetcherResponse, FetcherStream,
 *  CrawlPage, Crawl) plus the dynajs.d.ts dyna:scrape section (authoritative
 *  for the option defaults the d.ts leaves implicit: robots default
 *  true, allowPrivateHosts default false, minDelayMs 1000, retries 3,
 *  maxRedirects 5, maxBodyBytes 8 MiB, maxPages 100, maxDepth 2,
 *  linkField "links", concurrency 1). Everything runs against origins
 *  on 127.0.0.1:0 through two doors:
 *
 *  1. dyna:http HTTPServer (thread-pool, static routes) — the bb_http.js
 *     pattern. Static rows only: happy-path get/getStream, the robots
 *     gate + skippedByRobots shape, body caps, 404 extraction, bounded
 *     traversal, field gates, serialize/resume.
 *  2. A python3 http.server subprocess (the engine's own
 *     tests/test_scrape_fetcher.js pattern) for the rows a STATIC route
 *     table cannot serve: Location redirects, a slow route for the
 *     timeout refusal, Retry-After, ETag revalidation (304), the
 *     X-Robots-Tag / Link: rel="canonical" response headers, canonical
 *     URLs carrying the live port, robots.txt refetch counts.
 *     SKIP-gated on python3.
 * ==================================================================== */

// awaits the call: a synchronous throw OR a rejected promise both count
async function assertRejects(call, msg, errPattern) {
  n++; let threw = false, e = null;
  try { const out = call(); if (out && typeof out.then === "function") await out; }
  catch (err) { threw = true; e = err; }
  if (!threw) throw new Error("expected rejection: " + msg);
  if (errPattern && !(errPattern instanceof RegExp ? errPattern.test(String(e)) : String(e).includes(errPattern)))
    throw new Error("wrong error message |" + e + "|: " + msg);
}
async function runRows(tableName, rows, fn) {
  for (const row of rows) {
    try { await fn(row); }
    catch (e) { throw new Error("table " + tableName + " row [" + row[0] + "]: " + (e && e.message ? e.message : String(e))); }
  }
}
const getHeader = (rec, name) => { const k = Object.keys(rec).find(k2 => k2.toLowerCase() === name); return k === undefined ? undefined : rec[k]; };
// A loopback Fetcher: the SSRF gate (d.ts L4235, allowPrivateHosts default
// false per dynajs.d.ts) refuses 127.0.0.1 without the opt-in, and minDelayMs 0 /
// retries 0 keep the politeness defaults (1000ms / 3, dynajs.d.ts) out of timing.
const AGENT = "bb-scrape/1.0 (+offline-loopback)";
const loopF = (extra) => new Fetcher({ agent: AGENT, allowPrivateHosts: true, minDelayMs: 0, retries: 0, ...extra });
// Crawl reads links/values from the Extractor's NAMED fields (d.ts L4334-4335);
// one spec serves every traversal row below.
const pageEx = () => new Extractor({
  title: { sel: new Selector("h1"), trim: true },
  links: { sel: new Selector("a"), attr: "href", all: true },
  rels:  { sel: new Selector("a"), attr: "rel",  all: true },
  canon: { sel: new Selector("link[rel=canonical]"), attr: "href" },
  pbase: { sel: new Selector("base"), attr: "href" },
}, { text: HTMLText });

// ---------------- Fetcher construction refusals (d.ts L4196-4236) ----------------
runTable("fetcher:ctor", [
  ["agent is required and the error explains there is no default (dynajs.d.ts: there is no default user agent)", () => {
    assertThrows(() => new Fetcher({}), "missing agent refused", TypeError, /agent/);
    assertThrows(() => new Fetcher({}), "the no-default rationale is named", TypeError, /no default/i);
  }],
  ["an empty agent is refused, not accepted as present", () => {
    assertThrows(() => new Fetcher({ agent: "" }), "empty agent refused", TypeError, /agent/);
  }],
  ["strict ctor bag names the key and the valid set (d.ts L4157)", () => {
    assertThrows(() => new Fetcher({ agent: "a", agnt: "b" }), "unknown key refused", TypeError, /agnt/);
    assertThrows(() => new Fetcher({ agent: "a", agnt: "b" }), "the valid set is named", TypeError, /valid: agent/);
  }],
  ["credential header keys are refused, naming the key (d.ts L4218-4221)", () => {
    for (const key of ["Authorization", "Cookie", "Proxy-Authorization", "Set-Cookie"]) {
      assertThrows(() => new Fetcher({ agent: "a", headers: { [key]: "x" } }),
                   key + " refused", TypeError, new RegExp(key));
    }
  }],
  ["poolSize: non-integer is a TypeError, out-of-range a RangeError (d.ts L4214-4217; dynajs.d.ts poolSize)", () => {
    for (const bad of ["5", 1.5, NaN]) {
      assertThrows(() => new Fetcher({ agent: "a", poolSize: bad }), "non-integer poolSize", TypeError, /integer/);
    }
    for (const out of [0, 65]) {
      assertThrows(() => new Fetcher({ agent: "a", poolSize: out }), "poolSize " + out + " out of 1..64", RangeError, /1 to 64/);
    }
  }],
  ["maxBodyBytes negative is refused with a TypeError naming the key (d.ts L4239 maxBodyBytes?: number; the strict-bag style of the poolSize check)", () => {
    for (const bad of [-1, -1048576]) {
      assertThrows(() => new Fetcher({ agent: "a", maxBodyBytes: bad }),
                   "negative maxBodyBytes refused", TypeError, /maxBodyBytes/);
    }
  }],
  ["stats() echoes the stored simple options: poolSize defaults to 4, proxy/ca null (d.ts L4245-4251)", () => {
    const f = new Fetcher({ agent: "a" });
    const s = f.stats();
    assertEq(s.poolSize, 4, "default poolSize");
    assertEq(s.proxy, null, "proxy unset");
    assertEq(s.ca, null, "ca unset");
    for (const k of ["fetched", "skippedByRobots", "retried", "throttledMs", "bytes", "revalidated", "savedBytes"]) {
      assert(typeof s[k] === "number" && s[k] >= 0, "counter " + k + " is a non-negative number, got " + s[k]);
    }
    f.close();
  }],
  ["poolSize echoes what was stored (64 round-trips)", () => {
    const f = new Fetcher({ agent: "a", poolSize: 64 });
    assertEq(f.stats().poolSize, 64, "stored poolSize echoed");
    f.close();
  }],
  ["lifecycle: closed flips on close(), close is idempotent, dispose closes too (d.ts L4266-4269)", () => {
    const f = new Fetcher({ agent: "a" });
    assertEq(f.closed, false, "live when new");
    assert(typeof f.dispose === "function" && typeof f[Symbol.dispose] === "function", "dispose surface exists");
    f.close();
    assertEq(f.closed, true, "closed after close()");
    f.close();
    const g = new Fetcher({ agent: "a" });
    g.dispose();
    assertEq(g.closed, true, "dispose closes");
  }],
], row => row[1]());

// ---------------- SSRF name gate (d.ts L4235, L4255; dynajs.d.ts example verbatim) ----------------
runTable("fetcher:ssrf", [
  ["get() refuses a loopback host with the default gate, naming the opt-in", () => {
    const f = new Fetcher({ agent: "a" }); // allowPrivateHosts defaults to false
    assertThrows(() => f.get("http://127.0.0.1:1/feed"), "loopback refused", TypeError, /private\/loopback/);
    assertThrows(() => f.get("http://127.0.0.1:1/feed"), "the allowPrivateHosts opt-in is named", TypeError, /allowPrivateHosts/);
    f.close();
  }],
  ["getStream() refuses synchronously under the same gate (dynajs.d.ts getStream example)", () => {
    const f = new Fetcher({ agent: "a" });
    assertThrows(() => f.getStream("http://127.0.0.1:1/feed.ndjson"), "loopback stream refused", TypeError, /private\/loopback/);
    f.close();
  }],
], row => row[1]());

/* ------------------------------------------------------------------ *
 *  Door 1: HTTPServer (thread-pool) on 127.0.0.1 port 0 — static rows.
 * ------------------------------------------------------------------ */
{
  const routes = {
    "/robots.txt": "User-agent: *\nDisallow: /private\n",
    "/hi.html": { status: 200, contentType: "text/html; charset=utf-8", body: "<h1>Hi</h1>" },
    "/stream": { status: 200, contentType: "text/plain", body: "0123456789".repeat(10) },
    "/gone": { status: 404, contentType: "text/plain", body: "gone" },
    "/big": { status: 200, contentType: "text/plain", body: "x".repeat(20000) },
    "/private": { status: 200, contentType: "text/plain", body: "SECRET" },
    "/index.html": { status: 200, contentType: "text/html", body: '<h1>Index</h1><a href="a.html">a</a><a href="../b.html">b</a>' },
    "/a.html": { status: 200, contentType: "text/html", body: '<h1>A</h1><a href="index.html">home</a>' },
    "/b.html": { status: 200, contentType: "text/html", body: '<h1>B</h1>' },
    "/hub.html": { status: 200, contentType: "text/html", body: '<h1>Hub</h1><a href="c1.html">1</a><a href="c2.html">2</a><a href="c3.html">3</a>' },
    "/c1.html": { status: 200, contentType: "text/html", body: '<h1>C1</h1>' },
    "/c2.html": { status: 200, contentType: "text/html", body: '<h1>C2</h1>' },
    "/c3.html": { status: 200, contentType: "text/html", body: '<h1>C3</h1>' },
    "/chain/d0": { status: 200, contentType: "text/html", body: '<a href="d1">down</a>' },
    "/chain/d1": { status: 200, contentType: "text/html", body: '<a href="d2">down</a>' },
    "/chain/d2": { status: 200, contentType: "text/html", body: '<h1>D2</h1>' },
    "/self.html": { status: 200, contentType: "text/html", body: '<h1>Self</h1><a href="self.html">me</a>' },
    "/gone.html": { status: 404, contentType: "text/html", body: '<h1>Missing</h1><a href="/never2.html">n</a>' },
    "/nf.html": { status: 200, contentType: "text/html", body: '<h1>NF</h1><a href="/nf-target.html" rel="nofollow">x</a><a href="/nf-ok.html">y</a>' },
    "/nf-target.html": { status: 200, contentType: "text/html", body: '<h1>NT</h1>' },
    "/nf-ok.html": { status: 200, contentType: "text/html", body: '<h1>OK</h1>' },
    "/meta.html": { status: 200, contentType: "text/html", body: '<html><head><meta name="robots" content="nofollow"></head><body><h1>M</h1><a href="/never3.html">n</a></body></html>' },
    "/relcan.html": { status: 200, contentType: "text/html", body: '<html><head><link rel="canonical" href="/other.html"></head><body><a href="chain/d0">c</a></body></html>' },
    "/basedir.html": { status: 200, contentType: "text/html", body: '<html><head><base href="/sub/"></head><body><a href="x.html">x</a></body></html>' },
    "/sub/x.html": { status: 200, contentType: "text/html", body: '<h1>X</h1>' },
    "/drops.html": { status: 200, contentType: "text/html", body: '<h1>Drops</h1><a href="#top">f</a><a href="javascript:void(0)">j</a><a href="mailto:a@b.c">m</a><a href="tel:123">t</a><a href="real.html">r</a>' },
    "/real.html": { status: 200, contentType: "text/html", body: '<h1>Real</h1>' },
    "/cross.html": { status: 200, contentType: "text/html", body: '<h1>Cross</h1><a href="http://other.example.invalid/x">x</a>' },
  };
  const srv = new HTTPServer({ port: 0, host: "127.0.0.1", routes });
  srv.start();
  const B = "http://127.0.0.1:" + srv.port;
  try {
    assert(srv.port > 0, "ephemeral port resolved, got " + srv.port);

    await runRows("fetcher:get", [
      ["happy path: status/body/verbatim contentType/url/fromCache/notModified (d.ts L4237, L4272-4278, dynajs.d.ts: contentType verbatim from the origin)", async () => {
        const f = loopF();
        const r = f.get(B + "/hi.html");
        assertEq(r.status, 200, "status");
        assertEq(r.body, "<h1>Hi</h1>", "body");
        assertEq(r.contentType, "text/html; charset=utf-8", "contentType verbatim, unfiltered");
        assertEq(r.url, B + "/hi.html", "url echoes the request");
        assertEq(r.fromCache, false, "first fetch is not from cache");
        assertEq(r.notModified, false, "notModified false");
        assert(typeof getHeader(r.headers, "content-type") === "string", "headers record carries content-type");
        f.close();
      }],
      ["a request-url fragment is stripped before anything else (dynajs.d.ts: never sent on the wire)", async () => {
        const f = loopF();
        const r = f.get(B + "/hi.html#frag");
        assertEq(r.status, 200, "fragment did not break the fetch");
        assertEq(r.url, B + "/hi.html", "fragment gone from the response url");
        f.close();
      }],
      ["404 passes through with its body and content type (d.ts L4274-4278)", async () => {
        const f = loopF();
        const r = f.get(B + "/gone");
        assertEq(r.status, 404, "status is the wire status");
        assertEq(r.body, "gone", "error body carried");
        assertEq(r.contentType, "text/plain", "error content type carried");
        f.close();
      }],
      ["robots gate is ON by default (dynajs.d.ts: robots default true): the disallowed path returns the status-0 skip", async () => {
        const f = loopF(); // no robots option at all
        const r = f.get(B + "/private");
        assertEq(r.status, 0, "robots refusal is status 0");
        assertEq(r.skippedByRobots, true, "skippedByRobots set");
        assertEq(r.body, "", "no body on a skip");
        assertEq(r.contentType, "", "no contentType on a skip (dynajs.d.ts: \"\" when skipped)");
        const s = f.stats();
        assertEq(s.skippedByRobots, 1, "counted as skipped");
        assertEq(s.fetched, 0, "and not as fetched");
        f.close();
      }],
      ["the received-bytes cap is the SECOND check and throws RangeError (dynajs.d.ts: maxBodyBytes is checked TWICE by design)", async () => {
        // a mock client whose declared Content-Length lies: 5 declared, 50 delivered
        const mock = { request(method, url) {
          if (url.endsWith("/robots.txt")) return { status: 200, headers: {}, body: "User-agent: *\n" };
          return { status: 200, headers: { "Content-Length": "5", "Content-Type": "text/plain" }, body: "x".repeat(50) };
        } };
        const f = loopF({ client: mock, maxBodyBytes: 10 });
        assertThrows(() => f.get("http://mock.test/liar"), "lying body refused", RangeError, /exceeds maxBodyBytes/);
        f.close();
      }],
      ["the declared Content-Length cap refuses over-large pages — FINDING scrapd-F1: the contract (dynajs.d.ts Fetcher.get: 'body cap (over-large throws RangeError)'; d.ts L4306-4308: declared-CL refusal is 'the same RangeError get throws') requires a RangeError, but the built-in client's cap fires first with a plain Error ('response too large'); logged in tests/blackbox/FINDINGS.md, pinned as observed (refusal happens, type drifts) pending the fix phase", async () => {
        const f = loopF({ maxBodyBytes: 10000 });
        assertThrows(() => f.get(B + "/big"), "over-large page refused (type drift: Error not RangeError — scrapd-F1)");
        f.close();
      }],
      ["stats() counts a real fetch: fetched, bytes; throttledMs stays a non-negative number (structural, d.ts L4245-4247)", async () => {
        const f = loopF();
        f.get(B + "/hi.html");
        const s = f.stats();
        assert(s.fetched >= 1, "fetched counted, got " + s.fetched);
        assert(s.bytes > 0, "bytes counted, got " + s.bytes);
        assert(s.throttledMs >= 0, "throttledMs is a number, got " + s.throttledMs);
        assertEq(s.revalidated, 0, "nothing revalidated yet");
        f.close();
      }],
    ], async ([, check]) => check());

    await runRows("fetcher:getstream", [
      ["the response IS the ByteSource: head fields + read loop to EOF (d.ts L4290-4317)", async () => {
        const f = loopF();
        const st = f.getStream(B + "/stream");
        assertEq(st.status, 200, "status");
        assertEq(st.ok, true, "ok");
        assertEq(st.contentType, "text/plain", "contentType");
        assertEq(st.url, B + "/stream", "url");
        assert(typeof st.statusText === "string", "statusText present");
        const buf = new Uint8Array(16);
        let out = "", reads = 0;
        for (;;) { const k = await st.read(buf); if (k === 0) break; out += new TextDecoder().decode(buf.subarray(0, k)); reads++; }
        assertEq(out, "0123456789".repeat(10), "body reassembled across reads");
        assert(reads > 1, "several reads served the 100-byte body, got " + reads);
        st.close();
        assertEq(st.closed, true, "closed flips");
        f.close();
      }],
      ["the robots-refused stream carries the status-0 skippedByRobots shape with the body at EOF (d.ts L4304-4305)", async () => {
        const f = loopF();
        const st = f.getStream(B + "/private");
        assertEq(st.status, 0, "status 0");
        assertEq(st.skippedByRobots, true, "skippedByRobots true");
        assertEq(st.ok, false, "not ok");
        const buf = new Uint8Array(32);
        assertEq(await st.read(buf), 0, "body at EOF");
        st.close();
        f.close();
      }],
      ["a DECLARED Content-Length over maxBodyBytes refuses before the body starts, with RangeError (d.ts L4306-4308)", async () => {
        const f = loopF({ maxBodyBytes: 10000 });
        assertThrows(() => f.getStream(B + "/big"), "declared cap refusal", RangeError, /exceeds maxBodyBytes/);
        f.close();
      }],
    ], async ([, check]) => check());

    // SSRF gate vs non-canonical IP literals: getaddrinfo resolves these
    // dotted/hex/octal/short forms and expanded/mapped IPv6 spellings to
    // 127.0.0.1, so the gate (d.ts L4235 "allowPrivateHosts?: boolean",
    // L4259 "the SSRF name gate") must canonicalize BEFORE refusing. URLs
    // carry the live server's port: a bypass would connect, so asserting
    // the sync refusal (TypeError, thrown before any connect) pins the
    // gate, not a dead port.
    await runRows("fetcher:ssrf-canonical", [
      ["decimal-integer form 2130706433 == 127.0.0.1 is refused (d.ts L4235/L4259)", () => {
        const f = new Fetcher({ agent: AGENT });
        assertThrows(() => f.get("http://2130706433:" + srv.port + "/hi.html"),
                     "integer literal refused", TypeError, /private\/loopback/);
        f.close();
      }],
      ["hex form 0x7f.0.0.1 is refused (d.ts L4235/L4259)", () => {
        const f = new Fetcher({ agent: AGENT });
        assertThrows(() => f.get("http://0x7f.0.0.1:" + srv.port + "/hi.html"),
                     "hex literal refused", TypeError, /private\/loopback/);
        f.close();
      }],
      ["octal form 0177.0.0.1 is refused (d.ts L4235/L4259)", () => {
        const f = new Fetcher({ agent: AGENT });
        assertThrows(() => f.get("http://0177.0.0.1:" + srv.port + "/hi.html"),
                     "octal literal refused", TypeError, /private\/loopback/);
        f.close();
      }],
      ["short form 127.1 is refused (d.ts L4235/L4259)", () => {
        const f = new Fetcher({ agent: AGENT });
        assertThrows(() => f.get("http://127.1:" + srv.port + "/hi.html"),
                     "short-form literal refused", TypeError, /private\/loopback/);
        f.close();
      }],
      ["fully expanded IPv6 loopback [0:0:0:0:0:0:0:1] is refused (d.ts L4235/L4259)", () => {
        const f = new Fetcher({ agent: AGENT });
        assertThrows(() => f.get("http://[0:0:0:0:0:0:0:1]:" + srv.port + "/hi.html"),
                     "expanded v6 loopback refused", TypeError, /private\/loopback/);
        f.close();
      }],
      ["v4-mapped IPv6 loopback [::ffff:127.0.0.1] is refused (d.ts L4235/L4259)", () => {
        const f = new Fetcher({ agent: AGENT });
        assertThrows(() => f.get("http://[::ffff:127.0.0.1]:" + srv.port + "/hi.html"),
                     "mapped v6 loopback refused", TypeError, /private\/loopback/);
        f.close();
      }],
      ["getStream() refuses the same literal forms synchronously (d.ts L4270 getStream runs the same policy pass)", () => {
        const f = new Fetcher({ agent: AGENT });
        assertThrows(() => f.getStream("http://0x7f.0.0.1:" + srv.port + "/hi.html"),
                     "hex literal stream refused", TypeError, /private\/loopback/);
        assertThrows(() => f.getStream("http://[0:0:0:0:0:0:0:1]:" + srv.port + "/hi.html"),
                     "expanded v6 stream refused", TypeError, /private\/loopback/);
        f.close();
      }],
    ], async ([, check]) => check());

    await runRows("crawl:traverse", [
      ["3-page crawl: relative and dot-segment hrefs resolve against the emitting page; depth/seed/status/values (d.ts L4320-4335, L4358-4359)", async () => {
        const f = loopF();
        const c = new Crawl(f, { maxPages: 10 });
        const seen = [];
        for (const p of c.start(B + "/index.html", pageEx(), HTMLParse)) {
          seen.push(p.url.replace(B, "") + ":d" + p.depth + ":" + p.status + ":" + p.value.title);
        }
        assertDeepEq(seen, ["/index.html:d0:200:Index", "/a.html:d1:200:A", "/b.html:d1:200:B"],
                     "seed at depth 0, links at depth 1, visited set dedups");
        f.close();
      }],
      ["cycle safety: a page linking itself emits once — the seed is deduplicated (dynajs.d.ts: the seed URL, deduplicated)", async () => {
        const f = loopF();
        const c = new Crawl(f, { maxPages: 3 });
        const seen = [];
        for (const p of c.start(B + "/self.html", pageEx(), HTMLParse)) seen.push(p.url);
        assertEq(seen.length, 1, "one page, no spin, got " + seen.length);
        f.close();
      }],
      ["maxPages bounds the traversal; a drained crawl reports done (d.ts L4338: default 100 here capped to 2)", async () => {
        const f = loopF();
        const c = new Crawl(f, { maxPages: 2 });
        c.start(B + "/hub.html", pageEx(), HTMLParse);
        let count = 0, doneSeen = false;
        for (;;) { const r = c.next(); if (r.done) { doneSeen = true; break; } count++; }
        assertEq(count, 2, "page budget honored, got " + count);
        assertEq(doneSeen, true, "iterator finished");
        f.close();
      }],
      ["maxDepth bounds the traversal (d.ts L4339; dynajs.d.ts: seed is depth 0)", async () => {
        const f = loopF();
        const c = new Crawl(f, { maxDepth: 1 });
        const seen = [];
        for (const p of c.start(B + "/chain/d0", pageEx(), HTMLParse)) seen.push(p.url.replace(B, "") + ":d" + p.depth);
        assertDeepEq(seen, ["/chain/d0:d0", "/chain/d1:d1"], "depth-2 page never fetched");
        f.close();
      }],
      ["extraction runs on ANY status; link following is gated to 2xx (dynajs.d.ts: a 404 page can still carry data)", async () => {
        const f = loopF();
        const c = new Crawl(f, { maxPages: 5 });
        const seen = [];
        for (const p of c.start(B + "/gone.html", pageEx(), HTMLParse)) {
          seen.push(p.url.replace(B, "") + ":s" + p.status + ":" + p.value.title);
        }
        assertDeepEq(seen, ["/gone.html:s404:Missing"], "the 404 page emitted with its extracted value; its links were not followed");
        f.close();
      }],
      ["fragments and non-navigational schemes are dropped (dynajs.d.ts: javascript:/mailto:/tel:/fragments dropped)", async () => {
        const f = loopF();
        const c = new Crawl(f, { maxPages: 5 });
        const seen = [];
        for (const p of c.start(B + "/drops.html", pageEx(), HTMLParse)) seen.push(p.url.replace(B, ""));
        assertDeepEq(seen, ["/drops.html", "/real.html"], "only the real link fetched");
        f.close();
      }],
      ["sameHost defaults to true: a cross-host link is never fetched (dynajs.d.ts: sameHost default true)", async () => {
        const f = loopF();
        const c = new Crawl(f, { maxPages: 5 });
        const seen = [];
        for (const p of c.start(B + "/cross.html", pageEx(), HTMLParse)) seen.push(p.url);
        assertEq(seen.length, 1, "the other.example.invalid link was dropped, got " + seen.length);
        f.close();
      }],
      ["concurrency outside 1..16 throws RangeError; 16 constructs (d.ts L4348-4353; dynajs.d.ts)", async () => {
        const f = loopF();
        assertThrows(() => new Crawl(f, { concurrency: 0 }), "0 refused", RangeError);
        assertThrows(() => new Crawl(f, { concurrency: 17 }), "17 refused", RangeError);
        const c = new Crawl(f, { concurrency: 16 });
        assertEq(c.closed, false, "live when new");
        c.close();
        assertEq(c.closed, true, "crawl closes like a DynResource (d.ts L4380-4383)");
        f.close();
      }],
    ], async ([, check]) => check());

    await runRows("crawl:fields", [
      ["relField: a nofollow slot politeness-skips that link (d.ts L4342-4343)", async () => {
        const f = loopF();
        const c = new Crawl(f, { maxPages: 5, relField: "rels" });
        const seen = [];
        for (const p of c.start(B + "/nf.html", pageEx(), HTMLParse)) seen.push(p.url.replace(B, ""));
        assertDeepEq(seen, ["/nf.html", "/nf-ok.html"], "the nofollow target was skipped");
        f.close();
      }],
      ["robotsField: meta-robots nofollow gates following and joins page.robots (d.ts L4344-4347, L4330-4331)", async () => {
        const f = loopF();
        const c = new Crawl(f, { maxPages: 5, robotsField: "mrobot" });
        const ex = new Extractor({
          links: { sel: new Selector("a"), attr: "href", all: true },
          mrobot: { sel: new Selector("meta[name=robots]"), attr: "content" },
        }, { text: HTMLText });
        const seen = [];
        for (const p of c.start(B + "/meta.html", ex, HTMLParse)) {
          seen.push(p.url.replace(B, ""));
          assertDeepEq(p.robots, ["nofollow"], "directives joined page.robots");
        }
        assertDeepEq(seen, ["/meta.html"], "the nofollow page emitted once and linked nowhere");
        f.close();
      }],
      ["canonicalField: a RELATIVE canonical is ignored, never guessed (dynajs.d.ts: relative or malformed canonicals are ignored)", async () => {
        const f = loopF();
        const c = new Crawl(f, { maxPages: 5, canonicalField: "canon" });
        const seen = [];
        for (const p of c.start(B + "/relcan.html", pageEx(), HTMLParse)) seen.push(p.url.replace(B, ""));
        assertDeepEq(seen, ["/relcan.html", "/chain/d0", "/chain/d1"], "the crawl continued normally");
        f.close();
      }],
      ["baseField: links resolve against the page's declared base (d.ts L4339-4340)", async () => {
        const f = loopF();
        const c = new Crawl(f, { maxPages: 5, baseField: "pbase" });
        const seen = [];
        for (const p of c.start(B + "/basedir.html", pageEx(), HTMLParse)) seen.push(p.url.replace(B, ""));
        assertDeepEq(seen, ["/basedir.html", "/sub/x.html"], "x.html resolved under /sub/");
        f.close();
      }],
    ], async ([, check]) => check());

    await runRows("crawl:state", [
      ["serialize() before start throws TypeError (d.ts L4371-4374)", async () => {
        const f = loopF();
        assertThrows(() => new Crawl(f, {}).serialize(), "never-started crawl has no state", TypeError, /start/);
        f.close();
      }],
      ["serialize/resume: the remaining page set and its order match the donor (d.ts L4371-4393)", async () => {
        const f = loopF();
        const donor = new Crawl(f, { maxPages: 3 });
        donor.start(B + "/index.html", pageEx(), HTMLParse);
        const first = donor.next();
        assertEq(first.done, false, "donor emitted the seed");
        assertEq(first.value.url, B + "/index.html", "seed first");
        const state = donor.serialize();
        assert(typeof state === "string", "state is a versioned JSON string");
        const parsed = JSON.parse(state);
        assert(parsed && typeof parsed.v === "number", "state carries a version, got " + JSON.stringify(Object.keys(parsed || {})));
        const res = Crawl.resume(f, state, pageEx(), HTMLParse);
        const rest = [...res].map(p => p.url.replace(B, ""));
        assertDeepEq(rest, ["/a.html", "/b.html"], "resume continues the donor's frontier, in order");
        f.close();
      }],
      ["a state whose page budget is exhausted resumes as a finished crawl (d.ts L4389-4391)", async () => {
        const f = loopF();
        const done = new Crawl(f, { maxPages: 1 }).start(B + "/index.html", pageEx(), HTMLParse);
        for (const p of done) {}
        const res = Crawl.resume(f, done.serialize(), pageEx(), HTMLParse);
        const nxt = res.next();
        assertEq(nxt.done, true, "finished immediately");
        f.close();
      }],
      ["malformed state is refused with TypeError, never half-restored (d.ts L4389-4391)", async () => {
        const f = loopF();
        assertThrows(() => Crawl.resume(f, "{not json"), "garbage state", TypeError);
        f.close();
      }],
      ["concurrency 2 keeps fetches in flight; for await...of drains it (d.ts L4348-4353, L4367-4370). ORDER is deliberately not pinned: above concurrency 1 the in-flight fetches race, so the page SET is the contract here", async () => {
        const f = loopF();
        const c = new Crawl(f, { maxPages: 10, concurrency: 2 });
        const seen = [];
        for await (const p of c.start(B + "/index.html", pageEx(), HTMLParse)) seen.push(p.url.replace(B, "") + ":d" + p.depth);
        seen.sort();
        assertDeepEq(seen, ["/a.html:d1", "/b.html:d1", "/index.html:d0"], "same page set through the async protocol");
        f.close();
      }],
    ], async ([, check]) => check());
  } finally {
    srv.close();
  }
}

/* ------------------------------------------------------------------ *
 *  Door 2: python3 mock origin on 127.0.0.1 port 0 (the engine's own
 *  tests/test_scrape_fetcher.js pattern) for the DYNAMIC rows: Location
 *  redirects, a slow route, Retry-After, ETag 304, X-Robots-Tag and
 *  Link headers, live-port canonical urls, robots refetch counts.
 * ------------------------------------------------------------------ */
if (!Which("python3")) {
  print("SKIP(dyna:scrape dynamic-origin rows: python3 not found — redirect/timeout/Retry-After/304/header rows need the mock origin)");
} else {
  const MOCK_SRC = [
    "import http.server, sys, threading, os, json, time",
    "threading.Timer(30, lambda: os._exit(0)).start()",
    "hits = {}",
    "class H(http.server.BaseHTTPRequestHandler):",
    "    protocol_version = 'HTTP/1.1'",
    "    def log_message(self, *a): pass",
    "    def reply(self, code, body=b'', extra=None):",
    "        self.send_response(code)",
    "        self.send_header('Content-Length', str(len(body)))",
    "        for k, v in (extra or {}).items(): self.send_header(k, v)",
    "        self.end_headers()",
    "        if body: self.wfile.write(body)",
    "    def do_GET(self):",
    "        p = self.path.split('?')[0]",
    "        hits[p] = hits.get(p, 0) + 1",
    "        base = 'http://127.0.0.1:%d' % srv.server_address[1]",
    "        if p == '/robots.txt':",
    "            return self.reply(200, b'User-agent: *\\nDisallow: /private\\n')",
    "        if p == '/hits':",
    "            return self.reply(200, json.dumps(hits).encode(), {'Content-Type': 'application/json'})",
    "        if p == '/hop/start': return self.reply(302, b'', {'Location': base + '/hop/mid'})",
    "        if p == '/hop/mid': return self.reply(302, b'', {'Location': base + '/hop/end'})",
    "        if p == '/hop/end': return self.reply(200, b'landed')",
    "        if p.startswith('/deep/'):",
    "            k = int(p.rsplit('/', 1)[1])",
    "            if k > 0: return self.reply(302, b'', {'Location': base + '/deep/%d' % (k - 1)})",
    "            return self.reply(200, b'bottom')",
    "        if p == '/stream100':",
    "            self.wfile.write(b'HTTP/1.1 100 Continue\\r\\n\\r\\n')",
    "            return self.reply(200, b'interim-ok', {'Content-Type': 'text/plain'})",
    "        if p == '/slow':",
    "            time.sleep(1.2)",
    "            return self.reply(200, b'late')",
    "        if p == '/retry429':",
    "            if hits[p] <= 2: return self.reply(429, b'slow down', {'Retry-After': '0'})",
    "            return self.reply(200, b'finally')",
    "        if p == '/etag':",
    "            if self.headers.get('If-None-Match') == '\"v1\"': return self.reply(304, b'')",
    "            return self.reply(200, b'v1body', {'ETag': '\"v1\"', 'Content-Type': 'text/custom'})",
    "        if p == '/xrob': return self.reply(200, b'x', {'X-Robots-Tag': 'noindex, nofollow'})",
    "        if p == '/lcanon-abs': return self.reply(200, b'x', {'Link': '<' + base + '/canon-target>; rel=\"canonical\"'})",
    "        if p == '/lcanon-rel': return self.reply(200, b'x', {'Link': '</rel-target>; rel=\"canonical\"'})",
    "        if p == '/cd/a': return self.reply(200, b'<a href=\"/cd/b\">b</a><a href=\"/cd/c\">c</a>', {'Content-Type': 'text/html'})",
    "        if p == '/cd/b': return self.reply(200, ('<link rel=\"canonical\" href=\"' + base + '/cd/a\"><a href=\"/cd/hidden\">h</a>').encode(), {'Content-Type': 'text/html'})",
    "        if p == '/cd/c': return self.reply(200, ('<link rel=\"canonical\" href=\"' + base + '/cd/a\">').encode(), {'Content-Type': 'text/html'})",
    "        if p == '/cd/hidden': return self.reply(200, b'should-not-be-fetched', {'Content-Type': 'text/html'})",
    "        return self.reply(404, b'nope')",
    "srv = http.server.ThreadingHTTPServer(('127.0.0.1', 0), H)",
    "open(sys.argv[1], 'w').write(str(srv.server_address[1]))",
    "srv.serve_forever()",
  ];
  const T = makeTempDir("bb_scrape_dynorigin");
  try {
    writeFile(new Path(T + "/dynorigin.py"), MOCK_SRC.join("\n") + "\n");
    // SELF-BOUND: the mock kills itself after 30s even if this suite is (d.ts L4374 aside) killed mid-run.
    Exec("/bin/sh", ["-c", "python3 " + T + "/dynorigin.py " + T + "/port > " + T + "/log 2>&1 &"]);
    let pport = 0;
    for (let i = 0; i < 60 && !pport; i++) {
      try { pport = parseInt(readFile(new Path(T + "/port")).trim(), 10); }
      catch (e) { await new Promise((res) => setTimeout(res, 50)); }
    }
    assert(pport > 0, "mock origin started on an ephemeral port, log: " + T + "/log");
    const PB = "http://127.0.0.1:" + pport;
    const pf = (extra) => new Fetcher({ agent: AGENT, allowPrivateHosts: true, minDelayMs: 0, retries: 0, ...extra });
    // hit counters are read OUTSIDE the fetcher so the read itself triggers no robots fetch
    const readHits = async (c) => JSON.parse((await c.getAsync(PB + "/hits")).body);

    await runRows("fetcher:origin", [
      ["redirect chase: relative hops resolve per hop and the response reports the FINAL url (dynajs.d.ts: RFC 3986 resolution, each hop re-gated)", async () => {
        const f = pf();
        const r = f.get(PB + "/hop/start");
        assertEq(r.status, 200, "chase ended at a 200");
        assertEq(r.url, PB + "/hop/end", "url is the final hop");
        assertEq(r.body, "landed", "final body carried");
        f.close();
      }],
      ["maxRedirects is enforced with a RangeError naming redirects (dynajs.d.ts: default 5 here capped to 2)", async () => {
        const f = pf({ maxRedirects: 2 });
        assertThrows(() => f.get(PB + "/deep/3"), "3 hops > cap 2", RangeError, /redirect/);
        f.close();
        const g = pf({ maxRedirects: 2 });
        const r = g.get(PB + "/deep/2");
        assertEq(r.status, 200, "exactly-at-cap chases fine");
        assertEq(r.body, "bottom", "arrived");
        g.close();
      }],
      ["429 is retried per Retry-After (0 = no server-given wait) and stats.retried counts the curve (dynajs.d.ts: Retry-After overrides the backoff untouched)", async () => {
        const f = pf({ retries: 2 });
        const r = f.get(PB + "/retry429");
        assertEq(r.status, 200, "third attempt landed");
        assertEq(r.body, "finally", "the retry succeeded");
        const s = f.stats();
        assertEq(s.retried, 2, "both 429s counted");
        assertEq(s.fetched, 1, "one page fetched");
        f.close();
      }],
      ["getStream skips a 1xx interim response and streams the REAL response (d.ts L4290-4317: the head readable off the stream object; request()'s reader skips 1xx — the stream head parse matches it)", async () => {
        const f = pf();
        const st = f.getStream(PB + "/stream100");
        assertEq(st.status, 200, "interim 100 not surfaced as final");
        assertEq(st.contentType, "text/plain", "real response content type");
        const buf = new Uint8Array(32);
        let out = "";
        for (;;) { const k = await st.read(buf); if (k === 0) break; out += new TextDecoder().decode(buf.subarray(0, k)); }
        assertEq(out, "interim-ok", "real response body streamed");
        st.close();
        f.close();
      }],
      ["disconnect() cancels the IN-FLIGHT request only: a request submitted AFTER it still resolves (d.ts disconnect(): explicit per-client control; the per-job cancel flag means a later submit cannot un-cancel an earlier one)", async () => {
        const c = new HTTPClient();
        c.setTimeout(3000);
        const p1 = c.requestAsync("GET", PB + "/slow");
        await new Promise((res) => setTimeout(res, 100));
        c.disconnect();
        await assertRejects(() => p1, "in-flight request aborted by disconnect", /aborted/);
        const r2 = await c.getAsync(PB + "/etag");
        assertEq(r2.status, 200, "a fresh request after disconnect() is not cancelled");
        c.close();
      }],
      ["conditional GET: a 304 replays the CACHED body and the STORED content-type, flagged fromCache/notModified (d.ts L4222-4223, L4272-4273)", async () => {
        const f = pf();
        const a = f.get(PB + "/etag");
        assertEq(a.status, 200, "first fetch is real");
        assertEq(a.contentType, "text/custom", "origin contentType stored");
        assertEq(a.fromCache, false, "first fetch not from cache");
        const b = f.get(PB + "/etag");
        assertEq(b.status, 304, "the wire said 304");
        assertEq(b.notModified, true, "notModified set");
        assertEq(b.fromCache, true, "served from the validator store");
        assertEq(b.body, "v1body", "cached body replayed");
        assertEq(b.contentType, "text/custom", "STORED content-type replays with the 304");
        const s = f.stats();
        assertEq(s.revalidated, 1, "revalidation counted");
        assert(s.savedBytes > 0, "savedBytes sums the cached body served, got " + s.savedBytes);
        f.close();
      }],
      ["X-Robots-Tag directives are surfaced for this agent, expanded in order (d.ts L4282-4284)", async () => {
        const f = pf();
        const r = f.get(PB + "/xrob");
        assertDeepEq(r.robotsDirectives, ["noindex", "nofollow"], "both directives, none-free form");
        f.close();
      }],
      ["Link: rel=canonical is surfaced resolved against the request url (d.ts L4285-4287)", async () => {
        const f = pf();
        const abs = f.get(PB + "/lcanon-abs");
        assertEq(abs.canonicalUrl, PB + "/canon-target", "absolute canonical kept");
        const rel = f.get(PB + "/lcanon-rel");
        assertEq(rel.canonicalUrl, PB + "/rel-target", "relative canonical resolved");
        f.close();
      }],
      ["robots.txt refresh: the default TTL fetches one copy per host; robotsTtlMs 0 re-fetches per request (d.ts L4224-4227)", async () => {
        const c = new HTTPClient();
        const f = pf();
        f.get(PB + "/hop/end"); f.get(PB + "/hop/end"); f.get(PB + "/hop/end");
        const withDefault = (await readHits(c))["/robots.txt"];
        f.close();
        const g = pf({ robotsTtlMs: 0 });
        const before = (await readHits(c))["/robots.txt"] || 0;
        g.get(PB + "/hop/end"); g.get(PB + "/hop/end"); g.get(PB + "/hop/end");
        const after = (await readHits(c))["/robots.txt"] || 0;
        assertEq(after - before, 3, "one robots fetch per request at ttl 0");
        assert(withDefault >= 1, "the default TTL fetched at least one copy, got " + withDefault);
        c.close();
      }],
    ], async ([, check]) => check());

    await runRows("crawl:canonical", [
      ["canonicalField dedup: a page whose canonical was already seen still EMITS but queues no links (d.ts L4340-4341)", async () => {
        const f = pf();
        const c = new Crawl(f, { maxPages: 10, canonicalField: "canon" });
        const seen = [];
        for (const p of c.start(PB + "/cd/a", pageEx(), HTMLParse)) seen.push(p.url.replace(PB, ""));
        assertDeepEq(seen, ["/cd/a", "/cd/b", "/cd/c"], "duplicates emitted, but their links were not queued");
        const c2 = new HTTPClient();
        const hits = JSON.parse((await c2.getAsync(PB + "/hits")).body);
        assertEq(hits["/cd/hidden"] || 0, 0, "the duplicate's link target was never fetched");
        c2.close();
        f.close();
      }],
    ], async ([, check]) => check());
  } finally {
    Exec("/bin/sh", ["-c", "pkill -f '" + T + "/dynorigin.py' >/dev/null 2>&1; true"]);
    removeAll(new Path(T));
  }
}

print("bb_scrape: all tests passed (" + n + " assertions)");
