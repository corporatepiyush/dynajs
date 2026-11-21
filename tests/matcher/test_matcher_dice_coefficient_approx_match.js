// covers: dyna:matcher.DiceCoefficient
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_matcher_dice_coefficient_approx_match", { chain: true });
import { Levenshtein, DiceCoefficient, DamerauLevenshtein } from "dyna:matcher";
import { cpuNow } from "../lib/cpuclock.js";

let n = 0, fails = 0;
function assert(c, msg) {
    n++;
    if (!c) { fails++; print("FAIL: " + msg); }
}
function eq(a, b, msg) {
    assert(a === b, msg + " (got " + a + ", want " + b + ")");
}
function near(a, b, msg) {
    assert(Math.abs(a - b) < 1e-9, msg + " (got " + a + ", want " + b + ")");
}

function levOracle(a, b) {
    const A = Array.from(a), B = Array.from(b);
    const m = B.length;
    let prev = new Array(m + 1), cur = new Array(m + 1);
    for (let j = 0; j <= m; j++) prev[j] = j;
    for (let i = 1; i <= A.length; i++) {
        cur[0] = i;
        for (let j = 1; j <= m; j++) {
            cur[j] = Math.min(prev[j] + 1, cur[j - 1] + 1,
                              prev[j - 1] + (A[i - 1] === B[j - 1] ? 0 : 1));
        }
        const t = prev; prev = cur; cur = t;
    }
    return prev[m];
}

function diceOracle(a, b) {
    const strip = (s) => s.replace(/\s+/g, '');
    a = strip(a); b = strip(b);
    if (a === b) return 1;
    if (a.length < 2 || b.length < 2) return 0;
    const A = Array.from(a), B = Array.from(b);
    const left = [];
    for (let i = 0; i + 1 < A.length; i++) left.push(A[i] + A[i + 1]);
    const right = [];
    for (let i = 0; i + 1 < B.length; i++) right.push(B[i] + B[i + 1]);
    const pool = left.slice();
    let hits = 0;
    for (const g of right) {
        const k = pool.indexOf(g);
        if (k >= 0) { pool.splice(k, 1); hits++; }
    }
    return (2 * hits) / (left.length + right.length);
}

let seed = 0x9E3779B9 >>> 0;

function rnd() { seed = (seed * 1664525 + 1013904223) >>> 0; return seed; }

function randStr(len, alphabet) {
    let s = "";
    for (let i = 0; i < len; i++) s += alphabet[rnd() % alphabet.length];
    return s;
}

__kitS.section("dice identical = 1", () => {
near(DiceCoefficient("france", "france"), 1, "dice identical = 1");
near(DiceCoefficient("french", "quebec"), 0, "dice disjoint = 0");
near(DiceCoefficient("fRaNce", "france"), 0.2, "dice case-differing");
near(DiceCoefficient("healed", "sealed"), 0.8, "dice healed/sealed");
near(DiceCoefficient("web applications", "applications of the web"),
     0.7878787878787878, "dice web applications (whitespace stripped)");
near(DiceCoefficient("Olive-green table for sale, in extremely good condition.",
                     "For sale: table in very good condition, olive green in colour."),
     0.6060606060606061, "dice olive-green listing pair");
near(DiceCoefficient("this will not match", "to the moon"), 4 / 23,
     "dice near-no-match = 4/23, not the README's rounded 0");
});

__kitS.section("dice single char, equal, short-circuits to 1", () => {
near(DiceCoefficient("a", "a"), 1, "dice single char, equal, short-circuits to 1");
near(DiceCoefficient("a", "b"), 0, "dice single char, unequal, scores 0");
near(DiceCoefficient("ab", "ab"), 1, "dice two chars equal");
near(DiceCoefficient("a b", "ab"), 1, "dice strips whitespace before comparing");
near(DiceCoefficient("", ""), 1, "dice empty/empty short-circuits to 1");
near(DiceCoefficient("", "ab"), 0, "dice empty vs non-empty = 0");
near(DiceCoefficient(" ", ""), 1, "dice whitespace-only vs empty strips to equal");
near(DiceCoefficient(" \t\n", " "), 1, "dice two whitespace-only sides -> 1");
assert(DiceCoefficient("\u00A0ab", "ab") > 0 && DiceCoefficient("\u00A0ab", "ab") < 1,
       "dice NBSP is NOT stripped (2/3: 'ab' is one shared bigram)");
});

__kitS.section("dice aa/aaaa is below 1 (multiset)", () => {
assert(DiceCoefficient("aa", "aaaa") < 1, "dice aa/aaaa is below 1 (multiset)");
});

__kitS.section("dice aa/aaaa exact value", () => {
near(DiceCoefficient("aa", "aaaa"), 2 * 1 / (1 + 3), "dice aa/aaaa exact value");
});

let diceCases = 0;

__kitS.section("abc", () => {
for (let trial = 0; trial < 3000; trial++) {
    const a = randStr(rnd() % 30, "abc "), b = randStr(rnd() % 30, "abc ");
    const want = diceOracle(a, b), got = DiceCoefficient(a, b);
    diceCases++;
    if (Math.abs(got - want) > 1e-12) {
        assert(false, "dice differential: " + JSON.stringify([a, b]) +
               " got " + got + " want " + want);
        break;
    }
}
});

__kitS.section("dice differential ran all 3000 cases", () => {
eq(diceCases, 3000, "dice differential ran all 3000 cases");
});

let dlongDone = 0;

__kitS.section("dice long differential", () => {
for (let trial = 0; trial < 500; trial++) {
    const alpha = "abcdefghijklmnop";
    const a = randStr(rnd() % 200, alpha), b = randStr(rnd() % 200, alpha);
    const want = diceOracle(a, b), got = DiceCoefficient(a, b);
    if (Math.abs(got - want) > 1e-12) {
        assert(false, "dice long differential: got " + got + " want " + want);
        break;
    }
    dlongDone++;
}
});

__kitS.section("dice long-input differential completed (all 500 iterations ran)", () => {
eq(dlongDone, 500, "dice long-input differential completed (all 500 iterations ran)");
});

let dsymDone = 0;

__kitS.section("dice range/symmetry broken", () => {
for (let trial = 0; trial < 300; trial++) {
    const a = randStr(rnd() % 40, "abcd"), b = randStr(rnd() % 40, "abcd");
    const d = DiceCoefficient(a, b);
    if (!(d >= 0 && d <= 1) || Math.abs(d - DiceCoefficient(b, a)) > 1e-12) {
        assert(false, "dice range/symmetry broken: " + d);
        break;
    }
    dsymDone++;
}
});

__kitS.section("dice is in [0,1] and symmetric over 300 pairs (all 300 iterations ran)", () => {
eq(dsymDone, 300, "dice is in [0,1] and symmetric over 300 pairs (all 300 iterations ran)");
});

__kitS.section("dice identical astral", () => {
near(DiceCoefficient("\u{1f600}\u{1f601}", "\u{1f600}\u{1f601}"), 1,
     "dice identical astral");
assert(DiceCoefficient("\u{1f600}\u{1f601}", "\u{1f600}\u{1f602}") === 0,
       "dice astral: one shared code point is no shared bigram");
});
__kitS.end();

if (fails) {
    print("test_approx_match: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_approx_match failed");
}
print("test_approx_match: " + n + " assertions, 0 failures");
