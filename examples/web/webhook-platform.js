// Webhook platform — transactional outbox, signed deliveries, backoff, ordering, dead letters, replay, and verifying receivers.
//
// WHAT IT IS
//   Outbound webhooks done properly, end to end:
//
//   outbox        the application writes its state change and the event in ONE SQLite transaction, so an event
//                 is never lost and never sent for a change that rolled back
//   subscriptions endpoints subscribe to topic patterns ("order.*", "invoice.paid"); each has its own secret
//   signing       Stripe-style `Webhook-Signature: t=<unix>,v1=<hmac-sha256(t.body)>`; receivers reject stale
//                 timestamps (replay window) and duplicate event ids
//   delivery      per-endpoint ordering (one in flight per endpoint, endpoints in parallel), exponential backoff
//                 with full jitter, a per-request timeout, 410 Gone disables the subscription
//   dead letters  after the last attempt the delivery is parked; an operator replays it once the endpoint is fixed
//   evidence      every attempt is recorded (status, latency, error); counters in Prometheus format; each
//                 endpoint also keeps a latency TDigest and an outcome Multiset, and the health report ranks
//                 endpoints by attempts per delivered event in a ZSet (the noisiest receivers first)
//
// MODULES  dyna:http (App, fetch) · dyna:net (SQLite, Metrics) · dyna:crypto (Hmac) · dyna:async (Semaphore, sleep)
//          dyna:random · dyna:structures (LRU, TDigest, Multiset, ZSet) · dyna:file (Glob) · dyna:uuid · dyna:time
//
// RUN      dynajs examples/web/webhook-platform.js     orders flow through the outbox to four receivers: healthy,
//          flaky, broken-then-fixed, and gone; the run checks ordering, signatures, retries, DLQ and replay

import { App } from "dyna:http";
import { SQLite, Metrics } from "dyna:net";
import { Hmac, TimingSafeEqual } from "dyna:crypto";
import { Semaphore, sleep } from "dyna:async";
import { Random } from "dyna:random";
import { LRU, TDigest, Multiset, ZSet } from "dyna:structures";
import { Glob, makeTempDir, removeAll } from "dyna:file";
import { v7 } from "dyna:uuid";
import { nowSec } from "dyna:time";
import { HexDecode } from "dyna:encoding";

const work = makeTempDir("webhooks");
const db = new SQLite(work.join("platform.db"));
db.exec(`CREATE TABLE orders (id TEXT PRIMARY KEY, customer TEXT NOT NULL, total TEXT NOT NULL, status TEXT NOT NULL)`);
db.exec(
    `CREATE TABLE outbox (seq INTEGER PRIMARY KEY, id TEXT NOT NULL, topic TEXT NOT NULL, payload TEXT NOT NULL, created INTEGER NOT NULL)`,
);
db.exec(
    `CREATE TABLE subscriptions (id TEXT PRIMARY KEY, url TEXT NOT NULL, pattern TEXT NOT NULL, secret TEXT NOT NULL, active INTEGER NOT NULL DEFAULT 1)`,
);
db.exec(`CREATE TABLE deliveries (id TEXT PRIMARY KEY, sub TEXT NOT NULL, event_seq INTEGER NOT NULL, state TEXT NOT NULL, attempts INTEGER NOT NULL DEFAULT 0,
         last_status INTEGER, last_error TEXT, UNIQUE (sub, event_seq))`);
db.exec(`CREATE TABLE attempts (delivery TEXT NOT NULL, n INTEGER NOT NULL, status INTEGER, ms REAL, error TEXT)`);

// =============================================================================
// Application side: state change + event in one transaction
// =============================================================================
function placeOrder(customer, total) {
    const id = "ord_" + v7();
    db.exec("BEGIN");
    try {
        db.exec("INSERT INTO orders VALUES (?, ?, ?, 'placed')", [id, customer, total]);
        db.exec("INSERT INTO outbox (id, topic, payload, created) VALUES (?, 'order.placed', ?, ?)", [
            v7(),
            JSON.stringify({ order: id, customer, total }),
            nowSec(),
        ]);
        if (total === "0.00") throw new Error("empty order"); // a business rule failing after the inserts
        db.exec("COMMIT");
        return id;
    } catch (e) {
        db.exec("ROLLBACK"); // the event disappears with the order
        return null;
    }
}
function payOrder(id) {
    db.exec("BEGIN");
    db.exec("UPDATE orders SET status = 'paid' WHERE id = ?", [id]);
    db.exec("INSERT INTO outbox (id, topic, payload, created) VALUES (?, 'invoice.paid', ?, ?)", [
        v7(),
        JSON.stringify({ order: id }),
        nowSec(),
    ]);
    db.exec("COMMIT");
}

// =============================================================================
// Dispatcher
// =============================================================================
const MAX_ATTEMPTS = 5,
    BASE_DELAY_MS = 10,
    TIMEOUT_MS = 300;
const rng = new Random(11);
const lanes = new Map(); // subscription -> Semaphore(1): ordering
let fanOutCursor = 0;

function fanOut() {
    // outbox rows -> one delivery per matching subscription
    const events = db.query("SELECT * FROM outbox WHERE seq > ? ORDER BY seq", [fanOutCursor]);
    const subs = db.query("SELECT * FROM subscriptions WHERE active = 1");
    for (const e of events) {
        for (const s of subs)
            if (Glob.match(e.topic, s.pattern))
                db.exec("INSERT OR IGNORE INTO deliveries (id, sub, event_seq, state) VALUES (?, ?, ?, 'pending')", [
                    v7(),
                    s.id,
                    e.seq,
                ]);
        fanOutCursor = e.seq;
    }
    return events.length;
}
const health = new Map(); // subscription -> { latency, outcomes }
function sign(secret, timestamp, body) {
    const h = new Hmac("sha256", HexDecode(secret));
    try {
        return `t=${timestamp},v1=${h.signHex(`${timestamp}.${body}`)}`;
    } finally {
        h.close();
    }
}
async function attempt(d, sub, event) {
    const body = JSON.stringify({
        id: event.id,
        topic: event.topic,
        created: event.created,
        data: JSON.parse(event.payload),
    });
    const t0 = performance.now();
    let status = null,
        error = null;
    try {
        const r = await fetch(sub.url, {
            method: "POST",
            timeout: TIMEOUT_MS,
            body,
            headers: {
                "content-type": "application/json",
                "webhook-id": event.id,
                "webhook-signature": sign(sub.secret, nowSec(), body),
            },
        });
        status = r.status;
        await r.text();
    } catch (e) {
        error = e.message;
    }
    const ms = performance.now() - t0;
    db.exec("INSERT INTO attempts VALUES (?, ?, ?, ?, ?)", [d.id, d.attempts + 1, status, ms, error]);
    const h = health.getOrInsertComputed(sub.id, () => ({ latency: new TDigest(100), outcomes: new Multiset() }));
    h.latency.add(ms);
    h.outcomes.add(status ? String(status) : "error");
    Metrics.counter("webhook_attempts_total", 1, { outcome: status ? String(status) : "error" });
    return { status, error };
}
async function deliver(d) {
    const sub = db.query("SELECT * FROM subscriptions WHERE id = ?", [d.sub])[0];
    const event = db.query("SELECT * FROM outbox WHERE seq = ?", [d.event_seq])[0];
    for (let n = d.attempts; n < MAX_ATTEMPTS; n++) {
        if (!db.query("SELECT active FROM subscriptions WHERE id = ?", [sub.id])[0].active) break;
        const r = await attempt({ ...d, attempts: n }, sub, event);
        db.exec("UPDATE deliveries SET attempts = ?, last_status = ?, last_error = ? WHERE id = ?", [
            n + 1,
            r.status,
            r.error,
            d.id,
        ]);
        if (r.status >= 200 && r.status < 300) {
            db.exec("UPDATE deliveries SET state = 'delivered' WHERE id = ?", [d.id]);
            return "delivered";
        }
        if (r.status === 410) {
            // the receiver told us to stop for good
            db.exec("UPDATE subscriptions SET active = 0 WHERE id = ?", [sub.id]);
            db.exec("UPDATE deliveries SET state = 'cancelled' WHERE sub = ? AND state IN ('pending', 'dead')", [
                sub.id,
            ]);
            return "gone";
        }
        if (r.status >= 400 && r.status < 500 && r.status !== 408 && r.status !== 429) break; // a permanent client error
        await sleep(rng.nextBounded(BASE_DELAY_MS * 2 ** n) + 1); // full jitter
    }
    db.exec("UPDATE deliveries SET state = 'dead' WHERE id = ? AND state = 'pending'", [d.id]);
    Metrics.counter("webhook_dead_letters_total", 1, { sub: sub.id });
    return "dead";
}
async function dispatch() {
    fanOut();
    const pending = db.query(
        "SELECT d.* FROM deliveries d JOIN subscriptions s ON s.id = d.sub WHERE d.state = 'pending' AND s.active = 1 ORDER BY d.event_seq",
    );
    await Promise.all(
        pending.map(async (d) => {
            if (!lanes.has(d.sub)) lanes.set(d.sub, new Semaphore(1));
            return lanes.get(d.sub).run(() => deliver(d)); // in order per endpoint, parallel across endpoints
        }),
    );
}
function replayDead(subId) {
    return db.exec("UPDATE deliveries SET state = 'pending', attempts = 0 WHERE sub = ? AND state = 'dead'", [subId]);
}

// =============================================================================
// Receivers (other companies' endpoints), each verifying signatures
// =============================================================================
function receiver(name, secret, behaviour) {
    const seen = new LRU(10_000, { ttlMs: 3600_000 });
    const got = [];
    const app = new App({ port: 0 });
    app.post("/hooks", (req) => {
        const m = /^t=(\d+),v1=([0-9a-f]{64})$/.exec(req.headers["webhook-signature"] || "");
        if (!m) return { status: 400, body: "missing signature" };
        if (Math.abs(nowSec() - Number(m[1])) > 300) return { status: 400, body: "stale timestamp" };
        const h = new Hmac("sha256", HexDecode(secret));
        let genuine;
        try {
            genuine = h.verify(`${m[1]}.${req.body}`, m[2]);
        } finally {
            h.close();
        }
        if (!genuine) return { status: 401, body: "bad signature" };
        const verdict = behaviour(got.length);
        if (verdict !== 200) return { status: verdict, body: "" };
        const id = req.headers["webhook-id"];
        if (seen.has(id)) return { status: 200, body: "duplicate ignored" }; // at-least-once delivery, exactly-once effect
        seen.put(id, true);
        got.push(JSON.parse(req.body));
        return { status: 200, body: "ok" };
    });
    app.start();
    return { name, app, got, url: `http://127.0.0.1:${app.port}/hooks` };
}
const secrets = { erp: "a1".repeat(32), crm: "b2".repeat(32), legacy: "c3".repeat(32), sunset: "d4".repeat(32) };
let crmCalls = 0,
    legacyFixed = false;
const erp = receiver("erp", secrets.erp, () => 200);
const crm = receiver("crm", secrets.crm, () => (crmCalls++ % 3 === 2 ? 200 : 503)); // fails twice, then succeeds
const legacy = receiver("legacy", secrets.legacy, () => (legacyFixed ? 200 : 500)); // down until "fixed"
const sunset = receiver("sunset", secrets.sunset, () => 410);
for (const [id, r, pattern] of [
    ["erp", erp, "*"],
    ["crm", crm, "order.*"],
    ["legacy", legacy, "invoice.*"],
    ["sunset", sunset, "order.*"],
])
    db.exec("INSERT INTO subscriptions (id, url, pattern, secret) VALUES (?, ?, ?, ?)", [
        id,
        r.url,
        pattern,
        secrets[id],
    ]);

// =============================================================================
// The run
// =============================================================================
const results = {};
const ids = [];
for (let i = 0; i < 6; i++) {
    const id = placeOrder(`cust-${i}`, `${(i + 1) * 12}.50`);
    if (id) ids.push(id);
}
results["a rolled-back order leaves no event behind"] =
    placeOrder("cust-x", "0.00") === null && db.query("SELECT COUNT(*) AS n FROM outbox")[0].n === 6;
payOrder(ids[0]);
payOrder(ids[3]);
await dispatch();

const states = Object.fromEntries(
    db
        .query("SELECT sub, state, COUNT(*) AS n FROM deliveries GROUP BY sub, state")
        .map((r) => [`${r.sub}:${r.state}`, r.n]),
);
results["erp received every event, in commit order"] =
    erp.got.length === 8 && erp.got.map((e) => e.data.order ?? "").join() === [...ids, ids[0], ids[3]].join();
results[`crm got all order events despite 503s (${JSON.stringify(states)})`] =
    crm.got.length === 6 && states["crm:delivered"] === 6;
results["crm deliveries needed retries"] =
    db.query("SELECT MAX(attempts) AS a FROM deliveries WHERE sub = 'crm'")[0].a >= 3;
results["legacy's invoice events are dead-lettered after five attempts"] = states["legacy:dead"] === 2;
results["410 Gone disables the subscription"] =
    states["sunset:cancelled"] >= 5 && db.query("SELECT active FROM subscriptions WHERE id = 'sunset'")[0].active === 0;

// The operator fixes the legacy endpoint and replays its dead letters.
legacyFixed = true;
results["two dead letters queued for replay"] = replayDead("legacy") === 2;
await dispatch();
results["replayed events arrive"] = legacy.got.length === 2 && legacy.got.every((e) => e.topic === "invoice.paid");

// A forged and a stale delivery are refused by the receiver.
const body = JSON.stringify({ id: "evt_forged", topic: "order.placed", data: {} });
const forged = await fetch(erp.url, {
    method: "POST",
    body,
    headers: { "webhook-id": "evt_forged", "webhook-signature": sign("00".repeat(32), nowSec(), body) },
});
const stale = await fetch(erp.url, {
    method: "POST",
    body,
    headers: { "webhook-id": "evt_stale", "webhook-signature": sign(secrets.erp, nowSec() - 3600, body) },
});
results[`forged -> ${forged.status}, stale -> ${stale.status}`] = forged.status === 401 && stale.status === 400;
await forged.text();
await stale.text();

// At-least-once: re-sending an already delivered event has no second effect.
db.exec("UPDATE deliveries SET state = 'pending', attempts = 0 WHERE sub = 'erp' AND event_seq = 1");
await dispatch();
results["a redelivered event is deduplicated by the receiver"] = erp.got.length === 8;
const noisiest = new ZSet();
for (const [id, h] of health) {
    const delivered = h.outcomes.count("200") + h.outcomes.count("204") + h.outcomes.count("202");
    noisiest.add(id, h.outcomes.entrySet().reduce((n, [, c]) => n + c, 0) / Math.max(1, delivered));
}
const worst = noisiest.range(0, 1, { reverse: true }).map(([id]) => id);
results[`the health report ranks the broken and flaky endpoints first (${worst.join(", ")})`] =
    worst.join() === "legacy,crm";
results["erp's outcomes are all successes"] = health
    .get("erp")
    .outcomes.elementSet()
    .every((s) => /^2/.test(s));

const attemptsTotal = db.query("SELECT COUNT(*) AS n FROM attempts")[0].n;
const latency = db.query("SELECT AVG(ms) AS avg, MAX(ms) AS max FROM attempts WHERE status = 200")[0];
print("webhook-platform");
print(
    `  events       ${db.query("SELECT COUNT(*) AS n FROM outbox")[0].n} in the outbox (one rolled back with its order)`,
);
print(
    `  deliveries   ${JSON.stringify(Object.fromEntries(db.query("SELECT state, COUNT(*) AS n FROM deliveries GROUP BY state").map((r) => [r.state, r.n])))}`,
);
print(
    `  attempts     ${attemptsTotal}, successful latency avg ${latency.avg.toFixed(1)} ms / max ${latency.max.toFixed(1)} ms`,
);
print(
    `  receivers    erp ${erp.got.length}, crm ${crm.got.length}, legacy ${legacy.got.length} (after replay), sunset disabled`,
);
for (const r of [erp, crm, legacy, sunset]) r.app.close();
db.close();
removeAll(work);
check(results);

function check(results) {
    const names = Object.keys(results);
    const failed = names.filter((name) => !results[name]);
    for (const name of failed) print("FAIL " + name);
    if (failed.length) throw new Error(`${failed.length} of ${names.length} checks failed`);
}
