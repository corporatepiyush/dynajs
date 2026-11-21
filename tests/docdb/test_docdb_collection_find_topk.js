// covers: dyna:docdb.Collection.find
// A find with a sort and a limit whose filter is answered by a single-field, non-multikey index on the sort's leading
// field walks that index's keys in sort order and stops after the key group in which skip+limit documents (that pass
// the whole filter) have been collected, instead of gathering every match and sorting it. Each key is one sort value,
// so the whole tie group at the cutoff is kept and the full sort (with its later keys) still orders the candidates;
// the answer must therefore equal an unindexed twin collection's for ties, skip, residual predicates, descending
// indexes, several intervals ($in, $or-free unions), mixed types, deleted documents, and an index that becomes
// multikey (which falls back to the full scan). A sort led by a different index than the filter's (or a filter with no
// index) walks that sort index testing the whole filter, under a budget that gives up and sorts the matches when the
// filter is anti-correlated with the sort; sparse, partial and multikey indexes are never walked for a sort.
import { suite } from "../lib/kit.js";
import { Database } from "dyna:docdb";
import { Random } from "dyna:random";

const t = suite("docdb.Collection.find top-k index walk");
const db = new Database("topk");
const rng = new Random(77);
const docs = Array.from({ length: 3000 }, (_, i) => {
    const r = rng.nextBounded(20);
    const v = r === 0 ? null : r === 1 ? undefined : r === 2 ? `s${rng.nextBounded(9)}` : r === 3 ? rng.nextBounded(50) + 0.5 : rng.nextBounded(50);
    const d = { _id: i, b: rng.nextBounded(4), c: rng.nextBounded(1000) };
    if (v !== undefined) d.v = v;
    return d;
});
const ix = db.collection("ix");
const dx = db.collection("dx");
const plain = db.collection("plain");
for (const c of [ix, dx, plain]) c.insertMany(docs.map((d) => ({ ...d })));
ix.createIndex({ v: 1 });
dx.createIndex({ v: -1 });
const ids = (c, f, o) => c.find(f, { ...o, projection: { _id: 1 } }).map((d) => d._id).join();

const filters = [
    { v: { $gte: 30 } },
    { v: { $lt: 10 } },
    { v: { $gt: 5, $lte: 40 } },
    { v: { $in: [7, 3, 41, 12.5, "s4", null] } },
    { v: { $gte: 20 }, b: 2 },
    { v: { $gte: "s" } },
    { v: { $type: "number" }, c: { $gt: 900 } },
];
const sorts = [{ v: 1 }, { v: -1 }, { v: 1, _id: -1 }, { v: -1, c: 1 }, { v: -1, _id: 1 }];

t.test("every filter, sort direction, skip and limit agrees with the unindexed twin on both index directions", (a) => {
    let n = 0;
    for (const f of filters)
        for (const sort of sorts)
            for (const [skip, limit] of [[0, 1], [0, 7], [3, 20], [40, 50], [0, 5000]]) {
                const want = ids(plain, f, { sort, skip, limit });
                a.eq(ids(ix, f, { sort, skip, limit }), want, JSON.stringify([f, sort, skip, limit]));
                a.eq(ids(dx, f, { sort, skip, limit }), want, JSON.stringify(["desc", f, sort, skip, limit]));
                n++;
            }
    a.eq(n, filters.length * sorts.length * 5);
});

t.test("the walk stops early: a small limit examines far fewer keys and documents than the matches", (a) => {
    const f = { v: { $gte: 0 } };
    const full = ix.explain(f, { sort: { v: -1, _id: 1 } }).executionStats;
    const top = ix.explain(f, { sort: { v: -1, _id: 1 }, limit: 5 }).executionStats;
    a.eq(top.nReturned, 5);
    a.ok(full.totalDocsExamined > 2000, `full scan examined ${full.totalDocsExamined}`);
    a.ok(top.totalDocsExamined < 200, `top-k examined ${top.totalDocsExamined} documents`);
    a.ok(top.totalKeysExamined < full.totalKeysExamined, `${top.totalKeysExamined} keys vs ${full.totalKeysExamined}`);
});

t.test("a residual predicate that rejects the first key groups still yields skip+limit documents", (a) => {
    const f = { v: { $gte: 0 }, c: { $lt: 15 } };
    for (const limit of [1, 3, 10]) a.eq(ids(ix, f, { sort: { v: -1 }, limit }), ids(plain, f, { sort: { v: -1 }, limit }));
});

t.test("deleted and updated documents leave the walk exact", (a) => {
    for (const c of [ix, dx, plain]) {
        c.deleteMany({ v: { $gte: 45 } });
        c.updateMany({ v: 44 }, { $set: { v: 2 } });
        c.updateMany({ _id: { $lt: 50 } }, { $inc: { c: 1 } });
    }
    for (const sort of sorts) a.eq(ids(ix, { v: { $gte: 1 } }, { sort, limit: 9 }), ids(plain, { v: { $gte: 1 } }, { sort, limit: 9 }));
    for (const sort of sorts) a.eq(ids(dx, { v: { $lte: 30 } }, { sort, limit: 9 }), ids(plain, { v: { $lte: 30 } }, { sort, limit: 9 }));
});

t.test("an index that turns multikey falls back to the full sort (desc sort keys use an array's largest element)", (a) => {
    for (const c of [ix, plain]) c.insertMany([{ _id: "m1", v: [1, 99] }, { _id: "m2", v: [3, 98] }, { _id: "m3", v: [40] }]);
    for (const sort of sorts)
        for (const f of [{ v: { $lte: 5 } }, { v: { $gte: 2 } }])
            a.eq(ids(ix, f, { sort, limit: 4 }), ids(plain, f, { sort, limit: 4 }), JSON.stringify([f, sort]));
});

const N2 = 3000;
const docs2 = Array.from({ length: N2 }, (_, i) => {
    const d = { _id: i, b: rng.nextBounded(4), c: rng.nextBounded(300), k: i, r: N2 - i, p: rng.nextBounded(100) };
    if (i % 5) d.w = rng.nextBounded(40);
    if (i % 7 === 0) d.c = i % 2 ? null : `t${i % 11}`;
    return d;
});
const ox = db.collection("ox");
const plain2 = db.collection("plain2");
for (const c of [ox, plain2]) c.insertMany(docs2.map((d) => ({ ...d })));
for (const f of ["b", "c", "k", "r"]) ox.createIndex({ [f]: 1 });
ox.createIndex({ w: -1 }, { sparse: true });
const sorts2 = [{ c: 1 }, { c: -1 }, { c: 1, _id: -1 }, { c: -1, k: 1 }, { k: 1 }, { k: -1 }];

t.test("a filter answered by one index and a sort led by another walks the sort index; every shape agrees with the twin", (a) => {
    const fs = [{ b: 2 }, { b: { $in: [1, 3] } }, { b: 0, w: { $gte: 20 } }, { r: { $lte: 2000 } }, { $or: [{ b: 1 }, { r: { $lt: 100 } }] }, {}, { w: { $exists: true } }];
    let n = 0;
    for (const f of fs)
        for (const sort of sorts2)
            for (const [skip, limit] of [[0, 1], [0, 10], [7, 13], [0, 3000]]) {
                a.eq(ids(ox, f, { sort, skip, limit }), ids(plain2, f, { sort, skip, limit }), JSON.stringify([f, sort, skip, limit]));
                n++;
            }
    a.eq(n, fs.length * sorts2.length * 4);
});

t.test("the sort-index walk stops early when matches are dense, for an indexed filter and for a scan", (a) => {
    for (const f of [{ b: 2 }, {}, { w: { $gte: 0 } }]) {
        const full = ox.explain(f, { sort: { k: 1 } }).executionStats;
        const top = ox.explain(f, { sort: { k: 1 }, limit: 10 }).executionStats;
        a.eq(top.nReturned, 10);
        a.ok(top.totalDocsExamined < 100, `${JSON.stringify(f)}: top-k examined ${top.totalDocsExamined} of ${full.totalDocsExamined}`);
    }
});

t.test("a filter anti-correlated with the sort exhausts the walk budget and falls back to the exact full sort", (a) => {
    for (const [f, sort] of [[{ r: { $lte: 1500 } }, { k: 1 }], [{ r: { $gte: 2990 } }, { k: -1 }], [{ b: 1, r: { $lte: 1200 } }, { k: 1, _id: 1 }]]) {
        const got = ox.find(f, { sort, limit: 5 }).map((d) => d._id).join();
        a.eq(got, ids(plain2, f, { sort, limit: 5 }), JSON.stringify([f, sort]));
        a.eq(ox.explain(f, { sort, limit: 5 }).executionStats.nReturned, 5);
    }
});

t.test("a sparse, partial or multikey index on the sort field is never walked for the sort", (a) => {
    ox.createIndex({ p: 1 }, { partialFilterExpression: { b: { $gt: 1 } } });
    for (const sort of [{ w: -1 }, { w: 1 }, { p: 1 }, { p: -1 }])
        for (const f of [{ b: 3 }, { b: 0 }, { b: { $gte: 1 } }, {}])
            a.eq(ids(ox, f, { sort, limit: 6 }), ids(plain2, f, { sort, limit: 6 }), JSON.stringify([f, sort]));
    for (const c of [ox, plain2]) c.insertMany([{ _id: "m1", b: 2, k: [5000, -1] }, { _id: "m2", b: 2, k: [2999.5] }, { _id: "m3", b: 2, k: [1.5, 0.5] }]);
    for (const sort of [{ k: 1 }, { k: -1 }])
        for (const f of [{ b: 2 }, {}])
            a.eq(ids(ox, f, { sort, limit: 4 }), ids(plain2, f, { sort, limit: 4 }), JSON.stringify(["multikey", f, sort]));
});

await t.run();
db.close();
