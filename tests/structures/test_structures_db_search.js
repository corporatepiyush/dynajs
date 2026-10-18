// flags: --std
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
{
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
}

// ---- ConsistentHash ---------------------------------------------------------
{
    const names = ["cache-a", "cache-b", "cache-c", "cache-d", "cache-e"];
    const ch = new ConsistentHash();
    names.forEach(nm => ch.addNode(nm));
    const K = 50000, owner = [], cnt = {};
    for (let i = 0; i < K; i++) { const o = ch.get("user:" + i); owner.push(o); cnt[o] = (cnt[o] || 0) + 1; }
    for (const nm of names) check(Math.abs(cnt[nm] / K - 1 / names.length) < 0.06, `ch balance ${nm} has ${(cnt[nm] / K * 100).toFixed(1)}% of keys`);
    const rev = new ConsistentHash(); [...names].reverse().forEach(nm => rev.addNode(nm));
    let diff = 0; for (let i = 0; i < 5000; i++) if (rev.get("user:" + i) !== owner[i]) diff++;
    eq(diff, 0, "ch placement does not depend on insertion order");
    ch.addNode("cache-f");
    let moved = 0, wrongDest = 0;
    for (let i = 0; i < K; i++) { const o = ch.get("user:" + i); if (o !== owner[i]) { moved++; if (o !== "cache-f") wrongDest++; } }
    eq(wrongDest, 0, "ch adding a node only moves keys to the new node");
    check(Math.abs(moved / K - 1 / 6) < 0.05, `ch adding a node moves about 1/6 of keys (${(moved / K * 100).toFixed(1)}%)`);
    ch.removeNode("cache-f");
    let back = 0; for (let i = 0; i < K; i++) if (ch.get("user:" + i) !== owner[i]) back++;
    eq(back, 0, "ch removing the node restores the original placement");
    ch.removeNode("cache-c");
    let wrongMove = 0; for (let i = 0; i < K; i++) { const o = ch.get("user:" + i); if (owner[i] !== "cache-c" && o !== owner[i]) wrongMove++; }
    eq(wrongMove, 0, "ch removing a node only moves that node's keys");
    ch.addNode("cache-c");
    for (let i = 0; i < 200; i++) {
        const rep = ch.getN("k" + i, 3);
        check(rep.length === 3 && new Set(rep).size === 3 && rep[0] === ch.get("k" + i), "ch getN gives 3 distinct nodes, primary first");
    }
    same(ch.getN("x", 99).length, 5, "ch getN capped at node count");
    const w = new ConsistentHash(); w.addNode("big", 3); w.addNode("small", 1);
    let big = 0; for (let i = 0; i < 40000; i++) if (w.get("k" + i) === "big") big++;
    check(Math.abs(big / 40000 - 0.75) < 0.05, `ch weight 3:1 gives ${(big / 400).toFixed(1)}% to the heavy node`);
    const rt = ConsistentHash.deserialize(ch.serialize());
    let rdiff = 0; for (let i = 0; i < 5000; i++) if (rt.get("user:" + i) !== ch.get("user:" + i)) rdiff++;
    eq(rdiff, 0, "ch serialize round trip keeps placement");
    // another process builds the same ring and must agree key for key
    const code = `import { ConsistentHash } from "dyna:structures"; import * as std from "std";
const c = new ConsistentHash(); for (const n of ${JSON.stringify(names)}) c.addNode(n);
const out = []; for (let i = 0; i < 2000; i++) out.push(c.get("user:" + i)); out.push(ConsistentHash.jump("user:7", 1000)); std.out.puts(out.join(","));`;
    const child = Exec("./dynajs", ["--std", "-m", "-e", code]).stdout.split(",");
    const fresh = new ConsistentHash(); names.forEach(nm => fresh.addNode(nm));
    let xdiff = 0; for (let i = 0; i < 2000; i++) if (child[i] !== fresh.get("user:" + i)) xdiff++;
    eq(xdiff, 0, "ch another process places every key identically");
    eq(Number(child[2000]), ConsistentHash.jump("user:7", 1000), "ch jump is stable across processes");
    eq(new ConsistentHash().get("x"), undefined, "ch empty ring");
    // An independent ring built from the documented construction: every key's owner,
    // including keys hashing past the last point (which wrap to the first).
    for (const reps of [1, 3, 160]) {
        const c2 = new ConsistentHash({ replicas: reps }), nodes = ["n1", "n2", "node-three", "é"];
        nodes.forEach((nm, i) => c2.addNode(nm, i === 1 ? 2 : 1));
        const pts = [];
        nodes.forEach((nm, i) => { for (let r = 0; r < reps * (i === 1 ? 2 : 1); r++) pts.push([XXHash64(nm + "#" + r, 0, { as: "bigint" }), nm]); });
        pts.sort((a, b) => (a[0] < b[0] ? -1 : a[0] > b[0] ? 1 : a[1] < b[1] ? -1 : 1));
        eq(c2.points, pts.length, `ch ring point count (replicas ${reps})`);
        let mism = 0, wrapped = 0;
        for (let i = 0; i < 3000; i++) {
            const key = "key-" + i, h = XXHash64(key, 0, { as: "bigint" });
            let j = pts.findIndex(p => p[0] >= h);
            if (j < 0) { j = 0; wrapped++; }
            if (c2.get(key) !== pts[j][1]) mism++;
            if (i < 300) { const want = []; for (let s = 0; want.length < 3 && s < pts.length; s++) { const nm = pts[(j + s) % pts.length][1]; if (!want.includes(nm)) want.push(nm); }
                same(c2.getN(key, 3), want, `ch getN matches the reference walk (replicas ${reps})`); }
        }
        eq(mism, 0, `ch owner matches the documented construction (replicas ${reps}, ${wrapped} wrapped keys)`);
        if (reps < 160) check(wrapped > 0, `ch the sample includes keys past the last point (replicas ${reps})`);
    }
    // Jump hash against the paper's loop in BigInt arithmetic.
    for (let i = 0; i < 300; i++) {
        const key = "jk" + i, buckets = 1 + (i * 37) % 997;
        let k = XXHash64(key, 0, { as: "bigint" }), b = -1n, j = 0n;
        while (j < BigInt(buckets)) { b = j; k = (k * 2862933555777941757n + 1n) & 0xFFFFFFFFFFFFFFFFn; j = BigInt(Math.floor(Number(b + 1n) * (2 ** 31 / Number((k >> 33n) + 1n)))); }
        eq(ConsistentHash.jump(key, buckets), Number(b), `jump(${key}, ${buckets}) matches the published algorithm`);
    }
}
{
    let monotone = true, inRange = true;
    const counts = new Array(10).fill(0);
    for (let i = 0; i < 20000; i++) {
        const key = "j" + i;
        let prev = ConsistentHash.jump(key, 1);
        for (let b = 2; b <= 40; b++) { const cur = ConsistentHash.jump(key, b); if (cur !== prev && cur !== b - 1) monotone = false; if (cur < 0 || cur >= b) inRange = false; prev = cur; }
        counts[ConsistentHash.jump(key, 10)]++;
    }
    check(monotone, "jump: growing buckets only moves a key to the newest bucket");
    check(inRange, "jump: result in 0..buckets-1");
    check(counts.every(c => Math.abs(c / 20000 - 0.1) < 0.012), "jump: uniform over 10 buckets " + counts.join(","));
    let threw = false; try { ConsistentHash.jump("a", 0); } catch (e) { threw = e instanceof RangeError; } check(threw, "jump: zero buckets refused");
}

// ---- InvertedIndex ----------------------------------------------------------
{
    same(InvertedIndex.tokenize("Hello, World! foo_bar 42x"), ["hello", "world", "foo_bar", "42x"], "tokenize ascii");
    same(InvertedIndex.tokenize("Ünïcode café—naïve x、y"), ["Ünïcode", "café", "naïve", "x", "y"], "tokenize: non-ASCII letters stay, punctuation splits, case folds ASCII only");
    same(InvertedIndex.tokenize(""), [], "tokenize empty");
    const long = "a".repeat(200) + " b";
    same(InvertedIndex.tokenize(long).map(t => t.length), [128, 1], "tokenize caps a token at 128 bytes");
    const ml = "é".repeat(100);
    const tk = InvertedIndex.tokenize(ml)[0];
    eq(tk, "é".repeat(64), "tokenize cuts at a UTF-8 boundary");
}
function bm25Ref(docs, query, opts) {
    const tok = (s) => InvertedIndex.tokenize(s).filter(t => !opts.stop.has(t));
    const D = new Map(); for (const [id, text] of docs) D.set(id, tok(text));
    const N = D.size; let total = 0; for (const t of D.values()) total += t.length;
    const avg = N ? total / N : 0;
    const qt = [...new Set(tok(query))].slice(0, 32);
    const groups = qt.map((t, gi) => {
        if (opts.prefix && gi === qt.length - 1) { const all = new Set(); for (const t2 of D.values()) for (const w of t2) if (w.startsWith(t)) all.add(w); return [...all]; }
        return [t];
    });
    const scores = new Map(), masks = new Map();
    groups.forEach((terms, g) => {
        for (const term of terms) {
            let df = 0; for (const t2 of D.values()) if (t2.includes(term)) df++;
            if (!df) continue;
            const idf = Math.log(1 + (N - df + 0.5) / (df + 0.5));
            for (const [id, t2] of D) {
                const tf = t2.filter(w => w === term).length; if (!tf) continue;
                const s = idf * tf * (opts.k1 + 1) / (tf + opts.k1 * (1 - opts.b + opts.b * t2.length / avg));
                scores.set(id, (scores.get(id) || 0) + s); masks.set(id, (masks.get(id) || 0) | (1 << g));
            }
        }
    });
    const full = (1 << qt.length) - 1;
    return [...scores].filter(([id]) => !opts.and || masks.get(id) === full).sort((a, b) => b[1] - a[1] || a[0] - b[0]).slice(0, opts.limit);
}
function sameRanked(got, want, m) {
    let ok = got.length === want.length;
    for (let i = 0; ok && i < got.length; i++) ok = got[i][0] === want[i][0] && Math.abs(got[i][1] - want[i][1]) <= 1e-9 * Math.max(1, want[i][1]);
    check(ok, m + "\n   got  " + JSON.stringify(got).slice(0, 240) + "\n   want " + JSON.stringify(want).slice(0, 240));
}
for (const [seed, k1, b, stopList] of [[1, 1.2, 0.75, []], [2, 2.0, 0.3, ["the", "of"]], [3, 0.5, 1, ["a"]]]) {
    const r = rng(seed), vocab = [];
    for (let i = 0; i < 300; i++) vocab.push(["the", "of", "a"][i] || ("w" + (i * 7919 % 1000).toString(36) + (i % 3 ? "" : "é")));
    const zipf = () => vocab[Math.min(vocab.length - 1, Math.floor(1 / Math.max(r(), 1e-6)) % vocab.length)];
    const docs = new Map();
    const ii = new InvertedIndex({ k1, b, stopWords: stopList });
    const opts = { k1, b, stop: new Set(stopList) };
    for (let step = 0; step < 900; step++) {
        const id = Math.floor(r() * 400);
        if (r() < 0.8) { const len = 1 + Math.floor(r() * 30); const words = []; for (let i = 0; i < len; i++) words.push(r() < 0.1 ? zipf().toUpperCase() : zipf()); const text = words.join(r() < 0.5 ? " " : ", "); ii.add(id, text); docs.set(id, text); }
        else eq(ii.remove(id), docs.delete(id), `ii[${seed}] remove`);
        if (step % 60 === 59) {
            eq(ii.size, docs.size, `ii[${seed}] size`);
            for (let t = 0; t < 10; t++) {
                const q = [zipf(), zipf(), r() < 0.5 ? zipf() : "nomatch"].join(" ");
                const limit = 1 + Math.floor(r() * 15);
                sameRanked(ii.search(q, { limit }), bm25Ref(docs, q, { ...opts, limit }), `ii[${seed}] or "${q}"`);
                sameRanked(ii.search(q, { limit, mode: "and" }), bm25Ref(docs, q, { ...opts, limit, and: true }), `ii[${seed}] and "${q}"`);
                const pq = zipf() + " " + zipf().slice(0, 2);
                sameRanked(ii.search(pq, { limit, prefix: true }), bm25Ref(docs, pq, { ...opts, limit, prefix: true }), `ii[${seed}] prefix "${pq}"`);
            }
            const term = zipf();
            let df = 0; for (const text of docs.values()) if (InvertedIndex.tokenize(text).includes(term.toLowerCase()) && !opts.stop.has(term)) df++;
            eq(ii.docFreq(term), opts.stop.has(term) ? 0 : df, `ii[${seed}] docFreq(${term})`);
        }
    }
    const back = InvertedIndex.deserialize(ii.serialize());
    for (let t = 0; t < 20; t++) { const q = zipf() + " " + zipf(); same(back.search(q, { limit: 20 }), ii.search(q, { limit: 20 }), `ii[${seed}] serialize keeps search results`); }
    const sug = ii.suggest("w", 5);
    check(sug.length <= 5 && sug.every((e, i) => e[0].startsWith("w") && (i === 0 || sug[i - 1][1] >= e[1])), `ii[${seed}] suggest sorted by docFreq`);
}
{
    const ii = new InvertedIndex();
    same(ii.search("anything"), [], "ii empty index");
    ii.add(1, "alpha beta");
    same(ii.search(""), [], "ii empty query");
    same(ii.search("alpha", { limit: 0 }), [], "ii limit 0");
    eq(ii.docLength(1), 2, "ii docLength");
    ii.add(1, "gamma");
    same(ii.search("alpha"), [], "ii re-adding an id replaces the document");
    eq(ii.search("gamma")[0][0], 1, "ii replaced document searchable");
    let threw = false; try { ii.search("x", { mode: "xor" }); } catch (e) { threw = e instanceof RangeError; } check(threw, "ii bad mode refused");
    threw = false; try { new InvertedIndex({ b: 2 }); } catch (e) { threw = e instanceof RangeError; } check(threw, "ii b out of range refused");
    threw = false; try { ii.add(-1, "x"); } catch (e) { threw = e instanceof RangeError; } check(threw, "ii negative id refused");
    const many = new InvertedIndex(); many.add(1, Array.from({ length: 40 }, (_, i) => "t" + i).join(" "));
    eq(many.search(Array.from({ length: 40 }, (_, i) => "t" + i).join(" "), { mode: "and" }).length, 1, "ii a 40-term AND query uses the first 32 terms");
}

print(`test_structures_db_search: ${fails ? fails + " FAILED of" : "all"} ${n} checks${fails ? "" : " passed"}`);
if (fails) throw new Error(fails + " failures");
