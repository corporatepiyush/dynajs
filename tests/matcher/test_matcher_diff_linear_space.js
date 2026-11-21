// timeout: 600
// covers: contract dyna:matcher.DiffChars dyna:matcher.DiffLines dyna:matcher.DiffWords dyna:matcher.UnifiedDiff -- T4 linear-space fallback and the dyna.maxSteps meta option
// T4 (owner 11-26, TRAPS): the step budget stays the safe default; opts.dyna.maxSteps raises it (an integer in [2^31, 2^53-1])
// or removes it (Infinity: no budget and no fallback, plain Myers to the end). The over-budget fallback for a pair whose
// cells fit 2^24 used to keep an n*m direction table; it now replays the same LCS backtrace in linear space (LCS rows checkpointed
// every K rows, one segment of at most 1 MiB of directions rebuilt at a time, the shorter side as columns), so memory is O(N+M) while the script is byte-identical. The oracle here is the
// old algorithm written out in JS (strip common prefix/suffix, LCS table, backtrace from the end preferring a match, then
// an insertion when the left cell is at least the upper one, else a deletion): every fallback case must give the same
// per-token script. The RSS section pins that the fallback no longer allocates n*m bytes.
import { DiffChars, DiffWords, DiffLines, UnifiedDiff, ApplyPatch } from "dyna:matcher";
import { rusage, Exec, args } from "dyna:sys";
import { sections } from "../lib/kit.js";

const S = sections("test_matcher_diff_linear_space");
let n = 0, fails = 0;
const ok = (c, m, d) => { n++; if (!c) { fails++; print("  FAIL " + m + (d !== undefined ? "  [" + d + "]" : "")); } };
const err = (f) => { try { f(); return "none"; } catch (e) { return e.name + ": " + e.message; } };
let seed = 99;
const rnd = () => (seed = (seed * 1103515245 + 12345) >>> 0) / 4294967296;
const str = (len, alpha, base = 97) => { let o = ""; for (let i = 0; i < len; i++) o += String.fromCharCode(base + Math.floor(rnd() * alpha)); return o; };
const lines = (len, alpha) => { let o = ""; for (let i = 0; i < len; i++) o += "l" + Math.floor(rnd() * alpha) + "\n"; return o; };
const chars = (s) => Array.from(s);
const lineToks = (s) => s.match(/[^\n]*\n|[^\n]+$/g) || [];

function oracle(A, B) {
    let p = 0, s = 0;
    while (p < A.length && p < B.length && A[p] === B[p]) p++;
    while (s < A.length - p && s < B.length - p && A[A.length - 1 - s] === B[B.length - 1 - s]) s++;
    const N = A.length - p - s, M = B.length - p - s, dir = new Uint8Array(N * M);
    let prv = new Uint32Array(M + 1), cur = new Uint32Array(M + 1);
    for (let i = 1; i <= N; i++) {
        cur[0] = 0;
        const ai = A[p + i - 1];
        for (let j = 1; j <= M; j++) {
            const k = (i - 1) * M + (j - 1);
            if (ai === B[p + j - 1]) { cur[j] = prv[j - 1] + 1; dir[k] = 0; }
            else if (cur[j - 1] >= prv[j]) { cur[j] = cur[j - 1]; dir[k] = 2; }
            else { cur[j] = prv[j]; dir[k] = 1; }
        }
        const t = prv; prv = cur; cur = t;
    }
    const rev = [];
    let i = N, j = M;
    while (i || j) {
        const d = i && j ? dir[(i - 1) * M + (j - 1)] : i ? 1 : 2;
        rev.push(d);
        if (d === 0) { i--; j--; } else if (d === 1) i--; else j--;
    }
    const ops = new Array(p).fill(0);
    for (let k = rev.length - 1; k >= 0; k--) ops.push(rev[k] === 0 ? 0 : rev[k] === 1 ? -1 : 1);
    for (let k = 0; k < s; k++) ops.push(0);
    return ops;
}
function tokenOps(hunks, toks) { const o = []; for (const h of hunks) for (let k = toks(h.text).length; k > 0; k--) o.push(h.op); return o; }
function rebuilds(hunks, a, b) { return hunks.filter((h) => h.op <= 0).map((h) => h.text).join("") === a && hunks.filter((h) => h.op >= 0).map((h) => h.text).join("") === b; }
function sameOps(x, y) { if (x.length !== y.length) return "length " + x.length + " vs " + y.length; for (let k = 0; k < x.length; k++) if (x[k] !== y[k]) return "token " + k + ": " + x[k] + " vs " + y[k]; return ""; }

const sanBuild = String(Exec(args()[0], ["-e", "1"], { timeoutMs: 20000, encoding: "utf8", env: { DYNA_TEARDOWN_TRACE: "1" } }).stderr).includes("teardown: precise (build)");
S.section("the fallback no longer allocates an n*m table (RSS)", () => {
    const a = str(66000, 26), b = str(250, 26);
    const r0 = rusage().maxrss;
    const h = DiffChars(a, b);
    const grew = (rusage().maxrss - r0) / 1048576;
    ok(rebuilds(h, a, b), "fallback script rebuilds both sides");
    ok(sanBuild || grew < 8, "66000 x 250 fallback grows peak RSS by less than 8 MB (the table alone was 16.5 MB)", grew.toFixed(1) + " MB");
    const h2 = DiffChars(b, str(66000, 26));
    ok(h2.length > 0 && (sanBuild || (rusage().maxrss - r0) / 1048576 < 8), "250 x 66000 (transposed) stays under the same 8 MB (sanitizer builds: shadow memory and quarantine, gate off)", ((rusage().maxrss - r0) / 1048576).toFixed(1) + " MB");
});

S.section("fallback scripts equal the old full-table backtrace (chars)", () => {
    for (const [na, nb, alpha] of [[66000, 60, 26], [66000, 40, 2], [67000, 200, 4], [70000, 3, 3], [66500, 120, 1]]) {
        const a = str(na, alpha), b = str(nb, alpha);
        const h = DiffChars(a, b);
        ok(rebuilds(h, a, b), `DiffChars ${na}x${nb}/${alpha} rebuilds`);
        const why = sameOps(tokenOps(h, chars), oracle(chars(a), chars(b)));
        ok(why === "", `DiffChars ${na}x${nb}/${alpha} equals the old backtrace`, why);
    }
    for (const [na, nb, alpha] of [[60, 66000, 26], [200, 67000, 4], [120, 66500, 2]]) {
        const a = str(na, alpha), b = str(nb, alpha);
        const h = DiffChars(a, b);
        ok(rebuilds(h, a, b) && sameOps(tokenOps(h, chars), oracle(chars(a), chars(b))) === "", `DiffChars ${na}x${nb}/${alpha} (b longer: the trace runs transposed) equals the old backtrace`);
    }
    for (const [a, b] of [["yab" + "x".repeat(66000), "ba"], ["ba", "yab" + "x".repeat(66000)]]) {
        const why = sameOps(tokenOps(DiffChars(a, b), chars), oracle(chars(a), chars(b)));
        ok(why === "", `tie cell (left == up) resolves as the old table did, ${a.length}x${b.length}`, why);
    }
    const core = str(70000, 26), a = "PREFIX" + core + "SUFFIX", b = "PREFIX" + str(90, 26) + "SUFFIX";
    const h = DiffChars(a, b);
    ok(sameOps(tokenOps(h, chars), oracle(chars(a), chars(b))) === "", "shared prefix and suffix around a fallback pair");
});

S.section("fallback scripts equal the old full-table backtrace (lines)", () => {
    const a = lines(66000, 50), b = lines(80, 50);
    const h = DiffLines(a, b);
    ok(rebuilds(h, a, b), "DiffLines fallback rebuilds");
    const why = sameOps(tokenOps(h, lineToks), oracle(lineToks(a), lineToks(b)));
    ok(why === "", "DiffLines fallback equals the old backtrace", why);
    ok(ApplyPatch(a, UnifiedDiff(a, b)) === b, "UnifiedDiff over a fallback pair applies back");
});

S.section("dyna.maxSteps: Infinity removes the budget, a finite value only raises it", () => {
    const a = str(34000, 13, 97), b = str(34000, 13, 110);
    ok(/too dissimilar/.test(err(() => DiffChars(a, b))), "the default budget refuses a 68000-token disjoint pair");
    ok(/too dissimilar/.test(err(() => DiffChars(a, b, { dyna: {} }))), "an empty dyna bag keeps the default");
    const h = DiffChars(a, b, { dyna: { maxSteps: Infinity } });
    ok(rebuilds(h, a, b), "maxSteps Infinity completes the same pair");
    ok(/too dissimilar/.test(err(() => DiffChars(a, b, { dyna: { maxSteps: 2 ** 31 } }))), "maxSteps 2^31 equals the default ceiling");
    const u = UnifiedDiff(a.replace(/(.{40})/g, "$1\n"), b.replace(/(.{40})/g, "$1\n"), { dyna: { maxSteps: Infinity } });
    ok(u.startsWith("--- a\n+++ b\n"), "UnifiedDiff takes dyna.maxSteps");
    const small = DiffWords("the quick brown fox", "the slow brown cat", { dyna: { maxSteps: Infinity } });
    ok(JSON.stringify(small) === JSON.stringify(DiffWords("the quick brown fox", "the slow brown cat")), "an unbudgeted small diff equals the default one");
    const fa = str(66000, 26), fb = str(60, 26);
    const fh = DiffChars(fa, fb, { dyna: { maxSteps: Infinity } });
    ok(rebuilds(fh, fa, fb), "Infinity skips the fallback and still rebuilds a lopsided pair");
});

S.section("dyna.maxSteps validation", () => {
    for (const [v, re] of [[2 ** 31 - 1, /RangeError.*only raises/], [1000, /RangeError.*only raises/], [-Infinity, /RangeError/], [NaN, /RangeError/], [2 ** 31 + 0.5, /RangeError/], [2 ** 53, /RangeError/], ["9e9", /TypeError.*must be a number/]])
        ok(re.test(err(() => DiffChars("a", "b", { dyna: { maxSteps: v } }))), "maxSteps " + String(v) + " is refused", err(() => DiffChars("a", "b", { dyna: { maxSteps: v } })));
    ok(/TypeError.*unknown/.test(err(() => DiffChars("a", "b", { dyna: { steps: 1 } }))), "an unknown dyna key is a TypeError");
    ok(/TypeError.*unknown/.test(err(() => DiffChars("a", "b", { maxSteps: 2 ** 32 }))), "maxSteps outside dyna is an unknown option");
    ok(/TypeError.*dyna must be an object/.test(err(() => DiffLines("a", "b", { dyna: 5 }))), "dyna must be an object");
    ok(/TypeError.*opts must be an object/.test(err(() => DiffWords("a", "b", 5))), "opts must be an object");
    ok(/TypeError.*valid: context, aName, bName, dyna/.test(err(() => UnifiedDiff("a", "b", { zz: 1 }))), "UnifiedDiff lists dyna among its options");
    ok(/RangeError.*only raises/.test(err(() => UnifiedDiff("a", "b", { dyna: { maxSteps: 5 } }))), "UnifiedDiff validates maxSteps");
    ok(DiffChars("a", "b", { dyna: { maxSteps: 2 ** 53 - 1 } }).length === 2, "maxSteps 2^53-1 is accepted");
});

S.end();
if (fails) throw new Error("test_matcher_diff_linear_space: " + fails + " FAILED of " + n);
print("test_matcher_diff_linear_space: " + n + " checks passed");
