// covers: dyna:stream.fromBytes
// v10b-19: fromBytes(b) is a ReadableStream<Uint8Array> (a byte stream: default and BYOB readers) over a COPY of b taken at the call (a
// string is UTF-8 encoded); chunks are at most 64 KiB; cancel() drops the copy. It pipes natively into toFile (a C loop, no per-chunk
// promise) and feeds lines()/ndjson()/DecompressionStream like any ReadableStream. The old ByteSource read(buf)/close() API is gone.
import { suite } from "../lib/kit.js";
import { fromBytes, toFile, lines, ndjson } from "dyna:stream";
import { Path, readBytes, makeTempDir, removeAll } from "dyna:file";
import { gzip } from "dyna:compress";

const t = suite("stream_from_bytes");
const enc = new TextEncoder();
const dir = makeTempDir();
async function bytesOf(rs) { const parts = []; let n = 0; for await (const c of rs) { parts.push(c); n += c.length; } const o = new Uint8Array(n); let k = 0; for (const p of parts) { o.set(p, k); k += p.length; } return o; }
function pattern(n) { const u = new Uint8Array(n); for (let i = 0; i < n; i++) u[i] = (i * 131 + (i >> 9)) & 255; return u; }
function same(a, b) { if (a.length !== b.length) return false; for (let i = 0; i < a.length; i++) if (a[i] !== b[i]) return false; return true; }

t.test("a ReadableStream over a copy of the bytes; strings are UTF-8", async (a) => {
    const b = enc.encode("abcdef");
    const rs = fromBytes(b);
    a.ok(rs instanceof ReadableStream, "ReadableStream");
    b.fill(0);
    a.eq(new TextDecoder().decode(await bytesOf(rs)), "abcdef", "copied at the call");
    const s = await bytesOf(fromBytes("héllo"));
    a.eq(s.length, 6);
    a.eq(s[1], 0xc3, "UTF-8 lead byte");
    a.eq((await bytesOf(fromBytes(""))).length, 0, "empty string");
    a.eq((await bytesOf(fromBytes(new Uint8Array(0)))).length, 0, "empty bytes");
    const sub = pattern(100).subarray(10, 20);
    a.ok(same(await bytesOf(fromBytes(sub)), sub), "a subarray view is read from its offset");
});

t.test("chunks are at most 64 KiB; a BYOB reader fills its own view", async (a) => {
    const data = pattern(300000);
    const sizes = [];
    for await (const c of fromBytes(data)) sizes.push(c.length);
    a.ok(sizes.every((n) => n > 0 && n <= 65536), "sizes " + sizes.join(","));
    a.eq(sizes.reduce((x, y) => x + y, 0), data.length);
    const r = fromBytes("0123456789").getReader({ mode: "byob" });
    const { value, done } = await r.read(new Uint8Array(4));
    a.eq(done, false);
    a.eq(new TextDecoder().decode(value), "0123");
    await r.cancel();
});

t.test("cancel ends the stream; a locked stream cannot be read twice", async (a) => {
    const rs = fromBytes("xyz");
    await rs.cancel("bye");
    a.eq((await rs.getReader().read()).done, true, "cancelled stream reads done");
    const rs2 = fromBytes("abc");
    rs2.getReader();
    let err = null;
    try { rs2.getReader(); } catch (e) { err = e; }
    a.ok(err instanceof TypeError, "locked");
    for (const bad of [undefined, 42, {}, [1, 2]]) {
        err = null;
        try { fromBytes(bad); } catch (e) { err = e; }
        a.ok(err instanceof TypeError, "fromBytes(" + JSON.stringify(bad) + ") TypeError");
    }
});

t.test("feeds toFile, lines, ndjson and DecompressionStream", async (a) => {
    const data = pattern(1 << 20);
    const p = new Path(dir, "fb.bin");
    await fromBytes(data).pipeTo(toFile(p));
    a.ok(same(readBytes(p), data), "byte-exact file");
    const got = [];
    for await (const l of lines(fromBytes("a\nb\r\nc"))) got.push(l);
    a.eq(JSON.stringify(got), '["a","b","c"]');
    const vals = [];
    for await (const v of ndjson(fromBytes('{"x":1}\n[2]\n'))) vals.push(v);
    a.eq(JSON.stringify(vals), '[{"x":1},[2]]');
    a.ok(same(await bytesOf(fromBytes(gzip(data)).pipeThrough(new DecompressionStream("gzip"))), data), "gunzip through DecompressionStream");
});

await t.run();
removeAll(dir);
