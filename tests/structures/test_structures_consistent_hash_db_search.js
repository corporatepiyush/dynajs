// flags: --std
// covers: dyna:structures.ConsistentHash
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
const __kitS = __kitSections("test_structures_consistent_hash_db_search");
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

__kitS.section("ch placement does not depend on insertion order", () => {
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
});

__kitS.section("jump: growing buckets only moves a key to the newest bucket", () => {
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
});
__kitS.end();

print(`test_structures_db_search: ${fails ? fails + " FAILED of" : "all"} ${n} checks${fails ? "" : " passed"}`);
if (fails) throw new Error(fails + " failures");
