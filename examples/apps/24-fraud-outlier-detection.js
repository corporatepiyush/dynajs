// 24 · Outlier detection — density clustering separates normal card activity from suspicious transactions.
//
// WHAT IT SHOWS
//   - dyna:ml DBScan: clusters of any shape, and a "noise" label (-1) for points that fit nowhere
//   - why DBSCAN suits this job: no need to say how many clusters exist, and outliers are a first-class result
//   - choosing eps from the data (the k-distance heuristic) instead of guessing
//   - explaining each flagged point by its nearest normal behaviour
//
// RUN      dynajs examples/apps/24-fraud-outlier-detection.js

import { DBScan, StandardScaler } from "dyna:ml";
import { Random } from "dyna:random";
import { distL2 } from "dyna:simd";

// ---- data: [amount, hour of day, km from home] ------------------------------
const rng = new Random(99);
const txns = [];
const add = (n, amount, hour, km, kind) => {
    for (let i = 0; i < n; i++)
        txns.push({ kind, row: [Math.max(1, rng.normal(...amount)), rng.normal(...hour), Math.max(0, rng.normal(...km))] });
};
add(260, [12, 4], [8.5, 1], [2, 1], "coffee");         // small morning purchases near home
add(180, [85, 20], [18.5, 1.5], [6, 2], "groceries");  // evening shops
add(60, [45, 10], [13, 1], [1, 0.5], "lunch");
// A handful of things that do not look like this cardholder at all.
const suspicious = [[2400, 3.2, 4100], [1900, 2.5, 3900], [15, 4.1, 8800], [990, 23.8, 640], [3100, 14, 2]];
for (const row of suspicious) txns.push({ kind: "planted", row });
rng.shuffle(txns);
const X = txns.map((t) => t.row);

// ---- scale: amounts are in hundreds, hours in tens, km in thousands ----------
const scaler = new StandardScaler().fit(X);
const Xs = scaler.transform(X);

// ---- pick eps: distance to the 5th nearest neighbour, at the 95th percentile --
// Dense points have near neighbours; outliers do not. The value where that
// distance starts to climb is a principled eps.
const MIN_PTS = 5;
const f32 = Xs.map((r) => Float32Array.from(r));
const kDist = f32.map((p) => f32.map((q) => distL2(p, q)).sort((a, b) => a - b)[MIN_PTS]);
const eps = [...kDist].sort((a, b) => a - b)[Math.floor(kDist.length * 0.95)];

const model = new DBScan(eps, MIN_PTS).fit(Xs);
const labels = model.labels;

// ---- explain ---------------------------------------------------------------
const flagged = [];
labels.forEach((label, i) => {
    if (label !== -1) return;
    // Nearest point that DID belong to a cluster: "this looks least unlike X".
    let nearest = -1, best = Infinity;
    for (let j = 0; j < f32.length; j++) {
        if (labels[j] === -1) continue;
        const d = distL2(f32[i], f32[j]);
        if (d < best) { best = d; nearest = j; }
    }
    flagged.push({ index: i, row: X[i], kind: txns[i].kind, nearestKind: txns[nearest].kind, distance: best });
});
flagged.sort((a, b) => b.distance - a.distance);

console.log(`${X.length} transactions, eps ${eps.toFixed(2)}, ${model.nClusters} behaviour clusters, ${flagged.length} flagged`);
for (const f of flagged.slice(0, 8))
    console.log(`  amount ${f.row[0].toFixed(0).padStart(5)}  hour ${f.row[1].toFixed(1).padStart(4)}  ${f.row[2].toFixed(0).padStart(5)} km` +
                `  (${f.distance.toFixed(1)} sigma-units from the nearest ${f.nearestKind} purchase)`);

// ---- self-test -------------------------------------------------------------
const check = (cond, what) => { if (!cond) throw new Error("self-test failed: " + what); };
const caught = flagged.filter((f) => f.kind === "planted").length;
check(caught === suspicious.length, "every planted transaction is flagged: " + caught);
check(model.nClusters >= 2 && model.nClusters <= 4, "the everyday behaviours form a few clusters: " + model.nClusters);
check(flagged.length < X.length * 0.08, "few ordinary purchases are flagged: " + flagged.length);
check(flagged.slice(0, 3).every((f) => f.kind === "planted"), "the strangest points rank first");
console.log("self-test passed");
scaler.close(); model.close();
