// Booking system — a hotel reservation API with per-room occupancy sets, expiring holds, idempotent confirmation,
// cancellations and occupancy-based pricing, stress-tested by concurrent clients and audited for double bookings.
//
// WHAT IT IS
//   The inventory core of a booking site, where the one unforgivable bug is selling the same room-night twice:
//
//   inventory   each room's booked nights in a RangeSet (half-open [checkIn, checkOut) day ranges); "is room free for
//               these nights" is one intersects() call; all bookings also go into an IntervalTree (closed night
//               intervals [checkIn, checkOut - 1]) for audits
//   holds       a hold reserves a room for 10 minutes; expiries sit in an IndexedHeap and are swept before every
//               request, so an abandoned cart gives the room back on time
//   confirm     POST /bookings with an Idempotency-Key: the first result is cached in an LRU and replayed on retry,
//               so a client retrying after a timeout cannot book twice
//   occupancy   nightly sold counts in a Fenwick tree used as a difference array (range add, point query); the price
//               of a night rises with occupancy; totals use dyna:decimal Money
//   stress      400 clients race for the same peak week over HTTP; the single-threaded event loop makes each
//               check-and-reserve atomic; afterwards a sweep over every room's bookings proves no overlap, and the
//               Fenwick counts are recomputed from the bookings themselves
//
// MODULES  dyna:http (App, fetch) · dyna:structures (RangeSet, IntervalTree, IndexedHeap, LRU, Fenwick, Multiset) ·
//          dyna:decimal (Money) · dyna:random · dyna:cli · Array/Number extensions
//
// RUN      dynajs examples/web/booking-system.js

import { App } from "dyna:http";
import { RangeSet, IntervalTree, IndexedHeap, LRU, Fenwick, Multiset } from "dyna:structures";
import { Money } from "dyna:decimal";
import { Random } from "dyna:random";
import { Table } from "dyna:cli";

const results = {};
const rng = new Random(1890);
const DAYS = 120,
    HOLD_MS = 10 * 60_000;
const TYPES = {
    single: { count: 30, base: 9000 },
    double: { count: 50, base: 12000 },
    suite: { count: 10, base: 30000 },
};

// =============================================================================
// Inventory
// =============================================================================
const rooms = [];
for (const [type, t] of Object.entries(TYPES))
    for (let i = 0; i < t.count; i++) rooms.push({ id: rooms.length, type, nights: new RangeSet() });
const holds = new Map(),
    holdExpiry = new IndexedHeap(),
    bookings = new Map(),
    audit = new IntervalTree();
const sold = Object.fromEntries(Object.keys(TYPES).map((t) => [t, new Fenwick(DAYS + 2)]));
const idempotency = new LRU(10_000);
let now = 0,
    nextId = 1;
const stats = new Multiset();

function sweepHolds() {
    for (const id of holdExpiry.popUntil(now)) {
        const h = holds.get(id);
        if (!h) continue;
        rooms[h.room].nights.remove(h.from, h.to);
        holds.delete(id);
        stats.add("hold-expired");
    }
}
const occupied = (type, night) => sold[type].prefixSum(night);
function priceFor(type, from, to) {
    let cents = 0;
    for (let n = from; n < to; n++) {
        const occ = occupied(type, n) / TYPES[type].count;
        cents += Math.round(TYPES[type].base * (1 + Math.max(0, occ - 0.5)));
    }
    return Money.fromMinor(cents, "EUR");
}
function placeHold(type, from, to) {
    sweepHolds();
    if (!(from >= 0 && to <= DAYS && from < to) || !TYPES[type]) return { status: 400, body: "bad request" };
    const room = rooms.find((r) => r.type === type && !r.nights.intersects(from, to));
    if (!room) {
        stats.add("sold-out");
        return { status: 409, body: "sold out" };
    }
    room.nights.add(from, to);
    const id = nextId++;
    holds.set(id, { room: room.id, from, to, type, price: priceFor(type, from, to) });
    holdExpiry.push(id, now + HOLD_MS);
    stats.add("hold");
    return { status: 201, body: JSON.stringify({ hold: id, room: room.id, price: holds.get(id).price.toString() }) };
}
function confirm(holdId, key) {
    const cached = idempotency.get(key);
    if (cached) {
        stats.add("replayed");
        return cached;
    }
    sweepHolds();
    const h = holds.get(holdId);
    let res;
    if (!h) res = { status: 410, body: "hold expired or unknown" };
    else {
        holds.delete(holdId);
        holdExpiry.delete(holdId);
        const id = nextId++;
        bookings.set(id, { ...h, id });
        audit.insert(h.from, h.to - 1, { booking: id, room: h.room });
        sold[h.type].update(h.from, 1);
        sold[h.type].update(h.to, -1);
        stats.add("booked");
        res = {
            status: 201,
            body: JSON.stringify({ booking: id, room: h.room, nights: h.to - h.from, total: h.price.toString() }),
        };
    }
    idempotency.set(key, res);
    return res;
}
function cancel(bookingId) {
    const b = bookings.get(bookingId);
    if (!b) return { status: 404, body: "no such booking" };
    rooms[b.room].nights.remove(b.from, b.to);
    sold[b.type].update(b.from, -1);
    sold[b.type].update(b.to, 1);
    bookings.delete(bookingId);
    stats.add("cancelled");
    return { status: 200, body: JSON.stringify({ cancelled: bookingId }) };
}

const app = new App({ port: 0 });
app.post("/holds", (req) => {
    const b = JSON.parse(req.body);
    return placeHold(b.type, b.from, b.to);
});
app.post("/bookings", (req) => confirm(JSON.parse(req.body).hold, req.headers["idempotency-key"] ?? `anon-${nextId}`));
app.del("/bookings/:id", (req) => cancel(Number(req.params.id)));
app.get("/availability", (req) => {
    sweepHolds();
    const from = Number(req.query.from),
        to = Number(req.query.to);
    return Object.fromEntries(
        Object.keys(TYPES).map((t) => [t, rooms.filter((r) => r.type === t && !r.nights.intersects(from, to)).length]),
    );
});
app.start();
const base = `http://127.0.0.1:${app.port}`;
const post = (path, body, headers = {}) => fetch(base + path, { method: "POST", body: JSON.stringify(body), headers });

// =============================================================================
// Stress: 400 clients race for the peak week
// =============================================================================
const outcomes = await Promise.all(
    Array.from({ length: 400 }, async (_, c) => {
        const type = ["single", "double", "suite"][c % 3];
        const from = 50 + rng.nextBounded(4),
            to = from + 2 + rng.nextBounded(4);
        const r = await post("/holds", { type, from, to });
        if (r.status !== 201) return { held: false };
        const { hold } = await r.json();
        if (c % 5 === 0) return { held: true, abandoned: true };
        const key = `client-${c}-hold-${hold}`;
        const first = await post("/bookings", { hold }, { "idempotency-key": key });
        const retry = c % 7 === 0 ? await post("/bookings", { hold }, { "idempotency-key": key }) : null;
        const a = await first.json(),
            b = retry ? await retry.json() : null;
        return { held: true, booking: a.booking, retrySame: retry ? b.booking === a.booking : null };
    }),
);
const booked = outcomes.filter((o) => o.booking);
results["retries with the same Idempotency-Key return the same booking"] =
    outcomes.filter((o) => typeof o.retrySame === "boolean").every((o) => o.retrySame) && stats.count("replayed") > 0;
results["sold-out clients are refused with 409"] = stats.count("sold-out") > 0;

function noDoubleBooking() {
    const byRoom = new Map();
    for (const b of bookings.values()) (byRoom.get(b.room) ?? byRoom.set(b.room, []).get(b.room)).push(b);
    for (const list of byRoom.values()) {
        list.sort((x, y) => x.from - y.from);
        for (let i = 1; i < list.length; i++) if (list[i].from < list[i - 1].to) return false;
    }
    return true;
}
results["no room-night is ever sold twice"] = noDoubleBooking();
results["the audit tree sees every booking on the peak night"] =
    audit.at(52).filter(([, , v]) => bookings.has(v.booking)).length ===
    [...bookings.values()].filter((b) => b.from <= 52 && b.to > 52).length;

// =============================================================================
// Abandoned holds expire and return inventory
// =============================================================================
{
    const before = await (await fetch(`${base}/availability?from=50&to=60`)).json();
    now += HOLD_MS + 1;
    const after = await (await fetch(`${base}/availability?from=50&to=60`)).json();
    const abandoned = outcomes.filter((o) => o.abandoned).length;
    results["abandoned holds expire after 10 minutes"] = stats.count("hold-expired") === abandoned && holds.size === 0;
    results["expired holds give rooms back"] = Object.keys(TYPES).some((t) => after[t] > before[t]) || abandoned === 0;
    const brute = Object.fromEntries(
        Object.keys(TYPES).map((t) => [
            t,
            rooms.filter(
                (r) => r.type === t && ![...bookings.values()].some((b) => b.room === r.id && b.from < 60 && b.to > 50),
            ).length,
        ]),
    );
    results["availability equals a brute-force scan of bookings"] = JSON.stringify(after) === JSON.stringify(brute);
}

// =============================================================================
// Cancellation, pricing, occupancy
// =============================================================================
{
    const victim = booked[0].booking,
        b = bookings.get(victim);
    const del = await fetch(`${base}/bookings/${victim}`, { method: "DELETE" });
    const again = await fetch(`${base}/bookings/${victim}`, { method: "DELETE" });
    results["cancel frees the nights and is not repeatable"] =
        del.status === 200 && again.status === 404 && !rooms[b.room].nights.intersects(b.from, b.to);
    const late = await post("/bookings", { hold: 999999 }, { "idempotency-key": "never-held" });
    results["confirming an unknown hold is 410"] = late.status === 410;
    const bad = await post("/holds", { type: "suite", from: 10, to: 5 });
    results["an inverted date range is 400"] = bad.status === 400;
}
{
    let ok = true;
    for (const t of Object.keys(TYPES))
        for (let n = 0; n < DAYS; n++) {
            const brute = [...bookings.values()].filter((b) => b.type === t && b.from <= n && b.to > n).length;
            if (occupied(t, n) !== brute) ok = false;
        }
    results["Fenwick nightly counts equal counts recomputed from bookings"] = ok;
    const quiet = priceFor("double", 10, 12),
        peak = priceFor("double", 52, 54);
    results["peak nights cost more than quiet nights"] = peak.cmp(quiet) > 0;
    const revenue = [...bookings.values()].reduce((s, b) => s.add(b.price), Money.fromMinor(0, "EUR"));
    print(
        Table(
            [
                ["clients", "400"],
                ["holds placed", stats.count("hold")],
                ["booked", stats.count("booked")],
                ["sold out", stats.count("sold-out")],
                ["expired holds", stats.count("hold-expired")],
                ["idempotent replays", stats.count("replayed")],
                ["cancelled", stats.count("cancelled")],
                ["peak occupancy (double, night 52)", `${occupied("double", 52)}/${TYPES.double.count}`],
                ["2 quiet vs 2 peak nights", `${quiet.format()} vs ${peak.format()}`],
                ["revenue", revenue.format()],
            ].map(([k, v]) => [k, String(v)]),
            { head: ["bookings", "value"] },
        ),
    );
}

app.close();
check(results);

function check(results) {
    const names = Object.keys(results);
    const failed = names.filter((name) => !results[name]);
    for (const name of failed) print("FAIL " + name);
    if (failed.length) throw new Error(`${failed.length} of ${names.length} checks failed`);
}
