// covers: dyna:mathx.stats
// log-gamma is memoised per thread in an 8-slot table keyed by the argument's exact bits (NaN bypasses it), so the
// distribution kernels (t, chi2, F, binomial, Poisson, incomplete gamma/beta and their inverses) stop recomputing
// lgamma(a), lgamma(b), lgamma(a+b) for the same shape parameters on every element and Newton step. A hit returns the
// value lgamma_r produced for those bits, so every result is unchanged. Oracles: fingerprints of 5000-element vector
// calls and scalar special functions recorded from the binary before the cache, and scalar calls that cycle through
// more shapes than the table holds (colliding slots) equal the same calls made one shape at a time.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_mathx_stats_lgamma_cache_oracle");
import * as mx from "dyna:mathx";
import { platform } from "dyna:sys";
// The recorded fingerprints come from macOS (Apple libm, clang); glibc exp/lgamma and libc tie orders differ in the last bit, so those sections run only where they were recorded. The same-binary oracles run everywhere.
const RECORDED_HERE = platform() === "darwin";

let checks = 0, fails = 0;
function eq(got, want, what) {
    checks++;
    if (got !== want) { fails++; if (fails < 20) print("FAIL: " + what + ": got " + got + ", want " + want); }
}
const before = [
    "t0.7 438446965 1268747723 1625835685 813470999",
    "t1 2028644229 1527357150 1132344846 1217854593",
    "t2.5 2091543017 -1460108042 1106245824 -1364962867",
    "t7 530110518 -2067033172 1396835987 -158892062",
    "t30 18104487 1312666264 -166112149 -1019205217",
    "t1000000 1130595543 1191062793 -219220678 205186446",
    "chi0.5 -1933823207 -854871992 -1311809057 1620394168",
    "chi1 252361485 1219798471 46134081 -1902301123",
    "chi3 2114614974 1301453891 649035129 1979740667",
    "chi9 1053347020 -317888629 -606666068 1630550321",
    "chi40 365051048 -1699358521 -232196878 -1631288616",
    "chi300 -1063331738 -969337553 -1806699285 931135488",
    "f1,1 74768967 -1600952195 -1896285546",
    "f3,7 -1503483936 1008437257 -1187620078",
    "f20,50 585314114 -266206963 -89773953",
    "binom -50656492 -570715413 -1149102440",
    "pois 1151716185 1237982649 1481335897",
    "special 1659919416",
];

__kitS.section("vector distributions and special functions equal the uncached build", () => {
    if (!RECORDED_HERE) { print("skip: fingerprints recorded on darwin, running on " + platform()); return; }
    const N = 5000;
    let seed = 9; const rnd = () => ((seed = (Math.imul(seed, 1103515245) + 12345) >>> 0) / 4294967296);
    const st = mx.stats;
    const h = (arr) => { const b = arr instanceof Float64Array ? arr : Float64Array.from(Array.isArray(arr) ? arr : [arr]); const u = new Int32Array(b.buffer, b.byteOffset, b.length * 2); let x = 0; for (let i = 0; i < u.length; i++) x = (Math.imul(x, 31) + u[i]) | 0; return x; };
    const a = Float64Array.from({ length: N }, () => rnd() * 10 - 5), b = Float64Array.from({ length: N }, (_, i) => a[i] * 0.7 + rnd());
    const ai = Float64Array.from({ length: N }, () => Math.floor(rnd() * 50)), p = Float64Array.from({ length: N }, () => rnd() * 0.999 + 0.0005);
    const out = [];
    for (const df of [0.7, 1, 2.5, 7, 30, 1e6]) out.push("t" + df + " " + h(st.tCdf(a, df)) + " " + h(st.tPpf(p, df)) + " " + h(st.tPdf(a, df)) + " " + h(st.tSf(a, df)));
    for (const k of [0.5, 1, 3, 9, 40, 300]) out.push("chi" + k + " " + h(st.chi2Cdf(ai, k)) + " " + h(st.chi2Ppf(p, k)) + " " + h(st.chi2Pdf(ai, k)) + " " + h(st.chi2Sf(ai, k)));
    for (const [d1, d2] of [[1, 1], [3, 7], [20, 50]]) out.push("f" + d1 + "," + d2 + " " + h(st.fCdf(ai, d1, d2)) + " " + h(st.fPpf(p, d1, d2)) + " " + h(st.fSf(ai, d1, d2)));
    out.push("binom " + h(st.binomCdf(ai, 60, 0.4)) + " " + h(st.binomPmf(ai, 60, 0.4)) + " " + h(st.binomSf(ai, 80, 0.7)));
    out.push("pois " + h(st.poissonCdf(ai, 20)) + " " + h(st.poissonPmf(ai, 3.3)) + " " + h(st.poissonSf(ai, 50)));
    const sp = []; for (let i = 0; i < 400; i++) { const x = Math.abs(a[i]) + 0.1; sp.push(mx.gamma(x), mx.gammainc(x, 2.5), mx.gammainc(x, 0.4, "upper"), mx.gammaincinv(p[i], x), mx.betainc(p[i], x, 2), mx.betaincinv(p[i], 0.5, x), mx.beta(x, 3), mx.betaln(x, 4), mx.lgamma(x)[0], mx.gammaln(x)); }
    out.push("special " + h(sp));
    for (let i = 0; i < before.length; i++) eq(out[i], before[i], "line " + i);
});

__kitS.section("shapes cycling through colliding slots give the one-at-a-time values", () => {
    const st = mx.stats, shapes = Array.from({ length: 37 }, (_, i) => 0.3 + i * 0.77), xs = [0.05, 0.4, 1.3, 2.2, 5.5, 11];
    const alone = new Map();
    for (const k of shapes) alone.set(k, xs.map((x) => [st.chi2Cdf(x, k), st.tCdf(x - 1, k), st.chi2Ppf(x / 12, k), mx.betainc(x / 12, k, 2.5), mx.gammaincinv(x / 12, k)]));
    let bad = 0;
    for (let round = 0; round < 3; round++)
        for (let xi = 0; xi < xs.length; xi++)
            for (let si = 0; si < shapes.length; si++) {
                const k = shapes[(si * 7 + round) % shapes.length], x = xs[xi];
                const got = [st.chi2Cdf(x, k), st.tCdf(x - 1, k), st.chi2Ppf(x / 12, k), mx.betainc(x / 12, k, 2.5), mx.gammaincinv(x / 12, k)];
                const want = alone.get(k)[xi];
                for (let q = 0; q < 5; q++) if (!Object.is(got[q], want[q])) bad++;
            }
    eq(bad, 0, "values that differ from the isolated calls");
});

if (fails) throw new Error(fails + " of " + checks + " checks failed");
print("test_mathx_stats_lgamma_cache_oracle: " + checks + " checks passed");
__kitS.end();
