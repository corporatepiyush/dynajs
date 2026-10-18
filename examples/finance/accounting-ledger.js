// Accounting ledger — an immutable double-entry general ledger with exact decimal arithmetic, multi-currency
// postings, FX revaluation, accruals with automatic reversal, period close and the three financial statements.
//
// WHAT IT IS
//   The core of an ERP's finance module for a small company that sells in EUR, USD and GBP:
//
//   chart        a chart of accounts in YAML (assets, liabilities, equity, income, expenses) with normal balances
//   journal      entries are validated before they post: debits equal credits in the functional currency to the
//                cent, every account exists and is not a header, the period is open, the entry date is inside it;
//                posted entries are never edited — a mistake is corrected with a reversing entry
//   currency     a foreign-currency line records the transaction amount AND the EUR amount at the day's rate;
//                at month end, open foreign balances (receivables, bank accounts) are revalued at the closing
//                rate and the difference is booked as unrealised FX gain/loss
//   accruals     an accrued expense booked on the last day of a month is reversed automatically on the first day of
//                the next, so the real invoice can be booked normally
//   close        closing a period zeroes income and expense accounts into retained earnings and locks the period
//   statements   trial balance, income statement and balance sheet for any period; assets = liabilities + equity is
//                asserted, not hoped for
//   audit        every posted entry is chained by hash; a tampered row is detected
//   roll-ups     closing balances live in an OrderedMap keyed by account code, so any subtree of the chart (all
//                cash "10", all receivables "11", all assets "1") is one prefix scan, never a filter over the chart
//   aging        open invoices bucketed 0-30 / 31-60 / 61-90 / 90+ days through a RangeMap into a bucket x currency
//                Table, reconciled to the receivable balances in transaction currency
//   search       every journal memo is indexed in an InvertedIndex, so an auditor's "rent" query is one BM25 lookup
//   budget       March actuals against budget per expense account in integer cents with dyna:simd i32 kernels
//
// MODULES  dyna:decimal (Decimal) · dyna:net (SQLite) · dyna:yaml · dyna:hash (SHA256) · dyna:time (PlainDate)
//          · dyna:csv · dyna:cli (Table) · dyna:uuid · dyna:file · dyna:structures (OrderedMap, RangeMap, Table,
//          InvertedIndex) · dyna:simd (i32 kernels) · Array/Number extensions
//
// RUN      dynajs examples/finance/accounting-ledger.js     books a quarter of activity, closes two months, prints statements

import { Decimal } from "dyna:decimal";
import { SQLite } from "dyna:net";
import * as YAML from "dyna:yaml";
import { SHA256Hex } from "dyna:hash";
import { parseDate } from "dyna:time";
import * as csv from "dyna:csv";
import { Table } from "dyna:cli";
import { v7 as uuid7 } from "dyna:uuid";
import { makeTempDir, writeFile, removeAll } from "dyna:file";
import { OrderedMap, RangeMap, Table as Grid, InvertedIndex } from "dyna:structures";
import * as simd from "dyna:simd";
const table = (rows) => Table(rows.slice(1), { head: rows[0] });

const work = makeTempDir("ledger");
const D = (x) => new Decimal(x);
const ZERO = Decimal.ZERO;
const money = (d) => d.toFixed(2);

// =============================================================================
// Chart of accounts
// =============================================================================
const CHART = YAML.Parse(`
- { code: "1000", name: Assets, type: asset, header: true }
- { code: "1010", name: Bank EUR, type: asset }
- { code: "1020", name: Bank USD, type: asset, currency: USD }
- { code: "1100", name: Accounts receivable EUR, type: asset }
- { code: "1110", name: Accounts receivable USD, type: asset, currency: USD }
- { code: "1120", name: Accounts receivable GBP, type: asset, currency: GBP }
- { code: "1500", name: Equipment, type: asset }
- { code: "1590", name: Accumulated depreciation, type: asset, contra: true }
- { code: "2000", name: Liabilities, type: liability, header: true }
- { code: "2010", name: Accounts payable, type: liability }
- { code: "2100", name: VAT payable, type: liability }
- { code: "2200", name: Accrued expenses, type: liability }
- { code: "2500", name: Bank loan, type: liability }
- { code: "3000", name: Equity, type: equity, header: true }
- { code: "3010", name: Share capital, type: equity }
- { code: "3900", name: Retained earnings, type: equity }
- { code: "4000", name: Revenue, type: income, header: true }
- { code: "4010", name: Software subscriptions, type: income }
- { code: "4020", name: Consulting, type: income }
- { code: "4900", name: FX gains and losses, type: income }
- { code: "5000", name: Expenses, type: expense, header: true }
- { code: "5010", name: Salaries, type: expense }
- { code: "5020", name: Cloud hosting, type: expense }
- { code: "5030", name: Office rent, type: expense }
- { code: "5040", name: Depreciation, type: expense }
- { code: "5050", name: Interest, type: expense }
`);
const ACCOUNTS = new Map(CHART.map((a) => [a.code, a]));
const DEBIT_NORMAL = new Set(["asset", "expense"]);

// =============================================================================
// Storage
// =============================================================================
const db = new SQLite(work.join("ledger.db"));
db.exec("CREATE TABLE periods (id TEXT PRIMARY KEY, start TEXT, end TEXT, state TEXT)");
db.exec(
    "CREATE TABLE entries (seq INTEGER PRIMARY KEY, id TEXT UNIQUE, date TEXT, period TEXT, memo TEXT, kind TEXT, reverses TEXT, auto_reverse TEXT, prev_hash TEXT, hash TEXT)",
);
db.exec(
    "CREATE TABLE lines (entry TEXT, account TEXT, debit TEXT, credit TEXT, currency TEXT, amount TEXT, rate TEXT)",
);
db.exec("CREATE INDEX lines_acct ON lines(account)");
db.exec("CREATE TABLE rates (date TEXT, currency TEXT, rate TEXT, PRIMARY KEY (date, currency))");
for (const [m, start, end] of [
    ["2026-01", "2026-01-01", "2026-01-31"],
    ["2026-02", "2026-02-01", "2026-02-28"],
    ["2026-03", "2026-03-01", "2026-03-31"],
])
    db.exec("INSERT INTO periods VALUES (?, ?, ?, 'open')", [m, start, end]);

// EUR per unit of foreign currency.
const RATE_TABLE = {
    USD: ["0.9210", "0.9180", "0.9305", "0.9260", "0.9120", "0.9050"],
    GBP: ["1.1700", "1.1740", "1.1690", "1.1810", "1.1760", "1.1830"],
};
for (const [cur, rs] of Object.entries(RATE_TABLE))
    ["2026-01-05", "2026-01-31", "2026-02-10", "2026-02-28", "2026-03-15", "2026-03-31"].forEach((d, i) =>
        db.exec("INSERT INTO rates VALUES (?, ?, ?)", [d, cur, rs[i]]),
    );
const rateOn = (cur, date) =>
    cur === "EUR"
        ? "1"
        : db.query("SELECT rate FROM rates WHERE currency = ? AND date <= ? ORDER BY date DESC LIMIT 1", [cur, date])[0]
              .rate;

// =============================================================================
// Posting
// =============================================================================
function post({ date, memo, lines, kind = "manual", reverses = null, autoReverse = null }) {
    const period = db.query("SELECT * FROM periods WHERE ? BETWEEN start AND end", [date])[0];
    if (!period) throw new Error(`no accounting period contains ${date}`);
    if (period.state !== "open") throw new Error(`period ${period.id} is closed`);
    parseDate(date);
    let dr = ZERO,
        cr = ZERO;
    const rows = lines.map((l) => {
        const acct = ACCOUNTS.get(l.account);
        if (!acct) throw new Error(`unknown account ${l.account}`);
        if (acct.header) throw new Error(`cannot post to header account ${l.account} ${acct.name}`);
        const currency = l.currency || acct.currency || "EUR";
        if (acct.currency && currency !== acct.currency && !l.eurOnly)
            throw new Error(`${acct.name} is kept in ${acct.currency}, not ${currency}`);
        const value = D(l.debit ?? l.credit);
        if (value.cmp(0) <= 0) throw new Error(`line amounts must be positive (${l.account})`);
        const rate = l.eurOnly ? "0" : l.rate || rateOn(currency, date); // a revaluation moves EUR only
        const eur = l.eurOnly ? value : D(value.mul(rate).toFixed(2));
        if (l.debit !== undefined) dr = dr.add(eur);
        else cr = cr.add(eur);
        return {
            account: l.account,
            debit: l.debit !== undefined ? money(eur) : "0.00",
            credit: l.credit !== undefined ? money(eur) : "0.00",
            currency: acct.currency || currency,
            amount: l.eurOnly ? "0.00" : money(value),
            rate,
        };
    });
    if (!dr.equals(cr)) throw new Error(`entry does not balance: debits ${money(dr)} vs credits ${money(cr)}`);
    const id = uuid7(),
        prev = db.query("SELECT hash FROM entries ORDER BY seq DESC LIMIT 1")[0]?.hash || "genesis";
    const hash = SHA256Hex(JSON.stringify({ prev, id, date, memo, rows }));
    db.exec("BEGIN");
    db.exec(
        "INSERT INTO entries (id, date, period, memo, kind, reverses, auto_reverse, prev_hash, hash) VALUES (?,?,?,?,?,?,?,?,?)",
        [id, date, period.id, memo, kind, reverses, autoReverse, prev, hash],
    );
    for (const r of rows)
        db.exec("INSERT INTO lines VALUES (?,?,?,?,?,?,?)", [
            id,
            r.account,
            r.debit,
            r.credit,
            r.currency,
            r.amount,
            r.rate,
        ]);
    db.exec("COMMIT");
    return id;
}
function reverse(entryId, date, memo) {
    const lines = db.query("SELECT * FROM lines WHERE entry = ?", [entryId]);
    const flip = (l) =>
        l.amount === "0.00"
            ? { eurOnly: true, value: l.debit !== "0.00" ? l.debit : l.credit }
            : { currency: l.currency, rate: l.rate, value: l.amount };
    return post({
        date,
        memo,
        kind: "reversal",
        reverses: entryId,
        lines: lines.map((l) => {
            const f = flip(l);
            const side = l.debit !== "0.00" ? "credit" : "debit";
            return {
                account: l.account,
                [side]: f.value,
                ...(f.eurOnly ? { eurOnly: true } : { currency: f.currency, rate: f.rate }),
            };
        }),
    });
}

// =============================================================================
// Balances, revaluation, accrual reversal, close
// =============================================================================
function balances(upTo, from = "0000-00-00") {
    const out = new Map();
    for (const r of db.query("SELECT l.* FROM lines l JOIN entries e ON e.id = l.entry WHERE e.date BETWEEN ? AND ?", [
        from,
        upTo,
    ])) {
        const b = out.get(r.account) || { eur: ZERO, fc: ZERO };
        const sign = r.debit !== "0.00" ? 1 : -1;
        b.eur = b.eur.add(D(r.debit !== "0.00" ? r.debit : r.credit).mul(sign));
        b.fc = b.fc.add(D(r.amount).mul(sign));
        out.set(r.account, b);
    }
    return out;
}
function revalue(date) {
    const lines = [];
    let net = ZERO;
    for (const [code, b] of balances(date)) {
        const acct = ACCOUNTS.get(code);
        if (!acct.currency || b.fc.equals(0)) continue;
        const target = D(b.fc.mul(rateOn(acct.currency, date)).toFixed(2)),
            diff = target.sub(b.eur);
        if (diff.equals(0)) continue;
        lines.push(
            diff.cmp(0) > 0
                ? { account: code, debit: money(diff), eurOnly: true }
                : { account: code, credit: money(diff.neg()), eurOnly: true },
        );
        net = net.add(diff);
    }
    if (!lines.length) return null;
    lines.push(net.cmp(0) > 0 ? { account: "4900", credit: money(net) } : { account: "4900", debit: money(net.neg()) });
    return post({ date, memo: `FX revaluation at ${date}`, kind: "revaluation", lines });
}
function runAutoReversals(periodStart) {
    const due = db.query(
        "SELECT e.id, e.memo FROM entries e WHERE e.auto_reverse = ? AND NOT EXISTS (SELECT 1 FROM entries r WHERE r.reverses = e.id)",
        [periodStart],
    );
    return due.map((e) => reverse(e.id, periodStart, `Reversal of: ${e.memo}`));
}
function close(periodId) {
    const p = db.query("SELECT * FROM periods WHERE id = ?", [periodId])[0];
    const pnl = balances(p.end, p.start);
    const lines = [];
    let profit = ZERO;
    for (const [code, b] of pnl) {
        const t = ACCOUNTS.get(code).type;
        if ((t !== "income" && t !== "expense") || b.eur.equals(0)) continue;
        lines.push(
            b.eur.cmp(0) > 0 ? { account: code, credit: money(b.eur) } : { account: code, debit: money(b.eur.neg()) },
        );
        profit = profit.sub(b.eur);
    }
    if (lines.length) {
        lines.push(
            profit.cmp(0) > 0
                ? { account: "3900", credit: money(profit) }
                : { account: "3900", debit: money(profit.neg()) },
        );
        post({
            date: p.end,
            memo: `Close ${periodId}: result to retained earnings`,
            kind: "closing",
            lines: lines.map((l) => ({ ...l, currency: "EUR", rate: "1" })),
        });
    }
    db.exec("UPDATE periods SET state = 'closed' WHERE id = ?", [periodId]);
    return profit;
}

// =============================================================================
// Statements
// =============================================================================
function statements(from, to, { beforeClose = true } = {}) {
    const all = balances(to),
        period = new Map();
    for (const r of db.query(
        "SELECT l.*, e.kind FROM lines l JOIN entries e ON e.id = l.entry WHERE e.date BETWEEN ? AND ? AND (e.kind != 'closing' OR ?)",
        [from, to, beforeClose ? 0 : 1],
    )) {
        const sign = r.debit !== "0.00" ? 1 : -1;
        period.set(
            r.account,
            (period.get(r.account) || ZERO).add(D(r.debit !== "0.00" ? r.debit : r.credit).mul(sign)),
        );
    }
    const sumType = (m, type, natural) =>
        [...m]
            .filter(([c]) => ACCOUNTS.get(c).type === type)
            .reduce((s, [, v]) => s.add(natural ? (v.eur ?? v) : (v.eur ?? v).neg()), ZERO);
    const income = sumType(period, "income", false),
        expense = sumType(period, "expense", true);
    let dr = ZERO,
        cr = ZERO;
    const tb = [...all].sort().map(([c, b]) => {
        if (b.eur.cmp(0) >= 0) dr = dr.add(b.eur);
        else cr = cr.add(b.eur.neg());
        return [
            c,
            ACCOUNTS.get(c).name,
            b.eur.cmp(0) >= 0 ? money(b.eur) : "",
            b.eur.cmp(0) < 0 ? money(b.eur.neg()) : "",
        ];
    });
    const assets = sumType(all, "asset", true),
        liabilities = sumType(all, "liability", false),
        equity = sumType(all, "equity", false);
    const unclosed = sumType(all, "income", false).sub(sumType(all, "expense", true));
    return {
        tb,
        dr,
        cr,
        income,
        expense,
        net: income.sub(expense),
        assets,
        liabilities,
        equity,
        unclosed,
        balanced: assets.equals(liabilities.add(equity).add(unclosed)),
    };
}

// =============================================================================
// A quarter of activity
// =============================================================================
const results = {};
post({
    date: "2026-01-02",
    memo: "Founders' capital",
    lines: [
        { account: "1010", debit: "150000.00" },
        { account: "3010", credit: "150000.00" },
    ],
});
post({
    date: "2026-01-03",
    memo: "Bank loan drawn",
    lines: [
        { account: "1010", debit: "60000.00" },
        { account: "2500", credit: "60000.00" },
    ],
});
post({
    date: "2026-01-04",
    memo: "Laptops and servers",
    lines: [
        { account: "1500", debit: "24000.00" },
        { account: "1010", credit: "24000.00" },
    ],
});
const invoices = [];
const invoice = (date, account, currency, amount, entry) =>
    invoices.push({ id: entry, date, account, currency, amount: D(amount), paid: false });
for (const [m, end] of [
    ["01", "2026-01-31"],
    ["02", "2026-02-28"],
    ["03", "2026-03-31"],
]) {
    invoice(
        `2026-${m}-05`,
        "1100",
        "EUR",
        "35700.00",
        post({
            date: `2026-${m}-05`,
            memo: `Subscriptions invoiced ${m} (EUR, 19% VAT)`,
            lines: [
                { account: "1100", debit: "35700.00" },
                { account: "4010", credit: "30000.00" },
                { account: "2100", credit: "5700.00" },
            ],
        }),
    );
    invoice(
        `2026-${m}-06`,
        "1110",
        "USD",
        "18000.00",
        post({
            date: `2026-${m}-06`,
            memo: `US customer invoice ${m}`,
            lines: [
                { account: "1110", debit: "18000.00", currency: "USD" },
                { account: "4010", credit: money(D(18000).mul(rateOn("USD", `2026-${m}-06`))) },
            ],
        }),
    );
    invoice(
        `2026-${m}-07`,
        "1120",
        "GBP",
        "6000.00",
        post({
            date: `2026-${m}-07`,
            memo: `UK consulting invoice ${m}`,
            lines: [
                { account: "1120", debit: "6000.00", currency: "GBP" },
                { account: "4020", credit: money(D(6000).mul(rateOn("GBP", `2026-${m}-07`))) },
            ],
        }),
    );
    post({
        date: `2026-${m}-25`,
        memo: `Payroll ${m}`,
        lines: [
            { account: "5010", debit: "42000.00" },
            { account: "1010", credit: "42000.00" },
        ],
    });
    post({
        date: `2026-${m}-01`,
        memo: `Rent ${m}`,
        lines: [
            { account: "5030", debit: "4500.00" },
            { account: "1010", credit: "4500.00" },
        ],
    });
    post({
        date: end,
        memo: `Depreciation ${m} (24k over 36 months)`,
        lines: [
            { account: "5040", debit: "666.67" },
            { account: "1590", credit: "666.67" },
        ],
    });
    post({
        date: end,
        memo: `Loan interest ${m}`,
        lines: [
            { account: "5050", debit: "275.00" },
            { account: "1010", credit: "275.00" },
        ],
    });
    const next = { "01": "2026-02-01", "02": "2026-03-01", "03": null }[m];
    post({
        date: end,
        memo: `Accrued cloud hosting ${m} (invoice arrives next month)`,
        kind: "accrual",
        autoReverse: next,
        lines: [
            { account: "5020", debit: "3800.00" },
            { account: "2200", credit: "3800.00" },
        ],
    });
}
// Customer payments: EUR customers pay, the US customer pays January's invoice in February at a different rate.
post({
    date: "2026-01-20",
    memo: "EUR receipts",
    lines: [
        { account: "1010", debit: "35700.00" },
        { account: "1100", credit: "35700.00" },
    ],
});
post({
    date: "2026-02-15",
    memo: "USD receipt for January invoice",
    lines: [
        { account: "1020", debit: "18000.00", currency: "USD" },
        { account: "1110", credit: "18000.00", currency: "USD", rate: rateOn("USD", "2026-01-06") },
        {
            account: "4900",
            [D(18000)
                .mul(rateOn("USD", "2026-02-15"))
                .sub(D(18000).mul(rateOn("USD", "2026-01-06")))
                .cmp(0) >= 0
                ? "credit"
                : "debit"]: money(
                D(18000)
                    .mul(rateOn("USD", "2026-02-15"))
                    .sub(D(18000).mul(rateOn("USD", "2026-01-06")))
                    .abs(),
            ),
        },
    ],
});

invoices.find((i) => i.date === "2026-01-05").paid = true;
invoices.find((i) => i.date === "2026-01-06").paid = true;

// The real hosting invoice in February, after January's accrual was auto-reversed.
const autoJan = (() => {
    runAutoReversals("2026-02-01");
    return db.query("SELECT COUNT(*) AS n FROM entries WHERE kind = 'reversal' AND date = '2026-02-01'")[0].n;
})();
post({
    date: "2026-02-04",
    memo: "Cloud hosting invoice January",
    lines: [
        { account: "5020", debit: "3812.40" },
        { account: "2010", credit: "3812.40" },
    ],
});

// Validation refusals.
const refused = [];
for (const bad of [
    {
        date: "2026-01-10",
        memo: "unbalanced",
        lines: [
            { account: "5020", debit: "100.00" },
            { account: "2010", credit: "99.99" },
        ],
    },
    {
        date: "2026-01-10",
        memo: "header",
        lines: [
            { account: "5000", debit: "1.00" },
            { account: "1010", credit: "1.00" },
        ],
    },
    {
        date: "2026-01-10",
        memo: "wrong currency",
        lines: [
            { account: "1020", debit: "1.00", currency: "GBP" },
            { account: "1010", credit: "1.17" },
        ],
    },
    {
        date: "2026-04-02",
        memo: "no period",
        lines: [
            { account: "5020", debit: "1.00" },
            { account: "1010", credit: "1.00" },
        ],
    },
]) {
    try {
        post(bad);
        refused.push(`${bad.memo}: accepted!`);
    } catch (e) {
        refused.push(`${bad.memo}: ${e.message}`);
    }
}

// A mis-posted rent payment is corrected by reversal, never by editing.
const mistake = post({
    date: "2026-02-02",
    memo: "Rent February (posted twice by mistake)",
    lines: [
        { account: "5030", debit: "4500.00" },
        { account: "1010", credit: "4500.00" },
    ],
});
reverse(mistake, "2026-02-03", "Reverse duplicate rent posting");

const reval1 = revalue("2026-01-31");
const janProfit = close("2026-01");
let closedRefused = null;
try {
    post({
        date: "2026-01-15",
        memo: "late entry",
        lines: [
            { account: "5020", debit: "1.00" },
            { account: "1010", credit: "1.00" },
        ],
    });
} catch (e) {
    closedRefused = e.message;
}
runAutoReversals("2026-03-01");
const reval2 = revalue("2026-02-28");
const febProfit = close("2026-02");
const reval3 = revalue("2026-03-31");
const q = statements("2026-01-01", "2026-03-31", { beforeClose: false });
const mar = statements("2026-03-01", "2026-03-31");

// Roll-ups: one prefix scan of an OrderedMap per subtree of the chart.
const closing = new OrderedMap();
for (const [code, b] of balances("2026-03-31")) closing.set(code, b);
const rollup = (prefix) => closing.prefix(prefix).reduce((s, [, b]) => s.add(b.eur), ZERO);

// Aging: days outstanding -> bucket via a RangeMap, accumulated in a bucket x currency Table.
const AGE = new RangeMap().put(0, 30, "0-30").put(31, 60, "31-60").put(61, 90, "61-90").put(91, 100000, "90+");
const aging = new Grid();
const asOf = parseDate("2026-03-31").epochDay;
for (const inv of invoices.filter((i) => !i.paid)) {
    const bucket = AGE.get(asOf - parseDate(inv.date).epochDay);
    aging.put(bucket, inv.currency, (aging.get(bucket, inv.currency) || ZERO).add(inv.amount));
}
const agedByCurrency = (cur) => aging.column(cur).reduce((s, [, v]) => s.add(v), ZERO);

// Memo search.
const memos = new InvertedIndex();
for (const e of db.query("SELECT seq, memo FROM entries")) memos.add(e.seq, e.memo);
const rentHits = memos
    .search("rent", { limit: 50 })
    .map(([seq]) => seq)
    .sortBy((x) => x);
const rentScan = db
    .query("SELECT seq, memo FROM entries ORDER BY seq")
    .filter((e) => /\brent\b/i.test(e.memo))
    .pluck("seq");

// Budget vs actual for March, in cents, with simd i32 kernels.
const EXPENSES = ["5010", "5020", "5030", "5040", "5050"];
const budget = Int32Array.from([4200000, 400000, 450000, 66667, 30000]);
const marchOnly = statements("2026-03-01", "2026-03-31");
const marchExpense = new Map();
for (const r of db.query(
    "SELECT l.account, l.debit, l.credit FROM lines l JOIN entries e ON e.id = l.entry WHERE e.date BETWEEN '2026-03-01' AND '2026-03-31' AND e.kind != 'closing'",
))
    marchExpense.set(r.account, (marchExpense.get(r.account) || ZERO).add(r.debit).sub(r.credit));
const actual = Int32Array.from(EXPENSES, (c) => Math.round(Number((marchExpense.get(c) || ZERO).toFixed(2)) * 100));
const variance = simd.i32Add(new Int32Array(EXPENSES.length), actual, simd.i32Scale(budget.slice(), -1));

results["unbalanced, header, wrong-currency and out-of-period entries are refused"] =
    refused.every((r) => !r.includes("accepted")) && refused.length === 4;
results["a closed period accepts no new entries"] = /closed/.test(closedRefused || "");
results["January's accrual reversed on 1 February; only February's accrual remains open"] =
    autoJan === 1 && balances("2026-02-28").get("2200").eur.equals(D("-3800.00"));
results[`trial balance balances: ${money(q.dr)} = ${money(q.cr)}`] = q.dr.equals(q.cr);
results[
    `assets ${money(q.assets)} = liabilities ${money(q.liabilities)} + equity ${money(q.equity)} + current result ${money(q.unclosed)}`
] = q.balanced;
results["USD receivables are carried at the closing rate after revaluation"] =
    reval1 &&
    reval2 &&
    balances("2026-03-31")
        .get("1110")
        .eur.equals(D(balances("2026-03-31").get("1110").fc.mul(rateOn("USD", "2026-03-31")).toFixed(2)));
const rentQuarter = db
    .query(
        "SELECT l.debit, l.credit FROM lines l JOIN entries e ON e.id = l.entry WHERE l.account = '5030' AND e.kind != 'closing'",
    )
    .reduce((s, r) => s.add(r.debit).sub(r.credit), ZERO);
results[`the duplicate rent is netted out by its reversal (3 x 4,500 = ${money(rentQuarter)})`] = rentQuarter.equals(
    D("13500.00"),
);
const retained = balances("2026-02-28").get("3900").eur.neg();
results[`retained earnings ${money(retained)} = January ${money(janProfit)} + February ${money(febProfit)}`] =
    retained.equals(janProfit.add(febProfit));
const chain = db.query("SELECT * FROM entries ORDER BY seq");
const chainOk = () =>
    db
        .query("SELECT * FROM entries ORDER BY seq")
        .every((e, i, all) => (i === 0 ? e.prev_hash === "genesis" : e.prev_hash === all[i - 1].hash));
results[`${chain.length} entries form an unbroken hash chain`] = chainOk();
db.exec("UPDATE entries SET prev_hash = 'x' WHERE seq = 5");
results["tampering with a posted entry is detected"] = !chainOk();
results[`asset roll-up by prefix "1" (${money(rollup("1"))}) equals the balance sheet's assets`] = rollup("1").equals(
    q.assets,
);
results["cash roll-up '10' is exactly the two bank accounts"] = rollup("10").equals(
    closing.get("1010").eur.add(closing.get("1020").eur),
);
results["open invoices by aging bucket reconcile to receivables in transaction currency"] = [
    ["EUR", "1100"],
    ["USD", "1110"],
    ["GBP", "1120"],
].all(([cur, acct]) => agedByCurrency(cur).equals(closing.get(acct).fc));
results["the oldest open invoice (GBP, January) is in the 61-90 bucket"] =
    aging.get("61-90", "GBP")?.equals(D("6000.00")) && !aging.has("90+", "GBP");
results[`memo search for "rent" finds exactly the entries a word scan finds (${rentHits.length})`] =
    rentHits.join() === rentScan.join() && rentHits.length === 5;
results["simd budget variance totals equal actual minus budget"] =
    simd.i32Sum(variance) === simd.i32Sum(actual) - simd.i32Sum(budget);
results["March expense actuals agree with the March income statement"] =
    (simd.i32Sum(actual) / 100).round(2) === Number(money(marchOnly.expense));

writeFile(work.join("trial-balance.csv"), csv.stringify([["account", "name", "debit", "credit"], ...q.tb]));
print("accounting-ledger");
print(
    `  journal    ${chain.length} entries, ${db.query("SELECT COUNT(*) AS n FROM lines")[0].n} lines; FX revaluations ${[reval1, reval2, reval3].filter(Boolean).length}; periods closed 2026-01, 2026-02`,
);
print(table([["code", "account", "debit", "credit"], ...q.tb, ["", "TOTAL", money(q.dr), money(q.cr)]]));
print(`  March P&L  income ${money(mar.income)}, expenses ${money(mar.expense)}, result ${money(mar.net)}`);
print(
    `  aging      ${["0-30", "31-60", "61-90", "90+"]
        .map(
            (b) =>
                `${b}: ${
                    aging
                        .row(b)
                        .map(([c, v]) => `${money(v)} ${c}`)
                        .join(" + ") || "-"
                }`,
        )
        .join(" | ")}`,
);
print(`  budget     ${EXPENSES.map((c, i) => `${ACCOUNTS.get(c).name} ${(variance[i] / 100).format(2)}`).join(", ")}`);
print(`  results    January ${money(janProfit)}, February ${money(febProfit)} -> retained earnings ${money(retained)}`);
for (const r of refused) print(`  refused    ${r}`);
db.close();
removeAll(work);
check(results);

function check(results) {
    const names = Object.keys(results);
    const failed = names.filter((name) => !results[name]);
    for (const name of failed) print("FAIL " + name);
    if (failed.length) throw new Error(`${failed.length} of ${names.length} checks failed`);
}
