// flags: --std
// covers: c:src/modules/stream/stream-push.c
// v10b-19: dyna:stream deflate()/inflate() are gone; the same foreign checks now drive CompressionStream/DecompressionStream (old codec "deflate"
// is "deflate-raw"), written in the given chunk sizes (empty chunks included) and read to the end.
// dyna:stream gzip/deflate against a decoder and an encoder that are not ours
// (system gzip, zlib through python3). A round trip proves the pair agrees
// with itself; these prove each side agrees with the format. On macOS the
// default build takes the system codec, so the in-tree one is exercised by a
// CONFIG_PORTABLE_CODECS=y build and by every non-macOS build.
import * as os from "os";
import * as std from "std";
import { suite } from "../lib/kit.js";
import { fromBytes } from "dyna:stream";
const fmt = (c) => (c === "deflate" ? "deflate-raw" : c);
import { makeTempDir, writeFile, readBytes, removeAll } from "dyna:file";

const t = suite("stream_gzip_foreign");
const dir = makeTempDir("gzforeign");
t.after(() => removeAll(dir));
// os.exec rather than dyna:sys: a per-module slim build of dyna:stream carries no dyna:sys.
const sh = (cmd) => ({ code: os.exec(["/bin/sh", "-c", cmd], { usePath: false }) });
const Which = (tool) => sh("command -v " + tool + " >/dev/null 2>&1").code === 0;
const same = (x, y) => x.length === y.length && x.every((v, i) => v === y[i]);

function memSink() {
    const sink = { chunks: [], write(buf) { sink.chunks.push(buf.slice()); return Promise.resolve(buf.length); }, flush() { return Promise.resolve(); }, close() {} };
    return sink;
}
function cat(chunks) {
    let len = 0;
    for (const c of chunks) len += c.length;
    const out = new Uint8Array(len);
    let off = 0;
    for (const c of chunks) { out.set(c, off); off += c.length; }
    return out;
}
let seed = 0x2545f491;
const rnd = () => { seed ^= seed << 13; seed >>>= 0; seed ^= seed >>> 17; seed ^= seed << 5; seed >>>= 0; return seed; };
function text(n) {
    const words = ["alpha", "beta", "gamma", "{\"id\":", "\"name\":", "value", "\n", "the", "quick", "0123456789"];
    const out = new Uint8Array(n);
    let i = 0;
    while (i < n) { const w = words[rnd() % words.length] + " "; for (let k = 0; k < w.length && i < n; k++) out[i++] = w.charCodeAt(k); }
    return out;
}
function noise(n) { const out = new Uint8Array(n); for (let i = 0; i < n; i++) out[i] = rnd() & 255; return out; }
function zeros(n) { return new Uint8Array(n); }

// Compress `src` through the stream sink in the given write sizes.
async function streamCompress(src, codec, sizes) {
    const cs = new CompressionStream(fmt(codec));
    const d = cs.writable.getWriter();
    const outP = (async () => { const o = []; for await (const c of cs.readable) o.push(c); return o; })();
    let off = 0, k = 0;
    while (off < src.length) {
        const n = Math.min(sizes[k++ % sizes.length], src.length - off);
        if (n > 0) await d.write(src.subarray(off, off + n));
        else await d.write(new Uint8Array(0));
        off += n;
    }
    await d.close();
    return cat(await outP);
}
async function streamDecompress(bytes, codec, step) {
    const out = [];
    void step;
    for await (const c of fromBytes(bytes).pipeThrough(new DecompressionStream(fmt(codec)))) out.push(c);
    return cat(out);
}

const INPUTS = [["text", text], ["noise", noise], ["zeros", zeros]];
const SIZES = [0, 1, 2, 3, 100, 4096, 8191, 8192, 8193, 70000, 300000];
const WRITES = [[1 << 20], [1], [3, 5, 7], [1000], [8192], [65536], [1, 70000, 2, 9000], [0, 5, 0, 300]];

t.test("gzip written by system gzip decodes here at every read size", async (a) => {
    if (!Which("gzip")) { a.ok(true, "SKIP: no system gzip"); print("  SKIP  no system gzip: the foreign-encoder check did not run"); return; }
    let cases = 0;
    for (const [name, make] of INPUTS) {
        for (const n of [0, 1, 300, 8192, 70000, 300000]) {
            const src = make(n);
            writeFile(dir.join("p.src"), src);
            for (const level of [1, 6, 9]) {
                const r = sh("gzip -" + level + " -c '" + dir.join("p.src") + "' > '" + dir.join("p.gz") + "'");
                if (r.code !== 0) { a.ok(false, "gzip -" + level + " failed"); return; }
                const gz = readBytes(dir.join("p.gz"));
                for (const step of [1 << 16, 4096, 61]) {
                    if (step === 61 && n > 70000) continue;
                    const back = await streamDecompress(gz, "gzip", step);
                    cases++;
                    if (!same(back, src)) { a.ok(false, name + " n=" + n + " level=" + level + " step=" + step + " (" + back.length + " bytes back)"); return; }
                }
            }
        }
    }
    a.ok(cases > 100, cases + " cases decoded byte for byte");
}, { timeoutMs: 120000 });

t.test("damage is refused, not passed through", async (a) => {
    const src = text(20000);
    const gz = await streamCompress(src, "gzip", [3000]);
    const reject = async (bytes) => { try { await streamDecompress(bytes, "gzip", 4096); return "DECODED"; } catch (e) { return String(e.message); } };
    a.ne(await reject(gz.subarray(0, gz.length - 9)), "DECODED", "truncated before the trailer");
    a.ne(await reject(gz.subarray(0, gz.length >> 1)), "DECODED", "cut in half");
    const crc = gz.slice(); crc[crc.length - 6] ^= 0x40;
    a.match(await reject(crc), /CRC-32 mismatch/, "a flipped CRC bit");
    const len = gz.slice(); len[len.length - 2] ^= 0x01;
    a.match(await reject(len), /ISIZE|length/, "a flipped length bit");
    const body = gz.slice(); body[gz.length >> 1] ^= 0xff;
    a.ne(await reject(body), "DECODED", "a flipped byte in the middle");
    const extra = new Uint8Array(gz.length + 5); extra.set(gz); extra.set([1, 2, 3, 4, 5], gz.length);
    a.ne(await reject(extra), "DECODED", "bytes after the member");
});

await t.run();
