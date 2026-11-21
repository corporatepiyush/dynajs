// timeout: 120
// covers: c:src/modules/stream/stream-lines.c
// lines()/ndjson() over a ReadableStream (bytes or string chunks) and the {batch: true} option: one C splitter serves both modes.
// Batch mode yields string[] (ndjson: unknown[]) per source read, never empty, at most 65536 per batch, so a for-await costs one promise per
// batch instead of one per line (the engine's await costs ~280 ns; a line costs a few ns in a batch). Per-line mode serves a buffered line
// with an already-resolved promise and reads the source only when no complete line is buffered. Semantics are the ByteSource path's:
// CRLF stripped, empty lines kept, unterminated last line delivered, UTF-8 decoded as before; ndjson skips blank lines and rejects a bad
// line with its 1-based number, then resumes after it; in batch mode the values before the bad line come first as their own batch.
// The fuzz compares every mode against a JS reference over random chunk splits (G7: no periodic data), and pins the ByteSource per-line
// output to a hash recorded on the pre-6d binary.
import { suite } from "../lib/kit.js";
import { lines, ndjson, fromBytes } from "dyna:stream";

const t = suite("stream_lines_batch");
const GOLDEN = 168975166;
function rng(seed) { let x = seed >>> 0; return () => { x = (Math.imul(x, 1103515245) + 12345) >>> 0; return x / 4294967296; }; }
const PIECES = [[0x61], [0x62], [0x7a], [0x20], [0xc3, 0xa9], [0xf0, 0x9f, 0x98, 0x80], [0xff], [0xe2, 0x82], [0x0d], [0x0a], [0x0a], [0x0d, 0x0a]];
function corpus(seed, n) {
    const r = rng(seed), out = [];
    while (out.length < n) {
        if (r() < 0.01) { const k = 1 + Math.floor(r() * 300000); for (let i = 0; i < k; i++) out.push(0x41 + Math.floor(r() * 26)); continue; }
        for (const b of PIECES[Math.floor(r() * PIECES.length)]) out.push(b);
    }
    return new Uint8Array(out);
}
function refLines(bytes) {
    const td = new TextDecoder(), out = [];
    let s = 0;
    const cut = (a, b) => { let e = b; if (e > a && bytes[e - 1] === 0x0d) e--; out.push(td.decode(bytes.subarray(a, e))); };
    for (let i = 0; i < bytes.length; i++) if (bytes[i] === 0x0a) { cut(s, i); s = i + 1; }
    if (s < bytes.length) cut(s, bytes.length);
    return out;
}
function splits(bytes, seed) {
    const r = rng(seed), parts = [];
    for (let o = 0; o < bytes.length;) { const k = r() < 0.1 ? 1 + Math.floor(r() * 200000) : 1 + Math.floor(r() * 300); parts.push(bytes.slice(o, o + k)); o += k; }
    return parts;
}
function rsOf(parts, asString) {
    let i = 0;
    const td = new TextDecoder();
    return new ReadableStream({ pull(c) { if (i < parts.length) c.enqueue(asString ? td.decode(parts[i++]) : parts[i++]); else c.close(); } });
}
async function collect(it, batch) {
    const out = [];
    for await (const v of it) { if (batch) { if (!Array.isArray(v) || v.length === 0) throw new Error("bad batch"); for (const x of v) out.push(x); } else out.push(v); }
    return out;
}
function fnv(strs) { let h = 0x811c9dc5; for (const s of strs) { for (let i = 0; i < s.length; i++) h = Math.imul(h ^ s.charCodeAt(i), 16777619) >>> 0; h = Math.imul(h ^ 0x2400, 16777619) >>> 0; } return h; }

t.test("fuzz: every mode equals the reference over random chunk splits (CRLF/UTF-8 straddling chunks, empty lines, long lines, no trailing newline)", async (a) => {
    for (let seed = 1; seed <= 12; seed++) {
        const bytes = corpus(seed, 20000 + seed * 5000), ref = refLines(bytes), exp = JSON.stringify(ref);
        a.eq(JSON.stringify(await collect(lines(rsOf(splits(bytes, seed))), false)), exp, "rs per-line seed " + seed);
        a.eq(JSON.stringify(await collect(lines(rsOf(splits(bytes, seed + 100)), { batch: true }), true)), exp, "rs batch seed " + seed);
        a.eq(JSON.stringify(await collect(lines(fromBytes(bytes)), false)), exp, "ByteSource per-line seed " + seed);
        a.eq(JSON.stringify(await collect(lines(fromBytes(bytes), { batch: true }), true)), exp, "ByteSource batch seed " + seed);
    }
});

t.test("golden: the ByteSource per-line output matches the pre-6d binary (hash over 12 seeds, includes WTF-8 surrogate bytes)", async (a) => {
    let h = 0;
    for (let seed = 1; seed <= 12; seed++) {
        const b = corpus(seed, 20000 + seed * 5000), x = new Uint8Array(b.length + 4);
        x.set(b); x.set([0xed, 0xa0, 0x80, 0x0a], b.length);
        h = (h * 31 + fnv(await collect(lines(fromBytes(x)), false))) >>> 0;
    }
    a.eq(h, GOLDEN);
});

t.test("string chunks (e.g. after TextDecoderStream) split the same way; a line split across string chunks joins", async (a) => {
    const bytes = corpus(77, 30000);
    a.eq(JSON.stringify(await collect(lines(rsOf(splits(bytes, 5), false).pipeThrough(new TextDecoderStream()), { batch: true }), true)), JSON.stringify(refLines(bytes)));
    a.eq(JSON.stringify(await collect(lines(rsOf([new TextEncoder().encode("ab"), new TextEncoder().encode("c\r"), new TextEncoder().encode("\nd")], true)), false)), JSON.stringify(["abc", "d"]));
    for (const batch of [false, true]) a.eq(JSON.stringify(await collect(lines(rsOf([new TextEncoder().encode("p\nq\r")]), { batch }), batch)), JSON.stringify(["p", "q"]), "trailing CR without a newline is stripped, batch=" + batch);
});

t.test("batches: one per source read, never empty, capped at 65536; chunks without a newline are joined before the next batch", async (a) => {
    const enc = new TextEncoder();
    const sizes = [];
    for await (const b of lines(rsOf([enc.encode("a\nb\n"), enc.encode("c"), enc.encode("d"), enc.encode("e\nf")]), { batch: true })) sizes.push(JSON.stringify(b));
    a.eq(sizes.join("|"), '["a","b"]|["cde"]|["f"]');
    const nl = new Uint8Array(70000).fill(10);
    const lens = [];
    for await (const b of lines(rsOf([nl]), { batch: true })) lens.push(b.length);
    a.eq(lens.join(","), "65536,4464");
});

t.test("the reader is released at the end, the stream unlocks; return() cancels the stream; a locked stream is a TypeError", async (a) => {
    const rs = rsOf([new TextEncoder().encode("x\ny\n")]);
    a.eq(JSON.stringify(await collect(lines(rs, { batch: true }), true)), '["x","y"]');
    a.eq(rs.locked, false, "released after done");
    let cancelled = null;
    const rs2 = new ReadableStream({ pull(c) { c.enqueue(new TextEncoder().encode("1\n2\n")); }, cancel(r) { cancelled = String(r); } });
    for await (const l of lines(rs2)) { if (l === "2") break; }
    a.eq(cancelled, "undefined", "break cancels the source");
    const rs3 = new ReadableStream(); rs3.getReader();
    let err = null; try { lines(rs3); } catch (e) { err = e; }
    a.eq(err && err.constructor.name, "TypeError");
    let bad = null;
    try { for await (const l of lines(new ReadableStream({ start(c) { c.enqueue(42); } }))) void l; } catch (e) { bad = e.message; }
    a.eq(bad, "lines: ReadableStream chunks must be BufferSources or strings");
    let ser = null;
    try { for await (const l of lines(new ReadableStream({ start(c) { c.error(new RangeError("boom")); } }))) void l; } catch (e) { ser = e.message; }
    a.eq(ser, "boom", "a stream error rejects next()");
});

t.test("next() while a read is pending is a synchronous TypeError in both modes", async (a) => {
    for (const batch of [false, true]) {
        let ctl;
        const it = lines(new ReadableStream({ start(c) { ctl = c; } }), { batch });
        const p = it.next();
        let err = null; try { it.next(); } catch (e) { err = e.constructor.name; }
        a.eq(err, "TypeError");
        ctl.enqueue(new TextEncoder().encode("q\n")); ctl.close();
        a.eq(JSON.stringify((await p).value), batch ? '["q"]' : '"q"');
    }
});

async function events(it, batch) {
    const ev = [];
    for (;;) {
        let r;
        try { r = await it.next(); } catch (e) { ev.push("E:" + e.message); continue; }
        if (r.done) return ev;
        if (batch) { if (r.value.length === 0) ev.push("EMPTY"); for (const v of r.value) ev.push(JSON.stringify(v)); } else ev.push(JSON.stringify(r.value));
    }
}
t.test("ndjson: errors carry their line number and iteration resumes; in batch mode the values before a bad line arrive first, then the error", async (a) => {
    const text = '{"a":1}\n\n  \t\n[2]\nbad1\n3\n{"x":\n"s"\r\nbad2';
    const exp = ['{"a":1}', "[2]", "E:ndjson: line 5: unexpected token: 'bad1'", "3", "E:ndjson: line 7: Unexpected end of JSON input", '"s"', "E:ndjson: line 9: unexpected token: 'bad2'"];
    const enc = new TextEncoder().encode(text);
    for (const batch of [false, true]) {
        a.eq(JSON.stringify(await events(ndjson(fromBytes(enc), { batch }), batch)), JSON.stringify(exp), "ByteSource batch=" + batch);
        a.eq(JSON.stringify(await events(ndjson(rsOf(splits(enc, 9)), { batch }), batch)), JSON.stringify(exp), "rs batch=" + batch);
        a.eq(JSON.stringify(await events(ndjson(rsOf([enc]), { batch }), batch)), JSON.stringify(exp), "one chunk batch=" + batch);
    }
    const first = await (async () => { const it = ndjson(rsOf([new TextEncoder().encode("x\n1\n")]), { batch: true }); return [await it.next().catch((e) => "E:" + e.message), JSON.stringify((await it.next()).value)]; })();
    a.eq(JSON.stringify(first), JSON.stringify(["E:ndjson: line 1: unexpected token: 'x'", "[1]"]), "bad line first in a chunk rejects at once");
});

t.test("buffer compaction with a partial line pending (>= half the buffer consumed) keeps the resumable scan offset in step", async (a) => {
    const enc = new TextEncoder(), head = "ab\n".repeat(66666) + "xyz", tail = "w\nq\n";
    const exp = JSON.stringify([...Array(66666).fill("ab"), "xyzw", "q"]);
    for (const batch of [false, true]) {
        a.eq(JSON.stringify(await collect(lines(rsOf([enc.encode(head), enc.encode(tail)]), { batch }), batch)), exp, "rs batch=" + batch);
        a.eq(JSON.stringify(await collect(lines(fromBytes(enc.encode(head + tail)), { batch }), batch)), exp, "ByteSource batch=" + batch);
    }
});

await t.run();
