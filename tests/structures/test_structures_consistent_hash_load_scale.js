// covers: dyna:structures.ConsistentHash
// ConsistentHash.deserialize (and so load) once replayed addNode per node, and
// every addNode rebuilds the whole ring: decoding n nodes cost O(n * ring). It
// now appends every node and rebuilds once. The ring must be the one the
// original holds (same owner for every probe key, same getN order), and decode
// time must grow (near) linearly: 16x the nodes may take at most 64x as long
// (CPU time, best of N; sorted rebuild is ~20x, the old per-node replay ~300x). A record
// whose total ring would pass DYN_CHASH_MAX_POINTS is still refused, now at
// the single rebuild.
import { ConsistentHash } from "dyna:structures";
import { perfCheck } from "../lib/perf.js";
import { CRC32C } from "dyna:hash";
import { suite } from "../lib/kit.js";
import { bestMs } from "../lib/cpuclock.js";

const t = suite("structures_consistent_hash_load_scale");

function build(nodes) {
    const x = new ConsistentHash({ replicas: 8 });
    for (let i = 0; i < nodes; i++) x.addNode("node-" + i, 1 + (i % 4));
    return x;
}

t.test("decoded ring routes every key like the original", (a) => {
    const x = build(300), y = ConsistentHash.deserialize(x.serialize());
    let same = 0;
    for (let i = 0; i < 5000; i++) if (x.get("key" + i) === y.get("key" + i)) same++;
    a.eq(same, 5000, "owners agree");
    a.deep(y.getN("abc", 5), x.getN("abc", 5), "getN order");
    a.deep(y.nodes(), x.nodes(), "node list");
});

t.test("decode time is linear in nodes", (a) => {
    const s = build(250).serialize(), b = build(4000).serialize();
    const ts = bestMs(() => ConsistentHash.deserialize(s), 5).ms, tb = bestMs(() => ConsistentHash.deserialize(b), 3).ms;
    perfCheck(a.ok, tb <= 64 * Math.max(ts, 0.5), "4000/250 node decode ratio " + (tb / ts).toFixed(1) + " (" + tb.toFixed(1) + " ms vs " + ts.toFixed(1) + " ms)", a.skip);
});

t.test("a record whose ring would pass the point cap is refused", (a) => {
    const x = new ConsistentHash({ replicas: 1000 });
    for (let i = 0; i < 4; i++) x.addNode("n" + i, 1000);
    x.addNode("tail", 194);
    a.eq(x.nodes().length, 5, "4,194,000 points fit under the 4,194,304 cap");
    const bytes = x.serialize(), dv = new DataView(bytes.buffer, bytes.byteOffset, bytes.length);
    a.eq(dv.getUint32(bytes.length - 8, true), 194, "the last field is the tail node's weight");
    dv.setUint32(bytes.length - 8, 195, true);
    dv.setUint32(bytes.length - 4, CRC32C(bytes.subarray(0, bytes.length - 4)) >>> 0, true);
    a.throws(() => ConsistentHash.deserialize(bytes), TypeError, "4,195,000 points");
});

await t.run();
