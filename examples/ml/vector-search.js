// Vector search — an IVF (inverted-file) approximate nearest-neighbour index over embeddings, with filtered search,
// int8 scalar quantisation and recall measured against exact brute force.
//
// WHAT IT IS
//   The retrieval layer of a semantic-search or RAG service (FAISS IVF-Flat / pgvector ivfflat in miniature):
//
//   exact       brute force as one gemv per query over the whole row-major matrix, then topkIndices — the oracle
//   train       KMeans (dyna:ml) on a sample picks nlist coarse centroids; every vector is assigned by one gemv
//               against the centroid matrix and argmax
//   lists       vectors are re-laid out list by list so each inverted list is ONE contiguous subarray: a probe is a
//               single gemv over that slice, not a gather
//   search      nprobe nearest lists by centroid score, candidate scores concatenated, topkIndices; recall@10 must
//               rise with nprobe and reach 1.0 when every list is probed
//   filters     a metadata predicate is a RoaringBitmap of allowed ids; pre-filtering (score only allowed rows)
//               is compared with post-filtering (search then drop), which loses results on selective filters
//   quantise    per-dimension int8 codes (scale/offset) cut memory 4x; asymmetric distance over dequantised rows
//               re-ranked with full precision keeps recall
//   PQ          product quantisation, 8 subspaces x 64 sub-centroids: 8 bytes per vector (32x smaller); a query builds
//               8 small score tables and scores the whole corpus as 8 gathers + adds over code columns, then
//               re-ranks 100 candidates in full precision; table scores must equal the reconstructed dot product
//   live        2000 tombstoned deletes (a RoaringBitmap) and 1000 inserts in a flat delta segment merged into
//               every result, then a compaction that rebuilds the lists; no deleted id may come back and recall is
//               measured against a one-by-one scan of the live set
//   dedup       near-duplicate embeddings (cosine > 0.995) found by the index are merged with UnionFind
//
// MODULES  dyna:ml (KMeans) · dyna:simd (gemv, gather, add, topkIndices, argmax, normL2, scale, dot, distCos) · dyna:structures
//          (RoaringBitmap, UnionFind) · dyna:random · dyna:cli · Array extensions
//
// RUN      dynajs examples/ml/vector-search.js [vectors]

import { KMeans } from "dyna:ml";
import { gemv, topkIndices, argmax, normL2, scale, dot, distCos, gather, add } from "dyna:simd";
import { RoaringBitmap, UnionFind } from "dyna:structures";
import { Random } from "dyna:random";
import { Table } from "dyna:cli";

const results = {};
const N = Number(scriptArgs[1] ?? 20_000),
    D = 64,
    NLIST = 64,
    K = 10,
    QUERIES = 200;
const rng = new Random(42);
const unit = (v) => scale(v, 1 / normL2(v));

// =============================================================================
// Corpus: clustered unit vectors (topics) with a few planted near-duplicates
// =============================================================================
const TOPICS = 150;
const topicCentres = Array.from({ length: TOPICS }, () => {
    const c = new Float32Array(D);
    for (let d = 0; d < D; d++) c[d] = rng.normal();
    return unit(c);
});
const X = new Float32Array(N * D),
    topicOf = new Int32Array(N),
    lang = new Uint8Array(N);
for (let i = 0; i < N; i++) {
    const t = rng.nextBounded(TOPICS),
        u = rng.nextBounded(TOPICS),
        w = rng.nextFloat(),
        row = X.subarray(i * D, (i + 1) * D);
    for (let d = 0; d < D; d++)
        row[d] = w * topicCentres[t][d] + (1 - w) * topicCentres[u][d] + (0.6 / Math.sqrt(D)) * rng.normal();
    unit(row);
    topicOf[i] = t;
    lang[i] = rng.nextBounded(20);
}
const planted = [];
for (let j = 0; j < 30; j++) {
    const src = rng.nextBounded(N),
        dst = rng.nextBounded(N);
    if (src === dst) continue;
    const row = X.subarray(dst * D, (dst + 1) * D);
    row.set(X.subarray(src * D, (src + 1) * D));
    for (let d = 0; d < D; d++) row[d] += 0.002 * rng.normal();
    unit(row);
    planted.push([src, dst]);
}
const Q = new Float32Array(QUERIES * D);
for (let q = 0; q < QUERIES; q++) {
    const row = Q.subarray(q * D, (q + 1) * D),
        t = rng.nextBounded(TOPICS),
        u = rng.nextBounded(TOPICS),
        w = rng.nextFloat();
    for (let d = 0; d < D; d++)
        row[d] = w * topicCentres[t][d] + (1 - w) * topicCentres[u][d] + (0.6 / Math.sqrt(D)) * rng.normal();
    unit(row);
}

// =============================================================================
// Exact search
// =============================================================================
const scores = new Float32Array(N);
function exact(q, k = K, allowed = null) {
    gemv(scores, X, q, N, D, 0);
    if (allowed) for (let i = 0; i < N; i++) if (!allowed.has(i)) scores[i] = -2;
    return topkIndices(scores, k);
}
const tExact = performance.now();
const truth = [];
for (let q = 0; q < QUERIES; q++) truth.push(new Set(exact(Q.subarray(q * D, (q + 1) * D))));
const exactMs = (performance.now() - tExact) / QUERIES;
results["exact top-1 of a corpus vector is itself"] = exact(X.subarray(123 * D, 124 * D), 1)[0] === 123;
results["exact search agrees with a scalar dot-product scan"] = (() => {
    const q = Q.subarray(0, D);
    let best = -1,
        bestScore = -Infinity;
    for (let i = 0; i < N; i++) {
        let s = 0;
        for (let d = 0; d < D; d++) s += X[i * D + d] * q[d];
        if (s > bestScore) {
            bestScore = s;
            best = i;
        }
    }
    return exact(q, 1)[0] === best;
})();

// =============================================================================
// IVF index
// =============================================================================
const tBuild = performance.now();
const sampleIdx = [...Array(N).keys()].sample(4000);
const sample = new Float64Array(sampleIdx.length * D);
sampleIdx.forEach((i, r) => {
    for (let d = 0; d < D; d++) sample[r * D + d] = X[i * D + d];
});
const km = await new KMeans(NLIST, 7).fit(sample, sampleIdx.length, D);
const sampleLabels = await km.predict(sample, sampleIdx.length, D);
const C = new Float32Array(NLIST * D),
    counts = new Int32Array(NLIST);
sampleLabels.forEach((c, r) => {
    counts[c]++;
    for (let d = 0; d < D; d++) C[c * D + d] += sample[r * D + d];
});
for (let c = 0; c < NLIST; c++) unit(C.subarray(c * D, (c + 1) * D));
km.close();

const listOf = new Int32Array(N),
    cscore = new Float32Array(NLIST),
    listSize = new Int32Array(NLIST);
for (let i = 0; i < N; i++) {
    gemv(cscore, C, X.subarray(i * D, (i + 1) * D), NLIST, D, 0);
    listOf[i] = argmax(cscore);
    listSize[listOf[i]]++;
}
const listStart = new Int32Array(NLIST + 1);
for (let c = 0; c < NLIST; c++) listStart[c + 1] = listStart[c] + listSize[c];
const XL = new Float32Array(N * D),
    idOf = new Int32Array(N),
    fill = listStart.slice(0, NLIST);
for (let i = 0; i < N; i++) {
    const pos = fill[listOf[i]]++;
    XL.set(X.subarray(i * D, (i + 1) * D), pos * D);
    idOf[pos] = i;
}
const buildMs = performance.now() - tBuild;
results["every vector lands in exactly one list"] = listStart[NLIST] === N && new Set(idOf).size === N;

const cand = new Float32Array(N),
    candId = new Int32Array(N);
function ivfSearch(q, nprobe, k = K, allowed = null) {
    gemv(cscore, C, q, NLIST, D, 0);
    const probes = topkIndices(cscore, nprobe);
    let n = 0;
    for (const c of probes) {
        const lo = listStart[c],
            len = listStart[c + 1] - lo;
        if (!len) continue;
        const out = cand.subarray(n, n + len);
        gemv(out, XL.subarray(lo * D, (lo + len) * D), q, len, D, 0);
        for (let j = 0; j < len; j++) {
            candId[n + j] = idOf[lo + j];
            if (allowed && !allowed.has(idOf[lo + j])) out[j] = -2;
        }
        n += len;
    }
    const top = topkIndices(cand.subarray(0, n), Math.min(k, n));
    return { ids: Array.from(top, (j) => candId[j]), scanned: n };
}
const recallAt = (nprobe) => {
    let hit = 0,
        scanned = 0;
    const t = performance.now();
    for (let q = 0; q < QUERIES; q++) {
        const r = ivfSearch(Q.subarray(q * D, (q + 1) * D), nprobe);
        hit += r.ids.filter((id) => truth[q].has(id)).length;
        scanned += r.scanned;
    }
    return { nprobe, recall: hit / (QUERIES * K), ms: (performance.now() - t) / QUERIES, scanned: scanned / QUERIES };
};
const sweep = [1, 2, 4, 8, 16, NLIST].map(recallAt);
print(
    Table(
        sweep
            .map((s) => [s.nprobe, s.recall.toFixed(3), s.scanned.format(0), s.ms.toFixed(3)])
            .concat([["exact", "1.000", N.format(), exactMs.toFixed(3)]]),
        { head: ["nprobe", "recall@10", "rows scanned", "ms/query"] },
    ),
);
results["recall rises with nprobe"] = sweep.every((s, i) => i === 0 || s.recall >= sweep[i - 1].recall - 1e-9);
results["probing every list is exact"] = sweep.at(-1).recall === 1;
results["nprobe=8 reaches 0.9 recall"] = sweep[3].recall >= 0.9;
results["nprobe=8 scans under a quarter of the corpus"] = sweep[3].scanned < N / 4;
print(`index build ${buildMs.toFixed(0)} ms (KMeans on 4000 samples, ${NLIST} lists)`);

// =============================================================================
// Filtered search
// =============================================================================
{
    const allowed = new RoaringBitmap();
    for (let i = 0; i < N; i++) if (lang[i] === 3) allowed.add(i);
    let pre = 0,
        post = 0,
        preShort = 0;
    for (let q = 0; q < 50; q++) {
        const qv = Q.subarray(q * D, (q + 1) * D);
        const want = new Set(exact(qv, K, allowed));
        const a = ivfSearch(qv, 8, K, allowed).ids.filter((id) => allowed.has(id));
        const b = ivfSearch(qv, 8, K).ids.filter((id) => allowed.has(id));
        pre += a.filter((id) => want.has(id)).length;
        post += b.filter((id) => want.has(id)).length;
        if (b.length < K) preShort++;
    }
    results["pre-filtering keeps recall on a 5% filter"] = pre / (50 * K) > 0.7;
    results["post-filtering starves selective filters"] = post < pre && preShort > 40;
    print(
        `filtered (5% of corpus): pre-filter recall ${(pre / 500).toFixed(3)}, post-filter ${(post / 500).toFixed(3)}`,
    );
}

// =============================================================================
// Scalar quantisation: int8 codes per dimension, re-ranked in full precision
// =============================================================================
{
    const lo = new Float32Array(D).fill(Infinity),
        hi = new Float32Array(D).fill(-Infinity);
    for (let i = 0; i < N; i++)
        for (let d = 0; d < D; d++) {
            const v = XL[i * D + d];
            if (v < lo[d]) lo[d] = v;
            if (v > hi[d]) hi[d] = v;
        }
    const codes = new Uint8Array(N * D);
    for (let i = 0; i < N * D; i++) {
        const d = i % D;
        codes[i] = Math.round(((XL[i] - lo[d]) / (hi[d] - lo[d] || 1)) * 255);
    }
    // Asymmetric distance: q . (lo + code * step) = q . lo + (q * step) . code — one gemv over the codes as floats.
    const codeF = Float32Array.from(codes);
    const step = Float32Array.from(hi, (h, d) => (h - lo[d]) / 255);
    let hits = 0,
        rawHits = 0;
    const qs = new Float32Array(D),
        approx = new Float32Array(N);
    for (let q = 0; q < QUERIES; q++) {
        const qv = Q.subarray(q * D, (q + 1) * D);
        for (let d = 0; d < D; d++) qs[d] = qv[d] * step[d];
        const bias = dot(qv, lo);
        gemv(approx, codeF, qs, N, D, 0);
        const shortlist = Array.from(topkIndices(approx, 50)).sort((a, b) => approx[b] - approx[a]);
        rawHits += shortlist.slice(0, K).filter((p) => truth[q].has(idOf[p])).length;
        const rer = Array.from(shortlist, (p) => [p, dot(XL.subarray(p * D, (p + 1) * D), qv)])
            .sort((a, b) => b[1] - a[1])
            .slice(0, K);
        hits += rer.filter(([p]) => truth[q].has(idOf[p])).length;
        if (q === 0) {
            const p0 = shortlist[0];
            results["dequantised score is close to the true score"] =
                Math.abs(approx[p0] + bias - dot(XL.subarray(p0 * D, (p0 + 1) * D), qv)) < 0.01;
        }
    }
    results["int8 codes are 4x smaller than floats"] = codes.byteLength * 4 === XL.byteLength;
    results["re-ranking int8 candidates recovers recall"] = hits / (QUERIES * K) > 0.98 && hits >= rawHits;
    print(
        `int8: raw recall ${(rawHits / (QUERIES * K)).toFixed(3)}, after full-precision re-rank ${(hits / (QUERIES * K)).toFixed(3)}`,
    );
}

// =============================================================================
// Product quantisation: 8 bytes per vector, scored through per-query lookup tables
// =============================================================================
// Each vector is cut into M sub-vectors of D/M dims; each sub-vector is replaced by the id of its nearest of KS
// sub-centroids (KMeans per subspace). A query builds M tables of KS dot products once; a vector's approximate
// score is then M table reads, done for the whole corpus as M gathers + adds over Int32 code columns.
const M = 8,
    DS = D / M,
    KS = 64;
const tPq = performance.now();
const subCentroids = new Float32Array(M * KS * DS),
    codeIdx = Array.from({ length: M }, () => new Int32Array(N)); // m*KS + code, ready to index the flat table
for (let m = 0; m < M; m++) {
    const sub = new Float64Array(N * DS);
    for (let i = 0; i < N; i++) for (let d = 0; d < DS; d++) sub[i * DS + d] = X[i * D + m * DS + d];
    const train = sub.subarray(0, 4000 * DS),
        km = await new KMeans(KS, 11 + m).fit(train, 4000, DS),
        lab = await km.predict(sub, N, DS, { as: "f64" });
    km.close();
    const sums = new Float64Array(KS * DS),
        cnt = new Int32Array(KS);
    for (let i = 0; i < N; i++) {
        cnt[lab[i]]++;
        for (let d = 0; d < DS; d++) sums[lab[i] * DS + d] += sub[i * DS + d];
    }
    for (let c = 0; c < KS; c++)
        for (let d = 0; d < DS; d++) subCentroids[(m * KS + c) * DS + d] = cnt[c] ? sums[c * DS + d] / cnt[c] : 0;
    for (let i = 0; i < N; i++) codeIdx[m][i] = m * KS + lab[i];
}
const pqBuildMs = performance.now() - tPq;
{
    const lut = new Float32Array(M * KS),
        approx = new Float32Array(N),
        part = new Float32Array(N);
    const adc = (qv) => {
        for (let m = 0; m < M; m++)
            gemv(
                lut.subarray(m * KS, (m + 1) * KS),
                subCentroids.subarray(m * KS * DS, (m + 1) * KS * DS),
                qv.subarray(m * DS, (m + 1) * DS),
                KS,
                DS,
                0,
            );
        approx.fill(0);
        for (let m = 0; m < M; m++) add(approx, approx, gather(part, lut, codeIdx[m]));
        return approx;
    };
    let raw = 0,
        reranked = 0,
        worst = 0;
    const tq = performance.now();
    for (let q = 0; q < QUERIES; q++) {
        const qv = Q.subarray(q * D, (q + 1) * D),
            a = adc(qv),
            shortlist = Array.from(topkIndices(a, 100));
        raw += Array.from(topkIndices(a, K)).filter((i) => truth[q].has(i)).length;
        const rer = shortlist
            .map((i) => [i, dot(X.subarray(i * D, (i + 1) * D), qv)])
            .sort((x, y) => y[1] - x[1])
            .slice(0, K);
        reranked += rer.filter(([i]) => truth[q].has(i)).length;
        if (q < 3)
            // the definition: the dot product of the query with the vector rebuilt from its codes
            for (const i of shortlist.slice(0, 20)) {
                let s = 0;
                for (let m = 0; m < M; m++) {
                    const c = codeIdx[m][i] - m * KS;
                    for (let d = 0; d < DS; d++) s += qv[m * DS + d] * subCentroids[(m * KS + c) * DS + d];
                }
                worst = Math.max(worst, Math.abs(s - a[i]));
            }
    }
    const pqMs = (performance.now() - tq) / QUERIES;
    results[
        `PQ lookup-table scores equal the dot product with the reconstructed vector (max diff ${worst.toExponential(1)})`
    ] = worst < 1e-5;
    results[
        `PQ at ${M} bytes per vector (${(D * 4) / M}x smaller): re-ranking 100 candidates gives recall ${(reranked / (QUERIES * K)).toFixed(3)}`
    ] = reranked / (QUERIES * K) > 0.9 && reranked > raw;
    print(
        `PQ ${M}x${KS}: build ${pqBuildMs.toFixed(0)} ms, ${pqMs.toFixed(3)} ms/query over all ${N.format()} codes; raw recall ${(raw / (QUERIES * K)).toFixed(3)}, re-ranked ${(reranked / (QUERIES * K)).toFixed(3)}`,
    );
}

// =============================================================================
// Live updates: tombstoned deletes, a flat delta segment for inserts, compaction into the lists
// =============================================================================
{
    const deleted = new RoaringBitmap();
    for (const i of [...Array(N).keys()].sample(2000)) deleted.add(i);
    const NEW = 1000,
        Xd = new Float32Array(NEW * D); // the delta segment: searched exhaustively, ids N..N+NEW-1
    for (let j = 0; j < NEW; j++) {
        const row = Xd.subarray(j * D, (j + 1) * D),
            t = rng.nextBounded(TOPICS);
        for (let d = 0; d < D; d++) row[d] = topicCentres[t][d] + (0.6 / Math.sqrt(D)) * rng.normal();
        unit(row);
    }
    const deltaScores = new Float32Array(NEW);
    const vec = (id) => (id < N ? X.subarray(id * D, (id + 1) * D) : Xd.subarray((id - N) * D, (id - N + 1) * D));
    const liveSearch = (q, nprobe) => {
        // IVF over the base (tombstones dropped) merged with the whole delta segment, then top-k of the union
        const base = ivfSearch(q, nprobe, K + 32).ids.filter((id) => !deleted.has(id));
        gemv(deltaScores, Xd, q, NEW, D, 0);
        const fresh = Array.from(topkIndices(deltaScores, K), (j) => N + j);
        return [...base, ...fresh]
            .map((id) => [id, dot(vec(id), q)])
            .sort((a, b) => b[1] - a[1])
            .slice(0, K)
            .map(([id]) => id);
    };
    const delIds = deleted.toArray();
    const exactLive = (q) => {
        // the oracle: every live vector scored (gemv over the base was checked against a scalar scan above)
        gemv(scores, X, q, N, D, 0);
        for (const i of delIds) scores[i] = -2;
        gemv(deltaScores, Xd, q, NEW, D, 0);
        const all = [
            ...Array.from(topkIndices(scores, K), (i) => [i, scores[i]]),
            ...Array.from(topkIndices(deltaScores, K), (j) => [N + j, deltaScores[j]]),
        ];
        return new Set(
            all
                .sort((a, b) => b[1] - a[1])
                .slice(0, K)
                .map(([i]) => i),
        );
    };
    let hits = 0,
        leaked = 0,
        freshHit = 0;
    for (let q = 0; q < 40; q++) {
        const qv = Q.subarray(q * D, (q + 1) * D),
            got = liveSearch(qv, 8),
            want = exactLive(qv);
        hits += got.filter((id) => want.has(id)).length;
        leaked += got.filter((id) => deleted.has(id)).length;
    }
    for (let j = 0; j < 50; j++) if (liveSearch(Xd.subarray(j * D, (j + 1) * D), 4)[0] === N + j) freshHit++;
    results[
        `with 2000 deletes and ${NEW} inserts live: no deleted id returned, recall ${(hits / 400).toFixed(3)}, inserts findable`
    ] = leaked === 0 && hits / 400 > 0.85 && freshHit === 50;
    // compaction: rebuild the inverted lists from base minus tombstones plus the delta, assigned to the same centroids
    const liveIds = [...Array(N + NEW).keys()].filter((i) => !deleted.has(i));
    const sizes = new Int32Array(NLIST),
        listOfLive = new Int32Array(liveIds.length);
    liveIds.forEach((id, k) => {
        gemv(cscore, C, vec(id), NLIST, D, 0);
        sizes[(listOfLive[k] = argmax(cscore))]++;
    });
    const start = new Int32Array(NLIST + 1);
    for (let c = 0; c < NLIST; c++) start[c + 1] = start[c] + sizes[c];
    const XC = new Float32Array(liveIds.length * D),
        idC = new Int32Array(liveIds.length),
        at = start.slice(0, NLIST);
    liveIds.forEach((id, k) => {
        const pos = at[listOfLive[k]]++;
        XC.set(vec(id), pos * D);
        idC[pos] = id;
    });
    let after = 0;
    const out = new Float32Array(liveIds.length);
    for (let q = 0; q < 40; q++) {
        const qv = Q.subarray(q * D, (q + 1) * D),
            want = exactLive(qv);
        gemv(cscore, C, qv, NLIST, D, 0);
        const cands = [];
        for (const c of topkIndices(cscore, 8)) {
            const lo = start[c],
                len = start[c + 1] - lo;
            gemv(out.subarray(0, len), XC.subarray(lo * D, (lo + len) * D), qv, len, D, 0);
            for (let j = 0; j < len; j++) cands.push([idC[lo + j], out[j]]);
        }
        after += cands
            .sort((a, b) => b[1] - a[1])
            .slice(0, K)
            .filter(([id]) => want.has(id)).length;
    }
    results[`compaction folds ${NEW} inserts and drops 2000 tombstones; recall after ${(after / 400).toFixed(3)}`] =
        start[NLIST] === N - 2000 + NEW &&
        new Set(idC).size === idC.length &&
        !idC.some((id) => deleted.has(id)) &&
        after / 400 > 0.85;
    print(
        `live: recall ${(hits / 400).toFixed(3)} with tombstones + delta segment, ${(after / 400).toFixed(3)} after compaction`,
    );
}

// =============================================================================
// Near-duplicate detection
// =============================================================================
{
    const uf = new UnionFind(N);
    let pairs = 0;
    for (let i = 0; i < N; i++) {
        const r = ivfSearch(X.subarray(i * D, (i + 1) * D), 1, 3);
        for (const j of r.ids)
            if (j !== i && distCos(X.subarray(i * D, (i + 1) * D), X.subarray(j * D, (j + 1) * D)) < 0.005) {
                if (uf.union(i, j)) pairs++;
            }
    }
    const found = planted.filter(([a, b]) => uf.connected(a, b)).length;
    results["planted near-duplicates are merged"] = found >= planted.length - 1;
    results["no unrelated vectors are merged"] = pairs <= planted.length + 2;
    print(`dedup: ${pairs} merges, ${found}/${planted.length} planted pairs recovered`);
}

check(results);

function check(results) {
    const names = Object.keys(results);
    const failed = names.filter((name) => !results[name]);
    for (const name of failed) print("FAIL " + name);
    if (failed.length) throw new Error(`${failed.length} of ${names.length} checks failed`);
}
