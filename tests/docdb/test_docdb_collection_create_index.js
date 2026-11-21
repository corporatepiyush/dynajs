// covers: dyna:docdb.Collection.createIndex
// Index creation and maintenance: names, idempotence and conflicts (85/86), unique (E11000 on insert, update and
// creation over duplicates, missing fields count as null), sparse (absent documents not indexed, so unique ignores
// them), partial (only matching documents), multikey and parallel arrays (171), compound + descending order, and
// that every write keeps every index consistent (a query through the index equals a scan).
import { suite } from "../lib/kit.js";
import { Database, DocDBError } from "dyna:docdb";

const t = suite("docdb.Collection.createIndex");
const db = new Database("ix");
const code = (fn) => { try { fn(); return 0; } catch (e) { return e instanceof DocDBError ? e.code : -1; } };

t.test("default names, idempotent re-creation, name/spec conflicts, bad key patterns", (a) => {
    const c = db.collection("names");
    a.eq(c.createIndex({ a: 1, "b.c": -1 }), "a_1_b.c_-1");
    a.eq(c.createIndex({ a: 1, "b.c": -1 }), "a_1_b.c_-1");
    a.eq(code(() => c.createIndex({ a: 1, "b.c": -1 }, { unique: true })), 85);
    a.eq(code(() => c.createIndex({ z: 1 }, { name: "a_1_b.c_-1" })), 86);
    a.eq(code(() => c.createIndex({ a: 2 })), 2);
    a.eq(code(() => c.createIndex({})), 2);
    a.eq(code(() => c.createIndex({ _id: 1 }, { name: "_id_" })), 2);
    a.eq(c.listIndexes().map((i) => i.name).join(), "_id_,a_1_b.c_-1");
    c.dropIndex("a_1_b.c_-1");
    a.eq(code(() => c.dropIndex("a_1_b.c_-1")), 27);
    a.eq(code(() => c.dropIndex("_id_")), 72);
    c.createIndex({ q: 1 });
    c.dropIndex({ q: 1 });
    a.eq(c.listIndexes().length, 1);
});

t.test("unique: duplicate insert, duplicate via update, creation over duplicates, missing counts as null", (a) => {
    const c = db.collection("uniq");
    c.createIndex({ email: 1 }, { unique: true });
    c.insertOne({ _id: 1, email: "a@x" });
    let err;
    try { c.insertOne({ _id: 2, email: "a@x" }); } catch (e) { err = e; }
    a.eq(err.code, 11000);
    a.ok(/E11000 duplicate key error collection: ix\.uniq index: email_1 dup key: \{ email: "a@x" \}/.test(err.message), err.message);
    a.eq(c.countDocuments(), 1, "the refused insert left nothing behind");
    c.insertOne({ _id: 3, email: "b@x" });
    a.eq(code(() => c.updateOne({ _id: 3 }, { $set: { email: "a@x" } })), 11000);
    a.eq(c.findOne({ _id: 3 }).email, "b@x", "the refused update left the document as it was");
    c.insertOne({ _id: 4 });
    a.eq(code(() => c.insertOne({ _id: 5 })), 11000, "a second missing email is a second null");
    const d = db.collection("uniq2");
    d.insertMany([{ k: 1 }, { k: 1 }]);
    a.eq(code(() => d.createIndex({ k: 1 }, { unique: true })), 11000);
    a.eq(d.listIndexes().length, 1);
});

t.test("sparse and partial indexes", (a) => {
    const c = db.collection("sp");
    c.createIndex({ s: 1 }, { unique: true, sparse: true });
    c.insertMany([{ _id: 1 }, { _id: 2 }, { _id: 3, s: 1 }]);
    a.eq(code(() => c.insertOne({ _id: 4, s: 1 })), 11000);
    a.eq(c.find({ s: null }).length, 2, "a null query does not use the sparse index (it would miss the absent docs)");
    a.eq(c.explain({ s: 1 }).queryPlanner.winningPlan.inputStage.indexName, "s_1");
    const p = db.collection("part");
    p.createIndex({ u: 1 }, { unique: true, partialFilterExpression: { active: true } });
    p.insertMany([{ u: 1, active: false }, { u: 1, active: false }, { u: 1, active: true }]);
    a.eq(code(() => p.insertOne({ u: 1, active: true })), 11000);
    a.eq(code(() => p.createIndex({ v: 1 }, { partialFilterExpression: { n: /x/ } })), 2);
    a.eq(p.listIndexes()[1].partialFilterExpression.active, true);
});

t.test("multikey indexes and parallel arrays", (a) => {
    const c = db.collection("mk");
    c.createIndex({ tags: 1 });
    c.insertMany([{ _id: 1, tags: ["a", "b"] }, { _id: 2, tags: [] }, { _id: 3, tags: "a" }, { _id: 4 }]);
    a.eq(c.find({ tags: "a" }).map((d) => d._id).join(), "1,3");
    a.eq(c.find({ tags: [] }).map((d) => d._id).join(), "2");
    a.eq(c.explain({ tags: "a" }).queryPlanner.winningPlan.inputStage.isMultiKey, true);
    c.createIndex({ tags: 1, other: 1 });
    a.eq(code(() => c.insertOne({ tags: [1], other: [2] })), 171);
    a.eq(c.countDocuments(), 4);
});

t.test("compound + descending: equality prefix with a range on the next field, results equal a scan", (a) => {
    const c = db.collection("cmp");
    c.insertMany(Array.from({ length: 500 }, (_, i) => ({ _id: i, g: i % 5, n: (i * 37) % 101, s: "s" + (i % 13) })));
    c.createIndex({ g: 1, n: -1 });
    const f = { g: { $in: [1, 3] }, n: { $gte: 20, $lt: 60 } };
    const want = c.find({}).filter((d) => (d.g === 1 || d.g === 3) && d.n >= 20 && d.n < 60).map((d) => d._id).sort((x, y) => x - y).join();
    a.eq(c.find(f).map((d) => d._id).sort((x, y) => x - y).join(), want);
    const e = c.explain(f);
    a.eq(e.queryPlanner.winningPlan.inputStage.boundFields, 2);
    a.eq(e.executionStats.totalDocsExamined, e.executionStats.nReturned, "the bounds are exact on a non-multikey compound index");
    const s = c.find({ g: 2 }, { sort: { g: 1, n: -1 }, limit: 3 }).map((d) => d.n).join();
    a.eq(s, c.find({ g: 2 }).map((d) => d.n).sort((x, y) => y - x).slice(0, 3).join());
    a.eq(c.explain({}, { sort: { g: -1, n: 1 } }).queryPlanner.winningPlan.inputStage.direction, "backward");
});

t.test("every write path keeps every index equal to a scan", (a) => {
    const c = db.collection("churn");
    c.createIndex({ k: 1 });
    c.createIndex({ "arr.v": 1 });
    let seed = 7;
    const rnd = (n) => (seed = (seed * 1103515245 + 12345) % 2147483648) % n;
    for (let i = 0; i < 3000; i++) {
        const op = rnd(6), k = rnd(50);
        if (op < 2) c.insertOne({ k, arr: [{ v: rnd(30) }, { v: rnd(30) }] });
        else if (op === 2) c.updateMany({ k }, { $inc: { k: 1 } });
        else if (op === 3) c.deleteOne({ k });
        else if (op === 4) c.replaceOne({ k }, { k: rnd(50), arr: [{ v: rnd(30) }] });
        else c.updateOne({ "arr.v": rnd(30) }, { $push: { arr: { v: rnd(30) } } });
    }
    const all = c.find({});
    let bad = 0;
    for (let k = 0; k < 52; k++) if (c.find({ k }).length !== all.filter((d) => d.k === k).length) bad++;
    for (let v = 0; v < 30; v++) if (c.find({ "arr.v": v }).length !== all.filter((d) => d.arr.some((e) => e.v === v)).length) bad++;
    a.eq(bad, 0);
});

t.test("a path that reaches no value (through scalars, nested arrays or an empty array) is indexed with the null bracket", (a) => {
    const c = db.collection("holes");
    c.insertMany([{ _id: 1, a: [1, 2] }, { _id: 2, a: [] }, { _id: 3, a: { b: null } }, { _id: 4 }, { _id: 5, a: [{ b: 1 }] }, { _id: 6, a: [[1], [{ b: 2 }]] }]);
    const q = [{ "a.b": null }, { "a.b": { $lt: 5 } }, { "a.b": { $in: [null, 1] } }];
    const scan = q.map((f) => JSON.stringify(c.find(f).map((d) => d._id)));
    c.createIndex({ "a.b": 1 });
    a.eq(JSON.stringify(q.map((f) => JSON.stringify(c.find(f).map((d) => d._id)))), JSON.stringify(scan));
    a.eq(c.explain(q[0]).queryPlanner.winningPlan.stage, "FETCH");
    a.eq(scan[0], "[1,2,3,4,6]");
});

await t.run();
db.close();
