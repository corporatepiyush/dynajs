// covers: c:src/modules/libc/libc-wst-misc.c
// TextEncoderStream/TextDecoderStream (Encoding spec) and CompressionStream/DecompressionStream (Compression spec):
// "deflate" is zlib framing (RFC 1950), "deflate-raw" is RFC 1951, "gzip" is RFC 1952. The native push codec in
// stream-push.c drives the dyna:stream codecs synchronously; dyna:compress's one-shot codecs are the independent
// oracle in both directions, at every chunk split, including the chunk sizes 1 and 3 that cut headers and trailers.
// "brotli" (Compression spec, 2025) is backed by Apple libcompression, so it exists only on darwin; elsewhere it throws TypeError.
// "zstd" and "lz4" are dynajs extensions (tests/stream/test_compression_zstd_lz4.js); unknown names still throw TypeError.
// v10b: the web streams call the push codec in C (no JS method call per chunk); one input chunk that inflates past the
// inflater's 96 KiB output window is enqueued as several pieces (the Compression spec allows any split), never empty ones.
// v10b-5: CompressionStream("gzip"/"deflate"/"deflate-raw") runs one streaming DEFLATE encoder (dyn_defs in dyn-compress.c) with a
// 32 KiB window carried across chunks; its output must not depend on how the input was chunked.
// D2 (owner choice, node parity): DecompressionStream("gzip") decodes concatenated members and ignores everything after a NUL that follows a
// member, as node zlib.gunzip does; any other byte after a member must start a valid member or it is a TypeError. Expectations were
// taken from node v26 zlib.gunzipSync and createGunzip at chunk sizes whole/1/3/7. deflate and deflate-raw keep the spec TypeError.
// WPT cost of D2 (tests/wpt-expectations.tsv): decompression-corrupt-input "trailing junk for 'gzip'" (one 0x00 appended) is FAIL, and
// decompression-extra-input times out as a whole file: its gzip case writes member+0x00 without closing and waits for an error that never comes.
import { suite } from "../lib/kit.js";
import { gzip, gunzip, deflate, inflate } from "dyna:compress";
import { platform } from "dyna:sys";

const t = suite("web_streams_codecs");
const enc = new TextEncoder();
async function bytesOf(rs) { const parts = []; for await (const c of rs) parts.push(...c); return new Uint8Array(parts); }
async function textOf(rs) { let s = ""; for await (const c of rs) s += c; return s; }
function split(u, n) { const out = []; for (let i = 0; i < u.length; i += n) out.push(u.slice(i, i + n)); return out; }
const sample = enc.encode("streams ".repeat(500) + "tail é中");
const oracle = {
    gzip: { dec: (u) => gunzip(u), enc: (u) => gzip(u) },
    deflate: { dec: (u) => inflate(u, { format: "zlib" }), enc: (u) => deflate(u, { format: "zlib" }) },
    "deflate-raw": { dec: (u) => inflate(u, { format: "raw" }), enc: (u) => deflate(u, { format: "raw" }) },
};

t.test("TextEncoderStream: strings to UTF-8, surrogate pairs split across chunks, lone surrogates", async (a) => {
    const te = new TextEncoderStream();
    a.eq(te.encoding, "utf-8");
    const out = await bytesOf(ReadableStream.from(["a\uD83D", "\uDE00b", "\uD800", "c", "\uDBFF"]).pipeThrough(te));
    a.deep(Array.from(out), [0x61, 0xf0, 0x9f, 0x98, 0x80, 0x62, 0xef, 0xbf, 0xbd, 0x63, 0xef, 0xbf, 0xbd]);
    const nums = await bytesOf(ReadableStream.from([12]).pipeThrough(new TextEncoderStream()));
    a.deep(Array.from(nums), [0x31, 0x32], "chunks are converted with ToString");
    await a.rejects(ReadableStream.from([Symbol("x")]).pipeThrough(new TextEncoderStream()).getReader().read(), TypeError);
});

t.test("TextDecoderStream: multi-byte sequences split across chunks, BufferSource kinds", async (a) => {
    const bytes = enc.encode("hé中😀!");
    const parts = split(bytes, 1).map((p, i) => i % 3 === 0 ? p.buffer : i % 3 === 1 ? new DataView(p.buffer) : p);
    a.eq(await textOf(ReadableStream.from(parts).pipeThrough(new TextDecoderStream())), "hé中😀!");
    const d = new TextDecoderStream("utf-8", { fatal: true, ignoreBOM: true });
    a.eq(d.encoding, "utf-8"); a.eq(d.fatal, true); a.eq(d.ignoreBOM, true);
    a.eq(await textOf(ReadableStream.from([new Uint8Array([0xef, 0xbb]), new Uint8Array([0xbf, 0x41])]).pipeThrough(new TextDecoderStream())), "A", "BOM stripped across chunks");
    a.throws(() => new TextDecoderStream("no-such-label"), RangeError);
});

t.test("TextDecoderStream: fatal errors and an incomplete tail at flush", async (a) => {
    await a.rejects(textOf(ReadableStream.from([new Uint8Array([0xff])]).pipeThrough(new TextDecoderStream("utf-8", { fatal: true }))), TypeError);
    await a.rejects(textOf(ReadableStream.from([new Uint8Array([0xe4, 0xb8])]).pipeThrough(new TextDecoderStream("utf-8", { fatal: true }))), TypeError);
    a.eq(await textOf(ReadableStream.from([new Uint8Array([0x41, 0xe4, 0xb8])]).pipeThrough(new TextDecoderStream())), "A�");
    await a.rejects(textOf(ReadableStream.from(["not bytes"]).pipeThrough(new TextDecoderStream())), TypeError);
});

for (const fmt of ["gzip", "deflate", "deflate-raw"]) {
    t.test(`CompressionStream(${fmt}): output decodes with dyna:compress at every chunk split`, async (a) => {
        for (const n of [1, 3, 4096, 1 << 20]) {
            const z = await bytesOf(ReadableStream.from(split(sample, n)).pipeThrough(new CompressionStream(fmt)));
            a.deep(Array.from(oracle[fmt].dec(z)), Array.from(sample), "split " + n);
        }
        if (fmt === "deflate") {
            const z = await bytesOf(ReadableStream.from([sample]).pipeThrough(new CompressionStream(fmt)));
            a.eq(z[0], 0x78, "zlib CMF");
            a.eq(((z[0] << 8) | z[1]) % 31, 0, "zlib FCHECK");
        }
        if (fmt === "gzip") {
            const z = await bytesOf(ReadableStream.from([sample]).pipeThrough(new CompressionStream(fmt)));
            a.deep([z[0], z[1], z[2]], [0x1f, 0x8b, 8], "gzip magic");
        }
    });
    t.test(`DecompressionStream(${fmt}): decodes dyna:compress output at every chunk split`, async (a) => {
        const z = oracle[fmt].enc(sample);
        for (const n of [1, 3, 7, 1 << 20]) {
            const out = await bytesOf(ReadableStream.from(split(z, n)).pipeThrough(new DecompressionStream(fmt)));
            a.deep(Array.from(out), Array.from(sample), "split " + n);
        }
    });
    t.test(`DecompressionStream(${fmt}): truncated and corrupted input is a TypeError`, async (a) => {
        const z = oracle[fmt].enc(sample);
        await a.rejects(bytesOf(ReadableStream.from([z.slice(0, z.length - 3)]).pipeThrough(new DecompressionStream(fmt))), TypeError, "truncated");
        if (fmt !== "deflate-raw") {
            const bad = z.slice(); bad[bad.length - 2] ^= 0xff;
            await a.rejects(bytesOf(ReadableStream.from([bad]).pipeThrough(new DecompressionStream(fmt))), TypeError, "checksum");
        }
        await a.rejects(bytesOf(ReadableStream.from([new Uint8Array([1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12])]).pipeThrough(new DecompressionStream(fmt))), TypeError, "garbage");
        await a.rejects(bytesOf(ReadableStream.from([new Uint8Array(0)]).pipeThrough(new DecompressionStream(fmt))), TypeError, "empty input");
    });
}

t.test("Compression round trip through both streams; empty input compresses to a valid empty stream", async (a) => {
    for (const fmt of ["gzip", "deflate", "deflate-raw"]) {
        const back = await bytesOf(ReadableStream.from(split(sample, 333)).pipeThrough(new CompressionStream(fmt)).pipeThrough(new DecompressionStream(fmt)));
        a.deep(Array.from(back), Array.from(sample), fmt);
        const empty = await bytesOf(ReadableStream.from([]).pipeThrough(new CompressionStream(fmt)));
        a.eq(oracle[fmt].dec(empty).length, 0, fmt + " empty");
    }
});

t.test("CompressionStream: one deflate stream across chunks (the 32 KiB window reaches back over chunk boundaries), split-invariant output, oracle-decodable", async (a) => {
    const bytesOf = async (rs) => { const ps = []; let L = 0; for await (const c of rs) { ps.push(c); L += c.length; } const o = new Uint8Array(L); let k = 0; for (const p of ps) { o.set(p, k); k += p.length; } return o; };
    const same = (x, y) => x.length === y.length && x.every((v, i) => v === y[i]);
    let s = 99;
    const blk = new Uint8Array(20000); for (let i = 0; i < blk.length; i++) { s = (Math.imul(s, 1103515245) + 12345) >>> 0; blk[i] = s >>> 24; }
    const two = new Uint8Array(blk.length * 2); two.set(blk); two.set(blk, blk.length);
    for (const fmt of ["gzip", "deflate", "deflate-raw"]) {
        const whole = await bytesOf(ReadableStream.from([two]).pipeThrough(new CompressionStream(fmt)));
        a.ok(whole.length < blk.length * 1.1, fmt + ": a random 20000-byte block repeated once compresses to " + whole.length + " (matches cross the first chunk)");
        a.ok(same(oracle[fmt].dec(whole), two), fmt + " whole: dyna:compress decodes");
        for (const n of [1, 7, 4096, 16384, 16653, 65536]) {
            const parts = await bytesOf(ReadableStream.from(split(two, n)).pipeThrough(new CompressionStream(fmt)));
            a.ok(parts.length === whole.length && parts.every((v, i) => v === whole[i]), fmt + " split " + n + ": output identical to the whole-chunk run");
        }
    }
    for (const len of [0, 1, 16383, 16384, 16653, 49152 + 269, 65536]) {
        const u = two.subarray(0, len);
        const z = await bytesOf(ReadableStream.from([u]).pipeThrough(new CompressionStream("gzip")));
        a.ok(same(gunzip(z), u), "gzip length " + len + " (block-boundary and empty-final-block cases)");
    }
});

t.test("DecompressionStream gzip after a member ends: node zlib.gunzip semantics (D2), at every split", async (a) => {
    const A = gzip(enc.encode("hello A ".repeat(50))), B = gzip(enc.encode("second B member")), E = gzip(new Uint8Array(0));
    const cat = (...xs) => { const o = new Uint8Array(xs.reduce((s, x) => s + x.length, 0)); let k = 0; for (const x of xs) { o.set(x, k); k += x.length; } return o; };
    const u = (...b) => new Uint8Array(b);
    const zeros = (n) => new Uint8Array(n);
    const dA = gunzip(A), dAB = cat(dA, gunzip(B));
    const badB = B.slice(); badB[badB.length - 6] ^= 1;
    const cases = [
        ["garbage", cat(A, u(1, 2, 3, 4)), null],
        ["one garbage byte", cat(A, u(0x41)), null],
        ["second member", cat(A, B), dAB],
        ["second member then garbage", cat(A, B, u(0xde, 0xad, 0xbe, 0xef)), null],
        ["second member truncated in its header", cat(A, B.subarray(0, 5)), null],
        ["second member truncated in its body", cat(A, B.subarray(0, 14)), null],
        ["second member truncated in its trailer", cat(A, B.subarray(0, B.length - 3)), null],
        ["magic only", cat(A, u(0x1f, 0x8b)), null],
        ["ID1 only", cat(A, u(0x1f)), null],
        ["ID1 then garbage", cat(A, u(0x1f, 0, 5)), null],
        ["bad method", cat(A, u(0x1f, 0x8b, 0xff, 0xff)), null],
        ["bad method then NULs", cat(A, u(0x1f, 0x8b, 0xff, 0xff), zeros(10)), null],
        ["NUL padding", cat(A, zeros(16)), dA],
        ["NUL then a valid member (ignored)", cat(A, zeros(4), B), dA],
        ["NUL then garbage (ignored)", cat(A, u(0, 0, 7, 7)), dA],
        ["empty middle member", cat(A, E, B), dAB],
        ["second member CRC-32 mismatch", cat(A, badB), null],
        ["zlib stream after the member", cat(A, deflate(enc.encode("x"), { format: "zlib" })), null],
    ];
    const run = async (chunks) => { try { return await bytesOf(ReadableStream.from(chunks).pipeThrough(new DecompressionStream("gzip"))); } catch (e) { return e; } };
    const check = (got, want, msg) => {
        if (want === null) a.ok(got instanceof TypeError, msg + ": TypeError (" + (got && got.message) + ")");
        else a.ok(got instanceof Uint8Array && got.length === want.length && got.every((v, i) => v === want[i]), msg + ": complete output (" + (got && got.length) + " of " + want.length + ")");
    };
    for (const [name, z, want] of cases) {
        for (const n of [z.length, 1, 3, 7]) check(await run(split(z, n)), want, name + " / chunks of " + n);
        let bad = 0, first = "";
        for (let p = 1; p < z.length; p++) {
            const got = await run([z.subarray(0, p), z.subarray(p)]);
            const ok = want === null ? got instanceof TypeError : got instanceof Uint8Array && got.length === want.length && got.every((v, i) => v === want[i]);
            if (!ok && !bad++) first = "split at " + p + " gave " + (got && (got.message || got.length));
        }
        a.eq(bad, 0, name + " / every two-chunk split (incl. inside the 8-byte trailer and the next header) " + first);
    }
    for (const p of [A.length - 8, A.length - 1, A.length, A.length + 1, A.length + 3, A.length + 9, A.length + B.length - 4, A.length + B.length - 1])
        check(await run([cat(A, B).subarray(0, p), cat(A, B).subarray(p, p + 2), cat(A, B).subarray(p + 2)]), dAB, "second member, three chunks cut at " + p);
});

t.test("DecompressionStream deflate/deflate-raw: any data after the end stays a TypeError (no multi-member, no NUL padding)", async (a) => {
    for (const [fmt, z] of [["deflate", deflate(sample, { format: "zlib" })], ["deflate-raw", deflate(sample, { format: "raw" })]]) {
        for (const tail of [[1, 2, 3, 4], [0, 0, 0, 0], [0], Array.from(z)]) {
            const junk = new Uint8Array(z.length + tail.length); junk.set(z); junk.set(tail, z.length);
            for (const n of [junk.length, 1, 3])
                await a.rejects(bytesOf(ReadableStream.from(split(junk, n)).pipeThrough(new DecompressionStream(fmt))), TypeError, fmt + " + " + tail.length + " trailing bytes, chunks of " + n);
        }
    }
});

t.test("DecompressionStream deflate: trailing data delivers a decoded prefix, then a TypeError (spec: the error resets the queue, so only chunks already handed to a pending read survive)", async (a) => {
    const z = deflate(sample, { format: "zlib" });
    const junk = new Uint8Array(z.length + 4); junk.set(z); junk.set([1, 2, 3, 4], z.length);
    const r = ReadableStream.from([junk]).pipeThrough(new DecompressionStream("deflate")).getReader();
    const got = [];
    let err;
    try { for (;;) { const x = await r.read(); if (x.done) break; got.push(...x.value); } } catch (e) { err = e; }
    a.ok(got.length > 0, "non-empty prefix");
    a.deep(got, Array.from(sample).slice(0, got.length), "prefix");
    a.ok(err instanceof TypeError && /trailing data/.test(err.message), "TypeError: " + (err && err.message));
});

t.test("DecompressionStream: one chunk that inflates past the 96 KiB output window arrives as ordered non-empty pieces", async (a) => {
    const big = new Uint8Array(3 << 20);
    for (let i = 0; i < big.length; i++) big[i] = (Math.imul(i, 2654435761) >>> 26) + (i % 4093 === 0 ? 7 : 0);
    for (const fmt of ["gzip", "deflate", "deflate-raw"]) {
        const z = oracle[fmt].enc(big);
        const parts = [];
        for await (const c of ReadableStream.from([z]).pipeThrough(new DecompressionStream(fmt))) parts.push(c);
        let n = 0, same = true, empty = 0;
        for (const p of parts) { if (!p.length) empty++; for (let i = 0; i < p.length && same; i++) same = p[i] === big[n + i]; n += p.length; }
        a.ok(parts.length > 1, fmt + " several pieces (" + parts.length + ")");
        a.eq(empty, 0, fmt + " no empty piece");
        a.eq(n, big.length, fmt + " length");
        a.ok(same, fmt + " bytes in order");
    }
});

t.test("format validation and non-BufferSource chunks", async (a) => {
    const brotliNative = platform() === "darwin";
    for (const f of brotliNative ? ["Gzip", "deflate64", "", "zstd2", "lz4-raw"] : ["Gzip", "deflate64", "brotli", "", "zstd2", "lz4-raw"]) {
        a.throws(() => new CompressionStream(f), TypeError, "compress " + JSON.stringify(f));
        a.throws(() => new DecompressionStream(f), TypeError, "decompress " + JSON.stringify(f));
    }
    a.throws(() => new CompressionStream(), TypeError, "undefined format");
    if (brotliNative) {
        const src = enc.encode("brotli ".repeat(500));
        const z = await bytesOf(ReadableStream.from([src]).pipeThrough(new CompressionStream("brotli")));
        a.ok(z.length > 0 && z.length < src.length, "brotli compresses");
        const back = await bytesOf(ReadableStream.from([z.subarray(0, 7), z.subarray(7)]).pipeThrough(new DecompressionStream("brotli")));
        a.eq(new TextDecoder().decode(back), "brotli ".repeat(500), "brotli round-trips across a chunk split");
    }
    await a.rejects(bytesOf(ReadableStream.from(["str"]).pipeThrough(new CompressionStream("gzip"))), TypeError);
    a.eq(CompressionStream.length, 1);
    a.eq(Object.prototype.toString.call(new CompressionStream("gzip")), "[object CompressionStream]");
});

await t.run();
