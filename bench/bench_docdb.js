// dyna:docdb against the pure-JS engine the document-database example used to be (bench/bench_docdb_js_engine.js) and
// against SQLite (dyna:net, in memory, the same data normalised into orders/items/customers tables with the same
// indexes). One seeded workload (generated, never stored): orders with 1-4 line items; point queries on an indexed
// field, an indexed range with sort+limit, a revenue-by-SKU aggregation (match, unwind, group, sort, limit), orders per
// customer city (group, join, group), updates through an index, and a snapshot written and read back (the JS engine
// syncs and replays its journal; SQLite writes a file with VACUUM INTO and reopens it). Each engine's answers are
// checked against the others before any time is reported. The workload runs at several sizes (orders N; customers N/20,
// so every size has ~20 orders per customer). Repetitions are interleaved: each rep runs every engine once, in an order
// rotated per rep, so drift in machine load hits all engines alike; before every timed run a fixed CPU control (an
// integer loop) is timed too, and its spread (worst/best) is printed per size as the noise floor of that table. Fresh-
// state operations (load, update, snapshot) reload untimed before each rep. The host load average is printed and the
// run refuses to start above DOCDB_BENCH_MAX_LOAD (default 12) unless that is set to 0. All three engines share one JS
// heap, so under --std a full GC runs (untimed) before every timed op: otherwise the JS engine's garbage is collected
// inside whichever engine's op happens to trip the GC threshold.
// Usage: dynajs --std bench/bench_docdb.js [reps=5] [sizes=1000,10000,100000]
import { Database } from "dyna:docdb";
import { Database as JsDatabase } from "./bench_docdb_js_engine.js";
import { SQLite } from "dyna:net";
import { makeTempDir, removeAll } from "dyna:file";
import { Random } from "dyna:random";
import { Table } from "dyna:cli";
import { loadAvg, env } from "dyna:sys";

const RUNS = Number(scriptArgs[1] ?? 5);
const SIZES = String(scriptArgs[2] ?? "1000,10000,100000").split(",").map(Number);
let rng = new Random(1010);
const CITIES = ["Berlin", "Paris", "Madrid", "Rome", "Vienna", "Warsaw"];
const STATUS = ["new", "paid", "paid", "shipped", "shipped", "shipped", "cancelled"];
let customers, orders, POINTS;
function generate(N) {
    rng = new Random(1010);
    customers = Array.from({ length: Math.max(1, Math.floor(N / 20)) }, (_, c) => ({
        _id: `c${c}`,
        email: `user${c}@mail.example`,
        address: { city: CITIES[c % 6], zip: String(10000 + c) },
    }));
    orders = Array.from({ length: N }, (_, o) => {
        const items = Array.from({ length: 1 + rng.nextBounded(4) }, () => ({
            sku: `SKU-${rng.nextBounded(300)}`,
            qty: 1 + rng.nextBounded(3),
            price: 5 + rng.nextBounded(200),
        }));
        return {
            _id: `o${String(o).padStart(6, "0")}`,
            customer: `c${rng.nextBounded(customers.length)}`,
            status: STATUS[rng.nextBounded(7)],
            total: items.reduce((s, i) => s + i.qty * i.price, 0),
            items,
        };
    });
    POINTS = Array.from({ length: 2000 }, (_, i) => `c${(i * 7919) % customers.length}`);
}
const REVENUE = [
    { $match: { status: { $in: ["paid", "shipped"] } } },
    { $unwind: "$items" },
    { $group: { _id: "$items.sku", units: { $sum: "$items.qty" }, revenue: { $sum: { $multiply: ["$items.qty", "$items.price"] } } } },
    { $sort: { revenue: -1, _id: 1 } },
    { $limit: 5 },
];
const byCity = (city) => [
    { $match: { status: "shipped" } },
    { $group: { _id: "$customer", n: { $sum: 1 } } },
    { $lookup: { from: "customers", localField: "_id", foreignField: "_id", as: "c" } },
    { $project: { city, n: 1 } },
    { $group: { _id: "$city", orders: { $sum: "$n" } } },
    { $sort: { _id: 1 } },
];

function nativeEngine() {
    let db, o, c;
    const dir = makeTempDir("bench-docdb");
    return {
        name: "dyna:docdb",
        load() {
            db = new Database("bench");
            c = db.collection("customers");
            o = db.collection("orders");
            c.createIndex({ email: 1 }, { unique: true });
            for (const f of ["customer", "status", "total", "items.sku"]) o.createIndex({ [f]: 1 });
            c.insertMany(customers);
            for (const d of orders) o.insertOne(d);
        },
        point: () => POINTS.reduce((n, id) => n + o.find({ customer: id }).length, 0),
        range: () => o.find({ total: { $gte: 1500 } }, { sort: { total: -1, _id: 1 }, limit: 50, projection: { _id: 1 } }).map((d) => d._id),
        revenue: async () => (await o.aggregate(REVENUE)).map((r) => `${r._id}:${r.units}:${r.revenue}`).join(),
        city: async () => (await o.aggregate(byCity({ $first: "$c.address.city" }))).map((r) => `${r._id}:${r.orders}`).join(),
        update: () => POINTS.slice(0, 200).reduce((n, id) => n + o.updateMany({ customer: id, status: "new" }, { $inc: { total: 1 } }).modifiedCount, 0),
        async snapshot() {
            await db.persist(`${dir}/s.dds`, { fsync: false });
            db.close();
            db = await Database.load(`${dir}/s.dds`);
            o = db.collection("orders");
            return o.estimatedDocumentCount();
        },
        close() {
            db.close();
            removeAll(dir);
        },
    };
}

function jsEngine() {
    let db, o, c, dir;
    return {
        name: "JS example engine",
        load() {
            if (dir) removeAll(dir);
            dir = makeTempDir("bench-docdb-js");
            db = new JsDatabase(dir);
            c = db.collection("customers");
            o = db.collection("orders");
            c.createIndex("email", { unique: true });
            for (const f of ["customer", "status", "total", "items.sku"]) o.createIndex(f);
            for (const d of customers) c.insertOne(structuredClone(d));
            for (const d of orders) o.insertOne(structuredClone(d));
        },
        point: () => POINTS.reduce((n, id) => n + o.find({ customer: id }).length, 0),
        range: () => o.find({ total: { $gte: 1500 } }, { sort: { total: -1, _id: 1 }, limit: 50, projection: { _id: 1 } }).map((d) => d._id),
        revenue: () => o.aggregate(REVENUE).map((r) => `${r._id}:${r.units}:${r.revenue}`).join(),
        city: () => o.aggregate(byCity("$c.0.address.city")).map((r) => `${r._id}:${r.orders}`).join(),
        update: () => POINTS.slice(0, 200).reduce((n, id) => n + o.updateMany({ customer: id, status: "new" }, { $inc: { total: 1 } }), 0),
        snapshot() {
            db.sync();
            db = new JsDatabase(dir);
            o = db.collection("orders");
            return o.docs.size;
        },
        close() {
            removeAll(dir);
        },
    };
}

function sqliteEngine() {
    let db;
    const dir = makeTempDir("bench-docdb-sql");
    const rows = (sql, p) => db.query(sql, p);
    let snaps = 0;
    return {
        name: "SQLite (dyna:net)",
        load() {
            if (db) db.close();
            db = new SQLite(":memory:");
            db.exec("CREATE TABLE customers (id TEXT PRIMARY KEY, email TEXT UNIQUE, city TEXT, doc TEXT)");
            db.exec("CREATE TABLE orders (id TEXT PRIMARY KEY, customer TEXT, status TEXT, total INTEGER, doc TEXT)");
            db.exec("CREATE TABLE items (order_id TEXT, sku TEXT, qty INTEGER, price INTEGER)");
            for (const ix of ["orders(customer)", "orders(status)", "orders(total)", "items(sku)", "items(order_id)"])
                db.exec(`CREATE INDEX ON ${ix}`.replace("ON", `ix_${ix.replace(/\W/g, "_")} ON`));
            db.exec("BEGIN");
            for (const d of customers) db.exec("INSERT INTO customers VALUES (?, ?, ?, ?)", [d._id, d.email, d.address.city, JSON.stringify(d)]);
            for (const d of orders) {
                db.exec("INSERT INTO orders VALUES (?, ?, ?, ?, ?)", [d._id, d.customer, d.status, d.total, JSON.stringify(d)]);
                for (const i of d.items) db.exec("INSERT INTO items VALUES (?, ?, ?, ?)", [d._id, i.sku, i.qty, i.price]);
            }
            db.exec("COMMIT");
        },
        point: () => POINTS.reduce((n, id) => n + rows("SELECT doc FROM orders WHERE customer = ?", [id]).map((r) => JSON.parse(r.doc)).length, 0),
        range: () => rows("SELECT id FROM orders WHERE total >= 1500 ORDER BY total DESC, id LIMIT 50").map((r) => r.id),
        revenue: () =>
            rows(
                "SELECT sku, SUM(qty) AS units, SUM(qty * price) AS revenue FROM items JOIN orders ON orders.id = items.order_id " +
                    "WHERE status IN ('paid', 'shipped') GROUP BY sku ORDER BY revenue DESC, sku LIMIT 5",
            )
                .map((r) => `${r.sku}:${r.units}:${r.revenue}`)
                .join(),
        city: () =>
            rows("SELECT c.city AS city, COUNT(*) AS n FROM orders o JOIN customers c ON c.id = o.customer WHERE o.status = 'shipped' GROUP BY c.city ORDER BY c.city")
                .map((r) => `${r.city}:${r.n}`)
                .join(),
        update: () => {
            let n = 0;
            for (const id of POINTS.slice(0, 200)) {
                const ids = rows("SELECT id, doc FROM orders WHERE customer = ? AND status = 'new'", [id]);
                for (const r of ids) {
                    const d = JSON.parse(r.doc);
                    d.total += 1;
                    n += db.exec("UPDATE orders SET total = ?, doc = ? WHERE id = ?", [d.total, JSON.stringify(d), r.id]);
                }
            }
            return n;
        },
        snapshot() {
            const path = `${dir}/s${++snaps}.db`;
            db.exec(`VACUUM INTO '${path}'`);
            db.close();
            db = new SQLite(path);
            return rows("SELECT COUNT(*) AS n FROM orders")[0].n;
        },
        close() {
            db.close();
            removeAll(dir);
        },
    };
}

function control() {
    const t = performance.now();
    let x = 0;
    for (let i = 0; i < 2000000; i++) x = (x * 31 + i) | 0;
    if (x === 42) print("");
    return performance.now() - t;
}

const median = (v) => {
    const s = [...v].sort((a, b) => a - b);
    return s.length % 2 ? s[s.length >> 1] : (s[s.length / 2 - 1] + s[s.length / 2]) / 2;
};
const ms = (v) => (v >= 100 ? v.toFixed(0) : v >= 10 ? v.toFixed(1) : v.toFixed(2));
const ops = ["load", "point", "range", "revenue", "city", "update", "snapshot"];
const fresh = new Set(["load", "update", "snapshot"]);
const std = await import("std").catch(() => null);
const settle = () => std?.gc?.();
const maxLoad = Number(env().DOCDB_BENCH_MAX_LOAD ?? 12);
const before = loadAvg();
if (maxLoad > 0 && before[0] > maxLoad) throw new Error(`load average ${before[0].toFixed(1)} > ${maxLoad}: wait for a quiet host (DOCDB_BENCH_MAX_LOAD=0 overrides)`);

for (const N of SIZES) {
    generate(N);
    const engines = [nativeEngine(), jsEngine(), sqliteEngine()];
    const times = engines.map(() => Object.fromEntries(ops.map((o) => [o, []])));
    const ctl = [];
    const answers = {};
    for (const e of engines) e.load();
    for (const op of ops) {
        for (let r = 0; r < RUNS; r++) {
            for (let k = 0; k < engines.length; k++) {
                const ei = (k + r) % engines.length, e = engines[ei];
                if (fresh.has(op) && op !== "load") e.load();
                settle();
                ctl.push(control());
                const t = performance.now();
                let out = await e[op]();
                times[ei][op].push(performance.now() - t);
                if (op === "load") out = null;
                const key = JSON.stringify(out);
                answers[op] ??= key;
                if (answers[op] !== key) throw new Error(`${e.name} disagrees on ${op} at N=${N}: ${key.slice(0, 120)} vs ${answers[op].slice(0, 120)}`);
            }
        }
        if (op === "load") for (const e of engines) e.load();
    }
    for (const e of engines) e.close();
    const head = ["operation", ...engines.map((e) => `${e.name} best / median`), "winner", "docdb vs best other"];
    const rows = ops.map((op) => {
        const b = engines.map((_, i) => Math.min(...times[i][op]));
        const m = engines.map((_, i) => median(times[i][op]));
        const w = b.indexOf(Math.min(...b));
        const other = Math.min(...b.slice(1));
        return [op, ...b.map((x, i) => `${ms(x)} / ${ms(m[i])} ms`), engines[w].name, b[0] <= other ? `${(other / b[0]).toFixed(1)}x faster` : `${(b[0] / other).toFixed(1)}x SLOWER`];
    });
    const cm = Math.min(...ctl);
    print(`\nN = ${N} orders, ${customers.length} customers, ${RUNS} interleaved reps; answers matched across engines; ${std ? "GC run before every timed op (--std)" : "no GC settling (run with --std for it)"}`);
    print(`control loop: best ${ms(cm)} ms, median ${ms(median(ctl))} ms, worst/best ${(Math.max(...ctl) / cm).toFixed(2)}x`);
    print(Table(rows, { head }));
}
const after = loadAvg();
print(`\nload average 1m: ${before[0].toFixed(1)} at start, ${after[0].toFixed(1)} at end`);
