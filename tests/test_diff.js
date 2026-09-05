/* test_diff.js -- DiffLines / DiffWords / DiffChars in dyna:matcher.
 *
 * THE ORACLE IS APPLY-PATCH, NOT A ROUND TRIP THROUGH OUR OWN DIFF. Two
 * properties decide correctness and a wrong alignment breaks at least one:
 *   concat(hunks where op != +1) === a      (the deletions and the common part)
 *   concat(hunks where op != -1) === b      (the insertions and the common part)
 * Minimality is checked separately against a full-matrix LCS in JS.
 *
 * Run: dynajs (built with CONFIG_NATIVE_MODULES=y) tests/test_diff.js
 */
import { DiffLines, DiffWords, DiffChars } from "dyna:matcher";

let n = 0, fails = 0;
function assert(c, msg) {
    n++;
    if (!c) { fails++; print("FAIL: " + msg); }
}
function eq(a, b, msg) {
    assert(a === b, msg + " (got " + JSON.stringify(a) + ", want " + JSON.stringify(b) + ")");
}

function rebuildA(h) { return h.filter(x => x.op !== 1).map(x => x.text).join(""); }
function rebuildB(h) { return h.filter(x => x.op !== -1).map(x => x.text).join(""); }

/* The two properties, applied to every diff this file makes. */
let props = 0;
function checkPair(fn, a, b, label) {
    const h = fn(a, b);
    props++;
    if (rebuildA(h) !== a) { assert(false, label + ": rebuildA broken"); return null; }
    if (rebuildB(h) !== b) { assert(false, label + ": rebuildB broken"); return null; }
    for (const x of h) {
        if (x.op !== -1 && x.op !== 0 && x.op !== 1) {
            assert(false, label + ": bad op " + x.op); return null;
        }
        if (x.text.length === 0) { assert(false, label + ": empty hunk"); return null; }
    }
    for (let i = 1; i < h.length; i++)
        if (h[i].op === h[i - 1].op) {
            assert(false, label + ": adjacent hunks share op " + h[i].op); return null;
        }
    return h;
}

/* --------------------------------------------------------- known shapes */

eq(JSON.stringify(DiffLines("", "")), "[]", "diff empty/empty is no hunks");
eq(DiffLines("a\n", "a\n").length, 1, "identical input is one common hunk");
eq(DiffLines("a\n", "a\n")[0].op, 0, "identical input hunk is op 0");

{
    const h = checkPair(DiffLines, "a\nb\nc\n", "a\nX\nc\n", "line substitution");
    assert(h !== null, "line substitution passed the properties");
    /* One line replaced: common, delete, insert, common -- in that order. */
    eq(h.map(x => x.op).join(","), "0,-1,1,0", "substitution op sequence");
    eq(h[0].text, "a\n", "leading common line");
    eq(h[1].text, "b\n", "deleted line");
    eq(h[2].text, "X\n", "inserted line");
    eq(h[3].text, "c\n", "trailing common line");
}
{
    const h = checkPair(DiffLines, "a\nc\n", "a\nb\nc\n", "pure insertion");
    eq(h.map(x => x.op).join(","), "0,1,0", "insertion op sequence");
    eq(h[1].text, "b\n", "inserted line text");
}
{
    const h = checkPair(DiffLines, "a\nb\nc\n", "a\nc\n", "pure deletion");
    eq(h.map(x => x.op).join(","), "0,-1,0", "deletion op sequence");
    eq(h[1].text, "b\n", "deleted line text");
}
{
    const h = checkPair(DiffLines, "x\n", "y\n", "whole-file replace");
    eq(h.map(x => x.op).sort().join(","), "-1,1", "replace is one delete + one insert");
}

/* A file with no trailing newline still reassembles exactly. */
checkPair(DiffLines, "a\nb", "a\nc", "no trailing newline");
eq(rebuildA(DiffLines("a\nb", "a\nc")), "a\nb", "no-trailing-newline rebuildA");

/* ------------------------------------------------------------- words */

{
    const h = checkPair(DiffWords, "the quick brown fox", "the slow brown fox",
                        "word substitution");
    assert(h !== null, "word substitution passed the properties");
    eq(rebuildB(h), "the slow brown fox", "word rebuildB");
    /* Whitespace is its own token, so the common runs survive the edit. */
    assert(h.some(x => x.op === 0 && x.text.indexOf("brown") >= 0),
           "the unchanged tail stays a common hunk");
}

/* ------------------------------------------------------------- chars */

{
    const h = checkPair(DiffChars, "kitten", "sitting", "char diff");
    assert(h !== null, "char diff passed the properties");
    eq(rebuildA(h), "kitten", "char rebuildA");
    eq(rebuildB(h), "sitting", "char rebuildB");
}

/* Astral characters are single tokens: a diff must never split a surrogate
 * pair, which would produce an unpaired half and a broken string. */
{
    const a = "a\u{1f600}b", b = "a\u{1f601}b";
    const h = checkPair(DiffChars, a, b, "astral char diff");
    eq(rebuildA(h), a, "astral rebuildA");
    eq(rebuildB(h), b, "astral rebuildB");
    for (const x of h)
        assert(x.text === x.text.toWellFormed(),
               "no hunk contains an unpaired surrogate: " + JSON.stringify(x.text));
}

/* ------------------------------------------------- minimality vs an LCS */

/* Full-matrix LCS length in JS -- independent of the Myers implementation. */
function lcsLen(A, B) {
    const m = B.length;
    let prev = new Array(m + 1).fill(0), cur = new Array(m + 1).fill(0);
    for (let i = 1; i <= A.length; i++) {
        cur[0] = 0;
        for (let j = 1; j <= m; j++)
            cur[j] = A[i - 1] === B[j - 1] ? prev[j - 1] + 1
                                           : Math.max(prev[j], cur[j - 1]);
        const t = prev; prev = cur; cur = t;
    }
    return prev[m];
}

let seed = 0x5bf03635 >>> 0;
function rnd() { seed = (seed * 1664525 + 1013904223) >>> 0; return seed; }

let minCases = 0;
for (let trial = 0; trial < 600; trial++) {
    const AB = "abcd";
    let a = "", b = "";
    const la = rnd() % 25, lb = rnd() % 25;
    for (let i = 0; i < la; i++) a += AB[rnd() % 4];
    for (let i = 0; i < lb; i++) b += AB[rnd() % 4];
    const h = checkPair(DiffChars, a, b, "random char pair " + trial);
    if (h === null) break;
    /* A minimal edit script keeps exactly LCS(a,b) characters common. */
    const common = h.filter(x => x.op === 0).reduce((s, x) => s + x.text.length, 0);
    const want = lcsLen(Array.from(a), Array.from(b));
    if (common !== want) {
        assert(false, "diff is not minimal on " + JSON.stringify([a, b]) +
               ": kept " + common + " common, LCS is " + want);
        break;
    }
    minCases++;
}
eq(minCases, 600, "diff matched the LCS on all 600 random pairs (minimality)");

/* PROVE THE MINIMALITY CHECK CAN FAIL: a deliberately non-minimal script keeps
 * fewer characters common than the LCS, and lcsLen must notice. */
assert(lcsLen(Array.from("abc"), Array.from("abc")) === 3, "lcs identical = 3");
assert(lcsLen(Array.from("abc"), Array.from("xyz")) === 0, "lcs disjoint = 0");
assert(lcsLen(Array.from("kitten"), Array.from("sitting")) === 4,
       "lcs kitten/sitting = 4 -- the check discriminates");

/* ---------------------------------------------- random line-level pairs */

let lineCases = 0;
for (let trial = 0; trial < 300; trial++) {
    const pool = ["alpha\n", "beta\n", "gamma\n", "delta\n", "eps\n"];
    let a = "", b = "";
    for (let i = 0; i < rnd() % 20; i++) a += pool[rnd() % pool.length];
    for (let i = 0; i < rnd() % 20; i++) b += pool[rnd() % pool.length];
    if (checkPair(DiffLines, a, b, "random line pair " + trial) === null) break;
    lineCases++;
}
eq(lineCases, 300, "line diff held both properties on all 300 random pairs");

/* The real-world shape the design names: a large file, 1% of lines changed. */
{
    let a = "", b = "";
    for (let i = 0; i < 2000; i++) {
        const line = "line " + i + " of the file\n";
        a += line;
        b += (i % 100 === 7) ? ("CHANGED " + i + "\n") : line;
    }
    const h = checkPair(DiffLines, a, b, "2000 lines, 1% changed");
    assert(h !== null, "large sparse diff passed the properties");
    assert(h.length < 100, "large sparse diff stays a small number of hunks (got "
           + h.length + ")");
}

/* Adversarial: the same line repeated. Every alignment is equally good, which
 * is where a diff that trusts its first snake goes quadratic or wrong. */
{
    const a = "same\n".repeat(400), b = "same\n".repeat(400);
    const h = checkPair(DiffLines, a, b, "400 identical lines");
    eq(h.length, 1, "identical repeated lines collapse to one common hunk");
}
{
    const a = "same\n".repeat(300), b = "same\n".repeat(200) + "x\n";
    const h = checkPair(DiffLines, a, b, "repeated lines, different lengths");
    assert(h !== null, "repeated-line pair passed the properties");
}

/* ------------------------------------------------------------- refusals */

for (const bad of [null, undefined, 42, {}, []]) {
    let threw = false;
    try { DiffLines(bad, "x"); } catch (e) { threw = e instanceof TypeError; }
    assert(threw, "DiffLines refuses a non-string: " + JSON.stringify(bad));
}

/* P1b-4: the edit-distance / work cap. Two ~4M-token fully-disjoint inputs ran
 * ~6e13 middle-snake steps (minutes-to-hours on the loop thread). The cap now
 * refuses a diff that would need more than the budget of diagonal steps, with a
 * clean RangeError like the token cap. These must be FAST (the OLD code hangs
 * on this by construction) and must not fire on legitimate diffs. */
{
    const t0 = Date.now();
    let threw = null;
    try { DiffChars("a".repeat(200000), "b".repeat(200000)); }
    catch (e) { threw = e; }
    const ms = Date.now() - t0;
    assert(threw !== null, "DiffChars refuses two disjoint 200k-token inputs");
    assert(threw instanceof RangeError,
           "DiffChars refusal is a RangeError (got " + (threw && threw.name) + ")");
    assert(ms < 3000, "DiffChars refusal is prompt (" + ms + "ms)");
}
/* The same cap must hold for DIFFLINES and DIFFWORDS, which share dyn_diff_run. */
for (const [name, fn, a, b] of [
    ["DiffLines", DiffLines, "a\n".repeat(200000), "b\n".repeat(200000)],
    ["DiffWords", DiffWords, ("a ").repeat(200000), ("b ").repeat(200000)],
]) {
    const t0 = Date.now();
    let threw = null;
    try { fn(a, b); } catch (e) { threw = e; }
    const ms = Date.now() - t0;
    assert(threw instanceof RangeError, name +
           " refuses two disjoint 200k-token inputs (got " +
           (threw && threw.name) + ")");
    assert(ms < 3000, name + " refusal is prompt (" + ms + "ms)");
}
/* The cap is work-bounded, not length-bounded: a SHORT-but-completely-different
 * input clearly under it still diffs (no false positive), while the same shape
 * clearly over it refuses. This pins the boundary on the right side of the
 * curve so a future change cannot grow the cap into a hang or shrink it into a
 * false positive. The two sizes sit on opposite sides of the flip with a wide
 * safety margin (empirically ~11.6k chars is the flip point for this shape). */
{
    const a = "a".repeat(10000), b = "b".repeat(10000);
    const h = checkPair(DiffChars, a, b, "boundary: disjoint well under the cap");
    assert(h !== null, "boundary disjoint input under cap diffs successfully");
    let threw = null;
    try { DiffChars("a".repeat(13000), "b".repeat(13000)); }
    catch (e) { threw = e; }
    assert(threw instanceof RangeError,
           "boundary input well over cap throws RangeError");
}
/* The legit change-heavy shapes the design names must NOT trip the cap: a
 * 2000-line fully-disjoint file and a 1%-changed file both still diff. */
{
    let a = "", b = "";
    for (let i = 0; i < 2000; i++) { a += "alpha " + i + "\n"; b += "beta " + i + "\n"; }
    const h = checkPair(DiffLines, a, b, "2000-line fully-disjoint file");
    assert(h !== null, "2000-line fully-disjoint file diffs under the cap");
}
{
    let a = "", b = "";
    for (let i = 0; i < 2000; i++) {
        const line = "line " + i + " of the file\n";
        a += line; b += (i % 100 === 7) ? ("CHANGED " + i + "\n") : line;
    }
    const h = checkPair(DiffLines, a, b, "2000-line 1%-changed file");
    assert(h !== null, "2000-line 1%-changed file diffs under the cap");
}

/* -------------------------------------------------------------------- done */

assert(props >= 900, "the apply-patch properties ran on every diff (" + props + ")");

if (fails) {
    print("test_diff: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_diff failed");
}
print("test_diff: " + n + " assertions, 0 failures (" + props + " diffs property-checked)");
