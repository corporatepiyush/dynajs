// timeout: 900
import { DBScan } from "dyna:ml";

let pass = 0, fail = 0;
const ok = (c, w, d) => { if (c) pass++; else { fail++; print("  FAIL  " + w + (d ? "  [" + d + "]" : "")); } };

const MAX_GRID_COLS = 6;
const gridThreshold = (cols) => (cols <= MAX_GRID_COLS ? 1 << (cols * 2) : Infinity);

function refDbscan(X, n, c, eps, minPts) {
    const eps2 = eps * eps;
    const labels = new Int32Array(n).fill(-1);
    const queued = new Uint8Array(n);
    let cluster = 0;
    const region = (p) => {
        const o = [], px = p * c;
        for (let i = 0; i < n; i++) {
            let s = 0; const ix = i * c;
            for (let d = 0; d < c; d++) { const t = X[ix + d] - X[px + d]; s += t * t; }
            if (s <= eps2) o.push(i);
        }
        return o;
    };
    for (let i = 0; i < n; i++) {
        if (labels[i] !== -1 || queued[i]) continue;
        const nn = region(i);
        if (nn.length < minPts) continue;
        labels[i] = cluster;
        let qn = 0; const queue = [];
        for (const q of nn) { if (q === i || queued[q]) continue; queued[q] = 1; queue[qn++] = q; }
        for (let qi = 0; qi < qn; qi++) {
            const q = queue[qi];
            if (labels[q] === -1) labels[q] = cluster;
            const sn = region(q);
            if (sn.length < minPts) continue;
            for (const s of sn) { if (queued[s] || labels[s] !== -1) continue; queued[s] = 1; queue[qn++] = s; }
        }
        cluster++;
    }
    return { labels, nClusters: cluster };
}

function partition(labels) {
    const noise = [];
    const groups = [];
    const seen = new Map();
    for (let i = 0; i < labels.length; i++) {
        const l = labels[i];
        if (l < 0) { noise.push(i); continue; }
        if (!seen.has(l)) { seen.set(l, groups.length); groups.push([]); }
        groups[seen.get(l)].push(i);
    }
    return { noise, groups };
}
function samePartition(a, b) {
    const pa = partition(a), pb = partition(b);
    if (pa.noise.length !== pb.noise.length) return false;
    for (let i = 0; i < pa.noise.length; i++) if (pa.noise[i] !== pb.noise[i]) return false;
    if (pa.groups.length !== pb.groups.length) return false;
    const key = (g) => g.join(",");
    const sa = pa.groups.map(key).sort(), sb = pb.groups.map(key).sort();
    for (let i = 0; i < sa.length; i++) if (sa[i] !== sb[i]) return false;
    return true;
}

function lcg(seed) { let s = seed >>> 0; return () => { s = (s * 1103515245 + 12345) & 0x7fffffff; return s / 0x7fffffff; }; }
function corpora(kind, n, c, seed) {
    const r = lcg(seed);
    const X = new Float64Array(n * c);
    for (let i = 0; i < n; i++) {
        for (let d = 0; d < c; d++) {
            let v;
            switch (kind) {
                case "random":   v = r() * 10; break;
                case "equal":    v = 3.5; break;
                case "collinear":v = i * 0.25; break;
                case "dup":      v = (i % 4) * 2; break;
                case "blobs":    v = (i % 3) * 10 + r() * 1.0; break;
                case "grid":     v = (i % 7) * 1.5 + Math.floor(i / 7) * 0.01; break;
                case "negzero":  v = (i % 5 === 0) ? 0 : -0; break;
                case "tiny":     v = (r() - 0.5) * 1e-300; break;
                default:         v = r() * 10;
            }
            X[i * c + d] = v;
        }
    }
    return X;
}

function run(X, n, c, eps, mp) {
    const d = new DBScan(eps, mp);
    let lab, nc;
    try { d.fit(X, n, c); lab = d.labels; nc = d.nClusters; }
    finally { d.close(); }
    return { lab, nc };
}

print("== 1. the crash repro: 64 DISTINCT points, 2-D, eps 5, minPts 2 ==");
{
    const N = 64, C = 2;
    const thr = gridThreshold(C);
    ok(N >= thr, "the repro crosses the grid threshold (" + N + " >= 1<<(" + C + "*2) = " + thr + ")",
       N + " < " + thr);
    const X = corpora("random", N, C, 12345);
    const ref = refDbscan(X, N, C, 5.0, 2);
    const got = run(X, N, C, 5.0, 2);
    ok(got.nc === ref.nClusters, "the crash repro agrees with brute force on nClusters",
       "got " + got.nc + " want " + ref.nClusters);
    ok(samePartition(got.lab, ref.labels), "the crash repro agrees with brute force on the partition");
    ok(got.lab.length === N, "one label per point");
}

print("");
print("== 1b. the same defect, caught by ASSERTION instead of by a crash ==");
{
    const ROWS = [[64, 3, 0.25, 2, 25, 0],
                  [16, 2, 0.25, 2, 23, 0],
                  [64, 3, 0.5, 2, 25, 2],
                  [64, 2, 1.0, 2, 9, 14]];
    for (const [rows, c, eps, mp, seed, wantNC] of ROWS) {
        ok(rows >= gridThreshold(c), "cols=" + c + " rows=" + rows + " reaches the grid path");
        const X = corpora("random", rows, c, seed);
        const ref = refDbscan(X, rows, c, eps, mp);
        ok(ref.nClusters === wantNC, "brute force confirms cols=" + c + " rows=" + rows +
           " eps=" + eps + " minPts=" + mp + " finds " + wantNC + " clusters", String(ref.nClusters));
        const got = run(X, rows, c, eps, mp);
        ok(got.nc === wantNC, "cols=" + c + " rows=" + rows + " eps=" + eps + " minPts=" + mp +
           " reports " + wantNC + " clusters", "got " + got.nc);
        ok(samePartition(got.lab, ref.labels), "cols=" + c + " rows=" + rows + " eps=" + eps +
           " minPts=" + mp + " matches the brute-force partition");
    }
    const X = corpora("random", 64, 3, 25);
    const got = run(X, 64, 3, 0.25, 2);
    ok(got.lab.every((l) => l === -1), "an eps smaller than the data spacing leaves ALL noise",
       "nClusters=" + got.nc);
}

print("");
print("== 2. the neighbour COUNT is the semantics, not just the buffer bound ==");
{
    const N = 80, C = 2;
    ok(N >= gridThreshold(C), "count-semantics corpus crosses the grid threshold");
    for (const mp of [2, 3, 4]) {
        const X = corpora("grid", N, C, 777);
        const ref = refDbscan(X, N, C, 0.7, mp);
        const got = run(X, N, C, 0.7, mp);
        ok(got.nc === ref.nClusters, "minPts=" + mp + " core count matches brute force",
           "got " + got.nc + " want " + ref.nClusters);
        ok(samePartition(got.lab, ref.labels), "minPts=" + mp + " partition matches brute force");
    }
}

print("");
print("== 3. SWEEP: cols 1.." + MAX_GRID_COLS + " x rows around 1<<(cols*2) x eps x minPts x shape ==");
{
    let cases = 0, gridCases = 0, linCases = 0, maxBad = 0, badCases = 0;
    const EPS = [1e-9, 0.1, 0.25, 0.5, 1.0, 2.0, 5.0, 20.0, 1e3, 1e9];
    const MP = [1, 2, 3, 8, 64];
    const SHAPES = ["random", "equal", "collinear", "dup", "blobs", "grid", "negzero", "tiny"];
    for (let c = 1; c <= MAX_GRID_COLS; c++) {
        const thr = gridThreshold(c);
        const rowsList = [Math.max(1, thr - 1), thr, thr + 1, 2 * thr, 4 * thr];
        for (const rows of rowsList) {
            if (rows > 900) continue;
            for (const shape of SHAPES) {
                const X = corpora(shape, rows, c, rows * 7 + c);
                for (const eps of EPS) {
                    for (const mp of MP) {
                        if (mp > rows + 1) continue;
                        const isGrid = rows >= thr;
                        const ref = refDbscan(X, rows, c, eps, mp);
                        const got = run(X, rows, c, eps, mp);
                        cases++;
                        if (isGrid) gridCases++; else linCases++;
                        let bad = got.lab.length !== ref.labels.length ? 1 : 0;
                        if (!bad) for (let i = 0; i < rows; i++) if (got.lab[i] !== ref.labels[i]) bad++;
                        if (!samePartition(got.lab, ref.labels)) bad = rows + 1;
                        if (bad) {
                            badCases++;
                            if (bad > maxBad) maxBad = bad;
                            if (badCases <= 20)
                                print("  FAIL  cols=" + c + " rows=" + rows + " eps=" + eps +
                                      " minPts=" + mp + " shape=" + shape + " grid=" + isGrid +
                                      " diff=" + bad + " nC ref=" + ref.nClusters + " got=" + got.nc);
                        }
                    }
                }
            }
        }
    }
    ok(cases > 500, "the sweep actually ran (" + cases + " configurations)", "only " + cases);
    ok(gridCases > 200, "the sweep reached the GRID path (" + gridCases + " configurations)",
       "only " + gridCases);
    ok(linCases > 20, "the sweep also covers the linear path (" + linCases + " configurations)");
    ok(badCases === 0, "every one of the " + cases + " configurations matches brute force",
       badCases + " divergent, worst " + maxBad + " labels");
    print("  (sweep: " + cases + " cases, " + gridCases + " on the grid path, " +
          linCases + " on the linear path)");
}

print("");
print("== 4. boundary rows: just below, AT, and just above 1<<(cols*2) ==");
{
    let checked = 0;
    for (let c = 1; c <= 4; c++) {
        const thr = gridThreshold(c);
        for (const delta of [-1, 0, 1]) {
            const rows = thr + delta;
            if (rows < 1) continue;
            const X = corpora("random", rows, c, 4242 + rows);
            for (const eps of [0.5, 2.0]) {
                const ref = refDbscan(X, rows, c, eps, 2);
                const got = run(X, rows, c, eps, 2);
                ok(got.nc === ref.nClusters, "cols=" + c + " rows=" + rows + " (" +
                   (rows >= thr ? "grid" : "linear") + ") eps=" + eps + " nClusters matches",
                   "got " + got.nc + " want " + ref.nClusters);
                ok(samePartition(got.lab, ref.labels), "cols=" + c + " rows=" + rows +
                   " (" + (rows >= thr ? "grid" : "linear") + ") eps=" + eps + " partition matches");
                checked++;
            }
        }
    }
    ok(checked >= 12, "the boundary rows were exercised (" + checked + " configurations)");
}

print("");
print("== 5. the LINEAR path is pinned: unchanged, bit for bit ==");
{
    const X = [[0, 0], [0, 0.2], [0.2, 0], [0.1, 0.1],
               [5, 5], [5, 5.2], [5.2, 5], [5.1, 5.1],
               [50, 50]];
    ok(!(9 >= gridThreshold(2)), "the pinned corpus is below the grid threshold");
    const d = new DBScan(1.0, 3);
    try {
        d.fit(X);
        ok(d.nClusters === 2, "pinned nClusters is 2", String(d.nClusters));
        ok(d.labels.join(",") === "0,0,0,0,1,1,1,1,-1",
           "pinned labels are unchanged", d.labels.join(","));
    } finally { d.close(); }
    const chain = [];
    for (let i = 0; i < 20; i++) chain.push([i * 0.5, 0]);
    const c2 = new DBScan(0.6, 2);
    try {
        c2.fit(chain);
        ok(c2.nClusters === 1, "pinned chain nClusters is 1", String(c2.nClusters));
        ok(c2.labels.every((l) => l === 0), "pinned chain labels are all 0");
    } finally { c2.close(); }
    const c3 = new DBScan(0.1, 1);
    try {
        c3.fit([[0, 0], [10, 10], [20, 20]]);
        ok(c3.nClusters === 3, "pinned minPts=1 nClusters is 3", String(c3.nClusters));
    } finally { c3.close(); }
}

print("");
print("== 6. degenerate shapes that must not crash or invent clusters ==");
{
    const d1 = new DBScan(0.5, 1);
    try {
        d1.fit([[1, 2]]);
        ok(d1.nClusters === 1, "a single point is one cluster", String(d1.nClusters));
        ok(d1.labels.join(",") === "0", "a single point is labelled 0", d1.labels.join(","));
    } finally { d1.close(); }
    const N = 64, C = 3;
    ok(N >= gridThreshold(C), "the all-equal corpus crosses the grid threshold");
    const eq = corpora("equal", N, C, 9);
    const r1 = run(eq, N, C, 0.1, 2), e1 = refDbscan(eq, N, C, 0.1, 2);
    ok(r1.nc === e1.nClusters, "64 identical points, one cluster", "got " + r1.nc + " want " + e1.nClusters);
    ok(samePartition(r1.lab, e1.labels), "64 identical points agree with brute force");
    const dup = corpora("dup", 64, 2, 11);
    const r2 = run(dup, 64, 2, 1.5, 2), e2 = refDbscan(dup, 64, 2, 1.5, 2);
    ok(r2.nc === e2.nClusters, "duplicated points agree with brute force", "got " + r2.nc + " want " + e2.nClusters);
    ok(samePartition(r2.lab, e2.labels), "duplicated points match the brute-force partition");
    const tiny = corpora("random", 64, 2, 13);
    const r3 = run(tiny, 64, 2, 1e-9, 2), e3 = refDbscan(tiny, 64, 2, 1e-9, 2);
    ok(r3.nc === 0 && r3.lab.every((l) => l === -1), "eps=1e-9 leaves every point as noise",
       "nC=" + r3.nc);
    ok(e3.nClusters === 0, "and brute force agrees that eps=1e-9 finds nothing");
    const big = corpora("random", 64, 2, 17);
    const r4 = run(big, 64, 2, 1e9, 2), e4 = refDbscan(big, 64, 2, 1e9, 2);
    ok(r4.nc === 1, "eps=1e9 puts everything in one cluster", "got " + r4.nc);
    ok(samePartition(r4.lab, e4.labels), "eps=1e9 agrees with brute force");
    const r5 = run(big, 64, 2, 1e9, 1000), e5 = refDbscan(big, 64, 2, 1e9, 1000);
    ok(r5.nc === 0 && r5.lab.every((l) => l === -1), "minPts > rows makes everything noise");
    ok(e5.nClusters === 0, "and brute force agrees");
}

print("");
print("== 7. a refit reuses the grid, so the stamp has to reset per fit ==");
{
    const A = corpora("random", 64, 2, 21);
    const B = corpora("blobs", 128, 3, 22);
    const d = new DBScan(1.0, 3);
    try {
        d.fit(A, 64, 2);
        const first = d.labels.join(",");
        d.fit(A, 64, 2);
        ok(d.labels.join(",") === first, "refitting the same data is idempotent");
        d.fit(B, 128, 3);
        const b1 = d.labels.join(",");
        const ref = refDbscan(B, 128, 3, 1.0, 3);
        ok(d.nClusters === ref.nClusters, "a refit on new data matches brute force",
           "got " + d.nClusters + " want " + ref.nClusters);
        ok(samePartition(d.labels, ref.labels), "a refit on new data matches the brute-force partition");
        d.fit(A, 64, 2);
        ok(d.labels.join(",") === first, "and going BACK to the first data gives the first labels");
        void b1;
    } finally { d.close(); }
}

print("");
print("== 8. labels survive serialize/deserialize unchanged ==");
{
    const N = 96, C = 3;
    ok(N >= gridThreshold(C), "the round-trip corpus crosses the grid threshold");
    const X = corpora("random", N, C, 31);
    const d = new DBScan(1.0, 4);
    let rec, want, wantNC;
    try {
        d.fit(X, N, C);
        want = d.labels.join(",");
        wantNC = d.nClusters;
        rec = d.serialize();
    } finally { d.close(); }
    const m = DBScan.deserialize(rec);
    try {
        ok(m.labels.join(",") === want, "deserialized labels are identical",
           m.labels.slice(0, 20).join(",") + "...");
        ok(m.nClusters === wantNC, "deserialized nClusters is identical");
    } finally { m.close(); }
}

print("");
print("test_ml_dbscan_grid: " + (fail ? "FAILURES: " + fail + " / " : "all ") +
      (pass + fail) + " assertions passed");
if (fail)
    throw new Error("test_ml_dbscan_grid: " + fail + " of " + (pass + fail) + " assertions FAILED");
