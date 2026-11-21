// covers: dyna:structures.ConsistentHash
// The ring build hashes "<name>#<decimal replica>" with an in-place decimal counter and sorts the
// points with a radix sort on the 64-bit hash (ties by name bytes, then length) once a ring
// reaches 4096 points, qsort below that. Routing must stay exactly what the snprintf + qsort
// build produced: the digests below were computed by that build (fixA2) over get and getN for
// rings under and over the radix threshold, replica counters across every decimal width up to
// 999999, names holding "#", "", non-ASCII and 300 bytes, a reloaded ring and one edited after
// load (addNode/removeNode merge path).
import { suite } from "../lib/kit.js";
import { ConsistentHash } from "dyna:structures";
import { CRC32C } from "dyna:hash";

const t = suite("consistent_hash_ring_pins");
const enc = new TextEncoder();
const digest = (c, k) => {
    let s = "";
    for (let i = 0; i < k; i++) {
        const key = "key:" + (i * 2654435761 % 1000003);
        s += c.get(key) + "|" + (i % 7 === 0 ? c.getN(key, 3).join(",") : "") + ";";
    }
    return (CRC32C(enc.encode(s)) >>> 0).toString(16);
};

t.test("routing equals the snprintf + qsort ring build", (a) => {
    const small = new ConsistentHash();
    ["a", "b", "c", "d", "e"].forEach((n, i) => small.addNode(n, i + 1));
    a.eq(digest(small, 20000), "d8a8778d", "2400 points: the qsort path");
    const big = new ConsistentHash({ replicas: 1000 });
    ["", "x#1", "x#", "héllo", "node-07", "node-7", "node-70", "\u{1F600}", "a".repeat(300)].forEach((n, i) => big.addNode(n, 1 + ((i * 37) % 13)));
    big.addNode("big", 1000);
    a.eq(digest(big, 30000), "872dba32", "about 1.07M points: the radix path, counters to 999999");
    const back = ConsistentHash.deserialize(big.serialize());
    a.eq(digest(back, 30000), "872dba32", "a reloaded ring routes the same");
    back.addNode("late", 7);
    back.removeNode("node-7");
    back.addNode("x#", 2);
    a.eq(digest(back, 30000), "5b4d4866", "edits after load merge into the same ring");
    const wide = new ConsistentHash({ replicas: 1 });
    for (let i = 0; i < 9000; i++) wide.addNode("n" + i, 1);
    a.eq(digest(ConsistentHash.deserialize(wide.serialize()), 20000), "34b95083", "9000 one-point nodes");
});

await t.run();
