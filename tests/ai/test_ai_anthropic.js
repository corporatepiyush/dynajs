// flags: --std
// timeout: 240
// covers: dyna:ai.Anthropic
// dyna:ai's Anthropic client against an in-process dyna:http App that speaks
// the Messages API wire format (platform.claude.com/docs/en/api/messages/
// create.md, build-with-claude/streaming.md, api/errors.md). No live calls and
// no real key: every request goes to 127.0.0.1 with apiKey "test-key". The
// client runs its I/O on a fiber offloaded to a worker thread, which is why
// the App on the same event loop can answer it.
// The mock enforces three anthropic-version 2026-10-21 rules so the suite pins
// that the client passes bodies through and surfaces the API's 400 as
// APIError instead of rewriting the request: temperature/top_p/top_k, a
// document block in an assistant message, and a user text block with
// non-empty citations. With apiVersion "2023-06-01" the mock accepts
// temperature, which pins that the option reaches the wire.
import { sections as __kitSections } from "../lib/kit.js";
import { Anthropic, APIError, MessageStream } from "dyna:ai";
import { App } from "dyna:http";
import { getEnv } from "dyna:sys";
import * as std from "std";
const __kitS = __kitSections("test_ai_anthropic", { chain: true });
let plan, app, BASE;

let pass = 0, fail = 0, skip = 0;
const ok = (c, w, d) => { if (c) { pass++; print("  ok    " + w); }
                          else { fail++; print("  FAIL  " + w + (d !== undefined ? "  [" + d + "]" : "")); } };
const rejects = async (p, pred, w) => {
    try { await p; ok(false, w, "resolved"); }
    catch (e) { ok(pred(e), w, e && (e.constructor.name + ": " + e.message + (e.status !== undefined ? " status=" + e.status + " type=" + e.type : ""))); }
};
const sse = (evs) => evs.map(([e, d]) => `event: ${e}\ndata: ${typeof d === "string" ? d : JSON.stringify(d)}\n\n`).join("");
const startMsg = (model) => ["message_start", { type: "message_start", message: { id: "msg_s1", type: "message", role: "assistant", model, content: [], stop_reason: null, stop_sequence: null, usage: { input_tokens: 9, output_tokens: 1 } } }];
const textStream = (model, text, stop = "end_turn") => [
    startMsg(model),
    ["content_block_start", { type: "content_block_start", index: 0, content_block: { type: "text", text: "" } }],
    ["content_block_delta", { type: "content_block_delta", index: 0, delta: { type: "text_delta", text: text.slice(0, 3) } }],
    ["ping", { type: "ping" }],
    ["content_block_delta", { type: "content_block_delta", index: 0, delta: { type: "text_delta", text: text.slice(3) } }],
    ["content_block_stop", { type: "content_block_stop", index: 0 }],
    ["message_delta", { type: "message_delta", delta: { stop_reason: stop, stop_sequence: null }, usage: { output_tokens: 11 } }],
    ["message_stop", { type: "message_stop" }],
];
const err = (status, type, message, headers) => ({ status, contentType: "application/json", headers: { "request-id": "req_" + status, ...(headers || {}) },
    body: JSON.stringify({ type: "error", error: { type, message }, request_id: "req_body_" + status }) });

function violates2026(b) {
    for (const k of ["temperature", "top_p", "top_k"]) if (k in b) return k + ": not supported on this API version";
    for (const m of b.messages || []) {
        if (!Array.isArray(m.content)) continue;
        for (const blk of m.content) {
            if (m.role === "assistant" && blk.type === "document") return "messages: document blocks are not allowed in assistant messages";
            if (m.role === "user" && blk.type === "text" && Array.isArray(blk.citations) && blk.citations.length) return "messages: text blocks with citations are not allowed in user messages";
        }
    }
    return null;
}

const log = [];
const M = "claude-opus-5-5";
const user = (t) => [{ role: "user", content: t }];
const client = (o = {}) => new Anthropic({ apiKey: "test-key", baseURL: BASE, ...o });
const last = () => log[log.length - 1];
__kitS.section("fixture: create the scripted response plan", () => {
plan = new Map();
app = new App({ port: 0, host: "127.0.0.1" });
app.post("/v1/messages", (req) => {
    const b = JSON.parse(req.body);
    log.push({ path: req.path, headers: req.headers, body: b, raw: req.body });
    const script = plan.get(b.model);
    if (script && script.length) {
        const step = script.shift();
        return typeof step === "function" ? step(req, b) : step;
    }
    if (req.headers["anthropic-version"] === "2026-10-21") {
        const v = violates2026(b);
        if (v) return err(400, "invalid_request_error", v);
    }
    if (b.stream) return { status: 200, contentType: "text/event-stream", body: sse(textStream(b.model, "Hello world")) };
    return { status: 200, contentType: "application/json", headers: { "request-id": "req_ok" },
        body: JSON.stringify({ id: "msg_1", type: "message", role: "assistant", model: b.model, content: [{ type: "text", text: "hi" }], stop_reason: "end_turn", stop_sequence: null, usage: { input_tokens: 3, output_tokens: 1 } }) };
});
app.post("/v1/messages/count_tokens", (req) => {
    log.push({ path: req.path, headers: req.headers, body: JSON.parse(req.body) });
    return { status: 200, contentType: "application/json", body: JSON.stringify({ input_tokens: 42 }) };
});
app.get("/v1/models", (req) => {
    log.push({ path: req.path, query: req.query, headers: req.headers });
    return { status: 200, contentType: "application/json", body: JSON.stringify({ data: [{ id: "claude-opus-5-5", type: "model" }], has_more: false, first_id: "claude-opus-5-5", last_id: "claude-opus-5-5" }) };
});
app.get("/v1/models/:id", (req) => {
    log.push({ path: req.path, params: req.params, headers: req.headers });
    if (req.params.id === "nope") return err(404, "not_found_error", "model: nope");
    return { status: 200, contentType: "application/json", body: JSON.stringify({ id: req.params.id, type: "model" }) };
});
app.start();
BASE = "http://127.0.0.1:" + app.port;
});

try {
    {
let c, m, h;
await __kitS.section("create resolves the Message", async () => {
        c = client({ betas: ["files-api-x"] });
        log.length = 0;
        m = await c.messages.create({ model: M, max_tokens: 64, messages: user("x"), thinking: { type: "adaptive" }, output_config: { effort: "high" } }, { betas: ["b2"], headers: { "x-trace": "t1" } });
        h = last().headers;
        ok(m.content[0].text === "hi" && m.stop_reason === "end_turn", "create resolves the Message");
        ok(h["x-api-key"] === "test-key" && !("authorization" in h), "x-api-key carries the key");
        ok(h["anthropic-version"] === "2026-10-21", "anthropic-version defaults to 2026-10-21", h["anthropic-version"]);
        ok(h["content-type"] === "application/json" && h["accept"] === "application/json", "JSON content-type and accept");
        ok(h["anthropic-beta"] === "files-api-x,b2", "client betas then request betas in anthropic-beta", h["anthropic-beta"]);
        ok(h["x-trace"] === "t1", "per-request headers are sent");
        ok(JSON.stringify(last().body.thinking) === '{"type":"adaptive"}' && last().body.output_config.effort === "high", "thinking and output_config pass through as given");
        ok(!("stream" in last().body), "create sends no stream key");
        ok(c.baseURL === BASE && c.apiVersion === "2026-10-21" && c.maxRetries === 2 && c.timeout === 600000, "client getters report the defaults");
});
    }
    {
let c;
await __kitS.section("authToken sends Authorization: Bearer", async () => {
        log.length = 0;
        await client({ apiKey: undefined, authToken: "tok-1" }).messages.create({ model: M, max_tokens: 1, messages: user("x") });
        ok(last().headers["authorization"] === "Bearer tok-1" && !("x-api-key" in last().headers), "authToken sends Authorization: Bearer");
});
__kitS.section("trailing slashes on baseURL are dropped", () => {
        c = new Anthropic({ apiKey: "test-key", baseURL: BASE + "///" });
        ok(c.baseURL === BASE, "trailing slashes on baseURL are dropped", c.baseURL);
});
    }
    {
let m;
await __kitS.section("2026-10-21: temperature is sent as given and the API's 400 surfaces as APIError", async () => {
        await rejects(client().messages.create({ model: M, max_tokens: 64, messages: user("x"), temperature: 0.2 }),
            (e) => e instanceof APIError && e.status === 400 && e.type === "invalid_request_error" && /temperature/.test(e.message) && e.requestId === "req_body_400" && e.retryable === false,
            "2026-10-21: temperature is sent as given and the API's 400 surfaces as APIError");
});
await __kitS.section("the client did not strip temperature", async () => {
        ok("temperature" in last().body, "the client did not strip temperature");
        await rejects(client().messages.create({ model: M, max_tokens: 8, messages: [{ role: "user", content: "q" }, { role: "assistant", content: [{ type: "document", source: { type: "text", media_type: "text/plain", data: "d" } }] }] }),
            (e) => e instanceof APIError && e.status === 400 && /assistant/.test(e.message), "2026-10-21: a document block in an assistant message is a 400 APIError");
        await rejects(client().messages.create({ model: M, max_tokens: 8, messages: [{ role: "user", content: [{ type: "text", text: "t", citations: [{ type: "char_location" }] }] }] }),
            (e) => e instanceof APIError && e.status === 400 && /citations/.test(e.message), "2026-10-21: a user text block with citations is a 400 APIError");
});
await __kitS.section("apiVersion pins the header (the older contract accepts temperature)", async () => {
        log.length = 0;
        m = await client({ apiVersion: "2023-06-01" }).messages.create({ model: M, max_tokens: 8, messages: user("x"), temperature: 0.2 });
        ok(m.type === "message" && last().headers["anthropic-version"] === "2023-06-01", "apiVersion pins the header (the older contract accepts temperature)");
});
    }
    {
let m, m2, r;
await __kitS.section("429 with retry-after: 0 is retried once, then succeeds", async () => {
        plan.set("m-429", [err(429, "rate_limit_error", "slow down", { "retry-after": "0" })]);
        log.length = 0;
        m = await client().messages.create({ model: "m-429", max_tokens: 1, messages: user("x") });
        ok(m.type === "message" && log.length === 2, "429 with retry-after: 0 is retried once, then succeeds", log.length);
});
await __kitS.section("maxRetries 0: the 429 rejects with retryAfterMs, headers, body and the typed message", async () => {
        plan.set("m-429b", [err(429, "rate_limit_error", "slow down", { "retry-after": "7" })]);
        await rejects(client({ maxRetries: 0 }).messages.create({ model: "m-429b", max_tokens: 1, messages: user("x") }),
            (e) => e instanceof APIError && e.status === 429 && e.type === "rate_limit_error" && e.retryAfterMs === 7000 && e.retryable === true
                && e.headers["retry-after"] === "7" && e.body.error.message === "slow down" && /^429 rate_limit_error: slow down/.test(e.message),
            "maxRetries 0: the 429 rejects with retryAfterMs, headers, body and the typed message");
});
await __kitS.section("529 then 500 are retried (default maxRetries 2)", async () => {
        plan.set("m-529", [err(529, "overloaded_error", "Overloaded", { "retry-after-ms": "5" }), err(500, "api_error", "boom", { "retry-after": "0" })]);
        log.length = 0;
        m2 = await client().messages.create({ model: "m-529", max_tokens: 1, messages: user("x") });
        ok(m2.type === "message" && log.length === 3, "529 then 500 are retried (default maxRetries 2)", log.length);
});
await __kitS.section("retries exhausted: the last answer rejects", async () => {
        plan.set("m-500", [err(500, "api_error", "a", { "retry-after": "0" }), err(500, "api_error", "b", { "retry-after": "0" })]);
        log.length = 0;
        await rejects(client({ maxRetries: 1 }).messages.create({ model: "m-500", max_tokens: 1, messages: user("x") }),
            (e) => e instanceof APIError && e.status === 500 && /b/.test(e.message), "retries exhausted: the last answer rejects");
        ok(log.length === 2, "maxRetries 1 makes two attempts", log.length);
});
await __kitS.section("a 400 is not retried", async () => {
        plan.set("m-400", [err(400, "invalid_request_error", "bad")]);
        log.length = 0;
        await rejects(client().messages.create({ model: "m-400", max_tokens: 1, messages: user("x") }), (e) => e.status === 400, "a 400 is not retried");
        ok(log.length === 1, "one attempt for a 400", log.length);
});
await __kitS.section("401 authentication_error", async () => {
        plan.set("m-401", [err(401, "authentication_error", "invalid x-api-key")]);
        await rejects(client().messages.create({ model: "m-401", max_tokens: 1, messages: user("x") }),
            (e) => e.status === 401 && e.type === "authentication_error", "401 authentication_error");
});
await __kitS.section("a non-JSON error body still gives a typed APIError", async () => {
        plan.set("m-html", [{ status: 502, contentType: "text/html", body: "<html>bad gateway</html>", headers: { "retry-after": "0" } }]);
        await rejects(client({ maxRetries: 0 }).messages.create({ model: "m-html", max_tokens: 1, messages: user("x") }),
            (e) => e instanceof APIError && e.status === 502 && e.type === "api_error" && /bad gateway/.test(e.message) && e.body === null, "a non-JSON error body still gives a typed APIError");
});
await __kitS.section("a refusal is a 200 Message with stop_details", async () => {
        plan.set("m-refuse", [{ status: 200, contentType: "application/json", body: JSON.stringify({ id: "m", type: "message", role: "assistant", content: [], stop_reason: "refusal", stop_sequence: null, stop_details: { type: "refusal", category: "cyber", explanation: "no" }, usage: { input_tokens: 1, output_tokens: 0 } }) }]);
        r = await client().messages.create({ model: "m-refuse", max_tokens: 1, messages: user("x") });
        ok(r.stop_reason === "refusal" && r.stop_details.category === "cyber", "a refusal is a 200 Message with stop_details");
});
    }
    {
let c;
        let t = null;
await __kitS.section("a refused connection rejects with connection_error", async () => {
        await rejects(new Anthropic({ apiKey: "test-key", baseURL: "http://127.0.0.1:9", maxRetries: 0 }).messages.create({ model: M, max_tokens: 1, messages: user("x") }),
            (e) => e instanceof APIError && e.status === 0 && e.type === "connection_error" && e.retryable === true && /connect/.test(e.message), "a refused connection rejects with connection_error");
        await rejects(client().messages.create({ max_tokens: 1, messages: user("x") }), (e) => e instanceof TypeError && /no default model/.test(e.message), "model is required: there is no default");
        await rejects(client().messages.create({ model: M, messages: user("x") }), (e) => e instanceof TypeError && /max_tokens/.test(e.message), "max_tokens is required");
        await rejects(client().messages.create({ model: M, max_tokens: 1 }), (e) => e instanceof TypeError && /messages/.test(e.message), "messages is required");
        await rejects(client().messages.create({ model: M, max_tokens: 1, messages: [], stream: true }), (e) => e instanceof TypeError && /messages.stream/.test(e.message), "create refuses stream: true");
        await rejects(client().messages.create({ model: M, max_tokens: 1, messages: [] }, { nope: 1 }), (e) => e instanceof TypeError && /unknown option "nope"/.test(e.message), "an unknown request option is a TypeError");
});
__kitS.section("an unknown client option names the valid ones", () => {
        try { new Anthropic({ apiKey: "k", baseUrl: BASE }); } catch (e) { t = e; }
        ok(t instanceof TypeError && /unknown option "baseUrl" \(valid: apiKey, authToken, baseURL, apiVersion, maxRetries, timeout, betas, headers, ca\)/.test(t.message), "an unknown client option names the valid ones", t && t.message);
});
__kitS.section("a non-http baseURL is a TypeError", () => {
        t = null;
        try { new Anthropic({ apiKey: "k", baseURL: "ftp://x" }); } catch (e) { t = e; }
        ok(t instanceof TypeError && /baseURL/.test(t.message), "a non-http baseURL is a TypeError");
});
__kitS.section("maxRetries above 10 is a RangeError", () => {
        t = null;
        try { new Anthropic({ apiKey: "k", maxRetries: 11 }); } catch (e) { t = e; }
        ok(t instanceof RangeError, "maxRetries above 10 is a RangeError");
});
await __kitS.section("no key and no token rejects with TypeError", async () => {
        if (getEnv("ANTHROPIC_API_KEY") || getEnv("ANTHROPIC_AUTH_TOKEN")) { skip++; print("  SKIP  no-credential row: the environment has a credential"); }
        else await rejects(new Anthropic({ baseURL: BASE }).messages.create({ model: M, max_tokens: 1, messages: [] }),
            (e) => e instanceof TypeError && /no credentials/.test(e.message), "no key and no token rejects with TypeError");
});
await __kitS.section("a closed client refuses calls", async () => {
        c = client();
        c.close();
        await rejects(c.messages.create({ model: M, max_tokens: 1, messages: [] }), (e) => e instanceof TypeError && /closed/.test(e.message), "a closed client refuses calls");
});
    }
    {
let s, fm, s2, p1;
        const types = [];
__kitS.section("stream() returns a MessageStream before any I/O", () => {
        log.length = 0;
        s = client().messages.stream({ model: M, max_tokens: 64, messages: user("x"), stream: false });
        ok(s instanceof MessageStream && s.status === null && s.message === null, "stream() returns a MessageStream before any I/O");
});
await __kitS.section("for await yields every event in order", async () => {
        for await (const ev of s) types.push(ev.type);
        ok(types.join(",") === "message_start,content_block_start,content_block_delta,ping,content_block_delta,content_block_stop,message_delta,message_stop", "for await yields every event in order", types.join(","));
        ok(last().body.stream === true && last().headers["accept"] === "text/event-stream", "stream sends stream: true (overriding the body's) and accepts text/event-stream");
        ok(s.message.content[0].text === "Hello world" && s.message.usage.output_tokens === 11 && s.status === 200, "the accumulated Message and status");
        ok(await client().messages.stream({ model: M, max_tokens: 64, messages: user("x") }).finalText() === "Hello world", "finalText()");
});
await __kitS.section("finalMessage()", async () => {
        fm = await client().messages.stream({ model: M, max_tokens: 64, messages: user("x") }).finalMessage();
        ok(fm.stop_reason === "end_turn" && fm.content.length === 1, "finalMessage()");
});
await __kitS.section("a second next() while one is pending is refused", async () => {
        s2 = client().messages.stream({ model: M, max_tokens: 64, messages: user("x") });
        p1 = s2.next();
        await rejects(s2.next(), (e) => e instanceof TypeError && /one read at a time/.test(e.message), "a second next() while one is pending is refused");
        ok((await p1).value.type === "message_start", "the pending next() still resolves");
});
await __kitS.section("breaking out of for await closes the stream", async () => {
        for await (const ev of s2) { if (ev.type === "content_block_delta") break; }
        ok((await s2.next()).done === true, "breaking out of for await closes the stream");
});
    }
    {
let s, s4;
await __kitS.section("a mid-stream error event rejects with APIError 529", async () => {
        plan.set("m-midstream", [{ status: 200, contentType: "text/event-stream", body: sse([startMsg("m"),
            ["content_block_start", { type: "content_block_start", index: 0, content_block: { type: "text", text: "" } }],
            ["content_block_delta", { type: "content_block_delta", index: 0, delta: { type: "text_delta", text: "partial" } }],
            ["error", { type: "error", error: { type: "overloaded_error", message: "Overloaded" } }]]) }]);
        s = client().messages.stream({ model: "m-midstream", max_tokens: 9, messages: user("x") });
        await rejects(s.finalMessage(), (e) => e instanceof APIError && e.status === 529 && e.type === "overloaded_error", "a mid-stream error event rejects with APIError 529");
});
await __kitS.section("the partial message stays readable after the error", async () => {
        ok(s.message.content[0].text === "partial", "the partial message stays readable after the error");
        await rejects(s.finalMessage(), (e) => e instanceof APIError, "a failed stream keeps failing");
});
await __kitS.section("a body that ends before message_stop rejects", async () => {
        plan.set("m-trunc", [{ status: 200, contentType: "text/event-stream", body: sse(textStream("m", "Hello world").slice(0, 5)) }]);
        await rejects(client().messages.stream({ model: "m-trunc", max_tokens: 9, messages: user("x") }).finalMessage(),
            (e) => e instanceof APIError && e.type === "connection_error" && /before message_stop/.test(e.message), "a body that ends before message_stop rejects");
});
await __kitS.section("a malformed event rejects with stream_error", async () => {
        plan.set("m-bad", [{ status: 200, contentType: "text/event-stream", body: sse([startMsg("m"), ["content_block_delta", "{oops"]]) }]);
        await rejects(client().messages.stream({ model: "m-bad", max_tokens: 9, messages: user("x") }).finalMessage(),
            (e) => e instanceof APIError && e.type === "stream_error", "a malformed event rejects with stream_error");
});
await __kitS.section("a stream is retried while no response head has arrived (429 then 200)", async () => {
        plan.set("m-s429", [err(429, "rate_limit_error", "later", { "retry-after": "0" })]);
        log.length = 0;
        ok(await client().messages.stream({ model: "m-s429", max_tokens: 9, messages: user("x") }).finalText() === "Hello world" && log.length === 2,
            "a stream is retried while no response head has arrived (429 then 200)", log.length);
});
await __kitS.section("a stream's HTTP error rejects the first next()", async () => {
        plan.set("m-s401", [err(401, "authentication_error", "bad key")]);
        s4 = client().messages.stream({ model: "m-s401", max_tokens: 9, messages: user("x") });
        await rejects(s4.next(), (e) => e instanceof APIError && e.status === 401 && e.requestId === "req_body_401", "a stream's HTTP error rejects the first next()");
});
    }
    {
let c, msgs, first, uses, second;
        const tools = [{ name: "get_weather", description: "weather", eager_input_streaming: true, input_schema: { type: "object", properties: { location: { type: "string" } }, required: ["location"] } },
                       { name: "get_time", description: "time", input_schema: { type: "object", properties: {} } }];
await __kitS.section("parallel tool_use blocks with streamed input", async () => {
        plan.set("m-tools", [{ status: 200, contentType: "text/event-stream", body: sse([startMsg("m-tools"),
            ["content_block_start", { type: "content_block_start", index: 0, content_block: { type: "tool_use", id: "toolu_a", name: "get_weather", input: {} } }],
            ["content_block_delta", { type: "content_block_delta", index: 0, delta: { type: "input_json_delta", partial_json: "{\"location\": \"Pa" } }],
            ["content_block_delta", { type: "content_block_delta", index: 0, delta: { type: "input_json_delta", partial_json: "ris\"}" } }],
            ["content_block_stop", { type: "content_block_stop", index: 0 }],
            ["content_block_start", { type: "content_block_start", index: 1, content_block: { type: "tool_use", id: "toolu_b", name: "get_time", input: {} } }],
            ["content_block_stop", { type: "content_block_stop", index: 1 }],
            ["message_delta", { type: "message_delta", delta: { stop_reason: "tool_use", stop_sequence: null }, usage: { output_tokens: 30 } }],
            ["message_stop", { type: "message_stop" }]]) },
            (req, b) => {
                const lastMsg = b.messages[b.messages.length - 1];
                const results = Array.isArray(lastMsg.content) ? lastMsg.content.filter((x) => x.type === "tool_result") : [];
                const okShape = lastMsg.role === "user" && results.length === 2 && results[1].is_error === true
                    && b.messages[1].content[0].input.location === "Paris" && JSON.stringify(b.tools) === JSON.stringify(tools);
                return { status: 200, contentType: "text/event-stream", body: sse(textStream("m-tools", okShape ? "tool round trip ok" : "bad shape")) };
            }]);
        c = client();
        msgs = user("weather and time?");
        first = await c.messages.stream({ model: "m-tools", max_tokens: 100, messages: msgs, tools }).finalMessage();
        uses = first.content.filter((b) => b.type === "tool_use");
        ok(first.stop_reason === "tool_use" && uses.length === 2 && uses[0].input.location === "Paris", "parallel tool_use blocks with streamed input");
});
await __kitS.section("tool round trip: both results in one user message, is_error kept, tools sent verbatim", async () => {
        msgs.push({ role: "assistant", content: first.content });
        msgs.push({ role: "user", content: [
            { type: "tool_result", tool_use_id: uses[0].id, content: "18C" },
            { type: "tool_result", tool_use_id: uses[1].id, is_error: true, content: "clock unavailable" }] });
        second = await c.messages.stream({ model: "m-tools", max_tokens: 100, messages: msgs, tools }).finalText();
        ok(second === "tool round trip ok", "tool round trip: both results in one user message, is_error kept, tools sent verbatim", second);
});
    }
    {
let n, page, one;
await __kitS.section("countTokens posts to /v1/messages/count_tokens", async () => {
        log.length = 0;
        n = await client().messages.countTokens({ model: M, messages: user("count me"), system: "s" });
        ok(n.input_tokens === 42 && last().path === "/v1/messages/count_tokens" && last().body.system === "s" && !("max_tokens" in last().body), "countTokens posts to /v1/messages/count_tokens");
});
await __kitS.section("models.list sends its query percent-encoded", async () => {
        page = await client().models.list({ limit: 2, after_id: "a b&c" });
        ok(page.data[0].id === M && last().query.limit === "2" && last().query.after_id === "a b&c", "models.list sends its query percent-encoded", JSON.stringify(last().query));
});
await __kitS.section("claude-haiku-5-5", async () => {
        one = await client().models.retrieve("claude-haiku-5-5");
        ok(one.id === "claude-haiku-5-5" && last().params.id === "claude-haiku-5-5", "models.retrieve");
        await rejects(client().models.retrieve("nope"), (e) => e instanceof APIError && e.status === 404 && e.type === "not_found_error", "models.retrieve 404 is a typed APIError");
        await rejects(client().models.retrieve(""), (e) => e instanceof TypeError, "models.retrieve refuses an empty id");
});
    }
    {
let ms, m, p, cyc;
await __kitS.section("client.messages outlives a collected client object", async () => {
        ms = client().messages;
        std.gc();
        m = await ms.create({ model: M, max_tokens: 1, messages: user("x") });
        ok(m.type === "message", "client.messages outlives a collected client object");
});
await __kitS.section("a stream collected mid-read still completes (the read pins it)", async () => {
        p = client().messages.stream({ model: M, max_tokens: 9, messages: user("x") }).finalText();
        std.gc();
        ok(await p === "Hello world", "a stream collected mid-read still completes (the read pins it)");
});
await __kitS.section("a cycle through the held Message (collected at exit only if the GC can trace it)", async () => {
        cyc = client().messages.stream({ model: M, max_tokens: 9, messages: user("x") });
        await cyc.finalMessage();
        cyc.message.back = cyc;
        ok(cyc.message.back === cyc, "a cycle through the held Message (collected at exit only if the GC can trace it)");
});
    }
__kitS.section("APIError is constructible and an Error", () => {
    {
        const e = new APIError("custom", { status: 503, type: "api_error", requestId: "r", retryAfterMs: 10 });
        ok(e instanceof Error && e instanceof APIError && e.name === "APIError" && e.message === "custom" && e.retryable && e.retryAfterMs === 10, "APIError is constructible and an Error");
    }
});
} finally { if (!__kitS.listing) {
    app.close();
} }
__kitS.end();
if (!__kitS.listing) {
print(`test_ai_anthropic: ${pass} passed, ${fail} failed, ${skip} skipped`);
if (fail) throw new Error("test_ai_anthropic failed");
}
