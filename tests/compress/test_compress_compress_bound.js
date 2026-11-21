// flags: --std
// covers: dyna:compress.compressBound
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_compress_compress_bound");
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

__kitS.section("(got", () => {
    function eq(got, want, msg) {
        assert(got === want, msg + " (got " + got + ", want " + want + ")");
    }
    function ok(c, msg) { assert(c, msg); }
    function throws(fn, msg) {
        let threw = false;
        try { fn(); } catch { threw = true; }
        assert(threw, msg);
    }
    eq(compressBound(0, { algo: "gzip" }), 23, "gzip bound(0) = 18 + 5");
    eq(compressBound(65535, { algo: "gzip" }), 18 + 65535 + 5, "gzip bound(65535): one block");
    eq(compressBound(65536, { algo: "gzip" }), 18 + 65536 + 10, "gzip bound(65536): two blocks");
    eq(compressBound(131070, { algo: "gzip" }), 18 + 131070 + 10, "gzip bound(2 blocks exact)");
    eq(compressBound(0, { algo: "lz4" }), 16, "lz4 bound(0)");
    eq(compressBound(255, { algo: "lz4" }), 255 + 1 + 16, "lz4 bound(255)");
    eq(compressBound(510, { algo: "lz4" }), 510 + 2 + 16, "lz4 bound(510)");
    eq(compressBound(0, { algo: "lz4frame" }), 7 + 4 + 16 + 8 + 4, "lz4frame bound(0)");
    eq(compressBound(4 * 1024 * 1024, { algo: "lz4frame" }),
       7 + 1 * 4 + (4 * 1024 * 1024 + Math.floor(4 * 1024 * 1024 / 255) + 16) + 8 + 4,
       "lz4frame bound(one full block)");
    eq(compressBound(4 * 1024 * 1024 + 1, { algo: "lz4frame" }),
       7 + 2 * 4 + (4 * 1024 * 1024 + 1 + Math.floor((4 * 1024 * 1024 + 1) / 255) + 16) + 8 + 4,
       "lz4frame bound(block boundary + 1)");
    eq(compressBound(0, { algo: "snappy" }), 16, "snappy bound(0)");
    eq(compressBound(100, { algo: "snappy" }), 100 + 2 + 16, "snappy bound(100)");
    ok(compressBound(1000, { algo: "zstd" }) >= 1000, "zstd bound >= input");
    ok(compressBound(1000, { algo: "brotli" }) >= 1000, "brotli bound >= input");

    const incompressible = new Uint8Array(70000);
    let seed = 12345;
    for (let i = 0; i < incompressible.length; i++) {
        seed = (seed * 1103515245 + 12345) & 0x7fffffff;
        incompressible[i] = seed & 0xff;
    }
    for (const algo of ["gzip", "lz4", "lz4frame", "zstd", "brotli", "snappy"]) {
        const bound = compressBound(incompressible.length, { algo });
        let out;
        if (algo === "gzip") out = gzip(incompressible);
        else if (algo === "lz4") out = lz4Compress(incompressible);
        else if (algo === "lz4frame") out = lz4Frame(incompressible);
        else if (algo === "zstd") out = zstd(incompressible);
        else if (algo === "brotli") out = brotli(incompressible);
        else out = snappy(incompressible);
        ok(out.length <= bound, algo + " output " + out.length + " fits bound " + bound);
    }

    throws(() => compressBound(-1, { algo: "gzip" }), "negative n throws");
    throws(() => compressBound(1e18, { algo: "gzip" }), "absurd n throws");
    throws(() => compressBound(10, { algo: "nope" }), "unknown algo throws");
    throws(() => compressBound(10, {}), "missing algo throws");
    throws(() => compressBound(10, "gzip"), "non-object opts throws");
});
__kitS.end();

print("test_compress: all tests passed (" + n + " assertions)");
