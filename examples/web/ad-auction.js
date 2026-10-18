// Ad auction — a real-time bidding exchange: bitmap targeting, quality-weighted second-price auctions with a reserve,
// budget pacing, frequency caps from a sketch, all served over HTTP and audited for the properties auctions promise.
//
// WHAT IT IS
//   The decision an ad server makes in a few milliseconds per page view:
//
//   targeting   one RoaringBitmap of campaign ids per (geo, device, interest) value; the eligible set for an
//               impression is an AND/OR of a handful of bitmaps instead of a scan over every campaign's rules
//   auction     rank by eCPM = bid x pCTR; the winner pays the minimum bid that would still have won,
//               (runner-up eCPM / own pCTR) + 0.01, never less than the reserve and never more than its bid
//   pacing      each campaign spends its daily budget in step with traffic: every 100 requests its throttle
//               (participation probability) is nudged by how far actual spend is ahead or behind plan; compared with an unpaced run that burns
//               budgets by mid-morning
//   freq caps   impressions per (user, campaign) counted in a CountMinSketch; it can only over-count, so the cap
//               errs on the side of showing fewer ads, never more
//   audit       eligibility equals brute-force rule evaluation; no campaign exceeds its budget; every price is
//               between the reserve and the bid; truthful bidding is a best response in sampled auctions
//
// MODULES  dyna:structures (RoaringBitmap, CountMinSketch, Multiset, TDigest) · dyna:http (App, fetch) · dyna:random ·
//          dyna:cli · Array/Number extensions
//
// RUN      dynajs examples/web/ad-auction.js

import { RoaringBitmap, CountMinSketch, Multiset, TDigest } from "dyna:structures";
import { App } from "dyna:http";
import { Random } from "dyna:random";
import { Table } from "dyna:cli";

const results = {};
const rng = new Random(1994);
const GEOS = ["US", "DE", "FR", "JP", "BR"],
    DEVICES = ["mobile", "desktop", "tv"],
    INTERESTS = ["sports", "travel", "tech", "food", "finance", "games"];
const RESERVE = 0.5,
    CAP = 3,
    HOURS = 24;

// =============================================================================
// Campaigns and targeting bitmaps
// =============================================================================
const campaigns = Array.from({ length: 40 }, (_, id) => ({
    id,
    geos: GEOS.filter(() => rng.nextFloat() < 0.5),
    devices: DEVICES.filter(() => rng.nextFloat() < 0.6),
    interests: INTERESTS.filter(() => rng.nextFloat() < 0.35),
    bid: 1 + rng.nextBounded(900) / 100,
    ctr: 0.005 + rng.nextFloat() * 0.03,
    budget: 40 + rng.nextBounded(160),
}));
for (const c of campaigns) {
    if (!c.geos.length) c.geos = ["US"];
    if (!c.devices.length) c.devices = ["mobile"];
}
const byValue = new Map();
const bitmap = (k) => byValue.get(k) ?? byValue.set(k, new RoaringBitmap()).get(k);
for (const c of campaigns) {
    for (const g of c.geos) bitmap(`geo:${g}`).add(c.id);
    for (const d of c.devices) bitmap(`dev:${d}`).add(c.id);
    if (!c.interests.length) bitmap("int:*").add(c.id);
    for (const i of c.interests) bitmap(`int:${i}`).add(c.id);
}
function eligible(imp) {
    const any = bitmap("int:*").clone();
    for (const i of imp.interests) any.or(bitmap(`int:${i}`));
    return bitmap(`geo:${imp.geo}`)
        .clone()
        .and(bitmap(`dev:${imp.device}`))
        .and(any);
}
const bruteEligible = (imp) =>
    campaigns
        .filter(
            (c) =>
                c.geos.includes(imp.geo) &&
                c.devices.includes(imp.device) &&
                (!c.interests.length || c.interests.some((i) => imp.interests.includes(i))),
        )
        .map((c) => c.id);

// =============================================================================
// Auction
// =============================================================================
function auction(imp, state, { bids = null } = {}) {
    const cands = [];
    for (const id of eligible(imp)) {
        const c = campaigns[id],
            s = state.camp[id];
        if (s.spent >= c.budget) continue;
        if (state.paced && rng.nextFloat() > s.throttle) continue;
        if (state.freq.count(`${imp.user}|${id}`) >= CAP) {
            state.capped++;
            continue;
        }
        const bid = bids?.[id] ?? c.bid;
        if (bid < RESERVE) continue;
        cands.push({ id, bid, ecpm: bid * c.ctr });
    }
    if (!cands.length) return null;
    cands.sort((a, b) => b.ecpm - a.ecpm || a.id - b.id);
    const w = cands[0],
        c = campaigns[w.id];
    const price = Math.min(w.bid, Math.max(RESERVE, cands[1] ? cands[1].ecpm / c.ctr + 0.01 : RESERVE));
    return { id: w.id, bid: w.bid, price, runnerUp: cands[1]?.id ?? null };
}
function newState(paced) {
    return {
        paced,
        camp: campaigns.map(() => ({ spent: 0, hourSpent: new Float64Array(HOURS), throttle: 1 })),
        freq: new CountMinSketch(8192, 4),
        exactFreq: new Multiset(),
        capped: 0,
    };
}
// Traffic follows a daily curve; prices are CPM-like per impression for readability.
const traffic = Array.from({ length: HOURS }, (_, h) => Math.round(800 + 700 * Math.sin(((h - 8) / 24) * 2 * Math.PI)));
const makeImp = (h) => ({
    user: `u${rng.nextBounded(3000)}`,
    geo: GEOS[rng.nextBounded(5)],
    device: DEVICES[rng.nextBounded(3)],
    interests: INTERESTS.filter(() => rng.nextFloat() < 0.3),
    hour: h,
});

function run(paced, serve) {
    const state = newState(paced);
    const prices = new TDigest(100);
    let won = 0,
        clicks = 0,
        priceOk = true;
    const totalTraffic = traffic.sum();
    let seen = 0;
    if (paced) for (const s of state.camp) s.throttle = 0.1;
    for (let h = 0; h < HOURS; h++) {
        for (let i = 0; i < traffic[h]; i++) {
            if (paced && seen % 100 === 0)
                for (const [id, s] of state.camp.entries()) {
                    const planned = campaigns[id].budget * ((seen + 100) / totalTraffic);
                    const err = (s.spent - planned) / campaigns[id].budget;
                    s.throttle = Math.min(1, Math.max(0.005, s.throttle * Math.exp(-20 * err)));
                }
            const imp = makeImp(h);
            const r = serve ? serve(imp, state) : auction(imp, state);
            seen++;
            if (!r) continue;
            const s = state.camp[r.id];
            const charge = Math.min(r.price, campaigns[r.id].budget - s.spent);
            s.spent += charge;
            s.hourSpent[h] += charge;
            state.freq.add(`${imp.user}|${r.id}`);
            state.exactFreq.add(`${imp.user}|${r.id}`);
            prices.add(r.price);
            won++;
            if (rng.nextFloat() < campaigns[r.id].ctr) clicks++;
            if (!(r.price >= RESERVE - 1e-9 && r.price <= r.bid + 1e-9)) priceOk = false;
        }
    }
    return { state, won, clicks, priceOk, prices };
}

// =============================================================================
// Audits
// =============================================================================
{
    let agree = 0;
    for (let i = 0; i < 2000; i++) {
        const imp = makeImp(0);
        if (JSON.stringify(Array.from(eligible(imp).toArray())) === JSON.stringify(bruteEligible(imp))) agree++;
    }
    results["bitmap eligibility equals brute-force rule evaluation"] = agree === 2000;
}
const unpaced = run(false),
    paced = run(true);
const overspent = (r) => r.state.camp.some((s, id) => s.spent > campaigns[id].budget + 1e-9);
results["no campaign spends past its budget"] = !overspent(unpaced) && !overspent(paced);
results["every clearing price lies between the reserve and the bid"] = unpaced.priceOk && paced.priceOk;
results["frequency caps are never exceeded (the sketch only over-counts)"] = [unpaced, paced].every((r) =>
    r.state.exactFreq.entrySet().every(([, n]) => n <= CAP),
);
const spendCurve = (r) => Array.from({ length: HOURS }, (_, h) => r.state.camp.reduce((s, c) => s + c.hourSpent[h], 0));
const burnHour = (r) => {
    const total = r.state.camp.reduce((s, c) => s + c.spent, 0);
    let acc = 0;
    return spendCurve(r).findIndex((x) => (acc += x) >= total / 2);
};
results["unpaced campaigns burn half their spend by mid-morning"] = burnHour(unpaced) <= 10;
results["pacing moves the half-spend point into the afternoon"] = burnHour(paced) > burnHour(unpaced) + 2;
{
    // Truthfulness: in a single-slot second-price auction, shading or inflating your bid never helps.
    let violations = 0,
        tested = 0;
    for (let i = 0; i < 300; i++) {
        const imp = makeImp(12),
            st = newState(false),
            r = auction(imp, st);
        if (!r || r.runnerUp === null) continue;
        tested++;
        for (const id of [r.id, r.runnerUp]) {
            const value = campaigns[id].bid;
            const utility = (b) => {
                const x = auction(imp, newState(false), { bids: { [id]: b } });
                return x && x.id === id ? value - x.price : 0;
            };
            const truthful = utility(value);
            for (const f of [0.5, 0.8, 1.2, 2]) if (utility(value * f) > truthful + 1e-9) violations++;
        }
    }
    results["no sampled bid deviation beats bidding truthfully"] = violations === 0 && tested > 100;
}

// =============================================================================
// Serving over HTTP
// =============================================================================
{
    const live = newState(true),
        latency = new TDigest(100);
    const app = new App({ port: 0 });
    app.post("/bid", (req) => {
        const t = performance.now();
        const r = auction(JSON.parse(req.body), live);
        latency.add(performance.now() - t);
        return r ?? { status: 204, body: "" };
    });
    app.start();
    const base = `http://127.0.0.1:${app.port}`;
    let answered = 0,
        noFill = 0;
    for (let i = 0; i < 2000; i += 100) {
        const rs = await Promise.all(
            Array.from({ length: 100 }, () =>
                fetch(base + "/bid", { method: "POST", body: JSON.stringify(makeImp(9)) }),
            ),
        );
        for (const r of rs) {
            if (r.status === 200) {
                const x = await r.json();
                if (x.price <= x.bid) answered++;
            } else if (r.status === 204) noFill++;
        }
    }
    app.close();
    results["the exchange answers every bid request"] = answered + noFill === 2000 && answered > 1500;
    results["auction decision p99 under 1 ms"] = latency.quantile(0.99) < 1;
    print(
        `HTTP: ${answered} wins, ${noFill} no-fill; decision p50 ${latency.quantile(0.5).toFixed(3)} ms, p99 ${latency.quantile(0.99).toFixed(3)} ms`,
    );
}

print(
    Table(
        [
            ["unpaced", unpaced],
            ["paced", paced],
        ].map(([n, r]) => [
            n,
            r.won.format(),
            r.clicks,
            r.prices.quantile(0.5).toFixed(2),
            r.state.camp.reduce((s, c) => s + c.spent, 0).toFixed(0),
            burnHour(r) + ":00",
            r.state.capped,
        ]),
        { head: ["run", "impressions won", "clicks", "median price", "total spend", "half-spend at", "capped"] },
    ),
);

check(results);

function check(results) {
    const names = Object.keys(results);
    const failed = names.filter((name) => !results[name]);
    for (const name of failed) print("FAIL " + name);
    if (failed.length) throw new Error(`${failed.length} of ${names.length} checks failed`);
}
