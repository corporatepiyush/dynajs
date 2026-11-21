// Master data deduplication — entity resolution across CRM, billing and support into golden customer records.
//
// WHAT IT IS
//   Three systems each hold "the" customer list, with typos, nicknames, moved addresses and reformatted phones.
//   This builds one master record per real person:
//
//   standardise  names (case, accents, nicknames: Bob -> Robert), emails (case, gmail dots and +tags), phones to
//                E.164 digits, addresses (abbreviations, unit numbers), dates to ISO
//   blocking     candidate pairs only within blocks — same postcode + first letter of surname, same email, same
//                phone — so 9,000 records need tens of thousands of comparisons, not 40 million; pair keys are
//                deduplicated in a RoaringBitmap
//   features     Jaro-Winkler on names, Levenshtein on street, Dice on the full address, exact email/phone/DOB flags
//   model        a logistic regression trained on a labelled sample of pairs, evaluated on held-out pairs
//                (precision/recall/F1), then used to score every candidate pair
//   clustering   union-find over the pairs above the threshold, visited best-first from a max IndexedHeap; transitive
//                chains are checked so one bad link cannot merge two clusters that disagree on date of birth
//   golden       survivorship rules per field: the most recent non-empty address, the most frequent name spelling,
//                the billing system's email over the CRM's; every master keeps its source keys (lineage)
//   blocking QA  pair completeness (true matches that survive blocking) and reduction ratio, from the hidden truth
//   stewardship  a steward answers the grey-zone queue best-first: confirmed pairs are merged
//   incremental  the next day's records are resolved against the existing masters through the same block index
//                (no batch re-run), so every existing master keeps its id; new people get new masters
//   output       a master table and a crosswalk in SQLite, with a stewardship queue for pairs in the grey zone
//   oracle       golden fields are scored against the people the records were generated from
//
// MODULES  dyna:matcher (JaroWinkler, Levenshtein, DiceCoefficient) · dyna:ml (LogisticRegression, metrics)
//          · dyna:structures (UnionFind, Multimap, RoaringBitmap, IndexedHeap, Multiset) · dyna:sqlite · dyna:time
//          · dyna:dataframe · dyna:random · dyna:file · Array extensions (sortWith)
//
// RUN      dynajs examples/data/master-data-dedup.js     generates 3,000 people across three systems and resolves them

import { JaroWinkler, Levenshtein, DiceCoefficient } from "dyna:matcher";
import { LogisticRegression, precision, recall, f1 } from "dyna:ml";
import { UnionFind, Multimap, RoaringBitmap, IndexedHeap, Multiset } from "dyna:structures";
import { SQLite } from "dyna:sqlite";
import { parseDate } from "dyna:time";
import { DataFrame } from "dyna:dataframe";
import { Random } from "dyna:random";
import { makeTempDir, removeAll } from "dyna:file";

const work = makeTempDir("mdm");
const rng = new Random(99);

// =============================================================================
// Standardisation
// =============================================================================
const NICK = {
    bob: "robert",
    rob: "robert",
    bill: "william",
    will: "william",
    liz: "elizabeth",
    beth: "elizabeth",
    kate: "katherine",
    katie: "katherine",
    jim: "james",
    jimmy: "james",
    mike: "michael",
    tom: "thomas",
    alex: "alexander",
    sasha: "alexander",
    jenny: "jennifer",
    jen: "jennifer",
    dan: "daniel",
    danny: "daniel",
    chris: "christopher",
    sam: "samuel",
    meg: "margaret",
    peggy: "margaret",
};
const STREET = {
    st: "street",
    "st.": "street",
    str: "street",
    rd: "road",
    "rd.": "road",
    ave: "avenue",
    av: "avenue",
    "ave.": "avenue",
    ln: "lane",
    dr: "drive",
    "dr.": "drive",
    blvd: "boulevard",
    apt: "#",
    unit: "#",
    "no.": "#",
};
const fold = (s) => s.normalize("NFD").replace(/\p{M}/gu, "").toLowerCase();
const stdName = (s) =>
    fold(s)
        .replace(/[^a-z' -]/g, "")
        .split(/\s+/)
        .filter(Boolean)
        .map((p) => NICK[p] || p);
const stdEmail = (e) => {
    if (!e) return "";
    let [local, domain] = fold(e.trim()).split("@");
    if (!domain) return "";
    local = local.replace(/\+.*/, "");
    if (domain === "googlemail.com") domain = "gmail.com";
    if (domain === "gmail.com") local = local.replace(/\./g, "");
    return `${local}@${domain}`;
};
const stdPhone = (p) => {
    const d = (p || "").replace(/\D/g, "");
    return d.length === 10 ? "1" + d : d.length === 11 && d[0] === "1" ? d : d ? d : "";
};
const stdAddress = (a) =>
    fold(a || "")
        .replace(/,/g, " ")
        .split(/\s+/)
        .filter(Boolean)
        .map((w) => STREET[w] || w)
        .join(" ")
        .replace(/# (\w+)/, "#$1");
const stdDob = (d) => {
    if (!d) return "";
    const us = /^(\d{1,2})\/(\d{1,2})\/(\d{4})$/.exec(d);
    if (us) return `${us[3]}-${us[1].padStart(2, "0")}-${us[2].padStart(2, "0")}`;
    try {
        return parseDate(d).toString();
    } catch {
        return "";
    }
};
function standardise(r) {
    const name = stdName(r.name);
    return {
        ...r,
        first: name[0] || "",
        last: name[name.length - 1] || "",
        email: stdEmail(r.email),
        phone: stdPhone(r.phone),
        street: stdAddress(r.street),
        zip: (r.zip || "").replace(/\s/g, "").slice(0, 5),
        dob: stdDob(r.dob),
    };
}

// =============================================================================
// Blocking and features
// =============================================================================
const blockKeys = (r) =>
    [
        r.zip && r.last && `z:${r.zip}:${r.last[0]}`,
        r.email && `e:${r.email}`,
        r.phone && `p:${r.phone}`,
        r.dob && r.last && `d:${r.dob}:${r.last.slice(0, 2)}`,
    ].filter(Boolean);
function candidatePairs(recs) {
    const blocks = new Multimap();
    recs.forEach((r, i) => {
        for (const k of blockKeys(r)) blocks.put(k, i);
    });
    const pairs = new RoaringBitmap(); // key a * 100000 + b, a < b
    let largest = 0;
    for (const key of blocks.keys()) {
        const ids = blocks.get(key);
        largest = Math.max(largest, ids.length);
        if (ids.length > 50) continue; // a block that large is a junk value
        for (let a = 0; a < ids.length; a++)
            for (let b = a + 1; b < ids.length; b++)
                pairs.add(ids[a] < ids[b] ? ids[a] * 100000 + ids[b] : ids[b] * 100000 + ids[a]);
    }
    return {
        pairs: Array.from(pairs.toArray(), (k) => [Math.floor(k / 100000), k % 100000]),
        blocks: blocks.keys().length,
        index: blocks,
        largest,
    };
}
const eq = (a, b) => (a && b ? (a === b ? 1 : 0) : 0.5); // 0.5 = unknown, neither evidence
const features = (a, b) => [
    JaroWinkler(a.first, b.first),
    JaroWinkler(a.last, b.last),
    a.street && b.street ? 1 - Levenshtein(a.street, b.street) / Math.max(a.street.length, b.street.length) : 0.5,
    DiceCoefficient(`${a.street} ${a.zip}`, `${b.street} ${b.zip}`),
    eq(a.email, b.email),
    eq(a.phone, b.phone),
    eq(a.dob, b.dob),
    eq(a.zip, b.zip),
];

// =============================================================================
// Golden records
// =============================================================================
const SOURCE_RANK = { billing: 3, crm: 2, support: 1 };
// how often each spelling occurs across ALL records: a one-off typo loses a tie to the spelling everyone else uses
const spelling = { first: new Multiset(), last: new Multiset() };
function golden(members) {
    const mode = (vals, global) => {
        const m = new Multiset();
        for (const v of vals) if (v) m.add(v);
        return (
            m
                .entrySet()
                .sortWith([
                    (a, b) => b[1] - a[1],
                    (a, b) => (global ? global.count(b[0]) - global.count(a[0]) : 0),
                    (a, b) => a[0].localeCompare(b[0]),
                ])[0]?.[0] || ""
        );
    };
    const byRecency = [...members].sort((a, b) => b.updated.localeCompare(a.updated));
    const bySource = [...members].sort((a, b) => SOURCE_RANK[b.source] - SOURCE_RANK[a.source]);
    const title = (s) => s.replace(/\b\w/g, (c) => c.toUpperCase());
    return {
        first: title(
            mode(
                members.map((m) => m.first),
                spelling.first,
            ),
        ),
        last: title(
            mode(
                members.map((m) => m.last),
                spelling.last,
            ),
        ),
        email: bySource.find((m) => m.email)?.email || "",
        phone: byRecency.find((m) => m.phone)?.phone || "",
        street: byRecency.find((m) => m.street)?.street || "",
        zip: byRecency.find((m) => m.zip)?.zip || "",
        dob: mode(members.map((m) => m.dob)),
        sources: members.map((m) => `${m.source}:${m.key}`),
    };
}

// =============================================================================
// Synthetic people in three systems
// =============================================================================
const FIRST = [
    "robert",
    "william",
    "elizabeth",
    "katherine",
    "james",
    "michael",
    "thomas",
    "alexander",
    "jennifer",
    "daniel",
    "christopher",
    "samuel",
    "margaret",
    "maria",
    "jose",
    "anna",
    "lukas",
    "sofia",
    "noah",
    "emma",
];
const NICKS = Object.fromEntries(FIRST.map((f) => [f, Object.keys(NICK).filter((k) => NICK[k] === f)]));
const LAST = [
    "smith",
    "johnson",
    "müller",
    "garcía",
    "brown",
    "o'neil",
    "nguyen",
    "kowalski",
    "schmidt",
    "rossi",
    "dubois",
    "jensen",
    "novak",
    "silva",
    "ivanova",
    "kim",
    "patel",
    "lopez",
    "martin",
    "becker",
];
const STREETS = ["Oak", "Maple", "Cedar", "Pine", "Elm", "Main", "Park", "Lake", "Hill", "River"];
const people = Array.from({ length: 3000 }, (_, i) => ({
    id: i,
    first: FIRST[rng.nextBounded(FIRST.length)],
    last: LAST[rng.nextBounded(LAST.length)],
    dob: `19${50 + rng.nextBounded(50)}-${String(1 + rng.nextBounded(12)).padStart(2, "0")}-${String(1 + rng.nextBounded(28)).padStart(2, "0")}`,
    street: `${1 + rng.nextBounded(999)} ${STREETS[rng.nextBounded(10)]} ${["St", "Rd", "Ave", "Lane"][rng.nextBounded(4)]}`,
    zip: String(10000 + rng.nextBounded(400) * 7),
    phone: `555${String(rng.nextBounded(10_000_000)).padStart(7, "0")}`,
    email: null,
}));
for (const p of people)
    p.email = `${p.first}.${fold(p.last).replace(/\W/g, "")}${p.id % 97}@${["gmail.com", "mail.example", "corp.example"][p.id % 3]}`;
const typo = (s) => {
    if (s.length < 4) return s;
    const i = 1 + rng.nextBounded(s.length - 2);
    return rng.nextFloat() < 0.5 ? s.slice(0, i) + s.slice(i + 1) : s.slice(0, i) + s[i + 1] + s[i] + s.slice(i + 2);
};
const cap = (s) => s.replace(/\b\w/g, (c) => c.toUpperCase());
function render(p, source, k) {
    const r = {
        source,
        key: `${source[0].toUpperCase()}${k}`,
        truth: p.id,
        updated: `2025-${String(1 + rng.nextBounded(12)).padStart(2, "0")}-01`,
    };
    const first =
        rng.nextFloat() < 0.25 && NICKS[p.first].length
            ? NICKS[p.first][rng.nextBounded(NICKS[p.first].length)]
            : p.first;
    r.name = cap(`${rng.nextFloat() < 0.08 ? typo(first) : first} ${rng.nextFloat() < 0.08 ? typo(p.last) : p.last}`);
    if (source === "crm" && rng.nextFloat() < 0.3) r.name = r.name.toUpperCase();
    r.email =
        rng.nextFloat() < 0.15
            ? ""
            : source === "support" && p.email.endsWith("gmail.com")
              ? p.email.replace(".", "").replace("@", "+help@")
              : p.email;
    r.phone =
        rng.nextFloat() < 0.2
            ? ""
            : source === "billing"
              ? `+1 (${p.phone.slice(0, 3)}) ${p.phone.slice(3, 6)}-${p.phone.slice(6)}`
              : p.phone;
    const moved = rng.nextFloat() < 0.1;
    r.street = moved
        ? `${1 + rng.nextBounded(999)} ${STREETS[rng.nextBounded(10)]} Street`
        : rng.nextFloat() < 0.5
          ? p.street
          : p.street.replace("St", "Street").replace("Rd", "Road").replace("Ave", "Avenue");
    r.zip = moved ? String(10000 + rng.nextBounded(400) * 7) : p.zip;
    const [y, m, d] = p.dob.split("-");
    r.dob = rng.nextFloat() < 0.1 ? "" : source === "crm" ? `${Number(m)}/${Number(d)}/${y}` : p.dob;
    return r;
}
const raw = [];
for (const p of people) {
    if (rng.nextFloat() < 0.9) raw.push(render(p, "crm", raw.length));
    if (rng.nextFloat() < 0.7) raw.push(render(p, "billing", raw.length));
    if (rng.nextFloat() < 0.5) raw.push(render(p, "support", raw.length));
    if (rng.nextFloat() < 0.15) raw.push(render(p, "crm", raw.length)); // a duplicate inside one system
}
const twins = people
    .filter((p) => p.id % 300 === 1)
    .map((p) => ({
        ...p,
        id: p.id + 100000,
        first: p.first === "anna" ? "emma" : "anna",
        dob: `${Number(p.dob.slice(0, 4)) - 27}${p.dob.slice(4)}`,
        email: "",
        phone: "",
    })); // a parent at the same address
for (const t of twins) raw.push(render(t, "crm", raw.length)); // same address and surname, different person

// =============================================================================
// Pipeline
// =============================================================================
const recs = raw.map(standardise);
for (const r of recs) {
    if (r.first) spelling.first.add(r.first);
    if (r.last) spelling.last.add(r.last);
}
const { pairs, blocks, largest, index: blockIndex } = candidatePairs(recs);
const X = pairs.map(([a, b]) => features(recs[a], recs[b]));
const y = pairs.map(([a, b]) => (recs[a].truth === recs[b].truth ? 1 : 0));

const labelled = [...pairs.keys()];
rng.shuffle(labelled);
const train = labelled.slice(0, 2000),
    test = labelled.slice(2000, 6000); // a data steward labelled 2,000 pairs
const model = await new LogisticRegression({ maxIter: 4000, learningRate: 0.5, classWeight: "balanced" }).fit(
    train.map((i) => X[i]),
    train.map((i) => y[i]),
);
const probaTest = (await model.predictProba(test.map((i) => X[i]))).map((p) => p[1]);
const yTest = test.map((i) => y[i]),
    yHat = probaTest.map((p) => (p >= 0.5 ? 1 : 0));
const quality = {
    precision: await precision(yTest, yHat),
    recall: await recall(yTest, yHat),
    f1: await f1(yTest, yHat),
};

const scores = (await model.predictProba(X)).map((p) => p[1]);
const MATCH = 0.85,
    REVIEW = 0.4;
let dayTwo;
const uf = new UnionFind(recs.length);
const review = [];
const best = new IndexedHeap({ max: true }).pushMany(Uint32Array.from(scores.keys()), Float64Array.from(scores));
const order = Array.from(best.popUntil(REVIEW), (i) => [scores[i], i]); // every pair >= REVIEW, best first
const clusterDob = new Map(); // root -> set of known DOBs
const dobsOf = (root) => clusterDob.get(root) || new Set();
let refused = 0;
for (const [s, i] of order) {
    const [a, b] = pairs[i];
    if (s < MATCH) {
        review.push([a, b, s]);
        continue;
    }
    const ra = uf.find(a),
        rb = uf.find(b);
    if (ra === rb) continue;
    const da = new Set([...dobsOf(ra), recs[a].dob].filter(Boolean)),
        db = new Set([...dobsOf(rb), recs[b].dob].filter(Boolean));
    if (da.size && db.size && ![...da].some((d) => db.has(d))) {
        refused++;
        review.push([a, b, s]);
        continue;
    } // the chain would join two birthdays
    uf.union(a, b);
    clusterDob.set(uf.find(a), new Set([...da, ...db]));
}
// Blocking QA: of all pairs that really are one person, how many did blocking even let the model see?
const truthSize = new Map();
for (const r of recs) truthSize.set(r.truth, (truthSize.get(r.truth) || 0) + 1);
const truePairs = [...truthSize.values()].reduce((s, n) => s + (n * (n - 1)) / 2, 0);
const pairCompleteness = y.sum() / truePairs,
    reductionRatio = 1 - pairs.length / ((recs.length * (recs.length - 1)) / 2);

// Cluster quality against the hidden truth: pairwise precision/recall over records.
function pairwise() {
    const byRoot = Map.groupBy(recs.keys(), (i) => uf.find(i));
    let tp = 0,
        fp = 0,
        fn = 0;
    for (const idx of byRoot.values())
        for (let a = 0; a < idx.length; a++)
            for (let b = a + 1; b < idx.length; b++) recs[idx[a]].truth === recs[idx[b]].truth ? tp++ : fp++;
    for (const ids of Map.groupBy(recs.keys(), (i) => recs[i].truth).values())
        for (let a = 0; a < ids.length; a++)
            for (let b = a + 1; b < ids.length; b++) if (uf.find(ids[a]) !== uf.find(ids[b])) fn++;
    return { precision: tp / (tp + fp), recall: tp / (tp + fn) };
}
const beforeSteward = pairwise();
// Stewardship: the 300 highest-scoring grey-zone pairs, answered by someone who can look at the documents
// (the hidden truth). Confirmed pairs merge; rejected ones leave the queue as decided.
let confirmed = 0,
    rejected = 0;
for (const [a, b] of review.toSorted((p, q) => q[2] - p[2]).slice(0, 300))
    if (recs[a].truth === recs[b].truth) {
        if (uf.find(a) !== uf.find(b)) {
            uf.union(a, b);
            confirmed++;
        }
    } else rejected++;
const afterSteward = pairwise();
const clusters = new Multimap(); // root -> record index
recs.forEach((r, i) => clusters.put(String(uf.find(i)), i));
const clusterIdx = clusters.keys().map((k) => clusters.get(k));
const masters = clusterIdx.map((idx) => golden(idx.map((i) => recs[i])));

const byTruth = Map.groupBy(recs.keys(), (i) => recs[i].truth);
// Golden records against the people they came from (pure clusters only: a mixed one has no single truth)
const personOf = new Map([...people, ...twins].map((p) => [p.id, p]));
const gold = { dob: 0, email: 0, first: 0, last: 0, n: 0 };
clusterIdx.forEach((idx, k) => {
    const truth = recs[idx[0]].truth;
    if (!idx.every((i) => recs[i].truth === truth)) return;
    const p = personOf.get(truth),
        m = masters[k];
    gold.n++;
    gold.dob += m.dob === p.dob || (!m.dob && idx.every((i) => !recs[i].dob));
    gold.email += m.email === stdEmail(p.email || "") || (!m.email && idx.every((i) => !recs[i].email));
    gold.first += m.first.toLowerCase() === p.first;
    gold.last += fold(m.last) === fold(p.last);
});

const db = new SQLite(work.join("mdm.db"));
db.exec(
    "CREATE TABLE master (id INTEGER PRIMARY KEY, first TEXT, last TEXT, email TEXT, phone TEXT, street TEXT, zip TEXT, dob TEXT)",
);
db.exec("CREATE TABLE crosswalk (source_key TEXT PRIMARY KEY, master_id INTEGER REFERENCES master(id))");
db.exec("CREATE TABLE stewardship (a TEXT, b TEXT, score REAL)");
db.exec("BEGIN");
masters.forEach((m, id) => {
    db.exec("INSERT INTO master VALUES (?,?,?,?,?,?,?,?)", [
        id,
        m.first,
        m.last,
        m.email,
        m.phone,
        m.street,
        m.zip,
        m.dob,
    ]);
    for (const s of m.sources) db.exec("INSERT INTO crosswalk VALUES (?, ?)", [s.split(":")[1], id]);
});
for (const [a, b, s] of review) db.exec("INSERT INTO stewardship VALUES (?,?,?)", [recs[a].key, recs[b].key, s]);
db.exec("COMMIT");
const perSource = db.query(
    "SELECT substr(source_key,1,1) AS s, COUNT(DISTINCT master_id) AS masters FROM crosswalk GROUP BY s ORDER BY s",
);
const sizes = new DataFrame({ n: Float64Array.from(clusterIdx, (c) => c.length) });

const results = {};
const pairPrecision = afterSteward.precision,
    pairRecall = afterSteward.recall;
results["email and phone canonical forms"] =
    stdEmail("J.Smith+promo@GoogleMail.com") === "jsmith@gmail.com" && stdPhone("+1 (555) 123-4567") === "15551234567";
results["nickname and address standardisation"] =
    stdName("Bob O'Neil").join(" ") === "robert o'neil" && stdAddress("12 Oak St., Apt 4") === "12 oak street #4";
results[`blocking keeps ${pairs.length} of ${((recs.length * (recs.length - 1)) / 2).toExponential(1)} pairs`] =
    pairs.length < recs.length * 30;
results[`pair classifier F1 ${quality.f1.toFixed(3)} on held-out pairs`] = quality.f1 > 0.9;
results[`cluster pairwise precision ${pairPrecision.toFixed(3)}, recall ${pairRecall.toFixed(3)}`] =
    pairPrecision > 0.97 && pairRecall > 0.85;
results[`${masters.length} masters for ${byTruth.size} real people`] =
    Math.abs(masters.length - new Set(recs.map((r) => r.truth)).size) / byTruth.size < 0.1;
results["same-address relatives with different first names are not merged"] = twins.every((t) => {
    const tr = recs.findIndex((r) => r.truth === t.id);
    const orig = recs.findIndex((r) => r.truth === t.id - 100000);
    return tr < 0 || orig < 0 || uf.find(tr) !== uf.find(orig);
});
results["every source record maps to exactly one master"] =
    db.query("SELECT COUNT(*) AS n FROM crosswalk")[0].n === recs.length;
results["every merged master has a surviving first name"] = masters.every((m) => m.sources.length === 1 || m.first);
results[
    `blocking keeps ${(pairCompleteness * 100).toFixed(1)}% of true pairs while discarding ${(reductionRatio * 100).toFixed(3)}% of all pairs`
] = pairCompleteness > 0.97 && reductionRatio > 0.999;
results[
    `golden records: DOB ${gold.dob}/${gold.n}, email ${gold.email}/${gold.n}, first ${gold.first}/${gold.n}, last ${gold.last}/${gold.n} match the person`
] = gold.dob / gold.n > 0.995 && gold.email / gold.n > 0.98 && gold.first / gold.n > 0.98 && gold.last / gold.n > 0.98;
results[`stewardship (${confirmed} confirmed, ${rejected} rejected) raises recall without losing precision`] =
    afterSteward.recall > beforeSteward.recall && afterSteward.precision >= beforeSteward.precision && confirmed > 0;
{
    // Day two: 250 records from known people and 60 new people, resolved against the existing masters only
    const masterOfRoot = new Map(clusterIdx.map((idx, id) => [uf.find(idx[0]), id]));
    const next = [];
    for (let k = 0; k < 250; k++) next.push(render(people[rng.nextBounded(people.length)], "support", raw.length + k));
    // the new people are family members of existing customers: same surname and address, their own name, birthday
    // and phone -- what a careless link would merge
    const fresh = Array.from({ length: 60 }, (_, k) => ({
        ...people[k],
        id: 200000 + k,
        first: FIRST[(FIRST.indexOf(people[k].first) + 7) % FIRST.length],
        dob: `20${String(k % 10).padStart(2, "0")}-01-15`,
        phone: `555${String(9_000_000 + k)}`,
    }));
    for (const f of fresh) {
        f.email = `${f.first}.${fold(f.last).replace(/\W/g, "")}${f.id}@mail.example`;
        next.push(render(f, "billing", raw.length + next.length));
    }
    const nextRecs = next.map(standardise);
    const oldCrosswalk = new Map(
        db.query("SELECT source_key, master_id FROM crosswalk").map((r) => [r.source_key, r.master_id]),
    );
    let linked = 0,
        linkedRight = 0,
        created = 0,
        freshNew = 0,
        nextId = masters.length;
    const assigned = new Map(); // new record index -> master id, so a second record of a new person joins the first
    // candidates through the existing block index, scored in one batch
    const cand = nextRecs.flatMap((r, j) =>
        [...new Set(blockKeys(r).flatMap((k) => blockIndex.get(k) || []))].map((i) => [j, i]),
    );
    const candScore = cand.length
        ? (await model.predictProba(cand.map(([j, i]) => features(nextRecs[j], recs[i])))).map((q) => q[1])
        : [];
    const bestOf = new Map(); // new record -> [score, master id]
    cand.forEach(([j, i], k) => {
        const r = nextRecs[j];
        if (candScore[k] < MATCH) return; // the model already weighs the DOB; a separate guard here changed nothing
        if (!(bestOf.get(j)?.[0] >= candScore[k])) bestOf.set(j, [candScore[k], masterOfRoot.get(uf.find(i))]);
    });
    db.exec("BEGIN");
    nextRecs.forEach((r, j) => {
        let bestId = bestOf.get(j)?.[1] ?? -1;
        if (bestId >= 0) {
            linked++;
            linkedRight += clusterIdx[bestId].some((i) => recs[i].truth === r.truth);
        } else {
            bestId = nextId++;
            created++;
            freshNew += r.truth >= 200000;
        }
        assigned.set(j, bestId);
        db.exec("INSERT INTO crosswalk VALUES (?, ?)", [r.key, bestId]);
    });
    db.exec("COMMIT");
    const stable = [...oldCrosswalk].every(
        ([k, id]) => db.query("SELECT master_id FROM crosswalk WHERE source_key = ?", [k])[0].master_id === id,
    );
    dayTwo = { linked, linkedRight, created, freshNew, known: 250, fresh: fresh.length };
    results[`day two links ${linked} records to existing masters, ${linkedRight} correctly`] =
        linkedRight / linked > 0.99 && linked > 0.85 * 250;
    results[`and opens ${created} new masters, ${freshNew} of them for the ${fresh.length} new people`] =
        freshNew === fresh.length && created - freshNew < 0.15 * 250;
    results["every existing crosswalk row keeps its master id"] = stable;
}
db.close();

print("master-data-dedup");
print(
    `  records    ${recs.length} from crm/billing/support for ${byTruth.size} real people (${twins.length} look-alike relatives)`,
);
print(`  blocking   ${blocks} blocks (largest ${largest}), ${pairs.length} candidate pairs`);
print(
    `  model      held-out precision ${quality.precision.toFixed(3)} recall ${quality.recall.toFixed(3)} F1 ${quality.f1.toFixed(3)}; weights ${model.coef[0].map((w) => w.toFixed(1)).join(" ")}`,
);
print(
    `  clusters   ${masters.length} masters, sizes p50 ${sizes.MEDIAN("n")} max ${sizes.MAX("n")}; ${refused} links refused on DOB conflict, ${review.length} pairs to steward`,
);
print(
    `  quality    pairwise precision ${pairPrecision.toFixed(4)}, recall ${pairRecall.toFixed(4)} (before stewardship ${beforeSteward.precision.toFixed(4)} / ${beforeSteward.recall.toFixed(4)})`,
);
print(
    `  day two    ${dayTwo.linked} linked to existing masters (${dayTwo.linkedRight} right), ${dayTwo.created} new masters (${dayTwo.freshNew} of ${dayTwo.fresh} new people)`,
);
print(`  coverage   ${perSource.map((r) => `${r.s}:${r.masters}`).join("  ")}`);
model.close();
removeAll(work);
check(results);

function check(results) {
    const names = Object.keys(results);
    const failed = names.filter((name) => !results[name]);
    for (const name of failed) print("FAIL " + name);
    if (failed.length) throw new Error(`${failed.length} of ${names.length} checks failed`);
}
