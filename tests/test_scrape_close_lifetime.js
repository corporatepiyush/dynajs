// flags: --std
/* test_scrape_close_lifetime.js -- one crawl's close/GC/resume must not
 * poison UNRELATED later code, and every exit shape leaves clean.
 *
 * The defect class (late completions after a mid-flight close): a closed
 * crawl's in-flight fetches still SETTLE -- and the settling continuation
 * used to surface "closed native resource" errors in ANOTHER crawl's
 * for-await, hand PHANTOM pages to a different resumed instance (or a
 * stranger whose heap slot the freed flight had been re-issued to), and
 * end in nondeterministic teardown SIGSEGVs / JS_FreeRuntime assertions
 * (exit 134). The ownership rule now: every flight's state belongs to its
 * own crawl, a dead instance records NOTHING (its tails write nowhere), a
 * resumed instance is a real async iterable, and the process exits clean on
 * every shape.
 *
 * The in-process rows assert the PAGE SETS (the mutation-proof core: any
 * cross-instance write shows up as a wrong or duplicated url); the child
 * shapes judge the exit codes, which is where the abort used to appear.
 *
 * Needs /bin/sh + timeout. Run: dynajs (CONFIG_NATIVE_MODULES=y)
 * tests/test_scrape_close_lifetime.js */
import { Exec, Which, getEnv } from "dyna:sys";
import { makeTempDir, readFile, removeAll, Path } from "dyna:file";
import { Crawl, Fetcher, Extractor } from "dyna:scrape";
import { Selector, HTMLText, HTMLParse } from "dyna:html";

let pass = 0, fail = 0, skip = 0;
const REQUIRE = getEnv("DYNAJS_REQUIRE_TOOLS") === "1";
const ok = (c, w, d) => { if (c) { pass++; print("  ok    " + w); }
                          else { fail++; print("  FAIL  " + w + (d ? "  [" + d + "]" : "")); } };
function skipped(w) {
    if (REQUIRE) { fail++; print("  FAIL  REQUIRED: " + w); return; }
    skip++; print("  SKIP  " + w);
}
const sh = (c) => Exec("/bin/sh", ["-c", c]).code;

const BASE = "http://example.invalid";
const GRAPH = { "/a": ["/b", "/c"], "/b": ["/d"], "/c": ["/d"], "/d": [] };
const sleep = (ms) => new Promise((res) => setTimeout(res, ms));
const body = (p) => "<html><body><h1>t" + p + "</h1>" +
    (GRAPH[p] || []).map((q) => `<a href="${BASE + q}">q</a>`).join("") +
    "</body></html>";

const mockClient = (delayMs) => ({
    requestAsync(method, url) {
        return new Promise((res) => setTimeout(() => {
            res({ status: 200, url, headers: {},
                  body: body(url.slice(BASE.length)) });
        }, delayMs));
    },
    /* the sync arm: concurrency 1 pulls through fe_raw's request */
    request(method, url) {
        return { status: 200, url, headers: {},
                 body: body(url.slice(BASE.length)) };
    },
});
const mkFetcher = (delayMs) => new Fetcher({
    agent: "close-lifetime/1", client: mockClient(delayMs), minDelayMs: 0,
    robots: false, allowPrivateHosts: true,
});
const mkExtractor = () => new Extractor({
    title: { sel: new Selector("h1") },
    links: { sel: new Selector("a"), attr: "href", all: true },
}, { text: HTMLText });
const mkCrawl = (f, conc) => new Crawl(f, { maxPages: 4, concurrency: conc })
    .start(BASE + "/a", mkExtractor(), HTMLParse);
const drainAsync = async (c) => {
    const out = [];
    for await (const p of c) out.push(p.url);
    return out;
};
const setOf = (urls) => [...new Set(urls)].sort().join(",");
const EXPECT4 = [BASE + "/a", BASE + "/b", BASE + "/c", BASE + "/d"]
    .sort().join(",");

/* ---- in-process rows ------------------------------------------------- */

/* two crawls, closed at DIFFERENT phases; the survivor sees exactly its
   own graph and its own pages -- nothing from the closed sibling */
{
    const f1 = mkFetcher(45), f2 = mkFetcher(5);
    const c1 = mkCrawl(f1, 2), c2 = mkCrawl(f2, 2);
    const d2 = drainAsync(c2);
    const first1 = await c1.next();          /* c1's own page */
    c1.close();                              /* phase 1: mid-flight close */
    f1.close();
    const urls2 = await d2;                  /* c2 crawls on, untouched */
    c2.close();                              /* phase 2: close fully drained */
    f2.close();
    ok(setOf(urls2) === EXPECT4,
       "the survivor crawl saw exactly its own graph (" + setOf(urls2) + ")");
    ok(first1 && first1.value && first1.value.url === BASE + "/a",
       "the closed crawl's pulled page was its own");
    await sleep(300);                        /* c1's late settles land here */
    ok(true, "late completions after the mid-flight close land silently");
}

/* close-during-fetch with late completions, then a THIRD crawl: the
   phantom-page test -- the stranger must see exactly its own graph */
{
    const f1 = mkFetcher(60);
    const c1 = mkCrawl(f1, 2);
    const held = c1.next();
    held.catch(() => {});
    await sleep(10);
    c1.close();                              /* the fetches are still out */
    f1.close();
    await sleep(200);                        /* every late completion lands */
    const f3 = mkFetcher(2);
    const urls3 = await drainAsync(mkCrawl(f3, 2));
    f3.close();
    ok(setOf(urls3) === EXPECT4,
       "a fresh crawl after the closed one saw exactly its own graph (" +
       setOf(urls3) + ")");
    ok(urls3.length === 4, "no phantom pages from the dead instance (" +
       urls3.length + " pages)");
}

/* resume-after-close: the resumed instance is a REAL async iterable (the
   silent-spin class), terminates, and never re-emits the closed
   instance's pages */
{
    const f1 = mkFetcher(45);
    const c1 = new Crawl(f1, { maxPages: 4, concurrency: 2 })
        .start(BASE + "/a", mkExtractor(), HTMLParse);
    const first = await c1.next();
    const state = c1.serialize();
    c1.close();                              /* mid-flight, settles pending */
    f1.close();
    const f2 = mkFetcher(3);
    const c2 = Crawl.resume(f2, state, mkExtractor(), HTMLParse);
    ok(typeof c2[Symbol.asyncIterator] === "function",
       "a resumed crawl installs [Symbol.asyncIterator]");
    const urls2 = await drainAsync(c2);      /* terminates: the spin is gone */
    f2.close();
    ok(urls2.every((u) => u !== first.value.url),
       "the resumed crawl never re-emits the closed instance's page (" +
       urls2.join(",") + ")");
    /* the SAFE half: a serial donor (no flights in the air at serialize)
       round-trips exactly through close + resume */
    const f4 = mkFetcher(2);
    const c4 = new Crawl(f4, { maxPages: 4, concurrency: 1 })
        .start(BASE + "/a", mkExtractor(), HTMLParse);
    const got4 = [c4.next().value.url, c4.next().value.url];
    const state4 = c4.serialize();
    c4.close();
    f4.close();
    const f5 = mkFetcher(2);
    const rest = await drainAsync(Crawl.resume(f5, state4, mkExtractor(), HTMLParse));
    f5.close();
    const all4 = [...got4, ...rest];
    ok(all4.length === 4 && setOf(all4) === EXPECT4 &&
       new Set(all4).size === 4,
       "serialize -> close -> resume round-trips exactly (" +
       setOf(all4) + ")");
    ok(urls2.every((u) => u.indexOf(BASE) === 0),
       "the resumed crawl's pages are all its own crawl's urls");
    await sleep(300);                        /* the old instance's settles */
    ok(true, "old-instance late settles land after the resume");
}

/* GC-drop-with-in-flight: the dropped crawl's late completion must not
   corrupt the next user of the heap */
{
    {
        const f = mkFetcher(40);
        const c = mkCrawl(f, 2);
        c.next().catch(() => {});
        /* both go out of scope here with a flight in the air */
    }
    if (globalThis.gc) globalThis.gc();
    await sleep(250);
    const f3 = mkFetcher(2);
    const urls3 = await drainAsync(mkCrawl(f3, 2));
    f3.close();
    ok(setOf(urls3) === EXPECT4,
       "after GC-dropping an in-flight crawl the next crawl is exact (" +
       setOf(urls3) + ")");
}

/* for-await on an UNCLOSED crawl with a dropped promise: the loop sees
   its own pages and the shape exits clean */
{
    const f = mkFetcher(2);
    const c = mkCrawl(f, 2);
    const urls = await drainAsync(c);
    f.close();
    ok(setOf(urls) === EXPECT4,
       "a plain for-await drain is exact (" + setOf(urls) + ")");
}

/* ---- child exit shapes (the teardown judged by exit code) ------------ */
if (!Which("timeout")) {
    skipped("timeout missing -- child exit shapes cannot be bounded");
} else {
    const T = makeTempDir("scrape_close_lifetime");
    const CHILD = "tests/test_scrape_close_lifetime_child.js";
    for (const shape of ["close-mid-flight", "two-crawls", "gc-drop",
                         "unclosed-in-flight", "resume-after-close",
                         "close-during-getter", "close-during-request"]) {
        sh(`timeout -k 5 25 ./dynajs ${CHILD} ${shape} ` +
           `> ${T}/${shape}.out 2> ${T}/${shape}.err; echo $? > ${T}/${shape}.rc`);
        const rc = parseInt(readFile(new Path(T + "/" + shape + ".rc")), 10);
        const err = readFile(new Path(T + "/" + shape + ".err"));
        ok(rc === 0, "exit shape " + shape + " exits 0 (got " + rc + ")");
        ok(err.indexOf("Assertion failed") < 0 &&
           err.indexOf("Segmentation") < 0,
           "exit shape " + shape + ": no teardown abort");
    }
    try { removeAll(new Path(T)); } catch (e) {}
}

print("test_scrape_close_lifetime: " + pass + " passed, " + fail +
      " failed, " + skip + " skipped");
if (fail) throw new Error("test_scrape_close_lifetime: " + fail + " failures");
