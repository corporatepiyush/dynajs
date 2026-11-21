// flags: --std
// timeout: 600
// covers: dyna:ai.OpenAI -- live, opt-in: a local model server speaking the Chat Completions API
// Runs only when a server answers on 127.0.0.1:11555 (DYNAJS_AI_LIVE_URL overrides, as for
// test_ai_anthropic_live.js, but only a loopback host is accepted: nothing is ever sent off the
// machine); the client uses <that origin>/v1. Otherwise it prints SKIP. Opt in with a small model:
//   DYNAJS_AI_LIVE_URL=http://127.0.0.1:11434 (a running ollama), DYNAJS_AI_LIVE_MODEL=qwen3:0.6b
// A missing model also SKIPs. Against a real decoder this pins what a mock cannot: chunks arrive
// while the model generates, finish_reason stop/length/tool_calls, streamed usage, a tool call
// whose arguments parse as an object, a tool-result round trip, reasoning deltas accumulating, a
// 404 for an unknown model as APIError, return() mid-generation leaving the server usable, and
// embeddings decoded to Float32Array (or a typed APIError from a server without embeddings), and
// a signal abort mid-generation rejecting with type "aborted".
// Model output is not asserted verbatim.
import { OpenAI, APIError, ChatCompletionStream } from "dyna:ai";
import { getEnv } from "dyna:sys";
import * as std from "std";

const ORIGIN = (getEnv("DYNAJS_AI_LIVE_URL") || "http://127.0.0.1:11555").replace(/\/+$/, "");
const MODEL = getEnv("DYNAJS_AI_LIVE_MODEL") || "qwen3:0.6b";
if (!/^http:\/\/(127\.0\.0\.1|localhost|\[::1\])(:\d+)?$/.test(ORIGIN)) {
    print("test_ai_open_ai_live: FAIL -- DYNAJS_AI_LIVE_URL must be a loopback http URL, got " + ORIGIN);
    std.exit(1);
}

let pass = 0, fail = 0;
const ok = (c, w, d) => { if (c) { pass++; print("  ok    " + w); } else { fail++; print("  FAIL  " + w + (d !== undefined ? "  [" + d + "]" : "")); } };
const FINISH = ["stop", "length", "tool_calls"];
const c = new OpenAI({ apiKey: "local-test-key", baseURL: ORIGIN + "/v1", maxRetries: 0, timeout: 120000 });
const user = (t) => [{ role: "user", content: t }];
const NO_THINK = { reasoning_effort: "none" };
const WEATHER = { type: "function", function: { name: "get_weather", description: "Get the current weather for a city", parameters: { type: "object", properties: { city: { type: "string" } }, required: ["city"] } } };

try {
    await c.chat.completions.create({ model: MODEL, max_tokens: 1, ...NO_THINK, messages: user("hi") });
} catch (e) {
    const why = e instanceof APIError && e.status === 404 ? "model " + MODEL + " is not available" : "no server on " + ORIGIN;
    print("test_ai_open_ai_live: SKIP -- " + why + " (" + String(e && e.message).slice(0, 80) + ")");
    c.close();
    std.exit(0);
}

try {
    {
        const r = await c.chat.completions.create({ model: MODEL, max_tokens: 64, ...NO_THINK, messages: user("Reply with one short sentence about the sea.") });
        const ch = r.choices && r.choices[0];
        ok(r.object === "chat.completion" && ch && ch.message.role === "assistant", "create resolves a ChatCompletion", JSON.stringify(r).slice(0, 160));
        ok(FINISH.includes(ch.finish_reason), "finish_reason is a known value", ch.finish_reason);
        ok(r.usage && r.usage.prompt_tokens > 0 && r.usage.completion_tokens > 0 && r.usage.total_tokens === r.usage.prompt_tokens + r.usage.completion_tokens, "usage counts tokens", JSON.stringify(r.usage));
        ok(typeof ch.message.content === "string" && ch.message.content.length > 0, "the reply has text");
    }
    {
        const r = await c.chat.completions.create({ model: MODEL, max_tokens: 4, ...NO_THINK, messages: user("Write a long story about a lighthouse keeper.") });
        ok(r.choices[0].finish_reason === "length", "max_tokens 4 stops with length", r.choices[0].finish_reason);
    }
    {
        const s = c.chat.completions.stream({ model: MODEL, max_tokens: 120, ...NO_THINK, messages: user("List ten fruits, one per line.") });
        ok(s instanceof ChatCompletionStream, "stream() returns a ChatCompletionStream");
        const ts = [];
        for await (const ch of s) if (ch.choices.length && ch.choices[0].delta.content) ts.push(Date.now());
        ok(ts.length >= 5, "many content chunks arrive", ts.length);
        ok(ts[ts.length - 1] - ts[0] >= 20, "chunks are delivered while the model generates (" + (ts[ts.length - 1] - ts[0]) + " ms between first and last)");
        const f = await s.finalChatCompletion();
        ok(FINISH.includes(f.choices[0].finish_reason) && f.choices[0].message.content.length > 10, "finalChatCompletion after iterating has the text", JSON.stringify(f.choices[0]).slice(0, 120));
        ok(f.usage && f.usage.completion_tokens > 0, "include_usage gives streamed usage", JSON.stringify(f.usage));
        ok(s.status === 200, "status 200");
    }
    {
        const f = await c.chat.completions.stream({ model: MODEL, max_tokens: 300, messages: user("What is 2+2? Answer briefly.") }).finalChatCompletion();
        const m = f.choices[0].message, r = m.reasoning ?? m.reasoning_content;
        ok(r === undefined || (typeof r === "string" && r.length > 0), "streamed reasoning deltas accumulate", JSON.stringify(m).slice(0, 120));
    }
    {
        const req = { model: MODEL, max_tokens: 300, ...NO_THINK, tools: [WEATHER], messages: user("What is the weather in Paris? Use the get_weather tool.") };
        let r = null, tc = null;
        for (let i = 0; i < 3 && !tc; i++) {
            r = await c.chat.completions.stream(req).finalChatCompletion();
            tc = (r.choices[0].message.tool_calls || [])[0] || null;
        }
        ok(r.choices[0].finish_reason === "tool_calls" && tc && tc.function.name === "get_weather", "a streamed tool call finishes with tool_calls", JSON.stringify(r.choices[0]).slice(0, 200));
        let args = null;
        try { args = JSON.parse(tc.function.arguments); } catch (e) { }
        ok(tc && typeof tc.id === "string" && tc.id.length > 0 && args && typeof args === "object" && !Array.isArray(args), "tool call carries an id and object arguments", JSON.stringify(tc));
        if (tc) {
            const msgs = [...req.messages, r.choices[0].message, { role: "tool", tool_call_id: tc.id, content: "18 degrees C and sunny" }];
            const r2 = await c.chat.completions.create({ ...req, messages: msgs });
            ok(typeof r2.choices[0].message.content === "string" && r2.choices[0].message.content.length > 0, "tool result round trip yields an answer", JSON.stringify(r2.choices[0].message).slice(0, 160));
        }
    }
    {
        let e0 = null;
        try { await c.chat.completions.create({ model: "no-such-model-dynajs", messages: user("x") }); } catch (e) { e0 = e; }
        ok(e0 instanceof APIError && e0.status === 404, "an unknown model is a 404 APIError", e0 && e0.message);
    }
    {
        const s = c.chat.completions.stream({ model: MODEL, max_tokens: 400, ...NO_THINK, messages: user("Count from 1 to 200.") });
        let n = 0;
        for await (const ch of s) if (++n === 3) break;
        ok((await s.next()).done === true, "return() mid-generation closes the stream");
        const r = await c.chat.completions.create({ model: MODEL, max_tokens: 8, ...NO_THINK, messages: user("Say ok.") });
        ok(r.choices[0].message.role === "assistant", "the server stays usable after an abandoned stream");
        const ac = new AbortController();
        const sa = c.chat.completions.stream({ model: MODEL, max_tokens: 400, ...NO_THINK, messages: user("Count from 1 to 300.") }, { signal: ac.signal });
        await sa.next();
        ac.abort();
        let ae = null;
        try { for await (const ch of sa) { } } catch (e) { ae = e; }
        ok(ae instanceof APIError && ae.type === "aborted", "an abort mid-generation rejects the stream with aborted", ae && ae.message);
        const list = await c.models.list();
        ok(Array.isArray(list.data) && list.data.some((m) => m.id === MODEL), "models.list includes the model", JSON.stringify(list).slice(0, 120));
    }
    {
        let r = null, e0 = null;
        try { r = await c.embeddings.create({ model: MODEL, input: ["the sea", "a lighthouse"] }); } catch (e) { e0 = e; }
        ok(e0 ? e0 instanceof APIError && e0.status >= 400 : r.data.length === 2 && r.data.every((d) => d.embedding instanceof Float32Array && d.embedding.length > 0 && d.embedding.every(Number.isFinite)),
            "embeddings: Float32Array vectors, or a typed APIError when the server has no embeddings", e0 ? e0.message : r.data[0].embedding.length);
    }
} finally {
    c.close();
}
print(`test_ai_open_ai_live: ${pass} passed, ${fail} failed`);
if (fail) throw new Error("test_ai_open_ai_live failed");
