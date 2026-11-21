// covers: dyna:docdb.Collection.aggregate
// $unwind followed by $group runs as one streaming pass: each array element is read in place as a view into its parent
// document (no unwound document is built), the group's field paths are fetched in one scan per element, and $add,
// $subtract and $multiply over fields and literals are computed without the general expression evaluator. Anything that
// pass cannot answer exactly (a path that is a prefix of the unwound path, an array value, a date, a decimal, an
// includeArrayIndex) falls back per element to building the unwound document. Each pipeline here is checked against its
// twin that cannot take either shortcut ({$match: {}} between the stages and every expression wrapped in $cond), so the
// answers, result types and error codes must agree exactly; a few are also pinned to hand-derived values.
import { suite } from "../lib/kit.js";
import { Database, DocDBError } from "dyna:docdb";

const t = suite("docdb.Collection.aggregate fused $unwind+$group");
const db = new Database("fused");
const o = db.collection("o");
o.insertMany([
    {
        _id: 1,
        cust: "a",
        amt: 3,
        meta: { k: "m1", list: [1, 2] },
        items: [
            { sku: "x", qty: 2, price: 5 },
            { sku: "y", qty: 1, price: 2.5 },
        ],
    },
    {
        _id: 2,
        cust: "b",
        amt: 2,
        meta: { k: "m2", list: [] },
        items: [
            { sku: "x", qty: 2147483647, price: 2 },
            { sku: "z", qty: 4611686018427387904n, price: 2 },
        ],
    },
    {
        _id: 3,
        cust: "a",
        amt: 1,
        meta: { k: "m1" },
        items: [
            { sku: "y", qty: null, price: 4 },
            { sku: "y", price: 4 },
            { qty: 3, price: 1 },
        ],
    },
    { _id: 4, cust: "c", amt: 4, items: [] },
    { _id: 5, cust: "c", amt: 5, items: null },
    { _id: 6, cust: "d", amt: 6 },
    { _id: 7, cust: "d", amt: 7, items: "solo" },
    {
        _id: 8,
        cust: "e",
        amt: 8,
        items: [[1, 2], { sku: [1, 2], qty: 1, price: 1 }, 5, { sku: "x", qty: -4, price: 1.5 }],
    },
    {
        _id: 9,
        cust: "e",
        amt: 9,
        items: [
            { sku: "w", qty: { n: 1 }, price: 1, sub: { a: 1 } },
            { sku: "w", qty: 1, price: 2, sub: { a: 2 } },
        ],
    },
]);
const J = (v) => JSON.stringify(v, (k, x) => (typeof x === "bigint" ? `${x}n` : x));
const slow = (e) => ({ $cond: [true, e, null] });
function twin(pipe) {
    const out = [];
    for (const st of pipe) {
        if (st.$group) {
            const g = {};
            for (const [k, v] of Object.entries(st.$group))
                g[k] = k === "_id" ? slow(v) : { [Object.keys(v)[0]]: slow(Object.values(v)[0]) };
            out.push({ $match: {} }, { $group: g });
        } else out.push(st);
    }
    return out;
}
async function run(pipe) {
    try {
        return J(await o.aggregate(pipe));
    } catch (e) {
        return e instanceof DocDBError ? `code ${e.code}` : String(e);
    }
}
async function same(a, pipe) {
    const fast = await run(pipe);
    a.eq(fast, await run(twin(pipe)), J(pipe));
    return fast;
}
const SORT = { $sort: { _id: 1 } };

t.test("element fields, sums, products and result types agree with the general path", async (a) => {
    const g = {
        _id: "$items.sku",
        units: { $sum: "$items.qty" },
        rev: { $sum: { $multiply: ["$items.qty", "$items.price"] } },
        plus: { $push: { $add: ["$items.qty", 1, "$amt"] } },
        minus: { $push: { $subtract: ["$items.price", "$items.qty"] } },
        lo: { $min: "$items.qty" },
        hi: { $max: "$items.price" },
        first: { $first: "$cust" },
        set: { $addToSet: "$cust" },
        avg: { $avg: "$items.price" },
        n: { $sum: 1 },
    };
    const r = await same(a, [{ $match: { _id: { $in: [1, 2, 3] } } }, { $unwind: "$items" }, { $group: g }, SORT]);
    const x = JSON.parse(r).find((d) => d._id === "x");
    a.eq(x.units, 2147483649);
    a.eq(x.rev, 10 + 4294967294);
    a.eq(J(x.plus), J([2 + 1 + 3, 2147483647 + 1 + 2]));
    a.eq(J(x.minus), J([3, 2 - 2147483647]));
    const e = await same(a, [
        { $match: { _id: 8 } },
        { $unwind: "$items" },
        { $group: { _id: "$items.sku", p: { $push: { $add: ["$items.qty", 1] } } } },
    ]);
    a.eq(e, "code 16554");
});

t.test("int32 overflow widens to long, long overflow to double, nullish operands give null", async (a) => {
    const p = [
        { $match: { _id: 2 } },
        { $unwind: "$items" },
        {
            $group: {
                _id: "$items.sku",
                m: { $push: { $multiply: ["$items.qty", "$items.price", 4] } },
                s: { $push: { $add: ["$items.qty", "$items.qty"] } },
            },
        },
        SORT,
    ];
    const r = await same(a, p);
    a.eq(
        r,
        J([
            { _id: "x", m: [17179869176], s: [4294967294] },
            { _id: "z", m: [2 ** 62 * 8], s: [2 ** 63] },
        ]),
    );
    const t = (await o.aggregate(p))[1];
    a.eq(typeof t.m[0] === "number" && typeof t.s[0] === "number", true);
    const l = await o.aggregate([
        { $match: { _id: 2 } },
        { $unwind: "$items" },
        { $group: { _id: "$items.sku", v: { $first: { $add: ["$items.qty", 1] } } } },
        SORT,
    ]);
    a.eq(J(l.map((d) => d.v)), J([2147483648, 4611686018427387905n]));
    const w = db.collection("w");
    w.insertOne({
        _id: 1,
        xs: [
            { q: 5n, r: 2 },
            { q: 7, r: 2.5 },
        ],
    });
    const ty = [
        { $unwind: "$xs" },
        {
            $group: {
                _id: "$xs.r",
                a: { $first: { $add: ["$xs.q", 0] } },
                s: { $first: { $subtract: ["$xs.q", 1] } },
                m: { $first: { $multiply: ["$xs.q", "$xs.r"] } },
            },
        },
        { $project: { a: { $type: "$a" }, s: { $type: "$s" }, m: { $type: "$m" } } },
        SORT,
    ];
    a.eq(
        J(await w.aggregate(ty)),
        J([
            { _id: 2, a: "long", s: "long", m: "long" },
            { _id: 2.5, a: "int", s: "int", m: "double" },
        ]),
    );
    a.eq(J(await w.aggregate(twin(ty))), J(await w.aggregate(ty)));
    const n = await same(a, [
        { $match: { _id: 3 } },
        { $unwind: "$items" },
        {
            $group: {
                _id: null,
                v: { $push: { $multiply: ["$items.qty", 2] } },
                d: { $push: { $subtract: ["$items.qty", 1] } },
            },
        },
    ]);
    a.eq(n, J([{ _id: null, v: [null, null, 6], d: [null, null, 2] }]));
});

t.test("scalars, nested arrays and the element itself fall back per element", async (a) => {
    await same(a, [{ $unwind: "$items" }, { $group: { _id: "$items", n: { $sum: 1 }, c: { $push: "$cust" } } }, SORT]);
    await same(a, [
        { $unwind: "$items" },
        { $group: { _id: "$cust", e: { $push: "$items" }, k: { $push: "$items.sku" } } },
        SORT,
    ]);
    await same(a, [{ $unwind: "$items" }, { $group: { _id: "$items.sub.a", q: { $push: "$items.qty" } } }, SORT]);
});

t.test("preserveNullAndEmptyArrays keeps missing, null, [] and scalars as one row each", async (a) => {
    const r = await same(a, [
        { $unwind: { path: "$items", preserveNullAndEmptyArrays: true } },
        { $group: { _id: "$cust", n: { $sum: 1 }, s: { $push: "$items.sku" } } },
        SORT,
    ]);
    a.eq(
        J(JSON.parse(r).map((d) => [d._id, d.n])),
        J([
            ["a", 5],
            ["b", 2],
            ["c", 2],
            ["d", 2],
            ["e", 6],
        ]),
    );
});

t.test("parent fields beside the unwound path, and a path inside the same parent document", async (a) => {
    await same(a, [
        { $unwind: "$items" },
        { $group: { _id: "$meta.k", t: { $sum: { $multiply: ["$items.price", "$amt"] } } } },
        SORT,
    ]);
    const r = await same(a, [
        { $unwind: "$meta.list" },
        { $group: { _id: "$meta.k", l: { $push: "$meta.list" }, m: { $push: "$meta" } } },
        SORT,
    ]);
    a.eq(J(JSON.parse(r).map((d) => d.l)), J([[1, 2]]));
    a.eq(
        J(JSON.parse(r)[0].m),
        J([
            { k: "m1", list: 1 },
            { k: "m1", list: 2 },
        ]),
    );
});

t.test("errors are the general evaluator's: a non-number operand is 16554, a date sum is a date", async (a) => {
    a.eq(
        await same(a, [
            { $match: { _id: 9 } },
            { $unwind: "$items" },
            { $group: { _id: null, v: { $sum: { $multiply: ["$items.qty", 2] } } } },
        ]),
        "code 16554",
    );
    const d = db.collection("d");
    d.insertOne({ _id: 1, at: new Date(1000), xs: [{ ms: 5 }, { ms: 7 }] });
    const p = [{ $unwind: "$xs" }, { $group: { _id: null, t: { $push: { $add: ["$at", "$xs.ms"] } } } }];
    const fast = J(await d.aggregate(p));
    a.eq(fast, J(await d.aggregate(twin(p))));
    a.eq(fast, J([{ _id: null, t: [new Date(1005), new Date(1007)] }]));
});

t.test("includeArrayIndex and $bucket after $unwind agree too", async (a) => {
    await same(a, [
        { $unwind: { path: "$items", includeArrayIndex: "i" } },
        { $group: { _id: "$i", n: { $sum: 1 } } },
        SORT,
    ]);
    await same(a, [
        { $unwind: "$items" },
        { $bucket: { groupBy: "$items.price", boundaries: [0, 2, 5], default: "big", output: { n: { $sum: 1 } } } },
    ]);
});

await t.run();
db.close();
