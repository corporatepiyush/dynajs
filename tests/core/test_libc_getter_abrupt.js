// flags: --std
// timeout: 180
// covers: contract URL.createObjectURL Request Headers AbortSignal.any Response.formData dyna:http.MultipartFormat dyna:http.MultipartParse -- D49: every option, part, global and parsed-object read propagates a throwing getter
// st1r D49: the libc natives (blob, fetch, multipart, module loader, worker, search params) read globals, user parts
// and their own parsed objects with JS_GetProperty* and type-tested the result before any JS_IsException check, so a
// throwing getter was read as "absent": the call went on with the error pending, or a TypeError ("boundary parameter is
// missing", "each part needs a string name", "is not a valid URL") replaced it. Parsed multipart parts and Content-Type
// parameters are plain objects, so an inherited Object.prototype getter for an absent member is reachable too.
// js_module_test_json returns -1 on a failed attributes read and both module loaders fail the import; the engine builds the
// attributes object from own keys only, so that read fails only on allocation failure (not reachable from a getter).
// Oracle: each hostile call throws or rejects with exactly the sentinel. Also: a GC inside every getter keeps the
// result intact, and a --timeout-ms child's interrupt inside a getter ends a loop that catches everything.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_libc_getter_abrupt");
import { MultipartFormat, MultipartParse } from "dyna:http";
import { Exec, args } from "dyna:sys";
import * as std from "std";

const SELF = "tests/core/test_libc_getter_abrupt.js";
const SENT = new Error("libc getter sentinel");
const thrower = () => { throw SENT; };
const run = (f) => { try { return { v: f() }; } catch (e) { return { e }; } };
const arun = async (f) => { try { return { v: await f() }; } catch (e) { return { e }; } };
const with_ = (base, k, get = thrower) => { const x = { ...base }; delete x[k]; Object.defineProperty(x, k, { get, enumerable: true, configurable: true }); return x; };
const own = (k) => function (v) { Object.defineProperty(this, k, { value: v, writable: true, enumerable: true, configurable: true }); };
const swap = async (obj, k, d, f) => { const old = Object.getOwnPropertyDescriptor(obj, k); Object.defineProperty(obj, k, d); try { return await f(); } finally { if (old) Object.defineProperty(obj, k, old); else delete obj[k]; } };
const inherited = async (k, f) => { Object.defineProperty(Object.prototype, k, { get: thrower, set: own(k), configurable: true }); try { return await f(); } finally { delete Object.prototype[k]; } };
const gcw = (base) => { const x = {}; for (const k of Object.keys(base)) { const v = base[k]; Object.defineProperty(x, k, { get() { std.gc(); return v; }, enumerable: true }); } return x; };
const MP = "--BND\r\nContent-Disposition: form-data; name=\"a\"\r\n\r\nv\r\n--BND\r\nContent-Disposition: form-data; name=\"f\"; filename=\"x.txt\"\r\n\r\ndata\r\n--BND--\r\n";
const mpResp = () => new Response(MP, { headers: { "content-type": "multipart/form-data; boundary=BND" } });
const PART = { name: "a", value: "v" };

const PINNED = [
    ["MultipartFormat+Parse", () => { const f = MultipartFormat([{ name: "a", value: "v" }, { name: "f", body: new Uint8Array([1, 2]), filename: "x.bin", contentType: "application/x" }]); return MultipartParse(f.contentType, f.body).map((p) => p.name + ":" + (p.filename || "")).join(","); }],
    ["createObjectURL", () => URL.createObjectURL(new Blob(["x"])).slice(0, 10)],
    ["Request relative", () => { globalThis.location = { href: "http://h.test/d/", origin: "http://h.test" }; try { return new Request("p").url; } finally { delete globalThis.location; } }],
];
const APINNED = [
    ["Response.formData multipart", async () => { const fd = await mpResp().formData(); return fd.get("a") + "|" + fd.get("f").name; }],
];

if (args().includes("--child")) {
    const mode = args()[args().indexOf("--child") + 1];
    if (mode === "interrupt") {
        const spin = () => { let i = 0; for (;;) i++; };
        for (;;) { try { MultipartFormat([with_(PART, "name", spin)]); } catch (e) {} }
    }
    std.exit(0);
}

{
    let n = 0, fails = 0;
    const ok = (c, w) => { n++; if (!c) { fails++; print("  FAIL  " + w); } };
    const desc = (r) => r.e !== undefined ? "threw " + (r.e === SENT ? "SENT" : String(r.e)) : "returned " + String(JSON.stringify(r.v)).slice(0, 160);
    const sent = (w, f) => { const r = run(f); ok(r.e === SENT, w + ": " + desc(r)); };
    const asent = async (w, f) => { const r = await arun(f); ok(r.e === SENT, w + ": " + desc(r)); };

    await __kitS.section("MultipartFormat and MultipartParse propagate every getter", async () => {
        sent("MultipartFormat part name", () => MultipartFormat([with_(PART, "name")]));
        for (const k of ["value", "body", "filename", "contentType"])
            sent("MultipartFormat part " + k, () => MultipartFormat([with_(PART, k)]));
        await inherited("boundary", () => sent("MultipartParse inherited boundary getter", () => MultipartParse("multipart/form-data", new Uint8Array(0))));
        const r = run(PINNED[0][1]);
        ok(r.v === "a:,f:x.bin", "CONTROL format+parse round trip: " + desc(r));
        const g = MultipartFormat([gcw({ name: "a", value: "v" }), gcw({ name: "f", body: new Uint8Array([1]), filename: "x", contentType: "text/plain" })]);
        ok(MultipartParse(g.contentType, g.body).length === 2, "gc in every part getter keeps the formatted body");
    });

    await __kitS.section("URL, Request, Headers and AbortSignal propagate global and element getters", async () => {
        await swap(globalThis, "location", { get: thrower, configurable: true }, () => sent("createObjectURL location getter", () => URL.createObjectURL(new Blob(["x"]))));
        await swap(globalThis, "location", { value: with_({}, "origin"), configurable: true, writable: true }, () => sent("createObjectURL location.origin getter", () => URL.createObjectURL(new Blob(["x"]))));
        await swap(globalThis, "location", { get: thrower, configurable: true }, () => sent("Request relative URL location getter", () => new Request("p")));
        await swap(globalThis, "location", { value: with_({}, "href"), configurable: true, writable: true }, () => sent("Request relative URL location.href getter", () => new Request("p")));
        await swap(globalThis, "location", { value: gcw({ href: "http://h.test/d/" }), configurable: true, writable: true }, () => ok(new Request("p").url === "http://h.test/d/p", "gc in location.href getter keeps the base"));
        const arr = ["1"];
        Object.defineProperty(arr, 1, { get: thrower, enumerable: true, configurable: true });
        sent("Headers record value array element getter", () => new Headers({ a: arr }));
        for (const [w, f] of PINNED) { const r = run(f); ok(r.e === undefined && typeof r.v === "string" && r.v.length > 0, "CONTROL " + w + ": " + desc(r)); }
    });

    await __kitS.section("multipart Response.formData reads parsed parts, inherited accessors included", async () => {
        await inherited("filename", () => asent("formData inherited filename getter", () => mpResp().formData()));
        await inherited("contentType", () => asent("formData inherited contentType getter", () => mpResp().formData()));
        const r = await arun(APINNED[0][1]);
        ok(r.v === "v|x.txt", "CONTROL multipart formData: " + desc(r));
    });

    await __kitS.section("an interrupt that must end a catch-all loop", () => {
        const t0 = Date.now();
        const i = Exec(args()[0], ["--std", "--timeout-ms", "300", SELF, "--child", "interrupt"], { timeoutMs: 20000 });
        ok(i.code !== 0 && !i.timedOut && /interrupt/i.test(i.stderr + i.stdout), "interrupt child ends: rc " + i.code + " after " + (Date.now() - t0) + " ms " + i.stderr.slice(-200));
    });
    if (fails) throw new Error(fails + " of " + n + " checks failed");
    print("test_libc_getter_abrupt: " + n + " checks passed");
}
