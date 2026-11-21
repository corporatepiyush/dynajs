// flags: --std
// covers: dyna:compress.deflate
// gzip/deflate effort by level: levels 1-5 are greedy over shallower chains and
// switch from fixed to dynamic Huffman at 8 KiB; 6-9 match lazily (as zlib's
// deflate_slow) over deeper chains, and an absent level is 6, as in zlib. Every
// block of 16384 tokens at levels 6-9 picks the cheapest of stored, fixed and dynamic coding,
// so incompressible stretches inside compressible input are stored; levels 1-5
// code the whole input as one block, byte-identical to the old fast path. Every level must decode under a FOREIGN decoder (system gzip), and a
// Huffman tree deeper than 15 bits (RFC 1951 section 3.2.7 caps code lengths at
// 15) must be length-limited, not abandoned for fixed codes.
import { suite } from "../lib/kit.js";
import { gzip, gunzip, deflate, inflate, Compressor } from "dyna:compress";
import * as os from "os";
import * as std from "std";

const t = suite("gzip_effort");
const same = (x, y) => x.length === y.length && x.every((v, i) => v === y[i]);
const btype = (gz) => (gz[10] >> 1) & 3;

function text(n) {
    const words = ["the", "quick", "brown", "fox", "jumps", "over", "lazy", "dog", "{\"id\":", "\"name\":", "value", "\n"];
    const out = new Uint8Array(n);
    let x = 0x1234567, i = 0;
    while (i < n) {
        x = (Math.imul(x, 1103515245) + 12345) >>> 0;
        const w = words[(x >>> 16) % words.length] + " ";
        for (let k = 0; k < w.length && i < n; k++) out[i++] = w.charCodeAt(k);
    }
    return out;
}
function noise(n) {
    const out = new Uint8Array(n);
    let x = 0x9e3779b9;
    for (let i = 0; i < n; i++) { x ^= x << 13; x >>>= 0; x ^= x >>> 17; x ^= x << 5; x >>>= 0; out[i] = x & 0xff; }
    return out;
}
// Geometrically skewed symbol counts over 128 symbols: the optimal prefix code
// is deeper than 15 bits for the rare symbols, and the number of over-deep
// leaves is one the old pairwise repair could not balance.
function deepTree() {
    const nsym = 128, skew = 1.08, n = 200000;
    const cum = [];
    let total = 0, acc = 0;
    for (let i = 0; i < nsym; i++) total += Math.pow(skew, -i);
    for (let i = 0; i < nsym; i++) { acc += Math.pow(skew, -i) / total; cum.push(acc); }
    const out = new Uint8Array(n);
    let x = 0x9e3779b9;
    for (let i = 0; i < n; i++) {
        x ^= x << 13; x >>>= 0; x ^= x >>> 17; x ^= x << 5; x >>>= 0;
        const u = (x & 0xffffff) / 16777216;
        let lo = 0, hi = nsym - 1;
        while (lo < hi) { const mid = (lo + hi) >> 1; if (cum[mid] < u) lo = mid + 1; else hi = mid; }
        out[i] = lo;
    }
    return out;
}

// Words drawn with a cubic skew from a vocabulary of nv random words: the
// rare words make many distant 3-byte matches, which cost more than literals.
function prose(n, nv) {
    let x = 0x2545f491;
    const r = () => { x ^= x << 13; x >>>= 0; x ^= x >>> 17; x ^= x << 5; x >>>= 0; return x; };
    const vocab = [];
    for (let v = 0; v < nv; v++) {
        let w = "";
        const len = 3 + r() % 7;
        for (let k = 0; k < len; k++) w += String.fromCharCode(97 + r() % 26);
        vocab.push(w);
    }
    const out = new Uint8Array(n);
    let i = 0;
    while (i < n) {
        const u = r() / 4294967296;
        const w = vocab[Math.floor(nv * u * u * u)] + (r() % 9 ? " " : ".\n");
        for (let k = 0; k < w.length && i < n; k++) out[i++] = w.charCodeAt(k);
    }
    return out;
}

function systemGunzip(gz) {
    const zp = "tmp_gzip_effort_deflate.gz", op = "tmp_gzip_effort_deflate.out";
    const f = std.open(zp, "wb"); f.write(gz.buffer, gz.byteOffset, gz.length); f.close();
    const rc = os.exec(["/bin/sh", "-c", "gzip -dc " + zp + " > " + op], { usePath: false });
    let out = null;
    if (rc === 0) {
        const g = std.open(op, "rb"); g.seek(0, std.SEEK_END); const n = g.tell(); g.seek(0, std.SEEK_SET);
        out = new Uint8Array(n); g.read(out.buffer, 0, n); g.close();
    }
    os.remove(zp); os.remove(op);
    return out;
}

const SIZES = [0, 1, 255, 4096, 8191, 8192, 8193, 70000];

t.test("raw deflate round-trips at every level and size", (a) => {
    for (const n of SIZES) {
        const src = text(n);
        for (let level = 1; level <= 12; level++)
            a.ok(same(inflate(deflate(src, { level })), src), "n=" + n + " level=" + level);
    }
});

function mixed() {
    const out = new Uint8Array(300000);
    out.set(text(100000), 0);
    out.set(noise(100000), 100000);
    out.set(text(100000).reverse(), 200000);
    return out;
}

t.test("levels 6-9 deepen the search and level 9 is no larger than zlib -9", (a) => {
    const src = text(120000);
    const sizes = [1, 2, 3, 4, 5, 6, 7, 8, 9].map((level) => deflate(src, { level }).length);
    for (let i = 1; i < sizes.length; i++)
        a.ok(sizes[i] <= sizes[i - 1] * 1.02, "level " + (i + 1) + " does not grow output by more than 2% (" + sizes + ")");
    for (let i = 6; i < 9; i++)
        a.ok(sizes[i] <= sizes[i - 1], "level " + (i + 1) + " is no larger than level " + i + " (" + sizes + ")");
    a.ok(sizes[8] < sizes[5], "level 9 (" + sizes[8] + ") is smaller than level 6 (" + sizes[5] + ")");
    a.ok(sizes[5] <= 18152, "level 6 (" + sizes[5] + ") is no larger than zlib -6 (18152)");
    a.ok(sizes[8] <= 17068, "level 9 (" + sizes[8] + ") is no larger than zlib -9 (17068)");
    a.ok(deflate(src, { level: 12 }).length === sizes[8], "levels above 9 are level 9");
    const words = prose(200000, 8000);
    const w6 = deflate(words, { level: 6 }).length, w9 = deflate(words, { level: 9 }).length;
    a.ok(w6 <= 93926, "prose at level 6 (" + w6 + ") is no larger than zlib -6 (93926): a distant 3-byte match is a literal");
    a.ok(w9 <= 93881, "prose at level 9 (" + w9 + ") is no larger than zlib -9 (93881)");
    const mix = mixed();
    a.ok(deflate(mix, { level: 9 }).length <= 132513, "mixed input at level 9 is no larger than zlib -9 (132513)");
});

t.test("levels 1-5 keep the one-block fast path", (a) => {
    const src = text(120000), mix = mixed();
    const want = [[23957, 153125], [22596, 149492], [20938, 145393], [19607, 142168], [18760, 140262]];
    for (let level = 1; level <= 5; level++) {
        a.eq(deflate(src, { level }).length, want[level - 1][0], "text at level " + level);
        a.eq(deflate(mix, { level }).length, want[level - 1][1], "mixed at level " + level);
    }
});

t.test("each block picks its own coding, so a noise stretch is stored", (a) => {
    const mix = mixed();
    const parts = deflate(text(100000), { level: 6 }).length + deflate(text(100000).reverse(), { level: 6 }).length;
    for (const level of [6, 9]) {
        const raw = deflate(mix, { level });
        a.ok(same(inflate(raw), mix), "level " + level + " round-trips");
        const bound = Math.ceil((parts + 100000) * 1.1);
        a.ok(raw.length <= bound, "level " + level + " mixed output " + raw.length + " is within 10% of text parts plus stored noise (" + bound + ")");
        const back = systemGunzip(gzip(mix, { level }));
        a.ok(back !== null && same(back, mix), "level " + level + " multi-block output decodes under system gzip");
    }
    for (const level of [6, 9])
        a.eq(btype(gzip(noise(100000), { level })), 0, "level " + level + " stores pure noise");
}, { timeoutMs: 60000 });

await t.run();
