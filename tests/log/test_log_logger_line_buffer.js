// covers: c:src/modules/log/log-line-buffer.c
// Every byte of a log line goes through dyn_line_put. It is now an inline fast path (copy when the
// piece fits the buffer and the 64 KiB line limit and the line is not truncated) in front of the
// out-of-line grow/truncate path; the out-of-line call plus a libc memcpy per 1..10-byte piece was
// ~38% of Logger#info, which became 17-43% faster across the json/text/iso/child/sink workloads.
// Oracles, all computed here in JS: a JSON line is JSON.stringify({level, name, ...base, ...fields,
// msg}) + "\n" with U+007F, U+0080..U+009F, U+2028 and U+2029 also \u-escaped; a text line is
// "LEVEL name: msg k=v ...\n". Lengths sweep across the 512-byte inline buffer and every growth
// step. A line over the limit is cut at 65520 bytes: when the cut falls inside a string value the
// line is the first 65520 bytes + '..."' + the closing braces + "\n". The long cases make the
// buffer grow to 64 KiB first and then cross 65520 with small pieces, so a fast path that skipped
// the limit check would keep bytes past the cut.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_log_logger_line_buffer");
import { Logger } from "dyna:log";

let pass = 0, fail = 0;
const eq = (a, b, m) => {
    if (a === b) pass++;
    else {
        fail++;
        let i = 0;
        while (i < a.length && a[i] === b[i]) i++;
        print("FAIL " + m + ": len " + a.length + " vs " + b.length + ", first difference at " + i + ": got " + JSON.stringify(a.slice(i, i + 30)) + ", want " + JSON.stringify(b.slice(i, i + 30)));
    }
};
const ok = (c, m) => { if (c) pass++; else { fail++; print("FAIL " + m); } };
const esc = (s) => JSON.stringify(s).replace(/[\u007f-\u009f\u2028\u2029]/g, (c) => "\\u" + c.charCodeAt(0).toString(16).padStart(4, "0"));
const jsonLine = (obj) => "{" + Object.entries(obj).map(([k, v]) => esc(k) + ":" + (typeof v === "string" ? esc(v) : JSON.stringify(v))).join(",") + "}\n";
const u8len = (s) => new TextEncoder().encode(s).length;

function capture(opts) {
    const lines = [];
    const log = new Logger(Object.assign({ level: "trace", timestamp: false, dest: (x) => lines.push(x) }, opts));
    return { log, lines };
}

const PIECES = ["a", "é", "日", "🎉", "\n", "\t", "\"", "\\", "\u0001", "\u007f", "\u0085", "\u2028", "\u2029", " ", "z9"];
function text(n, seed) {
    let s = "", k = seed;
    while (s.length < n) { k = (k * 1103515245 + 12345) >>> 0; s += k % 5 ? "abcdefghij".slice(0, 1 + (k % 9)) : PIECES[k % PIECES.length]; }
    return s.slice(0, n);
}

__kitS.section("json lines across the inline buffer and growth steps", () => {
    const { log, lines } = capture({ name: "svc", base: { app: "x", n: 7 } });
    const want = [];
    for (let n = 0; n <= 1400; n += n < 600 ? 1 : 13) {
        const msg = text(n, n + 1), f = { k: text(n % 97, n), i: n, ok: !!(n & 1), z: null };
        log.info(f, msg);
        want.push(jsonLine(Object.assign({ level: "info", name: "svc", app: "x", n: 7 }, f, { msg })));
    }
    for (const n of [4000, 9000, 20000, 40000]) {
        const msg = text(n, n);
        log.warn(msg);
        want.push(jsonLine({ level: "warn", name: "svc", app: "x", n: 7, msg }));
    }
    eq(lines.length, want.length, "line count");
    for (let i = 0; i < want.length; i++) eq(lines[i], want[i], "json line " + i);
});

__kitS.section("text lines across the inline buffer", () => {
    const { log, lines } = capture({ format: "text", name: "nm" });
    const want = [];
    for (let n = 0; n <= 1200; n += n < 600 ? 1 : 11) {
        const msg = "m".repeat(n), v = "v".repeat(n % 300);
        log.info({ a: v, b: n }, msg);
        want.push("info  nm: " + msg + " a=" + v + " b=" + n + "\n");
    }
    for (let i = 0; i < want.length; i++) eq(lines[i], want[i], "text line " + i);
});

__kitS.section("a cut inside a \\u escape drops the partial escape", () => {
    for (const tail of ["\u0001", "\\u0001", "\\\u0001"]) {
        for (let d = 0; d < 10; d++) {
            const { log, lines } = capture({});
            log.info("x".repeat(65520 - 22 - d) + tail.repeat(3));
            let parsed = null;
            try { parsed = JSON.parse(lines[0]); } catch (e) { }
            ok(parsed !== null && parsed.msg.endsWith("..."), "cut " + d + " in " + JSON.stringify(tail) + " parses: " + JSON.stringify(lines[0].slice(-16)));
        }
    }
});

__kitS.section("a line over 64 KiB is cut at 65520 bytes after the buffer grew", () => {
    const { log, lines } = capture({});
    let exact = 0;
    for (let a = 65380; a <= 65520; a++) {
        const f = { a: "x".repeat(a), b: "y".repeat(40), c: "z".repeat(30) };
        log.info(f, "tail");
        const got = lines[lines.length - 1];
        const full = jsonLine(Object.assign({ level: "info" }, f, { msg: "tail" }));
        ok(u8len(got) <= 65536 && got.endsWith("\n"), "cut line stays within 64 KiB, a=" + a);
        let parsed = null;
        try { parsed = JSON.parse(got); } catch (e) { /* reported below */ }
        ok(parsed !== null, "cut line is JSON, a=" + a);
        const cut = full.slice(0, 65520);
        const q = cut.lastIndexOf("\"");
        const inValue = q > 0 && cut[q - 1] === ":" && !cut.slice(q + 1).includes("\"");
        if (inValue && cut.length < full.length - 1) {
            exact++;
            eq(got, cut + "...\"}\n", "cut inside a value, a=" + a);
        }
    }
    ok(exact >= 60, "the sweep put the cut inside a value " + exact + " times");
});

__kitS.end();
print(`test_log_logger_line_buffer: ${pass} passed, ${fail} failed`);
if (fail) throw new Error(fail + " failed");
