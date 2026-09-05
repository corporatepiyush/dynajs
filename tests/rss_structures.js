/* rss_structures.js — peak-RSS-plateau memory check for dyna:structures.
 *
 * These are plain GC-managed objects: nothing is ever closed. This loop creates
 * 11 objects per iteration and lets each fall out of scope; the reference-
 * counting GC frees them immediately. FLAT peak RSS across N => reclamation
 * works and there is no leak; linear growth => a leak. (No LeakSanitizer on
 * arm64-darwin, so this plateau is the leak proof there.)
 *
 * Run via: ./dev.sh rss tests/rss_structures.js            (N = 20k/100k/500k)
 *      or:  ./dynajs tests/rss_structures.js 500000
 */

import { BitSet, UnionFind, Deque, Fenwick, RingBuffer, SegTree, BloomFilter,
         Trie, LRU, SortedSet, SortedMap } from "dyna:structures";

const N = (typeof scriptArgs !== "undefined" && scriptArgs[1])
    ? (scriptArgs[1] | 0) : 100000;

for (let i = 0; i < N; i++) {
    const b = new BitSet(); for (let k = 0; k < 16; k++) b.set((i + k * 7) & 1023);
    b.count; b.toArray(); b.nextSet(0);

    const uf = new UnionFind(16); for (let k = 0; k < 8; k++) uf.union(k, (k + i) & 15);
    uf.connected(0, 15); uf.count;

    const d = new Deque();
    d.pushBack({ i }); d.pushFront("s" + (i & 15)); d.pushBack(i);
    d.popFront(); d.popBack(); d.toArray();

    const f = new Fenwick(16); for (let k = 0; k < 16; k++) f.update(k, (i + k) & 255);
    f.prefixSum(15); f.rangeQuery(3, 12);

    const rb = new RingBuffer(8);
    for (let k = 0; k < 20; k++) rb.push({ k });   /* forces eviction+free */
    rb.toArray();

    const sg = new SegTree(16, "max");
    for (let k = 0; k < 16; k++) sg.update(k, (i * 7 + k) & 1023);
    sg.rangeQuery(2, 13);

    const bf = new BloomFilter(1024, 4);
    for (let k = 0; k < 8; k++) bf.add("k" + i + "_" + k);
    bf.mayContain("k0_0");

    const tr = new Trie();
    for (let k = 0; k < 8; k++) tr.insert("w" + ((i + k) & 63));
    tr.keysWithPrefix("w"); tr.longestPrefix("w12345");

    const lru = new LRU(4);
    for (let k = 0; k < 12; k++) lru.put("k" + k, { i, k });   /* forces eviction+free */
    lru.get("k11"); lru.delete("k10");

    const ss = new SortedSet();
    for (let k = 0; k < 16; k++) ss.add((i * 13 + k) & 255);
    ss.floor(100); ss.ceil(100); ss.rangeQuery(20, 200); ss.toArray();

    const sm = new SortedMap();
    for (let k = 0; k < 16; k++) sm.set(k, { i, k });          /* value dup+free */
    sm.floorKey(8); sm.rangeQuery(2, 12); sm.delete(3);
    /* every object above becomes unreachable at iteration end — freed by GC */
}

print("rss_structures: completed N=" + N + " create-and-drop cycles (no cleanup)");
