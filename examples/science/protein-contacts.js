// Protein contacts — a C-alpha backbone built as a compact self-avoiding chain, its contact map from a KDTree, radius of
// gyration and contact order, and structural superposition (RMSD) by Horn's quaternion method.
//
// WHAT IT IS
//   The geometry kernels behind structure comparison and contact prediction:
//
//   backbone    600 residues placed 3.8 A apart (the C-alpha virtual bond), each step biased toward the centre and
//               rejected if it would come within 4 A of an earlier residue (KDTree radius query on the placed prefix); a
//               trapped end backs up 8 residues and regrows
//   contacts    residue pairs within 8 A, from one KDTree radius query per residue; checked pair-for-pair against an
//               O(n^2) scan; per-residue neighbour sets as RoaringBitmaps; relative contact order and the share of
//               short-range (|i - j| < 6) versus long-range (>= 24) contacts
//   size        radius of gyration with simd (f64Mean, f64Dot) against the compact-globule scaling ~2.2 N^0.38 A
//   superpose   a second conformation is the first rotated, translated and jittered; Horn's method builds the 4x4
//               key matrix from the cross-covariance and takes its top eigenvector (power iteration on a shifted
//               matrix) as the optimal rotation quaternion. RMSD must be ~0 for a pure rigid motion and ~sigma*sqrt(3)
//               for isotropic noise
//
// MODULES  dyna:structures (KDTree, RoaringBitmap, Multiset) · dyna:simd (f64Mean, f64Dot, f64Sum) · dyna:random ·
//          dyna:cli · Array extensions
//
// RUN      dynajs examples/science/protein-contacts.js

import { KDTree, RoaringBitmap, Multiset } from "dyna:structures";
import { f64Mean, f64Dot, f64Sum } from "dyna:simd";
import { Random } from "dyna:random";
import { Table } from "dyna:cli";

const results = {};
const rng = new Random(1958);
const N = 600,
    BOND = 3.8,
    CLASH = 4.0,
    CONTACT = 8.0;

// =============================================================================
// Compact self-avoiding chain
// =============================================================================
const xyz = new Float64Array(N * 3);
let placed = 1,
    attemptsTotal = 0,
    backtracks = 0;
// The KDTree is rebuilt every 25 residues over the placed prefix; the few residues placed since are scanned directly.
while (placed < N) {
    const indexed = placed,
        tree = new KDTree(xyz.subarray(0, indexed * 3), 3);
    let stuck = false;
    for (const end = Math.min(N, placed + 25); placed < end && !stuck; placed++) {
        const p = xyz.subarray((placed - 1) * 3, placed * 3);
        let ok = false;
        for (let attempt = 0; attempt < 200 && !ok; attempt++) {
            attemptsTotal++;
            const dx = rng.normal() - 0.04 * p[0],
                dy = rng.normal() - 0.04 * p[1],
                dz = rng.normal() - 0.04 * p[2];
            const s = BOND / Math.hypot(dx, dy, dz);
            const q = [p[0] + dx * s, p[1] + dy * s, p[2] + dz * s];
            if (Array.from(tree.radius(q, CLASH).indices).some((j) => j < placed - 1)) continue;
            let clash = false;
            for (let j = indexed; j < placed - 1 && !clash; j++)
                clash = Math.hypot(xyz[j * 3] - q[0], xyz[j * 3 + 1] - q[1], xyz[j * 3 + 2] - q[2]) < CLASH;
            if (clash) continue;
            xyz.set(q, placed * 3);
            ok = true;
        }
        // A trapped chain end backs up a few residues and regrows (the tree is rebuilt for the shorter prefix).
        if (!ok) {
            placed = Math.max(1, placed - 8) - 1;
            stuck = true;
            backtracks++;
        }
    }
}
const dist = (a, b, A = xyz, B = xyz) =>
    Math.hypot(A[a * 3] - B[b * 3], A[a * 3 + 1] - B[b * 3 + 1], A[a * 3 + 2] - B[b * 3 + 2]);
results["every virtual bond is 3.8 A"] = Array.from({ length: N - 1 }, (_, i) => dist(i, i + 1)).every(
    (d) => Math.abs(d - BOND) < 1e-9,
);
results["no two non-bonded residues clash (< 4 A)"] = (() => {
    for (let i = 0; i < N; i++) for (let j = i + 2; j < N; j++) if (dist(i, j) < CLASH - 1e-9) return false;
    return true;
})();

// =============================================================================
// Contact map
// =============================================================================
const tree = new KDTree(xyz, 3);
const nbr = [],
    pairs = [];
const t0 = performance.now();
for (let i = 0; i < N; i++) {
    const hits = Array.from(tree.radius(xyz.subarray(i * 3, i * 3 + 3), CONTACT).indices).filter(
        (j) => Math.abs(j - i) >= 3,
    );
    nbr.push(new RoaringBitmap(hits));
    for (const j of hits) if (j > i) pairs.push([i, j]);
}
const treeMs = performance.now() - t0;
const t1 = performance.now();
let brute = 0,
    missing = 0;
for (let i = 0; i < N; i++)
    for (let j = i + 3; j < N; j++)
        if (dist(i, j) <= CONTACT) {
            brute++;
            if (!nbr[i].has(j)) missing++;
        }
const bruteMs = performance.now() - t1;
results["KDTree contact map equals the O(n^2) scan"] = brute === pairs.length && missing === 0;
const sep = new Multiset();
for (const [i, j] of pairs) sep.add(j - i < 6 ? "short" : j - i < 24 ? "medium" : "long");
const rco = pairs.reduce((s, [i, j]) => s + (j - i), 0) / (pairs.length * N);
results["a compact chain has long-range contacts"] = sep.count("long") > sep.count("short");
results["contacts are symmetric"] = pairs.every(([i, j]) => nbr[j].has(i));

// =============================================================================
// Radius of gyration
// =============================================================================
const coord = (k) => Float64Array.from({ length: N }, (_, i) => xyz[i * 3 + k]);
const cx = coord(0),
    cy = coord(1),
    cz = coord(2);
const centred = [cx, cy, cz].map((c) => {
    const m = f64Mean(c);
    return c.map((v) => v - m);
});
const rg = Math.sqrt(centred.reduce((s, c) => s + f64Dot(c, c), 0) / N);
const expected = 2.2 * N ** 0.38;
results["radius of gyration is globule-like (within 2x of 2.2 N^0.38)"] = rg > expected / 2 && rg < expected * 2;

// =============================================================================
// Horn's quaternion superposition
// =============================================================================
function quatToMatrix([w, x, y, z]) {
    return [
        [w * w + x * x - y * y - z * z, 2 * (x * y - w * z), 2 * (x * z + w * y)],
        [2 * (x * y + w * z), w * w - x * x + y * y - z * z, 2 * (y * z - w * x)],
        [2 * (x * z - w * y), 2 * (y * z + w * x), w * w - x * x - y * y + z * z],
    ];
}
function centre(P) {
    const c = [0, 1, 2].map((k) => f64Sum(Float64Array.from({ length: N }, (_, i) => P[i * 3 + k])) / N);
    return [Float64Array.from(P, (v, idx) => v - c[idx % 3]), c];
}
function rmsdAfterSuperposition(A, B) {
    const [a] = centre(A),
        [b] = centre(B);
    const S = [
        [0, 0, 0],
        [0, 0, 0],
        [0, 0, 0],
    ];
    for (let i = 0; i < N; i++)
        for (let r = 0; r < 3; r++) for (let c = 0; c < 3; c++) S[r][c] += a[i * 3 + r] * b[i * 3 + c];
    const [[xx, xy, xz], [yx, yy, yz], [zx, zy, zz]] = S;
    const K = [
        [xx + yy + zz, yz - zy, zx - xz, xy - yx],
        [yz - zy, xx - yy - zz, xy + yx, zx + xz],
        [zx - xz, xy + yx, -xx + yy - zz, yz + zy],
        [xy - yx, zx + xz, yz + zy, -xx - yy + zz],
    ];
    // Shift so the largest eigenvalue is also the largest in magnitude, then power-iterate.
    const shift = K.reduce((s, row) => s + row.reduce((t, v) => t + Math.abs(v), 0), 0);
    let q = [1, 0, 0, 0];
    for (let it = 0; it < 500; it++) {
        const nq = K.map((row, r) => row.reduce((s, v, c) => s + v * q[c], 0) + shift * q[r]);
        const n = Math.hypot(...nq);
        q = nq.map((v) => v / n);
    }
    // With S = sum a b^T, the top eigenvector of K is the rotation taking a onto b; its transpose maps b onto a.
    const R = quatToMatrix(q);
    let sum = 0;
    for (let i = 0; i < N; i++) {
        const p = [0, 1, 2].map((r) => R[0][r] * b[i * 3] + R[1][r] * b[i * 3 + 1] + R[2][r] * b[i * 3 + 2]);
        sum += (p[0] - a[i * 3]) ** 2 + (p[1] - a[i * 3 + 1]) ** 2 + (p[2] - a[i * 3 + 2]) ** 2;
    }
    return Math.sqrt(sum / N);
}
function moved(noise) {
    const u = [rng.normal(), rng.normal(), rng.normal(), rng.normal()],
        n = Math.hypot(...u);
    const R = quatToMatrix(u.map((v) => v / n)),
        t = [12, -7, 30];
    const out = new Float64Array(N * 3);
    for (let i = 0; i < N; i++)
        for (let r = 0; r < 3; r++)
            out[i * 3 + r] =
                R[r][0] * xyz[i * 3] +
                R[r][1] * xyz[i * 3 + 1] +
                R[r][2] * xyz[i * 3 + 2] +
                t[r] +
                noise * rng.normal();
    return out;
}
const rigid = rmsdAfterSuperposition(xyz, moved(0));
const noisy = rmsdAfterSuperposition(xyz, moved(0.5));
const naive = (() => {
    const B = moved(0);
    let s = 0;
    for (let i = 0; i < N; i++) s += dist(i, i, xyz, B) ** 2;
    return Math.sqrt(s / N);
})();
results["RMSD after superposing a rigid motion is ~0"] = rigid < 1e-6;
results["RMSD with 0.5 A isotropic noise is ~0.5*sqrt(3)"] = Math.abs(noisy - 0.5 * Math.sqrt(3)) < 0.1;
results["without superposition the same structures look far apart"] = naive > 10;

print(
    Table(
        [
            ["residues / contacts", `${N} / ${pairs.length.format()}`],
            ["short / medium / long range", ["short", "medium", "long"].map((k) => sep.count(k)).join(" / ")],
            ["relative contact order", rco.toFixed(3)],
            ["radius of gyration", `${rg.toFixed(1)} A (scaling law ${expected.toFixed(1)} A)`],
            ["contact map: KDTree vs scan", `${treeMs.toFixed(1)} ms vs ${bruteMs.toFixed(1)} ms`],
            [
                "RMSD rigid / noisy / unsuperposed",
                `${rigid.toExponential(1)} / ${noisy.toFixed(3)} / ${naive.toFixed(1)} A`,
            ],
            ["placement attempts / backtracks", `${attemptsTotal.format()} / ${backtracks}`],
        ],
        { head: ["structure", "value"] },
    ),
);

check(results);

function check(results) {
    const names = Object.keys(results);
    const failed = names.filter((name) => !results[name]);
    for (const name of failed) print("FAIL " + name);
    if (failed.length) throw new Error(`${failed.length} of ${names.length} checks failed`);
}
