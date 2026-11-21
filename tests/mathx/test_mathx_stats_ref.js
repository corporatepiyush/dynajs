// covers: integration dyna:mathx.stats -- every distribution and hypothesis test of stats against scipy reference vectors
// Vectors: tests/corpus/ref/stats_vectors.js from tests/ref/gen_stats_ref_vectors.py (scipy 1.18.1). Distribution values must
// agree to 1e-9 relative (1e-300 absolute floor); test statistics to 1e-9 and p-values to 1e-8 relative (1e-14 absolute).
import { stats } from "dyna:mathx";
import V from "../corpus/ref/stats_vectors.js";

let pass = 0, fail = 0, worst = 0, worstAt = "";
function close(a, b, rel, abs) {
    if (Number.isNaN(b)) return Number.isNaN(a);
    if (!Number.isFinite(b)) return a === b;
    const e = Math.abs(a - b), tol = Math.max(abs, rel * Math.abs(b));
    if (e > abs && e / Math.max(Math.abs(b), abs) > worst) { worst = e / Math.max(Math.abs(b), abs); worstAt = ""; }
    return e <= tol;
}
function ok(c, msg) {
    if (c) pass++;
    else { fail++; print("FAIL " + msg); }
}
for (const row of V.dist) {
    const [fn, ...rest] = row, want = rest.pop();
    const got = stats[fn](...rest);
    ok(close(got, want, 1e-9, 1e-300), `${fn}(${rest.join(", ")}) = ${got}, scipy ${want}`);
    const vec = stats[fn](Float64Array.of(rest[0], rest[0]), ...rest.slice(1));
    ok(vec instanceof Float64Array && Object.is(vec[1], got), `${fn} over a Float64Array matches the scalar form`);
}
const tag = (t) => `${t.op}(${t.a ? "n=" + t.a.length : ""}${t.b ? ", m=" + t.b.length : ""} ${JSON.stringify(t.opts ?? t.method ?? "")})`;
for (const t of V.tests) {
    let got;
    if (typeof t.a === "number") t.a = V.samples[t.a];
    if (typeof t.b === "number") t.b = V.samples[t.b];
    switch (t.op) {
        case "ttest1samp": got = stats.ttest1samp(t.a, t.mu, t.opts); break;
        case "proportionsZTest": got = stats.proportionsZTest(...t.args, t.opts); break;
        case "chiSquare": got = stats.chiSquare(t.a, t.b, t.opts); break;
        case "chi2Contingency": got = stats.chi2Contingency(t.table, t.opts); break;
        case "rankData": got = stats.rankData(t.a, t.opts); break;
        case "pAdjust": got = stats.pAdjust(t.a, t.method); break;
        case "psi": case "psiCounts": got = stats[t.op](t.a, t.b, t.opts); break;
        default: got = stats[t.op](t.a, t.b, t.opts);
    }
    if (t.op === "rankData" || t.op === "pAdjust") {
        ok(got.length === t.r.length && t.r.every((v, i) => close(got[i], v, 1e-12, 1e-15)), `${tag(t)} = [${Array.from(got)}], want [${t.r}]`);
        continue;
    }
    if (t.op === "psi" || t.op === "psiCounts") {
        ok(close(got, t.r, 1e-12, 1e-15), `${tag(t)} = ${got}, want ${t.r}`);
        continue;
    }
    ok(close(got.statistic, t.r[0], 1e-9, 1e-14), `${tag(t)} statistic ${got.statistic}, scipy ${t.r[0]}`);
    ok(close(got.pvalue, t.r[1], 1e-8, 1e-14), `${tag(t)} pvalue ${got.pvalue}, scipy ${t.r[1]}`);
    if (t.r.length > 2) ok(close(got.df, t.r[2], 1e-12, 1e-14), `${tag(t)} df ${got.df}, scipy ${t.r[2]}`);
    if (t.expected) ok(t.expected.every((row, i) => row.every((v, j) => close(got.expected[i][j], v, 1e-12, 1e-14))), `${tag(t)} expected frequencies`);
}
print(`test_mathx_stats_ref: ${pass} passed, ${fail} failed (worst relative error ${worst.toExponential(2)}${worstAt})`);
if (fail) throw new Error(`${fail} reference mismatches`);
