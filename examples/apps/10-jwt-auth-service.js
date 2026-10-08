// 10 · Token-protected API — login issues short-lived JWTs; routes enforce expiry, audience and scopes.
//
// WHAT IT SHOWS
//   - dyna:crypto Argon2id: password hashes that carry their own parameters
//   - dyna:crypto JWTSign and dyna:oauth2 verifyJWT: signature AND claims (exp, aud, iss, scope)
//   - dyna:oauth2 bearer helpers and a correct WWW-Authenticate challenge
//
// RUN      dynajs examples/apps/10-jwt-auth-service.js
// DEPLOY   PORT=8080 JWT_SECRET=<32+ random bytes> dynajs examples/apps/10-jwt-auth-service.js
//          Password hashing blocks the thread for its whole cost; at scale move it to a Worker.

import { App } from "dyna:net";
import { Argon2id, JWTSign, RandomBytes } from "dyna:crypto";
import { verifyJWT, parseBearerHeader, buildWWWAuthenticate, formatScope } from "dyna:oauth2";
import { getEnv } from "dyna:sys";

const PORT = Number(getEnv("PORT") ?? 0);
const SECRET = getEnv("JWT_SECRET") ?? "dev-only-secret-of-at-least-32-bytes!!";
const ISSUER = "https://auth.example";
const AUDIENCE = "orders-api";
const TOKEN_TTL_SEC = 300;

// Cheap parameters keep the self-test fast. In production raise memory to
// 64 MiB or more and measure the cost on your own hardware.
const HASH_OPTS = { memory: 8192, iterations: 2, parallelism: 1 };
const hashPassword = (pw) => Argon2id.hash(pw, RandomBytes(16), HASH_OPTS);

// The user table. The stored string contains salt and parameters, so
// verification needs nothing else.
const users = new Map([
    ["ada",  { hash: hashPassword("correct horse"), scopes: ["orders:read", "orders:write"] }],
    ["bob",  { hash: hashPassword("battery staple"), scopes: ["orders:read"] }],
]);

const json = (status, value) => ({ status, contentType: "application/json", body: JSON.stringify(value) });
const now = () => Math.floor(Date.now() / 1000);

function issue(sub, scopes, ttl = TOKEN_TTL_SEC) {
    return JWTSign({ sub, iss: ISSUER, aud: AUDIENCE, scope: formatScope(scopes), iat: now(), exp: now() + ttl },
                   SECRET, { alg: "HS256" });
}

// Returns the verified claims, or an envelope describing why access is denied.
function authorize(req, requiredScope) {
    const token = parseBearerHeader(req.headers["authorization"] ?? "");
    if (!token) return { denied: json(401, { error: "bearer token required",
        challenge: buildWWWAuthenticate({ realm: "orders", error: "invalid_request" }) }) };
    try {
        // One call checks the signature, the algorithm allow-list, expiry,
        // issuer, audience and that every required scope is present.
        const claims = verifyJWT(token, SECRET, {
            algorithms: ["HS256"], iss: ISSUER, aud: AUDIENCE,
            requiredScope: [requiredScope], requireExp: true,
        });
        return { claims };
    } catch (e) {
        const insufficient = /scope/i.test(e.message);
        return { denied: json(insufficient ? 403 : 401, { error: e.message }) };
    }
}

const orders = [{ id: 1, item: "keyboard" }];
const app = new App({ port: PORT });

app.post("/login", (req) => {
    const { username, password } = JSON.parse(req.body);
    const user = users.get(username);
    // Verify even for unknown users would be ideal to equalize timing; here a
    // uniform message at least avoids telling the caller which half was wrong.
    if (!user || !Argon2id.verify(user.hash, password)) return json(401, { error: "invalid credentials" });
    return json(200, { access_token: issue(username, user.scopes), token_type: "Bearer", expires_in: TOKEN_TTL_SEC });
});

app.get("/orders", (req) => {
    const { claims, denied } = authorize(req, "orders:read");
    return denied ?? json(200, { for: claims.sub, orders });
});

app.post("/orders", (req) => {
    const { claims, denied } = authorize(req, "orders:write");
    if (denied) return denied;
    orders.push({ id: orders.length + 1, item: JSON.parse(req.body).item, by: claims.sub });
    return json(201, orders[orders.length - 1]);
});

app.start();
console.log("auth service on port", app.port);

// ---- self-test -------------------------------------------------------------
if (!PORT) {
    const base = `http://127.0.0.1:${app.port}`;
    const call = async (method, path, { token, body } = {}) => {
        const res = await fetch(base + path, {
            method, body: body && JSON.stringify(body),
            headers: token ? { Authorization: "Bearer " + token } : {},
        });
        return { status: res.status, data: await res.json() };
    };
    const login = async (username, password) => (await call("POST", "/login", { body: { username, password } }));
    const check = (cond, what) => { if (!cond) throw new Error("self-test failed: " + what); };

    check((await login("ada", "wrong")).status === 401, "a wrong password is refused");
    check((await login("nobody", "x")).status === 401, "an unknown user is refused the same way");
    const ada = (await login("ada", "correct horse")).data.access_token;
    const bob = (await login("bob", "battery staple")).data.access_token;

    check((await call("GET", "/orders")).status === 401, "no token is 401");
    check((await call("GET", "/orders", { token: bob })).status === 200, "a read scope can read");
    check((await call("POST", "/orders", { token: bob, body: { item: "mouse" } })).status === 403,
          "a read-only token cannot write");
    check((await call("POST", "/orders", { token: ada, body: { item: "mouse" } })).status === 201,
          "a write scope can write");

    const expired = issue("ada", ["orders:read"], -10);
    check((await call("GET", "/orders", { token: expired })).status === 401, "an expired token is refused");
    const forged = ada.slice(0, -4) + "AAAA";
    check((await call("GET", "/orders", { token: forged })).status === 401, "a tampered signature is refused");
    const otherAud = JWTSign({ sub: "ada", iss: ISSUER, aud: "billing-api", scope: "orders:read", exp: now() + 60 },
                             SECRET, { alg: "HS256" });
    check((await call("GET", "/orders", { token: otherAud })).status === 401, "a token for another audience is refused");
    console.log("self-test passed: login, scopes, expiry, tampering, audience");
    app.close();
}
