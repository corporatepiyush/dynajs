// covers: dyna:docdb.Collection.replaceOne
// replaceOne swaps the first match (slot order without a sort) for the replacement, keeping _id first; the oracle is
// tests/lib/docdb_model.js, which replays every call on plain JS objects and compares the whole collection (JSON text,
// so field order counts) after each one. Also pinned: a different _id is ImmutableField 66 and leaves the document,
// an operator document is BadValue 2, an identical replacement matches without modifying, a unique-index violation is
// 11000 with the document kept and the index still agreeing with a scan, upsert inserts the filter's _id with the
// replacement (no other filter field: MongoDB's replace-upsert). A replaceOne that matches nothing on a collection that
// does not exist used to create it (listCollections grew); it now leaves the database as it was, and an upsert creates
// it.
import { suite } from "../lib/kit.js";
import { Database, DocDBError } from "dyna:docdb";
import { rng, genDocs, filters, byId, clone, J } from "../lib/docdb_model.js";

const t = suite("docdb.Collection.replaceOne");

t.test("300 seeded replacements against the JS model", (a) => {
    const r = rng(0x5eed1), db = new Database("rep"), c = db.collection("m");
    let model = genDocs(r, 150);
    c.insertMany(model.map(clone));
    for (let round = 0; round < 300; round++) {
        const fs = filters(r), [f, pred] = fs[Math.floor(r() * fs.length)];
        const repl = { a: round % 10, n: round, r: round };
        if (r() < 0.3) repl.b = "s" + (round % 5);
        const res = c.replaceOne(f, repl);
        const i = model.findIndex(pred);
        if (i >= 0) model[i] = { _id: model[i]._id, ...repl };
        if (!a.eq(J([res.matchedCount, res.modifiedCount, res.upsertedCount, res.upsertedId]), J([i >= 0 ? 1 : 0, i >= 0 ? 1 : 0, 0, null]), "result " + J(f))) return;
        if (!a.eq(J(c.find({}, { sort: { _id: 1 } })), J(byId(model)), "collection after " + J(f) + " round " + round)) return;
    }
});

t.test("_id stays first and immutable; operators refused; identical replacement does not modify", (a) => {
    const c = new Database("rep2").collection("c");
    c.insertMany([{ _id: 1, a: 1, b: 2 }, { _id: 2, a: 2 }]);
    a.deep(c.replaceOne({ _id: 1 }, { z: 1, y: 2 }), { acknowledged: true, matchedCount: 1, modifiedCount: 1, upsertedCount: 0, upsertedId: null });
    a.eq(J(c.findOne({ _id: 1 })), J({ _id: 1, z: 1, y: 2 }), "_id first, replacement order kept");
    a.eq(c.replaceOne({ _id: 1 }, { y: 3, _id: 1 }).modifiedCount, 1, "the same _id in the replacement is allowed");
    a.eq(J(c.findOne({ _id: 1 })), J({ _id: 1, y: 3 }), "_id first even when the replacement names it later");
    const e = a.throws(() => c.replaceOne({ _id: 2 }, { _id: 7, q: 1 }), DocDBError);
    a.eq(e && e.code, 66, "a different _id is ImmutableField");
    a.eq(J(c.findOne({ _id: 2 })), J({ _id: 2, a: 2 }), "the refused replacement left the document");
    a.eq(c.findOne({ _id: 7 }), null, "and inserted nothing");
    const e2 = a.throws(() => c.replaceOne({ _id: 2 }, { $set: { q: 1 } }), DocDBError);
    a.eq(e2 && e2.code, 2, "an operator document is BadValue");
    a.deep(c.replaceOne({ _id: 2 }, { a: 2 }), { acknowledged: true, matchedCount: 1, modifiedCount: 0, upsertedCount: 0, upsertedId: null }, "an identical replacement");
    a.throws(() => c.replaceOne({ _id: 2 }), TypeError, "the replacement is required");
});

t.test("unique index violation keeps the document; indexes agree with a scan after replacements", (a) => {
    const r = rng(77), db = new Database("rep3"), ix = db.collection("ix"), plain = db.collection("plain");
    ix.createIndex({ b: 1 }, { unique: true, sparse: true });
    ix.createIndex({ a: 1 });
    const docs = Array.from({ length: 60 }, (_, i) => ({ _id: i, a: i % 7, b: "u" + i }));
    ix.insertMany(docs.map(clone)); plain.insertMany(docs.map(clone));
    const e = a.throws(() => ix.replaceOne({ _id: 3 }, { a: 0, b: "u4" }), DocDBError);
    a.eq(e && e.code, 11000, "duplicate key");
    a.eq(J(ix.findOne({ _id: 3 })), J({ _id: 3, a: 3, b: "u3" }), "the document is unchanged");
    for (let i = 0; i < 80; i++) {
        const id = Math.floor(r() * 60), repl = { a: Math.floor(r() * 7), b: "v" + i };
        for (const c of [ix, plain]) c.replaceOne({ _id: id }, repl);
    }
    for (let k = 0; k < 7; k++) a.eq(J(ix.find({ a: k }, { sort: { _id: 1 } })), J(plain.find({ a: k }, { sort: { _id: 1 } })), "a = " + k);
    a.eq(J(ix.find({ b: { $gte: "v" } }, { sort: { _id: 1 } })), J(plain.find({ b: { $gte: "v" } }, { sort: { _id: 1 } })), "b range");
    a.eq(ix.find({ b: "u3" }).length, plain.find({ b: "u3" }).length, "a replaced key is gone from the index");
});

t.test("upsert inserts the filter _id with the replacement; no match without upsert does not create the collection", (a) => {
    const db = new Database("rep4");
    const res = db.collection("none").replaceOne({ a: 1 }, { q: 1 });
    a.deep(res, { acknowledged: true, matchedCount: 0, modifiedCount: 0, upsertedCount: 0, upsertedId: null });
    a.deep(db.listCollections(), [], "a replaceOne that wrote nothing created no collection");
    const c = db.collection("u");
    const up = c.replaceOne({ _id: 9, k: 3 }, { q: 1 }, { upsert: true });
    a.eq(J([up.matchedCount, up.modifiedCount, up.upsertedCount, up.upsertedId]), J([0, 0, 1, 9]));
    a.eq(J(c.find()), J([{ _id: 9, q: 1 }]), "only _id comes from the filter");
    const up2 = c.replaceOne({ k: 4 }, { q: 2 }, { upsert: true });
    a.eq(typeof up2.upsertedId, "object", "a generated ObjectId");
    a.eq(c.countDocuments({ q: 2 }), 1);
    a.deep(db.listCollections(), ["u"]);
    a.eq(c.replaceOne({ _id: 9 }, { q: 5 }, { upsert: true }).upsertedCount, 0, "a match is replaced, not upserted");
});

await t.run();
