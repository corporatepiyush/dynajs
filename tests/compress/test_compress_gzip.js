// flags: --std
// covers: dyna:compress.gzip
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_compress_gzip");
import { gzip, gunzip, zstd, unzstd, brotli, snappy, lz4Compress, lz4Frame, compressBound, Compressor, ZipPack, TarExtract } from "dyna:compress";
import * as std from "std";
import * as os from "os";

let n = 0;
function assert(cond, msg) {
    n++;
    if (!cond) throw new Error("assertion failed: " + msg);
}

function bytesEqual(a, b) {
    if (a.length !== b.length) return false;
    for (let i = 0; i < a.length; i++)
        if (a[i] !== b[i]) return false;
    return true;
}

function readFileBytes(path) {
    const [st, serr] = os.stat(path);
    if (serr) throw new Error("stat failed: " + path);
    const size = st.size;
    const fd = os.open(path, os.O_RDONLY);
    if (fd < 0) throw new Error("open failed: " + path);
    const buf = new Uint8Array(size);
    let got = 0;
    while (got < size) {
        const r = os.read(fd, buf.buffer, got, size - got);
        if (r <= 0) break;
        got += r;
    }
    os.close(fd);
    if (got !== size) throw new Error("short read: " + path);
    return buf;
}

function writeFileBytes(path, u8) {
    const fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o644);
    if (fd < 0) throw new Error("open(w) failed: " + path);
    let put = 0;
    while (put < u8.length) {
        const w = os.write(fd, u8.buffer, put, u8.length - put);
        if (w <= 0) break;
        put += w;
    }
    os.close(fd);
    if (put !== u8.length) throw new Error("short write: " + path);
}

function u8(...vals) { return new Uint8Array(vals); }

const enc = new TextEncoder();
const englishText =
    "The quick brown fox jumps over the lazy dog. Pack my box with five " +
    "dozen liquor jugs. How vexingly quick daft zebras jump! Sphinx of " +
    "black quartz, judge my vow. The five boxing wizards jump quickly. ";
const corpora = {
    empty: new Uint8Array(0),
    oneByte: enc.encode("Z"),
    small: enc.encode("hello, gzip world!"),
    repeated: enc.encode("abcdefgh".repeat(4000)),
    newlines: enc.encode("line\n".repeat(1000)),
    english: enc.encode(englishText.repeat(80)),
};

{
    const r = new Uint8Array(5000);
    let x = 0x12345678 >>> 0;
    for (let i = 0; i < r.length; i++) {
        x = (Math.imul(x, 1103515245) + 12345) >>> 0;
        r[i] = (x >>> 16) & 0xff;
    }
    corpora.random = r;
}

{
    const b = new Uint8Array(3000);
    for (let i = 0; i < b.length; i++)
        b[i] = (i % 7 === 0) ? 0x00 : ((i * 31 + (i >> 3)) & 0xff);
    corpora.binaryNul = b;
}

__kitS.section("gzip returns Uint8Array", () => {
for (const [name, data] of Object.entries(corpora)) {
    const packed = gzip(data);
    assert(packed instanceof Uint8Array, "gzip returns Uint8Array (" + name + ")");
    assert(packed.length >= 18, "gzip output has header+trailer (" + name + ")");
    assert(packed[0] === 0x1f && packed[1] === 0x8b && packed[2] === 0x08,
           "gzip magic + deflate method (" + name + ")");
    const back = gunzip(packed);
    assert(back instanceof Uint8Array, "gunzip returns Uint8Array (" + name + ")");
    assert(bytesEqual(back, data), "round-trip preserves bytes (" + name + ")");
}
});

{
    function ratio(name) {
        const data = corpora[name];
        const packed = gzip(data);
        const r = packed.length / data.length;
        print("  ratio[" + name + "]: " + data.length + " -> " +
              packed.length + " bytes (" + r.toFixed(4) + ")");
        return r;
    }
    assert(ratio("repeated") < 0.2, "repeated data compresses hard (<0.2)");
    assert(ratio("newlines") < 0.2, "newline runs compress hard (<0.2)");
    assert(ratio("english") < 0.6, "English text actually shrinks (<0.6)");
    const rr = ratio("random");
    assert(rr < 1.02, "random data does not blow up (stored fallback)");
}

{
    const data = corpora.english;
    const mb = data.length / (1024 * 1024);
    let iters = 40, best = Infinity;
    for (let pass = 0; pass < 5; pass++) {
        const t0 = performance.now();
        for (let i = 0; i < iters; i++) gzip(data);
        const dt = (performance.now() - t0) / iters;
        if (dt < best) best = dt;
    }
    print("  gzip throughput: " + (mb / (best / 1000)).toFixed(1) +
          " MB/s (" + best.toFixed(3) + " ms / " + data.length + " bytes)");
}

__kitS.section("The quick brown fox jumps over the lazy dog.", () => {
{
    const text = "The quick brown fox jumps over the lazy dog. ".repeat(50);
    const packedStr = gzip(text);
    const decoded = gunzip(packedStr, { asString: true });
    assert(typeof decoded === "string", "asString yields a string");
    assert(decoded === text, "string round-trip via asString");

    const ab = enc.encode(text).buffer;
    const packedAb = gunzip(gzip(ab), { asString: true });
    assert(packedAb === text, "ArrayBuffer input round-trips");
}
});

__kitS.section("gzip -dc", () => {
for (const name of ["repeated", "english", "binaryNul", "random", "small"]) {
    const data = corpora[name];
    const packed = gzip(data);
    const gzPath = "tmp_compress_a.gz";
    const outPath = "tmp_compress_a.out";
    writeFileBytes(gzPath, packed);
    const rc = os.exec(["/bin/sh", "-c", "gzip -dc " + gzPath + " > " + outPath],
                       { usePath: false });
    assert(rc === 0, "system gzip -dc decoded our output (" + name + ", rc=" +
           rc + ")");
    const sysOut = readFileBytes(outPath);
    assert(bytesEqual(sysOut, data),
           "system gzip bytes match original (" + name + ")");
    os.remove(gzPath);
    os.remove(outPath);
}
});

__kitS.section("gzip -c", () => {
{
    const data = corpora.newlines;
    const inPath = "tmp_compress_b.in";
    const gzPath = "tmp_compress_b.gz";
    writeFileBytes(inPath, data);
    const rc = os.exec(["/bin/sh", "-c",
                        "gzip -c " + inPath + " > " + gzPath],
                       { usePath: false });
    assert(rc === 0, "system gzip produced a file (rc=" + rc + ")");
    const sysGz = readFileBytes(gzPath);
    const back = gunzip(sysGz);
    assert(bytesEqual(back, data), "our gunzip decoded system gzip output");
    os.remove(inPath);
    os.remove(gzPath);
}
});

__kitS.section("a string level is a TypeError", () => {
{
    function throws(fn, msg) {
        let threw = false;
        try { fn(); } catch { threw = true; }
        assert(threw, msg);
    }
    throws(() => gzip(corpora.small, "9"), "a string level is a TypeError");
    throws(() => gzip(corpora.small, { level: "9" }), "an options level that is a string is a TypeError");
    throws(() => gzip(corpora.small, { level: null }), "an options level of null is a TypeError");
    assert(bytesEqual(gzip(corpora.small, 9), gzip(corpora.small)),
           "a numeric level is accepted (tiny payload: same fixed bytes)");
    assert(bytesEqual(gzip(corpora.small), gzip(corpora.small, undefined)),
           "an absent level still works");
    assert(bytesEqual(gzip(corpora.small, { level: 9 }), gzip(corpora.small, 9)),
           "options {level:9} == positional 9");
}
});

{
    const deepcomb = (() => {
        const parts = [];
        let a = 1, b = 1;
        for (let i = 0; i < 28; i++) {
            parts.push(new Uint8Array(b).fill(i));
            const t = a + b; a = b; b = t;
        }
        const flat = new Uint8Array(parts.reduce((s, p) => s + p.length, 0));
        let off = 0;
        for (const p of parts) { flat.set(p, off); off += p.length; }
        let x = 0x2545f491 >>> 0;
        for (let i = flat.length - 1; i > 0; i--) {
            x = (Math.imul(x, 1103515245) + 12345) >>> 0;
            const j = ((x >>> 16) & 0x7fffffff) % (i + 1);
            const t = flat[i]; flat[i] = flat[j]; flat[j] = t;
        }
        return flat;
    })();

    for (const [name, data] of Object.entries(corpora)) {
        const lo = gzip(data, 1), hi = gzip(data, 6);
        assert(bytesEqual(gunzip(hi), data),
               "level 6 round-trips (" + name + ")");
        assert(hi.length <= lo.length,
               "level 6 never exceeds level 1 (" + name + ": " +
               hi.length + " vs " + lo.length + ")");
    }
    assert(bytesEqual(gunzip(gzip(deepcomb, 6)), deepcomb),
           "level 6 round-trips a >15-bit-deep Huffman tree");
    assert(gzip(deepcomb, 6).length <= gzip(deepcomb, 1).length,
           "level 6 never exceeds level 1 (deep tree)");
    {
        // Below 8 KiB the fast levels keep fixed Huffman (the table build is
        // not repaid); from 8 KiB up they use dynamic Huffman like level 6.
        const small = corpora.english.subarray(0, 4096);
        const lo = gzip(small, 1), hi = gzip(small, 6);
        print("  level 6 vs 1 [english 4 KiB]: " + lo.length + " -> " + hi.length +
              " (" + (hi.length / lo.length).toFixed(3) + ")");
        assert(((lo[10] >> 1) & 3) === 1, "level 1 below 8 KiB is a fixed-Huffman block");
        assert(((hi[10] >> 1) & 3) === 2, "level 6 is a dynamic-Huffman block");
        assert(hi.length < lo.length * 0.95, // not perf: compressed size, not time
               "dynamic Huffman beats fixed by >5% on English text");
        const big1 = gzip(corpora.english, 1), big6 = gzip(corpora.english, 6);
        assert(((big1[10] >> 1) & 3) === 2, "level 1 from 8 KiB up is a dynamic-Huffman block");
        assert(big6.length <= big1.length, "level 6 never exceeds level 1 (english, full)");
    }
    for (const data of [corpora.english, deepcomb]) {
        const gzPath = "tmp_compress_l6.gz";
        const outPath = "tmp_compress_l6.out";
        writeFileBytes(gzPath, gzip(data, 6));
        const rc = os.exec(["/bin/sh", "-c",
                            "gzip -dc " + gzPath + " > " + outPath],
                           { usePath: false });
        assert(rc === 0, "system gzip -dc decoded our level-6 output (rc=" +
               rc + ")");
        assert(bytesEqual(readFileBytes(outPath), data),
               "system gzip level-6 bytes match original");
        os.remove(gzPath);
        os.remove(outPath);
    }
    {
        function throws(fn, msg) {
            let threw = false;
            try { fn(); } catch { threw = true; }
            assert(threw, msg);
        }
        const c6 = new Compressor({ algo: "gzip", level: 6 });
        const packed = c6.compress(corpora.english);
        assert(bytesEqual(c6.decompress(packed), corpora.english),
               "Compressor gzip level 6 round-trips");
        assert(packed.length === gzip(corpora.english, 6).length,
               "Compressor level 6 matches the one-shot size");
        throws(() => new Compressor({ algo: "gzip", level: 0 }),
               "gzip level 0 is out of bounds (1..9)");
        throws(() => new Compressor({ algo: "gzip", level: 10 }),
               "gzip level 10 is out of bounds (1..9)");
        c6.close();
    }
}

__kitS.section("The quick brown fox jumps over the lazy dog. (L268)", () => {
{
    const text = "The quick brown fox jumps over the lazy dog. ".repeat(50);
    for (const algo of ["gzip", "lz4", "lz4frame", "snappy"]) {
        const c = new Compressor({ algo });
        const packed = c.compress(text);
        assert(typeof c.decompress(packed, { asString: true }) === "string",
               algo + ": asString yields a string");
        assert(c.decompress(packed, { asString: true }) === text,
               algo + ": asString round-trips the text");
        assert(c.decompress(packed) instanceof Uint8Array,
               algo + ": default stays bytes");
    }
}
});

__kitS.section("(got", () => {
{
    function eq(got, want, msg) {
        assert(got === want, msg + " (got " + got + ", want " + want + ")");
    }
    function ok(c, msg) { assert(c, msg); }
    function throws(fn, msg) {
        let threw = false;
        try { fn(); } catch { threw = true; }
        assert(threw, msg);
    }
    const msg = "the options object and the positional level drive the same encoder";
    eq(gunzip(gzip(msg, { level: 9 }), { asString: true }), msg, "gzip({level:9}) round-trips");
    eq(gunzip(gzip(msg, { level: 1 }), { asString: true }), msg, "gzip({level:1}) round-trips");
    eq(gunzip(gzip(msg, {}), { asString: true }), msg, "gzip({}) uses the default level");
    eq(gunzip(gzip(msg, { level: undefined }), { asString: true }), msg, "level: undefined is absent");
    const a = gzip(msg, 9), b = gzip(msg, { level: 9 });
    ok(a.length === b.length && a.join() === b.join(), "positional == options output");
    throws(() => gzip(msg, { level: "9" }), "options level must be a number");
    throws(() => gzip(msg, { level: null }), "options level null throws");
    throws(() => gzip(msg, "9"), "positional level must be a number");
}
});
__kitS.end();

print("test_compress: all tests passed (" + n + " assertions)");
