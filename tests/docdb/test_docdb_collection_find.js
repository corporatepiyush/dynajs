// covers: dyna:docdb.Collection.find
// Query semantics against hand-derived answers, each from the MongoDB manual: comparison/BSON type order
// ("Comparison/Sort Order"), array traversal, null-or-missing, $type aliases, $size, $all, $mod, $elemMatch,
// $regex/$options, $not, $nor; then projection, sort (array fields sort by their min/max element), skip and limit.
// $in with a value larger than its 256-byte stack scratch once realloc'd that stack buffer and aborted the process.
// A projection of only { _id: 1 } is an inclusion projection (MongoDB returns just _id); it once returned whole documents.
// A dotted path is not followed into an array nested in an array, so such an element holds the field missing: {f: null}
// must match it as it matches {f: {$exists: false}} (fuzz find: [[{x:1}]] matched $exists:false but not null).
// Likewise a numeric component that names no element ("a.1" over [5]) is missing (fuzz laws: $exists:false not in null).
import { suite } from "../lib/kit.js";
import { Database, DocDBError, ObjectId } from "dyna:docdb";

const t = suite("docdb.Collection.find");
const db = new Database("find");
const c = db.collection("c");
c.insertMany([
    { _id: 1, v: 1 },
    { _id: 2, v: 2.5 },
    { _id: 3, v: 10n },
    { _id: 4, v: "10" },
    { _id: 5, v: null },
    { _id: 6 },
    { _id: 7, v: [1, 20] },
    { _id: 8, v: [] },
    { _id: 9, v: { x: 1 } },
    { _id: 10, v: true },
    { _id: 11, v: new Date(1000) },
    { _id: 12, v: [[5, 6]] },
    { _id: 13, v: [{ x: 1, y: 2 }, { x: 3, y: 4 }] },
    { _id: 14, v: "abc" },
    { _id: 15, v: "ABD" },
]);
const ids = (f, o) => c.find(f, o).map((d) => d._id).join();

t.test("comparisons stay inside a type bracket; numbers compare across int32/int64/double", (a) => {
    a.eq(ids({ v: { $gt: 2 } }), "2,3,7");
    a.eq(ids({ v: { $gte: 10 } }), "3,7");
    a.eq(ids({ v: 10 }), "3");
    a.eq(ids({ v: { $lt: "b" } }), "4,14,15");
    a.eq(ids({ v: { $gt: new Date(0) } }), "11");
    a.eq(ids({ v: { $lte: true } }), "10");
});

t.test("null matches null and missing; $exists; $ne", (a) => {
    a.eq(ids({ v: null }), "5,6");
    a.eq(ids({ v: { $exists: false } }), "6");
    a.eq(ids({ v: { $in: [null, "abc"] } }), "5,6,14");
    a.eq(ids({ v: { $ne: 1 } }), "2,3,4,5,6,8,9,10,11,12,13,14,15");
    a.eq(ids({ v: { $nin: [1, null] } }), "2,3,4,8,9,10,11,12,13,14,15");
});

t.test("arrays: element match, whole-array equality, nested arrays, dotted paths through arrays", (a) => {
    a.eq(ids({ v: 20 }), "7");
    a.eq(ids({ v: [1, 20] }), "7");
    a.eq(ids({ v: [5, 6] }), "12");
    a.eq(ids({ v: 5 }), "", "a nested array's elements are not traversed");
    a.eq(ids({ v: [] }), "8");
    a.eq(ids({ "v.x": 3 }), "13");
    a.eq(ids({ "v.1.y": 4 }), "13", "a numeric component indexes the array");
    a.eq(ids({ "v.x": { $gt: 0 } }), "9,13");
});

t.test("$type by alias and number, $size, $all, $mod", (a) => {
    a.eq(ids({ v: { $type: "string" } }), "4,14,15");
    a.eq(ids({ v: { $type: "number" } }), "1,2,3,7");
    a.eq(ids({ v: { $type: "array" } }), "7,8,12,13");
    a.eq(ids({ v: { $type: ["null", 8] } }), "5,10");
    a.eq(ids({ v: { $type: "long" } }), "3");
    a.eq(ids({ v: { $size: 2 } }), "7,13");
    a.eq(ids({ v: { $size: 0 } }), "8");
    a.eq(ids({ v: { $all: [20, 1] } }), "7");
    a.eq(ids({ v: { $mod: [5, 0] } }), "3,7");
    a.throws(() => c.find({ v: { $mod: [0, 1] } }), DocDBError);
    a.throws(() => c.find({ v: { $type: "nope" } }), DocDBError);
});

t.test("$elemMatch on documents and on values; $not; $nor; $regex with options", (a) => {
    a.eq(ids({ v: { $elemMatch: { x: 1, y: 2 } } }), "13");
    a.eq(ids({ v: { $elemMatch: { x: 1, y: 4 } } }), "", "both conditions on one element");
    a.eq(ids({ "v.x": 1, "v.y": 4 }), "13", "without $elemMatch they may hold on different elements");
    a.eq(ids({ v: { $elemMatch: { $gt: 5, $lt: 25 } } }), "7", "a nested array element is compared whole, not traversed");
    a.eq(ids({ v: { $not: { $gt: 1 } }, _id: { $lt: 7 } }), "1,4,5,6");
    a.eq(ids({ $nor: [{ v: { $type: "number" } }, { v: { $type: "string" } }], _id: { $lte: 10 } }), "5,6,8,9,10");
    a.eq(ids({ v: /^ab/i }), "14,15");
    a.eq(ids({ v: { $regex: "^AB", $options: "i" } }), "14,15");
    a.eq(ids({ v: { $regex: "c$" } }), "14");
    a.eq(ids({ v: { $in: [/^AB/, 1] } }), "1,7,15");
    a.throws(() => c.find({ v: { $options: "i" } }), DocDBError);
    a.throws(() => c.find({ $where: "1" }), DocDBError);
    a.throws(() => c.find({ v: { $gt: 1, x: 1 } }), DocDBError);
});

t.test("sort: BSON type order, arrays by min (asc) / max (desc), stable by insertion; skip and limit", (a) => {
    const asc = c.find({}, { sort: { v: 1 } }).map((d) => d._id).join();
    a.eq(asc, "8,5,6,1,7,2,3,4,15,14,9,13,12,10,11", "[] < null=missing < numbers < strings (bytewise: ABD < abc) < objects < arrays < bool < date");
    a.eq(c.find({ _id: { $in: [1, 2, 3, 7] } }, { sort: { v: -1 } }).map((d) => d._id).join(), "7,3,2,1");
    a.eq(c.find({}, { sort: { _id: -1 }, skip: 2, limit: 3 }).map((d) => d._id).join(), "13,12,11");
    a.eq(c.find({}, { skip: 14 }).length, 1);
    a.eq(c.find({}, { limit: 0 }).length, 15, "limit 0 is no limit");
    a.throws(() => c.find({}, { sort: { v: 2 } }), DocDBError);
    a.throws(() => c.find({}, { skip: -1 }), DocDBError);
});

t.test("projection: inclusion, exclusion, dotted, _id handling, mixing refused", (a) => {
    a.eq(JSON.stringify(c.findOne({ _id: 13 }, { projection: { "v.x": 1 } })), '{"_id":13,"v":[{"x":1},{"x":3}]}');
    a.eq(JSON.stringify(c.findOne({ _id: 13 }, { projection: { "v.y": 0, _id: 0 } })), '{"v":[{"x":1},{"x":3}]}');
    a.eq(JSON.stringify(c.findOne({ _id: 9 }, { projection: { _id: 0 } })), '{"v":{"x":1}}');
    a.eq(JSON.stringify(c.findOne({ _id: 9 }, { projection: { nope: 1 } })), '{"_id":9}');
    a.eq(JSON.stringify(c.findOne({ _id: 9 }, { projection: { _id: 1 } })), '{"_id":9}');
    a.eq(JSON.stringify(c.findOne({ _id: 9 }, { projection: { _id: true } })), '{"_id":9}');
    a.eq(JSON.stringify(c.findOne({ _id: 9 }, { projection: {} })), '{"_id":9,"v":{"x":1}}');
    a.throws(() => c.find({}, { projection: { a: 1, b: 0 } }), DocDBError);
    a.throws(() => c.find({}, { projection: { a: 1, "a.b": 1 } }), DocDBError);
});

t.test("values round-trip through BSON: Date, BigInt beyond 2^53, ObjectId, binary, nested", (a) => {
    const d = db.collection("vals");
    const oid = new ObjectId();
    d.insertOne({ _id: oid, big: 2n ** 60n, small: 5n, at: new Date(123), bin: new Uint8Array([1, 2]), deep: { a: [{ b: null }] } });
    const got = d.findOne({ _id: oid });
    a.ok(got._id instanceof ObjectId && got._id.equals(oid));
    a.eq(got.big, 2n ** 60n);
    a.eq(got.small, 5, "an int64 within 2^53 reads back as a number");
    a.eq(got.at.getTime(), 123);
    a.eq(Array.from(got.bin).join(), "1,2");
    a.eq(JSON.stringify(got.deep), '{"a":[{"b":null}]}');
    a.eq(d.find({ _id: oid.toHexString() }).length, 0, "an ObjectId does not equal its hex string");
});

t.test("find on a collection that does not exist is empty; a bad filter throws", (a) => {
    a.eq(db.collection("nope").find({ a: 1 }).length, 0);
    a.eq(db.collection("nope").findOne({}), null);
    a.throws(() => c.find(5), TypeError);
    a.throws(() => c.find([]), TypeError);
    a.eq(db.listCollections().includes("nope"), false);
});

t.test("$in and $nin over values of any size: long strings, big arrays and documents", (a) => {
    const d = db.collection("big-in");
    const s = "a".repeat(1000), arr = Array.from({ length: 100 }, (_, i) => i), doc = { k: "z".repeat(300) };
    d.insertMany([{ _id: 1, v: s }, { _id: 2, v: arr }, { _id: 3, v: doc }, { _id: 4, v: "short" }]);
    a.eq(JSON.stringify(d.find({ v: { $in: [s, "b"] } }).map((x) => x._id)), "[1]");
    a.eq(JSON.stringify(d.find({ v: { $in: [arr] } }).map((x) => x._id)), "[2]");
    a.eq(JSON.stringify(d.find({ v: { $in: [doc, 700] } }).map((x) => x._id)), "[3]");
    a.eq(JSON.stringify(d.find({ v: { $nin: [s, doc, "short"] } }).map((x) => x._id)), "[2]");
    a.eq(JSON.stringify(d.find({ v: { $in: [5, "short"] } }).map((x) => x._id)), "[2,4]");
});

t.test("an array nested in an array is a missing branch for a dotted path: null matches it like $exists false", (a) => {
    const d = db.collection("nested-null");
    d.insertMany([{ _id: 1, a: [[{ x: 1 }]] }, { _id: 2, a: [[1], { x: 2 }] }, { _id: 3, a: [[]] }, { _id: 4, a: [{ x: 3 }] }, { _id: 5, a: [{ x: 3 }, [{ x: 4 }]] }]);
    const q = (f) => JSON.stringify(d.find(f).map((x) => x._id));
    a.eq(q({ "a.x": { $exists: false } }), "[1,3]");
    a.eq(q({ "a.x": null }), "[1,2,3,5]");
    a.eq(q({ "a.x": { $in: [null, 3] } }), "[1,2,3,4,5]");
    a.eq(q({ "a.x": { $ne: null } }), "[4]");
    a.eq(q({ "a.x": 4 }), "[]");
    d.createIndex({ "a.x": 1 });
    a.eq(q({ "a.x": null }), "[1,2,3,5]");
    a.eq(q({ "a.x": { $ne: null } }), "[4]");
});

t.test("a numeric path component naming no element is missing: null matches it like $exists false", (a) => {
    const d = db.collection("numeric-null");
    d.insertMany([{ _id: 1, a: [5] }, { _id: 2, a: [5, 6] }, { _id: 3, a: [] }, { _id: 4, a: [{ x: 1 }] }, { _id: 5, a: [{ 1: 7 }] }, { _id: 6, a: 5 }, { _id: 7, a: [5, null] }]);
    const q = (f) => JSON.stringify(d.find(f).map((x) => x._id));
    a.eq(q({ "a.1": { $exists: false } }), "[1,3,4,6]");
    a.eq(q({ "a.1": null }), "[1,3,4,6,7]");
    a.eq(q({ "a.1": { $ne: null } }), "[2,5]");
    d.createIndex({ "a.1": 1 });
    a.eq(q({ "a.1": null }), "[1,3,4,6,7]");
    a.eq(q({ "a.1": { $in: [null, 6] } }), "[1,2,3,4,6,7]");
});

await t.run();
db.close();
