// flags: --std
// timeout: 120
// covers: dyna:ai.Anthropic
// RequestOptions.signal cancels a dyna:ai call (both clients share the
// transport; OpenAI rows ride along). A raw TCP mock can hang before the
// response head, answer 503 with a long retry-after, or send a head and one
// event then hold the stream open. An abort must reject promptly with APIError
// {status 0, type "aborted", retryable false}: before any I/O (an already
// aborted signal never connects), while waiting for the head (the socket is
// shut down), during a retry backoff sleep, and in a stream both mid-read and
// between reads. A call that finished first is unaffected and its abort
// listener is removed (a long-lived signal does not collect listeners).
import { sections as __kitSections } from "../lib/kit.js";
import { Anthropic, OpenAI, APIError } from "dyna:ai";
import { TCPServer } from "dyna:net";
import * as std from "std";
import * as os from "os";
const __kitS = __kitSections("test_ai_anthropic_signal", { chain: true });
let enc, dec, msgJson, startEv, oaChunk, pending, held, srv;

let pass = 0, fail = 0;
const ok = (c, w, d) => { if (c) { pass++; print("  ok    " + w); } else { fail++; print("  FAIL  " + w + (d !== undefined ? "  [" + d + "]" : "")); } };
const sleep = (ms) => new Promise((r) => os.setTimeout(r, ms));
const aborted = (e) => e instanceof APIError && e.type === "aborted" && e.status === 0 && e.retryable === false;
const outcome = async (p) => { const t0 = Date.now(); try { const v = await p; return { v, ms: Date.now() - t0 }; } catch (e) { return { e, ms: Date.now() - t0 }; } };
const desc = (r) => r.e ? r.e.constructor.name + ": " + r.e.message + " type=" + r.e.type + " in " + r.ms + " ms" : "resolved in " + r.ms + " ms";

const M = "claude-opus-5-5";
const reply = (status, ctype, body, extra = "") => enc.encode(`HTTP/1.1 ${status} X\r\nContent-Type: ${ctype}\r\nContent-Length: ${enc.encode(body).length}\r\n${extra}Connection: close\r\n\r\n${body}`);

let conns = 0, closed = 0, reqs = 0;
const base = (p) => "http://127.0.0.1:" + srv.port + p;
const ant = (p, o = {}) => new Anthropic({ apiKey: "k", baseURL: base(p), maxRetries: 0, timeout: 30000, ...o });
const oai = (p, o = {}) => new OpenAI({ apiKey: "k", baseURL: base(p) + "/v1", maxRetries: 0, timeout: 30000, ...o });
const body = { model: M, max_tokens: 8, messages: [{ role: "user", content: "x" }] };
const chat = { model: "g", messages: [{ role: "user", content: "x" }] };
__kitS.section("fixture: create the text encoder and decoder", () => {
enc = new TextEncoder(), dec = new TextDecoder();
msgJson = JSON.stringify({ id: "msg_1", type: "message", role: "assistant", model: M, content: [{ type: "text", text: "hi" }], stop_reason: "end_turn", stop_sequence: null, usage: { input_tokens: 1, output_tokens: 1 } });
startEv = "event: message_start\ndata: " + JSON.stringify({ type: "message_start", message: { id: "m", type: "message", role: "assistant", model: M, content: [], stop_reason: null, stop_sequence: null, usage: { input_tokens: 1, output_tokens: 1 } } }) + "\n\n";
oaChunk = "data: " + JSON.stringify({ id: "c", object: "chat.completion.chunk", created: 1, model: "g", choices: [{ index: 0, delta: { content: "x" }, finish_reason: null }] }) + "\n\n";
pending = new Map(), held = new Set();
srv = new TCPServer({ port: 0 });
srv.start({
    connect: () => { conns++; },
    data: (c, bytes) => {
        const got = (pending.get(c) || "") + dec.decode(bytes);
        const he = got.indexOf("\r\n\r\n");
        const cl = he >= 0 ? /\r\ncontent-length: *(\d+)/i.exec(got.slice(0, he)) : null;
        if (he < 0 || (cl && enc.encode(got.slice(he + 4)).length < Number(cl[1]))) { pending.set(c, got); return; }
        pending.delete(c);
        reqs++;
        const path = /^\w+ (\S+) /.exec(got)[1];
        if (path.startsWith("/hang/")) { held.add(c); return; }
        if (path.startsWith("/hold/")) {
            const ev = path.includes("/chat/") ? oaChunk : path.endsWith("/results") ? JSON.stringify({ custom_id: "a", result: { type: "canceled" } }) + "\n" : startEv;
            c.write(enc.encode("HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nConnection: close\r\n\r\n" + ev));
            held.add(c);
            return;
        }
        if (path.startsWith("/busy/")) c.write(reply(503, "application/json", JSON.stringify({ type: "error", error: { type: "overloaded_error", message: "busy" } }), "retry-after: 30\r\n"));
        else if (path.includes("/chat/")) c.write(reply(200, "application/json", JSON.stringify({ id: "c", object: "chat.completion", created: 1, model: "g", choices: [{ index: 0, message: { role: "assistant", content: "ok" }, finish_reason: "stop" }] })));
        else c.write(reply(200, "application/json", msgJson));
        if (c.bufferedAmount === 0) c.close(); else c.closeOnDrain = true;
    },
    drain: (c) => { if (c.closeOnDrain) c.close(); },
    close: (c) => { pending.delete(c); if (held.delete(c)) closed++; },
});
});

try {
    {
let r, r1, r2;
        const c0 = conns;
await __kitS.section("an already aborted signal rejects with APIError aborted", async () => {
        r = await outcome(ant("/ok").messages.create(body, { signal: AbortSignal.abort() }));
        ok(r.e && aborted(r.e), "an already aborted signal rejects with APIError aborted", desc(r));
        ok(conns === c0, "an already aborted signal never connects", conns - c0);
});
await __kitS.section("aborted is read up front: a signal that is already aborted rejects even if it never fires", async () => {
        r1 = await outcome(ant("/ok").messages.create(body, { signal: { aborted: true, addEventListener() {}, removeEventListener() {} } }));
        ok(r1.e && aborted(r1.e) && conns === c0, "aborted is read up front: a signal that is already aborted rejects even if it never fires", desc(r1));
});
await __kitS.section("OpenAI: an already aborted signal rejects with APIError aborted", async () => {
        r2 = await outcome(oai("/ok").chat.completions.create(chat, { signal: AbortSignal.abort() }));
        ok(r2.e && aborted(r2.e), "OpenAI: an already aborted signal rejects with APIError aborted", desc(r2));
});
    }
    {
let ac, p, r, ac2, p2, r2, r3;
        const c0 = closed;
await __kitS.section("an abort while waiting for the response head rejects promptly", async () => {
        ac = new AbortController();
        p = outcome(ant("/hang").messages.create(body, { signal: ac.signal }));
        await sleep(150);
        ac.abort();
        r = await p;
        ok(r.e && aborted(r.e) && r.ms < 3000, "an abort while waiting for the response head rejects promptly", desc(r));
});
await __kitS.section("the aborted connection is closed", async () => {
        await sleep(50);
        ok(closed > c0, "the aborted connection is closed");
});
await __kitS.section("OpenAI: an abort while waiting for the head rejects promptly", async () => {
        ac2 = new AbortController();
        p2 = outcome(oai("/hang").chat.completions.create(chat, { signal: ac2.signal }));
        await sleep(100);
        ac2.abort();
        r2 = await p2;
        ok(r2.e && aborted(r2.e) && r2.ms < 3000, "OpenAI: an abort while waiting for the head rejects promptly", desc(r2));
});
await __kitS.section("AbortSignal.timeout aborts a hung call", async () => {
        r3 = await outcome(ant("/hang").messages.create(body, { signal: AbortSignal.timeout(150) }));
        ok(r3.e && aborted(r3.e) && r3.ms < 3000, "AbortSignal.timeout aborts a hung call", desc(r3));
});
    }
    {
let ac, p, r;
        const r0 = reqs;
await __kitS.section("an abort during a retry-after backoff (30 s) rejects promptly, not with the 503", async () => {
        ac = new AbortController();
        p = outcome(ant("/busy", { maxRetries: 2 }).messages.create(body, { signal: ac.signal }));
        await sleep(250);
        ac.abort();
        r = await p;
        ok(r.e && aborted(r.e) && r.ms < 3000, "an abort during a retry-after backoff (30 s) rejects promptly, not with the 503", desc(r));
        ok(reqs - r0 === 1, "no retry is sent after the abort", reqs - r0);
});
    }
    {
let ac, s, first, p, r, r2, ac2, s2, r3, ac3, br, r4;
await __kitS.section("the held stream delivers its first event", async () => {
        ac = new AbortController();
        s = ant("/hold").messages.stream(body, { signal: ac.signal });
        first = await s.next();
        ok(first.value && first.value.type === "message_start", "the held stream delivers its first event");
});
await __kitS.section("an abort mid-read rejects the pending next() promptly", async () => {
        p = outcome(s.next());
        await sleep(100);
        ac.abort();
        r = await p;
        ok(r.e && aborted(r.e) && r.ms < 3000, "an abort mid-read rejects the pending next() promptly", desc(r));
});
await __kitS.section("the aborted stream keeps failing", async () => {
        r2 = await outcome(s.finalMessage());
        ok(r2.e instanceof APIError, "the aborted stream keeps failing", desc(r2));
});
await __kitS.section("OpenAI: the held stream delivers its first chunk", async () => {
        ac2 = new AbortController();
        s2 = oai("/hold").chat.completions.stream(chat, { signal: ac2.signal });
        ok((await s2.next()).value.object === "chat.completion.chunk", "OpenAI: the held stream delivers its first chunk");
});
await __kitS.section("OpenAI: an abort between reads fails the next read with aborted", async () => {
        ac2.abort();
        r3 = await outcome(s2.next());
        ok(r3.e && aborted(r3.e) && r3.ms < 3000, "OpenAI: an abort between reads fails the next read with aborted", desc(r3));
});
await __kitS.section("batch results: the held body delivers its first line", async () => {
        ac3 = new AbortController();
        br = ant("/hold").messages.batches.results("b1", { signal: ac3.signal });
        ok((await br.next()).value.custom_id === "a", "batch results: the held body delivers its first line");
});
await __kitS.section("batch results: after an abort the next read rejects with aborted, never a clean end", async () => {
        ac3.abort();
        r4 = await outcome(br.next());
        ok(r4.e && aborted(r4.e), "batch results: after an abort the next read rejects with aborted, never a clean end", desc(r4));
});
    }
    {
let calls, sig, f, ac, m;
await __kitS.section("each finished call, and a stream that failed, removes its own abort listener", async () => {
        calls = { add: 0, remove: 0, fns: new Set() };
        sig = { aborted: false,
            addEventListener(t, f) { if (t === "abort") { calls.add++; calls.fns.add(f); } },
            removeEventListener(t, f) { if (t === "abort" && calls.fns.delete(f)) calls.remove++; } };
        for (let i = 0; i < 5; i++) await ant("/ok").messages.create(body, { signal: sig });
        await oai("/ok").chat.completions.create(chat, { signal: sig });
        f = await ant("/ok").messages.stream({ ...body }, { signal: sig }).finalMessage().catch((e) => e);
        ok(calls.add === 7 && calls.remove === 7 && calls.fns.size === 0, "each finished call, and a stream that failed, removes its own abort listener",
            JSON.stringify({ add: calls.add, remove: calls.remove, left: calls.fns.size, stream: f && f.type }));
});
await __kitS.section("a call that completed is unaffected by a later abort", async () => {
        ac = new AbortController();
        m = await ant("/ok").messages.create(body, { signal: ac.signal });
        ac.abort();
        ok(m.content[0].text === "hi", "a call that completed is unaffected by a later abort");
});
    }
    {
let r;
await __kitS.section("a signal that is not an AbortSignal is a TypeError", async () => {
        for (const [bad, w] of [[{}, "an object without aborted"], [5, "a number"], [{ aborted: false }, "an object without addEventListener"]]) {
            const r = await outcome(ant("/ok").messages.create(body, { signal: bad }));
            ok(r.e instanceof TypeError && /signal must be an AbortSignal/.test(r.e.message), "signal: " + w + " is a TypeError", desc(r));
        }
});
await __kitS.section("signal: null means no signal", async () => {
        r = await outcome(ant("/ok").messages.create(body, { signal: null }));
        ok(r.v && r.v.type === "message", "signal: null means no signal");
});
    }
} finally { if (!__kitS.listing) {
    for (const c of held) c.close();
    srv.close();
} }
__kitS.end();
if (!__kitS.listing) {
print(`test_ai_anthropic_signal: ${pass} passed, ${fail} failed`);
if (fail) throw new Error("test_ai_anthropic_signal failed");
}
