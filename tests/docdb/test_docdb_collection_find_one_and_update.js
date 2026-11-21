// covers: dyna:docdb.Collection.findOneAndUpdate
// findOneAndUpdate updates the first match in sort order (slot order without a sort) and returns it before (default)
// or after the update, projected, or null. Oracle: tests/lib/docdb_model.js replays 300 seeded calls mixing filters,
// sorts (numeric keys with an _id tie-break, computed by the model), $set/$inc/$unset updates, returnDocument and
// projection, against an indexed and an unindexed collection; the returned document and the whole collection must
// equal the model after each call. Also pinned: upsert returns null before and the inserted document after, a
// replacement document is accepted (MongoDB's driver refuses it; here it replaces), a bad returnDocument is BadValue 2,
// and a call matching nothing on a missing collection returns null without creating it.
import { suite } from "../lib/kit.js";
import { Database, DocDBError } from "dyna:docdb";
import { rng, genDocs, filters, updates, SORTS, sortModel, project, byId, clone, J } from "../lib/docdb_model.js";

const t = suite("docdb.Collection.findOneAndUpdate");

t.test("300 seeded calls against the JS model, plain and indexed", (a) => {
    const r = rng(0xf0a1), db = new Database("fou"), plain = db.collection("plain"), ix = db.collection("ix");
    ix.createIndex({ a: 1 }); ix.createIndex({ n: 1 }); ix.createIndex({ tags: 1 });
    const model = genDocs(r, 250);
    plain.insertMany(model.map(clone)); ix.insertMany(model.map(clone));
    for (let round = 0; round < 300; round++) {
        const fs = filters(r), [f, pred] = fs[Math.floor(r() * fs.length)];
        const us = updates(r), [u, apply] = us[Math.floor(r() * us.length)];
        const sorted = r() < 0.8, [sort, spec] = SORTS[Math.floor(r() * SORTS.length)];
        const after = r() < 0.5, proj = r() < 0.3 ? { n: 1 } : undefined;
        const opts = { returnDocument: after ? "after" : "before" };
        if (sorted) opts.sort = sort;
        if (proj) opts.projection = proj;
        const order = sorted ? sortModel(model, spec) : model;
        const hit = order.find(pred);
        let want = null;
        if (hit) {
            const before = clone(hit);
            apply(hit);
            want = project(after ? hit : before, proj);
        }
        for (const c of sorted ? [plain, ix] : [plain]) {
            const got = c.findOneAndUpdate(f, u, opts);
            if (!a.eq(J(got), J(want), c.name + " returned " + J([f, u, opts]) + " round " + round)) return;
        }
        if (!sorted && hit) ix.updateOne({ _id: hit._id }, u);
        for (const c of [plain, ix]) if (!a.eq(J(c.find({}, { sort: { _id: 1 } })), J(byId(model)), c.name + " collection round " + round)) return;
    }
});

t.test("upsert, replacement documents, bad options, missing collection", (a) => {
    const db = new Database("fou2"), c = db.collection("c");
    a.eq(db.collection("none").findOneAndUpdate({ a: 1 }, { $set: { b: 1 } }), null);
    a.deep(db.listCollections(), [], "no match, no upsert: nothing created");
    a.eq(c.findOneAndUpdate({ _id: 5 }, { $set: { b: 1 } }, { upsert: true }), null, "upsert returns null before");
    a.eq(J(c.findOneAndUpdate({ _id: 6 }, { $set: { b: 2 } }, { upsert: true, returnDocument: "after" })), J({ _id: 6, b: 2 }));
    a.eq(J(c.find()), J([{ _id: 5, b: 1 }, { _id: 6, b: 2 }]));
    a.eq(J(c.findOneAndUpdate({ _id: 5 }, { z: 1 }, { returnDocument: "after" })), J({ _id: 5, z: 1 }), "a replacement document replaces");
    const e = a.throws(() => c.findOneAndUpdate({}, { $set: { q: 1 } }, { returnDocument: "middle" }), DocDBError);
    a.eq(e && e.code, 2);
    a.eq(c.countDocuments({ q: 1 }), 0, "the refused call changed nothing");
    a.throws(() => c.findOneAndUpdate({}), TypeError);
});

await t.run();
