// covers: dyna:docdb.Collection.findOneAndDelete
// findOneAndDelete removes the first match in sort order (slot order without a sort) and returns it, projected, or
// null. Oracle: tests/lib/docdb_model.js replays 300 seeded calls (filters, model-computed sorts, projection) against
// an indexed and an unindexed collection; the returned document and the remaining collection must equal the model
// after each. Also pinned: skip in the options picks the next match in order, and a missing collection answers null
// without being created.
import { suite } from "../lib/kit.js";
import { Database } from "dyna:docdb";
import { rng, genDocs, filters, SORTS, sortModel, project, byId, clone, J } from "../lib/docdb_model.js";

const t = suite("docdb.Collection.findOneAndDelete");

t.test("300 seeded calls against the JS model, plain and indexed", (a) => {
    const r = rng(0xf0a3), db = new Database("fod"), plain = db.collection("plain"), ix = db.collection("ix");
    ix.createIndex({ a: 1 }); ix.createIndex({ n: 1 }); ix.createIndex({ tags: 1 });
    let model = genDocs(r, 400);
    plain.insertMany(model.map(clone)); ix.insertMany(model.map(clone));
    for (let round = 0; round < 300; round++) {
        const fs = filters(r), [f, pred] = fs[Math.floor(r() * fs.length)];
        const sorted = r() < 0.8, [sort, spec] = SORTS[Math.floor(r() * SORTS.length)];
        const proj = r() < 0.3 ? { a: 1 } : undefined, opts = {};
        if (sorted) opts.sort = sort;
        if (proj) opts.projection = proj;
        const hit = (sorted ? sortModel(model, spec) : model).find(pred);
        const want = hit ? project(hit, proj) : null;
        for (const c of sorted ? [plain, ix] : [plain]) {
            if (!a.eq(J(c.findOneAndDelete(f, opts)), J(want), c.name + " returned " + J([f, opts]) + " round " + round)) return;
        }
        if (hit) {
            model = model.filter((d) => d !== hit);
            if (!sorted) ix.deleteOne({ _id: hit._id });
        }
        for (const c of [plain, ix]) if (!a.eq(J(c.find({}, { sort: { _id: 1 } })), J(byId(model)), c.name + " collection round " + round)) return;
    }
});

t.test("skip picks a later match; a missing collection answers null and is not created", (a) => {
    const db = new Database("fod2"), c = db.collection("c");
    c.insertMany([{ _id: 1, a: 1 }, { _id: 2, a: 1 }, { _id: 3, a: 1 }]);
    a.eq(J(c.findOneAndDelete({ a: 1 }, { sort: { _id: -1 }, skip: 1 })), J({ _id: 2, a: 1 }));
    a.eq(J(c.find()), J([{ _id: 1, a: 1 }, { _id: 3, a: 1 }]));
    a.eq(c.findOneAndDelete({ a: 2 }), null);
    a.eq(db.collection("none").findOneAndDelete({}), null);
    a.deep(db.listCollections(), ["c"]);
});

await t.run();
