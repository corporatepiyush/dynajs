// flags: --std
// dyna:stream gzip/deflate against a decoder and an encoder that are not ours
// (system gzip, zlib through python3). A round trip proves the pair agrees
// with itself; these prove each side agrees with the format. On macOS the
// default build takes the system codec, so the in-tree one is exercised by a
// CONFIG_PORTABLE_CODECS=y build and by every non-macOS build.
import * as os from "os";
import * as std from "std";
import { suite } from "../lib/kit.js";
import { deflate, inflate, fromBytes } from "dyna:stream";
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
    const sink = memSink();
    const d = deflate(sink, { codec });
    let off = 0, k = 0;
    while (off < src.length) {
        const n = Math.min(sizes[k++ % sizes.length], src.length - off);
        if (n > 0) await d.write(src.subarray(off, off + n));
        else await d.write(new Uint8Array(0));
        off += n;
    }
    await d.finish();
    return cat(sink.chunks);
}
async function streamDecompress(bytes, codec, step) {
    const src = inflate(fromBytes(bytes), { codec });
    const out = [];
    const buf = new Uint8Array(step);
    for (;;) { const n = await src.read(buf); if (n === 0) break; out.push(buf.slice(0, n)); }
    return cat(out);
}

const INPUTS = [["text", text], ["noise", noise], ["zeros", zeros]];
const SIZES = [0, 1, 2, 3, 100, 4096, 8191, 8192, 8193, 70000, 300000];
const WRITES = [[1 << 20], [1], [3, 5, 7], [1000], [8192], [65536], [1, 70000, 2, 9000], [0, 5, 0, 300]];

t.test("every chunking of every input decodes under system gzip", async (a) => {
    if (!Which("gzip")) { a.ok(true, "SKIP: no system gzip"); print("  SKIP  no system gzip: the foreign-decoder check did not run"); return; }
    let cases = 0;
    for (const [name, make] of INPUTS) {
        for (const n of SIZES) {
            const src = make(n);
            for (const sizes of WRITES) {
                if (sizes[0] === 1 && sizes.length === 1 && n > 5000) continue;
                const gz = await streamCompress(src, "gzip", sizes);
                writeFile(dir.join("c.gz"), gz);
                const r = sh("gzip -dc '" + dir.join("c.gz") + "' > '" + dir.join("c.out") + "'");
                const back = r.code === 0 ? readBytes(dir.join("c.out")) : null;
                cases++;
                if (!(back && same(back, src))) { a.ok(false, name + " n=" + n + " writes=" + JSON.stringify(sizes) + " (gzip exit " + r.code + ")"); return; }
            }
        }
    }
    a.ok(cases > 200, cases + " cases decoded byte for byte");
}, { timeoutMs: 120000 });

t.test("raw deflate output decodes under zlib", async (a) => {
    if (!Which("python3")) { a.ok(true, "SKIP: no python3"); print("  SKIP  no python3: the zlib check did not run"); return; }
    let cases = 0;
    for (const [name, make] of INPUTS) {
        for (const n of [0, 1, 300, 8192, 70000]) {
            const src = make(n);
            for (const sizes of [[1 << 20], [7, 1000], [8192]]) {
                const raw = await streamCompress(src, "deflate", sizes);
                writeFile(dir.join("r.bin"), raw);
                writeFile(dir.join("r.src"), src);
                const r = sh("python3 -c \"import zlib,sys; d=zlib.decompress(open(sys.argv[1],'rb').read(), -15); sys.exit(0 if d==open(sys.argv[2],'rb').read() else 3)\" '" + dir.join("r.bin") + "' '" + dir.join("r.src") + "'");
                cases++;
                if (r.code !== 0) { a.ok(false, name + " n=" + n + " writes=" + JSON.stringify(sizes) + " (zlib exit " + r.code + ")"); return; }
            }
        }
    }
    a.ok(cases === 45, cases + " cases");
}, { timeoutMs: 120000 });

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

t.test("raw deflate written by zlib decodes here", async (a) => {
    if (!Which("python3")) { a.ok(true, "SKIP: no python3"); print("  SKIP  no python3: the zlib-encoder check did not run"); return; }
    let cases = 0;
    for (const [name, make] of INPUTS) {
        for (const n of [1, 300, 8192, 70000, 300000]) {
            const src = make(n);
            writeFile(dir.join("z.src"), src);
            const r = sh("python3 -c \"import zlib,sys; c=zlib.compressobj(6, zlib.DEFLATED, -15); d=c.compress(open(sys.argv[1],'rb').read())+c.flush(); open(sys.argv[2],'wb').write(d)\" '" + dir.join("z.src") + "' '" + dir.join("z.bin") + "'");
            if (r.code !== 0) { a.ok(false, "zlib failed"); return; }
            const raw = readBytes(dir.join("z.bin"));
            for (const step of [1 << 16, 4096]) {
                const back = await streamDecompress(raw, "deflate", step);
                cases++;
                if (!same(back, src)) { a.ok(false, name + " n=" + n + " step=" + step + " (" + back.length + " bytes back)"); return; }
            }
        }
    }
    a.ok(cases === 30, cases + " cases");
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
