// covers: dyna:docdb.Collection.distinct
// distinct(field) without a filter, on a field with a single-field, non-multikey, non-sparse, non-partial index, reads
// one document per index key (the first in slot order whose field is present) instead of every document, and orders
// the values by that document, which is the order of first appearance a scan produces. A representative whose value is
// an array (a one-element or empty array does not mark an index multikey) falls back to the scan. Each answer must
// equal an unindexed twin's exactly (values, types, order), over mixed BSON types, deletes, updates and a filter. The
// scan reuses one keystring buffer for every value, so a long value grows it and a short one after it must not see
// stale bytes.
import { suite } from "../lib/kit.js";
import { Database } from "dyna:docdb";
import { Decimal128 } from "dyna:serialize";
import { Random } from "dyna:random";

const t = suite("docdb.Collection.distinct index keys");
const db = new Database("distinct");
const rng = new Random(9);
const pool = [1, 1.0, 2, 3.5, -0, 0, NaN, Infinity, 2n ** 40n, new Decimal128("2.5"), "a", "b", "", null, undefined, true,
    false, new Date(0), { k: 1 }, { k: [1, 2] }];
const mk = (n, arrays) => Array.from({ length: n }, (_, i) => {
    const d = { _id: i, w: rng.nextBounded(4) };
    if (rng.nextBounded(9)) d.v = arrays && i % 97 === 5 ? [pool[i % pool.length]] : pool[rng.nextBounded(pool.length)];
    return d;
});
const J = (x) => JSON.stringify(x, (k, v) =>
    typeof v === "bigint" ? `${v}n` : typeof v === "number" && !Number.isFinite(v) ? String(v) : Object.is(v, -0) ? "-0" : v instanceof Decimal128 ? `D${v}` : v);
const pair = (name, docs) => {
    const ix = db.collection(`${name}_ix`), plain = db.collection(`${name}_plain`);
    ix.insertMany(docs.map((d) => ({ ...d })));
    plain.insertMany(docs.map((d) => ({ ...d })));
    ix.createIndex({ v: 1 });
    return [ix, plain];
};

t.test("values, types and first-appearance order agree with the unindexed twin", (a) => {
    const [ix, plain] = pair("s", mk(3000, false));
    a.eq(J(ix.distinct("v")), J(plain.distinct("v")));
    a.eq(J(ix.distinct("w")), J(plain.distinct("w")));
    a.eq(J(ix.distinct("v", { w: 1 })), J(plain.distinct("v", { w: 1 })));
    a.ok(ix.distinct("v").length >= 15, "the pool's distinct values are all present");
});

t.test("one-element arrays fall back to the scan and still agree", (a) => {
    const [ix, plain] = pair("arr", mk(3000, true));
    a.eq(J(ix.distinct("v")), J(plain.distinct("v")));
});

t.test("after deletes and updates the representatives are still the first present documents", (a) => {
    const [ix, plain] = pair("churn", mk(2000, false));
    for (const c of [ix, plain]) {
        c.deleteMany({ _id: { $lt: 300 } });
        c.updateMany({ w: 2 }, { $unset: { v: 1 } });
        c.updateMany({ w: 3, v: "a" }, { $set: { v: "z" } });
        c.insertMany([{ _id: "late", v: "late" }, { _id: "first?", v: 1 }]);
    }
    a.eq(J(ix.distinct("v")), J(plain.distinct("v")));
});

t.test("values longer than the keystring stack scratch are deduplicated on the heap", (a) => {
    const c = db.collection("long");
    const long = (i) => `${"L".repeat(300)}${i % 3}`;
    c.insertMany(Array.from({ length: 30 }, (_, i) => ({ _id: i, s: long(i), d: { blob: long(i).repeat(2) } })));
    a.eq(c.distinct("s").length, 3);
    a.eq(c.distinct("s", { _id: { $gte: 0 } }).join("|"), [0, 1, 2].map(long).join("|"));
    a.eq(c.distinct("d").length, 3);
});

await t.run();
db.close();
