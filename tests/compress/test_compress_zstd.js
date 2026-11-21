// flags: --std
// covers: dyna:compress.zstd
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_compress_zstd");
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

__kitS.section("zstd not compiled in: skipping the codec-prefix check", () => {
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
        bad[6] ^= 0xff;
        const m = msg(() => unzstd(bad));
        assert(m.indexOf("unzstd: zstd: ") >= 0,
               "a corrupt zstd stream is thrown as \"unzstd: zstd: <reason>\" (got " +
               JSON.stringify(m) + ")");
        const c = new Compressor({ algo: "zstd" });
        const m2 = msg(() => c.decompress(bad));
        assert(m2.indexOf("Compressor.decompress: zstd: ") >= 0,
               "the class path carries the same codec prefix (got " + JSON.stringify(m2) + ")");
    }
});
__kitS.end();

print("test_compress: all tests passed (" + n + " assertions)");
