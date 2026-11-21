// flags: --std
// covers: dyna:stream.lines
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_stream_lines");
import { fromBytes, fromFile, toFile, lines, ndjson } from "dyna:stream";
import {
    Path, writeFile, readFile, readBytes, makeDir, removeAll,
} from "dyna:file";
import * as std from "std";
import * as mod_compress from "dyna:compress";

let n = 0;
function assert(cond, msg) {
    n++;
    if (!cond) throw new Error("assertion failed: " + msg);
}
function assertEq(got, want, msg) {
    n++;
    if (got !== want)
        throw new Error("assertion failed: " + msg + " (got " + got + ", want " + want + ")");
}
async function assertRejects(p, msg, code) {
    n++;
    try {
        await p;
    } catch (e) {
        if (code !== undefined && e.code !== code)
            throw new Error("assertion failed: " + msg + ": wrong code " + e.code);
        return;
    }
    throw new Error("assertion failed: " + msg + " (promise resolved, expected rejection)");
}
function assertThrows(fn, msg) {
    n++;
    try {
        fn();
    } catch (e) {
        return;
    }
    throw new Error("assertion failed: " + msg + " (did not throw)");
}

const TMP = std.getenv("TMPDIR") || "/tmp";
const dir = new Path(TMP, "dynastream-test-" + (Date.now() % 10000000));
makeDir(dir, { recursive: true });
let seq = 0;
const tmpPath = (name) => new Path(dir, name + "." + (++seq) + ".tmp");
const enc = new TextEncoder();

await __kitS.section("lines basic split", async () => {
    const out = [];
    for await (const l of lines(fromBytes("alpha\nbeta\ngamma\n"))) out.push(l);
    assertEq(JSON.stringify(out), JSON.stringify(["alpha", "beta", "gamma"]),
        "lines basic split");
});

await __kitS.section("1-byte-read source keeps lines intact across boundaries", async () => {
    let i = 0;
    const text = "one\ntwo!\nthree";
    const src = {
        async read(buf) {
            if (i >= text.length) return 0;
            buf[0] = text.charCodeAt(i++);
            return 1;
        },
    };
    const out = [];
    for await (const l of lines(src)) out.push(l);
    assertEq(JSON.stringify(out), JSON.stringify(["one", "two!", "three"]),
        "1-byte-read source keeps lines intact across boundaries");
});

await __kitS.section("residual final line without newline", async () => {
    const out = [];
    for await (const l of lines(fromBytes("a\nb\nc"))) out.push(l);
    assertEq(JSON.stringify(out), JSON.stringify(["a", "b", "c"]),
        "residual final line without newline");
});

await __kitS.section("CRLF + empty lines", async () => {
    const out = [];
    for await (const l of lines(fromBytes("a\r\n\r\nb\r\n"))) out.push(l);
    assertEq(JSON.stringify(out), JSON.stringify(["a", "", "b"]), "CRLF + empty lines");
});

await __kitS.section("break after 3 lines", async () => {
    let closed = false;
    let pos = 0;
    const src = {
        async read(buf) {
            if (pos >= 64 * 1024) return 0;
            const n = Math.min(64, 64 * 1024 - pos);
            for (let i = 0; i < n; i++) buf[i] = (pos + i) % 32 === 31 ? 10 : 65;
            pos += n;
            return n;
        },
        close() { closed = true; },
    };
    let n = 0;
    for await (const l of lines(src)) { n++; if (n === 3) break; }
    assertEq(n, 3, "break after 3 lines");
    assert(closed, "return() closed the source");
});

await __kitS.section("héllo wörld", async () => {
    const s = "héllo wörld\n".repeat(50);
    const bytes = enc.encode(s);
    let i = 0;
    const src = { async read(buf) { if (i >= bytes.length) return 0; buf[0] = bytes[i++]; return 1; } };
    let ok = true, count = 0;
    for await (const l of lines(src)) { count++; if (l !== "héllo wörld") ok = false; }
    assert(ok && count === 50, "utf-8 carried across chunk boundaries");
});

await __kitS.section("auFFFD b", async () => {
    const out = [];
    for await (const l of lines(fromBytes(new Uint8Array([0x61, 0xff, 0x62, 0x0a]))))
        out.push(l);
    assertEq(out[0], "a\uFFFD b".replace(" ", ""), "invalid utf-8 replaced");
});

await __kitS.section("explicit utf-8 accepted", async () => {
    const out = [];
    for await (const l of lines(fromBytes("x\n"), { encoding: "utf-8" })) out.push(l);
    assertEq(out[0], "x", "explicit utf-8 accepted");
    let threw = false;
    try { lines(fromBytes("x\n"), { encoding: "utf-16le" }); }
    catch (e) { threw = /utf-8/.test(e.message); }
    assert(threw, "non-utf-8 encoding refused");
});

await __kitS.section("10000-line file count", async () => {
    const f = tmpPath("lines-file");
    const w = toFile(f).getWriter();
    const one = enc.encode("x".repeat(31) + "\n");
    for (let i = 0; i < 10000; i++) await w.write(one);
    await w.close();
    let count = 0, last = "";
    for await (const l of lines(fromFile(f))) { count++; last = l; }
    assertEq(count, 10000, "10000-line file count");
    assertEq(last.length, 31, "10000-line file last line");
});

await __kitS.section("read error rejects next()", async () => {
    const src = { async read(buf) { throw new Error("read-died"); } };
    const it = lines(src);
    let msg = "";
    try { await it.next(); } catch (e) { msg = e.message; }
    assertEq(msg, "read-died", "read error rejects next()");
});
__kitS.end();

removeAll(dir);
print("test_stream: all tests passed (" + n + " assertions)");
