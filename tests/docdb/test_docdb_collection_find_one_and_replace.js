// covers: dyna:docdb.Collection.findOneAndReplace
// findOneAndReplace replaces the first match in sort order and returns it before (default) or after, projected, or
// null. Oracle: tests/lib/docdb_model.js replays 250 seeded calls (filters, model-computed sorts, returnDocument,
// projection) against an indexed and an unindexed collection; the returned document and the whole collection (JSON
// text: the replacement's field order after _id) must equal the model after each. Also pinned: an operator document
// is BadValue 2, a different _id is ImmutableField 66 with the document kept, upsert returns null before and the new
// document after, and a call matching nothing on a missing collection returns null without creating it.
import { suite } from "../lib/kit.js";
import { Database, DocDBError } from "dyna:docdb";
import { rng, genDocs, filters, SORTS, sortModel, project, byId, clone, J } from "../lib/docdb_model.js";

const t = suite("docdb.Collection.findOneAndReplace");

t.test("250 seeded calls against the JS model, plain and indexed", (a) => {
    const r = rng(0xf0a2), db = new Database("for"), plain = db.collection("plain"), ix = db.collection("ix");
    ix.createIndex({ a: 1 }); ix.createIndex({ n: -1 });
    const model = genDocs(r, 200);
    plain.insertMany(model.map(clone)); ix.insertMany(model.map(clone));
    for (let round = 0; round < 250; round++) {
        const fs = filters(r), [f, pred] = fs[Math.floor(r() * fs.length)];
        const repl = { r: round, a: Math.floor(r() * 10), n: Math.floor(r() * 400) / 4 };
        const [sort, spec] = SORTS[Math.floor(r() * SORTS.length)];
        const after = r() < 0.5, proj = r() < 0.3 ? { a: 1 } : undefined;
        const opts = { sort, returnDocument: after ? "after" : "before" };
        if (proj) opts.projection = proj;
        const i = model.indexOf(sortModel(model, spec).find(pred));
        let want = null;
        if (i >= 0) {
            const before = model[i];
            model[i] = { _id: before._id, ...repl };
            want = project(after ? model[i] : before, proj);
        }
        for (const c of [plain, ix]) {
            if (!a.eq(J(c.findOneAndReplace(f, repl, opts)), J(want), c.name + " returned " + J([f, opts]) + " round " + round)) return;
            if (!a.eq(J(c.find({}, { sort: { _id: 1 } })), J(byId(model)), c.name + " collection round " + round)) return;
        }
    }
});

t.test("operators, _id changes, upsert, missing collection", (a) => {
    const db = new Database("for2"), c = db.collection("c");
    a.eq(db.collection("none").findOneAndReplace({ a: 1 }, { b: 1 }), null);
    a.deep(db.listCollections(), [], "no match, no upsert: nothing created");
    c.insertMany([{ _id: 1, a: 1 }, { _id: 2, a: 2 }]);
    const e = a.throws(() => c.findOneAndReplace({ _id: 1 }, { $set: { a: 9 } }), DocDBError);
    a.eq(e && e.code, 2, "operators are BadValue");
    const e2 = a.throws(() => c.findOneAndReplace({ _id: 1 }, { _id: 3, a: 9 }), DocDBError);
    a.eq(e2 && e2.code, 66, "a different _id is ImmutableField");
    a.eq(J(c.find()), J([{ _id: 1, a: 1 }, { _id: 2, a: 2 }]), "refused calls changed nothing");
    a.eq(c.findOneAndReplace({ _id: 7 }, { q: 1 }, { upsert: true }), null, "upsert returns null before");
    a.eq(J(c.findOneAndReplace({ _id: 8 }, { q: 2 }, { upsert: true, returnDocument: "after" })), J({ _id: 8, q: 2 }));
    a.eq(J(c.findOneAndReplace({ _id: 2 }, { q: 3, a: 4 }, { returnDocument: "after" })), J({ _id: 2, q: 3, a: 4 }));
    a.eq(c.estimatedDocumentCount(), 4);
});

await t.run();
