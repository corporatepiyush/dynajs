// covers: integration dyna:matcher.DiffChars dyna:matcher.DiffLines dyna:matcher.UnifiedDiff -- lopsided inputs
// Lopsided diffs ("x" * 65536 vs "abc"): Myers' linear-space search needs 2*ceil(D/2)^2 diagonal steps
// before its two halves meet, so with D >= |n - m| it burned the whole 2^28..2^31 step budget (~2 s
// release, ~6 s ASan) and then threw "too dissimilar". textdiff now computes the LCS first when the
// trimmed sides multiply to <= 2^24 cells (one side small) and, when that proves the search could not
// finish, returns the exact minimal script instead; otherwise the search runs unchanged (identical
// output, checked against the previous binary on 1800 random cases). Oracle: the hunks rebuild both
// inputs and keep exactly LCS characters equal (LCS by a plain DP here); CPU time is bounded. The
// ordinary path is pinned: 1800 seeded small diffs hash to the digest the pre-change binary gave, so
// routing ordinary inputs to the LCS table (also minimal, different tie-breaks) is caught.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_matcher_diff_lopsided");
import { DiffChars, DiffLines, UnifiedDiff, ApplyPatch } from "dyna:matcher";
import { perfCheck } from "../lib/perf.js";
import { cpuNow } from "../lib/cpuclock.js";
import { SHA256Hex } from "dyna:crypto";
import * as M from "dyna:matcher";

let pass = 0, fail = 0;
const ok = (c, m) => { if (c) pass++; else { fail++; print("FAIL: " + m); } };

function lcs(a, b) {
    let prev = new Uint32Array(b.length + 1), cur = new Uint32Array(b.length + 1);
    for (let i = 1; i <= a.length; i++) {
        for (let j = 1; j <= b.length; j++)
            cur[j] = a[i - 1] === b[j - 1] ? prev[j - 1] + 1 : Math.max(prev[j], cur[j - 1]);
        [prev, cur] = [cur, prev];
    }
    return prev[b.length];
}

function check(name, a, b) {
    const t0 = cpuNow();
    let h;
    try { h = DiffChars(a, b); } catch (e) { ok(false, name + " threw " + e.message); return; }
    const ms = cpuNow() - t0;
    let ra = "", rb = "", eq = 0;
    for (const x of h) {
        if (x.op !== 1) ra += x.text;
        if (x.op !== -1) rb += x.text;
        if (x.op === 0) eq += x.text.length;
    }
    ok(ra === a && rb === b, name + ": hunks do not rebuild the inputs");
    ok(eq === lcs(a, b), name + ": " + eq + " equal chars, LCS " + lcs(a, b));
    perfCheck(ok, ms < 300, name + ": " + ms.toFixed(1) + " ms CPU (budget-burning search took ~2000 ms)");
}

const X = "x".repeat(65536);
check("x*65536 vs abc", X, "abc");
check("abc vs x*65536", "abc", X);
check("x*65536 vs axc", X, "axc");
check("x*65536 vs xxyxx", X, "xxyxx");
check("pre+x*65536+suf vs pre+ab+suf", "hello " + X + " world", "hello ab world");
{
    const a = "q".repeat(100000), b = "zq".repeat(40);
    check("q*100000 vs (zq)*40", a, b);
}
{
    const a = Array.from({ length: 70000 }, (_, i) => "l" + (i % 3)).join("\n") + "\n", b = "l9\nl1\n";
    const t0 = cpuNow();
    const h = DiffLines(a, b), u = UnifiedDiff(a, b);
    const ms = cpuNow() - t0;
    ok(h.map((x) => x.op !== 1 ? x.text : "").join("") === a && h.map((x) => x.op !== -1 ? x.text : "").join("") === b, "DiffLines lopsided rebuilds");
    ok(ApplyPatch(a, u) === b, "ApplyPatch(a, UnifiedDiff(a, b)) === b on a lopsided pair");
    perfCheck(ok, ms < 600, "DiffLines + UnifiedDiff 70000 lines vs 2: " + ms.toFixed(1) + " ms CPU");
}
{
    let threw = "";
    const t0 = cpuNow();
    try { DiffChars("x".repeat(70000), "y".repeat(70000)); } catch (e) { threw = e.message; }
    const ms = cpuNow() - t0;
    ok(/too dissimilar/.test(threw), "two large disjoint sides still refuse: " + threw);
    perfCheck(ok, ms < 300, "the refusal is decided up front: " + ms.toFixed(1) + " ms CPU");
}

{
    let seed = 777, s = "";
    const rnd = (k) => { seed = (seed * 1103515245 + 12345) >>> 0; return seed % k; };
    for (let i = 0; i < 600; i++) {
        const al = "abcx\n ";
        let a = "", b = "";
        const la = rnd(200), lb = rnd(i % 4 === 0 ? 5 : 200);
        for (let j = 0; j < la; j++) a += al[rnd(6)];
        for (let j = 0; j < lb; j++) b += al[rnd(6)];
        for (const f of ["DiffChars", "DiffWords", "DiffLines"]) s += JSON.stringify(M[f](a, b)) + "\n";
    }
    ok(SHA256Hex(s) === "fcde6c700ccfb9c0846242d4e7d0f08456d5a3e07d80849a018cf3f007fca845", "ordinary diffs changed: " + SHA256Hex(s));
}

print("test_matcher_diff_lopsided: " + pass + " passed, " + fail + " failed");
if (fail) throw new Error(fail + " failures");
