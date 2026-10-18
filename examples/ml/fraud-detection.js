// Fraud detection — real-time card authorisation scoring with streaming velocity features, a device/account link
// graph, a supervised model, rules, step-up decisions and a cost-based threshold.
//
// WHAT IT IS
//   The decision a payment processor has ~50 ms to make for every card authorisation:
//
//   stream       authorisations arrive over a TCP socket as length-prefixed MessagePack frames (the way an
//                acquirer gateway would send them); every one gets a reply frame with the decision
//   velocity     per card, per device and per merchant: counts and sums over 1 minute / 1 hour / 24 hours kept in
//                ring buffers, distinct merchants in an hour, distance and implied speed from the previous
//                transaction (geo), amount vs the card's own median
//   graph        cards, devices, emails and IPs are linked with union-find; a ring of cards sharing devices is a
//                classic fraud signal, and the size of a card's component becomes a feature
//   model        gradient boosted trees (XGBoost-style) trained on a labelled month, threshold chosen by the
//                business cost of a false decline (lost margin + annoyance) vs a missed fraud (chargeback)
//   rules        rules a risk analyst owns overlay the model: blocked BIN ranges decline, implausible travel steps up
//   decisions    approve / step-up (3-D Secure challenge) / decline, with reason codes
//   feedback     fraud reports arrive hours later and update a per-merchant fraud rate (a count-min sketch keeps it
//                bounded in memory); the devices behind confirmed fraud go into a CuckooFilter that the live path
//                consults, and a TopK keeps the merchants with the most chargebacks for the risk team
//   threshold    the cost-optimal threshold is found in one pass: validation scores are sorted once and the false-
//                decline and missed-fraud costs become two simd cumulative sums, instead of re-scanning every
//                score for each of ~90 candidate thresholds
//   velocity     one pass over a card's ring buffer yields all three windows (1 min, 1 h, 24 h) at once
//
// MODULES  dyna:net (TCPServer) · dyna:serialize (MsgPack) · dyna:encoding (uvarint framing) · dyna:ml (XGBClassifier,
//          rocAuc, averagePrecision) · dyna:structures (UnionFind, RingBuffer, CountMinSketch, BloomFilter,
//          CuckooFilter, TopK) · dyna:simd (cumsum) · dyna:mathx (stats) · dyna:random
//
// RUN      dynajs examples/ml/fraud-detection.js     trains on 40,000 historical authorisations, then scores 8,000 live ones over TCP

import { TCPServer } from "dyna:net";
import { MsgPackEncode, MsgPackDecode } from "dyna:serialize";
import { PutUvarint, Uvarint } from "dyna:encoding";
import { XGBClassifier, rocAuc, averagePrecision } from "dyna:ml";
import { UnionFind, RingBuffer, CountMinSketch, BloomFilter, CuckooFilter, TopK, Multiset } from "dyna:structures";
import * as simd from "dyna:simd";
import { stats } from "dyna:mathx";
import { Random } from "dyna:random";

const rng = new Random(4242);

// =============================================================================
// Simulated card population, merchants, fraud rings
// =============================================================================
const CITIES = [
    [52.52, 13.4],
    [48.85, 2.35],
    [51.5, -0.12],
    [40.42, -3.7],
    [41.9, 12.5],
    [59.33, 18.07],
    [40.71, -74.0],
    [35.68, 139.69],
];
const CARDS = 3000,
    MERCHANTS = 400;
const cards = Array.from({ length: CARDS }, (_, i) => ({
    id: `C${i}`,
    home: rng.nextBounded(6),
    device: `D${i}`,
    email: `e${i}@mail.example`,
    bin: [411111, 545454, 376000, 601100][i % 4],
    spend: Math.exp(rng.normal(3.3, 0.8)),
}));
const merchants = Array.from({ length: MERCHANTS }, (_, i) => ({
    id: `M${i}`,
    mcc: [5411, 5812, 5999, 4829, 7995, 5732][i % 6],
    risky: i % 6 >= 3 && rng.nextFloat() < 0.4,
}));
const riskyMerchants = merchants.filter((m) => m.risky);
const rings = Array.from({ length: 12 }, (_, r) => ({
    devices: [`FD${r}a`, `FD${r}b`],
    ip: `203.0.113.${r}`,
    cards: Array.from({ length: 6 }, () => rng.nextBounded(CARDS)),
}));
const BLOCKED_BINS = new BloomFilter(1 << 16, 5);
BLOCKED_BINS.add("666666");

const haversine = (a, b) => {
    const R = 6371,
        dLat = ((b[0] - a[0]) * Math.PI) / 180,
        dLon = ((b[1] - a[1]) * Math.PI) / 180;
    const h =
        Math.sin(dLat / 2) ** 2 +
        Math.cos((a[0] * Math.PI) / 180) * Math.cos((b[0] * Math.PI) / 180) * Math.sin(dLon / 2) ** 2;
    return 2 * R * Math.asin(Math.sqrt(h));
};
function generate(n, t0) {
    const out = [];
    let t = t0;
    for (let k = 0; k < n; k++) {
        t += rng.exponential(1 / 4000); // ~ one auth per 4 seconds
        const fraud = rng.nextFloat() < 0.02;
        let card, device, ip, city, amount, merchant;
        if (!fraud) {
            card = cards[rng.nextBounded(CARDS)];
            device = card.device;
            ip = `198.51.100.${card.home}`;
            city = rng.nextFloat() < 0.95 ? card.home : rng.nextBounded(CITIES.length);
            amount = card.spend * Math.exp(rng.normal(0, 0.5));
            merchant = merchants[rng.nextBounded(MERCHANTS)];
        } else {
            const ring = rings[rng.nextBounded(rings.length)];
            const style = rng.nextBounded(3);
            card = cards[style === 0 ? ring.cards[rng.nextBounded(ring.cards.length)] : rng.nextBounded(CARDS)];
            device =
                style === 0 ? ring.devices[rng.nextBounded(2)] : style === 1 ? `X${rng.nextBounded(1e6)}` : card.device;
            ip = style === 0 ? ring.ip : `192.0.2.${rng.nextBounded(255)}`;
            city = style === 2 ? card.home : 6 + rng.nextBounded(2);
            amount = card.spend * Math.exp(rng.normal(1.3, 0.6));
            merchant = riskyMerchants[rng.nextBounded(riskyMerchants.length)];
            for (let burst = 0; burst < (style === 1 ? 3 : 0); burst++)
                // card testing: tiny auths first
                out.push({
                    id: `A${t0}-${out.length}`,
                    t: t - 30_000 + burst * 5000,
                    card: card.id,
                    bin: String(card.bin),
                    device,
                    ip,
                    email: card.email,
                    merchant: merchant.id,
                    mcc: merchant.mcc,
                    amount: 1 + rng.nextBounded(200) / 100,
                    geo: CITIES[city],
                    fraud: 1,
                });
        }
        out.push({
            id: `A${t0}-${out.length}`,
            t,
            card: card.id,
            bin: rng.nextFloat() < 0.001 ? "666666" : String(card.bin),
            device,
            ip,
            email: card.email,
            merchant: merchant.id,
            mcc: merchant.mcc,
            amount: Math.round(amount * 100) / 100,
            geo: CITIES[city],
            fraud: fraud ? 1 : 0,
        });
    }
    return out.sort((a, b) => a.t - b.t);
}

// =============================================================================
// Streaming feature state
// =============================================================================
class Velocity {
    constructor() {
        this.buf = new RingBuffer(256);
    }
    add(t, amount) {
        this.buf.push([t, amount]);
    }
    windows(t) {
        // [n1m, sum1m, n1h, sum1h, n24h, sum24h] in one pass
        const w = [0, 0, 0, 0, 0, 0];
        for (const [u, a] of this.buf.toArray()) {
            const age = t - u;
            if (age <= 86400_000) {
                w[4]++;
                w[5] += a;
                if (age <= 3600_000) {
                    w[2]++;
                    w[3] += a;
                    if (age <= 60_000) {
                        w[0]++;
                        w[1] += a;
                    }
                }
            }
        }
        return w;
    }
}
class FeatureState {
    constructor() {
        this.card = new Map();
        this.device = new Map();
        this.merchantFraud = new CountMinSketch(4096, 4);
        this.merchantVolume = new CountMinSketch(4096, 4);
        this.last = new Map();
        this.amounts = new Map();
        this.hourMerchants = new Map();
        this.ids = new Map();
        this.uf = new UnionFind(200000);
        this.size = new Map();
    }
    node(key) {
        if (!this.ids.has(key)) this.ids.set(key, this.ids.size);
        return this.ids.get(key);
    }
    link(a) {
        const c = this.node(`c:${a.card}`);
        for (const k of [`d:${a.device}`, `i:${a.ip}`]) {
            const n = this.node(k),
                rc = this.uf.find(c),
                rn = this.uf.find(n);
            if (rc !== rn) {
                const s = (this.size.get(rc) || 1) + (this.size.get(rn) || 1);
                this.uf.union(c, n);
                this.size.set(this.uf.find(c), s);
            }
        }
    }
    cardsInComponent(card) {
        const r = this.uf.find(this.node(`c:${card}`));
        return this.size.get(r) || 1;
    }
    features(a) {
        const v = this.card.get(a.card) || new Velocity(),
            dv = this.device.get(a.device) || new Velocity();
        const [n1m, s1m, n1h, s1h, n24, s24] = v.windows(a.t),
            dn1h = dv.windows(a.t)[2];
        const last = this.last.get(a.card);
        const km = last ? haversine(last.geo, a.geo) : 0,
            hours = last ? Math.max((a.t - last.t) / 3600_000, 1 / 60) : 24;
        const hist = this.amounts.get(a.card) || [];
        const median = hist.length >= 3 ? stats.median(hist) : a.amount;
        const mset = this.hourMerchants.get(a.card) || [];
        const distinctMerchants = new Set(mset.filter(([u]) => a.t - u < 3600_000).map(([, m]) => m)).size;
        const vol = this.merchantVolume.count(a.merchant),
            fr = this.merchantFraud.count(a.merchant);
        return [
            a.amount,
            Math.log1p(a.amount),
            a.amount / median,
            n1m,
            s1m,
            n1h,
            s1h,
            n24,
            s24,
            dn1h,
            km,
            km / hours,
            distinctMerchants,
            last && last.device !== a.device ? 1 : 0,
            this.cardsInComponent(a.card),
            (fr + 0.5) / (vol + 50),
            [5411, 5812, 5999, 4829, 7995, 5732].indexOf(a.mcc),
            a.amount < 3 ? 1 : 0,
        ];
    }
    update(a) {
        if (!this.card.has(a.card)) this.card.set(a.card, new Velocity());
        if (!this.device.has(a.device)) this.device.set(a.device, new Velocity());
        this.card.get(a.card).add(a.t, a.amount);
        this.device.get(a.device).add(a.t, a.amount);
        this.last.set(a.card, { t: a.t, geo: a.geo, device: a.device });
        const h = this.amounts.get(a.card) || [];
        h.push(a.amount);
        if (h.length > 20) h.shift();
        this.amounts.set(a.card, h);
        const m = this.hourMerchants.get(a.card) || [];
        m.push([a.t, a.merchant]);
        if (m.length > 30) m.shift();
        this.hourMerchants.set(a.card, m);
        this.merchantVolume.add(a.merchant);
        this.link(a);
    }
    chargeback(a) {
        this.merchantFraud.add(a.merchant);
        chargebackMerchants.add(a.merchant);
        if (deviceReports.add(a.device) === 2) compromised.add(a.device); // two independent reports, not one victim
    }
}
const compromised = new CuckooFilter(1 << 14);
const chargebackMerchants = new TopK(64);
const deviceReports = new Multiset();
const NAMES = [
    "amount",
    "logAmount",
    "amountVsMedian",
    "n1m",
    "sum1m",
    "n1h",
    "sum1h",
    "n24h",
    "sum24h",
    "deviceN1h",
    "km",
    "speedKmh",
    "merchants1h",
    "newDevice",
    "componentSize",
    "merchantFraudRate",
    "mcc",
    "microAuth",
];

// =============================================================================
// Training on history (features computed in time order, as they would have been live)
// =============================================================================
const history = generate(40000, Date.UTC(2026, 1, 1));
const state = new FeatureState();
const Xh = [],
    yh = [];
const pendingCb = [];
for (const a of history) {
    Xh.push(state.features(a));
    yh.push(a.fraud);
    state.update(a);
    if (a.fraud) pendingCb.push([a.t + 6 * 3600_000, a]); // fraud reports land six hours later
    while (pendingCb.length && pendingCb[0][0] <= a.t) state.chargeback(pendingCb.shift()[1]);
}
const cut = Math.floor(Xh.length * 0.75);
const model = await new XGBClassifier({
    nEstimators: 250,
    maxDepth: 5,
    learningRate: 0.08,
    subsample: 0.8,
    colsampleByTree: 0.8,
    seed: 5,
}).fit(Xh.slice(0, cut), yh.slice(0, cut));
const pVal = (await model.predictProba(Xh.slice(cut))).map((p) => p[1]);
const yVal = yh.slice(cut);
const auc = await rocAuc(yVal, pVal),
    ap = await averagePrecision(yVal, pVal);
const COST = { falseDecline: 100, missedFraud: (amount) => amount + 15 }; // a false decline costs margin plus a share of lost customers
const valAmounts = history.slice(cut).map((a) => a.amount);
// Sort by score once; declining everything at or above position k costs the false declines from k up plus the
// missed fraud below k, i.e. a suffix sum and a prefix sum of two cost vectors.
const order = [...pVal.keys()].sortBy((i) => pVal[i]);
const fdCost = simd.cumsum(Float32Array.from(order, (i) => (yVal[i] ? 0 : COST.falseDecline)));
const missCost = simd.cumsum(Float32Array.from(order, (i) => (yVal[i] ? COST.missedFraud(valAmounts[i]) : 0)));
const totalFd = fdCost[fdCost.length - 1];
let best = null;
for (let k = 0; k < order.length; k++) {
    const th = pVal[order[k]];
    if (th < 0.02 || th > 0.95) continue;
    const cost = totalFd - (k ? fdCost[k - 1] : 0) + (k ? missCost[k - 1] : 0);
    if (!best || cost < best.cost) best = { th, cost };
}
const bruteCost = (th) =>
    pVal.reduce(
        (c, p, i) =>
            c +
            (p >= th && !yVal[i] ? COST.falseDecline : 0) +
            (p < th && yVal[i] ? COST.missedFraud(valAmounts[i]) : 0),
        0,
    );
const DECLINE = best.th,
    STEP_UP = DECLINE / 3;

// =============================================================================
// Live scoring over TCP with framed MessagePack
// =============================================================================
const frame = (obj) => {
    const body = MsgPackEncode(obj),
        len = PutUvarint(body.length),
        out = new Uint8Array(len.length + body.length);
    out.set(len);
    out.set(body, len.length);
    return out;
};
function deframer(onMessage) {
    let buf = new Uint8Array(0);
    return (first, second) => {
        const chunk = second ?? first; // usable as feed(bytes) or as a TCP data(conn, bytes) handler
        const merged = new Uint8Array(buf.length + chunk.length);
        merged.set(buf);
        merged.set(chunk, buf.length);
        buf = merged;
        for (;;) {
            if (!buf.length) return;
            let n, used;
            try {
                [n, used] = Uvarint(buf);
            } catch {
                return;
            }
            if (used <= 0 || buf.length < used + Number(n)) return;
            onMessage(MsgPackDecode(buf.subarray(used, used + Number(n))));
            buf = buf.slice(used + Number(n));
        }
    };
}
const latencies = [];
const reasons = new Map();
async function decide(a) {
    const started = performance.now();
    const codes = [];
    if (BLOCKED_BINS.mayContain(a.bin)) codes.push("R01-blocked-bin");
    const x = state.features(a);
    if (x[11] > 900 && x[10] > 500) codes.push("R02-implausible-travel");
    const knownBad = compromised.mayContain(a.device);
    if (knownBad) codes.push("R16-known-bad-device");
    const [[, p]] = await model.predictProba([x]);
    state.update(a);
    let decision = "approve";
    const travel = x[11] > 900 && x[10] > 500; // possible with a flight: challenge, never auto-decline
    if (codes.includes("R01-blocked-bin") || p >= DECLINE) decision = "decline";
    else if (p >= STEP_UP || travel || knownBad) decision = "step-up";
    if (p >= STEP_UP) {
        // reason codes: the features furthest above normal
        const order = [
            [2, "R10-amount-unusual"],
            [3, "R11-burst"],
            [12, "R12-many-merchants"],
            [14, "R13-linked-cards"],
            [17, "R14-card-testing"],
            [13, "R15-new-device"],
        ];
        for (const [k, code] of order)
            if (
                (k === 2 && x[k] > 4) ||
                (k === 3 && x[k] >= 3) ||
                (k === 12 && x[k] >= 4) ||
                (k === 14 && x[k] > 4) ||
                (k === 17 && x[k]) ||
                (k === 13 && x[k])
            )
                codes.push(code);
    }
    for (const c of codes) reasons.set(c, (reasons.get(c) || 0) + 1);
    latencies.push(performance.now() - started);
    return { id: a.id, decision, score: Math.round(p * 1000) / 1000, reasons: codes };
}
const server = new TCPServer({ port: 0 });
server.start({
    connect: (conn) => {
        // one decision at a time per connection, in order
        let queue = Promise.resolve();
        conn.feed = deframer((msg) => {
            queue = queue.then(async () => conn.write(frame(await decide(msg))));
        });
    },
    data: (conn, bytes) => conn.feed(bytes),
});
const live = generate(8000, Date.UTC(2026, 2, 10));
const decisions = new Map();
let watchdog;
await new Promise((resolve, reject) => {
    TCPServer.connect(
        { port: server.port, host: "127.0.0.1" },
        {
            connect(conn, err) {
                if (err) return reject(new Error(err));
                for (let i = 0; i < live.length; i += 100)
                    conn.write(
                        new Uint8Array(live.slice(i, i + 100).flatMap((a) => [...frame({ ...a, fraud: undefined })])),
                    );
            },
            data: deframer((msg) => {
                decisions.set(msg.id, msg);
                if (decisions.size === live.length) resolve();
            }),
        },
    );
    watchdog = setTimeout(() => reject(new Error(`only ${decisions.size} decisions arrived`)), 25_000);
}).finally(() => {
    clearTimeout(watchdog);
    server.close();
});

// =============================================================================
// Outcome, checks and report
// =============================================================================
const results = {};
const tally = { approve: [0, 0], "step-up": [0, 0], decline: [0, 0] };
let fraudLoss = 0,
    fraudTotal = 0,
    falseDeclines = 0;
for (const a of live) {
    const d = decisions.get(a.id);
    tally[d.decision][a.fraud]++;
    if (a.fraud) {
        fraudTotal += a.amount;
        if (d.decision === "approve") fraudLoss += a.amount;
    } else if (d.decision === "decline") falseDeclines++;
}
const fraudCount = live.filter((a) => a.fraud).length;
const caught = tally.decline[1] + tally["step-up"][1];
results[`validation ROC AUC ${auc.toFixed(3)}, average precision ${ap.toFixed(3)}`] = auc > 0.95 && ap > 0.6;
results["every authorisation got a framed reply over TCP"] = decisions.size === live.length;
results[`${caught}/${fraudCount} fraudulent auths declined or challenged`] = caught / fraudCount > 0.8;
results[`false-decline rate ${((falseDeclines / (live.length - fraudCount)) * 100).toFixed(2)}%`] =
    falseDeclines / (live.length - fraudCount) < 0.02;
results[`fraud loss ${((fraudLoss / fraudTotal) * 100).toFixed(1)}% of attempted fraud value`] =
    fraudLoss / fraudTotal < 0.2;
results["blocked BINs are always declined by rule"] = live
    .filter((a) => a.bin === "666666")
    .every((a) => decisions.get(a.id).decision === "decline");
results["graph and card-testing reason codes fire"] =
    reasons.get("R13-linked-cards") > 0 && reasons.get("R14-card-testing") > 0;
const flaggedDevice = live.filter((a) => decisions.get(a.id).reasons.includes("R16-known-bad-device"));
results[`the one-pass threshold search agrees with a brute-force cost scan (${best.cost.round(0)})`] =
    Math.abs(bruteCost(best.th) - best.cost) < 1e-3 * best.cost;
results[
    `devices learned from chargebacks catch repeat fraud (${flaggedDevice.count((a) => a.fraud)}/${flaggedDevice.length} flagged were fraud)`
] = flaggedDevice.length > 0 && flaggedDevice.count((a) => a.fraud) / flaggedDevice.length > 0.5;
results["the chargeback TopK names only risky merchants"] = chargebackMerchants
    .list()
    .all(([m]) => merchants[Number(m.slice(1))].risky);
const p99 = stats.quantile(latencies, 0.99);
results[`p99 decision latency ${p99.toFixed(2)} ms`] = p99 < 50;

print("fraud-detection");
print(
    `  training   ${history.length} auths (${yh.filter(Boolean).length} fraud); AUC ${auc.toFixed(3)}, AP ${ap.toFixed(3)}; cost-optimal decline threshold ${DECLINE.toFixed(2)}, step-up ${STEP_UP.toFixed(2)}`,
);
print(
    `  features   top: ${model.featureImportances
        .map((v, i) => [NAMES[i], v])
        .sort((a, b) => b[1] - a[1])
        .slice(0, 5)
        .map(([n, v]) => `${n} ${v.toFixed(2)}`)
        .join(", ")}`,
);
print(
    `  live       ${live.length} auths over TCP: approve ${tally.approve[0] + tally.approve[1]}, step-up ${tally["step-up"][0] + tally["step-up"][1]}, decline ${tally.decline[0] + tally.decline[1]}`,
);
print(
    `  outcome    caught ${caught}/${fraudCount} fraud, ${falseDeclines} false declines, loss ${fraudLoss.toFixed(2)} of ${fraudTotal.toFixed(2)}`,
);
print(`  latency    p50 ${stats.median(latencies).toFixed(2)} ms, p99 ${p99.toFixed(2)} ms`);
print(
    `  feedback   ${chargebackMerchants
        .list()
        .take(5)
        .map(([m, n, err]) => `${m} ${n - err}+`)
        .join(", ")} most fraud reports; ${flaggedDevice.length} auths from known-bad devices`,
);
print(
    `  reasons    ${[...reasons]
        .sort((a, b) => b[1] - a[1])
        .map(([k, v]) => `${k} ${v}`)
        .join(", ")}`,
);
model.close();
check(results);

function check(results) {
    const names = Object.keys(results);
    const failed = names.filter((name) => !results[name]);
    for (const name of failed) print("FAIL " + name);
    if (failed.length) throw new Error(`${failed.length} of ${names.length} checks failed`);
}
