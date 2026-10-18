// Document management — typed uploads, a deduplicated content store, versions, search, signed links, retention, export.
//
// WHAT IT IS
//   The core of a document system (think a team drive), as one service:
//
//   intake      uploads stream to disk through App.upload (size cap, declared-type allow-list); the content is
//               then checked against its magic bytes, so an executable labelled image/png goes to quarantine
//   storage     content-addressed by BLAKE3: identical bytes are stored once, however many documents use them
//   documents   a name and folder per document, each upload claimed as a new version (SQLite)
//   search      text documents are indexed on arrival in a native BM25 InvertedIndex (a new version replaces the
//               old document in place); results carry highlighted snippets; near-identical text versions are
//               flagged by DiffWords so a reviewer sees what actually changed between versions
//   sharing     share links are HMAC-signed (document, version, expiry); a tampered or expired link is refused
//   retention   an RFC 5545 rule (weekly on Sunday) schedules purges that keep the newest N versions and
//               drop content no version references any more
//   export      a folder exports as a zip with a manifest of SHA-256 digests, verified by the client
//
// MODULES  dyna:http · dyna:net (SQLite) · dyna:file · dyna:hash · dyna:crypto · dyna:compress · dyna:time (RRule)
//          dyna:matcher (Matcher, DiffWords) · dyna:encoding · dyna:structures (Multiset, InvertedIndex) · dyna:log · dyna:cli
//
// RUN      dynajs examples/web/document-management.js --port 8080 --root ./drive     serve for real
//          dynajs examples/web/document-management.js                                self-test

import { App } from "dyna:http";
import { SQLite } from "dyna:net";
import { Path, makeTempDir, makeDir, readBytes, readDir, sniffType, move, remove, removeAll, exists } from "dyna:file";
import { BLAKE3Hex, SHA256Hex } from "dyna:hash";
import { Hmac, RandomBytes, TimingSafeEqual } from "dyna:crypto";
import { ZipPack, ZipList, ZipRead } from "dyna:compress";
import { RRule } from "dyna:time";
import { Matcher, DiffWords } from "dyna:matcher";
import { Base64URLEncode, Base64URLDecode } from "dyna:encoding";
import { Multiset, InvertedIndex } from "dyna:structures";
import { Logger } from "dyna:log";
import { Command } from "dyna:cli";

const { options } = new Command("document-management")
    .option("-p, --port <n>", "port", { type: "number" })
    .option("--root <dir>", "storage root", { type: "string" })
    .option("--keep <n>", "versions kept per document by retention", { type: "number", default: 3 })
    .parse(scriptArgs.slice(1));
const selfTest = options.port === undefined;
const root = selfTest ? makeTempDir("drive") : new Path(options.root || "drive");
const dirs = { incoming: root.join("incoming"), blobs: root.join("blobs"), quarantine: root.join("quarantine") };
for (const d of Object.values(dirs)) makeDir(d, { recursive: true });
const auditLines = [];
const audit = new Logger({
    name: "dms",
    timestamp: false,
    dest: selfTest ? (l) => auditLines.push(JSON.parse(l)) : undefined,
});

// =============================================================================
// Storage: content-addressed blobs + document/version metadata
// =============================================================================
const db = new SQLite(root.join("drive.db"));
db.exec(
    `CREATE TABLE IF NOT EXISTS documents (id INTEGER PRIMARY KEY, folder TEXT NOT NULL, name TEXT NOT NULL, UNIQUE (folder, name))`,
);
db.exec(`CREATE TABLE IF NOT EXISTS versions (doc INTEGER NOT NULL, version INTEGER NOT NULL, blob TEXT NOT NULL, type TEXT NOT NULL,
         size INTEGER NOT NULL, at INTEGER NOT NULL, PRIMARY KEY (doc, version))`);
const blobPath = (digest) => dirs.blobs.join(digest.slice(0, 2), digest);
function putBlob(tmp) {
    const digest = BLAKE3Hex(readBytes(tmp));
    const dest = blobPath(digest);
    if (exists(dest)) {
        remove(tmp);
        return { digest, dedup: true };
    }
    makeDir(dest.dirname, { recursive: true });
    move(tmp, dest);
    return { digest, dedup: false };
}

// =============================================================================
// Intake: verify, quarantine, hold until claimed
// =============================================================================
const ALLOWED = ["application/pdf", "image/png", "image/jpeg", "text/plain", "text/markdown", "text/csv"];
const pending = new Map(); // upload id -> { digest, type, size }
const rejected = [];
const app = new App({ port: options.port || 0 });
app.upload("/upload", { dir: dirs.incoming, maxFileSize: 20 * 1024 * 1024, allow: ALLOWED }, (saved, meta) => {
    const tmp = new Path(saved),
        declared = meta.contentType.split(";")[0].trim();
    const sniffed = sniffType(tmp);
    const consistent = declared === sniffed || (declared.startsWith("text/") && sniffed.startsWith("text/"));
    if (!consistent) {
        move(tmp, dirs.quarantine.join(tmp.basename));
        rejected.push({ id: tmp.basename, declared, sniffed });
        audit.warn({ event: "upload.quarantined", declared, sniffed }, "content does not match its declared type");
        return;
    }
    const { digest, dedup } = putBlob(tmp);
    // App.upload answers the client with { ok, id }: the saved file's basename, which the client later claims.
    pending.set(tmp.basename, { digest, type: declared, size: meta.size });
    audit.info({ event: "upload.accepted", digest, dedup }, "stored");
});

// =============================================================================
// Search: inverted index over text versions, with snippets
// =============================================================================
const index = new InvertedIndex(); // latest version of each text document
const docText = new Map(),
    changes = new Map(); // doc -> text; doc -> word diff summary vs the previous version
const tokenize = (s) => s.toLowerCase().match(/[a-z0-9]{2,}/g) || [];
function indexDoc(doc, text) {
    const before = docText.get(doc);
    if (before !== undefined) {
        const hunks = DiffWords(before, text);
        changes.set(doc, {
            added: hunks
                .filter((h) => h.op === 1)
                .map((h) => h.text.trim())
                .filter(Boolean),
            removed: hunks
                .filter((h) => h.op === -1)
                .map((h) => h.text.trim())
                .filter(Boolean),
        });
    }
    docText.set(doc, text);
    index.remove(doc);
    index.add(doc, tokenize(text).join(" "));
}
function snippet(text, term) {
    const at = new Matcher(term).firstIn(text.toLowerCase());
    if (at < 0) return "";
    const s = Math.max(0, at - 30),
        e = Math.min(text.length, at + term.length + 30);
    return (
        (s ? "…" : "") +
        text.slice(s, at) +
        "[" +
        text.slice(at, at + term.length) +
        "]" +
        text.slice(at + term.length, e) +
        (e < text.length ? "…" : "")
    );
}
function search(q) {
    const terms = tokenize(q);
    return index.search(terms.join(" "), { mode: "or", limit: 50 }).map(([doc]) => {
        const d = db.query("SELECT * FROM documents WHERE id = ?", [doc])[0];
        return { id: doc, path: `${d.folder}/${d.name}`, snippet: snippet(docText.get(doc), terms[0]) };
    });
}

// =============================================================================
// Signed share links
// =============================================================================
const linkKey = new Hmac("sha256", RandomBytes(32));
function shareLink(doc, version, ttlSec, nowSec = Math.floor(Date.now() / 1000)) {
    const payload = `${doc}.${version}.${nowSec + ttlSec}`;
    return Base64URLEncode(payload) + "." + Base64URLEncode(linkKey.sign(payload));
}
function openLink(token, nowSec = Math.floor(Date.now() / 1000)) {
    const [p, sig] = token.split(".");
    let payload;
    try {
        payload = new TextDecoder().decode(Base64URLDecode(p));
        if (!TimingSafeEqual(Base64URLDecode(sig), linkKey.sign(payload))) return { error: "bad signature" };
    } catch {
        return { error: "malformed link" };
    }
    const [doc, version, exp] = payload.split(".").map(Number);
    return exp < nowSec ? { error: "expired" } : { doc, version };
}

// =============================================================================
// Retention
// =============================================================================
function purge(keep) {
    const removedVersions = db.exec(
        `DELETE FROM versions WHERE (doc, version) IN (
        SELECT doc, version FROM (SELECT doc, version, ROW_NUMBER() OVER (PARTITION BY doc ORDER BY version DESC) AS rn FROM versions) WHERE rn > ?)`,
        [keep],
    );
    const referenced = new Multiset();
    for (const r of db.query("SELECT blob FROM versions")) referenced.add(r.blob);
    let removedBlobs = 0;
    for (const shard of readDir(dirs.blobs).filter((e) => e.isDir)) {
        const dir = dirs.blobs.join(shard.name);
        for (const f of readDir(dir))
            if (f.isFile && !referenced.has(f.name)) {
                remove(dir.join(f.name));
                removedBlobs++;
            }
    }
    audit.info({ event: "retention.purged", removedVersions, removedBlobs }, "retention run");
    return { removedVersions, removedBlobs };
}
const retention = new RRule({ freq: "WEEKLY", byweekday: ["SU"], dtstart: "2026-01-04T03:00:00Z" });

// =============================================================================
// HTTP surface
// =============================================================================
const json = (status, body, headers) => ({
    status,
    body: JSON.stringify(body),
    contentType: "application/json",
    headers,
});
app.post("/documents", (req) => {
    const b = JSON.parse(req.body || "{}");
    const up = pending.get(b.upload);
    if (!up) return json(404, { error: "unknown or already claimed upload" });
    if (!/^[\w .-]{1,120}$/.test(b.name || "") || !/^[\w/-]{1,200}$/.test(b.folder || ""))
        return json(400, { error: "bad name or folder" });
    pending.delete(b.upload);
    db.exec("INSERT OR IGNORE INTO documents (folder, name) VALUES (?, ?)", [b.folder, b.name]);
    const doc = db.query("SELECT id FROM documents WHERE folder = ? AND name = ?", [b.folder, b.name])[0].id;
    const version = (db.query("SELECT MAX(version) AS v FROM versions WHERE doc = ?", [doc])[0].v || 0) + 1;
    db.exec("INSERT INTO versions VALUES (?, ?, ?, ?, ?, ?)", [doc, version, up.digest, up.type, up.size, Date.now()]);
    if (up.type.startsWith("text/")) indexDoc(doc, new TextDecoder().decode(readBytes(blobPath(up.digest))));
    audit.info({ event: "version.created", doc, version, digest: up.digest }, "new version");
    return json(201, { doc, version, digest: up.digest });
});
app.get("/documents/:id/versions", (req) =>
    db.query("SELECT version, blob, type, size FROM versions WHERE doc = ? ORDER BY version", [Number(req.params.id)]),
);
app.get("/search", (req) => search(req.query.q || ""));
app.post("/documents/:id/share", (req) => {
    const v = db.query("SELECT MAX(version) AS v FROM versions WHERE doc = ?", [Number(req.params.id)])[0].v;
    if (!v) return json(404, { error: "no such document" });
    return json(201, {
        link: `/s/${shareLink(Number(req.params.id), v, Number(JSON.parse(req.body || "{}").ttl) || 3600)}`,
    });
});
app.get("/s/:token", (req) => {
    const r = openLink(req.params.token);
    if (r.error) {
        audit.warn({ event: "share.refused", reason: r.error }, "share link refused");
        return json(403, { error: r.error });
    }
    const v = db.query("SELECT * FROM versions WHERE doc = ? AND version = ?", [r.doc, r.version])[0];
    return v ? { body: readBytes(blobPath(v.blob)), contentType: v.type } : json(410, { error: "version purged" });
});
app.get("/export/*folder", (req) => {
    // a folder's latest versions as a verified zip
    const rows = db.query(
        `SELECT d.name, v.blob, v.version FROM documents d JOIN versions v ON v.doc = d.id
        WHERE d.folder = ? AND v.version = (SELECT MAX(version) FROM versions WHERE doc = d.id) ORDER BY d.name`,
        [req.params.folder],
    );
    const files = rows.map((r) => ({ name: r.name, data: readBytes(blobPath(r.blob)) }));
    const manifest = files.map((f) => ({ name: f.name, sha256: SHA256Hex(f.data), size: f.data.length }));
    return {
        body: ZipPack([
            ...files,
            { name: "MANIFEST.json", data: new TextEncoder().encode(JSON.stringify(manifest, null, 2)) },
        ]),
        contentType: "application/zip",
    };
});
app.post("/admin/retention", () => json(200, purge(options.keep)));
app.get("/admin/retention/next", () => ({ next: retention.next(new Date())?.toISOString() }));
app.start();

if (!selfTest)
    print(
        `document-management on :${app.port}  root=${root}  next retention ${retention.next(new Date()).toISOString()}`,
    );
else await selfCheck();

async function selfCheck() {
    const results = {};
    const base = `http://127.0.0.1:${app.port}`;
    const upload = async (type, body) => {
        const r = await fetch(base + "/upload", { method: "POST", headers: { "content-type": type }, body });
        const j = JSON.parse(await r.text());
        return { status: r.status, id: j.id ?? null };
    };
    const claim = async (id, folder, name) => {
        const r = await fetch(base + "/documents", {
            method: "POST",
            body: JSON.stringify({ upload: id, folder, name }),
            headers: { "content-type": "application/json" },
        });
        return { status: r.status, body: await r.json() };
    };
    const add = async (type, body, folder, name) => claim((await upload(type, body)).id, folder, name);

    const policy = (n) =>
        `# Travel policy v${n}\n\nEmployees book economy for flights under ${4 + n} hours. Hotels up to $${180 + n * 10} per night need no approval.\nMeals are reimbursed with receipts.\n`;
    const pdf = new TextEncoder().encode("%PDF-1.7\n1 0 obj<<>>endobj\ntrailer<<>>\n%%EOF\n");
    for (let v = 1; v <= 5; v++)
        results[`travel.md v${v}`] = (await add("text/markdown", policy(v), "hr/policies", "travel.md")).status === 201;
    results["security.txt"] =
        (
            await add(
                "text/plain",
                "Security policy: rotate keys every 90 days. Report incidents within 24 hours.\n",
                "hr/policies",
                "security.txt",
            )
        ).status === 201;
    results["pdf"] = (await add("application/pdf", pdf, "hr/policies", "handbook.pdf")).body.version === 1;
    const dup = await add("application/pdf", pdf, "finance", "handbook-copy.pdf");
    results["the same bytes under another name"] = dup.status === 201;
    const evil = await upload("image/png", "#!/bin/sh\nrm -rf /\n");
    results["an impostor is quarantined and cannot be claimed"] =
        evil.id === null || !(await claim(evil.id, "x", "evil.png")).body.doc;
    results["quarantine recorded"] = rejected.length === 1 && rejected[0].declared === "image/png";

    const versions = await (await fetch(`${base}/documents/1/versions`)).json();
    results["five distinct versions of travel.md"] =
        versions.length === 5 && new Set(versions.map((v) => v.blob)).size === 5;
    const hits = await (await fetch(`${base}/search?q=hotels`)).json();
    results[`search with snippet ${JSON.stringify(hits)}`] =
        hits.length === 1 && hits[0].path === "hr/policies/travel.md" && /\[Hotels\] up to \$230/.test(hits[0].snippet);
    const travelChanges = changes.get(1);
    results[`a new version's word diff names exactly what changed (${travelChanges?.added.join(" ")})`] =
        travelChanges &&
        travelChanges.added.join(" ") === "v5 9 $230" &&
        travelChanges.removed.join(" ") === "v4 8 $220";
    results["second document searchable"] =
        (await (await fetch(`${base}/search?q=incidents`)).json())[0].path === "hr/policies/security.txt";

    const { link } = await (
        await fetch(`${base}/documents/1/share`, {
            method: "POST",
            body: JSON.stringify({ ttl: 600 }),
            headers: { "content-type": "application/json" },
        })
    ).json();
    const shared = await fetch(base + link);
    results["a share link serves the latest version"] = shared.status === 200 && (await shared.text()).includes("v5");
    const tampered = link.slice(0, -3) + (link.endsWith("A") ? "BAA" : "AAA");
    results["a tampered link is refused"] = (await fetch(base + tampered)).status === 403;
    results["an expired link is refused"] = openLink(shareLink(1, 5, 60, 1000), 1100).error === "expired";

    const zip = new Uint8Array(await (await fetch(`${base}/export/hr/policies`)).arrayBuffer());
    const manifest = JSON.parse(new TextDecoder().decode(ZipRead(zip, "MANIFEST.json")));
    results["export verifies against its manifest"] =
        manifest.length === 3 && manifest.every((m) => SHA256Hex(ZipRead(zip, m.name)) === m.sha256);
    results["export holds three documents and the manifest"] = ZipList(zip).length === 4;

    const blobsBefore = new Set(db.query("SELECT blob FROM versions").map((r) => r.blob)).size;
    const purged = await (await fetch(`${base}/admin/retention`, { method: "POST" })).json();
    results[`retention keeps 3 versions (${JSON.stringify(purged)}, ${blobsBefore} blobs before)`] =
        purged.removedVersions === 2 && purged.removedBlobs === 2;
    results["four Sunday retention runs in January 2026"] =
        retention.between(new Date("2026-01-01T00:00:00Z"), new Date("2026-02-01T00:00:00Z")).length === 4;

    print("document-management");
    print(
        `  documents  ${db.query("SELECT COUNT(*) AS n FROM documents")[0].n}, versions ${db.query("SELECT COUNT(*) AS n FROM versions")[0].n}, blobs ${blobsBefore - purged.removedBlobs} (one shared by two documents)`,
    );
    print(`  search     "hotels" -> ${hits[0].snippet.replace(/\n/g, " ")}`);
    print(
        `  audit      ${auditLines
            .map((l) => l.event)
            .filter((e, i, a) => a.indexOf(e) === i)
            .join(", ")}`,
    );
    app.close();
    db.close();
    linkKey.close();
    removeAll(root);
    check(results);
}

function check(results) {
    const names = Object.keys(results);
    const failed = names.filter((name) => !results[name]);
    for (const name of failed) print("FAIL " + name);
    if (failed.length) throw new Error(`${failed.length} of ${names.length} checks failed`);
}
