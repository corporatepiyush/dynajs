// covers: dyna:docdb.Collection.updateOne
// Update operators against hand-derived results; each case cites the MongoDB manual rule it encodes
// (docs.mongodb.com/manual/reference/operator/update/<op>). updateOne applies the operators to the first match.
import { suite } from "../lib/kit.js";
import { Database, DocDBError } from "dyna:docdb";

const t = suite("docdb.Collection.updateOne");
const db = new Database("upd");
let n = 0;
function run(doc, update, filter, opts) {
    const c = db.collection("u" + ++n);
    c.insertOne({ _id: 1, ...doc });
    const r = c.updateOne(filter ?? { _id: 1 }, update, opts);
    return { r, doc: c.findOne({ _id: 1 }), c };
}
const J = (x) => JSON.stringify(x);
const codeOf = (fn) => { try { fn(); return "no throw"; } catch (e) { return e instanceof DocDBError ? e.code : String(e); } };

t.test("$set creates embedded documents along a dotted path; fields are added in lexicographic order", (a) => {
    a.eq(J(run({ a: 1 }, { $set: { "z.y": 1, b: 2, "m.n.o": 3 } }).doc), '{"_id":1,"a":1,"b":2,"m":{"n":{"o":3}},"z":{"y":1}}');
    a.eq(J(run({ arr: [1, 2] }, { $set: { "arr.4": 9 } }).doc), '{"_id":1,"arr":[1,2,null,null,9]}', "pads with null");
});

t.test("$set through a scalar is PathNotViable (28); _id is immutable (66); overlapping paths conflict (40)", (a) => {
    a.throws(() => run({ a: 5 }, { $set: { "a.b": 1 } }), DocDBError);
    a.eq(codeOf(() => run({ a: 5 }, { $set: { "a.b": 1 } })), 28);
    a.eq(codeOf(() => run({}, { $set: { _id: 2 } })), 66);
    a.eq(codeOf(() => run({}, { $set: { a: 1 }, $inc: { "a.b": 1 } })), 40);
    a.eq(codeOf(() => run({}, { $bogus: { a: 1 } })), 9);
    a.eq(codeOf(() => run({}, { $set: { "a.$[x]": 1 } })), 2);
});

t.test("$unset removes fields; inside an array it nulls the element", (a) => {
    a.eq(J(run({ a: 1, b: { c: 1, d: 2 }, e: [1, 2, 3] }, { $unset: { a: "", "b.c": 1, "e.1": 1, nope: 1 } }).doc), '{"_id":1,"b":{"d":2},"e":[1,null,3]}');
});

t.test("$inc/$mul: missing field starts at the operand (inc) or 0 (mul); int32 overflow widens to int64", (a) => {
    a.eq(J(run({ a: 1 }, { $inc: { a: 2, b: 5 }, $mul: { c: 3 } }).doc), '{"_id":1,"a":3,"b":5,"c":0}');
    a.eq(run({ a: 2147483647 }, { $inc: { a: 1 } }).doc.a, 2147483648);
    a.eq(run({ a: 2.5 }, { $mul: { a: 2 } }).doc.a, 5);
    a.eq(codeOf(() => run({ a: "x" }, { $inc: { a: 1 } })), 14);
    a.eq(codeOf(() => run({ a: 1 }, { $inc: { a: "1" } })), 14);
});

t.test("$min/$max compare in BSON order and set a missing field", (a) => {
    a.eq(J(run({ lo: 5, hi: 5 }, { $min: { lo: 3, m: 1 }, $max: { hi: 3 } }).doc), '{"_id":1,"lo":3,"hi":5,"m":1}');
    a.eq(run({ v: 10 }, { $max: { v: "a" } }).doc.v, "a", "a string sorts above every number");
});

t.test("$rename moves a value, including into a new embedded path; renames apply in destination-path order", (a) => {
    a.eq(J(run({ a: 1, b: { c: 2 } }, { $rename: { a: "x.y", "b.c": "d" } }).doc), '{"_id":1,"b":{},"d":2,"x":{"y":1}}');
});

t.test("$push with $each/$position/$sort/$slice is applied in that order (manual: $push modifiers)", (a) => {
    a.eq(J(run({ s: [5, 1] }, { $push: { s: 3 } }).doc.s), "[5,1,3]");
    a.eq(J(run({ s: [5, 1] }, { $push: { s: { $each: [7, 2], $position: 1 } } }).doc.s), "[5,7,2,1]");
    a.eq(J(run({ s: [5, 1] }, { $push: { s: { $each: [7, 2], $sort: 1, $slice: 3 } } }).doc.s), "[1,2,5]");
    a.eq(J(run({ s: [5, 1] }, { $push: { s: { $each: [7, 2], $sort: -1, $slice: -2 } } }).doc.s), "[2,1]");
    a.eq(J(run({ s: [{ k: 2 }, { k: 9 }] }, { $push: { s: { $each: [{ k: 4 }], $sort: { k: -1 } } } }).doc.s), '[{"k":9},{"k":4},{"k":2}]');
    a.eq(J(run({}, { $push: { s: 1 } }).doc.s), "[1]");
    a.eq(codeOf(() => run({ s: 1 }, { $push: { s: 1 } })), 2);
});

t.test("$addToSet adds only absent values (BSON equality: 1 equals 1.0); $pull/$pullAll/$pop remove", (a) => {
    a.eq(J(run({ s: [1, "a"] }, { $addToSet: { s: { $each: [1.0, "a", "b", { x: 1 }] } } }).doc.s), '[1,"a","b",{"x":1}]');
    a.eq(J(run({ s: [1, 5, 9, 5] }, { $pull: { s: 5 } }).doc.s), "[1,9]");
    a.eq(J(run({ s: [1, 5, 9] }, { $pull: { s: { $gte: 5 } } }).doc.s), "[1]");
    a.eq(J(run({ s: [{ a: 1, b: 2 }, { a: 2 }] }, { $pull: { s: { a: 1 } } }).doc.s), '[{"a":2}]', "a document condition is a query on each element");
    a.eq(J(run({ s: [1, 2, 3, 2] }, { $pullAll: { s: [2, 3] } }).doc.s), "[1]");
    a.eq(J(run({ s: [1, 2, 3] }, { $pop: { s: 1 } }).doc.s), "[1,2]");
    a.eq(J(run({ s: [1, 2, 3] }, { $pop: { s: -1 } }).doc.s), "[2,3]");
});

t.test("positional $ is the first array position the filter matched; $[] is every element", (a) => {
    a.eq(J(run({ g: [70, 85, 90] }, { $set: { "g.$": 86 } }, { g: { $gte: 80 } }).doc.g), "[70,86,90]");
    a.eq(J(run({ g: [{ s: 1 }, { s: 5 }] }, { $inc: { "g.$.s": 10 } }, { "g.s": 5 }).doc.g), '[{"s":1},{"s":15}]');
    a.eq(J(run({ g: [1, 2, 3] }, { $mul: { "g.$[]": 2 } }).doc.g), "[2,4,6]");
    a.eq(codeOf(() => run({ g: [1] }, { $set: { "g.$": 1 } })), 2);
});

t.test("$currentDate sets a Date; $setOnInsert applies only on an upsert insert", (a) => {
    const before = Date.now();
    const d = run({}, { $currentDate: { at: true } }).doc.at;
    a.ok(d instanceof Date && d.getTime() >= before - 5 && d.getTime() <= Date.now() + 5);
    a.eq(J(run({}, { $setOnInsert: { x: 1 }, $set: { y: 1 } }).doc), '{"_id":1,"y":1}');
    const c = db.collection("ups");
    const r = c.updateOne({ k: "a", n: { $gt: 3 }, "deep.v": 7 }, { $setOnInsert: { x: 1 }, $inc: { cnt: 1 } }, { upsert: true });
    a.eq(r.upsertedCount, 1);
    const doc = c.findOne({ k: "a" });
    a.eq(J({ ...doc, _id: 0 }), '{"_id":0,"k":"a","deep":{"v":7},"cnt":1,"x":1}', "upsert seeds the filter's equality fields in filter order, then applies the operators; non-equality conditions are dropped");
    a.eq(c.updateOne({ k: "a" }, { $setOnInsert: { x: 2 } }, { upsert: true }).modifiedCount, 0);
});

t.test("result counts: a no-op update matches but does not modify", (a) => {
    const { r, c } = run({ a: 1 }, { $set: { a: 1 } });
    a.eq(J(r), '{"acknowledged":true,"matchedCount":1,"modifiedCount":0,"upsertedCount":0,"upsertedId":null}');
    a.eq(c.updateOne({ _id: 2 }, { $set: { a: 1 } }).matchedCount, 0);
    a.throws(() => c.updateOne({}, { a: 1 }), DocDBError, "a replacement document is refused by updateOne");
});

await t.run();
db.close();
