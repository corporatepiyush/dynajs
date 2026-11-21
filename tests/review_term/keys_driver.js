// flags: --std
// keys_driver.js -- adversarial key-decoder scenarios with owned byte timing.
// Each scenario: list of [bytes, sleep_after_sec] chunks written to the child's
// stdin, expected decoded event list. Prints PASS/FAIL per scenario (each one is
// a probe). The driver synchronizes on the child's flushed READY marker before
// the first write, so a slow child start cannot coalesce scenario bytes that
// must arrive split (the scenarios are about arrival gaps, not about startup).
// Port of keys_driver.py: stdin is a pipe written chunk by chunk through Spawn.
import * as std from "std";
import { Spawn } from "dyna:sys";

const BIN = std.getenv("DYNAJS") || "./dynajs";
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const b = (s) => Uint8Array.from(s, (c) => c.charCodeAt(0));
const cat = (...a) => { const o = new Uint8Array(a.reduce((n, x) => n + x.length, 0)); let k = 0; for (const x of a) { o.set(x, k); k += x.length; } return o; };
const ones = (n) => b("1".repeat(n));

async function drain(pipe, sink) {
    for await (const c of pipe) sink.push(c);
}
async function run(chunks, timeoutMs = 20000) {
    const p = new Spawn(BIN, ["--std", "tests/review_term/probe_keys.js"], { stdin: "pipe", timeoutMs });
    const outC = [], errC = [];
    const outDone = drain(p.stdout, outC), errDone = drain(p.stderr, errC);
    const text = () => new TextDecoder().decode(cat(...outC));
    const deadline = Date.now() + 15000;
    while (Date.now() < deadline && !text().includes("READY\n") && !p.exited) await sleep(10);
    try {
        for (const [data, gap] of chunks) {
            if (data.length) { await p.stdin.write(data); await p.stdin.flush(); }
            if (gap) await sleep(gap * 1000);
        }
    } catch (e) {}
    p.stdin.close();
    const res = await p.wait();
    await outDone; await errDone;
    p.close();
    if (res.timedOut) return ["<TIMEOUT>"];
    const all = text() + new TextDecoder().decode(cat(...errC));
    return all.split(/\r\n|\n|\r/).filter((l) => l.startsWith("E|") || l.startsWith("EOF") || l.startsWith("<TIMEOUT>"));
}
const ev = (name, hex, cms) => `E|${name}|${hex}|${cms}`;

const SCN = [
    ["K1", "ESC [ A split 60ms apart degrades to 3 events (timeout window)",
        [[b("\x1b"), .2], [b("["), .2], [b("A"), .1]],
        [ev("escape", "1b,", "000"), ev("[", "5b,", "000"), ev("A", "41,", "000"), "EOF"]],
    ["K2", "ESC [ A fast decodes as one 'up'",
        [[b("\x1b[A"), .05]],
        [ev("up", "1b,5b,41,", "000"), "EOF"]],
    ["K3", "ESC [ 1 ; split from 5 ~ : truncated event, leftovers decode as chars",
        [[b("\x1b[1;"), .2], [b("5~"), .1]],
        [ev("<null>", "1b,5b,31,3b,", "000"), ev("5", "35,", "000"), ev("~", "7e,", "000"), "EOF"]],
    ["K4", "ESC ESC is escape with meta",
        [[b("\x1b\x1b"), .06]],
        [ev("escape", "1b,1b,", "010"), "EOF"]],
    ["K5", "ESC ESC A is meta+A",
        [[b("\x1b\x1bA"), .05]],
        [ev("A", "1b,1b,41,", "010"), "EOF"]],
    ["K6", "bare ESC times out to 'escape', late q is its own key",
        [[b("\x1b"), .2], [b("q"), .1]],
        [ev("escape", "1b,", "000"), ev("q", "71,", "000"), "EOF"]],
    ["K7", "ESC [ A split 30ms apart stays one 'up' event",
        [[b("\x1b"), .02], [b("["), .02], [b("A"), .1]],
        [ev("up", "1b,5b,41,", "000"), "EOF"]],
    ["K8", "CSI 9:;<=>?~ garbage params: one event, name null, then clean stream",
        [[b("\x1b[9:;<=>?~"), .05], [b("z"), .05]],
        [ev("<null>", "1b,5b,39,3a,3b,3c,3d,3e,3f,7e,", "000"), ev("z", "7a,", "000"), "EOF"]],
    ["K9", "CSI with 30 param digits: params overflow spares no crash; stream spills",
        [[cat(b("\x1b["), ones(30), b("~")), .05]],
        ["SPLILL"]],
    ["K10", "SS3 P/Q/R/S decode to name-null events with full sequence",
        [[b("\x1bOP\x1bOQ\x1bOR\x1bOS"), .05]],
        [ev("<null>", "1b,4f,50,", "000"), ev("<null>", "1b,4f,51,", "000"),
            ev("<null>", "1b,4f,52,", "000"), ev("<null>", "1b,4f,53,", "000"), "EOF"]],
    ["K11", "Ctrl-C/D/Z are ctrl chords c/d/z (ISIG off), never EOF",
        [[b("\x03\x04\x1a"), .05]],
        [ev("c", "3,", "100"), ev("d", "4,", "100"), ev("z", "1a,", "100"), "EOF"]],
    ["K12", "e-acute (c3 a9) decodes to one key named U+00E9",
        [[b("\xc3\xa9"), .05]],
        [ev("\u00e9", "e9,", "000"), "EOF"]],
    ["K13", "lone 0x80: name null, sequence is U+FFFD (d.ts claim)",
        [[b("\x80"), .05]],
        [ev("<null>", "fffd,", "000"), "EOF"]],
    ["K14a", "truncated c3 at EOF: name null, sequence U+FFFD",
        [[b("\xc3"), .05]],
        [ev("<null>", "fffd,", "000"), "EOF"]],
    ["K14b", "c3 then non-continuation A within the window: A must not vanish",
        [[b("\xc3A"), .05]],
        ["SWALLOW"]],
    ["K14c", "c3 then late A: two events",
        [[b("\xc3"), .2], [b("A"), .1]],
        [ev("<null>", "fffd,", "000"), ev("A", "41,", "000"), "EOF"]],
    ["K15", "4-byte emoji is one key, name is the character",
        [[b("\xf0\x9f\x98\x80"), .05]],
        [ev("\u{1f600}", "1f600,", "000"), "EOF"]],
    ["K16", "CSI modifier encodings: Z=shift+tab, 1;5C=ctrl+right, 5~/6~=pages, 1~/4~/3~",
        [[b("\x1b[Z\x1b[1;5C\x1b[5~\x1b[6~\x1b[1~\x1b[4~\x1b[3~\x1b[H\x1bOF"), .05]],
        [ev("tab", "1b,5b,5a,", "001"), ev("right", "1b,5b,31,3b,35,43,", "100"),
            ev("pageup", "1b,5b,35,7e,", "000"), ev("pagedown", "1b,5b,36,7e,", "000"),
            ev("home", "1b,5b,31,7e,", "000"), ev("end", "1b,5b,34,7e,", "000"),
            ev("delete", "1b,5b,33,7e,", "000"), ev("home", "1b,5b,48,", "000"),
            ev("end", "1b,4f,46,", "000"), "EOF"]],
    ["K17", "ESC ESC [ A is meta+up, one event, 4-byte sequence",
        [[b("\x1b\x1b[A"), .05]],
        [ev("up", "1b,1b,5b,41,", "010"), "EOF"]],
    ["K18", "empty stdin: keypress() is null at once",
        [[b(""), .05]],
        ["EOF"]],
    ["K19", "20-byte ESC run: 15 + 5 delivered, 20 bytes consumed 20 reported",
        [[b("\x1b".repeat(20)), .05]],
        [ev("escape", "1b,".repeat(15), "010"), ev("escape", "1b,".repeat(5), "010"), "EOF"]],
    ["K20", "CSI param-overflow spill is identical whether one write or in-window split",
        [[cat(b("\x1b["), ones(13)), .02], [ones(10), .02], [cat(ones(7), b("~")), .05]],
        ["SPLILL2"]],
    ["K21", "ESC ESC [ A split in-window is still one meta+up event",
        [[b("\x1b\x1b"), .02], [b("[A"), .05]],
        [ev("up", "1b,1b,5b,41,", "010"), "EOF"]],
];

const J = JSON.stringify;
const same = (a, b2) => a.length === b2.length && a.every((v, i) => v === b2[i]);
let fails = 0;
for (const [pid, desc, chunks, expect] of SCN) {
    const got = await run(chunks);
    let ok, detail;
    if (expect[0] === "SPLILL" || expect[0] === "SPLILL2") {
        ok = got.length === 21 && got[0].startsWith("E|<null>|") &&
            got.slice(1, 19).every((g) => g === ev("1", "31,", "000")) &&
            got[19] === ev("~", "7e,", "000") && got[20] === "EOF";
        detail = `got ${got.length} events, first=${J(got[0] ?? null)}`;
    } else if (expect[0] === "SWALLOW") {
        const want = [ev("<null>", "fffd,", "000"), ev("A", "41,", "000"), "EOF"];
        ok = same(got, want);
        if (!ok) detail = same(got, [ev("<null>", "fffd,", "000"), "EOF"])
            ? `BYTE-SWALLOWED: 'A' consumed but neither delivered nor reported (want ${J(want)} got ${J(got)})`
            : `want ${J(want)} got ${J(got)}`;
    } else {
        ok = same(got, expect);
        detail = `want ${J(expect)} got ${J(got)}`;
    }
    print(`${ok ? "PASS" : "FAIL"} ${pid} -- ${desc}${ok ? "" : " :: " + detail}`);
    if (!ok) fails++;
}
print(`keys_driver: ${SCN.length - fails}/${SCN.length} scenarios ok`);
std.exit(fails ? 1 : 0);
