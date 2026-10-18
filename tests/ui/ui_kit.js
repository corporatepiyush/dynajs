// Expected-frame building blocks for the dyna:ui suites. Every number here comes from the layout
// contract in dynajs.d.ts (theme defaults, element sizes), never from what the code produced:
//   ProggyClean at 13 px: line height 13, ascent 10, every glyph advances 7 px.
//   padX 6, padY 4 -> a frame-height control is 13 + 2 * 4 = 21 px tall.
//   spacing 4 between stacked widgets; the screen is padded by windowPad 8; borders are 1 px.
//   Box edges are rounded to whole pixels half away from zero after layout.
import { Frame, assertFrame, countColour } from "../lib/screen.js";

export const LH = 13, ADV = 7, PADX = 6, PADY = 4, FH = 21, GAP = 4, WPAD = 8, BW = 1;

export const T = {
    text: 0xe6e6e6ff, textDim: 0x9aa0a6ff, onAccent: 0xffffffff, bg: 0x202428ff, panel: 0x262b30ff,
    frame: 0x15181bff, frameHot: 0x1b1f23ff, frameActive: 0x101214ff, button: 0x3a4048ff,
    buttonHot: 0x4a525cff, buttonActive: 0x2f343aff, accent: 0x3d7effff, accentHot: 0x5a92ffff,
    border: 0x50565eff, title: 0x2c3238ff, titleActive: 0x34506eff, selection: 0x3d7eff66,
    popup: 0x2a2f35ff, shade: 0x00000080, tooltip: 0x111315f0, link: 0x6fa8ffff,
};

export const rgba = (u) => [u >>> 24, (u >>> 16) & 255, (u >>> 8) & 255, u & 255];

// The source-over rule the software backend states: c = (s * a + d * (255 - a) + 127) / 255.
export function over(dst, src) {
    const s = rgba(src), d = rgba(dst), a = s[3], ia = 255 - a;
    if (a === 255) return src >>> 0;
    const ch = (k) => Math.floor((s[k] * a + d[k] * ia + 127) / 255);
    const out = [ch(0), ch(1), ch(2), a + Math.floor((d[3] * ia + 127) / 255)];
    return ((out[0] << 24) | (out[1] << 16) | (out[2] << 8) | out[3]) >>> 0;
}

export const snap = (v) => Math.sign(v) * Math.round(Math.abs(v));
export const tw = (s) => [...s].length * ADV;

export function frame(w, h) { return new Frame(w, h, rgba(T.bg)); }

export function rect(F, x, y, w, h, c) {
    for (let py = y; py < y + h; py++)
        for (let px = x; px < x + w; px++) {
            if (!F.inside(px, py)) continue;
            const i = py * F.width + px;
            F.want[i] = over(F.want[i], c);
            F.cls[i] = 0;
        }
    return F;
}

export function ring(F, x, y, w, h, c, bw = BW) {
    rect(F, x, y, w, bw, c);
    rect(F, x, y + h - bw, w, bw, c);
    rect(F, x, y + bw, bw, h - 2 * bw, c);
    rect(F, x + w - bw, y + bw, bw, h - 2 * bw, c);
    return F;
}

// The signed distance the rasteriser states for a rounded rect: from the pixel centre to the
// rect shrunk by r, minus r. Inside by 0.5 -> exact, outside by 0.5 -> untouched, else a blend.
export function sdRound(x, y, w, h, r, px, py) {
    r = Math.min(r, w / 2, h / 2);
    const cx = Math.min(Math.max(px, x + r), x + w - r), cy = Math.min(Math.max(py, y + r), y + h - r);
    const dx = px - cx, dy = py - cy;
    if (dx === 0 && dy === 0) return -Infinity;
    return Math.hypot(dx, dy) - r;
}

export function roundRect(F, x, y, w, h, r, c, hole = 0) {
    for (let py = Math.floor(y); py < Math.ceil(y + h); py++)
        for (let px = Math.floor(x); px < Math.ceil(x + w); px++) {
            if (!F.inside(px, py)) continue;
            const d = sdRound(x, y, w, h, r, px + 0.5, py + 0.5);
            const di = hole > 0 ? sdRound(x + hole, y + hole, w - 2 * hole, h - 2 * hole, Math.max(0, r - hole), px + 0.5, py + 0.5) : Infinity;
            const i = py * F.width + px;
            const solidOuter = d <= -0.5, outsideOuter = d >= 0.5;
            const solidInner = di <= -0.5, outsideInner = di >= 0.5;
            if (outsideOuter) continue;
            if (hole > 0 && solidInner) continue;
            if (solidOuter && (hole === 0 || outsideInner)) {
                F.want[i] = over(F.want[i], c);
                F.cls[i] = 0;
            } else widen(F, i, c);
        }
    return F;
}

// A pixel partly covered by `c`: its range grows to hold every blend of c over what it could be.
export function widen(F, i, c) {
    const prev = F.cls[i] && F.sets[F.cls[i]].blend ? [F.sets[F.cls[i]].a, F.sets[F.cls[i]].b] : [F.want[i], F.want[i]];
    const cand = [prev[0], prev[1], over(prev[0], c), over(prev[1], c)].map(rgba);
    let lo = 0, hi = 0;
    for (let k = 0; k < 4; k++) {
        lo = (lo << 8) | Math.min(...cand.map((v) => v[k]));
        hi = (hi << 8) | Math.max(...cand.map((v) => v[k]));
    }
    F.sets.push({ blend: true, a: lo >>> 0, b: hi >>> 0 });
    F.cls[i] = F.sets.length - 1;
}

// Pixels a test does not pin (an animation frame, a triangle's anti-aliased rim).
export function any(F, x, y, w, h) {
    for (let py = y; py < y + h; py++)
        for (let px = x; px < x + w; px++) {
            if (!F.inside(px, py)) continue;
            const i = py * F.width + px;
            F.sets.push({ blend: true, a: 0, b: 0xffffffff });
            F.cls[i] = F.sets.length - 1;
        }
    return F;
}

export function glyphs(F, x, y, s, ink, px = 13) {
    const n = [...s].length;
    if (n) F.glyphs(x, y, n * ADV * (px / 13), px, rgba(ink));
    return F;
}

export function check(ok, c, F, message, inks = []) {
    const shot = { width: c.width, height: c.height, pixels: c.pixels() };
    const good = assertFrame(ok, shot, F, message);
    for (const [x, y, w, h, ink, min] of inks) {
        const n = countColour(shot, x, y, w, h, rgba(ink));
        ok(n >= min, `${message}: the text at ${x},${y} holds ink (${n} pixels of ${ink.toString(16)}, at least ${min} expected)`);
    }
    return good;
}

// A button-like box: fill, centred text, 1 px border drawn after the content.
export function buttonBox(F, x, y, label, fill, ink = T.text, border = T.border) {
    const w = tw(label) + 2 * PADX;
    rect(F, x, y, w, FH, fill);
    glyphs(F, x + PADX, y + PADY, label, ink);
    ring(F, x, y, w, FH, border);
    return w;
}

export function ink(x, y, s, colour = T.text) { return [x, y, tw(s), LH, colour, [...s].length * 3]; }
