// covers: dyna:structures.ConsistentHash.addNode
// addNode, a weight change and removeNode once rebuilt the whole ring: every point of
// every node re-hashed and the ring re-sorted, so adding 4000 nodes one by one took
// 21 s and each change at 4000 nodes cost as much as decoding the whole map (~11 ms).
// Now only the changed node's points are hashed and merged into the sorted ring
// (O(ring) memory moves, no re-hash): 4000 adds 0.3 s, a change 0.1-0.2 ms.
// Oracle: after any mix of adds, removes (middle and last slot, which renumbers the
// last node) and reweights, the ring must route exactly like a from-scratch rebuild
// (deserialize rebuilds once). Cost: one change at 4000 nodes must cost under 1/2 of
// one full rebuild at the same size, CPU time, best of N. A per-change rebuild is ~1x; the merge
// was ~80x against the qsort rebuild and is ~11x (release) / ~3.5x (ASan) against the radix one.
import { ConsistentHash } from "dyna:structures";
import { suite } from "../lib/kit.js";
import { bestMs } from "../lib/cpuclock.js";

const t = suite("structures_consistent_hash_add_node_scale");

function build(nodes, replicas = 8) {
    const x = new ConsistentHash({ replicas });
    for (let i = 0; i < nodes; i++) x.addNode("node-" + i, 1 + (i % 4));
    return x;
}

function sameRouting(a, x, label) {
    const y = ConsistentHash.deserialize(x.serialize());
    let diff = 0;
    for (let i = 0; i < 20000; i++) if (x.get("key" + i) !== y.get("key" + i)) diff++;
    a.eq(diff, 0, label + ": owners agree with a full rebuild");
    for (const k of ["abc", "key7", "zz", ""]) a.deep(x.getN(k, 6), y.getN(k, 6), label + ": getN(" + JSON.stringify(k) + ")");
}

t.test("incremental ring equals a full rebuild after adds, removes and reweights", (a) => {
    const x = build(300);
    sameRouting(a, x, "after 300 adds");
    x.removeNode("node-299");
    x.removeNode("node-17");
    x.removeNode("node-0");
    sameRouting(a, x, "after removing last, middle, first");
    x.addNode("node-5", 9);
    x.addNode("node-100", 1);
    x.addNode("node-6", 1);
    x.addNode("node-7", 2);
    x.addNode("node-17", 3);
    x.addNode("fresh", 2);
    sameRouting(a, x, "after reweights up and down and re-adds");
    for (let i = 0; i < 290; i++) x.removeNode("node-" + i);
    sameRouting(a, x, "after removing almost every node");
    a.deep(x.nodes().slice().sort(), ["fresh", "node-290", "node-291", "node-292", "node-293", "node-294", "node-295", "node-296", "node-297", "node-298"].sort(), "remaining nodes");
    for (const n of x.nodes()) x.removeNode(n);
    a.eq(x.nodes().length, 0, "empty");
    a.eq(x.get("k"), undefined, "empty ring routes nowhere");
    x.addNode("solo");
    a.eq(x.get("k"), "solo", "one node takes every key");
});

t.test("one change costs a fraction of a full rebuild", (a) => {
    const x = build(4000), bytes = x.serialize();
    const full = bestMs(() => ConsistentHash.deserialize(bytes), 3).ms;
    let i = 0;
    const add = bestMs(() => { x.addNode("extra-" + i++, 2); }, 7).ms;
    let j = 0;
    const rw = bestMs(() => { x.addNode("node-" + j++, 4); }, 7).ms;
    let k = 0;
    const rm = bestMs(() => { x.removeNode("extra-" + k++); }, 7).ms;
    const lim = Math.max(full, 1) / 2;
    a.ok(add <= lim, "addNode " + add.toFixed(3) + " ms vs full rebuild " + full.toFixed(2) + " ms");
    a.ok(rw <= lim, "reweight " + rw.toFixed(3) + " ms vs full rebuild " + full.toFixed(2) + " ms");
    a.ok(rm <= lim, "removeNode " + rm.toFixed(3) + " ms vs full rebuild " + full.toFixed(2) + " ms");
});

t.test("an add past the point cap is refused and leaves the ring as it was", (a) => {
    const x = new ConsistentHash({ replicas: 1000 });
    for (let i = 0; i < 4; i++) x.addNode("n" + i, 1000);
    x.addNode("tail", 194);
    const before = Array.from({ length: 2000 }, (_, i) => x.get("key" + i)).join(",");
    a.throws(() => x.addNode("over", 1), Error, "4,195,000 points");
    a.throws(() => x.addNode("tail", 195), Error, "reweight to 4,195,000 points");
    a.eq(x.nodes().length, 5, "node count unchanged");
    a.eq(x.hasNode("over"), false, "refused node absent");
    a.eq(Array.from({ length: 2000 }, (_, i) => x.get("key" + i)).join(","), before, "routing unchanged");
});

await t.run();
