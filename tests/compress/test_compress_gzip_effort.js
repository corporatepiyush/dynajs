// flags: --std
// covers: dyna:compress.gzip
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
    const zp = "tmp_gzip_effort_text.gz", op = "tmp_gzip_effort_text.out";
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

for (const [name, make] of [["text", text], ["noise", noise]]) {
    t.test("every level of " + name + " decodes under system gzip", (a) => {
        for (const n of SIZES) {
            const src = make(n);
            for (let level = 1; level <= 9; level++) {
                const gz = gzip(src, { level });
                const back = systemGunzip(gz);
                a.ok(back !== null && same(back, src), name + " n=" + n + " level=" + level);
            }
        }
    }, { timeoutMs: 60000 });
}

t.test("fast levels switch to dynamic Huffman exactly at 8 KiB", (a) => {
    for (const level of [1, 3, 5]) {
        a.eq(btype(gzip(text(8191), { level })), 1, "level " + level + " at 8191 is fixed");
        a.eq(btype(gzip(text(8192), { level })), 2, "level " + level + " at 8192 is dynamic");
    }
    a.eq(btype(gzip(text(300), { level: 6 })), 2, "level 6 is dynamic at any size");
});

function mixed() {
    const out = new Uint8Array(300000);
    out.set(text(100000), 0);
    out.set(noise(100000), 100000);
    out.set(text(100000).reverse(), 200000);
    return out;
}

t.test("an absent level is level 6 for gzip, deflate and the gzip Compressor", (a) => {
    const src = text(120000);
    const g6 = gzip(src, { level: 6 }), d6 = deflate(src, { level: 6 });
    a.ok(!same(g6, gzip(src, { level: 1 })), "level 6 differs from level 1 on this input");
    a.ok(same(gzip(src), g6), "gzip(src) equals level 6");
    a.ok(same(gzip(src, {}), g6), "gzip(src, {}) equals level 6");
    a.ok(same(deflate(src), d6), "deflate(src) equals level 6");
    a.ok(same(new Compressor({ algo: "gzip" }).compress(src), g6), "gzip Compressor without level equals level 6");
});

t.test("a tree deeper than 15 bits is length-limited, not dropped to fixed codes", (a) => {
    const src = deepTree();
    for (const level of [1, 3, 6]) {
        const gz = gzip(src, { level });
        a.eq(btype(gz), 2, "level " + level + " stays dynamic");
        a.ok(gz.length < src.length * 0.8, "level " + level + " output " + gz.length + " is well under the fixed-code size");
        a.ok(same(gunzip(gz), src), "level " + level + " round-trips");
        const back = systemGunzip(gz);
        a.ok(back !== null && same(back, src), "level " + level + " decodes under system gzip");
    }
}, { timeoutMs: 60000 });

await t.run();
