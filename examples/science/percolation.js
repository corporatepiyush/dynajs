// Percolation — site and bond percolation thresholds on the square lattice estimated by the Newman-Ziff algorithm
// over a native UnionFind, compared with the published values, plus cluster statistics near criticality.
//
// WHAT IT IS
//   A statistical-physics workhorse (porous media, epidemics on grids, network robustness):
//
//   Newman-Ziff sites are opened in a random order (Random.shuffle over an Int32Array) and merged with open neighbours
//               in a UnionFind that also holds two virtual nodes, top and bottom; the fraction open at the moment
//               top and bottom connect is one sample of the spanning threshold. One sweep per sample, not one
//               simulation per p
//   thresholds  site percolation must land near p_c = 0.592746 (Newman & Ziff, PRL 85, 2000); bond percolation near
//               the exact p_c = 1/2 (Kesten 1980); finite-size estimates move toward them as L grows
//   oracle      at fixed p, a BFS flood fill over a BitSet of open sites decides spanning independently of the
//               UnionFind; the two must agree on every lattice
//   clusters    cluster sizes at p_c from UnionFind roots (Multiset); the largest cluster's share jumps across p_c;
//               the size distribution has a heavy tail (many more large clusters than at p = 0.45)
//
// MODULES  dyna:structures (UnionFind, BitSet, Multiset, Deque) · dyna:random · dyna:simd (f64Mean, f64Variance) ·
//          dyna:cli · Array extensions
//
// RUN      dynajs examples/science/percolation.js

import { UnionFind, BitSet, Multiset, Deque } from "dyna:structures";
import { f64Mean, f64Variance } from "dyna:simd";
import { Random } from "dyna:random";
import { Table } from "dyna:cli";

const results = {};
const rng = new Random(1957);
const SITE_PC = 0.592746,
    BOND_PC = 0.5;

// =============================================================================
// Newman-Ziff sweeps
// =============================================================================
function siteThreshold(L) {
    const N = L * L,
        TOP = N,
        BOTTOM = N + 1;
    const uf = new UnionFind(N + 2),
        open = new Uint8Array(N);
    const order = Int32Array.from({ length: N }, (_, i) => i);
    rng.shuffle(order);
    for (let k = 0; k < N; k++) {
        const s = order[k],
            r = (s / L) | 0,
            c = s % L;
        open[s] = 1;
        if (r === 0) uf.union(s, TOP);
        if (r === L - 1) uf.union(s, BOTTOM);
        if (r > 0 && open[s - L]) uf.union(s, s - L);
        if (r < L - 1 && open[s + L]) uf.union(s, s + L);
        if (c > 0 && open[s - 1]) uf.union(s, s - 1);
        if (c < L - 1 && open[s + 1]) uf.union(s, s + 1);
        if (uf.connected(TOP, BOTTOM)) return (k + 1) / N;
    }
    return 1;
}
function bondThreshold(L) {
    const N = L * L,
        TOP = N,
        BOTTOM = N + 1,
        bonds = [];
    for (let r = 0; r < L; r++)
        for (let c = 0; c < L; c++) {
            const s = r * L + c;
            if (c < L - 1) bonds.push(s, s + 1);
            if (r < L - 1) bonds.push(s, s + L);
        }
    const uf = new UnionFind(N + 2);
    for (let c = 0; c < L; c++) {
        uf.union(c, TOP);
        uf.union((L - 1) * L + c, BOTTOM);
    }
    const order = Int32Array.from({ length: bonds.length / 2 }, (_, i) => i);
    rng.shuffle(order);
    for (let k = 0; k < order.length; k++) {
        const b = order[k];
        uf.union(bonds[2 * b], bonds[2 * b + 1]);
        if (uf.connected(TOP, BOTTOM)) return (k + 1) / order.length;
    }
    return 1;
}
const estimate = (fn, L, runs) => {
    const xs = new Float64Array(runs);
    for (let i = 0; i < runs; i++) xs[i] = fn(L);
    return { mean: f64Mean(xs), se: Math.sqrt(f64Variance(xs) / runs) };
};
const t0 = performance.now();
const rows = [];
const siteBy = {},
    bondBy = {};
for (const L of [16, 64, 160]) {
    const runs = L === 160 ? 60 : 200;
    siteBy[L] = estimate(siteThreshold, L, runs);
    bondBy[L] = estimate(bondThreshold, L, runs);
    rows.push([
        L,
        runs,
        `${siteBy[L].mean.toFixed(4)} ± ${siteBy[L].se.toFixed(4)}`,
        `${bondBy[L].mean.toFixed(4)} ± ${bondBy[L].se.toFixed(4)}`,
    ]);
}
const sweepMs = performance.now() - t0;
print(Table(rows, { head: ["L", "samples", `site p_c (ref ${SITE_PC})`, `bond p_c (exact ${BOND_PC})`] }));
results["site threshold at L=160 within 0.01 of 0.592746"] = Math.abs(siteBy[160].mean - SITE_PC) < 0.01;
results["bond threshold at L=160 within 0.01 of 1/2"] = Math.abs(bondBy[160].mean - BOND_PC) < 0.01;
results["finite-size spread shrinks as L grows"] = siteBy[160].se < siteBy[16].se && bondBy[160].se < bondBy[16].se;

// =============================================================================
// Oracle: spanning by BFS flood fill vs UnionFind at fixed p
// =============================================================================
function lattice(L, p) {
    const open = new BitSet(L * L);
    for (let i = 0; i < L * L; i++) if (rng.nextFloat() < p) open.set(i);
    return open;
}
function spansUF(L, open) {
    const N = L * L,
        uf = new UnionFind(N + 2);
    for (let s = 0; s < N; s++) {
        if (!open.get(s)) continue;
        const r = (s / L) | 0,
            c = s % L;
        if (r === 0) uf.union(s, N);
        if (r === L - 1) uf.union(s, N + 1);
        if (r < L - 1 && open.get(s + L)) uf.union(s, s + L);
        if (c < L - 1 && open.get(s + 1)) uf.union(s, s + 1);
    }
    return { spans: uf.connected(N, N + 1), uf };
}
function spansBFS(L, open) {
    const seen = new Uint8Array(L * L),
        q = new Deque();
    for (let c = 0; c < L; c++)
        if (open.get(c)) {
            seen[c] = 1;
            q.pushBack(c);
        }
    for (let s; (s = q.popFront()) !== undefined;) {
        const r = (s / L) | 0,
            c = s % L;
        if (r === L - 1) return true;
        for (const n of [r > 0 ? s - L : -1, r < L - 1 ? s + L : -1, c > 0 ? s - 1 : -1, c < L - 1 ? s + 1 : -1])
            if (n >= 0 && !seen[n] && open.get(n)) {
                seen[n] = 1;
                q.pushBack(n);
            }
    }
    return false;
}
{
    let agree = 0,
        total = 0,
        spanning = 0;
    for (const p of [0.5, 0.57, 0.59, 0.6, 0.62, 0.7])
        for (let i = 0; i < 25; i++) {
            const open = lattice(48, p),
                a = spansUF(48, open).spans,
                b = spansBFS(48, open);
            total++;
            if (a === b) agree++;
            if (a) spanning++;
        }
    results["UnionFind spanning agrees with BFS flood fill on 150 lattices"] = agree === total;
    results["both outcomes occur near threshold"] = spanning > 20 && spanning < total - 20;
}

// =============================================================================
// Cluster statistics
// =============================================================================
function clusters(L, p) {
    const open = lattice(L, p),
        { uf } = spansUF(L, open),
        sizes = new Multiset();
    for (let s = 0; s < L * L; s++) if (open.get(s)) sizes.add(String(uf.find(s)));
    const list = sizes
        .entrySet()
        .map(([, n]) => n)
        .sort((a, b) => b - a);
    return { largestShare: (list[0] ?? 0) / (L * L), big: list.filter((n) => n >= 100).length, count: list.length };
}
{
    const L = 200;
    const below = clusters(L, 0.45),
        at = clusters(L, SITE_PC),
        above = clusters(L, 0.7);
    results["largest cluster share jumps across p_c"] = below.largestShare < 0.02 && above.largestShare > 0.5;
    results["near p_c the size distribution has a heavy tail"] = at.big > below.big;
    print(
        Table(
            [
                ["0.45", below],
                ["0.5927", at],
                ["0.70", above],
            ].map(([p, c]) => [p, c.count.format(), (100 * c.largestShare).toFixed(2) + "%", c.big]),
            { head: ["p", "clusters", "largest cluster", "clusters >= 100 sites"] },
        ),
    );
}
print(`threshold sweeps: ${sweepMs.toFixed(0)} ms for ${(200 * 2 * 2 + 120).format()} Newman-Ziff runs`);

check(results);

function check(results) {
    const names = Object.keys(results);
    const failed = names.filter((name) => !results[name]);
    for (const name of failed) print("FAIL " + name);
    if (failed.length) throw new Error(`${failed.length} of ${names.length} checks failed`);
}
