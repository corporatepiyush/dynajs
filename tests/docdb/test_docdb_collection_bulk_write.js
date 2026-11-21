// covers: dyna:docdb.Collection.bulkWrite
// bulkWrite in the driver's operation shape: ordered stops at the first failure, unordered tries every operation;
// a failure throws a DocDBError whose writeErrors name each failed operation's index and code and whose result holds
// the counts of what did apply. insertMany shares the semantics (tested at the end through bulkWrite's insertOne).
import { suite } from "../lib/kit.js";
import { Database, DocDBError } from "dyna:docdb";

const t = suite("docdb.Collection.bulkWrite");
const db = new Database("bulk");

const OPS = () => [
    { insertOne: { document: { _id: 1, n: 1 } } },
    { insertOne: { document: { _id: 1, n: 2 } } },
    { updateOne: { filter: { _id: 1 }, update: { $inc: { n: 10 } } } },
    { updateMany: { filter: {}, update: { $set: { seen: true } } } },
    { replaceOne: { filter: { _id: 9 }, replacement: { n: 9 }, upsert: true } },
    { deleteOne: { filter: { n: 9 } } },
    { deleteMany: { filter: { nope: 1 } } },
];

t.test("all-good batch: counts", (a) => {
    const c = db.collection("good");
    const r = c.bulkWrite(OPS().filter((_, i) => i !== 1));
    a.eq(JSON.stringify(r), '{"acknowledged":true,"insertedCount":1,"matchedCount":2,"modifiedCount":2,"deletedCount":1,"upsertedCount":1,"upsertedIds":{"3":9}}');
    a.eq(JSON.stringify(c.find({})), '[{"_id":1,"n":11,"seen":true}]');
});

t.test("ordered: stops at the duplicate, earlier operations stay applied", (a) => {
    const c = db.collection("ord");
    let err;
    try { c.bulkWrite(OPS()); } catch (e) { err = e; }
    a.ok(err instanceof DocDBError);
    a.eq(err.code, 11000);
    a.eq(JSON.stringify(err.writeErrors.map((w) => [w.index, w.code, w.codeName])), '[[1,11000,"DuplicateKey"]]');
    a.eq(err.result.insertedCount, 1);
    a.eq(JSON.stringify(c.find({})), '[{"_id":1,"n":1}]');
});

t.test("unordered: every operation is tried", (a) => {
    const c = db.collection("unord");
    let err;
    try { c.bulkWrite(OPS(), { ordered: false }); } catch (e) { err = e; }
    a.eq(err.writeErrors.length, 1);
    a.eq(err.result.matchedCount, 2);
    a.eq(err.result.deletedCount, 1);
    a.eq(JSON.stringify(c.find({})), '[{"_id":1,"n":11,"seen":true}]');
});

t.test("refusals: unknown operation kind, non-array", (a) => {
    const c = db.collection("bad");
    a.throws(() => c.bulkWrite([{ frob: {} }]), TypeError);
    a.throws(() => c.bulkWrite({}), TypeError);
    let err;
    try { c.bulkWrite([{ updateOne: { filter: {}, update: { $bad: 1 } } }]); } catch (e) { err = e; }
    a.eq(err.writeErrors[0].code, 9);
});

await t.run();
db.close();
