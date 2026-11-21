// covers: contract dyna:compress.gzip dyna:compress.deflate -- one encoder behind both: Huffman code lengths and the lazy LZ parse must be unchanged
// Two encoder paths changed. dyn_huff_lengths built the Huffman tree with a binary heap ordered by (frequency, node
// index); it now sorts the leaves once and merges them with a FIFO of internal nodes (the same pop order: internal
// nodes are created with non-decreasing weight and rising index), and computes depths top-down. The lazy matcher
// (levels 6..9) now rejects a chain candidate with one 2-byte compare at the current best end plus the first byte (a
// necessary condition for a longer match) instead of three byte compares. Oracle, independent of both: a JS
// bit-at-a-time inflater records every dynamic block's transmitted code lengths and the symbols it decodes; a JS
// Huffman built by repeated minimum selection (ties by lower index, the same depth clamp at 15 / 7 bits and the
// encoder's one-symbol padding) must give exactly the transmitted literal, distance and code-length lengths; and a JS
// port of the lazy parse (hash3, chain limits, good/nice/lazy, the 4 KiB too-far rule) must give exactly the decoded
// token sequence. Inputs: text, skewed bytes, Fibonacci-weighted bytes, repeats, 9..100000 bytes, levels 6..9; some
// code-length trees go past 7 bits, so the depth clamp runs. gzip must carry the same deflate body (bytes 10..-8).
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_compress_gzip_huffman_lazy_oracle");
import { deflate, gzip } from "dyna:compress";

let checks = 0, fails = 0;
function ok(c, what) { checks++; if (!c) { fails++; if (fails < 15) print("FAIL: " + what); } }
let seed = 777;
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
            const clLens = lengths.slice(0, 19), clf = new Array(19).fill(0);
            const lc = build(lengths, 19);
            if (lc.left !== 0) fail("incomplete code-length code");
            let idx = 0;
            while (idx < nlen + ndist) {
                let sym = decode(lc);
                clf[sym]++;
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
            const blk = { type: 2, ll, dl, clLens, clf, lf: new Array(288).fill(0), df: new Array(30).fill(0) };
            codes(lh, dh, blk); blocks.push(blk);
        } else fail("block type 3");
    } while (!last);
    return { out: Uint8Array.from(out), blocks, used: pos };
}
function jsHuff(freq, n, maxbits) {
    const lens = new Array(n).fill(0), f = [], syms = [];
    for (let i = 0; i < n; i++) if (freq[i]) { f.push(freq[i]); syms.push(i); }
    const nsym = f.length;
    if (nsym === 0) return lens;
    if (nsym === 1) { lens[syms[0]] = 1; return lens; }
    const parent = [], alive = new Set(f.map((_, i) => i));
    const takeMin = () => { let b = -1; for (const x of alive) if (b < 0 || f[x] < f[b] || (f[x] === f[b] && x < b)) b = x; alive.delete(b); return b; };
    while (alive.size > 1) { const a = takeMin(), b = takeMin(), t = f.length; f.push(f[a] + f[b]); parent[a] = t; parent[b] = t; alive.add(t); }
    const blc = new Array(maxbits + 1).fill(0), depth = [];
    let overflow = 0;
    for (let i = 0; i < nsym; i++) { let d = 0, x = i; while (parent[x] !== undefined) { x = parent[x]; d++; } if (d > maxbits) { overflow++; d = maxbits; } depth[i] = d; blc[d]++; }
    if (!overflow) { for (let i = 0; i < nsym; i++) lens[syms[i]] = depth[i]; return lens; }
    let clamped = 0; const full = 2 ** maxbits;
    for (let b = 1; b <= maxbits; b++) clamped += blc[b] * 2 ** (maxbits - b);
    while (clamped > full) { let b = maxbits - 1; while (b > 0 && blc[b] === 0) b--; blc[b]--; blc[b + 1] += 2; blc[maxbits]--; clamped--; }
    const order = [...Array(nsym).keys()].sort((x, y) => f[y] - f[x] || x - y);
    let pos = 0;
    for (let b = 1; b <= maxbits; b++) for (let c = 0; c < blc[b]; c++) lens[syms[order[pos++]]] = b;
    return lens;
}
function pad1(fr) { let used = 0, first = -1; fr.forEach((v, i) => { if (v) { used++; if (first < 0) first = i; } }); if (used < 2) fr[first === 0 ? 1 : 0]++; return fr; }
function padD(fr) { let used = 0, first = -1; fr.forEach((v, i) => { if (v) { used++; if (first < 0) first = i; } }); if (used === 0) { fr[0]++; fr[1]++; } else if (used === 1) fr[first === 0 ? 1 : 0]++; return fr; }
function padCL(fr) { const used = fr.filter((v) => v).length; if (used < 2) fr[fr[0] ? 1 : 0]++; return fr; }
const EFFORT = { 6: [128, 128, 8, 16], 7: [256, 128, 8, 32], 8: [1024, 258, 32, 128], 9: [4096, 258, 32, 258] };
function jsLazy(src, level) {
    const len = src.length, [chainMax, niceE, good, lazy] = EFFORT[level];
    let bits = 15; while (bits > 8 && 2 ** (bits - 1) >= len + 1) bits--;
    const shift = 32 - bits, head = new Int32Array(1 << bits), prev = new Uint16Array(65536), toks = [];
    const hash = (k) => (Math.imul(src[k] | (src[k + 1] << 8) | (src[k + 2] << 16), 2654435761 | 0) >>> shift);
    const insert = (k) => { const h = hash(k), hv = head[h]; const g = hv >= 1 ? k - (hv - 1) : 0; prev[k & 65535] = g <= 65535 ? g : 0; head[h] = k + 1; return hv; };
    const mlen = (a, b, max) => { let m = 0; while (m < max && src[a + m] === src[b + m]) m++; return m; };
    let i = 0, prevLen = 2, prevDist = 0, avail = false;
    while (i <= len) {
        let curLen = 2, curDist = 0;
        if (i + 3 <= len) {
            const hv = insert(i);
            const maxLen = Math.min(len - i, 258);
            if (hv >= 1 && prevLen < lazy && prevLen < maxLen) {
                let cur = hv - 1, dist = i - cur, best = prevLen;
                let chain = prevLen >= good ? chainMax >> 2 : chainMax;
                const nice = Math.min(niceE, maxLen);
                while (dist <= 32768) {
                    const g = prev[cur & 65535];
                    if (src[cur + best] === src[i + best] && src[cur + best - 1] === src[i + best - 1] && src[cur] === src[i]) {
                        const ml = mlen(i, cur, maxLen);
                        if (ml > best) { best = ml; curDist = dist; if (ml >= nice) break; }
                    }
                    if (g === 0 || --chain <= 0) break;
                    cur -= g; dist += g;
                }
                if (curDist) { curLen = best; if (curLen === 3 && curDist > 4096) curLen = 2; }
            }
        }
        if (prevLen >= 3 && curLen <= prevLen) {
            const end = i - 1 + prevLen;
            toks.push([prevLen, prevDist]);
            for (let k = i + 1; k < end; k++) if (k + 3 <= len) insert(k);
            i = end; avail = false; prevLen = 2; continue;
        }
        if (i === len) { if (avail) toks.push(src[i - 1]); break; }
        if (avail) toks.push(src[i - 1]);
        avail = true; prevLen = curLen; prevDist = curDist; i++;
    }
    return toks;
}
function inputs() {
    const list = [];
    const words = ["the", "quick", "brown", "fox", "jumps", "over", "lazy", "dog", "{\"id\":", ",", "\n", " "];
    for (const n of [9, 50, 300, 1000, 5000, 9000, 30000, 100000]) {
        let s = ""; while (s.length < n) s += words[(rnd() * words.length) | 0] + (rnd() < 0.3 ? (rnd() * 1000) | 0 : "");
        list.push(["text" + n, new TextEncoder().encode(s.slice(0, n))]);
        const sk = new Uint8Array(n); for (let i = 0; i < n; i++) sk[i] = Math.floor(256 * rnd() * rnd() * rnd()) & 255; list.push(["skew" + n, sk]);
        const rp = new Uint8Array(n); for (let i = 0; i < n; i++) rp[i] = i < 8 || rnd() < 0.15 ? (rnd() * 256) | 0 : rp[i - 1 - ((rnd() * 7) | 0)]; list.push(["rep" + n, rp]);
    }
    const fib = [1, 1]; while (fib.length < 22) fib.push(fib[fib.length - 1] + fib[fib.length - 2]);
    const bag = []; fib.forEach((c, k) => { for (let j = 0; j < c; j++) bag.push(k * 11 + 3); });
    for (let i = bag.length - 1; i > 0; i--) { const j = (rnd() * (i + 1)) | 0; const t = bag[i]; bag[i] = bag[j]; bag[j] = t; }
    list.push(["fib", Uint8Array.from(bag)]);
    return list;
}
const sameArr = (a, b) => a.length === b.length && a.every((v, i) => v === b[i]);
let dynBlocks = 0, clampedCL = 0, parsed = 0;
const decoded = [];
for (const [name, d] of inputs()) for (const lv of [6, 7, 8, 9]) {
    const f = deflate(d, { level: lv });
    const toks = [];
    const r = puff(f, 1e7, toks);
    decoded.push([name, lv, d, r, toks]);
}
__kitS.section("transmitted code lengths equal a JS Huffman of the decoded symbols", () => {
    for (const [name, lv, , r] of decoded) for (const b of r.blocks) {
        if (b.type !== 2) continue;
        dynBlocks++;
        const ll = jsHuff(pad1(b.lf.slice(0, 286)), 286, 15), dl = jsHuff(padD(b.df.slice()), 30, 15);
        const tl = b.ll.concat(new Array(286 - b.ll.length).fill(0)), td = b.dl.concat(new Array(30 - b.dl.length).fill(0));
        ok(sameArr(ll, tl), name + "/" + lv + ": literal/length code lengths");
        ok(sameArr(dl, td), name + "/" + lv + ": distance code lengths");
        ok(sameArr(jsHuff(padCL(b.clf.slice()), 19, 7), b.clLens), name + "/" + lv + ": code-length code lengths");
        const cf = padCL(b.clf.slice()); if (cf.filter((v) => v).length > 1 && Math.max(...jsHuff(cf, 19, 64)) > 7) clampedCL++;
    }
    ok(dynBlocks > 40, "dynamic blocks checked: " + dynBlocks);
    ok(clampedCL > 0, "blocks whose code-length tree was deeper than 7 bits (clamp ran): " + clampedCL);
});
__kitS.section("gzip carries the same deflate body as deflate", () => {
    for (const [name, lv, d] of decoded) {
        const g = gzip(d, lv), f = deflate(d, { level: lv }), body = g.subarray(10, g.length - 8);
        ok(body.length === f.length && body.every((v, i) => v === f[i]), name + "/" + lv + ": gzip body equals deflate");
    }
});
__kitS.section("decoded tokens equal a JS port of the lazy parse", () => {
    for (const [name, lv, d, r, toks] of decoded) {
        if (r.blocks.some((b) => b.type === 0)) continue;
        parsed++;
        const want = jsLazy(d, lv);
        const eq = want.length === toks.length && want.every((t, i) => (typeof t === "number" ? t === toks[i] : Array.isArray(toks[i]) && t[0] === toks[i][0] && t[1] === toks[i][1]));
        ok(eq, name + "/" + lv + ": " + toks.length + " tokens vs JS " + want.length);
    }
    ok(parsed > 60, "streams parsed: " + parsed);
});
print("gzip huffman/lazy oracle: " + checks + " checks, " + fails + " failed");
if (fails) throw new Error(fails + " failed");
__kitS.end();
