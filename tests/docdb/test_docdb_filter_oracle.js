// covers: integration dyna:docdb.Collection.find dyna:docdb.Collection.createIndex dyna:docdb.Collection.explain dyna:docdb.Database.persist dyna:docdb.Database.load -- the 124-filter differential oracle through every plan and a snapshot round trip
// Port of examples/systems/document-database.js's planner differential: 120 random filters over every operator
// ($and/$or nesting, $elemMatch, $not, $regex, $in with null) plus the 4 multikey edge filters, over 1200 orders.
// Each filter is answered (1) by a plain one-document-at-a-time reference written from the MongoDB manual's operator
// definitions, (2) by the collection with no secondary index (COLLSCAN/IDHACK), (3) with single-field indexes,
// (4) with compound and descending indexes added, and (5) by a Database loaded back from a persisted snapshot. All
// five must return the same _id set, and the plans seen must cover IXSCAN, AND_SORTED, OR and COLLSCAN.
// Reference deviations from the example's own matcher, each a MongoDB rule: {f: null} and {f: {$in: [..., null]}}
// also match a document where f is missing (manual: "Query for Null or Missing Fields").
import { suite } from "../lib/kit.js";
import { Database, Collection } from "dyna:docdb";
import { Random } from "dyna:random";
import { makeTempDir, removeAll } from "dyna:file";

const t = suite("docdb.filter-oracle");
const rng = new Random(77);
const pick = (xs) => xs[rng.nextBounded(xs.length)];
const TAGS = ["gift", "rush", "fragile", "bulk"];
const STATUS = ["new", "paid", "shipped", "cancelled", "lost"];

function makeOrders(n) {
    const out = [];
    for (let i = 0; i < n; i++) {
        const items = Array.from({ length: 1 + rng.nextBounded(3) }, () => ({ sku: `SKU-${rng.nextBounded(320)}`, qty: 1 + rng.nextBounded(4) }));
        const d = {
            _id: `o${i}`,
            customer: `c${rng.nextBounded(1100)}`,
            status: pick(STATUS),
            total: rng.nextBounded(2600),
            items,
            placed: `2026-0${1 + rng.nextBounded(6)}-${String(1 + rng.nextBounded(28)).padStart(2, "0")}`,
            tags: TAGS.filter(() => rng.nextBounded(3) === 0),
        };
        if (d.total >= 1800 && d.status === "paid") d.coupon = "SPRING";
        if (i % 97 === 0) delete d.status;
        out.push(d);
    }
    return out;
}

const FIELDS = {
    customer: () => `c${rng.nextBounded(1100)}`,
    status: () => pick(STATUS),
    total: () => rng.nextBounded(2600),
    "items.sku": () => `SKU-${rng.nextBounded(320)}`,
    "items.qty": () => 1 + rng.nextBounded(4),
    placed: () => `2026-0${1 + rng.nextBounded(6)}-${String(1 + rng.nextBounded(28)).padStart(2, "0")}`,
    coupon: () => "SPRING",
    tags: () => pick(TAGS),
};
function genCond(field) {
    const v = FIELDS[field];
    switch (rng.nextBounded(11)) {
        case 0: return v();
        case 1: return { $ne: v() };
        case 2: return { [pick(["$gt", "$gte"])]: v() };
        case 3: return { [pick(["$lt", "$lte"])]: v() };
        case 4: {
            const [a, b] = [v(), v()].sort((x, y) => (x < y ? -1 : x > y ? 1 : 0));
            return { [pick(["$gt", "$gte"])]: a, [pick(["$lt", "$lte"])]: b };
        }
        case 5: return { $in: Array.from({ length: 1 + rng.nextBounded(4) }, v).concat(rng.nextBounded(4) ? [] : [null]) };
        case 6: return { $nin: Array.from({ length: 1 + rng.nextBounded(3) }, v) };
        case 7: return { $exists: rng.nextBounded(2) === 0 };
        case 8: return typeof v() === "string" ? { $regex: `^${String(v()).slice(0, 2 + rng.nextBounded(4))}` } : v();
        case 9: return { $not: { [pick(["$gt", "$lte"])]: v() } };
        default: return rng.nextBounded(2) ? null : v();
    }
}
function genFilter(depth) {
    const f = {}, keys = Object.keys(FIELDS);
    for (let k = 0, n = 1 + rng.nextBounded(3); k < n; k++) {
        const field = pick(keys);
        f[field] = genCond(field);
    }
    if (depth < 2 && rng.nextBounded(4) === 0) f[pick(["$and", "$or"])] = [genFilter(depth + 1), genFilter(depth + 1)];
    if (depth === 0 && rng.nextBounded(6) === 0)
        f.items = { $elemMatch: { sku: FIELDS["items.sku"](), qty: { $gte: FIELDS["items.qty"]() } } };
    return f;
}

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
            if (!cond.every((f) => refMatch(doc, f))) return false;
            continue;
        }
        if (key === "$or") {
            if (!cond.some((f) => refMatch(doc, f))) return false;
            continue;
        }
        const ops = cond !== null && typeof cond === "object" && !Array.isArray(cond) ? cond : { $eq: cond };
        const vals = refValues(doc, key);
        for (const op of Object.keys(ops)) {
            const arg = ops[op];
            let ok = false;
            if (op === "$eq") ok = vals.includes(arg) || (arg === null && vals.length === 0);
            else if (op === "$ne") ok = !vals.includes(arg);
            else if (op === "$in") ok = vals.some((v) => arg.includes(v)) || (arg.includes(null) && vals.length === 0);
            else if (op === "$nin") ok = !vals.some((v) => arg.includes(v));
            else if (op === "$exists") ok = vals.length > 0 === arg;
            else if (op === "$regex") ok = vals.some((v) => typeof v === "string" && v.search(arg) >= 0);
            else if (op === "$not") ok = !refMatch(doc, { [key]: arg });
            else if (op === "$elemMatch") {
                const arr = doc[key];
                if (Array.isArray(arr)) for (const el of arr) if (el !== null && typeof el === "object" && refMatch(el, arg)) ok = true;
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

const ORDERS = makeOrders(1200);
const FILTERS = Array.from({ length: 120 }, () => genFilter(0)).concat([
    { tags: { $gt: "gift", $lt: "rush" } },
    { tags: { $gte: "bulk", $lt: "gift" } },
    { "items.qty": { $gt: 1, $lt: 3 } },
    { "items.sku": { $gt: "SKU-1", $lt: "SKU-2" }, tags: { $exists: true } },
]);
const ids = (docs) => docs.map((d) => d._id).sort();
const WANT = FILTERS.map((f) => ids(ORDERS.filter((d) => refMatch(d, f))));

function check(a, coll, label, stages) {
    let bad = 0;
    FILTERS.forEach((f, i) => {
        const got = ids(coll.find(f, { projection: { _id: 1 } }));
        if (stages) {
            const w = coll.explain(f).queryPlanner.winningPlan;
            const s = w.stage === "FETCH" ? w.inputStage.stage : w.stage;
            stages.set(s, (stages.get(s) ?? 0) + 1);
        }
        if (JSON.stringify(got) !== JSON.stringify(WANT[i])) {
            if (bad++ < 3) a.eq(got.length, WANT[i].length, `${label} filter ${i} ${JSON.stringify(f)}`);
        }
    });
    a.eq(bad, 0, `${label}: filters disagreeing with the reference`);
}

t.test("the reference is not trivial: matches vary and some filters match nothing / everything", (a) => {
    const sizes = WANT.map((w) => w.length);
    a.eq(FILTERS.length, 124);
    a.ok(sizes.some((n) => n === 0) && sizes.some((n) => n > 600) && sizes.filter((n) => n > 0 && n < 100).length > 20);
});

const db = new Database("oracle");
const probe = db.collection("probe");
probe.insertMany(ORDERS);

t.test("no secondary index: COLLSCAN answers every filter like the reference", (a) => {
    a.ok(probe instanceof Collection);
    const stages = new Map();
    check(a, probe, "collscan", stages);
    a.eq([...stages.keys()].join(), "COLLSCAN");
});

t.test("single-field indexes (multikey on items.* and tags)", (a) => {
    for (const f of ["customer", "status", "total", "items.sku", "items.qty", "placed", "tags", "coupon"]) probe.createIndex({ [f]: 1 });
    const stages = new Map();
    check(a, probe, "single", stages);
    for (const s of ["IXSCAN", "AND_SORTED", "COLLSCAN"]) a.ok(stages.has(s), `plan ${s} exercised: ${JSON.stringify([...stages])}`);
    const f = { $or: [{ customer: "c17" }, { total: { $lt: 40 } }, { tags: "gift", placed: "2026-01-05" }] };
    a.eq(probe.explain(f).queryPlanner.winningPlan.inputStage.stage, "OR");
    a.eq(JSON.stringify(ids(probe.find(f))), JSON.stringify(ids(ORDERS.filter((d) => refMatch(d, f)))));
});

t.test("compound and descending indexes added", (a) => {
    probe.createIndex({ status: 1, total: -1 });
    probe.createIndex({ placed: -1, customer: 1 });
    probe.createIndex({ coupon: -1 }, { sparse: true, name: "coupon_sparse" });
    const stages = new Map();
    check(a, probe, "compound", stages);
    a.ok(stages.size >= 3, JSON.stringify([...stages]));
});

t.test("IXAND: two selective predicates on different indexed fields intersect", (a) => {
    for (const k of ["status", "total"]) probe.createIndex({ [k]: 1 });
    const f = { status: "paid", total: { $gte: 100, $lt: 1500 } };
    const w = probe.explain(f, {}).queryPlanner.winningPlan;
    const want = ids(ORDERS.filter((d) => refMatch(d, f)));
    a.eq(JSON.stringify(ids(probe.find(f))), JSON.stringify(want));
    a.ok(["IXSCAN", "AND_SORTED"].includes(w.inputStage.stage), JSON.stringify(w));
    const db2 = new Database("ixand");
    const c = db2.collection("c");
    c.insertMany(Array.from({ length: 4000 }, (_, i) => ({ _id: i, a: i % 40, b: i % 37 })));
    c.createIndex({ a: 1 });
    c.createIndex({ b: 1 });
    const e = c.explain({ a: { $lt: 10 }, b: { $lt: 9 } });
    a.eq(e.queryPlanner.winningPlan.inputStage.stage, "AND_SORTED");
    a.eq(e.executionStats.nReturned, c.find({}).filter((d) => d.a < 10 && d.b < 9).length);
    db2.close();
});

t.test("persist -> Database.load -> every filter again, and indexes rebuilt", async (a) => {
    const dir = makeTempDir("docdb-oracle");
    try {
        const res = await db.persist(dir + "/s.dds");
        a.eq(res.documents, 1200);
        const db2 = await Database.load(dir + "/s.dds");
        const c2 = db2.collection("probe");
        a.eq(c2.countDocuments(), 1200);
        a.eq(c2.listIndexes().length, probe.listIndexes().length);
        a.eq(JSON.stringify(c2.listIndexes()), JSON.stringify(probe.listIndexes()));
        check(a, c2, "loaded");
        db2.close();
    } finally {
        removeAll(dir);
    }
});

await t.run();
db.close();
