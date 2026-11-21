// covers: dyna:svg.renderRGBA
// Pixel oracle and API contract of renderRGBA. The oracle (tests/lib/svg_oracle_cases.js) holds the SHA-256 of the RGBA8
// pixels the original svg2img CLI rendered for 67 documents (shapes, paths, transforms, gradients, patterns, clips, masks,
// markers, CSS, text incl. non-ASCII, filters, data: images); dyna:svg ports that renderer, so every hash must match exactly.
// The hashes were taken on darwin-arm64; float rounding in libm may differ elsewhere, so off darwin-arm64 a mismatch is a
// loud skip, not a pass.
import { suite } from "../lib/kit.js";
import { renderRGBA } from "dyna:svg";
import { SHA256Hex } from "dyna:crypto";
import { platform, arch } from "dyna:sys";
import { SVG_ORACLE_CASES } from "../lib/svg_oracle_cases.js";

const t = suite("test_svg_render_rgba");
const HOME = platform() === "darwin" && arch() === "arm64";
const RECT = '<svg xmlns="http://www.w3.org/2000/svg" width="4" height="2"><rect width="2" height="2" fill="#ff0000"/><rect x="2" width="2" height="2" fill="#0000ff" fill-opacity="0.5"/></svg>';
const PENDING = Symbol("pending");

function px(r, x, y) {
    const o = (y * r.width + x) * 4;
    return Array.from(r.data.subarray(o, o + 4));
}

t.test("oracle: every case matches the svg2img CLI pixels", async (a) => {
    const off = [];
    for (const c of SVG_ORACLE_CASES) {
        const r = await renderRGBA(c.svg);
        a.eq(r.width + "x" + r.height, c.width + "x" + c.height, c.name + " size");
        a.eq(r.data.length, c.width * c.height * 4, c.name + " data length");
        const h = SHA256Hex(r.data);
        if (h !== c.sha256) off.push(c.name);
    }
    if (off.length && !HOME) a.skip(`pixel hashes are darwin-arm64 references; ${off.length} differ here: ${off.join(" ")}`);
    a.eq(off.join(" "), "", "cases whose pixels differ from the CLI");
}, { timeoutMs: 60000 });

t.test("result shape and exact pixels, channel order RGBA", async (a) => {
    const p = renderRGBA(RECT);
    a.ok(p instanceof Promise, "returns a Promise");
    const r = await p;
    a.eq(Object.keys(r).join(","), "width,height,data");
    a.ok(r.data instanceof Uint8Array, "data is a Uint8Array");
    a.deep(px(r, 0, 0), [255, 0, 0, 255], "opaque red");
    a.deep(px(r, 3, 1), [0, 0, 255, 128], "half-transparent blue, not premultiplied");
});

t.test("Uint8Array input, including a subarray view, equals the string input", async (a) => {
    const s = SVG_ORACLE_CASES.find((c) => c.name === "grad_radial");
    const bytes = new TextEncoder().encode("JUNK" + s.svg + "JUNK");
    const r = await renderRGBA(bytes.subarray(4, bytes.length - 4));
    a.eq(SHA256Hex(r.data), SHA256Hex((await renderRGBA(s.svg)).data));
    const u = await renderRGBA(new TextEncoder().encode(RECT));
    a.deep(px(u, 0, 0), [255, 0, 0, 255]);
});

t.test("options: width/height/scale resize, background fills under the drawing", async (a) => {
    a.eq((await renderRGBA(RECT, { scale: 2.5 })).width, 10, "scale 2.5");
    const w = await renderRGBA(RECT, { width: 40 });
    a.deep([w.width, w.height], [40, 20], "width alone keeps the aspect ratio");
    const wh = await renderRGBA(RECT, { width: 8, height: 8 });
    a.deep([wh.width, wh.height], [8, 8], "width and height");
    const bg = await renderRGBA('<svg xmlns="http://www.w3.org/2000/svg" width="2" height="1"><rect width="1" height="1" fill="#00f"/></svg>', { background: "#11223344" });
    a.deep(px(bg, 1, 0), [17, 34, 51, 68], "background string #RRGGBBAA");
    a.deep(px(bg, 0, 0), [0, 0, 255, 255], "drawing over the background");
    a.deep(px(await renderRGBA(RECT, { background: "00ff00" }), 0, 0), [255, 0, 0, 255]);
    const n = await renderRGBA('<svg xmlns="http://www.w3.org/2000/svg" width="1" height="1"/>', { background: 0x80402010 });
    a.deep(px(n, 0, 0), [128, 64, 32, 16], "background integer 0xRRGGBBAA");
    const opaque = await renderRGBA('<svg xmlns="http://www.w3.org/2000/svg" width="1" height="1"/>', { background: "#abcdef" });
    a.deep(px(opaque, 0, 0), [171, 205, 239, 255], "#RRGGBB is opaque");
    const none = await renderRGBA('<svg xmlns="http://www.w3.org/2000/svg" width="1" height="1"/>', null);
    a.deep(px(none, 0, 0), [0, 0, 0, 0], "null options = defaults, transparent");
});

t.test("external image hrefs are never read", async (a) => {
    const base = '<svg xmlns="http://www.w3.org/2000/svg" width="8" height="8"><rect width="4" height="4" fill="red"/>';
    const plain = await renderRGBA(base + "</svg>");
    const ext = await renderRGBA(base + '<image href="/etc/passwd" width="8" height="8"/><image href="tests/svg/x.png" width="8" height="8"/></svg>');
    a.eq(SHA256Hex(ext.data), SHA256Hex(plain.data));
});

t.test("bad input and options reject (never throw synchronously)", async (a) => {
    for (const bad of [undefined, null, 42, {}, [], new Uint16Array(4), new ArrayBuffer(8)]) {
        let p;
        try { p = renderRGBA(bad); } catch (e) { a.ok(false, "threw synchronously: " + e); continue; }
        await a.rejects(p, TypeError, "input " + Object.prototype.toString.call(bad));
    }
    await a.rejects(renderRGBA("not xml at all"), { type: TypeError, message: "not an SVG document" });
    await a.rejects(renderRGBA("<html><body/></html>"), TypeError, "no <svg> root");
    await a.rejects(renderRGBA(""), TypeError, "empty");
    await a.rejects(renderRGBA(RECT, 5), TypeError, "options not an object");
    await a.rejects(renderRGBA(RECT, { width: "10" }), TypeError);
    await a.rejects(renderRGBA(RECT, { width: 0 }), RangeError);
    await a.rejects(renderRGBA(RECT, { height: 1.5 }), RangeError);
    await a.rejects(renderRGBA(RECT, { width: 70000 }), RangeError);
    await a.rejects(renderRGBA(RECT, { scale: -1 }), RangeError);
    await a.rejects(renderRGBA(RECT, { scale: NaN }), RangeError);
    await a.rejects(renderRGBA(RECT, { background: "#12345" }), TypeError);
    await a.rejects(renderRGBA(RECT, { background: "red" }), TypeError);
    await a.rejects(renderRGBA(RECT, { background: -1 }), RangeError);
    await a.rejects(renderRGBA(RECT, { background: true }), TypeError);
    await a.rejects(renderRGBA(RECT, { width: 65536, height: 65536 }), { type: RangeError, message: "resource limit" });
    a.eq((await renderRGBA(RECT)).width, 4, "still renders after rejections");
});

t.test("work runs off the loop thread: the promise is pending when the call returns", async (a) => {
    const p = renderRGBA(SVG_ORACLE_CASES.find((c) => c.name === "test_complex").svg);
    const first = await Promise.race([p, Promise.resolve(PENDING)]);
    a.eq(first, PENDING, "settled before the loop ran");
    a.eq((await p).width, 1200);
});

t.test("32 parallel renders equal the sequential results", async (a) => {
    const pick = SVG_ORACLE_CASES.filter((c, i) => i % 2 === 0).slice(0, 32);
    const seq = [];
    for (const c of pick) seq.push(SHA256Hex((await renderRGBA(c.svg)).data));
    const par = await Promise.all(pick.map((c) => renderRGBA(c.svg)));
    a.deep(par.map((r) => SHA256Hex(r.data)), seq);
    const mixed = await Promise.allSettled(pick.map((c, i) => renderRGBA(i % 4 === 3 ? "<nope/>" : c.svg)));
    a.eq(mixed.filter((s) => s.status === "rejected").length, 8, "failures in a parallel batch reject only their own promise");
}, { timeoutMs: 60000 });

t.test("past the pool queue: 2000 calls all start pending (none runs on the loop) and all resolve", async (a) => {
    const small = '<svg xmlns="http://www.w3.org/2000/svg" width="64" height="64"><circle cx="32" cy="32" r="20" fill="red"/></svg>';
    const ps = Array.from({ length: 2000 }, () => renderRGBA(small));
    const st = await Promise.all(ps.map((p) => Promise.race([p, Promise.resolve(PENDING)])));
    a.eq(st.filter((x) => x === PENDING).length, 2000, "pending right after the calls");
    const rs = await Promise.all(ps);
    a.eq(rs.filter((r) => r.width === 64 && r.data[(32 * 64 + 32) * 4] === 255).length, 2000);
}, { timeoutMs: 60000 });

t.test("deep nesting renders on the fiber stack up to 1024 levels; deeper content is dropped, never a crash", async (a) => {
    const nest = (d) => '<svg xmlns="http://www.w3.org/2000/svg" width="8" height="8"><defs><clipPath id="c"><rect width="8" height="8"/></clipPath></defs>'
        + '<g clip-path="url(#c)">'.repeat(d) + '<rect width="8" height="8" fill="blue"/>' + "</g>".repeat(d) + "</svg>";
    a.deep(px(await renderRGBA(nest(1000)), 4, 4), [0, 0, 255, 255], "1000 nested clipped groups draw");
    a.deep(px(await renderRGBA(nest(1100)), 4, 4), [0, 0, 0, 0], "content below 1024 levels is not drawn");
    a.eq((await renderRGBA(nest(100000))).width, 8, "100000 levels");
}, { timeoutMs: 60000 });

t.test("a document without a usable size renders at a non-zero default size", async (a) => {
    for (const s of ['<svg xmlns="http://www.w3.org/2000/svg"/>', '<svg xmlns="http://www.w3.org/2000/svg" width="0" height="0"/>', '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 0 0"/>']) {
        const r = await renderRGBA(s);
        a.ok(r.width > 0 && r.height > 0 && r.data.length === r.width * r.height * 4, s + " -> " + r.width + "x" + r.height);
    }
});

await t.run();
