// covers: dyna:docdb.Collection.insertMany
// Split from test_docdb_collection_bulk_write.js (its insertMany case; the prelude moved verbatim).
// bulkWrite in the driver's operation shape: ordered stops at the first failure, unordered tries every operation;
// a failure throws a DocDBError whose writeErrors name each failed operation's index and code and whose result holds
// the counts of what did apply. insertMany shares the semantics (tested at the end through bulkWrite's insertOne).
import { suite } from "../lib/kit.js";
import { Database, DocDBError } from "dyna:docdb";

const t = suite("docdb.Collection.insertMany");
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

t.test("insertMany: insertedIds by position, ordered/unordered failure", (a) => {
    const c = db.collection("im");
    const r = c.insertMany([{ _id: "a" }, { _id: "b" }]);
    a.eq(JSON.stringify(r), '{"acknowledged":true,"insertedCount":2,"insertedIds":{"0":"a","1":"b"}}');
    let err;
    try { c.insertMany([{ _id: "c" }, { _id: "a" }, { _id: "d" }], { ordered: false }); } catch (e) { err = e; }
    a.eq(err.result.insertedCount, 2);
    a.eq(err.writeErrors[0].index, 1);
    a.eq(c.countDocuments(), 4);
});

await t.run();
db.close();
