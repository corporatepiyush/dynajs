// covers: dyna:svg.render
// render() returns a PNG. Every oracle case is decoded (tests/lib/svg_png.js checks every chunk CRC and the zlib stream)
// and must hold exactly the pixels renderRGBA returns, so the PNG encoder (dynajs deflate + crc32) is lossless. Also: the
// module is async only (exports exactly render and renderRGBA), and bad input rejects.
import { suite } from "../lib/kit.js";
import * as svg from "dyna:svg";
import { render } from "dyna:svg";
import { SHA256Hex } from "dyna:crypto";
import { decodePNG } from "../lib/svg_png.js";
import { SVG_ORACLE_CASES } from "../lib/svg_oracle_cases.js";

const t = suite("test_svg_render");
const RECT = '<svg xmlns="http://www.w3.org/2000/svg" width="4" height="2"><rect width="2" height="2" fill="#ff0000"/><rect x="2" width="2" height="2" fill="#0000ff" fill-opacity="0.5"/></svg>';

t.test("module is async only: exactly render and renderRGBA, both return Promises", async (a) => {
    a.eq(Object.keys(svg).sort().join(","), "render,renderRGBA");
    a.ok(render(RECT) instanceof Promise);
    a.ok(svg.renderRGBA(RECT) instanceof Promise);
});

t.test("PNG is lossless: decoded pixels equal renderRGBA for every oracle case", async (a) => {
    for (const c of SVG_ORACLE_CASES) {
        const png = await render(c.svg);
        a.ok(png instanceof Uint8Array, c.name + " is a Uint8Array");
        const d = decodePNG(png);
        const r = await svg.renderRGBA(c.svg);
        a.eq(d.width + "x" + d.height, r.width + "x" + r.height, c.name + " size");
        a.eq(SHA256Hex(d.data), SHA256Hex(r.data), c.name + " pixels");
    }
}, { timeoutMs: 60000 });

t.test("PNG header, options and large images span several IDAT chunks", async (a) => {
    const png = await render(RECT, { scale: 2, background: "#00ff00" });
    a.deep(Array.from(png.subarray(0, 16)), [0x89, 0x50, 0x4e, 0x47, 13, 10, 26, 10, 0, 0, 0, 13, 0x49, 0x48, 0x44, 0x52]);
    const d = decodePNG(png);
    a.deep([d.width, d.height], [8, 4]);
    a.deep(Array.from(d.data.subarray(28, 32)), [0, 127, 127, 255], "blue at alpha .5 over green (CLI rounding)");
    const noisy = '<svg xmlns="http://www.w3.org/2000/svg" width="600" height="600"><filter id="f"><feTurbulence baseFrequency="0.9" numOctaves="3"/></filter><rect width="600" height="600" filter="url(#f)"/></svg>';
    const big = await render(noisy);
    let idat = 0;
    for (let o = 8; o < big.length;) {
        const len = ((big[o] << 24) | (big[o + 1] << 16) | (big[o + 2] << 8) | big[o + 3]) >>> 0;
        if (String.fromCharCode(...big.subarray(o + 4, o + 8)) === "IDAT") idat++;
        o += 12 + len;
    }
    a.ok(idat > 1, "IDAT chunks: " + idat);
    a.eq(SHA256Hex(decodePNG(big).data), SHA256Hex((await svg.renderRGBA(noisy)).data));
}, { timeoutMs: 30000 });

t.test("bad input rejects with TypeError/RangeError, never a synchronous throw", async (a) => {
    let p;
    try { p = render(Symbol("x")); } catch (e) { a.ok(false, "threw synchronously: " + e); }
    await a.rejects(p, TypeError);
    await a.rejects(render(new Uint8Array([0xff, 0xfe, 0, 1])), TypeError, "binary garbage");
    await a.rejects(render(RECT, { scale: 0 }), RangeError);
    await a.rejects(render(RECT, { scale: 30000 }), { type: RangeError, message: "resource limit" });
});

t.test("parallel PNG renders resolve independently", async (a) => {
    const cs = SVG_ORACLE_CASES.slice(0, 32);
    const pngs = await Promise.all(cs.map((c) => render(c.svg)));
    const want = [];
    for (const c of cs) want.push(SHA256Hex((await svg.renderRGBA(c.svg)).data));
    a.deep(pngs.map((p) => SHA256Hex(decodePNG(p).data)), want);
}, { timeoutMs: 60000 });

await t.run();
