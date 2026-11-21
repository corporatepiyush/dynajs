// covers: dyna:docdb.Collection.find
// P5 scan filter fast paths. (1) A range or equality between two int32/double values compares the doubles
// directly instead of going through the type-rank check and the general BSON comparison; NaN matches only NaN
// and only under $gte/$lte, -0 equals 0, and int64/Decimal128 operands keep the general path. (2) String $eq
// compares the length prefix and the bytes (embedded NULs included) instead of the general comparison. (3) The
// predicates of one node that read the same top-level field ({v: {$gte: a, $lt: b}}, $all, sibling $not) fetch
// that field once per document: the first lookup is remembered for the document being matched (keyed by the
// document's address and the field name, reset at the start of every top-level match), so nested documents
// ($elemMatch elements with the same key names) and the next document never see it. Measured at 100K documents,
// best of 3 interleaved: string $eq scan 4.69 -> 2.95 ms, numeric range scan 12.2 -> 8.2 ms, $elemMatch scan
// 23.9 -> 18.7 ms, countDocuments scan 9.0 -> 6.8 ms. Oracles: a JS reference of the MongoDB comparison rules
// for numbers and strings (type brackets, NaN, -0), and the same filters with the predicate order and nesting
// varied; the last case pins the single fetch with a thread-CPU ratio (no wall clock).
import { suite } from "../lib/kit.js";
import { Database } from "dyna:docdb";
import { threadCpuNow } from "dyna:sys";

const t = suite("docdb.Collection.find scan filter fast paths");
const db = new Database("scanfilter");
const J = JSON.stringify;

const VALUES = [0, -0, 1, -1, 2.5, -2.5, 3, 1e300, -1e300, Infinity, -Infinity, NaN, 2147483647, -2147483648, 0.1, 7,
    "", "a", "a\u0000", "a\u0000b", "ab", "b", "é", "7", null, true, [1, 7], [2.5, "a"], { x: 1 }];
const c = db.collection("v");
c.insertMany(VALUES.map((v, i) => ({ _id: i, v, w: { v: i % 3 }, arr: [{ v: i }, { v: -i }] })));

const num = (x) => typeof x === "number";
function cmp(op, a, b) {
    if (num(a) && num(b)) {
        if (a !== a || b !== b) return a !== a && b !== b && (op === "$gte" || op === "$lte" || op === "$eq");
        return op === "$gt" ? a > b : op === "$gte" ? a >= b : op === "$lt" ? a < b : op === "$lte" ? a <= b : a === b;
    }
    if (typeof a === "string" && typeof b === "string") return op === "$gt" ? a > b : op === "$gte" ? a >= b : op === "$lt" ? a < b : op === "$lte" ? a <= b : a === b;
    return false;
}
const matches = (doc, op, arg) => (Array.isArray(doc.v) ? doc.v.some((e) => cmp(op, e, arg)) : cmp(op, doc.v, arg));
const ids = (f) => J(c.find(f, { sort: { _id: 1 } }).map((d) => d._id));
const want = (pred) => J(VALUES.map((v, i) => ({ _id: i, v })).filter(pred).map((d) => d._id));

t.test("numeric and string comparisons follow the type brackets, NaN and -0 rules for every operator", (a) => {
    let bad = 0;
    for (const arg of VALUES.filter((v) => num(v) || typeof v === "string"))
        for (const op of ["$gt", "$gte", "$lt", "$lte", "$eq"]) {
            const got = ids({ v: { [op]: arg } }), exp = want((d) => matches(d, op, arg));
            if (got !== exp && bad++ < 4) a.eq(got, exp, `${op} ${typeof arg === "string" ? J(arg) : String(arg)}`);
        }
    a.eq(bad, 0);
});

t.test("int64 operands still compare by value", (a) => {
    a.eq(ids({ v: { $gte: 7n } }), want((d) => matches(d, "$gte", 7)));
    a.eq(ids({ v: { $lt: 1n } }), want((d) => matches(d, "$lt", 1)));
    a.eq(ids({ v: { $eq: 2147483647n } }), want((d) => matches(d, "$eq", 2147483647)));
});

t.test("two-sided ranges, $all, $not and $or on one field agree in every order and nesting", (a) => {
    let bad = 0;
    for (const [lo, hi] of [[0, 3], [-1e300, 0], [-Infinity, Infinity], [NaN, NaN], [2.5, 2.5], ["a", "b"], ["", "a\u0000b"]]) {
        const exp = want((d) => matches(d, "$gte", lo) && matches(d, "$lt", hi));
        const exp2 = want((d) => matches(d, "$gte", lo) && matches(d, "$lte", hi) && !matches(d, "$gt", hi));
        for (const f of [{ v: { $gte: lo, $lt: hi } }, { v: { $lt: hi, $gte: lo } }, { $and: [{ v: { $gte: lo } }, { v: { $lt: hi } }] }, { $and: [{ v: { $lt: hi } }, { v: { $gte: lo } }] }]) {
            const got = ids(f);
            if (got !== exp && bad++ < 4) a.eq(got, exp, J(f) + " " + String([lo, hi]));
        }
        const got2 = ids({ v: { $gte: lo, $lte: hi, $not: { $gt: hi } } });
        if (got2 !== exp2 && bad++ < 4) a.eq(got2, exp2, "with $not " + String([lo, hi]));
    }
    a.eq(ids({ v: { $all: [1, 7] } }), want((d) => Array.isArray(d.v) && d.v.includes(1) && d.v.includes(7)));
    a.eq(ids({ $or: [{ v: { $lt: -2 } }, { v: { $gt: 1e299 } }, { v: "a\u0000" }] }), want((d) => matches(d, "$lt", -2) || matches(d, "$gt", 1e299) || matches(d, "$eq", "a\u0000")));
    a.eq(bad, 0);
});

t.test("a remembered field never answers for a nested document or the next document", (a) => {
    const f = { v: { $gte: 0, $lt: 100 }, arr: { $elemMatch: { v: { $gte: 0, $lt: 1 } } }, "w.v": { $gte: 0 } };
    const exp = J(VALUES.map((v, i) => ({ _id: i, v })).filter((d) => matches(d, "$gte", 0) && d._id === 0).map((d) => d._id));
    a.eq(ids(f), exp, "only _id 0 has an element whose v lies in [0, 1)");
    const g = { v: { $gte: 2, $lte: 3 }, "w.v": { $gte: 1, $lte: 1 } };
    a.eq(ids(g), want((d) => matches(d, "$gte", 2) && matches(d, "$lte", 3) && d._id % 3 === 1));
    const p = db.collection("pull");
    p.insertOne({ _id: 1, list: [{ v: 1, k: 1 }, { v: 5, k: 2 }, { v: 9, k: 3 }] });
    p.updateOne({ _id: 1 }, { $pull: { list: { v: { $gte: 4, $lt: 10 } } } });
    a.eq(J(p.findOne({ _id: 1 }).list), J([{ v: 1, k: 1 }]));
});

t.test("a two-sided range on a late field fetches it once per document", (a) => {
    const w = db.collection("wide");
    const docs = Array.from({ length: 20000 }, (_, i) => {
        const d = { _id: i };
        for (let k = 0; k < 30; k++) d["f" + k] = k;
        d.late = i % 1000;
        return d;
    });
    w.insertMany(docs);
    const best = (f) => {
        let b = Infinity;
        for (let r = 0; r < 7; r++) {
            const t0 = threadCpuNow();
            w.countDocuments(f);
            b = Math.min(b, threadCpuNow() - t0);
        }
        return b;
    };
    const one = best({ late: { $gte: 100 } }), two = best({ late: { $gte: 100, $lt: 900 } });
    a.eq(w.countDocuments({ late: { $gte: 100, $lt: 900 } }), 16000);
    a.ok(two < one * 1.4, `a two-sided range cost ${(two / one).toFixed(2)}x a one-sided one (${two.toFixed(2)} vs ${one.toFixed(2)} ms CPU)`);
});

await t.run();
db.close();
