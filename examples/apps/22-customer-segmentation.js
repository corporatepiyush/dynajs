// 22 · Customer segmentation — clusters customers by behaviour and describes each segment in plain terms.
//
// WHAT IT SHOWS
//   - dyna:ml StandardScaler + KMeans, and picking k with the elbow of inertia
//   - scaling first, so recency (hundreds of days) does not outvote orders (tens)
//   - turning centroids back into business units with inverseTransform
//   - a fixed seed, so the same data always yields the same segments
//
// RUN      dynajs examples/apps/22-customer-segmentation.js

import { StandardScaler, KMeans } from "dyna:ml";
import { Random } from "dyna:random";

// ---- data: [orders per year, average basket, days since last order] ---------
// Three behaviours are planted: loyal regulars, big occasional spenders, and
// customers who have drifted away.
const rng = new Random(11);
const planted = [
    { n: 120, orders: [24, 4], basket: [35, 8],   recency: [10, 5] },    // regulars
    { n: 60,  orders: [3, 1],  basket: [240, 40], recency: [60, 20] },   // big spenders
    { n: 90,  orders: [2, 1],  basket: [30, 10],  recency: [300, 40] },  // lapsed
];
const X = [];
for (const g of planted)
    for (let i = 0; i < g.n; i++)
        X.push([Math.max(0, rng.normal(...g.orders)), Math.max(1, rng.normal(...g.basket)), Math.max(0, rng.normal(...g.recency))]);
rng.shuffle(X);

// ---- scale, then choose k --------------------------------------------------
const scaler = new StandardScaler().fit(X);
const Xs = scaler.transform(X);

// Inertia always falls as k grows; the useful k is where the drop stops being
// dramatic. Print the curve and take the largest relative improvement.
const inertia = [];
for (let k = 1; k <= 6; k++) {
    const model = new KMeans(k, 5).fit(Xs);
    inertia.push(model.inertia);
    model.close();
}
let bestK = 2, bestGain = 0;
for (let k = 2; k < inertia.length; k++) {
    const gain = (inertia[k - 2] - inertia[k - 1]) / inertia[k - 2] - (inertia[k - 1] - inertia[k]) / inertia[k - 1];
    if (gain > bestGain) { bestGain = gain; bestK = k; }
}
console.log("inertia by k:", inertia.map((v, i) => `${i + 1}:${v.toFixed(0)}`).join("  "), "-> k =", bestK);

// ---- final model -----------------------------------------------------------
const model = new KMeans(bestK, 5).fit(Xs);
const assignment = model.predict(Xs);

// Describe each segment using the ORIGINAL units, computed from its members.
const segments = Array.from({ length: bestK }, (_, id) => {
    const members = X.filter((_, i) => assignment[i] === id);
    const mean = (col) => members.reduce((s, r) => s + r[col], 0) / members.length;
    const profile = { orders: mean(0), basket: mean(1), recency: mean(2) };
    const label = profile.recency > 180 ? "lapsed" : profile.basket > 120 ? "big spender" : "regular";
    return { id, size: members.length, label, ...profile };
}).sort((a, b) => b.size - a.size);

for (const s of segments)
    console.log(`  ${s.label.padEnd(12)} ${String(s.size).padStart(4)} customers  ` +
                `${s.orders.toFixed(1)} orders/yr  basket ${s.basket.toFixed(0)}  last order ${s.recency.toFixed(0)}d ago`);

// A new customer is scored with the SAME scaler and model.
const newcomer = scaler.transform([[22, 40, 6]]);
const newcomerSegment = segments.find((s) => s.id === model.predict(newcomer)[0]);
console.log("a weekly shopper lands in:", newcomerSegment.label);

// ---- self-test -------------------------------------------------------------
const check = (cond, what) => { if (!cond) throw new Error("self-test failed: " + what); };
check(bestK === 3, "the elbow finds the three planted behaviours: " + bestK);
check(segments.map((s) => s.label).sort().join() === "big spender,lapsed,regular", "each behaviour gets its own segment");
check(segments.every((s) => planted.some((g) => Math.abs(g.n - s.size) <= 5)), "segment sizes match the planted groups");
check(newcomerSegment.label === "regular", "the newcomer is classified as a regular");

console.log("self-test passed");
for (const m of [scaler, model]) m.close();
