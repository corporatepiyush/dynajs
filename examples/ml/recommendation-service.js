// Recommendation service — implicit-feedback matrix factorisation, an IVF candidate index, hybrid ranking with
// diversity, business rules, offline evaluation and an HTTP serving layer.
//
// WHAT IT IS
//   "Recommended for you" for a shop with 2,000 users and 800 products, built the way production systems are:
//
//   events       views, carts and purchases with timestamps, weighted into implicit confidence (1 + alpha*count)
//   split        temporal: everything before the cut trains, each user's later purchases are the test set
//   factors      weighted ALS (Hu, Koren & Volinsky) with 16 factors, solved per user and per item with a small
//                Cholesky, in float32 vectors scored with SIMD gemv
//   content      item attributes (category, price band, brand) reduced with PCA give cold-start items a vector
//   index        an inverted-file index: k-means over item vectors, a query probes the nearest 8 of 32 lists,
//                recall@100 against exact search is measured
//   ranking      candidates are re-scored with factors + recency + popularity prior, then MMR re-ranking trades
//                relevance for category diversity
//   rules        never recommend what was bought in the last 30 days or what is out of stock
//   evaluation   precision@10, recall@10, NDCG@10 and catalogue coverage for: popularity, item-kNN, ALS, ALS+MMR
//   serving      GET /recommend/:user?k=10 with an LRU cache invalidated by POST /events
//   kernels      each ALS row solve gathers its interacted vectors into one K x n block and forms Σ(c-1)vvᵀ with a
//                single gemm and Σc·v with a single gemvT (not K axpy calls per interaction); item-kNN scores a user
//                with ONE gemv against the sum of their item vectors (similarity is linear); MMR keeps a running
//                max-similarity per candidate and updates it with one gemv per pick instead of re-dotting the list
//
// MODULES  dyna:simd (gemv, gemvT, gemm, dot, axpy, topkIndices, normL2, scale) · dyna:ml (KMeans, PCA) · dyna:structures (LRU, BitSet)
//          · dyna:http · dyna:mathx (stats, solveSPD) · dyna:cli (Table) · dyna:random
//
// RUN      dynajs examples/ml/recommendation-service.js     simulates 3 months of shop events and serves recommendations

import { gemv, gemvT, gemm, dot, axpy, topkIndices, normL2, scale } from "dyna:simd";
import { KMeans, PCA } from "dyna:ml";
import { LRU, BitSet } from "dyna:structures";
import { App } from "dyna:http";
import { stats, solveSPD } from "dyna:mathx";
import { Table } from "dyna:cli";
import { Random } from "dyna:random";
const table = (rows) => Table(rows.slice(1), { head: rows[0] });

const rng = new Random(31);
const U = 2000,
    I = 800,
    K = 16,
    DAY = 86400_000,
    T0 = Date.UTC(2026, 0, 1);

// =============================================================================
// Catalogue and simulated taste
// =============================================================================
const CATS = ["running", "hiking", "yoga", "cycling", "swimming", "tennis", "climbing", "camping"];
const items = Array.from({ length: I }, (_, i) => ({
    id: i,
    cat: i % CATS.length,
    brand: rng.nextBounded(25),
    price: Math.round(Math.exp(rng.normal(3.6, 0.7))),
    inStock: rng.nextFloat() > 0.05,
    launched: rng.nextFloat() < 0.04 ? 85 : rng.nextBounded(40), // a few launch late
}));
const TRUE_K = 6;
const latent = (n) => Float32Array.from({ length: TRUE_K }, () => rng.normal(0, 1));
const catTaste = CATS.map(() => latent());
const itemTrue = items.map((it) => Float32Array.from(catTaste[it.cat], (v) => v + rng.normal(0, 0.5)));
const users = Array.from({ length: U }, () => {
    const favs = [rng.nextBounded(CATS.length), rng.nextBounded(CATS.length)];
    return Float32Array.from(
        { length: TRUE_K },
        (_, k) => 0.6 * catTaste[favs[0]][k] + 0.4 * catTaste[favs[1]][k] + rng.normal(0, 0.3),
    );
});
const popularity = items.map(() => rng.exponential(1));
const events = []; // [user, item, kind, t]
for (let u = 0; u < U; u++) {
    const n = 8 + rng.poisson(20);
    const affinity = items.map((it, i) =>
        it.launched > 80 ? -Infinity : dot(users[u], itemTrue[i]) + 0.5 * Math.log(popularity[i]),
    );
    const top = topkIndices(
        Float32Array.from(affinity, (v) => (v === -Infinity ? -1e9 : v)),
        60,
    );
    for (let e = 0; e < n; e++) {
        const i = top[Math.min(59, Math.floor(rng.exponential(1 / 12)))];
        const t = T0 + rng.nextBounded(90) * DAY + rng.nextBounded(DAY);
        events.push([u, i, "view", t]);
        if (rng.nextFloat() < 0.3) events.push([u, i, "cart", t + 60_000]);
        if (rng.nextFloat() < 0.12) events.push([u, i, "purchase", t + 120_000]);
    }
}
events.sort((a, b) => a[3] - b[3]);
const CUT = T0 + 75 * DAY;
const WEIGHT = { view: 1, cart: 4, purchase: 10 };

// =============================================================================
// Interaction matrix (sparse, by user and by item)
// =============================================================================
const byUser = Array.from({ length: U }, () => new Map()),
    byItem = Array.from({ length: I }, () => new Map());
const lastBought = new Map(); // "u:i" -> t
const testSet = Array.from({ length: U }, () => new Set());
for (const [u, i, kind, t] of events) {
    if (t >= CUT) {
        if (kind === "purchase") testSet[u].add(i);
        continue;
    }
    byUser[u].set(i, (byUser[u].get(i) || 0) + WEIGHT[kind]);
    byItem[i].set(u, (byItem[i].get(u) || 0) + WEIGHT[kind]);
    if (kind === "purchase") lastBought.set(`${u}:${i}`, t);
}
for (let u = 0; u < U; u++) for (const i of testSet[u]) if (lastBought.has(`${u}:${i}`)) testSet[u].delete(i); // repurchases are not discovery

// =============================================================================
// Weighted ALS
// =============================================================================
const ALPHA = 2,
    LAMBDA = 0.1;
const X = Array.from({ length: U }, () => Float32Array.from({ length: K }, () => rng.normal(0, 0.1)));
const Y = Array.from({ length: I }, () => Float32Array.from({ length: K }, () => rng.normal(0, 0.1)));
function gram(M) {
    // M^T M, K x K
    const G = new Float64Array(K * K);
    for (const v of M) for (let a = 0; a < K; a++) for (let b = a; b < K; b++) G[a * K + b] += v[a] * v[b];
    for (let a = 0; a < K; a++) for (let b = 0; b < a; b++) G[a * K + b] = G[b * K + a];
    return G;
}
function alsStep(target, fixed, rows) {
    const G = gram(fixed),
        A = new Float32Array(K * K),
        rhs = new Float32Array(K);
    let V = new Float32Array(0),
        Vt = new Float32Array(0),
        Vs = new Float32Array(0),
        conf = new Float32Array(0);
    for (let r = 0; r < rows.length; r++) {
        const n = rows[r].size;
        if (V.length < n * K) {
            V = new Float32Array(n * K * 2);
            Vt = new Float32Array(n * K * 2);
            Vs = new Float32Array(n * K * 2);
            conf = new Float32Array(n * 2);
        }
        let m = 0;
        for (const [j, count] of rows[r]) {
            // gather: v, and sqrt(c-1)·v as rows and columns
            const c = 1 + ALPHA * count,
                v = fixed[j],
                w = Math.sqrt(c - 1);
            V.set(v, m * K);
            for (let a = 0; a < K; a++) {
                const x = w * v[a];
                Vt[a * n + m] = x;
                Vs[m * K + a] = x;
            }
            conf[m++] = c;
        }
        A.set(G);
        if (n) {
            gemm(A, Vt.subarray(0, K * n), Vs.subarray(0, n * K), K, K, n, 1, 1); // A += Σ (c-1) v vᵀ
            gemvT(rhs, V.subarray(0, n * K), conf.subarray(0, n), n, K, 0); // rhs = Σ c v
        } else rhs.fill(0);
        for (let a = 0; a < K; a++) A[a * K + a] += LAMBDA * (n || 1);
        target[r] = Float32Array.from(solveSPD(Float64Array.from(A), Float64Array.from(rhs)));
    }
}
const alsStart = Date.now();
for (let it = 0; it < 8; it++) {
    alsStep(X, Y, byUser);
    alsStep(Y, X, byItem);
}
const alsMs = Date.now() - alsStart;
const itemMatrix = new Float32Array(I * K);
Y.forEach((v, i) => itemMatrix.set(v, i * K));

// =============================================================================
// Content vectors for cold-start items
// =============================================================================
const attrs = items.map((it) => [...CATS.map((_, c) => (c === it.cat ? 1 : 0)), Math.log(it.price) / 5, it.brand / 25]);
const pca = await new PCA(6).fit(attrs);
const content = await pca.transform(attrs);
const cold = items.filter((it) => byItem[it.id].size === 0).map((it) => it.id);
for (const i of cold) {
    // borrow the mean factor vector of the nearest warm items in content space
    const near = items
        .filter((it) => byItem[it.id].size > 3)
        .map((it) => [it.id, content[i].reduce((s, v, k) => s + (v - content[it.id][k]) ** 2, 0)])
        .sort((a, b) => a[1] - b[1])
        .slice(0, 10);
    const v = new Float32Array(K);
    for (const [j] of near) for (let k = 0; k < K; k++) v[k] += Y[j][k] / near.length;
    Y[i] = v;
    itemMatrix.set(v, i * K);
}

// =============================================================================
// IVF index over item vectors
// =============================================================================
const LISTS = 32,
    PROBE = 8;
const toUnit = (v) => scale(v, 1 / (normL2(v) || 1));
const unit = Y.map((v) => toUnit(Float32Array.from(v)));
const km = await new KMeans(LISTS, 5).fit(unit.map((v) => Array.from(v)));
const assignment = await km.predict(unit.map((v) => Array.from(v)));
const unitMatrix = new Float32Array(I * K);
unit.forEach((v, i) => unitMatrix.set(v, i * K));
const lists = Array.from({ length: LISTS }, () => []);
assignment.forEach((c, i) => lists[c].push(i));
const centroids = lists.map((ids) =>
    toUnit(
        Float32Array.from({ length: K }, (_, k) => ids.reduce((s, i) => s + unit[i][k], 0) / Math.max(1, ids.length)),
    ),
);
function annCandidates(q, n) {
    const qn = toUnit(Float32Array.from(q));
    const nearest = centroids
        .map((c, k) => [k, dot(c, qn)])
        .sort((a, b) => b[1] - a[1])
        .slice(0, PROBE);
    const ids = nearest.flatMap(([k]) => lists[k]);
    return ids
        .map((i) => [i, dot(Y[i], q)])
        .sort((a, b) => b[1] - a[1])
        .slice(0, n)
        .map(([i]) => i);
}
function exactTop(q, n) {
    const scores = new Float32Array(I);
    gemv(scores, itemMatrix, q, I, K, 0);
    return Array.from(topkIndices(scores, n));
}

// =============================================================================
// Ranking, rules, MMR
// =============================================================================
const popScore = new Float64Array(I);
for (const [, i, kind, t] of events) if (t < CUT) popScore[i] += WEIGHT[kind] * Math.exp(-(CUT - t) / (14 * DAY));
const popMax = Math.max(...popScore);
const allowed = (u, i, now) =>
    items[i].inStock && !(lastBought.has(`${u}:${i}`) && now - lastBought.get(`${u}:${i}`) < 30 * DAY);
function recommend(u, k, { now = CUT, mmr = true, method = "als" } = {}) {
    if (method === "pop" || byUser[u].size === 0)
        return [...popScore.keys()]
            .sort((a, b) => popScore[b] - popScore[a])
            .filter((i) => allowed(u, i, now))
            .slice(0, k);
    if (method === "knn") {
        // Σ_j sim(i, j) = unit_i · Σ_j unit_j
        const profile = new Float32Array(K);
        for (const [j] of byUser[u]) axpy(profile, 1, unit[j]);
        const scores = gemv(new Float32Array(I), unitMatrix, profile, I, K, 0);
        return [...topkIndices(scores, Math.min(I, k + byUser[u].size + 60))]
            .sortBy((i) => -scores[i])
            .filter((i) => !byUser[u].has(i) && allowed(u, i, now))
            .slice(0, k);
    }
    const cand = annCandidates(X[u], 200).filter((i) => !byUser[u].has(i) && allowed(u, i, now));
    const rel = new Map(cand.map((i) => [i, dot(X[u], Y[i]) + (0.05 * popScore[i]) / popMax]));
    if (!mmr) return cand.sort((a, b) => rel.get(b) - rel.get(a)).slice(0, k);
    const chosen = [],
        LAMBDA_MMR = 0.97;
    const relMax = Math.max(...rel.values()),
        relMin = Math.min(...rel.values());
    const C = new Float32Array(cand.length * K);
    cand.forEach((i, r) => C.set(unit[i], r * K));
    const simMax = new Float32Array(cand.length).fill(-Infinity),
        sims = new Float32Array(cand.length),
        taken = new Uint8Array(cand.length);
    while (chosen.length < k && chosen.length < cand.length) {
        let best = -1,
            bestScore = -Infinity;
        for (let r = 0; r < cand.length; r++) {
            if (taken[r]) continue;
            const s =
                (LAMBDA_MMR * (rel.get(cand[r]) - relMin)) / (relMax - relMin || 1) -
                (1 - LAMBDA_MMR) * (chosen.length ? simMax[r] : 0);
            if (s > bestScore) {
                bestScore = s;
                best = r;
            }
        }
        taken[best] = 1;
        chosen.push(cand[best]);
        gemv(sims, C, unit[cand[best]], cand.length, K, 0); // similarity of every candidate to the pick
        for (let r = 0; r < cand.length; r++) if (sims[r] > simMax[r]) simMax[r] = sims[r];
    }
    return chosen;
}

// =============================================================================
// Offline evaluation
// =============================================================================
function evaluate(method, mmr) {
    const p = [],
        r = [],
        ndcg = [],
        covered = new BitSet(I);
    for (let u = 0; u < U; u += 2) {
        // every other user keeps item-kNN affordable
        if (!testSet[u].size) continue;
        const recs = recommend(u, 10, { method, mmr });
        recs.forEach((i) => covered.set(i));
        const hits = recs.map((i) => (testSet[u].has(i) ? 1 : 0));
        p.push(hits.reduce((a, b) => a + b, 0) / 10);
        r.push(hits.reduce((a, b) => a + b, 0) / testSet[u].size);
        const dcg = hits.reduce((s, h, k) => s + h / Math.log2(k + 2), 0),
            idcg = [...Array(Math.min(10, testSet[u].size)).keys()].reduce((s, k) => s + 1 / Math.log2(k + 2), 0);
        ndcg.push(dcg / idcg);
    }
    const div = [];
    for (let u = 0; u < 200; u++) div.push(new Set(recommend(u, 10, { method, mmr }).map((i) => items[i].cat)).size);
    return {
        method: method + (method === "als" && mmr ? "+mmr" : ""),
        p: stats.mean(p),
        r: stats.mean(r),
        ndcg: stats.mean(ndcg),
        coverage: covered.toArray().length / I,
        cats: stats.mean(div),
        n: p.length,
    };
}
const evalRows = [evaluate("pop", false), evaluate("knn", false), evaluate("als", false), evaluate("als", true)];
let recallAnn = 0;
for (let u = 0; u < 300; u++) {
    const exact = new Set(exactTop(X[u], 100));
    recallAnn += annCandidates(X[u], 100).filter((i) => exact.has(i)).length / 100;
}
recallAnn /= 300;

// =============================================================================
// Serving
// =============================================================================
const cache = new LRU(5000);
let cacheHits = 0,
    computed = 0;
const app = new App({ port: 0 });
app.get("/recommend/:user", (req) => {
    const u = Number(req.params.user),
        k = Math.min(50, Number(req.query.k) || 10);
    if (!Number.isInteger(u) || u < 0 || u >= U) return { status: 404, body: "unknown user" };
    const key = `${u}:${k}`,
        hit = cache.get(key);
    if (hit) {
        cacheHits++;
        return hit;
    }
    computed++;
    const out = {
        user: u,
        items: recommend(u, k).map((i) => ({ id: i, category: CATS[items[i].cat], price: items[i].price })),
    };
    cache.put(key, out);
    return out;
});
app.post("/events", (req) => {
    const e = JSON.parse(req.body);
    byUser[e.user].set(e.item, (byUser[e.user].get(e.item) || 0) + WEIGHT[e.kind]);
    if (e.kind === "purchase") lastBought.set(`${e.user}:${e.item}`, CUT);
    for (const k of [10, 20]) cache.delete(`${e.user}:${k}`);
    return { status: 202, body: "" };
});
app.start();
const base = `http://127.0.0.1:${app.port}`;
const first = await (await fetch(`${base}/recommend/42`)).json();
const again = await (await fetch(`${base}/recommend/42`)).json();
await (
    await fetch(`${base}/events`, {
        method: "POST",
        body: JSON.stringify({ user: 42, item: first.items[0].id, kind: "purchase" }),
    })
).text();
const after = await (await fetch(`${base}/recommend/42`)).json();
const unknown = await fetch(`${base}/recommend/999999`);
await unknown.text();
app.close();

// =============================================================================
// Checks and report
// =============================================================================
const results = {};
const [pop, knn, als, alsMmr] = evalRows;
results[`ALS beats popularity (NDCG ${als.ndcg.toFixed(3)} vs ${pop.ndcg.toFixed(3)})`] =
    als.ndcg > pop.ndcg * 1.5 && als.r > pop.r;
results[
    `MMR adds diversity (${als.cats.toFixed(2)} -> ${alsMmr.cats.toFixed(2)} categories) at a small relevance cost`
] = alsMmr.cats > als.cats && alsMmr.ndcg > als.ndcg * 0.8;
results["personalised lists cover far more of the catalogue than popularity"] = alsMmr.coverage > pop.coverage * 3;
results[`IVF recall@100 vs exact search ${recallAnn.toFixed(3)} probing ${PROBE}/${LISTS} lists`] = recallAnn > 0.8;
results[`${cold.length} cold-start items received content-based vectors`] =
    cold.length > 0 && cold.every((i) => Y[i].some((v) => v !== 0));
let violations = 0;
for (let u = 0; u < U; u += 7)
    for (const i of recommend(u, 10)) if (!items[i].inStock || byUser[u].has(i)) violations++;
results["no out-of-stock or already-interacted items are recommended"] = violations === 0;
results["the second request is served from cache"] = JSON.stringify(first) === JSON.stringify(again) && cacheHits === 1;
results["a purchase event invalidates the cache and removes the item"] =
    !after.items.some((x) => x.id === first.items[0].id) && computed === 2;
results["unknown users are 404"] = unknown.status === 404;

print("recommendation-service");
print(
    `  data      ${events.length} events from ${U} users on ${I} items; ${testSet.filter((s) => s.size).length} users with future purchases`,
);
print(
    `  model     ALS ${K} factors, 8 sweeps in ${alsMs} ms; IVF recall@100 ${recallAnn.toFixed(3)}; PCA explains ${(pca.explainedVarianceRatio.reduce((a, b) => a + b, 0) * 100).toFixed(0)}% of item attributes`,
);
print(
    table([
        ["method", "P@10", "R@10", "NDCG@10", "coverage", "categories/list"],
        ...evalRows.map((e) => [
            e.method,
            e.p.toFixed(4),
            e.r.toFixed(4),
            e.ndcg.toFixed(4),
            `${(e.coverage * 100).toFixed(1)}%`,
            e.cats.toFixed(2),
        ]),
    ]),
);
print(
    `  user 42   ${first.items
        .slice(0, 5)
        .map((x) => `#${x.id} ${x.category}`)
        .join(", ")}`,
);
km.close();
pca.close();
check(results);

function check(results) {
    const names = Object.keys(results);
    const failed = names.filter((name) => !results[name]);
    for (const name of failed) print("FAIL " + name);
    if (failed.length) throw new Error(`${failed.length} of ${names.length} checks failed`);
}
