// covers: blackbox dyna:file.* dyna:stream.* -- black-box contract suite: the public surface exercised end to end, as a user calls it
// Parametric black-box contract test for dyna:stream, generated from the dynajs.d.ts dyna:stream section. Engine sources not consulted.
// v10b-19: dyna:stream became sugar over web streams (fromBytes/fromFile -> ReadableStream, toFile -> WritableStream,
// pipe/inflate/deflate replaced by pipeTo and CompressionStream/DecompressionStream). Rows that pinned the removed
// ByteSource read counts, pipe totals/onChunk, finish() raw counts, flush() and the `using` disposal are gone with
// the API; every duck side is closed exactly once on a failed pipe (the C pipe engine no longer closes it a second time).

import { fromBytes, lines, ndjson, toFile, toReadableStream, toWritableStream } from "dyna:stream";
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

/* In-memory duck sink for toWritableStream (duck-typed surface per the ByteSource/ByteSink notes). */
function memSink() {
    const sink = {
        chunks: [],
        closed: 0,
        write(buf) { sink.chunks.push(buf.slice()); return Promise.resolve(buf.length); },
        close() { sink.closed++; },
    };
    return sink;
}

/* Duck-typed read(buf) source for toReadableStream/lines, yielding `step` bytes per read over an in-memory copy. */
function slowSource(input, step) {
    const u8 = bytesOf(input);
    const src = { pos: 0, closed: 0 };
    src.read = (buf) => {
        if (src.pos >= u8.length) return Promise.resolve(0);
        const cnt = Math.min(step, buf.length, u8.length - src.pos);
        buf.set(u8.subarray(src.pos, src.pos + cnt));
        src.pos += cnt;
        return Promise.resolve(cnt);
    };
    src.close = () => { src.closed++; };
    return src;
}

function failingSource() {
    const src = { closed: 0 };
    src.read = () => Promise.reject(new Error("read boom"));
    src.close = () => { src.closed++; };
    return src;
}

/* Drains a ReadableStream into one Uint8Array, recording each chunk length. */
async function drain(rs) {
    const counts = [];
    const chunks = [];
    const rd = rs.getReader();
    for (;;) {
        const { done, value } = await rd.read();
        if (done) break;
        const u8 = value instanceof Uint8Array ? value : new Uint8Array(value.buffer ?? value, value.byteOffset ?? 0, value.byteLength);
        counts.push(u8.length);
        chunks.push(u8.slice());
    }
    rd.releaseLock();
    return { counts, bytes: cat(chunks) };
}

async function compress(fmt, parts) {
    const cs = new CompressionStream(fmt);
    const w = cs.writable.getWriter();
    const out = drain(cs.readable);
    for (const p of parts) await w.write(p);
    await w.close();
    return (await out).bytes;
}

async function runRow(label, fn) {
    try { await fn(); } catch (e) { throw new Error("row [" + label + "]: " + (e && e.message ? e.message : String(e))); }
}

/* ------------------------------------------------------------------ *
 *  Table 1: fromBytes -- a ReadableStream of the copied bytes.
 *  Row: [label, input, expectedBytes]
 * ------------------------------------------------------------------ */
const FROMBYTES_ROWS = [
    ["ascii bytes", "abcde", [97, 98, 99, 100, 101]],
    ["empty source closes with no chunk", "", []],
    ["string input is UTF-8 encoded", "hé", [104, 0xc3, 0xa9]],
    ["Uint8Array input", Uint8Array.from([0, 255, 7]), [0, 255, 7]],
];
for (const [label, input, expBytes] of FROMBYTES_ROWS) {
    await runRow(label, async () => {
        const rs = fromBytes(input);
        assert(rs instanceof ReadableStream, label + " -- fromBytes returns a ReadableStream");
        const r = await drain(rs);
        assertEqArr(Array.from(r.bytes), expBytes, label + " -- bytes read");
        assert(r.counts.every((c) => c > 0), label + " -- no empty chunk is enqueued");
    });
}
await runRow("mutation of the source array after fromBytes is invisible", async () => {
    const b = Uint8Array.from([1, 2, 3]);
    const rs = fromBytes(b);
    b[0] = 99;
    const r = await drain(rs);
    assertEqArr(Array.from(r.bytes), [1, 2, 3], "copied bytes, not a live view");
});
await runRow("cancel() closes the stream; a second reader sees done", async () => {
    const rs = fromBytes("xyz");
    await rs.cancel();
    const r = await drain(rs);
    assertEq(r.bytes.length, 0, "a cancelled fromBytes stream yields nothing");
});

/* ------------------------------------------------------------------ *
 *  Table 2: pipeTo over toReadableStream/toWritableStream duck sides.
 * ------------------------------------------------------------------ */
const PIPE_ROWS = [
    ["pipeTo transfers all bytes and closes the duck sink once", async () => {
        const sink = memSink();
        await fromBytes("hello").pipeTo(toWritableStream(sink));
        assertDeepEq(Array.from(cat(sink.chunks)), Array.from(enc.encode("hello")), "sink bytes intact");
        assertEq(sink.closed, 1, "pipeTo closed the sink exactly once");
    }],
    ["preventClose leaves the duck sink open", async () => {
        const sink = memSink();
        await fromBytes("hi").pipeTo(toWritableStream(sink), { preventClose: true });
        assertEq(sink.closed, 0, "sink not closed under preventClose");
    }],
    ["a 1-byte-per-read duck source pipes intact and is closed once at EOF", async () => {
        const sink = memSink();
        const src = slowSource("abcde", 1);
        await toReadableStream(src).pipeTo(toWritableStream(sink));
        assertDeepEq(Array.from(cat(sink.chunks)), Array.from(enc.encode("abcde")), "every byte delivered");
        assertEq(src.closed, 1, "source closed exactly once at EOF");
    }],
    ["a short-writing duck sink is retried until the full chunk lands", async () => {
        const chunks = [];
        const sink = { write(buf) { const take = Math.min(2, buf.length); chunks.push(buf.slice(0, take)); return Promise.resolve(take); }, close() {} };
        await fromBytes("hello").pipeTo(toWritableStream(sink));
        assertDeepEq(Array.from(cat(chunks)), Array.from(enc.encode("hello")), "short writes were retried to completion");
    }],
    ["an empty source writes nothing", async () => {
        const sink = memSink();
        await fromBytes("").pipeTo(toWritableStream(sink));
        assertEq(sink.chunks.length, 0, "nothing written");
        assertEq(sink.closed, 1, "sink still closed at EOF");
    }],
    ["a rejecting read rejects pipeTo and closes each duck side exactly once", async () => {
        const sink = memSink();
        const src = failingSource();
        await assertRejects(toReadableStream(src).pipeTo(toWritableStream(sink)), "failing read rejects", /read boom/);
        assertEq(src.closed, 1, "source closed once on failure");
        assertEq(sink.closed, 1, "sink closed once on failure");
    }],
    ["pipeThrough a CompressionStream into a duck sink round trips", async () => {
        const sink = memSink();
        await fromBytes("hello world").pipeThrough(new CompressionStream("lz4")).pipeTo(toWritableStream(sink));
        const out = await drain(fromBytes(cat(sink.chunks)).pipeThrough(new DecompressionStream("lz4")));
        assertDeepEq(Array.from(out.bytes), Array.from(enc.encode("hello world")), "round trip through pipeThrough + pipeTo");
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
await runRow("break in a for-await loop closes a duck source once and releases a ReadableStream", async () => {
    const src = slowSource("a\nb\nc", 64);
    for await (const line of lines(src)) { void line; break; }
    assertEq(src.closed, 1, "break calls return() which closes the duck source exactly once");
    const rs = fromBytes("a\nb\nc");
    for await (const line of lines(rs)) { void line; break; }
    assertEq(rs.locked, false, "break releases the ReadableStream's reader");
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
 *  Table 5: CompressionStream/DecompressionStream "lz4" (portable on
 *  every platform; the other formats have their own suites).
 * ------------------------------------------------------------------ */
const CODEC_ROWS = [
    ["one write round trips", async () => {
        const z = await compress("lz4", [enc.encode("hello")]);
        const out = await drain(fromBytes(z).pipeThrough(new DecompressionStream("lz4")));
        assertDeepEq(Array.from(out.bytes), Array.from(enc.encode("hello")), "round trip restores the raw bytes");
    }],
    ["multiple writes accumulate", async () => {
        const z = await compress("lz4", [enc.encode("hello"), enc.encode("abc")]);
        const out = await drain(fromBytes(z).pipeThrough(new DecompressionStream("lz4")));
        assertDeepEq(Array.from(out.bytes), Array.from(enc.encode("helloabc")), "both writes survive the round trip");
    }],
    ["empty stream decompresses to nothing", async () => {
        const z = await compress("lz4", []);
        assert(z.length > 0, "an empty lz4 frame still has a header and end mark");
        const out = await drain(fromBytes(z).pipeThrough(new DecompressionStream("lz4")));
        assertEq(out.bytes.length, 0, "empty decompressed body");
    }],
    ["patterned 40-byte body round trips", async () => {
        const raw = new Uint8Array(40);
        for (let i = 0; i < raw.length; i++) raw[i] = i % 7;
        const z = await compress("lz4", [raw]);
        const out = await drain(fromBytes(z).pipeThrough(new DecompressionStream("lz4")));
        assertEqArr(Array.from(out.bytes), Array.from(raw), "decompressed bytes equal the original");
    }],
    ["a 3-byte-per-read duck source decompresses intact", async () => {
        const body = enc.encode("streaming decompression");
        const z = await compress("lz4", [body]);
        const src = slowSource(z, 3);
        const out = await drain(toReadableStream(src).pipeThrough(new DecompressionStream("lz4")));
        assertDeepEq(Array.from(out.bytes), Array.from(body), "chunked compressed input round trips");
        assertEq(src.closed, 1, "duck source closed once at EOF");
    }],
];
for (const [label, fn] of CODEC_ROWS) await runRow(label, fn);

/* ------------------------------------------------------------------ *
 *  Table 6: toFile -- a WritableStream over a lazily opened file.
 *  "created LAZILY at the first write"; a bad path rejects that write
 *  and errors the stream, so later writes reject with the same error
 *  (code AND errno) and close() rejects with a TypeError (spec: closing
 *  an errored stream).
 * ------------------------------------------------------------------ */
async function rejectionOf(p) {
    try { await p; } catch (e) { return e; }
    throw new Error("expected rejection");
}

{
    const TF = makeTempDir("bbstream-");
    try {
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
                const ws = toFile(p, bufSize === undefined ? undefined : { bufferSize: bufSize });
                assert(ws instanceof WritableStream, "toFile returns a WritableStream");
                const w = ws.getWriter();
                await w.write(payload);
                await w.close();
                const back = readBytes(p);
                assertEq(back.length, len, "byte count survived the round trip");
                let same = true;
                for (let i = 0; i < len; i++) if (back[i] !== payload[i]) { same = false; break; }
                assert(same, "every byte survived the round trip under the clamped buffer");
            });
        }

        await runRow("toFile is lazy: a stream that is only closed never creates the file", async () => {
            const p = new Path(TF, "lazy", "never.bin");
            const ws = toFile(p);
            assertEq(exists(p), false, "nothing on disk before the first write");
            await ws.getWriter().close();
            assertEq(exists(p), false, "close() without a write touched no disk");
        });

        await runRow("the first write creates the file; close persists the buffered tail", async () => {
            const p = new Path(TF, "counts.txt");
            const w = toFile(p).getWriter();
            assertEq(await w.write(enc.encode("buffered")), undefined, "write resolves with undefined");
            await w.write(new Uint8Array(0));
            assertEq(exists(p), true, "the file came into existence at the first write");
            await w.close();
            assertEq(readFile(p), "buffered", "close() pushed the buffered bytes to the file");
        });

        writeFile(new Path(TF, "seed.txt"), "seed");
        const STICKY_ROWS = [
            ["a missing parent dir rejects the write with ENOENT",
             (t) => String(new Path(t, "no-such-dir", "f.txt")), "ENOENT"],
            ["a file used as a parent dir rejects the write with ENOTDIR",
             (t) => String(new Path(t, "seed.txt", "child.txt")), "ENOTDIR"],
        ];
        for (const [label, badPath, code] of STICKY_ROWS) {
            await runRow(label, async () => {
                const w = toFile(badPath(TF)).getWriter();
                const e1 = await rejectionOf(w.write(enc.encode("x")));
                assertEq(e1.code, code, "the triggering write rejects with errno " + code);
                const e2 = await rejectionOf(w.write(enc.encode("y")));
                assertEq(e2.code, e1.code, "the NEXT write rejects with the same errno NAME");
                assertEq(e2.errno, e1.errno, "the NEXT write rejects with the same errno NUMBER");
                const e3 = await rejectionOf(w.close());
                assert(e3 instanceof TypeError, "close() of the errored stream rejects with a TypeError -- got |" + e3 + "|");
                const e4 = await rejectionOf(w.closed);
                assertEq(e4.code, code, "writer.closed rejects with the stored errno");
            });
        }
    } finally {
        removeAll(new Path(TF));
    }
}

await runRow("fromBytes refuses a non-byte-wide view", async () => {
    assertThrows(() => fromBytes(new Float64Array(4)),
        "d.ts fromBytes(b: Uint8Array | string): Float64Array is not a byte view",
        TypeError, /byte-wide/);
    assertThrows(() => fromBytes(new Int32Array(4)),
        "d.ts fromBytes(b: Uint8Array | string): Int32Array is not a byte view",
        TypeError, /byte-wide/);
    assert(fromBytes(new Uint8Array([1, 2, 3])) instanceof ReadableStream, "byte-wide Uint8Array still constructs a stream");
});

print("bb_stream: all tests passed (" + n + " assertions)");
