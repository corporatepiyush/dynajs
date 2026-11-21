// covers: contract dyna:docdb.Collection.find dyna:docdb.Collection.findOne dyna:docdb.Collection.countDocuments dyna:docdb.Collection.distinct dyna:docdb.Collection.updateMany dyna:docdb.Collection.deleteMany dyna:docdb.Collection.createIndex -- seeded differential fuzz: each query with and without every index kind against a JS reference
// Differential query fuzzer, seeded and reproducible. Documents mix every BSON type the engine stores (nested objects,
// arrays and arrays of arrays, missing vs null vs undefined, NaN, -0, +-Infinity, int64 beyond 2^53, Decimal128, dates,
// ObjectIds, regex values, huge and non-BMP strings, 60-deep nesting). Filters use every query operator ($eq $ne $gt
// $gte $lt $lte $in $nin $exists $type $regex/$options $size $all $elemMatch $not $mod, $and $or $nor $comment), dotted
// and numeric paths into arrays, and a share of malformed pieces (unknown operators, empty $and/$or/$nor, wrong
// argument types, nesting past the limit).
// Oracle 1: one plain collection is the reference; copies with single, compound, descending, multikey, sparse and
// partial indexes must give the same find result (set and order, also with sort+skip+limit and a projection),
// countDocuments, distinct, and the same updateMany/deleteMany counts and final contents. A malformed filter must throw
// a DocDBError (a numeric code) in every copy alike (a filter nested past BSON's 200 levels is refused by the encoder
// with the documented RangeError before it reaches the database): never a crash, a hang, a different error, or a silent answer.
// Oracle 2: on documents restricted to what a few lines of JS can model (finite numbers, strings, booleans, null,
// missing, dates, arrays and objects of those), a reference matcher written from the MongoDB manual must agree with
// the engine's _id set for every well-formed filter it understands.
// Oracle 3: set laws MongoDB guarantees, on the full document mix, unindexed and indexed: $nor/$not/$ne/$nin are exact
// complements, $or/$and are union/intersection, and {f: null} contains {f: {$exists: false}}.
// Sort ties are broken by _id (a final sort key) because the order of equal keys is not part of the contract.
// Long runs: DOCDB_FUZZ_SEED=<n> DOCDB_FUZZ_ROUNDS=<r> DOCDB_FUZZ_QUERIES=<q> (the suite default is one short round).
import { suite } from "../lib/kit.js";
import { Collection, Database, DocDBError, ObjectId } from "dyna:docdb";
import { Decimal128 } from "dyna:serialize";
import { Random } from "dyna:random";
import { env } from "dyna:sys";

const E = env();
const SEED = Number(E.DOCDB_FUZZ_SEED ?? 20261115);
const ROUNDS = Number(E.DOCDB_FUZZ_ROUNDS ?? 1);
const NQ = Number(E.DOCDB_FUZZ_QUERIES ?? 300);
const t = suite("docdb.find differential fuzz");

let rng;
const R = (n) => rng.nextBounded(n);
const pick = (xs) => xs[R(xs.length)];
const chance = (n) => R(n) === 0;

const OIDS = ["0123456789abcdef01234567", "0123456789abcdef01234568", "ffffffffffffffffffffffff"].map((h) => new ObjectId(h));
const HUGE = "h".repeat(5000);
const SCALARS = [0, -0, 1, 1.5, -2, 2, 3, 7, 100, NaN, Infinity, -Infinity, 2 ** 53, -(2 ** 31), 2 ** 31, 2n ** 60n,
    -(2n ** 62n), new Decimal128("2.5"), new Decimal128("1"), new Decimal128("NaN"), new Decimal128("-0"), "", "a", "b", "ab",
    "A", "é", "\u{1F600}", "a\u0000b", "￿", HUGE, null, undefined, true, false, new Date(0), new Date(-1),
    new Date(1.7e12), OIDS[0], OIDS[1], /ab/i, /^a/];
const MODEL = [0, -0, 1, 1.5, -2, 2, 3, 7, 100, "", "a", "b", "ab", "A", "é", "\u{1F600}", null, true, false,
    new Date(0), new Date(1.7e12)];
const KEYS = ["x", "y", "z"];

function value(depth, model) {
    const pool = model ? MODEL : SCALARS;
    const r = R(10);
    if (depth > 3 || r < 6) return pick(pool);
    if (r < 8) return Array.from({ length: R(4) }, () => value(depth + 1, model));
    const o = {};
    for (const k of KEYS) if (!chance(3)) o[k] = value(depth + 1, model);
    return o;
}
function deep(n) {
    let v = pick(SCALARS);
    for (let i = 0; i < n; i++) v = i % 3 ? { x: v } : [v];
    return v;
}
function makeDoc(i, model) {
    const d = { _id: i };
    for (const f of ["a", "b", "c"]) if (!chance(4)) d[f] = value(0, model);
    if (!chance(3)) d.arr = Array.from({ length: R(5) }, () => (chance(2) ? { x: value(1, model), y: value(2, model) } : value(1, model)));
    if (!chance(3)) d.n = { e: { f: value(1, model) }, g: value(0, model) };
    if (!model && chance(40)) d.deep = deep(30 + R(30));
    if (!model && chance(10)) d.c = chance(2) ? null : undefined;
    return d;
}

const PATHS = ["a", "b", "c", "a.x", "a.y", "b.x", "arr", "arr.x", "arr.y", "arr.0", "arr.1", "arr.x.y", "n.e.f", "n.g",
    "n.e", "a.0", "missing", "deep", "_id"];
const MPATHS = ["a", "b", "c", "a.x", "a.y", "b.x", "arr", "arr.x", "arr.y", "n.e.f", "n.g", "n.e", "missing", "_id"];
const TYPES = ["number", "string", "object", "array", "bool", "null", "date", "double", "int", "long", "decimal",
    "objectId", "regex", "undefined", "missing", 1, 2, 3, 4, 8, 9, 10, 16, 18, 19];

function cond(model, depth) {
    const v = () => value(2, model);
    const s = () => pick(model ? MODEL : SCALARS);
    switch (R(19)) {
        case 0: return s();
        case 1: return v();
        case 2: return { $eq: v() };
        case 3: return { $ne: s() };
        case 4: return { [pick(["$gt", "$gte", "$lt", "$lte"])]: s() };
        case 5: return { [pick(["$gt", "$gte"])]: s(), [pick(["$lt", "$lte"])]: s() };
        case 6: return { $in: Array.from({ length: R(5) }, s).concat(model || !chance(3) ? [] : [/a/]) };
        case 7: return { $nin: Array.from({ length: R(4) }, s) };
        case 8: return { $exists: chance(2) };
        case 9: return { $type: model ? pick(["number", "string", "object", "array", "bool", "null", "date"]) : pick(TYPES) };
        case 10: return model ? { $regex: pick(["^a", "b", "^$", "A"]) } : { $regex: pick(["^a", "b$", ".", "\\u00e9", "(a|b)+", "^$"]), ...(chance(2) ? { $options: pick(["i", "m", "s", "x", "im"]) } : {}) };
        case 11: return { $size: R(4) };
        case 12: return { $all: Array.from({ length: 1 + R(3) }, s) };
        case 13: return depth < 2 ? { $elemMatch: chance(2) ? { [pick(["$gt", "$lt", "$gte", "$eq"])]: s() } : filter(model, depth + 1, ["x", "y"]) } : s();
        case 14: return depth < 2 ? { $not: (() => { const c = cond(model, depth + 1); return c !== null && typeof c === "object" && !Array.isArray(c) && !(c instanceof Date) && Object.keys(c).every((k) => k[0] === "$") && Object.keys(c).length ? c : { $eq: c }; })() } : s();
        case 15: return { $mod: [pick([2, 3, -3, 7]), pick([0, 1, -1])] };
        case 16: return { $in: [null] };
        case 17: return null;
        default: return { $eq: [] };
    }
}
function filter(model, depth = 0, paths = model ? MPATHS : PATHS) {
    const f = {};
    for (let k = 0, n = R(3); k < n; k++) f[pick(paths)] = cond(model, depth);
    if (depth < 2 && chance(4)) f[pick(["$and", "$or", "$nor"])] = Array.from({ length: 1 + R(3) }, () => filter(model, depth + 1, paths));
    if (depth === 0 && chance(15)) f.$comment = "fuzz";
    return f;
}

const MALFORMED = [
    () => ({ a: { $foo: 1 } }), () => ({ $foo: [{ a: 1 }] }), () => ({ $and: [] }), () => ({ $or: [] }), () => ({ $nor: [] }),
    () => ({ $or: { a: 1 } }), () => ({ $and: [1] }), () => ({ a: { $in: 5 } }), () => ({ a: { $nin: "x" } }),
    () => ({ a: { $size: "x" } }), () => ({ a: { $size: -1 } }), () => ({ a: { $size: 1.5 } }), () => ({ a: { $regex: 5 } }),
    () => ({ a: { $regex: "(" } }), () => ({ a: { $options: "i" } }), () => ({ a: { $elemMatch: 3 } }),
    () => ({ a: { $type: "bogus" } }), () => ({ a: { $type: 999 } }), () => ({ a: { $mod: [0, 1] } }),
    () => ({ a: { $mod: [1] } }), () => ({ a: { $mod: "x" } }), () => ({ a: { $not: 5 } }), () => ({ a: { $not: {} } }),
    () => ({ a: { $all: 1 } }), () => ({ a: { $exists: true, b: 1 } }), () => ({ a: { $gt: 1, x: 2 } }),
    () => { let f = { a: 1 }; for (let i = 0; i < 120; i++) f = { $and: [f] }; return f; },
    () => { let f = { $eq: 1 }; for (let i = 0; i < 120; i++) f = { $not: f }; return { a: f }; },
    () => ({ a: { $elemMatch: { $foo: 1 } } }), () => ({ "": 1 }), () => ({ "a..b": 1 }), () => ({ ".a": 1 }), () => ({ "a.": 1 }),
];

const S = (x) => JSON.stringify(x, function (k, v) {
    const raw = this[k];
    if (typeof raw === "bigint") return `${raw}n`;
    if (typeof raw === "number") return Object.is(raw, -0) ? "-0" : Number.isFinite(raw) ? raw : String(raw);
    if (raw === undefined) return "<undef>";
    if (raw instanceof Date) return `D${raw.getTime()}`;
    if (raw instanceof RegExp) return `R${raw}`;
    if (raw instanceof Decimal128 || raw instanceof ObjectId) return `${raw.constructor.name}(${raw})`;
    return v;
});
const outcome = (fn) => {
    try {
        return { ok: S(fn()) };
    } catch (e) {
        if (e instanceof DocDBError && typeof e.code === "number") return { err: `DocDBError ${e.code}` };
        if (e instanceof RangeError && /nesting exceeds/.test(e.message)) return { err: "DocDBError nesting" };
        return { err: `${e?.name}: ${e?.message}` };
    }
};

const VARIANTS = [
    ["single", (c) => { for (const p of ["a", "b", "c", "a.x", "n.e.f", "_id", "deep"]) c.createIndex({ [p]: 1 }); }],
    ["multikey", (c) => { for (const p of ["arr", "arr.x", "arr.y", "arr.0", "a.0"]) c.createIndex({ [p]: 1 }); }],
    ["compound", (c) => { c.createIndex({ a: 1, b: -1 }); c.createIndex({ "n.g": -1, c: 1 }); c.createIndex({ b: 1, "arr.x": 1 }); }],
    ["desc_sparse", (c) => { c.createIndex({ a: -1 }); c.createIndex({ b: 1 }, { sparse: true }); c.createIndex({ c: -1 }, { sparse: true }); }],
    ["partial", (c) => { c.createIndex({ a: 1 }, { partialFilterExpression: { b: { $exists: true } } }); c.createIndex({ c: 1 }, { partialFilterExpression: { a: { $gt: 1 } } }); }],
];

function build(db, name, docs, setup) {
    const c = db.collection(name);
    if (!(c instanceof Collection)) throw new TypeError(`db.collection(${name}) is not a Collection`);
    for (const d of docs) {
        try {
            c.insertOne(d);
        } catch (e) {
            if (!(e instanceof DocDBError)) throw e;
        }
    }
    if (setup) {
        try {
            setup(c);
        } catch (e) {
            if (!(e instanceof DocDBError) || e.code !== 171) throw e;
        }
    }
    return c;
}

function differential(a, round) {
    rng = new Random(SEED + round);
    const docs = Array.from({ length: 160 + R(80) }, (_, i) => makeDoc(i, false));
    const db = new Database(`fz${round}`);
    const base = build(db, "base", docs);
    const colls = VARIANTS.map(([n, s]) => [n, build(db, n, docs, s)]);
    let bad = 0, malformed = 0, nonempty = 0;
    const same = (what, f, run) => {
        const want = outcome(() => run(base));
        if (want.err && !want.err.startsWith("DocDBError")) {
            if (bad++ < 5) a.ok(false, `${what} ${S(f)}: reference threw a non-DocDBError ${want.err}`);
            return want;
        }
        for (const [n, c] of colls) {
            const got = outcome(() => run(c));
            if (got.ok !== want.ok || got.err !== want.err)
                if (bad++ < 5) a.eq(got.ok ?? got.err, want.ok ?? want.err, `seed ${SEED + round} ${what} [${n}] ${S(f)}`);
        }
        return want;
    };
    for (let q = 0; q < NQ; q++) {
        const f = chance(8) ? pick(MALFORMED)() : filter(false);
        const ids = same("find", f, (c) => c.find(f).map((d) => d._id));
        if (ids.err) {
            malformed++;
            same("countDocuments", f, (c) => c.countDocuments(f));
            continue;
        }
        if (ids.ok !== "[]") nonempty++;
        const sk = { [pick(PATHS)]: pick([1, -1]) };
        if (chance(3)) sk[pick(["a", "b", "arr.x"])] = pick([1, -1]);
        sk._id = 1;
        const o = { sort: sk, skip: R(5), limit: pick([0, 1, 3, 10]) };
        if (chance(2)) o.projection = pick([{ _id: 1 }, { a: 1, "arr.x": 1 }, { a: 0, deep: 0 }, { "n.e": 1, _id: 0 }]);
        same("find sorted", [f, o], (c) => c.find(f, o));
        same("findOne", f, (c) => c.findOne(f, { sort: { _id: -1 } }));
        same("countDocuments", f, (c) => [c.countDocuments(f), c.countDocuments(f, { skip: 2, limit: 5 })]);
        const dp = pick(PATHS), df = chance(2) ? f : undefined;
        same("distinct", [dp, df], (c) => [c.distinct(dp), c.distinct("a", df)]);
    }
    a.ok(nonempty > NQ / 5, `filters matching something: ${nonempty} of ${NQ}`);
    a.ok(malformed > 0, "malformed filters were generated");
    for (let q = 0; q < 40; q++) {
        const f = filter(false);
        const u = pick([{ $set: { b: value(1, false) } }, { $inc: { "n.g": 1 } }, { $unset: { a: 1 } }, { $push: { arr: value(1, false) } }, { $set: { "a.x": 5 } }]);
        same("updateMany", [f, u], (c) => { const r = c.updateMany(f, u); return [r.matchedCount, r.modifiedCount]; });
        if (chance(4)) same("deleteMany", f, (c) => c.deleteMany(f).deletedCount);
    }
    same("final contents", {}, (c) => c.find({}, { sort: { _id: 1 } }));
    a.eq(bad, 0, `seed ${SEED + round}: disagreements with the unindexed reference`);
    db.close();
}

const isObj = (v) => v !== null && typeof v === "object" && !Array.isArray(v) && !(v instanceof Date);
const tag = (v) => (v === null ? "null" : typeof v === "number" ? "number" : typeof v === "string" ? "string" : typeof v === "boolean" ? "bool" : v instanceof Date ? "date" : Array.isArray(v) ? "array" : "object");
const RANK = { null: 1, number: 2, string: 3, object: 4, array: 5, bool: 8, date: 9 };
function cmpStr(x, y) {
    const a = [...x].map((ch) => ch.codePointAt(0)), b = [...y].map((ch) => ch.codePointAt(0));
    for (let i = 0; i < Math.min(a.length, b.length); i++) if (a[i] !== b[i]) return a[i] < b[i] ? -1 : 1;
    return a.length - b.length;
}
function cmp(x, y) {
    const tx = tag(x), ty = tag(y);
    if (tx !== ty) return RANK[tx] - RANK[ty];
    if (tx === "null") return 0;
    if (tx === "number" || tx === "bool") return x < y ? -1 : x > y ? 1 : 0;
    if (tx === "date") return x.getTime() - y.getTime();
    if (tx === "string") return cmpStr(x, y);
    if (tx === "array") {
        for (let i = 0; i < Math.min(x.length, y.length); i++) { const c = cmp(x[i], y[i]); if (c) return c; }
        return x.length - y.length;
    }
    const kx = Object.keys(x), ky = Object.keys(y);
    for (let i = 0; i < Math.min(kx.length, ky.length); i++) {
        const c = cmp(x[kx[i]], y[ky[i]]) || cmpStr(kx[i], ky[i]);
        if (c) return c;
    }
    return kx.length - ky.length;
}
const equal = (x, y) => cmp(x, y) === 0;
function at(doc, path) {
    let vs = [doc], miss = false;
    for (const p of path.split(".")) {
        const next = [];
        for (const v of vs)
            if (Array.isArray(v)) {
                if (v.length === 0) miss = true;
                for (const e of v)
                    if (isObj(e) && p in e) next.push(e[p]);
                    else miss = true;
            } else if (isObj(v) && p in v) next.push(v[p]);
            else miss = true;
        vs = next;
    }
    return { vals: vs, miss: miss || vs.length === 0 };
}
const expand = (vs) => vs.flatMap((v) => (Array.isArray(v) ? [v, ...v] : [v]));
function opMatch(found, op, arg, doc, key) {
    const direct = found.vals, vals = expand(direct);
    const eq = (x) => (x === null ? vals.some((v) => v === null) || found.miss : vals.some((v) => equal(v, x)));
    if (arg === null && (op === "$gte" || op === "$lte")) return eq(null);
    if (arg === null && (op === "$gt" || op === "$lt")) return false;
    const range = (test) => vals.some((v) => tag(v) === tag(arg) && tag(v) !== "array" && tag(v) !== "object" && test(cmp(v, arg)));
    switch (op) {
        case "$eq": return eq(arg);
        case "$ne": return !eq(arg);
        case "$gt": return range((c) => c > 0);
        case "$gte": return range((c) => c >= 0);
        case "$lt": return range((c) => c < 0);
        case "$lte": return range((c) => c <= 0);
        case "$in": return arg.some(eq);
        case "$nin": return !arg.some(eq);
        case "$exists": return direct.length > 0 === arg;
        case "$type": {
            const want = { number: ["number"], string: ["string"], object: ["object"], array: ["array"], bool: ["bool"], null: ["null"], date: ["date"] }[arg];
            return vals.some((v) => want.includes(tag(v))) || (arg === "array" && direct.some(Array.isArray));
        }
        case "$regex": return vals.some((v) => typeof v === "string" && new RegExp(arg, "u").test(v));
        case "$size": return direct.some((v) => Array.isArray(v) && v.length === arg);
        case "$all": return arg.length > 0 && arg.every(eq);
        case "$mod": return vals.some((v) => typeof v === "number" && Math.trunc(v) % arg[0] === arg[1]);
        case "$not": return !condMatch(found, arg, doc, key);
        case "$elemMatch": return direct.some((v) => Array.isArray(v) && v.some((e) =>
            Object.keys(arg).some((k) => k[0] === "$" && !["$and", "$or", "$nor", "$comment"].includes(k)) ? Object.entries(arg).every(([o, x]) => opMatch({ vals: [e], miss: false }, o, x, doc, key) && !Array.isArray(e)) : isObj(e) && refMatch(e, arg)));
    }
    throw new Error(`reference cannot model ${op}`);
}
function condMatch(found, c, doc, key) {
    if (isObj(c) && Object.keys(c).length && Object.keys(c).every((k) => k[0] === "$")) return Object.entries(c).every(([op, arg]) => opMatch(found, op, arg, doc, key));
    return opMatch(found, "$eq", c, doc, key);
}
function refMatch(doc, f) {
    return Object.entries(f).every(([k, c]) => {
        if (k === "$comment") return true;
        if (k === "$and") return c.every((g) => refMatch(doc, g));
        if (k === "$or") return c.some((g) => refMatch(doc, g));
        if (k === "$nor") return !c.some((g) => refMatch(doc, g));
        return condMatch(at(doc, k), c, doc, k);
    });
}

function reference(a, round) {
    rng = new Random(SEED * 7 + round);
    const docs = Array.from({ length: 200 }, (_, i) => makeDoc(i, true));
    const db = new Database(`ref${round}`);
    const c = build(db, "m", docs);
    let bad = 0, nonempty = 0;
    for (let q = 0; q < NQ; q++) {
        const f = filter(true);
        const want = S(docs.filter((d) => refMatch(d, f)).map((d) => d._id));
        const got = outcome(() => c.find(f, { sort: { _id: 1 } }).map((d) => d._id));
        if (want !== "[]") nonempty++;
        if (got.ok !== want && bad++ < 5) a.eq(got.ok ?? got.err, want, `seed ${SEED * 7 + round} ${S(f)}`);
    }
    a.ok(nonempty > NQ / 5, `reference filters matching something: ${nonempty} of ${NQ}`);
    a.eq(bad, 0, `seed ${SEED * 7 + round}: disagreements with the JS reference matcher`);
    db.close();
}

function laws(a, round) {
    rng = new Random(SEED * 13 + round);
    const docs = Array.from({ length: 200 }, (_, i) => makeDoc(i, false));
    const db = new Database(`law${round}`);
    const colls = [build(db, "plain", docs), build(db, "ix", docs, VARIANTS[0][1]), build(db, "mk", docs, VARIANTS[1][1])];
    let bad = 0, n = 0;
    const set = (c, f) => {
        try {
            return new Set(c.find(f, { projection: { _id: 1 } }).map((d) => S(d._id)));
        } catch (e) {
            if (!(e instanceof DocDBError)) throw e;
            return null;
        }
    };
    const all = new Set(colls[0].find({}).map((d) => S(d._id)));
    const law = (name, f, g, rel) => {
        for (const c of colls) {
            const x = set(c, f), y = set(c, g);
            if (!x || !y) return;
            n++;
            if (!rel(x, y) && bad++ < 5) a.ok(false, `seed ${SEED * 13 + round} ${c.name} ${name}: ${S(f)} vs ${S(g)}`);
        }
    };
    const complement = (x, y) => x.size + y.size === all.size && [...x].every((k) => !y.has(k));
    const subset = (x, y) => [...x].every((k) => y.has(k));
    for (let q = 0; q < NQ; q++) {
        const f = filter(false), g = filter(false), p = pick(PATHS), c = cond(false, 1), x = pick(SCALARS);
        law("$nor", f, { $nor: [f] }, complement);
        law("$or", { $or: [f, g] }, f, (u, a1) => subset(a1, u) && subset(set(colls[0], g) ?? new Set(), u));
        law("$and", { $and: [f, g] }, f, (i, a1) => subset(i, a1));
        if (c !== null && typeof c === "object" && !Array.isArray(c) && !(c instanceof Date) && Object.keys(c).length && Object.keys(c).every((k) => k[0] === "$"))
            law("$not", { [p]: c }, { [p]: { $not: c } }, complement);
        if (!(x instanceof RegExp) && x !== undefined) law("$ne", { [p]: x }, { [p]: { $ne: x } }, complement);
        const l = Array.from({ length: R(4) }, () => pick(SCALARS)).filter((v) => !(v instanceof RegExp));
        law("$nin", { [p]: { $in: l } }, { [p]: { $nin: l } }, complement);
        law("null", { [p]: { $exists: false } }, { [p]: null }, subset);
    }
    a.ok(n > NQ * 10, `law checks run: ${n}`);
    a.eq(bad, 0, `seed ${SEED * 13 + round}: violated set laws`);
    db.close();
}

for (let r = 0; r < ROUNDS; r++) {
    t.test(`round ${r}: indexed copies agree with the unindexed collection, malformed input throws DocDBError`, (a) => differential(a, r));
    t.test(`round ${r}: the engine agrees with a JS reference matcher on modelable documents`, (a) => reference(a, r));
    t.test(`round ${r}: complement, union, intersection and null-contains-missing laws hold`, (a) => laws(a, r));
}

await t.run();
