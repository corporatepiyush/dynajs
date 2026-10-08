// 09 · URL shortener — validates targets, mints short codes, counts hits, persists in SQLite.
//
// WHAT IT SHOWS
//   - dyna:url: strict WHATWG parsing to decide what may be shortened
//   - dyna:net isPrivate/isLoopback: refusing links into internal networks
//   - dyna:uuid NanoIDAlphabet: short, unambiguous, unguessable codes
//   - a UNIQUE index doing the collision check, with a retry on conflict
//
// RUN      dynajs examples/apps/09-url-shortener.js
// DEPLOY   PORT=8080 DB=/var/lib/short.db dynajs examples/apps/09-url-shortener.js
//          Put a web server in front that turns the JSON answer of /r/:code into a 302.

import { App, SQLite, isPrivate, isLoopback, isValid } from "dyna:net";
import { URL } from "dyna:url";
import { NanoIDAlphabet } from "dyna:uuid";
import { getEnv } from "dyna:sys";

const PORT = Number(getEnv("PORT") ?? 0);
const db = new SQLite(getEnv("DB") ?? ":memory:");
db.exec(`CREATE TABLE IF NOT EXISTS links(
           code TEXT PRIMARY KEY, target TEXT NOT NULL UNIQUE,
           hits INTEGER NOT NULL DEFAULT 0, created_at INTEGER NOT NULL)`);

// No 0/O or 1/l/I: codes get read aloud and typed from paper.
const ALPHABET = "23456789abcdefghjkmnpqrstuvwxyz";
const newCode = () => NanoIDAlphabet(ALPHABET, 7);

const json = (status, value) => ({ status, contentType: "application/json", body: JSON.stringify(value) });

// Decide whether a target is acceptable. Returns the normalized href or throws
// an Error whose message is safe to show the caller.
function vet(input) {
    const url = URL.parse(String(input ?? ""));
    if (!url) throw new Error("not a valid absolute URL");
    if (url.protocol !== "https:" && url.protocol !== "http:") throw new Error("only http and https links");
    if (url.username || url.password) throw new Error("links with credentials are refused");
    // A shortener that happily points at 10.0.0.1 or localhost becomes a way
    // to make other people's browsers probe internal systems.
    const host = url.hostname.replace(/^\[|\]$/g, "");
    if (host === "localhost" || (isValid(host) && (isPrivate(host) || isLoopback(host))))
        throw new Error("links into private networks are refused");
    return url.href;                                // normalized form: one spelling per target
}

const app = new App({ port: PORT });

app.post("/shorten", (req) => {
    let target;
    try { target = vet(JSON.parse(req.body).url); }
    catch (e) { return json(400, { error: e.message }); }

    // The same target always returns the same code.
    const existing = db.query("SELECT code FROM links WHERE target = ?", [target]);
    if (existing.length) return json(200, { code: existing[0].code, target, reused: true });

    // Collisions are astronomically unlikely at 7 characters, but the PRIMARY
    // KEY makes them impossible to miss: retry with a fresh code.
    for (let attempt = 0; attempt < 5; attempt++) {
        const code = newCode();
        try {
            db.exec("INSERT INTO links(code, target, created_at) VALUES (?, ?, ?)", [code, target, Date.now()]);
            return json(201, { code, target });
        } catch (e) { /* code collision: loop and mint another */ }
    }
    return json(500, { error: "could not allocate a code" });
});

app.get("/r/:code", (req) => {
    const changed = db.exec("UPDATE links SET hits = hits + 1 WHERE code = ?", [req.params.code]);
    if (!changed) return json(404, { error: "unknown code" });
    return json(200, { location: db.query("SELECT target FROM links WHERE code = ?", [req.params.code])[0].target });
});

app.get("/stats/:code", (req) => {
    const rows = db.query("SELECT code, target, hits FROM links WHERE code = ?", [req.params.code]);
    return rows.length ? json(200, rows[0]) : json(404, { error: "unknown code" });
});

app.start();
console.log("shortener on port", app.port);

// ---- self-test -------------------------------------------------------------
if (!PORT) {
    const base = `http://127.0.0.1:${app.port}`;
    const post = async (url) => {
        const res = await fetch(base + "/shorten", { method: "POST", body: JSON.stringify({ url }) });
        return { status: res.status, ...(await res.json()) };
    };
    const check = (cond, what) => { if (!cond) throw new Error("self-test failed: " + what); };

    const a = await post("HTTPS://Example.com/a/../docs?x=1");
    check(a.status === 201 && a.code.length === 7, "a link is shortened");
    check(a.target === "https://example.com/docs?x=1", "the target is stored normalized");
    const again = await post("https://example.com/docs?x=1");
    check(again.reused && again.code === a.code, "the same target reuses its code");

    for (const bad of ["javascript:alert(1)", "http://127.0.0.1/admin", "http://10.1.2.3/", "https://u:p@example.com/", "not a url"])
        check((await post(bad)).status === 400, "refused: " + bad);

    await fetch(`${base}/r/${a.code}`);
    const hit = await (await fetch(`${base}/r/${a.code}`)).json();
    check(hit.location === a.target, "resolution returns the target");
    const stats = await (await fetch(`${base}/stats/${a.code}`)).json();
    check(stats.hits === 2, "hits are counted");
    console.log("self-test passed:", a.code, "->", a.target);
    app.close();
    db.close();
}
