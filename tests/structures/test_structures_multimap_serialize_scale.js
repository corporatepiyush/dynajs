// covers: dyna:structures.Multimap
// Multimap.serialize (and so save) walks its cells through a per-index callback.
// It once rescanned every key from the start for each cell, which made a 100K-value
// map with 25K keys take seconds. The time must grow linearly: sixteen times the data
// may take at most 64 times as long (a quadratic walk takes about 256). Sizes are
// 10K and 160K so that the small timing is not sub-millisecond noise; CPU time, best of N.
import { Multimap } from "dyna:structures";
import { perfCheck } from "../lib/perf.js";
import { suite } from "../lib/kit.js";
import { bestMs } from "../lib/cpuclock.js";

const t = suite("structures_multimap_serialize_scale");

function build(n) {
    const m = new Multimap();
    for (let i = 0; i < n; i++) m.put("k" + (i % (n >> 2)), i);
    return m;
}

t.test("serialize time is linear in values x keys", (a) => {
    const small = build(10000), big = build(160000);
    const ts = bestMs(() => small.serialize(), 5).ms, tb = bestMs(() => big.serialize(), 3).ms;
    perfCheck(a.ok, tb <= 64 * Math.max(ts, 0.5), "160K/10K serialize ratio " + (tb / ts).toFixed(1) + " (" + tb.toFixed(1) + " ms vs " + ts.toFixed(1) + " ms)", a.skip);
});

t.test("cells come out in key order after interleaved puts", (a) => {
    const m = new Multimap();
    for (let i = 0; i < 300; i++) m.put("k" + (i % 7), i);
    const back = Multimap.deserialize(m.serialize());
    for (let k = 0; k < 7; k++) a.deep(back.get("k" + k), m.get("k" + k), "k" + k);
    a.eq(back.size, m.size);
});

await t.run();
