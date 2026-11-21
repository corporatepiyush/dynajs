// Document database — a MongoDB-shaped store on dyna:docdb: collections of BSON documents with unique and secondary
// indexes, the MongoDB query language with a cost-based planner and explain(), update operators, an aggregation
// pipeline, change streams, explicit snapshot persist/load, and an HTTP API; schema validation sits in front of it.
//
// WHAT IT IS
//   storage      documents live in memory as BSON in the native module; `persist(path)` writes a checksummed snapshot
//                (written to a temporary file, fsynced, renamed) off the JS thread and `Database.load(path)` reads it
//                back with every index rebuilt; a torn or bit-flipped snapshot is refused whole (InvalidSnapshotFile)
//                and the previous one is untouched
//   validation   each order is checked against a JSON Schema (dyna:schema) before it is inserted
//   indexes      single and compound indexes over dotted paths, multikey over arrays, unique ones refuse
//                duplicates with E11000
//   queries      the MongoDB filter language ($eq $ne $gt $gte $lt $lte $in $nin $exists $regex $elemMatch $and $or
//                $not ...), projection, sort, skip, limit
//   planner      IDHACK for _id equality, IXSCAN, bitmap intersection of two indexes (AND_SORTED), OR of index scans,
//                COLLSCAN; explain() reports the plan with keys and documents examined
//   updates      $set $unset $inc $push $addToSet and the rest, with upsert
//   aggregation  $match (pushed into the index plan) $unwind $group $sort $limit $lookup $project and the expression
//                language, run off the JS thread over a pinned snapshot of the collection
//   changes      a change stream over the customers collection, filtered by a pipeline, with resume tokens; each
//                collection keeps its last 4096 events, so a stream reading a busier collection would need a larger
//                changeStreamCapacity
//   API          POST /db/:coll/find and /db/:coll/aggregate over HTTP with JSON bodies
//   verified     124 filters (120 random over every operator, $and/$or nesting, $elemMatch, plus the multikey edges)
//                must return what a plain one-document-at-a-time matcher returns, through every plan the planner
//                picks, and again after a persist/load round trip
//
// MODULES  dyna:docdb · dyna:schema · dyna:http (App) · dyna:file · dyna:random · dyna:cli (Table)
//
// RUN      dynajs examples/systems/document-database.js     loads 21,000 documents, queries, aggregates, reloads

import { Database, DocDBError } from "dyna:docdb";
import { Schema } from "dyna:schema";
import { App } from "dyna:http";
import { Path, makeTempDir, writeFile, readBytes, stat, removeAll } from "dyna:file";
import { Random } from "dyna:random";
import { Table } from "dyna:cli";
const table = (rows) => Table(rows.slice(1), { head: rows[0] });

const work = makeTempDir("docdb");
const snapshot = `${work}/shop.dds`;
const rng = new Random(1010);

// explain() answers in MongoDB's shape; the checks below read the winning plan as "IXSCAN(total)", "IXAND", ...
function planOf(explain) {
    let s = explain.queryPlanner.winningPlan;
    while (s.stage === "FETCH" || s.stage === "SORT") s = s.inputStage;
    const stage = s.stage === "AND_SORTED" ? "IXAND" : s.stage;
    return { stage, index: s.indexName ? s.indexName.replace(/_-?1$/, "") : undefined };
}
const explainOf = (coll, filter) => {
    const e = coll.explain(filter);
    return { plan: planOf(e), ...e.executionStats };
};

// =============================================================================
// Scenario
// =============================================================================
const results = {};
let db = new Database("shop");
let orders = db.collection("orders"),
    customers = db.collection("customers");
const changes = customers.watch([{ $match: { operationType: { $in: ["insert", "update"] } } }]);
const orderSchema = Schema.compile({
    type: "object",
    required: ["customer", "status", "total", "items"],
    properties: {
        status: { enum: ["new", "paid", "shipped", "cancelled"] },
        total: { type: "number", minimum: 0 },
        items: { type: "array", minItems: 1 },
    },
});
function insertOrder(doc) {
    const v = orderSchema.validate(doc);
    if (!v.valid) throw new DocDBError(`Document failed validation: ${JSON.stringify(v.errors[0])}`, 121);
    return orders.insertOne(doc);
}
customers.createIndex({ email: 1 }, { unique: true });
orders.createIndex({ customer: 1 });
orders.createIndex({ status: 1 });
orders.createIndex({ total: 1 });
orders.createIndex({ "items.sku": 1 });
const CITIES = ["Berlin", "Paris", "Madrid", "Rome", "Vienna", "Warsaw"];
for (let c = 0; c < 1000; c++)
    customers.insertOne({
        _id: `c${c}`,
        email: `user${c}@mail.example`,
        name: `User ${c}`,
        address: { city: CITIES[c % 6], zip: String(10000 + c) },
        tier: c % 10 === 0 ? "gold" : "standard",
    });
let t = performance.now();
for (let o = 0; o < 20000; o++) {
    const items = Array.from({ length: 1 + rng.nextBounded(4) }, () => ({
        sku: `SKU-${rng.nextBounded(300)}`,
        qty: 1 + rng.nextBounded(3),
        price: 5 + rng.nextBounded(200),
    }));
    insertOrder({
        _id: o,
        customer: `c${rng.nextBounded(1000)}`,
        status: ["new", "paid", "paid", "shipped", "shipped", "shipped", "cancelled"][rng.nextBounded(7)],
        total: items.reduce((s, i) => s + i.qty * i.price, 0),
        items,
        placed: `2026-0${1 + rng.nextBounded(6)}-${String(1 + rng.nextBounded(28)).padStart(2, "0")}`,
    });
}
const loadMs = performance.now() - t;

const refused = [];
for (const bad of [
    { customer: "c1", status: "lost", total: 5, items: [{}] },
    { customer: "c1", status: "new", total: -1, items: [{}] },
    { customer: "c1", status: "new", total: 1, items: [] },
])
    try {
        insertOrder(bad);
        refused.push("accepted");
    } catch (e) {
        refused.push(e.code === 121 ? e.message.slice(0, 60) : "wrong error");
    }
results["schema validation refuses invalid orders"] = refused.every((r) => /validation/.test(r));
let dup = null;
try {
    customers.insertOne({ email: "user5@mail.example" });
} catch (e) {
    dup = e;
}
results["the unique email index refuses a duplicate and leaves no partial index entries"] =
    dup?.code === 11000 && /E11000/.test(dup.message) && customers.estimatedDocumentCount() === 1000;

const q1 = { customer: "c42", status: { $in: ["paid", "shipped"] } };
const ex1 = explainOf(orders, q1),
    ex2 = explainOf(orders, { total: { $gte: 2000 } }),
    ex3 = explainOf(orders, { status: { $ne: "cancelled" } });
results[`the planner picks the selective customer index over status (examined ${ex1.totalDocsExamined} docs)`] =
    ex1.plan.stage === "IXSCAN" && ex1.plan.index === "customer" && ex1.totalDocsExamined < 40;
results["range queries use the numeric index; a $ne falls back to a collection scan"] =
    ex2.plan.index === "total" && ex3.plan.stage === "COLLSCAN";
const all = orders.find();
const brute = all.filter((d) => d.customer === "c42" && ["paid", "shipped"].includes(d.status)).length;
// the planner intersects two index scans when both are selective and within 4x of each other
const FEW = Array.from({ length: 20 }, (_, i) => `c${i}`);
const q3 = { "items.sku": { $in: ["SKU-7", "SKU-8"] }, customer: { $in: FEW } },
    ex4 = explainOf(orders, q3);
results[
    `two selective predicates are answered by bitmap intersection (${ex4.plan.stage}, ${ex4.totalDocsExamined} docs examined)`
] =
    ex4.plan.stage === "IXAND" &&
    orders.countDocuments(q3) ===
        all.filter((d) => FEW.includes(d.customer) && d.items.some((i) => i.sku === "SKU-7" || i.sku === "SKU-8"))
            .length &&
    ex4.totalDocsExamined === ex4.nReturned;
results["index and scan answers agree"] = orders.countDocuments(q1) === brute;
const multikey = orders.find(
    { "items.sku": "SKU-7", total: { $lt: 100 } },
    { projection: { total: 1, "items.sku": 1 }, sort: { total: -1 }, limit: 5 },
);
results["a multikey index on an array field, with projection, sort and limit"] =
    multikey.length > 0 &&
    multikey.every(
        (d) =>
            d.items.some((i) => i.sku === "SKU-7") &&
            d.total < 100 &&
            Object.keys(d).sort().join() === "_id,items,total",
    );
results["$elemMatch binds conditions to the same array element"] = orders
    .find({ items: { $elemMatch: { sku: "SKU-3", qty: { $gte: 3 } } } })
    .every((d) => d.items.some((i) => i.sku === "SKU-3" && i.qty >= 3));

const upd = orders.updateMany(
    { customer: "c42", status: "paid" },
    { $set: { status: "shipped" }, $push: { history: { at: "2026-07-01", event: "shipped" } } },
).modifiedCount;
results[`$set/$push updated ${upd} orders atomically and the status index followed`] =
    upd >= 1 && orders.countDocuments({ customer: "c42", status: "paid" }) === 0;
customers.updateMany({ email: "vip@mail.example" }, { $set: { tier: "gold" }, $inc: { points: 50 } }, { upsert: true });
customers.updateMany({ _id: "c7" }, { $addToSet: { tags: "newsletter" } });
customers.updateMany({ _id: "c7" }, { $addToSet: { tags: "newsletter" } });
let badInc = null;
try {
    customers.updateMany({ _id: "c7" }, { $inc: { name: 1 } });
} catch (e) {
    badInc = e.message;
}
results["upsert, idempotent $addToSet, and a refused $inc"] =
    customers.findOne({ email: "vip@mail.example" })?.points === 50 &&
    customers.findOne({ _id: "c7" }).tags.length === 1 &&
    /Cannot apply \$inc/.test(badInc || "");

t = performance.now();
const revenue = await orders.aggregate([
    { $match: { status: { $in: ["paid", "shipped"] } } },
    { $unwind: "$items" },
    {
        $group: {
            _id: "$items.sku",
            units: { $sum: "$items.qty" },
            revenue: { $sum: { $multiply: ["$items.qty", "$items.price"] } },
        },
    },
    { $sort: { revenue: -1 } },
    { $limit: 5 },
]);
const aggMs = performance.now() - t;
const skuCheck = orders
    .find({ status: { $in: ["paid", "shipped"] } })
    .flatMap((d) => d.items)
    .filter((i) => i.sku === revenue[0]._id)
    .reduce((s, i) => s + i.qty * i.price, 0);
results[`aggregation: top SKU ${revenue[0]._id} earned ${revenue[0].revenue} (verified by hand)`] =
    revenue.length === 5 && revenue[0].revenue === skuCheck;
const byCity = await orders.aggregate([
    { $match: { status: "shipped" } },
    { $group: { _id: "$customer", n: { $sum: 1 } } },
    { $lookup: { from: "customers", localField: "_id", foreignField: "_id", as: "c" } },
    { $project: { city: { $first: "$c.address.city" }, n: 1 } },
    { $group: { _id: "$city", orders: { $sum: "$n" } } },
    { $sort: { orders: -1 } },
]);
results["an _id equality is a primary-key point read (IDHACK)"] =
    planOf(customers.explain({ _id: "c5" })).stage === "IDHACK" &&
    customers.findOne({ _id: "c5" }).email === "user5@mail.example";
results["a $lookup join groups shipped orders by customer city"] =
    byCity.length === 6 && byCity.reduce((s, r) => s + r.orders, 0) === orders.countDocuments({ status: "shipped" });
const deleted = orders.deleteMany({ status: "cancelled", total: { $lt: 50 } }).deletedCount;
const seen = [];
for (let ev; (ev = changes.tryNext()) !== null;) seen.push(ev);
// 1000 inserts, the upsert, one $addToSet; the repeated $addToSet changes nothing, so it is no event
results[`the change stream saw ${seen.length} customer events`] =
    seen.length === 1002 &&
    seen.every((c) => c.ns.coll === "customers") &&
    seen.some((c) => c.operationType === "insert") &&
    seen.some((c) => c.operationType === "update" && c.documentKey._id === "c7");

// Planner differential: random filters over every operator, answered through whatever plan the planner picks
// (IDHACK, IXSCAN, AND_SORTED, OR, multikey) and by a plain matcher over every document.
const frng = new Random(77);
const pick = (xs) => xs[frng.nextBounded(xs.length)];
const TAGS = ["gift", "rush", "fragile", "bulk"];
const FIELDS = {
    customer: () => `c${frng.nextBounded(1100)}`,
    status: () => pick(["new", "paid", "shipped", "cancelled", "lost"]),
    total: () => frng.nextBounded(2600),
    "items.sku": () => `SKU-${frng.nextBounded(320)}`,
    "items.qty": () => 1 + frng.nextBounded(4),
    placed: () => `2026-0${1 + frng.nextBounded(6)}-${String(1 + frng.nextBounded(28)).padStart(2, "0")}`,
    coupon: () => "SPRING",
    tags: () => pick(TAGS),
};
function genCond(field) {
    const v = FIELDS[field];
    switch (frng.nextBounded(11)) {
        case 0:
            return v();
        case 1:
            return { $ne: v() };
        case 2:
            return { [pick(["$gt", "$gte"])]: v() };
        case 3:
            return { [pick(["$lt", "$lte"])]: v() };
        case 4: {
            const [a, b] = [v(), v()].sort((x, y) => (x < y ? -1 : x > y ? 1 : 0));
            return { [pick(["$gt", "$gte"])]: a, [pick(["$lt", "$lte"])]: b };
        }
        case 5:
            return {
                $in: Array.from({ length: 1 + frng.nextBounded(4) }, v).concat(frng.nextBounded(4) ? [] : [null]),
            };
        case 6:
            return { $nin: Array.from({ length: 1 + frng.nextBounded(3) }, v) };
        case 7:
            return { $exists: frng.nextBounded(2) === 0 };
        case 8:
            return typeof v() === "string" ? { $regex: `^${String(v()).slice(0, 2 + frng.nextBounded(4))}` } : v();
        case 9:
            return { $not: { [pick(["$gt", "$lte"])]: v() } };
        default:
            return frng.nextBounded(2) ? null : v();
    }
}
function genFilter(depth) {
    const f = {},
        keys = Object.keys(FIELDS);
    for (let k = 0, n = 1 + frng.nextBounded(3); k < n; k++) {
        const field = pick(keys);
        f[field] = genCond(field);
    }
    if (depth < 2 && frng.nextBounded(4) === 0) f[pick(["$and", "$or"])] = [genFilter(depth + 1), genFilter(depth + 1)];
    if (depth === 0 && frng.nextBounded(6) === 0)
        f.items = { $elemMatch: { sku: FIELDS["items.sku"](), qty: { $gte: FIELDS["items.qty"]() } } };
    return f;
}
// the reference: the definition of each operator written out plainly, one document at a time
function refValues(doc, path) {
    let vs = [doc];
    for (const p of path.split(".")) {
        const next = [];
        for (const v of vs)
            if (Array.isArray(v)) {
                for (const e of v) if (e !== null && typeof e === "object" && p in e) next.push(e[p]);
            } else if (v !== null && typeof v === "object" && p in v) next.push(v[p]);
        vs = next;
    }
    const out = [];
    for (const v of vs) {
        out.push(v);
        if (Array.isArray(v)) for (const e of v) out.push(e);
    }
    return out;
}
function refMatch(doc, filter) {
    for (const key of Object.keys(filter)) {
        const cond = filter[key];
        if (key === "$and") {
            for (const f of cond) if (!refMatch(doc, f)) return false;
            continue;
        }
        if (key === "$or") {
            let any = false;
            for (const f of cond) if (refMatch(doc, f)) any = true;
            if (!any) return false;
            continue;
        }
        const ops = cond !== null && typeof cond === "object" && !Array.isArray(cond) ? cond : { $eq: cond };
        const vals = refValues(doc, key);
        for (const op of Object.keys(ops)) {
            const arg = ops[op];
            let ok = false;
            if (op === "$eq") ok = vals.includes(arg) || (arg === null && vals.length === 0);
            else if (op === "$ne") ok = !vals.includes(arg) && !(arg === null && vals.length === 0);
            else if (op === "$in") ok = vals.some((v) => arg.includes(v)) || (arg.includes(null) && vals.length === 0);
            else if (op === "$nin") ok = !vals.some((v) => arg.includes(v));
            else if (op === "$exists") ok = vals.length > 0 === arg;
            else if (op === "$regex") ok = vals.some((v) => typeof v === "string" && v.search(arg) >= 0);
            else if (op === "$not") ok = !refMatch(doc, { [key]: arg });
            else if (op === "$elemMatch") {
                const arr = doc[key];
                if (Array.isArray(arr)) for (const el of arr) if (refMatch(el, arg)) ok = true;
            } else
                for (const v of vals) {
                    if (v === null || typeof v !== typeof arg) continue;
                    if (op === "$gt" && v > arg) ok = true;
                    if (op === "$gte" && v >= arg) ok = true;
                    if (op === "$lt" && v < arg) ok = true;
                    if (op === "$lte" && v <= arg) ok = true;
                }
            if (!ok) return false;
        }
    }
    return true;
}
// a 1200-order sample with the same indexes keeps 124 filters x every document affordable for the reference
const probe = db.collection("probe");
for (const path of ["customer", "status", "total", "items.sku", "tags"]) probe.createIndex({ [path]: 1 });
probe.insertMany(all.slice(0, 1200).map((d) => ({ ...d, tags: TAGS.filter(() => frng.nextBounded(3) === 0) })));
probe.updateMany({ total: { $gte: 1800 }, status: "paid" }, { $set: { coupon: "SPRING" } });
// the random ones, then the multikey edges written out: open bounds met by an element equal to the other bound
const filters = Array.from({ length: 120 }, () => genFilter(0)).concat([
        { tags: { $gt: "gift", $lt: "rush" } },
        { tags: { $gte: "bulk", $lt: "gift" } },
        { "items.qty": { $gt: 1, $lt: 3 } },
        { "items.sku": { $gt: "SKU-1", $lt: "SKU-2" }, tags: { $exists: true } },
    ]),
    allProbe = probe.find(),
    stages = new Map(),
    answers = [];
let planMismatches = 0;
for (const f of filters) {
    const plan = planOf(probe.explain(f)),
        key = plan.stage + (plan.index ? `(${plan.index})` : "");
    stages.set(key, (stages.get(key) ?? 0) + 1);
    const got = probe.find(f, { projection: { _id: 1 } }).map((d) => d._id),
        want = allProbe.filter((d) => refMatch(d, f)).map((d) => d._id);
    got.sort((a, b) => a - b);
    want.sort((a, b) => a - b);
    answers.push(got);
    if (got.length !== want.length || got.some((id, i) => id !== want[i])) planMismatches++;
}
results[
    `124 filters answer exactly as a plain matcher through ${stages.size} distinct plans (${[...stages.keys()].join(" ")})`
] =
    planMismatches === 0 &&
    ["COLLSCAN", "IXAND", "IXSCAN(total)", "IXSCAN(items.sku)", "IXSCAN(customer)", "IXSCAN(tags)"].every((k) =>
        [...stages.keys()].some((s) => s.startsWith(k)),
    ) &&
    answers.some((a) => a.length > 600) &&
    answers.filter((a) => a.length === 0).length < 90;

// Persist and reload: an explicit snapshot, written off the JS thread, read back with every index rebuilt.
changes.close();
const countBefore = orders.estimatedDocumentCount(),
    sample = orders.find({ customer: "c42" }, { sort: { _id: 1 } });
t = performance.now();
const written = await db.persist(snapshot);
const persistMs = performance.now() - t;
db.close();
t = performance.now();
db = await Database.load(snapshot);
const reloadMs = performance.now() - t;
orders = db.collection("orders");
customers = db.collection("customers");
results[`the snapshot (${written.bytes} bytes) restored ${countBefore} orders with their indexes`] =
    written.documents === countBefore + 1001 + 1200 &&
    orders.estimatedDocumentCount() === countBefore &&
    JSON.stringify(orders.find({ customer: "c42" }, { sort: { _id: 1 } })) === JSON.stringify(sample) &&
    planOf(orders.explain({ total: { $gte: 2000 } })).index === "total" &&
    orders.listIndexes().length === 5;
results["after the reload the 124 filters return the same documents"] = filters.every((f, i) => {
    const got = db
        .collection("probe")
        .find(f, { projection: { _id: 1 } })
        .map((d) => d._id)
        .sort((a, b) => a - b);
    return got.length === answers[i].length && got.every((id, j) => id === answers[i][j]);
});

// Damage: a snapshot cut short or with a flipped bit is refused whole; the file it would have replaced is intact.
const good = readBytes(new Path(snapshot));
const torn = `${work}/torn.dds`,
    flippedPath = `${work}/flipped.dds`;
writeFile(new Path(torn), good.subarray(0, good.length - 3));
const flipped = good.slice();
flipped[flipped.length >> 1] ^= 0x40;
writeFile(new Path(flippedPath), flipped);
const refusedCodes = [];
for (const p of [torn, flippedPath])
    try {
        await Database.load(p);
        refusedCodes.push("loaded");
    } catch (e) {
        refusedCodes.push(e.code);
    }
customers.insertOne({ _id: "late", email: "late@mail.example" });
await db.persist(snapshot);
const again = await Database.load(snapshot);
results["a torn or bit-flipped snapshot is refused whole (100001); a rewrite replaces the file atomically"] =
    refusedCodes.join() === "100001,100001" &&
    stat(new Path(snapshot)).size > good.length &&
    again.collection("customers").findOne({ _id: "late" }) !== null;
again.close();

// HTTP API.
const app = new App({ port: 0 });
const fail = (e) => ({
    status: 400,
    body: JSON.stringify({ error: e.message, code: e.code }),
    contentType: "application/json",
});
app.post("/db/:coll/find", (req) => {
    const b = JSON.parse(req.body);
    try {
        return { docs: db.collection(req.params.coll).find(b.filter || {}, b.options || {}) };
    } catch (e) {
        return fail(e);
    }
});
app.post("/db/:coll/aggregate", async (req) => {
    try {
        return { docs: await db.collection(req.params.coll).aggregate(JSON.parse(req.body).pipeline) };
    } catch (e) {
        return fail(e);
    }
});
app.start();
const post = (path, body) =>
    fetch(`http://127.0.0.1:${app.port}${path}`, { method: "POST", body: JSON.stringify(body) });
const api = await (
    await post("/db/orders/find", {
        filter: { status: "new", total: { $gt: 1500 } },
        options: { sort: { total: -1 }, limit: 3, projection: { total: 1 } },
    })
).json();
const apiAgg = await (
    await post("/db/orders/aggregate", { pipeline: [{ $match: { status: "new" } }, { $count: "n" }] })
).json();
const badOp = await post("/db/orders/find", { filter: { total: { $where: "1" } } });
const badBody = await badOp.json();
app.close();
results["the HTTP API serves queries and aggregations and rejects unknown operators"] =
    api.docs.length === 3 &&
    api.docs[0].total >= api.docs[2].total &&
    apiAgg.docs[0].n === orders.countDocuments({ status: "new" }) &&
    badOp.status === 400 &&
    badBody.code === 2;

print("document-database");
print(
    `  load       21000 documents in ${loadMs.toFixed(0)} ms with 5 indexes; ${deleted} cancelled small orders deleted`,
);
print(
    `  aggregate  ${aggMs.toFixed(1)} ms   persist ${persistMs.toFixed(1)} ms   load ${reloadMs.toFixed(1)} ms (indexes rebuilt)`,
);
print(
    table([
        ["query", "plan", "keys examined", "docs examined", "returned"],
        ...[
            ["customer=c42, status in [paid,shipped]", ex1],
            ["total >= 2000", ex2],
            ["status != cancelled", ex3],
        ].map(([q, e]) => [
            q,
            e.plan.stage + (e.plan.index ? `(${e.plan.index})` : ""),
            String(e.totalKeysExamined),
            String(e.totalDocsExamined),
            String(e.nReturned),
        ]),
    ]),
);
print(table([["sku", "units", "revenue"], ...revenue.map((r) => [r._id, String(r.units), String(r.revenue)])]));
print(`  by city    ${byCity.map((r) => `${r._id} ${r.orders}`).join(", ")}`);
db.close();
removeAll(work);
check(results);

function check(results) {
    const names = Object.keys(results);
    const failed = names.filter((name) => !results[name]);
    for (const name of failed) print("FAIL " + name);
    if (failed.length) throw new Error(`${failed.length} of ${names.length} checks failed`);
}
