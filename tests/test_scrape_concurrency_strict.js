// flags: --std
import { Crawl, Fetcher, Extractor } from "dyna:scrape";
import { Selector, HTMLText, HTMLParse } from "dyna:html";

let n = 0, bad = 0;
function ok(c, w, d) {
    if (c) { n++; print("  ok    " + w); }
    else { bad++; print("  FAIL  " + w + (d ? "  [" + d + "]" : "")); }
}

const mockClient = () => ({
    request(method, url, body, headers) {
        return {
            status: 200,
            url,
            headers: {},
            body: "<html><body><h1>seed</h1></body></html>",
        };
    },
});
const mkFetcher = () => new Fetcher({
    agent: "conc-strict/1.0", client: mockClient(), minDelayMs: 0,
    robots: false, allowPrivateHosts: true,
});

function attempt(f, opts) {
    let c = null;
    try {
        c = new Crawl(f, opts);
    } catch (e) {
        const name = (e && e.constructor && e.constructor.name) ||
                     typeof e;
        return { kind: name, msg: String(e && e.message) };
    }
    c.close();
    return { kind: "ok", msg: "" };
}

function observed(f, opts) {
    const c = new Crawl(f, Object.assign({ maxPages: 3, maxDepth: 0 }, opts));
    c.start("http://127.0.0.1:1/seed", new Extractor({
        title: { sel: new Selector("h1") },
    }, { text: HTMLText }), HTMLParse);
    const env = JSON.parse(c.serialize());
    c.close();
    return env.concurrency;
}

{
    const f = mkFetcher();
    for (const v of [1, 2, 8, 15, 16]) {
        const r = attempt(f, { concurrency: v });
        ok(r.kind === "ok", "integer " + v + " accepted",
           r.kind + ": " + r.msg);
    }
    {
        const r = attempt(f, { concurrency: 15.0 });
        ok(r.kind === "ok", "15.0 (integral float) accepted",
           r.kind + ": " + r.msg);
    }
    for (const opts of [{}, { concurrency: undefined }, { concurrency: null }]) {
        const r = attempt(f, opts);
        ok(r.kind === "ok", JSON.stringify(opts) + " accepted (default)",
           r.kind + ": " + r.msg);
    }
    f.close();
}

{
    const f = mkFetcher();
    for (const v of [0, -1, 17, 100, -100, 2147483647, -2147483648, 3e9,
                     1e300]) {
        const r = attempt(f, { concurrency: v });
        ok(r.kind === "RangeError",
           "integer " + v + " is a RangeError, not " + r.kind,
           r.msg);
        ok(/between 1 and 16/.test(r.msg),
           "RangeError for " + v + " names the 1..16 range [" + r.msg + "]");
    }
    f.close();
}

{
    const f = mkFetcher();
    const rows = [
        ["2.5 (the truncating case)", 2.5],
        ["1.5", 1.5],
        ["16.5", 16.5],
        ["-0.5", -0.5],
        ['"3" (the coercing case)', "3"],
        ['"" ', ""],
        ['"abc"', "abc"],
        ["true", true],
        ["false", false],
        ["NaN", NaN],
        ["Infinity", Infinity],
        ["-Infinity", -Infinity],
        ["a plain object", {}],
        ["an array", []],
        ["a function", () => 3],
    ];
    for (const [label, v] of rows) {
        const r = attempt(f, { concurrency: v });
        ok(r.kind === "TypeError",
           label + " is a TypeError, not " + r.kind, r.msg);
        ok(/concurrency/.test(r.msg) && /integer/.test(r.msg),
           "TypeError for " + label + " names the option and the type " +
           "[" + r.msg + "]");
    }
    {
        const r = attempt(f, { concurrency: 3n });
        ok(r.kind === "TypeError", "3n (bigint) is a TypeError, not " + r.kind,
           r.msg);
    }
    f.close();
}

{
    const f = mkFetcher();
    const opts = {};
    Object.defineProperty(opts, "concurrency", {
        get() { throw new Error("sentinel getter boom"); },
    });
    const r = attempt(f, opts);
    ok(r.kind === "Error" && /sentinel getter boom/.test(r.msg),
       "a throwing concurrency getter propagates its own error [" + r.msg + "]");
    f.close();
}

{
    const f = mkFetcher();
    ok(observed(f, {}) === 1, "absent concurrency observes as 1",
       String(observed(f, {})));
    ok(observed(f, { concurrency: undefined }) === 1,
       "undefined concurrency observes as 1");
    ok(observed(f, { concurrency: null }) === 1,
       "null concurrency observes as 1");
    ok(observed(f, { concurrency: 16 }) === 16,
       "16 observes as 16 (no clamp below the bound)");
    ok(observed(f, { concurrency: 1 }) === 1,
       "1 observes as 1");
    ok(observed(f, { concurrency: 15.0 }) === 15,
       "15.0 observes as 15 (integral float, no truncation artifact)");
    ok(observed(f, { concurrency: 2.0 }) === 2,
       "2.0 observes as 2");
    f.close();
}

console.log("test_scrape_concurrency_strict.js: " + n + " assertions passed" +
            (bad ? ", " + bad + " FAILED" : ""));
if (bad) throw new Error("test_scrape_concurrency_strict.js: " + bad +
                         " failures");
