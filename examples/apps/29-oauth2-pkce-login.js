// 29 · OAuth 2.0 login with PKCE — an authorization server and a client walking the full code flow.
//
// WHAT IT SHOWS
//   - dyna:oauth2 on the client: state, code verifier/challenge, authorization URL, response parsing
//   - dyna:oauth2 on the server: redirect URI allow-listing, challenge verification, token responses
//   - the three checks that make the flow safe: exact redirect match, state equality, PKCE proof
//   - single-use, short-lived authorization codes
//
// RUN      dynajs examples/apps/29-oauth2-pkce-login.js
//          Both halves run in one process here; in real life they are different companies.

import { App } from "dyna:net";
import { generateCodeVerifier, generateCodeChallenge, verifyCodeChallenge, generateState,
         buildAuthorizationUrl, parseAuthorizationResponse, buildTokenRequestBody, parseTokenResponse,
         isValidRedirectUri, secureCompare } from "dyna:oauth2";
import { JWTSign } from "dyna:crypto";
import { NanoID } from "dyna:uuid";
import { URL } from "dyna:url";

// ================= the authorization server =================================
const CLIENTS = new Map([["web-app", { redirectUris: ["http://127.0.0.1/callback"] }]]);
const SIGNING_KEY = "auth-server-signing-key-32-bytes!!";
const codes = new Map();              // code -> { clientId, redirectUri, challenge, user, expires }

const json = (status, value) => ({ status, contentType: "application/json", body: JSON.stringify(value) });
const server = new App({ port: 0 });

// The user's browser lands here. A real server shows a login and consent
// page; this one approves as a fixed user so the flow can run unattended.
server.get("/authorize", (req) => {
    const q = req.query;
    const client = CLIENTS.get(q.client_id);
    // The redirect URI must match a registered one EXACTLY. Prefix or
    // substring matching is how authorization codes get sent to attackers.
    if (!client || !isValidRedirectUri(q.redirect_uri ?? "", client.redirectUris))
        return json(400, { error: "invalid_client_or_redirect" });   // never redirect on this error
    if (q.response_type !== "code" || q.code_challenge_method !== "S256" || !q.code_challenge)
        return json(400, { error: "invalid_request" });

    const code = NanoID(32);
    codes.set(code, { clientId: q.client_id, redirectUri: q.redirect_uri, challenge: q.code_challenge,
                      user: "ada", expires: Date.now() + 60000 });
    const back = new URL(q.redirect_uri);
    back.searchParams.set("code", code);
    back.searchParams.set("state", q.state ?? "");
    return json(200, { redirect: back.href });      // a browser would receive this as a 302 Location
});

// The client's back end exchanges the code for tokens.
server.post("/token", (req) => {
    const form = Object.fromEntries(new URLSearchParams(req.body));
    const grant = codes.get(form.code);
    codes.delete(form.code);                         // single use, even when the exchange fails
    if (!grant || grant.expires < Date.now()) return json(400, { error: "invalid_grant" });
    if (grant.clientId !== form.client_id || !secureCompare(grant.redirectUri, form.redirect_uri ?? ""))
        return json(400, { error: "invalid_grant" });
    // PKCE: only whoever started the flow knows the verifier behind the
    // challenge, so a stolen code is useless on its own.
    if (!verifyCodeChallenge(form.code_verifier ?? "", grant.challenge, "S256"))
        return json(400, { error: "invalid_grant", error_description: "PKCE verification failed" });
    const now = Math.floor(Date.now() / 1000);
    return json(200, {
        access_token: JWTSign({ sub: grant.user, aud: grant.clientId, exp: now + 600 }, SIGNING_KEY, { alg: "HS256" }),
        token_type: "Bearer", expires_in: 600,
    });
});
server.start();
const AUTH = `http://127.0.0.1:${server.port}`;

// ================= the client ===============================================
// Returns what the app keeps in the user's session while they are away logging in.
function beginLogin() {
    const verifier = generateCodeVerifier();
    const { url, state } = buildAuthorizationUrl({
        authorizationEndpoint: AUTH + "/authorize",
        clientId: "web-app",
        redirectUri: "http://127.0.0.1/callback",
        scope: "profile",
        state: generateState(),
        codeChallenge: generateCodeChallenge(verifier, "S256"),
        codeChallengeMethod: "S256",
        allowInsecure: true,                         // loopback http in this demo only
    });
    return { url, session: { state, verifier } };
}

async function finishLogin(callbackUrl, session, overrides = {}) {
    const response = parseAuthorizationResponse(callbackUrl);
    if (response.error) throw new Error("authorization failed: " + response.error);
    // The state must be the one we issued, or this callback did not come from
    // a login this browser started (cross-site request forgery).
    if (!secureCompare(response.state ?? "", session.state)) throw new Error("state mismatch");
    const res = await fetch(AUTH + "/token", {
        method: "POST",
        headers: { "Content-Type": "application/x-www-form-urlencoded" },
        body: buildTokenRequestBody({
            grant_type: "authorization_code", code: response.code, client_id: "web-app",
            redirect_uri: "http://127.0.0.1/callback", code_verifier: session.verifier, ...overrides,
        }),
    });
    const text = await res.text();
    if (!res.ok) throw new Error("token exchange failed: " + JSON.parse(text).error);
    return parseTokenResponse(text);
}

// Stands in for the browser following the authorization URL.
const visit = async (url) => (await (await fetch(url)).json());

// ---- self-test -------------------------------------------------------------
const check = (cond, what) => { if (!cond) throw new Error("self-test failed: " + what); };
const failure = (p) => p.then(() => "", (e) => e.message);

const login = beginLogin();
const { redirect } = await visit(login.url);
const tokens = await finishLogin(redirect, login.session);
check(tokens.token_type === "Bearer" && tokens.access_token.split(".").length === 3, "the happy path yields a token");

check((await failure(finishLogin(redirect, login.session))).includes("invalid_grant"), "a code cannot be exchanged twice");

const second = beginLogin();
const stolen = (await visit(second.url)).redirect;
check((await failure(finishLogin(stolen, second.session, { code_verifier: generateCodeVerifier() }))).includes("invalid_grant"),
      "a stolen code fails without the original verifier");

const third = beginLogin();
const cb = (await visit(third.url)).redirect;
check((await failure(finishLogin(cb, { ...third.session, state: "attacker-chosen" }))) === "state mismatch",
      "a callback with the wrong state is rejected before any exchange");

const evil = login.url.replace(encodeURIComponent("http://127.0.0.1/callback"), encodeURIComponent("http://127.0.0.1/callback.evil.example"));
check((await visit(evil)).error === "invalid_client_or_redirect", "an unregistered redirect URI is refused");
console.log("self-test passed: code flow, single-use codes, PKCE, state, redirect allow-list");
server.close();
