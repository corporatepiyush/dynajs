// Billing platform — subscriptions with seat, tiered and usage pricing, mid-cycle proration, metered usage with
// idempotent ingest, tax by jurisdiction, coupons, gap-free invoice numbering, dunning, credit notes, revenue
// recognition and SaaS metrics.
//
// WHAT IT IS
//   The Stripe-Billing-shaped core of a SaaS company, run over six months of simulated customers:
//
//   catalogue    plans in YAML: per-seat (Team), graduated tiers for API calls (Scale), a flat platform fee, each in
//                EUR minor units; coupons (percent for N months, fixed once)
//   usage        POST /usage takes metering events with an idempotency key; duplicates (client retries) are
//                acknowledged but counted once
//   invoicing    at each period end: recurring charges in advance, usage in arrears, proration lines for any
//                mid-cycle seat change or plan switch (credit for unused time on the old price, charge for the new),
//                discounts, then tax per line by the customer's country (reverse charge for EU B2B with a VAT id)
//   numbering    invoice numbers come from a per-year sequence in the same transaction as the invoice — no gaps,
//                no duplicates, even when two invoices are finalised concurrently
//   payments     a charge can fail; dunning retries on day 3, 5 and 7, then the subscription becomes past_due and
//                is cancelled after the last retry; a credit note can refund part of an invoice
//   revenue      each invoice line is recognised ratably over its service period (ASC 606), producing a deferred
//                revenue waterfall per month
//   webhooks     invoice.finalized / payment.failed / subscription.canceled events, HMAC-signed
//                — computed as a daily difference array (each line adds its daily rate at its first day and removes it
//                after its last), one prefix pass, and a dyna:simd sum per month, instead of a loop per line per day
//   metrics      MRR, net new MRR by movement (new, expansion, contraction, churn), and logo churn; the top accounts
//                by billed revenue from a ZSet, invoice-size percentiles from a TDigest
//
// MODULES  dyna:decimal (Money) · dyna:net (SQLite) · dyna:http (App) · dyna:yaml · dyna:crypto (HMAC)
//          · dyna:html (Template) · dyna:time (PlainDate) · dyna:dataframe · dyna:cli (Table) · dyna:random
//          · dyna:structures (ZSet, TDigest) · dyna:simd · Array/Number extensions
//
// RUN      dynajs examples/finance/billing-platform.js     bills 120 customers for six months and prints the books

import { Money } from "dyna:decimal";
import { SQLite } from "dyna:net";
import { App } from "dyna:http";
import * as YAML from "dyna:yaml";
import { HMACHex } from "dyna:crypto";
import { Template } from "dyna:html";
import { PlainDate } from "dyna:time";
import { DataFrame } from "dyna:dataframe";
import { Table } from "dyna:cli";
import { Random } from "dyna:random";
import { makeTempDir, removeAll } from "dyna:file";
import { ZSet, TDigest } from "dyna:structures";
import * as simd from "dyna:simd";
const table = (rows) => Table(rows.slice(1), { head: rows[0] });

const work = makeTempDir("billing");
const rng = new Random(606);
const EUR = (minor) => new Money(Math.round(minor), "EUR");

// =============================================================================
// Catalogue, tax, coupons
// =============================================================================
const CATALOGUE = YAML.Parse(`
plans:
  starter: { type: flat, price: 4900 }
  team:    { type: per_seat, price: 1500, minSeats: 3 }
  scale:   { type: per_seat, price: 1200, minSeats: 10, usage: api_calls }
usage:
  api_calls:
    unit: 1000 calls
    tiers: [{ upTo: 100, price: 0 }, { upTo: 1000, price: 40 }, { upTo: 10000, price: 25 }, { upTo: null, price: 10 }]
coupons:
  LAUNCH20: { percent: 20, months: 3 }
  WELCOME50: { amountOff: 5000, months: 1 }
tax:
  DE: 19
  FR: 20
  NL: 21
  US: 0
  GB: 20
`);
const EU = new Set(["DE", "FR", "NL"]);
function usageCharge(units) {
    // graduated tiers, units = thousands of calls
    let left = units,
        prev = 0,
        total = 0;
    const parts = [];
    for (const t of CATALOGUE.usage.api_calls.tiers) {
        const cap = t.upTo === null ? Infinity : t.upTo,
            take = Math.max(0, Math.min(left, cap - prev));
        if (take > 0) {
            parts.push({ units: take, unitPrice: t.price, amount: take * t.price });
            total += take * t.price;
            left -= take;
        }
        prev = cap;
        if (left <= 0) break;
    }
    return { total, parts };
}

// =============================================================================
// Storage
// =============================================================================
const db = new SQLite(work.join("billing.db"));
for (const stmt of [
    "CREATE TABLE customers (id TEXT PRIMARY KEY, name TEXT, country TEXT, vat_id TEXT, coupon TEXT, coupon_start TEXT)",
    "CREATE TABLE subscriptions (id TEXT PRIMARY KEY, customer TEXT, plan TEXT, seats INTEGER, status TEXT, period_start TEXT, started TEXT, canceled TEXT)",
    "CREATE TABLE changes (sub TEXT, at TEXT, plan TEXT, seats INTEGER)",
    "CREATE TABLE usage (key TEXT PRIMARY KEY, sub TEXT, at TEXT, units REAL)",
    "CREATE TABLE sequences (name TEXT PRIMARY KEY, next INTEGER)",
    "CREATE TABLE invoices (number TEXT PRIMARY KEY, customer TEXT, sub TEXT, period_start TEXT, period_end TEXT, subtotal INTEGER, tax INTEGER, total INTEGER, status TEXT, attempts INTEGER, next_retry TEXT)",
    "CREATE TABLE invoice_lines (invoice TEXT, description TEXT, amount INTEGER, tax INTEGER, service_start TEXT, service_end TEXT)",
    "CREATE TABLE credit_notes (number TEXT PRIMARY KEY, invoice TEXT, amount INTEGER, reason TEXT)",
    "CREATE TABLE events (id INTEGER PRIMARY KEY, type TEXT, payload TEXT, signature TEXT)",
])
    db.exec(stmt);
const WEBHOOK_SECRET = "whsec_billing_demo";
function emit(type, payload) {
    const body = JSON.stringify({ type, payload });
    db.exec("INSERT INTO events (type, payload, signature) VALUES (?, ?, ?)", [
        type,
        body,
        HMACHex("sha256", WEBHOOK_SECRET, body),
    ]);
}
function nextNumber(name) {
    // called inside the document's own transaction
    db.exec("INSERT OR IGNORE INTO sequences VALUES (?, 1)", [name]);
    const n = db.query("SELECT next FROM sequences WHERE name = ?", [name])[0].next;
    db.exec("UPDATE sequences SET next = next + 1 WHERE name = ?", [name]);
    return `${name}-${String(n).padStart(5, "0")}`;
}

// =============================================================================
// Dates
// =============================================================================
const pd = (s) => {
    const [y, m, d] = s.split("-").map(Number);
    return new PlainDate(y, m, d);
};
const addMonth = (s) => {
    const d = pd(s);
    const y = d.month === 12 ? d.year + 1 : d.year,
        m = d.month === 12 ? 1 : d.month + 1;
    return new PlainDate(y, m, Math.min(d.day, new PlainDate(y, m, 1).daysInMonth)).toString();
};
const daysBetween = (a, b) => pd(b).epochDay - pd(a).epochDay;
const addDays = (s, n) => {
    const d = new Date((pd(s).epochDay + n) * 86400_000);
    return `${d.getUTCFullYear()}-${String(d.getUTCMonth() + 1).padStart(2, "0")}-${String(d.getUTCDate()).padStart(2, "0")}`;
};

// =============================================================================
// Pricing and invoicing
// =============================================================================
const planPrice = (plan, seats) => {
    const p = CATALOGUE.plans[plan];
    return p.type === "flat" ? p.price : p.price * Math.max(seats, p.minSeats);
};
function invoice(sub, periodStart, periodEnd) {
    const cust = db.query("SELECT * FROM customers WHERE id = ?", [sub.customer])[0];
    const lines = [];
    const days = daysBetween(periodStart, periodEnd);
    // Proration for changes made during the period that is ending (they were billed at the old price in advance).
    const prevStart = db.query("SELECT period_start FROM invoices WHERE sub = ? ORDER BY period_start DESC LIMIT 1", [
        sub.id,
    ])[0]?.period_start;
    if (prevStart) {
        let plan = db.query("SELECT plan, seats FROM changes WHERE sub = ? AND at <= ? ORDER BY at DESC LIMIT 1", [
            sub.id,
            prevStart,
        ])[0];
        for (const ch of db.query("SELECT * FROM changes WHERE sub = ? AND at > ? AND at < ? ORDER BY at", [
            sub.id,
            prevStart,
            periodStart,
        ])) {
            const remaining = daysBetween(ch.at, periodStart),
                total = daysBetween(prevStart, periodStart);
            const oldAmt = (planPrice(plan.plan, plan.seats) * remaining) / total,
                newAmt = (planPrice(ch.plan, ch.seats) * remaining) / total;
            lines.push({
                description: `Unused time on ${plan.plan} x${plan.seats} after ${ch.at}`,
                amount: -Math.round(oldAmt),
                start: ch.at,
                end: periodStart,
            });
            lines.push({
                description: `Remaining time on ${ch.plan} x${ch.seats} from ${ch.at}`,
                amount: Math.round(newAmt),
                start: ch.at,
                end: periodStart,
            });
            plan = ch;
        }
        if (CATALOGUE.plans[sub.plan].usage) {
            // usage of the previous period, in arrears
            const units = db.query(
                "SELECT COALESCE(SUM(units), 0) AS u FROM usage WHERE sub = ? AND at >= ? AND at < ?",
                [sub.id, prevStart, periodStart],
            )[0].u;
            const u = usageCharge(Math.round(units));
            for (const p of u.parts)
                if (p.amount)
                    lines.push({
                        description: `API calls ${p.units}k @ €${(p.unitPrice / 100).toFixed(2)}/k`,
                        amount: p.amount,
                        start: prevStart,
                        end: periodStart,
                    });
        }
    }
    lines.push({
        description: `${sub.plan} plan x${sub.seats} seats, ${periodStart} – ${periodEnd}`,
        amount: planPrice(sub.plan, sub.seats),
        start: periodStart,
        end: periodEnd,
    });
    // Coupons.
    if (cust.coupon) {
        const c = CATALOGUE.coupons[cust.coupon],
            monthsIn = db.query("SELECT COUNT(*) AS n FROM invoices WHERE customer = ? AND period_start >= ?", [
                cust.id,
                cust.coupon_start,
            ])[0].n;
        if (monthsIn < c.months) {
            const base = lines.reduce((s, l) => s + l.amount, 0);
            const off = c.percent ? Math.round((base * c.percent) / 100) : Math.min(c.amountOff, base);
            if (off > 0)
                lines.push({ description: `Coupon ${cust.coupon}`, amount: -off, start: periodStart, end: periodEnd });
        }
    }
    // Tax per line.
    const reverseCharge = EU.has(cust.country) && cust.vat_id && cust.country !== "DE";
    const rate = reverseCharge ? 0 : CATALOGUE.tax[cust.country];
    for (const l of lines) l.tax = Math.round((l.amount * rate) / 100);
    const subtotal = lines.reduce((s, l) => s + l.amount, 0),
        tax = lines.reduce((s, l) => s + l.tax, 0);
    db.exec("BEGIN IMMEDIATE");
    const number = nextNumber(`INV-${periodStart.slice(0, 4)}`);
    db.exec("INSERT INTO invoices VALUES (?,?,?,?,?,?,?,?, 'open', 0, ?)", [
        number,
        cust.id,
        sub.id,
        periodStart,
        periodEnd,
        subtotal,
        tax,
        subtotal + tax,
        periodStart,
    ]);
    for (const l of lines)
        db.exec("INSERT INTO invoice_lines VALUES (?,?,?,?,?,?)", [
            number,
            l.description,
            l.amount,
            l.tax,
            l.start,
            l.end,
        ]);
    db.exec("COMMIT");
    emit("invoice.finalized", { number, customer: cust.id, total: subtotal + tax, reverseCharge: !!reverseCharge });
    return { number, subtotal, tax, total: subtotal + tax, lines, reverseCharge, days };
}
const RETRIES = [3, 5, 7];
function collect(today, failProbability) {
    for (const inv of db.query("SELECT * FROM invoices WHERE status IN ('open', 'retrying') AND next_retry <= ?", [
        today,
    ])) {
        const ok = inv.total <= 0 || rng.nextFloat() > failProbability(inv.customer);
        if (ok) {
            db.exec("UPDATE invoices SET status = 'paid' WHERE number = ?", [inv.number]);
            continue;
        }
        const attempts = inv.attempts + 1;
        emit("payment.failed", { invoice: inv.number, attempt: attempts });
        if (attempts > RETRIES.length) {
            db.exec("UPDATE invoices SET status = 'uncollectible', attempts = ? WHERE number = ?", [
                attempts,
                inv.number,
            ]);
            db.exec("UPDATE subscriptions SET status = 'canceled', canceled = ? WHERE id = ?", [today, inv.sub]);
            emit("subscription.canceled", { sub: inv.sub, reason: "payment_failed" });
        } else {
            db.exec("UPDATE invoices SET status = 'retrying', attempts = ?, next_retry = ? WHERE number = ?", [
                attempts,
                addDays(inv.period_start, RETRIES[attempts - 1]),
                inv.number,
            ]);
            db.exec("UPDATE subscriptions SET status = 'past_due' WHERE id = ?", [inv.sub]);
        }
    }
    db.exec(
        "UPDATE subscriptions SET status = 'active' WHERE status = 'past_due' AND id NOT IN (SELECT sub FROM invoices WHERE status = 'retrying')",
    );
}

// =============================================================================
// Usage ingest API
// =============================================================================
const app = new App({ port: 0 });
let duplicates = 0;
app.post("/usage", (req) => {
    const e = JSON.parse(req.body);
    if (!e.idempotencyKey || !e.subscription || !(e.units >= 0))
        return {
            status: 422,
            body: JSON.stringify({ error: "idempotencyKey, subscription and units are required" }),
            contentType: "application/json",
        };
    const n = db.exec("INSERT OR IGNORE INTO usage VALUES (?, ?, ?, ?)", [
        e.idempotencyKey,
        e.subscription,
        e.at,
        e.units,
    ]);
    if (!n) duplicates++;
    return { status: n ? 201 : 200, body: JSON.stringify({ recorded: !!n }), contentType: "application/json" };
});
app.start();

// =============================================================================
// Six months of customers
// =============================================================================
const COUNTRIES = ["DE", "DE", "FR", "NL", "US", "GB"];
const START = "2026-01-01";
for (let i = 0; i < 120; i++) {
    const country = COUNTRIES[i % COUNTRIES.length],
        plan = ["starter", "team", "team", "scale"][i % 4];
    const startDay = `2026-0${1 + rng.nextBounded(3)}-${String(1 + rng.nextBounded(28)).padStart(2, "0")}`;
    const coupon = i % 9 === 0 ? "LAUNCH20" : i % 13 === 0 ? "WELCOME50" : null;
    db.exec("INSERT INTO customers VALUES (?,?,?,?,?,?)", [
        `cus_${i}`,
        `Customer ${i} GmbH`,
        country,
        i % 3 === 0 ? `${country}${100000000 + i}` : null,
        coupon,
        coupon ? startDay : null,
    ]);
    const seats = plan === "starter" ? 1 : plan === "team" ? 3 + rng.nextBounded(12) : 10 + rng.nextBounded(40);
    db.exec("INSERT INTO subscriptions VALUES (?,?,?,?, 'active', ?, ?, NULL)", [
        `sub_${i}`,
        `cus_${i}`,
        plan,
        seats,
        startDay,
        startDay,
    ]);
    db.exec("INSERT INTO changes VALUES (?, ?, ?, ?)", [`sub_${i}`, startDay, plan, seats]);
}
const base = `http://127.0.0.1:${app.port}`;
const allInvoices = [];
let usageSent = 0;
const flaky = new Set(["cus_7", "cus_31", "cus_55"]);
for (let day = 0; day < 181; day++) {
    const today = addDays(START, day);
    // Usage for scale subscriptions, with ~10% of batches retried by the client.
    if (day % 2 === 0)
        for (const s of db.query(
            "SELECT * FROM subscriptions WHERE plan = 'scale' AND status != 'canceled' AND started <= ?",
            [today],
        )) {
            const ev = {
                idempotencyKey: `${s.id}-${today}`,
                subscription: s.id,
                at: today,
                units: 2 + rng.nextBounded(60),
            };
            for (let k = 0; k < (rng.nextFloat() < 0.1 ? 2 : 1); k++) {
                await (await fetch(base + "/usage", { method: "POST", body: JSON.stringify(ev) })).text();
                usageSent++;
            }
        }
    // Seat changes and plan switches mid-cycle.
    for (const s of db.query("SELECT * FROM subscriptions WHERE status = 'active' AND started < ?", [today])) {
        const r = rng.nextFloat();
        if (s.plan !== "starter" && r < 0.004) {
            const seats = Math.max(
                1,
                s.seats + (rng.nextFloat() < 0.7 ? 1 + rng.nextBounded(5) : -1 - rng.nextBounded(2)),
            );
            db.exec("UPDATE subscriptions SET seats = ? WHERE id = ?", [seats, s.id]);
            db.exec("INSERT INTO changes VALUES (?, ?, ?, ?)", [s.id, today, s.plan, seats]);
        } else if (s.plan === "starter" && r < 0.003) {
            db.exec("UPDATE subscriptions SET plan = 'team', seats = 3 WHERE id = ?", [s.id]);
            db.exec("INSERT INTO changes VALUES (?, ?, 'team', 3)", [s.id, today]);
        } else if (r > 0.9993) {
            db.exec("UPDATE subscriptions SET status = 'canceled', canceled = ? WHERE id = ?", [today, s.id]);
            emit("subscription.canceled", { sub: s.id, reason: "customer" });
        }
    }
    // Period boundaries: invoice in advance.
    for (const s of db.query(
        "SELECT * FROM subscriptions WHERE status IN ('active', 'past_due') AND period_start = ?",
        [today],
    )) {
        allInvoices.push(invoice(s, today, addMonth(today)));
        db.exec("UPDATE subscriptions SET period_start = ? WHERE id = ?", [addMonth(today), s.id]);
    }
    collect(today, (c) => (flaky.has(c) ? 0.97 : 0.02));
}
app.close();

// A credit note for an SLA breach on one invoice.
const slaInvoice = db.query(
    "SELECT * FROM invoices WHERE status = 'paid' AND total > 10000 ORDER BY number LIMIT 1",
)[0];
db.exec("BEGIN IMMEDIATE");
const cn = nextNumber("CN-2026");
db.exec("INSERT INTO credit_notes VALUES (?, ?, ?, ?)", [
    cn,
    slaInvoice.number,
    -Math.round(slaInvoice.total * 0.1),
    "SLA credit 10% (March outage)",
]);
db.exec("COMMIT");

// =============================================================================
// Revenue recognition and SaaS metrics
// =============================================================================
const months = ["2026-01", "2026-02", "2026-03", "2026-04", "2026-05", "2026-06", "2026-07"];
const DAY0 = pd(START).epochDay,
    HORIZON = daysBetween(START, "2026-08-01");
const daily = new Float64Array(HORIZON + 1);
const billed = new Map(months.map((m) => [m, 0]));
const lines = db.query(
    "SELECT l.*, i.period_start, i.customer FROM invoice_lines l JOIN invoices i ON i.number = l.invoice WHERE i.status != 'uncollectible'",
);
for (const l of lines) {
    billed.set(l.period_start.slice(0, 7), billed.get(l.period_start.slice(0, 7)) + l.amount);
    const from = pd(l.service_start).epochDay - DAY0,
        to = pd(l.service_end).epochDay - DAY0;
    const span = to - from || 1;
    daily[Math.max(0, from)] += l.amount / span;
    daily[Math.min(HORIZON, from + span)] -= l.amount / span;
}
for (let d = 1; d <= HORIZON; d++) daily[d] += daily[d - 1];
const monthStart = (m) => daysBetween(START, `${m}-01`);
const recognised = new Map(
    months.map((m, i) => [
        m,
        simd.f64Sum(daily.subarray(monthStart(m), i + 1 < months.length ? monthStart(months[i + 1]) : HORIZON)),
    ]),
);
let deferred = 0;
const waterfall = months.take(6).map((m) => {
    deferred += billed.get(m) - recognised.get(m);
    return [m, EUR(billed.get(m)).format(), EUR(recognised.get(m)).format(), EUR(deferred).format()];
});
function mrrAt(date) {
    const bySub = new Map();
    for (const s of db.query("SELECT * FROM subscriptions WHERE started <= ? AND (canceled IS NULL OR canceled > ?)", [
        date,
        date,
    ])) {
        const ch = db.query("SELECT * FROM changes WHERE sub = ? AND at <= ? ORDER BY at DESC LIMIT 1", [
            s.id,
            date,
        ])[0];
        bySub.set(s.id, planPrice(ch.plan, ch.seats));
    }
    return { mrr: [...bySub.values()].sum(), logos: bySub.size, bySub };
}
const mrrSeries = ["2026-02-01", "2026-03-01", "2026-04-01", "2026-05-01", "2026-06-01", "2026-07-01"].map((d) => ({
    d,
    ...mrrAt(d),
}));
// MRR movements between consecutive snapshots: every subscription's change falls in exactly one bucket.
const movements = mrrSeries.drop(1).map((cur, i) => {
    const prev = mrrSeries[i],
        m = { d: cur.d, new: 0, expansion: 0, contraction: 0, churn: 0 };
    for (const [id, v] of cur.bySub) {
        const was = prev.bySub.get(id);
        if (was === undefined) m.new += v;
        else if (v > was) m.expansion += v - was;
        else if (v < was) m.contraction += v - was;
    }
    for (const [id, v] of prev.bySub) if (!cur.bySub.has(id)) m.churn -= v;
    m.net = m.new + m.expansion + m.contraction + m.churn;
    return m;
});
// Top accounts by billed revenue, and invoice-size percentiles.
const revenueRank = new ZSet();
for (const l of lines) revenueRank.incrBy(l.customer, l.amount);
const topAccounts = revenueRank.range(0, 4, { reverse: true });
const totals = Float64Array.from(db.query("SELECT total FROM invoices WHERE total > 0").pluck("total"));
const sizes = new TDigest(100).addMany(totals);

// =============================================================================
// Checks and report
// =============================================================================
const results = {};
const numbers = db.query("SELECT number FROM invoices ORDER BY number").map((r) => Number(r.number.slice(-5)));
results[`${numbers.length} invoice numbers are gap-free and unique`] = numbers.every((n, i) => n === i + 1);
results[`${duplicates} retried usage events were counted once`] =
    duplicates > 0 && db.query("SELECT COUNT(*) AS n FROM usage")[0].n === usageSent - duplicates;
results["graduated tiers price each band at its own rate"] =
    usageCharge(1500).total === 0 + 900 * 40 + 500 * 25 && usageCharge(50).total === 0;
const prorated = allInvoices.find((i) => i.lines.some((l) => l.description.startsWith("Unused time")));
results["a mid-cycle change produces a credit and a charge for the remaining days"] =
    prorated &&
    prorated.lines.some((l) => l.amount < 0) &&
    prorated.lines.some((l) => l.description.startsWith("Remaining time"));
const rc = allInvoices.find((i) => i.reverseCharge);
results["EU B2B customers outside Germany are reverse-charged (0 VAT)"] = rc && rc.tax === 0;
results["coupons discount the first months"] = allInvoices.some((i) =>
    i.lines.some((l) => l.description === "Coupon LAUNCH20"),
);
results["persistent payment failures end in cancellation after the dunning schedule"] =
    flaky.size ===
    db.query(
        "SELECT COUNT(*) AS n FROM subscriptions WHERE id IN ('sub_7','sub_31','sub_55') AND status = 'canceled'",
    )[0].n;
results["each failed retry emits a signed webhook"] =
    db.query("SELECT COUNT(*) AS n FROM events WHERE type = 'payment.failed'")[0].n >= 9;
const ev = db.query("SELECT * FROM events LIMIT 1")[0];
results["webhook signatures verify"] = HMACHex("sha256", WEBHOOK_SECRET, ev.payload) === ev.signature;
const recognisedTotal = [...recognised.values()].sum(),
    billedTotal = [...billed.values()].sum();
results["revenue is recognised over service periods; the remainder is deferred"] =
    Math.abs(recognisedTotal - billedTotal) < billedTotal * 0.25 && deferred > 0;
results["the difference-array waterfall equals a per-day recognition of a sample line"] = (() => {
    const l = lines.find((x) => daysBetween(x.service_start, x.service_end) > 20);
    const span = daysBetween(l.service_start, l.service_end);
    const inMonth = (m) => [...Array(span).keys()].count((d) => addDays(l.service_start, d).startsWith(m));
    const one = new Float64Array(HORIZON + 1);
    const f = pd(l.service_start).epochDay - DAY0;
    one[f] += l.amount / span;
    one[f + span] -= l.amount / span;
    for (let d = 1; d <= HORIZON; d++) one[d] += one[d - 1];
    const m = l.service_start.slice(0, 7);
    return (
        Math.abs(simd.f64Sum(one.subarray(monthStart(m), monthStart(m) + 31)) - (l.amount * inMonth(m)) / span) < 1e-6
    );
})();
results["every month's MRR movements sum to the change in MRR"] = movements.every(
    (m, i) => m.net === mrrSeries[i + 1].mrr - mrrSeries[i].mrr,
);
results["movements include new business, expansion and churn"] =
    movements.some((m) => m.new > 0) && movements.some((m) => m.expansion > 0) && movements.some((m) => m.churn < 0);
results["the ZSet's top account billed at least the TDigest p95 invoice"] = topAccounts[0][1] >= sizes.quantile(0.95);
results[`MRR grew from ${EUR(mrrSeries[0].mrr).format()} to ${EUR(mrrSeries.at(-1).mrr).format()}`] =
    mrrSeries.at(-1).mrr > mrrSeries[0].mrr;

const INVOICE = new Template(
    `Invoice {{number}}\n{{#lines}}  {{description}}  {{amount}}\n{{/lines}}  Subtotal {{subtotal}}\n  VAT {{tax}}\n  Total {{total}}`,
    { escape: false },
);
const sample = prorated || allInvoices[0];
print("billing-platform");
print(
    `  customers  120; invoices ${allInvoices.length}; usage events ${usageSent} (${duplicates} duplicates); credit note ${cn} on ${slaInvoice.number}`,
);
print(
    INVOICE.render({
        ...sample,
        subtotal: EUR(sample.subtotal).format(),
        tax: EUR(sample.tax).format(),
        total: EUR(sample.total).format(),
        lines: sample.lines.map((l) => ({ ...l, amount: EUR(l.amount).format() })),
    })
        .split("\n")
        .map((l) => "  " + l)
        .join("\n"),
);
print(table([["month", "billed", "recognised", "deferred balance"], ...waterfall]));
print(
    table([
        ["date", "MRR", "customers", "new", "expansion", "contraction", "churn"],
        ...mrrSeries.map((r, i) => {
            const m = movements[i - 1];
            return [
                r.d,
                EUR(r.mrr).format(),
                String(r.logos),
                ...(m ? [m.new, m.expansion, m.contraction, m.churn].map((v) => EUR(v).format()) : ["", "", "", ""]),
            ];
        }),
    ]),
);
print(`  top        ${topAccounts.map(([c, v]) => `${c} ${EUR(v).format()}`).join(", ")}`);
print(
    `  invoices   p50 ${EUR(sizes.quantile(0.5)).format()}, p95 ${EUR(sizes.quantile(0.95)).format()}, max ${EUR(simd.f64Max(totals)).format()}`,
);
const status = new DataFrame({ total: Float64Array.from(db.query("SELECT total FROM invoices").map((r) => r.total)) });
print(
    `  receivables ${db
        .query("SELECT status, COUNT(*) AS n FROM invoices GROUP BY status")
        .map((r) => `${r.status} ${r.n}`)
        .join(", ")}; mean invoice ${EUR(status.MEAN("total")).format()}`,
);
db.close();
removeAll(work);
check(results);

function check(results) {
    const names = Object.keys(results);
    const failed = names.filter((name) => !results[name]);
    for (const name of failed) print("FAIL " + name);
    if (failed.length) throw new Error(`${failed.length} of ${names.length} checks failed`);
}
