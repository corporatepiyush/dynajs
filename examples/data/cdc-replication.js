// CDC replication — trigger-based change capture shipped over TCP to an idempotent replica, with resume and repair.
//
// WHAT IT IS
//   Logical replication between two databases, the moving parts of Debezium in one file:
//
//   capture      SQLite triggers write every INSERT/UPDATE/DELETE on the source tables into a changelog with a
//                monotonically increasing log sequence number (LSN), in the same transaction as the change
//   transport    a TCP connection carries batches of changes as length-prefixed (uvarint) frames of
//                LZ4-compressed MessagePack; the replica acknowledges each batch with the LSN it reached
//   apply        the replica applies a batch in one transaction and stores its checkpoint LSN with it, so a
//                batch is applied exactly once even when it is delivered twice
//   resume       the connection is cut mid-stream; the shipper reconnects, asks for the checkpoint and resumes
//   verification per-table checksums over key ranges (XXH3 over canonical rows) are compared; a range that
//                differs is split until the bad rows are found (collected in a RoaringBitmap per table), then
//                those rows are re-shipped (anti-entropy)
//   bookkeeping  the replica keeps an OrderedMap "table/pk" -> last applied LSN (prefix scans answer "which rows of
//                orders changed"), a Multiset of the change mix and a t-digest of per-batch apply latency
//
// MODULES  dyna:net (SQLite, TCPServer) · dyna:serialize (MsgPack) · dyna:compress (lz4Frame) · dyna:encoding (varints)
//          dyna:hash (XXH3_64) · dyna:bytes (concat) · dyna:structures (OrderedMap, RoaringBitmap, Multiset, TDigest)
//          dyna:random · dyna:async · dyna:file · String/Number/Array extensions
//
// RUN      dynajs examples/data/cdc-replication.js     a write workload on the source, replication with a dropped
//          connection, a corrupted replica row, and the repair that finds it

import { SQLite, TCPServer } from "dyna:net";
import { MsgPackEncode, MsgPackDecode } from "dyna:serialize";
import { lz4Frame, lz4Unframe } from "dyna:compress";
import { PutUvarint, Uvarint } from "dyna:encoding";
import { XXH3_64 } from "dyna:hash";
import { concat } from "dyna:bytes";
import { OrderedMap, RoaringBitmap, Multiset, TDigest } from "dyna:structures";
import { Random } from "dyna:random";
import { sleep } from "dyna:async";
import { makeTempDir, removeAll } from "dyna:file";

const work = makeTempDir("cdc");
const TABLES = { customers: ["id", "name", "tier", "balance"], orders: ["id", "customer", "total", "status"] };

// =============================================================================
// Source: tables + change capture triggers
// =============================================================================
const source = new SQLite(work.join("source.db"));
source.exec("CREATE TABLE customers (id INTEGER PRIMARY KEY, name TEXT, tier TEXT, balance INTEGER)");
source.exec("CREATE TABLE orders (id INTEGER PRIMARY KEY, customer INTEGER, total INTEGER, status TEXT)");
source.exec("CREATE TABLE changelog (lsn INTEGER PRIMARY KEY AUTOINCREMENT, tbl TEXT, op TEXT, pk INTEGER, row TEXT)");
for (const [t, cols] of Object.entries(TABLES)) {
    const obj = (p) => `json_object(${cols.map((c) => `'${c}', ${p}.${c}`).join(", ")})`;
    source.exec(
        `CREATE TRIGGER ${t}_ins AFTER INSERT ON ${t} BEGIN INSERT INTO changelog (tbl, op, pk, row) VALUES ('${t}', 'upsert', NEW.id, ${obj("NEW")}); END`,
    );
    source.exec(
        `CREATE TRIGGER ${t}_upd AFTER UPDATE ON ${t} BEGIN INSERT INTO changelog (tbl, op, pk, row) VALUES ('${t}', 'upsert', NEW.id, ${obj("NEW")}); END`,
    );
    source.exec(
        `CREATE TRIGGER ${t}_del AFTER DELETE ON ${t} BEGIN INSERT INTO changelog (tbl, op, pk, row) VALUES ('${t}', 'delete', OLD.id, NULL); END`,
    );
}

// =============================================================================
// Replica: schema, checkpoint, idempotent apply, and a TCP endpoint
// =============================================================================
const replica = new SQLite(work.join("replica.db"));
replica.exec("CREATE TABLE customers (id INTEGER PRIMARY KEY, name TEXT, tier TEXT, balance INTEGER)");
replica.exec("CREATE TABLE orders (id INTEGER PRIMARY KEY, customer INTEGER, total INTEGER, status TEXT)");
replica.exec("CREATE TABLE checkpoint (id INTEGER PRIMARY KEY CHECK (id = 1), lsn INTEGER NOT NULL)");
replica.exec("INSERT INTO checkpoint VALUES (1, 0)");
const replicaLsn = () => replica.query("SELECT lsn FROM checkpoint")[0].lsn;
const stats = { batches: 0, duplicates: 0, applied: 0 };
const rowLsn = new OrderedMap(),
    changeMix = new Multiset(),
    applyMs = new TDigest();
const rowKey = (tbl, pk) => `${tbl}/${String(pk).padStart(8, "0")}`;

function applyBatch(batch) {
    if (batch.to <= replicaLsn()) {
        stats.duplicates++;
        return replicaLsn();
    } // already applied: ack and ignore
    const t0 = performance.now();
    replica.exec("BEGIN");
    for (const c of batch.changes) {
        if (c.lsn <= replicaLsn()) continue;
        const cols = TABLES[c.tbl];
        if (c.op === "delete") replica.exec(`DELETE FROM ${c.tbl} WHERE id = ?`, [c.pk]);
        else
            replica.exec(
                `INSERT INTO ${c.tbl} (${cols}) VALUES (${cols.map(() => "?")}) ON CONFLICT(id) DO UPDATE SET ${cols.slice(1).map((k) => `${k} = excluded.${k}`)}`,
                cols.map((k) => c.row[k]),
            );
        stats.applied++;
        changeMix.add(`${c.tbl}.${c.op}`);
        rowLsn.set(rowKey(c.tbl, c.pk), c.lsn);
    }
    replica.exec("UPDATE checkpoint SET lsn = ?", [batch.to]); // in the same transaction as the rows
    replica.exec("COMMIT");
    applyMs.add(performance.now() - t0);
    stats.batches++;
    return batch.to;
}

// Framing: uvarint length, then the payload. Partial frames accumulate across reads.
const frame = (obj) => {
    const body = lz4Frame(MsgPackEncode(obj));
    return concat([PutUvarint(body.length), body]);
};
function deframer(onMessage) {
    let buf = new Uint8Array(0);
    return (first, second) => {
        const chunk = second ?? first; // usable as feed(bytes) or as a TCP data(conn, bytes) handler
        buf = buf.length ? concat([buf, chunk]) : chunk;
        while (buf.length) {
            const [len, n] = Uvarint(buf);
            if (n <= 0 || buf.length < n + Number(len)) break;
            onMessage(MsgPackDecode(lz4Unframe(buf.subarray(n, n + Number(len)))));
            buf = buf.slice(n + Number(len));
        }
    };
}

let dropAfterBatches = 3; // simulate a network cut once
const server = new TCPServer({ port: 0 });
server.start({
    connect: (conn) => {
        conn.read = deframer((m) => {
            if (m.type === "hello") return conn.write(frame({ type: "checkpoint", lsn: replicaLsn() }));
            if (m.type === "batch") {
                if (dropAfterBatches-- === 0) return conn.close(); // the cut: this batch is lost in flight
                conn.write(frame({ type: "ack", lsn: applyBatch(m) }));
            }
        });
    },
    data: (conn, bytes) => conn.read(bytes),
});

// =============================================================================
// Shipper: connect, learn the checkpoint, stream batches, resume on failure
// =============================================================================
async function ship({ batchSize = 50 } = {}) {
    for (let attempt = 0; attempt < 5; attempt++) {
        const done = await new Promise((resolve) => {
            let from = 0,
                inflight = null,
                conn = null;
            const sendNext = () => {
                const changes = source
                    .query("SELECT lsn, tbl, op, pk, row FROM changelog WHERE lsn > ? ORDER BY lsn LIMIT ?", [
                        from,
                        batchSize,
                    ])
                    .map((c) => ({ ...c, row: c.row ? JSON.parse(c.row) : null }));
                if (!changes.length) {
                    resolve(true);
                    conn.close();
                    return;
                } // resolve first: close() fires the close handler synchronously
                inflight = changes.at(-1).lsn;
                conn.write(frame({ type: "batch", to: inflight, changes }));
            };
            TCPServer.connect(
                { host: "127.0.0.1", port: server.port },
                {
                    connect: (c, err) => {
                        if (err) return resolve(false);
                        conn = c;
                        c.write(frame({ type: "hello" }));
                    },
                    data: deframer((m) => {
                        if (m.type === "checkpoint") {
                            from = m.lsn;
                            sendNext();
                        } else if (m.type === "ack") {
                            from = m.lsn;
                            sendNext();
                        }
                    }),
                    close: () => resolve(false),
                },
            );
        });
        if (done) return attempt;
        await sleep(20); // back off, then resume from the checkpoint
    }
    throw new Error("replication did not complete");
}

// =============================================================================
// Verification: range checksums, bisected down to the differing rows
// =============================================================================
function rangeHash(db, table, lo, hi) {
    const rows = db.query(`SELECT * FROM ${table} WHERE id >= ? AND id < ? ORDER BY id`, [lo, hi]);
    return { n: rows.length, h: XXH3_64(JSON.stringify(rows.map((r) => TABLES[table].map((c) => r[c])))) };
}
function findDifferences(table, lo, hi, out = new RoaringBitmap()) {
    const a = rangeHash(source, table, lo, hi),
        b = rangeHash(replica, table, lo, hi);
    if (a.n === b.n && a.h === b.h) return out;
    if (hi - lo <= 8) {
        // small enough: compare row by row
        for (let id = lo; id < hi; id++) {
            const x = JSON.stringify(source.query(`SELECT * FROM ${table} WHERE id = ?`, [id])),
                y = JSON.stringify(replica.query(`SELECT * FROM ${table} WHERE id = ?`, [id]));
            if (x !== y) out.add(id);
        }
        return out;
    }
    const mid = Math.floor((lo + hi) / 2);
    findDifferences(table, lo, mid, out);
    return findDifferences(table, mid, hi, out);
}
function repair(table, ids) {
    for (const id of ids) {
        rowLsn.set(rowKey(table, id), -1);
        const row = source.query(`SELECT * FROM ${table} WHERE id = ?`, [id])[0];
        if (row)
            replica.exec(
                `INSERT OR REPLACE INTO ${table} (${TABLES[table]}) VALUES (${TABLES[table].map(() => "?")})`,
                TABLES[table].map((c) => row[c]),
            );
        else replica.exec(`DELETE FROM ${table} WHERE id = ?`, [id]);
    }
}

// =============================================================================
// The run
// =============================================================================
const results = {};
const rng = new Random(5);
function workload(n) {
    for (let i = 0; i < n; i++) {
        const roll = rng.nextFloat();
        source.exec("BEGIN");
        if (roll < 0.35)
            source.exec("INSERT INTO customers (name, tier, balance) VALUES (?, ?, ?)", [
                `cust-${i}`,
                rng.choice(["free", "pro", "team"]),
                rng.nextBounded(10000),
            ]);
        else if (roll < 0.7) {
            const c = source.query("SELECT id FROM customers ORDER BY RANDOM() LIMIT 1")[0];
            if (c)
                source.exec("INSERT INTO orders (customer, total, status) VALUES (?, ?, 'open')", [
                    c.id,
                    100 + rng.nextBounded(50000),
                ]);
        } else if (roll < 0.9)
            source.exec(
                "UPDATE orders SET status = 'paid' WHERE id = (SELECT id FROM orders WHERE status = 'open' ORDER BY id LIMIT 1)",
            );
        else if (roll < 0.96)
            source.exec("UPDATE customers SET balance = balance + 500 WHERE id = (SELECT MIN(id) FROM customers)");
        else source.exec("DELETE FROM orders WHERE id = (SELECT MAX(id) FROM orders WHERE status = 'open')");
        source.exec("COMMIT");
    }
}
workload(400);
const retries = await ship({ batchSize: 40 });
const lsnAfterFirst = replicaLsn();
results[`the dropped connection forced ${retries} reconnect(s)`] = retries >= 1;
results["the replica reached the source's last LSN"] =
    lsnAfterFirst === source.query("SELECT MAX(lsn) AS m FROM changelog")[0].m;

workload(150); // more writes while the replica is idle
await ship({ batchSize: 40 });

// Deliver a batch twice: the checkpoint makes the second delivery a no-op.
const dupBefore = stats.duplicates;
applyBatch({ to: replicaLsn(), changes: [] });
results["a redelivered batch is recognised and skipped"] = stats.duplicates === dupBefore + 1;

const tableHashes = (db) =>
    Object.keys(TABLES)
        .map((t) => rangeHash(db, t, 0, 1e9).h)
        .join();
results["source and replica are identical after replication"] = tableHashes(source) === tableHashes(replica);

// Someone edits the replica by hand; anti-entropy finds the exact rows and repairs them.
replica.exec("UPDATE orders SET total = total + 1 WHERE id IN (17, 120)");
replica.exec("DELETE FROM customers WHERE id = 33");
const maxOrder = source.query("SELECT MAX(id) AS m FROM orders")[0].m + 1,
    maxCust = source.query("SELECT MAX(id) AS m FROM customers")[0].m + 1;
const diffs = { orders: findDifferences("orders", 0, maxOrder), customers: findDifferences("customers", 0, maxCust) };
const diffList = Object.fromEntries(Object.entries(diffs).map(([t, b]) => [t, [...b]]));
results[`anti-entropy located ${JSON.stringify(diffList)}`] =
    diffList.orders.join() === "17,120" && diffList.customers.join() === "33";
results["the row-version map has one entry per order the changelog touched"] =
    rowLsn.prefix("orders/").length ===
    source.query("SELECT COUNT(DISTINCT pk) AS n FROM changelog WHERE tbl = 'orders'")[0].n;
results["the change mix accounts for every applied change"] = changeMix.totalSize === stats.applied;
for (const [t, ids] of Object.entries(diffs)) repair(t, ids);
results["repair restores equality"] = tableHashes(source) === tableHashes(replica);

const counts = (db) =>
    Object.keys(TABLES)
        .map((t) => `${t} ${db.query(`SELECT COUNT(*) AS n FROM ${t}`)[0].n}`)
        .join(", ");
print("cdc-replication");
print(`  source     ${counts(source)}; ${source.query("SELECT COUNT(*) AS n FROM changelog")[0].n} changes captured`);
print(`  replica    ${counts(replica)}; checkpoint LSN ${replicaLsn()}`);
print(
    `  transport  ${stats.batches} batches applied, ${stats.applied.format()} changes, ${retries} reconnect(s), ${stats.duplicates} duplicate(s) skipped`,
);
print(
    `  apply      p50 ${applyMs.quantile(0.5).toFixed(2)} ms, p99 ${applyMs.quantile(0.99).toFixed(2)} ms per batch; mix ${changeMix
        .entrySet()
        .sortWith([(a, b) => b[1] - a[1]])
        .map(([k, n]) => `${k} ${n}`)
        .join(", ")}`,
);
print(
    `  repair     found ${diffs.orders.size + diffs.customers.size} divergent rows by range bisection; newest order row ${rowLsn.lowerKey("orders/~")} at LSN ${rowLsn.get(rowLsn.lowerKey("orders/~"))}`,
);
server.close();
source.close();
replica.close();
removeAll(work);
check(results);

function check(results) {
    const names = Object.keys(results);
    const failed = names.filter((name) => !results[name]);
    for (const name of failed) print("FAIL " + name);
    if (failed.length) throw new Error(`${failed.length} of ${names.length} checks failed`);
}
