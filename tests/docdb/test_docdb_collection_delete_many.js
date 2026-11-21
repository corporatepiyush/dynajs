// covers: dyna:docdb.Collection.deleteMany
// deleteMany removes every match and reports how many. Oracle: tests/lib/docdb_model.js replays 120 seeded deletes
// (re-filling the collection when it runs low) and the remaining documents and deletedCount must equal the model's
// after each. Also pinned: an indexed twin (single, multikey and compound indexes) answers every filter like the
// unindexed one afterwards and its index entry count drops to zero with the documents, {} empties the collection but
// keeps it (and its indexes), a missing filter is a TypeError, and a missing collection answers 0 without being created.
import { suite } from "../lib/kit.js";
import { Database } from "dyna:docdb";
import { rng, genDocs, filters, byId, clone, J } from "../lib/docdb_model.js";

const t = suite("docdb.Collection.deleteMany");

t.test("120 seeded deleteMany calls against the JS model, plain and indexed", (a) => {
    const r = rng(0xde2), db = new Database("dm"), plain = db.collection("plain"), ix = db.collection("ix");
    ix.createIndex({ a: 1 }); ix.createIndex({ tags: 1 }); ix.createIndex({ b: 1, n: -1 });
    let model = [], next = 1;
    for (let round = 0; round < 120; round++) {
        if (model.length < 50) {
            const more = genDocs(r, 200, next); next += 200;
            model.push(...more);
            plain.insertMany(more.map(clone)); ix.insertMany(more.map(clone));
        }
        const fs = filters(r), [f, pred] = fs[1 + Math.floor(r() * (fs.length - 1))];
        const want = model.filter(pred).length;
        model = model.filter((d) => !pred(d));
        for (const c of [plain, ix]) {
            if (!a.deep(c.deleteMany(f), { acknowledged: true, deletedCount: want }, c.name + " " + J(f))) return;
            if (!a.eq(J(c.find({}, { sort: { _id: 1 } })), J(byId(model)), c.name + " after " + J(f) + " round " + round)) return;
        }
    }
    for (const [f, pred] of filters(r)) a.eq(J(ix.find(f, { sort: { _id: 1 } })), J(byId(model.filter(pred))), "indexed find " + J(f));
});

t.test("{} empties the collection, keeps it and its indexes, and drops every index entry", (a) => {
    const db = new Database("dm2"), c = db.collection("c");
    c.createIndex({ a: 1 });
    c.insertMany(genDocs(rng(9), 500));
    a.eq(db.stats().indexEntries, 500);
    a.deep(c.deleteMany({}), { acknowledged: true, deletedCount: 500 });
    a.eq(c.estimatedDocumentCount(), 0);
    a.eq(db.stats().indexEntries, 0, "no index entry survives");
    a.deep(db.listCollections(), ["c"], "the collection stays");
    a.eq(c.listIndexes().length, 2, "and so do its indexes");
    c.insertOne({ _id: 1, a: 4 });
    a.eq(J(c.find({ a: 4 })), J([{ _id: 1, a: 4 }]), "the kept index serves new documents");
});

t.test("the filter is required; a missing collection answers 0 and is not created", (a) => {
    const db = new Database("dm3"), c = db.collection("c");
    c.insertMany([{ _id: 1 }, { _id: 2 }]);
    a.throws(() => c.deleteMany(), TypeError);
    a.eq(c.estimatedDocumentCount(), 2);
    a.deep(db.collection("none").deleteMany({}), { acknowledged: true, deletedCount: 0 });
    a.deep(db.listCollections(), ["c"]);
});

await t.run();
