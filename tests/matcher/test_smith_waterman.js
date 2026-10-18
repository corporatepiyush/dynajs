// matcher.SmithWaterman against (1) the worked example of the Wikipedia "Smith-Waterman algorithm" article
// (TGTTACGG vs GGTTGACTA, match +3, mismatch -3, linear gap -2 -> score 13, GTT-AC / GTTGAC) and (2) an independent
// full-matrix JS implementation of the recurrence with the documented tie order, over random inputs, bands and tokens.
import { SmithWaterman } from "dyna:matcher";
import { Random } from "dyna:random";

let pass = 0, fail = 0;
const ok = (c, m) => { if (c) pass++; else { fail++; print("FAIL: " + m); } };

function reference(a, b, { match = 2, mismatch = -1, gap = -1, band = -1, diagonal = 0 } = {}) {
    const n = a.length, m = b.length, H = [], P = [];
    for (let i = 0; i <= n; i++) { H.push(new Array(m + 1).fill(0)); P.push(new Array(m + 1).fill(0)); }
    let best = 0, bi = 0, bj = 0;
    for (let i = 1; i <= n; i++)
        for (let j = 1; j <= m; j++) {
            if (band >= 0 && (j - i < diagonal - band || j - i > diagonal + band)) continue;
            const d = H[i - 1][j - 1] + (a[i - 1] === b[j - 1] ? match : mismatch), u = H[i - 1][j] + gap, l = H[i][j - 1] + gap;
            const v = Math.max(0, d, u, l);
            H[i][j] = v;
            P[i][j] = v === 0 ? 0 : v === d ? 1 : v === u ? 2 : 3;
            if (v > best) { best = v; bi = i; bj = j; }
        }
    let i = bi, j = bj, ops = "", matches = 0;
    while (i > 0 && j > 0 && P[i][j]) {
        const p = P[i][j];
        if (p === 1) { if (a[i - 1] === b[j - 1]) matches++; ops = "M" + ops; i--; j--; } else if (p === 2) { ops = "I" + ops; i--; } else { ops = "D" + ops; j--; }
    }
    const cigar = (ops.match(/(.)\1*/g) ?? []).map((r) => r.length + r[0]).join("");
    return best ? { score: best, aStart: i, aEnd: bi, bStart: j, bEnd: bj, matches, cigar } : { score: 0, aStart: 0, aEnd: 0, bStart: 0, bEnd: 0, matches: 0, cigar: "" };
}
const same = (x, y) => JSON.stringify(x) === JSON.stringify(y);

{
    const r = SmithWaterman("TGTTACGG", "GGTTGACTA", { match: 3, mismatch: -3, gap: -2 });
    ok(r.score === 13, "Wikipedia example score 13 (got " + r.score + ")");
    ok("TGTTACGG".slice(r.aStart, r.aEnd) === "GTTAC" && "GGTTGACTA".slice(r.bStart, r.bEnd) === "GTTGAC", "Wikipedia example aligns GTT-AC with GTTGAC");
    ok(r.cigar === "3M1D2M" && r.matches === 5, "Wikipedia example CIGAR 3M1D2M (got " + r.cigar + ")");
}
{
    const rng = new Random(11);
    const word = (n, k) => Array.from({ length: n }, () => "ACGT"[rng.nextBounded(k)]).join("");
    let agree = 0, total = 0;
    for (let t = 0; t < 300; t++) {
        const a = word(1 + rng.nextBounded(60), 2 + rng.nextBounded(3)), b = word(1 + rng.nextBounded(60), 2 + rng.nextBounded(3));
        const opts = [{}, { match: 3, mismatch: -3, gap: -2 }, { match: 2, mismatch: -4, gap: -6 }, { match: 1, mismatch: 0, gap: 0 }][t % 4];
        total++;
        if (same(SmithWaterman(a, b, opts), reference(a, b, opts))) agree++;
        else if (agree + 3 > total) print("  mismatch: " + a + " / " + b + " " + JSON.stringify(opts) + " " + JSON.stringify(SmithWaterman(a, b, opts)) + " vs " + JSON.stringify(reference(a, b, opts)));
    }
    ok(agree === total, `full-matrix alignment equals the reference on ${total} random pairs (${agree})`);
    let bandAgree = 0;
    for (let t = 0; t < 200; t++) {
        const a = word(5 + rng.nextBounded(80), 4), b = word(5 + rng.nextBounded(80), 4);
        const opts = { match: 2, mismatch: -4, gap: -6, band: rng.nextBounded(10), diagonal: rng.nextBounded(21) - 10 };
        if (same(SmithWaterman(a, b, opts), reference(a, b, opts))) bandAgree++;
    }
    ok(bandAgree === 200, `banded alignment equals the band-restricted reference (${bandAgree}/200)`);
    let tokAgree = 0;
    for (let t = 0; t < 100; t++) {
        const a = Int32Array.from({ length: 1 + rng.nextBounded(50) }, () => rng.nextBounded(6) - 3), b = Int32Array.from({ length: 1 + rng.nextBounded(50) }, () => rng.nextBounded(6) - 3);
        if (same(SmithWaterman(a, b), reference(Array.from(a), Array.from(b)))) tokAgree++;
    }
    ok(tokAgree === 100, `Int32Array token alignment equals the reference (${tokAgree}/100)`);
    const ua = Uint32Array.of(0xffffffff, 1, 2), ub = Uint32Array.of(9, 0xffffffff, 1, 2);
    ok(SmithWaterman(ua, ub).score === 6, "Uint32Array compares raw 32-bit tokens");
}
{
    const r = SmithWaterman("xx😀é😀yy", "😀é😀", {});
    ok(r.score === 6 && r.aStart === 2 && r.aEnd === 5 && r.bStart === 0 && r.bEnd === 3, "code points, not UTF-16 units: " + JSON.stringify(r));
    const mixed = SmithWaterman("café", "cafe", {});
    ok(mixed.score === 6 && mixed.cigar === "3M", "ASCII against non-ASCII widens both sides: " + JSON.stringify(mixed));
    ok(same(SmithWaterman("", "ACGT"), { score: 0, aStart: 0, aEnd: 0, bStart: 0, bEnd: 0, matches: 0, cigar: "" }), "empty input scores 0");
    ok(SmithWaterman("AAAA", "TTTT").score === 0, "no similarity scores 0");
    const nt = SmithWaterman("TGTTACGG", "GGTTGACTA", { match: 3, mismatch: -3, gap: -2, traceback: false });
    ok(nt.score === 13 && nt.aEnd === 6 && nt.bEnd === 7 && nt.cigar === undefined, "traceback:false returns score and end positions only");
    const big = "A".repeat(30000);
    const b2 = SmithWaterman(big, big, { band: 3, traceback: false });
    ok(b2.score === 60000, "a 30k x 30k banded alignment runs in band memory");
}
{
    const throwsT = (f, cls, m) => { try { f(); ok(false, m + " did not throw"); } catch (e) { ok(e instanceof cls, m + " threw " + e.name + ": " + e.message); } };
    throwsT(() => SmithWaterman("a", Int32Array.of(1)), TypeError, "string vs typed array refused");
    throwsT(() => SmithWaterman(new Float64Array(2), new Float64Array(2)), TypeError, "8-byte typed arrays refused");
    throwsT(() => SmithWaterman("a", "b", { gapOpen: -3 }), TypeError, "unknown option refused");
    throwsT(() => SmithWaterman("a", "b", { match: 0 }), RangeError, "match must be positive");
    throwsT(() => SmithWaterman("a", "b", { gap: 1 }), RangeError, "a positive gap is refused");
    throwsT(() => SmithWaterman("a", "b", { match: 1, mismatch: 2 }), RangeError, "mismatch above match refused");
    throwsT(() => SmithWaterman("a", "b", { band: 1.5 }), RangeError, "fractional band refused");
    throwsT(() => SmithWaterman("A".repeat(30000), "A".repeat(30000)), RangeError, "an unbanded 9e8-cell problem is refused");
    throwsT(() => SmithWaterman("A".repeat(17000), "A".repeat(17000)), RangeError, "traceback beyond 2^28 cells is refused");
    throwsT(() => SmithWaterman("a"), TypeError, "one argument refused");
    const ta = new Int32Array(4);
    const hostile = { get match() { ta.buffer.transfer(); return 2; } };
    throwsT(() => SmithWaterman(ta, Int32Array.of(1), hostile), TypeError, "options are read before the token arrays are borrowed");
}
print("test_smith_waterman: " + pass + " passed, " + fail + " failed");
if (fail) throw new Error(fail + " failures");
