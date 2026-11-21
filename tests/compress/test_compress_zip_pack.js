// flags: --std
// covers: dyna:compress.ZipPack
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_compress_zip_pack");
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

__kitS.section("65536 entries are refused (the EOCD count field is 16-bit)", () => {
    function throws(fn, msg) {
        let threw = false;
        try { fn(); } catch { threw = true; }
        assert(threw, msg);
    }
    const many = new Array(65536);
    for (let i = 0; i < many.length; i++)
        many[i] = { name: "f" + i, data: new Uint8Array(0) };
    throws(() => ZipPack(many),
           "65536 entries are refused (the EOCD count field is 16-bit)");
    const few = ZipPack([{ name: "a.txt", data: enc.encode("hi") }],
                        { method: "store" });
    assert(few instanceof Uint8Array, "a small ZipPack still works");
    const edge = new Array(65535);
    for (let i = 0; i < edge.length; i++)
        edge[i] = { name: "f" + i, data: "" };
    const t0 = performance.now();
    const big = ZipPack(edge, { method: "store" });
    assert(big instanceof Uint8Array && big.length > 65535 * 32,
           "65535 entries (the exact boundary) still pack");
    print("  ZipPack 65535 entries: " + (performance.now() - t0).toFixed(0) +
          " ms, " + (big.length / 1048576).toFixed(1) + " MB");
});
__kitS.end();

print("test_compress: all tests passed (" + n + " assertions)");
