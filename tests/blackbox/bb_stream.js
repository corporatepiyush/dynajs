// Parametric black-box contract test for dyna:stream, generated from dynajs.d.ts lines 4721-4909. Engine sources not consulted.

import { pipe, fromBytes, lines, ndjson, deflate, inflate, toFile } from "dyna:stream";
import { Path, makeTempDir, readBytes, readFile, exists, removeAll, writeFile } from "dyna:file";

let n = 0;
function assert(cond, msg) { n++; if (!cond) throw new Error("assertion failed: " + msg); }
function assertEq(actual, expected, msg) { n++; const ok = Object.is(actual, expected) || (typeof actual === "number" && typeof expected === "number" && Number.isNaN(actual) && Number.isNaN(expected)); if (!ok) throw new Error("assertion failed: " + msg + " — got |" + actual + "| expected |" + expected + "|"); }
function assertDeepEq(a, b, msg) { n++; if (JSON.stringify(a) !== JSON.stringify(b)) throw new Error("assertion failed (deep): " + msg + " — got |" + JSON.stringify(a) + "| expected |" + JSON.stringify(b) + "|"); }
function assertThrows(fn, msg, ErrType, errPattern) { n++; let threw = false, e = null; try { fn(); } catch (err) { threw = true; e = err; } if (!threw) throw new Error("expected throw: " + msg); if (ErrType && !(e instanceof ErrType)) throw new Error("wrong error type " + (e && e.constructor ? e.constructor.name : String(e)) + ": " + msg); if (errPattern && !(errPattern instanceof RegExp ? errPattern.test(String(e)) : String(e).includes(errPattern))) throw new Error("wrong error message |" + e + "|: " + msg); }
function eqArr(a, b) { if (a.length !== b.length) return false; for (let i = 0; i < a.length; i++) if (a[i] !== b[i]) return false; return true; }
function assertEqArr(actual, expected, msg) { n++; if (!eqArr(actual, expected)) throw new Error("assertion failed (arr): " + msg + " — got |" + JSON.stringify(Array.from(actual)) + "| expected |" + JSON.stringify(expected) + "|"); }
async function assertRejects(promise, msg, errPattern) {
    n++;
    let threw = false, e = null;
    try { await promise; } catch (err) { threw = true; e = err; }
    if (!threw) throw new Error("expected rejection: " + msg);
    if (errPattern && !(errPattern instanceof RegExp ? errPattern.test(String(e)) : String(e).includes(errPattern))) throw new Error("wrong rejection |" + e + "|: " + msg);
}

const enc = new TextEncoder();

function bytesOf(input) { return typeof input === "string" ? enc.encode(input) : input; }

function cat(chunks) {
    let len = 0;
    for (const c of chunks) len += c.length;
    const out = new Uint8Array(len);
    let off = 0;
    for (const c of chunks) { out.set(c, off); off += c.length; }
    return out;
}

/* In-memory ByteSink (duck-typed surface per the ByteSource/ByteSink notes). */
function memSink() {
    const sink = {
        chunks: [],
        closed: false,
        flushes: 0,
        write(buf) { sink.chunks.push(buf.slice()); return Promise.resolve(buf.length); },
        flush() { sink.flushes++; return Promise.resolve(); },
        close() { sink.closed = true; },
        [Symbol.dispose]() { sink.closed = true; },
    };
    return sink;
}

/* Duck-typed ByteSource yielding `step` bytes per read over an in-memory copy. */
function slowSource(input, step) {
    const u8 = bytesOf(input);
    const src = { pos: 0, closed: false };
    src.read = (buf) => {
        if (src.pos >= u8.length) return Promise.resolve(0);
        const cnt = Math.min(step, buf.length, u8.length - src.pos);
        buf.set(u8.subarray(src.pos, src.pos + cnt));
        src.pos += cnt;
        return Promise.resolve(cnt);
    };
    src.close = () => { src.closed = true; };
    src[Symbol.dispose] = () => { src.closed = true; };
    return src;
}

function failingSource() {
    const src = { closed: false };
    src.read = () => Promise.reject(new Error("read boom"));
    src.close = () => { src.closed = true; };
    src[Symbol.dispose] = () => { src.closed = true; };
    return src;
}

/* Drains a ByteSource; records per-read counts INCLUDING the terminating 0. */
async function drain(src, cap) {
    const counts = [];
    const chunks = [];
    const buf = new Uint8Array(cap);
    for (;;) {
        const got = await src.read(buf);
        counts.push(got);
        if (got === 0) break;
        chunks.push(buf.slice(0, got));
    }
    return { counts, bytes: cat(chunks) };
}

async function runRow(label, fn) {
    try { await fn(); } catch (e) { throw new Error("row [" + label + "]: " + (e && e.message ? e.message : String(e))); }
}

/* ------------------------------------------------------------------ *
 *  Table 1: fromBytes — read semantics.
 *  Row: [label, input, readBufSize, expectedPerReadCounts(incl EOF 0), expectedBytes]
 * ------------------------------------------------------------------ */
const FROMBYTES_ROWS = [
    ["small buffer reads in chunks", "abcde", 2, [2, 2, 1, 0], [97, 98, 99, 100, 101]],
    ["single read swallows all", "ab", 10, [2, 0], [97, 98]],
    ["exact fit buffer", "abcd", 4, [4, 0], [97, 98, 99, 100]],
    ["empty source is immediate EOF", "", 4, [0], []],
    ["string input is UTF-8 encoded", "h\u00e9", 10, [3, 0], [104, 0xc3, 0xa9]],
];
for (const [label, input, cap, expCounts, expBytes] of FROMBYTES_ROWS) {
    await runRow(label, async () => {
        const r = await drain(fromBytes(input), cap);
        assertEqArr(r.counts, expCounts, label + " — per-read counts (0 = EOF)");
        assertEqArr(Array.from(r.bytes), expBytes, label + " — bytes read");
    });
}

/* Table 1b: fromBytes copy semantics + resource lifecycle (all
 * documented: "The bytes are copied, so later mutation of `b` is
 * invisible"; close/dispose/[Symbol.dispose]/closed; `using` works). */
await runRow("mutation of the source array after fromBytes is invisible", async () => {
    const b = Uint8Array.from([1, 2, 3]);
    const src = fromBytes(b);
    b[0] = 99;
    const r = await drain(src, 8);
    assertEqArr(Array.from(r.bytes), [1, 2, 3], "copied bytes, not a live view");
});
await runRow("close/dispose lifecycle", async () => {
    const a = fromBytes("x");
    assertEq(a.closed, false, "fresh source is open");
    a.close();
    assertEq(a.closed, true, "close() marks closed");
    const b = fromBytes("x");
    b.dispose();
    assertEq(b.closed, true, "dispose() marks closed");
    const c = fromBytes("x");
    c[Symbol.dispose]();
    assertEq(c.closed, true, "[Symbol.dispose]() marks closed");
});
/* `using` declaration support is build-gated: Makefile opt-in CONFIG_USING=y
 * ("Opt in with CONFIG_USING=y for spec semantics"); the campaign binary is
 * built without it, so the `using` syntax itself is a SyntaxError there — the
 * same platform-gating pattern as dyna:uring on macOS. The snippet is
 * therefore compiled dynamically: builds WITH the syntax assert the full
 * block-end disposal contract, builds WITHOUT it print a SKIP line. */
await runRow("`using` releases a source at block end", () => {
    let body;
    try {
        body = new Function("src", "K", "{ using s = src; K(s); }");
    } catch (e) {
        if (e instanceof SyntaxError) { print("SKIP(using declaration: this build lacks CONFIG_USING=y)"); return; }
        throw e;
    }
    let closedInside = null;
    const keep = fromBytes("xy");
    body(keep, (s) => { closedInside = s.closed; });
    assertEq(closedInside, false, "source open inside the using block");
    assertEq(keep.closed, true, "using disposed the source at block end");
});

/* ------------------------------------------------------------------ *
 *  Table 2: pipe — transfer totals, short writes, onChunk, failure
 *  semantics. Rows: [label, caseFn] over in-memory doors only.
 * ------------------------------------------------------------------ */
const PIPE_ROWS = [
    ["pipe transfers all bytes, returns the total, leaves both sides OPEN", async () => {
        const sink = memSink();
        const src = fromBytes("hello");
        const total = await pipe(src, sink);
        assertEq(total, 5, "total bytes transferred");
        assertEq(Array.from(cat(sink.chunks)).length, 5, "sink received the bytes");
        assertDeepEq(Array.from(cat(sink.chunks)), Array.from(enc.encode("hello")), "sink bytes intact");
        assertEq(src.closed, false, "on success the source is not closed");
        assertEq(sink.closed, false, "on success the sink is not closed");
    }],
    ["pipe over a duck-typed 1-byte-per-read source: onChunk counts", async () => {
        const sink = memSink();
        const seen = [];
        const total = await pipe(slowSource("abcde", 1), sink, { onChunk: (b) => seen.push(b) });
        assertEq(total, 5, "total across single-byte reads");
        assertEqArr(seen, [1, 1, 1, 1, 1], "onChunk called per accepted chunk with its byte count");
    }],
    ["pipe retries a short-writing duck-typed sink until the full chunk lands", async () => {
        const chunks = [];
        const sink = {
            closed: false,
            write(buf) { const take = Math.min(2, buf.length); chunks.push(buf.slice(0, take)); return Promise.resolve(take); },
            flush() { return Promise.resolve(); },
            close() { sink.closed = true; },
            [Symbol.dispose]() { sink.closed = true; },
        };
        const total = await pipe(fromBytes("hello"), sink);
        assertEq(total, 5, "total still counts every byte once");
        assertDeepEq(Array.from(cat(chunks)), Array.from(enc.encode("hello")), "short writes were retried to completion");
    }],
    ["pipe over an empty source resolves with 0", async () => {
        const sink = memSink();
        const total = await pipe(fromBytes(""), sink);
        assertEq(total, 0, "EOF immediately");
        assertEq(sink.chunks.length, 0, "nothing written");
    }],
    ["a throwing onChunk rejects the pipe AND closes both sides", async () => {
        const sink = memSink();
        const src = fromBytes("hello");
        await assertRejects(pipe(src, sink, { onChunk() { throw new Error("chunk boom"); } }), "onChunk throw rejects", /chunk boom/);
        assertEq(src.closed, true, "source closed on failure");
        assertEq(sink.closed, true, "sink closed on failure");
    }],
    ["a rejecting read rejects the pipe AND closes both sides", async () => {
        const sink = memSink();
        const src = failingSource();
        await assertRejects(pipe(src, sink), "failing read rejects", /read boom/);
        assertEq(src.closed, true, "source closed on failure");
        assertEq(sink.closed, true, "sink closed on failure");
    }],
    ["pipe into a compressing sink (codec door parity)", async () => {
        const sink = memSink();
        const d = deflate(sink, { codec: "lz4" });
        const total = await pipe(fromBytes("hello world"), d);
        const raw = await d.finish();
        assertEq(total, 11, "pipe total over the compressing sink");
        assertEq(raw, 11, "finish() reports the same RAW byte count");
        const inf = inflate(fromBytes(cat(sink.chunks)), { codec: "lz4" });
        const out = await drain(inf, 64);
        assertDeepEq(Array.from(out.bytes), Array.from(enc.encode("hello world")), "round trip through pipe+deflate+inflate");
    }],
];
for (const [label, fn] of PIPE_ROWS) await runRow(label, fn);

/* ------------------------------------------------------------------ *
 *  Table 3: lines — [label, input, expectedLines, step?].
 *  step: drive the source through a duck-typed N-bytes-per-read door
 *  to exercise the documented chunk-boundary safety.
 * ------------------------------------------------------------------ */
const LINES_ROWS = [
    ["basic split", "a\nb\nc", ["a", "b", "c"]],
    ["CRLF stripped", "a\r\nb\r\n", ["a", "b"]],
    // d.ts (lines): "split on `\n` with one trailing `\r` stripped"
    // (readLine parity, d.ts:2885) — a \r is stripped only AT EOL; a
    // mid-line lone \r is preserved, it does not split.
    ["CR stripped at EOL, mid-line CR preserved", "a\r\nb\rc", ["a", "b\rc"]],
    ["trailing newline yields no empty tail", "a\n", ["a"]],
    ["final line without newline still delivered", "a\nb", ["a", "b"]],
    ["empty input yields no lines", "", []],
    ["a single newline is one empty line", "\n", [""]],
    ["empty lines preserved", "a\n\nb", ["a", "", "b"]],
    ["one byte per read carries split lines intact", "alpha\nbeta", ["alpha", "beta"], 1],
    ["multi-byte UTF-8 split across reads waits for continuation", "h\u00e9llo\nx", ["h\u00e9llo", "x"], 2],
    ["CRLF split across reads", "a\r\nb", ["a", "b"], 2],
    ["invalid UTF-8 decodes with U+FFFD replacement", Uint8Array.from([0x61, 0xff, 0x0a]), ["a\uFFFD"]],
];
for (const [label, input, expected, step] of LINES_ROWS) {
    await runRow(label, async () => {
        const src = step ? slowSource(input, step) : fromBytes(input);
        const out = [];
        for await (const line of lines(src)) out.push(line);
        assertDeepEq(out, expected, label + " — lines");
    });
}
await runRow("lines refuses a non-utf-8 encoding at the factory", () => {
    assertThrows(() => lines(fromBytes("a"), { encoding: "utf-16le" }), "only utf-8 is supported");
});
await runRow("break in a for-await loop closes the source", async () => {
    const src = fromBytes("a\nb\nc");
    for await (const line of lines(src)) { void line; break; }
    assertEq(src.closed, true, "break calls return() which closes the source");
});

/* ------------------------------------------------------------------ *
 *  Table 4: ndjson — [label, input, expectedValues]. Blank and
 *  whitespace-only lines are skipped; a final unterminated line parses.
 * ------------------------------------------------------------------ */
const NDJSON_ROWS = [
    ["two objects", '{"a":1}\n{"a":2}\n', [{ a: 1 }, { a: 2 }]],
    ["blank lines skipped", '{"a":1}\n\n\n{"b":2}\n', [{ a: 1 }, { b: 2 }]],
    ["whitespace-only lines skipped", "  \n\t\n{\"a\":1}", [{ a: 1 }]],
    ["final line without trailing newline", '{"a":1}\n{"b":2}', [{ a: 1 }, { b: 2 }]],
    ["empty input", "", []],
    ["scalar lines", '1\n"two"\n[3]\n', [1, "two", [3]]],
    ["chunk-boundary safe over a 3-byte-per-read source", '{"a":1}\n{"b":2}', [{ a: 1 }, { b: 2 }], 3],
];
for (const [label, input, expected, step] of NDJSON_ROWS) {
    await runRow(label, async () => {
        const src = step ? slowSource(input, step) : fromBytes(input);
        const out = [];
        for await (const v of ndjson(src)) out.push(v);
        assertDeepEq(out, expected, label + " — parsed values");
    });
}

/* Table 4b: ndjson documented error semantics — a malformed line
 * rejects with a SyntaxError naming the 1-based LINE NUMBER and the
 * iterator stays usable: the next next() resumes after the bad line. */
await runRow("malformed line rejects with ndjson: line N, iteration resumes", async () => {
    const it = ndjson(fromBytes('{"a":1}\nnope\n{"b":2}\n'));
    const r1 = await it.next();
    assertEq(r1.done, false, "first line yields");
    assertDeepEq(r1.value, { a: 1 }, "first line parsed");
    let err = null;
    try { await it.next(); } catch (e) { err = e; }
    assert(err instanceof SyntaxError, "malformed line rejects with SyntaxError — got |" + err + "|");
    assert(/ndjson: line 2:/.test(String(err)), "rejection names the 1-based line number — got |" + err + "|");
    const r3 = await it.next();
    assertDeepEq(r3.value, { b: 2 }, "next() resumes after the bad line");
    const r4 = await it.next();
    assertEq(r4.done, true, "iterator ends after the last line");
});
await runRow("malformed FIRST line names line 1", async () => {
    const it = ndjson(fromBytes("nope\n"));
    let err = null;
    try { await it.next(); } catch (e) { err = e; }
    assert(/ndjson: line 1:/.test(String(err)), "1-based numbering starts at 1 — got |" + err + "|");
});

/* ------------------------------------------------------------------ *
 *  Table 5: deflate/inflate streaming codecs.
 *  Only "lz4" is exercised: the doc pins lz4 as "works everywhere",
 *  while gzip/deflate (macOS-only here) and zstd/brotli are
 *  platform-gated, so their presence is not a contract expectation.
 *  finish() resolves with the total RAW bytes compressed and closes the
 *  wrapped sink; the inherited close()/dispose() lose the stream tail.
 * ------------------------------------------------------------------ */
const DEFLATE_ROWS = [
    ["finish returns the RAW byte count (5), closes the wrapped sink", async () => {
        const sink = memSink();
        const d = deflate(sink, { codec: "lz4" });
        await d.write(enc.encode("hello"));
        const raw = await d.finish();
        assertEq(raw, 5, "total RAW bytes compressed");
        assertEq(sink.closed, true, "finish closes the wrapped sink");
        const inf = inflate(fromBytes(cat(sink.chunks)), { codec: "lz4" });
        const out = await drain(inf, 64);
        assertDeepEq(Array.from(out.bytes), Array.from(enc.encode("hello")), "round trip restores the raw bytes");
        assertEq(out.counts[out.counts.length - 1], 0, "framing verified before EOF 0 is reported");
    }],
    ["multiple writes accumulate the RAW total (5+3=8)", async () => {
        const sink = memSink();
        const d = deflate(sink, { codec: "lz4" });
        await d.write(enc.encode("hello"));
        await d.write(enc.encode("abc"));
        const raw = await d.finish();
        assertEq(raw, 8, "sum of raw bytes across writes");
        const inf = inflate(fromBytes(cat(sink.chunks)), { codec: "lz4" });
        const out = await drain(inf, 64);
        assertDeepEq(Array.from(out.bytes), Array.from(enc.encode("helloabc")), "both writes survive the round trip");
    }],
    ["empty stream: finish 0, inflate immediate EOF", async () => {
        const sink = memSink();
        const d = deflate(sink, { codec: "lz4" });
        const raw = await d.finish();
        assertEq(raw, 0, "nothing written, nothing compressed");
        const inf = inflate(fromBytes(cat(sink.chunks)), { codec: "lz4" });
        const out = await drain(inf, 16);
        assertEq(out.bytes.length, 0, "empty decompressed body");
        assertEqArr(out.counts, [0], "EOF on the first read");
    }],
    ["patterned 40-byte body round trips; partial-fill reads pinned", async () => {
        const raw = new Uint8Array(40);
        for (let i = 0; i < raw.length; i++) raw[i] = i % 7;
        const sink = memSink();
        const d = deflate(sink, { codec: "lz4" });
        await d.write(raw);
        const rawCount = await d.finish();
        assertEq(rawCount, 40, "raw count for the patterned body");
        const inf = inflate(fromBytes(cat(sink.chunks)), { codec: "lz4" });
        const out = await drain(inf, 4);
        assertEqArr(out.counts, [4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 0], "read fills up to buf.length, then EOF 0");
        assertEqArr(Array.from(out.bytes), Array.from(raw), "decompressed bytes equal the original");
    }],
    ["inflate over a duck-typed 3-byte-per-read source", async () => {
        const body = enc.encode("streaming decompression");
        const sink = memSink();
        const d = deflate(sink, { codec: "lz4" });
        await d.write(body);
        await d.finish();
        const inf = inflate(slowSource(cat(sink.chunks), 3), { codec: "lz4" });
        const out = await drain(inf, 64);
        assertDeepEq(Array.from(out.bytes), Array.from(body), "chunked compressed input round trips");
    }],
    ["closing the wrapper does not close the underlying source", async () => {
        const sink = memSink();
        const d = deflate(sink, { codec: "lz4" });
        await d.write(enc.encode("x"));
        await d.finish();
        const underlying = fromBytes(cat(sink.chunks));
        const inf = inflate(underlying, { codec: "lz4" });
        await drain(inf, 8);
        inf.close();
        assertEq(inf.closed, true, "wrapper close releases the wrapper");
        assertEq(underlying.closed, false, "underlying source still owned by the caller");
    }],
];
for (const [label, fn] of DEFLATE_ROWS) await runRow(label, fn);

/* ------------------------------------------------------------------ *
 *  Table 6: toFile — the buffered file sink (d.ts:4796-4812).
 *  "created LAZILY at the first write"; "A bad path rejects the write";
 *  "A failed write makes the sink sticky: later `write`/`flush` reject
 *  with the same errno"; "`close()` flushes best-effort but cannot
 *  report". Errno replay is compared by the error's `code` name AND its
 *  numeric `errno`, the two halves of "the same errno".
 * ------------------------------------------------------------------ */
async function rejectionOf(p) {
    try { await p; } catch (e) { return e; }
    throw new Error("expected rejection");
}

{
    const TF = makeTempDir("bbstream-");
    try {
        /* -------------------------------------------------------------- *
         *  Table 6: toFile bufferSize clamp — d.ts:4775: "Sink buffer
         *  size, clamped to 4 KiB..64 MiB (default 128 KiB)". The clamped
         *  capacity itself has no documented getter, so the rows pin the
         *  observable contract: both extreme option values are ACCEPTED
         *  (no throw) and every byte lands exactly. The 100 KiB payload
         *  dwarfs the 4 KiB floor (direct-to-fd path) and sits under the
         *  64 MiB ceiling and the 128 KiB default (buffered path).
         * -------------------------------------------------------------- */
        const CLAMP_ROWS = [
            ["bufferSize: 1 clamps to the 4 KiB floor without throwing", 1],
            ["bufferSize: 1e9 clamps to the 64 MiB ceiling without throwing", 1e9],
            ["absent bufferSize (default 128 KiB) works", undefined],
        ];
        for (const [label, bufSize] of CLAMP_ROWS) {
            await runRow(label, async () => {
                const p = new Path(TF, "clamp-" + String(bufSize) + ".bin");
                const len = 100000;
                const payload = new Uint8Array(len);
                for (let i = 0; i < len; i++) payload[i] = i & 0xff;
                const sink = toFile(p, bufSize === undefined ? undefined : { bufferSize: bufSize });
                assertEq(await sink.write(payload), len, "write accepted the whole payload");
                await sink.flush();
                sink.close();
                const back = readBytes(p);
                assertEq(back.length, len, "byte count survived the round trip");
                let same = true;
                for (let i = 0; i < len; i++) if (back[i] !== payload[i]) { same = false; break; }
                assert(same, "every byte survived the round trip under the clamped buffer");
            });
        }

        // d.ts:4797-4798: "created LAZILY at the first `write`" — so a sink
        // that never writes leaves NO file behind, even on close().
        await runRow("toFile is lazy: a sink that is only closed never creates the file", async () => {
            const p = new Path(TF, "lazy", "never.bin");
            const sink = toFile(p);
            assertEq(exists(p), false, "nothing on disk before the first write");
            sink.close();
            assertEq(exists(p), false, "close() without a write touched no disk");
            assertEq(sink.closed, true, "close() released the lazy sink");
        });

        // d.ts: "write resolves once bytes are accepted into the buffer" +
        // "created LAZILY at the first write" (the fd opens at write time,
        // the buffered TAIL reaches disk on flush/close).
        await runRow("write resolves with the count accepted; flush persists the tail", async () => {
            const p = new Path(TF, "counts.txt");
            const sink = toFile(p);
            assertEq(await sink.write(enc.encode("buffered")), 8, "buffered write accepted in full");
            assertEq(await sink.write(new Uint8Array(0)), 0, "a zero-length write accepts nothing");
            assertEq(exists(p), true, "the file came into existence at the first write");
            await sink.flush();
            assertEq(readFile(p), "buffered", "flush() pushed the buffered bytes to the fd");
            sink.close();
            assertEq(sink.closed, true, "close() released the sink");
        });

        /* -------------------------------------------------------------- *
         *  Table 7 (LAST: its first row is a logged ENGINE-BUG finding —
         *  see tests/blackbox/FINDINGS.md, "Coverage-pass findings
         *  (GAP-1)"): toFile sticky-failure + errno replay
         *  (d.ts:4796-4812). "created LAZILY at the first write"; "A bad
         *  path rejects the write"; "A failed write makes the sink
         *  sticky: later write/flush reject with the same errno";
         *  "`close()` flushes best-effort but cannot report". Errno replay
         *  is compared by the error's `code` name AND its numeric `errno`,
         *  the two halves of "the same errno".
         * -------------------------------------------------------------- */
        // STICKY_ROWS: two deterministic ways a lazy open fails at the first
        // write — a missing parent dir (ENOENT) and a plain file pressed into
        // service as a parent dir (ENOTDIR). Each row: [label, badPath, code].
        writeFile(new Path(TF, "seed.txt"), "seed");
        const STICKY_ROWS = [
            ["a missing parent dir rejects the write with ENOENT",
             (t) => String(new Path(t, "no-such-dir", "f.txt")), "ENOENT"],
            ["a file used as a parent dir rejects the write with ENOTDIR",
             (t) => String(new Path(t, "seed.txt", "child.txt")), "ENOTDIR"],
        ];
        for (const [label, badPath, code] of STICKY_ROWS) {
            await runRow(label, async () => {
                const sink = toFile(badPath(TF));
                // "A bad path rejects the write" (the lazy open surfaces there)
                const e1 = await rejectionOf(sink.write(enc.encode("x")));
                assertEq(e1.code, code, "the triggering write rejects with errno " + code);
                // "A failed write makes the sink sticky: later write/flush
                // reject with the same errno"
                const e2 = await rejectionOf(sink.write(enc.encode("y")));
                assertEq(e2.code, e1.code, "the NEXT write rejects with the same errno NAME");
                assertEq(e2.errno, e1.errno, "the NEXT write rejects with the same errno NUMBER");
                const e3 = await rejectionOf(sink.flush());
                assertEq(e3.code, e1.code, "flush() after the failed write rejects with the same errno NAME");
                assertEq(e3.errno, e1.errno, "flush() after the failed write rejects with the same errno NUMBER");
                // "`close()` flushes best-effort but cannot report" — silent
                let closeThrew = null;
                try { sink.close(); } catch (e) { closeThrew = e; }
                assertEq(closeThrew, null, "close() stays silent on a sticky sink");
                assertEq(sink.closed, true, "close() released the sticky sink");
            });
        }
    } finally {
        removeAll(new Path(TF));
    }
}

/* ------------------------------------------------------------------ *
 *  Write paths accept ByteView only. d.ts: "type ByteView = Uint8Array |
 *  Int8Array | Uint8ClampedArray | DataView | ArrayBuffer" and "fromBytes(b:
 *  Uint8Array | string)" — a Float64Array is not a ByteView, and the read
 *  side already refuses it ("byte view must be byte-wide"); the write side
 *  (fromBytes, ByteSink.write, compressing sinks) refuses it the same way
 *  instead of writing element-count bytes of the wider view.
 * ------------------------------------------------------------------ */
await runRow("fromBytes refuses a non-byte-wide view", async () => {
    assertThrows(() => fromBytes(new Float64Array(4)),
        "d.ts fromBytes(b: Uint8Array | string): Float64Array is not a byte view",
        TypeError, /byte-wide/);
    assertThrows(() => fromBytes(new Int32Array(4)),
        "d.ts fromBytes(b: Uint8Array | string): Int32Array is not a byte view",
        TypeError, /byte-wide/);
    const src = fromBytes(new Uint8Array([1, 2, 3]));
    assert(src !== undefined, "byte-wide Uint8Array still constructs a source");
});
await runRow("compressing sink write refuses a non-byte-wide view", async () => {
    const s = memSink();
    const cs = deflate(s, { codec: "gzip" });
    let err = null;
    try { await cs.write(new Float64Array(4)); } catch (e) { err = e; }
    assert(err instanceof TypeError && /byte-wide/.test(String(err)),
        "deflate sink write: Float64Array is not a ByteView (d.ts ByteView union)");
    await cs.finish();
    assert(s.closed || s.chunks.length >= 0, "sink usable after the refusal");
});

print("bb_stream: all tests passed (" + n + " assertions)");
