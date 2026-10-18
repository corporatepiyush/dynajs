// flags: --std
// gzip/deflate effort by level: levels 1-5 search shallower chains and switch
// from fixed to dynamic Huffman at 8 KiB; 6 and up search deep and are always
// dynamic. Every level must decode under a FOREIGN decoder (system gzip), and a
// Huffman tree deeper than 15 bits (RFC 1951 section 3.2.7 caps code lengths at
// 15) must be length-limited, not abandoned for fixed codes.
import { suite } from "../lib/kit.js";
import { gzip, gunzip, deflate, inflate } from "dyna:compress";
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

function systemGunzip(gz) {
    const zp = "tmp_gzip_effort.gz", op = "tmp_gzip_effort.out";
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

t.test("raw deflate round-trips at every level and size", (a) => {
    for (const n of SIZES) {
        const src = text(n);
        for (let level = 1; level <= 12; level++)
            a.ok(same(inflate(deflate(src, { level })), src), "n=" + n + " level=" + level);
    }
});

t.test("fast levels switch to dynamic Huffman exactly at 8 KiB", (a) => {
    for (const level of [1, 3, 5]) {
        a.eq(btype(gzip(text(8191), { level })), 1, "level " + level + " at 8191 is fixed");
        a.eq(btype(gzip(text(8192), { level })), 2, "level " + level + " at 8192 is dynamic");
    }
    a.eq(btype(gzip(text(300), { level: 6 })), 2, "level 6 is dynamic at any size");
});

t.test("deeper effort never loses much, and level 6+ output is one setting", (a) => {
    const src = text(120000);
    const sizes = [1, 2, 3, 4, 5, 6, 9].map((level) => gzip(src, { level }).length);
    a.ok(sizes[6] <= sizes[0], "level 9 (" + sizes[6] + ") is no larger than level 1 (" + sizes[0] + ")");
    a.eq(sizes[5], sizes[6], "levels 6 and 9 share the deepest search");
    for (let i = 1; i < sizes.length; i++)
        a.ok(sizes[i] <= sizes[i - 1] * 1.02, "level step " + i + " does not grow output by more than 2% (" + sizes + ")");
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
