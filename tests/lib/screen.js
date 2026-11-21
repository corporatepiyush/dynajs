// Whole-screen assertions for pixel tests (dyna:sdl, dyna:raylib, dyna:ui).
//
// An expected Frame is built from what the test drew -- background, then shapes in draw order -- never
// from what the code under test produced. Every pixel carries a class:
//   exact     one RGBA value
//   oneOf     any of a small set (text glyphs over a known background, blended edges)
//   edge      either of the two colours meeting at a shape's boundary (rasterisers disagree on
//             which side owns a pixel whose centre sits on the edge)
// compareFrame() checks EVERY pixel of the actual readback against its class, so a stray write
// anywhere on the screen fails the test, not only a wrong value at a sampled point.

const pack = (c) => ((c[0] << 24) | (c[1] << 16) | (c[2] << 8) | (c.length > 3 ? c[3] : 255)) >>> 0;
const unpack = (u) => [u >>> 24, (u >>> 16) & 255, (u >>> 8) & 255, u & 255];
const show = (u) => unpack(u).join(",");

export class Frame {
    constructor(width, height, background) {
        this.width = width;
        this.height = height;
        this.want = new Uint32Array(width * height).fill(pack(background));
        this.sets = [null];
        this.cls = new Uint16Array(width * height);
    }
    inside(x, y) { return x >= 0 && y >= 0 && x < this.width && y < this.height; }
    pixel(x, y, rgba) {
        if (!this.inside(x, y)) return this;
        const i = y * this.width + x;
        this.want[i] = pack(rgba);
        this.cls[i] = 0;
        return this;
    }
    // Pixels whose centre (px + 0.5, py + 0.5) lies inside [x, x + w) x [y, y + h).
    rect(x, y, w, h, rgba) {
        const v = pack(rgba);
        for (let py = Math.max(0, Math.ceil(y - 0.5)); py < Math.min(this.height, Math.ceil(y + h - 0.5)); py++)
            for (let px = Math.max(0, Math.ceil(x - 0.5)); px < Math.min(this.width, Math.ceil(x + w - 0.5)); px++) {
                this.want[py * this.width + px] = v;
                this.cls[py * this.width + px] = 0;
            }
        return this;
    }
    // A region where any listed colour may appear (each pixel also keeps what was under it).
    oneOf(x, y, w, h, colours) {
        for (let py = Math.max(0, y); py < Math.min(this.height, y + h); py++)
            for (let px = Math.max(0, x); px < Math.min(this.width, x + w); px++) {
                const i = py * this.width + px;
                const set = new Set(colours.map(pack));
                set.add(this.want[i]);
                if (this.cls[i]) for (const u of this.sets[this.cls[i]]) set.add(u);
                this.sets.push(set);
                this.cls[i] = this.sets.length - 1;
            }
        return this;
    }
    // A filled triangle: centres more than `band` px inside every edge are exact, centres within
    // `band` of an edge may be either colour, the rest keep what was there.
    triangle(x1, y1, x2, y2, x3, y3, rgba, band = 1) {
        const v = pack(rgba);
        const edges = [[x1, y1, x2, y2], [x2, y2, x3, y3], [x3, y3, x1, y1]];
        const area = (x2 - x1) * (y3 - y1) - (x3 - x1) * (y2 - y1);
        const s = Math.sign(area);
        for (let py = 0; py < this.height; py++)
            for (let px = 0; px < this.width; px++) {
                const cx = px + 0.5, cy = py + 0.5;
                let min = Infinity;
                for (const [ax, ay, bx, by] of edges) {
                    const len = Math.hypot(bx - ax, by - ay) || 1;
                    min = Math.min(min, (s * ((bx - ax) * (cy - ay) - (by - ay) * (cx - ax))) / len);
                }
                const i = py * this.width + px;
                if (min > band) { this.want[i] = v; this.cls[i] = 0; }
                else if (min > -band) this.edge(i, v);
            }
        return this;
    }
    // A text run: each pixel is a channel-wise blend between what was under it and `ink`
    // (anti-aliased glyph coverage); glyph shapes themselves are pinned by the C suite.
    glyphs(x, y, w, h, ink) {
        const v = pack(ink);
        for (let py = Math.max(0, y); py < Math.min(this.height, y + h); py++)
            for (let px = Math.max(0, x); px < Math.min(this.width, x + w); px++) {
                const i = py * this.width + px;
                this.sets.push({ blend: true, a: this.want[i], b: v });
                this.cls[i] = this.sets.length - 1;
            }
        return this;
    }
    edge(i, v) {
        const set = new Set([v, this.want[i]]);
        if (this.cls[i]) for (const u of this.sets[this.cls[i]]) set.add(u);
        this.sets.push(set);
        this.cls[i] = this.sets.length - 1;
    }
    // A filled circle: centres more than `band` inside the radius are exact, within `band` of it either way.
    circle(cx, cy, r, rgba, band = 1) {
        const v = pack(rgba);
        for (let py = 0; py < this.height; py++)
            for (let px = 0; px < this.width; px++) {
                const d = r - Math.hypot(px + 0.5 - cx, py + 0.5 - cy);
                const i = py * this.width + px;
                if (d > band) { this.want[i] = v; this.cls[i] = 0; }
                else if (d > -band) this.edge(i, v);
            }
        return this;
    }
    // An axis-aligned rect whose boundary pixels may go either way (scaled or rotated targets).
    rectEdge(x, y, w, h, rgba, band = 1) {
        const v = pack(rgba);
        for (let py = 0; py < this.height; py++)
            for (let px = 0; px < this.width; px++) {
                const cx = px + 0.5, cy = py + 0.5;
                const d = Math.min(cx - x, x + w - cx, cy - y, y + h - cy);
                const i = py * this.width + px;
                if (d > band) { this.want[i] = v; this.cls[i] = 0; }
                else if (d > -band) this.edge(i, v);
            }
        return this;
    }
}

function between(got, a, b) {
    for (let s = 24; s >= 0; s -= 8) {
        const g = (got >>> s) & 255, x = (a >>> s) & 255, y = (b >>> s) & 255;
        if (g < Math.min(x, y) || g > Math.max(x, y)) return false;
    }
    return true;
}

// How many pixels inside a box are exactly `rgba` (a glyph box that holds no ink is a missing text run).
export function countColour(actual, x, y, w, h, rgba) {
    const v = pack(rgba);
    let n = 0;
    for (let py = y; py < y + h; py++)
        for (let px = x; px < x + w; px++) {
            const i = (py * actual.width + px) * 4, p = actual.pixels;
            if ((((p[i] << 24) | (p[i + 1] << 16) | (p[i + 2] << 8) | p[i + 3]) >>> 0) === v) n++;
        }
    return n;
}

// actual: { width, height, pixels: Uint8Array RGBA } (dyna:sdl readPixels, raylib readPixels, ui).
export function compareFrame(actual, expected) {
    if (actual.width !== expected.width || actual.height !== expected.height)
        return { ok: false, mismatches: -1, report: `frame is ${actual.width}x${actual.height}, expected ${expected.width}x${expected.height}` };
    const W = expected.width, H = expected.height, px = actual.pixels, bad = new Uint8Array(W * H);
    let mismatches = 0;
    const first = [];
    for (let i = 0; i < W * H; i++) {
        const got = ((px[i * 4] << 24) | (px[i * 4 + 1] << 16) | (px[i * 4 + 2] << 8) | px[i * 4 + 3]) >>> 0;
        const c = expected.cls[i];
        const good = c === 0 ? got === expected.want[i] : expected.sets[c].blend ? between(got, expected.sets[c].a, expected.sets[c].b) : expected.sets[c].has(got);
        if (!good) {
            bad[i] = 1;
            if (mismatches++ < 5) first.push(`(${i % W},${(i / W) | 0}) got ${show(got)} want ${c === 0 ? show(expected.want[i]) : expected.sets[c].blend ? "between " + show(expected.sets[c].a) + " and " + show(expected.sets[c].b) : [...expected.sets[c]].map(show).join(" | ")}`);
        }
    }
    if (!mismatches) return { ok: true, mismatches: 0, report: `all ${W * H} pixels match` };
    const step = Math.max(1, Math.ceil(Math.max(W, H) / 64));
    let map = "";
    for (let y = 0; y < H; y += step) {
        let row = "";
        for (let x = 0; x < W; x += step) {
            let any = 0;
            for (let yy = y; yy < Math.min(H, y + step); yy++) for (let xx = x; xx < Math.min(W, x + step); xx++) any |= bad[yy * W + xx];
            row += any ? "X" : ".";
        }
        map += "    " + row + "\n";
    }
    return { ok: false, mismatches, report: `${mismatches} of ${W * H} pixels differ: ${first.join("; ")}\n${map}` };
}

// ok(cond, message) is the suite's own assertion function.
export function assertFrame(ok, actual, expected, message) {
    const r = compareFrame(actual, expected);
    ok(r.ok, message + (r.ok ? "" : "\n  " + r.report));
    return r.ok;
}
