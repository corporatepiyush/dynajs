/* rss_structures_ds2.js -- peak-RSS-plateau check for the dyna:structures
 * classes the standing rss_structures.js never covered: the Guava batch
 * (Multiset, Multimap, BiMap, Table, RangeSet, RangeMap, IntervalTree,
 * MinMaxHeap, CountMinSketch, HyperLogLog) plus Heap, List and Graph.
 *
 * Same shape as rss_structures.js: create-and-drop cycles, flat peak RSS
 * across N => reclamation works and nothing leaks; linear growth => a leak.
 * This is the leak proof on arm64-darwin, which has no LeakSanitizer.
 *
 * The values stored are OBJECTS (not atoms): a leaked JSValue refcount is
 * invisible for interned strings and small integers, but every object here
 * is heap-allocated and refcounted, so a leaked reference keeps it alive.
 *
 * Run via: ./dev.sh rss tests/rss_structures_ds2.js
 *      or:  ./dynajs tests/rss_structures_ds2.js 50000
 */

import { Heap, List,
         Multiset, Multimap, BiMap, Table, RangeSet, RangeMap, IntervalTree,
         MinMaxHeap, CountMinSketch, HyperLogLog, Graph } from "dyna:structures";

const N = (typeof scriptArgs !== "undefined" && scriptArgs[1])
    ? (scriptArgs[1] | 0) : 50000;

for (let i = 0; i < N; i++) {
    const h = new Heap((a, b) => a.i - b.i);
    for (let k = 0; k < 8; k++) h.push({ i: (i + k) & 31, k });
    while (h.size) h.pop();

    const l = new List();
    for (let k = 0; k < 8; k++) { l.pushBack({ i, k }); l.pushFront({ i, k }); }
    while (l.length) { l.popBack(); if (l.length) l.popFront(); }

    const ms = new Multiset();
    for (let k = 0; k < 8; k++) ms.add("k" + ((i + k) & 15), 3);
    ms.remove("k0", 1); ms.setCount("k1", 7); ms.count("k2");

    const mm = new Multimap();
    for (let k = 0; k < 8; k++) mm.put("k" + ((i + k) & 15), { i, k });
    mm.removeAt("k0", 0); mm.count("k1"); mm.get("k2");

    const bm = new BiMap();
    for (let k = 0; k < 8; k++) bm.set("k" + ((i + k) & 15), "v" + ((i + k * 3) & 31));
    bm.forceSet("k9", "v0"); bm.keyOf("v1");

    const t = new Table();
    for (let k = 0; k < 8; k++) t.put("r" + ((i + k) & 7), "c" + (k & 3), { i, k });
    t.delete("r0", "c0"); t.row("r1"); t.column("c2");

    const rs = new RangeSet();
    for (let k = 0; k < 8; k++) rs.add((i + k * 10) & 63, ((i + k * 10) & 63) + 1);
    rs.remove(0, 8); rs.measure; rs.contains(5);

    const rm = new RangeMap();
    for (let k = 0; k < 8; k++) rm.put(k * 10, k * 10 + 8, { i, k });
    rm.remove(20, 60); rm.get(5); rm.get(35);

    const it = new IntervalTree();
    for (let k = 0; k < 8; k++) it.insert(k * 10, k * 10 + 5, { i, k });
    it.at(15); it.overlapping(0, 100);

    const mh = new MinMaxHeap();
    for (let k = 0; k < 8; k++) mh.push((i + k) & 31, { i, k });
    mh.popMin(); mh.popMax(); mh.peekMin();

    const cms = new CountMinSketch(256, 4);
    for (let k = 0; k < 8; k++) cms.add("k" + ((i + k) & 15), 2);
    cms.count("k0");

    const hll = new HyperLogLog(8);
    for (let k = 0; k < 16; k++) hll.add("k" + ((i + k) & 255));
    hll.count();

    const g = new Graph({ directed: true, weighted: true });
    for (let k = 0; k < 8; k++) g.addEdge((i + k) & 7, (i + k + 1) & 7, 1.5);
    g.neighbors(0); g.dijkstra(0); g.connectedComponents();
}

print("rss_structures_ds2: completed N=" + N + " create-and-drop cycles (no cleanup)");
