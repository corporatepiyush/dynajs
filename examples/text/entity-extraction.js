// Entity extraction — gazetteer and pattern-based named-entity recognition with longest-match conflict resolution,
// validation, normalisation and entity linking, scored by precision / recall / F1 against gold annotations.
//
// WHAT IT IS
//   The extraction stage of a document-processing pipeline (contracts, news, support mail):
//
//   gazetteers  people and organisations from alias lists matched in ONE MultiMatcher pass; overlapping candidates are
//               resolved longest-first with a RangeSet of claimed character spans, so "Bank of Avalon Holdings" wins
//               over "Bank of Avalon" and a person's name inside an organisation is not double-tagged
//   patterns    money ("EUR 1.250,00", "$3.5 million"), dates (ISO, "12 March 2026", "03/12/2026"), e-mail, IBAN,
//               percentages; every candidate is VALIDATED (real calendar date, IBAN mod-97 per ISO 13616, sane amount)
//               before it becomes an entity
//   normalise   dates to ISO-8601, money to a dyna:decimal Decimal plus currency code
//   linking     aliases map to canonical ids through a BiMap; a later surname-only mention ("Okafor") links to the
//               full person seen earlier in the same document
//   scoring     documents are generated WITH their gold spans; exact-span precision, recall and F1 per type, plus
//               counts of rejected look-alikes (an invalid date, a bad IBAN checksum)
//
// MODULES  dyna:matcher (MultiMatcher) · dyna:structures (RangeSet, BiMap, Multiset) · dyna:decimal (Decimal) ·
//          dyna:random · dyna:cli · String/Array extensions
//
// RUN      dynajs examples/text/entity-extraction.js

import { MultiMatcher } from "dyna:matcher";
import { RangeSet, BiMap, Multiset } from "dyna:structures";
import { Decimal } from "dyna:decimal";
import { Random } from "dyna:random";
import { Table } from "dyna:cli";

const results = {};
const rng = new Random(2003);

// =============================================================================
// Knowledge: people, organisations, aliases
// =============================================================================
const PEOPLE = [
    ["P1", "Adaeze Okafor"],
    ["P2", "Lars Henriksen"],
    ["P3", "Mei-Ling Chou"],
    ["P4", "Diego Alvarez"],
    ["P5", "Priya Raman"],
    ["P6", "Tomasz Nowak"],
];
const ORGS = [
    ["O1", "Bank of Avalon"],
    ["O2", "Bank of Avalon Holdings"],
    ["O3", "Northwind Logistics GmbH"],
    ["O4", "Helios Energy Inc."],
    ["O5", "Okafor Foundation"],
    ["O6", "Raman & Partners LLP"],
];
const aliases = new BiMap();
for (const [id, name] of [...PEOPLE, ...ORGS]) aliases.set(name, id);
const surnameOf = new Map(PEOPLE.map(([id, n]) => [n.split(" ").at(-1), id]));
const gaz = [
    ...PEOPLE.map(([, n]) => ({ text: n, type: "PERSON" })),
    ...ORGS.map(([, n]) => ({ text: n, type: "ORG" })),
    ...[...surnameOf.keys()].map((s) => ({ text: s, type: "PERSON" })),
];
const gazMatcher = new MultiMatcher(gaz.map((g) => g.text));

// =============================================================================
// Validators
// =============================================================================
const MONTHS = [
    "january",
    "february",
    "march",
    "april",
    "may",
    "june",
    "july",
    "august",
    "september",
    "october",
    "november",
    "december",
];
const realDate = (y, m, d) => {
    const t = new Date(Date.UTC(y, m - 1, d));
    return t.getUTCFullYear() === y && t.getUTCMonth() === m - 1 && t.getUTCDate() === d;
};
function ibanValid(raw) {
    const s = raw.replace(/\s+/g, "");
    if (!/^[A-Z]{2}\d{2}[A-Z0-9]{10,30}$/.test(s)) return false;
    const moved = s.slice(4) + s.slice(0, 4);
    const digits = [...moved].map((c) => (/\d/.test(c) ? c : String(c.charCodeAt(0) - 55))).join("");
    return BigInt(digits) % 97n === 1n;
}
function ibanWithCheck(country, bban) {
    const digits = [...(bban + country + "00")].map((c) => (/\d/.test(c) ? c : String(c.charCodeAt(0) - 55))).join("");
    const check = 98n - (BigInt(digits) % 97n);
    return `${country}${String(check).padStart(2, "0")}${bban}`;
}
results["IBAN validator accepts the ISO 13616 example GB82 WEST 1234 5698 7654 32"] =
    ibanValid("GB82 WEST 1234 5698 7654 32");
results["IBAN validator rejects a single changed digit"] = !ibanValid("GB82 WEST 1234 5698 7654 33");

const PATTERNS = [
    {
        type: "MONEY",
        re: /(?:EUR|USD|GBP)\s?\d+(?:[.,]\d{3})*(?:[.,]\d{2})?|\$\d+(?:\.\d+)?\s(?:million|billion)/g,
        norm: (m) => {
            const cur = m.startsWith("$") ? "USD" : m.slice(0, 3);
            if (m.startsWith("$")) {
                const [n, unit] = m.slice(1).split(" ");
                return {
                    currency: cur,
                    amount: new Decimal(n).mul(unit === "million" ? "1000000" : "1000000000").toString(),
                };
            }
            let n = m.slice(3).trim();
            n = /,\d{2}$/.test(n) ? n.replace(/\./g, "").replace(",", ".") : n.replace(/,/g, "");
            return { currency: cur, amount: new Decimal(n).toFixed(2) };
        },
    },
    {
        type: "DATE",
        re: /\b\d{4}-\d{2}-\d{2}\b|\b\d{1,2} (?:January|February|March|April|May|June|July|August|September|October|November|December) \d{4}\b|\b\d{2}\/\d{2}\/\d{4}\b/g,
        norm: (m) => {
            let y, mo, d;
            if (/^\d{4}-/.test(m)) [y, mo, d] = m.split("-").map(Number);
            else if (m.includes("/")) [mo, d, y] = m.split("/").map(Number);
            else {
                const [dd, mon, yy] = m.split(" ");
                d = Number(dd);
                mo = MONTHS.indexOf(mon.toLowerCase()) + 1;
                y = Number(yy);
            }
            return realDate(y, mo, d) ? `${y}-${String(mo).padStart(2, "0")}-${String(d).padStart(2, "0")}` : null;
        },
    },
    { type: "EMAIL", re: /\b[\w.+-]+@[\w-]+(?:\.[\w-]+)+\b/g, norm: (m) => m.toLowerCase() },
    {
        type: "IBAN",
        re: /\b[A-Z]{2}\d{2}(?: ?[A-Z0-9]{4}){3,7}(?: ?[A-Z0-9]{1,4})?\b/g,
        norm: (m) => (ibanValid(m) ? m.replace(/\s+/g, "") : null),
    },
    { type: "PERCENT", re: /\b\d+(?:\.\d+)?%/g, norm: (m) => Number(m.slice(0, -1)) },
];

// =============================================================================
// Extraction
// =============================================================================
const rejected = new Multiset();
function extract(text) {
    const cands = [];
    for (const m of gazMatcher.allIn(text)) {
        const g = gaz[m.index],
            end = m.at + g.text.length;
        const okLeft = m.at === 0 || !/\w/.test(text[m.at - 1]),
            okRight = end >= text.length || !/\w/.test(text[end]);
        if (okLeft && okRight) cands.push({ start: m.at, end, type: g.type, text: g.text });
    }
    for (const p of PATTERNS)
        for (const m of text.matchAll(p.re)) {
            const value = p.norm(m[0]);
            if (value === null) {
                rejected.add(p.type);
                continue;
            }
            cands.push({ start: m.index, end: m.index + m[0].length, type: p.type, text: m[0], value });
        }
    cands.sort((a, b) => b.end - b.start - (a.end - a.start) || a.start - b.start);
    const claimed = new RangeSet(),
        out = [];
    for (const c of cands) {
        if (claimed.intersects(c.start, c.end)) continue;
        claimed.add(c.start, c.end);
        out.push(c);
    }
    out.sort((a, b) => a.start - b.start);
    const seenPeople = new Set();
    for (const e of out) {
        if (e.type === "PERSON") {
            const full = aliases.get(e.text);
            if (full) {
                e.id = full;
                seenPeople.add(full);
            } else {
                const id = surnameOf.get(e.text);
                e.id = seenPeople.has(id) ? id : null;
            }
        } else if (e.type === "ORG") e.id = aliases.get(e.text);
    }
    return out;
}

// =============================================================================
// Synthetic documents with gold spans
// =============================================================================
function document() {
    const parts = [],
        gold = [];
    const add = (s, type, extra) => {
        if (type) gold.push({ start: parts.join("").length, end: parts.join("").length + s.length, type, ...extra });
        parts.push(s);
    };
    const person = PEOPLE[rng.nextBounded(PEOPLE.length)],
        org = ORGS[rng.nextBounded(ORGS.length)];
    add("On ");
    const y = 2025 + rng.nextBounded(2),
        mo = 1 + rng.nextBounded(12),
        d = 1 + rng.nextBounded(28);
    const style = rng.nextBounded(3);
    add(
        style === 0
            ? `${y}-${String(mo).padStart(2, "0")}-${String(d).padStart(2, "0")}`
            : style === 1
              ? `${d} ${MONTHS[mo - 1][0].toUpperCase()}${MONTHS[mo - 1].slice(1)} ${y}`
              : `${String(mo).padStart(2, "0")}/${String(d).padStart(2, "0")}/${y}`,
        "DATE",
    );
    add(", ");
    add(person[1], "PERSON", { id: person[0] });
    add(" of ");
    add(org[1], "ORG", { id: org[0] });
    const amount = 1000 + rng.nextBounded(900_000);
    add(" approved a payment of ");
    add(`EUR ${amount.toLocaleString("en-US")}.00`, "MONEY");
    add(" to account ");
    const iban = ibanWithCheck("DE", String(10_000_000 + rng.nextBounded(89_999_999)).padEnd(18, "0"));
    add(iban.replace(/(.{4})/g, "$1 ").trim(), "IBAN");
    add(", a ");
    add(`${(rng.nextBounded(200) / 10).toFixed(1)}%`, "PERCENT");
    add(" increase. ");
    add(person[1].split(" ").at(-1), "PERSON", { id: person[0] });
    add(" can be reached at ");
    add(`${person[1].split(" ")[0].toLowerCase()}@${org[1].split(" ")[0].toLowerCase()}.example`, "EMAIL");
    add(". A draft dated 2026-02-30 and account DE00 1234 5678 9012 3456 78 were withdrawn.");
    if (rng.nextFloat() < 0.3) {
        add(" The deal is worth ");
        add(`$${1 + rng.nextBounded(9)}.${rng.nextBounded(10)} million`, "MONEY");
        add(".");
    }
    return { text: parts.join(""), gold };
}

const docs = Array.from({ length: 400 }, document);
const tp = new Multiset(),
    fp = new Multiset(),
    fn = new Multiset();
let linked = 0,
    linkable = 0;
const t0 = performance.now();
for (const doc of docs) {
    const found = extract(doc.text);
    const key = (e) => `${e.type}:${e.start}:${e.end}`;
    const goldKeys = new Map(doc.gold.map((g) => [key(g), g]));
    for (const e of found) {
        const g = goldKeys.get(key(e));
        if (g) {
            tp.add(e.type);
            goldKeys.delete(key(e));
            if (g.id) {
                linkable++;
                if (e.id === g.id) linked++;
            }
        } else fp.add(e.type);
    }
    for (const g of goldKeys.values()) fn.add(g.type);
}
const ms = performance.now() - t0;
const rows = ["PERSON", "ORG", "DATE", "MONEY", "IBAN", "EMAIL", "PERCENT"].map((t) => {
    const p = tp.count(t) / Math.max(1, tp.count(t) + fp.count(t)),
        r = tp.count(t) / Math.max(1, tp.count(t) + fn.count(t));
    return { t, p, r, f1: p + r ? (2 * p * r) / (p + r) : 0 };
});
print(
    Table(
        rows.map((x) => [x.t, x.p.toFixed(3), x.r.toFixed(3), x.f1.toFixed(3), tp.count(x.t)]),
        { head: ["type", "precision", "recall", "F1", "true positives"] },
    ),
);
results["every entity type scores F1 >= 0.99"] = rows.every((x) => x.f1 >= 0.99);
results["mentions link to the right canonical id"] = linked === linkable && linkable > 700;
results["the impossible date 2026-02-30 is rejected every time"] = rejected.count("DATE") >= docs.length;
results["the bad-checksum IBAN is rejected every time"] = rejected.count("IBAN") >= docs.length;
{
    const sample = extract(
        "Bank of Avalon Holdings paid EUR 1.250,00 on 12 March 2026; Adaeze Okafor of Okafor Foundation agreed.",
    );
    const types = sample.map((e) => `${e.type}:${e.text}`);
    results["longest match wins: the holding company, not the bank"] =
        types[0] === "ORG:Bank of Avalon Holdings" && !types.includes("ORG:Bank of Avalon");
    results["European money format normalises to 1250.00 EUR"] =
        sample.find((e) => e.type === "MONEY").value.amount === "1250.00";
    results["a written date normalises to ISO"] = sample.find((e) => e.type === "DATE").value === "2026-03-12";
    results["a surname inside an organisation is not a person"] =
        sample.filter((e) => e.type === "PERSON").length === 1;
    results["a lone surname before any full mention is left unlinked"] = extract("Okafor said nothing.")[0].id === null;
}
print(
    `${docs.length} documents in ${ms.toFixed(0)} ms; rejected look-alikes: ${rejected
        .entrySet()
        .map(([k, n]) => `${k} ${n}`)
        .join(", ")}`,
);

check(results);

function check(results) {
    const names = Object.keys(results);
    const failed = names.filter((name) => !results[name]);
    for (const name of failed) print("FAIL " + name);
    if (failed.length) throw new Error(`${failed.length} of ${names.length} checks failed`);
}
