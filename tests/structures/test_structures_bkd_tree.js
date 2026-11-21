// flags: --std
// timeout: 300
// covers: dyna:structures.BKDTree
// BKDTree (Lucene PointValues-style block k-d tree: f64/i64 points in 1..4 dims, ip, latlon) against
// brute-force scans. Key order is Lucene's: f64 by NumericUtils.doubleToSortableLong (-Inf < ... < -0 < +0 < ... < +Inf < NaN,
// every NaN canonical), i64 signed. Every query is compared with a scan of the same points on random, clustered,
// duplicate-heavy, all-equal and special-value data, at sizes around the leaf size (leaf 2..512) so INSIDE subtrees
// (bulk-collected), CROSSES leaves (per-point check) and OUTSIDE cells all occur. Inverted ranges (lo > hi in key
// order) are empty. Persistence: serialize/deserialize and save/load round trips are byte-identical; every truncation and
// every CRC-resealed bit flip of a record is refused or yields a tree that answers like a scan of its own points.
// ip: 16-byte keys, IPv4 mapped to ::ffff:a.b.c.d (Lucene InetAddressPoint); cidr() vs a BigInt prefix scan,
// ::/0, 0.0.0.0/0, /32, /128. latlon: box with and without antimeridian crossing, poles, and Haversine distance
// (R = 6371008.7714 m, the formula in dynajs.d.ts); points within 1e-6 m of the radius are not compared (libm
// last-bit differences). Records written before ip/latlon existed (X1) still load.
import { suite } from "../lib/kit.js";
import * as std from "std";
import { BKDTree, RoaringBitmap } from "dyna:structures";
import { Path } from "dyna:file";
import { CRC32C } from "dyna:hash";

const t = suite("bkd_tree");

function rng(seed) { let s = seed >>> 0 || 1; return () => { s ^= s << 13; s >>>= 0; s ^= s >>> 17; s ^= s << 5; s >>>= 0; return s / 4294967296; }; }
const dv = new DataView(new ArrayBuffer(8));
const M64 = (1n << 64n) - 1n;
function fkey(x) { if (x !== x) return 0xFFF8000000000000n; dv.setFloat64(0, x); const u = dv.getBigUint64(0); return (u >> 63n) ? (~u & M64) : (u ^ (1n << 63n)); }
const ikey = (x) => BigInt.asIntN(64, BigInt(x));
const ids = (bm) => Array.from(bm.toArray());
function bruteIds(n, docs, pred) { const s = new Set(); for (let i = 0; i < n; i++) if (pred(i)) s.add(docs ? docs[i] : i); return Array.from(s).sort((a, b) => a - b); }

function genF64(r, n, dims, kind) {
    const out = [];
    for (let i = 0; i < n; i++) {
        const p = [];
        for (let d = 0; d < dims; d++) {
            if (kind === "uniform") p.push(r() * 2000 - 1000);
            else if (kind === "clustered") p.push((i % 7) * 100 + r() * 0.5);
            else if (kind === "dups") p.push(Math.floor(r() * 5));
            else if (kind === "equal") p.push(3.25);
            else p.push([-0, 0, NaN, Infinity, -Infinity, 1e-300, -1e-300, 5e-324, 1][Math.floor(r() * 9)]);
        }
        out.push(p);
    }
    return out;
}

t.test("f64 range: differential vs brute force across data shapes, dims and leaf sizes", (a) => {
    let seed = 1;
    for (const kind of ["uniform", "clustered", "dups", "equal", "special"])
        for (const dims of [1, 2, 3, 4])
            for (const [n, leaf] of [[1, 512], [2, 2], [17, 2], [511, 512], [512, 512], [513, 512], [1500, 16], [3000, 3]]) {
                if (kind === "special" && n > 600) continue;
                const r = rng(seed++);
                const pts = genF64(r, n, dims, kind);
                const flat = new Float64Array(n * dims); pts.forEach((p, i) => p.forEach((x, d) => flat[i * dims + d] = x));
                const tree = dims === 1 && r() < 0.5 ? new BKDTree(pts.map((p) => p[0]), { leafSize: leaf }) : r() < 0.5 ? new BKDTree(pts, { leafSize: leaf }) : new BKDTree(flat, { dims, leafSize: leaf });
                a.eq(tree.size, n, `size ${kind}/${dims}/${n}`); a.eq(tree.dims, dims, "dims");
                const keys = pts.map((p) => p.map(fkey));
                let bad = 0;
                for (let q = 0; q < 25; q++) {
                    const lo = [], hi = [];
                    for (let d = 0; d < dims; d++) {
                        let x = pts[Math.floor(r() * n)][d], y = pts[Math.floor(r() * n)][d];
                        if (q % 5 === 0) y = x;
                        if (q % 7 === 1) { x = -Infinity; }
                        if (q % 9 === 2) { y = NaN; }
                        if (fkey(x) > fkey(y) && q % 4 !== 3) [x, y] = [y, x];
                        lo.push(x); hi.push(y);
                    }
                    const lk = lo.map(fkey), hk = hi.map(fkey);
                    const want = bruteIds(n, null, (i) => keys[i].every((k, d) => k >= lk[d] && k <= hk[d]));
                    const got = ids(dims === 1 && q % 2 ? tree.range(lo[0], hi[0]) : tree.range(lo, hi));
                    if (JSON.stringify(got) !== JSON.stringify(want)) { if (bad++ < 3) a.deep(got, want, `range ${kind} dims=${dims} n=${n} leaf=${leaf} lo=${lo} hi=${hi}`); }
                    const cnt = keys.filter((k) => k.every((v, d) => v >= lk[d] && v <= hk[d])).length;
                    if (tree.rangeCount(lo, hi) !== cnt && bad++ < 3) a.eq(tree.rangeCount(lo, hi), cnt, "rangeCount counts points");
                }
                a.eq(bad, 0, `mismatches ${kind}/${dims}/${n}/${leaf}`);
            }
});

t.test("f64 key order follows Lucene: -0 below +0, NaN above +Infinity, NaN payloads canonical", (a) => {
    const nan2 = new Float64Array(new BigUint64Array([0xFFF8000000000001n]).buffer)[0];
    const v = [-Infinity, -1, -0, 0, 1, Infinity, NaN, nan2];
    const tr = new BKDTree(v, { leafSize: 2 });
    a.deep(ids(tr.range(-0, -0)), [2], "-0 alone");
    a.deep(ids(tr.range(0, 0)), [3], "+0 alone");
    a.deep(ids(tr.range(-0, 0)), [2, 3], "both zeros");
    a.deep(ids(tr.range(0, -0)), [], "lo above hi is empty");
    a.deep(ids(tr.range(-Infinity, Infinity)), [0, 1, 2, 3, 4, 5], "NaN above +Infinity");
    a.deep(ids(tr.range(NaN, NaN)), [6, 7], "every NaN is the same key");
    a.deep(ids(tr.range(Infinity, NaN)), [5, 6, 7], "+Infinity..NaN");
    a.deep(ids(tr.range(-Infinity, NaN)), [0, 1, 2, 3, 4, 5, 6, 7], "everything");
});

t.test("i64 range: BigInt64Array, bigint and number inputs, extremes, infinite bounds", (a) => {
    const r = rng(77);
    for (const [n, leaf] of [[1, 2], [300, 4], [1025, 512], [4000, 32]]) {
        const vals = [];
        for (let i = 0; i < n; i++) {
            const k = r();
            vals.push(k < 0.05 ? -(2n ** 63n) : k < 0.1 ? 2n ** 63n - 1n : k < 0.3 ? BigInt(Math.floor(r() * 10) - 5) : BigInt.asIntN(64, BigInt(Math.floor(r() * 2 ** 32)) << 32n | BigInt(Math.floor(r() * 2 ** 32))));
        }
        const trees = [new BKDTree(new BigInt64Array(vals), { type: "i64", leafSize: leaf }), new BKDTree(vals, { type: "i64", leafSize: leaf })];
        let bad = 0;
        for (let q = 0; q < 60; q++) {
            let lo = vals[Math.floor(r() * n)], hi = vals[Math.floor(r() * n)];
            if (lo > hi && q % 6) [lo, hi] = [hi, lo];
            if (q % 10 === 0) hi = lo;
            const want = bruteIds(n, null, (i) => vals[i] >= lo && vals[i] <= hi);
            for (const tr of trees) { const got = ids(tr.range(lo, hi)); if (JSON.stringify(got) !== JSON.stringify(want) && bad++ < 3) a.deep(got, want, `i64 ${lo}..${hi}`); }
        }
        a.eq(bad, 0, `i64 mismatches n=${n}`);
        const tr = trees[0];
        a.deep(ids(tr.range(-Infinity, Infinity)), bruteIds(n, null, () => true), "infinite bounds cover int64");
        a.deep(ids(tr.range(-(2n ** 63n), -(2n ** 63n))), bruteIds(n, null, (i) => vals[i] === -(2n ** 63n)), "INT64_MIN exact");
        a.deep(ids(tr.range(2n ** 63n - 1n, Infinity)), bruteIds(n, null, (i) => vals[i] === 2n ** 63n - 1n), "INT64_MAX exact");
        a.deep(ids(tr.range(-1, 1)), bruteIds(n, null, (i) => vals[i] >= -1n && vals[i] <= 1n), "number bounds");
    }
    const two = new BKDTree([[1, 2n], [3n, -4]], { type: "i64" });
    a.deep(ids(two.range([0, -10], [5, 0])), [1], "2-d mixed bigint/number rows");
    a.throws(() => new BKDTree([1.5], { type: "i64" }), RangeError, "fractional i64 refused");
    a.throws(() => new BKDTree([2n ** 63n], { type: "i64" }), RangeError, "bigint above int64 refused");
    a.throws(() => new BKDTree([-(2n ** 63n) - 1n], { type: "i64" }), RangeError, "bigint below int64 refused");
    a.throws(() => new BKDTree([2 ** 63], { type: "i64" }), RangeError, "number 2^63 refused");
    a.throws(() => new BKDTree(["1"], { type: "i64" }), TypeError, "string refused");
    a.throws(() => two.range([0.5, 0], [1, 1]), RangeError, "fractional bound refused");
});

function v4(x) { return [x >>> 24, (x >>> 16) & 255, (x >>> 8) & 255, x & 255].join("."); }
function v6(b) { const g = []; for (let i = 0; i < 8; i++) g.push(((b >> BigInt(112 - 16 * i)) & 0xFFFFn).toString(16)); return g.join(":"); }
const MAPPED = 0xFFFFn << 32n;

t.test("ip: cidr and range vs brute force over IPv4 and IPv6, /0 and /128 edges", (a) => {
    const r = rng(9);
    for (const [n, leaf] of [[1, 2], [700, 8], [2000, 512]]) {
        const strs = [], big = [];
        for (let i = 0; i < n; i++) {
            if (r() < 0.5) { const x = r() < 0.5 ? (10 << 24 | Math.floor(r() * 65536)) >>> 0 : Math.floor(r() * 2 ** 32); strs.push(v4(x)); big.push(MAPPED | BigInt(x)); }
            else { let b = 0n; for (let k = 0; k < 4; k++) b = (b << 32n) | BigInt(Math.floor(r() * 2 ** 32)); if (r() < 0.3) b = (0x20010db8n << 96n) | (b & ((1n << 64n) - 1n)); if (r() < 0.05) b = 0n; strs.push(v6(b)); big.push(b); }
        }
        const tr = new BKDTree(strs, { type: "ip", leafSize: leaf });
        let bad = 0;
        const check = (got, want, m) => { if (JSON.stringify(got) !== JSON.stringify(want) && bad++ < 3) a.deep(got, want, m); };
        for (let q = 0; q < 80; q++) {
            const base = big[Math.floor(r() * n)];
            const isv4 = (base >> 32n) === 0xFFFFn;
            const bits = q < 4 ? [0, isv4 ? 32 : 128, 1, isv4 ? 31 : 127][q] : Math.floor(r() * (isv4 ? 33 : 129));
            const full = isv4 ? bits + 96 : bits;
            const mask = full === 0 ? 0n : ((1n << 128n) - 1n) ^ ((1n << BigInt(128 - full)) - 1n);
            const net = base & mask;
            const pfx = (isv4 ? v4(Number(net & 0xFFFFFFFFn)) : v6(net)) + "/" + bits;
            check(ids(tr.cidr(pfx)), bruteIds(n, null, (i) => (big[i] & mask) === net), `cidr ${pfx}`);
            let lo = big[Math.floor(r() * n)], hi = big[Math.floor(r() * n)];
            if (lo > hi) [lo, hi] = [hi, lo];
            const fmt = (b) => (b >> 32n) === 0xFFFFn ? v4(Number(b & 0xFFFFFFFFn)) : v6(b);
            check(ids(tr.range(fmt(lo), fmt(hi))), bruteIds(n, null, (i) => big[i] >= lo && big[i] <= hi), `ip range ${fmt(lo)}..${fmt(hi)}`);
        }
        a.eq(bad, 0, `ip mismatches n=${n}`);
        a.deep(ids(tr.cidr("::/0")), bruteIds(n, null, () => true), "::/0 is everything");
        a.deep(ids(tr.cidr("0.0.0.0/0")), bruteIds(n, null, (i) => (big[i] >> 32n) === 0xFFFFn), "0.0.0.0/0 is every IPv4 address only");
    }
    const tr = new BKDTree(["1.2.3.4", "::ffff:1.2.3.4", "1.2.3.5"], { type: "ip" });
    a.deep(ids(tr.cidr("1.2.3.4/32")), [0, 1], "IPv4 and its mapped IPv6 form are one key");
    a.deep(ids(tr.cidr("1.2.3.7/30")), [0, 1, 2], "host bits of the prefix are masked");
    a.throws(() => tr.cidr("1.2.3.4/33"), RangeError, "bad prefix length");
    a.throws(() => tr.cidr("1.2.3.4"), RangeError, "missing prefix length");
    a.throws(() => new BKDTree(["1.2.3"], { type: "ip" }), RangeError, "bad address");
    a.throws(() => new BKDTree([5], { type: "ip" }), TypeError, "number refused");
    a.throws(() => new BKDTree([1, 2]).cidr("::/0"), TypeError, "cidr needs an ip tree");
});

function hav(lat1, lon1, lat2, lon2) {
    const r = Math.PI / 180.0, p1 = lat1 * r, p2 = lat2 * r;
    const s1 = Math.sin((p2 - p1) / 2.0), s2 = Math.sin((lon2 - lon1) * r / 2.0);
    let h = s1 * s1 + Math.cos(p1) * Math.cos(p2) * s2 * s2;
    if (h > 1.0) h = 1.0;
    return 2.0 * 6371008.7714 * Math.asin(Math.sqrt(h));
}

function genGeo(r, n, kind) {
    const out = [];
    for (let i = 0; i < n; i++) {
        if (kind === "uniform") out.push([r() * 180 - 90, r() * 360 - 180]);
        else if (kind === "poles") out.push([(r() < 0.5 ? -1 : 1) * (90 - r() * 3), r() * 360 - 180]);
        else if (kind === "dateline") out.push([r() * 20 - 10, (r() < 0.5 ? 180 - r() * 2 : -180 + r() * 2)]);
        else if (kind === "edges") out.push([[90, -90, 0, 45][Math.floor(r() * 4)], [180, -180, 0, 179.999999][Math.floor(r() * 4)]]);
        else out.push([40 + (i % 5) + r() * 0.01, -74 + r() * 0.01]);
    }
    return out;
}

t.test("latlon box: normal, antimeridian-crossing, polar and degenerate boxes vs brute force", (a) => {
    const r = rng(31);
    for (const kind of ["uniform", "poles", "dateline", "edges", "city"])
        for (const [n, leaf] of [[1, 2], [600, 4], [3000, 512]]) {
            const pts = genGeo(r, n, kind);
            const tr = new BKDTree(pts, { type: "latlon", leafSize: leaf });
            let bad = 0;
            for (let q = 0; q < 60; q++) {
                const c = pts[Math.floor(r() * n)];
                let minLat = Math.max(-90, c[0] - r() * 30), maxLat = Math.min(90, c[0] + r() * 30);
                let minLon = c[1] - r() * 40, maxLon = c[1] + r() * 40;
                if (minLon < -180) minLon += 360;
                if (maxLon > 180) maxLon -= 360;
                if (q % 8 === 0) { minLat = -90; maxLat = 90; minLon = 170; maxLon = -170; }
                if (q % 8 === 1) { minLat = c[0]; maxLat = c[0]; minLon = c[1]; maxLon = c[1]; }
                if (q % 8 === 2) { minLat = 89; maxLat = 90; minLon = -180; maxLon = 180; }
                const cross = minLon > maxLon;
                const want = minLat > maxLat ? [] : bruteIds(n, null, (i) => pts[i][0] >= minLat && pts[i][0] <= maxLat && (cross ? (pts[i][1] >= minLon || pts[i][1] <= maxLon) : (pts[i][1] >= minLon && pts[i][1] <= maxLon)));
                const got = ids(tr.box(minLat, maxLat, minLon, maxLon));
                if (JSON.stringify(got) !== JSON.stringify(want) && bad++ < 3) a.deep(got, want, `box ${kind} n=${n} [${minLat},${maxLat}]x[${minLon},${maxLon}]`);
            }
            a.eq(bad, 0, `box mismatches ${kind} n=${n}`);
        }
    const tr = new BKDTree(new Float64Array([0, 0, 1, 1]), { type: "latlon" });
    a.deep(ids(tr.box(5, -5, -180, 180)), [], "minLat > maxLat is empty");
    a.throws(() => tr.box(-91, 0, 0, 0), RangeError, "latitude range checked");
    a.throws(() => tr.box(0, 0, 0, 181), RangeError, "longitude range checked");
    a.throws(() => new BKDTree([[91, 0]], { type: "latlon" }), RangeError, "point latitude checked");
    a.throws(() => new BKDTree([[0, NaN]], { type: "latlon" }), RangeError, "NaN longitude refused");
    a.throws(() => new BKDTree([[0, 0, 0]], { type: "latlon" }), RangeError, "latlon is 2-d");
    a.throws(() => new BKDTree([1, 2]).box(0, 1, 0, 1), TypeError, "box needs a latlon tree");
});

t.test("latlon distance: Haversine radius vs brute force near poles, the antimeridian and at huge radii", (a) => {
    const r = rng(47);
    for (const kind of ["uniform", "poles", "dateline", "edges", "city"])
        for (const [n, leaf] of [[1, 2], [800, 8], [3000, 512]]) {
            const pts = genGeo(r, n, kind);
            const tr = new BKDTree(pts, { type: "latlon", leafSize: leaf });
            let bad = 0;
            for (let q = 0; q < 50; q++) {
                const c = q % 3 ? pts[Math.floor(r() * n)] : [r() * 180 - 90, r() * 360 - 180];
                const lat = q % 11 === 5 ? 90 : q % 11 === 6 ? -90 : c[0], lon = q % 13 === 7 ? 180 : c[1];
                const m = q % 10 === 0 ? 0 : q % 10 === 1 ? 2.1e7 : q % 10 === 2 ? 1.9e7 : Math.pow(10, 1 + r() * 6.5);
                const d = pts.map((p) => hav(lat, lon, p[0], p[1]));
                const want = bruteIds(n, null, (i) => d[i] <= m), got = ids(tr.distance(lat, lon, m));
                const amb = new Set(); d.forEach((x, i) => { if (Math.abs(x - m) < 1e-6) amb.add(i); });
                const g2 = got.filter((i) => !amb.has(i)), w2 = want.filter((i) => !amb.has(i));
                if (JSON.stringify(g2) !== JSON.stringify(w2) && bad++ < 3) a.deep(g2, w2, `distance ${kind} n=${n} (${lat},${lon}) r=${m}`);
            }
            a.eq(bad, 0, `distance mismatches ${kind} n=${n}`);
        }
    const tr = new BKDTree([[10, 20], [10, 20.0000001]], { type: "latlon" });
    a.deep(ids(tr.distance(10, 20, 0)), [0], "radius 0 matches the exact point only");
    a.throws(() => tr.distance(0, 0, -1), RangeError, "negative radius");
    a.throws(() => tr.distance(0, 0, Infinity), RangeError, "infinite radius");
    a.throws(() => tr.distance(95, 0, 1), RangeError, "latitude checked");
});

t.test("edges: single point, all-equal points, inverted ranges, 2-d box", (a) => {
    const one = new BKDTree([42]);
    a.deep(ids(one.range(42, 42)), [0], "single point exact"); a.deep(ids(one.range(43, 50)), [], "single point miss");
    const eqv = new BKDTree(new Float64Array(2000).fill(7), { leafSize: 8 });
    a.eq(eqv.range(7, 7).size, 2000, "all-equal inside"); a.eq(eqv.range(7.000001, 8).size, 0, "all-equal outside");
    a.eq(eqv.range(8, 6).size, 0, "inverted range is empty");
    const g = new BKDTree([[0, 0], [1, 1], [2, 2], [1, 5]], { leafSize: 2 });
    a.deep(ids(g.range([0, 0], [1, 5])), [0, 1, 3], "2-d box"); a.deep(ids(g.range([2, 0], [0, 5])), [], "inverted on one dim");
    a.deep(ids(g.range([-Infinity, 1], [Infinity, 1])), [1], "open bounds on one dim");
});

t.test("docIds: multi-valued documents, dedup in results, rangeCount counts points", (a) => {
    const vals = [1, 2, 3, 4, 5, 6], docs = [7, 7, 9, 9, 4000000000, 0];
    const tr = new BKDTree(vals, { docIds: docs, leafSize: 2 });
    a.deep(ids(tr.range(1, 4)), [7, 9], "duplicates collapse");
    a.eq(tr.rangeCount(1, 4), 4, "rangeCount counts points");
    a.deep(ids(tr.range(5, 6)), [0, 4000000000], "full uint32 doc ids");
    a.deep(ids(new BKDTree(vals, { docIds: new Uint32Array(docs) }).range(0, 9)), [0, 7, 9, 4000000000], "typed docIds");
    a.throws(() => new BKDTree(vals, { docIds: [1, 2] }), RangeError, "docIds length must match");
    a.throws(() => new BKDTree(vals, { docIds: [1, 2, 3, 4, 5, -1] }), RangeError, "negative doc id refused");
});

t.test("argument validation", (a) => {
    a.throws(() => new BKDTree([1], { type: "f32" }), TypeError, "unknown type");
    a.throws(() => new BKDTree([1], { dims: 5 }), RangeError, "dims above 4");
    a.throws(() => new BKDTree([1], { dims: 0 }), RangeError, "dims 0");
    a.throws(() => new BKDTree([1], { leafSize: 1 }), RangeError, "leafSize 1");
    a.throws(() => new BKDTree([1], { leafSize: 65537 }), RangeError, "leafSize above 65536");
    a.throws(() => new BKDTree([[1, 2], [3]]), RangeError, "ragged rows");
    a.throws(() => new BKDTree([1, 2, 3], { dims: 2 }), RangeError, "flat length not a multiple of dims");
    a.throws(() => new BKDTree([[1, 2, 3, 4, 5]]), RangeError, "5-d rows");
    a.throws(() => new BKDTree(["x"]), TypeError, "string in f64");
    a.throws(() => new BKDTree("abc"), TypeError, "string values");
    a.throws(() => new BKDTree([1], 5), TypeError, "options not an object");
    const tr = new BKDTree([[1, 2], [3, 4]]);
    a.throws(() => tr.range(1, 2), TypeError, "scalar bound on a 2-d tree");
    a.throws(() => tr.range([1], [2]), RangeError, "bound of the wrong length");
    a.throws(() => tr.range([1, "a"], [2, 3]), TypeError, "non-number bound");
    const e = new BKDTree([]);
    a.deep(ids(BKDTree.deserialize(e.serialize()).range(-Infinity, Infinity)), [], "empty round trip");
    a.eq(e.size, 0, "empty tree"); a.eq(e.range(-Infinity, NaN).size, 0, "empty range"); a.eq(e.rangeCount(0, 1), 0, "empty count");
    a.eq(new BKDTree(new Float64Array(0), { dims: 3 }).dims, 3, "empty with dims");
    a.eq(tr.type, "f64", "type getter"); a.eq(tr.leafSize, 512, "default leafSize");
    a.ok(tr.range([0, 0], [9, 9]) instanceof RoaringBitmap, "results are RoaringBitmap");
});

function reseal(c) { const v = CRC32C(c.subarray(0, c.length - 4)) >>> 0; c[c.length - 4] = v & 255; c[c.length - 3] = (v >>> 8) & 255; c[c.length - 2] = (v >>> 16) & 255; c[c.length - 1] = v >>> 24; return c; }
function eqBytes(x, y) { if (x.length !== y.length) return false; for (let i = 0; i < x.length; i++) if (x[i] !== y[i]) return false; return true; }

t.test("persistence: round trips are byte-identical and answer identically; save/load", (a) => {
    const r = rng(5);
    const trees = [
        new BKDTree(genF64(r, 2000, 3, "uniform"), { leafSize: 16 }),
        new BKDTree(new BigInt64Array(500).map((_, i) => BigInt(i * 7919 % 1000) - 500n), { type: "i64", leafSize: 4 }),
        new BKDTree(genF64(r, 1000, 1, "dups"), { docIds: Array.from({ length: 1000 }, (_, i) => i >> 1) }),
        new BKDTree([], { type: "i64" }),
        new BKDTree(Array.from({ length: 300 }, (_, i) => i % 3 ? v4((i * 2654435761) >>> 0) : v6(BigInt(i) * 0x9E3779B97F4A7C15n << 40n)), { type: "ip", leafSize: 8 }),
        new BKDTree(genGeo(r, 1000, "uniform"), { type: "latlon", docIds: Array.from({ length: 1000 }, (_, i) => i >> 1) }),
    ];
    const dir = std.getenv("TMPDIR") || ".";
    for (const tr of trees) {
        const b = tr.serialize(), t2 = BKDTree.deserialize(b);
        a.ok(eqBytes(t2.serialize(), b), `${tr.type} serialize is stable`);
        a.eq(t2.size, tr.size, "size kept"); a.eq(t2.leafSize, tr.leafSize, "leafSize kept"); a.eq(t2.type, tr.type, "type kept");
        const p = new Path(dir + "/bkd_" + tr.type + ".dyns");
        tr.save(p, { fsync: false });
        const t3 = BKDTree.load(p), t4 = BKDTree.load(p, { mmap: true });
        a.ok(eqBytes(t3.serialize(), b) && eqBytes(t4.serialize(), b), `${tr.type} save/load identical`);
        if (tr.type === "latlon") a.deep(ids(t3.distance(0, 0, 5e6)), ids(tr.distance(0, 0, 5e6)), "loaded geo answers");
        else if (tr.type === "ip") a.deep(ids(t3.cidr("0.0.0.0/2")), ids(tr.cidr("0.0.0.0/2")), "loaded ip answers");
        else if (tr.dims === 3) a.deep(ids(t3.range([-500, -500, -500], [500, 0, 500])), ids(tr.range([-500, -500, -500], [500, 0, 500])), "loaded f64 answers");
        else if (tr.type === "f64") a.deep(ids(t3.range(1, 3)), ids(tr.range(1, 3)), "loaded multi-valued answers");
        else a.deep(ids(t3.range(-100, 100)), ids(tr.range(-100, 100)), "loaded i64 answers");
    }
});

t.test("persistence: every truncation and corruption is refused or still answers like brute force", (a) => {
    const r = rng(12);
    const pts = genF64(r, 40, 2, "uniform");
    const tr = new BKDTree(pts, { leafSize: 4 });
    const b = tr.serialize();
    let crashes = 0, accepted = 0, wrong = 0;
    for (let cut = 0; cut < b.length; cut++) {
        try { BKDTree.deserialize(b.subarray(0, cut)); accepted++; } catch (e) { if (!(e instanceof TypeError || e instanceof RangeError)) crashes++; }
    }
    for (const [off, v, what] of [[20, 2, "record version 2"], [21, 2, "unknown type"], [22, 0, "dims 0"], [22, 5, "dims 5"], [23, 1, "leafSize 1"]]) {
        const c = b.slice(); c[off] = v; reseal(c);
        a.throws(() => BKDTree.deserialize(c), TypeError, "forged " + what + " refused");
    }
    a.eq(accepted, 0, "no truncated record is accepted"); a.eq(crashes, 0, "truncations raise TypeError/RangeError");
    const keyOf = (c, off) => { let u = 0n; for (let i = 0; i < 8; i++) u = (u << 8n) | BigInt(c[off + i]); return u; };
    const lo = [-300, -800], hi = [400, 200], lk = lo.map(fkey), hk = hi.map(fkey);
    for (let k = 0; k < 3000; k++) {
        const c = b.slice();
        const pos = Math.floor(r() * c.length);
        c[pos] ^= 1 << Math.floor(r() * 8);
        reseal(c);
        let t2;
        try { t2 = BKDTree.deserialize(c); } catch (e) { if (!(e instanceof TypeError || e instanceof RangeError)) crashes++; continue; }
        if (t2.size !== pts.length || t2.dims !== 2 || t2.leafSize !== 4) continue;
        const kOff = c.length - 4 - 40 * 4 - 40 * 16, dOff = c.length - 4 - 40 * 4;
        const want = new Set();
        for (let i = 0; i < 40; i++) {
            const k0 = keyOf(c, kOff + i * 16), k1 = keyOf(c, kOff + i * 16 + 8);
            if (k0 >= lk[0] && k0 <= hk[0] && k1 >= lk[1] && k1 <= hk[1]) want.add((c[dOff + i * 4] | c[dOff + i * 4 + 1] << 8 | c[dOff + i * 4 + 2] << 16 | c[dOff + i * 4 + 3] << 24) >>> 0);
        }
        const w = Array.from(want).sort((x, y) => x - y), got = ids(t2.range(lo, hi));
        if (JSON.stringify(w) !== JSON.stringify(got) && wrong++ < 3) a.deep(got, w, "accepted corrupted record at byte " + pos);
        accepted++;
    }
    a.eq(crashes, 0, "corruptions raise TypeError/RangeError only");
    a.eq(wrong, 0, "an accepted corrupted tree answers like a scan of its own points");
    a.ok(accepted > 0, "some corruptions (key/doc bytes inside the cells) are accepted and checked");
});

t.test("persistence: records written by the X1 build (f64/i64 only) still load and answer the same", (a) => {
    const unhex = (h) => new Uint8Array(h.match(/../g).map((x) => parseInt(x, 16)));
    const f = BKDTree.deserialize(unhex("44594e53010033000000000093000000000000000100020200000005000000000101000000000000000000c000000000000000c000000000000000c01c000000000000c0080000000000007fffffffffffffffbff0000000000000c000000000000000c000000000000000c000000000000000c01c000000000000c01c000000000000fff8000000000000c0140000000000000800000009000000050000000600000007000000c80bfbc5"));
    const now = new BKDTree([[1, 2], [3, -0], [NaN, 5], [7, 7], [2, 2]], { leafSize: 2, docIds: [9, 8, 7, 6, 5] });
    a.eq(f.type, "f64", "old f64 type"); a.eq(f.size, 5, "old f64 size");
    a.ok(eqBytes(f.serialize(), now.serialize()), "old f64 record equals today's record for the same input");
    a.deep(ids(f.range([1, -0], [3, 2])), [5, 8, 9], "old f64 record answers");
    const i = BKDTree.deserialize(unhex("44594e530100330000000000410000000000000001010102000000030000000000000000000000000080000000000000007ffffffffffffffb8000000000000000ffffffffffffffff0000000002000000010000009bd930d9"));
    a.eq(i.type, "i64", "old i64 type");
    a.deep(ids(i.range(-Infinity, 0)), [0, 2], "old i64 record answers");
});

t.test("ip and latlon edges: 0.0.0.0, malformed prefixes, forged records", (a) => {
    const tr = new BKDTree(["0.0.0.0", "::", "255.255.255.255", "ffff:ffff:ffff:ffff:ffff:ffff:ffff:ffff", "::ffff:0.0.0.1"], { type: "ip", leafSize: 2 });
    a.deep(ids(tr.cidr("0.0.0.0/32")), [0], "0.0.0.0/32 is not ::");
    a.deep(ids(tr.cidr("::/128")), [1], "::/128 is not 0.0.0.0");
    a.deep(ids(tr.cidr("0.0.0.0/31")), [0, 4], "0.0.0.0/31");
    a.deep(ids(tr.cidr("::/0")), [0, 1, 2, 3, 4], "::/0");
    a.deep(ids(tr.range("0.0.0.0", "255.255.255.255")), [0, 2, 4], "IPv4 range over mapped keys");
    a.deep(ids(tr.range("::", "::")), [1], "range of one IPv6 key");
    a.deep(ids(tr.range("255.255.255.255", "0.0.0.0")), [], "inverted ip range");
    for (const bad of ["1.2.3.4/-1", "/8", "1.2.3.4/ 8", "::/129", "10.0.0.0/08", "1.2.3.4/33", "1.2.3/8", "zz::/8", "1.2.3.4/8/8", ""])
        a.throws(() => tr.cidr(bad), RangeError, "malformed prefix " + JSON.stringify(bad));
    for (const bad of [8, null, undefined, ["10.0.0.0/8"]]) a.throws(() => tr.cidr(bad), TypeError, "non-string prefix " + String(bad));
    a.throws(() => tr.range(1, 2), TypeError, "number bound on an ip tree");
    a.throws(() => new BKDTree(["1.2.3.4"], { type: "ip", dims: 2 }), RangeError, "ip is 1-d");
    a.throws(() => new BKDTree(["fe80::1%eth0"], { type: "ip" }), RangeError, "zone ids refused");
    const g = new BKDTree([[90, 180], [-90, -180], [0, 180], [0, -180]], { type: "latlon", leafSize: 2 });
    a.deep(ids(g.distance(90, 0, 1e-3)), [0], "north pole matches at any longitude (the formula gives 7.8e-10 m there, so radius 0 does not)");
    a.deep(ids(g.distance(0, -180, 1e-3)), [2, 3], "lon 180 and -180 are one meridian");
    a.deep(ids(g.distance(0, -180, 0)), [3], "radius 0 is exact under the documented formula");
    a.deep(ids(g.distance(0, 0, 2.1e7)), [0, 1, 2, 3], "radius above half circumference matches everything");
    a.deep(ids(g.box(-90, 90, 180, 180)), [0, 2], "box on lon 180 exactly");
    const hack = new BKDTree([[100, 0], [0, 0]]).serialize();
    hack[21] = 3; reseal(hack);
    a.throws(() => BKDTree.deserialize(hack), TypeError, "forged latlon record with latitude 100 refused");
    const ip1 = new BKDTree(["1.2.3.4"], { type: "ip" }).serialize();
    ip1[22] = 2; reseal(ip1);
    a.throws(() => BKDTree.deserialize(ip1), TypeError, "forged 2-d ip record refused");
});

t.test("async build: BKDTree.build resolves to the same tree and rejects bad input", async (a) => {
    const r = rng(99);
    const pts = genF64(r, 20000, 2, "clustered");
    const flat = new Float64Array(pts.flat());
    const p = BKDTree.build(flat, { dims: 2, leafSize: 64 });
    a.ok(p instanceof Promise, "build returns a Promise");
    const t1 = await p, t2 = new BKDTree(flat, { dims: 2, leafSize: 64 });
    for (let q = 0; q < 50; q++) { const c = pts[Math.floor(r() * pts.length)], w = r() * 50; const lo = [c[0] - w, c[1] - w], hi = [c[0] + w, c[1] + w]; if (JSON.stringify(ids(t1.range(lo, hi))) !== JSON.stringify(ids(t2.range(lo, hi)))) a.ok(false, "async and sync builds answer alike"); }
    a.deep(ids(t1.range([0, 0], [250, 250])), ids(t2.range([0, 0], [250, 250])), "same answers");
    await a.rejects(BKDTree.build([1], { type: "nope" }), TypeError, "bad options reject");
    await a.rejects(BKDTree.build([[1, 2], [3]]), RangeError, "ragged rows reject");
    const many = await Promise.all([1, 2, 3, 4].map((k) => BKDTree.build(new Float64Array(5000).map((_, i) => i * k))));
    a.deep(many.map((x) => x.rangeCount(0, 4999)), [5000, 2500, 1667, 1250], "concurrent builds");
});

await t.run();
