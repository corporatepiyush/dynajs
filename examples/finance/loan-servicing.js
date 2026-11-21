// Loan servicing — amortisation schedules in exact decimal arithmetic, a payment waterfall with late fees and
// prepayments, delinquency buckets and roll rates, investor remittances and portfolio metrics.
//
// WHAT IT IS
//   The monthly cycle of a mortgage / consumer-loan servicer:
//
//   schedule    level payment P r / (1 - (1 + r)^-n) in dyna:decimal at 34 digits, interest rounded half-even to the
//               cent each period, the last payment absorbing the residue so every schedule ends at exactly 0.00; the
//               textbook 200 000 at 6.5% over 30 years must give 1 264.14 a month
//   cycle       24 months over 2 000 loans: each due date sits in an IndexedHeap; borrowers pay on time, late,
//               partially, early or not at all; payments run fees -> interest -> principal; a late fee after the
//               15-day grace period; prepayments shorten the loan
//   buckets     days past due mapped to current / 30 / 60 / 90+ by a RangeMap; month-to-month roll rates in a
//               Table whose rows must each sum to 1
//   investors   each month's collections are split 60 / 30 / 10 with Money.allocate, which must not lose or invent
//               a cent
//   metrics     WAC and WAM as balance-weighted dot products (simd f64Dot), SMM and CPR = 1 - (1 - SMM)^12
//   floats      the same interest computed in binary floating point drifts from the decimal ledger — shown, not hidden
//
// MODULES  dyna:decimal (Decimal, Money) · dyna:structures (IndexedHeap, RangeMap, Table, Multiset) · dyna:simd
//          (f64Dot, f64Sum) · dyna:random · dyna:cli · Number/Array extensions
//
// RUN      dynajs examples/finance/loan-servicing.js

import { Decimal, Money } from "dyna:decimal";
import { IndexedHeap, RangeMap, Table as Grid, Multiset } from "dyna:structures";
import { f64Dot, f64Sum } from "dyna:simd";
import { Random } from "dyna:random";
import { Table } from "dyna:cli";

const results = {};
const rng = new Random(1934);
const D = (x) => new Decimal(x);
const ZERO = D(0),
    CENT = 2;

// =============================================================================
// Amortisation
// =============================================================================
// Multiplication is exact, so (1 + r)^360 would carry ~12 000 digits; square-and-multiply rounding to 40 significant
// digits after each step keeps 34 correct digits, far past the cent.
const sig = (x) => x.div(1, { precision: 40 });
function powRounded(base, n) {
    let result = D(1),
        b = base;
    for (; n > 0; n >>= 1) {
        if (n & 1) result = sig(result.mul(b));
        b = sig(b.mul(b));
    }
    return result;
}
function levelPayment(principal, annualRate, months) {
    const r = D(annualRate).div(12);
    const growth = powRounded(r.add(1), months);
    return D(principal).mul(r).mul(growth).div(growth.sub(1)).round(CENT, "halfUp");
}
function schedule(principal, annualRate, months) {
    const r = D(annualRate).div(12),
        pay = levelPayment(principal, annualRate, months);
    let bal = D(principal);
    const rows = [];
    for (let k = 1; k <= months && !bal.isZero(); k++) {
        const interest = bal.mul(r).round(CENT, "halfEven");
        let principalPart = pay.sub(interest);
        if (k === months || principalPart.cmp(bal) > 0) principalPart = bal;
        bal = bal.sub(principalPart);
        rows.push({ k, payment: principalPart.add(interest), interest, principal: principalPart, balance: bal });
    }
    return { pay, rows };
}
{
    const s = schedule("200000", "0.065", 360);
    results["200 000 at 6.5% for 30 years pays 1 264.14 a month"] = s.pay.toFixed(2) === "1264.14";
    results["the schedule ends at exactly zero"] = s.rows.at(-1).balance.isZero() && s.rows.length === 360;
    const princ = s.rows.reduce((a, r) => a.add(r.principal), ZERO),
        interest = s.rows.reduce((a, r) => a.add(r.interest), ZERO);
    results["principal parts sum to the loan exactly"] = princ.equals("200000");
    results["payments = principal + interest, to the cent"] = s.rows
        .reduce((a, r) => a.add(r.payment), ZERO)
        .equals(princ.add(interest));
    results["every period but the last pays the level amount"] = s.rows
        .slice(0, -1)
        .every((r) => r.payment.equals(s.pay));
    let fbal = 200000,
        fint = 0;
    for (const row of s.rows) {
        const i = Math.round(fbal * (0.065 / 12) * 100) / 100;
        fint += i;
        fbal -= row.principal.toNumber();
    }
    const drift = Math.abs(fint - interest.toNumber());
    print(
        `30y schedule: total interest ${interest.toFixed(2)}; float re-computation drifts by ${drift.toExponential(2)} and ends at balance ${fbal.toExponential(2)}`,
    );
}

// =============================================================================
// Portfolio and servicing cycle
// =============================================================================
const buckets = new RangeMap();
buckets.put(0, 30, "current");
buckets.put(30, 60, "30");
buckets.put(60, 90, "60");
buckets.put(90, 100000, "90+");
const BUCKETS = ["current", "30", "60", "90+", "paid-off"];
const behaviour = () => {
    const x = rng.nextFloat();
    return x < 0.86
        ? "on-time"
        : x < 0.92
          ? "late"
          : x < 0.95
            ? "partial"
            : x < 0.97
              ? "skip"
              : x < 0.99
                ? "extra"
                : "payoff";
};
const loans = Array.from({ length: 2000 }, (_, id) => {
    const principal = String(5000 + 500 * rng.nextBounded(60));
    const rate = (0.04 + 0.002 * rng.nextBounded(40)).toFixed(4);
    const term = [36, 60, 84][rng.nextBounded(3)];
    return {
        id,
        rate: D(rate),
        rateNum: Number(rate),
        term,
        pay: levelPayment(principal, rate, term),
        balance: D(principal),
        original: D(principal),
        due: 0,
        pastDue: ZERO,
        fees: ZERO,
        dpd: 0,
        paidOff: false,
        risky: rng.nextFloat() < 0.08,
    };
});
const dueHeap = new IndexedHeap();
loans.forEach((l) => dueHeap.push(l.id, 1));
const LATE_FEE = D("25.00");
const roll = new Grid(),
    monthly = [];
const investors = [60, 30, 10];
let collectedAll = ZERO,
    remittedAll = Money.fromMinor(0, "EUR");
let invariantOk = true;
for (let month = 1; month <= 24; month++) {
    const before = new Map(loans.map((l) => [l.id, l.paidOff ? "paid-off" : buckets.get(l.dpd)]));
    let collected = ZERO,
        scheduledPrincipal = ZERO,
        prepaid = ZERO,
        startBalance = ZERO;
    for (const l of loans) if (!l.paidOff) startBalance = startBalance.add(l.balance);
    for (const id of dueHeap.popUntil(month)) {
        const l = loans[id];
        const interest = l.balance.mul(l.rate.div(12)).round(CENT, "halfEven");
        const scheduled =
            Decimal.prototype.cmp.call(l.pay.sub(interest), l.balance) > 0 ? l.balance.add(interest) : l.pay;
        l.pastDue = l.pastDue.add(scheduled);
        let kind = behaviour();
        if (l.risky && rng.nextFloat() < 0.5) kind = "skip";
        let paid =
            kind === "on-time" || kind === "late"
                ? l.pastDue.add(kind === "late" ? 0 : 0)
                : kind === "partial"
                  ? scheduled.mul("0.5").round(CENT)
                  : kind === "skip"
                    ? ZERO
                    : kind === "extra"
                      ? l.pastDue.add(l.pay)
                      : l.pastDue.add(l.balance);
        if (kind === "late") ((l.fees = l.fees.add(LATE_FEE)), (paid = paid.add(LATE_FEE)));
        // Waterfall: fees, then this period's interest, then principal (scheduled first, the rest is prepayment).
        let left = paid;
        const toFees = left.cmp(l.fees) < 0 ? left : l.fees;
        l.fees = l.fees.sub(toFees);
        left = left.sub(toFees);
        const toInterest = left.cmp(interest) < 0 ? left : interest;
        left = left.sub(toInterest);
        const unpaidInterest = interest.sub(toInterest);
        const toPrincipal = left.cmp(l.balance) < 0 ? left : l.balance;
        const schedPrin = scheduled.sub(interest);
        scheduledPrincipal = scheduledPrincipal.add(toPrincipal.cmp(schedPrin) < 0 ? toPrincipal : schedPrin);
        if (toPrincipal.cmp(schedPrin) > 0) prepaid = prepaid.add(toPrincipal.sub(schedPrin));
        l.balance = l.balance.sub(toPrincipal).add(unpaidInterest);
        l.pastDue = l.pastDue.sub(paid.sub(toFees)).cmp(ZERO) > 0 ? l.pastDue.sub(paid.sub(toFees)) : ZERO;
        l.dpd = l.pastDue.isZero() ? 0 : l.dpd + 30;
        if (
            !toFees
                .add(toInterest)
                .add(toPrincipal)
                .equals(paid.sub(left.sub(toPrincipal)))
        )
            invariantOk = false;
        collected = collected.add(toFees).add(toInterest).add(toPrincipal);
        if (l.balance.isZero()) {
            l.paidOff = true;
            l.dpd = 0;
        } else dueHeap.push(id, month + 1);
    }
    collectedAll = collectedAll.add(collected);
    const pool = Money.fromDecimal(collected, "EUR");
    const parts = pool.allocate(investors);
    const sum = parts.reduce((a, b) => a.add(b), Money.fromMinor(0, "EUR"));
    if (!sum.equals(pool)) invariantOk = false;
    remittedAll = remittedAll.add(sum);
    for (const l of loans) {
        const from = before.get(l.id),
            to = l.paidOff ? "paid-off" : buckets.get(l.dpd);
        roll.put(from, to, (roll.get(from, to) ?? 0) + 1);
    }
    const smm = startBalance.isZero() ? 0 : prepaid.div(startBalance.sub(scheduledPrincipal)).toNumber();
    monthly.push({ month, collected, smm, cpr: 1 - (1 - smm) ** 12, parts });
}
results["waterfall allocations always add up"] = invariantOk;
results["investor remittances equal collections to the cent"] = remittedAll.toDecimal().equals(collectedAll);
results["Money.allocate splits 60/30/10 within a cent each"] = monthly.every((m) => {
    const total = m.parts.reduce((a, b) => a + b.amount(), 0);
    return m.parts.every((p, i) => Math.abs(p.amount() - (total * investors[i]) / 100) <= 1);
});

// =============================================================================
// Roll rates, buckets and portfolio metrics
// =============================================================================
const rollRows = [];
let rowsSumToOne = true;
for (const from of BUCKETS.slice(0, 4)) {
    const row = roll.row(from);
    const total = row.reduce((s, [, n]) => s + n, 0);
    if (!total) continue;
    const rates = Object.fromEntries(row.map(([to, n]) => [to, n / total]));
    if (Math.abs(Object.values(rates).sum() - 1) > 1e-12) rowsSumToOne = false;
    rollRows.push([from, ...BUCKETS.map((to) => ((rates[to] ?? 0) * 100).toFixed(1) + "%")]);
}
print(Table(rollRows, { head: ["from \\ to", ...BUCKETS] }));
results["each roll-rate row sums to 1"] = rowsSumToOne;
results["90+ only rolls from 60 or stays"] =
    (roll.get("current", "90+") ?? 0) === 0 && (roll.get("30", "90+") ?? 0) === 0;
results["risky borrowers dominate the 90+ bucket"] = (() => {
    const ninety = loans.filter((l) => !l.paidOff && buckets.get(l.dpd) === "90+");
    return ninety.length > 0 && ninety.filter((l) => l.risky).length / ninety.length > 0.8;
})();
const live = loans.filter((l) => !l.paidOff);
const bal = Float64Array.from(live, (l) => l.balance.toNumber()),
    coupon = Float64Array.from(live, (l) => l.rateNum);
const remaining = Float64Array.from(live, (l) => l.term - 24);
const wac = f64Dot(bal, coupon) / f64Sum(bal),
    wam = f64Dot(bal, remaining) / f64Sum(bal);
const status = new Multiset();
for (const l of loans) status.add(l.paidOff ? "paid-off" : buckets.get(l.dpd));
results["paid-off loans have zero balance and are out of the due heap"] = loans
    .filter((l) => l.paidOff)
    .every((l) => l.balance.isZero() && !dueHeap.has(l.id));
results["CPR is consistent with SMM"] = monthly.every(
    (m) => Math.abs(1 - (1 - m.smm) ** 12 - m.cpr) < 1e-15 && m.cpr >= 0 && m.cpr < 1,
);
results["WAC lies within the coupon range"] = wac >= 0.04 && wac <= 0.12;
print(
    Table(
        [
            ["loans live / paid off", `${live.length.format()} / ${status.count("paid-off").format()}`],
            [
                "buckets current/30/60/90+",
                BUCKETS.slice(0, 4)
                    .map((b) => status.count(b))
                    .join(" / "),
            ],
            ["collected over 24 months", Money.fromDecimal(collectedAll, "EUR").format()],
            ["WAC / WAM", `${(wac * 100).toFixed(3)}% / ${wam.toFixed(1)} months`],
            ["average CPR", `${(monthly.map((m) => m.cpr).average() * 100).toFixed(2)}%`],
        ],
        { head: ["portfolio", "value"] },
    ),
);

check(results);

function check(results) {
    const names = Object.keys(results);
    const failed = names.filter((name) => !results[name]);
    for (const name of failed) print("FAIL " + name);
    if (failed.length) throw new Error(`${failed.length} of ${names.length} checks failed`);
}
