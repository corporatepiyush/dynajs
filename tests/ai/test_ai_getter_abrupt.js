// flags: --std
// timeout: 120
// covers: contract dyna:ai.Anthropic dyna:ai.OpenAI dyna:ai.MessageAccumulator dyna:ai.ChatCompletionAccumulator dyna:ai.APIError -- D49: every option, body, event and response read propagates a throwing getter or Proxy trap
// st1o D49: these natives read options, bodies and parsed events with JS_GetProperty* and type-tested the result before
// any JS_IsException check, so a throwing getter was read as "absent" or "wrong type": the call went on with the error
// still pending, or a module TypeError/APIError replaced the caller's error. Parsed JSON (SSE events, response and
// error bodies) inherits from Object.prototype, so an inherited getter for an absent member is reachable too.
// Oracle: each hostile call throws (or rejects with) exactly the sentinel and leaves no exception pending
// (TOML.stringify of a Symbol must throw its own TypeError). Also: a GC inside every getter keeps the result intact, and a --timeout-ms child's interrupt ends a loop that
// catches everything. The inherited accessors carry a setter that defines an own property, so the accumulators' own
// writes to those names are not refused; delta keys are enumerated as own properties, so no inherited row exists for them.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_ai_getter_abrupt");
import { Anthropic, OpenAI, MessageAccumulator, ChatCompletionAccumulator, APIError } from "dyna:ai";
import { App } from "dyna:http";
import { TOML } from "dyna:config";
import { Exec, args } from "dyna:sys";
import * as std from "std";

const SELF = "tests/ai/test_ai_getter_abrupt.js";
const SENT = new Error("ai getter sentinel");
const slot = () => { try { TOML.stringify({ a: Symbol() }); return "no throw"; } catch (e) { return e === SENT ? "sentinel leaked" : e instanceof TypeError ? "clean" : "threw " + e; } };
const run = (f) => { try { return { v: f() }; } catch (e) { return { e }; } };
const arun = async (f) => { try { return { v: await f() }; } catch (e) { return { e }; } };
const thrower = () => { throw SENT; };
const with_ = (base, k, get) => { const x = { ...base }; delete x[k]; Object.defineProperty(x, k, { get, enumerable: true, configurable: true }); return x; };
const arrProxy = (a, bad) => new Proxy(a, { get(t, k) { if (k === bad) throw SENT; return Reflect.get(t, k); } });
const own = (k) => function (v) { Object.defineProperty(this, k, { value: v, writable: true, enumerable: true, configurable: true }); };
const inherited = (k, f, get = thrower) => { Object.defineProperty(Object.prototype, k, { get, set: own(k), configurable: true }); try { return f(); } finally { delete Object.prototype[k]; } };
const ainherited = async (k, f) => { Object.defineProperty(Object.prototype, k, { get: thrower, set: own(k), configurable: true }); try { return await f(); } finally { delete Object.prototype[k]; } };

const sse = (evs) => evs.map(([e, d]) => "event: " + e + "\ndata: " + JSON.stringify(d) + "\n\n").join("");
const M = "claude-opus-5-5";
const start = ["message_start", { type: "message_start", message: { id: "msg_01", type: "message", role: "assistant", model: M, content: [], stop_reason: null, stop_sequence: null, usage: { input_tokens: 3, output_tokens: 1 } } }];
const bstart = (b) => ["content_block_start", { type: "content_block_start", index: 0, content_block: b }];
const bdelta = (d) => ["content_block_delta", { type: "content_block_delta", index: 0, delta: d }];
const bstop = ["content_block_stop", { type: "content_block_stop", index: 0 }];
const mdelta = ["message_delta", { type: "message_delta", delta: { stop_reason: "end_turn", stop_sequence: null }, usage: { output_tokens: 2 } }];
const mstop = ["message_stop", { type: "message_stop" }];
const TEXT = { type: "text", text: "" };
const GOLD = sse([start, bstart(TEXT), bdelta({ type: "text_delta", text: "hi" }), bstop, mdelta, mstop]);
const feedMA = (s) => { const a = new MessageAccumulator(); a.feed(s); return a; };

const ck = (choices, extra) => ({ id: "c1", object: "chat.completion.chunk", created: 1, model: "m", choices, ...(extra || {}) });
const cdata = (chunks) => chunks.map((c) => "data: " + JSON.stringify(c) + "\n\n").join("") + "data: [DONE]\n\n";
const CGOLD = cdata([ck([{ index: 0, delta: { role: "assistant", content: "hi" }, finish_reason: null }]), ck([{ index: 0, delta: {}, finish_reason: "stop" }], { usage: { prompt_tokens: 1, completion_tokens: 1, total_tokens: 2 } })]);
const feedCA = (s) => { const a = new ChatCompletionAccumulator(); a.feed(s); return a; };

const PINNED = [
    ["message accumulator", () => JSON.stringify(feedMA(GOLD).message.content), '[{"type":"text","text":"hi"}]'],
    ["chat accumulator", () => feedCA(CGOLD).completion.choices[0].message.content, "hi"],
    ["APIError init", () => { const e = new APIError("m", { status: 429, type: "rate_limit_error", requestId: "r1", retryAfterMs: 5 }); return e.message === "m" && e.status === 429 && e.type === "rate_limit_error" && e.requestId === "r1" && e.retryAfterMs === 5 && e.retryable === true && e.headers === null && e.body === null && e instanceof APIError; }, true],
];

const MA_ROWS = [
    ["message", [["message_start", { type: "message_start" }]]],
    ["index", [start, ["content_block_start", { type: "content_block_start", content_block: TEXT }]]],
    ["content_block", [start, ["content_block_start", { type: "content_block_start", index: 0 }]]],
    ["delta", [start, bstart(TEXT), ["content_block_delta", { type: "content_block_delta", index: 0 }]]],
    ["text", [start, bstart(TEXT), bdelta({ type: "text_delta" })]],
    ["partial_json", [start, bstart({ type: "tool_use", id: "t", name: "n", input: {} }), bdelta({ type: "input_json_delta" })]],
    ["thinking", [start, bstart({ type: "thinking", thinking: "", signature: "" }), bdelta({ type: "thinking_delta" })]],
    ["signature", [start, bstart({ type: "thinking", thinking: "", signature: "" }), bdelta({ type: "signature_delta" })]],
    ["citation", [start, bstart(TEXT), bdelta({ type: "citations_delta" })]],
    ["usage", [start, ["message_delta", { type: "message_delta", delta: { stop_reason: "end_turn" } }]]],
    ["error", [["error", { type: "error" }]]],
    ["request_id", [["error", { type: "error", error: { type: "overloaded_error", message: "busy" } }]]],
];
const CA_ROWS = [
    ["choices", [{ id: "c1", object: "chat.completion.chunk", created: 1, model: "m" }]],
    ["usage", [ck([{ index: 0, delta: { content: "x" }, finish_reason: null }])]],
    ["error", [ck([{ index: 0, delta: { content: "x" }, finish_reason: null }])]],
    ["delta", [ck([{ index: 0, finish_reason: null }])]],
    ["finish_reason", [ck([{ index: 0, delta: { content: "x" } }])]],
    ["logprobs", [ck([{ index: 0, delta: { content: "x" }, finish_reason: null }])]],
    ["function", [ck([{ index: 0, delta: { tool_calls: [{ index: 0, id: "t1", type: "function" }] }, finish_reason: null }])]],
    ["arguments", [ck([{ index: 0, delta: { tool_calls: [{ index: 0, id: "t1", type: "function", function: { name: "f" } }] }, finish_reason: null }])]],
    ["refusal", [ck([{ index: 0, delta: { content: "x" }, finish_reason: null, logprobs: { content: [] } }])]],
    ["request_id", [{ error: { message: "bad", type: "server_error" } }]],
];

if (args().includes("--child")) {
    const mode = args()[args().indexOf("--child") + 1];
    {
        let caught = 0;
        for (;;) {
            try { inherited("text", () => feedMA(sse([start, bstart(TEXT), bdelta({ type: "text_delta" })]))); } catch (e) { caught++; }
            try { inherited("error", () => feedCA(CGOLD)); } catch (e) { caught++; }
            try { new APIError("m", with_({}, "type", thrower)); } catch (e) { caught++; }
        }
    }
} else {
    let n = 0, fails = 0;
    const ok = (c, m) => { n++; if (!c) { fails++; print("FAIL: " + m); } };
    const show = (r) => r.e === SENT ? "sentinel" : r.e ? "threw " + (r.e && r.e.constructor ? r.e.constructor.name + ": " : "") + (r.e && r.e.message) : "returned " + String(r.v);
    const sent = (name, f) => { const r = run(f); const s = slot(); ok(r.e === SENT && s === "clean", name + ": " + show(r) + ", " + s); };
    const asent = async (name, f) => { const r = await arun(f); const s = slot(); ok(r.e === SENT && s === "clean", name + ": " + show(r) + ", " + s); };

    await __kitS.section("pinned results", () => {
        for (const [name, f, want] of PINNED) { const r = run(f); ok(!r.e && r.v === want, name + ": " + (r.e ? "threw " + r.e : JSON.stringify(r.v))); }
    });
    await __kitS.section("APIError: every init getter propagates", () => {
        const INIT = { status: 400, type: "x", requestId: "r", retryAfterMs: 5, headers: {}, body: {} };
        for (const k of Object.keys(INIT)) sent("APIError init " + k, () => new APIError("m", with_(INIT, k, thrower)));
        sent("APIError init Proxy get trap", () => new APIError("m", new Proxy({}, { get: thrower })));
    });
    await __kitS.section("MessageAccumulator: an inherited getter for an absent event member propagates", () => {
        for (const [k, evs] of MA_ROWS) {
            const s = sse(evs);
            const ctl = run(() => feedMA(s));
            ok(ctl.e === undefined || ctl.e instanceof APIError, "CONTROL " + k + ": " + show(ctl));
            sent("MessageAccumulator " + k, () => inherited(k, () => feedMA(s)));
        }
    });
    await __kitS.section("ChatCompletionAccumulator: an inherited getter for an absent chunk member propagates", () => {
        for (const [k, chunks] of CA_ROWS) {
            const s = cdata(chunks);
            const ctl = run(() => feedCA(s));
            ok(ctl.e === undefined || ctl.e instanceof APIError, "CONTROL " + k + ": " + show(ctl));
            sent("ChatCompletionAccumulator " + k, () => inherited(k, () => feedCA(s)));
        }
    });
    await __kitS.section("a GC inside every inherited getter keeps the accumulated result intact", () => {
        const gcUndef = () => { std.gc(); return undefined; };
        for (const k of ["message", "index", "delta", "text", "usage", "error", "request_id", "citation", "signature"])
            ok(inherited(k, () => JSON.stringify(feedMA(GOLD).message)) === JSON.stringify(feedMA(GOLD).message), "gc MessageAccumulator " + k);
        for (const k of ["usage", "error", "logprobs", "tool_calls", "refusal", "reasoning"])
            ok(inherited(k, () => JSON.stringify(feedCA(CGOLD).completion), gcUndef) === JSON.stringify(feedCA(CGOLD).completion), "gc ChatCompletionAccumulator " + k);
        const gc = (base) => { const x = {}; for (const k of Object.keys(base)) { const v = base[k]; Object.defineProperty(x, k, { get() { std.gc(); return v; }, enumerable: true }); } return x; };
        const e = new APIError("m", gc({ status: 429, type: "rate_limit_error", requestId: "r1", retryAfterMs: 5 }));
        ok([e.status, e.type, e.requestId, e.retryAfterMs].join(",") === "429,rate_limit_error,r1,5", "gc APIError init");
    });

    let app, BASE;
    const reply = (status, body, headers) => ({ status, contentType: "application/json", body: JSON.stringify(body), ...(headers ? { headers } : {}) });
    const MSG = { id: "msg_1", type: "message", role: "assistant", model: M, content: [{ type: "text", text: "hi" }], stop_reason: "end_turn", stop_sequence: null, usage: { input_tokens: 1, output_tokens: 1 } };
    const EMB = (data) => reply(200, { object: "list", model: "emb", usage: { prompt_tokens: 1, total_tokens: 1 }, ...(data ? { data } : {}) });
    await __kitS.section("fixture: an in-process API server", () => {
        app = new App({ port: 0, host: "127.0.0.1" });
        app.post("/v1/messages", () => reply(200, MSG));
        app.post("/v1/messages/count_tokens", () => reply(200, { input_tokens: 3 }));
        app.post("/v1/messages/batches", () => reply(200, { id: "b1", type: "message_batch", processing_status: "in_progress" }));
        app.post("/e/v1/messages", (req) => {
            const b = JSON.parse(req.body);
            return b.model === "noerr" ? reply(400, {}, { "request-id": "h1" }) : reply(400, { error: { type: "invalid_request_error", message: "m" } });
        });
        app.post("/o/embeddings", (req) => {
            const b = JSON.parse(req.body);
            return b.model === "nodata" ? EMB(null) : b.model === "noemb" ? EMB([{ object: "embedding", index: 0 }]) : EMB([{ object: "embedding", index: 0, embedding: [0.5, 1] }]);
        });
        app.post("/o/chat/completions", () => reply(200, { id: "c1", object: "chat.completion", created: 1, model: "m", choices: [{ index: 0, message: { role: "assistant", content: "hi" }, finish_reason: "stop" }] }));
        app.start();
        BASE = "http://127.0.0.1:" + app.port;
    });
    try {
        const an = (o = {}) => new Anthropic({ apiKey: "k", baseURL: BASE, maxRetries: 0, ...o });
        const oa = (o = {}) => new OpenAI({ apiKey: "k", baseURL: BASE + "/o", maxRetries: 0, ...o });
        const BODY = { model: M, max_tokens: 4, messages: [{ role: "user", content: "x" }] };
        const OPTS = { betas: ["b1"], headers: { "x-a": "1" }, timeout: 5000, maxRetries: 0, signal: new AbortController().signal };
        await __kitS.section("client constructors: option getters propagate", () => {
            for (const k of ["betas", "headers", "apiKey", "baseURL", "maxRetries", "timeout"])
                sent("Anthropic ctor " + k, () => new Anthropic(with_({ apiKey: "k", baseURL: BASE, betas: ["b"], headers: { "x-a": "1" }, maxRetries: 0, timeout: 1000 }, k, thrower)));
            sent("Anthropic ctor betas element", () => new Anthropic({ apiKey: "k", baseURL: BASE, betas: arrProxy(["b"], "0") }));
            sent("Anthropic ctor header value", () => new Anthropic({ apiKey: "k", baseURL: BASE, headers: with_({}, "x-a", thrower) }));
            sent("OpenAI ctor headers", () => new OpenAI(with_({ apiKey: "k", baseURL: BASE, headers: {} }, "headers", thrower)));
            sent("OpenAI ctor header value", () => new OpenAI({ apiKey: "k", baseURL: BASE, headers: with_({}, "x-a", thrower) }));
        });
        await __kitS.section("Anthropic requests: body and option getters reject with the sentinel", async () => {
            ok((await arun(() => an().messages.create(BODY, OPTS))).v?.content?.[0]?.text === "hi", "CONTROL messages.create");
            for (const k of ["model", "messages", "max_tokens", "stream"])
                await asent("messages.create body " + k, () => an().messages.create(with_(BODY, k, thrower)));
            for (const k of Object.keys(OPTS))
                await asent("messages.create opts " + k, () => an().messages.create(BODY, with_(OPTS, k, thrower)));
            await asent("messages.create opts betas element", () => an().messages.create(BODY, { betas: arrProxy(["b"], "0") }));
            await asent("messages.create opts header value", () => an().messages.create(BODY, { headers: with_({}, "x-a", thrower) }));
            await asent("messages.create signal addEventListener", () => an().messages.create(BODY, { signal: with_({ aborted: false }, "addEventListener", thrower) }));
            await asent("messages.create signal aborted", () => an().messages.create(BODY, { signal: with_({ addEventListener() {}, removeEventListener() {} }, "aborted", thrower) }));
            await asent("messages.stream body extra key", () => arun(() => an().messages.stream(with_(BODY, "system", thrower)).finalMessage()).then((r) => { if (r.e) throw r.e; return r.v; }));
            for (const k of ["model", "messages"])
                await asent("countTokens body " + k, () => an().messages.countTokens(with_({ model: M, messages: BODY.messages }, k, thrower)));
            for (const k of ["custom_id", "params"])
                await asent("batches.create request " + k, () => an().messages.batches.create({ requests: [with_({ custom_id: "a", params: BODY }, k, thrower)] }));
            {
                let reads = 0;
                const req = { get custom_id() { throw SENT; }, get params() { reads++; return BODY; } };
                await asent("batches.create custom_id throws", () => an().messages.batches.create({ requests: [req] }));
                ok(reads === 0, "batches.create: params is not read once custom_id threw (reads " + reads + ")");
            }
            await asent("batches.create requests element", () => an().messages.batches.create({ requests: arrProxy([{ custom_id: "a", params: BODY }], "0") }));
        });
        await __kitS.section("error responses: an inherited getter for an absent member rejects with the sentinel", async () => {
            const ctl = await arun(() => an({ baseURL: BASE + "/e" }).messages.create({ ...BODY, model: "noerr" }));
            ok(ctl.e instanceof APIError && ctl.e.status === 400 && ctl.e.requestId === "h1", "CONTROL error body without error: " + show(ctl));
            await asent("error body: error", () => ainherited("error", () => an({ baseURL: BASE + "/e" }).messages.create({ ...BODY, model: "noerr" })));
            await asent("error body: request_id", () => ainherited("request_id", () => an({ baseURL: BASE + "/e" }).messages.create(BODY)));
            await asent("error headers: request-id", () => ainherited("request-id", () => an({ baseURL: BASE + "/e" }).messages.create(BODY)));
        });
        await __kitS.section("OpenAI requests: body getters and inherited response getters reject with the sentinel", async () => {
            const ctl = await arun(() => oa().embeddings.create({ model: "emb", input: "x", encoding_format: "float" }));
            ok(ctl.v && ctl.v.data[0].embedding.length === 2, "CONTROL embeddings.create: " + show(ctl));
            ok((await arun(() => oa().chat.completions.create({ model: "m", messages: BODY.messages }))).v?.choices?.[0]?.message?.content === "hi", "CONTROL chat.completions.create");
            for (const k of ["model", "input", "encoding_format", "dimensions"])
                await asent("embeddings.create body " + k, () => oa().embeddings.create(with_({ model: "emb", input: "x", dimensions: 2 }, k, thrower)));
            await asent("embeddings.create input element", () => oa().embeddings.create({ model: "emb", input: arrProxy(["x"], "0") }));
            for (const k of ["model", "messages", "stream"])
                await asent("chat.completions.create body " + k, () => oa().chat.completions.create(with_({ model: "m", messages: BODY.messages }, k, thrower)));
            for (const k of ["timeout", "maxRetries", "headers", "signal"])
                await asent("chat.completions.create opts " + k, () => oa().chat.completions.create({ model: "m", messages: BODY.messages }, with_({ timeout: 5000 }, k, thrower)));
            await asent("embeddings response: data", () => ainherited("data", () => oa().embeddings.create({ model: "nodata", input: "x" })));
            await asent("embeddings response: embedding", () => ainherited("embedding", () => oa().embeddings.create({ model: "noemb", input: "x" })));
        });
        await __kitS.section("a GC inside every request getter keeps the result intact", async () => {
            const gc = (base) => { const x = {}; for (const k of Object.keys(base)) { const v = base[k]; Object.defineProperty(x, k, { get() { std.gc(); return v; }, enumerable: true }); } return x; };
            ok((await arun(() => an().messages.create(gc(BODY), gc(OPTS)))).v?.content?.[0]?.text === "hi", "gc messages.create");
            const e = await arun(() => oa().embeddings.create(gc({ model: "emb", input: "x", encoding_format: "float" })));
            ok(e.v && e.v.data[0].embedding[0] === 0.5, "gc embeddings.create: " + show(e));
        });
    } finally {
        app.close();
    }
    await __kitS.section("an interrupt that must end a catch-all loop", () => {
        const t0 = Date.now();
        const i = Exec(args()[0], ["--std", "--timeout-ms", "300", SELF, "--child", "interrupt"], { timeoutMs: 20000 });
        ok(i.code !== 0 && !i.timedOut && /interrupt/i.test(i.stderr + i.stdout), "interrupt child ends: rc " + i.code + " after " + (Date.now() - t0) + " ms " + i.stderr.slice(-200));
    });
    if (fails) throw new Error(fails + " of " + n + " checks failed");
    print("test_ai_getter_abrupt: " + n + " checks passed");
}
