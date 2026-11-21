// covers: dyna:docdb.Collection.aggregate
// Pipeline stages against hand-derived answers, each from the MongoDB manual's stage page: $match, $project (inclusion,
// exclusion, computed, _id-only, mixed is 31254), $addFields/$set (dotted and nested merge, through arrays), $unset,
// $unwind (includeArrayIndex, preserveNullAndEmptyArrays: [] drops the field, null stays), $group (accumulator result
// types: int32 overflow widens to long, $avg of nothing is null, $first of missing is null, $push skips missing),
// $sort/$limit/$skip, $count (no document for zero), $lookup (null matches missing, array elements match singly),
// $facet, $bucket (default bucket last), $sortByCount, $replaceRoot/$replaceWith; the error codes; that errors reject
// rather than throw; and that the input is the collection as of the call.
import { suite } from "../lib/kit.js";
import { Database, DocDBError } from "dyna:docdb";

const t = suite("docdb.Collection.aggregate");
const db = new Database("agg");
const o = db.collection("orders");
o.insertMany([
    { _id: 1, cust: "a", amt: 10, items: ["x", "y"], d: new Date("2024-03-05T10:00:00Z") },
    { _id: 2, cust: "b", amt: 5, items: ["y"], d: new Date("2024-03-06T11:00:00Z") },
    { _id: 3, cust: "a", amt: 7.5, items: [], d: new Date("2024-04-01T00:00:00Z") },
    { _id: 4, cust: "c", amt: 2, items: null },
    { _id: 5, cust: "d", amt: 1 },
]);
db.collection("cust").insertMany([
    { _id: "a", name: "Ann", tags: ["x"] },
    { _id: "b", name: "Bob", tags: ["y", "x"] },
    { _id: "z", name: "Zed" },
    { _id: "n", name: "Nul", k: null },
]);
const J = (v) => JSON.stringify(v, (k, x) => (typeof x === "bigint" ? `${x}n` : x));
const agg = (p) => o.aggregate(p);
const codeOf = async (p) => {
    try {
        await o.aggregate(p);
        return "no throw";
    } catch (e) {
        return e instanceof DocDBError ? e.code : String(e);
    }
};

t.test("$match + $sort + $limit + inclusion $project with a computed field", async (a) => {
    const r = await agg([{ $match: { amt: { $gt: 3 } } }, { $sort: { amt: -1 } }, { $limit: 2 }, { $project: { cust: 1, dbl: { $multiply: ["$amt", 2] } } }]);
    a.eq(J(r), '[{"_id":1,"cust":"a","dbl":20},{"_id":3,"cust":"a","dbl":15}]');
});

t.test("$project: exclusion, _id only, _id: 0 with computed, nested spec merges into dotted paths, mixing is 31254", async (a) => {
    a.eq(J(await agg([{ $match: { _id: 1 } }, { $project: { items: 0, d: 0 } }])), '[{"_id":1,"cust":"a","amt":10}]');
    a.eq(J(await agg([{ $match: { _id: { $lt: 3 } } }, { $project: { _id: 1 } }])), '[{"_id":1},{"_id":2}]');
    a.eq(J(await agg([{ $match: { _id: 2 } }, { $project: { _id: 0, w: { c: "$cust", l: { $literal: "$x" } } } }])), '[{"w":{"c":"b","l":"$x"}}]');
    a.eq(await codeOf([{ $project: { a: 1, b: 0 } }]), 31254);
    a.eq(await codeOf([{ $project: {} }]), 51272);
});

t.test("$addFields/$set merge into embedded documents and apply through arrays; $unset removes", async (a) => {
    const r = await agg([{ $match: { _id: 1 } }, { $set: { "items.k": 1, "m.n": { $month: "$d" } } }, { $unset: ["d", "cust"] }]);
    a.eq(J(r), '[{"_id":1,"amt":10,"items":[{"k":1},{"k":1}],"m":{"n":3}}]');
    a.eq(await codeOf([{ $addFields: { a: 1, "a.b": 2 } }]), 31250);
    a.eq(await codeOf([{ $unset: [] }]), 31119);
});

t.test("$unwind: one document per element; includeArrayIndex; preserve keeps null/missing and drops []", async (a) => {
    a.eq(J(await agg([{ $unwind: "$items" }, { $project: { items: 1 } }])), '[{"_id":1,"items":"x"},{"_id":1,"items":"y"},{"_id":2,"items":"y"}]');
    const r = await agg([{ $unwind: { path: "$items", includeArrayIndex: "i", preserveNullAndEmptyArrays: true } }, { $project: { items: 1, i: 1 } }]);
    a.eq(
        J(r),
        '[{"_id":1,"items":"x","i":0},{"_id":1,"items":"y","i":1},{"_id":2,"items":"y","i":0},{"_id":3,"i":null},{"_id":4,"items":null,"i":null},{"_id":5,"i":null}]',
    );
    a.eq(await codeOf([{ $unwind: "items" }]), 28818);
});

t.test("$group accumulators and their result types", async (a) => {
    const r = await agg([
        { $group: { _id: "$cust", total: { $sum: "$amt" }, n: { $count: {} }, avg: { $avg: "$amt" }, first: { $first: "$items" }, ids: { $push: "$_id" } } },
        { $sort: { _id: 1 } },
    ]);
    a.eq(
        J(r),
        '[{"_id":"a","total":17.5,"n":2,"avg":8.75,"first":["x","y"],"ids":[1,3]},{"_id":"b","total":5,"n":1,"avg":5,"first":["y"],"ids":[2]},' +
            '{"_id":"c","total":2,"n":1,"avg":2,"first":null,"ids":[4]},{"_id":"d","total":1,"n":1,"avg":1,"first":null,"ids":[5]}]',
    );
    const t2 = db.collection("types");
    t2.insertMany([{ g: 1, v: 2147483647, s: "b" }, { g: 1, v: 1, s: "a" }, { g: 2, v: 5n }, { g: 2, x: 1 }]);
    const r2 = await t2.aggregate([
        { $group: { _id: "$g", s: { $sum: "$v" }, mn: { $min: "$s" }, mx: { $max: "$s" }, av: { $avg: "$nope" }, set: { $addToSet: "$s" } } },
        { $sort: { _id: 1 } },
    ]);
    a.eq(J(r2), '[{"_id":1,"s":2147483648,"mn":"a","mx":"b","av":null,"set":["b","a"]},{"_id":2,"s":5,"mn":null,"mx":null,"av":null,"set":[]}]');
    a.eq(J(await agg([{ $group: { _id: null, s: { $addToSet: "$cust" } } }])), '[{"_id":null,"s":["a","b","c","d"]}]', "$addToSet keeps one of each");
    a.eq(J(await agg([{ $group: { _id: "$cust", n: { $sum: 1 } } }, { $sort: { n: -1, _id: -1 } }, { $limit: 2 }])), '[{"_id":"a","n":2},{"_id":"d","n":1}]', "a $sort + $limit after $group keeps the top k");
    a.eq(await codeOf([{ $group: { n: { $sum: 1 } } }]), 15955);
    a.eq(await codeOf([{ $group: { _id: 1, n: { $bogus: 1 } } }]), 15952);
    a.eq(await codeOf([{ $group: { _id: 1, "a.b": { $sum: 1 } } }]), 40235);
    a.eq(await codeOf([{ $group: { _id: 1, n: { $sum: [1, 2] } } }]), 40237);
});

t.test("$group keys: compound _id, null for missing, documents equal by value", async (a) => {
    const r = await agg([{ $group: { _id: { y: { $year: "$d" }, m: { $month: "$d" } }, n: { $sum: 1 } } }, { $sort: { "_id.y": 1, "_id.m": 1 } }]);
    a.eq(J(r), '[{"_id":{"y":null,"m":null},"n":2},{"_id":{"y":2024,"m":3},"n":2},{"_id":{"y":2024,"m":4},"n":1}]');
    a.eq(J(await agg([{ $group: { _id: "$nope", n: { $sum: 1 } } }])), '[{"_id":null,"n":5}]');
});

t.test("$count, $skip, $limit and their errors", async (a) => {
    a.eq(J(await agg([{ $count: "n" }])), '[{"n":5}]');
    a.eq(J(await agg([{ $match: { cust: "zz" } }, { $count: "n" }])), "[]");
    a.eq(J(await agg([{ $sort: { _id: -1 } }, { $skip: 1 }, { $limit: 2 }, { $project: { _id: 1 } }])), '[{"_id":4},{"_id":3}]');
    a.eq(J(await agg([{ $limit: 3 }, { $skip: 2 }, { $project: { _id: 1 } }])), '[{"_id":3}]');
    a.eq(await codeOf([{ $limit: 0 }]), 15958);
    a.eq(await codeOf([{ $skip: -1 }]), 15956);
    a.eq(await codeOf([{ $count: "$n" }]), 40158);
    a.eq(await codeOf([{ $count: "a.b" }]), 40160);
});

t.test("$lookup: equality join, null/missing local matches null/missing foreign, array elements match singly", async (a) => {
    const r = await agg([{ $lookup: { from: "cust", localField: "cust", foreignField: "_id", as: "c" } }, { $project: { n: "$c.name" } }]);
    a.eq(J(r), '[{"_id":1,"n":["Ann"]},{"_id":2,"n":["Bob"]},{"_id":3,"n":["Ann"]},{"_id":4,"n":[]},{"_id":5,"n":[]}]');
    const r2 = await agg([{ $match: { _id: { $in: [1, 4] } } }, { $lookup: { from: "cust", localField: "nope", foreignField: "k", as: "c" } }, { $project: { n: "$c.name" } }]);
    a.eq(J(r2), '[{"_id":1,"n":["Ann","Bob","Zed","Nul"]},{"_id":4,"n":["Ann","Bob","Zed","Nul"]}]', "missing local = null matches missing and null k");
    const r3 = await agg([{ $match: { _id: 1 } }, { $lookup: { from: "cust", localField: "items", foreignField: "tags", as: "c" } }, { $project: { n: "$c.name" } }]);
    a.eq(J(r3), '[{"_id":1,"n":["Ann","Bob"]}]', "each element of items against each element of tags, each foreign doc once");
    a.eq(J(await agg([{ $match: { _id: 1 } }, { $lookup: { from: "absent", localField: "cust", foreignField: "_id", as: "c" } }, { $project: { c: 1 } }])), '[{"_id":1,"c":[]}]');
    a.eq(await codeOf([{ $lookup: { from: "cust", localField: "cust", foreignField: "_id" } }]), 40321);
    a.eq(await codeOf([{ $lookup: { from: "cust", pipeline: [], as: "c" } }]), 9);
});

t.test("$facet runs each sub-pipeline over the same input; nesting $facet is 40600", async (a) => {
    const r = await agg([{ $facet: { cnt: [{ $count: "n" }], top: [{ $sort: { amt: -1 } }, { $limit: 1 }, { $project: { _id: 1 } }], none: [{ $match: { amt: 99 } }] } }]);
    a.eq(J(r), '[{"cnt":[{"n":5}],"top":[{"_id":1}],"none":[]}]');
    a.eq(await codeOf([{ $facet: { a: [{ $facet: { b: [] } }] } }]), 40600);
});

t.test("$bucket groups by boundary [lo, hi); the default bucket comes last; out-of-range without default is 40066", async (a) => {
    const r = await agg([{ $bucket: { groupBy: "$amt", boundaries: [0, 5, 10], default: "big", output: { n: { $sum: 1 }, ids: { $push: "$_id" } } } }]);
    a.eq(J(r), '[{"_id":0,"n":2,"ids":[4,5]},{"_id":5,"n":2,"ids":[2,3]},{"_id":"big","n":1,"ids":[1]}]');
    a.eq(J(await agg([{ $bucket: { groupBy: "$amt", boundaries: [0, 100] } }])), '[{"_id":0,"count":5}]');
    a.eq(await codeOf([{ $bucket: { groupBy: "$amt", boundaries: [0, 5] } }]), 40066);
    a.eq(await codeOf([{ $bucket: { groupBy: "$amt", boundaries: [5, 0] } }]), 40194);
    a.eq(await codeOf([{ $bucket: { groupBy: "$amt", boundaries: [0, 5, 10], default: 7 } }]), 40199);
});

t.test("$sortByCount counts by value, most frequent first", async (a) => {
    a.eq(J(await agg([{ $unwind: "$items" }, { $sortByCount: "$items" }])), '[{"_id":"y","count":2},{"_id":"x","count":1}]');
});

t.test("$replaceRoot/$replaceWith need a document (40228)", async (a) => {
    a.eq(J(await agg([{ $match: { _id: 2 } }, { $replaceWith: { k: "$cust", a: "$amt" } }])), '[{"k":"b","a":5}]');
    a.eq(J(await agg([{ $match: { _id: 1 } }, { $replaceRoot: { newRoot: { $mergeObjects: [{ cust: "?", z: 1 }, { cust: "$cust" }] } } }])), '[{"cust":"a","z":1}]');
    a.eq(await codeOf([{ $replaceRoot: { newRoot: "$amt" } }]), 40228);
});

t.test("a $match on $regex after $group runs on the JS thread", async (a) => {
    a.eq(J(await agg([{ $group: { _id: "$cust" } }, { $match: { _id: /^[ab]$/ } }, { $sort: { _id: 1 } }])), '[{"_id":"a"},{"_id":"b"}]');
});

t.test("stage-level errors: unknown stage 40324, two keys 40323, non-array pipeline 14; they reject, never throw", async (a) => {
    a.eq(await codeOf([{ $bogus: 1 }]), 40324);
    a.eq(await codeOf([{ $match: {}, $limit: 1 }]), 40323);
    a.eq(await codeOf("x"), 14);
    a.eq(await codeOf(new Array(1001).fill({ $skip: 0 })), 7749, "at most 1000 stages");
    a.eq(await codeOf(new Array(4294967295)), 7749, "a sparse length is refused before it is walked");
    a.eq(await codeOf([{ $project: { q: { $divide: [1, 0] } } }]), 16608);
    let threw = false, p;
    try {
        p = o.aggregate([{ $bogus: 1 }]);
    } catch {
        threw = true;
    }
    a.eq(threw, false);
    await a.rejects(p, DocDBError);
});

t.test("the input is the collection as of the call, small (inline) and large (offloaded)", async (a) => {
    const big = db.collection("big");
    const docs = [];
    for (let i = 0; i < 5000; i++) docs.push({ _id: i, g: i % 3 });
    big.insertMany(docs);
    const p1 = o.aggregate([{ $count: "n" }]);
    const p2 = big.aggregate([{ $group: { _id: "$g", n: { $sum: 1 } } }, { $sort: { _id: 1 } }]);
    o.insertOne({ _id: 99, cust: "q", amt: 0 });
    big.deleteMany({ g: 0 });
    a.eq(J(await p1), '[{"n":5}]');
    a.eq(J(await p2), '[{"_id":0,"n":1667},{"_id":1,"n":1667},{"_id":2,"n":1666}]');
    o.deleteOne({ _id: 99 });
    a.eq(J(await big.aggregate([{ $count: "n" }])), '[{"n":3333}]');
});

t.test("a collection that does not exist aggregates as empty; $lookup from it joins nothing", async (a) => {
    a.eq(J(await db.collection("nope").aggregate([{ $count: "n" }])), "[]");
    a.eq(db.listCollections().includes("nope"), false);
});

await t.run();
db.close();
