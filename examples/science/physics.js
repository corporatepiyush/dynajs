// Physics — a Lennard-Jones molecular dynamics engine (velocity Verlet, periodic boundaries, cell lists, a
// thermostat) with the measurements statistical mechanics predicts, and a heat-equation solver comparing explicit
// and Crank–Nicolson schemes against the analytic solution, plus symplectic vs non-symplectic orbit integration.
//
// WHAT IT IS
//   MD           500 argon-like atoms in reduced units in a periodic box: forces from a truncated and shifted
//                Lennard-Jones potential over a Verlet neighbour list (pairs within rc + skin, rebuilt every 10
//                steps, cross-checked against brute force), integrated with velocity Verlet; equilibrated with a Berendsen thermostat, then
//                run in NVE where total energy must be conserved
//   observables  temperature from kinetic energy, pressure from the virial, the radial distribution function g(r)
//                (first peak near 2^(1/6)σ, tends to 1 at large r), the speed distribution tested against
//                Maxwell–Boltzmann with a Kolmogorov–Smirnov statistic, and the self-diffusion coefficient from the
//                mean-squared displacement (Einstein relation, unwrapped coordinates), cross-checked against the
//                Green–Kubo integral of the velocity autocorrelation (an independent route to the same coefficient)
//   vectorised   atoms are stored as one Float32Array per component; the force step is ~30 dyna:simd calls over the
//                whole pair list (gather, sub, fma, a threshold mask for the cutoff, vinv, scatterAdd of ±f·dx), and
//                the g(r) histogram images, bins and scatter-adds one atom's row at a time; both are diffed against
//                plain double-precision pair loops
//   heat         1-D diffusion u_t = α u_xx on a rod with fixed ends: FTCS (explicit) is stable only for
//                r = αΔt/Δx² ≤ ½ and blows up beyond it; Crank–Nicolson (tridiagonal Thomas solver) is stable at
//                any r; both are compared with the Fourier-series solution and the convergence order is measured
//   orbits       a Kepler orbit integrated for 1000 periods with leapfrog (symplectic: bounded energy error) and RK4
//                (energy drifts) — the reason simulations of planets use symplectic integrators
//
//   neighbours   the Verlet list is built with a KDTree over the atoms plus their periodic "ghost" images near the
//                faces: one native radius query per atom instead of all N²/2 pair distances. Each pair stores the
//                image shift it was found through, and positions are only wrapped back into the box when the list
//                is rebuilt — so the force loop needs no minimum-image rounding at all
//
// MODULES  dyna:simd (gather, scatterAdd, fma, vinv, threshold, axpy, dot, ...) · dyna:mathx (stats, erf) · dyna:dataframe (HISTOGRAM-free binning)
//          · dyna:random · dyna:cli (Table) · dyna:csv · dyna:file · dyna:structures (KDTree)
//
// RUN      dynajs examples/science/physics.js     runs the MD simulation, the PDE study and the orbit comparison

import {
    axpy,
    scale,
    dot,
    sum,
    addScalar,
    gather,
    sub,
    mul,
    fma,
    affine,
    threshold,
    vinv,
    scatterAdd,
    distL2,
    vsqrt,
    clamp,
} from "dyna:simd";
import { stats, erf } from "dyna:mathx";
import { DataFrame } from "dyna:dataframe";
import { Random } from "dyna:random";
import { Table } from "dyna:cli";
import * as csv from "dyna:csv";
import { makeTempDir, writeFile, removeAll } from "dyna:file";
import { KDTree } from "dyna:structures";
const table = (rows) => Table(rows.slice(1), { head: rows[0] });

const work = makeTempDir("physics");
const rng = new Random(1964);

// =============================================================================
// Molecular dynamics
// =============================================================================
const N = 500,
    RHO = 0.8,
    T_TARGET = 1.2,
    RC = 2.5,
    SKIN = 0.3,
    DT = 0.004;
const L = Math.cbrt(N / RHO),
    HALF = L / 2;
const ECUT = 4 * (RC ** -12 - RC ** -6);
// structure of arrays: one Float32Array per component, so every per-atom and per-pair step is a dyna:simd kernel
const xyz = () => [new Float32Array(N), new Float32Array(N), new Float32Array(N)];
const P = xyz(),
    V = xyz(),
    F = xyz(),
    U = xyz(); // positions (wrapped at rebuilds), velocities, forces, unwrapped positions
{
    // start on a simple cubic lattice
    const side = Math.ceil(Math.cbrt(N)),
        a = L / side;
    let k = 0;
    for (let x = 0; x < side && k < N; x++)
        for (let y = 0; y < side && k < N; y++)
            for (let z = 0; z < side && k < N; z++, k++) [x, y, z].forEach((c, d) => (P[d][k] = (c + 0.5) * a));
    for (let d = 0; d < 3; d++) {
        rng.fillNormal(V[d], 0, Math.sqrt(T_TARGET));
        addScalar(V[d], -sum(V[d]) / N); // zero total momentum
        U[d].set(P[d]);
    }
}
const minImage = (d) => d - L * Math.round(d / L);

// Pair buffers sized at each rebuild: I, J, the image shift of each pair, and scratch for the kernels.
let I = new Int32Array(0),
    J = new Int32Array(0),
    S = [],
    T = {},
    sinceBuild = 0,
    rebuilds = 0;
function computeForces() {
    // dx = x[I] - x[J] - shift; r2; mask = r2 < rc²; f = 24 r⁻² r⁻⁶ (2 r⁻⁶ - 1) on masked pairs; scatter ±f·dx
    const { xi, xj, d, r2, m, ir2, ir6, w, fd } = T;
    r2.fill(0);
    for (let c = 0; c < 3; c++) {
        gather(xi, P[c], I);
        gather(xj, P[c], J);
        sub(d[c], xi, xj);
        sub(d[c], d[c], S[c]);
        fma(r2, d[c], d[c]);
    }
    m.set(r2);
    affine(threshold(m, RC * RC), -1, 1); // 1 inside the cutoff, 0 outside
    ir2.set(r2);
    vinv(ir2);
    mul(ir6, ir2, ir2);
    mul(ir6, ir6, ir2);
    w.set(ir6);
    affine(w, 2, -1);
    mul(w, w, ir6);
    mul(w, w, ir2);
    mul(w, w, m);
    scale(w, 24); // w = f / r per pair, zero outside rc
    const virial = dot(w, r2);
    xi.set(ir6);
    affine(xi, 1, -1);
    mul(xi, xi, ir6); // r⁻¹² - r⁻⁶
    const pairs = sum(m),
        pot = 4 * dot(xi, m) - ECUT * pairs;
    for (let c = 0; c < 3; c++) {
        F[c].fill(0);
        mul(fd, w, d[c]);
        scatterAdd(F[c], I, fd);
        scale(fd, -1);
        scatterAdd(F[c], J, fd);
    }
    return { pot, virial, pairs: Math.round(pairs) };
}
// The oracle: every pair through the minimum image, one at a time, in double precision.
function bruteForces() {
    const out = [new Float64Array(N), new Float64Array(N), new Float64Array(N)];
    let pot = 0,
        virial = 0,
        pairs = 0;
    for (let i = 0; i < N; i++)
        for (let j = i + 1; j < N; j++) {
            const dx = minImage(P[0][i] - P[0][j]),
                dy = minImage(P[1][i] - P[1][j]),
                dz = minImage(P[2][i] - P[2][j]);
            const r2 = dx * dx + dy * dy + dz * dz;
            if (r2 >= RC * RC) continue;
            pairs++;
            const ir6 = 1 / r2 ** 3,
                f = (24 * ir6 * (2 * ir6 - 1)) / r2;
            [dx, dy, dz].forEach((dc, c) => {
                out[c][i] += f * dc;
                out[c][j] -= f * dc;
            });
            pot += 4 * ir6 * (ir6 - 1) - ECUT;
            virial += f * r2;
        }
    return { out, pot, virial, pairs };
}
function buildNeighbours() {
    // Verlet list: every pair within rc + skin
    for (const p of P) for (let i = 0; i < N; i++) p[i] -= L * Math.floor(p[i] / L); // wrap into the box only here
    const R = RC + SKIN,
        pts = [],
        owner = [],
        shifts = [];
    for (let i = 0; i < N; i++) {
        const p = [P[0][i], P[1][i], P[2][i]];
        const opts = p.map((v) => (v < R ? [0, L] : v > L - R ? [0, -L] : [0])); // ghost images near the faces
        for (const sx of opts[0])
            for (const sy of opts[1])
                for (const sz of opts[2]) {
                    pts.push(p[0] + sx, p[1] + sy, p[2] + sz);
                    owner.push(i);
                    shifts.push(sx, sy, sz);
                }
    }
    const tree = new KDTree(Float64Array.from(pts), 3),
        is = [],
        js = [],
        sh = [[], [], []];
    for (let i = 0; i < N; i++)
        for (const g of tree.radius([P[0][i], P[1][i], P[2][i]], R).indices) {
            const j = owner[g];
            if (j <= i) continue;
            is.push(i);
            js.push(j);
            for (let c = 0; c < 3; c++) sh[c].push(shifts[3 * g + c]);
        }
    I = Int32Array.from(is);
    J = Int32Array.from(js);
    S = sh.map((s) => Float32Array.from(s));
    const n = I.length,
        f32 = () => new Float32Array(n);
    T = {
        xi: f32(),
        xj: f32(),
        d: [f32(), f32(), f32()],
        r2: f32(),
        m: f32(),
        ir2: f32(),
        ir6: f32(),
        w: f32(),
        fd: f32(),
    };
    sinceBuild = 0;
    rebuilds++;
}
const kinetic = () => 0.5 * (dot(V[0], V[0]) + dot(V[1], V[1]) + dot(V[2], V[2]));
const temperature = () => (2 * kinetic()) / (3 * (N - 1));
function step(thermostat) {
    for (let c = 0; c < 3; c++) {
        axpy(V[c], DT / 2, F[c]);
        axpy(P[c], DT, V[c]);
        axpy(U[c], DT, V[c]);
    }
    if (++sinceBuild >= 10) buildNeighbours();
    const r = computeForces();
    for (let c = 0; c < 3; c++) axpy(V[c], DT / 2, F[c]);
    if (thermostat) {
        const k = Math.sqrt(1 + (DT / 0.1) * (T_TARGET / temperature() - 1)); // Berendsen, τ = 0.1
        for (const v of V) scale(v, k);
    }
    return r;
}

buildNeighbours();
const initialListPairs = I.length;
let state = computeForces();
const brute = bruteForces();
const bruteNeighbours = (() => {
    let n = 0;
    for (let i = 0; i < N; i++)
        for (let j = i + 1; j < N; j++) {
            const r2 =
                minImage(P[0][i] - P[0][j]) ** 2 + minImage(P[1][i] - P[1][j]) ** 2 + minImage(P[2][i] - P[2][j]) ** 2;
            if (r2 < (RC + SKIN) ** 2) n++;
        }
    return n;
})();
const listPairs = state.pairs,
    initial = state;
// the largest force component is O(100) at close contact: compare relative to it, in single precision
const forceErr = Math.max(
    ...[0, 1, 2].map((c) =>
        Math.max(...F[c].map((f, i) => Math.abs(f - brute.out[c][i]) / (1 + Math.abs(brute.out[c][i])))),
    ),
);
const tEquil = performance.now();
for (let s = 0; s < 400; s++) state = step(true);
const equilMs = performance.now() - tEquil;
const start = U.map((u) => Float32Array.from(u)),
    energies = [],
    temps = [],
    pressures = [],
    msd = [],
    vsnap = []; // velocity snapshots every 5 steps for the autocorrelation
const RDF_BINS = 100,
    rdf = new Float32Array(RDF_BINS + 1); // the last slot collects every distance beyond L/2
let rdfSamples = 0;
// One atom against all later atoms per call: minimum image as ±L where |d| > L/2 (positions drift at most a skin
// outside the box between rebuilds, so one image correction suffices), then bin r into the histogram.
const rdfScratch = { d: new Float32Array(N), r2: new Float32Array(N), t: new Float32Array(N), bin: new Int32Array(N) };
const ONES = new Float32Array(N).fill(2);
function rdfSample() {
    const { d, r2, t, bin } = rdfScratch;
    for (let i = 0; i < N - 1; i++) {
        const n = N - 1 - i,
            dd = d.subarray(0, n),
            rr = r2.subarray(0, n),
            tt = t.subarray(0, n);
        rr.fill(0);
        for (let c = 0; c < 3; c++) {
            dd.set(P[c].subarray(i + 1));
            addScalar(dd, -P[c][i]);
            tt.set(dd);
            threshold(tt, HALF); // 1 where d > L/2
            axpy(dd, -L, tt);
            tt.set(dd);
            scale(tt, -1);
            threshold(tt, HALF); // 1 where d < -L/2
            axpy(dd, L, tt);
            fma(rr, dd, dd);
        }
        vsqrt(rr);
        scale(rr, RDF_BINS / HALF);
        clamp(rr, 0, RDF_BINS);
        bin.subarray(0, n).set(rr); // truncation toward zero is the floor for non-negative r
        scatterAdd(rdf, bin.subarray(0, n), ONES.subarray(0, n));
    }
    rdfSamples++;
}
let rdfOracle = null,
    rdfFirst = null;
const tNve = performance.now();
for (let s = 0; s < 800; s++) {
    state = step(false);
    const ke = kinetic(),
        T = temperature();
    energies.push((ke + state.pot) / N);
    temps.push(T);
    pressures.push(RHO * T + state.virial / (3 * L ** 3));
    if (s % 5 === 0) vsnap.push(V.map((v) => Float32Array.from(v)));
    if (s % 50 === 0) {
        let m = 0;
        for (let c = 0; c < 3; c++) m += distL2(U[c], start[c]) ** 2;
        msd.push([s * DT, m / N]);
        rdfSample();
        if (s === 0) rdfFirst = rdf.slice(0, RDF_BINS);
        if (s === 0) rdfOracle = rdfPlain(); // the first sample, binned pair by pair through Math.round images
    }
}
const nveMs = performance.now() - tNve;
function rdfPlain() {
    const h = new Float64Array(RDF_BINS);
    for (let i = 0; i < N; i++)
        for (let j = i + 1; j < N; j++) {
            const r = Math.hypot(minImage(P[0][i] - P[0][j]), minImage(P[1][i] - P[1][j]), minImage(P[2][i] - P[2][j]));
            if (r < HALF) h[Math.floor((r / HALF) * RDF_BINS)] += 2;
        }
    return h;
}
// a pair at exactly a bin edge may land one bin over in single precision: allow a handful of moved pairs
const rdfMoved = rdfFirst.reduce((a, c, b) => a + Math.abs(c - rdfOracle[b]), 0) / 2;
const g = Array.from(rdf.subarray(0, RDF_BINS), (c, b) => {
    const r0 = (b / RDF_BINS) * HALF,
        r1 = ((b + 1) / RDF_BINS) * HALF;
    return c / (rdfSamples * N * RHO * (4 / 3) * Math.PI * (r1 ** 3 - r0 ** 3));
});
const gPeak = g.indexOf(Math.max(...g)),
    rPeak = ((gPeak + 0.5) / RDF_BINS) * HALF;
const speeds = Array.from({ length: N }, (_, i) => Math.hypot(V[0][i], V[1][i], V[2][i])).sort((a, b) => a - b);
const Tnow = temperature(),
    mbCdf = (v) =>
        erf(v / Math.sqrt(2 * Tnow)) - Math.sqrt(2 / Math.PI) * (v / Math.sqrt(Tnow)) * Math.exp(-(v * v) / (2 * Tnow));
const ks = Math.max(...speeds.map((v, i) => Math.max(Math.abs((i + 1) / N - mbCdf(v)), Math.abs(i / N - mbCdf(v)))));
const tail = msd.slice(Math.floor(msd.length / 2)),
    slope =
        stats.cov(
            tail.map((m) => m[0]),
            tail.map((m) => m[1]),
        ) / stats.variance(tail.map((m) => m[0]));
const D = slope / 6;
// Green–Kubo: D = (1/3) ∫ <v(0)·v(t)> dt, averaged over every time origin; an independent route to the same number
const LAGS = 60,
    vacf = Array.from({ length: LAGS }, (_, lag) => {
        let s = 0,
            n = 0;
        for (let t0 = 0; t0 + lag < vsnap.length; t0++, n++)
            for (let c = 0; c < 3; c++) s += dot(vsnap[t0][c], vsnap[t0 + lag][c]);
        return s / (n * N);
    });
const dtSnap = 5 * DT,
    Dgk = (dtSnap * (vacf.sum() - vacf[0] / 2)) / 3; // trapezoid rule
const vacfFirstNegative = vacf.findIndex((c) => c < 0);
const drift = Math.abs(energies.at(-1) - energies[0]) / Math.abs(energies[0]);

// =============================================================================
// Heat equation: FTCS vs Crank–Nicolson vs the exact solution
// =============================================================================
const ALPHA = 1;
const exact = (x, t) =>
    Math.sin(Math.PI * x) * Math.exp(-ALPHA * Math.PI ** 2 * t) +
    0.5 * Math.sin(3 * Math.PI * x) * Math.exp(-ALPHA * 9 * Math.PI ** 2 * t);
function solveHeat(nx, r, scheme, tEnd = 0.05) {
    const dx = 1 / nx,
        dt = (r * dx * dx) / ALPHA,
        steps = Math.round(tEnd / dt);
    let u = Float64Array.from({ length: nx + 1 }, (_, i) => exact(i * dx, 0));
    if (scheme === "ftcs") {
        for (let s = 0; s < steps; s++) {
            const n = new Float64Array(nx + 1);
            for (let i = 1; i < nx; i++) n[i] = u[i] + r * (u[i + 1] - 2 * u[i] + u[i - 1]);
            u = n;
            if (!Number.isFinite(u[nx >> 1]) || Math.abs(u[nx >> 1]) > 1e6) return { error: Infinity, steps };
        }
    } else {
        const m = nx - 1,
            a = new Float64Array(m).fill(-r / 2),
            b = new Float64Array(m).fill(1 + r),
            c = new Float64Array(m).fill(-r / 2);
        for (let s = 0; s < steps; s++) {
            const d = Float64Array.from(
                { length: m },
                (_, k) => (r / 2) * u[k] + (1 - r) * u[k + 1] + (r / 2) * u[k + 2],
            );
            const cp = new Float64Array(m),
                dp = new Float64Array(m); // Thomas algorithm
            cp[0] = c[0] / b[0];
            dp[0] = d[0] / b[0];
            for (let k = 1; k < m; k++) {
                const den = b[k] - a[k] * cp[k - 1];
                cp[k] = c[k] / den;
                dp[k] = (d[k] - a[k] * dp[k - 1]) / den;
            }
            const x = new Float64Array(m);
            x[m - 1] = dp[m - 1];
            for (let k = m - 2; k >= 0; k--) x[k] = dp[k] - cp[k] * x[k + 1];
            u = new Float64Array(nx + 1);
            u.set(x, 1);
        }
    }
    const tFinal = steps * dt;
    return { error: Math.max(...Array.from(u, (v, i) => Math.abs(v - exact(i * dx, tFinal)))), steps };
}
const heat = [];
for (const nx of [20, 40, 80])
    heat.push({
        nx,
        ftcsStable: solveHeat(nx, 0.4, "ftcs"),
        ftcsUnstable: solveHeat(nx, 0.6, "ftcs", 0.3),
        cn: solveHeat(nx, 2.0, "cn"),
        cnFine: solveHeat(nx, 0.5, "cn"),
    });
const order = Math.log2(heat[1].cnFine.error / heat[2].cnFine.error);

// =============================================================================
// Kepler orbit: leapfrog vs RK4
// =============================================================================
function orbit(method, periods = 1000, stepsPer = 30) {
    let x = 1,
        y = 0,
        vx = 0,
        vy = 1.0; // circular-ish orbit with GM = 1
    vy = 1.1; // an eccentric orbit, e ≈ 0.21
    const dt = (2 * Math.PI) / stepsPer,
        E0 = 0.5 * (vx * vx + vy * vy) - 1 / Math.hypot(x, y);
    const acc = (px, py) => {
        const r3 = Math.hypot(px, py) ** 3;
        return [-px / r3, -py / r3];
    };
    let maxErr = 0;
    for (let s = 0; s < periods * stepsPer * 1.5; s++) {
        if (method === "leapfrog") {
            let [ax, ay] = acc(x, y);
            vx += 0.5 * dt * ax;
            vy += 0.5 * dt * ay;
            x += dt * vx;
            y += dt * vy;
            [ax, ay] = acc(x, y);
            vx += 0.5 * dt * ax;
            vy += 0.5 * dt * ay;
        } else {
            const f = (s2) => {
                    const [ax, ay] = acc(s2[0], s2[1]);
                    return [s2[2], s2[3], ax, ay];
                },
                s0 = [x, y, vx, vy],
                add = (a, b, h) => a.map((v, i) => v + h * b[i]);
            const k1 = f(s0),
                k2 = f(add(s0, k1, dt / 2)),
                k3 = f(add(s0, k2, dt / 2)),
                k4 = f(add(s0, k3, dt));
            [x, y, vx, vy] = s0.map((v, i) => v + (dt / 6) * (k1[i] + 2 * k2[i] + 2 * k3[i] + k4[i]));
        }
        if (s % stepsPer === 0)
            maxErr = Math.max(maxErr, Math.abs((0.5 * (vx * vx + vy * vy) - 1 / Math.hypot(x, y) - E0) / E0));
    }
    return { finalErr: Math.abs((0.5 * (vx * vx + vy * vy) - 1 / Math.hypot(x, y) - E0) / E0), maxErr };
}
const leap = orbit("leapfrog"),
    rk4 = orbit("rk4");

// =============================================================================
// Checks and report
// =============================================================================
const results = {};
results[
    `vectorised neighbour-list forces match a double-precision brute force (max rel diff ${forceErr.toExponential(1)})`
] =
    forceErr < 1e-4 &&
    brute.pairs === listPairs &&
    Math.abs(initial.pot - brute.pot) < 1e-4 * Math.abs(brute.pot) &&
    Math.abs(initial.virial - brute.virial) < 1e-4 * Math.abs(brute.virial);
results[`the KDTree + ghost-image list holds exactly the ${bruteNeighbours} pairs a brute-force scan finds`] =
    initialListPairs === bruteNeighbours;
results[`equilibrated temperature ${stats.mean(temps).toFixed(3)} (target ${T_TARGET})`] =
    Math.abs(stats.mean(temps) - T_TARGET) < 0.1;
results[`NVE energy drift over 800 steps: ${(drift * 100).toFixed(3)}%`] = drift < 0.005;
results[`g(r) peaks at r = ${rPeak.toFixed(3)}σ (2^(1/6) = ${(2 ** (1 / 6)).toFixed(3)}) and tends to 1`] =
    Math.abs(rPeak - 2 ** (1 / 6)) < 0.12 && Math.abs(stats.mean(g.slice(80)) - 1) < 0.08;
results[
    `the vectorised g(r) histogram bins ${(rdfOracle.sum() / 2).toFixed(0)} pairs as the plain loop does (${rdfMoved / 2} moved a bin)`
] = rdfMoved <= 8 && rdfFirst.sum() === rdfOracle.sum();
results[`speeds follow Maxwell–Boltzmann (KS D = ${ks.toFixed(4)} < ${(1.36 / Math.sqrt(N)).toFixed(4)} at 5%)`] =
    ks < 1.36 / Math.sqrt(N);
results[`self-diffusion D = ${D.toFixed(4)} σ²/τ from the MSD slope`] = D > 0.02 && D < 0.2;
results[
    `Green–Kubo D = ${Dgk.toFixed(4)} from the velocity autocorrelation agrees with the MSD within 35%; the VACF turns negative at ${(vacfFirstNegative * dtSnap).toFixed(2)}τ (the cage)`
] = Math.abs(Dgk - D) < 0.35 * D && vacfFirstNegative > 0 && vacfFirstNegative * dtSnap < 0.6;
results["FTCS is stable at r = 0.4 and explodes at r = 0.6; Crank–Nicolson is stable at r = 2"] = heat.every(
    (h) => h.ftcsStable.error < 0.01 && h.ftcsUnstable.error > 1 && h.cn.error < 0.02,
);
results[`Crank–Nicolson converges at order ${order.toFixed(2)} in space-time (expected 2)`] = Math.abs(order - 2) < 0.3;
results[
    `after 1500 orbits: leapfrog energy error stays bounded (${leap.maxErr.toExponential(1)}); RK4 drifts to ${rk4.finalErr.toExponential(1)}`
] = leap.maxErr < 1e-2 && rk4.finalErr > leap.finalErr * 5;
writeFile(
    work.join("rdf.csv"),
    csv.stringify([["r", "g"], ...g.map((v, b) => [(((b + 0.5) / RDF_BINS) * HALF).toFixed(4), v.toFixed(4)])]),
);
const pdf = new DataFrame({ p: Float64Array.from(pressures) });

print("physics");
print(
    `  MD         ${N} atoms, ρ=${RHO}, box ${L.toFixed(2)}σ, ${state.pairs} pairs inside rc; 400 thermostatted + 800 NVE steps (${(equilMs / 400).toFixed(2)} ms/step thermostatted, ${(nveMs / 800).toFixed(2)} with measurements), ${rebuilds} neighbour-list rebuilds`,
);
print(
    table([
        ["observable", "value"],
        ["temperature", `${stats.mean(temps).toFixed(3)} ± ${stats.stddev(temps).toFixed(3)}`],
        ["pressure (virial)", `${pdf.MEAN("p").toFixed(3)} ± ${pdf.STDDEV("p").toFixed(3)}`],
        ["energy per atom", `${energies[0].toFixed(4)} -> ${energies.at(-1).toFixed(4)}`],
        ["g(r) first peak", `${rPeak.toFixed(3)}σ, height ${Math.max(...g).toFixed(2)}`],
        ["KS vs Maxwell–Boltzmann", ks.toFixed(4)],
        ["diffusion D (MSD)", D.toFixed(4)],
        ["diffusion D (Green–Kubo)", Dgk.toFixed(4)],
    ]),
);
print(
    table([
        ["nx", "FTCS r=0.4", "FTCS r=0.6", "CN r=2", "CN r=0.5"],
        ...heat.map((h) => [
            String(h.nx),
            h.ftcsStable.error.toExponential(2),
            h.ftcsUnstable.error > 1 ? "unstable" : h.ftcsUnstable.error.toExponential(2),
            h.cn.error.toExponential(2),
            h.cnFine.error.toExponential(2),
        ]),
    ]),
);
print(
    `  orbits     1500 periods, 30 steps each: leapfrog max |ΔE/E| ${leap.maxErr.toExponential(2)}, RK4 final ${rk4.finalErr.toExponential(2)}`,
);
removeAll(work);
check(results);

function check(results) {
    const names = Object.keys(results);
    const failed = names.filter((name) => !results[name]);
    for (const name of failed) print("FAIL " + name);
    if (failed.length) throw new Error(`${failed.length} of ${names.length} checks failed`);
}
