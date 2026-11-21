// covers: c:src/core/httpk/httpk-sse.c
// The SSE reader under MessageStream / MessageAccumulator / ChatCompletionAccumulator finds each
// line end with memchr for LF and then memchr for CR before it (a byte loop testing both before;
// 4-6% of feeding a 1K-100K-delta stream). An event must come out the same for LF, CRLF and lone
// CR line ends, mixed per line (a blank line after a lone CR is CRLF there: CR LF is one line
// end), for a CR that ends one chunk and an LF that starts the next, and for every chunk split. The oracle is the list of event objects this file serialises: each feed
// must return exactly them, in order, and the accumulated text must equal the deltas joined.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_ai_sse_line_ends");
import { MessageAccumulator, ChatCompletionAccumulator } from "dyna:ai";

let pass = 0, fail = 0;
const eq = (a, b, m) => { if (a === b) pass++; else { fail++; print("FAIL " + m + ": got " + String(a).slice(0, 120) + ", want " + String(b).slice(0, 120)); } };
const enc = new TextEncoder();

const EVENTS = [{ type: "message_start", message: { id: "m1", type: "message", role: "assistant", model: "x", content: [], stop_reason: null, stop_sequence: null, usage: { input_tokens: 1, output_tokens: 1 } } },
    { type: "content_block_start", index: 0, content_block: { type: "text", text: "" } }];
const DELTAS = [];
for (let i = 0; i < 60; i++) {
    const t = " d" + i + (i % 7 === 0 ? "\r\né" : "") + (i % 11 === 0 ? "x".repeat(300) : "");
    DELTAS.push(t);
    EVENTS.push({ type: "content_block_delta", index: 0, delta: { type: "text_delta", text: t } });
    if (i % 13 === 0) EVENTS.push({ type: "ping" });
}
EVENTS.push({ type: "content_block_stop", index: 0 }, { type: "message_delta", delta: { stop_reason: "end_turn", stop_sequence: null }, usage: { output_tokens: 9 } }, { type: "message_stop" });

function wire(eolOf) {
    let s = "\uFEFF", n = 0;
    const L = (x) => x + eolOf(n++, x);
    for (const e of EVENTS) {
        if (n % 5 === 0) s += L(": keep-alive");
        s += L("event: " + e.type);
        const j = JSON.stringify(e);
        if (e.type === "ping") s += L("data:" + j);
        else if (e.type === "content_block_stop") s += L("data: " + j.slice(0, j.indexOf(",") + 1)) + L("data: " + j.slice(j.indexOf(",") + 1));
        else s += L("data: " + j);
        s += L("");
    }
    return enc.encode(s);
}
const EOLS = { lf: () => "\n", crlf: () => "\r\n", cr: () => "\r", mixed: (n, x) => (x === "" ? "\r\n" : ["\n", "\r\n", "\r"][n % 3]) };

const key = (e) => (e.type === "message_start" ? "start:" + e.message.id : JSON.stringify(e));
function run(bytes, splits) {
    const a = new MessageAccumulator();
    const got = [];
    let i = 0;
    for (const s of splits) { for (const e of a.feed(bytes.subarray(i, s))) got.push(key(e)); i = s; }
    for (const e of a.feed(bytes.subarray(i))) got.push(key(e));
    return { got, a };
}
const WANT = EVENTS.map(key);

__kitS.section("every line-end style and fixed chunk sizes", () => {
    for (const [name, f] of Object.entries(EOLS)) {
        const b = wire(f);
        for (const step of [1, 2, 3, 5, 64, 333, 4096, b.length]) {
            const splits = [];
            for (let i = step; i < b.length; i += step) splits.push(i);
            const { got, a } = run(b, splits);
            eq(got.length, WANT.length, name + " step " + step + " event count");
            eq(got.join("\n"), WANT.join("\n"), name + " step " + step + " events");
            eq(a.done, true, name + " step " + step + " done");
            eq(a.message.content[0].text, DELTAS.join(""), name + " step " + step + " text");
        }
    }
});

__kitS.section("a split right after every CR and before every LF", () => {
    for (const name of ["crlf", "cr", "mixed"]) {
        const b = wire(EOLS[name]);
        const at = [];
        for (let i = 0; i < b.length; i++) if (b[i] === 13 || b[i] === 10) at.push(i, i + 1);
        const { got } = run(b, [...new Set(at)].filter((x) => x > 0 && x < b.length).sort((x, y) => x - y));
        eq(got.join("\n"), WANT.join("\n"), name + " split at every line-end byte");
    }
});

__kitS.section("chat completions chunks with CRLF and CR", () => {
    for (const eol of ["\n", "\r\n", "\r"]) {
        let s = "", want = "";
        for (let i = 0; i < 120; i++) {
            const c = "w" + i + (i % 9 ? "" : "\r\n");
            want += c;
            s += "data: " + JSON.stringify({ id: "c", object: "chat.completion.chunk", created: 1, model: "m", choices: [{ index: 0, delta: { content: c }, finish_reason: null }] }) + eol + eol;
        }
        s += "data: [DONE]" + eol + eol;
        for (const step of [1, 7, 100000]) {
            const a = new ChatCompletionAccumulator();
            let n = 0;
            for (let i = 0; i < s.length; i += step) n += a.feed(s.slice(i, i + step)).length;
            eq(n, 120, JSON.stringify(eol) + " step " + step + " chunks");
            eq(a.done, true, JSON.stringify(eol) + " step " + step + " done");
            eq(a.completion.choices[0].message.content, want, JSON.stringify(eol) + " step " + step + " content");
        }
    }
});

__kitS.end();
print(`test_ai_sse_line_ends: ${pass} passed, ${fail} failed`);
if (fail) throw new Error(fail + " failed");
