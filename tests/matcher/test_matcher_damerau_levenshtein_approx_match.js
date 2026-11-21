// covers: dyna:matcher.DamerauLevenshtein
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_matcher_damerau_levenshtein_approx_match");
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

__kitS.section("damerau self distance 0 at len", () => {
    const S = "abcde".repeat(11);
    for (let len = 1; len <= S.length; len++) {
        const s = S.slice(0, len);
        eq(DamerauLevenshtein(s, s), 0, "damerau self distance 0 at len " + len);
        const short = s.slice(0, Math.max(1, len - 2));
        const full = DamerauLevenshtein(s, short);
        for (const mx of [0, 1, 2, 3, 5]) {
            eq(DamerauLevenshtein(s, short, { max: mx }), Math.min(full, mx + 1),
               "damerau banded == min(full, max+1) at len " + len + " max " + mx);
        }
    }
    eq(DamerauLevenshtein("ca", "abc"), 3, "damerau ca/abc");
    eq(DamerauLevenshtein("kitten", "sitting"), 3, "damerau kitten/sitting");
    eq(DamerauLevenshtein("a cat", "an act", { max: 4 }), 2, "damerau a cat/an act banded");
    eq(DiceCoefficient("a".repeat(1048576), "b"), 0, "dice on a 1M repeated-char input");
    eq(DamerauLevenshtein("a".repeat(4096), "a".repeat(4095), { max: 3 }), 1,
       "damerau 4K repeated chars banded");
});
__kitS.end();

if (fails) {
    print("test_approx_match: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_approx_match failed");
}
print("test_approx_match: " + n + " assertions, 0 failures");
