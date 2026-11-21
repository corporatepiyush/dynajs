// covers: integration dyna:docdb.Collection.aggregate dyna:docdb.Collection.createIndex -- 150 random pipelines against a plain JS reference across plans, thread modes and $group paths
// Each pipeline is drawn from $match / $group / $sort+$skip+$limit / $unwind / $project / $bucket shapes over 3000
// documents whose fields are sometimes missing, null or arrays ("w.x" also crosses arrays of documents). The reference below is written from the MongoDB
// manual's stage and accumulator definitions (group order = first appearance, missing groups as null, $push skips
// missing, $first of missing is null, $min/$max skip null). Every pipeline runs four ways and must match the reference
// exactly: no index vs an index on k (the leading $match goes through the planner), 3000 documents (offloaded run)
// vs a 400-document collection (inline run), and accumulator arguments as plain "$field" (the direct-read $group
// path) vs wrapped in {$cond: [true, "$field", 0]} (the general expression evaluator). Values are multiples of 0.25,
// so every sum is exact and compensated vs naive summation cannot differ.
import { suite } from "../lib/kit.js";
import { Database } from "dyna:docdb";

const t = suite("docdb.Collection.aggregate oracle");
let seed = 20261114;
const rnd = () => ((seed = (seed * 1103515245 + 12345) & 0x7fffffff) / 0x80000000);
const pick = (xs) => xs[Math.floor(rnd() * xs.length)];
const int = (n) => Math.floor(rnd() * n);

function mkdoc(i) {
    const d = { _id: i };
    const r = rnd();
    if (r < 0.9) d.k = int(10);
    else if (r < 0.95) d.k = null;
    const q = rnd();
    if (q < 0.85) d.v = int(400) / 4;
    else if (q < 0.9) d.v = null;
    else if (q < 0.93) d.v = "str";
    if (rnd() < 0.8) d.s = pick(["red", "green", "blue", "cyan"]);
    const tg = rnd();
    if (tg < 0.6) d.tags = Array.from({ length: int(4) }, () => pick(["p", "q", "r"]));
    else if (tg < 0.7) d.tags = pick(["p", "q"]);
    else if (tg < 0.75) d.tags = null;
    const w = rnd();
    if (w < 0.4) d.w = { x: int(3) };
    else if (w < 0.7) d.w = Array.from({ length: 1 + int(2) }, () => (rnd() < 0.8 ? { x: int(3) } : {}));
    return d;
}

const isNum = (x) => typeof x === "number";
const nullish = (x) => x === undefined || x === null;
const keyOf = (x) => (x === undefined ? "null" : JSON.stringify(x));
function cmpVal(a, b) {
    const rank = (x) => (nullish(x) ? 1 : isNum(x) ? 2 : typeof x === "string" ? 3 : 4);
    const ra = rank(a), rb = rank(b);
    if (ra !== rb) return ra - rb;
    if (ra === 1) return 0;
    return a < b ? -1 : a > b ? 1 : 0;
}

function get(d, f) {
    if (f !== "w.x") return d[f];
    if (Array.isArray(d.w)) return d.w.filter((e) => e.x !== undefined).map((e) => e.x);
    return d.w ? d.w.x : undefined;
}

function refGroup(rows, keyField, accs) {
    const groups = new Map();
    for (const d of rows) {
        const kv = get(d, keyField) === undefined ? null : get(d, keyField);
        const ks = keyOf(kv);
        let g = groups.get(ks);
        if (!g) groups.set(ks, (g = { _id: kv, st: accs.map(() => ({ sum: 0, n: 0, has: false, val: null, arr: [] })) }));
        accs.forEach(([name, op, f], i) => {
            const v = f === 1 ? 1 : get(d, f);
            const s = g.st[i];
            if (op === "$sum" || op === "$avg") {
                if (isNum(v)) {
                    s.sum += v;
                    s.n++;
                }
            } else if (op === "$min" || op === "$max") {
                if (!nullish(v) && (!s.has || (op === "$min" ? cmpVal(v, s.val) < 0 : cmpVal(v, s.val) > 0))) {
                    s.val = v;
                    s.has = true;
                }
            } else if (op === "$first") {
                if (!s.has) {
                    s.val = v === undefined ? null : v;
                    s.has = true;
                }
            } else if (op === "$last") {
                s.val = v === undefined ? null : v;
                s.has = true;
            } else if (op === "$push") {
                if (v !== undefined) s.arr.push(v);
            }
        });
    }
    return [...groups.values()].map((g) => {
        const out = { _id: g._id };
        accs.forEach(([name, op], i) => {
            const s = g.st[i];
            if (op === "$sum") out[name] = s.sum;
            else if (op === "$avg") out[name] = s.n ? s.sum / s.n : null;
            else if (op === "$push") out[name] = s.arr;
            else out[name] = s.has ? s.val : null;
        });
        return out;
    });
}

function refMatch(rows, f) {
    const [field, op, val] = f;
    return rows.filter((d) => {
        const x = d[field];
        const vals = Array.isArray(x) ? x : [x];
        if (op === "$eq") return vals.some((y) => (val === null ? nullish(y) : y === val));
        if (op === "$in") return vals.some((y) => val.includes(y));
        return vals.some((y) => isNum(y) && (op === "$gt" ? y > val : y < val));
    });
}

function refSort(rows) {
    return [...rows].sort((a, b) => cmpVal(a.v, b.v) || a._id - b._id);
}

function genPipeline() {
    const p = [], ref = [];
    if (rnd() < 0.6) {
        const f = pick([["k", "$eq", int(10)], ["k", "$in", [int(10), int(10)]], ["k", "$gt", int(10)], ["v", "$lt", int(100)], ["s", "$eq", pick(["red", "blue"])], ["tags", "$eq", "p"], ["k", "$eq", null]]);
        p.push({ $match: { [f[0]]: { [f[1]]: f[2] } } });
        ref.push((rows) => refMatch(rows, f));
    }
    const shape = int(5);
    if (shape === 0 || shape === 1) {
        const keyField = pick(["k", "s", "nope", "w.x"]);
        const accs = [];
        const n = 1 + int(3);
        for (let i = 0; i < n; i++) {
            const op = pick(["$sum", "$avg", "$min", "$max", "$first", "$last", "$push"]);
            accs.push(["a" + i, op, pick(op === "$min" || op === "$max" ? ["v", "k", "s", 1] : ["v", "k", "s", 1, "w.x"])]);
        }
        p.push({ $group: Object.fromEntries([["_id", "$" + keyField], ...accs.map(([nm, op, f]) => [nm, { [op]: f === 1 ? 1 : "$" + f }])]) });
        ref.push((rows) => refGroup(rows, keyField, accs));
    } else if (shape === 2) {
        const skip = int(5), limit = 1 + int(20);
        p.push({ $sort: { v: 1, _id: 1 } }, { $skip: skip }, { $limit: limit }, { $project: { v: 1 } });
        ref.push((rows) => refSort(rows).slice(skip, skip + limit).map((d) => (d.v === undefined ? { _id: d._id } : { _id: d._id, v: d.v })));
    } else if (shape === 3) {
        p.push({ $unwind: "$tags" }, { $group: { _id: "$tags", n: { $sum: 1 } } });
        ref.push((rows) => {
            const un = [];
            for (const d of rows) {
                if (Array.isArray(d.tags)) for (const x of d.tags) un.push({ ...d, tags: x });
                else if (!nullish(d.tags)) un.push(d);
            }
            return refGroup(un, "tags", [["n", "$sum", 1]]);
        });
    } else {
        p.push({ $bucket: { groupBy: "$v", boundaries: [0, 25, 50, 100], default: "other", output: { n: { $sum: 1 }, s: { $sum: "$v" } } } });
        ref.push((rows) => {
            const b = new Map();
            for (const d of rows) {
                const v = d.v;
                const id = isNum(v) && v >= 0 && v < 100 ? (v < 25 ? 0 : v < 50 ? 25 : 50) : "other";
                const g = b.get(id) || { _id: id, n: 0, s: 0 };
                g.n++;
                if (isNum(v)) g.s += v;
                b.set(id, g);
            }
            return [0, 25, 50, "other"].filter((k) => b.has(k)).map((k) => b.get(k));
        });
    }
    return { p, ref };
}

const general = (stage) => {
    const s = JSON.parse(JSON.stringify(stage));
    const wrap = (x) => (typeof x === "string" && x[0] === "$" ? { $cond: [true, x, 0] } : x);
    if (s.$group) for (const k of Object.keys(s.$group)) s.$group[k] = k === "_id" ? wrap(s.$group[k]) : { [Object.keys(s.$group[k])[0]]: wrap(Object.values(s.$group[k])[0]) };
    return s;
};

const db = new Database("oracle");
const docs = Array.from({ length: 3000 }, (_, i) => mkdoc(i));
const big = db.collection("big"), small = db.collection("small"), bigIx = db.collection("bigix");
big.insertMany(docs);
bigIx.insertMany(docs);
bigIx.createIndex({ k: 1 });
small.insertMany(docs.slice(0, 400));

t.test("150 random pipelines x {no index, index on k} x {offloaded, inline} x {direct, general $group}", async (a) => {
    let checked = 0;
    for (let i = 0; i < 150; i++) {
        const { p, ref } = genPipeline();
        const want = (rows) => JSON.stringify(ref.reduce((r, f) => f(r), rows));
        const wantBig = want(docs), wantSmall = want(docs.slice(0, 400));
        const runs = [
            [big, p, wantBig, "big"],
            [bigIx, p, wantBig, "bigix"],
            [small, p, wantSmall, "small"],
            [big, p.map(general), wantBig, "big general"],
            [small, p.map(general), wantSmall, "small general"],
        ];
        for (const [c, pipe, w, label] of runs) {
            const got = JSON.stringify(await c.aggregate(pipe));
            if (got !== w) {
                a.eq(got, w, `${label}: ${JSON.stringify(pipe)}`);
                return;
            }
            checked++;
        }
    }
    a.eq(checked, 750);
});

await t.run();
db.close();
