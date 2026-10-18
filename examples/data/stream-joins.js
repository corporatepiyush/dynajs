// Stream joins — an event-time stream processor with watermarks, an interval join, tumbling and session windows,
// duplicate suppression and checkpoint/restore, verified against a batch computation over the same events.
//
// WHAT IT IS
//   Flink/Kafka-Streams semantics in a few hundred lines:
//
//   input       orders and payments arrive out of order (bounded delay) with 4% duplicate deliveries and a trickle
//               of very late events; event time, not arrival time, drives everything
//   watermark   max event time seen minus the allowed lateness; state older than the watermark is final
//   join        payment within 15 minutes after its order (interval join keyed by order id); orders whose window
//               closes unpaid are emitted as alerts by an IndexedHeap of deadlines popped up to the watermark
//   windows     5-minute tumbling revenue per region, emitted once when the watermark passes the window end
//               (SortedMap of open windows); per-user session windows with a 10-minute gap
//   late data   events behind the watermark go to a side output, never into closed results
//   dedup       a BloomFilter answers "definitely new" for almost every event; only its positives are checked
//               against the exact id map, so duplicates are dropped without trusting a probabilistic answer
//   checkpoint  operator state is serialised mid-stream (CBOR + structure serialize), the operator is rebuilt
//               from bytes, and the resumed run must produce exactly the outputs of an uninterrupted one
//   oracle      a batch job over the de-duplicated, on-time events computes joins, windows and sessions directly
//
// MODULES  dyna:structures (IndexedHeap, SortedMap, BloomFilter, Multiset) · dyna:serialize (CBOREncode, CBORDecode)
//          · dyna:random · dyna:cli · Array extensions
//
// RUN      dynajs examples/data/stream-joins.js

import { IndexedHeap, SortedMap, BloomFilter, Multiset } from "dyna:structures";
import { CBOREncode, CBORDecode } from "dyna:serialize";
import { Random } from "dyna:random";
import { Table } from "dyna:cli";

const results = {};
const rng = new Random(2015);
const MIN = 60_000,
    LATENESS = 2 * MIN,
    JOIN_WINDOW = 15 * MIN,
    TUMBLE = 5 * MIN,
    SESSION_GAP = 10 * MIN;
const REGIONS = ["eu", "us", "apac"];

// =============================================================================
// Event generation: event time vs arrival time
// =============================================================================
const events = [];
let eventId = 0;
const start = Date.UTC(2026, 9, 25, 9);
for (let i = 0; i < 6000; i++) {
    const t = start + Math.floor(rng.nextFloat() * 4 * 3600_000);
    const user = `u${rng.nextBounded(400)}`,
        region = REGIONS[rng.nextBounded(3)];
    const order = {
        id: ++eventId,
        kind: "order",
        orderId: i,
        user,
        region,
        amount: Math.round(rng.exponential(1 / 60) * 100) / 100,
        t,
    };
    events.push(order);
    if (rng.nextFloat() < 0.85) {
        const lag = rng.nextFloat() < 0.9 ? rng.nextBounded(10 * MIN) : JOIN_WINDOW + rng.nextBounded(10 * MIN);
        events.push({ id: ++eventId, kind: "payment", orderId: i, user, region, amount: order.amount, t: t + lag });
    }
}
const deliveries = [];
for (const e of events) {
    const veryLate = rng.nextFloat() < 0.005;
    const arrival = e.t + (veryLate ? LATENESS + MIN + rng.nextBounded(20 * MIN) : rng.nextBounded(LATENESS));
    deliveries.push({ e, arrival });
    if (rng.nextFloat() < 0.04) deliveries.push({ e, arrival: arrival + rng.nextBounded(3 * MIN) });
}
deliveries.sort((a, b) => a.arrival - b.arrival);

// =============================================================================
// The operator
// =============================================================================
class Operator {
    constructor() {
        this.watermark = -Infinity;
        this.maxSeen = -Infinity;
        this.seenFilter = new BloomFilter(200_000, 7);
        this.seen = new Map();
        this.orders = new Map();
        this.paidEarly = new Map();
        this.deadlines = new IndexedHeap();
        this.windows = new SortedMap();
        this.sessions = new Map();
        this.out = { joined: [], unpaid: [], windows: [], sessions: [], late: 0, dupes: 0, exactChecks: 0 };
    }
    process(e) {
        if (this.seenFilter.mayContain(String(e.id))) {
            this.out.exactChecks++;
            if (this.seen.has(e.id)) {
                this.out.dupes++;
                return;
            }
        }
        this.seenFilter.add(String(e.id));
        this.seen.set(e.id, e.t);
        if (e.t <= this.watermark) {
            this.out.late++;
            return;
        }
        if (e.kind === "order") {
            this.orders.set(e.orderId, e);
            const early = this.paidEarly.get(e.orderId);
            if (early && early.t >= e.t && early.t <= e.t + JOIN_WINDOW) {
                this.emitJoin(e, early);
                this.paidEarly.delete(e.orderId);
            } else this.deadlines.push(e.orderId, e.t + JOIN_WINDOW);
            const w = Math.floor(e.t / TUMBLE) * TUMBLE;
            const win = this.windows.get(w) ?? (this.windows.set(w, { eu: 0, us: 0, apac: 0 }), this.windows.get(w));
            win[e.region] = Math.round((win[e.region] + e.amount) * 100) / 100;
            this.session(e);
        } else {
            const o = this.orders.get(e.orderId);
            if (o && this.deadlines.has(e.orderId) && e.t >= o.t && e.t <= o.t + JOIN_WINDOW) {
                this.emitJoin(o, e);
                this.deadlines.delete(e.orderId);
            } else if (!o) this.paidEarly.set(e.orderId, e);
        }
        this.maxSeen = Math.max(this.maxSeen, e.t);
        this.advance(this.maxSeen - LATENESS);
    }
    emitJoin(o, p) {
        this.out.joined.push([o.orderId, p.t - o.t]);
    }
    session(e) {
        const s = this.sessions.get(e.user);
        if (s && e.t - s.end <= SESSION_GAP && e.t >= s.start - SESSION_GAP) {
            s.start = Math.min(s.start, e.t);
            s.end = Math.max(s.end, e.t);
            s.n++;
        } else {
            if (s) this.out.sessions.push([e.user, s.start, s.end, s.n]);
            this.sessions.set(e.user, { start: e.t, end: e.t, n: 1 });
        }
    }
    advance(wm) {
        if (wm <= this.watermark) return;
        this.watermark = wm;
        for (const id of this.deadlines.popUntil(wm)) {
            this.out.unpaid.push(id);
            this.orders.delete(id);
        }
        for (const [w, agg] of this.windows.rangeQuery(-Infinity, wm - TUMBLE)) {
            this.out.windows.push([w, agg.eu, agg.us, agg.apac]);
            this.windows.delete(w);
        }
        for (const [user, s] of this.sessions)
            if (s.end + SESSION_GAP <= wm) {
                this.out.sessions.push([user, s.start, s.end, s.n]);
                this.sessions.delete(user);
            }
        for (const [id, p] of this.paidEarly) if (p.t + JOIN_WINDOW < wm) this.paidEarly.delete(id);
    }
    flush() {
        this.advance(Infinity);
    }
    checkpoint() {
        return CBOREncode({
            watermark: this.watermark,
            maxSeen: this.maxSeen,
            seenFilter: this.seenFilter.serialize(),
            seen: [...this.seen],
            orders: [...this.orders.values()],
            paidEarly: [...this.paidEarly.values()],
            deadlines: this.deadlines.serialize(),
            windows: this.windows.keys().map((k) => [k, this.windows.get(k)]),
            sessions: [...this.sessions],
            out: this.out,
        });
    }
    static restore(bytes) {
        const s = CBORDecode(bytes),
            op = new Operator();
        op.watermark = s.watermark;
        op.maxSeen = s.maxSeen;
        op.seenFilter = BloomFilter.deserialize(s.seenFilter);
        op.seen = new Map(s.seen);
        op.orders = new Map(s.orders.map((o) => [o.orderId, o]));
        op.paidEarly = new Map(s.paidEarly.map((p) => [p.orderId, p]));
        op.deadlines = IndexedHeap.deserialize(s.deadlines);
        for (const [k, v] of s.windows) op.windows.set(k, v);
        op.sessions = new Map(s.sessions);
        op.out = s.out;
        return op;
    }
}

// =============================================================================
// Run: straight through, and with a checkpoint/restore in the middle
// =============================================================================
const t0 = performance.now();
const straight = new Operator();
for (const d of deliveries) straight.process(d.e);
straight.flush();
const runMs = performance.now() - t0;

let resumed = new Operator();
const half = deliveries.length >> 1;
for (let i = 0; i < half; i++) resumed.process(deliveries[i].e);
const snapshot = resumed.checkpoint();
resumed = Operator.restore(snapshot);
for (let i = half; i < deliveries.length; i++) resumed.process(deliveries[i].e);
resumed.flush();
const canon = (o) => JSON.stringify([o.joined, o.unpaid, o.windows, o.sessions.slice().sort(), o.late, o.dupes]);
results["checkpoint/restore produces identical outputs"] = canon(straight.out) === canon(resumed.out);

// =============================================================================
// Oracle: batch over unique, on-time events
// =============================================================================
const out = straight.out;
const lateIds = new Set();
{
    // Replay arrival order to know which events were behind the watermark when they arrived.
    let maxSeen = -Infinity,
        wm = -Infinity;
    const first = new Set();
    for (const { e } of deliveries) {
        if (first.has(e.id)) continue;
        first.add(e.id);
        if (e.t <= wm) {
            lateIds.add(e.id);
            continue;
        }
        maxSeen = Math.max(maxSeen, e.t);
        wm = maxSeen - LATENESS;
    }
}
const onTime = events.filter((e) => !lateIds.has(e.id));
const batchOrders = new Map(onTime.filter((e) => e.kind === "order").map((e) => [e.orderId, e]));
const batchJoined = new Set(),
    batchUnpaid = [];
for (const p of onTime.filter((e) => e.kind === "payment")) {
    const o = batchOrders.get(p.orderId);
    if (o && p.t >= o.t && p.t <= o.t + JOIN_WINDOW) batchJoined.add(o.orderId);
}
for (const o of batchOrders.values()) if (!batchJoined.has(o.orderId)) batchUnpaid.push(o.orderId);
const batchWindows = new Map();
for (const o of batchOrders.values()) {
    const w = Math.floor(o.t / TUMBLE) * TUMBLE;
    const agg = batchWindows.get(w) ?? (batchWindows.set(w, { eu: 0, us: 0, apac: 0 }), batchWindows.get(w));
    agg[o.region] = Math.round((agg[o.region] + o.amount) * 100) / 100;
}
const dupDeliveries = deliveries.length - new Set(deliveries.map((d) => d.e.id)).size;
results["every duplicate delivery is dropped"] = out.dupes === dupDeliveries;
results["Bloom filter sent only a small fraction to the exact check"] = out.exactChecks < dupDeliveries * 1.2 + 50;
results["late events go to the side output"] = out.late === lateIds.size && out.late > 0;
results["interval join equals the batch join"] =
    JSON.stringify(out.joined.map(([id]) => id).sort((a, b) => a - b)) ===
    JSON.stringify([...batchJoined].sort((a, b) => a - b));
results["every joined payment is inside the 15-minute window"] = out.joined.every(
    ([, dt]) => dt >= 0 && dt <= JOIN_WINDOW,
);
results["unpaid alerts equal the batch anti-join"] =
    JSON.stringify(out.unpaid.slice().sort((a, b) => a - b)) === JSON.stringify(batchUnpaid.sort((a, b) => a - b));
results["every tumbling window is emitted once with batch totals"] =
    out.windows.length === batchWindows.size &&
    out.windows.every(([w, eu, us, apac]) => {
        const b = batchWindows.get(w);
        return b && b.eu === eu && b.us === us && b.apac === apac;
    });
results["windows are emitted in event-time order"] = out.windows.every(
    (w, i) => i === 0 || w[0] > out.windows[i - 1][0],
);
{
    const byUser = Object.groupBy(
        onTime.filter((e) => e.kind === "order"),
        (e) => e.user,
    );
    let batchSessions = 0;
    for (const evs of Object.values(byUser)) {
        const ts = evs.map((e) => e.t).sort((a, b) => a - b);
        batchSessions++;
        for (let i = 1; i < ts.length; i++) if (ts[i] - ts[i - 1] > SESSION_GAP) batchSessions++;
    }
    results["session count is within 2% of the batch sessionisation"] =
        Math.abs(out.sessions.length - batchSessions) / batchSessions < 0.02;
    results["sessions cover every on-time order"] = out.sessions.reduce((s, x) => s + x[3], 0) === batchOrders.size;
}
const status = new Multiset();
for (const d of deliveries) status.add(d.e.kind);
print(
    Table(
        [
            [
                "deliveries (orders / payments)",
                `${status.count("order").format()} / ${status.count("payment").format()}`,
            ],
            ["duplicates dropped", out.dupes.format()],
            ["late (side output)", out.late.format()],
            ["joined / unpaid", `${out.joined.length.format()} / ${out.unpaid.length.format()}`],
            ["windows / sessions", `${out.windows.length} / ${out.sessions.length.format()}`],
            ["checkpoint size", snapshot.length.bytes()],
            ["throughput", `${Math.round(deliveries.length / (runMs / 1000)).format()} events/s`],
        ],
        { head: ["stream", "value"] },
    ),
);

check(results);

function check(results) {
    const names = Object.keys(results);
    const failed = names.filter((name) => !results[name]);
    for (const name of failed) print("FAIL " + name);
    if (failed.length) throw new Error(`${failed.length} of ${names.length} checks failed`);
}
