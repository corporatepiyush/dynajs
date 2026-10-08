// mathx.stats.quantile/median find their order statistics by SELECTION, not
// by sorting. The oracle is the definition: sort a copy with the engine's own
// numeric sort and interpolate (dynajs.d.ts: linear interpolation between the
// two nearest ranks, the numpy "linear" method). Sizes straddle the selection's
// own thresholds (the 24-element insertion cutoff) and the inputs include the
// shapes that defeat a naive pivot.
import { stats } from "dyna:mathx";

let n = 0, failed = 0;
function ok(cond, what, detail) {
    n++;
    if (!cond) { failed++; print("  FAIL  " + what + (detail === undefined ? "" : "  [" + detail + "]")); }
}
function reference(arr, q) {
    const v = Float64Array.from(arr).sort();
    const pos = q * (v.length - 1), lo = Math.floor(pos), frac = pos - lo;
    return frac === 0 || lo + 1 >= v.length ? v[lo] : v[lo] + frac * (v[lo + 1] - v[lo]);
}
// The interpolation lo + frac * (hi - lo) is one rounding where the compiler
// fuses the multiply-add (arm64) and two where it does not (baseline x86-64),
// so an interpolated value is compared to within 2 ulp; a value that IS an
// element (frac == 0) must match exactly, which is what `reference` returns then.
function same(got, want) {
    if (got === want || (Number.isNaN(got) && Number.isNaN(want))) return true;
    if (!Number.isFinite(got) || !Number.isFinite(want)) return false;
    return Math.abs(got - want) <= 2 * Math.abs(want) * Number.EPSILON;
}
let seed = 12345;
const rnd = () => { seed = (seed * 1103515245 + 12345) & 0x7fffffff; return seed / 0x7fffffff; };

const shapes = {
    random: (m) => Array.from({ length: m }, () => rnd() * 2000 - 1000),
    sorted: (m) => Array.from({ length: m }, (_, i) => i * 0.5),
    reversed: (m) => Array.from({ length: m }, (_, i) => (m - i) * 0.25),
    "all equal": (m) => new Array(m).fill(3.25),
    "two values": (m) => Array.from({ length: m }, (_, i) => (i * 7919) % 3 === 0 ? 1 : 2),
    "organ pipe": (m) => Array.from({ length: m }, (_, i) => Math.min(i, m - 1 - i)),
    "few distinct": (m) => Array.from({ length: m }, () => Math.floor(rnd() * 5)),
    "sawtooth": (m) => Array.from({ length: m }, (_, i) => i % 17),
    "one outlier": (m) => Array.from({ length: m }, (_, i) => i === (m >> 1) ? 1e300 : -i),
    "infinities": (m) => Array.from({ length: m }, (_, i) => i % 11 === 0 ? Infinity : i % 13 === 0 ? -Infinity : rnd()),
    // the classic median-of-three killer: every 3-sample taken from the middle is extreme
    "median-of-3 killer": (m) => { const a = new Array(m); const h = m >> 1; for (let i = 0; i < h; i++) { a[i] = i % 2 ? i : h + i; a[h + i] = 2 * (i + 1); } if (m % 2) a[m - 1] = m; return a; },
};
const sizes = [1, 2, 3, 23, 24, 25, 26, 48, 49, 100, 1000, 4097, 60000];
const qs = [0, 1, 0.5, 0.25, 0.75, 0.999, 0.001, 1 / 3];
let cases = 0, wrong = 0, first = "";
for (const [name, gen] of Object.entries(shapes)) {
    for (const m of sizes) {
        const data = gen(m);
        const keep = data.slice();
        for (const q of m > 5000 ? qs.slice(0, 4) : qs) {
            cases++;
            const got = stats.quantile(data, q), want = reference(keep, q);
            if (!same(got, want)) {
                wrong++;
                if (!first) first = `${name} n=${m} q=${q}: got ${got} want ${want}`;
            }
        }
        cases++;
        const gm = stats.median(new Float64Array(data)), wm = reference(keep, 0.5);
        if (gm !== wm) { wrong++; if (!first) first = `${name} n=${m} median(f64): got ${gm} want ${wm}`; }
        if (data.some((x, i) => !Object.is(x, keep[i]))) { wrong++; if (!first) first = `${name} n=${m}: the caller's array was reordered`; }
    }
}
ok(wrong === 0, cases + " quantile/median cases equal the sort-and-interpolate definition", wrong + " wrong; " + first);
ok(cases > 1000, "the sweep covered the shapes and sizes", cases);

ok(Number.isNaN(stats.median([1, NaN, 3])), "a NaN anywhere makes the median NaN");
let e = null;
try { stats.median([]); } catch (x) { e = x; }
ok(e instanceof RangeError, "the median of nothing is a RangeError", e && e.message);

// The work is bounded on the worst shape: selection on the killer input costs
// about what it costs on random input, not n^2. Compared as a RATIO on the same
// machine in the same process, with a generous factor.
{
    const m = 400000;
    const best = (data) => { let b = Infinity; for (let i = 0; i < 5; i++) { const t = performance.now(); stats.median(data); b = Math.min(b, performance.now() - t); } return b; };
    const tRandom = best(Float64Array.from(shapes.random(m)));
    const tKiller = best(Float64Array.from(shapes["median-of-3 killer"](m)));
    const tSorted = best(Float64Array.from(shapes.sorted(m)));
    ok(tKiller < 25 * tRandom + 5, "the median-of-3 killer input stays within a constant factor of random input", `killer ${tKiller.toFixed(2)} ms, random ${tRandom.toFixed(2)} ms`);
    ok(tSorted < 25 * tRandom + 5, "sorted input stays within a constant factor of random input", `sorted ${tSorted.toFixed(2)} ms, random ${tRandom.toFixed(2)} ms`);
}

print("test_mathx_quantile_select: " + (n - failed) + " passed, " + failed + " failed");
if (failed) throw new Error("test_mathx_quantile_select: " + failed + " failures");
