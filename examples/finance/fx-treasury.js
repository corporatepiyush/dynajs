// FX treasury — intercompany invoices across entities and currencies netted multilaterally, settled with the fewest
// transfers, and a quote sheet screened for triangular arbitrage with Bellman-Ford on negative log rates.
//
// WHAT IT IS
//   The monthly netting run of a multinational's in-house bank:
//
//   quotes      a sheet of bid/ask quotes between six currencies; cross rates triangulated through USD must agree with
//               direct quotes within the spread. A rate graph with edge weight -ln(rate) has a negative cycle exactly
//               when a round trip of conversions makes money: Graph.bellmanFord over it, run from every currency, finds
//               the planted mispriced quote and nothing in the clean sheet
//   invoices    3 000 intercompany invoices among 8 entities in their own currencies; every amount is a dyna:decimal
//               Decimal converted to EUR at mid with half-even rounding to the cent
//   netting     gross payables collapse to one net position per entity (a Table of entity x currency feeds it); the
//               positions sum to exactly zero
//   settlement  greedy largest-debtor-to-largest-creditor matching settles everything in at most n - 1 transfers; the
//               transfers reproduce every net position to the cent; compared with paying invoices gross
//
// MODULES  dyna:decimal (Decimal) · dyna:structures (Graph, Table, ZSet, Multiset) · dyna:random · dyna:cli ·
//          Array/Number extensions
//
// RUN      dynajs examples/finance/fx-treasury.js

import { Decimal } from "dyna:decimal";
import { Graph, Table as Grid, ZSet, Multiset } from "dyna:structures";
import { Random } from "dyna:random";
import { Table } from "dyna:cli";

const results = {};
const rng = new Random(1971);
const CCY = ["USD", "EUR", "GBP", "JPY", "CHF", "SGD"];
const MID_PER_USD = { USD: 1, EUR: 0.92, GBP: 0.79, JPY: 151.3, CHF: 0.88, SGD: 1.35 };
const SPREAD = 0.0004;

// =============================================================================
// Quotes and arbitrage screening
// =============================================================================
function quoteSheet(mispriced = null) {
    const q = new Map();
    for (const a of CCY)
        for (const b of CCY) {
            if (a === b) continue;
            let mid = MID_PER_USD[b] / MID_PER_USD[a];
            if (mispriced && mispriced[0] === a && mispriced[1] === b) mid *= 1.004;
            q.set(`${a}/${b}`, { bid: mid * (1 - SPREAD), ask: mid * (1 + SPREAD) });
        }
    return q;
}
// Converting a -> b at the bid; a profitable cycle is a negative cycle in -ln(bid).
function arbitrageFrom(q) {
    const g = new Graph({ directed: true, weighted: true });
    CCY.forEach(() => g.addNode());
    for (const [pair, { bid }] of q) {
        const [a, b] = pair.split("/");
        g.addEdge(CCY.indexOf(a), CCY.indexOf(b), -Math.log(bid));
    }
    const found = [];
    for (let s = 0; s < CCY.length; s++) {
        try {
            g.bellmanFord(s);
        } catch (e) {
            found.push(CCY[s]);
        }
    }
    return found;
}
function bruteTriangles(q) {
    const out = [];
    for (const a of CCY)
        for (const b of CCY)
            for (const c of CCY) {
                if (new Set([a, b, c]).size < 3) continue;
                const gain = q.get(`${a}/${b}`).bid * q.get(`${b}/${c}`).bid * q.get(`${c}/${a}`).bid;
                if (gain > 1) out.push(`${a}>${b}>${c}`);
            }
    return out;
}
{
    const clean = quoteSheet(),
        bad = quoteSheet(["GBP", "JPY"]);
    results["the clean sheet has no arbitrage cycle"] =
        arbitrageFrom(clean).length === 0 && bruteTriangles(clean).length === 0;
    results["a 0.4% mispriced GBP/JPY quote creates a negative cycle"] = arbitrageFrom(bad).length > 0;
    results["brute-force profitable triangles all use the GBP->JPY leg"] =
        bruteTriangles(bad).length > 0 &&
        bruteTriangles(bad).every((t) => `${t}>${t.split(">")[0]}`.includes("GBP>JPY"));
    let crossOk = true;
    for (const a of CCY)
        for (const b of CCY) {
            if (a === b || a === "USD" || b === "USD") continue;
            const tri =
                ((clean.get(`${a}/USD`).bid + clean.get(`${a}/USD`).ask) / 2) *
                ((clean.get(`USD/${b}`).bid + clean.get(`USD/${b}`).ask) / 2);
            const d = clean.get(`${a}/${b}`);
            if (tri < d.bid || tri > d.ask) crossOk = false;
        }
    results["USD-triangulated cross rates lie inside the direct bid/ask"] = crossOk;
}

// =============================================================================
// Invoices and netting
// =============================================================================
const ENTITIES = [
    ["US Inc", "USD"],
    ["DE GmbH", "EUR"],
    ["UK Ltd", "GBP"],
    ["JP KK", "JPY"],
    ["CH AG", "CHF"],
    ["SG Pte", "SGD"],
    ["FR SAS", "EUR"],
    ["IE Ltd", "EUR"],
];
const toEur = (amount, ccy) =>
    new Decimal(amount).mul(new Decimal(MID_PER_USD.EUR).div(MID_PER_USD[ccy], { precision: 20 })).round(2, "halfEven");
const invoices = [];
for (let i = 0; i < 3000; i++) {
    const from = rng.nextBounded(8);
    let to = rng.nextBounded(8);
    if (to === from) to = (to + 1) % 8;
    const ccy = ENTITIES[from][1];
    const raw =
        ccy === "JPY"
            ? String(Math.round(rng.exponential(1 / 5000) * 150))
            : (Math.round(rng.exponential(1 / 5000) * 100) / 100).toFixed(2);
    invoices.push({ payer: to, payee: from, ccy, amount: raw, eur: toEur(raw, ccy) });
}
const exposure = new Grid();
const net = Array.from({ length: 8 }, () => new Decimal(0));
for (const inv of invoices) {
    net[inv.payee] = net[inv.payee].add(inv.eur);
    net[inv.payer] = net[inv.payer].sub(inv.eur);
    const k = ENTITIES[inv.payer][0];
    exposure.put(k, inv.ccy, (exposure.get(k, inv.ccy) ?? new Decimal(0)).add(inv.eur));
}
results["net positions sum to exactly zero"] = net.reduce((s, d) => s.add(d), new Decimal(0)).isZero();
results["exposure table rows sum to each entity's gross payables"] = ENTITIES.every(([name], e) => {
    const row = exposure.row(name).reduce((s, [, v]) => s.add(v), new Decimal(0));
    const gross = invoices.filter((x) => x.payer === e).reduce((s, x) => s.add(x.eur), new Decimal(0));
    return row.equals(gross);
});

// =============================================================================
// Settlement with at most n - 1 transfers
// =============================================================================
const transfers = [];
{
    const debt = new ZSet(),
        cred = new ZSet();
    net.forEach((d, e) => {
        if (d.sign() < 0) debt.add(String(e), d.neg().toNumber());
        else if (d.sign() > 0) cred.add(String(e), d.toNumber());
    });
    const remaining = net.map((d) => d);
    while (true) {
        const dTop = debt.popMax(),
            cTop = cred.popMax();
        if (!dTop || !cTop) break;
        const di = Number(dTop[0]),
            ci = Number(cTop[0]);
        const owe = remaining[di].neg(),
            due = remaining[ci];
        const amt = owe.cmp(due) < 0 ? owe : due;
        transfers.push({ from: di, to: ci, amount: amt });
        remaining[di] = remaining[di].add(amt);
        remaining[ci] = remaining[ci].sub(amt);
        if (!remaining[di].isZero()) debt.add(String(di), remaining[di].neg().toNumber());
        if (!remaining[ci].isZero()) cred.add(String(ci), remaining[ci].toNumber());
    }
}
const replay = Array.from({ length: 8 }, () => new Decimal(0));
for (const t of transfers) {
    replay[t.from] = replay[t.from].sub(t.amount);
    replay[t.to] = replay[t.to].add(t.amount);
}
results["settlement transfers reproduce every net position to the cent"] = replay.every((d, e) => d.equals(net[e]));
results["at most n - 1 transfers settle the group"] = transfers.length <= ENTITIES.length - 1;
const grossPayments = new Multiset();
for (const inv of invoices) grossPayments.add(`${inv.payer}>${inv.payee}`);
const grossVolume = invoices.reduce((s, x) => s.add(x.eur), new Decimal(0));
const nettedVolume = transfers.reduce((s, t) => s.add(t.amount), new Decimal(0));
results["netting moves far less cash than gross settlement"] = nettedVolume.mul(5).cmp(grossVolume) < 0;
print(
    Table(
        ENTITIES.map(([name, ccy], e) => [name, ccy, net[e].toFixed(2)]),
        { head: ["entity", "currency", "net position (EUR)"] },
    ),
);
print(
    Table(
        transfers.map((t) => [ENTITIES[t.from][0], ENTITIES[t.to][0], t.amount.toFixed(2)]),
        { head: ["pays", "receives", "EUR"] },
    ),
);
print(
    `gross: ${invoices.length.format()} invoices over ${grossPayments.elementSet().length} entity pairs, ${grossVolume.toFixed(2)} EUR; netted: ${transfers.length} transfers, ${nettedVolume.toFixed(2)} EUR`,
);

check(results);

function check(results) {
    const names = Object.keys(results);
    const failed = names.filter((name) => !results[name]);
    for (const name of failed) print("FAIL " + name);
    if (failed.length) throw new Error(`${failed.length} of ${names.length} checks failed`);
}
