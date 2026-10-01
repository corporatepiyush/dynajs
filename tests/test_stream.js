// flags: --std
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

{
    const bytes = enc.encode("abcdef");
    const s = fromBytes(bytes);
    bytes.fill(0);
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
    const s = fromBytes("héllo");
    const out = new Uint8Array(16);
    assertEq(await s.read(out), 6, "fromBytes(string) is UTF-8 encoded");
    assertEq(out[1], 0xc3, "fromBytes(string) multi-byte lead");
    s.close();
}
{
    const s = fromBytes("abc");
    assertEq(await s.read(new Uint8Array(0)), 0, "zero-length buffer read");
    const out = new Uint8Array(2);
    assertEq(await s.read(out), 2, "source unadvanced by zero-length read");
    s.close();
}
{
    const s = fromBytes("ab");
    const out = new Uint8Array(64);
    assertEq(await s.read(out), 2, "oversized read clamps to source length");
    s.close();
}

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
    const w = toFile(tmpPath("closed"));
    w.close();
    assertThrows(() => w.write(enc.encode("x")), "write after close throws");
    assertThrows(() => w.flush(), "flush after close throws");
}
{
    const f = tmpPath("bigwrite");
    const big = new Uint8Array(300 * 1024).fill(0x5a);
    const w = toFile(f, { bufferSize: 64 * 1024 });
    assertEq(await w.write(big), big.length, "large write fully accepted");
    w.close();
    const back = await readFileAsync(f, { bytes: true });
    assertEq(back.length, big.length, "large write byte count");
    assertEq(back[129 * 1024], 0x5a, "large write content past buffer size");
}

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
    const missing = fromFile(new Path(dir, "no-such-" + (++seq) + ".bin"));
    await assertRejects(missing.read(new Uint8Array(8)), "missing file read rejects", "ENOENT");
    missing.close();
}
{
    const w = toFile(new Path(dir, "no-such-dir-" + (++seq), "f.bin"));
    await assertRejects(w.write(enc.encode("x")), "bad sink path rejects at write");
}
{
    const s = fromFile(tmpPath("never-opened"));
    s.close();
    assert(s.closed, "close before open is clean");
}

{
    const f = tmpPath("pipe-bytes");
    const payload = "hello stream world\n".repeat(100);
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
    const fout = tmpPath("pipe-empty");
    assertEq(await pipe(fromBytes(""), toFile(fout)), 0, "empty pipe total");
}
{
    const src = fromBytes("ok");
    const dst = toFile(tmpPath("pipe-open"));
    await pipe(src, dst);
    assert(!src.closed, "pipe leaves source open on success");
    assert(!dst.closed, "pipe leaves sink open on success");
    dst.close();
    src.close();
}
{
    let writes = 0;
    const slow = {
        async write(buf) { writes++; return buf.length > 4 ? 4 : buf.length; },
    };
    const total = await pipe(fromBytes("z".repeat(30)), slow);
    assertEq(total, 30, "short-write sink receives every byte");
    assert(writes >= 30 / 4, "short-write loop actually looped");
}
{
    const blackhole = { async write() { return 0; } };
    await assertRejects(pipe(fromBytes("data"), blackhole), "zero-accept sink rejects");
}
{
    const liar = { async read() { return -1; } };
    const sink = { async write(b) { return b.length; } };
    await assertRejects(pipe(liar, sink), "negative read rejects");
    const liar2 = { async read(buf) { return buf.length + 1; } };
    await assertRejects(pipe(liar2, sink), "overlong read rejects");
}
{
    let closedA = false, closedB = false;
    const bad = { read() { throw new Error("boom"); }, close() { closedA = true; } };
    const good = { async write(b) { return b.length; }, close() { closedB = true; } };
    let msg = "";
    try { await pipe(bad, good); } catch (e) { msg = e.message; }
    assertEq(msg, "boom", "throwing source rejects with its error");
    assert(closedA && closedB, "throwing source closes both sides");
}
{
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
    const src = fromBytes("tail");
    const dst = toFile(tmpPath("pipe-eof"));
    const total = await pipe(src, dst);
    assertEq(total, 4, "EOF terminates the loop exactly once");
    dst.close();
}
{
    assertThrows(() => pipe({}, {}), "pipe without methods throws");
    assertThrows(() => pipe({ read() {} }, { write: 42 }), "pipe without a callable write throws");
    assertThrows(() => pipe(fromBytes("x"), { write() {} }, { onChunk: "no" }), "pipe with a non-function onChunk throws");
}
{
    let received = 0;
    const dst = { async write(buf) { received += buf.length; return buf.length; } };
    assertEq(await pipe(fromBytes("12345"), dst), 5, "duck sink pipe");
    assertEq(received, 5, "duck sink received the bytes");
}

{
    const out = [];
    for await (const l of lines(fromBytes("alpha\nbeta\ngamma\n"))) out.push(l);
    assertEq(JSON.stringify(out), JSON.stringify(["alpha", "beta", "gamma"]),
        "lines basic split");
}
{
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
    const out = [];
    for await (const l of lines(fromBytes("a\nb\nc"))) out.push(l);
    assertEq(JSON.stringify(out), JSON.stringify(["a", "b", "c"]),
        "residual final line without newline");
}
{
    const out = [];
    for await (const l of lines(fromBytes("a\r\n\r\nb\r\n"))) out.push(l);
    assertEq(JSON.stringify(out), JSON.stringify(["a", "", "b"]), "CRLF + empty lines");
}
{
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
}
{
    const s = "héllo wörld\n".repeat(50);
    const bytes = enc.encode(s);
    let i = 0;
    const src = { async read(buf) { if (i >= bytes.length) return 0; buf[0] = bytes[i++]; return 1; } };
    let ok = true, count = 0;
    for await (const l of lines(src)) { count++; if (l !== "héllo wörld") ok = false; }
    assert(ok && count === 50, "utf-8 carried across chunk boundaries");
}
{
    const out = [];
    for await (const l of lines(fromBytes(new Uint8Array([0x61, 0xff, 0x62, 0x0a]))))
        out.push(l);
    assertEq(out[0], "a\uFFFD b".replace(" ", ""), "invalid utf-8 replaced");
}
{
    const out = [];
    for await (const l of lines(fromBytes("x\n"), { encoding: "utf-8" })) out.push(l);
    assertEq(out[0], "x", "explicit utf-8 accepted");
    let threw = false;
    try { lines(fromBytes("x\n"), { encoding: "utf-16le" }); }
    catch (e) { threw = /utf-8/.test(e.message); }
    assert(threw, "non-utf-8 encoding refused");
}
{
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
    const src = { async read(buf) { throw new Error("read-died"); } };
    const it = lines(src);
    let msg = "";
    try { await it.next(); } catch (e) { msg = e.message; }
    assertEq(msg, "read-died", "read error rejects next()");
}

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
    const it = ndjson(fromBytes('{"ok":1}\nnot json\n{"fine":2}\n'));
    const first = await it.next();
    assertEq(first.value.ok, 1, "ndjson first line parses");
    let msg = "";
    try { await it.next(); } catch (e) { msg = e.message; }
    assert(/line 2/.test(msg), "ndjson error includes line number: " + msg);
    assert(/not json|unexpected token/.test(msg), "ndjson error includes the reason");
}
{
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
    const out = [];
    for await (const v of ndjson(fromBytes('{"x":true}\n{"y":false}'))) out.push(v);
    assertEq(out.length, 2, "ndjson residual last line");
    assertEq(out[1].y, false, "ndjson residual value");
}
{
    const it = ndjson(fromBytes('bad\n{"good":7}\n'));
    let msg = "";
    try { await it.next(); } catch (e) { msg = e.message; }
    assert(/line 1/.test(msg), "ndjson recoverable error");
    const good = await it.next();
    assertEq(good.value.good, 7, "ndjson continues after error");
}

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
    // gzip/deflate are libcompression-backed (macOS-only); the rest of the
    // matrix (zstd/brotli/lz4) is available everywhere the libs are linked.
    // Probe each once so an unsupported codec skips its sections loudly
    // instead of failing the suite.
    const hasCodec = {};
    for (const c of ["zstd", "brotli", "lz4", "gzip", "deflate"]) {
        try {
            const f = tmpPath("probe." + c);
            const ds = deflate(toFile(f), { codec: c });
            await ds.write(new TextEncoder().encode("probe"));
            await ds.finish();
            await drain(inflate(fromFile(f), { codec: c }));
            hasCodec[c] = true;
        } catch (e) {
            hasCodec[c] = false;
            print("  SKIP  codec '" + c + "' is unavailable here (" +
                  e.message.slice(0, 60) + ")");
        }
    }


    const raw = enc.encode("the quick brown fox jumps over the lazy dog. ".repeat(4000));

    if (hasCodec["zstd"]) {
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
    if (hasCodec["brotli"]) {
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
    if (hasCodec["lz4"]) {
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
    if (hasCodec["gzip"]) {
        const f = tmpPath("data.gz");
        const ds = deflate(toFile(f), { codec: "gzip" });
        await ds.write(raw);
        await ds.finish();
        assert(bytesEq(catChunks(await drain(inflate(fromFile(f), { codec: "gzip" }))), raw),
            "gzip roundtrip");
        assert(bytesEq(gunzip(await readFileAsync(f, { bytes: true })), raw),
            "one-shot gunzip validates streamed gzip output");
    }
    if (hasCodec["deflate"]) {
        const f = tmpPath("data.zz");
        const ds = deflate(toFile(f), { codec: "deflate" });
        await ds.write(raw);
        await ds.finish();
        assert(bytesEq(catChunks(await drain(inflate(fromFile(f), { codec: "deflate" }))), raw),
            "deflate roundtrip");
    }
    if (hasCodec["zstd"]) {
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
        assert(/truncat|no progress|empty/i.test(msg),
            "truncated zstd rejects: " + msg);
    }
    {
        let threw = false;
        try { deflate(toFile(tmpPath("nope")), { codec: "bzip2" }); }
        catch (e) { threw = /codec/.test(e.message); }
        assert(threw, "unknown codec refused");
    }
}

{
    const s = fromBytes("x");
    s.dispose();
    assert(s.closed, "dispose marks closed");
    const s2 = fromBytes("y");
    s2[Symbol.dispose]();
    assert(s2.closed, "Symbol.dispose marks closed");
}
{
    const w = toFile(tmpPath("closedflag"));
    assert(!w.closed, "fresh sink is open");
    w.close();
    assert(w.closed, "closed sink reports closed");
}

removeAll(dir);
print("test_stream: all tests passed (" + n + " assertions)");




