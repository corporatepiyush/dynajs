// covers: dyna:docdb.Collection.aggregate
// $group remembers recent keys in a small direct-mapped cache keyed by the key's raw BSON bytes and type (values up to
// 30 bytes), so a repeated key skips the keystring encoding and the hash-map probe; a run whose first 4096 lookups hit
// less than half the time switches the cache off. The cache may only ever answer what the keystring path would: equal
// values with different bytes (int32 1, int64 1n, double 2^31 and int64 2^31, -0 and 0) still meet in one group
// through the slow path, equal bytes under different types (a date and an int64 of the same millisecond count) stay
// apart, a missing key joins the null group, keys longer than the cache holds are grouped exactly, and $bucket keeps its
// own boundaries. Each pipeline is checked against a JS grouping of the same documents.
import { suite } from "../lib/kit.js";
import { Database } from "dyna:docdb";
import { Random } from "dyna:random";

const t = suite("docdb.Collection.aggregate $group key cache");
const db = new Database("gk");
const rng = new Random(4242);

function canon(v) {
    if (v === undefined || v === null) return "null";
    if (typeof v === "bigint") return `num:${Number(v)}`;
    if (typeof v === "number") return `num:${v === 0 ? 0 : v}`;
    if (v instanceof Date) return `date:${v.getTime()}`;
    if (typeof v === "string") return `str:${v}`;
    if (typeof v === "boolean") return `bool:${v}`;
    return `json:${JSON.stringify(v)}`;
}

function expected(docs, field) {
    const m = new Map();
    for (const d of docs) {
        const k = canon(d[field]);
        const g = m.get(k) ?? { n: 0, s: 0 };
        g.n++;
        g.s += d.w;
        m.set(k, g);
    }
    return m;
}

async function grouped(c, field) {
    const m = new Map();
    for (const g of await c.aggregate([{ $group: { _id: `$${field}`, n: { $sum: 1 }, s: { $sum: "$w" } } }])) {
        const k = canon(g._id);
        if (m.has(k)) throw new Error(`group ${k} appears twice`);
        m.set(k, { n: g.n, s: g.s });
    }
    return m;
}

function same(a, got, want, what) {
    a.eq(got.size, want.size, `${what}: group count`);
    for (const [k, v] of want) a.eq(JSON.stringify(got.get(k)), JSON.stringify(v), `${what}: ${k}`);
}

const LONG = "x".repeat(40);
const pool = [1, 1n, 2147483648, 2147483648n, -0, 0, 0n, new Date(5), 5n, 5, null, undefined, "a", "ab", `${LONG}1`, `${LONG}2`, true, false, 1.5, "paid", "shipped"];
const mixed = Array.from({ length: 20000 }, (_, i) => {
    const d = { _id: i, w: i % 7 };
    const v = pool[rng.nextBounded(pool.length)];
    if (v !== undefined) d.k = v;
    return d;
});
const mc = db.collection("mixed");
mc.insertMany(mixed);

t.test("equal values with different bytes meet, equal bytes under different types stay apart, missing joins null", async (a) => {
    const got = await grouped(mc, "k");
    same(a, got, expected(mixed, "k"), "mixed");
    a.ok(got.has("date:5") && got.has("num:5"), "a date and an int64 of the same count are two groups");
    a.eq(got.get("num:1").n, mixed.filter((d) => d.k === 1 || d.k === 1n).length, "int32 1 and int64 1n are one group");
    a.eq(got.get("num:0").n, mixed.filter((d) => d.k === 0 || d.k === 0n).length, "-0, 0 and 0n are one group");
    a.eq(got.get("null").n, mixed.filter((d) => d.k === null || d.k === undefined).length, "missing joins null");
});

t.test("a key of every cardinality (few, near the cache size, beyond it, all distinct) groups exactly", async (a) => {
    for (const card of [3, 300, 1000, 1100, 5000, 30000]) {
        const docs = Array.from({ length: 30000 }, (_, i) => ({ _id: i, w: i % 5, k: card === 30000 ? `u${i}` : rng.nextBounded(4) ? `k${i % card}` : i % card }));
        const c = db.collection(`card${card}`);
        c.insertMany(docs);
        same(a, await grouped(c, "k"), expected(docs, "k"), `cardinality ${card}`);
    }
});

t.test("keys alternating between cached and uncached forms after the cache switches off stay exact", async (a) => {
    const docs = Array.from({ length: 12000 }, (_, i) => ({ _id: i, w: 1, k: i < 6000 ? `z${i}` : i % 2 ? 7 : 7n }));
    const c = db.collection("switch");
    c.insertMany(docs);
    same(a, await grouped(c, "k"), expected(docs, "k"), "switch-off");
});

t.test("$bucket keeps its boundaries and default", async (a) => {
    const out = await mc.aggregate([
        { $match: { k: { $type: "number" } } },
        { $bucket: { groupBy: "$k", boundaries: [0, 1, 2, 3000000000], default: "other", output: { n: { $sum: 1 } } } },
    ]);
    const want = { 0: 0, 1: 0, 2: 0, other: 0 };
    for (const d of mixed) {
        if (typeof d.k !== "number" && typeof d.k !== "bigint") continue;
        const v = Number(d.k);
        want[v < 0 ? "other" : v < 1 ? 0 : v < 2 ? 1 : v < 3000000000 ? 2 : "other"]++;
    }
    a.eq(JSON.stringify(out.map((b) => [b._id, b.n])), JSON.stringify([[0, want[0]], [1, want[1]], [2, want[2]], ["other", want.other]].filter((x) => x[1])));
});

await t.run();
db.close();
