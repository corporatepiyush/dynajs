// covers: contract dyna:compress.inflate dyna:compress.gunzip -- one decoder loop behind both: the inflate fast loop must decode exactly as the bit-by-bit reference
// Inflate's symbol loop now has a fast path while at least 8 input bytes remain: one refill to 56 bits, then up to
// three literals or one length/distance pair decoded without per-field bit checks (each field fits the refilled
// bits); codes longer than the 9-bit root table, the last 8 input bytes and every error keep the old checked path.
// Oracle: an independent bit-at-a-time inflater in JS (puff's canonical decoding, same validity rules: complete
// code-length code, incomplete literal/distance codes only with one code, end-of-block code present, length symbol
// < 286, distance symbol < 30, distance within the output). Valid streams from every level (fixed, dynamic with codes
// longer than 9 bits, stored) at lengths around the 8-byte tail, plus thousands of corrupted ones (bit flips, byte
// changes, truncation, random bytes): native and JS must both fail or both give the same bytes (bytes after the final
// block are an error for inflate). Crafted fixed blocks put the end-of-block code right after one, two or three
// literals, and streams cut 1..8 bytes short inside a larger buffer must not decode to the full output.
// Both 8-byte guards (dyn_refill and the dyn_inflate_codes fast loop: s->pos + 8 <= s->len) are proven only under the
// ASan build (.obj-san/dynajs): with either guard at + 4 this suite aborts there with a heap-buffer-overflow READ of
// size 8 (dyn_ld64le), while the release build stays green because the over-read lands in malloc slack (F-cmp-1).
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_compress_inflate_fast_loop_oracle");
import { deflate, inflate, gzip, gunzip } from "dyna:compress";

let checks = 0, fails = 0;
function ok(c, what) { checks++; if (!c) { fails++; if (fails < 15) print("FAIL: " + what); } }
let seed = 4242;
const rnd = () => ((seed = (Math.imul(seed, 1103515245) + 12345) >>> 0) / 4294967296);
const LBASE = [3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258];
const LEXT = [0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0];
const DBASE = [1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577];
const DEXT = [0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13];
function puff(src, limit, rec) {
    let pos = 0, bitbuf = 0, bitcnt = 0;
    const out = [];
    const fail = (m) => { throw new Error("puff: " + m); };
    const bits = (n) => { let v = bitbuf; while (bitcnt < n) { if (pos >= src.length) fail("out of input"); v |= src[pos++] << bitcnt; bitcnt += 8; } bitbuf = v >>> n; bitcnt -= n; return v & ((1 << n) - 1); };
    const build = (len, n) => {
        const count = new Array(16).fill(0), symbol = [];
        for (let s = 0; s < n; s++) count[len[s]]++;
        if (count[0] === n) return { count, symbol, left: 0 };
        let left = 1;
        for (let l = 1; l <= 15; l++) { left <<= 1; left -= count[l]; if (left < 0) return { count, symbol, left }; }
        const offs = [0, 0]; for (let l = 1; l < 15; l++) offs[l + 1] = offs[l] + count[l];
        for (let s = 0; s < n; s++) if (len[s]) symbol[offs[len[s]]++] = s;
        return { count, symbol, left };
    };
    const decode = (h) => {
        let code = 0, first = 0, index = 0;
        for (let l = 1; l <= 15; l++) {
            code |= bits(1);
            const c = h.count[l];
            if (code - c < first) return h.symbol[index + (code - first)];
            index += c; first += c; first <<= 1; code <<= 1;
        }
        fail("invalid code");
    };
    const codes = (lh, dh, blk) => {
        for (;;) {
            const sym = decode(lh);
            if (blk) blk.lf[sym]++;
            if (sym < 256) { out.push(sym); if (rec) rec.push(sym); if (out.length > limit) fail("limit"); continue; }
            if (sym === 256) return;
            const is = sym - 257;
            if (is >= 29) fail("bad length symbol");
            const len = LBASE[is] + bits(LEXT[is]);
            const ds = decode(dh);
            if (blk) blk.df[ds]++;
            if (ds >= 30) fail("bad distance symbol");
            const dist = DBASE[ds] + bits(DEXT[ds]);
            if (dist > out.length) fail("distance too far");
            if (rec) rec.push([len, dist]);
            for (let k = 0; k < len; k++) out.push(out[out.length - dist]);
            if (out.length > limit) fail("limit");
        }
    };
    const fl = []; for (let s = 0; s < 288; s++) fl[s] = s < 144 ? 8 : s < 256 ? 9 : s < 280 ? 7 : 8;
    const fixL = build(fl, 288), fixD = build(new Array(30).fill(5), 30);
    const blocks = [];
    let last;
    do {
        last = bits(1);
        const type = bits(2);
        if (type === 0) {
            bitbuf = 0; bitcnt = 0;
            if (pos + 4 > src.length) fail("out of input");
            const len = src[pos] | (src[pos + 1] << 8), nlen = src[pos + 2] | (src[pos + 3] << 8);
            pos += 4;
            if ((len ^ 0xffff) !== nlen) fail("stored length");
            if (pos + len > src.length) fail("out of input");
            for (let k = 0; k < len; k++) out.push(src[pos++]);
            if (out.length > limit) fail("limit");
            blocks.push({ type: 0 });
        } else if (type === 1) {
            const blk = { type: 1, lf: new Array(288).fill(0), df: new Array(30).fill(0) };
            codes(fixL, fixD, blk); blocks.push(blk);
        } else if (type === 2) {
            const order = [16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15];
            const nlen = bits(5) + 257, ndist = bits(5) + 1, ncode = bits(4) + 4;
            if (nlen > 286 || ndist > 30) fail("bad counts");
            const lengths = new Array(320).fill(0);
            for (let k = 0; k < ncode; k++) lengths[order[k]] = bits(3);
            const lc = build(lengths, 19);
            if (lc.left !== 0) fail("incomplete code-length code");
            let idx = 0;
            while (idx < nlen + ndist) {
                let sym = decode(lc);
                if (sym < 16) lengths[idx++] = sym;
                else {
                    let len = 0, rep;
                    if (sym === 16) { if (idx === 0) fail("repeat with no first"); len = lengths[idx - 1]; rep = 3 + bits(2); }
                    else if (sym === 17) rep = 3 + bits(3);
                    else rep = 11 + bits(7);
                    if (idx + rep > nlen + ndist) fail("too many lengths");
                    while (rep--) lengths[idx++] = len;
                }
            }
            if (lengths[256] === 0) fail("no end-of-block code");
            const ll = lengths.slice(0, nlen), dl = lengths.slice(nlen, nlen + ndist);
            const lh = build(ll, nlen);
            if (lh.left && (lh.left < 0 || nlen !== lh.count[0] + lh.count[1])) fail("incomplete literal code");
            const dh = build(dl, ndist);
            if (dh.left && (dh.left < 0 || ndist !== dh.count[0] + dh.count[1])) fail("incomplete distance code");
            const blk = { type: 2, ll, dl, lf: new Array(288).fill(0), df: new Array(30).fill(0) };
            codes(lh, dh, blk); blocks.push(blk);
        } else fail("block type 3");
    } while (!last);
    return { out: Uint8Array.from(out), blocks, used: pos };
}
const same = (a, b) => a.length === b.length && a.every((v, i) => v === b[i]);
function corpus() {
    const list = [];
    const words = ["alpha", "beta", "gamma", "delta", "{\"id\":", "\"v\":", "1234", ",", "\n", " ", "über", "x"];
    for (const n of [0, 1, 2, 3, 7, 8, 9, 15, 16, 17, 31, 64, 100, 255, 256, 300, 1000, 4096, 9000, 20000, 70000]) {
        let s = ""; while (s.length < n) s += words[(rnd() * rnd() * words.length) | 0];
        list.push(new TextEncoder().encode(s.slice(0, n)));
        const skew = new Uint8Array(n); for (let i = 0; i < n; i++) skew[i] = (Math.floor(256 * rnd() * rnd() * rnd())) & 255; list.push(skew);
        const rep = new Uint8Array(n); for (let i = 0; i < n; i++) rep[i] = i % 5 === 0 ? (rnd() * 256) | 0 : rep[Math.max(0, i - 1 - ((rnd() * 40) | 0))] | 0; list.push(rep);
    }
    return list;
}
const data = corpus();
function cmpNative(stream, what) {
    let jr, nr;
    try { const r = puff(stream, 300000); jr = r.used === stream.length ? r.out : null; } catch (e) { jr = null; }
    try { nr = inflate(stream, { maxOutputBytes: 300000 }); } catch (e) { nr = null; }
    ok((jr === null) === (nr === null) && (jr === null || same(jr, nr)), what + ": native " + (nr ? nr.length : "error") + " vs JS " + (jr ? jr.length : "error"));
}
__kitS.section("valid streams of every level decode like the JS reference", () => {
    let longCodes = 0;
    for (const [k, d] of data.entries()) for (const lv of [1, 3, 6, 9]) {
        const f = deflate(d, { level: lv });
        const ref = puff(f, 1e7);
        ok(same(ref.out, d), "JS reference round trip " + k + "/" + lv);
        ok(same(inflate(f), d), "inflate round trip " + k + "/" + lv);
        ok(same(gunzip(gzip(d, lv)), d), "gunzip round trip " + k + "/" + lv);
        for (const b of ref.blocks) if (b.type === 2 && b.ll.some((l) => l > 9)) longCodes++;
    }
    ok(longCodes > 10, "streams with codes longer than the 9-bit root: " + longCodes);
});
__kitS.section("corrupted streams fail or decode exactly as the JS reference", () => {
    for (const [k, d] of data.entries()) {
        if (d.length > 20000) continue;
        const f = deflate(d, { level: [1, 6, 9][k % 3] });
        for (let t = 0; t < 12; t++) {
            const c = f.slice();
            const m = t % 4;
            if (c.length && m === 0) c[(rnd() * c.length) | 0] ^= 1 << ((rnd() * 8) | 0);
            if (c.length && m === 1) c[(rnd() * c.length) | 0] = (rnd() * 256) | 0;
            if (c.length && m === 3) { const p = (rnd() * c.length) | 0; for (let q = p; q < Math.min(c.length, p + 4); q++) c[q] = (rnd() * 256) | 0; }
            cmpNative(m === 2 ? c.subarray(0, (rnd() * c.length) | 0) : c, "corrupt " + k + "/" + t);
        }
    }
    for (let t = 0; t < 1500; t++) {
        const n = (rnd() * 64) | 0, b = new Uint8Array(n);
        for (let i = 0; i < n; i++) b[i] = (rnd() * 256) | 0;
        if (n) b[0] = (b[0] & ~6) | (t % 2 ? 2 : 4);
        cmpNative(b, "random " + t);
    }
});
function fixedBlocks(groups) {
    const bytes = []; let acc = 0, n = 0;
    const put = (v, len) => { for (let i = 0; i < len; i++) { acc |= ((v >> i) & 1) << n; if (++n === 8) { bytes.push(acc); acc = 0; n = 0; } } };
    const code = (c, len) => { for (let i = len - 1; i >= 0; i--) put((c >> i) & 1, 1); };
    const lit = (v) => (v < 144 ? code(0x30 + v, 8) : code(0x190 + v - 144, 9));
    groups.forEach((g, k) => { put(k === groups.length - 1 ? 1 : 0, 1); put(1, 2); for (const v of g) lit(v); code(0, 7); });
    if (n) bytes.push(acc);
    return Uint8Array.from(bytes);
}
__kitS.section("end-of-block right after one or two literals inside the fast loop", () => {
    for (const per of [1, 2, 3, 4]) for (const hi of [false, true]) {
        const groups = [];
        for (let k = 0; k < 120; k++) groups.push(Array.from({ length: per }, (_, i) => (hi ? 150 + ((k * 7 + i) % 100) : 32 + ((k * 5 + i) % 90))));
        const s = fixedBlocks(groups);
        const want = groups.flat();
        const ref = puff(s, 1e6).out;
        ok(same(ref, want), "JS reference decodes the crafted blocks (" + per + ")");
        let got; try { got = inflate(s); } catch (e) { got = e.message; }
        ok(typeof got !== "string" && same(got, want), per + " literal(s) per block, high=" + hi + ": " + (typeof got === "string" ? got : got.length));
    }
});
__kitS.section("a stream cut short inside a larger buffer is refused", () => {
    for (const [k, d] of data.entries()) {
        if (d.length < 50 || d.length > 20000) continue;
        const f = deflate(d, { level: [1, 6, 9][k % 3] });
        for (const cut of [1, 2, 3, 4, 5, 8]) {
            if (f.length <= cut) continue;
            let r; try { r = inflate(f.subarray(0, f.length - cut)); } catch (e) { r = null; }
            ok(r === null || !same(r, d), "cut " + cut + " of " + f.length + " (" + k + ") is not the full output");
            let g; const gz = gzip(d, 6); try { g = gunzip(gz.subarray(0, gz.length - 8 - cut)); } catch (e) { g = null; }
            ok(g === null, "gzip body cut " + cut + " (" + k + ") is refused");
        }
    }
});
print("inflate fast loop oracle: " + checks + " checks, " + fails + " failed");
if (fails) throw new Error(fails + " failed");
__kitS.end();
