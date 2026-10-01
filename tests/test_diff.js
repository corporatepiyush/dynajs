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

eq(JSON.stringify(DiffLines("", "")), "[]", "diff empty/empty is no hunks");
eq(DiffLines("a\n", "a\n").length, 1, "identical input is one common hunk");
eq(DiffLines("a\n", "a\n")[0].op, 0, "identical input hunk is op 0");

{
    const h = checkPair(DiffLines, "a\nb\nc\n", "a\nX\nc\n", "line substitution");
    assert(h !== null, "line substitution passed the properties");
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

checkPair(DiffLines, "a\nb", "a\nc", "no trailing newline");
eq(rebuildA(DiffLines("a\nb", "a\nc")), "a\nb", "no-trailing-newline rebuildA");
{
    const h = checkPair(DiffLines, "a\nb", "a\nb\n", "trailing newline added");
    eq(h.map(x => x.op).join(","), "0,-1,1", "trailing-newline difference is an edit");
    eq(h[1].text, "b", "the bare last line is deleted");
    eq(h[2].text, "b\n", "the newline-terminated line is inserted");
}
{
    const h = checkPair(DiffLines, "a\r\nb\r\n", "a\r\nc\r\n", "CRLF substitution");
    eq(h.map(x => x.op).join(","), "0,-1,1", "CRLF substitution shape");
    eq(h[2].text, "c\r\n", "the \\r survives inside the line token");
}

{
    const h = checkPair(DiffWords, "the quick brown fox", "the slow brown fox",
                        "word substitution");
    assert(h !== null, "word substitution passed the properties");
    eq(rebuildB(h), "the slow brown fox", "word rebuildB");
    assert(h.some(x => x.op === 0 && x.text.indexOf("brown") >= 0),
           "the unchanged tail stays a common hunk");
}
{
    const h = checkPair(DiffWords, "a  b", "ab", "run-tokenization full replace");
    eq(h.length, 2, "words: [a,'  ',b] vs [ab] is a minimal del+ins");
    const h2 = checkPair(DiffWords, "a  b", "a b", "differing ws runs");
    eq(h2.map(x => x.op).join(","), "0,-1,1,0",
       "words: differing ws runs del/ins while the words stay common");
    eq(h2[1].text, "  ", "the deleted token is the double-space run");
}

{
    const h = checkPair(DiffChars, "kitten", "sitting", "char diff");
    assert(h !== null, "char diff passed the properties");
    eq(rebuildA(h), "kitten", "char rebuildA");
    eq(rebuildB(h), "sitting", "char rebuildB");
}

{
    const a = "a\u{1f600}b", b = "a\u{1f601}b";
    const h = checkPair(DiffChars, a, b, "astral char diff");
    eq(rebuildA(h), a, "astral rebuildA");
    eq(rebuildB(h), b, "astral rebuildB");
    for (const x of h)
        assert(x.text === x.text.toWellFormed(),
               "no hunk contains an unpaired surrogate: " + JSON.stringify(x.text));
}

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

assert(lcsLen(Array.from("abc"), Array.from("abc")) === 3, "lcs identical = 3");
assert(lcsLen(Array.from("abc"), Array.from("xyz")) === 0, "lcs disjoint = 0");
assert(lcsLen(Array.from("kitten"), Array.from("sitting")) === 4,
       "lcs kitten/sitting = 4 -- the check discriminates");

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

for (const bad of [null, undefined, 42, {}, []]) {
    let threw = false;
    try { DiffLines(bad, "x"); } catch (e) { threw = e instanceof TypeError; }
    assert(threw, "DiffLines refuses a non-string: " + JSON.stringify(bad));
}

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

assert(props >= 900, "the apply-patch properties ran on every diff (" + props + ")");

if (fails) {
    print("test_diff: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_diff failed");
}
print("test_diff: " + n + " assertions, 0 failures (" + props + " diffs property-checked)");
