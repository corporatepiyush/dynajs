// flags: --std
// timeout: 300
// covers: dyna:ai.MessageStream
// A response cut short (the peer closes after N bytes) never yields a silently truncated
// Message. A raw TCP mock answers each request with the first N bytes of a full wire
// response (head + body) and closes, for every N from 0 to the full length, in four framings:
// SSE to EOF, SSE with a Content-Length, SSE chunked one event per chunk, and a non-stream
// JSON body with a Content-Length. Each call either rejects with an APIError (type
// connection_error: unreachable head, truncated body or chunked framing, or the stream ended
// before message_stop) or resolves to exactly the full Message; a cut before the end of the
// message_stop event always rejects. maxRetries is 0 so every cut is seen once.
// A stream the server holds open delivers each event as it arrives (a read returns what is
// there, it never waits to fill its buffer), and return() closes the connection.
import { sections as __kitSections } from "../lib/kit.js";
import { Anthropic, APIError, MessageStream } from "dyna:ai";
import { TCPServer } from "dyna:net";
import * as std from "std";
const __kitS = __kitSections("test_ai_message_stream_cut", { chain: true });
let enc, dec, evBytes, sseBody, jsonBody, H_EOF, H_CL, H_CH, H_JS, chunked, stopEndSse, stopEndChunked, MODES, srv, pending, held;

let pass = 0, fail = 0;
const ok = (c, w, d) => { if (c) pass++; else { fail++; print("  FAIL  " + w + (d !== undefined ? "  [" + d + "]" : "")); } };
const M = "claude-opus-5-5";
const sseEv = ([e, d]) => `event: ${e}\ndata: ${JSON.stringify(d)}\n\n`;
const EVENTS = [
    ["message_start", { type: "message_start", message: { id: "msg_c1", type: "message", role: "assistant", model: M, content: [], stop_reason: null, stop_sequence: null, usage: { input_tokens: 7, output_tokens: 1 } } }],
    ["content_block_start", { type: "content_block_start", index: 0, content_block: { type: "text", text: "" } }],
    ["content_block_delta", { type: "content_block_delta", index: 0, delta: { type: "text_delta", text: "Hello, " } }],
    ["ping", { type: "ping" }],
    ["content_block_delta", { type: "content_block_delta", index: 0, delta: { type: "text_delta", text: "world \u{1F30D}." } }],
    ["content_block_stop", { type: "content_block_stop", index: 0 }],
    ["content_block_start", { type: "content_block_start", index: 1, content_block: { type: "tool_use", id: "toolu_c1", name: "lookup", input: {} } }],
    ["content_block_delta", { type: "content_block_delta", index: 1, delta: { type: "input_json_delta", partial_json: "{\"q\":" } }],
    ["content_block_delta", { type: "content_block_delta", index: 1, delta: { type: "input_json_delta", partial_json: "\"tides\"}" } }],
    ["content_block_stop", { type: "content_block_stop", index: 1 }],
    ["message_delta", { type: "message_delta", delta: { stop_reason: "tool_use", stop_sequence: null }, usage: { output_tokens: 31 } }],
    ["message_stop", { type: "message_stop" }],
];
const FULL = { id: "msg_c1", type: "message", role: "assistant", model: M,
    content: [{ type: "text", text: "Hello, world \u{1F30D}." }, { type: "tool_use", id: "toolu_c1", name: "lookup", input: { q: "tides" } }],
    stop_reason: "tool_use", stop_sequence: null, usage: { input_tokens: 7, output_tokens: 31 } };
const canon = (v) => JSON.stringify(v, (k, x) => (x && typeof x === "object" && !Array.isArray(x)) ? Object.fromEntries(Object.keys(x).sort().map((q) => [q, x[q]])) : x);

const cat = (...parts) => { const n = parts.reduce((a, p) => a + p.length, 0), o = new Uint8Array(n); let i = 0; for (const p of parts) { o.set(p, i); i += p.length; } return o; };
const head = (h) => enc.encode("HTTP/1.1 200 OK\r\n" + h + "Connection: close\r\n\r\n");
const chunk = (b) => cat(enc.encode(b.length.toString(16) + "\r\n"), b, enc.encode("\r\n"));
let heldClosed = 0;

const body = { model: M, max_tokens: 64, messages: [{ role: "user", content: "x" }] };
const tally = {};
__kitS.section("fixture: create the text encoder and decoder", () => {
enc = new TextEncoder(), dec = new TextDecoder();
evBytes = EVENTS.map((e) => enc.encode(sseEv(e)));
sseBody = cat(...evBytes);
jsonBody = enc.encode(JSON.stringify(FULL));

H_EOF = head("Content-Type: text/event-stream\r\n");
H_CL = head("Content-Type: text/event-stream\r\nContent-Length: " + sseBody.length + "\r\n");
H_CH = head("Content-Type: text/event-stream\r\nTransfer-Encoding: chunked\r\n");
H_JS = head("Content-Type: application/json\r\nContent-Length: " + jsonBody.length + "\r\n");
chunked = cat(...evBytes.map(chunk), enc.encode("0\r\n\r\n"));
stopEndSse = sseBody.length;
stopEndChunked = chunked.length - 5 - 2;
MODES = {
    eof: { wire: cat(H_EOF, sseBody), stopEnd: H_EOF.length + stopEndSse, stream: true },
    cl: { wire: cat(H_CL, sseBody), stopEnd: H_CL.length + stopEndSse, stream: true },
    chunked: { wire: cat(H_CH, chunked), stopEnd: H_CH.length + stopEndChunked, stream: true },
    json: { wire: cat(H_JS, jsonBody), stopEnd: H_JS.length + jsonBody.length, stream: false },
};

srv = new TCPServer({ port: 0 });
pending = new Map(), held = new Set();
srv.start({
    data: (c, bytes) => {
        const prev = pending.get(c) || "";
        const got = prev + dec.decode(bytes);
        const he = got.indexOf("\r\n\r\n");
        if (he < 0) { pending.set(c, got); return; }
        const cl = /\r\ncontent-length: *(\d+)/i.exec(got.slice(0, he));
        if (cl && enc.encode(got.slice(he + 4)).length < Number(cl[1])) { pending.set(c, got); return; }
        pending.delete(c);
        if (/^POST \/hold\/v1\/messages /.test(got)) {
            c.write(cat(H_EOF, ...evBytes.slice(0, 3)));
            held.add(c);
            return;
        }
        const m = /^POST \/cut\/(\w+)\/(\d+)\/v1\/messages /.exec(got);
        const wire = m ? MODES[m[1]].wire.subarray(0, Number(m[2])) : enc.encode("HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
        if (wire.length) c.write(wire);
        if (c.bufferedAmount === 0) c.close(); else c.closeOnDrain = true;
    },
    drain: (c) => { if (c.closeOnDrain) c.close(); },
    close: (c) => { pending.delete(c); if (held.delete(c)) heldClosed++; },
});
});
try {
await __kitS.section("rejects with an APIError connection_error", async () => {
    for (const [mode, { wire, stopEnd, stream }] of Object.entries(MODES)) {
        const t = tally[mode] = { rejected: 0, resolved: 0, bad: 0 };
        for (let n = 0; n <= wire.length; n++) {
            const c = new Anthropic({ apiKey: "test-key", baseURL: "http://127.0.0.1:" + srv.port + "/cut/" + mode + "/" + n, maxRetries: 0 });
            let res, err;
            try {
                res = stream ? await c.messages.stream(body).finalMessage() : await c.messages.create(body);
            } catch (e) { err = e; }
            if (err) {
                t.rejected++;
                const good = err instanceof APIError && err.type === "connection_error" && !err.status;
                if (!good) { t.bad++; ok(false, mode + " cut at " + n + ": rejects with an APIError connection_error", err && (err.constructor.name + ": " + err.message + " type=" + err.type + " status=" + err.status)); }
            } else {
                t.resolved++;
                if (n < stopEnd || canon(res) !== canon(FULL)) { t.bad++; ok(false, mode + " cut at " + n + " of " + wire.length + " (message_stop ends at " + stopEnd + ") must not resolve to " + JSON.stringify(res).slice(0, 160)); }
            }
        }
        ok(t.bad === 0, mode + ": every one of " + (wire.length + 1) + " cuts rejects or yields the full Message", JSON.stringify(t));
        ok(t.rejected >= stopEnd, mode + ": every cut before the end of the body rejects (" + t.rejected + " >= " + stopEnd + ")");
        ok(t.resolved >= 1, mode + ": the uncut response resolves");
    }
});
    {
let c, s;
        const seen = [];
        let err;
__kitS.section("messages.stream() returns a MessageStream", () => {
        c = new Anthropic({ apiKey: "test-key", baseURL: "http://127.0.0.1:" + srv.port + "/cut/eof/" + (MODES.eof.stopEnd - 1), maxRetries: 0 });
        s = c.messages.stream(body);
        ok(s instanceof MessageStream, "messages.stream() returns a MessageStream");
});
        let fe;
await __kitS.section("iterating a stream cut inside message_stop throws, naming message_stop", async () => {
        try { for await (const ev of s) seen.push(ev.type); } catch (e) { err = e; }
        ok(err instanceof APIError && err.type === "connection_error" && /message_stop/.test(err.message), "iterating a stream cut inside message_stop throws, naming message_stop", err && err.message);
        ok(seen.includes("message_delta") && !seen.includes("message_stop"), "the events before the cut were delivered", seen.join(","));
});
await __kitS.section("finalMessage() after the cut rejects too", async () => {
        try { await s.finalMessage(); } catch (e) { fe = e; }
        ok(fe instanceof APIError, "finalMessage() after the cut rejects too", fe && fe.message);
});
    }
    {
let c, c2;
await __kitS.section("the uncut stream's finalText is the whole text", async () => {
        c = new Anthropic({ apiKey: "test-key", baseURL: "http://127.0.0.1:" + srv.port + "/cut/eof/" + MODES.eof.wire.length, maxRetries: 0 });
        ok(await c.messages.stream(body).finalText() === "Hello, world \u{1F30D}.", "the uncut stream's finalText is the whole text");
});
        let fe;
await __kitS.section("finalText() on a cut stream rejects", async () => {
        c2 = new Anthropic({ apiKey: "test-key", baseURL: "http://127.0.0.1:" + srv.port + "/cut/eof/" + (MODES.eof.wire.length - 40), maxRetries: 0 });
        try { await c2.messages.stream(body).finalText(); } catch (e) { fe = e; }
        ok(fe instanceof APIError && fe.type === "connection_error", "finalText() on a cut stream rejects", fe && fe.message);
});
    }
    {
let c, s, t0, seen;
await __kitS.section("fixture: create the client against the local server", async () => {
        c = new Anthropic({ apiKey: "test-key", baseURL: "http://127.0.0.1:" + srv.port + "/hold", maxRetries: 0, timeout: 20000 });
        s = c.messages.stream(body);
        t0 = Date.now(), seen = [];
        for (let i = 0; i < 3; i++) seen.push((await s.next()).value.type);
});
__kitS.section("events are delivered as they arrive while the stream is held open", () => {
        // wall-clock: a deadline/timeout check; the bound sits far above the deadline and below the hang it guards against, so load only narrows the gap
        ok(seen.join(",") === "message_start,content_block_start,content_block_delta" && Date.now() - t0 < 5000,
            "events are delivered as they arrive, while the server holds the stream open", seen.join(",") + " in " + (Date.now() - t0) + " ms");
        ok(s.message && s.message.content[0].text === "Hello, ", "the partial message reflects the delivered delta", JSON.stringify(s.message && s.message.content));
});
await __kitS.section("return() closes the held connection", async () => {
        await s.return();
        for (let i = 0; i < 200 && heldClosed === 0; i++) await new Promise((r) => setTimeout(r, 10));
        ok(heldClosed === 1, "return() closes the held connection");
});
    }
__kitS.section("report: print the cut tally", () => {
    print("  cuts: " + JSON.stringify(tally));
});
} finally { if (!__kitS.listing) {
    srv.close();
} }
__kitS.end();
if (!__kitS.listing) {
print(`test_ai_message_stream_cut: ${pass} passed, ${fail} failed`);
std.exit(fail ? 1 : 0);
}
