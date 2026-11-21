// covers: dyna:ai.MessageAccumulator
// MessageAccumulator is the C accumulator behind MessageStream (ai-accum.c),
// fed raw SSE bytes. The golden streams follow the event shapes in
// platform.claude.com/docs/en/build-with-claude/streaming.md: message_start,
// content_block_start/delta/stop (text_delta, input_json_delta,
// thinking_delta, signature_delta, citations_delta), ping, message_delta
// (usage is cumulative) and message_stop. Every golden stream is also fed
// split at every byte offset and must give the same events and Message.
// eager_input_streaming: a tool input whose partial_json is not a JSON object
// keeps input {} and carries a non-enumerable inputError {raw, message}, so
// JSON.stringify (a replayed request) never sends it.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_ai_message_accumulator");
import { MessageAccumulator, APIError } from "dyna:ai";

let pass = 0, fail = 0;
const ok = (c, w, d) => { if (c) { pass++; print("  ok    " + w); }
                          else { fail++; print("  FAIL  " + w + (d !== undefined ? "  [" + d + "]" : "")); } };
const sse = (evs, nl = "\n") => evs.map(([e, d]) => `event: ${e}${nl}data: ${typeof d === "string" ? d : JSON.stringify(d)}${nl}${nl}`).join("");
const startMsg = (extra = {}) => ["message_start", { type: "message_start", message: { id: "msg_01", type: "message", role: "assistant", model: "claude-opus-5-5", content: [], stop_reason: null, stop_sequence: null, usage: { input_tokens: 12, output_tokens: 1, cache_read_input_tokens: 0 }, ...extra } }];
const blockStart = (i, b) => ["content_block_start", { type: "content_block_start", index: i, content_block: b }];
const delta = (i, d) => ["content_block_delta", { type: "content_block_delta", index: i, delta: d }];
const blockStop = (i) => ["content_block_stop", { type: "content_block_stop", index: i }];
const msgDelta = (d, u) => ["message_delta", { type: "message_delta", delta: d, usage: u }];
const stop = ["message_stop", { type: "message_stop" }];

const GOLDEN = sse([
    startMsg(),
    blockStart(0, { type: "thinking", thinking: "", signature: "" }),
    delta(0, { type: "thinking_delta", thinking: "Need the weather; " }),
    delta(0, { type: "thinking_delta", thinking: "call the tool twice." }),
    delta(0, { type: "signature_delta", signature: "EqQBCgIYAhIM1gbcDa9GJwZA2b3hGgxBdjrkzLoky3dl1pkiMOYds" }),
    blockStop(0),
    ["ping", { type: "ping" }],
    blockStart(1, { type: "text", text: "" }),
    delta(1, { type: "text_delta", text: "Checking été " }),
    delta(1, { type: "text_delta", text: "weather \u{1F327}." }),
    blockStop(1),
    blockStart(2, { type: "tool_use", id: "toolu_01", name: "get_weather", input: {} }),
    delta(2, { type: "input_json_delta", partial_json: "" }),
    delta(2, { type: "input_json_delta", partial_json: "{\"location\":" }),
    delta(2, { type: "input_json_delta", partial_json: " \"San Francisc" }),
    delta(2, { type: "input_json_delta", partial_json: "o, CA\"}" }),
    blockStop(2),
    blockStart(3, { type: "tool_use", id: "toolu_02", name: "get_time", input: {} }),
    blockStop(3),
    msgDelta({ stop_reason: "tool_use", stop_sequence: null }, { output_tokens: 89 }),
    stop,
]);

const EXPECT = {
    id: "msg_01", type: "message", role: "assistant", model: "claude-opus-5-5",
    content: [
        { type: "thinking", thinking: "Need the weather; call the tool twice.", signature: "EqQBCgIYAhIM1gbcDa9GJwZA2b3hGgxBdjrkzLoky3dl1pkiMOYds" },
        { type: "text", text: "Checking été weather \u{1F327}." },
        { type: "tool_use", id: "toolu_01", name: "get_weather", input: { location: "San Francisco, CA" } },
        { type: "tool_use", id: "toolu_02", name: "get_time", input: {} },
    ],
    stop_reason: "tool_use", stop_sequence: null,
    usage: { input_tokens: 12, output_tokens: 89, cache_read_input_tokens: 0 },
};
const canon = (v) => JSON.stringify(v, (k, x) => (x && typeof x === "object" && !Array.isArray(x)) ? Object.fromEntries(Object.keys(x).sort().map((q) => [q, x[q]])) : x);

__kitS.section("a fresh accumulator holds no message", () => {
    const a = new MessageAccumulator();
    ok(a.message === null && a.done === false, "a fresh accumulator holds no message");
    const evs = a.feed(GOLDEN);
    ok(evs.length === 21, "one event per SSE event, ping included", evs.length);
    ok(evs.map((e) => e.type).join(",").startsWith("message_start,content_block_start,content_block_delta"), "events keep their order");
    ok(a.done, "done after message_stop");
    ok(canon(a.message) === canon(EXPECT), "the golden stream accumulates to the expected Message", canon(a.message));
});

__kitS.section("split at every one of", () => {
    const bytes = new TextEncoder().encode(GOLDEN);
    let same = 0, evSame = 0;
    const want = canon(EXPECT);
    for (let i = 0; i <= bytes.length; i++) {
        const a = new MessageAccumulator();
        const e1 = a.feed(bytes.subarray(0, i));
        const e2 = a.feed(bytes.subarray(i));
        if (canon(a.message) === want) same++;
        if (e1.length + e2.length === 21) evSame++;
    }
    ok(same === bytes.length + 1, "split at every one of " + (bytes.length + 1) + " byte offsets: the same Message", same);
    ok(evSame === bytes.length + 1, "split at every byte offset: the same 21 events", evSame);
    const a = new MessageAccumulator();
    for (let i = 0; i < bytes.length; i++) a.feed(bytes.subarray(i, i + 1));
    ok(canon(a.message) === want, "fed one byte at a time: the same Message");
});

__kitS.section("CRLF framing; the snapshot carries text up to the last delta", () => {
    const a = new MessageAccumulator();
    a.feed(sse([startMsg(), blockStart(0, { type: "text", text: "" }), delta(0, { type: "text_delta", text: "a" })], "\r\n"));
    ok(a.message.content[0].text === "a" && !a.done, "CRLF framing; the snapshot carries text up to the last delta");
    a.feed(sse([delta(0, { type: "text_delta", text: "b" }), blockStop(0), msgDelta({ stop_reason: "end_turn", stop_sequence: null }, { output_tokens: 3 }), stop], "\r\n"));
    ok(a.message.content[0].text === "ab" && a.message.stop_reason === "end_turn" && a.done, "text keeps accumulating after a snapshot");
});

__kitS.section("{\"path\": \"a.txt\", \"contents\": \"he said \"hi\"\"}", () => {
    const a = new MessageAccumulator();
    a.feed(sse([startMsg(), blockStart(0, { type: "tool_use", id: "toolu_9", name: "write_file", input: {} }),
        delta(0, { type: "input_json_delta", partial_json: "{\"path\": \"a.txt\", \"contents\": \"he said \"hi\"\"}" }),
        blockStop(0), msgDelta({ stop_reason: "tool_use", stop_sequence: null }, { output_tokens: 20 }), stop]));
    const b = a.message.content[0];
    ok(b.inputError && b.inputError.raw === "{\"path\": \"a.txt\", \"contents\": \"he said \"hi\"\"}", "eager input that is not JSON: inputError.raw is the text received", JSON.stringify(b.inputError));
    ok(typeof b.inputError.message === "string" && b.inputError.message.length > 0, "inputError.message says why");
    ok(JSON.stringify(b.input) === "{}", "the broken input becomes {}");
    ok(!JSON.stringify(a.message).includes("inputError") && !Object.keys(b).includes("inputError"), "inputError is non-enumerable: a replayed message never sends it");
    const tr = { type: "tool_result", tool_use_id: b.id, is_error: true, content: JSON.stringify({ INVALID_JSON: b.inputError.raw }) };
    ok(JSON.parse(tr.content).INVALID_JSON === b.inputError.raw, "the documented INVALID_JSON tool_result can be built from it");
});

__kitS.section("an input that parses to an array is refused the same way", () => {
    const a = new MessageAccumulator();
    a.feed(sse([startMsg(), blockStart(0, { type: "tool_use", id: "t", name: "n", input: {} }),
        delta(0, { type: "input_json_delta", partial_json: "[1,2]" }), blockStop(0)]));
    ok(a.message.content[0].inputError && JSON.stringify(a.message.content[0].input) === "{}", "an input that parses to an array is refused the same way");
    const c = new MessageAccumulator();
    c.feed(sse([startMsg(), blockStart(0, { type: "tool_use", id: "t", name: "n", input: {} }),
        delta(0, { type: "input_json_delta", partial_json: "{\"q\": \"cut off" }),
        msgDelta({ stop_reason: "max_tokens", stop_sequence: null }, { output_tokens: 64 }), stop]));
    ok(c.message.stop_reason === "max_tokens", "max_tokens mid tool input: the stop reason says so");
});

__kitS.section("citations_delta appends to the block's citations", () => {
    const a = new MessageAccumulator();
    a.feed(sse([startMsg(), blockStart(0, { type: "text", text: "" }),
        delta(0, { type: "citations_delta", citation: { type: "char_location", cited_text: "x", document_index: 0, start_char_index: 0, end_char_index: 1 } }),
        delta(0, { type: "text_delta", text: "cited" }), blockStop(0),
        blockStart(1, { type: "redacted_thinking", data: "opaque" }), blockStop(1),
        blockStart(2, { type: "web_search_tool_result", tool_use_id: "srvtoolu_1", content: [] }), blockStop(2),
        delta(0, { type: "future_delta", x: 1 }),
        ["future_event", { type: "future_event", n: 1 }],
        msgDelta({ stop_reason: "refusal", stop_sequence: null, stop_details: { type: "refusal", category: "cyber", explanation: "x" } }, { output_tokens: 5 }), stop]));
    const m = a.message;
    ok(m.content[0].citations.length === 1 && m.content[0].text === "cited", "citations_delta appends to the block's citations");
    ok(m.content[1].type === "redacted_thinking" && m.content[1].data === "opaque", "a block that arrives whole is kept as sent");
    ok(m.content[2].type === "web_search_tool_result", "a server tool result block is kept");
    ok(m.stop_reason === "refusal" && m.stop_details.category === "cyber", "refusal with stop_details lands on the message");
});

__kitS.section("stop_reason", () => {
    for (const [e, d] of [["pause_turn", "pause_turn"], ["stop_sequence", "stop_sequence"], ["end_turn", "end_turn"]]) {
        const a = new MessageAccumulator();
        a.feed(sse([startMsg(), msgDelta({ stop_reason: e, stop_sequence: e === "stop_sequence" ? "###" : null }, { output_tokens: 1 }), stop]));
        ok(a.message.stop_reason === d && (e !== "stop_sequence" || a.message.stop_sequence === "###"), "stop_reason " + e + " is carried");
    }
});

const throwsAPI = (fn, pred, w) => {
    try { fn(); ok(false, w, "no throw"); }
    catch (e) { ok(e instanceof APIError && pred(e), w, e && (e.constructor.name + ": " + e.message + " status=" + e.status + " type=" + e.type)); }
};

__kitS.section("a mid-stream error event throws APIError 529 overloaded_error", () => {
    const a = new MessageAccumulator();
    a.feed(sse([startMsg(), blockStart(0, { type: "text", text: "" }), delta(0, { type: "text_delta", text: "part" })]));
    throwsAPI(() => a.feed(sse([["error", { type: "error", error: { type: "overloaded_error", message: "Overloaded" } }]])),
        (e) => e.status === 529 && e.type === "overloaded_error" && /Overloaded/.test(e.message) && e.retryable === true,
        "a mid-stream error event throws APIError 529 overloaded_error");
    ok(a.message.content[0].text === "part", "the partial message survives the error");
    throwsAPI(() => new MessageAccumulator().feed(sse([["error", { type: "error", error: { type: "api_error", message: "boom" }, request_id: "req_1" }]])),
        (e) => e.status === 500 && e.requestId === "req_1", "an error event's request_id becomes requestId");
    throwsAPI(() => new MessageAccumulator().feed(sse([startMsg(), ["content_block_delta", "{not json"]])),
        (e) => e.status === 0 && e.type === "stream_error" && /not a JSON object/.test(e.message), "a malformed event data line throws stream_error");
    throwsAPI(() => new MessageAccumulator().feed(sse([blockStart(0, { type: "text", text: "" })])),
        (e) => e.type === "stream_error" && /before message_start/.test(e.message), "content before message_start throws stream_error");
    throwsAPI(() => new MessageAccumulator().feed(sse([startMsg(), ["content_block_start", { type: "content_block_start", content_block: { type: "text" } }]])),
        (e) => e.type === "stream_error" && /missing a required field/.test(e.message), "a block event without an index throws stream_error");
    const p = new MessageAccumulator();
    p.feed(sse([["ping", { type: "ping" }]]));
    ok(p.message === null, "a ping before message_start is harmless");
});

__kitS.section("feed refuses a number", () => {
    let threw = null;
    try { new MessageAccumulator().feed(42); } catch (e) { threw = e; }
    ok(threw instanceof TypeError, "feed refuses a number", threw && threw.message);
    let t2 = null;
    try { MessageAccumulator(); } catch (e) { t2 = e; }
    ok(t2 instanceof TypeError, "MessageAccumulator needs new");
});
__kitS.end();

print(`test_ai_message_accumulator: ${pass} passed, ${fail} failed`);
if (fail) throw new Error("test_ai_message_accumulator failed");
