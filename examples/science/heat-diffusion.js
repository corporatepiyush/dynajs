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
//
// MODULES  dyna:simd (add, axpy, scale, normL2, max, sum, dot) · dyna:structures (RoaringBitmap, TDigest) ·
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
{
    const n = 41,
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

check(results);

function check(results) {
    const names = Object.keys(results);
    const failed = names.filter((name) => !results[name]);
    for (const name of failed) print("FAIL " + name);
    if (failed.length) throw new Error(`${failed.length} of ${names.length} checks failed`);
}
