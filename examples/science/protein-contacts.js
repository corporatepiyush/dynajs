// Protein contacts — a C-alpha backbone built as a compact self-avoiding chain, its contact map from a KDTree, radius of
// gyration and contact order, and structural superposition (RMSD) by Horn's quaternion method.
//
// WHAT IT IS
//   The geometry kernels behind structure comparison and contact prediction:
//
//   helices     nine alpha-helices are planted: each helical residue is placed by NeRF from the three before it with
//               the virtual bond angle and torsion of an ideal C-alpha helix (rise 1.5 A, 100 degrees per residue); a
//               clash inside a helix restarts the chain in the coil before it
//   assignment  secondary structure from C-alpha coordinates alone (i+2/i+3/i+4 distances and virtual torsions within
//               tolerance of the ideal); it must recover the planted helices and find none in the mirror image
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
//               for isotropic noise. Optimality is checked from the definition: 300 rotations near Horn's and at
//               random never give a lower RMSD
//   unfolding   fraction of native contacts Q of a copy shaken by noise sigma (its own KDTree, RoaringBitmap
//               andCardinality against the native neighbour sets): exactly 1 for a rigid motion, falling with sigma
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
// Ideal C-alpha helix: rise 1.5 A and 100 degrees per residue, the radius chosen so consecutive residues are exactly
// BOND apart. Its virtual bond angle and torsion are read off four consecutive points; NeRF (natural extension
// reference frame) then places each helical residue from the three before it.
// =============================================================================
const deg = Math.PI / 180,
    HR = Math.sqrt(BOND ** 2 - 1.5 ** 2) / (2 * Math.sin(50 * deg));
const ideal = (k) => [HR * Math.cos(k * 100 * deg), HR * Math.sin(k * 100 * deg), 1.5 * k];
const sub = (a, b) => [a[0] - b[0], a[1] - b[1], a[2] - b[2]],
    dot = (a, b) => a[0] * b[0] + a[1] * b[1] + a[2] * b[2],
    cross = (a, b) => [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]],
    unit = (a) => a.map((v) => v / Math.hypot(...a));
const angle = (a, b, c) => Math.acos(dot(unit(sub(a, b)), unit(sub(c, b))));
function torsion(a, b, c, d) {
    const b1 = sub(b, a),
        b2 = sub(c, b),
        b3 = sub(d, c),
        n1 = cross(b1, b2),
        n2 = cross(b2, b3);
    return Math.atan2(dot(cross(n1, n2), unit(b2)), dot(n1, n2));
}
const H_ANGLE = angle(ideal(0), ideal(1), ideal(2)),
    H_TORSION = torsion(ideal(0), ideal(1), ideal(2), ideal(3));
function nerf(a, b, c, length, theta, phi) {
    const bc = unit(sub(c, b)),
        n = unit(cross(sub(b, a), bc)),
        m = cross(n, bc);
    const d = [
        -length * Math.cos(theta),
        length * Math.sin(theta) * Math.cos(phi),
        length * Math.sin(theta) * Math.sin(phi),
    ];
    return [0, 1, 2].map((k) => c[k] + d[0] * bc[k] + d[1] * m[k] + d[2] * n[k]);
}
// Planted helices [start, end): every residue in them is placed by NeRF with the helical angle and torsion.
const HELICES = [
    [20, 34],
    [60, 78],
    [110, 122],
    [160, 176],
    [230, 246],
    [300, 314],
    [380, 398],
    [450, 466],
    [520, 534],
];
const helical = new Uint8Array(N);
for (const [a, b] of HELICES) helical.fill(1, a, b);

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
        const clashes = (q) => {
            if (Array.from(tree.radius(q, CLASH).indices).some((j) => j < placed - 1)) return true;
            for (let j = indexed; j < placed - 1; j++)
                if (Math.hypot(xyz[j * 3] - q[0], xyz[j * 3 + 1] - q[1], xyz[j * 3 + 2] - q[2]) < CLASH) return true;
            return false;
        };
        const at = (k) => [...xyz.subarray(k * 3, k * 3 + 3)];
        let ok = false;
        if (helical[placed]) {
            // deterministic: one try, and a clash backs the chain up to the coil before this helix
            attemptsTotal++;
            const q = nerf(at(placed - 3), at(placed - 2), at(placed - 1), BOND, H_ANGLE, H_TORSION);
            if (!clashes(q)) {
                xyz.set(q, placed * 3);
                ok = true;
            }
        } else
            for (let attempt = 0; attempt < 200 && !ok; attempt++) {
                attemptsTotal++;
                const dx = rng.normal() - 0.04 * p[0],
                    dy = rng.normal() - 0.04 * p[1],
                    dz = rng.normal() - 0.04 * p[2];
                const s = BOND / Math.hypot(dx, dy, dz);
                const q = [p[0] + dx * s, p[1] + dy * s, p[2] + dz * s];
                if (clashes(q)) continue;
                xyz.set(q, placed * 3);
                ok = true;
            }
        // A trapped chain end backs up a few residues and regrows (the tree is rebuilt for the shorter prefix). A
        // helix regrows identically from the same three residues, so the restart moves back into the coil before
        // it, whose random step gives the helix a new frame. (With this seed every helix clash happens within its
        // first 8 residues, where the plain 8-residue back-up already lands in the coil: the rule is not reached.)
        if (!ok) {
            let back = Math.max(1, placed - 8);
            while (back > 1 && helical[back]) back--;
            placed = back - 1;
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
// Secondary structure from C-alpha geometry alone: a window i..i+4 is helical when its i+2, i+3 and i+4 distances
// and both virtual torsions are those of an ideal helix (within 0.2 A and 15 degrees); every residue of a helical
// window is helix. Planted residues s..e-1 make windows s-2 .. e-5 exact, so the truth is the run [s-2, e).
// =============================================================================
const D2 = Math.hypot(...sub(ideal(2), ideal(0))),
    D3 = Math.hypot(...sub(ideal(3), ideal(0))),
    D4 = Math.hypot(...sub(ideal(4), ideal(0)));
function assignHelices(P) {
    const at = (k) => [P[k * 3], P[k * 3 + 1], P[k * 3 + 2]],
        d = (a, b) => Math.hypot(...sub(at(a), at(b))),
        tor = (k) => torsion(at(k), at(k + 1), at(k + 2), at(k + 3));
    const mark = new Uint8Array(N);
    for (let i = 0; i + 4 < N; i++)
        if (
            Math.abs(d(i, i + 2) - D2) < 0.2 &&
            Math.abs(d(i, i + 3) - D3) < 0.2 &&
            Math.abs(d(i, i + 4) - D4) < 0.2 &&
            Math.abs(tor(i) - H_TORSION) < 15 * deg &&
            Math.abs(tor(i + 1) - H_TORSION) < 15 * deg
        )
            mark.fill(1, i, i + 5);
    const runs = [];
    for (let i = 0; i < N; i++)
        if (mark[i] && !mark[i - 1]) runs.push([i, mark.indexOf(0, i) < 0 ? N : mark.indexOf(0, i)]);
    return runs;
}
const found = assignHelices(xyz);
// a boundary may shift by one residue where the random coil next to a helix happens to look helical, but a
// systematic shift would move all of them
const exact = found.filter(([a, b], k) => a === HELICES[k]?.[0] - 2 && b === HELICES[k]?.[1]).length;
results[
    `the C-alpha assignment finds the ${HELICES.length} planted helices and nothing else (${exact} exact, the rest within 1)`
] =
    found.length === HELICES.length &&
    exact >= HELICES.length - 2 &&
    found.every(([a, b], k) => Math.abs(a - (HELICES[k][0] - 2)) <= 1 && Math.abs(b - HELICES[k][1]) <= 1);
results["a mirrored structure has no right-handed helices"] =
    assignHelices(xyz.map((v, k) => (k % 3 === 0 ? -v : v))).length === 0;

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
// Fraction of native contacts Q of another conformation: one KDTree over it, each native pair checked in its bitmap.
function nativeFraction(P) {
    const t = new KDTree(P, 3);
    let kept = 0;
    for (let i = 0; i < N; i++) {
        const here = new RoaringBitmap(t.radius(P.subarray(i * 3, i * 3 + 3), CONTACT).indices);
        kept += nbr[i].andCardinality(here);
    }
    return kept / (2 * pairs.length);
}

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
    return { rmsd: rmsdUnder(R, a, b), R, a, b };
}
// RMSD of the centred a against the centred b mapped by R^T: the quantity the superposition minimises.
function rmsdUnder(R, a, b) {
    let sum = 0;
    for (let i = 0; i < N; i++) {
        const p = [0, 1, 2].map((r) => R[0][r] * b[i * 3] + R[1][r] * b[i * 3 + 1] + R[2][r] * b[i * 3 + 2]);
        sum += (p[0] - a[i * 3]) ** 2 + (p[1] - a[i * 3 + 1]) ** 2 + (p[2] - a[i * 3 + 2]) ** 2;
    }
    return Math.sqrt(sum / N);
}
const matMul = (A, B) => A.map((row) => [0, 1, 2].map((c) => row.reduce((s, v, k) => s + v * B[k][c], 0)));
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
const rigid = rmsdAfterSuperposition(xyz, moved(0)).rmsd;
const noisySup = rmsdAfterSuperposition(xyz, moved(0.5)),
    noisy = noisySup.rmsd;
{
    // optimality from the definition: no rotation near or far from Horn's beats it
    let beaten = 0;
    for (let k = 0; k < 300; k++) {
        const u = [0, 1, 2].map(() => rng.normal()),
            n = Math.hypot(...u),
            half = (k < 200 ? 10 ** (-3 + (k % 4)) : Math.PI) * rng.nextFloat();
        const dq = [Math.cos(half), ...u.map((v) => (v / n) * Math.sin(half))];
        if (rmsdUnder(matMul(quatToMatrix(dq), noisySup.R), noisySup.a, noisySup.b) < noisy - 1e-12) beaten++;
    }
    results["no rotation, near Horn's or random, gives a lower RMSD (300 tried)"] = beaten === 0;
}
// Q as the structure is shaken harder: a rigid copy keeps every contact, and Q falls as the noise doubles
const qCurve = [0, 0.25, 0.5, 1, 2, 4].map((sigma) => [sigma, nativeFraction(moved(sigma))]);
results[`native contacts Q: 1 for a rigid copy, then falling ${qCurve.map(([, q]) => q.toFixed(2)).join(" > ")}`] =
    qCurve[0][1] === 1 && qCurve.every(([, q], k) => k === 0 || q < qCurve[k - 1][1]) && qCurve.at(-1)[1] < 0.5;
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
            [
                "helices planted / assigned",
                `${HELICES.length} / ${found.length} (${found.map(([a, b]) => `${a}-${b - 1}`).join(" ")})`,
            ],
            ["native contacts Q at sigma 0..4 A", qCurve.map(([s, q]) => `${s}:${q.toFixed(2)}`).join(" ")],
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
