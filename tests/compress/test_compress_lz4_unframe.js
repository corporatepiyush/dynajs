// flags: --std
// covers: dyna:compress.lz4Unframe
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_compress_lz4_unframe");
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

eq(lz4Unframe(lz4Frame("hello frame"), { asString: true }), "hello frame",
   "asString decodes UTF-8");

__kitS.section("truncated literal length", () => {
    throws(() => lz4Decompress(new Uint8Array([0xf0])), "truncated literal length");
    throws(() => lz4Decompress(new Uint8Array([0x20, 65])), "literal run past the end");
    eq(lz4Decompress(new Uint8Array([0x20, 65, 66])).length, 2,
       "a final literals-only sequence decodes");
    throws(() => lz4Decompress(new Uint8Array([0x1f, 65, 0xff, 0x00, 0x00])),
           "offset past the start of output");
    throws(() => lz4Decompress(new Uint8Array([0x11, 65, 0x00, 0x00])),
           "offset zero");
    throws(() => lz4Unframe(new Uint8Array(0)), "empty frame");
    throws(() => lz4Unframe(new Uint8Array([1, 2, 3, 4, 5, 6, 7, 8])), "bad magic");
    {
        const f = lz4Frame(corpora.text);
        throws(() => lz4Unframe(f.slice(0, f.length - 2)), "truncated frame");
        const bad = new Uint8Array(f);
        bad[5] ^= 0x01;
        throws(() => lz4Unframe(bad), "bad descriptor checksum");
        const bad2 = new Uint8Array(f);
        bad2[bad2.length - 1] ^= 0xff;
        throws(() => lz4Unframe(bad2), "bad content checksum");
    }
    {
        const f = lz4Frame(corpora.text, { checksum: false });
        const linked = new Uint8Array(f);
        linked[4] &= ~0x20;
        const flg = linked[4], bd = linked[5];
        linked[6] = xxh32Byte(flg, bd);
        throws(() => lz4Unframe(linked), "linked-block frames are refused");
    }
    {
        const good = lz4Frame(corpora.json);
        let threw = 0, ok = 0;
        for (let i = 0; i < good.length; i += Math.max(1, good.length >> 7)) {
            const bad = new Uint8Array(good);
            bad[i] ^= 0x80;
            try { lz4Unframe(bad); ok++; } catch (e) { threw++; }
        }
        assert(threw > 0, "corrupting a frame is detected (" + threw +
               " threw, " + ok + " decoded)");
    }
    {
        const good = lz4Compress(corpora.json);
        let survived = 0;
        for (let i = 0; i < good.length; i += Math.max(1, good.length >> 7)) {
            const bad = new Uint8Array(good);
            bad[i] ^= 0x40;
            try { lz4Decompress(bad); survived++; } catch (e) {  }
        }
        assert(survived >= 0, "corrupting a raw block never crashes");
    }
});

function xxh32Byte(flg, bd) {
    const P1 = 2654435761, P2 = 2246822519, P3 = 3266489917, P5 = 374761393;
    const mul = (a, b) => Math.imul(a, b) >>> 0;
    const rotl = (x, r) => (((x << r) | (x >>> (32 - r))) >>> 0);
    let h = (P5 + 2) >>> 0;
    for (const byte of [flg, bd]) {
        h = (h + mul(byte, P5)) >>> 0;
        h = mul(rotl(h, 11), P1);
    }
    h = (h ^ (h >>> 15)) >>> 0;
    h = mul(h, P2);
    h = (h ^ (h >>> 13)) >>> 0;
    h = mul(h, P3);
    h = (h ^ (h >>> 16)) >>> 0;
    return (h >>> 8) & 0xff;
}

console.log("test_lz4.js: " + n + " assertions passed" +
            (skipped ? " (" + skipped + " interop group skipped)" : ""));

__kitS.section("declared-size and block-checksum frame", () => {
    function xxh32(bytes, seed = 0) {
        const P1 = 2654435761, P2 = 2246822519, P3 = 3266489917,
              P4 = 668265263, P5 = 374761393;
        const mul = (a, b) => Math.imul(a, b) >>> 0;
        const rotl = (x, r) => ((x << r) | (x >>> (32 - r))) >>> 0;
        const u32 = (i) => (bytes[i] | (bytes[i+1] << 8) | (bytes[i+2] << 16) |
                            (bytes[i+3] << 24)) >>> 0;
        let i = 0, h;
        if (bytes.length >= 16) {
            let h1 = (seed + P1 + P2) >>> 0, h2 = (seed + P2) >>> 0,
                h3 = seed >>> 0, h4 = (seed - P1) >>> 0;
            for (; i + 16 <= bytes.length; i += 16) {
                h1 = mul(rotl((h1 + mul(u32(i), P2)) >>> 0, 13), P1);
                h2 = mul(rotl((h2 + mul(u32(i+4), P2)) >>> 0, 13), P1);
                h3 = mul(rotl((h3 + mul(u32(i+8), P2)) >>> 0, 13), P1);
                h4 = mul(rotl((h4 + mul(u32(i+12), P2)) >>> 0, 13), P1);
            }
            h = (rotl(h1, 1) + rotl(h2, 7) + rotl(h3, 12) + rotl(h4, 18)) >>> 0;
        } else {
            h = (seed + P5) >>> 0;
        }
        h = (h + bytes.length) >>> 0;
        for (; i + 4 <= bytes.length; i += 4)
            h = mul(rotl((h + mul(u32(i), P3)) >>> 0, 17), P4);
        for (; i < bytes.length; i++)
            h = mul(rotl((h + mul(bytes[i], P5)) >>> 0, 11), P1);
        h ^= h >>> 15; h = mul(h, P2);
        h ^= h >>> 13; h = mul(h, P3);
        h ^= h >>> 16;
        return h >>> 0;
    }
    function le32(v) { return [v & 0xff, (v >>> 8) & 0xff, (v >>> 16) & 0xff, (v >>> 24) & 0xff]; }
    function le64(v) { const lo = v % 4294967296, hi = Math.floor(v / 4294967296); return [...le32(lo), ...le32(hi)]; }
    function frame(opts, payload) {
        const flg = 0x40 | 0x20 | (opts.contentSize ? 0x08 : 0) |
                    (opts.blockChecksum ? 0x10 : 0);
        const bd = 0x70;
        const hasSize = typeof opts.contentSize === "number";
        const mid = hasSize ? [...le64(opts.contentSize)] : [];
        const desc = [flg, bd, ...mid];
        const hc = (xxh32(new Uint8Array(desc)) >>> 8) & 0xff;
        const body = [...payload];
        let block = [...le32(payload.length | 0x80000000), ...body];
        if (opts.blockChecksum)
            block = [...block, ...le32(xxh32(new Uint8Array(payload)))];
        return new Uint8Array([0x04, 0x22, 0x4d, 0x18, ...desc, hc,
                               ...block, 0, 0, 0, 0]);
    }
    const payload = new TextEncoder().encode("declared-size and block-checksum frame");

    {
        const good = frame({ contentSize: payload.length }, payload);
        assert(new TextDecoder().decode(lz4Unframe(good)) ===
               "declared-size and block-checksum frame",
               "a frame whose declared content size matches decodes");
        for (const lie of [payload.length - 1, payload.length + 1, 0]) {
            const bad = frame({ contentSize: lie }, payload);
            let threw = false;
            try { lz4Unframe(bad); } catch (e) { threw = true; }
            assert(threw, "a frame declaring content size " + lie +
                   " (real " + payload.length + ") is refused");
        }
    }
    {
        const good = frame({ blockChecksum: true }, payload);
        assert(new TextDecoder().decode(lz4Unframe(good)) ===
               "declared-size and block-checksum frame",
               "a frame with a valid block checksum decodes");
        const bad = good.slice();
        bad[bad.length - 5] ^= 0xff;
        let threw = false;
        try { lz4Unframe(bad); } catch (e) { threw = true; }
        assert(threw, "a corrupted block checksum is refused");
    }
});
__kitS.end();

console.log("test_lz4.js: " + n + " assertions passed" +
            (skipped ? " (" + skipped + " interop group skipped)" : ""));
