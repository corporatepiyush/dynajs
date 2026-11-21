// flags: --std
// covers: contract dyna:scrape.Fetcher dyna:ai.MessageAccumulator dyna:dkim.verify dyna:docdb.Collection.bulkWrite dyna:http.App dyna:log.Debug dyna:net.TCPServer.connect dyna:validate.IsJWT dyna:smtp.SmtpServer dyna:redis.Redis dyna:postgres.PostgreSQL -- one D48 rule (a JS_ToCString NULL is a pending exception: propagate it or clear it, never carry it) across the module natives
// D48 st1j-c: these natives converted a caller value (or an exception object) with JS_ToCString and treated a NULL
// as "no text" while the conversion's exception stayed pending, so the call went on, a later native that consults
// the exception slot threw the stale sentinel (TOML.stringify below), or a second conversion error replaced the first.
// Argument and option conversions now propagate the caller's own error at once (Redis/PostgreSQL stop at the first
// option that throws, like a WebIDL dictionary); printers of a thrown value (App handler and rpc 500s, the
// MessageAccumulator inputError, a dkim lookup failure) clear it and fall back to their fixed text; a Fetcher
// response header value whose toString throws rejects get() with that error (st1q: header errors propagate, rule 18).
// Oracle: each hostile call throws exactly the sentinel, or completes with the fallback text and a clean slot.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_api_tostring_abrupt");
import { MessageAccumulator } from "dyna:ai";
import { sign, verify } from "dyna:dkim";
import { RSA } from "dyna:crypto";
import { Database } from "dyna:docdb";
import { App } from "dyna:http";
import { Debug } from "dyna:log";
import { TCPServer } from "dyna:net";
import { IsJWT } from "dyna:validate";
import { SmtpServer } from "dyna:smtp";
import { Redis } from "dyna:redis";
import { PostgreSQL } from "dyna:postgres";
import { Fetcher } from "dyna:scrape";
import { TOML } from "dyna:config";
import { makeTempDir, removeAll } from "dyna:file";
import * as std from "std";

let n = 0, fails = 0;
function ok(c, m) { n++; if (!c) { fails++; print("FAIL: " + m); } else print("ok: " + m); }

const SENT = new Error("tostring sentinel");
const SENT2 = new Error("second sentinel");
const T = { toString() { throw SENT; } };
const T2 = { toString() { throw SENT2; } };
const slot = () => {
    try { TOML.stringify({ a: Symbol() }); return "no throw"; } catch (e) { return e === SENT ? "sentinel leaked" : e instanceof TypeError ? "clean" : "threw " + e; }
};
const settle = async (f) => {
    try { const v = await f(); return { v }; } catch (e) { return { e }; }
};
const what = (r) => r.e === SENT ? "sentinel" : r.e === SENT2 ? "second sentinel" : r.e ? "threw " + r.e : "returned";
const close = (x) => { try { if (x && typeof x.close === "function") { const r = x.close(); if (r && r.catch) r.catch(() => {}); } } catch (e) {} };

await __kitS.section("argument and option conversions propagate the caller's error", async () => {
    const root = makeTempDir("st1jc");
    const tok = btoa(JSON.stringify({ alg: "HS256", typ: "JWT" })).replace(/=+$/, "") + "." + btoa("{}").replace(/=+$/, "") + ".c2ln";
    std.setenv("DEBUG", "st1jc");
    const dbg = Debug("st1jc");
    std.unsetenv("DEBUG");
    const db = new Database("st1jc");
    const rows = [
        ["App.static allow entry", () => new App({ port: 0 }).static("/s", root, { allow: [T] })],
        ["Debug printer argument", () => dbg("x", T)],
        ["TCPServer.connect tls.alpn entry", () => TCPServer.connect({ host: "127.0.0.1", port: 1, tls: { alpn: ["h2", T], rejectUnauthorized: false } }, { error() {}, close() {} })],
        ["IsJWT algs entry", () => IsJWT(tok, { algs: [T] })],
        ["SmtpServer disable entry", () => new SmtpServer({ port: 0, tls: "none", disable: [T], onData() { return true; } })],
        ["Redis host then password", () => new Redis({ host: T, password: T2, port: 1 })],
        ["PostgreSQL host then user", () => new PostgreSQL({ host: T, user: T2, port: 1 })],
        ["Collection.bulkWrite error message", () => db.collection("c").bulkWrite([{ updateOne: { get filter() { throw { code: 2, message: T }; }, update: { $set: { a: 1 } } } }])],
    ];
    for (const [name, f] of rows) {
        const r = await settle(f);
        const s = slot();
        ok(r.e === SENT && s === "clean", name + ": " + what(r) + ", " + s);
        close(r.v);
    }
    ok(IsJWT(tok, { algs: ["HS256"] }) === true && IsJWT(tok, { algs: ["RS256"] }) === false, "IsJWT plain algs still match");
    close(db);
    removeAll(root);
});

await __kitS.section("printers of a thrown value clear a throwing toString and use their fallback", async () => {
    const app = new App({ port: 0 });
    app.use((req) => { if (req.path === "/mw/x") throw T; });
    app.get("/mw/x", () => "unreached");
    app.get("/t", () => { throw T; });
    app.get("/d/:id", () => { throw T; });
    app.rpc("/rpc", { sync: () => { throw T; }, later: async () => { throw T; } });
    app.start();
    const B = "http://127.0.0.1:" + app.port;
    const rpc = (m) => fetch(B + "/rpc", { method: "POST", headers: { "content-type": "application/json" }, body: JSON.stringify({ jsonrpc: "2.0", method: m, id: 1 }) });
    for (const [name, f] of [["App middleware", () => fetch(B + "/mw/x")], ["App handler", () => fetch(B + "/t")], ["App dynamic route handler", () => fetch(B + "/d/7")], ["App rpc sync method", () => rpc("sync")], ["App rpc async method", () => rpc("later")], ["App rpc batch member", () => fetch(B + "/rpc", { method: "POST", headers: { "content-type": "application/json" }, body: JSON.stringify([{ jsonrpc: "2.0", method: "sync", id: 1 }, { jsonrpc: "2.0", method: "sync", id: 2 }]) })]]) {
        const r = await settle(async () => { const res = await f(); return res.status + " " + await res.text(); });
        const s = slot();
        ok(!r.e && /Server error/.test(r.v) && s === "clean", name + ": " + (r.e ? "threw " + r.e : r.v) + ", " + s);
    }
    app.close();

    const ets = Error.prototype.toString;
    Error.prototype.toString = function () { throw SENT; };
    let a, s;
    try {
        a = new MessageAccumulator();
        const ev = (e, d) => `event: ${e}\ndata: ${JSON.stringify(d)}\n\n`;
        a.feed(ev("message_start", { type: "message_start", message: { id: "m", type: "message", role: "assistant", model: "x", content: [], stop_reason: null, stop_sequence: null, usage: { input_tokens: 1, output_tokens: 1 } } })
            + ev("content_block_start", { type: "content_block_start", index: 0, content_block: { type: "tool_use", id: "t", name: "n", input: {} } })
            + ev("content_block_delta", { type: "content_block_delta", index: 0, delta: { type: "input_json_delta", partial_json: "{\"a\": nope" } })
            + ev("content_block_stop", { type: "content_block_stop", index: 0 }));
    } finally {
        s = slot();
        Error.prototype.toString = ets;
    }
    const ie = a.message.content[0].inputError;
    ok(ie && ie.message === "tool input is not a JSON object" && s === "clean", "MessageAccumulator inputError: " + (ie && ie.message) + ", " + s);

    const K = RSA.generate(2048);
    const MSG = "From: a@example.com\r\nTo: b@example.net\r\nSubject: hi\r\n\r\nbody\r\n";
    const f = await sign(MSG, { domain: "example.com", selector: "s1", privateKey: K.privateKey });
    const r = await settle(() => verify(f + MSG, { lookup: () => { throw T; } }));
    const s2 = slot();
    const v = r.v && r.v[0];
    ok(v && v.status === "temperror" && /key lookup failed: error/.test(v.reason) && s2 === "clean", "dkim lookup throwing a throwing-toString value: " + (r.e ? "threw " + r.e : v && v.status + ":" + v.reason) + ", " + s2);
});

await __kitS.section("Fetcher rejects on a response header value whose toString throws", async () => {
    const client = { request(m, url) { return Promise.resolve({ status: url.endsWith("/robots.txt") ? 404 : 200, url, body: "", headers: url.endsWith("/robots.txt") ? {} : { "x-robots-tag": T, link: T, "content-type": "text/html" } }); } };
    const fe = new Fetcher({ agent: "st1jc/1.0 (+https://example.test/bot)", client, minDelayMs: 0 });
    const r = await settle(() => fe.get("http://a.test/p"));
    const s = slot();
    ok(r.e === SENT && s === "clean", "Fetcher.get with throwing x-robots-tag/link values: " + (r.e ? "threw " + r.e : "status " + r.v.status) + ", " + s);
    close(fe);
});

if (fails) throw new Error(fails + " of " + n + " checks failed");
print("test_api_tostring_abrupt: " + n + " checks passed");
