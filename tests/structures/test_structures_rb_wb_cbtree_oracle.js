// covers: contract dyna:structures.RBTree dyna:structures.WBTree -- RBTree and WBTree storage moved from per-node heap trees (ds-rbtree.c, ds-wbtree.c, now deleted) to the counted B-tree shared with SkipList (ds-cbtree.c) with identical behaviour and records
// The digests below were computed by the fixA5 build (red-black and weight-balanced node trees)
// over seeded random sequences of set/delete/floorKey/ceilKey/get (plus rank/at for WBTree) with
// number and string keys at N = 128, 1000 and 20000: the first digest covers every answer plus the
// final entries, the second the serialize() bytes (CRC trailer excluded). The B-tree build must
// reproduce both. A SkipList fed the same sequence must give the same answers; at pin time
// (fixA5) SkipList was already the B-tree and RBTree/WBTree the old node trees, so the pins were
// cross-checked between independent code. (BTree cannot serve: number keys only.)
import { suite } from "../lib/kit.js";
import { RBTree, WBTree, SkipList } from "dyna:structures";
import { CRC32C } from "dyna:hash";

const t = suite("rb_wb_cbtree_oracle");
const enc = new TextEncoder();
const PINS = {
    rbn128: ["55419baf", "2e8743c2"], rbn1000: ["ba97512d", "9a86d3f3"], rbn20000: ["eab5af71", "80a16052"],
    rbs128: ["873ad846", "5d3b03a7"], rbs1000: ["5d108d1e", "5e5f2dd2"], rbs20000: ["5e002cbd", "d87ee1da"],
    wbn128: ["b42abeb7", "ccd17ae3"], wbn1000: ["a8627c56", "13e485a4"], wbn20000: ["5c43b04f", "952e4f68"],
    wbs128: ["cfd5dcfd", "6a61cf7"], wbs1000: ["b6485b91", "326a57e8"], wbs20000: ["9385632c", "cfbe7651"],
};

function run(C, strings, N, ranked) {
    let s = N * 13 + (strings ? 1 : 0) + (ranked ? 2 : 0);
    const rnd = (n) => (s = (s * 1103515245 + 12345) >>> 0) % n;
    const m = new C(strings ? { keys: "string" } : undefined);
    let log = "";
    const key = () => strings ? "k" + rnd(N * 2) : (rnd(N * 2) - N / 2) / 4;
    for (let op = 0; op < N * 4; op++) {
        const k = key(), c = rnd(10);
        if (c < 5) m.set(k, op);
        else if (c < 7) log += m.delete(k) ? "d" : "n";
        else if (c < 8 && ranked) log += m.rank(k) + ",";
        else if (c < 9 && ranked) { const a = m.at(rnd(m.size + 2) - 1); log += a ? a[0] + ":" + a[1] : "u"; }
        else log += String(m.floorKey(k)) + "/" + String(m.ceilKey(k)) + "/" + m.get(k) + "/" + m.has(k) + ";";
    }
    log += m.size + "|" + m.firstKey() + "|" + m.lastKey() + "|" + JSON.stringify(m.entries());
    log += "|" + JSON.stringify(m.keys()) + JSON.stringify(m.values()) + JSON.stringify([...m]);
    return [m, (CRC32C(enc.encode(log)) >>> 0).toString(16)];
}

const recDigest = (m) => { const rec = m.serialize(); return (CRC32C(rec.subarray(0, rec.length - 4)) >>> 0).toString(16); };

if (globalThis.scriptArgs?.[1] === "--pin") {
    const out = {};
    for (const [nm, C, ranked] of [["rb", RBTree, false], ["wb", WBTree, true]])
        for (const strings of [false, true])
            for (const N of [128, 1000, 20000]) {
                const [m, d] = run(C, strings, N, ranked);
                out[nm + (strings ? "s" : "n") + N] = [d, recDigest(m)];
            }
    print(JSON.stringify(out));
} else {
    for (const [nm, C, ranked] of [["rb", RBTree, false], ["wb", WBTree, true]]) {
        for (const strings of [false, true]) {
            for (const N of [128, 1000, 20000]) {
                const name = nm + (strings ? "s" : "n") + N;
                t.test(name + ": answers and records equal the node-tree build", (a) => {
                    const [m, d] = run(C, strings, N, ranked);
                    a.eq(d, PINS[name][0], "answers digest");
                    a.eq(recDigest(m), PINS[name][1], "record digest");
                    a.eq(run(SkipList, strings, N, ranked)[1], d, "a SkipList gives the same answers");
                    const back = C.deserialize(m.serialize());
                    a.eq(JSON.stringify(back.entries()), JSON.stringify(m.entries()), "the record reloads");
                    a.eq(back.constructor, C, "the record reloads as the same class");
                });
            }
        }
    }

    t.test("RBTree keeps no rank or at; WBTree and RBTree shrink to empty and grow again", (a) => {
        const r = new RBTree();
        a.eq(typeof r.rank, "undefined", "RBTree has no rank");
        a.eq(typeof r.at, "undefined", "RBTree has no at");
        for (const C of [RBTree, WBTree]) {
            const m = new C();
            for (let i = 0; i < 50000; i++) m.set((i * 7919) % 50000, i);
            a.eq(m.size, 50000);
            a.eq(m.floorKey(25000.5), 25000);
            for (let i = 0; i < 50000; i++) m.delete(i);
            a.eq(m.size, 0);
            a.eq(m.firstKey(), undefined);
            m.set(3, "x");
            a.eq(JSON.stringify(m.entries()), '[[3,"x"]]');
        }
    });

    await t.run();
}
