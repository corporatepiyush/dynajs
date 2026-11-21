// flags: --std
// Image pipeline — a PNG encoder and decoder written from the specification (zlib framing, CRC-checked chunks,
// all five scanline filters with adaptive selection), image operations (resize, Gaussian blur, unsharp mask,
// histogram equalisation), perceptual hashing with near-duplicate clustering, a worker-thread thumbnail farm and
// an HTTP image service with an LRU of rendered variants and ETags.
//
// WHAT IT IS
//   PNG          signature, IHDR/tEXt/IDAT/IEND chunks with CRC-32, a zlib stream (CMF/FLG header + raw DEFLATE
//                + Adler-32) and per-row filters None/Sub/Up/Average/Paeth; the encoder picks the filter per row
//                by the minimum-sum-of-absolute-differences heuristic, and the decoder validates every CRC, the
//                Adler-32, chunk lengths and filter types, refusing a damaged file with the reason
//   operations   bilinear resize, a separable Gaussian blur, unsharp mask, grayscale (Rec. 709 luma), histogram
//                equalisation, area-averaging downscale and crop
//   hashing      a 64-bit difference hash (dHash) per image: resized, recompressed and slightly edited copies
//                stay within a few bits; a UnionFind over every pair within 6 bits groups the near-duplicates
//   farm         thumbnails for a batch of uploads are produced by os.Worker threads in parallel, running the
//                same area-averaging function as the main thread (its source is shipped to the worker)
//   service      GET /img/:id?w=…&blur=…&fmt=gray returns a derived PNG, keeps the last 4 variants in an LRU keyed
//                by the source digest and the parameters, and answers If-None-Match with 304
//
//   vectors      every per-pixel stage is a whole-row or whole-image dyna:simd call: filter residuals are vector
//                subtractions whose cost is 128·n − normL1(r − 128), the Paeth predictor is abs/sub plus three
//                threshold masks blended with mul/add, the Up filter decodes as one add, resize is four gathers
//                and a lerp, blur is a shifted copy plus axpy per tap, grayscale is one gemv against the luma
//                weights, the histogram is a scatterAdd and its CDF a cumsum, and the area downscale sums rows
//                with add and columns with scatterAdd. Sub, Average and Paeth decoding carry a dependency along
//                the row and stay scalar. Float results become bytes through Uint8ClampedArray (round, clamp) or
//                Uint8Array (modulo 256 — exactly the arithmetic PNG filters are defined in)
//
// MODULES  dyna:compress (deflate/inflate) · dyna:hash (CRC32, Adler32, SHA256) · dyna:simd · dyna:structures (LRU,
//          UnionFind) · dyna:mathx (popcount) · dyna:file (sniffType) · dyna:http (App) · os (Worker) · dyna:random
//          (fillNormal) · dyna:cli (Table) · dyna:bytes (concat)
//
// RUN      dynajs --std examples/systems/image-pipeline.js     renders test images, round-trips them, runs the farm and the service

import * as os from "os";
import { deflate, inflate } from "dyna:compress";
import { CRC32, Adler32, SHA256Hex } from "dyna:hash";
import {
    abs,
    add,
    sub,
    mul,
    scale,
    axpy,
    affine,
    addScalar,
    normL1,
    gather,
    scatterAdd,
    cumsum,
    gemv,
    threshold,
    variance as varianceF32,
    distL1,
    i32Add,
} from "dyna:simd";
import { LRU, UnionFind } from "dyna:structures";
import { popcount } from "dyna:mathx";
import { concat } from "dyna:bytes";
import { makeTempDir, writeFile, readBytes, sniffType, removeAll } from "dyna:file";
import { App } from "dyna:http";
import { Random } from "dyna:random";
import { Table } from "dyna:cli";
const table = (rows) => Table(rows.slice(1), { head: rows[0] });

const work = makeTempDir("images");
const rng = new Random(255);
const enc = new TextEncoder(),
    dec = new TextDecoder();
const F = (n) => new Float32Array(n);
const toF = (bytes) => {
    const f = F(bytes.length);
    f.set(bytes);
    return f;
};
const toU8 = (f) => {
    const out = new Uint8Array(f.length);
    new Uint8ClampedArray(out.buffer).set(f);
    return out;
};
const tile = (row, rgb) => {
    row.set(rgb);
    for (let k = rgb.length; k < row.length; k *= 2) row.copyWithin(k, 0, Math.min(k, row.length - k));
    return row;
};

// =============================================================================
// PNG codec (RGB 8-bit, colour type 2; grayscale colour type 0)
// =============================================================================
const SIG = new Uint8Array([137, 80, 78, 71, 13, 10, 26, 10]);
const FILTERS = { none: 0, sub: 1, up: 2, avg: 3, paeth: 4 };
const u32 = (n) => new Uint8Array([(n >>> 24) & 255, (n >>> 16) & 255, (n >>> 8) & 255, n & 255]);
const be32 = (b, o) => ((b[o] << 24) | (b[o + 1] << 16) | (b[o + 2] << 8) | b[o + 3]) >>> 0;
function chunk(type, data) {
    const td = concat([enc.encode(type), Uint8Array.from(data)]);
    return concat([u32(data.length), td, u32(CRC32(td))]);
}
const paeth = (a, b, c) => {
    const p = a + b - c,
        pa = Math.abs(p - a),
        pb = Math.abs(p - b),
        pc = Math.abs(p - c);
    return pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
};
function paethPredict(pred, a, b, c, [pa, pb, pc, t1, t2]) {
    // pa = |p−a| = |b−c|, pb = |a−c|, pc = |a+b−2c|; pick a unless pa is beaten, then b unless pc beats pb
    abs(pa, sub(pa, b, c));
    abs(pb, sub(pb, a, c));
    abs(pc, sub(pc, add(pc, a, b), add(t1, c, c)));
    threshold(add(t1, threshold(sub(t1, pa, pb), 0), threshold(sub(t2, pa, pc), 0)), 0);
    threshold(sub(t2, pb, pc), 0);
    const inner = add(pc, mul(pc, sub(pc, c, b), t2), b);
    return add(pred, mul(pb, sub(pb, inner, a), t1), a);
}
function pngFromRaw(w, h, ch, raw, text = {}) {
    const zlib = concat([new Uint8Array([0x78, 0x9c]), deflate(raw, { level: 9 }), u32(Adler32(raw))]);
    const ihdr = concat([u32(w), u32(h), new Uint8Array([8, ch === 3 ? 2 : 0, 0, 0, 0])]);
    const texts = Object.entries(text).map(([k, v]) =>
        chunk("tEXt", concat([enc.encode(k), new Uint8Array([0]), enc.encode(v)])),
    );
    return concat([SIG, chunk("IHDR", ihdr), ...texts, chunk("IDAT", zlib), chunk("IEND", new Uint8Array(0))]);
}
function encodePNG(img, { filter = "adaptive", text = {} } = {}) {
    const { width: w, height: h, channels: ch, data } = img,
        stride = w * ch,
        raw = new Uint8Array(h * (stride + 1)),
        blank = new Uint8Array(stride);
    const cur = F(stride),
        up = F(stride),
        left = F(stride),
        upLeft = F(stride),
        pred = F(stride),
        scratch = Array.from({ length: 5 }, () => F(stride)),
        r = F(stride),
        outs = Array.from({ length: 5 }, () => new Uint8Array(stride));
    const candidates = filter === "adaptive" ? [0, 1, 2, 3, 4] : [FILTERS[filter]];
    const rows = new Uint8Array(h);
    for (let y = 0; y < h; y++) {
        const row = data.subarray(y * stride, (y + 1) * stride),
            prev = y ? data.subarray((y - 1) * stride, y * stride) : blank;
        cur.set(row);
        up.set(prev);
        left.fill(0, 0, ch);
        left.set(cur.subarray(0, stride - ch), ch);
        upLeft.fill(0, 0, ch);
        upLeft.set(up.subarray(0, stride - ch), ch);
        let best = -1,
            bestCost = Infinity;
        for (const f of candidates) {
            const out = outs[f];
            if (f === 0) out.set(row);
            else if (f === 1) out.set(sub(r, cur, left));
            else if (f === 2) out.set(sub(r, cur, up));
            else if (f === 3) {
                // row − ⌊(a+b)/2⌋ as row + 512.5 − (a+b)/2: always positive, so truncation is the floor
                add(r, left, up);
                affine(r, -0.5, 512.5);
                out.set(add(r, r, cur));
            } else out.set(sub(r, cur, paethPredict(pred, left, up, upLeft, scratch)));
            r.set(out);
            const cost = 128 * stride - normL1(addScalar(r, -128));
            if (cost < bestCost) {
                best = f;
                bestCost = cost;
            }
        }
        rows[y] = best;
        raw[y * (stride + 1)] = best;
        raw.set(outs[best], y * (stride + 1) + 1);
    }
    const used = Array.from({ length: 5 }, (_, f) =>
        rows
            .values()
            .filter((x) => x === f)
            .count(),
    );
    return { png: pngFromRaw(w, h, ch, raw, text), filters: used, rows };
}
function decodePNG(png) {
    if (png.length < 8 || !SIG.every((b, i) => png[i] === b)) throw new Error("not a PNG: bad signature");
    let off = 8,
        w,
        h,
        ch;
    const idat = [],
        text = {};
    while (off < png.length) {
        if (off + 12 > png.length) throw new Error(`truncated chunk header at byte ${off}`);
        const len = be32(png, off),
            type = dec.decode(png.subarray(off + 4, off + 8));
        if (off + 12 + len > png.length) throw new Error(`chunk ${type} at byte ${off}: truncated`);
        const data = png.subarray(off + 8, off + 8 + len);
        if (CRC32(png.subarray(off + 4, off + 8 + len)) !== be32(png, off + 8 + len))
            throw new Error(`chunk ${type} at byte ${off}: CRC mismatch`);
        if (type === "IHDR") {
            w = be32(data, 0);
            h = be32(data, 4);
            ch = data[9] === 2 ? 3 : 1;
            if (data[8] !== 8 || data[12] !== 0) throw new Error("only 8-bit non-interlaced images");
        } else if (type === "IDAT") idat.push(data);
        else if (type === "tEXt") {
            const z = data.indexOf(0);
            text[dec.decode(data.subarray(0, z))] = dec.decode(data.subarray(z + 1));
        } else if (type === "IEND") break;
        off += 12 + len;
    }
    const z = concat(idat);
    if ((z[0] & 15) !== 8 || ((z[0] << 8) | z[1]) % 31 !== 0) throw new Error("bad zlib header");
    const raw = inflate(z.subarray(2, z.length - 4));
    if (Adler32(raw) !== be32(z, z.length - 4)) throw new Error("zlib Adler-32 mismatch");
    const stride = w * ch,
        data = new Uint8Array(h * stride),
        blank = new Uint8Array(stride),
        cur = F(stride),
        up = F(stride);
    if (raw.length !== h * (stride + 1))
        throw new Error(`image data is ${raw.length} bytes, expected ${h * (stride + 1)}`);
    for (let y = 0; y < h; y++) {
        const f = raw[y * (stride + 1)],
            src = raw.subarray(y * (stride + 1) + 1, (y + 1) * (stride + 1)),
            line = data.subarray(y * stride, (y + 1) * stride),
            above = y ? data.subarray((y - 1) * stride, y * stride) : blank;
        if (f === 0) line.set(src);
        else if (f === 2) {
            cur.set(src);
            up.set(above);
            line.set(add(cur, cur, up));
        } else if (f === 1) for (let i = 0; i < stride; i++) line[i] = src[i] + (i >= ch ? line[i - ch] : 0);
        else if (f === 3)
            for (let i = 0; i < stride; i++) line[i] = src[i] + (((i >= ch ? line[i - ch] : 0) + above[i]) >> 1);
        else if (f === 4)
            for (let i = 0; i < stride; i++)
                line[i] = src[i] + paeth(i >= ch ? line[i - ch] : 0, above[i], i >= ch ? above[i - ch] : 0);
        else throw new Error(`row ${y}: unknown filter type ${f}`);
    }
    return { width: w, height: h, channels: ch, data, text };
}

// =============================================================================
// Operations
// =============================================================================
const make = (width, height, channels = 3) => ({
    width,
    height,
    channels,
    data: new Uint8Array(width * height * channels),
});
function resize(img, w, h) {
    const { width: W, height: H, channels: ch } = img,
        n = w * ch,
        sx = W / w,
        sy = H / h,
        src = toF(img.data);
    const c00 = new Int32Array(n),
        wx0 = F(n),
        wx1 = F(n);
    for (let x = 0; x < w; x++) {
        const fx = Math.min(W - 1.001, (x + 0.5) * sx - 0.5),
            x0 = Math.max(0, Math.floor(fx)),
            dx = Math.max(0, fx - x0);
        for (let c = 0; c < ch; c++) {
            c00[x * ch + c] = x0 * ch + c;
            wx0[x * ch + c] = 1 - dx;
            wx1[x * ch + c] = dx;
        }
    }
    const off = new Int32Array(n),
        c10 = i32AddConst(c00, ch, off),
        idx = Array.from({ length: 4 }, () => new Int32Array(n)),
        [p00, p10, p01, p11] = Array.from({ length: 4 }, () => F(n)),
        t = F(n),
        out = F(w * h * ch);
    for (let y = 0; y < h; y++) {
        const fy = Math.min(H - 1.001, (y + 0.5) * sy - 0.5),
            y0 = Math.max(0, Math.floor(fy)),
            dy = Math.max(0, fy - y0);
        i32AddConst(c00, y0 * W * ch, off, idx[0]);
        i32AddConst(c10, y0 * W * ch, off, idx[1]);
        i32AddConst(c00, (y0 + 1) * W * ch, off, idx[2]);
        i32AddConst(c10, (y0 + 1) * W * ch, off, idx[3]);
        gather(p00, src, idx[0]);
        gather(p10, src, idx[1]);
        gather(p01, src, idx[2]);
        gather(p11, src, idx[3]);
        const top = add(p00, mul(p00, p00, wx0), mul(t, p10, wx1)),
            bottom = add(p01, mul(p01, p01, wx0), mul(t, p11, wx1));
        out.set(axpy(scale(top, 1 - dy), dy, bottom), y * n);
    }
    return { width: w, height: h, channels: ch, data: toU8(out) };
}
function i32AddConst(a, k, scratch, out = new Int32Array(a.length)) {
    scratch.fill(k);
    return i32Add(out, a, scratch);
}
function shift(dst, src, d, horizontal, w, h, ch) {
    // dst[x] = src[clamp(x + d)] along rows (horizontal) or columns, for the whole interleaved image at once
    const n = src.length,
        stride = w * ch,
        s = Math.abs(d) * (horizontal ? ch : stride);
    if (d === 0) return (dst.set(src), dst);
    if (s < n) d > 0 ? dst.set(src.subarray(s)) : dst.set(src.subarray(0, n - s), s);
    if (horizontal) {
        const k = Math.min(Math.abs(d), w);
        for (let y = 0; y < h; y++) {
            const edge =
                d > 0
                    ? src.subarray((y + 1) * stride - ch, (y + 1) * stride)
                    : src.subarray(y * stride, y * stride + ch);
            for (let j = 0; j < k; j++) dst.set(edge, d > 0 ? (y + 1) * stride - (j + 1) * ch : y * stride + j * ch);
        }
    } else {
        const k = Math.min(Math.abs(d), h),
            edge = d > 0 ? src.subarray(n - stride) : src.subarray(0, stride);
        for (let j = 0; j < k; j++) dst.set(edge, d > 0 ? n - (j + 1) * stride : j * stride);
    }
    return dst;
}
function blurF(img, sigma) {
    const r = Math.ceil(sigma * 3),
        k = Float64Array.from(Iterator.range(-r, r + 1), (d) => Math.exp(-(d * d) / (2 * sigma * sigma))),
        ks = k.values().sum();
    const { width: w, height: h, channels: ch } = img,
        shifted = F(img.data.length);
    const pass = (src, horizontal) =>
        k
            .values()
            .reduce(
                (dst, kw, i) => axpy(dst, kw / ks, shift(shifted, src, i - r, horizontal, w, h, ch)),
                F(src.length),
            );
    return pass(pass(toF(img.data), true), false);
}
const blur = (img, sigma) => ({ ...img, data: toU8(blurF(img, sigma)) });
function unsharp(img, sigma = 1.5, amount = 0.8) {
    // img + amount·(img − blur) = (1 + amount)·img − amount·blur
    const out = scale(toF(img.data), 1 + amount);
    return { ...img, data: toU8(axpy(out, -amount, blurF(img, sigma))) };
}
function crop(img, x0, y0, w, h) {
    const out = make(w, h, img.channels),
        ch = img.channels;
    Iterator.range(0, h).forEach((y) =>
        out.data.set(
            img.data.subarray(((y0 + y) * img.width + x0) * ch, ((y0 + y) * img.width + x0 + w) * ch),
            y * w * ch,
        ),
    );
    return out;
}
const LUMA = Float32Array.of(0.2126, 0.7152, 0.0722);
function grayF(img) {
    const n = img.width * img.height;
    if (img.channels === 1) return toF(img.data);
    return gemv(F(n), toF(img.data), LUMA, n, 3, 0);
}
const gray = (img) => ({ width: img.width, height: img.height, channels: 1, data: toU8(grayF(img)) });
function equalize(g) {
    const n = g.data.length,
        idx = new Int32Array(n),
        hist = F(256);
    idx.set(g.data);
    scatterAdd(hist, idx, F(n).fill(1));
    const lut = scale(cumsum(hist), 255 / n);
    return { width: g.width, height: g.height, channels: 1, data: toU8(gather(F(n), lut, idx)) };
}
function boxResize(img, w, h) {
    // area averaging; self-contained because its source is also shipped to the thumbnail workers
    const { width: W, height: H, channels: ch } = img,
        sx = W / w,
        sy = H / h,
        n = w * ch,
        N = W * ch;
    const src = new Float32Array(img.data.length);
    src.set(img.data);
    const columns = new Int32Array(N).fill(n),
        counts = new Float32Array(n);
    for (let x = 0; x < w; x++)
        for (let xx = Math.floor(x * sx); xx < Math.floor((x + 1) * sx); xx++)
            for (let c = 0; c < ch; c++) {
                columns[xx * ch + c] = x * ch + c;
                counts[x * ch + c]++;
            }
    const inv = counts.map((v) => (v ? 1 / v : 0)),
        acc = new Float32Array(N),
        sums = new Float32Array(n + 1),
        out = new Float32Array(w * h * ch);
    for (let y = 0; y < h; y++) {
        const y0 = Math.floor(y * sy),
            y1 = Math.floor((y + 1) * sy);
        acc.fill(0);
        for (let yy = y0; yy < y1; yy++) add(acc, acc, src.subarray(yy * N, yy * N + N));
        sums.fill(0);
        scatterAdd(sums, columns, acc);
        scale(mul(out.subarray(y * n, y * n + n), sums.subarray(0, n), inv), 1 / Math.max(1, y1 - y0));
    }
    const data = new Uint8Array(out.length);
    new Uint8ClampedArray(data.buffer).set(out);
    return { width: w, height: h, channels: ch, data };
}
function dhash(img) {
    const g = boxResize(gray(img), 9, 8).data;
    return Iterator.range(0, 64).reduce((h, i) => {
        const p = (i >> 3) * 9 + (i & 7);
        return (h << 1n) | (g[p] > g[p + 1] ? 1n : 0n);
    }, 0n);
}
const hamming = (a, b) => popcount(a ^ b);

// =============================================================================
// Test images: a gradient, a landscape-like scene, a document scan, and noise
// =============================================================================
const SUN = Float32Array.of(255, 220, 120);
function scene(seed, w = 320, h = 220) {
    const r = new Random(seed),
        sun = [r.nextBounded(w), 40 + r.nextBounded(60)],
        hills = Array.from({ length: 4 }, () => [r.nextFloat() * 0.03, r.nextFloat() * 6, 120 + r.nextBounded(60)]);
    const stride = w * 3,
        horizon = F(stride); // ground below the lowest hill line, one value per channel
    Iterator.range(0, w).forEach((x) =>
        horizon.fill(Math.min(...hills.map(([f, p, base]) => base + 20 * Math.sin(x * f + p))), x * 3, x * 3 + 3),
    );
    const px = F(h * stride),
        mask = F(stride),
        sky = F(stride),
        ground = F(stride);
    for (let y = 0; y < h; y++) {
        tile(sky, [80 + y / 3, 140 + y / 4, 230]);
        tile(ground, [40 + y / 4, 90 + y / 6, 30]);
        threshold(sub(mask, mask.fill(y), horizon), 0); // 1 where y > horizon
        const row = px.subarray(y * stride, (y + 1) * stride);
        add(row, sky, mul(ground, sub(ground, ground, sky), mask));
        const dy = y - sun[1];
        if (dy * dy < 22 * 22) {
            const half = Math.sqrt(22 * 22 - dy * dy),
                lo = Math.max(0, Math.floor(sun[0] - half) + 1),
                hi = Math.min(w - 1, Math.ceil(sun[0] + half) - 1);
            Iterator.range(lo, hi + 1).forEach((x) => row.set(SUN, x * 3));
        }
    }
    const grain = F(h * stride);
    r.fillNormal(grain, 0, 3);
    return { width: w, height: h, channels: 3, data: toU8(add(px, px, grain)) };
}
function documentScan(w = 300, h = 200) {
    const img = make(w, h),
        d = img.data;
    for (let y = 0; y < h; y++)
        for (let x = 0; x < w; x++) {
            const line = y % 16 > 11 && x > 20 && x < w - 20 && (x * 7 + y) % 13 < 9;
            const v = line ? 40 : 238 - (x + y) / 30,
                i = (y * w + x) * 3;
            d[i] = d[i + 1] = v;
            d[i + 2] = v - 4;
        }
    return img;
}
const gradient = (() => {
    const img = make(256, 128);
    for (let y = 0; y < 128; y++) for (let x = 0; x < 256; x++) img.data.set([x, y * 2, 255 - x], (y * 256 + x) * 3);
    return img;
})();
const noise = (() => {
    const img = make(200, 150);
    rng.fill(img.data);
    return img;
})();
const sources = { gradient, beach: scene(1), mountains: scene(2), memo: documentScan(), noise };

// =============================================================================
// Codec checks, filter comparison
// =============================================================================
const results = {};
const codecRows = [];
const FILTER_NAMES = ["none", "sub", "up", "avg", "paeth", "adaptive"];
for (const [name, img] of Object.entries(sources)) {
    const sizes = Object.fromEntries(FILTER_NAMES.map((f) => [f, encodePNG(img, { filter: f }).png.length]));
    const { png, filters } = encodePNG(img, { text: { Title: name, Software: "dynajs image-pipeline" } });
    writeFile(work.join(`${name}.png`), png);
    const back = decodePNG(png);
    const same = back.width === img.width && back.data.every((v, i) => v === img.data[i]);
    if (!same) results[`${name}: decoded pixels differ`] = false;
    codecRows.push([
        name,
        `${img.width}x${img.height}`,
        ...FILTER_NAMES.map((f) => String(sizes[f])),
        filters.join("/"),
        back.text.Title === name ? "ok" : "missing",
    ]);
    sources[name].sizes = sizes;
}
results["every image round-trips losslessly with its tEXt metadata; the file sniffs as image/png"] =
    codecRows.every((r) => r.at(-1) === "ok") && sniffType(work.join("beach.png")) === "image/png";
results["adaptive filtering crushes a gradient and cannot compress noise"] =
    sources.gradient.sizes.adaptive < sources.gradient.sizes.none / 5 &&
    sources.noise.sizes.adaptive >= sources.noise.sizes.none * 0.98;
{
    // the vectorised filter search against the per-byte definition of every filter and of the cost
    const img = sources.beach,
        { width: w, channels: ch, data } = img,
        stride = w * ch;
    const choice = encodePNG(img).rows;
    let agree = true;
    for (let y = 0; y < img.height && agree; y++) {
        let best = -1,
            bestCost = Infinity;
        for (let f = 0; f < 5; f++) {
            let cost = 0;
            for (let i = 0; i < stride; i++) {
                const x = data[y * stride + i],
                    a = i >= ch ? data[y * stride + i - ch] : 0,
                    b = y ? data[(y - 1) * stride + i] : 0,
                    c = y && i >= ch ? data[(y - 1) * stride + i - ch] : 0;
                const v =
                    (x - (f === 0 ? 0 : f === 1 ? a : f === 2 ? b : f === 3 ? (a + b) >> 1 : paeth(a, b, c))) & 255;
                cost += v < 128 ? v : 256 - v;
            }
            if (cost < bestCost) [best, bestCost] = [f, cost];
        }
        agree = best === choice[y];
    }
    results["the vector filter search picks the same filter on every row as the per-byte definition"] = agree;
}
const refusals = {};
const expectRefusal = (label, bytes) => {
    try {
        decodePNG(bytes);
        refusals[label] = "accepted";
    } catch (e) {
        refusals[label] = e.message;
    }
};
const beachPng = readBytes(work.join("beach.png"));
const flipped = beachPng.slice();
flipped[60] ^= 0xff;
expectRefusal("flipped byte", flipped);
expectRefusal("truncated", beachPng.subarray(0, beachPng.length - 40));
const badRaw = new Uint8Array(4 * (4 * 3 + 1));
badRaw[13] = 7; // row 1 declares filter type 7
expectRefusal("filter 7", pngFromRaw(4, 4, 3, badRaw));
expectRefusal("short data", pngFromRaw(4, 4, 3, badRaw.subarray(0, 40)));
results[`damaged files are refused with the reason: ${Object.values(refusals).join(" | ")}`] =
    /CRC mismatch/.test(refusals["flipped byte"]) &&
    /truncated/.test(refusals.truncated) &&
    /row 1: unknown filter type 7/.test(refusals["filter 7"]) &&
    /expected 52/.test(refusals["short data"]);

// =============================================================================
// Operations and perceptual hashing
// =============================================================================
const beach = sources.beach;
const small = resize(beach, 160, 110),
    blurred = blur(beach, 2),
    sharp = unsharp(small),
    eq = equalize(gray(sources.memo));
const variance = (img) => varianceF32(grayF(img));
const edge = (img) => {
    const g = grayF(img);
    return distL1(g.subarray(1), g.subarray(0, g.length - 1)) / g.length;
};
results["blur lowers edge energy, unsharp mask raises it"] =
    edge(blurred) < edge(beach) * 0.6 && edge(sharp) > edge(small);
results["resizing preserves the overall tonal distribution"] =
    Math.abs(variance(small) - variance(beach)) / variance(beach) < 0.1;
{
    // independent per-pixel definitions on one image: bilinear sample, separable Gaussian with clamped edges, luma
    const img = beach,
        { width: W, height: H } = img,
        at = (x, y, c) => img.data[(y * W + x) * 3 + c];
    let resizeErr = 0;
    for (let y = 0; y < 110; y += 7)
        for (let x = 0; x < 160; x += 5)
            for (let c = 0; c < 3; c++) {
                const fx = Math.min(W - 1.001, (x + 0.5) * 2 - 0.5),
                    fy = Math.min(H - 1.001, (y + 0.5) * 2 - 0.5),
                    x0 = Math.floor(fx),
                    y0 = Math.floor(fy),
                    dx = fx - x0,
                    dy = fy - y0;
                const v =
                    at(x0, y0, c) * (1 - dx) * (1 - dy) +
                    at(x0 + 1, y0, c) * dx * (1 - dy) +
                    at(x0, y0 + 1, c) * (1 - dx) * dy +
                    at(x0 + 1, y0 + 1, c) * dx * dy;
                resizeErr = Math.max(resizeErr, Math.abs(small.data[(y * 160 + x) * 3 + c] - v));
            }
    const r = 6,
        k = [];
    for (let d = -r; d <= r; d++) k.push(Math.exp(-(d * d) / 8));
    const ks = k.reduce((a, b) => a + b, 0),
        clampX = (x) => Math.min(W - 1, Math.max(0, x)),
        clampY = (y) => Math.min(H - 1, Math.max(0, y));
    let blurErr = 0;
    for (const [x, y] of [
        [0, 0],
        [319, 219],
        [3, 100],
        [160, 110],
        [317, 2],
        [200, 218],
    ])
        for (let c = 0; c < 3; c++) {
            let v = 0;
            for (let j = -r; j <= r; j++) {
                let row = 0;
                for (let i = -r; i <= r; i++) row += k[i + r] * at(clampX(x + i), clampY(y + j), c);
                v += (k[j + r] * row) / ks;
            }
            blurErr = Math.max(blurErr, Math.abs(blurred.data[(y * W + x) * 3 + c] - v / ks));
        }
    const g = gray(img).data;
    let grayErr = 0;
    for (let i = 0; i < W * H; i += 97)
        grayErr = Math.max(
            grayErr,
            Math.abs(
                g[i] -
                    (0.2126 * at(i % W, (i / W) | 0, 0) +
                        0.7152 * at(i % W, (i / W) | 0, 1) +
                        0.0722 * at(i % W, (i / W) | 0, 2)),
            ),
        );
    results[
        `resize, blur and grayscale agree with their per-pixel definitions (max error ${Math.max(resizeErr, blurErr, grayErr).toFixed(3)})`
    ] = resizeErr <= 0.51 && blurErr <= 0.51 && grayErr <= 0.51;
}
{
    const g = gray(sources.memo).data,
        h2 = new Array(256).fill(0);
    for (const v of g) h2[v]++;
    let c = 0;
    const lut = h2.map((n) => Math.round(((c += n) * 255) / g.length));
    let mismatch = 0;
    for (let i = 0; i < g.length; i++) if (Math.abs(eq.data[i] - lut[g[i]]) > 1) mismatch++;
    results["histogram equalisation stretches the scan to the full range and matches its CDF"] =
        Math.max(...eq.data) === 255 && new Set(eq.data).size > 10 && mismatch === 0;
}
const variants = {
    "beach (half size)": small,
    "beach (blurred)": blurred,
    "beach (recompressed)": decodePNG(encodePNG(beach, { filter: "sub" }).png),
    "beach (crop 95%)": resize(crop(beach, 8, 5, 304, 209), 320, 220),
};
const hb = dhash(beach),
    dists = Object.fromEntries(Object.entries(variants).map(([k, v]) => [k, hamming(hb, dhash(v))]));
const others = {
    mountains: hamming(hb, dhash(sources.mountains)),
    memo: hamming(hb, dhash(sources.memo)),
    noise: hamming(hb, dhash(noise)),
};
results[
    `dHash: variants within ${Math.max(...Object.values(dists))} bits, different images ${Math.min(...Object.values(others))}+ bits away`
] = Math.max(...Object.values(dists)) <= 6 && Math.min(...Object.values(others)) > Math.max(...Object.values(dists));

// =============================================================================
// Thumbnail farm on worker threads
// =============================================================================
const workerSrc = work.join("thumb-worker.js");
writeFile(
    workerSrc,
    `
import * as os from "os";
import { add, mul, scale, scatterAdd } from "dyna:simd";
${boxResize.toString()}
const parent = os.Worker.parent;
parent.onmessage = (e) => {
    if (e.data === "stop") { parent.onmessage = null; return; }
    const { id, img, tw, th } = e.data;
    parent.postMessage({ id, data: boxResize(img, tw, th).data });
};
`,
);
const uploads = Array.from({ length: 24 }, (_, i) => ({ id: `upload-${i}`, img: scene(100 + i, 480, 320) }));
const pool = Array.from({ length: 4 }, () => new os.Worker(workerSrc.toString()));
const tStart = Date.now();
const thumbs = await new Promise((resolve) => {
    const done = new Map();
    let next = 0;
    const feed = (w) => {
        if (next >= uploads.length) return;
        const u = uploads[next++];
        w.postMessage({ id: u.id, img: u.img, tw: 96, th: 64 });
    };
    for (const w of pool) {
        w.onmessage = (e) => {
            done.set(e.data.id, { width: 96, height: 64, channels: 3, data: e.data.data });
            if (done.size === uploads.length) resolve(done);
            else feed(w);
        };
        feed(w);
    }
});
const farmMs = Date.now() - tStart;
for (const w of pool) {
    w.onmessage = null;
    w.postMessage("stop");
}
{
    // the area average of one thumbnail recomputed per pixel: every 5x5 block of the source, rounded
    const u = uploads[7],
        t = thumbs.get(u.id).data;
    let worst = 0;
    for (let y = 0; y < 64; y++)
        for (let x = 0; x < 96; x++)
            for (let c = 0; c < 3; c++) {
                let s = 0;
                for (let yy = y * 5; yy < y * 5 + 5; yy++)
                    for (let xx = x * 5; xx < x * 5 + 5; xx++) s += u.img.data[(yy * 480 + xx) * 3 + c];
                worst = Math.max(worst, Math.abs(t[(y * 96 + x) * 3 + c] - s / 25));
            }
    results[`24 thumbnails produced by 4 worker threads in ${farmMs} ms, each pixel the mean of its 5x5 block`] =
        thumbs.size === 24 && [...thumbs.values()].every((t) => t.data.length === 96 * 64 * 3) && worst <= 0.5;
}

// =============================================================================
// Near-duplicate clustering: UnionFind over every pair of dHashes within 6 bits
// =============================================================================
const gallery = [
    ["beach", beach],
    ...Object.entries(variants),
    ...["mountains", "memo", "noise", "gradient"].map((k) => [k, sources[k]]),
    ...uploads.map((u) => [u.id, u.img]),
    ...uploads.map((u) => [`${u.id} thumb`, thumbs.get(u.id)]),
];
const NEAR = 6;
const hashes = gallery.map(([, img]) => dhash(img)),
    uf = new UnionFind(gallery.length);
const nearPairs = Iterator.range(0, gallery.length)
    .flatMap((i) => Iterator.range(i + 1, gallery.length).map((j) => [i, j]))
    .filter(([i, j]) => hamming(hashes[i], hashes[j]) <= NEAR)
    .toArray();
nearPairs.forEach(([i, j]) => uf.union(i, j));
const clusterOf = (i) => uf.find(i);
{
    // oracle: bit-by-bit Hamming distance and a depth-first search over the same threshold
    const bits = (a, b) => {
        let n = 0;
        for (let x = a ^ b; x; x >>= 1n) n += Number(x & 1n);
        return n;
    };
    const n = gallery.length,
        comp = new Array(n).fill(-1);
    for (let s = 0; s < n; s++) {
        if (comp[s] >= 0) continue;
        const stack = [s];
        comp[s] = s;
        while (stack.length) {
            const i = stack.pop();
            for (let j = 0; j < n; j++)
                if (comp[j] < 0 && bits(hashes[i], hashes[j]) <= NEAR) {
                    comp[j] = s;
                    stack.push(j);
                }
        }
    }
    let samePartition = true;
    for (let i = 0; i < n; i++)
        for (let j = 0; j < n; j++)
            if ((comp[i] === comp[j]) !== (clusterOf(i) === clusterOf(j))) samePartition = false;
    const beachVariants = gallery.slice(1, 5).every((_, k) => clusterOf(k + 1) === clusterOf(0));
    const strangers = ["memo", "noise", "gradient"].every(
        (name) => clusterOf(gallery.findIndex(([g]) => g === name)) !== clusterOf(0),
    );
    const pairsKept = uploads.every((_, k) => clusterOf(9 + k) === clusterOf(9 + 24 + k));
    results[
        `${uf.count} near-duplicate groups among ${n} images: beach and its 4 edits together, every upload with its thumbnail, the partition equal to a DFS oracle`
    ] = samePartition && beachVariants && strangers && pairsKept;
}

// =============================================================================
// Image service with an LRU of rendered variants
// =============================================================================
const variantCache = new LRU(4);
const digests = new Map(Object.entries(sources).map(([k, s]) => [k, SHA256Hex(s.data)]));
let renders = 0;
const app = new App({ port: 0 });
app.get("/img/:id", (req) => {
    const src = sources[req.params.id];
    if (!src) return { status: 404, body: "no such image" };
    const w = Math.min(1024, Number(req.query.w) || src.width),
        sigma = Math.min(5, Number(req.query.blur) || 0),
        fmt = req.query.fmt === "gray" ? "gray" : "rgb";
    if (!Number.isFinite(w) || w < 8) return { status: 400, body: "w must be a number >= 8" };
    const key = SHA256Hex(`${digests.get(req.params.id)}|${w}|${sigma}|${fmt}`).slice(0, 24),
        etag = `"${key}"`;
    if (req.headers["if-none-match"] === etag) return { status: 304, body: "", headers: { etag } };
    let png = variantCache.get(key);
    if (!png) {
        renders++;
        let img = resize(src, w, Math.round((src.height * w) / src.width));
        if (sigma) img = blur(img, sigma);
        if (fmt === "gray") img = gray(img);
        variantCache.set(key, (png = encodePNG(img).png));
    }
    return { body: png, contentType: "image/png", headers: { etag, "cache-control": "public, max-age=31536000" } };
});
app.start();
const base = `http://127.0.0.1:${app.port}`;
const plan = [
    "w=120&blur=1&fmt=gray",
    "w=120&blur=1&fmt=gray",
    "w=40",
    "w=60&fmt=gray",
    "w=80&blur=2",
    "w=100",
    "w=120&blur=1&fmt=gray",
    "w=100",
];
const bodies = [];
for (const q of plan) {
    const res = await fetch(`${base}/img/beach?${q}`);
    bodies.push({ status: res.status, etag: res.headers.get("etag"), bytes: new Uint8Array(await res.arrayBuffer()) });
}
const v304 = await fetch(`${base}/img/beach?${plan[0]}`, { headers: { "if-none-match": bodies[0].etag } });
await v304.text();
const bad = await fetch(`${base}/img/beach?w=2`);
await bad.text();
const missing = await fetch(`${base}/img/nope`);
await missing.text();
app.close();
{
    // a plain least-recently-used list replaying the same request plan
    const lru = [];
    let modelRenders = 0,
        modelEvictions = 0;
    for (const q of plan) {
        const at = lru.indexOf(q);
        if (at >= 0) lru.splice(at, 1);
        else {
            modelRenders++;
            if (lru.length === 4) {
                lru.shift();
                modelEvictions++;
            }
        }
        lru.push(q);
    }
    const decoded = decodePNG(bodies[0].bytes);
    const reRendered =
        bodies[6].bytes.length === bodies[0].bytes.length && bodies[6].bytes.every((b, i) => b === bodies[0].bytes[i]);
    results[
        `${plan.length} requests: ${renders} renders and ${variantCache.stats.evictions} evictions, as an LRU of 4 predicts; an evicted variant re-renders byte-identical`
    ] =
        bodies.every((b) => b.status === 200) &&
        decoded.width === 120 &&
        decoded.channels === 1 &&
        renders === modelRenders &&
        variantCache.stats.evictions === modelEvictions &&
        reRendered;
    results["If-None-Match answers 304; invalid parameters and unknown images are refused"] =
        v304.status === 304 && bad.status === 400 && missing.status === 404;
}

print("image-pipeline");
print(table([["image", "size", ...FILTER_NAMES, "rows N/S/U/A/P", "tEXt"], ...codecRows]));
print(
    `  dHash      ${Object.entries(dists)
        .map(([k, d]) => `${k} ${d}`)
        .join(", ")} | ${Object.entries(others)
        .map(([k, d]) => `${k} ${d}`)
        .join(", ")} bits`,
);
print(
    `  clusters   ${uf.count} groups from ${nearPairs.length} pairs within ${NEAR} bits among ${gallery.length} images`,
);
print(`  farm       24 uploads 480x320 -> 96x64 in ${farmMs} ms on 4 threads`);
print(`  service    ${plan.length} requests, ${renders} renders, LRU ${JSON.stringify(variantCache.stats)}`);
removeAll(work);
check(results);

function check(results) {
    const names = Object.keys(results);
    const failed = names.filter((name) => !results[name]);
    for (const name of failed) print("FAIL " + name);
    if (failed.length) throw new Error(`${failed.length} of ${names.length} checks failed`);
}
