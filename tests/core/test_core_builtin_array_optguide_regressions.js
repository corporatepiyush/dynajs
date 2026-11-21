// covers: c:src/builtins/builtin-array.c
// Split from tests/test_optguide_regressions.js: the core sections.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_core_builtin_array_optguide_regressions");
export {};

let pass = 0, fail = 0;
function ok(c, m) { if (c) { pass++; } else { fail++; print("FAIL: " + m); } }
function eq(a, b, m) { ok(Object.is(a, b) || a === b, m + "  (got " + a + ", want " + b + ")"); }
function eqj(a, b, m) {
    const x = JSON.stringify(a), y = JSON.stringify(b);
    ok(x === y, m + "  (got " + x + ", want " + y + ")");
}

__kitS.section("median n", () => {
    for (const n of [1, 2, 3, 4, 11, 12, 13, 24, 25, 101, 1000]) {
        const a = []; for (let i = 0; i < n; i++) a.push((i * 7919) % n);
        const s = a.slice().sort((x, y) => x - y);
        const want = (n & 1) ? s[(n - 1) / 2] : (s[n / 2 - 1] + s[n / 2]) / 2;
        eq(a.median(), want, "median n=" + n + " matches a full sort");
    }
    eq([5, 5, 5, 5].median(), 5, "median all-equal even (the 3-way partition's equal band)");
    eq([5, 5, 5].median(), 5, "median all-equal odd");
    eq([1, 2, 2, 3].median(), 2, "median with a tie across the middle");
    {
        const up = [], down = [];
        for (let i = 0; i < 1001; i++) { up.push(i); down.push(1000 - i); }
        eq(up.median(), 500, "median already-sorted (median-of-three, not O(n^2))");
        eq(down.median(), 500, "median reverse-sorted");
    }
    eq([1e308, 1e308].median(), Infinity, "median keeps sum-then-halve overflow");
    ok(Number.isNaN([1, NaN, 3].median()), "median propagates NaN");
    ok(Number.isNaN([NaN, 1, 3].median()), "median propagates NaN whatever its position");
    ok(Number.isNaN([].median()), "median of an empty array is NaN");

    eqj([1, 2].zip([3, 4]), [[1, 3], [2, 4]], "zip fast arm");
    eqj([].zip([]), [], "zip of two empty arrays");
    eqj([1, 2, 3].zip([9]), [[1, 9]], "zip truncates to the shorter operand");
    {
        const a = [1, 2]; a.length = 5;
        const r = a.zip([9, 8, 7, 6, 5]);
        eq(r.length, 5, "zip length>count takes the generic arm without over-reading");
        eq(r[0][0], 1, "...and the present elements are right");
        ok(r[2][0] === undefined, "...and the absent ones are undefined");
    }
    {
        const h = [1, 2, 3]; delete h[1];
        const r = h.zip([9, 8, 7]);
        eq(r.length, 3, "zip over a hole");
        ok(r[1][0] === undefined, "the hole reads as undefined");
    }
    {
        let hits = 0;
        const g = [1, 2];
        Object.defineProperty(g, "0", { get() { hits++; return 42; }, configurable: true });
        const r = g.zip([9, 8]);
        eq(r[0][0], 42, "zip runs an index getter rather than bypassing it");
        eq(hits, 1, "zip runs the getter exactly once");
    }
    eqj(new Proxy([1, 2], {}).zip([9, 8]), [[1, 9], [2, 8]], "zip over a proxy receiver");
    { const self = [1, 2]; eqj(self.zip(self), [[1, 1], [2, 2]], "zip with itself"); }

    for (let n = 2; n <= 17; n++) {
        const src = [];
        for (let i = 0; i < n; i++) src.push({ k: i % 3, id: i });
        const want = src.map((_, i) => i).sort((a, b) => (src[a].k - src[b].k) || (a - b));
        eqj(src.sortBy("k").map(x => x.id), want, "sortBy stable at n=" + n + " (pass parity)");
    }
    eqj([3, 1, 2].sortBy(), [1, 2, 3], "sortBy odd pass count lands in the caller's buffer");
    eqj([{ k: 2 }, { k: 1 }, { k: 3 }].sortBy("k", true).map(x => x.k), [3, 2, 1], "sortBy descending");
});
__kitS.end();
print("test_core_optguide_regressions: " + pass + " passed, " + fail + " failed");
if (fail) throw new Error(fail + " failures");
