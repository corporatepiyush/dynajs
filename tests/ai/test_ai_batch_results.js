// flags: --std
// timeout: 300
// covers: dyna:ai.BatchResults -- client.messages.batches: create/retrieve/list/cancel/delete/results
// The Message Batches API (platform.claude.com/docs/en/build-with-claude/batch-processing.md)
// against a raw TCP mock on 127.0.0.1. create() checks requests client-side (1..100000
// entries, each a non-empty string custom_id and an object params) before any I/O.
// results(id) is a BatchResults async iterator over the JSONL results stream: one object per
// line (CRLF and blank lines tolerated, a final line without a newline accepted), a line that
// is not a JSON object is an APIError stream_error, and a body cut short at any byte never
// yields a partial or invented result: with Content-Length or chunked framing every cut
// before the end rejects with connection_error; with close-delimited framing a cut can only
// end the iteration on a line boundary. A line is yielded as soon as it arrives (the server
// may hold the body open) and return() closes the connection at once.
// No live calls: apiKey "test-key", maxRetries 0.
import { sections as __kitSections } from "../lib/kit.js";
import { Anthropic, APIError, BatchResults } from "dyna:ai";
import { TCPServer } from "dyna:net";
import * as std from "std";
const __kitS = __kitSections("test_ai_batch_results", { chain: true });
let enc, dec, RESULTS, lines, JSONL, JSONL_NL, lineEnds, H_EOF, H_CL, H_CH, CUT, srv, bufs, held, c;

let pass = 0, fail = 0;
const ok = (c, w, d) => { if (c) pass++; else { fail++; print("  FAIL  " + w + (d !== undefined ? "  [" + d + "]" : "")); } };
const rejects = async (p, pred, w) => {
    try { await p; ok(false, w, "resolved"); }
    catch (e) { ok(pred(e), w, e && (e.constructor.name + ": " + e.message + (e.status !== undefined ? " status=" + e.status + " type=" + e.type : ""))); }
};
const cat = (...parts) => { const n = parts.reduce((a, p) => a + p.length, 0), o = new Uint8Array(n); let i = 0; for (const p of parts) { o.set(p, i); i += p.length; } return o; };
const find = (h, n) => { outer: for (let i = 0; i + n.length <= h.length; i++) { for (let j = 0; j < n.length; j++) if (h[i + j] !== n[j]) continue outer; return i; } return -1; };
const canon = (v) => JSON.stringify(v, (k, x) => (x && typeof x === "object" && !Array.isArray(x)) ? Object.fromEntries(Object.keys(x).sort().map((q) => [q, x[q]])) : x);
const M = "claude-opus-5-5";

const msg = (t) => ({ id: "msg_" + t, type: "message", role: "assistant", model: M, content: [{ type: "text", text: t }], stop_reason: "end_turn", stop_sequence: null, usage: { input_tokens: 3, output_tokens: 1 } });
const head = (h) => enc.encode("HTTP/1.1 200 OK\r\nContent-Type: application/binary\r\nrequest-id: req_res\r\n" + h + "Connection: close\r\n\r\n");
const chunk = (b) => cat(enc.encode(b.length.toString(16) + "\r\n"), b, enc.encode("\r\n"));

const log = [];
const reply = (status, body, ctype = "application/json") => {
    const b = typeof body === "string" ? enc.encode(body) : body;
    return cat(enc.encode("HTTP/1.1 " + status + " X\r\nContent-Type: " + ctype + "\r\nContent-Length: " + b.length + "\r\nrequest-id: req_" + status + "\r\nConnection: close\r\n\r\n"), b);
};
const apiErr = (status, type, message) => reply(status, JSON.stringify({ type: "error", error: { type, message }, request_id: "req_e" }));
const batch = (id, st) => ({ id, type: "message_batch", processing_status: st, request_counts: { processing: 1, succeeded: 0, errored: 0, canceled: 0, expired: 0 },
    ended_at: null, created_at: "2026-11-14T00:00:00Z", expires_at: "2026-11-15T00:00:00Z", cancel_initiated_at: null, results_url: null });
const route = (q) => {
    const B = "/v1/messages/batches";
    if (q.method === "POST" && q.target === B) return reply(200, JSON.stringify(batch("msgbatch_1", "in_progress")));
    if (q.method === "GET" && (q.target === B || q.target.startsWith(B + "?"))) return reply(200, JSON.stringify({ data: [batch("msgbatch_1", "ended")], has_more: false, first_id: "msgbatch_1", last_id: "msgbatch_1" }));
    let m = /^\/v1\/messages\/batches\/cut-(\w+)-(\d+)\/results$/.exec(q.target);
    if (m) return { raw: CUT[m[1]].wire.subarray(0, Number(m[2])) };
    if (q.target === B + "/b1/results") return { raw: cat(H_EOF, JSONL) };
    if (q.target === B + "/hold/results") return { raw: cat(H_EOF, enc.encode(lines[0] + "\n")), hold: true };
    if (q.target === B + "/bnl/results") return reply(200, JSONL_NL, "application/binary");
    if (q.target === B + "/bad/results") return reply(200, lines[0] + "\n[1,2]\n" + lines[1] + "\n", "application/binary");
    if (q.target === B + "/junk/results") return reply(200, lines[0] + "\n{\"a\":\n", "application/binary");
    if (q.target === B + "/empty/results") return reply(200, "", "application/binary");
    if (q.target === B + "/gone/results" || q.target === B + "/gone") return apiErr(404, "not_found_error", "batch not found");
    if (q.method === "POST" && (m = /^\/v1\/messages\/batches\/([^/]+)\/cancel$/.exec(q.target))) return reply(200, JSON.stringify(batch(decodeURIComponent(m[1]), "canceling")));
    if (q.method === "DELETE" && (m = /^\/v1\/messages\/batches\/([^/]+)$/.exec(q.target))) return reply(200, JSON.stringify({ id: decodeURIComponent(m[1]), type: "message_batch_deleted" }));
    if (q.method === "GET" && (m = /^\/v1\/messages\/batches\/([^/]+)$/.exec(q.target))) return reply(200, JSON.stringify(batch(decodeURIComponent(m[1]), "ended")));
    return null;
};
let heldClosed = 0;
const last = () => log[log.length - 1];
const params = { model: M, max_tokens: 64, messages: [{ role: "user", content: "x" }] };
const drain = async (it) => { const out = []; for await (const r of it) out.push(r); return out; };
__kitS.section("fixture: create the text encoder and decoder", () => {
enc = new TextEncoder(), dec = new TextDecoder();
RESULTS = [
    { custom_id: "r-ok", result: { type: "succeeded", message: msg("héllo \u{1F30D}") } },
    { custom_id: "r-err", result: { type: "errored", error: { type: "error", error: { type: "invalid_request_error", message: "max_tokens: too large" } } } },
    { custom_id: "r-can", result: { type: "canceled" } },
    { custom_id: "r-exp", result: { type: "expired" } },
];
lines = RESULTS.map((r) => JSON.stringify(r));
JSONL = enc.encode(lines[0] + "\r\n\n" + lines[1] + "\n  \n" + lines[2] + "\n" + lines[3]);
JSONL_NL = enc.encode(lines.join("\n") + "\n");
lineEnds = (() => { const ends = []; let o = 0; for (const l of lines) { o += enc.encode(l).length; ends.push(o); o += 1; } return ends; })();
H_EOF = head(""), H_CL = head("Content-Length: " + JSONL_NL.length + "\r\n"), H_CH = head("Transfer-Encoding: chunked\r\n");
CUT = {
    eof: { wire: cat(H_EOF, JSONL_NL), body: H_EOF.length },
    cl: { wire: cat(H_CL, JSONL_NL), body: H_CL.length },
    chunked: { wire: cat(H_CH, ...lines.map((l) => chunk(enc.encode(l + "\n"))), enc.encode("0\r\n\r\n")), body: H_CH.length },
};
srv = new TCPServer({ port: 0 });
bufs = new Map(), held = new Set();
srv.start({
    data: (c, bytes) => {
        const got = cat(bufs.get(c) || new Uint8Array(0), bytes);
        const he = find(got, enc.encode("\r\n\r\n"));
        if (he < 0) { bufs.set(c, got); return; }
        const headText = dec.decode(got.subarray(0, he));
        const cl = /\r\ncontent-length: *(\d+)/i.exec(headText);
        const need = cl ? Number(cl[1]) : 0;
        if (got.length - he - 4 < need) { bufs.set(c, got); return; }
        bufs.delete(c);
        const hl = headText.split("\r\n"), [method, target] = hl[0].split(" ");
        const headers = {};
        for (const l of hl.slice(1)) { const k = l.indexOf(":"); headers[l.slice(0, k).toLowerCase()] = l.slice(k + 1).trim(); }
        const req = { method, target, headers, body: dec.decode(got.subarray(he + 4, he + 4 + need)) };
        log.push(req);
        const r = route(req) || apiErr(404, "not_found_error", "no route " + method + " " + target);
        const wire = r.raw || r;
        if (wire.length) c.write(wire);
        if (r.hold) { held.add(c); return; }
        if (c.bufferedAmount === 0) c.close(); else c.closeOnDrain = true;
    },
    drain: (c) => { if (c.closeOnDrain) c.close(); },
    close: (c) => { bufs.delete(c); if (held.delete(c)) heldClosed++; },
});

c = new Anthropic({ apiKey: "test-key", baseURL: "http://127.0.0.1:" + srv.port, maxRetries: 0 });
});

try {
    {
let b, q;
        const body = { requests: [{ custom_id: "a", params }, { custom_id: "b-é", params: { ...params, system: "s" } }] };
await __kitS.section("create POSTs /v1/messages/batches", async () => {
        b = await c.messages.batches.create(body);
        q = last();
        ok(q.method === "POST" && q.target === "/v1/messages/batches", "create POSTs /v1/messages/batches", q.method + " " + q.target);
        ok(canon(JSON.parse(q.body)) === canon(body), "create sends the requests as given");
        ok(q.headers["content-type"] === "application/json" && !("anthropic-beta" in q.headers) && q.headers["x-api-key"] === "test-key", "create: JSON, no beta header, the key");
        ok(b.type === "message_batch" && b.id === "msgbatch_1" && b.processing_status === "in_progress", "create resolves the MessageBatch");
});
    }
    {
let n0, te, re;
await __kitS.section("create() with no body is a TypeError", async () => {
        n0 = log.length, te = (e) => e instanceof TypeError, re = (e) => e instanceof RangeError;
        await rejects(c.messages.batches.create(), te, "create() with no body is a TypeError");
        await rejects(c.messages.batches.create({}), te, "create without requests is a TypeError");
        await rejects(c.messages.batches.create({ requests: {} }), te, "create with non-array requests is a TypeError");
        await rejects(c.messages.batches.create({ requests: [] }), re, "create with zero requests is a RangeError");
        await rejects(c.messages.batches.create({ requests: Array.from({ length: 100001 }, (_, i) => ({ custom_id: "r" + i, params })) }), re, "create with 100001 requests is a RangeError");
        await rejects(c.messages.batches.create({ requests: [{ params }] }), (e) => te(e) && /requests\[0\]/.test(e.message), "a request without custom_id is a TypeError naming its index");
        await rejects(c.messages.batches.create({ requests: [{ custom_id: "a", params }, { custom_id: "", params }] }), (e) => te(e) && /requests\[1\]/.test(e.message), "an empty custom_id is a TypeError");
        await rejects(c.messages.batches.create({ requests: [{ custom_id: 7, params }] }), te, "a numeric custom_id is a TypeError");
        await rejects(c.messages.batches.create({ requests: [{ custom_id: "a", params: [] }] }), te, "array params is a TypeError");
        await rejects(c.messages.batches.create({ requests: [{ custom_id: "a" }] }), te, "missing params is a TypeError");
        await rejects(c.messages.batches.create({ requests: ["x"] }), te, "a non-object request is a TypeError");
        ok(log.length === n0, "no rejected create reached the wire", log.length - n0);
});
    }
    {
let b, l, x, d, n0;
await __kitS.section("retrieve GETs /v1/messages/batches/{id}", async () => {
        b = await c.messages.batches.retrieve("msgbatch_1");
        ok(last().method === "GET" && last().target === "/v1/messages/batches/msgbatch_1" && b.id === "msgbatch_1", "retrieve GETs /v1/messages/batches/{id}");
});
await __kitS.section("the id is one percent-encoded path segment", async () => {
        await c.messages.batches.retrieve("a/b?c");
        ok(last().target === "/v1/messages/batches/a%2Fb%3Fc", "the id is one percent-encoded path segment", last().target);
});
await __kitS.section("list passes limit/after_id and resolves the page", async () => {
        l = await c.messages.batches.list({ limit: 2, after_id: "msgbatch_0" });
        ok(last().target === "/v1/messages/batches?limit=2&after_id=msgbatch_0" && l.has_more === false && l.data.length === 1, "list passes limit/after_id and resolves the page", last().target);
});
await __kitS.section("list() has no query string", async () => {
        await c.messages.batches.list();
        ok(last().target === "/v1/messages/batches", "list() has no query string");
});
await __kitS.section("cancel POSTs .../{id}/cancel", async () => {
        x = await c.messages.batches.cancel("msgbatch_1");
        ok(last().method === "POST" && last().target === "/v1/messages/batches/msgbatch_1/cancel" && x.processing_status === "canceling", "cancel POSTs .../{id}/cancel");
        ok(last().body === "" && !("content-type" in last().headers), "cancel sends no body");
});
await __kitS.section("delete sends DELETE .../{id}", async () => {
        d = await c.messages.batches.delete("msgbatch_1");
        ok(last().method === "DELETE" && last().target === "/v1/messages/batches/msgbatch_1" && d.type === "message_batch_deleted", "delete sends DELETE .../{id}");
        await rejects(c.messages.batches.retrieve("gone"), (e) => e instanceof APIError && e.status === 404 && e.type === "not_found_error", "a 404 is an APIError");
});
        let te;
await __kitS.section("an empty id is a TypeError", async () => {
        n0 = log.length;
        await rejects(c.messages.batches.retrieve(""), (e) => e instanceof TypeError, "an empty id is a TypeError");
        await rejects(c.messages.batches.cancel(1), (e) => e instanceof TypeError, "a numeric id is a TypeError");
});
__kitS.section("results() without an id throws a TypeError synchronously", () => {
        try { c.messages.batches.results(); } catch (e) { te = e; }
        ok(te instanceof TypeError, "results() without an id throws a TypeError synchronously");
        ok(log.length === n0, "bad ids never reached the wire");
});
    }
    {
let it, got, again;
__kitS.section("results() returns a BatchResults async iterator", () => {
        it = c.messages.batches.results("b1");
        ok(it instanceof BatchResults && it[Symbol.asyncIterator]() === it, "results() returns a BatchResults async iterator");
        ok(it.status === null && it.requestId === null && log.every((q) => !q.target.endsWith("/b1/results")), "results() connects lazily");
});
await __kitS.section("results yields every line as an object (CRLF, blank lines, no final newline)", async () => {
        got = await drain(it);
        ok(canon(got) === canon(RESULTS), "results yields every line as an object (CRLF, blank lines, no final newline)", JSON.stringify(got).slice(0, 200));
        ok(it.status === 200 && it.requestId === "req_res", "status and requestId after connecting", it.status + " " + it.requestId);
        ok(last().headers["accept"] === "*/*" && last().method === "GET", "results GETs with Accept: */*", last().headers["accept"]);
        ok(got[1].result.error.error.type === "invalid_request_error" && got[0].result.message.content[0].text === "héllo \u{1F30D}", "errored and succeeded results keep their shapes");
});
await __kitS.section("next() after the end stays done", async () => {
        again = await it.next();
        ok(again.done === true && again.value === undefined, "next() after the end stays done");
        ok(canon(await drain(c.messages.batches.results("bnl"))) === canon(RESULTS), "a Content-Length body with a final newline yields every line");
        ok((await drain(c.messages.batches.results("empty"))).length === 0, "an empty results body yields nothing");
});
    }
    {
let it, first, j;
await __kitS.section("lines before a bad one are delivered", async () => {
        it = c.messages.batches.results("bad");
        first = await it.next();
        ok(first.done === false && first.value.custom_id === "r-ok", "lines before a bad one are delivered");
        await rejects(it.next(), (e) => e instanceof APIError && e.type === "stream_error", "a line that is not a JSON object is an APIError stream_error");
        ok((await it.next()).done === true, "after the error the iterator is done");
});
await __kitS.section("a malformed JSON line is an APIError stream_error", async () => {
        j = c.messages.batches.results("junk");
        await j.next();
        await rejects(j.next(), (e) => e instanceof APIError && e.type === "stream_error", "a malformed JSON line is an APIError stream_error");
        await rejects(c.messages.batches.results("gone").next(), (e) => e instanceof APIError && e.status === 404 && e.type === "not_found_error", "a 404 on results rejects the first next() with the APIError");
});
    }
    {
let it, p1, r;
await __kitS.section("a second concurrent next() is a TypeError", async () => {
        it = c.messages.batches.results("b1");
        p1 = it.next();
        await rejects(it.next(), (e) => e instanceof TypeError && /one read at a time/.test(e.message), "a second concurrent next() is a TypeError");
        ok((await p1).value.custom_id === "r-ok", "the first next() still resolves");
});
        let n = 0;
await __kitS.section("return() ends the iteration", async () => {
        r = await it.return();
        ok(r.done === true && (await it.next()).done === true, "return() ends the iteration");
});
await __kitS.section("break in for-await stops early", async () => {
        for await (const x of c.messages.batches.results("b1")) { n++; if (x.custom_id === "r-err") break; }
        ok(n === 2, "break in for-await stops early", n);
});
    }
    {
let hc, it;
await __kitS.section("a held-open results stream delivers its first line", async () => {
        hc = new Anthropic({ apiKey: "test-key", baseURL: "http://127.0.0.1:" + srv.port, maxRetries: 0, timeout: 20000 });
        it = hc.messages.batches.results("hold");
        ok((await it.next()).value.custom_id === "r-ok", "a held-open results stream delivers its first line");
        ok(held.size === 1 && heldClosed === 0, "the server is holding the connection open");
});
await __kitS.section("return() closes the connection without waiting for the body to end", async () => {
        await it.return();
        for (let i = 0; i < 100 && heldClosed === 0; i++) await new Promise((r) => setTimeout(r, 10));
        ok(heldClosed === 1, "return() closes the connection without waiting for the body to end", heldClosed);
});
    }
    {
        let te;
__kitS.section("BatchResults cannot be constructed directly", () => {
        try { new BatchResults(); } catch (e) { te = e; }
        ok(te instanceof TypeError, "BatchResults cannot be constructed directly");
});
    }
    const tally = {};
await __kitS.section("results are a prefix of the full set", async () => {
    for (const [mode, { wire, body }] of Object.entries(CUT)) {
        const t = tally[mode] = { rejected: 0, complete: 0, partial: 0, bad: 0 };
        for (let n = 0; n <= wire.length; n++) {
            const got = [];
            let err;
            try { for await (const r of c.messages.batches.results("cut-" + mode + "-" + n)) got.push(r); } catch (e) { err = e; }
            const prefixOk = canon(got) === canon(RESULTS.slice(0, got.length));
            if (!prefixOk) { t.bad++; ok(false, mode + " cut at " + n + ": results are a prefix of the full set", JSON.stringify(got).slice(0, 160)); continue; }
            if (err) {
                t.rejected++;
                if (!(err instanceof APIError && err.type === "connection_error" && !err.status)) { t.bad++; ok(false, mode + " cut at " + n + ": rejects with connection_error", err.constructor.name + ": " + err.message + " type=" + err.type); }
            } else if (got.length === RESULTS.length) {
                t.complete++;
                if (mode !== "eof" && n < wire.length) { t.bad++; ok(false, mode + " cut at " + n + " of " + wire.length + " completed silently"); }
            } else {
                t.partial++;
                const bodyN = n - body;
                if (mode !== "eof" || !(bodyN === 0 || lineEnds.includes(bodyN) || lineEnds.includes(bodyN - 1))) { t.bad++; ok(false, mode + " cut at " + n + " ended early off a line boundary with " + got.length + " results"); }
            }
        }
        ok(t.bad === 0, mode + ": every one of " + (wire.length + 1) + " cuts rejects, completes, or (close-delimited only) stops on a line boundary", JSON.stringify(t));
        ok(t.complete >= 1, mode + ": the uncut body completes");
        ok(mode === "eof" || t.rejected >= wire.length - 1, mode + ": every framed cut before the end rejects", t.rejected);
    }
    print("  cuts: " + JSON.stringify(tally));
});
} finally { if (!__kitS.listing) {
    srv.close();
} }
__kitS.end();
if (!__kitS.listing) {
print(`test_ai_batch_results: ${pass} passed, ${fail} failed`);
std.exit(fail ? 1 : 0);
}
