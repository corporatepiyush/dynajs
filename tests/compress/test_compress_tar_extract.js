// flags: --std
// covers: dyna:compress.TarExtract
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_compress_tar_extract");
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

__kitS.section("PAX '=' at the final byte is skipped, not overread", () => {
    const octal = (buf, off, n, val) => {
        const t = val.toString(8).padStart(n - 1, "0") + "\0";
        for (let i = 0; i < n; i++) buf[off + i] = t.charCodeAt(i);
    };
    const hdr = new Uint8Array(512);
    for (let i = 0; i < 3; i++) hdr[i] = "pax".charCodeAt(i);
    octal(hdr, 100, 8, 0o644);
    octal(hdr, 124, 12, 512);
    hdr[156] = 0x78;
    for (let i = 148; i < 156; i++) hdr[i] = 0x20;
    let sum = 0; for (const b of hdr) sum += b;
    const cs = sum.toString(8).padStart(6, "0") + "\0 ";
    for (let i = 0; i < 8; i++) hdr[148 + i] = cs.charCodeAt(i);
    const pax = new Uint8Array(512);
    pax.set([0x35,0x30,0x35,0x20,0x78,0x3d], 0);
    pax[504] = 0x0a;
    pax.set([0x37,0x20,0x73,0x69,0x7a,0x65,0x3d], 505);
    const tar = new Uint8Array(1024);
    tar.set(hdr, 0); tar.set(pax, 512);
    let got;
    try { got = TarExtract(tar).length; } catch (e) { got = -1; }
    assert(got === 0, "PAX '=' at the final byte is skipped, not overread (got " + got + ")");
});
__kitS.end();

print("test_compress: all tests passed (" + n + " assertions)");
