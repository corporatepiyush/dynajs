// covers: integration c:src/core/dyn-textdec.c c:src/modules/libc/libc-text-codec.c -- fuzzes the decoder core through the TextDecoder binding
// Differential fuzz for the legacy TextDecoder: for every encoding, seeded random byte strings
// (biased toward each encoding's lead/trail/escape bytes and errors) are decoded whole and again
// in random stream:true slices; the two must be identical (the fast paths in dyn-textdec.c run
// only on clean state at a slice start, so slicing moves work between the fast loop and the
// per-byte state machine; 1-byte slices never reach a multi-byte fast path, so they are the pure
// state-machine reference). The output buffer is input+16 code units; under ASan this also proves
// that bound (carried gb18030/iso-2022-jp/utf-16 state, big5 two-code-point pointers, all-error
// input). fatal mode either throws (and the replacement output then holds a U+FFFD) or returns
// exactly the replacement output. Every decoded string is well-formed UTF-16 (a lone utf-16
// surrogate is an error, so it becomes U+FFFD).
import { suite } from "../lib/kit.js";

const t = suite("test_textdec_fuzz");
let seed = 0x9e3779b9;
const rnd = (n) => {
    seed ^= seed << 13; seed >>>= 0;
    seed ^= seed >>> 17;
    seed ^= seed << 5; seed >>>= 0;
    return seed % n;
};
const HOT = {
    "gb18030": [0x81, 0x30, 0x39, 0xFE, 0x84, 0x31, 0xA4, 0x80, 0xFF, 0x7F, 0x40],
    "gbk": [0x81, 0x30, 0xFE, 0x80, 0xFF, 0x40, 0x7E],
    "big5": [0x88, 0x62, 0x64, 0xA3, 0xE1, 0xFE, 0x81, 0x40, 0xA1, 0x7F],
    "euc-jp": [0x8E, 0x8F, 0xA1, 0xFE, 0xDF, 0xA2, 0xAF, 0x80],
    "iso-2022-jp": [0x1B, 0x24, 0x28, 0x40, 0x42, 0x4A, 0x49, 0x21, 0x7E, 0x0E, 0x0F, 0x5C],
    "shift_jis": [0x81, 0x9F, 0xE0, 0xFC, 0xF0, 0x40, 0x7F, 0x80, 0xA0, 0xA1, 0xDF, 0xFD],
    "euc-kr": [0x81, 0xFE, 0x41, 0xB0, 0xA1, 0xC9, 0x80],
    "utf-16le": [0x00, 0xD8, 0xDB, 0xDC, 0xDF, 0xFF, 0xFE, 0x41],
    "utf-16be": [0x00, 0xD8, 0xDB, 0xDC, 0xDF, 0xFF, 0xFE, 0x41],
    "x-user-defined": [0x80, 0xFF, 0x00],
    "iso-8859-8": [0xA0, 0xA1, 0xBF, 0xFF, 0xFD],
    "windows-874": [0x80, 0xDB, 0xFC, 0xFF],
    "koi8-u": [0x80, 0xFF],
};

function sample(enc, n) {
    const hot = HOT[enc], b = new Uint8Array(n);
    for (let i = 0; i < n; i++) b[i] = rnd(3) ? hot[rnd(hot.length)] : rnd(256);
    return b;
}

function wellFormed(s) {
    for (let i = 0; i < s.length; i++) {
        const c = s.charCodeAt(i);
        if (c >= 0xD800 && c <= 0xDBFF) {
            const d = s.charCodeAt(i + 1);
            if (!(d >= 0xDC00 && d <= 0xDFFF)) return false;
            i++;
        } else if (c >= 0xDC00 && c <= 0xDFFF) return false;
    }
    return true;
}

for (const enc of Object.keys(HOT)) {
    t.test(enc + ": whole == sliced, fatal agrees, well-formed", ({ eq, ok }) => {
        let mism = 0, fatalBad = 0, bad = 0, first = "";
        for (let k = 0; k < 300; k++) {
            const b = sample(enc, 1 + rnd(k < 200 ? 24 : 600));
            const whole = new TextDecoder(enc, { ignoreBOM: !!(k & 1) }).decode(b);
            const sliced = (fixed) => {
                const d = new TextDecoder(enc, { ignoreBOM: !!(k & 1) });
                let s = "";
                for (let i = 0; i < b.length;) {
                    const step = fixed || 1 + rnd(5);
                    s += d.decode(b.subarray(i, i + step), { stream: true });
                    i += step;
                }
                return s + d.decode();
            };
            if ((sliced(0) !== whole || sliced(1) !== whole) && !mism++) first = Array.from(b, (x) => x.toString(16)).join(" ");
            if (!wellFormed(whole)) bad++;
            let fat = null;
            try { fat = new TextDecoder(enc, { fatal: true, ignoreBOM: !!(k & 1) }).decode(b); } catch (e) { if (!(e instanceof TypeError)) throw e; }
            if (fat === null ? !whole.includes("\uFFFD") : fat !== whole) fatalBad++;
        }
        eq(mism, 0, "sliced decode differs from whole" + (first ? " (first input: " + first + ")" : ""));
        eq(bad, 0, "ill-formed UTF-16 output");
        eq(fatalBad, 0, "fatal throws iff the output has U+FFFD");
    });
}

await t.run();
