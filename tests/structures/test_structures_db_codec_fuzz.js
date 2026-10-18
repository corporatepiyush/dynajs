// Byte-flip and truncation fuzz of deserialize() for the classes in
// dyna-structures-db: the record's CRC and payload length are re-sealed after
// each mutation, so the corruption reaches the class's own parser instead of
// stopping at the checksum. Every accepted record is then driven through its
// mutating and query methods; a crash, a non-Error throw or a sanitizer report
// (this file is in SAN_NATIVE_LIST) is the failure.
import * as S from "dyna:structures";
import { CRC32C } from "dyna:hash";
function reseal(b) { if (b.length < 24) return b; const n = b.length - 24; for (let i = 0; i < 8; i++) b[12 + i] = i < 4 ? (n >>> (8 * i)) & 255 : 0; const c = CRC32C(b.subarray(0, b.length - 4)); b[b.length - 4] = c & 255; b[b.length - 3] = (c >>> 8) & 255; b[b.length - 2] = (c >>> 16) & 255; b[b.length - 1] = (c >>> 24) & 255; return b; }
let s = 7; const r = () => { s ^= s << 13; s >>>= 0; s ^= s >>> 17; s ^= s << 5; s >>>= 0; return s / 4294967296; };
const mk = {
  ZSet: () => { const x = new S.ZSet(); for (let i = 0; i < 50; i++) x.add("m" + i, i % 7); return x; },
  OrderedMap: () => { const x = new S.OrderedMap(); for (let i = 0; i < 30; i++) x.set("k" + i, i % 2 ? [i] : { i }); return x; },
  RoaringBitmap: () => { const x = new S.RoaringBitmap([1, 2, 70000]); x.addRange(200000, 210000); x.addRange(65536 * 9, 65536 * 10); return x; },
  TDigest: () => { const x = new S.TDigest(20); for (let i = 0; i < 500; i++) x.add(i); return x; },
  TopK: () => { const x = new S.TopK(5); for (let i = 0; i < 40; i++) x.add("w" + (i % 9)); return x; },
  CuckooFilter: () => { const x = new S.CuckooFilter(32); for (let i = 0; i < 20; i++) x.add("c" + i); return x; },
  IndexedHeap: () => { const x = new S.IndexedHeap({ max: true }); for (let i = 0; i < 30; i++) x.push(i * 3, i % 5); return x; },
  KDTree: () => new S.KDTree([[0, 0], [1, 1], [2, 5], [3, 3], [9, 9], [4, 4], [5, 1], [6, 6], [7, 2], [8, 8], [1, 9], [2, 2], [3, 7], [4, 0]]),
  ConsistentHash: () => { const x = new S.ConsistentHash({ replicas: 3 }); x.addNode("a"); x.addNode("b", 2); return x; },
  InvertedIndex: () => { const x = new S.InvertedIndex({ stopWords: ["the"] }); x.add(1, "the quick brown fox"); x.add(2, "lazy dog"); x.add(7, "quick quick dog"); return x; },
  Interner: () => { const x = new S.Interner(); for (let i = 0; i < 20; i++) x.intern("s" + i); return x; },
};
function exercise(name, o) {
  const tryc = (f) => { try { f(); } catch (e) { if (!(e instanceof Error)) throw e; } };
  switch (name) {
  case "ZSet": tryc(() => { o.add("m1", 3); o.incrBy("zz", 2); o.range(0, -1); o.rangeByScore(-1e9, 1e9, { offset: 1, limit: 3, reverse: true }); o.rank("m3"); o.removeRangeByRank(0, 1); o.popMax(); o.popMin(); [...o]; }); break;
  case "OrderedMap": tryc(() => { o.set("k1", 2); o.delete("k2"); o.prefix("k"); o.rangeQuery("a", "z", { reverse: true }); o.floorKey("k5"); o.at(-1); o.deleteRange("k1", "k3"); }); break;
  case "RoaringBitmap": tryc(() => { o.add(5); o.delete(70000); o.rank(1e6); o.select(3); o.min(); o.max(); const c = o.clone(); c.or(o); c.xor(o); o.andNot(c); o.addRange(0, 70000); o.removeRange(100, 200000); if (o.size < 1e6) o.toArray(); }); break;
  case "TDigest": tryc(() => { o.quantile(0.5); o.cdf(10); o.add(3); o.merge(o); o.quantiles([0, 0.1, 1]); }); break;
  case "TopK": tryc(() => { for (let i = 0; i < 20; i++) o.add("q" + i); o.list(); o.count("w1"); }); break;
  case "CuckooFilter": tryc(() => { for (let i = 0; i < 40; i++) o.add("n" + i); o.mayContain("c3"); o.delete("c3"); }); break;
  case "IndexedHeap": tryc(() => { o.push(3, 9); o.delete(6); o.pop(); o.popUntil(3); while (o.size) o.pop(); }); break;
  case "KDTree": tryc(() => { o.knn([1, 1], 3); o.radius([0, 0], 4); o.box([0, 0], [5, 5]); o.nearest([3, 3]); }); break;
  case "ConsistentHash": tryc(() => { o.get("x"); o.getN("y", 3); o.addNode("c"); o.removeNode("a"); o.get("x"); }); break;
  case "InvertedIndex": tryc(() => { o.search("quick dog"); o.search("qu", { prefix: true, mode: "and" }); o.add(1, "new text"); o.remove(2); o.suggest("q"); }); break;
  case "Interner": tryc(() => { o.intern("zz"); o.getMany([0, 1, 99]); o.lookup("s1"); }); break;
  }
}
let tried = 0, ok = 0, refused = 0;
for (const [name, f] of Object.entries(mk)) {
  const base = f().serialize();
  for (let it = 0; it < 700; it++) {
    const b = Uint8Array.from(base);
    const flips = 1 + Math.floor(r() * 4);
    for (let k = 0; k < flips; k++) { const i = 20 + Math.floor(r() * (b.length - 24)); b[i] = r() < 0.5 ? Math.floor(r() * 256) : b[i] ^ (1 << Math.floor(r() * 8)); }
    const cut = reseal(r() < 0.2 ? Uint8Array.from(b.subarray(0, 24 + Math.floor(r() * (b.length - 24)))) : b);
    tried++;
    try { const o = S[name].deserialize(cut); ok++; o.serialize(); exercise(name, o); } catch (e) { refused++; if (!(e instanceof Error)) throw e; }
  }
}
print(`test_structures_db_codec_fuzz: ${tried} records, ${ok} accepted, ${refused} refused`);
if (ok < tried / 10) throw new Error("too few records reached the class parsers: " + ok);
