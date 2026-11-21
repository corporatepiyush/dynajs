// covers: integration c:src/modules/stream/stream-push.c c:src/modules/libc/libc-wst-misc.c -- the format name and the push codec are two halves of one format
// CompressionStream/DecompressionStream "zstd" (when libzstd is compiled in) and "lz4" (LZ4 frame format) are dynajs extensions to the
// Compression spec's format list. They run the same C codecs as dyna:compress zstd()/lz4Frame(), which are the independent oracle in both
// directions at every chunk split (1, 3, 4096 and whole). Decoding is strict like the spec formats: bytes after the end of the frame, a
// truncated frame and a corrupt frame all error the stream with a TypeError. The encoder output must not depend on how input was chunked.
import { suite } from "../lib/kit.js";
import * as C from "dyna:compress";

const t = suite("compression_zstd_lz4");
const enc = new TextEncoder();
async function bytesOf(rs) { const parts = []; let n = 0; for await (const c of rs) { parts.push(c); n += c.length; } const o = new Uint8Array(n); let k = 0; for (const p of parts) { o.set(p, k); k += p.length; } return o; }
function split(u, n) { const out = []; for (let i = 0; i < u.length; i += n) out.push(u.slice(i, i + n)); return out; }
function same(a, b) { if (a.length !== b.length) return false; for (let i = 0; i < a.length; i++) if (a[i] !== b[i]) return false; return true; }
function sample(n) { const u = new Uint8Array(n); let x = 7; for (let i = 0; i < n; i++) { x = (x * 1103515245 + 12345) >>> 0; u[i] = i % 977 < 600 ? (i * 31) & 63 : x >>> 24; } return u; }
let hasZstd = true;
try { new CompressionStream("zstd"); } catch (e) { hasZstd = false; }
const formats = hasZstd ? ["zstd", "lz4"] : ["lz4"];
const oracle = {
    zstd: { enc: (u) => C.zstd(u), dec: (u) => C.unzstd(u) },
    lz4: { enc: (u) => C.lz4Frame(u), dec: (u) => C.lz4Unframe(u) },
};
const data = sample(300000);

for (const f of formats) {
    t.test(`${f}: CompressionStream output decodes with dyna:compress at every chunk split, and is split-independent`, async (a) => {
        let first = null;
        for (const n of [1, 3, 4096, data.length]) {
            const src = n === 1 ? data.subarray(0, 5000) : data;
            const c = await bytesOf(ReadableStream.from(split(src, n)).pipeThrough(new CompressionStream(f)));
            a.ok(same(oracle[f].dec(c), src), `${f} split ${n}: oracle round trip`);
            if (n === 1) continue;
            if (first === null) first = c;
            else a.ok(same(c, first), `${f} split ${n}: same bytes as split 3`);
        }
    });
    t.test(`${f}: DecompressionStream decodes dyna:compress output at every chunk split`, async (a) => {
        const c = oracle[f].enc(data);
        for (const n of [1, 3, 4096, c.length]) {
            const d = await bytesOf(ReadableStream.from(split(c, n)).pipeThrough(new DecompressionStream(f)));
            a.ok(same(d, data), `${f} split ${n}: ${d.length} bytes`);
        }
        a.eq((await bytesOf(ReadableStream.from([]).pipeThrough(new CompressionStream(f)))).length > 0, true, "empty input still makes a frame");
        a.eq((await bytesOf(ReadableStream.from([oracle[f].enc(new Uint8Array(0))]).pipeThrough(new DecompressionStream(f)))).length, 0, "empty frame decodes to nothing");
    });
    t.test(`${f}: trailing bytes, truncation and corruption error the stream with a TypeError`, async (a) => {
        const c = oracle[f].enc(data.subarray(0, 20000));
        const cases = {
            trailing: [c, new Uint8Array([1, 2, 3])],
            truncated: [c.subarray(0, c.length - 5)],
            empty: [new Uint8Array(0)],
            corrupt: [(() => { const x = c.slice(); x[0] ^= 0xff; return x; })()],
        };
        for (const [name, chunks] of Object.entries(cases)) {
            let err = null;
            try { await bytesOf(ReadableStream.from(chunks).pipeThrough(new DecompressionStream(f))); } catch (e) { err = e; }
            a.ok(err instanceof TypeError, `${name}: ${err && err.constructor.name}: ${err && err.message}`);
        }
    });
}

t.test("format names: unknown formats throw TypeError and list zstd and lz4", async (a) => {
    for (const bad of ["zstd2", "LZ4", "", "lz4-raw"]) {
        let err = null;
        try { new CompressionStream(bad); } catch (e) { err = e; }
        a.ok(err instanceof TypeError && /lz4/.test(err.message), `${JSON.stringify(bad)}: ${err && err.message}`);
    }
    a.eq(new DecompressionStream("lz4").readable instanceof ReadableStream, true);
});

await t.run();
