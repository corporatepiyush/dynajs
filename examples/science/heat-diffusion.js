// Heat diffusion — explicit and steady-state 2-D heat solvers whose stencils run as whole-grid vector kernels.
//
// WHAT IT IS
//   A plate solver as used for thermal design of a heat spreader:
//
//   transient   explicit FTCS on an N x N grid. One time step is four shifted-view adds, one axpy and one scaled
//               axpy over the flat interior (dyna:simd on Float32Array subarrays); the boundary is re-pinned after
//               each step. A per-cell scalar loop is kept as the reference and both must agree.
//   oracle      the initial field is the lowest Fourier mode sin(pi x) sin(pi y), an exact eigenvector of the
//               discrete Laplacian, so the amplitude after n steps must equal (1 - 8 r sin^2(pi h / 2))^n — a
//               number computed from the scheme's definition, not from the solver
//   stability   the same scheme at r = 0.3 (above the 1/4 bound) must blow up from round-off noise
//   steady      Jacobi iteration for a plate with one hot edge. By superposition of the four rotations the
//               centre temperature is exactly 1/4 of the hot edge; Jacobi must converge to it
//   hot spots   cells above a threshold become a RoaringBitmap; the hot region at two times must be nested
//   multigrid   V-cycles for -Δu = f: weighted-Jacobi smoothing as whole-grid vector kernels, full-weighting
//               restriction, bilinear prolongation, an exact 3 x 3 solve. Oracles from the definition: on a
//               manufactured solution the error falls by 4 per halving of h (second order), every cycle cuts the
//               residual 5x, and the hot-edge plate equals Jacobi's converged field after ~10 cycles vs 2200 sweeps
//   implicit    ADI (Peaceman-Rachford) with a Thomas solve per grid line; for the mode sin(pπx) sin(qπy) a step is
//               exactly (1-a)(1-b)/((1+a)(1+b)), a = rμ_p/2 — checked to 1e-9 for p = q and p ≠ q, at r = 10 where
//               FTCS explodes; the Thomas solver itself is checked against dense Gaussian elimination
//
// MODULES  dyna:simd (add, axpy, scale, normL2, max, sum, dot, threshold) · dyna:structures (RoaringBitmap, TDigest) ·
//          dyna:cli (Table) · Number extensions
//
// RUN      dynajs examples/science/heat-diffusion.js [N]

import { add, axpy, scale, normL2, max, sum, dot, threshold } from "dyna:simd";
import { RoaringBitmap, TDigest } from "dyna:structures";
import { Table } from "dyna:cli";

const results = {};
const N = Number(scriptArgs[1] ?? 129);
const h = 1 / (N - 1);

// =============================================================================
// Grid helpers
// =============================================================================
function modeField(n) {
    const u = new Float32Array(n * n),
        hh = 1 / (n - 1);
    for (let i = 0; i < n; i++)
        for (let j = 0; j < n; j++) u[i * n + j] = Math.sin(Math.PI * i * hh) * Math.sin(Math.PI * j * hh);
    return u;
}
function pinEdges(u, n, top = 0) {
    for (let j = 0; j < n; j++) {
        u[j] = top;
        u[(n - 1) * n + j] = 0;
    }
    for (let i = 1; i < n - 1; i++) {
        u[i * n] = 0;
        u[i * n + n - 1] = 0;
    }
}

// One FTCS step: next = u + r * (up + down + left + right - 4u) over the flat interior rows 1..n-2.
// Left/right neighbours wrap across row ends in the flat view; those cells are edge cells and get re-pinned.
function makeStepper(n) {
    const lo = n,
        len = n * n - 2 * n,
        lap = new Float32Array(len);
    return (u, next, r, top = 0) => {
        const mid = u.subarray(lo, lo + len);
        add(lap, u.subarray(lo - n, lo - n + len), u.subarray(lo + n, lo + n + len));
        add(lap, lap, u.subarray(lo - 1, lo - 1 + len));
        add(lap, lap, u.subarray(lo + 1, lo + 1 + len));
        axpy(lap, -4, mid);
        next.set(u);
        axpy(next.subarray(lo, lo + len), r, lap);
        pinEdges(next, n, top);
    };
}
function scalarStep(u, next, n, r) {
    next.set(u);
    for (let i = 1; i < n - 1; i++)
        for (let j = 1; j < n - 1; j++) {
            const k = i * n + j;
            next[k] = u[k] + r * (u[k - n] + u[k + n] + u[k - 1] + u[k + 1] - 4 * u[k]);
        }
}

// =============================================================================
// Transient: eigenmode decay against the discrete oracle
// =============================================================================
const r = 0.24,
    steps = 400;
const step = makeStepper(N);
let u = modeField(N),
    v = new Float32Array(N * N);
const amp0 = max(u),
    mode = modeField(N),
    modeNorm = dot(mode, mode);
const t0 = performance.now();
for (let s = 0; s < steps; s++) {
    step(u, v, r);
    [u, v] = [v, u];
}
const vecMs = performance.now() - t0;
const lambda = 1 - 8 * r * Math.sin((Math.PI * h) / 2) ** 2;
const projected = dot(u, mode) / modeNorm;
results["mode amplitude follows the discrete decay factor"] = Math.abs(projected / lambda ** steps - 1) < 2e-3;
results["field stays proportional to the mode"] = (() => {
    const resid = new Float32Array(u);
    axpy(resid, -projected, mode);
    return normL2(resid) / normL2(u) < 1e-3;
})();
const alphaT = r * h * h * steps;
results["continuum decay within discretisation error"] =
    Math.abs(projected / Math.exp(-2 * Math.PI ** 2 * alphaT) - 1) < 5e-3;

// Scalar reference on the same grid for a few steps: the vector stencil is the same arithmetic in another order.
{
    const n = 33,
        step33 = makeStepper(n);
    let a = modeField(n),
        b = new Float32Array(n * n),
        c = modeField(n),
        d = new Float32Array(n * n);
    for (let i = 0; i < n * n; i++) {
        const noise = ((i * 2654435761) >>> 0) / 2 ** 32 - 0.5;
        a[i] += 0.01 * noise;
        c[i] = a[i];
    }
    pinEdges(a, n);
    pinEdges(c, n);
    for (let s = 0; s < 50; s++) {
        step33(a, b, 0.2);
        [a, b] = [b, a];
        scalarStep(c, d, n, 0.2);
        [c, d] = [d, c];
    }
    let worst = 0;
    for (let i = 0; i < n * n; i++) worst = Math.max(worst, Math.abs(a[i] - c[i]));
    results["vector stencil matches the scalar loop"] = worst < 1e-5;
    const ts = performance.now();
    let p = modeField(N),
        q = new Float32Array(N * N);
    for (let s = 0; s < 40; s++) {
        scalarStep(p, q, N, r);
        [p, q] = [q, p];
    }
    const scalarMs = ((performance.now() - ts) / 40) * steps;
    print(
        `FTCS ${N}x${N}: ${(vecMs / steps).toFixed(3)} ms/step vector, ${(scalarMs / steps).toFixed(3)} ms/step scalar`,
    );
    results["vector stencil is faster than the per-cell loop"] = vecMs < scalarMs;
}

// =============================================================================
// Stability: r above 1/4 amplifies the checkerboard mode
// =============================================================================
{
    const n = 65,
        unstable = makeStepper(n);
    let a = new Float32Array(n * n),
        b = new Float32Array(n * n);
    for (let i = 0; i < a.length; i++) a[i] = 1e-6 * ((((i * 40503) >>> 0) % 1000) / 1000 - 0.5);
    pinEdges(a, n);
    const start = normL2(a);
    for (let s = 0; s < 200; s++) {
        unstable(a, b, 0.3);
        [a, b] = [b, a];
    }
    results["r = 0.3 is unstable"] = !(normL2(a) < start * 1e6);
    let c = new Float32Array(n * n);
    for (let i = 0; i < c.length; i++) c[i] = 1e-6 * ((((i * 40503) >>> 0) % 1000) / 1000 - 0.5);
    pinEdges(c, n);
    const s0 = normL2(c);
    for (let s = 0; s < 200; s++) {
        unstable(c, b, 0.24);
        [c, b] = [b, c];
    }
    results["r = 0.24 damps the same noise"] = normL2(c) < s0;
}

// =============================================================================
// Hot-spot tracking: a point source spreads; hot regions at later times are nested
// =============================================================================
{
    const n = 97,
        stepper = makeStepper(n);
    let a = new Float32Array(n * n),
        b = new Float32Array(n * n);
    a[(n >> 1) * n + (n >> 1)] = 1000;
    const hot = [];
    const spread = new TDigest(100);
    for (let s = 1; s <= 120; s++) {
        stepper(a, b, 0.24);
        [a, b] = [b, a];
        if (s === 40 || s === 120) {
            const mask = threshold(new Float32Array(a), 0.05);
            const cells = new RoaringBitmap();
            for (let i = 0; i < mask.length; i++) if (mask[i] > 0) cells.add(i);
            hot.push(cells);
        }
    }
    for (let i = 0; i < a.length; i++) if (a[i] > 1e-4) spread.add(a[i]);
    results["heat is conserved away from the edges"] = Math.abs(sum(a) - 1000) < 0.5;
    results["early hot region is inside the later one"] =
        hot[0].isSubsetOf(hot[1]) && hot[1].toArray().length > hot[0].toArray().length;
    results["hot region stays symmetric"] = [...hot[1]].every((k) =>
        hot[1].has((n - 1 - Math.floor(k / n)) * n + (k % n)),
    );
    print(
        `point source after 120 steps: ${hot[1].toArray().length} cells above 0.05, median warm cell ${spread.quantile(0.5).toFixed(4)}`,
    );
}

// =============================================================================
// Steady state: Jacobi for a plate with one hot edge
// =============================================================================
let jacobiField, jacobiIters;
{
    const n = 33,
        len = n * n - 2 * n,
        lo = n;
    let a = new Float32Array(n * n),
        b = new Float32Array(n * n);
    pinEdges(a, n, 1);
    pinEdges(b, n, 1);
    const acc = new Float32Array(len),
        diff = new Float32Array(len);
    let iters = 0,
        residual = 1;
    while (residual > 1e-6 && iters < 20000) {
        add(acc, a.subarray(lo - n, lo - n + len), a.subarray(lo + n, lo + n + len));
        add(acc, acc, a.subarray(lo - 1, lo - 1 + len));
        add(acc, acc, a.subarray(lo + 1, lo + 1 + len));
        scale(acc, 0.25);
        b.subarray(lo, lo + len).set(acc);
        pinEdges(b, n, 1);
        if (++iters % 50 === 0) {
            diff.set(b.subarray(lo, lo + len));
            axpy(diff, -1, a.subarray(lo, lo + len));
            residual = normL2(diff);
        }
        [a, b] = [b, a];
    }
    const centre = a[(n >> 1) * n + (n >> 1)];
    jacobiField = a;
    jacobiIters = iters;
    results["Jacobi converges"] = residual <= 1e-6;
    results["centre of a one-hot-edge plate is 1/4"] = Math.abs(centre - 0.25) < 2e-3;
    results["temperature falls away from the hot edge"] = (() => {
        const c = n >> 1;
        for (let i = 1; i < n - 2; i++) if (!(a[i * n + c] > a[(i + 1) * n + c])) return false;
        return true;
    })();
    print(
        Table(
            [
                ["iterations", iters.format()],
                ["centre", centre.toFixed(5)],
                ["residual", residual.toExponential(2)],
            ],
            { head: ["Jacobi", "value"] },
        ),
    );
}

// =============================================================================
// Multigrid: V-cycles for -Δu = f, every smoothing sweep a whole-grid vector kernel
// =============================================================================
// Weighted Jacobi (ω = 4/5, two sweeps before and after) smooths, full weighting restricts the residual, bilinear
// interpolation carries the coarse correction back, and the 3 x 3 grid is solved exactly. Grids are 2^k + 1 points.
const OMEGA = 0.8;
function vecLaplaceSum(acc, u, n) {
    // acc = up + down + left + right over the flat interior; the two edge columns hold garbage and are restored
    const lo = n,
        len = n * n - 2 * n;
    add(acc, u.subarray(lo - n, lo - n + len), u.subarray(lo + n, lo + n + len));
    add(acc, acc, u.subarray(lo - 1, lo - 1 + len));
    add(acc, acc, u.subarray(lo + 1, lo + 1 + len));
}
const scratch = new Map();
const buf = (n, tag) => scratch.getOrInsertComputed(`${n}:${tag}`, () => new Float32Array(n * n - 2 * n));
function edgeColumns(u, n) {
    const out = new Float32Array(2 * n);
    for (let i = 0; i < n; i++) ((out[2 * i] = u[i * n]), (out[2 * i + 1] = u[i * n + n - 1]));
    return out;
}
function putEdgeColumns(u, n, cols) {
    for (let i = 0; i < n; i++) ((u[i * n] = cols[2 * i]), (u[i * n + n - 1] = cols[2 * i + 1]));
}
function smooth(u, f, n, sweeps) {
    const lo = n,
        len = n * n - 2 * n,
        h2 = 1 / (n - 1) ** 2,
        acc = buf(n, "acc"),
        cols = edgeColumns(u, n);
    for (let s = 0; s < sweeps; s++) {
        const mid = u.subarray(lo, lo + len);
        vecLaplaceSum(acc, u, n);
        axpy(acc, h2, f.subarray(lo, lo + len));
        scale(acc, OMEGA / 4);
        axpy(acc, 1 - OMEGA, mid); // ω · (neighbours + h²f) / 4 + (1 - ω) u
        mid.set(acc);
        putEdgeColumns(u, n, cols);
    }
}
function residual(u, f, n) {
    const lo = n,
        len = n * n - 2 * n,
        r = new Float32Array(n * n),
        acc = buf(n, "res");
    vecLaplaceSum(acc, u, n);
    axpy(acc, -4, u.subarray(lo, lo + len));
    scale(acc, (n - 1) ** 2);
    add(acc, acc, f.subarray(lo, lo + len)); // f + Δ_h u
    r.subarray(lo, lo + len).set(acc);
    for (let i = 0; i < n; i++) r[i * n] = r[i * n + n - 1] = 0;
    for (let j = 0; j < n; j++) r[j] = r[(n - 1) * n + j] = 0;
    return r;
}
function restrictFW(r, n) {
    const nc = (n + 1) / 2,
        rc = new Float32Array(nc * nc);
    for (let I = 1; I < nc - 1; I++)
        for (let J = 1; J < nc - 1; J++) {
            const k = 2 * I * n + 2 * J;
            rc[I * nc + J] =
                (4 * r[k] +
                    2 * (r[k - 1] + r[k + 1] + r[k - n] + r[k + n]) +
                    (r[k - n - 1] + r[k - n + 1] + r[k + n - 1] + r[k + n + 1])) /
                16;
        }
    return rc;
}
function prolongAdd(u, n, ec) {
    const nc = (n + 1) / 2;
    for (let i = 1; i < n - 1; i++)
        for (let j = 1; j < n - 1; j++) {
            const I = i >> 1,
                J = j >> 1,
                di = i & 1,
                dj = j & 1;
            const e = (a, b) => ec[(I + a) * nc + J + b];
            u[i * n + j] += di
                ? dj
                    ? (e(0, 0) + e(1, 0) + e(0, 1) + e(1, 1)) / 4
                    : (e(0, 0) + e(1, 0)) / 2
                : dj
                  ? (e(0, 0) + e(0, 1)) / 2
                  : e(0, 0);
        }
}
let vcycles = 0;
function vcycle(u, f, n) {
    vcycles++;
    if (n === 3) {
        u[4] = (u[1] + u[3] + u[5] + u[7] + f[4] / 4) / 4;
        return;
    }
    smooth(u, f, n, 2);
    const nc = (n + 1) / 2,
        ec = new Float32Array(nc * nc);
    vcycle(ec, restrictFW(residual(u, f, n), n), nc);
    prolongAdd(u, n, ec);
    smooth(u, f, n, 2);
}
// Stops at the tolerance or when a cycle no longer halves the residual: in single precision the residual (scaled by
// 1/h²) has a floor near ε·4/h² of the solution, ~1e-4 relative on 129 x 129, which no cycle can go below.
function mgSolve(u, f, n, tol, maxCycles = 30) {
    const norms = [normL2(residual(u, f, n))];
    while (
        norms.at(-1) > tol * norms[0] &&
        norms.length <= maxCycles &&
        !(norms.length > 2 && norms.at(-1) > 0.5 * norms.at(-2))
    ) {
        vcycle(u, f, n);
        norms.push(normL2(residual(u, f, n)));
    }
    return norms;
}
{
    // the coarsest grid has one unknown and is solved exactly: one visit leaves no residual (a wrong coarse solve
    // would only slow the cycles down, never change their fixed point, so it is checked here directly)
    const u3 = Float32Array.from([0, 0.5, 0, 0.25, 0, 0.75, 0, 1, 0]),
        f3 = Float32Array.from([0, 0, 0, 0, 7, 0, 0, 0, 0]);
    vcycle(u3, f3, 3);
    results["one V-cycle on the 3 x 3 grid is exact"] = Math.abs(residual(u3, f3, 3)[4]) < 1e-5;
    // manufactured solution u = sin(πx) sin(2πy), so f = 5π² u: the discrete error must fall as h², a ratio of 4
    const errors = [],
        contraction = [];
    for (const n of [33, 65, 129]) {
        const hh = 1 / (n - 1),
            exact = new Float32Array(n * n),
            f = new Float32Array(n * n);
        for (let i = 0; i < n; i++)
            for (let j = 0; j < n; j++) {
                exact[i * n + j] = Math.sin(Math.PI * i * hh) * Math.sin(2 * Math.PI * j * hh);
                f[i * n + j] = 5 * Math.PI ** 2 * exact[i * n + j];
            }
        const u = new Float32Array(n * n),
            norms = mgSolve(u, f, n, 1e-5);
        let err = 0;
        for (let k = 0; k < n * n; k++) err = Math.max(err, Math.abs(u[k] - exact[k]));
        errors.push(err);
        contraction.push(Math.max(...norms.slice(1, 4).map((x, i) => x / norms[i])));
    }
    const ratios = [errors[0] / errors[1], errors[1] / errors[2]];
    results[`multigrid is second-order accurate (error ratios ${ratios.map((x) => x.toFixed(2)).join(", ")})`] =
        ratios.every((x) => x > 3.7 && x < 4.3);
    results[`each V-cycle cuts the residual at least 5x on every grid (worst ${Math.max(...contraction).toFixed(3)})`] =
        contraction.every((c) => c < 0.2);
    // the one-hot-edge plate again: multigrid must reach the field Jacobi converged to, in a handful of cycles
    const n = 33,
        u = new Float32Array(n * n),
        f = new Float32Array(n * n);
    pinEdges(u, n, 1);
    const before = vcycles,
        norms = mgSolve(u, f, n, 1e-6);
    let worst = 0;
    for (let k = 0; k < n * n; k++) worst = Math.max(worst, Math.abs(u[k] - jacobiField[k]));
    results[
        `multigrid matches Jacobi's plate in ${norms.length - 1} V-cycles vs ${jacobiIters} sweeps (max diff ${worst.toExponential(1)})`
    ] = worst < 2e-5 && norms.length - 1 < 15;
    print(
        `multigrid: errors ${errors.map((e) => e.toExponential(2)).join(" / ")} on 33/65/129, ${vcycles - before} grid visits for the plate`,
    );
}

// =============================================================================
// Implicit stepping: ADI (Peaceman-Rachford) with a tridiagonal solve per line
// =============================================================================
// Half a step implicit in x and explicit in y, then the reverse. For the mode sin(pπx) sin(qπy) each half step
// multiplies by (1 - r/2 μ_y)/(1 + r/2 μ_x) with μ = 4 sin²(kπh/2), so a full step is exactly
// ((1 - r μ/2)/(1 + r μ/2))² for p = q — below 1 for EVERY r, which is why r = 10 is stable here.
function thomas(sub, diag, sup, rhs, x, c = new Float64Array(diag.length), d = new Float64Array(diag.length)) {
    const m = diag.length;
    c[0] = sup[0] / diag[0];
    d[0] = rhs[0] / diag[0];
    for (let i = 1; i < m; i++) {
        const den = diag[i] - sub[i] * c[i - 1];
        c[i] = sup[i] / den;
        d[i] = (rhs[i] - sub[i] * d[i - 1]) / den;
    }
    x[m - 1] = d[m - 1];
    for (let i = m - 2; i >= 0; i--) x[i] = d[i] - c[i] * x[i + 1];
    return x;
}
function makeAdi(n, r) {
    const m = n - 2,
        sub = new Float64Array(m).fill(-r / 2),
        diag = new Float64Array(m).fill(1 + r),
        sup = new Float64Array(m).fill(-r / 2),
        rhs = new Float64Array(m),
        x = new Float64Array(m),
        c = new Float64Array(m),
        d = new Float64Array(m),
        mid = new Float64Array(n * n);
    return (u) => {
        for (let i = 1; i < n - 1; i++) {
            for (let j = 1; j < n - 1; j++) {
                const k = i * n + j;
                rhs[j - 1] = u[k] + (r / 2) * (u[k - n] - 2 * u[k] + u[k + n]);
            }
            thomas(sub, diag, sup, rhs, x, c, d);
            for (let j = 1; j < n - 1; j++) mid[i * n + j] = x[j - 1];
        }
        for (let j = 1; j < n - 1; j++) {
            for (let i = 1; i < n - 1; i++) {
                const k = i * n + j;
                rhs[i - 1] = mid[k] + (r / 2) * (mid[k - 1] - 2 * mid[k] + mid[k + 1]);
            }
            thomas(sub, diag, sup, rhs, x, c, d);
            for (let i = 1; i < n - 1; i++) u[i * n + j] = x[i - 1];
        }
    };
}
{
    // Thomas against dense Gaussian elimination with partial pivoting on random diagonally dominant systems
    let worst = 0;
    for (let t = 0; t < 20; t++) {
        const m = 3 + t,
            rnd = (k) => (((k * 2654435761 + t * 40503) >>> 0) % 1000) / 1000 - 0.5;
        const sub = Float64Array.from({ length: m }, (_, i) => (i ? rnd(i) : 0)),
            sup = Float64Array.from({ length: m }, (_, i) => (i < m - 1 ? rnd(i + 97) : 0)),
            diag = Float64Array.from({ length: m }, (_, i) => 2 + rnd(i + 31)),
            rhs = Float64Array.from({ length: m }, (_, i) => rnd(i + 57));
        const A = Array.from({ length: m }, (_, i) =>
            Array.from({ length: m + 1 }, (_, j) =>
                j === m ? rhs[i] : j === i ? diag[i] : j === i - 1 ? sub[i] : j === i + 1 ? sup[i] : 0,
            ),
        );
        for (let col = 0; col < m; col++) {
            let piv = col;
            for (let i = col + 1; i < m; i++) if (Math.abs(A[i][col]) > Math.abs(A[piv][col])) piv = i;
            [A[col], A[piv]] = [A[piv], A[col]];
            for (let i = col + 1; i < m; i++) {
                const k = A[i][col] / A[col][col];
                for (let j = col; j <= m; j++) A[i][j] -= k * A[col][j];
            }
        }
        const dense = new Float64Array(m);
        for (let i = m - 1; i >= 0; i--) {
            let acc = A[i][m];
            for (let j = i + 1; j < m; j++) acc -= A[i][j] * dense[j];
            dense[i] = acc / A[i][i];
        }
        const x = thomas(sub, diag, sup, rhs, new Float64Array(m));
        for (let i = 0; i < m; i++) worst = Math.max(worst, Math.abs(x[i] - dense[i]));
    }
    results["the Thomas solver equals dense elimination"] = worst < 1e-12;
    const n = 33,
        hh = 1 / (n - 1),
        modeOf = (p, q) =>
            Float64Array.from(
                { length: n * n },
                (_, k) => Math.sin(p * Math.PI * Math.floor(k / n) * hh) * Math.sin(q * Math.PI * (k % n) * hh),
            );
    const project = (u, m) => {
        let a = 0,
            b = 0;
        for (let k = 0; k < u.length; k++) ((a += u[k] * m[k]), (b += m[k] * m[k]));
        return a / b;
    };
    const rows = [];
    let exact = true;
    // p ≠ q rows separate the two directions: with a = rμ_p/2, b = rμ_q/2 a step is (1-a)(1-b)/((1+a)(1+b))
    for (const [rr, p, q] of [
        [2, 1, 1],
        [10, 1, 1],
        [0.5, 5, 5],
        [10, n - 2, n - 2],
        [1, 1, 6],
        [0.3, 9, 2],
    ]) {
        const mode = modeOf(p, q),
            u = Float64Array.from(mode),
            adi = makeAdi(n, rr),
            a = (rr / 2) * 4 * Math.sin((p * Math.PI * hh) / 2) ** 2,
            b = (rr / 2) * 4 * Math.sin((q * Math.PI * hh) / 2) ** 2,
            g = ((1 - a) * (1 - b)) / ((1 + a) * (1 + b));
        for (let s = 0; s < 20; s++) adi(u);
        const amp = project(u, mode),
            want = g ** 20;
        if (!(Math.abs(amp - want) <= 1e-9 * Math.abs(want))) exact = false;
        rows.push([`r=${rr} p=${p} q=${q}`, want.toExponential(4), amp.toExponential(4)]);
    }
    results["ADI multiplies each mode by exactly (1-a)(1-b)/((1+a)(1+b)) per step"] = exact;
    // the same r = 2 that FTCS cannot take: noise is damped by ADI and amplified by the explicit scheme
    const noise = Float32Array.from({ length: n * n }, (_, k) => 1e-3 * ((((k * 40503) >>> 0) % 1000) / 1000 - 0.5));
    pinEdges(noise, n);
    const implicitU = Float64Array.from(noise),
        adi = makeAdi(n, 2);
    let explicitU = Float32Array.from(noise),
        tmp = new Float32Array(n * n);
    const ftcs = makeStepper(n);
    for (let s = 0; s < 20; s++) {
        adi(implicitU);
        ftcs(explicitU, tmp, 2);
        [explicitU, tmp] = [tmp, explicitU];
    }
    const norm64 = (a) => Math.sqrt(a.reduce((s, x) => s + x * x, 0));
    results["at r = 2 ADI damps noise that FTCS amplifies"] =
        norm64(implicitU) < norm64(noise) && !(normL2(explicitU) < 1e3 * normL2(noise));
    print(Table(rows, { head: ["ADI 20 steps", "predicted", "measured"] }));
}

check(results);

function check(results) {
    const names = Object.keys(results);
    const failed = names.filter((name) => !results[name]);
    for (const name of failed) print("FAIL " + name);
    if (failed.length) throw new Error(`${failed.length} of ${names.length} checks failed`);
}
