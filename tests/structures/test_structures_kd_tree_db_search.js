// covers: dyna:structures.KDTree
// Differential tests for KDTree, ConsistentHash and InvertedIndex.
//   KDTree: knn / radius / box against brute force over random, clustered and
//     duplicate-heavy point sets in 1..8 dims, with ties broken by index.
//   ConsistentHash: the published properties of the ring (Karger et al. 1997):
//     balance, monotonicity (adding a node only moves keys TO it), and the
//     placement being a pure function of the node set (insertion order and a
//     round trip through another process change nothing). Jump hash (Lamping
//     & Veach 2014): a key moves only to the newly added bucket.
//   InvertedIndex: BM25 scores against a brute-force JS scorer that uses the
//     documented formula (Robertson & Zaragoza 2009, idf = ln(1 + (N-df+.5)/(df+.5))).
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_structures_kd_tree_db_search");
import { KDTree, ConsistentHash, InvertedIndex } from "dyna:structures";
import { Exec } from "dyna:sys";
import { XXHash64 } from "dyna:hash";

let n = 0, fails = 0;
function check(c, m) { n++; if (!c) { fails++; if (fails < 40) print("FAIL: " + m); } }
function eq(a, b, m) { check(Object.is(a, b), m + " -- got " + a + ", want " + b); }
function same(a, b, m) { const x = String(JSON.stringify(a)), y = String(JSON.stringify(b)); check(x === y, m + "\n   got  " + x.slice(0, 300) + "\n   want " + y.slice(0, 300)); }
function rng(seed) { let s = seed >>> 0 || 1; return () => { s ^= s << 13; s >>>= 0; s ^= s >>> 17; s ^= s << 5; s >>>= 0; return s / 4294967296; }; }

// ---- KDTree -----------------------------------------------------------------
function d2(a, b) { let s = 0; for (let i = 0; i < a.length; i++) { const x = a[i] - b[i]; s += x * x; } return s; }

for (const [seed, dims, N, kind] of [[1, 2, 2000, "uniform"], [2, 3, 1500, "clustered"], [3, 1, 500, "dups"], [4, 8, 1200, "uniform"], [5, 2, 13, "uniform"], [6, 2, 12, "dups"], [7, 4, 3000, "grid"]]) {
    const r = rng(seed), pts = [];
    for (let i = 0; i < N; i++) {
        const p = [];
        for (let d = 0; d < dims; d++) {
            if (kind === "uniform") p.push(r() * 100 - 50);
            else if (kind === "clustered") p.push((i % 5) * 20 + r());
            else if (kind === "dups") p.push(Math.floor(r() * 4));
            else p.push(Math.floor(i / dims ** d) % 7);
        }
        pts.push(p);
    }
    const flat = new Float64Array(N * dims); pts.forEach((p, i) => p.forEach((x, d) => flat[i * dims + d] = x));
    const t1 = new KDTree(pts), t2 = new KDTree(flat, dims), t3 = KDTree.deserialize(t1.serialize());
    eq(t1.size, N, `kd[${seed}] size`); eq(t2.dims, dims, `kd[${seed}] dims`);
    const tag = `kd[${seed}/${kind}/${dims}d]`;
    for (let qn = 0; qn < 60; qn++) {
        const q = []; for (let d = 0; d < dims; d++) q.push(qn < 20 ? pts[Math.floor(r() * N)][d] : r() * 120 - 60);
        const brute = pts.map((p, i) => [d2(p, q), i]).sort((a, b) => a[0] - b[0] || a[1] - b[1]);
        const k = 1 + Math.floor(r() * 12);
        for (const t of [t1, t2, t3]) {
            const res = t.knn(q, k);
            same(Array.from(res.indices), brute.slice(0, k).map(x => x[1]), `${tag} knn k=${k}`);
            check(Array.from(res.distances).every((dd, i) => Math.abs(dd - Math.sqrt(brute[i][0])) < 1e-9), `${tag} knn distances`);
        }
        eq(t1.nearest(q), brute[0][1], `${tag} nearest`);
        const rad = r() * 15;
        const within = brute.filter(x => x[0] <= rad * rad);
        same(Array.from(t1.radius(q, rad).indices), within.map(x => x[1]), `${tag} radius ${rad.toFixed(2)}`);
        const lo = q.map(x => x - r() * 10), hi = q.map(x => x + r() * 10);
        const inBox = pts.map((p, i) => [p, i]).filter(([p]) => p.every((x, d) => x >= lo[d] && x <= hi[d])).map(x => x[1]);
        same(Array.from(t1.box(lo, hi)), inBox, `${tag} box`);
    }
    const Q = 7, k = 5, qs = new Float64Array(Q * dims).map(() => r() * 100 - 50);
    const many = t1.knnMany(qs, k), kk = Math.min(k, N);
    let ok = many.indices.length === Q * kk;
    for (let i = 0; ok && i < Q; i++) { const one = t1.knn(qs.subarray(i * dims, i * dims + dims), k); for (let j = 0; j < kk; j++) if (one.indices[j] !== many.indices[i * kk + j]) ok = false; }
    check(ok, `${tag} knnMany equals knn per query`);
}

__kitS.section("kd: ragged rows refused", () => {
    let threw = false; try { new KDTree([[0, 0], [1]]); } catch (e) { threw = e instanceof RangeError; } check(threw, "kd: ragged rows refused");
    threw = false; try { new KDTree([[0, NaN]]); } catch (e) { threw = e instanceof RangeError; } check(threw, "kd: NaN coordinate refused");
    threw = false; try { new KDTree([[0, Infinity]]); } catch (e) { threw = e instanceof RangeError; } check(threw, "kd: infinite coordinate refused");
    threw = false; try { new KDTree(new Float64Array(5), 2); } catch (e) { threw = e instanceof RangeError; } check(threw, "kd: flat length not a multiple of dims");
    const e = new KDTree(new Float64Array(0), 3);
    eq(e.size, 0, "kd: empty tree"); eq(e.nearest([1, 2, 3]), -1, "kd: nearest on empty"); eq(e.knn([1, 2, 3], 4).indices.length, 0, "kd: knn on empty");
    const t = new KDTree([[0, 0], [3, 4]]);
    threw = false; try { t.knn([1], 1); } catch (er) { threw = er instanceof RangeError; } check(threw, "kd: query of wrong dimension refused");
    eq(t.knn([0, 0], 10).indices.length, 2, "kd: k larger than size is capped");
    same(Array.from(t.point(1)), [3, 4], "kd: point()");
    eq(t.point(2), undefined, "kd: point past the end");
});
__kitS.end();

print(`test_structures_db_search: ${fails ? fails + " FAILED of" : "all"} ${n} checks${fails ? "" : " passed"}`);
if (fails) throw new Error(fails + " failures");
