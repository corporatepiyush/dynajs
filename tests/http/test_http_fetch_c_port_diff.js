// flags: --std
// covers: dyna:http.fetch
// fetch, Headers, Request and Response moved from a JS literal compiled per context to C classes in libc-fetch.c (only a
// ~45-line await loop stays JS, compiled on the first fetch() call). fixtures/fetch_js_reference.js is the retired JS,
// kept verbatim as the WIRE oracle only: for every status x method x Location form x redirect mode an in-process App logs
// each hop (method, path, authorization, content-type, body) and the final status/url/body or error must match.
// Owner rule 12 (spec first): API-level behaviour (Headers sort-and-combine iteration, ByteString/token validation, Request
// and Response construction, body extraction, FormData, AbortSignal) is no longer diffed against the old JS, which deviated
// from the WHATWG Fetch/DOM specs; the oracle for those is WPT (tools/wpt.js over fetch/api and xhr/formdata, expectations
// in tests/wpt-expectations.tsv). Response header lists are compared sorted, since the spec iterates sorted. Intended wire
// deltas: a FormData filename on a byte-buffer value reaches the multipart part; a string body without a Content-Type now
// sends text/plain;charset=UTF-8 (Fetch "extract a body"), so cases here always set Content-Type explicitly.
import * as std from "std";
import { App, fetch } from "dyna:http";
import { sections } from "../lib/kit.js";
const S = sections("test_http_fetch_c_port_diff");
let checks = 0, fails = 0;
function eq(got, want, what) { checks++; if (got !== want) { fails++; if (fails < 30) print("FAIL: " + what + "\n  new: " + got + "\n  old: " + want); } }

const NEW = { Headers, Request, Response, FormData, AbortController, fetch };
if (globalThis.fetch !== fetch) throw new Error("dyna:http fetch is not the global fetch");
const src = std.loadFile(new URL("./fixtures/fetch_js_reference.js", import.meta.url).pathname ?? "tests/http/fixtures/fetch_js_reference.js");
std.evalScript(src)((only) => (only ? undefined : null));
const OLD = { Headers, Request, Response, FormData, AbortController, fetch: globalThis.fetch };
Object.assign(globalThis, NEW);


const log = [];
const app = new App({ port: 0, host: "127.0.0.1" });
const handler = (req) => {
    log.push([req.method, req.path, req.headers.authorization || "-", req.headers["content-type"] || "-", req.body || ""].join(" "));
    const q = req.query || {};
    if (req.path === "/r") return { status: +q.c, headers: q.to === undefined ? {} : { Location: q.to }, body: "from " + q.c };
    return { status: 200, body: "final " + req.path };
};
for (const m of ["get", "post", "put", "del", "patch"]) { app[m]("/r", handler); app[m]("/:p", handler); app[m]("/d/:p", handler); }
app.start();
const A = "http://127.0.0.1:" + app.port, B = "http://localhost:" + app.port;
const enc = encodeURIComponent;

async function outcome(G, url, init) {
    log.length = 0;
    let res;
    try {
        const r = await G.fetch(url, init);
        res = [r.status, r.url, await r.text(), [...r.headers].filter(([k]) => k !== "date").map(([k, v]) => [k.toLowerCase(), v]).sort((x, y) => x[0] < y[0] ? -1 : x[0] > y[0] ? 1 : 0)];
    } catch (e) { res = "throw " + (e && e.message); }
    return JSON.stringify([res, log.slice()]);
}

await S.section("redirect handling matches the reference JS", async () => {
    const LOCS = { abs: A + "/t", cross: B + "/t", root: "/t", rel: "t", deep: "/d/x?q=1", empty: "", none: undefined, bad: "http://[bad" };
    for (const c of [301, 302, 303, 307, 308, 300, 304])
        for (const [ln, loc] of Object.entries(LOCS))
            for (const mode of ["follow", "manual", "error"])
                for (const method of ["GET", "POST"]) {
                    const url = A + "/r?c=" + c + (loc === undefined ? "" : "&to=" + enc(loc));
                    const init = { method, redirect: mode, headers: { Authorization: "Bearer k", "Content-Type": "text/x", "X-Keep": "1" } };
                    if (method === "POST") init.body = "payload";
                    const n = await outcome(NEW, url, init), o = await outcome(OLD, url, init);
                    eq(n, o, c + " " + ln + " " + mode + " " + method);
                }
    const chain = A + "/r?c=302&to=" + enc("/r?c=307&to=" + enc(B + "/r?c=303&to=" + enc("/end")));
    eq(await outcome(NEW, chain, { method: "PUT", body: "z", headers: { Cookie: "c=1", "Content-Type": "text/x" } }), await outcome(OLD, chain, { method: "PUT", body: "z", headers: { Cookie: "c=1", "Content-Type": "text/x" } }), "3-hop chain");
    const loop = A + "/r?c=302&to=" + enc("/r?c=302&to=%2Fr%3Fc%3D302");
    eq(await outcome(NEW, A + "/r?c=302&to=" + enc("/r?c=302"), {}), await outcome(OLD, A + "/r?c=302&to=" + enc("/r?c=302"), {}), "redirect without target after hop");
    void loop;
});
await S.section("caller buffers are never aliased", async () => {
    const ab = new Uint8Array([1, 2, 3]).buffer;
    const r = new Response(ab); r.body; new Uint8Array(ab)[0] = 99;
    eq(JSON.stringify([...new Uint8Array(await r.arrayBuffer())]), "[1,2,3]", "body stream snapshots a caller ArrayBuffer");
    const ab2 = new Uint8Array([4, 5]).buffer;
    eq(await new Response(ab2).arrayBuffer() === ab2, false, "arrayBuffer() copies a caller ArrayBuffer");
    const big = new Uint8Array([9, 9, 1, 2, 3, 9]);
    const st = new ReadableStream({ start(c) { c.enqueue(new Uint8Array(big.buffer, 2, 3)); c.close(); } });
    const got = await new Response(st).arrayBuffer();
    eq(JSON.stringify([got.byteLength, ...new Uint8Array(got)]), "[3,1,2,3]", "a stream of a view yields only the view bytes");
    const one = new Uint8Array([5, 6]);
    const st2 = new ReadableStream({ start(c) { c.enqueue(one); c.close(); } });
    eq(await new Request("http://h/", { method: "POST", body: st2, duplex: "half" }).arrayBuffer() === one.buffer, false, "Request.arrayBuffer over a stream never returns the chunk buffer");
});
await S.section("a FormData filename reaches the multipart part", async () => {
    const fd = new FormData(); fd.append("up", new TextEncoder().encode("content"), "doc.txt"); fd.append("plain", "v");
    log.length = 0;
    await (await fetch(A + "/echo", { method: "POST", body: fd })).text();
    eq(/filename="doc.txt"/.test(log[0]), true, "filename in the multipart body");
    eq(/name="plain"; filename/.test(log[0]), false, "no filename on a plain part");
});
app.close();
print("test_http_fetch_c_port_diff: " + checks + " checks, " + fails + " failures");
if (fails) std.exit(1);
