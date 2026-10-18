// flags: --std
// Image pipeline — a PNG encoder and decoder written from the specification (zlib framing, CRC-checked chunks,
// all five scanline filters with adaptive selection), image operations (resize, Gaussian blur, unsharp mask,
// histogram equalisation), perceptual hashing for duplicate detection, a worker-thread thumbnail farm and an
// HTTP image service with on-the-fly variants and ETags.
//
// WHAT IT IS
//   PNG          signature, IHDR/tEXt/IDAT/IEND chunks with CRC-32, a zlib stream (CMF/FLG header + raw DEFLATE
//                + Adler-32) and per-row filters None/Sub/Up/Average/Paeth; the encoder picks the filter per row
//                by the minimum-sum-of-absolute-differences heuristic, and the decoder validates every CRC and the
//                Adler-32, refusing a corrupted file with the reason
//   operations   bilinear resize, a separable Gaussian blur computed with SIMD vector kernels, unsharp mask,
//                grayscale (Rec. 709 luma), histogram equalisation, and crop
//   hashing      a 64-bit difference hash (dHash) per image: resized, recompressed and slightly edited copies
//                stay within a few bits, different images are far apart
//   farm         thumbnails for a batch of uploads are produced by os.Worker threads in parallel
//   service      GET /img/:id?w=…&blur=…&fmt=gray returns a derived PNG, caches variants by a key derived from the
//                source hash and the parameters, and answers If-None-Match with 304
//
//   bytes        PNG chunks, the zlib frame and the file are assembled with dyna:bytes concat; the filter search
//                runs one tight loop per filter type (no array allocated per byte), and the test scenes compute each
//                column's horizon once instead of re-evaluating every hill for every pixel
//
// MODULES  dyna:compress (deflate/inflate) · dyna:hash (CRC32, Adler32, SHA256) · dyna:simd (axpy, scale) · dyna:file
//          (sniffType) · dyna:http (App) · os (Worker) · dyna:random · dyna:cli (Table) · dyna:bytes (concat)
//
// RUN      dynajs --std examples/systems/image-pipeline.js     renders test images, round-trips them, runs the farm and the service

import * as os from "os";
import { deflate, inflate } from "dyna:compress";
import { CRC32, Adler32, SHA256Hex } from "dyna:hash";
import { axpy, scale } from "dyna:simd";
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

// =============================================================================
// PNG codec (RGB 8-bit, colour type 2; grayscale colour type 0)
// =============================================================================
const SIG = new Uint8Array([137, 80, 78, 71, 13, 10, 26, 10]);
const u32 = (n) => new Uint8Array([(n >>> 24) & 255, (n >>> 16) & 255, (n >>> 8) & 255, n & 255]);
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
function encodePNG(img, { filter = "adaptive", text = {} } = {}) {
    const { width: w, height: h, channels: ch, data } = img,
        stride = w * ch,
        raw = new Uint8Array(h * (stride + 1));
    const used = [0, 0, 0, 0, 0];
    for (let y = 0; y < h; y++) {
        const row = data.subarray(y * stride, (y + 1) * stride),
            prev = y ? data.subarray((y - 1) * stride, y * stride) : new Uint8Array(stride);
        const candidates = (
            filter === "adaptive" ? [0, 1, 2, 3, 4] : [{ none: 0, sub: 1, up: 2, avg: 3, paeth: 4 }[filter]]
        ).map((f) => {
            const out = new Uint8Array(stride);
            if (f === 0) out.set(row);
            else if (f === 2) for (let i = 0; i < stride; i++) out[i] = row[i] - prev[i];
            else
                for (let i = 0; i < stride; i++) {
                    const a = i >= ch ? row[i - ch] : 0,
                        b = prev[i];
                    out[i] = row[i] - (f === 1 ? a : f === 3 ? (a + b) >> 1 : paeth(a, b, i >= ch ? prev[i - ch] : 0));
                }
            let cost = 0;
            for (let i = 0; i < stride; i++) {
                const v = out[i];
                cost += v < 128 ? v : 256 - v;
            }
            return { f, out, cost };
        });
        const best = candidates.reduce((x, y2) => (y2.cost < x.cost ? y2 : x));
        used[best.f]++;
        raw[y * (stride + 1)] = best.f;
        raw.set(best.out, y * (stride + 1) + 1);
    }
    const zlib = concat([new Uint8Array([0x78, 0x9c]), deflate(raw, { level: 9 }), u32(Adler32(raw))]);
    const ihdr = concat([u32(w), u32(h), new Uint8Array([8, ch === 3 ? 2 : 0, 0, 0, 0])]);
    const texts = Object.entries(text).map(([k, v]) =>
        chunk("tEXt", concat([enc.encode(k), new Uint8Array([0]), enc.encode(v)])),
    );
    return {
        png: concat([SIG, chunk("IHDR", ihdr), ...texts, chunk("IDAT", zlib), chunk("IEND", new Uint8Array(0))]),
        filters: used,
    };
}
function decodePNG(png) {
    if (!SIG.every((b, i) => png[i] === b)) throw new Error("not a PNG: bad signature");
    let off = 8,
        w,
        h,
        ch;
    const idat = [],
        text = {};
    while (off < png.length) {
        const len = ((png[off] << 24) | (png[off + 1] << 16) | (png[off + 2] << 8) | png[off + 3]) >>> 0;
        const type = dec.decode(png.subarray(off + 4, off + 8)),
            data = png.subarray(off + 8, off + 8 + len);
        const crc =
            ((png[off + 8 + len] << 24) |
                (png[off + 9 + len] << 16) |
                (png[off + 10 + len] << 8) |
                png[off + 11 + len]) >>>
            0;
        if (CRC32(png.subarray(off + 4, off + 8 + len)) !== crc)
            throw new Error(`chunk ${type} at byte ${off}: CRC mismatch`);
        if (type === "IHDR") {
            w = ((data[0] << 24) | (data[1] << 16) | (data[2] << 8) | data[3]) >>> 0;
            h = ((data[4] << 24) | (data[5] << 16) | (data[6] << 8) | data[7]) >>> 0;
            ch = data[9] === 2 ? 3 : 1;
            if (data[8] !== 8 || data[12] !== 0) throw new Error("only 8-bit non-interlaced images");
        } else if (type === "IDAT") idat.push(data);
        else if (type === "tEXt") {
            const z = data.indexOf(0);
            text[dec.decode(data.subarray(0, z))] = dec.decode(data.subarray(z + 1));
        } else if (type === "IEND") break;
        off += 12 + len;
    }
    const z = new Uint8Array(idat.reduce((s, d) => s + d.length, 0));
    let o = 0;
    for (const d of idat) {
        z.set(d, o);
        o += d.length;
    }
    if ((z[0] & 15) !== 8 || ((z[0] << 8) | z[1]) % 31 !== 0) throw new Error("bad zlib header");
    const raw = inflate(z.subarray(2, z.length - 4));
    if (
        Adler32(raw) !==
        ((z[z.length - 4] << 24) | (z[z.length - 3] << 16) | (z[z.length - 2] << 8) | z[z.length - 1]) >>> 0
    )
        throw new Error("zlib Adler-32 mismatch");
    const stride = w * ch,
        data = new Uint8Array(h * stride);
    for (let y = 0; y < h; y++) {
        const f = raw[y * (stride + 1)],
            src = raw.subarray(y * (stride + 1) + 1, (y + 1) * (stride + 1));
        for (let i = 0; i < stride; i++) {
            const a = i >= ch ? data[y * stride + i - ch] : 0,
                b = y ? data[(y - 1) * stride + i] : 0,
                c = y && i >= ch ? data[(y - 1) * stride + i - ch] : 0;
            data[y * stride + i] = (src[i] + [0, a, b, (a + b) >> 1, paeth(a, b, c)][f]) & 255;
        }
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
    const out = make(w, h, img.channels),
        sx = img.width / w,
        sy = img.height / h;
    for (let y = 0; y < h; y++)
        for (let x = 0; x < w; x++) {
            const fx = Math.min(img.width - 1.001, (x + 0.5) * sx - 0.5),
                fy = Math.min(img.height - 1.001, (y + 0.5) * sy - 0.5);
            const x0 = Math.max(0, Math.floor(fx)),
                y0 = Math.max(0, Math.floor(fy)),
                dx = Math.max(0, fx - x0),
                dy = Math.max(0, fy - y0);
            for (let c = 0; c < img.channels; c++) {
                const p = (xx, yy) => img.data[(yy * img.width + xx) * img.channels + c];
                out.data[(y * w + x) * img.channels + c] = Math.round(
                    p(x0, y0) * (1 - dx) * (1 - dy) +
                        p(x0 + 1, y0) * dx * (1 - dy) +
                        p(x0, y0 + 1) * (1 - dx) * dy +
                        p(x0 + 1, y0 + 1) * dx * dy,
                );
            }
        }
    return out;
}
function blur(img, sigma) {
    // separable Gaussian, rows and columns as SIMD axpy over planes
    const r = Math.ceil(sigma * 3),
        k = Array.from({ length: 2 * r + 1 }, (_, i) => Math.exp(-((i - r) ** 2) / (2 * sigma * sigma)));
    const ks = k.reduce((a, b) => a + b, 0);
    const { width: w, height: h, channels: ch } = img,
        out = make(w, h, ch);
    for (let c = 0; c < ch; c++) {
        const plane = new Float32Array(w * h);
        for (let i = 0; i < w * h; i++) plane[i] = img.data[i * ch + c];
        const pass = (src, horizontal) => {
            const dst = new Float32Array(w * h),
                shifted = new Float32Array(w * h);
            k.forEach((kw, i) => {
                const d = i - r;
                for (let y = 0; y < h; y++)
                    for (let x = 0; x < w; x++) {
                        const sx = horizontal ? Math.min(w - 1, Math.max(0, x + d)) : x,
                            sy = horizontal ? y : Math.min(h - 1, Math.max(0, y + d));
                        shifted[y * w + x] = src[sy * w + sx];
                    }
                axpy(dst, kw / ks, shifted);
            });
            return dst;
        };
        const res = pass(pass(plane, true), false);
        for (let i = 0; i < w * h; i++) out.data[i * ch + c] = Math.max(0, Math.min(255, Math.round(res[i])));
    }
    return out;
}
function unsharp(img, sigma = 1.5, amount = 0.8) {
    const b = blur(img, sigma),
        out = make(img.width, img.height, img.channels);
    for (let i = 0; i < img.data.length; i++)
        out.data[i] = Math.max(0, Math.min(255, Math.round(img.data[i] + amount * (img.data[i] - b.data[i]))));
    return out;
}
function crop(img, x0, y0, w, h) {
    const out = make(w, h, img.channels);
    for (let y = 0; y < h; y++)
        out.data.set(
            img.data.subarray(
                ((y0 + y) * img.width + x0) * img.channels,
                ((y0 + y) * img.width + x0 + w) * img.channels,
            ),
            y * w * img.channels,
        );
    return out;
}
function gray(img) {
    const out = make(img.width, img.height, 1);
    for (let i = 0; i < img.width * img.height; i++)
        out.data[i] = Math.round(
            0.2126 * img.data[i * 3] + 0.7152 * img.data[i * 3 + 1] + 0.0722 * img.data[i * 3 + 2],
        );
    return out;
}
function equalize(g) {
    const hist = new Float32Array(256);
    for (const v of g.data) hist[v]++;
    scale(hist, 1 / g.data.length);
    const cdf = [];
    let acc = 0;
    for (let v = 0; v < 256; v++) {
        acc += hist[v];
        cdf.push(acc);
    }
    const out = make(g.width, g.height, 1);
    for (let i = 0; i < g.data.length; i++) out.data[i] = Math.round(cdf[g.data[i]] * 255);
    return out;
}
function boxResize(img, w, h) {
    // area averaging: a large downscale without aliasing
    const out = make(w, h, img.channels),
        sx = img.width / w,
        sy = img.height / h;
    for (let y = 0; y < h; y++)
        for (let x = 0; x < w; x++)
            for (let c = 0; c < img.channels; c++) {
                let s = 0,
                    n = 0;
                for (let yy = Math.floor(y * sy); yy < Math.floor((y + 1) * sy); yy++)
                    for (let xx = Math.floor(x * sx); xx < Math.floor((x + 1) * sx); xx++) {
                        s += img.data[(yy * img.width + xx) * img.channels + c];
                        n++;
                    }
                out.data[(y * w + x) * img.channels + c] = Math.round(s / Math.max(1, n));
            }
    return out;
}
function dhash(img) {
    const g = boxResize(gray(img), 9, 8);
    let h = 0n;
    for (let y = 0; y < 8; y++)
        for (let x = 0; x < 8; x++) h = (h << 1n) | (g.data[y * 9 + x] > g.data[y * 9 + x + 1] ? 1n : 0n);
    return h;
}
const hamming = (a, b) => {
    let x = a ^ b,
        n = 0;
    while (x) {
        x &= x - 1n;
        n++;
    }
    return n;
};

// =============================================================================
// Test images: a gradient, a landscape-like scene, a document scan, and noise
// =============================================================================
function scene(seed, w = 320, h = 220) {
    const r = new Random(seed),
        img = make(w, h),
        sun = [r.nextBounded(w), 40 + r.nextBounded(60)],
        hills = Array.from({ length: 4 }, () => [r.nextFloat() * 0.03, r.nextFloat() * 6, 120 + r.nextBounded(60)]);
    const horizon = Float64Array.from({ length: w }, (_, x) =>
        Math.min(...hills.map(([f, p, base]) => base + 20 * Math.sin(x * f + p))),
    ); // ground below the lowest hill line
    const d = img.data,
        put = (i, v) => {
            const q = Math.round(v + r.normal(0, 3));
            d[i] = q < 0 ? 0 : q > 255 ? 255 : q;
        };
    for (let y = 0; y < h; y++)
        for (let x = 0; x < w; x++) {
            const i = (y * w + x) * 3,
                dx = x - sun[0],
                dy = y - sun[1];
            if (dx * dx + dy * dy < 22 * 22) {
                put(i, 255);
                put(i + 1, 220);
                put(i + 2, 120);
            } else if (y > horizon[x]) {
                put(i, 40 + y / 4);
                put(i + 1, 90 + y / 6);
                put(i + 2, 30);
            } else {
                put(i, 80 + y / 3);
                put(i + 1, 140 + y / 4);
                put(i + 2, 230);
            }
        }
    return img;
}
function documentScan(w = 300, h = 200) {
    const img = make(w, h);
    for (let y = 0; y < h; y++)
        for (let x = 0; x < w; x++) {
            const line = y % 16 > 11 && x > 20 && x < w - 20 && (x * 7 + y) % 13 < 9;
            const v = line ? 40 : 238 - (x + y) / 30;
            img.data.set([v, v, v - 4], (y * w + x) * 3);
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
for (const [name, img] of Object.entries(sources)) {
    const sizes = {};
    for (const f of ["none", "sub", "up", "paeth", "adaptive"]) sizes[f] = encodePNG(img, { filter: f }).png.length;
    const { png, filters } = encodePNG(img, { text: { Title: name, Software: "dynajs image-pipeline" } });
    writeFile(work.join(`${name}.png`), png);
    const back = decodePNG(png);
    const same = back.width === img.width && back.data.every((v, i) => v === img.data[i]);
    if (!same) results[`${name}: decoded pixels differ`] = false;
    codecRows.push([
        name,
        `${img.width}x${img.height}`,
        ...["none", "sub", "up", "paeth", "adaptive"].map((f) => String(sizes[f])),
        filters.join("/"),
        back.text.Title === name ? "ok" : "missing",
    ]);
    sources[name].sizes = sizes;
}
results["every image round-trips losslessly with its tEXt metadata; the file sniffs as image/png"] =
    codecRows.every((r) => r[8] === "ok") && sniffType(work.join("beach.png")) === "image/png";
results["adaptive filtering crushes a gradient and cannot compress noise"] =
    sources.gradient.sizes.adaptive < sources.gradient.sizes.none / 5 &&
    sources.noise.sizes.adaptive >= sources.noise.sizes.none * 0.98;
const corrupt = readBytes(work.join("beach.png"));
corrupt[60] ^= 0xff;
let crcError = null;
try {
    decodePNG(corrupt);
} catch (e) {
    crcError = e.message;
}
results[`a flipped byte is caught: ${crcError}`] = /CRC mismatch/.test(crcError || "");

// =============================================================================
// Operations and perceptual hashing
// =============================================================================
const beach = sources.beach;
const small = resize(beach, 160, 110),
    blurred = blur(beach, 2),
    sharp = unsharp(small),
    eq = equalize(gray(sources.memo));
const variance = (img) => {
    const g = gray(img).data;
    let m = 0;
    for (const v of g) m += v;
    m /= g.length;
    let s = 0;
    for (const v of g) s += (v - m) ** 2;
    return s / g.length;
};
const edge = (img) => {
    const g = gray(img),
        w = g.width;
    let e = 0;
    for (let i = 1; i < g.data.length; i++) e += Math.abs(g.data[i] - g.data[i - 1]);
    return e / g.data.length;
};
results["blur lowers edge energy, unsharp mask raises it"] =
    edge(blurred) < edge(beach) * 0.6 && edge(sharp) > edge(small);
results["resizing preserves the overall tonal distribution"] =
    Math.abs(variance(small) - variance(beach)) / variance(beach) < 0.1;
const eqHist = new Set(eq.data);
results["histogram equalisation stretches the scan to the full range"] =
    Math.max(...eq.data) === 255 && eqHist.size > 10;
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
const parent = os.Worker.parent;
parent.onmessage = (e) => {
    if (e.data === "stop") { parent.onmessage = null; return; }
    const { id, width, height, data, tw, th } = e.data, out = new Uint8Array(tw * th * 3), sx = width / tw, sy = height / th;
    for (let y = 0; y < th; y++) for (let x = 0; x < tw; x++) for (let c = 0; c < 3; c++) {
        let s = 0, n = 0;
        for (let yy = Math.floor(y * sy); yy < Math.floor((y + 1) * sy); yy++) for (let xx = Math.floor(x * sx); xx < Math.floor((x + 1) * sx); xx++) { s += data[(yy * width + xx) * 3 + c]; n++; }
        out[(y * tw + x) * 3 + c] = Math.round(s / Math.max(1, n));
    }
    parent.postMessage({ id, data: out });
};
`,
);
const uploads = Array.from({ length: 24 }, (_, i) => ({ id: `upload-${i}`, img: scene(100 + i, 480, 320) }));
const pool = Array.from({ length: 4 }, () => new os.Worker(workerSrc.toString()));
const tStart = Date.now();
const thumbs = await new Promise((resolve) => {
    const results = new Map();
    let next = 0;
    const feed = (w) => {
        if (next >= uploads.length) return;
        const u = uploads[next++];
        w.postMessage({ id: u.id, width: u.img.width, height: u.img.height, data: u.img.data, tw: 96, th: 64 });
    };
    for (const w of pool) {
        w.onmessage = (e) => {
            results.set(e.data.id, { width: 96, height: 64, channels: 3, data: e.data.data });
            if (results.size === uploads.length) resolve(results);
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
results[`24 thumbnails produced by 4 worker threads in ${farmMs} ms`] =
    thumbs.size === 24 && [...thumbs.values()].every((t) => t.data.length === 96 * 64 * 3);
results["every thumbnail keeps its source's perceptual hash"] = uploads.every(
    (u) => hamming(dhash(u.img), dhash(thumbs.get(u.id))) <= 6,
);

// =============================================================================
// Image service
// =============================================================================
const variantsCache = new Map();
let renders = 0;
const app = new App({ port: 0 });
app.get("/img/:id", (req) => {
    const src = sources[req.params.id];
    if (!src) return { status: 404, body: "no such image" };
    const w = Math.min(1024, Number(req.query.w) || src.width),
        sigma = Math.min(5, Number(req.query.blur) || 0),
        fmt = req.query.fmt === "gray" ? "gray" : "rgb";
    if (!Number.isFinite(w) || w < 8) return { status: 400, body: "w must be a number >= 8" };
    const key = SHA256Hex(`${SHA256Hex(src.data)}|${w}|${sigma}|${fmt}`).slice(0, 24),
        etag = `"${key}"`;
    if (req.headers["if-none-match"] === etag) return { status: 304, body: "", headers: { etag } };
    if (!variantsCache.has(key)) {
        renders++;
        let img = resize(src, w, Math.round((src.height * w) / src.width));
        if (sigma) img = blur(img, sigma);
        if (fmt === "gray") img = gray(img);
        variantsCache.set(key, encodePNG(img).png);
    }
    return {
        body: variantsCache.get(key),
        contentType: "image/png",
        headers: { etag, "cache-control": "public, max-age=31536000" },
    };
});
app.start();
const base = `http://127.0.0.1:${app.port}`;
const v1 = await fetch(`${base}/img/beach?w=120&blur=1&fmt=gray`),
    b1 = new Uint8Array(await v1.arrayBuffer());
const v2 = await fetch(`${base}/img/beach?w=120&blur=1&fmt=gray`);
await v2.arrayBuffer();
const v3 = await fetch(`${base}/img/beach?w=120&blur=1&fmt=gray`, {
    headers: { "if-none-match": v1.headers.get("etag") },
});
await v3.text();
const bad = await fetch(`${base}/img/beach?w=2`);
await bad.text();
const missing = await fetch(`${base}/img/nope`);
await missing.text();
const decoded = decodePNG(b1);
results["the service renders a grayscale variant once, then serves it from cache and as 304"] =
    v1.status === 200 && decoded.width === 120 && decoded.channels === 1 && renders === 1 && v3.status === 304;
results["invalid parameters and unknown images are refused"] = bad.status === 400 && missing.status === 404;
app.close();

print("image-pipeline");
print(table([["image", "size", "none", "sub", "up", "paeth", "adaptive", "rows N/S/U/A/P", "tEXt"], ...codecRows]));
print(
    `  dHash      ${Object.entries(dists)
        .map(([k, d]) => `${k} ${d}`)
        .join(", ")} | ${Object.entries(others)
        .map(([k, d]) => `${k} ${d}`)
        .join(", ")} bits`,
);
print(`  farm       24 uploads 480x320 -> 96x64 in ${farmMs} ms on 4 threads`);
print(`  service    variant ${b1.length} bytes PNG, ${renders} render(s) for 3 requests`);
removeAll(work);
check(results);

function check(results) {
    const names = Object.keys(results);
    const failed = names.filter((name) => !results[name]);
    for (const name of failed) print("FAIL " + name);
    if (failed.length) throw new Error(`${failed.length} of ${names.length} checks failed`);
}
