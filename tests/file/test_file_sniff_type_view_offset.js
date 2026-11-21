// covers: dyna:file.sniffType
// F16: sniffType on a typed-array view read from the start of the underlying ArrayBuffer and ignored the view's
// byteOffset and byteLength, so a PNG at offset 16 of a larger buffer sniffed as whatever sat at offset 0, and a
// NUL after the end of the view could make text read as binary. A DataView (part of ByteView in dynajs.d.ts) was a
// TypeError. Oracle: the same bytes copied into a fresh buffer that starts at 0 (view.slice() / new Uint8Array(copy))
// must sniff the same as the view, for every view type, offset and length, and every magic in the table below.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_file_sniff_type_view_offset");
import { sniffType } from "dyna:file";

let n = 0, fails = 0;
function ok(c, m) { n++; if (!c) { fails++; print("FAIL: " + m); } }
function eq(a, b, m) { ok(a === b, m + " (got " + JSON.stringify(a) + ", want " + JSON.stringify(b) + ")"); }

const enc = (s) => Array.from(s, (c) => c.charCodeAt(0));
const MAGIC = {
    png: [0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0, 0, 0, 13, 0x49, 0x48, 0x44, 0x52],
    gif: enc("GIF89a\x01\x00\x01\x00"),
    pdf: enc("%PDF-1.7\n%"),
    zip: [0x50, 0x4b, 0x03, 0x04, 20, 0, 0, 0],
    gzip: [0x1f, 0x8b, 8, 0, 0, 0, 0, 0],
    text: enc("plain words, no magic at all\n"),
};
function copyOf(bytes) { const c = new Uint8Array(bytes.length); c.set(bytes); return c; }

__kitS.section("a view sniffs its own window, not the buffer start (every magic, offset, view type)", () => {
    for (const [name, magic] of Object.entries(MAGIC)) {
        for (const off of [1, 3, 16, 100]) {
            const buf = new Uint8Array(off + magic.length + 40);
            buf.fill(0xff, 0, off);
            buf.set(magic, off);
            const win = buf.subarray(off, off + magic.length + 8);
            const want = sniffType(copyOf(win));
            eq(sniffType(win), want, name + " via subarray at " + off);
            eq(sniffType(new Uint8Array(buf.buffer, off, magic.length + 8)), want, name + " via Uint8Array(buffer, " + off + ")");
            eq(sniffType(new Int8Array(buf.buffer, off, magic.length + 8)), want, name + " via Int8Array at " + off);
            eq(sniffType(new Uint8ClampedArray(buf.buffer, off, magic.length + 8)), want, name + " via Uint8ClampedArray at " + off);
            eq(sniffType(new DataView(buf.buffer, off, magic.length + 8)), want, name + " via DataView at " + off);
        }
    }
});

__kitS.section("the magic is found at the view's offset and not at the buffer's", () => {
    const buf = new Uint8Array(64);
    buf.set(MAGIC.png, 16);
    eq(sniffType(buf.subarray(16)), "image/png", "PNG at offset 16 of the buffer");
    ok(sniffType(buf) !== "image/png", "the whole buffer starts with zeros, not PNG (control)");
    const g = new Uint8Array(64);
    g.set(MAGIC.gif, 0);
    g.set(MAGIC.pdf, 32);
    eq(sniffType(g.subarray(32)), sniffType(copyOf(g.subarray(32))), "a PDF view after a GIF at 0 is the PDF");
    ok(sniffType(g.subarray(32)) !== sniffType(g), "and differs from the buffer-start sniff");
});

__kitS.section("bytes past the end of the view do not count (the NUL rule reads only the window)", () => {
    const buf = new Uint8Array(100);
    buf.set(MAGIC.text, 10);
    const win = buf.subarray(10, 10 + MAGIC.text.length);
    eq(sniffType(win), sniffType(copyOf(win)), "text window followed by NUL bytes sniffs as the copied text");
    eq(sniffType(new DataView(buf.buffer, 10, MAGIC.text.length)), sniffType(copyOf(win)), "same through a DataView");
});

__kitS.section("empty and short views at the end of the buffer", () => {
    const buf = new Uint8Array(32);
    buf.set(MAGIC.png, 0);
    for (const len of [0, 1, 4, 7]) {
        const v = new Uint8Array(buf.buffer, 32 - len, len);
        eq(sniffType(v), sniffType(copyOf(v)), "a " + len + "-byte view at the buffer end");
    }
    eq(sniffType(new Uint8Array(buf.buffer, 0, 4)), sniffType(copyOf(buf.subarray(0, 4))), "a view cut inside the PNG magic");
});

if (fails) throw new Error(fails + " of " + n + " checks failed");
print("test_file_sniff_type_view_offset: " + n + " checks passed");
__kitS.end();
