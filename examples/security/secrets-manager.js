// Secrets manager — a Vault-style service: Shamir-sealed master key, envelope encryption, versioned secrets,
// path policies, short-lived tokens, dynamic leases, key rotation and a tamper-evident audit log.
//
// WHAT IT IS
//   The service an infrastructure team runs so applications never keep secrets in config files:
//
//   sealing      the master key is split into 5 Shamir shares over GF(256), any 3 unseal; the server starts SEALED
//                and refuses everything until enough operators submit shares (a wrong share is detected by a key
//                check value, not by decrypting garbage)
//   encryption   envelope scheme: per-secret data keys derived with HKDF from the active key-encryption key and a
//                random salt; secrets sealed with an AEAD whose associated data binds the path and version, so a
//                ciphertext copied to another path will not open
//   storage      SQLite: secrets are versioned (read latest or ?version=N, soft delete, destroy), metadata kept
//   policies     YAML policies map path globs to capabilities (read, create, update, delete, list); deny wins
//   tokens       issued by an app-role login (role id + secret id), carry policies and a TTL, renewable up to a max
//   leases       dynamic database credentials generated on read, with a lease that expires and is revoked
//   rotation     a new key-encryption key version; old secrets still open; a rewrap job upgrades them in place
//   audit        every request is logged with HMAC'd (never plain) secret paths and token ids, hash-chained
//   expiry       lease ids are interned and their deadlines kept in a min IndexedHeap: the revocation sweep pops
//                exactly the expired leases (popUntil(now)) instead of scanning every live lease each time
//
// MODULES  dyna:crypto (ChaCha20Poly1305/AESGCM, HKDF, HMAC, RandomBytes, TimingSafeEqual) · dyna:net (SQLite)
//          · dyna:http · dyna:yaml · dyna:hash (BLAKE3) · dyna:encoding (Base64URL) · dyna:file (glob matching)
//          · dyna:time · dyna:uuid · dyna:structures (Interner, IndexedHeap) · dyna:bytes (concat)
//
// RUN      dynajs examples/security/secrets-manager.js     starts sealed, unseals, and runs an application's lifecycle

import * as crypto from "dyna:crypto";
import { SQLite } from "dyna:net";
import { App } from "dyna:http";
import * as YAML from "dyna:yaml";
import { BLAKE3Hex } from "dyna:hash";
import { Base64URLEncode, Base64URLDecode, HexEncode } from "dyna:encoding";
import { makeTempDir, removeAll } from "dyna:file";
import { durationString } from "dyna:time";
import { v4 as uuid4 } from "dyna:uuid";
import { Interner, IndexedHeap } from "dyna:structures";
import { concat } from "dyna:bytes";

const work = makeTempDir("vault");
const AEAD = typeof crypto.ChaCha20Poly1305 === "function" ? crypto.ChaCha20Poly1305 : crypto.AESGCM;
const enc = new TextEncoder(),
    dec = new TextDecoder();
let clock = Date.UTC(2026, 4, 1); // a controllable clock for TTLs
const now = () => clock;

// =============================================================================
// Shamir secret sharing over GF(256)
// =============================================================================
const EXP = new Uint8Array(512),
    LOG = new Uint8Array(256);
for (let i = 0, x = 1; i < 255; i++) {
    EXP[i] = x;
    LOG[x] = i;
    x ^= (x << 1) ^ (x & 0x80 ? 0x11b : 0);
    x &= 0xff;
}
for (let i = 255; i < 512; i++) EXP[i] = EXP[i - 255];
const gmul = (a, b) => (a && b ? EXP[LOG[a] + LOG[b]] : 0);
const gdiv = (a, b) => {
    if (!b) throw new Error("division by zero");
    return a ? EXP[(LOG[a] + 255 - LOG[b]) % 255] : 0;
};
function split(secret, n, k) {
    const shares = Array.from({ length: n }, (_, i) => {
        const s = new Uint8Array(secret.length + 1);
        s[0] = i + 1;
        return s;
    });
    for (let b = 0; b < secret.length; b++) {
        const coeffs = [secret[b], ...crypto.RandomBytes(k - 1)];
        for (const s of shares) {
            let y = 0;
            for (let d = coeffs.length - 1; d >= 0; d--) y = gmul(y, s[0]) ^ coeffs[d]; // Horner in GF(256)
            s[1 + b] = y;
        }
    }
    return shares;
}
function combine(shares) {
    const out = new Uint8Array(shares[0].length - 1);
    for (let b = 0; b < out.length; b++) {
        let acc = 0;
        for (let i = 0; i < shares.length; i++) {
            let num = 1,
                den = 1;
            for (let j = 0; j < shares.length; j++)
                if (i !== j) {
                    num = gmul(num, shares[j][0]);
                    den = gmul(den, shares[j][0] ^ shares[i][0]);
                }
            acc ^= gmul(shares[i][1 + b], gdiv(num, den));
        }
        out[b] = acc;
    }
    return out;
}

// =============================================================================
// Storage and barrier
// =============================================================================
const db = new SQLite(work.join("vault.db"));
db.exec("CREATE TABLE IF NOT EXISTS meta (k TEXT PRIMARY KEY, v TEXT)");
db.exec("CREATE TABLE IF NOT EXISTS keyring (version INTEGER PRIMARY KEY, wrapped TEXT, created INTEGER)");
db.exec(
    "CREATE TABLE IF NOT EXISTS secrets (path TEXT, version INTEGER, kek INTEGER, salt TEXT, nonce TEXT, sealed TEXT, created INTEGER, deleted INTEGER DEFAULT 0, PRIMARY KEY (path, version))",
);
db.exec("CREATE TABLE IF NOT EXISTS audit (seq INTEGER PRIMARY KEY, at INTEGER, entry TEXT, prev TEXT, hash TEXT)");
const b64 = (u8) => Base64URLEncode(u8),
    unb64 = (s) => Base64URLDecode(s);
const state = { sealed: true, master: null, keyring: new Map(), active: 0, pending: [] };

function initialise() {
    const master = crypto.RandomBytes(32),
        shares = split(master, 5, 3);
    const check = crypto.HMAC("sha256", master, "key-check-value").slice(0, 8);
    db.exec("INSERT INTO meta VALUES ('kcv', ?), ('threshold', '3')", [b64(check)]);
    state.master = master;
    rotateKek();
    state.master = null;
    state.keyring.clear();
    return shares.map((s) => b64(s));
}
function wrap(key) {
    const a = new AEAD(state.master),
        nonce = crypto.RandomBytes(12);
    const out = b64(concat([nonce, a.seal(nonce, key, "keyring")]));
    a.close?.();
    return out;
}
function unwrap(text) {
    const u = unb64(text),
        a = new AEAD(state.master);
    const k = a.open(u.slice(0, 12), u.slice(12), "keyring");
    a.close?.();
    return k;
}
function rotateKek() {
    const version = (db.query("SELECT MAX(version) AS v FROM keyring")[0].v || 0) + 1,
        kek = crypto.RandomBytes(32);
    db.exec("INSERT INTO keyring VALUES (?, ?, ?)", [version, wrap(kek), now()]);
    state.keyring.set(version, kek);
    state.active = version;
    return version;
}
function submitShare(text) {
    if (!state.sealed) return { sealed: false };
    const share = unb64(text);
    if (share.length !== 33 || state.pending.some((s) => s[0] === share[0]))
        return { error: "malformed or repeated share", progress: state.pending.length };
    state.pending.push(share);
    const threshold = Number(db.query("SELECT v FROM meta WHERE k = 'threshold'")[0].v);
    if (state.pending.length < threshold) return { sealed: true, progress: state.pending.length, threshold };
    const candidate = combine(state.pending);
    state.pending = [];
    const kcv = unb64(db.query("SELECT v FROM meta WHERE k = 'kcv'")[0].v);
    if (!crypto.TimingSafeEqual(crypto.HMAC("sha256", candidate, "key-check-value").slice(0, 8), kcv))
        return { sealed: true, error: "shares do not reconstruct the master key" };
    state.master = candidate;
    state.sealed = false;
    for (const r of db.query("SELECT version, wrapped FROM keyring")) state.keyring.set(r.version, unwrap(r.wrapped));
    state.active = Math.max(...state.keyring.keys());
    return { sealed: false };
}
function seal() {
    state.master?.fill(0);
    for (const k of state.keyring.values()) k.fill(0);
    state.keyring.clear();
    state.master = null;
    state.sealed = true;
}

const dataKey = (kek, salt, path, version) =>
    crypto.HKDF({ hash: "sha256", key: kek, salt, info: `secret:${path}:${version}`, length: 32 });
function putSecret(path, value) {
    const version = (db.query("SELECT MAX(version) AS v FROM secrets WHERE path = ?", [path])[0].v || 0) + 1;
    const salt = crypto.RandomBytes(16),
        nonce = crypto.RandomBytes(12),
        aead = new AEAD(dataKey(state.keyring.get(state.active), salt, path, version));
    const sealed = aead.seal(nonce, JSON.stringify(value), `${path}#${version}`);
    db.exec("INSERT INTO secrets (path, version, kek, salt, nonce, sealed, created) VALUES (?,?,?,?,?,?,?)", [
        path,
        version,
        state.active,
        b64(salt),
        b64(nonce),
        b64(sealed),
        now(),
    ]);
    return version;
}
function getSecret(path, version) {
    const row = version
        ? db.query("SELECT * FROM secrets WHERE path = ? AND version = ?", [path, version])[0]
        : db.query("SELECT * FROM secrets WHERE path = ? AND deleted = 0 ORDER BY version DESC LIMIT 1", [path])[0];
    if (!row || row.deleted) return null;
    const aead = new AEAD(dataKey(state.keyring.get(row.kek), unb64(row.salt), path, row.version));
    return {
        version: row.version,
        kek: row.kek,
        data: JSON.parse(dec.decode(aead.open(unb64(row.nonce), unb64(row.sealed), `${path}#${row.version}`))),
    };
}
function rewrap() {
    let n = 0;
    for (const r of db.query("SELECT path, version FROM secrets WHERE kek < ? AND deleted = 0", [state.active])) {
        let data;
        try {
            ({ data } = getSecret(r.path, r.version));
        } catch {
            state.unreadable = (state.unreadable || 0) + 1;
            continue;
        } // a row that fails authentication is left for an operator
        const salt = crypto.RandomBytes(16),
            nonce = crypto.RandomBytes(12),
            aead = new AEAD(dataKey(state.keyring.get(state.active), salt, r.path, r.version));
        db.exec("UPDATE secrets SET kek = ?, salt = ?, nonce = ?, sealed = ? WHERE path = ? AND version = ?", [
            state.active,
            b64(salt),
            b64(nonce),
            b64(aead.seal(nonce, JSON.stringify(data), `${r.path}#${r.version}`)),
            r.path,
            r.version,
        ]);
        n++;
    }
    return n;
}

// =============================================================================
// Policies, roles, tokens, leases, audit
// =============================================================================
const POLICIES = YAML.Parse(`
billing-app:
  - { path: "secret/billing/*", caps: [read, list] }
  - { path: "database/creds/billing-ro", caps: [read] }
  - { path: "secret/billing/stripe-live", caps: [deny] }
ci-deployer:
  - { path: "secret/*", caps: [create, update, read, list] }
  - { path: "secret/*/root-*", caps: [deny] }
admin:
  - { path: "*", caps: [read, create, update, delete, list, sudo] }
`);
const globRe = (g) => new RegExp("^" + g.replace(/[.+^${}()|[\]\\]/g, "\\$&").replace(/\*/g, ".*") + "$");
const compiled = Object.fromEntries(
    Object.entries(POLICIES).map(([k, rules]) => [
        k,
        rules.map((r) => ({
            re: globRe(r.path),
            caps: new Set(r.caps),
            specificity: r.path.replace(/\*/g, "").length,
        })),
    ]),
);
function allowed(policies, path, cap) {
    const rules = policies.flatMap((p) => compiled[p] || []).filter((r) => r.re.test(path));
    if (rules.some((r) => r.caps.has("deny"))) return false;
    return rules.some((r) => r.caps.has(cap));
}
const ROLES = new Map([
    ["role-billing", { secretId: "s3cr3t-billing", policies: ["billing-app"], ttl: 3600_000, maxTtl: 4 * 3600_000 }],
    ["role-ci", { secretId: "s3cr3t-ci", policies: ["ci-deployer"], ttl: 900_000, maxTtl: 900_000 }],
]);
const tokens = new Map(),
    leases = new Map(),
    dbUsers = new Set();
const leaseIds = new Interner(),
    leaseDeadlines = new IndexedHeap();
const auditKey = crypto.RandomBytes(32);
const hmacId = (s) => "hmac:" + crypto.HMACHex("sha256", auditKey, s).slice(0, 20);
function audit(entry) {
    const prev = db.query("SELECT hash FROM audit ORDER BY seq DESC LIMIT 1")[0]?.hash || "genesis";
    const body = JSON.stringify(entry),
        hash = BLAKE3Hex(prev + body);
    db.exec("INSERT INTO audit (at, entry, prev, hash) VALUES (?, ?, ?, ?)", [now(), body, prev, hash]);
}
function issueToken(policies, ttl, maxTtl) {
    const id = "hvs." + b64(crypto.RandomBytes(24));
    tokens.set(id, { policies, expires: now() + ttl, ttl, hardExpiry: now() + maxTtl });
    return id;
}
function lookup(tokenId) {
    const t = tokens.get(tokenId || "");
    if (!t || now() >= t.expires) return null;
    return t;
}
function sweepLeases() {
    let revoked = 0;
    for (const n of leaseDeadlines.popUntil(now())) {
        const id = leaseIds.get(n),
            l = leases.get(id);
        if (!l) continue;
        dbUsers.delete(l.user);
        leases.delete(id);
        revoked++;
    }
    return revoked;
}

// =============================================================================
// HTTP API
// =============================================================================
const json = (status, obj) => ({ status, body: JSON.stringify(obj), contentType: "application/json" });
const app = new App({ port: 0 });
app.use((req) => {
    if (req.path.startsWith("/v1/sys/") || !state.sealed) return;
    return { response: json(503, { errors: ["Vault is sealed"] }) };
});
app.post("/v1/sys/unseal", (req) => json(200, submitShare(JSON.parse(req.body).key)));
app.post("/v1/sys/seal", (req) => {
    const t = lookup(req.headers["x-vault-token"]);
    if (!t || !allowed(t.policies, "sys/seal", "sudo")) return json(403, { errors: ["permission denied"] });
    seal();
    return json(204, {});
});
app.get("/v1/sys/health", () => json(state.sealed ? 503 : 200, { sealed: state.sealed, keyVersion: state.active }));
app.post("/v1/auth/approle/login", (req) => {
    const { role_id, secret_id } = JSON.parse(req.body),
        role = ROLES.get(role_id);
    const ok =
        role &&
        role.secretId.length === secret_id.length &&
        crypto.TimingSafeEqual(enc.encode(role.secretId), enc.encode(secret_id));
    audit({ op: "login", role: role_id, ok: !!ok });
    if (!ok) return json(400, { errors: ["invalid role or secret id"] });
    return json(200, {
        auth: {
            client_token: issueToken(role.policies, role.ttl, role.maxTtl),
            policies: role.policies,
            lease_duration: role.ttl / 1000,
        },
    });
});
app.post("/v1/auth/token/renew-self", (req) => {
    const t = lookup(req.headers["x-vault-token"]);
    if (!t) return json(403, { errors: ["permission denied"] });
    t.expires = Math.min(now() + t.ttl, t.hardExpiry);
    return json(200, { auth: { lease_duration: Math.round((t.expires - now()) / 1000) } });
});
function guarded(cap, fn) {
    return (req) => {
        const t = lookup(req.headers["x-vault-token"]),
            path = req.params.rest ? `secret/${req.params.rest}` : req.path.slice(4);
        const ok = t && allowed(t.policies, path, cap);
        audit({ op: cap, path: hmacId(path), token: hmacId(req.headers["x-vault-token"] || "none"), ok: !!ok });
        return ok ? fn(req, path, t) : json(403, { errors: ["permission denied"] });
    };
}
app.get(
    "/v1/secret/*rest",
    guarded("read", (req, path) => {
        const s = getSecret(path, Number(req.query.version) || 0);
        return s ? json(200, { data: s.data, metadata: { version: s.version } }) : json(404, { errors: [] });
    }),
);
app.put(
    "/v1/secret/*rest",
    guarded("update", (req, path) => json(200, { version: putSecret(path, JSON.parse(req.body).data) })),
);
app.del(
    "/v1/secret/*rest",
    guarded("delete", (req, path) => {
        const n = db.exec(
            "UPDATE secrets SET deleted = 1 WHERE path = ? AND version = (SELECT MAX(version) FROM secrets WHERE path = ?)",
            [path, path],
        );
        return json(n ? 204 : 404, {});
    }),
);
app.get(
    "/v1/database/creds/billing-ro",
    guarded("read", () => {
        const user = `v-billing-${uuid4().slice(0, 8)}`,
            password = b64(crypto.RandomBytes(18)),
            id = `database/creds/billing-ro/${uuid4()}`;
        dbUsers.add(user);
        leases.set(id, { user, expires: now() + 600_000 });
        leaseDeadlines.push(leaseIds.intern(id), now() + 600_000);
        return json(200, { lease_id: id, lease_duration: 600, data: { username: user, password } });
    }),
);
app.post("/v1/sys/rotate", (req) => {
    const t = lookup(req.headers["x-vault-token"]);
    if (!t || !allowed(t.policies, "sys/rotate", "sudo")) return json(403, {});
    return json(200, { version: rotateKek(), rewrapped: rewrap() });
});
app.start();

// =============================================================================
// An operator team and two applications
// =============================================================================
const results = {};
const base = `http://127.0.0.1:${app.port}`;
const call = async (method, path, { token, body } = {}) => {
    const r = await fetch(base + path, {
        method,
        headers: token ? { "x-vault-token": token } : {},
        body: body && JSON.stringify(body),
    });
    const text = await r.text();
    return { status: r.status, body: text ? JSON.parse(text) : null };
};
const shares = initialise();
results["a sealed vault refuses requests"] = (await call("GET", "/v1/secret/billing/db")).status === 503;
const wrong = split(crypto.RandomBytes(32), 5, 3).map(b64);
await call("POST", "/v1/sys/unseal", { body: { key: shares[0] } });
await call("POST", "/v1/sys/unseal", { body: { key: wrong[1] } });
const bad = await call("POST", "/v1/sys/unseal", { body: { key: shares[2] } });
results["a foreign share is detected by the key check value"] =
    bad.body.sealed && /do not reconstruct/.test(bad.body.error);
for (const k of [shares[4], shares[1]]) await call("POST", "/v1/sys/unseal", { body: { key: k } });
const third = await call("POST", "/v1/sys/unseal", { body: { key: shares[3] } });
results["any three genuine shares unseal"] = !third.body.sealed;

const adminToken = issueToken(["admin"], 86400_000, 86400_000);
for (const [p, v] of [
    ["billing/db", { user: "billing", password: "pw-1" }],
    ["billing/stripe-live", { key: "sk_live_x" }],
    ["billing/root-ca", { pem: "..." }],
    ["payroll/db", { password: "pw-pay" }],
])
    await call("PUT", `/v1/secret/${p}`, { token: adminToken, body: { data: v } });
await call("PUT", "/v1/secret/billing/db", {
    token: adminToken,
    body: { data: { user: "billing", password: "pw-2" } },
});

const login = await call("POST", "/v1/auth/approle/login", {
    body: { role_id: "role-billing", secret_id: "s3cr3t-billing" },
});
const appToken = login.body.auth.client_token;
const read = await call("GET", "/v1/secret/billing/db", { token: appToken });
results["the app reads the latest version"] =
    read.status === 200 && read.body.data.password === "pw-2" && read.body.metadata.version === 2;
results["older versions stay readable"] =
    (await call("GET", "/v1/secret/billing/db?version=1", { token: appToken })).body.data.password === "pw-1";
results["deny beats a broader allow"] =
    (await call("GET", "/v1/secret/billing/stripe-live", { token: appToken })).status === 403;
results["paths outside the policy are refused"] =
    (await call("GET", "/v1/secret/payroll/db", { token: appToken })).status === 403;
results["a read-only policy cannot write"] =
    (await call("PUT", "/v1/secret/billing/db", { token: appToken, body: { data: {} } })).status === 403;
results["a wrong secret id cannot log in"] =
    (await call("POST", "/v1/auth/approle/login", { body: { role_id: "role-billing", secret_id: "guess" } })).status ===
    400;

const ci = (await call("POST", "/v1/auth/approle/login", { body: { role_id: "role-ci", secret_id: "s3cr3t-ci" } })).body
    .auth.client_token;
results["CI may write app secrets but not root-* ones"] =
    (await call("PUT", "/v1/secret/web/api-key", { token: ci, body: { data: { k: 1 } } })).status === 200 &&
    (await call("PUT", "/v1/secret/web/root-token", { token: ci, body: { data: { k: 1 } } })).status === 403;

// Ciphertext bound to its path: copy billing/db's row onto payroll/db and it must not open.
const row = db.query("SELECT * FROM secrets WHERE path = 'secret/billing/db' AND version = 2")[0];
db.exec(
    "INSERT INTO secrets (path, version, kek, salt, nonce, sealed, created) VALUES ('secret/payroll/db', 9, ?, ?, ?, ?, 0)",
    [row.kek, row.salt, row.nonce, row.sealed],
);
let moved = "opened";
try {
    getSecret("secret/payroll/db", 9);
} catch {
    moved = "refused";
}
results["a ciphertext moved to another path fails authentication"] = moved === "refused";

// Dynamic credentials and leases.
const creds = await call("GET", "/v1/database/creds/billing-ro", { token: appToken });
results["dynamic database credentials are created on read"] =
    creds.status === 200 && dbUsers.has(creds.body.data.username);
clock += 60_000;
const later = await call("GET", "/v1/database/creds/billing-ro", { token: appToken }); // issued 1 minute after the first
clock += 9.5 * 60_000; // the first lease's 10 minutes are up
results["the sweep revokes only the lease whose deadline passed"] =
    sweepLeases() === 1 && !dbUsers.has(creds.body.data.username) && dbUsers.has(later.body.data.username);
clock += 2 * 60_000;
results["an expired lease revokes its database user"] =
    sweepLeases() === 1 && !dbUsers.has(later.body.data.username) && leaseDeadlines.size === 0;

// Token TTL, renewal and the hard maximum.
for (let h = 0; h < 4; h++) {
    clock += 50 * 60_000;
    await call("POST", "/v1/auth/token/renew-self", { token: appToken });
}
clock += 61 * 60_000;
results["renewal cannot extend a token past its max TTL"] =
    (await call("GET", "/v1/secret/billing/db", { token: appToken })).status === 403;

// Rotation and rewrap.
const rot = await call("POST", "/v1/sys/rotate", { token: adminToken });
results[`key rotation rewrapped ${rot.body.rewrapped} secrets`] = rot.body.version === 2 && rot.body.rewrapped >= 5;
results["every secret now opens under the new key"] =
    db.query("SELECT COUNT(*) AS n FROM secrets WHERE kek = 1 AND path != 'secret/payroll/db'")[0].n === 0 &&
    getSecret("secret/billing/db").data.password === "pw-2";

// Seal and re-unseal with a different subset.
await call("POST", "/v1/sys/seal", { token: adminToken });
results["sealing drops the key material"] =
    (await call("GET", "/v1/secret/billing/db", { token: adminToken })).status === 503 && state.master === null;
for (const k of [shares[0], shares[2], shares[4]]) await call("POST", "/v1/sys/unseal", { body: { key: k } });
results["after re-unsealing (different shares) the data is intact"] =
    getSecret("secret/billing/db").data.password === "pw-2";

// Audit chain: no plaintext paths, and edits are detectable.
const auditRows = db.query("SELECT * FROM audit ORDER BY seq");
const chainOk = (rows) =>
    rows.every((r, i) => r.hash === BLAKE3Hex(r.prev + r.entry) && (i === 0 || r.prev === rows[i - 1].hash));
results[`${auditRows.length} audit entries chain and contain no plaintext paths or tokens`] =
    chainOk(auditRows) && !auditRows.some((r) => r.entry.includes("billing/db") || r.entry.includes("hvs."));
db.exec(
    "UPDATE audit SET entry = replace(entry, '\"ok\":false', '\"ok\":true') WHERE seq = (SELECT MIN(seq) FROM audit WHERE entry LIKE '%\"ok\":false%')",
);
results["editing an audit entry breaks the chain"] = !chainOk(db.query("SELECT * FROM audit ORDER BY seq"));

print("secrets-manager");
print(
    `  seal       5 shares, threshold 3, ${AEAD.name} AEAD; master key check ${HexEncode(unb64(db.query("SELECT v FROM meta WHERE k='kcv'")[0].v))}`,
);
print(
    `  secrets    ${db.query("SELECT COUNT(DISTINCT path) AS n FROM secrets")[0].n} paths, ${db.query("SELECT COUNT(*) AS n FROM secrets")[0].n} versions; key versions ${[...state.keyring.keys()].join(",")}`,
);
print(
    `  tokens     billing app token lived ${durationString((tokens.get(appToken).hardExpiry - Date.UTC(2026, 4, 1)) * 1e6)} (max TTL); ${leases.size} live leases`,
);
print(
    `  audit      ${auditRows.length} entries, ${auditRows.filter((r) => r.entry.includes('"ok":false')).length} denials`,
);
app.close();
db.close();
removeAll(work);
check(results);

function check(results) {
    const names = Object.keys(results);
    const failed = names.filter((name) => !results[name]);
    for (const name of failed) print("FAIL " + name);
    if (failed.length) throw new Error(`${failed.length} of ${names.length} checks failed`);
}
