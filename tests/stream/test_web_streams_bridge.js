// flags: --std
// covers: c:src/modules/stream/stream-push.c
// dyna:stream <-> WHATWG Streams bridges and the pipeTo fast path. toReadableStream hands the BYOB view itself to src.read (no copy);
// v10b-19: fromFile/fromBytes/toFile now return the web streams themselves (their read(buf)/write(buf) objects are internal), so the
// native-pair cases run on them directly; fromReadableStream/fromWritableStream and the overridable native sink are gone.
// a pipe between a toReadableStream and a toWritableStream stream skips the per-chunk WHATWG machinery: fromFile/fromBytes into toFile is
// a C loop (perf-only: with 64 KiB chunks the generic path is only ~1.4x slower, so no test can tell them apart; the bench does), any other pair runs dyna:stream pipe() (proven by
// its 128 KiB read size against the stream's chunkSize). A signal forces the generic spec path; both paths must end in the same states.
import { suite } from "../lib/kit.js";
import { perfCheck } from "../lib/perf.js";
import * as std from "std";
import * as os from "os";
import * as DS from "dyna:stream";
import { fromFile, toFile, fromBytes, lines, toReadableStream, toWritableStream } from "dyna:stream";
import { gunzip } from "dyna:compress";
import { Spawn } from "dyna:sys";
import { HTTPClient } from "dyna:http";

const t = suite("web_streams_bridge");
const dir = (std.getenv("TMPDIR") || ".") + "/wsb." + os.getpid();
os.mkdir(dir, 0o755);
let seq = 0;
const tmp = (n) => `${dir}/${seq++}.${n}`;
function pattern(n) { const u = new Uint8Array(n); for (let i = 0; i < n; i++) u[i] = (i * 131 + (i >> 9)) & 255; return u; }
function writeBytes(p, u) { const f = std.open(p, "wb"); f.write(u.buffer, u.byteOffset, u.length); f.close(); }
function readBytes(p) {
    const f = std.open(p, "rb"); f.seek(0, std.SEEK_END); const n = f.tell(); f.seek(0, std.SEEK_SET);
    const u = new Uint8Array(n); f.read(u.buffer, 0, n); f.close(); return u;
}
function same(a, b) { if (a.length !== b.length) return false; for (let i = 0; i < a.length; i++) if (a[i] !== b[i]) return false; return true; }
async function bytesOf(rs) { const parts = []; let n = 0; for await (const c of rs) { parts.push(c); n += c.length; } const o = new Uint8Array(n); let k = 0; for (const p of parts) { o.set(p, k); k += p.length; } return o; }
function duckSource(data, log) {
    let off = 0, closed = false;
    return {
        read(buf) { log.push(buf.length); const n = Math.min(buf.length, data.length - off); buf.set(data.subarray(off, off + n)); off += n; return Promise.resolve(n); },
        close() { closed = true; },
        get closed() { return closed; },
    };
}
function duckSink(log) {
    const parts = []; let closed = false, flushed = 0;
    return {
        write(b) { log.push(b.length); parts.push(b.slice()); return Promise.resolve(b.length); },
        flush() { flushed++; return Promise.resolve(); },
        close() { closed = true; },
        get closed() { return closed; },
        get flushed() { return flushed; },
        bytes() { let n = 0; for (const p of parts) n += p.length; const o = new Uint8Array(n); let k = 0; for (const p of parts) { o.set(p, k); k += p.length; } return o; },
    };
}

t.test("fromFile: default reader chunks are at most 64 KiB", async (a) => {
    const p = tmp("in"), data = pattern(200000);
    writeBytes(p, data);
    const r = fromFile(p).getReader();
    const parts = [];
    let n = 0;
    for (;;) { const { done, value } = await r.read(); if (done) break; a.ok(value instanceof Uint8Array && value.length <= 65536, "chunk size"); parts.push(value); n += value.length; }
    const all = new Uint8Array(n); let o = 0; for (const p of parts) { all.set(p, o); o += p.length; }
    a.ok(same(all, data), "bytes");
    await r.closed;
});

t.test("toReadableStream: a BYOB reader's view goes to src.read as is (filled in place)", async (a) => {
    const log = [];
    let seen = null;
    const src = duckSource(pattern(100), log);
    const inner = src.read;
    src.read = (buf) => { seen = buf; return inner(buf); };
    const r = toReadableStream(src, { chunkSize: 7 }).getReader({ mode: "byob" });
    const { value } = await r.read(new Uint8Array(50));
    a.eq(log[0], 50, "src.read saw the reader's 50-byte view, not chunkSize");
    a.eq(seen.byteLength, 0, "the view src.read filled was transferred into the result (detached), not copied out of");
    a.eq(value.buffer.byteLength, 50);
    a.ok(same(value, pattern(100).subarray(0, 50)));
    const { value: v2 } = await r.read(new Uint16Array(4));
    a.eq(v2.constructor, Uint16Array, "element type is preserved");
    a.eq(log[1], 8, "a Uint16Array(4) is offered as 8 bytes");
});

t.test("toReadableStream: cancel, a rejected read and a bad count all close the source", async (a) => {
    const c1 = duckSource(pattern(10), []);
    await toReadableStream(c1).cancel("bye");
    a.ok(c1.closed, "cancel closes");
    let closed2 = false;
    const e = new Error("disk");
    const rs2 = toReadableStream({ read: () => Promise.reject(e), close() { closed2 = true; } });
    a.eq(await rs2.getReader().read().then(() => null, (x) => x), e, "the read rejection errors the stream");
    a.ok(closed2, "rejected read closes");
    let closed3 = false;
    const rs3 = toReadableStream({ read: (b) => Promise.resolve(b.length + 1), close() { closed3 = true; } });
    await a.rejects(rs3.getReader().read(), RangeError, "count above buf.length");
    a.ok(closed3, "bad count closes");
    a.throws(() => toReadableStream({}), TypeError, "not a ByteSource");
    a.throws(() => toReadableStream(c1, { chunkSize: 0 }), RangeError, "chunkSize 0");
});

t.test("toWritableStream: partial writes loop, close flushes then closes, abort closes", async (a) => {
    const got = [];
    let flushed = 0, closed = false;
    const sink = { write(b) { const n = Math.min(3, b.length); got.push(...b.subarray(0, n)); return Promise.resolve(n); }, flush() { flushed++; return Promise.resolve(); }, close() { closed = true; } };
    const w = toWritableStream(sink).getWriter();
    await w.write(new Uint8Array([1, 2, 3, 4, 5, 6, 7]));
    await w.write("hé");
    await w.close();
    a.deep(got, [1, 2, 3, 4, 5, 6, 7, 0x68, 0xc3, 0xa9]);
    a.eq(flushed, 1); a.ok(closed);
    let aborted = false;
    await toWritableStream({ write: (b) => Promise.resolve(b.length), close() { aborted = true; } }).abort("x");
    a.ok(aborted, "abort closes the sink");
    const w0 = toWritableStream({ write: () => Promise.resolve(0), close() {} }).getWriter();
    await a.rejects(w0.write(new Uint8Array(2)), TypeError, "0 accepted of a non-empty chunk");
    await a.rejects(toWritableStream({ write: (b) => Promise.resolve(b.length) }).getWriter().write(42), TypeError, "a number is not a chunk");
});

t.test("toFile writes straight into its C buffer: bytes, strings, views and ArrayBuffers in order", async (a) => {
    const p1 = tmp("nat"), w1 = toFile(p1).getWriter();
    const big = pattern(300000);
    await w1.write(new Uint8Array([1, 2, 3]));
    await w1.write("hé\ud800!");
    await w1.write(big.subarray(10));
    await w1.write(new Uint16Array([0x0201]).buffer);
    await w1.write(new Uint16Array([0x0403, 0x0605, 0x0807]).subarray(1));
    await w1.write(new DataView(new Uint8Array([9, 10, 11, 12]).buffer, 1, 2));
    await w1.close();
    const want = new Uint8Array(3 + 7 + big.length - 10 + 2 + 4 + 2);
    want.set([1, 2, 3]); want.set([0x68, 0xc3, 0xa9, 0xef, 0xbf, 0xbd, 0x21], 3); want.set(big.subarray(10), 10); want.set([1, 2, 5, 6, 7, 8, 10, 11], want.length - 8);
    a.ok(same(readBytes(p1), want), "bytes, UTF-8 strings (lone surrogate as U+FFFD), large views, wider typed-array views, DataViews and ArrayBuffers land in order");
});

t.test("a native toFile sink keeps byte order across its buffer threshold: small chunks coalesce, large ones go out with the pending bytes in one writev", async (a) => {
    for (const bufferSize of [4096, 131072]) {
        const p = tmp("mix"), w = toFile(p, { bufferSize }).getWriter(), src = pattern(1 << 20);
        let off = 0, i = 0;
        const sizes = [1, 7, 1000, 1023, 1024, 3000, 4095, 4096, 4097, 40000, 131071, 131072, 200000, 5, 33000];
        while (off < src.length) { const n = Math.min(sizes[i++ % sizes.length], src.length - off); w.write(src.subarray(off, off + n)); off += n; }
        await w.close();
        a.ok(same(readBytes(p), src), `bufferSize ${bufferSize}: file equals the concatenated chunks`);
    }
});

t.test("toWritableStream reads a duck-typed sink's write once at construction and once per chunk (the native-sink check never touches other sinks)", async (a) => {
    let gets = 0;
    const sink = { get write() { gets++; return (b) => Promise.resolve(b.length); }, close() {} };
    const w = toWritableStream(sink).getWriter();
    for (let i = 0; i < 3; i++) await w.write(new Uint8Array(4));
    a.eq(gets, 4);
});

t.test("a write error after open is sticky: a pipe whose reader closed fails a large write with EPIPE and errors the stream; an empty chunk never reaches the sink", async (a) => {
    os.signal(os.SIGPIPE, () => {});
    const [r, wfd] = os.pipe();
    const w = toFile(`/dev/fd/${wfd}`).getWriter();
    os.close(r);
    await w.write(new Uint8Array(0));
    a.ok(true, "an empty chunk resolves without opening the sink");
    let e1 = null, e2 = null;
    await w.write(new Uint8Array(300000)).catch((e) => { e1 = e; });
    a.ok(e1 && e1.errno === 32, `large write: ${e1}`);
    await w.write(new Uint8Array(3)).catch((e) => { e2 = e; });
    a.ok(e2 === e1, `a later small write rejects with the same error: ${e2}`);
    os.close(wfd);
});

t.test("a native toFile sink error is sticky through toWritableStream: the first write rejects with the errno error, the stream errors", async (a) => {
    const w = toFile(`${dir}/no/such/dir/x.bin`).getWriter();
    let e1 = null;
    await w.write(new Uint8Array(4)).catch((e) => { e1 = e; });
    a.ok(e1 && e1.code === "ENOENT", `first write: ${e1}`);
    let e2 = null;
    await w.write(new Uint8Array(4)).catch((e) => { e2 = e; });
    a.ok(e2 === e1, "the errored stream rejects later writes with the same error");
    await w.closed.catch(() => {});
});

t.test("CompressionStream into toFile: closing the pipe completes the gzip member", async (a) => {
    const p = tmp("gz"), data = pattern(50000);
    await ReadableStream.from([data.subarray(0, 20000), data.subarray(20000)]).pipeThrough(new CompressionStream("gzip")).pipeTo(toFile(p));
    a.ok(same(gunzip(readBytes(p)), data));
});

t.test("lines() reads a ReadableStream of mixed string and byte chunks", async (a) => {
    const ls = [];
    for await (const l of lines(ReadableStream.from(["al", "pha\nbe", new TextEncoder().encode("ta\n"), new Uint8Array(0), "gamma"]))) ls.push(l);
    a.deep(ls, ["alpha", "beta", "gamma"]);
});

t.test("fast path, native pair: fromFile -> toFile copies in C, closes both streams and releases the locks", async (a) => {
    const pin = tmp("in"), pout = tmp("out"), data = pattern((5 << 20) + 12345);
    writeBytes(pin, data);
    const rs = fromFile(pin), ws = toFile(pout);
    a.eq(await rs.pipeTo(ws), undefined);
    a.ok(same(readBytes(pout), data), "file copied");
    a.ok(!rs.locked && !ws.locked, "locks released");
    const r = rs.getReader(); a.eq((await r.read()).done, true, "readable is closed");
    a.eq(await ws.getWriter().closed, undefined, "writable is closed");
});

t.test("fast path, native pair from bytes; a missing input file errors both streams and closes both ends", async (a) => {
    const pout = tmp("out"), data = pattern(400000);
    await fromBytes(data).pipeTo(toFile(pout));
    a.ok(same(readBytes(pout), data));
    const rs = fromFile(dir + "/no-such-file"), ws = toFile(tmp("out"));
    const e = await rs.pipeTo(ws).then(() => null, (x) => x);
    a.ok(e instanceof Error && /no-such-file/.test(e.message), "the open error rejects pipeTo: " + (e && e.message));
    await a.rejects(rs.getReader().read(), Error, "readable errored");
    await a.rejects(ws.getWriter().closed, Error, "writable errored");
});

t.test("fast path, duck-typed pair goes through dyna:stream pipe(); a signal forces the generic path with the same result", async (a) => {
    const data = pattern(300000);
    const runIt = async (opts) => {
        const rl = [], wl = [];
        const src = duckSource(data, rl), sink = duckSink(wl);
        const rs = toReadableStream(src, { chunkSize: 1000 }), ws = toWritableStream(sink);
        const r = await rs.pipeTo(ws, opts);
        return { r, rl, wl, out: sink.bytes(), srcClosed: src.closed, sinkClosed: sink.closed, flushed: sink.flushed, locked: rs.locked || ws.locked };
    };
    const fast = await runIt(undefined);
    const slow = await runIt({ signal: new AbortController().signal });
    a.eq(fast.rl[0], 131072, "fast path reads 128 KiB");
    a.eq(slow.rl[0], 1000, "generic path reads chunkSize");
    a.ok(same(fast.out, data) && same(slow.out, data), "same bytes");
    for (const k of ["r", "srcClosed", "sinkClosed", "flushed", "locked"]) a.eq(fast[k], slow[k], k);
    a.eq(fast.srcClosed, true);
});

t.test("fast path into a small-buffer sink reads 128 KiB at a time and keeps bytes already buffered in the sink first", async (a) => {
    const pin = tmp("in"), pout = tmp("out"), data = pattern(1 << 20);
    writeBytes(pin, data);
    const sink = toFile(pout, { bufferSize: 4096 });
    const w0 = sink.getWriter();
    await w0.write(new Uint8Array([7, 7, 7]));
    w0.releaseLock();
    await fromFile(pin).pipeTo(sink);
    const got = readBytes(pout);
    a.eq(got.length, data.length + 3);
    a.deep(Array.from(got.subarray(0, 3)), [7, 7, 7], "the earlier buffered write comes first");
    a.ok(same(got.subarray(3), data));
});

t.test("fast path with highWaterMark > 0 piped in the same tick: the start-time pull does not race the native loop", async (a) => {
    const pout = tmp("out"), data = pattern(300000);
    const rl = [], sink = duckSink([]);
    await toReadableStream(duckSource(data, rl), { highWaterMark: 65536, chunkSize: 1000 }).pipeTo(toWritableStream(sink));
    a.ok(same(sink.bytes(), data), "duck pair too");
    a.ok(rl.every((n) => n === 131072), "only pipe() read the duck source: " + rl.slice(0, 4).join(","));
});

t.test("fast path is skipped with prevent* options or a queued chunk; those pipes still work", async (a) => {
    const data = pattern(5000);
    const rl = [], src = duckSource(data, rl), sink = duckSink([]);
    const rs = toReadableStream(src, { chunkSize: 1000 }), ws = toWritableStream(sink);
    await rs.pipeTo(ws, { preventClose: true });
    a.eq(rl[0], 1000, "generic path");
    a.ok(!sink.closed, "preventClose leaves the sink open");
    a.ok(same(sink.bytes(), data));
    const rl2 = [];
    const rs2 = toReadableStream(duckSource(data, rl2), { chunkSize: 1000, highWaterMark: 2000 });
    await new Promise((r) => setTimeout(r, 0));
    const s2 = duckSink([]);
    await rs2.pipeTo(toWritableStream(s2));
    a.eq(rl2[0], 1000, "a source with queued bytes takes the generic path");
    a.ok(same(s2.bytes(), data));
});

t.test("subprocess stdout and stdin as WHATWG streams", async (a) => {
    const p = new Spawn("sh", ["-c", "tr a-z A-Z"], { stdin: "pipe" });
    const ws = toWritableStream(p.stdin);
    const w = ws.getWriter();
    await w.write("hello ");
    await w.write(new TextEncoder().encode("streams"));
    await w.close();
    let s = "";
    for await (const c of toReadableStream(p.stdout).pipeThrough(new TextDecoderStream())) s += c;
    a.eq(s, "HELLO STREAMS");
    await p.wait();
    p.close();
});

t.test("an HTTP getStream response (chunked) as a ReadableStream, through TextDecoderStream", async (a) => {
    const w = new os.Worker("./web_streams_http_child.js");
    const port = await new Promise((r) => { w.onmessage = (e) => r(e.data.port); });
    const client = new HTTPClient();
    const res = await client.getStream("http://127.0.0.1:" + port + "/rows");
    a.eq(res.status, 200);
    let text = "";
    for await (const s of toReadableStream(res, { chunkSize: 777 }).pipeThrough(new TextDecoderStream())) text += s;
    const rows = text.split("\n");
    a.eq(rows.length, 2001);
    a.eq(rows[1999], "row 1999");
    w.postMessage("stop");
    w.onmessage = null;
    client.close();
}, { timeoutMs: 15000 });

t.test("cancel while a subprocess read is in flight: the read settles done, the late native completion is ignored", async (a) => {
    const p = new Spawn("sh", ["-c", "sleep 0.2; echo late"]);
    const r = toReadableStream(p.stdout).getReader();
    const pending = r.read();
    await new Promise((res) => setTimeout(res, 30));
    await r.cancel("stop");
    a.deep(await pending, { done: true, value: undefined });
    await p.wait();
    await new Promise((res) => setTimeout(res, 50));
    p.close();
});

t.test("toReadableStream with highWaterMark > 0 pulls ahead into its own chunkSize buffers before any reader", async (a) => {
    const rl = [];
    const rs = toReadableStream(duckSource(pattern(5000), rl), { chunkSize: 1000, highWaterMark: 2000 });
    await new Promise((r) => setTimeout(r, 0));
    a.deep(rl, [1000, 1000], "two chunks fill the 2000-byte high-water mark");
    a.ok(same(await bytesOf(rs), pattern(5000)));
    let closed = false;
    const bad = toReadableStream({ read() { throw new TypeError("sync throw"); }, close() { closed = true; } }, { highWaterMark: 10 });
    await new Promise((r) => setTimeout(r, 0));
    await a.rejects(bad.getReader().read(), TypeError, "a synchronous throw from read() errors the stream");
    a.ok(closed);
});

t.test("perf: a toWritableStream write costs about what a plain JS WritableStream write does (was 4x: a throwing ArrayBuffer brand check per chunk)", async (a) => {
    const c = new Uint8Array(1024);
    const run = async (ws) => { const w = ws.getWriter(); const t0 = performance.now(); for (let i = 0; i < 8192; i++) await w.write(c); return performance.now() - t0; };
    const sink = { write: (b) => Promise.resolve(b.length), flush: () => Promise.resolve(), close() {} };
    let bridge = Infinity, plain = Infinity;
    for (let i = 0; i < 3; i++) {
        bridge = Math.min(bridge, await run(toWritableStream(sink)));
        plain = Math.min(plain, await run(new WritableStream({ write() { return Promise.resolve(); } })));
    }
    const gate = (ok, w) => a.ok(ok, w);
    perfCheck(gate, bridge < plain * 3, `bridge ${(bridge * 1000 / 8192).toFixed(2)} us/write vs plain ${(plain * 1000 / 8192).toFixed(2)} us/write`, (w) => a.skip(w));
});

t.test("a default reader's short chunk: under half the buffer is copied out, half or more keeps the buffer (no autoAllocate pull-into)", async (a) => {
    let left = 3000;
    const src = { read(b) { const n = Math.min(left, 700, b.length); left -= n; return Promise.resolve(n); }, close() {} };
    const r = toReadableStream(src, { chunkSize: 4096 }).getReader();
    const x = await r.read();
    a.eq(x.value.length, 700);
    a.eq(x.value.buffer.byteLength, 700, "a short read is copied out, not handed over in a chunkSize buffer");
    let left2 = 3000;
    const src2 = { read(b) { const n = Math.min(left2, 3000, b.length); left2 -= n; return Promise.resolve(n); }, close() {} };
    const y = await toReadableStream(src2, { chunkSize: 4096 }).getReader().read();
    a.eq(y.value.length, 3000);
    a.eq(y.value.buffer.byteLength, 4096, "a read of at least half the buffer is handed over without a copy");
    const br = toReadableStream(duckSource(pattern(10), []), { chunkSize: 4096 }).getReader({ mode: "byob" });
    const v = await br.read(new Uint8Array(64));
    a.eq(v.value.length, 10, "a BYOB read still fills the caller's view");
    a.eq(v.value.buffer.byteLength, 64);
});

t.test("perf: the fast path beats the generic spec path (12x over the old JS core; v10b cut the generic path to ~2.5 us/chunk, quiet ratio ~2.0-2.6x, so the gate is 1.4x on best-of-5)", async (a) => {
    const N = 16 << 20, mem = pattern(N);
    const k = () => { let n = 0; return { write(b) { n += b.length; return Promise.resolve(b.length); }, flush: () => Promise.resolve(), close() {}, get n() { return n; } }; };
    const once = async (generic) => {
        const sink = k(), t0 = performance.now();
        await toReadableStream(duckSource(mem, []), { chunkSize: 16384 }).pipeTo(toWritableStream(sink), generic ? { signal: new AbortController().signal } : undefined);
        const ms = performance.now() - t0;
        a.eq(sink.n, N);
        return ms;
    };
    let fast = Infinity, slow = Infinity;
    for (let i = 0; i < 5; i++) { fast = Math.min(fast, await once(false)); slow = Math.min(slow, await once(true)); }
    const gate = (c, w) => a.ok(c, w);
    perfCheck(gate, fast * 1.4 < slow, `fast ${fast.toFixed(1)} ms vs generic ${slow.toFixed(1)} ms for 16 MiB in 16 KiB chunks`, (w) => a.skip(w));
});

t.test("a fast pipe into a sink whose write returns a plain number rejects with the Promise<number> TypeError, and the error is freed (it leaked and aborted runtime teardown)", async (a) => {
    let err;
    try { await toReadableStream(duckSource(new Uint8Array(10), [])).pipeTo(toWritableStream({ write(b) { return b.length; } })); } catch (e) { err = e; }
    a.ok(err instanceof TypeError, "rejects with a TypeError");
    a.ok(/must return a Promise<number>/.test(err.message), "names the contract");
});

t.test("dyna:stream bridge exports are plain functions; the ByteSource/ByteSink exports are gone", (a) => {
    for (const f of [toReadableStream, toWritableStream]) a.eq(typeof f, "function");
    for (const k of ["pipe", "inflate", "deflate", "fromReadableStream", "fromWritableStream"]) a.eq(k in DS, false, k);
    a.ok(fromBytes("x") instanceof ReadableStream && fromFile(dir + "/x") instanceof ReadableStream && toFile(tmp("w")) instanceof WritableStream);
});

await t.run();
for (const n of os.readdir(dir)[0]) if (n !== "." && n !== "..") os.remove(dir + "/" + n);
os.remove(dir);
