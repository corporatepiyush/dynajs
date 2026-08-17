/* UTF-8 -> JSString ingress (JS_NewStringLen) and the count_ascii prefix scan.
 *
 * Every byte that enters the engine as text goes through JS_NewStringLen:
 * readFile, HTTP bodies, module source, and any C string handed to JS. The path
 * has three regimes and this measures all of them, because they have completely
 * different costs and only one of them was ever fast:
 *
 *   pure ASCII  -- count_ascii proves it, then one memcpy into a narrow string;
 *   latin1      -- every code point <= 0xFF, so the result stays NARROW;
 *   mixed/BMP   -- forces a wide (16-bit) string.
 *
 * The ASCII case is the reference: the gap between it and the other two is what
 * the transcoders have to close. Reading the same byte count each time keeps the
 * comparison honest (a UTF-8 file with the same character count would move more
 * bytes and confound it).
 */
import { Path, readFile, writeFile, remove } from "dyna:file";
/* Hoisted once per file, not built per call: constructing the handle inside
 * the timed region would measure the constructor, not the ingress. */
const P = (n) => new Path("/tmp/_u8_" + n + ".txt");

function bench(name, f) {
    for (let i = 0; i < 3; i++) f();
    let best = Infinity;
    for (let r = 0; r < 7; r++) {
        const t0 = performance.now(); const v = f(); const t1 = performance.now();
        if (t1 - t0 < best) best = t1 - t0;
    }
    console.log(name.padEnd(40) + best.toFixed(3) + " ms");
    return best;
}

/* Build inputs of ~equal BYTE length so ms/MB is comparable across regimes. */
const TARGET = 400000;
function grow(unit) { let s = ""; while (s.length * 2 < TARGET) s += unit; return s; }

const ascii  = grow("the quick brown fox jumps over the lazy dog 0123456789\n");
const latin1 = grow("naïve café résumé Ünïcødé àèìòù ÿ ñ ç\n");        /* all <= U+00FF */
const mixed  = grow("naïve café — ünïcødé ✓ 日本語テキスト résumé\n");   /* forces wide */
const astral = grow("emoji \u{1F600}\u{1F601}\u{1F602} mixed ascii\n");

const files = [["ascii", ascii], ["latin1", latin1], ["mixed", mixed], ["astral", astral]];
const bytes = {};
for (const [n, s] of files) {
    writeFile(P(n), s);
    /* byte length as stored on disk */
    bytes[n] = readFile(P(n)).length;
}

console.log("--- readFile -> JS string (whole-file ingress) ---");
const t = {};
for (const [n] of files) t[n] = bench("readFile " + n, () => readFile(P(n)));

console.log("\n--- narrow/wide result (memory consequence) ---");
for (const [n] of files) {
    const s = readFile(P(n));
    let maxcp = 0;
    for (let i = 0; i < s.length; i++) { const c = s.charCodeAt(i); if (c > maxcp) maxcp = c; }
    console.log("  " + n.padEnd(8) + " chars=" + String(s.length).padStart(7) +
                "  maxCharCode=0x" + maxcp.toString(16).padStart(4, "0") +
                "  " + (maxcp <= 0xff ? "narrow-eligible" : "wide"));
}

console.log("\nrelative to ascii (same path, no transcode):");
for (const [n] of files)
    console.log("  " + n.padEnd(8) + (t[n] / t.ascii).toFixed(2) + "x");

/* count_ascii is also on every JS_NewAtomLen, i.e. every identifier interned
   during compilation -- short spans, which is why it is SWAR and not a kernel
   call. This measures it through the compiler. */
console.log("\n--- compile-time atom interning (short count_ascii spans) ---");
let src = "";
for (let i = 0; i < 20000; i++) src += "function fn" + i + "(aa" + i + ", bb" + i + "){ return aa" + i + "*bb" + i + "; }\n";
bench("compile 20k fns (ascii idents)", () => (0, eval)("(function(){" + src + "})"));
let usrc = "";
for (let i = 0; i < 20000; i++) usrc += "function fné" + i + "(aaé" + i + "){ return aaé" + i + "; }\n";
bench("compile 20k fns (utf8 idents)", () => (0, eval)("(function(){" + usrc + "})"));

for (const [n] of files) remove(P(n));
