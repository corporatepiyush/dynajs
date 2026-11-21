// covers: contract dyna:structures.* -- incremental save of the raw-section classes and the full-save fallback of the rest
// BloomFilter, CountMinSketch and HyperLogLog save their arrays as raw paged
// sections. With { incremental: true } a later save appends only the 16 KiB
// chunks that changed and commits through the other header slot. At N = 128,
// 1K, 10K, 50K and 100K this suite checks, in copy and mmap mode:
//   - the first save is full; after a small change the next is incremental,
//     writes fewer bytes than the file holds, and loads to the new state;
//   - an unchanged object saves incrementally with almost nothing written;
//   - repeated rounds stay loadable and finally compact into a full save;
//   - an object fresh from load (mmap-borrowed arrays included) first writes a
//     full save to its own path, then goes incremental, and the mapping it was
//     loaded from keeps reading the old state;
//   - saving to a new path, onto a file older than the last save, or onto a
//     file written by another object, is a full save.
// Every other class answers incremental: true with a full save.
import * as S0 from "dyna:structures";
import { Graph as Graph0 } from "dyna:graph";
const S = { ...S0, Graph: Graph0 };
import { makeTempDir, removeAll, readBytes, writeFile } from "dyna:file";
import { suite } from "../lib/kit.js";
import { PERSIST, SIZES, bytesEqual } from "../lib/persist_build.js";

const t = suite("structures_persist_incremental");
const dir = makeTempDir("persist-inc-");
t.after(() => removeAll(dir));

const RAW = {
    BloomFilter: (o, r) => { for (let i = 0; i < 3; i++) o.add("r" + r + "-" + i); },
    CountMinSketch: (o, r) => { o.add("r" + r, 7); },
    HyperLogLog: (o, r) => { for (let i = 0; i < 40; i++) o.add("r" + r + "-" + i); },
};
const INC = { incremental: true, fsync: false };

for (const [name, change] of Object.entries(RAW)) {
    const C = S[name];
    t.test(name + " incremental saves at every size", (a) => {
        for (const n of SIZES) {
            const tag = name + " n=" + n;
            const p = dir.join(name + "-" + n + ".dp");
            const o = PERSIST[name].build(n);
            const r0 = o.save(p, INC);
            a.eq(r0.incremental, false, tag + " first save is full");
            change(o, 1);
            const r1 = o.save(p, INC);
            a.ok(r1.incremental && r1.written < r1.bytes, tag + " small change is incremental " + JSON.stringify(r1));
            for (const mmap of [false, true])
                a.ok(bytesEqual(C.load(p, { mmap }).serialize(), o.serialize()), tag + (mmap ? " mmap" : " copy") + " loads the new state");
            const r2 = o.save(p, INC);
            a.ok(r2.incremental && r2.written <= 4096, tag + " unchanged save writes only the slot and table, wrote " + r2.written);
            let rounds = 0, compacted = false;
            for (; rounds < 400 && !compacted; rounds++) {
                for (let k = 0; k < 64; k++) change(o, 100 + rounds * 64 + k);
                const r = o.save(p, INC);
                compacted = !r.incremental;
                if (rounds % 7 === 0 || compacted)
                    a.ok(bytesEqual(C.load(p, { mmap: (rounds & 1) === 1 }).serialize(), o.serialize()), tag + " round " + rounds + " loads");
            }
            a.ok(compacted, tag + " growth ends in a compacting full save after " + rounds + " rounds");

            for (const mmap of [false, true]) {
                const L = C.load(p, { mmap });
                const before = L.serialize();
                change(L, 9000 + (mmap ? 1 : 0));
                const f1 = L.save(p, INC);
                a.eq(f1.incremental, false, tag + (mmap ? " mmap" : " copy") + " first save after load is full");
                a.ok(bytesEqual(C.deserialize(before).serialize(), before), tag + " pre-change snapshot is intact");
                change(L, 9100);
                const f2 = L.save(p, INC);
                a.ok(f2.incremental, tag + (mmap ? " mmap" : " copy") + " second save after load is incremental");
                a.ok(bytesEqual(C.load(p, { mmap: !mmap }).serialize(), L.serialize()), tag + " loaded-then-saved state reloads");
            }

            const q = dir.join(name + "-" + n + "-other.dp");
            o.save(p, INC);
            a.eq(o.save(q, INC).incremental, false, tag + " a new path gets a full save");
            change(o, 77777);
            a.eq(o.save(p, INC).incremental, false, tag + " a file older than the last save (stale gen) gets a full save");
            a.ok(bytesEqual(C.load(p).serialize(), o.serialize()), tag + " and it loads");
            PERSIST[name].build(n).save(q, INC);
            writeFile(p, readBytes(q));
            change(o, 88888);
            a.eq(o.save(p, INC).incremental, false, tag + " a file from another object (lineage) gets a full save");
            a.ok(bytesEqual(C.load(p, { mmap: true }).serialize(), o.serialize()), tag + " and it loads");
        }
    });
}

t.test("record classes answer incremental with a full save", (a) => {
    for (const [name, d] of Object.entries(PERSIST)) {
        if (RAW[name]) continue;
        const p = dir.join(name + "-rec.dp");
        const o = d.build(128);
        o.save(p, INC);
        d.mutate(o, 128);
        const r = o.save(p, INC);
        a.ok(r.incremental === false && r.written === r.bytes, name + " full save " + JSON.stringify(r));
    }
});

await t.run();
