// flags: --std
// covers: dyna:structures.CuckooFilter
// Differential and property tests for RoaringBitmap, CuckooFilter, TopK and
// TDigest. RoaringBitmap is checked against a JS Set on value sets built to
// cross its container thresholds (array <= 4096 < bitmap < 65536 = full) at
// N-1, N, N+1, with every set operation in both the in-place and the
// cardinality-only form. The sketches are checked against their published
// guarantees rather than against digits this engine produced:
//   CuckooFilter: no false negatives; FPR near 2*b/2^f (Fan et al. 2014).
//   TopK (Metwally et al. 2005): every key with count > N/k is listed, and
//     count - error <= true count <= count.
//   TDigest (Dunning 2019): quantile rank error bounded, tighter in the tails.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_structures_cuckoo_filter_db_sets");
import { RoaringBitmap, CuckooFilter, TopK, TDigest } from "dyna:structures";
import { Exec } from "dyna:sys";

let n = 0, fails = 0;
function check(c, m) { n++; if (!c) { fails++; if (fails < 40) print("FAIL: " + m); } }
function eq(a, b, m) { check(Object.is(a, b), m + " -- got " + a + ", want " + b); }

function rng(seed) {
    let s = seed >>> 0 || 1;
    return () => { s ^= s << 13; s >>>= 0; s ^= s >>> 17; s ^= s << 5; s >>>= 0; return s / 4294967296; };
}

// ---- RoaringBitmap ----------------------------------------------------------
function build(r, shape) {
    const out = new Set();
    const C = 65536;
    switch (shape) {
    case "sparse": for (let i = 0; i < 3000; i++) out.add(Math.floor(r() * 2 ** 32)); break;
    case "chunk4095": for (let i = 0; i < 4095; i++) out.add(C * 3 + i * 7); break;
    case "chunk4096": for (let i = 0; i < 4096; i++) out.add(C * 3 + i * 7); break;
    case "chunk4097": for (let i = 0; i < 4097; i++) out.add(C * 3 + i * 7); break;
    case "full": for (let i = 0; i < C; i++) out.add(C * 3 + i); break;
    case "full-1": for (let i = 0; i < C; i++) if (i !== 1234) out.add(C * 3 + i); break;
    case "mixed": for (let i = 0; i < 20000; i++) out.add(C * 3 + Math.floor(r() * C)); for (let i = 0; i < 100; i++) out.add(C * 4 + i); for (let i = 0; i < C; i++) out.add(C * 5 + i); out.add(2 ** 32 - 1); out.add(0); break;
    case "dense": for (let i = 0; i < 60000; i++) out.add(C * 3 + Math.floor(r() * C)); break;
    }
    return out;
}
const sortU = (s) => Uint32Array.from([...s].sort((a, b) => a - b));
function sameSet(rb, s, m) {
    const a = rb.toArray(), b = s.arr || sortU(s);
    eq(rb.size, s.size, m + " size");
    let ok = a.length === b.length;
    for (let i = 0; ok && i < a.length; i++) ok = a[i] === b[i];
    check(ok, m + " contents");
}
const SHAPES = ["sparse", "chunk4095", "chunk4096", "chunk4097", "full", "full-1", "mixed", "dense"];

__kitS.section("cuckoo: every key fits at the requested capacity", () => {
    const N = 50000, cf = new CuckooFilter(N);
    let added = 0;
    for (let i = 0; i < N; i++) if (cf.add("key:" + i)) added++;
    eq(added, N, "cuckoo: every key fits at the requested capacity");
    let fn = 0; for (let i = 0; i < N; i++) if (!cf.mayContain("key:" + i)) fn++;
    eq(fn, 0, "cuckoo: no false negatives");
    let fp = 0; const T = 200000; for (let i = 0; i < T; i++) if (cf.mayContain("other:" + i)) fp++;
    check(fp / T < 0.0005, "cuckoo: false positive rate " + (fp / T) + " under 0.05% (theory ~0.012%)");
    let del = 0; for (let i = 0; i < N; i += 2) if (cf.delete("key:" + i)) del++;
    eq(del, N / 2, "cuckoo: deletes of added keys succeed");
    fn = 0; for (let i = 1; i < N; i += 2) if (!cf.mayContain("key:" + i)) fn++;
    eq(fn, 0, "cuckoo: survivors still present after deletes");
    let gone = 0; for (let i = 0; i < N; i += 2) if (!cf.mayContain("key:" + i)) gone++;
    check(gone > N / 2 * 0.99, "cuckoo: deleted keys read absent (" + gone + ")");
    eq(cf.size, N / 2, "cuckoo: size after deletes");
    const back = CuckooFilter.deserialize(cf.serialize());
    fn = 0; for (let i = 1; i < N; i += 2) if (!back.mayContain("key:" + i)) fn++;
    eq(fn, 0, "cuckoo: serialize keeps every key");
    const small = new CuckooFilter(8); let ok = 0;
    for (let i = 0; i < 1000; i++) if (small.add("s" + i)) ok++;
    check(ok < 1000 && ok >= 8, "cuckoo: a full filter refuses (" + ok + " accepted)");
    let lost = 0; for (let i = 0; i < 1000; i++) if (!small.mayContain("s" + i) && i < ok) lost++;
    // every key that add() accepted must still be found, including the victim slot
    const acc = new CuckooFilter(8), keys = [];
    for (let i = 0; i < 1000; i++) if (acc.add("v" + i)) keys.push("v" + i);
    check(keys.every(k => acc.mayContain(k)), "cuckoo: accepted keys survive the eviction chain");
    let threw = false; try { new CuckooFilter(2 ** 27); } catch (e) { threw = e instanceof RangeError; } check(threw, "cuckoo: capacity cap");
});

await __kitS.section("import { CuckooFilter, BloomFilter } from \"dyna:structures\"; import * as std", async () => {
    // the hash is fixed: a filter written by another process answers the same
    const code = `import { CuckooFilter, BloomFilter } from "dyna:structures"; import * as std from "std";
const c = new CuckooFilter(1000); for (let i = 0; i < 500; i++) c.add("p" + i);
const b = new BloomFilter(1 << 14, 5); for (let i = 0; i < 500; i++) b.add("p" + i);
const out = [...c.serialize()].join(",") + "|" + [...b.serialize()].join(","); std.out.puts(out);`;
    const child = Exec("./dynajs", ["--std", "-m", "-e", code]);
    const [cs, bs] = child.stdout.split("|");
    const cf = CuckooFilter.deserialize(new Uint8Array(cs.split(",").map(Number)));
    const bf = (await import("dyna:structures")).BloomFilter.deserialize(new Uint8Array(bs.split(",").map(Number)));
    let cmiss = 0, bmiss = 0; for (let i = 0; i < 500; i++) { if (!cf.mayContain("p" + i)) cmiss++; if (!bf.mayContain("p" + i)) bmiss++; }
    eq(cmiss, 0, "cuckoo: a filter serialized by another process has no false negatives");
    eq(bmiss, 0, "bloom: a filter serialized by another process has no false negatives");
});
__kitS.end();

print(`test_structures_db_sets: ${fails ? fails + " FAILED of" : "all"} ${n} checks${fails ? "" : " passed"}`);
if (fails) throw new Error(fails + " failures");
