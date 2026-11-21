// flags: --std
// covers: dyna:url.URL
// URL is a web global, so it lives in the core (B1d: libc-url.c, libc-url-accessors.c): a build without dyna:url
// still has globalThis.URL with parse/canParse/join and the blob createObjectURL/revokeObjectURL statics.
// dyna:url re-exports the same core constructor (a per-context holder caches it, so replacing
// URL.prototype.constructor or deleting the global cannot change what the module exports), in every context and Worker.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_url_url_core_global");
import * as os from "os";
import * as std from "std";

let n = 0, fails = 0;
function ok(c, m) { n++; if (!c) { fails++; print("  FAIL: " + m); } }
function eq(a, b, m) { ok(a === b, m + " (got " + JSON.stringify(a) + ", want " + JSON.stringify(b) + ")"); }
function throwsType(fn, m) { let e = null; try { fn(); } catch (x) { e = x; } ok(e instanceof TypeError, m + " throws TypeError (got " + (e && e.name) + ")"); }

const GURL = globalThis.URL;
const urlMod = await import("dyna:url").catch(() => null);

__kitS.section("core global", () => {
    eq(typeof URL, "function", "URL is a core global");
    eq(URL.name, "URL", "URL.name");
    eq(URL.length, 0, "URL.length as built");
    const d = Object.getOwnPropertyDescriptor(globalThis, "URL");
    ok(d && d.writable && d.enumerable && d.configurable, "global descriptor is C_W_E");
    const u = new URL("../b/./c?q=1#h", "http://ex.com/a/x/y");
    eq(u.href, "http://ex.com/a/b/c?q=1#h", "relative resolution");
    eq(u.origin, "http://ex.com", "origin");
    eq(u.pathname, "/a/b/c", "pathname");
    eq(String(u), u.href, "toString");
    eq(JSON.stringify({ u }), "{\"u\":\"http://ex.com/a/b/c?q=1#h\"}", "toJSON");
    ok(Object.getPrototypeOf(u) === URL.prototype, "instance proto");
    ok(URL.prototype.constructor === URL, "prototype.constructor");
    u.port = "8080";
    eq(u.host, "ex.com:8080", "port setter");
    u.hostname = "Bücher.example";
    eq(u.hostname, "xn--bcher-kva.example", "IDNA host setter");
    ok(u.searchParams instanceof URLSearchParams, "searchParams is the core URLSearchParams");
    u.searchParams.append("z", "a b");
    eq(u.search, "?q=1&z=a+b", "searchParams writes back to search");
    ok(u.searchParams === u.searchParams, "searchParams is cached");
    throwsType(() => new URL("not a url"), "invalid input");
    throwsType(() => new URL("x", "also not"), "invalid base");
    throwsType(() => URL("http://h/"), "call without new");
});

__kitS.section("statics", () => {
    for (const k of ["parse", "canParse", "join", "createObjectURL", "revokeObjectURL"]) {
        eq(typeof URL[k], "function", "URL." + k);
        const d = Object.getOwnPropertyDescriptor(URL, k);
        ok(d && d.writable && d.enumerable && d.configurable, "URL." + k + " is C_W_E");
    }
    eq(URL.parse("nope"), null, "parse invalid -> null");
    eq(URL.parse("b", "http://h/a/").href, "http://h/a/b", "parse with base");
    ok(URL.parse("http://h/") instanceof URL, "parse returns a URL");
    eq(URL.canParse("x:y"), true, "canParse opaque");
    eq(URL.canParse("/p"), false, "canParse relative without base");
    eq(URL.canParse("/p", "http://h/"), true, "canParse relative with base");
    eq(URL.join("http://h/a/b", "../c"), "http://h/c", "join");
    const blobUrl = URL.createObjectURL(new Blob(["x"]));
    ok(/^blob:/.test(blobUrl), "createObjectURL on the core URL");
    URL.revokeObjectURL(blobUrl);
});

__kitS.section("subclass", () => {
    class Sub extends URL { tag() { return "sub:" + this.host; } }
    const s = new Sub("https://h.example/p");
    ok(s instanceof Sub && s instanceof URL, "subclass instance");
    eq(s.tag(), "sub:h.example", "subclass method sees the URL state");
});

if (urlMod) {
    __kitS.section("module export is the global", () => {
        ok(urlMod.URL === GURL, "dyna:url URL is the core global");
        ok(urlMod.URLSearchParams === globalThis.URLSearchParams, "dyna:url URLSearchParams is the core global");
        eq(typeof urlMod.domainToASCII, "function", "IDNA helpers stay in dyna:url");
    });
}

async function worker(lines) {
    const T = `${std.getenv("TMPDIR") || "/tmp"}/dj_urlcore.${Date.now()}.${Math.floor(Math.random() * 1e9)}`;
    const f = std.open(`${T}_wkr.js`, "w");
    f.puts(['import * as os from "os";', "const parent = os.Worker.parent;", "(async () => {", ...lines, "    parent.onmessage = null;", "})();"].join("\n"));
    f.close();
    try {
        return await new Promise((resolve) => {
            const w = new os.Worker(`${T}_wkr.js`);
            const tid = setTimeout(() => { w.onmessage = null; resolve(null); }, 10000);
            w.onmessage = (e) => { clearTimeout(tid); w.onmessage = null; resolve(e.data); };
            w.onerror = (err) => { clearTimeout(tid); w.onmessage = null; resolve({ error: String(err && err.message || err) }); };
        });
    } finally {
        os.remove(`${T}_wkr.js`);
    }
}

if (urlMod && typeof os.Worker === "function") {
    const orders = {
        "global first": [
            "    const g = new URL('http://h/a?x=1');",
            "    const m = await import('dyna:url');",
            "    parent.postMessage({ ident: m.URL === URL, inst: g instanceof m.URL, h: g.searchParams.get('x') });",
        ],
        "module first, prototype.constructor replaced": [
            "    const G = URL;",
            "    URL.prototype.constructor = function Fake() {};",
            "    delete globalThis.URL;",
            "    const m = await import('dyna:url');",
            "    const s = new m.URL('http://h/a?x=1');",
            "    parent.postMessage({ ident: m.URL === G, inst: s instanceof G, h: s.searchParams.get('x') });",
        ]
    };
    for (const [name, lines] of Object.entries(orders)) {
        const d = await worker(lines);
        ok(d && !d.error, `worker (${name}) ran: ${JSON.stringify(d)}`);
        if (d && !d.error) {
            ok(d.ident === true, `worker (${name}): module export is the core constructor`);
            ok(d.inst === true, `worker (${name}): instances share the class`);
            eq(d.h, "1", `worker (${name}): searchParams`);
        }
    }
}

__kitS.end();

if (fails) {
    print("test_url_url_core_global: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_url_url_core_global failed");
}
print("test_url_url_core_global: " + n + " assertions, 0 failures");
