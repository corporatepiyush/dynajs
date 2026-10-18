// Bank reconciliation — import CAMT.053 XML, MT940 and CSV statements, match bank lines to open invoices and
// payables with a rule cascade (reference, amount+date, fuzzy name, many-to-one subset sums, fee tolerance),
// post the matches, and prove the book balance against the bank balance.
//
// WHAT IT IS
//   The month-end job of a finance team with three bank accounts:
//
//   import      account 1 sends ISO 20022 camt.053 XML, account 2 a SWIFT MT940 text file (:61:/:86: records),
//               account 3 a CSV export; all three are parsed into one normalised line shape, duplicate imports of
//               the same statement are detected by a content hash, and each statement's opening + movements =
//               closing balance is verified before any line is used
//   open items  receivables (customer invoices) and payables (supplier bills) in SQLite
//   matching    a cascade, each rule only seeing what earlier rules left:
//                 1. structured reference (RF creditor reference with ISO 11649 check digits, or invoice number)
//                 2. exact amount + counterparty name similarity (Jaro–Winkler) within a 10-day window
//                 3. one payment settling several invoices of the same customer (subset sum)
//                 4. amount within a bank-fee tolerance, the difference booked to fees
//                 5. foreign-currency receipts converted at the value-date rate within 1%
//               every match records the rule and a confidence; anything else goes to an exception queue
//   indexes     every invoice number and RF reference is one pattern of a MultiMatcher, so a remittance text is
//               scanned once for all of them (not once per open item); open items are indexed by amount in cents in a
//               SortedMap, so "exact amount" is a point lookup and "within the fee tolerance" is a range query
//   triage      exception lines are tagged (fee, refund, interest, ...) by a keyword MultiMatcher
//   posting     matched lines become journal entries (bank / receivable / fees / FX difference) in Decimal
//   report      book balance vs statement balance per account with the reconciling items listed, plus an
//               ageing of what remains open
//
// MODULES  dyna:xml (XMLParse) · dyna:csv · dyna:decimal · dyna:net (SQLite) · dyna:matcher (JaroWinkler,
//          MultiMatcher) · dyna:structures (SortedMap, RangeMap, Multiset) · dyna:hash (SHA256) · dyna:time (PlainDate)
//          · dyna:cli (Table) · dyna:random · dyna:file · Array/String extensions
//
// RUN      dynajs examples/finance/bank-reconciliation.js     generates a month of invoices and three bank statements

import { XMLParse } from "dyna:xml";
import * as csv from "dyna:csv";
import { Decimal } from "dyna:decimal";
import { SQLite } from "dyna:net";
import { JaroWinkler, MultiMatcher } from "dyna:matcher";
import { SortedMap, RangeMap, Multiset } from "dyna:structures";
import { SHA256Hex } from "dyna:hash";
import { PlainDate } from "dyna:time";
import { Table } from "dyna:cli";
import { Random } from "dyna:random";
import { makeTempDir, writeFile, readFile, removeAll } from "dyna:file";
const table = (rows) => Table(rows.slice(1), { head: rows[0] });

const work = makeTempDir("recon");
const rng = new Random(53);
const D = (x) => new Decimal(String(x));
const sumD = (xs) => xs.reduce((a, b) => a.add(b), Decimal.ZERO);
const epoch = (s) => {
    const [y, m, d] = s.split("-").map(Number);
    return new PlainDate(y, m, d).epochDay;
};

// =============================================================================
// RF creditor references (ISO 11649)
// =============================================================================
const mod97 = (s) => {
    let m = 0;
    for (const ch of s.replace(/[A-Z]/g, (c) => c.charCodeAt(0) - 55)) m = (m * 10 + Number(ch)) % 97;
    return m;
};
const rfMake = (base) => `RF${String(98 - mod97(`${base}RF00`)).padStart(2, "0")}${base}`;
const rfValid = (rf) => /^RF\d{2}[A-Z0-9]{1,21}$/.test(rf) && mod97(rf.slice(4) + rf.slice(0, 4)) === 1;

// =============================================================================
// Books: open receivables and payables
// =============================================================================
const db = new SQLite(work.join("books.db"));
db.exec(
    "CREATE TABLE items (id TEXT PRIMARY KEY, kind TEXT, party TEXT, amount TEXT, currency TEXT, issued TEXT, due TEXT, rf TEXT, open TEXT)",
);
db.exec("CREATE TABLE journal (id INTEGER PRIMARY KEY, date TEXT, account TEXT, debit TEXT, credit TEXT, ref TEXT)");
db.exec("CREATE TABLE statements (hash TEXT PRIMARY KEY, account TEXT, opening TEXT, closing TEXT)");
const CUSTOMERS = [
    "Nordwind Logistik GmbH",
    "Atelier Dubois SARL",
    "Hoffmann & Söhne KG",
    "Brightline Media Ltd",
    "Costa Verde S.L.",
    "Kowalski Trading Sp. z o.o.",
    "Van Dijk Retail B.V.",
    "Berger Elektro AG",
];
const SUPPLIERS = ["CloudHost Europe", "Office Supplies Direct", "Stadtwerke Energie", "Legal Partners LLP"];
const invoices = [];
for (let i = 1; i <= 80; i++) {
    const party = CUSTOMERS[i % CUSTOMERS.length],
        amount = D(200 + rng.nextBounded(9800) + rng.nextBounded(100) / 100).toFixed(2);
    const day = 1 + rng.nextBounded(28),
        issued = `2026-04-${String(day).padStart(2, "0")}`,
        due = `2026-05-${String(Math.min(28, day)).padStart(2, "0")}`;
    const inv = {
        id: `INV-2026-${String(1000 + i)}`,
        kind: "AR",
        party,
        amount,
        currency: i % 17 === 0 ? "USD" : "EUR",
        issued,
        due,
        rf: rfMake(String(1000 + i).padStart(8, "0")),
    };
    invoices.push(inv);
    db.exec("INSERT INTO items VALUES (?,?,?,?,?,?,?,?,?)", [
        inv.id,
        "AR",
        party,
        amount,
        inv.currency,
        issued,
        due,
        inv.rf,
        amount,
    ]);
}
for (let i = 1; i <= 12; i++)
    db.exec("INSERT INTO items VALUES (?,?,?,?,?,?,?,?,?)", [
        `BILL-${i}`,
        "AP",
        SUPPLIERS[i % 4],
        D(100 + rng.nextBounded(3000)).toFixed(2),
        "EUR",
        "2026-04-02",
        "2026-05-02",
        null,
        D(0).toFixed(2),
    ]);
db.exec("UPDATE items SET open = amount WHERE kind = 'AP'");
const USDRATE = { "2026-05-06": "0.9234", "2026-05-12": "0.9198", "2026-05-20": "0.9251" };

// =============================================================================
// Statement generation (what the banks send)
// =============================================================================
const payments = [];
const pick = () => invoices.splice(rng.nextBounded(invoices.length), 1)[0];
for (let k = 0; k < 60 && invoices.length; k++) {
    const day = `2026-05-${String(2 + rng.nextBounded(26)).padStart(2, "0")}`,
        style = k % 6;
    if (style === 5 && invoices.length > 3) {
        // one payment for two invoices of one customer
        const a = pick(),
            b = invoices.find((x) => x.party === a.party && x.currency === "EUR" && a.currency === "EUR");
        if (b) {
            invoices.splice(invoices.indexOf(b), 1);
            payments.push({
                day,
                amount: D(a.amount).add(b.amount).toFixed(2),
                name: a.party.toUpperCase(),
                text: "Sammelzahlung April",
                truth: [a.id, b.id],
            });
            continue;
        }
        invoices.push(a);
    }
    const inv = pick();
    if (inv.currency === "USD") {
        const d = Object.keys(USDRATE)[k % 3];
        payments.push({
            day: d,
            amount: D(inv.amount).mul(USDRATE[d]).toFixed(2),
            name: inv.party,
            text: `Invoice ${inv.id} USD ${inv.amount}`,
            truth: [inv.id],
            fx: true,
        });
        continue;
    }
    const text =
        style === 0
            ? `${inv.rf}`
            : style === 1
              ? `Payment ${inv.id}`
              : style === 2
                ? `Thanks for the April delivery`
                : style === 3
                  ? `Rechnung ${inv.id.slice(-4)}`
                  : `${inv.rf}`;
    const fee = style === 4 ? D("7.50") : D(0);
    payments.push({
        day,
        amount: D(inv.amount).sub(fee).toFixed(2),
        name: style === 2 ? inv.party.replace("GmbH", "Gmbh").replace("&", "und") : inv.party,
        text,
        truth: [inv.id],
    });
}
payments.push({ day: "2026-05-15", amount: "1250.00", name: "UNKNOWN SENDER", text: "Refund?", truth: [] });
const fees = [{ day: "2026-05-31", amount: "-38.40", name: "BANK", text: "Account maintenance fee" }];
const bills = db.query("SELECT * FROM items WHERE kind = 'AP' LIMIT 6").map((b, i) => ({
    day: `2026-05-0${2 + i}`,
    amount: `-${b.amount}`,
    name: b.party,
    text: `Bill ${b.id}`,
    truth: [b.id],
}));
const accounts = { A1: [], A2: [], A3: [] };
[...payments, ...bills, ...fees].forEach((p, i) =>
    accounts[["A1", "A2", "A3"][i % 3]].push({ ...p, bankRef: `BR${10000 + i}` }),
);
const opening = { A1: "120000.00", A2: "54000.00", A3: "8200.00" };
const closing = (a) => sumD([D(opening[a]), ...accounts[a].map((l) => D(l.amount))]).toFixed(2);

// camt.053 for A1
const esc = (s) => s.replace(/&/g, "&amp;").replace(/</g, "&lt;");
writeFile(
    work.join("A1.camt053.xml"),
    `<?xml version="1.0"?><Document xmlns="urn:iso:std:iso:20022:tech:xsd:camt.053.001.08"><BkToCstmrStmt><Stmt><Id>A1-2026-05</Id>
<Acct><Id><IBAN>DE89370400440532013000</IBAN></Id></Acct>
<Bal><Tp><CdOrPrtry><Cd>OPBD</Cd></CdOrPrtry></Tp><Amt Ccy="EUR">${opening.A1}</Amt><CdtDbtInd>CRDT</CdtDbtInd></Bal>
<Bal><Tp><CdOrPrtry><Cd>CLBD</Cd></CdOrPrtry></Tp><Amt Ccy="EUR">${D(closing("A1")).abs().toFixed(2)}</Amt><CdtDbtInd>${D(closing("A1")).cmp(0) < 0 ? "DBIT" : "CRDT"}</CdtDbtInd></Bal>
${accounts.A1.map(
    (
        l,
    ) => `<Ntry><NtryRef>${l.bankRef}</NtryRef><Amt Ccy="EUR">${D(l.amount).abs().toFixed(2)}</Amt><CdtDbtInd>${D(l.amount).cmp(0) < 0 ? "DBIT" : "CRDT"}</CdtDbtInd><ValDt><Dt>${l.day}</Dt></ValDt>
<NtryDtls><TxDtls><RltdPties><${D(l.amount).cmp(0) < 0 ? "Cdtr" : "Dbtr"}><Nm>${esc(l.name)}</Nm></${D(l.amount).cmp(0) < 0 ? "Cdtr" : "Dbtr"}></RltdPties><RmtInf><Ustrd>${esc(l.text)}</Ustrd></RmtInf></TxDtls></NtryDtls></Ntry>`,
).join("\n")}
</Stmt></BkToCstmrStmt></Document>`,
);
// MT940 for A2
const mtAmt = (d) => D(d).abs().toFixed(2).replace(".", ",");
writeFile(
    work.join("A2.mt940"),
    [
        ":20:STMT2605",
        ":25:DE44500105175407324931",
        ":28C:5/1",
        `:60F:C260501EUR${mtAmt(opening.A2)}`,
        ...accounts.A2.flatMap((l) => [
            `:61:${l.day.slice(2).replace(/-/g, "")}${l.day.slice(5).replace("-", "")}${D(l.amount).cmp(0) < 0 ? "D" : "C"}${mtAmt(l.amount)}NTRF${l.bankRef}`,
            `:86:${l.name}?${l.text}`,
        ]),
        `:62F:${D(closing("A2")).cmp(0) < 0 ? "D" : "C"}260531EUR${mtAmt(closing("A2"))}`,
        "-",
    ].join("\r\n"),
);
// CSV for A3 (European number format, semicolons)
writeFile(
    work.join("A3.csv"),
    csv.stringify(
        [
            ["Buchungstag", "Betrag", "Name", "Verwendungszweck", "Referenz"],
            ...accounts.A3.map((l) => [
                l.day.split("-").reverse().join("."),
                l.amount.replace(".", ","),
                l.name,
                l.text,
                l.bankRef,
            ]),
            ["Endsaldo", closing("A3").replace(".", ","), "", "", ""],
        ],
        { delimiter: ";" },
    ),
);

// =============================================================================
// Parsers into one line shape
// =============================================================================
function parseCamt(text) {
    const doc = XMLParse(text, { trim: true });
    const find = (n, name) => (n.children || []).filter((c) => typeof c === "object" && c.name === name);
    const deep = (n, name) => {
        if (n.name === name) return n;
        for (const c of n.children || [])
            if (typeof c === "object") {
                const r = deep(c, name);
                if (r) return r;
            }
        return null;
    };
    const txt = (n) =>
        n ? (n.children || []).map((c) => (typeof c === "string" ? c : (c.text ?? txt(c)))).join("") : "";
    const stmt = deep(doc, "Stmt");
    const bal = (code) => {
        const b = find(stmt, "Bal").find((x) => txt(deep(x, "Cd")) === code);
        const v = D(txt(deep(b, "Amt")));
        return txt(deep(b, "CdtDbtInd")) === "DBIT" ? v.neg() : v;
    };
    const lines = find(stmt, "Ntry").map((e) => {
        const v = D(txt(deep(e, "Amt")));
        return {
            date: txt(deep(e, "Dt")),
            amount: txt(deep(e, "CdtDbtInd")) === "DBIT" ? v.neg() : v,
            name: txt(deep(e, "Nm")),
            text: txt(deep(e, "Ustrd")),
            ref: txt(deep(e, "NtryRef")),
        };
    });
    return { opening: bal("OPBD"), closing: bal("CLBD"), lines };
}
function parseMt940(text) {
    const recs = text
        .split(/\r?\n(?=:)/)
        .map((r) => /^:(\w+):([\s\S]*)$/.exec(r))
        .filter(Boolean);
    const amt = (sign, s) => {
        const v = D(s.replace(",", "."));
        return sign === "D" ? v.neg() : v;
    };
    let opening, close;
    const lines = [];
    for (const [, tag, body] of recs) {
        if (tag === "60F") {
            const m = /^([CD])\d{6}[A-Z]{3}([\d,]+)/.exec(body);
            opening = amt(m[1], m[2]);
        }
        if (tag === "62F") {
            const m = /^([CD])\d{6}[A-Z]{3}([\d,]+)/.exec(body);
            close = amt(m[1], m[2]);
        }
        if (tag === "61") {
            const m = /^(\d{2})(\d{2})(\d{2})\d{4}([CD])([\d,]+)N\w{3}(.*)$/.exec(body.trim());
            lines.push({ date: `20${m[1]}-${m[2]}-${m[3]}`, amount: amt(m[4], m[5]), ref: m[6] });
        }
        if (tag === "86") {
            const [name, ...rest] = body.trim().split("?");
            Object.assign(lines[lines.length - 1], { name, text: rest.join("?") });
        }
    }
    return { opening, closing: close, lines };
}
function parseCsv(text, openingBalance) {
    const { rows } = csv.parse(text, { hasHeader: true, delimiter: ";" });
    const end = rows.find((r) => r[0] === "Endsaldo");
    const lines = rows
        .filter((r) => r !== end)
        .map(([d, a, name, purpose, ref]) => ({
            date: d.split(".").reverse().join("-"),
            amount: D(a.replace(",", ".")),
            name,
            text: purpose,
            ref,
        }));
    return { opening: D(openingBalance), closing: D(end[1].replace(",", ".")), lines };
}
const statements = [];
let duplicateImports = 0;
for (const [acct, file, parse] of [
    ["A1", "A1.camt053.xml", parseCamt],
    ["A2", "A2.mt940", parseMt940],
    ["A3", "A3.csv", (t) => parseCsv(t, opening.A3)],
    ["A2", "A2.mt940", parseMt940],
]) {
    const text = readFile(work.join(file)),
        hash = SHA256Hex(text);
    if (db.query("SELECT 1 FROM statements WHERE hash = ?", [hash]).length) {
        duplicateImports++;
        continue;
    }
    const st = parse(text);
    const computed = sumD([st.opening, ...st.lines.map((l) => l.amount)]);
    if (!computed.equals(st.closing))
        throw new Error(`${file}: opening + movements ${computed.toFixed(2)} != closing ${st.closing.toFixed(2)}`);
    db.exec("INSERT INTO statements VALUES (?, ?, ?, ?)", [hash, acct, st.opening.toFixed(2), st.closing.toFixed(2)]);
    statements.push({ acct, ...st, lines: st.lines.map((l) => ({ ...l, acct })) });
}

// =============================================================================
// Matching cascade
// =============================================================================
const openItems = () => db.query("SELECT * FROM items WHERE CAST(open AS REAL) > 0");
const norm = (s) =>
    s
        .toLowerCase()
        .normalize("NFD")
        .replace(/\p{M}/gu, "")
        .replace(/\b(gmbh|ag|kg|ltd|llp|sarl|s\.l\.|b\.v\.|sp\. z o\.o\.)\b/g, "")
        .replace(/[^a-z0-9 ]/g, " ")
        .compact();
const cents = (d) => Math.round(Number(D(d).toFixed(2)) * 100);

// In-memory view of the open items: by id, by amount (cents -> ids), and one reference matcher over all of them.
const item = new Map(openItems().map((i) => [i.id, { ...i, normParty: norm(i.party) }]));
const byAmount = new SortedMap();
const index = (it) =>
    (byAmount.get(cents(it.open)) || byAmount.set(cents(it.open), new Set()).get(cents(it.open))).add(it.id);
for (const it of item.values()) index(it);
const refPatterns = [...item.values()].flatMap((i) =>
    i.rf
        ? [
              [i.rf, i.id],
              [i.id, i.id],
          ]
        : [[i.id, i.id]],
);
const refs = new MultiMatcher(refPatterns.pluck(0));

const matches = [],
    exceptions = [];
function settle(line, items, rule, confidence, extra = {}) {
    const total = sumD(items.map((i) => D(i.open)));
    for (const it of items) {
        db.exec("UPDATE items SET open = '0.00' WHERE id = ?", [it.id]);
        byAmount.get(cents(it.open))?.delete(it.id);
        item.delete(it.id);
    }
    matches.push({ line, items: items.map((i) => i.id), rule, confidence, total, ...extra });
}
function subsetSum(target, cands) {
    const n = Math.min(cands.length, 12);
    for (let mask = 3; mask < 1 << n; mask++) {
        if ((mask & (mask - 1)) === 0) continue;
        const pickd = cands.filter((_, i) => mask & (1 << i));
        if (pickd.length <= 4 && sumD(pickd.map((c) => D(c.open))).equals(target)) return pickd;
    }
    return null;
}
const referenced = (text) => {
    for (const { index, at } of refs.allIn(text)) {
        const [pattern, id] = refPatterns[index];
        if (pattern.startsWith("RF") && !rfValid(pattern)) continue;
        if (item.has(id)) return { it: item.get(id), rf: pattern.startsWith("RF"), at };
    }
    return null;
};
const allLines = statements.flatMap((s) => s.lines).sortBy((l) => l.date);
for (const line of allLines) {
    const credit = line.amount.cmp(0) > 0,
        abs = line.amount.abs(),
        kind = credit ? "AR" : "AP";
    const lineName = norm(line.name || "");
    const sim = (i) => JaroWinkler(i.normParty, lineName);
    const window = (i) => Math.abs(epoch(line.date) - epoch(i.due)) <= 30;
    // 1. structured reference: one Aho-Corasick pass over the remittance text
    const ref = referenced(line.text || "");
    const byRef = ref && ref.it.kind === kind ? ref.it : null;
    if (byRef && D(byRef.open).equals(abs)) {
        settle(line, [byRef], ref.rf ? "rf-reference" : "invoice-number", 1);
        continue;
    }
    if (byRef && byRef.currency === "USD") {
        const expected = D(byRef.amount).mul(USDRATE[line.date] || "0.92");
        const carrying = D(D(byRef.amount).mul("0.9200").toFixed(2)); // booked at the invoice-date rate
        if (expected.sub(abs).abs().cmp(expected.mul("0.01")) <= 0) {
            settle(line, [byRef], "fx-reference", 0.9, { carrying, fx: abs.sub(carrying) });
            continue;
        }
    }
    if (
        byRef &&
        byRef.currency === "EUR" &&
        D(byRef.open).sub(abs).cmp(0) > 0 &&
        D(byRef.open).sub(abs).cmp("15.00") <= 0
    ) {
        settle(line, [byRef], "reference-with-fee", 0.95, { fee: D(byRef.open).sub(abs) });
        continue;
    }
    // 2. exact amount (a point lookup in the amount index) + name similarity in a date window
    const exact = [...(byAmount.get(cents(abs)) || [])]
        .map((id) => item.get(id))
        .filter((i) => i.kind === kind && window(i))
        .map((i) => [i, sim(i)])
        .sortBy(([, s]) => -s);
    if (exact.length && exact[0][1] > 0.85 && (exact.length === 1 || exact[0][1] - exact[1][1] > 0.05)) {
        settle(line, [exact[0][0]], "amount+name", exact[0][1].round(2));
        continue;
    }
    // 3. one payment for several invoices of the same party
    const party = [...item.values()].filter((i) => i.kind === kind && sim(i) > 0.85);
    const combo = subsetSum(abs, party);
    if (combo) {
        settle(line, combo, "subset-sum", 0.85);
        continue;
    }
    // 4. amount within fee tolerance from the same party: a range query over the amount index
    const near = byAmount
        .rangeQuery(cents(abs) + 1, cents(abs) + 1500)
        .flatMap(([, ids]) => [...ids])
        .map((id) => item.get(id))
        .filter((i) => i.kind === kind && sim(i) > 0.85);
    if (near.length === 1) {
        settle(line, near, "fee-tolerance", 0.8, { fee: D(near[0].open).sub(abs) });
        continue;
    }
    exceptions.push(line);
}

// Exception triage by keyword.
const TRIAGE = [
    ["fee", "bank-charge"],
    ["maintenance", "bank-charge"],
    ["refund", "refund"],
    ["interest", "interest"],
    ["chargeback", "dispute"],
];
const triage = new MultiMatcher(TRIAGE.pluck(0));
for (const e of exceptions)
    e.category = TRIAGE[triage.firstIn(`${e.text} ${e.name}`.toLowerCase())?.index]?.[1] || "unidentified";

// =============================================================================
// Posting and the reconciliation report
// =============================================================================
const post = (date, account, debit, credit, ref) =>
    db.exec("INSERT INTO journal (date, account, debit, credit, ref) VALUES (?,?,?,?,?)", [
        date,
        account,
        debit ? debit.toFixed(2) : null,
        credit ? credit.toFixed(2) : null,
        ref,
    ]);
for (const m of matches) {
    const bank = `1200-${m.line.acct}`,
        ar = m.line.amount.cmp(0) > 0;
    if (ar) {
        post(m.line.date, bank, m.line.amount, null, m.line.ref);
        if (m.fee) post(m.line.date, "6855 Bank fees", m.fee, null, m.line.ref);
        if (m.fx && !m.fx.equals(0))
            post(
                m.line.date,
                "2660 FX differences",
                m.fx.cmp(0) < 0 ? m.fx.neg() : null,
                m.fx.cmp(0) > 0 ? m.fx : null,
                m.line.ref,
            );
        post(m.line.date, "1400 Receivables", null, m.carrying || m.total, m.line.ref);
    } else {
        post(m.line.date, "1600 Payables", m.line.amount.neg(), null, m.line.ref);
        post(m.line.date, bank, null, m.line.amount.neg(), m.line.ref);
    }
}
const bal = (acct) =>
    db
        .query("SELECT debit, credit FROM journal WHERE account = ?", [acct])
        .reduce((s, r) => s.add(D(r.debit || 0)).sub(D(r.credit || 0)), Decimal.ZERO);
const report = statements.map((s) => {
    const book = D(opening[s.acct]).add(bal(`1200-${s.acct}`)),
        unposted = exceptions.filter((e) => e.acct === s.acct);
    return {
        acct: s.acct,
        statement: s.closing,
        book,
        items: unposted,
        diff: s.closing.sub(book).sub(sumD(unposted.map((e) => e.amount))),
    };
});
const AGE = new RangeMap().put(-100000, 0, "not due").put(1, 30, "1-30").put(31, 100000, "31+");
const ageing = openItems()
    .filter((i) => i.kind === "AR")
    .reduce((m, i) => {
        const b = AGE.get(epoch("2026-05-31") - epoch(i.due));
        m[b] = (m[b] || Decimal.ZERO).add(D(i.open));
        return m;
    }, {});

// =============================================================================
// Checks and output
// =============================================================================
const results = {};
const truthOf = new Map([...payments, ...bills].map((p) => [[...p.truth].sort().join(","), p]));
const wrong = matches.filter((m) => !truthOf.has([...m.items].sort().join(",")));
results["RF creditor references carry valid ISO 11649 check digits"] =
    rfValid(rfMake("00001001")) && !rfValid("RF00" + "00001001");
results["three statements imported, a duplicate import refused by content hash"] =
    statements.length === 3 && duplicateImports === 1;
results[
    `every automatic match is correct (${wrong.length} wrong: ${wrong
        .slice(0, 2)
        .map((m) => m.items.join("+"))
        .join(", ")})`
] = wrong.length === 0;
results[`${matches.length} of ${payments.length + bills.length} payment lines matched automatically`] =
    matches.length >= (payments.length + bills.length - 1) * 0.9;
results["combined payments, fee deductions and FX receipts are matched"] =
    matches.some((m) => m.rule === "subset-sum" && m.items.length === 2) &&
    matches.some((m) => m.rule === "reference-with-fee" || m.rule === "fee-tolerance") &&
    matches.some((m) => m.rule === "fx-reference");
results["unexplained lines go to the exception queue"] =
    exceptions.some((e) => e.name === "UNKNOWN SENDER") && exceptions.some((e) => /maintenance fee/.test(e.text));
results[`book + reconciling items = statement for every account (${report.map((r) => r.diff.toFixed(2)).join(", ")})`] =
    report.every((r) => r.diff.equals(0));
const jd = db
    .query("SELECT debit, credit FROM journal")
    .reduce((s, r) => [s[0].add(D(r.debit || 0)), s[1].add(D(r.credit || 0))], [Decimal.ZERO, Decimal.ZERO]);
results[`postings balance (${jd[0].toFixed(2)})`] = jd[0].equals(jd[1]);
results["the amount index holds exactly the items still open in the books"] =
    byAmount
        .keys()
        .flatMap((k) => [...byAmount.get(k)])
        .sortBy((x) => x)
        .join() ===
    openItems()
        .pluck("id")
        .sortBy((x) => x)
        .join();
results['triage tags the maintenance fee as a bank charge and the "Refund?" line as a refund'] =
    exceptions.find((e) => /maintenance/.test(e.text))?.category === "bank-charge" &&
    exceptions.find((e) => e.name === "UNKNOWN SENDER")?.category === "refund";
const ageTotal = Object.values(ageing).reduce((a, b) => a.add(b), Decimal.ZERO);
results["ageing buckets sum to the open receivables"] = ageTotal.equals(
    sumD(
        openItems()
            .filter((i) => i.kind === "AR")
            .map((i) => D(i.open)),
    ),
);

print("bank-reconciliation");
print(
    `  imported   ${allLines.length} lines from camt.053, MT940 and CSV; ${duplicateImports} duplicate import refused`,
);
const byRule = new Multiset();
for (const m of matches) byRule.add(m.rule);
print(
    `  matched    ${matches.length}: ${byRule
        .entrySet()
        .map(([k, v]) => `${k} ${v}`)
        .join(", ")}; exceptions ${exceptions.length} (${exceptions.map((e) => e.category).join(", ")})`,
);
print(
    table([
        ["account", "statement", "book", "reconciling items", "difference"],
        ...report.map((r) => [
            r.acct,
            r.statement.toFixed(2),
            r.book.toFixed(2),
            r.items.map((e) => `${e.amount.toFixed(2)} ${e.name}`).join("; ") || "-",
            r.diff.toFixed(2),
        ]),
    ]),
);
print(
    `  open AR    ${Object.entries(ageing)
        .map(([k, v]) => `${k}: ${v.toFixed(2)}`)
        .join(", ")}`,
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
