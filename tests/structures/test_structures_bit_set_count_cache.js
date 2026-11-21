// covers: dyna:structures.BitSet -- the opt-in count cache ({ cache: true | { count: true } }) answers count exactly as the uncached set
// With the cache on, set/clear/flip adjust a kept population count by the bit they change, and/or/xor
// mark it stale so the next count recounts once. The cache is derived state: serialize() bytes are
// the same with it on and off, and a deserialized set starts with it off. Checked by running the
// same random op sequence against a cached and an uncached set and comparing after every op.
import { suite } from "../lib/kit.js";
import { BitSet } from "dyna:structures";

const t = suite("bit_set_count_cache");
const same = (x, y) => x.length === y.length && x.every((v, i) => v === y[i]);

t.test("cached and uncached sets agree after every op", (a) => {
    let s = 99;
    const rnd = (n) => (s = (s * 1103515245 + 12345) >>> 0) % n;
    for (const span of [64, 130, 5000, 70000]) {
        const on = new BitSet(span >> 1, { cache: { count: true } });
        const off = new BitSet(span >> 1);
        const mk = () => { const x = new BitSet(); for (let i = 0; i < 40; i++) x.set(rnd(span * 2)); return x; };
        let bad = 0;
        for (let op = 0; op < 4000; op++) {
            const k = rnd(20), i = rnd(span);
            if (k < 7) { on.set(i); off.set(i); }
            else if (k < 11) { on.clear(i); off.clear(i); }
            else if (k < 16) { on.flip(i); off.flip(i); }
            else if (k < 17) { const o = mk(); on.and(o); off.and(o); }
            else if (k < 18) { const o = mk(); on.or(o); off.or(o); }
            else if (k < 19) { const o = mk(); on.xor(o); off.xor(o); }
            else { on.clear(span * 4 + i); off.clear(span * 4 + i); on.set(i); off.set(i); }
            if (on.count !== off.count) bad++;
        }
        a.eq(bad, 0, "span " + span + ": count matches after every op");
        a.ok(same(on.toArray(), off.toArray()), "span " + span + ": same bits");
        a.ok(same([...on.serialize()], [...off.serialize()]), "span " + span + ": serialize() is identical with the cache on");
        const back = BitSet.deserialize(on.serialize());
        a.eq(back.count, off.count, "span " + span + ": a deserialized set counts the same");
    }
});

t.test("the cache option: spellings, idempotent writes, refusals", (a) => {
    const b = new BitSet(0, { cache: true });
    b.set(3).set(3).flip(5).flip(5).flip(5).clear(9).clear(3).set(200);
    a.eq(b.count, 2, "repeated set/clear of the same bit adjusts once");
    a.eq(new BitSet(8, { cache: false }).set(1).count, 1);
    a.eq(new BitSet(8, {}).set(1).count, 1);
    a.eq(new BitSet(8, { cache: { count: false } }).set(1).set(2).count, 2);
    a.throws(() => new BitSet(8, { cahce: true }), /unknown constructor option "cahce"/);
    a.throws(() => new BitSet(8, { cache: { rank: true } }), /unknown cache option "rank"/);
    a.throws(() => new BitSet(8, { cache: 1 }), TypeError);
    a.throws(() => new BitSet(8, 5), TypeError);
});

await t.run();
