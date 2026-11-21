// covers: dyna:dataframe.DataFrame
// QUANTILE on an unmasked f64 column interpolates between the radix-selected lo value and the next value
// above it. Both scans are branchless in C (the radix bucket compaction keeps every key and advances on a
// match; the next-above scan is split into a vectorized <= count and an fmin reduction that maps values
// <= lo to +Inf and exits early when lo repeats past rank+1). The oracle is a JS sort + linear interpolation;
// an interpolated result may differ by the C side's fused multiply-add rounding (2 ulp of |lo| + |hi|), a selected one never.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_dataframe_data_frame_quantile_select_oracle");
import { DataFrame } from "dyna:dataframe";

let checks = 0, fails = 0;
let scale = 0;
function same(got, want, what) {
    checks++;
    const ok = Object.is(got, want) || (typeof got === "number" && typeof want === "number" && Number.isFinite(want)
        && Math.abs(got - want) <= 2 * Number.EPSILON * scale);
    if (!ok) { fails++; print("FAIL: " + what + ": got " + got + ", want " + want); }
}
let seed = 0x9E3779B9;
const rnd = () => { seed ^= seed << 13; seed ^= seed >>> 17; seed ^= seed << 5; seed >>>= 0; return seed / 4294967296; };
function oracle(v, q) {
    const s = Array.from(v).filter((x) => !Number.isNaN(x)).sort((a, b) => a - b);
    if (!s.length) return undefined;
    const pos = q * (s.length - 1), lo = Math.floor(pos), frac = pos - lo;
    scale = 0;
    if (frac === 0 || lo + 1 >= s.length) return s[lo];
    scale = Math.abs(s[lo]) + Math.abs(s[lo + 1]);
    return s[lo] + frac * (s[lo + 1] - s[lo]);
}
const QS = [0, 0.001, 0.1, 0.25, 0.37, 0.5, 0.63, 0.75, 0.9, 0.999, 1];
const GENS = {
    uniform: () => rnd() * 1000 - 500,
    small_ints: () => (rnd() * 7) | 0,
    two_values: () => (rnd() < 0.9 ? 3.5 : 4.25),
    wide_exp: () => (rnd() - 0.5) * Math.pow(10, ((rnd() * 40) | 0) - 20),
    negatives: () => -1 - rnd(),
};

__kitS.section("random columns of every shape match the sort oracle", () => {
    for (const [name, gen] of Object.entries(GENS))
        for (const n of [1, 2, 3, 7, 16, 33, 257, 1000, 4099]) {
            const v = new Float64Array(n);
            for (let i = 0; i < n; i++) v[i] = gen();
            const df = new DataFrame({ v });
            for (const q of QS) same(df.QUANTILE("v", q), oracle(v, q), name + " n=" + n + " q=" + q);
            same(df.MEDIAN("v"), oracle(v, 0.5), name + " n=" + n + " median");
        }
});

__kitS.section("the next value above lo is found past a run of duplicates and past infinities", () => {
    const v = new Float64Array([5, 1, 1, 1, 1, 9, 2, -Infinity, Infinity, 1]);
    const df = new DataFrame({ v });
    for (const q of QS) same(df.QUANTILE("v", q), oracle(v, q), "dups+inf q=" + q);
    const w = new Float64Array([2, 2, 2, 2, 3]);
    same(new DataFrame({ w }).QUANTILE("w", 0.9), 2 + 0.6 * 1, "lo at the last duplicate takes the next above");
    same(new DataFrame({ w }).QUANTILE("w", 0.5), 2, "lo repeated past rank+1 is the answer");
});

__kitS.section("NaN entries are skipped by the select and by the next-above scan", () => {
    const v = new Float64Array([NaN, 4, NaN, 1, 3, NaN, 2]);
    const df = new DataFrame({ v });
    for (const q of QS) same(df.QUANTILE("v", q), oracle(v, q), "nan q=" + q);
});

if (fails) throw new Error(fails + " of " + checks + " checks failed");
print("test_dataframe_data_frame_quantile_select_oracle: " + checks + " checks passed");
__kitS.end();
