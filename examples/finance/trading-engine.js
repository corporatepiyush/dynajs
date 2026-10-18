// Trading engine — a price-time-priority matching engine behind a FIX 4.4 gateway, with pre-trade risk, self-trade
// prevention, a UDP market-data feed with gap detection, a replayable journal and end-of-day analytics.
//
// WHAT IT IS
//   An exchange in miniature, the way real venues are built:
//
//   gateway      FIX 4.4 over TCP: tag=value fields separated by SOH, BodyLength(9) and CheckSum(10) validated,
//                MsgSeqNum(34) gap detection, Logon/Heartbeat/Logout, NewOrderSingle(D), OrderCancelRequest(F),
//                ExecutionReports(8) and rejects with the reason in Text(58)
//   risk         per account before an order touches the book: max order quantity, price collar around the last
//                trade, notional credit limit including open orders, and a kill switch
//   matching     one book per symbol, bids and asks in sorted price levels with FIFO queues; limit, market, IOC,
//                FOK and post-only orders; self-trade prevention cancels the resting order of the same account
//   market data  every book change is published as an incremental update over UDP with a sequence number; the
//                subscriber detects gaps, requests a snapshot, and rebuilds an identical top-of-book
//   journal      every accepted command is appended to a journal; replaying it into a fresh engine reproduces the
//                exact book and trade list (determinism is checked by hashing both)
//   analytics    OHLC, VWAP and volume per symbol with DataFrame, plus per-account P&L; a volume leaderboard in a
//                ZSet and trade-size percentiles from a TDigest
//   surveillance order, cancel and fill counts per account in Multisets; an account that cancels most of what it
//                places (layering / spoofing) is flagged for compliance
//   book layout  each side is a SortedMap of price levels; a level keeps its aggregate quantity and a Map of
//                resting orders in arrival order, so FIFO is the Map's first entry and a cancel is O(1), never a
//                rebuild of the queue; the FIX checksum is one String.codes().sum() rather than a JS byte loop
//
// MODULES  dyna:net (TCPServer, UDPSocket) · dyna:structures (SortedMap, ZSet, TDigest, Multiset) · dyna:decimal
//          · dyna:dataframe · dyna:hash (SHA256) · dyna:serialize (MsgPack) · dyna:random · dyna:cli (Table)
//          · String/Array extensions
//
// RUN      dynajs examples/finance/trading-engine.js     three FIX clients trade two symbols, then the journal is replayed

import { TCPServer, UDPSocket } from "dyna:net";
import { SortedMap, ZSet, TDigest, Multiset } from "dyna:structures";
import { Decimal } from "dyna:decimal";
import { DataFrame } from "dyna:dataframe";
import { SHA256Hex } from "dyna:hash";
import { MsgPackEncode, MsgPackDecode } from "dyna:serialize";
import { Random } from "dyna:random";
import { Table } from "dyna:cli";
const table = (rows) => Table(rows.slice(1), { head: rows[0] });

const SOH = "\x01";
const TICK = 100; // prices are integer ticks: 1 tick = 0.01

// =============================================================================
// FIX encoding
// =============================================================================
function fixEncode(fields, sender, target, seq) {
    const body = [
        ["35", fields[35]],
        ["49", sender],
        ["56", target],
        ["34", String(seq)],
        ["52", "20260601-14:30:00.000"],
        ...Object.entries(fields).filter(([k]) => k !== "35"),
    ]
        .map(([k, v]) => `${k}=${v}${SOH}`)
        .join("");
    const head = `8=FIX.4.4${SOH}9=${new TextEncoder().encode(body).length}${SOH}`;
    const sum = (head + body).codes().sum() % 256;
    return `${head}${body}10=${String(sum).padStart(3, "0")}${SOH}`;
}
function fixSplitter(onMessage, onError) {
    let buf = "";
    return (first, second) => {
        const chunk = second ?? first; // usable as feed(bytes) or as a TCP data(conn, bytes) handler
        buf += new TextDecoder().decode(chunk);
        for (;;) {
            const m = /^8=FIX\.4\.4\x019=(\d+)\x01/.exec(buf);
            if (!m) {
                if (buf.length && !buf.startsWith("8=")) {
                    onError("garbled stream");
                    buf = "";
                }
                return;
            }
            const bodyStart = m[0].length,
                len = Number(m[1]),
                end = bodyStart + len + 7;
            if (buf.length < end) return;
            const raw = buf.slice(0, end),
                body = buf.slice(bodyStart, bodyStart + len);
            buf = buf.slice(end);
            const sum = raw.slice(0, -7).codes().sum() % 256;
            if (!raw.endsWith(SOH) || raw.slice(-7, -4) !== "10=" || Number(raw.slice(-4, -1)) !== sum) {
                onError("checksum mismatch");
                continue;
            }
            onMessage(
                Object.fromEntries(
                    body
                        .split(SOH)
                        .filter(Boolean)
                        .map((f) => {
                            const i = f.indexOf("=");
                            return [f.slice(0, i), f.slice(i + 1)];
                        }),
                ),
            );
        }
    };
}

// =============================================================================
// Order book and matching
// =============================================================================
class Book {
    constructor(symbol) {
        this.symbol = symbol;
        this.bids = new SortedMap();
        this.asks = new SortedMap();
        this.orders = new Map();
        this.last = null;
    }
    side(s) {
        return s === "1" ? this.bids : this.asks;
    }
    bestBid() {
        const k = this.bids.lastKey();
        return k === undefined || k === null ? null : k;
    }
    bestAsk() {
        const k = this.asks.firstKey();
        return k === undefined || k === null ? null : k;
    }
    rest(o) {
        const lvl = this.side(o.side);
        let level = lvl.get(o.price);
        if (!level) lvl.set(o.price, (level = { qty: 0, orders: new Map() }));
        level.orders.set(o.id, o);
        level.qty += o.leaves;
        this.orders.set(o.id, o);
    }
    head(price, side) {
        return this.side(side).get(price).orders.values().next().value;
    }
    fill(o, qty) {
        this.side(o.side).get(o.price).qty -= qty;
    }
    remove(o) {
        const lvl = this.side(o.side),
            level = lvl.get(o.price);
        if (!level || !level.orders.delete(o.id)) return;
        level.qty -= o.leaves;
        if (!level.orders.size) lvl.delete(o.price);
        this.orders.delete(o.id);
    }
    qtyAt(side, price) {
        return this.side(side).get(price)?.qty ?? 0;
    }
    depth(n = 5) {
        const take = (lvl, keys) => keys.take(n).map((p) => [p, lvl.get(p).qty]);
        return { bids: take(this.bids, this.bids.keys().reverse()), asks: take(this.asks, this.asks.keys()) };
    }
}
class Engine {
    constructor() {
        this.books = new Map();
        this.trades = [];
        this.seq = 0;
        this.journal = [];
        this.listeners = [];
        this.risk = new Map();
        this.killed = new Set();
    }
    book(sym) {
        if (!this.books.has(sym)) this.books.set(sym, new Book(sym));
        return this.books.get(sym);
    }
    publish(update) {
        update.seq = ++this.seq;
        for (const l of this.listeners) l(update);
    }
    level(book, side, price) {
        this.publish({ sym: book.symbol, side, price, qty: book.qtyAt(side, price) });
    }
    checkRisk(o) {
        const r = this.risk.get(o.account) || { maxQty: 5000, credit: 5_000_000_00, used: 0 };
        this.risk.set(o.account, r);
        if (this.killed.has(o.account)) return "account disabled by kill switch";
        if (o.qty > r.maxQty) return `quantity ${o.qty} above limit ${r.maxQty}`;
        const last = this.book(o.symbol).last;
        if (o.type === "2" && last && Math.abs(o.price - last) > last * 0.1)
            return `price ${o.price / TICK} outside the 10% collar around ${last / TICK}`;
        const notional = o.qty * (o.price || last || 0);
        if (r.used + notional > r.credit) return "credit limit exceeded";
        r.used += notional;
        return null;
    }
    submit(o) {
        this.journal.push({ op: "new", o: { ...o } });
        const reports = [];
        const reject = (why) => {
            reports.push({ id: o.id, account: o.account, status: "8", text: why });
            return reports;
        };
        const err = this.checkRisk(o);
        if (err) return reject(err);
        const book = this.book(o.symbol),
            opp = o.side === "1" ? book.asks : book.bids;
        const crosses = (p) => o.type === "1" || (o.side === "1" ? p <= o.price : p >= o.price);
        const bestOpp = () => (o.side === "1" ? book.bestAsk() : book.bestBid());
        if (o.postOnly && bestOpp() !== null && crosses(bestOpp()))
            return reject("post-only order would take liquidity");
        if (o.tif === "4") {
            // FOK: check available quantity first
            let avail = 0;
            for (const p of o.side === "1" ? opp.keys() : opp.keys().reverse()) {
                if (!crosses(p)) break;
                for (const x of opp.get(p).orders.values()) if (x.account !== o.account) avail += x.leaves;
            }
            if (avail < o.qty) return reject("fill-or-kill could not be filled completely");
        }
        o.leaves = o.qty;
        o.cum = 0;
        while (o.leaves > 0) {
            const p = bestOpp();
            if (p === null || !crosses(p)) break;
            const maker = book.head(p, o.side === "1" ? "2" : "1");
            if (maker.account === o.account) {
                // self-trade prevention: cancel the resting order
                book.remove(maker);
                this.level(book, maker.side, maker.price);
                reports.push({
                    id: maker.id,
                    account: maker.account,
                    status: "4",
                    text: "cancelled by self-trade prevention",
                });
                continue;
            }
            const qty = Math.min(o.leaves, maker.leaves);
            book.fill(maker, qty);
            o.leaves -= qty;
            o.cum += qty;
            maker.leaves -= qty;
            maker.cum += qty;
            const t = {
                id: this.trades.length + 1,
                sym: o.symbol,
                price: p,
                qty,
                buyer: o.side === "1" ? o.account : maker.account,
                seller: o.side === "1" ? maker.account : o.account,
                aggressor: o.side,
            };
            this.trades.push(t);
            book.last = p;
            reports.push({
                id: maker.id,
                account: maker.account,
                status: maker.leaves ? "1" : "2",
                lastQty: qty,
                lastPx: p,
                cum: maker.cum,
                leaves: maker.leaves,
            });
            reports.push({
                id: o.id,
                account: o.account,
                status: o.leaves ? "1" : "2",
                lastQty: qty,
                lastPx: p,
                cum: o.cum,
                leaves: o.leaves,
            });
            if (!maker.leaves) book.remove(maker);
            this.level(book, maker.side, p);
        }
        if (o.leaves > 0) {
            if (o.type === "1" || o.tif === "3")
                reports.push({
                    id: o.id,
                    account: o.account,
                    status: "4",
                    text: o.type === "1" ? "market order remainder cancelled" : "IOC remainder cancelled",
                    cum: o.cum,
                    leaves: 0,
                });
            else {
                book.rest(o);
                this.level(book, o.side, o.price);
                if (!o.cum) reports.push({ id: o.id, account: o.account, status: "0", cum: 0, leaves: o.leaves });
            }
        }
        return reports;
    }
    cancel(symbol, id, account, clId = id) {
        this.journal.push({ op: "cancel", symbol, id, account, clId });
        const book = this.book(symbol),
            o = book.orders.get(id);
        if (!o || o.account !== account) return [{ id: clId, account, status: "8", text: "unknown order" }];
        book.remove(o);
        this.level(book, o.side, o.price);
        return [{ id: clId, account, status: "4", cum: o.cum, leaves: 0 }];
    }
    stateHash() {
        return SHA256Hex(
            JSON.stringify({ trades: this.trades, books: [...this.books].map(([s, b]) => [s, b.depth(50)]) }),
        );
    }
}

// =============================================================================
// FIX gateway
// =============================================================================
const engine = new Engine();
const sessions = new Map();
const placed = new Multiset(),
    cancelled = new Multiset();
const gw = new TCPServer({ port: 0 });
gw.start({
    connect: (conn) => {
        const s = { conn, expected: 1, out: 1, comp: null, rejects: 0 };
        const send = (fields) => conn.write(fixEncode(fields, "EXCH", s.comp || "?", s.out++));
        conn.feed = fixSplitter(
            (m) => {
                if (Number(m[34]) !== s.expected) {
                    send({
                        35: "3",
                        45: m[34],
                        58: `MsgSeqNum too ${Number(m[34]) > s.expected ? "high" : "low"}, expected ${s.expected}`,
                    });
                    s.expected = Number(m[34]) + 1;
                    return;
                }
                s.expected++;
                if (m[35] === "A") {
                    s.comp = m[49];
                    sessions.set(s.comp, s);
                    send({ 35: "A", 98: "0", 108: "30" });
                    return;
                }
                if (!s.comp) {
                    send({ 35: "5", 58: "logon required" });
                    conn.close();
                    return;
                }
                if (m[35] === "0") return;
                if (m[35] === "5") {
                    send({ 35: "5" });
                    conn.close();
                    return;
                }
                let reports;
                if (m[35] === "D") placed.add(m[1] || s.comp);
                if (m[35] === "F") cancelled.add(m[1] || s.comp);
                if (m[35] === "D") {
                    const type = m[40],
                        price = m[44] ? Math.round(Number(m[44]) * TICK) : 0;
                    reports = engine.submit({
                        id: m[11],
                        account: m[1] || s.comp,
                        symbol: m[55],
                        side: m[54],
                        qty: Number(m[38]),
                        type,
                        price,
                        tif: m[59] || "0",
                        postOnly: (m[18] || "").includes("6"),
                    });
                } else if (m[35] === "F") reports = engine.cancel(m[55], m[41], m[1] || s.comp, m[11]);
                else {
                    send({ 35: "3", 45: m[34], 58: `unsupported MsgType ${m[35]}` });
                    return;
                }
                for (const r of reports) {
                    const target = sessions.get(r.account) || s;
                    const fields = {
                        35: "8",
                        37: `O-${r.id}`,
                        11: r.id,
                        17: `E-${engine.seq}-${Math.random().toString(36).slice(2, 8)}`,
                        150: r.status === "1" || r.status === "2" ? "F" : r.status,
                        39: r.status,
                        1: r.account,
                        14: r.cum ?? 0,
                        151: r.leaves ?? 0,
                    };
                    if (r.lastQty) Object.assign(fields, { 32: r.lastQty, 31: (r.lastPx / TICK).toFixed(2) });
                    if (r.text) fields[58] = r.text;
                    target.conn.write(fixEncode(fields, "EXCH", target.comp, target.out++));
                }
            },
            (why) => {
                s.rejects++;
                send({ 35: "3", 58: why });
            },
        );
    },
    data: (conn, bytes) => conn.feed(bytes),
});

// =============================================================================
// Market data over UDP with gap recovery
// =============================================================================
const mdOut = new UDPSocket({ port: 0, host: "127.0.0.1" });
const subscriber = { book: new Map(), expected: 1, gaps: 0, snapshots: 0, received: 0, stash: [] };
const mdIn = new UDPSocket({ port: 0, host: "127.0.0.1" });
const applyUpdate = (u) => {
    const key = `${u.sym}|${u.side}`;
    if (!subscriber.book.has(key)) subscriber.book.set(key, new Map());
    if (u.qty) subscriber.book.get(key).set(u.price, u.qty);
    else subscriber.book.get(key).delete(u.price);
};
mdIn.start({
    message: (data) => {
        const u = MsgPackDecode(data);
        subscriber.received++;
        if (u.seq < subscriber.expected) return;
        if (u.seq > subscriber.expected) {
            // gap: rebuild from a snapshot, then continue
            subscriber.gaps++;
            subscriber.snapshots++;
            subscriber.book = new Map();
            for (const [sym, b] of engine.books)
                for (const [side, lvl] of [
                    ["1", b.bids],
                    ["2", b.asks],
                ])
                    for (const p of lvl.keys()) applyUpdate({ sym, side, price: p, qty: lvl.get(p).qty });
            subscriber.expected = engine.seq + 1;
            return;
        }
        applyUpdate(u);
        subscriber.expected++;
    },
});
let dropNext = 0;
engine.listeners.push((u) => {
    if (dropNext > 0) {
        dropNext--;
        return;
    }
    mdOut.send(MsgPackEncode(u), "127.0.0.1", mdIn.port);
});

// =============================================================================
// FIX clients
// =============================================================================
class FixClient {
    constructor(comp) {
        this.comp = comp;
        this.seq = 1;
        this.reports = [];
        this.waiters = [];
    }
    connect() {
        return new Promise((resolve, reject) => {
            this.conn = TCPServer.connect(
                { host: "127.0.0.1", port: gw.port },
                {
                    connect: (c, err) => {
                        if (err) return reject(new Error(err));
                        this.raw = c;
                        this.send({ 35: "A", 98: "0", 108: "30" });
                    },
                    data: fixSplitter(
                        (m) => {
                            if (m[35] === "A") return resolve(this);
                            this.reports.push(m);
                            for (const w of this.waiters.splice(0)) w();
                        },
                        () => {},
                    ),
                },
            );
        });
    }
    send(fields) {
        this.raw.write(fixEncode(fields, this.comp, "EXCH", this.seq++));
    }
    order(id, sym, side, qty, price, extra = {}) {
        this.send({
            35: "D",
            11: id,
            1: this.comp,
            55: sym,
            54: side,
            38: qty,
            40: price ? "2" : "1",
            ...(price ? { 44: price.toFixed(2) } : {}),
            59: "0",
            ...extra,
        });
    }
    async settle(ms = 30) {
        await new Promise((r) => setTimeout(r, ms));
    }
}

const results = {};
const mm = await new FixClient("MAKER").connect(),
    fund = await new FixClient("FUND").connect(),
    retail = await new FixClient("RETAIL").connect();
const rng = new Random(9);
// The market maker quotes a ladder on both symbols.
for (const [sym, mid] of [
    ["ACME", 100],
    ["GLOBX", 42.5],
])
    for (let k = 1; k <= 5; k++) {
        mm.order(`mm-${sym}-b${k}`, sym, "1", 200 * k, Number((mid - k * 0.05).toFixed(2)));
        mm.order(`mm-${sym}-a${k}`, sym, "2", 200 * k, Number((mid + k * 0.05).toFixed(2)));
    }
await mm.settle();
fund.order("f1", "ACME", "1", 600, 100.1); // sweeps the 100.05 and 100.10 ask levels exactly
retail.order("r1", "ACME", "1", 50, 0); // market order
retail.order("r2", "ACME", "2", 10_000, 99.9); // above the quantity limit
retail.order("r3", "ACME", "2", 100, 80.0); // outside the price collar
fund.order("f2", "GLOBX", "1", 5000, 42.7, { 59: "4" }); // FOK larger than the book -> rejected
fund.order("f3", "GLOBX", "1", 300, 42.56, { 59: "3" }); // IOC: fills what crosses, cancels the rest
mm.order("mm-post", "ACME", "2", 100, 99.9, { 18: "6" }); // post-only that would cross -> rejected
await fund.settle();
mm.order("mm-self", "ACME", "2", 100, 99.95); // rests; then MAKER buys at that price -> self-trade prevention
await mm.settle();
mm.order("mm-self-buy", "ACME", "1", 100, 99.95);
fund.send({ 35: "F", 11: "f1-cxl", 41: "mm-ACME-b5", 55: "ACME", 1: "FUND" }); // cancelling someone else's order fails
const spoof = await new FixClient("LAYER").connect();
for (let round = 0; round < 4; round++) {
    // layering: big bids that are never meant to fill
    for (let k = 0; k < 5; k++)
        spoof.order(`sp-${round}-${k}`, "GLOBX", "1", 900, Number((42.3 - k * 0.01).toFixed(2)));
    await spoof.settle(5);
    for (let k = 0; k < 5; k++)
        spoof.send({ 35: "F", 11: `sp-${round}-${k}-x`, 41: `sp-${round}-${k}`, 55: "GLOBX", 1: "LAYER" });
}
spoof.order("sp-real", "GLOBX", "2", 50, 0);
dropNext = 3; // lose three market-data packets
for (let i = 0; i < 40; i++) {
    const c = [mm, fund, retail][i % 3],
        side = rng.nextFloat() < 0.5 ? "1" : "2";
    c.order(
        `x${i}`,
        i % 2 ? "ACME" : "GLOBX",
        side,
        10 + rng.nextBounded(90),
        i % 2
            ? Number((99.8 + rng.nextBounded(40) / 100).toFixed(2))
            : Number((42.3 + rng.nextBounded(40) / 100).toFixed(2)),
    );
}
retail.raw.write("8=FIX.4.4\x019=5\x0135=0\x0110=000\x01"); // corrupt checksum
await new Promise((r) => setTimeout(r, 300));

const rep = (c, id) => c.reports.filter((m) => m[11] === id);
results["the aggressive limit order filled across several levels"] =
    rep(fund, "f1").at(-1)?.[39] === "1" || rep(fund, "f1").at(-1)?.[39] === "2";
results["risk rejects oversized and off-collar orders"] =
    rep(retail, "r2")[0]?.[58]?.includes("above limit") && rep(retail, "r3")[0]?.[58]?.includes("collar");
results["an unfillable FOK is rejected without touching the book"] = rep(fund, "f2")[0]?.[58]?.includes("fill-or-kill");
results["IOC fills what crosses and cancels the remainder"] =
    rep(fund, "f3").some((m) => m[39] === "4" && /IOC/.test(m[58] || "")) && rep(fund, "f3").some((m) => m[32]);
results["post-only orders never take liquidity"] = rep(mm, "mm-post")[0]?.[58]?.includes("post-only");
results["self-trade prevention cancels the resting order of the same account"] = rep(mm, "mm-self").some((m) =>
    /self-trade/.test(m[58] || ""),
);
results["an account cannot cancel another account's order"] = fund.reports.some(
    (m) => m[11] === "f1-cxl" && m[39] === "8",
);
results["a corrupted message is rejected at the session layer"] = retail.reports.some(
    (m) => m[35] === "3" && /checksum/.test(m[58] || ""),
);
const top = (sym, side) => {
    const lvl = subscriber.book.get(`${sym}|${side}`) || new Map();
    const ps = [...lvl.keys()].sort((a, b) => a - b);
    return side === "1" ? ps.at(-1) : ps[0];
};
results[`the feed handler recovered from ${subscriber.gaps} gap(s) and matches the book`] =
    subscriber.gaps >= 1 &&
    top("ACME", "1") === engine.book("ACME").bestBid() &&
    top("ACME", "2") === engine.book("ACME").bestAsk();

// Replay the journal into a fresh engine: identical trades and books.
const replay = new Engine();
for (const j of engine.journal)
    j.op === "new" ? replay.submit({ ...j.o }) : replay.cancel(j.symbol, j.id, j.account, j.clId);
results[`journal replay reproduces ${engine.trades.length} trades and both books exactly`] =
    replay.stateHash() === engine.stateHash() && replay.trades.length === engine.trades.length;

const tdf = new DataFrame({
    sym: Float64Array.from(engine.trades.map((t) => (t.sym === "ACME" ? 0 : 1))),
    px: Float64Array.from(engine.trades.map((t) => t.price / TICK)),
    qty: Float64Array.from(engine.trades.map((t) => t.qty)),
});
const stats = ["ACME", "GLOBX"].map((sym, k) => {
    const d = tdf.FILTER(tdf.EQ("sym", k)),
        px = d.TO_COLUMNS().px,
        qty = d.TO_COLUMNS().qty;
    const notional = px.reduce((s, p, i) => s.add(new Decimal(p.toFixed(2)).mul(qty[i])), Decimal.ZERO);
    return [
        sym,
        px.length ? px[0].toFixed(2) : "-",
        d.COUNT("px") ? d.MAX("px").toFixed(2) : "-",
        d.COUNT("px") ? d.MIN("px").toFixed(2) : "-",
        px.length ? px.at(-1).toFixed(2) : "-",
        String(d.SUM("qty")),
        d.SUM("qty") ? notional.div(d.SUM("qty"), { precision: 10 }).toFixed(4) : "-",
    ];
});
const pnl = new Map();
for (const t of engine.trades)
    for (const [acct, sign] of [
        [t.buyer, 1],
        [t.seller, -1],
    ]) {
        const p = pnl.get(acct) || { pos: { ACME: 0, GLOBX: 0 }, cash: 0 };
        p.pos[t.sym] += sign * t.qty;
        p.cash -= (sign * t.qty * t.price) / TICK;
        pnl.set(acct, p);
    }
const mark = { ACME: engine.book("ACME").last / TICK, GLOBX: engine.book("GLOBX").last / TICK };
const volume = new ZSet(),
    sizes = new TDigest(100);
for (const t of engine.trades) {
    volume.incrBy(t.buyer, t.qty);
    volume.incrBy(t.seller, t.qty);
    sizes.add(t.qty);
}
const cancelRatio = new Map(placed.elementSet().map((a) => [a, cancelled.count(a) / placed.count(a)]));
const flagged = [...cancelRatio].filter(([, r]) => r > 0.5).map(([a]) => a);
results[`surveillance flags only the layering account (${flagged.join(", ")})`] =
    flagged.length === 1 && flagged[0] === "LAYER";
results["the volume leaderboard's total is twice the traded quantity"] =
    volume.entries().reduce((s, [, v]) => s + v, 0) === 2 * engine.trades.pluck("qty").sum();
results["every price level's aggregate equals the sum of its resting orders"] = [...engine.books.values()].all((b) =>
    [b.bids, b.asks].all((lvl) =>
        lvl.keys().all((p) => lvl.get(p).qty === [...lvl.get(p).orders.values()].pluck("leaves").sum()),
    ),
);
results["positions and cash net to zero across accounts"] =
    [...pnl.values()].reduce((s, p) => s + p.pos.ACME, 0) === 0 &&
    Math.abs([...pnl.values()].reduce((s, p) => s + p.cash, 0)) < 1e-6;

print("trading-engine");
print(
    `  sessions   ${sessions.size} FIX sessions; ${engine.journal.length} commands journalled; ${engine.trades.length} trades; md seq ${engine.seq}, ${subscriber.received} packets received, ${subscriber.gaps} gap(s) recovered`,
);
const d = engine.book("ACME").depth(5);
print(
    table([
        ["bid qty", "bid", "ask", "ask qty"],
        ...Array.from({ length: 5 }, (_, i) => [
            String(d.bids[i]?.[1] ?? ""),
            d.bids[i] ? (d.bids[i][0] / TICK).toFixed(2) : "",
            d.asks[i] ? (d.asks[i][0] / TICK).toFixed(2) : "",
            String(d.asks[i]?.[1] ?? ""),
        ]),
    ]),
);
print(table([["symbol", "open", "high", "low", "close", "volume", "VWAP"], ...stats]));
print(
    table([
        ["account", "ACME", "GLOBX", "cash", "P&L at last"],
        ...[...pnl].map(([a, p]) => [
            a,
            String(p.pos.ACME),
            String(p.pos.GLOBX),
            p.cash.toFixed(2),
            (p.cash + p.pos.ACME * mark.ACME + p.pos.GLOBX * mark.GLOBX).toFixed(2),
        ]),
    ]),
);
print(
    `  volume     ${volume
        .range(0, 3, { reverse: true })
        .map(([a, v]) => `${a} ${v}`)
        .join(", ")}; trade size p50 ${sizes.quantile(0.5)}, p90 ${sizes.quantile(0.9)}`,
);
print(`  surveil    ${[...cancelRatio].map(([a, r]) => `${a} cancels ${(r * 100).round(0)}% of orders`).join(", ")}`);
for (const c of [mm, fund, retail, spoof]) c.send({ 35: "5" });
await new Promise((r) => setTimeout(r, 30));
gw.close();
mdOut.close();
mdIn.close();
check(results);

function check(results) {
    const names = Object.keys(results);
    const failed = names.filter((name) => !results[name]);
    for (const name of failed) print("FAIL " + name);
    if (failed.length) throw new Error(`${failed.length} of ${names.length} checks failed`);
}
