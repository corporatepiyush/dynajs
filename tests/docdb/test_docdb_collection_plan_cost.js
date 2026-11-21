// covers: integration dyna:docdb.Collection.find dyna:docdb.Collection.explain dyna:docdb.Collection.aggregate dyna:docdb.Collection.countDocuments -- one plan ranking serves find, explain, aggregate and countDocuments
// Plan ranking and the fixed cost of an indexed query.
// Ranking: each usable index is costed as (estimated documents) + 2 x (index keys its intervals span), ties going to
// more bound fields and then to fewer key fields; of two indexes bound on the same leading fields (same document set)
// the one spanning fewer keys wins outright. Scanning K keys ORs their postings in one k-way pass (dyn_roar_or_many),
// not K copy-on-write unions, so a key costs about two fetched documents. A compound index used only through its leading field spans one key
// per distinct trailing value, so with {status:1} and {status:1, customer:1} a find on status alone must take the
// single-field index (1 key) and not the compound prefix (one key per customer): before this rule both had the same
// document estimate and the compound index won, examining ~5000 keys and running ~3x slower.
// Covered count: an aggregate whose leading $match (after an optional $sort/$skip/$limit) is answered exactly by one
// index and is followed by $count, or by $group with _id null whose accumulators are all $sum of an int32 literal,
// takes the count from the index like countDocuments instead of fetching every match.
// Fixed cost: a point interval that binds every field of an index is looked up in the index's key hash instead of two
// binary searches, the planner only touches as many field slots as the index has, and countDocuments plans once.
// Every answer here must equal an unindexed twin collection's.
import { suite } from "../lib/kit.js";
import { perfCheck } from "../lib/perf.js";
import { Database } from "dyna:docdb";
import { Random } from "dyna:random";
import { bestMs, cpuNow } from "../lib/cpuclock.js";

const t = suite("docdb.Collection plan cost: index ranking, covered aggregate count, fixed query cost");
const N = 60000, NC = 3000;
const rng = new Random(7070);
const STATUS = ["new", "paid", "paid", "shipped", "shipped", "shipped", "cancelled"];
const docs = Array.from({ length: N }, (_, i) => ({
    _id: i,
    customer: `c${rng.nextBounded(NC)}`,
    status: STATUS[rng.nextBounded(7)],
    total: rng.nextBounded(3000),
    tags: rng.nextBounded(4) ? [`t${rng.nextBounded(5)}`, `t${rng.nextBounded(5)}`] : `t${rng.nextBounded(5)}`,
    ...(rng.nextBounded(9) ? { v: rng.nextBounded(3) ? rng.nextBounded(40) : null } : {}),
}));
const db = new Database("plan-cost");
const both = db.collection("both");
const single = db.collection("single");
const plain = db.collection("plain");
for (const c of [both, single, plain]) c.insertMany(docs);
both.createIndex({ status: 1 });
both.createIndex({ status: 1, customer: 1 });
both.createIndex({ customer: 1 });
both.createIndex({ total: -1 });
both.createIndex({ tags: 1 });
both.createIndex({ v: 1, total: 1 });
single.createIndex({ status: 1 });

const J = (x) => JSON.stringify(x);
const ids = (rows) => rows.map((d) => d._id).sort((x, y) => x - y);
const plan = (c, f) => {
    const e = c.explain(f);
    const w = e.queryPlanner.winningPlan;
    return { stage: w.stage, ix: w.inputStage?.indexName, keys: e.executionStats.totalKeysExamined, docs: e.executionStats.totalDocsExamined, n: e.executionStats.nReturned };
};

t.test("an equality on the leading field takes the single-field index, not the compound prefix", (a) => {
    const p = plan(both, { status: "shipped" });
    a.eq(p.ix, "status_1", J(p));
    a.eq(p.keys, 1, "one index key examined");
    a.eq(p.n, plain.countDocuments({ status: "shipped" }));
    const q = plan(both, { status: { $in: ["new", "paid"] } });
    a.eq(q.ix, "status_1", J(q));
    a.eq(q.keys, 2);
});

t.test("a skewed compound prefix whose sampled estimate is low still loses to the exact single-field index", (a) => {
    const c = db.collection("skew");
    c.createIndex({ g: 1, h: 1 });
    c.createIndex({ g: 1 });
    const rows = [];
    for (let i = 0; i < 100; i++) rows.push({ g: "s", h: `h${String(i).padStart(3, "0")}` });
    for (let i = 0; i < 40; i++) for (let k = 0; k < 200; k++) rows.push({ g: "s", h: `z${i}` });
    for (let i = 0; i < 500; i++) rows.push({ g: "o", h: `h${i}` });
    c.insertMany(rows);
    const p = plan(c, { g: "s" });
    a.eq(p.ix, "g_1", J(p));
    a.eq(p.keys, 1);
    a.eq(p.n, 8100);
    a.eq(plan(c, { g: "s", h: "z3" }).ix, "g_1_h_1");
});

t.test("a range spanning more keys than a small point's documents loses to the point", (a) => {
    const c = db.collection("keycost");
    c.createIndex({ k: 1 });
    c.createIndex({ s: 1 });
    c.insertMany(Array.from({ length: 40000 }, (_, i) => ({ _id: i, k: i, s: i % 800 === 0 ? "a" : "b" })));
    const wide = plan(c, { k: { $gte: 1000, $lt: 1040 }, s: "a" });
    a.eq(wide.ix, "s_1", `40 keys/40 docs (cost 120) vs 1 key/50 docs (cost 52): ${J(wide)}`);
    a.eq(wide.keys, 1);
    const narrow = plan(c, { k: { $gte: 1000, $lt: 1010 }, s: "a" });
    a.eq(narrow.ix, "k_1", `10 keys/10 docs (cost 30) vs 1 key/50 docs (cost 52): ${J(narrow)}`);
    a.eq(J(ids(c.find({ k: { $gte: 1590, $lt: 1630 }, s: "a" }))), J([1600]));
});

t.test("a range over many keys ORs postings in one pass, across 65536-document chunks (results + CPU ratio)", (a) => {
    const c = db.collection("wide");
    c.createIndex({ k: 1 });
    c.createIndex({ m: 1 });
    c.createIndex({ g: 1 });
    c.insertMany(Array.from({ length: 140000 }, (_, i) => ({ _id: i, k: i % 70001, m: [i % 5, (i * 7) % 11], g: i % 8 })));
    const f = { k: { $gte: 60000, $lt: 70001 } };
    const got = c.find(f, { projection: { _id: 1 } }).map((d) => d._id);
    const want = [];
    for (let i = 0; i < 140000; i++) if (i % 70001 >= 60000) want.push(i);
    a.eq(got.length, want.length);
    a.eq(J(got.sort((x, y) => x - y)), J(want));
    a.eq(c.countDocuments({ m: { $gte: 3, $lte: 9 } }), Array.from({ length: 140000 }, (_, i) => [i % 5, (i * 7) % 11]).filter((m) => m.some((v) => v >= 3) && m.some((v) => v <= 9)).length, "array: each bound met by some element");
    a.eq(c.find({ m: { $in: [0, 1, 2, 4, 6, 8, 10] }, g: 3 }).length, Array.from({ length: 140000 }, (_, i) => i).filter((i) => i % 8 === 3 && [i % 5, (i * 7) % 11].some((v) => [0, 1, 2, 4, 6, 8, 10].includes(v))).length);
    const rangeT = bestMs(() => c.find({ k: { $gte: 1000, $lt: 9000 } }, { projection: { _id: 1 } }), 5);
    const pointT = bestMs(() => c.find({ g: 5 }, { projection: { _id: 1 } }), 5);
    a.eq(rangeT.value.length, 16000);
    a.eq(pointT.value.length, 17500);
    a.note(`range/point ${(rangeT.ms / pointT.ms).toFixed(2)}`);
    perfCheck(a.ok, rangeT.ms < pointT.ms * 4, `8000-key range ${rangeT.ms.toFixed(2)} ms vs 1-key point ${pointT.ms.toFixed(2)} ms`);
});

t.test("a predicate on both fields still takes the compound index", (a) => {
    const p = plan(both, { status: "new", customer: "c17" });
    a.eq(p.ix, "status_1_customer_1", J(p));
    a.eq(p.keys, 1);
    a.eq(p.docs, p.n, "every fetched document matches");
    const q = plan(both, { status: "paid", customer: { $in: ["c1", "c2", "c3"] } });
    a.eq(q.ix, "status_1_customer_1", J(q));
    a.eq(q.docs, q.n);
});

t.test("a compound prefix still wins when it is the only index on the field", (a) => {
    const p = plan(both, { v: 7 });
    a.eq(p.ix, "v_1_total_1", J(p));
    a.eq(p.n, plain.countDocuments({ v: 7 }));
});

const filters = [
    { status: "shipped" }, { status: "new", customer: "c5" }, { status: { $in: ["new", "cancelled"] }, customer: { $gte: "c2", $lt: "c3" } },
    { customer: "c9" }, { customer: { $in: ["c1", "c100", "nope"] } }, { total: 17 }, { total: { $gte: 100, $lt: 140 } },
    { total: { $in: [5, 6, 2999] }, status: "paid" }, { tags: "t3" }, { tags: { $in: ["t1", "t4"] }, status: "new" },
    { v: null }, { v: 3, total: { $lt: 1000 } }, { v: { $in: [1, null] }, total: 50 }, { v: 4, total: 2 }, { status: "zz" },
    { status: "paid", customer: "c3", total: { $gt: 1500 } }, { $or: [{ status: "new", customer: "c1" }, { total: 9 }] },
];

t.test("the single-field plan is not slower than without the compound index (CPU ratio)", (a) => {
    const f = { status: "shipped" };
    a.eq(both.find(f).length, single.find(f).length);
    const withCompound = bestMs(() => both.find(f, { projection: { customer: 1 } }), 5).ms;
    const alone = bestMs(() => single.find(f, { projection: { customer: 1 } }), 5).ms;
    a.note(`ratio ${(withCompound / alone).toFixed(2)}`);
    perfCheck(a.ok, withCompound < alone * 1.6, `with compound ${withCompound.toFixed(2)} ms vs single only ${alone.toFixed(2)} ms`);
});

t.test("every filter returns exactly the unindexed twin's documents, before and after churn", (a) => {
    const check = (label) => {
        for (const f of filters) {
            a.eq(J(ids(both.find(f))), J(ids(plain.find(f))), `${label} find ${J(f)}`);
            a.eq(both.countDocuments(f), plain.countDocuments(f), `${label} count ${J(f)}`);
        }
    };
    check("fresh");
    for (const c of [both, plain]) {
        c.deleteMany({ customer: { $in: ["c5", "c9", "c1"] } });
        c.updateMany({ total: { $lt: 30 } }, { $set: { status: "zz", v: 4 } });
        c.insertMany([{ _id: "x1", status: "new", customer: "c5", total: 2, v: 4 }, { _id: "x2", tags: ["t3"], v: null }]);
    }
    check("churned");
});

const pipes = [
    [{ $match: { status: "shipped" } }, { $count: "n" }],
    [{ $match: { status: "zz" } }, { $count: "n" }],
    [{ $match: { customer: { $in: ["c2", "c3"] } } }, { $count: "hits" }],
    [{ $match: { total: { $gte: 100, $lt: 900 } } }, { $count: "n" }, { $project: { n: 1, twice: { $multiply: ["$n", 2] } } }],
    [{ $match: { status: "paid" } }, { $group: { _id: null, n: { $sum: 1 } } }],
    [{ $match: { status: "paid" } }, { $group: { _id: null, n: { $sum: 1 }, k: { $count: {} }, m: { $sum: -3 }, z: { $sum: 0 } } }],
    [{ $match: { status: "paid" } }, { $group: { _id: null, big: { $sum: 2147483647 } } }],
    [{ $match: { status: "nope" } }, { $group: { _id: null, n: { $sum: 1 } } }],
    [{ $match: { status: "new" } }, { $skip: 10 }, { $limit: 500 }, { $count: "n" }],
    [{ $match: { status: "new" } }, { $sort: { total: 1 } }, { $skip: 1000000 }, { $count: "n" }],
    [{ $match: { status: "new" } }, { $limit: 3 }, { $group: { _id: null, n: { $sum: 1 } } }, { $addFields: { k: "x" } }],
    [{ $match: { tags: "t2" } }, { $count: "n" }],
    [{ $match: { status: "new", customer: "c4" } }, { $count: "n" }],
    [{ $match: { status: "paid" } }, { $group: { _id: null, s: { $sum: "$total" } } }],
    [{ $match: { status: "paid" } }, { $group: { _id: "$status", n: { $sum: 1 } } }],
    [{ $match: { status: "paid" } }, { $group: { _id: null, n: { $sum: 1.5 } } }],
    [{ $match: { v: null } }, { $count: "n" }],
    [{ $count: "all" }],
];

t.test("covered aggregate counts equal the unindexed twin's for every pipeline shape", async (a) => {
    for (const p of pipes) a.eq(J(await both.aggregate(p)), J(await plain.aggregate(p)), J(p));
});

async function cpuAsync(fn, reps) {
    let best = Infinity;
    for (let r = 0; r < reps; r++) {
        const t0 = cpuNow();
        await fn();
        best = Math.min(best, cpuNow() - t0);
    }
    return best;
}

t.test("[$match, $count] on an index costs a small fraction of materialising the matches (CPU ratio)", async (a) => {
    const M = { $match: { status: "shipped" } };
    const count = await cpuAsync(async () => { for (let i = 0; i < 10; i++) await single.aggregate([M, { $count: "n" }]); }, 5) / 10;
    const group = await cpuAsync(async () => { for (let i = 0; i < 10; i++) await single.aggregate([M, { $group: { _id: null, n: { $sum: 1 } } }]); }, 5) / 10;
    const ids = bestMs(() => single.find({ status: "shipped" }, { projection: { _id: 1 } }), 5).ms;
    a.note(`count/find ${(count / ids).toFixed(3)} group/find ${(group / ids).toFixed(3)}`);
    a.ok(count < ids / 8, `$count ${count.toFixed(3)} ms vs find _id ${ids.toFixed(3)} ms`);
    a.ok(group < ids / 8, `$group null ${group.toFixed(3)} ms vs find _id ${ids.toFixed(3)} ms`);
});

t.test("an exact point lookup counts one key, also after its documents are gone", (a) => {
    const c = db.collection("pts");
    c.createIndex({ k: 1 });
    c.createIndex({ a: 1, b: -1 });
    c.insertMany(Array.from({ length: 300 }, (_, i) => ({ _id: i, k: `k${i % 50}`, a: i % 7, b: i % 11 })));
    const e = c.explain({ k: "k3" }).executionStats;
    a.eq(e.totalKeysExamined, 1);
    a.eq(e.nReturned, 6);
    a.eq(c.explain({ a: 3, b: 4 }).executionStats.nReturned, c.find({ a: 3, b: 4 }).length);
    a.eq(c.explain({ a: 3, b: 4 }).executionStats.totalKeysExamined, 1);
    a.eq(c.explain({ k: "missing" }).executionStats.totalKeysExamined, 0);
    c.deleteMany({ k: "k3" });
    a.eq(c.find({ k: "k3" }).length, 0);
    a.eq(c.countDocuments({ k: "k3" }), 0);
    c.insertOne({ _id: "back", k: "k3" });
    a.eq(J(c.find({ k: "k3" })), J([{ _id: "back", k: "k3" }]));
    a.eq(c.countDocuments({ a: { $in: [3, 4] }, b: { $in: [4, 5] } }), c.find({}).filter((d) => [3, 4].includes(d.a) && [4, 5].includes(d.b)).length);
});

t.test("the fixed cost of an indexed miss on a 10-index collection stays near an _id miss (CPU ratio)", (a) => {
    const c = db.collection("fixed");
    for (let j = 0; j < 9; j++) c.createIndex({ [`f${j}`]: 1 });
    c.createIndex({ k: 1 });
    c.insertMany(Array.from({ length: 50000 }, (_, i) => {
        const d = { _id: i, k: `customer-account-${String(i * 7).padStart(9, "0")}` };
        for (let j = 0; j < 9; j++) d[`f${j}`] = i % (j + 2);
        return d;
    }));
    const R = 3000;
    const missT = bestMs(() => { let n = 0; for (let i = 0; i < R; i++) n += c.find({ k: `customer-account-${String(i * 7 + 3).padStart(9, "0")}` }).length; return n; }, 5);
    const byId = bestMs(() => { let n = 0; for (let i = 0; i < R; i++) n += c.find({ _id: -1 - i }).length; return n; }, 5);
    a.eq(missT.value + byId.value, 0);
    a.eq(c.find({ k: `customer-account-${String(7 * 123).padStart(9, "0")}` }).map((d) => d._id).join(), "123");
    a.note(`miss/id ${(missT.ms / byId.ms).toFixed(2)}`);
    perfCheck(a.ok, missT.ms < byId.ms * 2.1, `indexed miss ${(missT.ms * 1000 / R).toFixed(2)} us vs _id miss ${(byId.ms * 1000 / R).toFixed(2)} us`);
});

await t.run();
db.close();
