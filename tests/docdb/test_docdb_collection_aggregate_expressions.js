// covers: dyna:docdb.Collection.aggregate
// The aggregation expression language against hand-derived answers from the MongoDB manual's operator pages:
// field paths through arrays of documents, $$ROOT/$$CURRENT/$$REMOVE, arithmetic result types (int + int stays int,
// int32 overflow widens to long, $divide is always double, date + number is a date, date - date is milliseconds),
// comparison across BSON types, $cond (array and object forms), $switch, $ifNull, strings (code points, not bytes),
// arrays ($map/$filter with "as" and $$this, $slice, $arrayElemAt with negative index, $in, $concatArrays, $size),
// $mergeObjects (later value wins, first position kept), $objectToArray, conversions, date parts in UTC, and the
// error codes for bad operands.
import { suite } from "../lib/kit.js";
import { Database, DocDBError } from "dyna:docdb";

const t = suite("docdb.Collection.aggregate expressions");
const db = new Database("expr");
const c = db.collection("c");
c.insertMany([
    { _id: 1, n: 2147483647, l: 5n, f: 1.5, arr: [1, 2, 3], s: "Hello World", sub: { x: 1, y: [{ z: 1 }, { z: 2 }] }, d: new Date("2024-12-31T23:59:59.250Z") },
    { _id: 2, n: 1, f: null, arr: [], s: "héllo", sub: { x: 2 } },
    { _id: 3, n: -5, s: "a,b,,c" },
]);
const J = (v) => JSON.stringify(v, (k, x) => (typeof x === "bigint" ? `${x}n` : x));
const ev = async (expr, id = 1) => {
    const r = await c.aggregate([{ $match: { _id: id } }, { $project: { _id: 0, r: expr } }]);
    return r.length && "r" in r[0] ? r[0].r : "MISSING";
};
const codeOf = async (expr) => {
    try {
        await c.aggregate([{ $project: { r: expr } }]);
        return "no throw";
    } catch (e) {
        return e instanceof DocDBError ? e.code : String(e);
    }
};

t.test("field paths and variables", async (a) => {
    a.eq(J(await ev("$sub.y.z")), "[1,2]", "a path through an array of documents collects each element's value");
    a.eq(await ev("$nope"), "MISSING", "a missing field leaves the computed field out");
    a.eq(await ev("$$REMOVE"), "MISSING");
    a.eq(await ev("$$ROOT._id", 2), 2);
    a.eq(await ev("$$CURRENT.n", 3), -5);
    a.eq(await codeOf("$$nope"), 17276);
});

t.test("arithmetic and its result types", async (a) => {
    a.eq(J(await ev({ $add: ["$n", 1] })), "2147483648", "int32 overflow widens");
    a.eq(J(await ev({ $type: { $add: ["$n", 1] } })), '"long"');
    a.eq(J(await ev({ $type: { $add: [1, 2] } })), '"int"');
    a.eq(J(await ev({ $type: { $add: ["$l", 1] } })), '"long"', "long + int is long");
    a.eq(J(await ev({ $type: { $divide: [4, 2] } })), '"double"');
    a.eq(await ev({ $multiply: ["$f", 4] }), 6);
    a.eq(await ev({ $add: [1, null] }), null, "null operand gives null");
    a.eq(await ev({ $subtract: ["$d", new Date("2024-12-31T00:00:00Z")] }), 86399250, "date - date is milliseconds");
    a.eq(J(await ev({ $add: ["$d", 750] })), '"2025-01-01T00:00:00.000Z"', "date + number is a date");
    a.eq(await ev({ $mod: ["$n", 3] }, 3), -2, "$mod keeps the dividend's sign");
    a.eq(await ev({ $round: [{ $divide: ["$n", 3] }, 2] }, 3), -1.67);
    a.eq(await ev({ $abs: "$n" }, 3), 5);
    a.eq(await ev({ $floor: 2.5 }), 2);
    a.eq(await ev({ $ceil: 2.1 }), 3);
    a.eq(await ev({ $pow: [2, 10] }), 1024);
    a.eq(await ev({ $sqrt: 16 }), 4);
    a.eq(await codeOf({ $divide: [1, 0] }), 16608);
    a.eq(await codeOf({ $add: ["$s", 1] }), 16554);
    a.eq(await codeOf({ $subtract: ["$s", 1] }), 16556);
});

t.test("comparison follows the BSON type order; $cond/$switch/$ifNull/$and/$or/$not", async (a) => {
    a.eq(await ev({ $lt: [5, "a"] }), true, "numbers sort before strings");
    a.eq(await ev({ $lt: [null, 0] }), true);
    a.eq(await ev({ $eq: ["$nope", null] }), false, "missing is not null in an expression comparison");
    a.eq(await ev({ $cmp: ["b", "a"] }), 1);
    a.eq(await ev({ $cond: [{ $gt: ["$n", 0] }, "pos", "neg"] }, 3), "neg");
    a.eq(await ev({ $cond: { if: "$f", then: "t", else: "e" } }, 2), "e", "null is falsy");
    a.eq(await ev({ $switch: { branches: [{ case: { $eq: ["$_id", 2] }, then: "two" }], default: "other" } }, 2), "two");
    a.eq(await codeOf({ $switch: { branches: [{ case: false, then: 1 }] } }), 40066);
    a.eq(await ev({ $ifNull: ["$f", "$nope", "dflt"] }, 2), "dflt");
    a.eq(await ev({ $and: [1, "x", []] }), true, "only false, 0, null and missing are falsy");
    a.eq(await ev({ $or: [0, null, "$nope"] }), false);
    a.eq(await ev({ $not: [0] }), true);
});

t.test("strings count code points", async (a) => {
    a.eq(await ev({ $strLenCP: "$s" }, 2), 5);
    a.eq(await ev({ $substrCP: ["$s", 1, 3] }, 2), "éll");
    a.eq(await ev({ $concat: ["$s", "!", { $toString: "$_id" }] }, 2), "héllo!2");
    a.eq(await ev({ $concat: ["$s", "$nope"] }, 2), null);
    a.eq(J(await ev({ $split: ["$s", ","] }, 3)), '["a","b","","c"]');
    a.eq(await ev({ $toUpper: "$s" }), "HELLO WORLD");
    a.eq(await ev({ $toLower: "$s" }), "hello world");
    a.eq(await ev({ $trim: { $literal: "  a b  " } }), "a b");
    a.eq(await codeOf({ $concat: ["$s", 1] }), 16702);
});

t.test("arrays", async (a) => {
    a.eq(J(await ev({ $map: { input: "$arr", as: "v", in: { $multiply: ["$$v", 10] } } })), "[10,20,30]");
    a.eq(J(await ev({ $filter: { input: "$sub.y", cond: { $gt: ["$$this.z", 1] } } })), '[{"z":2}]');
    a.eq(J(await ev({ $map: { input: { $split: ["a-b", "-"] }, in: { $concat: ["$$this", "$$this"] } } })), '["aa","bb"]', "over a computed array");
    a.eq(await ev({ $filter: { input: "$nope", cond: true } }), null);
    a.eq(await ev({ $size: "$arr" }), 3);
    a.eq(await ev({ $arrayElemAt: ["$arr", -1] }), 3);
    a.eq(await ev({ $arrayElemAt: ["$arr", 7] }), "MISSING");
    a.eq(await ev({ $first: "$arr" }), 1);
    a.eq(await ev({ $last: "$arr" }), 3);
    a.eq(J(await ev({ $slice: ["$arr", -2] })), "[2,3]");
    a.eq(J(await ev({ $slice: ["$arr", 1, 1] })), "[2]");
    a.eq(J(await ev({ $concatArrays: ["$arr", [9], []] })), "[1,2,3,9]");
    a.eq(await ev({ $in: [2, "$arr"] }), true);
    a.eq(await ev({ $isArray: "$s" }), false);
    a.eq(await ev({ $sum: "$arr" }), 6, "$sum over an array operand");
    a.eq(await ev({ $max: [1, 5, null, 3] }), 5);
    a.eq(await ev({ $avg: [] }), null);
    a.eq(await codeOf({ $size: "$s" }), 17124);
    a.eq(await codeOf({ $in: [1, "$s"] }), 40081);
});

t.test("objects", async (a) => {
    a.eq(J(await ev({ $mergeObjects: ["$sub", { x: 9, w: 1 }] })), '{"x":9,"y":[{"z":1},{"z":2}],"w":1}', "later value wins, first position kept");
    a.eq(J(await ev({ $mergeObjects: ["$nope", { a: 1 }] })), '{"a":1}');
    a.eq(J(await ev({ $objectToArray: "$sub" }, 2)), '[{"k":"x","v":2}]');
    a.eq(J(await ev({ a: "$n", b: { c: "$nope" } }, 3)), '{"a":-5,"b":{}}');
    a.eq(await codeOf({ $mergeObjects: [1] }), 40400);
});

t.test("conversions and $type", async (a) => {
    a.eq(await ev({ $type: "$l" }), "long");
    a.eq(await ev({ $type: "$nope" }), "missing");
    a.eq(await ev({ $toInt: "12" }), 12);
    a.eq(await ev({ $toDouble: "$n" }, 3), -5);
    a.eq(await ev({ $toString: "$f" }), "1.5");
    a.eq(await ev({ $toString: "$d" }), "2024-12-31T23:59:59.250Z");
    a.eq(await ev({ $toBool: 0 }), false);
    a.eq(await codeOf({ $toInt: "12x" }), 241);
    a.eq(await codeOf({ $toInt: 3e10 }), 241);
});

t.test("date parts are UTC", async (a) => {
    const r = await ev({ y: { $year: "$d" }, m: { $month: "$d" }, d: { $dayOfMonth: "$d" }, h: { $hour: "$d" }, mi: { $minute: "$d" }, s: { $second: "$d" }, ms: { $millisecond: "$d" }, dow: { $dayOfWeek: "$d" }, doy: { $dayOfYear: "$d" } });
    a.eq(J(r), '{"y":2024,"m":12,"d":31,"h":23,"mi":59,"s":59,"ms":250,"dow":3,"doy":366}', "2024 is a leap year; 31 Dec 2024 is a Tuesday (Sunday = 1)");
    a.eq(await ev({ $year: "$nope" }), null);
    a.eq(await ev({ $year: new Date("1969-12-31T23:00:00Z") }), 1969, "before the epoch");
    a.eq(await codeOf({ $year: "$s" }), 16006);
});

t.test("parse errors: unknown operator 168, wrong arity 16020, two operator keys", async (a) => {
    a.eq(await codeOf({ $bogus: 1 }), 168);
    a.eq(await codeOf({ $subtract: [1] }), 16020);
    a.eq(await codeOf({ $add: [1], $abs: 1 }), 9);
    a.eq(await codeOf({ $size: { "a.b": 1 } }), 16412);
});

await t.run();
db.close();
