// covers: integration dyna:matcher.UnifiedDiff dyna:matcher.ApplyPatch -- the round-trip property ApplyPatch(a, UnifiedDiff(a, b)) === b needs both
// UnifiedDiff/ApplyPatch: the round-trip property ApplyPatch(a, UnifiedDiff(a, b)) === b over random
// line soups (missing final newline, CRLF, empty sides, context 0..4); ApplyPatch over patches
// written by the system `diff -u` (fixed vendored outputs, plus live random cases when a diff
// binary is on PATH), byte equality of UnifiedDiff's hunks with `diff -u` on unambiguous edits,
// and every refusal: malformed header, truncated hunk, mismatching context, second file, options.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_matcher_unified_diff_patch");
import { UnifiedDiff, ApplyPatch } from "dyna:matcher";
import { Random } from "dyna:random";
import { Exec, Which } from "dyna:sys";
import { Path, writeFile, makeTempDir, removeAll } from "dyna:file";

let pass = 0, fail = 0;
const ok = (c, m) => { if (c) pass++; else { fail++; print("FAIL: " + m); } };
const throwsT = (f, cls, m, re) => {
    try { f(); ok(false, m + " did not throw"); }
    catch (e) { ok(e.constructor === cls && (!re || re.test(e.message)), m + " threw " + e.name + ": " + e.message); }
};
const rng = new Random(20261120);
const POOL = ["alpha", "beta", "gamma", "delta", "", "  indented", "é ünïcode", "x\r", "@@ not a header", "-dash", "+plus", "\\ backslash", "--- dashes"];
function soup() {
    const n = rng.nextBounded(14);
    let s = "";
    for (let i = 0; i < n; i++) s += POOL[rng.nextBounded(POOL.length)] + "\n";
    if (n && rng.nextBounded(3) === 0) s = s.slice(0, -1);
    return s;
}
function mutate(a) {
    const lines = a.split("\n");
    const m = rng.nextBounded(5);
    for (let k = 0; k < m; k++) {
        const at = rng.nextBounded(lines.length + 1), w = POOL[rng.nextBounded(POOL.length)];
        const op = rng.nextBounded(3);
        if (op === 0) lines.splice(at, 0, w); else if (op === 1) lines.splice(at, 1); else lines[at] = w;
    }
    return lines.join("\n");
}

__kitS.section("fixed shapes", () => {
    ok(UnifiedDiff("same\n", "same\n") === "", "identical inputs give an empty diff (like diff -u)");
    ok(UnifiedDiff("", "x\n") === "--- a\n+++ b\n@@ -0,0 +1 @@\n+x\n", "empty a: -0,0");
    ok(UnifiedDiff("x", "") === "--- a\n+++ b\n@@ -1 +0,0 @@\n-x\n\\ No newline at end of file\n", "missing final newline marker");
    ok(UnifiedDiff("a\nb\n", "a\nB\n", { aName: "old.txt", bName: "new.txt", context: 0 }) === "--- old.txt\n+++ new.txt\n@@ -2 +2 @@\n-b\n+B\n", "names and context 0");
    ok(ApplyPatch("keep\n", "") === "keep\n", "empty patch is the identity");
    ok(ApplyPatch("a\nb\nc\n", "@@ -2 +2 @@\n-b\n+B\n") === "a\nB\nc\n", "headerless hunk");
    ok(ApplyPatch("a\n\nc\n", "@@ -1,3 +1,3 @@\n a\n\n-c\n+C\n") === "a\n\nC\n", "an empty patch line is an empty context line (trailing space stripped by mailers)");
    ok(ApplyPatch("a\nb", "--- a\n+++ b\n@@ -2 +2 @@\n-b\n\\ No newline at end of file\n+b\n") === "a\nb\n", "adds the missing final newline");
});

__kitS.section("round trip ApplyPatch(a, UnifiedDiff(a, b)) === b", () => {
    let bad = 0;
    for (let i = 0; i < 4000; i++) {
        const a = soup(), b = rng.nextBounded(4) ? mutate(a) : soup();
        const context = rng.nextBounded(5);
        const d = UnifiedDiff(a, b, { context });
        let got;
        try { got = ApplyPatch(a, d); } catch (e) { got = "THREW " + e.message; }
        if (got !== b) { if (bad++ < 3) print("  case " + JSON.stringify({ a, b, context, d, got })); }
        if (a.replace(/\n/g, "\r\n") !== a) {
            const ac = a.replace(/\n/g, "\r\n"), bc = b.replace(/\n/g, "\r\n");
            if (ApplyPatch(ac, UnifiedDiff(ac, bc, { context })) !== bc) bad++;
        }
    }
    ok(bad === 0, `round trip: ${bad} failures`);
});

__kitS.section("vendored diff -u output (Apple/FreeBSD and GNU diffutils agree on these)", () => {
    const A = "a\nb\nc\nd\ne\nf\ng\nh\ni\nj\nk\n", B = "a\nB\nc\nd\ne\nf\ng\nh\ni\nj\nk\nl";
    const P = "--- tmp/A.txt\t2026-11-20 12:45:27\n+++ tmp/B.txt\t2026-11-20 12:45:27\n@@ -1,5 +1,5 @@\n a\n-b\n+B\n c\n d\n e\n@@ -9,3 +9,4 @@\n i\n j\n k\n+l\n\\ No newline at end of file\n";
    ok(ApplyPatch(A, P) === B, "diff -u with timestamps and a no-newline tail");
    ok(UnifiedDiff(A, B, { aName: "tmp/A.txt\t2026-11-20 12:45:27", bName: "tmp/B.txt\t2026-11-20 12:45:27" }) === P, "UnifiedDiff text equals diff -u byte for byte");
    const git = "diff --git a/f b/f\nindex 1111111..2222222 100644\n--- a/f\n+++ b/f\n@@ -1,2 +1,3 @@ section heading\n one\n+inserted\n two\n-- \n2.44.0\n";
    ok(ApplyPatch("one\ntwo\n", git) === "one\ninserted\ntwo\n", "git-style headers, a hunk section heading and a mail signature are skipped");
});

__kitS.section("live diff -u when a diff binary exists", () => {
    if (!Which("diff")) { print("  (no diff on PATH: live section skipped)"); return; }
    const dir = makeTempDir("ud-");
    try {
        let bad = 0, same = 0, runs = 0;
        for (let i = 0; i < 150; i++) {
            const a = soup().replace(/\r/g, ""), b = mutate(a).replace(/\r/g, "");
            writeFile(new Path(dir, "a"), a);
            writeFile(new Path(dir, "b"), b);
            const r = Exec("diff", ["-u", new Path(dir, "a").toString(), new Path(dir, "b").toString()]);
            const out = typeof r.stdout === "string" ? r.stdout : new TextDecoder().decode(r.stdout);
            runs++;
            let got;
            try { got = ApplyPatch(a, out); } catch (e) { got = "THREW " + e.message; }
            if (got !== b) { if (bad++ < 3) print("  live case " + JSON.stringify({ a, b, out, got })); }
            const mine = UnifiedDiff(a, b);
            if (mine.slice(mine.indexOf("@@")) === out.slice(out.indexOf("@@") < 0 ? out.length : out.indexOf("@@"))) same++;
        }
        print(`  live: ${runs} diff -u patches applied, ${bad} failures; UnifiedDiff hunks byte-equal on ${same}`);
        ok(bad === 0, `ApplyPatch over ${runs} live diff -u patches: ${bad} failures`);
        ok(same > runs / 2, `UnifiedDiff hunks equal diff -u's on ${same}/${runs} random cases (both minimal; ties may differ)`);
    } finally { removeAll(dir); }
});

__kitS.section("refusals", () => {
    throwsT(() => ApplyPatch("a\n", "@@ -1 +1 @\n-a\n+b\n"), SyntaxError, "malformed header", /malformed hunk header/);
    throwsT(() => ApplyPatch("a\nb\n", "@@ -1,2 +1,2 @@\n-a\n+b\n"), SyntaxError, "truncated hunk", /truncated/);
    throwsT(() => ApplyPatch("a\n", "@@ -1 +1 @@\n*a\n"), SyntaxError, "bad line tag", /unexpected line/);
    throwsT(() => ApplyPatch("x\n", "@@ -1 +1 @@\n-a\n+b\n"), Error, "context mismatch", /does not apply: text line 1/);
    throwsT(() => ApplyPatch("a", "@@ -1 +1 @@\n-a\n+b\n"), Error, "newline state is part of the line", /does not apply/);
    throwsT(() => ApplyPatch("a\n", "@@ -3 +3 @@\n-a\n+b\n"), Error, "hunk past the end", /past the end/);
    throwsT(() => ApplyPatch("a\nb\n", "@@ -2 +2 @@\n-b\n+B\n@@ -1 +1 @@\n-a\n+A\n"), Error, "hunks out of order", /before the previous hunk/);
    throwsT(() => ApplyPatch("a\n", "--- a\n+++ b\n@@ -1 +1 @@\n-a\n+b\n--- c\n+++ d\n@@ -1 +1 @@\n-a\n+b\n"), SyntaxError, "two files", /second file/);
    throwsT(() => ApplyPatch("a\n", 5), TypeError, "non-string patch");
    throwsT(() => UnifiedDiff("a", "b", { context: -1 }), RangeError, "negative context");
    throwsT(() => UnifiedDiff("a", "b", { context: 1.5 }), RangeError, "fractional context");
    throwsT(() => UnifiedDiff("a", "b", { context: "3" }), TypeError, "string context");
    throwsT(() => UnifiedDiff("a", "b", { contxt: 3 }), TypeError, "unknown option", /unknown option "contxt"/);
    throwsT(() => UnifiedDiff("a", "b", { aName: "x\ny" }), RangeError, "line break in a name");
    throwsT(() => UnifiedDiff("a", "b", { bName: 7 }), TypeError, "non-string name");
});

__kitS.end();
print("test_matcher_unified_diff_patch: " + pass + " passed, " + fail + " failed");
if (fail) throw new Error(fail + " failures");
