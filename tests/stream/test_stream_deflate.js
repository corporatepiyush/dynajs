// covers: integration dyna:stream.toFile dyna:stream.fromFile -- CompressionStream/DecompressionStream between file streams (the C push codec)
// v10b-19: dyna:stream deflate()/inflate() are gone; CompressionStream/DecompressionStream over fromFile/toFile replace them (old codec
// "deflate" was raw DEFLATE, now "deflate-raw"; the `level` option is gone: zstd runs level 3, lz4 level 1, DEFLATE level 6). Every format
// round trips through files, and dyna:compress's one-shot codecs are the independent oracle in both directions. A truncated zstd file errors.
import { suite } from "../lib/kit.js";
import { fromFile, toFile, fromBytes } from "dyna:stream";
import * as C from "dyna:compress";
import { Path, readBytes, writeFile, makeTempDir, removeAll } from "dyna:file";

const t = suite("stream_deflate");
const dir = makeTempDir();
let seq = 0;
const tmp = (n) => new Path(dir, n + "." + (++seq));
const enc = new TextEncoder();
async function bytesOf(rs) { const parts = []; let n = 0; for await (const c of rs) { parts.push(c); n += c.length; } const o = new Uint8Array(n); let k = 0; for (const p of parts) { o.set(p, k); k += p.length; } return o; }
function same(a, b) { if (!a || a.length !== b.length) return false; for (let i = 0; i < a.length; i++) if (a[i] !== b[i]) return false; return true; }
const raw = enc.encode("the quick brown fox jumps over the lazy dog. ".repeat(4000));
const oracle = {
    zstd: [C.zstd, C.unzstd], brotli: [C.brotli, C.unbrotli], lz4: [C.lz4Frame, C.lz4Unframe], gzip: [C.gzip, C.gunzip],
    "deflate-raw": [(u) => C.deflate(u, { format: "raw" }), (u) => C.inflate(u, { format: "raw" })],
};

for (const f of Object.keys(oracle)) {
    let ok = true;
    try { new CompressionStream(f); } catch (e) { ok = false; }
    if (!ok) { print("  SKIP  format '" + f + "' is unavailable here"); continue; }
    t.test(`${f}: file round trip; one-shot oracle both ways`, async (a) => {
        const p = tmp(f);
        await fromBytes(raw).pipeThrough(new CompressionStream(f)).pipeTo(toFile(p));
        a.ok(same(await bytesOf(fromFile(p).pipeThrough(new DecompressionStream(f))), raw), "round trip");
        a.ok(same(oracle[f][1](readBytes(p)), raw), "one-shot decoder reads the streamed file");
        const q = tmp(f + ".one");
        writeFile(q, oracle[f][0](raw));
        a.ok(same(await bytesOf(fromFile(q).pipeThrough(new DecompressionStream(f))), raw), "stream decoder reads the one-shot file");
    });
}

t.test("a truncated zstd file errors the stream", async (a) => {
    let ok = true;
    try { new CompressionStream("zstd"); } catch (e) { ok = false; }
    if (!ok) return;
    const whole = C.zstd(enc.encode("payload that compresses, payload that compresses"));
    const p = tmp("trunc.zst");
    writeFile(p, whole.subarray(0, whole.length - 3));
    let err = null;
    try { await bytesOf(fromFile(p).pipeThrough(new DecompressionStream("zstd"))); } catch (e) { err = e; }
    a.ok(err instanceof TypeError, "truncated: " + err);
});

await t.run();
removeAll(dir);
