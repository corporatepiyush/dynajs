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
//               (SortedMap of open windows); per-user session windows with a 10-minute gap, MERGING: an
//               out-of-order event that falls within the gap of two open sessions bridges them into one, so the
//               result equals batch sessionisation exactly
//   hopping     15-minute windows every 5 minutes (revenue and p95 order size), built from 5-minute PANES: each
//               pane keeps a cent sum and a TDigest, and a window is the merge of its three panes, so an order is
//               added once instead of three times; the p95 is checked against the exact rank in the batch
//   late data   events behind the watermark go to a side output, never into closed results
//   dedup       a BloomFilter answers "definitely new" for almost every event; only its positives are checked
//               against the exact id map, so duplicates are dropped without trusting a probabilistic answer
//   checkpoint  operator state is serialised mid-stream (CBOR + structure serialize), the operator is rebuilt
//               from bytes, and the resumed run must produce exactly the outputs of an uninterrupted one -- at the
//               midpoint, and again in one run restored seven times (first event, last event, mid-window)
//   oracle      a batch job over the de-duplicated, on-time events computes joins, windows and sessions directly
//
// MODULES  dyna:structures (IndexedHeap, SortedMap, BloomFilter, Multiset, TDigest) · dyna:serialize (CBOREncode, CBORDecode)
//          · dyna:random · dyna:cli · Array extensions
//
// RUN      dynajs examples/data/stream-joins.js

import { IndexedHeap, SortedMap, BloomFilter, Multiset, TDigest } from "dyna:structures";
import { CBOREncode, CBORDecode } from "dyna:serialize";
import { Random } from "dyna:random";
import { Table } from "dyna:cli";

const results = {};
const rng = new Random(2015);
const MIN = 60_000,
    LATENESS = 2 * MIN,
    JOIN_WINDOW = 15 * MIN,
    TUMBLE = 5 * MIN,
    SESSION_GAP = 10 * MIN,
    HOP = 5 * MIN,
    HOP_SIZE = 15 * MIN;
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
const arrivals = [];
for (const e of events) {
    const veryLate = rng.nextFloat() < 0.005;
    const arrival = e.t + (veryLate ? LATENESS + MIN + rng.nextBounded(20 * MIN) : rng.nextBounded(LATENESS));
    arrivals.push({ e, arrival });
    if (rng.nextFloat() < 0.04) arrivals.push({ e, arrival: arrival + rng.nextBounded(3 * MIN) });
}
// One user's three orders are planted so that the middle one bridges two sessions: it is exactly one gap after the
// first and one minute before the third, and it arrives 90 s late -- after the third has opened its own session, but
// still ahead of the watermark (lateness 2 min). Random arrivals almost never produce this.
const BRIDGE_AT = start + 2 * 3600_000;
for (const [dt, delay] of [
    [0, 0],
    [SESSION_GAP + MIN, 0],
    [SESSION_GAP, 1.5 * MIN],
]) {
    const e = {
        id: ++eventId,
        kind: "order",
        orderId: 6000 + events.length,
        user: "u400",
        region: "eu",
        amount: 10,
        t: BRIDGE_AT + dt,
    };
    events.push(e);
    arrivals.push({ e, arrival: e.t + delay });
}
const deliveries = arrivals.sortBy((d) => d.arrival);

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
        this.sessions = new Map(); // user -> open sessions, pairwise more than SESSION_GAP apart
        this.panes = new SortedMap(); // pane start -> { cents, digest }
        this.lastHop = -Infinity; // start of the last hopping window emitted
        this.sessionDue = new IndexedHeap(); // user number -> watermark at which the user's earliest session closes
        this.out = {
            joined: [],
            unpaid: [],
            windows: [],
            sessions: [],
            hops: [],
            late: 0,
            dupes: 0,
            exactChecks: 0,
        };
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
            const p = Math.floor(e.t / HOP) * HOP;
            const pane =
                this.panes.get(p) ?? (this.panes.set(p, { cents: 0, digest: new TDigest() }), this.panes.get(p));
            pane.cents += Math.round(e.amount * 100);
            pane.digest.add(e.amount);
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
    // Every open session within the gap of the event merges with it (one on either side: a bridge). The survivors
    // stay more than the gap apart from each other and from the merged one, so no second pass is needed.
    session(e) {
        const merged = { start: e.t, end: e.t, n: 1 },
            keep = [];
        for (const s of this.sessions.get(e.user) ?? [])
            if (e.t >= s.start - SESSION_GAP && e.t <= s.end + SESSION_GAP) {
                merged.start = Math.min(merged.start, s.start);
                merged.end = Math.max(merged.end, s.end);
                merged.n += s.n;
            } else keep.push(s);
        keep.push(merged);
        this.sessions.set(e.user, keep);
        this.sessionDue.push(+e.user.slice(1), Math.min(...keep.map((s) => s.end + SESSION_GAP)));
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
        // a session can no longer grow once the watermark is a gap past its end: any later event is behind it. Only
        // the users whose earliest session is due are visited (an IndexedHeap keyed by user).
        for (const uid of this.sessionDue.popUntil(wm)) {
            const user = `u${uid}`,
                live = this.sessions.get(user).filter((s) => {
                    if (s.end + SESSION_GAP > wm) return true;
                    this.out.sessions.push([user, s.start, s.end, s.n]);
                    return false;
                });
            if (!live.length) this.sessions.delete(user);
            else {
                this.sessions.set(user, live);
                this.sessionDue.push(uid, Math.min(...live.map((s) => s.end + SESSION_GAP)));
            }
        }
        // hopping windows that end at or before the watermark, in start order; a pane p lies in the windows starting
        // at p - 10, p - 5 and p, and the first of them closes when the watermark reaches p + HOP
        const starts = new Set();
        for (const [p] of this.panes.rangeQuery(-Infinity, wm - HOP))
            for (let s = p - HOP_SIZE + HOP; s <= p; s += HOP)
                if (s > this.lastHop && s + HOP_SIZE <= wm) starts.add(s);
        for (const s of [...starts].sort((a, b) => a - b)) {
            const digest = new TDigest();
            let cents = 0;
            for (let p = s; p < s + HOP_SIZE; p += HOP) {
                const pane = this.panes.get(p);
                if (!pane) continue;
                cents += pane.cents;
                digest.merge(pane.digest);
            }
            this.out.hops.push([s, cents, digest.count, digest.quantile(0.95)]);
            this.lastHop = s;
        }
        // a pane is done once the window that starts at it has been emitted
        for (const [p] of this.panes.rangeQuery(-Infinity, this.lastHop)) this.panes.delete(p);
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
            panes: this.panes.keys().map((k) => [k, this.panes.get(k).cents, this.panes.get(k).digest.serialize()]),
            lastHop: this.lastHop,
            sessionDue: this.sessionDue.serialize(),
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
        for (const [k, cents, digest] of s.panes) op.panes.set(k, { cents, digest: TDigest.deserialize(digest) });
        op.lastHop = s.lastHop;
        op.sessionDue = IndexedHeap.deserialize(s.sessionDue);
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
// A TDigest flushes its insert buffer when it is serialized (or queried), and a flushed digest compresses later
// inserts differently: a checkpointed run's p95 estimates differ slightly from an uninterrupted run's, while every
// count, sum and the restore itself are exact. So the p95 is compared exactly everywhere except here, where each
// run's p95 is held to the batch's rank bound instead (below).
const canon = (o) =>
    JSON.stringify([
        o.joined,
        o.unpaid,
        o.windows,
        o.sessions.slice().sort(),
        o.hops.map((h) => h.slice(0, 3)),
        o.late,
        o.dupes,
        o.exactChecks,
    ]);
results["checkpoint/restore produces identical outputs"] = canon(straight.out) === canon(resumed.out);
{
    // one run, checkpointed and rebuilt from bytes at seven points: before the first event, after it, mid-stream at
    // arbitrary positions, and after the last (just before the final flush)
    // (the cut just after the last order matters: users with an open session there never order again, so their
    // sessions close only through the restored expiry heap)
    const lastOrder = deliveries.findLastIndex((d) => d.e.kind === "order") + 1,
        cuts = [0, 1, 777, 2501, half + 13, lastOrder, deliveries.length - 2, deliveries.length];
    let op = new Operator(),
        i = 0;
    for (const cut of cuts) {
        for (; i < cut; i++) op.process(deliveries[i].e);
        op = Operator.restore(op.checkpoint());
    }
    op.flush();
    results[`restored at ${cuts.length} points in one run, the outputs are identical`] =
        canon(op.out) === canon(straight.out);
    var restoredRuns = [resumed.out, op.out];
}

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
// the input must be the one described: in arrival order, so only the very late trickle (~0.5%) is behind the
// watermark -- an operator fed some other order agrees with a batch replaying that same order
results[`late events go to the side output (${out.late}, ~0.5% of events)`] =
    out.late === lateIds.size &&
    out.late > 0 &&
    out.late < events.length * 0.01 &&
    deliveries.every((d, i) => i === 0 || d.arrival >= deliveries[i - 1].arrival);
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
    const batchList = [];
    for (const [user, evs] of Object.entries(byUser)) {
        const ts = evs.map((e) => e.t).sort((a, b) => a - b);
        let start = ts[0],
            n = 1;
        for (let i = 1; i <= ts.length; i++)
            if (i === ts.length || ts[i] - ts[i - 1] > SESSION_GAP) {
                batchList.push(JSON.stringify([user, start, ts[i - 1], n]));
                start = ts[i];
                n = 1;
            } else n++;
    }
    const streamList = out.sessions.map((x) => JSON.stringify(x));
    results[`sessions equal the batch sessionisation exactly (${out.sessions.length} of ${batchSessions})`] =
        batchList.length === batchSessions && JSON.stringify(streamList.sort()) === JSON.stringify(batchList.sort());
    results["the out-of-order order bridges two open sessions into one"] = out.sessions.some(
        ([u, a, b, n]) => u === "u400" && a === BRIDGE_AT && b === BRIDGE_AT + SESSION_GAP + MIN && n === 3,
    );
    results["sessions cover every on-time order"] = out.sessions.reduce((s, x) => s + x[3], 0) === batchOrders.size;
}
{
    // hopping windows: the batch puts each on-time order in the three windows that contain it, then sums and ranks
    const byHop = new Map();
    for (const o of batchOrders.values())
        for (let s = Math.floor(o.t / HOP) * HOP - HOP_SIZE + HOP; s <= o.t; s += HOP)
            byHop.getOrInsertComputed(s, () => []).push(o.amount);
    const sorted = new Map([...byHop].map(([s, a]) => [s, Float64Array.from(a).sort()]));
    // the rank error of an estimate: the fraction of amounts at or below it against 0.95, less one element's share
    const rankError = (hops) =>
        hops.reduce((worst, [s, , , p95]) => {
            const a = sorted.get(s),
                below = a.filter((x) => x <= p95).length / a.length;
            return Math.max(worst, Math.abs(below - 0.95) - 1 / a.length);
        }, 0);
    const got = new Map(out.hops.map((h) => [h[0], h]));
    const exact =
        got.size === byHop.size &&
        [...byHop].every(([s, amounts]) => {
            const h = got.get(s);
            return h && h[1] === amounts.reduce((a, x) => a + Math.round(x * 100), 0) && h[2] === amounts.length;
        });
    const worstRank = rankError(out.hops),
        worstRestored = Math.max(...restoredRuns.map((o) => rankError(o.hops)));
    results[`every hopping window has the batch's orders and revenue to the cent (${got.size} windows)`] = exact;
    results[`and its p95 from merged pane digests is within 1% in rank (worst ${worstRank.toFixed(4)})`] =
        worstRank <= 0.01;
    results[`so is the p95 of every checkpointed run (worst ${worstRestored.toFixed(4)})`] = worstRestored <= 0.01;
    results["hopping windows are emitted in start order"] = out.hops.every(
        (h, i) => i === 0 || h[0] > out.hops[i - 1][0],
    );
    // a window's revenue is the sum of the three tumbling windows it spans (the two operators agree)
    const tumble = new Map(out.windows.map(([w, eu, us, apac]) => [w, Math.round((eu + us + apac) * 100)]));
    results["each hopping window's revenue is the sum of the three tumbling windows it spans"] = out.hops.every(
        ([s, cents]) => Math.abs(cents - [0, 1, 2].reduce((a, k) => a + (tumble.get(s + k * HOP) ?? 0), 0)) <= 3,
    );
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
            ["hopping windows (15 min every 5)", out.hops.length],
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
