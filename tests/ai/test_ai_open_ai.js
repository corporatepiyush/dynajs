// flags: --std
// timeout: 240
// covers: dyna:ai.OpenAI
// dyna:ai's OpenAI-compatible client against an in-process dyna:http App that
// speaks the Chat Completions wire format (POST {baseURL}/chat/completions,
// SSE "data: <chunk>" lines ending with "data: [DONE]", errors as
// {"error": {message, type, param, code}} with an x-request-id header). No
// live calls and no real key: every request goes to 127.0.0.1. The client runs
// its I/O on a fiber offloaded to a worker thread, so the App on the same
// event loop can answer it. Pins: Bearer auth plus OpenAI-Organization /
// OpenAI-Project, bodies passed through untouched (tools, tool_choice,
// response_format), stream_options.include_usage added only when the caller
// set no stream_options, chunk accumulation (content, reasoning, parallel
// tool calls whose id/name arrive once and arguments in pieces, n > 1
// choices, usage from the final choices-less chunk), [DONE] as the only clean
// end, mid-stream error objects, and retries through the shared HTTP layer.
import { sections as __kitSections } from "../lib/kit.js";
import { OpenAI, APIError, ChatCompletionStream } from "dyna:ai";
import { App } from "dyna:http";
import { getEnv } from "dyna:sys";
import * as std from "std";
const __kitS = __kitSections("test_ai_open_ai", { chain: true });
let plan, app, BASE;

let pass = 0, fail = 0, skip = 0;
const ok = (c, w, d) => { if (c) { pass++; print("  ok    " + w); }
                          else { fail++; print("  FAIL  " + w + (d !== undefined ? "  [" + d + "]" : "")); } };
const rejects = async (p, pred, w) => {
    try { await p; ok(false, w, "resolved"); }
    catch (e) { ok(pred(e), w, e && (e.constructor.name + ": " + e.message + (e.status !== undefined ? " status=" + e.status + " type=" + e.type : ""))); }
};
const sse = (chunks, done = true) => chunks.map((c) => "data: " + (typeof c === "string" ? c : JSON.stringify(c)) + "\n\n").join("") + (done ? "data: [DONE]\n\n" : "");
const chunk = (model, choices, extra) => ({ id: "chatcmpl-s1", object: "chat.completion.chunk", created: 1700000000, model, system_fingerprint: "fp_x", choices, ...(extra || {}) });
const textChunks = (model, text, usage = true) => [
    chunk(model, [{ index: 0, delta: { role: "assistant", content: "" }, finish_reason: null }]),
    chunk(model, [{ index: 0, delta: { content: text.slice(0, 3) }, finish_reason: null }]),
    chunk(model, [{ index: 0, delta: { content: text.slice(3) }, finish_reason: null }]),
    chunk(model, [{ index: 0, delta: {}, finish_reason: "stop" }]),
    ...(usage ? [chunk(model, [], { usage: { prompt_tokens: 9, completion_tokens: 2, total_tokens: 11 } })] : []),
];
const err = (status, type, message, headers, code) => ({ status, contentType: "application/json", headers: { "x-request-id": "req_" + status, ...(headers || {}) },
    body: JSON.stringify({ error: { message, type, param: null, code: code || null } }) });
const completion = (model, msg, finish = "stop") => ({ id: "chatcmpl-1", object: "chat.completion", created: 1700000000, model,
    choices: [{ index: 0, message: { role: "assistant", content: null, refusal: null, ...msg }, finish_reason: finish, logprobs: null }],
    usage: { prompt_tokens: 3, completion_tokens: 1, total_tokens: 4 } });

const log = [];
const M = "gpt-test";
const user = (t) => [{ role: "user", content: t }];
const client = (o = {}) => new OpenAI({ apiKey: "test-key", baseURL: BASE, ...o });
const last = () => log[log.length - 1];
__kitS.section("fixture: create the scripted response plan", () => {
plan = new Map();
app = new App({ port: 0, host: "127.0.0.1" });
app.post("/v1/chat/completions", (req) => {
    const b = JSON.parse(req.body);
    log.push({ path: req.path, headers: req.headers, body: b });
    const script = plan.get(b.model);
    if (script && script.length) {
        const step = script.shift();
        return typeof step === "function" ? step(req, b) : step;
    }
    if (!Array.isArray(b.messages) || !b.messages.length) return err(400, "invalid_request_error", "messages: too short", {}, "messages_empty");
    if (b.stream) return { status: 200, contentType: "text/event-stream", headers: { "x-request-id": "req_stream" }, body: sse(textChunks(b.model, "Hello world")) };
    return { status: 200, contentType: "application/json", headers: { "x-request-id": "req_ok" }, body: JSON.stringify(completion(b.model, { content: "hi" })) };
});
app.get("/v1/models", (req) => {
    log.push({ path: req.path, query: req.query, headers: req.headers });
    return { status: 200, contentType: "application/json", body: JSON.stringify({ object: "list", data: [{ id: "gpt-x", object: "model", owned_by: "o" }] }) };
});
app.get("/v1/models/:id", (req) => {
    log.push({ path: req.path, params: req.params, headers: req.headers });
    if (req.params.id === "nope") return err(404, "invalid_request_error", "The model 'nope' does not exist", {}, "model_not_found");
    return { status: 200, contentType: "application/json", body: JSON.stringify({ id: req.params.id, object: "model" }) };
});
app.start();
BASE = "http://127.0.0.1:" + app.port + "/v1";
});

try {
    {
let c, r, h;
        const tools = [{ type: "function", function: { name: "f", parameters: { type: "object", properties: {} } } }];
await __kitS.section("create resolves the ChatCompletion", async () => {
        c = client({ organization: "org-1", project: "proj-1", headers: { "x-client": "c1" } });
        log.length = 0;
        r = await c.chat.completions.create({ model: M, messages: user("x"), tools, tool_choice: "auto", response_format: { type: "json_object" }, temperature: 0.2 }, { headers: { "x-trace": "t1" } });
        h = last().headers;
        ok(r.object === "chat.completion" && r.choices[0].message.content === "hi" && r.usage.total_tokens === 4, "create resolves the ChatCompletion");
        ok(last().path === "/v1/chat/completions", "create posts to {baseURL}/chat/completions", last().path);
        ok(h["authorization"] === "Bearer test-key" && !("x-api-key" in h) && !("anthropic-version" in h), "apiKey is sent as Authorization: Bearer, no Anthropic headers");
        ok(h["openai-organization"] === "org-1" && h["openai-project"] === "proj-1", "organization and project headers");
        ok(h["content-type"] === "application/json" && h["accept"] === "application/json", "JSON content-type and accept");
        ok(h["x-client"] === "c1" && h["x-trace"] === "t1", "client headers then request headers");
        ok(JSON.stringify(last().body.tools) === JSON.stringify(tools) && last().body.tool_choice === "auto" && last().body.response_format.type === "json_object" && last().body.temperature === 0.2,
            "tools, tool_choice, response_format and temperature pass through as given");
        ok(!("stream" in last().body) && !("stream_options" in last().body), "create sends no stream keys");
        ok(c.baseURL === BASE && c.maxRetries === 2 && c.timeout === 600000 && !("apiVersion" in c), "client getters report the defaults");
});
await __kitS.section("no organization/project header unless configured", async () => {
        log.length = 0;
        await client().chat.completions.create({ model: M, messages: user("x") });
        ok(!("openai-organization" in last().headers) && !("openai-project" in last().headers), "no organization/project header unless configured");
        ok(new OpenAI({ apiKey: "k", baseURL: BASE + "//" }).baseURL === BASE, "trailing slashes on baseURL are dropped");
});
    }
    {
let r, r2;
await __kitS.section("a 400 rejects with APIError: type, x-request-id, body.error.code and the typed message", async () => {
        plan.set("m-400", [err(400, "invalid_request_error", "Unsupported value: 'temperature'", {}, "unsupported_value")]);
        log.length = 0;
        await rejects(client().chat.completions.create({ model: "m-400", messages: user("x") }),
            (e) => e instanceof APIError && e.status === 400 && e.type === "invalid_request_error" && e.requestId === "req_400" && e.retryable === false
                && e.body.error.code === "unsupported_value" && /^400 invalid_request_error: Unsupported value/.test(e.message),
            "a 400 rejects with APIError: type, x-request-id, body.error.code and the typed message");
});
__kitS.section("a 400 is not retried", () => {
        ok(log.length === 1, "a 400 is not retried", log.length);
});
await __kitS.section("401 invalid_api_key", async () => {
        plan.set("m-401", [err(401, "invalid_request_error", "Incorrect API key provided", {}, "invalid_api_key")]);
        await rejects(client().chat.completions.create({ model: "m-401", messages: user("x") }), (e) => e.status === 401 && e.body.error.code === "invalid_api_key", "401 invalid_api_key");
});
await __kitS.section("429 with retry-after-ms is retried once, then succeeds", async () => {
        plan.set("m-429", [err(429, "requests", "Rate limit reached", { "retry-after-ms": "1" }, "rate_limit_exceeded")]);
        log.length = 0;
        r = await client().chat.completions.create({ model: "m-429", messages: user("x") });
        ok(r.choices[0].message.content === "hi" && log.length === 2, "429 with retry-after-ms is retried once, then succeeds", log.length);
});
await __kitS.section("maxRetries 0: the 429 rejects with retryAfterMs", async () => {
        plan.set("m-429b", [err(429, "requests", "slow", { "retry-after": "7" })]);
        await rejects(client({ maxRetries: 0 }).chat.completions.create({ model: "m-429b", messages: user("x") }),
            (e) => e instanceof APIError && e.status === 429 && e.retryAfterMs === 7000 && e.retryable === true, "maxRetries 0: the 429 rejects with retryAfterMs");
});
await __kitS.section("retries exhausted: the last answer rejects", async () => {
        plan.set("m-500", [err(500, "server_error", "a", { "retry-after": "0" }), err(503, "server_error", "b", { "retry-after": "0" }), err(500, "server_error", "c", { "retry-after": "0" })]);
        log.length = 0;
        await rejects(client().chat.completions.create({ model: "m-500", messages: user("x") }),
            (e) => e instanceof APIError && e.status === 500 && e.type === "server_error" && /c/.test(e.message), "retries exhausted: the last answer rejects");
        ok(log.length === 3, "default maxRetries 2 makes three attempts", log.length);
});
await __kitS.section("a per-request maxRetries overrides the client's", async () => {
        plan.set("m-500b", [err(500, "server_error", "once", { "retry-after": "0" })]);
        log.length = 0;
        r2 = await client({ maxRetries: 0 }).chat.completions.create({ model: "m-500b", messages: user("x") }, { maxRetries: 1 });
        ok(r2.choices[0].message.content === "hi" && log.length === 2, "a per-request maxRetries overrides the client's", log.length);
});
await __kitS.section("a non-JSON error body still gives a typed APIError", async () => {
        plan.set("m-502", [{ status: 502, contentType: "text/html", body: "<html>bad gateway</html>", headers: { "retry-after": "0" } }]);
        await rejects(client({ maxRetries: 0 }).chat.completions.create({ model: "m-502", messages: user("x") }),
            (e) => e instanceof APIError && e.status === 502 && /bad gateway/.test(e.message) && e.body === null, "a non-JSON error body still gives a typed APIError");
        await rejects(new OpenAI({ apiKey: "k", baseURL: "http://127.0.0.1:9/v1", maxRetries: 0 }).chat.completions.create({ model: M, messages: user("x") }),
            (e) => e instanceof APIError && e.status === 0 && e.type === "connection_error" && e.retryable === true, "a refused connection rejects with connection_error");
});
    }
    {
let c;
        let t = null;
await __kitS.section("model is required: there is no default", async () => {
        await rejects(client().chat.completions.create({ messages: user("x") }), (e) => e instanceof TypeError && /no default model/.test(e.message), "model is required: there is no default");
        await rejects(client().chat.completions.create({ model: "", messages: user("x") }), (e) => e instanceof TypeError && /model/.test(e.message), "an empty model is refused");
        await rejects(client().chat.completions.create({ model: M }), (e) => e instanceof TypeError && /messages/.test(e.message), "messages is required");
        await rejects(client().chat.completions.create({ model: M, messages: [], stream: true }), (e) => e instanceof TypeError && /chat.completions.stream/.test(e.message), "create refuses stream: true");
        await rejects(client().chat.completions.create({ model: M, messages: [] }, { betas: ["x"] }), (e) => e instanceof TypeError && /unknown option "betas" \(valid: headers, timeout, maxRetries, signal\)/.test(e.message), "betas is not an OpenAI request option");
});
__kitS.section("an unknown client option names the valid ones", () => {
        try { new OpenAI({ apiKey: "k", apiVersion: "1" }); } catch (e) { t = e; }
        ok(t instanceof TypeError && /unknown option "apiVersion" \(valid: apiKey, baseURL, organization, project, maxRetries, timeout, headers, ca\)/.test(t.message), "an unknown client option names the valid ones", t && t.message);
});
__kitS.section("a baseURL with a query is a TypeError", () => {
        t = null;
        try { new OpenAI({ apiKey: "k", baseURL: "http://x/v1?a=1" }); } catch (e) { t = e; }
        ok(t instanceof TypeError && /baseURL/.test(t.message), "a baseURL with a query is a TypeError");
});
__kitS.section("organization must be a string", () => {
        t = null;
        try { new OpenAI({ apiKey: "k", organization: 5 }); } catch (e) { t = e; }
        ok(t instanceof TypeError && /organization must be a string/.test(t.message), "organization must be a string");
});
await __kitS.section("no key rejects with TypeError", async () => {
        if (getEnv("OPENAI_API_KEY")) { skip++; print("  SKIP  no-credential row: the environment has OPENAI_API_KEY"); }
        else await rejects(new OpenAI({ baseURL: BASE }).chat.completions.create({ model: M, messages: [] }),
            (e) => e instanceof TypeError && /OpenAI: no credentials/.test(e.message), "no key rejects with TypeError");
});
await __kitS.section("a key with a newline is refused, never sent", async () => {
        await rejects(client({ apiKey: "bad\nkey" }).chat.completions.create({ model: M, messages: user("x") }), (e) => e instanceof TypeError && /credential/.test(e.message), "a key with a newline is refused, never sent");
});
await __kitS.section("a closed client refuses calls", async () => {
        c = client();
        c.close();
        await rejects(c.chat.completions.create({ model: M, messages: [] }), (e) => e instanceof TypeError && /OpenAI: the client is closed/.test(e.message), "a closed client refuses calls");
});
__kitS.section("ChatCompletionStream is not constructible", () => {
        t = null;
        try { new ChatCompletionStream(); } catch (e) { t = e; }
        ok(t instanceof TypeError && /chat.completions.stream/.test(t.message), "ChatCompletionStream is not constructible");
});
    }
    {
let s, f, fc, s2, p1;
        const got = [];
__kitS.section("stream() returns a ChatCompletionStream before any I/O", () => {
        log.length = 0;
        s = client().chat.completions.stream({ model: M, messages: user("x"), stream: false });
        ok(s instanceof ChatCompletionStream && s.status === null && s.completion === null, "stream() returns a ChatCompletionStream before any I/O");
});
await __kitS.section("for await yields every chunk (not [DONE])", async () => {
        for await (const ch of s) got.push(ch);
        ok(got.length === 5 && got.every((c) => c.object === "chat.completion.chunk"), "for await yields every chunk (not [DONE])", got.length);
        ok(last().body.stream === true && last().headers["accept"] === "text/event-stream", "stream sends stream: true (overriding the body's) and accepts text/event-stream");
        ok(JSON.stringify(last().body.stream_options) === '{"include_usage":true}', "stream_options.include_usage is added when the body has none", JSON.stringify(last().body.stream_options));
});
await __kitS.section("the accumulated completion carries id, model, created, system_fingerprint", async () => {
        f = s.completion;
        ok(f.object === "chat.completion" && f.id === "chatcmpl-s1" && f.model === M && f.created === 1700000000 && f.system_fingerprint === "fp_x", "the accumulated completion carries id, model, created, system_fingerprint");
        ok(f.choices.length === 1 && f.choices[0].message.role === "assistant" && f.choices[0].message.content === "Hello world" && f.choices[0].finish_reason === "stop", "content deltas concatenate; finish_reason lands", JSON.stringify(f.choices));
        ok(f.choices[0].message.refusal === null && !("tool_calls" in f.choices[0].message) && f.choices[0].logprobs === null, "refusal null, no tool_calls, logprobs null");
        ok(f.usage && f.usage.total_tokens === 11 && f.usage.prompt_tokens === 9, "usage from the final choices-less chunk", JSON.stringify(f.usage));
        ok(s.status === 200 && s.requestId === "req_stream", "status and x-request-id", s.requestId);
        ok(await client().chat.completions.stream({ model: M, messages: user("x") }).finalContent() === "Hello world", "finalContent()");
});
await __kitS.section("finalChatCompletion()", async () => {
        fc = await client().chat.completions.stream({ model: M, messages: user("x") }).finalChatCompletion();
        ok(fc.choices[0].finish_reason === "stop" && fc.usage.completion_tokens === 2, "finalChatCompletion()");
});
await __kitS.section("a caller's stream_options is sent as given", async () => {
        log.length = 0;
        await client().chat.completions.stream({ model: M, messages: user("x"), stream_options: { include_usage: false } }).finalChatCompletion();
        ok(JSON.stringify(last().body.stream_options) === '{"include_usage":false}', "a caller's stream_options is sent as given");
});
await __kitS.section("stream_options: null is sent as null (no usage chunk requested)", async () => {
        await client().chat.completions.stream({ model: M, messages: user("x"), stream_options: null }).finalChatCompletion();
        ok(last().body.stream_options === null, "stream_options: null is sent as null (no usage chunk requested)");
});
await __kitS.section("a second next() while one is pending is refused", async () => {
        s2 = client().chat.completions.stream({ model: M, messages: user("x") });
        p1 = s2.next();
        await rejects(s2.next(), (e) => e instanceof TypeError && /ChatCompletionStream: one read at a time/.test(e.message), "a second next() while one is pending is refused");
        ok((await p1).value.object === "chat.completion.chunk", "the pending next() still resolves");
});
await __kitS.section("breaking out of for await closes the stream", async () => {
        for await (const c of s2) break;
        ok((await s2.next()).done === true, "breaking out of for await closes the stream");
});
    }
    {
let c, msgs, first, m, tc, second;
await __kitS.section("parallel tool calls accumulate by index", async () => {
        plan.set("m-tools", [{ status: 200, contentType: "text/event-stream", body: sse([
            chunk("m-tools", [{ index: 0, delta: { role: "assistant", content: null, tool_calls: [{ index: 0, id: "call_a", type: "function", function: { name: "get_weather", arguments: "" } }] }, finish_reason: null }]),
            chunk("m-tools", [{ index: 0, delta: { tool_calls: [{ index: 0, function: { arguments: "{\"city\": \"Pa" } }] }, finish_reason: null }]),
            chunk("m-tools", [{ index: 0, delta: { tool_calls: [{ index: 1, id: "call_b", type: "function", function: { name: "get_time", arguments: "{}" } }] }, finish_reason: null }]),
            chunk("m-tools", [{ index: 0, delta: { tool_calls: [{ index: 0, function: { arguments: "ris\"}" } }] }, finish_reason: null }]),
            chunk("m-tools", [{ index: 0, delta: {}, finish_reason: "tool_calls" }]),
            chunk("m-tools", [], { usage: { prompt_tokens: 20, completion_tokens: 10, total_tokens: 30 } })]) },
            (req, b) => {
                const a = b.messages[1], t1 = b.messages[2], t2 = b.messages[3];
                const good = a.role === "assistant" && a.tool_calls.length === 2 && a.tool_calls[0].function.arguments === "{\"city\": \"Paris\"}"
                    && t1.role === "tool" && t1.tool_call_id === "call_a" && t2.tool_call_id === "call_b";
                return { status: 200, contentType: "application/json", body: JSON.stringify(completion("m-tools", { content: good ? "round trip ok" : "bad shape" })) };
            }]);
        c = client();
        msgs = user("weather and time?");
        first = await c.chat.completions.stream({ model: "m-tools", messages: msgs, tools: [] }).finalChatCompletion();
        m = first.choices[0].message, tc = m.tool_calls || [];
        ok(first.choices[0].finish_reason === "tool_calls" && tc.length === 2, "parallel tool calls accumulate by index", JSON.stringify(m));
});
__kitS.section("id/name from the first piece, arguments concatenated across interleaved chunks", () => {
        ok(tc[0].id === "call_a" && tc[0].type === "function" && tc[0].function.name === "get_weather" && tc[0].function.arguments === "{\"city\": \"Paris\"}",
            "id/name from the first piece, arguments concatenated across interleaved chunks", JSON.stringify(tc[0]));
        ok(tc[1].id === "call_b" && tc[1].function.name === "get_time" && JSON.parse(tc[1].function.arguments) && m.content === null, "second call intact; content stays null", JSON.stringify(tc[1]));
});
await __kitS.section("tool round trip: the accumulated assistant message replays as is", async () => {
        msgs.push(m);
        msgs.push({ role: "tool", tool_call_id: tc[0].id, content: "18C" });
        msgs.push({ role: "tool", tool_call_id: tc[1].id, content: "12:00" });
        second = await c.chat.completions.create({ model: "m-tools", messages: msgs });
        ok(second.choices[0].message.content === "round trip ok", "tool round trip: the accumulated assistant message replays as is", second.choices[0].message.content);
});
    }
    {
let s;
await __kitS.section("a mid-stream error object rejects with APIError", async () => {
        plan.set("m-mid", [{ status: 200, contentType: "text/event-stream", body: sse([textChunks("m-mid", "partial")[1], { error: { message: "The server had an error", type: "server_error", param: null, code: null } }], false) }]);
        s = client().chat.completions.stream({ model: "m-mid", messages: user("x") });
        await rejects(s.finalChatCompletion(), (e) => e instanceof APIError && e.type === "server_error" && /The server had an error/.test(e.message), "a mid-stream error object rejects with APIError");
        ok(s.completion && s.completion.choices[0].message.content === "par", "the partial completion stays readable after the error", JSON.stringify(s.completion && s.completion.choices));
        await rejects(s.finalChatCompletion(), (e) => e instanceof APIError, "a failed stream keeps failing");
});
await __kitS.section("a body that ends before [DONE] rejects, even after finish_reason", async () => {
        plan.set("m-trunc", [{ status: 200, contentType: "text/event-stream", body: sse(textChunks("m-trunc", "Hello world"), false) }]);
        await rejects(client().chat.completions.stream({ model: "m-trunc", messages: user("x") }).finalChatCompletion(),
            (e) => e instanceof APIError && e.type === "connection_error" && /before \[DONE\]/.test(e.message), "a body that ends before [DONE] rejects, even after finish_reason");
});
await __kitS.section("a data line that is not JSON rejects with stream_error", async () => {
        plan.set("m-bad", [{ status: 200, contentType: "text/event-stream", body: sse(["{oops"]) }]);
        await rejects(client().chat.completions.stream({ model: "m-bad", messages: user("x") }).finalChatCompletion(),
            (e) => e instanceof APIError && e.type === "stream_error", "a data line that is not JSON rejects with stream_error");
});
await __kitS.section("a choice without an index rejects with stream_error", async () => {
        plan.set("m-noidx", [{ status: 200, contentType: "text/event-stream", body: sse([chunk("m", [{ delta: { content: "x" } }])]) }]);
        await rejects(client().chat.completions.stream({ model: "m-noidx", messages: user("x") }).finalChatCompletion(),
            (e) => e instanceof APIError && e.type === "stream_error" && /index/.test(e.message), "a choice without an index rejects with stream_error");
});
await __kitS.section("a stream is retried while no response head has arrived (429 then 200)", async () => {
        plan.set("m-s429", [err(429, "requests", "later", { "retry-after": "0" })]);
        log.length = 0;
        ok(await client().chat.completions.stream({ model: "m-s429", messages: user("x") }).finalContent() === "Hello world" && log.length === 2,
            "a stream is retried while no response head has arrived (429 then 200)", log.length);
});
await __kitS.section("a stream's HTTP error rejects the first next()", async () => {
        plan.set("m-s401", [err(401, "invalid_request_error", "bad key", {}, "invalid_api_key")]);
        await rejects(client().chat.completions.stream({ model: "m-s401", messages: user("x") }).next(),
            (e) => e instanceof APIError && e.status === 401 && e.requestId === "req_401", "a stream's HTTP error rejects the first next()");
});
    }
    {
let page, one;
await __kitS.section("models.list GETs {baseURL}/models", async () => {
        log.length = 0;
        page = await client().models.list();
        ok(page.data[0].id === "gpt-x" && last().path === "/v1/models", "models.list GETs {baseURL}/models");
});
await __kitS.section("models.retrieve percent-encodes the id as one segment", async () => {
        one = await client().models.retrieve("ft:gpt-x:org/1");
        ok(one.id === "ft:gpt-x:org/1" && /\/v1\/models\/ft%3Agpt-x%3Aorg%2F1$/.test(last().path), "models.retrieve percent-encodes the id as one segment", last().path);
        await rejects(client().models.retrieve("nope"), (e) => e instanceof APIError && e.status === 404 && e.body.error.code === "model_not_found", "models.retrieve 404 is a typed APIError");
        await rejects(client().models.retrieve(""), (e) => e instanceof TypeError, "models.retrieve refuses an empty id");
});
    }
    {
let cc, r, p;
await __kitS.section("client.chat.completions outlives a collected client object", async () => {
        cc = client().chat.completions;
        std.gc();
        r = await cc.create({ model: M, messages: user("x") });
        ok(r.choices[0].message.content === "hi", "client.chat.completions outlives a collected client object");
});
await __kitS.section("a stream collected mid-read still completes (the read pins it)", async () => {
        p = client().chat.completions.stream({ model: M, messages: user("x") }).finalContent();
        std.gc();
        ok(await p === "Hello world", "a stream collected mid-read still completes (the read pins it)");
});
    }
} finally { if (!__kitS.listing) {
    app.close();
} }
__kitS.end();
if (!__kitS.listing) {
print(`test_ai_open_ai: ${pass} passed, ${fail} failed, ${skip} skipped`);
if (fail) throw new Error("test_ai_open_ai failed");
}
