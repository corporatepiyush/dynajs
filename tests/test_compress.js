/* test_compress.js — dyna:compress (in-repo gzip/gunzip, no external deps).
 * Run: dynajs (built with CONFIG_NATIVE_MODULES=y) tests/test_compress.js
 * Prints "test_compress: all tests passed" on success; throws on failure.
 *
 * Covers: round-trip (empty/small/repeated/random), cross-tool against the
 * system gzip/gunzip CLIs, and malformed/truncated input handling. */

import { gzip, gunzip } from "dyna:compress";
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

/* Read a whole file as a Uint8Array (binary-safe via os.open/os.read). */
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

/* Write a Uint8Array to a file (binary-safe). */
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

/* --- test corpora --- */
const enc = new TextEncoder();
const englishText =
    "The quick brown fox jumps over the lazy dog. Pack my box with five " +
    "dozen liquor jugs. How vexingly quick daft zebras jump! Sphinx of " +
    "black quartz, judge my vow. The five boxing wizards jump quickly. ";
const corpora = {
    empty: new Uint8Array(0),
    oneByte: enc.encode("Z"),
    small: enc.encode("hello, gzip world!"),
    repeated: enc.encode("abcdefgh".repeat(4000)),      /* highly compressible */
    newlines: enc.encode("line\n".repeat(1000)),
    english: enc.encode(englishText.repeat(80)),        /* natural-language text */
};
/* pseudo-random (LCG) — incompressible, exercises stored-block fallback path */
{
    const r = new Uint8Array(5000);
    let x = 0x12345678 >>> 0;
    for (let i = 0; i < r.length; i++) {
        x = (Math.imul(x, 1103515245) + 12345) >>> 0;
        r[i] = (x >>> 16) & 0xff;
    }
    corpora.random = r;
}
/* binary data with embedded NULs — must survive round-trip byte-for-byte */
{
    const b = new Uint8Array(3000);
    for (let i = 0; i < b.length; i++)
        b[i] = (i % 7 === 0) ? 0x00 : ((i * 31 + (i >> 3)) & 0xff);
    corpora.binaryNul = b;
}

/* --- 1. round-trip: gunzip(gzip(x)) === x --- */
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

/* --- 1b. real compression ratio: text + repetitive data must shrink --- */
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
    /* incompressible input must not expand meaningfully (stored fallback). */
    const rr = ratio("random");
    assert(rr < 1.02, "random data does not blow up (stored fallback)");
}

/* --- 1c. throughput: report MB/s of gzip() (real deflate) --- */
{
    const data = corpora.english;      /* representative text workload */
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

/* --- 2. gzip accepts string and ArrayBuffer inputs; asString decode --- */
{
    const text = "The quick brown fox jumps over the lazy dog. ".repeat(50);
    const packedStr = gzip(text);
    const decoded = gunzip(packedStr, { asString: true });
    assert(typeof decoded === "string", "asString yields a string");
    assert(decoded === text, "string round-trip via asString");

    const ab = enc.encode(text).buffer;               /* ArrayBuffer input */
    const packedAb = gunzip(gzip(ab), { asString: true });
    assert(packedAb === text, "ArrayBuffer input round-trips");
}

/* --- 3. cross-tool (a): my gzip() output decodes with system gzip -dc --- */
/* Covers real fixed-Huffman output (repeated/english/binaryNul) + stored
 * fallback (random): the SYSTEM tool must reproduce the exact input. */
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

/* --- 4. cross-tool (b): system gzip output decodes with our gunzip --- */
{
    const data = corpora.newlines;
    const inPath = "tmp_compress_b.in";
    const gzPath = "tmp_compress_b.gz";
    writeFileBytes(inPath, data);
    /* system gzip emits dynamic-Huffman blocks — exercises full inflate */
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

/* --- 5. malformed / truncated input throws cleanly (no crash / no UAF) --- */
{
    function throws(fn, msg) {
        let threw = false;
        try { fn(); } catch { threw = true; }
        assert(threw, msg);
    }
    throws(() => gunzip(u8()), "empty input throws");
    throws(() => gunzip(u8(1, 2, 3)), "too-short input throws");
    throws(() => gunzip(u8(0x1f, 0x8b, 0x08, 0, 0, 0, 0, 0, 0, 0xff)),
           "header-only (no deflate/trailer) throws");
    throws(() => gunzip(u8(0x00, 0x00, 0x08, 0, 0, 0, 0, 0, 0, 0xff, 1, 2, 3, 4,
                          5, 6, 7, 8)), "bad magic throws");

    /* Truncate a valid gzip at every prefix length — none may crash. */
    const good = gzip(corpora.small);
    for (let cut = 0; cut < good.length; cut++) {
        throws(() => gunzip(good.slice(0, cut)),
               "truncated @" + cut + " throws");
    }

    /* Corrupt each byte of a valid gzip (flip a bit) — must not crash; either
     * throws (CRC/format) or, rarely, decodes to something (never a crash). */
    for (let i = 0; i < good.length; i++) {
        const bad = good.slice();
        bad[i] ^= 0xff;
        try { gunzip(bad); } catch { /* expected for most corruptions */ }
    }
    /* Corrupt just the deflate body of a larger, compressible member. */
    const big = gzip(corpora.repeated);
    for (let i = 12; i < big.length - 8; i += 7) {
        const bad = big.slice();
        bad[i] ^= 0x55;
        try { gunzip(bad); } catch { /* expected */ }
    }
}

print("test_compress: all tests passed (" + n + " assertions)");
