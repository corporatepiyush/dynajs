import { Levenshtein, DiceCoefficient, DiffLines, DiffChars } from "dyna:matcher";
import { monotonicNano } from "dyna:time";

const MIN_NS = 200e6;
let sink = 0;

function loopOverhead() {
    let reps = 1 << 20, t0 = monotonicNano();
    for (let i = 0; i < reps; i++) sink ^= i;
    return Number(monotonicNano() - t0) / reps;
}
const OVERHEAD = loopOverhead();

function bench(label, unit, n, fn) {
    let reps = 1;
    for (;;) {
        const t0 = monotonicNano();
        for (let i = 0; i < reps; i++) sink ^= fn();
        const ns = Number(monotonicNano() - t0);
        if (ns >= MIN_NS || reps > (1 << 28)) {
            const perOp = Math.max(0, ns / reps - OVERHEAD);
            print("BENCH " + label.padEnd(40) + " " +
                  perOp.toFixed(1).padStart(12) + " ns/op  " +
                  (n ? (perOp / n).toFixed(4).padStart(10) + " ns/" + unit : ""));
            return;
        }
        reps *= 2;
    }
}

const ESC = "\u001B";
const RED = ESC + "[31m", RESET = ESC + "[0m";

const clean1k = "the quick brown fox jumps over the lazy dog. ".repeat(23).slice(0, 1024);
const dense1k = clean1k.split(" ").map(w => RED + w + RESET).join(" ");
const clean64 = clean1k.slice(0, 64);
const dense64 = RED + clean64 + RESET;
const cjk1k = "\u4F60\u597D\u4E16\u754C".repeat(256);
const emoji1k = "\u{1f468}\u200D\u{1f469}\u200D\u{1f467}".repeat(150);

const ANSI_RE = new RegExp([
    '[\\u001B\\u009B][[\\]()#;?]*(?:(?:(?:(?:;[-a-zA-Z\\d\\/#&.:=?%@~_]+)*|[a-zA-Z\\d]+(?:;[-a-zA-Z\\d\\/#&.:=?%@~_]*)*)?\\u0007)',
    '(?:(?:\\d{1,4}(?:;\\d{0,4})*)?[\\dA-PR-TZcf-nq-uy=><~]))'
].join('|'), 'g');
function stripJs(s) { return s.replace(ANSI_RE, '').length; }
function widthJs(s) {
    let w = 0;
    for (const ch of s) {
        const c = ch.codePointAt(0);
        if (c < 0x20 || c === 0x7F) continue;
        w += (c >= 0x1100 && (c <= 0x115F || c === 0x2329 || c === 0x232A ||
              (c >= 0x2E80 && c <= 0xA4CF) || (c >= 0xAC00 && c <= 0xD7A3) ||
              (c >= 0xF900 && c <= 0xFAFF) || (c >= 0xFF00 && c <= 0xFF60) ||
              (c >= 0x1F300 && c <= 0x1FAFF))) ? 2 : 1;
    }
    return w;
}

print("# loop overhead calibrated at " + OVERHEAD.toFixed(2) + " ns/iter");
print("# String.prototype -- ANSI, width, clustering, wrapping");
print("# BYPASS FIRES (escape-free) vs BYPASS NEVER FIRES (escape-dense)");

bench("stripAnsi clean@1k       [FIRES]", "byte", 1024, () => clean1k.stripAnsi().length);
bench("stripAnsi dense@1k [NEVER FIRES]", "byte", dense1k.length, () => dense1k.stripAnsi().length);
bench("stripAnsi clean@64       [FIRES]", "byte", 64, () => clean64.stripAnsi().length);
bench("stripAnsi dense@64 [NEVER FIRES]", "byte", dense64.length, () => dense64.stripAnsi().length);
bench("stripAnsi clean@1k  BASELINE js", "byte", 1024, () => stripJs(clean1k));
bench("stripAnsi dense@1k  BASELINE js", "byte", dense1k.length, () => stripJs(dense1k));

bench("displayWidth ascii@1k    [FIRES]", "byte", 1024, () => clean1k.displayWidth());
bench("displayWidth styled@1k", "byte", dense1k.length, () => dense1k.displayWidth());
bench("displayWidth cjk@1k", "cp", 1024, () => cjk1k.displayWidth());
bench("displayWidth emojiZWJ@1k", "unit", emoji1k.length, () => emoji1k.displayWidth());
bench("displayWidth ascii@1k BASELINE js", "byte", 1024, () => widthJs(clean1k));
bench("displayWidth cjk@1k   BASELINE js", "cp", 1024, () => widthJs(cjk1k));

bench("CONTROL indexOf@1k", "byte", 1024, () => clean1k.indexOf("zzz") + 1);
bench("CONTROL slice@1k", "byte", 1024, () => clean1k.slice(4, 900).length);

bench("graphemes ascii@1k", "cp", 1024, () => clean1k.graphemes().length);
bench("graphemes emojiZWJ@1k", "unit", emoji1k.length, () => emoji1k.graphemes().length);

bench("wrapAnsi clean@1k w=80", "byte", 1024, () => clean1k.wrapAnsi(80).length);
bench("wrapAnsi dense@1k w=80", "byte", dense1k.length, () => dense1k.wrapAnsi(80).length);
bench("wrapAnsi clean@1k w=8 (many breaks)", "byte", 1024, () => clean1k.wrapAnsi(8).length);

print("");
print("# dyna:matcher -- edit distance and similarity");
print("# 64 code points is where Myers hands over to the two-row DP");

function words(len, salt) {
    let s = "", seed = (12345 + salt) >>> 0;
    const A = "abcdefghijklmnopqrstuvwxyz";
    for (let i = 0; i < len; i++) {
        seed = (seed * 1664525 + 1013904223) >>> 0;
        s += A[seed % 26];
    }
    return s;
}
const a10 = words(10, 0), b10 = words(10, 0) + "x";
const a63 = words(63, 0), b63 = words(63, 0) + "x";
const a64 = words(64, 0), b64 = words(64, 0) + "x";
const a65 = words(65, 0), b65 = words(65, 0) + "x";
const a1k = words(1000, 0), b1k = words(1000, 0) + "x";
const c1k = words(1000, 77);

bench("Levenshtein 10 (Myers)", "cp", 10, () => Levenshtein(a10, b10));
bench("Levenshtein 63 (Myers)", "cp", 63, () => Levenshtein(a63, b63));
bench("Levenshtein 64 (Myers, last word)", "cp", 64, () => Levenshtein(a64, b64));
bench("Levenshtein 65 (DP, first past it)", "cp", 65, () => Levenshtein(a65, b65));
bench("Levenshtein 1000 (DP)", "cp", 1000, () => Levenshtein(a1k, b1k));
bench("Levenshtein 1000 banded max=8", "cp", 1000, () => Levenshtein(a1k, b1k, { max: 8 }));
bench("Levenshtein 1000 max=999 [BAND MOOT]", "cp", 1000,
      () => Levenshtein(a1k, c1k, { max: 999 }));

function levJs(a, b) {
    const m = b.length;
    let prev = new Array(m + 1), cur = new Array(m + 1);
    for (let j = 0; j <= m; j++) prev[j] = j;
    for (let i = 1; i <= a.length; i++) {
        cur[0] = i;
        for (let j = 1; j <= m; j++)
            cur[j] = Math.min(prev[j] + 1, cur[j - 1] + 1,
                              prev[j - 1] + (a[i - 1] === b[j - 1] ? 0 : 1));
        const t = prev; prev = cur; cur = t;
    }
    return prev[m];
}
bench("Levenshtein 63   BASELINE js", "cp", 63, () => levJs(a63, b63));
bench("Levenshtein 1000 BASELINE js", "cp", 1000, () => levJs(a1k, b1k));

let bigA = "", bigB = "", allDiffA = "", allDiffB = "";
for (let i = 0; i < 2000; i++) {
    const line = "line " + i + " of a source file\n";
    bigA += line;
    bigB += (i % 100 === 7) ? ("CHANGED " + i + "\n") : line;
    allDiffA += "alpha " + i + "\n";
    allDiffB += "beta " + i + "\n";
}
const rep500 = "same\n".repeat(500);
bench("DiffLines 2000 lines, 1% changed", "line", 2000, () => DiffLines(bigA, bigB).length);
bench("DiffLines 2000 lines, 100% changed", "line", 2000, () => DiffLines(allDiffA, allDiffB).length);
bench("DiffLines 500 identical [TRIM FIRES]", "line", 500, () => DiffLines(rep500, rep500).length);
bench("DiffChars 200/200 similar", "cp", 200,
      () => DiffChars(a1k.slice(0, 200), b1k.slice(0, 200)).length);

const d1 = "Olive-green table for sale, in extremely good condition.";
const d2 = "For sale: table in very good condition, olive green in colour.";
bench("DiceCoefficient ~60 chars", "cp", 60, () => (DiceCoefficient(d1, d2) * 1000) | 0);
bench("DiceCoefficient 1k/1k", "cp", 1000, () => (DiceCoefficient(a1k, c1k) * 1000) | 0);

print("");
print("sink=" + sink);
