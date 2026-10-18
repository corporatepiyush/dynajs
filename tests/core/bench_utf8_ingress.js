import * as std from "std";
import { Path, readFile, writeFile, remove } from "dyna:file";
const P = (n) => new Path(`${std.getenv("TMPDIR") || "/tmp"}/dj_u8_${Date.now() % 10000000}_${n}.txt`);

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

const TARGET = 400000;
function grow(unit) { let s = ""; while (s.length * 2 < TARGET) s += unit; return s; }

const ascii  = grow("the quick brown fox jumps over the lazy dog 0123456789\n");
const latin1 = grow("naïve café résumé Ünïcødé àèìòù ÿ ñ ç\n");
const mixed  = grow("naïve café — ünïcødé ✓ 日本語テキスト résumé\n");
const astral = grow("emoji \u{1F600}\u{1F601}\u{1F602} mixed ascii\n");

const files = [["ascii", ascii], ["latin1", latin1], ["mixed", mixed], ["astral", astral]];
const bytes = {};
for (const [n, s] of files) {
    writeFile(P(n), s);
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

console.log("\n--- compile-time atom interning (short count_ascii spans) ---");
let src = "";
for (let i = 0; i < 20000; i++) src += "function fn" + i + "(aa" + i + ", bb" + i + "){ return aa" + i + "*bb" + i + "; }\n";
bench("compile 20k fns (ascii idents)", () => (0, eval)("(function(){" + src + "})"));
let usrc = "";
for (let i = 0; i < 20000; i++) usrc += "function fné" + i + "(aaé" + i + "){ return aaé" + i + "; }\n";
bench("compile 20k fns (utf8 idents)", () => (0, eval)("(function(){" + usrc + "})"));

for (const [n] of files) remove(P(n));
