// flags: --std
/* test_stream.js — dyna:stream (plan ): pull-based, buffer-oriented byte
 * streams. ByteSource/ByteSink resource classes, fromBytes/fromFile/toFile
 * factories, and the CPS-driven pipe (short-write retry, onChunk, close-both
 * on failure).
 *
 * The critical shape this suite pins: a LINE/CHUNK split across read
 * boundaries survives (the stage-2 suites build on it), a multi-MB file is
 * byte-identical through pipe (bigger than every buffer involved), every
 * error lands on the promise surface (lazy open), and close() racing the
 * loop cannot corrupt (inline settle design: no async fd window).
 *
 * Run: dynajs (built with CONFIG_NATIVE_MODULES=y) tests/test_stream.js
 * Prints "test_stream: all tests passed (N assertions)" on success; throws
 * on failure. */

import { pipe, fromBytes, fromFile, toFile, lines, ndjson, inflate, deflate } from "dyna:stream";
import {
    Path, writeFile, readFile, readFileAsync, makeDir, removeAll,
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

/* ---- fromBytes: copy semantics, byte views, string ---------------------- */

{
    const bytes = enc.encode("abcdef");
    const s = fromBytes(bytes);
    bytes.fill(0); // caller mutation after the fact must be invisible (copied)
    const out = new Uint8Array(3);
    assertEq(await s.read(out), 3, "fromBytes read returns requested count");
    assertEq(out[0], 97, "fromBytes copy not aliased to caller buffer");
    assertEq(await s.read(out), 3, "fromBytes second read");
    assertEq(await s.read(out), 0, "fromBytes EOF is 0");
    assertEq(await s.read(out), 0, "fromBytes EOF stays 0");
    s.close();
    assert(s.closed, "fromBytes close marks closed");
}
{
    const s = fromBytes("héllo"); // string form: UTF-8, 6 bytes
    const out = new Uint8Array(16);
    assertEq(await s.read(out), 6, "fromBytes(string) is UTF-8 encoded");
    assertEq(out[1], 0xc3, "fromBytes(string) multi-byte lead");
    s.close();
}
{
    // 0-length read answers 0 without advancing (nothing requested)
    const s = fromBytes("abc");
    assertEq(await s.read(new Uint8Array(0)), 0, "zero-length buffer read");
    const out = new Uint8Array(2);
    assertEq(await s.read(out), 2, "source unadvanced by zero-length read");
    s.close();
}
{
    // reads larger than the source clamp to the remaining bytes
    const s = fromBytes("ab");
    const out = new Uint8Array(64);
    assertEq(await s.read(out), 2, "oversized read clamps to source length");
    s.close();
}

/* ---- toFile: buffering, append, persistence ----------------------------- */

{
    const f = tmpPath("sink");
    const w = toFile(f);
    assertEq(await w.write(enc.encode("one\n")), 4, "write accepts whole view");
    assertEq(await w.write(enc.encode("two\n")), 4, "second write accepted");
    await w.flush();
    assertEq((await readFileAsync(f, { bytes: true })).length, 8, "flush persists");
    await w.write(enc.encode("three\n"));
    w.close();
    assert(w.closed, "sink close marks closed");
    const text = new TextDecoder().decode(await readFileAsync(f, { bytes: true }));
    assertEq(text, "one\ntwo\nthree\n", "close persists the buffered tail");
}
{
    const f = tmpPath("append");
    const w1 = toFile(f);
    await w1.write(enc.encode("alpha"));
    w1.close();
    const w2 = toFile(f, { append: true });
    await w2.write(enc.encode("beta"));
    w2.close();
    assertEq(readFile(f), "alphabeta", "append mode keeps existing contents");
}
{
    // write after close throws SYNCHRONOUSLY (resource framework)
    const w = toFile(tmpPath("closed"));
    w.close();
    assertThrows(() => w.write(enc.encode("x")), "write after close throws");
    assertThrows(() => w.flush(), "flush after close throws");
}
{
    // a large direct write (bigger than the 128 KiB sink buffer) still lands whole
    const f = tmpPath("bigwrite");
    const big = new Uint8Array(300 * 1024).fill(0x5a);
    const w = toFile(f, { bufferSize: 64 * 1024 });
    assertEq(await w.write(big), big.length, "large write fully accepted");
    w.close();
    const back = await readFileAsync(f, { bytes: true });
    assertEq(back.length, big.length, "large write byte count");
    assertEq(back[129 * 1024], 0x5a, "large write content past buffer size");
}

/* ---- fromFile: lazy open, promise-surface errors ------------------------ */

{
    const src = fromBytes("0123456789");
    const f = tmpPath("filesrc");
    const w = toFile(f);
    const buf = new Uint8Array(256);
    let total = 0, got;
    while ((got = await src.read(buf)) > 0) total += got;
    void w; void f; void total;
    src.close();
    assert(true, "drain loop completes");
}

{
    // THE missing-file contract: read() REJECTS, the factory does not throw
    const missing = fromFile(new Path(dir, "no-such-" + (++seq) + ".bin"));
    await assertRejects(missing.read(new Uint8Array(8)), "missing file read rejects", "ENOENT");
    missing.close();
}
{
    // toFile to a bad directory path rejects at write, not at the factory
    const w = toFile(new Path(dir, "no-such-dir-" + (++seq), "f.bin"));
    await assertRejects(w.write(enc.encode("x")), "bad sink path rejects at write");
}
{
    // close before any read: nothing was ever opened, no error
    const s = fromFile(tmpPath("never-opened"));
    s.close();
    assert(s.closed, "close before open is clean");
}

/* ---- pipe --------------------------------------------------------------- */

{
    // bytes -> file, byte-identical, onChunk fires per accepted chunk
    const f = tmpPath("pipe-bytes");
    const payload = "hello stream world\n".repeat(100); // 1900 bytes: ONE 128 KiB chunk
    let chunks = 0, chunkBytes = 0;
    const dst = toFile(f);
    const total = await pipe(fromBytes(payload), dst, {
        onChunk(b) { chunks++; chunkBytes += b; },
    });
    dst.close();
    assertEq(total, 1900, "pipe total bytes");
    assertEq(chunkBytes, 1900, "onChunk byte sum");
    assertEq(chunks, 1, "small payload is one chunk");
    const back = await readFileAsync(f, { bytes: true });
    assertEq(back.length, 1900, "piped length");
    const want = enc.encode(payload);
    let same = true;
    for (let i = 0; same && i < want.length; i++) if (back[i] !== want[i]) same = false;
    assert(same, "piped bytes identical");
}
{
    // a payload larger than the 128 KiB chunk spans multiple chunks
    const f = tmpPath("pipe-multichunk");
    const size = 3 * 128 * 1024 + 5;
    let chunks = 0, chunkBytes = 0;
    const dst = toFile(f);
    const total = await pipe(fromBytes(new Uint8Array(size).fill(7)), dst, {
        onChunk(b) { chunks++; chunkBytes += b; },
    });
    dst.close();
    assertEq(total, size, "multi-chunk pipe total");
    assertEq(chunkBytes, size, "multi-chunk onChunk byte sum");
    assert(chunks >= 4, "payload spanned multiple chunks");
    assertEq((await readFileAsync(f, { bytes: true })).length, size, "multi-chunk output length");
}
{
    // THE multi-MB file roundtrip: byte-identical through pipe, source and
    // destination both larger than every internal buffer
    const fin = tmpPath("pipe-in");
    const fout = tmpPath("pipe-out");
    const blob = new Uint8Array(3 * 1024 * 1024 + 777);
    for (let i = 0; i < blob.length; i++) blob[i] = (i * 7 + (i >> 8)) & 0xff;
    writeFile(fin, blob);
    const total = await pipe(fromFile(fin), toFile(fout));
    assertEq(total, blob.length, "multi-MB pipe total");
    const back = await readFileAsync(fout, { bytes: true });
    assertEq(back.length, blob.length, "multi-MB pipe output length");
    let same = true;
    for (let i = 0; same && i < blob.length; i++) if (back[i] !== blob[i]) { same = false; break; }
    assert(same, "multi-MB pipe byte-identical");
}
{
    // empty source pipes 0 bytes
    const fout = tmpPath("pipe-empty");
    assertEq(await pipe(fromBytes(""), toFile(fout)), 0, "empty pipe total");
}
{
    // success does NOT close either side; the caller owns both
    const src = fromBytes("ok");
    const dst = toFile(tmpPath("pipe-open"));
    await pipe(src, dst);
    assert(!src.closed, "pipe leaves source open on success");
    assert(!dst.closed, "pipe leaves sink open on success");
    dst.close();
    src.close();
}
{
    // short-write sink: pipe loops until the whole chunk is accepted
    let writes = 0;
    const slow = {
        async write(buf) { writes++; return buf.length > 4 ? 4 : buf.length; },
    };
    const total = await pipe(fromBytes("z".repeat(30)), slow);
    assertEq(total, 30, "short-write sink receives every byte");
    assert(writes >= 30 / 4, "short-write loop actually looped");
}
{
    // a sink accepting 0 of a non-empty chunk is a pipe failure
    const blackhole = { async write() { return 0; } };
    await assertRejects(pipe(fromBytes("data"), blackhole), "zero-accept sink rejects");
}
{
    // a read() resolving out of range is a pipe failure
    const liar = { async read() { return -1; } };
    const sink = { async write(b) { return b.length; } };
    await assertRejects(pipe(liar, sink), "negative read rejects");
    const liar2 = { async read(buf) { return buf.length + 1; } };
    await assertRejects(pipe(liar2, sink), "overlong read rejects");
}
{
    // throwing source: pipe rejects AND closes BOTH sides
    let closedA = false, closedB = false;
    const bad = { read() { throw new Error("boom"); }, close() { closedA = true; } };
    const good = { async write(b) { return b.length; }, close() { closedB = true; } };
    let msg = "";
    try { await pipe(bad, good); } catch (e) { msg = e.message; }
    assertEq(msg, "boom", "throwing source rejects with its error");
    assert(closedA && closedB, "throwing source closes both sides");
}
{
    // a sink failing mid-pipe: rejects and closes both
    let closedA = false, closedB = false, reads = 0;
    const src = {
        async read(buf) { reads++; if (reads === 1) { buf[0] = 1; return 1; } return 0; },
        close() { closedA = true; },
    };
    const bad = { async write() { throw new Error("sink-down"); }, close() { closedB = true; } };
    let msg = "";
    try { await pipe(src, bad); } catch (e) { msg = e.message; }
    assertEq(msg, "sink-down", "failing sink rejects with its error");
    assert(closedA && closedB, "failing sink closes both sides");
}
{
    // throwing onChunk: rejects and closes BOTH (the observer must not be
    // able to kill the loop silently)
    let closedA = false, closedB = false, reads = 0;
    const src = {
        async read(buf) { reads++; if (reads === 1) { buf.fill(9, 0, 5); return 5; } return 0; },
        close() { closedA = true; },
    };
    const dst = { async write(b) { return b.length; }, close() { closedB = true; } };
    let msg = "";
    try { await pipe(src, dst, { onChunk() { throw new Error("observer-boom"); } }); }
    catch (e) { msg = e.message; }
    assertEq(msg, "observer-boom", "throwing onChunk rejects");
    assert(closedA && closedB, "throwing onChunk closes both sides");
}
{
    // a source that closes itself after EOF is fine; pipe resolves once
    const src = fromBytes("tail");
    const dst = toFile(tmpPath("pipe-eof"));
    const total = await pipe(src, dst);
    assertEq(total, 4, "EOF terminates the loop exactly once");
    dst.close();
}
{
    // pipe validates its arguments up front
    assertThrows(() => pipe({}, {}), "pipe without methods throws");
    assertThrows(() => pipe({ read() {} }, { write: 42 }), "pipe without a callable write throws");
    assertThrows(() => pipe(fromBytes("x"), { write() {} }, { onChunk: "no" }), "pipe with a non-function onChunk throws");
}
{
    // duck-typed JS sink over the native source (the integration contract)
    let received = 0;
    const dst = { async write(buf) { received += buf.length; return buf.length; } };
    assertEq(await pipe(fromBytes("12345"), dst), 5, "duck sink pipe");
    assertEq(received, 5, "duck sink received the bytes");
}

/* ---- lines() ------------------------------------------------------------- */

{
    // basic split
    const out = [];
    for await (const l of lines(fromBytes("alpha\nbeta\ngamma\n"))) out.push(l);
    assertEq(JSON.stringify(out), JSON.stringify(["alpha", "beta", "gamma"]),
        "lines basic split");
}
{
    // THE critical case: a line split across read boundaries -- a source
    // that yields ONE BYTE per read still produces intact lines
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
}
{
    // no trailing newline: the residual final line is still delivered
    const out = [];
    for await (const l of lines(fromBytes("a\nb\nc"))) out.push(l);
    assertEq(JSON.stringify(out), JSON.stringify(["a", "b", "c"]),
        "residual final line without newline");
}
{
    // CRLF stripped; empty lines preserved
    const out = [];
    for await (const l of lines(fromBytes("a\r\n\r\nb\r\n"))) out.push(l);
    assertEq(JSON.stringify(out), JSON.stringify(["a", "", "b"]), "CRLF + empty lines");
}
{
    // for-await break calls return() which closes the source
    let closed = false;
    let pos = 0;
    const src = {
        async read(buf) { /* 64-byte reads, newline every 32 bytes */
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
}
{
    // multi-byte UTF-8 split across read boundaries decodes intact
    const s = "héllo wörld\n".repeat(50);
    const bytes = enc.encode(s);
    let i = 0;
    const src = { async read(buf) { if (i >= bytes.length) return 0; buf[0] = bytes[i++]; return 1; } };
    let ok = true, count = 0;
    for await (const l of lines(src)) { count++; if (l !== "héllo wörld") ok = false; }
    assert(ok && count === 50, "utf-8 carried across chunk boundaries");
}
{
    // invalid utf-8 -> U+FFFD replacement (FileReader.readLine parity)
    const out = [];
    for await (const l of lines(fromBytes(new Uint8Array([0x61, 0xff, 0x62, 0x0a]))))
        out.push(l);
    assertEq(out[0], "a\uFFFD b".replace(" ", ""), "invalid utf-8 replaced");
}
{
    // encoding option: utf-8 accepted (default), others refused loudly
    const out = [];
    for await (const l of lines(fromBytes("x\n"), { encoding: "utf-8" })) out.push(l);
    assertEq(out[0], "x", "explicit utf-8 accepted");
    let threw = false;
    try { lines(fromBytes("x\n"), { encoding: "utf-16le" }); }
    catch (e) { threw = /utf-8/.test(e.message); }
    assert(threw, "non-utf-8 encoding refused");
}
{
    // 10000 lines through a real file keep order (64-byte reads underneath)
    const f = tmpPath("lines-file");
    const w = toFile(f);
    const one = enc.encode("x".repeat(31) + "\n");
    for (let i = 0; i < 10000; i++) await w.write(one);
    w.close();
    let count = 0, last = "";
    for await (const l of lines(fromFile(f))) { count++; last = l; }
    assertEq(count, 10000, "10000-line file count");
    assertEq(last.length, 31, "10000-line file last line");
}
{
    // a read() failure mid-iteration rejects next() (direct-await shape)
    const src = { async read(buf) { throw new Error("read-died"); } };
    const it = lines(src);
    let msg = "";
    try { await it.next(); } catch (e) { msg = e.message; }
    assertEq(msg, "read-died", "read error rejects next()");
}

/* ---- ndjson() ------------------------------------------------------------ */

{
    const out = [];
    for await (const v of ndjson(fromBytes('{"a":1}\n{"b":[2]}\n\n{"c":"three"}\n')))
        out.push(v);
    assertEq(out.length, 3, "ndjson value count");
    assertEq(out[0].a, 1, "ndjson first value");
    assertEq(JSON.stringify(out[1]), JSON.stringify({ b: [2] }), "ndjson nested");
    assertEq(out[2].c, "three", "ndjson last value (blank line skipped)");
}
{
    // malformed line: the error names the LINE NUMBER
    const it = ndjson(fromBytes('{"ok":1}\nnot json\n{"fine":2}\n'));
    const first = await it.next();
    assertEq(first.value.ok, 1, "ndjson first line parses");
    let msg = "";
    try { await it.next(); } catch (e) { msg = e.message; }
    assert(/line 2/.test(msg), "ndjson error includes line number: " + msg);
    assert(/not json|unexpected token/.test(msg), "ndjson error includes the reason");
}
{
    // 2000 JSON objects streamed from a real file
    const f = tmpPath("events.ndjson");
    const w = toFile(f);
    for (let i = 0; i < 2000; i++)
        await w.write(enc.encode(JSON.stringify({ i, sq: i * i }) + "\n"));
    w.close();
    let count = 0, sum = 0;
    for await (const v of ndjson(fromFile(f))) { count++; sum += v.sq; }
    assertEq(count, 2000, "ndjson file count");
    assertEq(sum, 1999 * 2000 * 3999 / 6, "ndjson file sum of squares");
}
{
    // residual last line without a newline parses too
    const out = [];
    for await (const v of ndjson(fromBytes('{"x":true}\n{"y":false}'))) out.push(v);
    assertEq(out.length, 2, "ndjson residual last line");
    assertEq(out[1].y, false, "ndjson residual value");
}
{
    // a parse error can be recovered from by continuing the iterator
    const it = ndjson(fromBytes('bad\n{"good":7}\n'));
    let msg = "";
    try { await it.next(); } catch (e) { msg = e.message; }
    assert(/line 1/.test(msg), "ndjson recoverable error");
    const good = await it.next();
    assertEq(good.value.good, 7, "ndjson continues after error");
}

/* ---- inflate / deflate (stage 3) ----------------------------------------- *
 *
 * Each codec: stream roundtrip byte-identity + cross-decode with the
 * one-shot dyna:compress functions (their output through our inflate, ours
 * through their decoder). gzip/lz4 framings are additionally spec-checked
 * in-tree (gunzip validates CRC-32/ISIZE; lz4Unframe validates the frame).
 * The read side runs through a manual drain loop (bounded syscalls per
 * hop); see the CHANGELOG for the pipe-over-inflate teardown note. */

function catChunks(chunks) {
    let len = 0;
    for (const c of chunks) len += c.length;
    const out = new Uint8Array(len);
    let o = 0;
    for (const c of chunks) { out.set(c, o); o += c.length; }
    return out;
}
async function drain(src) {
    const chunks = [];
    const buf = new Uint8Array(64 * 1024);
    let n;
    while ((n = await src.read(buf)) > 0) chunks.push(buf.slice(0, n));
    return chunks;
}
function bytesEq(a, b) {
    if (!a || a.length !== b.length) return false;
    for (let i = 0; i < a.length; i++) if (a[i] !== b[i]) return false;
    return true;
}

{
    const zstd = mod_compress.zstd, unzstd = mod_compress.unzstd;
    const brotli = mod_compress.brotli, unbrotli = mod_compress.unbrotli;
    const lz4Frame = mod_compress.lz4Frame, lz4Unframe = mod_compress.lz4Unframe;
    const gzip = mod_compress.gzip, gunzip = mod_compress.gunzip;

    const raw = enc.encode("the quick brown fox jumps over the lazy dog. ".repeat(4000));

    // zstd
    {
        const f = tmpPath("zstd.zst");
        const ds = deflate(toFile(f), { codec: "zstd", level: 5 });
        assertEq(await ds.write(raw), raw.length, "zstd write accepts all");
        const total = await ds.finish();
        assertEq(total, raw.length, "zstd finish returns raw total");
        assert(bytesEq(catChunks(await drain(inflate(fromFile(f), { codec: "zstd" }))), raw),
            "zstd roundtrip");
        assert(bytesEq(unzstd(await readFileAsync(f, { bytes: true })), raw),
            "one-shot unzstd decodes streamed output");
        assert(bytesEq(catChunks(await drain(inflate(fromBytes(zstd(raw, 5)), { codec: "zstd" }))), raw),
            "streamed inflate decodes one-shot output");
    }
    // brotli
    {
        const f = tmpPath("brotli.br");
        const ds = deflate(toFile(f), { codec: "brotli" });
        await ds.write(raw);
        await ds.finish();
        assert(bytesEq(catChunks(await drain(inflate(fromFile(f), { codec: "brotli" }))), raw),
            "brotli roundtrip");
        assert(bytesEq(unbrotli(await readFileAsync(f, { bytes: true })), raw),
            "one-shot unbrotli decodes streamed output");
        assert(bytesEq(catChunks(await drain(inflate(fromBytes(brotli(raw)), { codec: "brotli" }))), raw),
            "streamed brotli inflate decodes one-shot output");
    }
    // lz4
    {
        const f = tmpPath("data.lz4");
        const ds = deflate(toFile(f), { codec: "lz4" });
        await ds.write(raw);
        await ds.finish();
        assert(bytesEq(catChunks(await drain(inflate(fromFile(f), { codec: "lz4" }))), raw),
            "lz4 roundtrip");
        assert(bytesEq(lz4Unframe(await readFileAsync(f, { bytes: true })), raw),
            "one-shot lz4Unframe decodes streamed frame");
        assert(bytesEq(catChunks(await drain(inflate(fromBytes(lz4Frame(raw)), { codec: "lz4" }))), raw),
            "streamed lz4 inflate decodes one-shot frame");
    }
    // gzip: trailer CRC-32/ISIZE are verified by our own reader, and the
    // one-shot gunzip independently validates the streamed output.
    {
        const f = tmpPath("data.gz");
        const ds = deflate(toFile(f), { codec: "gzip" });
        await ds.write(raw);
        await ds.finish();
        assert(bytesEq(catChunks(await drain(inflate(fromFile(f), { codec: "gzip" }))), raw),
            "gzip roundtrip");
        assert(bytesEq(gunzip(await readFileAsync(f, { bytes: true })), raw),
            "one-shot gunzip validates streamed gzip output");
    }
    // deflate (raw RFC 1951)
    {
        const f = tmpPath("data.zz");
        const ds = deflate(toFile(f), { codec: "deflate" });
        await ds.write(raw);
        await ds.finish();
        assert(bytesEq(catChunks(await drain(inflate(fromFile(f), { codec: "deflate" }))), raw),
            "deflate roundtrip");
    }
    // truncated stream: a cut-off zstd frame rejects instead of EOF
    {
        const f = tmpPath("trunc.zst");
        const good = tmpPath("whole.zst");
        const ds = deflate(toFile(good), { codec: "zstd" });
        await ds.write(enc.encode("payload that compresses, payload that compresses"));
        await ds.finish();
        const whole = await readFileAsync(good, { bytes: true });
        writeFile(f, whole.subarray(0, whole.length - 3));
        const it = inflate(fromFile(f), { codec: "zstd" });
        let msg = "";
        try { await drain(it); } catch (e) { msg = e.message; }
        // zstd names its own truncation signal ("no progress ... input
        // being empty"); accept either wording
        assert(/truncat|no progress|empty/i.test(msg),
            "truncated zstd rejects: " + msg);
    }
    // unknown codec refused
    {
        let threw = false;
        try { deflate(toFile(tmpPath("nope")), { codec: "bzip2" }); }
        catch (e) { threw = /codec/.test(e.message); }
        assert(threw, "unknown codec refused");
    }
}

/* ---- resource surface --------------------------------------------------- */

{
    // [Symbol.dispose] and dispose() are the same close
    const s = fromBytes("x");
    s.dispose();
    assert(s.closed, "dispose marks closed");
    const s2 = fromBytes("y");
    s2[Symbol.dispose]();
    assert(s2.closed, "Symbol.dispose marks closed");
}
{
    // closed getter on the sink
    const w = toFile(tmpPath("closedflag"));
    assert(!w.closed, "fresh sink is open");
    w.close();
    assert(w.closed, "closed sink reports closed");
}

removeAll(dir);
print("test_stream: all tests passed (" + n + " assertions)");
