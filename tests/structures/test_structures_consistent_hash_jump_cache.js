// covers: dyna:structures.ConsistentHash -- the opt-in hash-prefix jump table ({ cache: true | { jump: true } }) routes exactly as the plain search
// With the cache on, get/getN narrow the ring binary search to one bucket of a table indexed by the top
// bits of the key hash (about 8 points per bucket, 0.5 byte per point). Any ring change (addNode,
// reweight, removeNode, load) marks the table stale and the next lookup rebuilds it, so routing is
// never from an old ring. Rings under 64 points use the plain search. The cache is derived state:
// serialize() bytes are identical, and a deserialized ring starts with it off. Oracle: the same op
// sequence on a cached and an uncached ring, compared on every lookup, plus the pinned digests of
// test_structures_consistent_hash_ring_pins.js.
import { suite } from "../lib/kit.js";
import { ConsistentHash } from "dyna:structures";
import { CRC32C } from "dyna:hash";

const t = suite("consistent_hash_jump_cache");
const enc = new TextEncoder();
const digest = (c, k) => {
    let s = "";
    for (let i = 0; i < k; i++) {
        const key = "key:" + (i * 2654435761 % 1000003);
        s += c.get(key) + "|" + (i % 7 === 0 ? c.getN(key, 3).join(",") : "") + ";";
    }
    return (CRC32C(enc.encode(s)) >>> 0).toString(16);
};

t.test("the pinned routing digests hold with the cache on", (a) => {
    const big = new ConsistentHash({ replicas: 1000, cache: { jump: true } });
    ["", "x#1", "x#", "héllo", "node-07", "node-7", "node-70", "\u{1F600}", "a".repeat(300)].forEach((n, i) => big.addNode(n, 1 + ((i * 37) % 13)));
    big.addNode("big", 1000);
    a.eq(digest(big, 30000), "872dba32", "about 1.07M points");
    big.addNode("late", 7);
    big.removeNode("node-7");
    big.addNode("x#", 2);
    a.eq(digest(big, 30000), "5b4d4866", "edits after lookups rebuild the table");
    const small = new ConsistentHash({ cache: true });
    ["a", "b", "c", "d", "e"].forEach((n, i) => small.addNode(n, i + 1));
    a.eq(digest(small, 20000), "d8a8778d", "2400 points");
});

t.test("cached and uncached rings agree through adds, reweights and removes", (a) => {
    let s = 7;
    const rnd = (n) => (s = (s * 1103515245 + 12345) >>> 0) % n;
    for (const replicas of [1, 3, 40, 300]) {
        const on = new ConsistentHash({ replicas, cache: true });
        const off = new ConsistentHash({ replicas });
        let bad = 0, lookups = 0;
        for (let op = 0; op < 600; op++) {
            const k = rnd(10), name = "n" + rnd(80);
            if (k < 5) { const w = 1 + rnd(9); on.addNode(name, w); off.addNode(name, w); }
            else if (k < 7) { on.removeNode(name); off.removeNode(name); }
            for (let q = 0; q < 20; q++) {
                const key = "q" + rnd(1e9);
                lookups++;
                if (on.get(key) !== off.get(key) || on.getN(key, 2).join() !== off.getN(key, 2).join()) bad++;
            }
        }
        a.eq(bad, 0, "replicas " + replicas + ": " + lookups + " lookups agree");
        a.ok([...on.serialize()].join() === [...off.serialize()].join(), "replicas " + replicas + ": serialize() is identical");
        const back = ConsistentHash.deserialize(on.serialize());
        a.eq(back.get("zz"), off.get("zz"), "replicas " + replicas + ": a deserialized ring routes the same");
    }
    const empty = new ConsistentHash({ cache: true });
    a.eq(empty.get("k"), undefined, "an empty cached ring routes nowhere");
    empty.addNode("solo");
    a.eq(empty.get("k"), "solo");
    empty.removeNode("solo");
    a.eq(empty.get("k"), undefined, "emptied again");
});

t.test("the cache option is checked", (a) => {
    a.throws(() => new ConsistentHash({ cache: { count: true } }), /unknown cache option "count"/);
    a.throws(() => new ConsistentHash({ cache: "yes" }), TypeError);
    a.eq(new ConsistentHash({ cache: { jump: false } }).addNode("a").get("x"), "a");
});

await t.run();
