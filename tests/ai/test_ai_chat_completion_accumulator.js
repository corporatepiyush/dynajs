// covers: dyna:ai.ChatCompletionAccumulator
// The Chat Completions stream accumulator on its own (no I/O): raw SSE bytes
// in, chunks and the accumulated ChatCompletion out. A golden stream with two
// interleaved choices (n: 2), reasoning and reasoning_content deltas, a
// refusal, logprobs, two tool calls whose arguments arrive in pieces (a later
// piece carrying an empty id and name must not clear the first ones), an
// unknown delta key and a usage chunk is fed whole, then split at every byte
// offset (LF and CRLF framing): every split must give the same chunks and the
// same completion. Also pins: [DONE] ends it and later bytes are ignored, an
// error object throws APIError, a non-object data line throws stream_error.
import { sections as __kitSections } from "../lib/kit.js";
import { ChatCompletionAccumulator, APIError } from "dyna:ai";
const __kitS = __kitSections("test_ai_chat_completion_accumulator", { chain: true });
let CHUNKS;

let pass = 0, fail = 0;
const ok = (c, w, d) => { if (c) { pass++; print("  ok    " + w); }
                          else { fail++; print("  FAIL  " + w + (d !== undefined ? "  [" + d + "]" : "")); } };
const throws = (f, pred, w) => { try { f(); ok(false, w, "no throw"); } catch (e) { ok(pred(e), w, e && e.constructor.name + ": " + e.message); } };

const ck = (choices, extra) => ({ id: "chatcmpl-g", object: "chat.completion.chunk", created: 42, model: "m", choices, ...(extra || {}) });
const lp = (t) => ({ content: [{ token: t, logprob: -0.5, bytes: null, top_logprobs: [] }], refusal: null });
const frame = (eol) => CHUNKS.map((c) => "data: " + JSON.stringify(c) + eol + eol).join("") + ": keep-alive" + eol + eol + "data: [DONE]" + eol + eol
    + "data: " + JSON.stringify(ck([{ index: 0, delta: { content: "AFTER" } }])) + eol + eol;

const feedAll = (bytes, cuts) => {
    const a = new ChatCompletionAccumulator();
    const evs = [];
    let at = 0;
    for (const c of [...cuts, bytes.length]) { for (const e of a.feed(bytes.subarray(at, c))) evs.push(e); at = c; }
    return { a, evs };
};
__kitS.section("fixture: define the streamed chunks", () => {
CHUNKS = [
    ck([{ index: 0, delta: { role: "assistant", content: "", reasoning: "think " }, finish_reason: null }, { index: 1, delta: { role: "assistant", reasoning_content: "other " }, finish_reason: null }]),
    ck([{ index: 0, delta: { reasoning: "more" }, finish_reason: null }, { index: 1, delta: { content: "B1 " }, finish_reason: null, logprobs: lp("B1") }]),
    ck([{ index: 0, delta: { content: "Héllo " }, finish_reason: null, logprobs: lp("H") }, { index: 1, delta: { reasoning_content: "done", content: "B2" }, finish_reason: null, logprobs: lp("B2") }]),
    ck([{ index: 0, delta: { content: "wörld 🌍" }, finish_reason: null, logprobs: lp("w") }]),
    ck([{ index: 1, delta: { tool_calls: [{ index: 0, id: "call_1", type: "function", function: { name: "lookup", arguments: "{\"q\":" } }] }, finish_reason: null }]),
    ck([{ index: 1, delta: { tool_calls: [{ index: 1, id: "call_2", type: "function", function: { name: "now", arguments: "" } }, { index: 0, id: "", function: { name: "", arguments: "\"x\"}" } }] }, finish_reason: null }]),
    ck([{ index: 1, delta: { tool_calls: [{ index: 1, function: { arguments: "{}" } }], audio: { id: "aud_1" } }, finish_reason: null }]),
    ck([{ index: 0, delta: { refusal: "I can" }, finish_reason: null }]),
    ck([{ index: 0, delta: { refusal: "not." }, finish_reason: "stop" }, { index: 1, delta: {}, finish_reason: "tool_calls" }]),
    ck([], { usage: { prompt_tokens: 7, completion_tokens: 30, total_tokens: 37, completion_tokens_details: { reasoning_tokens: 4 } } }),
];
});

{
let a, evs, f, c0, c1, golden;
__kitS.section("completion is null and done false before any chunk", () => {
    a = new ChatCompletionAccumulator();
    ok(a.completion === null && a.done === false, "completion is null and done false before any chunk");
});
__kitS.section("feed returns every chunk as sent, never [DONE] or what follows it", () => {
    evs = a.feed(frame("\n"));
    ok(evs.length === CHUNKS.length && JSON.stringify(evs) === JSON.stringify(CHUNKS), "feed returns every chunk as sent, never [DONE] or what follows it", evs.length);
    ok(a.done === true, "done after [DONE]");
});
__kitS.section("top-level fields; object becomes chat.completion", () => {
    f = a.completion;
    c0 = f.choices[0], c1 = f.choices[1];
    ok(f.object === "chat.completion" && f.id === "chatcmpl-g" && f.created === 42 && f.model === "m", "top-level fields; object becomes chat.completion");
    ok(c0.index === 0 && c0.message.role === "assistant" && c0.message.content === "Héllo wörld 🌍" && c0.message.reasoning === "think more", "choice 0: UTF-8 content and reasoning concatenate", JSON.stringify(c0.message));
    ok(c0.message.refusal === "I cannot." && c0.finish_reason === "stop" && !("tool_calls" in c0.message), "choice 0: refusal concatenates, finish_reason stop");
    ok(c1.message.content === "B1 B2" && c1.message.reasoning_content === "other done" && !("reasoning" in c1.message) && c1.message.refusal === null, "choice 1: content and reasoning_content", JSON.stringify(c1.message));
    ok(c1.finish_reason === "tool_calls" && c1.message.tool_calls.length === 2, "choice 1: two tool calls, finish_reason tool_calls");
    ok(JSON.stringify(c1.message.tool_calls) === JSON.stringify([{ id: "call_1", type: "function", function: { name: "lookup", arguments: "{\"q\":\"x\"}" } }, { id: "call_2", type: "function", function: { name: "now", arguments: "{}" } }]),
        "tool calls keyed by index: interleaved argument pieces join in order", JSON.stringify(c1.message.tool_calls));
    ok(c1.message.audio && c1.message.audio.id === "aud_1", "an unknown delta key is copied onto the message");
    ok(c0.logprobs.content.map((x) => x.token).join(",") === "H,w" && c1.logprobs.content.map((x) => x.token).join(",") === "B1,B2", "logprobs.content arrays concatenate per choice");
    ok(f.usage.total_tokens === 37 && f.usage.completion_tokens_details.reasoning_tokens === 4, "usage from the choices-less chunk");
});
__kitS.section("bytes after [DONE] are ignored", () => {
    golden = JSON.stringify(f);
    ok(a.feed("data: " + JSON.stringify(ck([{ index: 0, delta: { content: "X" } }])) + "\n\n").length === 0 && JSON.stringify(a.completion) === golden, "bytes after [DONE] are ignored");
});
__kitS.section("every single split point gives the same completion", () => {
    for (const eol of ["\n", "\r\n"]) {
        const bytes = new TextEncoder().encode(frame(eol));
        let bad = 0, first = "";
        for (let i = 1; i < bytes.length; i++) {
            const { a: b, evs: e } = feedAll(bytes, [i]);
            if (JSON.stringify(b.completion) !== golden || e.length !== CHUNKS.length || !b.done) { bad++; if (!first) first = "cut " + i; }
        }
        ok(bad === 0, "every single split point gives the same completion (" + JSON.stringify(eol) + ", " + bytes.length + " cuts)", bad + " bad, first " + first);
        const { a: d } = feedAll(bytes, Array.from({ length: bytes.length - 1 }, (_, i) => i + 1));
        ok(JSON.stringify(d.completion) === golden, "byte-at-a-time feeding gives the same completion " + JSON.stringify(eol));
    }
});
}
{
let a, b;
__kitS.section("an error event in the stream is surfaced", () => {
    a = new ChatCompletionAccumulator();
    a.feed("data: " + JSON.stringify(ck([{ index: 0, delta: { content: "par" } }])) + "\n\n");
    throws(() => a.feed('data: {"error":{"message":"overloaded","type":"server_error","param":null,"code":"x"}}\n\n'),
        (e) => e instanceof APIError && e.type === "server_error" && /overloaded/.test(e.message) && e.body.error.code === "x", "an error object throws APIError with its type and body");
    ok(a.completion.choices[0].message.content === "par" && a.done === false, "the partial completion stays readable");
    throws(() => new ChatCompletionAccumulator().feed("data: [1,2]\n\n"), (e) => e instanceof APIError && e.type === "stream_error", "a data line that is an array throws stream_error");
    throws(() => new ChatCompletionAccumulator().feed("data: nope\n\n"), (e) => e instanceof APIError && e.type === "stream_error", "a data line that is not JSON throws stream_error");
    throws(() => new ChatCompletionAccumulator().feed("data: " + JSON.stringify(ck([{ index: 5000, delta: {} }])) + "\n\n"), (e) => e instanceof APIError && e.type === "stream_error", "a choice index past 1023 throws stream_error");
    throws(() => new ChatCompletionAccumulator().feed("data: " + JSON.stringify(ck([{ index: 0, delta: { tool_calls: [{ index: -1 }] } }])) + "\n\n"), (e) => e instanceof APIError && e.type === "stream_error", "a negative tool call index throws stream_error");
});
__kitS.section("a tool call without index is keyed by its array position", () => {
    b = new ChatCompletionAccumulator();
    b.feed("data: " + JSON.stringify(ck([{ index: 0, delta: { tool_calls: [{ id: "c", function: { name: "f", arguments: "{" } }] } }])) + "\n\n");
    b.feed("data: " + JSON.stringify(ck([{ index: 0, delta: { tool_calls: [{ function: { arguments: "}" } }] } }])) + "\n\n");
    ok(b.completion.choices[0].message.tool_calls[0].function.arguments === "{}" && b.completion.choices[0].message.tool_calls[0].id === "c", "a tool call without index is keyed by its array position");
    throws(() => ChatCompletionAccumulator(), (e) => e instanceof TypeError, "ChatCompletionAccumulator needs new");
    throws(() => new ChatCompletionAccumulator().feed(), (e) => e instanceof TypeError && /ChatCompletionAccumulator.feed/.test(e.message), "feed() without a chunk is a TypeError");
});
}
__kitS.end();
if (!__kitS.listing) {
print(`test_ai_chat_completion_accumulator: ${pass} passed, ${fail} failed`);
if (fail) throw new Error("test_ai_chat_completion_accumulator failed");
}
