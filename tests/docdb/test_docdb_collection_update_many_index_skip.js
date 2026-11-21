// covers: dyna:docdb.Collection.updateMany
// P5: an update rewrote every index of the collection for every changed document (unique check, remove the old
// keys, add the new keys) even when the update never touched an indexed field, so updateMany({}, {$set: {flag}})
// with 3 single-field indexes cost 1.95x the same update with none (117 vs 60 ms at 100K). An index is now left
// alone when it has no partial filter and every top-level field its key paths start at ("d" for "d.x", "a" for
// "a.1") holds byte-identical BSON (same type, length and bytes, or missing in both) before and after: the keys
// are a function of those values only, so they cannot have changed. A partial index is always maintained, and so
// is every index on a replacement (replaceOne, findOneAndReplace, bulkWrite replaceOne): there the walk cost 1-3%
// and almost never finds an untouched index. The changed top-level fields come from one lockstep walk of the old
// and new document (an update keeps field order), which stops once every index is known to need maintenance;
// when the walks disagree on a key, each remaining index field is looked up in both documents instead.
// Oracles: (1) every index answers exactly what a scan of an unindexed twin answers after updates that do and
// do not touch indexed fields, through dotted, numeric, multikey, compound, sparse, partial and unique indexes,
// including a change that keeps the value but changes its BSON type (int32 1 -> int64 1n) and changes below the indexed
// field's top level; (2) the work is gone: with 6 indexes on untouched fields the update costs under 1.6x the
// unindexed one (thread CPU, best of 5; base 4.3-4.9x, fixed 1.31-1.33x), so a regression that rewrites
// untouched indexes again fails here without a wall clock.
import { suite } from "../lib/kit.js";
import { Database, DocDBError } from "dyna:docdb";
import { threadCpuNow } from "dyna:sys";
import { perfCheck } from "../lib/perf.js";

const t = suite("docdb.Collection.updateMany index maintenance skip");
const db = new Database("ixskip");
const J = JSON.stringify;

function pair(name, specs) {
    const ix = db.collection(name), plain = db.collection(name + "_plain");
    for (const [keys, opts] of specs) ix.createIndex(keys, opts || {});
    return { ix, plain, both: (fn) => { fn(ix); fn(plain); } };
}
function sameAnswers(a, p, filters, label) {
    let bad = 0;
    for (const f of filters) {
        const got = J(p.ix.find(f, { sort: { _id: 1 } })), want = J(p.plain.find(f, { sort: { _id: 1 } }));
        if (got !== want && bad++ < 3) a.eq(got, want, `${label} ${J(f)}`);
    }
    a.eq(bad, 0, label);
}

t.test("updates that miss, hit, or sit beside indexed fields leave every index equal to a scan", (a) => {
    const p = pair("mix", [
        [{ k: 1 }],
        [{ "d.x": 1 }],
        [{ "arr.1": 1 }],
        [{ "tags": 1 }],
        [{ k: 1, "d.x": -1 }],
        [{ s: 1 }, { sparse: true }],
        [{ q: 1 }, { partialFilterExpression: { flag: { $exists: true } } }],
    ]);
    const docs = Array.from({ length: 300 }, (_, i) => ({ _id: i, k: i % 13, d: { x: i % 7, y: i }, arr: [i % 3, i % 5, i % 11], tags: ["t" + (i % 4), "u" + (i % 6)], s: i % 2 ? i % 9 : undefined, q: i % 17, n: i }));
    for (const d of docs) if (d.s === undefined) delete d.s;
    p.both((c) => c.insertMany(docs));
    const steps = [
        [{}, { $inc: { n: 1 } }],
        [{ k: 3 }, { $set: { "d.y": -1 } }],
        [{ k: { $lt: 5 } }, { $set: { "d.x": 99 } }],
        [{ n: { $gt: 200 } }, { $set: { "arr.1": 42 } }],
        [{ k: 4 }, { $push: { tags: "new" } }],
        [{ k: 5 }, { $unset: { s: "" } }],
        [{ k: 6 }, { $set: { s: 1 } }],
        [{ k: 7 }, { $set: { flag: true } }],
        [{ k: 8 }, { $set: { flag: true, q: 1000 } }],
        [{ k: 8, n: { $lt: 150 } }, { $unset: { flag: "" } }],
        [{ k: 9 }, { $set: { k: 9.0 } }],
        [{ k: 10 }, { $set: { k: 10n } }],
        [{ k: 11 }, { $rename: { k: "kk" } }],
        [{ kk: 11 }, { $rename: { kk: "k" } }],
        [{}, { $set: { "d.z": [1, 2] } }],
        [{ k: 12 }, { $set: { d: { y: 0, x: 3 } } }],
        [{ k: 3 }, { $rename: { n: "m" }, $set: { "d.x": 55 } }],
    ];
    for (const [f, u] of steps) p.both((c) => c.updateMany(f, u));
    const filters = [];
    for (let v = 0; v < 18; v++) filters.push({ k: v }, { "d.x": v }, { "arr.1": v }, { s: v }, { q: v, flag: { $exists: true } }, { k: v, "d.x": { $gte: 3 } });
    filters.push({ "d.x": 99 }, { "d.x": 55 }, { m: { $exists: true } }, { "arr.1": 42 }, { tags: "new" }, { tags: "t1" }, { s: { $exists: false } }, { q: 1000, flag: true }, { k: { $type: "double" } }, { k: { $type: "long" } }, { kk: { $exists: true } });
    sameAnswers(a, p, filters, "mix");
    a.eq(J(p.ix.find({ k: 10 }, { projection: { _id: 1 } }).length), J(p.plain.find({ k: 10 }).length));
    const entries = db.stats().indexEntries;
    for (const spec of p.ix.listIndexes().filter((s) => s.name !== "_id_")) {
        const opts = { name: spec.name };
        if (spec.sparse) opts.sparse = true;
        if (spec.partialFilterExpression) opts.partialFilterExpression = spec.partialFilterExpression;
        p.ix.dropIndex(spec.name);
        p.ix.createIndex(spec.key, opts);
    }
    a.eq(db.stats().indexEntries, entries, "index entries after the updates equal those of indexes rebuilt from the documents");
});

t.test("a unique index (plain or partial) still refuses a duplicate written by an update, and accepts an update that keeps its key", (a) => {
    const p = pair("uniq", [[{ u: 1 }, { unique: true }], [{ "d.v": 1 }, { unique: true }]]);
    p.ix.insertMany([{ _id: 1, u: 1, d: { v: 1, w: 0 } }, { _id: 2, u: 2, d: { v: 2, w: 0 } }]);
    a.eq(p.ix.updateMany({}, { $inc: { "d.w": 1 } }).modifiedCount, 2);
    let err = null;
    try { p.ix.updateOne({ _id: 2 }, { $set: { u: 1 } }); } catch (e) { err = e; }
    a.ok(err instanceof DocDBError && err.code === 11000, String(err));
    err = null;
    try { p.ix.updateOne({ _id: 2 }, { $set: { "d.v": 1 } }); } catch (e) { err = e; }
    a.ok(err instanceof DocDBError && err.code === 11000, String(err));
    a.eq(J(p.ix.find({ u: 2 }).map((d) => d.d.w)), "[1]");
    a.eq(J(p.ix.find({ "d.v": 2 }).map((d) => d._id)), "[2]");
    const pu = db.collection("uniq_partial");
    pu.createIndex({ pu: 1 }, { unique: true, partialFilterExpression: { on: { $exists: true } } });
    pu.insertMany([{ _id: 3, pu: 7 }, { _id: 4, pu: 7, on: 1 }]);
    err = null;
    try { pu.updateOne({ _id: 3 }, { $set: { on: 1 } }); } catch (e) { err = e; }
    a.ok(err instanceof DocDBError && err.code === 11000, "entering a unique partial index through its filter field alone: " + String(err));
});

t.test("updating an unindexed field does not pay for untouched indexes", (a) => {
    const N = 20000;
    const docs = Array.from({ length: N }, (_, i) => ({ _id: i, a: i % 97, b: "s" + (i % 1000), c: i % 13, d: { e: i % 31 }, f: [i % 5, i % 7], g: i, n: 0 }));
    const cost = (nix) => {
        let best = Infinity;
        for (let r = 0; r < 5; r++) {
            db.dropCollection("cost");
            const c = db.collection("cost");
            if (nix) for (const k of ["a", "b", "c", "d.e", "f", "g"]) c.createIndex({ [k]: 1 });
            c.insertMany(docs);
            const t0 = threadCpuNow();
            c.updateMany({}, { $set: { n: r + 1 } });
            best = Math.min(best, threadCpuNow() - t0);
        }
        return best;
    };
    const plain = cost(0), indexed = cost(6);
    perfCheck((c, m) => a.ok(c, m), indexed < plain * 1.6, `6 untouched indexes cost ${(indexed / plain).toFixed(2)}x the unindexed update (${indexed.toFixed(1)} vs ${plain.toFixed(1)} ms CPU)`);
});

await t.run();
db.close();
