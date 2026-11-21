// covers: dyna:structures.SkipList -- SkipList storage moved from linked skip-list nodes to the counted B-tree (ds-cbtree.c, nodes in a dyn_parr pool) with identical behaviour and records
// The digests below were computed by the skip-list build (fixA4) over seeded random sequences of
// set/delete/rank/at/floorKey/ceilKey/get with number and string keys at N = 128, 1000 and 20000:
// the first digest covers every answer plus the final entries, the second the serialize() bytes
// (CRC trailer excluded). The B-tree build must reproduce both. A WBTree (independent code) fed the
// same sequence must give the same answers, so the pins are not a self-check.
import { suite } from "../lib/kit.js";
import { SkipList, WBTree } from "dyna:structures";
import { CRC32C } from "dyna:hash";

const t = suite("skip_list_cbtree_oracle");
const enc = new TextEncoder();
const PINS = {
    n128: ["cee40985", "bfca7d83"], n1000: ["7b84c300", "ab835806"], n20000: ["6b1a6840", "fb9ca3e"],
    s128: ["899738b1", "970b1f2a"], s1000: ["de9ac879", "5058f8be"], s20000: ["926cd2fb", "ecf8996b"],
};

function run(C, strings, N) {
    let s = N * 7 + (strings ? 1 : 0);
    const rnd = (n) => (s = (s * 1103515245 + 12345) >>> 0) % n;
    const m = new C(strings ? { keys: "string" } : undefined);
    let log = "";
    const key = () => strings ? "k" + rnd(N * 2) : (rnd(N * 2) - N / 2) / 4;
    for (let op = 0; op < N * 4; op++) {
        const k = key(), c = rnd(10);
        if (c < 5) m.set(k, op);
        else if (c < 7) log += m.delete(k) ? "d" : "n";
        else if (c < 8) log += m.rank(k) + ",";
        else if (c < 9) { const a = m.at(rnd(m.size + 2) - 1); log += a ? a[0] + ":" + a[1] : "u"; }
        else log += String(m.floorKey(k)) + "/" + String(m.ceilKey(k)) + "/" + m.get(k) + ";";
    }
    log += m.size + "|" + m.firstKey() + "|" + m.lastKey() + "|" + JSON.stringify(m.entries());
    return [m, (CRC32C(enc.encode(log)) >>> 0).toString(16)];
}

for (const strings of [false, true]) {
    for (const N of [128, 1000, 20000]) {
        const name = (strings ? "s" : "n") + N;
        t.test(name + ": answers and records equal the skip-list build", (a) => {
            const [m, d] = run(SkipList, strings, N);
            const rec = m.serialize();
            a.eq(d, PINS[name][0], "answers digest");
            a.eq((CRC32C(rec.subarray(0, rec.length - 4)) >>> 0).toString(16), PINS[name][1], "record digest");
            a.eq(run(WBTree, strings, N)[1], d, "a WBTree gives the same answers");
            const back = SkipList.deserialize(rec);
            a.eq(JSON.stringify(back.entries()), JSON.stringify(m.entries()), "the record reloads");
        });
    }
}

t.test("a large map shrinks to empty and grows again", (a) => {
    const m = new SkipList();
    for (let i = 0; i < 50000; i++) m.set((i * 7919) % 50000, i);
    a.eq(m.size, 50000);
    a.eq(m.rank(25000), 25000);
    a.eq(m.at(-1)[0], 49999);
    for (let i = 0; i < 50000; i++) m.delete(i);
    a.eq(m.size, 0);
    a.eq(m.firstKey(), undefined);
    a.eq(m.at(0), undefined);
    m.set(3, "x");
    a.eq(JSON.stringify(m.entries()), '[[3,"x"]]');
});

await t.run();
