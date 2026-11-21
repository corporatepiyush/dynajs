// flags: --std
// covers: dyna:stream.toFile
// v10b-19: toFile(path, {append?, bufferSize?}) is a WritableStream of BufferSource or string (UTF-8) chunks over the file. The path is
// checked at the call; the file is created/truncated (or opened for append) on the first write, so an unwritable path rejects that write
// and errors the stream. Writes are coalesced in a buffer of bufferSize bytes (default 128 KiB, clamped to 4 KiB..64 MiB) that is allocated
// on the first write, so a live, never-written toFile costs no buffer; chunks of at least a quarter of the buffer that overflow it go out
// with one writev. close() flushes and closes the fd; abort() closes the fd keeping what was already flushed; a write error is sticky.
import { suite } from "../lib/kit.js";
import { toFile } from "dyna:stream";
import { Path, readBytes, readFile, writeFile, makeTempDir, removeAll } from "dyna:file";
import { memoryUsage } from "dyna:sys";

const t = suite("stream_to_file");
const dir = makeTempDir();
const enc = new TextEncoder();
let seq = 0;
const tmp = (n) => new Path(dir, n + "." + (++seq));
function pattern(n) { const u = new Uint8Array(n); for (let i = 0; i < n; i++) u[i] = (i * 131 + (i >> 9)) & 255; return u; }
function same(a, b) { if (a.length !== b.length) return false; for (let i = 0; i < a.length; i++) if (a[i] !== b[i]) return false; return true; }

t.test("a live toFile that was never written holds no write buffer (runs first: peakRss is a high-water mark)", async (a) => {
    const N = 1000;
    toFile(tmp("warm"));
    await new Promise((r) => setTimeout(r, 0));
    std.gc();
    const m0 = memoryUsage();
    const keep = [];
    for (let i = 0; i < N; i++) keep.push(toFile(tmp("live")));
    await new Promise((r) => setTimeout(r, 0));
    std.gc();
    const m1 = memoryUsage();
    const per = (m1.mallocSize - m0.mallocSize) / N;
    a.ok(per < 8192, per.toFixed(0) + " B engine heap per live toFile");
    if (m0.peakRss < 24 << 20) a.ok(m1.peakRss - m0.peakRss < 32 << 20, ((m1.peakRss - m0.peakRss) / N).toFixed(0) + " B peak RSS per live toFile (a touched 128 KiB buffer each would add 128 MB)");
    keep.length = 0;
});

t.test("writes bytes and strings in order; close flushes", async (a) => {
    const p = tmp("w");
    const ws = toFile(p);
    a.ok(ws instanceof WritableStream, "WritableStream");
    const w = ws.getWriter();
    await w.write(enc.encode("one\n"));
    await w.write("two\n");
    await w.write(new Uint16Array([0x6874, 0x6572]));
    await w.write(new ArrayBuffer(0));
    await w.close();
    a.eq(readFile(p), "one\ntwo\nthre");
});

t.test("append keeps existing contents; bufferSize-sized and larger writes are byte-exact", async (a) => {
    const p = tmp("app");
    writeFile(p, "alpha");
    let w = toFile(p, { append: true }).getWriter();
    await w.write("beta");
    await w.close();
    a.eq(readFile(p), "alphabeta");
    for (const bs of [4096, 65536, undefined]) {
        const q = tmp("big");
        const data = pattern(700000);
        w = toFile(q, bs ? { bufferSize: bs } : undefined).getWriter();
        for (const n of [1, 100, 5000, 70000, 300000]) await w.write(data.subarray(0, n));
        await w.write(data);
        await w.close();
        const want = new Uint8Array(1 + 100 + 5000 + 70000 + 300000 + data.length);
        let o = 0;
        for (const n of [1, 100, 5000, 70000, 300000, data.length]) { want.set(data.subarray(0, n), o); o += n; }
        a.ok(same(readBytes(q), want), "bufferSize " + bs);
    }
});

t.test("an unwritable path rejects the first write and errors the stream; bad paths throw TypeError", async (a) => {
    const ws = toFile(new Path(dir, "no-such-dir", "f.bin"));
    const w = ws.getWriter();
    let err = null;
    try { await w.write("x"); } catch (e) { err = e; }
    a.ok(err && err.code === "ENOENT", "ENOENT: " + (err && err.message));
    err = null;
    try { await w.write("y"); } catch (e) { err = e; }
    a.ok(err !== null, "errored stream rejects later writes");
    for (const bad of [undefined, 7, "a\0b"]) {
        err = null;
        try { toFile(bad); } catch (e) { err = e; }
        a.ok(err instanceof TypeError, "toFile(" + JSON.stringify(bad) + ") TypeError");
    }
});

t.test("abort keeps flushed bytes and closes; writes after close reject", async (a) => {
    const p = tmp("abort");
    const w = toFile(p, { bufferSize: 4096 }).getWriter();
    await w.write(pattern(10000));
    await w.abort(new Error("stop"));
    a.ok(readBytes(p).length >= 8192, "flushed part kept: " + readBytes(p).length);
    const w2 = toFile(tmp("closed")).getWriter();
    await w2.close();
    let err = null;
    try { await w2.write("x"); } catch (e) { err = e; }
    a.ok(err instanceof TypeError, "write after close: " + err);
});

await t.run();
removeAll(dir);
