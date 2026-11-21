// flags: --std
// timeout: 600
// covers: dyna:ai.Anthropic -- live, opt-in: a local model server speaking the Messages API
// Runs only when a server answers on 127.0.0.1:11555 (DYNAJS_AI_LIVE_URL overrides, but only a
// loopback host is accepted: nothing is ever sent off the machine); otherwise it prints SKIP.
// Opt in with a small local model, for example:
//   OLLAMA_HOST=127.0.0.1:11555 ollama serve & ; OLLAMA_HOST=127.0.0.1:11555 ollama pull qwen3:0.6b
//   (or llama-server --host 127.0.0.1 --port 11555 -m model.gguf; DYNAJS_AI_LIVE_MODEL names the model)
// Both servers expose POST /v1/messages. A missing model also SKIPs. Against a real decoder this
// pins what a mock cannot: events arrive while the model is still generating (not at EOF),
// stop reasons end_turn/max_tokens/tool_use, a forced tool call whose input parses as an object,
// a tool_result round trip, thinking deltas accumulating, a 404 for an unknown model as APIError,
// and return() mid-generation leaving the server usable. Model output is not asserted verbatim.
import { Anthropic, APIError, MessageStream } from "dyna:ai";
import { getEnv } from "dyna:sys";
import * as std from "std";

const URL_ = getEnv("DYNAJS_AI_LIVE_URL") || "http://127.0.0.1:11555";
const MODEL = getEnv("DYNAJS_AI_LIVE_MODEL") || "qwen3:0.6b";
if (!/^http:\/\/(127\.0\.0\.1|localhost|\[::1\])(:\d+)?\/?$/.test(URL_)) {
    print("test_ai_anthropic_live: FAIL -- DYNAJS_AI_LIVE_URL must be a loopback http URL, got " + URL_);
    std.exit(1);
}

let pass = 0, fail = 0;
const ok = (c, w, d) => { if (c) { pass++; print("  ok    " + w); } else { fail++; print("  FAIL  " + w + (d !== undefined ? "  [" + d + "]" : "")); } };
const STOPS = ["end_turn", "max_tokens", "stop_sequence", "tool_use"];
const c = new Anthropic({ apiKey: "local-test-key", baseURL: URL_, maxRetries: 0, timeout: 120000 });
const user = (t) => [{ role: "user", content: t }];
const noThink = { type: "disabled" };
const WEATHER = { name: "get_weather", description: "Get the current weather for a city", input_schema: { type: "object", properties: { city: { type: "string" } }, required: ["city"] } };

try {
    await c.messages.create({ model: MODEL, max_tokens: 1, thinking: noThink, messages: user("hi") });
} catch (e) {
    const why = e instanceof APIError && e.status === 404 ? "model " + MODEL + " is not available" : "no server on " + URL_;
    print("test_ai_anthropic_live: SKIP -- " + why + " (" + String(e && e.message).slice(0, 80) + ")");
    c.close();
    std.exit(0);
}

try {
    {
        const m = await c.messages.create({ model: MODEL, max_tokens: 64, thinking: noThink, messages: user("Reply with one short sentence about the sea.") });
        ok(m.type === "message" && m.role === "assistant" && Array.isArray(m.content) && m.content.length >= 1, "create resolves a Message", JSON.stringify(m).slice(0, 160));
        ok(STOPS.includes(m.stop_reason), "stop_reason is a known value", m.stop_reason);
        ok(m.usage && m.usage.input_tokens > 0 && m.usage.output_tokens > 0, "usage counts tokens", JSON.stringify(m.usage));
        ok(m.content.some((b) => b.type === "text" && b.text.length > 0), "the reply has text");
    }
    {
        const m = await c.messages.create({ model: MODEL, max_tokens: 4, thinking: noThink, messages: user("Write a long story about a lighthouse keeper.") });
        ok(m.stop_reason === "max_tokens", "max_tokens 4 stops with max_tokens", m.stop_reason);
    }
    {
        const s = c.messages.stream({ model: MODEL, max_tokens: 120, thinking: noThink, messages: user("List ten fruits, one per line.") });
        ok(s instanceof MessageStream, "stream() returns a MessageStream");
        const evs = [];
        for await (const ev of s) evs.push([ev.type, Date.now()]);
        const deltas = evs.filter(([t]) => t === "content_block_delta");
        ok(evs[0][0] === "message_start" && evs[evs.length - 1][0] === "message_stop", "events run message_start .. message_stop", evs.map(([t]) => t).slice(0, 4).join(","));
        ok(deltas.length >= 5, "many deltas arrive", deltas.length);
        const span = deltas[deltas.length - 1][1] - deltas[0][1];
        ok(span >= 20, "deltas are delivered while the model generates, not all at once at the end (" + span + " ms between first and last)");
        const m = await s.finalMessage();
        ok(STOPS.includes(m.stop_reason) && m.content.some((b) => b.type === "text" && b.text.length > 10), "finalMessage after iterating has the text", JSON.stringify(m.content).slice(0, 120));
        ok(s.status === 200, "status 200");
    }
    {
        const t = await c.messages.stream({ model: MODEL, max_tokens: 200, messages: user("What is 2+2? Answer briefly.") }).finalMessage();
        const th = t.content.find((b) => b.type === "thinking");
        ok(th === undefined || (typeof th.thinking === "string" && th.thinking.length > 0), "streamed thinking deltas accumulate into a thinking block", JSON.stringify(t.content).slice(0, 120));
    }
    let toolMsg;
    {
        const req = { model: MODEL, max_tokens: 300, thinking: noThink, tools: [WEATHER], tool_choice: { type: "any" }, messages: user("What is the weather in Paris? Use the tool.") };
        toolMsg = await c.messages.create(req);
        const tu = toolMsg.content.find((b) => b.type === "tool_use");
        ok(toolMsg.stop_reason === "tool_use" && tu && tu.name === "get_weather", "a forced tool call stops with tool_use", JSON.stringify(toolMsg.content).slice(0, 160));
        ok(tu && tu.input && typeof tu.input === "object" && !Array.isArray(tu.input) && typeof tu.id === "string", "tool_use carries an id and an object input", JSON.stringify(tu));
        const sm = await c.messages.stream(req).finalMessage();
        const stu = sm.content.find((b) => b.type === "tool_use");
        ok(sm.stop_reason === "tool_use" && stu && typeof stu.input === "object" && !stu.inputError, "a streamed tool call accumulates its input_json_delta into an object", JSON.stringify(sm.content).slice(0, 160));
        if (tu) {
            const fin = await c.messages.create({ model: MODEL, max_tokens: 120, thinking: noThink, tools: [WEATHER],
                messages: [...req.messages, { role: "assistant", content: toolMsg.content }, { role: "user", content: [{ type: "tool_result", tool_use_id: tu.id, content: "18 C, light rain" }] }] });
            ok(fin.type === "message" && STOPS.includes(fin.stop_reason), "a tool_result round trip resolves a Message", fin.stop_reason);
        }
    }
    {
        let e0;
        try { await c.messages.create({ model: "dynajs-no-such-model", max_tokens: 4, messages: user("x") }); } catch (e) { e0 = e; }
        ok(e0 instanceof APIError && e0.status === 404 && e0.type === "not_found_error" && e0.retryable === false, "an unknown model is a 404 APIError not_found_error", e0 && e0.message);
    }
    {
        const s = c.messages.stream({ model: MODEL, max_tokens: 1000, thinking: noThink, messages: user("Write a very long essay about rivers.") });
        let n = 0;
        for await (const ev of s) { if (ev.type === "content_block_delta" && ++n === 3) break; }
        ok(n === 3 && (await s.next()).done === true, "a for-await break mid-generation ends the stream");
        const m = await c.messages.create({ model: MODEL, max_tokens: 8, thinking: noThink, messages: user("Say ok.") });
        ok(m.type === "message", "the server answers the next request after a cancel");
    }
} finally {
    c.close();
}
print(`test_ai_anthropic_live: ${pass} passed, ${fail} failed`);
std.exit(fail ? 1 : 0);
