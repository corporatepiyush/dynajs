// covers: contract dyna:structures.* -- save/load argument and IO errors, and state after a failed save
// save and load take a dyna:file Path; a string, a missing path or a
// non-boolean option is a TypeError. IO failures are Error objects with
// code EPERSIST_IO and the errno. A failed save must not poison the next one:
// save hashes the arrays before writing, so if a failure left those hashes
// marked valid, a later incremental save to the old file would skip the
// chunks that changed and the file would load stale or fail its checksum.
// That case runs for the raw-section classes at every size. Heap.load runs the
// caller's compare while rebuilding; an error from it must reach the caller as
// thrown, not relabelled as file corruption.
import * as S0 from "dyna:structures";
import { Graph as Graph0 } from "dyna:graph";
const S = { ...S0, Graph: Graph0 };
import { makeTempDir, readDir, removeAll } from "dyna:file";
import { suite } from "../lib/kit.js";
import { PERSIST, SIZES, bytesEqual } from "../lib/persist_build.js";

const t = suite("structures_persist_save_errors");
const dir = makeTempDir("persist-err-");
t.after(() => removeAll(dir));

t.test("arguments are checked before any IO", (a) => {
    const b = new S.BloomFilter(1024, 3);
    a.throws(() => b.save(dir.join("x").toString()), TypeError, "string path");
    a.throws(() => b.save(), TypeError, "missing path");
    a.throws(() => b.save(dir.join("x"), { fsync: 1 }), TypeError, "non-boolean fsync");
    a.throws(() => b.save(dir.join("x"), { incremental: "yes" }), TypeError, "non-boolean incremental");
    a.throws(() => b.save(dir.join("x"), 5), TypeError, "non-object options");
    a.throws(() => S.BloomFilter.load("x"), TypeError, "load with a string");
    a.throws(() => S.BloomFilter.load(dir.join("x"), { mmap: "y" }), TypeError, "non-boolean mmap");
    a.throws(() => S.Heap.load(dir.join("x"), { compare: 3 }), TypeError, "non-function compare");
    a.throws(() => S.BloomFilter.prototype.save.call({}, dir.join("x")), TypeError, "foreign receiver");
});

t.test("IO errors carry EPERSIST_IO and errno", (a) => {
    const missing = dir.join("no", "such", "dir", "f.dp");
    let e = a.throws(() => new S.LRU(4).save(missing), { code: "EPERSIST_IO" }, "save into a missing dir");
    a.ok(e && e.errno > 0 && e.message.includes("LRU.save"), "errno and class in " + (e && e.message));
    e = a.throws(() => S.LRU.load(missing), { code: "EPERSIST_IO" }, "load a missing file");
    a.ok(e && e.errno > 0, "load errno");
    e = a.throws(() => S.LRU.load(dir), Error, "load a directory");
    a.match(e && e.code, /^EPERSIST_/, "directory load code");
    const empty = dir.join("empty.dp");
    new S.LRU(1).save(empty);
    a.deep(readDir(dir).map((x) => x.name).filter((x) => x.includes(".tmp.")), [], "no temp file is left behind");
});

t.test("Heap.load passes an error thrown by compare through unchanged", (a) => {
    const p = dir.join("heap.dp");
    const h = new S.Heap((x, y) => x - y);
    for (let i = 0; i < 50; i++) h.push(i);
    h.save(p);
    const mine = new RangeError("compare refused");
    let got = null;
    try { S.Heap.load(p, { compare: () => { throw mine; } }); } catch (e) { got = e; }
    a.ok(got === mine, "the same error object comes back");
    a.eq(got && got.code, undefined, "and no code is added to it");
    a.eq(S.Heap.load(p, { compare: (x, y) => x - y }).peek(), 0, "a good compare loads");
});

const RAW = {
    BloomFilter: (o, r) => { for (let i = 0; i < 5; i++) o.add("f" + r + "-" + i); },
    CountMinSketch: (o, r) => { o.add("f" + r, 3); },
    HyperLogLog: (o, r) => { for (let i = 0; i < 60; i++) o.add("f" + r + "-" + i); },
};

for (const [name, change] of Object.entries(RAW)) {
    t.test(name + " a failed save does not poison the next incremental save", (a) => {
        for (const n of SIZES) {
            const p = dir.join(name + "-" + n + ".dp");
            const o = PERSIST[name].build(n);
            o.save(p, { fsync: false });
            change(o, 1);
            a.throws(() => o.save(dir.join("gone", "x.dp"), { incremental: true }), { code: "EPERSIST_IO" }, name + " n=" + n + " failing save");
            const r = o.save(p, { incremental: true, fsync: false });
            a.ok(bytesEqual(S[name].load(p, { mmap: (n & 1024) !== 0 }).serialize(), o.serialize()),
                name + " n=" + n + " the save after a failure carries every change (" + JSON.stringify(r.incremental) + ")");
        }
    });
}

await t.run();
