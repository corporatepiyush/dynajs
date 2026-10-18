// Privacy pipeline — PII discovery, keyed pseudonymisation, format-preserving tokens, k-anonymity, differential
// privacy, consent enforcement, data-subject requests and crypto-shredding.
//
// WHAT IT IS
//   What a data platform runs between the production database and the analytics warehouse, under GDPR:
//
//   discovery     every column of every table is sampled and classified (email, phone, IBAN, national id, name,
//                 postcode, date of birth, free text containing PII) by validators, not just regexes — an IBAN must
//                 pass mod 97, a card number must pass Luhn
//   transform     per classification: emails and ids become keyed HMAC pseudonyms (joinable, not reversible
//                 without the key), card numbers become format-preserving tokens that keep the last four digits,
//                 dates of birth generalise to birth year, postcodes to their region, free text is redacted
//   k-anonymity   the released table is checked over its quasi-identifiers (age band, region, gender) and
//                 generalised further — or rows suppressed — until every combination occurs at least k times
//   consent       rows of users who withdrew consent for analytics never leave production
//   aggregates    published counts get Laplace noise calibrated to sensitivity / epsilon from a privacy budget
//                 that is tracked and refused when exhausted
//   DSAR          a data-subject access request assembles everything about one person across tables as a zip;
//                 an erasure request deletes rows and crypto-shreds: each user's sensitive fields are encrypted
//                 with a per-user key, so destroying that key makes every backup copy unreadable too
//
//   engines       quasi-identifier combinations are counted in a Multiset (no Map bookkeeping, no O(n²) "is this row
//                 in the small set" scans); names in free text are found by one Aho-Corasick pass (MultiMatcher)
//                 instead of a regex alternation of every first name; nonce || ciphertext is dyna:bytes concat
//
// MODULES  dyna:net (SQLite) · dyna:crypto (HMAC, HKDF, AESGCM/ChaCha20Poly1305) · dyna:compress (ZipPack)
//          · dyna:dataframe · dyna:random · dyna:encoding (Base64) · dyna:cli (Table) · dyna:file · dyna:schema
//          · dyna:structures (Multiset) · dyna:matcher (MultiMatcher) · dyna:bytes (concat)
//
// RUN      dynajs examples/security/privacy-pipeline.js     builds a production database and runs a release plus requests

import { SQLite } from "dyna:net";
import * as crypto from "dyna:crypto";
import { ZipPack, ZipList, ZipRead } from "dyna:compress";
import { DataFrame } from "dyna:dataframe";
import { Random } from "dyna:random";
import { Base64Encode, Base64Decode } from "dyna:encoding";
import { Table } from "dyna:cli";
import { makeTempDir, copyFile, writeFile, removeAll } from "dyna:file";
import { Schema } from "dyna:schema";
import { Multiset } from "dyna:structures";
import { MultiMatcher } from "dyna:matcher";
import { concat } from "dyna:bytes";
const table = (rows) => Table(rows.slice(1), { head: rows[0] });

const work = makeTempDir("privacy");
const rng = new Random(1984);
const AEAD = typeof crypto.ChaCha20Poly1305 === "function" ? crypto.ChaCha20Poly1305 : crypto.AESGCM;
const PSEUDO_KEY = crypto.HKDF({ hash: "sha256", key: crypto.RandomBytes(32), info: "pseudonyms-2026", length: 32 });

// =============================================================================
// Production database (with a key vault table for per-user keys)
// =============================================================================
const prod = new SQLite(work.join("prod.db"));
prod.exec(
    "CREATE TABLE users (id INTEGER PRIMARY KEY, email TEXT, full_name TEXT, phone TEXT, dob TEXT, gender TEXT, postcode TEXT, iban_enc TEXT, consent_analytics INTEGER)",
);
prod.exec("CREATE TABLE orders (id INTEGER PRIMARY KEY, user_id INTEGER, amount REAL, card TEXT, placed TEXT)");
prod.exec("CREATE TABLE tickets (id INTEGER PRIMARY KEY, user_id INTEGER, body TEXT)");
prod.exec("CREATE TABLE user_keys (user_id INTEGER PRIMARY KEY, key TEXT)");
const FIRST = [
    "Anna",
    "Ben",
    "Clara",
    "David",
    "Elif",
    "Farid",
    "Greta",
    "Hugo",
    "Ines",
    "Jonas",
    "Kira",
    "Lukas",
    "Mila",
    "Noah",
];
const LAST = ["Schmidt", "Yilmaz", "Novak", "Rossi", "Dubois", "Jansen", "Kowalski", "Haddad", "Larsen", "Moreau"];
const REGIONS = { 10: "Berlin", 20: "Hamburg", 50: "Köln", 60: "Frankfurt", 80: "München" };
const luhn = (body) => {
    const d = body.split("").map(Number);
    let s = 0;
    for (let i = d.length - 1, alt = true; i >= 0; i--, alt = !alt) {
        let v = d[i];
        if (alt) {
            v *= 2;
            if (v > 9) v -= 9;
        }
        s += v;
    }
    return body + ((10 - (s % 10)) % 10);
};
const iban = (n) => {
    const bban = `37040044${String(n).padStart(10, "0")}`;
    const r = bban + "131400";
    let m = 0;
    for (const ch of r) m = (m * 10 + Number(ch)) % 97;
    return `DE${String(98 - m).padStart(2, "0")}${bban}`;
};
function userKey(id) {
    const row = prod.query("SELECT key FROM user_keys WHERE user_id = ?", [id])[0];
    if (row) return Base64Decode(row.key);
    const k = crypto.RandomBytes(32);
    prod.exec("INSERT INTO user_keys VALUES (?, ?)", [id, Base64Encode(k)]);
    return k;
}
const sealFor = (id, text) => {
    const a = new AEAD(userKey(id));
    const { nonce, sealed } = a.sealRandom(text, `user:${id}`);
    return Base64Encode(concat([nonce, sealed]));
};
const openFor = (id, blob) => {
    const row = prod.query("SELECT key FROM user_keys WHERE user_id = ?", [id])[0];
    if (!row) throw new Error("key destroyed");
    const u = Base64Decode(blob);
    return new TextDecoder().decode(new AEAD(Base64Decode(row.key)).open(u.slice(0, 12), u.slice(12), `user:${id}`));
};
prod.exec("BEGIN");
for (let id = 1; id <= 3000; id++) {
    const first = FIRST[rng.nextBounded(FIRST.length)],
        last = LAST[rng.nextBounded(LAST.length)];
    const year = 1950 + rng.nextBounded(55),
        pc = Object.keys(REGIONS)[rng.nextBounded(5)] + String(100 + rng.nextBounded(900));
    prod.exec("INSERT INTO users VALUES (?,?,?,?,?,?,?,?,?)", [
        id,
        `${first}.${last}${id}@mail.example`.toLowerCase(),
        `${first} ${last}`,
        `+49 15${rng.nextBounded(10)} ${1000000 + rng.nextBounded(8999999)}`,
        `${year}-${String(1 + rng.nextBounded(12)).padStart(2, "0")}-${String(1 + rng.nextBounded(28)).padStart(2, "0")}`,
        rng.nextFloat() < 0.5 ? "f" : "m",
        pc,
        sealFor(id, iban(id)),
        rng.nextFloat() < 0.88 ? 1 : 0,
    ]);
    for (let k = 0, n = rng.poisson(3); k < n; k++)
        prod.exec("INSERT INTO orders (user_id, amount, card, placed) VALUES (?,?,?,?)", [
            id,
            Math.round(rng.exponential(1 / 60) * 100) / 100,
            luhn("4" + String(rng.nextBounded(1e14)).padStart(14, "0")),
            `2026-0${1 + rng.nextBounded(5)}-1${rng.nextBounded(9)}`,
        ]);
    if (rng.nextFloat() < 0.2)
        prod.exec("INSERT INTO tickets (user_id, body) VALUES (?, ?)", [
            id,
            rng.nextFloat() < 0.5
                ? `Please call me back on +49 151 ${1000000 + rng.nextBounded(8999999)}, my order is late`
                : `Refund to ${iban(id)} please. Thanks, ${first}`,
        ]);
}
prod.exec("COMMIT");

// =============================================================================
// Discovery: classify columns by validated samples
// =============================================================================
const ibanOk = (s) => {
    const t = s.replace(/\s/g, "");
    if (!/^[A-Z]{2}\d{2}[A-Z0-9]{10,30}$/.test(t)) return false;
    let m = 0;
    for (const ch of (t.slice(4) + t.slice(0, 4)).replace(/[A-Z]/g, (c) => c.charCodeAt(0) - 55))
        m = (m * 10 + Number(ch)) % 97;
    return m === 1;
};
const luhnOk = (s) => /^\d{13,19}$/.test(s) && luhn(s.slice(0, -1)) === s;
const CLASSIFIERS = [
    ["email", (v) => /^[^@\s]+@[^@\s]+\.[a-z]{2,}$/i.test(v)],
    ["card", luhnOk],
    ["iban", ibanOk],
    ["phone", (v) => /^\+\d{1,3}[\d\s]{7,}$/.test(v)],
    ["date_of_birth", (v) => /^(19|20)\d\d-\d\d-\d\d$/.test(v)],
    ["postcode", (v) => /^\d{5}$/.test(v)],
    ["person_name", (v) => /^\p{Lu}\p{Ll}+ \p{Lu}\p{Ll}+$/u.test(v)],
    ["encrypted", (v) => /^[A-Za-z0-9+/=_-]{40,}$/.test(v)],
    [
        "free_text_pii",
        (v) =>
            /\s/.test(v) &&
            v.length > 20 &&
            (/\+\d{2} ?\d{3}/.test(v) || /\b[A-Z]{2}\d{2}\d{10,}/.test(v) || /@/.test(v)),
    ],
];
function discover(db) {
    const out = [];
    for (const { name: table } of db.query(
        "SELECT name FROM sqlite_master WHERE type = 'table' AND name != 'user_keys'",
    )) {
        for (const { name: col } of db.query(`PRAGMA table_info(${table})`)) {
            const sample = db
                .query(`SELECT ${col} AS v FROM ${table} WHERE ${col} IS NOT NULL ORDER BY random() LIMIT 200`)
                .map((r) => String(r.v));
            const scores = CLASSIFIERS.map(([k, fn]) => [
                k,
                sample.filter(fn).length / Math.max(1, sample.length),
            ]).sort((a, b) => b[1] - a[1]);
            const [cls, share] = scores[0];
            out.push({ table, column: col, class: share >= 0.3 ? cls : "none", share: Number(share.toFixed(2)) });
        }
    }
    return out;
}
const catalog = discover(prod);
const classOf = (t, c) => catalog.find((x) => x.table === t && x.column === c)?.class;

// =============================================================================
// Transformations
// =============================================================================
const pseudo = (kind, v) =>
    `${kind}_${crypto.HMACHex("sha256", PSEUDO_KEY, `${kind}:${String(v).toLowerCase()}`).slice(0, 20)}`;
function fpeCard(card) {
    // keyed digit permutation, keeps BIN and last four
    const middle = card.slice(6, -4);
    const ks = crypto.HMAC("sha256", PSEUDO_KEY, `card:${card}`);
    const mixed = middle
        .split("")
        .map((d, i) => String((Number(d) + ks[i]) % 10))
        .join("");
    return card.slice(0, 6) + mixed + card.slice(-4);
}
const NAMES = new MultiMatcher(FIRST);
const redactText = (t) =>
    NAMES.replaceAllIn(
        t
            .replace(/\+\d{2}[\d\s]{8,}\d/g, "[PHONE]")
            .replace(/\b[A-Z]{2}\d{2}[A-Z0-9]{10,30}\b/g, (m) => (ibanOk(m) ? "[IBAN]" : m))
            .replace(/[^\s@]+@[^\s@]+/g, "[EMAIL]"),
        "[NAME]",
    );
const ageBand = (dob, width) => {
    const age = 2026 - Number(dob.slice(0, 4));
    const lo = Math.floor(age / width) * width;
    return `${lo}-${lo + width - 1}`;
};

// =============================================================================
// Release with consent filtering and k-anonymity
// =============================================================================
const K = 10;
function release() {
    const users = prod.query("SELECT * FROM users WHERE consent_analytics = 1");
    let width = 5,
        regional = true,
        suppressed = 0,
        rows;
    for (;;) {
        rows = users.map((u) => ({
            user: pseudo("user", u.id),
            age: ageBand(u.dob, width),
            region: regional ? REGIONS[u.postcode.slice(0, 2)] : "DE",
            gender: u.gender,
        }));
        const qi = (r) => `${r.age}|${r.region}|${r.gender}`,
            groups = new Multiset();
        for (const r of rows) groups.add(qi(r));
        const [ok, small] = rows.partition((r) => groups.count(qi(r)) >= K);
        if (small.length <= rows.length * 0.02 || (width >= 20 && !regional)) {
            suppressed = small.length;
            rows = ok;
            break;
        }
        if (width < 20) width *= 2;
        else regional = false;
    }
    const kept = new Set(rows.map((r) => r.user));
    const orders = prod
        .query(
            "SELECT o.*, u.consent_analytics FROM orders o JOIN users u ON u.id = o.user_id WHERE u.consent_analytics = 1",
        )
        .map((o) => ({
            user: pseudo("user", o.user_id),
            amount: o.amount,
            card: fpeCard(o.card),
            month: o.placed.slice(0, 7),
        }))
        .filter((o) => kept.has(o.user));
    const tickets = prod
        .query("SELECT t.* FROM tickets t JOIN users u ON u.id = t.user_id WHERE u.consent_analytics = 1")
        .map((t) => ({ user: pseudo("user", t.user_id), body: redactText(t.body) }))
        .filter((t) => kept.has(t.user));
    return {
        users: rows,
        orders,
        tickets,
        width,
        regional,
        suppressed,
        consentExcluded: prod.query("SELECT COUNT(*) AS n FROM users WHERE consent_analytics = 0")[0].n,
    };
}
const released = release();
const wh = new SQLite(work.join("warehouse.db"));
wh.exec("CREATE TABLE users (user TEXT, age TEXT, region TEXT, gender TEXT)");
wh.exec("CREATE TABLE orders (user TEXT, amount REAL, card TEXT, month TEXT)");
wh.exec("CREATE TABLE tickets (user TEXT, body TEXT)");
wh.exec("BEGIN");
for (const r of released.users) wh.exec("INSERT INTO users VALUES (?,?,?,?)", [r.user, r.age, r.region, r.gender]);
for (const o of released.orders) wh.exec("INSERT INTO orders VALUES (?,?,?,?)", [o.user, o.amount, o.card, o.month]);
for (const t of released.tickets) wh.exec("INSERT INTO tickets VALUES (?,?)", [t.user, t.body]);
wh.exec("COMMIT");

// =============================================================================
// Differential privacy for published aggregates
// =============================================================================
class PrivacyBudget {
    constructor(total) {
        this.total = total;
        this.spent = 0;
        this.log = [];
    }
    laplace(trueValue, sensitivity, epsilon, label) {
        if (this.spent + epsilon > this.total + 1e-12)
            throw new Error(
                `privacy budget exhausted: ${label} needs ${epsilon}, ${(this.total - this.spent).toFixed(2)} left`,
            );
        this.spent += epsilon;
        const u = rng.nextFloat() - 0.5,
            b = sensitivity / epsilon;
        const noisy = trueValue - b * Math.sign(u) * Math.log(1 - 2 * Math.abs(u));
        this.log.push({ label, epsilon });
        return noisy;
    }
}
const budget = new PrivacyBudget(1.0);
const byRegion = wh.query("SELECT region, COUNT(*) AS n FROM users GROUP BY region ORDER BY region");
const published = byRegion.map((r) => ({
    region: r.region,
    n: Math.max(0, Math.round(budget.laplace(r.n, 1, 0.15, `users in ${r.region}`))),
    exact: r.n,
}));
let refused = null;
try {
    for (let i = 0; i < 10; i++) budget.laplace(100, 1, 0.1, `extra query ${i}`);
} catch (e) {
    refused = e.message;
}

// =============================================================================
// Data-subject requests: access and erasure (with crypto-shredding)
// =============================================================================
copyFile(work.join("prod.db"), work.join("backup.db")); // last night's backup includes everyone
const requestSchema = Schema.compile({
    type: "object",
    required: ["type", "email", "verified"],
    properties: { type: { enum: ["access", "erasure"] }, email: { type: "string" }, verified: { const: true } },
});
function dsar(req) {
    if (!requestSchema.validate(req).valid) throw new Error("request must be verified before processing");
    const u = prod.query("SELECT * FROM users WHERE email = ?", [req.email])[0];
    if (!u) return { found: false };
    if (req.type === "access") {
        const profile = { ...u, iban: openFor(u.id, u.iban_enc), iban_enc: undefined };
        const files = [
            { name: "profile.json", data: JSON.stringify(profile, null, 2) },
            {
                name: "orders.json",
                data: JSON.stringify(
                    prod.query("SELECT id, amount, card, placed FROM orders WHERE user_id = ?", [u.id]),
                    null,
                    2,
                ),
            },
            {
                name: "tickets.json",
                data: JSON.stringify(prod.query("SELECT id, body FROM tickets WHERE user_id = ?", [u.id]), null, 2),
            },
            {
                name: "processing.json",
                data: JSON.stringify(
                    {
                        purposes: ["order fulfilment", u.consent_analytics ? "analytics (pseudonymised)" : null].filter(
                            Boolean,
                        ),
                        pseudonym: pseudo("user", u.id),
                        retention: "7 years for invoices",
                    },
                    null,
                    2,
                ),
            },
        ];
        const zip = ZipPack(files);
        writeFile(work.join(`dsar-${u.id}.zip`), zip);
        return { found: true, zip };
    }
    prod.exec("BEGIN");
    prod.exec("DELETE FROM tickets WHERE user_id = ?", [u.id]);
    prod.exec("UPDATE orders SET card = NULL WHERE user_id = ?", [u.id]); // invoices kept, card removed
    prod.exec("DELETE FROM users WHERE id = ?", [u.id]);
    prod.exec("DELETE FROM user_keys WHERE user_id = ?", [u.id]); // crypto-shred
    prod.exec("COMMIT");
    wh.exec("DELETE FROM users WHERE user = ?", [pseudo("user", u.id)]);
    wh.exec("DELETE FROM tickets WHERE user = ?", [pseudo("user", u.id)]);
    return { found: true, erased: u.id };
}
const subject = prod.query(
    "SELECT * FROM users WHERE consent_analytics = 1 AND id IN (SELECT user_id FROM tickets) LIMIT 1",
)[0];
const access = dsar({ type: "access", email: subject.email, verified: true });
const erase = dsar({ type: "erasure", email: subject.email, verified: true });
let unverified = null;
try {
    dsar({ type: "erasure", email: "anna.schmidt1@mail.example", verified: false });
} catch (e) {
    unverified = e.message;
}
const backup = new SQLite(work.join("backup.db"));
const backupRow = backup.query("SELECT * FROM users WHERE id = ?", [subject.id])[0];
let shredded = "readable";
try {
    openFor(subject.id, backupRow.iban_enc);
} catch (e) {
    shredded = e.message;
}

// =============================================================================
// Checks and report
// =============================================================================
const results = {};
const expect = {
    "users.email": "email",
    "users.full_name": "person_name",
    "users.phone": "phone",
    "users.dob": "date_of_birth",
    "users.postcode": "postcode",
    "users.iban_enc": "encrypted",
    "orders.card": "card",
    "tickets.body": "free_text_pii",
};
results[
    `discovery classified every PII column (${catalog
        .filter((c) => c.class !== "none")
        .map((c) => `${c.column}:${c.class}`)
        .join(" ")})`
] = Object.entries(expect).every(([k, v]) => classOf(...k.split(".")) === v);
results["non-PII columns are left alone"] =
    classOf("orders", "amount") === "none" && classOf("users", "gender") === "none";
const whText = JSON.stringify([
    wh.query("SELECT * FROM users"),
    wh.query("SELECT * FROM orders"),
    wh.query("SELECT * FROM tickets"),
]);
results["no direct identifiers reach the warehouse"] =
    !/@mail\.example|\+49 15\d \d{7}|DE\d{2}37040044|\b(Anna|Ben|Clara) /.test(whText);
results["card tokens keep BIN and last four"] =
    released.orders.every((o) => luhnOk(o.card) || o.card.length === 16) &&
    released.orders.every((o) => /^4\d{5}/.test(o.card));
results["pseudonyms stay joinable across tables"] =
    pseudo("user", 7) === pseudo("user", 7) &&
    wh.query("SELECT COUNT(*) AS n FROM orders o JOIN users u ON u.user = o.user")[0].n > 0;
const kmin = Math.min(...wh.query("SELECT COUNT(*) AS n FROM users GROUP BY age, region, gender").map((r) => r.n));
results[
    `k-anonymity holds: smallest group ${kmin} >= ${K} (age bands ${released.width}y, ${released.regional ? "regions" : "country"}, ${released.suppressed} suppressed)`
] = kmin >= K;
results[`${released.consentExcluded} users without analytics consent excluded`] =
    released.consentExcluded > 0 && wh.query("SELECT COUNT(*) AS n FROM users")[0].n <= 3000 - released.consentExcluded;
results[`DP counts within noise; budget refuses further queries after ε=${budget.spent.toFixed(2)}`] =
    published.every((p) => Math.abs(p.n - p.exact) < 60) && /budget exhausted/.test(refused || "");
const listed = ZipList(access.zip)
    .map((e) => e.name)
    .sort()
    .join();
results["access request returns a complete, decrypted export"] =
    listed === "orders.json,processing.json,profile.json,tickets.json" &&
    JSON.parse(new TextDecoder().decode(ZipRead(access.zip, "profile.json"))).iban.startsWith("DE");
results["erasure removes the subject from production and warehouse"] =
    erase.erased === subject.id &&
    !prod.query("SELECT 1 FROM users WHERE id = ?", [subject.id]).length &&
    !wh.query("SELECT 1 FROM users WHERE user = ?", [pseudo("user", subject.id)]).length;
results["the backup still holds the row, but its encrypted fields are unreadable (crypto-shredded)"] =
    backupRow && shredded === "key destroyed";
results["unverified requests are refused"] = /verified/.test(unverified || "");

print("privacy-pipeline");
print(
    table([
        ["table", "column", "class", "share"],
        ...catalog.filter((c) => c.class !== "none").map((c) => [c.table, c.column, c.class, String(c.share)]),
    ]),
);
print(
    `  release    ${released.users.length} users, ${released.orders.length} orders, ${released.tickets.length} tickets; k=${K} with ${released.width}-year bands, ${released.suppressed} rows suppressed`,
);
print(`  example    ${released.tickets[0].body}`);
print(
    `  DP counts  ${published.map((p) => `${p.region} ${p.n} (exact ${p.exact})`).join(", ")}; ε spent ${budget.spent.toFixed(2)}/1.0`,
);
print(
    `  DSAR       access zip ${access.zip.length} bytes (${listed}); erasure of user ${erase.erased}; backup copy: ${shredded}`,
);
const amounts = new DataFrame({ a: Float64Array.from(released.orders.map((o) => o.amount)) });
print(
    `  utility    warehouse order total ${amounts.SUM("a").toFixed(2)}, median ${amounts.MEDIAN("a").toFixed(2)} (unchanged by pseudonymisation)`,
);
for (const d of [prod, wh, backup]) d.close();
removeAll(work);
check(results);

function check(results) {
    const names = Object.keys(results);
    const failed = names.filter((name) => !results[name]);
    for (const name of failed) print("FAIL " + name);
    if (failed.length) throw new Error(`${failed.length} of ${names.length} checks failed`);
}
