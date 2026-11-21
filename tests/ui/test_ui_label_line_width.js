// covers: c:src/core/dyn-ui.c
// A text element that does not wrap and whose run has no newline and no missing glyph takes its
// single line's width from the cached run (the run measured it when it was shaped) instead of
// walking every glyph again in ui_wrap; a frame of 2000 labels laid out 19% faster. Each run now
// records whether it is that simple (8 more bytes per cached run). The oracles: ProggyClean at
// 13 px advances every glyph 7 px, so a label is 7 * code points wide and 13 px per line; with
// Inter a label is exactly as wide as the same text with a trailing newline, which goes the
// per-glyph way; wrapped text still breaks into several lines. The glyph emitter keeps its pen in
// a local (it was stored and reloaded through the caller's float pointer for every glyph, which
// aliased the quad stores; 4-6% of a text frame): a cut button label's three dots must follow its
// last letter 7 px apart, and a kerned pair must sit the same at the start of a label as after
// another glyph. The bundled fonts carry no kerning stb_truetype reads, so the kerning sections
// load the host's Arial when it exists (macOS) and are skipped elsewhere.
import * as ui from "dyna:ui";
import { Path, exists } from "dyna:file";
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_ui_label_line_width");

let pass = 0, fail = 0;
const eq = (a, b, m) => { if (a === b) pass++; else { fail++; print("FAIL " + m + ": got " + a + ", want " + b); } };

function rects(ctx, texts, opts) {
    const r = [];
    const body = (u) => { r.length = 0; for (const t of texts) { if (opts && opts.wrap) u.text(t, { wrap: true }); else u.label(t); r.push(u.lastRect()); } };
    ctx.frame(body);
    ctx.frame(body);
    return r.slice();
}

const ASCII = [];
for (let n = 0; n <= 60; n++) {
    let s = "";
    for (let i = 0; i < n; i++) s += String.fromCharCode(33 + ((i * 7 + n) % 94));
    ASCII.push(s);
}
const KERN = ["AV", "AVAVAV", "To Ta Tu", "Wa We yA LT", "Yo! P. F,", "AWAY VAT", "kerning: AVAWAY", "Ünïcödé ÅV", "ff fi fl", "LTA Va"];

__kitS.section("ProggyClean label width and height", () => {
    const c = new ui.Context({ width: 1000, height: 3000, clock: "manual" });
    c.theme({ font: 0, fontSize: 13 });
    const r = rects(c, ASCII);
    for (let i = 0; i < ASCII.length; i++) {
        eq(r[i] && r[i].width, 7 * ASCII[i].length, "ProggyClean label width of " + ASCII[i].length + " glyphs");
        eq(r[i] && r[i].height, 13, "ProggyClean label height of " + ASCII[i].length + " glyphs");
    }
    const ml = rects(c, ["a\nbb", "abc\n", "x\ny\nzzzz"]);
    eq(ml[0].width + "x" + ml[0].height, "14x26", "two-line label");
    eq(ml[1].width + "x" + ml[1].height, "21x26", "label ending in a newline");
    eq(ml[2].width + "x" + ml[2].height, "28x39", "three-line label");
});

__kitS.section("Inter label width equals its newline twin", () => {
for (const px of [11, 14, 17, 24]) {
    const c = new ui.Context({ width: 2000, height: 3000, clock: "manual" });
    c.theme({ font: 1, fontSize: px });
    const plain = rects(c, KERN.concat(ASCII.slice(1, 40)));
    const nl = rects(c, KERN.concat(ASCII.slice(1, 40)).map((s) => s + "\n"));
    for (let i = 0; i < plain.length; i++)
        eq(plain[i] && plain[i].width, nl[i] && nl[i].width, "Inter " + px + " px label width equals its newline twin #" + i);
}
});

__kitS.section("wrapped text still breaks", () => {
    const c = new ui.Context({ width: 300, height: 2000, clock: "manual" });
    c.theme({ font: 0, fontSize: 13 });
    const w = rects(c, ["word ".repeat(40)], { wrap: true });
    eq(w[0].height > 13 * 3, true, "wrapped text still breaks (height " + w[0].height + ")");
    eq(w[0].width <= 300, true, "wrapped text fits the width (" + w[0].width + ")");
});

function glyphQuads(ctx, body) {
    ctx.frame(body);
    ctx.frame(body);
    const v = ctx.geometry().vertices, q = [];
    for (let i = 0; i + 32 <= v.length; i += 32) {
        const xs = [v[i], v[i + 8], v[i + 16], v[i + 24]], ys = [v[i + 1], v[i + 9], v[i + 17], v[i + 25]];
        const us = [v[i + 6], v[i + 14], v[i + 22], v[i + 30]];
        if (Math.max(...us) > Math.min(...us))
            q.push({ x: Math.min(...xs), y: Math.min(...ys), w: Math.max(...xs) - Math.min(...xs), h: Math.max(...ys) - Math.min(...ys) });
    }
    return q;
}

__kitS.section("a cut label ends in dots after the last letter", () => {
    const c = new ui.Context({ width: 300, height: 200, clock: "manual" });
    c.theme({ font: 0, fontSize: 13 });
    for (const w of [60, 90, 130]) {
        const q = glyphQuads(c, (u) => u.column({ width: w }, (u) => u.button("M".repeat(40))));
        const dots = q.filter((g) => g.h <= 3), letters = q.filter((g) => g.h > 3);
        eq(dots.length, 3, "a cut button label ends in three dots (column " + w + ")");
        eq(letters.length > 0 && letters.every((g, i) => i === 0 || g.x - letters[i - 1].x === 7), true, "the letters sit 7 px apart (column " + w + ")");
        const last = Math.max(...letters.map((g) => g.x));
        eq(dots.every((d, i) => d.x > last && (i === 0 || d.x - dots[i - 1].x === 7)), true, "the dots follow the last letter 7 px apart (column " + w + ")");
    }
});

const ARIAL = "/System/Library/Fonts/Supplemental/Arial.ttf";
__kitS.section("kerned pairs with a host font", () => {
if (exists(new Path(ARIAL))) {
    const c = new ui.Context({ width: 1600, height: 400, clock: "manual" });
    c.theme({ font: c.loadFont(ARIAL), fontSize: 64 });
    const gap = (q, i) => q[i + 1].x - q[i].x;
    const av = glyphQuads(c, (u) => u.label("xAV")), aa = glyphQuads(c, (u) => u.label("xAA"));
    eq(gap(av, 1) < gap(aa, 1) - 1, true, "Arial kerns AV tighter than AA (" + gap(av, 1) + " vs " + gap(aa, 1) + ")");
    for (const pair of ["AV", "To", "LT", "Yo", "Wa", "AY", "P."]) {
        const a = glyphQuads(c, (u) => u.label(pair)), b = glyphQuads(c, (u) => u.label("x" + pair));
        const d = a.length === 2 && b.length === 3 ? Math.abs(gap(a, 0) - gap(b, 1)) : 99;
        eq(d <= 1, true, "kerned pair " + pair + " sits the same at the start of a label as after a glyph (off by " + d + ")");
    }
    const KERNED = ["AV", "AVAVAV", "To Ta Tu", "Wa We yA LT", "Yo! P. F,", "AWAY VAT", "kerning: AVAWAY", "LTA Va"];
    for (const px of [14, 24]) {
        c.theme({ fontSize: px });
        const plain = rects(c, KERNED), nl = rects(c, KERNED.map((s) => s + "\n"));
        for (let i = 0; i < KERNED.length; i++)
            eq(plain[i] && plain[i].width, nl[i] && nl[i].width, "Arial " + px + " px label width equals its newline twin " + JSON.stringify(KERNED[i]));
    }
} else print("test_ui_label_line_width: no " + ARIAL + ", kerning sections skipped");
});

__kitS.end();
print(`test_ui_label_line_width: ${pass} pass, ${fail} fail`);
if (fail) throw new Error(`${fail} failures`);
