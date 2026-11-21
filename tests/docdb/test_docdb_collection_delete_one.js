// covers: dyna:docdb.Collection.deleteOne
// deleteOne removes the first match in slot order and reports deletedCount 0 or 1. Oracle: tests/lib/docdb_model.js
// replays 300 seeded deletes (filters on equality, ranges, $in, $exists, array membership, $or) on plain objects and
// the whole collection must equal the model after each one. Also pinned: a deleted document leaves every index (an
// indexed twin answers what an unindexed one answers), estimatedDocumentCount follows, the filter is required (a
// missing filter is a TypeError, {} deletes one document), and a delete on a collection that does not exist answers 0
// without creating it.
import { suite } from "../lib/kit.js";
import { Database } from "dyna:docdb";
import { rng, genDocs, filters, byId, clone, J } from "../lib/docdb_model.js";

const t = suite("docdb.Collection.deleteOne");

t.test("300 seeded deletes against the JS model", (a) => {
    const r = rng(0xde1), c = new Database("d1").collection("m");
    const model = genDocs(r, 400);
    c.insertMany(model.map(clone));
    for (let round = 0; round < 300; round++) {
        const fs = filters(r), [f, pred] = fs[1 + Math.floor(r() * (fs.length - 1))];
        const i = model.findIndex(pred);
        const res = c.deleteOne(f);
        if (i >= 0) model.splice(i, 1);
        if (!a.deep(res, { acknowledged: true, deletedCount: i >= 0 ? 1 : 0 }, "result " + J(f))) return;
        if (!a.eq(J(c.find({}, { sort: { _id: 1 } })), J(byId(model)), "collection after " + J(f) + " round " + round)) return;
        if (!a.eq(c.estimatedDocumentCount(), model.length, "count")) return;
    }
});

t.test("indexes drop the deleted document's keys", (a) => {
    const r = rng(5), db = new Database("d2"), ix = db.collection("ix"), plain = db.collection("plain");
    ix.createIndex({ a: 1 }); ix.createIndex({ tags: 1 }); ix.createIndex({ b: 1, n: -1 });
    const docs = genDocs(r, 300);
    ix.insertMany(docs.map(clone)); plain.insertMany(docs.map(clone));
    const before = db.stats().indexEntries;
    for (let i = 0; i < 120; i++) {
        const [f] = filters(r)[1 + Math.floor(r() * 9)];
        a.eq(ix.deleteOne(f).deletedCount, plain.deleteOne(f).deletedCount, "deletedCount " + J(f));
    }
    for (const [f] of filters(r)) a.eq(J(ix.find(f, { sort: { _id: 1 } })), J(plain.find(f, { sort: { _id: 1 } })), "indexed vs scan " + J(f));
    a.ok(db.stats().indexEntries < before, "index entries shrank");
});

t.test("{} deletes exactly one; the filter is required; a missing collection answers 0 and is not created", (a) => {
    const db = new Database("d3"), c = db.collection("c");
    c.insertMany([{ _id: 1 }, { _id: 2 }, { _id: 3 }]);
    a.eq(c.deleteOne({}).deletedCount, 1);
    a.eq(J(c.find()), J([{ _id: 2 }, { _id: 3 }]), "the first in slot order went");
    a.throws(() => c.deleteOne(), TypeError);
    a.eq(c.estimatedDocumentCount(), 2, "a refused call deleted nothing");
    a.deep(db.collection("none").deleteOne({ a: 1 }), { acknowledged: true, deletedCount: 0 });
    a.deep(db.listCollections(), ["c"]);
});

await t.run();
