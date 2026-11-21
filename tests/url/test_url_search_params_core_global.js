// flags: --std
// covers: integration dyna:url.URLSearchParams dyna:http.MultipartParse dyna:http.MultipartFormat -- the core URLSearchParams global, Request URL parsing and FormData multipart bodies against the dyna:url and dyna:http re-exports of the same core code
// URLSearchParams, FormData and the multipart parse/format that global fetch uses live in the core (B1c): a build
// without dyna:url or dyna:net still has globalThis.URLSearchParams and FormData bodies. dyna:url and dyna:http
// re-export the same core code, so the global and the module export are one constructor in every context.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_url_search_params_core_global");
import * as os from "os";
import * as std from "std";

let n = 0, fails = 0;
function ok(c, m) { n++; if (!c) { fails++; print("  FAIL: " + m); } }
function eq(a, b, m) { ok(a === b, m + " (got " + JSON.stringify(a) + ", want " + JSON.stringify(b) + ")"); }

const urlMod = await import("dyna:url").catch(() => null);
const httpMod = await import("dyna:http").catch(() => null);

__kitS.section("core global", () => {
    eq(typeof URLSearchParams, "function", "URLSearchParams is a core global");
    const sp = new URLSearchParams("a=1&b=2&a=3");
    eq(sp.getAll("a").join(","), "1,3", "getAll");
    eq(sp.size, 3, "size");
    eq(Object.prototype.toString.call(sp[Symbol.iterator]()), "[object URLSearchParams Iterator]", "iterator tag");
    eq([...sp.keys()].join(""), "aba", "keys iterate");
    const it = sp.entries();
    eq(Object.getPrototypeOf(Object.getPrototypeOf(it)), Object.getPrototypeOf(Object.getPrototypeOf([].values())), "iterator inherits %IteratorPrototype%");
    const d = Object.getOwnPropertyDescriptor(globalThis, "URLSearchParams");
    ok(d && d.writable && d.enumerable && d.configurable, "global is writable/enumerable/configurable");
});

if (urlMod) {
    __kitS.section("module export is the global", () => {
        ok(urlMod.URLSearchParams === URLSearchParams, "dyna:url.URLSearchParams === globalThis.URLSearchParams");
        const u = new urlMod.URL("https://h.io/p?x=1&y=2");
        ok(u.searchParams instanceof URLSearchParams, "URL#searchParams is a global URLSearchParams");
        ok(Object.getPrototypeOf(u.searchParams) === URLSearchParams.prototype, "searchParams proto");
        u.searchParams.append("z", "3");
        eq(u.href, "https://h.io/p?x=1&y=2&z=3", "searchParams writes through to href");
        u.search = "?q=9";
        eq(u.searchParams.get("q"), "9", "href writes through to searchParams");
    });
}

__kitS.section("Request URLs parse with the core parser", () => {
    eq(new Request("HTTP://EXAMPLE.invalid:80/a/../b?q#f").url, "http://example.invalid/b?q#f", "Request url is the WHATWG href");
    eq(new Request("http://example.invalid/é x").url, "http://example.invalid/%C3%A9%20x", "path percent-encoding");
    eq(new Request("http://xn--nxasmq6b.example/").url, new Request("http://ββββ.example/").url.replace(/^http:\/\/[^/]+/, "http://xn--nxasmq6b.example"), "IDNA host round trip");
    let e = null;
    try { new Request("http://user:pw@example.invalid/"); } catch (x) { e = x; }
    ok(e instanceof TypeError && /includes credentials/.test(e.message), "Request with credentials is a TypeError: " + e);
    e = null;
    try { new Request("relative/path"); } catch (x) { e = x; }
    ok(e instanceof TypeError && /is not a valid URL/.test(e.message), "relative Request URL without a base is a TypeError: " + e);
    eq(Response.redirect("http://u:p@example.invalid/x", 302).headers.get("location"), "http://u:p@example.invalid/x", "Response.redirect keeps credentials");
    const saved = globalThis.URL;
    globalThis.URL = function Fake() { throw new Error("global URL must not be used"); };
    try { eq(new Request("http://example.invalid/z").url, "http://example.invalid/z", "Request ignores a replaced globalThis.URL"); }
    finally { globalThis.URL = saved; }
});

__kitS.section("fetch bodies", async () => {
    const rq = new Request("http://example.invalid/", { method: "POST", body: new URLSearchParams("a=1 2&b=%") });
    eq(rq.headers.get("content-type"), "application/x-www-form-urlencoded;charset=UTF-8", "URLSearchParams body content-type");
    eq(await rq.text(), "a=1+2&b=%25", "URLSearchParams body text");
    const fd = new FormData();
    fd.append("name", "vé");
    fd.append("file", new Blob(["xyz"], { type: "text/plain" }), "f.txt");
    const r = new Response(fd);
    const ct = r.headers.get("content-type");
    ok(/^multipart\/form-data; boundary=/.test(ct), "FormData body content-type: " + ct);
    const back = await r.formData();
    eq(back.get("name"), "vé", "multipart round trip: text field");
    const f = back.get("file");
    eq(f && f.name, "f.txt", "multipart round trip: file name");
    eq(f && await f.text(), "xyz", "multipart round trip: file bytes");
    const bad = new Response("x", { headers: { "content-type": "multipart/form-data" } });
    let threw = null;
    try { await bad.formData(); } catch (e) { threw = e; }
    ok(threw instanceof TypeError, "multipart without a boundary rejects with TypeError");
});

if (httpMod) {
    __kitS.section("dyna:http re-exports the core multipart", () => {
        const fmt = httpMod.MultipartFormat([{ name: "k", value: "v" }]);
        const parts = httpMod.MultipartParse(fmt.contentType, fmt.body);
        eq(parts.length, 1, "MultipartParse part count");
        eq(parts[0].name, "k", "MultipartParse name");
        const ctp = httpMod.ContentTypeParse("text/html; Charset=\"utf-8\"");
        eq(ctp.type + "/" + ctp.subtype + ";" + ctp.parameters.charset, "text/html;utf-8", "ContentTypeParse");
    });
}

async function worker(lines) {
    const T = `${std.getenv("TMPDIR") || "/tmp"}/dj_spcore.${Date.now()}.${Math.floor(Math.random() * 1e9)}`;
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
            "    const g = new URLSearchParams('a=1');",
            "    const m = await import('dyna:url');",
            "    parent.postMessage({ ident: m.URLSearchParams === URLSearchParams, inst: new m.URL('http://h/?a=1').searchParams instanceof URLSearchParams, g: g.get('a') });",
        ],
        "module first": [
            "    const m = await import('dyna:url');",
            "    const s = new m.URLSearchParams('a=1');",
            "    parent.postMessage({ ident: m.URLSearchParams === URLSearchParams, inst: s instanceof URLSearchParams, g: s.get('a') });",
        ]
    };
    for (const [name, lines] of Object.entries(orders)) {
        const d = await worker(lines);
        ok(d && !d.error, `worker (${name}) ran: ${JSON.stringify(d)}`);
        if (d && !d.error) {
            ok(d.ident === true, `worker (${name}): module export is the global`);
            ok(d.inst === true, `worker (${name}): URL#searchParams is the global class`);
            eq(d.g, "1", `worker (${name}): get`);
        }
    }
}

if (fails) {
    print("test_url_search_params_core_global: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_url_search_params_core_global failed");
}
print("test_url_search_params_core_global: " + n + " assertions, 0 failures");
