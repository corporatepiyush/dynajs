// flags: --std
/* test_globals.js -- the globals parity pack (+ ).
 *
 * `URL`, `URLSearchParams`, `sleep` and `structuredClone` resolve without an
 * import. The first two (and the last) must be THE SAME objects the dyna:*
 * modules export -- a copy would satisfy typeof checks and still be the
 * wrong feature (instanceof against the module class would fail, and the
 * two spellings of one API would drift).
 *
 * Absence tolerance: on a build without the native modules this file must
 * SKIP, not fail (feature-detect, early return). A build that HAS the
 * modules but lost the globals is a regression and fails.
 *
 * Run: dynajs (built with CONFIG_NATIVE_MODULES=y) tests/test_globals.js
 */
import * as os from "os";
import * as std from "std";

let n = 0, fails = 0;
function ok(c, msg) { n++; if (!c) { fails++; print("FAIL: " + msg); } }
function eq(a, b, msg) {
    ok(a === b, msg + " (got " + JSON.stringify(a) + ", want " + JSON.stringify(b) + ")");
}
function throws(fn, msg) {
    let t = false;
    try { fn(); } catch (e) { t = true; }
    ok(t, msg);
}

/* Run `p` under a watchdog so a broken sleep/worker fails the suite instead
 * of hanging the parallel runner. */
function withTimeout(p, ms, label) {
    let tid;
    const watchdog = new Promise((_, rej) => {
        tid = setTimeout(() => rej(new Error(label + ": no result within " + ms + "ms")), ms);
    });
    return Promise.race([p, watchdog]).finally(() => clearTimeout(tid));
}

/* ---------------------------------------------------- absence tolerance */

let urlMod = null, serMod = null;
try { urlMod = await import("dyna:url"); } catch (e) { urlMod = null; }
try { serMod = await import("dyna:serialize"); } catch (e) { serMod = null; }

if (!urlMod && !serMod) {
    /* Non-native build: neither module nor globals exist. Nothing to test. */
    print("test_globals: SKIP (dyna:url and dyna:serialize not built in)");
} else {

/* A module present but its global missing is a REGRESSION, never a skip. */
if (urlMod) {
    ok(typeof URL === "function", "URL is global when dyna:url builds in");
    ok(typeof URLSearchParams === "function", "URLSearchParams is global when dyna:url builds in");
}
if (serMod) {
    ok(typeof structuredClone === "function", "structuredClone is global when dyna:serialize builds in");
}
if (!urlMod) print("test_globals: URL/URLSearchParams checks skipped (no dyna:url)");
if (!serMod) print("test_globals: structuredClone checks skipped (no dyna:serialize)");

/* -------------------------------------------------------- sleep */

ok(typeof sleep === "function", "sleep is a global function");
eq(sleep.length, 1, "sleep declares one parameter");

{
    /* Resolves, and not before it should (Date-based, watchdog-wrapped). */
    const t0 = Date.now();
    await withTimeout(sleep(40), 5000, "sleep(40)");
    const dt = Date.now() - t0;
    ok(dt >= 39, "sleep(40) waits at least its delay (elapsed " + dt + "ms)");
    ok(dt < 5000, "sleep(40) does not hang (elapsed " + dt + "ms)");
}
{
    /* The poll loop must keep servicing OTHER ready sources while a sleep
     * is pending: a 10ms timer must fire inside a 60ms sleep window. */
    let fired = false;
    setTimeout(() => { fired = true; }, 10);
    await withTimeout(sleep(60), 5000, "sleep(60) with pending timer");
    ok(fired, "setTimeout fires while a sleep is pending (os_poll integration)");
}
{
    /* Concurrent sleeps resolve independently. */
    const t0 = Date.now();
    await withTimeout(Promise.all([sleep(20), sleep(35)]), 5000, "concurrent sleeps");
    ok(Date.now() - t0 >= 34, "concurrent sleeps both resolve (longest bounds the wait)");
}
eq(typeof (await withTimeout(sleep(0), 5000, "sleep(0)")), "undefined", "sleep(0) resolves");
await withTimeout(sleep(), 5000, "sleep()").then(
    () => ok(true, "sleep() with no argument resolves (treated as 0)"),
    (e) => ok(false, "sleep() with no argument resolves: " + e.message));
await withTimeout(sleep(-5), 5000, "sleep(-5)").then(
    () => ok(true, "sleep(-5) resolves (negative clamps to 0)"),
    (e) => ok(false, "sleep(-5) resolves: " + e.message));
ok(sleep(1) instanceof Promise, "sleep returns a Promise");
ok(sleep(1) !== sleep(1), "sleep returns a fresh Promise per call");

/* ------------------------------------------------ structuredClone */

if (serMod) {
    ok(structuredClone === serMod.structuredClone,
       "global structuredClone IS dyna:serialize.structuredClone (identity)");
    const sc = structuredClone;

    /* Plain nesting: a deep copy, not a reference. */
    {
        const v = { a: 1, b: [1, 2, { c: "x" }], d: null };
        const c = sc(v);
        ok(c !== v, "clone of an object is a new object");
        eq(JSON.stringify(c), JSON.stringify(v), "clone round-trips through JSON");
        ok(c.b[2] !== v.b[2], "nested objects are copied, not shared");
    }
    {
        const v = [1, [2, [3]]];
        const c = sc(v);
        ok(c !== v && c[1] !== v[1], "arrays and nested arrays are copied");
        eq(JSON.stringify(c), JSON.stringify(v), "nested array content matches");
    }
    ok(sc(42) === 42 && sc("x") === "x" && sc(null) === null && sc(undefined) === undefined && sc(true) === true,
       "primitives clone as themselves");
    eq(String(sc(42n)), "42", "bigint clones");
    eq(typeof sc(Symbol("s")), "symbol", "symbols clone");

    /* Dates: same time, distinct object. */
    {
        const d = new Date(1234567890);
        const c = sc(d);
        ok(c instanceof Date && c !== d && +c === +d, "Date clones to a distinct equal Date");
    }
    /* Map / Set / RegExp. */
    {
        const m = new Map([[1, "a"], ["b", 2]]);
        const c = sc(m);
        ok(c instanceof Map && c !== m && c.get(1) === "a" && c.get("b") === 2, "Map clones");
        const s = new Set([1, 2, 3]);
        const cs = sc(s);
        ok(cs instanceof Set && cs !== s && cs.has(3), "Set clones");
        const r = /ab+/gi;
        const cr = sc(r);
        ok(cr instanceof RegExp && cr !== r && cr.source === "ab+" && cr.flags === "gi", "RegExp clones with flags");
    }
    /* Typed arrays / ArrayBuffer: fresh buffer, same bytes. */
    {
        const t = new Uint8Array([1, 2, 3]);
        const c = sc(t);
        ok(c instanceof Uint8Array && c !== t && c.buffer !== t.buffer,
           "typed array clones into a fresh buffer");
        eq(c[0] + "," + c[1] + "," + c[2], "1,2,3", "typed array bytes survive");
    }
    {
        const ab = new ArrayBuffer(8);
        new Uint8Array(ab)[0] = 7;
        const c = sc(ab);
        ok(c instanceof ArrayBuffer && c !== ab && c.byteLength === 8 && new Uint8Array(c)[0] === 7,
           "ArrayBuffer clones with content");
    }
    /* Cycles: the copy's cycle points INTO the copy. */
    {
        const v = { name: "root" };
        v.self = v;
        const c = sc(v);
        ok(c.self === c && c !== v, "cyclic reference is preserved inside the copy");
    }
    /* Functions are structured-clone-forbidden, and the error is honest. */
    throws(() => sc(() => 1), "cloning a function throws");
    throws(() => sc(), "structuredClone() with no argument throws");
} else {
    print("test_globals: structuredClone checks skipped (no dyna:serialize)");
}

/* -------------------------------------------- URL / URLSearchParams */

if (urlMod) {
    ok(URL === urlMod.URL, "global URL IS dyna:url.URL (identity)");
    ok(URLSearchParams === urlMod.URLSearchParams, "global URLSearchParams IS dyna:url.URLSearchParams (identity)");

    /* instanceof against the GLOBAL constructor binds the module class. */
    const u = new URL("https://user:pw@example.com:8443/a/b?x=1&y=2#frag");
    ok(u instanceof URL, "instanceof URL (the global) holds");
    ok(u instanceof urlMod.URL, "instanceof dyna:url.URL holds for a global-made instance");

    /* Parse smoke: the component table from WHATWG/URL. */
    eq(u.protocol, "https:", "protocol");
    eq(u.username, "user", "username");
    eq(u.password, "pw", "password");
    eq(u.hostname, "example.com", "hostname");
    eq(u.port, "8443", "port");
    eq(u.host, "example.com:8443", "host");
    eq(u.pathname, "/a/b", "pathname");
    eq(u.search, "?x=1&y=2", "search");
    eq(u.hash, "#frag", "hash");
    eq(u.origin, "https://example.com:8443", "origin");
    eq(u.href, "https://user:pw@example.com:8443/a/b?x=1&y=2#frag", "href round-trips");
    eq(String(u), u.href, "toString is href");

    /* Relative resolution through the optional base argument. */
    eq(new URL("/p", "https://a.com/dir/page").href, "https://a.com/p", "relative resolution with base");
    eq(new URL("../up", "https://a.com/dir/page").href, "https://a.com/up", "dot-dot resolution with base");

    /* searchParams is live: writes go through to the URL's query slot. */
    eq(u.searchParams.get("x"), "1", "searchParams.get reads the URL's query");
    u.searchParams.set("x", "9");
    u.searchParams.append("z", "a b");
    eq(u.search, "?x=9&y=2&z=a+b", "searchParams writes through to u.search");
    eq(String(u), "https://user:pw@example.com:8443/a/b?x=9&y=2&z=a+b#frag", "toString reflects the mutation");
    ok(u.searchParams === u.searchParams, "searchParams is [SameObject]");

    /* Standalone URLSearchParams. */
    const sp = new URLSearchParams("a=1&b=%20x&a=3");
    eq(sp.get("a"), "1", "URLSearchParams.get returns the first value");
    eq(sp.get("b"), " x", "percent-encoded value decodes; + means space");
    eq(sp.getAll("a").join("|"), "1|3", "getAll returns every value");
    ok(sp.has("b") && !sp.has("nope"), "has");
    eq(sp.size, 3, "size counts pairs");
    sp.delete("a");
    ok(!sp.has("a"), "delete removes every pair of the name");
    sp.set("q", "1");
    eq(sp.toString(), "b=+x&q=1", "toString serializes; space is +");
    {
        const s2 = new URLSearchParams("b=2&a=1");
        s2.sort();                       /* sorts in place; returns undefined */
        eq([...s2].map((e) => e[0]).join(","), "a,b", "sort orders by name");
    }
    eq([...new URLSearchParams("a=1&b=2").keys()].join(","), "a,b", "keys() iterates");
    eq([...new URLSearchParams("a=1&b=2").entries()][0][1], "1", "entries() iterates pairs");
    {
        let seen = "";
        new URLSearchParams("a=1&b=2").forEach((v, k) => { seen += k + "=" + v + ";"; });
        eq(seen, "a=1;b=2;", "forEach walks every pair");
    }
    throws(() => new URL("http://[::1"), "malformed URL throws");

    /* Object descriptors match the engine's other globals: writable,
     * enumerable, configurable -- a plain data property a script may shadow
     * or delete. (Node defines URL/URLSearchParams NON-enumerable; ours are
     * enumerable by engine convention -- a documented divergence.) */
    for (const name of ["URL", "URLSearchParams", "sleep", "structuredClone"]) {
        const d = Object.getOwnPropertyDescriptor(globalThis, name);
        ok(d && d.writable && d.enumerable && d.configurable,
           "globalThis." + name + " is a writable/enumerable/configurable data property");
    }
} else {
    print("test_globals: URL/URLSearchParams checks skipped (no dyna:url)");
}

/* ---------------------------------------------- Worker-context globals */

/* js_std_add_helpers runs on the worker's own runtime too, after that
 * runtime's js_nat_init_all: the same four names must resolve there, and
 * URL must be the worker runtime's own dyna:url constructor. */
{
    const T = `${std.getenv("TMPDIR") || "/tmp"}/dj_glob.${Date.now()}.${Math.floor(Math.random() * 1e9)}`;
    const f = std.open(`${T}_wkr.js`, "w");
    f.puts([
        'import * as os from "os";',
        "const parent = os.Worker.parent;",
        "(async () => {",
        '    const m = await import("dyna:url");',
        "    parent.postMessage({",
        "        urlType: typeof URL,",
        "        spType: typeof URLSearchParams,",
        "        sleepType: typeof sleep,",
        "        scType: typeof structuredClone,",
        "        ident: URL === m.URL,",
        '        q: new URL("https://w.io/a?b=7").searchParams.get("b"),',
        "        slept: await sleep(5).then(() => true, () => false),",
        "        cloned: structuredClone({ k: [1] }).k[0] === 1,",
        "    });",
        "    parent.onmessage = null;",
        "})();",
    ].join("\n"));
    f.close();
    await (async () => {
        let tid;
        const done = new Promise((resolve) => {
            const w = new os.Worker(`${T}_wkr.js`);
            tid = setTimeout(() => { ok(false, "worker globals probe timed out"); w.onmessage = null; resolve(); }, 10000);
            w.onmessage = function (e) {
                clearTimeout(tid);
                const d = e.data;
                eq(d.urlType, "function", "worker: URL is global");
                eq(d.spType, "function", "worker: URLSearchParams is global");
                eq(d.sleepType, "function", "worker: sleep is global");
                eq(d.scType, "function", "worker: structuredClone is global");
                ok(d.ident === true, "worker: URL is the worker runtime's dyna:url.URL");
                eq(d.q, "7", "worker: global URL parses");
                ok(d.slept === true, "worker: sleep resolves on the worker loop");
                ok(d.cloned === true, "worker: structuredClone deep-copies");
                w.onmessage = null;
                resolve();
            };
            w.onerror = function (err) {
                clearTimeout(tid);
                ok(false, "worker error: " + (err && err.message ? err.message : err));
                w.onmessage = null;
                resolve();
            };
        });
        await withTimeout(done, 12000, "worker globals probe");
    })();
}

/* ------------------------------------ delete / shadow (last: destructive) */

if (urlMod && typeof URL === "function") {
    const saved = URL;
    ok(delete globalThis.URL, "globalThis.URL is deletable");
    eq(typeof URL, "undefined", "URL gone after delete");
    ok(urlMod.URL === saved, "the module export survives the delete");
    globalThis.URL = saved;             /* a script may reinstall its own */
    eq(typeof URL, "function", "URL re-definable after delete");
    ok(URL === saved, "reinstalled URL is the same constructor");
}

} /* end non-skip region */

if (fails) {
    print("test_globals: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_globals: " + fails + " failures");
}
print("test_globals: " + (n - fails) + "/" + n + " ok");
