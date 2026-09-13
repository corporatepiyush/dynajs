/* test_approx_match.js -- Levenshtein and DiceCoefficient in dyna:matcher.
 *
 * The Levenshtein oracle is a plain full-matrix DP in JS: it shares no code and
 * no idea with the bit-parallel kernel under test, which is the only thing that
 * makes a disagreement mean something. Dice is pinned by string-similarity's own
 * published examples as well as by a JS restatement of the definition.
 *
 * Run: dynajs (built with CONFIG_NATIVE_MODULES=y) tests/test_approx_match.js
 */
import { Levenshtein, DiceCoefficient } from "dyna:matcher";

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

/* ------------------------------------------------------------------ oracles */

/* Full-matrix DP over CODE POINTS. Deliberately the slow obvious form. */
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

/* The Dice definition, restated. Multiset intersection over adjacent pairs. */
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

/* ------------------------------------------------ Levenshtein: known vectors */

eq(Levenshtein("", ""), 0, "lev empty/empty");
eq(Levenshtein("", "abc"), 3, "lev empty/abc");
eq(Levenshtein("abc", ""), 3, "lev abc/empty");
eq(Levenshtein("abc", "abc"), 0, "lev identical");
eq(Levenshtein("kitten", "sitting"), 3, "lev kitten/sitting");
eq(Levenshtein("saturday", "sunday"), 3, "lev saturday/sunday");
eq(Levenshtein("flaw", "lawn"), 2, "lev flaw/lawn");
eq(Levenshtein("gumbo", "gambol"), 2, "lev gumbo/gambol");
eq(Levenshtein("a", "b"), 1, "lev single substitution");
eq(Levenshtein("abc", "abd"), 1, "lev trailing substitution");

/* --------------------------------------- Levenshtein: differential vs oracle */

let seed = 0x9E3779B9 >>> 0;
function rnd() { seed = (seed * 1664525 + 1013904223) >>> 0; return seed; }
function randStr(len, alphabet) {
    let s = "";
    for (let i = 0; i < len; i++) s += alphabet[rnd() % alphabet.length];
    return s;
}

/* Lengths sweep the one-word boundary in BOTH operands: 64 is where the kernel
 * switches from bit-parallel to the two-row DP, and the shorter side decides. */
const LENS = [0, 1, 2, 3, 7, 15, 16, 31, 32, 62, 63, 64, 65, 66, 100, 129];
const ALPHABETS = ["ab", "abc", "abcdefgh",
                   "abcdefghijklmnopqrstuvwxyz0123456789"];

let levCases = 0;
outer:
for (const la of LENS) {
    for (const lb of LENS) {
        for (const ab of ALPHABETS) {
            const a = randStr(la, ab), b = randStr(lb, ab);
            const want = levOracle(a, b), got = Levenshtein(a, b);
            levCases++;
            if (got !== want) {
                assert(false, "lev differential len " + la + "/" + lb +
                       " alphabet " + ab.length + ": got " + got + " want " + want);
                break outer;
            }
        }
    }
}
assert(levCases === LENS.length * LENS.length * ALPHABETS.length,
       "lev differential ran every length pair (" + levCases + " cases)");

/* PROVE THE DIFFERENTIAL CAN FAIL. If the oracle agreed with everything, the
 * run above would prove nothing at all. */
assert(levOracle("kitten", "sitting") !== 4,
       "fault injection: the oracle rejects a deliberately wrong distance");
assert(levOracle("abc", "abd") === 1 && levOracle("abc", "xyz") === 3,
       "fault injection: the oracle discriminates between different inputs");

/* Every byte value, not a readable alphabet: an off-by-one in the Eq mask
 * mis-cases exactly the character a hand-picked alphabet omits. */
let allBytes = "";
for (let b = 1; b < 256; b++) allBytes += String.fromCharCode(b);
let byteDone = 0;
for (let trial = 0; trial < 300; trial++) {
    const la = rnd() % 40, lb = rnd() % 40;
    const a = randStr(la, allBytes), b = randStr(lb, allBytes);
    const want = levOracle(a, b), got = Levenshtein(a, b);
    if (got !== want) {
        assert(false, "lev full-byte-range differential: got " + got + " want " + want +
               " for " + JSON.stringify(a) + " / " + JSON.stringify(b));
        break;
    }
    byteDone++;
}
eq(byteDone, 300, "lev full-byte-range differential completed (all 300 iterations ran)");

/* Non-ASCII and astral: distance is in CODE POINTS, not bytes or UTF-16 units. */
eq(Levenshtein("\u00E9", "e"), 1, "lev precomposed e-acute vs e = 1 code point");
eq(Levenshtein("\u4F60\u597D", "\u4F60\u597D"), 0, "lev identical CJK");
eq(Levenshtein("\u4F60\u597D", "\u4F60"), 1, "lev CJK deletion");
eq(Levenshtein("\u{1f600}", "\u{1f601}"), 1,
   "lev astral substitution = 1 (not 2 UTF-16 units)");
eq(Levenshtein("a\u{1f600}b", "ab"), 1, "lev astral deletion = 1");

let uniDone = 0;
for (let trial = 0; trial < 200; trial++) {
    const alpha = "a\u00E9\u4F60\u{1f600}\u{1f1fa}z";
    const a = randStr(rnd() % 30, Array.from(alpha));
    const b = randStr(rnd() % 30, Array.from(alpha));
    const want = levOracle(a, b), got = Levenshtein(a, b);
    if (got !== want) {
        assert(false, "lev unicode differential: got " + got + " want " + want);
        break;
    }
    uniDone++;
}
eq(uniDone, 200, "lev unicode differential completed (all 200 iterations ran)");

/* ------------------------------------------------------- Levenshtein: max */

/* The contract: exact while <= max, and max + 1 once it exceeds max. So
 * `d <= max` is always a correct "within max" test, never a truncation. */
eq(Levenshtein("kitten", "sitting", { max: 5 }), 3, "lev max above the answer is exact");
eq(Levenshtein("kitten", "sitting", { max: 3 }), 3, "lev max equal to the answer is exact");
eq(Levenshtein("kitten", "sitting", { max: 2 }), 3, "lev max below the answer = max + 1");
eq(Levenshtein("kitten", "sitting", { max: 0 }), 1, "lev max 0 on a different pair = 1");
eq(Levenshtein("abc", "abc", { max: 0 }), 0, "lev max 0 on an identical pair = 0");

/* A length difference alone can exceed max, before any character is compared. */
eq(Levenshtein("a", "a".repeat(100), { max: 3 }), 4, "lev length gap exceeds max");

/* { max } must NOT bypass the cell budget: a max past the pattern length
   degenerates the band to the full matrix (audit 4.2). */
{
    let threw = false, t0 = Date.now();
    try { Levenshtein("a".repeat(100000), "b".repeat(100000), { max: 2 ** 40 }); }
    catch (e) { threw = e instanceof RangeError; }
    assert(threw, "lev { max: 2^40 } is still refused (cell budget)");
    assert(Date.now() - t0 < 2000, "and the refusal is immediate, not a ~16s DP run");
}

/* max never changes an answer that was already within it. */
let maxDone = 0;
for (let trial = 0; trial < 400; trial++) {
    const a = randStr(rnd() % 50, "abcd"), b = randStr(rnd() % 50, "abcd");
    const exact = Levenshtein(a, b);
    const max = rnd() % 20;
    const got = Levenshtein(a, b, { max: max });
    const want = exact <= max ? exact : max + 1;
    if (got !== want) {
        assert(false, "lev max contract: exact " + exact + " max " + max +
               " got " + got + " want " + want);
        break;
    }
    maxDone++;
}
eq(maxDone, 400, "lev max contract held over 400 random pairs (all 400 iterations ran)");

/* --------------------------------------------- Levenshtein: metric properties */

let symDone = 0;
for (let trial = 0; trial < 300; trial++) {
    const a = randStr(rnd() % 40, "abcde"), b = randStr(rnd() % 40, "abcde");
    if (Levenshtein(a, b) !== Levenshtein(b, a)) {
        assert(false, "lev symmetry broken on " + JSON.stringify([a, b]));
        break;
    }
    symDone++;
}
eq(symDone, 300, "lev is symmetric over 300 pairs (all 300 iterations ran)");

let triDone = 0;
for (let trial = 0; trial < 200; trial++) {
    const a = randStr(rnd() % 20, "abc"), b = randStr(rnd() % 20, "abc"),
          c = randStr(rnd() % 20, "abc");
    if (Levenshtein(a, c) > Levenshtein(a, b) + Levenshtein(b, c)) {
        assert(false, "lev triangle inequality broken");
        break;
    }
    triDone++;
}
eq(triDone, 200, "lev satisfies the triangle inequality over 200 triples (all 200 iterations ran)");

/* Bounds: the distance is never more than the longer side, never less than the
 * length difference. */
let bndDone = 0;
for (let trial = 0; trial < 300; trial++) {
    const a = randStr(rnd() % 70, "ab"), b = randStr(rnd() % 70, "ab");
    const d = Levenshtein(a, b);
    const lo = Math.abs(a.length - b.length), hi = Math.max(a.length, b.length);
    if (d < lo || d > hi) {
        assert(false, "lev out of bounds: " + d + " not in [" + lo + ", " + hi + "]");
        break;
    }
    bndDone++;
}
eq(bndDone, 300, "lev stayed within [|la-lb|, max(la,lb)] over 300 pairs (all 300 iterations ran)");

/* ------------------------------------------------- Levenshtein: refusals */

for (const bad of [null, undefined, 42, {}, []]) {
    let threw = false;
    try { Levenshtein(bad, "x"); } catch (e) { threw = e instanceof TypeError; }
    assert(threw, "Levenshtein refuses a non-string: " + JSON.stringify(bad));
}
{
    let threw = false;
    try { Levenshtein("a", "b", { max: -1 }); } catch (e) { threw = e instanceof RangeError; }
    assert(threw, "Levenshtein refuses a negative max");
}

/* ------------------------------------------- DiceCoefficient: published rows */

near(DiceCoefficient("france", "france"), 1, "dice identical = 1");
near(DiceCoefficient("french", "quebec"), 0, "dice disjoint = 0");
near(DiceCoefficient("fRaNce", "france"), 0.2, "dice case-differing");
near(DiceCoefficient("healed", "sealed"), 0.8, "dice healed/sealed");
near(DiceCoefficient("web applications", "applications of the web"),
     0.7878787878787878, "dice web applications (whitespace stripped)");
near(DiceCoefficient("Olive-green table for sale, in extremely good condition.",
                     "For sale: table in very good condition, olive green in colour."),
     0.6060606060606061, "dice olive-green listing pair");
/* string-similarity's README rounds this row to "0.0"; its own source returns
 * 4/23. Hand-checked: "thiswillnotmatch" has 15 bigrams, "tothemoon" has 8, and
 * they share exactly "th" and "ot". The published prose is what was wrong. */
near(DiceCoefficient("this will not match", "to the moon"), 4 / 23,
     "dice near-no-match = 4/23, not the README's rounded 0");

/* The two contract surprises, asserted rather than assumed. */
near(DiceCoefficient("a", "a"), 1, "dice single char, equal, short-circuits to 1");
near(DiceCoefficient("a", "b"), 0, "dice single char, unequal, scores 0");
near(DiceCoefficient("ab", "ab"), 1, "dice two chars equal");
near(DiceCoefficient("a b", "ab"), 1, "dice strips whitespace before comparing");
near(DiceCoefficient("", ""), 1, "dice empty/empty short-circuits to 1");
near(DiceCoefficient("", "ab"), 0, "dice empty vs non-empty = 0");

/* Multiset, not set: a repeated bigram is consumed once. */
assert(DiceCoefficient("aa", "aaaa") < 1, "dice aa/aaaa is below 1 (multiset)");
near(DiceCoefficient("aa", "aaaa"), 2 * 1 / (1 + 3), "dice aa/aaaa exact value");

/* ------------------------------------------- DiceCoefficient: differential */

let diceCases = 0;
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
eq(diceCases, 3000, "dice differential ran all 3000 cases");

/* Bigger alphabet, and lengths past the hash table's first few growth steps. */
let dlongDone = 0;
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
eq(dlongDone, 500, "dice long-input differential completed (all 500 iterations ran)");

/* Score stays in range and is symmetric. */
let dsymDone = 0;
for (let trial = 0; trial < 300; trial++) {
    const a = randStr(rnd() % 40, "abcd"), b = randStr(rnd() % 40, "abcd");
    const d = DiceCoefficient(a, b);
    if (!(d >= 0 && d <= 1) || Math.abs(d - DiceCoefficient(b, a)) > 1e-12) {
        assert(false, "dice range/symmetry broken: " + d);
        break;
    }
    dsymDone++;
}
eq(dsymDone, 300, "dice is in [0,1] and symmetric over 300 pairs (all 300 iterations ran)");

/* Unicode bigrams are over code points, so an astral pair is one element. */
near(DiceCoefficient("\u{1f600}\u{1f601}", "\u{1f600}\u{1f601}"), 1,
     "dice identical astral");
assert(DiceCoefficient("\u{1f600}\u{1f601}", "\u{1f600}\u{1f602}") === 0,
       "dice astral: one shared code point is no shared bigram");

/* -------------------------------------------------------------------- done */

if (fails) {
    print("test_approx_match: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_approx_match failed");
}
print("test_approx_match: " + n + " assertions, 0 failures");
