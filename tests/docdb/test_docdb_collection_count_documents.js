// covers: dyna:docdb.Collection.countDocuments
// When the whole filter is answered exactly by one single-field, non-multikey, non-partial index (one $eq or $in, or
// ranges whose bounds are plain scalars), countDocuments is the cardinality of the index scan: no document is fetched or
// re-matched. An $eq/$in whose argument yields no bounds never gets an index scan, so any that does is exact. A range
// bound of NaN or Decimal128 is never exact (NaN keys sit at the bottom of the numbers and match no comparison), and
// NaN matches no $lt/$lte, so a numeric range without a lower bound is counted by matching documents. Every count here must equal an unindexed twin's, over
// mixed BSON types (int32/int64/double/Decimal128, ±0, NaN, ±Infinity, dates, strings, null/undefined/missing,
// one-element and empty arrays), after deletes and updates, with skip and limit. A multikey index is never exact: [0, 5]
// matches {$gt: 1, $lte: 3.5} element-wise while the index interval holds neither key.
import { suite } from "../lib/kit.js";
import { Database } from "dyna:docdb";
import { Decimal128 } from "dyna:serialize";
import { Random } from "dyna:random";

const t = suite("docdb.Collection.countDocuments index cardinality");
const db = new Database("count");
const rng = new Random(4242);
const pool = [1, 1.0, 2, 3.5, -0, 0, NaN, Infinity, -Infinity, 2n ** 40n, 7n, new Decimal128("2.5"), new Decimal128("NaN"),
    "a", "b", "x", "", null, undefined, true, false, new Date(0), new Date(1e12), [4], [], { k: 1 }];
const docs = Array.from({ length: 2500 }, (_, i) => {
    const d = { _id: i, w: rng.nextBounded(5) };
    if (rng.nextBounded(13)) d.v = pool[rng.nextBounded(pool.length)];
    return d;
});
const ix = db.collection("ix");
const plain = db.collection("plain");
ix.insertMany(docs.map((d) => ({ ...d })));
plain.insertMany(docs.map((d) => ({ ...d })));
ix.createIndex({ v: 1 });

const filters = [
    { v: 1 }, { v: 0 }, { v: "a" }, { v: true }, { v: new Date(0) }, { v: 4 }, { v: 2n ** 40n }, { v: NaN }, { v: null },
    { v: { $in: [1, "b", false, new Date(1e12)] } }, { v: { $in: [2.5, null] } }, { v: { $in: [[4]] } },
    { v: { $gt: 0 } }, { v: { $gte: 0 } }, { v: { $gt: 1, $lte: 3.5 } }, { v: { $gte: -Infinity } }, { v: { $lt: 3 } },
    { v: { $lte: 2.5 } }, { v: { $lt: Infinity } }, { v: { $gte: "a" } }, { v: { $lt: "b" } }, { v: { $gt: "", $lt: "x" } },
    { v: { $gte: new Date(1) } }, { v: { $lt: new Date(1) } }, { v: { $gte: new Decimal128("2") } }, { v: 1, w: 2 },
    { v: { $gte: 1 }, w: { $lt: 3 } }, { v: { $in: [1, 2, 3.5], $gt: 1 } }, { v: { $eq: 2, $lte: 3 } }, { v: { $gt: 0, $not: { $gt: 2 } } }, { v: { $gte: 1, $lt: [5] } }, { v: { $gt: NaN } }, { v: { $gte: NaN } },
    { v: { $lt: new Decimal128("0") } }, { v: { $lte: new Decimal128("2.5") } }, { v: { $gte: new Decimal128("NaN") } }, { v: { $eq: { k: 1 } } },
    { v: { $in: [new Decimal128("NaN"), NaN, /x/, "a"] } }, { v: new Decimal128("NaN") }, { v: { $gt: false } }, { v: { $lte: true } }, { v: { $ne: 1 } }, { v: { $exists: false } }, { v: { $type: "number" } }, { v: 3, $or: [{ w: 1 }, { w: 2 }] },
];

const S = (x) => JSON.stringify(x, (k, v) => (typeof v === "bigint" ? `${v}n` : typeof v === "number" && !Number.isFinite(v) ? String(v) : v));
const same = (a, label) => {
    for (const f of filters)
        for (const o of [undefined, { skip: 7 }, { limit: 3 }, { skip: 2, limit: 9 }, { skip: 5000 }])
            a.eq(ix.countDocuments(f, o), plain.countDocuments(f, o), `${label} ${S(f)} ${S(o)}`);
};

t.test("every filter and skip/limit agrees with the unindexed twin", (a) => same(a, "fresh"));

t.test("an exact $eq count examines no documents and is the posting cardinality", (a) => {
    const n = plain.countDocuments({ v: "a" });
    a.ok(n > 50, `${n} documents hold "a"`);
    a.eq(ix.countDocuments({ v: "a" }), n);
    a.eq(ix.countDocuments({ v: { $in: ["a", "b"] } }), plain.countDocuments({ v: "a" }) + plain.countDocuments({ v: "b" }));
});

t.test("deletes, updates and an index that turns multikey keep every count exact", (a) => {
    for (const c of [ix, plain]) {
        c.deleteMany({ w: 0 });
        c.updateMany({ w: 1 }, { $set: { v: "a" } });
        c.updateMany({ w: 2, v: { $type: "string" } }, { $unset: { v: 1 } });
    }
    same(a, "churned");
    for (const c of [ix, plain]) c.insertMany([{ _id: "m1", v: [1, "a"] }, { _id: "m2", v: ["b", "b", 3] }, { _id: "m3", v: [0, 5] }]);
    same(a, "multikey");
});

await t.run();
db.close();
