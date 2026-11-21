// flags: --std
// covers: dyna:compress.lz4Compress
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_compress_lz4_compress");
import {
    gzip, gunzip, lz4Compress, lz4Decompress, lz4Frame, lz4Unframe, Compressor,
} from "dyna:compress";
import * as os from "os";
import * as std from "std";

const REQUIRE_TOOLS = (std.getenv("DYNAJS_REQUIRE_TOOLS") === "1");
let n = 0, skipped = 0;
function assert(cond, msg) {
    n++;
    if (!cond) throw new Error("assertion failed: " + msg);
}
function eq(got, want, msg) {
    n++;
    if (got !== want)
        throw new Error("assertion failed: " + msg + "\n  got:  " + got +
                        "\n  want: " + want);
}
function throws(fn, msg) {
    n++;
    try { fn(); } catch (e) { return; }
    throw new Error("assertion failed: " + msg + " did not throw");
}
function bytesEqual(a, b) {
    if (a.length !== b.length) return false;
    for (let i = 0; i < a.length; i++) if (a[i] !== b[i]) return false;
    return true;
}
function readFileBytes(path) {
    const [st, serr] = os.stat(path);
    if (serr) return null;
    const fd = os.open(path, os.O_RDONLY);
    if (fd < 0) return null;
    const buf = new Uint8Array(st.size);
    let got = 0;
    while (got < st.size) {
        const r = os.read(fd, buf.buffer, got, st.size - got);
        if (r <= 0) break;
        got += r;
    }
    os.close(fd);
    return got === st.size ? buf : null;
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

const enc = new TextEncoder();
const dec = new TextDecoder();

const corpora = {};
corpora.empty = new Uint8Array(0);
for (const k of [1, 4, 5, 11, 12, 13, 16, 64]) {
    const b = new Uint8Array(k);
    for (let i = 0; i < k; i++) b[i] = 65 + (i % 26);
    corpora["len" + k] = b;
}
corpora.runs = enc.encode("a".repeat(1000) + "b".repeat(1000) + "ab".repeat(500));
corpora.text = enc.encode("the quick brown fox jumps over the lazy dog. ".repeat(400));
corpora.json = enc.encode(JSON.stringify(
    Array.from({ length: 400 }, (_, i) => ({ jsonrpc: "2.0", id: i, method: "sub" }))));
fixture: {
    const r = new Uint8Array(20000);
    let x = 0x2545f491 >>> 0;
    for (let i = 0; i < r.length; i++) {
        x = (Math.imul(x, 1103515245) + 12345) >>> 0;
        r[i] = (x >>> 16) & 0xff;
    }
    corpora.random = r;
}
fixture: {
    const b = new Uint8Array(256 * 4);
    for (let i = 0; i < b.length; i++) b[i] = i & 0xff;
    corpora.allbytes = b;
}

for (const [name, data] of Object.entries(corpora)) {
    for (const level of [1, 3, 12]) {
        const packed = lz4Compress(data, { level });
        assert(packed instanceof Uint8Array, name + " returns Uint8Array");
        const back = lz4Decompress(packed);
        assert(bytesEqual(back, data), "block round trip " + name + " @" + level);
    }
}

__kitS.section("both levels decode with the same decoder", () => {
    const d = corpora.text;
    const a = lz4Compress(d, { level: 1 }), b = lz4Compress(d, { level: 12 });
    assert(bytesEqual(lz4Decompress(a), d) && bytesEqual(lz4Decompress(b), d),
           "both levels decode with the same decoder");
    assert(b.length <= a.length, "level 12 is not worse than level 1 (" +
           a.length + " -> " + b.length + ")");
});

__kitS.section("incompressible block does not expand:", () => {
    const r = corpora.random;
    const packed = lz4Compress(r);
    assert(packed.length <= r.length + r.length / 200 + 16,
           "incompressible block does not expand: " + r.length + " -> " + packed.length);
    const framed = lz4Frame(r);
    assert(framed.length <= r.length + 32,
           "incompressible frame stores rather than expands: " + framed.length);
    assert(bytesEqual(lz4Unframe(framed), r), "stored frame round trip");
});
__kitS.end();

console.log("test_lz4.js: " + n + " assertions passed" +
            (skipped ? " (" + skipped + " interop group skipped)" : ""));
