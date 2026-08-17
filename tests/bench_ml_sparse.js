/* bench_ml_sparse.js -- what CSR is worth, and where it is a tax
 * (the sparse-matrix gap).
 *
 * The sparse normal equations run over nonzero PAIRS: O(sum nnz_i^2) against
 * O(rows * cols^2). So the win grows as the matrix gets wider at fixed
 * nonzeros per row, and vanishes -- then reverses -- as it fills up, because
 * an index lookup per element is more work than a contiguous load.
 *
 * `#B <case> <dense_ms> <sparse_ms> <ratio>` (ratio < 1 = sparse faster) and
 * `#M <case> <dense_bytes> <sparse_bytes>` for the memory the two forms cost.
 */
import { CSR, LinearRegression, LogisticRegression } from "dyna:ml";
;

function lcg(seed) { let s = seed >>> 0; return () => (s = (s * 1664525 + 1013904223) >>> 0) / 4294967296; }

function data(rows, cols, nz, seed) {
    const rnd = lcg(seed), dense = [], y = [];
    for (let i = 0; i < rows; i++) {
        const r = new Array(cols).fill(0);
        for (let k = 0; k < nz; k++) r[Math.floor(rnd() * cols)] = rnd() * 2 - 1;
        dense.push(r);
        y.push(3 * r[0] - 2 * r[1] + rnd() * 0.05);
    }
    return { dense, y };
}

function time(fn, reps) {
    let best = Infinity;
    for (let run = 0; run < 3; run++) {
        const t0 = performance.now();
        for (let r = 0; r < reps; r++) fn();
        const dt = performance.now() - t0;
        if (dt < best) best = dt;
    }
    return best;
}

function row(name, rows, cols, nz, reps, logistic) {
    const d = data(rows, cols, nz, 7 + cols);
    const S = CSR.fromDense(d.dense);
    /* Built OUTSIDE the timed region: mapping y inside it would time the map. */
    const target = logistic ? d.y.map(v => (v > 0 ? 1 : 0)) : d.y;
    const mk = logistic ? (() => new LogisticRegression({ maxIter: 40 }))
                        : (() => new LinearRegression());
    const a = time(() => mk().fit(d.dense, target), reps);
    const b = time(() => mk().fit(S, target), reps);
    print("#B " + name + " " + a.toFixed(3) + " " + b.toFixed(3) + " " + (b / a).toFixed(3) +
          "  density=" + S.density.toFixed(4));
}

/* A matrix with NO zeros at all, which is where the index lookup per element
 * has nothing to skip and must lose. */
function denseRow(name, rows, cols, reps) {
    const rnd = lcg(5), dense = [], y = [];
    for (let i = 0; i < rows; i++) {
        const r = [];
        for (let j = 0; j < cols; j++) r.push(rnd() * 2 - 1);
        dense.push(r);
        y.push(3 * r[0] - 2 * r[1]);
    }
    const S = CSR.fromDense(dense);
    const a = time(() => new LinearRegression().fit(dense, y), reps);
    const b = time(() => new LinearRegression().fit(S, y), reps);
    print("#B " + name + " " + a.toFixed(3) + " " + b.toFixed(3) + " " + (b / a).toFixed(3) +
          "  density=" + S.density.toFixed(4));
}

/* NOTE on the wide rows: both paths share an O(p^3) solve of the p x p normal
 * equations (p = cols + 1), so past a few hundred columns the SOLVE dominates
 * and caps the ratio however sparse the accumulation is. The 60-column rows
 * show what the accumulation itself is worth. */
print("# LinearRegression / LogisticRegression, dense X vs CSR");
row("linreg_400x60_nz4", 400, 60, 4, 20, false);
row("linreg_400x600_nz4", 400, 600, 4, 3, false);
row("linreg_2000x2000_nz6", 2000, 2000, 6, 1, false);
row("logreg_400x600_nz4", 400, 600, 4, 3, true);
/* ---- the adversarial half: a matrix that is not sparse ---- */
row("linreg_400x60_nz60_63pct", 400, 60, 60, 20, false);
denseRow("linreg_400x60_FULL", 400, 60, 20);
denseRow("linreg_2000x100_FULL", 2000, 100, 3);

/* ---- memory: the reason the class exists ----
 *
 * Reported from the SHAPE rather than from the allocator, because the CSR's
 * buffers are malloc'd outside the JS heap and memoryUsage() cannot see them:
 * an allocator delta of 184 bytes for a 32 MB matrix is an accounting artifact,
 * not a measurement. Dense costs rows*cols doubles; a CSR costs nnz values,
 * nnz uint32 indices and rows+1 offsets. */
{
    for (const [rows, cols, nz] of [[2000, 2000, 6], [500, 5000, 4], [100000, 50000, 20]]) {
        const denseBytes = rows * cols * 8;
        const nnz = rows * nz;
        const csrBytes = nnz * 8 + nnz * 4 + (rows + 1) * 8;
        print("#M " + rows + "x" + cols + "_nz" + nz + " " + denseBytes + " " +
              csrBytes + "  (" + (denseBytes / csrBytes).toFixed(0) + "x smaller)");
    }
}
