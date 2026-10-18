// Identity provider — accounts, 2FA, sessions, OAuth 2.0 with PKCE, token families and a tamper-evident audit trail.
//
// WHAT IT IS
//   A self-hosted identity provider with the parts that are usually bolted on later already in place:
//
//   accounts     email + password, checked against a strength policy; bcrypt hashes in SQLite
//   2FA          TOTP enrolment (base32 secret, otpauth URI, terminal QR code), a code accepted at most once
//                per time step, single-use backup codes stored only as hashes; the TOTP secret is sealed
//                with AES-GCM under a key derived by HKDF (on builds with OpenSSL)
//   lockout      five failed passwords lock an account for fifteen minutes
//   sessions     an HttpOnly, SameSite=Lax session cookie plus a double-submit CSRF token on state changes
//   OAuth 2.0    authorization code + PKCE for registered clients; ES256 access tokens (HS256 without
//                OpenSSL); refresh tokens rotate, and presenting a rotated one revokes the whole family
//   discovery    /.well-known/openid-configuration and the public key, so resource servers verify locally
//   audit        every security event appended to a BLAKE3 hash chain; verification finds any edited line
//   breaches     registration refuses passwords from a breach corpus: 20,000 leaked passwords live in a 32 KiB
//                BloomFilter (only SHA-1 digests go in, like a k-anonymity range query), so a "strong-looking" leaked
//                password is refused without keeping the corpus
//   spraying     failed logins are counted per source address in a CountMinSketch: an address that fails across
//                many accounts is throttled before any one account reaches its lockout threshold
//
// MODULES  dyna:http · dyna:net (SQLite) · dyna:crypto · dyna:oauth2 · dyna:hash · dyna:encoding
//          dyna:validate · dyna:structures (LRU, BloomFilter, CountMinSketch) · dyna:url · dyna:uuid · dyna:file
//
// RUN      dynajs examples/web/identity-provider.js     a scripted session: sign-up, 2FA enrolment, lockout,
//          backup codes, an OAuth client and a resource server, token-family revocation, audit verification

import { App, CookieParse, CookieSerialize } from "dyna:http";
import { SQLite } from "dyna:net";
import * as crypto from "dyna:crypto";
import * as oauth from "dyna:oauth2";
import { BLAKE3Hex, SHA256Hex, SHA1Hex } from "dyna:hash";
import { Base32Encode, Base32Decode, Base64URLEncode, QRToString } from "dyna:encoding";
import { IsEmail, IsStrongPassword } from "dyna:validate";
import { LRU, BloomFilter, CountMinSketch } from "dyna:structures";
import { formDecode } from "dyna:url";
import { NanoID, v7 } from "dyna:uuid";
import { makeTempDir, removeAll } from "dyna:file";

const work = makeTempDir("idp");
const db = new SQLite(work.join("idp.db"));
db.exec(`CREATE TABLE users (email TEXT PRIMARY KEY, pw TEXT NOT NULL, totp BLOB, totp_last INTEGER NOT NULL DEFAULT -1,
         failures INTEGER NOT NULL DEFAULT 0, locked_until INTEGER NOT NULL DEFAULT 0)`);
db.exec(`CREATE TABLE backup_codes (email TEXT NOT NULL, hash TEXT NOT NULL, used INTEGER NOT NULL DEFAULT 0)`);
db.exec(`CREATE TABLE refresh (hash TEXT PRIMARY KEY, family TEXT NOT NULL, email TEXT NOT NULL, client TEXT NOT NULL,
         scope TEXT NOT NULL, state TEXT NOT NULL DEFAULT 'active')`);
db.exec(`CREATE TABLE audit (seq INTEGER PRIMARY KEY, line TEXT NOT NULL, hash TEXT NOT NULL)`);

const ISSUER = "https://id.example.test";
const now = () => Math.floor(Date.now() / 1000);

// ---- key material ------------------------------------------------------------
const hasOpenSSL = typeof crypto.ECDSA?.generate === "function" && typeof crypto.AESGCM === "function";
const signing = hasOpenSSL ? crypto.ECDSA.generate("P-256") : { privateKey: crypto.RandomBytes(32), publicKey: null };
const ALG = hasOpenSSL ? "ES256" : "HS256";
const KID = "k-" + NanoID(8);
const masterKey = crypto.RandomBytes(32); // in production: from a KMS or an env secret
const secretKey = crypto.HKDF({ key: masterKey, info: "idp/totp-secrets/v1", length: 32 });
const seal = (bytes, aad) => {
    if (!hasOpenSSL) return bytes; // demo fallback; real deployments run with OpenSSL
    const { nonce, sealed } = new crypto.AESGCM(secretKey).sealRandom(bytes, aad);
    return new Uint8Array([...nonce, ...sealed]);
};
const unseal = (blob, aad) =>
    hasOpenSSL ? new crypto.AESGCM(secretKey).open(blob.slice(0, 12), blob.slice(12), aad) : blob;

// ---- audit trail: each line commits to the previous line's hash ------------------
let lastHash = "0".repeat(64);
function audit(event, fields = {}) {
    const line = JSON.stringify({ at: new Date().toISOString(), event, ...fields, prev: lastHash });
    lastHash = BLAKE3Hex(line);
    db.exec("INSERT INTO audit (line, hash) VALUES (?, ?)", [line, lastHash]);
}
function verifyAudit() {
    let prev = "0".repeat(64);
    for (const row of db.query("SELECT seq, line, hash FROM audit ORDER BY seq")) {
        if (JSON.parse(row.line).prev !== prev || BLAKE3Hex(row.line) !== row.hash)
            return { ok: false, brokenAt: row.seq };
        prev = row.hash;
    }
    return { ok: true, head: prev };
}

// ---- accounts, 2FA, lockout --------------------------------------------------------
const user = (email) => db.query("SELECT * FROM users WHERE email = ?", [email])[0];
const breached = new BloomFilter(20_000 * 13, 9); // ~0.2% false positives at 20k entries
const LEAKED = [
    "Correct-Horse-9!",
    "Summer2026!!Fun",
    "Tr0ub4dor&3xyz",
    ...Array.from({ length: 19_997 }, (_, i) => `Leaked#${(i * 2654435761) >>> 0}Pw!`),
];
for (const p of LEAKED) breached.add(SHA1Hex(p));
const failuresByAddress = new CountMinSketch(4096, 4);
function register(email, password) {
    if (!IsEmail(email)) return "a valid email is required";
    if (!IsStrongPassword(password, { minLength: 12, minSymbols: 1 }))
        return "the password needs 12+ characters with upper, lower, digit and symbol";
    if (breached.mayContain(SHA1Hex(password))) return "this password appears in a known breach; choose another";
    if (user(email)) return "account exists";
    db.exec("INSERT INTO users (email, pw) VALUES (?, ?)", [email, crypto.Bcrypt.hash(password, 4)]);
    audit("user.registered", { email });
    return null;
}
function beginTotp(email) {
    const secret = crypto.RandomBytes(20);
    db.exec("UPDATE users SET totp = ? WHERE email = ?", [seal(secret, email), email]);
    const b32 = Base32Encode(secret).replace(/=+$/, "");
    const uri = `otpauth://totp/Example:${encodeURIComponent(email)}?secret=${b32}&issuer=Example&digits=6&period=30`;
    const codes = Array.from({ length: 8 }, () => NanoID(10));
    for (const c of codes) db.exec("INSERT INTO backup_codes (email, hash) VALUES (?, ?)", [email, SHA256Hex(c)]);
    audit("totp.enrolled", { email });
    return { uri, qr: QRToString(uri, { ecc: "M" }), backupCodes: codes };
}
function checkSecondFactor(email, code) {
    const u = user(email);
    if (/^\d{6}$/.test(code)) {
        const secret = unseal(u.totp, email);
        const t = now(),
            step = Math.floor(t / 30);
        for (const w of [0, -1, 1]) {
            // one step of clock skew either way
            if (step + w <= u.totp_last) continue; // that step's code was already spent
            if (crypto.TOTPVerify(secret, code, { atSec: t + w * 30 })) {
                db.exec("UPDATE users SET totp_last = ? WHERE email = ?", [step + w, email]);
                return "totp";
            }
        }
        return null;
    }
    const n = db.exec("UPDATE backup_codes SET used = 1 WHERE email = ? AND hash = ? AND used = 0", [
        email,
        SHA256Hex(code),
    ]);
    return n === 1 ? "backup-code" : null;
}
function login(email, password, code, address = "unknown") {
    if (failuresByAddress.count(address) >= 20)
        return { status: 429, error: "too many failed sign-ins from this address" };
    const u = user(email);
    if (!u) {
        failuresByAddress.add(address);
        crypto.Bcrypt.hash(password || "x", 4);
        return { status: 401, error: "wrong email or password" };
    } // equal work for unknown users
    if (u.locked_until > now()) return { status: 423, error: "account locked; try again later" };
    if (!crypto.Bcrypt.verify(password, u.pw)) {
        failuresByAddress.add(address);
        const failures = u.failures + 1;
        db.exec("UPDATE users SET failures = ?, locked_until = ? WHERE email = ?", [
            failures,
            failures >= 5 ? now() + 900 : 0,
            email,
        ]);
        audit(failures >= 5 ? "user.locked" : "login.failed", { email, failures });
        return { status: 401, error: "wrong email or password" };
    }
    if (u.totp) {
        const how = code ? checkSecondFactor(email, code) : null;
        if (!how) {
            audit("login.second_factor_failed", { email });
            return { status: 401, error: "second factor required", mfa: true };
        }
        audit("login.second_factor", { email, how });
    }
    db.exec("UPDATE users SET failures = 0, locked_until = 0 WHERE email = ?", [email]);
    const sid = NanoID(32),
        csrf = NanoID(24);
    sessions.put(sid, { email, csrf });
    audit("login.succeeded", { email });
    return { status: 200, sid, csrf };
}
const sessions = new LRU(100_000, { ttlMs: 8 * 3600_000 });

// ---- OAuth 2.0 ------------------------------------------------------------------
const clients = new Map([
    [
        "reports-app",
        {
            secret: SHA256Hex("reports-secret"),
            redirects: ["http://127.0.0.1/cb"],
            scopes: ["profile", "reports:read"],
        },
    ],
]);
const codes = new LRU(10_000, { ttlMs: 60_000 });
function mintTokens(email, client, scope, family) {
    const t = now();
    const access = crypto.JWTSign(
        {
            iss: ISSUER,
            aud: "reports-api",
            sub: email,
            scope,
            client_id: client,
            iat: t,
            exp: t + 600,
            jti: v7(),
            kid: KID,
        },
        signing.privateKey,
        { alg: ALG },
    );
    const refresh = NanoID(43);
    db.exec("INSERT INTO refresh (hash, family, email, client, scope) VALUES (?, ?, ?, ?, ?)", [
        SHA256Hex(refresh),
        family,
        email,
        client,
        scope,
    ]);
    audit("token.issued", { email, client, family });
    return { access_token: access, token_type: "Bearer", expires_in: 600, refresh_token: refresh, scope };
}
function refreshGrant(token, client) {
    const row = db.query("SELECT * FROM refresh WHERE hash = ?", [SHA256Hex(token || "")])[0];
    if (!row || row.client !== client) return null;
    if (row.state !== "active") {
        // A rotated token came back: someone holds a copy. Revoke everything descended from that login.
        db.exec("UPDATE refresh SET state = 'revoked' WHERE family = ?", [row.family]);
        audit("token.family_revoked", { email: row.email, family: row.family });
        return null;
    }
    db.exec("UPDATE refresh SET state = 'rotated' WHERE hash = ?", [row.hash]);
    return mintTokens(row.email, client, row.scope, row.family);
}

// ---- HTTP -------------------------------------------------------------------------
const app = new App({ port: 0 });
const json = (status, body, headers) => ({
    status,
    body: JSON.stringify(body),
    contentType: "application/json",
    headers,
});
const sessionOf = (req) => sessions.get(CookieParse(req.headers.cookie || "").sid || "");
const body = (req) =>
    /json/.test(req.headers["content-type"] || "") ? JSON.parse(req.body || "{}") : formDecode(req.body || "");

app.get("/.well-known/openid-configuration", () => ({
    issuer: ISSUER,
    authorization_endpoint: "/authorize",
    token_endpoint: "/token",
    id_token_signing_alg_values_supported: [ALG],
    code_challenge_methods_supported: ["S256"],
    keys_endpoint: "/keys",
}));
app.get("/keys", () => ({ keys: hasOpenSSL ? [{ kid: KID, alg: ALG, pem: signing.publicKey }] : [] }));
app.post("/register", (req) => {
    const b = body(req);
    const err = register(b.email, b.password);
    return err ? json(400, { error: err }) : json(201, { email: b.email });
});
app.post("/login", (req) => {
    const b = body(req);
    const r = login(
        b.email || "",
        b.password || "",
        b.code,
        (req.headers["x-forwarded-for"] || "direct").split(",")[0].trim(),
    );
    if (r.status !== 200) return json(r.status, { error: r.error, mfa: r.mfa });
    return json(
        200,
        { csrf: r.csrf },
        {
            "set-cookie": CookieSerialize("sid", r.sid, {
                httpOnly: true,
                secure: true,
                sameSite: "Lax",
                path: "/",
                maxAge: 28800,
            }),
        },
    );
});
app.post("/account/totp", (req) => {
    const s = sessionOf(req);
    if (!s) return json(401, { error: "sign in first" });
    if (req.headers["x-csrf-token"] !== s.csrf) return json(403, { error: "CSRF token missing or wrong" });
    return json(201, beginTotp(s.email));
});
app.get("/authorize", (req) => {
    const q = req.query,
        c = clients.get(q.client_id);
    if (!c || !oauth.isValidRedirectUri(q.redirect_uri || "", c.redirects))
        return json(400, { error: "unknown client or redirect_uri" });
    const back = (p) => ({
        status: 302,
        body: "",
        headers: { location: `${q.redirect_uri}?${new URLSearchParams(p)}` },
    });
    const s = sessionOf(req);
    if (!s) return back({ error: "login_required", state: q.state });
    if (q.code_challenge_method !== "S256" || !q.code_challenge)
        return back({ error: "invalid_request", state: q.state });
    const scopes = oauth.parseScope(q.scope || "");
    if (!scopes.every((x) => c.scopes.includes(x))) return back({ error: "invalid_scope", state: q.state });
    const code = NanoID(32);
    codes.put(code, {
        client: q.client_id,
        redirect: q.redirect_uri,
        challenge: q.code_challenge,
        email: s.email,
        scope: oauth.formatScope(scopes),
    });
    return back({ code, state: q.state });
});
app.post("/token", (req) => {
    const m = /^Basic (.+)$/.exec(req.headers.authorization || "");
    const [id, secret] = m ? atob(m[1]).split(":").map(decodeURIComponent) : [];
    const c = clients.get(id);
    if (!c || !oauth.secureCompare(SHA256Hex(secret || ""), c.secret)) return json(401, { error: "invalid_client" });
    const f = body(req);
    if (f.grant_type === "authorization_code") {
        const g = codes.get(f.code);
        codes.delete(f.code);
        if (
            !g ||
            g.client !== id ||
            g.redirect !== f.redirect_uri ||
            !oauth.verifyCodeChallenge(f.code_verifier || "", g.challenge, "S256")
        )
            return json(400, { error: "invalid_grant" });
        return json(200, mintTokens(g.email, id, g.scope, v7()), { "cache-control": "no-store" });
    }
    if (f.grant_type === "refresh_token") {
        const t = refreshGrant(f.refresh_token, id);
        return t ? json(200, t, { "cache-control": "no-store" }) : json(400, { error: "invalid_grant" });
    }
    return json(400, { error: "unsupported_grant_type" });
});
app.start();

// ---- a resource server that verifies with the published key ----------------------------
const discovery = await (await fetch(`http://127.0.0.1:${app.port}/keys`)).json();
const verifyKey = hasOpenSSL ? discovery.keys[0].pem : signing.privateKey;
const api = new App({ port: 0 });
api.get("/reports", (req) => {
    try {
        const c = oauth.verifyJWT(oauth.parseBearerHeader(req.headers.authorization || "") || "", verifyKey, {
            algorithms: [ALG],
            aud: "reports-api",
            iss: ISSUER,
            requiredScope: ["reports:read"],
            requireExp: true,
        });
        return { owner: c.sub, reports: ["q1-revenue", "q2-revenue"] };
    } catch (e) {
        return {
            status: 401,
            body: "",
            headers: {
                "www-authenticate": oauth.buildWWWAuthenticate({ error: "invalid_token", errorDescription: e.message }),
            },
        };
    }
});
api.start();

// =============================================================================
// Scripted session
// =============================================================================
const results = {};
const IDP = `http://127.0.0.1:${app.port}`;
const jar = { cookie: "" };
async function post(path, payload, headers = {}) {
    const r = await fetch(IDP + path, {
        method: "POST",
        headers: { "content-type": "application/json", cookie: jar.cookie, ...headers },
        body: JSON.stringify(payload),
    });
    const setCookie = r.headers.get("set-cookie");
    if (setCookie) jar.cookie = setCookie.split(";")[0];
    return { status: r.status, body: JSON.parse((await r.text()) || "null"), setCookie };
}

results["weak password refused"] =
    (await post("/register", { email: "dana@example.com", password: "password" })).status === 400;
results["registration"] =
    (await post("/register", { email: "dana@example.com", password: "Tr0ub4dor&3-horse" })).status === 201;
const leaked = await post("/register", { email: "erin@example.com", password: "Correct-Horse-9!" });
results["a strong-looking password from the breach corpus is refused"] = leaked.status === 400;
const fresh = Array.from({ length: 10_000 }, (_, i) => `Fresh-${i}-Pw!x`).filter((p) =>
    breached.mayContain(SHA1Hex(p)),
).length;
results[`the breach filter's false-positive rate on 10,000 fresh passwords is ${(fresh / 100).toFixed(2)}%`] =
    fresh / 10_000 < 0.01;
const first = await post("/login", { email: "dana@example.com", password: "Tr0ub4dor&3-horse" });
results["login sets a hardened session cookie"] =
    first.status === 200 && /HttpOnly/i.test(first.setCookie) && /SameSite=Lax/i.test(first.setCookie);

results["state change without the CSRF token is refused"] = (await post("/account/totp", {})).status === 403;
const enrol = await post("/account/totp", {}, { "x-csrf-token": first.body.csrf });
results["TOTP enrolment"] =
    enrol.status === 201 && enrol.body.uri.startsWith("otpauth://totp/") && enrol.body.backupCodes.length === 8;
const secret = Base32Decode(new URL(enrol.body.uri).searchParams.get("secret").padEnd(32, "="));

jar.cookie = "";
results["password alone now asks for the second factor"] =
    (await post("/login", { email: "dana@example.com", password: "Tr0ub4dor&3-horse" })).body.mfa === true;
const code = crypto.TOTPGenerate(secret, { atSec: now() });
results["password + TOTP signs in"] =
    (await post("/login", { email: "dana@example.com", password: "Tr0ub4dor&3-horse", code })).status === 200;
results["the same TOTP code cannot be replayed"] =
    (await post("/login", { email: "dana@example.com", password: "Tr0ub4dor&3-horse", code })).status === 401;
const backup = enrol.body.backupCodes[3];
results["a backup code works"] =
    (await post("/login", { email: "dana@example.com", password: "Tr0ub4dor&3-horse", code: backup })).status === 200;
results["a backup code works only once"] =
    (await post("/login", { email: "dana@example.com", password: "Tr0ub4dor&3-horse", code: backup })).status === 401;

// OAuth: the reports app gets a token through the browser session.
const verifier = oauth.generateCodeVerifier();
const { url, state } = oauth.buildAuthorizationUrl({
    authorizationEndpoint: `${IDP}/authorize`,
    clientId: "reports-app",
    redirectUri: "http://127.0.0.1:9000/cb",
    scope: "profile reports:read",
    codeChallenge: oauth.generateCodeChallenge(verifier),
    codeChallengeMethod: "S256",
});
const redirect = await fetch(url, { redirect: "manual", headers: { cookie: jar.cookie } });
await redirect.text();
const back = oauth.parseAuthorizationResponse(redirect.headers.get("location"));
results["authorization code issued to the signed-in user"] = back.state === state && back.code;
const tokenCall = async (form) => {
    const r = await fetch(`${IDP}/token`, {
        method: "POST",
        headers: {
            authorization: oauth.buildClientAuthHeader("reports-app", "reports-secret"),
            "content-type": "application/x-www-form-urlencoded",
        },
        body: oauth.buildTokenRequestBody(form),
    });
    return { status: r.status, body: JSON.parse(await r.text()) };
};
const t1 = await tokenCall({
    grant_type: "authorization_code",
    code: back.code,
    redirect_uri: "http://127.0.0.1:9000/cb",
    code_verifier: verifier,
});
results["code exchanged for tokens"] = t1.status === 200;
const reports = await fetch(`http://127.0.0.1:${api.port}/reports`, {
    headers: { authorization: oauth.buildBearerHeader(t1.body.access_token) },
});
results["the resource server accepts the token via the published key"] =
    reports.status === 200 && (await reports.json()).owner === "dana@example.com";

const t2 = await tokenCall({ grant_type: "refresh_token", refresh_token: t1.body.refresh_token });
results["refresh rotates"] = t2.status === 200;
results["a rotated refresh token is refused"] =
    (await tokenCall({ grant_type: "refresh_token", refresh_token: t1.body.refresh_token })).status === 400;
results["reuse detection revoked the whole family, including the newest token"] =
    (await tokenCall({ grant_type: "refresh_token", refresh_token: t2.body.refresh_token })).status === 400;

// Lockout after five wrong passwords, even with the right one afterwards.
for (let i = 0; i < 5; i++) await post("/login", { email: "dana@example.com", password: "nope" + i });
results["account locked after five failures"] =
    (
        await post("/login", {
            email: "dana@example.com",
            password: "Tr0ub4dor&3-horse",
            code: crypto.TOTPGenerate(secret, { atSec: now() + 30 }),
        })
    ).status === 423;

// Password spraying: one address tries one password against many accounts; no account locks, the address does.
for (let i = 0; i < 6; i++)
    await post("/register", { email: `user${i}@example.com`, password: `Unique-${i}-Passw0rd!` });
const spray = [];
for (let i = 0; i < 24; i++)
    spray.push(
        (
            await post(
                "/login",
                { email: `user${i % 6}@example.com`, password: "Winter2026!!" },
                { "x-forwarded-for": "198.51.100.77" },
            )
        ).status,
    );
const honest = await post(
    "/login",
    { email: "user1@example.com", password: "Unique-1-Passw0rd!" },
    { "x-forwarded-for": "192.0.2.10" },
);
results[
    `a spraying address is throttled (${spray.filter((x) => x === 429).length} of 24 refused) while each account stays below its lockout`
] = spray.filter((x) => x === 429).length >= 3 && spray.indexOf(429) >= 15 && honest.status === 200;

// The audit trail verifies, and a single edited line breaks it.
const v = verifyAudit();
const events = db.query("SELECT line FROM audit").map((r) => JSON.parse(r.line).event);
results[`audit chain verifies (${events.length} events)`] =
    v.ok && events.includes("token.family_revoked") && events.includes("user.locked");
const victim = db.query("SELECT seq, line FROM audit WHERE line LIKE '%login.failed%' LIMIT 1")[0];
db.exec("UPDATE audit SET line = ? WHERE seq = ?", [
    victim.line.replace("login.failed", "login.succeeded"),
    victim.seq,
]);
results["tampering is located to the edited line"] = verifyAudit().brokenAt === victim.seq;

print(`identity-provider (${ALG}${hasOpenSSL ? ", TOTP secrets sealed with AES-GCM" : ""})`);
print(`  audit      ${events.length} events, head ${v.head.slice(0, 16)}…`);
print(`  events     ${[...new Set(events)].join(", ")}`);
print(enrol.body.qr.split("\n").slice(0, 4).join("\n") + "\n  … (TOTP enrolment QR, truncated)");
app.close();
api.close();
db.close();
removeAll(work);
check(results);

function check(results) {
    const names = Object.keys(results);
    const failed = names.filter((name) => !results[name]);
    for (const name of failed) print("FAIL " + name);
    if (failed.length) throw new Error(`${failed.length} of ${names.length} checks failed`);
}
