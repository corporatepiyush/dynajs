/* test_compress.js — dyna:compress (in-repo gzip/gunzip, no external deps).
 * Run: dynajs (built with CONFIG_NATIVE_MODULES=y) tests/test_compress.js
 * Prints "test_compress: all tests passed" on success; throws on failure.
 *
 * Covers: round-trip (empty/small/repeated/random), cross-tool against the
 * system gzip/gunzip CLIs, malformed/truncated input handling, and the
 * level-selected dynamic-Huffman encoder (level >= 6). */

import { gzip, gunzip, zstd, unzstd, Compressor, ZipPack, TarExtract } from "dyna:compress";
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

/* --- 6. ZipPack refuses an entry count the format cannot write --- */
{
    function throws(fn, msg) {
        let threw = false;
        try { fn(); } catch { threw = true; }
        assert(threw, msg);
    }
    /* The EOCD records the entry count in a 16-bit field and no zip64 is
     * written, so 65536 entries would wrap into a silently corrupt archive:
     * refused at pack time, before any bytes are emitted. */
    const many = new Array(65536);
    for (let i = 0; i < many.length; i++)
        many[i] = { name: "f" + i, data: new Uint8Array(0) };
    throws(() => ZipPack(many),
           "65536 entries are refused (the EOCD count field is 16-bit)");
    const few = ZipPack([{ name: "a.txt", data: enc.encode("hi") }],
                        { method: "store" });
    assert(few instanceof Uint8Array, "a small ZipPack still works");
    /* the exact boundary is legal: 65535 is the largest count the field holds */
    const edge = new Array(65535);
    for (let i = 0; i < edge.length; i++)
        edge[i] = { name: "f" + i, data: "" };
    const t0 = performance.now();
    const big = ZipPack(edge, { method: "store" });
    assert(big instanceof Uint8Array && big.length > 65535 * 32,
           "65535 entries (the exact boundary) still pack");
    print("  ZipPack 65535 entries: " + (performance.now() - t0).toFixed(0) +
          " ms, " + (big.length / 1048576).toFixed(1) + " MB");
}

/* --- 7. a mistyped gzip level is refused, not silently ignored --- */
{
    function throws(fn, msg) {
        let threw = false;
        try { fn(); } catch { threw = true; }
        assert(threw, msg);
    }
    throws(() => gzip(corpora.small, "9"), "a string level is a TypeError");
    throws(() => gzip(corpora.small, { level: 9 }), "an options object is not a level");
    /* A numeric level is accepted; for a payload this tiny the block-type
     * comparison keeps the fixed block, so the bytes are the fixed ones --
     * which pins the replay-as-fixed path to the inline-fixed bytes. */
    assert(bytesEqual(gzip(corpora.small, 9), gzip(corpora.small)),
           "a numeric level is accepted (tiny payload: same fixed bytes)");
    assert(bytesEqual(gzip(corpora.small), gzip(corpora.small, undefined)),
           "an absent level still works");
}

/* --- 7b. level >= 6 selects the dynamic-Huffman encoder ------------------- *
 *
 * The entropy stage is the only thing level turns: same parse, same stored
 * fallback, and the block type is chosen by exact size comparison -- so a
 * higher level may never produce a LARGER output than a lower one, and the
 * win on statistics-skewed input must be real. deepcomb (shuffled Fibonacci
 * frequencies) is the one shape whose Huffman tree is deeper than 15 bits,
 * which is the only input that exercises the length-limit repair. */
{
    /* Shuffled Fibonacci counts over 28 symbols: no runs for the matcher to
     * collapse, so the literal histogram itself is a comb. */
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
        for (let i = flat.length - 1; i > 0; i--) { /* deterministic Fisher-Yates */
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
        const lo = gzip(corpora.english, 1), hi = gzip(corpora.english, 6);
        print("  level 6 vs 1 [english]: " + lo.length + " -> " + hi.length +
              " (" + (hi.length / lo.length).toFixed(3) + ")");
        assert(hi.length < lo.length * 0.95,
               "dynamic Huffman beats fixed by >5% on English text");
    }
    /* The dynamic stream must be readable by the system tool, not just by us:
     * english is a natural dynamic block, deepcomb carries a repaired
     * (length-limited) code. */
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
    /* The Compressor class takes the same level, with per-algo bounds. */
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

/* --- 8. Compressor.decompress takes {asString} like the one-shots --- */
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

/* --- 9. zstd failures carry the codec name (no raw library strings) --- */
{
    /* hand-encoded minimal zstd frame for "hello" (RFC 8878): magic, desc 0x00,
     * window descriptor 0x58, raw block "hello", no checksum */
    const fr = new Uint8Array([
        0x28, 0xB5, 0x2F, 0xFD, 0x00, 0x58, 0x29, 0x00, 0x00,
        0x68, 0x65, 0x6C, 0x6C, 0x6F]);
    let have = true;
    try {
        const dec = unzstd(fr);
        have = dec.length === 5 && dec[0] === 0x68;
    } catch { have = false; }
    if (!have) {
        print("  zstd not compiled in: skipping the codec-prefix check");
    } else {
        function msg(fn) {
            try { fn(); } catch (e) { return String(e); }
            return "";
        }
        const bad = fr.slice();
        bad[6] ^= 0xff;         /* the raw-block header; the payload itself
                                   carries no checksum and can "decode" */
        const m = msg(() => unzstd(bad));
        assert(m.indexOf("unzstd: zstd: ") >= 0,
               "a corrupt zstd stream is thrown as \"unzstd: zstd: <reason>\" (got " +
               JSON.stringify(m) + ")");
        const c = new Compressor({ algo: "zstd" });
        const m2 = msg(() => c.decompress(bad));
        assert(m2.indexOf("Compressor.decompress: zstd: ") >= 0,
               "the class path carries the same codec prefix (got " + JSON.stringify(m2) + ")");
    }
}


/* ---- tar PAX underflow regression ----
 * A PAX record whose '=' is the final byte of the archive made
 * vlen = len - eq - 2 wrap to SIZE_MAX and the size scan read past the
 * buffer (ASan heap-buffer-overflow at tar_pax_one, reproduced 2026-09-03).
 * The crafted archive below is byte-for-byte the repro: a well-formed
 * skipped record positions "7 size=" so '=' is the last byte. */
{
    const octal = (buf, off, n, val) => {
        const t = val.toString(8).padStart(n - 1, "0") + "\0";
        for (let i = 0; i < n; i++) buf[off + i] = t.charCodeAt(i);
    };
    const hdr = new Uint8Array(512);
    for (let i = 0; i < 3; i++) hdr[i] = "pax".charCodeAt(i);
    octal(hdr, 100, 8, 0o644);
    octal(hdr, 124, 12, 512);
    hdr[156] = 0x78;                                   /* 'x': PAX header */
    for (let i = 148; i < 156; i++) hdr[i] = 0x20;
    let sum = 0; for (const b of hdr) sum += b;
    const cs = sum.toString(8).padStart(6, "0") + "\0 ";
    for (let i = 0; i < 8; i++) hdr[148 + i] = cs.charCodeAt(i);
    const pax = new Uint8Array(512);
    pax.set([0x35,0x30,0x35,0x20,0x78,0x3d], 0);       /* "505 x=" ... */
    pax[504] = 0x0a;
    pax.set([0x37,0x20,0x73,0x69,0x7a,0x65,0x3d], 505); /* "7 size=" at end */
    const tar = new Uint8Array(1024);
    tar.set(hdr, 0); tar.set(pax, 512);
    let got;
    try { got = TarExtract(tar).length; } catch (e) { got = -1; }
    assert(got === 0, "PAX '=' at the final byte is skipped, not overread (got " + got + ")");
}

print("test_compress: all tests passed (" + n + " assertions)");
