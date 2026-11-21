// flags: --std
// covers: dyna:stream.fromFile
// v10b-19: fromFile(path) is a ReadableStream<Uint8Array> (a byte stream) over the file. The path is checked at the call (a Path or a
// string without NUL, else TypeError); the file is opened on the first read, so a missing file errors the stream (the read rejects with
// the errno error, code ENOENT) instead of throwing at the call. Chunks are at most 64 KiB; the fd is closed at EOF, on cancel() and on a
// read error, so many streams opened and dropped leak no descriptor. Piped into toFile it runs a C copy loop (see test_web_streams_bridge).
import { suite } from "../lib/kit.js";
import { fromFile, toFile } from "dyna:stream";
import { Path, writeFile, readBytes, makeTempDir, removeAll, readDir } from "dyna:file";

const t = suite("stream_from_file");
const dir = makeTempDir();
async function bytesOf(rs) { const parts = []; let n = 0; for await (const c of rs) { parts.push(c); n += c.length; } const o = new Uint8Array(n); let k = 0; for (const p of parts) { o.set(p, k); k += p.length; } return o; }
function pattern(n) { const u = new Uint8Array(n); for (let i = 0; i < n; i++) u[i] = (i * 131 + (i >> 9)) & 255; return u; }
function same(a, b) { if (a.length !== b.length) return false; for (let i = 0; i < a.length; i++) if (a[i] !== b[i]) return false; return true; }
function fdCount() { try { return readDir(new Path("/dev/fd")).length; } catch (e) { return -1; } }

t.test("reads the whole file in chunks of at most 64 KiB, by Path or string", async (a) => {
    const data = pattern(1000003);
    const p = new Path(dir, "a.bin");
    writeFile(p, data);
    const sizes = [];
    const parts = [];
    for await (const c of fromFile(p)) { sizes.push(c.length); parts.push(c); }
    a.ok(sizes.every((n) => n > 0 && n <= 65536), "chunk sizes");
    a.ok(same(await bytesOf(fromFile(String(p))), data), "string path");
    writeFile(new Path(dir, "empty"), new Uint8Array(0));
    a.eq((await bytesOf(fromFile(new Path(dir, "empty")))).length, 0, "empty file");
});

t.test("a missing file errors the stream with ENOENT; bad paths throw TypeError at the call", async (a) => {
    const rs = fromFile(new Path(dir, "nope"));
    let err = null;
    try { await rs.getReader().read(); } catch (e) { err = e; }
    a.ok(err && err.code === "ENOENT", "ENOENT: " + (err && err.message));
    for (const bad of [undefined, 3, {}, "a\0b"]) {
        err = null;
        try { fromFile(bad); } catch (e) { err = e; }
        a.ok(err instanceof TypeError, "fromFile(" + JSON.stringify(bad) + ") TypeError");
    }
});

t.test("the fd closes at EOF, on cancel and when dropped unread", async (a) => {
    const p = new Path(dir, "fd.bin");
    writeFile(p, pattern(200000));
    const before = fdCount();
    for (let i = 0; i < 50; i++) await bytesOf(fromFile(p));
    for (let i = 0; i < 50; i++) { const r = fromFile(p).getReader(); await r.read(); await r.cancel(); }
    for (let i = 0; i < 50; i++) { const r = fromFile(p).getReader(); await r.read(); }
    std.gc();
    await new Promise((r) => setTimeout(r, 0));
    std.gc();
    const after = fdCount();
    a.ok(before < 0 || after - before <= 2, `fds ${before} -> ${after}`);
});

t.test("pipes into toFile byte-exact", async (a) => {
    const data = pattern(3 << 20);
    const src = new Path(dir, "src.bin"), dst = new Path(dir, "dst.bin");
    writeFile(src, data);
    await fromFile(src).pipeTo(toFile(dst));
    a.ok(same(readBytes(dst), data), "copy");
});

await t.run();
removeAll(dir);
