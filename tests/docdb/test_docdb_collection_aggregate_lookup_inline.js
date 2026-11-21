// covers: dyna:docdb.Collection.aggregate
// P9: a pipeline with a $lookup always ran on a worker, so even a 10-document $lookup paid the hand-off (30.6 us vs
// 11.6 us inline; 100 orders: 74.3 -> 56.7 us; 1000 orders + 100 customers: 532 -> 502 us; wall best of 7, load
// 10-12). It now runs on the JS thread when the input plus every $lookup collection holds fewer than 2048 documents
// (the same bound as a pipeline without $lookup) and off the JS thread from 2048 (dynajs.d.ts aggregate).
// Oracle for WHERE it ran: an inline run settles the promise before aggregate returns, so it is settled after a few
// microtask turns; an offloaded run settles only from the event loop. Oracle for WHAT it returns: the same pipeline
// over the same matching documents on both sides of the bound (the foreign side padded with non-matching documents)
// gives the same rows.
import { suite } from "../lib/kit.js";
import { Database } from "dyna:docdb";

const t = suite("docdb.Collection.aggregate $lookup inline below 2048 documents");
const db = new Database("p9lookup");
let seq = 0;

async function settledEarly(p) {
    let done = false;
    p.then(() => { done = true; }, () => { done = true; });
    for (let i = 0; i < 5; i++) await null;
    const early = done;
    return { early, rows: await p };
}
function pair(n, m, pad) {
    const k = seq++;
    const o = db.collection("o" + k), c = db.collection("c" + k);
    const od = [];
    for (let i = 0; i < n; i++) od.push({ _id: i, c: i % m, amt: i % 7 });
    if (od.length) o.insertMany(od);
    const cd = [];
    for (let i = 0; i < m; i++) cd.push({ _id: i, city: "city" + (i % 5) });
    for (let i = 0; i < pad; i++) cd.push({ _id: 100000 + i, city: "nowhere" });
    if (cd.length) c.insertMany(cd);
    return { o, from: "c" + k };
}
const pipe = (from) => [
    { $lookup: { from, localField: "c", foreignField: "_id", as: "cc" } },
    { $project: { city: { $first: "$cc.city" }, amt: 1 } },
    { $group: { _id: "$city", n: { $sum: 1 }, amt: { $sum: "$amt" } } },
    { $sort: { _id: 1 } },
];

t.test("below 2048 documents a $lookup runs inline, from 2048 on a worker", async ({ eq }) => {
    for (const [n, m, inline] of [[10, 5, true], [1000, 100, true], [1900, 147, true], [1900, 148, false], [3000, 10, false], [0, 2048, false], [0, 2047, true]]) {
        const { o, from } = pair(n, m, 0);
        const r = await settledEarly(o.aggregate(pipe(from)));
        eq(r.early, inline, n + " input + " + m + " foreign documents: settled before the event loop ran");
    }
});

t.test("inline and offloaded runs return the same rows", async ({ deep, eq }) => {
    for (const [n, m] of [[50, 7], [600, 40], [1500, 300]]) {
        const a = pair(n, m, 0), b = pair(n, m, 2048);
        const ra = await settledEarly(a.o.aggregate(pipe(a.from)));
        const rb = await settledEarly(b.o.aggregate(pipe(b.from)));
        eq(ra.early, true, n + "+" + m + " runs inline");
        eq(rb.early, false, n + "+" + m + " padded past 2048 runs on a worker");
        deep(ra.rows, rb.rows, n + "+" + m + " same rows");
        eq(ra.rows.reduce((s, r) => s + r.n, 0), n, "every input document grouped once");
    }
});

t.test("inline run sees the collections as of the call", async ({ eq }) => {
    const { o, from } = pair(20, 4, 0);
    const p = o.aggregate([{ $lookup: { from, localField: "c", foreignField: "_id", as: "cc" } }, { $match: { cc: { $size: 1 } } }]);
    db.collection(from).deleteMany({});
    o.insertMany([{ _id: 999, c: 0 }]);
    const rows = await p;
    eq(rows.length, 20, "rows from the pinned state");
});

t.test("a $regex pipeline stays on the JS thread at any size", async ({ eq }) => {
    const { o, from } = pair(3000, 10, 0);
    const r = await settledEarly(o.aggregate([{ $match: { amt: { $gte: 0 } } }, { $lookup: { from, localField: "c", foreignField: "_id", as: "cc" } }, { $match: { "cc.city": { $regex: "^city" } } }, { $count: "n" }]));
    eq(r.early, true, "regex keeps it inline");
    eq(r.rows[0].n, 3000, "count");
});

await t.run();
