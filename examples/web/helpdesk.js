// Helpdesk — email-to-ticket intake, ML triage, business-hours SLAs, round-robin assignment, and service reports.
//
// WHAT IT IS
//   A support desk backend:
//
//   intake       raw RFC 5322 messages (with MIME multipart bodies and base64 / quoted-printable parts) become
//                tickets; replies thread onto their ticket by the [#ID] tag in the subject; attachments are
//                stored by content hash
//   triage       a text classifier (hashed bag-of-words -> sparse CSR -> multinomial logistic regression)
//                trained on past tickets picks the queue; low-confidence predictions go to a human queue
//   priority     keyword rules (Aho-Corasick) raise priority for outages and security reports
//   SLAs         first-response deadlines counted in BUSINESS hours (Mon-Fri 09:00-17:00, holidays from an
//                RFC 5545 rule), breaches detected on a sweep. The year's open hours are built once as a RangeSet;
//                their cumulative open minutes are one dyna:simd cumsum, so "start + N business hours" is a binary
//                search over the prefix sums instead of a walk in 15-minute steps that asks the RRule every step
//   assignment   round-robin per queue among agents who are on shift
//   API          an App for agents: list my queue, reply, resolve; satisfaction survey links signed with HMAC
//   reporting    a DataFrame over the ticket history: volume by queue, median resolution hours, SLA attainment
//
// MODULES  dyna:http (App) · dyna:net (SQLite) · dyna:ml (CSR, LogisticRegression) · dyna:hash
//          dyna:matcher · dyna:encoding · dyna:time · dyna:dataframe · dyna:crypto · dyna:random · dyna:file
//          · dyna:structures (RangeSet) · dyna:simd (cumsum)
//
// RUN      dynajs examples/web/helpdesk.js       trains the triage model, ingests a day of email, sweeps SLAs, works
//          tickets through the agent API, and checks the outcome

import { App } from "dyna:http";
import { SQLite } from "dyna:net";
import { CSR, LogisticRegression } from "dyna:ml";
import { XXHash32, SHA256Hex } from "dyna:hash";
import { MultiMatcher } from "dyna:matcher";
import { Base64Decode } from "dyna:encoding";
import { RRule, PlainDateTime, Duration } from "dyna:time";
import { DataFrame } from "dyna:dataframe";
import { HMACHex } from "dyna:crypto";
import { Random } from "dyna:random";
import { makeTempDir, makeDir, writeFile, removeAll } from "dyna:file";
import { RangeSet } from "dyna:structures";
import { cumsum } from "dyna:simd";

const work = makeTempDir("helpdesk");
const db = new SQLite(work.join("helpdesk.db"));
db.exec(`CREATE TABLE tickets (id INTEGER PRIMARY KEY, subject TEXT, requester TEXT, queue TEXT, confidence REAL, priority TEXT,
         status TEXT, agent TEXT, created INTEGER, first_response_due INTEGER, first_response INTEGER, resolved INTEGER)`);
db.exec(`CREATE TABLE messages (ticket INTEGER, sender TEXT, body TEXT, at INTEGER)`);
db.exec(`CREATE TABLE attachments (ticket INTEGER, name TEXT, sha256 TEXT, size INTEGER)`);
const rng = new Random(31);

// =============================================================================
// Email parsing: headers, MIME multipart, transfer encodings
// =============================================================================
function mimeParts(contentType, body) {
    const boundary = /boundary="?([^";]+)"?/i.exec(contentType)?.[1];
    if (!boundary) throw new Error("multipart message without a boundary");
    const parts = [];
    for (const chunk of body.split(`--${boundary}`).slice(1)) {
        if (chunk.startsWith("--")) break;
        const raw = chunk.replace(/^\r\n/, "").replace(/\r\n$/, "");
        const head = raw.slice(0, Math.max(raw.indexOf("\r\n\r\n"), 0));
        const filename = /filename="?([^";\r\n]+)"?/i.exec(head)?.[1];
        parts.push(filename ? { raw, filename } : { raw });
    }
    return parts;
}
function parseEmail(raw) {
    const split = raw.indexOf("\r\n\r\n");
    const headerText = raw.slice(0, split).replace(/\r\n[ \t]+/g, " ");
    const headers = {};
    for (const line of headerText.split("\r\n")) {
        const i = line.indexOf(":");
        headers[line.slice(0, i).toLowerCase()] = line.slice(i + 1).trim();
    }
    const body = raw.slice(split + 4);
    const decode = (text, encoding) => {
        if (/base64/i.test(encoding || "")) return Base64Decode(text.replace(/\s+/g, ""));
        if (/quoted-printable/i.test(encoding || "")) {
            const out = [];
            for (const piece of text.replace(/=\r\n/g, "").split(/(=[0-9A-F]{2})/)) {
                if (/^=[0-9A-F]{2}$/.test(piece)) out.push(parseInt(piece.slice(1), 16));
                else out.push(...new TextEncoder().encode(piece));
            }
            return new Uint8Array(out);
        }
        return new TextEncoder().encode(text);
    };
    let text = "",
        attachments = [];
    if (/^multipart\//i.test(headers["content-type"] || "")) {
        for (const part of mimeParts(headers["content-type"], body)) {
            const raw2 = part.raw;
            const s = raw2.indexOf("\r\n\r\n");
            const ph = {};
            for (const line of raw2.slice(0, Math.max(s, 0)).split("\r\n")) {
                const i = line.indexOf(":");
                if (i > 0) ph[line.slice(0, i).toLowerCase()] = line.slice(i + 1).trim();
            }
            const bytes = decode(s >= 0 ? raw2.slice(s + 4) : raw2, ph["content-transfer-encoding"]);
            if (part.filename) attachments.push({ name: part.filename, bytes });
            else text += new TextDecoder().decode(bytes);
        }
    } else text = new TextDecoder().decode(decode(body, headers["content-transfer-encoding"]));
    const from = /<([^>]+)>/.exec(headers.from || "")?.[1] || headers.from;
    return {
        from,
        subject: headers.subject || "(no subject)",
        text: text.trim(),
        attachments,
        date: Date.parse(headers.date) || Date.now(),
    };
}

// =============================================================================
// Triage model: feature hashing into a sparse matrix
// =============================================================================
const QUEUES = ["billing", "technical", "account"];
const DIM = 1 << 12;
const featurize = (text) => {
    const counts = new Map();
    for (const w of text.toLowerCase().match(/[a-z]{3,}/g) || []) {
        const h = XXHash32(w, 7) % DIM;
        counts.set(h, (counts.get(h) || 0) + 1);
    }
    return counts;
};
function toCSR(texts) {
    const values = [],
        columns = [],
        rowPointers = [0];
    for (const t of texts) {
        const f = [...featurize(t)].sort((a, b) => a[0] - b[0]);
        const norm = Math.sqrt(f.reduce((s, [, c]) => s + c * c, 0)) || 1;
        for (const [col, c] of f) {
            columns.push(col);
            values.push(c / norm);
        }
        rowPointers.push(values.length);
    }
    return new CSR(values, columns, rowPointers, DIM);
}
const VOCAB = {
    billing: [
        "invoice",
        "charged",
        "refund",
        "payment",
        "card",
        "billing",
        "receipt",
        "subscription",
        "price",
        "twice",
    ],
    technical: ["error", "crash", "timeout", "server", "api", "bug", "outage", "slow", "deploy", "exception"],
    account: ["password", "login", "email", "username", "locked", "access", "profile", "delete", "sso", "invite"],
};
const filler = ["please", "help", "hello", "thanks", "team", "today", "issue", "urgent", "again", "since"];
const sampleText = (q) =>
    Array.from({ length: 10 }, () => (rng.nextFloat() < 0.6 ? rng.choice(VOCAB[q]) : rng.choice(filler))).join(" ");
const trainTexts = [],
    trainY = [];
for (let i = 0; i < 600; i++) {
    const q = i % 3;
    trainTexts.push(sampleText(QUEUES[q]));
    trainY.push(q);
}
const model = new LogisticRegression({ maxIter: 600, learningRate: 0.5 });
await model.fit(toCSR(trainTexts), trainY);

// =============================================================================
// Priority rules, business-hours SLA clock
// =============================================================================
const urgent = new MultiMatcher([" outage", " down ", " security ", " breach", " data loss ", " cannot login "]);
const words = (text) => ` ${text.toLowerCase().replace(/[^a-z0-9]+/g, " ")} `;
const holidays = new RRule({ freq: "YEARLY", bymonth: [12], bymonthday: [25], dtstart: "2026-01-01T00:00:00Z" });
const isHoliday = (d) =>
    holidays.between(
        new Date(Date.UTC(d.year, d.month - 1, d.day)),
        new Date(Date.UTC(d.year, d.month - 1, d.day, 23, 59)),
        true,
    ).length > 0;
function addBusinessHoursWalk(startMs, hours) {
    // the definition, kept as the test oracle
    let t = Math.ceil(startMs / 900_000) * 900_000,
        left = hours * 4;
    while (left > 0) {
        const d = new Date(t),
            dow = d.getUTCDay(),
            h = d.getUTCHours();
        const open =
            dow >= 1 &&
            dow <= 5 &&
            h >= 9 &&
            h < 17 &&
            !isHoliday({ year: d.getUTCFullYear(), month: d.getUTCMonth() + 1, day: d.getUTCDate() });
        if (open) left--;
        t += 900_000;
    }
    return t;
}
// Open hours of 2026 in minutes since the epoch: one range per business day, holidays removed.
const MIN = 60_000,
    openHours = new RangeSet();
for (let day = Date.UTC(2026, 0, 1); day < Date.UTC(2027, 0, 1); day += 86_400_000) {
    const d = new Date(day),
        dow = d.getUTCDay();
    if (dow >= 1 && dow <= 5) openHours.add(day / MIN + 9 * 60, day / MIN + 17 * 60);
}
for (const h of holidays.between(new Date(Date.UTC(2026, 0, 1)), new Date(Date.UTC(2027, 0, 1)), true))
    openHours.remove(h.getTime() / MIN, h.getTime() / MIN + 1440);
const windows = openHours.ranges(),
    starts = Float64Array.from(windows, ([lo]) => lo);
const openBefore = cumsum(Int32Array.from(windows, ([lo, hi]) => hi - lo)); // open minutes up to the end of window i
function addBusinessHours(startMs, hours) {
    const t = (Math.ceil(startMs / 900_000) * 900_000) / MIN;
    let lo = 0,
        hi = windows.length; // first window ending after t
    while (lo < hi) {
        const m = (lo + hi) >> 1;
        if (windows[m][1] <= t) lo = m + 1;
        else hi = m;
    }
    const elapsed = (lo ? openBefore[lo - 1] : 0) + Math.max(0, t - starts[lo]);
    const target = elapsed + hours * 60;
    let a = lo,
        b = windows.length - 1; // the window in which the target minute falls
    while (a < b) {
        const m = (a + b) >> 1;
        if (openBefore[m] < target) a = m + 1;
        else b = m;
    }
    return (windows[a][1] - (openBefore[a] - target)) * MIN;
}
const SLA_HOURS = { urgent: 1, normal: 8 };

// =============================================================================
// Intake and assignment
// =============================================================================
const agents = { billing: ["ana", "bo"], technical: ["chen", "dev", "eli"], account: ["fay"], triage: ["gus"] };
const rr = new Map();
const nextAgent = (queue) => {
    const list = agents[queue];
    const i = (rr.get(queue) || 0) % list.length;
    rr.set(queue, i + 1);
    return list[i];
};

async function ingest(raw) {
    const m = parseEmail(raw);
    const thread = /\[#(\d+)\]/.exec(m.subject);
    if (thread && db.query("SELECT 1 AS x FROM tickets WHERE id = ?", [Number(thread[1])]).length) {
        db.exec("INSERT INTO messages VALUES (?, ?, ?, ?)", [Number(thread[1]), m.from, m.text, m.date]);
        db.exec("UPDATE tickets SET status = 'open' WHERE id = ? AND status = 'pending'", [Number(thread[1])]);
        return { threaded: Number(thread[1]) };
    }
    const proba = (await model.predictProba(toCSR([`${m.subject} ${m.text}`])))[0];
    const best = proba.indexOf(Math.max(...proba));
    const queue = proba[best] >= 0.55 ? QUEUES[best] : "triage";
    const priority = urgent.test(words(`${m.subject} ${m.text}`)) ? "urgent" : "normal";
    db.exec(
        `INSERT INTO tickets (subject, requester, queue, confidence, priority, status, agent, created, first_response_due)
             VALUES (?, ?, ?, ?, ?, 'open', ?, ?, ?)`,
        [
            m.subject,
            m.from,
            queue,
            proba[best],
            priority,
            nextAgent(queue),
            m.date,
            addBusinessHours(m.date, SLA_HOURS[priority]),
        ],
    );
    const id = db.lastInsertRowId;
    db.exec("INSERT INTO messages VALUES (?, ?, ?, ?)", [id, m.from, m.text, m.date]);
    for (const a of m.attachments) {
        const sha = SHA256Hex(a.bytes);
        makeDir(work.join("files"), { recursive: true });
        writeFile(work.join("files", sha), a.bytes);
        db.exec("INSERT INTO attachments VALUES (?, ?, ?, ?)", [id, a.name, sha, a.bytes.length]);
    }
    return { id, queue, priority };
}
function sweepSLA(nowMs) {
    return db.query(
        "SELECT id, agent, priority FROM tickets WHERE first_response IS NULL AND first_response_due < ? AND status != 'resolved'",
        [nowMs],
    );
}

// =============================================================================
// Agent API
// =============================================================================
const surveyKey = "survey-signing-key";
const app = new App({ port: 0 });
let clock = Date.UTC(2026, 2, 2, 8, 30); // Monday 08:30 UTC
app.get("/agents/:name/queue", (req) =>
    db.query(
        "SELECT id, subject, priority, first_response_due FROM tickets WHERE agent = ? AND status != 'resolved' ORDER BY priority = 'urgent' DESC, first_response_due",
        [req.params.name],
    ),
);
app.post("/tickets/:id/reply", (req) => {
    const id = Number(req.params.id),
        b = JSON.parse(req.body);
    db.exec("INSERT INTO messages VALUES (?, ?, ?, ?)", [id, b.agent, b.text, clock]);
    db.exec("UPDATE tickets SET first_response = COALESCE(first_response, ?), status = 'pending' WHERE id = ?", [
        clock,
        id,
    ]);
    return { ok: true };
});
app.post("/tickets/:id/resolve", (req) => {
    const id = Number(req.params.id);
    db.exec("UPDATE tickets SET status = 'resolved', resolved = ? WHERE id = ?", [clock, id]);
    return { survey: `/survey/${id}/${HMACHex("sha256", surveyKey, String(id)).slice(0, 16)}` };
});
app.get("/survey/:id/:sig", (req) =>
    req.params.sig === HMACHex("sha256", surveyKey, req.params.id).slice(0, 16)
        ? { thanks: true }
        : { status: 403, body: "invalid survey link" },
);
app.start();

// =============================================================================
// A day at the desk
// =============================================================================
const results = {};
const email = (from, subject, body, at, extra = "") =>
    `From: Customer <${from}>\r\nTo: support@example.test\r\nSubject: ${subject}\r\nDate: ${new Date(at).toUTCString()}\r\n${extra}\r\n${body}`;
const day = Date.UTC(2026, 2, 2);
const inbox = [];
for (let i = 0; i < 30; i++) {
    const q = QUEUES[i % 3];
    inbox.push(
        email(
            `user${i}@corp.example`,
            `Question ${i}`,
            sampleText(q) + (i % 10 === 0 ? " the api is down outage now" : ""),
            day + (7 + i * 0.3) * 3600_000,
        ),
    );
}
const boundary = "XYZ123";
inbox.push(
    email(
        "finance@corp.example",
        "Charged twice on invoice INV-77",
        `--${boundary}\r\nContent-Type: text/plain\r\nContent-Transfer-Encoding: quoted-printable\r\n\r\nWe were charged twice for the subscription =E2=80=93 please refund the duplicate payment.\r\n--${boundary}\r\nContent-Type: application/pdf; name="inv-77.pdf"\r\nContent-Disposition: attachment; name="file"; filename="inv-77.pdf"\r\nContent-Transfer-Encoding: base64\r\n\r\nJVBERi0xLjcKJSVFT0YK\r\n--${boundary}--\r\n`,
        day + 10 * 3600_000,
        `MIME-Version: 1.0\r\nContent-Type: multipart/mixed; boundary="${boundary}"\r\n`,
    ),
);
inbox.push(email("x@corp.example", "Misc", "hello hello thanks team", day + 11 * 3600_000));
const tickets = [];
for (const raw of inbox) tickets.push(await ingest(raw));
const finance = tickets[30];
results[`the multipart billing email is routed to billing (${JSON.stringify(finance)})`] = finance.queue === "billing";
results["attachment extracted and stored by hash"] =
    db.query("SELECT name, size FROM attachments")[0]?.name === "inv-77.pdf";
results["quoted-printable decoded (UTF-8 en dash)"] = db
    .query("SELECT body FROM messages WHERE ticket = ?", [finance.id])[0]
    .body.includes("subscription – please");
results["a content-free email goes to the human triage queue"] = tickets[31].queue === "triage";
const correct = tickets.slice(0, 30).filter((r, i) => r.queue === QUEUES[i % 3]).length;
results[`classifier routes ${correct}/30 sample tickets correctly`] = correct >= 25;
results["outage keywords raise priority"] =
    [0, 10, 20].every((i) => tickets[i].priority === "urgent") && tickets[31].priority === "normal";
const t0 = db.query("SELECT created, first_response_due FROM tickets WHERE id = 1")[0];
results[
    `an urgent ticket at 07:00 Monday is due 10:00 (1 business hour after opening): ${new Date(t0.first_response_due).toISOString()}`
] = new Date(t0.first_response_due).toISOString() === "2026-03-02T10:00:00.000Z";

// A follow-up email threads onto its ticket instead of opening a new one.
const before = db.query("SELECT COUNT(*) AS n FROM tickets")[0].n;
await ingest(email("finance@corp.example", `Re: [#${finance.id}] Charged twice`, "Any update?", day + 12 * 3600_000));
results["replies thread by [#ID]"] =
    db.query("SELECT COUNT(*) AS n FROM tickets")[0].n === before &&
    db.query("SELECT COUNT(*) AS n FROM messages WHERE ticket = ?", [finance.id])[0].n === 2;

// Agents work their queues during the day; some urgent tickets breach.
const base = `http://127.0.0.1:${app.port}`;
for (const agent of ["ana", "bo", "chen", "dev", "eli", "fay", "gus"]) {
    const queue = await (await fetch(`${base}/agents/${agent}/queue`)).json();
    for (const [i, t] of queue.entries()) {
        clock = day + (9.5 + i * 0.75) * 3600_000;
        await (
            await fetch(`${base}/tickets/${t.id}/reply`, {
                method: "POST",
                body: JSON.stringify({ agent, text: "Looking into it." }),
            })
        ).text();
        if (i % 2 === 0) {
            clock += (2 + rng.nextBounded(20)) * 3600_000;
            const r = await (await fetch(`${base}/tickets/${t.id}/resolve`, { method: "POST" })).json();
            if (t.id === finance.id)
                results["signed survey links"] =
                    (await fetch(base + r.survey)).status === 200 &&
                    (await fetch(base + r.survey.slice(0, -2) + "00")).status === 403;
        }
    }
}
const breaches = db.query("SELECT COUNT(*) AS n FROM tickets WHERE first_response > first_response_due")[0].n;
results["the prefix-sum business clock equals the 15-minute walk on 200 random starts"] = Array.from(
    { length: 200 },
    (_, k) => Date.UTC(2026, 0, 5) + k * 155_000_017,
).every(
    (t) =>
        addBusinessHours(t, 1) === addBusinessHoursWalk(t, 1) && addBusinessHours(t, 8) === addBusinessHoursWalk(t, 8),
);
results["every ticket got a first response"] = sweepSLA(day + 30 * 3600_000).length === 0;

// Reporting.
const rows = db.query("SELECT queue, priority, created, resolved, first_response, first_response_due FROM tickets");
const df = new DataFrame({
    queue: rows.map((r) => r.queue),
    hours: Float64Array.from(rows, (r) => (r.resolved ? (r.resolved - r.created) / 3600_000 : NaN)),
    met: Float64Array.from(rows, (r) => (r.first_response <= r.first_response_due ? 1 : 0)),
});
const volume = df.GROUP_BY_COUNT("queue"),
    attainment = df.GROUP_BY_MEAN("queue", "met");
print("helpdesk");
print(`  intake      ${rows.length} tickets from ${inbox.length + 1} emails; triage accuracy ${correct}/30`);
print(`  volume      ${volume.keys.map((k, i) => `${k} ${volume.values[i]}`).join(", ")}`);
print(
    `  SLA met     ${attainment.keys.map((k, i) => `${k} ${(attainment.values[i] * 100).toFixed(0)}%`).join(", ")} (${breaches} breaches)`,
);
print(
    `  resolution  median ${df.MEDIAN("hours", df.NOT_NA("hours")).toFixed(1)} h over ${df.COUNT("hours", df.NOT_NA("hours"))} resolved tickets`,
);
app.close();
model.close();
db.close();
removeAll(work);
check(results);

function check(results) {
    const names = Object.keys(results);
    const failed = names.filter((name) => !results[name]);
    for (const name of failed) print("FAIL " + name);
    if (failed.length) throw new Error(`${failed.length} of ${names.length} checks failed`);
}
