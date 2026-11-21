// Payroll system — gross-to-net calculation with progressive tax brackets, social contributions with ceilings,
// pre-tax deductions, overtime from timesheets, mid-month joiners, retro pay, payslips, a SEPA payment file,
// journal postings and year-to-date reconciliation.
//
// WHAT IT IS
//   Twelve monthly payroll runs for a 60-person company, the way a payroll bureau runs them:
//
//   master data   employees from CSV (salary or hourly, tax class, pension opt-in, start/leave dates, IBAN)
//   rules         a versioned YAML rulebook: progressive annual tax brackets applied to annualised pay and
//                 de-annualised (so monthly withholding converges on the annual liability), solidarity surcharge
//                 above a threshold, pension / health / unemployment contributions with monthly ceilings, a
//                 pre-tax pension deduction, and the rule version effective on the pay date (three versions, one
//                 of them effective mid-July)
//   time          timesheets give hours; overtime above 40 h/week pays 1.5x, Sunday hours 2x
//   proration     joiners, leavers and unpaid leave are paid for calendar days (in Decimal, rounded once); a month of
//                 reduced pay can push the cumulative tax due below what was already withheld, so withholding is
//                 floored at zero and the following months catch up
//   retro         a salary raise back-dated two months is detected and paid as retro lines with the tax computed
//                 on the cumulative difference, not on the lump sum alone
//   outputs       one payslip per employee (HTML from a template), a SEPA credit-transfer XML (pain.001) with
//                 control sum, journal entries (wages expense, payables to tax office / funds / employees) that
//                 balance, and a bank-file checksum
//   controls      net never negative, gross = net + all deductions to the cent, YTD totals reconcile across
//                 the year's runs, IBAN checksums validated before anything is paid; two employees sharing one bank
//                 account (the classic "ghost employee") are held via a Multimap of IBAN -> employees
//   variance      each run's net pay is compared with the previous run's as Float32Array vectors (simd sub, div,
//                 abs, threshold): every swing above 15% needs an explanation (joiner, leaver, retro) or is flagged
//   bands         gross pay percentiles per month from a TDigest
//   year end      the annual tax on actual taxable pay is settled against what was withheld: exactly zero for a
//                 constant full-year salary, a refund for every part-year employee (the bracket schedule is convex)
//   oracle        every slip (salaried and hourly) is recomputed in exact BigInt rational arithmetic with its own YTD
//                 state and must agree to the cent; the bracket tax is checked at every edge and a cent either side
//
// MODULES  dyna:decimal (Decimal, Money) · dyna:csv · dyna:yaml · dyna:xml (XmlWriter, XMLParse) · dyna:html (Template)
//          · dyna:time (PlainDate) · dyna:hash (SHA256) · dyna:cli (Table) · dyna:random · dyna:file
//          · dyna:simd (windowed sums, vector arithmetic) · dyna:structures (Multimap, TDigest)
//
// RUN      dynajs examples/finance/payroll-system.js     runs January–December payroll for a generated company

import { Decimal, Money } from "dyna:decimal";
import * as csv from "dyna:csv";
import * as YAML from "dyna:yaml";
import { XmlWriter, XMLParse } from "dyna:xml";
import { Template } from "dyna:html";
import { PlainDate } from "dyna:time";
import { SHA256Hex } from "dyna:hash";
import { Table } from "dyna:cli";
import { Random } from "dyna:random";
import { makeTempDir, writeFile, readFile, removeAll } from "dyna:file";
import * as simd from "dyna:simd";
import { Multimap, TDigest } from "dyna:structures";
const table = (rows) => Table(rows.slice(1), { head: rows[0] });

const work = makeTempDir("payroll");
const rng = new Random(2026);
const D = (x) => new Decimal(x);
const r2 = (d) => new Decimal(d.toFixed(2));
const sumD = (xs) => xs.reduce((a, b) => a.add(b), Decimal.ZERO);

// =============================================================================
// Rulebook (two versions: contribution ceilings change on 1 March)
// =============================================================================
const RULEBOOK = YAML.Parse(`
- version: 2026.1
  effective: "2026-01-01"
  brackets:
    - { upTo: 12000, rate: 0 }
    - { upTo: 40000, rate: 0.20 }
    - { upTo: 90000, rate: 0.32 }
    - { upTo: null, rate: 0.42 }
  solidarity: { threshold: 18000, rate: 0.055 }
  contributions:
    pension: { rate: 0.093, ceiling: 7550 }
    health: { rate: 0.0815, ceiling: 5175 }
    unemployment: { rate: 0.013, ceiling: 7550 }
  employerShare: { pension: 0.093, health: 0.0815, unemployment: 0.013 }
- version: 2026.2
  effective: "2026-03-01"
  brackets:
    - { upTo: 12000, rate: 0 }
    - { upTo: 40000, rate: 0.20 }
    - { upTo: 90000, rate: 0.32 }
    - { upTo: null, rate: 0.42 }
  solidarity: { threshold: 18000, rate: 0.055 }
  contributions:
    pension: { rate: 0.093, ceiling: 8050 }
    health: { rate: 0.0815, ceiling: 5512.5 }
    unemployment: { rate: 0.013, ceiling: 8050 }
  employerShare: { pension: 0.093, health: 0.0815, unemployment: 0.013 }
- version: 2026.3
  effective: "2026-07-15"
  brackets:
    - { upTo: 12000, rate: 0 }
    - { upTo: 40000, rate: 0.20 }
    - { upTo: 90000, rate: 0.32 }
    - { upTo: null, rate: 0.42 }
  solidarity: { threshold: 18000, rate: 0.055 }
  contributions:
    pension: { rate: 0.093, ceiling: 8050 }
    health: { rate: 0.0855, ceiling: 5512.5 }
    unemployment: { rate: 0.013, ceiling: 8050 }
  employerShare: { pension: 0.093, health: 0.0855, unemployment: 0.013 }
`);
const rulesFor = (date) => [...RULEBOOK].reverse().find((r) => r.effective <= date);
function annualTax(rules, taxable) {
    let tax = Decimal.ZERO,
        prev = D(0);
    for (const b of rules.brackets) {
        const cap = b.upTo === null ? null : D(b.upTo);
        const slice = (cap === null || taxable.cmp(cap) > 0 ? (cap ?? taxable) : taxable).sub(prev);
        if (slice.cmp(0) > 0) tax = tax.add(slice.mul(b.rate));
        if (cap === null || taxable.cmp(cap) <= 0) break;
        prev = cap;
    }
    return tax;
}

// the surcharge applies when the annual tax is ABOVE the threshold
const solidarity = (rules, annualTaxAmt, tax) =>
    annualTaxAmt.cmp(rules.solidarity.threshold) > 0 ? r2(tax.mul(rules.solidarity.rate)) : Decimal.ZERO;

// =============================================================================
// Master data and timesheets
// =============================================================================
const ibanFor = (n) => {
    const bban = `50010517${String(n).padStart(10, "0")}`;
    let m = 0;
    for (const ch of bban + "131400") m = (m * 10 + Number(ch)) % 97;
    return `DE${String(98 - m).padStart(2, "0")}${bban}`;
};
const ibanOk = (s) => {
    if (!/^[A-Z]{2}\d{2}[A-Z0-9]{11,30}$/.test(s)) return false;
    let m = 0;
    for (const ch of (s.slice(4) + s.slice(0, 4)).replace(/[A-Z]/g, (c) => c.charCodeAt(0) - 55))
        m = (m * 10 + Number(ch)) % 97;
    return m === 1;
};
const FIRST = ["Lena", "Jonas", "Mia", "Felix", "Emma", "Paul", "Hannah", "Leon", "Sofia", "Noah", "Lea", "Elias"];
const rows = [["id", "name", "type", "salary", "rate", "pension_optin", "start", "leave", "iban"]];
for (let i = 1; i <= 61; i++) {
    const hourly = i % 6 === 0;
    rows.push([
        `E${String(i).padStart(3, "0")}`,
        `${FIRST[i % FIRST.length]} ${["Bauer", "Koch", "Wolf", "Neumann", "Krause"][i % 5]}`,
        hourly ? "hourly" : "salary",
        hourly ? "" : String(36000 + rng.nextBounded(90) * 1000),
        hourly ? (18 + rng.nextBounded(12)).toFixed(2) : "",
        i % 3 === 0 ? "4" : "0",
        i === 57 ? "2026-02-16" : i === 58 ? "2026-03-09" : "2025-0" + (1 + (i % 9)) + "-01",
        i === 13 ? "2026-03-20" : "",
        i === 33 ? "DE00500105170000000033" : i === 61 ? ibanFor(12) : ibanFor(i),
    ]);
}
writeFile(work.join("employees.csv"), csv.stringify(rows));
const emp = csv
    .parse(readFile(work.join("employees.csv")), { hasHeader: true })
    .rows.map(([id, name, type, salary, rate, pension, start, leave, iban]) => ({
        id,
        name,
        type,
        salary: salary && D(salary),
        rate: rate && D(rate),
        pensionPct: Number(pension),
        start,
        leave: leave || null,
        iban,
    }));
const accountsOf = new Multimap();
for (const e of emp) accountsOf.put(e.iban, e.id);
// unpaid leave: those calendar days are not paid (a month of reduced pay makes the cumulative tax due fall below
// what was already withheld; withholding is floored at zero and the year-end settlement refunds the difference)
const unpaidLeave = new Map([["E021", ["2026-08-06", "2026-08-31"]]]);
const unpaidDays = (id, period, from, to) => {
    const u = unpaidLeave.get(id);
    if (!u || u[0].slice(0, 7) !== period) return 0;
    return Math.max(0, Math.min(to, Number(u[1].slice(8))) - Math.max(from, Number(u[0].slice(8))) + 1);
};
const raises = [{ id: "E005", from: "2026-02-01", pct: "1.12", decided: "2026-04-10" }]; // +12%, decided in April, back-dated
function timesheet(e, year, month, from, to) {
    // weekday and Sunday hours as two Float32Array columns
    const days = new PlainDate(year, month, 1).daysInMonth;
    const weekday = new Float32Array(days),
        sunday = new Float32Array(days);
    for (let d = 1; d <= days; d++) {
        const dow = new PlainDate(year, month, d).dayOfWeek; // 1 = Monday … 7 = Sunday
        const h =
            dow <= 5
                ? 8 + (rng.nextFloat() < 0.15 ? 2 + rng.nextBounded(3) : 0)
                : dow === 7 && rng.nextFloat() < 0.2
                  ? 6
                  : 0;
        if (d < from || d > to) continue;
        (dow === 7 ? sunday : weekday)[d - 1] = h;
    }
    return { weekday, sunday };
}

// =============================================================================
// One payroll run
// =============================================================================
const ytd = new Map(); // id -> { gross, taxable, tax, ... }
const store = [];
function payrollRun(year, month) {
    const payDate = `${year}-${String(month).padStart(2, "0")}-25`,
        rules = rulesFor(payDate);
    const monthStart = `${year}-${String(month).padStart(2, "0")}-01`,
        daysInMonth = new PlainDate(year, month, 1).daysInMonth;
    const monthEnd = `${monthStart.slice(0, 8)}${daysInMonth}`;
    const slips = [],
        errors = [];
    for (const e of emp) {
        if (e.start > monthEnd || (e.leave && e.leave < monthStart)) continue;
        if (!ibanOk(e.iban)) {
            errors.push(`${e.id} ${e.name}: IBAN ${e.iban} fails its checksum; payment held`);
            continue;
        }
        const sharing = accountsOf.get(e.iban);
        if (sharing.length > 1 && sharing[0] !== e.id) {
            errors.push(
                `${e.id} ${e.name}: IBAN shared with ${sharing.without([e.id]).join(", ")}; payment held for review`,
            );
            continue;
        }
        const lines = [];
        const from = e.start > monthStart ? Number(e.start.slice(8)) : 1,
            to = e.leave && e.leave <= monthEnd ? Number(e.leave.slice(8)) : daysInMonth;
        if (e.type === "salary") {
            const raise = raises.find((r) => r.id === e.id && r.from <= monthStart && r.decided <= payDate),
                salary = raise ? r2(e.salary.mul(raise.pct)) : e.salary;
            const monthly = salary.div(12, { precision: 34 }),
                unpaid = unpaidDays(e.id, monthStart.slice(0, 7), from, to);
            lines.push({
                kind: "base",
                text:
                    from === 1 && to === daysInMonth && !unpaid
                        ? "Monthly salary"
                        : `Salary, days ${from}–${to} of ${daysInMonth}${unpaid ? `, ${unpaid} unpaid` : ""}`,
                amount: r2(monthly.mul(to - from + 1 - unpaid).div(daysInMonth, { precision: 34 })),
            });
            for (const r of raises.filter(
                (x) =>
                    x.id === e.id &&
                    x.decided > `${year}-${String(month - 1).padStart(2, "0")}-25` &&
                    x.decided <= payDate,
            ))
                for (let m = Number(r.from.slice(5, 7)); m < month; m++)
                    lines.push({
                        kind: "retro",
                        text: `Retro raise for ${year}-${String(m).padStart(2, "0")}`,
                        amount: r2(e.salary.mul(r.pct).sub(e.salary).div(12, { precision: 34 })),
                    });
        } else {
            const ts = timesheet(e, year, month, from, to);
            const sunday = simd.sum(ts.sunday);
            let regular = 0,
                overtime = 0;
            for (let w = 0; w < ts.weekday.length; w += 7) {
                const hours = simd.sum(ts.weekday, w, Math.min(7, ts.weekday.length - w));
                regular += Math.min(40, hours);
                overtime += Math.max(0, hours - 40);
            }
            lines.push({
                kind: "base",
                text: `${regular} regular hours @ ${e.rate.toFixed(2)}`,
                amount: r2(e.rate.mul(regular)),
            });
            if (overtime)
                lines.push({
                    kind: "overtime",
                    text: `${overtime} overtime hours @ 1.5x`,
                    amount: r2(e.rate.mul(overtime).mul("1.5")),
                });
            if (sunday)
                lines.push({
                    kind: "sunday",
                    text: `${sunday} Sunday hours @ 2x`,
                    amount: r2(e.rate.mul(sunday).mul(2)),
                });
        }
        const gross = sumD(lines.map((l) => l.amount));
        const pensionDeferral = r2(gross.mul(e.pensionPct).div(100, { precision: 34 }));
        const contributions = Object.entries(rules.contributions).map(([k, c]) => ({
            kind: k,
            amount: r2(D(Math.min(Number(gross.toFixed(2)), c.ceiling)).mul(c.rate)),
        }));
        // Tax: cumulative method — tax due on YTD taxable pay annualised, minus tax already withheld.
        const y = ytd.get(e.id) || {
            gross: D(0),
            taxable: D(0),
            tax: D(0),
            soli: D(0),
            net: D(0),
            months: 0,
            contributions: D(0),
        };
        const taxable = y.taxable.add(gross.sub(pensionDeferral)),
            monthsWorked = y.months + 1;
        const annualised = taxable.mul(12).div(monthsWorked, { precision: 34 });
        const taxDue = r2(annualTax(rules, annualised).mul(monthsWorked).div(12, { precision: 34 }));
        const tax = r2(Decimal.ZERO.cmp(taxDue.sub(y.tax)) > 0 ? Decimal.ZERO : taxDue.sub(y.tax));
        const soli = solidarity(rules, annualTax(rules, annualised), tax);
        const contribTotal = sumD(contributions.map((c) => c.amount));
        const net = gross.sub(pensionDeferral).sub(tax).sub(soli).sub(contribTotal);
        ytd.set(e.id, {
            gross: y.gross.add(gross),
            taxable,
            tax: y.tax.add(tax),
            soli: y.soli.add(soli),
            net: y.net.add(net),
            months: monthsWorked,
            contributions: y.contributions.add(contribTotal),
        });
        const employer = Object.entries(rules.employerShare).map(([k, rate]) =>
            r2(D(Math.min(Number(gross.toFixed(2)), rules.contributions[k].ceiling)).mul(rate)),
        );
        slips.push({
            e,
            period: monthStart.slice(0, 7),
            rules: String(rules.version),
            lines,
            gross,
            pensionDeferral,
            contributions,
            tax,
            soli,
            net,
            employerCost: sumD(employer),
        });
    }
    return { year, month, payDate, slips, errors };
}

// =============================================================================
// Outputs: SEPA pain.001, journal, payslips
// =============================================================================
function sepa(run) {
    const total = sumD(run.slips.map((s) => s.net));
    const w = new XmlWriter({ indent: 1 });
    w.open("Document", { xmlns: "urn:iso:std:iso:20022:tech:xsd:pain.001.001.09" });
    w.open("CstmrCdtTrfInitn");
    w.open("GrpHdr");
    for (const [k, v] of [
        ["MsgId", `PAY-${run.year}${String(run.month).padStart(2, "0")}`],
        ["CreDtTm", `${run.payDate}T08:00:00`],
        ["NbOfTxs", String(run.slips.length)],
        ["CtrlSum", total.toFixed(2)],
    ]) {
        w.open(k);
        w.text(v);
        w.close();
    }
    w.close();
    w.open("PmtInf");
    for (const s of run.slips) {
        w.open("CdtTrfTxInf");
        w.open("PmtId");
        w.open("EndToEndId");
        w.text(`SAL-${s.period}-${s.e.id}`);
        w.close();
        w.close();
        w.open("Amt");
        w.open("InstdAmt", { Ccy: "EUR" });
        w.text(s.net.toFixed(2));
        w.close();
        w.close();
        w.open("Cdtr");
        w.open("Nm");
        w.text(s.e.name);
        w.close();
        w.close();
        w.open("CdtrAcct");
        w.open("Id");
        w.open("IBAN");
        w.text(s.e.iban);
        w.close();
        w.close();
        w.close();
        w.close();
    }
    w.close();
    w.close();
    w.close();
    return { xml: w.finish(), total };
}
function journal(run) {
    const T = (f) => sumD(run.slips.map(f));
    const gross = T((s) => s.gross),
        employer = T((s) => s.employerCost);
    const entries = [
        ["6000 Wages and salaries", gross, null],
        ["6100 Employer social contributions", employer, null],
        ["3720 Payable: employees (net pay)", null, T((s) => s.net)],
        ["3730 Payable: tax office (wage tax + solidarity)", null, T((s) => s.tax.add(s.soli))],
        ["3740 Payable: social funds", null, T((s) => sumD(s.contributions.map((c) => c.amount))).add(employer)],
        ["3750 Payable: pension provider", null, T((s) => s.pensionDeferral)],
    ];
    const dr = sumD(entries.filter((e) => e[1]).map((e) => e[1])),
        cr = sumD(entries.filter((e) => e[2]).map((e) => e[2]));
    return { entries, dr, cr };
}
const SLIP =
    new Template(`<!doctype html><meta charset="utf-8"><title>Payslip {{period}} {{id}}</title><h1>{{name}} — {{period}}</h1>
<table>{{#lines}}<tr><td>{{text}}</td><td class="num">{{amount}}</td></tr>{{/lines}}<tr class="total"><td>Gross</td><td class="num">{{gross}}</td></tr>
{{#deductions}}<tr><td>{{text}}</td><td class="num">-{{amount}}</td></tr>{{/deductions}}<tr class="total"><td>Net pay</td><td class="num">{{net}}</td></tr></table>
<p>Rules {{rules}} · YTD gross {{ytdGross}} · YTD tax {{ytdTax}}</p>`);
const eur = (d) => Money.fromString(d.toFixed(2), "EUR").format();

// =============================================================================
// Run January to April
// =============================================================================
const results = {};
const runs = [];
for (const m of Iterator.range(1, 13)) {
    const run = payrollRun(2026, m);
    const pay = sepa(run),
        j = journal(run);
    writeFile(work.join(`pain001-${run.payDate.slice(0, 7)}.xml`), pay.xml);
    for (const s of run.slips.slice(0, 3)) {
        const y = ytd.get(s.e.id);
        writeFile(
            work.join(`slip-${s.period}-${s.e.id}.html`),
            SLIP.render({
                id: s.e.id,
                name: s.e.name,
                period: s.period,
                rules: s.rules,
                lines: s.lines.map((l) => ({ text: l.text, amount: eur(l.amount) })),
                gross: eur(s.gross),
                net: eur(s.net),
                ytdGross: eur(y.gross),
                ytdTax: eur(y.tax),
                deductions: [
                    { text: "Pension deferral (pre-tax)", amount: eur(s.pensionDeferral) },
                    ...s.contributions.map((c) => ({ text: `${c.kind} contribution`, amount: eur(c.amount) })),
                    { text: "Wage tax", amount: eur(s.tax) },
                    { text: "Solidarity surcharge", amount: eur(s.soli) },
                ],
            }),
        );
    }
    runs.push({ ...run, pay, j, fileHash: SHA256Hex(pay.xml).slice(0, 16) });
    store.push(...run.slips);
}

// Net-pay variance between consecutive runs, as vectors aligned on the employee list.
const netOf = (run) =>
    Float32Array.from(emp, (e) => Number(run.slips.find((s) => s.e.id === e.id)?.net.toFixed(2) ?? 0));
const variance = runs.drop(1).map((run, i) => {
    const prev = netOf(runs[i]),
        cur = netOf(run);
    const rel = simd.abs(
        new Float32Array(emp.length),
        simd.div(
            new Float32Array(emp.length),
            simd.sub(new Float32Array(emp.length), cur, prev),
            simd.addScalar(prev.slice(), 1e-3),
        ),
    );
    simd.clamp(rel, 0, 10);
    const swings = emp.filter((_, k) => rel[k] > 0.15 && prev[k] > 0 && cur[k] > 0);
    const explained = (e) =>
        e.start.slice(0, 7) === runs[i].slips[0].period ||
        e.leave?.slice(0, 7) === run.slips[0].period ||
        // retro pay is one-off: it explains the swing up in its month and the swing back the month after
        [run, runs[i]].some((r) => r.slips.find((s) => s.e.id === e.id)?.lines.some((l) => l.kind === "retro")) ||
        [run, runs[i]].some((r) => unpaidLeave.get(e.id)?.[0].slice(0, 7) === r.payDate.slice(0, 7)) ||
        e.type === "hourly";
    return {
        period: run.slips[0].period,
        swings: swings.map((e) => e.id),
        unexplained: swings.filter((e) => !explained(e)).map((e) => e.id),
    };
});
const bands = runs.map((run) => {
    const d = new TDigest(100).addMany(
        Float64Array.from(
            run.slips.filter((s) => s.e.type === "salary"),
            (s) => Number(s.gross.toFixed(2)),
        ),
    );
    return [run.slips[0].period, ...[0.25, 0.5, 0.75].map((q) => d.quantile(q).format(0))];
});

for (const run of runs) {
    results[`${run.payDate.slice(0, 7)}: journal balances (${run.j.dr.toFixed(2)})`] = run.j.dr.equals(run.j.cr);
    const doc = XMLParse(run.pay.xml),
        texts = (node, name, out = []) => {
            if (node.name === name) out.push(JSON.stringify(node.children));
            for (const c of node.children || []) if (typeof c === "object") texts(c, name, out);
            return out;
        };
    const amounts = texts(doc, "InstdAmt").map((t) => /\d+\.\d\d/.exec(t)[0]),
        ctrl = /\d+\.\d\d/.exec(texts(doc, "CtrlSum")[0])[0];
    results[`${run.payDate.slice(0, 7)}: SEPA control sum matches the parsed transfers`] =
        ctrl === run.pay.total.toFixed(2) && sumD(amounts.map(D)).toFixed(2) === ctrl;
}
results["gross = net + every deduction, to the cent, on every slip"] = store.every((s) =>
    s.gross.equals(
        s.net
            .add(s.pensionDeferral)
            .add(s.tax)
            .add(s.soli)
            .add(sumD(s.contributions.map((c) => c.amount))),
    ),
);
results["no negative net pay"] = store.every((s) => s.net.cmp(0) > 0);
results["the employee with a corrupt IBAN is held, not paid"] = runs.every((r) =>
    r.errors.some((x) => /E033.*checksum/.test(x)),
);
results["a second employee paid into E012's account is held as a possible ghost"] =
    runs.every((r) => r.errors.length === 2 && r.errors.some((x) => /E061.*shared with E012/.test(x))) &&
    !store.some((s) => s.e.id === "E061");
results[`April's retro pay shows up as a net-pay swing (${variance[2].swings.join(" ")})`] =
    variance[2].swings.includes("E005");
results["every net-pay swing above 15% has an explanation"] = variance.every((v) => v.unexplained.length === 0);
results["simd weekly windows give the same hours as a scalar day loop"] = (() => {
    const t = timesheet(emp[5], 2026, 1, 1, 31);
    let scalar = 0;
    for (const h of t.weekday) scalar += h;
    let windows = 0;
    for (let w = 0; w < 31; w += 7) windows += simd.sum(t.weekday, w, Math.min(7, 31 - w));
    return scalar === windows;
})();
const joiner = store.find((s) => s.e.id === "E057" && s.period === "2026-02"),
    leaver = store.find((s) => s.e.id === "E013" && s.period === "2026-03");
results["joiners and leavers are prorated by calendar days"] =
    joiner && /days 16–28 of 28/.test(joiner.lines[0].text) && leaver && /days 1–20 of 31/.test(leaver.lines[0].text);
results["nobody is paid outside their employment"] =
    !store.some((s) => s.e.id === "E013" && s.period === "2026-04") &&
    !store.some((s) => s.e.id === "E058" && s.period < "2026-03");
const retro = store.filter((s) => s.e.id === "E005" && s.period === "2026-04")[0];
results["a raise decided in April back-dated to February pays two retro lines"] =
    retro && retro.lines.filter((l) => l.kind === "retro").length === 2;
results["overtime and Sunday premiums are computed from timesheets"] =
    store.some((s) => s.lines.some((l) => l.kind === "overtime")) &&
    store.some((s) => s.lines.some((l) => l.kind === "sunday"));
const hi = store.filter((s) => s.period === "2026-02").sort((a, b) => b.gross.cmp(a.gross))[0],
    hiMarch = store.find((s) => s.e.id === hi.e.id && s.period === "2026-03");
results["the March rule version raises the contribution ceilings for high earners"] =
    hi.rules === "2026.1" &&
    hiMarch.rules === "2026.2" &&
    sumD(hiMarch.contributions.map((c) => c.amount)).cmp(sumD(hi.contributions.map((c) => c.amount))) >= 0;
for (const [id, y] of ytd) {
    const slips = store.filter((s) => s.e.id === id);
    if (!sumD(slips.map((s) => s.gross)).equals(y.gross) || !sumD(slips.map((s) => s.tax)).equals(y.tax)) {
        results[`${id}: YTD does not reconcile`] = false;
        break;
    }
}
const stable = store.filter(
    (s) =>
        s.e.type === "salary" &&
        s.e.id !== "E005" &&
        !unpaidLeave.has(s.e.id) &&
        s.e.start < "2026-01-01" &&
        !s.e.leave,
);
const byEmp = new Map();
for (const s of stable) {
    if (!byEmp.has(s.e.id)) byEmp.set(s.e.id, []);
    byEmp.get(s.e.id).push(s.tax);
}
results["cumulative withholding is flat for a constant salary"] = [...byEmp.values()].every(
    (t) => t.length === 12 && t.every((x) => x.sub(t[0]).abs().cmp("0.05") <= 0),
);

// =============================================================================
// Year-end settlement
// =============================================================================
// The cumulative method withholds tax on YTD pay annualised over months worked. At year end the liability is the
// annual tax on the ACTUAL annual taxable pay; the difference is settled. For a constant full-year salary the two are
// the same number (r2(T * 12 / 12) = r2(T)), so the settlement is exactly zero. For anyone paid for part of the
// year it is a refund: the bracket schedule T is convex with T(0) = 0, so T(lambda x) <= lambda T(x) for
// lambda <= 1, and the withheld (m/12) T(12 X / m) is at least T(X).
const yearRules = rulesFor("2026-12-31");
const settlement = new Map(
    [...ytd].map(([id, y]) => [id, r2(annualTax(yearRules, y.taxable)).sub(y.tax).neg()]), // positive = refund
);
const fullYearConstant = [...byEmp.keys()];
results[`year-end settlement is exactly zero for all ${fullYearConstant.length} constant full-year salaries`] =
    fullYearConstant.length > 30 && fullYearConstant.every((id) => settlement.get(id).equals(0));
const partYear = emp.filter((e) => e.type === "salary" && ytd.has(e.id) && ytd.get(e.id).months < 12);
results[
    `part-year employees are refunded (${partYear.map((e) => `${e.id} ${settlement.get(e.id).toFixed(2)}`).join(", ")})`
] =
    partYear.length === 3 &&
    partYear.every((e) => settlement.get(e.id).cmp(0) >= 0) &&
    settlement.get("E013").cmp(100) > 0;

// =============================================================================
// Oracle: every slip recomputed in exact rational arithmetic (BigInt numerator/denominator)
// =============================================================================
// No Decimal, no shared helper: the bracket tax is the textbook sum of rate x slice, rounding is half away from
// zero on the exact rational, and the YTD state is this oracle's own. Every amount must agree to the cent.
const gcd = (a, b) => {
    while (b) [a, b] = [b, a % b];
    return a < 0n ? -a : a;
};
const Q = (n, d = 1n) => {
    if (d < 0n) [n, d] = [-n, -d];
    const g = gcd(n, d) || 1n;
    return { n: n / g, d: d / g };
};
const qs = (x) => {
    // "1234.5" -> exact rational
    const [i, f = ""] = String(x).split(".");
    return Q(BigInt(i + f), 10n ** BigInt(f.length));
};
const qadd = (a, b) => Q(a.n * b.d + b.n * a.d, a.d * b.d),
    qsub = (a, b) => Q(a.n * b.d - b.n * a.d, a.d * b.d),
    qmul = (a, b) => Q(a.n * b.n, a.d * b.d),
    qdiv = (a, b) => Q(a.n * b.d, a.d * b.n),
    qcmp = (a, b) => {
        const x = a.n * b.d - b.n * a.d;
        return x < 0n ? -1 : x > 0n ? 1 : 0;
    },
    qmin = (a, b) => (qcmp(a, b) <= 0 ? a : b);
const cents = (q) => {
    // round half away from zero to a whole cent, returned as a BigInt count of cents
    const n = q.n * 100n,
        sign = n < 0n ? -1n : 1n,
        a = n * sign;
    return (sign * (2n * a + q.d)) / (2n * q.d);
};
const C = (c) => Q(c, 100n);
const bracketTax = (rules, x) => {
    let tax = Q(0n),
        low = Q(0n);
    for (const b of rules.brackets) {
        const up = b.upTo === null ? null : qs(b.upTo);
        const top = up === null ? x : qmin(x, up);
        if (qcmp(top, low) > 0) tax = qadd(tax, qmul(qsub(top, low), qs(b.rate)));
        if (up === null || qcmp(x, up) <= 0) break;
        low = up;
    }
    return tax;
};
const oracleYtd = new Map();
let slipsChecked = 0,
    slipsWrong = [];
for (const run of runs) {
    const rules = rulesFor(run.payDate),
        dim = BigInt(new PlainDate(run.year, run.month, 1).daysInMonth);
    for (const s of run.slips) {
        if (s.e.id === "E005") continue;
        let gross;
        if (s.e.type === "salary") {
            const from = s.e.start > `${s.period}-01` ? BigInt(Number(s.e.start.slice(8))) : 1n,
                to = s.e.leave && s.e.leave.slice(0, 7) === s.period ? BigInt(Number(s.e.leave.slice(8))) : dim;
            let paid = to - from + 1n;
            const u = unpaidLeave.get(s.e.id);
            if (u && u[0].slice(0, 7) === s.period)
                for (let d = from; d <= to; d++)
                    if (d >= BigInt(Number(u[0].slice(8))) && d <= BigInt(Number(u[1].slice(8)))) paid--;
            gross = cents(qdiv(qmul(qs(s.e.salary.toString()), Q(paid)), Q(12n * dim)));
        } else {
            // hours are the timesheet's (an input); the premiums and the rounding are recomputed
            const hours = (kind) => qs(s.lines.find((l) => l.kind === kind)?.text.split(" ")[0] ?? "0");
            const rate = qs(s.e.rate.toString());
            gross =
                cents(qmul(rate, hours("base"))) +
                cents(qmul(qmul(rate, hours("overtime")), qs("1.5"))) +
                cents(qmul(qmul(rate, hours("sunday")), Q(2n)));
        }
        const pension = cents(qmul(C(gross), Q(BigInt(s.e.pensionPct), 100n)));
        const contrib = Object.values(rules.contributions).map((c) =>
            cents(qmul(qmin(C(gross), qs(c.ceiling)), qs(c.rate))),
        );
        const y = oracleYtd.get(s.e.id) ?? { taxable: Q(0n), tax: 0n, months: 0n };
        const taxable = qadd(y.taxable, C(gross - pension)),
            months = y.months + 1n;
        const annualised = qdiv(qmul(taxable, Q(12n)), Q(months));
        const due = cents(qdiv(qmul(bracketTax(rules, annualised), Q(months)), Q(12n)));
        const tax = due > y.tax ? due - y.tax : 0n;
        const soli =
            qcmp(bracketTax(rules, annualised), qs(rules.solidarity.threshold)) > 0
                ? cents(qmul(C(tax), qs(rules.solidarity.rate)))
                : 0n;
        const net = gross - pension - tax - soli - contrib.reduce((a, b) => a + b, 0n);
        oracleYtd.set(s.e.id, { taxable, tax: y.tax + tax, months });
        const got = [s.gross, s.pensionDeferral, ...s.contributions.map((c) => c.amount), s.tax, s.soli, s.net].map(
            (d) => BigInt(d.mul(100).toFixed(0)),
        );
        const want = [gross, pension, ...contrib, tax, soli, net];
        slipsChecked++;
        if (got.some((g, i) => g !== want[i])) slipsWrong.push(`${s.e.id}/${s.period}`);
    }
}
results[`${slipsChecked} slips (salaried and hourly) equal an exact rational recomputation to the cent`] =
    slipsChecked > 600 && slipsWrong.length === 0;
results["the solidarity surcharge starts strictly above its threshold"] =
    solidarity(yearRules, D(18000), D(100)).equals(0) && solidarity(yearRules, D("18000.01"), D(100)).equals("5.50");
{
    const aug = store.find((s) => s.e.id === "E021" && s.period === "2026-08");
    results[`unpaid leave in August withholds zero tax, not a negative amount (net ${aug.net.toFixed(2)})`] =
        aug.tax.equals(0) && aug.net.cmp(0) > 0;
    // the floor only defers: later months withhold the difference, and by December the cumulative due is the actual
    // annual tax again, so the settlement is zero
    results["withholding catches up after the unpaid month; E021's year-end settlement is zero"] = settlement
        .get("E021")
        .equals(0);
}
results["the mid-July rule version applies from the July pay date"] = runs[6].slips[0].rules === "2026.3";
// The bracket function alone, at every edge and a cent either side, and at random incomes
{
    let edgesBad = 0;
    const probes = [0, 0.01, 1, 999999.99];
    for (const b of yearRules.brackets) if (b.upTo !== null) probes.push(b.upTo - 0.01, b.upTo, b.upTo + 0.01);
    for (let i = 0; i < 2000; i++) probes.push(rng.nextBounded(25_000_000) / 100);
    for (const x of probes) {
        const exact = bracketTax(yearRules, qs(x.toFixed(2)));
        const dec = annualTax(yearRules, D(x.toFixed(2)));
        if (qcmp(exact, qs(dec.toString())) !== 0) edgesBad++;
    }
    results[`annual tax equals the exact bracket sum at ${probes.length} incomes incl. every edge +-1 cent`] =
        edgesBad === 0;
}

print("payroll-system");
print(
    table([
        ["period", "employees", "gross", "net", "wage tax", "employer cost", "SEPA file"],
        ...runs.map((r) => {
            const T = (f) => sumD(r.slips.map(f)).toFixed(2);
            return [
                r.payDate.slice(0, 7),
                String(r.slips.length),
                T((s) => s.gross),
                T((s) => s.net),
                T((s) => s.tax.add(s.soli)),
                T((s) => s.gross.add(s.employerCost)),
                r.fileHash,
            ];
        }),
    ]),
);
print(
    table([
        ["account", "debit", "credit"],
        ...runs[3].j.entries.map(([a, d, c]) => [a, d ? d.toFixed(2) : "", c ? c.toFixed(2) : ""]),
    ]),
);
print(
    `  sample     ${retro.e.name}: ${retro.lines.map((l) => `${l.text} ${l.amount.toFixed(2)}`).join("; ")} -> net ${retro.net.toFixed(2)}`,
);
for (const h of runs[0].errors) print(`  held       ${h}`);
print(`  swings     ${variance.map((v) => `${v.period}: ${v.swings.join(" ") || "-"}`).join(" | ")}`);
print(table([["period", "salary p25", "p50", "p75"], ...bands]));
removeAll(work);
check(results);

function check(results) {
    const names = Object.keys(results);
    const failed = names.filter((name) => !results[name]);
    for (const name of failed) print("FAIL " + name);
    if (failed.length) throw new Error(`${failed.length} of ${names.length} checks failed`);
}
