// covers: dyna:ml.DecisionTreeRegressor
// Exact (non-histogram) tree nodes with 512 or more rows sort each candidate feature with an LSD radix sort on the
// order-preserving bits of the key instead of the comparison quicksort; histogram binning always does. Radix keeps
// equal keys in input order while the quicksort does not, and with tied feature values a regression or weighted tree
// sums y and w in that order, so those fits keep the quicksort for any feature whose node range has two distinct rows
// with one value (found once per subtree, from the node where the radix ran); unweighted classification counts are
// order-free and always use the radix. Oracles: a JS exact CART regressor on tie-free data (bit-equal predictions),
// and prediction fingerprints recorded from the binary before the change on tied (integer) and tie-free data for
// regression, weighted, classification, forest, boosting and histogram fits.
// mlk-tree.c compiles with FP contraction off: clang fused different multiply-adds at -O0/-O1 (san, coverage) than at -O2,
// so the weighted regressor and forest regressor fingerprints only held on release. dtrw and rfr were re-recorded
// from a quicksort-only build (DYN_TREE_RADIX_MIN 2^40) with contraction off; the radix build matches them at -O0..-O2.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_ml_decision_tree_regressor_radix_sort_oracle");
import * as ml from "dyna:ml";
import { platform } from "dyna:sys";
// The recorded fingerprints come from macOS (Apple libm, clang); glibc exp/lgamma and libc tie orders differ in the last bit, so those sections run only where they were recorded. The same-binary oracles run everywhere.
const RECORDED_HERE = platform() === "darwin";

let checks = 0, fails = 0;
function eq(got, want, what) {
    checks++;
    if (got !== want) { fails++; if (fails < 20) print("FAIL: " + what + ": got " + got + ", want " + want); }
}

function data(N, C, INT) {
    let seed = 11;
    const rnd = () => ((seed = (Math.imul(seed, 1103515245) + 12345) >>> 0) / 4294967296);
    const X = new Float64Array(N * C), yr = new Float64Array(N), yc = new Float64Array(N), w = new Float64Array(N);
    for (let i = 0; i < N; i++) {
        let a = 0;
        for (let j = 0; j < C; j++) { const v = INT ? Math.floor(rnd() * 7) : rnd() * 10 - 5; X[i * C + j] = v; a += (j + 1) * v * (j % 2 ? -1 : 1); }
        yr[i] = a + rnd(); yc[i] = (Math.floor(a) % 3 + 3) % 3; w[i] = 0.5 + rnd();
    }
    return { X, yr, yc, w, N, C };
}
const hash = (arr) => { const b = new Float64Array(Array.from(arr)); const u = new Int32Array(b.buffer); let x = 0; for (let i = 0; i < u.length; i++) x = (Math.imul(x, 31) + u[i]) | 0; return x; };

function cart(X, y, N, C, maxDepth) {
    const nodes = [];
    const build = (idx, depth) => {
        const self = nodes.length;
        nodes.push({ f: -1 });
        const n = idx.length;
        let pure = true;
        for (let i = 1; i < n; i++) if (y[idx[i]] !== y[idx[0]]) { pure = false; break; }
        const leaf = () => { let s = 0; for (const r of idx) s += y[r]; nodes[self].v = s / n; return self; };
        if (pure || n < 2 || depth >= maxDepth) return leaf();
        let bestGain = 0, bestF = -1, bestThr = 0, bestOrder = null;
        for (let f = 0; f < C; f++) {
            const ord = idx.slice().sort((a, b) => X[a * C + f] - X[b * C + f]);
            let sumAll = 0, sqAll = 0;
            for (const r of ord) { const v = y[r]; sumAll += v; sqAll += v * v; }
            const parent = sqAll - sumAll * sumAll / n;
            let sumL = 0, sqL = 0, xprev = X[ord[0] * C + f], improved = false;
            for (let i = 1; i < n; i++) {
                const xcur = X[ord[i] * C + f], v = y[ord[i - 1]];
                sumL += v; sqL += v * v;
                if (xcur !== xprev) {
                    const sumR = sumAll - sumL, sqR = sqAll - sqL;
                    const gain = parent - ((sqL - sumL * sumL / i) + (sqR - sumR * sumR / (n - i)));
                    if (gain > bestGain) { bestGain = gain; bestF = f; bestThr = xprev + (xcur - xprev) * 0.5; if (bestThr >= xcur) bestThr = xprev; improved = true; }
                }
                xprev = xcur;
            }
            if (improved) bestOrder = ord;
        }
        if (bestF < 0) return leaf();
        let nl = 0;
        while (nl < n && X[bestOrder[nl] * C + bestF] <= bestThr) nl++;
        if (nl === 0 || nl === n) { idx = bestOrder; return leaf(); }
        const l = build(bestOrder.slice(0, nl), depth + 1), r = build(bestOrder.slice(nl), depth + 1);
        Object.assign(nodes[self], { f: bestF, t: bestThr, l, r });
        return self;
    };
    build(Array.from({ length: N }, (_, i) => i), 0);
    return (x, o) => { let at = 0; while (nodes[at].f >= 0) at = x[o + nodes[at].f] <= nodes[at].t ? nodes[at].l : nodes[at].r; return nodes[at].v; };
}

await __kitS.section("tie-free exact regression trees equal a JS CART bit for bit", async () => {
    for (const [N, C, depth] of [[600, 4, 5], [3000, 6, 8], [9000, 3, 12]]) {
        const d = data(N, C, false);
        const m = await new ml.DecisionTreeRegressor({ maxDepth: depth }).fit(d.X, d.yr, N, C);
        const got = await m.predict(d.X, N, C);
        m.close();
        const ref = cart(d.X, d.yr, N, C, depth);
        let bad = 0, first = -1;
        for (let i = 0; i < N; i++) if (got[i] !== ref(d.X, i * C)) { bad++; if (first < 0) first = i; }
        eq(bad, 0, "N=" + N + " C=" + C + " depth=" + depth + " mismatched rows (first " + first + ")");
    }
});

const before = {
    "3000/0": "dtc -316669952 dtr 1459593265 dtrw -271703730 dtcw 1016070144 rfc 1593835520 rfr 90737667 gbr -1541974639 gbc 429916160 xgb 1820430252 rfh 825121915",
    "3000/1": "dtc 563085312 dtr -1743002767 dtrw -242629896 dtcw 438304768 rfc 1578106880 rfr 450745582 gbr -938969141 gbc 2099249152 xgb 846959507 rfh 989293037",
};
const WEIGHTED = "423006980,1654764210,-915158130,2044581066";
const probaGot = {}, probaBefore = { "3000/0": -453167607, "3000/1": -1849834698 };
await __kitS.section("tied and tie-free fits predict exactly what the quicksort build did", async () => {
    if (!RECORDED_HERE) { print("skip: fingerprints recorded on darwin, running on " + platform()); return; }
    for (const key of Object.keys(before)) {
        const [N, INT] = key.split("/").map(Number), C = 6, d = data(N, C, INT === 1), out = [];
        const run = async (nm, m, y, opts) => { await m.fit(d.X, y, N, C, opts); out.push(nm + " " + hash(await m.predict(d.X, N, C))); m.close(); };
        await run("dtc", new ml.DecisionTreeClassifier({ maxDepth: 12 }), d.yc);
        await run("dtr", new ml.DecisionTreeRegressor({ maxDepth: 12 }), d.yr);
        await run("dtrw", new ml.DecisionTreeRegressor({ maxDepth: 12 }), d.yr, { sampleWeight: d.w });
        await run("dtcw", new ml.DecisionTreeClassifier({ maxDepth: 12 }), d.yc, { sampleWeight: d.w });
        {
            const m = await new ml.DecisionTreeClassifier({ maxDepth: 3 }).fit(d.X, d.yc, N, C, { sampleWeight: d.w });
            const pp = await m.predictProba(d.X, N, C);
            m.close();
            probaGot[key] = hash(Array.from(pp).flat());
        }
        await run("rfc", new ml.RandomForestClassifier({ nEstimators: 6, maxDepth: 10, seed: 3 }), d.yc);
        await run("rfr", new ml.RandomForestRegressor({ nEstimators: 6, maxDepth: 10, seed: 3 }), d.yr);
        await run("gbr", new ml.GradientBoostingRegressor({ nEstimators: 8, maxDepth: 4, seed: 3 }), d.yr);
        await run("gbc", new ml.GradientBoostingClassifier({ nEstimators: 8, maxDepth: 4, seed: 3 }), d.yc.map((v) => (v > 0 ? 1 : 0)));
        await run("xgb", new ml.XGBRegressor({ nEstimators: 8, maxDepth: 4, seed: 3 }), d.yr);
        await run("rfh", new ml.RandomForestRegressor({ nEstimators: 4, maxDepth: 8, seed: 3, maxBins: 64 }), d.yr);
        const got = out.join(" ").split(" "), want = before[key].split(" ");
        for (let i = 0; i < want.length; i += 2) eq(got[i + 1], want[i + 1], "N/INT=" + key + " " + want[i]);
        eq(probaGot[key], probaBefore[key], "N/INT=" + key + " weighted classifier predictProba");
    }
});

const SPECIAL = "1435193616,-1441267712";
await __kitS.section("a weighted classifier on tied values sums weights in the quicksort order", async () => {
    const got = [];
    for (const N of [600, 2000]) for (const depth of [2, 4]) {
        const C = 2, X = new Float64Array(N * C), y = new Float64Array(N), w = new Float64Array(N);
        let seed = 77 + N;
        const rnd = () => ((seed = (Math.imul(seed, 1103515245) + 12345) >>> 0) / 4294967296);
        for (let i = 0; i < N; i++) { X[i * C] = Math.floor(rnd() * 4); X[i * C + 1] = Math.floor(rnd() * 3); y[i] = (X[i * C] + (rnd() < 0.3 ? 1 : 0)) % 3; w[i] = rnd() < 0.2 ? 1e16 : 1 + rnd() * 1e-3; }
        const m = await new ml.DecisionTreeClassifier({ maxDepth: depth }).fit(X, y, N, C, { sampleWeight: w });
        got.push(hash(Array.from(await m.predictProba(X, N, C)).flat()));
        m.close();
    }
    eq(got.join(","), WEIGHTED, "predictProba fingerprints (N 600/2000 x depth 2/4)");
});

await __kitS.section("signed zeros, negatives and huge magnitudes split where the quicksort put them", async () => {
    const N = 2048, C = 2, X = new Float64Array(N * C), y = new Float64Array(N);
    const vals = [-1e300, -3.5, -0, 0, 1e-300, 2.25, 7, 1e300, -1e-300, -7];
    for (let i = 0; i < N; i++) { X[i * C] = vals[i % vals.length]; X[i * C + 1] = vals[(i * 7 + 3) % vals.length] * 0.5 + (i % 13); y[i] = (i % vals.length) * 1.5 + (i % 13) * 0.25; }
    const m = await new ml.DecisionTreeRegressor({ maxDepth: 10 }).fit(X, y, N, C);
    const got = await m.predict(X, N, C);
    m.close();
    const mc = await new ml.DecisionTreeClassifier({ maxDepth: 10 }).fit(X, y.map((v) => Math.floor(v) % 4), N, C);
    const pc = await mc.predict(X, N, C);
    mc.close();
    eq(hash(got) + "," + hash(pc), SPECIAL, "regressor and classifier predictions");
});

if (fails) throw new Error(fails + " of " + checks + " checks failed");
print("test_ml_decision_tree_regressor_radix_sort_oracle: " + checks + " checks passed");
__kitS.end();
